/* PATTERNS layer: functions and variables built over a DartNode. Each is a thin
 * interaction pattern over dedicated topic KINDS (transport/core.h DartTopicKind), so a
 * function/variable never cross-wires with a plain topic or with each other even when
 * they share a name. Depends on node/runtime only; the node hooks it uses are kind-agnostic.
 * Compile it out with DART_NO_PATTERNS.
 *
 *   FUNCTION  request/response, exactly one reply per call, ONE handler (req/rsp channels)
 *   TASK      a function with progress and cancellation (req/prg/rsp channels, same handle)
 *   VARIABLE  replicated state, one owner, dumb writes + optional force (value/@set channels)
 *
 * API doctrine: every constructor is dart_node_create_*. A DEFINITION is where the body or
 * storage lives; a REMOTE is a reference to a definition on another node
 * (function_definition / remote_function, variable_definition / remote_variable: one node
 * cannot be both sides). Bytes in C; the wrappers add typed ergonomics. */
#ifndef DART_PATTERNS_H
#define DART_PATTERNS_H

#include "../node/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DART_PATTERN_BP_WAIT_US
#define DART_PATTERN_BP_WAIT_US 1000000u   /* default backpressure wait for pattern channels (1s):
                                              low-rate by intent, loss unacceptable */
#endif
#ifndef DART_CALL_TIMEOUT_US
#define DART_CALL_TIMEOUT_US 5000000u      /* default client call timeout (5s) */
#endif

/* Max bytes of the human-readable response message (the wire carries its length in one
 * byte, so this is fixed, not tunable). A longer message passed to dart_request_fail /
 * dart_function_complete is truncated here, never refused. */
#define DART_CALL_MSG_MAX 255u

/* ---- FUNCTIONS ---------------------------------------------------------------------- */

/* A call's outcome. OK/APP_ERROR/NO_HANDLER/CANCELLED/RUNNING travel on the wire (the
 * response status byte); TIMEOUT/PEER_LOST are synthesized client-side when no response
 * arrives. */
typedef enum {
    DART_CALL_OK        = 0,
    DART_CALL_APP_ERROR = 1,   /* the handler replied with dart_request_fail */
    DART_CALL_NO_HANDLER= 2,   /* the definition side has no on_request registered */
    DART_CALL_TIMEOUT   = 3,   /* client-synthesized: no response within the timeout */
    DART_CALL_PEER_LOST = 4,   /* client-synthesized: the handler node dropped mid-call */
    DART_CALL_CANCELLED = 5,   /* wire-carried when a provider honors a cancel (or retires
                                  mid-run); also synthesized locally when the node closes
                                  or the handle retires with the call still pending, so
                                  every call gets exactly one outcome */
    DART_CALL_RUNNING   = 6    /* task, wire-carried, the ONLY non-terminal status: the
                                  request was accepted and runs. The call stays pending,
                                  its timeout is dropped, and the caller's on_progress
                                  fires once with zero-length data. */
} DartCallStatus;

typedef struct DartFunction DartFunction;   /* opaque function handle */

/* The request as delivered to the definition side's on_request: what a DartMsg carries, in
 * function vocabulary. Views are valid for the callback only. The reply machinery lives
 * BEHIND this struct, so pass only the exact pointer the callback received to
 * dart_request_reply / dart_request_fail / dart_request_defer, never a copy. */
typedef struct DartRequest {
    DartNode         *node;          /* the node the handler runs on */
    DartString        function_name; /* the function's name, @req suffix stripped (a view) */
    DartBytes         data;          /* the request payload */
    const DartSchema *schema;        /* the schema data decodes with (NULL = untyped caller).
                                        Non-NULL means data.len was validated against it. */
    uint32_t          caller;        /* the calling peer's id */
    DartString        caller_name;   /* the calling node's name (.data never NULL) */
    uint64_t          recv_us;       /* the node's monotonic clock at arrival */
    uint64_t          written_us;    /* the CALLER's wall clock when it wrote the request
                                        (DartMsg.written_us; 0 = it opted out of the stamp) */
} DartRequest;

/* Delivered to the caller when a response arrives (or is synthesized). data is a view valid
 * for the callback only. provider = the peer that answered (0 if synthesized). */
typedef struct {
    DartCallStatus    status;
    DartBytes         data;
    const DartSchema *schema;    /* the schema data decodes with (NULL = an untyped handler
                                    side or a synthesized outcome) */
    uint32_t          provider;
    void             *user;      /* the user pointer passed to dart_function_call_async */
    uint64_t          written_us; /* the PROVIDER's wall clock when it wrote the response
                                     (DartMsg.written_us); 0 for a synthesized outcome */
    DartString        message;   /* human-readable outcome text, the ONE field a generic
                                    consumer (an HMI) displays on failure: the provider's
                                    message when it sent one (dart_request_fail /
                                    dart_function_complete, capped at DART_CALL_MSG_MAX),
                                    else default status text ("timeout", "peer lost", ...)
                                    for every non-OK outcome, wire-carried or synthesized.
                                    Empty (len 0) only on OK with no message; .data is
                                    never NULL. A view, same lifetime as data. */
} DartResponse;
typedef void (*DartResponseFn)(const DartResponse *response);

/* The handler callback: inspect the request, then reply exactly once with
 * dart_request_reply / dart_request_fail, or dart_request_defer for an async completion.
 * Returning without replying auto-acks DART_CALL_OK with an empty payload. */
typedef void (*DartRequestFn)(DartRequest *request, void *user);

typedef struct {
    uint32_t backpressure_wait_us;  /* 0 = DART_PATTERN_BP_WAIT_US */
    uint32_t timeout_us;            /* remote-side call timeout; 0 = DART_CALL_TIMEOUT_US */
    uint16_t keep_last;             /* req + rsp history depth; 0 = the reliable default (10).
                                       An inline reply is a REENTRANT send (the handler runs
                                       under the node lock), so it can never wait for a TX
                                       pass: this ring is the only thing holding a batch of
                                       replies. Raise it above the most requests one poll pass
                                       can drain, or replies past the depth are lost and the
                                       callers see a timeout. */
    uint8_t  reflect_from_mesh;     /* NULL schemas take the entity's from the mesh (a caller
                                       writes @req with the widest schema the provider accepts
                                       and reads @rsp with the provider's); dart_function_refresh
                                       re-types the handle when that moves. A schema you pass
                                       always wins. */
    uint8_t  multi;                 /* many definitions of this function are EXPECTED, so the
                                       duplicate-authority diagnostic is suppressed for it.
                                       Direct a call at one definition with DartCallOpts
                                       .provider; an undirected call reaches EVERY definition
                                       and the first answer wins. The built-in @dart/meta is
                                       the canonical user; an ordinary function should keep
                                       the one-definition contract (leave it 0). */
} DartFunctionOpts;

/* One progress update as delivered to a task caller's on_progress. Views are valid for
 * the callback only. The RUNNING acknowledgment fires it once with zero-length data. */
typedef struct {
    uint32_t          call_id;
    uint32_t          provider;   /* the peer working the call */
    DartBytes         data;       /* the progress payload (len 0 = the RUNNING ack) */
    const DartSchema *schema;     /* prg schema (NULL = untyped or the RUNNING ack) */
    uint64_t          written_us; /* the provider's wall clock (0 = unstamped) */
    uint64_t          recv_us;    /* this node's monotonic clock at arrival */
    void             *user;       /* DartCallOpts.progress_user */
} DartProgress;
typedef void (*DartProgressFn)(const DartProgress *progress);

/* Optional per-call config (a trailing compound literal; NULL = defaults). */
typedef struct {
    uint32_t provider;              /* peer id to DIRECT the call at: only that peer receives
                                       the request (found via dart_node_peers / PEER_UP). 0 =
                                       undirected: every matched definition receives it, the
                                       first answer wins. A directed call whose peer drops
                                       fails with DART_CALL_PEER_LOST immediately. A TASK
                                       request is ALWAYS directed: 0 resolves to the oldest
                                       matched provider at send time. */
    DartProgressFn on_progress;     /* task: fires per progress update on the delivering
                                       thread (the RUNNING ack fires it once with empty
                                       data); NULL = updates are discarded */
    void     *progress_user;        /* handed back as DartProgress.user */
    uint32_t *id_out;               /* filled with the call id (for dart_function_cancel) */
} DartCallOpts;

/* Create the DEFINITION (the implementation lives here): subscribes requests, publishes
 * replies, runs on_request for each request (NULL answers DART_CALL_NO_HANDLER). Create a
 * REMOTE (a reference to a definition on another node): publishes requests, subscribes
 * replies. req/rsp schemas may be NULL (untyped; an empty rsp schema still flows an ack).
 * Returns a handle or NULL. */
DartFunction *dart_node_create_function_definition(DartNode *n, const char *name,
                    const DartSchema *req_schema, const DartSchema *rsp_schema,
                    DartRequestFn on_request, void *user, const DartFunctionOpts *opts);
DartFunction *dart_node_create_remote_function(DartNode *n, const char *name,
                    const DartSchema *req_schema, const DartSchema *rsp_schema,
                    const DartFunctionOpts *opts);

/* Call the function: blocks driving the node loop until the response arrives or timeout_ms
 * elapses (negative = the function's default timeout). *out is filled; out->data views a
 * manager-owned buffer valid until the next blocking call on this function. opts may be
 * NULL (undirected). Returns 1 (answered, read out->status: OK, APP_ERROR, NO_HANDLER, or
 * PEER_LOST), 0 (timed out, out->status = DART_CALL_TIMEOUT whether the local wait or the
 * pending deadline expired first), or a negative DartResult. Refused (DART_ERR_STATE) from
 * inside a callback or while a service thread owns the loop. On a TASK the timeout bounds
 * only the wait for the FIRST response: once RUNNING (or progress) arrives it waits for
 * the terminal outcome indefinitely, with opts->on_progress firing while it waits;
 * impatience is dart_function_cancel from another thread. */
int  dart_function_call(DartFunction *fn, DartBytes req, DartResponse *out, int timeout_ms,
                        const DartCallOpts *opts);
/* The async form: returns as soon as the request is committed, then on_response (NULL =
 * fire-and-forget: the outcome is discarded) fires once with the outcome. opts may be
 * NULL (undirected). Returns DART_OK, or a negative DartResult. */
int  dart_function_call_async(DartFunction *fn, DartBytes req, DartResponseFn on_response,
                              void *user, const DartCallOpts *opts);
/* Providers matched (remote side) / callers matched (definition side). */
int  dart_function_match_count(DartFunction *fn);
/* Retire the handle: park both channels (role INACTIVE, re-advertised, so peers re-verify
 * the name against a successor and a new create under the same name binds cleanly where a
 * live twin would have been SHADOWED), cancel every outstanding call with one
 * DART_CALL_CANCELLED outcome, silence its callbacks, and free the handle: it is INVALID
 * after, like a closed node. The channel SLOTS are released for reuse (dart_topic_retire):
 * a re-created same-name entity takes them back, so retire/create cycles plateau instead
 * of growing the announce. Complete (or abandon) outstanding defer tokens BEFORE retiring
 * a definition. Returns DART_OK; DART_ERR_STATE from inside a callback (the role flip
 * would rematch lanes mid-delivery), and the handle then remains valid. */
int  dart_function_retire(DartFunction *fn);
/* A reflect_from_mesh handle: re-read the mesh and, if the entity's generation moved, re-type
 * every channel IN PLACE (same handle; outstanding calls are answered CANCELLED first).
 * 1 = re-typed, 0 = current, negative on error (DART_ERR_ROLE without the flag). */
int  dart_function_refresh(DartFunction *fn);

/* ---- the built-in @dart/meta introspection endpoint ----------------------------------
 * Every node (patterns compiled in, opts.disable_meta off) hosts a "@dart/meta" function
 * AND can call every other node's: its channels are PUBSUB on both nodes, created with
 * .multi (every node being a definition is the design, not a duplicate-authority fault)
 * and DIRECTED requests, so a call aimed at one peer never wakes the rest. Ask with
 * DartCallOpts { .provider = peer_id } and an optional 4-byte LE DART_META_* section
 * mask as the payload (empty = everything); the reply is a `DartMeta { info: map }`
 * message whose map body is documented at the mask in node/runtime.h. An UNDIRECTED
 * call reaches every node and returns the first answer. Under a service thread / from a
 * callback use dart_function_call_async, as with any function. */
DartFunction *dart_node_meta_function(DartNode *n);   /* NULL when disabled / not built */

/* ---- in the handler callback (DartRequestFn) ----------------------------------------- */
void      dart_request_reply(DartRequest *request, DartBytes rsp);   /* answer OK */
/* Answer APP_ERROR. message is the human-readable reason (NUL-terminated, NULL = none:
 * the caller then sees the default "app error"; truncated at DART_CALL_MSG_MAX). It rides
 * the response header, so rsp may still carry structured failure data beside it. */
void      dart_request_fail (DartRequest *request, const char *message, DartBytes rsp);
/* Defer the reply: returns a token (0 on failure), suppresses the auto-ack, and lets the
 * handler return now. Complete it later (from any thread) with dart_function_complete
 * (message as in dart_request_fail; also carried on OK for warning/debug text). */
uint64_t  dart_request_defer(DartRequest *request);
int       dart_function_complete(DartFunction *fn, uint64_t token, DartCallStatus status,
                                 const char *message, DartBytes rsp);

/* ---- TASKS ---------------------------------------------------------------------------
 * A task is a FUNCTION with progress and cancellation: the same DartFunction handle, call
 * ids, statuses, pending table and defer tokens, plus a broadcast progress channel
 * (name+"@prg") and a cancel op on the request channel. One request, N progress updates,
 * exactly one terminal response, from RUNNING to the end cancellable (cooperatively).
 * The per-call timeout covers only the window until the FIRST response of any kind; after
 * RUNNING a task runs as long as it runs and cancel is the caller's tool for impatience.
 * A provider that drops mid-run fails the call with DART_CALL_PEER_LOST (requests are
 * always directed, so the peer reap covers it); retire and node close answer every live
 * deferred call with DART_CALL_CANCELLED while the channels are still up, so a RUNNING
 * caller never hangs on a clean shutdown. Unlike a function, a task handler that returns
 * without reply/fail/defer answers APP_ERROR "handler returned no result": an instant
 * empty OK on a long-running operation would read as success that never ran. */
typedef struct {
    uint8_t  progress_best_effort;  /* progress-channel reliability: 0 = reliable. The
                                       definition OFFERS, a remote REQUESTS (the RxO rule
                                       composes them: an observer may tap a reliable
                                       stream best-effort and can never stall the task) */
    uint16_t progress_keep_last;    /* progress ring depth; 0 = the pattern default */
    uint8_t  no_cancel;             /* definition: will not honor cancellation. Clears the
                                       CANCELLABLE attrs bit, so remotes refuse
                                       dart_function_cancel locally (DART_ERR_ROLE) */
    uint8_t  exclusive;             /* definition: declared serialization; the handler
                                       enforces it (answer "busy" via dart_request_fail) */
    uint8_t  multi;                 /* redundant providers intended (DartFunctionOpts.multi);
                                       each request still has exactly one executor */
    uint32_t timeout_us;            /* remote: until-first-response bound; 0 = DART_CALL_TIMEOUT_US */
    uint32_t backpressure_wait_us;  /* 0 = DART_PATTERN_BP_WAIT_US */
    uint16_t keep_last;             /* req + rsp history depth, as DartFunctionOpts.keep_last
                                       (progress_keep_last covers the prg channel) */
    uint8_t  reflect_from_mesh;     /* as DartFunctionOpts.reflect_from_mesh, for all three channels */
} DartTaskOpts;

/* Create the DEFINITION (the implementation lives here) or a REMOTE, exactly as for a
 * function; prg_schema types the progress channel (NULL = untyped). The handler answers
 * inline (dart_request_reply / dart_request_fail) or dart_request_start +
 * dart_request_defer + return, then works through the token from whatever thread the app
 * owns. Returns a handle or NULL. */
DartFunction *dart_node_create_task_definition(DartNode *n, const char *name,
                    const DartSchema *req_schema, const DartSchema *prg_schema,
                    const DartSchema *rsp_schema, DartRequestFn on_request, void *user,
                    const DartTaskOpts *opts);
DartFunction *dart_node_create_remote_task(DartNode *n, const char *name,
                    const DartSchema *req_schema, const DartSchema *prg_schema,
                    const DartSchema *rsp_schema, const DartTaskOpts *opts);

/* In the handler: send RUNNING to the caller now (empty payload, non-terminal).
 * Idempotent; dart_request_defer on a task implies it. DART_ERR_STATE on a plain
 * function request or after a reply. */
int dart_request_start(DartRequest *request);
/* Token verbs (any thread, like dart_function_complete). Every token verb validates the
 * token against the handle's live-defer registry first, so a stale token (completed, or
 * cancelled away by retire/close) is DART_ERR_STATE, never UB; functions gain the same
 * validation. progress broadcasts on the @prg channel (any observer may watch);
 * cancelled answers 1 the moment a cancel for the call arrived. Cancellation is
 * COOPERATIVE: honor it with dart_function_complete(DART_CALL_CANCELLED, ...), or run to
 * completion anyway. Task-only (DART_ERR_STATE on a plain function). */
int dart_function_progress (DartFunction *fn, uint64_t token, DartBytes progress);
int dart_function_cancelled(DartFunction *fn, uint64_t token);
/* Cancel notification, one slot per definition (re-register replaces, NULL clears):
 * fires on the poll thread when a cancel lands on a live deferred call, under the usual
 * callback restrictions. Optional: polling dart_function_cancelled alone is complete. */
typedef void (*DartCancelFn)(uint64_t token, void *user);
int dart_function_on_cancel(DartFunction *def, DartCancelFn on_cancel, void *user);
/* Caller: request cancellation of the outstanding call call_id (from
 * DartCallOpts.id_out). Cooperative and never acked: the terminal status is the answer
 * (CANCELLED = honored or never started; a normal outcome = it completed anyway). A call
 * still queued (no provider matched yet) cancels locally with one CANCELLED outcome.
 * DART_ERR_ROLE when the provider declared no_cancel (checked against its cached attrs,
 * nothing sent); DART_ERR_STATE when the call is not pending (already answered). */
int dart_function_cancel(DartFunction *fn, uint32_t call_id);

/* ---- VARIABLES ---------------------------------------------------------------------- */

/* Replicated state with ONE owner: the definition. It publishes the value on change (value
 * channel); writers push new values over a set channel (dumb writes, no response: a set that
 * needs completion wants a function). Optional FORCE overrides the value with a shadow source
 * until unforced (v1: dart-owned storage only). Remotes cache the latest value. */
typedef struct DartVariable DartVariable;

typedef enum { DART_VAR_READWRITE = 0, DART_VAR_READONLY = 1 } DartVarAccess;

typedef struct {
    DartBytes initial;              /* definition: the value before any set (empty = none yet) */
    uint8_t   access;              /* DartVarAccess: a READONLY definition creates no set channel */
    uint8_t   allow_force;         /* definition: permit force (local + remote); off by default */
    uint16_t  catch_up;            /* value-channel catch_up; 0 = 1 (a late remote gets the latest) */
    uint16_t  keep_last;           /* history depth of BOTH channels: the REPAIR window a burst of
                                      writes rides in, not what a late remote replays (catch_up is
                                      that). 0 = the reliable default (10). Raise it when writes
                                      outrun repair, or when catch_up is deeper (a catch_up past the
                                      ring truncates); lower it to pin less on a big value */
    uint32_t  backpressure_wait_us;/* 0 = DART_PATTERN_BP_WAIT_US */
    uint8_t   reflect_from_mesh;   /* a NULL schema takes the variable's from the mesh (the owner's);
                                      dart_variable_refresh re-types the handle when that moves */
} DartVariableOpts;

/* Create the DEFINITION (this node holds the authoritative value) or a REMOTE (the value
 * lives elsewhere: reads see the cached latest, writes go over the set channel). schema may
 * be NULL (untyped). Returns a handle or NULL. */
DartVariable *dart_node_create_variable_definition(DartNode *n, const char *name,
                              const DartSchema *schema, const DartVariableOpts *opts);
DartVariable *dart_node_create_remote_variable(DartNode *n, const char *name,
                              const DartSchema *schema, const DartVariableOpts *opts);

/* Read the current value: definition = the store, remote = the cached latest. Returns 1 and fills
 * *out (a view valid until the next call on this variable / next poll) if a value exists, else 0. */
int  dart_variable_get(DartVariable *var, DartBytes *out);
/* Set the value. Definition: apply + publish immediately (absorbed into the shadow source
 * while forced). Remote: send over the set channel. Returns DART_OK; DART_ERR_NO_TOPIC when
 * no owner is matched at all; DART_ERR_ROLE when an owner is matched but advertises no set
 * channel (a read-only variable); or a negative DartResult from the send. A fresh remote's
 * FIRST write no longer races the forming match: while candidate verdicts are in flight it
 * waits like a first topic send (bounded by opts.match_wait_ms; skipped from a callback or
 * with the wait disabled) before deriving the verdict, so NO_TOPIC means the owner is
 * genuinely absent, never merely still-matching. Converged matching never waits. The same
 * routing (and wait) covers dart_variable_force / dart_variable_unforce. */
int  dart_variable_set(DartVariable *var, DartBytes value);
/* Force the value to `value`: writes are absorbed into the shadow source until unforce, which
 * restores the LATEST absorbed set. Definition: applies locally; returns DART_ERR_STATE unless
 * the variable was created with .allow_force (a refusable local call fails loudly). Remote:
 * sends the force op; a definition without allow_force IGNORES it silently (ops on the set
 * channel are opaque, like every write). forced() reports the current state (definition:
 * authoritative; remote: the last received value's FORCED flag). */
int  dart_variable_force(DartVariable *var, DartBytes value);
int  dart_variable_unforce(DartVariable *var);
int  dart_variable_forced(DartVariable *var);

/* ---- variable events (on_change / on_write) ------------------------------------------
 * One registration slot each (re-register replaces, NULL clears); both receive the same
 * DartVariableUpdate view of the state just applied. Both sides observe: a definition sees
 * local and remote writes the moment they apply, a remote sees each value as it arrives.
 *   on_write   fires on EVERY write applied to the observed value, byte-identical or not:
 *              local and remote sets, force, unforce, each value a remote receives. A write
 *              absorbed into the shadow while forced does not fire (the observed value did
 *              not change; the unforce that restores it does).
 *   on_change  fires only when the observed STATE changes: the first value, bytes that
 *              differ from the current value, or a flip of the forced flag. A byte-identical
 *              re-set stays silent (write_seq still advances). If a value already exists at
 *              registration the callback fires once immediately with it, so registering
 *              right after create can never miss the current state.
 * THREADING: callbacks fire inline, under the node lock, on the thread that applied the
 * write: the poll / service thread for anything arriving off the wire, the calling thread
 * for a local set/force/unforce. Same restrictions as any delivery callback. A reentrant
 * set from inside a callback is SAFE: the layer skips the then-stale outer publish, so
 * transport history (and catch_up replay) always ends on the newest write. Consumer-thread
 * delivery (dispatch-style, collapsed to the latest state) and edge triggers (rising /
 * falling) are planned extensions; they will keep this shape: one slot per event kind, the
 * same DartVariableUpdate payload. */
typedef struct {
    DartVariable     *variable;
    DartString        name;        /* the variable's name (a stable view) */
    DartBytes         value;       /* the value just applied (a view, valid for the callback) */
    const DartSchema *schema;      /* the schema value decodes with (NULL = untyped) */
    uint8_t           forced;      /* the value is a forced override */
    uint32_t          write_seq;   /* the owner's write counter */
    uint32_t          source;      /* peer id the write arrived from; 0 = a local call */
    uint64_t          recv_us;     /* the node's monotonic clock when the write applied */
    uint64_t          written_us;  /* the WRITER's wall clock for this write: the delivering
                                      message's DartMsg.written_us for a remote write, this node's
                                      wall clock for a local one (0 = the source opted out) */
} DartVariableUpdate;
typedef void (*DartVariableUpdateFn)(const DartVariableUpdate *update, void *user);

int  dart_variable_on_change(DartVariable *var, DartVariableUpdateFn on_change, void *user);
int  dart_variable_on_write (DartVariable *var, DartVariableUpdateFn on_write,  void *user);
/* Block driving the node loop until a value exists (remote first value) or timeout_ms
 * elapses (negative = forever-ish). 1 = have a value, 0 = timeout. Refused from a callback /
 * under a service thread (returns 0). */
int  dart_variable_wait(DartVariable *var, int timeout_ms);
/* Remote: owners matched (0 = no owner present). Definition: remotes matched. */
int  dart_variable_match_count(DartVariable *var);
/* Retire the handle: park its channels, silence on_change/on_write, free the handle
 * (INVALID after). The one lifecycle verb for "this accessor/definition is done": without
 * it a re-created same-name handle is silently shadowed by the live twin (the per-peer
 * index maps bind a name to ONE local topic, preferring the oldest active one). Same
 * contract as dart_function_retire. */
int  dart_variable_retire(DartVariable *var);
/* As dart_function_refresh, for a reflect_from_mesh variable. */
int  dart_variable_refresh(DartVariable *var);

#ifdef __cplusplus
}
#endif
#endif /* DART_PATTERNS_H */
