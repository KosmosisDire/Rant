# DART WebSocket bridge protocol (v9)

The bridge turns a WebSocket connection into a full DART node on the mesh. One
connection = one node: the bridge opens the node when asked, owns its sockets and
service thread, and closes it (with a BYE) when the connection drops. Everything a
native node needs -- **pub/sub** plus the pattern entities (**functions**,
**variables**) -- arrives over one socket, so a browser, a phone, or
any language with a WebSocket client is a first-class peer.

This is a lean proxy. Introspection is **query-based and pull-only**: the client
asks (`peers` / `entities` / `peer_entities` / `meta`) and the bridge answers from
what its node already knows; nothing mesh-wide is pushed or mirrored, and there is
no way to see another node's message *schema* (a typed topic/entity carries its own
declared schema, both ends paste the same DSL text, so the client encodes and
decodes with the field tables the create replies return). The client owns all
encoding: payloads cross the bridge as raw bytes.

Two planes, split by WebSocket frame type:

- **Text frames = control plane, JSON.** Open the node, create topics and pattern
  entities, flip roles, drain, settle. Requests carry a `seq`; every request gets
  exactly one reply echoing it. The server also pushes unsolicited `event`,
  `match`, and `request` messages.
- **Binary frames = data plane.** Publish/delivery, variable writes/updates,
  calls/responses: a fixed little-endian header of a few
  bytes and the raw payload after it. No JSON, no base64: the hot path costs a
  memcpy.

All integers in binary frames are little-endian (matching the DART wire, and
`DataView`'s `true` flag in JS). The protocol version is returned by `open`;
there is no cross-version compatibility (see the repo rule: robust parsing, no
back-compat).

## Connection lifecycle

1. Client connects: `ws://host:7480/`.
2. Client sends `open` (must be the first message). The bridge creates the node.
3. Client creates topics/entities, publishes, calls, receives, at will.
4. Client closes the socket (or errors out): the bridge fails any function
   requests still parked at the client (so remote callers get an answer now, not a
   timeout), then closes the node with a BYE and frees everything. There is no
   explicit close op.

Any request before `open` (or a second `open`) gets an error reply. A binary
frame before `open`, or on an unknown id, is dropped and reported with a
`send_error` event (never a reply: data-plane frames carry no seq).

## Control plane (text frames)

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

Reply: `{ "ok": true, "proto": 9, "name": "dashboard" }` (the actual node name,
so an auto-generated one is visible).

### `topic` : create a topic

```json
{ "op": "topic", "seq": 2,
  "name": "pose",                 // required: the cross-peer topic identity
  "role": "pubsub",               // "pubsub" | "pub" | "sub" | "inactive"
  "schema": "Pose { stamp: u64, x: f64, y: f64, vel: { dx: f32, dy: f32 } }",
                                  // optional DSL text; omitted = raw-bytes topic
  "reliable": false,              // QoS, all optional
  "keep_last": 16,
  "catch_up": 0,
  "max_message_bytes": 0,
  "backpressure_wait_ms": 0,
  "max_rate_hz": 0 }              // subscriber, best-effort: cap delivery from each publisher
```

Reply for a typed topic returns the compiled layout, which is everything a
client needs to encode and decode messages by itself (fixed fields at absolute
offsets, little-endian, no padding):

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

`id` is the topic id used in binary frames (the node-local index, dense from
0). `hash` is the 64-bit schema identity as hex (it exceeds JS safe integers). A
raw topic replies just `{ "ok": true, "id": 0 }`.

A **BARE-TYPE schema** (the whole schema is one type, e.g. `bool` or `f32[]`, not a
struct) replies with exactly ONE field whose `path` is the **empty string**, and its
type name is empty (an anonymous root: a bare type's identity is its shape, so the
same one from any language is the same bytes and the same `hash`):

```json
{ "ok": true, "id": 0, "size": 1, "hash": "ee90234f61d2520b",
  "fields": [ { "path": "", "kind": "bool", "offset": 0, "size": 1 } ] }
```

Nothing else changes: that one field is read and written exactly like any other, at
`offset` (or as the single tail frame for a variable kind). The reference client
detects the shape and lets the message BE the value (`topic.send(true)`,
`msg.value() === true`) instead of an object with one empty-named key.

**`size`** is the length of the **fixed section** -- the exact message size when the
schema has no variable fields, and where the variable tail begins when it does.

**Field kinds** (`kind`):

| kind      | shape | extra fields |
|-----------|-------|--------------|
| `u8`..`u64`, `i8`..`i64`, `f32`, `f64`, `bool` | fixed scalar at `offset` | -- |
| `arr`     | fixed array of `count` elements at `offset` | `elem`, `count` (+ `cap` if `elem` is `string`) |
| `struct`  | group header; members follow with dotted paths (a decoder can skip it) | -- |
| `string`  | capped string, fixed slot `[u16 len][cap bytes]` at `offset` (`size` = 2+cap) | `cap` |
| `enum`    | named integer; on the wire just its `backing` scalar at `offset` | `backing`, `variants` (`[{name,value}]`) |
| `vstring` | variable string; lives in the tail (`offset`/`size` = 0) | -- |
| `varr`    | variable array; lives in the tail, live element count | `elem` (+ `cap` if `elem` is `string`) |
| `map`     | self-describing tagged value tree; lives in the tail | -- |

A field's type may also carry a NAME (`named`), which the schema wire carries and no
message byte does. It NARROWS matching: a `Pose` field never binds to a same-shaped
`Twist` one, while a reader declaring the bare shape reads either. The `kind` a row
reports is always what the name WRAPS (a `Pose` reads as `struct`), so a decoder that
ignores `named` is still correct; see docs/stdtypes.md for the standard names.

| extra field | on | meaning |
|-------------|----|---------|
| `named`      | any row | the field type's name (`"Pose"`, `"Uuid"`, `"Timestamp"`) |
| `elem_named` | `arr` / `varr` | the ELEMENT type's name (`"Float3"` for `Float3[4]`) |
| `elem_size`  | `arr` / `varr` | bytes of one element |
| `elem_struct`| `arr` / `varr` | the element is a struct: its element-0 template rows follow |
| `in_array`   | any row | this row is a member of that array row's element template |

An array whose element is a STRUCT (`Float3[4]`, `{ x: f32 }[]`) contributes ONE set of
member rows, the element-0 template: each carries `in_array` naming its array row, and
element *i* of a member sits at `offset + i * elem_size` (for a variable array, at that
stride inside the array's tail frame). An element is always FIXED, so its stride is
static.

An **`enum`** is a fixed field carrying its `backing` integer (`u8`..`i64`); the
`variants` array names the values, so a client reads/writes the number and resolves
the label from `variants` (an unknown value has no name: forward-compatible). The
reference client (`dart.ts`) exposes `Schema.enumName` / `Schema.enumValue`.

The **variable kinds** (`vstring` / `varr` / `map`) have no fixed offset: each is
one `[u32 len][payload]` frame in the message tail, the frames in depth-first
declaration order (one declared inside a nested struct still claims a top-level frame),
starting at `size`. To read variable field *k*, walk from `size` reading a u32
length and hopping, *k* times. The `map` payload is a JSON-style tagged tree; the
reference client (`dart.ts`) has the codec.

A schema error replies `ok:false` with the reason in `error`. The schema DSL text
must be byte-identical across nodes for the identical-hash fast path, and
structurally a subset to match a wider publisher (the normal DART rules; the
bridge adds nothing).

### `role` / `drain` : topic maintenance

```json
{ "op": "role",  "seq": 3, "topic": 0, "role": "sub" }
{ "op": "drain", "seq": 5, "topic": 0, "timeout_ms": 1000 }
```

`role` re-advertises immediately; peers rematch (`"inactive"` = declared but
off). `drain` replies `{ "ok": true, "drained": true }` when every reader acked;
call before closing when the last burst matters (reliable topics).

### `settle` : wait for discovery + matching to converge

```json
{ "op": "settle", "seq": 6, "timeout_ms": 5000 }   // negative = 3 announce intervals
```

Reply `{ "ok": true, "settled": true }` when everything created so far is matched
against everyone currently on the network. Runs on the connection's receive
thread, so further requests from this client wait for it (nothing else does).

### `log` / `log_subscribe` : the built-in @dart/log topics

Every node hosts three shared reliable log topics (`@dart/log/{error,warn,info}`,
rosout-style), unless opened with `disable_logs`. `log` publishes a line; the text
is formatted client-side (truncated at 512 bytes on the wire).

```json
{ "op": "log", "seq": 11, "level": "error", "text": "gripper stalled" }
```

Reply `{ "ok": true }`. `log_subscribe` starts pushing every OTHER node's lines at
the requested levels as `log` server pushes (below); it never receives this node's
own lines. Idempotent per level; late-join history (each writer's last `keep_last`
lines) replays on match.

```json
{ "op": "log_subscribe", "seq": 12, "levels": ["error", "warn"] }   // omit levels = all three
```

Reply `{ "ok": true }`.

### Pattern entities

The four creates below return a dense per-connection **entity id** (its own id
space, separate from topic ids) plus the compiled field tables for every schema
the entity carries. All schemas are optional DSL text; omitted = raw bytes.
Entity ids appear in the pattern binary frames and in `match` pushes.

**`function_definition`** -- host a request/response function (ONE definition per
name on the network; exactly one reply per call).

```json
{ "op": "function_definition", "seq": 7, "name": "add",
  "req_schema": "AddReq { a: i32, b: i32 }",     // optional
  "rsp_schema": "AddRsp { sum: i32 }",           // optional
  "timeout_ms": 0, "backpressure_wait_ms": 0 }   // optional
```

Reply: `{ "ok": true, "id": 0, "req": {size, hash, fields}, "rsp": {...} }`
(each block present only when that schema was given). The bridge registers a
handler that **defers every request to the client**: see "Function requests"
below. There is no bridge-side handler logic; the client is the implementation.

**`remote_function`** -- a reference to a function hosted elsewhere. Same fields
and reply shape as `function_definition` (no handler). `timeout_ms` sets the call
timeout (0 = 5s): the answer the wire never delivers becomes status `timeout`.

**`variable_definition`** / **`remote_variable`** -- replicated state with ONE
owner (the definition side holds the authoritative value).

```json
{ "op": "variable_definition", "seq": 8, "name": "config",
  "schema": "Config { rate_hz: u32 }",   // optional
  "initial": [80, 0, 0, 0],              // optional raw payload bytes
  "read_only": false,                    // no set channel: remote sets get refused
  "allow_force": false,                  // permit force (local + remote)
  "on_write": false,                     // also push EVERY applied write (not just changes)
  "catch_up": 0, "backpressure_wait_ms": 0 }
{ "op": "remote_variable", "seq": 9, "name": "config", "schema": "...", "on_write": false }
```

Reply: `{ "ok": true, "id": 1, size, hash, fields }` (layout only when typed).
`initial` is raw pre-encoded bytes; a client that encodes with the reply's field
table (the reference client) instead sends a normal set right after the create,
which is equivalent (the value channel latches the latest for late joiners).
With `on_write`, the bridge also pushes a variable-update frame on every applied
write (flag bit1 set; see the data plane), so a client can observe writes that
leave the value unchanged, not only state changes. Both sides may set it.

### Introspection (query-based, pull-only)

Four request ops let a client ask what the node knows about the mesh. `peers`,
`entities` and `peer_entities` are **local reads** answered synchronously from this
node's own discovery state; `meta` is an **async directed call** to a peer's
`@dart/meta` endpoint. None of them push: a client polls at whatever cadence it
wants (the `match` pushes below already cover live match-state changes cheaply).

**`peers`** -- snapshot the discovered peer table.

```json
{ "op": "peers", "seq": 20 }
```

Reply: `{ "ok": true, "peers": [ { "id": 2, "name": "gripper",
"address": "192.168.1.9:47001", "active": true, "fragment_size": 1350 } ] }`.
`active` is false for a dormant (dropped-but-remembered) peer.

**`entities`** / **`peer_entities`** -- the entities this node hosts, or the ones a
peer advertises (pattern channels folded: a function's req/rsp pair is one
`function` entity; a variable's set channel merges as `writable`).

```json
{ "op": "entities", "seq": 21 }
{ "op": "peer_entities", "seq": 22, "peer": 2 }
```

Reply: `{ "ok": true, "entities": [ ... ] }` (`peer_entities` also echoes `peer`).
A dropped (silent, resumable) peer answers `entities: []` by default: its cached
entities are its dead incarnation's, and after a restart they would stand beside the
live incarnation's. Pass `"include_dropped": true` to serve that last-known view
anyway (a ghost display); gate on `peers`' `active` flag yourself then.
Each entity:

```json
{ "kind": "variable", "name": "config", "provides": true, "consumes": false,
  "reliable": true, "writable": true, "forceable": false, "index": 3, "hash": 2748219392,
  "schema_hash": "9f3a5c1e22b40d77",
  "schema": { "size": 4, "hash": "9f3a5c1e22b40d77",
              "fields": [ { "path": "rate_hz", "kind": "u32", "offset": 0, "size": 4 } ] } }
```

`kind` is `topic` | `function` | `variable`. `provides`/`consumes` are
the source/sink sides. `writable`/`forceable` appear on variables; `incomplete: true`
marks a surfaced pattern half-pair. `hash` is the low-32 name hash; `schema_hash`
(and `rsp_schema_hash` on functions) ride as hex, present only when typed and
fetched. `name` is a `"0x????????"` placeholder until the peer's details arrive --
open with `fetch_details` to resolve names and schemas for topics this node does not
itself share.

When the schema is known, **`schema`** carries the full field table -- the exact
`{ size, hash, fields }` block a `topic` create reply returns (see that op for the
kind/offset/enum rules), so a client renders a discovered topic's types and can
build a decoder for its live messages with no shared DSL. A `function` entity also
carries **`rsp`** (the response field table). Both are absent for a raw/untyped
entity or one whose details have not been fetched. The reference client folds these
into `Entity.schema` / `Entity.rspSchema`, ready for `new Layout(entity.schema)`.

**`meta`** -- fetch a peer's `@dart/meta` runtime snapshot (an async call; works
under the bridge's service thread).

```json
{ "op": "meta", "seq": 23, "peer": 2, "sections": 0 }
```

`sections` is an OR of the section mask (`1` node, `2` proc, `4` topics, `8` peers;
`0` = all). The reply echoes `seq` when the call completes (or times out); it never
fails on status -- inspect `status`:

```json
{ "ok": true, "valid": true, "status": 0, "provider": 2,
  "info": { "node": { "name": "gripper", "uptime_us": 84213374, "mem_in_use": 30512,
                      "peers": 3, "topics": 5, ... },
            "proc": { "pid": 4123, "cpu_us": 220000, "rss": 8912896, ... },
            "topics": [ { "name": "pose", "role": "pub", "tx_msgs": 900, ... } ],
            "peers":  [ { "id": 1, "name": "dashboard", ... } ] } }
```

`status` is the `DartCallStatus` (0 ok, 3 timeout, ...); `info` is the whole decoded
self-describing snapshot body (the sub-objects present per the requested sections,
empty when `valid` is false). This is the same data the C++/C#/Python wrappers'
`meta_request` / `MetaAsync` / `meta` return.

## Server pushes (text frames)

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
| `msg_lost`         | `topic`, `peer`, `first`, `count`               |
| `error`            | `error` (numeric code) + whichever of `topic_name`, `topic`, `peer`, `os_error` apply |
| `send_error`       | `topic` OR `entity` (the id the failed data frame named), `code`, `error` |

Everything that goes wrong is one `error` event: `text` carries the message and
`error` the numeric code (a `DartErrorKind`: 1 name-collision, 2 qos-incompatible,
3 kind-mismatch, 4 schema-mismatch, 5 interest-overflow, 6/7 meta-truncated
interest/schema, 8 peer-meta-too-big, 9 msg-too-big, 10 peer-refused,
11 evicted-unsent, 12 unmatched-send, 13 duplicate-authority, 14 oom,
15 platform, 16 socket, 17 bind, 18 mcast-join, 19 send, 20 recv, 21 poll,
22 waker). A client that only prints `text` needs no per-code handling.
`send_error` is the bridge's own event for a data-plane frame refused
synchronously (bad id, bad role, too big, backpressure timeout).

### `match` : per-entity match state

Pushed whenever a created entity's match summary CHANGES (recomputed on peer /
interest events and on the bridge's poll tick, deduplicated against the last
pushed value, so steady state is silent). The client maintains its
`ready`/`hasDefinition`/count properties purely from these.

```json
{ "op": "match", "type": "topic",               "id": 0, "matches": 2, "ready": true }
{ "op": "match", "type": "function_definition", "id": 0, "callers": 1 }
{ "op": "match", "type": "remote_function",     "id": 1, "has_definition": true }
{ "op": "match", "type": "variable_definition", "id": 2, "remotes": 1 }
{ "op": "match", "type": "remote_variable",     "id": 3, "has_definition": true }
```

`type` picks the id space (`topic` = topic ids, everything else = entity ids).
`ready` is the send-path match-wait predicate: a send now would not block on a
forming match.

### `log` : a mesh log line (after `log_subscribe`)

One per delivered `@dart/log` line at a subscribed level, from any other node:

```json
{ "op": "log", "level": "error", "node": "gripper", "text": "stalled",
  "wall_us": 1753200000000000, "mono_us": 84213374, "recv_us": 84213402,
  "written_us": 1753200000000012 }
```

`node` is the publishing node's name; `wall_us` is epoch micros (comparable across
nodes, and within JS safe-integer range); `mono_us` orders lines within one node;
`recv_us` is this node's clock when the poll received it; `written_us` is the carrying
message's source stamp (as on the binary frames). Low rate, so this rides the text
plane as decoded JSON (no schema table needed).

### `request` : an incoming function call (definition side)

The bridge defers every call to the client as a pair of frames, JSON meta first,
then the binary request payload (WebSocket frames are ordered, so the client can
correlate by `req`):

```json
{ "op": "request", "fn": 0, "req": 41, "caller": 2, "caller_name": "dashboard" }
```

followed by binary `0x05` (below) carrying the same `req` id and the payload.
The client answers with a binary `0x05` request-reply frame whenever it is ready
(the call is parked bridge-side as a deferred reply; the caller's timeout still
bounds the wait). Requests parked when the connection drops are failed
(`app_error`) so callers are not left to their timeout.

## Data plane (binary frames)

One WebSocket binary message = one DART payload. WebSocket does the framing, so
there are no length fields. Byte 0 is the op; the same op value means the
matching thing in each direction. All headers little-endian.

**Client to server:**

| op | frame | meaning |
|----|-------|---------|
| `0x01` | `[u16 topic][payload]` | publish on a topic |
| `0x02` | `[u16 entity][u8 mode][payload]` | variable write: mode 0 = set, 1 = force, 2 = unforce (empty payload) |
| `0x04` | `[u16 entity][u32 call][payload]` | function call; `call` is a client-chosen correlation id |
| `0x05` | `[u32 req][u8 status][u8 msg_len][msg][payload]` | reply to a pushed request: status 0 = ok, 1 = app_error; `msg` = the response message (UTF-8, max 255 bytes, may be empty) |

**Server to client:**

| op | frame | meaning |
|----|-------|---------|
| `0x01` | `[u16 topic][u32 publisher][u64 written_us][payload]` | topic delivery |
| `0x02` | `[u16 entity][u8 flags][u64 written_us][payload]` | variable update; `flags` bit0 = forced (shadow source active), bit1 = write-event (an `on_write` push, not a change) |
| `0x04` | `[u32 call][u8 status][u32 provider][u64 written_us][u8 msg_len][msg][payload]` | function-call outcome for `call`; `msg` = the response message (UTF-8, empty when the definition sent none: display the default status text then) |
| `0x05` | `[u16 fn][u32 req][u64 written_us][payload]` | request payload (pairs with the `request` JSON push) |

Every server-to-client frame carries `written_us` as its last fixed header field;
the call outcome (0x04) alone appends the variable `[u8 msg_len][msg]` after it.
The payload starts at 15, 12, 15, 19 + msg_len, 15 bytes respectively.
Client-to-server frames carry no stamp.

**`written_us` is a SOURCE timestamp**: the writing node's wall clock in UTC
microseconds, taken when its send committed, so a message repaired, replayed from
history, or handed over shared memory keeps the original value. 0 means the
publisher opted out (`no_timestamp` on its topic) or the frame is a synthesized
outcome (a refused call). It compares across hosts only as well as their clocks
are synced, and it is a different clock from the `recv_us` on a log line: never
mix the two.

Other op values are reserved: a receiver must ignore unknown ops, the server
drops them silently.

Call `status` is the DART `DartCallStatus`: 0 ok, 1 app_error, 2 no_handler,
3 timeout, 4 peer_lost, 5 cancelled. A call the bridge refuses synchronously
(out of memory, bad state) answers status 5 (cancelled) plus a `send_error`
event carrying the reason, so the client's promise always settles.

The call outcome's `msg` is the DART response message (`DartResponse.message`):
human-readable failure text set by the definition (`dart_request_fail`, or a
reply frame's `msg`, or -- in the reference client -- a thrown `Error`'s text).
When it is empty on a non-ok status the client fills the default status text
("timeout", "no handler", ...), so `message` is always displayable.

**Variable updates** are pushed event-driven off the C `dart_variable_on_change`
hook: an update frame goes out the moment a change applies (no poll latency),
including once right after the create reply when a value already exists (a
definition's `initial`). A set to the byte-identical current value pushes
nothing (on_change fires only on an actual state change; idempotent). Definition
and remote sides both receive updates (a definition client sees remote writes
land). A variable opened with `on_write` ALSO gets a frame on every applied write
(flag bit1 set), including byte-identical re-sets that fire no change event; the
client routes those separately (the reference client's `onWrite` handler) and the
cached value stays driven by the change frames.

Publishing/writes are fire and forget: no ack (a reliable topic's guarantees run
between the bridge node and its peers, as usual). A data frame that fails
immediately surfaces as a `send_error` event. A reliable send under backpressure
(including the send-path match wait) blocks the connection's receive thread,
which backpressures the WebSocket via TCP: a fast publisher into a slow mesh
slows down instead of buffering unboundedly.

Delivery on a slow client: frames queue in the socket's send buffer. If the
WebSocket client cannot drain its TCP connection the bridge drops the
connection rather than buffer forever (`--max-buffered` bytes, default 8 MiB).

## The JS/TS client

The reference client is **TypeScript source** (`bridge/client/dart.ts`, zero
runtime dependencies) compiled by pure type stripping into three committed
distributables (regenerate with `node bridge/client/build.mjs`, or configure
CMake with `-DDART_BUILD_JS_CLIENT=ON`; tsc is pinned at 5.5 via npx):

- `dist/dart.mjs` -- the ES module (browser + Node >= 22 + Deno + Bun)
- `dist/dart.d.ts` -- the type declarations
- `dist/dart.js` -- the classic-script twin: identical code with the export
  block replaced by ONE global, `globalThis.DartNode`, for a plain
  `<script src>` tag (no module MIME pitfalls)

```ts
import { DartNode } from "../../dist/dart.mjs";

const node = await DartNode.connect("ws://localhost:7480", {
  name: "dashboard",
  onEvent: (e) => { if (e.event === "error") console.warn(e.text); } });

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
if (r.ok) console.log(r.value.sum);         // { ok, status, value, data, provider }

// variables: client-cached latest, fed by pushed updates
const cfg = await node.remoteVariable("config", "Config { rate_hz: u32 }");
await cfg.wait(2000);
cfg.set({ rate_hz: 100 });

// logs: publish a line, and stream the whole mesh's lines (rosout-style)
await node.logError("gripper stalled");
await node.onLog((l) => console.log(`[${l.level}] ${l.node}: ${l.text}`));  // levels default to all

await node.settle(5000);
node.close();   // outstanding call promises settle with status "cancelled"
```

The factories take optional type parameters (`publisher<T>`,
`remoteFunction<Req, Rsp>`, ...) defaulting to plain-object types, so JS callers
see no difference. `node.topic(...)` remains the dynamic form with flat
dotted-path `get`/`send` and raw bytes. Properties maintained from `match`
pushes: `ready`, `matchCount`, `hasDefinition`, `callerCount`, `remoteCount`.
`call(value, timeoutMs)` adds a client-side bound on top of the
bridge's wire timeout; `close()` cancels, a dropped connection rejects.
`VariableDefinition.initial` is implemented as a set right after the create
(encoding needs the field table the create returns).

Introspection is `await node.peers()`, `await node.entities()`,
`await node.peerEntities(peerId)`, and `await node.meta(peerId, MetaSection.All)`
(returns `{ valid, status, provider, info }`, never throwing on status). A
variable created with `{ onWrite: true }` routes every applied write to its
`onWrite(handler)` callback, separately from `onChange`.

The source stamp reaches every delivery surface as `writtenUs` (microseconds, a
plain number): `msg.writtenUs` on a topic message, the second argument's `writtenUs` on
a variable `onChange`/`onWrite` handler, `r.writtenUs` on a call result, and
`info.writtenUs` on a function definition's request handler.

## Non-goals

- **Introspection is query-based, never a live mirror.** `peers` / `entities` /
  `peer_entities` / `meta` answer on demand from what the node already knows; the
  bridge keeps no mesh model for the client and pushes nothing mesh-wide (only the
  per-entity `match` state, which the client needs for send-readiness, is pushed).
  There is still no `adopt`, and no way to see another node's message *schema* -- a
  typed subscriber declares its own (the same DSL the publisher uses); a raw
  subscriber gets bytes. Continuous whole-mesh observability (topology graphs, live
  traffic) remains a job for a dedicated tool (the explorer), not a browser over
  this bridge.
- **No auth, no TLS.** The bridge binds 127.0.0.1 by default; exposing it
  (`--bind 0.0.0.0`) puts full mesh access on that port. Put a reverse proxy in
  front for wss:// or auth.
- **No JSON data path.** The data plane is bytes; a client that wants JSON
  converts at the edge with the field tables (the reference client shows how).
- **No per-publisher layout tables.** A typed delivery is validated against the
  publisher's schema by the node, but the client decodes with its own layout.
  Byte-identical schema text end to end (the recommended deployment) is always
  exact.
