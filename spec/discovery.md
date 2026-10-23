# Discovery

How nodes find each other and how a peer's address and liveness are tracked. The user
facing options are in docs/discovery.md.

## The announce

Every node multicasts an announce every `announce_interval_us` (3 s) on its domain's
group and sends copies by unicast to its seed peers and some known peers (Owed work and
copies). Every discovery
datagram carries the node's uuid, its name, its data port, the version of its blob and how
long ago that blob last changed. The blob is opaque here: the fragment size, the shared
memory host id and the interest list (spec/interest.md).

The blob is full state and idempotent, so it is simply re sent on change. It rides the
opening datagram, the first announce after a change (`RANT_DISCOVERY_BLOB_RESEND`, 1) and
every targeted send. Every announce carries the blob's version. A steady state announce
without the blob but with a newer version makes the peer fetch the blob. So a connection forms from the announces the nodes already send, with no per
topic handshake and no steady state metadata heartbeat. `on_peer_up` fires only on a
change (admission, address, blob, revive), never per announce.

Every datagram stays under one MTU. Nothing ever relies on IP reassembly, because lwIP
class stacks often cannot reassemble and a dropped fragmented announce would leave the
peer unresolvable forever. When the interest list no longer fits, the announce ships a
fixed size bootstrap instead and peers page the interest blob over unicast
(spec/interest.md).

The accept bound (the largest peer blob stored, first sized from our own topic count)
self heals. A bigger peer's announce, even one the OS truncated, fires
`RANT_E_PEER_META_TOO_BIG` internally. The node grows the bound and its receive buffers
at the next poll, and the grown bound fetches that peer's blob alone (Judging on arrival).
Per peer blobs are hook allocated at their actual length and reused across occupants. The event carries the overlay when the
datagram arrived whole and a NULL data pointer when the OS truncated it.

Unicast discovery traffic leaves the DATA socket (`i_rant_discovery_set_tx_fd`). Group sends
stay on the multicast socket. So all unicast between two nodes uses one port pair, and each
announce is identity plus return path in one datagram. Per peer unicast goes to one port: the data port
every datagram names, else the discovery port when that is 0. Discovery always runs inside a node, so the locator it
advertises is the node's data socket and it owns no unicast socket of its own.

## Wire format

Every discovery datagram is, little endian:

| bytes | field |
|---|---|
| 0 | the magic `uDSC` |
| 4 | the wire version, `RANT_WIRE_VERSION` = 8 |
| 5 | flags |
| 6 | the u16 domain |
| 8 | the sender's 16 byte uuid |
| 24 | u32 the version of the sender's blob |
| 28 | u32 how many ms ago that blob last changed, saturating |
| 32 | u16 the blob's length, 0 without BLOB |
| 34 | u16 the sender's data port |
| 36 | u8 the name's length, at most `RANT_NODE_NAME_MAX` |
| 37 | the name, then the blob when BLOB is set |

The flags are BYE 0x01, REQ 0x02 (a solicit: answer with your blob), UNI 0x04 (a copy by
unicast: copy your announces back), ALL 0x08 (an observer's solicit: answer at once), BLOB
0x10 (the blob follows the name, maybe empty) and ONE 0x20 (a targeted send or a plain copy,
whose sender holds our blob: a REQ in it is answered at once, and it starts no copies). A steady state announce carries no blob. A
datagram longer or shorter than its fields say is malformed, except one whose blob arrived
short: the OS cut it to our buffer, and the stated length says how big the peer is.
`i_rant_discovery_encode` and `i_rant_discovery_decode` are the one codec. The change age
is the sender's clock read when it builds the datagram, so a stale clock reads young.
Version history: v3 added the versioned blob, v4 and v5 relaying, which v6 removed. v8 moved
the data port and name out of the blob into every datagram and added the change age and
the flags UNI, ALL and ONE. Since
v5 the number versions everything that crosses the wire, discovery or not, and any change to it
bumps the number. The first eight bytes (magic, version, flags, domain) keep their place in
every version, so a datagram of this domain on another version is recognized from them
alone: it is dropped and its source reported once with `RANT_E_WIRE_VERSION`, the last 8
sources remembered. A datagram with another magic or domain is dropped silently, as is a
malformed blob. The per peer
overlay capacity defaults to `RANT_DISCOVERY_META_MAX` (64) bytes and the name to
`RANT_NODE_NAME_MAX` (32).

## Timing

The opening sends the blob at once with no REQ, or REQ|ALL from an observer. The first
steady announce is phased by a hash of the uuid within one interval, so nodes do not
announce in lockstep. Peer timeout sweeps run four times per interval, so detection lags by
at most a quarter interval. `i_rant_discovery_next_due_us` tells a driving loop when the
next timer or owed send is due, a time already past while one is. The defaults are a 3 s interval, a 12 s
peer timeout, a 2 min gone timeout and 16 peers. The interval, the peer count, the group
and the port are stated once in `common/types.h`. A BYE is sent three times
(`RANT_DISCOVERY_BYE_SENDS`), since one shot UDP may lose it and receivers dedup by uuid. The runtime drains a socket to empty on every pass, capped at
2048 datagrams. One receive per poll would let a slow poller's backlog keep a dead peer
alive. Multicast loop stays on, since several instances on one host
need it, and the uuid self filter drops the echoes.

## Records and slots

Every peer heard has a record: its uuid and local id, its locator, name and last heard time,
the version of its blob judged and the highest it advertised, and the work owed to it. Two
hash indexes find a record by uuid and by id. What only a matched peer needs sits in a
slot: the blob, the node's per peer scratch, the source paths and the locator's freshness.
`max_peers` counts slots and `max_records` bounds the records, never fewer than the slots.
The slot hooks (`on_peer_up`, `on_peer_down`) hear only of slotted peers, so the transport
and node core see only them, and a source maps back to a slotted peer only. `on_peer_seen`
hears of every record: up at first contact and on a return, down at the drop or when it goes
while ACTIVE, each time before the slot hooks.

Records are referenced by index, never by pointer, because a store on the hook moves when it
doubles. Without a hook the store sits in the arena at `max_records` and never grows. With
one, `max_peers` records are taken at init and the store doubles as it fills, up to
`max_records`. The node leaves the bound open on a pool that grows. On a fixed pool it passes
no hook, so its records, `RANT_RECORDS_PER_PEER` (4) per slot, and its blobs sit in the arena
from open and a crowd costs it nothing later. A grow moves the slots into the new arena and
leaves the records where they are, so only a core with the hook migrates, and never to fewer
slots.

A store full of live records forgets a newcomer, and one on the hook does so only when it
cannot grow. A forgotten peer has no record:

- Its header alone does nothing. Its blob is judged each time it passes by (a change, an
  answer, an ask) with peer id 0 and a scratch filter, and nothing is kept.
- A match among them takes the record of a live peer that holds no slot and wants none, the
  oldest heard of those the group reaches, else of those heard by UNI. That peer is
  forgotten, the seen hook hearing it go, and the match goes the first contact path. With no
  such record to take, the match is refused under no id.
- A source heard by a UNI copy takes such a record too, but only one the group reaches. Only
  a record's UNI copies carry our changes to it, while a peer the group reaches still hears
  them and asks us when one meets it, so forgetting it loses nothing.
- A solicit from it is answered from the loose queue (Owed work and copies).
- Forgetting a live peer, either way, sets the distrust (Judging on arrival) and notes the
  time. `partial` says one was forgotten within three announce intervals, which the node
  shows as its `directory_partial` condition (spec/node.md).

## Judging on arrival

The admit hook says of a blob whether its peer takes a slot: NO (nothing of ours to meet),
YES (a match) or LOAN (a blob that cannot be judged alone, as an external interest whose
pages come through a slot). A blob is judged as it arrives: always for a record without a
slot, at a newer version for one with. Without a hook every peer is a match and takes a slot
when first heard, blob or not.

| verdict | without a slot | with one |
|---|---|---|
| NO | the record is kept, judged at that version | given back: the slot hooks see `RANT_DISCOVERY_GIVE_BACK`, the seen hook nothing |
| YES | a free slot, else the oldest DROPPED peer's, else a live loan's. None: refused | kept |
| LOAN | a free or DROPPED slot only. None: it waits, unreported | kept, a match staying one |

A refused match is noted under its id for the node's condition and refused again each time
it is heard. When a slot opens (given back, a slotted peer gone or dropped, a migrate), as
many records that wanted one as there are spare slots are fetched, and their answers take
the slots. A slot is promised to a fetched record only while its fetch runs, so one that
never answers gives it to the next once its asks ran out. A record whose last ask went
unanswered is passed over when a slot opens and fetched once each time it is heard while
one is spare, so a peer we cannot reach holds no slot and costs one ask per announce. `rejudge` judges one slotted peer again by the blob it holds, `replay_peers` every
live one: a slot that meets nothing is given back, a loan that now meets us is told.

Each refused match and each loan left waiting bumps `seat_misses`, so a node that can grow
does, at once to `slots_wanted`: every live record holding a slot or waiting for one. A
caller whose own criteria grew calls `refetch_unmatched`, which fetches the live records
judged no match that may meet us now (The filter) until each blob is judged again. `pending`
counts the records without a slot whose blob is being fetched, so one that never answers
stops counting once its asks ran out.

A match the peer may not know of is told by an ask (Owed work and copies), repeated until any
datagram marked ONE comes from it: a new match, or a held one whose version skipped one, since
the peer may have given us back on the version we missed. Consecutive versions need no
answer: the pairings are symmetric, so a pair's verdicts agree at each step. Nor does a match
from a datagram marked ONE (its sender holds our blob or asks for it) or ALL (an observer
never assumes). A slot given back ends the ask, since the peer is no match now.

A first contact heard without a blob is judged by assumption when its change age exceeds the
time since our own last change by 250 ms, the margin alone before our opening: it judged our
current blob since, and asks if it meets us. Else it is fetched. Our last change, not our
open, orders a pair, so of two nodes meeting the one that changed earlier fetches and they
never both assume. It is a comparison, so an age past our uptime never wraps. An observer
assumes nothing, since it admits peers that would not admit it, nor does a core without a
hook, nor one that forgot a peer for room: that peer may hold us and would be a first
contact again. Nor is a plain copy (Owed work and copies) assumed judged, since its sender
holds us already and asks nothing.

A blob past our bound, cut by the OS or bigger than `meta_cap`, leaves the header, which
counts and makes the record. Its fetch stops until a newer version, or a migrate that grew
the bound, which fetches that peer alone.

## The filter

A record judged no match keeps a filter of the keys its blob carries, so a topic of ours that
comes up later fetches only the peers it may meet, not every peer heard.

- The admit hook gets the record's filter, `RANT_DISCOVERY_FILTER_BYTES` (32, a power of two
  from 8 to 64) zeroed, and on NO has folded the blob into it. The core marks the record
  FILTER exactly then, and any other verdict clears the mark, so the filter always belongs
  to the blob judged at the version held.
- `refetch_unmatched(st, maybe, ctx)` fetches a live record judged no match unless its filter
  is current and `maybe` says no. Current means FILTER is set and the peer advertises no
  version past the one judged. A record judged by assumption has no filter, so it is fetched,
  and its answer gives it one. NULL `maybe` fetches every such record.
- The filter only saves sends. A false hit costs a fetch and its answer, judged no match
  again. The node's filter is a Bloom filter with three bits per item (spec/interest.md, The
  want set): about 1% false hits for a peer of 20 items, 20% for 75.
- A node that opened among old peers judged them all by assumption, so its first topic after
  the gather fetches all of them once. After that only candidates are fetched.

## Owed work and copies

A record can be owed three targeted sends, each carrying our blob and ONE. A fetch and a
match are asks, sent with REQ at once, then every 250 ms, four sends:

| owed | sent as | when |
|---|---|---|
| reply | BLOB\|ONE | A solicit marked UNI, ONE or ALL: at once. A group solicit: within one ms per live peer, placed by a hash of both uuids, so a crowd's answers spread |
| fetch | REQ\|ONE\|BLOB | While the peer advertises a version past the one judged, unless that one is too big to hold, and while a slot opened for it or the bound grew. A datagram after the last send starts over, with one send when the last ask went unanswered, as from a peer that refused us. The blob judged ends it, and so does one too big to hold, since a retry brings the same bytes |
| match | REQ\|ONE\|BLOB | A match the peer may not know of (Judging on arrival). Any datagram marked ONE from the peer ends it: it holds our blob, or asks and our reply follows |

An ask carries our blob, so it settles a reply owed too. The records owed work sit on one
due queue in the record store, oldest first, so a poll walks only them. Each send settles
its record or moves it to the tail with a later due time, and a peer that drops is owed
nothing. Nothing goes out before the opening.

A solicit from a peer with no record owes a reply from the loose queue: 8 entries in the
core (`RANT_DISCOVERY_LOOSE`), each a locator and a due time, timed as a record's reply and
sent by `poll_targeted` the same way. A second solicit from a queued locator keeps the
earlier time, and a full queue drops the solicit, since the asker asks again in 250 ms.

Each announce goes by unicast to the seeds marked UNI, to every peer heard by a UNI copy
within the peer timeout marked UNI, and to every other slotted peer plain, which is marked
ONE. A UNI copy asks its receiver to copy back, which keeps two segments that share no group
in touch through one seed. A plain copy never starts the receiver's own copies, so the copies
between two peers end with their slots. A targeted send starts none either. A plain copy goes
only to a slotted peer, so its ONE says what any targeted send does: the sender holds our
blob. That ends a match ask owed to it, and a first contact by one is fetched, never assumed
judged, since its sender asks nothing (a record lost to the gone timeout while the peer kept
us is matched again this way).

## Peer lifecycle

| state | meaning |
|---|---|
| ACTIVE | heard within `peer_timeout_us` (12 s). Flow controlled, its slot never taken |
| DROPPED | silent past the timeout. The record, any slot and `local_id` are kept, `on_peer_seen` fires down and, for a slotted peer, `on_peer_down` with reason `RANT_DISCOVERY_DROP`. The node marks it dormant: out of flow control, proxies and delivery position kept, inbound still processed |
| GONE | freed. A DROPPED peer silent past `gone_timeout_us` (2 min, 0 = never) is promoted on its own. A `BYE` is GONE at once |

The same uuid returning resumes under the same `local_id`: `on_peer_up` fires again and
the subscriber kept its `deliver_upto`, so the writer fills only the gap and replayed
history is deduped (spec/transport.md, peer resume). A new uuid is a fresh peer and gets
`catch_up`. When a match needs a slot, or a newcomer a record, the oldest DROPPED peer is
forgotten (reason `RANT_DISCOVERY_GONE`). A live slot is never taken: a match that finds
every slot live is refused (a `RANT_E_PEER_REFUSED` event). A store full of live records
forgets the newcomer, or for a match the oldest unmatched record (Records and slots).
`timeout_us` is the single tolerance knob. A separate grace timer was rejected as part of
the timeout. One helper fires PEER_DOWN then frees the slot for BYE, both eviction sweeps,
allocation eviction and GONE promotion.

A new uuid announcing from an (ip, port) we already hold evicts the predecessor as GONE
first: one socket is one process, so it is provably dead. A port of 0 is not an endpoint, so
it never triggers the eviction. When a hook
allocation for a bigger blob fails, the stale blob and version are kept and the
advertised version stays ahead, so the fetch retries.

Reflection respects the lifecycle. A DROPPED peer is still listed by
`rant_node_peers_next` and its last known entities are served as a ghost view, but the
mesh fold never counts it, because its cached entities are the dead incarnation's and
would stand beside a restarted live one. The explorer shows only ACTIVE peers.

## Interfaces and locator ranking

Both sockets bind INADDR_ANY unless pinned to loopback, below. Discovery joins the group on every interface and sends each
announce out every one. A per interface join or send failure is ordinary (an adapter with
no multicast, two addresses on one adapter, a membership cap). Open fails only if no
membership takes, then through an INADDR_ANY fallback. Loopback is the fallback for a
loopback only host and never a member of the normal set, or it would duplicate every same
host announce. Auto mode re enumerates every 3 s (`RANT_DISCOVERY_IF_SCAN_US`), so a link
that comes up later is joined and one that goes away drops its membership.
`multicast_interface` pins one interface and freezes the set (its mask is still looked up
so the ranking works). A loopback pin is single host: the data socket binds 127.0.0.1, and
the discovery socket does too on Windows. POSIX stacks hand group traffic only to a socket
bound to INADDR_ANY, so discovery stays there, and no POSIX firewall prompts for it. Linux
would also hand that socket every group another socket on the host joined, another
domain's included, so every join also turns off `IP_MULTICAST_ALL`, and a bridge does the
same for the sockets it lends.

An announce carries no address of its own, so each copy arrives with the source address
correct for its path. A peer heard on several paths has several candidate locators, and
every announce offers one from its source, blob less steady state ones included. The core
ranks them: 169.254 link local scores 0 (checked first, or its own /16 makes every APIPA
address look same subnet), on one of our subnets scores 2, otherwise 1. At equal rank the
incumbent is kept while it was heard within two announce intervals, so a genuine renumber
still converges. The IO layer feeds its subnets in with
`i_rant_discovery_set_local_subnets`. A sans-IO caller that does not gets only the routed
versus link local half of the ranking.

The locator is where we send. What we accept is wider, since a multihomed peer sends from
whichever address its route to our locator picks, which need not be the one we chose for it.
Each peer keeps the sources of its last announces, up to 4 paths. A datagram is the
peer's when it comes from the locator, else from its data port on a path heard within three
announce intervals. A path two peers share attributes to neither.

## Node names

The node name rides every discovery datagram, never a data message, capped at
`RANT_NODE_NAME_MAX` (32) bytes. `RantMsg.publisher_name` is a view into discovery state.
A NULL or empty name becomes `node-` plus eight random hex digits, from the pid when
there is no entropy source.
`RANT_NODE_NAME` in the environment, read through `i_rant_plat_env` at open, replaces the
name the code passed, for every node the process opens. It is a launcher input, never an
API parameter, so a launcher such as the `rant` CLI decides the instance's name and the
code cannot. `RANT_NODE_NAME_PREFIX` then goes with `/` in front of it (spec/node.md, Name
prefix).

## Domains

Each domain announces on its own group, `RANT_DISCOVERY_GROUP_BASE` (239.192.0.0) plus
the domain, so an IGMP snooping switch, the network card's multicast filter and the
kernel drop other domains' announces before Rant reads one. The /16 holds every u16 domain
and stays clear of the top /24 of the scope, which RFC 2365 reserves (SSDP, SLP), and of
239.255, where DDS, Gazebo Transport and LCM put their defaults. The u16 domain still rides
every announce and every uDTL header, and a datagram from another domain is dropped. `RANT_DOMAIN` in the environment, read through `i_rant_plat_env` at
open, sets the domain only when the code left `RantNodeOpts.domain` at 0. So a machine or a
workspace puts every node on one domain, while a tool that picks its domain on purpose,
like a test program or the explorer, keeps it. A value that is not a decimal number from 0
to 65535 refuses the open with `RANT_E_BAD_DOMAIN`, never a silent fall back to 0. An
empty value counts as unset.
