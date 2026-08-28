/* DART WebSocket bridge client: one DartNode = one full DART node on the mesh, spoken
 * through the bridge (protocol v10, see ../PROTOCOL.md). Zero runtime dependencies: runs
 * in browsers, Node (>= 22), Deno and Bun off the global WebSocket.
 *
 * TypeScript source, compiled by pure type stripping to dist/dart.mjs (+ dart.d.ts and
 * the classic-script twin dist/dart.js); see build.mjs. The emitted JS reads like this
 * file: no enums, no namespaces, no parameter properties, no decorators.
 *
 * Two API layers over one wire:
 *  - the dynamic form: node.topic(...) with flat dotted-path get/send (unchanged from v2);
 *  - the pattern factories: publisher/subscriber, functionDefinition/remoteFunction,
 *    taskDefinition/remoteTask, variableDefinition/remoteVariable. These speak decoded
 *    PLAIN OBJECTS (nested, mirroring the schema) and carry optional type parameters for
 *    TS callers.
 *
 *   const node = await DartNode.connect("ws://localhost:7480", { name: "dashboard" });
 *   const pub  = await node.publisher("pose", "Pose { x: f64, y: f64 }");
 *   pub.send({ x: 1.5, y: 2.0 });
 *   const add  = await node.remoteFunction("add", "A { a: i32, b: i32 }", "R { sum: i32 }");
 *   const r    = await add.call({ a: 2, b: 3 });   // r.status === "ok", r.value.sum === 5
 */

/* binary frame ops (byte 0); meaning per direction, headers little-endian */
const OP_DATA = 0x01;      /* publish / delivery */
const OP_VAR = 0x02;       /* var set-force-unforce / var update */
const OP_PROGRESS = 0x03;  /* task progress (definition streams / caller receives) */
const OP_CALL = 0x04;      /* call / call response */
const OP_REQUEST = 0x05;   /* request reply / request */

/* Every SERVER-TO-CLIENT data frame ends its header with [u64 written_us], the sender's wall
 * clock (UTC microseconds) at the moment its send committed: a SOURCE stamp, so a repaired
 * or replayed message keeps the original value. 0 = the publisher opted out, or the frame is
 * a synthesized outcome. Surfaced as `writtenUs` wherever a delivery reaches the app; it is a
 * different clock from a log line's recvUs, so never mix them. Microseconds stay inside the
 * JS safe-integer range, so it reads as a plain number. */
function rdWrittenUs(view: DataView, off: number): number {
    return Number(view.getBigUint64(off, true));
}

type Field = {
    path: string;
    kind: string;
    named?: string;                                /* the field type's NAME ("Pose"), if any */
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

type Role = "pubsub" | "pub" | "sub" | "inactive";

type TopicOpts = {
    schema?: string;
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
    announce_interval_ms?: number;
    peer_timeout_ms?: number;
    match_wait_ms?: number;
    disable_shm?: boolean;
    disable_logs?: boolean;   /* strip the built-in @dart/log topics */
    disable_meta?: boolean;   /* do not host the @dart/meta endpoint */
    disable_error_logs?: boolean; /* suppress default mirroring onto @dart/log/error */
    fetch_details?: boolean;  /* resolve reflected entity names for topics this node does not share */
    onEvent?: (e: DartEvent) => void;
};

type DartEvent = { op: "event"; event: string; text?: string } & Record<string, unknown>;

type LogLevelName = "error" | "warn" | "info";

/* One decoded @dart/log line handed to DartNode.onLog. wallUs is epoch micros
 * (comparable across nodes); monoUs is the publisher's monotonic clock; recvUs is this
 * node's clock when the poll received it; writtenUs is the carrying message's source stamp. */
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

/* ---- introspection (query-based, see DartNode.peers / entities / meta) ------------ */

/* A discovered peer, as DartNode.peers() snapshots it. */
type Peer = {
    id: number;
    name: string;
    address: string;        /* "1.2.3.4:port" */
    active: boolean;        /* heard within peer_timeout (vs a dormant/dropped peer) */
    fragmentSize: number;
};

type EntityKindName = "topic" | "function" | "task" | "variable";

/* One network entity a node hosts or a peer advertises (pattern channels folded: a
 * function's req/rsp pair is one entity, a variable's set channel merges as `writable`).
 * `name` is "0x????????" until the peer's details are fetched (see fetch_details). */
type Entity = {
    kind: EntityKindName;
    name: string;
    provides: boolean;      /* source side: publisher / definition / owner / emitter */
    consumes: boolean;      /* sink side: subscriber / caller / accessor / listener */
    reliable: boolean;
    writable?: boolean;     /* variable: a set channel is advertised */
    forceable?: boolean;    /* variable: the owner permits force/unforce */
    cancellable?: boolean;  /* task: the provider honors cancel (default on) */
    exclusive?: boolean;    /* task: declared serialization (the handler enforces it) */
    multi?: boolean;        /* task: redundant providers intended */
    incomplete?: boolean;   /* a pattern half-pair, surfaced not dropped */
    index: number;          /* the primary channel's index at the peer */
    hash: number;           /* the primary channel's low-32 name hash */
    schemaHash?: string;    /* value/request/payload schema identity, hex (absent = untyped/unfetched) */
    rspSchemaHash?: string; /* function/task: the response schema identity, hex */
    progressSchemaHash?: string; /* task only: the progress schema identity, hex */
    schema?: SchemaBlock;   /* value/request/payload field table (present when the schema is known);
                               pass to `new Layout(e.schema)` to decode/inspect messages */
    rspSchema?: SchemaBlock; /* function/task: the response field table */
    progressSchema?: SchemaBlock; /* task only: the progress field table */
};

/* @dart/meta section mask (OR the bits; 0 = every section). Mirrors DART_META_*. */
const MetaSection = { Node: 0x1, Proc: 0x2, Topics: 0x4, Peers: 0x8, All: 0 } as const;

/* A decoded @dart/meta reply. Never faults: inspect `status`. `info` is the whole
 * self-describing snapshot body (node / proc / topics[] / peers[] sub-objects, present
 * per the requested sections). */
type MetaSnapshot = {
    valid: boolean;              /* a status "ok" reply decoded */
    status: CallStatusName;
    provider: number;            /* peer id that answered */
    info: Record<string, any>;   /* the decoded body (empty when not valid) */
};

/* one reflected-entity reply row -> Entity (camel-cases the hex hash fields) */
function toEntity(e: any): Entity {
    const out: Entity = {
        kind: e.kind, name: e.name, provides: !!e.provides, consumes: !!e.consumes,
        reliable: !!e.reliable, index: e.index, hash: e.hash };
    if (e.writable !== undefined)   out.writable = !!e.writable;
    if (e.forceable !== undefined)  out.forceable = !!e.forceable;
    if (e.cancellable !== undefined) out.cancellable = !!e.cancellable;
    if (e.exclusive !== undefined)   out.exclusive = !!e.exclusive;
    if (e.multi !== undefined)       out.multi = !!e.multi;
    if (e.incomplete)               out.incomplete = true;
    if (e.schema_hash !== undefined)     out.schemaHash = e.schema_hash;
    if (e.rsp_schema_hash !== undefined) out.rspSchemaHash = e.rsp_schema_hash;
    if (e.progress_schema_hash !== undefined) out.progressSchemaHash = e.progress_schema_hash;
    if (e.schema) out.schema = e.schema;
    if (e.rsp)    out.rspSchema = e.rsp;
    if (e.progress_schema) out.progressSchema = e.progress_schema;
    return out;
}

type Response<Rsp = any> = {
    ok: boolean;
    status: CallStatusName;
    value: Rsp | undefined;   /* decoded reply (typed functions, status "ok") */
    data: Uint8Array;         /* the raw reply payload */
    provider: number;         /* peer id of the answering node (0 if none) */
    writtenUs: number;           /* the provider's source stamp (see rdWrittenUs); 0 = synthesized */
    message: string;          /* human-readable outcome text, the one field to display on a
                                 failure: the definition's message (a throw's Error.message,
                                 or the C side's dart_request_fail text), else default status
                                 text ("timeout", ...). "" only on ok with no message. */
};

/* default Response.message per status when the definition sent no text (mirrors the C) */
const CALL_STATUS_TEXT: Record<CallStatusName, string> = {
    ok: "", app_error: "app error", no_handler: "no handler",
    timeout: "timeout", peer_lost: "peer lost", cancelled: "cancelled",
};

type RequestInfo = { caller: number; callerName: string; writtenUs: number };

type SubscriberHandler<T> = (value: T, msg: DartMessage) => void;
type FunctionHandler<Req, Rsp> = (req: Req, info: RequestInfo) => Rsp | Promise<Rsp>;

/* What a task handler receives beside the request. progress() streams one update to
 * every observer; signal aborts when the caller (or a third party) requests
 * cancellation. Honor a cancel by throwing the signal's reason
 * (ctx.signal.throwIfAborted()): the client answers status "cancelled". Any other throw
 * answers "app_error"; a normal return always completes "ok", even after an abort (it
 * ran to completion anyway). */
type TaskContext<Prg = any> = {
    progress: (value: Prg) => void;
    signal: AbortSignal;
    cancelled: boolean;      /* sugar for signal.aborted */
    caller: number;
    callerName: string;
    writtenUs: number;
};
type TaskHandler<Req, Prg, Rsp> = (req: Req, ctx: TaskContext<Prg>) => Rsp | Promise<Rsp>;

/* one decoded progress update on a task call; null value = the RUNNING acknowledgment */
type TaskProgressHandler<Prg> = (value: Prg | null,
                                 info: { provider: number; writtenUs: number }) => void;

/* the cancel op's verdict: "ok" = cancel requested (the terminal status answers whether
 * it was honored), "no_cancel" = the provider declared no_cancel (refused locally),
 * "not_pending" = the call already answered, "error" = anything else */
type CancelStatus = "ok" | "no_cancel" | "not_pending" | "error";

type TaskOpts = {
    progress_best_effort?: boolean;  /* progress-channel reliability: omit/false = reliable */
    progress_keep_last?: number;     /* progress ring depth; 0 = the pattern default */
    no_cancel?: boolean;             /* definition: will not honor cancellation */
    exclusive?: boolean;             /* definition: declared serialization (handler enforces) */
    multi?: boolean;                 /* definition: redundant providers intended */
    timeout_ms?: number;             /* remote: until-first-response bound; 0 = 5s */
    backpressure_wait_ms?: number;
};

type VariableDefOpts<T> = {
    initial?: T;
    readOnly?: boolean;
    allowForce?: boolean;
    catch_up?: number;
    backpressure_wait_ms?: number;
    onWrite?: boolean;   /* also push every applied write (route via VarHandle.onWrite) */
};

type RemoteVarOpts = {
    onWrite?: boolean;   /* also push every applied write (route via VarHandle.onWrite) */
};

const SCALAR_BYTES: Record<string, number> = {
    u8: 1, u16: 2, u32: 4, u64: 8, i8: 1, i16: 2, i32: 4, i64: 8, f32: 4, f64: 8, bool: 1,
};
const VARIABLE = new Set(["vstring", "varr", "map"]);
const enc = new TextEncoder();
const dec = new TextDecoder();

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

/* Read a capped-string slot [u16 len][cap bytes] at off; len is clamped to cap like the
 * C reader, so a hostile length can never over-read. */
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

/* JS value -> map value. bigint keeps its 64-bit width/sign; a plain number encodes as
 * i64 when integral, f64 otherwise (the C reader widens, so this stays lossless). */
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

/* Decode an `arr`/`varr` payload (`bytes`, length `len` from `off`) into a JS value: a
 * Uint8Array view for u8 elements, an Array of strings for string elements, else an
 * Array of scalars. */
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

/* Layout: one compiled schema's field tables (from a create reply), with the field
 * codec and the full-message encode/decode over plain nested objects. A layout with no
 * schema (raw entity) passes bytes through. */
class Layout {
    size: number | undefined;         /* fixed-section length; where the variable tail begins */
    hash: string | undefined;         /* 64-bit schema identity, hex */
    fields: Map<string, Field>;       /* dotted path -> field, in schema order */
    varFields: Field[];               /* variable fields in tail order */
    valueRoot: boolean;               /* a bare type: the whole message is one value */

    constructor(r: SchemaBlock | undefined) {
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

    /* one field by dotted path (see DartMessage.get) */
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
        if (f.kind === "enum") return readScalar(view, f.backing!, f.offset);   /* the number; label via enumName */
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

    /* Full-message encode from a plain nested object (missing fixed fields are zero,
     * missing variable fields empty), or from the bare value for a bare-type schema. An
     * untyped layout accepts bytes (or nothing). */
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
class DartMessage {
    topic: DartTopic | null;   /* the receiving topic (topic deliveries; null elsewhere) */
    publisher: number;         /* peer id of the sending node */
    data: Uint8Array;          /* the payload, verbatim */
    writtenUs: number;            /* the publisher's source stamp (see rdWrittenUs); 0 = opted out */
    _layout: Layout;
    _view: DataView;

    constructor(layout: Layout, topic: DartTopic | null, publisher: number, data: Uint8Array,
                writtenUs: number = 0) {
        this._layout = layout;
        this.topic = topic;
        this.publisher = publisher;
        this.data = data;
        this.writtenUs = writtenUs;
        this._view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    }

    /* Typed read of one field by dotted path ("vel.dx"). Scalars return number
     * (u64/i64: bigint, bool: boolean); a `string`/`vstring` returns a JS string; a
     * `map` returns a plain object; arrays return an Array (a u8 array returns a
     * Uint8Array view); structs return a Uint8Array view of their bytes. */
    get(path: string): any { return this._layout.getField(this.data, this._view, path); }

    /* The whole message as a plain nested object (untyped: the raw bytes). */
    value(): any { return this._layout.decode(this.data); }

    /* A u8 array field decoded as UTF-8 text, trailing NULs stripped (or up to
     * `lenField`'s value when given). Prefer a `string`/`vstring` field, which `get`
     * returns as a JS string directly; this stays for `u8[]`-style byte fields. */
    text(path: string, lenField?: string): string {
        const bytes = this.get(path) as Uint8Array;
        let n = lenField !== undefined ? Number(this.get(lenField)) : bytes.length;
        if (lenField === undefined) while (n > 0 && bytes[n - 1] === 0) n--;
        return dec.decode(bytes.subarray(0, Math.min(n, bytes.length)));
    }
}

/* One topic on the node (the dynamic form). Returned by DartNode.topic(). */
class DartTopic {
    _node: DartNode;
    name: string;              /* the cross-peer topic identity */
    id: number;                /* topic id in binary frames */
    layout: Layout;
    onMessage: ((msg: DartMessage) => void) | null;
    matchCount: number;        /* matched remote endpoints (from match pushes) */
    ready: boolean;            /* a send would not wait on a forming match */

    constructor(node: DartNode, name: string, r: { id: number } & SchemaBlock) {
        this._node = node;
        this.name = name;
        this.id = r.id;
        this.layout = new Layout(r);
        this.onMessage = null;
        this.matchCount = 0;
        this.ready = false;
    }

    /* fixed-section size / schema hash / field table (typed topics) */
    get size(): number | undefined { return this.layout.size; }
    get hash(): string | undefined { return this.layout.hash; }
    get fields(): Map<string, Field> { return this.layout.fields; }

    /* Publish raw bytes. */
    sendRaw(bytes: Uint8Array): void {
        const frame = new Uint8Array(3 + bytes.length);
        frame[0] = OP_DATA;
        frame[1] = this.id & 0xff; frame[2] = this.id >> 8;
        frame.set(bytes, 3);
        this._node._ws.send(frame);
    }

    /* Typed publish: encode named fields (FLAT dotted paths, the v2 form) into a
     * message and send it. Unset fixed fields are zero; unset variable fields are
     * empty. For nested plain objects use a Publisher. A BARE-TYPE topic (`bool`,
     * `f32[]`, ...) takes the value itself: send(true). */
    send(values: Record<string, any> | any): void {
        if (this.layout.size === undefined) throw new Error(`'${this.name}' is a raw topic: use sendRaw`);
        if (this.layout.valueRoot) { this.sendRaw(this.layout.encode(values)); return; }
        const fixed = new Uint8Array(this.layout.size);
        const view = new DataView(fixed.buffer);
        for (const [path, v] of Object.entries(values)) {
            const f = this.layout.fields.get(path);
            if (!f) throw new Error(`no field '${path}'`);
            if (VARIABLE.has(f.kind)) continue;   /* handled in tail order below */
            writeFixedField(view, fixed, f, v);
        }
        /* variable tail: one frame per variable field, in schema order */
        const frames = this.layout.varFields.map((f) => encodeVarFrame(f, values[f.path]));
        let total = this.layout.size;
        for (const fr of frames) total += 4 + fr.length;
        const buf = new Uint8Array(total);
        buf.set(fixed, 0);
        const dv = new DataView(buf.buffer);
        let pos = this.layout.size;
        for (const fr of frames) { dv.setUint32(pos, fr.length, true); pos += 4; buf.set(fr, pos); pos += fr.length; }
        this.sendRaw(buf);
    }

    /* Flip this topic's role: "pubsub" | "pub" | "sub" | "inactive". */
    setRole(role: Role): Promise<any> { return this._node._request({ op: "role", topic: this.id, role }); }

    /* Wait until every reader acked everything (reliable topics, before close). */
    async drain(timeout_ms: number = 1000): Promise<boolean> {
        const r = await this._node._request({ op: "drain", topic: this.id, timeout_ms });
        return r.drained as boolean;
    }

    _match(m: any): void { this.matchCount = m.matches; this.ready = !!m.ready; }
    _deliver(publisher: number, data: Uint8Array, writtenUs: number): void {
        this.onMessage?.(new DartMessage(this.layout, this, publisher, data, writtenUs));
    }
}

/* Publish-side handle over a topic; speaks plain nested objects. */
class Publisher<T = any> {
    topic: DartTopic;

    constructor(topic: DartTopic) { this.topic = topic; }

    send(value: T): void { this.topic.sendRaw(this.topic.layout.encode(value)); }
    sendRaw(bytes: Uint8Array): void { this.topic.sendRaw(bytes); }
    get matchCount(): number { return this.topic.matchCount; }
    get ready(): boolean { return this.topic.ready; }
}

/* Subscribe-side handle: the handler gets (decoded plain object, message). */
class Subscriber<T = any> {
    topic: DartTopic;

    constructor(topic: DartTopic, handler: SubscriberHandler<T>) {
        this.topic = topic;
        this.topic.onMessage = (msg) => handler(msg.value() as T, msg);
    }

    get matchCount(): number { return this.topic.matchCount; }
}

/* The implementation side of a request/response function: the bridge defers every
 * request to this client; the handler's (possibly async) return value is the reply,
 * a throw answers "app_error". ONE definition per name on the network. */
class FunctionDefinition<Req = any, Rsp = any> {
    _node: DartNode;
    id: number;
    name: string;
    reqLayout: Layout;
    rspLayout: Layout;
    callerCount: number;       /* callers currently matched (from match pushes) */
    _handler: FunctionHandler<Req, Rsp>;

    constructor(node: DartNode, name: string, r: any, handler: FunctionHandler<Req, Rsp>) {
        this._node = node;
        this.id = r.id;
        this.name = name;
        this.reqLayout = new Layout(r.req);
        this.rspLayout = new Layout(r.rsp);
        this.callerCount = 0;
        this._handler = handler;
    }

    _match(m: any): void { this.callerCount = m.callers; }

    async _handle(reqId: number, info: RequestInfo, payload: Uint8Array): Promise<void> {
        let status = 0;
        let rsp = new Uint8Array(0);
        let msg = new Uint8Array(0);
        try {
            const out = await this._handler(this.reqLayout.decode(payload) as Req, info);
            rsp = this.rspLayout.encode(out);
        } catch (e: any) {
            status = 1;   /* app_error; the throw's text becomes the response message */
            const t = typeof e?.message === "string" ? e.message : (typeof e === "string" ? e : "");
            if (t) msg = enc.encode(t).subarray(0, 255);   /* one length byte, like the wire */
        }
        const frame = new Uint8Array(7 + msg.length + rsp.length);
        frame[0] = OP_REQUEST;
        new DataView(frame.buffer).setUint32(1, reqId, true);
        frame[5] = status;
        frame[6] = msg.length;
        frame.set(msg, 7);
        frame.set(rsp, 7 + msg.length);
        this._node._ws.send(frame);
    }
}

type PendingCall<Rsp> = {
    resolve: (r: Response<Rsp>) => void;
    reject: (e: Error) => void;
    layout: Layout;
    timer: ReturnType<typeof setTimeout> | undefined;
    /* task calls only: routes 0x03 progress frames to the run handle */
    progress?: (data: Uint8Array, provider: number, writtenUs: number) => void;
};

/* A reference to a function definition on another node. call() resolves with the
 * outcome and NEVER rejects on a status (only on connection loss). */
class RemoteFunction<Req = any, Rsp = any> {
    _node: DartNode;
    id: number;
    name: string;
    reqLayout: Layout;
    rspLayout: Layout;
    hasDefinition: boolean;    /* a definition is matched (from match pushes) */

    constructor(node: DartNode, name: string, r: any) {
        this._node = node;
        this.id = r.id;
        this.name = name;
        this.reqLayout = new Layout(r.req);
        this.rspLayout = new Layout(r.rsp);
        this.hasDefinition = false;
    }

    _match(m: any): void { this.hasDefinition = !!m.has_definition; }

    /* Call the remote function. timeoutMs > 0 adds a CLIENT-side bound resolving with
     * status "timeout" (the bridge's own call timeout, default 5s, still answers with
     * a wire status when it fires first). */
    call(value: Req, timeoutMs: number = 0): Promise<Response<Rsp>> {
        const payload = this.reqLayout.encode(value);
        const callId = ++this._node._nextCall;
        const frame = new Uint8Array(7 + payload.length);
        frame[0] = OP_CALL;
        frame[1] = this.id & 0xff; frame[2] = this.id >> 8;
        new DataView(frame.buffer).setUint32(3, callId, true);
        frame.set(payload, 7);
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
            this._node._ws.send(frame);
        });
    }
}

/* The implementation side of a task: a function with progress and cancellation. The
 * bridge defers every request here; the async handler streams ctx.progress(...) while it
 * works and its settlement is the one terminal answer (return = "ok", throw the abort
 * reason = "cancelled", any other throw = "app_error"). ONE definition per name (the
 * multi option declares redundant providers). */
class TaskDefinition<Req = any, Prg = any, Rsp = any> {
    _node: DartNode;
    id: number;
    name: string;
    reqLayout: Layout;
    prgLayout: Layout;
    rspLayout: Layout;
    callerCount: number;       /* callers currently matched (from match pushes) */
    _handler: TaskHandler<Req, Prg, Rsp>;
    _aborts: Map<number, AbortController>;   /* req id -> its cancel signal */

    constructor(node: DartNode, name: string, r: any, handler: TaskHandler<Req, Prg, Rsp>) {
        this._node = node;
        this.id = r.id;
        this.name = name;
        this.reqLayout = new Layout(r.req);
        this.prgLayout = new Layout(r.prg);
        this.rspLayout = new Layout(r.rsp);
        this.callerCount = 0;
        this._handler = handler;
        this._aborts = new Map();
    }

    _match(m: any): void { this.callerCount = m.callers; }

    /* an {op:"cancel"} push: abort the request's signal (default AbortError reason) */
    _cancel(reqId: number): void { this._aborts.get(reqId)?.abort(); }

    async _handle(reqId: number, info: RequestInfo, payload: Uint8Array): Promise<void> {
        const ctrl = new AbortController();
        this._aborts.set(reqId, ctrl);
        let done = false;
        const node = this._node;
        const prg = this.prgLayout;
        const ctx: TaskContext<Prg> = {
            progress: (v: Prg) => {
                if (done) throw new Error("task request already completed");
                const p = prg.encode(v);
                const frame = new Uint8Array(5 + p.length);
                frame[0] = OP_PROGRESS;
                new DataView(frame.buffer).setUint32(1, reqId, true);
                frame.set(p, 5);
                node._ws.send(frame);
            },
            signal: ctrl.signal,
            get cancelled(): boolean { return ctrl.signal.aborted; },
            caller: info.caller,
            callerName: info.callerName,
            writtenUs: info.writtenUs,
        };
        let status = 0;
        let rsp = new Uint8Array(0);
        let msg = new Uint8Array(0);
        try {
            const out = await this._handler(this.reqLayout.decode(payload) as Req, ctx);
            rsp = this.rspLayout.encode(out);
        } catch (e: any) {
            /* the abort reason (or any AbortError) after a cancel = the handler honored
             * it; anything else is an app error carrying the throw's text */
            const honored = ctrl.signal.aborted && (e === ctrl.signal.reason || e?.name === "AbortError");
            status = honored ? 5 : 1;
            const t = typeof e?.message === "string" ? e.message : (typeof e === "string" ? e : "");
            if (t) msg = enc.encode(t).subarray(0, 255);   /* one length byte, like the wire */
        }
        done = true;
        this._aborts.delete(reqId);
        const frame = new Uint8Array(7 + msg.length + rsp.length);
        frame[0] = OP_REQUEST;
        new DataView(frame.buffer).setUint32(1, reqId, true);
        frame[5] = status;
        frame[6] = msg.length;
        frame.set(msg, 7);
        frame.set(rsp, 7 + msg.length);
        this._node._ws.send(frame);
    }
}

/* One task invocation in flight, returned synchronously by RemoteTask.call. result is
 * the one terminal Response (never rejecting on a status, only on connection loss);
 * onProgress observes the updates (null = the RUNNING ack; updates arriving before
 * registration are buffered and replayed); cancel() asks the provider to stop. */
class TaskRun<Prg = any, Rsp = any> {
    _node: DartNode;
    _prgLayout: Layout;
    callId: number;            /* the client correlation id (the cancel op's `call`) */
    result: Promise<Response<Rsp>>;
    _onProgress: TaskProgressHandler<Prg> | null;
    _buffered: { value: Prg | null; info: { provider: number; writtenUs: number } }[];

    constructor(node: DartNode, prgLayout: Layout, callId: number, result: Promise<Response<Rsp>>) {
        this._node = node;
        this._prgLayout = prgLayout;
        this.callId = callId;
        this.result = result;
        this._onProgress = null;
        this._buffered = [];
    }

    /* One handler (re-register replaces, null clears); buffered updates replay in order. */
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
        const value = data.length ? (this._prgLayout.decode(data) as Prg) : null;   /* empty = RUNNING */
        if (this._onProgress) this._onProgress(value, { provider, writtenUs });
        else this._buffered.push({ value, info: { provider, writtenUs } });
    }
}

/* A reference to a task definition on another node. call() returns a TaskRun handle
 * SYNCHRONOUSLY; the per-call timeout (remote_task's timeout_ms) bounds only the wait
 * for the first response, so there is no client-side timer: after RUNNING a task runs
 * as long as it runs and run.cancel() is the caller's tool for impatience. */
class RemoteTask<Req = any, Prg = any, Rsp = any> {
    _node: DartNode;
    id: number;
    name: string;
    reqLayout: Layout;
    prgLayout: Layout;
    rspLayout: Layout;
    hasDefinition: boolean;    /* a definition is matched (from match pushes) */

    constructor(node: DartNode, name: string, r: any) {
        this._node = node;
        this.id = r.id;
        this.name = name;
        this.reqLayout = new Layout(r.req);
        this.prgLayout = new Layout(r.prg);
        this.rspLayout = new Layout(r.rsp);
        this.hasDefinition = false;
    }

    _match(m: any): void { this.hasDefinition = !!m.has_definition; }

    call(value: Req): TaskRun<Prg, Rsp> {
        const payload = this.reqLayout.encode(value);
        const callId = ++this._node._nextCall;
        const frame = new Uint8Array(7 + payload.length);
        frame[0] = OP_CALL;
        frame[1] = this.id & 0xff; frame[2] = this.id >> 8;
        new DataView(frame.buffer).setUint32(3, callId, true);
        frame.set(payload, 7);
        let run!: TaskRun<Prg, Rsp>;
        const result = new Promise<Response<Rsp>>((resolve, reject) => {
            const p: PendingCall<Rsp> = { resolve, reject, layout: this.rspLayout, timer: undefined,
                progress: (data, provider, writtenUs) => run._push(data, provider, writtenUs) };
            this._node._calls.set(callId, p);
        });
        run = new TaskRun<Prg, Rsp>(this._node, this.prgLayout, callId, result);
        this._node._ws.send(frame);
        return run;
    }
}

type VarWaiter = { res: (ok: boolean) => void; timer: ReturnType<typeof setTimeout> | undefined };
type VarChangeHandler<T> = (value: T, info: { forced: boolean; writtenUs: number }) => void;

/* Shared variable-handle core: the client-cached latest value fed by pushed updates. */
class VarHandle<T = any> {
    _node: DartNode;
    id: number;
    name: string;
    layout: Layout;
    forced: boolean;
    writtenUs: number;            /* source stamp of the last update pushed (0 = none/unstamped) */
    _value: T | undefined;
    _raw: Uint8Array | undefined;
    _waiters: Set<VarWaiter>;
    _onChange: VarChangeHandler<T> | null;
    _onWrite: VarChangeHandler<T> | null;

    constructor(node: DartNode, name: string, r: any) {
        this._node = node;
        this.id = r.id;
        this.name = name;
        this.layout = new Layout(r);
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
    /* the cached latest raw payload */
    raw(): Uint8Array | undefined { return this._raw; }

    /* Resolve true as soon as a value exists (immediately if cached), false at
     * timeoutMs (negative = wait forever). */
    wait(timeoutMs: number = -1): Promise<boolean> {
        if (this._raw !== undefined) return Promise.resolve(true);
        return new Promise((res) => {
            const w: VarWaiter = { res, timer: undefined };
            if (timeoutMs >= 0) w.timer = setTimeout(() => { this._waiters.delete(w); res(false); }, timeoutMs);
            this._waiters.add(w);
        });
    }

    /* Observe changes: fires per pushed update (the bridge pushes only when the value
     * or forced flag actually changed), and once immediately if a value is already
     * cached, so registering late can never miss the current state. One handler
     * (re-register replaces, null clears). */
    onChange(handler: VarChangeHandler<T> | null): void {
        this._onChange = handler;
        if (handler && this._value !== undefined)
            handler(this._value, { forced: this.forced, writtenUs: this.writtenUs });
    }

    /* Observe EVERY applied write (not just state changes; no replay). Requires the
     * variable to have been created with onWrite:true so the bridge pushes them. One
     * handler (re-register replaces, null clears). */
    onWrite(handler: VarChangeHandler<T> | null): void { this._onWrite = handler; }

    set(value: T): void { this._sendVar(0, this.layout.encode(value)); }
    force(value: T): void { this._sendVar(1, this.layout.encode(value)); }
    unforce(): void { this._sendVar(2, new Uint8Array(0)); }

    _sendVar(mode: number, payload: Uint8Array): void {
        const frame = new Uint8Array(4 + payload.length);
        frame[0] = OP_VAR;
        frame[1] = this.id & 0xff; frame[2] = this.id >> 8;
        frame[3] = mode;
        frame.set(payload, 4);
        this._node._ws.send(frame);
    }

    _update(payload: Uint8Array, forced: boolean, writtenUs: number): void {
        this._raw = payload;
        this._value = this.layout.decode(payload) as T;
        this.forced = forced;
        this.writtenUs = writtenUs;
        for (const w of this._waiters) { if (w.timer !== undefined) clearTimeout(w.timer); w.res(true); }
        this._waiters.clear();
        if (this._onChange) this._onChange(this._value, { forced, writtenUs });
    }
    /* a write-event frame (bit1 set): fire onWrite only; the cache is maintained by the
     * on_change frames, so a write is not double-counted. */
    _write(payload: Uint8Array, forced: boolean, writtenUs: number): void {
        if (this._onWrite) this._onWrite(this.layout.decode(payload) as T, { forced, writtenUs });
    }
    _match(_m: any): void {}
}

/* The authoritative value lives on THIS node (held by the bridge). */
class VariableDefinition<T = any> extends VarHandle<T> {
    remoteCount: number;       /* remote accessors matched (from match pushes) */

    constructor(node: DartNode, name: string, r: any) {
        super(node, name, r);
        this.remoteCount = 0;
    }
    _match(m: any): void { this.remoteCount = m.remotes; }
}

/* The value lives on another node; reads see the cached latest, writes go over the
 * set channel (dumb writes, no response). */
class RemoteVariable<T = any> extends VarHandle<T> {
    hasDefinition: boolean;    /* an owner is matched (from match pushes) */

    constructor(node: DartNode, name: string, r: any) {
        super(node, name, r);
        this.hasDefinition = false;
    }
    _match(m: any): void { this.hasDefinition = !!m.has_definition; }
}

type PatternEntity = FunctionDefinition | RemoteFunction | TaskDefinition | RemoteTask | VarHandle;

/* The node handle: one WebSocket connection = one DART node owned by the bridge. */
class DartNode {
    _ws: WebSocket;
    _seq: number;
    _pending: Map<number, { resolve: (r: any) => void; reject: (e: Error) => void }>;
    _topics: Map<number, DartTopic>;
    _entities: Map<number, PatternEntity>;
    _reqMeta: Map<number, RequestInfo>;
    _calls: Map<number, PendingCall<any>>;
    _nextCall: number;
    _closing: boolean;
    name: string;                                /* this node's name (auto-generated if none given) */
    onEvent: ((e: DartEvent) => void) | null;    /* every bridge event (errors, peer up/down, msg loss) */
    onClose: ((e: CloseEvent) => void) | null;
    _onLog: ((l: LogLine) => void) | null;       /* mesh log-stream handler (set by onLog) */

    /* Connect to a bridge and open the node. */
    static async connect(url: string, opts: NodeOpts = {}): Promise<DartNode> {
        const ws = new WebSocket(url);
        ws.binaryType = "arraybuffer";
        await new Promise<unknown>((res, rej) => {
            ws.onopen = res;
            ws.onerror = () => rej(new Error(`connect failed: ${url}`));
        });
        const c = new DartNode(ws);
        const { onEvent, ...open } = opts;
        if (onEvent) c.onEvent = onEvent;
        const r = await c._request({ op: "open", ...open });
        c.name = r.name as string;
        return c;
    }

    constructor(ws: WebSocket) {
        this._ws = ws;
        this._seq = 0;
        this._pending = new Map();
        this._topics = new Map();
        this._entities = new Map();
        this._reqMeta = new Map();
        this._calls = new Map();
        this._nextCall = 0;
        this._closing = false;
        this.name = "";
        this.onEvent = null;
        this.onClose = null;
        this._onLog = null;

        ws.onmessage = (e: MessageEvent) => {
            if (typeof e.data === "string") this._onText(JSON.parse(e.data));
            else this._onBinary(e.data as ArrayBuffer);
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

    _onText(m: any): void {
        if (m.op === "reply") {
            const p = this._pending.get(m.seq);
            if (!p) return;
            this._pending.delete(m.seq);
            if (m.ok) p.resolve(m);
            else p.reject(new Error(m.error ?? "request failed"));
        } else if (m.op === "event") {
            this.onEvent?.(m as DartEvent);
        } else if (m.op === "match") {
            if (m.type === "topic") this._topics.get(m.id)?._match(m);
            else this._entities.get(m.id)?._match(m);
        } else if (m.op === "request") {
            /* meta first; the binary payload frame follows on the same ordered socket */
            this._reqMeta.set(m.req, { caller: m.caller, callerName: m.caller_name ?? "", writtenUs: 0 });
        } else if (m.op === "cancel") {
            /* cancellation requested on a parked task request: abort its signal */
            const t = this._entities.get(m.task);
            if (t instanceof TaskDefinition) t._cancel(m.req);
        } else if (m.op === "log") {
            this._onLog?.({ level: m.level, node: m.node, wallUs: m.wall_us,
                            monoUs: m.mono_us, recvUs: m.recv_us, writtenUs: m.written_us ?? 0,
                            text: m.text });
        }
    }

    _onBinary(buf: ArrayBuffer): void {
        const b = new Uint8Array(buf);
        if (b.length < 3) return;
        const view = new DataView(buf);
        switch (b[0]) {
        case OP_DATA: {                      /* [u16 topic][u32 publisher][u64 written][payload] */
            if (b.length < 15) return;
            const ch = this._topics.get(view.getUint16(1, true));
            ch?._deliver(view.getUint32(3, true), b.subarray(15), rdWrittenUs(view, 7));
            return;
        }
        case OP_VAR: {                       /* [u16 ent][u8 flags][u64 written][payload]; flags bit0=forced bit1=write-event */
            if (b.length < 12) return;
            const v = this._entities.get(view.getUint16(1, true));
            if (v instanceof VarHandle) {
                const forced = (b[3] & 1) !== 0;
                const writtenUs = rdWrittenUs(view, 4);
                if (b[3] & 2) v._write(b.subarray(12), forced, writtenUs);
                else          v._update(b.subarray(12), forced, writtenUs);
            }
            return;
        }
        case OP_PROGRESS: {                  /* [u32 call][u32 provider][u64 written][payload]; empty payload = RUNNING */
            if (b.length < 17) return;
            const p = this._calls.get(view.getUint32(1, true));
            p?.progress?.(b.subarray(17), view.getUint32(5, true), rdWrittenUs(view, 9));
            return;
        }
        case OP_CALL: {                      /* [u32 call][u8 status][u32 provider][u64 written][u8 msg_len][msg][payload] */
            if (b.length < 19) return;
            const callId = view.getUint32(1, true);
            const p = this._calls.get(callId);
            if (!p) return;                  /* client-side timeout already settled it */
            this._calls.delete(callId);
            if (p.timer !== undefined) clearTimeout(p.timer);
            const status = CALL_STATUS[b[5]] ?? "cancelled";
            const ml = b[18];
            if (b.length < 19 + ml) return;
            const data = b.subarray(19 + ml);
            p.resolve({
                ok: status === "ok",
                status,
                value: status === "ok" ? p.layout.decode(data) : undefined,
                data,
                provider: view.getUint32(6, true),
                writtenUs: rdWrittenUs(view, 10),
                message: ml ? dec.decode(b.subarray(19, 19 + ml)) : CALL_STATUS_TEXT[status],
            });
            return;
        }
        case OP_REQUEST: {                   /* [u16 ent][u32 req][u64 written][payload] */
            if (b.length < 15) return;
            const fn = this._entities.get(view.getUint16(1, true));
            const reqId = view.getUint32(3, true);
            const info = this._reqMeta.get(reqId) ?? { caller: 0, callerName: "", writtenUs: 0 };
            info.writtenUs = rdWrittenUs(view, 7);   /* the caller's stamp rides the binary frame */
            this._reqMeta.delete(reqId);
            if (fn instanceof FunctionDefinition)   void fn._handle(reqId, info, b.subarray(15));
            else if (fn instanceof TaskDefinition)  void fn._handle(reqId, info, b.subarray(15));
            return;
        }
        default: return;                     /* reserved ops: ignore */
        }
    }

    /* Create a topic (the dynamic form). Pass opts.schema (DSL text) for a typed
     * topic; omit it for a raw bytes topic. */
    async topic(name: string, role: Role = "pubsub", opts: TopicOpts = {}): Promise<DartTopic> {
        const r = await this._request({ op: "topic", name, role, ...opts });
        const ch = new DartTopic(this, name, r);
        this._topics.set(ch.id, ch);
        return ch;
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

    /* Host a function: handler(reqValue) returns the reply value (may be async; a
     * throw answers "app_error"). Schemas are DSL text (null = raw bytes). */
    async functionDefinition<Req = any, Rsp = any>(name: string, reqSchema: string | null,
            rspSchema: string | null, handler: FunctionHandler<Req, Rsp>): Promise<FunctionDefinition<Req, Rsp>> {
        const r = await this._request({
            op: "function_definition", name,
            ...(reqSchema ? { req_schema: reqSchema } : {}),
            ...(rspSchema ? { rsp_schema: rspSchema } : {}),
        });
        const fn = new FunctionDefinition<Req, Rsp>(this, name, r, handler);
        this._entities.set(fn.id, fn);
        return fn;
    }

    /* A reference to a function hosted elsewhere. */
    async remoteFunction<Req = any, Rsp = any>(name: string, reqSchema: string | null,
            rspSchema: string | null): Promise<RemoteFunction<Req, Rsp>> {
        const r = await this._request({
            op: "remote_function", name,
            ...(reqSchema ? { req_schema: reqSchema } : {}),
            ...(rspSchema ? { rsp_schema: rspSchema } : {}),
        });
        const fn = new RemoteFunction<Req, Rsp>(this, name, r);
        this._entities.set(fn.id, fn);
        return fn;
    }

    /* Host a task (a function with progress and cancellation): handler(reqValue, ctx)
     * streams ctx.progress(...) while it works; its (possibly async) settlement is the
     * one terminal answer, and ctx.signal aborts when the caller requests cancellation.
     * Schemas are DSL text (null = raw bytes) for request, progress and response. */
    async taskDefinition<Req = any, Prg = any, Rsp = any>(name: string, reqSchema: string | null,
            prgSchema: string | null, rspSchema: string | null, handler: TaskHandler<Req, Prg, Rsp>,
            opts: TaskOpts = {}): Promise<TaskDefinition<Req, Prg, Rsp>> {
        const r = await this._request({
            op: "task_definition", name, ...opts,
            ...(reqSchema ? { req_schema: reqSchema } : {}),
            ...(prgSchema ? { prg_schema: prgSchema } : {}),
            ...(rspSchema ? { rsp_schema: rspSchema } : {}),
        });
        const t = new TaskDefinition<Req, Prg, Rsp>(this, name, r, handler);
        this._entities.set(t.id, t);
        return t;
    }

    /* A reference to a task hosted elsewhere. task.call(req) returns a TaskRun handle
     * synchronously: run.onProgress(cb), await run.result, run.cancel(). */
    async remoteTask<Req = any, Prg = any, Rsp = any>(name: string, reqSchema: string | null,
            prgSchema: string | null, rspSchema: string | null,
            opts: TaskOpts = {}): Promise<RemoteTask<Req, Prg, Rsp>> {
        const r = await this._request({
            op: "remote_task", name, ...opts,
            ...(reqSchema ? { req_schema: reqSchema } : {}),
            ...(prgSchema ? { prg_schema: prgSchema } : {}),
            ...(rspSchema ? { rsp_schema: rspSchema } : {}),
        });
        const t = new RemoteTask<Req, Prg, Rsp>(this, name, r);
        this._entities.set(t.id, t);
        return t;
    }

    /* Host a variable (this node holds the authoritative value). `initial` is applied
     * with a set right after the create (the client owns encoding, and encoding needs
     * the field table the create returns). */
    async variableDefinition<T = any>(name: string, schema: string | null,
            opts: VariableDefOpts<T> = {}): Promise<VariableDefinition<T>> {
        const r = await this._request({
            op: "variable_definition", name,
            ...(schema ? { schema } : {}),
            ...(opts.readOnly ? { read_only: true } : {}),
            ...(opts.allowForce ? { allow_force: true } : {}),
            ...(opts.catch_up ? { catch_up: opts.catch_up } : {}),
            ...(opts.backpressure_wait_ms ? { backpressure_wait_ms: opts.backpressure_wait_ms } : {}),
            ...(opts.onWrite ? { on_write: true } : {}),
        });
        const v = new VariableDefinition<T>(this, name, r);
        this._entities.set(v.id, v);
        if (opts.initial !== undefined) v.set(opts.initial);
        return v;
    }

    /* Access a variable owned elsewhere. Pass { onWrite: true } to also receive every
     * applied write (route it via RemoteVariable.onWrite). */
    async remoteVariable<T = any>(name: string, schema: string | null,
            opts: RemoteVarOpts = {}): Promise<RemoteVariable<T>> {
        const r = await this._request({ op: "remote_variable", name,
            ...(schema ? { schema } : {}), ...(opts.onWrite ? { on_write: true } : {}) });
        const v = new RemoteVariable<T>(this, name, r);
        this._entities.set(v.id, v);
        return v;
    }

    /* Block until discovery + matching settle for everything created so far. */
    async settle(timeoutMs: number = -1): Promise<boolean> {
        const r = await this._request({ op: "settle", timeout_ms: timeoutMs });
        return r.settled as boolean;
    }

    /* Publish a line on a level's built-in @dart/log topic (mesh-wide, rosout-style).
     * Every node that subscribed to that level receives it. */
    async log(level: LogLevelName, text: string): Promise<void> {
        await this._request({ op: "log", level, text });
    }
    logError(text: string): Promise<void> { return this.log("error", text); }
    logWarn (text: string): Promise<void> { return this.log("warn",  text); }
    logInfo (text: string): Promise<void> { return this.log("info",  text); }

    /* Subscribe to the mesh's log stream at the given levels (default all three). The
     * handler fires for every OTHER node's lines at those levels (never this node's own),
     * decoded to a LogLine; late-join history (keep_last per writer) replays on match.
     * One handler for all subscribed levels (call again to widen the set). */
    async onLog(handler: (line: LogLine) => void,
                levels: LogLevelName[] = ["error", "warn", "info"]): Promise<void> {
        this._onLog = handler;
        await this._request({ op: "log_subscribe", levels });
    }

    /* ---- introspection (query-based, pull-only) ---------------------------------- */

    /* Snapshot the discovered peer table (a local read; resolves immediately). */
    async peers(): Promise<Peer[]> {
        const r = await this._request({ op: "peers" });
        return (r.peers as any[]).map((p) => ({
            id: p.id, name: p.name, address: p.address, active: p.active, fragmentSize: p.fragment_size }));
    }

    /* The entities THIS node hosts (its functions and variables, then its topics). */
    async entities(): Promise<Entity[]> {
        const r = await this._request({ op: "entities" });
        return (r.entities as any[]).map(toEntity);
    }

    /* What one peer advertises, folded into entities (a local read of this node's view;
     * names need fetch_details or a shared topic to resolve past the hash placeholder).
     * A dropped (silent, resumable) peer yields [] by default: its cached entities are
     * its dead incarnation's. includeDropped serves that last-known view anyway. */
    async peerEntities(peerId: number, includeDropped: boolean = false): Promise<Entity[]> {
        const r = await this._request({ op: "peer_entities", peer: peerId,
                                        include_dropped: includeDropped });
        return (r.entities as any[]).map(toEntity);
    }

    /* Fetch a peer's @dart/meta snapshot (an async directed call; works under the
     * bridge's service thread). Never rejects on status: inspect the returned `status`.
     * sections = OR of MetaSection (default All). */
    async meta(peerId: number, sections: number = MetaSection.All): Promise<MetaSnapshot> {
        const r = await this._request({ op: "meta", peer: peerId, sections });
        return { valid: !!r.valid, status: CALL_STATUS[r.status] ?? "cancelled",
                 provider: r.provider ?? 0, info: r.info ?? {} };
    }

    /* Close the connection; the bridge closes the node with a BYE. Outstanding call
     * promises settle with status "cancelled". */
    close(): void {
        this._closing = true;
        this._ws.close();
    }
}

export {
    DartNode, DartTopic, DartMessage, Layout,
    Publisher, Subscriber,
    FunctionDefinition, RemoteFunction,
    TaskDefinition, RemoteTask, TaskRun,
    VariableDefinition, RemoteVariable,
    MetaSection,
    type Field, type SchemaBlock, type Role, type TopicOpts, type NodeOpts, type DartEvent,
    type CallStatusName, type Response, type RequestInfo,
    type SubscriberHandler, type FunctionHandler,
    type TaskContext, type TaskHandler, type TaskProgressHandler, type TaskOpts, type CancelStatus,
    type VariableDefOpts, type RemoteVarOpts,
    type LogLevelName, type LogLine,
    type Peer, type Entity, type EntityKindName, type MetaSnapshot,
};
