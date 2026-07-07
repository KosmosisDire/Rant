# DART for Unity

Discovery And Realtime Transport in Unity: peer discovery + reliable realtime UDP
pub/sub with typed messages. Desktop standalone (Windows / Linux x86_64) and the Editor.

This is the **same `Dart.cs`** as the NuGet wrapper (one source, in `csharp/Dart.cs`) plus
a **prebuilt native plugin** — Unity cannot compile the C at build time, so a native
library is required. To keep one copy of the code and no binaries in git, the package's
`Runtime/Dart.cs` and `Runtime/Plugins/` are **assembled by a script** (both gitignored).

## Build the package

```sh
# 1. build the native lib(s) on each target OS (or in CI)
powershell -File ../native/build.ps1     # -> csharp/runtimes/win-x64/native/dart.dll
sh          ../native/build.sh           # -> csharp/runtimes/linux-x64/native/libdart.so
# 2. assemble Runtime/ (copies Dart.cs + the built libs in)
powershell -File pack.ps1                # or: sh pack.sh
```

Then use it in Unity one of these ways:
- **Add package from disk:** Package Manager -> `+` -> "Add package from disk" -> pick
  `csharp/unity/package.json`.
- **Tarball / .unitypackage:** zip the assembled `Runtime/` for distribution (a downloadable
  `.tgz` added via "Add package from tarball", or a `.unitypackage`). No account/keys needed.

In the plugin import settings, set each native lib to its platform + CPU (Unity usually
auto-detects Standalone by extension: `.dll` -> Windows, `.so` -> Linux).

## Use

```csharp
using Dart;

[DartSchema] public struct Pose {
    public ulong Stamp; public double X; public double Y;
    [DartArray(4)] public byte[] Uuid;
}

// A node is single-threaded: poll it from Update() (or a dedicated loop). OnMessage
// fires on the thread that calls Poll, so from Update() you can touch the Unity API.
Dart.Node _node;
Dart.Channel _ch;
void Start() {
    _node = Node.Open("player1", new NodeOptions { Domain = 7 },
                      onMessage: m => transform.position = ToVec(m.As<Pose>()));
    _ch = _node.CreateChannel("pose", Role.PubSub, typeof(Pose));
}
void Update()      { _node.Poll(0); /* discovery/RX/TX; delivers messages inline */ }
void OnDestroy()   { _node.Close(); }
```

Wire field names are the C# field names (override with `[DartField("stamp")]`) and must
match on every node for a topic. Do NOT call `Node`/`Channel` methods from inside the
message handler.

## IL2CPP / AOT

The wrapper is IL2CPP-safe: native callbacks are static and marked `[MonoPInvokeCallback]`.
If code stripping removes reflected schema types, add a `link.xml` (or `[Preserve]`) so
your `[DartSchema]` structs keep their fields.
