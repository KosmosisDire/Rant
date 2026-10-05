# Rant for Unity

Rant in Unity: peer discovery + reliable realtime UDP
pub/sub with typed messages. Desktop standalone (Windows, Linux x86_64, macOS) and the Editor.

This is the **same `Rant.cs`** as the NuGet wrapper (one source, in `bindings/csharp/Rant.cs`) plus
a **prebuilt native plugin** per platform. Unity cannot compile the C at build time, so a
native library is required. The package is assembled into `dist/` by a script, so git
keeps one copy of the code and no binaries.

## Install

Add it in the Package Manager (`+`, then "Add package from git URL"):
```
https://github.com/KosmosisDire/Rant.git#upm
```
The `upm` branch is published by CI on each release: it carries the assembled `Rant.cs` +
native plugins (`main` stays binary-free). Or download `rant-<version>.unitypackage` from
the release and import it via `Assets > Import Package > Custom Package`.

## Building the package locally (maintainers)

`tools/unity.cmake` assembles `dist/com.rant.rant/` and `dist/rant-<version>.unitypackage`
from `bindings/csharp/Rant.cs`, this folder's `Runtime/` and the libraries in `dist/native/`. It
writes `package.json` with the version from `VERSION` and a `.meta` for every entry, each
plugin enabled for exactly its platform. Nothing generated lives in this folder.

```sh
cmake -S . -B build
cmake --build build --config Release --target unity_package
```

Then Package Manager `+`, "Add package from disk", `dist/com.rant.rant/package.json`. Only
the platforms built into `dist/native/` get a plugin, so a local package covers this
machine and the release covers Windows, Linux and macOS.

## Use

Put one **RantNodeUnity** component in the scene (Add Component > Rant > Rant RantNode). It
owns the shared node and every other script publishes and subscribes through it:

```csharp
using Rant;
using UnityEngine;
using Std = Rant.Types;   // Unity has its own Pose and Quaternion

public class PoseSender : MonoBehaviour {
    RantTopic<Std.Pose> pose;
    void OnEnable() { pose = RantNodeUnity.Topic<Std.Pose>("player/pose"); }
    void Update() {
        Vector3 p = transform.position; Quaternion q = transform.rotation;
        pose.Publish(new Std.Pose {
            Position    = new Std.Double3 { X = p.x, Y = p.y, Z = p.z },
            Orientation = new Std.Quaternion { X = q.x, Y = q.y, Z = q.z, W = q.w } });
    }
}

public class PoseReceiver : MonoBehaviour {
    void OnEnable() => RantNodeUnity.Topic<Std.Pose>("player/pose").Subscribe(this, OnPose);
    void OnPose(Std.Pose p) {   // main thread, always
        Std.Double3 v = p.Position; Std.Quaternion r = p.Orientation;
        transform.SetPositionAndRotation(new Vector3((float)v.X, (float)v.Y, (float)v.Z),
            new Quaternion((float)r.X, (float)r.Y, (float)r.Z, (float)r.W));
    }
}
```

Topics are shared by name, roles are automatic, and the wire runs on its own thread while
your handlers run on the frame. **docs/unity.md** is the guide: QoS, variables, functions and
tasks, edit mode, the component's settings and the things that catch people out.

## IL2CPP / AOT

The wrapper is IL2CPP-safe: native callbacks are static and marked `[MonoPInvokeCallback]`.
If code stripping removes reflected schema types, add a `link.xml` (or `[Preserve]`) so
your `[RantSchema]` structs keep their fields.
