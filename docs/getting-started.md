# Getting started

Rant is a C99 library shipped as single headers built from `src/`. Pick one packaging:

- `rant.h` holds everything.
- `rant_discovery.h` plus `rant_transport.h` split it in two. The transport header includes
  the discovery one.

Define the implementation macro in exactly one C file:

```c
#define RANT_IMPLEMENTATION
#include "rant.h"
```

Link the platform libraries: `-lws2_32 -lbcrypt -lwinmm` on Windows, `-lrt -lpthread` on
Linux. macOS and BSD need neither. See docs/building.md for the CMake build.

## A first node

```c
static void on_message(const RantMsg *msg){
    printf("%.*s @ %.*s > %.*s\n",
           (int)msg->publisher_name.len, msg->publisher_name.data,
           (int)msg->topic_name.len, msg->topic_name.data,
           (int)msg->data.len, (const char *)msg->data.data);
}
static void on_event(const RantEvent *ev){
    char buf[256];
    puts(rant_event_str(ev, buf, sizeof buf));
}

RantAllocator mem = rant_allocator_heap(0);
RantNode    *n = rant_node_open(&mem, "robot1", on_message, on_event,
                   &(RantNodeOpts){ .domain = 7 });
RantTopic *t = rant_node_create_topic(n, "msg", RANT_PUBSUB, NULL,
                   &(RantTopicOpts){ .qos = { .reliability = RANT_RELIABLE } });
rant_topic_send(t, rant_bytes(data, len));
while (running) rant_node_poll(n, 10);
rant_node_close(n, 1);
```

Required arguments are positional. Optional config is a trailing pointer to an options
struct, and `NULL` means all defaults. Names on a `RantMsg` are `RantString` views and
payloads are `RantBytes` views, each with a `.data` and a `.len`, never NUL terminated.

The node name is a human readable label. Pass `NULL` for an auto generated
`node-XXXXXXXX`. It travels once in the discovery announce and shows up on every delivered
message as `publisher_name`, whose `.data` is never NULL (`unknown-peer` when missing).
Names are capped at `RANT_NODE_NAME_MAX` (32) bytes.

## Memory

A `RantAllocator` is the one memory model at every layer. `rant_allocator_dynamic` takes
pages from the heap. `rant_allocator_static(buf, size)` runs over a caller buffer with no
heap at all, for embedded use. Buffers, index maps and lane records size to real traffic
inside whatever bound the allocator enforces. Running out surfaces as a `RANT_E_OOM` error
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
| `user_data` | surfaced as `RantMsg.user` and `RantEvent.user` |

## Build flags

Define these before the include. `<P>` is `RANT_DISCOVERY` or `RANT_TRANSPORT` for the
split headers and `Rant` for the combined one.

| flag | effect |
|---|---|
| `<P>_IMPLEMENTATION` | emit the implementation, in one file only |
| `<P>_SANS_IO` | strip the runtime, keep the portable cores |
| `RANT_PLAT_CUSTOM` | drop only the bundled platform implementation, so a caller links its own `i_rant_plat_*` functions |
| `RANT_SHM` | same host shared memory path. Auto on for Windows, Linux, macOS and BSD |
| `RANT_NO_SHM` | force shared memory off. Drops the Linux `-lrt` |
| `RANT_THREADS` | node lock and service thread. Auto on for Windows and POSIX with pthreads |
| `RANT_NO_THREADS` | force threading off. `rant_node_start` returns `RANT_ERR_NOSYS` |
| `RANT_PROC_STATS` | process CPU and memory in the meta snapshot. Auto on where the OS can measure |
| `RANT_NO_PROC_STATS` | force the proc section off |
| `RANT_NO_DIAG` | strip the `rant_event_str` texts. An error then formats as `error <N>` |
| `RANT_NO_PATTERNS` | strip functions, tasks and variables |
| `RANT_NO_STDTYPES` | strip the standard type roster (docs/stdtypes.md) |

A `NO` flag always wins. The runtime is on by default. Embedded or bring your own IO
callers define `<P>_SANS_IO`:

```c
#define RANT_IMPLEMENTATION
#define RANT_SANS_IO
#include "rant.h"
```

## Tunables

| tunable | default | meaning |
|---|---|---|
| `RANT_FRAG_SIZE` | 1350 | fragment payload bytes |
| `RANT_RX_BUDGET_US` | 5000 | receive time budget per poll |
| `RANT_HB_SWEEP_US` | 25000 | period of the heartbeat timer sweep |
| `RANT_HB_TAIL_US` | 20000 | tail heartbeat delay until a peer's round trip is measured |
| `RANT_RTO_MIN_US` | 2000 | floor of every timer derived from the round trip |
| `RANT_MATCH_WAIT_MS` | 1000 | default first send match wait |
| `RANT_TOPIC_NAME_MAX` | 64 | max topic name bytes |
| `RANT_QUEUE_CAP` | 1 MB | default per topic consumer queue cap |
| `RANT_DISCOVERY_META_MAX` | 64 | default per peer announce blob capacity |
| `RANT_DISCOVERY_PROTO_VERSION` | 5 | discovery protocol version |

Shared memory tunables are in spec/shm.md.
