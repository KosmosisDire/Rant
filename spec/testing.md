# Testing and measuring

`tests/rant_test.c` holds the selftest phases, the sweep harness and the benches. The
commands are in docs/building.md. This file holds what makes a test or a measurement
trustworthy.

## Harness rules

- `ST_CHECK` evaluates its condition once now, but never put a side effecting or draining
  call inside a check macro.
- Launch paired processes in one shell command. The gap between two tool calls fakes a
  peer drop.
- Both `rant_test selftest` and `rant_test_noshm selftest` must pass, plus a sweep for
  transport changes. A comment only change must leave code byte identical.
- The selftests use `"127.0.0.1"` as the discovery interface to stay off the LAN, and a
  per process domain base so concurrent runs and the SHM module test (segment names carry
  it) do not collide.
- A raw transport `on_message` sees the 8 byte source stamp first. A payload tag sits at
  `data[RANT_TIMESTAMP_BYTES]`.
- A wall clock threshold check (waker under 400 ms, varwait under 250 ms) can flake once
  under load. Suspect those before the code.
- A nested wait only pumps the calling node, so a peer in the same process must answer
  from a service thread.
- Default QoS (shallow ring, no backpressure wait) loses bursts legally. Lossless
  multithreaded tests set `keep_last` and `backpressure_wait_us`. A burst that fits one
  datagram parks in `tx_hold` and counts as sent, so a hostile eviction test needs bursts
  bigger than one datagram plus the ring.

## Selftest phases

What each phase of `rant_test selftest` pins. The phase names match the comments in
`tests/rant_test.c`. Every phase runs on loopback on its own domain, offset from a per
process base picked from the clock so concurrent runs never join each other.

### Transport core, sans IO with a controlled clock

- Rate throttle: a subscriber advertising `max_rate_hz` paces the writer's best effort
  lane, decimating to the newest sample each tick. The per peer seqno keeps loss honest:
  a paced skip is not loss, a dropped sent sample is.
- Lapped reader and NACK merge: a six fragment reliable stream through a dropping pump.
  The writer merges a repair request into the one it still holds, so two ACKNACKs landing
  in one pass are both served. A reader the writer's floor passes while it is still
  fetching restarts at the oldest cached sample once, and on the next such skip with
  nothing delivered rejoins at the writer's head and reports the cached window lost.
  Without the second rule a stream whose repair takes longer than one inter message
  period starves the reader forever.
- Per peer RTT: a symmetric one way delay makes every round trip exact. The writer
  samples push to ack, the reader samples request to resend, both feed the per peer
  estimator, the reader's re ask backstop is the 50 ms default until the first sample and
  the RTT bound after, and the tail heartbeat follows the same bound. A re asked seqno is
  ambiguous and never a sample.
- Held sample: the reader keeps one sample beyond the head while the head repairs, so a
  late resend no longer discards the message that arrived meanwhile. The held sample
  delivers right behind the repaired head in order, a second future sample is still
  dropped, the hold fills while the consumer has the head parked, a floor landing on the
  held sample delivers it, and a hold across a wholly lost sample waits its turn.
- QoS: a reliable subscriber refuses a best effort publisher and every other direction
  matches. A best effort reader matched to a reliable writer never counts toward
  backpressure, since it never acks.
- Send result codes: the size check precedes the role check, so an oversize send on a pub
  topic is TOO_BIG even with no subscriber and a valid send on a sub only topic is ROLE.
  An init with no allocator refuses.
- Shared memory: a mock resolver whose success is controllable and a pump that drops SHM
  descriptors. A failed resolve does not ack so it repairs, a dropped descriptor resends,
  and an unresolvable descriptor is skipped after the retry cap as MSG_LOST. A sample
  under one fragment ships inline, larger ones take the shared memory path.

### Discovery core

- Lifecycle: two announces are two ups, silence past the timeout drops both but keeps
  their slots, the same uuid resumes under the same local id, and a new uuid from a held
  address evicts the predecessor as GONE because one socket is one process.
- Relay: who enlists us, one hop only, and a proxied locator losing to a direct path we
  still hear. PROXIED is the loop stop and defeats enlistment even with RELAY_ME set.
  RELAY_ME rides along as origin information, so a stated locator is no endpoint identity.
- Self ip: a stated locator rides our own announce and a relay propagates the stated
  address, not the source it saw, or relaying would undo the override for third parties.
- Observed sources: a unicast only peer behind a NAT advertises a fiction. Its RELAY_ME
  announces carry the real return path, so the receiver binds one observed source per
  local channel. Data goes there and never to the phantom, attribution refuses the
  phantom, the discovery channel binds independently, and a quiet rebind after the NAT
  expired sends at once with no peer up refire.
- Ghost gate: relaying is sustained by direct liveness only. Proxies still refresh plain
  liveness, so a dead origin outlives by one extra timeout and then dies, but proxies
  never keep us relaying it, or a dead origin would be re announced forever mesh wide.
- Unicast only receiver: a published port forward rewrites the source to the gateway on
  our own subnet, which would win the locator ranking and black hole every send. So a
  source never overrides a held locator there and binds as the observed return path
  instead, for every direct peer.
- Duplicate suppression: two or more foreign proxies for an origin within one announce
  interval suppress our own emission, our own looped echo (told apart by the relayer uuid
  trailer) never counts, and a changed origin re arms emission at once.

### Node core and runtime

- The node core drives its peer table over a transport and a real discovery core with no
  sockets, clock or platform. A nameless announce becomes "unknown-peer". A third peer is
  refused while the table is full of active peers and the node forwards the event.
- Open failure paths: a forced failure at each staged cleanup label returns NULL and
  leaves the platform balanced. A non multicast group fails the join and
  `rant_last_error(NULL)` names MCAST_JOIN. An occupied data port collides, since the
  unicast bind takes no reuse, and names RANT_E_BIND with the port and errno. An over
  long topic name is refused at create and the node stays usable.
- User data forwarding: a shared memory capable node rewraps the transport callbacks, and a
  transport fired event must still reach `on_event` with the app's user data. A crafted
  name collision is the deterministic transport event that proves it.
- Dynamic growth: creating topics past the reserve relocates the node into a bigger arena.
  A stream on the original handle loses, duplicates and reorders nothing across it.

### Schema and detail exchange

- Variable fields ride the tail as length framed sections in schema order and fixed
  offsets never move. A named integer is a fixed field carrying its backing scalar, the
  option table is schema only so an unknown value stays readable, and subset compares the
  backing width only.
- Announce interest is one positional hash and flags entry per topic slot with no names or
  schemas. An inactive topic is not yielded but holds its position, so a later role flip
  advertises the same index. A typed match forming at all proves the schema travels by
  the detail exchange.
- Subset binding: a subscriber declaring a subset of the publisher's schema by name in
  any order matches and receives a rebased schema. A same field with a different kind is
  refused on both sides with RANT_SCHEMA_MISMATCH and a wrong size message is dropped.
- A 1024 option u16 enum round trips, and identical lists on both nodes send no schema
  wire. A differing large enum would inline about 11 KB and IP fragment, which is left for
  a within entry paging pass.
- Primitive rooted schemas: one bare type is a schema, anonymous and addressed by the
  empty path, byte identical in every language with pinned canonical hashes. A bool writer
  against a u8 reader is refused, as is a struct against a bare type. A named type in root
  position stays refused because the name may only ride the header.
- Schema wire: named types and the narrow only matching they buy, alias roots, struct
  element arrays with indexed paths, variable members at any struct depth, and the
  refusals that keep an array element's stride static. Print hoists each named type as a
  leading definition, dependencies first, and recompiles to the same wire.
- Standard types: golden bytes and a golden hash every wrapper pins, and recognition that
  verifies the shape as well as the name. Change the golden pair only with a deliberate
  wire bump.
- Detail codec: request build and header accessors, the stateless responder (advertised
  indices answered, inactive and unknown skipped), schema wire inlined only on a hash
  mismatch, entry boundary truncation, and wholesale rejection of malformed input.
- Detail paging: a response fits one datagram and the requester pages the rest, because a
  peer whose receive buffer cannot reassemble an IP fragmented reply drops the whole
  thing. A lone entry whose schema wire alone exceeds a datagram is still emitted on its
  own page, never an endless header only reply.
- Live routing: a detail request at a node's data socket is answered to the request's
  source even from a bare socket, which the explorer relies on. A sub only topic's schema
  is fetched exactly like a publisher's. Observing before subscribing re pends the
  dissolved indices when the topic appears. Duplicate requests are idempotent and wrong
  domain or garbage datagrams are ignored.
- External interest: a publisher with about 300 topics cannot inline its interest, so the
  announce ships a sub MTU bootstrap and the subscriber pulls the blob by byte range
  paging. No datagram ever exceeds RANT_DGRAM_MAX, the match forms through the fetch, and
  steady state announces trigger no re fetch. Reflection reads the assembled interest and
  the interest epoch is the observer cache key.

### Threads and queues

- RELIABLE: three sender threads of 1000 reliable messages each with no poll anywhere,
  exactly once, per thread ordered, no unsent eviction, drain completes.
- BURST: 64 back to back best effort sends from one thread all reach the wire.
- HOSTILE (Windows): transport sends forced to would block. The first pass parks one
  datagram in `tx_hold`, past that the burst overwrites truly unsent history, which must
  surface as RANT_EVICTED_UNSENT so every send is delivered or accounted.
- WAKER: an idle started pair delivers a single send within milliseconds, not at the next
  announce capped wakeup.
- REENTRANT: a callback echo works, and create, set_role and a foreign poll refuse loudly.
  The ring must be deeper than the request burst, since a reentrant send never waits for
  a TX pass.
- STOP UNDER LOAD: hammer threads parked in the backpressure wait while stop broadcasts
  them loose, repeated under a watchdog.
- Callback queues: two topics on one queue and one inline. The inline one fires in the
  poll, a capped dispatch runs the oldest two across topics in arrival order, dispatch from
  an inline callback, a nested dispatch, a retire of the running handle and a close from a
  callback are refused, a sibling retire from a callback works, a retire from outside drops
  the unrun records, another node's queue and the ninth queue are refused, and a timed
  dispatch under service threads wakes on arrival on the caller's thread.
- Callback queues on patterns: a provider and a caller with one queue each. The request
  parks at the provider and its handler's own retire is refused, the reply parks at the
  caller, a blocking call completes inline through a queued channel, a NO_PROVIDER outcome
  parks, a task's RUNNING, progress, cancel notification and terminal outcome each park
  with the cancel flag set at receipt, a variable's replay parks, the owner applies a
  remote write at receipt and notifies at dispatch, the remote caches before its own
  dispatch, and a retire settles a parked reply CANCELLED and drops the records.
- Consumer queues: the first take enables the queue and a timeout take drives the loop,
  a best effort queue overwrites oldest at the cap, a reliable queue parks by withholding
  acks so a slow take loop still receives everything in order, dispatch runs the callback
  on the calling thread, and the condvar take path works beside service threads.

### Patterns

- Functions: match, async call, an empty ack when the handler returns without replying,
  deferred completion, NO_HANDLER, a client synthesized TIMEOUT, a fail with an over long
  message that truncates and still carries data. A call before the match forms queues and
  flushes rather than timing out. Two callers answered in one provider tick each get
  their own reply. A burst past keep_last engages backpressure without the pattern layer
  holding the node lock. A deferred reply landing after a local timeout is dropped and
  the next call still works.
- Provider reply burst: an inline reply is a reentrant send that never waits, so the rsp
  ring is the only thing between a drained batch and lost replies, which are invisible at
  the provider. The caller needs the same depth in its req ring, and both lanes must be
  up before the first reply commits since rsp carries no catch_up.
- Variables: a typed remote force and unforce pass the schema gate through the empty
  payload exemption, on_change dedups and replays, on_write fires on every write, a
  reentrant set inside on_write commits before the outer publish so the owner skips the
  stale one and the remote's write_seq guard drops any stale value, and the ring is the
  repair window not the replay window so a burst from a callback must fit it.
- Tasks: defer with the RUNNING ack first, ordered progress, cancel honored both ways,
  no_cancel refused locally, the bare return AppError synthesis. The matrix adds
  concurrent caller demux by the caller's uuid low 32 since call ids are per caller
  counters, provider selection, a third party cancel crafted on the raw wire, retire mid
  run through both delivery paths with the caller side severed lane backstop, close mid
  run flushing CANCELLED, file transfer shaped reliable progress, peer loss answering
  PEER_LOST, and task reflection. The raw tap is created before the normal remote handle
  because the demux binds a name to the oldest local topic.
- Duplicate authority: detected off the announce interest and surfaced once per entity
  and peer on both rivals. Accessors and callers never fire it.
- Retire: a second same name handle on one node is shadowed while the first lives.
  Retire parks the predecessor so a re created handle receives and writes.
- Dropped peers: the entity walk refuses dropped peers unless `include_dropped` is set,
  or every observer would grow ghost entities after a restart.
- Variable set match wait: a fresh accessor's first write rides the send path's match
  wait, so NO_TOPIC means the owner is absent. Converged verdicts stay instant.
- Churn soak: retire and create cycles reuse slots so nothing grows over sustained churn,
  across identical re creation, incompatible and compatible retypes, untyped subscribers,
  rival same name publishers, late joiners and transient peers. The plateau is judged
  against a warmed baseline after the one time allocations.
- Match wait: a sample committed after our side matched but before the peer verified us
  heals through reliable repair once its verdict lands, so delivery is writer
  authoritative. A first send waits for an already present subscriber. A disabled wait
  drops at once but loudly with RANT_E_UNMATCHED_SEND.

### End to end

- JOIN: a reader joining mid stream adopts the head silently. GAP: a burst beyond
  keep_last without a flush arrives as exactly one gap. BLOCKED: a send that would evict
  unacked history waits the backpressure window and then proceeds. RELEASED: once the
  reader acks, sends are instant. SWEEP-ACK: a sub only reader's timer armed ACKNACK is
  flushed only by the periodic sweep, and a sweep that skipped topics with no sent seqno
  starved it. DYNAMIC: every re subscribe replays history with no gap. SCALE: 40 topics
  match at peer up.
- FLAP: a one sided flap where the reader rebuilt its state. The changed reader epoch in
  the first ACKNACK makes every writer lane re join and replay. RESUME: a discovery blip
  drops the peer on both sides without tearing transport state, dormant peers leave flow
  control, and a same incarnation resume keeps the deliver position.
- NAMED and COLLISION: the cross peer identity is the name hash, independent of local
  handles. Two names with the same 64 bit identity, found by Pollard's rho against FNV-1a,
  fire the collision event and never cross wire.
- Relay: a unicast only node seeded with one address reaches a third node through the
  relay's proxied announces and then the two sustain each other directly. The control
  with nobody to relay it is unreachable both ways.
- NAT: the unicast only node advertises a black holed port bound by the test and never
  read, so only observed sources carry the mesh, and the relay introduces its peers so the
  NAT'd node speaks first to each.
- Self ip: on one host the advertised port carries the proof. An unparseable locator is a
  config error, never a silent fallback.
- Write timestamps: a delivered stamp lies within the send and delivery walls, the opt
  out gives 0 with a byte identical payload, catch_up replay keeps the original stamp,
  and the queued path and the patterns layer carry the same value.
- Logs and meta: history logged before anyone listens replays to a late joiner, a
  mirrored internal error lands on the error level, and a directed meta call decodes.

### Benches

- sweep spawns node children per rate and aggregates their SUMMARY lines. The two machine
  form runs `serve` workers on a control domain: the CMD topic is reliable with catch_up
  0 so stale commands never replay, the RES topic keeps catch_up at depth so results
  survive a control peer flap, entries are tagged with domain and run nonce, workers
  HELLO at startup, and both machines must share a subnet.
- memscale: steady state must be alloc free and warm sends pay no allocation.
- threadbench: keep_last 64 so the drain guard rather than ack flow control engages, and
  backpressure only ever engages flat out.
- queuebench: inline callbacks against a 4 MB consumer queue. The queue must cover the
  writer's in flight burst or the reader parks and the overrun heals through repair,
  which is a sizing bug, not steady state cost.

## Binding tests

`bindings/cpp/test.cpp` runs two nodes in one process on loopback and exits 0 on pass. Legs 1 and
2 cover the dynamic schema API direct and over service threads, leg 3 the patterns layer
with the memcpy, loop and rebase codec paths plus entity reflection, leg 4 bare type roots
and the cross language hashes, leg 5 the standard types including the video family, leg 6
variable members as tail frames, and leg 7 tasks with defer, progress, cancel, no_cancel
and a retire mid run. Leg 8 runs only as `rant_cpp_test bench`: the cost of each codec path
(memcpy, loop, tails, dynamic) alone and end to end on loopback, next to a C node pair on
the same footing, so a wrapper change is measured against the C floor.
`bindings/csharp/test` and `bindings/python/test.py` mirror the same ground, and the
golden hash vectors are shared by every binding and pinned in C by the schema root phase.

## Measurement traps

- Never redirect a child's stdout through a PowerShell pipe. The pipe fills and an
  unbuffered printf in the child blocks for seconds. `rant_test sweep` spawns children
  itself and redirects to files.
- A load generator that repays its backlog invents drops: a stall becomes one burst from
  every node, receive buffers overflow, and the drop percentage reads as stall over
  duration. Check the stall and max gap columns before trusting a row.
- Silent kernel drops are the usual real cause. The default 64 KB receive buffer holds
  about 900 datagrams and Windows drops with no counter moving. Bigger buffers give
  bufferbloat, not a fix. Drain the receive socket to empty every poll: one datagram per
  poll let a churn flood back up and delayed peer detection by tens of seconds.
- Deep replay is by design, so replay receivers must be idempotent. The sweep control
  plane keys on a per run nonce.
- Windows charges a multicast sender about 65 us per local joiner and picks different
  egress interfaces per socket. That measurement is why data is unicast only.

## Measured envelope

The shape of the limits on one host (10 node full mesh, 32 core Windows 11), not current
figures. Per datagram cost dominates, so a 1024 B send costs the same as 70 B. Best effort
sustains 50 kHz per node at zero drops and 0.26 ms round trip. Reliable is clean through
45 kHz per node, and 50 kHz hits the repair ceiling (the 32 seqno NACK window). Mesh
datagrams grow as N squared times the rate. Idle topics cost nothing.

Threaded (`rant_test threadbench`): flat out 688k msg/s UDP and 452k SHM on loopback with
zero evictions. Queued delivery (`queuebench`) costs 14% at 1 KB up to 35% at 1 MB.
Reliable same host SHM at keep_last 4 went from 92 to 2300 msg/s with the immediate in
order ack. A zero subscriber send costs 52 ns. `rant_topic_match_count` costs 11 ns, so an
app that builds an expensive payload should gate on it.

## Live debugging

A `probe.c` that defines `RANT_IMPLEMENTATION` against `dist/rant.h` can read every
internal struct (discovery peer flags, observed sources) and wrap `sendto` and `recvfrom`
with macros to log all discovery and detail traffic. Interest lines stuck at a fixed
publish count while liveness continues mean the peer's detail egress is black holed. Do
not trust a peer down time at seconds granularity to tell a BYE from a timeout.

Repair diagnostics (`rant_repair_stats`): `frags_dup` should stay near 0. Rising
`nacks_sent` with a flat reader position is a wedge. `frags_ahead` tracking the live
fragment rate with `msgs_skipped` tracking the message rate was the lapped treadmill.
Reproduce with a pub sub pair on loopback, `disable_shm`, 200 KB at 30 Hz and a queued
subscriber whose consumer sleeps 3 s.

`pubsub pub --rate` prints an in pump repair series from a 200 ms probe that runs while a
send is blocked in backpressure, and `pubsub sub --rate` prints a 250 ms reader series.
Resent bursty then flat with idle polls means the reader stopped asking (self quench).
Resent steady while the reader receives nothing means the resends are dropped. On the
reader, `recv` counts accepted fragments and `old` and `ahead` count arrivals rejected by
seqno position, so a zero recv with high old or ahead means the flood lands on the wrong
position rather than failing to arrive. The publisher's pacing loop fires one send per
iteration and re reads the clock after it, makes up lag under 50 ms one send per spin and
resyncs past that, so a blocked stretch never bursts a backlog and the rate readout is
normalised by real elapsed time. An earlier loop that fired every due tick in a burst
made a true 6 msg/s read as 64/s.
