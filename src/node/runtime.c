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
#include <string.h>    /* heap access goes through dart_plat_realloc (no <stdlib.h> here) */

struct DartChannel { DartNode *n; uint16_t index; };

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
    /* backpressure accumulators, read via dart_node_backpressure_stats */
    uint64_t      backpressure_total_us;
    uint32_t      backpressure_wait_count;
    /* diagnostic: in-pump probe sampled inside the backpressure wait (dart_channel_send) */
    DartPumpProbeFn pump_probe;
    void          *pump_probe_user;
    uint64_t       pump_probe_interval_us;
    /* app callbacks: one message sink (wrapped to build a DartMsg) + everything-else */
    DartMsgFn    user_on_message;
    DartEventFn  on_event;
    void          *user_data;
    void          *arena;      /* the memory block; freed at close iff we own it */
    int            owns_arena;
    /* message-buffer allocation behind dart__node_alloc: static bumps from the arena
       tail [static_pos,static_end); dynamic uses dart_plat_realloc, bounded by mem_cap */
    uint8_t        alloc_dynamic;
    uint8_t        grow_pending;   /* a peer was refused for lack of slots; grow at next poll */
    uint16_t       max_peers;      /* current peer-table capacity (doubles on a dynamic grow) */
    uint8_t       *static_pos, *static_end;
    size_t         mem_cap, mem_used, mem_peak;
    uint64_t       mem_alloc_calls;  /* message-buffer (re)allocations: ~0 in steady state */
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
    void         **shm_pool;      /* [nchan*N_CLASSES] our (channel,class) segments (NULL = not created) */
    uint8_t       *shm_pool_mem;  /* arena: nchan * N_CLASSES * dart_shm_state_bytes */
    uint16_t       shm_n_channels;
    uint64_t      *shm_reader_segments;      /* [reader_max] attached reader-segment ids (0 = empty) */
    uint8_t       *shm_reader_pool_mem; /* [reader_max * dart_shm_state_bytes] */
    uint16_t       shm_reader_max;
    uint32_t       shm_tx, shm_rx;/* messages published / delivered via SHM (observability) */
#endif
};

/* The node's message-buffer allocator, handed to the transport as its realloc hook
 * (u is the node). Every block carries a DART_ALLOC_HDR-byte header storing its
 * payload size so realloc/free know the old size: the static path needs it to copy
 * on grow, the dynamic path to maintain the memory cap. The header keeps the
 * returned pointer 16-aligned. Static: bump from the arena tail, free is a no-op
 * (the whole arena is reclaimed at node close). Dynamic: dart_plat_realloc, refused
 * past mem_cap. */
#define DART_ALLOC_HDR 16u
static void *dart__node_alloc(void *u, void *ptr, size_t size){
    DartNode *n = (DartNode*)u;
    uint8_t *base = ptr ? (uint8_t*)ptr - DART_ALLOC_HDR : NULL;
    size_t   old  = base ? *(size_t*)base : 0;

    if (size == 0){                                   /* free */
        if (base && n->alloc_dynamic){
            n->mem_used -= old + DART_ALLOC_HDR;
            dart_plat_realloc(base, 0);
        }
        return NULL;
    }
    if (n->alloc_dynamic){
        size_t newtot = size + DART_ALLOC_HDR, used = n->mem_used;
        uint8_t *nb;
        if (base) used -= old + DART_ALLOC_HDR;
        if (used + newtot > n->mem_cap) return NULL;   /* runaway guard */
        nb = (uint8_t*)dart_plat_realloc(base, newtot);
        if (!nb) return NULL;
        n->mem_used = used + newtot;
        if (n->mem_used > n->mem_peak) n->mem_peak = n->mem_used;
        n->mem_alloc_calls++;                          /* a real heap (re)alloc -- cold in steady state */
        *(size_t*)nb = size;
        return nb + DART_ALLOC_HDR;
    } else {                                           /* static: bump from the arena tail */
        size_t need = DART_ALLOC_HDR + ((size + 15u) & ~(size_t)15u);
        uint8_t *nb;
        if (base && old >= size) return ptr;           /* fits in place: no re-bump */
        if ((size_t)(n->static_end - n->static_pos) < need) return NULL;
        n->mem_alloc_calls++;
        nb = n->static_pos; n->static_pos += need;
        *(size_t*)nb = size;
        if (base) memcpy(nb + DART_ALLOC_HDR, ptr, old);
        return nb + DART_ALLOC_HDR;
    }
}

/* build a DartMsg and hand it to the app (the channel name is a local lookup, never
 * on the wire). Shared by the inline and SHM delivery paths. */
static void dart__deliver(DartNode *n, uint16_t ch, uint32_t from, DartBytes data){
    DartMsg m;
    if (!n->user_on_message) return;
    memset(&m, 0, sizeof m);
    m.node = n; m.user = n->user_data;
    m.channel_id = ch; m.sender_id = from;
    m.sender_name = dart_node_core_peer_name(n->core, from);          /* view into discovery state */
    if (!m.sender_name.data) m.sender_name = dart_cstr("unknown-peer"); /* .data never NULL on delivery */
    m.channel_name = dart_channel_name(n->transport, ch);
    m.data = data;
    n->user_on_message(&m);
}
static void dart__node_on_message(void *u, uint16_t ch, uint32_t from, DartBytes data){
    dart__deliver((DartNode*)u, ch, from, data);
}
/* node-core events (the app DartEvent: PEER_UP/DOWN/INTEREST/REFUSED) funnel through
 * here; transport events arrive separately via dart__node_on_transport_event. The core
 * sets ev->user to this node; swap it for the app's real user_data before handing on. */
static void dart__node_on_event(const DartEvent *ev){
    DartNode *n = (DartNode*)ev->user;
    /* dynamic mode has no peer cap: a refusal means grow the table (deferred to the next
       poll, out of this callback); the peer re-announces and is admitted, so the app is
       never told it was refused. Static mode keeps the cap and surfaces the event. */
    if (n->alloc_dynamic && ev->kind == DART_PEER_REFUSED){ n->grow_pending = 1; return; }
    if (n->on_event){ DartEvent e = *ev; e.user = n->user_data; n->on_event(&e); }
}

/* transport events arrive as a DartTransportEvent; the node maps them onto its app
 * DartEvent union and hands them on. This is the node combining the two lower layers'
 * events into one app callback (peer events come via the node core, above). */
static void dart__node_on_transport_event(const DartTransportEvent *tev){
    DartNode *n = (DartNode*)tev->user;
    DartEvent e;
    if (!n->on_event) return;
    memset(&e, 0, sizeof e);
    switch (tev->kind){
    case DART_TRANSPORT_MSG_LOST:        e.kind = DART_MSG_LOST; break;
    case DART_TRANSPORT_MSG_TOO_BIG:     e.kind = DART_MSG_TOO_BIG; break;
    case DART_TRANSPORT_NAME_COLLISION:  e.kind = DART_NAME_COLLISION; break;
    case DART_TRANSPORT_QOS_INCOMPATIBLE:e.kind = DART_QOS_INCOMPATIBLE; break;
    default: return;
    }
    e.detail = tev->detail; e.user = n->user_data; e.peer = tev->peer; e.channel = tev->channel;
    e.lost_first = tev->lost_first; e.lost_count = tev->lost_count;
    e.too_big_bytes = tev->too_big_bytes; e.identity = tev->identity;
    n->on_event(&e);
}

#ifdef DART_SHM
/* reader-pool cache capacity: a same-host peer publishes on its channels, one segment
 * each, so size to peers x channels (clamped to the u16 the cache index uses) */
static uint16_t dart__node_shm_reader_max(uint16_t max_peers, uint16_t n_channels){
    uint32_t r = (uint32_t)max_peers * (n_channels ? n_channels : 1u) * DART_SHM_N_CLASSES;
    return (uint16_t)(r > 0xFFFFu ? 0xFFFFu : r);
}
#endif

/* The node arena's sub-blocks, laid out in ONE place so the measure pass (bump.base
 * NULL, read bump.offset) and the build pass (read the pointers) run the same dart_take
 * sequence and can never drift. */
typedef struct {
    uint8_t   *handles, *node_core, *transport, *discovery;
#ifdef DART_SHM
    uint8_t   *shm_pool, *shm_pool_mem, *shm_reader_segments, *shm_reader_pool;
#endif
    size_t     node_core_bytes, transport_bytes, discovery_bytes;
} i_DartNodeBlocks;

static void dart__node_layout(i_DartBump *b, uint16_t max_peers, uint16_t max_channels,
                              const DartConfig *transport_cfg,
                              const DartDiscoveryNetConfig *discovery_rt_cfg, i_DartNodeBlocks *o){
    o->handles       = (uint8_t*)dart_take(b, (size_t)max_channels * sizeof(DartChannel*), 16);
    o->node_core_bytes = dart_node_core_required_memory(max_channels);   /* peers live in discovery now */
    o->node_core = (uint8_t*)dart_take(b, o->node_core_bytes, 16);
    o->transport_bytes = dart_required_memory(transport_cfg);
    o->transport = (uint8_t*)dart_take(b, o->transport_bytes, 16);
#ifdef DART_SHM
    {   size_t state_bytes  = dart_shm_state_bytes();
        uint32_t n_segments = (uint32_t)max_channels * DART_SHM_N_CLASSES;
        uint16_t reader_max = dart__node_shm_reader_max(max_peers, max_channels);
        o->shm_pool            = (uint8_t*)dart_take(b, (size_t)n_segments * sizeof(void*), 16);
        o->shm_pool_mem        = (uint8_t*)dart_take(b, (size_t)n_segments * state_bytes, 16);
        o->shm_reader_segments = (uint8_t*)dart_take(b, (size_t)reader_max * 8u, 16);
        o->shm_reader_pool     = (uint8_t*)dart_take(b, (size_t)reader_max * state_bytes, 16);
    }
#endif
    o->discovery_bytes = dart_discovery_placement_memory(discovery_rt_cfg);
    o->discovery = (uint8_t*)dart_take(b, o->discovery_bytes, 16);
}

/* send one datagram to a peer; returns 1 when done with it, 0 only on a would-block
 * TX-full. The core resolves the abstract destination; we just map it to a UDP address. */
static int dart__node_tx(DartNode *n, uint32_t to, const uint8_t *buf, size_t len){
    i_DartNodeDest d;
    if (!dart_node_core_resolve(n->core, to, &d)) return 1;   /* peer vanished */
    if (dart_plat_send(n->fd, buf, len, d.ip, d.port) < 0 && dart_plat_would_block())
        return 0;
    return 1;
}

#ifdef DART_SHM
/* lazily create our per-channel segment: chunk_bytes = the channel's locked size class,
 * n_chunks = its keep_last, so history slot i binds chunk i (no free list). NULL on fail. */
static i_DartShmPool *dart__shm_chan_pool(DartNode *n, uint16_t ch, uint32_t k, uint16_t keep_last){
    i_DartShmConfig c; uint8_t *mem; uint64_t seg; size_t idx;
    if (k >= DART_SHM_N_CLASSES) return NULL;
    idx = (size_t)ch * DART_SHM_N_CLASSES + k;
    if (n->shm_pool[idx]) return (i_DartShmPool*)n->shm_pool[idx];
    memset(&c, 0, sizeof c);
    seg = n->shm_base | ((uint64_t)ch << 3) | (uint64_t)k;   /* low 3 bits class, next 16 channel */
    c.segment_id = seg;
    dart_shm_seg_name(c.name, seg);
    c.chunk_bytes = dart_shm_class_bytes(k);
    c.n_chunks = keep_last ? keep_last : 1u;
    mem = n->shm_pool_mem + idx * dart_shm_state_bytes();
    n->shm_pool[idx] = dart_shm_create(mem, &c);
    return (i_DartShmPool*)n->shm_pool[idx];
}
/* lazily attach a peer's segment by id (class is in the low bits); cache it. */
static i_DartShmPool *dart__shm_reader_pool(DartNode *n, uint64_t seg){
    uint16_t i, slot = 0xFFFF; i_DartShmConfig c; uint8_t *mem; size_t state_bytes;
    state_bytes = dart_shm_state_bytes();
    for (i=0;i<n->shm_reader_max;i++){
        if (n->shm_reader_segments[i]==seg) return (i_DartShmPool*)(n->shm_reader_pool_mem + (size_t)i*state_bytes);
        if (n->shm_reader_segments[i]==0 && slot==0xFFFF) slot = i;
    }
    if (slot==0xFFFF) return NULL;                         /* cache full */
    memset(&c, 0, sizeof c);
    c.segment_id = seg; dart_shm_seg_name(c.name, seg);     /* attach maps whole + reads geometry */
    mem = n->shm_reader_pool_mem + (size_t)slot*state_bytes;
    if (!dart_shm_attach(mem, &c)) return NULL;
    n->shm_reader_segments[slot] = seg;
    return (i_DartShmPool*)mem;
}
static int dart__node_on_shm(void *u, uint16_t ch, uint32_t from, const uint8_t *desc){
    DartNode *n = (DartNode*)u; i_DartShmDesc d; i_DartShmPool *reader_pool; const void *p; uint32_t len;
    if (!dart_shm_desc_decode(&d, desc, DART_SHM_DESC_WIRE)) return 0;
    reader_pool = dart__shm_reader_pool(n, d.segment_id);
    if (!reader_pool) return 0;                                     /* can't attach -> reader NACKs (repair) */
    p = dart_shm_read(reader_pool, &d, &len);                       /* seqlock head: generation == descriptor? */
    if (!p) return 0;                                      /* recycled -> NACK -> repair or skip */
    /* one-copy: copy out of shared memory so the user owns the bytes (ack-timing safe) */
    if (len > n->shm_scratch_cap){
        void *new_buf = dart__node_alloc(n, n->shm_scratch, len?len:1u);
        if (!new_buf) return 0;
        n->shm_scratch = new_buf; n->shm_scratch_cap = len;
    }
    memcpy(n->shm_scratch, p, len);
    if (!dart_shm_verify(reader_pool, &d)) return 0;                /* seqlock tail: writer recycled mid-copy -> torn -> drop */
    n->shm_rx++;
    dart__deliver(n, ch, from, dart_bytes(n->shm_scratch, len));
    return 1;
}
#endif

DartNode *dart_node_open(DartAllocator *mem, const char *name, DartMsgFn on_message, DartEventFn on_event, const DartNodeOpts *opts){
    DartNodeOpts o; DartDiscoveryNetConfig dc; DartConfig tc; i_DartNodeBlocks blocks;
    uint16_t max_peers, max_channels;
    uint8_t *base; void *arena; int owns; size_t need, arena_size, ctrl_end, ctrl_cap;
    DartNode *n; i_DartSock fd; uint16_t local_port;
    char node_name[DART_NODE_NAME_MAX + 1]; uint8_t node_name_len = 0;   /* name is discovery-level */

    if (!mem || mem->claimed) return NULL;             /* required, and one allocator per node */
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
    dc.discovery.peer_user_bytes = dart_node_core_peer_user_bytes();   /* node-core lifecycle state per peer */
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
    tc.allocator   = dart__node_alloc;   /* non-NULL => reserve/dynamic mode in dart_init */

    {   i_DartBump b; memset(&b,0,sizeof b);
        dart__node_layout(&b, max_peers, max_channels, &tc, &dc, &blocks);
        need = b.offset + 32u; }

    /* The node struct lives OUTSIDE the "layers" arena (node_core+transport+discovery+
       handle-pointer-array), because the user holds DartNode* across a grow that relocates
       the arena. In static mode the struct sits at the buffer front and the layers arena
       follows; in dynamic mode each is its own heap block and the arena can be grown. */
    if (mem->dynamic){
        n = (DartNode*)dart_plat_realloc(NULL, sizeof *n);
        if (!n) return NULL;
        arena_size = mem->size > need ? mem->size : need;   /* hint floors at the layout need */
        arena = dart_plat_realloc(NULL, arena_size); owns = 1;
        if (!arena){ dart_plat_realloc(n, 0); return NULL; }
    } else {
        size_t nsz = (sizeof(struct DartNode) + 15u) & ~(size_t)15u;
        uint8_t *bb = (uint8_t*)(((uintptr_t)mem->buffer + 15u) & ~(uintptr_t)15u);
        size_t head = (size_t)(bb - (uint8_t*)mem->buffer) + nsz;
        if (!mem->buffer || mem->size < head + need) return NULL;
        n = (DartNode*)bb; arena = bb + nsz; arena_size = mem->size - head; owns = 0;
    }
    base = (uint8_t*)(((uintptr_t)arena+15u)&~(uintptr_t)15u);
    {   i_DartBump b; memset(&b,0,sizeof b);
        b.base = base; b.cap = arena_size - (size_t)(base - (uint8_t*)arena);
        dart__node_layout(&b, max_peers, max_channels, &tc, &dc, &blocks);
        ctrl_end = (b.offset + 15u) & ~(size_t)15u;   /* 16-aligned tail: static bump start */
        ctrl_cap = b.cap; }

    if (!dart_plat_startup()){
        if (owns){ dart_plat_realloc(arena, 0); dart_plat_realloc(n, 0); }
        return NULL;
    }

    memset(n, 0, sizeof *n);
    n->fd = DART_SOCK_BAD;
    n->domain = o.domain;
    n->net = o.net;
    n->user_on_message = on_message; n->on_event = on_event;
    n->user_data = o.user_data;
    n->arena = arena; n->owns_arena = owns;
    n->alloc_dynamic = mem->dynamic;
    if (mem->dynamic){
        n->mem_cap = mem->max_bytes ? mem->max_bytes : DART_MEM_DEFAULT_MAX;
        n->static_pos = n->static_end = NULL;
    } else {
        n->static_pos = base + ctrl_end;     /* message buffers bump from the arena tail */
        n->static_end = base + ctrl_cap;
    }
    n->handles = (DartChannel**)blocks.handles;
    memset(n->handles, 0, (size_t)max_channels * sizeof(DartChannel*));
    n->max_channels = max_channels;
    n->max_peers = max_peers;

    tc.on_message = dart__node_on_message;     /* wrap so on_message receives a DartMsg */
    tc.on_event   = dart__node_on_transport_event;  /* map DartTransportEvent -> app DartEvent */
    tc.user       = n;
#ifdef DART_SHM
    n->shm_capable = (uint8_t)(mem->dynamic && !o.disable_shm);   /* static mode never uses SHM */
    if (n->shm_capable){
        dart_plat_host_uuid(n->shm_host);
        if (!dart_plat_random(&n->shm_base, sizeof n->shm_base)) n->shm_base = dart_plat_pid();
        n->shm_base ^= (uint64_t)dart_plat_pid() << 32;    /* fold in pid for cross-process uniqueness */
        n->shm_base &= ~(((uint64_t)1u << 19) - 1u);       /* low 19 bits: 3 class + 16 channel index */
        if (n->shm_base == 0) n->shm_base = (uint64_t)1u << 19;
    }
    tc.on_shm = dart__node_on_shm;
#endif

    n->transport = dart_init(blocks.transport, blocks.transport_bytes, &tc);
    if (!n->transport) goto fail_startup;
#ifdef DART_SHM
    {   uint32_t n_segments = (uint32_t)max_channels * DART_SHM_N_CLASSES;
        uint16_t reader_max = dart__node_shm_reader_max(max_peers, max_channels); uint32_t i;
        n->shm_n_channels = max_channels; n->shm_reader_max = reader_max;
        n->shm_pool            = (void**)blocks.shm_pool;
        n->shm_pool_mem        = blocks.shm_pool_mem;
        n->shm_reader_segments = (uint64_t*)blocks.shm_reader_segments;
        n->shm_reader_pool_mem = blocks.shm_reader_pool;
        for (i=0;i<n_segments;i++) n->shm_pool[i]=NULL;
        for (i=0;i<reader_max;i++) n->shm_reader_segments[i]=0;
    }
#endif

    /* sans-IO node core: drives the discovery->transport lifecycle and resolves addresses
       over the discovery core's peer table (bound below, once discovery exists). */
    node_name_len = dart_discovery_default_name(node_name, sizeof node_name, name);   /* handed to discovery below */
    {   i_DartNodeCoreConfig cc;
        memset(&cc, 0, sizeof cc);
        cc.transport = n->transport;   /* cc.discovery bound after dart_discovery_place */
        cc.n_channels = max_channels; cc.frag_size = dart_clamp_frag(o.net.fragment_size);
        cc.on_event = dart__node_on_event; cc.user = n;
#ifdef DART_SHM
        cc.oob_capable = n->shm_capable; memcpy(cc.oob_host, n->shm_host, 16);
#endif
        n->core = dart_node_core_init(blocks.node_core, blocks.node_core_bytes, &cc);
        if (!n->core) goto fail_startup;
    }

    /* Bind the data socket before opening discovery so we can advertise its real
       port (0 => OS ephemeral, read back via getsockname). No reuse: a unicast
       endpoint owns its port, so a collision fails loudly here. */
    fd = dart_plat_udp_open();
    if (fd==DART_SOCK_BAD) goto fail_startup;
    n->fd=fd;                       /* owned now: fail_sock closes it */
    if (!dart_plat_bind(fd, 0, o.net.data_port, 0)) goto fail_sock;
    local_port = dart_plat_local_port(fd);
    if (local_port==0) goto fail_sock;
    dart_plat_set_nonblock(fd);   /* never block in recv/send; poll drains the queue */
    dart_plat_suppress_connreset(fd);  /* suppress WSAECONNRESET from a bounced send */
    if (o.net.recv_buffer_bytes) dart_plat_set_rcvbuf(fd, (int)o.net.recv_buffer_bytes);
    if (o.net.send_buffer_bytes) dart_plat_set_sndbuf(fd, (int)o.net.send_buffer_bytes);
    dc.discovery.data_port = local_port;       /* advertise the actual port */

    dc.discovery.on_event = dart_node_core_on_disc_event;   /* node core demuxes PEER_UP/DOWN/REFUSED */
    dc.discovery.user     = n->core;
    dc.discovery.name     = dart_string(node_name, node_name_len);   /* discovery-owned (its own blob section) */
    /* the core builds our OVERLAY (frag size + OOB host + interest); discovery wraps it in
       its blob (after the locator + name) so peers reassemble and match from discovery */
    dart_node_core_build_meta(n->core);
    dc.discovery.meta = dart_node_core_meta(n->core);
    n->discovery = dart_discovery_place(blocks.discovery, blocks.discovery_bytes, &dc);
    if (!n->discovery) goto fail_sock;
    /* the node core delegates id<->address resolution + per-peer scratch to discovery's table */
    dart_node_core_bind_discovery(n->core, dart_discovery_state(n->discovery));

    mem->claimed = 1;          /* taken over; the allocator can't back a second node */
    return n;

fail_sock:
    if (n->fd != DART_SOCK_BAD) dart_plat_close(n->fd);
    n->fd = DART_SOCK_BAD;
fail_startup:
    dart_plat_cleanup();
    if (owns){ dart_plat_realloc(arena, 0); dart_plat_realloc(n, 0); }
    return NULL;
}

/* Dynamic-mode growth: relocate the whole node into a bigger arena at the given counts so
 * a full peer table or channel reserve stops being a hard cap. Heap message buffers and SHM
 * writer segments stay put (only their owning control structures move); the user-held
 * DartNode and DartChannel handles live outside the arena, so they survive. Returns 1 with n
 * now on the new arena, or 0 if the bigger arena couldn't be allocated (n left unchanged). */
static int dart__node_grow(DartNode *n, uint16_t new_max_peers, uint16_t new_max_channels){
    DartConfig tc; DartDiscoveryNetConfig dc; i_DartNodeBlocks nb; i_DartBump b;
    DartTransportState *nt; i_DartNodeCore *ncore; DartDiscovery *ndisc;
    void *new_arena, *old_arena = n->arena;
    uint8_t *nbase; size_t need;
    uint16_t old_max_channels = n->max_channels, new_meta_cap = dart_meta_capacity(new_max_channels);

    if (!n->alloc_dynamic) return 0;
    if (new_max_peers <= n->max_peers && new_max_channels <= n->max_channels) return 0;

    memset(&tc,0,sizeof tc); memset(&dc,0,sizeof dc);
    tc.channels=NULL; tc.n_channels=new_max_channels; tc.max_peers=new_max_peers;
    tc.allocator=dart__node_alloc; tc.frag_payload=n->net.fragment_size;
    dc.discovery.max_peers=new_max_peers; dc.discovery.meta_capacity=new_meta_cap;
    dc.discovery.peer_user_bytes = dart_node_core_peer_user_bytes();   /* size discovery's scratch to match */

    memset(&b,0,sizeof b);
    dart__node_layout(&b, new_max_peers, new_max_channels, &tc, &dc, &nb);
    need = b.offset + 32u;
    new_arena = dart_plat_realloc(NULL, need);
    if (!new_arena) return 0;
    nbase = (uint8_t*)(((uintptr_t)new_arena+15u)&~(uintptr_t)15u);
    memset(&b,0,sizeof b); b.base=nbase; b.cap=need-(size_t)(nbase-(uint8_t*)new_arena);
    dart__node_layout(&b, new_max_peers, new_max_channels, &tc, &dc, &nb);

    /* migrate the three cores; each leaves the old intact, so a failure just frees the new
       arena and bails (the old node keeps running, only refusing the would-be growth) */
    nt = dart_migrate(n->transport, nb.transport, nb.transport_bytes, new_max_peers, new_max_channels);
    if (!nt){ dart_plat_realloc(new_arena,0); return 0; }
    ncore = dart_node_core_migrate(n->core, nb.node_core, nb.node_core_bytes, new_max_channels);
    if (!ncore){ dart_plat_realloc(new_arena,0); return 0; }
    ncore->transport = nt;                         /* re-point cross-layer pointer */
    dart_node_core_build_meta(ncore);              /* rebuild the announce blob into the new buf */
    ndisc = dart_discovery_migrate(n->discovery, nb.discovery, nb.discovery_bytes,
                                      new_max_peers, new_meta_cap, dart_node_core_meta(ncore).data, ncore);
    if (!ndisc){ dart_plat_realloc(new_arena,0); return 0; }
    dart_node_core_bind_discovery(ncore, dart_discovery_state(ndisc));   /* re-point to the relocated table */

    /* handle pointer array (the handle structs themselves are stable, not moved) */
    memcpy(nb.handles, n->handles, (size_t)old_max_channels*sizeof(DartChannel*));
    memset((DartChannel**)nb.handles + old_max_channels, 0,
           (size_t)(new_max_channels-old_max_channels)*sizeof(DartChannel*));

#ifdef DART_SHM
    if (n->shm_capable){
        size_t sb = dart_shm_state_bytes();
        uint32_t old_segs=(uint32_t)n->shm_n_channels*DART_SHM_N_CLASSES;
        uint32_t new_segs=(uint32_t)new_max_channels*DART_SHM_N_CLASSES, i;
        uint16_t new_reader_max = dart__node_shm_reader_max(new_max_peers, new_max_channels);
        void **np = (void**)nb.shm_pool;
        for (i=0;i<new_segs;i++) np[i]=NULL;
        for (i=0;i<old_segs;i++)        /* writer pool: keep segments mapped, relocate the state */
            if (n->shm_pool[i]){
                memcpy(nb.shm_pool_mem + (size_t)i*sb, n->shm_pool[i], sb);
                np[i] = nb.shm_pool_mem + (size_t)i*sb;
            }
        for (i=0;i<n->shm_reader_max;i++)  /* reader pool: detach + reset (re-attaches lazily) */
            if (n->shm_reader_segments[i]) dart_shm_detach((i_DartShmPool*)(n->shm_reader_pool_mem+(size_t)i*sb));
        n->shm_pool=np; n->shm_pool_mem=nb.shm_pool_mem;
        n->shm_reader_segments=(uint64_t*)nb.shm_reader_segments;
        n->shm_reader_pool_mem=nb.shm_reader_pool;
        for (i=0;i<new_reader_max;i++) n->shm_reader_segments[i]=0;
        n->shm_reader_max=new_reader_max; n->shm_n_channels=new_max_channels;
    }
#endif

    n->transport=nt; n->core=ncore; n->discovery=ndisc;
    n->handles=(DartChannel**)nb.handles;
    n->max_channels=new_max_channels; n->max_peers=new_max_peers;
    dart_plat_realloc(old_arena,0);    /* control structs only; heap bufs + segments moved by ref */
    n->arena=new_arena;
    return 1;
}

DartChannel *dart_node_create_channel(DartNode *n, const char *name, DartRole role,
                                      const DartChannelOpts *opts){
    DartChannelDef def; DartChannel *h; uint16_t idx;
    if (!n || !name) return NULL;
    if (n->n_created >= n->max_channels){       /* reserve full: grow (dynamic) or refuse (static) */
        uint16_t want = n->max_channels < 0x8000u ? (uint16_t)(n->max_channels*2u) : 0xFFFFu;
        if (want <= n->max_channels || !dart__node_grow(n, n->max_peers, want)) return NULL;
    }
    idx = n->n_created;
    h = (DartChannel*)dart__node_alloc(n, NULL, sizeof *h);   /* stable: outlives any arena grow */
    if (!h) return NULL;
    memset(&def, 0, sizeof def);
    def.name = name; def.role = (uint8_t)role;
    if (opts) def.qos = opts->qos;
    if (dart_channel_define(n->transport, idx, &def) != 0){ dart__node_alloc(n, h, 0); return NULL; }
    h->n = n; h->index = idx;
    /* re-advertise our interest so peers match the new channel as the blob arrives, and
       replay known peers' interest so this channel matches what they already advertised */
    dart_node_core_build_meta(n->core);
    dart_discovery_advertise(n->discovery, dart_node_core_meta(n->core));
    dart_discovery_replay(n->discovery);
    n->handles[idx] = h;
    n->n_created++;
    return h;
}

/* max wall-time draining RX (and running on_message) per poll tick before
 * yielding to discovery, so a slow on_message never starves it */
#ifndef DART_RX_BUDGET_US
#define DART_RX_BUDGET_US 5000u
#endif

/* drain one socket's RX queue into the transport until empty or past deadline
 * (full drain avoids NACK storms). Distinct from public dart_channel_drain. */
static void dart__node_rx_drain(DartNode *n, i_DartSock fd, uint64_t deadline){
    uint8_t buf[DART_DGRAM_MAX];
    for (;;){
        uint8_t src_ip[4]; uint16_t src_port;
        int r = dart_plat_recv(fd, buf, sizeof buf, src_ip, &src_port);
        if (r<0){
            if (dart_plat_would_block()) break;        /* queue empty */
            continue;   /* per-datagram error (e.g. bounced send); keep draining */
        }
        if (r>0){
            if (r>=4 && buf[0]=='u' && buf[1]=='D' && buf[2]=='S' && buf[3]=='C'){
                /* unicast announce aimed at our data port: hand it to discovery */
                dart_discovery_feed(n->discovery, src_ip, 4, dart_bytes(buf, (size_t)r));
            } else {
                uint32_t from;
                if (dart_node_core_id_for_addr(n->core, src_ip, src_port, &from))
                    dart_on_datagram(n->transport, from, dart_bytes(buf, (size_t)r), dart_plat_now_us());
            }
        }
        if (dart_plat_now_us() >= deadline) break;      /* yield to discovery/send */
    }
}

int dart_node_poll(DartNode *n, int timeout_ms){
    uint8_t buf[DART_DGRAM_MAX]; uint32_t to; size_t out_len; uint64_t now;
    i_DartPollfd pfd[1];

    /* a peer was refused last tick for lack of slots: grow the table now, between ticks
       (safe: not inside any layer's processing), then the peer's next announce is admitted */
    if (n->grow_pending){
        uint16_t want = n->max_peers < 0x8000u ? (uint16_t)(n->max_peers*2u) : 0xFFFFu;
        n->grow_pending = 0;
        if (want > n->max_peers) dart__node_grow(n, want, n->max_channels);
    }

    dart_discovery_poll(n->discovery, 0);                 /* discovery tick (non-blocking) */

    memset(pfd,0,sizeof pfd);
    pfd[0].fd=n->fd; pfd[0].events=DART_POLLIN;
    /* cap the wait at the next internal timer so a due ack/NACK/heartbeat fires on
       time, not after the full quantum (no traffic to wake us when a writer stalls) */
    { uint64_t next_deadline = dart_next_deadline_us(n->transport);
      if (next_deadline){ uint64_t t0 = dart_plat_now_us();
               uint64_t us = (next_deadline > t0) ? next_deadline - t0 : 0;          /* until the timer */
               int ms = (us >= (uint64_t)timeout_ms*1000u) ? timeout_ms
                                                           : (int)((us + 999u)/1000u);
               if (ms < timeout_ms) timeout_ms = ms; } }       /* round up: no busy-spin */
    if (dart_plat_poll(pfd,1,timeout_ms) > 0){
        uint64_t rx_deadline = dart_plat_now_us() + DART_RX_BUDGET_US;
        if (pfd[0].revents & DART_POLLIN) dart__node_rx_drain(n, n->fd, rx_deadline);
    }

    now=dart_plat_now_us();
    /* the core already consumed any held datagram, so retry it before pulling new */
    if (n->tx_hold_len && dart__node_tx(n, n->tx_hold_peer, n->tx_hold, n->tx_hold_len))
        n->tx_hold_len = 0;
    if (!n->tx_hold_len)
        while (dart_poll_send(n->transport,&to,buf,sizeof buf,&out_len,now)){
            if (!dart__node_tx(n, to, buf, out_len)){
                memcpy(n->tx_hold, buf, out_len);
                n->tx_hold_len = out_len; n->tx_hold_peer = to;
                break;          /* TX buffer full: yield this tick */
            }
            now=dart_plat_now_us();
        }
    return 0;
}

/* publish on a channel index: bounded backpressure pump, then SHM fast path, then UDP */
static int dart__node_do_send(DartNode *n, uint16_t channel, DartBytes data){
    size_t len = data.len;
    /* bounded backpressure: pump the loop (on_message/on_event may fire here) until
       a slow reader acks or qos.backpressure_wait_us elapses, then send anyway */
    const DartQos *q = dart_channel_qos(n->transport, channel);
    if (q && q->backpressure_wait_us && dart_send_would_evict(n->transport, channel)){
        uint64_t t0 = dart_plat_now_us(), deadline = t0 + q->backpressure_wait_us;
        /* in-pump diagnostic: the publisher is blocked here for the whole wait, so its
           normal per-message print sees nothing within it. When a probe is set, sample
           the writer's repair progress on a ~interval timer so the stall is visible as a
           within-block time series (resends bursty-then-flat vs steady; writer idle for
           lack of NACKs). Observational; when no probe is set this whole block is skipped. */
        uint64_t sample_last = t0; uint32_t sample_polls = 0, sample_idle = 0;
        uint64_t interval = n->pump_probe_interval_us ? n->pump_probe_interval_us : 200000u;
        DartRepairStats sample_prev;
        if (n->pump_probe) dart_repair_stats(n->transport, channel, &sample_prev);
        do {
            dart_node_poll(n, 1);
            if (n->pump_probe){
                uint64_t now = dart_plat_now_us();
                sample_polls++;
                if (dart_repair_pending(n->transport, channel) == 0) sample_idle++;
                if (now - sample_last >= interval){
                    DartRepairStats sample_now; DartPumpSample sample;
                    dart_repair_stats(n->transport, channel, &sample_now);
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
            if (!dart_send_would_evict(n->transport, channel)) break;
        } while (dart_plat_now_us() < deadline);
        n->backpressure_total_us += dart_plat_now_us() - t0;
        n->backpressure_wait_count++;
    }
#ifdef DART_SHM
    if (n->shm_capable && len>0 && channel < n->shm_n_channels && dart_writer_shm_eligible(n->transport, channel)){
        uint16_t keep_last = (q && q->keep_last) ? q->keep_last : 1u;
        /* a hint (shm_max_bytes / max_message_bytes) pins the channel to one class, so
           same-sized traffic reuses a single prefix-sized segment; without it each message
           uses its own size class's segment, created on demand. Per channel either way. */
        uint32_t hint = q ? (q->shm_max_bytes ? q->shm_max_bytes : q->max_message_bytes) : 0u;
        uint32_t k = dart_shm_class_for(hint ? hint : (uint32_t)len);
        /* fits its class (with a hint, the hinted class) -> publish via SHM; chunk index =
           the history slot this send will occupy, so chunk i binds slot i (no free list) */
        if (k < DART_SHM_N_CLASSES && (uint32_t)len <= dart_shm_class_bytes(k)){
            i_DartShmPool *pool = dart__shm_chan_pool(n, channel, k, keep_last);
            uint16_t slot = dart_channel_hist_head(n->transport, channel);
            void *chunk_ptr = pool ? dart_shm_chunk(pool, slot, NULL) : NULL;
            if (chunk_ptr){
                i_DartShmDesc d; uint8_t desc[DART_SHM_DESC_WIRE];
                memcpy(chunk_ptr, data.data, len);                  /* one-copy write into shm */
                dart_shm_stamp(pool, slot, (uint32_t)len, &d);
                dart_shm_desc_encode(&d, desc);
                if (dart_send_shm(n->transport, channel, dart_bytes(chunk_ptr, len), desc, dart_plat_now_us())==0){
                    n->shm_tx++;
                    return 0;
                }
            }
        }
    }
#endif
    return dart_send(n->transport, channel, data, dart_plat_now_us());
}

int dart_channel_send(DartChannel *ch, DartBytes data){
    if (!ch) return DART_ERR_NO_CHANNEL;
    return dart__node_do_send(ch->n, ch->index, data);
}

int dart_channel_set_role(DartChannel *ch, DartRole role){
    int r;
    if (!ch) return -1;
    r = dart_set_role(ch->n->transport, ch->index, (uint8_t)role);
    if (r == 0){   /* re-advertise our interest: peers rematch as the new blob arrives */
        dart_node_core_build_meta(ch->n->core);
        dart_discovery_advertise(ch->n->discovery, dart_node_core_meta(ch->n->core));
        dart_discovery_replay(ch->n->discovery);   /* re-apply peers' interest to our new role */
    }
    return r;
}

uint16_t dart_channel_index(const DartChannel *ch){ return ch ? ch->index : 0; }

DartChannel *dart_node_channel(DartNode *n, uint16_t index){
    if (!n || index >= n->n_created) return NULL;
    return n->handles[index];
}

/* Read-only peer view: discovery already packs its peer table into a zero-copy array, and a
 * node peer IS a discovery peer (it adds only the decoded overlay, read on demand via
 * dart_node_peer_frag / dart_node_peer_interest_next). So this just forwards. */
const DartDiscoveryPeer *dart_node_peers(DartNode *n, uint16_t *count){
    return dart_discovery_peers(n ? n->discovery : NULL, count);
}

void dart_node_backpressure_stats(DartNode *n, uint64_t *waited_us, uint32_t *waited_sends){
    if (waited_us)    *waited_us    = n->backpressure_total_us;
    if (waited_sends) *waited_sends = n->backpressure_wait_count;
}

void dart_node_mem_stats(DartNode *n, size_t *in_use, size_t *peak, uint64_t *alloc_calls){
    if (!n) return;
    if (in_use)      *in_use      = n->mem_used;        /* live message-buffer bytes (dynamic) */
    if (peak)        *peak        = n->mem_peak;        /* high-water of the above */
    if (alloc_calls) *alloc_calls = n->mem_alloc_calls; /* (re)allocations so far; flat in steady state */
}

void dart_channel_repair_stats(DartChannel *ch, DartRepairStats *out){
    if (ch) dart_repair_stats(ch->n->transport, ch->index, out);
    else if (out) memset(out, 0, sizeof *out);
}

void dart_node_set_pump_probe(DartNode *n, DartPumpProbeFn fn, uint64_t interval_us, void *user){
    n->pump_probe = fn; n->pump_probe_interval_us = interval_us; n->pump_probe_user = user;
}

int dart_channel_reader_progress(DartChannel *ch, uint32_t peer,
                                 uint64_t *base_seqno, uint32_t *have, uint32_t *total){
    return ch ? dart_reader_progress(ch->n->transport, ch->index, peer, base_seqno, have, total) : 0;
}

#ifdef DART_SHM
void dart_node_shm_stats(DartNode *n, uint32_t *sent, uint32_t *recv){
    if (sent) *sent = n->shm_tx;
    if (recv) *recv = n->shm_rx;
}
#endif

int dart_channel_drain(DartChannel *ch, int timeout_ms){
    DartNode *n; uint64_t deadline;
    if (!ch) return 0;
    n = ch->n;
    deadline = dart_plat_now_us() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000u;
    while (!dart_send_drained(n->transport, ch->index)){
        if (dart_plat_now_us() >= deadline) return 0;
        dart_node_poll(n, 1);
    }
    return 1;
}

int dart_channel_match_count(DartChannel *ch){
    return ch ? dart_writer_match_count(ch->n->transport, ch->index) : 0;
}

void dart_node_close(DartNode *n, int send_bye){
    void *arena; int owns;
    if (!n) return;
    arena = n->arena; owns = n->owns_arena;
    if (n->discovery) dart_discovery_close(n->discovery, send_bye);
    if (n->fd != DART_SOCK_BAD) dart_plat_close(n->fd);
    if (n->transport) dart_destroy(n->transport);     /* free hook-allocated dynamic buffers + rings */
#ifdef DART_SHM
    if (n->shm_capable){
        size_t state_bytes = dart_shm_state_bytes(); uint32_t i, n_segments = (uint32_t)n->shm_n_channels * DART_SHM_N_CLASSES;
        for (i=0;i<n_segments;i++)
            if (n->shm_pool[i]) dart_shm_detach((i_DartShmPool*)n->shm_pool[i]);   /* unlinks ours */
        for (i=0;i<n->shm_reader_max;i++)
            if (n->shm_reader_segments[i]) dart_shm_detach((i_DartShmPool*)(n->shm_reader_pool_mem + (size_t)i*state_bytes));
        if (n->shm_scratch) dart__node_alloc(n, n->shm_scratch, 0);
    }
#endif
    {   uint16_t i; for (i=0;i<n->n_created;i++) if (n->handles[i]) dart__node_alloc(n, n->handles[i], 0); }
    dart_plat_cleanup();
    if (owns){ dart_plat_realloc(arena, 0); dart_plat_realloc(n, 0); }   /* touch nothing after */
}
