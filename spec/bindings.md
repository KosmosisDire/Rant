# Bindings

Six API surfaces share one C core: C, C++ (`bindings/cpp/rant.hpp`), C# (`bindings/csharp/Rant.cs` plus
the Unity copy), Python (`bindings/python/rant/__init__.py`), Rust (`bindings/rust/`) and
TypeScript (`bindings/web/src/`, over the C built to wasm). The cross language rules below
were argued once and apply everywhere.
Where anything else disagrees about a wrapper API, this file wins.

## Cross language rules

- DEFINITION and REMOTE vocabulary everywhere: `create_function_definition`,
  `create_remote_function`, `create_variable_definition`, `create_remote_variable`.
- `call` is the completed transaction in every language. `_async` marks the callback or
  Task form. TypeScript has no callAsync, the Promise is it.
- Match queries are `match_count` and `MatchCount` on every handle, the count of the
  other side. A C# or Python subscriber has none yet since the C exposes no publisher
  count on the subscribing side. C keeps its generic primitives.
- A call outcome is a value, never a fault. Its value reads whatever payload came back,
  whatever the status, so a cancelled task's partial result and an AppError's failure data
  are readable: C++ `value()` and Rust `value()` are optional, and C# `Value` and Python
  `.value` throw only when the call did not end OK and nothing came back. Rust's
  `into_result()` is the `?` form. A TypeScript `call()` never rejects. C# is the
  exception: `Call` and `CallAsync` return the response itself and throw `CallException`
  off OK, and `TryCall` and `TryCallAsync` are the value forms.
- A payload that does not decode into a handle's type raises an Error event of kind
  SchemaMismatch naming the topic in every binding, and the handler does not run. A pull
  or a variable read of one throws.
- Durations follow the language: `std::chrono` in C++ (a timeout in milliseconds,
  `rant::forever` for no end, option fields without a unit suffix), int milliseconds in
  C#, float seconds in Python, `std::time::Duration` in Rust (`rant::FOREVER` for no end).
  Timestamps stay integer microseconds everywhere.
- Every handle reports its name and the schema it uses now, the adopted one on a
  reflect_from_mesh handle, through `rant_topic_schema`, `rant_function_request_schema`,
  `rant_function_response_schema`, `rant_function_progress_schema` and
  `rant_variable_schema`.
- Variable reads are local and status free. Writes return a status. C# `Value` has a
  setter that throws on a non OK status. A remote set round trips through the definition.
  The cache is authoritative only, never an optimistic echo.
- Every handle comes from a factory on the node named after it: `publisher`,
  `subscriber`, `function_definition`, `remote_function`, `task_definition`, `remote_task`,
  `variable_definition`, `remote_variable`, PascalCase in C#. Only a raw handle takes a
  schema, after its options, and `reflect_from_mesh` is an option field.
- Construction failures throw or reject. Data path outcomes are statuses and events. A C++
  node has a constructor, not `open`, and it and every factory throw `rant::Error`.
  `-fno-exceptions` degrades to an empty handle, `valid()` and `node.last_error()`. Rust
  returns `Result` with `rant::Error` from the node and every factory, and the data path
  returns `Result` with `SendError`.
- Node open takes a name and options only: `rant::Node(name, NodeOptions)`, `new
  RantNode(name, NodeOptions)`, `Node(name, **options)`, `Node::new(name,
  NodeOptions::new())`. There is no node level message
  handler, subscribers carry every one. The service thread runs from open unless the
  `threading` option is manual, and dispatch parks every callback on the node's queue
  for `dispatch()`. The event handler is optional and attaches after open (`on_event(h)`,
  the C# `OnEvent` event, Python's `on_event` keyword or `node.on_event(h)`). With none
  set, errors print to stderr, and the last error is recorded either way.
- Handlers come in two forms: payload only, or payload plus message envelope. A
  subscriber takes its handler at creation in every binding, and a subscriber created
  without one is pulled: `take` and `latest` in Python, `TryTake` and `TryTakeLatest` in
  C#, `take` and `take_latest` in C++. C# node and variable handlers are events
  (`OnChange`, `OnWrite`, `OnEvent`, `OnLog`, `OnStatus`) whose delegate takes the value and the
  envelope, discarded with `_` when unwanted. Rust has one form that is both, a value
  that reads as the payload through `Deref` and carries the envelope, since a closure's
  type cannot be inferred across two, and its pulled subscriber has a factory of its own,
  `pulled_subscriber`, with `take` and `take_latest`.
- An owning message copies its payload, since the transport buffer is reused after the
  callback, but decodes only when the fields or the typed value are read. A handler that
  wants the bytes must not pay for the topic's schema. The decode outlives the callback,
  and the schema a message carries lives until the node closes (spec/schema.md), so the
  wrapper keeps that schema itself, keyed by pointer and never by hash or re parsed from
  its wire: a rebased schema keeps the reader's hash and wire with the writer's offsets.
  It invalidates its wrappers at close since the node frees them.
- An owning type is a plain noun, a view type has a `View` suffix, "Taken" is banned:
  `Message`, `MessageView`, `MessageBuilder`, `Response`, `ResponseView`.
- `Peer::entities` replaces any `Peer::topics`. No dual surfaces.
- A pending call at close gets exactly one synthesized outcome, `RANT_CALL_CANCELLED`,
  fixed in C so every binding inherits it.
- `rant_node_close` frees everything and assumes no other thread is inside a call, so
  every binding holds a pass on a per node gate across each call into the C, and close
  refuses new passes, waits until none is out, then closes. A handle that outlives its
  node or races its close answers `NoTopic`. A wait that can be long (dispatch, take,
  drain, a variable wait) takes one pass per 50 ms slice, so close waits at most a slice
  behind it. A poll, a settle and a blocking call hold theirs to the end, so close waits
  for a function call up to its timeout and for a running task call until it ends, the
  user's choice over a C step that cancels them first. Every callback entry marks its
  thread, and a close inside one of the node's own callbacks runs on a new thread once
  that callback returns. A later close waits until the first is done. Anything that
  reads a node owned schema later (an encode, a lazy decode, a pulled message) holds a
  pass for it, and a schema read after close refuses loudly.
- `rant_function_retire` and `rant_variable_retire` free the handle and assume the same,
  so every function, task and variable handle has a gate of its own too, shared with the
  deferred answers and task contexts that outlive their callback. A call holds a pass on
  the node, then on the handle. Close refuses new handle passes, waits until none is out,
  takes the pointer once so two closes retire once, then retires, and a refused retire
  reopens the gate. Inside one of the node's callbacks close never waits, since that
  callback's own caller may hold a pass: it answers `State` while one is out. Rust needs
  no wait, since a call borrows the handle that `close` consumes.
- TypeScript: `await openNode(name, options)` is the one async step. Handles come from
  synchronous factory methods. The schema is a positional argument, never inside an opts
  object. A null schema means raw or payload less.
- A reflected field table mirrors the C `RantSchemaFieldInfo` exactly, everywhere,
  including `type_name`, `backing`, `arr_parent` and `arr_depth`, and every array's element
  row. A dropped `type_name`
  turns `color: Color` into an unnamed struct, which the wire refuses. Only `rant_schema_print` spells DSL text.
- A public feature lands in every binding. Nothing lists the exports: a declaration
  carrying `RANT_API` in a public header is exported and nothing else is, so a new entry
  point is reachable from every binding the moment it is declared. Bindings format log
  text in their own runtime and call `rant_node_log_text`, never the variadic
  `rant_node_log` over FFI.
- A binding reads the log and status builtins by registering its handler for the topic's
  index first, then widening the role. The other order lets the catch up replay land
  before the handler exists, which first call compilation in C# made happen most runs. A
  status handler also sees an empty set for a node that went away: the binding remembers
  which peers last sent a set that was not empty and delivers one on their
  `RANT_PEER_DOWN`.
- A thrown handler in C++, C#, Python or TypeScript, or a panicking one in Rust, answers APP_ERROR
  with its text.
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
run `ctest -L bindings`. Its layout legs compare every mirror's size and field offsets with
`tests/support/layout.c`, which the build runs into `build/tests/layout.txt`, so a mirror
that drifted fails there instead of crashing. A newly mirrored struct gets its lines in
`layout.c` too. Import alone succeeds silently with a wrong struct. C++ is immune because
it embeds the real header, and Rust because bindgen generates its declarations from it.

## C++

`bindings/cpp/rant.hpp` is a header only C++17 wrapper. The packer splices `rant.h` in, so
`dist/rant.hpp` is the one file a consumer needs. Regenerate after any edit. Verified on
MinGW g++, clang++, clang-cl and MSVC, and clean under `-fno-exceptions -fno-rtti`.

- Surface: `Node`, `Reflection`, `Publisher<T>`, `Subscriber<T>`,
  `FunctionDefinition<Req, Rsp>`, `RemoteFunction<Req, Rsp>`,
  `TaskDefinition<Req, Prg, Rsp>`, `RemoteTask<Req, Prg, Rsp>`, `VariableDefinition<T>`,
  `RemoteVariable<T>`, `Request`, `Deferred`, `TaskRequest`, `PendingTask`, `Response`,
  `ResponseView`, `ProgressView`, `Schema`, `MessageBuilder`, `MessageView`, `FieldView`,
  `MapWriter`, `MapReader`, `Event`, `Peer`, `Entity`, `LogLine`, `Condition`, `StatusUpdate`,
  `MetaSnapshot`, `Bytes`,
  `Qos`, `NodeOptions`, `Queue`, `Error`. `rant::Bytes` as every type argument is the raw
  form, an explicit specialization the typed templates build on. There is no separate
  untyped tier.
- Every handle comes from a node factory named after it, as in C# and Python:
  `node.publisher<T>(name, qos, schema)`, `subscriber<T>(name, handler, qos, schema)` or
  without a handler for the pulled form, `function_definition`, `remote_function`,
  `task_definition`, `remote_task`, `variable_definition` and `remote_variable`. Only the
  `rant::Bytes` form takes schemas or `reflect_from_mesh`. A failed factory throws
  `rant::Error`, or without exceptions returns an invalid handle with `last_error()` set.
- `NodeOptions::threading` is `ServiceThread`, `Manual` or `Dispatch`. Dispatch creates
  one queue at open, hands it to every handle made without one and to the events, log and
  status, and `node.dispatch()` drains it. `create_queue()` gives a handle a thread of its
  own through its options. A blocking call from an inline callback throws, since it
  would stall the loop it waits on.
- Durations are `std::chrono`: timeouts are `milliseconds` with `rant::forever` as the
  wait with no end, and `std::optional` where leaving one out takes the default. Option
  fields are `microseconds` without a unit in their names. Timestamps stay integer
  microseconds.
- `RANT_SCHEMA(T, fields...)` synthesizes DSL text and compiles it through
  `rant_node_schema`, once per node and type, so the C compiler stays the single source of
  wire truth. It builds a plan (below) and copies the whole value with memcpy only when
  the type is trivially copyable, little endian and padding free. The decode cache is keyed per
  incoming schema pointer, not per hash, because a rebased schema keeps our hash with the
  publisher's offsets. The node keeps every delivered schema until close
  (spec/schema.md), so a pointer never names two schemas.
- Encoding is strict. `priv::encode` returns `std::optional<Bytes>` into a thread local
  scratch buffer, empty when the value does not fit, such as an over long `String<N>`.
  A send or set then answers `SendStatus::Schema` and a reply answers AppError, never a
  cut or empty message.
- A type's plan is built once from its members: fixed leaves copied at their wire offsets,
  and each variable value (`std::string`, `std::vector<E>`, an array of variable
  elements) written and read through `rant_set_value_at` and `rant_get_value_at` with one
  index per enclosing array, in record order so each resize moves only defaults. A variable
  array is sized first by `rant_set_array_count_at`. Fixed
  elements go as one packed block, a single memcpy when host and wire agree. A struct
  element's fixed part is written in place through its element row. The buffer is sized
  once from `msg_min` plus what each value adds, an element costing its `min_len` and an
  end. Any element nests, roots included. Still refused at compile time: `vector<bool>`,
  maps, pointers.
- `rant::std_type<T>` names a user type the way the standard roster is named. `RANT_ENUM`
  registers an `enum class` under its name. Unregistered enums ship as their backing integer.
- Callbacks never unwind into C. `priv::guarded` wraps every app callback run from a C
  trampoline and turns a throw into stderr text plus an Error event of kind `None`.
  `priv::answered` wraps a function or task handler and turns a throw into an AppError
  reply with `e.what()`. A payload that does not decode raises a `SchemaMismatch` event.
  `Response<T>::value()` is `std::optional`, present whenever a payload came back.
- Every handle reports `name()` and its current schemas: `schema()` on topics and
  variables, `request_schema()`, `response_schema()` and `progress_schema()` on functions
  and tasks, read from the C getters, so a `reflect_from_mesh` handle reports what it
  adopted. Raw handles have `refresh()` to retype in place.
- With `RANT_IMPLEMENTATION` the C header embeds at global scope. Otherwise declarations
  embed inside `namespace rant::detail`. A `.cpp` anchor must spell
  `#define RANT_IMPLEMENTATION` itself, since the auto define is guarded by
  `!defined(__cplusplus)`.
- Memory is configured on `NodeOptions`: a buffer plus size means static mode, otherwise
  dynamic. One allocator per node, never shared, because open copies it and close resets
  it. The node's schemas and typed codecs live in that memory too.
- Strings are `std::string_view`. `Bytes` is a contiguous range convertible to
  string_view, string and vector, with `std::span` where available. Field name parameters
  stay `const char*` on purpose.
- The node's state is `priv::NodeImpl`, shared by the node and every handle through a
  `shared_ptr`. The node closes the C node at `close()` or destruction whatever handles
  remain, through the gate above (`priv::Gate`, `priv::Pass`, `priv::InCallback`). A
  typed handle holds one pass across encode, call and decode, since its codec's schema
  lives in the node. Handles are move only and `close()` or scope end
  retires them. A topic name keeps a hold count per side, the role follows the live
  holds and the last close retires the topic. A close refused inside the handle's own
  callback raises a `State` event. Pattern handler boxes hold the state weakly, since the
  node owns them.
- A function, task or variable handle keeps its C pointer in a `priv::HandleState` with a
  gate of its own, shared with its `Deferred` and `PendingTask` answers, since a retire
  frees it. Every call holds a `priv::HandlePass` on the node and the handle. `close()`
  waits for them, empties the handle in place with one atomic exchange, so two closes
  retire once and another thread may still read it, and inside one of the node's
  callbacks answers `State` while a pass is out instead of waiting on its own caller.
- `defer()` on a task handler returns a movable thread safe `PendingTask`.
- Every public declaration carries a comment that stands alone as clangd hover text, with
  no pointers into docs.

## C#

`bindings/csharp/Rant.cs` is a hand written P/Invoke layer over a prebuilt native library named
`rant`, resolved from a NuGet package's `runtimes/<rid>/native/` or Unity's
`Assets/Plugins/`. The standard types beside it, `bindings/csharp/types/Types.cs`, are generated and are their own assembly, `Rant.Types`, with no native code.

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
  its own, and same name handles must agree on it. Continuations of an awaited call are
  kept off the loop thread unless the handle is queued. Every throw is a `RantException`:
  a failed create carries the `rant_last_error` event, `CallException` and
  `SchemaException` derive from it, and `LastError` is null while the C reads no error. A
  task `CallAsync` whose own token was cancelled ends in `OperationCanceledException`.
- Typed messages by reflection: any struct, class or record with public fields or auto
  properties, ordered by MetadataToken. Each member's C# type becomes a `Shape`, one per
  array level down to the element, which spells the DSL. Encode and decode walk the
  compiled flat table instead, so they serve a peer's schema too. Encode takes each row's
  value by wire name from a typed object or a Dictionary, sets an array of fixed scalars,
  enums or capped strings as one packed value, and sets any other element by its indexed
  path once a variable array is sized. The buffer is sized once: `msg_min` plus every
  payload, and an array sized to n adds n times the element's `min_len` plus 4. An array
  whose element type is the wire kind's is pinned where it lies, and a `List` of it is
  copied in one block. Decode reads each row through `rant_get_value_at`: a typed decode
  builds the type directly, with numbers copied once, and an untyped one builds
  dictionaries. The copy is `Buffer.MemoryCopy` under `RANT_UNSAFE`, which the NuGet project
  and the Unity asmdef define with unsafe code allowed, else `Marshal.Copy`, whose signed
  overloads .NET also lets an unsigned array through. A file compiled without it still works. An over cap string throws.
- Callbacks are static trampolines dispatched by an int id in `user_data`, marked
  `[MonoPInvokeCallback]` for IL2CPP. Allocators are managed callbacks over
  `Marshal.ReAllocHGlobal`, which pays a native to managed transition per page.
  `rant_allocator_heap` is the library's own heap hook and costs no transition.
- Stale DLL trap: a stale `rant.dll` presents as memory corruption or peers that never
  match. Check the timestamp of the copy next to the executable against
  `dist/native/<rid>/`, which the `rant_shared` target overwrites on every build.
- Task handlers are async delegates with a real `CancellationToken`. They run on the poll
  thread until their first await, so CPU work belongs in `Task.Run`.
- A function, task or variable core keeps its C pointer in a `HandleState` with a gate of
  its own, shared with its request box, `DeferredCore` and `TaskContextCore`. Every call
  holds a `HandlePass` on the node and the handle, and `Dispose` waits for them, zeroes the
  pointer in place with one exchange, and inside one of the node's callbacks throws `State`
  while a pass is out instead of waiting on its own caller.
- Unity (`bindings/csharp/unity/`) is a subset. `tools/unity.cmake` assembles the package into
  `dist/com.rant.rant/`: `bindings/csharp/Rant.cs`, `bindings/csharp/unity/Runtime/`, the `dist/native/`
  libraries for win-x64, linux-x64 and osx, a `package.json` stamped from `VERSION`, and
  a `.meta` per entry whose GUID is the md5 of its path so an upgrade keeps references.
  Each plugin `.meta` enables exactly its own platform, since the three libraries share
  the name `rant`. The assembly is `Rant.Runtime`, since one named `Rant` shares the native
  library's file name, which Unity warns of. The scene MonoBehaviour is `RantNodeUnity`. Unity only
  registers a MonoBehaviour whose class name matches its file name, and fails silently
  otherwise. Pattern: open in `Threading.Dispatch` and call `Dispatch()` per frame, which
  runs every callback on the frame. OnDisable closes and a beforeAssemblyReload backstop
  stops the service thread. A WebGL player has no native library: the package's
  `Runtime/Plugins/WebGL/` holds `rant_webgl.c` (the library and `bindings/web/platform.c` as
  one translation unit with the web defines), `rant_web.jslib`, `rant_unity.jslib` (the
  connect behind `RantWebGL`) and `rant_tunnel.jspre` (the binding's `TunnelNet` with its module
  syntax taken out), so the web package builds first. `Rant.cs` binds `__Internal` there,
  and a close inside a callback waits for the end of `Dispatch` since no thread can. With a
  bridge set the component opens the tunnel before the node in WebGL, and elsewhere opens
  the node on a worker, since an open through a bridge waits for it, then adopts it in
  `Pump`. The editor path was run in Unity 6 in batch mode: the package compiles, and a
  node and the component reach a local node through a bridge. The WebGL files are checked
  by compiling them with a stand in player under Node.js, never in a Unity WebGL build.

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
  `on_log` rebinding. `Threading.DISPATCH` and `queue=` on every handle mirror C#'s
  Dispatch and `Qos.Queue`, with `rant.Queue` from `node.create_queue()`.
- Any annotated class is a schema, no decorator needed. Field types: `rant.u8` to
  `rant.f64`, `rant.string(cap)`, a nested class, plain int, float and bool, `str`
  (VSTR), `dict` (MAP), a bare `IntEnum`, and `rant.<t>[N]` (ARR) or `list[...]` (VARR) of
  any of them, nested. `__rant_name__` overrides the type name. `rant.dsl(cls)` computes
  DSL with no library load. The standard types are tagged dataclasses in the package
  `bindings/python/rant_types`, generated but for `_vocab.py`, reached as `rant.types.*`. Encode and decode walk the
  compiled flat table as C# does, so a peer's schema works the same. A numeric buffer of
  the field's own type is read in place, a send views the encoded buffer where it was
  built, and a numeric array decodes into an `array.array` by one copy from the message.
- `__init__.pyi`, hand maintained beside the runtime, and the generated `rant_types/__init__.pyi` are the checker's view,
  and checked by running pyright over `bindings/python/test.py`. Handles are `Generic[T]` there
  while the runtime subscripts through `__class_getitem__`. The scalars are `int` and
  `float` aliases in the stub, so a capped string or array field is spelled
  `Annotated[T, "<dsl>"]`, which `_resolve` reads at runtime.
- The map body is built and parsed in pure Python.
- Task handlers run on a daemon thread per call. Raising `rant.CancelledError` completes
  CANCELLED.
- A function, task or variable handle keeps its C pointer in a `_HandleState` with a gate
  of its own, shared with its definition box, `Request`, `Deferred` and `TaskContext`.
  Every call holds a `_HandlePass` on the node and the handle, and `close()` waits for
  them, takes the pointer under a lock so two closes retire once, and inside one of the
  node's callbacks returns False while a pass is out instead of waiting on its own caller.
- `node.reflection` carries the walks as `Peer` and `Entity` dataclasses whose schemas
  are registered in the node through `rant_node_schema_parse`.

## Rust

`bindings/rust/` is a Cargo workspace: `rant-middleware` (imported as `rant`, since `rant` on
crates.io is taken), `rant-macros` (the derives), `ffigen` (the FFI generator) and the
interop tick. `build.rs` compiles the amalgamated C into the crate with `cc`, so a program
needs no DLL and no cmake.

- `src/ffi.rs` is generated by bindgen from `dist/rant.h` for one canonical target and
  checked in. `cargo run -p rant-ffigen` regenerates it, and ctest `bindings.rust.ffi`
  runs it with `--check`. `RANT_VERSION` is left out, so a version bump leaves it alone.
  `tests/coverage.rs` fails while a declared `rant_*` function is neither used nor
  skipped with a reason.
- Options are setters on the options value, `Qos::new().reliable(true)`, never a builder.
- `#[derive(rant::Message)]` and `#[derive(rant::Enum)]` spell DSL text and compile it
  through `rant_node_schema`, once per node and type, as `RANT_SCHEMA` does. The codec
  resolves the type's flat ops against a schema into a plan, one per delivered schema
  pointer. An op is a fixed leaf at a static offset, a String, a `Vec` of scalars, enums
  or capped strings packed as one value, or an array whose elements each take the
  element's own ops. A fixed array of fixed elements flattens into leaves. Variable values
  go through `rant_set_value_at` and `rant_get_value_at` with one index per enclosing
  array, a `Vec` is sized by `rant_set_array_count_at`, and an element's fixed part is a
  view the leaves write in place.
- `Payload` is sealed: a `Message` type, or `Bytes` for raw bytes, which alone takes a
  schema or `reflect_from_mesh`. A typed and a raw side may mix on one pattern handle.
- The node gate is the one above. Every function, task and variable handle has a gate of
  its own as well, since a retire frees the C handle while a `Deferred` or `PendingTask` on
  another thread may still use it. Every encode and decode holds a pass, and a value read
  outside a callback holds one in its `FieldView`.
- Every app callback runs through one guard: it marks the thread, catches a panic, and
  refuses a re entry, which a `FnMut` cannot allow. A panic becomes stderr text and an Error
  event, and in a function or task handler an AppError answer with the panic text.
- The C points at handler boxes the node keeps until the handle retires or the node
  closes, replaced ones included, since the C may still run a replaced handler on another
  thread. A closure that holds a handle of its node is a cycle, which close breaks.
- An in flight call's callbacks are found through a process wide registry by an id the C
  carries as its user pointer, so a progress callback racing the call's end finds nothing
  instead of freed memory. The C returns a blocking call only after its last progress
  callback, which lets that handler borrow the caller's stack.
- `call` blocks through `rant_function_call`, and `call_async` returns a Future woken from
  the response callback, which needs no runtime. Dropping an unfinished `TaskCall` cancels
  the task, as dropping a future stops its work, and `detach()` keeps it running. Dropping a
  `Deferred` or `PendingTask` unanswered answers AppError.
- A handler's return value is its answer through `IntoReply`: a value answers Ok, an `Err`
  AppError with its text, and `()` leaves the answer to its defer.
- Static mode runs in one block the node allocates at open, through a copy of
  `rant_allocator_static`, which the C keeps inline.
- The crate ships as a git dependency on the tag `crate-v<VERSION>` (spec/build.md).

## TypeScript

`bindings/web/src/` binds the C library built to wasm as the module `rant_wasm`, whose
exports are the `RANT_API` functions (spec/web.md). The npm package `rant-middleware` ships
the module prebuilt.

- `src/layout.ts` holds the C struct offsets for wasm32, generated from `tests/support/layout.c`
  built to wasm, so the mirrors cannot drift: `web.layout` compares it with a fresh build. A
  struct passed by value goes as a pointer to a copy, and one returned by value through a
  pointer first argument, as the wasm C ABI has it. 64 bit integers cross as bigints.
- One module instance per net object, shared by the nodes opened on it. Callbacks are one
  function pointer per kind made with `addFunction`, routed by the user value the C passes
  back, and a thrown JavaScript exception never unwinds through C.
- The binding drives its nodes through `Module.rantWeb`: each drive polls the node, then
  dispatches its one queue, which every handle, the events and the logs are created with. So
  a handler runs outside the poll and may use the whole API, and a close asked for while
  handlers run waits until they end. A pulled subscriber has no queue, as the C requires.
- Values are encoded and decoded by walking the flat field table, an array's element row
  once per element, and calling `rant_set_value_at` and `rant_get_value_at` with one index
  per enclosing array, so the wire layout stays the C's alone. A variable array of
  variable elements is sized first by `rant_set_array_count_at`, and the buffer once from
  `msg_min` and each element's `min_len`. Only the map body is built and parsed in TypeScript. Member names map
  to wire names through `rant_field_name`.
- A message decodes when it is delivered, since its view lives only for the callback.
- Nothing blocks: `settle`, `drain` and a variable `wait` are promises that check after
  each drive, and `match_wait` is always negative.
- In Node.js `openNode` without a net shares one `UdpNet` per bind address, shut once its last
  node closes, since its sockets keep the process alive.
