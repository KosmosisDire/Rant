# Bindings

Five API surfaces share one C core: C, C++ (`cpp/dart.hpp`), C# (`csharp/Dart.cs` plus
the Unity copy), Python (`python/dart.py.in`) and JS through the bridge. The cross
language rules below were argued once and apply everywhere. Where anything else disagrees
about a wrapper API, this file wins.

## Cross language rules

- DEFINITION and REMOTE vocabulary everywhere: `create_function_definition`,
  `create_remote_function`, `create_variable_definition`, `create_remote_variable`.
- `call` is the completed transaction in every language. `_async` marks the callback or
  Task form. JS has no callAsync, the Promise is it.
- Match queries are side named. The remote side exposes a bool `has_definition`. The
  definition side uses `caller_count` or `remote_count`. C keeps its generic primitives.
- A call outcome is a value, never a fault. C# `CallAsync` always completes with a
  Response, and reading `.Value` off OK throws. JS `call()` rejects only on connection loss.
- Variable reads are local and status free. Writes return a status. C# `Value` has a
  setter that throws on a non OK status. A remote set round trips through the definition.
  The cache is authoritative only, never an optimistic echo.
- Construction failures throw or reject. Data path outcomes are statuses and events. C++
  has a constructor, not `open`, and throws `dart::Error`. `-fno-exceptions` degrades to
  `valid()` and `last_open_error()` at compile time.
- Node open takes `(name, on_message, on_event, opts)`. `on_message` is nullable, since
  typed subscribers and pattern handles carry their own handlers. `on_event` is validated
  non null by every wrapper so the first diagnostics are never missed. Setter methods
  exist only for later rebinding.
- Handlers come in two forms: payload only, or payload plus message envelope.
- An owning message copies its payload, since the transport buffer is reused after the
  callback, but decodes only when the fields or the typed value are read. A handler that
  wants the bytes must not pay for the topic's schema. The decode outlives the callback,
  so the wrapper holds its own `dart_schema_copy` of the publisher's schema, made once
  per distinct hash and released with the last message that names it, never at close.
- An owning type is a plain noun, a view type has a `View` suffix, "Taken" is banned:
  `Message`, `MessageView`, `MessageBuilder`, `Response`, `ResponseView`.
- `Peer::entities` replaces any `Peer::topics`. No dual surfaces.
- A pending call at close gets exactly one synthesized outcome, `DART_CALL_CANCELLED`,
  fixed in C so every binding inherits it.
- JS: the client class is `DartNode` (`await DartNode.connect(url)`). Handles come from
  async factory methods. The schema is a positional argument, never inside an opts object.
  A null schema means raw or payload less.
- A reflected field table mirrors the C `DartSchemaFieldInfo` exactly, everywhere,
  including `type_name`, `elem_name`, `elem_size` and `arr_parent`. A dropped `type_name`
  turns `color: Color` into an anonymous struct and every named reader then refuses the
  sender. Only `dart_schema_print` spells DSL text.
- A public feature lands in every binding, and `csharp/native/dart.def` plus the Python
  `_EXPORTS` list gain every new C export. Bindings format log text in their own runtime
  and call `dart_node_log_text`, never the variadic `dart_node_log` over FFI.
- A thrown handler in C#, Python or JS answers APP_ERROR with the exception text.
- Every wait is a probe plus an event plus a bounded block. No blocking only APIs.

Deferred, confirmed: a C reflection macro (`serialize/reflect.h` over C11 `_Generic`),
variable external storage, name directed calls, a `call_all` keyed response list.

## The hand mirrored struct footgun

C# and Python hand mirror the C structs (`DartAllocator`, `DartNodeOpts`, `DartQos`,
`DartMsg`, `DartEvent`, `DartCallOpts` and friends) with no compile time check. When a C
struct gains a field and the mirror does not, every node open crashes with an access
violation. `DartEvent` is returned by value by `dart_last_error`, so a missing trailing
field is an out of bounds read by the C formatter. After any layout change to a public
struct: update the C#, Unity and Python mirrors, rebuild the prebuilt native library, and
run `csharp/test` and `python/test.py`. Import alone succeeds silently with a wrong
struct. C++ is immune because it embeds the real header.

## C++

`cpp/dart.hpp` is a header only C++17 wrapper. The packer splices `dart.h` in, so
`dist/dart.hpp` is the one file a consumer needs. Regenerate after any edit. Verified on
MinGW g++, clang++, clang-cl and MSVC, and clean under `-fno-exceptions -fno-rtti`.

- Surface: `Node`, `Topic`, `Publisher<T>`, `Subscriber<T>`, `FunctionDefinition<Req,Rsp>`,
  `RemoteFunction<Req,Rsp>`, `TaskDefinition`, `RemoteTask`, `VariableDefinition<T>`,
  `RemoteVariable<T>`, `Schema`, `MessageBuilder`, `MessageView`, `Message`, `Response`,
  `ResponseView`, `Event`, `Peer`, `Entity`, `Bytes`, `Qos`, `NodeOptions`, `MapWriter`,
  `MapReader`. Every typed handle has an untyped `<void>` twin for the bridge and explorer.
- `DART_SCHEMA(T, fields...)` synthesizes DSL text and compiles it through
  `dart_schema_compile`, so the C compiler stays the single source of wire truth. It builds
  a flat codec table and uses memcpy only when the type is trivially copyable, little
  endian and padding free. The decode cache is keyed per incoming schema pointer, not per
  hash, because a rebased schema keeps our hash with the publisher's offsets.
- Tail members: `std::vector<scalar>` and `std::string` struct members ride their tail
  frame through the C accessors by dotted path. Bare `std::vector<E>` roots work. Still
  refused at compile time: `vector<bool>`, vectors of structs or strings, struct array
  members, maps, pointers.
- `dart::std_type<T>` names a user type the way the standard roster is named. `DART_ENUM`
  registers an `enum class`. Unregistered enums ship as their backing integer.
- With `DART_IMPLEMENTATION` the C header embeds at global scope. Otherwise declarations
  embed inside `namespace dart::detail`. A `.cpp` anchor must spell
  `#define DART_IMPLEMENTATION` itself, since the auto define is guarded by
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

`csharp/Dart.cs` is a hand written P/Invoke layer over a prebuilt native library named
`dart`, resolved from a NuGet package's `runtimes/<rid>/native/` or Unity's
`Assets/Plugins/`. Nothing is generated.

- Collision prone nouns are Dart prefixed: `DartNode`, `DartEvent`, `DartMessage`,
  `DartRequest`, `DartResponse`. Domain names (`Publisher`, `Subscriber`, `Topic`) stay
  bare. The C mirror structs carry a `Native` suffix.
- Everything user creatable uses `new` with optional named parameters. Constructors throw
  on failure. `CallAsync` returns a Task that never faults.
- Typed messages by reflection: any struct or class with public fields. `[DartSchema("Name")]`
  overrides the type name, `[DartArray(N)]` marks fixed arrays, `[DartString(cap)]` is
  required on every string field, `[DartField("name")]` overrides a wire name. Fields
  order by MetadataToken. Wire names default to the C# names, so cross language interop
  needs lowercase `[DartField]` overrides (the std mirrors silently decoded to zeros
  without them). An over cap string throws.
- Callbacks are static trampolines dispatched by an int id in `user_data`, marked
  `[MonoPInvokeCallback]` for IL2CPP. Allocators are managed callbacks over
  `Marshal.ReAllocHGlobal`.
- Stale DLL trap: a stale `dart.dll` presents as memory corruption or peers that never
  match. Check the DLL timestamp first. `csharp/native/build.ps1` deletes the old library
  before building. `csharp/native/dart.def` is the hand maintained MSVC export list.
- Task handlers are async delegates with a real `CancellationToken`. They run on the poll
  thread until their first await, so CPU work belongs in `Task.Run`.
- Unity (`csharp/unity/`) is a subset. `Runtime/Dart.cs` and `Runtime/Plugins/` are
  assembled by `pack.ps1`. The scene MonoBehaviour is `UnityDartNode`. Unity only
  registers a MonoBehaviour whose class name matches its file name, and fails silently
  otherwise. Pattern: `Start()` the service thread, force `QueueBytes` nonzero on every
  topic so it is queued from creation, then `Dispatch()` per frame. OnDisable closes and a
  beforeAssemblyReload backstop stops the service thread. The Unity path has never run in
  a real editor here. `package.json` keeps a `-preview` suffix, `-beta` belongs only to
  the git tag.

## Python

`python/dart.py.in` becomes `dist/dart.py` with `dart.h` embedded. On the first `Node(...)`
it hashes the embedded C, compiler id, platform and wrapper version, compiles a shared
library into `%LOCALAPPDATA%/dart-python` (or `~/.cache/dart-python`) and loads it with
ctypes. A C compiler is needed once (`DART_CC` overrides). Verified with clang-cl and
MinGW.

- The API mirrors C#: constructors not factories, `Node(name, on_message, on_event,
  options=None, **opts)`, `Topic(node, name, schema=None, role=PUBSUB, **qos)`,
  `Topic[T](...)`, `Schema(source)` from DSL text or a class. Variables are methods only.
  Handlers are arity dispatched.
- Any annotated class is a schema, no decorator needed. Field types: `dart.u8` to
  `dart.f64`, `dart.bool_`, `dart.string(cap)`, `dart.<t>[N]`, a nested class, plain int,
  float and bool, `str` (VSTR), `list[...]` (VARR), `dict` (MAP), or a bare `IntEnum`.
  `__dart_name__` overrides the type name. `dart.dsl(cls)` computes DSL with no library
  load. Std types are tagged dataclasses.
- The map body is built and parsed in pure Python.
- The compiled library is cached by content hash. A new C export needs the `_EXPORTS`
  list updated and the cache cleared.
- Task handlers run on a daemon thread per call. Raising `dart.CancelledError` completes
  CANCELLED.
- Gaps: no `seed_peers`, no peer reflection.
