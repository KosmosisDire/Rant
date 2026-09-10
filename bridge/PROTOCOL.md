# DART bridge protocol (v11)

The bridge turns a WebSocket connection into a full DART node on the mesh. One
connection = one node: the bridge opens the node when asked, owns its sockets and
service thread, and closes it (with a BYE) when the connection drops. Everything a
native node needs, **pub/sub** plus the pattern entities (**functions**, **tasks**,
**variables**), arrives over that one connection, so a browser, a phone, or any
language with a WebSocket client is a first-class peer.

Two planes:

- **Control plane: JSON over the WebSocket.** Open the node, create entities, flip
  roles, drain, settle, introspect, and negotiate WebRTC. Requests carry a `seq`;
  every request gets exactly one reply echoing it. The server also pushes
  unsolicited `event`, `match`, `log` and `rtc` messages.
- **Data plane: binary FRAMES.** One header for every kind of traffic in both
  directions (publish/delivery, variable writes/updates, calls/requests/replies,
  progress, cancel), a few bytes plus the raw payload. No JSON, no base64: the hot
  path costs a memcpy. A frame travels over a **WebRTC data channel** when one is
  open for its entity, and as a **binary WebSocket message** otherwise. The two
  carriers are interchangeable per frame: a client with no WebRTC at all sends and
  receives exactly the same bytes over the socket.

The WebSocket is always opened first and always carries the control plane; WebRTC
is negotiated over it and carries data only. A `VideoFrame` field anywhere in a
streamed entity (a topic, a variable, a task's progress) can additionally arrive as a
real **WebRTC video track** (see Media), decoded by the browser instead of crossing the
data plane as pixels.

All integers in frames are little-endian (matching the DART wire, and `DataView`'s
`true` flag in JS). The protocol version is returned by `open`; there is no
cross-version compatibility (see the repo rule: robust parsing, no back-compat).

## Connection lifecycle

1. Client connects: `ws://host:7480/`.
2. Client sends `open` (must be the first message). The bridge creates the node.
3. Client optionally negotiates WebRTC (`rtc`); the reference client always tries.
4. Client creates entities, publishes, calls, receives, at will.
5. Client closes the socket (or errors out): the bridge answers any requests still
   parked at the client (function requests fail, task requests complete
   `cancelled`, so remote callers get an answer now, not a timeout), closes the
   WebRTC connection, then closes the node with a BYE and frees everything. There
   is no explicit close op.

Any request before `open` (or a second `open`) gets an error reply. A frame before
`open`, or on an unknown id, is dropped and reported with a `send_error` event
(never a reply: frames carry no seq).

## Control plane (JSON)

Every request is a JSON object with `op` and a client-chosen `seq` (any number;
echo-correlated, so a client can key a promise map on it). Every reply is:

```json
{ "op": "reply", "seq": 3, "ok": true,  ...result fields }
{ "op": "reply", "seq": 3, "ok": false, "error": "topic name too long" }
```

Unknown `op`, malformed JSON, or a missing field gets `ok:false` (with `seq: 0`
when the request's seq was unreadable). The connection is never killed for a bad
request, only for a broken WebSocket.

### `open` : create the node

```json
{ "op": "open", "seq": 1,
  "name": "dashboard",            // optional; omitted => auto "ws-XXXXXXXX"
  "domain": 0,                    // optional; every field below is optional too
  "max_topics": 8,
  "max_peers": 16,
  "interface": "192.168.1.10",    // pin discovery to ONE interface; omit = all of them
  "seed_peers": ["10.0.0.7", "10.0.0.8:7400"],  // unicast discovery seeds
  "unicast_only": false,          // no multicast: seeds + relaying (see docs/discovery.md)
  "self_ip": "", "advertise_port": 0,   // state our locator outright
  "fragment_size": 0,             // UDP payload bytes per fragment; 0 = default
  "announce_interval_ms": 1000,
  "peer_timeout_ms": 3500,
  "match_wait_ms": 0,             // send-path match wait; 0 = default 1s, negative = off
  "disable_shm": false,           // force on-wire UDP even to same-host peers
  "disable_logs": false,          // strip the built-in @dart/log topics
  "disable_meta": false,          // do not host the @dart/meta endpoint
  "disable_error_logs": false,    // suppress default mirroring onto @dart/log/error
  "fetch_details": false }        // greedily fetch peer details so reflected entity
                                  //   names resolve even for topics this node doesn't share
```

Reply: `{ "ok": true, "proto": 11, "name": "dashboard", "webrtc": true }` (the actual
node name, so an auto-generated one is visible; `webrtc` = this bridge can
negotiate a WebRTC data path).

### `create` : the one constructor

Every entity, a topic or a pattern handle, is created by one op. The **`id` is
client-chosen** (1..65534, unique per connection, like a request `seq`): it names
the entity in every frame, and it is also the WebRTC data channel id, so both ends
open the channel without a round trip (the client opens its side before it even
sends the create, and nothing the bridge sends right after the create can miss it).
`kind` picks the entity:

| kind                  | schema keys           | extra fields |
|-----------------------|-----------------------|--------------|
| `topic`               | `schema`              | `role`, QoS (below) |
| `function_definition` | `req`, `rsp`          | `timeout_ms`, `backpressure_wait_ms`, `keep_last` |
| `remote_function`     | `req`, `rsp`          | same |
| `task_definition`     | `req`, `prg`, `rsp`   | function fields + `progress_best_effort`, `progress_keep_last`, `no_cancel`, `exclusive`, `multi` |
| `remote_task`         | `req`, `prg`, `rsp`   | same (only the timeouts and progress fields apply) |
| `variable_definition` | `schema`              | `read_only`, `allow_force`, `on_write`, `catch_up`, `keep_last`, `backpressure_wait_ms` |
| `remote_variable`     | `schema`              | `on_write` |

Every schema key is optional DSL text; omitted = raw bytes. **Each schema key you
pass comes back in the reply as a field-table block under the same key**, and the
reply always carries `reliable` (the frame path's reliability: the topic's QoS,
`true` for every pattern entity). Every kind also takes **`reflect: true`**: with no
schema, the entity takes its types (and, for a topic, its reliability) from the
mesh, a reader its provider's, a writer the widest every reader accepts; the reply
then carries the adopted blocks and `reflected: true`, or `reflected: false` when
nobody advertises the name yet (untyped until `refresh`, below).

```json
{ "op": "create", "seq": 2, "id": 1, "kind": "topic",
  "name": "pose",                 // required: the cross-peer identity
  "role": "pubsub",               // "pubsub" | "pub" | "sub" | "inactive"
  "schema": "Pose { stamp: u64, x: f64, y: f64, vel: { dx: f32, dy: f32 } }",
  "reliable": false,              // QoS, all optional
  "keep_last": 16, "catch_up": 0, "max_message_bytes": 0,
  "heartbeat_ms": 0, "repair_delay_ms": 0, "backpressure_wait_ms": 0,
  "shm_max_bytes": 0,
  "max_rate_hz": 0,               // subscriber, best-effort: cap delivery from each publisher
  "reflect": false }              // no schema: take the mesh's for this name
```

Reply: `{ "ok": true, "id": 1, "reliable": false, "schema": { "name": "Pose", "size": 32,
"hash": "9f3a5c1e22b40d77", "fields": [...] } }` (a raw topic replies just `id` and
`reliable`).

```json
{ "op": "create", "seq": 7, "id": 2, "kind": "function_definition", "name": "add",
  "req": "AddReq { a: i32, b: i32 }", "rsp": "AddRsp { sum: i32 }",
  "timeout_ms": 0, "backpressure_wait_ms": 0,
  "keep_last": 0 }               // req + rsp ring depth; 0 = the reliable default (10).
                                 //   An inline reply cannot wait for a TX pass, so this must
                                 //   cover the biggest batch one poll pass drains.
```

Reply: `{ "ok": true, "id": 2, "reliable": true, "req": {...}, "rsp": {...} }`. The
bridge registers a handler that **defers every request to the client** (see the
CALL frame): there is no bridge-side handler logic, the client is the implementation.
A `remote_function` is the same shape without a handler; its `timeout_ms` is the
call timeout (0 = 5s): the answer the wire never delivers becomes status `timeout`.

```json
{ "op": "create", "seq": 10, "id": 3, "kind": "task_definition", "name": "transfer",
  "req": "Xfer { total: u32 }", "prg": "Prog { done: u32 }", "rsp": "Sum { bytes: u32 }",
  "progress_best_effort": false,   // progress-channel reliability: false = reliable
  "progress_keep_last": 0,         // progress ring depth; 0 = the pattern default
  "no_cancel": false,              // definition: will not honor cancellation; a remote's
                                   //   cancel is then refused locally ("no_cancel")
  "exclusive": false,              // definition: declared serialization (the handler enforces it)
  "multi": false,                  // definition: redundant providers intended
  "timeout_ms": 0,                 // remote: until-first-response bound; 0 = 5s
  "backpressure_wait_ms": 0, "keep_last": 0 }
```

A task is a function with progress and cancellation: same call ids and statuses,
plus a broadcast progress channel and a cancel op. Parking a request implies
**RUNNING** to the caller (its call timeout is dropped: a task runs as long as it
runs); the client streams PROGRESS frames and finishes with a RESULT frame whose
status may be 5 (`cancelled`).

```json
{ "op": "create", "seq": 8, "id": 4, "kind": "variable_definition", "name": "config",
  "schema": "Config { rate_hz: u32 }",
  "read_only": false,                    // no set channel: remote sets get refused
  "allow_force": false,                  // permit force (local + remote)
  "on_write": false,                     // also push EVERY applied write (not just changes)
  "catch_up": 0,                         // how much a late remote replays
  "keep_last": 0,                        // both channels' repair window; 0 = reliable default (10)
  "backpressure_wait_ms": 0 }
{ "op": "create", "seq": 9, "id": 5, "kind": "remote_variable", "name": "config",
  "schema": "Config { rate_hz: u32 }", "on_write": false }
```

Reply: `{ "ok": true, "id": 4, "reliable": true, "schema": {...} }`. There is no
`initial`: a client that wants one sends a VAR frame right after the create (the
value channel latches the latest for late joiners), which is what the reference
client does. With `on_write`, the bridge also pushes a VAR frame on every applied
write (flag bit1 set), so a client can observe writes that leave the value
unchanged. Both sides may set it.

### The field table

A typed create replies with the compiled layout, which is everything a client needs
to encode and decode messages by itself (fixed fields at absolute offsets,
little-endian, no padding):

```json
{ "name": "Pose", "size": 32, "hash": "9f3a5c1e22b40d77",
  "fields": [
    { "path": "stamp",  "kind": "u64",    "offset": 0,  "size": 8 },
    { "path": "x",      "kind": "f64",    "offset": 8,  "size": 8 },
    { "path": "y",      "kind": "f64",    "offset": 16, "size": 8 },
    { "path": "vel",    "kind": "struct", "offset": 24, "size": 8 },
    { "path": "vel.dx", "kind": "f32",    "offset": 24, "size": 4 },
    { "path": "vel.dy", "kind": "f32",    "offset": 28, "size": 4 } ] }
```

`name` is the root type's name ("" when anonymous). `hash` is the 64-bit schema
identity as hex (it exceeds JS safe integers). **`size`** is the length of the
**fixed section**: the exact message size when the schema has no variable fields,
and where the variable tail begins when it does.

A **BARE-TYPE schema** (the whole schema is one type, e.g. `bool` or `f32[]`, not a
struct) replies with exactly ONE field whose `path` is the **empty string** (an
anonymous root: a bare type's identity is its shape, so the same one from any
language is the same bytes and the same `hash`). That one field is read and written
like any other; the reference client lets the message BE the value
(`topic.send(true)`, `msg.value() === true`).

**Field kinds** (`kind`):

| kind      | shape | extra fields |
|-----------|-------|--------------|
| `u8`..`u64`, `i8`..`i64`, `f32`, `f64`, `bool` | fixed scalar at `offset` | none |
| `arr`     | fixed array of `count` elements at `offset` | `elem`, `count` (+ `cap` if `elem` is `string`) |
| `struct`  | group header; members follow with dotted paths (a decoder can skip it) | none |
| `string`  | capped string, fixed slot `[u16 len][cap bytes]` at `offset` (`size` = 2+cap) | `cap` |
| `enum`    | named integer; on the wire just its `backing` scalar at `offset` | `backing`, `variants` (`[{name,value}]`) |
| `vstring` | variable string; lives in the tail (`offset`/`size` = 0) | none |
| `varr`    | variable array; lives in the tail, live element count | `elem` (+ `cap` if `elem` is `string`) |
| `map`     | self-describing tagged value tree; lives in the tail | none |

A field's type may also carry a NAME (`named`), which the schema wire carries and no
message byte does. It NARROWS matching: a `Transform` field never binds to a same-shaped
`Twist` one, while a reader declaring the bare shape reads either. The `kind` a row
reports is always what the name WRAPS (a `Transform` reads as `struct`), so a decoder that
ignores `named` is still correct; see docs/stdtypes.md for the standard names.

| extra field | on | meaning |
|-------------|----|---------|
| `named`      | any row | the field type's name (`"Transform"`, `"Uuid"`, `"Timestamp"`) |
| `elem_named` | `arr` / `varr` | the ELEMENT type's name (`"Float3"` for `Float3[4]`) |
| `elem_size`  | `arr` / `varr` | bytes of one element |
| `elem_struct`| `arr` / `varr` | the element is a struct: its element-0 template rows follow |
| `in_array`   | any row | this row is a member of that array row's element template |

An array whose element is a STRUCT contributes ONE set of member rows, the element-0
template: each carries `in_array` naming its array row, and element *i* of a member
sits at `offset + i * elem_size`. An **`enum`** is a fixed field carrying its
`backing` integer; `variants` names the values (an unknown value has no name). The
**variable kinds** have no fixed offset: each is one `[u32 len][payload]` frame in
the message tail, in depth-first declaration order, starting at `size`. The `map`
payload is a JSON-style tagged tree; the reference client has the codec.

A schema error replies `ok:false` with the reason. The DSL text must be
byte-identical across nodes for the identical-hash fast path, and structurally a
subset to match a wider publisher (the normal DART rules; the bridge adds nothing).
The standard media types are always in scope by name: a schema of just `VideoFrame`
or `Image` is the canonical type.

### `role` / `drain` / `settle` / `cancel` / `refresh`

```json
{ "op": "role",    "seq": 3, "id": 1, "role": "sub" }
{ "op": "drain",   "seq": 5, "id": 1, "timeout_ms": 1000 }
{ "op": "settle",  "seq": 6, "timeout_ms": 5000 }         // negative = 3 announce intervals
{ "op": "cancel",  "seq": 11, "call": 7 }
{ "op": "refresh", "seq": 12, "id": 1 }
```

`role` re-advertises immediately; peers rematch (`"inactive"` = declared but off).
`drain` replies `{ "drained": true }` when every reader acked; call before closing
when the last burst matters (reliable topics). `settle` replies `{ "settled": true }`
when everything created so far is matched against everyone currently on the
network; it runs on the connection's receive thread, so further requests from this
client wait for it. `cancel` requests cancellation of an outstanding task call by
the client's call id and replies `{ "status": "ok" | "no_cancel" | "not_pending" |
"error" }`: the C verdict, a value and never a fault (`ok` = cancel requested; the
terminal outcome answers whether it was honored; `no_cancel` = the provider declared
`no_cancel`, refused locally). There is no cancel acknowledgment on the wire: the
terminal status is the answer. `refresh` re-types a `reflect` entity in place when
the mesh moved and replies `{ "retyped": true|false, "reflected": ..., ...schema blocks }`
(the current tables either way; outstanding calls on a re-typed remote are answered
`cancelled` first).

### `log` / `log_subscribe` : the built-in @dart/log topics

Every node hosts three shared reliable log topics (`@dart/log/{error,warn,info}`,
rosout-style), unless opened with `disable_logs`.

```json
{ "op": "log", "seq": 11, "level": "error", "text": "gripper stalled" }
{ "op": "log_subscribe", "seq": 12, "levels": ["error", "warn"] }   // omit levels = all three
```

`log` publishes a line (truncated at 512 bytes on the wire). `log_subscribe` starts
pushing every OTHER node's lines at the requested levels as `log` pushes; idempotent
per level; late-join history replays on match.

### Introspection (query-based, pull-only)

`peers`, `entities`, `peer_entities`, `mesh` and `mesh_find` are local reads of this
node's reflection tables; `meta` is an async directed call to a peer's `@dart/meta`
endpoint. None of them push.

```json
{ "op": "peers", "seq": 20 }
{ "op": "entities", "seq": 21 }
{ "op": "peer_entities", "seq": 22, "peer": 2, "include_dropped": false }
{ "op": "mesh", "seq": 24 }
{ "op": "mesh_find", "seq": 25, "kind": "variable", "name": "config" }
{ "op": "meta", "seq": 23, "peer": 2, "sections": 0 }
```

`peers` replies `{ "peers": [ { "id": 2, "name": "gripper", "address": "192.168.1.9:47001",
"active": true, "fragment_size": 1350, "rtt_us": 1240, "rtt_jitter_us": 180, "rtt_min_us": 910,
"rtt_samples": 57 } ] }` (`active` is false for a dormant peer; the `rtt_*` fields are the
bridge node's own round-trip measurement to that peer, microseconds, `rtt_samples` 0 = none yet).
`entities` / `peer_entities` reply `{ "entities": [...] }` (the latter echoes `peer`;
a dropped peer answers `[]` unless `include_dropped`). **`mesh`** replies the whole mesh
folded, one entity per (kind, name) across every active peer and the bridge's own node,
plus `epoch` (moves on every reflected change: re-read iff it moved); **`mesh_find`**
replies `{ "entity": {...} | null, "epoch" }`. Each entity carries `kind` (`topic` |
`function` | `task` | `variable`), `name` (a `"0x????????"` placeholder until details
arrive: open with `fetch_details`), `provides` / `consumes` (someone live is on that
side), `providers` / `consumers` (how many; 0 or 1 from a per-node walk), `provider`
(the ranked provider's peer id, 0 = the bridge's node), `from` (the node the schemas
were read from), `generation` (hex; changes iff the provider, a schema or an attr
changed), `reliable`, the pattern attrs (`writable` / `forceable` on variables,
`cancellable` / `exclusive` / `multi` on tasks), `incomplete` for a surfaced half pair,
`conflict` when live endpoints declare schemas that cannot read each other, the schema
hashes as hex, and when known the full field tables under `schema`, `rsp` and `prg`. `meta` replies
when the call completes, echoing `seq`; it never fails on status: `{ "valid": true,
"status": 0, "provider": 2, "info": { "node": {...}, "proc": {...}, "topics": [...],
"peers": [...] } }` (`sections` is an OR of 1 node, 2 proc, 4 topics, 8 peers; 0 = all).

### `rtc` : WebRTC negotiation

The **client offers, the bridge answers**, first and on every re-offer (there is one
signaling flow and no glare; it also makes the bridge the DTLS client, which is what
a DTLS stack that cannot reassemble a browser's fragmented ClientHello needs).
Candidates trickle both ways. One op, three request forms:

```json
{ "op": "rtc", "seq": 30 }                                    // start
{ "op": "rtc", "seq": 31, "sdp": "v=0...",                    // an offer, with what each
  "tracks": { "1": { "id": 7, "path": "frame", "call": 0, "keep_data": false } } }  // video line is for
{ "op": "rtc", "seq": 32, "candidate": "candidate:...", "mid": "0" }
```

The bare form creates the bridge's peer connection and replies `{ "ice_servers":
["stun:...", "turn:user:pass@host:port"] }`, the bridge's own `--ice` list, so one
flag configures both ends (the client builds its RTCPeerConnection from it). The
client then creates a **negotiated data channel with id 0** (the anchor that puts
the SCTP line in the offer; entity ids start at 1), offers, and sends the offer;
the reply carries `{ "sdp": "<answer>", "tracks": { "1": "ok" } }`. `tracks` maps the
mid of every recvonly video line in the offer to what it carries: the entity `id`,
the dotted `path` of the `VideoFrame` inside that entity's stream ("" = the value
itself), the client `call` id when the stream is one task call's progress (0
otherwise), and `keep_data` (see Media); the reply's `tracks` carries the verdict per
mid, `"ok"` or the reason the line was refused (not a VideoFrame at that path, an
untyped entity, no such entity). A candidate request replies `{}`; the bridge's
candidates arrive as pushes:

```json
{ "op": "rtc", "candidate": "candidate:...", "mid": "0" }
```

Every WebRTC datagram is sent with "don't fragment" set and sized to the bridge's `--mtu`
(default 1200 bytes on the wire, RFC 8261's safe value; lower it on a tunnel path whose
bridge log says `datagram is too large`). Connection state changes are reported as `rtc` events (`state`: `connecting`,
`connected`, `disconnected`, `failed`, `closed`). Once connected, **every entity gets
a data channel**: negotiated out of band with `id` = entity id on both sides,
ordered + reliable for reliable topics and every pattern entity, unordered with
`maxRetransmits: 0` for a best-effort topic. Frames then travel on the channel; the
WebSocket remains the carrier for anything else (a frame larger than the channel's
message size limit, an entity created before the channel opened, a channel that
died). If WebRTC never connects, or fails later, nothing changes but the carrier.

## Server pushes (JSON)

### `event`

```json
{ "op": "event", "event": "peer_up", "text": "peer 2 up ...", ...fields }
```

`text` is the human-readable one-liner (`dart_event_str`); the structured fields
are per event:

| event              | fields                                          |
|--------------------|-------------------------------------------------|
| `peer_up`          | `peer`                                          |
| `peer_down`        | `peer`                                          |
| `peer_interest`    | `peer`, `publishes`, `receives` (matched counts)|
| `msg_lost`         | `id`, `peer`, `first`, `count`; or `id`, `peer: 0`, `count` for frames the BRIDGE dropped (a best-effort entity whose carrier was backed up) |
| `error`            | `error` (numeric code) + whichever of `topic_name`, `id`, `peer`, `os_error` apply |
| `send_error`       | `id` (the entity the refused frame named), `code`, `error` |
| `rtc`              | `state`                                         |

Everything that goes wrong is one `error` event: `text` carries the message and
`error` the numeric code (a `DartErrorKind`, see docs/node.md). `send_error` is the
bridge's own event for a frame refused synchronously (bad id, bad role, too big,
backpressure timeout).

### `match` : per-entity match state

Pushed whenever an entity's match summary CHANGES (recomputed on peer / interest
events and on the bridge's poll tick, deduplicated, so steady state is silent). One
shape for every kind:

```json
{ "op": "match", "id": 1, "count": 2, "ready": true }
```

`count` is what the entity's side counts: a topic's matched readers of its
publisher (a subscribe-only topic reads 0), a definition's matched callers or
remotes, and 0/1 for a remote (`has_definition`). `ready` is the send-path match-wait
predicate: a send or call now would not block on a forming match (a definition is
always ready). The client maintains its `ready` / `hasDefinition` / count properties
purely from these.

### `log` : a mesh log line (after `log_subscribe`)

```json
{ "op": "log", "level": "error", "node": "gripper", "text": "stalled",
  "wall_us": 1753200000000000, "mono_us": 84213374, "recv_us": 84213402,
  "written_us": 1753200000000012 }
```

`node` is the publishing node's name; `wall_us` is epoch micros; `mono_us` orders
lines within one node; `recv_us` is this node's clock when the poll received it;
`written_us` is the carrying message's source stamp.

## Data plane (frames)

One frame = one WebSocket binary message or one data channel message (both do the
framing, so there are no length fields). **Every frame, every op, both directions,
has the same header** (little-endian):

```
[u8 op][u8 flags][u16 id][u32 seq][u32 peer][u64 written_us][u64 capture_us]
[u8 text_len][text][payload]
```

| field        | meaning |
|--------------|---------|
| `op`         | 1 DATA, 2 VAR, 3 CALL, 4 RESULT, 5 PROGRESS, 6 CANCEL (others reserved: ignore) |
| `flags`      | per op, below (0 when unused) |
| `id`         | the entity the frame belongs to (the client-chosen create id) |
| `seq`        | a call id or request id (0 for DATA / VAR) |
| `peer`       | server to client: the peer id of the publisher / provider / caller / write source (0 = local); a client sends 0 |
| `written_us` | server to client: the source stamp (below); a client sends 0 |
| `capture_us` | when the data was true, as against when it was sent (below); 0 = unstated, and a client MAY set it on DATA |
| `text`       | a short UTF-8 string, at most 255 bytes: a caller name or a result message; empty otherwise |
| `payload`    | the raw message bytes (the schema validates these alone) |

The payload starts at byte `29 + text_len`.

| op | client to server | server to client |
|----|------------------|------------------|
| 1 DATA     | publish on topic `id` | a delivery on topic `id`; `peer` = publisher |
| 2 VAR      | write variable `id`: `flags` 0 = set, 1 = force, 2 = unforce (empty payload) | an update: `flags` bit0 = forced (shadow source active), bit1 = write-event (an `on_write` push, not a change); `peer` = the writing node (0 = the owner itself) |
| 3 CALL     | call remote function/task `id`; `seq` = a client-chosen call id | a request for definition `id`; `seq` = the request id, `peer` = caller, `text` = the caller's node name |
| 4 RESULT   | the reply to request `seq`: `flags` = status (0 ok, 1 app_error, 5 cancelled for tasks; the payload is dropped on 5), `text` = the response message (may be empty) | the outcome of call `seq`: `flags` = status, `peer` = provider, `text` = the response message (empty when the definition sent none: display the default status text then) |
| 5 PROGRESS | one progress update on parked task request `seq` (repeat at will, in order) | one progress update on task call `seq`; `peer` = provider; an EMPTY payload is the RUNNING acknowledgment |
| 6 CANCEL   | (never sent: use the `cancel` op, which returns a verdict) | cancellation requested on parked task request `seq` of definition `id`, at most once per request; cooperative |

Because every frame is self-contained (the caller and its name ride the CALL frame,
the message rides the RESULT frame), nothing ever depends on ordering between the
two planes or between two carriers.

**`written_us` is a SOURCE timestamp**: the writing node's wall clock in UTC
microseconds, taken when its send committed, so a message repaired, replayed from
history, or handed over shared memory keeps the original value. 0 means the
publisher opted out (`no_timestamp` on its topic) or the frame is a synthesized
outcome. It compares across hosts only as well as their clocks are synced, and it
is a different clock from the `recv_us` on a log line: never mix the two.

**`capture_us` is a CAPTURE timestamp**: when the data was true, which is not when
it was sent. A camera driver sets it to the exposure time, so a consumer can line a
frame up against other data from that instant rather than against the moment the
driver published. Same clock and same cross host caveat as `written_us`. 0 means
nobody stated one. On a DATA frame a client may set it to stamp its own publish; on
every other op it is 0. A topic with `no_timestamp` carries neither stamp.

Call `status` is the DART `DartCallStatus`: 0 ok, 1 app_error, 2 no_handler,
3 timeout, 4 peer_lost, 5 cancelled. A call the bridge refuses synchronously (out
of memory, bad state) answers status 5 plus a `send_error` event carrying the
reason, so the client's promise always settles. The one non-terminal status, 6
(running), never rides a RESULT: it is the empty PROGRESS push, after which a task
call's timeout is dropped and exactly one terminal RESULT still follows, even at
provider retire or close. When the definition sends no message on a non-ok status
the client fills the default status text ("timeout", "no handler", ...), so a
message is always displayable.

**Variable updates** are pushed event-driven off the C `dart_variable_on_change`
hook: an update frame goes out the moment a change applies, including once right
after the create reply when a value already exists. A set to the byte-identical
current value pushes nothing (on_change fires only on an actual state change).
Definition and remote sides both receive updates.

**Flow control.** Publishing and writes are fire and forget: no ack (a reliable
topic's guarantees run between the bridge node and its peers, as usual). A frame
that fails immediately surfaces as a `send_error` event. A reliable send under
backpressure (including the send-path match wait) blocks the connection's receive
thread, which backpressures the WebSocket via TCP. Deliveries to a slow client:
a **best-effort** entity's frames are DROPPED once its carrier has 1 MB unsent
(reported as `msg_lost` with `peer: 0`, batched per second, never silent); a
**reliable** entity's frames queue, and a client that cannot drain its carrier at
all is dropped rather than buffered forever (`--max-buffered` bytes, default 8 MiB).
The reference client applies the same drop rule when publishing best-effort frames.

## Media

Video is not a kind of entity: a `VideoFrame`, an `Image` or an `ExternalVideoStream`
(the standard types, docs/stdtypes.md) can sit at the root of a topic, inside a bigger
sample type, in a variable's value, in a task's progress or a call's result, and a
client finds them in the field tables (rows whose `named` is one of those, or that
root name). So the bridge attaches no meaning to them on the frame path: they cross as
bytes inside their message like any other field, and the reference client's `VideoView`
shows whatever is pushed into it (WebCodecs for H264 / H265 / AV1, the image decoder
for MJPEG / JPEG / PNG, a pixel converter for raw formats, onto a canvas; an
`ExternalVideoStream` makes the view follow the URL itself: WHEP as a WebRTC session,
HTTP MJPEG through an image, HLS through a media element where the browser plays it;
RTSP, SRT and RTP cannot play in a browser and are reported as such).

Over WebRTC a client can ask for better: a recvonly video line in its offer, whose
`tracks` entry names an entity, the `path` of a `VideoFrame` inside that entity's
STREAM (a topic's deliveries, a variable's updates, or one task call's progress by
`call` id), and `keep_data`. The bridge binds the line at the answer (the reply's
verdict says whether the field checked out) and from then on packetizes every
**H264, H265 or AV1** frame's `data` at that path onto it as RTP: payload types are
the browser's, from its offer (H264 preferring `packetization-mode=1`); Annex B or
length-prefixed NAL units are both taken; RTP timestamp = `pts` microseconds on the
90 kHz clock, `written_us` when `pts` is 0; delivery starts at a keyframe (the
`keyframe` flag, or an IDR NAL found in the bitstream), frames before the first one
counted as `msg_lost`. The message itself STILL goes out on the frame path, with that
field's `data` emptied (its tail frame has length 0), so the rest of the sample (a
pose, a timestamp, the codec and size) arrives as usual and a client knows the pixels
went by track; `keep_data: true` keeps them in the frame too. MJPEG frames, frames
of a codec the browser did not offer, and every frame while the track is not open
keep their pixels and arrive as before, so the client decodes those itself. A line's
binding is one (entity, path, call); a re-offer for the same triple replaces it;
`Image` fields never ride a track. Media tracks are bridge-to-browser only: a browser
publishes video with the generic path (encode, put the value in whatever entity it
likes).

## The JS/TS client

The reference client is **TypeScript source** (`bridge/client/dart.ts`, zero
runtime dependencies) compiled by pure type stripping into three committed
distributables (regenerate with `node bridge/client/build.mjs`, or configure
CMake with `-DDART_BUILD_JS_CLIENT=ON`; tsc is pinned at 5.5 via npx):

- `dist/dart.mjs` : the ES module (browser + Node >= 22 + Deno + Bun)
- `dist/dart.d.ts` : the type declarations
- `dist/dart.js` : the classic-script twin: identical code with the export block
  replaced by ONE global, `globalThis.DartNode`, for a plain `<script src>` tag
  (no module MIME pitfalls); the constant tables hang off it (`DartNode.VideoCodec`,
  `DartNode.ImageFormat`, `DartNode.MetaSection`)

```ts
import { DartNode } from "../../dist/dart.mjs";

const node = await DartNode.connect("ws://localhost:7480", {
  name: "dashboard",                     // transport: "auto" (try WebRTC, default) | "websocket"
  onEvent: (e) => { if (e.event === "error") console.warn(e.text); } });
console.log(node.transport);             // "webrtc" or "websocket": the API is the same

// pub/sub over plain nested objects (schema = the DSL text; null = raw bytes)
const pub = await node.publisher("pose", "Pose { x: f64, y: f64 }");
pub.send({ x: 1.5, y: 2.0 });
await node.subscriber("pose", "Pose { x: f64, y: f64 }",
  (v, msg) => console.log(msg.publisher, v.x, v.y));

// functions: the handler's (possibly async) return value is the reply
await node.functionDefinition("add", "A { a: i32, b: i32 }", "R { sum: i32 }",
  (req) => ({ sum: req.a + req.b }));
const add = await node.remoteFunction("add", "A { a: i32, b: i32 }", "R { sum: i32 }");
const r = await add.call({ a: 2, b: 3 });   // never rejects on status:
if (r.ok) console.log(r.value.sum);         // { ok, status, value, data, provider, message }

// tasks: a function with progress and cancellation
await node.taskDefinition("transfer", "X { total: u32 }", "P { done: u32 }", "S { bytes: u32 }",
  async (req, ctx) => {
    for (let i = 1; i <= req.total; i++) {
      ctx.signal.throwIfAborted();          // honor a cancel: throw its AbortError
      await doChunk(i);
      ctx.progress({ done: i });
    }
    return { bytes: req.total };
  });
const xfer = await node.remoteTask("transfer", "X { total: u32 }", "P { done: u32 }", "S { bytes: u32 }");
const run = xfer.call({ total: 3 });        // synchronous run handle
run.onProgress((p) => console.log(p === null ? "running" : p.done));  // null = the RUNNING ack
const outcome = await run.result;           // one terminal Response, like fn.call()
// run.cancel() resolves "ok" | "no_cancel" | "not_pending"; the terminal status answers

// variables: client-cached latest, fed by pushed updates
const cfg = await node.remoteVariable("config", "Config { rate_hz: u32 }");
await cfg.wait(2000);
cfg.set({ rate_hz: 100 });

// video: a VideoView is a picture sink (a MediaStream) fed from anywhere: push() any
// VideoFrame / Image value, or attach() it to an entity's stream by field path; over
// WebRTC an attached VideoFrame field arrives as a real video track
const sub = await node.subscriber("camera", "CamSample { pose: Transform, frame: VideoFrame }",
  (v) => drawPose(v.pose));                 // the rest of the sample still arrives
const view = await node.videoView().attach(sub, "frame");
videoEl.srcObject = view.stream;            // view.path: "track" | "decoder"
const cam = await node.video("camera/front");   // sugar: subscribe VideoFrame + attach
snapshot.push(result.value.thumbnail);     // a one-shot value, painted by hand

// logs: publish a line, and stream the whole mesh's lines (rosout-style)
await node.logError("gripper stalled");
await node.onLog((l) => console.log(`[${l.level}] ${l.node}: ${l.text}`));

await node.settle(5000);
node.close();   // outstanding call promises settle with status "cancelled"
```

The factories take optional type parameters (`publisher<T>`,
`remoteFunction<Req, Rsp>`, `taskDefinition<Req, Prg, Rsp>`, ...) defaulting to
plain-object types. `node.topic(...)` is the dynamic form (`send(value)`,
`sendRaw(bytes)`, `onMessage` with `msg.get("vel.dx")` / `msg.value()`). Properties
maintained from `match` pushes: `ready`, `matchCount`, and the side-named
`hasDefinition`, `callerCount`, `remoteCount`. `call(value, timeoutMs)` adds a
client-side bound on top of the bridge's wire timeout; `close()` cancels, a dropped
connection rejects. `VariableDefinition` `initial` is a set right after the create.
Every factory takes `reflect: true` in its options in place of DSL text (`entity.reflected`
says whether the mesh had a type, `await entity.refresh()` re-types later); `node.mesh()`
and `node.meshFind(kind, name)` read the folded mesh.

Task cancellation maps to the platform primitive: a CANCEL frame aborts
`ctx.signal` (an `AbortController` per request). The handler honors it by throwing
the signal's reason (`ctx.signal.throwIfAborted()`), which the client turns into
status 5; any other throw answers `app_error` with the error's text; a normal return
always completes `ok`, even after an abort. On the caller, `run.result` never
rejects on a status, `run.onProgress` replays updates buffered before its
registration, and a task call takes no client-side `timeoutMs`.

Introspection is `await node.peers()`, `await node.entities()`,
`await node.peerEntities(peerId)`, and `await node.meta(peerId, MetaSection.All)`.
The source stamp reaches every delivery surface as `writtenUs`: `msg.writtenUs` on a
topic message, the info argument of a variable `onChange` / `onWrite` handler, `r.writtenUs`
on a call result, and `info.writtenUs` on a definition's request handler.

**WebRTC in the client.** `connect` opens the WebSocket, opens the node, then (unless
`transport: "websocket"`, or the runtime has no `RTCPeerConnection`, or the bridge
was started `--no-webrtc`) asks the bridge for its ICE servers, offers, and waits up
to `rtcTimeoutMs` (4000) for the connection; a refusal reported by the bridge (`rtc`
event `failed`) falls back at once. Every entity created afterwards opens its data
channel before the create request goes out; frames for an entity whose create reply
is still in flight are held and replayed. A best-effort frame is dropped rather than
queued when its channel has 1 MB unsent; a frame over the SCTP message size limit
takes the WebSocket. `VideoView.attach` on a live link adds a recvonly transceiver and
re-offers, so the track binds within one round trip (`view.trackState` carries the
bridge's verdict); if the link later fails, the same view keeps painting the values
that then arrive with their pixels on the data plane. `Layout.mediaFields()` lists
where a type carries video (`VideoFrame`, `Image` or `ExternalVideoStream`), and `attach`
defaults to the first such field; only a `VideoFrame` field asks for a track.

## Non-goals

- **Introspection is query-based, never a live mirror.** The bridge keeps no mesh
  model for the client and pushes nothing mesh-wide (only the per-entity `match`
  state). Continuous whole-mesh observability remains a job for a dedicated tool
  (the explorer), not a browser over this bridge.
- **No auth, no TLS.** The bridge binds 127.0.0.1 by default; exposing it
  (`--bind 0.0.0.0`) puts full mesh access on that port. Put a reverse proxy in
  front for wss:// or auth (WebRTC's own DTLS encrypts the data path regardless).
- **No JSON data path.** The data plane is bytes; a client that wants JSON
  converts at the edge with the field tables (the reference client shows how).
- **No per-publisher layout tables.** A typed delivery is validated against the
  publisher's schema by the node, but the client decodes with its own layout.
  Byte-identical schema text end to end (the recommended deployment) is always
  exact.
- **No browser-to-mesh media tracks.** Publishing video from a browser is the
  generic path (WebCodecs + `VideoFrame` messages); the bridge depacketizes nothing.
