# Discovery

How nodes find each other and how a peer's address and liveness are tracked. The user
facing options are in docs/discovery.md.

## The announce

Every node multicasts an announce every `announce_interval_us` (3 s) on its domain's
group, and unicasts a copy to every peer it knows and to its seed peers. The announce
carries the node's uuid, its name, and an opaque versioned meta blob: the data port, the
fragment size, the shared memory host id, the protocol version and the interest list
(spec/interest.md).

The blob is full state and idempotent, so it is simply re sent on change. It rides the
startup solicit, the first `RAMBLE_DISCOVERY_BLOB_RESEND` (3) announces after a change, and
unicast replies. Every announce carries the blob's version. A steady state announce
without the blob but with a newer version makes the peer solicit the blob with a targeted
request. So a connection forms from the announces the nodes already send, with no per
topic handshake and no steady state metadata heartbeat. `on_peer_up` fires only on a
change (first contact, address, blob, revive), never per announce.

Every datagram stays under one MTU. Nothing ever relies on IP reassembly, because lwIP
class stacks often cannot reassemble and a dropped fragmented announce would leave the
peer unresolvable forever. When the interest list no longer fits, the announce ships a
fixed size bootstrap instead and peers page the interest blob over unicast
(spec/interest.md).

The accept bound (the largest peer blob stored, first sized from our own topic count)
self heals. A bigger peer's announce, even one the OS truncated, fires
`RAMBLE_E_PEER_META_TOO_BIG` internally. The node grows the bound and its receive buffers
at the next poll and solicits a re announce. Per peer blobs are hook allocated at their
actual length and reused across occupants. The event carries the overlay when the
datagram arrived whole and a NULL data pointer when the OS truncated it.

Unicast discovery traffic leaves the DATA socket (`ramble_discovery_set_tx_fd`). Group sends
stay on the multicast socket. So each announce is identity plus return path in one
datagram. Per peer unicast goes to one port: the peer's data port once its blob named
one, else the discovery port. A discovery only instance (no transport socket, such as the
explorer's capture layer) binds its own unicast receive socket on an ephemeral port and
advertises it, because sharing the discovery port with same host nodes handed unicast
replies to an arbitrary socket.

## Wire format

Every discovery datagram starts with a 24 byte header: the magic `uDSC`, the protocol
version (`RAMBLE_DISCOVERY_PROTO_VERSION` = 5), a flags byte, the u16 domain and the
sender's 16 byte uuid. Then `[u32 meta_version][u16 meta_len]` and the blob. The blob is
the discovery section `[u16 data_port][u8 self_ip_len][self_ip][u8 name_len][name]`
followed by the opaque overlay. A steady state announce carries meta_len 0 and only the
version. The flags are BYE 0x01, REQ 0x02 (a solicit, recipients announce back now),
RELAY_ME 0x04 and PROXIED 0x08. A proxied announce appends the relayer's uuid as a 16
byte trailer after the blob, which meta_len excludes and parsers otherwise ignore. Version
history: v3 added the versioned blob, v4 the relay flags, v5 observed sources. A datagram
with another magic, version or domain is dropped, as is a malformed blob. The per peer
overlay capacity defaults to `RAMBLE_DISCOVERY_META_MAX` (64) bytes and the name to
`RAMBLE_DISCOVERY_NAME_MAX` (32).

## Timing

The first announce is phased by a hash of the uuid within one interval, so nodes do not
announce in lockstep, and a solicit goes out at startup. Peer timeout sweeps run on the
announce cadence, so detection lags by at most one interval. `ramble_discovery_next_due_us`
tells a driving loop when the next timer is due. The core defaults are a 3 s interval, a
12 s peer timeout, a 2 min gone timeout and 32 peers (the node's default is 16). A BYE is
sent three times (`RAMBLE_DISCOVERY_BYE_SENDS`), since one shot UDP may lose it and
receivers dedup by uuid. The runtime drains a socket to empty on every pass, capped at
2048 datagrams. One receive per poll would let a slow poller's backlog keep a dead peer
alive. `ramble_discovery_gather` re solicits four times a second until the peer set has been
quiet for the asked time. Multicast loop stays on, since several instances on one host
need it, and the uuid self filter drops the echoes.

## Peer lifecycle

| state | meaning |
|---|---|
| ACTIVE | heard within `peer_timeout_us` (12 s). Flow controlled, never evicted |
| DROPPED | silent past the timeout. The slot and `local_id` are kept, `on_peer_down` fires with reason `RAMBLE_DISCOVERY_DROP`, and the node marks it dormant: out of flow control, proxies and delivery position kept, inbound still processed |
| GONE | freed. A DROPPED peer silent past `gone_timeout_us` (2 min, 0 = never) is promoted on its own. A `BYE` is GONE at once |

The same uuid returning resumes under the same `local_id`: `on_peer_up` fires again and
the subscriber kept its `deliver_upto`, so the writer fills only the gap and replayed
history is deduped (spec/transport.md, peer resume). A new uuid is a fresh peer and gets
`catch_up`. When a new peer needs a slot the oldest DROPPED peer is evicted (reason
`RAMBLE_DISCOVERY_GONE`). A table full of ACTIVE peers refuses the newcomer
(`on_peer_refused`, `RAMBLE_E_PEER_REFUSED`) rather than evicting a live one.
`timeout_us` is the single tolerance knob. A separate grace timer was rejected as part of
the timeout. One helper fires PEER_DOWN then frees the slot for BYE, both eviction sweeps,
allocation eviction and GONE promotion.

A new uuid announcing from an (ip, port) we already hold evicts the predecessor as GONE
first: one socket is one process, so it is provably dead. A port of 0 (a peer first seen
through a blob less announce) is not an endpoint, so it never triggers the eviction, or
several same host instances awaiting their blobs would evict each other. When a hook
allocation for a bigger blob fails, the stale blob and version are kept and the
advertised version stays ahead, so the re fetch path retries.

Reflection respects the lifecycle. A DROPPED peer is still listed by
`ramble_node_peers_next` and its last known entities are served as a ghost view, but the
mesh fold never counts it, because its cached entities are the dead incarnation's and
would stand beside a restarted live one. The explorer shows only ACTIVE peers.

## Interfaces and locator ranking

Both sockets bind INADDR_ANY. Discovery joins the group on every interface and sends each
announce out every one. A per interface join or send failure is ordinary (an adapter with
no multicast, two addresses on one adapter, a membership cap). Open fails only if no
membership takes, then through an INADDR_ANY fallback. Loopback is the fallback for a
loopback only host and never a member of the normal set, or it would duplicate every same
host announce. Auto mode re enumerates every 3 s (`RAMBLE_DISCOVERY_IF_SCAN_US`), so a link
that comes up later is joined and one that goes away drops its membership.
`multicast_interface` pins one interface and freezes the set (its mask is still looked up
so the ranking works).

An announce carries no address of its own, so each copy arrives with the source address
correct for its path. A peer heard on several paths has several candidate locators, and
every announce offers one from its source, blob less steady state ones included. The core
ranks them: 169.254 link local scores 0 (checked first, or its own /16 makes every APIPA
address look same subnet), on one of our subnets scores 2, otherwise 1. At equal rank the
incumbent is kept while it was heard within two announce intervals, so a genuine renumber
still converges. The IO layer feeds its subnets in with
`ramble_discovery_set_local_subnets`. A sans-IO caller that does not gets only the routed
versus link local half of the ranking.

## Stated locators

`self_ip` and `advertise_port` put a locator in the announce that peers treat as
authoritative. It skips the ranking, and a relay proxies the stated address rather than
the source it saw. One locator goes to every peer. An unparseable address refuses the
open with `RAMBLE_E_BAD_ADDRESS`.

## Relaying for unicast only nodes

A `unicast_only` node joins no group, announces only to seed peers and known peers, and
marks every announce `RELAY_ME`. It also skips the join, the group sends and the ttl and
loop options, so a device with no multicast can open at all. A node that hears a
`RELAY_ME` announce DIRECTLY re announces it: `ramble_discovery_poll_relay` builds an
announce on behalf of that peer (its uuid, its blob at the origin's version, and the
locator we know it at stated outright) and the IO layer sends it out every path. This is
one hop and the relay only introduces. Data is unicast point to point as always, and once
two nodes know each other they keep each other alive directly, so the relay may die.

Three rules keep it safe:

- `PROXIED` is the loop stop. A proxied announce never enlists a second hop. Only a direct
  announce enlists a relay. Proxies carry the origin's `RELAY_ME` as information only.
- A proxied locator is a ranked candidate, never authoritative, so a relay's view cannot
  stomp or flap a direct path we still hear, which would re fire peer up forever.
- Relaying and introducing are sustained by DIRECT liveness only (`last_direct_us`). Every
  relay hears the other relays' proxies and its own multicast looped echoes (a proxy
  carries the origin's uuid, so the self filter never drops it). If those sustained
  relaying, a dead origin would be re announced forever, mesh wide. Proxies still refresh
  plain liveness for third parties, so on real silence every relay stops within one
  `peer_timeout` and the mesh forgets the peer within about two.

The cost is one proxy per relayed peer per announce interval, duplicate suppressed mesh
wide: two or more foreign proxies for an origin within one interval mean a quorum exists,
so a relay sits that interval out. Steady state is two or three active relays whatever
the mesh size. A new or changed origin is always emitted at once. A relay tells foreign
proxies from its own echoes by the 16 byte relayer uuid trailer that `build_proxy`
appends after the blob (`meta_len` excludes it, and a trailer less proxy counts as
foreign). A proxy is never unicast back to its own origin.

## Observed sources (NAT)

A `unicast_only` node behind an outbound only NAT (rootless containers, slirp class
stacks) has a fictional locator and one rewritten source per flow. Three mechanisms cover
it with no new wire kinds.

1. Every node sends unicast discovery from its data socket, so each announce opens,
   identifies and keeps alive exactly the NAT mapping that peer needs. The 3 s cadence
   beats common UDP mapping timeouts. A NAT'd peer that replies to an announce's ARRIVAL
   source reaches the one socket that demuxes every datagram family.
2. Peers bind one OBSERVED source per arrival channel (`obs_disc`, `obs_data`) from direct
   `RELAY_ME` announces, keyed by uuid, with `RambleDiscoveryVia` saying which local socket.
   A unicast only node binds them for EVERY direct peer, because everything reaching it
   came through its own NAT: a published port forward rewrites the source to the NAT's
   internal gateway, which is the only endpoint replies are known to reach. For the same
   reason a unicast only node never lets an arrival source override a locator it already
   holds. The gateway source sits on the container's own subnet, so it would win the
   ranking and black hole every send to that peer. `ramble_discovery_addr_of_id` (the
   node's one data and detail destination) returns `obs_data`, then `obs_disc`, then the
   locator. Discovery TX targets `obs_disc`, then `obs_data`, then the locator, exactly,
   never expanded to conventional ports. Attribution (`id_for_addr`) accepts observed
   sources only for peers that have one and refuses the phantom locator, which two NAT'd
   containers on one device even share. A `RELAY_ME` locator asserts no endpoint, so
   neither evicts the other. A new uuid arriving from a held observed endpoint does evict.
   A changed source rebinds quietly. A `self_ip` stated in a DIRECT announce stays
   authoritative and clears observed sources. A proxy always embeds a locator and must
   never set the stated flag.
3. The NAT'd side must speak first, so relays also run introductions
   (`ramble_discovery_poll_introduce`): each relay me peer is sent proxied announces of the
   peers we hear directly (the `heard_direct` flag, one hop). This is change triggered
   (once when it appears, again when any direct peer appears, resumes or moves, plus a
   repair sweep every `RAMBLE_DISCOVERY_INTRODUCE_SWEEP` = 10 announce intervals), never
   periodic. The NAT'd node then contacts each introduced peer itself and the pair self
   sustains.

Two NAT'd nodes are out of scope. Neither can receive first, so their matches stay
pending. The `nat:` selftests pin these rules: `disc_core_checks` sections 10 to 13 cover
observed binding, eviction, introductions, the ghost gate, the published port forward and
relay suppression, and the e2e phase has the NAT'd node advertise a black holed port yet
carry reliable data that outlives the introducer.

## Node names

The node name rides the announce blob once, never per message, capped at
`RAMBLE_NODE_NAME_MAX` (32) bytes. `RambleMsg.publisher_name` is a view into discovery state.
A NULL or empty name becomes `node-` plus eight random hex digits, from the pid when
there is no entropy source.
