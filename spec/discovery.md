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
startup solicit, the first `RANT_DISCOVERY_BLOB_RESEND` (3) announces after a change, and
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
`RANT_E_PEER_META_TOO_BIG` internally. The node grows the bound and its receive buffers
at the next poll and solicits a re announce. Per peer blobs are hook allocated at their
actual length and reused across occupants. The event carries the overlay when the
datagram arrived whole and a NULL data pointer when the OS truncated it.

Unicast discovery traffic leaves the DATA socket (`i_rant_discovery_set_tx_fd`). Group sends
stay on the multicast socket. So all unicast between two nodes uses one port pair, and each
announce is identity plus return path in one datagram. Per peer unicast goes to one port: the peer's data port once its blob named
one, else the discovery port. Discovery always runs inside a node, so the locator it
advertises is the node's data socket and it owns no unicast socket of its own.

## Wire format

Every discovery datagram starts with a 24 byte header: the magic `uDSC`, the wire version
(`RANT_WIRE_VERSION` = 6), a flags byte, the u16 domain and the sender's 16 byte uuid. Then `[u32 meta_version][u16 meta_len]` and the blob. The blob is
the discovery section `[u16 data_port][u8 name_len][name]`
followed by the opaque overlay. A steady state announce carries meta_len 0 and only the
version. The flags are BYE 0x01 and REQ 0x02 (a solicit, recipients announce back now).
Version history: v3 added the versioned blob, v4 and v5 relaying, which v6 removed. Since
v5 the number versions everything that crosses the wire, discovery or not, and any change to it
bumps the number. The first eight bytes (magic, version, flags, domain) keep their place in
every version, so a datagram of this domain on another version is recognized from them
alone: it is dropped and its source reported once with `RANT_E_WIRE_VERSION`, the last 8
sources remembered. A datagram with another magic or domain is dropped silently, as is a
malformed blob. The per peer
overlay capacity defaults to `RANT_DISCOVERY_META_MAX` (64) bytes and the name to
`RANT_NODE_NAME_MAX` (32).

## Timing

The first announce is phased by a hash of the uuid within one interval, so nodes do not
announce in lockstep, and a solicit goes out at startup. Peer timeout sweeps run on the
announce cadence, so detection lags by at most one interval. `i_rant_discovery_next_due_us`
tells a driving loop when the next timer is due. The defaults are a 3 s interval, a 12 s
peer timeout, a 2 min gone timeout and 16 peers. The interval, the peer count, the group
and the port are stated once in `common/types.h`. A BYE is sent three times
(`RANT_DISCOVERY_BYE_SENDS`), since one shot UDP may lose it and receivers dedup by uuid. The runtime drains a socket to empty on every pass, capped at
2048 datagrams. One receive per poll would let a slow poller's backlog keep a dead peer
alive. Multicast loop stays on, since several instances on one host
need it, and the uuid self filter drops the echoes.

## Peer lifecycle

| state | meaning |
|---|---|
| ACTIVE | heard within `peer_timeout_us` (12 s). Flow controlled, never evicted |
| DROPPED | silent past the timeout. The slot and `local_id` are kept, `on_peer_down` fires with reason `RANT_DISCOVERY_DROP`, and the node marks it dormant: out of flow control, proxies and delivery position kept, inbound still processed |
| GONE | freed. A DROPPED peer silent past `gone_timeout_us` (2 min, 0 = never) is promoted on its own. A `BYE` is GONE at once |

The same uuid returning resumes under the same `local_id`: `on_peer_up` fires again and
the subscriber kept its `deliver_upto`, so the writer fills only the gap and replayed
history is deduped (spec/transport.md, peer resume). A new uuid is a fresh peer and gets
`catch_up`. When a new peer needs a slot the oldest DROPPED peer is evicted (reason
`RANT_DISCOVERY_GONE`). A table full of ACTIVE peers refuses the newcomer
(a `RANT_E_PEER_REFUSED` event) rather than evicting a live one.
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

The node name rides the announce blob once, never per message, capped at
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
