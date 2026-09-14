# Unity

Rant in Unity is the C# wrapper of docs/csharp.md plus one component that owns the node for
the whole scene. Every script talks to that component instead of opening its own node.
bindings/csharp/unity/README.md covers installing the package. This page is what the component adds,
and everything it does not mention behaves as docs/csharp.md describes.

## Set it up

Add one **RantNodeUnity** component to any GameObject in the scene (Add Component > Rant >
Rant RantNode). That is the whole setup. It opens the node, finds peers, and shuts down with
the scene. Only one is allowed, and a second warns and stays inactive.

## Send and receive

```csharp
using Rant;
using UnityEngine;

public struct Pose { public float X, Y, Z; }

public class PoseSender : MonoBehaviour {
    RantTopic<Pose> pose;
    void OnEnable() { pose = RantNodeUnity.Topic<Pose>("player/pose"); }
    void Update()   { pose.Publish(new Pose { X = transform.position.x,
                                              Y = transform.position.y,
                                              Z = transform.position.z }); }
}

public class PoseReceiver : MonoBehaviour {
    void OnEnable() => RantNodeUnity.Topic<Pose>("player/pose").Subscribe(this, OnPose);
    void OnPose(Pose p) { transform.position = new Vector3(p.X, p.Y, p.Z); }
}
```

`RantNodeUnity.Topic<T>(name)` gives you the topic of that name. Ask for the same name from
ten scripts and you get the same `RantTopic<T>` back, so a topic is shared across the scene
rather than duplicated. `RantNodeUnity.Topic(name)` is the raw form carrying `byte[]` or a
UTF-8 `string`.

Message types are written as docs/csharp.md describes, and the field names have to match on
every node using the topic.

## Your handlers always run on the main thread

This is what makes the code above safe, and it is the main thing the component buys you.

The network runs on its own thread and never waits for a frame. Received messages are copied
into a queue per topic, and once per frame, before other scripts' `Update()`, the component
drains those queues and calls your handlers. It also sets the node's `CallbackDispatcher`, so
peer events, variable observers, function handlers and the result of a call you awaited all
land on the frame too. You can touch transforms, UI and any other Unity object from any of
them. Nothing is thrown away to keep up.

`Publish` is safe from any thread at any time, and it never blocks the frame: the component
turns off the send path match wait, so a publish before anybody is matched returns at once
instead of pausing. Check `topic.Ready` if you would rather hold a payload back.

## Roles happen by themselves

A topic starts advertising nothing. The first `Publish` tells the network this node
publishes, the first `Subscribe` tells it this node subscribes, and the last unsubscribe
withdraws that interest. You never set a role by hand.

## Subscriptions and lifetime

Pass a component first and the subscription belongs to it:

```csharp
topic.Subscribe(this, OnPose);
```

It stops when that component is destroyed and is skipped while it is disabled. This is what
you want almost always. Leave the owner out and you get a `RantSubscription` to dispose
yourself. Both forms have an overload taking `(T value, RantMessage message)` when you want
the sender or the clocks beside the value.

## QoS

QoS is the `Qos` object of docs/csharp.md, passed the first time a name is requested. Later
callers share the topic that already exists, so their QoS is ignored and the Console says so.

```csharp
RantNodeUnity.Topic<Pose>("player/pose", new Qos { MaxRateHz = 30 })
             .Subscribe(this, OnPose);
```

Two rules are specific to Unity. Every topic is queued, because that is how handlers reach
the frame, so `QueueBytes` sizes that queue rather than turning it off. And the queue size
left at zero comes from the component's Queue Bytes setting rather than the C default.

For a panel that carries no message type of its own and wants to show whatever is on the
network, `new Qos { ReflectFromMesh = true }` takes the schema from the provider instead.
docs/csharp.md covers it and the `Refresh()` that re types a handle later.

## Variables, functions and tasks

These are the wrapper's own types, described in docs/csharp.md and docs/patterns.md. The
component hands them out shared by name, the same way it hands out topics, so several
scripts can use one variable.

```csharp
public class MotorPanel : MonoBehaviour {
    RemoteVariable<float> speed;
    void OnEnable() {
        speed = RantNodeUnity.RemoteVariable<float>("motor/speed");
        RantNodeUnity.Bind(this, speed.OnChange(v => slider.value = v));
    }
}
```

The shorthands are `VariableDefinition<T>(name)`, `VariableDefinition<T>(name, initial)`,
`RemoteVariable<T>(name)`, `FunctionDefinition<TReq, TRsp>(name, handler)`,
`RemoteFunction<TReq, TRsp>(name)`, `TaskDefinition<TReq, TPrg, TRsp>(name, handler)` and
`RemoteTask<TReq, TPrg, TRsp>(name)`. They take no options. For anything the shorthands do
not cover, build the handle yourself and let the component share it:

```csharp
RantNodeUnity.Shared("motor/speed",
    n => new RemoteVariable<float>(n, "motor/speed", catchUp: 5));
```

`RantNodeUnity.Bind(component, subscription)` disposes an observer when that component is
destroyed, which is the pattern side of an owner bound subscription.

## Edit mode and reopening

The component is `[ExecuteAlways]` and Run In Edit Mode is on by default, so the node is live
in the editor outside play mode. Peers see it and topics work. Whether your own scripts run
at edit time is up to them.

The node is closed and reopened when you change one of its settings in the inspector, and
around an editor script recompile. Topics survive that on their own. Pattern handles are
rebuilt, so ask for one again rather than keeping it in a field across a reopen, and observers
you registered on the old one are gone. `OnEnable` is the right place, since it runs again
after a recompile. A handle left over from before a reopen refuses cleanly, it never
misbehaves.

## Settings on the component

| setting | meaning |
|---|---|
| Node Name | the name peers see. Empty means one is generated |
| Domain | nodes only see peers on the same domain |
| Max Topics | how many topics this node may create |
| Multicast Interface | this machine's LAN IP, for hosts with VPN or docker adapters. Empty means probe for it |
| Queue Bytes | default size of each topic's frame queue |
| Dispatch Budget | most messages handled per frame, 0 = all of them. Set it to bound a frame under a burst |
| Run In Edit Mode | keep the node live outside play mode |
| Log Events | print peer lifecycle, loss and errors to the Console |
| Persist Across Scenes | survive scene loads in play mode |

## Watching what happens

`RantNodeUnity.Events` reports peers joining and leaving, lost messages and errors, on the
main thread. With Log Events on they also go to the Console. On a `RantTopic`, `Matches`
counts matched remote endpoints and `SubscriberCount` your own handlers.

`RantNodeUnity.Main.Raw` is the underlying `RantNode` and `topic.Raw` the underlying `Topic`,
which is where the rest of docs/csharp.md lives. The plain wrapper also works on its own in
Unity without the component.

## Things that catch people out

- **Nothing arrives.** Check both sides use the same Domain, and that the message type's
  field names and types match exactly on both.
- **It works in the editor but not between two machines.** Set Multicast Interface to the
  machine's LAN IP on hosts that have VPN, WSL or docker adapters.
- **A handler stops firing.** An owner bound subscription is skipped while its component is
  disabled and ends when the component is destroyed.
- **QoS seems ignored.** Only the first request for a name creates the topic. The Console
  warns when a later request passes QoS that cannot apply.
- **A pattern handle went quiet.** It was rebuilt by a node reopen. Ask for it again in
  `OnEnable` and register the observer there.
