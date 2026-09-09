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

Put one **DartNodeUnity** component in the scene (Add Component > DART > DART DartNode). It owns
the shared node (name, domain, lifecycle) and every other script publishes/subscribes
through it:

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

- **The wire never runs on your thread.** The node runs the C service thread, which fills a
  per topic queue, and DartNodeUnity drains it once per frame before other scripts'
  `Update()`. Everything else the node reports (events, pattern handlers, variable
  observers, awaited call results) is parked by that thread and run on the same frame, so
  **every callback you write is on the main thread**. Nothing is ever dropped to keep up.
- **Topics are shared by name**: every script asking for `"player/pose"` gets the same
  `DartTopic<Pose>`. Roles are automatic: created inactive, the first `Publish`
  advertises pub, the first `Subscribe` advertises sub, the last unsubscribe withdraws it.
- **QoS** is the wrapper's `Qos` object, passed on the first request for a name:
  `DartNodeUnity.Topic<Pose>("player/pose", new Qos { MaxRateHz = 30 })` caps delivery to
  30 Hz per publisher. Defaults are the C ones, best effort included. The one Unity rule is
  that every topic is queued, so `Qos.QueueBytes` sizes that queue rather than opting out.
- **Publishing never blocks the frame.** The node disables the send path match wait, so a
  publish before a subscriber is matched returns at once instead of stalling up to a second.
  Check `topic.Ready` if you would rather hold the payload back.
- **Owner-bound subscriptions** (`Subscribe(this, handler)`) die with their component and
  are skipped while it is disabled. The ownerless overload returns a `DartSubscription`:
  dispose it yourself.
- **Edit mode**: `DartNodeUnity` is `[ExecuteAlways]`. With Run In Edit Mode on (default) the
  node is live in the editor outside play. Whether your publishers/subscribers run at
  edit time is up to them. A topic acquired while the node is closed goes live when it
  opens.
- **Events** (peer up/down, message loss, errors) are logged to the Console (toggle on
  the component) and observable via `DartNodeUnity.Events`, on the main thread.
- **Escape hatch**: `DartNodeUnity.Main.Raw` is the underlying `DartNode`, `topic.Raw` the
  underlying `Topic` (TryTake, Drain, QueueStats...). The low-level wrapper (`new
  DartNode(...)` + `Poll()`/`Start()`) remains fully usable without the component.

Any struct/class with public fields is a message type. Wire field names are the C#
field names (override with `[DartField("stamp")]`, and `[DartString(cap)]` is required on
string fields, `[DartArray(n)]` on fixed arrays) and must match on every node for a
topic. `Publish` is thread-safe from any thread.

## Patterns

Variables, functions and tasks are the **wrapper's own types**, handed out shared by name so
every script gets the same one:

```csharp
public class MotorPanel : MonoBehaviour {
    RemoteVariable<float> speed;
    void OnEnable() {
        speed = DartNodeUnity.RemoteVariable<float>("motor/speed");
        DartNodeUnity.Bind(this, speed.OnChange(v => slider.value = v));
    }
}

DartNodeUnity.FunctionDefinition<int, int>("square", x => x * x);   // handler on the frame

async void OnEnable() {
    var r = await DartNodeUnity.RemoteFunction<int, int>("square").CallAsync(7);
    label.text = r.Value.ToString();                                 // resumes on the frame
}
```

`VariableDefinition<T>`, `RemoteVariable<T>`, `FunctionDefinition<,>`, `RemoteFunction<,>`,
`TaskDefinition<,,>` and `RemoteTask<,,>` all work the same way, and docs/patterns.md
describes them. Two Unity rules:

- **Acquire them in `OnEnable`, not `Start`.** These handles belong to the native node, so a
  node reopen (an inspector change, or an editor assembly reload) replaces them. `OnEnable`
  runs again after both; `Start` does not.
- **`OnChange` and `OnWrite` return an `IDisposable`.** Any number of scripts may observe one
  variable, and a late one is replayed the current value. Pass the handle through
  `DartNodeUnity.Bind(this, ...)` to have it disposed when your component is destroyed.

For options the shorthands above do not take, build the handle yourself and let the node
share it: `DartNodeUnity.Shared("motor/speed", n => new RemoteVariable<float>(n, "motor/speed", catchUp: 5))`.

## IL2CPP / AOT

The wrapper is IL2CPP-safe: native callbacks are static and marked `[MonoPInvokeCallback]`.
If code stripping removes reflected schema types, add a `link.xml` (or `[Preserve]`) so
your `[DartSchema]` structs keep their fields.
