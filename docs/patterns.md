# Functions, tasks and variables

Three interaction patterns built over topics. Strip them with `DART_NO_PATTERNS`. A
DEFINITION is where the body or the storage lives. A REMOTE is a reference to a definition
on another node. One node cannot be both sides of the same entity.

| pattern | shape | create |
|---|---|---|
| function | one request, exactly one response | `dart_node_create_function_definition` / `dart_node_create_remote_function` |
| task | one request, N progress updates, one response, cancellable | `dart_node_create_task_definition` / `dart_node_create_remote_task` |
| variable | replicated state with one owner | `dart_node_create_variable_definition` / `dart_node_create_remote_variable` |

A function, task or variable never cross wires with a plain topic or with each other under
the same name. Two definitions of the same entity on different nodes fire
`DART_E_DUPLICATE_AUTHORITY` on both, as a warning. Declare that on purpose with `.multi`.

## Functions

```c
static void on_add(DartRequest *req, void *user){
    int32_t sum = add(req->data);                  /* decode req->data with req->schema */
    dart_request_reply(req, dart_bytes(&sum, sizeof sum));
}
DartFunction *def = dart_node_create_function_definition(n, "add", req_schema, rsp_schema,
                        on_add, NULL, NULL);
DartFunction *fn  = dart_node_create_remote_function(n, "add", req_schema, rsp_schema, NULL);

DartResponse rsp;
if (dart_function_call(fn, req, &rsp, 1000, NULL) == 1 && rsp.status == DART_CALL_OK) ...
dart_function_call_async(fn, req, on_response, user, NULL);
```

The handler gets a `DartRequest`: `data`, `schema`, `caller`, `caller_name`,
`function_name`, `recv_us` and `written_us`. It answers with `dart_request_reply` (OK),
`dart_request_fail(req, message, rsp)` (APP_ERROR with text), or `dart_request_defer(req)`,
which returns a token for `dart_function_complete(fn, token, status, message, rsp)` later
from any thread. A handler that returns without answering auto acks OK. Pass only the
exact request pointer the callback received, never a copy.

The caller's `DartResponse` carries `status`, `data`, `schema`, `provider` (the answering
peer), `message` (human readable outcome text, always displayable) and `written_us`.
Statuses are `DART_CALL_OK`, `APP_ERROR`, `NO_HANDLER`, `TIMEOUT`, `PEER_LOST`,
`CANCELLED` and, for tasks, `RUNNING`.

`DartFunctionOpts`: `timeout_us`, `backpressure_wait_us`, `keep_last` (request and
response history depth, default 10), `reflect_from_mesh` and `multi`. Raise `keep_last`
above the biggest batch of requests one poll pass can deliver, on both sides, or replies
past the depth are lost and the callers see `DART_CALL_TIMEOUT`.

`DartCallOpts`: `provider` (direct the call at one peer, 0 = every definition and the
first answer wins), `on_progress` and `progress_user` (tasks), and `id_out` (the call id,
for cancel).

`dart_function_call` drives the node loop itself and returns 1 when answered, 0 on a
timeout and a negative `DartResult` on error, so it is refused with `DART_ERR_STATE` from
a callback or under a service thread, where `dart_function_call_async` is the tool. On a
task the timeout bounds only the wait for the first response, then it waits for the
terminal outcome and `dart_function_cancel` from another thread is the way out.

A call from a fresh remote waits for its match like a first topic send (docs/topics.md).

## Tasks

A task is a function with progress and cancellation on the same `DartFunction` handle.
docs/tasks.md has the full model, the handler forms and the wrappers.

## Variables

```c
DartVariable *owner  = dart_node_create_variable_definition(n, "speed", schema,
                           &(DartVariableOpts){ .initial = dart_bytes(&v, sizeof v) });
DartVariable *remote = dart_node_create_remote_variable(n, "speed", schema, NULL);
dart_variable_set(remote, value);          /* pushed to the owner, no response */
dart_variable_get(remote, &bytes);         /* the latest replicated value */
dart_variable_wait(remote, 1000);          /* until the first value arrives */
```

The definition owns the value. Remotes write with `dart_variable_set` and a late remote
gets the latest value at once. `DartVariableOpts`: `initial`, `access`
(`DART_VAR_READONLY` creates no set channel, so remote sets get `DART_ERR_ROLE`),
`allow_force`, `catch_up` (default 1), `keep_last` (the repair window for a burst of
writes, default 10), `backpressure_wait_us` and `reflect_from_mesh`.

`dart_variable_force` overrides the value until `dart_variable_unforce`. Writes while
forced are absorbed, and unforce restores the latest. `dart_variable_forced` reports it.

Events, one slot each, `NULL` clears: `dart_variable_on_change` fires when the observed
state actually changes (first value, different bytes, a forced flip) and replays the
current value once at registration. `dart_variable_on_write` fires on every applied write.
Both get a `DartVariableUpdate` (value, schema, forced, write_seq, source peer with 0 =
local, recv_us, written_us) inline on the thread that applied the write, with the usual
callback restrictions.

A remote's first write waits for the owner match like a first topic send, so
`DART_ERR_NO_TOPIC` means the owner is really absent.

## Retire

`dart_function_retire` and `dart_variable_retire` park the entity's channels, answer every
outstanding call CANCELLED, silence callbacks and free the handle. The handle is invalid
after. From inside a callback the call is refused with `DART_ERR_STATE` and the handle
stays valid. A re created entity with the same name takes its old slots back. Do not
create a second same name handle while the first lives: it is silently shadowed and
receives nothing.
Complete or abandon outstanding defer tokens before retiring a definition.

## Wrappers

C++, C#, Python and the JS bridge expose all three patterns. A thrown handler in C#,
Python or JS answers APP_ERROR with the exception text. C++ `defer()` returns a movable
`PendingTask`, C# handlers are async with a real `CancellationToken`, JS handlers get
`ctx.progress` and an `AbortSignal`. Only Python spawns threads (one daemon thread per
running call).
