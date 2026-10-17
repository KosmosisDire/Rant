/* The net object for a browser. A page cannot open UDP sockets, so its node borrows them from
 * a Rant bridge and is a node of the bridge's host to the mesh. The bridge moves datagrams
 * and reads none. spec/tunnel.md has the protocol. */

import { dotted, naddr, type Interface, type NetObject, type Sink } from "./net.js";

const PROTOCOL = 13;
const UDP = 9;
const PING = 7;             /* [u8 7][u32 seq][u32 us][u32 jitter_us][u32 samples][u32 missed] */
const PONG = 8;             /* [u8 8][u32 seq] */
const PING_BYTES = 21;
const PONG_BYTES = 5;
const RECORD = 14;          /* [u16 sock][4 ip][u16 port][4 via][u16 len] before each datagram */
const BATCH = 60000;        /* the most one message carries */
const BACKLOG = 4 << 20;    /* past this a batch is dropped, as a full socket drops datagrams */
const CONTROL_CHANNEL = 0;
const TUNNEL_CHANNEL = 1;
const REQUEST_MS = 10000;   /* a request the bridge leaves unanswered this long fails */

export type ConnectionState = "connecting" | "open" | "reconnecting" | "closed";
export interface Rtt { us: number; jitterUs: number; samples: number; atUs: number }
export interface Connection { state: ConnectionState; carrier: "webrtc" | "websocket"; rtt: Rtt; missed: number }

/* kind is error, warning, resumed or restored, or an event the bridge pushes such as rtc. */
export interface TunnelEvent { kind: string; text: string; [field: string]: unknown }

export interface TunnelOptions {
    passphrase?: string;
    /* the session's name in the bridge's logs, and the hostname the node sees */
    name?: string;
    transport?: "auto" | "websocket";
    /* the RTCPeerConnection to use, the global one by default */
    rtc?: typeof RTCPeerConnection;
    iceServers?: RTCIceServer[];
    rtcTimeoutMs?: number;
    /* how often the bridge checks the link, which also bounds how fast a drop shows */
    probeIntervalMs?: number;
    /* sockets bound ahead, one for each node opened before the next refill lands */
    spares?: number;
    /* the address they bind to on the bridge's host, which must be the one each node binds
     * its data socket to: 127.0.0.1 for a node pinned to loopback, else any */
    address?: string;
    onEvent?: (e: TunnelEvent) => void;
}

/* A bridge on the network, as a bridge lists them. */
export interface FoundBridge { name: string; url: string; version: string; self: boolean }

/* The bridges on a network, as the bridge at url lists them. Every bridge answers the name
 * rant-bridges.local, so the default reaches whichever answers first. No passphrase needed. */
export async function findBridges(url = "ws://rant-bridges.local:7480", timeoutMs = 3000): Promise<FoundBridge[]> {
    const ws = new WebSocket(url);
    try {
        return await new Promise<FoundBridge[]>((resolve, reject) => {
            const timer = setTimeout(() => reject(new Error(`no bridge answered at ${url}`)), timeoutMs);
            ws.onerror = () => { clearTimeout(timer); reject(new Error(`cannot reach a bridge at ${url}`)); };
            ws.onopen = () => ws.send(JSON.stringify({ op: "bridges", seq: 1 }));
            ws.onmessage = (m) => {
                clearTimeout(timer);
                const r = JSON.parse(String(m.data));
                if (r.ok) resolve(r.bridges ?? []);
                else reject(new Error(`the bridge at ${url} lists no bridges, update it to 0.0.7 or later`));
            };
        });
    } finally {
        ws.close();
    }
}

/* What crossed the tunnel. Packets are datagrams, batches the messages that carried them. */
export interface TunnelStats {
    sentPackets: number; sentBatches: number; sentBytes: number;
    receivedPackets: number; receivedBatches: number; receivedBytes: number; dropped: number;
}

/* A socket of the node and what was asked of it, so a lost session can be set up again. */
interface Socket {
    sink: Sink;
    sock: number;           /* the bridge's number for it, 0 until bound */
    port: number;
    ifNaddr: number;
    reuse: boolean;
    via: number;            /* the interface group datagrams leave by, sent with each */
    options: Record<string, unknown>;
    groups: Map<string, { group: number; ifNaddr: number }>;
}

export class TunnelNet implements NetObject {
    readonly stats: TunnelStats = { sentPackets: 0, sentBatches: 0, sentBytes: 0, receivedPackets: 0, receivedBatches: 0, receivedBytes: 0, dropped: 0 };
    private nextId = 1;
    private nextSock = 1;
    private sockets = new Map<number, Socket>();
    private ids = new Map<number, number>();        /* the bridge's socket number to the node's id */
    private spares: { sock: number; port: number }[] = [];
    /* the batch being filled: what one task sends leaves as one message */
    private out = new Uint8Array(BATCH);
    private outView = new DataView(this.out.buffer);
    private outLen = 2;
    private outCount = 0;
    private flushing = false;

    private constructor(private readonly link: Link, private readonly spareCount: number, private readonly host: string,
                        private readonly address: string) {
        link.onBatch = (b) => this.received(b);
        link.onLost = () => this.restore();
    }

    /* Opens a tunnel session and binds the sockets the first node takes at once. Rejects when
     * the bridge cannot be reached or refuses. */
    static async connect(url: string, opts: TunnelOptions = {}): Promise<TunnelNet> {
        const link = new Link(url, opts);
        const net = new TunnelNet(link, opts.spares ?? 2, opts.name ?? "web", opts.address ?? "0.0.0.0");
        try {
            await link.start();
            await net.prepare();
        } catch (e) {
            link.close();
            throw e;
        }
        return net;
    }

    /* The link's state, its carrier and the round trip the bridge measures. */
    get connection(): Connection { return this.link.connection; }
    onConnectionChange(h: (c: Connection) => void): void { this.link.connHandlers.push(h); }
    onEvent(h: (e: TunnelEvent) => void): void { this.link.eventHandlers.push(h); }

    /* Binds spare sockets until there are enough for the next nodes. */
    async prepare(): Promise<void> {
        while (this.spares.length < this.spareCount) {
            const sock = this.newSock();
            const r = await this.link.call({ op: "udp_bind", sock, port: 0, if: this.address });
            this.spares.push({ sock, port: r.port });
        }
    }

    /* Ends the tunnel session and every socket in it. */
    shutdown(): void { this.link.close(); }

    /* ---- the net object ---- */

    open(sink: Sink): number {
        const id = this.nextId++;
        this.sockets.set(id, { sink, sock: 0, port: 0, ifNaddr: 0, reuse: false, via: 0, options: {}, groups: new Map() });
        return id;
    }

    bind(id: number, ifNaddr: number, port: number, reuse: boolean): number {
        const s = this.sockets.get(id);
        if (!s || s.sock) return 0;
        if (port === 0) {
            /* a node needs its port now, so it takes a socket bound ahead */
            if (ifNaddr !== naddr(this.address)) {
                this.link.warn("binding", `a node binds ${dotted(ifNaddr)} but the tunnel binds its spare sockets to ${this.address}, so connect with that address`);
                return 0;
            }
            const spare = this.spares.shift();
            if (!spare) return 0;
            s.ifNaddr = ifNaddr;
            this.adopt(id, s, spare.sock, spare.port);
            this.prepare().catch((e) => this.link.warn("binding a spare socket", e));
            return spare.port;
        }
        Object.assign(s, { ifNaddr, reuse });
        this.adopt(id, s, this.newSock(), port);
        this.ask({ op: "udp_bind", sock: s.sock, port, if: dotted(ifNaddr), reuse }, `binding port ${port}`);
        return port;
    }

    setMulticastInterface(id: number, ifNaddr: number): void {
        const s = this.sockets.get(id);
        if (s) s.via = ifNaddr;
    }
    setMulticastTTL(id: number, ttl: number): void { this.option(id, { ttl }); }
    setMulticastLoopback(id: number, on: boolean): void { this.option(id, { loop: on }); }
    ownGroupsOnly(id: number): void { this.option(id, { own_groups: true }); }

    membership(id: number, group: number, ifNaddr: number, join: boolean): boolean {
        const s = this.sockets.get(id);
        if (!s?.sock) return false;
        const key = `${group}/${ifNaddr}`;
        if (join) s.groups.set(key, { group, ifNaddr }); else s.groups.delete(key);
        this.ask({ op: "udp_member", sock: s.sock, group: dotted(group), if: dotted(ifNaddr), join }, `joining ${dotted(group)}`);
        return true;
    }

    /* bytes views wasm memory and is valid only during this call, so it is copied into the
     * batch, which leaves once this task is done. */
    send(id: number, bytes: Uint8Array, to: number, port: number): number {
        const s = this.sockets.get(id);
        if (!s?.sock) return -1;
        if (this.outLen + RECORD + bytes.length > BATCH) this.flush();
        const o = this.outLen, v = this.outView;
        v.setUint16(o, s.sock, true);
        v.setUint32(o + 2, to, true);
        v.setUint16(o + 6, port, true);
        v.setUint32(o + 8, s.via, true);
        v.setUint16(o + 12, bytes.length, true);
        this.out.set(bytes, o + RECORD);
        this.outLen += RECORD + bytes.length;
        this.outCount++;
        if (!this.flushing) {
            this.flushing = true;
            queueMicrotask(() => this.flush());
        }
        return bytes.length;
    }

    close(id: number): void {
        const s = this.sockets.get(id);
        this.sockets.delete(id);
        if (s?.sock) {
            this.ids.delete(s.sock);
            this.ask({ op: "udp_close", sock: s.sock }, "closing a socket");
        }
    }

    interfaces(): Interface[] { return this.link.interfaces; }
    hostname(): string { return this.host; }
    loopbackGroupBind(loopbackNaddr: number): number { return this.link.groupBindsAny ? 0 : loopbackNaddr; }

    /* ---- inside ---- */

    /* The next free number for a bridge socket, which wraps within the u16 a batch carries. */
    private newSock(): number {
        for (;;) {
            const sock = this.nextSock;
            this.nextSock = sock >= 65535 ? 1 : sock + 1;
            if (!this.ids.has(sock) && !this.spares.some((x) => x.sock === sock)) return sock;
        }
    }

    private adopt(id: number, s: Socket, sock: number, port: number): void {
        s.sock = sock;
        s.port = port;
        this.ids.set(sock, id);
    }

    private option(id: number, o: Record<string, unknown>): void {
        const s = this.sockets.get(id);
        if (!s?.sock) return;
        Object.assign(s.options, o);
        this.ask({ op: "udp_option", sock: s.sock, ...o }, "setting a socket option");
    }

    private ask(req: Record<string, unknown>, what: string): void {
        this.link.call(req).catch((e) => { if (!this.link.closed) this.link.warn(what, e); });
    }

    /* Sends the batch. A carrier with no room drops it whole, as a full socket drops datagrams. */
    private flush(): void {
        this.flushing = false;
        if (this.outCount === 0) return;
        this.out[0] = UDP;
        this.out[1] = 0;
        /* a copy, since a carrier may still hold what it was given when the next batch fills */
        if (this.link.sendBatch(this.out.slice(0, this.outLen))) {
            this.stats.sentPackets += this.outCount;
            this.stats.sentBatches++;
            this.stats.sentBytes += this.outLen;
        } else this.stats.dropped += this.outCount;
        this.outLen = 2;
        this.outCount = 0;
    }

    private received(frame: Uint8Array): void {
        const v = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);
        this.stats.receivedBatches++;
        this.stats.receivedBytes += frame.length;
        for (let at = 2; at + RECORD <= frame.length;) {
            const len = v.getUint16(at + 12, true);
            const id = this.ids.get(v.getUint16(at, true));
            if (at + RECORD + len > frame.length) return;
            const s = id === undefined ? undefined : this.sockets.get(id);
            if (s) {
                this.stats.receivedPackets++;
                s.sink.deliver(id!, v.getUint32(at + 2, true), v.getUint16(at + 6, true), frame.subarray(at + RECORD, at + RECORD + len));
            }
            at += RECORD + len;
        }
    }

    /* The bridge lost the session, and its sockets with it: every socket is bound again to the
     * port the node knows, with its options and groups, and new spares are bound. */
    private async restore(): Promise<void> {
        this.spares = [];
        for (const s of this.sockets.values()) {
            if (!s.sock) continue;
            try {
                await this.link.request({ op: "udp_bind", sock: s.sock, port: s.port, if: dotted(s.ifNaddr), reuse: s.reuse });
                if (Object.keys(s.options).length) await this.link.request({ op: "udp_option", sock: s.sock, ...s.options });
                for (const g of s.groups.values())
                    await this.link.request({ op: "udp_member", sock: s.sock, group: dotted(g.group), if: dotted(g.ifNaddr), join: true });
            } catch (e) { this.link.warn(`binding port ${s.port} again`, e); }
        }
        /* spares wait for the link to open, which waits for this */
        this.prepare().catch((e) => this.link.warn("binding a spare socket", e));
    }
}

/* The session with the bridge: the WebSocket, WebRTC beside it, the requests, the pings, and a
 * reconnect that resumes the session while the bridge still holds it. */
class Link {
    conn: Connection = { state: "connecting", carrier: "websocket", rtt: { us: 0, jitterUs: 0, samples: 0, atUs: 0 }, missed: 0 };
    interfaces: Interface[] = [];
    groupBindsAny = false;      /* the bridge's host hears a group only on a socket bound to any */
    eventHandlers: ((e: TunnelEvent) => void)[] = [];
    connHandlers: ((c: Connection) => void)[] = [];
    onBatch: (b: Uint8Array) => void = () => {};
    onLost: () => Promise<void> = async () => {};
    private ws: WebSocket | null = null;
    private pc: RTCPeerConnection | null = null;
    private channels = new Map<number, RTCDataChannel>();
    private token = "";
    private seq = 0;
    private pending = new Map<number, { resolve: (r: any) => void; reject: (e: Error) => void }>();
    private backoff = 250;
    private userClosed = false;
    private refused: Error | null = null;      /* the bridge said no: retrying cannot help */
    private waiters: { resolve: () => void; reject: (e: Error) => void }[] = [];

    constructor(readonly url: string, private readonly opts: TunnelOptions) {
        if (opts.onEvent) this.eventHandlers.push(opts.onEvent);
    }

    get connection(): Connection { return { ...this.conn, rtt: { ...this.conn.rtt } }; }

    /* Closed by its owner, so what was still asked of it no longer matters. */
    get closed(): boolean { return this.userClosed; }

    start(): Promise<void> { return this.open(); }

    close(): void {
        this.userClosed = true;
        if (this.ws) this.ws.close(); else this.setState("closed");
    }

    /* A request that waits out a reconnect and is sent again when the link dropped before the
     * answer. Only for requests that may run twice, as every socket op may. */
    async call(obj: Record<string, unknown>): Promise<any> {
        for (let tries = 0; ; tries++) {
            await this.whenOpen();
            try { return await this.request(obj); }
            catch (e) { if ((e as { dropped?: boolean }).dropped !== true || tries >= 3) throw e; }
        }
    }

    request(obj: Record<string, unknown>): Promise<any> {
        const ws = this.ws;
        if (!ws || ws.readyState !== WebSocket.OPEN) return Promise.reject(new Error("not connected to the bridge"));
        const seq = ++this.seq;
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                if (this.pending.delete(seq)) reject(new Error(`the bridge did not answer '${obj.op}' in time`));
            }, REQUEST_MS);
            this.pending.set(seq, {
                resolve: (r) => { clearTimeout(timer); resolve(r); },
                reject: (e) => { clearTimeout(timer); reject(e); },
            });
            ws.send(JSON.stringify({ ...obj, seq }));
        });
    }

    /* A batch on the tunnel channel while it is open, else the WebSocket. One carrier at a time,
     * so datagrams keep their order. False past a socket's worth of backlog. */
    sendBatch(b: Uint8Array<ArrayBuffer>): boolean {
        const dc = this.channels.get(TUNNEL_CHANNEL);
        if (dc && dc.readyState === "open") {
            if (dc.bufferedAmount > BACKLOG) return false;
            dc.send(b);
            return true;
        }
        const ws = this.ws;
        if (!ws || ws.readyState !== WebSocket.OPEN || ws.bufferedAmount > BACKLOG) return false;
        ws.send(b);
        return true;
    }

    warn(what: string, e: unknown): void {
        this.emit({ kind: "warning", text: `${what}: ${(e as Error)?.message ?? e}` });
    }

    private emit(e: TunnelEvent): void {
        if (!this.eventHandlers.length && (e.kind === "error" || e.kind === "warning")) console.warn(`rant tunnel: ${e.text}`);
        for (const h of this.eventHandlers) h(e);
    }

    private setState(state: ConnectionState, carrier = this.conn.carrier): void {
        if (this.conn.state === state && this.conn.carrier === carrier) return;
        this.conn.state = state;
        this.conn.carrier = carrier;
        if (state === "open" || state === "closed") {
            for (const w of this.waiters.splice(0)) state === "open" ? w.resolve() : w.reject(new Error("the tunnel is closed"));
        }
        for (const h of this.connHandlers) h(this.connection);
    }

    private whenOpen(): Promise<void> {
        if (this.conn.state === "open") return Promise.resolve();
        if (this.conn.state === "closed") return Promise.reject(new Error("the tunnel is closed"));
        return new Promise((resolve, reject) => this.waiters.push({ resolve, reject }));
    }

    private async open(): Promise<void> {
        const resuming = !!this.token;
        const ws = new WebSocket(this.url);
        ws.binaryType = "arraybuffer";
        await new Promise<void>((res, rej) => {
            ws.onopen = () => res();
            ws.onerror = () => rej(new Error(`cannot reach the bridge at ${this.url}`));
        });
        this.ws = ws;
        ws.onmessage = (m) => typeof m.data === "string" ? this.text(JSON.parse(m.data)) : this.binary(new Uint8Array(m.data as ArrayBuffer));
        ws.onclose = (e) => this.dropped(ws, e.code ? ` (${e.code}${e.reason ? " " + e.reason : ""})` : "");
        const r = await this.request({ op: "open", proto: PROTOCOL, passphrase: this.opts.passphrase,
                                       name: this.opts.name, resume: this.token || undefined, probe_interval_ms: this.opts.probeIntervalMs })
            .catch((e: Error & { answered?: boolean }) => { if (e.answered) this.refused = e; throw e; });
        this.token = r.resume_token;
        this.interfaces = (r.interfaces ?? []).map((f: { addr: string; mask: string }) => ({ addr: naddr(f.addr), mask: naddr(f.mask) }));
        this.groupBindsAny = r.group_binds_any === true;
        this.backoff = 250;
        if (resuming && !r.resumed) await this.onLost();
        this.setState("open", "websocket");
        if (resuming) this.emit(r.resumed ? { kind: "resumed", text: "the bridge session resumed" }
                                          : { kind: "restored", text: "the bridge had lost the session, every socket is bound again" });
        /* the WebSocket carries everything until WebRTC is up, so nothing waits for it */
        if (r.webrtc && this.opts.transport !== "websocket")
            this.rtcConnect().catch((e) => this.warn("WebRTC did not connect, the WebSocket carries everything", e));
    }

    private dropped(ws: WebSocket, why: string): void {
        if (this.ws !== ws) return;
        this.ws = null;
        this.rtcDrop();
        for (const p of this.pending.values()) p.reject(Object.assign(new Error("the connection to the bridge dropped" + why), { dropped: true }));
        this.pending.clear();
        if (this.userClosed || this.refused) {
            if (this.refused && !this.userClosed) this.emit({ kind: "error", text: `the bridge refused the tunnel: ${this.refused.message}` });
            this.setState("closed");
            return;
        }
        this.setState("reconnecting", "websocket");
        const retry = () => {
            if (this.userClosed) return;
            this.open().catch((e) => {
                this.emit({ kind: "error", text: `reconnecting failed: ${e?.message ?? e}` });
                this.backoff = Math.min(this.backoff * 2, 5000);
                setTimeout(retry, this.backoff);
            });
        };
        setTimeout(retry, this.backoff);
    }

    private text(m: any): void {
        switch (m.op) {
        case "reply": {
            const p = this.pending.get(m.seq);
            if (!p) return;
            this.pending.delete(m.seq);
            if (m.ok) p.resolve(m);
            else p.reject(Object.assign(new Error(m.error ?? "refused"), { answered: true }));
            return;
        }
        case "event": {
            const { op: _op, event, ...rest } = m;
            this.emit({ ...rest, kind: event, text: m.text ?? event });
            return;
        }
        case "warning": this.emit({ kind: "warning", text: m.text }); return;
        case "rtc":
            if (this.pc && m.candidate) this.pc.addIceCandidate({ candidate: m.candidate, sdpMid: m.mid }).catch(() => {});
            return;
        }
    }

    private binary(b: Uint8Array): void {
        if (b[0] === UDP) this.onBatch(b);
        else if (b[0] === PING) this.ping(b);
    }

    /* The bridge measures the link with pings and says in each what it saw. */
    private ping(b: Uint8Array): void {
        if (b.length < PING_BYTES) return;
        const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
        this.conn.rtt = { us: v.getUint32(5, true), jitterUs: v.getUint32(9, true), samples: v.getUint32(13, true), atUs: Date.now() * 1000 };
        this.conn.missed = v.getUint32(17, true);
        const pong = b.slice(0, PONG_BYTES);
        pong[0] = PONG;
        const dc = this.channels.get(CONTROL_CHANNEL);
        if (dc && dc.readyState === "open") dc.send(pong);
        else this.ws?.send(pong);
    }

    /* One attempt at WebRTC, done or given up within rtcTimeoutMs. A failed attempt drops its own
     * link only, since a reconnect meanwhile may have started a newer one. */
    private async rtcConnect(): Promise<void> {
        const Pc = this.opts.rtc ?? (globalThis as any).RTCPeerConnection;
        if (!Pc) return;
        let pc: RTCPeerConnection | null = null;
        const attempt = (async () => {
            const r = await this.request({ op: "rtc" });
            const p: RTCPeerConnection = pc = new Pc({ iceServers: [...(r.ice_servers ?? []), ...(this.opts.iceServers ?? [])] });
            this.pc = p;
            p.onicecandidate = (e) => {
                if (e.candidate?.candidate) this.request({ op: "rtc", candidate: e.candidate.candidate, mid: e.candidate.sdpMid ?? "" }).catch(() => {});
            };
            this.channel(p, CONTROL_CHANNEL, true);
            this.channel(p, TUNNEL_CHANNEL, false);
            const up = new Promise<void>((res, rej) => {
                p.onconnectionstatechange = () => {
                    const s = p.connectionState;
                    if (s === "connected") { this.setState("open", "webrtc"); res(); }
                    else if (s === "failed" || s === "closed") { if (this.pc === p) this.rtcDrop(); rej(new Error(s)); }
                    else if (s === "disconnected") this.setState(this.conn.state, "websocket");
                };
            });
            up.catch(() => {});
            const offer = await p.createOffer();
            await p.setLocalDescription(offer);
            const a = await this.request({ op: "rtc", sdp: p.localDescription?.sdp ?? offer.sdp });
            await p.setRemoteDescription({ type: "answer", sdp: a.sdp });
            await up;
        })();
        let timer: ReturnType<typeof setTimeout> | undefined;
        try {
            await Promise.race([attempt, new Promise((_, rej) => { timer = setTimeout(() => rej(new Error("no connection in time")), this.opts.rtcTimeoutMs ?? 4000); })]);
        } catch (e) {
            attempt.catch(() => {});
            if (pc && this.pc !== pc) return;     /* a drop or a newer attempt replaced it */
            if (pc) this.rtcDrop();
            throw e;
        } finally {
            clearTimeout(timer);
        }
    }

    /* Channels are negotiated: the id is agreed, so neither side announces it. */
    private channel(pc: RTCPeerConnection, id: number, reliable: boolean): void {
        const dc = pc.createDataChannel(`d${id}`, reliable ? { negotiated: true, id, ordered: true }
                                                            : { negotiated: true, id, ordered: false, maxRetransmits: 0 });
        dc.binaryType = "arraybuffer";
        dc.onmessage = (e) => this.binary(new Uint8Array(e.data as ArrayBuffer));
        this.channels.set(id, dc);
    }

    private rtcDrop(): void {
        const pc = this.pc;
        this.pc = null;
        this.channels.clear();
        if (pc) { pc.onconnectionstatechange = null; pc.onicecandidate = null; try { pc.close(); } catch { /* already closed */ } }
        if (this.conn.state === "open") this.setState("open", "websocket");
    }
}

