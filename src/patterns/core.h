/* PATTERNS layer: functions, variables, and signals built over a DartNode. Each is a thin
 * interaction pattern over dedicated topic KINDS (transport/core.h DartTopicKind), so a
 * function/variable/signal never cross-wires with a plain topic or with each other even when
 * they share a name. Depends on node/runtime only; the node hooks it uses are kind-agnostic.
 * Compile it out with DART_NO_PATTERNS.
 *
 *   FUNCTION  request/response, exactly one reply per call, ONE handler (req/rsp channels)
 *   VARIABLE  replicated state, one owner, dumb writes + optional force (value/@set channels)
 *   SIGNAL    reliable fire-and-forget event, N emitters / N listeners, never latched
 *
 * API doctrine: every constructor is dart_node_create_*. A DEFINITION is where the body or
 * storage lives; a REMOTE is a reference to a definition on another node
 * (function_definition / remote_function, variable_definition / remote_variable: one node
 * cannot be both sides). A signal has no side to declare: passing a handler IS the
 * subscription, and every handle may emit. Bytes in C; the wrappers add typed ergonomics. */
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

/* ---- FUNCTIONS ---------------------------------------------------------------------- */

/* A call's outcome. OK/APP_ERROR/NO_HANDLER travel on the wire (the response status byte);
 * TIMEOUT/PEER_LOST are synthesized client-side when no response arrives. */
typedef enum {
    DART_CALL_OK        = 0,
    DART_CALL_APP_ERROR = 1,   /* the handler replied with dart_request_fail */
    DART_CALL_NO_HANDLER= 2,   /* the definition side has no on_request registered */
    DART_CALL_TIMEOUT   = 3,   /* client-synthesized: no response within the timeout */
    DART_CALL_PEER_LOST = 4,   /* client-synthesized: the handler node dropped mid-call */
    DART_CALL_CANCELLED = 5    /* client-synthesized: the LOCAL node closed with the call still
                                  pending (fired during dart_node_close, on the closing thread),
                                  so every call gets exactly one outcome even at close */
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
} DartResponse;
typedef void (*DartResponseFn)(const DartResponse *response);

/* The handler callback: inspect the request, then reply exactly once with
 * dart_request_reply / dart_request_fail, or dart_request_defer for an async completion.
 * Returning without replying auto-acks DART_CALL_OK with an empty payload. */
typedef void (*DartRequestFn)(DartRequest *request, void *user);

typedef struct {
    uint32_t backpressure_wait_us;  /* 0 = DART_PATTERN_BP_WAIT_US */
    uint32_t timeout_us;            /* remote-side call timeout; 0 = DART_CALL_TIMEOUT_US */
} DartFunctionOpts;

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
 * manager-owned buffer valid until the next blocking call on this function. Returns 1
 * (answered, read out->status: OK, APP_ERROR, NO_HANDLER, or PEER_LOST), 0 (timed out,
 * out->status = DART_CALL_TIMEOUT whether the local wait or the pending deadline expired
 * first), or a negative DartResult. Refused (DART_ERR_STATE) from inside a callback or while
 * a service thread owns the loop. */
int  dart_function_call(DartFunction *fn, DartBytes req, DartResponse *out, int timeout_ms);
/* The async form: returns as soon as the request is committed, then on_response (NULL =
 * fire-and-forget: use a signal instead if you truly do not care) fires once with the
 * outcome. Returns DART_OK, or a negative DartResult. */
int  dart_function_call_async(DartFunction *fn, DartBytes req, DartResponseFn on_response, void *user);
/* Providers matched (remote side) / callers matched (definition side). */
int  dart_function_match_count(DartFunction *fn);

/* ---- in the handler callback (DartRequestFn) ----------------------------------------- */
void      dart_request_reply(DartRequest *request, DartBytes rsp);   /* answer OK */
void      dart_request_fail (DartRequest *request, DartBytes rsp);   /* answer APP_ERROR */
/* Defer the reply: returns a token (0 on failure), suppresses the auto-ack, and lets the
 * handler return now. Complete it later (from any thread) with dart_function_complete. */
uint64_t  dart_request_defer(DartRequest *request);
int       dart_function_complete(DartFunction *fn, uint64_t token, DartCallStatus status, DartBytes rsp);

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
    uint32_t  backpressure_wait_us;/* 0 = DART_PATTERN_BP_WAIT_US */
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
 * channel (a read-only variable); or a negative DartResult from the send. */
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
/* Block driving the node loop until a value exists (remote first value) or timeout_ms
 * elapses (negative = forever-ish). 1 = have a value, 0 = timeout. Refused from a callback /
 * under a service thread (returns 0). */
int  dart_variable_wait(DartVariable *var, int timeout_ms);
/* Remote: owners matched (0 = no owner present). Definition: remotes matched. */
int  dart_variable_match_count(DartVariable *var);

/* ---- SIGNALS ------------------------------------------------------------------------ */

/* A reliable fire-and-forget event: N emitters, N listeners, NEVER latched (a late joiner
 * receives NOTHING published before it joined: the safety property). There is no role to
 * declare: passing a handler IS the subscription, and EVERY handle may emit. Emit interest
 * is advertised eagerly at create, so a first emit never pays an announce round trip (the
 * e-stop case). */
typedef struct DartSignal DartSignal;
typedef void (*DartSignalFn)(const DartMsg *msg, void *user);   /* a received signal */

typedef struct {
    uint32_t backpressure_wait_us;  /* 0 = DART_PATTERN_BP_WAIT_US */
} DartSignalOpts;

/* Create a signal handle. schema may be NULL (untyped / payload-less). on_signal (NULL =
 * emit-only, no subscription) fires for each signal from ANOTHER node. Returns a handle
 * or NULL. */
DartSignal *dart_node_create_signal(DartNode *n, const char *name, const DartSchema *schema,
                              DartSignalFn on_signal, void *user, const DartSignalOpts *opts);
/* Emit the signal to every matched listener (payload may be {NULL,0}). Any handle may emit.
 * Returns DART_OK, or a negative DartResult from the send. */
int  dart_signal_emit(DartSignal *sig, DartBytes payload);
/* Listeners currently matched (other nodes subscribed to this signal). */
int  dart_signal_listener_count(DartSignal *sig);

/* ---- REFLECTION (entity enumeration) -------------------------------------------------
 * The canonical way to see what exists on the network. Observers consume ENTITIES, never
 * channels: pattern channels (f@req, v@set, ...) are folded back into the function/variable/
 * signal they implement and never escape this iterator as raw topics, so no tool ever
 * reimplements the name-mangling or kind rules. Everything is derived from what the wire
 * already carries (kind bits in the announce, names + schemas from the detail cache): there
 * is no reflection protocol, and a peer built without the patterns layer reflects
 * identically. A tool built WITHOUT this layer hides pattern internals by skipping interest
 * entries whose kind != DART_KIND_TOPIC. */
typedef enum {
    DART_ENTITY_TOPIC = 0,
    DART_ENTITY_FUNCTION,
    DART_ENTITY_VARIABLE,
    DART_ENTITY_SIGNAL
} DartEntityKind;

/* One entity as advertised by a peer (or hosted locally). Views follow the same rules as
 * dart_node_peer_topic_name: valid until the next poll; bracket with dart_node_lock when a
 * poller runs on another thread. name is the BASE name with any @-mangling stripped;
 * {NULL,0} until the peer's details are fetched (show the hash-pending state, as for topics). */
typedef struct {
    DartEntityKind    kind;
    DartString        name;
    uint8_t           provides;    /* they are the data source side: topic publisher / function
                                      provider / variable owner / signal emitter */
    uint8_t           consumes;    /* they are the sink side: subscriber / caller / accessor / listener */
    uint8_t           reliable;    /* the primary channel's advertised reliability */
    uint8_t           writable;    /* VARIABLE: a @set channel is advertised alongside the value */
    uint8_t           incomplete;  /* a pattern half-pair (partner channel missing or not yet
                                      identifiable): surfaced, never silently dropped */
    uint16_t          index;       /* the primary channel's index at the peer (the key for the
                                      dart_node_peer_topic_* queries) */
    uint32_t          hash;        /* the primary channel's low-32 name hash: the placeholder an
                                      observer shows while name is still {NULL,0} (details paging) */
    const DartSchema *schema;      /* value/request/payload schema (NULL = untyped or unfetched) */
    uint64_t          schema_hash;
    const DartSchema *rsp_schema;  /* FUNCTION only: the response schema */
    uint64_t          rsp_schema_hash;
} DartEntityInfo;

/* Iterator: zero-initialize, then call until 0. Internal walk state, not for direct use. */
typedef struct { uint16_t next_index; uint8_t phase; } DartEntityIter;

/* Walk the entities a PEER advertises, one per call: plain topics pass through, pattern
 * channels fold (a function's @req/@rsp pair yields ONE function entity; a variable's @set
 * merges into its value entity as `writable`). Names and schemas come from the detail cache,
 * so an observer wanting full coverage runs with opts.fetch_details like the explorer does.
 * Returns 1 and fills *out, or 0 at the end / unknown peer. */
int dart_node_peer_entity_next(DartNode *n, uint32_t peer, DartEntityIter *it, DartEntityInfo *out);

/* Walk the entities THIS node hosts (its own functions/variables/signals, then its plain
 * topics), same shape. Local names/schemas are stable for the entity's lifetime. */
int dart_node_entity_next(DartNode *n, DartEntityIter *it, DartEntityInfo *out);

#ifdef __cplusplus
}
#endif
#endif /* DART_PATTERNS_H */
