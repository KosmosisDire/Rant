# Rant in wasm

`bindings/web` builds the C library to wasm with Emscripten (`cmake --preset web`). The node
keeps every rule of the native one. Only the platform layer differs: `platform.c`
implements `src/platform/core.h` over a net object the host supplies, the way the Yaskawa
build supplies its own under `RANT_PLAT_CUSTOM`. The build sets `RANT_NO_THREADS`,
`RANT_NO_SHM` and `RANT_NO_PROC_STATS`, publicly, so an app's headers agree with the
library.

| File | Is |
|---|---|
| `platform.c`, `rant_web.h` | the platform layer and the driver API an app calls |
| `rant_web.js` | their Emscripten library: the net imports and the driver |
| `src/net.ts` | the net object contract |
| `src/udp.ts` | `UdpNet`, Node's UDP sockets |
| `src/tunnel.ts` | `TunnelNet`, sockets borrowed from a bridge, spec/tunnel.md |
| `test.mjs`, `bench/` | the ctest entry `web` and the bench it runs |

npm compiles `src/` into `dist/`. `web_package` packs that with `c/` (the platform files and
the single headers) into `dist/rant-middleware-<version>.tgz`, which a release attaches, so a
project that fetched Rant never needs npm or TypeScript.

## The net object

The host sets `Module.rantNet` before the first node opens. A socket is a small integer the
net object hands out. Addresses are naddrs: the four address bytes read as one little
endian u32, which is how wasm memory holds them.

| Call | Does |
|---|---|
| `open(sink)` | a new socket id, above 0. Its datagrams go to `sink.deliver(id, naddr, port, bytes)` |
| `bind(id, ifNaddr, port, reuse)` | the bound port now, 0 when it cannot be bound now |
| `setMulticastInterface(id, ifNaddr)`, `setMulticastTTL(id, ttl)`, `setMulticastLoopback(id, on)`, `ownGroupsOnly(id)` | the socket options |
| `setReceiveBufferSize(id, bytes)`, `setSendBufferSize(id, bytes)` | optional, the socket buffers a node asks for |
| `membership(id, group, ifNaddr, join)` | joins or leaves a group, true when it will |
| `send(id, bytes, naddr, port)` | bytes sent, or -1. `bytes` views wasm memory and is valid only during the call |
| `close(id)` | |
| `interfaces()` | `[{addr, mask}]`, the interfaces of the host whose sockets these are |
| `hostname()` | a string |
| `loopbackGroupBind(naddr)` | optional: where a loopback pinned discovery socket binds. Loopback, the default, where the host hands it group traffic (Windows), else 0 (POSIX). The host is the net object's, so `UdpNet` asks Node and `TunnelNet` the bridge |

Each module instance passes its own sink, so several instances can share one net object,
an explorer and a second app in one page over one bridge session for example.

A node needs its data port the moment it binds, while a browser or Node.js binds only
asynchronously. So a net object binds sockets to ephemeral ports ahead of time and hands
one out for each bind to port 0. Both net objects bind them to one address chosen at
create, which must be the one the node binds its data socket to: 127.0.0.1 for a node
pinned to loopback, else any. A bind to a fixed port (the discovery port, with reuse) answers that port at once
and binds in the background. Work asked of a socket meanwhile waits for it.

## The driver

A page never blocks. `rant_web_drive(fn, user)` runs fn now, again when a datagram arrives,
and again when the wait the node asked for runs out. fn polls each node with a long
timeout. The platform's poll returns at once with what is queued and hands the wait the
node computed (its next timer, discovery's next announce) to the driver, so a node at
rest wakes only when Rant has work. `rant_web_wake(ms)` adds an app timer. A run with
nothing due costs nothing until the next deadline. A delivery runs the drives in a
microtask, so a datagram is handled with no hop through the event loop.

Three rules follow:

- Every blocking call takes a zero timeout or is avoided, and `match_wait` is negative. A
  wait inside wasm would spin, since no datagram can arrive while wasm runs.
- A send outside a handler waits for the next poll, as on any node polled by hand. An app
  that sends from a page event polls once with a zero timeout, or calls
  `rant_web_wake(0)`.
- Close a node outside its own callbacks. A wrapper that defers a close from inside one to
  another thread (the C++ `Node::close`) cannot in a build without threads.

The driver keeps its schedule in fields of the library object `$RantWeb`. Emscripten writes
that object out as source text, and an `Infinity` there becomes `null`, so "none" is -1.
