# Functions, tasks and variables

Three interaction patterns built over topics. Strip them with `RANT_NO_PATTERNS`. A
DEFINITION is where the body or the storage lives. A REMOTE is a reference to a definition
on another node. One node cannot be both sides of the same entity.

| pattern | shape | create |
|---|---|---|
| function | one request, exactly one response | `rant_node_create_function_definition` / `rant_node_create_remote_function` |
| task | one request, N progress updates, one response, cancellable | `rant_node_create_task_definition` / `rant_node_create_remote_task` |
| variable | replicated state with one owner | `rant_node_create_variable_definition` / `rant_node_create_remote_variable` |

A function, task or variable never cross wires with a plain topic or with each other under
the same name. Two definitions of the same entity on different nodes fire
`RANT_E_DUPLICATE_AUTHORITY` on both, as a warning. Declare that on purpose with `.multi`.

## Functions

```c
static void on_add(RantRequest *req, void *user){
    int32_t sum = add(req->data);                  /* decode req->data with req->schema */
    rant_request_reply(req, rant_bytes(&sum, sizeof sum));
}
RantFunction *def = rant_node_create_function_definition(n, "add", req_schema, rsp_schema,
                        on_add, NULL, NULL);
RantFunction *fn    = rant_node_create_remote_function(n, "add", req_schema, rsp_schema, NULL);

RantResponse rsp;
if (rant_function_call(fn, req, &rsp, 1000, NULL) == 1 && rsp.status == RANT_CALL_OK) ...
rant_function_call_async(fn, req, on_response, user, NULL);
```

The handler gets a `RantRequest`: `data`, `schema`, `caller`, `caller_name`,
`function_name`, `recv_us` and `written_us`. It answers with `rant_request_reply` (OK),
`rant_request_fail(req, message, rsp)` (APP_ERROR with text), or `rant_request_defer(req)`,
which returns a token for `rant_function_complete(fn, token, status, message, rsp)` later
from any thread. A handler that returns without answering auto acks OK. Pass only the
exact request pointer the callback received, never a copy.

Every callback of a handle, the handler, `on_response`, `on_progress`, `on_cancel`,
`on_change` and `on_write`, runs inline on the node loop's thread unless the handle was
created with `queue` in its options. Then it runs at `rant_queue_dispatch` on the draining
thread with the whole API available, see docs/node.md. State still applies at receipt: a
variable's value is current before its `on_change` runs, and the blocking
`rant_function_call` completes without a dispatch.

The caller's `RantResponse` carries `status`, `data`, `schema`, `provider` (the answering
peer), `message` (human readable outcome text, always displayable) and `written_us`.
Statuses are `RANT_CALL_OK`, `APP_ERROR`, `NO_HANDLER`, `TIMEOUT`, `PEER_LOST`,
`NO_PROVIDER` (the timeout passed and no definition ever matched, so the request never
left this node), `CANCELLED` and, for tasks, `RUNNING`.

`RantFunctionOpts`: `timeout_us`, `backpressure_wait_us`, `keep_last` (request and
response history depth, default 10), `reflect_from_mesh` and `multi`. Raise `keep_last`
above the biggest batch of requests one poll pass can deliver, on both sides, or replies
past the depth are lost and the callers see `RANT_CALL_TIMEOUT`.

`RantCallOpts`: `provider` (direct the call at one peer, 0 = every definition and the
first answer wins), `on_progress` and `progress_user` (tasks), and `id_out` (the call id,
for cancel).

`rant_function_call` blocks and returns 1 with an outcome in `out` (a synthesized
`NO_PROVIDER` or `PEER_LOST` included), 0 on its own timeout and a negative `RantResult`
on error. Under `rant_node_start` it sleeps on the service thread's progress,
otherwise it drives the node loop itself. From a callback it is refused with
`RANT_ERR_STATE`, and `rant_function_call_async` is the tool there. On a task the timeout
bounds only the wait for the first response, then it waits for the terminal outcome and
`rant_function_cancel` from another thread is the way out.

A call from a fresh remote waits for its match like a first topic send (docs/topics.md).

## Tasks

A task is a function with progress and cancellation on the same `RantFunction` handle.
docs/tasks.md has the full model, the handler forms and the wrappers.

## Variables

```c
RantVariable *owner    = rant_node_create_variable_definition(n, "speed", schema,
                           &(RantVariableOpts){ .initial = rant_bytes(&v, sizeof v) });
RantVariable *remote = rant_node_create_remote_variable(n, "speed", schema, NULL);
rant_variable_set(remote, value);            /* pushed to the owner, no response */
rant_variable_get(remote, &bytes);           /* the latest replicated value */
rant_variable_wait(remote, 1000);            /* until the first value arrives */
```

The definition owns the value. Remotes write with `rant_variable_set` and a late remote
gets the latest value at once. `RantVariableOpts`: `initial`, `access`
(`RANT_VAR_READONLY` creates no set channel, so remote sets get `RANT_ERR_ROLE`),
`allow_force`, `catch_up` (default 1), `keep_last` (the repair window for a burst of
writes, default 10), `backpressure_wait_us` and `reflect_from_mesh`.

`rant_variable_force` overrides the value until `rant_variable_unforce`. Writes while
forced are absorbed, and unforce restores the latest. `rant_variable_forced` reports it.
A definition needs `allow_force` (`RANT_ERR_STATE` without), and a remote's force against
an owner that advertises none is refused with `RANT_ERR_ROLE`, nothing sent.

Events, one slot each, `NULL` clears: `rant_variable_on_change` fires when the observed
state actually changes (first value, different bytes, a forced flip) and replays the
current value once at registration. `rant_variable_on_write` fires on every applied write.
Both get a `RantVariableUpdate` (value, schema, forced, write_seq, source peer with 0 =
local, recv_us, written_us) inline on the thread that applied the write, with the usual
callback restrictions, or at dispatch with that write's value when the variable has a
queue.

A remote's first write waits for the owner match like a first topic send, so
`RANT_ERR_NO_TOPIC` means the owner is really absent.

## Retire

`rant_function_retire` and `rant_variable_retire` park the entity's channels, answer every
outstanding call CANCELLED, silence callbacks and free the handle. The handle is invalid
after. From inside a callback, or while one of the handle's queued callbacks runs on
another thread, the call is refused with `RANT_ERR_STATE` and the handle stays valid. A
queued handle's parked callbacks are dropped, and a parked reply is answered CANCELLED
inline. A re created entity with the same name takes its old slots back. A second
same name handle while the first lives is refused: the create returns `NULL` and
`rant_last_error` says `RANT_E_NAME_COLLISION`. Retire the first.
Complete or abandon outstanding defer tokens before retiring a definition.

## Wrappers

C++, C#, Python and the JS bridge expose all three patterns. A thrown handler in C#,
Python or JS answers APP_ERROR with the exception text. C++ `defer()` returns a movable
`PendingTask`, C# handlers are async with a real `CancellationToken`, JS handlers get
`ctx.progress` and an `AbortSignal`. Only Python spawns threads (one daemon thread per
running call).
