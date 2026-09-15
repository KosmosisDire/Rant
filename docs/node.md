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
`RANT_ERR_STATE` (or NULL or 0). Nothing is corrupted.

Flow control needs no poll cadence. A send that would overwrite unacked reliable history
sleeps, bounded by `qos.backpressure_wait_us`. A send that would overwrite history not yet
handed to the wire waits one TX pass, bounded by `RANT_UNSENT_WAIT_US`. If it still must
evict, KEEP_LAST proceeds and a `RANT_E_EVICTED_UNSENT` error fires.
`rant_node_evicted_unsent` counts them.

Reflection views (the names, addresses and schemas the `rant_node_*_next` walks return)
are bracketed with `rant_node_lock` and `rant_node_unlock` when a poller runs on another
thread.

## Consumer queues

By default callbacks run on whichever thread polls, so a heavy handler lags the whole
node. A topic becomes queued on its first `rant_topic_take` or `rant_topic_dispatch`, or
from creation with `qos.queue_bytes`. The poll thread then only copies its messages into a
per topic ring and one consumer thread of your choice drains it. Different topics can go
to different threads.

- `rant_topic_take` pops a zero copy view valid until that topic's next take or dispatch.
- `rant_topic_dispatch` runs `on_message` on the calling thread outside the node lock, so
  those callbacks may use the whole API.
- `rant_node_dispatch` drains every queued topic. Call it from a Unity `Update()` or a UI
  frame.
- `rant_topic_queue_stats` observes the queue.

The ring starts small and grows to `qos.queue_bytes` (0 = `RANT_QUEUE_CAP`, 1 MB). One
bigger message still fits. At the cap the reliability QoS decides: best effort overwrites
the oldest and fires `RANT_MSG_LOST`, reliable parks delivery so the publisher's send
blocks on normal flow control. Draining the queue unparks it. Size the queue to cover the
writer's burst (at least `keep_last` times the message size), or a parked reader heals
through the slow repair path.

Timeouts on take and dispatch: 0 checks, positive waits that long, negative waits forever.
They work under a service thread, with a manual poll cadence, single threaded and under
`RANT_NO_THREADS`. From inside a callback they cannot wait.

The queue's extra copy costs about 15% of peak goodput at small payloads and up to 35% at
1 MB. Measure with `rant_test queuebench`.

## Timestamps

Every `RantMsg` carries three clocks.

- `recv_us` is this node's monotonic clock when the poll received the message (at enqueue
  for a queued topic). Use it for rates and jitter.
- `written_us` is the writer's wall clock in UTC microseconds when it wrote the message.
  Repair, catch up replay, shared memory and the consumer queue all keep the original
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

The four other kinds are `RANT_PEER_UP`, `RANT_PEER_DOWN`, `RANT_PEER_INTEREST` (peer
lifecycle) and `RANT_MSG_LOST` (best effort loss, expected, not an error).

| group | codes |
|---|---|
| matching and config | `NAME_COLLISION`, `QOS_INCOMPATIBLE`, `SCHEMA_MISMATCH`, `KIND_MISMATCH`, `INTEREST_OVERFLOW`, `META_TRUNCATED_INTEREST`, `META_TRUNCATED_SCHEMA`, `PEER_META_TOO_BIG`, `MSG_TOO_BIG`, `PEER_REFUSED`, `EVICTED_UNSENT`, `UNMATCHED_SEND`, `DUPLICATE_AUTHORITY` |
| IO and setup | `OOM`, `PLATFORM`, `SOCKET`, `BIND`, `MCAST_JOIN`, `SEND`, `RECV`, `POLL`, `WAKER`, `BAD_ADDRESS` |
| a refused create | `NAME_COLLISION` with `.peer` 0 (a live same name topic on this node), `BAD_NAME`, `STATE` (from a callback, or the reserve is full), `BAD_SCHEMA`, `OOM` |

Socket faults carry the OS errno in `.os_error`. Topic scoped ones carry the name in
`.topic_name`. `rant_last_error(node)` returns the last error by value, with its strings
copied into the node so the event stays printable after the topic or peer is gone. Before
any error it is a `RANT_ERROR` event whose `.error` is `RANT_E_NONE` and prints "no
error". Every create that returns `NULL` records why there first. Pass `NULL` for the
process global slot that records why `rant_node_open` failed. `RantResult` (the negative
`RANT_ERR_*` returned by `rant_topic_send` and friends) is separate. Error reporting
costs nothing on the data path, and `RANT_NO_DIAG` strips the text.

## Built in logs and meta

Every node hosts three shared reliable log topics: `@rant/log/error`, `@rant/log/warn` and
`@rant/log/info`. `rant_node_log(n, level, fmt, ...)` publishes a line, also from inside
callbacks. To read other nodes' lines, take your own handle with `rant_node_log_topic`,
widen its role to `RANT_PUBSUB`, and read it like any topic. A node never receives its
own lines. A late subscriber gets each node's last lines per level. A slow subscriber never
blocks the app, it only loses the oldest lines. `opts.disable_logs` strips the topics.
Internal `RANT_ERROR` events are mirrored to the error level unless
`opts.disable_error_logs` is set.

Every node also hosts the `@rant/meta` function, which answers a snapshot of its peers,
topics and counters for tools like the explorer. `opts.disable_meta` strips it.
`rant_topic_counts` reads the per topic message and byte counters directly. Builtins are
hidden from every reflection walk.
