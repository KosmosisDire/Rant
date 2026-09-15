# Interest and matching

How topics match across nodes. The user facing behavior is in docs/topics.md.

## Identity and index

A topic's cross peer identity is its name, hashed with FNV-1a to 64 bits. The local
handle is the topic's index in creation order. Peers match on the identity whatever order
each created its topics in.

Topics are created at runtime against a fixed reserve. The sans-IO core takes them at
init or reserves slots that `rant_topic_define` fills later, which is how
`rant_node_create_topic` works.

## The announce interest list

The announce carries interest only, hash only: `[u16 n]` then one
`[u32 name_hash][u8 flags]` entry per topic in index order. The position IS the
advertiser's index, and an inactive or undefined slot still occupies its position so
later indices stay stable. The hash is the low 32 bits of the identity. Flags: role in
bits 0 and 1, offered or requested reliability in bit 2, the topic kind in bits 3 to 6,
the hole run marker in bit 7 (a run of empty slots, so a big reserve node stops padding
every announce). About 5 bytes per topic. Three sparse sections follow: rate caps,
no timestamp publishers and rebind generations, each 2 bytes when unused, located by one
up front section walk with one truncation policy (a section that does not fully fit is
absent along with everything after it). No names and no schemas ride the announce.

Interest is broadcast state: small, everyone needs it, replayable blind, so it belongs in
discovery's store and repeat model. Details are a per request subset computed from the
requester's index list, so they never belonged there. The old design carried names and
schemas in the announce, and a 2000 topic node truncated its own announce.

The list inlines while the whole datagram fits `RANT_DGRAM_MAX` (about 250 topics at
defaults). Past that the announce ships a fixed size bootstrap (locator, frag, shared
memory host, version and the `INTEREST_EXTERNAL` flag) and peers pull the identical blob
over the unicast `uDTL` channel as byte range pages (`INTEREST_REQ` = 3, `INTEREST_RESP`
= 4). The request body is `[u32 offset]`. The response body is
`[u32 total][u32 offset][u16 chunk_len][chunk]`. The responder is stateless: it rebuilds
the blob into its detail buffer after 24 bytes of headroom and writes the page header
just before the chunk. The requester keeps one cursor per peer (`interest_version` is the
last assembled version and the dedup), pages are reply clocked, the detail rearm sweep is
the loss backstop, and a version stamp change mid fetch restarts at 0. The assembled
blob feeds the unchanged interest apply. A match wait treats a fetch in flight as still
resolving, so a permanent poisoning by an unresolvable peer is impossible.

Rule: every consumer of a peer's interest reads through `interest_of` or
`rant_node_peer_interest_next`, never `rant_meta_interest(v.meta)` directly. The
reflection walk once read the blob directly and showed zero entities for an external
peer. Observers key cached reflection walks on the per peer interest epoch, which bumps
at the one funnel for inline apply, external assembly and fresh verdicts.

## Nomination and verification

A 32 bit hash overlap only NOMINATES a candidate. Names and schemas travel by pairwise
detail exchange on the unicast data socket, a third datagram family `uDTL` beside `uDSC`
and the transport's. The requester batches one `DETAIL_REQ` per peer, each entry carrying
its own schema hash. The responder answers statelessly to the request's source (which is
why an explorer that is not even a peer can be answered) with each topic's name, its
attrs byte (`RANT_ATTR_*`: no timestamp, multi, forceable, cancellable, exclusive), its
schema hash, and the schema wire only where the two hashes differ.

Verdicts run at intake: name equality against the full 64 bit identity first (a same hash
different name peer fires `RANT_E_NAME_COLLISION` and is refused), then the schema gate
per direction. Both sides need the other's facts, whoever lacks them requests, and
simultaneous requests are harmless. Nobody tells the publisher it wants to match: once
both hold the same facts they run the same deterministic gates and form the match
independently, which is what keeps crossing and replay safe.

Verified verdicts are cached per (peer, index) for the peer's incarnation, not per meta
version, because an index's name and schema are immutable while it stays bound and per
version invalidation would drop live lanes for one round trip on every role flip. Slot
reuse, peer removal and GONE clear them. Dormant and resume keep them. Every later
interest apply re derives matches from verdict plus current flags instantly. An
unverified candidate stays PENDING: no proxy, no data, no demux. A false hash overlap costs
one request datagram, not a lane.

The gotcha that cache caused: a DISSOLVED verdict (details arrived, no local topic
matched) is judged against the local topic set, which grows. A `fetch_details` observer
dissolves every index the moment a peer appears, so a later subscribe created the topic
but the cached non match made apply skip it forever. `rant_transport_topic_define` sends
every dissolved verdict of every peer back to pending (name bound verdicts stay), and
create's replay re requests them. Cleared indices are only re fetched if the hash matches
a local topic. Re pending must not clear cached peer attrs: an observer never re asks a
fetched index, and attrs are invalidated only by the peer's generation gate.

Retry is requester driven. Pending state re asks on the peer's announces and on a
periodic sweep, so lost datagrams heal within an announce interval with no acks and
nobody storing requests. A response bigger than one datagram truncates at an entry
boundary and the requester re asks for the rest. Within one entry a large differing
schema still inlines whole.

Observer mode (`opts.fetch_details`) makes the normal detail cycle request every
advertised index of every peer and cache (peer, index) to name, schema hash and interned
schema. INACTIVE entries must be skipped or the responder's skip re requests forever.
`rant_node_peer_topic_name` and `rant_node_peer_topic_schema` return that node owned
data. The explorer, the bridge and the C++ wrapper use this rather than sniffing blobs.

Rejected at design time: a cuckoo filter (membership only, so a fetch per prospective
match), metadata over a reliable topic. An app level introspection channel must never
become load bearing for matching. `@rant/meta` is that facility, kept out of the
matching path.

## The demux table

The data path never carries the topic name. The receiver builds a per peer table from
(peer, index) to our topic. Only name verified indices map, so no unverified index can
ever demux. The data path carries the 2 byte index. Each peer's index and verdict maps
are allocated at its announced entry count, about 3 bytes per entry, so there is no index
ceiling. A failed map allocation surfaces as `RANT_E_INTEREST_OVERFLOW`. The maps bind a
name to ONE local topic, preferring the oldest active one.

## Matching rules

Every interest apply applies the RxO rule: offered reliability must be at least the
requested one. A best effort publisher never serves a reliable subscriber. That match is
refused with `RANT_E_QOS_INCOMPATIBLE` and forms on its own if the publisher upgrades. On
the allowed downgrade the writer reads the reader's requested reliability from the sub
entry's flags and keeps a best effort reader out of flow control.

A role flip via `rant_topic_set_role` re advertises at once. A (re)subscribe joins like a
late joiner and gets `catch_up` cached messages.

Delivery is WRITER AUTHORITATIVE. Our verdict alone forms the lane. A sample committed
before the peer verifies us heals through reliable repair: the reader drops the
unverified DATA, then NACKs it after its own verdict lands. Pinned by a fault injection
that swallows detail responses to one port.

## Slot lifecycle: retire and reuse

`rant_topic_retire` parks the slot (role INACTIVE plus a retired flag), releases every
lane, frees the history ring, sample buffers, consumer queue, node schema copy and the
handle. The slot keeps identity, name, kind, qos, generation and next seqno for the reuse
compare, is announced as a hole run, and is invisible to identity lookups and detail
answers. The node core keeps the schema hash as the binding fingerprint. The next create
first scans for a retired slot (same identity and kind first, else any), so churn never
grows the table, the announce or steady state memory. The `churn:` selftests assert byte
exact plateaus.

An identical re creation (same name, kind and schema hash) relinks with nothing changed:
peers' cached verdicts stay valid and the slot's seqno line continues. A changed re
creation bumps the slot's u8 generation, sets `rebind_version` to the announce version the
advertise will stamp, severs our own maps bound to the slot, and purges the node core's
binds for the topic. The generation rides the third sparse section
`[u16 n][(u16 index)(u8 gen)]*`. Apply compares the announced generation against the one
stored per (peer, entry), re pends the verdict and severs the map on mismatch, and always
updates the stored generation (before the INACTIVE skip, so parked entries track too). A
verdicted entry whose hash no longer matches its bound topic also re pends.

Until a peer's own `DETAIL_REQ` names our blob at or past the rebind's version (proof it
applied the announce that severed its old binding), the writer lane to it stays under the
REBIND HOLD. An `INTEREST_REQ` does not count, since it names the version being fetched.
The match wait counts held peers as still resolving. Stale detail responses (a meta
version older than the blob we hold for the peer) are refused, since they may describe
the dead binding. Delivery map entries for identical hash publishers store a sentinel
resolved at read time, because the compiled copy is freed and re parsed across retire and
reuse.

A reader that re incarnates while the far writer's lane survived (an identical reuse seen
by a peer that coalesced the announces) is RESUMED, not late joined. The writer re joins
at the older of the fresh match join point and its acked floor, so a sample committed
inside the re create window is re pushed. A genuine unsubscribe and resubscribe keeps
late joiner semantics (its announce change re forms the writer proxy, epoch 0).

`rant_transport_topic_seqno` is the slot's next write seqno. It continues across retire
and reuse so a successor can seed its counters above everything its predecessor
published. Gotchas: destroy must skip NULL history (retired slots freed theirs), and an
untyped reuse over a typed retired slot must still clear the schema or the stale
fingerprint is served to peers.

## The match wait

Matching a fresh topic to a present peer costs an announce and detail round trip after
create. A send in that window would commit to zero subscribers. The node closes the
window: discovery solicits at open, create's replay re fires peer up so detail requests
queue and the kick sends them, and a send that would reach zero subscribers WHILE a
candidate is resolving blocks, bounded by `opts.match_wait_ms`. Resolving means a cached
announce nominates this topic but its verdicts are in flight, or the post open gather
has not settled. The gather latch is set opportunistically each poll pass so a later
create's own replay cannot reset the quiet clock. A topic that retains history (reliable
with `catch_up` above 0) is exempt. A topic nobody advertises interest in proceeds at
once, and the converged state is memoized per topology epoch (bumped on PEER_UP, PEER_DOWN,
PEER_INTEREST, create and set role) so steady state costs one compare. Entries with no
verdict storage are not counted, since they never resolve. A wait that cannot happen
(disabled, or a reentrant send) or that times out proceeds and fires
`RANT_E_UNMATCHED_SEND`. `rant_topic_ready` shares the exact predicate. A variable
accessor's first write rides the same wait without the send and without the error. The
`matchwait:` selftests pin it.

Rejected: eager details as the default (the per peer name and schema cache is the full
table cost hash only announces exist to avoid), core level TTL queueing, a blocking open,
a blocking create (100 creates would be 100 serialized round trips).

## Sans-IO surface

A sans-IO caller runs the exchange itself: `rant_transport_build_interest` and
`rant_transport_apply_peer_interest` for announces, the detail codec
(`rant_transport_detail_wants`, `rant_detail_req_build`, `rant_transport_detail_respond`,
`rant_transport_apply_peer_details`), and the paging codec (`rant_interest_req_build`,
`rant_interest_resp_head`, `rant_interest_resp_parse`, `rant_transport_interest_size`).
The interest codec measures and builds in one walk (a NULL out means measure). The node
wires all of it into discovery and its data socket. The transport's linear hash to topic
scans run only per PENDING entry and cost about 25 us per announce at 1600 by 1600. A
hash index is the next step only if thousands of pending entries show up in one pass.

## Wire layouts

The announce overlay is `['D','N'][ver][u16 frag][u8 shm][host 16][u8 iflags][interest]`.
The version byte is 21 with SHM compiled in and 20 without, and the even form has no shm
byte and no host. iflags bit 0 is INTEREST_EXTERNAL. A blob with another magic or version
is rejected, never reinterpreted.

The interest blob is `[u16 n]` then one `[u32 hash][u8 flags]` cell per slot up to the
highest announced one, where a run of undefined or retired slots collapses to one cell
flagged with bit 7 whose hash field is the run length. Then the rate section
`[u16 n][(u16 index)(u16 rate_hz)]*` and the generation section
`[u16 n][(u16 index)(u8 gen)]*`. `rant_interest_max` bounds it at 5 bytes per slot plus
both sections at every topic.

Every uDTL datagram starts with `['u','D','T','L'][u8 kind][u8 ver 2][u16 domain]
[u32 meta_version][u16 n]`. A DETAIL_REQ entry is `[u16 index][u64 schema_hash]`, the
requester's hash for its matching topic, and a DETAIL_RESP entry is
`[u16 index][u8 attrs][u8 namelen][name][u64 schema_hash][u16 wire_len][wire]`. The
interest paging kinds carry n = 0 and the bodies given above.

## Kinds and twins

One node cannot define the same name twice while both would be live, nor under two
kinds at all. The per peer maps bind an entry to one local topic by identity, so a live
twin would be shadowed and a cross kind twin would cross bind and refuse forever. The
define returns -2 and the node records `RANT_E_NAME_COLLISION` with `.peer` 0. Across
nodes a name verified peer that advertises the name under another kind is refused with
`RANT_E_KIND_MISMATCH`, never cross wired. Retired slots are exempt from the local check,
and so is a twin on either side that is `RANT_INACTIVE`.

A node may hold two topics of one identity with different QoS and switch which is live by
role. Identity lookup prefers the non INACTIVE one, and a verdict bound to the parked twin
re pends when the other goes live, so its schema gates are re judged against the live one.
