# Discovery

Nodes find each other with a UDP multicast announce every 3 s
(`opts.discovery.announce_interval_us`). Each domain announces on its own group,
239.192.<domain high byte>.<domain low byte>, so the switch, the network card and the OS
can drop other domains' announces. The announce carries the node's name, its data port,
its fragment size, a host id for shared memory and its interest list. Data never uses
multicast. spec/discovery.md explains how announces and peer state work inside.

## Peer lifecycle

A peer heard within `peer_timeout_us` (default 12 s) is ACTIVE. On silence past that it
is DROPPED: `RANT_PEER_DOWN` fires and the peer leaves flow control, but its transport
state is kept. If the same node returns it resumes where it left off: `RANT_PEER_UP`
fires again, the subscriber keeps its position and the writer fills only the gap. A
restarted node is a new peer and gets `catch_up`. A dropped peer silent for 2 minutes is
GONE and its slot is freed. When a new peer needs a slot the oldest dropped one is
evicted. A table full of active peers refuses a new one with `RANT_E_PEER_REFUSED`, which
means raise `opts.discovery.max_peers` (default 16).

## Interfaces

Both sockets bind every interface, except in single host mode below. Discovery joins the
group on every interface the host has and sends each announce out all of them, so
multihomed hosts, VPN adapters, WSL and docker bridges need no configuration. Interfaces
are re scanned every 3 s.
`opts.net.multicast_interface` pins discovery to one interface. `"127.0.0.1"` is single
host: the data socket binds loopback, and so does discovery on Windows, so nothing listens on
the network and the Windows firewall never asks.

A peer heard on several paths is reached at the best ranked address: one on our own
subnet first, then any routed address, then link local. At equal rank the address in use
stays until it goes quiet, so a peer's address never flaps.

## No multicast or behind a NAT

A node whose network gives it no multicast, or that sits behind a NAT such as a rootless
container or a cloud host, runs on a bridge's sockets (Through a bridge). Peers reach a
node at the address its announces arrive from.

`opts.net.seed_peers` also sends our announces by unicast, for two segments that multicast
does not cross. A seeded node learns us and answers, but its other peers do not.

## Through a bridge

A node can run on the UDP sockets of another host: a Rant bridge (`rant bridge enable` on
that host) lends them over one WebSocket, and to the mesh the node is a node of that host.
It reaches a mesh on another network, or one its own host cannot join.

```c
RantNode *n = rant_node_open(&mem, "arm", NULL, NULL, &(RantNodeOpts){
    .net = { .bridge = "ws://192.168.1.20:7480", .bridge_passphrase = "secret" } });
```

- `RANT_BRIDGE` and `RANT_BRIDGE_PASSPHRASE` set them for a node whose code leaves them
  NULL. An empty `bridge` means this host's own sockets whatever the environment says.
- The open connects first and fails with `RANT_E_BRIDGE` when the bridge cannot be reached
  or refuses, `.detail` saying why, such as `wrong passphrase`.
- A dropped link fires `RANT_E_BRIDGE` and reconnects on its own. A restarted bridge gets
  the node's sockets bound again on the same ports, so peers keep reaching it.
- `ws://` only: the library carries no TLS. Shared memory is off, since its peers are on
  the bridge's host.
- The other network options apply on the bridge's host: `multicast_interface` names one
  of its interfaces, `127.0.0.1` its loopback.

## Network options

| option | meaning |
|---|---|
| `data_port` | the unicast data port. 0 = OS assigned |
| `discovery_port` | the announce port (7400) |
| `multicast_interface` | pin discovery to one interface IP. NULL = all |
| `multicast_ttl` | hops an announce may travel. 1 = same subnet |
| `seed_peers`, `n_seed_peers` | addresses to also unicast announces to. Port 0 = the discovery port |
| `recv_buffer_bytes`, `send_buffer_bytes` | data socket OS buffers. 0 = OS default |
| `fragment_size` | UDP payload bytes per fragment. 0 = `RANT_FRAG_SIZE`. One size per node |
| `bridge`, `bridge_passphrase` | run on a bridge's sockets (Through a bridge) |

`opts.discovery` holds `announce_interval_us` (3 s), `peer_timeout_us` (12 s) and
`max_peers` (16).

A message has no flow control inside it, so a fragment the receive buffer cannot hold is
dropped and repair carries it instead. Raise `recv_buffer_bytes` to at least the largest
message a topic carries once payloads reach a megabyte. Linux also needs
`net.core.rmem_max` raised to that size or the request is silently clamped. Every binding
takes both under its own name.

## Peer information

`rant_node_peers_next` walks the peers with a `RantPeerInfo`: id, uuid, name, address,
liveness, last heard time, an epoch that bumps on every change, fragment size, and the
measured round trip (`rtt_us`, `rtt_jitter_us`, `rtt_min_us`, `rtt_samples`). A dropped
peer is still listed. Gate on `.liveness`. See docs/reflection.md.
