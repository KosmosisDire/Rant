# DART WebSocket bridge protocol (v1)

The bridge turns a WebSocket connection into a full DART node on the mesh. One
connection = one node: the bridge opens the node when asked, owns its sockets and
poll loop on a dedicated thread, and closes it (with a BYE) when the connection
drops. Everything a native node can do arrives over one socket, so a browser,
a phone, or any language with a WebSocket client is a first-class peer.

Two planes, split by WebSocket frame type:

- **Text frames = control plane, JSON.** Open the node, create channels, flip
  roles, query peers. Requests carry a `seq`; every request gets exactly one
  reply echoing it. The server also pushes unsolicited `event` messages.
- **Binary frames = data plane.** Publish and delivery, with a fixed little-endian
  header of a few bytes and the raw payload after it. No JSON, no base64, no
  per-message metadata: the hot path costs a memcpy.

All integers in binary frames are little-endian (matching the DART wire, and
`DataView`'s `true` flag in JS). The protocol version is returned by `open`;
there is no cross-version compatibility (see the repo rule: robust parsing, no
back-compat).

## Connection lifecycle

1. Client connects: `ws://host:7480/`.
2. Client sends `open` (must be the first message). The bridge creates the node.
3. Client creates channels, publishes, receives, at will.
4. Client closes the socket (or errors out): the bridge closes the node with a
   BYE and frees everything. There is no explicit close op.

Any request before `open` (or a second `open`) gets an error reply. A binary
frame before `open`, or on an unknown channel, is dropped and reported with a
`send_error` event (never a reply: publishes carry no seq).

## Control plane (text frames)

Every request is a JSON object with `op` and a client-chosen `seq` (any number;
echo-correlated, so a client can key a promise map on it). Every reply is:

```json
{ "op": "reply", "seq": 3, "ok": true,  ...result fields }
{ "op": "reply", "seq": 3, "ok": false, "error": "channel name too long" }
```

Unknown `op`, malformed JSON, or a missing field gets `ok:false` (with `seq: 0`
when the request's seq was unreadable). The connection is never killed for a bad
request, only for a broken WebSocket.

### `open` : create the node

```json
{ "op": "open", "seq": 1,
  "name": "dashboard",            // optional; omitted => auto "node-XXXXXXXX"
  "domain": 0,                    // optional; every field below is optional too
  "max_channels": 8,
  "max_peers": 16,
  "interface": "192.168.1.10",    // pin discovery multicast (multihomed hosts)
  "seed_peers": ["10.0.0.7", "10.0.0.8:7400"],  // unicast discovery seeds
  "fragment_size": 0,             // UDP payload bytes per fragment; 0 = default
  "announce_interval_ms": 1000,
  "peer_timeout_ms": 3500,
  "disable_shm": false,           // force on-wire UDP even to same-host peers
  "memory": 0 }                   // heap cap in bytes; 0 = default runaway guard
```

Reply: `{ "ok": true, "proto": 1, "name": "dashboard" }` (the actual node name,
so an auto-generated one is visible).

### `channel` : create a channel (topic)

```json
{ "op": "channel", "seq": 2,
  "name": "pose",                 // required: the cross-peer topic identity
  "role": "pubsub",               // "pubsub" | "pub" | "sub" | "inactive"
  "schema": "Pose { stamp: u64, x: f64, y: f64, vel: { dx: f32, dy: f32 } }",
                                  // optional DSL text; omitted = raw-bytes channel
  "adopt": false,                 // no schema given: adopt the one a live peer
                                  // advertises for this topic (debug tools joining
                                  // typed topics they never declared); falls back to
                                  // raw if none is advertised ("adopted" in the reply)
  "reliable": false,              // QoS, all optional
  "keep_last": 16,
  "catch_up": 0,
  "max_message_bytes": 0,
  "backpressure_wait_ms": 0 }
```

Reply for a typed channel returns the compiled layout, which is everything a
client needs to encode and decode messages by itself (fields are at fixed
absolute offsets, scalars little-endian, no padding):

```json
{ "ok": true, "id": 0, "size": 32, "hash": "9f3a5c1e22b40d77",
  "fields": [
    { "path": "stamp",  "kind": "u64",    "offset": 0,  "size": 8 },
    { "path": "x",      "kind": "f64",    "offset": 8,  "size": 8 },
    { "path": "y",      "kind": "f64",    "offset": 16, "size": 8 },
    { "path": "vel",    "kind": "struct", "offset": 24, "size": 8 },
    { "path": "vel.dx", "kind": "f32",    "offset": 24, "size": 4 },
    { "path": "vel.dy", "kind": "f32",    "offset": 28, "size": 4 } ] }
```

`id` is the channel id used in binary frames (the node-local index, dense from
0). Array fields carry `"kind": "arr"` plus `"elem"` and `"count"`; `struct`
rows are group headers whose members follow with dotted paths (a decoder can
skip them). `hash` is the 64-bit schema identity as hex (it exceeds JS safe
integers). A raw channel replies just `{ "ok": true, "id": 0 }`.

A schema error replies `ok:false` with the offending position in `error`. The
schema DSL text must be byte-identical across nodes for the identical-hash fast
path, and structurally a subset to match a wider publisher (the normal DART
rules; the bridge adds nothing).

### `role` : flip a channel's role at runtime

```json
{ "op": "role", "seq": 3, "channel": 0, "role": "sub" }
```

Re-advertises immediately; peers rematch. `"inactive"` = declared but off.

### `peers` : snapshot the live peer table

```json
{ "op": "peers", "seq": 4 }
```

Reply: one row per peer with its advertised interest list (its topics) decoded
from the announce overlay, including the schema field table for publish entries
that inline one, so a debug client can decode any typed topic on the network:

```json
{ "ok": true, "peers": [
  { "id": 2, "name": "robot1", "addr": "192.168.1.7:52011", "active": true,
    "frag": 1024,
    "topics": [
      { "name": "pose", "role": "pub", "reliable": true,
        "hash": "9f3a5c1e22b40d77", "size": 32, "fields": [ ... ] },
      { "name": "cmd",  "role": "sub", "reliable": false } ] } ] }
```

A pubsub topic appears as two rows (a `pub` and a `sub` entry). `hash`/`size`/
`fields` appear only on publish entries whose schema was advertised inline
(`fields` matches the `channel` reply format). `active:false` is a DROPPED
(dormant, resumable) peer. Peer changes also push as events (`peer_interest`
fires when a peer's list is applied), so polling this is only for late-joining
UIs.

### `drain` : wait until every reader acked a channel

```json
{ "op": "drain", "seq": 5, "channel": 0, "timeout_ms": 1000 }
```

Reply: `{ "ok": true, "drained": true }`. Call before closing when the last
burst matters.

### `stats` : node counters

```json
{ "op": "stats", "seq": 6 }
```

Reply: `{ "ok": true, "mem_in_use": 73216, "mem_peak": 73216, "alloc_calls": 12,
"backpressure_waited_us": 0, "backpressure_waited_sends": 0 }`.

## Events (server pushed, text frames)

```json
{ "op": "event", "event": "peer_up", "text": "peer 2 up ...", ...fields }
```

`text` is the human-readable one-liner (`dart_event_str`); the structured fields
are per event:

| event              | fields                                          |
|--------------------|-------------------------------------------------|
| `peer_up`          | `peer`, `name`, `addr`                          |
| `peer_down`        | `peer`, `name`                                  |
| `peer_interest`    | `peer`, `publishes`, `receives` (matched counts)|
| `msg_lost`         | `channel`, `peer`, `first`, `count`             |
| `error`            | `error` (numeric code) + whichever of `channel_name`, `channel`, `peer`, `os_error`, `addr`, `bytes`, `first`/`count`, `identity` apply |
| `send_error`       | `channel`, `code`, `error` (a failed publish)   |

Everything that goes wrong is one `error` event: `text` carries the human-readable
message and `error` the numeric code (a `DartErrorKind`: 1 name-collision,
2 qos-incompatible, 3 schema-mismatch, 4 interest-overflow, 5/6 meta-truncated
interest/schema, 7 peer-meta-too-big, 8 msg-too-big, 9 peer-refused, 10 evicted-unsent,
11 oom, 12 platform, 13 socket, 14 bind, 15 mcast-join, 16 send, 17 recv, 18 poll,
19 waker). A client that only prints `text` needs no per-code handling.

`peer_up` fires again on a dormant peer's resume; a client keying names on
`peer` ids stays correct across blips for free.

## Data plane (binary frames)

One WebSocket binary message = one DART message. WebSocket does the framing, so
there are no length fields.

**Publish, client to server** (3-byte header):

```
[u8 op = 0x01] [u16 channel] [payload bytes ...]
```

**Delivery, server to client** (7-byte header):

```
[u8 op = 0x01] [u16 channel] [u32 sender] [payload bytes ...]
```

`channel` is the id from the `channel` reply. `sender` is the peer id; resolve
it to a name from `peer_up` events (or the `peers` snapshot). The payload is the
DART message verbatim: for a typed channel it decodes with the `fields` table;
for a raw channel it is whatever the publisher sent. On a typed channel the
bridge's node has already length-validated the payload against the sender's
schema before forwarding.

Other `op` byte values are reserved and ignored (a client must not fail on an
unknown op; the server drops unknown ops silently).

Publishing is fire and forget: no ack (a reliable channel's guarantees run
between the bridge node and its peers, as usual). A publish that fails
immediately (bad role, too big, out of memory) surfaces as a `send_error`
event. A reliable channel under backpressure blocks the connection's receive
thread, which backpressures the WebSocket via TCP: a fast publisher into a slow
mesh slows down instead of buffering unboundedly.

Delivery on a slow client: frames queue in the socket's send buffer. The
subscriber side of the bridge node is a normal DART reader (KEEP_LAST etc.); if
the WebSocket client cannot drain its TCP connection the bridge drops the
connection rather than buffer forever (`--max-buffered` bytes, default 8 MiB).

## What a JS/TS client looks like

The reference client (`bridge/client/dart.mjs`: one JSDoc-typed ES module, no
dependencies, no build step, browser + Node + Deno + Bun) wraps this in a
promise API and uses the `fields` table to give typed access without codegen:

```ts
import { DartClient } from "./dart.mjs";

const node = await DartClient.connect("ws://localhost:7480", {
  name: "dashboard", domain: 0 });

const pose = await node.channel("pose", "pubsub", {
  reliable: true,
  schema: "Pose { stamp: u64, x: f64, y: f64, vel: { dx: f32, dy: f32 } }" });

pose.onMessage = (msg) => {
  console.log(msg.sender, msg.get("x"), msg.get("vel.dx"));  // typed reads
  // msg.data is the raw Uint8Array view when you want the bytes
};

pose.send({ stamp: BigInt(Date.now()) * 1000n, x: 1.5, y: 2.0 });  // typed encode
pose.sendRaw(new Uint8Array(32));                                  // or raw bytes

node.onEvent = (e) => { if (e.event === "peer_up") console.log("+", e.name); };
node.onPeers = (peers) => render(peers);   // maintained from events for you

node.close();
```

`get`/`send` read and write straight through a `DataView` at the offsets the
server reported: no per-message parsing, no allocation beyond the message
buffer. u64/i64 fields surface as `bigint`; everything else as `number`.

## Non-goals in v1

- **No auth, no TLS.** The bridge binds 127.0.0.1 by default; exposing it
  (`--bind 0.0.0.0`) puts full mesh access on that port. Put a reverse proxy in
  front for wss:// or auth.
- **No JSON data path.** The data plane is bytes; a client that wants JSON
  converts at the edge with the fields table (the reference client shows how).
- **No per-sender layout tables.** A typed delivery is validated against the
  sender's schema by the node, but the client decodes with its own channel
  layout. A subscriber whose schema is a strict subset of a publisher's sees
  correct fields only when the leading layout matches; byte-identical schema
  text end to end (the recommended deployment) is always exact. A per-sender
  `fields` push is a v2 item if it earns its complexity.
