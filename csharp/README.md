# Dart

C# wrapper for **DART** (Discovery And Realtime Transport): peer discovery over UDP
multicast plus reliable realtime UDP pub/sub, with typed (schema) messages.

`Dart.cs` is a thin P/Invoke layer over a **prebuilt native library** (`dart`), bundled
per-platform (win-x64, linux-x64). Every call is **thread-safe** (a node-level lock in
the C core): drive a node with `Start()` (a C background service thread runs the loop
and fires handlers) or by calling `Poll()` from your own loop.

## Layout

```
csharp/
  Dart.cs               the wrapper (the one source; compiled by the package + examples)
  Dart.csproj           NuGet package: Dart.cs + runtimes/<rid>/native/  ->  .nupkg
  native/               build.ps1 / build.sh / dart.def  (build the native lib per platform)
  runtimes/<rid>/native/   prebuilt libs (gitignored; built by the scripts or CI)
  test/ publisher/ subscriber/    console examples (compile ../Dart.cs)
  unity/                Unity package (see unity/README.md)
```

## Build + package

```sh
# 1. build the native lib(s)  (do this on each target OS, or in CI)
powershell -File csharp/native/build.ps1      # Windows -> runtimes/win-x64/native/dart.dll
sh          csharp/native/build.sh            # Linux   -> runtimes/linux-x64/native/libdart.so
# 2. pack the NuGet
dotnet pack csharp/Dart.csproj -c Release -o nupkg
```

The binaries are **not committed** to git; the build scripts (or a CI job) produce them
before packing. The resulting `.nupkg` bundles `lib/netstandard2.1/Dart.dll` +
`runtimes/<rid>/native/*`.

## Install (no nuget.org, no keys)

Put the `.nupkg` in a folder, point a local NuGet source at it, and add the package:

```sh
dotnet nuget add source ./path/to/nupkg -n dart-local
dotnet add package Dart
```

(Or attach the `.nupkg` to a GitHub Release and download it: release assets need no key.)

## Use

```csharp
using Dart;

public struct Twist { public float Dx; public float Dy; }
public struct Pose  {
    public ulong Stamp; public double X; public double Y;
    [DartArray(4)] public byte[] Uuid;
    [DartString(16)] public string Frame;
    public Twist Vel;
}

var node = new DartNode("robot1",
                    onMessage: m => Console.WriteLine(m.As<Pose>()),
                    onEvent: e => Console.Error.WriteLine(e),   // required: it carries the diagnostics
                    domain: 7);                                 // all options are named parameters
var ch = new Topic<Pose>(node, "pose", reliable: true);
node.Start();                                    // C-level service thread owns the loop
ch.Send(new Pose { Stamp = 1, X = 1, Frame = "map" });   // thread-safe from any thread
// (or skip Start() and drive node.Poll(1) in your own loop)
```

The patterns layer is bound too, untyped (`Schema` + `byte[]`) and typed:

```csharp
// request/response: ONE definition on the network, callers anywhere
var def = new FunctionDefinition<AddReq, AddRsp>(node, "add", q => new AddRsp { Sum = q.A + q.B });
var fn  = new RemoteFunction<AddReq, AddRsp>(other, "add");
var rsp = fn.Call(new AddReq { A = 2, B = 3 });          // blocking; rsp.Ok / rsp.Value
var t   = fn.CallAsync(new AddReq { A = 2, B = 3 });     // Task<Response<AddRsp>>, never faults

// replicated state: ONE owner, remotes read the cached latest and push writes
var own = new VariableDefinition<Level>(node, "level", new Level { Value = 5 });
var acc = new RemoteVariable<Level>(other, "level");     // acc.Value / acc.Set(...) / acc.Wait(...)

// reliable fire-and-forget event: a handler IS the subscription, every handle may emit
var sig = new Signal(node, "estop", handler: m => Stop());
new Signal(other, "estop").Emit();

// side-named topic handles (share the topic slot by name, widening the role)
var pub = new Publisher<Pose>(node, "pose", reliable: true);
var sub = new Subscriber<Pose>(other, "pose", p => Console.WriteLine(p.X));
```

Any struct/class with public fields is a message type: the fields become the schema in
declaration order. `[DartArray(n)]` fixes an array's element count, `[DartString(cap)]`
fixes a string's UTF-8 byte capacity (required on every string; combine both for a
`string[]`), `[DartField("stamp")]` overrides a wire field name, and `[DartSchema("Name")]`
optionally overrides the wire type name. Wire names must match on every node for a
topic. `new Schema(typeof(Pose)).Dsl` prints the DSL for pasting into a C/C++ node.
Handlers fire on the service thread (never two at once for one node); from inside a
handler, `Topic.Send` and read-only queries are allowed, Poll/topic
create/SetRole/Drain/Start/Stop/Close are not. To keep handlers on one thread (e.g.
Unity's main thread), skip `Start()` and call `Poll()` from that thread.
