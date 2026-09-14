# Discovery

Nodes find each other with a UDP multicast announce every 3 s
(`opts.discovery.announce_interval_us`). The announce carries the node's name, its data
port, its fragment size, a host id for shared memory and its interest list. Data never
uses multicast. spec/discovery.md explains how announces, peer state and relaying work
inside.

## Peer lifecycle

A peer heard within `peer_timeout_us` (default 12 s) is ACTIVE. On silence past that it
is DROPPED: `RAMBLE_PEER_DOWN` fires and the peer leaves flow control, but its transport
state is kept. If the same node returns it resumes where it left off: `RAMBLE_PEER_UP`
fires again, the subscriber keeps its position and the writer fills only the gap. A
restarted node is a new peer and gets `catch_up`. A dropped peer silent for 2 minutes is
GONE and its slot is freed. When a new peer needs a slot the oldest dropped one is
evicted. A table full of active peers refuses a new one with `RAMBLE_E_PEER_REFUSED`, which
means raise `opts.discovery.max_peers` (default 16).

## Interfaces

Both sockets bind every interface. Discovery joins the group on every interface the host
has and sends each announce out all of them, so multihomed hosts, VPN adapters, WSL and
docker bridges need no configuration. Interfaces are re scanned every 3 s.
`opts.net.multicast_interface` pins discovery to one interface. `"127.0.0.1"` isolates a
run from the LAN.

A peer heard on several paths is reached at the best ranked address: one on our own
subnet first, then any routed address, then link local. At equal rank the address in use
stays until it goes quiet, so a peer's address never flaps.

## No multicast at all

A node whose network gives it no multicast sets `opts.net.unicast_only`. It joins no group
and announces only to `opts.net.seed_peers` and to peers it already knows. Any node that
hears it directly re announces it on its behalf, one hop, so seeding one reachable node
makes it discoverable mesh wide. The relay only introduces. Data is always direct, and
once two nodes know each other they keep each other alive without it.

The same flag covers a node behind an outbound only NAT, such as a rootless container.
Peers reply to the address an announce actually arrived from, and relays introduce the
NAT'd node to every peer they hear directly so it can speak first. Two NAT'd nodes cannot
reach each other.

`seed_peers` also helps a normal node where multicast is broken between two segments.

## Stating our own address

By default a node announces no address and every peer records the source an announce
arrived from. `opts.net.self_ip` and `opts.net.advertise_port` override that with an
address peers treat as authoritative. Use them when the address peers must reach us at
is not the one our packets appear to come from and the mapping is static and one to one:
a cloud elastic IP, or a container published with `-p 7400:7400`. State the port whenever
it is translated. One address goes to every peer. Neither option creates an inbound path.
An unparseable address refuses the open with `RAMBLE_E_BAD_ADDRESS`. A VLAN does not
translate addresses, so it needs only the relay above.

## Network options

| option | meaning |
|---|---|
| `data_port` | the unicast data port. 0 = OS assigned |
| `discovery_group`, `discovery_port` | the announce group and port (239.255.0.7, 7400) |
| `multicast_interface` | pin discovery to one interface IP. NULL = all |
| `multicast_ttl` | hops an announce may travel. 1 = same subnet |
| `seed_peers`, `n_seed_peers` | addresses to also unicast announces to. Port 0 = the discovery port |
| `unicast_only` | join no group. Rely on seeds and relays |
| `recv_buffer_bytes`, `send_buffer_bytes` | data socket OS buffers. 0 = OS default |
| `fragment_size` | UDP payload bytes per fragment. 0 = `RAMBLE_FRAG_SIZE`. One size per node |
| `self_ip`, `advertise_port` | state our own locator |

`opts.discovery` holds `announce_interval_us` (3 s), `peer_timeout_us` (12 s) and
`max_peers` (16).

A message has no flow control inside it, so a fragment the receive buffer cannot hold is
dropped and repair carries it instead. Raise `recv_buffer_bytes` to at least the largest
message a topic carries once payloads reach a megabyte. Linux also needs
`net.core.rmem_max` raised to that size or the request is silently clamped. Every binding
takes both under its own name.

## Peer information

`ramble_node_peers_next` walks the peers with a `RamblePeerInfo`: id, uuid, name, address,
liveness, last heard time, an epoch that bumps on every change, fragment size, and the
measured round trip (`rtt_us`, `rtt_jitter_us`, `rtt_min_us`, `rtt_samples`). A dropped
peer is still listed. Gate on `.liveness`. See docs/reflection.md.
