# DART for Unity

Discovery And Realtime Transport in Unity: peer discovery + reliable realtime UDP
pub/sub with typed messages. Desktop standalone (Windows, Linux x86_64, macOS) and the Editor.

This is the **same `Dart.cs`** as the NuGet wrapper (one source, in `csharp/Dart.cs`) plus
a **prebuilt native plugin** per platform. Unity cannot compile the C at build time, so a
native library is required. The package is assembled into `dist/` by a script, so git
keeps one copy of the code and no binaries.

## Install

Add it in the Package Manager (`+`, then "Add package from git URL"):
```
https://github.com/KosmosisDire/DART.git#upm
```
The `upm` branch is published by CI on each release: it carries the assembled `Dart.cs` +
native plugins (`main` stays binary-free). Or download `dart-<version>.unitypackage` from
the release and import it via `Assets > Import Package > Custom Package`.

## Building the package locally (maintainers)

`tools/unity.cmake` assembles `dist/com.rant.dart/` and `dist/dart-<version>.unitypackage`
from `csharp/Dart.cs`, this folder's `Runtime/` and the libraries in `dist/native/`. It
writes `package.json` with the version from `VERSION` and a `.meta` for every entry, each
plugin enabled for exactly its platform. Nothing generated lives in this folder.

```sh
cmake -S . -B build
cmake --build build --config Release --target unity_package
```

Then Package Manager `+`, "Add package from disk", `dist/com.rant.dart/package.json`. Only
the platforms built into `dist/native/` get a plugin, so a local package covers this
machine and the release covers Windows, Linux and macOS.

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
