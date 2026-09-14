# Layers and transports

Ramble separates what decides which bytes move from what moves them. The sans-IO cores
decide. A runtime moves the bytes. Adding a new transport (serial, Bluetooth, a second
link) is a new runtime over the same cores with no change to any sans-IO file.

```
  sans-IO cores  (never touched to add a transport)
    transport core   reliability, fragmentation, history, NACK repair
    discovery core   peer table and announce state machine
    node core        peer id to physical address, discovery to transport lifecycle

  platform           the one OS layer: sockets, clock, entropy, route probe, SHM map

  runtimes           own the sockets and the clock and drive the cores
    discovery runtime
    node runtime      the public ramble_node_* API (UDP today)
```

A core never calls the platform. A runtime is the only thing that does IO.

## Layer rules

Discovery is generic peer discovery, liftable into a non pub sub system. It carries an
opaque meta blob and knows nothing of transport or node. The transport knows nothing of
discovery or node. The node is the only layer that combines them. Two attempts were
rejected as coupling and are not to be retried: a shared `RambleEvent` in `common/` (it
drags transport kinds into a generic discovery), and moving the blob parsers into
discovery. Each layer owns its own event type and the node maps between them. In the
combined header all enum constants share one scope, so the three event enums use distinct
prefixes.

The peer table lives in discovery, not in a node shadow copy. The node core keeps two
lifecycle bytes per peer in discovery's generic per peer user scratch. The scratch is
zeroed when a new uuid takes a slot, preserved across drop and resume, and copied on
migrate. The GONE event fires before the slot is marked free so a GONE handler can still
read the scratch.

The node core is sans-IO but is designed to depend on the transport and discovery cores,
never their runtimes. That is its role as the combiner.

## The abstract destination

The transport core never names a wire address. Every datagram it emits is addressed to a
peer id. The node core owns the peer id to address map and resolves it
(`ramble_node_core_resolve`, `ramble_node_core_id_for_addr`). The runtime owns the wire
convention and the send. That is the one place a transport shows through, and why a
serial transport needs no sans-IO change: it resolves the same peer ids to serial
addresses, frames the same datagram bytes, and feeds received frames back to the core.

## SHM is not a transport

A real transport (serial) replaces the physical pipe under every lane. The core sees
identical DATA, HB and NACK submessages either way. Shared memory instead changes the
payload form on a lane that is still inside the reliable stream: a range still takes
seqnos, acks and repair, and falls back to inline fragment repair when the reader cannot
resolve the locator. So it earns four small core hooks and otherwise lives in the node
and the shm module:

- a per peer capability bit (`peer_shm`),
- a wire flag and a fixed locator size (`RAMBLE_F_SHM`, the 24 byte descriptor),
- a callback to hand the locator up (`on_shm`, where 0 means unresolved and the
  reliability layer repairs or skips),
- a publish entry that references an external buffer plus its locator
  (`ramble_transport_send_shm`).

The core never decodes the locator, maps memory or checks a host id. The node core decides
a peer is out of band reachable when its host id equals ours (`oob_capable`, `oob_host`).

## Adding a serial transport

1. Platform: add serial primitives behind the same kind of contract the sockets use:
   open, read, write, and a frame delimiter or a length prefix.
2. Node runtime: a second runtime beside `node/runtime.c` that owns the serial handle and
   the clock and drives the same node core. It resolves a peer id to a serial address,
   frames the datagram bytes and recovers boundaries on receive (COBS or a length prefix),
   feeds each frame to the core, and answers the `is_local` hook.
3. Discovery: drive the discovery core over serial by feeding it the announces that arrive
   on the link, or skip discovery and add peers from static config through the node core
   peer lifecycle. The node core does not care how a peer was learned.
4. Address shape: the physical address is IPv4 shaped today. For a non IP link treat the
   record as an opaque blob the runtime interprets. The node core only reads it in the two
   resolve functions.

Nothing in the transport core, discovery core or node core changes. SHM rides whichever
transport carries the control plane and stays off on a link with no same host peer.
