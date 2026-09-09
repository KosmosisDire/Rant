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

Then add it locally via Package Manager `+`, then "Add package from disk", then `package.json`.
In the plugin import settings, set each native lib to its platform + CPU (Unity usually
auto-detects Standalone by extension: `.dll` is Windows, `.so` is Linux).

## Use

Put one **DartNodeUnity** component in the scene (Add Component > DART > DART DartNode). It
owns the shared node and every other script publishes and subscribes through it:

```csharp
using Dart;
using UnityEngine;

public struct Pose { public float X, Y, Z; }

public class PoseSender : MonoBehaviour {
    DartTopic<Pose> pose;
    void OnEnable() { pose = DartNodeUnity.Topic<Pose>("player/pose"); }
    void Update()   { pose.Publish(new Pose { X = transform.position.x,
                                              Y = transform.position.y,
                                              Z = transform.position.z }); }
}

public class PoseReceiver : MonoBehaviour {
    void OnEnable() => DartNodeUnity.Topic<Pose>("player/pose").Subscribe(this, OnPose);
    void OnPose(Pose p) { transform.position = new Vector3(p.X, p.Y, p.Z); }  // main thread, always
}
```

Topics are shared by name, roles are automatic, and the wire runs on its own thread while
your handlers run on the frame. **docs/unity.md** is the guide: QoS, variables, functions and
tasks, edit mode, the component's settings and the things that catch people out.

## IL2CPP / AOT

The wrapper is IL2CPP-safe: native callbacks are static and marked `[MonoPInvokeCallback]`.
If code stripping removes reflected schema types, add a `link.xml` (or `[Preserve]`) so
your `[DartSchema]` structs keep their fields.
