# Python

`pip install rant-middleware` installs the `rant` package: the wrapper in `bindings/python/rant`
plus the shared library for your platform, bound through ctypes, so nothing compiles on
your machine. In a source checkout, the CMake target `python_install` builds the wheel
and installs it, or build `rant_shared` and put `bindings/python/` on `sys.path`. `RANT_LIBRARY` points the wrapper at any other copy of the
library. The semantics are the C ones, so docs/node.md, docs/topics.md, docs/patterns.md
and docs/tasks.md apply. This page says what is different in Python.

## A node

```python
from dataclasses import dataclass
import rant

@dataclass
class Conveyor:
    speed:   rant.f64 = 0.0
    running: bool = False
    part:    rant.string(16) = ""

node = rant.Node("robot1", domain=7)
belt = node.publisher("line1/conveyor", Conveyor, reliable=True)
belt.send(Conveyor(speed=0.5, running=True, part="bracket"))
node.subscriber("line1/conveyor", Conveyor, lambda c: print(c))
```

`rant.Node(name, **options)` opens a node, the name optional. The options are
docs/getting-started.md's as keywords: `domain`, `prefix`, `multicast_interface`, `max_topics`,
`match_wait`, `fetch_details`, the discovery options of docs/discovery.md (`seed_peers` as
"ip" or "ip:port" strings, `unicast_only`, `self_ip` with `advertise_port`) and the rest.
`on_event` receives every peer, loss and error event, and `node.on_event(handler)` sets or
replaces it after open. With none set, error events print to stderr.
Every duration option is float seconds, 0 = the default, and every timeout is float
seconds, None = forever or the default. Timestamps stay integer microseconds.

Every handle comes from a method on the node named after it: `publisher`, `subscriber`,
`function_definition`, `remote_function`, `task_definition`, `remote_task`,
`variable_definition` and `remote_variable`. The name comes first, the schema second, a
handler where one belongs, and the options of the C opts as keywords. The node and every
handle carry `name`, and a typed handle carries `schema`.

Every call is thread safe. The C service thread runs from construction and handlers fire
on it one at a time. `threading=rant.Threading.MANUAL` leaves the loop to your own thread
instead: call `poll()` from it and handlers fire there. A build without threads refuses
the service thread with `rant.Error`, and MANUAL is the way in.
`threading=rant.Threading.DISPATCH` runs the service thread but parks every handler,
response, log line and event on the node's queue until your thread calls `node.dispatch()`,
the one liner for a frame paced app. `node.create_queue()` makes another `rant.Queue`,
and a handle created with `queue=` set to it parks its callbacks there alone until
`queue.dispatch()`. Same name topic handles must agree on the queue. From inside a handler,
sends and read only queries are allowed, and poll, handle creation, close, drain and
`settle` are refused with `SendStatus.STATE`, None or False.

`settle()` blocks until discovery and matching converge. `close()` is refused from a handler and returns
False. A node also closes at the end of a `with` block, when its last reference goes (a
handle holds one) and at interpreter exit. A handle used after its node closed answers
`NO_TOPIC`, None or False.

A construction failure raises `rant.Error` with the C reason in the message and the
`Event` behind it in `.event`. `SchemaError` and `CallError` derive from it. `stats` reads
the node's counters as one `NodeStats`, and `last_error` is the most recent error `Event`.
An `Event` carries `kind`, `error`, `peer`, `peer_name`, `topic` and `topic_name`, plus the
C event's other fields flat, zero or None where the kind does not set them, as in
docs/node.md. `str(event)` is the one line text. `rant.version()` is the native library's
version, such as "0.0.17".

## Schemas

Any class with annotated fields is a schema, no decorator needed, and `@dataclass` just
gives a constructor. Field types are `rant.u8` to `rant.f64`,
`rant.string(cap)` for a capped string, `rant.<scalar>[n]` or `rant.string(cap)[n]` for a
fixed array, a nested annotated class, plain `int`, `float` and `bool`, `str` for an
unbounded string, `list[...]` for a variable array, `dict` for a map, and an `IntEnum` or
`rant.enum(cls, backing)` for a named integer. A member's wire name is its own name in
the wire spelling, camelCase (docs/stdtypes.md): `frame_id` is `frameId`, the same field
a C# `FrameId` names, and it decodes back into `frame_id`. A decoded dictionary carries
the wire names and a dictionary source may spell its keys any way. `__rant_name__` on the
class overrides the wire type name. `rant.dsl(source)` gives the DSL text of a class, a bare type, a compiled
`Schema` or DSL text, with no library load, for display or for pasting into a C node.

A nested class, alone or as `list[Cls]` for an array of structs, takes its class name on
the wire: the reflected text defines it once above its first use, as
`Code { type: string<16>, angle: f32 }`, and each use is the name, so `codes: list[Code]`
is `codes: Code[]` and a C# or C peer spells `Code` too. A name has one shape per node, so
a class called `Twist` with fields of its own is refused. A standard type spells by its
name alone (`list[rant.types.Float3]` is `Float3[]`). An element is fixed all the way down, so a `str`, a `list` or a `dict` inside
one is refused at compile, and so is an array of them. A struct array inside one must be
fixed, `Annotated[list[Float2], "Float2[4]"]`, and nests to any depth. `encode` takes a list of instances
or of dicts and `decode` gives one back, typed when the schema came from a class. DSL text
also declares the fixed form, `{ x: f32, y: f32 }[3]`, which zero fills to its count.

A bare type is a schema of its own: `node.publisher("estop", bool)` sends plain booleans,
and so do the scalars, `rant.string(N)`, arrays, `list[rant.f32]`, `str`, `dict`, an enum
class and the plain Python scalars. Such a root is anonymous, so the same one in any
language is the same wire bytes and the same hash.

The schema argument of every handle is a schema class, a bare type, a compiled `Schema` or
DSL text, and None makes a raw handle whose payloads are bytes or str. The package ships
type stubs, so a checker sees `node.publisher("line1/conveyor", Conveyor).send` take a
`Conveyor` and the handler of `node.subscriber("line1/conveyor", Conveyor, handler)` take
one, and `rant.Publisher[Conveyor]` is the annotation for one. The scalars are `int` and
`float` aliases to a checker. A capped string, a fixed or variable array and a pinned
enum width are spelled
`Annotated[T, "<dsl field type>"]`, the Python type for the checker and the DSL for the
wire: `Annotated[str, "string<16>"]`, `Annotated[bytes, "u8[4]"]`,
`Annotated[list[float], "f32[]"]`, `Annotated[Mode, "u8"]`. The text may name a struct
over a list of that class, so `Annotated[list[Float2], "Float2[4]"]` is a fixed array of
four `Float2`, which a plain `list[Float2]` cannot say. In a value position the same
DSL text does it: `node.schema("u8[4]")`.

`node.schema(text)`, `node.schema(cls)` and `node.schema(bare_type)` compile explicitly,
in the node's registry: every definition stays in scope for the node's later compiles, and
the node owns the result for its life. `encode` and
`decode` walk the compiled flat field table, so they work for any schema including a
peer's. `fields` lists `Schema.Field` records whose kinds are `Schema.FieldType`, and
`enum_variants(field)` the options of an enum field, for a tool that renders a schema it
has never seen. `can_read(pub)` is the subset test, `hash` the wire identity. A missing
value keeps the zeroed default. The map body is built and parsed in Python and validated
by the C setter. The standard types in docs/stdtypes.md live in `rant.types` as tagged
dataclasses and aliases: `rant.types.Transform`, `rant.types.Color`,
`rant.types.Timestamp` and the rest, and `rant.types.now()` is the Timestamp clock.

## Topics

`node.publisher(name, schema=None, **options)` and `node.subscriber(name, schema=None,
handler=None, **options)` take the topic options of docs/topics.md as keywords:
`reliable=True` for the reliable transport, `keep_last`, `catch_up`, `max_message_bytes`,
`heartbeat`, `repair_delay`, `backpressure_wait`, `shm_max_bytes`, `queue_bytes`,
`max_rate_hz`, `no_timestamp`, `reflect_from_mesh` and `queue`.

A subscriber's handler takes the decoded value, or the value and the `Message`, and runs on
the polling thread. A `Message` is copied out, so it outlives the callback. `value` is the
decoded object, None on a raw topic, and `data` the wire bytes. `recv_us` is the node's
monotonic clock at receipt and `written_us` the writer's wall clock, 0 when the publisher
opted out, as in docs/node.md. A subscriber created without a handler is pulled instead:
`take(timeout)` returns the oldest waiting message and `latest(timeout)` the newest,
dropping the older ones, each as a one argument handler would get it, or None. Such a
subscriber takes no `queue`. `queue_stats` on a subscriber and `counts` on either side are
the ring and traffic counters.

```python
frames = node.subscriber("camera/image", rant.types.Image)
img = frames.latest()          # newest frame since the last call, or None
```

Same name handles on one node share the topic slot: the node advertises the roles the live
handles hold, `close()` on one leaves its siblings working, and the last close retires the
slot so the name can carry another schema. A different schema on a live name raises
`rant.Error`. Every handle has `close()`, which returns False when refused from a callback,
and works as a context manager. `ready` on a publisher is true when a send would not wait,
`match_count` counts matched subscribers, and `drain(timeout)` pumps until every reader
has acked. A subscriber has no match count, since the C exposes no publisher count on the
subscribing side. `reflect_from_mesh=True` lets a handle with no schema adopt the mesh's,
and `refresh()` re types it in place when the mesh moved, as in docs/reflection.md. Every
handle reports `name` and the schema it uses now, the adopted one on a reflect handle:
`schema` on a topic or a variable, `request_schema`, `response_schema` and, on a task,
`progress_schema` on a function or task handle, None while untyped.

## Functions, tasks and variables

`node.function_definition(name, handler, req_schema=None, rsp_schema=None, **options)`,
`node.remote_function(name, req_schema=None, rsp_schema=None, **options)`,
`node.task_definition(name, handler, req_schema=None, prg_schema=None, rsp_schema=None,
**options)`, `node.remote_task(...)`, `node.variable_definition(name, schema=None,
**options)` and `node.remote_variable(name, schema=None, **options)` are the pattern
handles. The options are the C opts as keywords: `timeout`, `backpressure_wait`,
`keep_last`, `multi` and `reflect_from_mesh` on functions, plus `progress_best_effort`,
`progress_keep_last`, `no_cancel` and `exclusive` on tasks, and `initial`, `read_only`,
`allow_force`, `catch_up`, `keep_last`, `backpressure_wait` and `reflect_from_mesh` on
variables. `match_count` on any handle counts the other side: callers on a definition,
providers on a remote, remotes on a variable definition, owners on a remote variable.
`close()` retires the handle and answers every outstanding call CANCELLED.

Handlers are arity dispatched. A one argument function handler returns the reply and is
acknowledged OK, and raising answers APP_ERROR with the exception text. The two argument
form receives a `Request` and replies, fails or defers explicitly. A `Request` is valid
only while the handler runs, and its verbs raise RuntimeError after. `defer()` returns a
`Deferred` completed once from any thread. A definition without a handler raises
ValueError.

A task handler runs on a dedicated daemon thread per call and receives the request value,
or the value and a `TaskContext`. Returning completes OK with the return value, raising
`rant.CancelledError(message, partial)` completes CANCELLED with an optional partial result,
and any other exception completes APP_ERROR.
`progress(x)` on the context streams updates and `cancelled` or the `cancel_event` observe
a cooperative cancel. Closing a definition answers every live call CANCELLED, and a worker
completing after that is refused silently.

A blocking `call()` waits on the service thread's progress, drives the loop of a MANUAL
node, and is refused from a callback. It never raises on a failed call: inspect `status`.
`value` reads whatever payload came back, a partial result or failure data too, and
`has_value` says whether one did. `value` raises `CallError` only when the call did not
complete OK and nothing came back, or when the payload did not decode. A message, variable
update or progress update that does not decode raises an ERROR event of kind
SCHEMA_MISMATCH naming the topic, and the handler does not run. `take()` and a variable's
`get()` raise `SchemaError` instead. `call_async()` returns the launch status, or on a task the call id for `cancel()`, and
fires `on_response` exactly once from the polling thread. An `on_progress` handler takes
the value, None for the RUNNING ack, or the value and a `Progress`.

`VariableDefinition` and `RemoteVariable` share the `Variable` surface. Reads are local
and status free: `get()` returns the copied out value, None while none exists. Every write
returns its status: `set()` answers NO_TOPIC when no owner matched and BAD_ROLE when the
owner has no set channel, `force()` needs `allow_force` on the definition, STATE on the
owner without it and BAD_ROLE on a remote whose owner advertises none. `wait()` blocks
until a value exists and `forced` says whether a force is in effect. `on_change(handler)`
replays the current value at registration and fires on every state change, `on_write` fires
on every applied write, both inline on the thread that applied the write, with the value or
the value and a `VariableUpdate`. A later call rebinds and None clears.

## Reflection

The walks of docs/reflection.md live on `node.reflection` and return copied snapshots, so
they outlive the poll and need no lock. `peers()` lists every discovered peer as a `Peer`,
dropped ones included, so gate on `active`. `entities(peer)` lists what one node offers,
peer 0 for this one, and `mesh()` folds the whole mesh into one `Entity` per kind and name.
`find(kind, name)` returns one or None, and `epoch` bumps whenever the folded view changed.
`meta(peer, sections)` blocks and must not run from a callback, `meta_async` works
anywhere, and both decode a peer's snapshot into a `MetaSnapshot` whose node and proc
scalars are fields and whose full body is `info`. The schemas on an `Entity` are owned
copies, None when untyped or not fetched, and `fetch_details` on the node is what makes
them arrive.

## Logs

`node.log(level, text)` publishes a line at a `LogLevel`, truncated at RANT_LOG_MAX, and
`node.on_log(handler)` delivers every other node's lines at every level as a `LogLine`
carrying its `level`, at `dispatch()` under DISPATCH. It raises `rant.Error` on a node
opened with `disable_logs`.
