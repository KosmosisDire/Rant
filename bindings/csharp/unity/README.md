# Rant for Unity

Rant in Unity: peer discovery + reliable realtime UDP
pub/sub with typed messages. Desktop standalone (Windows, Linux x86_64, macOS), the Editor,
and WebGL through a Rant bridge (docs/unity.md).

This is the **same `Rant.cs`** as the NuGet wrapper (one source, in `bindings/csharp/Rant.cs`) plus
a **prebuilt native plugin** per desktop platform. A WebGL player compiles the C into its own
wasm from the package's `Plugins/WebGL`. The package is assembled into `dist/` by a script, so git
keeps one copy of the code and no binaries.

## Install

Add both in the Package Manager (`+`, then "Add package from git URL"), the types first, since
Unity fetches no git dependency by itself:
```
https://github.com/KosmosisDire/Rant.git#upm-types
https://github.com/KosmosisDire/Rant.git#upm
```
The `upm` and `upm-types` branches are published by CI on each release: they carry the assembled
`Rant.cs` + native plugins and the standard types (`main` stays binary-free). Or download
`rant-<version>.unitypackage` from the release, which holds both, and import it via
`Assets > Import Package > Custom Package`.

## Building the package locally (maintainers)

`tools/unity.cmake` assembles `dist/com.rant.types/`, `dist/com.rant.rant/` and
`dist/rant-<version>.unitypackage` from `bindings/csharp/types/Types.cs`,
`bindings/csharp/Rant.cs`, this folder's `Runtime/`, the libraries in `dist/native/` and,
for WebGL, the web build in `bindings/web/dist/` (`cmake --preset web`). It
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
