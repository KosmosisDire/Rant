/* The patterns layer: functions, tasks and variables over dedicated topic kinds, so they
 * never cross wire with a plain topic or each other. docs/patterns.md explains the API. */
#ifndef RAMBLE_PATTERNS_H
#define RAMBLE_PATTERNS_H

#include "../common/api.h"
#include "../node/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RAMBLE_PATTERN_BP_WAIT_US
#define RAMBLE_PATTERN_BP_WAIT_US 1000000u   /* the default backpressure wait, 1 s */
#endif
#ifndef RAMBLE_CALL_TIMEOUT_US
#define RAMBLE_CALL_TIMEOUT_US 5000000u      /* the default call timeout, 5 s */
#endif

/* The response message cap. The wire carries its length in one byte, so this is fixed.
 * A longer message is truncated, never refused. */
#define RAMBLE_CALL_MSG_MAX 255u

/* functions */

/* A call's outcome. OK, APP_ERROR, NO_HANDLER, CANCELLED and RUNNING travel on the wire,
 * TIMEOUT and PEER_LOST are synthesized at the caller. See spec/patterns.md. */
typedef enum {
    RAMBLE_CALL_OK        = 0,
    RAMBLE_CALL_APP_ERROR = 1,   /* the handler replied with ramble_request_fail */
    RAMBLE_CALL_NO_HANDLER= 2,   /* the definition has no on_request */
    RAMBLE_CALL_TIMEOUT   = 3,   /* no response within the timeout */
    RAMBLE_CALL_PEER_LOST = 4,   /* the handler node dropped mid call */
    RAMBLE_CALL_CANCELLED = 5,   /* a provider honored a cancel or retired mid run, or the node
                                  closed or the handle retired with the call still pending */
    RAMBLE_CALL_RUNNING   = 6    /* task, the one non terminal status: the request runs, the
                                  timeout is dropped, on_progress fires once with no data */
} RambleCallStatus;

typedef struct RambleFunction RambleFunction;   /* an opaque handle */

/* The request as delivered to the definition's on_request. Views are valid for the
 * callback only. Pass only the exact pointer received to the reply calls, never a copy. */
typedef struct RambleRequest {
    RambleNode         *node;          /* the node the handler runs on */
    RambleString        function_name; /* the name with the @req suffix stripped */
    RambleBytes         data;          /* the request payload */
    const RambleSchema *schema;        /* the schema data decodes with, NULL = an untyped caller.
                                        Non NULL means data.len was validated against it */
    uint32_t            caller;        /* the calling peer's id */
    RambleString        caller_name;   /* the calling node's name, .data never NULL */
    uint64_t            recv_us;       /* the monotonic clock at arrival */
    uint64_t            written_us;    /* the caller's wall clock at the write, 0 = it opted out */
} RambleRequest;

/* Delivered to the caller when a response arrives or is synthesized. data is a view valid
 * for the callback only. provider is the peer that answered, 0 if synthesized. */
typedef struct {
    RambleCallStatus    status;
    RambleBytes         data;
    const RambleSchema *schema;    /* NULL = an untyped handler or a synthesized outcome */
    uint32_t          provider;
    void             *user;      /* the pointer passed to ramble_function_call_async */
    uint64_t            written_us; /* the provider's wall clock at the write, 0 if synthesized */
    RambleString        message;   /* the outcome text an HMI displays: the provider's, else the
                                    default status text. Empty only on OK, .data never NULL */
} RambleResponse;
typedef void (*RambleResponseFn)(const RambleResponse *response);

/* The handler: reply exactly once with ramble_request_reply or ramble_request_fail, or defer
 * with ramble_request_defer. Returning without replying auto acks RAMBLE_CALL_OK. */
typedef void (*RambleRequestFn)(RambleRequest *request, void *user);

typedef struct {
    uint32_t backpressure_wait_us;  /* 0 = RAMBLE_PATTERN_BP_WAIT_US */
    uint32_t timeout_us;            /* the remote side call timeout, 0 = RAMBLE_CALL_TIMEOUT_US */
    uint16_t keep_last;             /* req and rsp history depth, 0 = 10. Raise it above the most
                                       requests one pass drains, or replies past it are lost */
    uint8_t  reflect_from_mesh;     /* NULL schemas take the entity's from the mesh, and refresh
                                       re types the handle when that moves. A passed schema wins */
    uint8_t  multi;                 /* many definitions are expected, so the duplicate authority
                                       diagnostic is off. An undirected call reaches every one */
} RambleFunctionOpts;

/* One progress update as delivered to a task caller's on_progress. Views are valid for
 * the callback only. The RUNNING acknowledgment fires it once with zero length data. */
typedef struct {
    uint32_t            call_id;
    uint32_t            provider; /* the peer working the call */
    RambleBytes         data;     /* len 0 = the RUNNING ack */
    const RambleSchema *schema;   /* NULL = untyped or the RUNNING ack */
    uint64_t          written_us; /* the provider's wall clock, 0 = unstamped */
    uint64_t          recv_us;    /* the monotonic clock at arrival */
    void             *user;       /* RambleCallOpts.progress_user */
} RambleProgress;
typedef void (*RambleProgressFn)(const RambleProgress *progress);

/* Per call options, a trailing compound literal, NULL = defaults. */
typedef struct {
    uint32_t provider;              /* direct the call at this peer only, 0 = every definition and
                                       the first answer wins. A task always directs, 0 = oldest */
    RambleProgressFn on_progress;     /* task: fires per progress update on the delivering thread,
                                       NULL = updates are discarded */
    void     *progress_user;        /* RambleProgress.user */
    uint32_t *id_out;               /* filled with the call id, for ramble_function_cancel */
} RambleCallOpts;

/* Creates the definition (the body lives here, on_request NULL answers NO_HANDLER) or a
 * remote (a reference to one). Schemas may be NULL for untyped. NULL on failure. */
RAMBLE_API RambleFunction *ramble_node_create_function_definition(RambleNode *n, const char *name,
                             const RambleSchema *req_schema, const RambleSchema *rsp_schema,
                             RambleRequestFn on_request, void *user, const RambleFunctionOpts *opts);
RAMBLE_API RambleFunction *ramble_node_create_remote_function(RambleNode *n, const char *name,
                             const RambleSchema *req_schema, const RambleSchema *rsp_schema,
                             const RambleFunctionOpts *opts);

/* Calls and blocks driving the node loop. timeout_ms negative = the function's default.
 * out->data is valid until the next blocking call. docs/patterns.md has the returns. */
RAMBLE_API int  ramble_function_call(RambleFunction *fn, RambleBytes req, RambleResponse *out, int timeout_ms,
                                 const RambleCallOpts *opts);
/* Returns as soon as the request is committed, then on_response fires once with the
 * outcome. NULL = fire and forget. RAMBLE_OK or a negative RambleResult. */
RAMBLE_API int  ramble_function_call_async(RambleFunction *fn, RambleBytes req, RambleResponseFn on_response,
                                       void *user, const RambleCallOpts *opts);
/* Providers matched at a remote, callers matched at a definition. */
RAMBLE_API int  ramble_function_match_count(RambleFunction *fn);
/* Parks both channels, cancels every outstanding call with one CANCELLED outcome and frees
 * the handle. RAMBLE_ERR_STATE from a callback, the handle then stays valid. */
RAMBLE_API int  ramble_function_retire(RambleFunction *fn);
/* A reflect_from_mesh handle: re types every channel in place when the generation moved.
 * 1 re typed, 0 current, negative on error, RAMBLE_ERR_ROLE without the flag. */
RAMBLE_API int  ramble_function_refresh(RambleFunction *fn);

/* The built in @ramble/meta function every node hosts and can call, with .multi and directed
 * requests. Ask with RambleCallOpts.provider and a RAMBLE_META_* mask. NULL when disabled. */
RAMBLE_API RambleFunction *ramble_node_meta_function(RambleNode *n);

/* in the handler callback */
RAMBLE_API void      ramble_request_reply(RambleRequest *request, RambleBytes rsp);   /* answers OK */
/* Answers APP_ERROR with a human readable message (NULL = the default text, truncated at
 * RAMBLE_CALL_MSG_MAX). rsp may still carry structured failure data. */
RAMBLE_API void      ramble_request_fail (RambleRequest *request, const char *message, RambleBytes rsp);
/* Defers the reply: returns a token (0 on failure) and suppresses the auto ack. Complete it
 * later from any thread with ramble_function_complete. */
RAMBLE_API uint64_t  ramble_request_defer(RambleRequest *request);
RAMBLE_API int       ramble_function_complete(RambleFunction *fn, uint64_t token, RambleCallStatus status,
                                          const char *message, RambleBytes rsp);

/* Tasks: a function with progress and cancellation on the same handle. docs/tasks.md and
 * spec/patterns.md explain the model. */
typedef struct {
    uint8_t  progress_best_effort;  /* 0 = reliable. The definition offers, a remote requests,
                                       and the RxO rule composes them */
    uint16_t progress_keep_last;    /* the progress ring depth, 0 = the pattern default */
    uint8_t  no_cancel;             /* definition: will not honor cancellation, so remotes refuse
                                       ramble_function_cancel with RAMBLE_ERR_ROLE */
    uint8_t  exclusive;             /* definition: declared serialization, the handler enforces */
    uint8_t  multi;                 /* redundant providers intended, one executor per request */
    uint32_t timeout_us;            /* remote: the until first response bound, 0 = the default */
    uint32_t backpressure_wait_us;  /* 0 = RAMBLE_PATTERN_BP_WAIT_US */
    uint16_t keep_last;             /* req and rsp history depth, as RambleFunctionOpts.keep_last */
    uint8_t  reflect_from_mesh;     /* as RambleFunctionOpts.reflect_from_mesh, for all three */
} RambleTaskOpts;

/* Creates the definition or a remote, exactly as for a function, with prg_schema typing the
 * progress channel. The handler answers inline, or starts, defers and works the token. */
RAMBLE_API RambleFunction *ramble_node_create_task_definition(RambleNode *n, const char *name,
                             const RambleSchema *req_schema, const RambleSchema *prg_schema,
                             const RambleSchema *rsp_schema, RambleRequestFn on_request, void *user,
                             const RambleTaskOpts *opts);
RAMBLE_API RambleFunction *ramble_node_create_remote_task(RambleNode *n, const char *name,
                             const RambleSchema *req_schema, const RambleSchema *prg_schema,
                             const RambleSchema *rsp_schema, const RambleTaskOpts *opts);

/* Sends RUNNING to the caller now. Idempotent, and implied by defer on a task.
 * RAMBLE_ERR_STATE on a plain function or after a reply. */
RAMBLE_API int ramble_request_start(RambleRequest *request);
/* Token verbs, any thread. A stale token is RAMBLE_ERR_STATE. progress broadcasts on the prg
 * channel, cancelled answers 1 once a cancel arrived. Cancellation is cooperative. */
RAMBLE_API int ramble_function_progress (RambleFunction *fn, uint64_t token, RambleBytes progress);
RAMBLE_API int ramble_function_cancelled(RambleFunction *fn, uint64_t token);
/* The cancel notification, one slot per definition, NULL clears. Fires on the poll thread
 * under the usual callback restrictions. Polling ramble_function_cancelled alone is complete. */
typedef void (*RambleCancelFn)(uint64_t token, void *user);
RAMBLE_API int ramble_function_on_cancel(RambleFunction *def, RambleCancelFn on_cancel, void *user);
/* Requests cancellation of call_id, cooperative and never acked: the terminal status is the
 * answer. RAMBLE_ERR_ROLE when the provider declared no_cancel, RAMBLE_ERR_STATE when not pending. */
RAMBLE_API int ramble_function_cancel(RambleFunction *fn, uint32_t call_id);

/* Variables: replicated state with one owner, the definition, which publishes the value.
 * Writers push over a set channel with no response. Remotes cache the latest value. */
typedef struct RambleVariable RambleVariable;

typedef enum { RAMBLE_VAR_READWRITE = 0, RAMBLE_VAR_READONLY = 1 } RambleVarAccess;

typedef struct {
    RambleBytes initial;              /* definition: the value before any set, empty = none */
    uint8_t     access;              /* RambleVarAccess, READONLY creates no set channel */
    uint8_t     allow_force;         /* definition: permit force, off by default */
    uint16_t    catch_up;            /* the value channel's catch_up, 0 = 1 */
    uint16_t    keep_last;           /* both channels' history depth, the repair window for a burst of
                                      writes, 0 = 10. Raised to catch_up. See spec/patterns.md */
    uint32_t  backpressure_wait_us;/* 0 = RAMBLE_PATTERN_BP_WAIT_US */
    uint8_t   reflect_from_mesh;   /* a NULL schema takes the owner's from the mesh, and refresh
                                      re types the handle when that moves */
} RambleVariableOpts;

/* Creates the definition (this node holds the value) or a remote (reads see the cached
 * latest, writes go over the set channel). schema NULL = untyped. NULL on failure. */
RAMBLE_API RambleVariable *ramble_node_create_variable_definition(RambleNode *n, const char *name,
                                       const RambleSchema *schema, const RambleVariableOpts *opts);
RAMBLE_API RambleVariable *ramble_node_create_remote_variable(RambleNode *n, const char *name,
                                       const RambleSchema *schema, const RambleVariableOpts *opts);

/* Reads the current value into *out, a view valid until the next call on this variable or
 * the next poll. 1 if a value exists. */
RAMBLE_API int  ramble_variable_get(RambleVariable *var, RambleBytes *out);
/* Sets the value: a definition applies and publishes, a remote sends over the set channel.
 * RAMBLE_ERR_NO_TOPIC = no owner matched, RAMBLE_ERR_ROLE = a read only owner. docs/patterns.md. */
RAMBLE_API int  ramble_variable_set(RambleVariable *var, RambleBytes value);
/* Force overrides the value until unforce restores the latest absorbed set. A definition
 * needs .allow_force (RAMBLE_ERR_STATE without), a remote's force is ignored by one without. */
RAMBLE_API int  ramble_variable_force(RambleVariable *var, RambleBytes value);
RAMBLE_API int  ramble_variable_unforce(RambleVariable *var);
RAMBLE_API int  ramble_variable_forced(RambleVariable *var);

/* Variable events, one slot each, NULL clears. on_write fires on every applied write,
 * on_change only when the observed state changes, with a replay at registration. */
/* Both fire inline under the node lock on the thread that applied the write, with the usual
 * callback restrictions. A set from inside a callback is safe. See spec/patterns.md. */
typedef struct {
    RambleVariable     *variable;
    RambleString        name;      /* a stable view */
    RambleBytes         value;     /* the value just applied, valid for the callback */
    const RambleSchema *schema;    /* NULL = untyped */
    uint8_t           forced;      /* the value is a forced override */
    uint32_t          write_seq;   /* the owner's write counter */
    uint32_t          source;      /* the peer the write arrived from, 0 = a local call */
    uint64_t          recv_us;     /* the monotonic clock when the write applied */
    uint64_t          written_us;  /* the writer's wall clock, ours locally, 0 = opted out */
} RambleVariableUpdate;
typedef void (*RambleVariableUpdateFn)(const RambleVariableUpdate *update, void *user);

RAMBLE_API int  ramble_variable_on_change(RambleVariable *var, RambleVariableUpdateFn on_change, void *user);
RAMBLE_API int  ramble_variable_on_write (RambleVariable *var, RambleVariableUpdateFn on_write,  void *user);
/* Blocks driving the node loop until a value exists or timeout_ms elapses (negative =
 * forever). 1 = a value, 0 = timeout, or from a callback or under a service thread. */
RAMBLE_API int  ramble_variable_wait(RambleVariable *var, int timeout_ms);
/* Remote: owners matched. Definition: remotes matched. */
RAMBLE_API int  ramble_variable_match_count(RambleVariable *var);
/* Parks the channels, silences the callbacks and frees the handle, invalid after. The same
 * contract as ramble_function_retire. See docs/patterns.md. */
RAMBLE_API int  ramble_variable_retire(RambleVariable *var);
/* As ramble_function_refresh, for a reflect_from_mesh variable. */
RAMBLE_API int  ramble_variable_refresh(RambleVariable *var);

#ifdef __cplusplus
}
#endif
#endif /* RAMBLE_PATTERNS_H */
