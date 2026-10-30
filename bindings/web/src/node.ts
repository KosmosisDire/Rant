/* The node and its handles over Rant built to wasm. The binding drives its nodes itself: every
 * callback parks on the node's queue and runs after a poll, where the whole API may be used.
 * docs/javascript.md shows the API, spec/bindings.md the rules every binding keeps. */

import * as L from "./layout.js";
import { decode, encode, namesOf, SchemaError, Table, type FieldInfo, type Names } from "./codec.js";
import type { NetObject } from "./net.js";
import { dsl, type Infer, type Schema } from "./schema.js";
import { encodeUtf8, Native, scoped, type Scope } from "./wasm.js";

/* ---- public types ------------------------------------------------------------------------- */

export type SendStatus = "ok" | "no_topic" | "too_big" | "role" | "oom" | "state" | "nosys" | "schema";
export type CallStatus = "ok" | "app_error" | "no_handler" | "timeout" | "peer_lost" | "cancelled" | "running" | "no_provider";
export type CancelStatus = "ok" | "no_cancel" | "not_pending";
/* the severity of a log line or a status condition, rising */
export type Level = "debug" | "info" | "warn" | "error";
/* who wrote a log line or a condition */
export type Origin = "app" | "rant";
export type EntityKind = "topic" | "function" | "variable" | "task";
export type EventKind = "peer_up" | "peer_down" | "peer_interest" | "msg_lost" | "error" | "status_cleared";
export type ErrorKind =
    | "none" | "name_collision" | "qos_incompatible" | "kind_mismatch" | "schema_mismatch" | "interest_overflow"
    | "meta_truncated_interest" | "peer_meta_too_big" | "msg_too_big" | "peer_refused" | "evicted_unsent"
    | "unmatched_send" | "duplicate_authority" | "oom" | "platform" | "socket" | "bind" | "mcast_join" | "send"
    | "recv" | "poll" | "waker" | "bad_address" | "bad_name" | "state" | "bad_schema" | "bad_domain"
    | "bad_prefix" | "bad_log_level" | "wire_version" | "bridge";

const SEND: Record<number, SendStatus> = { 0: "ok", [-1]: "no_topic", [-2]: "too_big", [-3]: "role", [-4]: "oom", [-5]: "state", [-6]: "nosys", [-7]: "schema" };
const CALL: CallStatus[] = ["ok", "app_error", "no_handler", "timeout", "peer_lost", "cancelled", "running", "no_provider"];
const CALL_NUMBER: Record<CallStatus, number> = { ok: 0, app_error: 1, no_handler: 2, timeout: 3, peer_lost: 4, cancelled: 5, running: 6, no_provider: 7 };
const LEVELS: Level[] = ["debug", "info", "warn", "error"];      /* the C numbers them from 1 */
/* a decoded enum is its option name, a number past the known ones reads as the nearest */
const levelOf = (v: unknown): Level => typeof v === "string" ? (v.toLowerCase() as Level)
    : LEVELS[Math.min(Math.max(Number(v ?? 2), 1), 4) - 1];
const originOf = (v: unknown): Origin => (typeof v === "string" ? v.toLowerCase() === "rant" : Number(v ?? 0) === 1)
    ? "rant" : "app";
const ENTITY: EntityKind[] = ["topic", "function", "variable", "task"];
const EVENT: EventKind[] = ["peer_up", "peer_down", "peer_interest", "msg_lost", "error", "status_cleared"];
const ERROR: ErrorKind[] = [
    "none", "name_collision", "qos_incompatible", "kind_mismatch", "schema_mismatch", "interest_overflow",
    "meta_truncated_interest", "peer_meta_too_big", "msg_too_big", "peer_refused", "evicted_unsent",
    "unmatched_send", "duplicate_authority", "oom", "platform", "socket", "bind", "mcast_join", "send", "recv",
    "poll", "waker", "bad_address", "bad_name", "state", "bad_schema", "bad_domain", "bad_prefix", "bad_log_level",
    "wire_version", "bridge"];
const sendStatus = (r: number): SendStatus => SEND[r] ?? "state";

/* The section mask of a meta snapshot, 0 for all of them. */
export const MetaSection = { node: 1, proc: 2, topics: 4, peers: 8 } as const;

/* A peer, loss or error notification, flat. Fields the kind does not set are zero or null. */
export interface RantEvent {
    kind: EventKind; error: ErrorKind;
    /* the one line text, as rant_event_str spells it */
    text: string;
    peer: number; peerName: string | null; topic: number; topicName: string | null; osError: number;
    /* "ip:port" of the peer, null when the event names none */
    address: string | null;
    lostFirst: number; lostCount: number; tooBigBytes: number; identity: bigint;
    publishTopics: number; receiveTopics: number; detail: string | null;
}

export interface NodeOptions {
    /* where the sockets come from: TunnelNet in a page, UdpNet or nothing for this machine's own in Node.js */
    net?: NetObject;
    domain?: number; prefix?: string; maxTopics?: number; fetchDetails?: boolean;
    disableLogs?: boolean; disableStatus?: boolean; disableMeta?: boolean; disableErrorLogs?: boolean;
    /* the lowest level log() sends, unset takes RANT_LOG_LEVEL in Node.js, else "info" */
    logLevel?: Level;
    /* "127.0.0.1" keeps the node on this computer */
    multicastInterface?: string;
    dataPort?: number; discoveryPort?: number; multicastTtl?: number;
    /* "ip" or "ip:port" */
    seedPeers?: string[];
    fragmentSize?: number;
    recvBufferBytes?: number; sendBufferBytes?: number;
    announceIntervalMs?: number; peerTimeoutMs?: number; maxPeers?: number;
    onEvent?: (e: RantEvent) => void;
}

export interface TopicOptions {
    reliable?: boolean; keepLast?: number; catchUp?: number; maxMessageBytes?: number;
    heartbeatMs?: number; repairDelayMs?: number; backpressureWaitMs?: number; queueBytes?: number;
    maxRateHz?: number; noTimestamp?: boolean;
    /* take what was left out from the mesh: a null schema and the reliability */
    reflectFromMesh?: boolean;
}
export interface FunctionOptions {
    timeoutMs?: number; backpressureWaitMs?: number; keepLast?: number; multi?: boolean; reflectFromMesh?: boolean;
}
export interface TaskOptions {
    progressBestEffort?: boolean; progressKeepLast?: number; noCancel?: boolean; exclusive?: boolean; multi?: boolean;
    timeoutMs?: number; backpressureWaitMs?: number; keepLast?: number; reflectFromMesh?: boolean;
}
export interface VariableOptions<T> {
    /* a definition's value before any set */
    initial?: T;
    readOnly?: boolean; allowForce?: boolean; catchUp?: number; keepLast?: number;
    backpressureWaitMs?: number; reflectFromMesh?: boolean;
}

/* A delivered message, copied out of the callback. */
export interface Message<T> {
    value: T; data: Uint8Array; publisherId: number; publisherName: string; topicName: string;
    recvUs: number; writtenUs: number; captureUs: number;
}

/* Who called a function or task. */
export interface Request {
    caller: number; callerName: string; functionName: string; recvUs: number; writtenUs: number; data: Uint8Array;
}

/* What a task handler works through. Throw CancelledError, or the signal's reason, to answer cancelled. */
export interface TaskContext<Prg> extends Request {
    progress(value: Prg): SendStatus;
    readonly cancelled: boolean;
    readonly signal: AbortSignal;
}

/* A call's outcome. value holds whatever came back, a partial result or failure data too. */
export interface Response<T> {
    ok: boolean; status: CallStatus;
    /* why the call was never sent, "ok" when it was */
    sendStatus: SendStatus;
    value: T | undefined; hasValue: boolean; data: Uint8Array;
    /* the outcome text to show, the status's own when the provider gave none */
    message: string;
    provider: number; writtenUs: number;
}

/* One task progress update, value null for the RUNNING acknowledgment. */
export interface Progress<T> {
    value: T | null; data: Uint8Array; callId: number; provider: number; writtenUs: number; recvUs: number;
}

export interface VariableUpdate<T> {
    value: T; data: Uint8Array; forced: boolean; writeSeq: number;
    /* the peer the write came from, 0 for this node */
    source: number;
    recvUs: number; writtenUs: number;
}

export interface TopicCounts { txMsgs: number; txBytes: number; rxMsgs: number; rxBytes: number }
export interface QueueStats { messages: number; bytes: number; capacity: number; dropped: number }
export interface NodeStats {
    evictedUnsent: number; memInUse: number; memPeak: number; allocCalls: number;
    backpressureWaitedUs: number; backpressureWaits: number;
}

/* fragmentSize is 0 while the peer shares nothing with this node */
export interface Peer {
    id: number; uuid: string; name: string; address: string; active: boolean; lastHeardUs: number; epoch: number;
    catchingUp: boolean; fragmentSize: number; rttUs: number; rttJitterUs: number; rttMinUs: number; rttSamples: number;
}

export interface Entity {
    kind: EntityKind; name: string; hash: number;
    provides: boolean; consumes: boolean; reliable: boolean; writable: boolean; forceable: boolean;
    cancellable: boolean; exclusive: boolean; multi: boolean; incomplete: boolean; conflict: boolean;
    providers: number; consumers: number; provider: number;
    /* the node the schemas were read from */
    from: string;
    schema: CompiledSchema | null; schemaHash: bigint;
    rspSchema: CompiledSchema | null; rspSchemaHash: bigint;
    progressSchema: CompiledSchema | null; progressSchemaHash: bigint;
    generation: bigint;
}

export interface LogLine {
    level: Level; origin: Origin; node: string; nodeId: number; text: string;
    /* the sender's UTC clock when it logged, and this node's monotonic clock at receipt */
    wallUs: number; recvUs: number;
}

/* One active condition in a node's status. sinceUs is the owner's UTC clock at the raise. */
export interface Condition {
    key: string; level: Level; origin: Origin; clearable: boolean; sinceUs: number; text: string;
}

/* A node's whole status: every condition active there now, empty when it cleared them all or went away. */
export interface StatusUpdate { node: string; nodeId: number; conditions: Condition[] }

export interface MetaSnapshot { valid: boolean; status: CallStatus; provider: number; info: Record<string, unknown> }

export class RantError extends Error {
    constructor(message: string, readonly event?: RantEvent) { super(message); this.name = "RantError"; }
}

/* Thrown by a task handler to answer cancelled, with an optional partial result. */
export class CancelledError<T = unknown> extends Error {
    constructor(message = "cancelled", readonly partial?: T) { super(message); this.name = "CancelledError"; }
}

/* Thrown by a handler to answer app_error with data beside the message. */
export class AppError<T = unknown> extends Error {
    constructor(message: string, readonly value?: T) { super(message); this.name = "AppError"; }
}

/* What a handle's schema argument takes: a builder, DSL text, a compiled schema, or null for raw bytes. */
export type SchemaArg = Schema<any> | string | CompiledSchema | null;
type Val<S> = S extends Schema<any> ? Infer<S> : S extends null ? Uint8Array : unknown;

/* ---- the node ----------------------------------------------------------------------------- */

/* The C library's version, such as "0.0.40". A promise, since the wasm loads first. */
export function version(): Promise<string> { return Native.version(); }

/* Opens a node. In Node.js without a net it runs on this machine's UDP. The async step. */
export async function openNode(name?: string | null, opts: NodeOptions = {}): Promise<RantNode> {
    let net = opts.net, release: (() => void) | null = null;
    if (!net) ({ net, release } = await defaultNet(opts.multicastInterface === "127.0.0.1" ? "127.0.0.1" : "0.0.0.0"));
    try {
        return new RantNode(await Native.load(net), name ?? null, opts, release);
    } catch (e) {
        release?.();
        throw e;
    }
}

const defaults = new Map<string, { net: Promise<NetObject & { shutdown(): void }>; users: number }>();

/* The UdpNet nodes share when given none, shut once its last node closes so the process may end. */
async function defaultNet(address: string): Promise<{ net: NetObject; release: () => void }> {
    if (typeof process === "undefined" || !process.versions?.node)
        throw new RantError("a page reaches the mesh through a bridge: pass net, such as await TunnelNet.connect(url)");
    let d = defaults.get(address);
    if (!d) {
        /* by URL, so a bundle for a page never pulls in node:dgram */
        const udp = new URL("./udp.js", import.meta.url).href;
        d = { net: import(udp).then((m) => m.UdpNet.create({ address })), users: 0 };
        defaults.set(address, d);
    }
    const entry = d;
    entry.users++;
    let net: NetObject & { shutdown(): void };
    try { net = await entry.net; }
    catch (e) {
        if (--entry.users === 0) defaults.delete(address);
        throw e;
    }
    let done = false;
    return {
        net, release: () => {
            if (done) return;
            done = true;
            if (--entry.users === 0) { defaults.delete(address); net.shutdown(); }
        },
    };
}

interface TopicRec {
    h: number; index: number; name: string; schema: number; pull: boolean; pubs: number; subs: number;
    subscribers: Set<Subscriber<any>>;
}

interface Waiter { done: () => boolean; resolve: (ok: boolean) => void; timer: ReturnType<typeof setTimeout> | null }

export class RantNode {
    readonly reflection: Reflection;
    /** @internal */ readonly n: Native;
    /** @internal */ h = 0;
    /** @internal */ queue = 0;
    private readonly id: number;
    private eventHandler: ((e: RantEvent) => void) | null;
    private logHandler: ((l: LogLine) => void) | null = null;
    private logIndex = -1;
    private statusHandler: ((u: StatusUpdate) => void) | null = null;
    private statusIndex = -1;
    /* peers whose last status set was not empty */
    private statusPeers = new Set<number>();
    private topics = new Map<string, TopicRec>();
    private byIndex = new Map<number, TopicRec>();
    private tables = new Map<number, Table>();
    private compiled = new Map<number, CompiledSchema>();
    private handles = new Set<Handle>();
    private boxes = new Set<number>();
    private waiters = new Set<Waiter>();
    private later: (() => void)[] = [];
    /* calls in flight, settled as cancelled if the C never answers them */
    private calls = new Set<() => void>();
    private readonly settleMs: number;

    /** @internal */
    constructor(n: Native, readonly name: string | null, opts: NodeOptions, private release: (() => void) | null) {
        this.n = n;
        this.reflection = new Reflection(this);
        this.eventHandler = opts.onEvent ?? null;
        this.settleMs = 3 * (opts.announceIntervalMs ?? 3000);
        this.id = this.box((p: number, kind: string) => kind === "event" ? this.emit(eventFrom(n, p)) : this.onMessage(p));
        const h = scoped(n, (sc) => {
            const o = sc.alloc(L.RantNodeOpts.sizeof), N = L.RantNodeOpts, net = o + N.net, NN = L.RantNodeNet;
            const disc = o + N.discovery, D = L.RantNodeDiscovery;
            const str = (p: number, s?: string) => { if (s) n.setU32(p, sc.cstr(s)); };
            n.setU16(o + N.domain, opts.domain ?? 0);
            n.setU16(o + N.max_topics, opts.maxTopics ?? 0);
            n.setU32(o + N.user_data, this.id);
            n.setU8(o + N.disable_shm, 1);
            n.setU8(o + N.fetch_details, opts.fetchDetails ? 1 : 0);
            n.setI32(o + N.match_wait_ms, -1);          /* a page never blocks: settle() instead */
            n.setU8(o + N.disable_logs, opts.disableLogs ? 1 : 0);
            n.setU8(o + N.disable_meta, opts.disableMeta ? 1 : 0);
            n.setU8(o + N.disable_error_logs, opts.disableErrorLogs ? 1 : 0);
            n.setU8(o + N.log_level, opts.logLevel ? LEVELS.indexOf(opts.logLevel) + 1 : 0);
            n.setU8(o + N.disable_status, opts.disableStatus ? 1 : 0);
            str(o + N.prefix, opts.prefix);
            n.setU16(net + NN.data_port, opts.dataPort ?? 0);
            n.setU16(net + NN.discovery_port, opts.discoveryPort ?? 0);
            str(net + NN.multicast_interface, opts.multicastInterface);
            n.setU8(net + NN.multicast_ttl, opts.multicastTtl ?? 0);
            const seeds = opts.seedPeers ?? [];
            if (seeds.length) {
                const arr = sc.alloc(seeds.length * L.RantAddr.sizeof);
                seeds.forEach((s, i) => writeAddr(n, arr + i * L.RantAddr.sizeof, s));
                n.setU32(net + NN.seed_peers, arr);
                n.setU16(net + NN.n_seed_peers, seeds.length);
            }
            n.setU32(net + NN.recv_buffer_bytes, opts.recvBufferBytes ?? 0);
            n.setU32(net + NN.send_buffer_bytes, opts.sendBufferBytes ?? 0);
            n.setU16(net + NN.fragment_size, opts.fragmentSize ?? 0);
            n.setU32(disc + D.announce_interval_us, (opts.announceIntervalMs ?? 0) * 1000);
            n.setU32(disc + D.peer_timeout_us, (opts.peerTimeoutMs ?? 0) * 1000);
            n.setU16(disc + D.max_peers, opts.maxPeers ?? 0);
            const alloc = sc.alloc(L.RantAllocator.sizeof);
            n.c.rant_allocator_heap(alloc, 0);
            return n.c.rant_node_open(alloc, name ? sc.cstr(name) : 0, n.fn.message, n.fn.event, o);
        });
        if (!h) {
            this.unboxAll();
            const e = lastError(n, 0);
            throw new RantError(`openNode failed: ${e.text}`, e);
        }
        this.h = h;
        this.queue = n.c.rant_node_create_queue(h);
        if (!this.queue || n.c.rant_node_set_event_queue(h, this.queue) !== 0) {
            n.c.rant_node_close(h, 0);
            this.h = 0;
            this.unboxAll();
            throw new RantError("openNode failed: the callback queue was refused");
        }
        n.drive(this.drive);
    }

    get closed(): boolean { return this.h === 0; }

    /* Every peer, loss and error event, where the callbacks run. null prints the errors. */
    onEvent(handler: ((e: RantEvent) => void) | null): void { this.eventHandler = handler; }

    /* ---- driving ---- */

    private drive = (): void => {
        if (!this.h) return;
        const n = this.n;
        try {
            n.c.rant_node_poll(this.h, 60000);
            n.dispatching++;
            try { n.c.rant_queue_dispatch(this.queue, 0, 0); }
            finally { n.dispatching--; }
        } catch (e) {
            console.error("rant: the node's drive failed:", e);
        }
        for (const f of this.later.splice(0)) f();
        for (const w of [...this.waiters]) if (w.done()) this.endWait(w, true);
    };

    /** @internal */ defer(f: () => void): boolean {
        if (!this.n.dispatching) return false;
        this.later.push(f);
        this.n.wake();
        return true;
    }

    /** @internal */ until(done: () => boolean, timeoutMs: number): Promise<boolean> {
        if (!this.h) return Promise.resolve(false);
        if (done()) return Promise.resolve(true);
        return new Promise((resolve) => {
            const w: Waiter = { done, resolve, timer: null };
            if (timeoutMs >= 0) w.timer = setTimeout(() => this.endWait(w, !!this.h && w.done()), timeoutMs);
            this.waiters.add(w);
            this.n.wake();
        });
    }

    private endWait(w: Waiter, ok: boolean): void {
        if (!this.waiters.delete(w)) return;
        if (w.timer) clearTimeout(w.timer);
        w.resolve(ok);
    }

    /** @internal */ box(f: (arg: any, kind: string) => void): number {
        const id = this.n.box(f);
        this.boxes.add(id);
        return id;
    }
    /** @internal */ unbox(id: number): void {
        this.boxes.delete(id);
        this.n.unbox(id);
    }
    private unboxAll(): void {
        for (const id of this.boxes) this.n.unbox(id);
        this.boxes.clear();
    }

    /** @internal */ track(h: Handle): void { this.handles.add(h); }
    /** @internal */ untrack(h: Handle): void { this.handles.delete(h); }
    /** @internal */ inFlight(settle: () => void): () => void {
        this.calls.add(settle);
        return () => this.calls.delete(settle);
    }

    /** @internal */ live(): number {
        if (!this.h) throw new RantError("the node is closed");
        return this.h;
    }

    /* ---- callbacks ---- */

    /** @internal */ emit(e: RantEvent): void {
        if (e.kind === "peer_down" && this.statusPeers.delete(e.peer))   /* its conditions went with it */
            this.deliverStatus({ node: e.peerName ?? "", nodeId: e.peer, conditions: [] });
        if (this.eventHandler) {
            try { this.eventHandler(e); }
            catch (err) { console.error("rant: the event handler threw:", err); }
        } else if (e.kind === "error") console.error(`rant[${this.name ?? "node"}]: ${e.text}`);
    }

    /** @internal */ report(error: ErrorKind, text: string, topicName: string | null = null, detail: string | null = null): void {
        this.emit({ kind: "error", error, text, peer: 0, peerName: null, topic: 0, topicName, osError: 0, address: null,
                    lostFirst: 0, lostCount: 0, tooBigBytes: 0, identity: 0n, publishTopics: 0, receiveTopics: 0, detail });
    }

    private onMessage(p: number): void {
        const n = this.n, M = L.RantMsg;
        const index = n.u16(p + M.topic_index);
        if (index === this.logIndex) { this.deliverLog(p); return; }
        if (index === this.statusIndex) { this.statusFrom(p); return; }
        const rec = this.byIndex.get(index);
        if (!rec) return;
        const data = n.u32(p + M.data), len = n.u32(p + M.data + 4), schema = n.u32(p + M.schema);
        let base: Omit<Message<unknown>, "value"> | null = null;
        for (const sub of [...rec.subscribers]) {
            if (!sub.handler) continue;
            base ??= {
                data: n.copy(data, len), publisherId: n.u32(p + M.publisher_id), publisherName: n.strAt(p + M.publisher_name),
                topicName: n.strAt(p + M.topic_name), recvUs: n.us(p + M.recv_us), writtenUs: n.us(p + M.written_us),
                captureUs: n.us(p + M.capture_us),
            };
            let value: unknown;
            try { value = schema ? this.decodeAt(schema, data, len, sub.names) : base.data; }
            catch (e) {
                this.report("schema_mismatch", `a message on '${rec.name}' did not decode: ${errorText(e)}`, rec.name, errorText(e));
                continue;
            }
            try { sub.handler(value, { ...base, value }); }
            catch (e) { this.report("none", `the handler of '${rec.name}' threw: ${errorText(e)}`, rec.name); }
        }
    }

    /* a builtin message's fields, empty when it does not decode */
    private builtinFields(p: number): Record<string, unknown> {
        const n = this.n, M = L.RantMsg, schema = n.u32(p + M.schema);
        try {
            if (schema) return this.decodeAt(schema, n.u32(p + M.data), n.u32(p + M.data + 4), null) as Record<string, unknown>;
        } catch { /* it still says who sent it */ }
        return {};
    }

    private deliverLog(p: number): void {
        const h = this.logHandler;
        if (!h) return;
        const n = this.n, M = L.RantMsg, f = this.builtinFields(p);
        const capture = n.us(p + M.capture_us);
        try {
            h({ level: levelOf(f.level), origin: originOf(f.origin), node: n.strAt(p + M.publisher_name),
                nodeId: n.u32(p + M.publisher_id), text: String(f.text ?? ""),
                wallUs: capture || n.us(p + M.written_us), recvUs: n.us(p + M.recv_us) });
        } catch (e) { console.error("rant: the log handler threw:", e); }
    }

    private statusFrom(p: number): void {
        const n = this.n, M = L.RantMsg, f = this.builtinFields(p);
        const rows = Array.isArray(f.conditions) ? f.conditions as Record<string, unknown>[] : [];
        const conditions = rows.map((c) => ({ key: String(c.key ?? ""), level: levelOf(c.level), origin: originOf(c.origin),
                                              clearable: !!c.clearable, sinceUs: Number(c.since_us ?? 0), text: String(c.text ?? "") }));
        const nodeId = n.u32(p + M.publisher_id);
        if (conditions.length) this.statusPeers.add(nodeId);
        else this.statusPeers.delete(nodeId);
        this.deliverStatus({ node: n.strAt(p + M.publisher_name), nodeId, conditions });
    }

    private deliverStatus(u: StatusUpdate): void {
        const h = this.statusHandler;
        if (!h) return;
        try { h(u); }
        catch (e) { console.error("rant: the status handler threw:", e); }
    }

    /* Widens a builtin topic of this node to subscribe for its life, the queue first so the catch up replay parks. */
    private bindBuiltin(what: string, topic: number, setQueue: (h: number, q: number) => number): number {
        const h = this.live();
        const r = setQueue(h, this.queue);
        if (r !== 0 || !topic) throw new RantError(`${what}: ${r === -6 || !topic ? "the builtin is disabled on this node" : sendStatus(r)}`);
        const index = this.n.c.rant_topic_index(topic);
        const rr = this.n.c.rant_topic_set_role(topic, 0);
        if (rr !== 0) throw new RantError(`${what}: subscribe refused: ${sendStatus(rr)}`);
        return index;
    }

    /* ---- schemas ---- */

    /** @internal */ table(ptr: number): Table {
        let t = this.tables.get(ptr);
        if (!t) {
            t = new Table(this.n, ptr);
            this.tables.set(ptr, t);
        }
        return t;
    }

    /** @internal */ decodeAt(schema: number, ptr: number, len: number, names: Names | null): unknown {
        return decode(this.table(schema), ptr, len, names);
    }

    /** @internal */ compiledAt(ptr: number): CompiledSchema | null {
        if (!ptr) return null;
        let s = this.compiled.get(ptr);
        if (!s) {
            s = new CompiledSchema(this, ptr);
            this.compiled.set(ptr, s);
        }
        return s;
    }

    /** @internal */ resolve(s: SchemaArg | undefined): { ptr: number; names: Names | null } {
        if (s === null || s === undefined) return { ptr: 0, names: null };
        if (s instanceof CompiledSchema) {
            if (s.node !== this) throw new RantError("the schema belongs to another node");
            return { ptr: s.ptr, names: null };
        }
        if (typeof s === "string") return { ptr: this.compile(s), names: null };
        return { ptr: this.compile(dsl(s)), names: namesOf(this.n, s.shape) };
    }

    private compile(text: string): number {
        const h = this.live();
        const ptr = scoped(this.n, (sc) => this.n.c.rant_node_schema(h, sc.cstr(text)));
        if (!ptr) {
            const e = this.lastError;
            throw new RantError(`the schema did not compile: ${e.detail ?? e.text}`, e);
        }
        return ptr;
    }

    /* A schema compiled in this node from a builder or DSL text. Every definition stays in
     * scope for later ones, and a standard type's name alone is a schema. */
    schema(source: Schema<any> | string): CompiledSchema {
        return this.compiledAt(this.resolve(source).ptr)!;
    }

    /* A payload as a RantBytes struct in sc: value through the schema, or bytes for a raw handle. */
    /** @internal */ payload(sc: Scope, schema: number, names: Names | null, value: unknown): number {
        if (!schema) return sc.bytes(rawBytes(value));
        const m = encode(this.table(schema), value, names);
        sc.own(m.ptr);
        return sc.view(m.ptr, m.len);
    }

    /* ---- topics ---- */

    /** @internal */ acquire(name: string, bit: number, schema: number, opts: TopicOptions, pull: boolean): TopicRec {
        const h = this.live();
        let rec = this.topics.get(name);
        if (rec) {
            if (schema && rec.schema && schema !== rec.schema) throw new RantError(`'${name}' is already open on this node with another schema`);
            if (pull !== rec.pull) throw new RantError(`'${name}' is already open on this node as a ${rec.pull ? "pulled" : "pushed"} topic`);
            const bits = rolesOf(rec) | bit;
            if (bits !== rolesOf(rec)) {
                const r = this.n.c.rant_topic_set_role(rec.h, roleFor(bits));
                if (r !== 0) throw new RantError(`'${name}' refused the role change: ${sendStatus(r)}`, this.lastError);
            }
            if (!rec.schema) rec.schema = schema;
        } else {
            const t = scoped(this.n, (sc) => {
                const o = sc.alloc(L.RantTopicOpts.sizeof), Q = L.RantQos, T = L.RantTopicOpts, n = this.n;
                n.setU32(o + Q.reliability, opts.reliable ? 1 : 0);
                n.setU16(o + Q.keep_last, opts.keepLast ?? 0);
                n.setU16(o + Q.catch_up, opts.catchUp ?? 0);
                n.setU32(o + Q.max_message_bytes, opts.maxMessageBytes ?? 0);
                n.setU32(o + Q.heartbeat_us, us(opts.heartbeatMs));
                n.setU32(o + Q.repair_delay_us, us(opts.repairDelayMs));
                n.setU32(o + Q.backpressure_wait_us, us(opts.backpressureWaitMs));
                n.setU32(o + Q.queue_bytes, opts.queueBytes ?? 0);
                n.setU16(o + Q.max_rate_hz, opts.maxRateHz ?? 0);
                n.setU8(o + Q.no_timestamp, opts.noTimestamp ? 1 : 0);
                n.setU8(o + T.reflect_from_mesh, opts.reflectFromMesh ? 1 : 0);
                n.setU32(o + T.queue, pull ? 0 : this.queue);
                n.setU8(o + T.pull, pull ? 1 : 0);
                return n.c.rant_node_create_topic(h, sc.cstr(name), roleFor(bit), schema, o);
            });
            if (!t) throw new RantError(`'${name}' could not be created: ${this.lastError.text}`, this.lastError);
            rec = { h: t, index: this.n.c.rant_topic_index(t), name, schema, pull, pubs: 0, subs: 0, subscribers: new Set() };
            this.topics.set(name, rec);
            this.byIndex.set(rec.index, rec);
        }
        if (bit === PUB) rec.pubs++; else rec.subs++;
        return rec;
    }

    /* Drops one hold, and retires the topic with its last. */
    /** @internal */ releaseTopic(rec: TopicRec, bit: number): boolean {
        if (!this.h || this.topics.get(rec.name) !== rec) return true;
        const before = rolesOf(rec);
        if (bit === PUB) rec.pubs--; else rec.subs--;
        const after = rolesOf(rec);
        const r = after === 0 ? this.n.c.rant_topic_retire(rec.h)
                : after !== before ? this.n.c.rant_topic_set_role(rec.h, roleFor(after)) : 0;
        if (r !== 0) {
            if (bit === PUB) rec.pubs++; else rec.subs++;
            return false;
        }
        if (after === 0) {
            this.topics.delete(rec.name);
            this.byIndex.delete(rec.index);
        }
        this.n.wake();
        return true;
    }

    /* ---- handles ---- */

    /* The publishing side of a topic. */
    publisher<S extends SchemaArg>(name: string, schema: S, opts: TopicOptions = {}): Publisher<Val<S>> {
        return new Publisher(this, name, this.resolve(schema), opts);
    }

    /* The subscribing side of a topic. Without a handler it is pulled with take() and latest(). */
    subscriber<S extends SchemaArg>(name: string, schema: S,
                                    handler?: ((value: Val<S>, msg: Message<Val<S>>) => void) | null,
                                    opts: TopicOptions = {}): Subscriber<Val<S>> {
        return new Subscriber(this, name, this.resolve(schema), handler ?? null, opts);
    }

    /* A function this node answers. The handler returns the response, or a promise of it. */
    functionDefinition<Q extends SchemaArg, P extends SchemaArg>(
            name: string, handler: (value: Val<Q>, request: Request) => Val<P> | Promise<Val<P>>, req: Q, rsp: P,
            opts: FunctionOptions = {}): FunctionDefinition<Val<Q>, Val<P>> {
        return new FunctionDefinition(this, name, handler, this.resolve(req), this.resolve(rsp), opts);
    }

    /* The calling side of a function another node answers. */
    remoteFunction<Q extends SchemaArg, P extends SchemaArg>(name: string, req: Q, rsp: P,
                                                            opts: FunctionOptions = {}): RemoteFunction<Val<Q>, Val<P>> {
        return new RemoteFunction(this, name, this.resolve(req), this.resolve(rsp), opts);
    }

    /* A task this node runs: a long call that streams progress and may be cancelled. */
    taskDefinition<Q extends SchemaArg, G extends SchemaArg, P extends SchemaArg>(
            name: string, handler: (value: Val<Q>, ctx: TaskContext<Val<G>>) => Val<P> | Promise<Val<P>>,
            req: Q, prg: G, rsp: P, opts: TaskOptions = {}): TaskDefinition<Val<Q>, Val<G>, Val<P>> {
        return new TaskDefinition(this, name, handler, this.resolve(req), this.resolve(prg), this.resolve(rsp), opts);
    }

    /* The calling side of a task another node runs. */
    remoteTask<Q extends SchemaArg, G extends SchemaArg, P extends SchemaArg>(
            name: string, req: Q, prg: G, rsp: P, opts: TaskOptions = {}): RemoteTask<Val<Q>, Val<G>, Val<P>> {
        return new RemoteTask(this, name, this.resolve(req), this.resolve(prg), this.resolve(rsp), opts);
    }

    /* A variable this node owns and publishes. */
    variableDefinition<S extends SchemaArg>(name: string, schema: S, opts: VariableOptions<Val<S>> = {}): VariableDefinition<Val<S>> {
        return new VariableDefinition(this, name, this.resolve(schema), opts, false);
    }

    /* A variable another node owns: reads see the latest value, writes go to the owner. */
    remoteVariable<S extends SchemaArg>(name: string, schema: S, opts: VariableOptions<Val<S>> = {}): RemoteVariable<Val<S>> {
        return new RemoteVariable(this, name, this.resolve(schema), opts, true);
    }

    /* ---- the rest of the node ---- */

    /* Waits until discovery and matching settle for what exists now. 3 announce intervals by default. */
    settle(timeoutMs = this.settleMs): Promise<boolean> {
        return this.until(() => !!this.h && this.n.c.rant_node_settle(this.h, 0) === 1, timeoutMs);
    }

    /* Publishes a log line, truncated at 512 bytes. A level below logLevel sends nothing. "nosys" when logs are disabled. */
    log(level: Level, text: string): SendStatus {
        const h = this.live();
        const b = encodeUtf8(text);
        const r = scoped(this.n, (sc) => this.n.c.rant_node_log_text(h, LEVELS.indexOf(level) + 1, sc.data(b), b.length));
        this.n.wake();
        return sendStatus(r);
    }

    /* Every other node's log lines. A later call replaces the handler, null stops them. */
    onLog(handler: ((line: LogLine) => void) | null): void {
        const h = this.live();
        if (handler && this.logIndex < 0)
            this.logIndex = this.bindBuiltin("onLog", this.n.c.rant_node_log_topic(h), this.n.c.rant_node_set_log_queue);
        this.logHandler = handler;
    }

    /* Raises the condition key on this node's status, or updates it while active: sinceUs stays from the raise and the
     * same values again send nothing. "too_big" for a key over 48 bytes or past 32 conditions, "nosys" when disabled. */
    setStatus(key: string, level: Level, text: string, opts: { clearable?: boolean } = {}): SendStatus {
        const h = this.live();
        const r = scoped(this.n, (sc) => {
            const so = sc.alloc(L.RantStatusOpts.sizeof);
            this.n.setU8(so + L.RantStatusOpts.clearable, opts.clearable ? 1 : 0);
            return this.n.c.rant_node_set_status(h, sc.cstr(key), LEVELS.indexOf(level) + 1, sc.cstr(text), so);
        });
        this.n.wake();
        return sendStatus(r);
    }

    /* Clears the condition key, "ok" whether or not it was active. */
    clearStatus(key: string): SendStatus {
        const h = this.live();
        const r = scoped(this.n, (sc) => this.n.c.rant_node_clear_status(h, sc.cstr(key)));
        this.n.wake();
        return sendStatus(r);
    }

    /* Asks another node to clear its clearable condition key, the occurrence raised at sinceUs. Nothing answers:
     * its next status shows it gone, or still there when it refused. "role" while this node does not read
     * status with onStatus. */
    clearPeerStatus(nodeId: number, key: string, sinceUs: number): SendStatus {
        const h = this.live();
        const r = scoped(this.n, (sc) => this.n.c.rant_node_clear_peer_status(h, nodeId, sc.cstr(key), BigInt(sinceUs)));
        this.n.wake();
        return sendStatus(r);
    }

    /* Every other node's status on each change, each node's current set when this binds, and an empty set when a
     * node goes away. A later call replaces the handler, null stops them. */
    onStatus(handler: ((update: StatusUpdate) => void) | null): void {
        const h = this.live();
        if (handler && this.statusIndex < 0)
            this.statusIndex = this.bindBuiltin("onStatus", this.n.c.rant_node_status_topic(h), this.n.c.rant_node_set_status_queue);
        this.statusHandler = handler;
    }

    get stats(): NodeStats {
        const h = this.live(), n = this.n;
        return scoped(n, (sc) => {
            const a = sc.alloc(32);
            n.c.rant_node_mem_stats(h, a, a + 4, a + 8);
            n.c.rant_node_backpressure_stats(h, a + 16, a + 24);
            return { evictedUnsent: n.c.rant_node_evicted_unsent(h) >>> 0, memInUse: n.u32(a), memPeak: n.u32(a + 4),
                     allocCalls: Number(n.u64(a + 8)), backpressureWaitedUs: Number(n.u64(a + 16)), backpressureWaits: n.u32(a + 24) };
        });
    }

    /* The latest error this node reported, kind "error" with error "none" before any. */
    get lastError(): RantEvent { return lastError(this.n, this.h); }

    /* Closes the node. Calls in flight answer cancelled. Inside a callback it closes once the callbacks end. */
    close(sendBye = true): boolean {
        if (!this.h) return true;
        if (this.defer(() => this.close(sendBye))) return true;
        if (this.n.c.rant_node_close(this.h, sendBye ? 1 : 0) !== 0) return false;
        this.h = 0;
        this.n.undrive(this.drive);
        for (const h of [...this.handles]) h.forget();
        this.handles.clear();
        for (const settle of [...this.calls]) settle();
        this.calls.clear();
        for (const w of [...this.waiters]) this.endWait(w, false);
        this.unboxAll();
        this.tables.clear();
        this.topics.clear();
        this.byIndex.clear();
        this.release?.();
        this.release = null;
        return true;
    }
    [Symbol.dispose](): void { this.close(); }
}

/* Events reach the node's box like messages do, so the event trampoline marks them first. */
const PUB = 1, SUB = 2;
const rolesOf = (r: TopicRec) => (r.pubs ? PUB : 0) | (r.subs ? SUB : 0);
const roleFor = (bits: number) => bits === 3 ? 0 : bits === PUB ? 1 : bits === SUB ? 2 : 3;
const us = (ms?: number) => Math.round((ms ?? 0) * 1000);
const errorText = (e: unknown) => e instanceof Error ? e.message : String(e);
const isThenable = (v: unknown): v is PromiseLike<unknown> =>
    !!v && (typeof v === "object" || typeof v === "function") && typeof (v as { then?: unknown }).then === "function";

function rawBytes(v: unknown): Uint8Array | null {
    if (v === null || v === undefined) return null;
    if (v instanceof Uint8Array) return v;
    throw new SchemaError("a handle without a schema sends a Uint8Array");
}

function writeAddr(n: Native, p: number, s: string): void {
    const [host, port] = s.split(":");
    const parts = host.split(".").map(Number);
    if (parts.length !== 4 || parts.some((x) => !(x >= 0 && x <= 255))) throw new RantError(`the seed peer '${s}' is not an IPv4 address`);
    parts.forEach((x, i) => n.setU8(p + L.RantAddr.ip + i, x));
    n.setU8(p + L.RantAddr.ip_len, 4);
    n.setU16(p + L.RantAddr.port, port ? Number(port) : 0);
}

function eventFrom(n: Native, p: number): RantEvent {
    const E = L.RantEvent;
    const text = scoped(n, (sc) => {
        const buf = sc.alloc(256);
        n.c.rant_event_str(p, buf, 256);
        return n.cstr(buf) ?? "";
    });
    const ipLen = n.u8(p + E.ip_len);
    return {
        kind: EVENT[n.i32(p + E.kind)] ?? "error", error: ERROR[n.i32(p + E.error)] ?? "none", text,
        peer: n.u32(p + E.peer), peerName: n.cstr(n.u32(p + E.peer_name)), topic: n.u16(p + E.topic),
        topicName: n.cstr(n.u32(p + E.topic_name)), osError: n.i32(p + E.os_error),
        address: ipLen === 4 ? `${[0, 1, 2, 3].map((i) => n.u8(p + E.ip + i)).join(".")}:${n.u16(p + E.port)}` : null,
        lostFirst: Number(n.u64(p + E.lost_first)), lostCount: Number(n.u64(p + E.lost_count)),
        tooBigBytes: Number(n.u64(p + E.too_big_bytes)), identity: n.u64(p + E.identity),
        publishTopics: n.u16(p + E.publish_topics), receiveTopics: n.u16(p + E.receive_topics),
        detail: n.cstr(n.u32(p + E.detail)),
    };
}

function lastError(n: Native, h: number): RantEvent {
    return scoped(n, (sc) => {
        const e = sc.alloc(L.RantEvent.sizeof);
        n.c.rant_last_error(e, h);
        return eventFrom(n, e);
    });
}

/* ---- compiled schemas --------------------------------------------------------------------- */

/* A schema the node compiled or received, alive while the node is open. */
export class CompiledSchema {
    /** @internal */ constructor(/** @internal */ readonly node: RantNode, /** @internal */ readonly ptr: number) {}

    private live(): Native {
        this.node.live();
        return this.node.n;
    }
    get name(): string {
        const n = this.live();
        return scoped(n, (sc) => { const s = sc.alloc(8); n.c.rant_schema_name(s, this.ptr); return n.strAt(s); });
    }
    get hash(): bigint { return BigInt.asUintN(64, BigInt(this.live().c.rant_schema_hash(this.ptr))); }
    /* DSL text that compiles back to this schema */
    get dsl(): string {
        const n = this.live();
        const len = n.c.rant_schema_print(this.ptr, 0, 0);
        return scoped(n, (sc) => {
            const buf = sc.alloc(len + 1);
            n.c.rant_schema_print(this.ptr, buf, len + 1);
            return n.str(buf, len);
        });
    }
    /* the canonical bytes peers exchange */
    get wire(): Uint8Array {
        const n = this.live();
        return scoped(n, (sc) => { const b = sc.alloc(8); n.c.rant_schema_wire(b, this.ptr); return n.bytesAt(b); });
    }
    /* the flat depth first field table */
    get fields(): FieldInfo[] { this.live(); return this.node.table(this.ptr).fields; }
    enumVariants(field: number): { name: string; value: number }[] {
        this.live();
        return [...(this.node.table(this.ptr).rows[field]?.variants ?? [])].map(([value, name]) => ({ name, value }));
    }
    /* true when a reader of this schema reads what writer writes */
    canRead(writer: CompiledSchema): boolean { return this.live().c.rant_schema_subset(this.ptr, writer.ptr) === 1; }
    encode(value: unknown): Uint8Array {
        const n = this.live();
        const m = encode(this.node.table(this.ptr), value, null);
        try { return n.copy(m.ptr, m.len); } finally { n.free(m.ptr); }
    }
    decode(data: Uint8Array): unknown {
        const n = this.live();
        return scoped(n, (sc) => this.node.decodeAt(this.ptr, sc.data(data), data.length, null));
    }
}

/* ---- handles ------------------------------------------------------------------------------ */

/* What every handle has: a name and close(). A closed handle answers no_topic. */
export abstract class Handle {
    /** @internal */ constructor(readonly node: RantNode, readonly name: string) {}

    abstract get closed(): boolean;
    /** @internal */ abstract forget(): void;
    protected abstract retire(): boolean;

    /* Closes the handle. Inside a callback it closes once the callbacks end. */
    close(): boolean {
        if (this.closed) return true;
        if (this.node.defer(() => this.close())) return true;
        if (!this.retire()) return false;
        this.forget();
        this.node.untrack(this);
        return true;
    }
    [Symbol.dispose](): void { this.close(); }
}

abstract class TopicHandle extends Handle {
    protected rec: TopicRec | null;
    /** @internal */ readonly names: Names | null;

    /** @internal */
    constructor(node: RantNode, name: string, s: { ptr: number; names: Names | null }, opts: TopicOptions, bit: number, pull: boolean) {
        super(node, name);
        this.names = s.names;
        this.rec = node.acquire(name, bit, s.ptr, opts, pull);
        node.track(this);
    }
    get closed(): boolean { return !this.rec; }
    /** @internal */ forget(): void { this.rec = null; }
    protected t(): number { return this.rec ? this.rec.h : 0; }

    /* the schema in use now, the mesh's for a reflectFromMesh handle */
    get schema(): CompiledSchema | null { const t = this.t(); return t ? this.node.compiledAt(this.node.n.c.rant_topic_schema(t)) : null; }
    get counts(): TopicCounts {
        const t = this.t(), n = this.node.n;
        if (!t) return { txMsgs: 0, txBytes: 0, rxMsgs: 0, rxBytes: 0 };
        return scoped(n, (sc) => {
            const a = sc.alloc(32);
            n.c.rant_topic_counts(t, a, a + 8, a + 16, a + 24);
            return { txMsgs: Number(n.u64(a)), txBytes: Number(n.u64(a + 8)), rxMsgs: Number(n.u64(a + 16)), rxBytes: Number(n.u64(a + 24)) };
        });
    }
    /* A reflectFromMesh handle takes the mesh's types again when they moved. true when they changed. */
    refresh(): boolean { const t = this.t(); return !!t && this.node.n.c.rant_topic_refresh(t) === 1; }
}

export class Publisher<T> extends TopicHandle {
    /** @internal */
    constructor(node: RantNode, name: string, s: { ptr: number; names: Names | null }, opts: TopicOptions) {
        super(node, name, s, opts, PUB, false);
    }
    protected retire(): boolean { return !this.rec || this.node.releaseTopic(this.rec, PUB); }

    /* Publishes to every matched subscriber. captureUs is when the data was true, if not now. */
    send(value: T, captureUs = 0): SendStatus {
        const t = this.t(), n = this.node.n;
        if (!t) return "no_topic";
        let r: number;
        try {
            r = scoped(n, (sc) => {
                const opts = sc.alloc(L.RantSendOpts.sizeof);
                n.setU64(opts + L.RantSendOpts.capture_us, BigInt(Math.round(captureUs)));
                return n.c.rant_topic_send(t, this.node.payload(sc, n.c.rant_topic_schema(t), this.names, value), opts);
            });
        } catch (e) {
            if (e instanceof SchemaError) return "schema";
            throw e;
        }
        n.wake();
        return sendStatus(r);
    }
    get matchCount(): number { const t = this.t(); return t ? this.node.n.c.rant_topic_match_count(t) : 0; }
    /* true when a send would not wait for a match still forming */
    get ready(): boolean { const t = this.t(); return !!t && this.node.n.c.rant_topic_ready(t) === 1; }
    /* Waits until every subscriber has everything sent so far. */
    drain(timeoutMs = 1000): Promise<boolean> {
        return this.node.until(() => { const t = this.t(); return !!t && this.node.n.c.rant_topic_drain(t, 0) === 1; }, timeoutMs);
    }
}

export class Subscriber<T> extends TopicHandle {
    /** @internal */ handler: ((value: T, msg: Message<T>) => void) | null;

    /** @internal */
    constructor(node: RantNode, name: string, s: { ptr: number; names: Names | null },
                handler: ((value: T, msg: Message<T>) => void) | null, opts: TopicOptions) {
        super(node, name, s, opts, SUB, !handler);
        this.handler = handler;
        this.rec!.subscribers.add(this);
    }
    protected retire(): boolean {
        if (!this.rec) return true;
        this.rec.subscribers.delete(this);
        if (this.node.releaseTopic(this.rec, SUB)) return true;
        this.rec.subscribers.add(this);
        return false;
    }

    /* A pulled subscriber's oldest waiting message, or null. */
    take(): Message<T> | null { return this.pull(false); }
    /* The newest waiting message, dropping the older ones, or null. */
    latest(): Message<T> | null { return this.pull(true); }

    private pull(latest: boolean): Message<T> | null {
        const t = this.t(), node = this.node, n = node.n;
        if (!t) return null;
        if (this.handler) throw new RantError(`'${this.name}' has a handler, so its messages are pushed`);
        return scoped(n, (sc) => {
            const p = sc.alloc(L.RantMsg.sizeof), M = L.RantMsg;
            if ((latest ? n.c.rant_topic_take_latest(t, p, 0) : n.c.rant_topic_take(t, p, 0)) !== 1) return null;
            const data = n.u32(p + M.data), len = n.u32(p + M.data + 4), schema = n.u32(p + M.schema);
            const bytes = n.copy(data, len);
            let value: unknown;
            try { value = schema ? node.decodeAt(schema, data, len, this.names) : bytes; }
            catch (e) { throw new SchemaError(`a message on '${this.name}' did not decode: ${errorText(e)}`); }
            return {
                value: value as T, data: bytes, publisherId: n.u32(p + M.publisher_id), publisherName: n.strAt(p + M.publisher_name),
                topicName: n.strAt(p + M.topic_name), recvUs: n.us(p + M.recv_us), writtenUs: n.us(p + M.written_us), captureUs: n.us(p + M.capture_us),
            };
        });
    }

    /* A pulled subscriber's ring. */
    get queueStats(): QueueStats {
        const t = this.t(), n = this.node.n;
        if (!t) return { messages: 0, bytes: 0, capacity: 0, dropped: 0 };
        return scoped(n, (sc) => {
            const a = sc.alloc(16);
            n.c.rant_topic_queue_stats(t, a, a + 4, a + 8, a + 12);
            return { messages: n.u32(a), bytes: n.u32(a + 4), capacity: n.u32(a + 8), dropped: n.u32(a + 12) };
        });
    }
}

/* ---- functions and tasks ------------------------------------------------------------------ */

type Resolved = { ptr: number; names: Names | null };

function functionOpts(n: Native, sc: Scope, o: FunctionOptions, q: number): number {
    const p = sc.alloc(L.RantFunctionOpts.sizeof), F = L.RantFunctionOpts;
    n.setU32(p + F.backpressure_wait_us, us(o.backpressureWaitMs));
    n.setU32(p + F.timeout_us, us(o.timeoutMs));
    n.setU16(p + F.keep_last, o.keepLast ?? 0);
    n.setU8(p + F.reflect_from_mesh, o.reflectFromMesh ? 1 : 0);
    n.setU8(p + F.multi, o.multi ? 1 : 0);
    n.setU32(p + F.queue, q);
    return p;
}

function taskOpts(n: Native, sc: Scope, o: TaskOptions, q: number): number {
    const p = sc.alloc(L.RantTaskOpts.sizeof), T = L.RantTaskOpts;
    n.setU8(p + T.progress_best_effort, o.progressBestEffort ? 1 : 0);
    n.setU16(p + T.progress_keep_last, o.progressKeepLast ?? 0);
    n.setU8(p + T.no_cancel, o.noCancel ? 1 : 0);
    n.setU8(p + T.exclusive, o.exclusive ? 1 : 0);
    n.setU8(p + T.multi, o.multi ? 1 : 0);
    n.setU32(p + T.timeout_us, us(o.timeoutMs));
    n.setU32(p + T.backpressure_wait_us, us(o.backpressureWaitMs));
    n.setU16(p + T.keep_last, o.keepLast ?? 0);
    n.setU8(p + T.reflect_from_mesh, o.reflectFromMesh ? 1 : 0);
    n.setU32(p + T.queue, q);
    return p;
}

/* The surface the four function and task handles share: one C RantFunction. */
abstract class FunctionHandle extends Handle {
    /** @internal */ h = 0;
    protected boxes: number[] = [];

    /** @internal */
    constructor(node: RantNode, name: string, /** @internal */ readonly req: Resolved, /** @internal */ readonly rsp: Resolved,
                /** @internal */ readonly prg: Resolved) {
        super(node, name);
    }
    /** @internal */ created(h: number, what: string): void {
        if (!h) {
            for (const b of this.boxes) this.node.unbox(b);
            const e = this.node.lastError;
            throw new RantError(`${what}('${this.name}') failed: ${e.text}`, e);
        }
        this.h = h;
        this.node.track(this);
    }
    get closed(): boolean { return !this.h; }
    /** @internal */ forget(): void {
        this.h = 0;
        for (const b of this.boxes) this.node.unbox(b);
    }
    protected retire(): boolean {
        if (!this.h) return true;
        const ok = this.node.n.c.rant_function_retire(this.h) === 0;
        if (ok) this.node.n.wake();
        return ok;
    }
    /* callers matched at a definition, definitions matched at a remote */
    get matchCount(): number { return this.h ? this.node.n.c.rant_function_match_count(this.h) : 0; }
    get requestSchema(): CompiledSchema | null { return this.h ? this.node.compiledAt(this.node.n.c.rant_function_request_schema(this.h)) : null; }
    get responseSchema(): CompiledSchema | null { return this.h ? this.node.compiledAt(this.node.n.c.rant_function_response_schema(this.h)) : null; }
    /* A reflectFromMesh handle takes the mesh's types again when they moved. true when they changed. */
    refresh(): boolean { return !!this.h && this.node.n.c.rant_function_refresh(this.h) === 1; }

    /** @internal */ request(req: number): { info: Request; value: unknown } {
        const n = this.node.n, R = L.RantRequest;
        const data = n.u32(req + R.data), len = n.u32(req + R.data + 4), schema = n.u32(req + R.schema);
        const info: Request = {
            caller: n.u32(req + R.caller), callerName: n.strAt(req + R.caller_name), functionName: n.strAt(req + R.function_name),
            recvUs: n.us(req + R.recv_us), writtenUs: n.us(req + R.written_us), data: n.copy(data, len),
        };
        return { info, value: schema ? this.node.decodeAt(schema, data, len, this.req.names) : info.data };
    }

    /* A thrown error as an answer: its message, an AppError's value, a CancelledError's partial result. */
    /** @internal */ failure(e: unknown, cancelled: boolean): { status: CallStatus; message: string; value: unknown } {
        if (e instanceof CancelledError && cancelled) return { status: "cancelled", message: e.message, value: e.partial };
        if (e instanceof AppError) return { status: "app_error", message: e.message, value: e.value };
        return { status: cancelled ? "cancelled" : "app_error", message: errorText(e) || "the handler threw", value: undefined };
    }

    /* Completes a deferred call. A stale token, as after a retire, answers state and is dropped. */
    /** @internal */ complete(token: bigint, status: CallStatus, message: string, value: unknown): void {
        const h = this.h, n = this.node.n;
        if (!h) return;
        scoped(n, (sc) => {
            let bytes: number;
            try { bytes = value === undefined ? sc.bytes(null) : this.node.payload(sc, n.c.rant_function_response_schema(h), this.rsp.names, value); }
            catch (e) {
                status = "app_error";
                message = `the response did not encode: ${errorText(e)}`;
                bytes = sc.bytes(null);
            }
            n.c.rant_function_complete(h, token, CALL_NUMBER[status], message ? sc.cstr(message) : 0, bytes);
        });
        n.wake();
    }
}

export class FunctionDefinition<Req, Rsp> extends FunctionHandle {
    /** @internal */
    constructor(node: RantNode, name: string, private handler: (value: Req, request: Request) => Rsp | Promise<Rsp>,
                req: Resolved, rsp: Resolved, opts: FunctionOptions) {
        super(node, name, req, rsp, { ptr: 0, names: null });
        const n = node.n, nh = node.live();
        const box = node.box((p: number) => this.onRequest(p));
        this.boxes.push(box);
        this.created(scoped(n, (sc) => n.c.rant_node_create_function_definition(
            nh, sc.cstr(name), req.ptr, rsp.ptr, n.fn.request, box, functionOpts(n, sc, opts, node.queue))), "functionDefinition");
    }

    private onRequest(req: number): void {
        const n = this.node.n;
        let call: { info: Request; value: unknown };
        try { call = this.request(req); }
        catch (e) { return this.answer(req, "app_error", `the request did not decode: ${errorText(e)}`, undefined); }
        let out: Rsp | Promise<Rsp>;
        try { out = this.handler(call.value as Req, call.info); }
        catch (e) { const f = this.failure(e, false); return this.answer(req, f.status, f.message, f.value); }
        if (!isThenable(out)) return this.answer(req, "ok", "", out);
        const token: bigint = n.c.rant_request_defer(req);
        if (!token) return;
        Promise.resolve(out).then((v) => this.complete(token, "ok", "", v),
                                  (e) => { const f = this.failure(e, false); this.complete(token, f.status, f.message, f.value); });
    }

    /* Answers inside the request callback, where the C wants the request pointer itself. */
    private answer(req: number, status: CallStatus, message: string, value: unknown): void {
        const n = this.node.n;
        scoped(n, (sc) => {
            let bytes: number;
            try { bytes = value === undefined ? sc.bytes(null) : this.node.payload(sc, n.c.rant_function_response_schema(this.h), this.rsp.names, value); }
            catch (e) {
                n.c.rant_request_fail(req, sc.cstr(`the response did not encode: ${errorText(e)}`), sc.bytes(null));
                return;
            }
            if (status === "ok") n.c.rant_request_reply(req, bytes);
            else n.c.rant_request_fail(req, message ? sc.cstr(message) : 0, bytes);
        });
        n.wake();
    }
}

/* A response from the C struct, decoded while its view lives. */
function responseFrom<T>(node: RantNode, p: number, names: Names | null): Response<T> {
    const n = node.n, R = L.RantResponse;
    const data = n.u32(p + R.data), len = n.u32(p + R.data + 4), schema = n.u32(p + R.schema);
    const status = CALL[n.i32(p + R.status)] ?? "cancelled";
    const bytes = n.copy(data, len);
    let value: unknown;
    if (len) {
        try { value = schema ? node.decodeAt(schema, data, len, names) : bytes; }
        catch { value = undefined; }
    }
    return { ok: status === "ok", status, sendStatus: "ok", value: value as T | undefined, hasValue: value !== undefined,
             data: bytes, message: n.strAt(p + R.message), provider: n.u32(p + R.provider), writtenUs: n.us(p + R.written_us) };
}

function failed<T>(status: CallStatus, message: string, sendStatus: SendStatus = "ok"): Response<T> {
    return { ok: false, status, sendStatus, value: undefined, hasValue: false, data: new Uint8Array(0), message, provider: 0, writtenUs: 0 };
}

export class RemoteFunction<Req, Rsp> extends FunctionHandle {
    /** @internal */
    constructor(node: RantNode, name: string, req: Resolved, rsp: Resolved, opts: FunctionOptions) {
        super(node, name, req, rsp, { ptr: 0, names: null });
        const n = node.n, nh = node.live();
        this.created(scoped(n, (sc) => n.c.rant_node_create_remote_function(
            nh, sc.cstr(name), req.ptr, rsp.ptr, functionOpts(n, sc, opts, node.queue))), "remoteFunction");
    }

    /* Calls the function. Never rejects: the status says how it went. timeoutMs adds a bound of
     * your own to the handle's, provider directs the call at one peer. */
    call(value: Req, opts: { timeoutMs?: number; provider?: number } = {}): Promise<Response<Rsp>> {
        return startCall<Rsp>(this.node, this.h, this.req.names, this.rsp.names, value, opts.provider ?? 0, opts.timeoutMs, null).result;
    }
}

/* One call through rant_function_call_async on the C function h: the response box, the
 * progress box for a task, and the backstop that answers cancelled if the node closes first. */
function startCall<Rsp>(node: RantNode, h: number, req: Names | null, rsp: Names | null, value: unknown, provider: number,
                        timeoutMs: number | undefined, progress: ((p: number) => void) | null): { result: Promise<Response<Rsp>>; callId: number } {
    const n = node.n;
    let resolve!: (r: Response<Rsp>) => void;
    const result = new Promise<Response<Rsp>>((r) => { resolve = r; });
    if (!h) { resolve(failed("cancelled", "the handle is closed", "no_topic")); return { result, callId: 0 }; }
    let done = false, timer: ReturnType<typeof setTimeout> | null = null;
    const boxes: number[] = [];
    const finish = (r: Response<Rsp>) => {
        if (done) return;
        done = true;
        if (timer) clearTimeout(timer);
        resolve(r);
    };
    const end = () => { forget(); for (const b of boxes) node.unbox(b); };
    const forget = node.inFlight(() => { finish(failed("cancelled", "the node closed")); end(); });
    boxes.push(node.box((p: number) => {
        finish(responseFrom<Rsp>(node, p, rsp));
        end();
    }));
    if (progress) boxes.push(node.box(progress));
    let callId = 0;
    const r = scoped(n, (sc) => {
        let bytes: number;
        try { bytes = node.payload(sc, n.c.rant_function_request_schema(h), req, value); }
        catch (e) {
            if (e instanceof SchemaError) return -7;
            throw e;
        }
        const o = sc.alloc(L.RantCallOpts.sizeof), C = L.RantCallOpts, id = sc.alloc(4);
        n.setU32(o + C.provider, provider);
        if (progress) {
            n.setU32(o + C.on_progress, n.fn.progress);
            n.setU32(o + C.progress_user, boxes[1]);
        }
        n.setU32(o + C.id_out, id);
        const rr = n.c.rant_function_call_async(h, bytes, n.fn.response, boxes[0], o);
        callId = n.u32(id);
        return rr;
    });
    if (r !== 0) {
        finish(failed("cancelled", `the call was not sent: ${sendStatus(r)}`, sendStatus(r)));
        end();
        return { result, callId: 0 };
    }
    if (timeoutMs !== undefined && timeoutMs >= 0) timer = setTimeout(() => finish(failed("timeout", "timeout")), timeoutMs);
    n.wake();
    return { result, callId };
}

export class TaskDefinition<Req, Prg, Rsp> extends FunctionHandle {
    private aborts = new Map<bigint, AbortController>();

    /** @internal */
    constructor(node: RantNode, name: string, private handler: (value: Req, ctx: TaskContext<Prg>) => Rsp | Promise<Rsp>,
                req: Resolved, prg: Resolved, rsp: Resolved, opts: TaskOptions) {
        super(node, name, req, rsp, prg);
        const n = node.n, nh = node.live();
        const box = node.box((p: number) => this.onRequest(p));
        const cancel = node.box((token: bigint) => this.aborts.get(BigInt.asUintN(64, token))?.abort(new CancelledError()));
        this.boxes.push(box, cancel);
        this.created(scoped(n, (sc) => n.c.rant_node_create_task_definition(
            nh, sc.cstr(name), req.ptr, prg.ptr, rsp.ptr, n.fn.request, box, taskOpts(n, sc, opts, node.queue))), "taskDefinition");
        n.c.rant_function_on_cancel(this.h, n.fn.cancel, cancel);
    }

    get progressSchema(): CompiledSchema | null { return this.h ? this.node.compiledAt(this.node.n.c.rant_function_progress_schema(this.h)) : null; }

    private onRequest(req: number): void {
        const n = this.node.n;
        let call: { info: Request; value: unknown };
        try { call = this.request(req); }
        catch (e) {
            scoped(n, (sc) => n.c.rant_request_fail(req, sc.cstr(`the request did not decode: ${errorText(e)}`), sc.bytes(null)));
            return;
        }
        const token = BigInt.asUintN(64, n.c.rant_request_defer(req));      /* implies RUNNING to the caller */
        if (!token) return;
        const ctrl = new AbortController();
        this.aborts.set(token, ctrl);
        let finished = false;
        const self = this;
        const ctx: TaskContext<Prg> = {
            ...call.info, signal: ctrl.signal,
            get cancelled() { return ctrl.signal.aborted || (!!self.h && n.c.rant_function_cancelled(self.h, token) === 1); },
            progress: (v: Prg): SendStatus => {
                const h = this.h;
                if (finished || !h) return "state";
                let r: number;
                try { r = scoped(n, (sc) => n.c.rant_function_progress(h, token, this.node.payload(sc, n.c.rant_function_progress_schema(h), this.prg.names, v))); }
                catch (e) {
                    if (e instanceof SchemaError) return "schema";
                    throw e;
                }
                n.wake();
                return sendStatus(r);
            },
        };
        let out: Promise<Rsp>;
        try { out = Promise.resolve(this.handler(call.value as Req, ctx)); }
        catch (e) { out = Promise.reject(e); }
        out.then((v) => ({ status: "ok" as CallStatus, message: "", value: v as unknown }),
                 (e) => this.failure(e, e instanceof CancelledError || (ctrl.signal.aborted && e === ctrl.signal.reason)))
           .then((f) => {
               finished = true;
               this.aborts.delete(token);
               this.complete(token, f.status, f.message, f.value);
           });
    }
}

/* One task call in flight. result settles once with the outcome. */
export interface TaskCall<Rsp> {
    readonly callId: number;
    readonly result: Promise<Response<Rsp>>;
    /* Asks the provider to stop. Cooperative: the result says how it ended. */
    cancel(): CancelStatus;
}

export class RemoteTask<Req, Prg, Rsp> extends FunctionHandle {
    /** @internal */
    constructor(node: RantNode, name: string, req: Resolved, prg: Resolved, rsp: Resolved, opts: TaskOptions) {
        super(node, name, req, rsp, prg);
        const n = node.n, nh = node.live();
        this.created(scoped(n, (sc) => n.c.rant_node_create_remote_task(
            nh, sc.cstr(name), req.ptr, prg.ptr, rsp.ptr, taskOpts(n, sc, opts, node.queue))), "remoteTask");
    }

    get progressSchema(): CompiledSchema | null { return this.h ? this.node.compiledAt(this.node.n.c.rant_function_progress_schema(this.h)) : null; }

    /* Starts the task. onProgress sees each update, null first for the RUNNING acknowledgment. */
    call(value: Req, opts: { onProgress?: (value: Prg | null, p: Progress<Prg>) => void; provider?: number } = {}): TaskCall<Rsp> {
        const node = this.node, n = node.n, on = opts.onProgress;
        const progress = on ? (p: number) => {
            const P = L.RantProgress;
            const data = n.u32(p + P.data), len = n.u32(p + P.data + 4), schema = n.u32(p + P.schema);
            let value: Prg | null = null;
            if (len) {
                try { value = (schema ? node.decodeAt(schema, data, len, this.prg.names) : n.copy(data, len)) as Prg; }
                catch (e) { node.report("schema_mismatch", `a progress update of '${this.name}' did not decode: ${errorText(e)}`, this.name); return; }
            }
            try {
                on(value, { value, data: n.copy(data, len), callId: n.u32(p + P.call_id), provider: n.u32(p + P.provider),
                            writtenUs: n.us(p + P.written_us), recvUs: n.us(p + P.recv_us) });
            } catch (e) { node.report("none", `the progress handler of '${this.name}' threw: ${errorText(e)}`, this.name); }
        } : null;
        const { result, callId } = startCall<Rsp>(this.node, this.h, this.req.names, this.rsp.names, value, opts.provider ?? 0, undefined, progress);
        return {
            callId, result,
            cancel: (): CancelStatus => {
                if (!this.h || !callId) return "not_pending";
                const r = n.c.rant_function_cancel(this.h, callId);
                n.wake();
                return r === 0 ? "ok" : r === -3 ? "no_cancel" : "not_pending";
            },
        };
    }
}

/* ---- variables ---------------------------------------------------------------------------- */

export class Variable<T> extends Handle {
    /** @internal */ h = 0;
    private names: Names | null;
    private changeBox = 0;
    private writeBox = 0;

    /** @internal */
    constructor(node: RantNode, name: string, s: Resolved, opts: VariableOptions<T>, remote: boolean) {
        super(node, name);
        this.names = s.names;
        const n = node.n, nh = node.live();
        const h = scoped(n, (sc) => {
            const o = sc.alloc(L.RantVariableOpts.sizeof), V = L.RantVariableOpts;
            if (opts.initial !== undefined) {
                const b = node.payload(sc, s.ptr, s.names, opts.initial);
                n.setU32(o + V.initial, n.u32(b));
                n.setU32(o + V.initial + 4, n.u32(b + 4));
            }
            n.setU8(o + V.access, opts.readOnly ? 1 : 0);
            n.setU8(o + V.allow_force, opts.allowForce ? 1 : 0);
            n.setU16(o + V.catch_up, opts.catchUp ?? 0);
            n.setU16(o + V.keep_last, opts.keepLast ?? 0);
            n.setU32(o + V.backpressure_wait_us, us(opts.backpressureWaitMs));
            n.setU8(o + V.reflect_from_mesh, opts.reflectFromMesh ? 1 : 0);
            n.setU32(o + V.queue, node.queue);
            const create = remote ? n.c.rant_node_create_remote_variable : n.c.rant_node_create_variable_definition;
            return create(nh, sc.cstr(name), s.ptr, o);
        });
        if (!h) {
            const e = node.lastError;
            throw new RantError(`${remote ? "remoteVariable" : "variableDefinition"}('${name}') failed: ${e.text}`, e);
        }
        this.h = h;
        node.track(this);
    }

    get closed(): boolean { return !this.h; }
    /** @internal */ forget(): void {
        this.h = 0;
        if (this.changeBox) this.node.unbox(this.changeBox);
        if (this.writeBox) this.node.unbox(this.writeBox);
        this.changeBox = this.writeBox = 0;
    }
    protected retire(): boolean {
        if (!this.h) return true;
        const ok = this.node.n.c.rant_variable_retire(this.h) === 0;
        if (ok) this.node.n.wake();
        return ok;
    }

    get schema(): CompiledSchema | null { return this.h ? this.node.compiledAt(this.node.n.c.rant_variable_schema(this.h)) : null; }

    /* The current value: the owner's store, or the latest a remote received. undefined before any. */
    get(): T | undefined {
        const h = this.h, node = this.node, n = node.n;
        if (!h) return undefined;
        return scoped(n, (sc) => {
            const b = sc.alloc(L.RantBytes.sizeof);
            if (n.c.rant_variable_get(h, b) !== 1) return undefined;
            const data = n.u32(b), len = n.u32(b + 4), schema = n.c.rant_variable_schema(h);
            return (schema ? node.decodeAt(schema, data, len, this.names) : n.copy(data, len)) as T;
        });
    }

    /* An owner applies and publishes, a remote sends the write to the owner. */
    set(value: T): SendStatus { return this.write(value, false); }
    /* Overrides the value until unforce. */
    force(value: T): SendStatus { return this.write(value, true); }
    unforce(): SendStatus {
        if (!this.h) return "no_topic";
        const r = this.node.n.c.rant_variable_unforce(this.h);
        this.node.n.wake();
        return sendStatus(r);
    }
    get forced(): boolean { return !!this.h && this.node.n.c.rant_variable_forced(this.h) === 1; }

    private write(value: T, force: boolean): SendStatus {
        const h = this.h, node = this.node, n = node.n;
        if (!h) return "no_topic";
        let r: number;
        try {
            r = scoped(n, (sc) => {
                const b = node.payload(sc, n.c.rant_variable_schema(h), this.names, value);
                return force ? n.c.rant_variable_force(h, b) : n.c.rant_variable_set(h, b);
            });
        } catch (e) {
            if (e instanceof SchemaError) return "schema";
            throw e;
        }
        n.wake();
        return sendStatus(r);
    }

    /* Resolves true once a value exists, false at the timeout. Negative waits with no end. */
    wait(timeoutMs = -1): Promise<boolean> {
        return this.node.until(() => this.get() !== undefined, timeoutMs);
    }

    /* owners matched at a remote, remotes matched at the owner */
    get matchCount(): number { return this.h ? this.node.n.c.rant_variable_match_count(this.h) : 0; }

    /* Each change of the value, and the current one at once. null stops them. */
    onChange(handler: ((value: T, update: VariableUpdate<T>) => void) | null): void {
        this.changeBox = this.watch(handler, this.changeBox, this.node.n.c.rant_variable_on_change);
    }
    /* Every applied write, even one that changes nothing. null stops them. */
    onWrite(handler: ((value: T, update: VariableUpdate<T>) => void) | null): void {
        this.writeBox = this.watch(handler, this.writeBox, this.node.n.c.rant_variable_on_write);
    }

    private watch(handler: ((value: T, update: VariableUpdate<T>) => void) | null, old: number,
                  register: (v: number, fn: number, user: number) => number): number {
        const h = this.h, node = this.node, n = node.n;
        if (!h) return 0;
        const box = handler ? node.box((p: number) => {
            const U = L.RantVariableUpdate;
            const data = n.u32(p + U.value), len = n.u32(p + U.value + 4), schema = n.u32(p + U.schema);
            let value: T;
            try { value = (schema ? node.decodeAt(schema, data, len, this.names) : n.copy(data, len)) as T; }
            catch (e) { node.report("schema_mismatch", `a value of '${this.name}' did not decode: ${errorText(e)}`, this.name); return; }
            try {
                handler(value, { value, data: n.copy(data, len), forced: n.u8(p + U.forced) !== 0, writeSeq: n.u32(p + U.write_seq),
                                 source: n.u32(p + U.source), recvUs: n.us(p + U.recv_us), writtenUs: n.us(p + U.written_us) });
            } catch (e) { node.report("none", `the handler of '${this.name}' threw: ${errorText(e)}`, this.name); }
        }) : 0;
        register(h, handler ? n.fn.update : 0, box);
        if (old) node.unbox(old);
        return box;
    }

    /* A reflectFromMesh variable takes the owner's type again when it moved. true when it changed. */
    refresh(): boolean { return !!this.h && this.node.n.c.rant_variable_refresh(this.h) === 1; }
}

export class VariableDefinition<T> extends Variable<T> {}
export class RemoteVariable<T> extends Variable<T> {}

/* ---- reflection --------------------------------------------------------------------------- */

/* The mesh as this node sees it. Every result is a copy. */
export class Reflection {
    /** @internal */ constructor(private node: RantNode) {}

    private walk<T>(size: number, next: (it: number, out: number) => number, read: (out: number) => T): T[] {
        const node = this.node, n = node.n;
        node.live();
        return scoped(n, (sc) => {
            const it = sc.alloc(L.RantIter.sizeof), out = sc.alloc(size), list: T[] = [];
            while (next(it, out)) list.push(read(out));
            return list;
        });
    }

    /* Every discovered peer, dropped ones too: gate on active. */
    peers(): Peer[] {
        const n = this.node.n, h = this.node.h, P = L.RantPeerInfo;
        return this.walk(P.sizeof, (it, out) => n.c.rant_node_peers_next(h, it, out), (p) => ({
            id: n.u32(p + P.id), uuid: [...n.copy(p + P.uuid, 16)].map((b) => b.toString(16).padStart(2, "0")).join(""),
            name: n.strAt(p + P.name), address: n.strAt(p + P.address), active: n.i32(p + P.liveness) === 0,
            lastHeardUs: n.us(p + P.last_heard_us), epoch: n.u32(p + P.epoch), catchingUp: n.u8(p + P.catching_up) !== 0,
            fragmentSize: n.u16(p + P.fragment_size), rttUs: n.u32(p + P.rtt_us), rttJitterUs: n.u32(p + P.rtt_jitter_us),
            rttMinUs: n.u32(p + P.rtt_min_us), rttSamples: n.u32(p + P.rtt_samples),
        }));
    }

    /* What one node offers, 0 for this one. A dropped peer serves its last known view. */
    entities(peer = 0): Entity[] {
        const n = this.node.n, h = this.node.h;
        return this.walk(L.RantEntityInfo.sizeof, (it, out) => n.c.rant_node_entities_next(h, peer, it, out), (p) => this.entity(p));
    }

    /* The whole mesh folded, one entity per kind and name. */
    mesh(): Entity[] {
        const n = this.node.n, h = this.node.h;
        return this.walk(L.RantEntityInfo.sizeof, (it, out) => n.c.rant_node_mesh_next(h, it, out), (p) => this.entity(p));
    }

    find(kind: EntityKind, name: string): Entity | null {
        const node = this.node, n = node.n, h = node.live();
        return scoped(n, (sc) => {
            const out = sc.alloc(L.RantEntityInfo.sizeof);
            return n.c.rant_node_mesh_find(h, ENTITY.indexOf(kind), sc.cstr(name), out) ? this.entity(out) : null;
        });
    }

    /* Moves whenever anything in the mesh changed: walk again only when it moved. */
    get epoch(): number { return this.node.h ? this.node.n.c.rant_node_mesh_epoch(this.node.h) >>> 0 : 0; }

    /* A peer's @rant/meta snapshot. sections is a MetaSection mask, 0 for all. */
    meta(peer: number, opts: { sections?: number; timeoutMs?: number } = {}): Promise<MetaSnapshot> {
        const node = this.node, n = node.n, h = node.live();
        const fn = n.c.rant_node_meta_function(h);
        if (!fn) return Promise.resolve({ valid: false, status: "no_handler", provider: 0, info: {} });
        const sections = opts.sections ?? 0;
        const req = sections ? new Uint8Array(new Uint32Array([sections]).buffer) : null;
        return startCall<unknown>(node, fn, null, null, req, peer, opts.timeoutMs ?? 1000, null).result.then((r) => {
            const info = r.ok && r.hasValue ? (r.value as { info?: Record<string, unknown> }).info : undefined;
            return { valid: !!info, status: r.status, provider: r.provider, info: info ?? {} };
        });
    }

    private entity(p: number): Entity {
        const node = this.node, n = node.n, E = L.RantEntityInfo;
        const own = (view: number): CompiledSchema | null => {
            if (!view) return null;
            return scoped(n, (sc) => {
                const w = sc.alloc(8);
                n.c.rant_schema_wire(w, view);
                return node.compiledAt(n.c.rant_node_schema_parse(node.h, n.u32(w), n.u32(w + 4)));
            });
        };
        const hash = n.u32(p + E.hash);
        const name = n.u32(p + E.name) ? n.strAt(p + E.name) : `0x${hash.toString(16).padStart(8, "0")}`;
        return {
            kind: ENTITY[n.i32(p + E.kind)] ?? "topic", name, hash,
            provides: n.u8(p + E.provides) !== 0, consumes: n.u8(p + E.consumes) !== 0, reliable: n.u8(p + E.reliable) !== 0,
            writable: n.u8(p + E.writable) !== 0, forceable: n.u8(p + E.forceable) !== 0, cancellable: n.u8(p + E.cancellable) !== 0,
            exclusive: n.u8(p + E.exclusive) !== 0, multi: n.u8(p + E.multi) !== 0, incomplete: n.u8(p + E.incomplete) !== 0,
            conflict: n.u8(p + E.conflict) !== 0, providers: n.u16(p + E.providers), consumers: n.u16(p + E.consumers),
            provider: n.u32(p + E.provider), from: n.strAt(p + E.from),
            schema: own(n.u32(p + E.schema)), schemaHash: n.u64(p + E.schema_hash),
            rspSchema: own(n.u32(p + E.rsp_schema)), rspSchemaHash: n.u64(p + E.rsp_schema_hash),
            progressSchema: own(n.u32(p + E.progress_schema)), progressSchemaHash: n.u64(p + E.progress_schema_hash),
            generation: n.u64(p + E.generation),
        };
    }
}
