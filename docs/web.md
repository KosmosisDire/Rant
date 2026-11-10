# Rant in a page

Rant builds to wasm with Emscripten and runs the real node in a browser or in Node.js. A
TypeScript or JavaScript app uses the binding in the npm package `rant-middleware`
(docs/javascript.md). A C or C++ app links the library itself, as below. Either way the
sockets come from a net object:

- `TunnelNet` in a browser: UDP sockets borrowed from a Rant bridge, over a WebSocket and
  WebRTC.
- `UdpNet` in Node.js: the machine's own UDP sockets.

## Build a C or C++ app

In a CMake project built with Emscripten that fetches Rant, link `rant::rant_web`.

Without CMake, take the release's web package `rant-middleware-<version>.tgz`. Compile
`c/rant.c` and `c/platform.c` with these defines, and link the JavaScript side:

```
-DRANT_PLAT_CUSTOM -DRANT_NO_THREADS -DRANT_NO_SHM -DRANT_NO_PROC_STATS
--js-library c/rant_web.js
```

## Drive the node

A page never blocks, so the page drives the node. Open it polled by hand, and register a
drive that polls it:

```cpp
#include "rant.hpp"
#include "rant_web.h"

static rant::Node* node;

int main() {
    rant::NodeOptions o;
    o.threading = rant::Threading::Manual;
    o.match_wait = std::chrono::milliseconds(-1);
    node = new rant::Node("page", o);
    rant_web_drive([](void*) { node->poll(std::chrono::milliseconds(60000)); }, nullptr);
}
```

- `rant_web_drive(fn, user)`: fn runs now, when a datagram arrives and when the node's next
  timer is due. The long poll returns at once.
- `rant_web_wake(ms)`: the next drive comes no later than ms from now, for a timer of the
  app's own.

## Give it sockets

The binding takes the net object as `openNode`'s `net` option. A C or C++ app gets it as
`rantNet`, before the first node opens:

```js
import { TunnelNet } from "rant-middleware/tunnel";
import createApp from "./app.mjs";     // built with -sMODULARIZE -sEXPORT_ES6

const rantNet = await TunnelNet.connect("ws://192.168.1.20:6731", { passphrase: "..." });
await createApp({ rantNet });
```

In Node.js:

```js
import { UdpNet } from "rant-middleware/udp";
const rantNet = await UdpNet.create();
```

| `TunnelNet.connect(url, opts)` option | Meaning |
|---|---|
| `passphrase` | The bridge's passphrase. |
| `name` | The session's name in the bridge's logs, and the hostname the node sees. |
| `transport` | `"auto"` (default) adds WebRTC when it connects, `"websocket"` never tries. |
| `iceServers`, `rtcTimeoutMs` | Servers beside the bridge's own, and how long WebRTC may take (4000). |
| `probeIntervalMs` | How often the bridge checks the link (250). |
| `spares` | Sockets bound ahead, one per node opened at once (2). |
| `address` | Where they bind on the bridge's host: `"127.0.0.1"` for nodes pinned to loopback, else any (default). |
| `onEvent` | `(e) => ...` for `resumed`, `restored`, warnings and errors. |

`connection` holds the state, the carrier (`"webrtc"` or `"websocket"`) and the round trip.
`onConnectionChange(h)` watches it, `stats` counts what crossed, and `shutdown()` ends the
session. A dropped connection reconnects on its own and keeps every socket.

`findBridges(url?)` from `rant-middleware/tunnel` lists the bridges on the network as
`[{ name, url, version, self }]`. Every bridge answers the name `rant-bridges.local` over
mDNS and lists the others it hears, so the default `ws://rant-bridges.local:6731` reaches
one, with no passphrase.

| `UdpNet.create(opts)` option | Meaning |
|---|---|
| `spares` | Sockets bound ahead, one per node opened at once (2). |
| `address` | Where they bind: `"127.0.0.1"` for nodes pinned to loopback, else any (default). |

One net object can serve several modules in one page. In Node.js its sockets keep the
process alive, so shut it down once its nodes closed. `openNode` does that for the `UdpNet`
it makes when given none.

## Rules for a C or C++ app

The binding keeps these itself.

- Never block: every timeout is zero and `match_wait` is negative.
- A send from a page event waits for the next drive. Poll once with a zero timeout after
  it, or call `rant_web_wake(0)`.
- Close a node outside its own callbacks.
- Behind a bridge the node is a node of the bridge's host, its loopback included.
