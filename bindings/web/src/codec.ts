/* Values to messages and back. Both walk a compiled schema's flat field table and read or
 * write each field through the C accessors by index, so the wire layout stays the C's alone
 * and a rebased schema decodes like any other. Only the map body is built and parsed here. */

import * as L from "./layout.js";
import type { Shape } from "./schema.js";
import { decodeUtf8, encodeUtf8, scoped, type Native, type Scope } from "./wasm.js";

/* The kind byte of src/serialize/schema.h. */
const K = { U8: 0, U16: 1, U32: 2, U64: 3, I8: 4, I16: 5, I32: 6, I64: 7, F32: 8, F64: 9, BOOL: 10,
            ARR: 11, STRUCT: 12, STR: 13, VSTR: 14, VARR: 15, MAP: 16, ENUM: 17, NAMED: 18 } as const;
const KIND_NAMES = ["u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "f32", "f64", "bool",
                    "arr", "struct", "str", "vstr", "varr", "map", "enum", "named"] as const;
export type FieldKind = typeof KIND_NAMES[number];
const NO_PARENT = 0xffff;
const SIZE = [1, 2, 4, 8, 1, 2, 4, 8, 4, 8, 1];
const MAX_DEPTH = 8;     /* RANT_SCHEMA_MAX_DEPTH, which bounds a map's nesting too */

/* One field of the flat depth first table, as the C RantSchemaFieldInfo has it. */
export interface FieldInfo {
    name: string; typeName: string; elemName: string; kind: FieldKind;
    /* the array element kind or the enum backing, null for any other field */
    elem: FieldKind | null;
    count: number; depth: number; strCap: number;
    /* the flat index of the nearest enclosing struct array, -1 for none */
    arrParent: number;
    arrDepth: number; offset: number; size: number; elemSize: number;
}

interface Row {
    info: FieldInfo;
    kind: number; elem: number; count: number; strCap: number; elemSize: number;
    end: number;                          /* one past the members of a struct or struct array */
    structArray: boolean;
    variants: Map<number, string> | null; /* an enum's option names */
}

/* A compiled schema's table, read once per schema pointer. */
export class Table {
    readonly rows: Row[] = [];
    /* the root is a bare type: the value is the one anonymous field */
    readonly bare: boolean;

    constructor(readonly n: Native, readonly s: number) {
        const c = n.c;
        const count = c.rant_schema_field_count(s);
        scoped(n, (sc) => {
            const p = sc.alloc(L.RantSchemaFieldInfo.sizeof);
            const vname = sc.alloc(L.RantString.sizeof), vvalue = sc.alloc(8);
            for (let i = 0; i < count; i++) {
                c.rant_schema_field_at(s, i, p);
                const F = L.RantSchemaFieldInfo;
                const kind = n.u8(p + F.kind), elem = n.u8(p + F.elem), parent = n.u16(p + F.arr_parent);
                const info: FieldInfo = {
                    name: n.strAt(p + F.name), typeName: n.strAt(p + F.type_name), elemName: n.strAt(p + F.elem_name),
                    kind: KIND_NAMES[kind], elem: kind === K.ARR || kind === K.VARR || kind === K.ENUM ? KIND_NAMES[elem] : null,
                    count: n.u16(p + F.count), depth: n.u16(p + F.depth), strCap: n.u16(p + F.str_cap),
                    arrParent: parent === NO_PARENT ? -1 : parent, arrDepth: n.u16(p + F.arr_depth),
                    offset: n.u32(p + F.offset), size: n.u32(p + F.size), elemSize: n.u32(p + F.elem_size),
                };
                let variants: Map<number, string> | null = null;
                if (kind === K.ENUM) {
                    variants = new Map();
                    for (let v = 0, m = c.rant_schema_enum_count(s, i); v < m; v++)
                        if (c.rant_schema_enum_variant(s, i, v, vvalue, vname)) variants.set(Number(n.i64(vvalue)), n.strAt(vname));
                }
                this.rows.push({ info, kind, elem, count: info.count, strCap: info.strCap, elemSize: info.elemSize, end: i + 1,
                                 structArray: (kind === K.ARR || kind === K.VARR) && elem === K.STRUCT, variants });
            }
        });
        for (let i = 0; i < this.rows.length; i++) {
            let j = i + 1;
            while (j < this.rows.length && this.rows[j].info.depth > this.rows[i].info.depth) j++;
            this.rows[i].end = j;
        }
        this.bare = this.rows.length > 0 && this.rows[0].info.name === "";
    }

    get fields(): FieldInfo[] { return this.rows.map((r) => ({ ...r.info })); }
}

/* Your member names for the wire's, per struct level: a member called partNumber travels as
 * part_number. null where the values carry the wire's names. */
export type Names = Map<string, { mine: string; sub: Names | null }>;

const namesCache = new WeakMap<Shape, Names | null>();

export function namesOf(n: Native, shape: Shape | null): Names | null {
    if (!shape) return null;
    let sh = shape;
    while (sh.k === "array") sh = sh.elem;
    if (sh.k !== "struct") return null;
    if (namesCache.has(sh)) return namesCache.get(sh)!;
    const out: Names = new Map();
    for (const [mine, sub] of Object.entries(sh.fields)) out.set(wireName(n, mine), { mine, sub: namesOf(n, sub) });
    namesCache.set(sh, out);
    return out;
}

function wireName(n: Native, name: string): string {
    return scoped(n, (sc) => {
        const out = sc.alloc(256);
        const len = n.c.rant_field_name(sc.cstr(name), out, 256);
        return len ? n.str(out, len) : name;
    });
}

export class SchemaError extends Error {
    constructor(message: string) { super(message); this.name = "SchemaError"; }
}

/* ---- decode ---------------------------------------------------------------------------- */

/* The value of the message at ptr, len in the C heap. */
export function decode(t: Table, ptr: number, len: number, names: Names | null): unknown {
    return scoped(t.n, (sc) => new Reader(t, sc, sc.view(ptr, len)).root(names));
}

class Reader {
    private val: number;
    private idx: number;
    constructor(private t: Table, sc: Scope, private mb: number) {
        this.val = sc.alloc(L.RantValue.sizeof);
        this.idx = sc.alloc(4 * MAX_DEPTH);
    }

    root(names: Names | null): unknown {
        const rows = this.t.rows;
        if (this.t.bare) return rows[0].structArray ? this.array(0, [], names) : this.value(0, []);
        const root: Record<string, unknown> = {};
        const dests: { obj: Record<string, unknown>; names: Names | null }[] = [{ obj: root, names }];
        for (let i = 0; i < rows.length; i++) {
            const r = rows[i];
            if (r.info.arrParent >= 0) continue;          /* an element template member: its array reads it */
            const d = dests[r.info.depth];
            const nm = d.names?.get(r.info.name);
            const key = nm?.mine ?? r.info.name;
            if (r.structArray) d.obj[key] = this.array(i, [], nm?.sub ?? null);
            else if (r.kind === K.STRUCT) {
                const child: Record<string, unknown> = {};
                d.obj[key] = child;
                dests[r.info.depth + 1] = { obj: child, names: nm?.sub ?? null };
            }
            else d.obj[key] = this.value(i, []);
        }
        return root;
    }

    private array(i: number, outer: number[], names: Names | null): Record<string, unknown>[] {
        const n = this.t.n;
        const count = n.c.rant_array_count_at(this.mb, this.t.s, i);
        const out: Record<string, unknown>[] = [];
        for (let e = 0; e < count; e++) out.push(this.element(i, [...outer, e], names));
        return out;
    }

    private element(i: number, idx: number[], names: Names | null): Record<string, unknown> {
        const rows = this.t.rows;
        const top: Record<string, unknown> = {};
        const dests: Record<number, { obj: Record<string, unknown>; names: Names | null }> = { [rows[i].info.depth + 1]: { obj: top, names } };
        for (let j = i + 1; j < rows[i].end;) {
            const m = rows[j];
            const d = dests[m.info.depth];
            const nm = d.names?.get(m.info.name);
            const key = nm?.mine ?? m.info.name;
            if (m.structArray) {
                d.obj[key] = this.array(j, idx, nm?.sub ?? null);
                j = m.end;
                continue;
            }
            if (m.kind === K.STRUCT) {
                const child: Record<string, unknown> = {};
                d.obj[key] = child;
                dests[m.info.depth + 1] = { obj: child, names: nm?.sub ?? null };
            }
            else d.obj[key] = this.value(j, idx);
            j++;
        }
        return top;
    }

    /* One field through rant_get_value_at, idx holding every enclosing array's index. */
    private value(i: number, idx: number[]): unknown {
        const n = this.t.n, V = L.RantValue, v = this.val;
        idx.forEach((e, k) => n.setU32(this.idx + 4 * k, e));
        if (!n.c.rant_get_value_at(this.mb, this.t.s, i, this.idx, idx.length, v))
            throw new SchemaError(`the field '${this.t.rows[i].info.name}' did not read`);
        const kind = n.u8(v + V.kind), elem = n.u8(v + V.elem);
        const data = n.u32(v + V.bytes), len = n.u32(v + V.bytes + 4);
        switch (kind) {
        case K.STR: case K.VSTR: return n.str(data, len);
        case K.MAP: return decodeMap(n.copy(data, len));
        case K.ARR: case K.VARR: {
            const raw = n.copy(data, len);
            if (elem === K.STR) return stringSlots(raw, n.u16(v + V.str_cap));
            return scalarArray(elem, raw);
        }
        case K.F32: case K.F64: return n.f64(v + V.v_f);
        case K.ENUM: {
            const num = Number(n.i64(v + V.v_i));
            return this.t.rows[i].variants?.get(num) ?? num;
        }
        case K.I64: return n.i64(v + V.v_i);
        case K.U64: return n.u64(v + V.v_u);
        case K.I8: case K.I16: case K.I32: return Number(n.i64(v + V.v_i));
        case K.BOOL: return n.u64(v + V.v_u) !== 0n;
        default: return Number(n.u64(v + V.v_u));
        }
    }
}

/* [u16 len][cap bytes] per string. A hostile length is clamped as the C reader does. */
function stringSlots(raw: Uint8Array, cap: number): string[] {
    const out: string[] = [];
    for (let off = 0; off + 2 <= raw.length; off += 2 + cap) {
        const len = Math.min(raw[off] | (raw[off + 1] << 8), cap, raw.length - off - 2);
        out.push(decodeUtf8(raw.subarray(off + 2, off + 2 + len)));
    }
    return out;
}

function scalarArray(elem: number, raw: Uint8Array): unknown {
    const b = raw.buffer as ArrayBuffer, n = raw.length;
    switch (elem) {
    case K.U8: return raw;
    case K.I8: return new Int8Array(b, 0, n);
    case K.U16: return new Uint16Array(b, 0, n >> 1);
    case K.I16: return new Int16Array(b, 0, n >> 1);
    case K.U32: return new Uint32Array(b, 0, n >> 2);
    case K.I32: return new Int32Array(b, 0, n >> 2);
    case K.U64: return new BigUint64Array(b, 0, n >> 3);
    case K.I64: return new BigInt64Array(b, 0, n >> 3);
    case K.F32: return new Float32Array(b, 0, n >> 2);
    case K.F64: return new Float64Array(b, 0, n >> 3);
    case K.BOOL: return Array.from(raw, (x) => x !== 0);
    default: throw new SchemaError(`an array of kind ${elem} does not read`);
    }
}

/* ---- encode ---------------------------------------------------------------------------- */

/* A message in the C heap, which the caller frees. */
export interface HeapBytes { ptr: number; len: number }

/* The message for value, or SchemaError when it does not fit. A missing fixed field encodes as
 * zero and a missing variable one as empty. */
export function encode(t: Table, value: unknown, names: Names | null): HeapBytes {
    const n = t.n, rows = t.rows;
    const src: Record<string, unknown> = t.bare ? { "": value } : asObject(value, "the value");
    const ops: { i: number; val: unknown; bytes?: Uint8Array | null; elems?: unknown[]; names?: Names | null }[] = [];
    let tail = 0;
    const srcs: { obj: Record<string, unknown> | null; names: Names | null }[] = [{ obj: src, names: t.bare ? null : names }];
    for (let i = 0; i < rows.length; i++) {
        const r = rows[i];
        if (r.info.arrParent >= 0) continue;              /* an element template member: its array writes it */
        const d = srcs[r.info.depth];
        const nm = d.names?.get(r.info.name);
        const val = d.obj ? pick(d.obj, r.info.name, nm?.mine) : undefined;
        if (r.kind === K.STRUCT) {
            srcs[r.info.depth + 1] = { obj: val == null ? null : asObject(val, r.info.name), names: nm?.sub ?? null };
            continue;
        }
        if (val == null) continue;                        /* keeps the zeroed default */
        const sub = t.bare ? names : nm?.sub ?? null;
        if (r.structArray) {
            if (!Array.isArray(val)) throw new SchemaError(`${r.info.name || "the value"} is an array of structs and takes an array`);
            const count = r.kind === K.VARR ? val.length : Math.min(val.length, r.count);
            if (r.kind === K.VARR) tail += count * r.elemSize;
            ops.push({ i, val, elems: val.slice(0, count), names: sub });
            continue;
        }
        const bytes = payload(t, i, val);
        if (bytes && (r.kind === K.VSTR || r.kind === K.VARR || r.kind === K.MAP)) tail += bytes.length;
        ops.push({ i, val, bytes });
    }

    const size = n.c.rant_schema_msg_min(t.s) + tail;
    const buf = n.alloc(size);
    try {
        n.c.rant_schema_message_default(t.s, buf, size);
        scoped(n, (sc) => {
            const w = new Writer(t, sc, buf, size);
            for (const op of ops) {
                if (op.elems) w.elements(op.i, op.elems, [], op.names ?? null);
                else w.set(op.i, [], op.val, op.bytes);
            }
        });
        return { ptr: buf, len: n.c.rant_schema_msg_len(t.s, buf, size) };
    } catch (e) {
        n.free(buf);
        throw e;
    }
}

class Writer {
    private val: number;
    private idx: number;
    constructor(private t: Table, private sc: Scope, private buf: number, private size: number) {
        this.val = sc.alloc(L.RantValue.sizeof);
        this.idx = sc.alloc(4 * MAX_DEPTH);
    }

    /* Every member of every element of the struct array at i, a variable one sized first. */
    elements(i: number, items: unknown[], outer: number[], names: Names | null): void {
        const rows = this.t.rows, r = rows[i];
        if (r.kind === K.VARR) this.set(i, outer, null, new Uint8Array(items.length * r.elemSize));
        items.forEach((item, e) => {
            const idx = [...outer, e];
            const srcs: Record<number, { obj: Record<string, unknown> | null; names: Names | null }> =
                { [r.info.depth + 1]: { obj: asObject(item, `${r.info.name}[${e}]`), names } };
            for (let j = i + 1; j < r.end;) {
                const m = rows[j];
                const d = srcs[m.info.depth];
                const nm = d.names?.get(m.info.name);
                const val = d.obj ? pick(d.obj, m.info.name, nm?.mine) : undefined;
                if (m.structArray) {
                    if (val != null) {
                        if (!Array.isArray(val)) throw new SchemaError(`${m.info.name} is an array of structs and takes an array`);
                        this.elements(j, m.kind === K.VARR ? val : val.slice(0, m.count), idx, nm?.sub ?? null);
                    }
                    j = m.end;
                    continue;
                }
                if (m.kind === K.STRUCT) srcs[m.info.depth + 1] = { obj: val == null ? null : asObject(val, m.info.name), names: nm?.sub ?? null };
                else if (val != null) this.set(j, idx, val, payload(this.t, j, val));
                j++;
            }
        });
    }

    /* One field through rant_set_value_at, bytes carrying a string, array or map. */
    set(i: number, idx: number[], val: unknown, bytes?: Uint8Array | null): void {
        const n = this.t.n, V = L.RantValue, v = this.val, r = this.t.rows[i];
        n.heap.fill(0, v, v + V.sizeof);
        n.setU8(v + V.kind, r.kind);
        n.setU8(v + V.elem, r.elem);
        n.setU16(v + V.count, r.count);
        n.setU16(v + V.str_cap, r.strCap);
        if (bytes) {
            n.setU32(v + V.bytes, bytes.length ? this.sc.data(bytes) : 0);
            n.setU32(v + V.bytes + 4, bytes.length);
        } else {
            switch (r.kind) {
            case K.F32: case K.F64: n.setF64(v + V.v_f, num(val, r.info.name)); break;
            case K.BOOL: n.setU64(v + V.v_u, val ? 1n : 0n); break;
            case K.ENUM: n.setI64(v + V.v_i, BigInt(enumNumber(this.t, i, val))); break;
            case K.I8: case K.I16: case K.I32: case K.I64: n.setI64(v + V.v_i, int(val, r.info.name)); break;
            default: n.setU64(v + V.v_u, BigInt.asUintN(64, int(val, r.info.name)));
            }
        }
        idx.forEach((e, k) => n.setU32(this.idx + 4 * k, e));
        if (!n.c.rant_set_value_at(this.buf, this.size, this.t.s, i, this.idx, idx.length, v))
            throw new SchemaError(`the value does not fit '${r.info.name || "the value"}'`);
    }
}

/* The bytes a string, array or map field carries, null for a scalar. */
function payload(t: Table, i: number, val: unknown): Uint8Array | null {
    const r = t.rows[i];
    switch (r.kind) {
    case K.STR: case K.VSTR: {
        if (typeof val !== "string") throw new SchemaError(`'${r.info.name}' takes a string`);
        const b = encodeUtf8(val);
        if (r.kind === K.STR && b.length > r.strCap) throw new SchemaError(`'${r.info.name}' takes at most ${r.strCap} bytes`);
        return b;
    }
    case K.MAP: return encodeMap(asObject(val, r.info.name));
    case K.ARR: case K.VARR: {
        if (r.elem === K.STR) {
            if (!Array.isArray(val)) throw new SchemaError(`'${r.info.name}' takes an array of strings`);
            const items = r.kind === K.ARR ? val.slice(0, r.count) : val;
            const slot = 2 + r.strCap, out = new Uint8Array(items.length * slot);
            items.forEach((s, k) => {
                const b = encodeUtf8(String(s));
                if (b.length > r.strCap) throw new SchemaError(`'${r.info.name}[${k}]' takes at most ${r.strCap} bytes`);
                out[k * slot] = b.length & 255;
                out[k * slot + 1] = b.length >> 8;
                out.set(b, k * slot + 2);
            });
            return out;
        }
        return packScalars(r.elem, val, r.info.name);
    }
    default: return null;
    }
}

/* The view type whose bytes already are the wire kind, so one copy suffices. */
const WIRE_VIEW: Partial<Record<number, Function[]>> = {
    [K.U8]: [Uint8Array, Uint8ClampedArray], [K.I8]: [Int8Array], [K.U16]: [Uint16Array], [K.I16]: [Int16Array],
    [K.U32]: [Uint32Array], [K.I32]: [Int32Array], [K.U64]: [BigUint64Array], [K.I64]: [BigInt64Array],
    [K.F32]: [Float32Array], [K.F64]: [Float64Array],
};

function packScalars(elem: number, val: unknown, name: string): Uint8Array {
    if (ArrayBuffer.isView(val) && !(val instanceof DataView)) {
        if (WIRE_VIEW[elem]?.includes(val.constructor))
            return new Uint8Array(val.buffer.slice(val.byteOffset, val.byteOffset + val.byteLength));
        val = Array.from(val as unknown as ArrayLike<unknown>);   /* another element type converts */
    }
    if (!Array.isArray(val)) throw new SchemaError(`'${name}' takes an array`);
    const size = SIZE[elem], out = new DataView(new ArrayBuffer(val.length * size));
    val.forEach((x, k) => writeScalar(out, k * size, elem, x, name));
    return new Uint8Array(out.buffer);
}

function writeScalar(d: DataView, o: number, kind: number, x: unknown, name: string): void {
    switch (kind) {
    case K.U8: case K.BOOL: d.setUint8(o, kind === K.BOOL ? (x ? 1 : 0) : Number(x)); break;
    case K.I8: d.setInt8(o, Number(x)); break;
    case K.U16: d.setUint16(o, Number(x), true); break;
    case K.I16: d.setInt16(o, Number(x), true); break;
    case K.U32: d.setUint32(o, Number(x), true); break;
    case K.I32: d.setInt32(o, Number(x), true); break;
    case K.U64: d.setBigUint64(o, BigInt.asUintN(64, int(x, name)), true); break;
    case K.I64: d.setBigInt64(o, int(x, name), true); break;
    case K.F32: d.setFloat32(o, num(x, name), true); break;
    case K.F64: d.setFloat64(o, num(x, name), true); break;
    default: throw new SchemaError(`'${name}' cannot hold kind ${kind}`);
    }
}

function enumNumber(t: Table, i: number, val: unknown): number {
    if (typeof val !== "string") return Number(int(val, t.rows[i].info.name));
    for (const [v, name] of t.rows[i].variants ?? []) if (name === val) return v;
    /* any other spelling of an option, as the C matches it */
    return scoped(t.n, (sc) => {
        const out = sc.alloc(8);
        if (!t.n.c.rant_enum_value_of(t.s, i, sc.cstr(val), out))
            throw new SchemaError(`'${val}' is not an option of '${t.rows[i].info.name}'`);
        return Number(t.n.i64(out));
    });
}

/* A value by its wire name, or by your member name for it. */
function pick(obj: Record<string, unknown>, wire: string, mine?: string): unknown {
    if (mine !== undefined && obj[mine] !== undefined) return obj[mine];
    return obj[wire];
}

function asObject(v: unknown, what: string): Record<string, unknown> {
    if (v === null || typeof v !== "object" || Array.isArray(v) || ArrayBuffer.isView(v))
        throw new SchemaError(`${what || "the value"} takes an object`);
    return v as Record<string, unknown>;
}

function num(v: unknown, name: string): number {
    if (typeof v === "number") return v;
    if (typeof v === "bigint") return Number(v);
    throw new SchemaError(`'${name}' takes a number`);
}

function int(v: unknown, name: string): bigint {
    if (typeof v === "bigint") return v;
    if (typeof v === "number" && Number.isInteger(v)) return BigInt(v);
    if (typeof v === "boolean") return v ? 1n : 0n;
    throw new SchemaError(`'${name}' takes an integer`);
}

/* ---- the map body ---------------------------------------------------------------------- */

/* [u16 n]([u8 klen][key][u8 kind]payload)*, spec/schema.md. Integers take the smallest kind
 * that holds them, and a key whose value is null or undefined is left out. */
export function encodeMap(obj: Record<string, unknown>, depth = 1): Uint8Array {
    if (depth > MAX_DEPTH) throw new SchemaError("a map nests too deep");
    const parts: Uint8Array[] = [];
    let count = 0;
    for (const [key, val] of Object.entries(obj)) {
        if (val == null) continue;
        const k = encodeUtf8(key);
        if (k.length > 255) throw new SchemaError(`the map key '${key}' is longer than 255 bytes`);
        parts.push(Uint8Array.of(k.length), k, mapValue(val, depth));
        count++;
    }
    if (count > 0xffff) throw new SchemaError("a map holds at most 65535 keys");
    return concat([Uint8Array.of(count & 255, count >> 8), ...parts]);
}

function mapValue(v: unknown, depth: number): Uint8Array {
    if (typeof v === "boolean") return Uint8Array.of(K.BOOL, v ? 1 : 0);
    if (typeof v === "bigint" || (typeof v === "number" && Number.isInteger(v))) return mapInt(BigInt(v));
    if (typeof v === "number") {
        const d = new DataView(new ArrayBuffer(9));
        d.setUint8(0, K.F64);
        d.setFloat64(1, v, true);
        return new Uint8Array(d.buffer);
    }
    if (typeof v === "string") {
        const b = encodeUtf8(v);
        if (b.length > 0xffff) throw new SchemaError("a map string holds at most 65535 bytes");
        return concat([Uint8Array.of(K.VSTR, b.length & 255, b.length >> 8), b]);
    }
    if (Array.isArray(v)) {
        if (v.length > 0xffff) throw new SchemaError("a map array holds at most 65535 values");
        return concat([Uint8Array.of(K.VARR, v.length & 255, v.length >> 8), ...v.map((x) => mapValue(x, depth))]);
    }
    if (typeof v === "object" && v !== null) return concat([Uint8Array.of(K.MAP), encodeMap(v as Record<string, unknown>, depth + 1)]);
    throw new SchemaError(`a map cannot hold a ${typeof v}`);
}

function mapInt(v: bigint): Uint8Array {
    const pick: [number, number, (d: DataView) => void][] = v < 0n
        ? [[K.I8, 1, (d) => d.setInt8(1, Number(v))], [K.I16, 2, (d) => d.setInt16(1, Number(v), true)],
           [K.I32, 4, (d) => d.setInt32(1, Number(v), true)], [K.I64, 8, (d) => d.setBigInt64(1, v, true)]]
        : [[K.U8, 1, (d) => d.setUint8(1, Number(v))], [K.U16, 2, (d) => d.setUint16(1, Number(v), true)],
           [K.U32, 4, (d) => d.setUint32(1, Number(v), true)], [K.U64, 8, (d) => d.setBigUint64(1, v, true)]];
    const limits = v < 0n ? [-0x80n, -0x8000n, -0x80000000n] : [0xffn, 0xffffn, 0xffffffffn];
    let k = 0;
    while (k < 3 && (v < 0n ? v < limits[k] : v > limits[k])) k++;
    const [kind, size, write] = pick[k];
    const d = new DataView(new ArrayBuffer(1 + size));
    d.setUint8(0, kind);
    write(d);
    return new Uint8Array(d.buffer);
}

/* A map body back to an object. Bounds checked: a short or hostile body stops early. */
export function decodeMap(b: Uint8Array): Record<string, unknown> {
    const d = new DataView(b.buffer, b.byteOffset, b.byteLength);
    return mapBody(b, d, { at: 0 }, 1);
}

function mapBody(b: Uint8Array, d: DataView, p: { at: number }, depth: number): Record<string, unknown> {
    const out: Record<string, unknown> = {};
    if (depth > MAX_DEPTH || p.at + 2 > b.length) { p.at = b.length; return out; }
    const count = d.getUint16(p.at, true);
    p.at += 2;
    for (let i = 0; i < count && p.at < b.length; i++) {
        const klen = b[p.at++];
        if (p.at + klen > b.length) break;
        const key = decodeUtf8(b.subarray(p.at, p.at + klen));
        p.at += klen;
        out[key] = mapItem(b, d, p, depth);
    }
    return out;
}

function mapItem(b: Uint8Array, d: DataView, p: { at: number }, depth: number): unknown {
    if (p.at >= b.length) return null;
    const kind = b[p.at++];
    if (kind <= K.BOOL) {
        const size = SIZE[kind];
        if (p.at + size > b.length) { p.at = b.length; return null; }
        const o = p.at;
        p.at += size;
        switch (kind) {
        case K.U8: return d.getUint8(o);
        case K.U16: return d.getUint16(o, true);
        case K.U32: return d.getUint32(o, true);
        case K.U64: return d.getBigUint64(o, true);
        case K.I8: return d.getInt8(o);
        case K.I16: return d.getInt16(o, true);
        case K.I32: return d.getInt32(o, true);
        case K.I64: return d.getBigInt64(o, true);
        case K.F32: return d.getFloat32(o, true);
        case K.F64: return d.getFloat64(o, true);
        default: return d.getUint8(o) !== 0;
        }
    }
    if (kind === K.VSTR) {
        if (p.at + 2 > b.length) { p.at = b.length; return ""; }
        const len = Math.min(d.getUint16(p.at, true), b.length - p.at - 2);
        const s = decodeUtf8(b.subarray(p.at + 2, p.at + 2 + len));
        p.at += 2 + len;
        return s;
    }
    if (kind === K.VARR) {
        if (p.at + 2 > b.length) { p.at = b.length; return []; }
        const count = d.getUint16(p.at, true);
        p.at += 2;
        const out: unknown[] = [];
        for (let i = 0; i < count && p.at < b.length; i++) out.push(mapItem(b, d, p, depth));
        return out;
    }
    if (kind === K.MAP) return mapBody(b, d, p, depth + 1);
    p.at = b.length;
    return null;
}

function concat(parts: Uint8Array[]): Uint8Array {
    const out = new Uint8Array(parts.reduce((s, x) => s + x.length, 0));
    let at = 0;
    for (const x of parts) { out.set(x, at); at += x.length; }
    return out;
}
