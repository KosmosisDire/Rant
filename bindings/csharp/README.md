# Rant

C# wrapper for **Rant**: peer discovery over UDP
multicast plus reliable realtime UDP pub/sub, with typed (schema) messages.

`Rant.cs` is a thin P/Invoke layer over a **prebuilt native library** (`rant`), bundled
per-platform (win-x64, linux-x64, linux-arm64, osx). Every call is **thread-safe** (a node-level lock in
the C core): a C service thread runs the loop and fires handlers, with `Threading.Manual`
your own loop calls `Poll()`, and with `Threading.Dispatch` your thread calls `Dispatch()`
to run every parked callback.

## Layout

```
bindings/csharp/
  Rant.cs                 the wrapper (the one source; compiled by the package + the test)
  Rant.csproj             NuGet package: Rant.cs + ../../dist/native/<rid>/    ->  .nupkg
  nuget.config            the examples take the packed Rant from ../../dist
  test/                   the binding test (compiles ../Rant.cs)
  publisher/ subscriber/  console examples referencing the Rant package, as a user would
  unity/                Unity package (see unity/README.md)
```

The examples restore the package into `bindings/csharp/.packages`, which a pack clears, so
after a change to Rant.cs run the pack step below and build them again.

## Build + package

```sh
# 1. build the native library (on each target OS, or let the release workflow do it)
cmake -S . -B build
cmake --build build --config Release --target rant_shared     # -> dist/native/<rid>/
# 2. pack the NuGet
dotnet pack bindings/csharp/Rant.csproj -c Release -o dist
```

The binaries are **not committed** to git. The CMake target (or the release workflow)
produces them before packing. The resulting `.nupkg` bundles `lib/netstandard2.1/Rant.dll` +
`runtimes/<rid>/native/*`.

## Install

```sh
dotnet add package Rant
```

Before the package is on nuget.org, download the `.nupkg` from the GitHub Release and
register its folder as a source first:

```sh
dotnet nuget add source ./path/to/that/folder -n rant
dotnet add package Rant
```

## Use

```csharp
using Rant;

public struct Twist { public float Dx; public float Dy; }
public struct Pose  {
    public ulong Stamp; public double X; public double Y;
    [RantArray(4)] public byte[] Uuid;
    [RantString(16)] public string Frame;
    public Twist Vel;
}

var node = new RantNode("robot1", new NodeOptions { Domain = 7 });   // the service thread runs from here
node.OnEvent += e => Console.Error.WriteLine(e);                   // the diagnostics, optional
var sub = node.Subscriber<Pose>("pose");
sub.OnMessage += (p, _) => Console.WriteLine(p.X);
var pub = node.Publisher<Pose>("pose", new Qos { Reliability = Reliability.Reliable });
pub.Send(new Pose { Stamp = 1, X = 1, Frame = "map" });   // thread-safe from any thread
// (or Threading = Threading.Manual in the options and drive node.Poll(1) in your own loop)
```

The patterns layer is bound too. A `byte[]` type argument carries the message bytes as is,
for the untyped case:

```csharp
// request/response: ONE definition on the network, callers anywhere
var def = node.FunctionDefinition<AddReq, AddRsp>("add", q => new AddRsp { Sum = q.A + q.B });
var fn  = other.RemoteFunction<AddReq, AddRsp>("add");
var rsp = fn.Call(new AddReq { A = 2, B = 3 });          // blocking; rsp.Ok / rsp.Value
var t   = fn.CallAsync(new AddReq { A = 2, B = 3 });     // Task<RantResponse<AddRsp>>, never faults

// replicated state: ONE owner, remotes read the cached latest and push writes
var own = node.VariableDefinition<Level>("level", new Level { Value = 5 });
var acc = other.RemoteVariable<Level>("level");          // acc.Value / acc.Set(...) / acc.Wait(...)
```

Any struct/class with public fields is a message type: the fields become the schema in
declaration order. `[RantArray(n)]` fixes an array's element count, `[RantString(cap)]`
fixes a string's UTF-8 byte capacity (required on every string, combine both for a
`string[]`), `[RantField("stamp")]` overrides a wire field name, and `[RantSchema("Name")]`
optionally overrides the wire type name. Wire names must match on every node for a
topic. `new Schema(typeof(Pose)).Dsl` prints the DSL for pasting into a C/C++ node.
Handlers fire on the service thread (never two at once for one node). From inside a
handler, `Send` and read-only queries are allowed, Poll, handle creation, Dispose and
Close are not. To keep handlers on one thread (e.g. Unity's main thread), open the node
with `Threading.Dispatch` and call `Dispatch()` from that thread, where the whole API is
allowed.
