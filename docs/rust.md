# Rust

The crate `rant-middleware`, used as `rant`, compiles the C library into your program, so
nothing else installs. It works the same as C, so docs/node.md, docs/topics.md,
docs/patterns.md and docs/tasks.md apply. This page covers what is different in Rust.

## Setup

```toml
[dependencies]
rant-middleware = { git = "https://github.com/KosmosisDire/Rant.git", tag = "crate-v0.0.27" }
```

- Every release tags the crate `crate-v<version>` with the C sources packed in, so a build
  needs only cargo and a C compiler.
- `rant lib install` adds the line for you.
- Rust 1.85 or newer.

## Node

```rust
let node = Node::new("robot", NodeOptions::new().domain(7))?;
```

- Options are setters on the options value. Every option left out takes its default.
  `NodeOptions` mirrors docs/getting-started.md's node options, `prefix` included.
- `on_event(handler)` gets peer, loss and error events. With no handler, errors print to
  stderr.
- `settle(None)` waits until discovery is done. Call it after creating your handles.
- `close()` or drop shuts the node down. It first waits until the calls other threads are
  making on the node return. Inside one of the node's own callbacks it runs once that
  callback returns.
- `rant::version()` is the library's version, such as `"0.0.27"`.

Durations are `std::time::Duration`. `rant::FOREVER` waits with no end.

### Threading

`NodeOptions::threading` picks where callbacks run.

| Mode | Callbacks run |
|---|---|
| `ServiceThread` (default) | on the node's own thread |
| `Manual` | inside your `node.poll(timeout)` |
| `Dispatch` | inside your `node.dispatch(0, timeout)`, for a game or UI loop |

To run one handle's callbacks on a thread of your own, pass it a queue from
`node.create_queue()` in its options and call `queue.dispatch()` on that thread.

### Errors

- A failed node or handle creation returns `rant::Error`, whose text names the call and
  the reason.
- Sends and sets return `Result<(), SendError>`.
- A panicking callback never breaks the node. The panic goes to stderr and `on_event`. A
  function or task handler that panics answers the caller `AppError`.

## Types

```rust
#[derive(rant::Message)]
struct Conveyor { speed: f64, running: bool, part: rant::String<16> }
```

Supported fields:

- numbers and `bool`
- `rant::String<N>` (a string of at most N bytes) and `String`
- `[T; N]` and `Vec<T>`
- other `rant::Message` structs
- enums with `#[derive(rant::Enum)]` and an integer `#[repr]`

Not supported: `Option`, maps, references, and arrays of `String`, of enums or of arrays.

- Field names are the wire names, and `#[rant(name = "Other")]` renames the type.
- A plain type works without a derive: `publisher::<f32>`.
- A value that does not fit its type, such as a `rant::String<N>` longer than N, is
  refused, never cut short.
- The standard types of docs/stdtypes.md are in `rant::types`: `Pose`, `Transform`,
  `Empty` and the rest.

### Raw messages

Use `rant::Bytes` as the type to work with schemas at run time:

```rust
let s = node.schema("Chat { seq: u32, text: string }")?;
let chat = node.publisher::<Bytes>("chat", Qos::new().schema(&s))?;
let mut m = MessageBuilder::new(&s);
m.set_uint("seq", 1)?.set_string("text", "hi")?;
chat.send(m.bytes())?;
```

A received `MessageView` reads fields by name, such as `get_uint("seq")`. It is valid
inside the callback only.

## Topics

```rust
let conveyor = node.publisher::<Conveyor>("line1/conveyor", Qos::new().reliable(true))?;
let _watch = node.subscriber::<Conveyor>("line1/conveyor", Qos::new(), |c| println!("{}", c.speed))?;
conveyor.send(&Conveyor { speed: 0.5, running: true, part: "A7".parse()? })?;
```

- The handler gets a `Received`, which reads as the value and also carries
  `publisher_name()` and the timestamps.
- `pulled_subscriber` has no handler: `take(timeout)` returns the oldest waiting message,
  `take_latest(timeout)` the newest.
- `match_count()`, `ready()` and `drain(timeout)` on a publisher, as in docs/topics.md.

## Functions

```rust
let _limit = node.function_definition("line1/set_speed", FunctionOptions::new(), |req: Request<f64, f64>| {
    if *req > 2.0 { Err("over the limit") } else { Ok(*req) }
})?;

let set_speed = node.remote_function::<f64, f64>("line1/set_speed", FunctionOptions::new())?;
let applied = set_speed.call(&1.5, CallOptions::new())?.into_result()?;
let applied = set_speed.call_async(&1.5, CallOptions::new())?.await.into_result()?;
```

- The handler's return value is the answer: a value answers Ok, an `Err` answers AppError
  with its text.
- `call` blocks. An `Err` means the request never left, such as `State` inside an inline
  callback, where `call_async` is the tool.
- `call_async` returns a Future any executor can await. No runtime crate is needed.
- A `Response` holds `status()`, `message()` and `value()`. `into_result()` gives the
  value of an Ok call, else a `CallError` with the status and the message.
- `CallOptions::new().timeout(t).provider(peer)` bounds one call or aims it at one peer.
- To answer later, call `req.defer()` and return `()`. The `Deferred` answers from any
  thread, and dropping it answers AppError.

## Tasks

A task is a function that reports progress and can be cancelled.

```rust
let _arm = node.task_definition("arm/move", TaskOptions::new(), |req: TaskRequest<Pose, f32, Pose>| {
    let goal = req.clone();
    let task = req.defer();
    std::thread::spawn(move || {
        for i in 1..=10 {
            if task.cancelled() {
                return task.complete_cancelled("stopped", None);
            }
            task.progress(&(i as f32 / 10.0))?;
        }
        task.complete(&goal)
    });
})?;

let arm = node.remote_task::<Pose, f32, Pose>("arm/move", TaskOptions::new())?;
let r = arm.call(&goal, CallOptions::new(), |p| {
    if let Some(share) = p.value() { println!("{share}") }
})?;
```

- A `PendingTask` sends `progress()`, then one of `complete()`, `fail()` or
  `complete_cancelled()`, which consume it. Dropping it answers AppError.
- The first progress update has no value: the work started. It carries `call_id()` for
  `cancel(id)` from another thread.
- `call_async` returns a `TaskCall` Future. Dropping it before the outcome cancels the
  task, and `detach()` keeps it going.

## Variables

```rust
let speed = node.variable_definition::<f64>("line1/speed", VariableOptions::new().initial(1.5))?;

let remote = node.remote_variable::<f64>("line1/speed", VariableOptions::new())?;
remote.on_change(|v| println!("{}", *v))?;
remote.set(&2.0)?;
```

- `get()` reads locally, `Ok(None)` while no value exists.
- `on_change` fires once with the current value, then on every change. `on_write` fires on
  every write, even one with the same value.
- `wait(timeout)` blocks until a value exists. `force`, `unforce` and `forced` as in
  docs/patterns.md.

## Names and schemas

- Every handle has `name()`.
- `schema()` on topics and variables, and `request_schema()`, `response_schema()` and
  `progress_schema()` on functions and tasks, give the schema in use.
- A `rant::Bytes` handle with `reflect_from_mesh` set takes its schema from the peers.
  `refresh()` updates it when they change.
- `schema.view(&bytes)` reads a raw response or value by field name.

## Lifetime

- Handles hold the node state, so they can live beside the node in one struct.
- Drop or `close()` releases the name. Closing a remote cancels its open calls.
- A handle cannot close inside its own callback. It then stays until the node closes.
- A handle that outlives its node is safe. Its calls answer `NoTopic`.

## Reflection and logs

```rust
node.log(LogLevel::Info, "started")?;
node.on_log(|line| println!("{}: {}", line.node, line.text))?;

for peer in node.reflection().peers() {
    println!("{}", peer.name);
}
```

- `on_log` receives every other node's log lines.
- `reflection()` lists `peers()`, one peer's `entities(id)` and the whole `mesh()`.
  `epoch()` changes when anything does.
- `meta(peer, sections, timeout)` asks a peer for its runtime snapshot, and `meta_async`
  returns a Future of it.
- `node.stats()` shows memory use and send counters.

## Static memory

`NodeOptions::new().memory(4 << 20)` runs the node in one block of that many bytes, with no
growth and no shared memory. The open fails with `Oom` when the block is too small.
