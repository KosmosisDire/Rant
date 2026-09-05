# Memory

`DartAllocator` in `src/common/alloc.h` is the memory model at every layer. It is header
only, depends on nothing but stddef, stdint and string, and the backing is injected, so
`common/` never calls the platform.

## Modes and intents

Two modes. `dart_allocator_static(buf, size)` bumps one caller buffer, never grows, and
returns NULL on overflow, for embedded no heap use. `dart_allocator_dynamic(backing,
page_size)` bumps within pages and takes a new page through the injected `DartPageFn`
when full (the runtime passes `i_dart_plat_realloc`). Exhaustion surfaces as
`DART_E_OOM`. `max_bytes` is a runaway guard (0 = `DART_MEM_DEFAULT_MAX`).

Two intents, explicit not size based. `dart_allocator_fixed` bumps a shared page,
cheapest, never individually freed. `dart_allocator_alloc` is a `DartAllocFn` (pass `&a`
as user) and returns a freeable block. Anything that grows uses the freeable form, so it
is freeable by construction. `dart_allocator_reset` frees everything in both intents, so
no per allocation free is required. In static mode both intents bump and free is a no op.

Freed blocks go to a reuse pool in both modes and never back to the backing heap, so
steady state churn cannot fragment the system heap. Size classes are quarter power of two
(waste at most 25%), exact size above 4 kB. Static mode sizes every block exactly, since
the buffer cannot grow. A pooled block is reused only when it is within twice the ask, so
a big block is not spent on a small one. The runaway guard counts pooled memory as held.
The page allocator halves its request on failure for fragmented heaps.

## The copied pool rule

`dart_node_open` and `dart_discovery_open` copy the caller's allocator by value into their
own struct, so the caller may pass a stack temporary. Every close and every failure path
must copy the pool value out, then reset the copy, because the reset frees the very page
holding the struct that holds the pool field. Copy it out LAST, after every hook free has
run, or the stale copy double frees (this was a heap corruption crash). One allocator per
node: a shared long lived allocator would have its pages freed under anything that
outlives the node. Callers never reset it themselves.

## Growth

Dynamic mode grows the node by relocating: allocate a bigger arena and migrate all three
cores. The node struct and the topic handles live outside the growable arena so a
relocation never moves a pointer the user holds. Heap message buffers and SHM segments
stay put. 2D tables re stride, the scheduler is rebuilt from proxy state, and discovery
is copied, not re inited, so its uuid, sockets and peers survive. Static mode never grows.
Migrate adopts the lane pool (it is hook memory), re strides only the ticket table, and
clears in use records' scheduler fields while leaving free records' links alone.
`common/arena.h` packs the one relocatable arena block.

## The arena

The arena is one contiguous block the node sizes and takes from its allocator at open. It
holds the fixed control tables, sized by `max_peers` and `n_topics`, independent of
message size and of how many peers or matches exist. Nothing in it is freed until close.

| buffer | count | element |
|---|---|---|
| peer ids, used, dormant, frag, shm | max_peers | 1 to 4 bytes |
| interest bitmaps (pub, sub, sub reliable) | max_peers x ceil(n_topics/8) | 1 byte |
| topics | n_topics | i_DartTopic |
| lane ticket table | n_topics x max_peers | 2 bytes |
| lane scheduler (dest lists and queue) | max_peers | 1 to 4 bytes |
| index and verdict map pointers | max_peers | 8 + 4 + 8 bytes |
| name pool | n_topics x (DART_TOPIC_NAME_MAX + 1) | 1 byte |

## Hook allocations

These scale with real traffic and real matches, never with worst case tables.

| buffer | when | size |
|---|---|---|
| writer history slot | first send that needs it | grows to the largest message sent |
| lane record pool | first match (doubles from 8) | live matches x sizeof(i_DartLane) |
| reader reassembly and bitmap, two slots | per matched lane, on receive | grows to the largest message received |
| per peer index and verdict maps | peer's first matched topic | that peer's advertised entry count |
| announce blob | first build | actual overlay size |
| per peer interest and detail buffers | on fetch | actual length, freed on GONE |

A grown buffer never shrinks. Freeing returns the block to the reuse pool. The sparse
matched lane record (`i_DartLane`, both proxies inline plus back references) is allocated
on the match edge and freed to a free list on unmatch, with a u16 ticket table for
lookup. Topics carry a lane chain, so commit, eviction, drain and repair walk O(matched),
not O(max_peers). Every proxy accessor returns NULL for an unmatched lane, so every entry
point guards. Per peer index maps are sized to the peer's highest advertised index at
apply time, so a later local subscribe never forces a regrow.

Rules from the footprint work:

- Never hold an `i_DartLane*` across `i_dart_lane_ensure`. The pool reallocs and records
  move. Durable references are pool indices.
- Releasing a lane must drop it from the scheduler, or a recycled record on a destination
  list misroutes submessages to the old peer.
- Our own announce blob is allocated at `dart_transport_meta_size`, which mirrors
  `meta_build` byte for byte. Keep the two in sync.
- Peer removal frees the lane's assembly buffers and bitmaps.
- The per lane and per sample structs are laid out widest field first, so they carry no
  padding. They are allocated per match and per history slot, so padding multiplies.
- The lane pool caps at 65535 records, the u16 ticket. A further match is refused until
  a lane frees, and the peer's next announce retries.

## Sends larger than the wire cap

There is no per topic message cap. `qos.max_message_bytes` is a hint that pins the SHM
size class. The only hard limit is the wire's 65535 fragment cap (`DART_MESSAGE_MAX`). A
receive that cannot allocate its reassembly buffer skips the sample and fires
`DART_E_MSG_TOO_BIG`.

## Outside the arena

| buffer | size |
|---|---|
| socket receive buffer | `net.recv_buffer_bytes` (OS) |
| socket send buffer | `net.send_buffer_bytes` (OS) |
| per datagram stack buffer | `DART_DGRAM_MAX` = `DART_FRAG_SIZE_MAX` + 40 |
| SHM segments | OS mapped, lazy, see spec/shm.md |
| SHM receive scratch | grows via the hook, one per node |

The serializer takes the same `DartAllocFn` hook as the transport core, not the
`DartAllocator`, so it stays a sans-IO core. SHM shared segments cannot use the allocator
(cross process mmap, fixed geometry), only their control state does.

## Footprint

Measured with an instrumented page backing (two in process nodes, no SHM, 64 bit host): an
idle full node opens at about 20 kB, an ESP32 shaped example sits at 27 kB before traffic
and about 87 kB warmed. The first matched peer adds about 2 kB (the lane pool). Idle poll
is about 4 us. Code size at -Os on x86-64 is 118 kB full, 94 kB with no SHM, diag and
patterns.

Where the RAM goes, in order: writer history (`keep_last` times message size, never
shrinks), the builtin topics (about 5 kB), the fixed node buffers sized by the compile
time `DART_FRAG_SIZE` (not the runtime `net.fragment_size`), per topic repair stats and
qos, and the lane record, which carries both sides even when one is used.

Config only wins: drop a deep `keep_last` when `catch_up` is 0, set `DART_FRAG_SIZE`
smaller at compile time, set `qos.queue_bytes` to cap take rings, and call `take(0)`
beside an explicit poll, since a timed take on an empty queue drives a full nested poll
pass.

Not applied, no capability loss: a per topic history slab instead of a page per slot,
lazily allocated repair stats, splitting the lane record per side, zero copy delivery of a
single fragment in order message, caching `now_us` per poll pass. The builtins must stay
functional, so make them cheap rather than leaning on `disable_logs` and `disable_meta`.

## Tunables

| tunable | default | effect |
|---|---|---|
| DART_FRAG_SIZE | 1350 | fragment payload bytes, MIN and MAX bound the range |
| DART_FRAG_SIZE_MAX | DART_FRAG_SIZE | sizes the datagram buffers |
| DART_SHM_CLASS_BASE | 64K | smallest SHM chunk |
| DART_SHM_CLASS_SHIFT | 2 | SHM class growth ratio |
| DART_SHM_N_CLASSES | 7 | number of SHM classes |
