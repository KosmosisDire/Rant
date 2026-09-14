# Getting started

Ramble is a C99 library shipped as single headers built from `src/`. Pick one packaging:

- `ramble.h` holds everything.
- `ramble_discovery.h` plus `ramble_transport.h` split it in two. The transport header includes
  the discovery one.

Define the implementation macro in exactly one C file:

```c
#define RAMBLE_IMPLEMENTATION
#include "ramble.h"
```

Link the platform libraries: `-lws2_32 -lbcrypt -lwinmm` on Windows, `-lrt -lpthread` on
Linux. macOS and BSD need neither. See docs/building.md for the CMake build.

## A first node

```c
static void on_message(const RambleMsg *msg){
    printf("%.*s @ %.*s > %.*s\n",
           (int)msg->publisher_name.len, msg->publisher_name.data,
           (int)msg->topic_name.len, msg->topic_name.data,
           (int)msg->data.len, (const char *)msg->data.data);
}
static void on_event(const RambleEvent *ev){
    char buf[256];
    puts(ramble_event_str(ev, buf, sizeof buf));
}

RambleAllocator mem = ramble_allocator_heap(0);
RambleNode  *n = ramble_node_open(&mem, "robot1", on_message, on_event,
                   &(RambleNodeOpts){ .domain = 7 });
RambleTopic *t = ramble_node_create_topic(n, "msg", RAMBLE_PUBSUB, NULL,
                   &(RambleTopicOpts){ .qos = { .reliability = RAMBLE_RELIABLE } });
ramble_topic_send(t, ramble_bytes(data, len));
while (running) ramble_node_poll(n, 10);
ramble_node_close(n, 1);
```

Required arguments are positional. Optional config is a trailing pointer to an options
struct, and `NULL` means all defaults. Names on a `RambleMsg` are `RambleString` views and
payloads are `RambleBytes` views, each with a `.data` and a `.len`, never NUL terminated.

The node name is a human readable label. Pass `NULL` for an auto generated
`node-XXXXXXXX`. It travels once in the discovery announce and shows up on every delivered
message as `publisher_name`, whose `.data` is never NULL (`unknown-peer` when missing).
Names are capped at `RAMBLE_NODE_NAME_MAX` (32) bytes.

## Memory

A `RambleAllocator` is the one memory model at every layer. `ramble_allocator_dynamic` takes
pages from the heap. `ramble_allocator_static(buf, size)` runs over a caller buffer with no
heap at all, for embedded use. Buffers, index maps and lane records size to real traffic
inside whatever bound the allocator enforces. Running out surfaces as a `RAMBLE_E_OOM` error
event, never a silent truncation. spec/allocation.md lists what is allocated when.

## Node options

| option | meaning |
|---|---|
| `domain` | logical network selector, nodes only see their own domain |
| `max_topics` | how many topics can be created (default 8) |
| `disable_shm` | force the wire path even to a same host peer |
| `match_wait_ms` | first send match wait bound, 0 = 1 s, negative = off (docs/topics.md) |
| `fetch_details` | fetch names and schemas for every topic every peer has, for observer tools |
| `disable_logs`, `disable_meta`, `disable_error_logs` | strip the built in log topics and meta function (docs/node.md) |
| `net` | ports, group, interface, seed peers, unicast only, own locator (docs/discovery.md) |
| `discovery` | announce interval, peer timeout, max peers (docs/discovery.md) |
| `user_data` | surfaced as `RambleMsg.user` and `RambleEvent.user` |

## Build flags

Define these before the include. `<P>` is `RAMBLE_DISCOVERY` or `RAMBLE_TRANSPORT` for the
split headers and `Ramble` for the combined one.

| flag | effect |
|---|---|
| `<P>_IMPLEMENTATION` | emit the implementation, in one file only |
| `<P>_SANS_IO` | strip the runtime, keep the portable cores |
| `RAMBLE_PLAT_CUSTOM` | drop only the bundled platform implementation, so a caller links its own `i_ramble_plat_*` functions |
| `RAMBLE_SHM` | same host shared memory path. Auto on for Windows, Linux, macOS and BSD |
| `RAMBLE_NO_SHM` | force shared memory off. Drops the Linux `-lrt` |
| `RAMBLE_THREADS` | node lock and service thread. Auto on for Windows and POSIX with pthreads |
| `RAMBLE_NO_THREADS` | force threading off. `ramble_node_start` returns `RAMBLE_ERR_NOSYS` |
| `RAMBLE_PROC_STATS` | process CPU and memory in the meta snapshot. Auto on where the OS can measure |
| `RAMBLE_NO_PROC_STATS` | force the proc section off |
| `RAMBLE_NO_DIAG` | strip the `ramble_event_str` texts. An error then formats as `error <N>` |
| `RAMBLE_NO_PATTERNS` | strip functions, tasks and variables |
| `RAMBLE_NO_STDTYPES` | strip the standard type roster (docs/stdtypes.md) |

A `NO` flag always wins. The runtime is on by default. Embedded or bring your own IO
callers define `<P>_SANS_IO`:

```c
#define RAMBLE_IMPLEMENTATION
#define RAMBLE_SANS_IO
#include "ramble.h"
```

## Tunables

| tunable | default | meaning |
|---|---|---|
| `RAMBLE_FRAG_SIZE` | 1350 | fragment payload bytes |
| `RAMBLE_RX_BUDGET_US` | 5000 | receive time budget per poll |
| `RAMBLE_HB_SWEEP_US` | 25000 | period of the heartbeat timer sweep |
| `RAMBLE_HB_TAIL_US` | 20000 | tail heartbeat delay until a peer's round trip is measured |
| `RAMBLE_RTO_MIN_US` | 2000 | floor of every timer derived from the round trip |
| `RAMBLE_UNSENT_WAIT_US` | 20000 | threaded mode, max wait for a poller before a send may overwrite unsent history |
| `RAMBLE_MATCH_WAIT_MS` | 1000 | default first send match wait |
| `RAMBLE_TOPIC_NAME_MAX` | 64 | max topic name bytes |
| `RAMBLE_QUEUE_CAP` | 1 MB | default per topic consumer queue cap |
| `RAMBLE_DISCOVERY_META_MAX` | 64 | default per peer announce blob capacity |
| `RAMBLE_DISCOVERY_PROTO_VERSION` | 5 | discovery protocol version |

Shared memory tunables are in spec/shm.md.
