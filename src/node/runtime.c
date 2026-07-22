/* NODE runtime: owns the data sockets, drives discovery, and the clock; it wires
 * peers into the transport via the sans-IO node core (node/core.h). Topics are
 * created at runtime and handed back as opaque handles. All OS access goes through
 * dart_plat. See node/runtime.h for the public dart_node_* / dart_topic_* API. */

#include "runtime.h"   /* public API + config structs */
#include "core.h"      /* sans-IO node core: peer table + discovery->transport lifecycle */
#include "../discovery/runtime.h"
#include "../platform/core.h"
#ifdef DART_SHM
#include "../shm/core.h"
#endif
#include "../common/arena.h"
#include <string.h>    /* heap access goes through i_dart_plat_realloc (no <stdlib.h> here) */
#include <stdarg.h>    /* dart_node_log's printf-style formatting */
#include <stdio.h>     /* vsnprintf/snprintf (log text only, never the data path) */

#ifndef DART_NO_PATTERNS
struct DartNode;       /* patterns/core.c hosts the @dart/meta endpoint; open calls this seam */
void i_dart_patterns_meta_open(struct DartNode *n);
#endif

/* Consumer-queue ring record: header, then the publisher name, then the payload at an
 * 8-aligned offset. Records never wrap: a tail-end too small for the next record holds a
 * DART__QWRAP sentinel (or nothing, if smaller than a header) and the record starts at 0. */
typedef struct {
    uint32_t rec_bytes;    /* whole record, 8-aligned; DART__QWRAP = wrap sentinel. FIRST:
                              the ring reads it at offset 0 for the sentinel test */
    uint32_t data_len;
    uint64_t t_recv_us;    /* poll-side arrival stamp, surfaced as DartMsg.recv_us */
    uint32_t publisher_id;
    uint8_t  name_len;     /* publisher name copied inline (discovery views die with the peer) */
    uint8_t  pad[3];
} i_DartQRec;
#define DART__QWRAP 0xFFFFFFFFu
#define DART__QALIGN(x) (((uint32_t)(x) + 7u) & ~7u)

/* One topic's consumer queue (runtime.h "consumer queues"): a byte ring filled by the
 * poll thread at delivery and drained by take/dispatch on the consumer's thread. All
 * access is under the node lock; the ring and this struct are stable pool allocations
 * (they survive arena grows and are freed by the close-time pool reset). */
typedef struct {
    uint8_t  *buf;
    uint32_t  cap;         /* current ring bytes (8-aligned); grows on demand */
    uint32_t  cap_limit;   /* growth bound: qos.queue_bytes or DART_QUEUE_CAP */
    uint32_t  head, tail;  /* byte offsets; head == tail means empty iff count == 0 */
    uint32_t  bytes;       /* queued record bytes (stats; excludes wrap padding) */
    uint32_t  count;       /* queued records, INCLUDING one being viewed */
    uint32_t  dropped;     /* best-effort records overwritten/refused since open */
    uint8_t   viewing;     /* the record at tail is the consumer's live take view */
    uint8_t   busy;        /* inside dispatch's unlocked callback window (no reentry) */
    uint8_t   reliable;    /* full-queue policy: park (reliable) vs overwrite (BE) */
    uint8_t   parked;      /* transport lanes parked on this topic: retry as we drain */
} i_DartMsgQueue;

struct DartTopic {   /* schema: node-owned copy */
    DartNode *n; uint16_t index; DartSchema *schema;
    i_DartMsgQueue *q;                  /* consumer queue (NULL = inline callbacks) */
    i_DartSysMsgFn sys_on_message;      /* patterns layer: routes this topic's deliveries here
                                           instead of the app on_message (NULL = normal topic) */
    void    *sys_msg_user;
    uint64_t tx_msgs, tx_bytes;         /* messages/bytes committed by our sends (dart_topic_counts) */
    uint64_t rx_msgs, rx_bytes;         /* messages/bytes delivered to us (accepted, parked excluded) */
    uint32_t resolve_epoch;             /* match-wait memo: node->match_epoch at which this topic
                                           last computed "converged, nothing unresolved", so a
                                           steady-state zero-subscriber send never re-walks the
                                           peers (0 = never computed; the epoch starts at 1) */
    uint8_t  prefix_bytes;              /* pattern-header bytes split off the front of each
                                           delivered payload into DartMsg.header (0 = plain topic) */
    uint8_t  kind;                      /* DartTopicKind, mirrored from the def (reflection) */
    uint8_t  role;                      /* DartRole, mirrored at create + set_role (reflection) */
    uint8_t  name_len;                  /* stable topic-name copy: queued DartMsg views
                                           must not point into the (relocatable) arena */
    char     name[DART_TOPIC_NAME_MAX];
};

/* One pending error->log mirror entry (opts.log_errors). Errors fire deep inside RX and
 * delivery processing, where a send may not re-enter the transport mid-datagram, so
 * i_dart_node_emit only RECORDS the event here (its text formatted immediately: the
 * event's views die with the callback) and the poll pass publishes the ring at a safe
 * point. A burst of the same (error, topic, peer) coalesces into a count; a full ring
 * bumps a dropped counter that the next flush summarizes: never silent, never unbounded. */
#define DART_LOG_PEND 8
typedef struct {
    uint64_t wall_us, mono_us;   /* when the FIRST occurrence fired */
    uint32_t peer, count;
    uint16_t topic;
    uint8_t  error;              /* DartErrorKind */
    char     text[192];
} i_DartLogPend;

struct DartNode {
    DartTransportState     *transport;
    i_DartNodeCore *core;     /* peer table (id<->address) + discovery lifecycle (sans-IO) */
    DartDiscovery     *discovery;
    i_DartSock       fd;       /* unicast data socket */
    uint16_t      domain;
    DartNodeNet  net;         /* copy of opts.net: socket buffers + discovery addressing */
    /* detail-exchange retry cadence: while any peer holds unverified candidates, the
       queued requests drain each poll and a periodic rearm sweep (one announce interval)
       re-asks, so a lost datagram heals; once converged the sweep sends nothing and the
       timer disarms until new candidates appear. */
    uint32_t      announce_us;     /* resolved discovery announce interval */
    uint64_t      next_detail_us;  /* next retry sweep; 0 = disarmed */
    /* datagram the socket refused; retried first next poll so it is never lost */
    uint8_t       tx_hold[DART_DGRAM_MAX];
    size_t        tx_hold_len;
    uint32_t      tx_hold_peer;
    /* data-socket RX buffer (in the arena): unicast discovery announces land on the data
       port too (solicit replies carry the full blob), so it is sized for the larger of a
       transport datagram and the biggest discovery datagram this node accepts. */
    uint8_t      *rx_buf;
    size_t        rx_buf_bytes;
    /* backpressure accumulators, read via dart_node_backpressure_stats */
    uint64_t      backpressure_total_us;
    uint32_t      backpressure_wait_count;
    uint32_t      evicted_unsent;  /* DART_E_EVICTED_UNSENT count (dart_node_evicted_unsent) */
#ifdef DART_THREADS
    /* thread safety + the optional service thread (runtime.h "Threading"). The lock is
       never held across a blocking wait: the poller drops it around the socket poll and
       any thread that mutates state kicks the waker so the change is serviced now. */
    i_DartMutex   mu;             /* THE node lock: every public entry point takes it */
    i_DartCond    cv;             /* senders/drainers waiting on the poller's progress */
    uint32_t      cv_waiters;     /* under mu; broadcast skipped when 0 */
    i_DartWaker   waker;          /* interrupts the unlocked socket wait */
    i_DartThread  svc;
    volatile uint8_t svc_running; /* a service thread is alive (dart_node_start) */
    volatile uint8_t svc_stop;    /* stop requested; the service loop exits on it */
    uint8_t       svc_joining;    /* under mu: one stopper owns the join; others wait on cv */
    uint32_t      pollers_sleeping; /* under mu: pollers in (or headed into) the unlocked wait */
    uint8_t       wake_signaled;  /* under mu: coalesce a burst into one waker datagram */
    uint8_t       user_locked;    /* the public dart_node_lock is held (pairs its unlock) */
    uint64_t      work_seq;       /* completed work passes (the unsent-wait predicate) */
    volatile uint8_t  lock_held;  /* owner-reentrancy check: only the owner thread ever */
    volatile uint64_t lock_owner; /* stores its own id, and zeroes it before releasing */
#endif
    /* diagnostic: in-pump probe sampled inside the backpressure wait (dart_topic_send) */
    DartPumpProbeFn pump_probe;
    void          *pump_probe_user;
    uint64_t       pump_probe_interval_us;
    /* app callbacks: one message sink (wrapped to build a DartMsg) + everything-else */
    DartMsgFn    user_on_message;
    DartEventFn  on_event;
    void          *user_data;
    /* patterns layer (src/patterns): a node-wide event observer + a per-poll tick, and
       (per-topic) message routing via DartTopic.sys_on_message. All optional. */
    i_DartSysEventFn sys_on_event;
    i_DartSysTickFn  sys_tick;
    i_DartSysCloseFn sys_on_close;
    void            *sys_user;
    uint64_t         sys_tick_next;   /* the tick's returned next deadline, folded into the poll wait */
    uint64_t         settle_topology_us; /* last PEER_UP/DOWN/INTEREST change (dart_node_settle) */
    /* the send-path match wait (runtime.h "MATCH WAIT"): bound, the post-open gather latch
       (anchored at open_us; discovery solicits on startup so existing peers answer within an
       RTT), and the epoch that invalidates each topic's converged memo whenever the peer/
       interest topology (or our own topic set) changes. */
    uint32_t         match_wait_us;   /* 0 = disabled (opts.match_wait_ms < 0) */
    uint64_t         open_us;         /* when the node opened (the gather predicate's anchor) */
    uint8_t          gather_done;     /* latched once the post-open gather settles */
    uint32_t         match_epoch;     /* bumped on peer/interest change + create/set_role */
    void            *patterns;        /* the patterns layer's per-node manager (lazily created); the
                                         node treats it opaquely and its memory rides the pool reset */
    DartEvent     last_error;  /* most recent DART_ERROR (dart_last_error(n)); DART_E_NONE until one fires */
    /* built-in @dart/log topics (runtime.h "logs") + the error->log mirror ring */
    DartTopic    *log_topics[3];       /* [DartLogLevel]; all NULL under opts.disable_logs */
    DartSchema   *log_schema;          /* DartLog { wall_us, mono_us, text } (node-owned) */
    uint8_t       log_errors;          /* opts.log_errors: mirror DART_ERROR onto @dart/log/error */
    uint8_t       log_flushing;        /* reentrancy guard: an error fired while publishing a
                                          mirrored line must not re-enter the ring */
    uint8_t       log_pend_n;
    uint32_t      log_pend_dropped;    /* ring overflow between flushes (summarized, never silent) */
    i_DartLogPend log_pend[DART_LOG_PEND];
    /* @dart/meta snapshot scratch (i_dart_node_snapshot), grown on demand */
    uint8_t      *snap_buf; uint32_t snap_cap;
    char          name[DART_NODE_NAME_MAX + 1];   /* our advertised node name (the snapshot's) */
    uint8_t       name_len;
    void          *arena;      /* control-structs block (a freeable pool allocation); relocated on grow */
    /* the node's paged region allocator (common/alloc.h): backs the node struct, arena, message
       buffers, schemas, and handles. COPIED from the caller's allocator at open (so the caller's
       may be a temporary); the node owns it and resets it on close. */
    DartAllocator pool;
    uint8_t        alloc_dynamic;
    uint8_t        grow_pending;   /* a peer was refused for lack of slots; grow at next poll */
    uint16_t       max_peers;      /* current peer-table capacity (doubles on a dynamic grow) */
    /* the metadata accept bound (largest peer announce blob we store/receive). Starts at
       dart_meta_cap(max_topics) and SELF-HEALS: a peer whose blob exceeds it fires
       META_TOO_BIG with the needed size, we grow the bound + RX buffers at the next poll
       and solicit a re-announce, so a big-topology peer and a default node interoperate
       with no configuration. meta_grow_need is the pending request; meta_grow_failed
       remembers a size that would not allocate, so the retry loop surfaces instead of
       silently spinning. */
    uint16_t       meta_cap;
    uint16_t       meta_grow_need;
    uint16_t       meta_grow_failed;
    /* topic handles. handles is a pointer array in the arena; each DartTopic struct
       is a separate stable allocation, so a grow that relocates the arena never moves a
       handle the user holds. */
    DartTopic **handles;    /* [max_topics] -> stable per-topic structs */
    uint16_t      max_topics;
    uint16_t      n_created;       /* USER topics created (dense indices from 0, stepping
                                      over the builtin block once they reach it) */
    /* the BUILT-IN topics (the @dart/log levels, the @dart/meta channels) live at the TOP
       of the original reserve, [builtin_lo, builtin_lo + n_builtin), so user topics keep
       the dense 0-based indices apps and tests key on. The hole between the user region and
       the block rides the announce as INACTIVE entries (positional indices stay stable). */
    uint16_t      builtin_lo;      /* first builtin index (= the app's max_topics budget) */
    uint16_t      n_builtin;
    uint8_t       creating_builtin;/* open is creating builtins: allocate from the block */
#ifdef DART_SHM
    /* zero-fragment same-host path: lazy per-size-class segments (see shm/core.h) */
    uint8_t        shm_capable;       /* always 1: the node always has an allocator */
    uint8_t        shm_host[16];  /* our host uuid (advertised; same-host check) */
    uint64_t       shm_base;      /* per-node segment id base; low 19 bits = (topic<<3)|class */
    void          *shm_scratch; uint32_t shm_scratch_cap;   /* one-copy receive scratch */
    void         **shm_pool;      /* [n_topics*N_CLASSES] our (topic,class) segments (NULL = not
                                     created); each created segment's state is a stable hook
                                     allocation, made when the segment is (lazily) created */
    uint16_t       shm_n_topics;
    /* reader-side attach cache, hook-allocated and grown on demand: sized by segments
       actually attached (same-host peers x their publishing topics), not the worst-case
       peers x topics x classes. A node with no same-host peer allocates none of it. */
    uint64_t      *shm_reader_segments;  /* [shm_reader_cap] attached segment ids */
    void         **shm_reader_states;    /* [shm_reader_cap] their pool states (stable allocations) */
    uint16_t       shm_reader_cap, shm_reader_count;
    uint32_t       shm_tx, shm_rx;/* messages published / delivered via SHM (observability) */
#endif
};

/* The node's message-buffer allocator, handed to the transport as its realloc hook and used
 * for schemas and topic handles (u is the node). It delegates to the node's paged region
 * allocator: one freeable allocation per call, so free/resize work and reset reclaims all. */
static void *i_dart_node_alloc(void *u, void *ptr, size_t size){
    return dart_allocator_alloc(&((DartNode*)u)->pool, ptr, size);
}

#ifdef DART_THREADS
/* Raw lock ops that keep the owner-id bookkeeping consistent. lock_owner is zeroed
   BEFORE the release so a non-owner reading the pair unlocked can never observe
   lock_held with its own id (it is the only thread that ever writes that id). */
static void i_dart_node_lock_raw(DartNode *n){
    i_dart_plat_mutex_lock(&n->mu);
    n->lock_owner = i_dart_plat_thread_id();
    n->lock_held = 1;
}
static void i_dart_node_unlock_raw(DartNode *n){
    n->lock_owner = 0;
    n->lock_held = 0;
    i_dart_plat_mutex_unlock(&n->mu);
}
/* Reentrant-aware entry lock: 1 = this call acquired mu (unlock releases it), 0 = the
   calling thread already held it (a callback calling back in, or the pump's nested
   poll). Reentrant callers proceed without waiting anywhere. */
static int i_dart_node_lock(DartNode *n){
    if (n->lock_held && n->lock_owner == i_dart_plat_thread_id()) return 0;
    i_dart_node_lock_raw(n);
    return 1;
}
static void i_dart_node_unlock(DartNode *n, int acquired){
    if (acquired) i_dart_node_unlock_raw(n);
}
/* With mu held, before unlocking: cut the pollers' blocking waits short so the state
   change (a queued send, a new advertisement, a stop) is serviced now, not at the
   next tick. One datagram wakes every sleeping poller (they all poll the same waker
   fd; the first to re-lock drains it), and the burst coalesces to one datagram per
   sleep. Armed only when the signal actually went out, so a failed loopback send
   can never wedge the coalescing. */
static void i_dart_node_kick(DartNode *n){
    if (n->pollers_sleeping && !n->wake_signaled && i_dart_plat_waker_signal(&n->waker))
        n->wake_signaled = 1;
}
/* Condvar wait that keeps the owner bookkeeping honest: mu is released inside the
   wait (another thread will take it and zero/claim the fields), so re-stamp ownership
   after it is reacquired. Anything cached from inside the node across this call is
   invalid: the poller may have grown/relocated the arena while we slept. */
static void i_dart_node_cv_wait(DartNode *n, uint64_t timeout_us){
    n->lock_owner = 0;
    n->lock_held = 0;
    i_dart_plat_cond_wait(&n->cv, &n->mu,
                          timeout_us > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)timeout_us);
    n->lock_owner = i_dart_plat_thread_id();
    n->lock_held = 1;
}
#else
static int  i_dart_node_lock(DartNode *n){ (void)n; return 1; }
static void i_dart_node_unlock(DartNode *n, int acquired){ (void)n; (void)acquired; }
static void i_dart_node_kick(DartNode *n){ (void)n; }
#endif /* DART_THREADS */

/* --- error reporting --------------------------------------------------------------
 * One path for every runtime-side event: stamp the app's user_data, capture a DART_ERROR
 * into the node's last-error slot (so dart_last_error(n) can report it even with no
 * on_event set), then hand it to on_event. Zero cost on the success path: only errors and
 * the (already rare) lifecycle events ever reach here. */
/* one past the highest DEFINED topic index: the iteration/lookup bound over handles[]
 * (entries between the user region and the builtin block are NULL holes) */
static uint16_t i_dart_node_topic_hi(DartNode *n){
    uint16_t hi = n->n_created;
    if (n->n_builtin){
        if (hi >= n->builtin_lo) hi = (uint16_t)(hi + n->n_builtin);   /* users grew past the block */
        if ((uint16_t)(n->builtin_lo + n->n_builtin) > hi) hi = (uint16_t)(n->builtin_lo + n->n_builtin);
    }
    return hi;
}

static int i_dart_node_is_log_topic(DartNode *n, uint16_t topic_index){
    DartTopic *h = (topic_index < i_dart_node_topic_hi(n)) ? n->handles[topic_index] : NULL;
    return h && (h == n->log_topics[0] || h == n->log_topics[1] || h == n->log_topics[2]);
}

static void i_dart_node_emit(DartNode *n, DartEvent *e){
    e->user = n->user_data;
    if (e->peer){    /* resolve the peer's node name once here, so every event message can print
                        a human label instead of an opaque id (dart_event_str prefers it) */
        DartString nm = i_dart_node_core_peer_name(n->core, e->peer);
        e->peer_name = nm.data;   /* NUL-terminated view (discovery state); NULL if the id is unknown */
    }
    if (e->kind == DART_ERROR) n->last_error = *e;
    /* the error->log mirror (opts.log_errors): RECORD only; the poll pass publishes the
       ring at a safe point (i_dart_node_log_flush). Errors scoped to a log topic itself
       are excluded, and nothing is recorded while a flush publishes (reentrancy). */
    if (e->kind == DART_ERROR && n->log_errors && !n->log_flushing
        && !(e->topic_name && i_dart_node_is_log_topic(n, e->topic))){
        uint8_t i;
        for (i = 0; i < n->log_pend_n; i++)
            if (n->log_pend[i].error == (uint8_t)e->error && n->log_pend[i].topic == e->topic
                && n->log_pend[i].peer == e->peer) break;
        if (i < n->log_pend_n) n->log_pend[i].count++;
        else if (n->log_pend_n < DART_LOG_PEND){
            i_DartLogPend *p = &n->log_pend[n->log_pend_n++];
            p->error = (uint8_t)e->error; p->topic = e->topic; p->peer = e->peer; p->count = 1;
            p->wall_us = i_dart_plat_wall_us(); p->mono_us = i_dart_plat_now_us();
            dart_event_str(e, p->text, sizeof p->text);
        } else n->log_pend_dropped++;
    }
    if (e->kind == DART_PEER_UP || e->kind == DART_PEER_DOWN || e->kind == DART_PEER_INTEREST){
        n->settle_topology_us = i_dart_plat_now_us();   /* dart_node_settle's quiet-window clock */
        n->match_epoch++;                               /* invalidate the topics' converged memos */
    }
    if (n->sys_on_event) n->sys_on_event(n->sys_user, e);   /* patterns layer observes peer up/down, etc. */
    if (n->on_event) n->on_event(e);
}

/* our topic's name as a C string (the name pool is NUL-terminated), or NULL if the
 * topic is undefined: the topic_name view carried on topic-scoped events. */
static const char *i_dart_node_topic_name(DartNode *n, uint16_t topic_index){
    DartString s = dart_transport_topic_name(n->transport, topic_index);
    return (const char*)s.data;
}

/* Split a delivered wire payload into its pattern header (the first prefix_bytes) and the
 * user payload after it. A plain topic (or a wire shorter than the prefix) yields an empty
 * header and the whole payload, so plain delivery is unchanged. */
static void i_dart_node_split(DartTopic *h, DartBytes wire, DartBytes *hdr, DartBytes *payload){
    uint8_t pfx = (h && (uint32_t)wire.len >= h->prefix_bytes) ? h->prefix_bytes : 0u;
    hdr->data = pfx ? wire.data : NULL; hdr->len = pfx;
    payload->data = wire.data + pfx; payload->len = wire.len - pfx;
}

/* ---- consumer-queue ring (runtime.h "consumer queues") ------------------------------ */

/* oldest record, with the wrap normalized into tail; NULL when empty */
static const i_DartQRec *i_dart_q_peek(i_DartMsgQueue *q){
    if (!q->count) return NULL;
    if (q->cap - q->tail < (uint32_t)sizeof(i_DartQRec) ||
        ((const i_DartQRec*)(q->buf + q->tail))->rec_bytes == DART__QWRAP)
        q->tail = 0;
    return (const i_DartQRec*)(q->buf + q->tail);
}

/* drop the oldest record (never called on a viewed one: the view sits at tail) */
static void i_dart_q_pop(i_DartMsgQueue *q){
    const i_DartQRec *rec = i_dart_q_peek(q);
    if (!rec) return;
    q->tail += rec->rec_bytes;
    q->bytes -= rec->rec_bytes;
    if (--q->count == 0){ q->head = 0; q->tail = 0; }
}

/* contiguous space for `need` bytes; fills *at and pre-writes the wrap sentinel */
static int i_dart_q_fit(i_DartMsgQueue *q, uint32_t need, uint32_t *at){
    if (!q->buf || need > q->cap) return 0;
    if (q->count == 0){ q->head = 0; q->tail = 0; *at = 0; return 1; }
    if (q->head > q->tail){
        if (q->cap - q->head >= need){ *at = q->head; return 1; }
        if (q->tail >= need){                                    /* wrap to the start */
            if (q->cap - q->head >= 4u)
                ((i_DartQRec*)(q->buf + q->head))->rec_bytes = DART__QWRAP;
            *at = 0; return 1;
        }
        return 0;
    }
    if (q->head < q->tail && q->tail - q->head >= need){ *at = q->head; return 1; }
    return 0;                                                    /* full (head == tail) */
}

/* relinearize into a bigger ring: doubles toward cap_limit (a single over-cap message
 * still fits: the cap yields to it). 0 = cannot grow now (at cap, OOM, or a live take
 * view pins the ring; the caller falls back to its full-queue policy). */
static int i_dart_q_grow(DartNode *n, i_DartMsgQueue *q, uint32_t need_total, uint32_t need_one){
    uint32_t target = q->cap ? q->cap * 2u : 4096u;
    uint8_t *nb;
    if (q->viewing) return 0;
    if (target < 4096u) target = 4096u;
    while (target < need_total && target < 0x80000000u) target *= 2u;
    if (target > q->cap_limit) target = q->cap_limit;
    if (target < need_one) target = DART__QALIGN(need_one);      /* one message always fits */
    if (target <= q->cap) return 0;
    nb = (uint8_t*)i_dart_node_alloc(n, NULL, target);
    if (!nb) return 0;
    {   uint32_t off = 0, i, src = q->tail;                        /* compact, oldest first */
        for (i = 0; i < q->count; i++){
            const i_DartQRec *rec;
            if (q->cap - src < (uint32_t)sizeof(i_DartQRec) ||
                ((const i_DartQRec*)(q->buf + src))->rec_bytes == DART__QWRAP) src = 0;
            rec = (const i_DartQRec*)(q->buf + src);
            memcpy(nb + off, rec, rec->rec_bytes);
            off += rec->rec_bytes; src += rec->rec_bytes;
        }
        q->tail = 0; q->head = off;
    }
    if (q->buf) i_dart_node_alloc(n, q->buf, 0);
    q->buf = nb; q->cap = target;
    return 1;
}

/* best-effort queue loss is loss like any other: DART_MSG_LOST, never silent */
static void i_dart_node_queue_lost(DartNode *n, uint16_t topic_index, uint32_t from, uint32_t count){
    DartEvent e; memset(&e, 0, sizeof e);
    e.kind = DART_MSG_LOST; e.topic = topic_index; e.peer = from;
    e.topic_name = i_dart_node_topic_name(n, topic_index);
    e.lost_count = count;
    i_dart_node_emit(n, &e);
}

/* enqueue one delivered message. 0 = accepted (stored, or dropped per the best-effort
 * contract), 1 = refused (reliable at cap: the transport parks and the writer's flow
 * control backpressures the publisher). */
static int i_dart_node_queue_push(DartNode *n, uint16_t topic_index, i_DartMsgQueue *q,
                                  uint32_t from, DartBytes data){
    DartString name = i_dart_node_core_peer_name(n->core, from);
    uint32_t name_len = name.len > DART_NODE_NAME_MAX ? (uint32_t)DART_NODE_NAME_MAX
                                                      : (uint32_t)name.len;
    uint32_t payload_off = DART__QALIGN(sizeof(i_DartQRec) + name_len);
    uint32_t need = DART__QALIGN(payload_off + data.len);
    uint32_t at = 0, evicted = 0;
    for (;;){
        if (i_dart_q_fit(q, need, &at)) break;
        if (i_dart_q_grow(n, q, q->bytes + need, need)) continue;
        if (q->reliable){ q->parked = 1; return 1; }
        if (!q->viewing && q->count){                        /* overwrite oldest (KEEP_LAST) */
            i_dart_q_pop(q); q->dropped++; evicted++; continue;
        }
        q->dropped++;                     /* a live view pins the ring: drop the incoming */
        i_dart_node_queue_lost(n, topic_index, from, evicted + 1u);
        return 0;
    }
    {   i_DartQRec *rec = (i_DartQRec*)(q->buf + at);
        rec->rec_bytes = need; rec->data_len = (uint32_t)data.len;
        rec->t_recv_us = i_dart_plat_now_us();
        rec->publisher_id = from; rec->name_len = (uint8_t)name_len;
        rec->pad[0] = rec->pad[1] = rec->pad[2] = 0;
        if (name_len) memcpy((uint8_t*)rec + sizeof *rec, name.data, name_len);
        if (data.len) memcpy(q->buf + at + payload_off, data.data, data.len);
        q->head = at + need;
        q->bytes += need; q->count++;
    }
    if (evicted) i_dart_node_queue_lost(n, topic_index, from, evicted);
    return 0;
}

/* a queued DartMsg: every view points at stable memory (the ring record, the handle's
 * name copy), valid until the next take/dispatch on this topic. The decode schema is
 * re-resolved now rather than stored: the delivery map may repoint between queue and
 * take, and the record must never outlive a pointer into it. */
static void i_dart_node_queue_msg(DartNode *n, DartTopic *h, const i_DartQRec *rec, DartMsg *m){
    memset(m, 0, sizeof *m);
    m->node = n; m->user = n->user_data;
    m->topic_index = h->index;
    m->publisher_id = rec->publisher_id;
    m->publisher_name = rec->name_len ? dart_string((const char*)(rec + 1), rec->name_len)
                                   : dart_cstr("unknown-peer");
    m->topic_name = dart_string(h->name, h->name_len);
    i_dart_node_split(h, dart_bytes((const uint8_t*)rec + DART__QALIGN(sizeof *rec + rec->name_len),
                         rec->data_len), &m->header, &m->data);
    m->schema = i_dart_node_core_msg_schema(n->core, rec->publisher_id, h->index);
    if (h->prefix_bytes && m->data.len == 0) m->schema = NULL;   /* op-only pattern message */
    m->recv_us = rec->t_recv_us;
}

/* create the queue (qos.queue_bytes at topic create, or lazily on first take/dispatch).
 * An explicit queue_bytes allocates in full (deterministic); the lazy default starts at
 * one page and grows on demand toward DART_QUEUE_CAP. NULL on OOM. */
static i_DartMsgQueue *i_dart_node_queue_ensure(DartNode *n, DartTopic *h, const DartQos *qos){
    i_DartMsgQueue *q = h->q;
    uint32_t limit, initial;
    if (q) return q;
    if (!qos) qos = dart_transport_topic_qos(n->transport, h->index);
    limit = DART__QALIGN((qos && qos->queue_bytes) ? qos->queue_bytes : DART_QUEUE_CAP);
    initial = (qos && qos->queue_bytes) ? limit : (limit < 4096u ? limit : 4096u);
    q = (i_DartMsgQueue*)i_dart_node_alloc(n, NULL, sizeof *q);
    if (!q) return NULL;
    memset(q, 0, sizeof *q);
    q->buf = (uint8_t*)i_dart_node_alloc(n, NULL, initial);
    if (!q->buf){ i_dart_node_alloc(n, q, 0); return NULL; }
    q->cap = initial; q->cap_limit = limit;
    q->reliable = (qos && qos->reliability == DART_RELIABLE) ? 1u : 0u;
    h->q = q;
    return q;
}

/* finish an outstanding take view, then retry any parked transport lanes into the space
 * it freed (their acks need a TX pass: kick the poller). Lock held. */
static void i_dart_node_queue_release(DartNode *n, DartTopic *h, i_DartMsgQueue *q){
    if (q->viewing){
        q->viewing = 0;
        i_dart_q_pop(q);
    }
    if (q->parked){
        q->parked = dart_transport_deliver_parked(n->transport, h->index,
                                                  i_dart_plat_now_us()) ? 1u : 0u;
        i_dart_node_kick(n);
    }
}

/* The process-global last-error slot, for failures during dart_node_open where the node
 * does not exist yet (or is half-built). Best-effort: a plain global with no lock,
 * meaningful right after a failed open on the calling thread (open is rare; runtime
 * errors live in the per-node slot and never touch this). */
static DartEvent g_last_error;

/* Report an open-time failure: fill a DART_ERROR event, store it in the global slot, and
 * fire the caller's on_event directly (the node handle does not exist yet). Returns NULL
 * so a failing open can `return i_dart_node_open_fail(...)`. */
static DartNode *i_dart_node_open_fail(DartEventFn on_event, void *user, DartErrorKind err,
                            int os_error, uint16_t port, uint64_t need){
    DartEvent e; memset(&e, 0, sizeof e);
    e.kind = DART_ERROR; e.error = err; e.user = user;
    e.os_error = os_error; e.port = port; e.too_big_bytes = need;
    g_last_error = e;
    if (on_event) on_event(&e);
    return NULL;
}

DartEvent dart_last_error(DartNode *n){ return n ? n->last_error : g_last_error; }

/* build a DartMsg and hand it to the app: inline on_message, or a copy into the
 * topic's consumer queue when one exists (the topic name is a local lookup, never
 * on the wire). Shared by the inline and SHM delivery paths. Returns 0 = accepted,
 * nonzero = refused (reliable consumer queue at cap: the transport parks the message).
 * A message that does not fit its publisher's declared schema broke the publisher's own
 * contract: dropped + surfaced (accepted), never handed to the app to misdecode. */
static int i_dart_node_deliver(DartNode *n, uint16_t topic_index, uint32_t from, DartBytes data){
    DartMsg m;
    DartTopic *h = (topic_index < i_dart_node_topic_hi(n)) ? n->handles[topic_index] : NULL;
    DartBytes hdr, payload;
    const DartSchema *schema = i_dart_node_core_msg_schema(n->core, from, topic_index);
    i_dart_node_split(h, data, &hdr, &payload);              /* schema validates the payload, not the prefix */
    /* a ZERO-LENGTH payload on a prefix-carrying (pattern) channel is an op-only message
       (a variable's unforce, an empty ack): the op byte is the content, so the payload
       schema does not apply; the pattern layer judges it. Plain topics are unaffected. */
    if (h && h->prefix_bytes && payload.len == 0) schema = NULL;
    if (schema && !dart_schema_validate(schema, payload)){
        DartEvent e; memset(&e, 0, sizeof e);
        e.kind = DART_ERROR; e.error = DART_E_SCHEMA_MISMATCH;
        e.peer = from; e.topic = topic_index; e.topic_name = i_dart_node_topic_name(n, topic_index);
        e.schema_detail = i_dart_node_core_note_size_mismatch(n->core, from, topic_index,
                                            payload.len, dart_schema_msg_min(schema));
        i_dart_node_emit(n, &e);
        return 0;
    }
    if (h && h->q){
        if (i_dart_node_queue_push(n, topic_index, h->q, from, data))   /* store the full wire */
            return 1;   /* parked: the accepted retry re-counts */
        h->rx_msgs++; h->rx_bytes += data.len;
        return 0;
    }
    if (h){ h->rx_msgs++; h->rx_bytes += data.len; }
    if (!(h && h->sys_on_message) && !n->user_on_message) return 0;
    memset(&m, 0, sizeof m);
    m.node = n; m.user = n->user_data;
    m.topic_index = topic_index; m.publisher_id = from;
    m.publisher_name = i_dart_node_core_peer_name(n->core, from);          /* view into discovery state */
    if (!m.publisher_name.data) m.publisher_name = dart_cstr("unknown-peer"); /* .data never NULL on delivery */
    m.topic_name = dart_transport_topic_name(n->transport, topic_index);
    m.header = hdr; m.data = payload;
    m.schema = schema;
    m.recv_us = i_dart_plat_now_us();
    if (h && h->sys_on_message) h->sys_on_message(h->sys_msg_user, &m);    /* patterns layer routing */
    else n->user_on_message(&m);
    return 0;
}
static int i_dart_node_on_message(void *u, uint16_t topic_index, uint32_t from, DartBytes data){
    return i_dart_node_deliver((DartNode*)u, topic_index, from, data);
}
/* node-core events (the app DartEvent: PEER_UP/DOWN/INTEREST/REFUSED) funnel through
 * here; transport events arrive separately via i_dart_node_on_transport_event. The core
 * sets ev->user to this node; swap it for the app's real user_data before handing on. */
static void i_dart_node_on_event(const DartEvent *ev){
    DartNode *n = (DartNode*)ev->user;
    /* dynamic mode has no peer cap: a refusal means grow the table (deferred to the next
       poll, out of this callback); the peer re-announces and is admitted, so the app is
       never told it was refused. Static mode keeps the cap and surfaces the error. */
    if (n->alloc_dynamic && ev->kind == DART_ERROR && ev->error == DART_E_PEER_REFUSED){
        n->grow_pending = 1; return;
    }
    /* same treatment for the accept bound: a peer's blob we cannot hold (or even receive:
       the OS truncated the datagram, but the header carried its true size) is a growth
       signal, not a verdict. Grow at the next poll, then solicit the re-announce. Sizes
       past the wire ceiling, and a size that already failed to allocate, surface. */
    if (n->alloc_dynamic && ev->kind == DART_ERROR && ev->error == DART_E_PEER_META_TOO_BIG){
        uint64_t need = ev->too_big_bytes;
        if (need > n->meta_cap && need <= 65000u && (uint16_t)need != n->meta_grow_failed){
            n->meta_grow_need = (uint16_t)need;
            return;
        }
    }
    { DartEvent e = *ev; i_dart_node_emit(n, &e); }
}

/* the transport's schema gate (DartConfig.schema_check): the node core answers it over
 * the serialize layer, from the peer's schema as its detail response advertised it.
 * u is the node. */
static int i_dart_node_schema_check(void *u, uint32_t peer, uint16_t topic_index,
                                    int peer_is_pub, uint64_t hash, DartBytes wire){
    return i_dart_node_core_schema_check(((DartNode*)u)->core, peer, topic_index,
                                         peer_is_pub, hash, wire);
}

/* transport events arrive as a DartTransportEvent; the node maps them onto its app
 * DartEvent union and hands them on. This is the node combining the two lower layers'
 * events into one app callback (peer events come via the node core, above). */
static void i_dart_node_on_transport_event(const DartTransportEvent *tev){
    DartNode *n = (DartNode*)tev->user;
    DartEvent e;
    memset(&e, 0, sizeof e);
    e.peer = tev->peer; e.topic = tev->topic;
    e.lost_first = tev->lost_first; e.lost_count = tev->lost_count;
    e.too_big_bytes = tev->too_big_bytes; e.identity = tev->identity;
    /* MSG_LOST is a top-level info kind (best-effort loss is expected); everything else
       is a DART_ERROR carrying its specific DartErrorKind. Topic-scoped kinds get our
       topic name for the message string. */
    switch (tev->kind){
    case DART_TRANSPORT_MSG_LOST:
        e.kind = DART_MSG_LOST; e.topic_name = i_dart_node_topic_name(n, tev->topic); break;
    case DART_TRANSPORT_MSG_TOO_BIG:
        e.kind = DART_ERROR; e.error = DART_E_MSG_TOO_BIG; e.topic_name = i_dart_node_topic_name(n, tev->topic); break;
    case DART_TRANSPORT_NAME_COLLISION:
        e.kind = DART_ERROR; e.error = DART_E_NAME_COLLISION; e.topic_name = i_dart_node_topic_name(n, tev->topic); break;
    case DART_TRANSPORT_QOS_INCOMPATIBLE:
        e.kind = DART_ERROR; e.error = DART_E_QOS_INCOMPATIBLE; e.topic_name = i_dart_node_topic_name(n, tev->topic); break;
    case DART_TRANSPORT_KIND_MISMATCH:
        e.kind = DART_ERROR; e.error = DART_E_KIND_MISMATCH; e.topic_name = i_dart_node_topic_name(n, tev->topic); break;
    case DART_TRANSPORT_SCHEMA_MISMATCH:
        e.kind = DART_ERROR; e.error = DART_E_SCHEMA_MISMATCH; e.topic_name = i_dart_node_topic_name(n, tev->topic);
        e.schema_detail = i_dart_node_core_schema_why(n->core, tev->peer, tev->topic,
                                                      tev->peer_is_pub); break;
    case DART_TRANSPORT_INTEREST_OVERFLOW:
        e.kind = DART_ERROR; e.error = DART_E_INTEREST_OVERFLOW; break;
    case DART_TRANSPORT_META_TRUNCATED_INTEREST:
        e.kind = DART_ERROR; e.error = DART_E_META_TRUNCATED_INTEREST; break;
    case DART_TRANSPORT_META_TRUNCATED_SCHEMA:
        e.kind = DART_ERROR; e.error = DART_E_META_TRUNCATED_SCHEMA; break;
    default: return;
    }
    i_dart_node_emit(n, &e);
}

/* The node arena's sub-blocks, laid out in ONE place so the measure pass (bump.base
 * NULL, read bump.offset) and the build pass (read the pointers) run the same i_dart_bump_take
 * sequence and can never drift. */
typedef struct {
    uint8_t   *handles, *node_core, *transport, *discovery, *rx_buf;
#ifdef DART_SHM
    uint8_t   *shm_pool;
#endif
    size_t     node_core_bytes, transport_bytes, discovery_bytes, rx_buf_bytes;
} i_DartNodeBlocks;

static void i_dart_node_layout(i_DartBump *b, uint16_t max_peers, uint16_t max_topics,
                              const DartConfig *transport_cfg,
                              const DartDiscoveryNetConfig *discovery_rt_cfg, i_DartNodeBlocks *o){
    o->handles       = (uint8_t*)i_dart_bump_take(b, (size_t)max_topics * sizeof(DartTopic*), 16);
    o->node_core_bytes = i_dart_node_core_required_memory(max_topics);   /* peers live in discovery */
    o->node_core = (uint8_t*)i_dart_bump_take(b, o->node_core_bytes, 16);
    o->transport_bytes = dart_transport_required_memory(transport_cfg);
    o->transport = (uint8_t*)i_dart_bump_take(b, o->transport_bytes, 16);
#ifdef DART_SHM
    /* only the (topic,class) -> segment pointer table lives in the arena; segment state
       and the reader attach cache are lazy hook allocations, made when SHM is actually used */
    o->shm_pool = (uint8_t*)i_dart_bump_take(b, (size_t)max_topics * DART_SHM_N_CLASSES * sizeof(void*), 16);
#endif
    o->discovery_bytes = dart_discovery_placement_memory(discovery_rt_cfg);
    o->discovery = (uint8_t*)i_dart_bump_take(b, o->discovery_bytes, 16);
    /* data-socket RX buffer: sized so a unicast announce carrying the largest blob this
       node accepts fits (recvfrom drops an oversized datagram, which would leave a late
       joiner unable to ever fetch a big peer blob: its only path is this socket) */
    o->rx_buf_bytes = dart_discovery_wire_size(discovery_rt_cfg->discovery.meta_cap);
    if (o->rx_buf_bytes < DART_DGRAM_MAX) o->rx_buf_bytes = DART_DGRAM_MAX;
    o->rx_buf = (uint8_t*)i_dart_bump_take(b, o->rx_buf_bytes, 16);
}

/* send one datagram to a peer; returns 1 when done with it, 0 only on a would-block
 * TX-full. The core resolves the abstract destination; we just map it to a UDP address. */
static int i_dart_node_tx(DartNode *n, uint32_t to, const uint8_t *buf, size_t len){
    i_DartNodeDest d;
    if (!i_dart_node_core_resolve(n->core, to, &d)) return 1;   /* peer vanished */
    if (i_dart_plat_send(n->fd, buf, len, d.ip, d.port) < 0){
        if (i_dart_plat_would_block()) return 0;                /* TX full: retry this datagram next tick */
        {   /* hard send failure: report it and drop the datagram (reliable data is repaired) */
            DartEvent e; memset(&e, 0, sizeof e);
            e.kind = DART_ERROR; e.error = DART_E_SEND; e.peer = to;
            e.os_error = i_dart_plat_last_socket_error();
            i_dart_node_emit(n, &e);
        }
    }
    return 1;
}

#ifdef DART_SHM
/* lazily create our per-topic segment: chunk_bytes = the topic's locked size class,
 * n_chunks = its keep_last, so history slot i binds chunk i (no free list). NULL on fail. */
static i_DartShmPool *i_dart_node_shm_topic_pool(DartNode *n, uint16_t topic_index, uint32_t k, uint16_t keep_last){
    i_DartShmConfig c; uint8_t *mem; uint64_t seg; size_t idx;
    if (k >= DART_SHM_N_CLASSES) return NULL;
    idx = (size_t)topic_index * DART_SHM_N_CLASSES + k;
    if (n->shm_pool[idx]) return (i_DartShmPool*)n->shm_pool[idx];
    memset(&c, 0, sizeof c);
    seg = n->shm_base | ((uint64_t)topic_index << 3) | (uint64_t)k;   /* low 3 bits class, next 16 topic */
    c.segment_id = seg;
    i_dart_shm_seg_name(c.name, seg);
    c.chunk_bytes = i_dart_shm_class_bytes(k);
    c.n_chunks = keep_last ? keep_last : 1u;
    mem = (uint8_t*)i_dart_node_alloc(n, NULL, i_dart_shm_state_bytes());   /* stable: survives arena grows */
    if (!mem) return NULL;
    n->shm_pool[idx] = i_dart_shm_create(mem, &c);
    if (!n->shm_pool[idx]) i_dart_node_alloc(n, mem, 0);
    return (i_DartShmPool*)n->shm_pool[idx];
}
/* lazily attach a peer's segment by id (class is in the low bits); cache it. The cache
 * grows with segments actually attached, so a node with no same-host peer holds none. */
static i_DartShmPool *i_dart_node_shm_reader_pool(DartNode *n, uint64_t seg){
    uint16_t i; i_DartShmConfig c; uint8_t *mem;
    for (i=0;i<n->shm_reader_count;i++)
        if (n->shm_reader_segments[i]==seg) return (i_DartShmPool*)n->shm_reader_states[i];
    if (n->shm_reader_count == n->shm_reader_cap){        /* grow the id + state-ptr arrays */
        uint16_t ncap = n->shm_reader_cap ? (uint16_t)(n->shm_reader_cap*2u) : 8u;
        uint64_t *nseg; void **nst;
        if (ncap <= n->shm_reader_cap) return NULL;       /* u16 wrap: an absurd segment count */
        nseg = (uint64_t*)i_dart_node_alloc(n, n->shm_reader_segments, (size_t)ncap*sizeof(uint64_t));
        if (!nseg) return NULL;
        n->shm_reader_segments = nseg;
        nst = (void**)i_dart_node_alloc(n, n->shm_reader_states, (size_t)ncap*sizeof(void*));
        if (!nst) return NULL;
        n->shm_reader_states = nst;
        n->shm_reader_cap = ncap;
    }
    memset(&c, 0, sizeof c);
    c.segment_id = seg; i_dart_shm_seg_name(c.name, seg);     /* attach maps whole + reads geometry */
    mem = (uint8_t*)i_dart_node_alloc(n, NULL, i_dart_shm_state_bytes());
    if (!mem) return NULL;
    if (!i_dart_shm_attach(mem, &c)){ i_dart_node_alloc(n, mem, 0); return NULL; }
    n->shm_reader_segments[n->shm_reader_count] = seg;
    n->shm_reader_states[n->shm_reader_count] = mem;
    n->shm_reader_count++;
    return (i_DartShmPool*)mem;
}
static int i_dart_node_on_shm(void *u, uint16_t topic_index, uint32_t from, const uint8_t *desc){
    DartNode *n = (DartNode*)u; i_DartShmDesc d; i_DartShmPool *reader_pool; const void *p; uint32_t len;
    if (!i_dart_shm_desc_decode(&d, desc, DART_SHM_DESC_WIRE)) return 0;
    reader_pool = i_dart_node_shm_reader_pool(n, d.segment_id);
    if (!reader_pool) return 0;                                     /* can't attach -> reader NACKs (repair) */
    p = i_dart_shm_read(reader_pool, &d, &len);                       /* seqlock head: generation == descriptor? */
    if (!p) return 0;                                      /* recycled -> NACK -> repair or skip */
    /* one-copy: copy out of shared memory so the user owns the bytes (ack-timing safe) */
    if (len > n->shm_scratch_cap){
        void *new_buf = i_dart_node_alloc(n, n->shm_scratch, len?len:1u);
        if (!new_buf) return 0;
        n->shm_scratch = new_buf; n->shm_scratch_cap = len;
    }
    memcpy(n->shm_scratch, p, len);
    if (!i_dart_shm_verify(reader_pool, &d)) return 0;                /* seqlock tail: writer recycled mid-copy -> torn -> drop */
    if (i_dart_node_deliver(n, topic_index, from, dart_bytes(n->shm_scratch, len)))
        return -1;                                         /* consumer queue full -> reader parks the descriptor */
    n->shm_rx++;
    return 1;
}
#endif

static void i_dart_node_logs_open(DartNode *n);   /* defined with the log API below */

DartNode *dart_node_open(DartAllocator *alloc, const char *name, DartMsgFn on_message, DartEventFn on_event, const DartNodeOpts *opts){
    DartNodeOpts o; DartDiscoveryNetConfig dc; DartConfig tc; i_DartNodeBlocks blocks;
    uint16_t max_peers, max_topics, user_topics;
    uint8_t *base; void *arena; size_t need; DartAllocator pool;
    DartNode *n; i_DartSock fd; uint16_t local_port;
    char node_name[DART_NODE_NAME_MAX + 1]; uint8_t node_name_len = 0;   /* name is discovery-level */

    if (!alloc) return NULL;
    memset(&o, 0, sizeof o);
    if (opts) o = *opts;
    user_topics = o.max_topics ? o.max_topics : 8;
    /* the built-in topics (the @dart/log levels + the @dart/meta channels) ride OUTSIDE
       the app's max_topics budget, in a block ABOVE it (user topics keep dense indices) */
    max_topics = (uint16_t)(user_topics + (o.disable_logs ? 0 : 3));
#ifndef DART_NO_PATTERNS
    max_topics = (uint16_t)(max_topics + (o.disable_meta ? 0 : 2));
#endif
    max_peers    = o.discovery.max_peers ? o.discovery.max_peers : 16;

    /* sub-configs (sizing only depends on counts; the callback wrappers are set later) */
    memset(&dc,0,sizeof dc); memset(&tc,0,sizeof tc);
    dc.discovery.domain_id   = o.domain;
    dc.discovery.data_port   = o.net.data_port;
    dc.discovery.announce_interval_us = o.discovery.announce_interval_us;
    dc.discovery.peer_timeout_us = o.discovery.peer_timeout_us;
    dc.discovery.max_peers   = max_peers;
    dc.discovery.meta_cap = dart_meta_cap(max_topics);
    dc.discovery.peer_user_bytes = i_dart_node_core_peer_user_bytes();   /* node-core lifecycle state per peer */
    dc.discovery.alloc = i_dart_node_alloc;   /* per-peer blobs at actual size, not the worst-case pool
                                                 (set before sizing so measure and place agree; the
                                                 alloc_user is patched to n below, once it exists) */
    dc.group                 = o.net.discovery_group;
    dc.discovery_port        = o.net.discovery_port;
    dc.ttl                   = o.net.multicast_ttl;
    dc.multicast_interface   = o.net.multicast_interface;
    dc.seeds                 = o.net.seed_peers;
    dc.n_seeds               = o.net.n_seed_peers;
    tc.topics    = NULL;            /* reserve mode: topics created at runtime */
    tc.n_topics  = max_topics;
    tc.max_peers   = max_peers;
    tc.frag_size= o.net.fragment_size;
    tc.allocator   = i_dart_node_alloc;   /* required by dart_transport_init */

    {   i_DartBump b; memset(&b,0,sizeof b);
        i_dart_node_layout(&b, max_peers, max_topics, &tc, &dc, &blocks);
        need = b.offset + 32u; }

    /* The node COPIES the caller's allocator into its own pool (so the caller's may be a
       temporary, e.g. an open-and-return helper, and is left pristine). The node struct + the
       control-structs arena are freeable allocations from that pool: the node struct stays put
       across a grow (the user holds DartNode*), the arena is freed + relocated on grow, and
       message buffers/schemas/handles come from it. A failure resets a copy of the pool (which
       holds only what the node allocated), then returns. */
    pool = *alloc;
    n = (DartNode*)dart_allocator_alloc(&pool, NULL, sizeof *n);
    if (!n) return i_dart_node_open_fail(on_event, o.user_data, DART_E_OOM, 0, 0, sizeof *n);
    memset(n, 0, sizeof *n);
    n->pool = pool;                                  /* the node owns the pool now; allocate via &n->pool */
    n->alloc_dynamic = (alloc->page_realloc != NULL);
    arena = dart_allocator_alloc(&n->pool, NULL, need);
    if (!arena){ DartAllocator p = n->pool; dart_allocator_reset(&p);
                 return i_dart_node_open_fail(on_event, o.user_data, DART_E_OOM, 0, 0, need); }
    base = (uint8_t*)(((uintptr_t)arena+15u)&~(uintptr_t)15u);
    {   i_DartBump b; memset(&b,0,sizeof b);
        b.base = base; b.cap = need - (size_t)(base - (uint8_t*)arena);
        i_dart_node_layout(&b, max_peers, max_topics, &tc, &dc, &blocks); }

    if (!i_dart_plat_startup()){
        DartAllocator p = n->pool; dart_allocator_reset(&p);
        return i_dart_node_open_fail(on_event, o.user_data, DART_E_PLATFORM, 0, 0, 0);
    }

    n->fd = DART_SOCK_BAD;
    n->user_data = o.user_data;      /* set early so i_dart_node_emit can stamp it on any open-time error */
    n->on_event = on_event;
#ifdef DART_THREADS
    i_dart_plat_mutex_init(&n->mu);
    i_dart_plat_cond_init(&n->cv);
    /* opened for every node (not just started ones): it also wakes a plain
       dart_node_poll blocked in its wait when another thread sends. A platform
       whose loopback cannot carry it (exotic lwIP configs) DEGRADES rather than
       failing the open: the fd stays DART_SOCK_BAD, kick becomes a no-op, and
       cross-thread mutations are serviced at the next timer-capped tick (reported once). */
    if (!i_dart_plat_waker_open(&n->waker)){
        DartEvent e; memset(&e, 0, sizeof e);
        e.kind = DART_ERROR; e.error = DART_E_WAKER;
        i_dart_node_emit(n, &e);
    }
#endif
    n->domain = o.domain;
    n->net = o.net;
    n->announce_us = o.discovery.announce_interval_us ? o.discovery.announce_interval_us
                                                      : 1000000u;   /* discovery's default */
    n->match_wait_us = o.match_wait_ms < 0 ? 0u
                     : o.match_wait_ms ? (uint32_t)o.match_wait_ms * 1000u
                                       : (uint32_t)DART_MATCH_WAIT_MS * 1000u;
    n->match_epoch = 1;   /* topics memo at 0 = never computed, so a first send always checks */
    n->user_on_message = on_message;   /* on_event + user_data were set early (open-time error reporting) */
    n->arena = arena;
    n->handles = (DartTopic**)blocks.handles;
    memset(n->handles, 0, (size_t)max_topics * sizeof(DartTopic*));
    n->rx_buf = blocks.rx_buf; n->rx_buf_bytes = blocks.rx_buf_bytes;
    n->max_topics = max_topics;
    n->builtin_lo = user_topics;           /* the builtin block sits above the app's budget */
    n->max_peers = max_peers;
    n->meta_cap = dc.discovery.meta_cap;   /* the initial accept bound (self-heals up) */

    tc.on_message = i_dart_node_on_message;     /* wrap so on_message receives a DartMsg */
    tc.on_event   = i_dart_node_on_transport_event;  /* map DartTransportEvent -> app DartEvent */
    tc.schema_check = i_dart_node_schema_check; /* the schema gate, answered by the node core */
    tc.user       = n;
#ifdef DART_SHM
    n->shm_capable = (uint8_t)(n->alloc_dynamic && !o.disable_shm);   /* static mode never uses SHM */
    if (n->shm_capable){
        i_dart_plat_host_uuid(n->shm_host);
        if (!i_dart_plat_random(&n->shm_base, sizeof n->shm_base)) n->shm_base = i_dart_plat_pid();
        n->shm_base ^= (uint64_t)i_dart_plat_pid() << 32;    /* fold in pid for cross-process uniqueness */
        n->shm_base &= ~(((uint64_t)1u << 19) - 1u);       /* low 19 bits: 3 class + 16 topic index */
        if (n->shm_base == 0) n->shm_base = (uint64_t)1u << 19;
    }
    tc.on_shm = i_dart_node_on_shm;
#endif

    n->transport = dart_transport_init(blocks.transport, blocks.transport_bytes, &tc);
    if (!n->transport){ (void)i_dart_node_open_fail(on_event, o.user_data, DART_E_OOM, 0, 0, 0); goto fail_threads; }
#ifdef DART_SHM
    {   uint32_t n_segments = (uint32_t)max_topics * DART_SHM_N_CLASSES; uint32_t i;
        n->shm_n_topics = max_topics;
        n->shm_pool = (void**)blocks.shm_pool;
        for (i=0;i<n_segments;i++) n->shm_pool[i]=NULL;
        n->shm_reader_segments = NULL; n->shm_reader_states = NULL;   /* lazy: first attach allocates */
        n->shm_reader_cap = 0; n->shm_reader_count = 0;
    }
#endif

    /* sans-IO node core: drives the discovery->transport lifecycle and resolves addresses
       over the discovery core's peer table (bound below, once discovery exists). */
    node_name_len = dart_discovery_default_name(node_name, sizeof node_name, name);   /* handed to discovery below */
    memcpy(n->name, node_name, node_name_len); n->name[node_name_len] = '\0';   /* snapshot's copy */
    n->name_len = node_name_len;
    {   i_DartNodeCoreConfig cc;
        memset(&cc, 0, sizeof cc);
        cc.transport = n->transport;   /* cc.discovery bound after dart_discovery_place */
        cc.n_topics = max_topics; cc.frag_size = dart_clamp_frag(o.net.fragment_size);
        cc.on_event = i_dart_node_on_event; cc.user = n;
        cc.alloc = i_dart_node_alloc; cc.alloc_user = n;   /* backs interned/rebased peer schemas */
        cc.fetch_details = o.fetch_details;   /* observer mode: fetch + cache every peer topic */
#ifdef DART_SHM
        cc.oob_capable = n->shm_capable; memcpy(cc.oob_host, n->shm_host, 16);
#endif
        n->core = i_dart_node_core_init(blocks.node_core, blocks.node_core_bytes, &cc);
        if (!n->core){ (void)i_dart_node_open_fail(on_event, o.user_data, DART_E_OOM, 0, 0, 0); goto fail_threads; }
    }

    /* Bind the data socket before opening discovery so we can advertise its real
       port (0 => OS ephemeral, read back via getsockname). No reuse: a unicast
       endpoint owns its port, so a collision fails loudly here. */
    fd = i_dart_plat_udp_open();
    if (fd==DART_SOCK_BAD){ (void)i_dart_node_open_fail(on_event, o.user_data, DART_E_SOCKET, i_dart_plat_last_socket_error(), 0, 0); goto fail_threads; }
    n->fd=fd;                       /* owned now: fail_sock closes it */
    if (!i_dart_plat_bind(fd, 0, o.net.data_port, 0)){ (void)i_dart_node_open_fail(on_event, o.user_data, DART_E_BIND, i_dart_plat_last_socket_error(), o.net.data_port, 0); goto fail_sock; }
    local_port = i_dart_plat_local_port(fd);
    if (local_port==0){ (void)i_dart_node_open_fail(on_event, o.user_data, DART_E_SOCKET, 0, 0, 0); goto fail_sock; }
    i_dart_plat_set_nonblock(fd);   /* never block in recv/send; poll drains the queue */
    i_dart_plat_suppress_connreset(fd);  /* suppress WSAECONNRESET from a bounced send */
    if (o.net.recv_buffer_bytes) i_dart_plat_set_rcvbuf(fd, (int)o.net.recv_buffer_bytes);
    if (o.net.send_buffer_bytes) i_dart_plat_set_sndbuf(fd, (int)o.net.send_buffer_bytes);
    dc.discovery.data_port = local_port;       /* advertise the actual port */

    dc.discovery.on_event = i_dart_node_core_on_disc_event;   /* node core demuxes PEER_UP/DOWN/REFUSED */
    dc.discovery.user     = n->core;
    dc.discovery.alloc_user = n;               /* the blob hook allocates from the node's pool */
    dc.discovery.name     = dart_string(node_name, node_name_len);   /* discovery-owned (its own blob section) */
    /* the core builds our OVERLAY (frag size + OOB host + interest); discovery wraps it in
       its blob (after the locator + name) so peers reassemble and match from discovery */
    i_dart_node_core_build_meta(n->core);
    dc.discovery.meta = i_dart_node_core_meta(n->core);
    n->discovery = dart_discovery_place(blocks.discovery, blocks.discovery_bytes, &dc);
    if (!n->discovery){
        DartErrorKind err;   /* translate discovery's setup-failure reason into our vocabulary */
        switch (dart_discovery_last_error()){
        case DART_DISCOVERY_E_PLATFORM:   err = DART_E_PLATFORM;   break;
        case DART_DISCOVERY_E_SOCKET:     err = DART_E_SOCKET;     break;
        case DART_DISCOVERY_E_BIND:       err = DART_E_BIND;       break;
        case DART_DISCOVERY_E_MCAST_JOIN: err = DART_E_MCAST_JOIN; break;
        default:                          err = DART_E_OOM;        break;
        }
        (void)i_dart_node_open_fail(on_event, o.user_data, err, dart_discovery_last_os_error(),
                                    o.net.discovery_port, 0);
        goto fail_sock;
    }
    /* the node core delegates id<->address resolution + per-peer scratch to discovery's table */
    i_dart_node_core_bind_discovery(n->core, dart_discovery_state(n->discovery));

    /* the post-open gather anchor: discovery solicits on startup, so every peer already
       out there answers within an RTT of the first poll; the send-path match wait treats
       "settled since open" as the proof that the announce cache covers them */
    n->open_us = i_dart_plat_now_us();

    /* built-ins, last (they create topics, so the node must be fully open). A failed
       creation degrades (dart_node_log reports NOSYS / no meta endpoint), never fails
       the open: the node itself is healthy. */
    n->log_errors = (uint8_t)(o.log_errors && !o.disable_logs);
    n->creating_builtin = 1;               /* allocate from the builtin block */
    if (!o.disable_logs) i_dart_node_logs_open(n);
#ifndef DART_NO_PATTERNS
    if (!o.disable_meta) i_dart_patterns_meta_open(n);
#endif
    n->creating_builtin = 0;
    return n;

fail_sock:
    if (n->fd != DART_SOCK_BAD) i_dart_plat_close(n->fd);
    n->fd = DART_SOCK_BAD;
fail_threads:
#ifdef DART_THREADS
    if (n->waker.fd != DART_SOCK_BAD) i_dart_plat_waker_close(&n->waker);
    i_dart_plat_cond_destroy(&n->cv);
    i_dart_plat_mutex_destroy(&n->mu);
#endif
    i_dart_plat_cleanup();
    { DartAllocator p = n->pool; dart_allocator_reset(&p); }   /* frees the node struct + arena */
    return NULL;
}

/* Dynamic-mode growth: relocate the whole node into a bigger arena at the given counts so
 * a full peer table or topic reserve stops being a hard cap. Heap message buffers and SHM
 * writer segments stay put (only their owning control structures move); the user-held
 * DartNode and DartTopic handles live outside the arena, so they survive. Returns 1 with n
 * now on the new arena, or 0 if the bigger arena couldn't be allocated (n left unchanged). */
static int i_dart_node_grow(DartNode *n, uint16_t new_max_peers, uint16_t new_max_topics,
                            uint16_t want_meta_cap){
    DartConfig tc; DartDiscoveryNetConfig dc; i_DartNodeBlocks nb; i_DartBump b;
    DartTransportState *nt; i_DartNodeCore *ncore; DartDiscovery *ndisc;
    void *new_arena, *old_arena = n->arena;
    uint8_t *nbase; size_t need;
    uint16_t old_max_topics = n->max_topics;
    /* the accept bound never shrinks: topic-derived, previously grown, or requested */
    uint16_t new_meta_cap = dart_meta_cap(new_max_topics);
    if (n->meta_cap > new_meta_cap) new_meta_cap = n->meta_cap;
    if (want_meta_cap    > new_meta_cap) new_meta_cap = want_meta_cap;

    if (!n->alloc_dynamic) return 0;
    if (new_max_peers <= n->max_peers && new_max_topics <= n->max_topics
        && new_meta_cap <= n->meta_cap) return 0;

    memset(&tc,0,sizeof tc); memset(&dc,0,sizeof dc);
    tc.topics=NULL; tc.n_topics=new_max_topics; tc.max_peers=new_max_peers;
    tc.allocator=i_dart_node_alloc; tc.frag_size=n->net.fragment_size;
    dc.discovery.max_peers=new_max_peers; dc.discovery.meta_cap=new_meta_cap;
    dc.discovery.peer_user_bytes = i_dart_node_core_peer_user_bytes();   /* size discovery's scratch to match */
    dc.discovery.alloc = i_dart_node_alloc; dc.discovery.alloc_user = n; /* sizing must match the live core's
                                                                            hook mode (no arena blob pool) */

    memset(&b,0,sizeof b);
    i_dart_node_layout(&b, new_max_peers, new_max_topics, &tc, &dc, &nb);
    need = b.offset + 32u;
    new_arena = dart_allocator_alloc(&n->pool, NULL, need);
    if (!new_arena) return 0;
    nbase = (uint8_t*)(((uintptr_t)new_arena+15u)&~(uintptr_t)15u);
    memset(&b,0,sizeof b); b.base=nbase; b.cap=need-(size_t)(nbase-(uint8_t*)new_arena);
    i_dart_node_layout(&b, new_max_peers, new_max_topics, &tc, &dc, &nb);

    /* migrate the three cores; each leaves the old intact, so a failure just frees the new
       arena and bails (the old node keeps running, only refusing the would-be growth) */
    nt = dart_transport_migrate(n->transport, nb.transport, nb.transport_bytes, new_max_peers, new_max_topics);
    if (!nt){ dart_allocator_alloc(&n->pool, new_arena, 0); return 0; }
    ncore = i_dart_node_core_migrate(n->core, nb.node_core, nb.node_core_bytes, new_max_topics);
    if (!ncore){ dart_allocator_alloc(&n->pool, new_arena, 0); return 0; }
    ncore->transport = nt;                         /* re-point cross-layer pointer */
    i_dart_node_core_build_meta(ncore);              /* rebuild the announce blob into the new buf */
    ndisc = dart_discovery_migrate(n->discovery, nb.discovery, nb.discovery_bytes,
                                      new_max_peers, new_meta_cap, i_dart_node_core_meta(ncore).data, ncore);
    if (!ndisc){ dart_allocator_alloc(&n->pool, new_arena, 0); return 0; }
    i_dart_node_core_bind_discovery(ncore, dart_discovery_state(ndisc));   /* re-point to the relocated table */

    /* handle pointer array (the handle structs themselves are stable, not moved) */
    memcpy(nb.handles, n->handles, (size_t)old_max_topics*sizeof(DartTopic*));
    memset((DartTopic**)nb.handles + old_max_topics, 0,
           (size_t)(new_max_topics-old_max_topics)*sizeof(DartTopic*));

#ifdef DART_SHM
    if (n->shm_capable){
        uint32_t old_segs=(uint32_t)n->shm_n_topics*DART_SHM_N_CLASSES;
        uint32_t new_segs=(uint32_t)new_max_topics*DART_SHM_N_CLASSES, i;
        void **np = (void**)nb.shm_pool;
        for (i=0;i<new_segs;i++) np[i]=NULL;
        for (i=0;i<old_segs;i++) np[i]=n->shm_pool[i];   /* segment states are stable hook
                                                            allocations: only the pointer table moves */
        n->shm_pool=np; n->shm_n_topics=new_max_topics;
        /* the reader attach cache is hook-allocated too: attachments survive the grow */
    }
#endif

    n->transport=nt; n->core=ncore; n->discovery=ndisc;
    n->handles=(DartTopic**)nb.handles;
    n->rx_buf=nb.rx_buf; n->rx_buf_bytes=nb.rx_buf_bytes;
    n->max_topics=new_max_topics; n->max_peers=new_max_peers;
    n->meta_cap=new_meta_cap;
    dart_allocator_alloc(&n->pool, old_arena, 0);    /* control structs only; heap bufs + segments moved by ref */
    n->arena=new_arena;
    return 1;
}

/* Shared topic-create core: the public dart_node_create_topic and the patterns layer's
 * i_dart_node_create_pattern_topic both funnel here. kind/prefix_bytes/directed stamp the
 * entity (0/0/0 = a plain topic); sys_msg routes deliveries to the patterns layer; allow_at
 * permits the reserved '@' in the name (public topics may not use it). */
static DartTopic *i_dart_node_create_impl(DartNode *n, const char *name, DartRole role,
                              const DartSchema *schema, const DartTopicOpts *opts,
                              uint8_t kind, uint8_t prefix_bytes, uint8_t directed, uint8_t forceable,
                              i_DartSysMsgFn sys_msg, void *sys_user, int allow_at){
    DartTopicDef def; DartTopic *h; uint16_t idx; int acquired;
    if (!n || !name) return NULL;
    if (!allow_at){   /* '@' is reserved for pattern channels (f@req, v@set, ...) */
        const char *s = name;
        while (*s){ if (*s=='@') return NULL; s++; }
    }
    acquired = i_dart_node_lock(n);
    if (!acquired) return NULL;   /* from a callback: a grow here would relocate the arena mid-delivery */
    if (n->creating_builtin){
        /* open-time builtins fill their pre-reserved block at the top of the original
           reserve; a full block (accounting drift) degrades to no builtin, never a grow */
        idx = (uint16_t)(n->builtin_lo + n->n_builtin);
        if (idx >= n->max_topics){ i_dart_node_unlock(n, acquired); return NULL; }
    } else {
        idx = n->n_created;
        if (n->n_builtin && idx >= n->builtin_lo)
            idx = (uint16_t)(idx + n->n_builtin);   /* step over the builtin block */
        if (idx >= n->max_topics){       /* reserve full: grow (dynamic) or refuse (static) */
            uint16_t want = n->max_topics < 0x8000u ? (uint16_t)(n->max_topics*2u) : 0xFFFFu;
            if (want <= n->max_topics || !i_dart_node_grow(n, n->max_peers, want, 0)){
                i_dart_node_unlock(n, acquired);
                return NULL;
            }
        }
    }
    h = (DartTopic*)i_dart_node_alloc(n, NULL, sizeof *h);   /* stable: outlives any arena grow */
    if (!h){ i_dart_node_unlock(n, acquired); return NULL; }
    memset(h, 0, sizeof *h);
    if (schema){   /* copy into node memory so the caller's schema need not outlive the topic */
        DartBytes w = dart_schema_wire(schema);
        h->schema = dart_schema_parse(w.data, w.len, i_dart_node_alloc, n);
        if (!h->schema){ i_dart_node_alloc(n, h, 0); i_dart_node_unlock(n, acquired); return NULL; }
    }
    memset(&def, 0, sizeof def);
    def.name = name; def.role = (uint8_t)role;
    def.kind = kind; def.prefix_bytes = prefix_bytes; def.directed = directed; def.forceable = forceable;
    if (opts) def.qos = opts->qos;
    if (dart_transport_topic_define(n->transport, idx, &def) != 0){
        if (h->schema) dart_schema_free(h->schema, i_dart_node_alloc, n);
        i_dart_node_alloc(n, h, 0);
        i_dart_node_unlock(n, acquired);
        return NULL;
    }
    h->n = n; h->index = idx; h->prefix_bytes = prefix_bytes; h->kind = kind;
    h->role = (uint8_t)role;
    h->sys_on_message = sys_msg; h->sys_msg_user = sys_user;
    {   /* stable name copy: queued DartMsg views must not point into the relocatable arena */
        size_t nl = strlen(name);
        if (nl > DART_TOPIC_NAME_MAX) nl = DART_TOPIC_NAME_MAX;
        memcpy(h->name, name, nl);
        h->name_len = (uint8_t)nl;
    }
    if (def.qos.queue_bytes)   /* queued from creation; best-effort on OOM (take retries) */
        (void)i_dart_node_queue_ensure(n, h, &def.qos);
    if (h->schema)   /* advertise + gate matches with it (any non-INACTIVE role) */
        i_dart_node_core_set_topic_schema(n->core, idx, h->schema);
    /* re-advertise our interest so peers match the new topic as the blob arrives, and
       replay known peers' interest so this topic matches what they already advertised */
    i_dart_node_core_build_meta(n->core);
    dart_discovery_advertise(n->discovery, i_dart_node_core_meta(n->core));
    dart_discovery_replay(n->discovery);
    n->handles[idx] = h;
    if (n->creating_builtin) n->n_builtin++; else n->n_created++;
    n->match_epoch++;   /* a new topic may raise fresh candidates: converged memos are stale */
    i_dart_node_kick(n);                       /* announce the new topic now; the replay above
                                                  queued any DETAIL_REQs, so the pass sends them */
    i_dart_node_unlock(n, acquired);
    return h;
}

DartTopic *dart_node_create_topic(DartNode *n, const char *name, DartRole role,
                                      const DartSchema *schema, const DartTopicOpts *opts){
    return i_dart_node_create_impl(n, name, role, schema, opts, DART_KIND_TOPIC, 0, 0, 0, NULL, NULL, 0);
}

DartTopic *i_dart_node_create_pattern_topic(DartNode *n, const char *name, DartRole role,
                              const DartSchema *schema, const DartTopicOpts *opts,
                              uint8_t kind, uint8_t prefix_bytes, uint8_t directed, uint8_t forceable,
                              i_DartSysMsgFn on_msg, void *on_msg_user){
    return i_dart_node_create_impl(n, name, role, schema, opts, kind, prefix_bytes, directed, forceable,
                                   on_msg, on_msg_user, 1);
}

/* ---- the built-in log topics (runtime.h "logs") ------------------------------------- */

static int i_dart_node_do_send(DartNode *n, uint16_t topic_index, DartBytes data, int may_wait);

static const char *const i_dart_log_topic_names[3] =
    { "@dart/log/error", "@dart/log/warn", "@dart/log/info" };

/* Create the three log topics at open: plain reliable topics whose KEEP_LAST ring IS
 * the replayable history (keep_last = catch_up), publish-only until the app widens a
 * role, and NO backpressure wait: a slow or absent subscriber loses old lines instead
 * of ever blocking the app. Degrades to NULL handles on OOM (never fails the open). */
static void i_dart_node_logs_open(DartNode *n){
    DartTopicOpts topt; int lvl;
    n->log_schema = dart_schema_compile(i_dart_node_alloc, n,
        "DartLog { wall_us: u64, mono_us: u64, text: string }", NULL);
    if (!n->log_schema) return;
    for (lvl = 0; lvl < 3; lvl++){
        memset(&topt, 0, sizeof topt);
        topt.qos.reliability = DART_RELIABLE;
        topt.qos.keep_last = topt.qos.catch_up = (lvl == DART_LOG_INFO) ? 8 : 16;
        /* backpressure_wait_us stays 0 = NONE (pure KEEP_LAST): logs never block */
        n->log_topics[lvl] = i_dart_node_create_impl(n, i_dart_log_topic_names[lvl],
                                 DART_PUB_ONLY, n->log_schema, &topt,
                                 DART_KIND_TOPIC, 0, 0, 0, NULL, NULL, 1);
    }
}

/* Build one DartLog message and publish it on the level's topic. locked=1 is the mirror
 * flush (node lock held: the reentrant internal send); 0 the public thread-safe path. */
static int i_dart_node_log_publish(DartNode *n, DartLogLevel level, const char *text,
                                   size_t text_len, uint64_t wall_us, uint64_t mono_us,
                                   int locked){
    uint8_t msg[20u + DART_LOG_MAX];   /* fixed section (16) + the text frame header (4) */
    uint32_t len;
    DartTopic *h = n->log_topics[level];
    if (!h || !n->log_schema) return DART_ERR_NOSYS;
    if (!dart_schema_message_default(n->log_schema, msg, sizeof msg)) return DART_ERR_OOM;
    dart_set_uint(msg, sizeof msg, n->log_schema, "wall_us", wall_us);
    dart_set_uint(msg, sizeof msg, n->log_schema, "mono_us", mono_us);
    if (!dart_set_string(msg, sizeof msg, n->log_schema, "text", dart_string(text, text_len)))
        return DART_ERR_TOO_BIG;
    len = dart_schema_msg_len(n->log_schema, msg, sizeof msg);
    if (locked) return i_dart_node_do_send(n, h->index, dart_bytes(msg, len), 0);
    return dart_topic_send(h, dart_bytes(msg, len));
}

int dart_node_log(DartNode *n, DartLogLevel level, const char *fmt, ...){
    char text[DART_LOG_MAX]; int tn;
    va_list ap;
    if (!n || (int)level < 0 || level > DART_LOG_INFO || !fmt) return DART_ERR_NO_TOPIC;
    if (!n->log_topics[level]) return DART_ERR_NOSYS;
    va_start(ap, fmt);
    tn = vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (tn < 0) tn = 0;                                       /* encoding error: empty line */
    if (tn >= (int)sizeof text) tn = (int)sizeof text - 1;    /* truncated at DART_LOG_MAX */
    return i_dart_node_log_publish(n, level, text, (size_t)tn,
                                   i_dart_plat_wall_us(), i_dart_plat_now_us(), 0);
}

DartTopic *dart_node_log_topic(DartNode *n, DartLogLevel level){
    if (!n || (int)level < 0 || level > DART_LOG_INFO) return NULL;
    return n->log_topics[level];
}

/* Publish the pending error->log mirror ring (poll pass, lock held): each entry's text
 * was formatted at record time; a coalesced burst appends its count, and overflow
 * between flushes becomes one summary line. */
static void i_dart_node_log_flush(DartNode *n){
    uint8_t i, cnt;
    if (!n->log_topics[DART_LOG_ERROR]){ n->log_pend_n = 0; n->log_pend_dropped = 0; return; }
    n->log_flushing = 1;
    cnt = n->log_pend_n; n->log_pend_n = 0;
    for (i = 0; i < cnt; i++){
        i_DartLogPend *p = &n->log_pend[i];
        char line[sizeof p->text + 16]; int ln;
        ln = (p->count > 1) ? snprintf(line, sizeof line, "%s (x%u)", p->text, (unsigned)p->count)
                            : snprintf(line, sizeof line, "%s", p->text);
        if (ln < 0) ln = 0;
        if (ln >= (int)sizeof line) ln = (int)sizeof line - 1;
        i_dart_node_log_publish(n, DART_LOG_ERROR, line, (size_t)ln, p->wall_us, p->mono_us, 1);
    }
    if (n->log_pend_dropped){
        char line[64];
        int ln = snprintf(line, sizeof line, "log mirror overflow: %u error events dropped",
                          (unsigned)n->log_pend_dropped);
        n->log_pend_dropped = 0;
        i_dart_node_log_publish(n, DART_LOG_ERROR, line, (size_t)(ln < 0 ? 0 : ln),
                                i_dart_plat_wall_us(), i_dart_plat_now_us(), 1);
    }
    n->log_flushing = 0;
}

/* max wall-time draining RX (and running on_message) per poll tick before
 * yielding to discovery, so a slow on_message never starves it */
#ifndef DART_RX_BUDGET_US
#define DART_RX_BUDGET_US 5000u
#endif

/* drain one socket's RX queue into the transport until empty or past deadline
 * (full drain avoids NACK storms). Distinct from public dart_topic_drain. */
static void i_dart_node_rx_drain(DartNode *n, i_DartSock fd, uint64_t deadline){
    uint8_t *buf = n->rx_buf;
    for (;;){
        uint8_t src_ip[4]; uint16_t src_port;
        int r = i_dart_plat_recv(fd, buf, n->rx_buf_bytes, src_ip, &src_port);
        if (r<0){
            if (i_dart_plat_would_block()) break;        /* queue empty */
            {   /* a hard recv error (rare: connreset is suppressed on the data socket).
                   report it and stop draining this tick so we never spin on a wedged socket. */
                DartEvent e; memset(&e, 0, sizeof e);
                e.kind = DART_ERROR; e.error = DART_E_RECV; e.os_error = i_dart_plat_last_socket_error();
                i_dart_node_emit(n, &e);
            }
            break;
        }
        if (r>0){
            if (r>=4 && buf[0]=='u' && buf[1]=='D' && buf[2]=='S' && buf[3]=='C'){
                /* unicast announce aimed at our data port: hand it to discovery */
                dart_discovery_feed(n->discovery, src_ip, 4, dart_bytes(buf, (size_t)r));
            } else if (r>=5 && buf[0]=='u' && buf[1]=='D' && buf[2]=='T' && buf[3]=='L'){
                /* pairwise detail exchange: answer a request to its SOURCE (stateless, so
                   any requester works, peer or not: the explorer, a not-yet-added node).
                   A failed/refused send is just dropped: the requester re-asks on the
                   responder's next announce. A response feeds the pending-match cycle:
                   verdicts cached, interest re-applied, matches form. */
                if (buf[4]==DART_DETAIL_REQ){
                    DartBytes resp = i_dart_node_core_detail_respond(n->core, n->domain,
                                                                     dart_bytes(buf, (size_t)r));
                    if (resp.len) i_dart_plat_send(fd, resp.data, resp.len, src_ip, src_port);
                } else if (buf[4]==DART_DETAIL_RESP){
                    uint32_t from;
                    if (i_dart_node_core_id_for_addr(n->core, src_ip, src_port, &from))
                        i_dart_node_core_apply_details(n->core, n->domain, from,
                                                       dart_bytes(buf, (size_t)r));
                } else if (buf[4]==DART_INTEREST_REQ){
                    /* external-interest paging: serve one byte-range page of our interest
                       blob to the request's source (stateless, like the detail responder) */
                    DartBytes resp = i_dart_node_core_interest_respond(n->core, n->domain,
                                                                       dart_bytes(buf, (size_t)r));
                    if (resp.len) i_dart_plat_send(fd, resp.data, resp.len, src_ip, src_port);
                } else if (buf[4]==DART_INTEREST_RESP){
                    uint32_t from;
                    if (i_dart_node_core_id_for_addr(n->core, src_ip, src_port, &from))
                        i_dart_node_core_apply_interest_page(n->core, n->domain, from,
                                                             dart_bytes(buf, (size_t)r));
                }
            } else {
                uint32_t from;
                if (i_dart_node_core_id_for_addr(n->core, src_ip, src_port, &from))
                    dart_transport_on_datagram(n->transport, from, dart_bytes(buf, (size_t)r), i_dart_plat_now_us());
            }
        }
        if (i_dart_plat_now_us() >= deadline) break;      /* yield to discovery/send */
    }
}

/* threaded mode: the longest a send waits for a poller to hand unsent head history
 * to the wire before overwriting it (KEEP_LAST + DART_E_EVICTED_UNSENT after). One
 * waker-kicked work pass normally clears it in microseconds; the bound only gates
 * a genuinely wedged socket. */
#ifndef DART_UNSENT_WAIT_US
#define DART_UNSENT_WAIT_US 20000u
#endif

/* Cap a wait at the transport's next timer (ack/NACK/heartbeat) and discovery's next
 * announce, so both fire on time with no traffic to wake us. Lock held; pure compute. */
static int i_dart_node_wait_ms(DartNode *n, int timeout_ms){
    uint64_t next = dart_transport_next_deadline_us(n->transport);
    uint64_t due  = dart_discovery_next_due_us(dart_discovery_state(n->discovery));
    if (timeout_ms < 0) timeout_ms = 0;
    if (!next || due < next) next = due;   /* due is a real time (0 = now); next 0 = none */
    if (n->sys_tick_next && (!next || n->sys_tick_next < next)) next = n->sys_tick_next;  /* patterns tick */
    if (next){
        uint64_t t0 = i_dart_plat_now_us();
        uint64_t us = (next > t0) ? next - t0 : 0;
        int ms = (us >= (uint64_t)timeout_ms*1000u) ? timeout_ms
                                                    : (int)((us + 999u)/1000u);   /* round up: no busy-spin */
        if (ms < timeout_ms) timeout_ms = ms;
    } else timeout_ms = 0;                 /* something is due right now */
    return timeout_ms;
}

static int i_dart_node_gather_done(DartNode *n, uint64_t now);   /* defined with the match wait below */

/* One poll tick with the node lock held: deferred grow + discovery tick, the blocking
 * wait, then RX drain + TX flush. The ONE poll body: dart_node_poll, the service loop,
 * and the backpressure pump all run it. outer=1 means this thread's own acquisition
 * wraps the call, so the wait may drop the lock (other threads send into it and kick
 * the waker); the pump's nested call passes 0 and keeps the lock across its wait. */
static void i_dart_node_poll_locked(DartNode *n, int timeout_ms, int outer){
    uint8_t buf[DART_DGRAM_MAX]; uint32_t to; size_t out_len; uint64_t now;
    i_DartPollfd pfd[5]; int nfds = 0, wait_ms, poll_rc;
#ifdef DART_THREADS
    int waker_slot = -1;
#endif

    /* a peer was refused last tick for lack of slots: grow the table now, between ticks
       (safe: not inside any layer's processing), then the peer's next announce is admitted */
    if (n->grow_pending){
        uint16_t want = n->max_peers < 0x8000u ? (uint16_t)(n->max_peers*2u) : 0xFFFFu;
        n->grow_pending = 0;
        if (want > n->max_peers) i_dart_node_grow(n, want, n->max_topics, 0);
    }
    /* a peer's announce blob exceeded the accept bound last tick: raise the bound (grows
       the RX buffers + discovery capacity with it), then solicit so the peer re-sends its
       blob to buffers that now fit. A failed grow is remembered so the retry loop surfaces
       through the normal event path instead of silently spinning. */
    if (n->meta_grow_need){
        uint16_t want = n->meta_grow_need;
        n->meta_grow_need = 0;
        if (want > n->meta_cap){
            if (i_dart_node_grow(n, n->max_peers, n->max_topics, want)){
                n->meta_grow_failed = 0;
                dart_discovery_solicit(dart_discovery_state(n->discovery));
            } else
                n->meta_grow_failed = want;
        }
    }

    dart_discovery_poll(n->discovery, 0);                 /* discovery tick (non-blocking) */

    wait_ms = i_dart_node_wait_ms(n, timeout_ms);
    memset(pfd, 0, sizeof pfd);
    pfd[nfds].fd = n->fd; pfd[nfds].events = DART_POLLIN; nfds++;
#ifdef DART_THREADS
    if (n->waker.fd != DART_SOCK_BAD){
        waker_slot = nfds;
        pfd[nfds].fd = n->waker.fd; pfd[nfds].events = DART_POLLIN; nfds++;
    }
#endif
    {   /* discovery's sockets join the wait so an inbound announce cuts a long sleep
           short (drained by the next tick's discovery poll). The fds are stable by
           value: a grow while the lock is dropped relocates structs, never sockets. */
        i_DartSock dfds[2]; int i, dn = dart_discovery_pollfds(n->discovery, dfds);
        for (i = 0; i < dn; i++){ pfd[nfds].fd = dfds[i]; pfd[nfds].events = DART_POLLIN; nfds++; }
    }

#ifdef DART_THREADS
    if (outer){
        /* the wait runs UNLOCKED so a sender on another thread is never blocked behind
           it; pollers_sleeping tells such a sender to kick the waker on a state change
           (a counter, not a flag: several threads may poll the same node) */
        n->pollers_sleeping++;
        i_dart_node_unlock_raw(n);
        poll_rc = i_dart_plat_poll(pfd, nfds, wait_ms);
        i_dart_node_lock_raw(n);
        n->pollers_sleeping--;
    } else {
        poll_rc = i_dart_plat_poll(pfd, nfds, wait_ms);
    }
    if (waker_slot >= 0){
        i_dart_plat_waker_drain(&n->waker);
        n->wake_signaled = 0;
    }
#else
    (void)outer;
    poll_rc = i_dart_plat_poll(pfd, nfds, wait_ms);
#endif
    if (poll_rc < 0){   /* the wait itself failed (a real fault, e.g. a bad fd; would-block is not it) */
        DartEvent e; memset(&e, 0, sizeof e);
        e.kind = DART_ERROR; e.error = DART_E_POLL; e.os_error = i_dart_plat_last_socket_error();
        i_dart_node_emit(n, &e);
    }

    if (pfd[0].revents & DART_POLLIN)
        i_dart_node_rx_drain(n, n->fd, i_dart_plat_now_us() + DART_RX_BUDGET_US);

    /* mirrored errors recorded during RX (or by other threads since the last pass)
       publish now, BEFORE the TX pull, so the lines ride this pass's datagrams */
    if (n->log_pend_n || n->log_pend_dropped) i_dart_node_log_flush(n);

    now=i_dart_plat_now_us();
    /* detail-exchange requester: drain queued DETAIL_REQs (new candidates from an
       announce, or a paged response's remainder), and rearm every active peer once per
       announce interval while any request went out (the lost-datagram retry). Converged
       = a sweep that sends nothing: the timer disarms until candidates reappear. */
    if (i_dart_node_core_detail_any(n->core) || (n->next_detail_us && now >= n->next_detail_us)){
        i_DartNodeDest dst; size_t len; int sent = 0;
        if (n->next_detail_us && now >= n->next_detail_us)
            i_dart_node_core_detail_rearm(n->core);
        while ((len = i_dart_node_core_detail_req_next(n->core, n->domain, buf, sizeof buf, &dst)) != 0){
            i_dart_plat_send(n->fd, buf, len, dst.ip, dst.port);   /* best-effort: retries heal */
            sent = 1;
        }
        n->next_detail_us = sent ? now + n->announce_us : 0;
    }
    /* the core already consumed any held datagram, so retry it before pulling new */
    if (n->tx_hold_len && i_dart_node_tx(n, n->tx_hold_peer, n->tx_hold, n->tx_hold_len))
        n->tx_hold_len = 0;
    if (!n->tx_hold_len)
        while (dart_transport_poll_send(n->transport,&to,buf,sizeof buf,&out_len,now)){
            if (!i_dart_node_tx(n, to, buf, out_len)){
                memcpy(n->tx_hold, buf, out_len);
                n->tx_hold_len = out_len; n->tx_hold_peer = to;
                break;          /* TX buffer full: yield this tick */
            }
            now=i_dart_plat_now_us();
        }

    /* latch the post-open gather the moment it settles (cheap peer walk, only until it
       latches): a later create's own replay events then cannot reset the quiet clock and
       make that topic's first send re-wait a window the network already earned. */
    if (!n->gather_done) (void)i_dart_node_gather_done(n, now);

    /* patterns layer per-poll tick (call timeouts, retry sweeps): runs under the node lock
       like a callback, returns its next deadline for the wait cap. */
    if (n->sys_tick) n->sys_tick_next = n->sys_tick(n->sys_user, i_dart_plat_now_us());

#ifdef DART_THREADS
    n->work_seq++;
    if (n->cv_waiters) i_dart_plat_cond_broadcast(&n->cv);   /* acks/TX may have progressed */
#endif
}

int dart_node_poll(DartNode *n, int timeout_ms){
    int acquired;
    if (!n) return DART_ERR_STATE;
    acquired = i_dart_node_lock(n);
#ifdef DART_THREADS
    if (!acquired || n->svc_running){
        /* called from inside a callback, or a service thread owns the loop: refuse
           loudly rather than double-drive the node */
        i_dart_node_unlock(n, acquired);
        return DART_ERR_STATE;
    }
#endif
    i_dart_node_poll_locked(n, timeout_ms, acquired);
    i_dart_node_unlock(n, acquired);
    return 0;
}

/* ---- the send-path MATCH WAIT (runtime.h "MATCH WAIT") ------------------------------- */

static int i_dart_node_settled(DartNode *n, uint64_t start, uint64_t now);   /* defined with settle below */

/* Has the post-open discovery gather completed? The settle predicate anchored at open
 * (discovery solicits on startup, so every already-present peer answers within an RTT of
 * the first poll); LATCHED once true, so it is derived at most a handful of times. */
static int i_dart_node_gather_done(DartNode *n, uint64_t now){
    if (n->gather_done) return 1;
    if (!i_dart_node_settled(n, n->open_us, now)) return 0;
    n->gather_done = 1;
    return 1;
}

/* Would a send on this topic RIGHT NOW race a still-forming match? 1 while the post-open
 * gather is unsettled or some candidate's verdicts are in flight; 0 once converged, then
 * MEMOIZED against the topology epoch, so the steady-state zero-subscriber send pays one
 * integer compare here, never the peer walk. Node lock held. */
static int i_dart_node_topic_unsettled(DartNode *n, DartTopic *h, uint64_t now){
    if (h->resolve_epoch == n->match_epoch) return 0;   /* converged at this topology */
    if (!i_dart_node_gather_done(n, now)) return 1;
    if (i_dart_node_core_topic_unresolved(n->core, h->index) > 0) return 1;
    h->resolve_epoch = n->match_epoch;
    return 0;
}

/* Bounded wait for a forming match before a would-be-zero-subscriber send commits: sleep
 * on the poller's progress under a service thread, else pump the loop (both re-check the
 * clock on a short cadence: gather settling is partly time-driven, like settle). Returns
 * the matched count after the wait; a timeout fires DART_E_UNMATCHED_SEND, never silent. */
static int i_dart_node_match_wait(DartNode *n, uint16_t topic_index, DartTopic *h){
    uint64_t now = i_dart_plat_now_us();
    uint64_t deadline = now + n->match_wait_us;
    int matched = 0, timed_out = 0;
#ifdef DART_THREADS
    if (n->svc_running){
        n->cv_waiters++;
        for (;;){
            matched = dart_transport_publisher_match_count(n->transport, topic_index);
            if (matched) break;
            now = i_dart_plat_now_us();
            if (!i_dart_node_topic_unsettled(n, h, now)) break;
            if (now >= deadline){ timed_out = 1; break; }
            {   uint64_t left = deadline - now;
                i_dart_node_cv_wait(n, left < 50000u ? left : 50000u);   /* mu drops: nothing cached survives */
            }
            if (!n->svc_running) break;   /* stopped under us */
        }
        n->cv_waiters--;
    } else
#endif
    for (;;){
        matched = dart_transport_publisher_match_count(n->transport, topic_index);
        if (matched) break;
        now = i_dart_plat_now_us();
        if (!i_dart_node_topic_unsettled(n, h, now)) break;
        if (now >= deadline){ timed_out = 1; break; }
        i_dart_node_poll_locked(n, 1, 0);   /* nested tick: the lock stays held */
    }
    if (timed_out){
        DartEvent e; memset(&e, 0, sizeof e);
        e.kind = DART_ERROR; e.error = DART_E_UNMATCHED_SEND;
        e.topic = topic_index; e.topic_name = i_dart_node_topic_name(n, topic_index);
        i_dart_node_emit(n, &e);
    }
    return matched;
}

/* publish on a topic index: match wait (a first send racing the forming match), then
 * flow-control wait (condvar under a service thread, the nested pump otherwise), then SHM
 * fast path, then UDP. may_wait=0 is a reentrant send (from a callback): it can never
 * block or run the loop, so it commits KEEP_LAST-style and only the unsent guard below
 * applies. */
static int i_dart_node_do_send_ex(DartNode *n, uint16_t topic_index, DartBytes hdr, DartBytes data,
                                  int directed, uint32_t to_peer, int may_wait){
    size_t len = hdr.len + data.len;
    /* one O(1) count gates the per-send fast paths: a topic no peer subscribes to skips
       backpressure and the SHM eligibility scan here, and the copy+commit in the core. */
    int matched = dart_transport_publisher_match_count(n->transport, topic_index);
    const DartQos *q = dart_transport_topic_qos(n->transport, topic_index);
    int guarded = 0;   /* the unsent-eviction check runs after the wait */

    /* MATCH WAIT: a send that would commit to ZERO subscribers while a match is still
       forming (candidate verdicts in flight after create, or the post-open gather not yet
       settled) waits for matching to converge instead of silently dropping, so the first
       send after create/open reaches subscribers that were already on the network. When
       it cannot wait (disabled, or a reentrant send) it proceeds immediately but the
       drop-in-the-making still fires DART_E_UNMATCHED_SEND. A topic that RETAINS history
       (reliable + catch_up > 0) is exempt: the forming match replays its history, so the
       race loses nothing there. Steady state costs one epoch compare
       (i_dart_node_topic_unsettled memoizes convergence). */
    if (!matched && topic_index < n->max_topics
        && !(q && q->reliability == DART_RELIABLE && q->catch_up > 0)){
        DartTopic *h = n->handles[topic_index];
        if (h && (h->role == DART_PUBSUB || h->role == DART_PUB_ONLY)
              && i_dart_node_topic_unsettled(n, h, i_dart_plat_now_us())){
            if (may_wait && n->match_wait_us){
                matched = i_dart_node_match_wait(n, topic_index, h);
                q = dart_transport_topic_qos(n->transport, topic_index);   /* the wait may have grown/moved the arena */
            } else {
                DartEvent e; memset(&e, 0, sizeof e);
                e.kind = DART_ERROR; e.error = DART_E_UNMATCHED_SEND;
                e.topic = topic_index; e.topic_name = i_dart_node_topic_name(n, topic_index);
                i_dart_node_emit(n, &e);
            }
        }
    }

#ifdef DART_THREADS
    if (matched && n->svc_running){
        guarded = 1;
        /* Threaded: wait on the condvar for the poller's progress instead of pumping the
           loop ourselves. Two predicates: unacked reliable history (bounded by
           qos.backpressure_wait_us, as the pump always was) and UNSENT history on any
           lane (bounded by DART_UNSENT_WAIT_US; one kicked work pass normally clears it,
           so bursts block for microseconds, not the bound). */
        if (may_wait){
            uint64_t now = i_dart_plat_now_us(), t0 = now;
            uint64_t rel_deadline = (q && q->backpressure_wait_us) ? now + q->backpressure_wait_us : 0;
            uint64_t unsent_deadline = now + DART_UNSENT_WAIT_US;
            uint64_t seq0 = n->work_seq;
            int waited = 0;
            n->cv_waiters++;
            for (;;){
                int evict_unacked = rel_deadline && dart_transport_send_would_evict(n->transport, topic_index);
                int evict_unsent  = dart_transport_send_would_evict_unsent(n->transport, topic_index, NULL, NULL);
                uint64_t deadline;
                if (!evict_unacked && !evict_unsent) break;
                /* unsent-only after a completed work pass: a pass either empties the TX
                   queue or parks a datagram in tx_hold on a socket would-block. Held
                   datagram = the SOCKET is the bottleneck, so KEEP_LAST drop-oldest
                   applies; no hold = another sender refilled the ring meanwhile
                   (contention), so re-arm and keep waiting (the deadline still bounds it) */
                if (!evict_unacked && n->work_seq != seq0){
                    if (n->tx_hold_len) break;
                    seq0 = n->work_seq;
                }
                now = i_dart_plat_now_us();
                deadline = evict_unacked
                         ? (rel_deadline > unsent_deadline ? rel_deadline : unsent_deadline)
                         : unsent_deadline;
                if (now >= deadline) break;
                waited = 1;
                i_dart_node_kick(n);
                i_dart_node_cv_wait(n, deadline - now);   /* mu drops: nothing cached survives */
                if (!n->svc_running) break;               /* stopped under us */
            }
            n->cv_waiters--;
            if (waited){
                n->backpressure_total_us += i_dart_plat_now_us() - t0;
                n->backpressure_wait_count++;
            }
            /* the poller may have grown/relocated the arena while we slept */
            q = dart_transport_topic_qos(n->transport, topic_index);
            matched = dart_transport_publisher_match_count(n->transport, topic_index);
        }
    } else
#endif
    /* No service thread: bounded backpressure pump. Run the loop (on_message/on_event
       fire here) until a slow reader acks or qos.backpressure_wait_us elapses, then
       send anyway. */
    if (matched && may_wait && q && q->backpressure_wait_us && dart_transport_send_would_evict(n->transport, topic_index)){
        uint64_t t0 = i_dart_plat_now_us(), deadline = t0 + q->backpressure_wait_us;
        /* in-pump diagnostic: the publisher is blocked here for the whole wait, so its
           normal per-message print sees nothing within it. When a probe is set, sample
           the writer's repair progress on a ~interval timer so the stall is visible as a
           within-block time series (resends bursty-then-flat vs steady; writer idle for
           lack of NACKs). Observational; when no probe is set this whole block is skipped. */
        uint64_t sample_last = t0; uint32_t sample_polls = 0, sample_idle = 0;
        uint64_t interval = n->pump_probe_interval_us ? n->pump_probe_interval_us : 200000u;
        DartRepairStats sample_prev;
        if (n->pump_probe) dart_transport_repair_stats(n->transport, topic_index, &sample_prev);
        guarded = 1;
        do {
            i_dart_node_poll_locked(n, 1, 0);   /* nested tick: the lock stays held */
            if (n->pump_probe){
                uint64_t now = i_dart_plat_now_us();
                sample_polls++;
                if (dart_transport_repair_pending(n->transport, topic_index) == 0) sample_idle++;
                if (now - sample_last >= interval){
                    DartRepairStats sample_now; DartPumpSample sample;
                    dart_transport_repair_stats(n->transport, topic_index, &sample_now);
                    sample.topic         = topic_index;
                    sample.wait_elapsed_us = now - t0;
                    sample.interval_us     = now - sample_last;
                    sample.frags_resent    = sample_now.frags_resent - sample_prev.frags_resent;
                    sample.nacks_recv      = sample_now.nacks_recv  - sample_prev.nacks_recv;
                    sample.polls           = sample_polls;
                    sample.polls_idle      = sample_idle;
                    n->pump_probe(n->pump_probe_user, &sample);
                    sample_prev = sample_now; sample_last = now; sample_polls = 0; sample_idle = 0;
                }
            }
            if (!dart_transport_send_would_evict(n->transport, topic_index)) break;
        } while (i_dart_plat_now_us() < deadline);
        n->backpressure_total_us += i_dart_plat_now_us() - t0;
        n->backpressure_wait_count++;
        /* a mid-pump grow relocates the arena: re-derive the cached pointers */
        q = dart_transport_topic_qos(n->transport, topic_index);
        matched = dart_transport_publisher_match_count(n->transport, topic_index);
    }

    /* Never-silent: a send that evicts history never handed to the wire (the wait
       above timed out, the socket is wedged, or a reentrant send cannot wait) is
       surfaced. The would-evict state is read here (before the commit changes it),
       but the event fires only if the send actually commits, below: a send the
       transport then rejects (TOO_BIG / ROLE / OOM) overwrote nothing. */
    {
        uint64_t evict_base = 0; uint32_t evict_count = 0;
        int will_evict = guarded &&
            dart_transport_send_would_evict_unsent(n->transport, topic_index, &evict_base, &evict_count);
        int r;
#ifdef DART_SHM
        /* Only messages that would FRAGMENT (len > our fragment size) gain from SHM:
           a single-datagram message ships as one inline DATA either way, and the SHM
           path still sends a descriptor datagram plus pool/desc/attach overhead, so
           below the fragment size inline UDP is strictly cheaper. Fragmentation is
           writer-driven with our own one size (never the peer's), so this is the
           unambiguous cutoff even when several same-host peers match. */
        if (!directed && n->shm_capable && len > dart_transport_frag(n->transport)
            && topic_index < n->shm_n_topics && matched
            && dart_transport_publisher_shm_eligible(n->transport, topic_index)){
            uint16_t keep_last = (q && q->keep_last) ? q->keep_last : 1u;
            /* a hint (shm_max_bytes / max_message_bytes) pins the topic to one class, so
               same-sized traffic reuses a single prefix-sized segment; without it each message
               uses its own size class's segment, created on demand. Per topic either way. */
            uint32_t hint = q ? (q->shm_max_bytes ? q->shm_max_bytes : q->max_message_bytes) : 0u;
            uint32_t k = i_dart_shm_class_for(hint ? hint : (uint32_t)len);
            /* fits its class (with a hint, the hinted class) -> publish via SHM; chunk index =
               the history slot this send will occupy, so chunk i binds slot i (no free list) */
            if (k < DART_SHM_N_CLASSES && (uint32_t)len <= i_dart_shm_class_bytes(k)){
                i_DartShmPool *pool = i_dart_node_shm_topic_pool(n, topic_index, k, keep_last);
                uint16_t slot = dart_transport_topic_hist_head(n->transport, topic_index);
                void *chunk_ptr = pool ? i_dart_shm_chunk(pool, slot, NULL) : NULL;
                if (chunk_ptr){
                    i_DartShmDesc d; uint8_t desc[DART_SHM_DESC_WIRE];
                    if (hdr.len) memcpy(chunk_ptr, hdr.data, hdr.len);  /* gather: pattern header then payload */
                    memcpy((uint8_t*)chunk_ptr + hdr.len, data.data, data.len);  /* one-copy write into shm */
                    i_dart_shm_stamp(pool, slot, (uint32_t)len, &d);
                    i_dart_shm_desc_encode(&d, desc);
                    if (dart_transport_send_shm(n->transport, topic_index, dart_bytes(chunk_ptr, len), desc, i_dart_plat_now_us())==0){
                        n->shm_tx++;
                        r = DART_OK;
                        goto committed;
                    }
                }
            }
        }
#endif
        r = directed ? dart_transport_send_to(n->transport, topic_index, to_peer, hdr, data, i_dart_plat_now_us())
                     : dart_transport_send_hdr(n->transport, topic_index, hdr, data, i_dart_plat_now_us());
#ifdef DART_SHM
committed:
#endif
        if (r == DART_OK && topic_index < i_dart_node_topic_hi(n) && n->handles[topic_index]){
            n->handles[topic_index]->tx_msgs++;                      /* dart_topic_counts */
            n->handles[topic_index]->tx_bytes += len;
        }
        if (r == DART_OK && will_evict){
            DartEvent e; memset(&e, 0, sizeof e);
            n->evicted_unsent++;
            e.kind = DART_ERROR; e.error = DART_E_EVICTED_UNSENT;
            e.topic = topic_index; e.topic_name = i_dart_node_topic_name(n, topic_index);
            e.lost_first = evict_base; e.lost_count = evict_count;
            i_dart_node_emit(n, &e);
        }
        return r;
    }
}

/* plain broadcast send (the hot dart_topic_send path): no header, no directed target */
static int i_dart_node_do_send(DartNode *n, uint16_t topic_index, DartBytes data, int may_wait){
    DartBytes nohdr; nohdr.data=NULL; nohdr.len=0;
    return i_dart_node_do_send_ex(n, topic_index, nohdr, data, 0, 0, may_wait);
}

int dart_topic_send(DartTopic *topic, DartBytes data){
    int acquired, r;
    if (!topic) return DART_ERR_NO_TOPIC;
    acquired = i_dart_node_lock(topic->n);
    r = i_dart_node_do_send(topic->n, topic->index, data, acquired);
    i_dart_node_kick(topic->n);              /* flush the commit now, not at the next tick */
    i_dart_node_unlock(topic->n, acquired);
    return r;
}

int i_dart_topic_send_hdr(DartTopic *topic, DartBytes hdr, DartBytes data){
    int acquired, r;
    if (!topic) return DART_ERR_NO_TOPIC;
    acquired = i_dart_node_lock(topic->n);
    r = i_dart_node_do_send_ex(topic->n, topic->index, hdr, data, 0, 0, acquired);
    i_dart_node_kick(topic->n);
    i_dart_node_unlock(topic->n, acquired);
    return r;
}

int i_dart_topic_send_to(DartTopic *topic, uint32_t to_peer, DartBytes hdr, DartBytes data){
    int acquired, r;
    if (!topic) return DART_ERR_NO_TOPIC;
    acquired = i_dart_node_lock(topic->n);
    r = i_dart_node_do_send_ex(topic->n, topic->index, hdr, data, 1, to_peer, acquired);
    i_dart_node_kick(topic->n);
    i_dart_node_unlock(topic->n, acquired);
    return r;
}

/* Node-pool allocation for the patterns layer (freed en masse by the close-time reset,
 * like every other node allocation), the layer's per-node handle slot, and the clock.
 * Not lock-guarded: the patterns layer calls them only while holding the node lock (from a
 * create/send that took it, or from a sys hook that runs under it). */
void  *i_dart_node_sys_alloc(DartNode *n, void *ptr, size_t size){ return i_dart_node_alloc(n, ptr, size); }
void **i_dart_node_sys_slot (DartNode *n){ return &n->patterns; }
uint64_t i_dart_node_now_us (DartNode *n){ (void)n; return i_dart_plat_now_us(); }
/* The node lock, for a pattern API call that mutates manager state: reentrancy-aware exactly
 * like the internal entry lock (1 = acquired here, 0 = the calling thread already held it,
 * e.g. a pattern call from inside a callback). No kick on unlock: every mutating path already
 * kicks at its own layer (the send helpers, topic create, set_sys_hooks), and the read-only
 * pattern ops (dart_variable_get and friends) must not wake a sleeping service thread. */
int  i_dart_node_sys_lock  (DartNode *n){ return i_dart_node_lock(n); }
void i_dart_node_sys_unlock(DartNode *n, int acquired){ i_dart_node_unlock(n, acquired); }
int  i_dart_node_sys_poll  (DartNode *n, int timeout_ms){ return dart_node_poll(n, timeout_ms); }

/* A topic-scoped DART_ERROR from the patterns layer, through the node's one event path. */
void i_dart_node_sys_error(DartNode *n, DartErrorKind error, DartTopic *topic, uint32_t peer){
    DartEvent e;
    if (!n) return;
    memset(&e, 0, sizeof e);
    e.kind = DART_ERROR; e.error = error; e.peer = peer;
    if (topic){ e.topic = topic->index; e.topic_name = i_dart_node_topic_name(n, topic->index); }
    i_dart_node_emit(n, &e);
}

/* Matched subscribers on this topic excluding dormant peers: the liveness query behind the
 * patterns layer's provider-loss detection (dart_topic_match_count keeps counting a
 * dropped-but-resumable peer, so it cannot answer "can anyone still reply?"). */
int i_dart_topic_live_match_count(DartTopic *topic){
    int acquired, r;
    if (!topic) return 0;
    acquired = i_dart_node_lock(topic->n);
    r = dart_transport_publisher_live_matches(topic->n->transport, topic->index);
    i_dart_node_unlock(topic->n, acquired);
    return r;
}

/* Matched publishers feeding this topic's subscription side (the mirror of
 * dart_topic_match_count): the patterns layer's "is any owner present" query. */
int i_dart_topic_source_match_count(DartTopic *topic){
    int acquired, r;
    if (!topic) return 0;
    acquired = i_dart_node_lock(topic->n);
    r = dart_transport_subscriber_match_count(topic->n->transport, topic->index);
    i_dart_node_unlock(topic->n, acquired);
    return r;
}

/* Reflection getters for the patterns layer's entity enumeration (read-only, stable). */
uint8_t    i_dart_topic_kind (const DartTopic *topic){ return topic ? topic->kind : 0; }
uint8_t    i_dart_topic_role (const DartTopic *topic){ return topic ? topic->role : (uint8_t)DART_INACTIVE; }
DartString i_dart_topic_name (const DartTopic *topic){
    return topic ? dart_string(topic->name, topic->name_len) : dart_string(NULL, 0);
}
uint16_t   i_dart_node_topic_count(DartNode *n){ return n ? i_dart_node_topic_hi(n) : 0; }

void i_dart_node_set_sys_hooks(DartNode *n, i_DartSysEventFn on_event, i_DartSysTickFn tick,
                               i_DartSysCloseFn on_close, void *user){
    int acquired;
    if (!n) return;
    acquired = i_dart_node_lock(n);
    n->sys_on_event = on_event; n->sys_tick = tick; n->sys_on_close = on_close; n->sys_user = user;
    n->sys_tick_next = 0;
    i_dart_node_kick(n);   /* re-evaluate the wait cap with the new tick */
    i_dart_node_unlock(n, acquired);
}

int dart_topic_set_role(DartTopic *topic, DartRole role){
    int r, acquired;
    if (!topic) return -1;
    acquired = i_dart_node_lock(topic->n);
    if (!acquired){
        /* from a callback: the discovery replay would rematch/reset the very reader
           proxy mid-delivery, so refuse loudly */
        return DART_ERR_STATE;
    }
    r = dart_transport_set_role(topic->n->transport, topic->index, (uint8_t)role);
    if (r == 0) topic->role = (uint8_t)role;
    if (r == 0){   /* re-advertise our interest: peers rematch as the new blob arrives */
        i_dart_node_core_build_meta(topic->n->core);
        dart_discovery_advertise(topic->n->discovery, i_dart_node_core_meta(topic->n->core));
        dart_discovery_replay(topic->n->discovery);   /* re-apply peers' interest to our new role */
        topic->n->match_epoch++;   /* a role flip may raise fresh candidates: memos are stale */
        i_dart_node_kick(topic->n);                   /* announce the change now */
    }
    i_dart_node_unlock(topic->n, acquired);
    return r;
}

uint16_t dart_topic_index(const DartTopic *topic){ return topic ? topic->index : 0; }

const DartSchema *dart_topic_schema(const DartTopic *topic){ return topic ? topic->schema : NULL; }

DartTopic *dart_node_topic(DartNode *n, uint16_t index){
    DartTopic *h; int acquired;
    if (!n) return NULL;
    acquired = i_dart_node_lock(n);
    h = (index < i_dart_node_topic_hi(n)) ? n->handles[index] : NULL;   /* holes yield NULL */
    i_dart_node_unlock(n, acquired);
    return h;
}

/* Read-only peer view: discovery already packs its peer table into a zero-copy array, and a
 * node peer IS a discovery peer (it adds only the decoded overlay, read on demand via
 * dart_node_peer_frag / dart_node_peer_interest_next). So this just forwards. With a poller
 * on another thread, bracket the CALL AND THE USE with dart_node_lock/dart_node_unlock
 * (from inside a callback the view is already safe for the callback's duration). */
const DartDiscoveryPeer *dart_node_peers(DartNode *n, uint16_t *count){
    return dart_discovery_peers(n ? n->discovery : NULL, count);
}

/* Greedy detail-cache queries (opts.fetch_details): a peer topic's fetched name and
 * parsed schema by (peer id, index). Same view rules as dart_node_peers. */
DartString dart_node_peer_topic_name(DartNode *n, uint32_t peer, uint16_t index){
    DartString s = dart_string(NULL, 0); int acquired;
    if (!n) return s;
    acquired = i_dart_node_lock(n);
    i_dart_node_core_topic_detail(n->core, peer, index, &s, NULL, NULL);
    i_dart_node_unlock(n, acquired);
    return s;
}

const DartSchema *dart_node_peer_topic_schema(DartNode *n, uint32_t peer, uint16_t index,
                                              uint64_t *schema_hash){
    const DartSchema *sch = NULL; int acquired;
    if (schema_hash) *schema_hash = 0;
    if (!n) return NULL;
    acquired = i_dart_node_lock(n);
    i_dart_node_core_topic_detail(n->core, peer, index, NULL, &sch, schema_hash);
    i_dart_node_unlock(n, acquired);
    return sch;
}

void dart_node_backpressure_stats(DartNode *n, uint64_t *waited_us, uint32_t *waited_sends){
    int acquired = i_dart_node_lock(n);
    if (waited_us)    *waited_us    = n->backpressure_total_us;
    if (waited_sends) *waited_sends = n->backpressure_wait_count;
    i_dart_node_unlock(n, acquired);
}

/* Sends that evicted never-emitted history after the bounded wait (the DART_E_EVICTED_UNSENT
 * count) since open: the burst/overload indicator. */
uint32_t dart_node_evicted_unsent(DartNode *n){
    uint32_t v; int acquired;
    if (!n) return 0;
    acquired = i_dart_node_lock(n);
    v = n->evicted_unsent;
    i_dart_node_unlock(n, acquired);
    return v;
}

void dart_node_mem_stats(DartNode *n, size_t *in_use, size_t *peak, uint64_t *alloc_calls){
    int acquired;
    if (!n) return;
    acquired = i_dart_node_lock(n);
    dart_allocator_stats(&n->pool, in_use, peak, alloc_calls);   /* live bytes, high-water, (re)allocs */
    i_dart_node_unlock(n, acquired);
}

void dart_topic_repair_stats(DartTopic *topic, DartRepairStats *out){
    if (topic){
        int acquired = i_dart_node_lock(topic->n);
        dart_transport_repair_stats(topic->n->transport, topic->index, out);
        i_dart_node_unlock(topic->n, acquired);
    } else if (out) memset(out, 0, sizeof *out);
}

void dart_topic_counts(DartTopic *topic, uint64_t *tx_msgs, uint64_t *tx_bytes,
                       uint64_t *rx_msgs, uint64_t *rx_bytes){
    uint64_t tm = 0, tb = 0, rm = 0, rb = 0;
    if (topic){
        int acquired = i_dart_node_lock(topic->n);
        tm = topic->tx_msgs; tb = topic->tx_bytes; rm = topic->rx_msgs; rb = topic->rx_bytes;
        i_dart_node_unlock(topic->n, acquired);
    }
    if (tx_msgs)  *tx_msgs  = tm;
    if (tx_bytes) *tx_bytes = tb;
    if (rx_msgs)  *rx_msgs  = rm;
    if (rx_bytes) *rx_bytes = rb;
}

/* ---- the @dart/meta snapshot builder (runtime.h DART_META_*) ------------------------ */

/* one pass of the map body; a latched writer error (out of room) makes finish return 0
 * and the caller grows + rebuilds */
static void i_dart_node_snapshot_fill(DartNode *n, DartMapWriter *w, uint32_t sections){
    uint64_t now = i_dart_plat_now_us();
    if (sections & DART_META_NODE){
        size_t in_use = 0, peak = 0; uint64_t allocs = 0;
        dart_allocator_stats(&n->pool, &in_use, &peak, &allocs);
        dart_map_open_map(w, "node");
        dart_map_put_string(w, "name", dart_string(n->name, n->name_len));
        dart_map_put_uint(w, "uptime_us", now - n->open_us);
        dart_map_put_uint(w, "mono_us", now);
        dart_map_put_uint(w, "wall_us", i_dart_plat_wall_us());
        dart_map_put_uint(w, "mem_in_use", (uint64_t)in_use);
        dart_map_put_uint(w, "mem_peak", (uint64_t)peak);
        dart_map_put_uint(w, "alloc_calls", allocs);
        dart_map_put_uint(w, "evicted_unsent", n->evicted_unsent);
        dart_map_put_uint(w, "bp_waited_us", n->backpressure_total_us);
        dart_map_put_uint(w, "bp_waits", n->backpressure_wait_count);
        dart_map_put_uint(w, "peers", dart_discovery_peer_count(dart_discovery_state(n->discovery)));
        dart_map_put_uint(w, "max_peers", n->max_peers);
        dart_map_put_uint(w, "topics", (uint64_t)(n->n_created + n->n_builtin));
        dart_map_put_uint(w, "max_topics", n->max_topics);
#ifdef DART_SHM
        dart_map_put_uint(w, "shm_tx", n->shm_tx);
        dart_map_put_uint(w, "shm_rx", n->shm_rx);
#endif
        dart_map_put_uint(w, "last_error", (uint64_t)n->last_error.error);
        if (n->last_error.error != DART_E_NONE){
            /* format from a SANITIZED copy: the stored event's name views can outlive
               what they pointed at (a gone peer, a relocated table), so drop them and
               let the text carry the indices instead */
            DartEvent le = n->last_error; char txt[160];
            le.topic_name = NULL; le.peer_name = NULL; le.schema_detail = NULL;
            dart_event_str(&le, txt, sizeof txt);
            dart_map_put_string(w, "last_error_text", dart_cstr(txt));
        }
        dart_map_close(w);
    }
#ifdef DART_PROC_STATS
    if (sections & DART_META_PROC){
        uint64_t cpu = 0, rss = 0, peak_rss = 0;
        if (i_dart_plat_proc_stats(&cpu, &rss, &peak_rss)){   /* absent where unsupported */
            dart_map_open_map(w, "proc");
            dart_map_put_uint(w, "pid", i_dart_plat_pid());
            dart_map_put_uint(w, "cpu_us", cpu);
            dart_map_put_uint(w, "rss", rss);
            dart_map_put_uint(w, "peak_rss", peak_rss);
            dart_map_close(w);
        }
    }
#endif
    if (sections & DART_META_TOPICS){
        uint16_t i, hi = i_dart_node_topic_hi(n);
        dart_map_open_array(w, "topics");
        for (i = 0; i < hi; i++){
            DartTopic *h = n->handles[i];
            const DartQos *q = dart_transport_topic_qos(n->transport, i);
            DartRepairStats rs;
            if (!h) continue;
            dart_transport_repair_stats(n->transport, i, &rs);
            dart_map_open_map(w, NULL);
            dart_map_put_uint(w, "index", i);
            dart_map_put_string(w, "name", dart_string(h->name, h->name_len));
            dart_map_put_uint(w, "kind", h->kind);
            dart_map_put_uint(w, "role", h->role);
            dart_map_put_bool(w, "reliable", q && q->reliability == DART_RELIABLE);
            dart_map_put_uint(w, "keep_last", q ? q->keep_last : 0);
            dart_map_put_uint(w, "catch_up", q ? q->catch_up : 0);
            dart_map_put_uint(w, "subs", (uint64_t)dart_transport_publisher_match_count(n->transport, i));
            dart_map_put_uint(w, "pubs", (uint64_t)dart_transport_subscriber_match_count(n->transport, i));
            dart_map_put_uint(w, "pending", (uint64_t)i_dart_node_core_topic_unresolved(n->core, i));
            dart_map_put_uint(w, "tx_msgs", h->tx_msgs);
            dart_map_put_uint(w, "tx_bytes", h->tx_bytes);
            dart_map_put_uint(w, "rx_msgs", h->rx_msgs);
            dart_map_put_uint(w, "rx_bytes", h->rx_bytes);
            dart_map_put_uint(w, "nacks_recv", rs.nacks_recv);
            dart_map_put_uint(w, "frags_resent", rs.frags_resent);
            dart_map_put_uint(w, "frags_sent", rs.frags_sent);
            dart_map_put_uint(w, "nacks_sent", rs.nacks_sent);
            dart_map_put_uint(w, "frags_recv", rs.frags_recv);
            dart_map_put_uint(w, "frags_dup", rs.frags_dup);
            dart_map_put_uint(w, "msgs_skipped", rs.msgs_skipped);
            if (h->q){
                dart_map_put_uint(w, "q_msgs", h->q->count);
                dart_map_put_uint(w, "q_bytes", h->q->bytes);
                dart_map_put_uint(w, "q_cap", h->q->cap);
                dart_map_put_uint(w, "q_dropped", h->q->dropped);
            }
            dart_map_close(w);
        }
        dart_map_close(w);
    }
    if (sections & DART_META_PEERS){
        uint16_t cnt = 0, i;
        const DartDiscoveryPeer *ps = dart_discovery_peers(n->discovery, &cnt);
        dart_map_open_array(w, "peers");
        for (i = 0; i < cnt; i++){
            uint16_t pub_to = 0, recv_from = 0;
            char ip[16]; int ln = 0;
            dart_transport_peer_match_counts(n->transport, ps[i].id, &pub_to, &recv_from);
            dart_map_open_map(w, NULL);
            dart_map_put_uint(w, "id", ps[i].id);
            dart_map_put_string(w, "name", ps[i].name);
            dart_map_put_bool(w, "active", ps[i].liveness == DART_PEER_ACTIVE);
            if (ps[i].addr.ip_len == 4)
                ln = snprintf(ip, sizeof ip, "%u.%u.%u.%u", ps[i].addr.ip[0], ps[i].addr.ip[1],
                              ps[i].addr.ip[2], ps[i].addr.ip[3]);
            dart_map_put_string(w, "ip", dart_string(ip, ln > 0 ? (size_t)ln : 0));
            dart_map_put_uint(w, "port", ps[i].addr.port);
            dart_map_put_uint(w, "age_us", now > ps[i].last_heard_us ? now - ps[i].last_heard_us : 0);
            dart_map_put_uint(w, "publish_to", pub_to);
            dart_map_put_uint(w, "receive_from", recv_from);
            dart_map_close(w);
        }
        dart_map_close(w);
    }
}

DartBytes i_dart_node_snapshot(DartNode *n, uint32_t sections){
    uint32_t body;
    if (!n) return dart_bytes(NULL, 0);
    if (!sections) sections = 0xFFFFFFFFu;
    for (;;){
        DartMapWriter w;
        if (!n->snap_buf){
            n->snap_buf = (uint8_t*)i_dart_node_alloc(n, NULL, 4096u);
            if (!n->snap_buf) return dart_bytes(NULL, 0);
            n->snap_cap = 4096u;
        }
        w = dart_map_begin(n->snap_buf, n->snap_cap);
        i_dart_node_snapshot_fill(n, &w, sections);
        body = dart_map_finish(&w);
        if (body) break;
        {   /* did not fit (the writer latches on any failure): double and rebuild */
            uint32_t ncap = n->snap_cap * 2u; uint8_t *nb;
            if (ncap > (1u << 22)) return dart_bytes(NULL, 0);   /* 4 MB: not a size problem */
            nb = (uint8_t*)i_dart_node_alloc(n, n->snap_buf, ncap);
            if (!nb) return dart_bytes(NULL, 0);
            n->snap_buf = nb; n->snap_cap = ncap;
        }
    }
    return dart_bytes(n->snap_buf, body);
}

void dart_node_set_pump_probe(DartNode *n, DartPumpProbeFn fn, uint64_t interval_us, void *user){
    int acquired = i_dart_node_lock(n);
    n->pump_probe = fn; n->pump_probe_interval_us = interval_us; n->pump_probe_user = user;
    i_dart_node_unlock(n, acquired);
}

int dart_topic_subscriber_progress(DartTopic *topic, uint32_t peer,
                                 uint64_t *base_seqno, uint32_t *have, uint32_t *total){
    int r, acquired;
    if (!topic) return 0;
    acquired = i_dart_node_lock(topic->n);
    r = dart_transport_subscriber_progress(topic->n->transport, topic->index, peer, base_seqno, have, total);
    i_dart_node_unlock(topic->n, acquired);
    return r;
}

#ifdef DART_SHM
void dart_node_shm_stats(DartNode *n, uint32_t *sent, uint32_t *recv){
    int acquired = i_dart_node_lock(n);
    if (sent) *sent = n->shm_tx;
    if (recv) *recv = n->shm_rx;
    i_dart_node_unlock(n, acquired);
}
#endif

int dart_topic_drain(DartTopic *topic, int timeout_ms){
    DartNode *n; uint64_t deadline; int acquired, drained = 1;
    if (!topic) return 0;
    n = topic->n;
    acquired = i_dart_node_lock(n);
    if (!acquired) return 0;   /* from a callback: can neither pump nor wait */
    deadline = i_dart_plat_now_us() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000u;
#ifdef DART_THREADS
    if (n->svc_running){
        n->cv_waiters++;
        while (!dart_transport_send_drained(n->transport, topic->index)){
            uint64_t now = i_dart_plat_now_us();
            if (now >= deadline){ drained = 0; break; }
            i_dart_node_kick(n);
            i_dart_node_cv_wait(n, deadline - now);
            if (!n->svc_running) break;   /* stopped under us: finish with the pump below */
        }
        n->cv_waiters--;
        if (n->svc_running || !drained){
            i_dart_node_unlock(n, acquired);
            return drained;
        }
    }
#endif
    while (!dart_transport_send_drained(n->transport, topic->index)){
        if (i_dart_plat_now_us() >= deadline){ drained = 0; break; }
        i_dart_node_poll_locked(n, 1, 0);
    }
    i_dart_node_unlock(n, acquired);
    return drained;
}

int dart_topic_match_count(DartTopic *topic){
    int r, acquired;
    if (!topic) return 0;
    acquired = i_dart_node_lock(topic->n);
    r = dart_transport_publisher_match_count(topic->n->transport, topic->index);
    i_dart_node_unlock(topic->n, acquired);
    return r;
}

int dart_topic_pending_count(DartTopic *topic){
    int r, acquired;
    if (!topic) return 0;
    acquired = i_dart_node_lock(topic->n);
    r = i_dart_node_core_topic_unresolved(topic->n->core, topic->index);
    i_dart_node_unlock(topic->n, acquired);
    return r;
}

/* 1 = a send now would not wait (see runtime.h): matched, or converged with nobody to
 * wait for. Shares the send path's own predicate, so a GUI polling this then sending
 * observes exactly what the send would have decided. */
int dart_topic_ready(DartTopic *topic){
    int r, acquired;
    if (!topic) return 0;
    acquired = i_dart_node_lock(topic->n);
    r = dart_transport_publisher_match_count(topic->n->transport, topic->index) > 0
     || !i_dart_node_topic_unsettled(topic->n, topic, i_dart_plat_now_us());
    i_dart_node_unlock(topic->n, acquired);
    return r;
}

/* settled = the network answered and went quiet. dart_node_settle SOLICITS (the same
 * mechanism dart_discovery_gather uses: peers reply immediately with targeted unicast
 * announces), so on a populated network every existing peer is heard within an RTT and
 * this returns after one quiet window (~300ms), not a full announce interval. Three
 * conditions, under the node lock:
 *   - every ACTIVE peer has been heard since the solicit went out (it answered), and
 *   - the peer/match topology has been QUIET for a beat (no PEER_UP/DOWN/INTEREST change:
 *     announces only re-apply interest when something actually changed), and
 *   - one quiet window has passed overall (a straggler's first announce gets its chance).
 * With NO peer heard at all, only a full announce interval can rule out a slow one. */
static int i_dart_node_settled(DartNode *n, uint64_t start, uint64_t now){
    uint64_t quiet = n->announce_us < 300000u ? n->announce_us : 300000u;
    uint16_t i, count = 0;
    const DartDiscoveryPeer *peers = dart_discovery_peers(n->discovery, &count);
    int any = 0;
    for (i = 0; i < count; i++){
        if (peers[i].liveness != DART_PEER_ACTIVE) continue;   /* dropped: not expected to answer */
        any = 1;
        if (peers[i].last_heard_us < start) return 0;          /* has not answered the solicit yet */
    }
    if (!any) return now - start >= n->announce_us;            /* alone, as far as we can know */
    if (now - start < quiet) return 0;
    return n->settle_topology_us < start || now - n->settle_topology_us >= quiet;
}

int dart_node_settle(DartNode *n, int timeout_ms){
    uint64_t start, deadline, last_solicit; int acquired, settled = 1;
    if (!n) return 0;
    acquired = i_dart_node_lock(n);
    if (!acquired){ return 0; }   /* from a callback: can neither pump nor wait */
    start = i_dart_plat_now_us();
    last_solicit = 0;
    deadline = start + (timeout_ms >= 0 ? (uint64_t)timeout_ms * 1000u
                                        : (uint64_t)n->announce_us * 3u);
#ifdef DART_THREADS
    if (n->svc_running){
        n->cv_waiters++;
        while (!i_dart_node_settled(n, start, i_dart_plat_now_us())){
            uint64_t now = i_dart_plat_now_us();
            if (now >= deadline){ settled = 0; break; }
            if (now - last_solicit >= 250000u){   /* (re)solicit ~4x/s so a lost one retries */
                dart_discovery_solicit(dart_discovery_state(n->discovery));
                i_dart_node_kick(n);              /* the service thread sends it now */
                last_solicit = now;
            }
            {   uint64_t left = deadline - now;
                i_dart_node_cv_wait(n, left < 50000u ? left : 50000u);   /* re-check the clock */
            }
            if (!n->svc_running) break;   /* stopped under us: finish with the pump below */
        }
        n->cv_waiters--;
        if (n->svc_running || !settled){
            i_dart_node_unlock(n, acquired);
            return settled;
        }
    }
#endif
    while (!i_dart_node_settled(n, start, i_dart_plat_now_us())){
        uint64_t now = i_dart_plat_now_us();
        if (now >= deadline){ settled = 0; break; }
        if (now - last_solicit >= 250000u){
            dart_discovery_solicit(dart_discovery_state(n->discovery));
            last_solicit = now;
        }
        i_dart_node_poll_locked(n, 1, 0);   /* sends the solicit, takes in the replies */
    }
    i_dart_node_unlock(n, acquired);
    return settled;
}

/* ---- consumer-queue API (runtime.h "consumer queues") ------------------------------- */

static int i_dart_node_any_queued(DartNode *n){
    uint16_t i, hi = i_dart_node_topic_hi(n);
    for (i = 0; i < hi; i++)
        if (n->handles[i] && n->handles[i]->q && n->handles[i]->q->count) return 1;
    return 0;
}

/* Wait until data is queued: on q when given, else on ANY queued topic. Sleeps on the
 * service thread's progress when one runs (its end-of-pass broadcast covers every queue
 * push); otherwise nobody else is obliged to fill the queue, so it drives the poll loop
 * itself (which is what makes take/dispatch work with a manual poll cadence, alongside
 * other pollers, and under DART_NO_THREADS). Lock held on entry and exit; only called on
 * our own acquisition (never from a callback). */
static void i_dart_node_queue_wait(DartNode *n, i_DartMsgQueue *q, int timeout_ms){
    uint64_t now = i_dart_plat_now_us();
    uint64_t deadline = timeout_ms < 0 ? (uint64_t)-1 : now + (uint64_t)timeout_ms * 1000u;
    while (!(q ? q->count : (uint32_t)i_dart_node_any_queued(n))){
        uint64_t left;
        now = i_dart_plat_now_us();
        if (now >= deadline) break;
        left = (deadline == (uint64_t)-1) ? 3600000000u : deadline - now;
#ifdef DART_THREADS
        if (n->svc_running){
            n->cv_waiters++;
            i_dart_node_cv_wait(n, left);   /* mu drops: nothing cached survives */
            n->cv_waiters--;
            continue;
        }
#endif
        {   int ms = left >= 1000u ? (left / 1000u > 0x7FFFFFFFu ? 0x7FFFFFFF
                                                                 : (int)(left / 1000u)) : 1;
            i_dart_node_poll_locked(n, ms, 1);   /* outer: the wait drops the lock */
        }
    }
}

/* dispatch up to max_msgs of the messages queued at entry, callbacks on the calling
 * thread and OUTSIDE the lock when this thread owns the acquisition (so they may use the
 * whole API); a reentrant caller (from a callback) keeps the lock, like any callback. */
static int i_dart_topic_dispatch_locked(DartNode *n, DartTopic *h, int max_msgs, int acquired){
    i_DartMsgQueue *q = h->q;
    int done = 0; uint32_t todo;
    if (!q || q->busy) return 0;
    i_dart_node_queue_release(n, h, q);
    todo = q->count;                    /* snapshot: later arrivals wait for the next call */
    if (max_msgs > 0 && todo > (uint32_t)max_msgs) todo = (uint32_t)max_msgs;
    while (todo-- && q->count){
        const i_DartQRec *rec = i_dart_q_peek(q);
        DartMsg m;
        i_dart_node_queue_msg(n, h, rec, &m);
        q->viewing = 1;
        if (n->user_on_message){
            q->busy = 1;
#ifdef DART_THREADS
            if (acquired){
                i_dart_node_unlock_raw(n);
                n->user_on_message(&m);
                i_dart_node_lock_raw(n);
            } else
#endif
            n->user_on_message(&m);
            q->busy = 0;
        }
        i_dart_node_queue_release(n, h, q);
        done++;
    }
    (void)acquired;
    return done;
}

int dart_topic_take(DartTopic *topic, DartMsg *out, int timeout_ms){
    DartNode *n; i_DartMsgQueue *q; int acquired, got = 0;
    if (!topic || !out) return DART_ERR_NO_TOPIC;
    n = topic->n;
    acquired = i_dart_node_lock(n);
    q = i_dart_node_queue_ensure(n, topic, NULL);
    if (!q){ i_dart_node_unlock(n, acquired); return DART_ERR_OOM; }
    if (q->busy){ i_dart_node_unlock(n, acquired); return DART_ERR_STATE; }
    i_dart_node_queue_release(n, topic, q);      /* finish the previous view first */
    if (!q->count && timeout_ms != 0 && acquired)
        i_dart_node_queue_wait(n, q, timeout_ms);
    {   const i_DartQRec *rec = i_dart_q_peek(q);
        if (rec){
            i_dart_node_queue_msg(n, topic, rec, out);
            q->viewing = 1;                   /* the view lives until the next take/dispatch */
            got = 1;
        }
    }
    i_dart_node_unlock(n, acquired);
    return got;
}

int dart_topic_dispatch(DartTopic *topic, int max_msgs, int timeout_ms){
    DartNode *n; i_DartMsgQueue *q; int acquired, done;
    if (!topic) return DART_ERR_NO_TOPIC;
    n = topic->n;
    acquired = i_dart_node_lock(n);
    q = i_dart_node_queue_ensure(n, topic, NULL);
    if (!q){ i_dart_node_unlock(n, acquired); return DART_ERR_OOM; }
    if (q->busy){ i_dart_node_unlock(n, acquired); return DART_ERR_STATE; }
    i_dart_node_queue_release(n, topic, q);
    if (!q->count && timeout_ms != 0 && acquired)
        i_dart_node_queue_wait(n, q, timeout_ms);
    done = i_dart_topic_dispatch_locked(n, topic, max_msgs, acquired);
    i_dart_node_unlock(n, acquired);
    return done;
}

int dart_node_dispatch(DartNode *n, int max_msgs, int timeout_ms){
    int acquired, total = 0;
    uint16_t i;
    if (!n) return DART_ERR_STATE;
    acquired = i_dart_node_lock(n);
    if (!i_dart_node_any_queued(n) && timeout_ms != 0 && acquired)
        i_dart_node_queue_wait(n, NULL, timeout_ms);
    for (i = 0; i < i_dart_node_topic_hi(n); i++){
        DartTopic *h = n->handles[i];
        if (!h || !h->q || !h->q->count) continue;
        total += i_dart_topic_dispatch_locked(n, h, max_msgs > 0 ? max_msgs - total : 0, acquired);
        if (max_msgs > 0 && total >= max_msgs) break;
    }
    i_dart_node_unlock(n, acquired);
    return total;
}

void dart_topic_queue_stats(DartTopic *topic, uint32_t *msgs, uint32_t *bytes,
                              uint32_t *capacity, uint32_t *dropped){
    uint32_t m = 0, b = 0, c = 0, d = 0;
    if (topic && topic->q){
        int acquired = i_dart_node_lock(topic->n);
        m = topic->q->count; b = topic->q->bytes; c = topic->q->cap; d = topic->q->dropped;
        i_dart_node_unlock(topic->n, acquired);
    }
    if (msgs)     *msgs = m;
    if (bytes)    *bytes = b;
    if (capacity) *capacity = c;
    if (dropped)  *dropped = d;
}

#ifdef DART_THREADS
/* the service thread: the same poll body in a loop, sleeping exactly until the next
 * timer (or a waker kick). It holds the lock for every work pass and drops it inside
 * poll_locked's blocking wait, so user threads only ever contend with bounded work. */
static void i_dart_node_service(void *arg){
    DartNode *n = (DartNode *)arg;
    i_dart_node_lock_raw(n);
    while (!n->svc_stop)
        i_dart_node_poll_locked(n, 3600000, 1);   /* cap is moot: timers/kicks wake it */
    i_dart_node_unlock_raw(n);
}
#endif

int dart_node_start(DartNode *n){
#ifndef DART_THREADS
    (void)n;
    return DART_ERR_NOSYS;
#else
    int acquired;
    if (!n) return DART_ERR_STATE;
    acquired = i_dart_node_lock(n);
    if (!acquired) return DART_ERR_STATE;               /* from a callback */
    if (n->svc_running){ i_dart_node_unlock(n, acquired); return DART_ERR_STATE; }
    n->svc_stop = 0;
    n->svc_running = 1;
    if (!i_dart_plat_thread_start(&n->svc, i_dart_node_service, n)){
        n->svc_running = 0;
        i_dart_node_unlock(n, acquired);
        return DART_ERR_NOSYS;
    }
    i_dart_node_unlock(n, acquired);                    /* the service takes the lock now */
    return DART_OK;
#endif
}

int dart_node_stop(DartNode *n){
#ifndef DART_THREADS
    (void)n;
    return DART_OK;                                     /* nothing to stop; idempotent */
#else
    int acquired;
    if (!n) return DART_ERR_STATE;
    acquired = i_dart_node_lock(n);
    if (!acquired) return DART_ERR_STATE;               /* from a callback (it IS the service) */
    if (n->svc_joining){
        /* another thread owns the join: wait for it to finish rather than joining
           the same thread twice (UB). Counted as a cv waiter so its broadcasts land. */
        n->cv_waiters++;
        while (n->svc_running) i_dart_node_cv_wait(n, 100000u);
        n->cv_waiters--;
        i_dart_node_unlock(n, acquired);
        return DART_OK;
    }
    if (!n->svc_running){ i_dart_node_unlock(n, acquired); return DART_OK; }
    n->svc_joining = 1;
    n->svc_stop = 1;
    i_dart_node_kick(n);
    i_dart_node_unlock(n, acquired);
    i_dart_plat_thread_join(&n->svc);                   /* NEVER joined holding the lock */
    acquired = i_dart_node_lock(n);
    n->svc_running = 0;
    n->svc_stop = 0;
    n->svc_joining = 0;
    /* free every waiter: blocked senders see svc_running == 0 and proceed unwaited,
       and any concurrent stopper parked above returns */
    if (n->cv_waiters) i_dart_plat_cond_broadcast(&n->cv);
    i_dart_node_unlock(n, acquired);
    return DART_OK;
#endif
}

int dart_node_is_started(DartNode *n){
#ifndef DART_THREADS
    (void)n;
    return 0;
#else
    return (n && n->svc_running) ? 1 : 0;
#endif
}

void dart_node_lock(DartNode *n){
#ifndef DART_THREADS
    (void)n;
#else
    if (!n) return;
    if (n->lock_held && n->lock_owner == i_dart_plat_thread_id()) return;  /* already ours (a callback) */
    i_dart_node_lock_raw(n);
    n->user_locked = 1;
#endif
}

void dart_node_unlock(DartNode *n){
#ifndef DART_THREADS
    (void)n;
#else
    if (!n || !n->user_locked) return;   /* only releases what dart_node_lock took */
    if (!(n->lock_held && n->lock_owner == i_dart_plat_thread_id())) return;
    n->user_locked = 0;
    i_dart_node_unlock_raw(n);
#endif
}

int dart_node_close(DartNode *n, int send_bye){
    DartAllocator pool;
    if (!n) return DART_OK;
#ifdef DART_THREADS
    /* from a callback: refuse LOUDLY (the return code lets a binding keep its handle
       alive instead of stranding a running node it can never close again) */
    if (n->lock_held && n->lock_owner == i_dart_plat_thread_id()) return DART_ERR_STATE;
    dart_node_stop(n);
    /* from here the contract holds: no other thread is inside, or may enter, any
       dart_* call on this node, so the teardown runs truly single-threaded */
#endif
    /* settle outstanding pattern promises (pending calls get CANCELLED) while the node is
       still fully alive; cleared first so a callback closing the node cannot recurse */
    if (n->sys_on_close){
        i_DartSysCloseFn f = n->sys_on_close;
        n->sys_on_close = NULL;
        f(n->sys_user);
    }
    if (n->discovery) dart_discovery_close(n->discovery, send_bye);   /* frees peer blobs via our
                                                                         hook, so it must run BEFORE
                                                                         the pool is copied out */
    if (n->fd != DART_SOCK_BAD) i_dart_plat_close(n->fd);
#ifdef DART_SHM
    if (n->shm_capable){                              /* unmap OS segments; the pool reset frees only pool memory */
        uint32_t i, n_segments = (uint32_t)n->shm_n_topics * DART_SHM_N_CLASSES;
        for (i=0;i<n_segments;i++)
            if (n->shm_pool[i]) i_dart_shm_detach((i_DartShmPool*)n->shm_pool[i]);   /* unlinks ours */
        for (i=0;i<n->shm_reader_count;i++)
            if (n->shm_reader_states[i]) i_dart_shm_detach((i_DartShmPool*)n->shm_reader_states[i]);
    }
#endif
#ifdef DART_THREADS
    if (n->waker.fd != DART_SOCK_BAD) i_dart_plat_waker_close(&n->waker);
    i_dart_plat_cond_destroy(&n->cv);
    i_dart_plat_mutex_destroy(&n->mu);
#endif
    i_dart_plat_cleanup();
    pool = n->pool;                /* copy out LAST (no hook frees may follow): the reset frees n itself */
    dart_allocator_reset(&pool);   /* frees the node struct, arena, message buffers, schemas, handles */
    return DART_OK;
}
