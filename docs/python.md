# Python

`pip install dart-middleware` installs the `dart` package: the wrapper in `python/dart`
plus the shared library for your platform, bound through ctypes, so nothing compiles on
your machine. In a source checkout, build the CMake target `dart_shared` and put
`python/` on `sys.path`. `DART_LIBRARY` points the wrapper at any other copy of the
library. The semantics are the C ones, so docs/node.md, docs/topics.md, docs/patterns.md
and docs/tasks.md apply. This page says what is different in Python.

## A node

```python
from dataclasses import dataclass
import dart

@dataclass
class Pose:
    stamp: dart.u64 = 0
    x:     dart.f64 = 0.0
    frame: dart.string(16) = ""

node = dart.Node("robot1", on_message=lambda m: print(m.value), domain=7)
pose = dart.Topic[Pose](node, "pose", reliable=True)
node.start()
pose.send(Pose(stamp=1, x=1.0, frame="map"))
```

Everything is a constructor whose required arguments are positional and whose options are
keyword only: the node options of docs/getting-started.md on `Node`, and `role=` plus the
QoS keywords of docs/topics.md on `Topic`, `Publisher` and `Subscriber`, with
`reliable=True` for the reliable transport. Every duration option is float seconds, 0 = the default, and every
timeout is float seconds, None = forever or the default. Timestamps stay integer
microseconds. Payloads are bytes, str or typed objects. `on_message` may be None, since
subscribers and pattern handles carry their own handlers, and `on_event` defaults to
printing every event to stderr. Both are wired before the constructor returns.
`on_message(fn)` and `on_event(fn)` rebind one and return it, so they work as decorators.
The node and every handle carry `name`.

Every call is thread safe. Drive a node with `start()`, where the C service thread runs
the loop and handlers fire on it one at a time, or call `poll()` from your own loop. From
inside a handler, sends and read only queries are allowed, and poll, create, set_role,
drain, start, stop and close are refused with `SendStatus.STATE`, None or False.

`settle()` blocks until discovery and matching converge. `dispatch()` on the node drains
every queued topic on the calling thread. `close()` is refused from a handler and returns
False. A handle used after its node closed answers `NO_TOPIC`, None or False.

An `Event` carries `kind`, `error`, `peer`, `peer_name`, `topic` and `topic_name`, plus a
`details` dict holding only the fields the kind names, as in docs/node.md. `str(event)` is
the one line text.

## Schemas

Any class with annotated fields is a schema, no decorator needed, and `@dataclass` just
gives a constructor. Field types are `dart.u8` to `dart.f64`,
`dart.string(cap)` for a capped string, `dart.<scalar>[n]` or `dart.string(cap)[n]` for a
fixed array, a nested annotated class, plain `int`, `float` and `bool`, `str` for an
unbounded string, `list[...]` for a variable array, `dict` for a map, and an `IntEnum` or
`dart.enum(cls, backing)` for a named integer. `__dart_name__` on the class overrides the
wire type name. `dart.dsl(source)` gives the DSL text of a class, a bare type, a compiled
`Schema` or DSL text, with no library load, for display or for pasting into a C node.

A bare type is a schema of its own: `dart.Topic[bool](node, "estop")` sends and
receives plain booleans, and so do the scalars, `dart.string(N)`, arrays, `list[dart.f32]`,
`str`, `dict`, an enum class and the plain Python scalars. Such a root is anonymous, so the
same one in any language is the same wire bytes and the same hash.

The package ships type stubs, so a checker sees `Publisher[Pose].send` take a `Pose` and
`Subscriber[Pose].take()` return `Pose | None`. The scalars are `int` and `float` aliases
to a checker. A capped string, a fixed or variable array and a pinned enum width are
spelled `Annotated[T, "<dsl field type>"]`, the Python type for the checker and the DSL for
the wire: `Annotated[str, "string<16>"]`, `Annotated[bytes, "u8[4]"]`,
`Annotated[list[float], "f32[]"]`, `Annotated[Mode, "u8"]`. In a value position the same
DSL text does it: `dart.Schema("u8[4]")`.

`Schema(text)`, `Schema(cls)` and `Schema(bare_type)` compile explicitly. `encode` and
`decode` walk the compiled flat field table, so they work for any schema including a
peer's. `fields()` lists `Schema.Field` records whose kinds are `Schema.FieldType`. A
missing value keeps the zeroed default. The map body is built and parsed in Python and
validated by the C setter. The standard types in docs/stdtypes.md live in `dart.types` as
tagged dataclasses and aliases: `dart.types.Transform`, `dart.types.Color`,
`dart.types.Timestamp` and the rest, and `dart.types.now()` is the Timestamp clock.

## Messages and queues

A `Message` is copied out, so it outlives the callback. `value` is the decoded object,
None on a raw topic, and `data` the wire bytes. `recv_us` is the node's monotonic clock at
receipt and `written_us` the writer's wall clock, 0 when the publisher opted out, as in
docs/node.md.

`take(timeout)` and `dispatch(max_msgs, timeout)` switch a topic to queued
delivery. `take` returns None when nothing arrived, and dispatch handlers run on the
calling thread without the node lock. `stats.queue()` and `stats.traffic()` on a topic
return the queue and traffic counters as tuples.

`retire()` releases a topic's name for a re creation with another schema. The handle is
unusable after and every call returns NO_TOPIC. `ready()` is true when a send would not
wait on the match wait, and `pending_count()` counts unresolved candidates. `Publisher` and
`Subscriber` carry `match_count()` and `ready()` too.

## Functions, tasks and variables

`FunctionDefinition[Req, Rsp](node, name, handler)`, `RemoteFunction[Req, Rsp](node, name)`,
`TaskDefinition[Req, Prg, Rsp]`, `RemoteTask[Req, Prg, Rsp]`, `VariableDefinition[T]`,
`RemoteVariable[T]`, `Publisher[T]` and `Subscriber[T]` are the typed forms. The untyped
forms take explicit schema arguments, None for raw bytes. `match_count()` on any handle
counts the other side: callers on a definition, providers on a remote, remotes on a
variable definition, owners on a remote variable.

Handlers are arity dispatched. A one argument function handler returns the reply and is
acknowledged OK, and raising answers APP_ERROR with the exception text. The two argument
form receives a `Request` and replies, fails or defers explicitly. A `Request` is valid
only while the handler runs, and its verbs raise RuntimeError after. `defer()` returns a
`Deferred` completed once from any thread. A None handler answers NO_HANDLER.

A task handler runs on a dedicated daemon thread per call and receives a `TaskRequest`.
Returning completes OK with the return value, raising `dart.CancelledError` completes
CANCELLED, and any other exception completes APP_ERROR. `progress(x)` streams updates and
`cancelled` or the `cancel_event` observe a cooperative cancel. Retiring a definition
answers every live call CANCELLED, and a worker completing after that is refused silently.

A blocking `call()` drives the loop and is refused from a callback or under a service
thread, with `send_status` STATE and `status` TIMEOUT. It never raises on a failed call:
inspect `status`, and reading `value` raises `CallError` when the call did not complete
OK. `call_async()` returns the launch status, or on a task the call id for `cancel()`, and
fires `on_response` exactly once from the polling thread. An `on_progress` handler takes
the value, None for the RUNNING ack, or the value and a `Progress`.

Variables are methods only, so every write returns its status. `set()` answers NO_TOPIC
when no owner matched and BAD_ROLE when the owner has no set channel. `force()` needs
`allow_force` on the definition. `wait()` blocks until a value exists. `on_change` replays
the current value at registration and fires on every state change, `on_write` fires on
every applied write, both inline on the thread that applied the write.

## Logs and meta

`node.log(level, text)`, or `node.log.error(text)`, `warn` and `info`, publishes a
formatted line, truncated at DART_LOG_MAX. `node.log.on(level, handler)` delivers every
other node's lines at that level as a `LogLine`, and `node.log.topic(level)` is the topic
behind a level. `meta(peer, sections)` blocks and must not run under `start()` or from a
callback, and `meta_async` works anywhere. Both decode into a `MetaSnapshot` whose node and
proc scalars are fields and whose full body is `info`. `node.stats.memory()`,
`backpressure()` and `evicted_unsent()` are the node's own counters.
