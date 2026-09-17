/* The node runtime: the sockets, the clock, the send and receive paths, and the public
 * rant_node_* and rant_topic_* API over the sans-IO cores. The rules are in spec/node.md. */

#include "runtime.h"
#include "core.h"
#include "../discovery/runtime.h"
#include "../platform/core.h"
#ifdef RANT_SHM
#include "../shm/core.h"
#endif
#include "../common/arena.h"
#include "../common/bytes.h"
#include <string.h>    /* heap access goes through the node pool, no stdlib.h here */
#include <stdarg.h>    /* rant_node_log */
#include <stdio.h>     /* vsnprintf for log text only, never the data path */

#ifndef RANT_NO_PATTERNS
struct RantNode;         /* patterns/core.c hosts the @rant/meta endpoint, open calls this seam */
void i_rant_patterns_meta_open(struct RantNode *n);
#endif

/* A consumer queue ring record: this header, the publisher name, then the payload at an
 * 8 aligned offset. Records never wrap, a short tail holds the RANT__QWRAP sentinel. */
typedef struct {
    uint32_t rec_bytes;    /* the whole record, 8 aligned. First, the ring reads it at offset 0 */
    uint32_t data_len;
    uint64_t t_recv_us;    /* the arrival stamp, RantMsg.recv_us */
    uint64_t t_written_us; /* the publisher's source stamp, stripped at enqueue, 0 = opted out */
    uint64_t t_capture_us; /* the publisher's capture stamp, 0 = it sent none */
    uint32_t publisher_id;
    uint8_t  name_len;     /* the name is copied inline, discovery views die with the peer */
    uint8_t  kind;         /* 0 a delivered message, else the patterns layer's record kind */
    uint8_t  pad[2];
} i_RantQRec;
#define RANT__QWRAP 0xFFFFFFFFu
#define RANT__QALIGN(x) (((uint32_t)(x) + 7u) & ~7u)

/* One topic's consumer queue: a byte ring filled by the poll thread and drained by take or
 * dispatch. Under the node lock. The ring and this struct are stable pool allocations. */
typedef struct {
    uint8_t  *buf;
    uint32_t  cap;         /* current ring bytes, 8 aligned, grows on demand */
    uint32_t  cap_limit;   /* the growth bound: qos.queue_bytes or RANT_QUEUE_CAP */
    uint32_t  head, tail;  /* byte offsets, empty iff count == 0 */
    uint32_t  bytes;       /* queued record bytes, excludes wrap padding */
    uint32_t  count;       /* queued records, including one being viewed */
    uint32_t  dropped;     /* best effort records overwritten or refused since open */
    uint8_t   viewing;     /* the record at tail is the consumer's live take view */
    uint8_t   busy;        /* inside dispatch's unlocked callback window, no reentry */
    uint8_t   reliable;    /* the full queue policy: park (reliable) or overwrite */
    uint8_t   parked;      /* transport lanes parked on this topic, retried as we drain */
    uint8_t   silent;      /* the event ring: an eviction counts, it emits nothing */
} i_RantMsgQueue;

/* the node's own record kind: a RantEvent copy then its three strings as [u16 len][bytes] */
#define I_RANT_REC_EVENT (I_RANT_REC_SYNTH | 0x40u)

/* A callback queue: a drain group over the rings of the handles created with it. */
struct RantQueue {
    RantNode *n;
    uint8_t   busy;         /* a dispatch is running on some thread */
};

struct RantTopic {
    RantNode *n; uint16_t index; RantSchema *schema;       /* the schema is the node's own copy */
    i_RantMsgQueue *q;                    /* the consumer queue, NULL = inline callbacks */
    RantQueue *queue;                     /* the callback queue the topic was created with, or NULL */
    i_RantSysMsgFn sys_on_message;        /* the patterns layer's routing, NULL = a normal topic */
    void    *sys_msg_user;
    i_RantSysDispatchFn sys_on_dispatch;  /* the patterns layer's parked record handler */
    uint64_t  tx_msgs, tx_bytes;         /* committed by our sends */
    uint64_t  rx_msgs, rx_bytes;         /* delivered to us, parked excluded */
    uint32_t  resolve_epoch;             /* match_epoch at the last convergence, 0 = never */
    uint8_t   prefix_bytes;              /* pattern header bytes split into .header, 0 = plain */
    uint8_t   prefix_string;             /* [u8 len][bytes] follows the prefix on a response kind */
    uint8_t   kind;                      /* RantTopicKind */
    uint8_t   role;                      /* RantRole, mirrored at create and set_role */
    uint8_t   attrs, directed;           /* the def's declared facts, kept so refresh can redefine */
    uint8_t   reflect;                   /* created with reflect_from_mesh, so refresh applies */
    uint64_t  generation;                /* the mesh generation the schema was taken at */
    RantQos qos;
    uint8_t   name_len;                  /* stable copy, queued views never point into the arena */
    char      name[RANT_TOPIC_NAME_MAX];
};

/* One pending error to log mirror entry. Errors fire deep inside receive processing where
 * a send may not re enter the transport, so emit records here and the poll pass publishes. */
#define RANT_LOG_PEND 8
typedef struct {
    uint64_t wall_us, mono_us;   /* when the first occurrence fired */
    uint32_t peer, count;
    uint16_t topic;
    uint8_t  error;              /* RantErrorKind */
    char     text[192];
} i_RantLogPend;

struct RantNode {
    RantTransportState       *transport;
    i_RantNodeCore *core;       /* the peer table and discovery lifecycle, sans-IO */
    RantDiscovery       *discovery;
    i_RantSock         fd;       /* the unicast data socket */
    uint16_t      domain;
    RantNodeNet    net;         /* a copy of opts.net */
    /* the detail exchange retry cadence: queued requests drain each poll and a sweep re
       asks once per announce interval until a sweep sends nothing */
    uint32_t      announce_us;     /* the resolved discovery announce interval */
    uint64_t      next_detail_us;  /* the next retry sweep, 0 = disarmed */
    /* the datagram the socket refused, retried first next poll so it is never lost */
    uint8_t       tx_hold[RANT_DGRAM_MAX];
    size_t        tx_hold_len;
    uint32_t      tx_hold_peer;
    /* the data socket RX buffer, sized for the largest discovery datagram we accept too */
    uint8_t      *rx_buf;
    size_t        rx_buf_bytes;
    /* backpressure accumulators, read via rant_node_backpressure_stats */
    uint64_t      backpressure_total_us;
    uint32_t      backpressure_wait_count;
    uint32_t      evicted_unsent;  /* the RANT_E_EVICTED_UNSENT count */
#ifdef RANT_THREADS
    /* the node lock and the optional service thread. See spec/node.md */
    i_RantMutex     mu;             /* the node lock: every public entry point takes it */
    i_RantCond      cv;             /* senders and drainers waiting on the poller's progress */
    uint32_t        cv_waiters;     /* under mu, broadcast skipped when 0 */
    i_RantWaker     waker;          /* interrupts the unlocked socket wait */
    i_RantThread    svc;
    volatile uint8_t svc_running; /* a service thread is alive */
    volatile uint8_t svc_stop;    /* stop requested, the service loop exits on it */
    uint8_t       svc_joining;    /* under mu: one stopper owns the join, others wait on cv */
    uint32_t      pollers_sleeping; /* under mu: pollers in or headed into the unlocked wait */
    uint8_t       wake_signaled;  /* under mu: a burst coalesces into one waker datagram */
    uint8_t       user_locked;    /* the public rant_node_lock is held */
    volatile uint8_t  lock_held;  /* the owner reentrancy check, written by the owner only */
    volatile uint64_t lock_owner;
#endif
    /* the in pump probe sampled inside the backpressure wait */
    RantPumpProbeFn pump_probe;
    void          *pump_probe_user;
    uint64_t       pump_probe_interval_us;
    /* the app callbacks */
    RantMsgFn      user_on_message;
    RantEventFn    on_event;
    void          *user_data;
    /* the patterns layer's hooks, all optional */
    i_RantSysEventFn sys_on_event;
    i_RantSysTickFn    sys_tick;
    i_RantSysCloseFn sys_on_close;
    void            *sys_user;
    uint64_t         sys_tick_next;   /* the tick's next deadline, folded into the poll wait */
    uint64_t         settle_topology_us; /* the last PEER_UP, DOWN or INTEREST change */
    /* the send path match wait: the bound, the post open gather latch and the epoch that
       stales each topic's converged memo. See spec/interest.md */
    uint32_t         match_wait_us;   /* 0 = disabled */
    uint64_t         open_us;         /* when the node opened, the gather anchor */
    uint8_t          gather_done;     /* latched once the post open gather settles */
    uint32_t         match_epoch;     /* bumps on peer or interest change, create and set_role */
    void            *patterns;        /* the patterns layer's per node manager, opaque here */
    RantEvent       last_error;  /* the most recent RANT_ERROR, RANT_E_NONE until one fires */
    /* last_error's strings, copied so the by value event outlives a retire or a peer drop */
    char            last_error_topic[RANT_TOPIC_NAME_MAX + 1];
    char            last_error_peer[RANT_NODE_NAME_MAX + 1];
    char            last_error_detail[160];
    /* the built in @rant/log topics and the error mirror ring */
    RantTopic      *log_topics[3];       /* by RantLogLevel, all NULL under opts.disable_logs */
    RantSchema     *log_schema;          /* RantLog { wall_us, mono_us, text }, node owned */
    uint8_t         log_errors;          /* the default on RANT_ERROR mirror */
    uint8_t         log_flushing;        /* reentrancy guard: a flush must not re enter the ring */
    uint8_t         log_pend_n;
    uint32_t        log_pend_dropped;    /* ring overflow between flushes, summarized, never silent */
    i_RantLogPend *log_pend;             /* [RANT_LOG_PEND], allocated on the first recorded error */
    /* the @rant/meta snapshot scratch, grown on demand */
    uint8_t      *snap_buf; uint32_t snap_cap;
    uint16_t     *snap_pend; uint16_t snap_pend_cap;   /* per topic pending counts, one walk */
    char          name[RANT_NODE_NAME_MAX + 1];     /* our advertised node name */
    uint8_t       name_len;
    void          *arena;      /* the control structs block, relocated on grow */
    /* the node's pool, copied from the caller's allocator at open and reset on close */
    RantAllocator pool;
    uint8_t        alloc_dynamic;
    uint8_t        grow_pending;   /* a peer was refused for lack of slots, grow at the next poll */
    uint16_t       max_peers;      /* the current peer table capacity, doubles on a dynamic grow */
    /* the accept bound: the largest peer announce blob we store. It self heals up, see
       spec/discovery.md. meta_grow_failed remembers a size that would not allocate */
    uint16_t       meta_cap;
    uint16_t       meta_grow_need;
    uint16_t       meta_grow_failed;
    /* topic handles: a pointer array in the arena, each RantTopic a stable allocation */
    RantTopic **handles;      /* [max_topics] */
    uint16_t      max_topics;
    uint16_t      n_created;       /* user topics created, dense from 0, stepping over builtins */
    /* the builtins live at [builtin_lo, builtin_lo + n_builtin), above the user budget */
    uint16_t      builtin_lo;      /* the first builtin index */
    uint16_t      n_builtin;
    uint8_t       creating_builtin;/* open is creating builtins: allocate from the block */
    /* the callback queues, stable pool allocations, see spec/node.md */
    RantQueue    *queues[RANT_QUEUES_MAX];
    uint8_t       n_queues;
    uint8_t       dispatching;     /* queues busy right now, close is refused while nonzero */
    /* on_event parks on event_queue when set, records on the event ring */
    RantQueue      *event_queue;
    i_RantMsgQueue *event_q;
    uint32_t        event_queue_bytes;
#ifdef RANT_SHM
    /* the same host path: lazy per topic and size class segments, see spec/shm.md */
    uint8_t        shm_capable;
    uint8_t        shm_host[16];  /* our host uuid, advertised for the same host check */
    uint64_t       shm_base;      /* the segment id base, low 19 bits = (topic << 3) | class */
    void          *shm_scratch; uint32_t shm_scratch_cap;   /* the one copy receive scratch */
    void         **shm_pool;      /* [n_topics * N_CLASSES] our segments, NULL = not created */
    uint16_t       shm_n_topics;
    /* the reader attach cache, grown by segments actually attached */
    uint64_t      *shm_reader_segments;  /* [shm_reader_cap] attached segment ids */
    void         **shm_reader_states;    /* [shm_reader_cap] their pool states */
    uint16_t       shm_reader_cap, shm_reader_count;
    uint32_t       shm_tx, shm_rx;/* messages published and delivered via SHM */
#endif
};

/* The node's allocator hook for the transport, schemas and handles: one freeable pool
 * allocation per call, so reset reclaims all. u is the node. */
static void *i_rant_node_alloc(void *u, void *ptr, size_t size){
    return rant_allocator_alloc(&((RantNode*)u)->pool, ptr, size);
}

#ifdef RANT_THREADS
/* Raw lock ops keeping the owner id consistent. lock_owner is zeroed before the release
   so a non owner reading the pair unlocked never sees lock_held with its own id. */
static void i_rant_node_lock_raw(RantNode *n){
    i_rant_plat_mutex_lock(&n->mu);
    n->lock_owner = i_rant_plat_thread_id();
    n->lock_held = 1;
}
static void i_rant_node_unlock_raw(RantNode *n){
    n->lock_owner = 0;
    n->lock_held = 0;
    i_rant_plat_mutex_unlock(&n->mu);
}
/* The reentrant aware entry lock: 1 = this call acquired mu, 0 = the calling thread
   already held it (a callback, or the pump's nested poll) and proceeds without waiting. */
static int i_rant_node_lock(RantNode *n){
    if (n->lock_held && n->lock_owner == i_rant_plat_thread_id()) return 0;
    i_rant_node_lock_raw(n);
    return 1;
}
static void i_rant_node_unlock(RantNode *n, int acquired){
    if (acquired) i_rant_node_unlock_raw(n);
}
/* With mu held, before unlocking: cut the pollers' wait short so the change is serviced
   now. One datagram wakes every sleeping poller, and a burst coalesces to one per sleep. */
static void i_rant_node_kick(RantNode *n){
    if (n->pollers_sleeping && !n->wake_signaled && i_rant_plat_waker_signal(&n->waker))
        n->wake_signaled = 1;
}
/* A condvar wait that re stamps ownership after reacquire. Nothing cached from inside
   the node survives it: the poller may have relocated the arena. */
static void i_rant_node_cv_wait(RantNode *n, uint64_t timeout_us){
    n->lock_owner = 0;
    n->lock_held = 0;
    i_rant_plat_cond_wait(&n->cv, &n->mu,
                          timeout_us > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)timeout_us);
    n->lock_owner = i_rant_plat_thread_id();
    n->lock_held = 1;
}
#else
static int  i_rant_node_lock(RantNode *n){ (void)n; return 1; }
static void i_rant_node_unlock(RantNode *n, int acquired){ (void)n; (void)acquired; }
static void i_rant_node_kick(RantNode *n){ (void)n; }
#endif /* RANT_THREADS */

/* error reporting: one emit path stamps user_data, keeps the last error and hands on */

/* one past the highest defined topic index, the bound over handles[] (holes are NULL) */
static uint16_t i_rant_node_topic_hi(RantNode *n){
    uint16_t hi = n->n_created;
    if (n->n_builtin){
        if (hi >= n->builtin_lo) hi = (uint16_t)(hi + n->n_builtin);   /* users grew past it */
        if ((uint16_t)(n->builtin_lo + n->n_builtin) > hi) hi = (uint16_t)(n->builtin_lo + n->n_builtin);
    }
    return hi;
}

static int i_rant_node_is_log_topic(RantNode *n, uint16_t topic_index){
    RantTopic *h = (topic_index < i_rant_node_topic_hi(n)) ? n->handles[topic_index] : NULL;
    return h && (h == n->log_topics[0] || h == n->log_topics[1] || h == n->log_topics[2]);
}

/* copies src into buf and points *field at it, or clears the field when there is none */
static void i_rant_node_keep_str(char *buf, size_t cap, const char *src, const char **field){
    size_t len = src ? strlen(src) : 0;
    if (!len){ *field = NULL; return; }
    if (len >= cap) len = cap - 1;
    memcpy(buf, src, len); buf[len] = '\0';
    *field = buf;
}

static void i_rant_node_park_event(RantNode *n, const RantEvent *e);   /* with the ring below */
static void i_rant_node_emit(RantNode *n, RantEvent *e){
    e->user = n->user_data;
    if (e->peer){    /* resolve the peer's name once so every event message prints a label */
        RantString nm = i_rant_node_core_peer_name(n->core, e->peer);
        e->peer_name = nm.data;   /* a NUL terminated view into discovery state, NULL if unknown */
    }
    if (e->kind == RANT_ERROR){
        n->last_error = *e;
        i_rant_node_keep_str(n->last_error_topic, sizeof n->last_error_topic, e->topic_name, &n->last_error.topic_name);
        i_rant_node_keep_str(n->last_error_peer, sizeof n->last_error_peer, e->peer_name, &n->last_error.peer_name);
        i_rant_node_keep_str(n->last_error_detail, sizeof n->last_error_detail, e->schema_detail, &n->last_error.schema_detail);
    }
    /* the error to log mirror records only, the poll pass publishes. Errors on a log
       topic itself are excluded, and nothing is recorded during a flush */
    if (e->kind == RANT_ERROR && n->log_errors && !n->log_flushing
        && !(e->topic_name && i_rant_node_is_log_topic(n, e->topic))){
        uint8_t i;
        if (!n->log_pend){                 /* the first error ever: allocate the mirror ring */
            n->log_pend = (i_RantLogPend*)i_rant_node_alloc(n, NULL,
                              sizeof(i_RantLogPend) * RANT_LOG_PEND);
            if (!n->log_pend){ n->log_pend_dropped++; goto pend_done; }
        }
        for (i = 0; i < n->log_pend_n; i++)
            if (n->log_pend[i].error == (uint8_t)e->error && n->log_pend[i].topic == e->topic
                && n->log_pend[i].peer == e->peer) break;
        if (i < n->log_pend_n) n->log_pend[i].count++;
        else if (n->log_pend_n < RANT_LOG_PEND){
            i_RantLogPend *p = &n->log_pend[n->log_pend_n++];
            p->error = (uint8_t)e->error; p->topic = e->topic; p->peer = e->peer; p->count = 1;
            p->wall_us = i_rant_plat_wall_us(); p->mono_us = i_rant_plat_now_us();
            rant_event_str(e, p->text, sizeof p->text);
        } else n->log_pend_dropped++;
    }
pend_done:
    if (e->kind == RANT_PEER_UP || e->kind == RANT_PEER_DOWN || e->kind == RANT_PEER_INTEREST){
        n->settle_topology_us = i_rant_plat_now_us();     /* rant_node_settle's quiet window clock */
        n->match_epoch++;                                 /* stale the topics' converged memos */
    }
    if (n->sys_on_event) n->sys_on_event(n->sys_user, e);
    if (!n->on_event) return;
    if (n->event_queue && n->event_q) i_rant_node_park_event(n, e);
    else n->on_event(e);
}

/* our topic's name as a C string, NULL if undefined: the topic_name view on events */
static const char *i_rant_node_topic_name(RantNode *n, uint16_t topic_index){
    RantString s = rant_transport_topic_name(n->transport, topic_index);
    return (const char*)s.data;
}

/* Strips the stamps a stamped publisher prepends, filling *written_us and *capture_us.
 * The one strip point: every delivery path runs through here before the header split. */
static RantBytes i_rant_node_strip_ts(RantBytes wire, int stamped, uint64_t *written_us,
                                      uint64_t *capture_us){
    uint64_t w;
    *written_us = 0; *capture_us = 0;
    if (!stamped || wire.len < RANT_TIMESTAMP_BYTES) return wire;
    w = i_rant_le_r64(wire.data);
    *written_us = w & RANT_STAMP_MASK;
    if (!(w & RANT_STAMP_CAPTURE))
        return rant_bytes(wire.data + RANT_TIMESTAMP_BYTES, wire.len - RANT_TIMESTAMP_BYTES);
    /* the marker promises a second slot: a sample too short for it is malformed, keep the
       bytes whole rather than reading past them */
    if (wire.len < (size_t)RANT_TIMESTAMP_BYTES + RANT_CAPTURE_BYTES) return wire;
    *capture_us = i_rant_le_r64(wire.data + RANT_TIMESTAMP_BYTES);
    return rant_bytes(wire.data + RANT_TIMESTAMP_BYTES + RANT_CAPTURE_BYTES,
                      wire.len - RANT_TIMESTAMP_BYTES - RANT_CAPTURE_BYTES);
}

/* Splits a delivered wire into its pattern header and the payload after it, so the schema
 * validates a message free payload. A plain topic yields an empty header. */
static void i_rant_node_split(RantTopic *h, RantBytes wire, RantBytes *hdr, RantBytes *payload){
    size_t pfx = (h && (uint32_t)wire.len >= h->prefix_bytes) ? h->prefix_bytes : 0u;
    if (pfx && h->prefix_string && wire.len > pfx){
        size_t ml = wire.data[pfx], avail = wire.len - pfx - 1u;
        pfx += 1u + (ml <= avail ? ml : avail);
    }
    hdr->data = pfx ? wire.data : NULL; hdr->len = pfx;
    payload->data = wire.data + pfx; payload->len = wire.len - pfx;
}

/* the consumer queue ring */

/* the oldest record with the wrap normalized into tail, NULL when empty */
static const i_RantQRec *i_rant_q_peek(i_RantMsgQueue *q){
    if (!q->count) return NULL;
    if (q->cap - q->tail < (uint32_t)sizeof(i_RantQRec) ||
        ((const i_RantQRec*)(q->buf + q->tail))->rec_bytes == RANT__QWRAP)
        q->tail = 0;
    return (const i_RantQRec*)(q->buf + q->tail);
}

/* drops the oldest record, never a viewed one */
static void i_rant_q_pop(i_RantMsgQueue *q){
    const i_RantQRec *rec = i_rant_q_peek(q);
    if (!rec) return;
    q->tail += rec->rec_bytes;
    q->bytes -= rec->rec_bytes;
    if (--q->count == 0){ q->head = 0; q->tail = 0; }
}

/* contiguous space for need bytes, fills *at and pre writes the wrap sentinel */
static int i_rant_q_fit(i_RantMsgQueue *q, uint32_t need, uint32_t *at){
    if (!q->buf || need > q->cap) return 0;
    if (q->count == 0){ q->head = 0; q->tail = 0; *at = 0; return 1; }
    if (q->head > q->tail){
        if (q->cap - q->head >= need){ *at = q->head; return 1; }
        if (q->tail >= need){                                    /* wrap to the start */
            if (q->cap - q->head >= 4u)
                ((i_RantQRec*)(q->buf + q->head))->rec_bytes = RANT__QWRAP;
            *at = 0; return 1;
        }
        return 0;
    }
    if (q->head < q->tail && q->tail - q->head >= need){ *at = q->head; return 1; }
    return 0;                                                    /* full (head == tail) */
}

/* Relinearizes into a bigger ring, doubling toward cap_limit. One over cap message still
 * fits. 0 = cannot grow now: at the cap, OOM, or a live take view pins the ring. */
static int i_rant_q_grow(RantNode *n, i_RantMsgQueue *q, uint32_t need_total, uint32_t need_one){
    uint32_t target = q->cap ? q->cap * 2u : 4096u;
    uint8_t *nb;
    if (q->viewing) return 0;
    if (target < 4096u) target = 4096u;
    while (target < need_total && target < 0x80000000u) target *= 2u;
    if (target > q->cap_limit) target = q->cap_limit;
    if (target < need_one) target = RANT__QALIGN(need_one);        /* one message always fits */
    if (target <= q->cap) return 0;
    nb = (uint8_t*)i_rant_node_alloc(n, NULL, target);
    if (!nb) return 0;
    {   uint32_t off = 0, i, src = q->tail;                        /* compact, oldest first */
        for (i = 0; i < q->count; i++){
            const i_RantQRec *rec;
            if (q->cap - src < (uint32_t)sizeof(i_RantQRec) ||
                ((const i_RantQRec*)(q->buf + src))->rec_bytes == RANT__QWRAP) src = 0;
            rec = (const i_RantQRec*)(q->buf + src);
            memcpy(nb + off, rec, rec->rec_bytes);
            off += rec->rec_bytes; src += rec->rec_bytes;
        }
        q->tail = 0; q->head = off;
    }
    if (q->buf) i_rant_node_alloc(n, q->buf, 0);
    q->buf = nb; q->cap = target;
    return 1;
}

/* best effort queue loss is loss like any other: RANT_MSG_LOST, never silent */
static void i_rant_node_queue_lost(RantNode *n, uint16_t topic_index, uint32_t from, uint32_t count){
    RantEvent e; memset(&e, 0, sizeof e);
    e.kind = RANT_MSG_LOST; e.topic = topic_index; e.peer = from;
    e.topic_name = i_rant_node_topic_name(n, topic_index);
    e.lost_count = count;
    i_rant_node_emit(n, &e);
}

/* Enqueues one record, hdr then data as one body. 0 = accepted (stored, or dropped per
 * the best effort contract), 1 = refused, a reliable queue at cap: the transport parks and
 * flow control backpressures. kind 0 is a delivered message. recv_us 0 = now. */
static int i_rant_node_queue_push(RantNode *n, uint16_t topic_index, i_RantMsgQueue *q,
                                  uint32_t from, RantBytes hdr, RantBytes data,
                                  uint64_t written_us, uint64_t capture_us,
                                  uint8_t kind, uint64_t recv_us){
    RantString name = i_rant_node_core_peer_name(n->core, from);
    uint32_t name_len = name.len > RANT_NODE_NAME_MAX ? (uint32_t)RANT_NODE_NAME_MAX
                                                      : (uint32_t)name.len;
    uint32_t payload_off = RANT__QALIGN(sizeof(i_RantQRec) + name_len);
    uint32_t body_len = (uint32_t)(hdr.len + data.len);
    uint32_t need = RANT__QALIGN(payload_off + body_len);
    uint32_t at = 0, evicted = 0;
    for (;;){
        if (i_rant_q_fit(q, need, &at)) break;
        if (i_rant_q_grow(n, q, q->bytes + need, need)) continue;
        if (q->reliable){ q->parked = 1; return 1; }
        if (!q->viewing && q->count){                        /* overwrite the oldest */
            i_rant_q_pop(q); q->dropped++; evicted++; continue;
        }
        q->dropped++;                     /* a live view pins the ring: drop the incoming */
        if (!q->silent) i_rant_node_queue_lost(n, topic_index, from, evicted + 1u);
        return 0;
    }
    {   i_RantQRec *rec = (i_RantQRec*)(q->buf + at);
        rec->rec_bytes = need; rec->data_len = body_len;
        rec->t_recv_us = recv_us ? recv_us : i_rant_plat_now_us();
        rec->t_written_us = written_us;
        rec->t_capture_us = capture_us;
        rec->publisher_id = from; rec->name_len = (uint8_t)name_len;
        rec->kind = kind; rec->pad[0] = rec->pad[1] = 0;
        if (name_len) memcpy((uint8_t*)rec + sizeof *rec, name.data, name_len);
        if (hdr.len) memcpy(q->buf + at + payload_off, hdr.data, hdr.len);
        if (data.len) memcpy(q->buf + at + payload_off + hdr.len, data.data, data.len);
        q->head = at + need;
        q->bytes += need; q->count++;
    }
    if (evicted && !q->silent) i_rant_node_queue_lost(n, topic_index, from, evicted);
    return 0;
}

/* Parks an event: the struct, then topic_name, peer_name and schema_detail copied, since
 * every one of them is a view that dies with a retire or a peer drop. Lock held. */
static void i_rant_node_park_event(RantNode *n, const RantEvent *e){
    uint8_t strs[3u * (3u + 255u)]; size_t off = 0; int i;
    const char *src[3];
    src[0] = e->topic_name; src[1] = e->peer_name; src[2] = e->schema_detail;
    for (i = 0; i < 3; i++){
        size_t len = src[i] ? strlen(src[i]) : 0;
        if (len > 255u) len = 255u;
        strs[off] = (uint8_t)len; strs[off + 1] = (uint8_t)(src[i] != NULL);   /* NULL stays NULL */
        if (len) memcpy(strs + off + 2, src[i], len);
        strs[off + 2 + len] = 0;                                              /* the view's terminator */
        off += 3 + len;
    }
    (void)i_rant_node_queue_push(n, 0, n->event_q, e->peer, rant_bytes((const uint8_t*)e, sizeof *e),
                                 rant_bytes(strs, off), 0, 0, I_RANT_REC_EVENT, 0);
}

/* the parked event as a RantEvent whose strings point into the record, valid for the callback */
static void i_rant_node_queue_event(RantNode *n, const i_RantQRec *rec, RantEvent *out){
    const uint8_t *body = (const uint8_t*)rec + RANT__QALIGN(sizeof *rec + rec->name_len);
    const uint8_t *p = body + sizeof *out; const char **dst[3]; int i;
    memcpy(out, body, sizeof *out);
    out->user = n->user_data;
    dst[0] = &out->topic_name; dst[1] = &out->peer_name; dst[2] = &out->schema_detail;
    for (i = 0; i < 3; i++){
        size_t len = p[0];
        *dst[i] = p[1] ? (const char*)(p + 2) : NULL;   /* NUL terminated: a 0 byte follows */
        p += 3 + len;
    }
}

/* A queued RantMsg: every view points at stable memory, valid until the next take or
 * dispatch. The schema is re resolved now, since the delivery map may repoint. */
static void i_rant_node_queue_msg(RantNode *n, RantTopic *h, const i_RantQRec *rec, RantMsg *m){
    memset(m, 0, sizeof *m);
    m->node = n; m->user = n->user_data;
    m->topic_index = h->index;
    m->publisher_id = rec->publisher_id;
    m->publisher_name = rec->name_len ? rant_string((const char*)(rec + 1), rec->name_len)
                                   : rant_cstr("unknown-peer");
    m->topic_name = rant_string(h->name, h->name_len);
    if (rec->kind & I_RANT_REC_SYNTH){     /* the patterns layer's own blob, whole */
        m->header = rant_bytes(NULL, 0);
        m->data = rant_bytes((const uint8_t*)rec + RANT__QALIGN(sizeof *rec + rec->name_len), rec->data_len);
        m->schema = NULL;
    } else {
        i_rant_node_split(h, rant_bytes((const uint8_t*)rec + RANT__QALIGN(sizeof *rec + rec->name_len),
                             rec->data_len), &m->header, &m->data);
        m->schema = i_rant_node_core_msg_schema(n->core, rec->publisher_id, h->index);
        if (h->prefix_bytes && m->data.len == 0) m->schema = NULL;   /* an op only pattern message */
    }
    m->recv_us = rec->t_recv_us;
    m->written_us = rec->t_written_us;
    m->capture_us = rec->t_capture_us;
}

/* Creates the queue. An explicit queue_bytes allocates in full, the lazy default starts at
 * one page and grows toward RANT_QUEUE_CAP. NULL on OOM. */
static i_RantMsgQueue *i_rant_node_queue_ensure(RantNode *n, RantTopic *h, const RantQos *qos){
    i_RantMsgQueue *q = h->q;
    uint32_t limit, initial;
    if (q) return q;
    if (!qos) qos = rant_transport_topic_qos(n->transport, h->index);
    limit = RANT__QALIGN((qos && qos->queue_bytes) ? qos->queue_bytes : RANT_QUEUE_CAP);
    initial = (qos && qos->queue_bytes) ? limit : (limit < 4096u ? limit : 4096u);
    q = (i_RantMsgQueue*)i_rant_node_alloc(n, NULL, sizeof *q);
    if (!q) return NULL;
    memset(q, 0, sizeof *q);
    q->buf = (uint8_t*)i_rant_node_alloc(n, NULL, initial);
    if (!q->buf){ i_rant_node_alloc(n, q, 0); return NULL; }
    q->cap = initial; q->cap_limit = limit;
    q->reliable = (qos && qos->reliability == RANT_RELIABLE) ? 1u : 0u;
    h->q = q;
    return q;
}

/* Frees a topic's ring, if any. Lock held. */
static void i_rant_node_queue_drop(RantNode *n, RantTopic *h){
    if (!h->q) return;
    if (h->q->buf) i_rant_node_alloc(n, h->q->buf, 0);
    i_rant_node_alloc(n, h->q, 0);
    h->q = NULL;
}

/* Finishes an outstanding take view, then retries parked lanes into the freed space. Their
 * acks need a TX pass, so kick. Lock held. */
static void i_rant_node_queue_release(RantNode *n, RantTopic *h, i_RantMsgQueue *q){
    if (q->viewing){
        q->viewing = 0;
        i_rant_q_pop(q);
    }
    if (q->parked){
        q->parked = rant_transport_deliver_parked(n->transport, h->index,
                                                  i_rant_plat_now_us()) ? 1u : 0u;
        i_rant_node_kick(n);
    }
}

/* The process global last error slot for failures during open, before the node exists.
 * A plain global, meaningful right after a failed open on the calling thread. */
static RantEvent g_last_error = { RANT_ERROR };   /* kind RANT_ERROR with .error NONE = "no error" */

/* Reports an open time failure into the global slot and the caller's on_event. Returns NULL. */
static RantNode *i_rant_node_open_fail(RantEventFn on_event, void *user, RantErrorKind err,
                            int os_error, uint16_t port, uint64_t need){
    RantEvent e; memset(&e, 0, sizeof e);
    e.kind = RANT_ERROR; e.error = err; e.user = user;
    e.os_error = os_error; e.port = port; e.too_big_bytes = need;
    g_last_error = e;
    if (on_event) on_event(&e);
    return NULL;
}

RantEvent rant_last_error(RantNode *n){ return n ? n->last_error : g_last_error; }

/* Builds a RantMsg and hands it to the app, inline or copied into the consumer queue.
 * Returns 0 accepted, nonzero refused (a reliable queue at cap, the transport parks). */
static int i_rant_node_deliver(RantNode *n, uint16_t topic_index, uint32_t from, RantBytes data){
    RantMsg m;
    RantTopic *h = (topic_index < i_rant_node_topic_hi(n)) ? n->handles[topic_index] : NULL;
    RantBytes hdr, payload, body;
    uint64_t written_us, capture_us;
    const RantSchema *schema = i_rant_node_core_msg_schema(n->core, from, topic_index);
    /* the stamps first, then the header split, then the schema validates the payload */
    body = i_rant_node_strip_ts(data,
              rant_transport_peer_timestamped(n->transport, topic_index, from),
              &written_us, &capture_us);
    i_rant_node_split(h, body, &hdr, &payload);
    /* an op only pattern message (a zero payload, or a task op that is not CALL) skips
       the schema, the pattern layer judges it. See spec/node.md */
    if (h && h->prefix_bytes
        && (payload.len == 0
            || (h->kind == RANT_KIND_TASK_REQ && hdr.len >= 5 && hdr.data[4] != 0)))
        schema = NULL;
    if (schema && !rant_schema_validate(schema, payload)){
        RantEvent e; memset(&e, 0, sizeof e);
        e.kind = RANT_ERROR; e.error = RANT_E_SCHEMA_MISMATCH;
        e.peer = from; e.topic = topic_index; e.topic_name = i_rant_node_topic_name(n, topic_index);
        e.schema_detail = i_rant_node_core_note_size_mismatch(n->core, from, topic_index,
                                            payload.len, rant_schema_msg_min(schema));
        i_rant_node_emit(n, &e);
        return 0;
    }
    if (h && h->q && h->kind == RANT_KIND_TOPIC){
        /* store the wire minus the stamps, the stamps ride the record */
        if (i_rant_node_queue_push(n, topic_index, h->q, from, rant_bytes(NULL, 0), body,
                                   written_us, capture_us, 0, 0))
            return 1;   /* parked: the accepted retry re counts */
        h->rx_msgs++; h->rx_bytes += data.len;
        return 0;
    }
    if (h){ h->rx_msgs++; h->rx_bytes += data.len; }
    if (!(h && h->sys_on_message) && !n->user_on_message) return 0;
    memset(&m, 0, sizeof m);
    m.node = n; m.user = n->user_data;
    m.topic_index = topic_index; m.publisher_id = from;
    m.publisher_name = i_rant_node_core_peer_name(n->core, from);            /* a discovery view */
    if (!m.publisher_name.data) m.publisher_name = rant_cstr("unknown-peer"); /* never NULL */
    m.topic_name = rant_transport_topic_name(n->transport, topic_index);
    m.header = hdr; m.data = payload;
    m.schema = schema;
    m.recv_us = i_rant_plat_now_us();
    m.written_us = written_us;
    m.capture_us = capture_us;
    if (h && h->sys_on_message) h->sys_on_message(h->sys_msg_user, &m);    /* the patterns layer */
    else n->user_on_message(&m);
    return 0;
}
static int i_rant_node_on_message(void *u, uint16_t topic_index, uint32_t from, RantBytes data){
    return i_rant_node_deliver((RantNode*)u, topic_index, from, data);
}
/* Node core events funnel through here. The core sets ev->user to this node, swap it for
 * the app's user_data before handing on. */
static void i_rant_node_on_event(const RantEvent *ev){
    RantNode *n = (RantNode*)ev->user;
    /* dynamic mode has no peer cap: a refusal means grow at the next poll, out of this
       callback, and the peer's next announce is admitted. Static mode surfaces it */
    if (n->alloc_dynamic && ev->kind == RANT_ERROR && ev->error == RANT_E_PEER_REFUSED){
        n->grow_pending = 1; return;
    }
    /* a blob we cannot hold is a growth signal too: grow at the next poll, then solicit.
       Sizes past the wire ceiling and a size that already failed surface */
    if (n->alloc_dynamic && ev->kind == RANT_ERROR && ev->error == RANT_E_PEER_META_TOO_BIG){
        uint64_t need = ev->too_big_bytes;
        if (need > n->meta_cap && need <= 65000u && (uint16_t)need != n->meta_grow_failed){
            n->meta_grow_need = (uint16_t)need;
            return;
        }
    }
    { RantEvent e = *ev; i_rant_node_emit(n, &e); }
}

/* the transport's schema gate, answered by the node core. u is the node */
static int i_rant_node_schema_check(void *u, uint32_t peer, uint16_t topic_index,
                                    int peer_is_pub, uint64_t hash, RantBytes wire){
    return i_rant_node_core_schema_check(((RantNode*)u)->core, peer, topic_index,
                                         peer_is_pub, hash, wire);
}

/* the transport's source clock: the wall clock, since it is read on other hosts */
static uint64_t i_rant_node_source_time(void *u){ (void)u; return i_rant_plat_wall_us(); }

/* Maps a transport event onto the app RantEvent. MSG_LOST is an info kind, everything
 * else is a RANT_ERROR with its RantErrorKind, and topic scoped kinds get our name. */
static void i_rant_node_on_transport_event(const RantTransportEvent *tev){
    RantNode *n = (RantNode*)tev->user;
    RantEvent e;
    memset(&e, 0, sizeof e);
    e.peer = tev->peer; e.topic = tev->topic;
    e.lost_first = tev->lost_first; e.lost_count = tev->lost_count;
    e.too_big_bytes = tev->too_big_bytes; e.identity = tev->identity;
    switch (tev->kind){
    case RANT_TRANSPORT_MSG_LOST:
        e.kind = RANT_MSG_LOST; e.topic_name = i_rant_node_topic_name(n, tev->topic); break;
    case RANT_TRANSPORT_MSG_TOO_BIG:
        e.kind = RANT_ERROR; e.error = RANT_E_MSG_TOO_BIG; e.topic_name = i_rant_node_topic_name(n, tev->topic); break;
    case RANT_TRANSPORT_NAME_COLLISION:
        e.kind = RANT_ERROR; e.error = RANT_E_NAME_COLLISION; e.topic_name = i_rant_node_topic_name(n, tev->topic); break;
    case RANT_TRANSPORT_QOS_INCOMPATIBLE:
        e.kind = RANT_ERROR; e.error = RANT_E_QOS_INCOMPATIBLE; e.topic_name = i_rant_node_topic_name(n, tev->topic); break;
    case RANT_TRANSPORT_KIND_MISMATCH:
        e.kind = RANT_ERROR; e.error = RANT_E_KIND_MISMATCH; e.topic_name = i_rant_node_topic_name(n, tev->topic); break;
    case RANT_TRANSPORT_SCHEMA_MISMATCH:
        e.kind = RANT_ERROR; e.error = RANT_E_SCHEMA_MISMATCH; e.topic_name = i_rant_node_topic_name(n, tev->topic);
        e.schema_detail = i_rant_node_core_schema_why(n->core, tev->peer, tev->topic,
                                                      tev->peer_is_pub); break;
    case RANT_TRANSPORT_INTEREST_OVERFLOW:
        e.kind = RANT_ERROR; e.error = RANT_E_INTEREST_OVERFLOW; break;
    case RANT_TRANSPORT_META_TRUNCATED_INTEREST:
        e.kind = RANT_ERROR; e.error = RANT_E_META_TRUNCATED_INTEREST; break;
    case RANT_TRANSPORT_META_TRUNCATED_SCHEMA:
        e.kind = RANT_ERROR; e.error = RANT_E_META_TRUNCATED_SCHEMA; break;
    default: return;
    }
    i_rant_node_emit(n, &e);
}

/* The arena's sub blocks, laid out in one place so the measure pass and the build pass
 * run the same sequence and never drift. */
typedef struct {
    uint8_t   *handles, *node_core, *transport, *discovery, *rx_buf;
#ifdef RANT_SHM
    uint8_t   *shm_pool;
#endif
    size_t     node_core_bytes, transport_bytes, discovery_bytes, rx_buf_bytes;
} i_RantNodeBlocks;

static void i_rant_node_layout(i_RantBump *b, uint16_t max_peers, uint16_t max_topics,
                              const RantConfig *transport_cfg,
                              const RantDiscoveryNetConfig *discovery_rt_cfg, i_RantNodeBlocks *o){
    o->handles       = (uint8_t*)i_rant_bump_take(b, (size_t)max_topics * sizeof(RantTopic*), 16);
    o->node_core_bytes = i_rant_node_core_required_memory(max_topics);     /* no peers here */
    o->node_core = (uint8_t*)i_rant_bump_take(b, o->node_core_bytes, 16);
    o->transport_bytes = rant_transport_required_memory(transport_cfg);
    o->transport = (uint8_t*)i_rant_bump_take(b, o->transport_bytes, 16);
#ifdef RANT_SHM
    /* only the (topic, class) segment pointer table lives in the arena */
    o->shm_pool = (uint8_t*)i_rant_bump_take(b, (size_t)max_topics * RANT_SHM_N_CLASSES * sizeof(void*), 16);
#endif
    o->discovery_bytes = rant_discovery_placement_memory(discovery_rt_cfg);
    o->discovery = (uint8_t*)i_rant_bump_take(b, o->discovery_bytes, 16);
    /* the RX buffer fits a unicast announce carrying the largest blob we accept, since
       recvfrom drops an oversized datagram and a late joiner has no other path to it */
    o->rx_buf_bytes = rant_discovery_wire_size(discovery_rt_cfg->discovery.meta_cap);
    if (o->rx_buf_bytes < RANT_DGRAM_MAX) o->rx_buf_bytes = RANT_DGRAM_MAX;
    o->rx_buf = (uint8_t*)i_rant_bump_take(b, o->rx_buf_bytes, 16);
}

/* Sends one datagram to a peer. 1 when done with it, 0 only on a would block TX full. */
static int i_rant_node_tx(RantNode *n, uint32_t to, const uint8_t *buf, size_t len){
    i_RantNodeDest d;
    if (!i_rant_node_core_resolve(n->core, to, &d)) return 1;     /* the peer vanished */
    if (i_rant_plat_send(n->fd, buf, len, d.ip, d.port) < 0){
        if (i_rant_plat_would_block()) return 0;                  /* TX full: retry next tick */
        {   /* a hard failure: report and drop, reliable data is repaired. Byte 0 is type and
               flags, bytes 1 and 2 the first submessage's topic index */
            RantEvent e; memset(&e, 0, sizeof e);
            e.kind = RANT_ERROR; e.error = RANT_E_SEND; e.peer = to;
            e.os_error = i_rant_plat_last_socket_error();
            e.too_big_bytes = len;
            if (len >= 3){
                uint16_t idx = (uint16_t)(buf[1] | ((uint16_t)buf[2] << 8));
                e.topic = idx;
                e.topic_name = i_rant_node_topic_name(n, idx);
            }
            i_rant_node_emit(n, &e);
        }
    }
    return 1;
}

/* The one TX drain: retries the held datagram, then pulls until the transport is empty.
 * The core already consumed a pulled datagram, so a full socket parks it in tx_hold. */
static void i_rant_node_tx_drain(RantNode *n){
    uint8_t buf[RANT_DGRAM_MAX]; uint32_t to; size_t out_len;
    if (n->tx_hold_len && i_rant_node_tx(n, n->tx_hold_peer, n->tx_hold, n->tx_hold_len))
        n->tx_hold_len = 0;
    if (n->tx_hold_len) return;
    while (rant_transport_poll_send(n->transport, &to, buf, sizeof buf, &out_len, i_rant_plat_now_us())){
        if (!i_rant_node_tx(n, to, buf, out_len)){
            memcpy(n->tx_hold, buf, out_len);
            n->tx_hold_len = out_len; n->tx_hold_peer = to;
            return;         /* the TX buffer is full: the next pass retries */
        }
    }
}

/* What a send owes its commit: where another thread runs the loop the sender writes the
 * socket itself, a lone thread batches at its poll, a callback leaves it to its pass. */
static void i_rant_node_send_tx(RantNode *n, int acquired){
#ifdef RANT_THREADS
    if (acquired && (n->svc_running || n->pollers_sleeping) && n->fd != RANT_SOCK_BAD)
        i_rant_node_tx_drain(n);
    /* a full socket or a callback's commit is left to the poller: cut its sleep short */
    if (n->tx_hold_len || rant_transport_tx_pending(n->transport)) i_rant_node_kick(n);
#else
    (void)n; (void)acquired;
#endif
}

#ifdef RANT_SHM
/* Lazily creates our per topic segment: chunk_bytes is the size class, n_chunks the
 * keep_last, so history slot i binds chunk i. NULL on failure. */
static i_RantShmPool *i_rant_node_shm_topic_pool(RantNode *n, uint16_t topic_index, uint32_t k, uint16_t keep_last){
    i_RantShmConfig c; uint8_t *mem; uint64_t seg; size_t idx;
    if (k >= RANT_SHM_N_CLASSES) return NULL;
    idx = (size_t)topic_index * RANT_SHM_N_CLASSES + k;
    if (n->shm_pool[idx]) return (i_RantShmPool*)n->shm_pool[idx];
    memset(&c, 0, sizeof c);
    seg = n->shm_base | ((uint64_t)topic_index << 3) | (uint64_t)k;   /* class low, topic above */
    c.segment_id = seg;
    i_rant_shm_seg_name(c.name, seg);
    c.chunk_bytes = i_rant_shm_class_bytes(k);
    c.n_chunks = keep_last ? keep_last : 1u;
    mem = (uint8_t*)i_rant_node_alloc(n, NULL, i_rant_shm_state_bytes());       /* stable */
    if (!mem) return NULL;
    n->shm_pool[idx] = i_rant_shm_create(mem, &c);
    if (!n->shm_pool[idx]) i_rant_node_alloc(n, mem, 0);
    return (i_RantShmPool*)n->shm_pool[idx];
}
/* Lazily attaches a peer's segment by id and caches it. The cache grows with segments
 * actually attached, so a node with no same host peer holds none. */
static i_RantShmPool *i_rant_node_shm_reader_pool(RantNode *n, uint64_t seg){
    uint16_t i; i_RantShmConfig c; uint8_t *mem;
    for (i=0;i<n->shm_reader_count;i++)
        if (n->shm_reader_segments[i]==seg) return (i_RantShmPool*)n->shm_reader_states[i];
    if (n->shm_reader_count == n->shm_reader_cap){        /* grow the id and state arrays */
        uint16_t ncap = n->shm_reader_cap ? (uint16_t)(n->shm_reader_cap*2u) : 8u;
        uint64_t *nseg; void **nst;
        if (ncap <= n->shm_reader_cap) return NULL;       /* u16 wrap: an absurd segment count */
        nseg = (uint64_t*)i_rant_node_alloc(n, n->shm_reader_segments, (size_t)ncap*sizeof(uint64_t));
        if (!nseg) return NULL;
        n->shm_reader_segments = nseg;
        nst = (void**)i_rant_node_alloc(n, n->shm_reader_states, (size_t)ncap*sizeof(void*));
        if (!nst) return NULL;
        n->shm_reader_states = nst;
        n->shm_reader_cap = ncap;
    }
    memset(&c, 0, sizeof c);
    c.segment_id = seg; i_rant_shm_seg_name(c.name, seg);       /* attach reads the geometry */
    mem = (uint8_t*)i_rant_node_alloc(n, NULL, i_rant_shm_state_bytes());
    if (!mem) return NULL;
    if (!i_rant_shm_attach(mem, &c)){ i_rant_node_alloc(n, mem, 0); return NULL; }
    n->shm_reader_segments[n->shm_reader_count] = seg;
    n->shm_reader_states[n->shm_reader_count] = mem;
    n->shm_reader_count++;
    return (i_RantShmPool*)mem;
}
static int i_rant_node_on_shm(void *u, uint16_t topic_index, uint32_t from, const uint8_t *desc){
    RantNode *n = (RantNode*)u; i_RantShmDesc d; i_RantShmPool *reader_pool; const void *p; uint32_t len;
    if (!i_rant_shm_desc_decode(&d, desc, RANT_SHM_DESC_WIRE)) return 0;
    reader_pool = i_rant_node_shm_reader_pool(n, d.segment_id);
    if (!reader_pool) return 0;                                     /* cannot attach: NACK */
    p = i_rant_shm_read(reader_pool, &d, &len);                         /* the seqlock head */
    if (!p) return 0;                                      /* recycled: NACK, then repair or skip */
    /* one copy out of shared memory so the user owns the bytes */
    if (len > n->shm_scratch_cap){
        void *new_buf = i_rant_node_alloc(n, n->shm_scratch, len?len:1u);
        if (!new_buf) return 0;
        n->shm_scratch = new_buf; n->shm_scratch_cap = len;
    }
    memcpy(n->shm_scratch, p, len);
    if (!i_rant_shm_verify(reader_pool, &d)) return 0;                  /* the seqlock tail: torn */
    if (i_rant_node_deliver(n, topic_index, from, rant_bytes(n->shm_scratch, len)))
        return -1;                                         /* the queue is full: park */
    n->shm_rx++;
    return 1;
}
#endif

static void i_rant_node_logs_open(RantNode *n);       /* defined with the log API below */

RantAllocator rant_allocator_heap(uint32_t page_size){
    return rant_allocator_dynamic(i_rant_plat_realloc, page_size);
}

void *rant_heap_realloc(void *user, void *ptr, size_t size){
    (void)user;
    return i_rant_plat_realloc(ptr, size);
}

RantNode *rant_node_open(RantAllocator *alloc, const char *name, RantMsgFn on_message, RantEventFn on_event, const RantNodeOpts *opts){
    RantNodeOpts o; RantDiscoveryNetConfig dc; RantConfig tc; i_RantNodeBlocks blocks;
    uint16_t max_peers, max_topics, user_topics;
    uint8_t *base; void *arena; size_t need; RantAllocator pool;
    RantNode *n; i_RantSock fd; uint16_t local_port;
    char node_name[RANT_NODE_NAME_MAX + 1]; uint8_t node_name_len = 0;

    memset(&o, 0, sizeof o);
    if (opts) o = *opts;
    /* a node has no memory of its own, so no allocator is the same fault as one that
       returns NULL. Reported, never a bare NULL with an empty last error. */
    if (!alloc) return i_rant_node_open_fail(on_event, o.user_data, RANT_E_OOM, 0, 0, sizeof *n);
    user_topics = o.max_topics ? o.max_topics : 8;
    /* the builtins ride outside the app's budget, in a block above it */
    max_topics = (uint16_t)(user_topics + (o.disable_logs ? 0 : 3));
#ifndef RANT_NO_PATTERNS
    max_topics = (uint16_t)(max_topics + (o.disable_meta ? 0 : 2));
#endif
    max_peers    = o.discovery.max_peers ? o.discovery.max_peers : 16;

    /* the sub configs. Sizing depends only on counts, the callbacks are set later */
    memset(&dc,0,sizeof dc); memset(&tc,0,sizeof tc);
    dc.discovery.domain_id   = o.domain;
    dc.discovery.data_port   = o.net.data_port;
    dc.discovery.announce_interval_us = o.discovery.announce_interval_us;
    dc.discovery.peer_timeout_us = o.discovery.peer_timeout_us;
    dc.discovery.max_peers   = max_peers;
    dc.discovery.meta_cap = rant_meta_cap(max_topics);
    dc.discovery.peer_user_bytes = i_rant_node_core_peer_user_bytes();     /* the core's scratch */
    dc.discovery.alloc = i_rant_node_alloc;     /* per peer blobs at their size. Set before sizing so
                                                 measure and place agree. alloc_user is set below */
    dc.group                 = o.net.discovery_group;
    dc.discovery_port        = o.net.discovery_port;
    dc.ttl                   = o.net.multicast_ttl;
    dc.multicast_interface   = o.net.multicast_interface;
    dc.seeds                 = o.net.seed_peers;
    dc.n_seeds               = o.net.n_seed_peers;
    dc.unicast_only          = o.net.unicast_only;
    tc.topics    = NULL;            /* reserve mode: topics created at runtime */
    tc.n_topics  = max_topics;
    tc.max_peers   = max_peers;
    tc.frag_size= o.net.fragment_size;
    tc.allocator   = i_rant_node_alloc;     /* required by rant_transport_init */

    {   i_RantBump b; memset(&b,0,sizeof b);
        i_rant_node_layout(&b, max_peers, max_topics, &tc, &dc, &blocks);
        need = b.offset + 32u; }

    /* The node copies the caller's allocator into its own pool, so the caller's may be a
       temporary. The node struct stays put across a grow, the arena is relocated. */
    pool = *alloc;
    n = (RantNode*)rant_allocator_alloc(&pool, NULL, sizeof *n);
    if (!n) return i_rant_node_open_fail(on_event, o.user_data, RANT_E_OOM, 0, 0, sizeof *n);
    memset(n, 0, sizeof *n);
    n->pool = pool;                                  /* the node owns the pool now */
    n->alloc_dynamic = (alloc->page_realloc != NULL);
    arena = rant_allocator_alloc(&n->pool, NULL, need);
    if (!arena){ RantAllocator p = n->pool; rant_allocator_reset(&p);
                 return i_rant_node_open_fail(on_event, o.user_data, RANT_E_OOM, 0, 0, need); }
    base = (uint8_t*)(((uintptr_t)arena+15u)&~(uintptr_t)15u);
    {   i_RantBump b; memset(&b,0,sizeof b);
        b.base = base; b.cap = need - (size_t)(base - (uint8_t*)arena);
        i_rant_node_layout(&b, max_peers, max_topics, &tc, &dc, &blocks); }

    if (!i_rant_plat_startup()){
        RantAllocator p = n->pool; rant_allocator_reset(&p);
        return i_rant_node_open_fail(on_event, o.user_data, RANT_E_PLATFORM, 0, 0, 0);
    }

    n->fd = RANT_SOCK_BAD;
    n->user_data = o.user_data;      /* set early so emit can stamp any open time error */
    n->on_event = on_event;
#ifdef RANT_THREADS
    i_rant_plat_mutex_init(&n->mu);
    i_rant_plat_cond_init(&n->cv);
    /* opened for every node: it also wakes a plain poll when another thread sends. A
       platform whose loopback cannot carry it degrades, reported once. See spec/node.md */
    if (!i_rant_plat_waker_open(&n->waker)){
        RantEvent e; memset(&e, 0, sizeof e);
        e.kind = RANT_ERROR; e.error = RANT_E_WAKER;
        i_rant_node_emit(n, &e);
    }
#endif
    n->domain = o.domain;
    n->net = o.net;
    n->announce_us = o.discovery.announce_interval_us ? o.discovery.announce_interval_us
                                                      : 3000000u;   /* discovery's default */
    n->match_wait_us = o.match_wait_ms < 0 ? 0u
                     : o.match_wait_ms ? (uint32_t)o.match_wait_ms * 1000u
                                       : (uint32_t)RANT_MATCH_WAIT_MS * 1000u;
    n->match_epoch = 1;   /* a topic memo at 0 = never computed, so a first send always checks */
    n->event_queue_bytes = o.event_queue_bytes ? o.event_queue_bytes : (64u << 10);
    n->user_on_message = on_message;
    n->arena = arena;
    n->handles = (RantTopic**)blocks.handles;
    memset(n->handles, 0, (size_t)max_topics * sizeof(RantTopic*));
    n->rx_buf = blocks.rx_buf; n->rx_buf_bytes = blocks.rx_buf_bytes;
    n->max_topics = max_topics;
    n->builtin_lo = user_topics;           /* the builtin block sits above the app's budget */
    n->max_peers = max_peers;
    n->meta_cap = dc.discovery.meta_cap;   /* the initial accept bound, self heals up */

    tc.on_message = i_rant_node_on_message;       /* wrapped so on_message receives a RantMsg */
    tc.on_event   = i_rant_node_on_transport_event;
    tc.schema_check = i_rant_node_schema_check;
    tc.source_time = i_rant_node_source_time;
    tc.user       = n;
#ifdef RANT_SHM
    n->shm_capable = (uint8_t)(n->alloc_dynamic && !o.disable_shm);   /* never in static mode */
    if (n->shm_capable){
        i_rant_plat_host_uuid(n->shm_host);
        if (!i_rant_plat_random(&n->shm_base, sizeof n->shm_base)) n->shm_base = i_rant_plat_pid();
        n->shm_base ^= (uint64_t)i_rant_plat_pid() << 32;      /* unique per process */
        n->shm_base &= ~(((uint64_t)1u << 19) - 1u);       /* low 19 bits: 3 class, 16 topic */
        if (n->shm_base == 0) n->shm_base = (uint64_t)1u << 19;
    }
    tc.on_shm = i_rant_node_on_shm;
#endif

    n->transport = rant_transport_init(blocks.transport, blocks.transport_bytes, &tc);
    if (!n->transport){ (void)i_rant_node_open_fail(on_event, o.user_data, RANT_E_OOM, 0, 0, 0); goto fail_threads; }
#ifdef RANT_SHM
    {   uint32_t n_segments = (uint32_t)max_topics * RANT_SHM_N_CLASSES; uint32_t i;
        n->shm_n_topics = max_topics;
        n->shm_pool = (void**)blocks.shm_pool;
        for (i=0;i<n_segments;i++) n->shm_pool[i]=NULL;
        n->shm_reader_segments = NULL; n->shm_reader_states = NULL;   /* lazy */
        n->shm_reader_cap = 0; n->shm_reader_count = 0;
    }
#endif

    /* the sans-IO node core, bound to discovery's peer table below once it exists */
    node_name_len = rant_discovery_default_name(node_name, sizeof node_name, name);
    memcpy(n->name, node_name, node_name_len); n->name[node_name_len] = '\0';   /* snapshot copy */
    n->name_len = node_name_len;
    {   i_RantNodeCoreConfig cc;
        memset(&cc, 0, sizeof cc);
        cc.transport = n->transport;   /* cc.discovery is bound after rant_discovery_place */
        cc.n_topics = max_topics; cc.frag_size = rant_clamp_frag(o.net.fragment_size);
        cc.on_event = i_rant_node_on_event; cc.user = n;
        cc.alloc = i_rant_node_alloc; cc.alloc_user = n;     /* backs the peer schema state */
        cc.fetch_details = o.fetch_details;
#ifdef RANT_SHM
        cc.oob_capable = n->shm_capable; memcpy(cc.oob_host, n->shm_host, 16);
#endif
        n->core = i_rant_node_core_init(blocks.node_core, blocks.node_core_bytes, &cc);
        if (!n->core){ (void)i_rant_node_open_fail(on_event, o.user_data, RANT_E_OOM, 0, 0, 0); goto fail_threads; }
    }

    /* the data socket is bound before discovery opens so we advertise its real port. No
       reuse: a unicast endpoint owns its port, so a collision fails loudly here */
    fd = i_rant_plat_udp_open();
    if (fd==RANT_SOCK_BAD){ (void)i_rant_node_open_fail(on_event, o.user_data, RANT_E_SOCKET, i_rant_plat_last_socket_error(), 0, 0); goto fail_threads; }
    n->fd=fd;                       /* owned now: fail_sock closes it */
    if (!i_rant_plat_bind(fd, 0, o.net.data_port, 0)){ (void)i_rant_node_open_fail(on_event, o.user_data, RANT_E_BIND, i_rant_plat_last_socket_error(), o.net.data_port, 0); goto fail_sock; }
    local_port = i_rant_plat_local_port(fd);
    if (local_port==0){ (void)i_rant_node_open_fail(on_event, o.user_data, RANT_E_SOCKET, 0, 0, 0); goto fail_sock; }
    i_rant_plat_set_nonblock(fd);     /* never block in recv or send, poll drains the queue */
    i_rant_plat_suppress_connreset(fd);    /* suppress WSAECONNRESET from a bounced send */
    if (o.net.recv_buffer_bytes) i_rant_plat_set_rcvbuf(fd, (int)o.net.recv_buffer_bytes);
    if (o.net.send_buffer_bytes) i_rant_plat_set_sndbuf(fd, (int)o.net.send_buffer_bytes);
    /* our advertised locator: the port we really bound and no address, unless the caller
       states one outright. See docs/discovery.md */
    dc.discovery.data_port = o.net.advertise_port ? o.net.advertise_port : local_port;
    if (o.net.self_ip){
        uint32_t naddr = i_rant_plat_parse_ip(o.net.self_ip);
        /* 0 and 0xFFFFFFFF are inet_addr's failure value and the broadcast address, neither
           a unicast locator, so a bad string is a config error */
        if (!naddr || naddr == 0xFFFFFFFFu){
            (void)i_rant_node_open_fail(on_event, o.user_data, RANT_E_BAD_ADDRESS, 0, 0, 0);
            goto fail_sock;
        }
        i_rant_plat_naddr_to_ip4(naddr, dc.discovery.self_ip);
        dc.discovery.self_ip_len = 4;
    }

    dc.discovery.on_event = i_rant_node_core_on_disc_event;     /* the core demuxes peer events */
    dc.discovery.user     = n->core;
    dc.discovery.alloc_user = n;               /* the blob hook allocates from the node's pool */
    dc.discovery.name     = rant_string(node_name, node_name_len);     /* discovery owned */
    /* the core builds our overlay, discovery wraps it in its blob after the locator and name */
    i_rant_node_core_build_meta(n->core);
    dc.discovery.meta = i_rant_node_core_meta(n->core);
    n->discovery = rant_discovery_place(blocks.discovery, blocks.discovery_bytes, &dc);
    if (!n->discovery){
        RantErrorKind err;     /* discovery's setup failure in our vocabulary */
        switch (rant_discovery_last_error()){
        case RANT_DISCOVERY_E_PLATFORM:     err = RANT_E_PLATFORM;     break;
        case RANT_DISCOVERY_E_SOCKET:       err = RANT_E_SOCKET;       break;
        case RANT_DISCOVERY_E_BIND:         err = RANT_E_BIND;         break;
        case RANT_DISCOVERY_E_MCAST_JOIN: err = RANT_E_MCAST_JOIN; break;
        default:                          err = RANT_E_OOM;          break;
        }
        (void)i_rant_node_open_fail(on_event, o.user_data, err, rant_discovery_last_os_error(),
                                    o.net.discovery_port, 0);
        goto fail_sock;
    }
    /* the node core delegates address resolution and per peer scratch to discovery's table */
    i_rant_node_core_bind_discovery(n->core, rant_discovery_state(n->discovery));
    i_rant_node_core_set_self_name(n->core, rant_string(n->name, strlen(n->name)));

    /* every unicast discovery send goes out of the data socket, so a NAT's per flow
       mappings are the ones the data will use. See spec/discovery.md */
    rant_discovery_set_tx_fd(n->discovery, fd);

    /* the post open gather anchor: discovery solicits on startup, so every peer already
       out there answers within an RTT of the first poll */
    n->open_us = i_rant_plat_now_us();
    n->last_error.kind = RANT_ERROR;     /* .error NONE until one fires: reads as "no error" */

    /* the builtins last, since they create topics. A failed creation degrades and never
       fails the open */
    n->log_errors = (uint8_t)(!o.disable_error_logs && !o.disable_logs);
    n->creating_builtin = 1;               /* allocate from the builtin block */
    if (!o.disable_logs) i_rant_node_logs_open(n);
#ifndef RANT_NO_PATTERNS
    if (!o.disable_meta) i_rant_patterns_meta_open(n);
#endif
    n->creating_builtin = 0;
    return n;

fail_sock:
    if (n->fd != RANT_SOCK_BAD) i_rant_plat_close(n->fd);
    n->fd = RANT_SOCK_BAD;
fail_threads:
#ifdef RANT_THREADS
    if (n->waker.fd != RANT_SOCK_BAD) i_rant_plat_waker_close(&n->waker);
    i_rant_plat_cond_destroy(&n->cv);
    i_rant_plat_mutex_destroy(&n->mu);
#endif
    i_rant_plat_cleanup();
    { RantAllocator p = n->pool; rant_allocator_reset(&p); }       /* frees the node struct and arena */
    return NULL;
}

/* Dynamic mode growth: relocates the node into a bigger arena at the given counts. Message
 * buffers, SHM segments and the user held handles stay put. 0 leaves n unchanged. */
static int i_rant_node_grow(RantNode *n, uint16_t new_max_peers, uint16_t new_max_topics,
                            uint16_t want_meta_cap){
    RantConfig tc; RantDiscoveryNetConfig dc; i_RantNodeBlocks nb; i_RantBump b;
    RantTransportState *nt; i_RantNodeCore *ncore; RantDiscovery *ndisc;
    void *new_arena, *old_arena = n->arena;
    uint8_t *nbase; size_t need;
    uint16_t old_max_topics = n->max_topics;
    /* the accept bound never shrinks: topic derived, previously grown, or requested */
    uint16_t new_meta_cap = rant_meta_cap(new_max_topics);
    if (n->meta_cap > new_meta_cap) new_meta_cap = n->meta_cap;
    if (want_meta_cap    > new_meta_cap) new_meta_cap = want_meta_cap;

    if (!n->alloc_dynamic) return 0;
    if (new_max_peers <= n->max_peers && new_max_topics <= n->max_topics
        && new_meta_cap <= n->meta_cap) return 0;

    memset(&tc,0,sizeof tc); memset(&dc,0,sizeof dc);
    tc.topics=NULL; tc.n_topics=new_max_topics; tc.max_peers=new_max_peers;
    tc.allocator=i_rant_node_alloc; tc.frag_size=n->net.fragment_size;
    dc.discovery.max_peers=new_max_peers; dc.discovery.meta_cap=new_meta_cap;
    dc.discovery.peer_user_bytes = i_rant_node_core_peer_user_bytes();     /* scratch to match */
    /* sizing must match the live core's hook mode: no arena blob pool */
    dc.discovery.alloc = i_rant_node_alloc; dc.discovery.alloc_user = n;

    memset(&b,0,sizeof b);
    i_rant_node_layout(&b, new_max_peers, new_max_topics, &tc, &dc, &nb);
    need = b.offset + 32u;
    new_arena = rant_allocator_alloc(&n->pool, NULL, need);
    if (!new_arena) return 0;
    nbase = (uint8_t*)(((uintptr_t)new_arena+15u)&~(uintptr_t)15u);
    memset(&b,0,sizeof b); b.base=nbase; b.cap=need-(size_t)(nbase-(uint8_t*)new_arena);
    i_rant_node_layout(&b, new_max_peers, new_max_topics, &tc, &dc, &nb);

    /* migrate the three cores. Each leaves the old intact, so a failure frees the new
       arena and the old node keeps running, only refusing the growth */
    nt = rant_transport_migrate(n->transport, nb.transport, nb.transport_bytes, new_max_peers, new_max_topics);
    if (!nt){ rant_allocator_alloc(&n->pool, new_arena, 0); return 0; }
    ncore = i_rant_node_core_migrate(n->core, nb.node_core, nb.node_core_bytes, new_max_topics);
    if (!ncore){ rant_allocator_alloc(&n->pool, new_arena, 0); return 0; }
    ncore->transport = nt;                         /* re point the cross layer pointer */
    i_rant_node_core_build_meta(ncore);                /* rebuild the blob in the new buffer */
    ndisc = rant_discovery_migrate(n->discovery, nb.discovery, nb.discovery_bytes,
                                      new_max_peers, new_meta_cap, i_rant_node_core_meta(ncore).data, ncore);
    if (!ndisc){ rant_allocator_alloc(&n->pool, new_arena, 0); return 0; }
    i_rant_node_core_bind_discovery(ncore, rant_discovery_state(ndisc));       /* the relocated table */

    /* the handle pointer array. The handle structs are stable and do not move */
    memcpy(nb.handles, n->handles, (size_t)old_max_topics*sizeof(RantTopic*));
    memset((RantTopic**)nb.handles + old_max_topics, 0,
           (size_t)(new_max_topics-old_max_topics)*sizeof(RantTopic*));

#ifdef RANT_SHM
    if (n->shm_capable){
        uint32_t old_segs=(uint32_t)n->shm_n_topics*RANT_SHM_N_CLASSES;
        uint32_t new_segs=(uint32_t)new_max_topics*RANT_SHM_N_CLASSES, i;
        void **np = (void**)nb.shm_pool;
        for (i=0;i<new_segs;i++) np[i]=NULL;
        for (i=0;i<old_segs;i++) np[i]=n->shm_pool[i];   /* only the table moves */
        n->shm_pool=np; n->shm_n_topics=new_max_topics;
        /* the reader attach cache is hook allocated too, so attachments survive */
    }
#endif

    n->transport=nt; n->core=ncore; n->discovery=ndisc;
    n->handles=(RantTopic**)nb.handles;
    n->rx_buf=nb.rx_buf; n->rx_buf_bytes=nb.rx_buf_bytes;
    n->max_topics=new_max_topics; n->max_peers=new_max_peers;
    n->meta_cap=new_meta_cap;
    rant_allocator_alloc(&n->pool, old_arena, 0);      /* control structs only */
    n->arena=new_arena;
    return 1;
}

/* the local channel table behind RANT_SELF reflection: every live handle as a peer sees it */
static void i_rant_node_reflect_self(RantNode *n){
    uint16_t i;
    i_rant_node_core_self_begin(n->core);
    for (i = 0; i < n->max_topics; i++){
        RantTopic *h = n->handles[i];
        const RantQos *q;
        if (!h) continue;
        q = rant_transport_topic_qos(n->transport, i);
        i_rant_node_core_self_channel(n->core, i, rant_string(h->name, h->name_len), h->kind, h->role,
                                      (uint8_t)(q ? q->reliability : 0),
                                      rant_transport_topic_attrs(n->transport, i), h->schema);
    }
    i_rant_node_core_self_end(n->core);
}

/* Our own interest changed: re advertise, replay known peers' interest against the new
 * state, stale the converged memos and kick so the announce goes out now. Lock held. */
static void i_rant_node_readvertise(RantNode *n){
    i_rant_node_reflect_self(n);
    i_rant_node_core_build_meta(n->core);
    rant_discovery_advertise(n->discovery, i_rant_node_core_meta(n->core));
    rant_discovery_replay(n->discovery);
    n->match_epoch++;
    i_rant_node_kick(n);
}

/* The shared topic create: the public create and the patterns layer's both funnel here.
 * kind, prefix_bytes and directed stamp the entity, allow_at permits the reserved '@'. */
/* Records why a create refused: the last error slot, and the RANT_ERROR event when the
 * lock is held. From a callback only the slot is written, since an event must not nest
 * inside the handler that is running. */
static void i_rant_node_create_fail(RantNode *n, RantErrorKind err, const char *name,
                                    uint64_t need, int emit){
    RantEvent e; memset(&e, 0, sizeof e);
    e.kind = RANT_ERROR; e.error = err; e.topic_name = name; e.too_big_bytes = need;
    if (err == RANT_E_NAME_COLLISION) e.identity = rant_topic_id(name);
    if (emit){ i_rant_node_emit(n, &e); return; }
    e.user = n->user_data;
    n->last_error = e;
    i_rant_node_keep_str(n->last_error_topic, sizeof n->last_error_topic, name, &n->last_error.topic_name);
    n->last_error.peer_name = NULL; n->last_error.schema_detail = NULL;
}

static RantTopic *i_rant_node_create_impl(RantNode *n, const char *name, RantRole role,
                              const RantSchema *schema, const RantTopicOpts *opts,
                              uint8_t kind, uint8_t prefix_bytes, uint8_t directed, uint8_t attrs,
                              i_RantSysMsgFn sys_msg, void *sys_user, int allow_at){
    RantTopicDef def; RantTopic *h; uint16_t idx; int acquired;
    int reuse = 0;
    if (!n || !name) return NULL;
    acquired = i_rant_node_lock(n);
    if (!acquired){   /* from a callback: a grow would move the arena mid delivery */
        i_rant_node_create_fail(n, RANT_E_STATE, name, 0, 0);
        return NULL;
    }
    if (!name[0] || strlen(name) > RANT_TOPIC_NAME_MAX){
        i_rant_node_create_fail(n, RANT_E_BAD_NAME, name, 0, 1);
        i_rant_node_unlock(n, acquired); return NULL;
    }
    if (!allow_at){   /* '@' is reserved for pattern channels */
        const char *s = name;
        while (*s){
            if (*s=='@'){
                i_rant_node_create_fail(n, RANT_E_BAD_NAME, name, 0, 1);
                i_rant_node_unlock(n, acquired); return NULL;
            }
            s++;
        }
    }
    if (n->creating_builtin){
        /* open time builtins fill their block. A full block degrades to no builtin, never a grow */
        idx = (uint16_t)(n->builtin_lo + n->n_builtin);
        if (idx >= n->max_topics){
            i_rant_node_create_fail(n, RANT_E_STATE, name, 0, 1);
            i_rant_node_unlock(n, acquired); return NULL;
        }
    } else if ((reuse = rant_transport_topic_reuse_find(n->transport, name, kind, &idx)) != 0){
        /* a retired slot takes this create so churn never grows the table. Same identity,
           kind and schema (find = 2) relinks silently, anything else rebinds. See spec/interest.md */
    } else {
        idx = n->n_created;
        if (n->n_builtin && idx >= n->builtin_lo)
            idx = (uint16_t)(idx + n->n_builtin);   /* step over the builtin block */
        if (idx >= n->max_topics){       /* the reserve is full: grow or refuse (static) */
            uint16_t want = n->max_topics < 0x8000u ? (uint16_t)(n->max_topics*2u) : 0xFFFFu;
            if (want <= n->max_topics || !i_rant_node_grow(n, n->max_peers, want, 0)){
                i_rant_node_create_fail(n, RANT_E_STATE, name, 0, 1);
                i_rant_node_unlock(n, acquired);
                return NULL;
            }
        }
    }
    h = (RantTopic*)i_rant_node_alloc(n, NULL, sizeof *h);       /* stable: outlives any arena grow */
    if (!h){
        i_rant_node_create_fail(n, RANT_E_OOM, name, sizeof *h, 1);
        i_rant_node_unlock(n, acquired); return NULL;
    }
    memset(h, 0, sizeof *h);
    if (opts) h->qos = opts->qos;
    if (opts && opts->reflect_from_mesh && kind == RANT_KIND_TOPIC){
        /* fill what the caller left unspecified from the mesh: a reader takes the
           provider's schema, a writer the widest every reader accepts */
        const RantSchema *ms = NULL; uint8_t rel = 0;
        h->reflect = 1;
        i_rant_node_core_reflect_pick(n->core, RANT_ENTITY_TOPIC, name, 0,
                                      rant_role_pubs((uint8_t)role), &ms, &rel, &h->generation);
        if (!schema) schema = ms;
        if (!h->qos.reliability) h->qos.reliability = rel ? RANT_RELIABLE : RANT_BEST_EFFORT;
    }
    if (opts && opts->queue){
        if (opts->queue->n != n){          /* another node's queue */
            i_rant_node_create_fail(n, RANT_E_STATE, name, 0, 1);
            i_rant_node_alloc(n, h, 0); i_rant_node_unlock(n, acquired); return NULL;
        }
        h->queue = opts->queue;
        if (!i_rant_node_queue_ensure(n, h, &h->qos)){
            i_rant_node_create_fail(n, RANT_E_OOM, name, 4096, 1);
            i_rant_node_alloc(n, h, 0); i_rant_node_unlock(n, acquired); return NULL;
        }
        if (kind != RANT_KIND_TOPIC) h->q->reliable = 0;   /* state applied at receipt: never park the lane */
    }
    if (schema){   /* copied into node memory so the caller's schema need not outlive the topic */
        RantBytes w = rant_schema_wire(schema);
        h->schema = rant_schema_parse(w.data, w.len, i_rant_node_alloc, n);
        if (!h->schema){
            i_rant_node_create_fail(n, RANT_E_BAD_SCHEMA, name, 0, 1);
            i_rant_node_queue_drop(n, h);
            i_rant_node_alloc(n, h, 0); i_rant_node_unlock(n, acquired); return NULL;
        }
    }
    memset(&def, 0, sizeof def);
    def.name = name; def.role = (uint8_t)role;
    def.kind = kind; def.prefix_bytes = prefix_bytes; def.directed = directed; def.attrs = attrs;
    def.qos = h->qos;
    h->attrs = attrs; h->directed = directed;
    {   int rc;
        if (reuse){
            /* an identical binding rebinds silently, anything else bumps the slot's
               generation and publishes the rebind under the version the advertise stamps */
            uint64_t new_hash = h->schema ? rant_schema_hash(h->schema) : 0;
            int changed = (reuse != 2)
                       || (new_hash != i_rant_node_core_topic_schema_hash(n->core, idx));
            uint32_t rb = changed
                ? rant_discovery_meta_version(rant_discovery_state(n->discovery)) + 1u : 0u;
            rc = rant_transport_topic_reuse(n->transport, idx, &def, changed, rb);
            if (rc == 0 && changed) i_rant_node_core_topic_rebound(n->core, idx);
        } else {
            rc = rant_transport_topic_define(n->transport, idx, &def);
        }
        if (rc != 0){
            /* the transport's verdict: -2 a live same name slot, -3 the name, -4 OOM */
            i_rant_node_create_fail(n, rc == -2 ? RANT_E_NAME_COLLISION : rc == -3 ? RANT_E_BAD_NAME
                                     : rc == -4 ? RANT_E_OOM : RANT_E_STATE, name, 0, 1);
            if (h->schema) rant_schema_free(h->schema, i_rant_node_alloc, n);
            i_rant_node_queue_drop(n, h);
            i_rant_node_alloc(n, h, 0);
            i_rant_node_unlock(n, acquired);
            return NULL;
        }
    }
    h->n = n; h->index = idx; h->prefix_bytes = prefix_bytes; h->kind = kind;
    /* a response prefix is followed by [u8 len][message] on the wire, derived from the
       kind so every creator of the kind splits alike */
    h->prefix_string = (uint8_t)(kind == RANT_KIND_FUNC_RSP || kind == RANT_KIND_TASK_RSP);
    h->role = (uint8_t)role;
    h->sys_on_message = sys_msg; h->sys_msg_user = sys_user;
    {   /* the stable name copy, queued views must not point into the arena */
        size_t nl = strlen(name);
        if (nl > RANT_TOPIC_NAME_MAX) nl = RANT_TOPIC_NAME_MAX;
        memcpy(h->name, name, nl);
        h->name_len = (uint8_t)nl;
    }
    if (def.qos.queue_bytes && !h->queue)   /* queued from creation, best effort on OOM (take retries) */
        (void)i_rant_node_queue_ensure(n, h, &def.qos);
    if (h->schema || reuse)   /* advertise and gate with it. A reused slot must also clear
                                 the retired occupant's fingerprint when the new topic is untyped */
        i_rant_node_core_set_topic_schema(n->core, idx, h->schema);
    n->handles[idx] = h;
    if (n->creating_builtin) n->n_builtin++;
    else if (!reuse) n->n_created++;   /* a reused slot is already inside the dense region */
    /* the handle is installed before the publish: the replay fires peer events into the
       patterns layer and the app, which must never see a half created topic */
    i_rant_node_readvertise(n);
    i_rant_node_unlock(n, acquired);
    return h;
}

RantTopic *rant_node_create_topic(RantNode *n, const char *name, RantRole role,
                                      const RantSchema *schema, const RantTopicOpts *opts){
    return i_rant_node_create_impl(n, name, role, schema, opts, RANT_KIND_TOPIC, 0, 0, 0, NULL, NULL, 0);
}

RantTopic *i_rant_node_create_pattern_topic(RantNode *n, const char *name, RantRole role,
                              const RantSchema *schema, const RantTopicOpts *opts,
                              uint8_t kind, uint8_t prefix_bytes, uint8_t directed, uint8_t attrs,
                              i_RantSysMsgFn on_msg, void *on_msg_user){
    return i_rant_node_create_impl(n, name, role, schema, opts, kind, prefix_bytes, directed, attrs,
                                   on_msg, on_msg_user, 1);
}

/* the built in log topics */

static int i_rant_node_do_send(RantNode *n, uint16_t topic_index, RantBytes data,
                               uint64_t capture_us, int may_wait);

static const char *const i_rant_log_topic_names[3] =
    { "@rant/log/error", "@rant/log/warn", "@rant/log/info" };

/* Creates the three log topics at open: reliable, keep_last = catch_up as the replayable
 * history, publish only, and no backpressure wait. NULL handles on OOM, never a failed open. */
static void i_rant_node_logs_open(RantNode *n){
    RantTopicOpts topt; int lvl;
    n->log_schema = rant_schema_compile(i_rant_node_alloc, n,
        "RantLog { wall_us: u64, mono_us: u64, text: string }", NULL);
    if (!n->log_schema) return;
    for (lvl = 0; lvl < 3; lvl++){
        memset(&topt, 0, sizeof topt);
        topt.qos.reliability = RANT_RELIABLE;
        topt.qos.keep_last = topt.qos.catch_up = (lvl == RANT_LOG_INFO) ? 8 : 16;
        /* backpressure_wait_us stays 0, so logs never block */
        n->log_topics[lvl] = i_rant_node_create_impl(n, i_rant_log_topic_names[lvl],
                                 RANT_PUB_ONLY, n->log_schema, &topt,
                                 RANT_KIND_TOPIC, 0, 0, 0, NULL, NULL, 1);
    }
}

/* Builds one RantLog message and publishes it on the level's topic. locked = 1 is the
 * mirror flush under the node lock, 0 the public thread safe path. */
static int i_rant_node_log_publish(RantNode *n, RantLogLevel level, const char *text,
                                   size_t text_len, uint64_t wall_us, uint64_t mono_us,
                                   int locked){
    uint8_t msg[20u + RANT_LOG_MAX];     /* the fixed section (16) and the text frame header (4) */
    uint32_t len;
    RantTopic *h = n->log_topics[level];
    if (!h || !n->log_schema) return RANT_ERR_NOSYS;
    if (!rant_schema_message_default(n->log_schema, msg, sizeof msg)) return RANT_ERR_OOM;
    rant_set_uint(msg, sizeof msg, n->log_schema, "wall_us", wall_us);
    rant_set_uint(msg, sizeof msg, n->log_schema, "mono_us", mono_us);
    if (!rant_set_string(msg, sizeof msg, n->log_schema, "text", rant_string(text, text_len)))
        return RANT_ERR_TOO_BIG;
    len = rant_schema_msg_len(n->log_schema, msg, sizeof msg);
    if (locked) return i_rant_node_do_send(n, h->index, rant_bytes(msg, len), 0, 0);
    return rant_topic_send(h, rant_bytes(msg, len), NULL);
}

int rant_node_log(RantNode *n, RantLogLevel level, const char *fmt, ...){
    char text[RANT_LOG_MAX]; int tn;
    va_list ap;
    if (!n || (int)level < 0 || level > RANT_LOG_INFO || !fmt) return RANT_ERR_NO_TOPIC;
    if (!n->log_topics[level]) return RANT_ERR_NOSYS;
    va_start(ap, fmt);
    tn = vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (tn < 0) tn = 0;                                       /* an encoding error: an empty line */
    if (tn >= (int)sizeof text) tn = (int)sizeof text - 1;    /* truncated at RANT_LOG_MAX */
    return i_rant_node_log_publish(n, level, text, (size_t)tn,
                                   i_rant_plat_wall_us(), i_rant_plat_now_us(), 0);
}

int rant_node_log_text(RantNode *n, RantLogLevel level, const char *text, int len){
    size_t tl;
    if (!n || (int)level < 0 || level > RANT_LOG_INFO || !text) return RANT_ERR_NO_TOPIC;
    if (!n->log_topics[level]) return RANT_ERR_NOSYS;
    tl = len < 0 ? strlen(text) : (size_t)len;
    if (tl >= RANT_LOG_MAX) tl = RANT_LOG_MAX - 1;       /* match the variadic path's truncation */
    return i_rant_node_log_publish(n, level, text, tl,
                                   i_rant_plat_wall_us(), i_rant_plat_now_us(), 0);
}

RantTopic *rant_node_log_topic(RantNode *n, RantLogLevel level){
    if (!n || (int)level < 0 || level > RANT_LOG_INFO) return NULL;
    return n->log_topics[level];
}

/* Publishes the pending error mirror ring under the lock. A coalesced burst appends its
 * count and overflow between flushes becomes one summary line. */
static void i_rant_node_log_flush(RantNode *n){
    uint8_t i, cnt;
    if (!n->log_topics[RANT_LOG_ERROR]){ n->log_pend_n = 0; n->log_pend_dropped = 0; return; }
    n->log_flushing = 1;
    cnt = n->log_pend_n; n->log_pend_n = 0;
    for (i = 0; i < cnt; i++){
        i_RantLogPend *p = &n->log_pend[i];
        char line[sizeof p->text + 16]; int ln;
        ln = (p->count > 1) ? snprintf(line, sizeof line, "%s (x%u)", p->text, (unsigned)p->count)
                            : snprintf(line, sizeof line, "%s", p->text);
        if (ln < 0) ln = 0;
        if (ln >= (int)sizeof line) ln = (int)sizeof line - 1;
        i_rant_node_log_publish(n, RANT_LOG_ERROR, line, (size_t)ln, p->wall_us, p->mono_us, 1);
    }
    if (n->log_pend_dropped){
        char line[64];
        int ln = snprintf(line, sizeof line, "log mirror overflow: %u error events dropped",
                          (unsigned)n->log_pend_dropped);
        n->log_pend_dropped = 0;
        i_rant_node_log_publish(n, RANT_LOG_ERROR, line, (size_t)(ln < 0 ? 0 : ln),
                                i_rant_plat_wall_us(), i_rant_plat_now_us(), 1);
    }
    n->log_flushing = 0;
}

/* the longest one poll tick drains RX before yielding to discovery and send */
#ifndef RANT_RX_BUDGET_US
#define RANT_RX_BUDGET_US 5000u
#endif

/* Drains the socket into the transport until empty or past the deadline. A full drain
 * avoids NACK storms. Distinct from the public rant_topic_drain. */
static void i_rant_node_rx_drain(RantNode *n, i_RantSock fd, uint64_t deadline){
    uint8_t *buf = n->rx_buf;
    for (;;){
        uint8_t src_ip[4]; uint16_t src_port;
        int r = i_rant_plat_recv(fd, buf, n->rx_buf_bytes, src_ip, &src_port);
        if (r<0){
            if (i_rant_plat_would_block()) break;          /* the queue is empty */
            {   /* a hard error: report and stop this tick, never spin on a wedged socket */
                RantEvent e; memset(&e, 0, sizeof e);
                e.kind = RANT_ERROR; e.error = RANT_E_RECV; e.os_error = i_rant_plat_last_socket_error();
                i_rant_node_emit(n, &e);
            }
            break;
        }
        if (r>0){
            if (r>=4 && buf[0]=='u' && buf[1]=='D' && buf[2]=='S' && buf[3]=='C'){
                /* a unicast announce aimed at our data port goes to discovery with its
                   source port, so a translated peer's observed source can bind */
                RantDiscoveryAddr src;
                memset(&src, 0, sizeof src);
                memcpy(src.ip, src_ip, 4); src.ip_len = 4; src.port = src_port;
                rant_discovery_feed(n->discovery, &src, rant_bytes(buf, (size_t)r));
            } else if (r>=5 && buf[0]=='u' && buf[1]=='D' && buf[2]=='T' && buf[3]=='L'){
                /* the pairwise detail exchange, stateless: a request is answered to its
                   source, a response feeds the pending match cycle. See spec/interest.md */
                if (buf[4]==RANT_DETAIL_REQ){
                    RantBytes resp = i_rant_node_core_detail_respond(n->core, n->domain,
                                                                     rant_bytes(buf, (size_t)r));
                    if (resp.len) i_rant_plat_send(fd, resp.data, resp.len, src_ip, src_port);
                    {   /* the request names the version the peer applied: feed the rebind hold */
                        uint32_t from;
                        if (i_rant_node_core_id_for_addr(n->core, src_ip, src_port, &from)
                            && i_rant_node_core_seen_version(n->core, from,
                                   rant_detail_meta_version(rant_bytes(buf, (size_t)r))))
                            n->match_epoch++;
                    }
                } else if (buf[4]==RANT_DETAIL_RESP){
                    uint32_t from;
                    if (i_rant_node_core_id_for_addr(n->core, src_ip, src_port, &from))
                        i_rant_node_core_apply_details(n->core, n->domain, from,
                                                       rant_bytes(buf, (size_t)r));
                } else if (buf[4]==RANT_INTEREST_REQ){
                    /* interest paging: serve one page. Not a rebind hold confirmation, the
                       peer is still fetching that version */
                    RantBytes resp = i_rant_node_core_interest_respond(n->core, n->domain,
                                                                       rant_bytes(buf, (size_t)r));
                    if (resp.len) i_rant_plat_send(fd, resp.data, resp.len, src_ip, src_port);
                } else if (buf[4]==RANT_INTEREST_RESP){
                    uint32_t from;
                    if (i_rant_node_core_id_for_addr(n->core, src_ip, src_port, &from))
                        i_rant_node_core_apply_interest_page(n->core, n->domain, from,
                                                             rant_bytes(buf, (size_t)r));
                }
            } else {
                uint32_t from;
                if (i_rant_node_core_id_for_addr(n->core, src_ip, src_port, &from))
                    rant_transport_on_datagram(n->transport, from, rant_bytes(buf, (size_t)r), i_rant_plat_now_us());
            }
        }
        if (i_rant_plat_now_us() >= deadline) break;        /* yield to discovery and send */
    }
}

/* Caps a wait at the transport's next timer, discovery's next announce and the patterns
 * tick, so each fires on time with no traffic. Lock held, pure compute. */
static int i_rant_node_wait_ms(RantNode *n, int timeout_ms){
    uint64_t next = rant_transport_next_deadline_us(n->transport);
    uint64_t due  = rant_discovery_next_due_us(rant_discovery_state(n->discovery));
    if (timeout_ms < 0) timeout_ms = 0;
    if (!next || due < next) next = due;   /* due is a real time (0 = now), next 0 = none */
    /* the patterns tick */
    if (n->sys_tick_next && (!next || n->sys_tick_next < next)) next = n->sys_tick_next;
    if (next){
        uint64_t t0 = i_rant_plat_now_us();
        uint64_t us = (next > t0) ? next - t0 : 0;
        int ms = (us >= (uint64_t)timeout_ms*1000u) ? timeout_ms   /* rounded up: no busy spin */
                                                    : (int)((us + 999u)/1000u);
        if (ms < timeout_ms) timeout_ms = ms;
    } else timeout_ms = 0;                 /* something is due right now */
    return timeout_ms;
}

static int i_rant_node_gather_done(RantNode *n, uint64_t now);       /* defined with the match wait */

/* One poll tick under the node lock: deferred grows, the wait, discovery's tick, RX drain,
 * TX flush. The one poll body. outer = 1 lets the wait drop the lock. See spec/node.md. */
static void i_rant_node_poll_locked(RantNode *n, int timeout_ms, int outer){
    uint8_t buf[RANT_DGRAM_MAX]; uint64_t now;
    i_RantPollfd pfd[5]; int nfds = 0, wait_ms, poll_rc;
    int disc_slot = 0, disc_n = 0;
#ifdef RANT_THREADS
    int waker_slot = -1;
#endif

    /* a peer was refused last tick for lack of slots: grow now, between ticks */
    if (n->grow_pending){
        uint16_t want = n->max_peers < 0x8000u ? (uint16_t)(n->max_peers*2u) : 0xFFFFu;
        n->grow_pending = 0;
        if (want > n->max_peers) i_rant_node_grow(n, want, n->max_topics, 0);
    }
    /* a peer's blob exceeded the accept bound last tick: raise it, then solicit so the
       peer re sends into buffers that fit. A failed grow is remembered, see spec/node.md */
    if (n->meta_grow_need){
        uint16_t want = n->meta_grow_need;
        n->meta_grow_need = 0;
        if (want > n->meta_cap){
            if (i_rant_node_grow(n, n->max_peers, n->max_topics, want)){
                n->meta_grow_failed = 0;
                rant_discovery_solicit(rant_discovery_state(n->discovery));
            } else
                n->meta_grow_failed = want;
        }
    }

    /* the discovery tick runs after the wait off its revents, so a pass costs one syscall */
    wait_ms = i_rant_node_wait_ms(n, timeout_ms);
    memset(pfd, 0, sizeof pfd);
    pfd[nfds].fd = n->fd; pfd[nfds].events = RANT_POLLIN; nfds++;
#ifdef RANT_THREADS
    if (n->waker.fd != RANT_SOCK_BAD){
        waker_slot = nfds;
        pfd[nfds].fd = n->waker.fd; pfd[nfds].events = RANT_POLLIN; nfds++;
    }
#endif
    {   /* discovery's sockets join the wait so an announce cuts a long sleep short. The
           fds are stable by value: a grow relocates structs, never sockets */
        i_RantSock dfds[2]; int i;
        disc_slot = nfds; disc_n = rant_discovery_pollfds(n->discovery, dfds);
        for (i = 0; i < disc_n; i++){ pfd[nfds].fd = dfds[i]; pfd[nfds].events = RANT_POLLIN; nfds++; }
    }

#ifdef RANT_THREADS
    if (outer){
        /* the wait runs unlocked so a sender on another thread is never blocked behind
           it. pollers_sleeping is a counter, several threads may poll the same node */
        n->pollers_sleeping++;
        i_rant_node_unlock_raw(n);
        poll_rc = i_rant_plat_poll(pfd, nfds, wait_ms);
        i_rant_node_lock_raw(n);
        n->pollers_sleeping--;
    } else {
        poll_rc = i_rant_plat_poll(pfd, nfds, wait_ms);
    }
    if (waker_slot >= 0 && (pfd[waker_slot].revents & RANT_POLLIN)){
        /* revents gated, so an idle pass costs no drain syscall. A kick still in flight
           wakes the next wait, which drains and clears it, so no kick is ever lost */
        i_rant_plat_waker_drain(&n->waker);
        n->wake_signaled = 0;
    }
#else
    (void)outer;
    poll_rc = i_rant_plat_poll(pfd, nfds, wait_ms);
#endif
    if (poll_rc < 0){   /* the wait itself failed, a real fault such as a bad fd */
        RantEvent e; memset(&e, 0, sizeof e);
        e.kind = RANT_ERROR; e.error = RANT_E_POLL; e.os_error = i_rant_plat_last_socket_error();
        i_rant_node_emit(n, &e);
    }

    /* the discovery tick off this wait's readiness. A failed wait leaves revents zeroed,
       so the clock driven work still runs */
    rant_discovery_service(n->discovery,
                           disc_n > 0 && (pfd[disc_slot].revents & RANT_POLLIN) != 0,
                           disc_n > 1 && (pfd[disc_slot + 1].revents & RANT_POLLIN) != 0);
    if (pfd[0].revents & RANT_POLLIN)
        i_rant_node_rx_drain(n, n->fd, i_rant_plat_now_us() + RANT_RX_BUDGET_US);

    /* mirrored errors publish now, before the TX pull, so the lines ride this pass */
    if (n->log_pend_n || n->log_pend_dropped) i_rant_node_log_flush(n);

    now=i_rant_plat_now_us();
    /* the detail requester: drain queued requests and rearm every active peer once per
       announce interval while any went out. A sweep that sends nothing disarms the timer */
    if (i_rant_node_core_detail_any(n->core) || (n->next_detail_us && now >= n->next_detail_us)){
        i_RantNodeDest dst; size_t len; int sent = 0;
        if (n->next_detail_us && now >= n->next_detail_us)
            i_rant_node_core_detail_rearm(n->core);
        while ((len = i_rant_node_core_detail_req_next(n->core, n->domain, buf, sizeof buf, &dst)) != 0){
            i_rant_plat_send(n->fd, buf, len, dst.ip, dst.port);     /* best effort: retries heal */
            sent = 1;
        }
        n->next_detail_us = sent ? now + n->announce_us : 0;
    }
    i_rant_node_tx_drain(n);
    now=i_rant_plat_now_us();

    /* latch the post open gather the moment it settles, so a later create's replay events
       cannot reset the quiet clock and make a first send re wait */
    if (!n->gather_done) (void)i_rant_node_gather_done(n, now);

    /* the patterns tick runs under the lock like a callback and returns the next deadline */
    if (n->sys_tick) n->sys_tick_next = n->sys_tick(n->sys_user, i_rant_plat_now_us());

#ifdef RANT_THREADS
    if (n->cv_waiters) i_rant_plat_cond_broadcast(&n->cv);     /* acks or TX may have progressed */
#endif
}

int rant_node_poll(RantNode *n, int timeout_ms){
    int acquired;
    if (!n) return RANT_ERR_STATE;
    acquired = i_rant_node_lock(n);
#ifdef RANT_THREADS
    if (!acquired || n->svc_running){
        /* from inside a callback, or a service thread owns the loop: refuse loudly */
        i_rant_node_unlock(n, acquired);
        return RANT_ERR_STATE;
    }
#endif
    i_rant_node_poll_locked(n, timeout_ms, acquired);
    i_rant_node_unlock(n, acquired);
    return 0;
}

/* The one blocking wait skeleton: sample the clock, ask done (which states the deadline),
 * run periodic, then sleep on the condvar or pump. See spec/node.md. */
typedef struct {
    /* nonzero = the wait is over, *deadline is this iteration's bound, UINT64_MAX = none */
    int    (*done)(RantNode *n, void *ctx, uint64_t now, uint64_t *deadline);
    void   (*periodic)(RantNode *n, void *ctx, uint64_t now);     /* optional, NULL = nothing */
    void    *ctx;
    uint64_t cv_cap_us;      /* the longest single sleep, 0 = the whole remaining time */
    int      pump_ms;        /* the pump tick in ms, 0 = the remaining time */
    int      pump_outer;     /* poll_locked's outer flag: 1 lets the pump's wait drop the lock */
    int      pump_after_svc; /* 1 = a service thread stopping under us finishes in the pump,
                                0 = the wait ends there with RANT__WAIT_SVC_GONE */
} i_RantWait;

/* what ended the wait: the predicate, the deadline, or the service thread going away */
#define RANT__WAIT_DONE       1
#define RANT__WAIT_TIMEOUT    0
#define RANT__WAIT_SVC_GONE (-1)

static int i_rant_node_wait_until(RantNode *n, const i_RantWait *w){
    for (;;){
        uint64_t deadline = 0, left, now = i_rant_plat_now_us();
        if (w->done(n, w->ctx, now, &deadline)) return RANT__WAIT_DONE;
        if (now >= deadline) return RANT__WAIT_TIMEOUT;
        if (w->periodic) w->periodic(n, w->ctx, now);
        left = deadline - now;
        if (w->cv_cap_us && left > w->cv_cap_us) left = w->cv_cap_us;
#ifdef RANT_THREADS
        if (n->svc_running){
            n->cv_waiters++;
            i_rant_node_cv_wait(n, left);     /* mu drops: nothing cached survives */
            n->cv_waiters--;
            if (!n->svc_running && !w->pump_after_svc) return RANT__WAIT_SVC_GONE;
            continue;                       /* stopped under us: the next pass pumps */
        }
#endif
        {   int ms = w->pump_ms;
            if (!ms){                       /* derive the tick from the time left */
                uint64_t left_ms = left / 1000u;
                ms = left < 1000u ? 1 : (left_ms > 0x7FFFFFFFu ? 0x7FFFFFFF : (int)left_ms);
            }
            i_rant_node_poll_locked(n, ms, w->pump_outer);
        }
    }
}

/* The kick a wait owes before it sleeps: only a service thread needs telling, a pump is
 * the poller and services the change on its own next tick. */
static void i_rant_node_wait_kick(RantNode *n, void *ctx, uint64_t now){
    (void)n; (void)ctx; (void)now;
#ifdef RANT_THREADS
    if (n->svc_running) i_rant_node_kick(n);
#endif
}

/* the send path match wait, see spec/interest.md */

static int i_rant_node_settled(RantNode *n, uint64_t start, uint64_t now);       /* with settle */

/* Has the post open gather completed: the settle predicate anchored at open, latched
 * once true. */
static int i_rant_node_gather_done(RantNode *n, uint64_t now){
    if (n->gather_done) return 1;
    if (!i_rant_node_settled(n, n->open_us, now)) return 0;
    n->gather_done = 1;
    return 1;
}

/* Would a send now race a forming match: 1 while the gather is unsettled or verdicts are
 * in flight, else 0, memoized against the topology epoch. Lock held. */
static int i_rant_node_topic_unsettled(RantNode *n, RantTopic *h, uint64_t now){
    if (h->resolve_epoch == n->match_epoch) return 0;   /* converged at this topology */
    if (!i_rant_node_gather_done(n, now)) return 1;
    if (i_rant_node_core_topic_unresolved(n->core, h->index) > 0) return 1;
    h->resolve_epoch = n->match_epoch;
    return 0;
}

/* The match wait's predicate: done once a subscriber matched, or once matching converged
 * with none. The match count is re read from the transport every call. */
typedef struct {
    RantTopic *h;
    uint16_t   index;
    uint64_t   deadline;
    int        matched;   /* the count the wait ended on */
} i_RantMatchWait;

static int i_rant_node_match_wait_done(RantNode *n, void *ctx, uint64_t now, uint64_t *deadline){
    i_RantMatchWait *c = (i_RantMatchWait*)ctx;
    *deadline = c->deadline;
    c->matched = rant_transport_publisher_match_count(n->transport, c->index);
    if (c->matched) return 1;
    return !i_rant_node_topic_unsettled(n, c->h, now);
}

/* Bounded wait for a forming match before a zero subscriber send commits. Returns the
 * matched count. loud fires RANT_E_UNMATCHED_SEND on timeout, a pattern write passes 0. */
static int i_rant_node_match_wait(RantNode *n, uint16_t topic_index, RantTopic *h, int loud){
    i_RantMatchWait c; i_RantWait w = { 0 };
    c.h = h; c.index = topic_index; c.matched = 0;
    c.deadline = i_rant_plat_now_us() + n->match_wait_us;
    w.done = i_rant_node_match_wait_done; w.ctx = &c;
    w.cv_cap_us = 50000u;   /* re check the clock: gather settling is partly time driven */
    w.pump_ms   = 1;        /* a nested tick: the lock stays held */
    if (i_rant_node_wait_until(n, &w) == RANT__WAIT_TIMEOUT && loud){
        RantEvent e; memset(&e, 0, sizeof e);
        e.kind = RANT_ERROR; e.error = RANT_E_UNMATCHED_SEND;
        e.topic = topic_index; e.topic_name = i_rant_node_topic_name(n, topic_index);
        i_rant_node_emit(n, &e);
    }
    return c.matched;
}

/* The backpressure wait's predicate: done once the send no longer evicts unacked history.
 * It also samples the in pump probe on a timer, after a tick and before the re check. */
typedef struct {
    uint16_t index;
    uint64_t deadline;
    uint64_t t0, sample_last, interval;
    uint32_t polls, polls_idle;
    int      polled;
    RantRepairStats prev;
} i_RantPumpWait;

static int i_rant_node_pump_wait_done(RantNode *n, void *ctx, uint64_t now, uint64_t *deadline){
    i_RantPumpWait *c = (i_RantPumpWait*)ctx;
    *deadline = c->deadline;
    if (n->pump_probe && c->polled){
        c->polls++;
        if (rant_transport_repair_pending(n->transport, c->index) == 0) c->polls_idle++;
        if (now - c->sample_last >= c->interval){
            RantRepairStats sample_now; RantPumpSample sample;
            rant_transport_repair_stats(n->transport, c->index, &sample_now);
            sample.topic           = c->index;
            sample.wait_elapsed_us = now - c->t0;
            sample.interval_us     = now - c->sample_last;
            sample.frags_resent    = sample_now.frags_resent - c->prev.frags_resent;
            sample.nacks_recv      = sample_now.nacks_recv  - c->prev.nacks_recv;
            sample.polls           = c->polls;
            sample.polls_idle      = c->polls_idle;
            n->pump_probe(n->pump_probe_user, &sample);
            c->prev = sample_now; c->sample_last = now; c->polls = 0; c->polls_idle = 0;
        }
    }
    return !rant_transport_send_would_evict(n->transport, c->index);
}

static void i_rant_node_pump_wait_tick(RantNode *n, void *ctx, uint64_t now){
    (void)n; (void)now;
    ((i_RantPumpWait*)ctx)->polled = 1;     /* a tick runs next: sample on the call after it */
}

/* Publishes on a topic index: the match wait, the flow control wait, then SHM or UDP.
 * may_wait = 0 is a reentrant send from a callback: it never blocks or runs the loop. */
static int i_rant_node_do_send_ex(RantNode *n, uint16_t topic_index, RantBytes hdr, RantBytes data,
                                  uint64_t capture_us, int directed, uint32_t to_peer,
                                  int may_wait){
    /* one O(1) count gates the per send fast paths: an unsubscribed topic skips them all */
    int matched;
    {   /* a typed plain topic refuses a payload its schema cannot read, so the fault lands
           on the sender instead of as a schema mismatch at every reader */
        RantTopic *th = topic_index < n->max_topics ? n->handles[topic_index] : NULL;
        if (th && th->schema && th->kind == RANT_KIND_TOPIC && !rant_schema_validate(th->schema, data))
            return RANT_ERR_SCHEMA;
    }
    matched = rant_transport_publisher_match_count(n->transport, topic_index);
    const RantQos *q = rant_transport_topic_qos(n->transport, topic_index);
    /* the source stamp is ordinary payload, so every size rule here counts it. qos is
       immutable, so this stays valid across the waits that may re fetch q */
    size_t ts_bytes = (q && q->no_timestamp) ? 0u : (size_t)RANT_TIMESTAMP_BYTES;
    size_t cap_bytes = (ts_bytes && capture_us) ? (size_t)RANT_CAPTURE_BYTES : 0u;
    size_t len = ts_bytes + cap_bytes + hdr.len + data.len;
    int guarded = 0;   /* the unsent eviction check runs after the wait */

    /* a send to zero subscribers while a match forms waits for it to converge. A topic
       that retains history is exempt, since its history replays. See spec/interest.md */
    if (!matched && topic_index < n->max_topics
        && !(q && q->reliability == RANT_RELIABLE && q->catch_up > 0)){
        RantTopic *h = n->handles[topic_index];
        if (h && rant_role_pubs(h->role)
              && i_rant_node_topic_unsettled(n, h, i_rant_plat_now_us())){
            if (may_wait && n->match_wait_us){
                matched = i_rant_node_match_wait(n, topic_index, h, 1);
                q = rant_transport_topic_qos(n->transport, topic_index);     /* the arena may move */
            } else {
                RantEvent e; memset(&e, 0, sizeof e);
                e.kind = RANT_ERROR; e.error = RANT_E_UNMATCHED_SEND;
                e.topic = topic_index; e.topic_name = i_rant_node_topic_name(n, topic_index);
                i_rant_node_emit(n, &e);
            }
        }
    }

#ifdef RANT_THREADS
    if (matched && n->svc_running) guarded = 1;   /* sends transmit inline: unsent means a full socket */
#endif
    /* the bounded backpressure wait: until a slow reader acks or the wait elapses, then send
       anyway. It sleeps on the service thread's progress when one runs and pumps otherwise */
    if (matched && may_wait && q && q->backpressure_wait_us && rant_transport_send_would_evict(n->transport, topic_index)){
        i_RantPumpWait c = { 0 }; i_RantWait w = { 0 };
        c.index = topic_index;
        c.t0 = i_rant_plat_now_us();
        c.deadline = c.t0 + q->backpressure_wait_us;
        c.sample_last = c.t0;
        c.interval = n->pump_probe_interval_us ? n->pump_probe_interval_us : 200000u;
        if (n->pump_probe) rant_transport_repair_stats(n->transport, topic_index, &c.prev);
        w.done = i_rant_node_pump_wait_done; w.periodic = i_rant_node_pump_wait_tick;
        w.ctx = &c;
        w.pump_ms = 1;   /* a nested tick: the lock stays held */
        guarded = 1;
        i_rant_node_wait_until(n, &w);
        n->backpressure_total_us += i_rant_plat_now_us() - c.t0;
        n->backpressure_wait_count++;
        /* a mid pump grow relocates the arena: re derive the cached pointers */
        q = rant_transport_topic_qos(n->transport, topic_index);
        matched = rant_transport_publisher_match_count(n->transport, topic_index);
    }

    /* a send that evicts never sent history is surfaced. The state is read before the
       commit, but the event fires only if the send commits: a rejected send overwrote nothing */
    {
        uint64_t evict_base = 0; uint32_t evict_count = 0;
        int will_evict = guarded &&
            rant_transport_send_would_evict_unsent(n->transport, topic_index, &evict_base, &evict_count);
        int r;
#ifdef RANT_SHM
        /* only a message that would fragment gains from SHM, below the fragment size
           inline UDP is strictly cheaper. See spec/transport.md */
        if (!directed && n->shm_capable && len > rant_transport_frag(n->transport)
            && topic_index < n->shm_n_topics && matched
            && rant_transport_publisher_shm_eligible(n->transport, topic_index)){
            uint16_t keep_last = (q && q->keep_last) ? q->keep_last : 1u;
            /* a hint pins the topic to one class, else each message uses its own size
               class's segment. Per topic either way */
            uint32_t hint = q ? (q->shm_max_bytes ? q->shm_max_bytes : q->max_message_bytes) : 0u;
            uint32_t k = i_rant_shm_class_for(hint ? hint : (uint32_t)len);
            /* fits its class: publish via SHM. The chunk index is the history slot this
               send occupies, so chunk i binds slot i */
            if (k < RANT_SHM_N_CLASSES && (uint32_t)len <= i_rant_shm_class_bytes(k)){
                i_RantShmPool *pool = i_rant_node_shm_topic_pool(n, topic_index, k, keep_last);
                uint16_t slot = rant_transport_topic_hist_head(n->transport, topic_index);
                void *chunk_ptr = pool ? i_rant_shm_chunk(pool, slot, NULL) : NULL;
                if (chunk_ptr){
                    i_RantShmDesc d; uint8_t desc[RANT_SHM_DESC_WIRE];
                    /* gather the whole wire sample into the chunk: stamp, header, payload,
                       exactly as the inline commit writes it */
                    if (ts_bytes){
                        uint64_t w = i_rant_plat_wall_us() & RANT_STAMP_MASK;
                        if (cap_bytes) w |= RANT_STAMP_CAPTURE;
                        i_rant_le_w64((uint8_t*)chunk_ptr, w);
                        if (cap_bytes) i_rant_le_w64((uint8_t*)chunk_ptr + ts_bytes, capture_us);
                    }
                    if (hdr.len) memcpy((uint8_t*)chunk_ptr + ts_bytes + cap_bytes, hdr.data, hdr.len);
                    memcpy((uint8_t*)chunk_ptr + ts_bytes + cap_bytes + hdr.len, data.data, data.len);
                    i_rant_shm_stamp(pool, slot, (uint32_t)len, &d);
                    i_rant_shm_desc_encode(&d, desc);
                    if (rant_transport_send_shm(n->transport, topic_index, rant_bytes(chunk_ptr, len), desc, i_rant_plat_now_us())==0){
                        n->shm_tx++;
                        r = RANT_OK;
                        goto committed;
                    }
                }
            }
        }
#endif
        r = directed ? rant_transport_send_to(n->transport, topic_index, to_peer, hdr, data,
                                              capture_us, i_rant_plat_now_us())
                     : rant_transport_send_hdr(n->transport, topic_index, hdr, data,
                                               capture_us, i_rant_plat_now_us());
#ifdef RANT_SHM
committed:
#endif
        if (r == RANT_OK && topic_index < i_rant_node_topic_hi(n) && n->handles[topic_index]){
            n->handles[topic_index]->tx_msgs++;                      /* rant_topic_counts */
            n->handles[topic_index]->tx_bytes += len;
        }
        if (r == RANT_OK && will_evict){
            RantEvent e; memset(&e, 0, sizeof e);
            n->evicted_unsent++;
            e.kind = RANT_ERROR; e.error = RANT_E_EVICTED_UNSENT;
            e.topic = topic_index; e.topic_name = i_rant_node_topic_name(n, topic_index);
            e.lost_first = evict_base; e.lost_count = evict_count;
            i_rant_node_emit(n, &e);
        }
        return r;
    }
}

/* the plain broadcast send, the hot rant_topic_send path */
static int i_rant_node_do_send(RantNode *n, uint16_t topic_index, RantBytes data,
                               uint64_t capture_us, int may_wait){
    RantBytes nohdr; nohdr.data=NULL; nohdr.len=0;
    return i_rant_node_do_send_ex(n, topic_index, nohdr, data, capture_us, 0, 0, may_wait);
}

int rant_topic_send(RantTopic *topic, RantBytes data, const RantSendOpts *opts){
    int acquired, r;
    if (!topic) return RANT_ERR_NO_TOPIC;
    acquired = i_rant_node_lock(topic->n);
    r = i_rant_node_do_send(topic->n, topic->index, data,
                            opts ? opts->capture_us : 0u, acquired);
    i_rant_node_send_tx(topic->n, acquired);
    i_rant_node_unlock(topic->n, acquired);
    return r;
}

int i_rant_topic_send_hdr(RantTopic *topic, RantBytes hdr, RantBytes data){
    int acquired, r;
    if (!topic) return RANT_ERR_NO_TOPIC;
    acquired = i_rant_node_lock(topic->n);
    r = i_rant_node_do_send_ex(topic->n, topic->index, hdr, data, 0, 0, 0, acquired);
    i_rant_node_send_tx(topic->n, acquired);
    i_rant_node_unlock(topic->n, acquired);
    return r;
}

uint64_t i_rant_topic_seqno(RantTopic *topic){
    return topic ? rant_transport_topic_seqno(topic->n->transport, topic->index) : 0;
}

void i_rant_node_flush_tx(RantNode *n){
    int acquired;
    if (!n || n->fd == RANT_SOCK_BAD) return;
    acquired = i_rant_node_lock(n);
    i_rant_node_tx_drain(n);
    i_rant_node_unlock(n, acquired);
}

void i_rant_topic_clear_sys(RantTopic *topic){
    if (!topic) return;
    topic->sys_on_message = NULL;
    topic->sys_msg_user = NULL;
    topic->sys_on_dispatch = NULL;
}

void i_rant_topic_set_dispatch(RantTopic *topic, i_RantSysDispatchFn on_dispatch){
    if (topic) topic->sys_on_dispatch = on_dispatch;
}
RantQueue *i_rant_topic_queue(RantTopic *topic){ return topic ? topic->queue : NULL; }
int i_rant_topic_busy(RantTopic *topic){ return topic && topic->q && topic->q->busy; }

int i_rant_topic_park(RantTopic *topic, uint8_t kind, uint32_t from, RantBytes hdr,
                      RantBytes body, uint64_t written_us, uint64_t recv_us){
    if (!topic || !topic->q) return RANT_ERR_STATE;
    (void)i_rant_node_queue_push(topic->n, topic->index, topic->q, from, hdr, body,
                                 written_us, 0, kind, recv_us);   /* never refused: evicts */
    return RANT_OK;
}

int i_rant_topic_match_wait(RantTopic *topic){
    RantNode *n; int acquired, matched;
    if (!topic) return 0;
    n = topic->n;
    acquired = i_rant_node_lock(n);
    matched = rant_transport_publisher_match_count(n->transport, topic->index);
    /* wait only when it can help and can run: a match still forming, the knob on, a
       publishing role, and not from a callback. Converged matching returns at once */
    if (acquired && !matched && n->match_wait_us
        && rant_role_pubs(topic->role)
        && i_rant_node_topic_unsettled(n, topic, i_rant_plat_now_us()))
        matched = i_rant_node_match_wait(n, topic->index, topic, 0);
    i_rant_node_unlock(n, acquired);
    return matched;
}

int i_rant_topic_send_to(RantTopic *topic, uint32_t to_peer, RantBytes hdr, RantBytes data){
    int acquired, r;
    if (!topic) return RANT_ERR_NO_TOPIC;
    acquired = i_rant_node_lock(topic->n);
    r = i_rant_node_do_send_ex(topic->n, topic->index, hdr, data, 0, 1, to_peer, acquired);
    i_rant_node_send_tx(topic->n, acquired);
    i_rant_node_unlock(topic->n, acquired);
    return r;
}

/* The patterns layer's seams. Not lock guarded, the layer calls them under the node lock. */
void  *i_rant_node_sys_alloc(RantNode *n, void *ptr, size_t size){ return i_rant_node_alloc(n, ptr, size); }
void **i_rant_node_sys_slot (RantNode *n){ return &n->patterns; }
uint64_t i_rant_node_now_us (RantNode *n){ (void)n; return i_rant_plat_now_us(); }
uint64_t i_rant_node_wall_us(RantNode *n){ (void)n; return i_rant_plat_wall_us(); }
/* The node lock for a pattern call, reentrancy aware like the internal entry lock. No kick
 * on unlock: every mutating path kicks at its own layer, and read only ops must not wake. */
int  i_rant_node_sys_lock    (RantNode *n){ return i_rant_node_lock(n); }
void i_rant_node_sys_unlock(RantNode *n, int acquired){ i_rant_node_unlock(n, acquired); }
int  i_rant_node_sys_poll    (RantNode *n, int timeout_ms){ return rant_node_poll(n, timeout_ms); }

/* The patterns layer's blocking wait: the node's wait skeleton, so it sleeps on the
 * service thread's progress when one runs and pumps the loop itself otherwise. */
int i_rant_node_sys_wait(RantNode *n, i_RantSysWaitFn done, void *ctx){
    i_RantWait w = { 0 }; int acquired, r;
    if (!n) return -1;
    acquired = i_rant_node_lock(n);
    if (!acquired) return -1;       /* from a callback: can neither pump nor sleep */
    w.done = done; w.ctx = ctx;
    w.pump_ms = 5;                  /* the manual mode tick, as the callers pumped before */
    w.pump_after_svc = 1;           /* a service thread stopping under us finishes with the pump */
    r = i_rant_node_wait_until(n, &w);
    i_rant_node_unlock(n, acquired);
    return r == RANT__WAIT_DONE ? 1 : 0;
}

/* A topic scoped RANT_ERROR from the patterns layer, through the node's one event path. */
void i_rant_node_sys_error(RantNode *n, RantErrorKind error, RantTopic *topic, uint32_t peer){
    RantEvent e;
    if (!n) return;
    memset(&e, 0, sizeof e);
    e.kind = RANT_ERROR; e.error = error; e.peer = peer;
    if (topic){ e.topic = topic->index; e.topic_name = i_rant_node_topic_name(n, topic->index); }
    i_rant_node_emit(n, &e);
}

/* Matched subscribers excluding dormant peers, since rant_topic_match_count keeps counting
 * a dropped but resumable peer. */
int i_rant_topic_live_match_count(RantTopic *topic){
    int acquired, r;
    if (!topic) return 0;
    acquired = i_rant_node_lock(topic->n);
    r = rant_transport_publisher_live_matches(topic->n->transport, topic->index);
    i_rant_node_unlock(topic->n, acquired);
    return r;
}
int i_rant_topic_peer_matched(RantTopic *topic, uint32_t peer){
    int acquired, r;
    if (!topic) return 0;
    acquired = i_rant_node_lock(topic->n);
    r = rant_transport_publisher_peer_matched(topic->n->transport, topic->index, peer);
    i_rant_node_unlock(topic->n, acquired);
    return r;
}

/* Matched publishers feeding this topic's subscription side. */
int i_rant_topic_source_match_count(RantTopic *topic){
    int acquired, r;
    if (!topic) return 0;
    acquired = i_rant_node_lock(topic->n);
    r = rant_transport_subscriber_match_count(topic->n->transport, topic->index);
    i_rant_node_unlock(topic->n, acquired);
    return r;
}

/* The oldest live matched subscriber's peer id, 0 = none. */
uint32_t i_rant_topic_oldest_match(RantTopic *topic){
    uint32_t r; int acquired;
    if (!topic) return 0;
    acquired = i_rant_node_lock(topic->n);
    r = rant_transport_publisher_oldest_match(topic->n->transport, topic->index);
    i_rant_node_unlock(topic->n, acquired);
    return r;
}

const uint8_t *i_rant_node_uuid(RantNode *n){
    return n ? rant_discovery_uuid(rant_discovery_state(n->discovery)) : NULL;
}

/* Reflection getters for the patterns layer's entity enumeration. */
uint8_t    i_rant_topic_kind (const RantTopic *topic){ return topic ? topic->kind : 0; }
uint8_t    i_rant_topic_role (const RantTopic *topic){ return topic ? topic->role : (uint8_t)RANT_INACTIVE; }
uint8_t    i_rant_topic_reliability(const RantTopic *topic){ return topic ? (uint8_t)topic->qos.reliability : 0; }
int i_rant_node_peer_uuid(RantNode *n, uint32_t peer, uint8_t out[16]){
    const RantDiscoveryState *st; uint16_t q, np; int acquired, found = 0;
    RantDiscoveryPeer v;
    if (!n) return 0;
    acquired = i_rant_node_lock(n);
    st = rant_discovery_state(n->discovery);
    np = rant_discovery_max_peers(st);
    for (q = 0; q < np; q++)
        if (rant_discovery_peer_at(st, q, &v) && v.id == peer){ memcpy(out, v.uuid, 16); found = 1; break; }
    i_rant_node_unlock(n, acquired);
    return found;
}
RantString i_rant_topic_name (const RantTopic *topic){
    return topic ? rant_string(topic->name, topic->name_len) : rant_string(NULL, 0);
}
uint16_t   i_rant_node_topic_count(RantNode *n){ return n ? i_rant_node_topic_hi(n) : 0; }

void i_rant_node_set_sys_hooks(RantNode *n, i_RantSysEventFn on_event, i_RantSysTickFn tick,
                               i_RantSysCloseFn on_close, void *user){
    int acquired;
    if (!n) return;
    acquired = i_rant_node_lock(n);
    n->sys_on_event = on_event; n->sys_tick = tick; n->sys_on_close = on_close; n->sys_user = user;
    n->sys_tick_next = 0;
    i_rant_node_kick(n);     /* re evaluate the wait cap with the new tick */
    i_rant_node_unlock(n, acquired);
}

int rant_topic_set_role(RantTopic *topic, RantRole role){
    int r, acquired;
    if (!topic) return -1;
    acquired = i_rant_node_lock(topic->n);
    if (!acquired){
        /* from a callback: the replay would rematch the reader proxy mid delivery, refuse loudly */
        return RANT_ERR_STATE;
    }
    if (topic->q && topic->q->busy){    /* its own dispatched callback is running somewhere */
        i_rant_node_unlock(topic->n, acquired);
        return RANT_ERR_STATE;
    }
    /* a log builtin is queued before its subscribe side goes live, so the catch up replay
       can never race the first take into the inline path. See spec/node.md */
    if (rant_role_subs((uint8_t)role) && !topic->q
        && i_rant_node_is_log_topic(topic->n, topic->index))
        (void)i_rant_node_queue_ensure(topic->n, topic, NULL);
    r = rant_transport_set_role(topic->n->transport, topic->index, (uint8_t)role);
    if (r == 0){
        topic->role = (uint8_t)role;
        i_rant_node_readvertise(topic->n);     /* a role flip may raise fresh candidates */
    }
    i_rant_node_unlock(topic->n, acquired);
    return r;
}

int rant_topic_retire(RantTopic *topic){
    RantNode *n; int acquired; uint16_t idx;
    if (!topic) return RANT_ERR_NO_TOPIC;
    n = topic->n;
    acquired = i_rant_node_lock(n);
    if (!acquired) return RANT_ERR_STATE;     /* from a callback: lanes are live mid delivery */
    if (topic->sys_on_message                       /* a pattern channel: retire its handle */
        || i_rant_node_is_log_topic(n, topic->index)
        || (n->n_builtin && topic->index >= n->builtin_lo
            && topic->index < (uint16_t)(n->builtin_lo + n->n_builtin))
        || (topic->q && topic->q->busy)){           /* mid dispatch on another thread */
        i_rant_node_unlock(n, acquired);
        return RANT_ERR_STATE;
    }
    idx = topic->index;
    if (rant_transport_topic_retire(n->transport, idx) != 0){
        i_rant_node_unlock(n, acquired);
        return RANT_ERR_STATE;
    }
    /* the slot keeps its schema hash as the reuse fingerprint, the parsed copy goes */
    i_rant_node_core_retire_topic_schema(n->core, idx);
    i_rant_node_queue_drop(n, topic);
    if (topic->schema) rant_schema_free(topic->schema, i_rant_node_alloc, n);
    n->handles[idx] = NULL;
    i_rant_node_alloc(n, topic, 0);           /* the handle is invalid from here */
    /* the slot rides the announce as a hole from the next blob on */
    i_rant_node_readvertise(n);
    i_rant_node_unlock(n, acquired);
    return RANT_OK;
}

uint16_t rant_topic_index(const RantTopic *topic){ return topic ? topic->index : 0; }

const RantSchema *rant_topic_schema(const RantTopic *topic){ return topic ? topic->schema : NULL; }

RantTopic *rant_node_topic(RantNode *n, uint16_t index){
    RantTopic *h; int acquired;
    if (!n) return NULL;
    acquired = i_rant_node_lock(n);
    h = (index < i_rant_node_topic_hi(n)) ? n->handles[index] : NULL;     /* holes yield NULL */
    i_rant_node_unlock(n, acquired);
    return h;
}

/* reflection: the walks read the core's tables under the node lock */

int rant_node_peers_next(RantNode *n, RantIter *it, RantPeerInfo *out){
    int r, acquired;
    if (!n) return 0;
    acquired = i_rant_node_lock(n);
    r = i_rant_node_core_peers_next(n->core, it, out);
    if (r){   /* the transport's path measurement, folded in since the core is sans transport */
        RantPeerRtt e;
        if (rant_transport_peer_rtt(n->transport, out->id, &e)){
            out->rtt_us = e.rtt_us; out->rtt_jitter_us = e.rtt_jitter_us;
            out->rtt_min_us = e.rtt_min_us; out->rtt_samples = e.samples;
        }
    }
    i_rant_node_unlock(n, acquired);
    return r;
}

int rant_node_entities_next(RantNode *n, uint32_t peer, RantIter *it, RantEntityInfo *out){
    int r, acquired;
    if (!n) return 0;
    acquired = i_rant_node_lock(n);
    r = i_rant_node_core_entities_next(n->core, peer, it, out);
    i_rant_node_unlock(n, acquired);
    return r;
}

int rant_node_mesh_next(RantNode *n, RantIter *it, RantEntityInfo *out){
    int r, acquired;
    if (!n) return 0;
    acquired = i_rant_node_lock(n);
    r = i_rant_node_core_mesh_next(n->core, it, out);
    i_rant_node_unlock(n, acquired);
    return r;
}

int rant_node_mesh_find(RantNode *n, RantEntityKind kind, const char *name, RantEntityInfo *out){
    int r, acquired;
    if (!n) return 0;
    acquired = i_rant_node_lock(n);
    r = i_rant_node_core_mesh_find(n->core, kind, name, out);
    i_rant_node_unlock(n, acquired);
    return r;
}

uint32_t rant_node_mesh_epoch(RantNode *n){
    uint32_t e; int acquired;
    if (!n) return 0;
    acquired = i_rant_node_lock(n);
    e = i_rant_node_core_mesh_epoch(n->core);
    i_rant_node_unlock(n, acquired);
    return e;
}

/* Re types a live topic in place: the slot is retired and reused at the same index under
 * a bumped generation, the schema copy replaced, the handle and queue kept. Lock held. */
int i_rant_topic_retype(RantTopic *topic, const RantSchema *schema, uint8_t reliability){
    RantNode *n = topic->n; RantTopicDef def; RantSchema *copy = NULL; uint16_t idx = topic->index;
    uint32_t rb; int rc;
    if (schema){
        RantBytes w = rant_schema_wire(schema);
        copy = rant_schema_parse(w.data, w.len, i_rant_node_alloc, n);
        if (!copy) return RANT_ERR_OOM;
    }
    if (rant_transport_topic_retire(n->transport, idx) != 0){
        if (copy) rant_schema_free(copy, i_rant_node_alloc, n);
        return RANT_ERR_STATE;
    }
    i_rant_node_core_retire_topic_schema(n->core, idx);
    if (topic->schema) rant_schema_free(topic->schema, i_rant_node_alloc, n);
    topic->schema = copy;
    topic->qos.reliability = (RantReliability)reliability;
    memset(&def, 0, sizeof def);
    def.name = topic->name; def.role = topic->role; def.kind = topic->kind;
    def.prefix_bytes = topic->prefix_bytes; def.directed = topic->directed; def.attrs = topic->attrs;
    def.qos = topic->qos;
    rb = rant_discovery_meta_version(rant_discovery_state(n->discovery)) + 1u;
    rc = rant_transport_topic_reuse(n->transport, idx, &def, 1, rb);
    if (rc != 0) return rc == -4 ? RANT_ERR_OOM : RANT_ERR_STATE;
    i_rant_node_core_topic_rebound(n->core, idx);
    i_rant_node_core_set_topic_schema(n->core, idx, topic->schema);
    i_rant_node_readvertise(n);
    return RANT_OK;
}

int rant_topic_refresh(RantTopic *topic){
    RantNode *n; int acquired, r = 0;
    const RantSchema *ms = NULL; uint8_t rel = 0; uint64_t gen = 0;
    if (!topic) return RANT_ERR_NO_TOPIC;
    if (!topic->reflect) return RANT_ERR_ROLE;
    n = topic->n;
    acquired = i_rant_node_lock(n);
    if (!acquired) return RANT_ERR_STATE;     /* from a callback: lanes are live mid delivery */
    if (i_rant_node_core_reflect_pick(n->core, RANT_ENTITY_TOPIC, topic->name, 0,
                                      rant_role_pubs(topic->role), &ms, &rel, &gen)
        && gen != topic->generation){
        r = i_rant_topic_retype(topic, ms, rel ? RANT_RELIABLE : RANT_BEST_EFFORT);
        if (r == 0){ topic->generation = gen; r = 1; }
    }
    i_rant_node_unlock(n, acquired);
    return r;
}

/* the patterns layer's reflect_from_mesh: the pick for one channel of an entity */
int i_rant_node_reflect_pick(RantNode *n, RantEntityKind kind, const char *name, int which, int writer,
                             const RantSchema **schema, uint8_t *reliable, uint64_t *generation){
    int r, acquired;
    if (!n) return 0;
    acquired = i_rant_node_lock(n);
    r = i_rant_node_core_reflect_pick(n->core, kind, name, which, writer, schema, reliable, generation);
    i_rant_node_unlock(n, acquired);
    return r;
}

void rant_node_backpressure_stats(RantNode *n, uint64_t *waited_us, uint32_t *waited_sends){
    int acquired = i_rant_node_lock(n);
    if (waited_us)    *waited_us    = n->backpressure_total_us;
    if (waited_sends) *waited_sends = n->backpressure_wait_count;
    i_rant_node_unlock(n, acquired);
}

/* Sends that evicted never sent history after the bounded wait, since open. */
uint32_t rant_node_evicted_unsent(RantNode *n){
    uint32_t v; int acquired;
    if (!n) return 0;
    acquired = i_rant_node_lock(n);
    v = n->evicted_unsent;
    i_rant_node_unlock(n, acquired);
    return v;
}

#ifndef RANT_NO_STDTYPES
/* The standard types that need a platform. They sit here so serialize/ keeps needing
 * nothing but memory. */
RantTimestamp rant_timestamp_now(void){
    return (RantTimestamp)i_rant_plat_wall_us();
}

void rant_uuid_new(RantUuid *out){
    /* one generator: the CSPRNG path, else the host identity mix a node's own uuid uses */
    if (out) i_rant_discovery_auto_uuid(out->bytes);
}
#endif

void rant_node_mem_stats(RantNode *n, size_t *in_use, size_t *peak, uint64_t *alloc_calls){
    int acquired;
    if (!n) return;
    acquired = i_rant_node_lock(n);
    rant_allocator_stats(&n->pool, in_use, peak, alloc_calls);
    i_rant_node_unlock(n, acquired);
}

void rant_topic_repair_stats(RantTopic *topic, RantRepairStats *out){
    if (topic){
        int acquired = i_rant_node_lock(topic->n);
        rant_transport_repair_stats(topic->n->transport, topic->index, out);
        i_rant_node_unlock(topic->n, acquired);
    } else if (out) memset(out, 0, sizeof *out);
}

void rant_topic_counts(RantTopic *topic, uint64_t *tx_msgs, uint64_t *tx_bytes,
                       uint64_t *rx_msgs, uint64_t *rx_bytes){
    uint64_t tm = 0, tb = 0, rm = 0, rb = 0;
    if (topic){
        int acquired = i_rant_node_lock(topic->n);
        tm = topic->tx_msgs; tb = topic->tx_bytes; rm = topic->rx_msgs; rb = topic->rx_bytes;
        i_rant_node_unlock(topic->n, acquired);
    }
    if (tx_msgs)  *tx_msgs  = tm;
    if (tx_bytes) *tx_bytes = tb;
    if (rx_msgs)  *rx_msgs  = rm;
    if (rx_bytes) *rx_bytes = rb;
}

/* the @rant/meta snapshot builder, see spec/node.md */

/* one pass of the map body. A latched writer error makes finish return 0 and the caller
 * grows and rebuilds */
static void i_rant_node_snapshot_fill(RantNode *n, RantMapWriter *w, uint32_t sections){
    uint64_t now = i_rant_plat_now_us();
    if (sections & RANT_META_NODE){
        size_t in_use = 0, peak = 0; uint64_t allocs = 0;
        rant_allocator_stats(&n->pool, &in_use, &peak, &allocs);
        rant_map_open_map(w, "node");
        rant_map_put_string(w, "name", rant_string(n->name, n->name_len));
        rant_map_put_uint(w, "uptime_us", now - n->open_us);
        rant_map_put_uint(w, "mono_us", now);
        rant_map_put_uint(w, "wall_us", i_rant_plat_wall_us());
        rant_map_put_uint(w, "mem_in_use", (uint64_t)in_use);
        rant_map_put_uint(w, "mem_peak", (uint64_t)peak);
        rant_map_put_uint(w, "alloc_calls", allocs);
        rant_map_put_uint(w, "evicted_unsent", n->evicted_unsent);
        rant_map_put_uint(w, "bp_waited_us", n->backpressure_total_us);
        rant_map_put_uint(w, "bp_waits", n->backpressure_wait_count);
        rant_map_put_uint(w, "peers", rant_discovery_peer_count(rant_discovery_state(n->discovery)));
        rant_map_put_uint(w, "max_peers", n->max_peers);
        /* app topics only: the @rant/ builtins are hidden, so neither the counts nor the
           topics array below surface them */
        rant_map_put_uint(w, "topics", (uint64_t)n->n_created);
        rant_map_put_uint(w, "max_topics", (uint64_t)(n->max_topics - n->n_builtin));
#ifdef RANT_SHM
        rant_map_put_uint(w, "shm_tx", n->shm_tx);
        rant_map_put_uint(w, "shm_rx", n->shm_rx);
#endif
        rant_map_put_uint(w, "last_error", (uint64_t)n->last_error.error);
        if (n->last_error.error != RANT_E_NONE){
            /* format from a sanitized copy: the stored event's name views can outlive what
               they pointed at, so the text carries the indices instead */
            RantEvent le = n->last_error; char txt[160];
            le.topic_name = NULL; le.peer_name = NULL; le.schema_detail = NULL;
            rant_event_str(&le, txt, sizeof txt);
            rant_map_put_string(w, "last_error_text", rant_cstr(txt));
        }
        rant_map_close(w);
    }
#ifdef RANT_PROC_STATS
    if (sections & RANT_META_PROC){
        uint64_t cpu = 0, rss = 0, peak_rss = 0; int have_cpu = 0;
        if (i_rant_plat_proc_stats(&cpu, &rss, &peak_rss, &have_cpu)){     /* absent if unsupported */
            rant_map_open_map(w, "proc");
            rant_map_put_uint(w, "pid", i_rant_plat_pid());
            if (have_cpu) rant_map_put_uint(w, "cpu_us", cpu);
            rant_map_put_uint(w, "rss", rss);
            rant_map_put_uint(w, "peak_rss", peak_rss);
            {   uint64_t heap_total, heap_free, heap_min_free, heap_largest_free_block;
                if (i_rant_plat_heap_stats(&heap_total, &heap_free, &heap_min_free,
                                           &heap_largest_free_block)){
                    rant_map_put_uint(w, "heap_total", heap_total);
                    rant_map_put_uint(w, "heap_free", heap_free);
                    rant_map_put_uint(w, "heap_min_free", heap_min_free);
                    rant_map_put_uint(w, "heap_largest_free_block", heap_largest_free_block);
                }
            }
            rant_map_close(w);
        }
    }
#endif
    if (sections & RANT_META_TOPICS){
        uint16_t i, hi = i_rant_node_topic_hi(n);
        const uint16_t *pend = NULL;
        if (hi){
            /* every topic's unresolved count in one walk of each peer's interest, since the
               per topic query is quadratic for an observer. On OOM the per topic query stands in */
            if (n->snap_pend_cap < hi){
                uint16_t *nb = (uint16_t*)i_rant_node_alloc(n, n->snap_pend, (size_t)hi * sizeof *nb);
                if (nb){ n->snap_pend = nb; n->snap_pend_cap = hi; }
            }
            if (n->snap_pend_cap >= hi){
                i_rant_node_core_topics_unresolved(n->core, n->snap_pend, hi);
                pend = n->snap_pend;
            }
        }
        rant_map_open_array(w, "topics");
        for (i = 0; i < hi; i++){
            RantTopic *h = n->handles[i];
            const RantQos *q = rant_transport_topic_qos(n->transport, i);
            RantRepairStats rs;
            if (!h) continue;
            if (n->n_builtin && i >= n->builtin_lo
                             && i < (uint16_t)(n->builtin_lo + n->n_builtin)) continue;
            rant_transport_repair_stats(n->transport, i, &rs);
            rant_map_open_map(w, NULL);
            rant_map_put_uint(w, "index", i);
            rant_map_put_string(w, "name", rant_string(h->name, h->name_len));
            rant_map_put_uint(w, "kind", h->kind);
            rant_map_put_uint(w, "role", h->role);
            rant_map_put_bool(w, "reliable", q && q->reliability == RANT_RELIABLE);
            rant_map_put_uint(w, "keep_last", q ? q->keep_last : 0);
            rant_map_put_uint(w, "catch_up", q ? q->catch_up : 0);
            rant_map_put_uint(w, "subs", (uint64_t)rant_transport_publisher_match_count(n->transport, i));
            rant_map_put_uint(w, "pubs", (uint64_t)rant_transport_subscriber_match_count(n->transport, i));
            rant_map_put_uint(w, "pending", pend ? (uint64_t)pend[i]
                                                 : (uint64_t)i_rant_node_core_topic_unresolved(n->core, i));
            rant_map_put_uint(w, "tx_msgs", h->tx_msgs);
            rant_map_put_uint(w, "tx_bytes", h->tx_bytes);
            rant_map_put_uint(w, "rx_msgs", h->rx_msgs);
            rant_map_put_uint(w, "rx_bytes", h->rx_bytes);
            rant_map_put_uint(w, "nacks_recv", rs.nacks_recv);
            rant_map_put_uint(w, "frags_resent", rs.frags_resent);
            rant_map_put_uint(w, "frags_sent", rs.frags_sent);
            rant_map_put_uint(w, "nacks_sent", rs.nacks_sent);
            rant_map_put_uint(w, "frags_recv", rs.frags_recv);
            rant_map_put_uint(w, "frags_dup", rs.frags_dup);
            rant_map_put_uint(w, "msgs_skipped", rs.msgs_skipped);
            if (h->q){
                rant_map_put_uint(w, "q_msgs", h->q->count);
                rant_map_put_uint(w, "q_bytes", h->q->bytes);
                rant_map_put_uint(w, "q_cap", h->q->cap);
                rant_map_put_uint(w, "q_dropped", h->q->dropped);
            }
            rant_map_close(w);
        }
        rant_map_close(w);
    }
    if (sections & RANT_META_PEERS){
        uint16_t cnt = 0, i;
        const RantDiscoveryPeer *ps = rant_discovery_peers(n->discovery, &cnt);
        rant_map_open_array(w, "peers");
        for (i = 0; i < cnt; i++){
            uint16_t pub_to = 0, recv_from = 0;
            char ip[16]; int ln = 0;
            rant_transport_peer_match_counts(n->transport, ps[i].id, &pub_to, &recv_from);
            rant_map_open_map(w, NULL);
            rant_map_put_uint(w, "id", ps[i].id);
            rant_map_put_string(w, "name", ps[i].name);
            rant_map_put_bool(w, "active", ps[i].liveness == RANT_PEER_ACTIVE);
            if (ps[i].addr.ip_len == 4)
                ln = snprintf(ip, sizeof ip, "%u.%u.%u.%u", ps[i].addr.ip[0], ps[i].addr.ip[1],
                              ps[i].addr.ip[2], ps[i].addr.ip[3]);
            rant_map_put_string(w, "ip", rant_string(ip, ln > 0 ? (size_t)ln : 0));
            rant_map_put_uint(w, "port", ps[i].addr.port);
            rant_map_put_uint(w, "age_us", now > ps[i].last_heard_us ? now - ps[i].last_heard_us : 0);
            rant_map_put_uint(w, "publish_to", pub_to);
            rant_map_put_uint(w, "receive_from", recv_from);
            {   RantPeerRtt e;     /* the measured round trip, absent until the first sample */
                if (rant_transport_peer_rtt(n->transport, ps[i].id, &e) && e.samples){
                    rant_map_put_uint(w, "rtt_us", e.rtt_us);
                    rant_map_put_uint(w, "rtt_jitter_us", e.rtt_jitter_us);
                    rant_map_put_uint(w, "rtt_min_us", e.rtt_min_us);
                    rant_map_put_uint(w, "rtt_samples", e.samples);
                } }
            rant_map_close(w);
        }
        rant_map_close(w);
    }
}

RantBytes i_rant_node_snapshot(RantNode *n, uint32_t sections){
    uint32_t body;
    if (!n) return rant_bytes(NULL, 0);
    if (!sections) sections = 0xFFFFFFFFu;
    for (;;){
        RantMapWriter w;
        if (!n->snap_buf){
            /* a small node's snapshot is 1 to 2 KB: start there, the doubling finds the size */
            n->snap_buf = (uint8_t*)i_rant_node_alloc(n, NULL, 1024u);
            if (!n->snap_buf) return rant_bytes(NULL, 0);
            n->snap_cap = 1024u;
        }
        w = rant_map_begin(n->snap_buf, n->snap_cap);
        i_rant_node_snapshot_fill(n, &w, sections);
        body = rant_map_finish(&w);
        if (body) break;
        {   /* did not fit (the writer latches on any failure): double and rebuild */
            uint32_t ncap = n->snap_cap * 2u; uint8_t *nb;
            if (ncap > (1u << 22)) return rant_bytes(NULL, 0);     /* 4 MB: not a size problem */
            nb = (uint8_t*)i_rant_node_alloc(n, n->snap_buf, ncap);
            if (!nb) return rant_bytes(NULL, 0);
            n->snap_buf = nb; n->snap_cap = ncap;
        }
    }
    return rant_bytes(n->snap_buf, body);
}

void rant_node_set_pump_probe(RantNode *n, RantPumpProbeFn fn, uint64_t interval_us, void *user){
    int acquired = i_rant_node_lock(n);
    n->pump_probe = fn; n->pump_probe_interval_us = interval_us; n->pump_probe_user = user;
    i_rant_node_unlock(n, acquired);
}

int rant_topic_subscriber_progress(RantTopic *topic, uint32_t peer,
                                 uint64_t *base_seqno, uint32_t *have, uint32_t *total){
    int r, acquired;
    if (!topic) return 0;
    acquired = i_rant_node_lock(topic->n);
    r = rant_transport_subscriber_progress(topic->n->transport, topic->index, peer, base_seqno, have, total);
    i_rant_node_unlock(topic->n, acquired);
    return r;
}

#ifdef RANT_SHM
void rant_node_shm_stats(RantNode *n, uint32_t *sent, uint32_t *recv){
    int acquired = i_rant_node_lock(n);
    if (sent) *sent = n->shm_tx;
    if (recv) *recv = n->shm_rx;
    i_rant_node_unlock(n, acquired);
}
#endif

/* drain's predicate: the topic's send queue is empty at the transport */
typedef struct { uint16_t index; uint64_t deadline; } i_RantDrainWait;

static int i_rant_node_drain_wait_done(RantNode *n, void *ctx, uint64_t now, uint64_t *deadline){
    i_RantDrainWait *c = (i_RantDrainWait*)ctx;
    (void)now;
    *deadline = c->deadline;
    return rant_transport_send_drained(n->transport, c->index) != 0;
}

int rant_topic_drain(RantTopic *topic, int timeout_ms){
    RantNode *n; i_RantDrainWait c; i_RantWait w = { 0 }; int acquired, drained;
    if (!topic) return 0;
    n = topic->n;
    acquired = i_rant_node_lock(n);
    if (!acquired) return 0;   /* from a callback: can neither pump nor wait */
    c.index = topic->index;
    c.deadline = i_rant_plat_now_us() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000u;
    w.done = i_rant_node_drain_wait_done; w.periodic = i_rant_node_wait_kick; w.ctx = &c;
    w.pump_ms = 1;          /* a nested tick: the lock stays held */
    w.pump_after_svc = 1;   /* a service thread stopping under us finishes with the pump */
    drained = i_rant_node_wait_until(n, &w) == RANT__WAIT_DONE;
    i_rant_node_unlock(n, acquired);
    return drained;
}

int rant_topic_match_count(RantTopic *topic){
    int r, acquired;
    if (!topic) return 0;
    acquired = i_rant_node_lock(topic->n);
    r = rant_transport_publisher_match_count(topic->n->transport, topic->index);
    i_rant_node_unlock(topic->n, acquired);
    return r;
}

int rant_topic_pending_count(RantTopic *topic){
    int r, acquired;
    if (!topic) return 0;
    acquired = i_rant_node_lock(topic->n);
    r = i_rant_node_core_topic_unresolved(topic->n->core, topic->index);
    i_rant_node_unlock(topic->n, acquired);
    return r;
}

/* 1 = a send now would not wait: matched, or converged with nobody to wait for. Shares the
 * send path's predicate, so a GUI polling this then sending sees what the send decides. */
int rant_topic_ready(RantTopic *topic){
    int r, acquired;
    if (!topic) return 0;
    acquired = i_rant_node_lock(topic->n);
    r = rant_transport_publisher_match_count(topic->n->transport, topic->index) > 0
     || !i_rant_node_topic_unsettled(topic->n, topic, i_rant_plat_now_us());
    i_rant_node_unlock(topic->n, acquired);
    return r;
}

/* Settled: the network answered and went quiet. Every active peer heard since the solicit,
 * the topology quiet for one window, one window passed overall. See spec/node.md. */
static int i_rant_node_settled(RantNode *n, uint64_t start, uint64_t now){
    uint64_t quiet = n->announce_us < 300000u ? n->announce_us : 300000u;
    uint16_t i, count = 0;
    const RantDiscoveryPeer *peers = rant_discovery_peers(n->discovery, &count);
    int any = 0;
    for (i = 0; i < count; i++){
        if (peers[i].liveness != RANT_PEER_ACTIVE) continue;     /* dropped: not expected to answer */
        any = 1;
        if (peers[i].last_heard_us < start) return 0;          /* not answered the solicit yet */
    }
    if (!any) return now - start >= n->announce_us;            /* alone, as far as we can know */
    if (now - start < quiet) return 0;
    return n->settle_topology_us < start || now - n->settle_topology_us >= quiet;
}

typedef struct {
    uint64_t start;         /* the settled anchor and the deadline's base */
    uint64_t deadline;
    uint64_t last_solicit;  /* 0 = never: the first iteration solicits at once */
} i_RantSettleWait;

static int i_rant_node_settle_wait_done(RantNode *n, void *ctx, uint64_t now, uint64_t *deadline){
    i_RantSettleWait *c = (i_RantSettleWait*)ctx;
    *deadline = c->deadline;
    return i_rant_node_settled(n, c->start, now);
}

/* re solicit 4 times a second so a lost one retries. The kick makes a service thread send
 * it now, a pump sends it on the tick that follows. */
static void i_rant_node_settle_wait_solicit(RantNode *n, void *ctx, uint64_t now){
    i_RantSettleWait *c = (i_RantSettleWait*)ctx;
    if (now - c->last_solicit < 250000u) return;
    rant_discovery_solicit(rant_discovery_state(n->discovery));
    c->last_solicit = now;
    i_rant_node_wait_kick(n, ctx, now);
}

int rant_node_settle(RantNode *n, int timeout_ms){
    i_RantSettleWait c; i_RantWait w = { 0 }; int acquired, settled;
    if (!n) return 0;
    acquired = i_rant_node_lock(n);
    if (!acquired){ return 0; }   /* from a callback: can neither pump nor wait */
    c.start = i_rant_plat_now_us();
    c.last_solicit = 0;
    c.deadline = c.start + (timeout_ms >= 0 ? (uint64_t)timeout_ms * 1000u
                                            : (uint64_t)n->announce_us * 3u);
    w.done = i_rant_node_settle_wait_done; w.periodic = i_rant_node_settle_wait_solicit;
    w.ctx = &c;
    w.cv_cap_us = 50000u;   /* re check the clock: settling is partly time driven */
    w.pump_ms = 1;          /* a nested tick: sends the solicit, takes in the replies */
    w.pump_after_svc = 1;   /* a service thread stopping under us finishes with the pump */
    settled = i_rant_node_wait_until(n, &w) == RANT__WAIT_DONE;
    i_rant_node_unlock(n, acquired);
    return settled;
}

/* the consumer queue API, see spec/node.md */

static int i_rant_node_any_queued(RantNode *n){
    uint16_t i, hi = i_rant_node_topic_hi(n);
    for (i = 0; i < hi; i++)
        if (n->handles[i] && n->handles[i]->q && n->handles[i]->q->count) return 1;
    return 0;
}

/* records waiting on the handles created with the callback queue g, or on its event ring */
static int i_rant_queue_any(RantNode *n, RantQueue *g){
    uint16_t i, hi = i_rant_node_topic_hi(n);
    if (n->event_queue == g && n->event_q && n->event_q->count) return 1;
    for (i = 0; i < hi; i++){
        RantTopic *h = n->handles[i];
        if (h && h->queue == g && h->q && h->q->count) return 1;
    }
    return 0;
}

/* the queue wait's predicate: data on q when one was given, else on the group g, else on
 * any queued topic. The structs are stable allocations, so the pointers survive the waits. */
typedef struct { i_RantMsgQueue *q; RantQueue *g; uint64_t deadline; } i_RantQueueWait;

static int i_rant_node_queue_wait_done(RantNode *n, void *ctx, uint64_t now, uint64_t *deadline){
    i_RantQueueWait *c = (i_RantQueueWait*)ctx;
    (void)now;
    *deadline = c->deadline;
    return c->q ? (c->q->count != 0) : c->g ? i_rant_queue_any(n, c->g) : i_rant_node_any_queued(n);
}

/* Waits until data is queued: on the service thread's progress when one runs, else by
 * driving the poll loop itself. Lock held on entry and exit, never from a callback. */
static void i_rant_node_queue_wait(RantNode *n, i_RantMsgQueue *q, RantQueue *g, int timeout_ms){
    i_RantQueueWait c; i_RantWait w = { 0 };
    c.q = q; c.g = g;
    c.deadline = timeout_ms < 0 ? (uint64_t)-1
                                : i_rant_plat_now_us() + (uint64_t)timeout_ms * 1000u;
    w.done = i_rant_node_queue_wait_done; w.ctx = &c;
    w.cv_cap_us = 3600000000u;   /* an unbounded wait still re checks hourly */
    w.pump_outer = 1;            /* outer: this wait may drop the lock */
    w.pump_after_svc = 1;        /* a service thread stopping under us pumps from then on */
    i_rant_node_wait_until(n, &w);
}

/* Dispatches up to max_msgs of the messages queued at entry, callbacks on the calling
 * thread and outside the lock when this thread owns it. A reentrant caller keeps the lock. */
static int i_rant_topic_dispatch_locked(RantNode *n, RantTopic *h, int max_msgs, int acquired){
    i_RantMsgQueue *q = h->q;
    int done = 0; uint32_t todo;
    if (!q || q->busy) return 0;
    i_rant_node_queue_release(n, h, q);
    todo = q->count;                    /* a snapshot: later arrivals wait for the next call */
    if (max_msgs > 0 && todo > (uint32_t)max_msgs) todo = (uint32_t)max_msgs;
    while (todo-- && q->count){
        const i_RantQRec *rec = i_rant_q_peek(q);
        RantMsg m;
        i_rant_node_queue_msg(n, h, rec, &m);
        q->viewing = 1;
        if (n->user_on_message){
            q->busy = 1;
#ifdef RANT_THREADS
            if (acquired){
                i_rant_node_unlock_raw(n);
                n->user_on_message(&m);
                i_rant_node_lock_raw(n);
            } else
#endif
            n->user_on_message(&m);
            q->busy = 0;
        }
        i_rant_node_queue_release(n, h, q);
        done++;
    }
    (void)acquired;
    return done;
}

int rant_topic_take(RantTopic *topic, RantMsg *out, int timeout_ms){
    RantNode *n; i_RantMsgQueue *q; int acquired, got = 0;
    if (!topic || !out) return RANT_ERR_NO_TOPIC;
    n = topic->n;
    acquired = i_rant_node_lock(n);
    q = i_rant_node_queue_ensure(n, topic, NULL);
    if (!q){ i_rant_node_unlock(n, acquired); return RANT_ERR_OOM; }
    if (q->busy){ i_rant_node_unlock(n, acquired); return RANT_ERR_STATE; }
    i_rant_node_queue_release(n, topic, q);        /* finish the previous view first */
    if (!q->count && timeout_ms != 0 && acquired)
        i_rant_node_queue_wait(n, q, NULL, timeout_ms);
    {   const i_RantQRec *rec = i_rant_q_peek(q);
        if (rec){
            i_rant_node_queue_msg(n, topic, rec, out);
            q->viewing = 1;                   /* the view lives until the next take or dispatch */
            got = 1;
        }
    }
    i_rant_node_unlock(n, acquired);
    return got;
}

int rant_topic_dispatch(RantTopic *topic, int max_msgs, int timeout_ms){
    RantNode *n; i_RantMsgQueue *q; int acquired, done;
    if (!topic) return RANT_ERR_NO_TOPIC;
    n = topic->n;
    acquired = i_rant_node_lock(n);
    q = i_rant_node_queue_ensure(n, topic, NULL);
    if (!q){ i_rant_node_unlock(n, acquired); return RANT_ERR_OOM; }
    if (q->busy){ i_rant_node_unlock(n, acquired); return RANT_ERR_STATE; }
    i_rant_node_queue_release(n, topic, q);
    if (!q->count && timeout_ms != 0 && acquired)
        i_rant_node_queue_wait(n, q, NULL, timeout_ms);
    done = i_rant_topic_dispatch_locked(n, topic, max_msgs, acquired);
    i_rant_node_unlock(n, acquired);
    return done;
}

int rant_node_dispatch(RantNode *n, int max_msgs, int timeout_ms){
    int acquired, total = 0;
    uint16_t i;
    if (!n) return RANT_ERR_STATE;
    acquired = i_rant_node_lock(n);
    if (!i_rant_node_any_queued(n) && timeout_ms != 0 && acquired)
        i_rant_node_queue_wait(n, NULL, NULL, timeout_ms);
    for (i = 0; i < i_rant_node_topic_hi(n); i++){
        RantTopic *h = n->handles[i];
        if (!h || !h->q || !h->q->count) continue;
        total += i_rant_topic_dispatch_locked(n, h, max_msgs > 0 ? max_msgs - total : 0, acquired);
        if (max_msgs > 0 && total >= max_msgs) break;
    }
    i_rant_node_unlock(n, acquired);
    return total;
}

/* the callback queue API, see spec/node.md */

RantQueue *rant_node_create_queue(RantNode *n){
    RantQueue *q; int acquired;
    if (!n) return NULL;
    acquired = i_rant_node_lock(n);
    if (!acquired || n->n_queues >= RANT_QUEUES_MAX){
        i_rant_node_create_fail(n, RANT_E_STATE, NULL, 0, 1);
        i_rant_node_unlock(n, acquired); return NULL;
    }
    q = (RantQueue*)i_rant_node_alloc(n, NULL, sizeof *q);
    if (!q){
        i_rant_node_create_fail(n, RANT_E_OOM, NULL, sizeof *q, 1);
        i_rant_node_unlock(n, acquired); return NULL;
    }
    memset(q, 0, sizeof *q);
    q->n = n;
    n->queues[n->n_queues++] = q;
    i_rant_node_unlock(n, acquired);
    return q;
}

/* The ring holding the oldest waiting record of the group, stamped at or before cutoff so
 * arrivals during a dispatch wait for the next call. *h is NULL for the event ring. NULL
 * when none. Lock held. */
static i_RantMsgQueue *i_rant_queue_oldest(RantNode *n, RantQueue *g, uint64_t cutoff, RantTopic **h_out){
    i_RantMsgQueue *best = NULL; RantTopic *best_h = NULL; uint64_t best_us = 0;
    uint16_t i, hi = i_rant_node_topic_hi(n);
    if (n->event_queue == g && n->event_q && n->event_q->count){
        const i_RantQRec *rec = i_rant_q_peek(n->event_q);
        if (rec->t_recv_us <= cutoff){ best = n->event_q; best_us = rec->t_recv_us; }
    }
    for (i = 0; i < hi; i++){
        RantTopic *h = n->handles[i]; const i_RantQRec *rec;
        if (!h || h->queue != g || !h->q || !h->q->count) continue;
        rec = i_rant_q_peek(h->q);
        if (rec->t_recv_us > cutoff) continue;
        if (!best || rec->t_recv_us < best_us){ best = h->q; best_h = h; best_us = rec->t_recv_us; }
    }
    *h_out = best_h;
    return best;
}

int rant_queue_dispatch(RantQueue *g, int max_callbacks, int timeout_ms){
    RantNode *n; int acquired, done = 0; uint64_t cutoff;
    if (!g) return RANT_ERR_STATE;
    n = g->n;
    acquired = i_rant_node_lock(n);
    if (!acquired) return RANT_ERR_STATE;          /* from an inline callback */
    if (g->busy){ i_rant_node_unlock(n, acquired); return RANT_ERR_STATE; }   /* nested, or another thread */
    if (!i_rant_queue_any(n, g) && timeout_ms != 0 && acquired)
        i_rant_node_queue_wait(n, NULL, g, timeout_ms);
    cutoff = i_rant_plat_now_us();
    g->busy = 1; n->dispatching++;
    while (max_callbacks <= 0 || done < max_callbacks){
        RantTopic *h;
        i_RantMsgQueue *q = i_rant_queue_oldest(n, g, cutoff, &h);
        const i_RantQRec *rec; uint8_t kind; RantMsg m; RantEvent e;
        i_RantSysDispatchFn sys = NULL; int run = 0;
        if (!q) break;
        rec = i_rant_q_peek(q); kind = rec->kind;
        if (!h){                               /* the event ring */
            i_rant_node_queue_event(n, rec, &e);
            run = n->on_event != NULL;
        } else {
            sys = h->kind != RANT_KIND_TOPIC ? h->sys_on_dispatch : NULL;
            i_rant_node_queue_msg(n, h, rec, &m);
            /* a pattern channel whose routing was cleared mid retire drops the record */
            run = sys != NULL || (h->kind == RANT_KIND_TOPIC && n->user_on_message != NULL);
        }
        q->viewing = 1; q->busy = 1;           /* busy: retire of this handle is refused meanwhile */
        if (run){
#ifdef RANT_THREADS
            if (acquired){
                i_rant_node_unlock_raw(n);
                if (!h) n->on_event(&e); else if (sys) sys(h->sys_msg_user, &m, kind); else n->user_on_message(&m);
                i_rant_node_lock_raw(n);
            } else
#endif
            { if (!h) n->on_event(&e); else if (sys) sys(h->sys_msg_user, &m, kind); else n->user_on_message(&m); }
        }
        q->busy = 0;
        if (h) i_rant_node_queue_release(n, h, q);
        else if (q->viewing){ q->viewing = 0; i_rant_q_pop(q); }
        done++;
    }
    g->busy = 0; n->dispatching--;
    i_rant_node_unlock(n, acquired);
    return done;
}

int rant_node_set_event_queue(RantNode *n, RantQueue *q){
    int acquired;
    if (!n) return RANT_ERR_STATE;
    acquired = i_rant_node_lock(n);
    if (!acquired || (q && q->n != n) || (n->event_q && n->event_q->busy)){
        i_rant_node_unlock(n, acquired); return RANT_ERR_STATE;
    }
    if (!q){                                   /* inline again: the parked events go */
        if (n->event_q){
            if (n->event_q->buf) i_rant_node_alloc(n, n->event_q->buf, 0);
            i_rant_node_alloc(n, n->event_q, 0);
            n->event_q = NULL;
        }
        n->event_queue = NULL;
        i_rant_node_unlock(n, acquired); return RANT_OK;
    }
    if (!n->event_q){
        i_RantMsgQueue *r = (i_RantMsgQueue*)i_rant_node_alloc(n, NULL, sizeof *r);
        uint32_t limit = RANT__QALIGN(n->event_queue_bytes), initial = limit < 4096u ? limit : 4096u;
        if (!r){ i_rant_node_unlock(n, acquired); return RANT_ERR_OOM; }
        memset(r, 0, sizeof *r);
        r->buf = (uint8_t*)i_rant_node_alloc(n, NULL, initial);
        if (!r->buf){ i_rant_node_alloc(n, r, 0); i_rant_node_unlock(n, acquired); return RANT_ERR_OOM; }
        r->cap = initial; r->cap_limit = limit; r->silent = 1;   /* best effort, evicts quietly */
        n->event_q = r;
    }
    n->event_queue = q;
    i_rant_node_unlock(n, acquired);
    return RANT_OK;
}

void rant_queue_stats(RantQueue *g, uint32_t *waiting, uint32_t *dropped){
    uint32_t w = 0, d = 0;
    if (g){
        RantNode *n = g->n; int acquired = i_rant_node_lock(n);
        uint16_t i, hi = i_rant_node_topic_hi(n);
        if (n->event_queue == g && n->event_q){ w += n->event_q->count; d += n->event_q->dropped; }
        for (i = 0; i < hi; i++){
            RantTopic *h = n->handles[i];
            if (h && h->queue == g && h->q){ w += h->q->count; d += h->q->dropped; }
        }
        i_rant_node_unlock(n, acquired);
    }
    if (waiting) *waiting = w;
    if (dropped) *dropped = d;
}

void rant_topic_queue_stats(RantTopic *topic, uint32_t *msgs, uint32_t *bytes,
                              uint32_t *capacity, uint32_t *dropped){
    uint32_t m = 0, b = 0, c = 0, d = 0;
    if (topic && topic->q){
        int acquired = i_rant_node_lock(topic->n);
        m = topic->q->count; b = topic->q->bytes; c = topic->q->cap; d = topic->q->dropped;
        i_rant_node_unlock(topic->n, acquired);
    }
    if (msgs)     *msgs = m;
    if (bytes)    *bytes = b;
    if (capacity) *capacity = c;
    if (dropped)  *dropped = d;
}

#ifdef RANT_THREADS
/* The service thread: the poll body in a loop, holding the lock for every work pass and
 * dropping it inside the wait. The 250 ms cap bounds a lost wakeup. See spec/node.md. */
static void i_rant_node_service(void *arg){
    RantNode *n = (RantNode *)arg;
    i_rant_node_lock_raw(n);
    while (!n->svc_stop)
        i_rant_node_poll_locked(n, 250, 1);
    i_rant_node_unlock_raw(n);
}
#endif

int rant_node_start(RantNode *n){
#ifndef RANT_THREADS
    (void)n;
    return RANT_ERR_NOSYS;
#else
    int acquired;
    if (!n) return RANT_ERR_STATE;
    acquired = i_rant_node_lock(n);
    if (!acquired) return RANT_ERR_STATE;                 /* from a callback */
    if (n->svc_running){ i_rant_node_unlock(n, acquired); return RANT_ERR_STATE; }
    n->svc_stop = 0;
    n->svc_running = 1;
    if (!i_rant_plat_thread_start(&n->svc, i_rant_node_service, n)){
        n->svc_running = 0;
        i_rant_node_unlock(n, acquired);
        return RANT_ERR_NOSYS;
    }
    i_rant_node_unlock(n, acquired);                      /* the service takes the lock now */
    return RANT_OK;
#endif
}

int rant_node_stop(RantNode *n){
#ifndef RANT_THREADS
    (void)n;
    return RANT_OK;                                       /* nothing to stop, idempotent */
#else
    int acquired;
    if (!n) return RANT_ERR_STATE;
    acquired = i_rant_node_lock(n);
    if (!acquired) return RANT_ERR_STATE;                 /* from a callback (it is the service) */
    if (n->svc_joining){
        /* another thread owns the join: wait for it rather than join twice. Counted as
           a cv waiter so its broadcasts land */
        n->cv_waiters++;
        while (n->svc_running) i_rant_node_cv_wait(n, 100000u);
        n->cv_waiters--;
        i_rant_node_unlock(n, acquired);
        return RANT_OK;
    }
    if (!n->svc_running){ i_rant_node_unlock(n, acquired); return RANT_OK; }
    n->svc_joining = 1;
    n->svc_stop = 1;
    i_rant_node_kick(n);
    i_rant_node_unlock(n, acquired);
    i_rant_plat_thread_join(&n->svc);                     /* never joined holding the lock */
    acquired = i_rant_node_lock(n);
    n->svc_running = 0;
    n->svc_stop = 0;
    n->svc_joining = 0;
    /* free every waiter: blocked senders proceed unwaited, a parked stopper returns */
    if (n->cv_waiters) i_rant_plat_cond_broadcast(&n->cv);
    i_rant_node_unlock(n, acquired);
    return RANT_OK;
#endif
}

int rant_node_is_started(RantNode *n){
#ifndef RANT_THREADS
    (void)n;
    return 0;
#else
    return (n && n->svc_running) ? 1 : 0;
#endif
}

void rant_node_lock(RantNode *n){
#ifndef RANT_THREADS
    (void)n;
#else
    if (!n) return;
    if (n->lock_held && n->lock_owner == i_rant_plat_thread_id()) return;    /* already ours */
    i_rant_node_lock_raw(n);
    n->user_locked = 1;
#endif
}

void rant_node_unlock(RantNode *n){
#ifndef RANT_THREADS
    (void)n;
#else
    if (!n || !n->user_locked) return;   /* only releases what rant_node_lock took */
    if (!(n->lock_held && n->lock_owner == i_rant_plat_thread_id())) return;
    n->user_locked = 0;
    i_rant_node_unlock_raw(n);
#endif
}

int rant_node_close(RantNode *n, int send_bye){
    RantAllocator pool;
    if (!n) return RANT_OK;
    /* a dispatch in progress, on this thread or another, still views a ring: refuse loudly */
    if (n->dispatching) return RANT_ERR_STATE;
#ifdef RANT_THREADS
    /* from a callback: refuse loudly, the return code lets a binding keep its handle alive */
    if (n->lock_held && n->lock_owner == i_rant_plat_thread_id()) return RANT_ERR_STATE;
    rant_node_stop(n);
    /* from here no other thread is inside, or may enter, any call on this node */
#endif
    /* settle outstanding pattern promises while the node is fully alive, cleared first so
       a callback closing the node cannot recurse */
    if (n->sys_on_close){
        i_RantSysCloseFn f = n->sys_on_close;
        n->sys_on_close = NULL;
        f(n->sys_user);
    }
    /* discovery frees peer blobs via our hook, so it must close before the pool is copied out */
    if (n->discovery) rant_discovery_close(n->discovery, send_bye);
    if (n->fd != RANT_SOCK_BAD) i_rant_plat_close(n->fd);
#ifdef RANT_SHM
    if (n->shm_capable){                              /* the pool reset unmaps nothing */
        uint32_t i, n_segments = (uint32_t)n->shm_n_topics * RANT_SHM_N_CLASSES;
        for (i=0;i<n_segments;i++)
            if (n->shm_pool[i]) i_rant_shm_detach((i_RantShmPool*)n->shm_pool[i]);
        for (i=0;i<n->shm_reader_count;i++)
            if (n->shm_reader_states[i]) i_rant_shm_detach((i_RantShmPool*)n->shm_reader_states[i]);
    }
#endif
#ifdef RANT_THREADS
    if (n->waker.fd != RANT_SOCK_BAD) i_rant_plat_waker_close(&n->waker);
    i_rant_plat_cond_destroy(&n->cv);
    i_rant_plat_mutex_destroy(&n->mu);
#endif
    i_rant_plat_cleanup();
    pool = n->pool;                  /* copied out last, the reset frees n itself */
    rant_allocator_reset(&pool);     /* frees the node struct, arena, buffers, schemas and handles */
    return RANT_OK;
}
