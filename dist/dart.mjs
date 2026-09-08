/* The bridge client: one DartNode is one full node on the mesh, spoken through the bridge
 * over a WebSocket with a WebRTC data path. docs/javascript.md explains how to use it. */
/* Data-plane frame: ONE header for every op, both directions, little-endian:
 *   [u8 op][u8 flags][u16 id][u32 seq][u32 peer][u64 written_us][u8 text_len][text][payload] */
const OP_DATA = 1; /* topic message (publish / delivery) */
const OP_VAR = 2; /* variable write / update */
const OP_CALL = 3; /* call a remote / a request for a definition */
const OP_RESULT = 4; /* call outcome / request reply */
const OP_PROGRESS = 5; /* task progress, either direction */
const OP_CANCEL = 6; /* server->client: cancel a parked request */
const HDR = 21;
const LOSSY_BUFFER = 1 << 20; /* a best-effort frame drops past this much unsent on its carrier */
const enc = new TextEncoder();
const dec = new TextDecoder();
function buildFrame(op, flags, id, seq, text, payload) {
    let tb = text ? enc.encode(text) : new Uint8Array(0);
    if (tb.length > 255)
        tb = tb.subarray(0, 255); /* one length byte, like the wire */
    const f = new Uint8Array(HDR + tb.length + payload.length);
    const v = new DataView(f.buffer);
    f[0] = op;
    f[1] = flags;
    v.setUint16(2, id, true);
    v.setUint32(4, seq, true); /* peer and written_us are 0 from a client */
    f[20] = tb.length;
    f.set(tb, HDR);
    f.set(payload, HDR + tb.length);
    return f;
}
function parseFrame(b) {
    if (b.length < HDR)
        return null;
    const tl = b[20];
    if (b.length < HDR + tl)
        return null;
    const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
    return { op: b[0], flags: b[1], id: v.getUint16(2, true), seq: v.getUint32(4, true),
        peer: v.getUint32(8, true), writtenUs: Number(v.getBigUint64(12, true)),
        text: tl ? dec.decode(b.subarray(HDR, HDR + tl)) : "", payload: b.subarray(HDR + tl) };
}
const MEDIA_TYPES = new Set(["VideoFrame", "Image", "ExternalVideoStream"]);
const CALL_STATUS = ["ok", "app_error", "no_handler", "timeout", "peer_lost", "cancelled"];
/* the @dart/meta section mask, OR the bits. 0 = every section. Mirrors DART_META_*. */
const MetaSection = { Node: 0x1, Proc: 0x2, Topics: 0x4, Peers: 0x8, All: 0 };
/* one reflected entity reply row to an Entity, camel casing the hex hash fields */
function toEntity(e) {
    const out = {
        kind: e.kind, name: e.name, provides: !!e.provides, consumes: !!e.consumes,
        reliable: !!e.reliable, index: e.index, hash: e.hash
    };
    if (e.writable !== undefined)
        out.writable = !!e.writable;
    if (e.forceable !== undefined)
        out.forceable = !!e.forceable;
    if (e.cancellable !== undefined)
        out.cancellable = !!e.cancellable;
    if (e.exclusive !== undefined)
        out.exclusive = !!e.exclusive;
    if (e.multi !== undefined)
        out.multi = !!e.multi;
    if (e.incomplete)
        out.incomplete = true;
    if (e.providers !== undefined)
        out.providers = e.providers;
    if (e.consumers !== undefined)
        out.consumers = e.consumers;
    if (e.provider !== undefined)
        out.provider = e.provider;
    if (e.from)
        out.from = e.from;
    if (e.conflict)
        out.conflict = true;
    if (e.generation !== undefined)
        out.generation = e.generation;
    if (e.schema_hash !== undefined)
        out.schemaHash = e.schema_hash;
    if (e.rsp_schema_hash !== undefined)
        out.rspSchemaHash = e.rsp_schema_hash;
    if (e.progress_schema_hash !== undefined)
        out.progressSchemaHash = e.progress_schema_hash;
    if (e.schema)
        out.schema = e.schema;
    if (e.rsp)
        out.rspSchema = e.rsp;
    if (e.prg)
        out.progressSchema = e.prg;
    return out;
}
/* default Response.message per status when the definition sent no text (mirrors the C) */
const CALL_STATUS_TEXT = {
    ok: "", app_error: "app error", no_handler: "no handler",
    timeout: "timeout", peer_lost: "peer lost", cancelled: "cancelled",
};
const SCALAR_BYTES = {
    u8: 1, u16: 2, u32: 4, u64: 8, i8: 1, i16: 2, i32: 4, i64: 8, f32: 4, f64: 8, bool: 1,
};
const VARIABLE = new Set(["vstring", "varr", "map"]);
function readScalar(view, kind, off) {
    switch (kind) {
        case "u8": return view.getUint8(off);
        case "u16": return view.getUint16(off, true);
        case "u32": return view.getUint32(off, true);
        case "u64": return view.getBigUint64(off, true);
        case "i8": return view.getInt8(off);
        case "i16": return view.getInt16(off, true);
        case "i32": return view.getInt32(off, true);
        case "i64": return view.getBigInt64(off, true);
        case "f32": return view.getFloat32(off, true);
        case "f64": return view.getFloat64(off, true);
        case "bool": return view.getUint8(off) !== 0;
        default: throw new Error(`unknown kind ${kind}`);
    }
}
function writeScalar(view, kind, off, v) {
    switch (kind) {
        case "u8":
            view.setUint8(off, Number(v));
            break;
        case "u16":
            view.setUint16(off, Number(v), true);
            break;
        case "u32":
            view.setUint32(off, Number(v), true);
            break;
        case "u64":
            view.setBigUint64(off, BigInt(v), true);
            break;
        case "i8":
            view.setInt8(off, Number(v));
            break;
        case "i16":
            view.setInt16(off, Number(v), true);
            break;
        case "i32":
            view.setInt32(off, Number(v), true);
            break;
        case "i64":
            view.setBigInt64(off, BigInt(v), true);
            break;
        case "f32":
            view.setFloat32(off, Number(v), true);
            break;
        case "f64":
            view.setFloat64(off, Number(v), true);
            break;
        case "bool":
            view.setUint8(off, v ? 1 : 0);
            break;
        default: throw new Error(`unknown kind ${kind}`);
    }
}
/* Read a capped string slot [u16 len][cap bytes] at off. len is clamped to cap like the C
 * reader, so a hostile length never over reads. */
function readCappedString(view, data, off, cap) {
    const len = Math.min(view.getUint16(off, true), cap);
    return dec.decode(data.subarray(off + 2, off + 2 + len));
}
function writeCappedString(view, buf, off, cap, s) {
    const b = enc.encode(s ?? "");
    if (b.length > cap)
        throw new Error(`string ${b.length} > cap ${cap}`);
    view.setUint16(off, b.length, true);
    buf.set(b, off + 2); /* the rest of the slot stays zero (buf starts zeroed) */
}
/* Locate the ordinal-th variable-field frame [u32 len][payload] in the message tail
 * (which starts at fixedSize). Returns { off, len } into `data`, or null if truncated. */
function varFrame(data, view, fixedSize, ordinal) {
    let pos = fixedSize;
    for (let k = 0;; k++) {
        if (pos + 4 > data.length)
            return null;
        const len = view.getUint32(pos, true);
        pos += 4;
        if (pos + len > data.length)
            return null;
        if (k === ordinal)
            return { off: pos, len };
        pos += len;
    }
}
/* ---- the self-describing map body (a tagged value tree) --------------------------- */
const MAP_U64 = 3, MAP_I64 = 7, MAP_F64 = 9, MAP_BOOL = 10, MAP_VSTR = 14, MAP_VARR = 15, MAP_MAP = 16;
function readMapValue(data, view, off) {
    const kind = data[off++];
    switch (kind) {
        case 0: return { value: data[off], off: off + 1 }; /* u8 */
        case 1: return { value: view.getUint16(off, true), off: off + 2 }; /* u16 */
        case 2: return { value: view.getUint32(off, true), off: off + 4 }; /* u32 */
        case 3: return { value: view.getBigUint64(off, true), off: off + 8 }; /* u64 */
        case 4: return { value: view.getInt8(off), off: off + 1 }; /* i8 */
        case 5: return { value: view.getInt16(off, true), off: off + 2 }; /* i16 */
        case 6: return { value: view.getInt32(off, true), off: off + 4 }; /* i32 */
        case 7: return { value: view.getBigInt64(off, true), off: off + 8 }; /* i64 */
        case 8: return { value: view.getFloat32(off, true), off: off + 4 }; /* f32 */
        case 9: return { value: view.getFloat64(off, true), off: off + 8 }; /* f64 */
        case 10: return { value: data[off] !== 0, off: off + 1 }; /* bool */
        case 14: { /* vstring */
            const len = view.getUint16(off, true);
            off += 2;
            return { value: dec.decode(data.subarray(off, off + len)), off: off + len };
        }
        case 15: { /* varr */
            const n = view.getUint16(off, true);
            off += 2;
            const arr = [];
            for (let i = 0; i < n; i++) {
                const r = readMapValue(data, view, off);
                arr.push(r.value);
                off = r.off;
            }
            return { value: arr, off };
        }
        case 16: return readMapBody(data, view, off); /* nested map */
        default: throw new Error(`bad map value kind ${kind}`);
    }
}
function readMapBody(data, view, off) {
    const n = view.getUint16(off, true);
    off += 2;
    const obj = {};
    for (let i = 0; i < n; i++) {
        const kl = data[off++];
        const key = dec.decode(data.subarray(off, off + kl));
        off += kl;
        const r = readMapValue(data, view, off);
        obj[key] = r.value;
        off = r.off;
    }
    return { value: obj, off };
}
function decodeMap(frame) {
    if (frame.length === 0)
        return {};
    const view = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);
    return readMapBody(frame, view, 0).value;
}
/* A little-endian byte sink for building a map body. */
class ByteSink {
    constructor() { this.a = []; this._dv = new DataView(new ArrayBuffer(8)); }
    u8(v) { this.a.push(v & 0xff); }
    u16(v) { this.a.push(v & 0xff, (v >> 8) & 0xff); }
    _push(n) { for (let i = 0; i < n; i++)
        this.a.push(this._dv.getUint8(i)); }
    i64(v) { this._dv.setBigInt64(0, BigInt(v), true); this._push(8); }
    u64(v) { this._dv.setBigUint64(0, BigInt(v), true); this._push(8); }
    f64(v) { this._dv.setFloat64(0, v, true); this._push(8); }
    raw(u8) { for (const b of u8)
        this.a.push(b); }
    str(s) { const b = enc.encode(s); this.u16(b.length); this.raw(b); }
    bytes() { return new Uint8Array(this.a); }
}
/* JS value to map value. bigint keeps its 64 bit width and sign, a plain number encodes
 * as i64 when integral and f64 otherwise, which the C reader widens losslessly. */
function writeMapValue(sink, v) {
    if (typeof v === "boolean") {
        sink.u8(MAP_BOOL);
        sink.u8(v ? 1 : 0);
    }
    else if (typeof v === "bigint") {
        if (v < 0n) {
            sink.u8(MAP_I64);
            sink.i64(v);
        }
        else {
            sink.u8(MAP_U64);
            sink.u64(v);
        }
    }
    else if (typeof v === "number") {
        if (Number.isInteger(v)) {
            sink.u8(MAP_I64);
            sink.i64(v);
        }
        else {
            sink.u8(MAP_F64);
            sink.f64(v);
        }
    }
    else if (typeof v === "string") {
        sink.u8(MAP_VSTR);
        sink.str(v);
    }
    else if (Array.isArray(v)) {
        sink.u8(MAP_VARR);
        sink.u16(v.length);
        for (const x of v)
            writeMapValue(sink, x);
    }
    else if (v && typeof v === "object") {
        sink.u8(MAP_MAP);
        writeMapBody(sink, v);
    }
    else
        throw new Error(`cannot encode map value: ${v}`);
}
function writeMapBody(sink, obj) {
    const keys = Object.keys(obj);
    sink.u16(keys.length);
    for (const k of keys) {
        const kb = enc.encode(k);
        if (kb.length > 255)
            throw new Error(`map key too long: ${k}`);
        sink.u8(kb.length);
        sink.raw(kb);
        writeMapValue(sink, obj[k]);
    }
}
function encodeMap(obj) {
    const s = new ByteSink();
    writeMapBody(s, obj);
    return s.bytes();
}
/* Decode an arr or varr payload into a JS value: a Uint8Array view for u8 elements, an
 * Array of strings for string elements, else an Array of scalars. */
function decodeArray(f, data, view, off, len) {
    if (f.elem === "string") {
        const slot = 2 + (f.cap ?? 0), count = Math.floor(len / slot), out = new Array(count);
        for (let i = 0; i < count; i++)
            out[i] = readCappedString(view, data, off + i * slot, f.cap ?? 0);
        return out;
    }
    if (f.elem === "u8")
        return data.subarray(off, off + len);
    const n = SCALAR_BYTES[f.elem], count = Math.floor(len / n), out = new Array(count);
    for (let i = 0; i < count; i++)
        out[i] = readScalar(view, f.elem, off + i * n);
    return out;
}
/* an enum value: a number passes through, a string is resolved to its option value */
function enumToValue(f, v) {
    if (typeof v !== "string")
        return v;
    const hit = f.variants?.find((o) => o.name === v);
    if (!hit)
        throw new Error(`'${f.path}': unknown enum option '${v}'`);
    return hit.value;
}
function writeFixedField(view, buf, f, v) {
    if (f.kind === "struct")
        throw new Error(`'${f.path}' is a struct: set its members`);
    if (f.kind === "enum") {
        writeScalar(view, f.backing, f.offset, enumToValue(f, v));
        return;
    }
    if (f.kind === "string") {
        writeCappedString(view, buf, f.offset, f.cap ?? 0, v);
        return;
    }
    if (f.kind === "arr" && f.elem === "string") {
        const slot = 2 + (f.cap ?? 0);
        if (v.length > (f.count ?? 0))
            throw new Error(`'${f.path}': ${v.length} > ${f.count} strings`);
        for (let i = 0; i < v.length; i++)
            writeCappedString(view, buf, f.offset + i * slot, f.cap ?? 0, v[i]);
        return;
    }
    if (f.kind === "arr") {
        const elems = typeof v === "string" ? enc.encode(v) : v;
        if (elems.length > (f.count ?? 0))
            throw new Error(`'${f.path}': ${elems.length} > ${f.count} elements`);
        const n = SCALAR_BYTES[f.elem];
        for (let i = 0; i < elems.length; i++)
            writeScalar(view, f.elem, f.offset + i * n, elems[i]);
        return;
    }
    writeScalar(view, f.kind, f.offset, v);
}
/* Build the payload frame for one variable field (empty when the value is absent). */
function encodeVarFrame(f, v) {
    if (v === undefined || v === null)
        return new Uint8Array(0);
    if (f.kind === "vstring")
        return enc.encode(v);
    if (f.kind === "map")
        return encodeMap(v);
    /* varr */
    if (f.elem === "string") {
        const slot = 2 + (f.cap ?? 0), buf = new Uint8Array(v.length * slot);
        const view = new DataView(buf.buffer);
        for (let i = 0; i < v.length; i++)
            writeCappedString(view, buf, i * slot, f.cap ?? 0, v[i]);
        return buf;
    }
    if (f.elem === "u8" && v instanceof Uint8Array)
        return v;
    const elems = typeof v === "string" ? enc.encode(v) : v;
    const n = SCALAR_BYTES[f.elem], buf = new Uint8Array(elems.length * n);
    const view = new DataView(buf.buffer);
    for (let i = 0; i < elems.length; i++)
        writeScalar(view, f.elem, i * n, elems[i]);
    return buf;
}
/* nested plain-object helpers for the full-message codec */
function setPath(obj, path, v) {
    const segs = path.split(".");
    let o = obj;
    for (let i = 0; i < segs.length - 1; i++)
        o = o[segs[i]] ?? (o[segs[i]] = {});
    o[segs[segs.length - 1]] = v;
}
function getPath(obj, path) {
    let o = obj;
    for (const s of path.split(".")) {
        if (o === undefined || o === null)
            return undefined;
        o = o[s];
    }
    return o;
}
/* One compiled schema's field tables from a create reply, with the field codec and the
 * whole message encode and decode over plain nested objects. No schema passes bytes through. */
class Layout {
    constructor(r) {
        this.name = r?.name;
        this.size = r?.size;
        this.hash = r?.hash;
        const list = r?.fields ?? [];
        this.fields = new Map(list.map((f) => [f.path, f]));
        this.varFields = [];
        for (const f of list)
            if (VARIABLE.has(f.kind)) {
                f.varOrdinal = this.varFields.length;
                this.varFields.push(f);
            }
        /* a BARE TYPE: one anonymous field (empty path), so the message IS one value */
        this.valueRoot = list.length === 1 && list[0].path === "" && list[0].kind !== "struct";
    }
    get typed() { return this.size !== undefined; }
    /* Every VideoFrame / Image in this type, by dotted path ("" = the message itself):
     * what a VideoView can attach to, wherever it sits. */
    mediaFields() {
        const out = [];
        if (this.name && MEDIA_TYPES.has(this.name))
            out.push({ path: "", type: this.name });
        for (const f of this.fields.values())
            if (f.named && MEDIA_TYPES.has(f.named))
                out.push({ path: f.path, type: f.named });
        return out;
    }
    /* one field by dotted path (see DartMessage.get) */
    getField(data, view, path) {
        const f = this.fields.get(path);
        if (!f)
            throw new Error(`no field '${path}'`);
        if (VARIABLE.has(f.kind)) {
            const fr = varFrame(data, view, this.size, f.varOrdinal);
            if (!fr)
                return f.kind === "varr" ? [] : (f.kind === "map" ? {} : "");
            const frame = data.subarray(fr.off, fr.off + fr.len);
            if (f.kind === "vstring")
                return dec.decode(frame);
            if (f.kind === "map")
                return decodeMap(frame);
            return decodeArray(f, frame, new DataView(frame.buffer, frame.byteOffset, frame.byteLength), 0, frame.length);
        }
        if (f.kind === "struct")
            return data.subarray(f.offset, f.offset + f.size);
        if (f.kind === "enum")
            return readScalar(view, f.backing, f.offset); /* the number */
        if (f.kind === "string")
            return readCappedString(view, data, f.offset, f.cap ?? 0);
        if (f.kind === "arr")
            return decodeArray(f, data, view, f.offset, f.size);
        return readScalar(view, f.kind, f.offset);
    }
    /* enum option helpers (by field path): resolve a wire number to its option name (""
     * if none, i.e. an unknown/newer value) and a name to its number (undefined if none). */
    enumName(path, value) {
        const f = this.fields.get(path);
        const hit = f?.variants?.find((o) => BigInt(o.value) === BigInt(value));
        return hit ? hit.name : "";
    }
    enumValue(path, name) {
        return this.fields.get(path)?.variants?.find((o) => o.name === name)?.value;
    }
    /* Full-message decode into a plain nested object (structs become sub-objects), or the
     * bare value for a bare-type schema. An untyped layout returns the raw bytes unchanged. */
    decode(data) {
        if (!this.typed)
            return data;
        const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
        if (this.valueRoot)
            return this.getField(data, view, "");
        const out = {};
        for (const f of this.fields.values()) {
            if (f.kind === "struct")
                continue; /* members fill it via their paths */
            setPath(out, f.path, this.getField(data, view, f.path));
        }
        return out;
    }
    /* Whole message encode from a plain nested object, missing fixed fields zero and
     * variable ones empty, or from the bare value of a bare type schema. Untyped takes bytes. */
    encode(value) {
        if (!this.typed) {
            if (value === undefined || value === null)
                return new Uint8Array(0);
            if (value instanceof Uint8Array)
                return value;
            throw new Error("raw entity: pass a Uint8Array");
        }
        if (this.valueRoot)
            value = { "": value }; /* the value IS the one anonymous field */
        const fixed = new Uint8Array(this.size);
        const view = new DataView(fixed.buffer);
        for (const f of this.fields.values()) {
            if (f.kind === "struct" || VARIABLE.has(f.kind))
                continue;
            const v = getPath(value, f.path);
            if (v === undefined)
                continue;
            writeFixedField(view, fixed, f, v);
        }
        const frames = this.varFields.map((f) => encodeVarFrame(f, getPath(value, f.path)));
        let total = this.size;
        for (const fr of frames)
            total += 4 + fr.length;
        const buf = new Uint8Array(total);
        buf.set(fixed, 0);
        const dv = new DataView(buf.buffer);
        let pos = this.size;
        for (const fr of frames) {
            dv.setUint32(pos, fr.length, true);
            pos += 4;
            buf.set(fr, pos);
            pos += fr.length;
        }
        return buf;
    }
}
/* A delivered message: raw bytes plus typed reads through the entity's field table. */
class DartMessage {
    constructor(layout, topic, publisher, data, writtenUs = 0) {
        this._layout = layout;
        this.topic = topic;
        this.publisher = publisher;
        this.data = data;
        this.writtenUs = writtenUs;
        this._view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    }
    /* Typed read of one field by dotted path. The JS types per kind are in docs/javascript.md. */
    get(path) { return this._layout.getField(this.data, this._view, path); }
    /* The whole message as a plain nested object, the raw bytes when untyped. Decoded once. */
    value() { return this._value ?? (this._value = this._layout.decode(this.data)); }
    /* A u8 array field decoded as UTF-8 text with trailing NULs stripped, or up to lenField's
     * value. Prefer a string field, which get returns directly. */
    text(path, lenField) {
        const bytes = this.get(path);
        let n = lenField !== undefined ? Number(this.get(lenField)) : bytes.length;
        if (lenField === undefined)
            while (n > 0 && bytes[n - 1] === 0)
                n--;
        return dec.decode(bytes.subarray(0, Math.min(n, bytes.length)));
    }
}
/* Everything created on a node: one client-chosen id (also its WebRTC data channel id),
 * one match summary from the bridge's pushes, one frame inbox. */
class DartEntity {
    constructor(node, name, r, dc) {
        this._node = node;
        this.id = r.id;
        this.name = name;
        this.reliable = !!r.reliable;
        this.reflected = !!r.reflected;
        this.matchCount = 0;
        this.ready = false;
        this._dc = dc;
        this._taps = [];
    }
    /* A reflect handle: re-type from the mesh if what it took has moved (the bridge
     * re-types in place, the layouts here follow). Resolves true when the types changed. */
    async refresh() {
        const r = await this._node._request({ op: "refresh", id: this.id });
        this.reflected = !!r.reflected;
        this._retype(r);
        return !!r.retyped;
    }
    _match(m) { this.matchCount = m.count; this.ready = !!m.ready; }
    _frame(_f) { }
    _retype(_r) { }
    _tap(value) { for (const t of this._taps)
        t(value); }
    _send(op, flags, seq, text, payload) {
        this._node._sendFrame(this, buildFrame(op, flags, this.id, seq, text, payload));
    }
}
/* One topic on the node (the dynamic form). Returned by DartNode.topic(). */
class DartTopic extends DartEntity {
    constructor(node, name, r, dc) {
        super(node, name, r, dc);
        this.layout = new Layout(r.schema);
        this.onMessage = null;
    }
    _retype(r) { this.layout = new Layout(r.schema); }
    /* fixed-section size / schema hash / field table (typed topics) */
    get size() { return this.layout.size; }
    get hash() { return this.layout.hash; }
    get fields() { return this.layout.fields; }
    /* Publish raw bytes. */
    sendRaw(bytes) { this._send(OP_DATA, 0, 0, "", bytes); }
    /* Typed publish from a plain nested object mirroring the schema. A bare type topic takes
     * the value itself. */
    send(value) { this.sendRaw(this.layout.encode(value)); }
    /* Flip this topic's role: "pubsub" | "pub" | "sub" | "inactive". */
    setRole(role) { return this._node._request({ op: "role", id: this.id, role }); }
    /* Wait until every reader acked everything (reliable topics, before close). */
    async drain(timeout_ms = 1000) {
        const r = await this._node._request({ op: "drain", id: this.id, timeout_ms });
        return r.drained;
    }
    _frame(f) {
        if (f.op !== OP_DATA)
            return;
        const msg = new DartMessage(this.layout, this, f.peer, f.payload, f.writtenUs);
        this.onMessage?.(msg);
        if (this._taps.length)
            this._tap(msg.value());
    }
}
/* The publish side handle over a topic, speaking plain nested objects. */
class Publisher {
    constructor(topic) { this.topic = topic; }
    send(value) { this.topic.send(value); }
    sendRaw(bytes) { this.topic.sendRaw(bytes); }
    get matchCount() { return this.topic.matchCount; }
    get ready() { return this.topic.ready; }
}
/* Subscribe-side handle: the handler gets (decoded plain object, message). */
class Subscriber {
    constructor(topic, handler) {
        this.topic = topic;
        this.topic.onMessage = (msg) => handler(msg.value(), msg);
    }
    get matchCount() { return this.topic.matchCount; }
}
/* the RESULT frame a definition answers a request with */
function replyFrame(entity, reqId, status, err, rsp) {
    const t = err === undefined ? "" : typeof err?.message === "string" ? err.message : (typeof err === "string" ? err : "");
    entity._send(OP_RESULT, status, reqId, t, rsp);
}
/* The implementation side of a function: the bridge defers every request here, the
 * handler's return value is the reply and a throw answers "app_error". One per name. */
class FunctionDefinition extends DartEntity {
    constructor(node, name, r, dc, handler) {
        super(node, name, r, dc);
        this.reqLayout = new Layout(r.req);
        this.rspLayout = new Layout(r.rsp);
        this._handler = handler;
    }
    get callerCount() { return this.matchCount; } /* callers currently matched */
    _retype(r) { this.reqLayout = new Layout(r.req); this.rspLayout = new Layout(r.rsp); }
    _frame(f) {
        if (f.op === OP_CALL)
            void this._handle(f.seq, { caller: f.peer, callerName: f.text, writtenUs: f.writtenUs }, f.payload);
    }
    async _handle(reqId, info, payload) {
        try {
            const out = await this._handler(this.reqLayout.decode(payload), info);
            replyFrame(this, reqId, 0, undefined, this.rspLayout.encode(out));
        }
        catch (e) {
            replyFrame(this, reqId, 1, e, new Uint8Array(0)); /* app_error + the throw's text */
        }
    }
}
/* A reference to a function definition on another node. call() resolves with the
 * outcome and NEVER rejects on a status (only on connection loss). */
class RemoteFunction extends DartEntity {
    constructor(node, name, r, dc) {
        super(node, name, r, dc);
        this.reqLayout = new Layout(r.req);
        this.rspLayout = new Layout(r.rsp);
    }
    get hasDefinition() { return this.matchCount > 0; } /* a definition is matched */
    _retype(r) { this.reqLayout = new Layout(r.req); this.rspLayout = new Layout(r.rsp); }
    /* Call the remote function. timeoutMs > 0 adds a client side bound resolving "timeout".
     * The bridge's own timeout still answers with a wire status when it fires first. */
    call(value, timeoutMs = 0) {
        const payload = this.reqLayout.encode(value);
        const callId = ++this._node._nextCall;
        return new Promise((resolve, reject) => {
            const p = { resolve, reject, layout: this.rspLayout, timer: undefined };
            if (timeoutMs > 0) {
                p.timer = setTimeout(() => {
                    this._node._calls.delete(callId);
                    resolve({ ok: false, status: "timeout", value: undefined,
                        data: new Uint8Array(0), provider: 0, writtenUs: 0,
                        message: CALL_STATUS_TEXT.timeout });
                }, timeoutMs);
            }
            this._node._calls.set(callId, p);
            this._send(OP_CALL, 0, callId, "", payload);
        });
    }
}
/* The implementation side of a task: the bridge defers every request here, the async
 * handler streams ctx.progress() and its settlement is the one terminal answer. */
class TaskDefinition extends DartEntity {
    constructor(node, name, r, dc, handler) {
        super(node, name, r, dc);
        this.reqLayout = new Layout(r.req);
        this.prgLayout = new Layout(r.prg);
        this.rspLayout = new Layout(r.rsp);
        this._handler = handler;
        this._aborts = new Map();
    }
    get callerCount() { return this.matchCount; }
    _retype(r) {
        this.reqLayout = new Layout(r.req);
        this.prgLayout = new Layout(r.prg);
        this.rspLayout = new Layout(r.rsp);
    }
    _frame(f) {
        if (f.op === OP_CALL)
            void this._handle(f.seq, { caller: f.peer, callerName: f.text, writtenUs: f.writtenUs }, f.payload);
        else if (f.op === OP_CANCEL)
            this._aborts.get(f.seq)?.abort(); /* the default reason */
    }
    async _handle(reqId, info, payload) {
        const ctrl = new AbortController();
        this._aborts.set(reqId, ctrl);
        let done = false;
        const prg = this.prgLayout;
        const ctx = {
            progress: (v) => {
                if (done)
                    throw new Error("task request already completed");
                this._send(OP_PROGRESS, 0, reqId, "", prg.encode(v));
            },
            signal: ctrl.signal,
            get cancelled() { return ctrl.signal.aborted; },
            caller: info.caller,
            callerName: info.callerName,
            writtenUs: info.writtenUs,
        };
        try {
            const out = await this._handler(this.reqLayout.decode(payload), ctx);
            done = true;
            this._aborts.delete(reqId);
            replyFrame(this, reqId, 0, undefined, this.rspLayout.encode(out));
        }
        catch (e) {
            /* the abort reason or any AbortError after a cancel means the handler honored it.
             * Anything else is an app error carrying the throw's text */
            done = true;
            this._aborts.delete(reqId);
            const honored = ctrl.signal.aborted && (e === ctrl.signal.reason || e?.name === "AbortError");
            replyFrame(this, reqId, honored ? 5 : 1, e, new Uint8Array(0));
        }
    }
}
/* One task invocation in flight, returned synchronously by RemoteTask.call. result is the
 * terminal Response, onProgress observes and replays buffered updates, cancel() asks to stop. */
class TaskRun {
    constructor(node, id, prgLayout, callId, result) {
        this._node = node;
        this._prgLayout = prgLayout;
        this.id = id;
        this.callId = callId;
        this.result = result;
        this._onProgress = null;
        this._buffered = [];
        this._taps = [];
    }
    /* One handler, a re register replaces and null clears. Buffered updates replay in order. */
    onProgress(handler) {
        this._onProgress = handler;
        if (handler) {
            const b = this._buffered;
            this._buffered = [];
            for (const u of b)
                handler(u.value, u.info);
        }
        return this;
    }
    /* Request cancellation. Cooperative and never acked on the wire: resolves with the
     * local verdict, and the terminal result's status is the real answer. */
    async cancel() {
        const r = await this._node._request({ op: "cancel", call: this.callId });
        return r.status;
    }
    _push(data, provider, writtenUs) {
        const value = data.length ? this._prgLayout.decode(data) : null;
        if (this._onProgress)
            this._onProgress(value, { provider, writtenUs });
        else
            this._buffered.push({ value, info: { provider, writtenUs } });
        if (value !== null)
            for (const t of this._taps)
                t(value);
    }
}
/* A reference to a task defined elsewhere. call() returns a TaskRun synchronously and the
 * timeout bounds only the first response, so there is no client side timer. */
class RemoteTask extends DartEntity {
    constructor(node, name, r, dc) {
        super(node, name, r, dc);
        this.reqLayout = new Layout(r.req);
        this.prgLayout = new Layout(r.prg);
        this.rspLayout = new Layout(r.rsp);
    }
    get hasDefinition() { return this.matchCount > 0; }
    _retype(r) {
        this.reqLayout = new Layout(r.req);
        this.prgLayout = new Layout(r.prg);
        this.rspLayout = new Layout(r.rsp);
    }
    call(value) {
        const payload = this.reqLayout.encode(value);
        const callId = ++this._node._nextCall;
        let run;
        const result = new Promise((resolve, reject) => {
            const p = { resolve, reject, layout: this.rspLayout, timer: undefined,
                progress: (data, provider, writtenUs) => run._push(data, provider, writtenUs) };
            this._node._calls.set(callId, p);
        });
        run = new TaskRun(this._node, this.id, this.prgLayout, callId, result);
        this._send(OP_CALL, 0, callId, "", payload);
        return run;
    }
}
/* Shared variable-handle core: the client-cached latest value fed by pushed updates. */
class VarHandle extends DartEntity {
    constructor(node, name, r, dc) {
        super(node, name, r, dc);
        this.layout = new Layout(r.schema);
        this.forced = false;
        this.writtenUs = 0;
        this._value = undefined;
        this._raw = undefined;
        this._waiters = new Set();
        this._onChange = null;
        this._onWrite = null;
    }
    /* the cached latest value as a plain object (undefined = none seen yet) */
    get() { return this._value; }
    /* the cached latest value's raw bytes */
    getRaw() { return this._raw; }
    /* Resolve true once a value is cached (immediately if one already is), false on
     * timeout. timeoutMs < 0 waits indefinitely. */
    wait(timeoutMs = -1) {
        if (this._value !== undefined)
            return Promise.resolve(true);
        return new Promise((res) => {
            const w = { res, timer: undefined };
            if (timeoutMs >= 0)
                w.timer = setTimeout(() => { this._waiters.delete(w); res(false); }, timeoutMs);
            this._waiters.add(w);
        });
    }
    /* Observe changes: fires per pushed update, and once at registration when a value is
     * cached, so a late registration never misses the state. One handler, null clears. */
    onChange(handler) {
        this._onChange = handler;
        if (handler && this._value !== undefined)
            handler(this._value, { forced: this.forced, writtenUs: this.writtenUs, source: 0 });
    }
    /* Observe every applied write with no replay. Needs onWrite: true at create so the bridge
     * pushes them. One handler, null clears. */
    onWrite(handler) { this._onWrite = handler; }
    set(value) { this._send(OP_VAR, 0, 0, "", this.layout.encode(value)); }
    force(value) { this._send(OP_VAR, 1, 0, "", this.layout.encode(value)); }
    unforce() { this._send(OP_VAR, 2, 0, "", new Uint8Array(0)); }
    _retype(r) { this.layout = new Layout(r.schema); }
    _frame(f) {
        if (f.op !== OP_VAR)
            return;
        const forced = (f.flags & 1) !== 0;
        const info = { forced, writtenUs: f.writtenUs, source: f.peer };
        if (f.flags & 2) { /* a write event: no cache change */
            this._onWrite?.(this.layout.decode(f.payload), info);
            return;
        }
        this._raw = f.payload;
        this._value = this.layout.decode(f.payload);
        this.forced = forced;
        this.writtenUs = f.writtenUs;
        for (const w of this._waiters) {
            if (w.timer !== undefined)
                clearTimeout(w.timer);
            w.res(true);
        }
        this._waiters.clear();
        this._onChange?.(this._value, info);
        if (this._taps.length)
            this._tap(this._value);
    }
}
/* The owner side: this node holds the authoritative value. */
class VariableDefinition extends VarHandle {
    get remoteCount() { return this.matchCount; } /* remotes currently matched */
}
/* A reference to a variable owned elsewhere. set() round trips through the owner. */
class RemoteVariable extends VarHandle {
    get hasDefinition() { return this.matchCount > 0; }
}
/* ---- media -------------------------------------------------------------------------- */
/* the standard media types (docs/stdtypes.md), always in scope by name */
const VIDEO_FRAME = "VideoFrame"; /* { codec, width, height, keyframe, pts, data } */
const IMAGE = "Image"; /* { width, height, stride, format, data } */
const EXTERNAL_VIDEO_STREAM = "ExternalVideoStream"; /* kind, codec, width, height, url, name */
const VideoCodec = { Unknown: 0, Mjpeg: 1, H264: 2, H265: 3, Av1: 4 };
const ImageFormat = { Mono8: 0, Mono16: 1, Rgb8: 2, Rgba8: 3, Bgr8: 4, Yuyv: 5, Nv12: 6, Jpeg: 16, Png: 17 };
const StreamKind = { Rtsp: 0, WebrtcWhep: 1, Hls: 2, Srt: 3, Rtp: 4, HttpMjpeg: 5, Other: 15 };
/* the WebCodecs codec string for an encoded VideoFrame. H264 reads its SPS so the decoder
 * gets the real profile and level */
function codecString(codec, data) {
    if (codec === VideoCodec.H264) {
        const n = data.length;
        for (let i = 0; i + 4 < n; i++) {
            if (data[i] === 0 && data[i + 1] === 0 && (data[i + 2] === 1 || (data[i + 2] === 0 && data[i + 3] === 1))) {
                const h = i + (data[i + 2] === 1 ? 3 : 4);
                if (h + 3 < n && (data[h] & 0x1f) === 7)
                    return "avc1." + [data[h + 1], data[h + 2], data[h + 3]].map((b) => b.toString(16).padStart(2, "0")).join("");
            }
        }
        if (n > 8 && (data[4] & 0x1f) === 7) /* length-prefixed, SPS first */
            return "avc1." + [data[5], data[6], data[7]].map((b) => b.toString(16).padStart(2, "0")).join("");
        return "avc1.42e01e";
    }
    if (codec === VideoCodec.H265)
        return "hev1.1.6.L120.B0";
    return "av01.0.08M.08";
}
/* raw pixels to RGBA, for the formats a canvas cannot take directly */
function toRgba(img) {
    const { width: w, height: h, format } = img;
    const src = img.data;
    const out = new Uint8ClampedArray(w * h * 4);
    const stride = img.stride || (format === ImageFormat.Mono16 ? w * 2 : format === ImageFormat.Mono8 ? w
        : format === ImageFormat.Rgb8 || format === ImageFormat.Bgr8 ? w * 3
            : format === ImageFormat.Rgba8 ? w * 4 : format === ImageFormat.Yuyv ? w * 2 : w);
    const yuv = (y, u, v, o) => {
        const c = y - 16, d = u - 128, e = v - 128;
        out[o] = (298 * c + 409 * e + 128) >> 8;
        out[o + 1] = (298 * c - 100 * d - 208 * e + 128) >> 8;
        out[o + 2] = (298 * c + 516 * d + 128) >> 8;
        out[o + 3] = 255;
    };
    switch (format) {
        case ImageFormat.Rgba8:
            for (let y = 0; y < h; y++)
                out.set(src.subarray(y * stride, y * stride + w * 4), y * w * 4);
            return out;
        case ImageFormat.Rgb8:
        case ImageFormat.Bgr8: {
            const swap = format === ImageFormat.Bgr8;
            for (let y = 0; y < h; y++)
                for (let x = 0, s = y * stride, o = y * w * 4; x < w; x++, s += 3, o += 4) {
                    out[o] = src[swap ? s + 2 : s];
                    out[o + 1] = src[s + 1];
                    out[o + 2] = src[swap ? s : s + 2];
                    out[o + 3] = 255;
                }
            return out;
        }
        case ImageFormat.Mono8:
            for (let y = 0; y < h; y++)
                for (let x = 0, s = y * stride, o = y * w * 4; x < w; x++, s++, o += 4) {
                    out[o] = out[o + 1] = out[o + 2] = src[s];
                    out[o + 3] = 255;
                }
            return out;
        case ImageFormat.Mono16:
            for (let y = 0; y < h; y++)
                for (let x = 0, s = y * stride, o = y * w * 4; x < w; x++, s += 2, o += 4) {
                    out[o] = out[o + 1] = out[o + 2] = src[s + 1];
                    out[o + 3] = 255; /* the high byte */
                }
            return out;
        case ImageFormat.Yuyv:
            for (let y = 0; y < h; y++)
                for (let x = 0, s = y * stride, o = y * w * 4; x + 1 < w; x += 2, s += 4, o += 8) {
                    yuv(src[s], src[s + 1], src[s + 3], o);
                    yuv(src[s + 2], src[s + 1], src[s + 3], o + 4);
                }
            return out;
        case ImageFormat.Nv12: {
            const uvOff = stride * h;
            for (let y = 0; y < h; y++)
                for (let x = 0; x < w; x++) {
                    const u = src[uvOff + (y >> 1) * stride + (x & ~1)], v = src[uvOff + (y >> 1) * stride + (x & ~1) + 1];
                    yuv(src[y * stride + x], u, v, (y * w + x) * 4);
                }
            return out;
        }
        default:
            return null;
    }
}
/* A picture as a MediaStream fed from anywhere: push() shows any VideoFrame, Image or
 * ExternalVideoStream value, attach() binds it to an entity's stream (docs/javascript.md). */
class VideoView {
    constructor(node) {
        this._keepData = false;
        this._node = node;
        this.stream = typeof MediaStream !== "undefined" ? new MediaStream() : null;
        this.canvas = null;
        this.path = "none";
        this.frames = 0;
        this.width = 0;
        this.height = 0;
        this.onFrame = null;
        this.source = null;
        this.sourcePath = "";
        this.sourceType = "";
        this.trackState = "";
        this.decodeState = "";
        this.external = null;
        this.externalState = "";
        this._extPc = null;
        this._extResource = "";
        this._extImg = null;
        this._extVideo = null;
        this._extTimer = 0;
        this._tap = null;
        this._transceiver = null;
        this._ctx = null;
        this._canvasTrack = null;
        this._rtcTrack = null;
        this._decoder = null;
        this._decoderCodec = "";
        this._pendingBlob = null;
        this._blobBusy = false;
        this._closed = false;
        node._views.add(this);
    }
    /* Bind to an entity's stream: the VideoFrame or Image at path of every delivery, update or
     * progress value is pushed here, and over WebRTC an encoded field rides a video track. */
    async attach(target, opts = {}) {
        if (typeof opts === "string")
            opts = { path: opts };
        this.detach();
        const src = target instanceof Subscriber ? target.topic : target;
        const layout = src instanceof TaskRun ? src._prgLayout : src.layout;
        const fields = layout.mediaFields();
        const field = opts.path === undefined ? fields[0] : fields.find((f) => f.path === opts.path);
        const path = opts.path ?? field?.path ?? "";
        this.source = src;
        this.sourcePath = path;
        this.sourceType = field?.type ?? "";
        this._tap = (value) => {
            const v = path ? getPath(value, path) : value;
            if (!v)
                return;
            if (v.url !== undefined)
                this.push(v); /* a descriptor: follow it */
            else if (v.data && v.data.length)
                this.push(v); /* empty: the track has the pixels */
        };
        src._taps.push(this._tap);
        this._keepData = !!opts.keepData;
        if (this._node._rtcUp) {
            this._node._addVideoLine(this);
            if (this._transceiver)
                await this._node._rtcOffer();
        }
        return this;
    }
    /* Unbind from the source (the view keeps painting whatever is pushed). */
    detach() {
        const src = this.source;
        if (src && this._tap)
            src._taps = src._taps.filter((t) => t !== this._tap);
        this.source = null;
        this.sourcePath = "";
        this._tap = null;
        this.trackState = "";
        if (this._transceiver) {
            try {
                this._transceiver.stop();
            }
            catch (_e) { /* not supported everywhere */ }
            this._transceiver = null;
            this._detachTrack();
            if (this._node._rtcUp)
                void this._node._rtcOffer(); /* the line is gone, re offer */
        }
    }
    /* Detach, stop every track, close the decoder, leave any external stream. */
    close() {
        this.detach();
        this._stopExternal();
        this._closed = true;
        this._decoder?.close?.();
        this._decoder = null;
        this._canvasTrack?.stop();
        if (this.stream)
            for (const t of this.stream.getTracks())
                this.stream.removeTrack(t);
        this._node._views.delete(this);
    }
    /* Show one value: a VideoFrame (by its codec), an Image (by its format), or an
     * ExternalVideoStream (the view connects to its URL). */
    push(value) {
        if (this._closed)
            return;
        this.onFrame?.(value);
        if (typeof document === "undefined") {
            this.frames++;
            return;
        } /* no canvas here */
        if ("url" in value) {
            this._follow(value);
            return;
        }
        if ("codec" in value) {
            if (value.codec === VideoCodec.Mjpeg)
                this._paintBlob(value.data, "image/jpeg");
            else if (value.codec >= VideoCodec.H264)
                this._decode(value);
        }
        else if ("format" in value) {
            if (value.format === ImageFormat.Jpeg)
                this._paintBlob(value.data, "image/jpeg");
            else if (value.format === ImageFormat.Png)
                this._paintBlob(value.data, "image/png");
            else {
                const rgba = toRgba(value);
                if (rgba && this._surface(value.width, value.height)) {
                    this._ctx.putImageData(new ImageData(rgba, value.width, value.height), 0, 0);
                    this._painted();
                }
            }
        }
    }
    /* what the offer's `tracks` entry says about this view's line (VideoFrame fields only) */
    _trackReq() {
        const s = this.source;
        if (!s || this.sourceType !== "VideoFrame")
            return null;
        return { id: s.id, path: this.sourcePath, call: s instanceof TaskRun ? s.callId : 0, keep_data: this._keepData };
    }
    /* an ExternalVideoStream descriptor: connect to it by kind (a same-URL repeat is a no-op) */
    _follow(desc) {
        if (this.external && this.external.url === desc.url && this.external.kind === desc.kind)
            return;
        this._stopExternal();
        this.external = desc;
        this.externalState = "connecting";
        if (desc.kind === StreamKind.WebrtcWhep)
            void this._whep(desc.url);
        else if (desc.kind === StreamKind.HttpMjpeg)
            this._paintElement(Object.assign(new Image(), { src: desc.url, crossOrigin: "anonymous" }));
        else if (desc.kind === StreamKind.Hls) {
            const v = document.createElement("video");
            v.muted = true;
            v.playsInline = true;
            v.crossOrigin = "anonymous";
            v.src = desc.url;
            v.onerror = () => { this.externalState = "this browser cannot play HLS natively"; };
            void v.play().catch(() => { });
            this._paintElement(v);
        }
        else
            this.externalState = `cannot play kind ${desc.kind} in a browser`;
    }
    _stopExternal() {
        if (this._extPc) {
            try {
                this._extPc.close();
            }
            catch (_e) { /* already closed */ }
            if (this._extResource)
                fetch(this._extResource, { method: "DELETE" }).catch(() => { });
            this._extPc = null;
            this._extResource = "";
        }
        if (this._extTimer) {
            cancelAnimationFrame(this._extTimer);
            this._extTimer = 0;
        }
        if (this._extImg) {
            this._extImg.src = "";
            this._extImg = null;
        }
        if (this._extVideo) {
            this._extVideo.pause();
            this._extVideo.src = "";
            this._extVideo = null;
        }
        this._detachTrack();
        this.external = null;
        this.externalState = "";
    }
    /* WHEP (RFC draft-ietf-wish-whep): POST our offer, get the answer, receive the track */
    async _whep(url) {
        const pc = new RTCPeerConnection();
        this._extPc = pc;
        pc.addTransceiver("video", { direction: "recvonly" });
        pc.ontrack = (e) => { if (this._extPc === pc) {
            this._attachTrack(e.track);
            this.externalState = "playing";
        } };
        try {
            const offer = await pc.createOffer();
            await pc.setLocalDescription(offer);
            await new Promise((res) => {
                if (pc.iceGatheringState === "complete")
                    return res();
                const t = setTimeout(res, 1500);
                pc.onicegatheringstatechange = () => { if (pc.iceGatheringState === "complete") {
                    clearTimeout(t);
                    res();
                } };
            });
            const rsp = await fetch(url, { method: "POST", headers: { "content-type": "application/sdp" },
                body: pc.localDescription?.sdp ?? offer.sdp });
            if (!rsp.ok)
                throw new Error(`WHEP ${rsp.status}`);
            const loc = rsp.headers.get("location");
            if (loc)
                this._extResource = new URL(loc, url).toString();
            await pc.setRemoteDescription({ type: "answer", sdp: await rsp.text() });
        }
        catch (e) {
            if (this._extPc === pc) {
                this.externalState = `WHEP failed: ${e?.message ?? e}`;
                pc.close();
                this._extPc = null;
            }
        }
    }
    /* paint a live <img> (MJPEG) or <video> (HLS) onto the canvas every frame */
    _paintElement(el) {
        if (el instanceof HTMLVideoElement)
            this._extVideo = el;
        else
            this._extImg = el;
        const tick = () => {
            if (this._closed || (this._extImg !== el && this._extVideo !== el))
                return;
            const w = el instanceof HTMLVideoElement ? el.videoWidth : el.naturalWidth;
            const h = el instanceof HTMLVideoElement ? el.videoHeight : el.naturalHeight;
            if (w && h && this._surface(w, h)) {
                try {
                    this._ctx.drawImage(el, 0, 0);
                    this._painted();
                    this.externalState = "playing";
                }
                catch (_e) {
                    this.externalState = "cross-origin: the stream cannot be painted";
                }
            }
            this._extTimer = requestAnimationFrame(tick);
        };
        this._extTimer = requestAnimationFrame(tick);
    }
    /* The WebRTC video track bound to this source. It shows once RTP flows, and whenever pixels
     * arrive on the frame path instead the canvas shows. Exactly one of the two at a time. */
    _attachTrack(track) {
        if (this._closed || !this.stream)
            return;
        this._rtcTrack = track;
        track.onended = () => { if (this._rtcTrack === track)
            this._detachTrack(); };
        track.onunmute = () => { if (this._rtcTrack === track)
            this._showTrack(); };
        if (!track.muted)
            this._showTrack();
    }
    _showTrack() {
        const t = this._rtcTrack;
        if (!t || !this.stream)
            return;
        if (this._canvasTrack && this.stream.getTracks().includes(this._canvasTrack))
            this.stream.removeTrack(this._canvasTrack);
        if (!this.stream.getTracks().includes(t))
            this.stream.addTrack(t);
        this.path = "track";
    }
    _showCanvas() {
        const ct = this._canvasTrack;
        if (!ct || !this.stream)
            return;
        if (this._rtcTrack && this.stream.getTracks().includes(this._rtcTrack))
            this.stream.removeTrack(this._rtcTrack);
        if (!this.stream.getTracks().includes(ct))
            this.stream.addTrack(ct);
        this.path = "decoder";
    }
    /* the track went away (line refused, link lost): the canvas shows what is painted */
    _detachTrack() {
        const track = this._rtcTrack;
        if (!track)
            return;
        this._rtcTrack = null;
        if (this.stream && this.stream.getTracks().includes(track))
            this.stream.removeTrack(track);
        this.path = this._canvasTrack && this.stream?.getTracks().includes(this._canvasTrack) ? "decoder" : "none";
    }
    /* the canvas and its captured track, sized to the picture. false when unavailable */
    _surface(w, h) {
        if (!w || !h)
            return false;
        if (!this.canvas) {
            this.canvas = document.createElement("canvas");
            this._ctx = this.canvas.getContext("2d");
        }
        if (this.canvas.width !== w || this.canvas.height !== h) {
            this.canvas.width = w;
            this.canvas.height = h;
        }
        this.width = w;
        this.height = h;
        if (!this._canvasTrack && this.stream && this.canvas.captureStream)
            this._canvasTrack = this.canvas.captureStream(0).getVideoTracks()[0];
        return !!this._ctx;
    }
    _painted() {
        this.frames++;
        /* pixels are arriving here: show the canvas unless the track is live too (keepData) */
        if (this.path !== "decoder" && (!this._rtcTrack || this._rtcTrack.muted))
            this._showCanvas();
        this._canvasTrack?.requestFrame?.();
    }
    /* JPEG or PNG through the browser's image decoder. The newest frame wins while one decodes */
    _paintBlob(bytes, mime) {
        this._pendingBlob = { bytes, mime };
        if (this._blobBusy)
            return;
        const next = () => {
            const p = this._pendingBlob;
            this._pendingBlob = null;
            if (!p || this._closed) {
                this._blobBusy = false;
                return;
            }
            this._blobBusy = true;
            createImageBitmap(new Blob([p.bytes], { type: p.mime })).then((bmp) => {
                if (this._surface(bmp.width, bmp.height)) {
                    this._ctx.drawImage(bmp, 0, 0);
                    this._painted();
                }
                bmp.close();
                next();
            }, () => next());
        };
        next();
    }
    /* H264, H265 and AV1 through WebCodecs, with no track: the fallback, or a pushed value */
    _decode(f) {
        const VD = globalThis.VideoDecoder;
        const EVC = globalThis.EncodedVideoChunk;
        if (!VD || !EVC) { /* WebCodecs lives in secure contexts only */
            this.decodeState = "no WebCodecs VideoDecoder on this origin (https, localhost or file:// needed)";
            return;
        }
        if (!f.data.length)
            return;
        if (!this._decoder && !f.keyframe)
            return; /* a decoder starts on a keyframe */
        /* the codec string comes from the keyframe's parameter sets and holds for its deltas */
        const codec = f.keyframe ? codecString(f.codec, f.data) : this._decoderCodec;
        if (!this._decoder || this._decoderCodec !== codec) {
            this._decoder?.close?.();
            const d = new VD({
                output: (frame) => {
                    if (this._surface(frame.displayWidth, frame.displayHeight)) {
                        this._ctx.drawImage(frame, 0, 0);
                        this._painted();
                    }
                    frame.close();
                },
                error: () => { if (this._decoder === d) {
                    this._decoder = null;
                } },
            });
            const cfg = { codec, optimizeForLatency: true };
            if (f.width && f.height) {
                cfg.codedWidth = f.width;
                cfg.codedHeight = f.height;
            }
            d.configure(cfg);
            this._decoder = d;
            this._decoderCodec = codec;
        }
        try {
            this._decoder.decode(new EVC({ type: f.keyframe ? "key" : "delta",
                timestamp: Number(f.pts) || this.frames * 33333, data: f.data }));
        }
        catch (_e) {
            this._decoder = null; /* resync on the next keyframe */
        }
    }
}
/* the bridge's --ice syntax, "stun:host:port" or "turn:user:pass@host:port", to RTCIceServer */
function iceServerFromUrl(url) {
    const m = /^(stuns?|turns?):(?:([^:@]*):([^@]*)@)?(.*)$/.exec(url);
    if (!m)
        return { urls: url };
    const out = { urls: `${m[1]}:${m[4]}` };
    if (m[2] !== undefined) {
        out.username = decodeURIComponent(m[2]);
        out.credential = decodeURIComponent(m[3] ?? "");
    }
    return out;
}
class DartNode {
    /* Connect to a bridge and open the node. WebRTC is tried first (opts.transport
     * "auto", the default) and the WebSocket carries the data if it cannot connect. */
    static async connect(url, opts = {}) {
        const ws = new WebSocket(url);
        ws.binaryType = "arraybuffer";
        await new Promise((res, rej) => {
            ws.onopen = res;
            ws.onerror = () => rej(new Error(`connect failed: ${url}`));
        });
        const c = new DartNode(ws);
        const { onEvent, transport, rtcTimeoutMs, iceServers, ...open } = opts;
        if (onEvent)
            c.onEvent = onEvent;
        const r = await c._request({ op: "open", ...open });
        c.name = r.name;
        if ((transport ?? "auto") !== "websocket" && r.webrtc && typeof RTCPeerConnection !== "undefined")
            await c._rtcConnect(rtcTimeoutMs ?? 4000, iceServers ?? []);
        return c;
    }
    constructor(ws) {
        this._ws = ws;
        this._seq = 0;
        this._pending = new Map();
        this._entities = new Map();
        this._early = new Map();
        this._nextId = 0;
        this._calls = new Map();
        this._nextCall = 0;
        this._closing = false;
        this._views = new Set();
        this._pc = null;
        this._rtcUp = false;
        this._rtcMax = 65536;
        this._iceQueue = [];
        this._rtcChain = Promise.resolve();
        this._rtcFail = null;
        this._anchor = null;
        this.name = "";
        this.transport = "websocket";
        this.onEvent = null;
        this.onClose = null;
        this._onLog = null;
        ws.onmessage = (e) => {
            if (typeof e.data === "string")
                this._onText(JSON.parse(e.data));
            else
                this._onFrame(new Uint8Array(e.data));
        };
        ws.onclose = (e) => {
            for (const p of this._pending.values())
                p.reject(new Error("connection closed"));
            this._pending.clear();
            for (const p of this._calls.values()) {
                if (p.timer !== undefined)
                    clearTimeout(p.timer);
                if (this._closing)
                    p.resolve({ ok: false, status: "cancelled", value: undefined,
                        data: new Uint8Array(0), provider: 0, writtenUs: 0,
                        message: CALL_STATUS_TEXT.cancelled });
                else
                    p.reject(new Error("connection closed"));
            }
            this._calls.clear();
            this._rtcDrop();
            this.onClose?.(e);
        };
    }
    _request(obj) {
        const seq = ++this._seq;
        return new Promise((resolve, reject) => {
            this._pending.set(seq, { resolve, reject });
            this._ws.send(JSON.stringify({ ...obj, seq }));
        });
    }
    /* WebRTC: we offer, the bridge answers and candidates trickle both ways. The client always
     * offers, which makes the bridge the DTLS client (spec/bridge.md). */
    async _rtcConnect(timeoutMs, extraIce) {
        let pc = null;
        try {
            const r = await this._request({ op: "rtc" });
            const ice = [...(r.ice_servers ?? []).map(iceServerFromUrl), ...extraIce];
            pc = new RTCPeerConnection({ iceServers: ice });
            this._pc = pc;
            pc.onicecandidate = (e) => {
                if (e.candidate && e.candidate.candidate)
                    this._request({ op: "rtc", candidate: e.candidate.candidate, mid: e.candidate.sdpMid ?? "" }).catch(() => { });
            };
            /* the anchor channel puts the SCTP line in the offer (entity ids start at 1) */
            this._anchor = pc.createDataChannel("dart", { negotiated: true, id: 0 });
            for (const v of this._views)
                this._addVideoLine(v);
            const connected = new Promise((res, rej) => {
                const timer = setTimeout(() => rej(new Error("webrtc connect timeout")), timeoutMs);
                const fail = (why) => { clearTimeout(timer); rej(new Error(`webrtc ${why}`)); };
                this._rtcFail = fail; /* the bridge side can fail first, fall back at once */
                pc.onconnectionstatechange = () => {
                    const s = pc.connectionState;
                    if (s === "connected") {
                        clearTimeout(timer);
                        res();
                    }
                    else if (s === "failed" || s === "closed")
                        fail(s);
                    this._rtcState();
                };
            });
            await Promise.all([this._rtcOffer(), connected]);
            this._rtcFail = null;
            this._rtcUp = true;
            this.transport = "webrtc";
            const max = pc.sctp?.maxMessageSize;
            this._rtcMax = max && Number.isFinite(max) && max > 0 ? max : 65536;
            for (const e of this._entities.values())
                this._openChannel(e);
        }
        catch (_e) {
            this._rtcFail = null;
            this._rtcDrop(); /* the WebSocket carries everything: same API, same wire */
        }
    }
    /* one offer and answer round, the first or a re offer after a video line was added.
     * tracks tells the bridge which media entity each offered video line is for */
    _rtcOffer() {
        const pc = this._pc;
        if (!pc)
            return Promise.resolve();
        const step = async () => {
            const offer = await pc.createOffer();
            await pc.setLocalDescription(offer);
            const tracks = {};
            const lines = [];
            for (const v of this._views) {
                const req = v._transceiver?.mid ? v._trackReq() : null;
                if (req) {
                    tracks[v._transceiver.mid] = req;
                    lines.push([v._transceiver.mid, v]);
                }
            }
            const r = await this._request({ op: "rtc", sdp: pc.localDescription?.sdp ?? offer.sdp, tracks });
            await pc.setRemoteDescription({ type: "answer", sdp: r.sdp });
            for (const cand of this._iceQueue.splice(0))
                await pc.addIceCandidate(cand).catch(() => { });
            for (const [mid, v] of lines) { /* the bridge's verdict per line */
                v.trackState = (r.tracks ?? {})[mid] ?? "no answer";
                if (v.trackState !== "ok") {
                    v._detachTrack();
                    v._transceiver = null;
                }
            }
        };
        this._rtcChain = this._rtcChain.then(step, step);
        return this._rtcChain;
    }
    /* a recvonly video line for a view's source. The bridge sends on it after the re offer */
    _addVideoLine(v) {
        const pc = this._pc;
        if (!pc || v._transceiver || !v.source)
            return;
        try {
            v._transceiver = pc.addTransceiver("video", { direction: "recvonly" });
            v._attachTrack(v._transceiver.receiver.track);
        }
        catch (_e) {
            v._transceiver = null; /* frames it is, then */
        }
    }
    _rtcState() {
        const s = this._pc?.connectionState;
        const up = s === "connected";
        if (up === this._rtcUp)
            return;
        this._rtcUp = up;
        this.transport = up ? "webrtc" : "websocket"; /* a broken link falls back mid-session */
        if (up)
            for (const e of this._entities.values())
                this._openChannel(e);
    }
    _rtcDrop() {
        this._rtcUp = false;
        this.transport = "websocket";
        const pc = this._pc;
        this._pc = null;
        this._anchor = null;
        if (pc) {
            pc.onicecandidate = null;
            pc.onconnectionstatechange = null;
            try {
                pc.close();
            }
            catch (_e) { /* already closed */ }
        }
        for (const e of this._entities.values())
            e._dc = null;
        for (const v of this._views) {
            v._transceiver = null;
            v._detachTrack();
        }
    }
    /* the entity's data channel, negotiated with id = entity id so both ends open it without
     * a round trip. Ordering and retransmission follow the topic's reliability */
    _makeChannel(id, reliable) {
        const pc = this._pc;
        if (!pc || !this._rtcUp)
            return null;
        try {
            const init = reliable ? { negotiated: true, id, ordered: true }
                : { negotiated: true, id, ordered: false, maxRetransmits: 0 };
            const dc = pc.createDataChannel(`d${id}`, init);
            dc.binaryType = "arraybuffer";
            dc.onmessage = (e) => this._onFrame(new Uint8Array(e.data));
            return dc;
        }
        catch (_e) {
            return null; /* the WebSocket carries this entity */
        }
    }
    _openChannel(e) {
        if (e._dc)
            return;
        e._dc = this._makeChannel(e.id, e.reliable);
    }
    /* the carrier per frame: the entity's open data channel when the frame fits, else the
     * WebSocket. A best effort frame is dropped rather than queued behind a backlog */
    _sendFrame(e, bytes) {
        const dc = e._dc;
        if (dc && dc.readyState === "open" && bytes.byteLength <= this._rtcMax) {
            if (!e.reliable && dc.bufferedAmount > LOSSY_BUFFER)
                return;
            try {
                dc.send(bytes);
                return;
            }
            catch (_e) { /* the WebSocket takes it */ }
        }
        if (!e.reliable && this._ws.bufferedAmount > LOSSY_BUFFER)
            return;
        this._ws.send(bytes);
    }
    _onText(m) {
        if (m.op === "reply") {
            const p = this._pending.get(m.seq);
            if (!p)
                return;
            this._pending.delete(m.seq);
            if (m.ok)
                p.resolve(m);
            else
                p.reject(new Error(m.error ?? "request failed"));
        }
        else if (m.op === "event") {
            if (m.event === "rtc" && (m.state === "failed" || m.state === "closed"))
                this._rtcFail?.(m.state);
            this.onEvent?.(m);
        }
        else if (m.op === "match") {
            this._entities.get(m.id)?._match(m);
        }
        else if (m.op === "log") {
            this._onLog?.({ level: m.level, node: m.node, wallUs: m.wall_us,
                monoUs: m.mono_us, recvUs: m.recv_us, writtenUs: m.written_us ?? 0,
                text: m.text });
        }
        else if (m.op === "rtc" && typeof m.candidate === "string") {
            const cand = { candidate: m.candidate, sdpMid: m.mid ?? "" };
            if (this._pc?.remoteDescription)
                this._pc.addIceCandidate(cand).catch(() => { });
            else
                this._iceQueue.push(cand); /* the answer is still being applied */
        }
    }
    _onFrame(b) {
        const f = parseFrame(b);
        if (!f)
            return;
        if (f.op === OP_RESULT || f.op === OP_PROGRESS) {
            this._onCallFrame(f);
            return;
        }
        const e = this._entities.get(f.id);
        if (e)
            e._frame(f);
        else
            this._early.get(f.id)?.push(b); /* its create is still in flight, replay after */
    }
    /* outcomes and progress route by call id (node-wide), not by entity */
    _onCallFrame(f) {
        const p = this._calls.get(f.seq);
        if (!p)
            return; /* a client-side timeout already settled it */
        if (f.op === OP_PROGRESS) {
            p.progress?.(f.payload, f.peer, f.writtenUs);
            return;
        }
        this._calls.delete(f.seq);
        if (p.timer !== undefined)
            clearTimeout(p.timer);
        const status = CALL_STATUS[f.flags] ?? "cancelled";
        p.resolve({
            ok: status === "ok",
            status,
            value: status === "ok" ? p.layout.decode(f.payload) : undefined,
            data: f.payload,
            provider: f.peer,
            writtenUs: f.writtenUs,
            message: f.text || CALL_STATUS_TEXT[status],
        });
    }
    /* the one create round trip: pick the id, open its channel first (so nothing the
     * bridge sends right after the create can miss it), ask, register, replay early frames */
    async _create(kind, name, reliable, fields, make) {
        const id = ++this._nextId;
        if (id > 0xfffe)
            throw new Error("out of entity ids");
        const dc = this._makeChannel(id, reliable);
        this._early.set(id, []);
        let r;
        try {
            r = await this._request({ op: "create", id, kind, name, ...fields });
        }
        catch (e) {
            this._early.delete(id);
            dc?.close();
            throw e;
        }
        const ent = make(r, dc);
        this._entities.set(id, ent);
        const early = this._early.get(id) ?? [];
        this._early.delete(id);
        for (const b of early)
            this._onFrame(b);
        return ent;
    }
    /* Create a topic, the dynamic form. opts.schema is DSL text for a typed topic, omit it
     * for raw bytes. */
    async topic(name, role = "pubsub", opts = {}) {
        return this._create("topic", name, !!opts.reliable, { role, ...opts }, (r, dc) => new DartTopic(this, name, r, dc));
    }
    /* Typed publish side: schema is the DSL text (null = raw bytes). */
    async publisher(name, schema, opts = {}) {
        const t = await this.topic(name, "pub", { ...opts, ...(schema ? { schema } : {}) });
        return new Publisher(t);
    }
    /* Typed subscribe side: handler(value, msg) fires per delivery with the decoded
     * plain object (raw topic: the payload bytes). */
    async subscriber(name, schema, handler, opts = {}) {
        const t = await this.topic(name, "sub", { ...opts, ...(schema ? { schema } : {}) });
        return new Subscriber(t, handler);
    }
    /* A picture sink: push VideoFrame / Image values into it from anywhere, or attach it to
     * an entity's stream (see VideoView). videoEl.srcObject = view.stream. */
    videoView() { return new VideoView(this); }
    /* Sugar: subscribe to a VideoFrame topic, best effort unless opts say otherwise, and
     * attach a view. Over WebRTC an encoded stream arrives as a track, else frames decode here. */
    async video(name, opts = {}) {
        const t = await this.topic(name, "sub", { schema: VIDEO_FRAME, ...opts });
        return new VideoView(this).attach(t);
    }
    /* Sugar: subscribe to an Image topic and attach a view (JPEG / PNG / raw pixels). */
    async image(name, opts = {}) {
        const t = await this.topic(name, "sub", { schema: IMAGE, ...opts });
        return new VideoView(this).attach(t);
    }
    /* Host a function: handler(reqValue) returns the reply, possibly async, and a throw answers
     * "app_error". Schemas are DSL text, null = raw bytes. */
    async functionDefinition(name, reqSchema, rspSchema, handler, opts = {}) {
        return this._create("function_definition", name, true, { ...opts, ...(reqSchema ? { req: reqSchema } : {}), ...(rspSchema ? { rsp: rspSchema } : {}) }, (r, dc) => new FunctionDefinition(this, name, r, dc, handler));
    }
    /* A reference to a function hosted elsewhere. */
    async remoteFunction(name, reqSchema, rspSchema, opts = {}) {
        return this._create("remote_function", name, true, { ...opts, ...(reqSchema ? { req: reqSchema } : {}), ...(rspSchema ? { rsp: rspSchema } : {}) }, (r, dc) => new RemoteFunction(this, name, r, dc));
    }
    /* Host a task: handler(reqValue, ctx) streams ctx.progress() and its settlement is the one
     * terminal answer, ctx.signal aborts on a cancel request. Schemas are DSL text or null. */
    async taskDefinition(name, reqSchema, prgSchema, rspSchema, handler, opts = {}) {
        return this._create("task_definition", name, true, { ...opts, ...(reqSchema ? { req: reqSchema } : {}), ...(prgSchema ? { prg: prgSchema } : {}),
            ...(rspSchema ? { rsp: rspSchema } : {}) }, (r, dc) => new TaskDefinition(this, name, r, dc, handler));
    }
    /* A reference to a task hosted elsewhere. task.call(req) returns a TaskRun handle
     * synchronously: run.onProgress(cb), await run.result, run.cancel(). */
    async remoteTask(name, reqSchema, prgSchema, rspSchema, opts = {}) {
        return this._create("remote_task", name, true, { ...opts, ...(reqSchema ? { req: reqSchema } : {}), ...(prgSchema ? { prg: prgSchema } : {}),
            ...(rspSchema ? { rsp: rspSchema } : {}) }, (r, dc) => new RemoteTask(this, name, r, dc));
    }
    /* Host a variable. initial is applied with a set right after the create, since encoding
     * needs the field table the create returns. */
    async variableDefinition(name, schema, opts = {}) {
        const v = await this._create("variable_definition", name, true, {
            ...(schema ? { schema } : {}),
            ...(opts.readOnly ? { read_only: true } : {}),
            ...(opts.allowForce ? { allow_force: true } : {}),
            ...(opts.catch_up ? { catch_up: opts.catch_up } : {}),
            ...(opts.keep_last ? { keep_last: opts.keep_last } : {}),
            ...(opts.backpressure_wait_ms ? { backpressure_wait_ms: opts.backpressure_wait_ms } : {}),
            ...(opts.onWrite ? { on_write: true } : {}),
            ...(opts.reflect ? { reflect: true } : {}),
        }, (r, dc) => new VariableDefinition(this, name, r, dc));
        if (opts.initial !== undefined)
            v.set(opts.initial);
        return v;
    }
    /* Access a variable owned elsewhere. Pass { onWrite: true } to also receive every
     * applied write (route it via RemoteVariable.onWrite). */
    async remoteVariable(name, schema, opts = {}) {
        return this._create("remote_variable", name, true, { ...(schema ? { schema } : {}), ...(opts.onWrite ? { on_write: true } : {}),
            ...(opts.reflect ? { reflect: true } : {}) }, (r, dc) => new RemoteVariable(this, name, r, dc));
    }
    /* Block until discovery + matching settle for everything created so far. */
    async settle(timeoutMs = -1) {
        const r = await this._request({ op: "settle", timeout_ms: timeoutMs });
        return r.settled;
    }
    /* Publish a line on a level's built-in @dart/log topic (mesh-wide, rosout-style).
     * Every node that subscribed to that level receives it. */
    async log(level, text) {
        await this._request({ op: "log", level, text });
    }
    logError(text) { return this.log("error", text); }
    logWarn(text) { return this.log("warn", text); }
    logInfo(text) { return this.log("info", text); }
    /* Subscribe to the mesh's log stream at the given levels, default all three: every other
     * node's lines as a LogLine, history replaying on match. One handler for all levels. */
    async onLog(handler, levels = ["error", "warn", "info"]) {
        this._onLog = handler;
        await this._request({ op: "log_subscribe", levels });
    }
    /* ---- introspection (query-based, pull-only) ---------------------------------- */
    /* Snapshot the discovered peer table. A local read that resolves immediately. */
    async peers() {
        const r = await this._request({ op: "peers" });
        return r.peers.map((p) => ({
            id: p.id, name: p.name, address: p.address, active: p.active, fragmentSize: p.fragment_size,
            rttUs: p.rtt_us ?? 0, rttJitterUs: p.rtt_jitter_us ?? 0, rttMinUs: p.rtt_min_us ?? 0,
            rttSamples: p.rtt_samples ?? 0
        }));
    }
    /* The entities THIS node hosts (its functions and variables, then its topics). */
    async entities() {
        const r = await this._request({ op: "entities" });
        return r.entities.map(toEntity);
    }
    /* What one peer advertises, folded into entities. A dropped peer yields [] by default since
     * its cached entities are its dead incarnation's, includeDropped serves them anyway. */
    async peerEntities(peerId, includeDropped = false) {
        const r = await this._request({ op: "peer_entities", peer: peerId,
            include_dropped: includeDropped });
        return r.entities.map(toEntity);
    }
    /* The whole mesh folded, one entity per kind and name across every active peer and the
     * bridge's node, conflict when endpoints disagree. epoch moves on every change. */
    async mesh() {
        const r = await this._request({ op: "mesh" });
        return { entities: r.entities.map(toEntity), epoch: r.epoch ?? 0 };
    }
    /* One mesh entity by kind and name, or null. */
    async meshFind(kind, name) {
        const r = await this._request({ op: "mesh_find", kind, name });
        return r.entity ? toEntity(r.entity) : null;
    }
    /* Fetch a peer's @dart/meta snapshot by an async directed call. Never rejects on status.
     * sections is a MetaSection mask, default All. */
    async meta(peerId, sections = MetaSection.All) {
        const r = await this._request({ op: "meta", peer: peerId, sections });
        return { valid: !!r.valid, status: CALL_STATUS[r.status] ?? "cancelled",
            provider: r.provider ?? 0, info: r.info ?? {} };
    }
    /* Close the connection. The bridge closes the node with a BYE and outstanding call
     * promises settle "cancelled". */
    close() {
        this._closing = true;
        this._rtcDrop();
        this._ws.close();
    }
}
/* the constant tables, reachable from the classic-script build (one global) */
DartNode.MetaSection = MetaSection;
DartNode.VideoCodec = VideoCodec;
DartNode.ImageFormat = ImageFormat;
DartNode.StreamKind = StreamKind;
export { DartNode, DartEntity, DartTopic, DartMessage, Layout, Publisher, Subscriber, FunctionDefinition, RemoteFunction, TaskDefinition, RemoteTask, TaskRun, VariableDefinition, RemoteVariable, VideoView, VideoCodec, ImageFormat, StreamKind, VIDEO_FRAME, IMAGE, EXTERNAL_VIDEO_STREAM, MetaSection, };
