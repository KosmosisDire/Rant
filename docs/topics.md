# Topics

A topic is created at runtime and returned as an opaque `RantTopic *`. Its identity across
peers is its name, so use the same name on every node. Locally a topic is its index in
creation order, which is `RantMsg.topic_index` on delivery. Peers match on the name no
matter how each ordered its topics. `opts.max_topics` (default 8) bounds how many you can
create.

```c
RantTopic *t = rant_node_create_topic(n, "pose", RANT_PUBSUB, schema,
                   &(RantTopicOpts){ .qos = { .reliability = RANT_RELIABLE, .keep_last = 16 } });
```

Roles (`RantRole`) are `RANT_PUBSUB`, `RANT_PUB_ONLY`, `RANT_SUB_ONLY` and
`RANT_INACTIVE` (declared but off). `rant_topic_set_role` flips the role at runtime and re advertises at once. Peers
rematch with no round trip.

The schema is optional. With one, a peer whose schema cannot read yours is refused with
`RANT_E_SCHEMA_MISMATCH`. docs/stdtypes.md covers the schema DSL and the standard types.

## QoS

| field | meaning |
|---|---|
| `reliability` | `RANT_BEST_EFFORT` or `RANT_RELIABLE` |
| `keep_last` | messages kept for late join and repair. 0 = 1, or 10 on a reliable topic |
| `catch_up` | messages a new subscriber gets at once. 0 = future only, 1 = latest value |
| `max_message_bytes` | a size hint that pins the shared memory class. Buffers grow to fit anyway |
| `heartbeat_us` | reliable idle publisher ping. 0 = 250 ms |
| `repair_delay_us` | how long a subscriber waits before asking again for a resend. 0 = measured round trip |
| `backpressure_wait_us` | how long a send pauses for a slow subscriber before evicting unacked history. 0 = none |
| `shm_max_bytes` | pin the topic to one shared memory size class |
| `queue_bytes` | consumer queue cap. Setting it makes the topic queued from creation |
| `max_rate_hz` | subscriber side, best effort only. Cap delivery from each publisher to this rate |
| `no_timestamp` | publisher side. Send without the `written_us` stamp |

Matching follows the RxO rule: the offered reliability must be at least the requested
one. A reliable publisher serves a best effort subscriber, and that reader is fire and
forget: no backpressure and no heartbeats, so it can never stall the writer. A best effort
publisher does not serve a reliable subscriber. That match is refused with
`RANT_E_QOS_INCOMPATIBLE` and forms on its own if the publisher later upgrades.

A subscriber that joins late, or re subscribes, gets `catch_up` cached messages replayed.
Keep it small, since a deep catch up bursts at startup.

## Sending

`rant_topic_send(t, rant_bytes(data, len))` commits the message to history and sends it
point to point to every matched subscriber. Data is always unicast. Only discovery uses
multicast. On a topic with a schema the bytes must be a message of that schema, else the
send returns `RANT_ERR_SCHEMA` and commits nothing.

A message larger than the fragment size is split into fragments. To a same host
subscriber it goes through shared memory instead, unless `opts.disable_shm` is set or the
build has `RANT_NO_SHM`. A message that fits one datagram always goes inline.

A send that would overwrite unacked reliable history blocks, bounded by
`qos.backpressure_wait_us`. See docs/node.md.

## The first send

Matching a new topic to a peer that is already present takes one announce round trip. A
send inside that window would reach nobody. So a send that would reach zero subscribers
while a match is still resolving blocks, bounded by `opts.match_wait_ms` (0 = 1 s,
negative = off), until the match forms or matching settles. The wait only covers the
topic's own start: it ends `match_wait_ms` after the topic was created, re roled or re
typed. A subscriber that appears later never blocks a send. A topic that keeps history
(reliable with `catch_up` above 0) is exempt, since replay covers it. A topic nobody
advertises interest in proceeds at once. A wait that cannot happen or that times out
proceeds and fires `RANT_E_UNMATCHED_SEND`.

For a GUI, disable the wait and check `rant_topic_ready(t)` instead. It uses the same
predicate. `rant_topic_pending_count` is the raw count of unresolved candidates.
`rant_node_settle(n, timeout_ms)` waits for the whole node: open, create topics, settle,
publish.

Best effort publishing is improved by the wait but not guaranteed. Best effort stays lossy.

## Retire and re create

`rant_topic_retire(t)` says this node is done with this incarnation of the name. The
topic leaves the announce, lanes tear down, history and the queue are freed, and the
handle is freed. It is invalid after, and the call is refused from callbacks, for
builtins, for pattern channels and mid dispatch.

The slot is parked and the next create reuses it, so retire and create churn never grows
the topic table or the announce. Re creating with the same name, kind and schema relinks
silently. Re creating with a different schema, kind or name makes peers re verify before
any data crosses. This is the retype flow: retire, then create with the new schema.
Creating a second topic with the same name while the first lives is refused, whatever
its schema: the create returns `NULL` and `rant_last_error` says `RANT_E_NAME_COLLISION`.
A twin created `RANT_INACTIVE` is allowed, so a node can hold two QoS variants of one name
and switch which is live by role.

## Loss

A best effort gap, or a reliable reader that fell so far behind that the writer overwrote
what it needed, fires one `RANT_MSG_LOST` with the first seqno and the count.
spec/transport.md explains how a lapped reader rejoins.

## Counters

`rant_topic_counts` returns messages and bytes sent and delivered on a topic. The same
numbers are in the `@rant/meta` snapshot.
