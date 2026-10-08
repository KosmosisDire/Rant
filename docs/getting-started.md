# Getting started

Rant is a C99 library shipped as one header, `rant.h`, built from `src/`. Define the
implementation macro in exactly one C file:

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

## Environment

| variable | effect |
|---|---|
| `RANT_NODE_NAME` | replaces the node name the code passes. The `rant` CLI sets it for every node it starts |
| `RANT_NODE_NAME_PREFIX` | goes with `/` in front of the node name, cut to the 32 byte cap |
| `RANT_DOMAIN` | the domain for a node whose code leaves `domain` at 0. Not a number from 0 to 65535 refuses the open with `RANT_E_BAD_DOMAIN` |
| `RANT_PREFIX` | the outer name prefix, in front of `opts.prefix` (see Name prefix) |

All are read once at `rant_node_open`. Set `RANT_DOMAIN` machine wide, or per workspace,
to put every node there on one domain.

## Name prefix

Every name a node creates (topics, functions, tasks, variables) goes under its prefix:
`RANT_PREFIX`, then `opts.prefix`, then the name, joined with `/`.

```c
/* RANT_PREFIX=cellA */
RantNode *n = rant_node_open(&mem, "arm", NULL, NULL, &(RantNodeOpts){ .prefix = "robot1" });
rant_node_create_topic(n, "pose", RANT_PUB_ONLY, NULL, NULL);    /* cellA/robot1/pose */
rant_node_create_topic(n, "/clock", RANT_SUB_ONLY, NULL, NULL);  /* clock */
```

- A name starting with `/` skips the prefix, to reach a shared name or another prefix.
- The full name counts toward `RANT_TOPIC_NAME_MAX` (64). Too long fires `RANT_E_BAD_NAME`.
- A prefix holding `@`, starting or ending with `/`, or too long refuses the open with
  `RANT_E_BAD_PREFIX`.
- Messages, reflection and `rant_node_mesh_find` use full names, with no prefix applied.
- Node names are not prefixed. `RANT_NODE_NAME_PREFIX` does that separately.

## Memory

A `RantAllocator` is the one memory model at every layer. `rant_allocator_dynamic` takes
pages from the heap. `rant_allocator_static(buf, size)` runs over a caller buffer with no
heap at all, for embedded use. Buffers, index maps and lane records size to real traffic
inside whatever bound the allocator enforces. Running out surfaces as a `RANT_E_OOM` error
event, never a silent truncation. spec/allocation.md lists what is allocated when.

## Node options

| option | meaning |
|---|---|
| `domain` | logical network selector, nodes only see their own domain. 0 takes `RANT_DOMAIN` |
| `prefix` | goes with `/` in front of every name this node creates, inside `RANT_PREFIX` (see Name prefix) |
| `max_topics` | how many topics can be created (default 8) |
| `disable_shm` | force the wire path even to a same host peer |
| `match_wait_ms` | first send match wait bound, 0 = 1 s, negative = off (docs/topics.md) |
| `fetch_details` | fetch names and schemas for every topic every peer has, for observer tools |
| `disable_logs`, `disable_meta`, `disable_error_logs` | strip the built in log topics and meta function (docs/node.md) |
| `net` | ports, group, interface, seed peers, unicast only, own locator (docs/discovery.md) |
| `discovery` | announce interval, peer timeout, max peers (docs/discovery.md) |
| `user_data` | surfaced as `RantMsg.user` and `RantEvent.user` |

## Build flags

Define these before the include.

| flag | effect |
|---|---|
| `RANT_IMPLEMENTATION` | emit the implementation, in one file only |
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

A `NO` flag always wins. An embedded target with its own sockets and clock defines
`RANT_PLAT_CUSTOM` and links its own `i_rant_plat_*` functions (spec/platform.md).

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
| `RANT_QUEUE_CAP` | 1 MB | default ring cap of a topic on a callback queue |
| `RANT_DISCOVERY_META_MAX` | 64 | default per peer announce blob capacity |
| `RANT_DISCOVERY_PROTO_VERSION` | 5 | discovery protocol version |

Shared memory tunables are in spec/shm.md.
