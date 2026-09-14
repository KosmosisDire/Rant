# JavaScript

`dist/rant.mjs` is the bridge client: one `RantNode` is one full node on the mesh, spoken
through `rant_bridge` over a WebSocket. It has no runtime dependencies and runs in
browsers, Node 22 and later, Deno and Bun off the global WebSocket. `dist/rant.d.ts`
carries the types and `dist/rant.js` is the classic script twin exposing one global,
`RantNode`. The wire is bridge/PROTOCOL.md and the design is spec/bridge.md. The
semantics are the C ones, so docs/topics.md, docs/patterns.md and docs/tasks.md apply.

## A node

```js
const node = await RantNode.connect("ws://localhost:7480", { name: "dashboard" });
const pub  = await node.publisher("pose", "Pose { x: f64, y: f64 }");
pub.send({ x: 1.5, y: 2.0 });
const add  = await node.remoteFunction("add", "A { a: i32, b: i32 }", "R { sum: i32 }");
const r    = await add.call({ a: 2, b: 3 });
```

The WebSocket opens first and carries the JSON control plane. Data rides binary frames
over a WebRTC data channel per entity when WebRTC negotiates, and over the WebSocket
otherwise. The API is the same either way and `node.transport` says which carrier won.
`transport: "websocket"` skips WebRTC, `rtcTimeoutMs` bounds the attempt, and
`iceServers` adds servers beside the bridge's own. The other connect options mirror the
C node options with snake case names, plus `onEvent` for the event stream.

Every handle comes from an async factory on the node: `topic(name, role, opts)`,
`publisher`, `subscriber`, `functionDefinition`, `remoteFunction`, `taskDefinition`,
`remoteTask`, `variableDefinition`, `remoteVariable` and `video`. Schemas are DSL text
passed positionally, and null means raw bytes. The client owns encoding: a create reply
carries the field table, and `Layout` encodes and decodes plain nested objects against
it. A missing fixed field encodes as zero and a missing variable field as empty. A bare
type schema takes the value itself. `settle()`, `close()`, `peers()`, `entities()`,
`peerEntities(peer, includeDropped)`, `mesh()`, `meshFind(kind, name)`, `meta(peer,
sections)`, `log(level, text)` and `onLog(handler, levels)` mirror the C node.

## Messages

A subscriber handler receives the decoded value and a `RantMessage`. `get(path)` reads
one field by dotted path, with bigint for 64 bit integers, a string for string fields, a
plain object for a map and arrays for array fields, where a u8 array is a `Uint8Array`
view. `value` decodes the whole message once. `writtenUs` is the publisher's source stamp,
0 when it opted out.

## Functions, tasks and variables

A function handler's return value, possibly a promise, is the reply, and a throw answers
"app_error". `call(value, timeoutMs)` resolves with a `Response` whose `status` is a
string and never rejects on a status, only on connection loss. `timeoutMs` adds a client
side bound that resolves "timeout". `message` is the outcome text to display on a
failure.

A task handler receives the request and a context: `ctx.progress(value)` streams updates
and `ctx.signal` aborts when the caller or a third party requests cancellation. Honor a
cancel by throwing the signal's reason, which answers "cancelled". Any other throw answers
"app_error" and a normal return completes "ok" even after an abort. `RemoteTask.call()`
returns a `TaskRun` synchronously: `result` is the one terminal response, `onProgress`
observes updates with null for the RUNNING ack and replays any that arrived before
registration, and `cancel()` resolves with "ok", "no_cancel", "not_pending" or "error".
There is no client side timer, since after RUNNING a task runs as long as it runs.

A variable handle reads with `get()`, writes with `set`, `force` and `unforce`, and waits
with `wait(timeoutMs)`. `onChange` fires per pushed change and once at registration when
a value is cached. `onWrite` needs `onWrite: true` at create so the bridge pushes every
write. A remote set round trips through the owner and the cache is authoritative.

## Video

A `VideoView` is a picture as a `MediaStream`: set `videoEl.srcObject = view.stream`.
`push(value)` shows any VideoFrame, Image or ExternalVideoStream value from anywhere, using
WebCodecs for H264, H265 and AV1, the image decoder for MJPEG, JPEG and PNG, and a pixel
converter for raw formats. An ExternalVideoStream makes the view follow its URL: WHEP as
a WebRTC session, MJPEG through an image, HLS through a media element. `attach(source,
path)` binds the view to the VideoFrame or Image field of an entity's stream, and over
WebRTC asks the bridge to carry an encoded VideoFrame field on a real video track the
browser decodes, with the frames then arriving with the pixels emptied. `node.video(name)`
subscribes a VideoFrame topic best effort and attaches a view in one call. The stream
carries exactly one of the track and the canvas at a time.
