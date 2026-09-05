# Transport

The reliable, fragmenting UDP transport. RTPS inspired: KEEP_LAST history, NACK repair,
fragmentation, unicast point to point data. Every rule here was measured, not guessed.

## Data is unicast

A publisher sends each message point to point to every matched subscriber's data port.
There is no data plane multicast. Multicast data measured slower than the unicast fan out
even at high fan out, and it carried group join, membership cap and interface complexity.

## Wire

Submessage headers: DATA is 13 bytes for a single fragment (`frag`, `count` and `len` are
implied) or 21 for multi fragment, HB is 23, NACK is 21. Byte 0 is `type | flags` (the
single fragment bit lives here, and `(b[0] & 0x07)` in 1 to 3 picks out a transport
datagram). Bytes 1 and 2 are the topic index. Every message carries 8 little endian bytes
of `written_us` inside the sample ahead of any pattern header, unless the topic opted out,
so repair, replay, shared memory and the queue all keep the original stamp.

There is no GAP submessage. A writer that cannot satisfy a NACK, or that overran its
ring, answers with an HB whose `first` advertises its floor. The reader's HB handler
skips past the dropped range with one `DART_MSG_LOST` counted in seqnos (fragments, not
messages) and restarts at the writer's oldest cached sample.

## Scheduling

Sending is driven by an active lane queue. The event that gives a (topic, peer) lane
work also enqueues it, so poll cost scales with traffic, not with how many topics exist.
Heartbeats and delayed acks are clock driven and found by an amortized sweep that covers
the lane table every `DART_HB_SWEEP_US`. The sweep skips a whole topic row when nothing
reliable is matched on it.

A send with zero matched writers skips the grow, the memcpy and the commit unless the
topic retains history (reliable with `catch_up` above 0). The match count is O(1) off a
cached counter. The send path kicks the waker only when it committed something
(`dart_transport_tx_pending`): the waker is a loopback send that costs about 36 us on
Windows, and an idle publisher used to pay it once per poller sleep.

The poll wait is capped at `next_deadline_us`, the earliest armed timer, fed at the arm
sites. Immediate acks are deliberately not tracked, so the no loss hot path keeps no
deadline. The win is on the deferred paths (a gap repair NACK, a lost final message HB),
which fire at their timer instead of up to a sweep period later.

## Flow control

A reliable send that would overwrite unacked history waits up to
`qos.backpressure_wait_us`. In threaded mode a send that would overwrite history not yet
handed to the wire waits one TX pass, bounded by `DART_UNSENT_WAIT_US`. If it still must
evict, KEEP_LAST proceeds and `DART_E_EVICTED_UNSENT` fires. Evicting sent but unacked
history is ordinary KEEP_LAST and fires no event.

A best effort reader on a reliable topic is fire and forget: out of flow control, no
heartbeats, no waiting for acks.

## Acks and repair

A completed in order sample arms its cumulative ack as due now. Partial, out of order and
heartbeat acks keep the deferred `repair_delay_us` timer. Reliable throughput is window
depth over ack round trip, and a deferred in order ack put that at the mercy of the HB
sweep (a keep_last 4 stream capped near 100 msg/s). The ack is still armed after
delivery, which the reader owns copy and zero copy SHM invariants require.

Repair is gap triggered, received bounded and in flight deduped. A fixed re NACK cadence
collapsed the repair channel, since a far behind reader re requested its whole window
every period. The reader NACKs only on a proven skip, bounded by `received_high` (the
highest seqno actually received, kept separate from `hb_last`, which is only the HB
claim). Each floor is requested once. `nack_high` is the top of the last request, so a
refill asks only for the new part. A stalled floor re asks only after the
`nack_retransmit_us` backstop, which is also the one path allowed to chase `hb_last` for
tail loss. Outstanding repair is capped at one `DART_NACK_WINDOW` (32) and clocked to
delivery. `nack_high` is not reset when a sample's assembly starts, or a whole message
request already in flight would be re asked.

The writer MERGES a repair request into the one it still holds: an ACKNACK that lands
before the previous one's resends went out trims the pending bits at the new cumulative
ack base, then ORs the new bits in. Overwriting instead stalled every such hole on the
50 ms backstop.

The writer raises HB `first` to `acked_upto`, and the reader acks its mid message
contiguous front, so the reader ignores an HB `first` that lands inside the sample it is
still assembling or has parked. Otherwise it skipped into its own partial sample and
rejected every resend as old. Eviction is whole message, so a genuine floor never splits
a sample.

## The lapped reader

When the writer's floor passes a sample the reader was still fetching, the restart point
is the sample the writer evicts NEXT, and every fragment of it already went by, so it
must come back through the repair window before the writer's next commit. A reader that
loses that race once loses it every time on a fragmented stream whose whole message
repair outlasts one inter message period, and would drop every live message as "ahead"
for as long as the stream lasts.

So when the floor passes a sample the reader was still FETCHING for the second time with
nothing delivered in between, the reader rejoins at the writer's head. The cached window
is given up (that one `DART_MSG_LOST` covers it) and the next message arrives in order. A
skip of a PARKED sample (the consumer refused it) is the consumer's stall, so it neither
counts nor resets. Any delivery resets. Single fragment and shared memory paths never lap.
The `lapped:` selftests pin it.

## One sample held ahead

A reader lane keeps two assembly slots: the head sample and the one after it (or, while
the head is wholly missing, any one later sample). Without it a fragment of a later
message was dropped outright, so one late resend threw away everything that arrived
meanwhile and the next message came back through the repair window in serial pieces. A
repair longer than one inter message period cascaded into whole message refetches.

Now a repair costs only its own round trip. The held sample delivers right behind the
repaired head, in order, with one cumulative ack for the run. A second future sample is
still dropped and repaired in order later. The hold keeps filling while the consumer has
the head parked, and an unpark delivers both with no resend. A writer floor that lands ON
the held sample delivers it from the hold. The slots swap by struct on rotation so the
grown buffers travel with them. Memory per reliable multi fragment lane is two messages
instead of one. An allocation failure for the hold just drops the fragment.

Nothing ever moves the floor on arrival. Only a delivery, the writer's HB floor and the
lapped rule do, so the hold can never outrun a repair. A repair request may span both
slots when they are adjacent. `frags_ahead` counts only fragments beyond the hold. The
`ahead:` selftests pin it.

Known limits, by design: the hold covers a repair that finishes within one message period
beyond the head. A round trip longer than the period plus a lost resend still cascades,
and k slots under a byte budget is the generalization if that ever matters. A whole
missing message is still fetched through the 32 seqno window in serial pieces. The
`nbits` field is already u16, so a variable length bitmap is the way to make that one
round trip. Rejected: a seqno keyed fragment ring (a wrap forces a copy out), and
assembling into the consumer queue (the transport is per lane and sans-IO, the queue is
per topic and node owned).

## Round trip estimate

The transport keeps one estimator per peer (`DartPeerRtt`, RFC 6298 shape: smoothed
value, mean deviation, floor, sample count), fed by the reliable path with no probe
traffic. The first sample seeds srtt = R and rttvar = R/2, then rttvar = (3v + |srtt - R|)
/ 4 and srtt = (7s + R) / 8. A writer times the push of a sample's LAST fragment to the
cumulative ack that covers it (armed at the sample boundary only). A reader times a repair
request to the resend it brings (the lowest seqno of a NACK never asked before). Only
unambiguous samples count (Karn's rule: a seqno asked twice, a resent sample, a resync or
an eviction jump disarms the probe). Both directions feed the same estimator, so a node
that only subscribes still measures the peers it calls functions on, and a node that only
publishes measures from its acks. The estimator is per peer slot, carried across grow and
zeroed on peer add and remove.

The bound is the smoothed value plus max(1 ms, 4 times the deviation), never under
`DART_RTO_MIN_US` (the poll wait is millisecond granular). It drives the reader's re ask
backstop (`qos.repair_delay_us` 0 = adaptive, a nonzero value pins it, 50 ms until the
first sample) and the writer's tail heartbeat (`DART_HB_TAIL_US` until the first sample).
One lost resend then costs one round trip instead of 50 ms. Read it through
`DartPeerInfo.rtt_*`, the `@dart/meta` peers section, or `dart_transport_peer_rtt`. The
`rtt:` selftests pin it with a virtual clock.

## Tail heartbeat

When a reliable lane's send queue drains, the next HB comes after `DART_HB_TAIL_US` (or
the peer's measured bound) instead of `heartbeat_us`, so a lost final message repairs
fast. The reader's immediate ack suppresses it when nothing was lost.

## Rate throttle

A fire and forget lane (best effort reader, non directed topic) stamps a per lane wire
seqno of `sent_upto - wire_skip` on DATA and SHM DATA, so its reader sees a private
contiguous line while history and flow control stay global. `wire_skip` starts at the
join seqno. A paced skip adds the skipped count to `wire_skip` so it leaves no gap. An
eviction leaves it alone so it does leave a gap, which is real loss. The rate rides a
sparse section of the interest blob, `[u16 n][(u16 topic_index)(u16 rate_hz)]*`, applied
after rematch onto each matched writer lane. The emit throttles at a sample boundary,
holds until the tick, then decimates to the newest sample. The scheduler gates on
`rate_next_us` and the HB sweep re wakes due throttled lanes.

Reliable and directed lanes keep the shared global line, since repair must map a NACK
straight into history. A private line only pays off where there is no repair.

## Directed sends

A directed send shares the topic's single seqno line. Only the destination lane carries
the message. Every other matched reliable lane is advanced past the seqno and skipped via
its HB floor, with no cross delivery, no repair and no `MSG_LOST` on a directed topic.
Known costs of the shared ring for directed reliable, accepted at RPC rates: cross peer
backpressure coupling, O(peers) floor HB traffic per directed send, repair depth diluted
by active destinations, and `catch_up` refused at define.
A sample stores its destination as a peer slot, not an id. Slot reuse is safe because a
directed topic never replays history and a new peer joins at the head, above every
stamped sample.

## Peer resume

The subscriber owns the truth. A publisher fans out to many subscribers, so its peer table
is the pressured one, while a subscriber listens to few publishers per topic. The reader's
`deliver_upto` is also literally what its app consumed. The writer owns only its KEEP_LAST
ring and may forget a subscriber freely.

Resume is lossless for free: the reader's receive path already drops anything at or below
`deliver_upto` and its ACKNACK already carries that position. Duplicates only ever came
from zeroing `deliver_upto` when discovery timed the writer out. Not zeroing it makes a re
pushed `catch_up` dedupe itself while the real gap is NACKed from the preserved position.

A position is meaningful per writer incarnation, because seqnos reset on restart, so it is
keyed on the writer's uuid: the same uuid returning reuses the same local id, and a new
uuid is a fresh peer.

Two identity holes are closed, both load bearing:

- The READER EPOCH rides the ACKNACK and covers a reader restart that reuses its address.
  Otherwise the surviving writer lane kept the dead incarnation's `acked_upto`, whose HB
  said "you miss nothing" to a fresh reader, and nothing ever rematched. An epoch change
  forces a full lane re join. Two traps it created: an unstarted reader must not adopt a
  position from an HB (the floor may be the dead reader's), and since that leaves it never
  NACKing, the ACKNACK carries an UNPOSITIONED flag telling the writer to re push from the
  unacked edge.
- ENDPOINT EVICTION covers the writer direction, which the epoch cannot, because DATA
  carries no writer incarnation tag. On a new uuid announcing from an (ip, port) we already
  hold, the predecessor is evicted as GONE first: one socket is one process, so it is
  provably dead.

A dormant reader still delivers inbound data but withholds acks until resume, so a one
sided announce loss with live data can briefly stall the writer's backpressure. Treating
inbound DATA as proof of life was left for a separate decision. Kept on purpose: the single
immediate ack at match (the one sided rematch depends on it), and reinforcing announces to
DROPPED peers (the only resume path on a network with no multicast). A BYE is a full
teardown because a graceful restart regenerates its uuid anyway.

## Shared memory

A message that would fragment to a same host subscriber goes through shared memory
instead: one 37 byte descriptor on the wire and the payload in a shared chunk. The cutoff
is this node's own fragment size (`dart_transport_frag`), since a writer fragments its
whole seqno line with one size. A message that fits one datagram goes inline either way,
because the shared memory path still costs a descriptor plus pool and attach work. Same
host is a host id match, not an address match. See spec/shm.md.
