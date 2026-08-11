/* PATTERNS layer implementation: functions, variables, signals, and the entity reflection
 * walk. Built entirely on the node's public API plus the kind-agnostic i_dart_node_* seams
 * (create a pattern topic, send with a header / directed, observe events + a per-poll tick).
 * The node knows nothing of what these patterns mean.
 *
 * Locking rule: manager state mutates under the node lock (i_dart_node_sys_lock), but the
 * SEND happens outside it, through the entry points that take the lock themselves: that is
 * what lets a pattern send engage the normal flow-control wait (backpressure) instead of
 * committing reentrantly. The payload for those sends is always the APPLICATION'S buffer
 * (valid for the whole API call by contract), never manager memory, so nothing the wait
 * releases the lock around can be reallocated underneath the send. The exceptions publish
 * under a held lock (reentrant, no wait, per the from-a-callback send rules): replies built
 * inside a delivery callback, and the owner's force/unforce republish from the shadow. */
#include "core.h"
#include "../common/bytes.h"   /* i_dart_le_* pack/unpack */
#include <string.h>
#include <stddef.h>            /* offsetof */

/* ---- per-node manager: fans the node-wide event + tick out to every entity -------------- */
typedef struct i_DartPatterns {
    DartNode            *n;
    struct DartFunction *funcs;   /* linked lists, for tick/event fanout + local reflection */
    struct DartVariable *vars;
    struct DartSignal   *sigs;
    struct DartFunction *meta;    /* the built-in @dart/meta endpoint (both sides in one handle) */
    DartSchema          *meta_rsp_schema;   /* DartMeta { info: map } */
    uint8_t             *meta_msg; uint32_t meta_msg_cap;   /* reply scratch, grown on demand */
} i_DartPatterns;

static void     i_dart_patterns_on_event(void *user, const DartEvent *ev);
static uint64_t i_dart_patterns_tick(void *user, uint64_t now_us);
static void     i_dart_patterns_on_close(void *user);
/* pending-call reaper (defined with the manager hooks; retire cancels through it) */
static uint64_t i_dart_func_reap(struct DartFunction *fn, uint64_t now,
                                 DartCallStatus fail_status, int all);
/* duplicate-authority sweep for a just-created provider/owner (defined with the rest of
 * the detection, after both entity structs) */
static void i_dart_pat_dup_sweep(DartNode *n, DartTopic *primary, uint8_t kind,
                                 int authority_is_pub, uint32_t **ids, uint16_t *n_ids,
                                 uint16_t *cap);

/* Lazily create the manager and register the node-wide sys hooks. Lock held (a create call
 * took it). NULL on OOM. */
static i_DartPatterns *i_dart_patterns_get(DartNode *n){
    void **slot = i_dart_node_sys_slot(n);
    i_DartPatterns *pm = (i_DartPatterns*)*slot;
    if (pm) return pm;
    pm = (i_DartPatterns*)i_dart_node_sys_alloc(n, NULL, sizeof *pm);
    if (!pm) return NULL;
    memset(pm, 0, sizeof *pm);
    pm->n = n; *slot = pm;
    i_dart_node_set_sys_hooks(n, i_dart_patterns_on_event, i_dart_patterns_tick,
                              i_dart_patterns_on_close, pm);
    return pm;
}

/* ---- FUNCTIONS -------------------------------------------------------------------------- */

#define DART__FN_PREFIX 5u   /* req: [u32 call_id][u8 flags]. rsp: [u32 call_id][u8 status]
                                then [u8 msg_len][msg] (the response message: the node's
                                split knows the FUNC_RSP framing, so all of it lands in
                                DartMsg.header and the payload stays the schema's) */
#define DART__FN_RSP_HDR_MAX (DART__FN_PREFIX + 1u + DART_CALL_MSG_MAX)

/* default response-message text: what DartResponse.message shows when the provider sent
 * none (or the outcome was synthesized locally), so a generic consumer always has text
 * to display for a failure. OK stays empty. All static: no lifetime to manage. */
static DartString i_dart_call_status_msg(DartCallStatus s){
    switch (s){
    case DART_CALL_APP_ERROR:  return dart_cstr("app error");
    case DART_CALL_NO_HANDLER: return dart_cstr("no handler");
    case DART_CALL_TIMEOUT:    return dart_cstr("timeout");
    case DART_CALL_PEER_LOST:  return dart_cstr("peer lost");
    case DART_CALL_CANCELLED:  return dart_cstr("cancelled");
    case DART_CALL_OK: default: return dart_cstr("");
    }
}

/* a caller-side outstanding call. A call made BEFORE any provider matched is not handed to
 * the transport (an unmatched reliable channel drops the send: nobody would ever replay it);
 * the request bytes queue here and flush the moment the match forms, so the first call after
 * open never silently times out. queued_req != NULL = not yet on the wire. */
typedef struct i_DartPending {
    struct i_DartPending *next;
    uint32_t        call_id;
    uint32_t        dest;        /* DartCallOpts.provider: the one peer this call is directed
                                    at (0 = undirected); its drop fails the call immediately */
    uint64_t        deadline_us;
    DartResponseFn  on_response;
    void           *user;
    uint8_t        *queued_req;
    uint32_t        queued_len;
} i_DartPending;

/* a deferred provider reply (dart_request_defer -> token -> dart_function_complete) */
typedef struct i_DartDefer {
    DartFunction *fn;
    uint32_t      caller;
    uint32_t      call_id;
} i_DartDefer;

struct DartFunction {
    DartNode       *n;
    i_DartPatterns *pm;
    struct DartFunction *next;      /* manager list */
    DartTopic      *req;            /* provider: SUB_ONLY; caller: PUB_ONLY (kind FUNC_REQ) */
    DartTopic      *rsp;            /* provider: PUB_ONLY; caller: SUB_ONLY (kind FUNC_RSP, directed) */
    DartRequestFn   on_request; void *on_request_user;   /* provider */
    uint32_t        next_call_id;   /* caller counter */
    uint32_t        timeout_us;
    i_DartPending  *pending;        /* caller: outstanding calls */
    uint8_t        *sync_buf; uint32_t sync_cap;   /* sync-call reply scratch (view lifetime) */
    char            sync_msg[DART_CALL_MSG_MAX];   /* sync-call response-message scratch */
    uint8_t         sync_msg_len;
    uint8_t         is_provider;    /* a pure DEFINITION (a both-sides handle is 0 here, so
                                       every caller-side path covers it) */
    uint8_t         both;           /* both sides in one handle (the @dart/meta shape):
                                       PUBSUB channels, a handler AND a pending list */
    uint8_t         multi;          /* DartFunctionOpts.multi: duplicate authority expected */
    uint32_t       *dup_peers;      /* provider: peers already reported for duplicate authority */
    uint16_t        dup_n, dup_cap;
};

/* the transient request handed to the provider's handler: the public read-only head
 * (core.h DartRequest) first, the reply machinery behind it. The reply entry points
 * downcast the handler's DartRequest* back to this, so it must be the first member. */
typedef struct {
    DartRequest   pub;
    DartFunction *fn;
    uint32_t      call_id;
    uint8_t       replied;   /* a reply/fail/defer already happened: suppress the auto-ack */
} i_DartRequest;
/* the downcast requires pub at offset 0 (C99: no _Static_assert) */
typedef char i_dart_request_pub_first[(offsetof(i_DartRequest, pub) == 0) ? 1 : -1];

/* build the response header: [call_id][status][u8 msg_len][msg]. message may be NULL
 * (msg_len 0); longer than DART_CALL_MSG_MAX truncates, never refuses. Returns the
 * header length. hdr must hold DART__FN_RSP_HDR_MAX. */
static size_t i_dart_func_rsp_hdr(uint8_t *hdr, uint32_t call_id, uint8_t status,
                                  const char *message){
    size_t ml = message ? strlen(message) : 0;
    if (ml > DART_CALL_MSG_MAX) ml = DART_CALL_MSG_MAX;
    i_dart_le_w32(hdr, call_id); hdr[4] = status; hdr[5] = (uint8_t)ml;
    if (ml) memcpy(hdr + 6, message, ml);
    return DART__FN_PREFIX + 1u + ml;
}

/* send a response directly to one caller: [call_id][status][msg] header, rsp payload. Runs
 * from a delivery callback or a held-lock context: the send commits reentrantly (no wait). */
static void i_dart_func_send_reply(DartFunction *fn, uint32_t caller, uint32_t call_id,
                                   uint8_t status, const char *message, DartBytes rsp){
    uint8_t hdr[DART__FN_RSP_HDR_MAX];
    size_t hl = i_dart_func_rsp_hdr(hdr, call_id, status, message);
    (void)i_dart_topic_send_to(fn->rsp, caller, dart_bytes(hdr, hl), rsp);
}

/* provider: a request arrived on the req channel. The public head carries what the DartMsg
 * does, in function vocabulary (the topic name minus its "@req" suffix, publisher = caller). */
static void i_dart_func_on_request(void *user, const DartMsg *msg){
    DartFunction *fn = (DartFunction*)user;
    i_DartRequest r;
    if (msg->header.len < DART__FN_PREFIX) return;   /* malformed prefix */
    r.pub.node          = msg->node;
    r.pub.function_name = dart_string(msg->topic_name.data,
                              msg->topic_name.len > 4u ? msg->topic_name.len - 4u : 0);
    r.pub.data          = msg->data;
    r.pub.schema        = msg->schema;
    r.pub.caller        = msg->publisher_id;
    r.pub.caller_name   = msg->publisher_name;
    r.pub.recv_us       = msg->recv_us;
    r.pub.written_us    = msg->written_us;
    r.fn = fn; r.call_id = i_dart_le_r32(msg->header.data); r.replied = 0;
    if (fn->on_request) fn->on_request(&r.pub, fn->on_request_user);
    else {   /* no handler: NO_HANDLER is the answer, not the auto-ack too (no message on
                the wire: the caller side fills the default status text) */
        i_dart_func_send_reply(fn, r.pub.caller, r.call_id, DART_CALL_NO_HANDLER, NULL, dart_bytes(NULL,0));
        r.replied = 1;
    }
    if (!r.replied)   /* handler returned without replying/deferring: auto-ack OK, empty */
        i_dart_func_send_reply(fn, r.pub.caller, r.call_id, DART_CALL_OK, NULL, dart_bytes(NULL,0));
}

/* caller: unlink the pending entry for call_id (dedup: a second provider's reply finds none) */
static i_DartPending *i_dart_func_take_pending(DartFunction *fn, uint32_t call_id){
    i_DartPending **pp = &fn->pending, *p;
    for (; (p = *pp) != NULL; pp = &p->next)
        if (p->call_id == call_id){ *pp = p->next; return p; }
    return NULL;
}

/* free an unlinked pending entry (and any still-queued request bytes) */
static void i_dart_func_free_pending(DartFunction *fn, i_DartPending *p){
    if (p->queued_req) i_dart_node_sys_alloc(fn->n, p->queued_req, 0);
    i_dart_node_sys_alloc(fn->n, p, 0);
}

/* send every call queued before a provider matched, oldest first (the list is newest-first).
 * Runs under the node lock (the tick / the interest event), so the sends commit reentrantly:
 * fine, a fresh lane has an empty history. */
static void i_dart_func_flush_queued(DartFunction *fn){
    if (fn->is_provider || !fn->pending) return;
    if (dart_topic_match_count(fn->req) == 0) return;   /* still no provider */
    for (;;){
        i_DartPending *pick = NULL, *p;
        uint8_t hdr[DART__FN_PREFIX];
        for (p = fn->pending; p; p = p->next) if (p->queued_req) pick = p;
        if (!pick) return;
        i_dart_le_w32(hdr, pick->call_id); hdr[4] = 0;
        if (pick->dest)
            (void)i_dart_topic_send_to(fn->req, pick->dest, dart_bytes(hdr, DART__FN_PREFIX),
                                       dart_bytes(pick->queued_req, pick->queued_len));
        else
            (void)i_dart_topic_send_hdr(fn->req, dart_bytes(hdr, DART__FN_PREFIX),
                                        dart_bytes(pick->queued_req, pick->queued_len));
        i_dart_node_sys_alloc(fn->n, pick->queued_req, 0);
        pick->queued_req = NULL; pick->queued_len = 0;
    }
}

/* caller: a response arrived on the rsp channel */
static void i_dart_func_on_response(void *user, const DartMsg *msg){
    DartFunction *fn = (DartFunction*)user;
    i_DartPending *p;
    DartResponse r;
    if (msg->header.len < DART__FN_PREFIX) return;
    p = i_dart_func_take_pending(fn, i_dart_le_r32(msg->header.data));
    if (!p) return;                          /* unknown/duplicate call_id: dropped */
    r.status = (DartCallStatus)msg->header.data[4];
    r.data = msg->data; r.schema = msg->schema;
    r.provider = msg->publisher_id; r.user = p->user;
    r.written_us = msg->written_us;
    /* the response message follows the fixed prefix as [u8 len][msg]; the node's split
       already bounded it, so the header length is the truth (not the len byte). A wire
       message wins; none = the default status text, so message always displays. */
    r.message = (msg->header.len > DART__FN_PREFIX + 1u)
              ? dart_string((const char*)msg->header.data + DART__FN_PREFIX + 1u,
                            msg->header.len - (DART__FN_PREFIX + 1u))
              : i_dart_call_status_msg(r.status);
    if (p->on_response) p->on_response(&r);
    i_dart_func_free_pending(fn, p);
}

/* a leading '@' is reserved for the "@dart/" builtins: refused in every public
   constructor so an application entity can never land in the hidden namespace */
static int i_dart_pat_reserved(const char *name){ return name && name[0] == '@'; }

/* Create both channels for a function; roles per mode. mode 1 = a pure DEFINITION
 * (SUB req / PUB rsp), 0 = a REMOTE (PUB req / SUB rsp), 2 = BOTH SIDES in one handle
 * (PUBSUB channels, a handler and a pending list: the @dart/meta shape). Both-sides
 * requests are DIRECTED, so a call aimed at one definition never wakes the others, and
 * the channels keep a shallow ring (calls carry no replay). */
static DartFunction *i_dart_function_new(DartNode *n, const char *name,
                    const DartSchema *req_schema, const DartSchema *rsp_schema,
                    DartRequestFn on_request, void *user, const DartFunctionOpts *opts, int mode){
    i_DartPatterns *pm; DartFunction *fn;
    char rn[DART_TOPIC_NAME_MAX + 1]; size_t nl;
    DartTopicOpts topt; int acquired;
    int handles_req = (mode != 0), makes_calls = (mode != 1);
    DartRole req_role = mode == 2 ? DART_PUBSUB : (handles_req ? DART_SUB_ONLY : DART_PUB_ONLY);
    DartRole rsp_role = mode == 2 ? DART_PUBSUB : (handles_req ? DART_PUB_ONLY : DART_SUB_ONLY);
    i_DartSysMsgFn req_cb = handles_req ? i_dart_func_on_request : NULL;
    i_DartSysMsgFn rsp_cb = makes_calls ? i_dart_func_on_response : NULL;
    if (!n || !name) return NULL;
    nl = strlen(name);
    if (nl == 0 || nl + 4 > DART_TOPIC_NAME_MAX) return NULL;   /* room for the "@req"/"@rsp" suffix */
    memset(&topt, 0, sizeof topt);
    topt.qos.reliability = DART_RELIABLE;
    topt.qos.catch_up = 0;
    /* shallow ring: calls carry no replay (catch_up 0), so history is only the repair
       window; a deep ring would pin keep_last x request/reply size per function. More
       than keep_last un-acked in-flight calls engage backpressure, never loss. The
       both-sides meta shape stays at 2; plain functions get room for small bursts. */
    topt.qos.keep_last = (mode == 2) ? 2 : 4;
    topt.qos.backpressure_wait_us = (opts && opts->backpressure_wait_us) ? opts->backpressure_wait_us
                                                                         : DART_PATTERN_BP_WAIT_US;
    /* Allocate + init the handle under the lock, then RELEASE it before creating the topics:
       i_dart_node_create_pattern_topic takes the node lock itself and refuses if it is already
       held (it reads that as a from-a-callback reentry). The handle must exist first so it can
       be the topics' sys_on_message user. */
    acquired = i_dart_node_sys_lock(n);
    pm = i_dart_patterns_get(n);
    fn = pm ? (DartFunction*)i_dart_node_sys_alloc(n, NULL, sizeof *fn) : NULL;
    if (fn){
        memset(fn, 0, sizeof *fn);
        fn->n = n; fn->pm = pm; fn->is_provider = (uint8_t)(mode == 1);
        fn->both = (uint8_t)(mode == 2);
        fn->multi = (uint8_t)(opts && opts->multi);
        fn->on_request = on_request; fn->on_request_user = user;
        fn->timeout_us = (opts && opts->timeout_us) ? opts->timeout_us : DART_CALL_TIMEOUT_US;
        fn->next_call_id = 1;
    }
    i_dart_node_sys_unlock(n, acquired);
    if (!fn) return NULL;

    memcpy(rn, name, nl); memcpy(rn + nl, "@req", 5);   /* NUL included */
    fn->req = i_dart_node_create_pattern_topic(n, rn, req_role, req_schema, &topt,
                              DART_KIND_FUNC_REQ, DART__FN_PREFIX, (uint8_t)(mode == 2), 0, req_cb, fn);
    if (!fn->req) return NULL;   /* fn stays pool-allocated: nothing routes into it yet */
    memcpy(rn + nl, "@rsp", 5);
    fn->rsp = i_dart_node_create_pattern_topic(n, rn, rsp_role, rsp_schema, &topt,
                              DART_KIND_FUNC_RSP, DART__FN_PREFIX, 1 /*directed*/, 0, rsp_cb, fn);
    if (!fn->rsp){
        /* Partial create: topics cannot be destroyed and fn->req still ROUTES deliveries to
           fn, so the handle must stay allocated (the pool reclaims it at close). Deactivate
           the half so it stops advertising; a stray delivery in the window finds valid
           memory and a reply through the NULL rsp topic fails harmlessly. */
        dart_topic_set_role(fn->req, DART_INACTIVE);
        return NULL;
    }

    acquired = i_dart_node_sys_lock(n);       /* publish into the manager list (tick/event fanout) */
    fn->next = pm->funcs; pm->funcs = fn;
    if (handles_req && !fn->multi)   /* a rival provider may already be on the network */
        i_dart_pat_dup_sweep(n, fn->req, DART_KIND_FUNC_REQ, 0,
                             &fn->dup_peers, &fn->dup_n, &fn->dup_cap);
    i_dart_node_sys_unlock(n, acquired);
    return fn;
}

DartFunction *dart_node_create_function_definition(DartNode *n, const char *name,
                    const DartSchema *req_schema, const DartSchema *rsp_schema,
                    DartRequestFn on_request, void *user, const DartFunctionOpts *opts){
    if (i_dart_pat_reserved(name)) return NULL;
    return i_dart_function_new(n, name, req_schema, rsp_schema, on_request, user, opts, 1);
}
DartFunction *dart_node_create_remote_function(DartNode *n, const char *name,
                    const DartSchema *req_schema, const DartSchema *rsp_schema,
                    const DartFunctionOpts *opts){
    if (i_dart_pat_reserved(name)) return NULL;
    return i_dart_function_new(n, name, req_schema, rsp_schema, NULL, NULL, opts, 0);
}

/* Link the pending entry under the lock, then send OUTSIDE it so the request engages the
 * normal flow-control wait (req is the caller's buffer, stable across the wait). The entry
 * must be linked before the send: a response can arrive the moment the datagram is out. */
static int i_dart_function_call_id(DartFunction *fn, DartBytes req, DartResponseFn on_response,
                                   void *user, uint32_t dest, uint32_t *id_out){
    uint8_t hdr[DART__FN_PREFIX]; i_DartPending *p; int acquired, r; uint32_t id;
    if (!fn || !fn->req || !fn->rsp) return DART_ERR_NO_TOPIC;
    acquired = i_dart_node_sys_lock(fn->n);
    p = (i_DartPending*)i_dart_node_sys_alloc(fn->n, NULL, sizeof *p);
    if (!p){ i_dart_node_sys_unlock(fn->n, acquired); return DART_ERR_OOM; }
    id = fn->next_call_id++;
    p->call_id = id;
    p->dest = dest;
    p->deadline_us = i_dart_node_now_us(fn->n) + fn->timeout_us;
    p->on_response = on_response; p->user = user;
    p->queued_req = NULL; p->queued_len = 0;
    p->next = fn->pending; fn->pending = p;
    if (dart_topic_match_count(fn->req) == 0){
        /* no provider matched yet (the announce/detail cycle after open takes a beat, or the
           provider is late): the transport would DROP an unmatched send, so QUEUE the request
           and flush the instant the match forms. Times out normally if none ever does. */
        p->queued_req = (uint8_t*)i_dart_node_sys_alloc(fn->n, NULL, req.len ? req.len : 1u);
        if (!p->queued_req){
            fn->pending = p->next;
            i_dart_node_sys_alloc(fn->n, p, 0);
            i_dart_node_sys_unlock(fn->n, acquired);
            return DART_ERR_OOM;
        }
        if (req.len) memcpy(p->queued_req, req.data, req.len);
        p->queued_len = (uint32_t)req.len;
        i_dart_node_sys_unlock(fn->n, acquired);
        if (id_out) *id_out = id;
        return DART_OK;
    }
    i_dart_node_sys_unlock(fn->n, acquired);
    i_dart_le_w32(hdr, id); hdr[4] = 0;   /* flags reserved */
    r = dest ? i_dart_topic_send_to(fn->req, dest, dart_bytes(hdr, DART__FN_PREFIX), req)
             : i_dart_topic_send_hdr(fn->req, dart_bytes(hdr, DART__FN_PREFIX), req);
    if (r != DART_OK){
        acquired = i_dart_node_sys_lock(fn->n);
        p = i_dart_func_take_pending(fn, id);   /* may already be reaped/answered: then gone */
        if (p) i_dart_func_free_pending(fn, p);
        i_dart_node_sys_unlock(fn->n, acquired);
        return r;
    }
    if (id_out) *id_out = id;
    return DART_OK;
}

int dart_function_call_async(DartFunction *fn, DartBytes req, DartResponseFn on_response,
                             void *user, const DartCallOpts *opts){
    return i_dart_function_call_id(fn, req, on_response, user, opts ? opts->provider : 0, NULL);
}

int dart_function_retire(DartFunction *fn){
    i_DartPatterns *pm; DartFunction **pp; DartNode *n; int acquired, r;
    if (!fn) return DART_ERR_NO_TOPIC;
    pm = fn->pm; n = fn->n;
    if (pm && fn == pm->meta) return DART_ERR_STATE;   /* the builtin endpoint is node infrastructure */
    /* Park the channels FIRST: set_role refuses from a callback (DART_ERR_STATE, nothing
       mutated yet), and on success it rematches under the node lock, so once both flips
       land nothing can deliver into fn again. */
    r = dart_topic_set_role(fn->req, DART_INACTIVE);
    if (r != 0) return r;
    (void)dart_topic_set_role(fn->rsp, DART_INACTIVE);
    acquired = i_dart_node_sys_lock(n);
    i_dart_topic_clear_sys(fn->req);
    i_dart_topic_clear_sys(fn->rsp);
    /* every outstanding call gets its one outcome, CANCELLED, exactly as at close; a
       reentrant call made from a cancel callback queues (the channel is unmatched now)
       and the next round cancels it too */
    while (fn->pending) i_dart_func_reap(fn, 0, DART_CALL_CANCELLED, 1);
    if (pm)
        for (pp = &pm->funcs; *pp; pp = &(*pp)->next)
            if (*pp == fn){ *pp = fn->next; break; }
    if (fn->sync_buf)  i_dart_node_sys_alloc(n, fn->sync_buf, 0);
    if (fn->dup_peers) i_dart_node_sys_alloc(n, fn->dup_peers, 0);
    i_dart_node_sys_alloc(n, fn, 0);
    i_dart_node_sys_unlock(n, acquired);
    return DART_OK;
}

/* blocking-call response capture: copy the payload into the function's scratch, mark done.
 * The schema pointer is safe to hold: an interned schema lives until node close. */
typedef struct { DartFunction *fn; volatile int done; DartCallStatus status;
                 const DartSchema *schema; uint32_t len; uint32_t provider;
                 uint64_t written_us; } i_DartSyncCtx;
static void i_dart_func_sync_response(const DartResponse *r){
    i_DartSyncCtx *c = (i_DartSyncCtx*)r->user;
    DartFunction *fn = c->fn;
    c->status = r->status; c->schema = r->schema; c->len = (uint32_t)r->data.len;
    c->provider = r->provider; c->written_us = r->written_us;
    {   /* the message view dies with the callback: copy into the handle's scratch */
        size_t ml = r->message.len <= DART_CALL_MSG_MAX ? r->message.len : DART_CALL_MSG_MAX;
        if (ml) memcpy(fn->sync_msg, r->message.data, ml);
        fn->sync_msg_len = (uint8_t)ml;
    }
    if (r->data.len){
        if (fn->sync_cap < r->data.len){
            uint8_t *nb = (uint8_t*)i_dart_node_sys_alloc(fn->n, fn->sync_buf, r->data.len);
            if (nb){ fn->sync_buf = nb; fn->sync_cap = (uint32_t)r->data.len; }
            else c->len = 0;
        }
        if (fn->sync_cap >= r->data.len) memcpy(fn->sync_buf, r->data.data, r->data.len);
    }
    c->done = 1;
}

int dart_function_call(DartFunction *fn, DartBytes req, DartResponse *out, int timeout_ms,
                       const DartCallOpts *opts){
    i_DartSyncCtx ctx; int acquired, r; uint64_t deadline; uint32_t id;
    if (!fn) return DART_ERR_NO_TOPIC;
    acquired = i_dart_node_sys_lock(fn->n);
    if (!acquired || dart_node_is_started(fn->n)){   /* from a callback, or a service thread owns the loop */
        i_dart_node_sys_unlock(fn->n, acquired);
        return DART_ERR_STATE;
    }
    i_dart_node_sys_unlock(fn->n, acquired);
    ctx.fn = fn; ctx.done = 0; ctx.status = DART_CALL_TIMEOUT; ctx.schema = NULL; ctx.len = 0;
    ctx.provider = 0; ctx.written_us = 0;
    r = i_dart_function_call_id(fn, req, i_dart_func_sync_response, &ctx,
                                opts ? opts->provider : 0, &id);
    if (r != DART_OK) return r;
    deadline = i_dart_node_now_us(fn->n)
             + (uint64_t)(timeout_ms >= 0 ? (uint64_t)timeout_ms * 1000u : fn->timeout_us) + 20000u;
    while (!ctx.done){
        if (i_dart_node_now_us(fn->n) >= deadline) break;
        i_dart_node_sys_poll(fn->n, 5);            /* the tick synthesizes TIMEOUT at the pending deadline */
    }
    if (!ctx.done){
        /* Local wait expired first: UNLINK the pending entry before this stack frame dies,
           or the tick reap / a late real response would fire i_dart_func_sync_response into
           a reclaimed frame. Under the lock a racing poller may have completed it meanwhile:
           then ctx.done flipped and the response stands. */
        i_DartPending *p;
        acquired = i_dart_node_sys_lock(fn->n);
        if (!ctx.done && (p = i_dart_func_take_pending(fn, id)) != NULL)
            i_dart_node_sys_alloc(fn->n, p, 0);
        i_dart_node_sys_unlock(fn->n, acquired);
    }
    if (out){
        memset(out, 0, sizeof *out);
        out->status = ctx.done ? ctx.status : DART_CALL_TIMEOUT;
        out->data = dart_bytes(fn->sync_buf,
                               (ctx.done && ctx.status != DART_CALL_TIMEOUT) ? ctx.len : 0);
        out->schema = out->data.len ? ctx.schema : NULL;
        out->provider = ctx.done ? ctx.provider : 0;
        out->written_us = ctx.done ? ctx.written_us : 0;
        /* same lifetime rule as data: the scratch holds it until the next blocking call */
        out->message = ctx.done ? dart_string(fn->sync_msg, fn->sync_msg_len)
                                : i_dart_call_status_msg(DART_CALL_TIMEOUT);
    }
    /* 0 = timed out, whichever deadline (local or pending) expired first; 1 = a real outcome */
    return (ctx.done && ctx.status != DART_CALL_TIMEOUT) ? 1 : 0;
}

int dart_function_match_count(DartFunction *fn){
    if (!fn) return 0;
    return dart_topic_match_count(fn->is_provider ? fn->rsp : fn->req);
}

/* ---- provider handler API ----------------------------------------------------------------
 * The handler's DartRequest* is the pub head of an i_DartRequest (first member), so these
 * recover the reply machinery by downcast. A copy of the public struct answers nothing. */
static void i_dart_request_answer(DartRequest *request, uint8_t status, const char *message,
                                  DartBytes rsp){
    i_DartRequest *r = (i_DartRequest*)request;
    if (!request || r->replied) return;
    r->replied = 1;
    i_dart_func_send_reply(r->fn, request->caller, r->call_id, status, message, rsp);
}
void dart_request_reply(DartRequest *request, DartBytes rsp){
    i_dart_request_answer(request, DART_CALL_OK, NULL, rsp);
}
void dart_request_fail (DartRequest *request, const char *message, DartBytes rsp){
    i_dart_request_answer(request, DART_CALL_APP_ERROR, message, rsp);
}

uint64_t dart_request_defer(DartRequest *request){
    i_DartRequest *r = (i_DartRequest*)request;
    i_DartDefer *d;
    if (!request || r->replied) return 0;
    d = (i_DartDefer*)i_dart_node_sys_alloc(r->fn->n, NULL, sizeof *d);
    if (!d) return 0;
    d->fn = r->fn; d->caller = request->caller; d->call_id = r->call_id;
    r->replied = 1;   /* suppress the auto-ack; the reply comes via dart_function_complete */
    return (uint64_t)(uintptr_t)d;
}

int dart_function_complete(DartFunction *fn, uint64_t token, DartCallStatus status,
                           const char *message, DartBytes rsp){
    i_DartDefer *d = (i_DartDefer*)(uintptr_t)token;
    uint8_t hdr[DART__FN_RSP_HDR_MAX]; size_t hl; DartTopic *rsp_topic; uint32_t caller;
    int acquired;
    if (!fn || !d) return DART_ERR_NO_TOPIC;
    acquired = i_dart_node_sys_lock(fn->n);
    hl = i_dart_func_rsp_hdr(hdr, d->call_id, (uint8_t)status, message);
    rsp_topic = d->fn->rsp; caller = d->caller;
    i_dart_node_sys_alloc(fn->n, d, 0);
    i_dart_node_sys_unlock(fn->n, acquired);
    /* send outside the lock: a deferred completion from an app thread engages backpressure
       (rsp is the app's buffer); from a callback it commits reentrantly as usual */
    return i_dart_topic_send_to(rsp_topic, caller, dart_bytes(hdr, hl), rsp);
}

/* ---- the built-in @dart/meta endpoint (patterns/core.h) ----------------------------------
 * Hosted at node open (the runtime's i_dart_patterns_meta_open seam) as a BOTH-SIDES multi
 * function: every node is a definition (serving its own i_dart_node_snapshot) and a remote
 * (calling any peer's, directed by DartCallOpts.provider). The node builds the snapshot;
 * this layer only wires it into the function machinery. */

/* node-pool DartAllocFn adapter (the schema builders take the generic hook shape) */
static void *i_dart_pat_alloc(void *user, void *ptr, size_t size){
    return i_dart_node_sys_alloc((DartNode*)user, ptr, size);
}

static void i_dart_meta_on_request(DartRequest *request, void *user){
    DartNode *n = (DartNode*)user;
    i_DartPatterns *pm = (i_DartPatterns*)*i_dart_node_sys_slot(n);
    uint32_t mask = 0, need;
    DartBytes body;
    if (!pm || !pm->meta_rsp_schema){ dart_request_fail(request, "meta schema unavailable", dart_bytes(NULL, 0)); return; }
    if (request->data.len >= 4) mask = i_dart_le_r32(request->data.data);   /* empty = everything */
    body = i_dart_node_snapshot(n, mask);
    if (!body.data){ dart_request_fail(request, "snapshot failed", dart_bytes(NULL, 0)); return; }
    need = dart_schema_msg_min(pm->meta_rsp_schema) + (uint32_t)body.len;
    if (pm->meta_msg_cap < need){
        uint8_t *nb = (uint8_t*)i_dart_node_sys_alloc(n, pm->meta_msg, need);
        if (!nb){ dart_request_fail(request, "out of memory", dart_bytes(NULL, 0)); return; }
        pm->meta_msg = nb; pm->meta_msg_cap = need;
    }
    if (!dart_schema_message_default(pm->meta_rsp_schema, pm->meta_msg, pm->meta_msg_cap)
        || !dart_set_map(pm->meta_msg, pm->meta_msg_cap, pm->meta_rsp_schema, "info", body)){
        dart_request_fail(request, "meta encode failed", dart_bytes(NULL, 0));
        return;
    }
    dart_request_reply(request, dart_bytes(pm->meta_msg,
                dart_schema_msg_len(pm->meta_rsp_schema, pm->meta_msg, pm->meta_msg_cap)));
}

/* the node-open seam (declared in node/runtime.c): host @dart/meta on this node */
void i_dart_patterns_meta_open(DartNode *n){
    i_DartPatterns *pm;
    DartSchema *rsp;
    DartFunctionOpts fo;
    int acquired;
    if (!n) return;
    acquired = i_dart_node_sys_lock(n);
    pm = i_dart_patterns_get(n);
    if (pm && !pm->meta_rsp_schema)
        pm->meta_rsp_schema = dart_schema_compile(i_dart_pat_alloc, n, "DartMeta { info: map }", NULL);
    rsp = pm ? pm->meta_rsp_schema : NULL;
    i_dart_node_sys_unlock(n, acquired);
    if (!pm || pm->meta || !rsp) return;   /* OOM degrades: no endpoint, node still healthy */
    memset(&fo, 0, sizeof fo);
    fo.multi = 1;                          /* every node hosting one is the design */
    pm->meta = i_dart_function_new(n, "@dart/meta", NULL /* raw 4-byte mask */, rsp,
                                   i_dart_meta_on_request, n, &fo, 2 /* both sides */);
}

DartFunction *dart_node_meta_function(DartNode *n){
    i_DartPatterns *pm; DartFunction *meta; int acquired;
    if (!n) return NULL;
    acquired = i_dart_node_sys_lock(n);
    pm = (i_DartPatterns*)*i_dart_node_sys_slot(n);
    meta = pm ? pm->meta : NULL;
    i_dart_node_sys_unlock(n, acquired);
    return meta;
}

/* ---- VARIABLES -------------------------------------------------------------------------- */

#define DART__VAR_PREFIX      5u    /* value channel: [u8 flags][u32 write_seq] */
#define DART__VAR_FLAG_FORCED 0x01u
#define DART__SET_PREFIX      1u    /* set channel: [u8 op] */
#define DART__SET_OP_FORCE    0x01u
#define DART__SET_OP_UNFORCE  0x02u

struct DartVariable {
    DartNode       *n;
    i_DartPatterns *pm;
    struct DartVariable *next;   /* manager list */
    DartTopic      *value;   /* owner PUB_ONLY / accessor SUB_ONLY (kind VARIABLE) */
    DartTopic      *set;     /* owner SUB_ONLY / accessor PUB_ONLY (kind VAR_SET); NULL: read-only owner */
    uint8_t         is_owner, allow_force, readonly, forced, has_value;
    uint32_t        write_seq;
    uint8_t        *store;  uint32_t store_len,  store_cap;    /* owner: published value; accessor: cache */
    uint8_t        *shadow; uint32_t shadow_len, shadow_cap;   /* owner: source value while forced */
    DartVariableUpdateFn on_change; void *on_change_user;   /* fires only on an actual state change */
    DartVariableUpdateFn on_write;  void *on_write_user;    /* fires on every applied write */
    const DartSchema *cur_schema;   /* what store decodes with (owner: the create schema;
                                       remote: the last arrival's msg->schema) */
    uint32_t        last_source;    /* peer behind the current state (0 = a local call) */
    uint64_t        last_write_us;  /* when the current state applied (node clock) */
    uint64_t        last_written_us;   /* the writer's wall clock for that write (0 = unstamped) */
    uint32_t       *dup_peers;      /* owner: peers already reported for duplicate authority */
    uint16_t        dup_n, dup_cap;
};

/* copy v into a grown buffer; 0 on OOM (buffer unchanged) */
static int i_dart_buf_put(DartNode *n, uint8_t **buf, uint32_t *len, uint32_t *cap, DartBytes v){
    if (*cap < v.len){
        uint8_t *nb = (uint8_t*)i_dart_node_sys_alloc(n, *buf, v.len ? v.len : 1u);
        if (!nb) return 0;
        *buf = nb; *cap = (uint32_t)(v.len ? v.len : 1u);
    }
    if (v.len) memcpy(*buf, v.data, v.len);
    *len = (uint32_t)v.len;
    return 1;
}

static void i_dart_var_hdr(const DartVariable *v, uint8_t hdr[DART__VAR_PREFIX]){
    hdr[0] = (uint8_t)(v->forced ? DART__VAR_FLAG_FORCED : 0);
    i_dart_le_w32(hdr + 1, v->write_seq);
}

/* under the lock, BEFORE the store is overwritten: would applying (val, forced_now) change
 * the observed state? First value, different bytes, or a forced-flag flip. */
static int i_dart_var_would_change(const DartVariable *v, DartBytes val, int forced_now){
    if (!v->has_value) return 1;
    if ((v->forced != 0) != (forced_now != 0)) return 1;
    if (v->store_len != val.len) return 1;
    return val.len ? memcmp(v->store, val.data, val.len) != 0 : 0;
}

/* the current state as a DartVariableUpdate (views into manager memory: callback-only) */
static void i_dart_var_update_view(DartVariable *v, DartVariableUpdate *u){
    u->variable   = v;
    u->name       = i_dart_topic_name(v->value);
    u->value      = dart_bytes(v->store, v->store_len);
    u->schema     = v->cur_schema;
    u->forced     = v->forced;
    u->write_seq  = v->write_seq;
    u->source     = v->last_source;
    u->recv_us    = v->last_write_us;
    u->written_us = v->last_written_us;
}

/* a write just applied to the observed value (lock held, state already updated): stamp the
 * write context, then fire on_write always and on_change when the state changed, inline on
 * this thread (the poll / service thread for wire-originated writes, the caller's for local
 * ones) */
static void i_dart_var_notify(DartVariable *v, int changed, uint32_t source, uint64_t when_us,
                              uint64_t written_us){
    DartVariableUpdate u;
    v->last_source = source; v->last_write_us = when_us; v->last_written_us = written_us;
    if (!v->on_write && !(changed && v->on_change)) return;
    i_dart_var_update_view(v, &u);
    if (v->on_write)             v->on_write(&u, v->on_write_user);
    if (changed && v->on_change) v->on_change(&u, v->on_change_user);
}

/* owner: publish the store under a HELD lock (reentrant send, no wait): the paths that
 * cannot hand the app's buffer to an unlocked send (remote sets applied inside a delivery
 * callback, force/unforce republishing from the shadow/store). */
static void i_dart_var_publish_locked(DartVariable *v){
    uint8_t hdr[DART__VAR_PREFIX];
    i_dart_var_hdr(v, hdr);
    (void)i_dart_topic_send_hdr(v->value, dart_bytes(hdr, DART__VAR_PREFIX),
                                dart_bytes(v->store, v->store_len));
}

/* owner: a dumb write, from a HELD-lock context. Absorbed into the shadow source while
 * forced (no event: the observed value did not change), else stored + published
 * (reentrantly) + notified. */
static void i_dart_var_owner_apply(DartVariable *v, DartBytes val, uint32_t source,
                                   uint64_t when_us, uint64_t written_us){
    int changed;
    if (v->forced){ i_dart_buf_put(v->n, &v->shadow, &v->shadow_len, &v->shadow_cap, val); return; }
    changed = i_dart_var_would_change(v, val, 0);
    if (!i_dart_buf_put(v->n, &v->store, &v->store_len, &v->store_cap, val)) return;
    v->has_value = 1; v->write_seq++;
    i_dart_var_publish_locked(v);
    i_dart_var_notify(v, changed, source, when_us, written_us);
}
/* owner: force to val. Without allow_force this is a SILENT NO-OP (force is opaque on the
 * wire; the local API layer refuses loudly before ever reaching here). */
static void i_dart_var_owner_force(DartVariable *v, DartBytes val, uint32_t source,
                                   uint64_t when_us, uint64_t written_us){
    int changed;
    if (!v->allow_force) return;
    changed = i_dart_var_would_change(v, val, 1);
    if (!v->forced)   /* entering force: save the current source into the shadow */
        i_dart_buf_put(v->n, &v->shadow, &v->shadow_len, &v->shadow_cap, dart_bytes(v->store, v->store_len));
    if (!i_dart_buf_put(v->n, &v->store, &v->store_len, &v->store_cap, val)) return;
    v->forced = 1; v->has_value = 1; v->write_seq++;
    i_dart_var_publish_locked(v);
    i_dart_var_notify(v, changed, source, when_us, written_us);
}
static void i_dart_var_owner_unforce(DartVariable *v, uint32_t source, uint64_t when_us,
                                     uint64_t written_us){
    if (!v->forced) return;
    v->forced = 0;
    i_dart_buf_put(v->n, &v->store, &v->store_len, &v->store_cap, dart_bytes(v->shadow, v->shadow_len));
    v->write_seq++;
    i_dart_var_publish_locked(v);
    i_dart_var_notify(v, 1, source, when_us, written_us);   /* the forced flag flipped: always a change */
}

/* owner: a set/force/unforce op arrived on the set channel (delivery callback, lock held).
 * A zero-length payload rides the schema-validation exemption for op-only messages, so a
 * plain set (op 0) must carry a value: an empty one is ignored (a dumb write of nothing). */
static void i_dart_var_on_set(void *user, const DartMsg *msg){
    DartVariable *v = (DartVariable*)user;
    uint8_t op = msg->header.len >= 1 ? msg->header.data[0] : 0;
    if (op & DART__SET_OP_UNFORCE)    i_dart_var_owner_unforce(v, msg->publisher_id, msg->recv_us, msg->written_us);
    else if (op & DART__SET_OP_FORCE) { if (msg->data.len) i_dart_var_owner_force(v, msg->data, msg->publisher_id, msg->recv_us, msg->written_us); }
    else                              { if (msg->data.len) i_dart_var_owner_apply(v, msg->data, msg->publisher_id, msg->recv_us, msg->written_us); }
}

/* accessor: a new value arrived on the value channel (cache it + its forced/write_seq,
 * then notify: the source is the owner that published, not the original setter) */
static void i_dart_var_on_value(void *user, const DartMsg *msg){
    DartVariable *v = (DartVariable*)user;
    int forced_in, changed; uint32_t seq_in;
    if (msg->header.len < DART__VAR_PREFIX) return;
    /* stale-order guard: a reentrant set inside an owner's on_write/on_change publishes
       BEFORE the outer set's deferred send, so the older write_seq arrives second. A
       SAME-OWNER value behind the cached seq is stale: drop it (signed distance, wrap-safe).
       A different publisher (owner restart, failover) always applies. */
    seq_in = i_dart_le_r32(msg->header.data + 1);
    if (v->has_value && msg->publisher_id == v->last_source
        && (int32_t)(seq_in - v->write_seq) < 0) return;
    forced_in = (msg->header.data[0] & DART__VAR_FLAG_FORCED) ? 1 : 0;
    changed = i_dart_var_would_change(v, msg->data, forced_in);
    if (i_dart_buf_put(v->n, &v->store, &v->store_len, &v->store_cap, msg->data)){
        v->has_value = 1;
        v->forced = (uint8_t)forced_in;
        v->write_seq = i_dart_le_r32(msg->header.data + 1);
        v->cur_schema = msg->schema;
        i_dart_var_notify(v, changed, msg->publisher_id, msg->recv_us, msg->written_us);
    }
}

static DartVariable *i_dart_variable_new(DartNode *n, const char *name, const DartSchema *schema,
                                         const DartVariableOpts *opts, int owner){
    i_DartPatterns *pm; DartVariable *v;
    char sn[DART_TOPIC_NAME_MAX + 1]; size_t nl;
    DartTopicOpts vopt, sopt; int acquired, readonly, make_set;
    if (!n || !name) return NULL;
    nl = strlen(name);
    if (nl == 0 || nl + 4 > DART_TOPIC_NAME_MAX) return NULL;   /* room for "@set" */
    readonly = (opts && opts->access == DART_VAR_READONLY);
    make_set = owner ? !readonly : 1;   /* read-only owner has no set channel; accessor always can try */

    memset(&vopt, 0, sizeof vopt);
    vopt.qos.reliability = DART_RELIABLE;
    vopt.qos.catch_up = (opts && opts->catch_up) ? opts->catch_up : 1u;   /* a late accessor gets the latest */
    vopt.qos.keep_last = vopt.qos.catch_up;
    vopt.qos.backpressure_wait_us = (opts && opts->backpressure_wait_us) ? opts->backpressure_wait_us
                                                                         : DART_PATTERN_BP_WAIT_US;
    sopt = vopt; sopt.qos.catch_up = 0;   /* set channel: no replay */
    sopt.qos.keep_last = 2;   /* writes are last-write-wins state, not a stream: history is
                                 only the repair window (0 would inherit the reliable default
                                 of 10 and pin 10x the value size); backpressure covers bursts */

    acquired = i_dart_node_sys_lock(n);
    pm = i_dart_patterns_get(n);
    v = pm ? (DartVariable*)i_dart_node_sys_alloc(n, NULL, sizeof *v) : NULL;
    if (v){
        memset(v, 0, sizeof *v);
        v->n = n; v->pm = pm; v->is_owner = (uint8_t)owner;
        v->allow_force = (uint8_t)(opts && opts->allow_force);
        v->readonly = (uint8_t)readonly;
        v->cur_schema = schema;
    }
    i_dart_node_sys_unlock(n, acquired);
    if (!v) return NULL;

    v->value = i_dart_node_create_pattern_topic(n, name, owner ? DART_PUB_ONLY : DART_SUB_ONLY,
                              schema, &vopt, DART_KIND_VARIABLE, DART__VAR_PREFIX, 0,
                              (uint8_t)(owner && v->allow_force),   /* owner advertises whether force is permitted */
                              owner ? NULL : i_dart_var_on_value, v);
    if (!v->value) return NULL;   /* v stays pool-allocated: nothing routes into it yet */
    if (make_set){
        memcpy(sn, name, nl); memcpy(sn + nl, "@set", 5);
        v->set = i_dart_node_create_pattern_topic(n, sn, owner ? DART_SUB_ONLY : DART_PUB_ONLY,
                              schema, &sopt, DART_KIND_VAR_SET, DART__SET_PREFIX, 0, 0,
                              owner ? i_dart_var_on_set : NULL, v);
        if (!v->set){
            /* partial create: the value topic may route to v (accessor side), so keep the
               handle allocated and deactivate the created half (see i_dart_function_new) */
            dart_topic_set_role(v->value, DART_INACTIVE);
            return NULL;
        }
    }
    acquired = i_dart_node_sys_lock(n);       /* publish into the manager list */
    v->next = pm->vars; pm->vars = v;
    if (owner)      /* a rival owner may already be on the network */
        i_dart_pat_dup_sweep(n, v->value, DART_KIND_VARIABLE, 1,
                             &v->dup_peers, &v->dup_n, &v->dup_cap);
    i_dart_node_sys_unlock(n, acquired);
    if (owner && opts && opts->initial.len)   /* seed the store + history so a late accessor catches up */
        dart_variable_set(v, opts->initial);
    return v;
}

DartVariable *dart_node_create_variable_definition(DartNode *n, const char *name,
                              const DartSchema *schema, const DartVariableOpts *opts){
    if (i_dart_pat_reserved(name)) return NULL;
    return i_dart_variable_new(n, name, schema, opts, 1);
}
DartVariable *dart_node_create_remote_variable(DartNode *n, const char *name,
                              const DartSchema *schema, const DartVariableOpts *opts){
    if (i_dart_pat_reserved(name)) return NULL;
    return i_dart_variable_new(n, name, schema, opts, 0);
}

int dart_variable_get(DartVariable *var, DartBytes *out){
    int acquired, has;
    if (!var) return 0;
    acquired = i_dart_node_sys_lock(var->n);
    has = var->has_value;
    if (out){ out->data = var->store; out->len = has ? var->store_len : 0; }
    i_dart_node_sys_unlock(var->n, acquired);
    return has;   /* out views manager memory: bracket with dart_node_lock if a poller runs elsewhere */
}

/* accessor write routing: distinguish "no owner at all" (no publisher feeds our value
 * subscription) from "the owner is read-only" (an owner publishes the value but advertises
 * no set channel for our writes) */
static int i_dart_var_accessor_route(DartVariable *var){
    if (i_dart_topic_source_match_count(var->value) == 0) return DART_ERR_NO_TOPIC; /* no owner present */
    if (!var->set || dart_topic_match_count(var->set) == 0) return DART_ERR_ROLE;   /* read-only owner */
    return DART_OK;
}

int dart_variable_set(DartVariable *var, DartBytes value){
    int acquired, r = DART_OK, publish = 0;
    uint32_t my_seq = 0;
    uint8_t hdr[DART__VAR_PREFIX];
    if (!var) return DART_ERR_NO_TOPIC;
    if (var->is_owner){
        /* mutate the store under the lock; publish OUTSIDE it from the CALLER'S buffer
           (same bytes as the store), so the send engages backpressure */
        acquired = i_dart_node_sys_lock(var->n);
        if (var->forced){
            i_dart_buf_put(var->n, &var->shadow, &var->shadow_len, &var->shadow_cap, value);
        } else {
            int changed = i_dart_var_would_change(var, value, 0);
            if (!i_dart_buf_put(var->n, &var->store, &var->store_len, &var->store_cap, value)){
                r = DART_ERR_OOM;
            } else {
                var->has_value = 1; var->write_seq++;
                my_seq = var->write_seq;
                i_dart_var_hdr(var, hdr);
                publish = 1;
                i_dart_var_notify(var, changed, 0, i_dart_node_now_us(var->n),
                                  i_dart_node_wall_us(var->n));   /* a local write: our own wall clock */
                /* a reentrant set inside the callback advanced the seq and already
                   published the newer value under the held lock, so it COMMITTED FIRST:
                   sending ours now would put stale bytes newest in transport history
                   (and catch_up would replay them). Skip; the write itself stands. */
                if (var->write_seq != my_seq) publish = 0;
            }
        }
        i_dart_node_sys_unlock(var->n, acquired);
        return publish ? i_dart_topic_send_hdr(var->value, dart_bytes(hdr, DART__VAR_PREFIX), value) : r;
    }
    r = i_dart_var_accessor_route(var);
    if (r != DART_OK) return r;
    {   uint8_t op = 0;
        return i_dart_topic_send_hdr(var->set, dart_bytes(&op, 1), value);
    }
}

int dart_variable_force(DartVariable *var, DartBytes value){
    int acquired, r = DART_OK;
    if (!var) return DART_ERR_NO_TOPIC;
    if (var->is_owner){
        if (!var->allow_force) return DART_ERR_STATE;   /* locally checkable: refuse loudly */
        acquired = i_dart_node_sys_lock(var->n);
        i_dart_var_owner_force(var, value, 0, i_dart_node_now_us(var->n),
                               i_dart_node_wall_us(var->n));   /* rare debug op: reentrant publish */
        i_dart_node_sys_unlock(var->n, acquired);
        return DART_OK;
    }
    r = i_dart_var_accessor_route(var);
    if (r != DART_OK) return r;
    {   uint8_t op = DART__SET_OP_FORCE;
        return i_dart_topic_send_hdr(var->set, dart_bytes(&op, 1), value);
    }
}

int dart_variable_unforce(DartVariable *var){
    int acquired, r = DART_OK;
    if (!var) return DART_ERR_NO_TOPIC;
    if (var->is_owner){
        if (!var->allow_force) return DART_ERR_STATE;
        acquired = i_dart_node_sys_lock(var->n);
        i_dart_var_owner_unforce(var, 0, i_dart_node_now_us(var->n), i_dart_node_wall_us(var->n));
        i_dart_node_sys_unlock(var->n, acquired);
        return DART_OK;
    }
    r = i_dart_var_accessor_route(var);
    if (r != DART_OK) return r;
    {   uint8_t op = DART__SET_OP_UNFORCE;
        return i_dart_topic_send_hdr(var->set, dart_bytes(&op, 1), dart_bytes(NULL,0));
    }
}

int dart_variable_forced(DartVariable *var){ return var ? var->forced : 0; }

int dart_variable_retire(DartVariable *var){
    i_DartPatterns *pm; DartVariable **pp; DartNode *n; int acquired, r;
    if (!var) return DART_ERR_NO_TOPIC;
    pm = var->pm; n = var->n;
    r = dart_topic_set_role(var->value, DART_INACTIVE);   /* refused from a callback, nothing mutated */
    if (r != 0) return r;
    if (var->set) (void)dart_topic_set_role(var->set, DART_INACTIVE);
    acquired = i_dart_node_sys_lock(n);
    i_dart_topic_clear_sys(var->value);
    if (var->set) i_dart_topic_clear_sys(var->set);
    if (pm)
        for (pp = &pm->vars; *pp; pp = &(*pp)->next)
            if (*pp == var){ *pp = var->next; break; }
    if (var->store)     i_dart_node_sys_alloc(n, var->store, 0);
    if (var->shadow)    i_dart_node_sys_alloc(n, var->shadow, 0);
    if (var->dup_peers) i_dart_node_sys_alloc(n, var->dup_peers, 0);
    i_dart_node_sys_alloc(n, var, 0);
    i_dart_node_sys_unlock(n, acquired);
    return DART_OK;
}

int dart_variable_on_change(DartVariable *var, DartVariableUpdateFn on_change, void *user){
    int acquired;
    if (!var) return DART_ERR_NO_TOPIC;
    acquired = i_dart_node_sys_lock(var->n);
    var->on_change = on_change; var->on_change_user = user;
    if (on_change && var->has_value){
        /* replay the current state once, right here on the registering thread, so a value
           that arrived between create and register is never missed */
        DartVariableUpdate u;
        i_dart_var_update_view(var, &u);
        on_change(&u, user);
    }
    i_dart_node_sys_unlock(var->n, acquired);
    return DART_OK;
}

int dart_variable_on_write(DartVariable *var, DartVariableUpdateFn on_write, void *user){
    int acquired;
    if (!var) return DART_ERR_NO_TOPIC;
    acquired = i_dart_node_sys_lock(var->n);
    var->on_write = on_write; var->on_write_user = user;
    i_dart_node_sys_unlock(var->n, acquired);
    return DART_OK;   /* writes are events, not state: no replay */
}

int dart_variable_wait(DartVariable *var, int timeout_ms){
    int acquired; uint64_t deadline;
    if (!var) return 0;
    acquired = i_dart_node_sys_lock(var->n);
    if (!acquired || dart_node_is_started(var->n)){ i_dart_node_sys_unlock(var->n, acquired); return var->has_value; }
    if (var->has_value){ i_dart_node_sys_unlock(var->n, acquired); return 1; }
    deadline = i_dart_node_now_us(var->n) + (uint64_t)(timeout_ms >= 0 ? (uint64_t)timeout_ms*1000u : 3600000000ull);
    i_dart_node_sys_unlock(var->n, acquired);
    while (!var->has_value){
        if (i_dart_node_now_us(var->n) >= deadline) break;
        i_dart_node_sys_poll(var->n, 5);
    }
    return var->has_value;
}

int dart_variable_match_count(DartVariable *var){
    if (!var) return 0;
    return dart_topic_match_count(var->is_owner ? var->value : var->set);
}

/* ---- SIGNALS ---------------------------------------------------------------------------- */

struct DartSignal {
    DartNode       *n;
    i_DartPatterns *pm;
    struct DartSignal *next;    /* manager list */
    DartTopic      *topic;      /* kind SIGNAL, reliable, catch_up 0 (never latched);
                                   PUBSUB with a handler, else PUB_ONLY (emit always advertised) */
    DartSignalFn    on_signal;
    void           *user;
};

static void i_dart_signal_on_msg(void *user, const DartMsg *msg){
    DartSignal *s = (DartSignal*)user;
    if (s->on_signal) s->on_signal(msg, s->user);
}

DartSignal *dart_node_create_signal(DartNode *n, const char *name, const DartSchema *schema,
                              DartSignalFn on_signal, void *user, const DartSignalOpts *opts){
    i_DartPatterns *pm; DartSignal *s; DartTopicOpts topt; int acquired;
    /* the role is derived, never declared: a handler is the subscription, and the emit side
       is EAGER for every handle so a first emit never pays an announce round trip (e-stop) */
    DartRole role = on_signal ? DART_PUBSUB : DART_PUB_ONLY;
    if (!n || !name || !name[0] || i_dart_pat_reserved(name)) return NULL;
    memset(&topt, 0, sizeof topt);
    topt.qos.reliability = DART_RELIABLE;
    topt.qos.catch_up = 0;   /* SEALED: a late joiner receives nothing published before it joined */
    topt.qos.backpressure_wait_us = (opts && opts->backpressure_wait_us) ? opts->backpressure_wait_us
                                                                         : DART_PATTERN_BP_WAIT_US;
    acquired = i_dart_node_sys_lock(n);
    pm = i_dart_patterns_get(n);
    s = pm ? (DartSignal*)i_dart_node_sys_alloc(n, NULL, sizeof *s) : NULL;
    if (s){ memset(s, 0, sizeof *s); s->n = n; s->pm = pm; s->on_signal = on_signal; s->user = user; }
    i_dart_node_sys_unlock(n, acquired);
    if (!s) return NULL;
    s->topic = i_dart_node_create_pattern_topic(n, name, role, schema, &topt,
                              DART_KIND_SIGNAL, 0, 0, 0, on_signal ? i_dart_signal_on_msg : NULL, s);
    if (!s->topic) return NULL;   /* s stays pool-allocated: nothing routes into it */
    acquired = i_dart_node_sys_lock(n);
    s->next = pm->sigs; pm->sigs = s;
    i_dart_node_sys_unlock(n, acquired);
    return s;
}

int dart_signal_retire(DartSignal *sig){
    i_DartPatterns *pm; DartSignal **pp; DartNode *n; int acquired, r;
    if (!sig) return DART_ERR_NO_TOPIC;
    pm = sig->pm; n = sig->n;
    r = dart_topic_set_role(sig->topic, DART_INACTIVE);   /* refused from a callback, nothing mutated */
    if (r != 0) return r;
    acquired = i_dart_node_sys_lock(n);
    i_dart_topic_clear_sys(sig->topic);
    if (pm)
        for (pp = &pm->sigs; *pp; pp = &(*pp)->next)
            if (*pp == sig){ *pp = sig->next; break; }
    i_dart_node_sys_alloc(n, sig, 0);
    i_dart_node_sys_unlock(n, acquired);
    return DART_OK;
}

int dart_signal_emit(DartSignal *sig, DartBytes payload){
    if (!sig) return DART_ERR_NO_TOPIC;
    return dart_topic_send(sig->topic, payload);   /* public path: backpressure engages */
}

/* Listeners this handle's emits reach. */
int dart_signal_listener_count(DartSignal *sig){
    if (!sig) return 0;
    return dart_topic_match_count(sig->topic);
}

/* ---- duplicate-authority detection --------------------------------------------------------
 * The pattern contract expects exactly ONE handler per function and ONE owner per variable.
 * Two authorities never match each other (both hold the channel's authoritative direction,
 * so their roles are pub/pub or sub/sub and no lane forms), which means the transport's
 * gates can never see the conflict; the announce interest can. A peer entry with the same
 * kind, the same low-32 name hash, and the authoritative direction claims the same entity:
 * DART_E_DUPLICATE_AUTHORITY fires once per (entity, peer). Like matching, the 32-bit hash
 * only NOMINATES: when the peer's name is already in the detail cache it is confirmed first
 * (authorities exchange no details between themselves, so it usually is not); an actual
 * hash overlap costs one spurious diagnostic, never a refusal. Checked when a peer's
 * interest is (re)applied and when the authority-side entity is created (peers whose
 * interest arrived before the entity existed announce nothing new to trigger on). */

static uint32_t i_dart_pat_hash32(DartString base, const char *suffix);         /* reflection */
static const DartDiscoveryPeer *i_dart_pat_peer(DartNode *n, uint32_t peer);    /* helpers below */

static int i_dart_pat_dup_reported(const uint32_t *ids, uint16_t n_ids, uint32_t peer){
    uint16_t i;
    for (i = 0; i < n_ids; i++) if (ids[i] == peer) return 1;
    return 0;
}

/* remember peer in the entity's reported list (grown; on OOM it may report again later) */
static void i_dart_pat_dup_remember(DartNode *n, uint32_t **ids, uint16_t *n_ids,
                                    uint16_t *cap, uint32_t peer){
    if (*n_ids == *cap){
        uint16_t grown = *cap ? (uint16_t)(*cap * 2u) : 4u;
        uint32_t *nb = (uint32_t*)i_dart_node_sys_alloc(n, *ids, grown * sizeof **ids);
        if (!nb) return;
        *ids = nb; *cap = grown;
    }
    (*ids)[(*n_ids)++] = peer;
}

/* forget peer in one entity's reported list (peer down: a genuine return re-reports) */
static void i_dart_pat_dup_drop(uint32_t *ids, uint16_t *n_ids, uint32_t peer){
    uint16_t i;
    for (i = 0; i < *n_ids; i++)
        if (ids[i] == peer){ ids[i] = ids[--*n_ids]; return; }
}

/* does peer advertise the authoritative direction of (hash, kind)? authority_is_pub says
 * which direction is the authority on that channel (a variable's value channel: pub; a
 * function's req channel: sub). */
static int i_dart_pat_peer_claims(DartNode *n, const DartDiscoveryPeer *p, DartString our_name,
                                  uint32_t hash, uint8_t kind, int authority_is_pub){
    DartInterestIter it; DartTopicEntry e;
    memset(&it, 0, sizeof it);
    while (dart_node_peer_interest_next(p, &it, &e)){
        int claims;
        if (e.hash != hash || e.kind != kind) continue;
        claims = authority_is_pub ? (e.role == DART_PUB_ONLY || e.role == DART_PUBSUB)
                                  : (e.role == DART_SUB_ONLY || e.role == DART_PUBSUB);
        if (!claims) continue;
        {   /* the hash only nominates: confirm against the fetched name when we hold one */
            DartString nm = dart_node_peer_topic_name(n, p->id, e.index);
            if (nm.len && (nm.len != our_name.len
                           || memcmp(nm.data, our_name.data, nm.len) != 0)) continue;
        }
        return 1;
    }
    return 0;
}

/* check ONE authority-side entity against ONE peer; report a fresh claim exactly once */
static void i_dart_pat_dup_check(DartNode *n, const DartDiscoveryPeer *p, DartTopic *primary,
                                 uint8_t kind, int authority_is_pub,
                                 uint32_t **ids, uint16_t *n_ids, uint16_t *cap){
    DartString nm;
    if (!primary || p->liveness != DART_PEER_ACTIVE) return;
    if (i_dart_pat_dup_reported(*ids, *n_ids, p->id)) return;
    nm = i_dart_topic_name(primary);
    if (!i_dart_pat_peer_claims(n, p, nm, i_dart_pat_hash32(nm, ""), kind, authority_is_pub))
        return;
    i_dart_pat_dup_remember(n, ids, n_ids, cap, p->id);
    i_dart_node_sys_error(n, DART_E_DUPLICATE_AUTHORITY, primary, p->id);
}

/* every authority-side entity vs one peer whose interest was just (re)applied */
static void i_dart_pat_dup_check_peer(i_DartPatterns *pm, uint32_t peer){
    const DartDiscoveryPeer *p = i_dart_pat_peer(pm->n, peer);
    DartFunction *fn; DartVariable *v;
    if (!p) return;
    for (fn = pm->funcs; fn; fn = fn->next)
        if ((fn->is_provider || fn->both) && !fn->multi)   /* multi: rivals are the design */
            i_dart_pat_dup_check(pm->n, p, fn->req, DART_KIND_FUNC_REQ, 0,
                                 &fn->dup_peers, &fn->dup_n, &fn->dup_cap);
    for (v = pm->vars; v; v = v->next)
        if (v->is_owner)
            i_dart_pat_dup_check(pm->n, p, v->value, DART_KIND_VARIABLE, 1,
                                 &v->dup_peers, &v->dup_n, &v->dup_cap);
}

/* a just-created authority-side entity vs every known peer (create-time sweep) */
static void i_dart_pat_dup_sweep(DartNode *n, DartTopic *primary, uint8_t kind,
                                 int authority_is_pub, uint32_t **ids, uint16_t *n_ids,
                                 uint16_t *cap){
    uint16_t cnt, i;
    const DartDiscoveryPeer *ps = dart_node_peers(n, &cnt);
    if (!ps) return;
    for (i = 0; i < cnt; i++)
        i_dart_pat_dup_check(n, &ps[i], primary, kind, authority_is_pub, ids, n_ids, cap);
}

static void i_dart_pat_dup_forget_peer(i_DartPatterns *pm, uint32_t peer){
    DartFunction *fn; DartVariable *v;
    for (fn = pm->funcs; fn; fn = fn->next) i_dart_pat_dup_drop(fn->dup_peers, &fn->dup_n, peer);
    for (v = pm->vars; v; v = v->next)      i_dart_pat_dup_drop(v->dup_peers, &v->dup_n, peer);
}

/* ---- manager hooks: call timeouts (tick) + provider-loss (event) ------------------------ */

/* fail-and-remove every pending call of fn past its deadline (or, when all!=0,
 * unconditionally: provider lost). Callbacks run after the unlink so a reentrant new call
 * is safe. Returns the earliest surviving deadline (0 = none). */
static uint64_t i_dart_func_reap(DartFunction *fn, uint64_t now, DartCallStatus fail_status, int all){
    i_DartPending **pp = &fn->pending, *p;
    uint64_t soonest = 0;
    while ((p = *pp) != NULL){
        if (all || now >= p->deadline_us){
            *pp = p->next;
            {   DartResponse r; r.status = fail_status; r.data = dart_bytes(NULL,0);
                r.schema = NULL; r.provider = 0; r.user = p->user; r.written_us = 0;
                r.message = i_dart_call_status_msg(fail_status);
                if (p->on_response) p->on_response(&r);
            }
            i_dart_func_free_pending(fn, p);
            continue;
        }
        if (!soonest || p->deadline_us < soonest) soonest = p->deadline_us;
        pp = &p->next;
    }
    return soonest;
}

/* fail-and-remove the pending calls DIRECTED at a lost peer (a .provider call can only
 * ever be answered by that peer, so waiting out the timeout serves nothing) */
static void i_dart_func_reap_dest(DartFunction *fn, uint32_t dest){
    i_DartPending **pp = &fn->pending, *p;
    while ((p = *pp) != NULL){
        if (p->dest == dest){
            *pp = p->next;
            {   DartResponse r; r.status = DART_CALL_PEER_LOST; r.data = dart_bytes(NULL,0);
                r.schema = NULL; r.provider = 0; r.user = p->user; r.written_us = 0;
                r.message = i_dart_call_status_msg(DART_CALL_PEER_LOST);
                if (p->on_response) p->on_response(&r);
            }
            i_dart_func_free_pending(fn, p);
            continue;
        }
        pp = &p->next;
    }
}

static uint64_t i_dart_patterns_tick(void *user, uint64_t now_us){
    i_DartPatterns *pm = (i_DartPatterns*)user;
    DartFunction *fn; uint64_t soonest = 0;
    for (fn = pm->funcs; fn; fn = fn->next){
        uint64_t s;
        i_dart_func_flush_queued(fn);   /* backstop: the interest event is the fast path */
        s = i_dart_func_reap(fn, now_us, DART_CALL_TIMEOUT, 0);
        if (s && (!soonest || s < soonest)) soonest = s;
    }
    return soonest;   /* next timeout deadline for the poll wait cap */
}

/* node closing: a call still pending can never be answered, so synthesize CANCELLED for
 * each (the always-one-outcome contract holds at close; a binding's future/Task settles
 * instead of hanging). Runs once, on the closing thread, node still fully alive. */
static void i_dart_patterns_on_close(void *user){
    i_DartPatterns *pm = (i_DartPatterns*)user;
    DartFunction *fn;
    for (fn = pm->funcs; fn; fn = fn->next)
        if (!fn->is_provider && fn->pending)
            i_dart_func_reap(fn, 0, DART_CALL_CANCELLED, 1);
}

static void i_dart_patterns_on_event(void *user, const DartEvent *ev){
    i_DartPatterns *pm = (i_DartPatterns*)user;
    DartFunction *fn;
    if (ev->kind == DART_PEER_INTEREST){
        DartVariable *v;
        /* a match may just have formed: flush calls queued while no provider was matched */
        for (fn = pm->funcs; fn; fn = fn->next)
            if (!fn->is_provider && fn->pending) i_dart_func_flush_queued(fn);
        /* an accessor whose owner just parked (retire) or left: disarm the stale-order
           guard in i_dart_var_on_value. A successor definition on the SAME node keeps the
           peer id while its write_seq restarts at 1, so without this the guard would drop
           the new incarnation's values as stale forever. */
        for (v = pm->vars; v; v = v->next)
            if (!v->is_owner && v->last_source
                && i_dart_topic_source_match_count(v->value) == 0) v->last_source = 0;
        i_dart_pat_dup_check_peer(pm, ev->peer);   /* a rival authority may have appeared */
        return;
    }
    if (ev->kind != DART_PEER_DOWN) return;
    i_dart_pat_dup_forget_peer(pm, ev->peer);
    /* a provider dropped: a caller with no LIVE provider left can never be answered, so fail
       its outstanding calls now with PEER_LOST instead of waiting out the timeout. The live
       count excludes dormant peers: dart_topic_match_count keeps counting the dropped peer
       (its lane is preserved for resume), so it can never see the loss. A call DIRECTED at
       the dropped peer fails regardless of who else is alive. */
    for (fn = pm->funcs; fn; fn = fn->next){
        if (fn->is_provider || !fn->pending) continue;
        i_dart_func_reap_dest(fn, ev->peer);
        if (fn->pending && i_dart_topic_live_match_count(fn->req) == 0)
            i_dart_func_reap(fn, 0, DART_CALL_PEER_LOST, 1);
    }
}

/* ---- REFLECTION: the entity fold ---------------------------------------------------------
 * Observers consume entities, never channels. One raw interest entry per index (the walk's
 * pub/sub double-yield collapsed via entry.role); pattern channels fold by their kind plus
 * the @-suffix convention, with partners located by the LOW-32 HASH of the expected partner
 * name so pairing works from the announce alone once the primary's name is fetched. */

/* the first entry with index >= it->next_index, resuming the walk's persistent cursor
 * (entries are yielded in index order; the pub/sub double-yield collapses because
 * entry.role already covers both directions). An interest change mid-walk (epoch bump)
 * reseeks from the front, matching the old restart-per-call behavior. */
static int i_dart_pat_entry_from(const DartDiscoveryPeer *p, DartEntityIter *it, DartTopicEntry *out){
    uint32_t ep = dart_node_peer_interest_epoch(p);
    if (it->epoch != ep){
        memset(&it->pos, 0, sizeof it->pos);
        it->has_prev = 0;
        it->epoch = ep;
    }
    while (dart_node_peer_interest_next(p, &it->pos, out))
        if (out->index >= it->next_index) return 1;
    return 0;
}

/* peek the entry after index `from` through a copy of the persistent cursor: the
 * adjacent-partner fast path (pattern channel pairs are created back to back, so the
 * partner is almost always at from+1). A miss falls back to i_dart_pat_find_hash. */
static int i_dart_pat_peek_next(const DartDiscoveryPeer *p, const DartEntityIter *it,
                                uint16_t from, DartTopicEntry *out){
    DartInterestIter pos = it->pos;
    while (dart_node_peer_interest_next(p, &pos, out))
        if (out->index > from) return 1;
    return 0;
}

/* find an entry by (low-32 name hash, kind); 1 + *out on a hit */
static int i_dart_pat_find_hash(const DartDiscoveryPeer *p, uint32_t hash,
                                uint8_t kind, DartTopicEntry *out){
    DartInterestIter it; DartTopicEntry e;
    memset(&it, 0, sizeof it);
    while (dart_node_peer_interest_next(p, &it, &e))
        if (e.hash == hash && e.kind == kind){ *out = e; return 1; }
    return 0;
}

/* low-32 identity hash of base+suffix (suffix may be "") */
static uint32_t i_dart_pat_hash32(DartString base, const char *suffix){
    char buf[DART_TOPIC_NAME_MAX + 1]; size_t sl = strlen(suffix);
    if (base.len + sl > DART_TOPIC_NAME_MAX) return 0;
    memcpy(buf, base.data, base.len);
    memcpy(buf + base.len, suffix, sl + 1);
    return (uint32_t)dart_topic_id(buf);
}

/* strip a 4-byte "@xxx" suffix; 1 + *out when name ends with it */
static int i_dart_pat_strip(DartString name, const char *suffix, DartString *out){
    if (name.len < 5 || memcmp(name.data + name.len - 4, suffix, 4) != 0) return 0;
    out->data = name.data; out->len = name.len - 4;
    return 1;
}

/* The "@dart/" builtins every node hosts (the log topics + the meta function) are
   infrastructure, not application entities: both entity walks hide them. Peer entries
   are filtered by channel hash so the verdict never depends on detail-fetch state;
   local handles always have names. A leading '@' is refused in every public
   constructor (create_impl already refuses '@' anywhere in a plain topic name), so
   an application entity can never land in the hidden namespace. */
static int i_dart_pat_hidden_name(DartString nm){
    return nm.len >= 6 && memcmp(nm.data, "@dart/", 6) == 0;
}
static int i_dart_pat_hidden_hash(uint32_t h){
    static const char *const nm[] = { "@dart/log/error", "@dart/log/warn", "@dart/log/info",
                                      "@dart/meta@req", "@dart/meta@rsp" };
    size_t i;
    for (i = 0; i < sizeof nm / sizeof nm[0]; i++)
        if (h == (uint32_t)dart_topic_id(nm[i])) return 1;
    return 0;
}

static const DartDiscoveryPeer *i_dart_pat_peer(DartNode *n, uint32_t peer){
    uint16_t cnt, i;
    const DartDiscoveryPeer *ps = dart_node_peers(n, &cnt);
    if (!ps) return NULL;
    for (i = 0; i < cnt; i++)
        if (ps[i].id == peer) return &ps[i];
    return NULL;
}

static void i_dart_pat_fill(DartNode *n, uint32_t peer, DartEntityInfo *out, DartEntityKind kind,
                            DartString name, const DartTopicEntry *e){
    memset(out, 0, sizeof *out);
    out->kind = kind;
    out->name = name;
    out->reliable = e->reliable;
    out->index = e->index;
    out->hash = e->hash;
    out->provides = (uint8_t)(e->role == DART_PUBSUB || e->role == DART_PUB_ONLY);
    out->consumes = (uint8_t)(e->role == DART_PUBSUB || e->role == DART_SUB_ONLY);
    out->forceable = e->forceable;   /* variable value channel: the owner advertised allow_force */
    out->schema = dart_node_peer_topic_schema(n, peer, e->index, &out->schema_hash);
}

int dart_node_peer_entity_next(DartNode *n, uint32_t peer, DartEntityIter *it, DartEntityInfo *out){
    const DartDiscoveryPeer *p;
    DartTopicEntry e, partner, prev_e;
    DartString name, base;
    uint8_t had_prev;
    if (!n || !it || !out) return 0;
    p = i_dart_pat_peer(n, peer);
    if (!p) return 0;
    /* a DROPPED peer's cached entities are its DEAD incarnation's: refused unless the
       caller opted in (see core.h), so liveness gating cannot be forgotten */
    if (p->liveness != DART_PEER_ACTIVE && !it->include_dropped) return 0;
    for (;;){
        if (!i_dart_pat_entry_from(p, it, &e)) return 0;
        it->next_index = (uint16_t)(e.index + 1);
        prev_e = it->prev; had_prev = it->has_prev;   /* the entry walked before e */
        it->prev = e; it->has_prev = 1;
        if (i_dart_pat_hidden_hash(e.hash)) continue;   /* builtins never surface */
        name = dart_node_peer_topic_name(n, peer, e.index);   /* {NULL,0} until details arrive */
        switch (e.kind){
        case DART_KIND_TOPIC:
            i_dart_pat_fill(n, peer, out, DART_ENTITY_TOPIC, name, &e);
            return 1;
        case DART_KIND_SIGNAL:
            i_dart_pat_fill(n, peer, out, DART_ENTITY_SIGNAL, name, &e);
            return 1;
        case DART_KIND_VARIABLE:
            i_dart_pat_fill(n, peer, out, DART_ENTITY_VARIABLE, name, &e);
            /* the value channel is the bare name: a same-peer "<name>@set" VAR_SET entry
               makes it writable (unknowable until the name is fetched: shown unwritable) */
            if (name.len){
                uint32_t want = i_dart_pat_hash32(name, "@set");
                if (i_dart_pat_peek_next(p, it, e.index, &partner) && partner.index == e.index + 1
                    && partner.kind == DART_KIND_VAR_SET && partner.hash == want)
                    out->writable = 1;   /* adjacent partner: no rescan */
                else
                    out->writable = (uint8_t)i_dart_pat_find_hash(p, want, DART_KIND_VAR_SET, &partner);
            }
            return 1;
        case DART_KIND_VAR_SET:
            /* secondary: consumed by its bare-name VARIABLE entry when both are advertised */
            if (!name.len){   /* not yet identifiable: surface, never silently drop */
                i_dart_pat_fill(n, peer, out, DART_ENTITY_VARIABLE, name, &e);
                out->incomplete = 1;
                return 1;
            }
            if (i_dart_pat_strip(name, "@set", &base)){
                uint32_t want = i_dart_pat_hash32(base, "");
                if ((had_prev && prev_e.index + 1 == e.index && prev_e.kind == DART_KIND_VARIABLE
                     && prev_e.hash == want)   /* adjacent partner: no rescan */
                    || i_dart_pat_find_hash(p, want, DART_KIND_VARIABLE, &partner))
                    continue;   /* folded into the value entity */
            }
            i_dart_pat_fill(n, peer, out, DART_ENTITY_VARIABLE,
                            i_dart_pat_strip(name, "@set", &base) ? base : name, &e);
            out->writable = 1; out->incomplete = 1;   /* a set channel with no value channel */
            /* a set channel's SUB side is the owner (it receives writes) */
            out->provides = (uint8_t)(e.role == DART_PUBSUB || e.role == DART_SUB_ONLY);
            out->consumes = (uint8_t)(e.role == DART_PUBSUB || e.role == DART_PUB_ONLY);
            return 1;
        case DART_KIND_FUNC_REQ:
            i_dart_pat_fill(n, peer, out, DART_ENTITY_FUNCTION, name, &e);
            /* the req channel's SUB side is the provider */
            out->provides = (uint8_t)(e.role == DART_PUBSUB || e.role == DART_SUB_ONLY);
            out->consumes = (uint8_t)(e.role == DART_PUBSUB || e.role == DART_PUB_ONLY);
            if (name.len && i_dart_pat_strip(name, "@req", &base)){
                uint32_t want = i_dart_pat_hash32(base, "@rsp");
                out->name = base;
                if ((i_dart_pat_peek_next(p, it, e.index, &partner) && partner.index == e.index + 1
                     && partner.kind == DART_KIND_FUNC_RSP && partner.hash == want)   /* adjacent */
                    || i_dart_pat_find_hash(p, want, DART_KIND_FUNC_RSP, &partner))
                    out->rsp_schema = dart_node_peer_topic_schema(n, peer, partner.index,
                                                                  &out->rsp_schema_hash);
                else out->incomplete = 1;   /* half a function advertised */
            } else {
                out->incomplete = 1;        /* name unfetched (partner unknowable yet), or a
                                               FUNC_REQ kind without the @req convention */
            }
            return 1;
        case DART_KIND_FUNC_RSP:
            /* secondary: consumed by its @req twin when both are advertised */
            if (!name.len){
                i_dart_pat_fill(n, peer, out, DART_ENTITY_FUNCTION, name, &e);
                out->incomplete = 1;
                return 1;
            }
            if (i_dart_pat_strip(name, "@rsp", &base)){
                uint32_t want = i_dart_pat_hash32(base, "@req");
                if ((had_prev && prev_e.index + 1 == e.index && prev_e.kind == DART_KIND_FUNC_REQ
                     && prev_e.hash == want)   /* adjacent partner: no rescan */
                    || i_dart_pat_find_hash(p, want, DART_KIND_FUNC_REQ, &partner))
                    continue;   /* folded into the function entity */
            }
            i_dart_pat_fill(n, peer, out, DART_ENTITY_FUNCTION,
                            i_dart_pat_strip(name, "@rsp", &base) ? base : name, &e);
            out->incomplete = 1;
            /* the rsp channel's PUB side is the provider */
            out->provides = (uint8_t)(e.role == DART_PUBSUB || e.role == DART_PUB_ONLY);
            out->consumes = (uint8_t)(e.role == DART_PUBSUB || e.role == DART_SUB_ONLY);
            out->rsp_schema = out->schema; out->rsp_schema_hash = out->schema_hash;
            out->schema = NULL; out->schema_hash = 0;
            return 1;
        default:   /* an unknown future kind: surface it, do not render it as a plain topic */
            i_dart_pat_fill(n, peer, out, DART_ENTITY_TOPIC, name, &e);
            out->incomplete = 1;
            return 1;
        }
    }
}

/* local enumeration: this node's own entities (functions, variables, signals, then the
 * plain topics that are not pattern internals), from the manager lists + the handle table */
int dart_node_entity_next(DartNode *n, DartEntityIter *it, DartEntityInfo *out){
    i_DartPatterns *pm;
    uint16_t skip;
    if (!n || !it || !out) return 0;
    pm = (i_DartPatterns*)*i_dart_node_sys_slot(n);
    for (;;){
        switch (it->phase){
        case 0: {   /* functions */
            DartFunction *fn = pm ? pm->funcs : NULL;
            for (skip = it->next_index; fn && skip; skip--) fn = fn->next;
            if (!fn){ it->phase = 1; it->next_index = 0; continue; }
            it->next_index++;
            if (i_dart_pat_hidden_name(i_dart_topic_name(fn->req))) continue;
            memset(out, 0, sizeof *out);
            out->kind = DART_ENTITY_FUNCTION;
            { DartString nm = i_dart_topic_name(fn->req), base;
              out->name = i_dart_pat_strip(nm, "@req", &base) ? base : nm; }
            out->provides = fn->is_provider; out->consumes = (uint8_t)!fn->is_provider;
            out->reliable = 1;
            out->index = dart_topic_index(fn->req);
            out->schema = dart_topic_schema(fn->req);
            out->rsp_schema = dart_topic_schema(fn->rsp);
            return 1;
        }
        case 1: {   /* variables */
            DartVariable *v = pm ? pm->vars : NULL;
            for (skip = it->next_index; v && skip; skip--) v = v->next;
            if (!v){ it->phase = 2; it->next_index = 0; continue; }
            it->next_index++;
            if (i_dart_pat_hidden_name(i_dart_topic_name(v->value))) continue;
            memset(out, 0, sizeof *out);
            out->kind = DART_ENTITY_VARIABLE;
            out->name = i_dart_topic_name(v->value);
            out->provides = v->is_owner; out->consumes = (uint8_t)!v->is_owner;
            out->reliable = 1;
            out->writable = (uint8_t)(v->is_owner ? !v->readonly : 1);
            out->forceable = (uint8_t)(v->is_owner && v->allow_force);
            out->index = dart_topic_index(v->value);
            out->schema = dart_topic_schema(v->value);
            return 1;
        }
        case 2: {   /* signals */
            DartSignal *s = pm ? pm->sigs : NULL;
            for (skip = it->next_index; s && skip; skip--) s = s->next;
            if (!s){ it->phase = 3; it->next_index = 0; continue; }
            it->next_index++;
            if (i_dart_pat_hidden_name(i_dart_topic_name(s->topic))) continue;
            memset(out, 0, sizeof *out);
            out->kind = DART_ENTITY_SIGNAL;
            out->name = i_dart_topic_name(s->topic);
            { uint8_t role = i_dart_topic_role(s->topic);
              out->provides = (uint8_t)(role == DART_PUBSUB || role == DART_PUB_ONLY);
              out->consumes = (uint8_t)(role == DART_PUBSUB || role == DART_SUB_ONLY); }
            out->reliable = 1;
            out->index = dart_topic_index(s->topic);
            out->schema = dart_topic_schema(s->topic);
            return 1;
        }
        default: {  /* plain topics (pattern channels are covered by the lists above) */
            uint16_t count = i_dart_node_topic_count(n);
            while (it->next_index < count){
                DartTopic *t = dart_node_topic(n, it->next_index);
                it->next_index++;
                if (!t || i_dart_topic_kind(t) != DART_KIND_TOPIC
                       || i_dart_pat_hidden_name(i_dart_topic_name(t))) continue;
                memset(out, 0, sizeof *out);
                out->kind = DART_ENTITY_TOPIC;
                out->name = i_dart_topic_name(t);
                { uint8_t role = i_dart_topic_role(t);
                  out->provides = (uint8_t)(role == DART_PUBSUB || role == DART_PUB_ONLY);
                  out->consumes = (uint8_t)(role == DART_PUBSUB || role == DART_SUB_ONLY); }
                out->index = (uint16_t)(it->next_index - 1);
                out->schema = dart_topic_schema(t);
                return 1;
            }
            return 0;
        }
        }
    }
}
