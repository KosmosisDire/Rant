# Bindings

Five API surfaces share one C core: C, C++ (`bindings/cpp/rant.hpp`), C# (`bindings/csharp/Rant.cs` plus
the Unity copy), Python (`bindings/python/rant/__init__.py`) and JS through the bridge. The cross
language rules below were argued once and apply everywhere. Where anything else disagrees
about a wrapper API, this file wins.

## Cross language rules

- DEFINITION and REMOTE vocabulary everywhere: `create_function_definition`,
  `create_remote_function`, `create_variable_definition`, `create_remote_variable`.
- `call` is the completed transaction in every language. `_async` marks the callback or
  Task form. JS has no callAsync, the Promise is it.
- Match queries in C++ are side named: the remote side exposes a bool `has_definition`,
  the definition side `caller_count` or `remote_count`. Python and C# spell every one
  `match_count` and `MatchCount`, the count of the other side, except that a C# or Python
  subscriber has none since the C exposes no publisher count on the subscribing side.
  C keeps its generic primitives.
- A call outcome is a value, never a fault. C# `CallAsync` always completes with a
  Response, and reading `.Value` off OK throws. JS `call()` rejects only on connection loss.
- Variable reads are local and status free. Writes return a status. C# `Value` has a
  setter that throws on a non OK status. A remote set round trips through the definition.
  The cache is authoritative only, never an optimistic echo.
- Construction failures throw or reject. Data path outcomes are statuses and events. C++
  has a constructor, not `open`, and throws `rant::Error`. `-fno-exceptions` degrades to
  `valid()` and `last_open_error()` at compile time.
- Node open takes `(name, on_message, on_event, opts)`. `on_message` is nullable, since
  typed subscribers and pattern handles carry their own handlers. `on_event` is validated
  non null by every wrapper so the first diagnostics are never missed. Setter methods
  exist only for later rebinding. C# and Python are the exception: `new RantNode(name,
  NodeOptions)` and `Node(name, **options)` have no node level message handler and
  subscribers carry every one. C# events are the optional `OnEvent` C# event since
  `LastError` records the last diagnostic either way, and Python's optional `on_event`
  keyword prints to stderr when unset.
- Handlers come in two forms: payload only, or payload plus message envelope. C# has one:
  every handler is a C# event (`OnMessage`, `OnChange`, `OnWrite`, `OnEvent`, `OnLog`)
  whose delegate takes the value and the envelope, discarded with `_` when unwanted.
- An owning message copies its payload, since the transport buffer is reused after the
  callback, but decodes only when the fields or the typed value are read. A handler that
  wants the bytes must not pay for the topic's schema. The decode outlives the callback,
  so the wrapper holds its own `rant_schema_copy` of the publisher's schema, made once
  per distinct hash and released with the last message that names it, never at close.
- An owning type is a plain noun, a view type has a `View` suffix, "Taken" is banned:
  `Message`, `MessageView`, `MessageBuilder`, `Response`, `ResponseView`.
- `Peer::entities` replaces any `Peer::topics`. No dual surfaces.
- A pending call at close gets exactly one synthesized outcome, `RANT_CALL_CANCELLED`,
  fixed in C so every binding inherits it.
- JS: the client class is `RantNode` (`await RantNode.connect(url)`). Handles come from
  async factory methods. The schema is a positional argument, never inside an opts object.
  A null schema means raw or payload less.
- A reflected field table mirrors the C `RantSchemaFieldInfo` exactly, everywhere,
  including `type_name`, `elem_name`, `elem_size` and `arr_parent`. A dropped `type_name`
  turns `color: Color` into an anonymous struct and every named reader then refuses the
  sender. Only `rant_schema_print` spells DSL text.
- A public feature lands in every binding. Nothing lists the exports: a declaration
  carrying `RANT_API` in a public header is exported and nothing else is, so a new entry
  point is reachable from every binding the moment it is declared. Bindings format log
  text in their own runtime and call `rant_node_log_text`, never the variadic
  `rant_node_log` over FFI.
- A thrown handler in C#, Python or JS answers APP_ERROR with the exception text.
- Every wait is a probe plus an event plus a bounded block. No blocking only APIs.

Deferred, confirmed: a C reflection macro (`serialize/reflect.h` over C11 `_Generic`),
variable external storage, name directed calls, a `call_all` keyed response list.

## The hand mirrored struct footgun

C# and Python hand mirror the C structs (`RantAllocator`, `RantNodeOpts`, `RantQos`,
`RantMsg`, `RantEvent`, `RantCallOpts` and friends) with no compile time check. When a C
struct gains a field and the mirror does not, every node open crashes with an access
violation. `RantEvent` is returned by value by `rant_last_error`, so a missing trailing
field is an out of bounds read by the C formatter. After any layout change to a public
struct: update the C#, Unity and Python mirrors, rebuild the prebuilt native library, and
run `bindings/csharp/test` and `bindings/python/test.py`. Import alone succeeds silently with a wrong
struct. C++ is immune because it embeds the real header.

## C++

`bindings/cpp/rant.hpp` is a header only C++17 wrapper. The packer splices `rant.h` in, so
`dist/rant.hpp` is the one file a consumer needs. Regenerate after any edit. Verified on
MinGW g++, clang++, clang-cl and MSVC, and clean under `-fno-exceptions -fno-rtti`.

- Surface: `Node`, `Topic`, `Publisher<T>`, `Subscriber<T>`, `FunctionDefinition<Req,Rsp>`,
  `RemoteFunction<Req,Rsp>`, `TaskDefinition`, `RemoteTask`, `VariableDefinition<T>`,
  `RemoteVariable<T>`, `Schema`, `MessageBuilder`, `MessageView`, `Message`, `Response`,
  `ResponseView`, `Event`, `Peer`, `Entity`, `Bytes`, `Qos`, `NodeOptions`, `MapWriter`,
  `MapReader`. Every typed handle has an untyped `<void>` twin for the bridge and explorer.
- `RANT_SCHEMA(T, fields...)` synthesizes DSL text and compiles it through
  `rant_schema_compile`, so the C compiler stays the single source of wire truth. It builds
  a flat codec table and uses memcpy only when the type is trivially copyable, little
  endian and padding free. The decode cache is keyed per incoming schema pointer, not per
  hash, because a rebased schema keeps our hash with the publisher's offsets.
- Tail members: `std::vector<scalar>` and `std::string` struct members ride their tail
  frame through the C accessors by dotted path. Bare `std::vector<E>` roots work. Still
  refused at compile time: `vector<bool>`, vectors of structs or strings, struct array
  members, maps, pointers.
- `rant::std_type<T>` names a user type the way the standard roster is named. `RANT_ENUM`
  registers an `enum class`. Unregistered enums ship as their backing integer.
- With `RANT_IMPLEMENTATION` the C header embeds at global scope. Otherwise declarations
  embed inside `namespace rant::detail`. A `.cpp` anchor must spell
  `#define RANT_IMPLEMENTATION` itself, since the auto define is guarded by
  `!defined(__cplusplus)`.
- Memory is configured on `NodeOptions`: a buffer plus size means static mode, otherwise
  dynamic. One allocator per node, never shared, because open copies it and close resets
  it. `Schema::compile` has a zero heap static scratch overload.
- Strings are `std::string_view`. `Bytes` is a contiguous range convertible to
  string_view, string and vector, with `std::span` where available. Field name parameters
  stay `const char*` on purpose.
- `Topic` carries a `void* impl_` back pointer to `Node::Impl` because `Node` is incomplete
  at the class. `retire()` erases the name cache entry.
- `defer()` on a task handler returns a movable thread safe `PendingTask`.

## C#

`bindings/csharp/Rant.cs` is a hand written P/Invoke layer over a prebuilt native library named
`rant`, resolved from a NuGet package's `runtimes/<rid>/native/` or Unity's
`Assets/Plugins/`. Nothing is generated.

- Collision prone nouns are Rant prefixed: `RantNode`, `RantEvent`, `RantMessage`,
  `RantRequest`, `RantResponse`. Domain names (`Publisher<T>`, `Subscriber<T>`,
  `Variable<T>`) stay bare. The C mirror structs carry a `Native` suffix.
- Every handle is the typed generic. A `byte[]` type argument carries the message bytes as
  is, so there is no untyped tier: the engines behind the generics are internal.
- Every handle comes from a method on `RantNode` named after it, such as
  `node.Publisher<T>(name, qos)`, with one options object per kind (`NodeOptions`,
  `FunctionOptions`, `TaskOptions`, `VariableOptions`). Creation throws on failure. Every
  handle is `IDisposable`, and same name topic handles share one native slot whose role
  follows the live handles and which the last dispose retires. `NodeOptions.Threading` is
  ServiceThread, Manual or Dispatch: Dispatch creates one `RantQueue` at open, hands it to
  every handle and to the events, and `node.Dispatch()` drains it. `RantQueue` from
  `CreateQueue()` goes into a handle's options (`Qos.Queue` on a topic) for a thread of
  its own, and same name handles must agree on it. `CallAsync` returns a Task that never
  faults, with continuations kept off the loop thread unless the handle is queued.
- Typed messages by reflection: any struct or class with public fields. `[RantSchema("Name")]`
  overrides the type name, `[RantArray(N)]` marks fixed arrays, `[RantString(cap)]` is
  required on every string field, `[RantField("name")]` overrides a wire name. Fields
  order by MetadataToken. Wire names default to the C# names, so cross language interop
  needs lowercase `[RantField]` overrides (the std mirrors silently decoded to zeros
  without them). An over cap string throws.
- Callbacks are static trampolines dispatched by an int id in `user_data`, marked
  `[MonoPInvokeCallback]` for IL2CPP. Allocators are managed callbacks over
  `Marshal.ReAllocHGlobal`, which pays a native to managed transition per page.
  `rant_allocator_heap` and `rant_heap_realloc` are the library's own heap hooks and cost
  no transition.
- Stale DLL trap: a stale `rant.dll` presents as memory corruption or peers that never
  match. Check the timestamp of the copy next to the executable against
  `dist/native/<rid>/`, which the `rant_shared` target overwrites on every build.
- Task handlers are async delegates with a real `CancellationToken`. They run on the poll
  thread until their first await, so CPU work belongs in `Task.Run`.
- Unity (`bindings/csharp/unity/`) is a subset. `tools/unity.cmake` assembles the package into
  `dist/com.rant.rant/`: `bindings/csharp/Rant.cs`, `bindings/csharp/unity/Runtime/`, the `dist/native/`
  libraries for win-x64, linux-x64 and osx, a `package.json` stamped from `VERSION`, and
  a `.meta` per entry whose GUID is the md5 of its path so an upgrade keeps references.
  Each plugin `.meta` enables exactly its own platform, since the three libraries share
  the name `rant`. The scene MonoBehaviour is `RantNodeUnity`. Unity only
  registers a MonoBehaviour whose class name matches its file name, and fails silently
  otherwise. Pattern: open in `Threading.Dispatch` and call `Dispatch()` per frame, which
  runs every callback on the frame. OnDisable closes and a beforeAssemblyReload backstop
  stops the service thread. The Unity path has never run in
  a real editor here.

## Python

`bindings/python/rant/__init__.py` is the package and `bindings/python/rant/_native.py` the private ctypes
side: the struct mirrors, the callback types and the loader, referenced from the package
as `_c.*` so none of it shows up on `rant.`. The loader finds the library `rant_shared`
builds: `RANT_LIBRARY`, else the copy the wheel carries next to it, else
`dist/native/<rid>/` in a source checkout. scikit-build-core drives the wheel from
`pyproject.toml`, so `pip install .` runs this same CMake with the tools
and install off and the one `SKBUILD` rule copies the library next to the package. The
wheel is `py3-none-<platform>`, since ctypes needs no Python ABI, so one wheel per
platform serves every interpreter.

- `Node(name=None, *, on_event, **options)` plus a factory method per handle named after
  it, as C#: `node.publisher(name, schema, **options)`, `node.subscriber(name, schema,
  handler, **options)`, `node.function_definition(name, handler, req, rsp, **options)`
  and so on. The handle classes exist for annotations (`rant.Publisher[Pose]`) and are
  never constructed by the user. `Schema(source)` compiles DSL text, a class or a bare
  type. Options are keyword only and spelled out, no option objects. Durations and
  timeouts are float seconds (`_ms` and `_us` convert), timestamps stay integer
  microseconds. Every handle has `close()`, which returns False where C# would throw, and
  same name topic handles share one slot with a hold count per role. Queries with no
  argument are properties (`match_count`, `ready`, `forced`, `counts`, `stats`,
  `last_error`), actions are methods. Variables are methods only. Handlers are arity
  dispatched and single: a subscriber's in the factory, `on_change`, `on_write` and
  `on_log` rebinding.
- Any annotated class is a schema, no decorator needed. Field types: `rant.u8` to
  `rant.f64`, `rant.string(cap)`, `rant.<t>[N]`, a nested class, plain int, float and
  bool, `str` (VSTR), `list[...]` (VARR), `dict` (MAP), or a bare `IntEnum`.
  `__rant_name__` overrides the type name. `rant.dsl(cls)` computes DSL with no library
  load. The standard types are tagged dataclasses in `bindings/python/rant/types.py`, reached as
  `rant.types.*`.
- `__init__.pyi` and `types.pyi` are the checker's view, hand maintained beside the runtime
  and checked by running pyright over `bindings/python/test.py`. Handles are `Generic[T]` there
  while the runtime subscripts through `__class_getitem__`. The scalars are `int` and
  `float` aliases in the stub, so a capped string or array field is spelled
  `Annotated[T, "<dsl>"]`, which `_resolve` reads at runtime.
- The map body is built and parsed in pure Python.
- Task handlers run on a daemon thread per call. Raising `rant.CancelledError` completes
  CANCELLED.
- `node.reflection` carries the walks as `Peer` and `Entity` dataclasses whose schemas
  are `rant_schema_copy` owned copies freed by the `Schema` finalizer.
