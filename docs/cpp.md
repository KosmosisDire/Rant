# C++

`dist/rant.hpp` is a header only C++17 wrapper over the C library, and the only file you
need. It works the same as C, so docs/node.md, docs/topics.md, docs/patterns.md and
docs/tasks.md apply. This page covers what is different in C++. Your editor shows a
comment on every public name when you hover it.

## Setup

One source file builds the library:

```cpp
#define RANT_IMPLEMENTATION
#include "rant.hpp"
```

`dist/rant.cpp` is that file, ready to compile:

```sh
g++ -std=c++17 -Idist app.cpp dist/rant.cpp -o app -lrt -lpthread                        # Linux
g++ -std=c++17 -Idist app.cpp dist/rant.cpp -o app.exe -lws2_32 -liphlpapi -lbcrypt -lwinmm   # Windows
```

It builds with g++, clang++, clang-cl and MSVC, and with `-fno-exceptions -fno-rtti`.

## Node

```cpp
rant::Node node("robot", { .domain = 7 });
```

- The name and the options are both optional. Options left at zero use the defaults.
  `NodeOptions` mirrors docs/getting-started.md's node options, `prefix` included.
- `on_event(handler)` gets peer, loss and error events. With no handler, errors print
  to stderr.
- `settle()` waits until discovery is done. Call it after creating your handles.
- `close()` or the destructor shuts the node down. It first waits until the calls other
  threads are making on the node return. Inside one of the node's own callbacks it runs
  once that callback returns.
- `rant::version()` is the library's version, such as `"0.0.17"`.

Durations are `std::chrono`: write `500ms` or `2s`. `rant::forever` waits with no end.

### Threading

`NodeOptions::threading` picks where callbacks run.

| Mode | Callbacks run |
|---|---|
| `ServiceThread` (default) | on the node's own thread |
| `Manual` | inside your `node.poll()` |
| `Dispatch` | inside your `node.dispatch()`, for a game or UI loop |

```cpp
rant::Node app("app", { .threading = rant::Threading::Dispatch });
while (running) { app.dispatch(); draw(); }
```

To run one handle's callbacks on a thread of your own, pass it a queue from
`node.create_queue()` and call `queue.dispatch()` on that thread.

### Errors

- A failed node or handle creation throws `rant::Error`. Without exceptions, the object
  is not `valid()` and `node.last_error()` says why.
- Sends and sets never throw. They return a `SendStatus`.
- A callback that throws never breaks the node. The error goes to stderr and
  `on_event`. A function or task handler that throws answers the caller `AppError`.

### Memory

`memory` and `memory_size` run the node in one buffer, with no heap and no growth. `bulk`
and `bulk_size` add a second buffer for the big buffers touched rarely: message history and
reassembly, peer records, queue rings. On an ESP32-S3 give the node SRAM and the bulk PSRAM:

```cpp
static uint8_t sram[96 << 10];
rant::Node node("arm", { .memory = sram, .memory_size = sizeof sram,
                         .bulk = heap_caps_malloc(2 << 20, MALLOC_CAP_SPIRAM), .bulk_size = 2 << 20 });
```

## Types

```cpp
struct Conveyor { double speed; bool running; rant::String<16> part; };
RANT_SCHEMA(Conveyor, speed, running, part);
```

`RANT_SCHEMA` goes after the struct at global scope and lists its members in order. C#
and Python see the same type. Member names go on the wire in snake_case, so `frameId` is
`frame_id`.

Supported members:

- numbers and `bool`
- `rant::String<N>` (a string of at most N bytes) and `std::string`
- `T[N]`, `std::array<T, N>` and `std::vector<T>` of any of these, nested up to 8 levels:
  `std::vector<std::string>`, `std::vector<std::vector<float>>`, a vector of structs
  that hold vectors
- other `RANT_SCHEMA` structs
- `enum class` registered with `RANT_ENUM(E, values...)`, which ships as the enum named `E`

Not supported: pointers, maps and `std::vector<bool>`.

A plain type works without `RANT_SCHEMA`: `Publisher<float>`, `Subscriber<std::string>`.

A value that does not fit its type, such as a `String<N>` longer than N, is refused, never
cut short.

The standard types of docs/stdtypes.md are in `rant::types`: `rant::types::Transform`,
`rant::types::OrientedBox`, `rant::types::Empty` and the rest. A new value is all zeros, so a
rotation starts from `rant::types::identity_rotation()`. An enum option named after an X11 macro
takes a trailing underscore, as in `DistortionModel::None_`. Everything that computes on the types
is Rant Math.

`rant_types.hpp` holds them alone, with no middleware, and shares its guard with the copy in
`rant.hpp`, so a library built on the types alone and Rant compile into one program in either order.

### Raw messages

Use `rant::Bytes` as the type to work with schemas at run time:

```cpp
rant::Schema s = node.schema("Chat { seq: u32, text: string }");
auto pub = node.publisher<rant::Bytes>("Chat", {}, s);
rant::MessageBuilder m(s);
m.set_uint("seq", 1).set_string("text", "hi");
pub.send(m);
```

A received `MessageView` reads fields by name, such as `get_uint("seq")`. It is valid
inside the callback only.

## Topics

```cpp
auto pub = node.publisher<Conveyor>("Line1/Conveyor");
auto sub = node.subscriber<Conveyor>("Line1/Conveyor", [](const Conveyor& c) { /* ... */ });
pub.send({ 0.5, true, {} });
```

A subscriber with no handler is pulled: you read messages when you want them.

```cpp
auto sub = node.subscriber<Conveyor>("Line1/Conveyor");
if (auto c = sub.take_latest()) show(*c);
```

- `take()` returns the oldest waiting message, `take_latest()` the newest.
- `match_count()` counts the matched peers.
- `drain()` on a publisher waits until every reader has the messages.
- `Qos` sets reliability, history depth and rate: `{ rant::Reliability::Reliable }`.

## Functions

```cpp
auto add = node.function_definition<AddReq, int>("Add",
    [](const AddReq& r) { return r.a + r.b; });

auto fn = node.remote_function<AddReq, int>("Add");
auto r = fn.call({ 1, 2 });
if (r) std::printf("%d\n", *r.value());
else   std::printf("%s\n", r.message().data());
```

- `if (r)` is true when the call succeeded. `r.status()` says what happened.
- `r.value()` holds the answer when one came back, even on failure.
- `call()` blocks. From inside a callback it throws, so use `call_async()` there.
- To answer later, take `Request<Rsp>&` as a second handler argument and call
  `defer()`. The returned `Deferred` answers from any thread.

## Tasks

A task is a function that reports progress and can be cancelled.

```cpp
auto job = node.task_definition<Goal, float, Result>("Move",
    [](const Goal& g, rant::TaskRequest<float, Result>& t) {
        t.start();
        auto p = t.defer();   // finish on another thread
        worker(std::move(p));
    });

auto move = node.remote_task<Goal, float, Result>("Move");
auto r = move.call(goal, [](const rant::ProgressView<float>& p) { /* *p is the progress */ });
```

- A `PendingTask` sends `progress()`, then one of `complete()`, `fail()` or
  `complete_cancelled()`.
- `cancelled()` tells the worker the caller asked to cancel.
- `CallOptions::id_out` gives the call id so another thread can `cancel()` it.

## Variables

```cpp
auto speed = node.variable_definition<float>("Speed", { .initial = 1.5f });

auto remote = node.remote_variable<float>("Speed");
remote.on_change([](float v) { /* ... */ });
remote.set(2.0f);
```

- `on_change` fires once with the current value, then on every change.
- `on_write` fires on every write, even one with the same value.
- `wait()` blocks until a value exists.

## Names and schemas

- Every handle has `name()`.
- `schema()` on topics and variables, and `request_schema()`, `response_schema()` and
  `progress_schema()` on functions and tasks, give the schema in use.
- A `rant::Bytes` handle with `reflect_from_mesh` set takes its schema from the peers.
  `refresh()` updates it when they change.

## Lifetime

- Handles are move only. Scope end or `close()` releases the name.
- `close()` first waits until the calls other threads are making on the handle return.
  Closing a remote then cancels its async calls.
- A handle cannot close itself from inside its own callback.
- A handle that outlives its node is safe. Its calls just fail.

## Reflection, logs and status

```cpp
node.log(rant::Level::Info, "started");
node.on_log([](const rant::LogLine& l) { /* l.level, l.origin, l.node, l.text */ });

node.set_status("drive2", rant::Level::Error, "drive 2 tripped", /*clearable=*/true);
node.clear_status("drive2");
node.on_status([](const rant::StatusUpdate& u) { /* u.node, u.conditions */ });

auto r = node.reflection();
for (auto& p : r.peers()) std::printf("%s\n", p.name.c_str());
```

- `on_log` receives every other node's log lines. `NodeOptions::log_level` is the lowest
  level this node sends, empty takes `RANT_LOG_LEVEL`, else Info.
- `set_status` raises or updates a condition, `clear_status` clears it. `on_status`
  receives each node's whole set on every change, and an empty set when a node goes away.
- `clear_peer_status(u.node_id, c.key, c.since_us)` asks another node to clear a clearable
  condition. Its app sees `EventKind::StatusCleared` with the key in `detail()`.
- `reflection()` lists peers, their entities and the whole mesh. `epoch()` changes when
  anything does.
- `meta(peer)` asks a peer for its details.
- `node.stats()` shows memory use and send counters.
