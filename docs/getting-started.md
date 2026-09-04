# Getting started

DART is a C99 library shipped as single headers built from `src/`. Pick one packaging:

- `dart.h` holds everything.
- `dart_discovery.h` plus `dart_transport.h` split it in two. The transport header includes
  the discovery one.

Define the implementation macro in exactly one C file:

```c
#define DART_IMPLEMENTATION
#include "dart.h"
```

Link the platform libraries: `-lws2_32 -lbcrypt -lwinmm` on Windows, `-lrt -lpthread` on
Linux. macOS and BSD need neither. See docs/building.md for the CMake build.

## A first node

```c
static void on_message(const DartMsg *msg){
    printf("%.*s @ %.*s > %.*s\n",
           (int)msg->publisher_name.len, msg->publisher_name.data,
           (int)msg->topic_name.len, msg->topic_name.data,
           (int)msg->data.len, (const char *)msg->data.data);
}
static void on_event(const DartEvent *ev){
    char buf[256];
    puts(dart_event_str(ev, buf, sizeof buf));
}

DartAllocator mem = dart_allocator_dynamic(i_dart_plat_realloc, 0);
DartNode  *n = dart_node_open(&mem, "robot1", on_message, on_event,
                   &(DartNodeOpts){ .domain = 7 });
DartTopic *t = dart_node_create_topic(n, "msg", DART_PUBSUB, NULL,
                   &(DartTopicOpts){ .qos = { .reliability = DART_RELIABLE } });
dart_topic_send(t, dart_bytes(data, len));
while (running) dart_node_poll(n, 10);
dart_node_close(n, 1);
```

Required arguments are positional. Optional config is a trailing pointer to an options
struct, and `NULL` means all defaults. Names on a `DartMsg` are `DartString` views and
payloads are `DartBytes` views, each with a `.data` and a `.len`, never NUL terminated.

The node name is a human readable label. Pass `NULL` for an auto generated
`node-XXXXXXXX`. It travels once in the discovery announce and shows up on every delivered
message as `publisher_name`, whose `.data` is never NULL (`unknown-peer` when missing).
Names are capped at `DART_NODE_NAME_MAX` (32) bytes.

## Memory

A `DartAllocator` is the one memory model at every layer. `dart_allocator_dynamic` takes
pages from the heap. `dart_allocator_static(buf, size)` runs over a caller buffer with no
heap at all, for embedded use. Buffers, index maps and lane records size to real traffic
inside whatever bound the allocator enforces. Running out surfaces as a `DART_E_OOM` error
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
| `user_data` | surfaced as `DartMsg.user` and `DartEvent.user` |

## Build flags

Define these before the include. `<P>` is `DART_DISCOVERY` or `DART_TRANSPORT` for the
split headers and `DART` for the combined one.

| flag | effect |
|---|---|
| `<P>_IMPLEMENTATION` | emit the implementation, in one file only |
| `<P>_SANS_IO` | strip the runtime, keep the portable cores |
| `DART_SHM` | same host shared memory path. Auto on for Windows, Linux, macOS and BSD |
| `DART_NO_SHM` | force shared memory off. Drops the Linux `-lrt` |
| `DART_THREADS` | node lock and service thread. Auto on for Windows and POSIX with pthreads |
| `DART_NO_THREADS` | force threading off. `dart_node_start` returns `DART_ERR_NOSYS` |
| `DART_PROC_STATS` | process CPU and memory in the meta snapshot. Auto on where the OS can measure |
| `DART_NO_PROC_STATS` | force the proc section off |
| `DART_NO_DIAG` | strip the `dart_event_str` texts. An error then formats as `error <N>` |
| `DART_NO_PATTERNS` | strip functions, tasks and variables |
| `DART_NO_STDTYPES` | strip the standard type roster (docs/stdtypes.md) |

A `NO` flag always wins. The runtime is on by default. Embedded or bring your own IO
callers define `<P>_SANS_IO`:

```c
#define DART_IMPLEMENTATION
#define DART_SANS_IO
#include "dart.h"
```

## Tunables

| tunable | default | meaning |
|---|---|---|
| `DART_FRAG_SIZE` | 1350 | fragment payload bytes |
| `DART_RX_BUDGET_US` | 5000 | receive time budget per poll |
| `DART_HB_SWEEP_US` | 25000 | period of the heartbeat timer sweep |
| `DART_HB_TAIL_US` | 20000 | tail heartbeat delay until a peer's round trip is measured |
| `DART_RTO_MIN_US` | 2000 | floor of every timer derived from the round trip |
| `DART_UNSENT_WAIT_US` | 20000 | threaded mode, max wait for a poller before a send may overwrite unsent history |
| `DART_MATCH_WAIT_MS` | 1000 | default first send match wait |
| `DART_TOPIC_NAME_MAX` | 64 | max topic name bytes |
| `DART_QUEUE_CAP` | 1 MB | default per topic consumer queue cap |
| `DART_DISCOVERY_META_MAX` | 64 | default per peer announce blob capacity |
| `DART_DISCOVERY_PROTO_VERSION` | 5 | discovery protocol version |

Shared memory tunables are in spec/shm.md.
