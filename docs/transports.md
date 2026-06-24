# DART transports

DART separates what decides which bytes move from what moves them. The sans-IO cores
decide. A runtime moves the bytes. Adding a new transport (serial, Bluetooth, a second
link) is a new runtime over the same cores, with no change to any sans-IO file.

## Layers

```
  sans-IO cores  (never touched to add a transport)
    transport core   reliability, fragmentation, history, NACK repair
    discovery core   peer table + announce state machine
    node core        peer id <-> physical address, discovery->transport lifecycle

  platform           the one OS layer: sockets, clock, entropy, route probe, SHM map

  runtimes           own the sockets and the clock and drive the cores
    discovery runtime
    node runtime      the public dart_node_* API (UDP today)
```

A core never calls the platform. A runtime is the only thing that does IO. The node
core was split out of the node runtime for this reason: the peer table and the
discovery to transport wiring are pure, so they live in node/core.c and are tested
with no sockets (see the `node-core:` block in the selftest). node/runtime.c owns the
sockets, the clock, the route probe, and the send and receive paths.

## The abstract destination

The transport core never names a wire address. Every datagram it emits is addressed to
a `to`: either a peer id, or a multicast group selector flagged by the top bit. What
that resolves to on the wire is the runtime's job.

- a peer id resolves to a physical address record (an IP and a port today).
- a group selector resolves to a group address. On UDP that is
  `239.255.<domain>.<selector>`. On another link it is whatever that link broadcasts
  to.

This is the node core to runtime boundary, and it is the one place a transport shows
through. The node core owns the peer id to address map and resolves a peer id
(`dart_node_core_addr_for_id`, `dart_node_core_id_for_addr`). The runtime owns the
wire convention and the send. The `239.255` group formula lives in the UDP runtime,
not the core, because it is a UDP convention.

That is why a serial transport needs no sans-IO change. The serial runtime resolves
the same abstract destinations to serial addresses, frames the same datagram bytes
over the link, and feeds received frames back to the core. The cores do not know the
link changed.

## SHM is not a transport

SHM looks like a second transport but it is not. It is a payload encoding inside the
reliable stream, not a pipe under it.

A real transport (serial) replaces the physical pipe under every lane. The core sees
identical DATA, HB, and NACK submessages either way. Serial sits below the datagram
abstraction, so it touches the core zero times.

SHM changes the payload form on a lane that is still inside the reliable stream. An
SHM range still takes seqnos, still must be acked, and still falls back to inline
fragment repair when the reader cannot resolve the locator. That fallback is
reliability logic, which is core logic. SHM sits inside the stream.

So the two live at different layers:

- serial is below the datagrams. It is a transport. It needs a new runtime and no core
  change.
- SHM is inside the stream. It is an out-of-band payload. It needs the small core hooks
  below, and otherwise lives in the node and the shm module.

## Out-of-band payload

The transport core knows one generic thing that SHM uses: a per-peer lane may carry an
opaque fixed-size locator instead of inline fragments. That is four hooks.

- a per-peer capability bit (today `peer_shm`).
- a wire flag and a fixed locator size (`DART_F_SHM`, the 24 byte descriptor).
- a callback to hand the locator up for resolution (`on_shm`).
- a publish entry that references an external buffer plus its locator (`dart_send_shm`).

The core never decodes the locator, never maps memory, never checks a host id. To the
core the locator is opaque bytes it frames and ships. `on_shm` returning 0 means the
reader could not resolve it, so the reliability layer repairs or skips the range.

SHM is the only out-of-band provider today. The node core decides a peer is OOB
reachable when the peer advertises a host id equal to ours: a plain compare, with no
SHM knowledge in the core (`oob_capable`, `oob_host` in the node core config). The
descriptor format, the mmap, and the host identity live in the node runtime and the
shm module, above the core. The names say `shm` for now; the shape is generic
out-of-band, and a second provider would only justify renaming them.

## Adding a serial transport

The shape, concretely. None of this touches a sans-IO core.

1. Platform. Add serial primitives to the platform layer behind the same kind of
   contract the sockets use: open, read, write, and either a frame delimiter or a
   length prefix. A self-contained link driver can stay out of the platform layer.

2. Node runtime. Write a second node runtime beside node/runtime.c. It owns the serial
   handle and the clock, and drives the same node core the UDP runtime does:
   - resolve the abstract destination: a peer id to a serial address, a group selector
     to a bus broadcast or a multidrop address.
   - frame the datagram bytes the core emits, and recover datagram boundaries on
     receive (COBS or a length prefix), then feed each complete frame to
     `dart_on_datagram`.
   - answer the node core `is_local` hook (false on a bus between hosts, or a real
     check if two endpoints share a kernel).

3. Discovery. Discovery over UDP uses a multicast announce. A serial link may have no
   multicast, so either:
   - drive the discovery core over serial: feed it announces that arrive on the link,
     the same way the UDP runtime feeds it datagrams; or
   - skip discovery and add peers from static config by calling the node core peer
     lifecycle directly. The node core does not care how a peer was learned.

4. Address shape. The physical address is IPv4 shaped today (an IP and a port). For a
   non IP link, treat the address record as an opaque blob the runtime interprets. The
   node core already passes it through and only reads it in the two resolve functions.

What you do not touch: the transport core, the discovery core, the node core. The
reliability, fragmentation, repair, interest matching, and peer lifecycle are reused
as is. SHM rides whichever transport carries the control plane, and stays disabled on
a link with no same-host peer.
