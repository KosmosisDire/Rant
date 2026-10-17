/* The net object: the sockets a Rant node in wasm runs on, which the host sets as
 * Module.rantNet before the first node opens. spec/web.md has the contract. */

/* Where a socket's datagrams go: the module instance that opened it. bytes is valid during the
 * call only. */
export interface Sink {
    deliver(id: number, naddr: number, port: number, bytes: Uint8Array): void;
}

/* An IPv4 interface of the host whose sockets these are, as naddrs. */
export interface Interface { addr: number; mask: number }

export interface NetObject {
    /* a new socket id, above 0, whose datagrams go to sink */
    open(sink: Sink): number;
    /* the bound port now, 0 when it cannot be bound now */
    bind(id: number, ifNaddr: number, port: number, reuse: boolean): number;
    setMulticastInterface(id: number, ifNaddr: number): void;
    setMulticastTTL(id: number, ttl: number): void;
    setMulticastLoopback(id: number, on: boolean): void;
    ownGroupsOnly(id: number): void;
    setReceiveBufferSize?(id: number, bytes: number): void;
    setSendBufferSize?(id: number, bytes: number): void;
    /* true when the socket will join or leave */
    membership(id: number, group: number, ifNaddr: number, join: boolean): boolean;
    /* bytes sent, or -1. bytes views wasm memory and is valid during the call only */
    send(id: number, bytes: Uint8Array, naddr: number, port: number): number;
    close(id: number): void;
    interfaces(): Interface[];
    hostname(): string;
    /* where a node pinned to loopback binds its discovery socket: loopback where the host
     * hands it group traffic (Windows, the default), else 0 for any, as POSIX needs */
    loopbackGroupBind?(loopbackNaddr: number): number;
}

/* A naddr is the four address bytes read as one little endian u32, as wasm memory holds them. */
export const naddr = (dotted: string): number => {
    const p = dotted.split(".").map(Number);
    return (p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)) >>> 0;
};

export const dotted = (n: number): string => [n & 255, (n >>> 8) & 255, (n >>> 16) & 255, n >>> 24].join(".");
