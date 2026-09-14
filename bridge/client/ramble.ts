/* The bridge client: one RambleNode is one full node on the mesh, spoken through the bridge
 * over a WebSocket with a WebRTC data path. docs/javascript.md explains how to use it. */

/* Data-plane frame: ONE header for every op, both directions, little-endian:
 *   [u8 op][u8 flags][u16 id][u32 seq][u32 peer][u64 written_us][u64 capture_us]
 *   [u8 text_len][text][payload] */
const OP_DATA = 1;       /* topic message (publish / delivery) */
const OP_VAR = 2;        /* variable write / update */
const OP_CALL = 3;       /* call a remote / a request for a definition */
const OP_RESULT = 4;     /* call outcome / request reply */
const OP_PROGRESS = 5;   /* task progress, either direction */
const OP_CANCEL = 6;     /* server->client: cancel a parked request */
const HDR = 29;

const LOSSY_BUFFER = 1 << 20;   /* a best-effort frame drops past this much unsent on its carrier */

type Frame = {
    op: number; flags: number; id: number; seq: number; peer: number;
    writtenUs: number; captureUs: number; text: string; payload: Uint8Array;
};

const enc = new TextEncoder();
const dec = new TextDecoder();

function buildFrame(op: number, flags: number, id: number, seq: number, text: string,
                    payload: Uint8Array, captureUs = 0): Uint8Array {
    let tb = text ? enc.encode(text) : new Uint8Array(0);
    if (tb.length > 255) tb = tb.subarray(0, 255);   /* one length byte, like the wire */
    const f = new Uint8Array(HDR + tb.length + payload.length);
    const v = new DataView(f.buffer);
    f[0] = op; f[1] = flags;
    v.setUint16(2, id, true); v.setUint32(4, seq, true);   /* peer and written_us are 0 from a client */
    if (captureUs) v.setBigUint64(20, BigInt(captureUs), true);
    f[28] = tb.length;
    f.set(tb, HDR);
    f.set(payload, HDR + tb.length);
    return f;
}

function parseFrame(b: Uint8Array): Frame | null {
    if (b.length < HDR) return null;
    const tl = b[28];
    if (b.length < HDR + tl) return null;
    const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
    return { op: b[0], flags: b[1], id: v.getUint16(2, true), seq: v.getUint32(4, true),
             peer: v.getUint32(8, true), writtenUs: Number(v.getBigUint64(12, true)),
             captureUs: Number(v.getBigUint64(20, true)),
             text: tl ? dec.decode(b.subarray(HDR, HDR + tl)) : "", payload: b.subarray(HDR + tl) };
}

type Field = {
    path: string;
    kind: string;
    named?: string;                                /* the field type's NAME ("Transform"), if any */
    elem?: string;
    elem_named?: string;                           /* an array element type's name, if any */
    elem_size?: number;                            /* bytes of one array element */
    elem_struct?: boolean;                         /* the element-0 template rows follow this one */
    in_array?: number;                             /* this row is a member of that array row */
    count?: number;
    cap?: number;
    backing?: string;                              /* enum: the wire scalar kind */
    variants?: { name: string; value: number }[];  /* enum: the option table */
    offset: number;
    size: number;
    varOrdinal?: number;
};

type SchemaBlock = { name?: string; size?: number; hash?: string; fields?: Field[] };

/* a VideoFrame, Image or ExternalVideoStream somewhere in a type (see Layout.mediaFields) */
type MediaTypeName = "VideoFrame" | "Image" | "ExternalVideoStream";
type MediaField = { path: string; type: MediaTypeName };
const MEDIA_TYPES: Set<string> = new Set(["VideoFrame", "Image", "ExternalVideoStream"]);

type Role = "pubsub" | "pub" | "sub" | "inactive";

type TopicOpts = {
    schema?: string;
    reflect?: boolean;      /* no schema: take the mesh's for this name (see Entity.refresh) */
    reliable?: boolean;
    keep_last?: number;
    catch_up?: number;
    max_message_bytes?: number;
    heartbeat_ms?: number;
    repair_delay_ms?: number;
    backpressure_wait_ms?: number;
    shm_max_bytes?: number;
    max_rate_hz?: number;   /* subscriber, best-effort: cap delivery from each publisher */
};

type TransportName = "websocket" | "webrtc";

type NodeOpts = {
    name?: string;
    domain?: number;
    max_topics?: number;
    max_peers?: number;
    interface?: string;
    seed_peers?: string[];
    unicast_only?: boolean;   /* cannot multicast: announce to seed_peers + known peers only,
                                 and be re-announced onward by whoever hears us */
    self_ip?: string;         /* state this address as our locator instead of letting peers learn
                                 it from the datagram source (static 1:1 mapping / multihomed pin) */
    advertise_port?: number;  /* advertise this data port instead of the one we bound */
    fragment_size?: number;
    recv_buffer_bytes?: number;   /* data socket OS buffers. 0 = the OS default, which is too
                                     small to hold a multi megabyte message whole */
    send_buffer_bytes?: number;
    announce_interval_ms?: number;
    peer_timeout_ms?: number;
    match_wait_ms?: number;
    disable_shm?: boolean;
    disable_logs?: boolean;   /* strip the built-in @ramble/log topics */
    disable_meta?: boolean;   /* do not host the @ramble/meta endpoint */
    disable_error_logs?: boolean; /* suppress default mirroring onto @ramble/log/error */
    fetch_details?: boolean;  /* resolve reflected names for topics this node does not share */
    transport?: "auto" | "websocket";   /* auto (default): try WebRTC, fall back to the WebSocket */
    rtcTimeoutMs?: number;    /* how long WebRTC may take to connect before falling back (4000) */
    iceServers?: RTCIceServer[];   /* extra ICE servers beside the bridge's own (--ice) */
    onEvent?: (e: RambleEvent) => void;
};

type RambleEvent = { op: "event"; event: string; text?: string } & Record<string, unknown>;

type LogLevelName = "error" | "warn" | "info";

/* One decoded @ramble/log line for RambleNode.onLog. wallUs is epoch us, monoUs the publisher's
 * monotonic clock, recvUs this node's clock at receipt, writtenUs the carrier's stamp. */
type LogLine = {
    level: LogLevelName;
    node: string;      /* the publishing node's name */
    wallUs: number;
    monoUs: number;
    recvUs: number;
    writtenUs: number;
    text: string;
};

type CallStatusName = "ok" | "app_error" | "no_handler" | "timeout" | "peer_lost" | "cancelled";
const CALL_STATUS: CallStatusName[] = ["ok", "app_error", "no_handler", "timeout", "peer_lost", "cancelled"];

/* ---- introspection (query-based, see RambleNode.peers / entities / meta) ------------ */

type Peer = {
    id: number;
    name: string;
    address: string;        /* "1.2.3.4:port" */
    active: boolean;        /* heard within peer_timeout (vs a dormant/dropped peer) */
    fragmentSize: number;
    /* the round trip as the bridge node's reliable traffic measured it, in microseconds.
       rttSamples 0 = no estimate yet */
    rttUs: number;
    rttJitterUs: number;
    rttMinUs: number;
    rttSamples: number;
};

type EntityKindName = "topic" | "function" | "task" | "variable";

/* One network entity a node hosts or a peer advertises, pattern channels folded. name is
 * "0x????????" until the peer's details are fetched. */
type Entity = {
    kind: EntityKindName;
    name: string;
    provides: boolean;      /* someone is on the source side: publisher / definition / owner */
    consumes: boolean;      /* someone is on the sink side: subscriber / caller / accessor */
    reliable: boolean;
    providers?: number;     /* live endpoints on each side (a per-node walk reports 0 or 1) */
    consumers?: number;
    provider?: number;      /* the ranked provider's peer id (0 = the bridge's own node) */
    from?: string;          /* the node the schemas were read from */
    conflict?: boolean;     /* mesh: live endpoints declare schemas that cannot read each other */
    generation?: string;    /* changes iff the provider, a schema, or an attr changed (hex) */
    writable?: boolean;     /* variable: a set channel is advertised */
    forceable?: boolean;    /* variable: the owner permits force/unforce */
    cancellable?: boolean;  /* task: the provider honors cancel (default on) */
    exclusive?: boolean;    /* task: declared serialization (the handler enforces it) */
    multi?: boolean;        /* task: redundant providers intended */
    incomplete?: boolean;   /* a pattern half-pair, surfaced not dropped */
    index: number;          /* the primary channel's index at the peer */
    hash: number;           /* the primary channel's low-32 name hash */
    schemaHash?: string;    /* the primary schema identity in hex, absent = untyped or unfetched */
    rspSchemaHash?: string; /* function/task: the response schema identity, hex */
    progressSchemaHash?: string; /* task only: the progress schema identity, hex */
    schema?: SchemaBlock;   /* the field table when the schema is known, for new Layout(e.schema) */
    rspSchema?: SchemaBlock; /* function/task: the response field table */
    progressSchema?: SchemaBlock; /* task only: the progress field table */
};

/* the @ramble/meta section mask, OR the bits. 0 = every section. Mirrors RAMBLE_META_*. */
const MetaSection = { Node: 0x1, Proc: 0x2, Topics: 0x4, Peers: 0x8, All: 0 } as const;

/* A decoded @ramble/meta reply. Never faults, inspect status. info is the whole snapshot body
 * with the sections requested. */
type MetaSnapshot = {
    valid: boolean;              /* a status "ok" reply decoded */
    status: CallStatusName;
    provider: number;            /* peer id that answered */
    info: Record<string, any>;   /* the decoded body (empty when not valid) */
};

/* one reflected entity reply row to an Entity, camel casing the hex hash fields */
function toEntity(e: any): Entity {
    const out: Entity = {
        kind: e.kind, name: e.name, provides: !!e.provides, consumes: !!e.consumes,
        reliable: !!e.reliable, index: e.index, hash: e.hash };
    if (e.writable !== undefined)    out.writable = !!e.writable;
    if (e.forceable !== undefined)   out.forceable = !!e.forceable;
    if (e.cancellable !== undefined) out.cancellable = !!e.cancellable;
    if (e.exclusive !== undefined)   out.exclusive = !!e.exclusive;
    if (e.multi !== undefined)       out.multi = !!e.multi;
    if (e.incomplete)                out.incomplete = true;
    if (e.providers !== undefined)   out.providers = e.providers;
    if (e.consumers !== undefined)   out.consumers = e.consumers;
    if (e.provider !== undefined)    out.provider = e.provider;
    if (e.from)                      out.from = e.from;
    if (e.conflict)                  out.conflict = true;
    if (e.generation !== undefined)  out.generation = e.generation;
    if (e.schema_hash !== undefined)          out.schemaHash = e.schema_hash;
    if (e.rsp_schema_hash !== undefined)      out.rspSchemaHash = e.rsp_schema_hash;
    if (e.progress_schema_hash !== undefined) out.progressSchemaHash = e.progress_schema_hash;
    if (e.schema) out.schema = e.schema;
    if (e.rsp)    out.rspSchema = e.rsp;
    if (e.prg)    out.progressSchema = e.prg;
    return out;
}

type Response<Rsp = any> = {
    ok: boolean;
    status: CallStatusName;
    value: Rsp | undefined;   /* decoded reply (typed functions, status "ok") */
    data: Uint8Array;         /* the raw reply payload */
    provider: number;         /* peer id of the answering node (0 if none) */
    writtenUs: number;        /* the provider's source stamp, 0 = synthesized */
    message: string;          /* the outcome text to display on a failure: the definition's message,
                                 else the default status text. "" only on ok with no message */
};

/* default Response.message per status when the definition sent no text (mirrors the C) */
const CALL_STATUS_TEXT: Record<CallStatusName, string> = {
    ok: "", app_error: "app error", no_handler: "no handler",
    timeout: "timeout", peer_lost: "peer lost", cancelled: "cancelled",
};

type RequestInfo = { caller: number; callerName: string; writtenUs: number };

type SubscriberHandler<T> = (value: T, msg: RambleMessage) => void;
type FunctionHandler<Req, Rsp> = (req: Req, info: RequestInfo) => Rsp | Promise<Rsp>;

/* What a task handler receives beside the request: progress() streams an update and signal
 * aborts on a cancel request. The throw rules are in docs/javascript.md. */
type TaskContext<Prg = any> = {
    progress: (value: Prg) => void;
    signal: AbortSignal;
    cancelled: boolean;      /* sugar for signal.aborted */
    caller: number;
    callerName: string;
    writtenUs: number;
};
type TaskHandler<Req, Prg, Rsp> = (req: Req, ctx: TaskContext<Prg>) => Rsp | Promise<Rsp>;

/* one decoded progress update on a task call, a null value = the RUNNING ack */
type TaskProgressHandler<Prg> = (value: Prg | null,
                                 info: { provider: number; writtenUs: number }) => void;

/* the cancel op's verdict: "ok" = requested, "no_cancel" = refused locally, "not_pending"
 * = already answered, "error" = anything else */
type CancelStatus = "ok" | "no_cancel" | "not_pending" | "error";

type FunctionOpts = {
    timeout_ms?: number;             /* remote: the call timeout, 0 = 5 s */
    backpressure_wait_ms?: number;
    keep_last?: number;              /* req and rsp ring depth, 0 = 10 */
    reflect?: boolean;               /* no schemas: take the mesh's for this name */
};

type TaskOpts = {
    reflect?: boolean;               /* no schemas: take the mesh's for this name */
    progress_best_effort?: boolean;  /* progress-channel reliability: omit/false = reliable */
    progress_keep_last?: number;     /* progress ring depth, 0 = the pattern default */
    no_cancel?: boolean;             /* definition: will not honor cancellation */
    exclusive?: boolean;             /* definition: declared serialization (handler enforces) */
    multi?: boolean;                 /* definition: redundant providers intended */
    timeout_ms?: number;             /* remote: bound until the first response, 0 = 5 s */
    backpressure_wait_ms?: number;
    keep_last?: number;   /* req and rsp ring depth, 0 = 10, covering the batch one pass drains */
};

type VariableDefOpts<T> = {
    initial?: T;
    readOnly?: boolean;
    allowForce?: boolean;
    catch_up?: number;
    keep_last?: number;          /* both channels' repair window, 0 = 10 */
    backpressure_wait_ms?: number;
    onWrite?: boolean;   /* also push every applied write (route via VarHandle.onWrite) */
    reflect?: boolean;   /* no schema: take the mesh's for this name */
};

type RemoteVarOpts = {
    onWrite?: boolean;   /* also push every applied write (route via VarHandle.onWrite) */
    reflect?: boolean;   /* no schema: take the mesh's for this name */
};

const SCALAR_BYTES: Record<string, number> = {
    u8: 1, u16: 2, u32: 4, u64: 8, i8: 1, i16: 2, i32: 4, i64: 8, f32: 4, f64: 8, bool: 1,
};
const VARIABLE = new Set(["vstring", "varr", "map"]);

function readScalar(view: DataView, kind: string, off: number): number | bigint | boolean {
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

function writeScalar(view: DataView, kind: string, off: number, v: any): void {
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

/* Read a capped string slot [u16 len][cap bytes] at off. len is clamped to cap like the C
 * reader, so a hostile length never over reads. */
function readCappedString(view: DataView, data: Uint8Array, off: number, cap: number): string {
    const len = Math.min(view.getUint16(off, true), cap);
    return dec.decode(data.subarray(off + 2, off + 2 + len));
}

function writeCappedString(view: DataView, buf: Uint8Array, off: number, cap: number, s: string | undefined): void {
    const b = enc.encode(s ?? "");
    if (b.length > cap) throw new Error(`string ${b.length} > cap ${cap}`);
    view.setUint16(off, b.length, true);
    buf.set(b, off + 2);   /* the rest of the slot stays zero (buf starts zeroed) */
}

/* Locate the ordinal-th variable-field frame [u32 len][payload] in the message tail
 * (which starts at fixedSize). Returns { off, len } into `data`, or null if truncated. */
function varFrame(data: Uint8Array, view: DataView, fixedSize: number, ordinal: number): { off: number; len: number } | null {
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

const MAP_U64 = 3, MAP_I64 = 7, MAP_F64 = 9, MAP_BOOL = 10, MAP_VSTR = 14, MAP_VARR = 15, MAP_MAP = 16;

function readMapValue(data: Uint8Array, view: DataView, off: number): { value: any; off: number } {
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
        const arr: any[] = [];
        for (let i = 0; i < n; i++) { const r = readMapValue(data, view, off); arr.push(r.value); off = r.off; }
        return { value: arr, off };
    }
    case 16: return readMapBody(data, view, off);                              /* nested map */
    default: throw new Error(`bad map value kind ${kind}`);
    }
}

function readMapBody(data: Uint8Array, view: DataView, off: number): { value: any; off: number } {
    const n = view.getUint16(off, true); off += 2;
    const obj: Record<string, any> = {};
    for (let i = 0; i < n; i++) {
        const kl = data[off++];
        const key = dec.decode(data.subarray(off, off + kl)); off += kl;
        const r = readMapValue(data, view, off); obj[key] = r.value; off = r.off;
    }
    return { value: obj, off };
}

function decodeMap(frame: Uint8Array): Record<string, any> {
    if (frame.length === 0) return {};
    const view = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);
    return readMapBody(frame, view, 0).value;
}

/* A little-endian byte sink for building a map body. */
class ByteSink {
    a: number[];
    _dv: DataView;
    constructor() { this.a = []; this._dv = new DataView(new ArrayBuffer(8)); }
    u8(v: number): void { this.a.push(v & 0xff); }
    u16(v: number): void { this.a.push(v & 0xff, (v >> 8) & 0xff); }
    _push(n: number): void { for (let i = 0; i < n; i++) this.a.push(this._dv.getUint8(i)); }
    i64(v: number | bigint): void { this._dv.setBigInt64(0, BigInt(v), true); this._push(8); }
    u64(v: number | bigint): void { this._dv.setBigUint64(0, BigInt(v), true); this._push(8); }
    f64(v: number): void { this._dv.setFloat64(0, v, true); this._push(8); }
    raw(u8: Uint8Array): void { for (const b of u8) this.a.push(b); }
    str(s: string): void { const b = enc.encode(s); this.u16(b.length); this.raw(b); }
    bytes(): Uint8Array { return new Uint8Array(this.a); }
}

/* JS value to map value. bigint keeps its 64 bit width and sign, a plain number encodes
 * as i64 when integral and f64 otherwise, which the C reader widens losslessly. */
function writeMapValue(sink: ByteSink, v: any): void {
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

function writeMapBody(sink: ByteSink, obj: Record<string, any>): void {
    const keys = Object.keys(obj);
    sink.u16(keys.length);
    for (const k of keys) {
        const kb = enc.encode(k);
        if (kb.length > 255) throw new Error(`map key too long: ${k}`);
        sink.u8(kb.length); sink.raw(kb); writeMapValue(sink, obj[k]);
    }
}

function encodeMap(obj: Record<string, any>): Uint8Array {
    const s = new ByteSink(); writeMapBody(s, obj); return s.bytes();
}

/* Decode an arr or varr payload into a JS value: a Uint8Array view for u8 elements, an
 * Array of strings for string elements, else an Array of scalars. */
function decodeArray(f: Field, data: Uint8Array, view: DataView, off: number, len: number): any {
    if (f.elem === "string") {
        const slot = 2 + (f.cap ?? 0), count = Math.floor(len / slot), out = new Array(count);
        for (let i = 0; i < count; i++) out[i] = readCappedString(view, data, off + i * slot, f.cap ?? 0);
        return out;
    }
    if (f.elem === "u8") return data.subarray(off, off + len);
    const n = SCALAR_BYTES[f.elem!], count = Math.floor(len / n), out = new Array(count);
    for (let i = 0; i < count; i++) out[i] = readScalar(view, f.elem!, off + i * n);
    return out;
}

/* an enum value: a number passes through, a string is resolved to its option value */
function enumToValue(f: Field, v: any): number | bigint {
    if (typeof v !== "string") return v;
    const hit = f.variants?.find((o) => o.name === v);
    if (!hit) throw new Error(`'${f.path}': unknown enum option '${v}'`);
    return hit.value;
}

function writeFixedField(view: DataView, buf: Uint8Array, f: Field, v: any): void {
    if (f.kind === "struct") throw new Error(`'${f.path}' is a struct: set its members`);
    if (f.kind === "enum") { writeScalar(view, f.backing!, f.offset, enumToValue(f, v)); return; }
    if (f.kind === "string") { writeCappedString(view, buf, f.offset, f.cap ?? 0, v); return; }
    if (f.kind === "arr" && f.elem === "string") {
        const slot = 2 + (f.cap ?? 0);
        if (v.length > (f.count ?? 0)) throw new Error(`'${f.path}': ${v.length} > ${f.count} strings`);
        for (let i = 0; i < v.length; i++) writeCappedString(view, buf, f.offset + i * slot, f.cap ?? 0, v[i]);
        return;
    }
    if (f.kind === "arr") {
        const elems = typeof v === "string" ? enc.encode(v) : v;
        if (elems.length > (f.count ?? 0)) throw new Error(`'${f.path}': ${elems.length} > ${f.count} elements`);
        const n = SCALAR_BYTES[f.elem!];
        for (let i = 0; i < elems.length; i++) writeScalar(view, f.elem!, f.offset + i * n, elems[i]);
        return;
    }
    writeScalar(view, f.kind, f.offset, v);
}

/* Build the payload frame for one variable field (empty when the value is absent). */
function encodeVarFrame(f: Field, v: any): Uint8Array {
    if (v === undefined || v === null) return new Uint8Array(0);
    if (f.kind === "vstring") return enc.encode(v);
    if (f.kind === "map") return encodeMap(v);
    /* varr */
    if (f.elem === "string") {
        const slot = 2 + (f.cap ?? 0), buf = new Uint8Array(v.length * slot);
        const view = new DataView(buf.buffer);
        for (let i = 0; i < v.length; i++) writeCappedString(view, buf, i * slot, f.cap ?? 0, v[i]);
        return buf;
    }
    if (f.elem === "u8" && v instanceof Uint8Array) return v;
    const elems = typeof v === "string" ? enc.encode(v) : v;
    const n = SCALAR_BYTES[f.elem!], buf = new Uint8Array(elems.length * n);
    const view = new DataView(buf.buffer);
    for (let i = 0; i < elems.length; i++) writeScalar(view, f.elem!, i * n, elems[i]);
    return buf;
}

/* nested plain-object helpers for the full-message codec */
function setPath(obj: Record<string, any>, path: string, v: any): void {
    const segs = path.split(".");
    let o = obj;
    for (let i = 0; i < segs.length - 1; i++) o = o[segs[i]] ?? (o[segs[i]] = {});
    o[segs[segs.length - 1]] = v;
}

function getPath(obj: any, path: string): any {
    let o = obj;
    for (const s of path.split(".")) {
        if (o === undefined || o === null) return undefined;
        o = o[s];
    }
    return o;
}

/* One compiled schema's field tables from a create reply, with the field codec and the
 * whole message encode and decode over plain nested objects. No schema passes bytes through. */
class Layout {
    name: string | undefined;         /* the root type's name ("" when anonymous) */
    size: number | undefined;         /* the fixed section length, where the variable tail begins */
    hash: string | undefined;         /* 64-bit schema identity, hex */
    fields: Map<string, Field>;       /* dotted path to field, in schema order */
    varFields: Field[];               /* variable fields in tail order */
    valueRoot: boolean;               /* a bare type: the whole message is one value */

    constructor(r: SchemaBlock | undefined) {
        this.name = r?.name;
        this.size = r?.size;
        this.hash = r?.hash;
        const list = r?.fields ?? [];
        this.fields = new Map(list.map((f) => [f.path, f]));
        this.varFields = [];
        for (const f of list) if (VARIABLE.has(f.kind)) { f.varOrdinal = this.varFields.length; this.varFields.push(f); }
        /* a BARE TYPE: one anonymous field (empty path), so the message IS one value */
        this.valueRoot = list.length === 1 && list[0].path === "" && list[0].kind !== "struct";
    }

    get typed(): boolean { return this.size !== undefined; }

    /* Every VideoFrame / Image in this type, by dotted path ("" = the message itself):
     * what a VideoView can attach to, wherever it sits. */
    mediaFields(): MediaField[] {
        const out: MediaField[] = [];
        if (this.name && MEDIA_TYPES.has(this.name)) out.push({ path: "", type: this.name as MediaTypeName });
        for (const f of this.fields.values())
            if (f.named && MEDIA_TYPES.has(f.named)) out.push({ path: f.path, type: f.named as MediaTypeName });
        return out;
    }

    /* one field by dotted path (see RambleMessage.get) */
    getField(data: Uint8Array, view: DataView, path: string): any {
        const f = this.fields.get(path);
        if (!f) throw new Error(`no field '${path}'`);
        if (VARIABLE.has(f.kind)) {
            const fr = varFrame(data, view, this.size!, f.varOrdinal!);
            if (!fr) return f.kind === "varr" ? [] : (f.kind === "map" ? {} : "");
            const frame = data.subarray(fr.off, fr.off + fr.len);
            if (f.kind === "vstring") return dec.decode(frame);
            if (f.kind === "map") return decodeMap(frame);
            return decodeArray(f, frame, new DataView(frame.buffer, frame.byteOffset, frame.byteLength), 0, frame.length);
        }
        if (f.kind === "struct") return data.subarray(f.offset, f.offset + f.size);
        if (f.kind === "enum") return readScalar(view, f.backing!, f.offset);   /* the number */
        if (f.kind === "string") return readCappedString(view, data, f.offset, f.cap ?? 0);
        if (f.kind === "arr") return decodeArray(f, data, view, f.offset, f.size);
        return readScalar(view, f.kind, f.offset);
    }

    /* enum option helpers (by field path): resolve a wire number to its option name (""
     * if none, i.e. an unknown/newer value) and a name to its number (undefined if none). */
    enumName(path: string, value: number | bigint): string {
        const f = this.fields.get(path);
        const hit = f?.variants?.find((o) => BigInt(o.value) === BigInt(value));
        return hit ? hit.name : "";
    }
    enumValue(path: string, name: string): number | undefined {
        return this.fields.get(path)?.variants?.find((o) => o.name === name)?.value;
    }

    /* Full-message decode into a plain nested object (structs become sub-objects), or the
     * bare value for a bare-type schema. An untyped layout returns the raw bytes unchanged. */
    decode(data: Uint8Array): any {
        if (!this.typed) return data;
        const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
        if (this.valueRoot) return this.getField(data, view, "");
        const out: Record<string, any> = {};
        for (const f of this.fields.values()) {
            if (f.kind === "struct") continue;   /* members fill it via their paths */
            setPath(out, f.path, this.getField(data, view, f.path));
        }
        return out;
    }

    /* Whole message encode from a plain nested object, missing fixed fields zero and
     * variable ones empty, or from the bare value of a bare type schema. Untyped takes bytes. */
    encode(value: any): Uint8Array {
        if (!this.typed) {
            if (value === undefined || value === null) return new Uint8Array(0);
            if (value instanceof Uint8Array) return value;
            throw new Error("raw entity: pass a Uint8Array");
        }
        if (this.valueRoot) value = { "": value };   /* the value IS the one anonymous field */
        const fixed = new Uint8Array(this.size!);
        const view = new DataView(fixed.buffer);
        for (const f of this.fields.values()) {
            if (f.kind === "struct" || VARIABLE.has(f.kind)) continue;
            const v = getPath(value, f.path);
            if (v === undefined) continue;
            writeFixedField(view, fixed, f, v);
        }
        const frames = this.varFields.map((f) => encodeVarFrame(f, getPath(value, f.path)));
        let total = this.size!;
        for (const fr of frames) total += 4 + fr.length;
        const buf = new Uint8Array(total);
        buf.set(fixed, 0);
        const dv = new DataView(buf.buffer);
        let pos = this.size!;
        for (const fr of frames) { dv.setUint32(pos, fr.length, true); pos += 4; buf.set(fr, pos); pos += fr.length; }
        return buf;
    }
}

/* A delivered message: raw bytes plus typed reads through the entity's field table. */
class RambleMessage {
    topic: RambleTopic | null;   /* the receiving topic for topic deliveries, null elsewhere */
    publisher: number;           /* peer id of the sending node */
    data: Uint8Array;            /* the payload, verbatim */
    writtenUs: number;           /* the publisher's source stamp in UTC us, 0 = opted out */
    captureUs: number;           /* when the data was true, UTC us, 0 = the publisher gave none */
    _layout: Layout;
    _view: DataView;

    constructor(layout: Layout, topic: RambleTopic | null, publisher: number, data: Uint8Array,
                writtenUs: number = 0, captureUs: number = 0) {
        this._layout = layout;
        this.topic = topic;
        this.publisher = publisher;
        this.data = data;
        this.writtenUs = writtenUs;
        this.captureUs = captureUs;
        this._view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    }

    /* Typed read of one field by dotted path. The JS types per kind are in docs/javascript.md. */
    get(path: string): any { return this._layout.getField(this.data, this._view, path); }

    /* The whole message as a plain nested object, the raw bytes when untyped. Decoded once. */
    value(): any { return this._value ?? (this._value = this._layout.decode(this.data)); }
    _value: any;

    /* A u8 array field decoded as UTF-8 text with trailing NULs stripped, or up to lenField's
     * value. Prefer a string field, which get returns directly. */
    text(path: string, lenField?: string): string {
        const bytes = this.get(path) as Uint8Array;
        let n = lenField !== undefined ? Number(this.get(lenField)) : bytes.length;
        if (lenField === undefined) while (n > 0 && bytes[n - 1] === 0) n--;
        return dec.decode(bytes.subarray(0, Math.min(n, bytes.length)));
    }
}

/* Everything created on a node: one client-chosen id (also its WebRTC data channel id),
 * one match summary from the bridge's pushes, one frame inbox. */
class RambleEntity {
    _node: RambleNode;
    id: number;
    name: string;
    reliable: boolean;         /* the topic's QoS, patterns are reliable */
    reflected: boolean;        /* created with reflect, the types came from the mesh */
    matchCount: number;        /* matched remote endpoints (from match pushes) */
    ready: boolean;            /* a send/call now would not wait on a forming match */
    _dc: RTCDataChannel | null;
    _taps: ((value: any) => void)[];   /* VideoViews attached to this entity's stream */

    constructor(node: RambleNode, name: string, r: any, dc: RTCDataChannel | null) {
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
    async refresh(): Promise<boolean> {
        const r = await this._node._request({ op: "refresh", id: this.id });
        this.reflected = !!r.reflected;
        this._retype(r);
        return !!r.retyped;
    }

    _match(m: any): void { this.matchCount = m.count; this.ready = !!m.ready; }
    _frame(_f: Frame): void {}
    _retype(_r: any): void {}
    _tap(value: any): void { for (const t of this._taps) t(value); }
    _send(op: number, flags: number, seq: number, text: string, payload: Uint8Array,
          captureUs = 0): void {
        this._node._sendFrame(this, buildFrame(op, flags, this.id, seq, text, payload, captureUs));
    }
}

/* One topic on the node (the dynamic form). Returned by RambleNode.topic(). */
class RambleTopic extends RambleEntity {
    layout: Layout;
    onMessage: ((msg: RambleMessage) => void) | null;

    constructor(node: RambleNode, name: string, r: any, dc: RTCDataChannel | null) {
        super(node, name, r, dc);
        this.layout = new Layout(r.schema);
        this.onMessage = null;
    }

    _retype(r: any): void { this.layout = new Layout(r.schema); }

    /* fixed-section size / schema hash / field table (typed topics) */
    get size(): number | undefined { return this.layout.size; }
    get hash(): string | undefined { return this.layout.hash; }
    get fields(): Map<string, Field> { return this.layout.fields; }

    /* Publish raw bytes. captureUs is when the data was true, 0 = unstated. */
    sendRaw(bytes: Uint8Array, captureUs = 0): void {
        this._send(OP_DATA, 0, 0, "", bytes, captureUs);
    }

    /* Typed publish from a plain nested object mirroring the schema. A bare type topic takes
     * the value itself. */
    send(value: any, captureUs = 0): void {
        this.sendRaw(this.layout.encode(value), captureUs);
    }

    /* Flip this topic's role: "pubsub" | "pub" | "sub" | "inactive". */
    setRole(role: Role): Promise<any> { return this._node._request({ op: "role", id: this.id, role }); }

    /* Wait until every reader acked everything (reliable topics, before close). */
    async drain(timeout_ms: number = 1000): Promise<boolean> {
        const r = await this._node._request({ op: "drain", id: this.id, timeout_ms });
        return r.drained as boolean;
    }

    _frame(f: Frame): void {
        if (f.op !== OP_DATA) return;
        const msg = new RambleMessage(this.layout, this, f.peer, f.payload, f.writtenUs, f.captureUs);
        this.onMessage?.(msg);
        if (this._taps.length) this._tap(msg.value());
    }
}

/* The publish side handle over a topic, speaking plain nested objects. */
class Publisher<T = any> {
    topic: RambleTopic;

    constructor(topic: RambleTopic) { this.topic = topic; }

    send(value: T, captureUs = 0): void { this.topic.send(value, captureUs); }
    sendRaw(bytes: Uint8Array, captureUs = 0): void { this.topic.sendRaw(bytes, captureUs); }
    get matchCount(): number { return this.topic.matchCount; }
    get ready(): boolean { return this.topic.ready; }
}

/* Subscribe-side handle: the handler gets (decoded plain object, message). */
class Subscriber<T = any> {
    topic: RambleTopic;

    constructor(topic: RambleTopic, handler: SubscriberHandler<T>) {
        this.topic = topic;
        this.topic.onMessage = (msg) => handler(msg.value() as T, msg);
    }

    get matchCount(): number { return this.topic.matchCount; }
}

/* the RESULT frame a definition answers a request with */
function replyFrame(entity: RambleEntity, reqId: number, status: number, err: any, rsp: Uint8Array): void {
    const t = err === undefined ? "" : typeof err?.message === "string" ? err.message : (typeof err === "string" ? err : "");
    entity._send(OP_RESULT, status, reqId, t, rsp);
}

/* The implementation side of a function: the bridge defers every request here, the
 * handler's return value is the reply and a throw answers "app_error". One per name. */
class FunctionDefinition<Req = any, Rsp = any> extends RambleEntity {
    reqLayout: Layout;
    rspLayout: Layout;
    _handler: FunctionHandler<Req, Rsp>;

    constructor(node: RambleNode, name: string, r: any, dc: RTCDataChannel | null, handler: FunctionHandler<Req, Rsp>) {
        super(node, name, r, dc);
        this.reqLayout = new Layout(r.req);
        this.rspLayout = new Layout(r.rsp);
        this._handler = handler;
    }

    get callerCount(): number { return this.matchCount; }   /* callers currently matched */

    _retype(r: any): void { this.reqLayout = new Layout(r.req); this.rspLayout = new Layout(r.rsp); }

    _frame(f: Frame): void {
        if (f.op === OP_CALL) void this._handle(f.seq, { caller: f.peer, callerName: f.text, writtenUs: f.writtenUs }, f.payload);
    }

    async _handle(reqId: number, info: RequestInfo, payload: Uint8Array): Promise<void> {
        try {
            const out = await this._handler(this.reqLayout.decode(payload) as Req, info);
            replyFrame(this, reqId, 0, undefined, this.rspLayout.encode(out));
        } catch (e: any) {
            replyFrame(this, reqId, 1, e, new Uint8Array(0));   /* app_error + the throw's text */
        }
    }
}

type PendingCall<Rsp> = {
    resolve: (r: Response<Rsp>) => void;
    reject: (e: Error) => void;
    layout: Layout;
    timer: ReturnType<typeof setTimeout> | undefined;
    /* task calls only: routes PROGRESS frames to the run handle */
    progress?: (data: Uint8Array, provider: number, writtenUs: number) => void;
};

/* A reference to a function definition on another node. call() resolves with the
 * outcome and NEVER rejects on a status (only on connection loss). */
class RemoteFunction<Req = any, Rsp = any> extends RambleEntity {
    reqLayout: Layout;
    rspLayout: Layout;

    constructor(node: RambleNode, name: string, r: any, dc: RTCDataChannel | null) {
        super(node, name, r, dc);
        this.reqLayout = new Layout(r.req);
        this.rspLayout = new Layout(r.rsp);
    }

    get hasDefinition(): boolean { return this.matchCount > 0; }   /* a definition is matched */

    _retype(r: any): void { this.reqLayout = new Layout(r.req); this.rspLayout = new Layout(r.rsp); }

    /* Call the remote function. timeoutMs > 0 adds a client side bound resolving "timeout".
     * The bridge's own timeout still answers with a wire status when it fires first. */
    call(value: Req, timeoutMs: number = 0): Promise<Response<Rsp>> {
        const payload = this.reqLayout.encode(value);
        const callId = ++this._node._nextCall;
        return new Promise<Response<Rsp>>((resolve, reject) => {
            const p: PendingCall<Rsp> = { resolve, reject, layout: this.rspLayout, timer: undefined };
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
class TaskDefinition<Req = any, Prg = any, Rsp = any> extends RambleEntity {
    reqLayout: Layout;
    prgLayout: Layout;
    rspLayout: Layout;
    _handler: TaskHandler<Req, Prg, Rsp>;
    _aborts: Map<number, AbortController>;   /* req id to its cancel signal */

    constructor(node: RambleNode, name: string, r: any, dc: RTCDataChannel | null, handler: TaskHandler<Req, Prg, Rsp>) {
        super(node, name, r, dc);
        this.reqLayout = new Layout(r.req);
        this.prgLayout = new Layout(r.prg);
        this.rspLayout = new Layout(r.rsp);
        this._handler = handler;
        this._aborts = new Map();
    }

    get callerCount(): number { return this.matchCount; }

    _retype(r: any): void {
        this.reqLayout = new Layout(r.req); this.prgLayout = new Layout(r.prg); this.rspLayout = new Layout(r.rsp);
    }

    _frame(f: Frame): void {
        if (f.op === OP_CALL) void this._handle(f.seq, { caller: f.peer, callerName: f.text, writtenUs: f.writtenUs }, f.payload);
        else if (f.op === OP_CANCEL) this._aborts.get(f.seq)?.abort();   /* the default reason */
    }

    async _handle(reqId: number, info: RequestInfo, payload: Uint8Array): Promise<void> {
        const ctrl = new AbortController();
        this._aborts.set(reqId, ctrl);
        let done = false;
        const prg = this.prgLayout;
        const ctx: TaskContext<Prg> = {
            progress: (v: Prg) => {
                if (done) throw new Error("task request already completed");
                this._send(OP_PROGRESS, 0, reqId, "", prg.encode(v));
            },
            signal: ctrl.signal,
            get cancelled(): boolean { return ctrl.signal.aborted; },
            caller: info.caller,
            callerName: info.callerName,
            writtenUs: info.writtenUs,
        };
        try {
            const out = await this._handler(this.reqLayout.decode(payload) as Req, ctx);
            done = true;
            this._aborts.delete(reqId);
            replyFrame(this, reqId, 0, undefined, this.rspLayout.encode(out));
        } catch (e: any) {
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
class TaskRun<Prg = any, Rsp = any> {
    _node: RambleNode;
    _prgLayout: Layout;
    id: number;                /* the remote task's entity id */
    callId: number;            /* the client correlation id (the cancel op's `call`) */
    result: Promise<Response<Rsp>>;
    _onProgress: TaskProgressHandler<Prg> | null;
    _buffered: { value: Prg | null; info: { provider: number; writtenUs: number } }[];
    _taps: ((value: any) => void)[];   /* VideoViews attached to this call's progress */

    constructor(node: RambleNode, id: number, prgLayout: Layout, callId: number, result: Promise<Response<Rsp>>) {
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
    onProgress(handler: TaskProgressHandler<Prg> | null): this {
        this._onProgress = handler;
        if (handler) { const b = this._buffered; this._buffered = []; for (const u of b) handler(u.value, u.info); }
        return this;
    }

    /* Request cancellation. Cooperative and never acked on the wire: resolves with the
     * local verdict, and the terminal result's status is the real answer. */
    async cancel(): Promise<CancelStatus> {
        const r = await this._node._request({ op: "cancel", call: this.callId });
        return r.status as CancelStatus;
    }

    _push(data: Uint8Array, provider: number, writtenUs: number): void {
        const value = data.length ? (this._prgLayout.decode(data) as Prg) : null;
        if (this._onProgress) this._onProgress(value, { provider, writtenUs });
        else this._buffered.push({ value, info: { provider, writtenUs } });
        if (value !== null) for (const t of this._taps) t(value);
    }
}

/* A reference to a task defined elsewhere. call() returns a TaskRun synchronously and the
 * timeout bounds only the first response, so there is no client side timer. */
class RemoteTask<Req = any, Prg = any, Rsp = any> extends RambleEntity {
    reqLayout: Layout;
    prgLayout: Layout;
    rspLayout: Layout;

    constructor(node: RambleNode, name: string, r: any, dc: RTCDataChannel | null) {
        super(node, name, r, dc);
        this.reqLayout = new Layout(r.req);
        this.prgLayout = new Layout(r.prg);
        this.rspLayout = new Layout(r.rsp);
    }

    get hasDefinition(): boolean { return this.matchCount > 0; }

    _retype(r: any): void {
        this.reqLayout = new Layout(r.req); this.prgLayout = new Layout(r.prg); this.rspLayout = new Layout(r.rsp);
    }

    call(value: Req): TaskRun<Prg, Rsp> {
        const payload = this.reqLayout.encode(value);
        const callId = ++this._node._nextCall;
        let run!: TaskRun<Prg, Rsp>;
        const result = new Promise<Response<Rsp>>((resolve, reject) => {
            const p: PendingCall<Rsp> = { resolve, reject, layout: this.rspLayout, timer: undefined,
                progress: (data, provider, writtenUs) => run._push(data, provider, writtenUs) };
            this._node._calls.set(callId, p);
        });
        run = new TaskRun<Prg, Rsp>(this._node, this.id, this.prgLayout, callId, result);
        this._send(OP_CALL, 0, callId, "", payload);
        return run;
    }
}

type VarWaiter = { res: (ok: boolean) => void; timer: ReturnType<typeof setTimeout> | undefined };
type VarChangeHandler<T> = (value: T, info: { forced: boolean; writtenUs: number; source: number }) => void;

/* Shared variable-handle core: the client-cached latest value fed by pushed updates. */
class VarHandle<T = any> extends RambleEntity {
    layout: Layout;
    forced: boolean;
    writtenUs: number;            /* source stamp of the last update pushed (0 = none/unstamped) */
    _value: T | undefined;
    _raw: Uint8Array | undefined;
    _waiters: Set<VarWaiter>;
    _onChange: VarChangeHandler<T> | null;
    _onWrite: VarChangeHandler<T> | null;

    constructor(node: RambleNode, name: string, r: any, dc: RTCDataChannel | null) {
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
    get(): T | undefined { return this._value; }
    /* the cached latest value's raw bytes */
    getRaw(): Uint8Array | undefined { return this._raw; }

    /* Resolve true once a value is cached (immediately if one already is), false on
     * timeout. timeoutMs < 0 waits indefinitely. */
    wait(timeoutMs: number = -1): Promise<boolean> {
        if (this._value !== undefined) return Promise.resolve(true);
        return new Promise((res) => {
            const w: VarWaiter = { res, timer: undefined };
            if (timeoutMs >= 0) w.timer = setTimeout(() => { this._waiters.delete(w); res(false); }, timeoutMs);
            this._waiters.add(w);
        });
    }

    /* Observe changes: fires per pushed update, and once at registration when a value is
     * cached, so a late registration never misses the state. One handler, null clears. */
    onChange(handler: VarChangeHandler<T> | null): void {
        this._onChange = handler;
        if (handler && this._value !== undefined)
            handler(this._value, { forced: this.forced, writtenUs: this.writtenUs, source: 0 });
    }

    /* Observe every applied write with no replay. Needs onWrite: true at create so the bridge
     * pushes them. One handler, null clears. */
    onWrite(handler: VarChangeHandler<T> | null): void { this._onWrite = handler; }

    set(value: T): void { this._send(OP_VAR, 0, 0, "", this.layout.encode(value)); }
    force(value: T): void { this._send(OP_VAR, 1, 0, "", this.layout.encode(value)); }
    unforce(): void { this._send(OP_VAR, 2, 0, "", new Uint8Array(0)); }

    _retype(r: any): void { this.layout = new Layout(r.schema); }

    _frame(f: Frame): void {
        if (f.op !== OP_VAR) return;
        const forced = (f.flags & 1) !== 0;
        const info = { forced, writtenUs: f.writtenUs, source: f.peer };
        if (f.flags & 2) {                   /* a write event: no cache change */
            this._onWrite?.(this.layout.decode(f.payload) as T, info);
            return;
        }
        this._raw = f.payload;
        this._value = this.layout.decode(f.payload) as T;
        this.forced = forced;
        this.writtenUs = f.writtenUs;
        for (const w of this._waiters) { if (w.timer !== undefined) clearTimeout(w.timer); w.res(true); }
        this._waiters.clear();
        this._onChange?.(this._value, info);
        if (this._taps.length) this._tap(this._value);
    }
}

/* The owner side: this node holds the authoritative value. */
class VariableDefinition<T = any> extends VarHandle<T> {
    get remoteCount(): number { return this.matchCount; }   /* remotes currently matched */
}

/* A reference to a variable owned elsewhere. set() round trips through the owner. */
class RemoteVariable<T = any> extends VarHandle<T> {
    get hasDefinition(): boolean { return this.matchCount > 0; }
}

/* ---- media -------------------------------------------------------------------------- */

/* the standard media types (docs/stdtypes.md), always in scope by name */
const VIDEO_FRAME = "VideoFrame";   /* { codec, width, height, keyframe, pts, data } */
const IMAGE = "Image";              /* { width, height, stride, format, data } */

const EXTERNAL_VIDEO_STREAM = "ExternalVideoStream";   /* kind, codec, width, height, url, name */

const VideoCodec = { Unknown: 0, Mjpeg: 1, H264: 2, H265: 3, Av1: 4 } as const;
const ImageFormat = { Mono8: 0, Mono16: 1, Rgb8: 2, Rgba8: 3, Bgr8: 4, Yuyv: 5, Nv12: 6, Jpeg: 16, Png: 17 } as const;
const StreamKind = { Rtsp: 0, WebrtcWhep: 1, Hls: 2, Srt: 3, Rtp: 4, HttpMjpeg: 5, Other: 15 } as const;

/* a decoded VideoFrame message (enum as its number, pts as bigint microseconds) */
type VideoFrameValue = { codec: number; width: number; height: number; keyframe: boolean; pts: bigint; data: Uint8Array };
/* a decoded Image message */
type ImageValue = { width: number; height: number; stride: number; format: number; data: Uint8Array };
/* a decoded ExternalVideoStream descriptor: a URL a viewer connects to itself */
type ExternalVideoStreamValue = { kind: number; codec: number; width: number; height: number; url: string; name: string };
type MediaValue = VideoFrameValue | ImageValue | ExternalVideoStreamValue;

type VideoPath = "none" | "track" | "decoder";

/* what a VideoView attaches to: an entity whose stream carries a VideoFrame or Image
 * somewhere in its type. A Subscriber stands for its topic */
type MediaSource = RambleTopic | Subscriber | VarHandle | TaskRun;
type MediaAttachOpts = {
    path?: string;       /* the VideoFrame or Image field, dotted. "" = the value itself */
    keepData?: boolean;  /* keep the pixels in the frames too while a WebRTC track carries them */
};

/* the WebCodecs codec string for an encoded VideoFrame. H264 reads its SPS so the decoder
 * gets the real profile and level */
function codecString(codec: number, data: Uint8Array): string {
    if (codec === VideoCodec.H264) {
        const n = data.length;
        for (let i = 0; i + 4 < n; i++) {
            if (data[i] === 0 && data[i + 1] === 0 && (data[i + 2] === 1 || (data[i + 2] === 0 && data[i + 3] === 1))) {
                const h = i + (data[i + 2] === 1 ? 3 : 4);
                if (h + 3 < n && (data[h] & 0x1f) === 7)
                    return "avc1." + [data[h + 1], data[h + 2], data[h + 3]].map((b) => b.toString(16).padStart(2, "0")).join("");
            }
        }
        if (n > 8 && (data[4] & 0x1f) === 7)   /* length-prefixed, SPS first */
            return "avc1." + [data[5], data[6], data[7]].map((b) => b.toString(16).padStart(2, "0")).join("");
        return "avc1.42e01e";
    }
    if (codec === VideoCodec.H265) return "hev1.1.6.L120.B0";
    return "av01.0.08M.08";
}

/* raw pixels to RGBA, for the formats a canvas cannot take directly */
function toRgba(img: ImageValue): Uint8ClampedArray | null {
    const { width: w, height: h, format } = img;
    const src = img.data;
    const out = new Uint8ClampedArray(w * h * 4);
    const stride = img.stride || (format === ImageFormat.Mono16 ? w * 2 : format === ImageFormat.Mono8 ? w
                 : format === ImageFormat.Rgb8 || format === ImageFormat.Bgr8 ? w * 3
                 : format === ImageFormat.Rgba8 ? w * 4 : format === ImageFormat.Yuyv ? w * 2 : w);
    const yuv = (y: number, u: number, v: number, o: number) => {
        const c = y - 16, d = u - 128, e = v - 128;
        out[o]     = (298 * c + 409 * e + 128) >> 8;
        out[o + 1] = (298 * c - 100 * d - 208 * e + 128) >> 8;
        out[o + 2] = (298 * c + 516 * d + 128) >> 8;
        out[o + 3] = 255;
    };
    switch (format) {
    case ImageFormat.Rgba8:
        for (let y = 0; y < h; y++) out.set(src.subarray(y * stride, y * stride + w * 4), y * w * 4);
        return out;
    case ImageFormat.Rgb8: case ImageFormat.Bgr8: {
        const swap = format === ImageFormat.Bgr8;
        for (let y = 0; y < h; y++) for (let x = 0, s = y * stride, o = y * w * 4; x < w; x++, s += 3, o += 4) {
            out[o] = src[swap ? s + 2 : s]; out[o + 1] = src[s + 1]; out[o + 2] = src[swap ? s : s + 2]; out[o + 3] = 255;
        }
        return out;
    }
    case ImageFormat.Mono8:
        for (let y = 0; y < h; y++) for (let x = 0, s = y * stride, o = y * w * 4; x < w; x++, s++, o += 4) {
            out[o] = out[o + 1] = out[o + 2] = src[s]; out[o + 3] = 255;
        }
        return out;
    case ImageFormat.Mono16:
        for (let y = 0; y < h; y++) for (let x = 0, s = y * stride, o = y * w * 4; x < w; x++, s += 2, o += 4) {
            out[o] = out[o + 1] = out[o + 2] = src[s + 1]; out[o + 3] = 255;   /* the high byte */
        }
        return out;
    case ImageFormat.Yuyv:
        for (let y = 0; y < h; y++) for (let x = 0, s = y * stride, o = y * w * 4; x + 1 < w; x += 2, s += 4, o += 8) {
            yuv(src[s], src[s + 1], src[s + 3], o); yuv(src[s + 2], src[s + 1], src[s + 3], o + 4);
        }
        return out;
    case ImageFormat.Nv12: {
        const uvOff = stride * h;
        for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
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
    _node: RambleNode;
    stream: MediaStream | null;      /* the picture (null outside a browser) */
    canvas: HTMLCanvasElement | null;   /* the decode surface, null while a track carries it */
    path: VideoPath;                 /* "track" (WebRTC video track) or "decoder" (painted here) */
    frames: number;                  /* frames painted here (the track path counts nothing) */
    width: number;
    height: number;
    onFrame: ((value: MediaValue) => void) | null;   /* every value pushed */
    source: RambleTopic | VarHandle | TaskRun | null;   /* what attach() bound */
    sourcePath: string;              /* the media field inside its type */
    sourceType: MediaTypeName | "";  /* what that field is */
    trackState: string;              /* "" (no track asked), "ok", or the bridge's refusal */
    decodeState: string;             /* "" or why encoded frames cannot decode here */
    external: ExternalVideoStreamValue | null;   /* the descriptor the view follows, if any */
    externalState: string;           /* "" / "connecting" / "playing" / an error, for `external` */
    _extPc: RTCPeerConnection | null;            /* WHEP session */
    _extResource: string;                        /* WHEP resource URL (DELETE on close) */
    _extImg: HTMLImageElement | null;            /* HTTP MJPEG */
    _extVideo: HTMLVideoElement | null;          /* HLS (native playback) */
    _extTimer: number;                           /* the paint loop of the two above */
    _tap: ((value: any) => void) | null;
    _transceiver: RTCRtpTransceiver | null;   /* the offered recvonly video line */
    _ctx: CanvasRenderingContext2D | null;
    _canvasTrack: MediaStreamTrack | null;
    _rtcTrack: MediaStreamTrack | null;
    _decoder: any;
    _decoderCodec: string;
    _pendingBlob: { bytes: Uint8Array; mime: string } | null;
    _blobBusy: boolean;
    _closed: boolean;

    constructor(node: RambleNode) {
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
    async attach(target: MediaSource, opts: MediaAttachOpts | string = {}): Promise<this> {
        if (typeof opts === "string") opts = { path: opts };
        this.detach();
        const src = target instanceof Subscriber ? target.topic : target;
        const layout = src instanceof TaskRun ? src._prgLayout : src.layout;
        const fields = layout.mediaFields();
        const field = opts.path === undefined ? fields[0] : fields.find((f) => f.path === opts.path);
        const path = opts.path ?? field?.path ?? "";
        this.source = src;
        this.sourcePath = path;
        this.sourceType = field?.type ?? "";
        this._tap = (value: any) => {
            const v = path ? getPath(value, path) : value;
            if (!v) return;
            if (v.url !== undefined) this.push(v);                /* a descriptor: follow it */
            else if (v.data && v.data.length) this.push(v);   /* empty: the track has the pixels */
        };
        src._taps.push(this._tap);
        this._keepData = !!opts.keepData;
        if (this._node._rtcUp) {
            this._node._addVideoLine(this);
            if (this._transceiver) await this._node._rtcOffer();
        }
        return this;
    }
    _keepData: boolean = false;

    /* Unbind from the source (the view keeps painting whatever is pushed). */
    detach(): void {
        const src = this.source;
        if (src && this._tap) src._taps = src._taps.filter((t) => t !== this._tap);
        this.source = null;
        this.sourcePath = "";
        this._tap = null;
        this.trackState = "";
        if (this._transceiver) {
            try { this._transceiver.stop(); } catch (_e) { /* not supported everywhere */ }
            this._transceiver = null;
            this._detachTrack();
            if (this._node._rtcUp) void this._node._rtcOffer();   /* the line is gone, re offer */
        }
    }

    /* Detach, stop every track, close the decoder, leave any external stream. */
    close(): void {
        this.detach();
        this._stopExternal();
        this._closed = true;
        this._decoder?.close?.();
        this._decoder = null;
        this._canvasTrack?.stop();
        if (this.stream) for (const t of this.stream.getTracks()) this.stream.removeTrack(t);
        this._node._views.delete(this);
    }

    /* Show one value: a VideoFrame (by its codec), an Image (by its format), or an
     * ExternalVideoStream (the view connects to its URL). */
    push(value: MediaValue): void {
        if (this._closed) return;
        this.onFrame?.(value);
        if (typeof document === "undefined") { this.frames++; return; }   /* no canvas here */
        if ("url" in value) { this._follow(value); return; }
        if ("codec" in value) {
            if (value.codec === VideoCodec.Mjpeg) this._paintBlob(value.data, "image/jpeg");
            else if (value.codec >= VideoCodec.H264) this._decode(value);
        } else if ("format" in value) {
            if (value.format === ImageFormat.Jpeg) this._paintBlob(value.data, "image/jpeg");
            else if (value.format === ImageFormat.Png) this._paintBlob(value.data, "image/png");
            else {
                const rgba = toRgba(value);
                if (rgba && this._surface(value.width, value.height)) {
                    this._ctx!.putImageData(new ImageData(rgba, value.width, value.height), 0, 0);
                    this._painted();
                }
            }
        }
    }

    /* what the offer's `tracks` entry says about this view's line (VideoFrame fields only) */
    _trackReq(): { id: number; path: string; call: number; keep_data: boolean } | null {
        const s = this.source;
        if (!s || this.sourceType !== "VideoFrame") return null;
        return { id: s.id, path: this.sourcePath, call: s instanceof TaskRun ? s.callId : 0, keep_data: this._keepData };
    }

    /* an ExternalVideoStream descriptor: connect to it by kind (a same-URL repeat is a no-op) */
    _follow(desc: ExternalVideoStreamValue): void {
        if (this.external && this.external.url === desc.url && this.external.kind === desc.kind) return;
        this._stopExternal();
        this.external = desc;
        this.externalState = "connecting";
        if (desc.kind === StreamKind.WebrtcWhep) void this._whep(desc.url);
        else if (desc.kind === StreamKind.HttpMjpeg) this._paintElement(Object.assign(new Image(), { src: desc.url, crossOrigin: "anonymous" }));
        else if (desc.kind === StreamKind.Hls) {
            const v = document.createElement("video");
            v.muted = true; v.playsInline = true; v.crossOrigin = "anonymous"; v.src = desc.url;
            v.onerror = () => { this.externalState = "this browser cannot play HLS natively"; };
            void v.play().catch(() => {});
            this._paintElement(v);
        }
        else this.externalState = `cannot play kind ${desc.kind} in a browser`;
    }

    _stopExternal(): void {
        if (this._extPc) {
            try { this._extPc.close(); } catch (_e) { /* already closed */ }
            if (this._extResource) fetch(this._extResource, { method: "DELETE" }).catch(() => {});
            this._extPc = null; this._extResource = "";
        }
        if (this._extTimer) { cancelAnimationFrame(this._extTimer); this._extTimer = 0; }
        if (this._extImg) { this._extImg.src = ""; this._extImg = null; }
        if (this._extVideo) { this._extVideo.pause(); this._extVideo.src = ""; this._extVideo = null; }
        this._detachTrack();
        this.external = null;
        this.externalState = "";
    }

    /* WHEP (RFC draft-ietf-wish-whep): POST our offer, get the answer, receive the track */
    async _whep(url: string): Promise<void> {
        const pc = new RTCPeerConnection();
        this._extPc = pc;
        pc.addTransceiver("video", { direction: "recvonly" });
        pc.ontrack = (e) => { if (this._extPc === pc) { this._attachTrack(e.track); this.externalState = "playing"; } };
        try {
            const offer = await pc.createOffer();
            await pc.setLocalDescription(offer);
            await new Promise<void>((res) => {   /* gather, bounded: WHEP wants a complete offer */
                if (pc.iceGatheringState === "complete") return res();
                const t = setTimeout(res, 1500);
                pc.onicegatheringstatechange = () => { if (pc.iceGatheringState === "complete") { clearTimeout(t); res(); } };
            });
            const rsp = await fetch(url, { method: "POST", headers: { "content-type": "application/sdp" },
                                           body: pc.localDescription?.sdp ?? offer.sdp });
            if (!rsp.ok) throw new Error(`WHEP ${rsp.status}`);
            const loc = rsp.headers.get("location");
            if (loc) this._extResource = new URL(loc, url).toString();
            await pc.setRemoteDescription({ type: "answer", sdp: await rsp.text() });
        } catch (e: any) {
            if (this._extPc === pc) { this.externalState = `WHEP failed: ${e?.message ?? e}`; pc.close(); this._extPc = null; }
        }
    }

    /* paint a live <img> (MJPEG) or <video> (HLS) onto the canvas every frame */
    _paintElement(el: HTMLImageElement | HTMLVideoElement): void {
        if (el instanceof HTMLVideoElement) this._extVideo = el; else this._extImg = el;
        const tick = () => {
            if (this._closed || (this._extImg !== el && this._extVideo !== el)) return;
            const w = el instanceof HTMLVideoElement ? el.videoWidth : el.naturalWidth;
            const h = el instanceof HTMLVideoElement ? el.videoHeight : el.naturalHeight;
            if (w && h && this._surface(w, h)) {
                try { this._ctx!.drawImage(el, 0, 0); this._painted(); this.externalState = "playing"; }
                catch (_e) { this.externalState = "cross-origin: the stream cannot be painted"; }
            }
            this._extTimer = requestAnimationFrame(tick);
        };
        this._extTimer = requestAnimationFrame(tick);
    }

    /* The WebRTC video track bound to this source. It shows once RTP flows, and whenever pixels
     * arrive on the frame path instead the canvas shows. Exactly one of the two at a time. */
    _attachTrack(track: MediaStreamTrack): void {
        if (this._closed || !this.stream) return;
        this._rtcTrack = track;
        track.onended = () => { if (this._rtcTrack === track) this._detachTrack(); };
        track.onunmute = () => { if (this._rtcTrack === track) this._showTrack(); };
        if (!track.muted) this._showTrack();
    }

    _showTrack(): void {
        const t = this._rtcTrack;
        if (!t || !this.stream) return;
        if (this._canvasTrack && this.stream.getTracks().includes(this._canvasTrack)) this.stream.removeTrack(this._canvasTrack);
        if (!this.stream.getTracks().includes(t)) this.stream.addTrack(t);
        this.path = "track";
    }

    _showCanvas(): void {
        const ct = this._canvasTrack;
        if (!ct || !this.stream) return;
        if (this._rtcTrack && this.stream.getTracks().includes(this._rtcTrack)) this.stream.removeTrack(this._rtcTrack);
        if (!this.stream.getTracks().includes(ct)) this.stream.addTrack(ct);
        this.path = "decoder";
    }

    /* the track went away (line refused, link lost): the canvas shows what is painted */
    _detachTrack(): void {
        const track = this._rtcTrack;
        if (!track) return;
        this._rtcTrack = null;
        if (this.stream && this.stream.getTracks().includes(track)) this.stream.removeTrack(track);
        this.path = this._canvasTrack && this.stream?.getTracks().includes(this._canvasTrack) ? "decoder" : "none";
    }

    /* the canvas and its captured track, sized to the picture. false when unavailable */
    _surface(w: number, h: number): boolean {
        if (!w || !h) return false;
        if (!this.canvas) {
            this.canvas = document.createElement("canvas");
            this._ctx = this.canvas.getContext("2d");
        }
        if (this.canvas.width !== w || this.canvas.height !== h) { this.canvas.width = w; this.canvas.height = h; }
        this.width = w; this.height = h;
        if (!this._canvasTrack && this.stream && (this.canvas as any).captureStream)
            this._canvasTrack = (this.canvas as any).captureStream(0).getVideoTracks()[0];
        return !!this._ctx;
    }

    _painted(): void {
        this.frames++;
        /* pixels are arriving here: show the canvas unless the track is live too (keepData) */
        if (this.path !== "decoder" && (!this._rtcTrack || this._rtcTrack.muted)) this._showCanvas();
        (this._canvasTrack as any)?.requestFrame?.();
    }

    /* JPEG or PNG through the browser's image decoder. The newest frame wins while one decodes */
    _paintBlob(bytes: Uint8Array, mime: string): void {
        this._pendingBlob = { bytes, mime };
        if (this._blobBusy) return;
        const next = () => {
            const p = this._pendingBlob;
            this._pendingBlob = null;
            if (!p || this._closed) { this._blobBusy = false; return; }
            this._blobBusy = true;
            createImageBitmap(new Blob([p.bytes], { type: p.mime })).then((bmp) => {
                if (this._surface(bmp.width, bmp.height)) { this._ctx!.drawImage(bmp, 0, 0); this._painted(); }
                bmp.close();
                next();
            }, () => next());
        };
        next();
    }

    /* H264, H265 and AV1 through WebCodecs, with no track: the fallback, or a pushed value */
    _decode(f: VideoFrameValue): void {
        const VD = (globalThis as any).VideoDecoder;
        const EVC = (globalThis as any).EncodedVideoChunk;
        if (!VD || !EVC) {   /* WebCodecs lives in secure contexts only */
            this.decodeState = "no WebCodecs VideoDecoder on this origin (https, localhost or file:// needed)";
            return;
        }
        if (!f.data.length) return;
        if (!this._decoder && !f.keyframe) return;           /* a decoder starts on a keyframe */
        /* the codec string comes from the keyframe's parameter sets and holds for its deltas */
        const codec = f.keyframe ? codecString(f.codec, f.data) : this._decoderCodec;
        if (!this._decoder || this._decoderCodec !== codec) {
            this._decoder?.close?.();
            const d = new VD({
                output: (frame: any) => {
                    if (this._surface(frame.displayWidth, frame.displayHeight)) { this._ctx!.drawImage(frame, 0, 0); this._painted(); }
                    frame.close();
                },
                error: () => { if (this._decoder === d) { this._decoder = null; } },
            });
            const cfg: any = { codec, optimizeForLatency: true };
            if (f.width && f.height) { cfg.codedWidth = f.width; cfg.codedHeight = f.height; }
            d.configure(cfg);
            this._decoder = d;
            this._decoderCodec = codec;
        }
        try {
            this._decoder.decode(new EVC({ type: f.keyframe ? "key" : "delta",
                                           timestamp: Number(f.pts) || this.frames * 33333, data: f.data }));
        } catch (_e) {
            this._decoder = null;   /* resync on the next keyframe */
        }
    }
}

/* ---- the node --------------------------------------------------------------------- */

type AnyEntity = RambleTopic | FunctionDefinition | RemoteFunction | TaskDefinition | RemoteTask | VarHandle;

/* the bridge's --ice syntax, "stun:host:port" or "turn:user:pass@host:port", to RTCIceServer */
function iceServerFromUrl(url: string): RTCIceServer {
    const m = /^(stuns?|turns?):(?:([^:@]*):([^@]*)@)?(.*)$/.exec(url);
    if (!m) return { urls: url };
    const out: RTCIceServer = { urls: `${m[1]}:${m[4]}` };
    if (m[2] !== undefined) { out.username = decodeURIComponent(m[2]); out.credential = decodeURIComponent(m[3] ?? ""); }
    return out;
}

class RambleNode {
    /* the constant tables, reachable from the classic-script build (one global) */
    static MetaSection = MetaSection;
    static VideoCodec = VideoCodec;
    static ImageFormat = ImageFormat;
    static StreamKind = StreamKind;

    _ws: WebSocket;
    _seq: number;
    _pending: Map<number, { resolve: (r: any) => void; reject: (e: Error) => void }>;
    _entities: Map<number, AnyEntity>;
    _early: Map<number, Uint8Array[]>;   /* frames for an entity whose create is in flight */
    _nextId: number;
    _calls: Map<number, PendingCall<any>>;
    _nextCall: number;
    _closing: boolean;
    _views: Set<VideoView>;                      /* every VideoView on this node (for the offers) */
    _pc: RTCPeerConnection | null;
    _rtcUp: boolean;
    _rtcMax: number;                             /* the data channel message size limit */
    _iceQueue: RTCIceCandidateInit[];            /* remote candidates that beat the offer */
    _rtcChain: Promise<void>;                    /* serializes offer/answer rounds */
    _rtcFail: ((why: string) => void) | null;    /* aborts the connect attempt in flight */
    _anchor: RTCDataChannel | null;              /* channel 0: keeps the SCTP line in the offer */
    name: string;   /* this node's name, auto generated if none given */
    transport: TransportName;                    /* the data carrier in use */
    onEvent: ((e: RambleEvent) => void) | null;   /* every bridge event: errors, peers, loss, rtc */
    onClose: ((e: CloseEvent) => void) | null;
    _onLog: ((l: LogLine) => void) | null;       /* mesh log-stream handler (set by onLog) */

    /* Connect to a bridge and open the node. WebRTC is tried first (opts.transport
     * "auto", the default) and the WebSocket carries the data if it cannot connect. */
    static async connect(url: string, opts: NodeOpts = {}): Promise<RambleNode> {
        const ws = new WebSocket(url);
        ws.binaryType = "arraybuffer";
        await new Promise<unknown>((res, rej) => {
            ws.onopen = res;
            ws.onerror = () => rej(new Error(`connect failed: ${url}`));
        });
        const c = new RambleNode(ws);
        const { onEvent, transport, rtcTimeoutMs, iceServers, ...open } = opts;
        if (onEvent) c.onEvent = onEvent;
        const r = await c._request({ op: "open", ...open });
        c.name = r.name as string;
        if ((transport ?? "auto") !== "websocket" && r.webrtc && typeof RTCPeerConnection !== "undefined")
            await c._rtcConnect(rtcTimeoutMs ?? 4000, iceServers ?? []);
        return c;
    }

    constructor(ws: WebSocket) {
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

        ws.onmessage = (e: MessageEvent) => {
            if (typeof e.data === "string") this._onText(JSON.parse(e.data));
            else this._onFrame(new Uint8Array(e.data as ArrayBuffer));
        };
        ws.onclose = (e: CloseEvent) => {
            for (const p of this._pending.values()) p.reject(new Error("connection closed"));
            this._pending.clear();
            for (const p of this._calls.values()) {
                if (p.timer !== undefined) clearTimeout(p.timer);
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

    _request(obj: Record<string, any>): Promise<any> {
        const seq = ++this._seq;
        return new Promise((resolve, reject) => {
            this._pending.set(seq, { resolve, reject });
            this._ws.send(JSON.stringify({ ...obj, seq }));
        });
    }

    /* WebRTC: we offer, the bridge answers and candidates trickle both ways. The client always
     * offers, which makes the bridge the DTLS client (spec/bridge.md). */

    async _rtcConnect(timeoutMs: number, extraIce: RTCIceServer[]): Promise<void> {
        let pc: RTCPeerConnection | null = null;
        try {
            const r = await this._request({ op: "rtc" });
            const ice = [...((r.ice_servers ?? []) as string[]).map(iceServerFromUrl), ...extraIce];
            pc = new RTCPeerConnection({ iceServers: ice });
            this._pc = pc;
            pc.onicecandidate = (e) => {
                if (e.candidate && e.candidate.candidate)
                    this._request({ op: "rtc", candidate: e.candidate.candidate, mid: e.candidate.sdpMid ?? "" }).catch(() => {});
            };
            /* the anchor channel puts the SCTP line in the offer (entity ids start at 1) */
            this._anchor = pc.createDataChannel("ramble", { negotiated: true, id: 0 });
            for (const v of this._views) this._addVideoLine(v);
            const connected = new Promise<void>((res, rej) => {
                const timer = setTimeout(() => rej(new Error("webrtc connect timeout")), timeoutMs);
                const fail = (why: string) => { clearTimeout(timer); rej(new Error(`webrtc ${why}`)); };
                this._rtcFail = fail;   /* the bridge side can fail first, fall back at once */
                pc!.onconnectionstatechange = () => {
                    const s = pc!.connectionState;
                    if (s === "connected") { clearTimeout(timer); res(); }
                    else if (s === "failed" || s === "closed") fail(s);
                    this._rtcState();
                };
            });
            await Promise.all([this._rtcOffer(), connected]);
            this._rtcFail = null;
            this._rtcUp = true;
            this.transport = "webrtc";
            const max = (pc as any).sctp?.maxMessageSize;
            this._rtcMax = max && Number.isFinite(max) && max > 0 ? max : 65536;
            for (const e of this._entities.values()) this._openChannel(e);
        } catch (_e) {
            this._rtcFail = null;
            this._rtcDrop();   /* the WebSocket carries everything: same API, same wire */
        }
    }

    /* one offer and answer round, the first or a re offer after a video line was added.
     * tracks tells the bridge which media entity each offered video line is for */
    _rtcOffer(): Promise<void> {
        const pc = this._pc;
        if (!pc) return Promise.resolve();
        const step = async () => {
            const offer = await pc.createOffer();
            await pc.setLocalDescription(offer);
            const tracks: Record<string, any> = {};
            const lines: [string, VideoView][] = [];
            for (const v of this._views) {
                const req = v._transceiver?.mid ? v._trackReq() : null;
                if (req) { tracks[v._transceiver!.mid!] = req; lines.push([v._transceiver!.mid!, v]); }
            }
            const r = await this._request({ op: "rtc", sdp: pc.localDescription?.sdp ?? offer.sdp, tracks });
            await pc.setRemoteDescription({ type: "answer", sdp: r.sdp as string });
            for (const cand of this._iceQueue.splice(0)) await pc.addIceCandidate(cand).catch(() => {});
            for (const [mid, v] of lines) {                  /* the bridge's verdict per line */
                v.trackState = (r.tracks ?? {})[mid] ?? "no answer";
                if (v.trackState !== "ok") { v._detachTrack(); v._transceiver = null; }
            }
        };
        this._rtcChain = this._rtcChain.then(step, step);
        return this._rtcChain;
    }

    /* a recvonly video line for a view's source. The bridge sends on it after the re offer */
    _addVideoLine(v: VideoView): void {
        const pc = this._pc;
        if (!pc || v._transceiver || !v.source) return;
        try {
            v._transceiver = pc.addTransceiver("video", { direction: "recvonly" });
            v._attachTrack(v._transceiver.receiver.track);
        } catch (_e) {
            v._transceiver = null;   /* frames it is, then */
        }
    }

    _rtcState(): void {
        const s = this._pc?.connectionState;
        const up = s === "connected";
        if (up === this._rtcUp) return;
        this._rtcUp = up;
        this.transport = up ? "webrtc" : "websocket";   /* a broken link falls back mid-session */
        if (up) for (const e of this._entities.values()) this._openChannel(e);
    }

    _rtcDrop(): void {
        this._rtcUp = false;
        this.transport = "websocket";
        const pc = this._pc;
        this._pc = null;
        this._anchor = null;
        if (pc) { pc.onicecandidate = null; pc.onconnectionstatechange = null; try { pc.close(); } catch (_e) { /* already closed */ } }
        for (const e of this._entities.values()) e._dc = null;
        for (const v of this._views) { v._transceiver = null; v._detachTrack(); }
    }

    /* the entity's data channel, negotiated with id = entity id so both ends open it without
     * a round trip. Ordering and retransmission follow the topic's reliability */
    _makeChannel(id: number, reliable: boolean): RTCDataChannel | null {
        const pc = this._pc;
        if (!pc || !this._rtcUp) return null;
        try {
            const init: RTCDataChannelInit = reliable ? { negotiated: true, id, ordered: true }
                                                      : { negotiated: true, id, ordered: false, maxRetransmits: 0 };
            const dc = pc.createDataChannel(`d${id}`, init);
            dc.binaryType = "arraybuffer";
            dc.onmessage = (e: MessageEvent) => this._onFrame(new Uint8Array(e.data as ArrayBuffer));
            return dc;
        } catch (_e) {
            return null;   /* the WebSocket carries this entity */
        }
    }

    _openChannel(e: RambleEntity): void {
        if (e._dc) return;
        e._dc = this._makeChannel(e.id, e.reliable);
    }

    /* the carrier per frame: the entity's open data channel when the frame fits, else the
     * WebSocket. A best effort frame is dropped rather than queued behind a backlog */
    _sendFrame(e: RambleEntity, bytes: Uint8Array): void {
        const dc = e._dc;
        if (dc && dc.readyState === "open" && bytes.byteLength <= this._rtcMax) {
            if (!e.reliable && dc.bufferedAmount > LOSSY_BUFFER) return;
            try { dc.send(bytes); return; } catch (_e) { /* the WebSocket takes it */ }
        }
        if (!e.reliable && this._ws.bufferedAmount > LOSSY_BUFFER) return;
        this._ws.send(bytes);
    }

    _onText(m: any): void {
        if (m.op === "reply") {
            const p = this._pending.get(m.seq);
            if (!p) return;
            this._pending.delete(m.seq);
            if (m.ok) p.resolve(m);
            else p.reject(new Error(m.error ?? "request failed"));
        } else if (m.op === "event") {
            if (m.event === "rtc" && (m.state === "failed" || m.state === "closed")) this._rtcFail?.(m.state);
            this.onEvent?.(m as RambleEvent);
        } else if (m.op === "match") {
            this._entities.get(m.id)?._match(m);
        } else if (m.op === "log") {
            this._onLog?.({ level: m.level, node: m.node, wallUs: m.wall_us,
                            monoUs: m.mono_us, recvUs: m.recv_us, writtenUs: m.written_us ?? 0,
                            text: m.text });
        } else if (m.op === "rtc" && typeof m.candidate === "string") {
            const cand: RTCIceCandidateInit = { candidate: m.candidate, sdpMid: m.mid ?? "" };
            if (this._pc?.remoteDescription) this._pc.addIceCandidate(cand).catch(() => {});
            else this._iceQueue.push(cand);   /* the answer is still being applied */
        }
    }

    _onFrame(b: Uint8Array): void {
        const f = parseFrame(b);
        if (!f) return;
        if (f.op === OP_RESULT || f.op === OP_PROGRESS) { this._onCallFrame(f); return; }
        const e = this._entities.get(f.id);
        if (e) e._frame(f);
        else this._early.get(f.id)?.push(b);   /* its create is still in flight, replay after */
    }

    /* outcomes and progress route by call id (node-wide), not by entity */
    _onCallFrame(f: Frame): void {
        const p = this._calls.get(f.seq);
        if (!p) return;                          /* a client-side timeout already settled it */
        if (f.op === OP_PROGRESS) { p.progress?.(f.payload, f.peer, f.writtenUs); return; }
        this._calls.delete(f.seq);
        if (p.timer !== undefined) clearTimeout(p.timer);
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
    async _create<E extends AnyEntity>(kind: string, name: string, reliable: boolean, fields: Record<string, any>,
                                       make: (r: any, dc: RTCDataChannel | null) => E): Promise<E> {
        const id = ++this._nextId;
        if (id > 0xfffe) throw new Error("out of entity ids");
        const dc = this._makeChannel(id, reliable);
        this._early.set(id, []);
        let r: any;
        try {
            r = await this._request({ op: "create", id, kind, name, ...fields });
        } catch (e) {
            this._early.delete(id);
            dc?.close();
            throw e;
        }
        const ent = make(r, dc);
        this._entities.set(id, ent);
        const early = this._early.get(id) ?? [];
        this._early.delete(id);
        for (const b of early) this._onFrame(b);
        return ent;
    }

    /* Create a topic, the dynamic form. opts.schema is DSL text for a typed topic, omit it
     * for raw bytes. */
    async topic(name: string, role: Role = "pubsub", opts: TopicOpts = {}): Promise<RambleTopic> {
        return this._create("topic", name, !!opts.reliable, { role, ...opts },
                            (r, dc) => new RambleTopic(this, name, r, dc));
    }

    /* Typed publish side: schema is the DSL text (null = raw bytes). */
    async publisher<T = any>(name: string, schema: string | null, opts: TopicOpts = {}): Promise<Publisher<T>> {
        const t = await this.topic(name, "pub", { ...opts, ...(schema ? { schema } : {}) });
        return new Publisher<T>(t);
    }

    /* Typed subscribe side: handler(value, msg) fires per delivery with the decoded
     * plain object (raw topic: the payload bytes). */
    async subscriber<T = any>(name: string, schema: string | null, handler: SubscriberHandler<T>,
                              opts: TopicOpts = {}): Promise<Subscriber<T>> {
        const t = await this.topic(name, "sub", { ...opts, ...(schema ? { schema } : {}) });
        return new Subscriber<T>(t, handler);
    }

    /* A picture sink: push VideoFrame / Image values into it from anywhere, or attach it to
     * an entity's stream (see VideoView). videoEl.srcObject = view.stream. */
    videoView(): VideoView { return new VideoView(this); }

    /* Sugar: subscribe to a VideoFrame topic, best effort unless opts say otherwise, and
     * attach a view. Over WebRTC an encoded stream arrives as a track, else frames decode here. */
    async video(name: string, opts: TopicOpts = {}): Promise<VideoView> {
        const t = await this.topic(name, "sub", { schema: VIDEO_FRAME, ...opts });
        return new VideoView(this).attach(t);
    }

    /* Sugar: subscribe to an Image topic and attach a view (JPEG / PNG / raw pixels). */
    async image(name: string, opts: TopicOpts = {}): Promise<VideoView> {
        const t = await this.topic(name, "sub", { schema: IMAGE, ...opts });
        return new VideoView(this).attach(t);
    }

    /* Host a function: handler(reqValue) returns the reply, possibly async, and a throw answers
     * "app_error". Schemas are DSL text, null = raw bytes. */
    async functionDefinition<Req = any, Rsp = any>(name: string, reqSchema: string | null,
            rspSchema: string | null, handler: FunctionHandler<Req, Rsp>,
            opts: FunctionOpts = {}): Promise<FunctionDefinition<Req, Rsp>> {
        return this._create("function_definition", name, true,
            { ...opts, ...(reqSchema ? { req: reqSchema } : {}), ...(rspSchema ? { rsp: rspSchema } : {}) },
            (r, dc) => new FunctionDefinition<Req, Rsp>(this, name, r, dc, handler));
    }

    /* A reference to a function hosted elsewhere. */
    async remoteFunction<Req = any, Rsp = any>(name: string, reqSchema: string | null,
            rspSchema: string | null, opts: FunctionOpts = {}): Promise<RemoteFunction<Req, Rsp>> {
        return this._create("remote_function", name, true,
            { ...opts, ...(reqSchema ? { req: reqSchema } : {}), ...(rspSchema ? { rsp: rspSchema } : {}) },
            (r, dc) => new RemoteFunction<Req, Rsp>(this, name, r, dc));
    }

    /* Host a task: handler(reqValue, ctx) streams ctx.progress() and its settlement is the one
     * terminal answer, ctx.signal aborts on a cancel request. Schemas are DSL text or null. */
    async taskDefinition<Req = any, Prg = any, Rsp = any>(name: string, reqSchema: string | null,
            prgSchema: string | null, rspSchema: string | null, handler: TaskHandler<Req, Prg, Rsp>,
            opts: TaskOpts = {}): Promise<TaskDefinition<Req, Prg, Rsp>> {
        return this._create("task_definition", name, true,
            { ...opts, ...(reqSchema ? { req: reqSchema } : {}), ...(prgSchema ? { prg: prgSchema } : {}),
              ...(rspSchema ? { rsp: rspSchema } : {}) },
            (r, dc) => new TaskDefinition<Req, Prg, Rsp>(this, name, r, dc, handler));
    }

    /* A reference to a task hosted elsewhere. task.call(req) returns a TaskRun handle
     * synchronously: run.onProgress(cb), await run.result, run.cancel(). */
    async remoteTask<Req = any, Prg = any, Rsp = any>(name: string, reqSchema: string | null,
            prgSchema: string | null, rspSchema: string | null,
            opts: TaskOpts = {}): Promise<RemoteTask<Req, Prg, Rsp>> {
        return this._create("remote_task", name, true,
            { ...opts, ...(reqSchema ? { req: reqSchema } : {}), ...(prgSchema ? { prg: prgSchema } : {}),
              ...(rspSchema ? { rsp: rspSchema } : {}) },
            (r, dc) => new RemoteTask<Req, Prg, Rsp>(this, name, r, dc));
    }

    /* Host a variable. initial is applied with a set right after the create, since encoding
     * needs the field table the create returns. */
    async variableDefinition<T = any>(name: string, schema: string | null,
            opts: VariableDefOpts<T> = {}): Promise<VariableDefinition<T>> {
        const v = await this._create("variable_definition", name, true, {
            ...(schema ? { schema } : {}),
            ...(opts.readOnly ? { read_only: true } : {}),
            ...(opts.allowForce ? { allow_force: true } : {}),
            ...(opts.catch_up ? { catch_up: opts.catch_up } : {}),
            ...(opts.keep_last ? { keep_last: opts.keep_last } : {}),
            ...(opts.backpressure_wait_ms ? { backpressure_wait_ms: opts.backpressure_wait_ms } : {}),
            ...(opts.onWrite ? { on_write: true } : {}),
            ...(opts.reflect ? { reflect: true } : {}),
        }, (r, dc) => new VariableDefinition<T>(this, name, r, dc));
        if (opts.initial !== undefined) v.set(opts.initial);
        return v;
    }

    /* Access a variable owned elsewhere. Pass { onWrite: true } to also receive every
     * applied write (route it via RemoteVariable.onWrite). */
    async remoteVariable<T = any>(name: string, schema: string | null,
            opts: RemoteVarOpts = {}): Promise<RemoteVariable<T>> {
        return this._create("remote_variable", name, true,
            { ...(schema ? { schema } : {}), ...(opts.onWrite ? { on_write: true } : {}),
              ...(opts.reflect ? { reflect: true } : {}) },
            (r, dc) => new RemoteVariable<T>(this, name, r, dc));
    }

    /* Block until discovery + matching settle for everything created so far. */
    async settle(timeoutMs: number = -1): Promise<boolean> {
        const r = await this._request({ op: "settle", timeout_ms: timeoutMs });
        return r.settled as boolean;
    }

    /* Publish a line on a level's built-in @ramble/log topic (mesh-wide, rosout-style).
     * Every node that subscribed to that level receives it. */
    async log(level: LogLevelName, text: string): Promise<void> {
        await this._request({ op: "log", level, text });
    }
    logError(text: string): Promise<void> { return this.log("error", text); }
    logWarn (text: string): Promise<void> { return this.log("warn",  text); }
    logInfo (text: string): Promise<void> { return this.log("info",  text); }

    /* Subscribe to the mesh's log stream at the given levels, default all three: every other
     * node's lines as a LogLine, history replaying on match. One handler for all levels. */
    async onLog(handler: (line: LogLine) => void,
                levels: LogLevelName[] = ["error", "warn", "info"]): Promise<void> {
        this._onLog = handler;
        await this._request({ op: "log_subscribe", levels });
    }

    /* ---- introspection (query-based, pull-only) ---------------------------------- */

    /* Snapshot the discovered peer table. A local read that resolves immediately. */
    async peers(): Promise<Peer[]> {
        const r = await this._request({ op: "peers" });
        return (r.peers as any[]).map((p) => ({
            id: p.id, name: p.name, address: p.address, active: p.active, fragmentSize: p.fragment_size,
            rttUs: p.rtt_us ?? 0, rttJitterUs: p.rtt_jitter_us ?? 0, rttMinUs: p.rtt_min_us ?? 0,
            rttSamples: p.rtt_samples ?? 0 }));
    }

    /* The entities THIS node hosts (its functions and variables, then its topics). */
    async entities(): Promise<Entity[]> {
        const r = await this._request({ op: "entities" });
        return (r.entities as any[]).map(toEntity);
    }

    /* What one peer advertises, folded into entities. A dropped peer yields [] by default since
     * its cached entities are its dead incarnation's, includeDropped serves them anyway. */
    async peerEntities(peerId: number, includeDropped: boolean = false): Promise<Entity[]> {
        const r = await this._request({ op: "peer_entities", peer: peerId,
                                        include_dropped: includeDropped });
        return (r.entities as any[]).map(toEntity);
    }

    /* The whole mesh folded, one entity per kind and name across every active peer and the
     * bridge's node, conflict when endpoints disagree. epoch moves on every change. */
    async mesh(): Promise<{ entities: Entity[]; epoch: number }> {
        const r = await this._request({ op: "mesh" });
        return { entities: (r.entities as any[]).map(toEntity), epoch: r.epoch ?? 0 };
    }

    /* One mesh entity by kind and name, or null. */
    async meshFind(kind: EntityKindName, name: string): Promise<Entity | null> {
        const r = await this._request({ op: "mesh_find", kind, name });
        return r.entity ? toEntity(r.entity) : null;
    }

    /* Fetch a peer's @ramble/meta snapshot by an async directed call. Never rejects on status.
     * sections is a MetaSection mask, default All. */
    async meta(peerId: number, sections: number = MetaSection.All): Promise<MetaSnapshot> {
        const r = await this._request({ op: "meta", peer: peerId, sections });
        return { valid: !!r.valid, status: CALL_STATUS[r.status] ?? "cancelled",
                 provider: r.provider ?? 0, info: r.info ?? {} };
    }

    /* Close the connection. The bridge closes the node with a BYE and outstanding call
     * promises settle "cancelled". */
    close(): void {
        this._closing = true;
        this._rtcDrop();
        this._ws.close();
    }
}

export {
    RambleNode, RambleEntity, RambleTopic, RambleMessage, Layout,
    Publisher, Subscriber,
    FunctionDefinition, RemoteFunction,
    TaskDefinition, RemoteTask, TaskRun,
    VariableDefinition, RemoteVariable,
    VideoView, VideoCodec, ImageFormat, StreamKind, VIDEO_FRAME, IMAGE, EXTERNAL_VIDEO_STREAM,
    MetaSection,
    type Field, type SchemaBlock, type Role, type TopicOpts, type NodeOpts, type RambleEvent,
    type TransportName,
    type CallStatusName, type Response, type RequestInfo,
    type SubscriberHandler, type FunctionHandler,
    type TaskContext, type TaskHandler, type TaskProgressHandler, type TaskOpts, type CancelStatus,
    type VariableDefOpts, type RemoteVarOpts, type VarChangeHandler,
    type VideoFrameValue, type ImageValue, type ExternalVideoStreamValue, type MediaValue, type MediaTypeName,
    type VideoPath, type MediaSource, type MediaAttachOpts, type MediaField,
    type FunctionOpts,
    type LogLevelName, type LogLine,
    type Peer, type Entity, type EntityKindName, type MetaSnapshot,
};
