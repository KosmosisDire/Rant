/* One instance of Rant built to wasm, whose sockets come from one net object, and the memory
 * and callback work every node on it shares. Nodes opened on the same net object share it. */

import createRant from "./rant_wasm.mjs";
import * as L from "./layout.js";
import type { NetObject } from "./net.js";

type Fn = (...args: any[]) => any;

interface Module {
    HEAPU8: Uint8Array;
    addFunction(f: Fn, sig: string): number;
    rantWeb: { drive(f: () => void): void; wake(ms: number): void };
    [name: string]: any;
}

const utf8 = new TextEncoder();
const text = new TextDecoder();

/* The C callback kinds. Each passes a user value naming the box that handles it. */
type Callback = "message" | "event" | "request" | "response" | "progress" | "update" | "cancel";

let versionOf: Promise<string> | undefined;

export class Native {
    /* the exported C functions without their leading underscore */
    readonly c: Record<string, Fn> = {};
    /* a C function pointer per callback kind, for the create and call functions */
    readonly fn: Record<Callback, number>;
    private boxes = new Map<number, Fn>();
    private nextBox = 1;
    private drives = new Set<() => void>();
    private dv: DataView | null = null;
    /* above 0 while a node's queued callbacks run, where a close has to wait until they end */
    dispatching = 0;

    private constructor(private readonly m: Module) {
        for (const k of Object.keys(m)) if (k.startsWith("_")) this.c[k.slice(1)] = m[k];
        /* the user value sits in the struct for some kinds and comes as an argument for others,
         * and a node's messages and events share one, so the kind goes along */
        const at = (field: number, kind: Callback) => (p: number) => this.route(this.u32(p + field), p, kind);
        const arg = (kind: Callback) => (p: number, user: number) => this.route(user, p, kind);
        this.fn = {
            message: m.addFunction(at(L.RantMsg.user, "message"), "vi"),
            event: m.addFunction(at(L.RantEvent.user, "event"), "vi"),
            request: m.addFunction(arg("request"), "vii"),
            response: m.addFunction(at(L.RantResponse.user, "response"), "vi"),
            progress: m.addFunction(at(L.RantProgress.user, "progress"), "vi"),
            update: m.addFunction(arg("update"), "vii"),
            cancel: m.addFunction((token: bigint, user: number) => this.route(user, token, "cancel"), "vji"),
        };
        m.rantWeb.drive(() => { for (const d of [...this.drives]) d(); });
    }

    static load(net: NetObject): Promise<Native> {
        let p = loaded.get(net);
        if (!p) {
            p = createRant({ rantNet: net }).then((m) => new Native(m as Module));
            loaded.set(net, p);
        }
        return p;
    }

    /* The C library's version, from an instance of its own, which opens no socket. */
    static version(): Promise<string> {
        versionOf ??= createRant({}).then((m) => {
            const h = (m as Module).HEAPU8, p = (m as Module)._rant_version() as number;
            let end = p;
            while (h[end]) end++;
            return text.decode(h.subarray(p, end));
        });
        return versionOf;
    }

    /* A JavaScript exception must never unwind through C frames, so each callback stops here. */
    private route(user: number, arg: unknown, kind: Callback): void {
        const f = this.boxes.get(user);
        if (!f) return;
        try { f(arg, kind); }
        catch (e) { console.error("rant: a callback threw:", e); }
    }

    /* A callback's handler, named by the number the C hands back. */
    box(f: Fn): number {
        const id = this.nextBox++;
        this.boxes.set(id, f);
        return id;
    }
    unbox(id: number): void { this.boxes.delete(id); }

    /* f runs at every drive: when a datagram arrives and when a node's timer is due. */
    drive(f: () => void): void { this.drives.add(f); }
    undrive(f: () => void): void { this.drives.delete(f); }
    /* A drive right after the current task, so what was just sent goes out. */
    wake(): void { this.m.rantWeb.wake(0); }

    /* ---- memory ---- */

    private view(): DataView {
        const b = this.m.HEAPU8.buffer;
        if (this.dv?.buffer !== b) this.dv = new DataView(b);
        return this.dv;
    }
    get heap(): Uint8Array { return this.m.HEAPU8; }
    u8(p: number): number { return this.m.HEAPU8[p]; }
    u16(p: number): number { return this.view().getUint16(p, true); }
    u32(p: number): number { return this.view().getUint32(p, true); }
    i32(p: number): number { return this.view().getInt32(p, true); }
    u64(p: number): bigint { return this.view().getBigUint64(p, true); }
    i64(p: number): bigint { return this.view().getBigInt64(p, true); }
    f64(p: number): number { return this.view().getFloat64(p, true); }
    /* a microsecond count, exact in a double for any wall clock time */
    us(p: number): number { return Number(this.view().getBigUint64(p, true)); }
    setU8(p: number, v: number): void { this.m.HEAPU8[p] = v; }
    setU16(p: number, v: number): void { this.view().setUint16(p, v, true); }
    setU32(p: number, v: number): void { this.view().setUint32(p, v >>> 0, true); }
    setI32(p: number, v: number): void { this.view().setInt32(p, v, true); }
    setU64(p: number, v: bigint): void { this.view().setBigUint64(p, v, true); }
    setI64(p: number, v: bigint): void { this.view().setBigInt64(p, v, true); }
    setF64(p: number, v: number): void { this.view().setFloat64(p, v, true); }

    /* n zeroed bytes of the C heap, freed with free. */
    alloc(n: number): number {
        const p = this.c.malloc(Math.max(n, 1));
        if (!p) throw new Error("rant: out of wasm memory");
        this.m.HEAPU8.fill(0, p, p + n);
        return p;
    }
    free(p: number): void { if (p) this.c.free(p); }

    copy(p: number, len: number): Uint8Array { return len ? this.m.HEAPU8.slice(p, p + len) : new Uint8Array(0); }
    str(p: number, len: number): string { return len ? text.decode(this.m.HEAPU8.subarray(p, p + len)) : ""; }
    /* a RantString or RantBytes struct at p */
    strAt(p: number): string { return this.str(this.u32(p), this.u32(p + 4)); }
    bytesAt(p: number): Uint8Array { return this.copy(this.u32(p), this.u32(p + 4)); }
    cstr(p: number): string | null {
        if (!p) return null;
        const h = this.m.HEAPU8;
        let end = p;
        while (h[end]) end++;
        return text.decode(h.subarray(p, end));
    }
}

const loaded = new WeakMap<NetObject, Promise<Native>>();

/* Allocations for one call into C, freed together when it returns. */
export class Scope {
    private ptrs: number[] = [];
    constructor(readonly n: Native) {}
    alloc(size: number): number { return this.own(this.n.alloc(size)); }
    /* a block allocated elsewhere, freed with the scope */
    own(p: number): number {
        this.ptrs.push(p);
        return p;
    }
    data(b: Uint8Array): number {
        const p = this.alloc(b.length);
        this.n.heap.set(b, p);
        return p;
    }
    cstr(s: string): number {
        const b = utf8.encode(s);
        const p = this.alloc(b.length + 1);
        this.n.heap.set(b, p);
        return p;
    }
    /* a RantBytes struct over bytes, for the functions that take one by value */
    bytes(b: Uint8Array | null): number { return this.view(b ? this.data(b) : 0, b ? b.length : 0); }
    view(ptr: number, len: number): number {
        const s = this.alloc(L.RantBytes.sizeof);
        this.n.setU32(s + L.RantBytes.data, ptr);
        this.n.setU32(s + L.RantBytes.len, len);
        return s;
    }
    free(): void {
        for (const p of this.ptrs) this.n.free(p);
        this.ptrs.length = 0;
    }
}

/* f with a scope of its own. */
export function scoped<T>(n: Native, f: (s: Scope) => T): T {
    const s = new Scope(n);
    try { return f(s); }
    finally { s.free(); }
}

export const encodeUtf8 = (s: string): Uint8Array => utf8.encode(s);
export const decodeUtf8 = (b: Uint8Array): string => text.decode(b);
