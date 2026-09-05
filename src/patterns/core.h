/* The patterns layer: functions, tasks and variables over dedicated topic kinds, so they
 * never cross wire with a plain topic or each other. docs/patterns.md explains the API. */
#ifndef DART_PATTERNS_H
#define DART_PATTERNS_H

#include "../node/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DART_PATTERN_BP_WAIT_US
#define DART_PATTERN_BP_WAIT_US 1000000u   /* the default backpressure wait, 1 s */
#endif
#ifndef DART_CALL_TIMEOUT_US
#define DART_CALL_TIMEOUT_US 5000000u      /* the default call timeout, 5 s */
#endif

/* The response message cap. The wire carries its length in one byte, so this is fixed.
 * A longer message is truncated, never refused. */
#define DART_CALL_MSG_MAX 255u

/* functions */

/* A call's outcome. OK, APP_ERROR, NO_HANDLER, CANCELLED and RUNNING travel on the wire,
 * TIMEOUT and PEER_LOST are synthesized at the caller. See spec/patterns.md. */
typedef enum {
    DART_CALL_OK        = 0,
    DART_CALL_APP_ERROR = 1,   /* the handler replied with dart_request_fail */
    DART_CALL_NO_HANDLER= 2,   /* the definition has no on_request */
    DART_CALL_TIMEOUT   = 3,   /* no response within the timeout */
    DART_CALL_PEER_LOST = 4,   /* the handler node dropped mid call */
    DART_CALL_CANCELLED = 5,   /* a provider honored a cancel or retired mid run, or the node
                                  closed or the handle retired with the call still pending */
    DART_CALL_RUNNING   = 6    /* task, the one non terminal status: the request runs, the
                                  timeout is dropped, on_progress fires once with no data */
} DartCallStatus;

typedef struct DartFunction DartFunction;   /* an opaque handle */

/* The request as delivered to the definition's on_request. Views are valid for the
 * callback only. Pass only the exact pointer received to the reply calls, never a copy. */
typedef struct DartRequest {
    DartNode         *node;          /* the node the handler runs on */
    DartString        function_name; /* the name with the @req suffix stripped */
    DartBytes         data;          /* the request payload */
    const DartSchema *schema;        /* the schema data decodes with, NULL = an untyped caller.
                                        Non NULL means data.len was validated against it */
    uint32_t          caller;        /* the calling peer's id */
    DartString        caller_name;   /* the calling node's name, .data never NULL */
    uint64_t          recv_us;       /* the monotonic clock at arrival */
    uint64_t          written_us;    /* the caller's wall clock at the write, 0 = it opted out */
} DartRequest;

/* Delivered to the caller when a response arrives or is synthesized. data is a view valid
 * for the callback only. provider is the peer that answered, 0 if synthesized. */
typedef struct {
    DartCallStatus    status;
    DartBytes         data;
    const DartSchema *schema;    /* NULL = an untyped handler or a synthesized outcome */
    uint32_t          provider;
    void             *user;      /* the pointer passed to dart_function_call_async */
    uint64_t          written_us; /* the provider's wall clock at the write, 0 if synthesized */
    DartString        message;   /* the outcome text an HMI displays: the provider's, else the
                                    default status text. Empty only on OK, .data never NULL */
} DartResponse;
typedef void (*DartResponseFn)(const DartResponse *response);

/* The handler: reply exactly once with dart_request_reply or dart_request_fail, or defer
 * with dart_request_defer. Returning without replying auto acks DART_CALL_OK. */
typedef void (*DartRequestFn)(DartRequest *request, void *user);

typedef struct {
    uint32_t backpressure_wait_us;  /* 0 = DART_PATTERN_BP_WAIT_US */
    uint32_t timeout_us;            /* the remote side call timeout, 0 = DART_CALL_TIMEOUT_US */
    uint16_t keep_last;             /* req and rsp history depth, 0 = 10. Raise it above the most
                                       requests one pass drains, or replies past it are lost */
    uint8_t  reflect_from_mesh;     /* NULL schemas take the entity's from the mesh, and refresh
                                       re types the handle when that moves. A passed schema wins */
    uint8_t  multi;                 /* many definitions are expected, so the duplicate authority
                                       diagnostic is off. An undirected call reaches every one */
} DartFunctionOpts;

/* One progress update as delivered to a task caller's on_progress. Views are valid for
 * the callback only. The RUNNING acknowledgment fires it once with zero length data. */
typedef struct {
    uint32_t          call_id;
    uint32_t          provider;   /* the peer working the call */
    DartBytes         data;       /* len 0 = the RUNNING ack */
    const DartSchema *schema;     /* NULL = untyped or the RUNNING ack */
    uint64_t          written_us; /* the provider's wall clock, 0 = unstamped */
    uint64_t          recv_us;    /* the monotonic clock at arrival */
    void             *user;       /* DartCallOpts.progress_user */
} DartProgress;
typedef void (*DartProgressFn)(const DartProgress *progress);

/* Per call options, a trailing compound literal, NULL = defaults. */
typedef struct {
    uint32_t provider;              /* direct the call at this peer only, 0 = every definition and
                                       the first answer wins. A task always directs, 0 = oldest */
    DartProgressFn on_progress;     /* task: fires per progress update on the delivering thread,
                                       NULL = updates are discarded */
    void     *progress_user;        /* DartProgress.user */
    uint32_t *id_out;               /* filled with the call id, for dart_function_cancel */
} DartCallOpts;

/* Creates the definition (the body lives here, on_request NULL answers NO_HANDLER) or a
 * remote (a reference to one). Schemas may be NULL for untyped. NULL on failure. */
DartFunction *dart_node_create_function_definition(DartNode *n, const char *name,
                    const DartSchema *req_schema, const DartSchema *rsp_schema,
                    DartRequestFn on_request, void *user, const DartFunctionOpts *opts);
DartFunction *dart_node_create_remote_function(DartNode *n, const char *name,
                    const DartSchema *req_schema, const DartSchema *rsp_schema,
                    const DartFunctionOpts *opts);

/* Calls and blocks driving the node loop. timeout_ms negative = the function's default.
 * out->data is valid until the next blocking call. docs/patterns.md has the returns. */
int  dart_function_call(DartFunction *fn, DartBytes req, DartResponse *out, int timeout_ms,
                        const DartCallOpts *opts);
/* Returns as soon as the request is committed, then on_response fires once with the
 * outcome. NULL = fire and forget. DART_OK or a negative DartResult. */
int  dart_function_call_async(DartFunction *fn, DartBytes req, DartResponseFn on_response,
                              void *user, const DartCallOpts *opts);
/* Providers matched at a remote, callers matched at a definition. */
int  dart_function_match_count(DartFunction *fn);
/* Parks both channels, cancels every outstanding call with one CANCELLED outcome and frees
 * the handle. DART_ERR_STATE from a callback, the handle then stays valid. */
int  dart_function_retire(DartFunction *fn);
/* A reflect_from_mesh handle: re types every channel in place when the generation moved.
 * 1 re typed, 0 current, negative on error, DART_ERR_ROLE without the flag. */
int  dart_function_refresh(DartFunction *fn);

/* The built in @dart/meta function every node hosts and can call, with .multi and directed
 * requests. Ask with DartCallOpts.provider and a DART_META_* mask. NULL when disabled. */
DartFunction *dart_node_meta_function(DartNode *n);

/* in the handler callback */
void      dart_request_reply(DartRequest *request, DartBytes rsp);   /* answers OK */
/* Answers APP_ERROR with a human readable message (NULL = the default text, truncated at
 * DART_CALL_MSG_MAX). rsp may still carry structured failure data. */
void      dart_request_fail (DartRequest *request, const char *message, DartBytes rsp);
/* Defers the reply: returns a token (0 on failure) and suppresses the auto ack. Complete it
 * later from any thread with dart_function_complete. */
uint64_t  dart_request_defer(DartRequest *request);
int       dart_function_complete(DartFunction *fn, uint64_t token, DartCallStatus status,
                                 const char *message, DartBytes rsp);

/* Tasks: a function with progress and cancellation on the same handle. docs/tasks.md and
 * spec/patterns.md explain the model. */
typedef struct {
    uint8_t  progress_best_effort;  /* 0 = reliable. The definition offers, a remote requests,
                                       and the RxO rule composes them */
    uint16_t progress_keep_last;    /* the progress ring depth, 0 = the pattern default */
    uint8_t  no_cancel;             /* definition: will not honor cancellation, so remotes refuse
                                       dart_function_cancel with DART_ERR_ROLE */
    uint8_t  exclusive;             /* definition: declared serialization, the handler enforces */
    uint8_t  multi;                 /* redundant providers intended, one executor per request */
    uint32_t timeout_us;            /* remote: the until first response bound, 0 = the default */
    uint32_t backpressure_wait_us;  /* 0 = DART_PATTERN_BP_WAIT_US */
    uint16_t keep_last;             /* req and rsp history depth, as DartFunctionOpts.keep_last */
    uint8_t  reflect_from_mesh;     /* as DartFunctionOpts.reflect_from_mesh, for all three */
} DartTaskOpts;

/* Creates the definition or a remote, exactly as for a function, with prg_schema typing the
 * progress channel. The handler answers inline, or starts, defers and works the token. */
DartFunction *dart_node_create_task_definition(DartNode *n, const char *name,
                    const DartSchema *req_schema, const DartSchema *prg_schema,
                    const DartSchema *rsp_schema, DartRequestFn on_request, void *user,
                    const DartTaskOpts *opts);
DartFunction *dart_node_create_remote_task(DartNode *n, const char *name,
                    const DartSchema *req_schema, const DartSchema *prg_schema,
                    const DartSchema *rsp_schema, const DartTaskOpts *opts);

/* Sends RUNNING to the caller now. Idempotent, and implied by defer on a task.
 * DART_ERR_STATE on a plain function or after a reply. */
int dart_request_start(DartRequest *request);
/* Token verbs, any thread. A stale token is DART_ERR_STATE. progress broadcasts on the prg
 * channel, cancelled answers 1 once a cancel arrived. Cancellation is cooperative. */
int dart_function_progress (DartFunction *fn, uint64_t token, DartBytes progress);
int dart_function_cancelled(DartFunction *fn, uint64_t token);
/* The cancel notification, one slot per definition, NULL clears. Fires on the poll thread
 * under the usual callback restrictions. Polling dart_function_cancelled alone is complete. */
typedef void (*DartCancelFn)(uint64_t token, void *user);
int dart_function_on_cancel(DartFunction *def, DartCancelFn on_cancel, void *user);
/* Requests cancellation of call_id, cooperative and never acked: the terminal status is the
 * answer. DART_ERR_ROLE when the provider declared no_cancel, DART_ERR_STATE when not pending. */
int dart_function_cancel(DartFunction *fn, uint32_t call_id);

/* Variables: replicated state with one owner, the definition, which publishes the value.
 * Writers push over a set channel with no response. Remotes cache the latest value. */
typedef struct DartVariable DartVariable;

typedef enum { DART_VAR_READWRITE = 0, DART_VAR_READONLY = 1 } DartVarAccess;

typedef struct {
    DartBytes initial;              /* definition: the value before any set, empty = none */
    uint8_t   access;              /* DartVarAccess, READONLY creates no set channel */
    uint8_t   allow_force;         /* definition: permit force, off by default */
    uint16_t  catch_up;            /* the value channel's catch_up, 0 = 1 */
    uint16_t  keep_last;           /* both channels' history depth, the repair window for a burst of
                                      writes, 0 = 10. Raised to catch_up. See spec/patterns.md */
    uint32_t  backpressure_wait_us;/* 0 = DART_PATTERN_BP_WAIT_US */
    uint8_t   reflect_from_mesh;   /* a NULL schema takes the owner's from the mesh, and refresh
                                      re types the handle when that moves */
} DartVariableOpts;

/* Creates the definition (this node holds the value) or a remote (reads see the cached
 * latest, writes go over the set channel). schema NULL = untyped. NULL on failure. */
DartVariable *dart_node_create_variable_definition(DartNode *n, const char *name,
                              const DartSchema *schema, const DartVariableOpts *opts);
DartVariable *dart_node_create_remote_variable(DartNode *n, const char *name,
                              const DartSchema *schema, const DartVariableOpts *opts);

/* Reads the current value into *out, a view valid until the next call on this variable or
 * the next poll. 1 if a value exists. */
int  dart_variable_get(DartVariable *var, DartBytes *out);
/* Sets the value: a definition applies and publishes, a remote sends over the set channel.
 * DART_ERR_NO_TOPIC = no owner matched, DART_ERR_ROLE = a read only owner. docs/patterns.md. */
int  dart_variable_set(DartVariable *var, DartBytes value);
/* Force overrides the value until unforce restores the latest absorbed set. A definition
 * needs .allow_force (DART_ERR_STATE without), a remote's force is ignored by one without. */
int  dart_variable_force(DartVariable *var, DartBytes value);
int  dart_variable_unforce(DartVariable *var);
int  dart_variable_forced(DartVariable *var);

/* Variable events, one slot each, NULL clears. on_write fires on every applied write,
 * on_change only when the observed state changes, with a replay at registration. */
/* Both fire inline under the node lock on the thread that applied the write, with the usual
 * callback restrictions. A set from inside a callback is safe. See spec/patterns.md. */
typedef struct {
    DartVariable     *variable;
    DartString        name;        /* a stable view */
    DartBytes         value;       /* the value just applied, valid for the callback */
    const DartSchema *schema;      /* NULL = untyped */
    uint8_t           forced;      /* the value is a forced override */
    uint32_t          write_seq;   /* the owner's write counter */
    uint32_t          source;      /* the peer the write arrived from, 0 = a local call */
    uint64_t          recv_us;     /* the monotonic clock when the write applied */
    uint64_t          written_us;  /* the writer's wall clock, ours locally, 0 = opted out */
} DartVariableUpdate;
typedef void (*DartVariableUpdateFn)(const DartVariableUpdate *update, void *user);

int  dart_variable_on_change(DartVariable *var, DartVariableUpdateFn on_change, void *user);
int  dart_variable_on_write (DartVariable *var, DartVariableUpdateFn on_write,  void *user);
/* Blocks driving the node loop until a value exists or timeout_ms elapses (negative =
 * forever). 1 = a value, 0 = timeout, or from a callback or under a service thread. */
int  dart_variable_wait(DartVariable *var, int timeout_ms);
/* Remote: owners matched. Definition: remotes matched. */
int  dart_variable_match_count(DartVariable *var);
/* Parks the channels, silences the callbacks and frees the handle, invalid after. The same
 * contract as dart_function_retire. See docs/patterns.md. */
int  dart_variable_retire(DartVariable *var);
/* As dart_function_refresh, for a reflect_from_mesh variable. */
int  dart_variable_refresh(DartVariable *var);

#ifdef __cplusplus
}
#endif
#endif /* DART_PATTERNS_H */
