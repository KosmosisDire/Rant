# C#

`bindings/csharp/Rant.cs` is a P/Invoke layer over the native library `rant`, bundled per
platform in the NuGet package and in the Unity package. bindings/csharp/README.md has the
build and the install, and docs/unity.md the Unity component. The semantics are the C
ones, so docs/node.md, docs/topics.md, docs/patterns.md and docs/tasks.md apply. This page
says what is different in C#. Every public type and member carries a summary, and the
package ships them as `Rant.xml`, so an editor shows them as tooltips.

```csharp
using Rant;
using Rant.Types;

public record Pose(double X, double Y, [property: RantString(16)] string Frame);

var robot = new RantNode("robot", new NodeOptions { Domain = 7 });
var pub = robot.Publisher<Pose>("pose", new Qos { Reliability = Reliability.Reliable });
robot.FunctionDefinition<(double, double), double>("add", r => r.Item1 + r.Item2);

var viewer = new RantNode("viewer", new NodeOptions { Domain = 7 });   // usually another process
viewer.OnEvent += e => Console.Error.WriteLine(e);
viewer.Subscriber<Pose>("pose", p => Console.WriteLine(p.X));
double sum = viewer.RemoteFunction<(double, double), double>("add").Call((2, 3));
pub.Send(new Pose(1, 2, "map"));
```

## A node

`new RantNode(name, options)` opens a node, both arguments optional. `NodeOptions` holds
the C node options under C# names, where 0, false or null means the C default: `Domain`,
`MaxTopics`, `MulticastInterface`, `MatchWaitMs`, `FetchDetails`, the discovery options of
docs/discovery.md (`SeedPeers` as "ip" or "ip:port" strings, `UnicastOnly`, `SelfIp` with
`AdvertisePort`) and the rest. `node.OnEvent` carries peer lifecycle, loss and error
events. It is optional, since `LastError` records the last error either way, null before
any. `Stats` reads the node's memory, backpressure and evicted unsent counters.

Every handle comes from the node method named after it: `Publisher<T>`, `Subscriber<T>`,
`FunctionDefinition<TReq, TRsp>`, `RemoteFunction<TReq, TRsp>`,
`TaskDefinition<TReq, TPrg, TRsp>`, `RemoteTask<TReq, TPrg, TRsp>`,
`VariableDefinition<T>` and `RemoteVariable<T>`. A topic takes a `Qos`, a pattern handle
its options object, `FunctionOptions`, `TaskOptions` or `VariableOptions`. Every handle is
`IDisposable`: `Dispose()` retires it and releases the name. Closing the node invalidates
every handle it gave out, so a call on one that outlived its node returns `NoTopic`
rather than reading freed memory.

Every call is thread safe. `NodeOptions.Threading` says where every callback runs:
subscriber handlers, OnEvent, OnLog, function and task handlers, progress, OnChange,
OnWrite and awaited call results.

- `ServiceThread`, the default: the service thread runs the loop from construction and
  every callback fires on it, one at a time. From inside one, `Send` and read only
  queries work, while `Poll`, handle creation, `Dispose` and `Close` are refused with
  `SendStatus.State` or an exception. `Close()` returns false from a callback.
- `Manual`: your thread calls `Poll()` and callbacks fire there, under the same rules.
- `Dispatch`: the service thread runs the loop, every callback waits, and `Dispatch()`
  runs the waiting ones on the calling thread with the whole API available. Call it once
  per frame. `Dispatch(maxCallbacks, timeoutMs)` caps a burst and can wait for the first
  one. A build without threads makes `Dispatch()` poll as well.

A handle that wants its own thread takes a queue from `node.CreateQueue()` as the `Queue`
of its options, `Qos.Queue` on a subscriber, and that thread calls `queue.Dispatch()`.
One thread drains a queue at a time, and same name handles share their slot's queue.
State applies at receipt either way: a variable holds its newest value before its
`OnChange` runs, and `Call()` completes without a dispatch.

Everything the wrapper throws is a `RantException`. A failed create, of the node or of a
handle, carries the C reason as a `RantEvent` in `Error` and in the message. A refused
action, such as a `Dispose` from a service thread callback, carries its `SendStatus`.
`SchemaException` and `CallException` derive from it. The data path never throws: `Send`
and `Set` return a `SendStatus`. A build without threads refuses the service thread, and
`Threading.Manual` or `Threading.Dispatch` is the way in.

## Schemas

A struct, class or record is a message type: its public fields and auto properties are
the members, in the order written. `{ get; set; }`, `{ get; init; }` and a positional
record's parameters all count. A property with a body of its own is computed from the
others and is not sent. A decoded message is built with no arguments and its setters, or
through the constructor whose parameters name the members, and a getter only member that
neither fills is refused at `node.Schema`. An anonymous object encodes by name against any
schema. `node.Schema(typeof(T)).Dsl` prints the reflected DSL.

A member's wire name is its own name in the wire spelling, camelCase: `FrameId` is
`frameId`, the same field a Python `frame_id` or a C `frameId` names (docs/stdtypes.md).
The type name is the class name. Attributes change the defaults, and on a record parameter
one is written `[property: RantField("w")] double Omega`.

- `[RantArray(n)]` fixes an array at n elements. A plain array is a variable one.
- `[RantString(cap)]` caps a string at cap bytes. A plain string is unbounded. On a
  `string[]` it alone gives a variable array of capped strings, with `[RantArray]` a fixed
  one.
- `[RantField("name")]` gives a member another wire name.
- `[RantSchema("Name")]` gives the type another wire name.
- `[RantTypeName("Timestamp")]` spells a plain member's type as a standard type, and the
  shape must be the canonical one or compiling fails.

A struct member of another struct, or an array of structs (`Corner[]`,
`[RantArray(4)] Corner[]`, `Float3[]`), takes that type's name on the wire. The reflected
text defines the nested type once, as `Corner { x: f32, y: f32 }`, and each use is the
name, so a C or Python peer spells `Corner` too. A name has one shape per node, so a
nested type called `Twist` with fields of its own is refused, as the standard `Twist`
already is. A struct array's elements are fixed types only, so a struct array inside one
takes `[RantArray(n)]`: `Outline[]` whose `Outline` holds `[RantArray(4)] Float2[]` works.

A bare type as a handle's type is the whole schema: `Publisher<bool>`,
`Subscriber<float[]>`, `VariableDefinition<string>`,
`Publisher<Dictionary<string, object>>`, an enum, or a struct array such as
`Publisher<Corner[]>`, whose element is defined above it. Such a root is anonymous, so it
is the same bytes and hash in every language.

A tuple such as `(double, double)` has no name of its own, so the handle names it after
itself in PascalCase: `AddReq` and `AddRsp` on function `add`, `AddPrg` on a task,
`MotorSpeed` on a topic or variable named `motor/speed`. Every run of letters and digits
is a word and everything else is dropped. Its fields are `item1`, `item2` and so on. Both
ends of a C# pair derive the same name, and another language must spell it.
`node.Schema(typeof((double, double)))` on its own throws.

A `Dictionary<string, object>` encodes against any compiled schema by field name, the keys
in any spelling, nested structs as nested dictionaries and a struct array as a list of
them. Without a C# type a message decodes the same way, the keys in wire spelling.

Every typed handle also takes its schema as an optional argument: `schema:` on a topic or
variable, `requestSchema:`, `responseSchema:` and `progressSchema:` on a function or task.
The type argument is then only the C# shape the values pass through, and the wire type is
exactly the given schema: `Subscriber<string[]>` with `node.Schema("string<128>[]")` reads
a bare capped string array. A `byte[]` type carries the encoded message bytes as they are,
so `Subscriber<byte[]>` with no schema is a raw topic and `RemoteFunction<byte[], byte[]>`
with explicit schemas is the untyped form.

`node.Schema(text)` compiles DSL text in the node's registry. Every definition stays in
scope for the node's later compiles, and a name on its own is a schema. The node owns
every `Schema` for its life, one handle per shape, so there is nothing to dispose, and a
read after `Close` throws.

The standard types of docs/stdtypes.md are mirror structs in the `Rant.Types` namespace,
apart from the root so `Color` or `Quaternion` never clashes with a Unity or
`System.Numerics` type under `using Rant;`. `Rant.Types.Timestamp.Now()` is the Timestamp
clock, and the video enums carry the wire values.

## QoS and messages

A topic's QoS is one `Qos` object, and a null one means every default. The fields are the
C ones of docs/topics.md under C# names, and a `Us` suffix means microseconds.

```csharp
var qos = new Qos { Reliability = Reliability.Reliable, KeepLast = 8 };
var sub = node.Subscriber<Pose>("pose", (pose, msg) => Console.WriteLine($"{pose.X} from {msg.PublisherName}"), qos);

var frames = node.Subscriber<Pose>("pose");        // no handler: pulled
if (frames.TryTakeLatest(out Pose newest)) Draw(newest);
```

A subscriber's handler is given at creation and takes the value alone, or the value and the
`RantMessage` envelope. Without a handler the subscriber is pulled: `TryTake(out v,
timeoutMs)` reads the oldest waiting message and `TryTakeLatest` the newest, dropping the
older ones, on whatever thread calls. A pulled subscriber takes no `Qos.Queue` and never
parks on the node's Dispatch queue. The envelope copies
its payload out, so `Data` outlives the callback. Its `Value` decodes on the first read and
never if not read, so a handler that only wants the bytes pays nothing for the schema.
Read one message from one thread, as a handler does. `RecvUs` is this node's monotonic
clock at receipt, `WrittenUs` the sender's wall clock, 0 when it opted out, and
`CaptureUs` when the publisher says the data was true, 0 when it gave none.
`Send(value, captureUs)` sets that last one.

`Qos.QueueBytes` caps a queued subscriber's ring, 0 meaning 1 MB, and `QueueStats()` reads
it. Same name handles on one node share the topic slot: `Dispose()` on one leaves its
siblings receiving, and the last one retires the slot so the name can carry another
schema. `Ready` on a publisher is true when a send would not wait, and `MatchCount` counts
its matched subscribers. A subscriber has no count, since the C gives none on that side.

`Qos.ReflectFromMesh`, and the same field on the pattern options, is not a QoS field: a
null schema and a best effort reliability then follow the mesh. Every handle that can
carry it has `Refresh()`, which re types it in place when the mesh moved and returns true
when it did. docs/reflection.md says what gets adopted.

## Functions and tasks

A function handler returns the reply, and a thrown exception answers AppError with its
message. The full form takes a `RantRequest<TRsp>`, valid only inside the callback, which
replies, fails or defers. `Defer()` returns a `Deferred<TRsp>` that completes from any
thread. An async handler answers when its Task completes and runs on the loop thread until
its first await, so CPU bound work belongs in `Task.Run`. A null handler answers
NoHandler.

A task handler is an async delegate taking the request and a `TaskContext<TPrg>`. The call
is deferred and RUNNING sent before it runs. Its result answers Ok,
`OperationCanceledException` answers Cancelled and any other exception AppError. The
context streams `Progress` and exposes a real `CancellationToken`. A completion after
`Dispose` or `Close` is refused by the C and dropped.

A call is loud. `Call(req)` blocks and returns the response, `await CallAsync(req)` does the
same without blocking, and both throw `CallException` when the call did not end Ok. The
exception names the call and carries `Status`, `SendStatus`, `Provider` and the provider's
text, for a handler that threw its exception message. `TryCall` and `TryCallAsync` are the
quiet forms for an expected failure: they return a `RantResponse<TRsp>` and never throw,
and reading its `Value` off Ok throws the same `CallException`. `Call()` waits on the
service thread or drives a Manual node's loop, and is refused from a service thread
callback. On a task, `CallAsync(req, progress, cancellationToken)` reports each progress
update, skipping the valueless RUNNING. Cancelling the token asks for a cooperative
cancel, and a task that honors it ends the awaited call with
`OperationCanceledException`, or with status Cancelled from `TryCallAsync`.

`MatchCount` on a pattern handle counts the other side: definitions on a remote, callers
or remotes on a definition.

## Variables

`TryGet` reads a copy of the value, `Set` returns the status, `Force` needs `AllowForce`
in the definition's options and `Unforce` ends it, and `Wait` blocks until a value exists.
The `Value` getter throws while no value exists and its setter throws `RantException` on a
refused set.

`OnChange` and `OnWrite` are events whose handlers take the value and the
`VariableUpdate` envelope, on the thread the node's threading gives every callback.
`OnChange` fires on every state change and replays the current value to a handler as it is
added. `OnWrite` fires on every applied write, identical bytes or not, with no replay.

## Reflection and logs

The walks of docs/reflection.md live on `node.Reflection` and return copied snapshots, so
they outlive the loop and need no lock. `Peers()` lists every discovered peer as a
`RantPeer`, dropped ones included, so check `Active`. `Entities(peer)` lists what one node
offers, peer 0 for this one, and `Mesh()` folds the whole mesh into one `RantEntity` per
kind and name. `Find(kind, name)` returns one or null, and `Epoch` moves whenever the
folded view changed. A walk right after open is partial: names and schemas arrive as
details, so walk again when `Epoch` moved. The schemas on a `RantEntity` are owned copies,
null when untyped or not fetched, and `FetchDetails` on the node makes them arrive.
`MetaAsync(peer, sections)` decodes a peer's snapshot into a `RantMetaSnapshot` and throws
`CallException` when the peer did not answer.

`Schema.Fields` is the flat depth first field table as `SchemaField` rows, and
`Schema.EnumVariants(field)` the options of an enum field, for a tool that renders a
schema it has never seen.

`node.Log(level, text)` publishes a line at a `LogLevel`, and the `node.OnLog` event
delivers every other node's lines at every level as a `RantLogLine`, at `Dispatch()` under
`Threading.Dispatch` like every other handler. It throws on a node opened with `DisableLogs`.
