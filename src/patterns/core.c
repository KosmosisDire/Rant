/* The patterns layer over the node's public API and its kind agnostic seams. The node
 * knows nothing of what these patterns mean. The rules are in spec/patterns.md. */
#include "core.h"
#include "../common/bytes.h"
#include <string.h>
#include <stddef.h>            /* offsetof */

/* the per node manager: fans the node wide event and tick out to every entity */

/* one authority side entity keyed the way a peer's announce names it, for the duplicate
 * authority check */
typedef struct {
    uint32_t    hash;          /* the primary channel's low 32 name hash */
    uint8_t     kind;          /* RambleEntityKind */
    RambleTopic  *primary;     /* the entity's primary channel */
    uint32_t  **ids; uint16_t *n_ids, *cap;   /* the entity's reported peers list */
} i_RamblePatAuth;

typedef struct i_RamblePatterns {
    RambleNode            *n;
    struct RambleFunction *funcs;   /* linked lists for the tick and event fanout */
    struct RambleVariable *vars;
    /* the authorities sorted by (kind, hash), rebuilt lazily after a create or retire */
    i_RamblePatAuth       *auth; uint16_t auth_n, auth_cap; uint8_t auth_dirty;
    struct RambleFunction *meta;    /* the built in @ramble/meta endpoint, both sides in one handle */
    RambleSchema          *meta_rsp_schema;   /* RambleMeta { info: map } */
    uint8_t               *meta_msg; uint32_t meta_msg_cap;   /* reply scratch, grown on demand */
} i_RamblePatterns;

static void     i_ramble_patterns_on_event(void *user, const RambleEvent *ev);
static uint64_t i_ramble_patterns_tick(void *user, uint64_t now_us);
static void     i_ramble_patterns_on_close(void *user);
/* the pending call reaper, retire cancels through it */
static uint64_t i_ramble_func_reap(struct RambleFunction *fn, uint64_t now,
                                 RambleCallStatus fail_status, int all, uint32_t dest);
/* the duplicate authority sweep for a just created provider or owner */
static void i_ramble_pat_dup_sweep(RambleNode *n, RambleTopic *primary, RambleEntityKind kind,
                                 uint32_t **ids, uint16_t *n_ids, uint16_t *cap);
/* a peer's uuid low 32, the task wire's caller discriminator */
static uint32_t i_ramble_pat_peer_lo(RambleNode *n, uint32_t peer){
    uint8_t u[16];
    return i_ramble_node_peer_uuid(n, peer, u) ? i_ramble_le_r32(u) : 0;
}

/* the entity channel belongs to as peer advertises it, by kind and base name. 1 + *out */
static int i_ramble_pat_peer_entity(RambleNode *n, uint32_t peer, RambleTopic *channel, RambleEntityKind kind,
                                  RambleEntityInfo *out){
    RambleString nm = i_ramble_topic_name(channel);
    RambleIter it; size_t len = nm.len;
    char buf[RAMBLE_TOPIC_NAME_MAX + 1]; uint32_t hash;
    memcpy(buf, nm.data, nm.len); buf[nm.len] = '\0';
    hash = (uint32_t)ramble_topic_id(buf);           /* the primary channel's announce hash */
    if (len >= 5 && nm.data[len - 4] == '@') len -= 4;
    memset(&it, 0, sizeof it);
    while (ramble_node_entities_next(n, peer, &it, out)){
        if (out->kind != kind) continue;
        /* rivals never exchange details, so the peer's name may never arrive: the hash
           decides then, the fetched name otherwise */
        if (out->name.len ? (out->name.len == len && memcmp(out->name.data, nm.data, len) == 0)
                          : out->hash == hash)
            return 1;
    }
    return 0;
}

/* Lazily creates the manager and registers the sys hooks. Lock held. NULL on OOM. */
static i_RamblePatterns *i_ramble_patterns_get(RambleNode *n){
    void **slot = i_ramble_node_sys_slot(n);
    i_RamblePatterns *pm = (i_RamblePatterns*)*slot;
    if (pm) return pm;
    pm = (i_RamblePatterns*)i_ramble_node_sys_alloc(n, NULL, sizeof *pm);
    if (!pm) return NULL;
    memset(pm, 0, sizeof *pm);
    pm->n = n; *slot = pm;
    i_ramble_node_set_sys_hooks(n, i_ramble_patterns_on_event, i_ramble_patterns_tick,
                              i_ramble_patterns_on_close, pm);
    return pm;
}

/* the shared entity mechanics: the create prologue and the three retire phases */

/* The create prologue: take the lock, get the manager, allocate a zeroed handle, release.
 * The lock must be released before the topics are created. See spec/patterns.md. */
static void *i_ramble_pat_handle_new(RambleNode *n, size_t size, i_RamblePatterns **pm_out){
    i_RamblePatterns *pm; void *h; int acquired;
    acquired = i_ramble_node_sys_lock(n);
    pm = i_ramble_patterns_get(n);
    h = pm ? i_ramble_node_sys_alloc(n, NULL, size) : NULL;
    if (h) memset(h, 0, size);
    i_ramble_node_sys_unlock(n, acquired);
    *pm_out = h ? pm : NULL;
    return h;
}

/* A partial create: the created half may already route into the handle, so park it and
 * keep the handle allocated until close. Returns NULL, the create's verdict. */
static void *i_ramble_pat_half_create_fail(RambleTopic *created_half){
    ramble_topic_set_role(created_half, RAMBLE_INACTIVE);
    return NULL;
}

/* Retire phase 1, before the lock: park the channels. set_role refuses from a callback
 * with nothing mutated, so the primary's verdict is the whole retire's. */
static int i_ramble_pat_park_channels(RambleTopic *primary, RambleTopic *secondary){
    int r = ramble_topic_set_role(primary, RAMBLE_INACTIVE);
    if (r != 0) return r;
    if (secondary) (void)ramble_topic_set_role(secondary, RAMBLE_INACTIVE);
    return 0;
}

/* Retire phase 2, lock held: drop the delivery routing into the handle. */
static void i_ramble_pat_clear_channels(RambleTopic *primary, RambleTopic *secondary){
    i_ramble_topic_clear_sys(primary);
    if (secondary) i_ramble_topic_clear_sys(secondary);
}

/* Retire phase 3, lock released: release the slots for reuse. The topics outlive the
 * handle free, since a reap callback fired under the lock may have used them. */
static void i_ramble_pat_release_channels(RambleTopic *primary, RambleTopic *secondary){
    (void)ramble_topic_retire(primary);
    if (secondary) (void)ramble_topic_retire(secondary);
}

/* Unlinks elem from a manager list threaded at next_off. An absent elem is a no op. Lock held. */
static void i_ramble_pat_unlink(void **head, void *elem, size_t next_off){
    void **pp = head;
    while (*pp){
        void **next = (void**)((char*)*pp + next_off);
        if (*pp == elem){ *pp = *next; return; }
        pp = next;
    }
}

/* functions */

#define RAMBLE__FN_PREFIX 5u   /* req: [u32 call_id][u8 op]. rsp: [u32 call_id][u8 status], then
                                [u8 msg_len][msg], which the node's split puts in the header */
#define RAMBLE__FN_RSP_HDR_MAX (RAMBLE__FN_PREFIX + 1u + RAMBLE_CALL_MSG_MAX)
#define RAMBLE__FN_OP_CALL   0u   /* the payload is the request */
#define RAMBLE__FN_OP_CANCEL 1u   /* task req channel, op only: an empty payload cancels the
                                   sender's call, [u32 caller_lo] another caller's */
#define RAMBLE__PRG_PREFIX 8u  /* prg: [u32 caller_lo][u32 call_id]. caller_lo demuxes the shared
                                broadcast, since call ids are per caller counters */
#define RAMBLE__NO_DEADLINE ((uint64_t)-1)   /* a RUNNING call has no timeout */

/* the default response text, so a generic consumer always has text for a failure */
static RambleString i_ramble_call_status_msg(RambleCallStatus s){
    switch (s){
    case RAMBLE_CALL_APP_ERROR:  return ramble_cstr("app error");
    case RAMBLE_CALL_NO_HANDLER: return ramble_cstr("no handler");
    case RAMBLE_CALL_TIMEOUT:    return ramble_cstr("timeout");
    case RAMBLE_CALL_PEER_LOST:  return ramble_cstr("peer lost");
    case RAMBLE_CALL_CANCELLED:  return ramble_cstr("cancelled");
    case RAMBLE_CALL_OK: default: return ramble_cstr("");
    }
}

/* A caller side outstanding call. queued_req != NULL = not on the wire yet: the request
 * waits for the first provider match. See spec/patterns.md. */
typedef struct i_RamblePending {
    struct i_RamblePending *next;
    uint32_t          call_id;
    uint32_t          dest;        /* the peer the call is directed at, 0 = undirected */
    uint64_t          deadline_us; /* RAMBLE__NO_DEADLINE once RUNNING or progress arrived */
    RambleResponseFn  on_response;
    void           *user;
    RambleProgressFn  on_progress; /* task: per progress update */
    void           *progress_user;
    uint8_t         running;     /* a non terminal response arrived, the deadline is dropped */
    uint8_t        *queued_req;
    uint32_t        queued_len;
} i_RamblePending;

/* a deferred provider reply, linked into the handle's live defer registry so every token
 * verb validates membership first */
typedef struct i_RambleDefer {
    struct i_RambleDefer *next;
    RambleFunction *fn;
    uint32_t      caller;
    uint32_t      caller_lo;   /* the caller's uuid low 32 for progress and cancel */
    uint32_t      call_id;
    uint8_t       cancelled;   /* a cancel landed */
} i_RambleDefer;

struct RambleFunction {
    RambleNode       *n;
    i_RamblePatterns *pm;
    struct RambleFunction *next;      /* the manager list */
    RambleTopic      *req;            /* provider SUB_ONLY, caller PUB_ONLY */
    RambleTopic      *rsp;            /* provider PUB_ONLY, caller SUB_ONLY, directed */
    RambleTopic      *prg;            /* task: the broadcast progress channel, NULL = a function */
    RambleRequestFn   on_request; void *on_request_user;   /* provider */
    uint32_t          next_call_id;   /* the caller counter */
    uint32_t          timeout_us;
    i_RamblePending  *pending;        /* caller: outstanding calls */
    i_RambleDefer    *defers;         /* provider: the live defer registry */
    RambleCancelFn    on_cancel; void *on_cancel_user;     /* task definition, one slot */
    uint32_t          self_lo;        /* our own uuid low 32, the progress demux filter */
    uint8_t        *sync_buf; uint32_t sync_cap;     /* the sync call reply scratch */
    char            sync_msg[RAMBLE_CALL_MSG_MAX];   /* the sync call message scratch */
    uint8_t         sync_msg_len;
    uint8_t         is_provider;    /* a pure definition, 0 for a both sides handle */
    uint8_t         both;           /* both sides in one handle, the @ramble/meta shape */
    uint8_t         is_task;        /* the task shape */
    uint8_t         no_cancel, exclusive;   /* task definition: the declared attrs */
    uint8_t         multi;          /* duplicate authority expected */
    uint32_t       *dup_peers;      /* provider: peers already reported for duplicate authority */
    uint16_t        dup_n, dup_cap;
    uint8_t         reflect;        /* created with reflect_from_mesh */
    uint64_t        generation;     /* the mesh generation the schemas were taken at */
};

/* the transient request handed to the handler: the public head first, so the reply entry
 * points can downcast the handler's pointer back to this */
typedef struct {
    RambleRequest   pub;
    RambleFunction *fn;
    uint32_t      call_id;
    uint32_t      caller_lo;   /* task: the caller's uuid low 32 */
    uint8_t       replied;     /* a reply, fail or defer happened: no auto ack */
    uint8_t       started;     /* task: RUNNING already sent */
} i_RambleRequest;
/* the downcast needs pub at offset 0, and C99 has no static assert */
typedef char i_ramble_request_pub_first[(offsetof(i_RambleRequest, pub) == 0) ? 1 : -1];

/* builds the response header. A message over RAMBLE_CALL_MSG_MAX truncates. Returns the length */
static size_t i_ramble_func_rsp_hdr(uint8_t *hdr, uint32_t call_id, uint8_t status,
                                  const char *message){
    size_t ml = message ? strlen(message) : 0;
    if (ml > RAMBLE_CALL_MSG_MAX) ml = RAMBLE_CALL_MSG_MAX;
    i_ramble_le_w32(hdr, call_id); hdr[4] = status; hdr[5] = (uint8_t)ml;
    if (ml) memcpy(hdr + 6, message, ml);
    return RAMBLE__FN_PREFIX + 1u + ml;
}

/* sends a response to one caller. Runs under the lock, so the send commits reentrantly */
static void i_ramble_func_send_reply(RambleFunction *fn, uint32_t caller, uint32_t call_id,
                                   uint8_t status, const char *message, RambleBytes rsp){
    uint8_t hdr[RAMBLE__FN_RSP_HDR_MAX];
    size_t hl = i_ramble_func_rsp_hdr(hdr, call_id, status, message);
    (void)i_ramble_topic_send_to(fn->rsp, caller, ramble_bytes(hdr, hl), rsp);
}

/* provider: a CANCEL op landed. The target is the payload's caller_lo, else the sender's.
 * A match on a live defer sets its flag and fires on_cancel once, no match is a no op. */
static void i_ramble_func_cancel_op(RambleFunction *fn, const RambleMsg *msg){
    uint32_t lo = msg->data.len >= 4 ? i_ramble_le_r32(msg->data.data)
                                     : i_ramble_pat_peer_lo(msg->node, msg->publisher_id);
    uint32_t call_id = i_ramble_le_r32(msg->header.data);
    i_RambleDefer *d;
    for (d = fn->defers; d; d = d->next)
        if (d->caller_lo == lo && d->call_id == call_id) break;
    if (!d || d->cancelled) return;
    d->cancelled = 1;
    if (fn->on_cancel) fn->on_cancel((uint64_t)(uintptr_t)d, fn->on_cancel_user);
}

/* provider: a request arrived, or an op */
static void i_ramble_func_on_request(void *user, const RambleMsg *msg){
    RambleFunction *fn = (RambleFunction*)user;
    i_RambleRequest r;
    if (msg->header.len < RAMBLE__FN_PREFIX) return;   /* a malformed prefix */
    if (msg->header.data[4] != RAMBLE__FN_OP_CALL){    /* an op, not a request */
        if (fn->is_task && msg->header.data[4] == RAMBLE__FN_OP_CANCEL)
            i_ramble_func_cancel_op(fn, msg);
        return;   /* an unknown op is dropped */
    }
    r.pub.node          = msg->node;
    r.pub.function_name = ramble_string(msg->topic_name.data,
                              msg->topic_name.len > 4u ? msg->topic_name.len - 4u : 0);
    r.pub.data          = msg->data;
    r.pub.schema        = msg->schema;
    r.pub.caller        = msg->publisher_id;
    r.pub.caller_name   = msg->publisher_name;
    r.pub.recv_us       = msg->recv_us;
    r.pub.written_us    = msg->written_us;
    r.fn = fn; r.call_id = i_ramble_le_r32(msg->header.data); r.replied = 0; r.started = 0;
    r.caller_lo = fn->is_task ? i_ramble_pat_peer_lo(msg->node, msg->publisher_id) : 0;
    if (fn->on_request) fn->on_request(&r.pub, fn->on_request_user);
    else {   /* no handler: NO_HANDLER with no text, the caller fills the default */
        i_ramble_func_send_reply(fn, r.pub.caller, r.call_id, RAMBLE_CALL_NO_HANDLER, NULL, ramble_bytes(NULL,0));
        r.replied = 1;
    }
    if (!r.replied){
        /* a function's return is its answer, a task's is not: an instant empty OK on a
           long operation would read as success that never ran */
        if (fn->is_task)
            i_ramble_func_send_reply(fn, r.pub.caller, r.call_id, RAMBLE_CALL_APP_ERROR,
                                   "handler returned no result", ramble_bytes(NULL,0));
        else
            i_ramble_func_send_reply(fn, r.pub.caller, r.call_id, RAMBLE_CALL_OK, NULL, ramble_bytes(NULL,0));
    }
}

/* caller: unlinks the pending entry for call_id, so a second provider's reply finds none */
static i_RamblePending *i_ramble_func_take_pending(RambleFunction *fn, uint32_t call_id){
    i_RamblePending **pp = &fn->pending, *p;
    for (; (p = *pp) != NULL; pp = &p->next)
        if (p->call_id == call_id){ *pp = p->next; return p; }
    return NULL;
}

/* caller: the pending entry for call_id, left linked */
static i_RamblePending *i_ramble_func_find_pending(RambleFunction *fn, uint32_t call_id){
    i_RamblePending *p;
    for (p = fn->pending; p; p = p->next) if (p->call_id == call_id) return p;
    return NULL;
}

/* a non terminal response reached p: the call runs as long as it runs now */
static void i_ramble_func_mark_running(i_RamblePending *p){
    p->running = 1; p->deadline_us = RAMBLE__NO_DEADLINE;
}

/* frees an unlinked pending entry and any queued request bytes */
static void i_ramble_func_free_pending(RambleFunction *fn, i_RamblePending *p){
    if (p->queued_req) i_ramble_node_sys_alloc(fn->n, p->queued_req, 0);
    i_ramble_node_sys_alloc(fn->n, p, 0);
}

/* Sends every call queued before a provider matched, oldest first. Runs under the lock,
 * so the sends commit reentrantly, fine on a fresh lane with empty history. */
static void i_ramble_func_flush_queued(RambleFunction *fn){
    if (fn->is_provider || !fn->pending) return;
    if (ramble_topic_match_count(fn->req) == 0) return;   /* still no provider */
    for (;;){
        i_RamblePending *pick = NULL, *p;
        uint8_t hdr[RAMBLE__FN_PREFIX];
        for (p = fn->pending; p; p = p->next) if (p->queued_req) pick = p;
        if (!pick) return;
        if (fn->is_task && !pick->dest)   /* task requests are always directed: resolve now */
            pick->dest = i_ramble_topic_oldest_match(fn->req);
        i_ramble_le_w32(hdr, pick->call_id); hdr[4] = RAMBLE__FN_OP_CALL;
        if (pick->dest)
            (void)i_ramble_topic_send_to(fn->req, pick->dest, ramble_bytes(hdr, RAMBLE__FN_PREFIX),
                                       ramble_bytes(pick->queued_req, pick->queued_len));
        else
            (void)i_ramble_topic_send_hdr(fn->req, ramble_bytes(hdr, RAMBLE__FN_PREFIX),
                                        ramble_bytes(pick->queued_req, pick->queued_len));
        i_ramble_node_sys_alloc(fn->n, pick->queued_req, 0);
        pick->queued_req = NULL; pick->queued_len = 0;
    }
}

/* caller: a progress datagram. caller_lo filters other callers' calls off the broadcast.
 * Progress can beat RUNNING on its own lane, so it also drops the deadline. */
static void i_ramble_func_on_progress(void *user, const RambleMsg *msg){
    RambleFunction *fn = (RambleFunction*)user;
    i_RamblePending *p;
    RambleProgress pr;
    if (msg->header.len < RAMBLE__PRG_PREFIX) return;
    if (i_ramble_le_r32(msg->header.data) != fn->self_lo) return;   /* another caller's call */
    p = i_ramble_func_find_pending(fn, i_ramble_le_r32(msg->header.data + 4));
    if (!p) return;   /* already answered: a straggler */
    i_ramble_func_mark_running(p);
    if (!p->on_progress) return;
    pr.call_id = p->call_id; pr.provider = msg->publisher_id;
    pr.data = msg->data; pr.schema = msg->schema;
    pr.written_us = msg->written_us; pr.recv_us = msg->recv_us;
    pr.user = p->progress_user;
    p->on_progress(&pr);
}

/* caller: a response arrived */
static void i_ramble_func_on_response(void *user, const RambleMsg *msg){
    RambleFunction *fn = (RambleFunction*)user;
    i_RamblePending *p;
    RambleResponse r;
    if (msg->header.len < RAMBLE__FN_PREFIX) return;
    if (fn->is_task && (RambleCallStatus)msg->header.data[4] == RAMBLE_CALL_RUNNING){
        /* non terminal: the call stays pending with no deadline and on_progress fires once
           with zero length data, unless progress already beat the status here */
        p = i_ramble_func_find_pending(fn, i_ramble_le_r32(msg->header.data));
        if (!p) return;
        if (!p->running && p->on_progress){
            RambleProgress pr;
            pr.call_id = p->call_id; pr.provider = msg->publisher_id;
            pr.data = ramble_bytes(NULL, 0); pr.schema = NULL;
            pr.written_us = msg->written_us; pr.recv_us = msg->recv_us;
            pr.user = p->progress_user;
            i_ramble_func_mark_running(p);
            p->on_progress(&pr);
        } else i_ramble_func_mark_running(p);
        return;
    }
    p = i_ramble_func_take_pending(fn, i_ramble_le_r32(msg->header.data));
    if (!p) return;                          /* an unknown or duplicate call_id: dropped */
    r.status = (RambleCallStatus)msg->header.data[4];
    r.data = msg->data; r.schema = msg->schema;
    r.provider = msg->publisher_id; r.user = p->user;
    r.written_us = msg->written_us;
    /* the message follows the prefix as [u8 len][msg], and the node's split already
       bounded it, so the header length is the truth. None = the default status text */
    r.message = (msg->header.len > RAMBLE__FN_PREFIX + 1u)
              ? ramble_string((const char*)msg->header.data + RAMBLE__FN_PREFIX + 1u,
                            msg->header.len - (RAMBLE__FN_PREFIX + 1u))
              : i_ramble_call_status_msg(r.status);
    if (p->on_response) p->on_response(&r);
    i_ramble_func_free_pending(fn, p);
}

/* a leading '@' is reserved for the @ramble/ builtins, refused in every public constructor */
static int i_ramble_pat_reserved(const char *name){ return name && name[0] == '@'; }

/* Creates the channels for a function or task. mode 1 = a definition, 0 = a remote, 2 =
 * both sides in one handle (the @ramble/meta shape). topts non NULL = the task shape. */
static RambleFunction *i_ramble_function_new(RambleNode *n, const char *name,
                    const RambleSchema *req_schema, const RambleSchema *rsp_schema,
                    RambleRequestFn on_request, void *user, const RambleFunctionOpts *opts, int mode,
                    const RambleSchema *prg_schema, const RambleTaskOpts *topts){
    i_RamblePatterns *pm; RambleFunction *fn;
    char rn[RAMBLE_TOPIC_NAME_MAX + 1]; size_t nl;
    RambleTopicOpts topt; int acquired;
    int handles_req = (mode != 0), makes_calls = (mode != 1);
    RambleRole req_role = mode == 2 ? RAMBLE_PUBSUB : (handles_req ? RAMBLE_SUB_ONLY : RAMBLE_PUB_ONLY);
    RambleRole rsp_role = mode == 2 ? RAMBLE_PUBSUB : (handles_req ? RAMBLE_PUB_ONLY : RAMBLE_SUB_ONLY);
    i_RambleSysMsgFn req_cb = handles_req ? i_ramble_func_on_request : NULL;
    i_RambleSysMsgFn rsp_cb = makes_calls ? i_ramble_func_on_response : NULL;
    uint8_t req_kind = topts ? RAMBLE_KIND_TASK_REQ : RAMBLE_KIND_FUNC_REQ;
    uint8_t rsp_kind = topts ? RAMBLE_KIND_TASK_RSP : RAMBLE_KIND_FUNC_RSP;
    uint8_t req_attrs = 0;
    if (topts && handles_req)   /* only the definition declares the task facts */
        req_attrs = (uint8_t)((topts->no_cancel ? 0u : RAMBLE_ATTR_CANCELLABLE)
                            | (topts->exclusive ? RAMBLE_ATTR_EXCLUSIVE : 0u)
                            | (topts->multi     ? RAMBLE_ATTR_MULTI     : 0u));
    if (!n || !name) return NULL;
    nl = strlen(name);
    if (nl == 0 || nl + 4 > RAMBLE_TOPIC_NAME_MAX) return NULL;   /* room for the suffix */
    memset(&topt, 0, sizeof topt);
    topt.qos.reliability = RAMBLE_RELIABLE;
    topt.qos.catch_up = 0;   /* calls carry no replay: history is purely the repair window */
    /* an inline reply is a reentrant send that cannot wait for a TX pass, so this ring is
       the only thing holding a drained batch of replies. See spec/patterns.md */
    topt.qos.keep_last = (opts && opts->keep_last) ? opts->keep_last : 0u;
    topt.qos.backpressure_wait_us = (opts && opts->backpressure_wait_us) ? opts->backpressure_wait_us
                                                                         : RAMBLE_PATTERN_BP_WAIT_US;
    fn = (RambleFunction*)i_ramble_pat_handle_new(n, sizeof *fn, &pm);
    if (!fn) return NULL;
    if (opts && opts->reflect_from_mesh){
        /* the caller writes @req and reads @rsp and @prg, a definition the reverse */
        RambleEntityKind ek = topts ? RAMBLE_ENTITY_TASK : RAMBLE_ENTITY_FUNCTION;
        const RambleSchema *ms;
        fn->reflect = 1;
        i_ramble_node_reflect_pick(n, ek, name, 0, !handles_req, &ms, NULL, &fn->generation);
        if (!req_schema) req_schema = ms;
        i_ramble_node_reflect_pick(n, ek, name, 1, handles_req, &ms, NULL, NULL);
        if (!rsp_schema) rsp_schema = ms;
        if (topts){
            i_ramble_node_reflect_pick(n, ek, name, 2, handles_req, &ms, NULL, NULL);
            if (!prg_schema) prg_schema = ms;
        }
    }
    fn->n = n; fn->pm = pm; fn->is_provider = (uint8_t)(mode == 1);
    fn->both = (uint8_t)(mode == 2);
    fn->multi = (uint8_t)(opts && opts->multi);
    fn->is_task = (uint8_t)(topts != NULL);
    if (topts){ fn->no_cancel = topts->no_cancel; fn->exclusive = topts->exclusive; }
    fn->on_request = on_request; fn->on_request_user = user;
    fn->timeout_us = (opts && opts->timeout_us) ? opts->timeout_us : RAMBLE_CALL_TIMEOUT_US;
    fn->next_call_id = 1;
    {   const uint8_t *u = i_ramble_node_uuid(n);   /* the progress demux filter */
        fn->self_lo = u ? i_ramble_le_r32(u) : 0;
    }

    memcpy(rn, name, nl); memcpy(rn + nl, "@req", 5);   /* NUL included */
    /* the req channel is directed in every mode, or a directed request would leak to every
       other matched provider when their lanes next wake. See spec/patterns.md */
    fn->req = i_ramble_node_create_pattern_topic(n, rn, req_role, req_schema, &topt,
                              req_kind, RAMBLE__FN_PREFIX, 1 /*directed*/, req_attrs, req_cb, fn);
    if (!fn->req) return NULL;   /* fn stays pool allocated: nothing routes into it yet */
    if (topts){
        /* the broadcast progress channel: reliability is the definition's offer or the
           remote's request, the RxO rule composes them */
        RambleTopicOpts popt = topt;
        popt.qos.reliability = topts->progress_best_effort ? RAMBLE_BEST_EFFORT : RAMBLE_RELIABLE;
        if (topts->progress_keep_last) popt.qos.keep_last = topts->progress_keep_last;
        memcpy(rn + nl, "@prg", 5);
        fn->prg = i_ramble_node_create_pattern_topic(n, rn,
                              handles_req ? RAMBLE_PUB_ONLY : RAMBLE_SUB_ONLY, prg_schema, &popt,
                              RAMBLE_KIND_TASK_PRG, RAMBLE__PRG_PREFIX, 0, 0,
                              handles_req ? NULL : i_ramble_func_on_progress, fn);
        if (!fn->prg)
            return (RambleFunction*)i_ramble_pat_half_create_fail(fn->req);
    }
    memcpy(rn + nl, "@rsp", 5);
    fn->rsp = i_ramble_node_create_pattern_topic(n, rn, rsp_role, rsp_schema, &topt,
                              rsp_kind, RAMBLE__FN_PREFIX, 1 /*directed*/, 0, rsp_cb, fn);
    if (!fn->rsp){  /* req and prg already route into fn: keep the handle, park the halves */
        if (fn->prg) (void)ramble_topic_set_role(fn->prg, RAMBLE_INACTIVE);
        return (RambleFunction*)i_ramble_pat_half_create_fail(fn->req);
    }

    acquired = i_ramble_node_sys_lock(n);       /* publish into the manager list */
    fn->next = pm->funcs; pm->funcs = fn; pm->auth_dirty = 1;
    if (handles_req && !fn->multi)   /* a rival provider may already be on the network */
        i_ramble_pat_dup_sweep(n, fn->req, topts ? RAMBLE_ENTITY_TASK : RAMBLE_ENTITY_FUNCTION,
                             &fn->dup_peers, &fn->dup_n, &fn->dup_cap);
    i_ramble_node_sys_unlock(n, acquired);
    return fn;
}

RambleFunction *ramble_node_create_function_definition(RambleNode *n, const char *name,
                    const RambleSchema *req_schema, const RambleSchema *rsp_schema,
                    RambleRequestFn on_request, void *user, const RambleFunctionOpts *opts){
    if (i_ramble_pat_reserved(name)) return NULL;
    return i_ramble_function_new(n, name, req_schema, rsp_schema, on_request, user, opts, 1, NULL, NULL);
}
RambleFunction *ramble_node_create_remote_function(RambleNode *n, const char *name,
                    const RambleSchema *req_schema, const RambleSchema *rsp_schema,
                    const RambleFunctionOpts *opts){
    if (i_ramble_pat_reserved(name)) return NULL;
    return i_ramble_function_new(n, name, req_schema, rsp_schema, NULL, NULL, opts, 0, NULL, NULL);
}

/* a pattern channel's entity name, the topic name minus its suffix, in a static scratch */
static const char *i_ramble_pat_base_name(RambleTopic *channel){
    static char buf[RAMBLE_TOPIC_NAME_MAX + 1];
    RambleString nm = i_ramble_topic_name(channel);
    size_t len = nm.len;
    if (len >= 5 && nm.data[len - 4] == '@') len -= 4;
    memcpy(buf, nm.data, len); buf[len] = '\0';
    return buf;
}

/* the shared function fields of RambleTaskOpts, so the task constructors reuse one path */
static RambleFunctionOpts i_ramble_task_fn_opts(const RambleTaskOpts *to){
    RambleFunctionOpts fo;
    memset(&fo, 0, sizeof fo);
    fo.backpressure_wait_us = to->backpressure_wait_us;
    fo.timeout_us = to->timeout_us;
    fo.keep_last = to->keep_last;
    fo.multi = to->multi;
    return fo;
}

RambleFunction *ramble_node_create_task_definition(RambleNode *n, const char *name,
                    const RambleSchema *req_schema, const RambleSchema *prg_schema,
                    const RambleSchema *rsp_schema, RambleRequestFn on_request, void *user,
                    const RambleTaskOpts *opts){
    RambleTaskOpts to; RambleFunctionOpts fo;
    if (i_ramble_pat_reserved(name)) return NULL;
    memset(&to, 0, sizeof to); if (opts) to = *opts;
    fo = i_ramble_task_fn_opts(&to);
    return i_ramble_function_new(n, name, req_schema, rsp_schema, on_request, user, &fo, 1,
                               prg_schema, &to);
}
RambleFunction *ramble_node_create_remote_task(RambleNode *n, const char *name,
                    const RambleSchema *req_schema, const RambleSchema *prg_schema,
                    const RambleSchema *rsp_schema, const RambleTaskOpts *opts){
    RambleTaskOpts to; RambleFunctionOpts fo;
    if (i_ramble_pat_reserved(name)) return NULL;
    memset(&to, 0, sizeof to); if (opts) to = *opts;
    fo = i_ramble_task_fn_opts(&to);
    return i_ramble_function_new(n, name, req_schema, rsp_schema, NULL, NULL, &fo, 0,
                               prg_schema, &to);
}

/* Links the pending entry under the lock, then sends outside it so the request engages the
 * flow control wait. A task call with no provider auto directs at the oldest matched. */
static int i_ramble_function_call_id(RambleFunction *fn, RambleBytes req, RambleResponseFn on_response,
                                   void *user, const RambleCallOpts *opts, uint32_t *id_out){
    uint8_t hdr[RAMBLE__FN_PREFIX]; i_RamblePending *p; int acquired, r; uint32_t id;
    uint32_t dest = opts ? opts->provider : 0;
    if (!fn || !fn->req || !fn->rsp) return RAMBLE_ERR_NO_TOPIC;
    acquired = i_ramble_node_sys_lock(fn->n);
    p = (i_RamblePending*)i_ramble_node_sys_alloc(fn->n, NULL, sizeof *p);
    if (!p){ i_ramble_node_sys_unlock(fn->n, acquired); return RAMBLE_ERR_OOM; }
    if (fn->is_task && !dest) dest = i_ramble_topic_oldest_match(fn->req);   /* always directed */
    id = fn->next_call_id++;
    p->call_id = id;
    p->dest = dest;
    p->deadline_us = i_ramble_node_now_us(fn->n) + fn->timeout_us;
    p->on_response = on_response; p->user = user;
    p->on_progress = opts ? opts->on_progress : NULL;
    p->progress_user = opts ? opts->progress_user : NULL;
    p->running = 0;
    p->queued_req = NULL; p->queued_len = 0;
    p->next = fn->pending; fn->pending = p;
    if (id_out) *id_out = id;
    if (opts && opts->id_out) *opts->id_out = id;   /* set before the send */
    if (ramble_topic_match_count(fn->req) == 0){
        /* no provider matched yet: the transport would drop an unmatched send, so queue
           the request and flush the instant the match forms. See spec/patterns.md */
        p->queued_req = (uint8_t*)i_ramble_node_sys_alloc(fn->n, NULL, req.len ? req.len : 1u);
        if (!p->queued_req){
            fn->pending = p->next;
            i_ramble_node_sys_alloc(fn->n, p, 0);
            i_ramble_node_sys_unlock(fn->n, acquired);
            return RAMBLE_ERR_OOM;
        }
        if (req.len) memcpy(p->queued_req, req.data, req.len);
        p->queued_len = (uint32_t)req.len;
        i_ramble_node_sys_unlock(fn->n, acquired);
        return RAMBLE_OK;
    }
    i_ramble_node_sys_unlock(fn->n, acquired);
    i_ramble_le_w32(hdr, id); hdr[4] = RAMBLE__FN_OP_CALL;
    r = dest ? i_ramble_topic_send_to(fn->req, dest, ramble_bytes(hdr, RAMBLE__FN_PREFIX), req)
             : i_ramble_topic_send_hdr(fn->req, ramble_bytes(hdr, RAMBLE__FN_PREFIX), req);
    if (r != RAMBLE_OK){
        acquired = i_ramble_node_sys_lock(fn->n);
        p = i_ramble_func_take_pending(fn, id);   /* may already be reaped or answered: then gone */
        if (p) i_ramble_func_free_pending(fn, p);
        i_ramble_node_sys_unlock(fn->n, acquired);
        return r;
    }
    return RAMBLE_OK;
}

int ramble_function_call_async(RambleFunction *fn, RambleBytes req, RambleResponseFn on_response,
                             void *user, const RambleCallOpts *opts){
    return i_ramble_function_call_id(fn, req, on_response, user, opts, NULL);
}

/* Answers and frees every live deferred call of a definition with CANCELLED. Lock held,
 * the replies commit reentrantly, so run it while the rsp channel is still up. */
static void i_ramble_func_drain_defers(RambleFunction *fn, const char *message){
    while (fn->defers){
        i_RambleDefer *d = fn->defers; fn->defers = d->next;
        i_ramble_func_send_reply(fn, d->caller, d->call_id, RAMBLE_CALL_CANCELLED,
                               message, ramble_bytes(NULL, 0));
        i_ramble_node_sys_alloc(fn->n, d, 0);
    }
}

int ramble_function_retire(RambleFunction *fn){
    i_RamblePatterns *pm; RambleNode *n; int acquired, r;
    RambleTopic *req, *rsp, *prg;
    if (!fn) return RAMBLE_ERR_NO_TOPIC;
    pm = fn->pm; n = fn->n;
    if (pm && fn == pm->meta) return RAMBLE_ERR_STATE;   /* the builtin is node infrastructure */
    acquired = i_ramble_node_sys_lock(n);
    if (!acquired){   /* from a callback: refuse with nothing mutated */
        i_ramble_node_sys_unlock(n, acquired);
        return RAMBLE_ERR_STATE;
    }
    /* live deferred calls answer CANCELLED while the channels are still up, and the
       replies must flush to the wire before the park below tears the lanes down */
    i_ramble_func_drain_defers(fn, "provider retired");
    i_ramble_node_sys_unlock(n, acquired);
    i_ramble_node_flush_tx(n);
    r = i_ramble_pat_park_channels(fn->req, fn->rsp);
    if (r != 0) return r;
    if (fn->prg) (void)ramble_topic_set_role(fn->prg, RAMBLE_INACTIVE);
    acquired = i_ramble_node_sys_lock(n);
    i_ramble_pat_clear_channels(fn->req, fn->rsp);
    if (fn->prg) i_ramble_topic_clear_sys(fn->prg);
    /* a request that deferred in the drain to park window: a best effort answer, and the
       entries are freed either way */
    i_ramble_func_drain_defers(fn, "provider retired");
    /* every outstanding call gets its one outcome, CANCELLED. A reentrant call from a
       cancel callback queues and the next round cancels it too */
    while (fn->pending) i_ramble_func_reap(fn, 0, RAMBLE_CALL_CANCELLED, 1, 0);
    if (pm){ i_ramble_pat_unlink((void**)&pm->funcs, fn, offsetof(RambleFunction, next)); pm->auth_dirty = 1; }
    req = fn->req; rsp = fn->rsp; prg = fn->prg;   /* outlive fn, see phase 3 */
    if (fn->sync_buf)  i_ramble_node_sys_alloc(n, fn->sync_buf, 0);
    if (fn->dup_peers) i_ramble_node_sys_alloc(n, fn->dup_peers, 0);
    i_ramble_node_sys_alloc(n, fn, 0);
    i_ramble_node_sys_unlock(n, acquired);
    i_ramble_pat_release_channels(req, rsp);
    if (prg) (void)ramble_topic_retire(prg);
    return RAMBLE_OK;
}

int ramble_function_refresh(RambleFunction *fn){
    RambleNode *n; RambleEntityKind ek; uint64_t gen = 0; int acquired, r;
    const RambleSchema *rq = NULL, *rs = NULL, *pg = NULL;
    if (!fn) return RAMBLE_ERR_NO_TOPIC;
    if (!fn->reflect) return RAMBLE_ERR_ROLE;
    n = fn->n; ek = fn->is_task ? RAMBLE_ENTITY_TASK : RAMBLE_ENTITY_FUNCTION;
    if (!i_ramble_node_reflect_pick(n, ek, i_ramble_pat_base_name(fn->req), 0, !fn->is_provider, &rq, NULL, &gen)
        || gen == fn->generation) return 0;
    i_ramble_node_reflect_pick(n, ek, i_ramble_pat_base_name(fn->req), 1, fn->is_provider, &rs, NULL, NULL);
    if (fn->prg) i_ramble_node_reflect_pick(n, ek, i_ramble_pat_base_name(fn->req), 2, fn->is_provider, &pg, NULL, NULL);
    /* every outstanding call gets its one outcome before the lanes go */
    acquired = i_ramble_node_sys_lock(n);
    if (!acquired){ i_ramble_node_sys_unlock(n, acquired); return RAMBLE_ERR_STATE; }
    i_ramble_func_drain_defers(fn, "provider re-typed");
    while (fn->pending) i_ramble_func_reap(fn, 0, RAMBLE_CALL_CANCELLED, 1, 0);
    i_ramble_node_sys_unlock(n, acquired);
    i_ramble_node_flush_tx(n);
    r = i_ramble_topic_retype(fn->req, rq, RAMBLE_RELIABLE);
    if (r != 0) return r;
    r = i_ramble_topic_retype(fn->rsp, rs, RAMBLE_RELIABLE);
    if (r != 0) return r;
    if (fn->prg){
        r = i_ramble_topic_retype(fn->prg, pg, i_ramble_topic_reliability(fn->prg));
        if (r != 0) return r;
    }
    fn->generation = gen;
    return 1;
}

/* The blocking call's response capture: copies the payload into the function's scratch.
 * The schema pointer is safe to hold, an interned schema lives until close. */
typedef struct { RambleFunction *fn; volatile int done; RambleCallStatus status;
                 const RambleSchema *schema; uint32_t len; uint32_t provider;
                 uint64_t written_us; } i_RambleSyncCtx;
static void i_ramble_func_sync_response(const RambleResponse *r){
    i_RambleSyncCtx *c = (i_RambleSyncCtx*)r->user;
    RambleFunction *fn = c->fn;
    c->status = r->status; c->schema = r->schema; c->len = (uint32_t)r->data.len;
    c->provider = r->provider; c->written_us = r->written_us;
    {   /* the message view dies with the callback: copy into the handle's scratch */
        size_t ml = r->message.len <= RAMBLE_CALL_MSG_MAX ? r->message.len : RAMBLE_CALL_MSG_MAX;
        if (ml) memcpy(fn->sync_msg, r->message.data, ml);
        fn->sync_msg_len = (uint8_t)ml;
    }
    if (r->data.len){
        if (fn->sync_cap < r->data.len){
            uint8_t *nb = (uint8_t*)i_ramble_node_sys_alloc(fn->n, fn->sync_buf, r->data.len);
            if (nb){ fn->sync_buf = nb; fn->sync_cap = (uint32_t)r->data.len; }
            else c->len = 0;
        }
        if (fn->sync_cap >= r->data.len) memcpy(fn->sync_buf, r->data.data, r->data.len);
    }
    c->done = 1;
}

int ramble_function_call(RambleFunction *fn, RambleBytes req, RambleResponse *out, int timeout_ms,
                       const RambleCallOpts *opts){
    i_RambleSyncCtx ctx; int acquired, r; uint64_t deadline; uint32_t id;
    if (!fn) return RAMBLE_ERR_NO_TOPIC;
    acquired = i_ramble_node_sys_lock(fn->n);
    if (!acquired || ramble_node_is_started(fn->n)){   /* a callback or a service thread */
        i_ramble_node_sys_unlock(fn->n, acquired);
        return RAMBLE_ERR_STATE;
    }
    i_ramble_node_sys_unlock(fn->n, acquired);
    ctx.fn = fn; ctx.done = 0; ctx.status = RAMBLE_CALL_TIMEOUT; ctx.schema = NULL; ctx.len = 0;
    ctx.provider = 0; ctx.written_us = 0;
    r = i_ramble_function_call_id(fn, req, i_ramble_func_sync_response, &ctx, opts, &id);
    if (r != RAMBLE_OK) return r;
    deadline = i_ramble_node_now_us(fn->n)
             + (uint64_t)(timeout_ms >= 0 ? (uint64_t)timeout_ms * 1000u : fn->timeout_us) + 20000u;
    while (!ctx.done){
        if (i_ramble_node_now_us(fn->n) >= deadline){
            /* a task that reached RUNNING has no deadline: wait for the terminal outcome,
               and cancel from another thread is the impatience tool */
            int running = 0;
            acquired = i_ramble_node_sys_lock(fn->n);
            {   i_RamblePending *p = i_ramble_func_find_pending(fn, id);
                running = p && p->running;
            }
            i_ramble_node_sys_unlock(fn->n, acquired);
            if (!running) break;
            deadline = RAMBLE__NO_DEADLINE;
        }
        i_ramble_node_sys_poll(fn->n, 5);            /* the tick synthesizes TIMEOUT */
    }
    if (!ctx.done){
        /* the local wait expired first: unlink the entry before this frame dies, or a late
           response fires into a reclaimed frame. A racing poller may have completed it */
        i_RamblePending *p;
        acquired = i_ramble_node_sys_lock(fn->n);
        if (!ctx.done && (p = i_ramble_func_take_pending(fn, id)) != NULL)
            i_ramble_node_sys_alloc(fn->n, p, 0);
        i_ramble_node_sys_unlock(fn->n, acquired);
    }
    if (out){
        memset(out, 0, sizeof *out);
        out->status = ctx.done ? ctx.status : RAMBLE_CALL_TIMEOUT;
        out->data = ramble_bytes(fn->sync_buf,
                               (ctx.done && ctx.status != RAMBLE_CALL_TIMEOUT) ? ctx.len : 0);
        out->schema = out->data.len ? ctx.schema : NULL;
        out->provider = ctx.done ? ctx.provider : 0;
        out->written_us = ctx.done ? ctx.written_us : 0;
        /* the same lifetime rule as data: the scratch holds it until the next blocking call */
        out->message = ctx.done ? ramble_string(fn->sync_msg, fn->sync_msg_len)
                                : i_ramble_call_status_msg(RAMBLE_CALL_TIMEOUT);
    }
    /* 0 = timed out, whichever deadline expired first, 1 = a real outcome */
    return (ctx.done && ctx.status != RAMBLE_CALL_TIMEOUT) ? 1 : 0;
}

int ramble_function_match_count(RambleFunction *fn){
    if (!fn) return 0;
    return ramble_topic_match_count(fn->is_provider ? fn->rsp : fn->req);
}

/* The provider handler API. The handler's RambleRequest is the pub head of an i_RambleRequest,
 * so these recover the reply machinery by downcast. A copy answers nothing. */
static void i_ramble_request_answer(RambleRequest *request, uint8_t status, const char *message,
                                  RambleBytes rsp){
    i_RambleRequest *r = (i_RambleRequest*)request;
    if (!request || r->replied) return;
    r->replied = 1;
    i_ramble_func_send_reply(r->fn, request->caller, r->call_id, status, message, rsp);
}
void ramble_request_reply(RambleRequest *request, RambleBytes rsp){
    i_ramble_request_answer(request, RAMBLE_CALL_OK, NULL, rsp);
}
void ramble_request_fail (RambleRequest *request, const char *message, RambleBytes rsp){
    i_ramble_request_answer(request, RAMBLE_CALL_APP_ERROR, message, rsp);
}

int ramble_request_start(RambleRequest *request){
    i_RambleRequest *r = (i_RambleRequest*)request;
    if (!request) return RAMBLE_ERR_NO_TOPIC;
    if (!r->fn->is_task || r->replied) return RAMBLE_ERR_STATE;   /* task only, before the answer */
    if (r->started) return RAMBLE_OK;   /* idempotent */
    r->started = 1;
    i_ramble_func_send_reply(r->fn, request->caller, r->call_id, RAMBLE_CALL_RUNNING,
                           NULL, ramble_bytes(NULL, 0));
    return RAMBLE_OK;
}

uint64_t ramble_request_defer(RambleRequest *request){
    i_RambleRequest *r = (i_RambleRequest*)request;
    i_RambleDefer *d;
    int acquired;
    if (!request || r->replied) return 0;
    if (r->fn->is_task && !r->started) (void)ramble_request_start(request);   /* implies RUNNING */
    acquired = i_ramble_node_sys_lock(r->fn->n);
    d = (i_RambleDefer*)i_ramble_node_sys_alloc(r->fn->n, NULL, sizeof *d);
    if (d){
        d->fn = r->fn; d->caller = request->caller; d->caller_lo = r->caller_lo;
        d->call_id = r->call_id; d->cancelled = 0;
        d->next = r->fn->defers; r->fn->defers = d;   /* into the live defer registry */
        r->replied = 1;   /* no auto ack, the reply comes via ramble_function_complete */
    }
    i_ramble_node_sys_unlock(r->fn->n, acquired);
    return (uint64_t)(uintptr_t)d;
}

/* the token as a live defer of fn, unlink_it pops it. NULL = stale. Lock held */
static i_RambleDefer *i_ramble_func_defer_find(RambleFunction *fn, uint64_t token, int unlink_it){
    i_RambleDefer **pp = &fn->defers, *d = (i_RambleDefer*)(uintptr_t)token;
    for (; *pp; pp = &(*pp)->next)
        if (*pp == d){ if (unlink_it) *pp = d->next; return d; }
    return NULL;
}

int ramble_function_complete(RambleFunction *fn, uint64_t token, RambleCallStatus status,
                           const char *message, RambleBytes rsp){
    i_RambleDefer *d;
    uint8_t hdr[RAMBLE__FN_RSP_HDR_MAX]; size_t hl; RambleTopic *rsp_topic; uint32_t caller;
    int acquired;
    if (!fn || !token) return RAMBLE_ERR_NO_TOPIC;
    acquired = i_ramble_node_sys_lock(fn->n);
    d = i_ramble_func_defer_find(fn, token, 1);
    if (!d){   /* a stale token answers nothing and frees nothing */
        i_ramble_node_sys_unlock(fn->n, acquired);
        return RAMBLE_ERR_STATE;
    }
    hl = i_ramble_func_rsp_hdr(hdr, d->call_id, (uint8_t)status, message);
    rsp_topic = fn->rsp; caller = d->caller;
    i_ramble_node_sys_alloc(fn->n, d, 0);
    i_ramble_node_sys_unlock(fn->n, acquired);
    /* send outside the lock: a completion from an app thread engages backpressure, from a
       callback it commits reentrantly */
    return i_ramble_topic_send_to(rsp_topic, caller, ramble_bytes(hdr, hl), rsp);
}

int ramble_function_progress(RambleFunction *fn, uint64_t token, RambleBytes progress){
    i_RambleDefer *d;
    uint8_t hdr[RAMBLE__PRG_PREFIX]; RambleTopic *prg;
    int acquired;
    if (!fn) return RAMBLE_ERR_NO_TOPIC;
    if (!fn->is_task) return RAMBLE_ERR_STATE;
    acquired = i_ramble_node_sys_lock(fn->n);
    d = i_ramble_func_defer_find(fn, token, 0);
    if (!d){ i_ramble_node_sys_unlock(fn->n, acquired); return RAMBLE_ERR_STATE; }
    i_ramble_le_w32(hdr, d->caller_lo); i_ramble_le_w32(hdr + 4, d->call_id);
    prg = fn->prg;
    i_ramble_node_sys_unlock(fn->n, acquired);
    /* broadcast outside the lock: reliable subscribers get backpressure end to end, best
       effort observers are fire and forget */
    return i_ramble_topic_send_hdr(prg, ramble_bytes(hdr, RAMBLE__PRG_PREFIX), progress);
}

int ramble_function_cancelled(RambleFunction *fn, uint64_t token){
    i_RambleDefer *d; int acquired, r;
    if (!fn) return RAMBLE_ERR_NO_TOPIC;
    if (!fn->is_task) return RAMBLE_ERR_STATE;
    acquired = i_ramble_node_sys_lock(fn->n);
    d = i_ramble_func_defer_find(fn, token, 0);
    r = d ? (d->cancelled ? 1 : 0) : RAMBLE_ERR_STATE;
    i_ramble_node_sys_unlock(fn->n, acquired);
    return r;
}

int ramble_function_on_cancel(RambleFunction *def, RambleCancelFn on_cancel, void *user){
    int acquired;
    if (!def) return RAMBLE_ERR_NO_TOPIC;
    if (!def->is_task || !def->is_provider) return RAMBLE_ERR_STATE;
    acquired = i_ramble_node_sys_lock(def->n);
    def->on_cancel = on_cancel; def->on_cancel_user = user;
    i_ramble_node_sys_unlock(def->n, acquired);
    return RAMBLE_OK;
}

int ramble_function_cancel(RambleFunction *fn, uint32_t call_id){
    i_RamblePending *p; int acquired; uint32_t dest;
    uint8_t hdr[RAMBLE__FN_PREFIX];
    if (!fn || !fn->req) return RAMBLE_ERR_NO_TOPIC;
    acquired = i_ramble_node_sys_lock(fn->n);
    p = i_ramble_func_find_pending(fn, call_id);
    if (!p){   /* already answered or never made: nothing to cancel */
        i_ramble_node_sys_unlock(fn->n, acquired);
        return RAMBLE_ERR_STATE;
    }
    if (p->queued_req){
        /* never sent: cancel locally with the one CANCELLED outcome */
        RambleResponse r;
        (void)i_ramble_func_take_pending(fn, call_id);
        r.status = RAMBLE_CALL_CANCELLED; r.data = ramble_bytes(NULL, 0);
        r.schema = NULL; r.provider = 0; r.user = p->user; r.written_us = 0;
        r.message = i_ramble_call_status_msg(RAMBLE_CALL_CANCELLED);
        if (p->on_response) p->on_response(&r);
        i_ramble_func_free_pending(fn, p);
        i_ramble_node_sys_unlock(fn->n, acquired);
        return RAMBLE_OK;
    }
    dest = p->dest;
    {   /* the provider's declared attrs gate cancel locally: no CANCELLABLE bit refuses
           with nothing sent, like a read only variable */
        RambleEntityInfo ei;
        int cancellable = i_ramble_pat_peer_entity(fn->n, dest, fn->req, RAMBLE_ENTITY_TASK, &ei)
                          && ei.cancellable;
        if (!cancellable){
            i_ramble_node_sys_unlock(fn->n, acquired);
            return RAMBLE_ERR_ROLE;
        }
    }
    i_ramble_node_sys_unlock(fn->n, acquired);
    /* never acked: delivery is reliable and the terminal status is the answer. A cancel
       racing the call's own answer lands on nothing at the provider */
    i_ramble_le_w32(hdr, call_id); hdr[4] = RAMBLE__FN_OP_CANCEL;
    return i_ramble_topic_send_to(fn->req, dest, ramble_bytes(hdr, RAMBLE__FN_PREFIX), ramble_bytes(NULL, 0));
}

/* The built in @ramble/meta endpoint, hosted at node open as a both sides multi function.
 * The node builds the snapshot, this layer wires it into the function machinery. */

/* the node pool RambleAllocFn adapter */
static void *i_ramble_pat_alloc(void *user, void *ptr, size_t size){
    return i_ramble_node_sys_alloc((RambleNode*)user, ptr, size);
}

static void i_ramble_meta_on_request(RambleRequest *request, void *user){
    RambleNode *n = (RambleNode*)user;
    i_RamblePatterns *pm = (i_RamblePatterns*)*i_ramble_node_sys_slot(n);
    uint32_t mask = 0, need;
    RambleBytes body;
    if (!pm || !pm->meta_rsp_schema){ ramble_request_fail(request, "meta schema unavailable", ramble_bytes(NULL, 0)); return; }
    if (request->data.len >= 4) mask = i_ramble_le_r32(request->data.data);   /* empty = everything */
    body = i_ramble_node_snapshot(n, mask);
    if (!body.data){ ramble_request_fail(request, "snapshot failed", ramble_bytes(NULL, 0)); return; }
    need = ramble_schema_msg_min(pm->meta_rsp_schema) + (uint32_t)body.len;
    if (pm->meta_msg_cap < need){
        uint8_t *nb = (uint8_t*)i_ramble_node_sys_alloc(n, pm->meta_msg, need);
        if (!nb){ ramble_request_fail(request, "out of memory", ramble_bytes(NULL, 0)); return; }
        pm->meta_msg = nb; pm->meta_msg_cap = need;
    }
    if (!ramble_schema_message_default(pm->meta_rsp_schema, pm->meta_msg, pm->meta_msg_cap)
        || !ramble_set_map(pm->meta_msg, pm->meta_msg_cap, pm->meta_rsp_schema, "info", body)){
        ramble_request_fail(request, "meta encode failed", ramble_bytes(NULL, 0));
        return;
    }
    ramble_request_reply(request, ramble_bytes(pm->meta_msg,
                ramble_schema_msg_len(pm->meta_rsp_schema, pm->meta_msg, pm->meta_msg_cap)));
}

/* the node open seam declared in node/runtime.c: hosts @ramble/meta on this node */
void i_ramble_patterns_meta_open(RambleNode *n){
    i_RamblePatterns *pm;
    RambleSchema *rsp;
    RambleFunctionOpts fo;
    int acquired;
    if (!n) return;
    acquired = i_ramble_node_sys_lock(n);
    pm = i_ramble_patterns_get(n);
    if (pm && !pm->meta_rsp_schema)
        pm->meta_rsp_schema = ramble_schema_compile(i_ramble_pat_alloc, n, "RambleMeta { info: map }", NULL);
    rsp = pm ? pm->meta_rsp_schema : NULL;
    i_ramble_node_sys_unlock(n, acquired);
    if (!pm || pm->meta || !rsp) return;   /* OOM degrades: no endpoint, the node stays healthy */
    memset(&fo, 0, sizeof fo);
    fo.multi = 1;                          /* every node hosting one is the design */
    pm->meta = i_ramble_function_new(n, "@ramble/meta", NULL /* the raw mask */, rsp,
                                   i_ramble_meta_on_request, n, &fo, 2 /* both sides */, NULL, NULL);
}

RambleFunction *ramble_node_meta_function(RambleNode *n){
    i_RamblePatterns *pm; RambleFunction *meta; int acquired;
    if (!n) return NULL;
    acquired = i_ramble_node_sys_lock(n);
    pm = (i_RamblePatterns*)*i_ramble_node_sys_slot(n);
    meta = pm ? pm->meta : NULL;
    i_ramble_node_sys_unlock(n, acquired);
    return meta;
}

/* variables */

#define RAMBLE__VAR_PREFIX      5u    /* value channel: [u8 flags][u32 write_seq] */
#define RAMBLE__VAR_FLAG_FORCED 0x01u
#define RAMBLE__SET_PREFIX      1u    /* set channel: [u8 op] */
#define RAMBLE__SET_OP_FORCE    0x01u
#define RAMBLE__SET_OP_UNFORCE  0x02u

struct RambleVariable {
    RambleNode       *n;
    i_RamblePatterns *pm;
    struct RambleVariable *next;   /* the manager list */
    RambleTopic      *value;   /* owner PUB_ONLY, accessor SUB_ONLY */
    RambleTopic      *set;     /* owner SUB_ONLY, accessor PUB_ONLY, NULL = a read only owner */
    uint8_t         is_owner, allow_force, readonly, forced, has_value;
    uint32_t        write_seq;
    uint8_t        *store;  uint32_t store_len,  store_cap;    /* the value or the cache */
    uint8_t        *shadow; uint32_t shadow_len, shadow_cap;   /* owner: the source while forced */
    RambleVariableUpdateFn on_change; void *on_change_user;   /* fires only on a state change */
    RambleVariableUpdateFn on_write;  void *on_write_user;    /* fires on every applied write */
    const RambleSchema *cur_schema;  /* what store decodes with */
    uint32_t        last_source;     /* the peer behind the current state, 0 = a local call */
    uint64_t        last_write_us;   /* when the current state applied, node clock */
    uint64_t        last_written_us;   /* the writer's wall clock for that write, 0 = unstamped */
    uint32_t       *dup_peers;      /* owner: peers already reported for duplicate authority */
    uint16_t        dup_n, dup_cap;
    uint8_t         reflect;        /* created with reflect_from_mesh */
    uint64_t        generation;     /* the mesh generation the schema was taken at */
};

/* copies v into a grown buffer, 0 on OOM with the buffer unchanged */
static int i_ramble_buf_put(RambleNode *n, uint8_t **buf, uint32_t *len, uint32_t *cap, RambleBytes v){
    if (*cap < v.len){
        uint8_t *nb = (uint8_t*)i_ramble_node_sys_alloc(n, *buf, v.len ? v.len : 1u);
        if (!nb) return 0;
        *buf = nb; *cap = (uint32_t)(v.len ? v.len : 1u);
    }
    if (v.len) memcpy(*buf, v.data, v.len);
    *len = (uint32_t)v.len;
    return 1;
}

static void i_ramble_var_hdr(const RambleVariable *v, uint8_t hdr[RAMBLE__VAR_PREFIX]){
    hdr[0] = (uint8_t)(v->forced ? RAMBLE__VAR_FLAG_FORCED : 0);
    i_ramble_le_w32(hdr + 1, v->write_seq);
}

/* under the lock, before the store is overwritten: would applying (val, forced_now) change
 * the observed state? The first value, different bytes, or a forced flag flip. */
static int i_ramble_var_would_change(const RambleVariable *v, RambleBytes val, int forced_now){
    if (!v->has_value) return 1;
    if ((v->forced != 0) != (forced_now != 0)) return 1;
    if (v->store_len != val.len) return 1;
    return val.len ? memcmp(v->store, val.data, val.len) != 0 : 0;
}

/* the current state as a RambleVariableUpdate, views into manager memory */
static void i_ramble_var_update_view(RambleVariable *v, RambleVariableUpdate *u){
    u->variable   = v;
    u->name       = i_ramble_topic_name(v->value);
    u->value      = ramble_bytes(v->store, v->store_len);
    u->schema     = v->cur_schema;
    u->forced     = v->forced;
    u->write_seq  = v->write_seq;
    u->source     = v->last_source;
    u->recv_us    = v->last_write_us;
    u->written_us = v->last_written_us;
}

/* A write just applied, lock held: stamp the write context, then fire on_write always and
 * on_change when the state changed, inline on this thread. */
static void i_ramble_var_notify(RambleVariable *v, int changed, uint32_t source, uint64_t when_us,
                              uint64_t written_us){
    RambleVariableUpdate u;
    v->last_source = source; v->last_write_us = when_us; v->last_written_us = written_us;
    if (!v->on_write && !(changed && v->on_change)) return;
    i_ramble_var_update_view(v, &u);
    if (v->on_write)             v->on_write(&u, v->on_write_user);
    if (changed && v->on_change) v->on_change(&u, v->on_change_user);
}

/* owner: publishes the store under a held lock, a reentrant send with no wait */
static void i_ramble_var_publish_locked(RambleVariable *v){
    uint8_t hdr[RAMBLE__VAR_PREFIX];
    i_ramble_var_hdr(v, hdr);
    (void)i_ramble_topic_send_hdr(v->value, ramble_bytes(hdr, RAMBLE__VAR_PREFIX),
                                ramble_bytes(v->store, v->store_len));
}

/* owner: a dumb write from a held lock context. Absorbed into the shadow while forced,
 * with no event, else stored, published and notified. */
static void i_ramble_var_owner_apply(RambleVariable *v, RambleBytes val, uint32_t source,
                                   uint64_t when_us, uint64_t written_us){
    int changed;
    if (v->forced){ i_ramble_buf_put(v->n, &v->shadow, &v->shadow_len, &v->shadow_cap, val); return; }
    changed = i_ramble_var_would_change(v, val, 0);
    if (!i_ramble_buf_put(v->n, &v->store, &v->store_len, &v->store_cap, val)) return;
    v->has_value = 1; v->write_seq++;
    i_ramble_var_publish_locked(v);
    i_ramble_var_notify(v, changed, source, when_us, written_us);
}
/* owner: force to val. Without allow_force this is a silent no op, force is opaque on the
 * wire and the local API refuses loudly before reaching here. */
static void i_ramble_var_owner_force(RambleVariable *v, RambleBytes val, uint32_t source,
                                   uint64_t when_us, uint64_t written_us){
    int changed;
    if (!v->allow_force) return;
    changed = i_ramble_var_would_change(v, val, 1);
    if (!v->forced)   /* entering force: save the current source into the shadow */
        i_ramble_buf_put(v->n, &v->shadow, &v->shadow_len, &v->shadow_cap, ramble_bytes(v->store, v->store_len));
    if (!i_ramble_buf_put(v->n, &v->store, &v->store_len, &v->store_cap, val)) return;
    v->forced = 1; v->has_value = 1; v->write_seq++;
    i_ramble_var_publish_locked(v);
    i_ramble_var_notify(v, changed, source, when_us, written_us);
}
static void i_ramble_var_owner_unforce(RambleVariable *v, uint32_t source, uint64_t when_us,
                                     uint64_t written_us){
    if (!v->forced) return;
    v->forced = 0;
    i_ramble_buf_put(v->n, &v->store, &v->store_len, &v->store_cap, ramble_bytes(v->shadow, v->shadow_len));
    v->write_seq++;
    i_ramble_var_publish_locked(v);
    i_ramble_var_notify(v, 1, source, when_us, written_us);   /* a flag flip is always a change */
}

/* owner: a set, force or unforce op arrived, lock held. A plain set must carry a value,
 * an empty one is ignored. */
static void i_ramble_var_on_set(void *user, const RambleMsg *msg){
    RambleVariable *v = (RambleVariable*)user;
    uint8_t op = msg->header.len >= 1 ? msg->header.data[0] : 0;
    if (op & RAMBLE__SET_OP_UNFORCE)    i_ramble_var_owner_unforce(v, msg->publisher_id, msg->recv_us, msg->written_us);
    else if (op & RAMBLE__SET_OP_FORCE) { if (msg->data.len) i_ramble_var_owner_force(v, msg->data, msg->publisher_id, msg->recv_us, msg->written_us); }
    else                              { if (msg->data.len) i_ramble_var_owner_apply(v, msg->data, msg->publisher_id, msg->recv_us, msg->written_us); }
}

/* accessor: a new value arrived. Cache it with its forced flag and write_seq, then notify
 * with the owner as the source. */
static void i_ramble_var_on_value(void *user, const RambleMsg *msg){
    RambleVariable *v = (RambleVariable*)user;
    int forced_in, changed; uint32_t seq_in;
    if (msg->header.len < RAMBLE__VAR_PREFIX) return;
    /* the stale order guard: a same owner value behind the cached seq is dropped (signed
       distance, wrap safe). A different publisher always applies. See spec/patterns.md */
    seq_in = i_ramble_le_r32(msg->header.data + 1);
    if (v->has_value && msg->publisher_id == v->last_source
        && (int32_t)(seq_in - v->write_seq) < 0) return;
    forced_in = (msg->header.data[0] & RAMBLE__VAR_FLAG_FORCED) ? 1 : 0;
    changed = i_ramble_var_would_change(v, msg->data, forced_in);
    if (i_ramble_buf_put(v->n, &v->store, &v->store_len, &v->store_cap, msg->data)){
        v->has_value = 1;
        v->forced = (uint8_t)forced_in;
        v->write_seq = i_ramble_le_r32(msg->header.data + 1);
        v->cur_schema = msg->schema;
        i_ramble_var_notify(v, changed, msg->publisher_id, msg->recv_us, msg->written_us);
    }
}

static RambleVariable *i_ramble_variable_new(RambleNode *n, const char *name, const RambleSchema *schema,
                                         const RambleVariableOpts *opts, int owner){
    i_RamblePatterns *pm; RambleVariable *v;
    char sn[RAMBLE_TOPIC_NAME_MAX + 1]; size_t nl;
    RambleTopicOpts vopt, sopt; int acquired, readonly, make_set;
    if (!n || !name) return NULL;
    nl = strlen(name);
    if (nl == 0 || nl + 4 > RAMBLE_TOPIC_NAME_MAX) return NULL;   /* room for "@set" */
    readonly = (opts && opts->access == RAMBLE_VAR_READONLY);
    make_set = owner ? !readonly : 1;   /* a read only owner has no set channel */

    memset(&vopt, 0, sizeof vopt);
    vopt.qos.reliability = RAMBLE_RELIABLE;
    vopt.qos.catch_up = (opts && opts->catch_up) ? opts->catch_up : 1u;   /* for late accessors */
    /* keep_last is the repair window, not the replay window. Tying it to catch_up once left
       a reliable variable one slot deep. 0 = the reliable default (10). See spec/patterns.md */
    vopt.qos.keep_last = (opts && opts->keep_last) ? opts->keep_last : 0u;
    if (vopt.qos.keep_last && vopt.qos.keep_last < vopt.qos.catch_up)
        vopt.qos.keep_last = vopt.qos.catch_up;      /* the ring must hold what it replays */
    vopt.qos.backpressure_wait_us = (opts && opts->backpressure_wait_us) ? opts->backpressure_wait_us
                                                                         : RAMBLE_PATTERN_BP_WAIT_US;
    sopt = vopt; sopt.qos.catch_up = 0;   /* the set channel: no replay, the same repair depth */

    v = (RambleVariable*)i_ramble_pat_handle_new(n, sizeof *v, &pm);
    if (!v) return NULL;
    if (opts && opts->reflect_from_mesh){
        const RambleSchema *ms;
        v->reflect = 1;
        i_ramble_node_reflect_pick(n, RAMBLE_ENTITY_VARIABLE, name, 0, owner, &ms, NULL, &v->generation);
        if (!schema) schema = ms;
    }
    v->n = n; v->pm = pm; v->is_owner = (uint8_t)owner;
    v->allow_force = (uint8_t)(opts && opts->allow_force);
    v->readonly = (uint8_t)readonly;
    v->cur_schema = schema;
    v->value = i_ramble_node_create_pattern_topic(n, name, owner ? RAMBLE_PUB_ONLY : RAMBLE_SUB_ONLY,
                              schema, &vopt, RAMBLE_KIND_VARIABLE, RAMBLE__VAR_PREFIX, 0,
                              (uint8_t)((owner && v->allow_force) ? RAMBLE_ATTR_FORCEABLE : 0u),
                              owner ? NULL : i_ramble_var_on_value, v);
    if (!v->value) return NULL;   /* v stays pool allocated: nothing routes into it yet */
    if (owner)
        /* seed write_seq from the slot's continuing seqno line, so a successor definition's
           first write orders above its predecessor's last. See spec/patterns.md */
        v->write_seq = (uint32_t)i_ramble_topic_seqno(v->value);
    if (make_set){
        memcpy(sn, name, nl); memcpy(sn + nl, "@set", 5);
        v->set = i_ramble_node_create_pattern_topic(n, sn, owner ? RAMBLE_SUB_ONLY : RAMBLE_PUB_ONLY,
                              schema, &sopt, RAMBLE_KIND_VAR_SET, RAMBLE__SET_PREFIX, 0, 0,
                              owner ? i_ramble_var_on_set : NULL, v);
        if (!v->set)   /* the value topic may route to v: keep the handle */
            return (RambleVariable*)i_ramble_pat_half_create_fail(v->value);
    }
    acquired = i_ramble_node_sys_lock(n);       /* publish into the manager list */
    v->next = pm->vars; pm->vars = v; pm->auth_dirty = 1;
    if (owner)      /* a rival owner may already be on the network */
        i_ramble_pat_dup_sweep(n, v->value, RAMBLE_ENTITY_VARIABLE,
                             &v->dup_peers, &v->dup_n, &v->dup_cap);
    i_ramble_node_sys_unlock(n, acquired);
    if (owner && opts && opts->initial.len)   /* seed the store and history */
        ramble_variable_set(v, opts->initial);
    return v;
}

RambleVariable *ramble_node_create_variable_definition(RambleNode *n, const char *name,
                              const RambleSchema *schema, const RambleVariableOpts *opts){
    if (i_ramble_pat_reserved(name)) return NULL;
    return i_ramble_variable_new(n, name, schema, opts, 1);
}
RambleVariable *ramble_node_create_remote_variable(RambleNode *n, const char *name,
                              const RambleSchema *schema, const RambleVariableOpts *opts){
    if (i_ramble_pat_reserved(name)) return NULL;
    return i_ramble_variable_new(n, name, schema, opts, 0);
}

int ramble_variable_get(RambleVariable *var, RambleBytes *out){
    int acquired, has;
    if (!var) return 0;
    acquired = i_ramble_node_sys_lock(var->n);
    has = var->has_value;
    if (out){ out->data = var->store; out->len = has ? var->store_len : 0; }
    i_ramble_node_sys_unlock(var->n, acquired);
    return has;   /* out views manager memory */
}

/* accessor write routing: no owner at all versus a read only owner */
static int i_ramble_var_accessor_route(RambleVariable *var){
    if (i_ramble_topic_source_match_count(var->value) == 0) return RAMBLE_ERR_NO_TOPIC;
    if (!var->set || ramble_topic_match_count(var->set) == 0) return RAMBLE_ERR_ROLE;
    return RAMBLE_OK;
}

/* The routed form every accessor write goes through: while the owner match is still
 * forming, wait on the set channel like a first send does, then re derive the verdict. */
static int i_ramble_var_accessor_route_wait(RambleVariable *var){
    int r = i_ramble_var_accessor_route(var);
    if (r == RAMBLE_OK || !var->set) return r;
    (void)i_ramble_topic_match_wait(var->set);
    return i_ramble_var_accessor_route(var);
}

int ramble_variable_set(RambleVariable *var, RambleBytes value){
    int acquired, r = RAMBLE_OK, publish = 0;
    uint32_t my_seq = 0;
    uint8_t hdr[RAMBLE__VAR_PREFIX];
    if (!var) return RAMBLE_ERR_NO_TOPIC;
    if (var->is_owner){
        /* mutate the store under the lock, publish outside it from the caller's buffer so
           the send engages backpressure */
        acquired = i_ramble_node_sys_lock(var->n);
        if (var->forced){
            i_ramble_buf_put(var->n, &var->shadow, &var->shadow_len, &var->shadow_cap, value);
        } else {
            int changed = i_ramble_var_would_change(var, value, 0);
            if (!i_ramble_buf_put(var->n, &var->store, &var->store_len, &var->store_cap, value)){
                r = RAMBLE_ERR_OOM;
            } else {
                var->has_value = 1; var->write_seq++;
                my_seq = var->write_seq;
                i_ramble_var_hdr(var, hdr);
                publish = 1;
                i_ramble_var_notify(var, changed, 0, i_ramble_node_now_us(var->n),
                                  i_ramble_node_wall_us(var->n));   /* a local write: our clock */
                /* a reentrant set inside the callback committed first, so sending ours now
                   would put stale bytes newest in history. Skip, the write itself stands */
                if (var->write_seq != my_seq) publish = 0;
            }
        }
        i_ramble_node_sys_unlock(var->n, acquired);
        return publish ? i_ramble_topic_send_hdr(var->value, ramble_bytes(hdr, RAMBLE__VAR_PREFIX), value) : r;
    }
    r = i_ramble_var_accessor_route_wait(var);
    if (r != RAMBLE_OK) return r;
    {   uint8_t op = 0;
        return i_ramble_topic_send_hdr(var->set, ramble_bytes(&op, 1), value);
    }
}

int ramble_variable_force(RambleVariable *var, RambleBytes value){
    int acquired, r = RAMBLE_OK;
    if (!var) return RAMBLE_ERR_NO_TOPIC;
    if (var->is_owner){
        if (!var->allow_force) return RAMBLE_ERR_STATE;   /* locally checkable: refuse loudly */
        acquired = i_ramble_node_sys_lock(var->n);
        i_ramble_var_owner_force(var, value, 0, i_ramble_node_now_us(var->n),
                               i_ramble_node_wall_us(var->n));   /* a rare debug op, reentrant */
        i_ramble_node_sys_unlock(var->n, acquired);
        return RAMBLE_OK;
    }
    r = i_ramble_var_accessor_route_wait(var);
    if (r != RAMBLE_OK) return r;
    {   uint8_t op = RAMBLE__SET_OP_FORCE;
        return i_ramble_topic_send_hdr(var->set, ramble_bytes(&op, 1), value);
    }
}

int ramble_variable_unforce(RambleVariable *var){
    int acquired, r = RAMBLE_OK;
    if (!var) return RAMBLE_ERR_NO_TOPIC;
    if (var->is_owner){
        if (!var->allow_force) return RAMBLE_ERR_STATE;
        acquired = i_ramble_node_sys_lock(var->n);
        i_ramble_var_owner_unforce(var, 0, i_ramble_node_now_us(var->n), i_ramble_node_wall_us(var->n));
        i_ramble_node_sys_unlock(var->n, acquired);
        return RAMBLE_OK;
    }
    r = i_ramble_var_accessor_route_wait(var);
    if (r != RAMBLE_OK) return r;
    {   uint8_t op = RAMBLE__SET_OP_UNFORCE;
        return i_ramble_topic_send_hdr(var->set, ramble_bytes(&op, 1), ramble_bytes(NULL,0));
    }
}

int ramble_variable_forced(RambleVariable *var){ return var ? var->forced : 0; }

int ramble_variable_retire(RambleVariable *var){
    i_RamblePatterns *pm; RambleNode *n; int acquired, r;
    RambleTopic *value, *set;
    if (!var) return RAMBLE_ERR_NO_TOPIC;
    pm = var->pm; n = var->n;
    r = i_ramble_pat_park_channels(var->value, var->set);   /* set is NULL on a read only owner */
    if (r != 0) return r;
    acquired = i_ramble_node_sys_lock(n);
    i_ramble_pat_clear_channels(var->value, var->set);
    if (pm){ i_ramble_pat_unlink((void**)&pm->vars, var, offsetof(RambleVariable, next)); pm->auth_dirty = 1; }
    value = var->value; set = var->set;   /* outlive var, see phase 3 */
    if (var->store)     i_ramble_node_sys_alloc(n, var->store, 0);
    if (var->shadow)    i_ramble_node_sys_alloc(n, var->shadow, 0);
    if (var->dup_peers) i_ramble_node_sys_alloc(n, var->dup_peers, 0);
    i_ramble_node_sys_alloc(n, var, 0);
    i_ramble_node_sys_unlock(n, acquired);
    i_ramble_pat_release_channels(value, set);
    return RAMBLE_OK;
}

int ramble_variable_refresh(RambleVariable *var){
    RambleNode *n; uint64_t gen = 0; int r;
    const RambleSchema *ms = NULL;
    if (!var) return RAMBLE_ERR_NO_TOPIC;
    if (!var->reflect) return RAMBLE_ERR_ROLE;
    n = var->n;
    if (!i_ramble_node_reflect_pick(n, RAMBLE_ENTITY_VARIABLE, i_ramble_pat_base_name(var->value), 0,
                                  var->is_owner, &ms, NULL, &gen)
        || gen == var->generation) return 0;
    r = i_ramble_topic_retype(var->value, ms, RAMBLE_RELIABLE);
    if (r != 0) return r;
    if (var->set){
        r = i_ramble_topic_retype(var->set, ms, RAMBLE_RELIABLE);
        if (r != 0) return r;
    }
    var->generation = gen;
    return 1;
}

int ramble_variable_on_change(RambleVariable *var, RambleVariableUpdateFn on_change, void *user){
    int acquired;
    if (!var) return RAMBLE_ERR_NO_TOPIC;
    acquired = i_ramble_node_sys_lock(var->n);
    var->on_change = on_change; var->on_change_user = user;
    if (on_change && var->has_value){
        /* replay the current state once, right here, so a value that arrived between
           create and register is never missed */
        RambleVariableUpdate u;
        i_ramble_var_update_view(var, &u);
        on_change(&u, user);
    }
    i_ramble_node_sys_unlock(var->n, acquired);
    return RAMBLE_OK;
}

int ramble_variable_on_write(RambleVariable *var, RambleVariableUpdateFn on_write, void *user){
    int acquired;
    if (!var) return RAMBLE_ERR_NO_TOPIC;
    acquired = i_ramble_node_sys_lock(var->n);
    var->on_write = on_write; var->on_write_user = user;
    i_ramble_node_sys_unlock(var->n, acquired);
    return RAMBLE_OK;   /* writes are events, not state: no replay */
}

int ramble_variable_wait(RambleVariable *var, int timeout_ms){
    int acquired; uint64_t deadline;
    if (!var) return 0;
    acquired = i_ramble_node_sys_lock(var->n);
    if (!acquired || ramble_node_is_started(var->n)){ i_ramble_node_sys_unlock(var->n, acquired); return var->has_value; }
    if (var->has_value){ i_ramble_node_sys_unlock(var->n, acquired); return 1; }
    deadline = i_ramble_node_now_us(var->n) + (uint64_t)(timeout_ms >= 0 ? (uint64_t)timeout_ms*1000u : 3600000000ull);
    i_ramble_node_sys_unlock(var->n, acquired);
    while (!var->has_value){
        if (i_ramble_node_now_us(var->n) >= deadline) break;
        i_ramble_node_sys_poll(var->n, 5);
    }
    return var->has_value;
}

int ramble_variable_match_count(RambleVariable *var){
    if (!var) return 0;
    return ramble_topic_match_count(var->is_owner ? var->value : var->set);
}

/* Duplicate authority detection. Two authorities never match each other, so the announce
   interest is checked instead. The rules are in spec/patterns.md. */

static int i_ramble_pat_dup_reported(const uint32_t *ids, uint16_t n_ids, uint32_t peer){
    uint16_t i;
    for (i = 0; i < n_ids; i++) if (ids[i] == peer) return 1;
    return 0;
}

/* remembers peer in the entity's reported list. On OOM it may report again later */
static void i_ramble_pat_dup_remember(RambleNode *n, uint32_t **ids, uint16_t *n_ids,
                                    uint16_t *cap, uint32_t peer){
    if (*n_ids == *cap){
        uint16_t grown = *cap ? (uint16_t)(*cap * 2u) : 4u;
        uint32_t *nb = (uint32_t*)i_ramble_node_sys_alloc(n, *ids, grown * sizeof **ids);
        if (!nb) return;
        *ids = nb; *cap = grown;
    }
    (*ids)[(*n_ids)++] = peer;
}

/* forgets peer in one entity's reported list, so a genuine return re reports */
static void i_ramble_pat_dup_drop(uint32_t *ids, uint16_t *n_ids, uint32_t peer){
    uint16_t i;
    for (i = 0; i < *n_ids; i++)
        if (ids[i] == peer){ ids[i] = ids[--*n_ids]; return; }
}

/* checks one authority side entity against one active peer, reporting a fresh claim once */
static void i_ramble_pat_dup_check(RambleNode *n, uint32_t peer, RambleTopic *primary, RambleEntityKind kind,
                                 uint32_t **ids, uint16_t *n_ids, uint16_t *cap){
    RambleEntityInfo ei;
    if (!primary) return;
    if (i_ramble_pat_dup_reported(*ids, *n_ids, peer)) return;
    if (!i_ramble_pat_peer_entity(n, peer, primary, kind, &ei) || !ei.provides) return;
    i_ramble_pat_dup_remember(n, ids, n_ids, cap, peer);
    i_ramble_node_sys_error(n, RAMBLE_E_DUPLICATE_AUTHORITY, primary, peer);
}

static RambleEntityKind i_ramble_pat_fn_entity_kind(const RambleFunction *fn){
    return fn->is_task ? RAMBLE_ENTITY_TASK : RAMBLE_ENTITY_FUNCTION;
}

/* (kind, hash) order for the authority index */
static int i_ramble_pat_auth_before(const i_RamblePatAuth *a, const i_RamblePatAuth *b){
    return a->kind != b->kind ? a->kind < b->kind : a->hash < b->hash;
}

static void i_ramble_pat_auth_put(i_RamblePatterns *pm, uint32_t hash, RambleEntityKind kind,
                                RambleTopic *primary, uint32_t **ids, uint16_t *n_ids, uint16_t *cap){
    i_RamblePatAuth *a = &pm->auth[pm->auth_n++];
    a->hash = hash; a->kind = (uint8_t)kind; a->primary = primary;
    a->ids = ids; a->n_ids = n_ids; a->cap = cap;
}

static uint32_t i_ramble_pat_auth_hash(RambleTopic *primary){
    RambleString nm = i_ramble_topic_name(primary);
    char buf[RAMBLE_TOPIC_NAME_MAX + 1];
    memcpy(buf, nm.data, nm.len); buf[nm.len] = '\0';
    return (uint32_t)ramble_topic_id(buf);           /* what the announce carries for it */
}

/* rebuilds the sorted authority index from the entity lists, a shell sort. 0 on OOM */
static int i_ramble_pat_auth_rebuild(i_RamblePatterns *pm){
    RambleFunction *fn; RambleVariable *v; uint16_t want = 0, gap, i, j;
    for (fn = pm->funcs; fn; fn = fn->next) if ((fn->is_provider || fn->both) && !fn->multi) want++;
    for (v = pm->vars; v; v = v->next) if (v->is_owner) want++;
    if (want > pm->auth_cap){
        i_RamblePatAuth *nb = (i_RamblePatAuth*)i_ramble_node_sys_alloc(pm->n, pm->auth,
                                                                  (size_t)want * sizeof *nb);
        if (!nb) return 0;
        pm->auth = nb; pm->auth_cap = want;
    }
    pm->auth_n = 0;
    for (fn = pm->funcs; fn; fn = fn->next)
        if ((fn->is_provider || fn->both) && !fn->multi)   /* multi: rivals are the design */
            i_ramble_pat_auth_put(pm, i_ramble_pat_auth_hash(fn->req), i_ramble_pat_fn_entity_kind(fn),
                                fn->req, &fn->dup_peers, &fn->dup_n, &fn->dup_cap);
    for (v = pm->vars; v; v = v->next)
        if (v->is_owner)
            i_ramble_pat_auth_put(pm, i_ramble_pat_auth_hash(v->value), RAMBLE_ENTITY_VARIABLE,
                                v->value, &v->dup_peers, &v->dup_n, &v->dup_cap);
    for (gap = pm->auth_n / 2u; gap > 0; gap /= 2u)
        for (i = gap; i < pm->auth_n; i++){
            i_RamblePatAuth t = pm->auth[i];
            for (j = i; j >= gap && i_ramble_pat_auth_before(&t, &pm->auth[j - gap]); j -= gap)
                pm->auth[j] = pm->auth[j - gap];
            pm->auth[j] = t;
        }
    pm->auth_dirty = 0;
    return 1;
}

/* the first index whose (kind, hash) is not below the key */
static uint16_t i_ramble_pat_auth_lower(const i_RamblePatterns *pm, RambleEntityKind kind, uint32_t hash){
    uint16_t lo = 0, hi = pm->auth_n;
    i_RamblePatAuth key; key.kind = (uint8_t)kind; key.hash = hash;
    while (lo < hi){
        uint16_t mid = (uint16_t)(lo + (hi - lo) / 2u);
        if (i_ramble_pat_auth_before(&pm->auth[mid], &key)) lo = (uint16_t)(mid + 1u); else hi = mid;
    }
    return lo;
}

/* Every authority against one peer whose interest was just applied: one walk of the
 * peer's entities with a binary search each. On OOM the index is skipped this time. */
static void i_ramble_pat_dup_check_peer(i_RamblePatterns *pm, uint32_t peer){
    RambleIter it; RambleEntityInfo ei;
    if (pm->auth_dirty && !i_ramble_pat_auth_rebuild(pm)) return;
    if (!pm->auth_n) return;
    memset(&it, 0, sizeof it);
    while (ramble_node_entities_next(pm->n, peer, &it, &ei)){
        uint16_t k;
        if (!ei.provides || ei.kind == RAMBLE_ENTITY_TOPIC) continue;
        for (k = i_ramble_pat_auth_lower(pm, ei.kind, ei.hash);
             k < pm->auth_n && pm->auth[k].kind == (uint8_t)ei.kind && pm->auth[k].hash == ei.hash; k++){
            i_RamblePatAuth *a = &pm->auth[k];
            if (ei.name.len){
                RambleString nm = i_ramble_topic_name(a->primary); size_t len = nm.len;
                if (len >= 5 && nm.data[len - 4] == '@') len -= 4;   /* the entity's base name */
                if (ei.name.len != len || memcmp(ei.name.data, nm.data, len) != 0) continue;
            }
            if (i_ramble_pat_dup_reported(*a->ids, *a->n_ids, peer)) continue;
            i_ramble_pat_dup_remember(pm->n, a->ids, a->n_ids, a->cap, peer);
            i_ramble_node_sys_error(pm->n, RAMBLE_E_DUPLICATE_AUTHORITY, a->primary, peer);
        }
    }
}

/* a just created authority against every active peer */
static void i_ramble_pat_dup_sweep(RambleNode *n, RambleTopic *primary, RambleEntityKind kind,
                                 uint32_t **ids, uint16_t *n_ids, uint16_t *cap){
    RambleIter it; RamblePeerInfo p;
    memset(&it, 0, sizeof it);
    while (ramble_node_peers_next(n, &it, &p))
        if (p.liveness == RAMBLE_PEER_ACTIVE)
            i_ramble_pat_dup_check(n, p.id, primary, kind, ids, n_ids, cap);
}

static void i_ramble_pat_dup_forget_peer(i_RamblePatterns *pm, uint32_t peer){
    RambleFunction *fn; RambleVariable *v;
    for (fn = pm->funcs; fn; fn = fn->next) i_ramble_pat_dup_drop(fn->dup_peers, &fn->dup_n, peer);
    for (v = pm->vars; v; v = v->next)      i_ramble_pat_dup_drop(v->dup_peers, &v->dup_n, peer);
}

/* the manager hooks: call timeouts on the tick, provider loss on the event */

/* Fails pending calls with one synthesized outcome each: those directed at dest, or all,
 * or those past now. Callbacks run after the unlink. Returns the earliest deadline left. */
static uint64_t i_ramble_func_reap(RambleFunction *fn, uint64_t now, RambleCallStatus fail_status,
                                 int all, uint32_t dest){
    i_RamblePending **pp = &fn->pending, *p;
    uint64_t soonest = 0;
    while ((p = *pp) != NULL){
        if (dest ? (p->dest == dest) : (all || now >= p->deadline_us)){
            *pp = p->next;
            {   RambleResponse r; r.status = fail_status; r.data = ramble_bytes(NULL,0);
                r.schema = NULL; r.provider = 0; r.user = p->user; r.written_us = 0;
                r.message = i_ramble_call_status_msg(fail_status);
                if (p->on_response) p->on_response(&r);
            }
            i_ramble_func_free_pending(fn, p);
            continue;
        }
        if (!soonest || p->deadline_us < soonest) soonest = p->deadline_us;
        pp = &p->next;
    }
    return soonest;
}

/* Fails every sent call directed at peer whose lane no longer exists with CANCELLED: the
 * wire CANCELLED can be lost to the very announce that severed it. See spec/patterns.md. */
static void i_ramble_func_reap_severed(RambleFunction *fn, uint32_t peer){
    i_RamblePending **pp = &fn->pending, *p;
    while ((p = *pp) != NULL){
        if (p->dest == peer && !p->queued_req && !i_ramble_topic_peer_matched(fn->req, peer)){
            *pp = p->next;
            {   RambleResponse r; r.status = RAMBLE_CALL_CANCELLED; r.data = ramble_bytes(NULL,0);
                r.schema = NULL; r.provider = 0; r.user = p->user; r.written_us = 0;
                r.message = ramble_cstr("provider retired");
                if (p->on_response) p->on_response(&r);
            }
            i_ramble_func_free_pending(fn, p);
            continue;
        }
        pp = &p->next;
    }
}

static uint64_t i_ramble_patterns_tick(void *user, uint64_t now_us){
    i_RamblePatterns *pm = (i_RamblePatterns*)user;
    RambleFunction *fn; uint64_t soonest = 0;
    for (fn = pm->funcs; fn; fn = fn->next){
        uint64_t s;
        i_ramble_func_flush_queued(fn);   /* the backstop: the interest event is the fast path */
        s = i_ramble_func_reap(fn, now_us, RAMBLE_CALL_TIMEOUT, 0, 0);
        if (s && (!soonest || s < soonest)) soonest = s;
    }
    return soonest;   /* the next timeout deadline for the poll wait cap */
}

/* The node is closing: every pending call gets CANCELLED and every live deferred call
 * answers CANCELLED while the channels are still up. Runs once, node still fully alive. */
static void i_ramble_patterns_on_close(void *user){
    i_RamblePatterns *pm = (i_RamblePatterns*)user;
    RambleFunction *fn;
    for (fn = pm->funcs; fn; fn = fn->next){
        if (fn->defers){
            int acquired = i_ramble_node_sys_lock(pm->n);
            i_ramble_func_drain_defers(fn, "node closing");
            i_ramble_node_sys_unlock(pm->n, acquired);
        }
        if (!fn->is_provider && fn->pending)
            i_ramble_func_reap(fn, 0, RAMBLE_CALL_CANCELLED, 1, 0);
    }
    /* the drained CANCELLED replies must leave before the socket closes: no poll pass
       follows this hook */
    i_ramble_node_flush_tx(pm->n);
}

static void i_ramble_patterns_on_event(void *user, const RambleEvent *ev){
    i_RamblePatterns *pm = (i_RamblePatterns*)user;
    RambleFunction *fn;
    if (ev->kind == RAMBLE_PEER_INTEREST){
        RambleVariable *v;
        /* a match may just have formed: flush calls queued while no provider was matched */
        for (fn = pm->funcs; fn; fn = fn->next)
            if (!fn->is_provider && fn->pending) i_ramble_func_flush_queued(fn);
        /* and one may just have been severed: a directed call there can never resolve */
        for (fn = pm->funcs; fn; fn = fn->next)
            if (!fn->is_provider && fn->pending) i_ramble_func_reap_severed(fn, ev->peer);
        /* an accessor whose owner just parked or left: disarm the stale order guard, since
           a successor on the same node keeps the peer id with write_seq restarting */
        for (v = pm->vars; v; v = v->next)
            if (!v->is_owner && v->last_source
                && i_ramble_topic_source_match_count(v->value) == 0) v->last_source = 0;
        i_ramble_pat_dup_check_peer(pm, ev->peer);   /* a rival authority may have appeared */
        return;
    }
    if (ev->kind != RAMBLE_PEER_DOWN) return;
    i_ramble_pat_dup_forget_peer(pm, ev->peer);
    /* a provider dropped: a caller with no live provider left fails its calls now with
       PEER_LOST, and a call directed at the dropped peer fails regardless */
    for (fn = pm->funcs; fn; fn = fn->next){
        if (fn->is_provider || !fn->pending) continue;
        (void)i_ramble_func_reap(fn, 0, RAMBLE_CALL_PEER_LOST, 0, ev->peer);   /* directed at it */
        if (fn->pending && i_ramble_topic_live_match_count(fn->req) == 0)
            i_ramble_func_reap(fn, 0, RAMBLE_CALL_PEER_LOST, 1, 0);
    }
}
