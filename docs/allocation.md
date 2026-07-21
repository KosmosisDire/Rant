# DART memory and buffers

DART splits memory into two pools with one owner each.

The ARENA is one contiguous block the caller provides (the node sizes and takes it
from its `DartAllocator` at open). It holds the fixed control tables: the node
struct, the discovery state, the transport tables, the SHM handle table. Nothing
in it is freed until the node closes.

The ALLOCATOR HOOK (`DartConfig.allocator`, a `DartAllocFn`) backs everything that
scales with actual traffic: message buffers, reassembly state, per-peer index and
verdict maps, matched-lane records, the announce blob. The hook is REQUIRED; there
is no fixed-table mode. An embedded no-heap deployment passes a `DartAllocator` in
STATIC mode (one caller buffer, no growth, overflow refused with an explicit OOM
event); everything else uses DYNAMIC mode (pages from the platform heap, freed
blocks pooled for reuse so steady state makes no system malloc/free).

## Control tables (arena, transport)

Sized by `max_peers` and `n_topics`. Independent of message size and of how many
peers or matches actually exist.

| buffer | count | element |
|--------|-------|---------|
| peer ids, used, dormant, frag, shm | max_peers | 1 to 4 bytes |
| interest bitmaps (pub, sub, sub-reliable) | max_peers x ceil(n_topics/8) | 1 byte |
| topics | n_topics | i_DartTopic |
| lane ticket table | n_topics x max_peers | 2 bytes |
| lane scheduler (dest lists + queue) | max_peers | 1 to 4 bytes |
| index/verdict map POINTERS | max_peers | 8 + 4 + 8 bytes |
| name pool | n_topics x (DART_TOPIC_NAME_MAX + 1) | 1 byte |

## Hook allocations (transport)

These scale with real traffic and real matches, never with worst-case tables.

| buffer | when allocated | size |
|--------|----------------|------|
| writer history slot | first send that needs it | grows to the largest message sent |
| lane record pool | first match (doubles from 8) | live matches x sizeof(i_DartLane) |
| reader reassembly + bitmap | per matched lane, on receive | grows to the largest message received |
| per-peer index + verdict maps | peer's first matched topic | that peer's advertised entry count |
| announce blob | first build | actual overlay size |

A grown buffer never shrinks; freeing (peer gone, lane released) returns the block
to the allocator's reuse pool, not to the system heap.

## Sends larger than the wire cap

There is no per-topic message cap. `qos.max_message_bytes` is a HINT (it pins the
SHM size class when `shm_max_bytes` is 0); the only hard limit is the wire's
65535-fragment cap (`DART_MESSAGE_MAX`). A receive that cannot allocate its
reassembly buffer skips the sample and fires `MSG_TOO_BIG` (allocation failure,
never silent).

## Outside the arena

| buffer | size |
|--------|------|
| socket receive buffer | net.recv_buffer_bytes (OS) |
| socket send buffer | net.send_buffer_bytes (OS) |
| per-datagram stack buffer | DART_DGRAM_MAX = DART_FRAG_SIZE_MAX + 40 |
| SHM segments | OS mapped, lazy, see SHM section |
| SHM receive scratch | grows via the hook, one per node |

## SHM (only with DART_SHM, on by default)

SHM payloads live in shared-memory segments outside the arena. The OS maps them.
The node creates a segment only when a same-host SHM reader exists and a publish
needs that size class.

Segments use size classes. Class k holds chunks of
`DART_SHM_CLASS_BASE << (k * DART_SHM_CLASS_SHIFT)` bytes. The defaults give 64K,
256K, 1M, 4M, 16M, 64M, 256M. A publish takes the smallest class that fits.

Each (topic, class) gets its own segment of `keep_last` chunks; history slot i
binds chunk i. The arena holds only the (topic, class) -> segment pointer table;
segment state and the reader attach cache are lazy hook allocations. Build with
`DART_NO_SHM` to remove all of this.

## Tunables that affect size

| tunable | default | effect |
|---------|---------|--------|
| DART_FRAG_SIZE | 1350 | fragment payload bytes; MIN/MAX bound the range |
| DART_FRAG_SIZE_MAX | DART_FRAG_SIZE | sizes the datagram buffers |
| DART_SHM_CLASS_BASE | 64K | smallest SHM chunk |
| DART_SHM_CLASS_SHIFT | 2 | SHM class growth ratio |
| DART_SHM_N_CLASSES | 7 | number of SHM classes |
