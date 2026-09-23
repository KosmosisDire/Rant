# C++

`dist/rant.hpp` is a header only C++17 wrapper over the C library, and the one file a
consumer needs: the C single header is embedded inside it. The semantics are the C ones,
so docs/node.md, docs/topics.md, docs/patterns.md and docs/tasks.md apply. This page says
what is different in C++.

## The implementation anchor

Exactly one translation unit defines `RANT_IMPLEMENTATION` before the include. That unit
emits the C implementation at global scope with C linkage. It may be a `.cpp` file, so a
pure C++ project needs no C compiler, or a `.c` file, and a `.c` file that includes the
header with nothing defined is treated as the anchor. Every other translation unit gets
the C declarations inside `rant::detail` and nothing at global scope.

```cpp
#define RANT_IMPLEMENTATION
#include "rant.hpp"
```

`dist/rant.cpp` is exactly that, generated, so a project compiles it instead of writing one.

```sh
g++ -std=c++17 -Idist bindings/cpp/example.cpp dist/rant.cpp -o example -lrt -lpthread
g++ -std=c++17 -Idist bindings/cpp/example.cpp dist/rant.cpp -o example.exe -lws2_32 -lbcrypt -lwinmm
```

The header is clean under `-fno-exceptions -fno-rtti` and is verified on MinGW g++,
clang++, clang-cl and MSVC.

## A node

```cpp
rant::Node node("robot1", { .domain = 7 });     // the service thread runs from here
node.on_event([](const rant::Event& e){ std::fprintf(stderr, "%s\n", e.to_string().c_str()); });
rant::Qos reliable{ rant::Reliability::Reliable };
auto out = node.publisher<rant::Bytes>("chat", reliable);
auto in  = node.subscriber<rant::Bytes>("chat",
    [](const rant::MessageView& m){ /* every delivery on chat */ }, reliable);
out.send("hello");
```

`on_event` is optional: with no handler set, error events print to stderr, and
`last_error()` records the last error either way. A later call replaces the handler.
Options are plain structs mirroring the C ones, and all zero means every default. `Qos::queue_bytes`, `Qos::max_rate_hz` and
`Qos::no_timestamp` are the C fields of the same meaning.

Every handle comes from a factory on the node named after it: `publisher`, `subscriber`,
`function_definition`, `remote_function`, `task_definition`, `remote_task`,
`variable_definition` and `remote_variable`. A default constructed handle is empty.

Every failed factory or constructor throws `rant::Error`, which carries the error kind, the OS errno
of a socket fault and the formatted text as `what()`. With `-fno-exceptions` nothing
throws: the object is not `valid()` and `node.last_error()` holds the text. Data path
results are `SendStatus` and the status enums in both modes.

`NodeOptions::threading` says who runs the loop and where every callback fires:
subscriber and pattern handlers, `on_event`, `on_log`, progress and async responses.

- `Threading::ServiceThread`, the default: the C service thread runs from construction and
  every callback fires on it.
- `Threading::Manual`: your thread calls `poll(timeout_ms)` for one loop tick and callbacks
  fire there. `poll` on another threading answers `SendStatus::State`.
- `Threading::Dispatch`: the service thread runs, and every callback of a handle made
  without a queue, every event and every log line waits on the node's queue until
  `node.dispatch(max, timeout_ms)` runs them on the calling thread.

```cpp
rant::Node app("app", { .threading = rant::Threading::Dispatch });
auto pose = app.subscriber<Pose>("robot/pose", on_pose);
while (running) { app.dispatch(0, 16); draw(); }
```

`settle()` blocks until discovery and matching have converged, call it after creating the
topics. From inside an inline handler, sends and read only queries are allowed, and poll,
create, drain and close are refused with `SendStatus::State`. `close()` stops the loop,
leaves with a bye and frees the node, as the destructor does. Close before the state your
handlers capture goes out of scope.

`create_queue()` returns a `Queue` for the case where handles need a thread of their own,
such as a worker draining a slow provider while the UI thread drains the node queue. A
handle whose options name it (`Qos::queue`, or `queue` in the function, task and variable
options) parks its callbacks until `queue.dispatch(max, timeout_ms)` runs them on the
calling thread. Same name topic handles must agree on the queue. A `Queue` is non owning
and lives as long as its node.

Memory is configured on `NodeOptions::memory`: a buffer plus size means static mode, where
the node draws all its memory from the buffer, never grows, and turns the shared memory
path off. The node's schemas live in that buffer too, so a static node is heap free end
to end.

## Topics and messages

Every handle is a template over its message type, and `rant::Bytes` as the type argument
is the raw form: `node.publisher<rant::Bytes>(name, qos, schema)` sends a `MessageBuilder`
or any bytes, `node.subscriber<rant::Bytes>(name, handler, qos, schema)` delivers a
`MessageView` that reads fields by name. An empty schema means untyped bytes. Only a
`rant::Bytes` handle takes a schema, a typed one refuses it. A publisher and a subscriber
of the same name share one topic slot with a widened role. A live same name topic with a
different schema refuses, so `retire()` the old one first to retype a name. After a
successful retire every handle sharing the slot is invalid and the next factory call for
the name creates fresh. `match_count()` on any handle counts the matched
counterparts, and `drain(timeout_ms)` on a publisher waits until every reader has acked,
the flush before close.

A subscriber made without a handler is pulled: `take(timeout_ms)` returns the oldest
waiting message and `take_latest(timeout_ms)` the newest, dropping the older ones, as a
`std::optional<T>`, or a `std::optional<MessageView>` valid until the next take for
`rant::Bytes`. A pulled subscriber takes no `Qos::queue`, and `take` on one with a handler
throws.

```cpp
auto poses = node.subscriber<Pose>("pose");     // no handler: pulled
if (auto p = poses.take_latest()) draw(*p);
```

`Bytes` is a non owning view. It constructs from `std::string_view`, `std::string`, a C
string or any contiguous range of byte sized elements, and converts to `string_view`,
`string`, `vector` and `std::span` where available. Multi byte element types are rejected,
pass their raw bytes with `Bytes(ptr, len)`.

`node.schema(text)` compiles in the node's registry and returns a `Schema`, a handle
valid for the node's life. It throws `rant::Error`, or without exceptions returns an empty
handle with `last_error()` saying where. Every definition stays in scope for the node's
later compiles, and `node.schema_from_wire(bytes)` registers a peer's wire. The typed
codec of `RANT_SCHEMA` is built once per node and type. `MessageBuilder` sets fields by name or dotted path, grows for variable fields,
and refuses an over cap value by flipping `ok()` to false rather than truncating.
`set_array_count("pts", n)` sizes a variable array, and a struct array's members then set
by path with one index per level, `pts[2].corners[1].x`. Reads go through `FieldView`,
the surface shared by `MessageView`, `Request<rant::Bytes>` and `ResponseView<rant::Bytes>`,
so a typed read looks the same everywhere, and its `get_array_count` gives an array's live
count. Every handler
fires on the polling thread, and the views it receives are valid for the callback only.

A map field is written with `MapWriter` and read with `MapReader`, thin layers over the C
map codec. `MapReader::to_map()` decodes the whole tree into an owning `MapDict` of
`MapItem` that outlives the handler. Numeric getters coerce between integer and double
kinds, and a type mismatch yields the zero value rather than throwing.

## Typed messages

```cpp
struct Pose { double x, y; rant::String<16> frame; };
RANT_SCHEMA(Pose, x, y, frame);
auto pub = node.publisher<Pose>("pose");
pub.send({ 1.0, 2.0, {} });
```

`RANT_SCHEMA(T, fields...)` goes at global scope after the struct, listing up to 64
members in wire order. The wire name is the type name with namespace qualifiers stripped,
and a member's wire name is its own name in the wire spelling, camelCase (docs/stdtypes.md),
so `frame_id` is `frameId` on the wire and in the DSL the codec prints. A nested
reflected struct spells as its type name, defined once above the root, so
`Shape { Corner origin; }` sends `Corner { ... }` then `Shape { origin: Corner }`, the
same text and hash C# and Python send.
On first use the codec synthesizes the DSL, compiles it through the C compiler, and builds
a flat copy table. A padding free struct on a little endian host encodes and decodes with
one memcpy, anything else runs a per field loop. A delivery whose schema hash differs from
ours rebuilds the offsets from the incoming schema and caches them per schema pointer.

Wire types are the sized integers, float, double, bool, `T[N]` and `std::array<U, N>` of
those or of structs, `rant::String<N>` for a capped string, nested reflected structs, and
the variable members `std::vector` of scalars, of structs or of `rant::String<N>`, and
`std::string`. A fixed struct array spells `corners: Float2[4]` and may sit inside
another's element to any depth. A `std::vector<Outline>` spells `Outline[]`, and its
element must be fixed all the way down. Each of these also works as a handle's whole type.
A variable member rides the message tail as a length framed section, so a type with one
encodes into scratch and its decode allocates into the member. Refused at compile time:
pointers, maps, `std::vector<bool>` and `std::vector<std::string>`. Those shapes use the
dynamic `Schema` and `MessageBuilder` API.

Any wire type used directly as a handle's type is a bare schema with no `RANT_SCHEMA`:
`Publisher<bool>`, `RemoteVariable<float>`, `Subscriber<std::string>` or
`Publisher<std::vector<float>>`. A bare type is anonymous, so it is the same bytes and
the same hash from every language.

`RANT_ENUM(E, options...)` registers an `enum class` so a member ships as a named
`enum<uN>` with the enumerators as options. An unregistered enum ships as its backing
integer and matches an `enum<uN>` by width only.

`rant::String<N>` is the capped string slot. `assign()` refuses an over capacity value and
`view()` clamps a hostile length.

## Standard types

The roster in docs/stdtypes.md is mirrored in the `rant::types` namespace:
`rant::types::Timestamp`, `rant::types::Transform`, `rant::types::Color`,
`rant::types::Uuid` and the rest, with their helpers such as `rotate` and
`color_from_hex`. The namespace keeps `Color` or `Quaternion` from clashing with an engine
type under `using namespace rant;`. Each fixed mirror is standard layout and identical to
the wire, so the memcpy path applies. `Image` and `VideoFrame` carry a
`std::vector<uint8_t>` data member, so they take the tail path, and they nest as a member
but never as an array element. `rant::types::now()` and `rant::types::new_uuid()` are the
two values that need the platform.

## Functions, tasks and variables

The handles are `FunctionDefinition<Req, Rsp>`, `RemoteFunction<Req, Rsp>`,
`TaskDefinition<Req, Prg, Rsp>`, `RemoteTask<Req, Prg, Rsp>`, `VariableDefinition<T>` and
`RemoteVariable<T>`, from `node.function_definition<Req, Rsp>(name, handler, options)`
and the other factories. With `rant::Bytes` as every type argument the same handle is the
raw form, and the factory takes the schemas after the options. A definition needs a
handler. Every handle is thin and non owning: the entity lives in the node until close.

```cpp
auto add = node.function_definition<AddReq, AddRsp>("add",
    [](const AddReq& r) { return AddRsp{ r.x + r.y }; });
auto fn  = node.remote_function<AddReq, AddRsp>("add");
auto speed = node.variable_definition<float>("speed", { .initial = 1.5f });
```

A function handler is either `Rsp(const Req&)`, where the return value is the reply, or
`void(const Req&, Request<Rsp>&)`, which replies, fails or defers explicitly. Returning
from the full form without replying acknowledges OK with an empty payload. A handler that
throws answers `CallStatus::AppError` with the exception text and never unwinds into the
C. `defer()` returns a movable single shot `Deferred<Rsp>` that completes from any thread.
Dropping it unanswered leaves the caller to its timeout.

A task handler is `void(const Req&, TaskRequest<Prg, Rsp>&)`. It answers inline, or calls
`start()` and `defer()` and returns. `defer()` yields a movable `PendingTask` whose verbs
are thread safe: `progress()` any number of times, then exactly one of `complete()`,
`fail()` or `complete_cancelled()`, after which the handle is empty and a stale verb gets
`SendStatus::State`. `cancelled()` polls the caller's request. Returning from a task
handler with no reply, fail or defer answers AppError, since an instant empty OK on a long
operation would read as success. Complete or drop a `PendingTask` before retiring its
definition.

A blocking `call()` waits on the service thread's progress under `start()` and drives
the node loop otherwise. From an inline callback it is refused with `SendStatus::State`, so use
`call_async()` there. A
`Response` owns its payload, and `message()` is the provider's text or the default
status text on any non OK outcome. On a task, the timeout bounds only the wait for the
first response, `CallOptions::id_out` receives the call id at commit so another thread
can `cancel()`, and `cancel()` answers `BadRole` when the provider declared no cancel
and `State` when the call is not pending.

A variable's `on_change` handler replays the current value at registration and then fires
on every state change. `on_write` fires on every applied write, identical bytes or not.
Both run inline on the thread that applied the write, or at `dispatch()` of the handle's
queue. `RemoteVariable::wait()` blocks,
driving the loop, until a value exists.

A `rant::Bytes` handle made without a schema and with `reflect_from_mesh` set in its
options types itself from the mesh: a reader takes its provider's schema, a writer the
widest every reader accepts. `refresh()` re types the handle in place when the mesh moved.

Every handle has `retire()`. It is refused with `SendStatus::State` from a callback, and
after success the handle is empty. Retiring a remote completes every outstanding call
with `CallStatus::Cancelled`.

## Logs, meta and reflection

`Node::log(level, text)` publishes on a level's built in topic, with printf style
overloads that truncate at `RANT_LOG_MAX`. `on_log(handler)` delivers every other node's
lines at every level as a `LogLine`, whose `level` says which, where the node's callbacks
run. It holds one handler: a later call replaces it and `{}` clears it. It throws, or
returns false, on a node opened with `disable_logs`.

`node.reflection()` returns a `Reflection` that holds the walks of docs/reflection.md.
`peers()`, `entities(peer)` and `mesh()` return owned snapshots and `find(kind, name)` one
folded entity. An `Entity` name is the hash placeholder until the peer's details arrive.
`epoch()` moves on every reflected change, so a UI re walks only when it moved.
`meta(peer, sections, timeout_ms)` sends a directed `@rant/meta` call and decodes the reply
into an owning `MetaSnapshot`: the node and proc scalars are pulled out and the whole body
stays in `info` as a `MapDict`. `meta_async(peer, handler, sections)` is the same with the
handler firing where the node's callbacks run. `sections` is a mask of `MetaSection` bits,
0 for all.
`stats()` is the node's own counters: memory, the reliable send waits and the sends that
evicted never sent history.
