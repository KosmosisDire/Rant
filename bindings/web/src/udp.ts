/* The net object over Node's UDP sockets, so Rant in wasm runs in Node.js as a node of this
 * machine. A bind to port 0 takes a socket bound ahead of time, since a node needs its port
 * at once and Node binds asynchronously. */

import dgram from "node:dgram";
import os from "node:os";
import { dotted, naddr, type Interface, type NetObject, type Sink } from "./net.js";

interface Socket { sink: Sink; sock: dgram.Socket | null; ready: boolean; pending: (() => void)[] }

export interface UdpNetOptions {
    /* sockets bound ahead, one for each node opened before the next refill lands */
    spares?: number;
    /* the address they bind to, which must be the one each node binds its data socket to:
     * 127.0.0.1 for a node pinned to loopback, else any */
    address?: string;
}

export class UdpNet implements NetObject {
    private nextId = 1;
    private sockets = new Map<number, Socket>();
    private spares: { sock: dgram.Socket; port: number }[] = [];
    private shut = false;

    private constructor(private readonly spareCount: number, private readonly address: string) {}

    /* Binds the sockets the first node takes at once. */
    static async create(opts: UdpNetOptions = {}): Promise<UdpNet> {
        const net = new UdpNet(opts.spares ?? 2, opts.address ?? "0.0.0.0");
        await net.prepare();
        return net;
    }

    /* Binds spare sockets until there are enough for the next nodes. */
    async prepare(): Promise<void> {
        while (!this.shut && this.spares.length < this.spareCount) {
            const sock = dgram.createSocket({ type: "udp4" });
            await new Promise<void>((res, rej) => {
                sock.once("error", rej);
                sock.bind(0, this.address, () => { sock.off("error", rej); res(); });
            });
            /* a refill that lands after shutdown would keep the process alive */
            if (this.shut) sock.close();
            else this.spares.push({ sock, port: sock.address().port });
        }
    }

    /* Closes every socket, the spares too, and binds no more. */
    shutdown(): void {
        this.shut = true;
        for (const id of [...this.sockets.keys()]) this.close(id);
        for (const s of this.spares.splice(0)) s.sock.close();
    }

    open(sink: Sink): number {
        const id = this.nextId++;
        this.sockets.set(id, { sink, sock: null, ready: false, pending: [] });
        return id;
    }

    bind(id: number, ifNaddr: number, port: number, reuse: boolean): number {
        const s = this.sockets.get(id);
        if (!s || s.sock) return 0;
        if (port === 0) {
            if (ifNaddr !== naddr(this.address)) {
                console.warn(`rant: a node binds ${dotted(ifNaddr)} but UdpNet binds its spare sockets to ${this.address}, so create it with that address`);
                return 0;
            }
            const spare = this.spares.shift();
            if (!spare) return 0;
            this.adopt(id, s, spare.sock, true);
            this.prepare().catch((e) => console.warn(`rant: binding a spare socket: ${e?.message ?? e}`));
            return spare.port;
        }
        /* a fixed port binds in the background, and what the node asks of it meanwhile waits */
        const sock = dgram.createSocket({ type: "udp4", reuseAddr: reuse });
        this.adopt(id, s, sock, false);
        sock.bind(port, ifNaddr ? dotted(ifNaddr) : undefined, () => {
            s.ready = true;
            for (const f of s.pending.splice(0)) f();
        });
        return port;
    }

    setMulticastInterface(id: number, ifNaddr: number): void { this.when(id, (k) => k.setMulticastInterface(dotted(ifNaddr))); }
    setMulticastTTL(id: number, ttl: number): void { this.when(id, (k) => k.setMulticastTTL(ttl)); }
    setMulticastLoopback(id: number, on: boolean): void { this.when(id, (k) => k.setMulticastLoopback(on)); }
    ownGroupsOnly(_id: number): void { /* Node has no IP_MULTICAST_ALL, and only Linux needs it */ }
    setReceiveBufferSize(id: number, bytes: number): void { this.when(id, (k) => k.setRecvBufferSize(bytes)); }
    setSendBufferSize(id: number, bytes: number): void { this.when(id, (k) => k.setSendBufferSize(bytes)); }

    membership(id: number, group: number, ifNaddr: number, join: boolean): boolean {
        return this.when(id, (k) => {
            const on = ifNaddr ? dotted(ifNaddr) : undefined;
            try {
                if (join) k.addMembership(dotted(group), on);
                else k.dropMembership(dotted(group), on);
            } catch (e) { console.warn(`rant: udp membership ${dotted(group)} on ${dotted(ifNaddr)}: ${(e as Error).message}`); }
        });
    }

    /* bytes views wasm memory and is valid during this call only, so it is copied. */
    send(id: number, bytes: Uint8Array, to: number, port: number): number {
        const s = this.sockets.get(id);
        if (!s?.sock) return -1;
        s.sock.send(Buffer.from(bytes), port, dotted(to));
        return bytes.length;
    }

    close(id: number): void {
        const s = this.sockets.get(id);
        this.sockets.delete(id);
        try { s?.sock?.close(); } catch { /* already closed */ }
    }

    interfaces(): Interface[] {
        const out: Interface[] = [];
        for (const list of Object.values(os.networkInterfaces()))
            for (const f of list ?? [])
                if (f.family === "IPv4" && !f.internal) out.push({ addr: naddr(f.address), mask: naddr(f.netmask) });
        return out;
    }

    hostname(): string { return os.hostname(); }

    loopbackGroupBind(loopbackNaddr: number): number { return process.platform === "win32" ? loopbackNaddr : 0; }

    private adopt(id: number, s: Socket, sock: dgram.Socket, ready: boolean): void {
        s.sock = sock;
        s.ready = ready;
        sock.on("message", (msg, from) => s.sink.deliver(id, naddr(from.address), from.port, msg));
        sock.on("error", (e) => console.warn(`rant: udp socket ${id}: ${e.message}`));
    }

    /* Runs f once the socket is bound, now when it already is. */
    private when(id: number, f: (sock: dgram.Socket) => void): boolean {
        const s = this.sockets.get(id);
        if (!s?.sock) return false;
        const sock = s.sock;
        if (s.ready) f(sock); else s.pending.push(() => f(sock));
        return true;
    }
}
