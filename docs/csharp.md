# C#

`bindings/csharp/Rant.cs` is a P/Invoke layer over a prebuilt native library named `rant`, bundled
per platform in the NuGet package and in the Unity package. bindings/csharp/README.md has the
build, the install and the first example, and docs/unity.md the Unity component.
The semantics are the C ones, so docs/node.md, docs/topics.md, docs/patterns.md and
docs/tasks.md apply. This page says what is different in C#.

## A node

`new RantNode(name, options)` opens a node, both arguments optional. `NodeOptions` mirrors
the C node options under C# names, 0, false or null being the C default: `Domain`,
`MaxTopics`, `MulticastInterface`, `MatchWaitMs`, `FetchDetails`, the discovery options of
docs/discovery.md (`SeedPeers` as "ip" or "ip:port" strings, `UnicastOnly`, `SelfIp` with
`AdvertisePort`) and the rest. Every handler in the wrapper is a C# event: `node.OnEvent`
carries peer lifecycle, loss and error events, optional since `LastError` records the last
error either way, and `node.OnLog` the mesh wide log lines.

Every handle comes from a method on the node named after it: `Publisher<T>`,
`Subscriber<T>`, `FunctionDefinition<TReq, TRsp>`, `RemoteFunction<TReq, TRsp>`,
`TaskDefinition<TReq, TPrg, TRsp>`, `RemoteTask<TReq, TPrg, TRsp>`,
`VariableDefinition<T>` and `RemoteVariable<T>`. A topic takes a `Qos`, a pattern handle
one options object, `FunctionOptions`, `TaskOptions` or `VariableOptions`, mirroring the C
options. Every handle is `IDisposable`: `Dispose()` retires it and releases the name, and
is refused from a callback.

Every call is thread safe. The C service thread runs from construction and handlers fire
on it one at a time. `NodeOptions.Threading = Threading.Manual` leaves the loop to your
own thread instead: call `Poll()` from it and handlers fire there. Under the service
thread, `Dispatch()` once per frame runs every queued message handler on the calling
thread. `Dispatch()` covers messages only. `NodeOptions.Dispatcher` covers the rest: set
it to a delegate that posts to your thread and every event, pattern handler, variable
observer, progress report and awaited call result runs there instead of on the service
thread. A function or task handler that lands this way has its reply parked first, so it
still answers from wherever it runs. From inside a handler, `Send` and read only queries
are allowed, and Poll, handle creation, Dispose and Close are refused with
`SendStatus.State` or an exception. `Close()` returns false from a handler.

A construction failure throws with the C reason in the message, since there is no handle
to ask. A build without threads refuses the service thread the same way, and
`Threading.Manual` is the way in. `Stats` reads the node's counters: memory, backpressure
and the evicted unsent sends.

## Schemas

Any struct or class with public fields is a message type, the fields in declaration
order. `[RantArray(n)]` fixes an array's element count and a plain array is a variable
array. `[RantString(cap)]` caps a string and a plain string is unbounded. On a `string[]`,
`[RantString]` alone gives a variable array of capped strings and with `[RantArray]` a
fixed one. `[RantField("name")]` overrides a wire name, needed for lowercase interop
names since C# fields are PascalCase. `[RantSchema("Name")]` overrides the type name.
`[RantTypeName("Timestamp")]` names a field's type with a standard type, and the shape
must be the canonical one or compiling fails. `new Schema(typeof(T)).Dsl` prints the DSL.

A bare type used as a handle's type is the whole schema: `Publisher<bool>`,
`Subscriber<float[]>`, `VariableDefinition<string>`, `Publisher<Dictionary<string, object>>`
or an enum. Such a root is anonymous,
so it is the same bytes and hash in every language. A `Dictionary<string, object>` also
encodes against any compiled schema by field name, nested structs as nested dictionaries.

Every typed handle also takes its schema as an optional `Schema` argument (`schema:` on a
topic or variable, `requestSchema:`, `responseSchema:` and `progressSchema:` on a function
or task), usually compiled from DSL text. The type argument is then only the C# shape the
values pass through and the wire type is exactly the given schema: `Subscriber<string[]>`
with `new Schema("string<128>[]")` reads a bare capped string array, and a struct sent
under a schema with a user named type carries that name. A `byte[]` type is the exception:
it carries the encoded message bytes as is, so `Subscriber<byte[]>` with no schema is a raw
topic and `RemoteFunction<byte[], byte[]>` with explicit schemas is the untyped form.

The standard types of docs/stdtypes.md ship as mirror structs with lowercase wire names,
`Timestamp.Now()` is the Timestamp clock, and the video enums carry the wire values.

## QoS

Topic QoS is one `Qos` object passed to `Publisher<T>` and `Subscriber<T>`, and a null one
means every default. The fields are the C ones of docs/topics.md under C# names, so
`HeartbeatUs` and `BackpressureWaitUs` are microseconds.

```csharp
var sub = node.Subscriber<Pose>("pose", new Qos { Reliability = Reliability.Reliable, KeepLast = 8 });
sub.OnMessage += OnPose;
```

`Qos.ReflectFromMesh` is not a QoS field. It rides beside them in the C topic opts, so a
null schema and a BestEffort reliability then follow the mesh. The pattern options objects
carry the same `ReflectFromMesh`, and every handle that can carry it has `Refresh()`,
which re types it in place when the mesh moved and returns true when it did.
docs/reflection.md explains what gets adopted.

## Messages and queues

`OnMessage` on a subscriber is an event whose handler takes the value and the
`RantMessage` envelope beside it, `(pose, _) =>` when only the value matters.
`RantMessage` copies its payload out, so `Data` outlives the callback. `Value` decodes
on the first read and never at all if it is not read, so a handler that only wants the
bytes pays nothing for the topic's schema. Read one message from one thread, as a handler
does. `RecvUs` is the node's monotonic clock at receipt, `WrittenUs` the sender's wall
clock, 0 when it opted out, and `CaptureUs` when the publisher says the data was true, 0
when it gave none.

`TryTake(out value, timeoutMs)` and `Dispatch(maxMsgs, timeoutMs)` on a subscriber switch
the topic to queued delivery, as in docs/node.md. Dispatch handlers run on the calling
thread without the node lock. `QueueStats` and the traffic counters are always available.
Same name handles on one node share the topic slot: the node advertises the roles the live
handles hold, `Dispose()` on one leaves its siblings receiving, and the last one retires
the slot so the name can carry another schema. `Ready` on a publisher is true when a send
would not wait. `MatchCount` on a publisher counts matched subscribers. A subscriber has
none, since the C exposes no publisher count on the subscribing side.

## Functions, tasks and variables

`FunctionDefinition<TReq, TRsp>`, `RemoteFunction<TReq, TRsp>`, `TaskDefinition<TReq,
TPrg, TRsp>`, `RemoteTask<TReq, TPrg, TRsp>`, `VariableDefinition<T>`, `RemoteVariable<T>`,
`Publisher<T>` and `Subscriber<T>` are the handles, each from the node method of the same
name. `MatchCount` counts the other side: subscribers on a publisher, definitions on a
remote, callers or remotes on a definition.

`OnChange` and `OnWrite` on a variable are events whose handlers take the value and the
`VariableUpdate` envelope. A handler added after the first is replayed the value the
others already saw. Every handle a node
hands out (topics included) is invalidated when the node closes, so a call on one that
outlived its node returns `NoTopic` instead of reading a freed pointer.

A simple function handler returns the reply, and a thrown exception answers AppError with
its message. The full form receives a `RantRequest<TRsp>` valid only inside the callback,
and replies, fails or defers. `Defer()` returns a `Deferred<TRsp>` completed from any
thread. An
async handler answers the call when its Task completes, and runs on the polling thread
until its first await, so CPU bound work belongs in `Task.Run`. A null handler answers
NoHandler.

A task handler is an async delegate. The call is deferred and RUNNING sent before it is
invoked. Its result answers Ok, `OperationCanceledException` answers Cancelled, any other
exception AppError. It works through a context that streams `Progress` and exposes a
real `CancellationToken`. A completion after Dispose or Close is refused by the C and
swallowed.

`Call()` blocks, on the service thread's progress or driving a Manual node's loop, and is
refused from a callback. `CallAsync()` returns a Task that never faults:
inspect `Status` and `SendStatus`,
and reading `Value` when the call did not complete Ok throws `CallException`. On a task,
`CallAsync(req, progress, cancellationToken)` fires progress per update, skipping the
valueless RUNNING ack, and cancelling the token requests cooperative cancellation.

Variables: `TryGet` reads the copied out value, `Set` returns the status, `Force` needs
`AllowForce` in the definition's options, and `Wait` blocks until a value exists. The typed `Value` getter throws while
no value exists and the setter throws `RantException` on a non Ok status. `OnChange`
replays the current value at registration and fires on every state change, `OnWrite` on
every applied write, both inline on the applying thread.

## Reflection

The walks of docs/reflection.md live on `node.Reflection` and return copied snapshots, so
they outlive the poll and need no lock. `Peers()` lists every discovered peer as a
`RantPeer`, dropped ones included, so gate on `Active`. `Entities(peer)` lists what one
node offers, peer 0 for this one, and `Mesh()` folds the whole mesh into one `RantEntity`
per kind and name. `Find(kind, name)` returns one or null, and `Epoch` bumps whenever the
folded view changed. `MetaAsync(peer, sections)` decodes a peer's snapshot into a
`RantMetaSnapshot` and never faults. The schemas on a `RantEntity` are owned copies, null
when untyped or not fetched, and `FetchDetails` on the node is what makes them arrive.

`Schema.Fields` is the flat depth first field table as `SchemaField` rows, and
`Schema.EnumVariants(field)` the options of an enum field, for a tool that renders a schema
it has never seen.

## Logs

`Log(level, text)` publishes a line at a `LogLevel`, and the `OnLog` event delivers every
other node's lines at every level as a `RantLogLine` carrying its `Level`.
