# Testing and measuring

`tests/` holds every test program and `rant_bench`, and `tests/CMakeLists.txt` registers
each program as one ctest entry. The commands are in docs/building.md. This file holds what
makes a test or a measurement trustworthy.

- `tests/unit/` runs the sans IO cores with no sockets and a controlled clock.
- `tests/e2e/` runs real nodes on loopback.
- `tests/support/` is the harness every program shares: `test.h` compiles the whole
  implementation into the program so a case reaches every internal, `harness.c` holds the
  case runner, the checks, the pumps and the node helpers, and `wire_tap.c` wraps the
  platform's `sendto`, `recvfrom`, `bind` and `setsockopt` to count datagrams by type,
  inject faults and catch a socket bound outside loopback. `atomic.h` holds the loads, stores and adds for
  state a service thread shares with the test.
- `tests/interop/` holds one Tick program per language and the driver that runs a pair.
- `tests/fuzz/` holds the libFuzzer targets, their replay driver and the inputs that once
  crashed one.
- `tests/golden/` holds the schema hashes every binding pins.
- `tests/bench/` is `rant_bench`: the sweep and the benches.

## Harness rules

- `CHECK` evaluates its condition once, but never put a side effecting or draining call
  inside it.
- A program lists its cases and ends with `TEST_MAIN`. `prog` runs every case, `prog name`
  one, `prog --list` lists them. Exit 0 passes, 1 fails and 77 skips.
- Every program also builds with `RANT_NO_SHM` as a `.noshm` twin, except the shared memory
  ones.
- Launch paired processes in one shell command. The gap between two tool calls fakes a
  peer drop.
- `ctest --preset windows` (or `linux`) must pass, plus a `rant_bench sweep` for
  transport changes. A comment only change must leave code byte identical.
- Every node a test opens is pinned to `"127.0.0.1"`, and every bare socket binds it, so
  nothing listens on the network and the Windows firewall never asks. On Windows the
  harness fails a case that bound a socket anywhere else.
- ctest hands each program its own block of 64 domains through `RANT_TEST_DOMAIN`, a run
  by hand takes one from the clock, so parallel programs and the SHM module test (segment
  names carry the base) never meet. A fixed port derives from the base too.
- Wait on a condition with `PUMP_UNTIL` (or `WAIT_UNTIL` under service threads) and a
  generous budget: it ends as soon as the condition holds. A fixed `test_pump` window is
  only for proving that something does NOT happen, and it lasts as long as the protocol
  needs, such as an announce interval or a peer timeout. Never count passes, which stretch
  under load.
- A raw transport `on_message` sees the 8 byte source stamp first. A payload tag sits at
  `data[RANT_TIMESTAMP_BYTES]`.
- A wall clock threshold check (waker under 400 ms, varwait under 250 ms) can flake once
  under load. Suspect those before the code.
- A nested wait only pumps the calling node, so a peer in the same process must answer
  from a service thread.
- A counter or flag that a callback on a service thread writes while the test reads it
  goes through `atomic.h`, so the order holds and TSan sees it. A plain read is fine once
  `rant_node_stop` joined the thread.
- Default QoS (shallow ring, no backpressure wait) loses bursts legally. Lossless
  multithreaded tests set `keep_last` and `backpressure_wait_us`. A burst that fits one
  datagram parks in `tx_hold` and counts as sent, so a hostile eviction test needs bursts
  bigger than one datagram plus the ring.

## Programs

One ctest entry per program, named `<folder>.<program>`. Every case below runs in its
program in this order.

| Program | Cases |
|---|---|
| `unit.transport` | basics, rate, lapped, rtt, ahead, qos_match, best_effort_flow, tap_framing |
| `unit.discovery` | records, migrate, owed, wire, change_age, discovery_core, node_core |
| `unit.shm` | module, loss |
| `unit.schema` | dsl, wire |
| `unit.detail` | detail_codec, detail_paging, interest_codec |
| `unit.tunnel` | hash, handshake, frame, json, batch |
| `e2e.stream` | stream, scale, named, collision |
| `e2e.node` | open_fail, event_user, dynamic_grow, launch_name, launch_domain, launch_log_level, prefix, domain_groups |
| `e2e.schema` | advert, bind, big_enum, root, stdtypes, pattern_schema |
| `e2e.detail` | detail_live, interest_external |
| `e2e.shm` | shm_node |
| `e2e.queues` | queue, pull, pull_view, callback_queue, callback_pattern, queued_retire |
| `e2e.patterns` | patterns, variable_wait, dup_authority, retire, reflect_reuse, loud, sync_capture, one_announce, handle_gate |
| `e2e.tasks` | task, task_matrix |
| `e2e.entities` | reflect_dropped, churn, republish, log_and_meta, status, rant_conditions, rant_refused, rant_cap |
| `e2e.matchwait` | matchwait |
| `e2e.mesh` | builtins_quiet, seeded_segments, meta_from_callbacks, directed_first_call, static_bounded, static_meta_peers, observer_fast, grow_at_once, big_peer, slot_back, runtime_topic |
| `e2e.timestamps` | timestamps |
| `e2e.threads` | threads |
| `bench.memscale` | `rant_bench memscale`, a steady state send never allocates |
| `bindings.cpp`, `.python`, `.csharp` | the binding suites, below |
| `fuzz.receive`, `.schema`, `.message` | the target's seeds and saved crashes, below |
| `interop.<pub>_to_<sub>` | one language publishing to another, below |
| `build.strict` | the compile checks, below |

## Phases

What each phase pins, grouped by layer.

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

- Records: a store is found by uuid and by id through doubling and reuse, every add mints a
  new id that skips 0, an arena store stays at its reserve, and a refused growth leaves
  every record in place. The due queue holds exactly the records owed work, oldest first,
  keeps the earliest due time and loses a removed record, and the live count follows every
  change of state.
- Migrate: a grow keeps each peer's id, scratch, locator and name and takes the peer it
  refused. A pool core does not migrate, nor does any to fewer slots.
- Owed work: the opening carries the blob with no REQ, an observer's asks REQ|ALL, nothing
  goes out before it. A targeted, unicast or observer solicit is answered at once, a group
  one within one ms per live peer and spread. A first blob from the group is answered, one
  from a targeted send or a change is not. A version ahead is fetched at once, four sends
  250 ms apart, a later announce starts over with one send after a fetch that went
  unanswered, the blob at that version or one too big ends it, and a fetch settles an
  answer owed too. Copies go marked UNI to a record heard by unicast
  within the peer timeout and plain to a slotted one, none to a dropped one. A silent peer
  drops within a quarter interval of its timeout, and a change rides one announce.
- Wire: every field round trips and every short, long or cut datagram is refused as such.
- Change age: it counts from the start, restarts at each change and saturates.
- Lifecycle: two announces are two ups, silence past the timeout drops both but keeps
  their slots, the same uuid resumes under the same local id, and a new uuid from a held
  address evicts the predecessor as GONE because one socket is one process.

### Node core and runtime

- The node core drives its peer table over a transport and a real discovery core with no
  sockets, clock or platform. A nameless announce becomes "unknown-peer". A third peer is
  refused while the table is full of active peers and the node forwards the event.
- Domain groups: two domains on one host each discover themselves, and no announce of the
  other domain reaches either discovery socket. On Linux this fails without
  `IP_MULTICAST_ALL` off. Other programs' unicast seeds to the discovery port are not counted.
- Open failure paths: a forced failure at each staged cleanup label returns NULL and
  leaves the platform balanced. A join the wire tap fails (`fail_join`) unwinds the node
  and `rant_last_error(NULL)` names MCAST_JOIN. An occupied data port collides, since the
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
  refusals that keep an array element's stride static. A text of several statements
  compiles to its last, a repeated definition must match, and a definition taken by name
  hashes like the plain struct. A registry keeps a definition for its later compiles,
  rolls a failed text back and holds one handle per shape, and a node reserves its
  builtin names and reports a refusal with the place in `detail`. Print hoists each named type as a leading definition,
  dependencies first, and recompiles to the same wire.
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
- CAPPED: a `keep_last` 1 publisher at 1 kHz against a `max_rate_hz` 5 subscriber. No send
  may wait for the capped lane's tick, the subscriber stays paced and nothing counts as
  evicted unsent. A lone held back sample after a quiet spell must leave at the 200 ms
  tick, which pins the kick a send owes a timer it armed. The slow rate keeps a wait apart
  from a busy runner's stall: one would take up to 200 ms, the bounds are 100 ms and 1 s.
- HOSTILE (Windows): transport sends forced to would block. The first pass parks one
  datagram in `tx_hold`, past that the burst overwrites truly unsent history, which must
  surface as RANT_EVICTED_UNSENT so every send is delivered or accounted.
- WAKER: an idle started pair delivers a single send within milliseconds, not at the next
  announce capped wakeup.
- UNLOCKED HANDLER: an inline handler on the service thread is held while another thread
  sends on the same node, and lets go only once those sends returned, so a send that
  waited for it would take its one second hold, not a stall's milliseconds. A create
  from a third thread waits for the handler and succeeds, one from inside stays refused,
  a burst of eight sends from inside the handler on a depth 1 topic all arrive, and a
  function handler held the same way, driven by a caller thread, stalls no send either.
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
  dispatch under service threads wakes on arrival on the caller's thread. An error event
  parks on the event queue with last_error current, dispatches with its copied name, and
  a NULL event queue drops the parked one and fires inline again.
- Callback queues on patterns: a provider and a caller with one queue each. The request
  parks at the provider and its handler's own retire is refused, the reply parks at the
  caller, a blocking call completes inline through a queued channel, a NO_PROVIDER outcome
  parks, a task's RUNNING, progress, cancel notification and terminal outcome each park
  with the cancel flag set at receipt, a variable's replay parks, the owner applies a
  remote write at receipt and notifies at dispatch, the remote caches before its own
  dispatch, and a retire settles a parked reply CANCELLED and drops the records.
- Queue rings: polling only parks, a timed dispatch drives the loop, a best effort ring
  overwrites oldest at the cap, and a reliable ring parks by withholding acks so a slow
  dispatch loop still receives everything in order.
- Pull topics: polling only holds, take returns the oldest with its stamps and names,
  take_latest returns the newest and drops the rest, a timed take drives the loop, and
  take on a topic created without pull and pull with a queue are refused. A reader still
  holding the view of a 600 KB or 1.5 MB message keeps the next one, below the default cap
  and above it. Only with shared memory: over UDP a burst that big outgrows the receive
  buffer Linux allows by default, so the `.noshm` twin leaves it out.

### Patterns

- Functions: match, async call, an empty ack when the handler returns without replying,
  deferred completion, NO_HANDLER, a client synthesized TIMEOUT, a fail with an over long
  message that truncates and still carries data. A call before the match forms queues and
  flushes rather than timing out. Two callers answered in one provider tick each get
  their own reply. A burst past keep_last engages backpressure without the pattern layer
  holding the node lock. A deferred reply landing after a local timeout is dropped and
  the next call still works.
- Blocking call capture: the caller's page hook stalls any allocation the caller makes
  without its own lock. None may happen, and a call the provider never answers runs its
  whole timeout. When the capture ran unlocked, a call timed out mid copy, the late write
  ended the next call early and closing the node crashed.
- Provider reply burst: an inline reply is a reentrant send that never waits, so the rsp
  ring is the only thing between a drained batch and lost replies, which are invisible at
  the provider. The caller needs the same depth in its req ring, and both lanes must be
  up before the first reply commits since rsp carries no catch_up.
- Rant conditions: a writer with two ill fitting readers holds one schema_mismatch per
  reader naming it, published with origin Rant, and loses exactly one when that reader
  leaves. QoS, kind, duplicate authority and a forced variable each raise and clear with
  their cause. 20 refusals fold into one condition under the kind alone, and three left
  are listed one by one again, while the app keeps its room. Six kinds of three list 15
  and count 3 in more_problems. A static node with room for one peer holds peer_refused
  naming the address and drops it once the turned away node goes quiet. In the discovery
  core 20 sources on another wire keep the first 16 tracked, report 17 times in all, and
  age out together.
- Handle gate: 20 callers, each with a req schema whose wire (1325 to 1350 bytes) fills
  the first detail page so rsp follows on a second, call right after creating the remote.
  Every call is answered and no handler runs before its rsp lane. Without the gate 19 of
  20 replies went to a missing lane. The provider's match wait is off, so the case needs
  no sleep past the window.
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
- Republish: a `fetch_details` viewer that judged a new publisher while its own slot was
  retired still receives it once an identical subscriber reuses the slot.
- Match wait window (Windows): a publisher that never receives a candidate's DETAIL_RESP
  waits out one window on its first send, then no send waits, and a subscriber that
  appears after the window never blocks a send.
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
- Write timestamps: a delivered stamp lies within the send and delivery walls, the opt
  out gives 0 with a byte identical payload, catch_up replay keeps the original stamp,
  and the queued path and the patterns layer carry the same value.
- Logs and meta: history logged before anyone listens replays to a late joiner, a line
  below the log level is never sent, a mirrored internal error lands at Error with origin
  Rant and the time it fired, and a directed meta call decodes.
- Status: two raises before anyone listens coalesce into one set and a repeat sends
  nothing, a late joiner gets the whole set, an update keeps `since_us`, a clear that is
  not clearable, names an old raise or a missing key changes nothing and leaves a Debug
  note, an accepted clear fires `RANT_STATUS_CLEARED` with the asker, a full set refuses
  one more, and a node that reads no status has no clear to send (`RANT_ERR_ROLE`).
- Launch log level: `RANT_LOG_LEVEL` in any case fills a level the code left at 0, never
  one it set, and a word that is no level, or a bad `opts.log_level`, refuses the open.
- Mesh: two nodes that share nothing match nothing, builtins included, until one asks meta.
  Seeded segments with no shared topic keep hearing each other. Meta asked only from
  callbacks is answered. A call aimed at one provider waits for that provider. A static
  node among many unrelated peers stays bounded in memory and in its meta peers answer. An
  observer sees every peer within 100 ms. A new topic matches every known subscriber past
  the starting table. A peer too big for one announce never takes the slot a subscriber
  needs, and a peer that stops sharing gives its slot back. A runtime topic nobody wants
  sends at once, and one three peers want reaches all three.

### Benches

- sweep spawns node children per rate and aggregates their SUMMARY lines. The two machine
  form runs `serve` workers on a control domain: the CMD topic is reliable with catch_up
  0 so stale commands never replay, the RES topic keeps catch_up at depth so results
  survive a control peer flap, entries are tagged with domain and run nonce, workers
  HELLO at startup, and both machines must share a subnet.
- memscale: steady state must be alloc free and warm sends pay no allocation. The command
  exits 1 when a steady state send allocated.
- threadbench: keep_last 64 so the drain guard rather than ack flow control engages, and
  backpressure only ever engages flat out.
- queuebench: inline callbacks against a 4 MB callback queue ring. The ring must cover the
  writer's in flight burst or the reader parks and the overrun heals through repair,
  which is a sizing bug, not steady state cost.

## Binding tests

Each binding suite is one ctest entry under the `bindings` label: `bindings.cpp`
(`rant_cpp_test`), `bindings.python` (`bindings/python/test.py`), `bindings.csharp`
(`bindings/csharp/test`, which the `bindings.csharp.build` fixture builds with dotnet when
ctest runs) and `bindings.rust` (`cargo test` over `bindings/rust/rant`, beside
`bindings.rust.ffi`, which checks the generated FFI is current). Each runs two nodes in one
process on loopback, takes a leg name to run one and `--list` to list them, runs every leg
even after a failure, and reads its domains from `RANT_TEST_DOMAIN` like the C programs.
Rust's legs are cargo's test functions, so a name filter runs one.

The C++ legs: dynamic covers the dynamic schema API direct and over service threads,
patterns the patterns layer with the memcpy, loop and rebase codec paths plus entity
reflection, value_root bare type roots and the cross language hashes, stdtypes the standard
types including the video family, tails variable members as tail frames, tasks defer,
progress, cancel, no_cancel and a retire mid run, queue dispatch threading, a handle on its
own queue and pulled subscribers, lifetime shared name holds, moves, the last close
retiring a name, handles and deferred replies outliving their node, a handle close refused
in its callback and a node close inside its own callback, rebind a rebase decode after the
reader's retired slot goes to another name, close_race sends, writes and creates on other
threads while the node closes (a sanitizer build sees every miss), factory the node
factories and their refusals, `reflect_from_mesh`, and the reflection walks and meta
calls. `rant_cpp_test bench` runs the cost of each codec path
(memcpy, loop, tails, dynamic) alone and end to end on loopback, next to a C node pair on
the same footing, so a wrapper change is measured against the C floor.

The C#, Python and Rust suites mirror the same ground. Two things every suite shares:

- `tests/golden/schemas.txt` holds the canonical schema hashes, and the Float3 wire bytes,
  that every binding pins. The C schema program checks every line, each binding checks its
  own mirrors against them. A line changes only with a deliberate wire format bump.
- The layout leg of the C# and Python suites compares every struct mirror's size and field
  offsets with `build/tests/layout.txt`, which `tests/support/layout.c` writes at build
  time. A mirror that drifted from the C struct fails there instead of crashing a node
  open.

## Compile checks

`build.strict` compiles the whole implementation alone with every warning an error: C99
pedantic plus `-Wall -Wextra` on GCC and Clang, `/W4` on MSVC. It covers the default build,
`RANT_NO_SHM`, `RANT_NO_THREADS`, the Yaskawa module's `RANT_PLAT_CUSTOM` with both of those,
`RANT_NO_PATTERNS` with `RANT_NO_STDTYPES`, and the C++ wrapper with the implementation
compiled as C++. That guards the cores staying C99 clean for the VxWorks GCC without its
SDK. The entry builds the `rant_strict_*` targets in one call, so the normal build never
fails on a warning and one build tree never runs two builds at once. Partial initializers
are idiomatic, so `-Wmissing-field-initializers` stays off.

## Interop

Each binding suite talks only to itself, so `tests/interop/` crosses them. Every language
has a Tick program (`tick.c`, `tick.cpp`, `tick.py`, `csharp/`) on one schema, `Tick { seq:
u64, when: Timestamp, value: f64, at: Transform }`, reliable with keep_last 64 on loopback.
`pub` sends `value = sin(seq * 0.01)` and the position `(cos, sin, 0)`, `sub <seconds>
<publisher>` exits 0 once twenty ticks from that node decoded right. `rant_interop_pair`
starts a publisher, then a subscriber, returns the subscriber's exit code and stops the
publisher. ctest runs every ordered pair of the languages this machine has, one
`interop.<pub>_to_<sub>` entry each, and hands the pair its domain through `RANT_DOMAIN`.
C# builds as the `interop.csharp.build` fixture.

## Sanitizers and coverage

The `linux-asan`, `windows-asan`, `linux-tsan` and `linux-coverage` presets (commands in
docs/building.md) run the C and C++ entries instrumented.

- ASan and UBSan pass on Linux and Windows. A leak, an overflow or undefined behaviour stops
  the program, so the entry fails.
- TSan flags the node lock's reentrancy check (`lock_held`, `lock_owner` in
  `src/node/runtime.c`), which reads the owner outside the mutex. Only the holder writes
  its id and zeroes it before release, so the check is sound, but TSan fails the threaded
  entries until those reads are atomic, which the platform layer offers only under `RANT_SHM`.
- Coverage counters update atomically (`-fprofile-update=atomic`), or service threads
  corrupt them and gcovr refuses negative counts.
- Clang's UBSan (the `linux-fuzz` tree) fails `bindings.cpp` and the C and C++ interop
  pairs: the node calls the C++ wrapper's event trampoline through a pointer to the C
  `RantEvent` function type, while the trampoline takes `rant::detail::RantEvent`. GCC has
  no such check.
- Instrumented trees link the C and C++ runtimes dynamically (`RANT_STATIC_RUNTIME` off),
  since they never leave the machine.

## Fuzzing

Each `tests/fuzz/<name>.c` defines `LLVMFuzzerTestOneInput` and `fuzz_seeds`, which hands
over valid inputs the live code builds. Every build links it with `replay.c` as
`fuzz_<name>_replay`, and ctest runs that as `fuzz.<name>` over the seeds and every file in
`tests/fuzz/crashes/<name>/`. `RANT_FUZZ` (the `*-fuzz` presets) also links it with libFuzzer
as `fuzz_<name>`. Fuzzing itself runs by hand, never in CI.

- `receive` opens a node and hands each chunk, a source port and a datagram, to the
  runtime's own dispatch (`i_rant_node_rx_one`), then polls once and walks the reflection.
  The wire tap isolates it, so sends vanish and no real datagram arrives, and it runs on
  domain 65000, which the seed carries. The seed is a recorded session: a peer on loopback
  matches every entity, publishes a small and a fragmented image, calls, writes and leaves.
- `schema` compiles DSL text (first byte even) or parses a peer's wire (odd). Any schema
  reparses to the same hash and validates its default message. A compiled one also names
  every option, reads itself and prints back to the same schema. A wire may hold what text
  refuses, so those three are not asked of it.
- `message` reads a message of one of the `schemas.h` schemas, or a bare map body, through
  every getter, element and nested map. A valid map reads every entry, a valid message every
  top level field, and a valid message copied field by field onto a default one still
  validates with its integers intact.
- `FUZZ_REQUIRE` states a property beyond not crashing. A failure aborts, which libFuzzer
  saves as a crash.
- Once a crash is fixed, its input goes to `tests/fuzz/crashes/<name>/` under a name that
  says what it caught, so every build replays it. A receive input opens with recorded
  announces, so a `RANT_WIRE_VERSION` bump rewrites their version byte, or the peer is never
  accepted and the input reaches nothing.
- Found so far, fixed with their inputs saved: a zero length fragment of an empty sample
  offset a NULL buffer, the same empty sample offset NULL again in delivery, a DATA header
  claiming a sample longer than its fragments sized a 3 GB allocation, and a type name with
  a run of single letters (`eL` to `EL` to `El`) did not print back to itself.
- Accepted by design: a peer's wire may name a standard type with another shape, name a
  field twice or leave an option unnamed. Text refuses all three, but none breaks the layer
  below, so only a compiled schema owes the stricter properties.

## Measurement traps

- Never redirect a child's stdout through a PowerShell pipe. The pipe fills and an
  unbuffered printf in the child blocks for seconds. `rant_bench sweep` spawns children
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

Threaded (`rant_bench threadbench`): flat out 688k msg/s UDP and 452k SHM on loopback with
zero evictions. Queued delivery (`queuebench`) costs 14% at 1 KB up to 35% at 1 MB.
Reliable same host SHM at keep_last 4 went from 92 to 2300 msg/s with the immediate in
order ack. A zero subscriber send costs 52 ns. `rant_topic_match_count` costs 11 ns, so an
app that builds an expensive payload should gate on it.

## Live debugging

A `probe.c` that defines `RANT_IMPLEMENTATION` against `dist/rant.h` can read every
internal struct (discovery peer flags, locators and paths). `tests/support/rant_impl.h` shows
how to route `sendto` and `recvfrom` through wrappers to log all discovery and detail
traffic. Interest lines stuck at a fixed
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
