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

/* One field of the flat depth first table, as the C RantSchemaFieldInfo has it. An array's
 * element is the next row, named "". */
export interface FieldInfo {
    name: string; typeName: string; kind: FieldKind;
    /* an enum's backing kind, null for any other field */
    backing: FieldKind | null;
    count: number; depth: number; strCap: number;
    /* the flat index of the nearest enclosing array, -1 for none */
    arrParent: number;
    arrDepth: number; offset: number; size: number;
    /* a default value's bytes, what one more element at least costs */
    minLen: number;
}

interface Row {
    info: FieldInfo;
    kind: number; backing: number; count: number; strCap: number; size: number; minLen: number;
    end: number;                          /* one past the row's element and members */
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
                const kind = n.u8(p + F.kind), backing = n.u8(p + F.backing), parent = n.u16(p + F.arr_parent);
                const info: FieldInfo = {
                    name: n.strAt(p + F.name), typeName: n.strAt(p + F.type_name),
                    kind: KIND_NAMES[kind], backing: kind === K.ENUM ? KIND_NAMES[backing] : null,
                    count: n.u16(p + F.count), depth: n.u16(p + F.depth), strCap: n.u16(p + F.str_cap),
                    arrParent: parent === NO_PARENT ? -1 : parent, arrDepth: n.u16(p + F.arr_depth),
                    offset: n.u32(p + F.offset), size: n.u32(p + F.size), minLen: n.u32(p + F.min_len),
                };
                let variants: Map<number, string> | null = null;
                if (kind === K.ENUM) {
                    variants = new Map();
                    for (let v = 0, m = c.rant_schema_enum_count(s, i); v < m; v++)
                        if (c.rant_schema_enum_variant(s, i, v, vvalue, vname)) variants.set(Number(n.i64(vvalue)), n.strAt(vname));
                }
                this.rows.push({ info, kind, backing, count: info.count, strCap: info.strCap, size: info.size,
                                 minLen: info.minLen, end: i + 1, variants });
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

/* A row of fixed bytes. */
const fixed = (r: Row) => r.size !== 0 && r.minLen === r.size;
/* An array element that packs: a fixed scalar, enum or capped string. */
const packs = (el: Row) => fixed(el) && (el.kind <= K.BOOL || el.kind === K.ENUM || el.kind === K.STR);

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
        return this.t.bare ? this.row(0, [], names) : this.members(-1, [], names);
    }

    /* The members of the struct at row i, or of the root at -1, under your names for them. */
    private members(i: number, idx: number[], names: Names | null): Record<string, unknown> {
        const rows = this.t.rows, out: Record<string, unknown> = {};
        const depth = i < 0 ? 0 : rows[i].info.depth + 1;
        for (let j = i + 1; j < rows.length && rows[j].info.depth >= depth; j = rows[j].end) {
            const nm = names?.get(rows[j].info.name);
            out[nm?.mine ?? rows[j].info.name] = this.row(j, idx, nm?.sub ?? null);
        }
        return out;
    }

    /* Row i through rant_get_value_at, idx holding every enclosing array's index. An array of
     * fixed scalars is a typed array, of strings or enums their values, of anything else a
     * list of its elements. */
    private row(i: number, idx: number[], names: Names | null): unknown {
        const n = this.t.n, V = L.RantValue, v = this.val, rows = this.t.rows, r = rows[i];
        if (r.kind === K.STRUCT) return this.members(i, idx, names);
        idx.forEach((e, k) => n.setU32(this.idx + 4 * k, e));
        if (!n.c.rant_get_value_at(this.mb, this.t.s, i, this.idx, idx.length, v))
            throw new SchemaError(`the field '${r.info.name}' did not read`);
        const count = n.u32(v + V.count), data = n.u32(v + V.bytes), len = n.u32(v + V.bytes + 4);
        switch (r.kind) {
        case K.STR: case K.VSTR: return n.str(data, len);
        case K.MAP: return decodeMap(n.copy(data, len));
        case K.ARR: case K.VARR: {
            const el = rows[i + 1];
            if (!packs(el)) {
                const out: unknown[] = [];
                for (let e = 0; e < count; e++) out.push(this.row(i + 1, [...idx, e], names));
                return out;
            }
            const raw = n.copy(data, len);
            if (el.kind === K.STR) return stringSlots(raw, el.strCap);
            if (el.kind !== K.ENUM) return scalarArray(el.kind, raw);
            return Array.from(scalarArray(el.backing, raw) as ArrayLike<number | bigint>,
                              (x) => el.variants?.get(Number(x)) ?? Number(x));
        }
        case K.F32: case K.F64: return n.f64(v + V.v_f);
        case K.ENUM: {
            const num = Number(n.i64(v + V.v_i));
            return r.variants?.get(num) ?? num;
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

/* One rant_set_value_at: a scalar, the bytes of a string, map or packed array, or the count a
 * variable array is sized to. */
interface Op { i: number; idx: number[]; val?: unknown; bytes?: Uint8Array | null; count?: number }

/* The message for value, or SchemaError when it does not fit. A missing fixed field encodes as
 * zero and a missing variable one as empty. */
export function encode(t: Table, value: unknown, names: Names | null): HeapBytes {
    const n = t.n, ops = new Ops(t);
    if (t.bare) ops.row(0, value, [], names);
    else ops.members(-1, asObject(value, "the value"), [], names);
    const size = n.c.rant_schema_msg_min(t.s) + ops.grow;
    const buf = n.alloc(size);
    try {
        n.c.rant_schema_message_default(t.s, buf, size);
        scoped(n, (sc) => {
            const w = new Writer(t, sc, buf, size);
            for (const op of ops.list) w.set(op);
        });
        return { ptr: buf, len: n.c.rant_schema_msg_len(t.s, buf, size) };
    } catch (e) {
        n.free(buf);
        throw e;
    }
}

/* The setter calls a value makes, in table order, and the bytes they add to the default
 * message at most. */
class Ops {
    readonly list: Op[] = [];
    grow = 0;
    constructor(private t: Table) {}

    /* The members of the struct at row i, or of the root at -1, valued from src. */
    members(i: number, src: Record<string, unknown>, idx: number[], names: Names | null): void {
        const rows = this.t.rows;
        const depth = i < 0 ? 0 : rows[i].info.depth + 1;
        for (let j = i + 1; j < rows.length && rows[j].info.depth >= depth; j = rows[j].end) {
            const nm = names?.get(rows[j].info.name);
            this.row(j, pick(src, rows[j].info.name, nm?.mine), idx, nm?.sub ?? null);
        }
    }

    /* Row i valued val under idx. A missing value keeps the default. */
    row(i: number, val: unknown, idx: number[], names: Names | null): void {
        if (val == null) return;
        const r = this.t.rows[i];
        if (r.kind === K.STRUCT) return this.members(i, asObject(val, r.info.name), idx, names);
        if (r.kind === K.ARR || r.kind === K.VARR) return this.array(i, val, idx, names);
        const bytes = payload(this.t, i, val);
        if (bytes && r.kind !== K.STR) this.grow += bytes.length;
        this.list.push({ i, idx, val, bytes });
    }

    /* An array of fixed scalars, enums or capped strings is one packed value. Any other
     * element is set on its own through the element row, once a variable array is sized. */
    private array(i: number, val: unknown, idx: number[], names: Names | null): void {
        const r = this.t.rows[i], el = this.t.rows[i + 1];
        if (packs(el)) {
            let bytes = packElements(this.t, i + 1, val, r.info.name);
            if (r.kind === K.VARR) this.grow += bytes.length;
            else bytes = bytes.subarray(0, r.count * el.size);   /* a fixed array keeps its first count */
            this.list.push({ i, idx, bytes });
            return;
        }
        if (!Array.isArray(val)) throw new SchemaError(`${r.info.name || "the value"} is an array and takes an array`);
        const count = r.kind === K.VARR ? val.length : Math.min(val.length, r.count);
        if (r.kind === K.VARR) {
            this.list.push({ i, idx, count });
            this.grow += count * (fixed(el) ? el.size : 4 + el.minLen);   /* an end and a default body each */
        }
        for (let e = 0; e < count; e++) this.row(i + 1, val[e], [...idx, e], names);
    }
}

class Writer {
    private val: number;
    private idx: number;
    constructor(private t: Table, private sc: Scope, private buf: number, private size: number) {
        this.val = sc.alloc(L.RantValue.sizeof);
        this.idx = sc.alloc(4 * MAX_DEPTH);
    }

    /* One op through rant_set_value_at, or rant_set_array_count_at for a count. */
    set(op: Op): void {
        const n = this.t.n, V = L.RantValue, v = this.val, r = this.t.rows[op.i];
        op.idx.forEach((e, k) => n.setU32(this.idx + 4 * k, e));
        if (op.count !== undefined) {
            if (!n.c.rant_set_array_count_at(this.buf, this.size, this.t.s, op.i, this.idx, op.idx.length, op.count))
                throw new SchemaError(`'${r.info.name || "the value"}' does not fit ${op.count} elements`);
            return;
        }
        n.heap.fill(0, v, v + V.sizeof);
        n.setU8(v + V.kind, r.kind);
        if (op.bytes) {
            n.setU32(v + V.bytes, op.bytes.length ? this.sc.data(op.bytes) : 0);
            n.setU32(v + V.bytes + 4, op.bytes.length);
        } else {
            switch (r.kind) {
            case K.F32: case K.F64: n.setF64(v + V.v_f, num(op.val, r.info.name)); break;
            case K.BOOL: n.setU64(v + V.v_u, op.val ? 1n : 0n); break;
            case K.ENUM: n.setI64(v + V.v_i, BigInt(enumNumber(this.t, op.i, op.val))); break;
            case K.I8: case K.I16: case K.I32: case K.I64: n.setI64(v + V.v_i, int(op.val, r.info.name)); break;
            default: n.setU64(v + V.v_u, BigInt.asUintN(64, int(op.val, r.info.name)));
            }
        }
        if (!n.c.rant_set_value_at(this.buf, this.size, this.t.s, op.i, this.idx, op.idx.length, v))
            throw new SchemaError(`the value does not fit '${r.info.name || "the value"}'`);
    }
}

/* The bytes a string or map field carries, null for a scalar. */
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
    default: return null;
    }
}

/* The packed elements of an array whose element row e is a fixed scalar, enum or capped string. */
function packElements(t: Table, e: number, val: unknown, name: string): Uint8Array {
    const el = t.rows[e];
    if (el.kind === K.STR) {
        if (!Array.isArray(val)) throw new SchemaError(`'${name}' takes an array of strings`);
        const slot = 2 + el.strCap, out = new Uint8Array(val.length * slot);
        val.forEach((s, k) => {
            const b = encodeUtf8(String(s));
            if (b.length > el.strCap) throw new SchemaError(`'${name}[${k}]' takes at most ${el.strCap} bytes`);
            out[k * slot] = b.length & 255;
            out[k * slot + 1] = b.length >> 8;
            out.set(b, k * slot + 2);
        });
        return out;
    }
    if (el.kind !== K.ENUM) return packScalars(el.kind, val, name);
    if (!Array.isArray(val)) throw new SchemaError(`'${name}' takes an array`);
    return packScalars(el.backing, val.map((x) => enumNumber(t, e, x)), name);
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
