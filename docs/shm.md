# DART shared memory

SHM is the same-host fast path. A publisher delivers to a subscriber on the same
machine through shared memory instead of fragmenting the payload over UDP. It is on
by default. Build with `DART_NO_SHM` to strip it.

## What rides the wire

A normal large message goes out as many UDP fragment datagrams. An SHM message goes
out as one 37 byte SHM-DATA submessage. The payload is not on the wire. It sits in a
shared chunk that the reader maps and reads.

The SHM-DATA submessage is a DATA submessage with the `DART_F_SHM` flag set.

| field | bytes |
|-------|-------|
| type and flags | 1 |
| index | 2 |
| base seqno | 8 |
| count | 2 |
| descriptor | 24 |

The descriptor is segment id (8), chunk index (4), length (4), generation (8).

## When SHM is used

The node picks SHM per message. All of these must hold.

- The build has SHM (no `DART_NO_SHM`).
- The node has an allocator. The default dynamic mode sets one. Fixed mode and `--max` do not.
- Every matched reader is on the same host and is SHM capable.
- The message fits a size class.

If any does not hold, the publish goes inline over UDP. One remote reader sends the
whole message inline.

## Same host

Same host is a host id match, not an address match. Each node advertises a host id
in its discovery announce. A peer is SHM capable when it advertises SHM and its host
id equals ours. A loopback address is not enough.

## Segments and size classes

The payload lives in shared-memory segments outside the arena. The OS maps them. See
docs/allocation.md for sizes.

There is one segment per size class. Class k holds chunks of
`DART_SHM_CLASS_BASE << (k * DART_SHM_CLASS_SHIFT)` bytes. The defaults are 64K,
256K, 1M, 4M, 16M, 64M, 256M. A publish takes the smallest class that fits. There is
no size based fallback to UDP. A message larger than the wire cap is refused on both
paths. The wire cap is 65535 times the fragment size.

Segments are lazy. The node creates a class segment only when a same-host SHM reader
exists and a publish needs that class. A node with no same-host reader maps nothing.

The low 3 bits of the segment id are the class. The reader derives the chunk size
from the class and attaches the segment on first use.

## Chunk lifecycle

A chunk is the writer history slot. There is one chunk per keep_last slot per
topic. A publish takes a chunk from its class pool, writes the payload, stamps a
new generation, and sends the descriptor. The node frees the slot chunk when the
transport reuses that slot. The transport reuses a slot only after the message is
acked or evicted.

## Generation guard

Every chunk has a generation counter. The writer bumps it on each reuse and puts it
in the descriptor. The reader checks it before reading and again after the copy. A
mismatch means the writer recycled the chunk. The reader drops that read. This is
lock free. The writer never blocks.

## Delivery

The reader copies the chunk into a node owned buffer before delivery. This is one
copy. The user `on_message` gets the same bytes whether they came inline or through
SHM. The user writes one callback and does not see the transport.

The reader acks after the copy. The writer holds the chunk until the ack.

## Loss and repair

The descriptor still travels over UDP and can drop. Reliable topics repair it. The
reader NACKs the gap and the writer re-sends the descriptor.

A reader that cannot resolve a descriptor does not ack. A recycled chunk resolves to
a skip through the normal heartbeat path. A transient failure resolves on the
re-send. A descriptor that stays unresolvable, such as mismatched `DART_SHM_*`
constants between nodes, is skipped after `DART_SHM_MAX_RETRY` tries with a
`DART_MSG_LOST` event. The reader does not wedge.

Best effort topics do not repair. A reader that falls behind misses recycled
messages, same as best effort over UDP.

## Not supported yet

- Zero copy read. The reader does one copy. Zero copy is a later read side mode.

## Build

SHM needs `shm_open` and `mmap`. On POSIX link `-lrt`. On Windows it uses
`CreateFileMapping`. Strip it all with `DART_NO_SHM`.

```sh
cc  -std=c99 -Idist app.c -o app -lrt           # POSIX, SHM on
gcc -std=c99 -Idist app.c -o app -lws2_32 -lbcrypt   # Windows, SHM on
cc  -std=c99 -DDART_NO_SHM -Idist app.c -o app  # SHM off
```

## Tunables

| tunable | default | effect |
|---------|---------|--------|
| DART_SHM_CLASS_BASE | 64K | smallest chunk |
| DART_SHM_CLASS_SHIFT | 2 | class growth ratio |
| DART_SHM_N_CLASSES | 7 | number of classes |
| DART_SHM_CHUNKS_PER_CLASS | 4 | chunks per segment |
| DART_SHM_MAX_RETRY | 8 | resolve tries before skip |
