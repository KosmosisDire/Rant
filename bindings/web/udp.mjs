// A net object over Node's UDP sockets, so Rant in wasm runs in Node.js as a node of this
// machine. A bind to port 0 takes a socket bound ahead of time, since a node needs its
// port at once and Node binds asynchronously. spec/web.md has the net object contract.

import dgram from "node:dgram";
import os from "node:os";

// A naddr is the four address bytes as one little endian u32, as wasm memory holds them.
export const naddr = (dotted) => {
    const p = dotted.split(".").map(Number);
    return (p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)) >>> 0;
};
export const dotted = (n) => [n & 255, (n >>> 8) & 255, (n >>> 16) & 255, n >>> 24].join(".");

export class UdpNet {
    #sink = null;
    #next = 1;
    #sockets = new Map();     // id to { sock, ready, pending }
    #spares = [];             // { sock, port }, bound to an ephemeral port

    // Binds the sockets a node takes at once. Call before opening each node.
    static async create(spares = 2) {
        const net = new UdpNet();
        await net.prepare(spares);
        return net;
    }

    async prepare(spares = 2) {
        while (this.#spares.length < spares) {
            const sock = dgram.createSocket({ type: "udp4" });
            await new Promise((res, rej) => { sock.once("error", rej); sock.bind(0, () => { sock.off("error", rej); res(); }); });
            this.#spares.push({ sock, port: sock.address().port });
        }
    }

    attach(sink) { this.#sink = sink; }

    open() {
        const id = this.#next++;
        this.#sockets.set(id, { sock: null, ready: false, pending: [] });
        return id;
    }

    bind(id, ifNaddr, port, reuse) {
        const s = this.#sockets.get(id);
        if (!s || s.sock) return 0;
        if (port === 0) {
            const spare = this.#spares.shift();
            if (!spare) return 0;
            this.#adopt(id, s, spare.sock, true);
            this.prepare().catch(() => {});
            return spare.port;
        }
        // a fixed port binds in the background, and what the node asks of it meanwhile waits
        const sock = dgram.createSocket({ type: "udp4", reuseAddr: !!reuse });
        this.#adopt(id, s, sock, false);
        sock.bind(port, ifNaddr ? dotted(ifNaddr) : undefined, () => {
            s.ready = true;
            for (const f of s.pending.splice(0)) f();
        });
        return port;
    }

    #adopt(id, s, sock, ready) {
        s.sock = sock;
        s.ready = ready;
        sock.on("message", (msg, from) => this.#sink?.deliver(id, naddr(from.address), from.port, msg));
        sock.on("error", (e) => console.warn(`rant: udp socket ${id}: ${e.message}`));
    }

    // Runs f once the socket is bound, now when it already is.
    #when(id, f) {
        const s = this.#sockets.get(id);
        if (!s?.sock) return false;
        if (s.ready) f(s.sock); else s.pending.push(() => f(s.sock));
        return true;
    }

    setMulticastInterface(id, ifNaddr) { this.#when(id, (k) => k.setMulticastInterface(dotted(ifNaddr))); }
    setMulticastTTL(id, ttl) { this.#when(id, (k) => k.setMulticastTTL(ttl)); }
    setMulticastLoopback(id, on) { this.#when(id, (k) => k.setMulticastLoopback(on)); }
    ownGroupsOnly(_id) { /* Node has no IP_MULTICAST_ALL, and only Linux needs it */ }
    setReceiveBufferSize(id, bytes) { this.#when(id, (k) => k.setRecvBufferSize(bytes)); }
    setSendBufferSize(id, bytes) { this.#when(id, (k) => k.setSendBufferSize(bytes)); }

    membership(id, group, ifNaddr, join) {
        return this.#when(id, (k) => {
            try {
                if (join) k.addMembership(dotted(group), ifNaddr ? dotted(ifNaddr) : undefined);
                else k.dropMembership(dotted(group), ifNaddr ? dotted(ifNaddr) : undefined);
            } catch (e) { console.warn(`rant: udp membership ${dotted(group)} on ${dotted(ifNaddr)}: ${e.message}`); }
        });
    }

    // bytes is a view into wasm memory, valid during this call only, so it is copied.
    send(id, bytes, to, port) {
        const s = this.#sockets.get(id);
        if (!s?.sock) return -1;
        s.sock.send(Buffer.from(bytes), port, dotted(to));
        return bytes.length;
    }

    close(id) {
        const s = this.#sockets.get(id);
        this.#sockets.delete(id);
        try { s?.sock?.close(); } catch { /* already closed */ }
    }

    // Closes every socket, the spares too.
    shutdown() {
        for (const id of [...this.#sockets.keys()]) this.close(id);
        for (const s of this.#spares.splice(0)) s.sock.close();
    }

    interfaces() {
        const out = [];
        for (const list of Object.values(os.networkInterfaces()))
            for (const f of list ?? [])
                if (f.family === "IPv4" && !f.internal) out.push({ addr: naddr(f.address), mask: naddr(f.netmask) });
        return out;
    }

    hostname() { return os.hostname(); }
}
