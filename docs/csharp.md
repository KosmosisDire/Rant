# C#

`csharp/Dart.cs` is a P/Invoke layer over a prebuilt native library named `dart`, bundled
per platform in the NuGet package and in the Unity package. csharp/README.md has the
build, the install and the first example, and csharp/unity/README.md the Unity component.
The semantics are the C ones, so docs/node.md, docs/topics.md, docs/patterns.md and
docs/tasks.md apply. This page says what is different in C#.

## A node

`new DartNode(name, onMessage, onEvent, ...)` takes every option as a named parameter,
and 0 or null means the C default. `onMessage` may be null, since subscribers and pattern
handles carry their own handlers. `onEvent` is required and null throws. Both are wired
before the constructor returns. `seedPeers` are "ip" or "ip:port" strings, `unicastOnly`
and `selfIp` with `advertisePort` are the discovery options of docs/discovery.md.

Every call is thread safe. `Start()` runs the C service thread and handlers fire on it
one at a time. To keep handlers on one thread, such as Unity's main thread, skip `Start()`
and call `Poll()` from that thread, or keep `Start()` and call `Dispatch()` once per
frame so every queued handler runs there. `Dispatch()` covers messages only.
`CallbackDispatcher` covers the rest: set it to a delegate that posts to your thread and
every event, pattern handler, variable observer, progress report and awaited call result
runs there instead of on the service thread. A function or task handler that lands this
way has its reply parked first, so it still answers from wherever it runs. From inside a handler, `Send` and read only
queries are allowed, and Poll, topic create, SetRole, Drain, Start, Stop and Close are
refused with `SendStatus.State` or an exception. `Close()` returns false from a handler.

Construction failures throw `DartException`. `DartNode.LastOpenError` holds the text of
the most recent failed open, since there is no handle on failure.

## Schemas

Any struct or class with public fields is a message type, the fields in declaration
order. `[DartArray(n)]` fixes an array's element count and a plain array is a variable
array. `[DartString(cap)]` caps a string and a plain string is unbounded. On a `string[]`,
`[DartString]` alone gives a variable array of capped strings and with `[DartArray]` a
fixed one. `[DartField("name")]` overrides a wire name, needed for lowercase interop
names since C# fields are PascalCase. `[DartSchema("Name")]` overrides the type name.
`[DartTypeName("Timestamp")]` names a field's type with a standard type, and the shape
must be the canonical one or compiling fails. `new Schema(typeof(T)).Dsl` prints the DSL.

A bare type used as a handle's type is the whole schema: `Topic<bool>`, `Topic<float[]>`,
`Topic<string>`, `Topic<Dictionary<string, object>>` or an enum. Such a root is anonymous,
so it is the same bytes and hash in every language. A `Dictionary<string, object>` also
encodes against any compiled schema by field name, nested structs as nested dictionaries.

The standard types of docs/stdtypes.md ship as mirror structs with lowercase wire names,
`DartTimestamp.Now()` is the Timestamp clock, and the video enums carry the wire values.

## QoS

Topic QoS is one `Qos` object passed as the trailing argument to `Topic`, `Topic<T>`,
`Publisher` and `Subscriber`, and a null one means every default. The fields are the C
ones of docs/topics.md under C# names, so `HeartbeatUs` and `BackpressureWaitUs` are
microseconds.

```csharp
var t = new Topic<Pose>(node, "pose", Role.SubOnly,
                        new Qos { Reliability = Reliability.Reliable, KeepLast = 8 });
```

## Messages and queues

`DartMessage` is fully copied out. `Value` or `As<T>()` decodes, `RecvUs` is the node's
monotonic clock at receipt and `WrittenUs` the sender's wall clock, 0 when it opted out.

`TryTake(out msg, timeoutMs)` and `Dispatch(maxMsgs, timeoutMs)` switch a topic to
queued delivery, as in docs/node.md. Dispatch handlers run on the calling thread without
the node lock. `QueueStats` and the traffic counters are always available. `Retire()`
releases the name for a re creation and forgets every wrapper registration for the slot,
since a reused slot may carry a different topic. `Ready` is true when a send would not
wait, and `PendingCount` counts unresolved candidates.

## Functions, tasks and variables

`FunctionDefinition<TReq, TRsp>`, `RemoteFunction<TReq, TRsp>`, `TaskDefinition<TReq,
TPrg, TRsp>`, `RemoteTask<TReq, TPrg, TRsp>`, `VariableDefinition<T>`, `RemoteVariable<T>`,
`Publisher<T>` and `Subscriber<T>` are the typed handles. Each has an untyped core over
`Schema` and `byte[]`.

A simple function handler returns the reply, and a thrown exception answers AppError with
its message. The full form receives a `DartRequest` valid only inside the callback, and
replies, fails or defers. `Defer()` returns a `Deferred` completed from any thread. An
async handler answers the call when its Task completes, and runs on the polling thread
until its first await, so CPU bound work belongs in `Task.Run`. A null handler answers
NoHandler.

A task handler is an async delegate. The call is deferred and RUNNING sent before it is
invoked. Its result answers Ok, `OperationCanceledException` answers Cancelled, any other
exception AppError. It works through a context that streams `Progress` and exposes a
real `CancellationToken`. A completion after Retire or Close is refused by the C and
swallowed.

`Call()` blocks, drives the loop, and is refused from a callback or under a service
thread. `CallAsync()` returns a Task that never faults: inspect `Status` and `SendStatus`,
and reading `Value` when the call did not complete Ok throws `CallException`. On a task,
`CallAsync(req, progress, cancellationToken)` fires progress per update, the untyped form
with a null value for the RUNNING ack and the typed form skipping it, and cancelling the
token requests cooperative cancellation. The overload with `out callId` gives the handle
for `Cancel` from anywhere.

Variables: `TryGet` reads the copied out value, `Set` returns the status, `Force` needs
`allowForce`, and `Wait` blocks until a value exists. The typed `Value` getter throws while
no value exists and the setter throws `DartException` on a non Ok status. `OnChange`
replays the current value at registration and fires on every state change, `OnWrite` on
every applied write, both inline on the applying thread.

## Logs and meta

`Log(level, text)` publishes a line, `OnLog(level, handler)` delivers every other node's
lines as a `DartLogLine`. `MetaAsync(peer, sections)` decodes a peer's snapshot into a
`DartMetaSnapshot` and never faults. `MetaFunction()` is the raw caller handle.
