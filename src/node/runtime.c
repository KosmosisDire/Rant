/* NODE runtime: owns the data sockets, drives discovery, and the clock; it wires
 * peers into the transport via the sans-IO node core (node/core.h). Channels are
 * created at runtime and handed back as opaque handles. All OS access goes through
 * dart_plat. See node/runtime.h for the public dart_node_* / dart_channel_* API. */

#include "runtime.h"   /* public API + config structs */
#include "core.h"      /* sans-IO node core: peer table + discovery->transport lifecycle */
#include "../discovery/runtime.h"
#include "../platform/core.h"
#ifdef DART_SHM
#include "../shm/core.h"
#endif
#include "../common/arena.h"
#include <string.h>    /* heap access goes through i_dart_plat_realloc (no <stdlib.h> here) */

struct DartChannel { DartNode *n; uint16_t index; DartSchema *schema; };   /* schema: node-owned copy */

struct DartNode {
    DartTransportState     *transport;
    i_DartNodeCore *core;     /* peer table (id<->address) + discovery lifecycle (sans-IO) */
    DartDiscovery     *discovery;
    i_DartSock       fd;       /* unicast data socket */
    uint16_t      domain;
    DartNodeNet  net;         /* copy of opts.net: socket buffers + discovery addressing */
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
    /* diagnostic: in-pump probe sampled inside the backpressure wait (dart_channel_send) */
    DartPumpProbeFn pump_probe;
    void          *pump_probe_user;
    uint64_t       pump_probe_interval_us;
    /* app callbacks: one message sink (wrapped to build a DartMsg) + everything-else */
    DartMsgFn    user_on_message;
    DartEventFn  on_event;
    void          *user_data;
    DartEvent     last_error;  /* most recent DART_ERROR (dart_last_error(n)); DART_E_NONE until one fires */
    void          *arena;      /* control-structs block (a freeable pool allocation); relocated on grow */
    /* the node's paged region allocator (common/alloc.h): backs the node struct, arena, message
       buffers, schemas, and handles. COPIED from the caller's allocator at open (so the caller's
       may be a temporary); the node owns it and resets it on close. */
    DartAllocator pool;
    uint8_t        alloc_dynamic;
    uint8_t        grow_pending;   /* a peer was refused for lack of slots; grow at next poll */
    uint16_t       max_peers;      /* current peer-table capacity (doubles on a dynamic grow) */
    /* the metadata accept bound (largest peer announce blob we store/receive). Starts at
       dart_meta_capacity(max_channels) and SELF-HEALS: a peer whose blob exceeds it fires
       META_TOO_BIG with the needed size, we grow the bound + RX buffers at the next poll
       and solicit a re-announce, so a big-topology peer and a default node interoperate
       with no configuration. meta_grow_need is the pending request; meta_grow_failed
       remembers a size that would not allocate, so the retry loop surfaces instead of
       silently spinning. */
    uint16_t       meta_capacity;
    uint16_t       meta_grow_need;
    uint16_t       meta_grow_failed;
    /* channel handles. handles is a pointer array in the arena; each DartChannel struct
       is a separate stable allocation, so a grow that relocates the arena never moves a
       handle the user holds. */
    DartChannel **handles;    /* [max_channels] -> stable per-channel structs */
    uint16_t      max_channels;
    uint16_t      n_created;
#ifdef DART_SHM
    /* zero-fragment same-host path: lazy per-size-class segments (see shm/core.h) */
    uint8_t        shm_capable;       /* always 1: the node always has an allocator */
    uint8_t        shm_host[16];  /* our host uuid (advertised; same-host check) */
    uint64_t       shm_base;      /* per-node segment id base; low 19 bits = (channel<<3)|class */
    void          *shm_scratch; uint32_t shm_scratch_cap;   /* one-copy receive scratch */
    void         **shm_pool;      /* [nchan*N_CLASSES] our (channel,class) segments (NULL = not
                                     created); each created segment's state is a stable hook
                                     allocation, made when the segment is (lazily) created */
    uint16_t       shm_n_channels;
    /* reader-side attach cache, hook-allocated and grown on demand: sized by segments
       actually attached (same-host peers x their publishing channels), not the worst-case
       peers x channels x classes. A node with no same-host peer allocates none of it. */
    uint64_t      *shm_reader_segments;  /* [shm_reader_cap] attached segment ids */
    void         **shm_reader_states;    /* [shm_reader_cap] their pool states (stable allocations) */
    uint16_t       shm_reader_cap, shm_reader_count;
    uint32_t       shm_tx, shm_rx;/* messages published / delivered via SHM (observability) */
#endif
};

/* The node's message-buffer allocator, handed to the transport as its realloc hook and used
 * for schemas and channel handles (u is the node). It delegates to the node's paged region
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
static void i_dart_node_emit(DartNode *n, DartEvent *e){
    e->user = n->user_data;
    if (e->kind == DART_ERROR) n->last_error = *e;
    if (n->on_event) n->on_event(e);
}

/* our channel's name as a C string (the name pool is NUL-terminated), or NULL if the
 * channel is undefined: the channel_name view carried on channel-scoped events. */
static const char *i_dart_node_ch_name(DartNode *n, uint16_t ch){
    DartString s = dart_transport_channel_name(n->transport, ch);
    return (const char*)s.data;
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

/* build a DartMsg and hand it to the app (the channel name is a local lookup, never
 * on the wire). Shared by the inline and SHM delivery paths. A message that does not
 * fit its sender's declared schema broke the sender's own contract: dropped + surfaced,
 * never handed to the app to misdecode. */
static void i_dart_node_deliver(DartNode *n, uint16_t ch, uint32_t from, DartBytes data){
    DartMsg m;
    const DartSchema *schema = i_dart_node_core_msg_schema(n->core, from, ch);
    if (schema && !dart_schema_validate(schema, data)){
        DartEvent e; memset(&e, 0, sizeof e);
        e.kind = DART_ERROR; e.error = DART_E_SCHEMA_MISMATCH;
        e.peer = from; e.channel = ch; e.channel_name = i_dart_node_ch_name(n, ch);
        i_dart_node_emit(n, &e);
        return;
    }
    if (!n->user_on_message) return;
    memset(&m, 0, sizeof m);
    m.node = n; m.user = n->user_data;
    m.channel_id = ch; m.sender_id = from;
    m.sender_name = i_dart_node_core_peer_name(n->core, from);          /* view into discovery state */
    if (!m.sender_name.data) m.sender_name = dart_cstr("unknown-peer"); /* .data never NULL on delivery */
    m.channel_name = dart_transport_channel_name(n->transport, ch);
    m.data = data;
    m.schema = schema;
    n->user_on_message(&m);
}
static void i_dart_node_on_message(void *u, uint16_t ch, uint32_t from, DartBytes data){
    i_dart_node_deliver((DartNode*)u, ch, from, data);
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
        if (need > n->meta_capacity && need <= 65000u && (uint16_t)need != n->meta_grow_failed){
            n->meta_grow_need = (uint16_t)need;
            return;
        }
    }
    { DartEvent e = *ev; i_dart_node_emit(n, &e); }
}

/* the transport's schema gate (DartConfig.schema_check): the node core answers it over
 * the serialize layer, using the overlay it is currently applying. u is the node. */
static int i_dart_node_schema_check(void *u, uint16_t channel, uint16_t alias, int peer_is_pub){
    return i_dart_node_core_schema_check(((DartNode*)u)->core, channel, alias, peer_is_pub);
}

/* transport events arrive as a DartTransportEvent; the node maps them onto its app
 * DartEvent union and hands them on. This is the node combining the two lower layers'
 * events into one app callback (peer events come via the node core, above). */
static void i_dart_node_on_transport_event(const DartTransportEvent *tev){
    DartNode *n = (DartNode*)tev->user;
    DartEvent e;
    memset(&e, 0, sizeof e);
    e.peer = tev->peer; e.channel = tev->channel;
    e.lost_first = tev->lost_first; e.lost_count = tev->lost_count;
    e.too_big_bytes = tev->too_big_bytes; e.identity = tev->identity;
    /* MSG_LOST is a top-level info kind (best-effort loss is expected); everything else
       is a DART_ERROR carrying its specific DartErrorKind. Channel-scoped kinds get our
       channel name for the message string. */
    switch (tev->kind){
    case DART_TRANSPORT_MSG_LOST:
        e.kind = DART_MSG_LOST; e.channel_name = i_dart_node_ch_name(n, tev->channel); break;
    case DART_TRANSPORT_MSG_TOO_BIG:
        e.kind = DART_ERROR; e.error = DART_E_MSG_TOO_BIG; e.channel_name = i_dart_node_ch_name(n, tev->channel); break;
    case DART_TRANSPORT_NAME_COLLISION:
        e.kind = DART_ERROR; e.error = DART_E_NAME_COLLISION; e.channel_name = i_dart_node_ch_name(n, tev->channel); break;
    case DART_TRANSPORT_QOS_INCOMPATIBLE:
        e.kind = DART_ERROR; e.error = DART_E_QOS_INCOMPATIBLE; e.channel_name = i_dart_node_ch_name(n, tev->channel); break;
    case DART_TRANSPORT_SCHEMA_MISMATCH:
        e.kind = DART_ERROR; e.error = DART_E_SCHEMA_MISMATCH; e.channel_name = i_dart_node_ch_name(n, tev->channel); break;
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

static void i_dart_node_layout(i_DartBump *b, uint16_t max_peers, uint16_t max_channels,
                              const DartConfig *transport_cfg,
                              const DartDiscoveryNetConfig *discovery_rt_cfg, i_DartNodeBlocks *o){
    o->handles       = (uint8_t*)i_dart_bump_take(b, (size_t)max_channels * sizeof(DartChannel*), 16);
    o->node_core_bytes = i_dart_node_core_required_memory(max_channels, 1);   /* peers live in discovery; the
                                                                                 node always has an alloc hook,
                                                                                 so the blob is dynamic */
    o->node_core = (uint8_t*)i_dart_bump_take(b, o->node_core_bytes, 16);
    o->transport_bytes = dart_transport_required_memory(transport_cfg);
    o->transport = (uint8_t*)i_dart_bump_take(b, o->transport_bytes, 16);
#ifdef DART_SHM
    /* only the (channel,class) -> segment pointer table lives in the arena; segment state
       and the reader attach cache are lazy hook allocations, made when SHM is actually used */
    o->shm_pool = (uint8_t*)i_dart_bump_take(b, (size_t)max_channels * DART_SHM_N_CLASSES * sizeof(void*), 16);
#endif
    o->discovery_bytes = dart_discovery_placement_memory(discovery_rt_cfg);
    o->discovery = (uint8_t*)i_dart_bump_take(b, o->discovery_bytes, 16);
    /* data-socket RX buffer: sized so a unicast announce carrying the largest blob this
       node accepts fits (recvfrom drops an oversized datagram, which would leave a late
       joiner unable to ever fetch a big peer blob: its only path is this socket) */
    o->rx_buf_bytes = dart_discovery_wire_size(discovery_rt_cfg->discovery.meta_capacity);
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
/* lazily create our per-channel segment: chunk_bytes = the channel's locked size class,
 * n_chunks = its keep_last, so history slot i binds chunk i (no free list). NULL on fail. */
static i_DartShmPool *i_dart_node_shm_channel_pool(DartNode *n, uint16_t ch, uint32_t k, uint16_t keep_last){
    i_DartShmConfig c; uint8_t *mem; uint64_t seg; size_t idx;
    if (k >= DART_SHM_N_CLASSES) return NULL;
    idx = (size_t)ch * DART_SHM_N_CLASSES + k;
    if (n->shm_pool[idx]) return (i_DartShmPool*)n->shm_pool[idx];
    memset(&c, 0, sizeof c);
    seg = n->shm_base | ((uint64_t)ch << 3) | (uint64_t)k;   /* low 3 bits class, next 16 channel */
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
static int i_dart_node_on_shm(void *u, uint16_t ch, uint32_t from, const uint8_t *desc){
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
    n->shm_rx++;
    i_dart_node_deliver(n, ch, from, dart_bytes(n->shm_scratch, len));
    return 1;
}
#endif

DartNode *dart_node_open(DartAllocator *alloc, const char *name, DartMsgFn on_message, DartEventFn on_event, const DartNodeOpts *opts){
    DartNodeOpts o; DartDiscoveryNetConfig dc; DartConfig tc; i_DartNodeBlocks blocks;
    uint16_t max_peers, max_channels;
    uint8_t *base; void *arena; size_t need; DartAllocator pool;
    DartNode *n; i_DartSock fd; uint16_t local_port;
    char node_name[DART_NODE_NAME_MAX + 1]; uint8_t node_name_len = 0;   /* name is discovery-level */

    if (!alloc) return NULL;
    memset(&o, 0, sizeof o);
    if (opts) o = *opts;
    max_channels = o.max_channels ? o.max_channels : 8;
    max_peers    = o.discovery.max_peers ? o.discovery.max_peers : 16;

    /* sub-configs (sizing only depends on counts; the callback wrappers are set later) */
    memset(&dc,0,sizeof dc); memset(&tc,0,sizeof tc);
    dc.discovery.domain_id   = o.domain;
    dc.discovery.data_port   = o.net.data_port;
    dc.discovery.announce_interval_us = o.discovery.announce_interval_us;
    dc.discovery.peer_timeout_us = o.discovery.peer_timeout_us;
    dc.discovery.max_peers   = max_peers;
    dc.discovery.meta_capacity = dart_meta_capacity(max_channels);
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
    tc.channels    = NULL;            /* reserve mode: channels created at runtime */
    tc.n_channels  = max_channels;
    tc.max_peers   = max_peers;
    tc.frag_payload= o.net.fragment_size;
    tc.allocator   = i_dart_node_alloc;   /* non-NULL => reserve/dynamic mode in dart_transport_init */

    {   i_DartBump b; memset(&b,0,sizeof b);
        i_dart_node_layout(&b, max_peers, max_channels, &tc, &dc, &blocks);
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
        i_dart_node_layout(&b, max_peers, max_channels, &tc, &dc, &blocks); }

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
    n->user_on_message = on_message;   /* on_event + user_data were set early (open-time error reporting) */
    n->arena = arena;
    n->handles = (DartChannel**)blocks.handles;
    memset(n->handles, 0, (size_t)max_channels * sizeof(DartChannel*));
    n->rx_buf = blocks.rx_buf; n->rx_buf_bytes = blocks.rx_buf_bytes;
    n->max_channels = max_channels;
    n->max_peers = max_peers;
    n->meta_capacity = dc.discovery.meta_capacity;   /* the initial accept bound (self-heals up) */

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
        n->shm_base &= ~(((uint64_t)1u << 19) - 1u);       /* low 19 bits: 3 class + 16 channel index */
        if (n->shm_base == 0) n->shm_base = (uint64_t)1u << 19;
    }
    tc.on_shm = i_dart_node_on_shm;
#endif

    n->transport = dart_transport_init(blocks.transport, blocks.transport_bytes, &tc);
    if (!n->transport){ (void)i_dart_node_open_fail(on_event, o.user_data, DART_E_OOM, 0, 0, 0); goto fail_threads; }
#ifdef DART_SHM
    {   uint32_t n_segments = (uint32_t)max_channels * DART_SHM_N_CLASSES; uint32_t i;
        n->shm_n_channels = max_channels;
        n->shm_pool = (void**)blocks.shm_pool;
        for (i=0;i<n_segments;i++) n->shm_pool[i]=NULL;
        n->shm_reader_segments = NULL; n->shm_reader_states = NULL;   /* lazy: first attach allocates */
        n->shm_reader_cap = 0; n->shm_reader_count = 0;
    }
#endif

    /* sans-IO node core: drives the discovery->transport lifecycle and resolves addresses
       over the discovery core's peer table (bound below, once discovery exists). */
    node_name_len = dart_discovery_default_name(node_name, sizeof node_name, name);   /* handed to discovery below */
    {   i_DartNodeCoreConfig cc;
        memset(&cc, 0, sizeof cc);
        cc.transport = n->transport;   /* cc.discovery bound after dart_discovery_place */
        cc.n_channels = max_channels; cc.frag_size = dart_clamp_frag(o.net.fragment_size);
        cc.on_event = i_dart_node_on_event; cc.user = n;
        cc.alloc = i_dart_node_alloc; cc.alloc_user = n;   /* backs interned/rebased peer schemas */
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
 * a full peer table or channel reserve stops being a hard cap. Heap message buffers and SHM
 * writer segments stay put (only their owning control structures move); the user-held
 * DartNode and DartChannel handles live outside the arena, so they survive. Returns 1 with n
 * now on the new arena, or 0 if the bigger arena couldn't be allocated (n left unchanged). */
static int i_dart_node_grow(DartNode *n, uint16_t new_max_peers, uint16_t new_max_channels,
                            uint16_t want_meta_cap){
    DartConfig tc; DartDiscoveryNetConfig dc; i_DartNodeBlocks nb; i_DartBump b;
    DartTransportState *nt; i_DartNodeCore *ncore; DartDiscovery *ndisc;
    void *new_arena, *old_arena = n->arena;
    uint8_t *nbase; size_t need;
    uint16_t old_max_channels = n->max_channels;
    /* the accept bound never shrinks: channel-derived, previously grown, or requested */
    uint16_t new_meta_cap = dart_meta_capacity(new_max_channels);
    if (n->meta_capacity > new_meta_cap) new_meta_cap = n->meta_capacity;
    if (want_meta_cap    > new_meta_cap) new_meta_cap = want_meta_cap;

    if (!n->alloc_dynamic) return 0;
    if (new_max_peers <= n->max_peers && new_max_channels <= n->max_channels
        && new_meta_cap <= n->meta_capacity) return 0;

    memset(&tc,0,sizeof tc); memset(&dc,0,sizeof dc);
    tc.channels=NULL; tc.n_channels=new_max_channels; tc.max_peers=new_max_peers;
    tc.allocator=i_dart_node_alloc; tc.frag_payload=n->net.fragment_size;
    dc.discovery.max_peers=new_max_peers; dc.discovery.meta_capacity=new_meta_cap;
    dc.discovery.peer_user_bytes = i_dart_node_core_peer_user_bytes();   /* size discovery's scratch to match */
    dc.discovery.alloc = i_dart_node_alloc; dc.discovery.alloc_user = n; /* sizing must match the live core's
                                                                            hook mode (no arena blob pool) */

    memset(&b,0,sizeof b);
    i_dart_node_layout(&b, new_max_peers, new_max_channels, &tc, &dc, &nb);
    need = b.offset + 32u;
    new_arena = dart_allocator_alloc(&n->pool, NULL, need);
    if (!new_arena) return 0;
    nbase = (uint8_t*)(((uintptr_t)new_arena+15u)&~(uintptr_t)15u);
    memset(&b,0,sizeof b); b.base=nbase; b.cap=need-(size_t)(nbase-(uint8_t*)new_arena);
    i_dart_node_layout(&b, new_max_peers, new_max_channels, &tc, &dc, &nb);

    /* migrate the three cores; each leaves the old intact, so a failure just frees the new
       arena and bails (the old node keeps running, only refusing the would-be growth) */
    nt = dart_transport_migrate(n->transport, nb.transport, nb.transport_bytes, new_max_peers, new_max_channels);
    if (!nt){ dart_allocator_alloc(&n->pool, new_arena, 0); return 0; }
    ncore = i_dart_node_core_migrate(n->core, nb.node_core, nb.node_core_bytes, new_max_channels);
    if (!ncore){ dart_allocator_alloc(&n->pool, new_arena, 0); return 0; }
    ncore->transport = nt;                         /* re-point cross-layer pointer */
    i_dart_node_core_build_meta(ncore);              /* rebuild the announce blob into the new buf */
    ndisc = dart_discovery_migrate(n->discovery, nb.discovery, nb.discovery_bytes,
                                      new_max_peers, new_meta_cap, i_dart_node_core_meta(ncore).data, ncore);
    if (!ndisc){ dart_allocator_alloc(&n->pool, new_arena, 0); return 0; }
    i_dart_node_core_bind_discovery(ncore, dart_discovery_state(ndisc));   /* re-point to the relocated table */

    /* handle pointer array (the handle structs themselves are stable, not moved) */
    memcpy(nb.handles, n->handles, (size_t)old_max_channels*sizeof(DartChannel*));
    memset((DartChannel**)nb.handles + old_max_channels, 0,
           (size_t)(new_max_channels-old_max_channels)*sizeof(DartChannel*));

#ifdef DART_SHM
    if (n->shm_capable){
        uint32_t old_segs=(uint32_t)n->shm_n_channels*DART_SHM_N_CLASSES;
        uint32_t new_segs=(uint32_t)new_max_channels*DART_SHM_N_CLASSES, i;
        void **np = (void**)nb.shm_pool;
        for (i=0;i<new_segs;i++) np[i]=NULL;
        for (i=0;i<old_segs;i++) np[i]=n->shm_pool[i];   /* segment states are stable hook
                                                            allocations: only the pointer table moves */
        n->shm_pool=np; n->shm_n_channels=new_max_channels;
        /* the reader attach cache is hook-allocated too: attachments survive the grow */
    }
#endif

    n->transport=nt; n->core=ncore; n->discovery=ndisc;
    n->handles=(DartChannel**)nb.handles;
    n->rx_buf=nb.rx_buf; n->rx_buf_bytes=nb.rx_buf_bytes;
    n->max_channels=new_max_channels; n->max_peers=new_max_peers;
    n->meta_capacity=new_meta_cap;
    dart_allocator_alloc(&n->pool, old_arena, 0);    /* control structs only; heap bufs + segments moved by ref */
    n->arena=new_arena;
    return 1;
}

DartChannel *dart_node_create_channel(DartNode *n, const char *name, DartRole role,
                                      const DartSchema *schema, const DartChannelOpts *opts){
    DartChannelDef def; DartChannel *h; uint16_t idx; int acquired;
    if (!n || !name) return NULL;
    acquired = i_dart_node_lock(n);
    if (!acquired) return NULL;   /* from a callback: a grow here would relocate the arena mid-delivery */
    if (n->n_created >= n->max_channels){       /* reserve full: grow (dynamic) or refuse (static) */
        uint16_t want = n->max_channels < 0x8000u ? (uint16_t)(n->max_channels*2u) : 0xFFFFu;
        if (want <= n->max_channels || !i_dart_node_grow(n, n->max_peers, want, 0)){
            i_dart_node_unlock(n, acquired);
            return NULL;
        }
    }
    idx = n->n_created;
    h = (DartChannel*)i_dart_node_alloc(n, NULL, sizeof *h);   /* stable: outlives any arena grow */
    if (!h){ i_dart_node_unlock(n, acquired); return NULL; }
    h->schema = NULL;
    if (schema){   /* copy into node memory so the caller's schema need not outlive the channel */
        DartBytes w = dart_schema_wire(schema);
        h->schema = dart_schema_parse(w.data, w.len, i_dart_node_alloc, n);
        if (!h->schema){ i_dart_node_alloc(n, h, 0); i_dart_node_unlock(n, acquired); return NULL; }
    }
    memset(&def, 0, sizeof def);
    def.name = name; def.role = (uint8_t)role;
    if (opts) def.qos = opts->qos;
    if (dart_transport_channel_define(n->transport, idx, &def) != 0){
        if (h->schema) dart_schema_free(h->schema, i_dart_node_alloc, n);
        i_dart_node_alloc(n, h, 0);
        i_dart_node_unlock(n, acquired);
        return NULL;
    }
    h->n = n; h->index = idx;
    if (h->schema)   /* advertise + gate matches with it (any non-INACTIVE role) */
        i_dart_node_core_set_channel_schema(n->core, idx, h->schema);
    /* re-advertise our interest so peers match the new channel as the blob arrives, and
       replay known peers' interest so this channel matches what they already advertised */
    i_dart_node_core_build_meta(n->core);
    dart_discovery_advertise(n->discovery, i_dart_node_core_meta(n->core));
    dart_discovery_replay(n->discovery);
    n->handles[idx] = h;
    n->n_created++;
    i_dart_node_kick(n);                       /* announce the new channel now */
    i_dart_node_unlock(n, acquired);
    return h;
}

/* max wall-time draining RX (and running on_message) per poll tick before
 * yielding to discovery, so a slow on_message never starves it */
#ifndef DART_RX_BUDGET_US
#define DART_RX_BUDGET_US 5000u
#endif

/* drain one socket's RX queue into the transport until empty or past deadline
 * (full drain avoids NACK storms). Distinct from public dart_channel_drain. */
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
    if (next){
        uint64_t t0 = i_dart_plat_now_us();
        uint64_t us = (next > t0) ? next - t0 : 0;
        int ms = (us >= (uint64_t)timeout_ms*1000u) ? timeout_ms
                                                    : (int)((us + 999u)/1000u);   /* round up: no busy-spin */
        if (ms < timeout_ms) timeout_ms = ms;
    } else timeout_ms = 0;                 /* something is due right now */
    return timeout_ms;
}

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
        if (want > n->max_peers) i_dart_node_grow(n, want, n->max_channels, 0);
    }
    /* a peer's announce blob exceeded the accept bound last tick: raise the bound (grows
       the RX buffers + discovery capacity with it), then solicit so the peer re-sends its
       blob to buffers that now fit. A failed grow is remembered so the retry loop surfaces
       through the normal event path instead of silently spinning. */
    if (n->meta_grow_need){
        uint16_t want = n->meta_grow_need;
        n->meta_grow_need = 0;
        if (want > n->meta_capacity){
            if (i_dart_node_grow(n, n->max_peers, n->max_channels, want)){
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

    now=i_dart_plat_now_us();
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

/* publish on a channel index: flow-control wait (condvar under a service thread, the
 * nested pump otherwise), then SHM fast path, then UDP. may_wait=0 is a reentrant send
 * (from a callback): it can never block or run the loop, so it commits KEEP_LAST-style
 * and only the unsent guard below applies. */
static int i_dart_node_do_send(DartNode *n, uint16_t channel, DartBytes data, int may_wait){
    size_t len = data.len;
    /* one O(1) count gates the per-send fast paths: a channel no peer subscribes to skips
       backpressure and the SHM eligibility scan here, and the copy+commit in the core. */
    int matched = dart_transport_writer_match_count(n->transport, channel);
    const DartQos *q = dart_transport_channel_qos(n->transport, channel);
    int guarded = 0;   /* the unsent-eviction check runs after the wait */

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
                int evict_unacked = rel_deadline && dart_transport_send_would_evict(n->transport, channel);
                int evict_unsent  = dart_transport_send_would_evict_unsent(n->transport, channel, NULL, NULL);
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
            q = dart_transport_channel_qos(n->transport, channel);
            matched = dart_transport_writer_match_count(n->transport, channel);
        }
    } else
#endif
    /* No service thread: bounded backpressure pump. Run the loop (on_message/on_event
       fire here) until a slow reader acks or qos.backpressure_wait_us elapses, then
       send anyway. */
    if (matched && may_wait && q && q->backpressure_wait_us && dart_transport_send_would_evict(n->transport, channel)){
        uint64_t t0 = i_dart_plat_now_us(), deadline = t0 + q->backpressure_wait_us;
        /* in-pump diagnostic: the publisher is blocked here for the whole wait, so its
           normal per-message print sees nothing within it. When a probe is set, sample
           the writer's repair progress on a ~interval timer so the stall is visible as a
           within-block time series (resends bursty-then-flat vs steady; writer idle for
           lack of NACKs). Observational; when no probe is set this whole block is skipped. */
        uint64_t sample_last = t0; uint32_t sample_polls = 0, sample_idle = 0;
        uint64_t interval = n->pump_probe_interval_us ? n->pump_probe_interval_us : 200000u;
        DartRepairStats sample_prev;
        if (n->pump_probe) dart_transport_repair_stats(n->transport, channel, &sample_prev);
        guarded = 1;
        do {
            i_dart_node_poll_locked(n, 1, 0);   /* nested tick: the lock stays held */
            if (n->pump_probe){
                uint64_t now = i_dart_plat_now_us();
                sample_polls++;
                if (dart_transport_repair_pending(n->transport, channel) == 0) sample_idle++;
                if (now - sample_last >= interval){
                    DartRepairStats sample_now; DartPumpSample sample;
                    dart_transport_repair_stats(n->transport, channel, &sample_now);
                    sample.channel         = channel;
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
            if (!dart_transport_send_would_evict(n->transport, channel)) break;
        } while (i_dart_plat_now_us() < deadline);
        n->backpressure_total_us += i_dart_plat_now_us() - t0;
        n->backpressure_wait_count++;
        /* a mid-pump grow relocates the arena: re-derive the cached pointers */
        q = dart_transport_channel_qos(n->transport, channel);
        matched = dart_transport_writer_match_count(n->transport, channel);
    }

    /* Never-silent: a send that evicts history never handed to the wire (the wait
       above timed out, the socket is wedged, or a reentrant send cannot wait) is
       surfaced. The would-evict state is read here (before the commit changes it),
       but the event fires only if the send actually commits, below: a send the
       transport then rejects (TOO_BIG / ROLE / OOM) overwrote nothing. */
    {
        uint64_t evict_base = 0; uint32_t evict_count = 0;
        int will_evict = guarded &&
            dart_transport_send_would_evict_unsent(n->transport, channel, &evict_base, &evict_count);
        int r;
#ifdef DART_SHM
        if (n->shm_capable && len>0 && channel < n->shm_n_channels && matched && dart_transport_writer_shm_eligible(n->transport, channel)){
            uint16_t keep_last = (q && q->keep_last) ? q->keep_last : 1u;
            /* a hint (shm_max_bytes / max_message_bytes) pins the channel to one class, so
               same-sized traffic reuses a single prefix-sized segment; without it each message
               uses its own size class's segment, created on demand. Per channel either way. */
            uint32_t hint = q ? (q->shm_max_bytes ? q->shm_max_bytes : q->max_message_bytes) : 0u;
            uint32_t k = i_dart_shm_class_for(hint ? hint : (uint32_t)len);
            /* fits its class (with a hint, the hinted class) -> publish via SHM; chunk index =
               the history slot this send will occupy, so chunk i binds slot i (no free list) */
            if (k < DART_SHM_N_CLASSES && (uint32_t)len <= i_dart_shm_class_bytes(k)){
                i_DartShmPool *pool = i_dart_node_shm_channel_pool(n, channel, k, keep_last);
                uint16_t slot = dart_transport_channel_hist_head(n->transport, channel);
                void *chunk_ptr = pool ? i_dart_shm_chunk(pool, slot, NULL) : NULL;
                if (chunk_ptr){
                    i_DartShmDesc d; uint8_t desc[DART_SHM_DESC_WIRE];
                    memcpy(chunk_ptr, data.data, len);                  /* one-copy write into shm */
                    i_dart_shm_stamp(pool, slot, (uint32_t)len, &d);
                    i_dart_shm_desc_encode(&d, desc);
                    if (dart_transport_send_shm(n->transport, channel, dart_bytes(chunk_ptr, len), desc, i_dart_plat_now_us())==0){
                        n->shm_tx++;
                        r = DART_OK;
                        goto committed;
                    }
                }
            }
        }
#endif
        r = dart_transport_send(n->transport, channel, data, i_dart_plat_now_us());
#ifdef DART_SHM
committed:
#endif
        if (r == DART_OK && will_evict){
            DartEvent e; memset(&e, 0, sizeof e);
            n->evicted_unsent++;
            e.kind = DART_ERROR; e.error = DART_E_EVICTED_UNSENT;
            e.channel = channel; e.channel_name = i_dart_node_ch_name(n, channel);
            e.lost_first = evict_base; e.lost_count = evict_count;
            i_dart_node_emit(n, &e);
        }
        return r;
    }
}

int dart_channel_send(DartChannel *ch, DartBytes data){
    int acquired, r;
    if (!ch) return DART_ERR_NO_CHANNEL;
    acquired = i_dart_node_lock(ch->n);
    r = i_dart_node_do_send(ch->n, ch->index, data, acquired);
    i_dart_node_kick(ch->n);              /* flush the commit now, not at the next tick */
    i_dart_node_unlock(ch->n, acquired);
    return r;
}

int dart_channel_set_role(DartChannel *ch, DartRole role){
    int r, acquired;
    if (!ch) return -1;
    acquired = i_dart_node_lock(ch->n);
    if (!acquired){
        /* from a callback: the discovery replay would rematch/reset the very reader
           proxy mid-delivery, so refuse loudly */
        return DART_ERR_STATE;
    }
    r = dart_transport_set_role(ch->n->transport, ch->index, (uint8_t)role);
    if (r == 0){   /* re-advertise our interest: peers rematch as the new blob arrives */
        i_dart_node_core_build_meta(ch->n->core);
        dart_discovery_advertise(ch->n->discovery, i_dart_node_core_meta(ch->n->core));
        dart_discovery_replay(ch->n->discovery);   /* re-apply peers' interest to our new role */
        i_dart_node_kick(ch->n);                   /* announce the change now */
    }
    i_dart_node_unlock(ch->n, acquired);
    return r;
}

uint16_t dart_channel_index(const DartChannel *ch){ return ch ? ch->index : 0; }

const DartSchema *dart_channel_schema(const DartChannel *ch){ return ch ? ch->schema : NULL; }

DartChannel *dart_node_channel(DartNode *n, uint16_t index){
    DartChannel *h; int acquired;
    if (!n) return NULL;
    acquired = i_dart_node_lock(n);
    h = (index < n->n_created) ? n->handles[index] : NULL;
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

void dart_channel_repair_stats(DartChannel *ch, DartRepairStats *out){
    if (ch){
        int acquired = i_dart_node_lock(ch->n);
        dart_transport_repair_stats(ch->n->transport, ch->index, out);
        i_dart_node_unlock(ch->n, acquired);
    } else if (out) memset(out, 0, sizeof *out);
}

void dart_node_set_pump_probe(DartNode *n, DartPumpProbeFn fn, uint64_t interval_us, void *user){
    int acquired = i_dart_node_lock(n);
    n->pump_probe = fn; n->pump_probe_interval_us = interval_us; n->pump_probe_user = user;
    i_dart_node_unlock(n, acquired);
}

int dart_channel_reader_progress(DartChannel *ch, uint32_t peer,
                                 uint64_t *base_seqno, uint32_t *have, uint32_t *total){
    int r, acquired;
    if (!ch) return 0;
    acquired = i_dart_node_lock(ch->n);
    r = dart_transport_reader_progress(ch->n->transport, ch->index, peer, base_seqno, have, total);
    i_dart_node_unlock(ch->n, acquired);
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

int dart_channel_drain(DartChannel *ch, int timeout_ms){
    DartNode *n; uint64_t deadline; int acquired, drained = 1;
    if (!ch) return 0;
    n = ch->n;
    acquired = i_dart_node_lock(n);
    if (!acquired) return 0;   /* from a callback: can neither pump nor wait */
    deadline = i_dart_plat_now_us() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000u;
#ifdef DART_THREADS
    if (n->svc_running){
        n->cv_waiters++;
        while (!dart_transport_send_drained(n->transport, ch->index)){
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
    while (!dart_transport_send_drained(n->transport, ch->index)){
        if (i_dart_plat_now_us() >= deadline){ drained = 0; break; }
        i_dart_node_poll_locked(n, 1, 0);
    }
    i_dart_node_unlock(n, acquired);
    return drained;
}

int dart_channel_match_count(DartChannel *ch){
    int r, acquired;
    if (!ch) return 0;
    acquired = i_dart_node_lock(ch->n);
    r = dart_transport_writer_match_count(ch->n->transport, ch->index);
    i_dart_node_unlock(ch->n, acquired);
    return r;
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
    if (n->discovery) dart_discovery_close(n->discovery, send_bye);   /* frees peer blobs via our
                                                                         hook, so it must run BEFORE
                                                                         the pool is copied out */
    if (n->fd != DART_SOCK_BAD) i_dart_plat_close(n->fd);
#ifdef DART_SHM
    if (n->shm_capable){                              /* unmap OS segments; the pool reset frees only pool memory */
        uint32_t i, n_segments = (uint32_t)n->shm_n_channels * DART_SHM_N_CLASSES;
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
