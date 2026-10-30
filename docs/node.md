# Running a node

## Threading

Every `rant_node_*` and `rant_topic_*` call is thread safe. A node level lock serializes
them and is never held across a blocking wait.

Drive a node one of two ways:

- Call `rant_node_poll` from your own loop, from one thread or several. A send on another
  thread wakes a blocked poll.
- Call `rant_node_start`. A background service thread owns the loop, `rant_node_poll` then
  returns `RANT_ERR_STATE`, and callbacks fire on that thread, never two at once per node.
  `rant_node_stop` joins it. `rant_node_close` implies stop.

Inside `on_message` or `on_event` you may call `rant_topic_send` and read only queries.
Poll, create topic, set role, drain, start, stop and close are refused with
`RANT_ERR_STATE` (or NULL or 0). Nothing is corrupted. A topic handler does not hold the
node lock while it runs, so a send from another thread never waits for it, and a create
or close from another thread waits until the handler returns.

Flow control needs no poll cadence. A send that would overwrite unacked reliable history
sleeps, bounded by `qos.backpressure_wait_us`, 0 = never. No other send waits. With a
service thread a send writes the socket before it returns. When the socket is full and the
next send must overwrite history that never left, KEEP_LAST proceeds and a
`RANT_E_EVICTED_UNSENT` error fires. `rant_node_evicted_unsent` counts them.

Reflection views (the names, addresses and schemas the `rant_node_*_next` walks return)
are bracketed with `rant_node_lock` and `rant_node_unlock` when a poller runs on another
thread.

## Callback queues

A callback fires inline on the thread running the node loop unless the handle was created
with a queue. Then the loop parks the callback as a record and `rant_queue_dispatch` runs
the parked callbacks on the calling thread outside the node lock, so they may use the
whole API. The choice is made at creation and never changes.

- `rant_node_create_queue(n)` makes a queue, up to `RANT_QUEUES_MAX` (8) per node, freed
  at close.
- `RantTopicOpts.queue` binds a topic to it. `qos.queue_bytes` caps its ring, 0 = 1 MB.
- `rant_queue_dispatch(q, max_callbacks, timeout_ms)` runs what was parked at entry,
  oldest first across the queue's topics, at most `max_callbacks` (0 = all). Timeout 0
  returns at once when empty, positive waits that long for the first record, negative
  waits forever. Without a service thread the wait drives the loop itself.
- `rant_queue_stats(q, &waiting, &dropped)` counts records parked and dropped, and
  `rant_topic_queue_stats` reads one topic's ring.
- `rant_node_set_event_queue(n, q)` parks `on_event` too, on a node level ring capped by
  `RantNodeOpts.event_queue_bytes` (0 = 64 KB) that drops the oldest on overflow. NULL
  makes events inline again and drops what is parked. `rant_last_error` is current either
  way. Functions, tasks and variables take `queue` in their options the same way, see
  docs/patterns.md.
- `rant_node_set_log_queue(n, q)` parks the log lines this node receives on `q`, the
  built in log topic being created at open without one. `rant_node_set_status_queue` does
  the same for status sets. NULL makes them inline again and drops what is parked.

One thread drains a queue at a time. A concurrent dispatch, a dispatch from an inline
callback or from one of the queue's own callbacks returns `RANT_ERR_STATE`. Retiring a
topic from its own running callback, or while it runs on another thread, is refused the
same way, and from anywhere else it drops the unrun records. `rant_node_close` is refused
while any queue is being dispatched.

Each queued handle has its own ring. It starts small and grows to `qos.queue_bytes` (0 =
`RANT_QUEUE_CAP`, 1 MB), and two bigger messages still fit. At the cap the reliability QoS
decides: best effort overwrites the oldest and fires `RANT_MSG_LOST`, reliable parks
delivery so the publisher's send blocks on normal flow control, and dispatching unparks
it. Size the ring to cover the writer's burst (at least `keep_last` times the message
size), or a parked reader heals through the slow repair path. The extra copy costs about
15% of peak goodput at small payloads and up to 35% at 1 MB. Measure with
`rant_bench queuebench`.

```c
RantQueue *main_q = rant_node_create_queue(n);
RantTopic *pose = rant_node_create_topic(n, "Robot/Pose", RANT_SUB_ONLY, pose_schema,
                                         &(RantTopicOpts){ .queue = main_q });
rant_node_start(n);
while (running){
    rant_queue_dispatch(main_q, 0, 0);   /* never waits, runs the parked callbacks here */
    draw();
}
```

## Pull topics

A subscriber that reads when it wants, such as a frame paced viewer, creates its topic
with `RantTopicOpts.pull = 1`. It never calls back: messages wait in the topic's ring, sized
and capped as above, until the app takes them.

- `rant_topic_take(topic, &msg, timeout_ms)` returns the oldest waiting message.
- `rant_topic_take_latest(topic, &msg, timeout_ms)` returns the newest and drops the older
  ones.

Both return 1 with a message, 0 with none, and `RANT_ERR_STATE` on a topic created without
pull. The views stay valid until that topic's next take. Timeouts work as on
`rant_queue_dispatch`. Pull with a `queue` is refused, since pull has no callbacks to park.

```c
RantTopic *img = rant_node_create_topic(n, "Camera/Image", RANT_SUB_ONLY, img_schema,
                                        &(RantTopicOpts){ .pull = 1 });
RantMsg m;
if (rant_topic_take_latest(img, &m, 0) == 1) draw(m.data);
```

## Timestamps

Every `RantMsg` carries three clocks.

- `recv_us` is this node's monotonic clock when the poll received the message (at enqueue
  for a queued topic). Use it for rates and jitter.
- `written_us` is the writer's wall clock in UTC microseconds when it wrote the message.
  Repair, catch up replay, shared memory and a callback queue all keep the original
  stamp. Compare it across hosts only as far as their clocks are synced. Never mix it with
  `recv_us`.
- `capture_us` is when the data was true, which is not when it was sent. A camera driver
  sets it to the exposure time, so a consumer can line the frame up against other data
  from that instant instead of against the moment the driver got round to publishing.
  0 means the publisher gave none. Same clock and same caveats as `written_us`.

The publisher sets it per message, and it costs 8 wire bytes only on the messages that
carry one:

```c
rant_topic_send(topic, frame, &(RantSendOpts){ .capture_us = exposed_at });
rant_topic_send(topic, frame, NULL);          /* no capture time, no extra bytes */
```

The two together measure the pipeline: `written_us - capture_us` is how long the sensor
and the driver took, and the rest of the way to `recv_us` is transport.

A publisher opts out of both stamps per topic with `qos.no_timestamp`. `written_us` and
`capture_us` are then 0 and a capture time cannot ride that topic. The patterns layer
forwards the commit stamp as `written_us` on `RantRequest`, `RantResponse`,
`RantProgress` and `RantVariableUpdate`, and carries no capture time: it describes
sensor data on a topic, not a call or a write.

## Errors and events

Everything that goes wrong arrives the same way: a `RantEvent` with kind `RANT_ERROR` and
an `.error` code (`RANT_E_*`) naming the fault. `if (ev->kind == RANT_ERROR)` is the one
"did something break" test, and `rant_event_str` prints the full text.

The other kinds are `RANT_PEER_UP`, `RANT_PEER_DOWN`, `RANT_PEER_INTEREST` (peer
lifecycle), `RANT_MSG_LOST` (best effort loss, expected, not an error) and
`RANT_STATUS_CLEARED` (see Status below).

| group | codes |
|---|---|
| matching and config | `NAME_COLLISION`, `QOS_INCOMPATIBLE`, `SCHEMA_MISMATCH`, `KIND_MISMATCH`, `INTEREST_OVERFLOW`, `META_TRUNCATED_INTEREST`, `PEER_META_TOO_BIG`, `MSG_TOO_BIG`, `PEER_REFUSED`, `EVICTED_UNSENT`, `UNMATCHED_SEND`, `DUPLICATE_AUTHORITY` |
| IO and setup | `OOM`, `PLATFORM`, `SOCKET`, `BIND`, `MCAST_JOIN`, `SEND`, `RECV`, `POLL`, `WAKER`, `BAD_ADDRESS`, `BAD_DOMAIN`, `BAD_PREFIX`, `BAD_LOG_LEVEL`, `BRIDGE` |
| a refused create | `NAME_COLLISION` with `.peer` 0 (a live same name topic on this node), `BAD_NAME`, `STATE` (from a callback, or the reserve is full), `BAD_SCHEMA`, `OOM` |
| a refused schema text or wire | `BAD_SCHEMA` with `.detail` saying why and where, from `rant_node_schema` |
| a node this one cannot speak with | `WIRE_VERSION`: a node of the domain on another Rant wire version, at `.ip` and `.port`, once per source |

Socket faults carry the OS errno in `.os_error`. Topic scoped ones carry the name in
`.topic_name`. `rant_last_error(node)` returns the last error by value, with its strings
copied into the node so the event stays printable after the topic or peer is gone. Before
any error it is a `RANT_ERROR` event whose `.error` is `RANT_E_NONE` and prints "no
error". Every create that returns `NULL` records why there first. Pass `NULL` for the
process global slot that records why `rant_node_open` failed. `RantResult` (the negative
`RANT_ERR_*` returned by `rant_topic_send` and friends) is separate. Error reporting
costs nothing on the data path, and `RANT_NO_DIAG` strips the text.

## Built in log, status and meta

A log line says something happened. A status condition says something holds until it
clears, such as an e-stop pressed or a drive fault. Both carry a level and an origin:

- `RantLevel` rises: `RANT_LEVEL_DEBUG`, `RANT_LEVEL_INFO`, `RANT_LEVEL_WARN`,
  `RANT_LEVEL_ERROR`.
- `RantOrigin` is `RANT_ORIGIN_APP` for what the app wrote and `RANT_ORIGIN_RANT` for
  Rant's own.

### Log

Every node hosts one shared reliable topic, `@rant/log`, carrying
`RantLog { level: RantLevel, origin: RantOrigin, text: string }`.

- `rant_node_log(n, level, fmt, ...)` publishes a line, also from inside callbacks.
  `rant_node_log_text` takes formatted text. Text past `RANT_LOG_MAX` (512) bytes is cut.
- A line below `opts.log_level` sends nothing and returns `RANT_OK`. 0 takes
  `RANT_LOG_LEVEL` (`debug`, `info`, `warn` or `error`, any case), else Info. Any other
  value refuses the open with `RANT_E_BAD_LOG_LEVEL`.
- Rant mirrors its `RANT_ERROR` events as Error lines unless `opts.disable_error_logs` is
  set. A mirrored line carries the time it fired as `capture_us`. Read a line's time as
  `capture_us`, else `written_us`.
- To read other nodes' lines, take your own handle with `rant_node_log_topic`, widen its
  role to `RANT_PUBSUB`, and lines reach `on_message`, inline or on the queue given to
  `rant_node_set_log_queue`. A node never receives its own lines. A late subscriber gets
  each node's last 4. A slow subscriber never blocks the app, it only loses the oldest.
- `opts.disable_logs` strips the topic.

```c
rant_node_log(n, RANT_LEVEL_WARN, "battery at %d%%", pct);
```

### Status

A node's status is the set of conditions active on it now, published whole on
`@rant/status`.

- `rant_node_set_status(n, key, level, text, opts)` raises the condition `key`, or updates
  it while it is active. The same values again send nothing, so setting it every cycle is
  fine. `rant_node_clear_status(n, key)` clears it.
- The set publishes on the next poll pass, so a burst of changes is one message. The
  topic keeps the last set, so a late subscriber gets every node's current set at once.
- A key holds up to `RANT_STATUS_KEY_MAX` (48) bytes, a text `RANT_STATUS_TEXT_MAX` (160),
  and a node up to `RANT_STATUS_MAX` (32) conditions. A longer key or one too many is
  `RANT_ERR_TOO_BIG`, a longer text is cut.
- To read other nodes' sets, widen `rant_node_status_topic` to `RANT_PUBSUB`. Each set
  reaches `on_message` as `RantStatus { conditions: RantCondition[] }`, inline or on the
  queue given to `rant_node_set_status_queue`, and replaces that node's previous one. A
  node's conditions end with its `RANT_PEER_DOWN`.
- `RantCondition { key: string<48>, level: RantLevel, origin: RantOrigin,
  clearable: bool, since_us: u64, text: string<160> }`. `since_us` is the owner's UTC clock
  at the raise and stays through updates.
- `opts.disable_status` strips the topics.

A condition raised with `.clearable = 1` may be cleared from another node, such as an
operator resetting a latched fault from an HMI.

- `rant_node_clear_peer_status(n, peer, key, since_us)` asks `peer` to clear it. `since_us`
  is the one its last set reported, so a clear never lands on a later raise of the key.
  It is `RANT_ERR_ROLE` while this node does not read status.
- Nothing answers. The owner clears it, fires `RANT_STATUS_CLEARED` with `.peer` the asker
  and `.detail` the key, and publishes the smaller set. A clear of a condition that is
  missing, not clearable or raised again is dropped with a Debug line from Rant.

```c
rant_node_set_status(n, "drive2", RANT_LEVEL_ERROR, "drive 2 tripped on overcurrent",
                     &(RantStatusOpts){ .clearable = 1 });
/* in on_event */
if (ev->kind == RANT_STATUS_CLEARED && !strcmp(ev->detail, "drive2")) reset_drive(2);
```

Rant raises its own conditions, origin `RANT_ORIGIN_RANT`, never clearable. Each holds
while its cause does and clears itself, and its text names the peer, topic or address.

- A key is `kind/` and 8 hex digits for one problem, or the kind alone for the kind as a
  whole. The kinds are the table's. Split a Rant key at its first `/` to get its kind. App
  keys have no such form.
- Up to `RANT_STATUS_KIND_MAX` (3) problems of a kind are listed one by one. More fold into
  one condition under the kind alone, which counts them and quotes the first, such as
  `wire_version`: "500 nodes speak another wire version. First: 10.0.0.5:6733 ...".

| Kind | Level | Holds while |
|---|---|---|
| `wire_version` | Error | a node at that address speaks another wire version |
| `peer_refused` | Error | a peer that meets this node finds every slot taken |
| `peer_too_big` | Error | a peer's announce or topic map is too big for this node |
| `directory_partial` | Warn | this node hears more peers than it remembers, so it forgets some (docs/discovery.md) |
| `schema_mismatch` | Error | a match is refused because the schemas do not fit |
| `qos_incompatible` | Error | a reliable subscriber here meets a best effort publisher |
| `kind_mismatch` | Error | the name is another kind of entity at a peer |
| `bridge` | Error | the link to `net.bridge` is down |
| `announce_overflow` | Error | this node's topic list does not fit its announce |
| `duplicate_authority` | Warn | another node also provides or owns this entity |
| `forced` | Warn | a variable this node owns is forced |
| `more_problems` | Error | more than the `RANT_STATUS_RANT_MAX` (16) slots hold |

A source that is not a peer (`wire_version`, `peer_refused`) clears 3 announce intervals
after it goes quiet, and `directory_partial` 3 intervals after the last peer forgotten. Rant tracks `RANT_DISCOVERY_TROUBLE_MAX` (16) such sources and counts
any more as "16 or more", so a big mesh costs no memory past them. Each tracked source
fires its `RANT_ERROR` event once, and the rest once together. Rant's conditions have their
own cap, so they never take the app's `RANT_STATUS_MAX`. All three caps are build options:
a small target such as an ESP32 can define them lower.

### Meta

Every node also hosts the `@rant/meta` function, which answers a snapshot of its peers,
topics and counters for tools like the explorer. The peers section lists the matched peers,
so a small node's answer stays small on a big mesh. The node section counts them as
`matched_peers` against `max_peers`, and `peers` counts every active peer heard. Its
`mem_in_use`, `mem_peak` and `alloc_calls` count the main pool, and a node with `opts.bulk`
adds `bulk_in_use`, `bulk_peak` and `bulk_alloc_calls`.
`rant_node_meta_function(n)` returns the handle to call it. The first call of it opens this node's asking side, which matches every
node on the mesh, so a node that never asks matches nobody through meta.
`opts.disable_meta` strips it.
`rant_topic_counts` reads the per topic message and byte counters directly. The reflection
walks list the builtins like any entity, under their `@rant/` names.
