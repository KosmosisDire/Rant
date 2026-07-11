# DART for Unity

Discovery And Realtime Transport in Unity: peer discovery + reliable realtime UDP
pub/sub with typed messages. Desktop standalone (Windows / Linux x86_64) and the Editor.

This is the **same `Dart.cs`** as the NuGet wrapper (one source, in `csharp/Dart.cs`) plus
a **prebuilt native plugin**. Unity cannot compile the C at build time, so a native
library is required. To keep one copy of the code and no binaries in git, the package's
`Runtime/Dart.cs` and `Runtime/Plugins/` are **assembled by a script** (both gitignored).

## Install

Add it in the Package Manager (`+`, then "Add package from git URL"):
```
https://github.com/KosmosisDire/DART.git#upm
```
The `upm` branch is published by CI on each release: it carries the assembled `Dart.cs` +
native plugins (`main` stays binary-free). Or download `dart-<version>.unitypackage` from
the release and import it via `Assets > Import Package > Custom Package`.

## Building the package locally (maintainers)

`Runtime/Dart.cs` and `Runtime/Plugins/` are gitignored and assembled by a script, so `main`
keeps one copy of the code and no binaries:

```sh
# 1. build the native lib(s) on each target OS (or in CI)
powershell -File ../native/build.ps1     # -> csharp/runtimes/win-x64/native/dart.dll
sh          ../native/build.sh           # -> csharp/runtimes/linux-x64/native/libdart.so
# 2. assemble Runtime/ (copies Dart.cs + the built libs in)
powershell -File pack.ps1                # or: sh pack.sh
# 3. (optional) build a .unitypackage
sh mk-unitypackage.sh dart.unitypackage
```

Then add it locally via Package Manager `+` -> "Add package from disk" -> `package.json`.
In the plugin import settings, set each native lib to its platform + CPU (Unity usually
auto-detects Standalone by extension: `.dll` -> Windows, `.so` -> Linux).

## Use

```csharp
using Dart;

public struct Pose {
    public ulong Stamp; public double X; public double Y;
    [DartArray(4)] public byte[] Uuid;
    [DartString(16)] public string Frame;
}

// Drive the node by polling from Update(): handlers fire inline on the main thread,
// where the Unity API is legal.
Dart.Node _node;
Dart.Channel<Pose> _ch;
void Start() {
    _node = new Node("player1",
                     onMessage: m => transform.position = ToVec(m.As<Pose>()),
                     onEvent: e => Debug.LogWarning(e),
                     new NodeOptions { Domain = 7 });
    _ch = new Channel<Pose>(_node, "pose");
}
void Update()      { _node.Poll(0); /* non-blocking: RX + handlers, main thread */ }
void OnDestroy()   { _node.Close(); }
// (Alternative: _node.Start() runs a C-level background service thread with no Poll()
// anywhere, but handlers then fire on that thread, where the Unity API is NOT legal --
// marshal to the main thread yourself before touching Unity objects.)
```

Any struct/class with public fields is a message type. Wire field names are the C#
field names (override with `[DartField("stamp")]`; `[DartString(cap)]` is required on
string fields) and must match on every node for a topic. Sends are thread-safe from any
thread; from inside a handler, `Channel.Send` and read-only queries are allowed, other
node calls are not.

## IL2CPP / AOT

The wrapper is IL2CPP-safe: native callbacks are static and marked `[MonoPInvokeCallback]`.
If code stripping removes reflected schema types, add a `link.xml` (or `[Preserve]`) so
your `[DartSchema]` structs keep their fields.
