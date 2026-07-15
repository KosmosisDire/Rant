/* DART WebSocket bridge client: one DartClient = one full DART node on the mesh, spoken
 * through the bridge (see ../PROTOCOL.md). Zero dependencies, no build step: runs in
 * browsers, Node (>= 21), Deno and Bun off the global WebSocket. JSDoc-typed, so
 * TypeScript tooling gets full inference from the .mjs directly.
 *
 * Lean pub/sub only: create typed or raw topics, publish, receive. A typed topic
 * carries its own declared schema, so the client encodes/decodes with the field table
 * the `topic` reply returns (a DataView straight over the wire, no codegen). There is
 * no peer table or mesh introspection.
 *
 *   import { DartClient } from "./dart.mjs";
 *   const node = await DartClient.connect("ws://localhost:7480", { name: "dashboard" });
 *   const ch   = await node.topic("pose", "pubsub", {
 *       reliable: true, schema: "Pose { stamp: u64, x: f64, y: f64 }" });
 *   ch.onMessage = (m) => console.log(m.publisher, m.get("x"));
 *   ch.send({ stamp: 1n, x: 1.5, y: 2.0 });
 */

const OP_DATA = 0x01;

/** @typedef {{ path: string, kind: string, elem?: string, count?: number, cap?: number,
 *              offset: number, size: number, varOrdinal?: number }} Field */

const SCALAR_BYTES = { u8: 1, u16: 2, u32: 4, u64: 8, i8: 1, i16: 2, i32: 4, i64: 8,
                       f32: 4, f64: 8, bool: 1 };
const VARIABLE = new Set(["vstring", "varr", "map"]);
const enc = new TextEncoder();
const dec = new TextDecoder();

/** @returns {number|bigint|boolean} */
function readScalar(view, kind, off) {
    switch (kind) {
    case "u8":   return view.getUint8(off);
    case "u16":  return view.getUint16(off, true);
    case "u32":  return view.getUint32(off, true);
    case "u64":  return view.getBigUint64(off, true);
    case "i8":   return view.getInt8(off);
    case "i16":  return view.getInt16(off, true);
    case "i32":  return view.getInt32(off, true);
    case "i64":  return view.getBigInt64(off, true);
    case "f32":  return view.getFloat32(off, true);
    case "f64":  return view.getFloat64(off, true);
    case "bool": return view.getUint8(off) !== 0;
    default:     throw new Error(`unknown kind ${kind}`);
    }
}

function writeScalar(view, kind, off, v) {
    switch (kind) {
    case "u8":   view.setUint8(off, Number(v)); break;
    case "u16":  view.setUint16(off, Number(v), true); break;
    case "u32":  view.setUint32(off, Number(v), true); break;
    case "u64":  view.setBigUint64(off, BigInt(v), true); break;
    case "i8":   view.setInt8(off, Number(v)); break;
    case "i16":  view.setInt16(off, Number(v), true); break;
    case "i32":  view.setInt32(off, Number(v), true); break;
    case "i64":  view.setBigInt64(off, BigInt(v), true); break;
    case "f32":  view.setFloat32(off, Number(v), true); break;
    case "f64":  view.setFloat64(off, Number(v), true); break;
    case "bool": view.setUint8(off, v ? 1 : 0); break;
    default:     throw new Error(`unknown kind ${kind}`);
    }
}

/* Read a capped-string slot [u16 len][cap bytes] at off; len is clamped to cap like the
 * C reader, so a hostile length can never over-read. */
function readCappedString(view, data, off, cap) {
    const len = Math.min(view.getUint16(off, true), cap);
    return dec.decode(data.subarray(off + 2, off + 2 + len));
}

function writeCappedString(view, buf, off, cap, s) {
    const b = enc.encode(s ?? "");
    if (b.length > cap) throw new Error(`string ${b.length} > cap ${cap}`);
    view.setUint16(off, b.length, true);
    buf.set(b, off + 2);   /* the rest of the slot stays zero (buf starts zeroed) */
}

/* Locate the ordinal-th variable-field frame [u32 len][payload] in the message tail
 * (which starts at fixedSize). Returns { off, len } into `data`, or null if truncated. */
function varFrame(data, view, fixedSize, ordinal) {
    let pos = fixedSize;
    for (let k = 0; ; k++) {
        if (pos + 4 > data.length) return null;
        const len = view.getUint32(pos, true);
        pos += 4;
        if (pos + len > data.length) return null;
        if (k === ordinal) return { off: pos, len };
        pos += len;
    }
}

/* ---- the self-describing map body (a tagged value tree) --------------------------- */

const MAP_U8 = 0, MAP_U64 = 3, MAP_I64 = 7, MAP_F64 = 9, MAP_BOOL = 10,
      MAP_VSTR = 14, MAP_VARR = 15, MAP_MAP = 16;

function readMapValue(data, view, off) {
    const kind = data[off++];
    switch (kind) {
    case 0:  return { value: data[off], off: off + 1 };                        /* u8 */
    case 1:  return { value: view.getUint16(off, true), off: off + 2 };        /* u16 */
    case 2:  return { value: view.getUint32(off, true), off: off + 4 };        /* u32 */
    case 3:  return { value: view.getBigUint64(off, true), off: off + 8 };     /* u64 */
    case 4:  return { value: view.getInt8(off), off: off + 1 };                /* i8 */
    case 5:  return { value: view.getInt16(off, true), off: off + 2 };         /* i16 */
    case 6:  return { value: view.getInt32(off, true), off: off + 4 };         /* i32 */
    case 7:  return { value: view.getBigInt64(off, true), off: off + 8 };      /* i64 */
    case 8:  return { value: view.getFloat32(off, true), off: off + 4 };       /* f32 */
    case 9:  return { value: view.getFloat64(off, true), off: off + 8 };       /* f64 */
    case 10: return { value: data[off] !== 0, off: off + 1 };                  /* bool */
    case 14: {                                                                 /* vstring */
        const len = view.getUint16(off, true); off += 2;
        return { value: dec.decode(data.subarray(off, off + len)), off: off + len };
    }
    case 15: {                                                                 /* varr */
        const n = view.getUint16(off, true); off += 2;
        const arr = [];
        for (let i = 0; i < n; i++) { const r = readMapValue(data, view, off); arr.push(r.value); off = r.off; }
        return { value: arr, off };
    }
    case 16: return readMapBody(data, view, off);                              /* nested map */
    default: throw new Error(`bad map value kind ${kind}`);
    }
}

function readMapBody(data, view, off) {
    const n = view.getUint16(off, true); off += 2;
    const obj = {};
    for (let i = 0; i < n; i++) {
        const kl = data[off++];
        const key = dec.decode(data.subarray(off, off + kl)); off += kl;
        const r = readMapValue(data, view, off); obj[key] = r.value; off = r.off;
    }
    return { value: obj, off };
}

function decodeMap(frame) {
    if (frame.length === 0) return {};
    const view = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);
    return readMapBody(frame, view, 0).value;
}

/* A little-endian byte sink for building a map body. */
class ByteSink {
    constructor() { this.a = []; this._dv = new DataView(new ArrayBuffer(8)); }
    u8(v) { this.a.push(v & 0xff); }
    u16(v) { this.a.push(v & 0xff, (v >> 8) & 0xff); }
    _push(n) { for (let i = 0; i < n; i++) this.a.push(this._dv.getUint8(i)); }
    i64(v) { this._dv.setBigInt64(0, BigInt(v), true); this._push(8); }
    u64(v) { this._dv.setBigUint64(0, BigInt(v), true); this._push(8); }
    f64(v) { this._dv.setFloat64(0, v, true); this._push(8); }
    raw(u8) { for (const b of u8) this.a.push(b); }
    str(s) { const b = enc.encode(s); this.u16(b.length); this.raw(b); }
    bytes() { return new Uint8Array(this.a); }
}

/* JS value -> map value. bigint keeps its 64-bit width/sign; a plain number encodes as
 * i64 when integral, f64 otherwise (the C reader widens, so this stays lossless). */
function writeMapValue(sink, v) {
    if (typeof v === "boolean") { sink.u8(MAP_BOOL); sink.u8(v ? 1 : 0); }
    else if (typeof v === "bigint") { if (v < 0n) { sink.u8(MAP_I64); sink.i64(v); } else { sink.u8(MAP_U64); sink.u64(v); } }
    else if (typeof v === "number") {
        if (Number.isInteger(v)) { sink.u8(MAP_I64); sink.i64(v); } else { sink.u8(MAP_F64); sink.f64(v); }
    }
    else if (typeof v === "string") { sink.u8(MAP_VSTR); sink.str(v); }
    else if (Array.isArray(v)) { sink.u8(MAP_VARR); sink.u16(v.length); for (const x of v) writeMapValue(sink, x); }
    else if (v && typeof v === "object") { sink.u8(MAP_MAP); writeMapBody(sink, v); }
    else throw new Error(`cannot encode map value: ${v}`);
}

function writeMapBody(sink, obj) {
    const keys = Object.keys(obj);
    sink.u16(keys.length);
    for (const k of keys) {
        const kb = enc.encode(k);
        if (kb.length > 255) throw new Error(`map key too long: ${k}`);
        sink.u8(kb.length); sink.raw(kb); writeMapValue(sink, obj[k]);
    }
}

function encodeMap(obj) { const s = new ByteSink(); writeMapBody(s, obj); return s.bytes(); }

/* A delivered message: raw bytes plus typed reads through the topic's field table. */
export class DartMessage {
    /** @param {DartTopic} topic @param {number} publisher @param {Uint8Array} data */
    constructor(topic, publisher, data) {
        this.topic = topic;
        /** @type {number} peer id of the sending node */
        this.publisher = publisher;
        /** @type {Uint8Array} the payload, verbatim */
        this.data = data;
        this._view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    }

    /** Typed read of one field by dotted path ("vel.dx"). Scalars return number
     *  (u64/i64: bigint, bool: boolean); a `string`/`vstring` returns a JS string; a
     *  `map` returns a plain object; arrays return an Array (a u8 array returns a
     *  Uint8Array view); structs return a Uint8Array view of their bytes.
     *  @param {string} path @returns {any} */
    get(path) {
        const f = this.topic.fields.get(path);
        if (!f) throw new Error(`no field '${path}'`);

        if (VARIABLE.has(f.kind)) {
            const fr = varFrame(this.data, this._view, this.topic.size, f.varOrdinal);
            if (!fr) return f.kind === "varr" ? [] : (f.kind === "map" ? {} : "");
            const frame = this.data.subarray(fr.off, fr.off + fr.len);
            if (f.kind === "vstring") return dec.decode(frame);
            if (f.kind === "map") return decodeMap(frame);
            return decodeArray(f, frame, new DataView(frame.buffer, frame.byteOffset, frame.byteLength), 0, frame.length);
        }
        if (f.kind === "struct") return this.data.subarray(f.offset, f.offset + f.size);
        if (f.kind === "string") return readCappedString(this._view, this.data, f.offset, f.cap);
        if (f.kind === "arr") return decodeArray(f, this.data, this._view, f.offset, f.size);
        return readScalar(this._view, f.kind, f.offset);
    }

    /** A u8 array field decoded as UTF-8 text, trailing NULs stripped (or up to
     *  `lenField`'s value when given). Prefer a `string`/`vstring` field, which `get`
     *  returns as a JS string directly; this stays for `u8[]`-style byte fields.
     *  @param {string} path @param {string=} lenField @returns {string} */
    text(path, lenField) {
        let bytes = /** @type {Uint8Array} */ (this.get(path));
        let n = lenField !== undefined ? Number(this.get(lenField)) : bytes.length;
        if (lenField === undefined) while (n > 0 && bytes[n - 1] === 0) n--;
        return dec.decode(bytes.subarray(0, Math.min(n, bytes.length)));
    }
}

/* Decode an `arr`/`varr` payload (`bytes`, length `len` from `off`) into a JS value: a
 * Uint8Array view for u8 elements, an Array of strings for string elements, else an
 * Array of scalars. */
function decodeArray(f, data, view, off, len) {
    if (f.elem === "string") {
        const slot = 2 + f.cap, count = Math.floor(len / slot), out = new Array(count);
        for (let i = 0; i < count; i++) out[i] = readCappedString(view, data, off + i * slot, f.cap);
        return out;
    }
    if (f.elem === "u8") return data.subarray(off, off + len);
    const n = SCALAR_BYTES[f.elem], count = Math.floor(len / n), out = new Array(count);
    for (let i = 0; i < count; i++) out[i] = readScalar(view, f.elem, off + i * n);
    return out;
}

/* One topic (topic) on the node. Returned by DartClient.topic(). */
export class DartTopic {
    /** @param {DartClient} client @param {string} name
     *  @param {{id: number, size?: number, hash?: string, fields?: Field[]}} r */
    constructor(client, name, r) {
        this._client = client;
        /** @type {string} the topic name */
        this.name = name;
        /** @type {number} topic id in binary frames */
        this.id = r.id;
        /** @type {number|undefined} fixed-section size; where the variable tail begins */
        this.size = r.size;
        /** @type {string|undefined} 64-bit schema identity, hex */
        this.hash = r.hash;
        const list = r.fields ?? [];
        /** @type {Map<string, Field>} dotted path -> field (typed topics) */
        this.fields = new Map(list.map(f => [f.path, f]));
        /** @type {Field[]} variable fields in schema (tail) order */
        this.varFields = [];
        for (const f of list) if (VARIABLE.has(f.kind)) { f.varOrdinal = this.varFields.length; this.varFields.push(f); }
        /** @type {?(msg: DartMessage) => void} delivery callback */
        this.onMessage = null;
    }

    /** Publish raw bytes. @param {Uint8Array} bytes */
    sendRaw(bytes) {
        const frame = new Uint8Array(3 + bytes.length);
        frame[0] = OP_DATA;
        frame[1] = this.id & 0xff; frame[2] = this.id >> 8;
        frame.set(bytes, 3);
        this._client._ws.send(frame);
    }

    /** Typed publish: encode named fields (dotted paths) into a message and send it.
     *  Unset fixed fields are zero; unset variable fields are empty. Values: scalars as
     *  number/bigint/boolean; a `string`/`vstring` as a JS string (a u8 array also
     *  accepts a string); an `arr`/`varr` as an Array/TypedArray; a `map` as a plain
     *  object. @param {Record<string, any>} values */
    send(values) {
        if (this.size === undefined) throw new Error(`'${this.name}' is a raw topic: use sendRaw`);
        const fixed = new Uint8Array(this.size);
        const view = new DataView(fixed.buffer);
        for (const [path, v] of Object.entries(values)) {
            const f = this.fields.get(path);
            if (!f) throw new Error(`no field '${path}'`);
            if (VARIABLE.has(f.kind)) continue;   /* handled in tail order below */
            writeFixedField(view, fixed, f, v);
        }
        /* variable tail: one frame per variable field, in schema order */
        const frames = this.varFields.map(f => encodeVarFrame(f, values[f.path]));
        let total = this.size;
        for (const fr of frames) total += 4 + fr.length;
        const buf = new Uint8Array(total);
        buf.set(fixed, 0);
        const dv = new DataView(buf.buffer);
        let pos = this.size;
        for (const fr of frames) { dv.setUint32(pos, fr.length, true); pos += 4; buf.set(fr, pos); pos += fr.length; }
        this.sendRaw(buf);
    }

    /** Flip this topic's role: "pubsub" | "pub" | "sub" | "inactive". */
    setRole(role) { return this._client._request({ op: "role", topic: this.id, role }); }

    /** Wait until every reader acked everything (reliable topics, before close). */
    async drain(timeout_ms = 1000) {
        const r = await this._client._request({ op: "drain", topic: this.id, timeout_ms });
        return /** @type {boolean} */ (r.drained);
    }
}

function writeFixedField(view, buf, f, v) {
    if (f.kind === "struct") throw new Error(`'${f.path}' is a struct: set its members`);
    if (f.kind === "string") { writeCappedString(view, buf, f.offset, f.cap, v); return; }
    if (f.kind === "arr" && f.elem === "string") {
        const slot = 2 + f.cap;
        if (v.length > f.count) throw new Error(`'${f.path}': ${v.length} > ${f.count} strings`);
        for (let i = 0; i < v.length; i++) writeCappedString(view, buf, f.offset + i * slot, f.cap, v[i]);
        return;
    }
    if (f.kind === "arr") {
        const elems = typeof v === "string" ? enc.encode(v) : v;
        if (elems.length > f.count) throw new Error(`'${f.path}': ${elems.length} > ${f.count} elements`);
        const n = SCALAR_BYTES[f.elem];
        for (let i = 0; i < elems.length; i++) writeScalar(view, f.elem, f.offset + i * n, elems[i]);
        return;
    }
    writeScalar(view, f.kind, f.offset, v);
}

/* Build the payload frame for one variable field (empty when the value is absent). */
function encodeVarFrame(f, v) {
    if (v === undefined || v === null) return new Uint8Array(0);
    if (f.kind === "vstring") return enc.encode(v);
    if (f.kind === "map") return encodeMap(v);
    /* varr */
    if (f.elem === "string") {
        const slot = 2 + f.cap, buf = new Uint8Array(v.length * slot);
        const view = new DataView(buf.buffer);
        for (let i = 0; i < v.length; i++) writeCappedString(view, buf, i * slot, f.cap, v[i]);
        return buf;
    }
    const elems = typeof v === "string" ? enc.encode(v) : v;
    const n = SCALAR_BYTES[f.elem], buf = new Uint8Array(elems.length * n);
    const view = new DataView(buf.buffer);
    for (let i = 0; i < elems.length; i++) writeScalar(view, f.elem, i * n, elems[i]);
    return buf;
}

/* The node handle: one WebSocket connection = one DART node owned by the bridge. */
export class DartClient {
    /** Connect to a bridge and open the node.
     *  @param {string} url e.g. "ws://localhost:7480"
     *  @param {{ name?: string, domain?: number, max_topics?: number, max_peers?: number,
     *            interface?: string, seed_peers?: string[], fragment_size?: number,
     *            announce_interval_ms?: number, peer_timeout_ms?: number,
     *            disable_shm?: boolean }} [opts]
     *  @returns {Promise<DartClient>} */
    static async connect(url, opts = {}) {
        const ws = new WebSocket(url);
        ws.binaryType = "arraybuffer";
        await new Promise((res, rej) => {
            ws.onopen = res;
            ws.onerror = () => rej(new Error(`connect failed: ${url}`));
        });
        const c = new DartClient(ws);
        const r = await c._request({ op: "open", ...opts });
        c.name = /** @type {string} */ (r.name);
        return c;
    }

    /** @param {WebSocket} ws */
    constructor(ws) {
        this._ws = ws;
        this._seq = 0;
        /** @type {Map<number, {resolve: Function, reject: Function}>} */
        this._pending = new Map();
        /** @type {Map<number, DartTopic>} by topic id */
        this._topics = new Map();
        /** @type {string} this node's name (auto-generated if none was given) */
        this.name = "";
        /** @type {?(e: any) => void} every bridge event (errors, peer up/down, msg loss) */
        this.onEvent = null;
        /** @type {?(e: CloseEvent) => void} */
        this.onClose = null;

        ws.onmessage = (e) => {
            if (typeof e.data === "string") this._onText(JSON.parse(e.data));
            else this._onBinary(e.data);
        };
        ws.onclose = (e) => {
            for (const p of this._pending.values()) p.reject(new Error("connection closed"));
            this._pending.clear();
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

    _onText(m) {
        if (m.op === "reply") {
            const p = this._pending.get(m.seq);
            if (!p) return;
            this._pending.delete(m.seq);
            if (m.ok) p.resolve(m);
            else p.reject(new Error(m.error ?? "request failed"));
        } else if (m.op === "event") {
            this.onEvent?.(m);
        }
    }

    /** @param {ArrayBuffer} buf */
    _onBinary(buf) {
        const b = new Uint8Array(buf);
        if (b.length < 7 || b[0] !== OP_DATA) return;   /* reserved ops: ignore */
        const id = b[1] | (b[2] << 8);
        const publisher = b[3] | (b[4] << 8) | (b[5] << 16) | ((b[6] << 24) >>> 0);
        const ch = this._topics.get(id);
        ch?.onMessage?.(new DartMessage(ch, publisher, b.subarray(7)));
    }

    /** Create a topic (topic) on the node. Pass `schema` (DSL text) for a typed
     *  topic; omit it for a raw bytes topic.
     *  @param {string} name @param {"pubsub"|"pub"|"sub"|"inactive"} [role]
     *  @param {{ schema?: string, reliable?: boolean, keep_last?: number, catch_up?: number,
     *            max_message_bytes?: number, heartbeat_ms?: number, repair_delay_ms?: number,
     *            backpressure_wait_ms?: number, shm_max_bytes?: number }} [opts]
     *  @returns {Promise<DartTopic>} */
    async topic(name, role = "pubsub", opts = {}) {
        const r = await this._request({ op: "topic", name, role, ...opts });
        const ch = new DartTopic(this, name, r);
        this._topics.set(ch.id, ch);
        return ch;
    }

    /** Close the connection; the bridge closes the node with a BYE. */
    close() { this._ws.close(); }
}
