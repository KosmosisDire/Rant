# Tasks

A task is a function with progress and cancellation: the same request and response
machinery, call ids, statuses and `DartFunction` handle. Two additions: a progress channel
the definition publishes while it works, and a cancel op the caller (or a tool) can send.
The vocabulary is programming vocabulary throughout: request, call, progress, RUNNING,
complete, fail, cancel.

| pattern | shape | lifetime |
|---|---|---|
| FUNCTION | one request, exactly one response | one round trip |
| TASK | one request, N progress, one response, cancellable | seconds to hours |
| VARIABLE | replicated state, one owner | forever, latched |

Routing rule: something that answers in one round trip is a FUNCTION. A value that is
always there is a VARIABLE. A sample stream is a TOPIC. Something that runs, that you want
to watch and be able to stop, is a TASK. Canonical cases: file transfer (the chunks are the
progress), navigation, firmware update, calibration, recording, batch jobs.

## The model

```
CALLER (remote task)                        DEFINITION (the implementation)

call(request) ------------ t@req ---------> handler(request)
                                              dart_request_start / defer
on_progress (RUNNING) <--- t@rsp ---------  RUNNING (non terminal status)
on_progress(p) <---------- t@prg ---------  dart_function_progress   (repeat)
cancel(id) --------------- t@req ---------> cancel flag set (+ on_cancel slot)
                                              ... handler winds down ...
on_response(rsp) <-------- t@rsp ---------  dart_function_complete
```

One invocation is one call, identified by the function layer's u32 call id. Many calls
can be in flight at once, from many callers. Every call gets exactly one TERMINAL response
at every exit: normal completion, provider retire and node close (the definition answers
its live deferred calls CANCELLED on the wire before teardown), a severed lane
(synthesized CANCELLED), or a provider crash (the PEER_LOST reap). `DART_CALL_RUNNING` is
the one non terminal status. `CANCELLED` is wire carried when a provider honors a cancel
and may still carry a payload (a stopped recording's partial file), which a function
cannot express.

Timeouts. The per call timeout covers only the window until the FIRST response of any
kind (RUNNING or progress). After that a task runs as long as it runs, and cancel is the
caller's tool for impatience. There is no per call deadline option in the core. Wrappers
build one from cancel (C#: `CancellationTokenSource.CancelAfter`).

## Creating and calling

```c
DartFunction *t = dart_node_create_task_definition(n, "transfer",
                      req_schema, prg_schema, rsp_schema, on_transfer, NULL,
                      &(DartTaskOpts){ 0 });               /* NULL = all defaults */
DartFunction *r = dart_node_create_remote_task(n, "transfer",
                      req_schema, prg_schema, rsp_schema, NULL);

uint32_t id;
dart_function_call_async(r, req, on_response, NULL,
    &(DartCallOpts){ .on_progress = on_progress, .id_out = &id });
/* ... later, impatient: */
dart_function_cancel(r, id);
```

`DartCallOpts.on_progress` fires per update on the delivering thread. The RUNNING ack
fires it once with zero length data. The blocking `dart_function_call` fires it on the
calling thread while it waits. Its timeout bounds only the wait for the first response,
then it waits for the terminal outcome (a cancel from another thread is the way out).

Task requests are always directed at one provider. `.provider` picks explicitly, and 0
resolves to the oldest matched at send time (or at flush for a call queued before any
match), so a request never starts the work on two nodes. Redundant providers are declared
with `DartTaskOpts.multi`, which suppresses the duplicate authority diagnostic. Each
request still has exactly one executor.

## The handler: quick on the poll thread, work anywhere

The handler fires on the poll thread and must be quick. Either answer inline
(`dart_request_reply` or `dart_request_fail`, exactly as a function) or defer and return.
`dart_request_defer` implies RUNNING (`dart_request_start` sends it earlier, for example
before a slow validation). The returned token then drives the work from any thread the
app owns. DART spawns no threads. The fully single threaded superloop:

```c
static void on_transfer(DartRequest *req, void *user){  /* poll thread, returns fast */
    job.token = dart_request_defer(req);                /* implies RUNNING */
    job.active = 1;
}
for (;;){                                               /* the app's one loop */
    dart_node_poll(n, 10);
    if (job.active){
        if (dart_function_cancelled(t, job.token)){
            dart_function_complete(t, job.token, DART_CALL_CANCELLED, "stopped", partial);
            job.active = 0;
        } else {
            do_one_chunk(&job);
            dart_function_progress(t, job.token, chunk(&job));
            if (job.done){ dart_function_complete(t, job.token, DART_CALL_OK, NULL, result);
                           job.active = 0; }
        }
    }
}
```

Tokens live in a per handle registry and every token verb validates membership first, so
a stale token (already completed, or cancelled away by retire or close) is a safe
`DART_ERR_STATE`, never undefined behavior. A handler that returns without reply, fail or
defer answers APP_ERROR "handler returned no result". Unlike a function, where the return
is the answer, an instant empty OK on a long running operation would read as a success
that never ran.

## Cancellation: cooperative, default on, never acked

Cancel is an op on the reliable request channel, so delivery needs no ack. Whether the
handler ACTS is unknowable until it acts, so there is no cancel response. The terminal
status is the answer: CANCELLED means honored, a normal outcome means it ran to completion
anyway. The definition polls `dart_function_cancelled(fn, token)` or registers the one
`dart_function_on_cancel` slot (it fires on the poll thread, and exists so wrappers can
be event driven).

The cancellable attrs bit is ON by default. A definition that will not honor cancellation
opts out with `DartTaskOpts.no_cancel`. Remotes then refuse `dart_function_cancel` locally
with `DART_ERR_ROLE` (nothing sent) and tools grey out their cancel button. `.exclusive`
declares serialization. It is advisory: the handler enforces it by answering
`dart_request_fail(req, "busy", ...)`. There is no REJECTED status.

A CANCEL op with a 4 byte `[u32 caller_lo]` payload cancels another caller's call (the
explorer's cancel button). An empty payload means the sender's own.

## Progress: broadcast, demuxed by caller, QoS per side

The `t@prg` channel is broadcast, so any observer can watch. Its header is
`[u32 caller_lo][u32 call_id]` where caller_lo is the requester's uuid low 32 bits. Call
ids are per caller counters, so caller_lo is what keeps two callers' same numbered calls
apart, and it hands observers requester attribution on the wire for free. Who handles is
`DartMsg.publisher_name`, as always. The explorer keys progress rows on provider plus
caller_lo plus call_id.

Reliability is configurable per side with `DartTaskOpts.progress_best_effort` (default
reliable and timestamped, and `progress_keep_last` sizes the ring). The RxO rule composes
the mixed case: the caller subscribes reliable and gets every update with backpressure
end to end, an observer subscribes best effort and is fire and forget, out of flow
control, so it can never stall the task. That is what makes data bearing progress work: a
file transfer ships its chunks as progress (reliable, backpressured, nothing lost) while a
UI taps the same stream lossily for a percentage.

## Wire summary

Kinds `DART_KIND_TASK_REQ`, `DART_KIND_TASK_PRG` and `DART_KIND_TASK_RSP` (5, 6, 7) on
channels `name@req`, `name@prg` and `name@rsp`. `@req` keeps the 5 byte function prefix
`[u32 call_id][u8 op]` (op 0 = CALL, 1 = CANCEL, schema exempt). `@prg` is the 8 byte
`[u32 caller_lo][u32 call_id]`. `@rsp` is byte identical to function responses
(`[u32 call_id][u8 status][u8 msg_len][msg]`), with RUNNING as a zero payload non
terminal. The definition's attrs byte (cancellable, exclusive, multi) rides the detail
exchange, cached per (peer, index).

## Wrappers

No wrapper spawns threads except Python (one daemon thread per running call).

- C++: `TaskDefinition<Req,Prg,Rsp>` and `RemoteTask<Req,Prg,Rsp>`. The handler gets a
  `TaskRequest&`. `defer()` returns a movable thread safe `PendingTask` made to be moved
  into the app's own thread (`progress`, `cancelled`, `complete`, `fail`,
  `complete_cancelled`, the last accepting a partial result). `call` blocks and fires
  progress while waiting, `call_async` returns a `TaskCall` with the id, and `cancel(id)`
  cancels.
- C#: the handler is an async delegate `Func<TReq, TaskContext<TPrg>, Task<TRsp>>` with
  `ctx.Progress` and a real `CancellationToken`. It runs on the poll thread until its
  first await, so CPU work belongs in `Task.Run`. `OperationCanceledException` completes
  CANCELLED. Caller: `CallAsync(req, IProgress<TPrg>, CancellationToken, provider)`
  returning a never faulting `Task<DartResponse<TRsp>>`. Cancelling the token cancels the
  remote task. Functions also gained async handler overloads.
- Python: the handler runs on a per call daemon thread with a `TaskRequest`
  (`.progress(x)`, `.cancelled`, `.cancel_event`). Returning completes OK, raising
  `dart.CancelledError(msg)` completes CANCELLED, any other exception APP_ERROR. Caller:
  `call(req, on_progress=...)` blocking, `call_async(...)` returning the id, `cancel(id)`.
- JS and bridge: handler `async (req, ctx)` with `ctx.progress(v)` and `ctx.signal` (an
  AbortSignal, where `throwIfAborted` completes CANCELLED). Caller: `const run =
  task.call(req)`, then `run.onProgress(cb)` (null = the RUNNING ack), `await run.result`,
  `run.cancel()`.

Reflection folds the three channels into one `DART_ENTITY_TASK` entity (name, the three
schemas, cancellable, exclusive, multi). The explorer shows tasks with live progress per
run, requester names resolved from caller_lo, and a cancel button gated on cancellable.
