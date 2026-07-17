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

Put one **UnityDartNode** component in the scene (Add Component > DART > DART DartNode). It owns
the shared node (name, domain, lifecycle) and every other script publishes/subscribes
through it:

```csharp
using Dart;
using UnityEngine;

public struct Pose { public float X, Y, Z; }

public class PoseSender : MonoBehaviour {
    DartTopic<Pose> pose;
    void Start()  { pose = UnityDartNode.Topic<Pose>("player/pose"); }
    void Update() { pose.Publish(new Pose { X = transform.position.x,
                                            Y = transform.position.y,
                                            Z = transform.position.z }); }
}

public class PoseReceiver : MonoBehaviour {
    void Start() { UnityDartNode.Subscribe<Pose>("player/pose", this, OnPose); }
    void OnPose(Pose p) { transform.position = new Vector3(p.X, p.Y, p.Z); }  // main thread, always
}
```

- **Handlers always fire on the main thread.** The node runs the C service thread (the
  wire never waits for a frame); every topic is queued and UnityDartNode dispatches once
  per frame, before other scripts' `Update()`.
- **Topics are shared by name**: every script asking for `"player/pose"` gets the same
  `DartTopic<Pose>`. Roles are automatic: created inactive, the first `Publish`
  advertises pub, the first `Subscribe` advertises sub, the last unsubscribe withdraws it.
- **Owner-bound subscriptions** (`Subscribe(name, this, handler)`) die with their
  component and are skipped while it is disabled. The ownerless overload returns a
  `DartSubscription`: dispose it yourself.
- **Edit mode**: `UnityDartNode` is `[ExecuteAlways]`; with Run In Edit Mode on (default) the
  node is live in the editor outside play. Whether your publishers/subscribers run at
  edit time is up to them; a topic acquired while the node is closed goes live when it
  opens.
- **Events** (peer up/down, message loss, errors) are logged to the Console (toggle on
  the component) and observable via `UnityDartNode.Events`, on the main thread.
- **Escape hatch**: `UnityDartNode.Main.Raw` is the underlying `DartNode`, `topic.Raw` the
  underlying `Topic` (TryTake, Drain, QueueStats...). The low-level wrapper (`new
  DartNode(...)` + `Poll()`/`Start()`) remains fully usable without the component.

Any struct/class with public fields is a message type. Wire field names are the C#
field names (override with `[DartField("stamp")]`; `[DartString(cap)]` is required on
string fields, `[DartArray(n)]` on fixed arrays) and must match on every node for a
topic. `Publish` is thread-safe from any thread.

## IL2CPP / AOT

The wrapper is IL2CPP-safe: native callbacks are static and marked `[MonoPInvokeCallback]`.
If code stripping removes reflected schema types, add a `link.xml` (or `[Preserve]`) so
your `[DartSchema]` structs keep their fields.
