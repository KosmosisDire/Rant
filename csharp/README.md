# Dart.Middleware

C# wrapper for **DART** (Discovery And Realtime Transport): peer discovery over UDP
multicast plus reliable realtime UDP pub/sub, with typed (schema) messages.

`Dart.cs` is a thin P/Invoke layer over a **prebuilt native library** (`dart`), bundled
per-platform (win-x64, linux-x64). A node is **single-threaded**: you drive `Poll()` in
your own loop.

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
dotnet add package Dart.Middleware
```

(Or attach the `.nupkg` to a GitHub Release and download it — release assets need no key.)

## Use

```csharp
using Dart;

[DartSchema] public struct Twist { public float Dx; public float Dy; }
[DartSchema] public struct Pose  {
    public ulong Stamp; public double X; public double Y;
    [DartArray(4)] public byte[] Uuid; public Twist Vel;
}

var node = Node.Open("robot1", new NodeOptions { Domain = 7 },
                     onMessage: m => Console.WriteLine(m.As<Pose>()));
var ch = node.CreateChannel("pose", Role.PubSub, typeof(Pose),
                            new Qos { Reliability = Reliability.Reliable });
while (true) {                                  // single-threaded: drive poll yourself
    node.Poll(1);                               // discovery, RX+delivery, timers, TX flush
    ch.Send(new Pose { Stamp = 1, X = 1, Y = 2 });
}
```

Wire field names are the C# field names (override with `[DartField("stamp")]`) and must
match on every node for a topic. `Schema.FromType(typeof(Pose)).Dsl` prints the DSL for
pasting into a C/C++ node. Do NOT call `Node`/`Channel` methods from inside a handler.
