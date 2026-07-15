# DART memory and buffers

DART puts all fixed state in one arena the caller provides. The node mallocs it
once. `dart_node_required_memory` and `dart_required_memory` return the size. A
few buffers live outside the arena. This file lists every buffer and how its size
is set.

## The arena

One contiguous block. It holds the node struct, the peer table, the discovery
state, the transport state, and the SHM bookkeeping. Nothing in it is freed until
the caller frees the block.

## Control tables (transport)

Sized by `max_peers` and `n_topics`. Independent of message size. Small and fixed
for the life of the node.

| buffer | count | element |
|--------|-------|---------|
| peer ids, used, local, dormant, frag, shm | max_peers | 1 to 4 bytes |
| interest bitmaps (pub, sub) | max_peers x ceil(n_topics/8) | 1 byte |
| topics | n_topics | i_DartTopic |
| writer proxies | n_topics x max_peers | i_DartWriterProxy |
| reader proxies | n_topics x max_peers | i_DartReaderProxy |
| lane scheduler | n_topics x (max_peers+1), and max_peers+n_topics | 1 to 4 bytes |
| index table | max_peers x DART_META_MAX_IDS | 2 bytes |
| name pool | sum of topic name lengths | 1 byte |

## Payload buffers (transport)

These scale with message size.

| buffer | count | size |
|--------|-------|------|
| writer history | n_topics x keep_last | max_message_bytes |
| reader reassembly | n_topics x max_peers | max_message_bytes |
| reader fragment bitmap | n_topics x max_peers | ceil(maxfrags / 8) |

`maxfrags` is `ceil(max_message_bytes / DART_FRAG_PAYLOAD_MIN)`.

## Fixed mode and dynamic mode

The mode is set by whether you pass an allocator.

Fixed mode has no allocator. The payload buffers above are carved from the arena at
init and sized to `max_message_bytes`. There is no per-message allocation. A message
larger than `max_message_bytes` is refused.

Dynamic mode has an allocator. The payload buffers start empty and grow to fit each
message through the allocator hook. They grow only. They never shrink. One large
message sets the size of its buffer for the rest of the run. The control tables stay
in the arena.

## Node additions

The node arena also holds these.

| buffer | size |
|--------|------|
| node struct | one struct |
| peer table | max_peers x dart__nodepeer |
| discovery announce blob | meta_cap bytes |
| discovery state | its own region |
| SHM bookkeeping | see SHM section |

`meta_cap` is the frag prefix plus the largest interest list the topics can
produce, capped to one UDP datagram.

## Outside the arena

| buffer | size |
|--------|------|
| socket receive buffer | net.recv_buffer_bytes (OS) |
| socket send buffer | net.send_buffer_bytes (OS) |
| per-datagram stack buffer | DART_DGRAM_MAX = DART_FRAG_PAYLOAD_MAX + 40 |
| SHM segments | OS mapped, lazy, see SHM section |
| SHM receive scratch | grows via allocator, one per node |

## SHM (only with DART_SHM, on by default)

SHM payloads live in shared-memory segments outside the arena. The OS maps them. The
node creates a segment only when a same-host SHM reader exists and a publish needs
that size class.

Segments use size classes. Class k holds chunks of
`DART_SHM_CLASS_BASE << (k * DART_SHM_CLASS_SHIFT)` bytes. The defaults give 64K,
256K, 1M, 4M, 16M, 64M, 256M. A publish takes the smallest class that fits. There is
no size-based fallback to UDP.

One segment per class holds `DART_SHM_CHUNKS_PER_CLASS` chunks. A publish takes a
chunk from the class pool and frees it when its history slot is reused. Segment size
is the header plus n_chunks times (chunk header plus chunk bytes).

The node arena holds the SHM handles. It does not hold the segments.

| buffer | size |
|--------|------|
| our pool handles | DART_SHM_N_CLASSES x state_bytes |
| slot binding | n_topics x keepmax x 2 |
| reader segment ids | rmax x 8 |
| reader pool handles | rmax x state_bytes |

`keepmax` is the largest keep_last across topics. `rmax` is
`max_peers x DART_SHM_N_CLASSES`. `state_bytes` is the per-segment handle size.

The SHM receive scratch is a node-owned buffer. The reader copies a chunk into it
once before delivery. It grows to the largest message seen. Build with `DART_NO_SHM`
to remove all of this.

## How size is decided

Fixed mode. You set `max_message_bytes`. History and reassembly buffers are that
size.

Dynamic mode. Buffers grow to the largest message seen and stay there.

SHM chunks. The smallest size class that fits the message.

## Tunables that affect size

| tunable | default | effect |
|---------|---------|--------|
| DART_FRAG_PAYLOAD_MIN | 1024 | sizes the reassembly bitmap |
| DART_FRAG_PAYLOAD_MAX | 1024 | sizes the datagram buffers |
| DART_META_MAX_IDS | 256 | sizes the index table |
| DART_SHM_CLASS_BASE | 64K | smallest SHM chunk |
| DART_SHM_CLASS_SHIFT | 2 | SHM class growth ratio |
| DART_SHM_N_CLASSES | 7 | number of SHM classes |
| DART_SHM_CHUNKS_PER_CLASS | 4 | chunks per SHM segment |
