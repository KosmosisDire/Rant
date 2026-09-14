# Functions, tasks and variables

Three interaction patterns built over topics. Strip them with `RAMBLE_NO_PATTERNS`. A
DEFINITION is where the body or the storage lives. A REMOTE is a reference to a definition
on another node. One node cannot be both sides of the same entity.

| pattern | shape | create |
|---|---|---|
| function | one request, exactly one response | `ramble_node_create_function_definition` / `ramble_node_create_remote_function` |
| task | one request, N progress updates, one response, cancellable | `ramble_node_create_task_definition` / `ramble_node_create_remote_task` |
| variable | replicated state with one owner | `ramble_node_create_variable_definition` / `ramble_node_create_remote_variable` |

A function, task or variable never cross wires with a plain topic or with each other under
the same name. Two definitions of the same entity on different nodes fire
`RAMBLE_E_DUPLICATE_AUTHORITY` on both, as a warning. Declare that on purpose with `.multi`.

## Functions

```c
static void on_add(RambleRequest *req, void *user){
    int32_t sum = add(req->data);                  /* decode req->data with req->schema */
    ramble_request_reply(req, ramble_bytes(&sum, sizeof sum));
}
RambleFunction *def = ramble_node_create_function_definition(n, "add", req_schema, rsp_schema,
                        on_add, NULL, NULL);
RambleFunction *fn  = ramble_node_create_remote_function(n, "add", req_schema, rsp_schema, NULL);

RambleResponse rsp;
if (ramble_function_call(fn, req, &rsp, 1000, NULL) == 1 && rsp.status == RAMBLE_CALL_OK) ...
ramble_function_call_async(fn, req, on_response, user, NULL);
```

The handler gets a `RambleRequest`: `data`, `schema`, `caller`, `caller_name`,
`function_name`, `recv_us` and `written_us`. It answers with `ramble_request_reply` (OK),
`ramble_request_fail(req, message, rsp)` (APP_ERROR with text), or `ramble_request_defer(req)`,
which returns a token for `ramble_function_complete(fn, token, status, message, rsp)` later
from any thread. A handler that returns without answering auto acks OK. Pass only the
exact request pointer the callback received, never a copy.

The caller's `RambleResponse` carries `status`, `data`, `schema`, `provider` (the answering
peer), `message` (human readable outcome text, always displayable) and `written_us`.
Statuses are `RAMBLE_CALL_OK`, `APP_ERROR`, `NO_HANDLER`, `TIMEOUT`, `PEER_LOST`,
`CANCELLED` and, for tasks, `RUNNING`.

`RambleFunctionOpts`: `timeout_us`, `backpressure_wait_us`, `keep_last` (request and
response history depth, default 10), `reflect_from_mesh` and `multi`. Raise `keep_last`
above the biggest batch of requests one poll pass can deliver, on both sides, or replies
past the depth are lost and the callers see `RAMBLE_CALL_TIMEOUT`.

`RambleCallOpts`: `provider` (direct the call at one peer, 0 = every definition and the
first answer wins), `on_progress` and `progress_user` (tasks), and `id_out` (the call id,
for cancel).

`ramble_function_call` drives the node loop itself and returns 1 when answered, 0 on a
timeout and a negative `RambleResult` on error, so it is refused with `RAMBLE_ERR_STATE` from
a callback or under a service thread, where `ramble_function_call_async` is the tool. On a
task the timeout bounds only the wait for the first response, then it waits for the
terminal outcome and `ramble_function_cancel` from another thread is the way out.

A call from a fresh remote waits for its match like a first topic send (docs/topics.md).

## Tasks

A task is a function with progress and cancellation on the same `RambleFunction` handle.
docs/tasks.md has the full model, the handler forms and the wrappers.

## Variables

```c
RambleVariable *owner  = ramble_node_create_variable_definition(n, "speed", schema,
                           &(RambleVariableOpts){ .initial = ramble_bytes(&v, sizeof v) });
RambleVariable *remote = ramble_node_create_remote_variable(n, "speed", schema, NULL);
ramble_variable_set(remote, value);          /* pushed to the owner, no response */
ramble_variable_get(remote, &bytes);         /* the latest replicated value */
ramble_variable_wait(remote, 1000);          /* until the first value arrives */
```

The definition owns the value. Remotes write with `ramble_variable_set` and a late remote
gets the latest value at once. `RambleVariableOpts`: `initial`, `access`
(`RAMBLE_VAR_READONLY` creates no set channel, so remote sets get `RAMBLE_ERR_ROLE`),
`allow_force`, `catch_up` (default 1), `keep_last` (the repair window for a burst of
writes, default 10), `backpressure_wait_us` and `reflect_from_mesh`.

`ramble_variable_force` overrides the value until `ramble_variable_unforce`. Writes while
forced are absorbed, and unforce restores the latest. `ramble_variable_forced` reports it.

Events, one slot each, `NULL` clears: `ramble_variable_on_change` fires when the observed
state actually changes (first value, different bytes, a forced flip) and replays the
current value once at registration. `ramble_variable_on_write` fires on every applied write.
Both get a `RambleVariableUpdate` (value, schema, forced, write_seq, source peer with 0 =
local, recv_us, written_us) inline on the thread that applied the write, with the usual
callback restrictions.

A remote's first write waits for the owner match like a first topic send, so
`RAMBLE_ERR_NO_TOPIC` means the owner is really absent.

## Retire

`ramble_function_retire` and `ramble_variable_retire` park the entity's channels, answer every
outstanding call CANCELLED, silence callbacks and free the handle. The handle is invalid
after. From inside a callback the call is refused with `RAMBLE_ERR_STATE` and the handle
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
