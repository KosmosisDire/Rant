# Node

The node owns the sockets and the clock, drives discovery, and turns discovery into
transport lifecycle. The user facing API is in docs/node.md.

## Core and runtime

`node/core.c` is sans-IO: the peer table (peer id to physical address) and the discovery
to transport lifecycle. `node/runtime.c` owns the sockets, the clock, the route probe,
the send and receive paths and the public `rant_node_*` API. The runtime resolves the
transport core's abstract destination (a peer id) to a wire address. A second transport
(serial, Bluetooth) is a new runtime over the same cores. See spec/transports.md.

Three datagram families arrive on the data socket: the transport's, `uDSC` (discovery)
and `uDTL` (details and interest pages). Every data datagram pays two linear peer scans
(`id_for_addr` then the peer slot), the first thing to index if peer counts grow.

Receive is drained until the socket is empty or `RANT_RX_BUDGET_US` (5 ms) elapses, then
discovery and send get their turn, so a slow `on_message` never starves them. A datagram
the socket refuses is held in `tx_hold` and retried first next tick, never dropped. The
data socket is bound before discovery opens so the announce carries the real port, with no
address reuse, so a port collision fails loudly at open.

## Delivery

Every delivery path (inline UDP, shared memory, parked redelivery) runs one sequence:
strip the source stamp, split the pattern header (`prefix_bytes`, plus a `[u8 len][bytes]`
message on a response channel), validate the payload length against the publisher's
schema, then queue or call back. A zero length payload on a pattern channel is an op only
message and skips the schema, as does a task request whose op byte is not CALL. A message
that fails validation is dropped and surfaced, never handed to the app.

## Growth

In dynamic mode a refused peer and an oversized peer blob are growth signals, not
verdicts: the peer table or the accept bound grows at the next poll, between ticks, the
peer is admitted on its next announce (a solicit asks for it), and the app never sees the
refusal. Static mode surfaces both. A size that failed to allocate is remembered so the
retry surfaces instead of spinning. See spec/allocation.md for the grow itself.

## Settle

`rant_node_settle` solicits, re sent four times a second, and returns once every active
peer has been heard since the solicit, the topology has been quiet for one window (the
announce interval capped at 300 ms), and one window has passed overall. With no peer heard
at all only a full announce interval can rule out a slow one. The post open gather latch
uses the same predicate anchored at open, so a first send's match wait knows the announce
cache covers the peers that were already there.

## Threading

One node level mutex at the API boundary, never held across a blocking wait. The sans-IO
cores stay lock free. Per topic lock sharding is the future path and this design is a
strict prefix of it.

- `i_rant_node_poll_locked(n, timeout, outer)` is the one poll body. The public poll, the
  service loop and the non started backpressure pump all run it. `outer = 1` drops the
  lock around the platform poll over the data, waker and discovery descriptors. The wait
  is capped by the transport's next deadline and discovery's next announce, so an idle
  service thread wakes about once a second, and the service loop is capped at 250 ms so a
  lost wakeup is bounded.
- The waker is a self connected loopback UDP socket, the one self pipe pollable on both
  Windows and POSIX. Mutating entry points kick it (coalesced) so the change is serviced
  now. A waker open failure degrades the open (kicks no op, next tick latency).
- A send transmits on the caller's thread. `i_rant_node_send_tx` runs the one TX drain
  (`i_rant_node_tx_drain`, shared with the poll body and `i_rant_node_flush_tx`) under the
  lock the send already holds whenever another thread runs the loop (a service thread or
  a sleeping poller), so there is no handoff, no waker datagram and no wait for a TX
  pass. A full socket parks the datagram in `tx_hold` and kicks the poller to retry. A
  send from a callback leaves the drain to the pass that called it. A single threaded
  program keeps the batch at its poll, where one datagram carries many submessages. The
  price of the inline drain is that batching: a tight loop of small sends tops out near
  450k a second where the handoff reached 750k with a deep history.
  `pollers_sleeping` is a counter, since a flag lost kicks with several pollers.
- Reentrancy is an owner thread id check, zeroed before release. From callbacks, send and
  read only queries are legal. Poll, create topic, set role, drain, start, stop and close
  are refused with `RANT_ERR_STATE`: create would relocate the arena mid receive, set role
  would replay interest into the proxy mid delivery, stop would self join. A callback that
  sends to another node deadlocks (plain lock acquisition both ways). Forbidden in docs,
  not enforced.
- Backpressure: senders sleep on the node condvar, re stamping the lock owner after
  reacquire, and the work pass tail broadcasts when waiters exist. Two predicates: unacked
  reliable history (bounded by `qos.backpressure_wait_us`) and unsent history
  (`rant_transport_send_would_evict_unsent`, bounded by `RANT_UNSENT_WAIT_US`). The
  unsent wait breaks early after one completed work pass only when a datagram is held in
  `tx_hold` (socket bound, so evict). With no hold another sender refilled the ring, so it
  re arms and keeps waiting. `RANT_E_EVICTED_UNSENT` fires only after the send commits.
- The blocking waits (match wait, send backpressure, topic drain, node settle, queue
  wait) share one `i_rant_node_wait_until` skeleton: predicate, periodic hook, outer flag.
  Invariants it carries: waiter accounting, re derive arena pointers after any wait, exit
  when the service thread stops, kick before sleep. The patterns layer's blocking call and
  variable wait ride it through `i_rant_node_sys_wait`, so they sleep on the service
  thread's progress when one runs and pump the loop otherwise.
- `rant_node_stop` elects one joiner, broadcasts, kicks, unlocks, joins without the lock,
  then clears the running flag and broadcasts a second time for waiters that re slept.
- `RANT_NO_THREADS` keeps the single threaded contract and `rant_node_start` returns
  `RANT_ERR_NOSYS`. Guards are `#ifdef RANT_THREADS`.

## Callback queues

`RantQueue` is a drain group, not storage. `rant_queue_dispatch` scans the handles created
with it (`RantTopic.queue`) and pops the record with the oldest arrival stamp across their
rings, stopping at the stamp taken on entry so arrivals during the dispatch wait for the
next call. Each pop views the record (`viewing`), sets the ring busy so a retire of that
handle is refused meanwhile, releases the node lock around the callback when this thread
owns it, and releases the view after. `RantQueue.busy` refuses a concurrent or nested
dispatch, and `RantNode.dispatching` counts busy queues and refuses close. A topic created
with a queue allocates its ring at creation and the create fails with `OOM` when it cannot.
A queue of another node fails the create with `STATE`. Queues are pool allocations freed by
the reset at close, at most `RANT_QUEUES_MAX`. The rings, caps and park rules are the
consumer queue's below.

Events park on one node ring (`RantNode.event_q`, allocated by `rant_node_set_event_queue`,
`silent` so an eviction counts without emitting) as `I_RANT_REC_EVENT` records: the
`RantEvent` copy, then `topic_name`, `peer_name` and `schema_detail` as length, presence and
NUL terminated bytes, since all three are views that die with a retire or a peer drop. The
patterns layer's `sys_on_event` still runs inline at emit, state applies at receipt. The
group walk treats the ring as a handle less member of the event queue.

## Consumer queues

A queued topic's messages are copied by the poll thread into a per topic byte ring
(`i_RantMsgQueue`: a record header holding its size, the payload length, the two stamps and
the publisher id and name length, then the copied sender name, then an 8 aligned payload.
Records never wrap). The ring starts small and grows to `qos.queue_bytes`. An explicit
value pre allocates the ring in full. One bigger message still fits.

`take` returns a view valid until the topic's next take or dispatch. The viewed record
pins the ring tail, so grow and eviction skip while viewing, and a held view degrades
best effort to drop newest until the next take. `dispatch` runs `on_message` with the
node lock released and a busy flag refusing nested take or dispatch on the same topic.
`RantMsg.schema` is re resolved at take, since the delivery map can repoint. The sender
name is copied into the record because discovery views die with the peer. One consumer
thread per topic is documented, not enforced.

At the cap the policy is the reliability QoS. Best effort overwrites the oldest and fires
`RANT_MSG_LOST`. Reliable PARKS delivery in the transport reader: `on_message` returns
nonzero, no `deliver_upto` advance, no ack, no repair traffic, the message held in the
assembly buffer (the SHM variant parks the descriptor). Incoming DATA of the next sample
fills the one ahead hold, so an unpark delivers both with no resend. The writer's
`acked_upto` stalls, its history fills, and the publisher's send blocks on normal flow
control. Draining calls `rant_transport_deliver_parked` and kicks the waker. A writer HB
floor past the held sample gives up with one `MSG_LOST`, the bounded loss escape. Any new
sans-IO consumer of `RantMessageFn` must return 0.

Size the queue at least `keep_last` times the message size, or the reader parks while
the writer keeps bursting and heals only through the paced repair path (16.4 to 3.7 GB/s
measured with a 4 MB queue against a 16 MB burst). A zero copy take (refcounted hold, ack
on release) is the known escape for huge payloads, not built.

## Timestamps

`written_us` is 8 little endian bytes the writer prepends inside the sample ahead of any
pattern header, stamped in `i_rant_writer_store` from the `RantConfig.source_time` hook
(the node wires it to `i_rant_plat_wall_us`). A NULL hook still writes 8 zero bytes:
framing is driven by the QoS alone, never by hook presence. The opt out
(`qos.no_timestamp`) rides the attrs byte of the DETAIL_RESP (`RANT_ATTR_NO_TIMESTAMP`,
see spec/interest.md), and the detail intake re applies the cached interest blob in the
same call that forms proxies, so the receiver always knows before it can deliver. The
node strips the stamp at one point before the pattern prefix split (the queue path strips
at enqueue). `recv_us` is the node's monotonic clock at the moment the poll received the
message, or at enqueue for a queued topic.

`capture_us` is a second 8 byte slot right after `written_us`, present only when the
sender passed one. A microsecond wall clock needs 51 bits, so bit 63 of `written_us`
carries the marker that announces it, and the strip masks that bit off before the value
reaches `RantMsg`. The marker lives inside the sample rather than in the DATA submessage
flags on purpose: repair, catch up replay, the SHM chunk and the consumer queue then
carry it with no extra plumbing, exactly as the source stamp already does, and neither
delivery callback grows an argument. A sample whose marker is set but which is too short
to hold the slot is malformed and is passed through whole rather than read past.
A `no_timestamp` topic frames neither slot, so a capture time cannot ride it.

## Errors

One event kind, `RANT_ERROR`, with a `RantErrorKind` code. The transport keeps its own
typed event kinds (sans-IO) and discovery keeps a last error slot, so the node maps every
lower layer event into `RANT_ERROR` in one place per layer. Text is built on demand by
`rant_event_str`, so the data path never touches it. `rant_last_error(n)` keeps the last
error per node, and a process global slot keeps the failure of a `rant_node_open` that
returned no handle. That failure also fires the `on_event` passed to open.

`RantEvent.schema_detail` says exactly what was incompatible on a schema mismatch,
recorded per (peer, topic, direction) at detail intake. `RantEvent.peer_name` is resolved
once at the single emit point so `rant_event_str` never prints a bare id. `RantEvent` is
returned by value, so every binding mirror must match it exactly (spec/bindings.md).
Deferred on purpose: `RANT_E_SHM` (needs once only dedup) and `RANT_E_BAD_PACKET` (risks a
flood from hostile traffic). Internal errors are mirrored to `@rant/log/error` through a
fixed ring of 8 records (events fire mid receive where re entering the transport is
unsafe, so the poll pass flushes them), duplicates coalesce into "(xN)", overflow becomes
one summary line. The ring is allocated on the first error, so a healthy node never pays
for it. `RANT_E_SEND` carries the datagram size and the first submessage's topic, so a
starved link (an ESP32 out of WiFi buffers reports ENOMEM) says which topic and how big.

## Built ins

The builtins ride outside `opts.max_topics` in a block at the top of the reserve, so user
topics keep dense 0 based indices. A `creating_builtin` flag routes open time allocation
into the block and user creates step over it after a grow. Every handle iteration goes
through `i_rant_node_topic_hi`.

Builtins are hidden from every high level surface. Both entity walks skip them, the meta
snapshot reports app topics only, and a leading `@` is refused in every public constructor
so an app entity can never land in the hidden namespace. The raw interest walk still
yields them. `rant_node_log_topic` and `rant_node_meta_function` are the way in.

The three `@rant/log/{error,warn,info}` topics are reliable with `keep_last` = `catch_up` =
16 (8 for info) and no backpressure wait, so a slow subscriber only loses old lines.
Catch up replay is per writer lane, so a late subscriber gets each node's last lines per
level. The record schema is `RantLog { wall_us: u64, mono_us: u64, text: string }`, and the
text is capped at `RANT_LOG_MAX` (512) bytes. A node never delivers to itself, so
consuming a level means reading every other node's lines.
`set_role` queues a log builtin before its subscribe side goes live, or its catch up
replay could land in the inline callback path before the first take creates the queue.

`@rant/meta` is one handle carrying both sides: PUBSUB channels, a handler plus a pending
list, directed requests so a call aimed at one peer never wakes the rest, keep_last 2
rings, created with `.multi`. The request is an optional 4 byte section mask (NODE, PROC,
TOPICS, PEERS, empty = all). The response is `RantMeta { info: map }` with node, proc,
topics and peers sections. The snapshot builder is node owned: the buffer starts at 1 KB and doubles on
overflow up to 4 MB. The per topic `pending` count is one bulk walk per peer, not a walk per topic. No
self inspection, since a node never matches itself. An ordinary remote must never
advertise the provider side (it would auto answer NO_HANDLER and race the real provider).

Per node cost at rest is about 7 to 8 kB. Meta adds 4 to 8 kB only once queried.

## Interfaces

The runtime enumerates interfaces with `i_rant_plat_local_ifaces`, joins the group on
each, and feeds its subnets to discovery for locator ranking. It re enumerates every 3 s
in auto mode. Discovery's tick runs after the main wait off its revents, so a poll pass
costs one poll syscall. See spec/discovery.md.
