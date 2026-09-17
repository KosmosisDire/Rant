/* The patterns layer: functions, tasks and variables over dedicated topic kinds, so they
 * never cross wire with a plain topic or each other. docs/patterns.md explains the API. */
#ifndef RANT_PATTERNS_H
#define RANT_PATTERNS_H

#include "../common/api.h"
#include "../node/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RANT_PATTERN_BP_WAIT_US
#define RANT_PATTERN_BP_WAIT_US 1000000u     /* the default backpressure wait, 1 s */
#endif
#ifndef RANT_CALL_TIMEOUT_US
#define RANT_CALL_TIMEOUT_US 5000000u        /* the default call timeout, 5 s */
#endif

/* The response message cap. The wire carries its length in one byte, so this is fixed.
 * A longer message is truncated, never refused. */
#define RANT_CALL_MSG_MAX 255u

/* functions */

/* A call's outcome. OK, APP_ERROR, NO_HANDLER, CANCELLED and RUNNING travel on the wire,
 * TIMEOUT, PEER_LOST and NO_PROVIDER are synthesized at the caller. See spec/patterns.md. */
typedef enum {
    RANT_CALL_OK          = 0,
    RANT_CALL_APP_ERROR = 1,     /* the handler replied with rant_request_fail */
    RANT_CALL_NO_HANDLER= 2,     /* the definition has no on_request */
    RANT_CALL_TIMEOUT     = 3,   /* no response within the timeout */
    RANT_CALL_PEER_LOST = 4,     /* the handler node dropped mid call */
    RANT_CALL_CANCELLED = 5,     /* a provider honored a cancel or retired mid run, or the node
                                  closed or the handle retired with the call still pending */
    RANT_CALL_RUNNING     = 6,   /* task, the one non terminal status: the request runs, the
                                  timeout is dropped, on_progress fires once with no data */
    RANT_CALL_NO_PROVIDER = 7    /* the timeout passed with no definition ever matched, so the
                                  request never left this node */
} RantCallStatus;

typedef struct RantFunction RantFunction;       /* an opaque handle */

/* The request as delivered to the definition's on_request. Views are valid for the
 * callback only. Pass only the exact pointer received to the reply calls, never a copy. */
typedef struct RantRequest {
    RantNode           *node;          /* the node the handler runs on */
    RantString          function_name; /* the name with the @req suffix stripped */
    RantBytes           data;          /* the request payload */
    const RantSchema *schema;          /* the schema data decodes with, NULL = an untyped caller.
                                        Non NULL means data.len was validated against it */
    uint32_t            caller;        /* the calling peer's id */
    RantString          caller_name;   /* the calling node's name, .data never NULL */
    uint64_t            recv_us;       /* the monotonic clock at arrival */
    uint64_t            written_us;    /* the caller's wall clock at the write, 0 = it opted out */
} RantRequest;

/* Delivered to the caller when a response arrives or is synthesized. data is a view valid
 * for the callback only. provider is the peer that answered, 0 if synthesized. */
typedef struct {
    RantCallStatus      status;
    RantBytes           data;
    const RantSchema *schema;      /* NULL = an untyped handler or a synthesized outcome */
    uint32_t          provider;
    void             *user;      /* the pointer passed to rant_function_call_async */
    uint64_t            written_us; /* the provider's wall clock at the write, 0 if synthesized */
    RantString          message;   /* the outcome text an HMI displays: the provider's, else the
                                    default status text. Empty only on OK, .data never NULL */
} RantResponse;
typedef void (*RantResponseFn)(const RantResponse *response);

/* The handler: reply exactly once with rant_request_reply or rant_request_fail, or defer
 * with rant_request_defer. Returning without replying auto acks RANT_CALL_OK. */
typedef void (*RantRequestFn)(RantRequest *request, void *user);

typedef struct {
    uint32_t backpressure_wait_us;  /* 0 = RANT_PATTERN_BP_WAIT_US */
    uint32_t timeout_us;            /* the remote side call timeout, 0 = RANT_CALL_TIMEOUT_US */
    uint16_t keep_last;             /* req and rsp history depth, 0 = 10. Raise it above the most
                                       requests one pass drains, or replies past it are lost */
    uint8_t  reflect_from_mesh;     /* NULL schemas take the entity's from the mesh, and refresh
                                       re types the handle when that moves. A passed schema wins */
    uint8_t  multi;                 /* many definitions are expected, so the duplicate authority
                                       diagnostic is off. An undirected call reaches every one */
    RantQueue *queue;               /* the handle's callbacks park here and run on
                                       rant_queue_dispatch. NULL = inline on the loop thread */
} RantFunctionOpts;

/* One progress update as delivered to a task caller's on_progress. Views are valid for
 * the callback only. The RUNNING acknowledgment fires it once with zero length data. */
typedef struct {
    uint32_t            call_id;
    uint32_t            provider; /* the peer working the call */
    RantBytes           data;     /* len 0 = the RUNNING ack */
    const RantSchema *schema;     /* NULL = untyped or the RUNNING ack */
    uint64_t          written_us; /* the provider's wall clock, 0 = unstamped */
    uint64_t          recv_us;    /* the monotonic clock at arrival */
    void             *user;       /* RantCallOpts.progress_user */
} RantProgress;
typedef void (*RantProgressFn)(const RantProgress *progress);

/* Per call options, a trailing compound literal, NULL = defaults. */
typedef struct {
    uint32_t provider;              /* direct the call at this peer only, 0 = every definition and
                                       the first answer wins. A task always directs, 0 = oldest */
    RantProgressFn on_progress;       /* task: fires per progress update where the handle's
                                       callbacks run, NULL = updates are discarded */
    void     *progress_user;        /* RantProgress.user */
    uint32_t *id_out;               /* filled with the call id, for rant_function_cancel */
} RantCallOpts;

/* Creates the definition (the body lives here, on_request NULL answers NO_HANDLER) or a
 * remote (a reference to one). Schemas may be NULL for untyped. NULL on failure. */
RANT_API RantFunction *rant_node_create_function_definition(RantNode *n, const char *name,
                             const RantSchema *req_schema, const RantSchema *rsp_schema,
                             RantRequestFn on_request, void *user, const RantFunctionOpts *opts);
RANT_API RantFunction *rant_node_create_remote_function(RantNode *n, const char *name,
                             const RantSchema *req_schema, const RantSchema *rsp_schema,
                             const RantFunctionOpts *opts);

/* Calls and blocks: on the service thread's progress when one runs, else driving the loop
 * itself. timeout_ms negative = the function's default. out->data is valid until the next
 * blocking call. 1 = an outcome in *out (NO_PROVIDER and PEER_LOST included), 0 = the local
 * timeout, RANT_ERR_STATE from a callback. docs/patterns.md has the returns. */
RANT_API int    rant_function_call(RantFunction *fn, RantBytes req, RantResponse *out, int timeout_ms,
                                 const RantCallOpts *opts);
/* Returns as soon as the request is committed, then on_response fires once with the
 * outcome. NULL = fire and forget. RANT_OK or a negative RantResult. */
RANT_API int    rant_function_call_async(RantFunction *fn, RantBytes req, RantResponseFn on_response,
                                       void *user, const RantCallOpts *opts);
/* Providers matched at a remote, callers matched at a definition. */
RANT_API int    rant_function_match_count(RantFunction *fn);
/* Parks both channels, cancels every outstanding call with one CANCELLED outcome and frees
 * the handle. RANT_ERR_STATE from a callback, the handle then stays valid. */
RANT_API int    rant_function_retire(RantFunction *fn);
/* A reflect_from_mesh handle: re types every channel in place when the generation moved.
 * 1 re typed, 0 current, negative on error, RANT_ERR_ROLE without the flag. */
RANT_API int    rant_function_refresh(RantFunction *fn);

/* The built in @rant/meta function every node hosts and can call, with .multi and directed
 * requests. Ask with RantCallOpts.provider and a RANT_META_* mask. NULL when disabled. */
RANT_API RantFunction *rant_node_meta_function(RantNode *n);

/* in the handler callback */
RANT_API void        rant_request_reply(RantRequest *request, RantBytes rsp);         /* answers OK */
/* Answers APP_ERROR with a human readable message (NULL = the default text, truncated at
 * RANT_CALL_MSG_MAX). rsp may still carry structured failure data. */
RANT_API void        rant_request_fail (RantRequest *request, const char *message, RantBytes rsp);
/* Defers the reply: returns a token (0 on failure) and suppresses the auto ack. Complete it
 * later from any thread with rant_function_complete. */
RANT_API uint64_t    rant_request_defer(RantRequest *request);
RANT_API int         rant_function_complete(RantFunction *fn, uint64_t token, RantCallStatus status,
                                          const char *message, RantBytes rsp);

/* Tasks: a function with progress and cancellation on the same handle. docs/tasks.md and
 * spec/patterns.md explain the model. */
typedef struct {
    uint8_t  progress_best_effort;  /* 0 = reliable. The definition offers, a remote requests,
                                       and the RxO rule composes them */
    uint16_t progress_keep_last;    /* the progress ring depth, 0 = the pattern default */
    uint8_t  no_cancel;             /* definition: will not honor cancellation, so remotes refuse
                                       rant_function_cancel with RANT_ERR_ROLE */
    uint8_t  exclusive;             /* definition: declared serialization, the handler enforces */
    uint8_t  multi;                 /* redundant providers intended, one executor per request */
    uint32_t timeout_us;            /* remote: the until first response bound, 0 = the default */
    uint32_t backpressure_wait_us;  /* 0 = RANT_PATTERN_BP_WAIT_US */
    uint16_t keep_last;             /* req and rsp history depth, as RantFunctionOpts.keep_last */
    uint8_t  reflect_from_mesh;     /* as RantFunctionOpts.reflect_from_mesh, for all three */
    RantQueue *queue;               /* as RantFunctionOpts.queue, for every callback of the handle */
} RantTaskOpts;

/* Creates the definition or a remote, exactly as for a function, with prg_schema typing the
 * progress channel. The handler answers inline, or starts, defers and works the token. */
RANT_API RantFunction *rant_node_create_task_definition(RantNode *n, const char *name,
                             const RantSchema *req_schema, const RantSchema *prg_schema,
                             const RantSchema *rsp_schema, RantRequestFn on_request, void *user,
                             const RantTaskOpts *opts);
RANT_API RantFunction *rant_node_create_remote_task(RantNode *n, const char *name,
                             const RantSchema *req_schema, const RantSchema *prg_schema,
                             const RantSchema *rsp_schema, const RantTaskOpts *opts);

/* Sends RUNNING to the caller now. Idempotent, and implied by defer on a task.
 * RANT_ERR_STATE on a plain function or after a reply. */
RANT_API int rant_request_start(RantRequest *request);
/* Token verbs, any thread. A stale token is RANT_ERR_STATE. progress broadcasts on the prg
 * channel, cancelled answers 1 once a cancel arrived. Cancellation is cooperative. */
RANT_API int rant_function_progress (RantFunction *fn, uint64_t token, RantBytes progress);
RANT_API int rant_function_cancelled(RantFunction *fn, uint64_t token);
/* The cancel notification, one slot per definition, NULL clears. Fires where the handle's
 * callbacks run. Polling rant_function_cancelled alone is complete. */
typedef void (*RantCancelFn)(uint64_t token, void *user);
RANT_API int rant_function_on_cancel(RantFunction *def, RantCancelFn on_cancel, void *user);
/* Requests cancellation of call_id, cooperative and never acked: the terminal status is the
 * answer. RANT_ERR_ROLE when the provider declared no_cancel, RANT_ERR_STATE when not pending. */
RANT_API int rant_function_cancel(RantFunction *fn, uint32_t call_id);

/* Variables: replicated state with one owner, the definition, which publishes the value.
 * Writers push over a set channel with no response. Remotes cache the latest value. */
typedef struct RantVariable RantVariable;

typedef enum { RANT_VAR_READWRITE = 0, RANT_VAR_READONLY = 1 } RantVarAccess;

typedef struct {
    RantBytes initial;                /* definition: the value before any set, empty = none */
    uint8_t     access;              /* RantVarAccess, READONLY creates no set channel */
    uint8_t     allow_force;         /* definition: permit force, off by default */
    uint16_t    catch_up;            /* the value channel's catch_up, 0 = 1 */
    uint16_t    keep_last;           /* both channels' history depth, the repair window for a burst of
                                      writes, 0 = 10. Raised to catch_up. See spec/patterns.md */
    uint32_t  backpressure_wait_us;/* a write pauses this long for a slow reader, 0 = never */
    uint8_t   reflect_from_mesh;   /* a NULL schema takes the owner's from the mesh, and refresh
                                      re types the handle when that moves */
    RantQueue *queue;              /* on_change and on_write park here and run on
                                      rant_queue_dispatch. NULL = inline on the applying thread */
} RantVariableOpts;

/* Creates the definition (this node holds the value) or a remote (reads see the cached
 * latest, writes go over the set channel). schema NULL = untyped. NULL on failure. */
RANT_API RantVariable *rant_node_create_variable_definition(RantNode *n, const char *name,
                                       const RantSchema *schema, const RantVariableOpts *opts);
RANT_API RantVariable *rant_node_create_remote_variable(RantNode *n, const char *name,
                                       const RantSchema *schema, const RantVariableOpts *opts);

/* Reads the current value into *out, a view valid until the next call on this variable or
 * the next poll. 1 if a value exists. */
RANT_API int    rant_variable_get(RantVariable *var, RantBytes *out);
/* Sets the value: a definition applies and publishes, a remote sends over the set channel.
 * RANT_ERR_NO_TOPIC = no owner matched, RANT_ERR_ROLE = a read only owner. docs/patterns.md. */
RANT_API int    rant_variable_set(RantVariable *var, RantBytes value);
/* Force overrides the value until unforce restores the latest absorbed set. A definition
 * needs .allow_force (RANT_ERR_STATE without), and a remote's force against an owner that
 * advertises none is refused with RANT_ERR_ROLE, nothing sent. */
RANT_API int    rant_variable_force(RantVariable *var, RantBytes value);
RANT_API int    rant_variable_unforce(RantVariable *var);
RANT_API int    rant_variable_forced(RantVariable *var);

/* Variable events, one slot each, NULL clears. on_write fires on every applied write,
 * on_change only when the observed state changes, with a replay at registration. */
/* Without a queue both fire inline under the node lock on the thread that applied the
 * write, with the usual callback restrictions, and a set from inside a callback is safe.
 * With a queue they run at dispatch with the value of that write. See spec/patterns.md. */
typedef struct {
    RantVariable       *variable;
    RantString          name;      /* a stable view */
    RantBytes           value;     /* the value just applied, valid for the callback */
    const RantSchema *schema;      /* NULL = untyped */
    uint8_t           forced;      /* the value is a forced override */
    uint32_t          write_seq;   /* the owner's write counter */
    uint32_t          source;      /* the peer the write arrived from, 0 = a local call */
    uint64_t          recv_us;     /* the monotonic clock when the write applied */
    uint64_t          written_us;  /* the writer's wall clock, ours locally, 0 = opted out */
} RantVariableUpdate;
typedef void (*RantVariableUpdateFn)(const RantVariableUpdate *update, void *user);

RANT_API int    rant_variable_on_change(RantVariable *var, RantVariableUpdateFn on_change, void *user);
RANT_API int    rant_variable_on_write (RantVariable *var, RantVariableUpdateFn on_write,        void *user);
/* Blocks until a value exists or timeout_ms elapses (negative = forever), on the service
 * thread's progress when one runs, else driving the loop. 1 = a value, 0 = timeout or
 * from a callback. */
RANT_API int    rant_variable_wait(RantVariable *var, int timeout_ms);
/* Remote: owners matched. Definition: remotes matched. */
RANT_API int    rant_variable_match_count(RantVariable *var);
/* Parks the channels, silences the callbacks and frees the handle, invalid after. The same
 * contract as rant_function_retire. See docs/patterns.md. */
RANT_API int    rant_variable_retire(RantVariable *var);
/* As rant_function_refresh, for a reflect_from_mesh variable. */
RANT_API int    rant_variable_refresh(RantVariable *var);

#ifdef __cplusplus
}
#endif
#endif /* RANT_PATTERNS_H */
