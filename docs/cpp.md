# C++

`dist/ramble.hpp` is a header only C++17 wrapper over the C library, and the one file a
consumer needs: the C single header is embedded inside it. The semantics are the C ones,
so docs/node.md, docs/topics.md, docs/patterns.md and docs/tasks.md apply. This page says
what is different in C++.

## The implementation anchor

Exactly one translation unit defines `RAMBLE_IMPLEMENTATION` before the include. That unit
emits the C implementation at global scope with C linkage. It may be a `.cpp` file, so a
pure C++ project needs no C compiler, or a `.c` file, and a `.c` file that includes the
header with nothing defined is treated as the anchor. Every other translation unit gets
the C declarations inside `ramble::detail` and nothing at global scope.

```cpp
#define RAMBLE_IMPLEMENTATION
#include "ramble.hpp"
```

`dist/ramble.cpp` is exactly that, generated, so a project compiles it instead of writing one.

```sh
g++ -std=c++17 -Idist cpp/example.cpp dist/ramble.cpp -o example -lrt -lpthread
g++ -std=c++17 -Idist cpp/example.cpp dist/ramble.cpp -o example.exe -lws2_32 -lbcrypt -lwinmm
```

The header is clean under `-fno-exceptions -fno-rtti` and is verified on MinGW g++,
clang++, clang-cl and MSVC.

## A node

```cpp
ramble::Node node("robot1",
    [](const ramble::MessageView& m){ /* every delivery */ },
    [](const ramble::Event& e){ std::fprintf(stderr, "%s\n", e.to_string().c_str()); },
    { .domain = 7 });
ramble::Topic chat(node, "chat", ramble::Role::PubSub, nullptr,
                 { .reliability = ramble::Reliability::Reliable });
node.start();
chat.send("hello");
```

The message handler may be empty, since typed subscribers and pattern handles carry their
own. The event handler is required. Options are plain structs mirroring the C ones, and
all zero means every default. `Qos::queue_bytes`, `Qos::max_rate_hz` and
`Qos::no_timestamp` are the C fields of the same meaning.

Every failed constructor throws `ramble::Error`, which carries the error kind, the OS errno
of a socket fault and the formatted text as `what()`. With `-fno-exceptions` nothing
throws: the object is not `valid()` and `Node::last_open_error()` holds the text. Data
path results are `SendStatus` and the status enums in both modes.

`poll(timeout_ms)` runs one loop tick, or `start()` runs the C service thread and
`stop()` joins it. `settle()` blocks until discovery and matching have converged, call it
after creating the topics. From inside a handler, sends and read only queries are
allowed, and poll, topic create, set_role, drain and stop are refused with
`SendStatus::State`. `dispatch_all()` drains every queued topic on the calling thread.

Memory is configured on `NodeOptions::memory`: a buffer plus size means static mode, where
the node draws all its memory from the buffer, never grows, and turns the shared memory
path off. A typed topic copies its schema into that buffer, so pair it with the
`Schema::compile(text, scratch, size)` overload for a heap free node.

## Topics and messages

`Topic(node, name, role, schema, qos)` creates a topic or shares the same name slot with
a widened role. A live same name topic with a different schema refuses, so `retire()` the
old one first to retype a name. After a successful retire every handle sharing the slot is
invalid and the next construction of the name creates fresh.

`Bytes` is a non owning view. It constructs from `std::string_view`, `std::string`, a C
string or any contiguous range of byte sized elements, and converts to `string_view`,
`string`, `vector` and `std::span` where available. Multi byte element types are rejected,
pass their raw bytes with `Bytes(ptr, len)`.

`Schema::compile(text)` returns an empty optional on error and fills an optional error
string. `MessageBuilder` sets fields by name or dotted path, grows for variable fields,
and refuses an over cap value by flipping `ok()` to false rather than truncating. Reads go
through `FieldView`, the surface shared by `MessageView`, `Message<>`, `Request<>` and
`ResponseView<>`, so a typed read looks the same everywhere.

`take()` and `dispatch()` switch a topic to queued delivery, as in docs/node.md. A
`Message<>` from `take()` owns its envelope but its data views point into the ring and
stay valid until the next take or dispatch on that topic. A typed `Message<T>` owns the
decoded value. Handlers run by `dispatch()` execute on the calling thread without the
node lock and may use the full API.

A map field is written with `MapWriter` and read with `MapReader`, thin layers over the C
map codec. `MapReader::to_map()` decodes the whole tree into an owning `MapDict` of
`MapItem` that outlives the handler. Numeric getters coerce between integer and double
kinds, and a type mismatch yields the zero value rather than throwing.

## Typed messages

```cpp
struct Pose { double x, y; ramble::String<16> frame; };
RAMBLE_SCHEMA(Pose, x, y, frame);
ramble::Publisher<Pose> pub(node, "pose");
pub.send({ 1.0, 2.0, {} });
```

`RAMBLE_SCHEMA(T, fields...)` goes at global scope after the struct, listing up to 64
members in wire order. The wire name is the type name with namespace qualifiers stripped.
On first use the codec synthesizes the DSL, compiles it through the C compiler, and builds
a flat copy table. A padding free struct on a little endian host encodes and decodes with
one memcpy, anything else runs a per field loop. A delivery whose schema hash differs from
ours rebuilds the offsets from the incoming schema and caches them per schema pointer.

Wire types are the sized integers, float, double, bool, `T[N]` and `std::array<U, N>` of
those, `ramble::String<N>` for a capped string, nested reflected structs, and the variable
members `std::vector<scalar>` and `std::string`. A variable member rides the message tail
as a length framed section, so a type with one encodes into scratch and its decode
allocates into the member. Refused at compile time: pointers, maps, `std::vector<bool>`,
vectors of structs or strings, and struct array members. Those shapes use the dynamic
`Schema` and `MessageBuilder` API.

Any wire type used directly as a handle's type is a bare schema with no `RAMBLE_SCHEMA`:
`Publisher<bool>`, `RemoteVariable<float>`, `Subscriber<std::string>` or
`Publisher<std::vector<float>>`. A bare type is anonymous, so it is the same bytes and
the same hash from every language.

`RAMBLE_ENUM(E, options...)` registers an `enum class` so a member ships as a named
`enum<uN>` with the enumerators as options. An unregistered enum ships as its backing
integer and matches an `enum<uN>` by width only.

`ramble::String<N>` is the capped string slot. `assign()` refuses an over capacity value and
`view()` clamps a hostile length.

## Standard types

The roster in docs/stdtypes.md is mirrored as `ramble::Timestamp`, `ramble::Transform`,
`ramble::Color`, `ramble::Uuid` and the rest. Each fixed mirror is standard layout and
identical to the wire, so the memcpy path applies. `Image` and `VideoFrame` carry a
`std::vector<uint8_t>` data member, so they take the tail path, and they nest as a member
but never as an array element. `ramble::now()` and `ramble::new_uuid()` are the two values
that need the platform.

Name your own type the same way: `RAMBLE_STD_STRUCT(T, fields...)` reflects the members and
names the type, and `RAMBLE_STD_ALIAS(T, R)` names a type that copies as `R`.

## Functions, tasks and variables

The typed handles are `FunctionDefinition<Req, Rsp>`, `RemoteFunction<Req, Rsp>`,
`TaskDefinition<Req, Prg, Rsp>`, `RemoteTask<Req, Prg, Rsp>`, `VariableDefinition<T>` and
`RemoteVariable<T>`. Each has an untyped `<>` twin over `Bytes`, used by the bridge and
the explorer. Every handle is thin and non owning: the entity lives in the node until
close.

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

A blocking `call()` drives the node loop and is refused with `SendStatus::State` from a
callback or while a service thread owns the loop, so use `call_async()` there. A
`Response<>` owns its payload, and `message()` is the provider's text or the default
status text on any non OK outcome. On a task, the timeout bounds only the wait for the
first response, `CallOptions::id_out` receives the call id at commit so another thread
can `cancel()`, and `cancel()` answers `BadRole` when the provider declared no cancel
and `State` when the call is not pending.

A variable's `on_change` handler replays the current value at registration and then fires
on every state change. `on_write` fires on every applied write, identical bytes or not.
Both run inline on the thread that applied the write. `RemoteVariable::wait()` blocks,
driving the loop, until a value exists.

Pass `ramble::reflect_from_mesh` where a constructor takes a schema pointer and the handle
types itself from the mesh: a reader takes its provider's schema, a writer the widest
every reader accepts. `refresh()` re types the handle in place when the mesh moved.

Every handle has `retire()`. It is refused with `SendStatus::State` from a callback, and
after success the handle is empty. Retiring a remote completes every outstanding call
with `CallStatus::Cancelled`.

## Logs, meta and reflection

`Node::log(level, text)` publishes on a level's built in topic, with printf style
overloads that truncate at `RAMBLE_LOG_MAX`. `on_log(level, handler)` widens the node's own
log handle and delivers every other node's lines at that level as a `LogLine`, whose views
are valid for the callback only. Call it once per level from setup.

`meta_request(peer, handler, sections)` sends a directed `@ramble/meta` call and decodes the
reply into an owning `MetaSnapshot`: the node and proc scalars are pulled out and the whole
body stays in `info` as a `MapDict`. `sections` is a mask of `MetaSection` bits, 0 for all.
`meta()` is the raw caller handle for anything else.

`peers()`, `entities(peer)` and `mesh()` return owned snapshots, as in docs/reflection.md.
An `Entity` name is the hash placeholder until the peer's details arrive.
`mesh_generation()` bumps on every reflected change, so a UI re walks only when it moved.
