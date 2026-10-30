---
name: rant
description: Writing code with Rant, the robotics middleware, in C, C++, Python, C#, Rust or TypeScript: nodes, topics, variables, functions, tasks, threading, standard types and names. Use when code includes rant.hpp or rant.h, imports rant or rant-middleware, uses Rant, or the user asks to write or change a Rant node.
---

# Rant

Rant connects programs ("nodes") on one machine or a network ("the mesh"). Nodes find each
other on their own and share four kinds of entity: topics, variables, functions and tasks.
This skill is the code. The `rant` command makes, builds and runs nodes, and its own skill
`rant-skill` comes with `rant install rant_skill`. Full docs:
https://docs.rantlib.dev/llms.txt (fetch the page you need).

- Read the project before changing it. Keep its layout, names and style. The rules below
  are for new work and for what the project leaves open.
- Add Rant to a package with `rant add`, never by hand. `rant new package` and `rant new node`
  make one with Rant added.

## Writing nodes

```cpp
#include "rant.hpp"   // CMake: target_link_libraries(x PRIVATE rant::rant_host)
rant::Node node("talker");
auto pose = node.publisher<rant::types::Pose2D>("Pose");
pose.send({ { 1.0, 2.0 }, 0.5 });
auto sub = node.subscriber<rant::types::Pose2D>("Pose", [](const rant::types::Pose2D& p) { /* ... */ });
```

```python
import rant
from rant.types import Double2, Pose2D
node = rant.Node("talker")
pose = node.publisher("Pose", Pose2D)
pose.send(Pose2D(position=Double2(x=1.0, y=2.0), angle=0.5))
node.subscriber("Pose", Pose2D, lambda p: print(p.angle))
```

```csharp
using Rant; using Rant.Types;
using var node = new RantNode("talker");
var pose = node.Publisher<Pose2D>("Pose");
node.Subscriber<Pose2D>("Pose", p => Console.WriteLine(p.Angle));
```

```rust
use rant::types::{Double2, Pose2D};   // a program in src/bin
let node = rant::Node::new("talker", rant::NodeOptions::new())?;
let pose = node.publisher::<Pose2D>("Pose", rant::Qos::new())?;
pose.send(&Pose2D { position: Double2 { x: 1.0, y: 2.0 }, angle: 0.5 })?;
let _sub = node.subscriber::<Pose2D>("Pose", rant::Qos::new(), |p| println!("{}", p.angle))?;
```

```c
#define RANT_IMPLEMENTATION   /* in exactly one .c file */
#include "rant.h"
static void on_message(const RantMsg *m) {    /* every topic of the node lands here */
    RantPose2D p;                              /* standard types have C mirrors, wire layout */
    memcpy(&p, m->data.data, sizeof p);
}
RantAllocator mem = rant_allocator_heap(0);
RantNode *n = rant_node_open(&mem, "talker", on_message, NULL, NULL);
RantTopic *pose = rant_node_create_topic(n, "Pose", RANT_PUBSUB, rant_node_schema(n, "Pose2D"), NULL);
RantPose2D p = { { 1.0, 2.0 }, 0.5 };
rant_topic_send(pose, rant_bytes(&p, sizeof p));
```

Every handle comes from the node, named after what it is: publisher, subscriber,
variable_definition, remote_variable, function_definition, remote_function,
task_definition, remote_task (PascalCase in C#). A subscriber with no handler is pulled
with `take`, in Rust one from `pulled_subscriber`. A function or task handler returns its
answer, and in Rust a value answers Ok and an `Err` fails the call with its text.

TypeScript has the same names in the npm package `rant-middleware` each Rant release
attaches, with every pattern and reflection. In Node.js a node joins the mesh directly. A
page has no UDP sockets, so it goes through a bridge, and so does a node on another network:
the skill `bridge` (`rant install bridge`) shows both.

## Threading

Every call is thread safe. The node's threading mode decides where callbacks run:

- Service thread, the default: a thread of the node's own runs the loop and every
  callback, one at a time. Use it unless something below fits better.
- Manual: your thread calls poll and callbacks run inside it. For a node with its own loop.
- Dispatch: the service thread runs the loop, callbacks wait until your thread calls
  dispatch. For a game or UI loop, once per frame.
- A queue: one handle's callbacks wait on a queue that a thread of your choice dispatches.

Inside a callback only sends and read only queries work. Creating or closing handles is
refused there, except from a dispatch, which has the whole API.

```cpp
rant::NodeOptions o;
o.threading = rant::Threading::Manual;      // while (running) node.poll(10ms);
o.threading = rant::Threading::Dispatch;    // each frame: node.dispatch();
rant::Node node("talker", o);
rant::Queue q = node.create_queue();
rant::Qos qos; qos.queue = &q;
auto sub = node.subscriber<rant::types::Pose2D>("Pose", handler, qos);
std::thread worker([&] { while (running) q.dispatch(0, 100ms); });
```

```python
node = rant.Node("talker", threading=rant.Threading.MANUAL)     # while True: node.poll(0.01)
node = rant.Node("talker", threading=rant.Threading.DISPATCH)   # each frame: node.dispatch()
q = node.create_queue()
node.subscriber("Pose", Pose2D, handler, queue=q)               # a thread runs q.dispatch(timeout=0.1)
```

```csharp
var node = new RantNode("talker", new NodeOptions { Threading = Threading.Manual });    // node.Poll(10)
var node = new RantNode("talker", new NodeOptions { Threading = Threading.Dispatch });  // node.Dispatch()
var q = node.CreateQueue();
node.Subscriber<Pose2D>("Pose", handler, new Qos { Queue = q });                        // q.Dispatch(0, 100)
```

```rust
let node = Node::new("talker", NodeOptions::new().threading(Threading::Manual))?;    // node.poll(Duration::from_millis(10))
let node = Node::new("talker", NodeOptions::new().threading(Threading::Dispatch))?;  // node.dispatch(0, Duration::ZERO)
let q = node.create_queue()?;
let sub = node.subscriber::<Pose2D>("Pose", Qos::new().queue(&q), handler)?;        // q.dispatch(0, Duration::from_millis(100))
```

```c
rant_node_start(n);                          /* service thread, stop with rant_node_stop */
while (running) rant_node_poll(n, 10);       /* or manual, without start */
RantQueue *q = rant_node_create_queue(n);    /* a queue, given at creation: */
rant_node_create_topic(n, "Pose", RANT_SUB_ONLY, schema, &(RantTopicOpts){ .queue = q });
rant_queue_dispatch(q, 0, 100);              /* on the thread that runs them */
```

## Conventions

- Use a standard type whenever one fits: `Pose`, `Pose2D`, `Transform`, `Twist`, `Wrench`,
  `Double2/3`, `Quaternion`, `JointState`, `Odometry`, `Imu`, `LaserScan`, `PointCloud`,
  `Mesh`, `Grid`, `Image`, `File`, `Color`, `Timestamp`, `Duration`, `Empty` and the rest at
  https://docs.rantlib.dev/llms.txt. Never define your own pose, vector, cloud or map.
- Pick the entity by shape: a topic for a stream, a variable for state or a setting with
  one owner, a function for a quick question, a task for a long job with progress or cancel.
- Names are snake_case where they meet code and PascalCase everywhere else:
  - Entities and namespaces are PascalCase, `/` between levels: `FrontLidar/JointState`,
    `Line1/Conveyor`. So are types and enum options.
  - Every struct and enum has a name, the same in every language that sends or reads it.
    In schema text define it once and use the name: `Mode = enum<u8> { Idle, Run }` then
    `Arm { mode: Mode }`. An inline `{ }` or `enum` in a field is refused.
  - Fields are snake_case on the wire. Write them in the language's style (`frame_id` in
    Python and Rust, `FrameId` in C#) and Rant converts, so both are `frame_id`.
  - Nodes are snake_case, named like their file, as the skill `rant-skill` says.
- Units are SI: meters, seconds, m/s, N, radians. Time is a `Timestamp`, microseconds since
  the Unix epoch.
- Frames are right handed, and an angle turns from +x toward +y. Two kinds of 2D differ:
  - Physical 3D: z up. A body's own frame is x forward, y left, z up, as in ROS.
  - Physical 2D, such as a robot on the floor: the top view of 3D, so x right, y up, angles
    counterclockwise seen from above.
  - Image or screen: origin at the top left, x right, y down, angles clockwise on screen.
    Raw image rows start at the top.
- Keep domain, prefixes and node names out of the code: the config files and groups place
  them, and the name in the code is only a default.
- A type mismatch between two nodes is refused, not silently read, so both sides use the
  same type by name.
