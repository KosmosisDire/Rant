/* DART WebSocket bridge client: one DartClient = one full DART node on the mesh,
 * spoken through the bridge (see ../PROTOCOL.md). Zero dependencies, no build step:
 * runs in browsers, Node (>= 21), Deno and Bun off the global WebSocket. JSDoc-typed,
 * so TypeScript tooling gets full inference from the .mjs directly.
 *
 *   import { DartClient } from "./dart.mjs";
 *   const node = await DartClient.connect("ws://localhost:7480", { name: "dashboard" });
 *   const ch   = await node.channel("pose", "pubsub", {
 *       reliable: true, schema: "Pose { stamp: u64, x: f64, y: f64 }" });
 *   ch.onMessage = (m) => console.log(m.senderName, m.get("x"));
 *   ch.send({ stamp: 1n, x: 1.5, y: 2.0 });
 */

const OP_DATA = 0x01;

/** @typedef {{ path: string, kind: string, elem?: string, count?: number,
 *              offset: number, size: number }} Field */
/** @typedef {{ name: string, role: "pub"|"sub", reliable: boolean,
 *              hash?: string, size?: number, fields?: Field[] }} PeerTopic */
/** @typedef {{ id: number, name: string, addr: string, active: boolean,
 *              frag?: number, topics?: PeerTopic[] }} Peer */

const SCALAR_BYTES = { u8: 1, u16: 2, u32: 4, u64: 8, i8: 1, i16: 2, i32: 4, i64: 8,
                       f32: 4, f64: 8, bool: 1 };

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

/* A delivered message: raw bytes plus typed reads through the channel's field table. */
export class DartMessage {
    /** @param {DartChannel} channel @param {number} sender @param {Uint8Array} data
     *  @param {DartClient} client */
    constructor(channel, sender, data, client) {
        this.channel = channel;
        /** @type {number} peer id of the sending node */
        this.sender = sender;
        /** @type {string} sending node's name ("" until its peer_up was seen) */
        this.senderName = client.peers.get(sender)?.name ?? "";
        /** @type {Uint8Array} the payload, verbatim */
        this.data = data;
        this._view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    }

    /** Typed read of one field by dotted path ("vel.dx"). Scalars return number
     *  (u64/i64: bigint, bool: boolean); arrays return an Array of scalars (a u8 array
     *  returns a Uint8Array view); structs return a Uint8Array view of their bytes.
     *  @param {string} path @returns {number|bigint|boolean|Array<number|bigint|boolean>|Uint8Array} */
    get(path) {
        const f = this.channel.fields.get(path);
        if (!f) throw new Error(`no field '${path}'`);
        if (f.kind === "struct") return this.data.subarray(f.offset, f.offset + f.size);
        if (f.kind === "arr") {
            if (f.elem === "u8") return this.data.subarray(f.offset, f.offset + f.count);
            const n = SCALAR_BYTES[f.elem], out = new Array(f.count);
            for (let i = 0; i < f.count; i++) out[i] = readScalar(this._view, f.elem, f.offset + i * n);
            return out;
        }
        return readScalar(this._view, f.kind, f.offset);
    }

    /** A u8 array field decoded as UTF-8 text, trailing NULs stripped (or up to
     *  `lenField`'s value when given, the fixed-array-plus-count convention).
     *  @param {string} path @param {string=} lenField @returns {string} */
    text(path, lenField) {
        let bytes = /** @type {Uint8Array} */ (this.get(path));
        let n = lenField !== undefined ? Number(this.get(lenField)) : bytes.length;
        if (lenField === undefined) while (n > 0 && bytes[n - 1] === 0) n--;
        return new TextDecoder().decode(bytes.subarray(0, Math.min(n, bytes.length)));
    }
}

/* One channel (topic) on the node. Returned by DartClient.channel(). */
export class DartChannel {
    /** @param {DartClient} client @param {string} name
     *  @param {{id: number, size?: number, hash?: string, fields?: Field[],
     *           adopted?: boolean}} r */
    constructor(client, name, r) {
        this._client = client;
        /** @type {string} the topic name */
        this.name = name;
        /** @type {number} channel id in binary frames */
        this.id = r.id;
        /** @type {number|undefined} exact message size (typed channels) */
        this.size = r.size;
        /** @type {string|undefined} 64-bit schema identity, hex */
        this.hash = r.hash;
        /** @type {Map<string, Field>} dotted path -> field (typed channels) */
        this.fields = new Map((r.fields ?? []).map(f => [f.path, f]));
        /** @type {boolean|undefined} adopt requests only: whether a schema was found */
        this.adopted = r.adopted;
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
     *  Unset fields are zero. Array values may be Arrays, TypedArrays, or (for u8
     *  arrays) strings, shorter than the field: the tail stays zero.
     *  @param {Record<string, any>} values */
    send(values) {
        if (this.size === undefined) throw new Error(`'${this.name}' is a raw channel: use sendRaw`);
        const buf = new Uint8Array(this.size);
        const view = new DataView(buf.buffer);
        for (const [path, v] of Object.entries(values)) {
            const f = this.fields.get(path);
            if (!f) throw new Error(`no field '${path}'`);
            if (f.kind === "struct") throw new Error(`'${path}' is a struct: set its members`);
            if (f.kind === "arr") {
                const elems = typeof v === "string" ? new TextEncoder().encode(v) : v;
                if (elems.length > f.count) throw new Error(`'${path}': ${elems.length} > ${f.count} elements`);
                const n = SCALAR_BYTES[f.elem];
                for (let i = 0; i < elems.length; i++) writeScalar(view, f.elem, f.offset + i * n, elems[i]);
            } else {
                writeScalar(view, f.kind, f.offset, v);
            }
        }
        this.sendRaw(buf);
    }

    /** Flip this channel's role: "pubsub" | "pub" | "sub" | "inactive". */
    setRole(role) { return this._client._request({ op: "role", channel: this.id, role }); }

    /** Wait until every reader acked everything (reliable channels, before close). */
    async drain(timeout_ms = 1000) {
        const r = await this._client._request({ op: "drain", channel: this.id, timeout_ms });
        return /** @type {boolean} */ (r.drained);
    }
}

/* The node handle: one WebSocket connection = one DART node owned by the bridge. */
export class DartClient {
    /** Connect to a bridge and open the node.
     *  @param {string} url e.g. "ws://localhost:7480"
     *  @param {{ name?: string, domain?: number, max_channels?: number, max_peers?: number,
     *            interface?: string, seed_peers?: string[], fragment_size?: number,
     *            announce_interval_ms?: number, peer_timeout_ms?: number,
     *            memory?: number, disable_shm?: boolean }} [opts]
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
        /** @type {Map<number, DartChannel>} by channel id */
        this._channels = new Map();
        /** @type {string} this node's name (auto-generated if none was given) */
        this.name = "";
        /** @type {Map<number, Peer>} live peers, maintained from events */
        this.peers = new Map();
        /** @type {?(e: any) => void} every bridge event, verbatim */
        this.onEvent = null;
        /** @type {?(peers: Peer[]) => void} called whenever the peer set changes */
        this.onPeers = null;
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
            if (m.event === "peer_up") {
                this.peers.set(m.peer, { id: m.peer, name: m.name, addr: m.addr, active: true });
                this.onPeers?.([...this.peers.values()]);
            } else if (m.event === "peer_down") {
                const p = this.peers.get(m.peer);
                if (p) p.active = false;
                this.onPeers?.([...this.peers.values()]);
            }
            this.onEvent?.(m);
        }
    }

    /** @param {ArrayBuffer} buf */
    _onBinary(buf) {
        const b = new Uint8Array(buf);
        if (b.length < 7 || b[0] !== OP_DATA) return;   /* reserved ops: ignore */
        const id = b[1] | (b[2] << 8);
        const sender = b[3] | (b[4] << 8) | (b[5] << 16) | ((b[6] << 24) >>> 0);
        const ch = this._channels.get(id);
        ch?.onMessage?.(new DartMessage(ch, sender, b.subarray(7), this));
    }

    /** Create a channel (topic) on the node. `adopt: true` (and no schema text) makes
     *  the bridge use the schema a live peer advertises for this topic, so a debug tool
     *  can join typed topics it never declared.
     *  @param {string} name @param {"pubsub"|"pub"|"sub"|"inactive"} [role]
     *  @param {{ schema?: string, adopt?: boolean, reliable?: boolean, keep_last?: number, catch_up?: number,
     *            max_message_bytes?: number, heartbeat_ms?: number, repair_delay_ms?: number,
     *            backpressure_wait_ms?: number, shm_max_bytes?: number }} [opts]
     *  @returns {Promise<DartChannel>} */
    async channel(name, role = "pubsub", opts = {}) {
        const r = await this._request({ op: "channel", name, role, ...opts });
        const ch = new DartChannel(this, name, r);
        this._channels.set(ch.id, ch);
        return ch;
    }

    /** Snapshot the live peer table (peer events keep `this.peers` current anyway).
     *  @returns {Promise<Peer[]>} */
    async peersSnapshot() {
        const r = await this._request({ op: "peers" });
        this.peers = new Map(r.peers.map((p) => [p.id, p]));
        return r.peers;
    }

    /** Node counters (memory, backpressure, SHM). */
    async stats() { return this._request({ op: "stats" }); }

    /** Close the connection; the bridge closes the node with a BYE. */
    close() { this._ws.close(); }
}
