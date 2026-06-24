/* NODE runtime: owns the data socket, drives discovery, wires peers into the
 * transport. All OS access goes through dart_plat. See dart_node.h. */

#include "dart_node.h"
#include "dart_discovery_rt.h"
#include "dart_plat.h"
#ifdef DART_SHM
#include "dart_shm.h"
#endif
#include <string.h>

typedef struct {
    uint8_t  used;
    uint8_t  dormant;  /* discovery DROPPED it: kept for a same-incarnation resume */
    uint32_t id;
    uint8_t  ip[16];
    uint8_t  ip_len;
    uint16_t port;     /* peer's advertised data port */
} dart_node_peer;

struct dart_node {
    dart_state     *transport;
    dart_discovery_rt     *discovery;
    dart_sock       fd;       /* unicast data socket (also group TX) */
    dart_sock       multicast_fd;     /* multicast data RX socket (DART_SOCK_BAD if unused) */
    dart_node_peer *peers;
    uint16_t      max_peers;
    uint16_t      domain;
    uint16_t      multicast_port;
    /* datagram the socket refused; retried first next poll so it is never lost */
    uint8_t       tx_hold[DART_DGRAM_MAX];
    size_t        tx_hold_len;
    uint32_t      tx_hold_peer;
    /* backpressure accumulators, read via dart_node_backpressure_stats */
    uint64_t      backpressure_total_us;
    uint32_t      backpressure_wait_count;
    /* diagnostic: in-pump probe sampled inside the backpressure wait (dart_node_send) */
    dart_pump_probe_fn pump_probe;
    void          *pump_probe_user;
    uint64_t       pump_probe_interval_us;
    /* one app callback for everything but message delivery; node fills PEER_UP/DOWN */
    dart_event_fn  on_event;
    void          *user_data;
    /* opaque blob carried in every discovery announce; must outlive the node. Holds
       this node's UDP fragment size + pub/sub interest list (see dart__meta_*). */
    uint8_t       *discovery_meta;     /* arena, discovery_meta_cap bytes */
    uint16_t       discovery_meta_cap;
    uint16_t       discovery_meta_len;
    uint16_t       frag_size;     /* our clamped UDP fragment size, baked into the blob */
#ifdef DART_SHM
    /* zero-fragment same-host path: lazy per-size-class segments (see dart_shm.h) */
    uint8_t        shm_capable;       /* 1 = an allocator is set, so SHM is usable */
    uint8_t        shm_host[16];  /* our host uuid (advertised; same-host check) */
    uint64_t       shm_base;      /* per-node segment id base; low 19 bits = (channel<<3)|class */
    dart_message_fn shm_user_on_message;  /* the app's real callback (we wrap it) */
    dart_alloc_fn  shm_alloc;     /* one-copy receive scratch (== cfg.allocator) */
    void          *shm_scratch; uint32_t shm_scratch_cap;
    void         **shm_pool;      /* [nchan*N_CLASSES] our (channel,class) segments (NULL = not created) */
    uint8_t       *shm_pool_mem;  /* arena: nchan * N_CLASSES * dart_shm_state_bytes */
    uint16_t       shm_n_channels;
    uint64_t      *shm_reader_segments;      /* [reader_max] attached reader-segment ids (0 = empty) */
    uint8_t       *shm_reader_pool_mem; /* [reader_max * dart_shm_state_bytes] */
    uint16_t       shm_reader_max;
    uint32_t       shm_tx, shm_rx;/* messages published / delivered via SHM (observability) */
#endif
};

/* Discovery-announce metadata: a versioned blob carrying this node's fragment size,
 * (v3) its SHM capability + host uuid, then its interest list. Layout:
 *   v2: ['D','N',2, frag_lo, frag_hi,                      <interest>]   prefix 5
 *   v3: ['D','N',3, frag_lo, frag_hi, shm, host[16],       <interest>]   prefix 22
 * Decode is version-aware so v2 (non-SHM) and v3 nodes interop; frag is at [3..4] in
 * both. We write v3 when DART_SHM is compiled, v2 otherwise. */
#define DART__META_PREFIX_V2 5u
#define DART__META_PREFIX_V3 22u
#ifdef DART_SHM
#define DART__META_PREFIX DART__META_PREFIX_V3   /* what WE write */
#else
#define DART__META_PREFIX DART__META_PREFIX_V2
#endif

static int dart__meta_ok(const uint8_t *meta, uint16_t meta_len){
    return meta && meta_len >= 5 && meta[0]=='D' && meta[1]=='N' && (meta[2]==2 || meta[2]==3);
}
static uint16_t dart__meta_pfx(const uint8_t *meta){
    return meta[2]==3 ? DART__META_PREFIX_V3 : DART__META_PREFIX_V2;
}
static uint16_t dart__meta_frag(const uint8_t *meta, uint16_t meta_len){
    if (!dart__meta_ok(meta, meta_len)) return 0;
    return (uint16_t)(meta[3] | ((uint16_t)meta[4] << 8));
}
/* Locate the interest sub-blob within a peer's meta; NULL/0 if absent. */
static const uint8_t *dart__meta_interest(const uint8_t *meta, uint16_t meta_len, size_t *out_len){
    uint16_t prefix;
    if (!dart__meta_ok(meta, meta_len) || meta_len < (prefix = dart__meta_pfx(meta))){ *out_len = 0; return NULL; }
    *out_len = (size_t)(meta_len - prefix);
    return meta + prefix;
}
#ifdef DART_SHM
/* A peer's SHM capability + host uuid (v3 only); 1 if SHM-capable, fills host[16]. */
static int dart__meta_shm(const uint8_t *meta, uint16_t meta_len, uint8_t host[16]){
    if (!dart__meta_ok(meta, meta_len) || meta[2]!=3 || meta_len < DART__META_PREFIX_V3 || !meta[5]) return 0;
    memcpy(host, meta+6, 16);
    return 1;
}
#endif
/* (Re)build our blob: prefix (frag [+ shm/host]) + current interest list. */
static uint16_t dart__meta_build(dart_node *n){
    uint8_t *out = n->discovery_meta; size_t interest_len; uint16_t prefix = DART__META_PREFIX;
    out[0]='D'; out[1]='N'; out[2]=(uint8_t)(DART__META_PREFIX==DART__META_PREFIX_V3 ? 3 : 2);
    out[3]=(uint8_t)(n->frag_size & 0xFF); out[4]=(uint8_t)(n->frag_size >> 8);
#ifdef DART_SHM
    out[5]=(uint8_t)(n->shm_capable?1:0); memcpy(out+6, n->shm_host, 16);
#endif
    interest_len = dart_build_interest(n->transport, out + prefix, n->discovery_meta_cap - prefix);
    return (uint16_t)(prefix + interest_len);
}

/* announce blob capacity: frag prefix + the largest interest list our channels can
 * produce, capped to fit one (IP-fragmentable) UDP datagram */
static uint16_t dart__node_meta_capacity(const dart_node_config *cfg){
    size_t meta_capacity = DART__META_PREFIX + dart_interest_max(cfg->n_channels);
    if (meta_capacity > 65000u) meta_capacity = 65000u;
    return (uint16_t)meta_capacity;
}

#ifdef DART_SHM
/* reader-pool cache capacity: a same-host peer publishes on its channels, one segment
 * each, so size to peers x channels (clamped to the u16 the cache index uses) */
static uint16_t dart__node_shm_reader_max(uint16_t max_peers, uint16_t n_channels){
    uint32_t r = (uint32_t)max_peers * (n_channels ? n_channels : 1u) * DART_SHM_N_CLASSES;
    return (uint16_t)(r > 0xFFFFu ? 0xFFFFu : r);
}
/* arena bytes for the SHM bookkeeping (per-channel pool ptrs + handles + locked class,
 * plus the reader caches; the segments themselves are OS-mapped, lazily, outside it) */
static size_t dart__node_shm_bytes(const dart_node_config *cfg, uint16_t max_peers){
    size_t state_bytes = dart_shm_state_bytes();
    uint32_t n_segments = (uint32_t)cfg->n_channels * DART_SHM_N_CLASSES;   /* (channel,class) segments */
    uint16_t reader_max = dart__node_shm_reader_max(max_peers, cfg->n_channels);
    size_t s = 0;
    s += ((size_t)n_segments*sizeof(void*) + 15u)&~(size_t)15u;      /* (channel,class) pool ptr array */
    s += ((size_t)n_segments*state_bytes + 15u)&~(size_t)15u;                 /* (channel,class) pool handles */
    s += ((size_t)reader_max*8u + 15u)&~(size_t)15u;               /* reader segment ids */
    s += ((size_t)reader_max*state_bytes + 15u)&~(size_t)15u;               /* reader pool handles */
    return s;
}
#endif

/* split node config into discovery + transport sub-configs */
static void dart__node_cfgs(const dart_node_config *cfg, dart_discovery_rt_config *discovery_rt_cfg,
                          dart_config *transport_cfg, uint16_t *max_peers_out){
    uint16_t max_peers = cfg->discovery.max_peers ? cfg->discovery.max_peers : 16;
    memset(discovery_rt_cfg, 0, sizeof *discovery_rt_cfg); memset(transport_cfg, 0, sizeof *transport_cfg);
    discovery_rt_cfg->discovery.domain_id   = cfg->domain;
    discovery_rt_cfg->discovery.data_port   = cfg->net.data_port;
    discovery_rt_cfg->discovery.announce_interval_us = cfg->discovery.announce_interval_us; /* 0 uses default */
    discovery_rt_cfg->discovery.peer_timeout_us  = cfg->discovery.peer_timeout_us;
    discovery_rt_cfg->discovery.max_peers   = max_peers;
    discovery_rt_cfg->discovery.meta_capacity    = dart__node_meta_capacity(cfg);
    discovery_rt_cfg->group            = cfg->net.discovery_group;
    discovery_rt_cfg->discovery_port        = cfg->net.discovery_port;
    discovery_rt_cfg->ttl              = cfg->net.multicast_ttl;
    discovery_rt_cfg->multicast_interface         = cfg->net.multicast_interface;
    discovery_rt_cfg->seeds            = cfg->net.seed_peers;
    discovery_rt_cfg->n_seeds          = cfg->net.n_seed_peers;
    transport_cfg->channels   = cfg->channels;
    transport_cfg->n_channels = cfg->n_channels;
    transport_cfg->max_peers  = max_peers;
    transport_cfg->frag_payload = cfg->net.fragment_size;   /* 0 = default; dart_init clamps to [MIN,MAX] */
    transport_cfg->on_message = cfg->on_message;
    transport_cfg->on_event   = cfg->on_event;
    transport_cfg->allocator  = cfg->allocator;
    transport_cfg->user       = cfg->user_data;
    if (max_peers_out) *max_peers_out = max_peers;
}

/* local iff a route probe to the address selects that same address as source */
static int dart__node_is_local_ip(const uint8_t ip[4]){
    uint32_t d = dart_plat_ip4_to_naddr(ip);
    return dart_plat_route_src(d, 7) == d;
}

static int dart__node_find_id(dart_node *n, uint32_t id);   /* peer table lookups, defined below */

#ifdef DART_SHM
/* a peer can receive our SHM-DATA iff we are SHM-capable, it advertised SHM, and its
 * host uuid equals ours (same kernel). Set the transport's per-peer flag. */
static void dart__node_set_peer_shm(dart_node *n, uint32_t id, const uint8_t *meta, uint16_t meta_len){
    uint8_t host[16];
    int shm = n->shm_capable && dart__meta_shm(meta, meta_len, host) &&
              dart_shm_host_match(host, n->shm_host);
    dart_peer_set_shm(n->transport, id, shm);
}
#endif

static void dart__node_up(void *u, uint32_t id, const dart_discovery_addr *addr,
                        const uint8_t *meta, uint16_t meta_len){
    dart_node *n = (dart_node*)u; uint16_t i; int slot = -1;
    uint16_t frag = dart__meta_frag(meta, meta_len);
    size_t interest_len = 0; const uint8_t *interest = dart__meta_interest(meta, meta_len, &interest_len);
    for (i=0;i<n->max_peers;i++){
        if (n->peers[i].used && n->peers[i].id==id){      /* known peer: addr/interest update */
            memcpy(n->peers[i].ip, addr->ip, 16);
            n->peers[i].ip_len = addr->ip_len; n->peers[i].port = addr->port;
            dart_peer_set_frag(n->transport, id, frag);
#ifdef DART_SHM
            dart__node_set_peer_shm(n, id, meta, meta_len);
#endif
            if (interest) dart_apply_peer_interest(n->transport, id, interest, interest_len);
            if (n->peers[i].dormant){    /* a DROPPED peer's same incarnation returned: resume */
                n->peers[i].dormant = 0;
                dart_peer_resume(n->transport, id);   /* keeps reader position; writer fills any gap */
                if (n->on_event){
                    dart_event ev; memset(&ev, 0, sizeof ev);
                    ev.kind=DART_PEER_UP; ev.peer=id; ev.detail="peer resumed";
                    memcpy(ev.ip, addr->ip, 16); ev.ip_len=addr->ip_len; ev.port=addr->port;
                    n->on_event(n->user_data, &ev);
                }
            }
            return;
        }
        if (!n->peers[i].used && slot<0) slot=(int)i;
    }
    if (slot<0) return;
    n->peers[slot].used=1; n->peers[slot].id=id;
    memcpy(n->peers[slot].ip, addr->ip, 16);
    n->peers[slot].ip_len=addr->ip_len; n->peers[slot].port=addr->port;
    /* the announce blob carries the peer's frag size + pub/sub interest list */
    dart_peer_add(n->transport, id, (addr->ip_len==4) && dart__node_is_local_ip(addr->ip), frag);
#ifdef DART_SHM
    dart__node_set_peer_shm(n, id, meta, meta_len);
#endif
    if (interest) dart_apply_peer_interest(n->transport, id, interest, interest_len);
    if (n->on_event){
        dart_event ev; memset(&ev, 0, sizeof ev);
        ev.kind=DART_PEER_UP; ev.peer=id; ev.detail="peer discovered";
        memcpy(ev.ip, addr->ip, 16); ev.ip_len=addr->ip_len; ev.port=addr->port;
        n->on_event(n->user_data, &ev);
    }
}
static void dart__node_down(void *u, uint32_t id, dart_discovery_down_reason reason){
    dart_node *n=(dart_node*)u; int i = dart__node_find_id(n, id);
    if (reason == DART_DISCOVERY_DROP){
        /* fell silent: keep transport state so a same-incarnation return resumes
           losslessly; just stop flow-controlling it and tell the app once */
        if (i>=0 && !n->peers[i].dormant){
            n->peers[i].dormant = 1;
            dart_peer_dormant(n->transport, id);
            if (n->on_event){
                dart_event ev; memset(&ev, 0, sizeof ev);
                ev.kind=DART_PEER_DOWN; ev.peer=id; ev.detail="peer dropped";
                n->on_event(n->user_data, &ev);
            }
        }
    } else {   /* GONE: said BYE or its slot was reclaimed; free the transport state */
        int notify = (i>=0 && !n->peers[i].dormant);   /* active->gone: app not yet told */
        if (i>=0) n->peers[i].used=0;
        dart_peer_remove(n->transport, id);
        if (notify && n->on_event){
            dart_event ev; memset(&ev, 0, sizeof ev);
            ev.kind=DART_PEER_DOWN; ev.peer=id; ev.detail="peer lost";
            n->on_event(n->user_data, &ev);
        }
    }
}
static void dart__node_refused(void *u, const dart_discovery_addr *addr){
    dart_node *n=(dart_node*)u;
    if (n->on_event){
        dart_event ev; memset(&ev, 0, sizeof ev);
        ev.kind=DART_PEER_REFUSED; ev.detail="peer table full (all active)";
        memcpy(ev.ip, addr->ip, 16); ev.ip_len=addr->ip_len; ev.port=addr->port;
        n->on_event(n->user_data, &ev);
    }
}

static int dart__node_find_addr(dart_node *n, const uint8_t ip[4], uint16_t port){
    uint16_t i;
    for (i=0;i<n->max_peers;i++)
        if (n->peers[i].used && n->peers[i].ip_len>=4 && n->peers[i].port==port
            && memcmp(n->peers[i].ip, ip, 4)==0) return (int)i;
    return -1;
}
static int dart__node_find_id(dart_node *n, uint32_t id){
    uint16_t i;
    for (i=0;i<n->max_peers;i++) if (n->peers[i].used && n->peers[i].id==id) return (int)i;
    return -1;
}

/* data multicast group from a selector (topic identity & 0xFF); &0xFF wrap is
 * harmless, RX filters by peer table + identity */
static uint32_t dart__node_group_addr(uint16_t domain, uint16_t sel){
    return dart_plat_ipv4(239u, 255u, (uint8_t)(domain & 0xFFu), (uint8_t)(sel & 0xFFu));
}
/* a channel's group, from its identity so it matches DART_DEST_GROUP and every peer */
static uint32_t dart__node_chan_group(uint16_t domain, const dart_channel_def *def){
    return dart__node_group_addr(domain, (uint16_t)(dart_channel_identity(def) & 0xFFu));
}

/* send one datagram; returns 1 when done with it, 0 only on a would-block TX-full */
static int dart__node_tx(dart_node *n, uint32_t to, const uint8_t *buf, size_t len){
    uint8_t ip[4]; uint16_t port;
    if (DART_DEST_IS_GROUP(to)){
        dart_plat_naddr_to_ip4(dart__node_group_addr(n->domain, DART_DEST_GROUP_CHAN(to)), ip);
        port = n->multicast_port;
    } else {
        int peer_idx = dart__node_find_id(n, to);
        if (peer_idx < 0) return 1;              /* peer vanished */
        memcpy(ip, n->peers[peer_idx].ip, 4);
        port = n->peers[peer_idx].port;
    }
    if (dart_plat_send(n->fd, buf, len, ip, port) < 0 && dart_plat_would_block())
        return 0;
    return 1;
}

#ifdef DART_SHM
/* OS object name "/dart.shm.<16 hex>" -- valid on POSIX (leading /) and Windows. */
static void dart__shm_name(char *buf, uint64_t seg){
    static const char hex_digits[] = "0123456789abcdef";
    const char prefix[] = "/dart.shm."; int i, k = 0;
    while (prefix[k]){ buf[k] = prefix[k]; k++; }
    for (i=15;i>=0;i--) buf[k++] = hex_digits[(seg >> (4*i)) & 0xF];
    buf[k] = 0;
}
/* lazily create our per-channel segment: chunk_bytes = the channel's locked size class,
 * n_chunks = its keep_last, so history slot i binds chunk i (no free list). NULL on fail. */
static dart_shm_pool *dart__shm_chan_pool(dart_node *n, uint16_t ch, uint32_t k, uint16_t keep_last){
    dart_shm_config c; uint8_t *mem; uint64_t seg; size_t idx;
    if (k >= DART_SHM_N_CLASSES) return NULL;
    idx = (size_t)ch * DART_SHM_N_CLASSES + k;
    if (n->shm_pool[idx]) return (dart_shm_pool*)n->shm_pool[idx];
    memset(&c, 0, sizeof c);
    seg = n->shm_base | ((uint64_t)ch << 3) | (uint64_t)k;   /* low 3 bits class, next 16 channel */
    c.segment_id = seg;
    dart__shm_name(c.name, seg);
    c.chunk_bytes = dart_shm_class_bytes(k);
    c.n_chunks = keep_last ? keep_last : 1u;
    mem = n->shm_pool_mem + idx * dart_shm_state_bytes();
    n->shm_pool[idx] = dart_shm_create(mem, &c);
    return (dart_shm_pool*)n->shm_pool[idx];
}
/* lazily attach a peer's segment by id (class is in the low bits); cache it. */
static dart_shm_pool *dart__shm_reader_pool(dart_node *n, uint64_t seg){
    uint16_t i, slot = 0xFFFF; dart_shm_config c; uint8_t *mem; size_t state_bytes;
    state_bytes = dart_shm_state_bytes();
    for (i=0;i<n->shm_reader_max;i++){
        if (n->shm_reader_segments[i]==seg) return (dart_shm_pool*)(n->shm_reader_pool_mem + (size_t)i*state_bytes);
        if (n->shm_reader_segments[i]==0 && slot==0xFFFF) slot = i;
    }
    if (slot==0xFFFF) return NULL;                         /* cache full */
    memset(&c, 0, sizeof c);
    c.segment_id = seg; dart__shm_name(c.name, seg);        /* attach maps whole + reads geometry */
    mem = n->shm_reader_pool_mem + (size_t)slot*state_bytes;
    if (!dart_shm_attach(mem, &c)) return NULL;
    n->shm_reader_segments[slot] = seg;
    return (dart_shm_pool*)mem;
}
/* transport->node delivery wrappers (the transport's user is the node so on_shm can
 * reach SHM state; the app's single on_message sees identical bytes inline or SHM).
 * Because the transport shares one user across on_message/on_shm/allocator, we also
 * wrap the allocator to forward the app's real user_data. */
static void *dart__node_alloc(void *u, void *ptr, size_t size){
    dart_node *n = (dart_node*)u;
    return n->shm_alloc(n->user_data, ptr, size);
}
static void dart__node_on_message(void *u, uint16_t ch, uint32_t from, const void *data, size_t len){
    dart_node *n = (dart_node*)u;
    if (n->shm_user_on_message) n->shm_user_on_message(n->user_data, ch, from, data, len);
}
static int dart__node_on_shm(void *u, uint16_t ch, uint32_t from, const uint8_t *desc){
    dart_node *n = (dart_node*)u; dart_shm_desc d; dart_shm_pool *reader_pool; const void *p; uint32_t len;
    if (!dart_shm_desc_decode(&d, desc, DART_SHM_DESC_WIRE)) return 0;
    reader_pool = dart__shm_reader_pool(n, d.segment_id);
    if (!reader_pool) return 0;                                     /* can't attach -> reader NACKs (repair) */
    p = dart_shm_read(reader_pool, &d, &len);                       /* seqlock head: generation == descriptor? */
    if (!p) return 0;                                      /* recycled -> NACK -> repair or skip */
    /* one-copy: copy out of shared memory so the user owns the bytes (ack-timing safe) */
    if (len > n->shm_scratch_cap){
        void *new_buf = n->shm_alloc(n->user_data, n->shm_scratch, len?len:1u);
        if (!new_buf) return 0;
        n->shm_scratch = new_buf; n->shm_scratch_cap = len;
    }
    memcpy(n->shm_scratch, p, len);
    if (!dart_shm_verify(reader_pool, &d)) return 0;                /* seqlock tail: writer recycled mid-copy -> torn -> drop */
    n->shm_rx++;
    if (n->shm_user_on_message) n->shm_user_on_message(n->user_data, ch, from, n->shm_scratch, len);
    return 1;
}
#endif

size_t dart_node_required_memory(const dart_node_config *cfg){
    dart_discovery_rt_config discovery_rt_cfg; dart_config transport_cfg; uint16_t max_peers;
    size_t node_bytes, table_bytes, meta_bytes, discovery_bytes, transport_bytes;
    if (!cfg || cfg->n_channels==0) return 0;
    dart__node_cfgs(cfg,&discovery_rt_cfg,&transport_cfg,&max_peers);
    node_bytes = (sizeof(struct dart_node)+15u)&~(size_t)15u;
    table_bytes  = ((size_t)max_peers*sizeof(dart_node_peer)+15u)&~(size_t)15u;
    meta_bytes = ((size_t)dart__node_meta_capacity(cfg)+15u)&~(size_t)15u;
    discovery_bytes = (dart_discovery_rt_required_memory(&discovery_rt_cfg)+15u)&~(size_t)15u;
    transport_bytes   = (dart_required_memory(&transport_cfg)+15u)&~(size_t)15u;
    {   size_t shm_bytes = 0;
#ifdef DART_SHM
        shm_bytes = dart__node_shm_bytes(cfg, max_peers);
#endif
        return 32u + node_bytes + table_bytes + meta_bytes + discovery_bytes + transport_bytes + shm_bytes;
    }
}

dart_node *dart_node_open(void *mem, size_t cap, const dart_node_config *cfg){
    dart_discovery_rt_config discovery_rt_cfg; dart_config transport_cfg; uint16_t max_peers;
    uint8_t *base, *p; size_t node_bytes, table_bytes, meta_bytes, discovery_bytes, transport_bytes;
    dart_node *n; dart_sock fd; uint16_t local_port, frag;
    if (!mem || !cfg || cfg->n_channels==0) return NULL;
    if (cap < dart_node_required_memory(cfg)) return NULL;
    dart__node_cfgs(cfg,&discovery_rt_cfg,&transport_cfg,&max_peers);

    if (!dart_plat_startup()) return NULL;

    base = (uint8_t*)(((uintptr_t)mem+15u)&~(uintptr_t)15u);
    node_bytes = (sizeof(struct dart_node)+15u)&~(size_t)15u;
    table_bytes  = ((size_t)max_peers*sizeof(dart_node_peer)+15u)&~(size_t)15u;
    meta_bytes = ((size_t)dart__node_meta_capacity(cfg)+15u)&~(size_t)15u;
    discovery_bytes = (dart_discovery_rt_required_memory(&discovery_rt_cfg)+15u)&~(size_t)15u;
    transport_bytes   = (dart_required_memory(&transport_cfg)+15u)&~(size_t)15u;

    n=(dart_node*)base; memset(n,0,sizeof *n);
    n->fd = DART_SOCK_BAD; n->multicast_fd = DART_SOCK_BAD; n->max_peers=max_peers;
    n->domain = cfg->domain;
    n->on_event = cfg->on_event; n->user_data = cfg->user_data;
    n->multicast_port = cfg->net.multicast_port ? cfg->net.multicast_port
               : (uint16_t)((cfg->net.discovery_port ? cfg->net.discovery_port : 7400) + 1);
    /* our fragment size, clamped exactly as dart_init clamps it, baked into the blob */
    frag = cfg->net.fragment_size ? cfg->net.fragment_size : DART_FRAG_PAYLOAD;
    if (frag < DART_FRAG_PAYLOAD_MIN) frag = DART_FRAG_PAYLOAD_MIN;
    if (frag > DART_FRAG_PAYLOAD_MAX) frag = DART_FRAG_PAYLOAD_MAX;
    n->frag_size = frag;
#ifdef DART_SHM
    /* SHM is usable only with an allocator (the one-copy receive scratch + dynamic
       buffers); advertise capability accordingly and wrap the transport callbacks so
       on_shm can reach node state. The big segments are OS-mapped lazily, not here. */
    n->shm_capable = (uint8_t)(cfg->allocator != NULL);
    if (n->shm_capable){
        dart_plat_host_uuid(n->shm_host);
        if (!dart_plat_random(&n->shm_base, sizeof n->shm_base)) n->shm_base = dart_plat_pid();
        n->shm_base ^= (uint64_t)dart_plat_pid() << 32;    /* fold in pid for cross-process uniqueness */
        n->shm_base &= ~(((uint64_t)1u << 19) - 1u);       /* low 19 bits: 3 class + 16 channel index */
        if (n->shm_base == 0) n->shm_base = (uint64_t)1u << 19;
        n->shm_user_on_message = cfg->on_message;
        n->shm_alloc = cfg->allocator;
        transport_cfg.on_message = dart__node_on_message;
        transport_cfg.on_shm     = dart__node_on_shm;
        transport_cfg.allocator  = dart__node_alloc;
        transport_cfg.user       = n;
    }
#endif
    p = base + node_bytes;
    n->peers=(dart_node_peer*)p; memset(n->peers,0,(size_t)max_peers*sizeof(dart_node_peer));
    p += table_bytes;
    n->discovery_meta = p; n->discovery_meta_cap = dart__node_meta_capacity(cfg);
    p += meta_bytes;

    n->transport = dart_init(p, transport_bytes, &transport_cfg);
    if (!n->transport){ dart_plat_cleanup(); return NULL; }
    p += transport_bytes;
#ifdef DART_SHM
    {   size_t state_bytes = dart_shm_state_bytes();
        uint32_t n_segments = (uint32_t)cfg->n_channels * DART_SHM_N_CLASSES;
        uint16_t reader_max = dart__node_shm_reader_max(max_peers, cfg->n_channels); uint32_t i;
        n->shm_n_channels = cfg->n_channels; n->shm_reader_max = reader_max;
        n->shm_pool = (void**)p;     p += ((size_t)n_segments*sizeof(void*) + 15u)&~(size_t)15u;
        n->shm_pool_mem = p;         p += ((size_t)n_segments*state_bytes + 15u)&~(size_t)15u;
        n->shm_reader_segments = (uint64_t*)p;  p += ((size_t)reader_max*8u + 15u)&~(size_t)15u;
        n->shm_reader_pool_mem = p;        p += ((size_t)reader_max*state_bytes + 15u)&~(size_t)15u;
        for (i=0;i<n_segments;i++) n->shm_pool[i]=NULL;
        for (i=0;i<reader_max;i++) n->shm_reader_segments[i]=0;
    }
#endif

    /* Bind the data socket before opening discovery so we can advertise its real
       port (0 => OS ephemeral, read back via getsockname). No reuse: a unicast
       endpoint owns its port, so a collision fails loudly here. */
    fd = dart_plat_udp_open();
    if (fd==DART_SOCK_BAD){ dart_plat_cleanup(); return NULL; }
    if (!dart_plat_bind(fd, 0, cfg->net.data_port, 0)){
        dart_plat_close(fd); dart_plat_cleanup(); return NULL;
    }
    local_port = dart_plat_local_port(fd);
    if (local_port==0){ dart_plat_close(fd); dart_plat_cleanup(); return NULL; }
    dart_plat_set_nonblock(fd);   /* never block in recv/send; poll drains the queue */
    /* suppress WSAECONNRESET from a bounced send leaking into the shared RX path */
    dart_plat_suppress_connreset(fd);
    if (cfg->net.recv_buffer_bytes) dart_plat_set_rcvbuf(fd, (int)cfg->net.recv_buffer_bytes);
    if (cfg->net.send_buffer_bytes) dart_plat_set_sndbuf(fd, (int)cfg->net.send_buffer_bytes);
    n->fd=fd;
    discovery_rt_cfg.discovery.data_port = local_port;       /* advertise the actual port */

    /* multicast data: group TX rides the unicast socket; group RX needs its own
       socket on the shared multicast_port with one IGMP join per subscribed channel */
    { int want_rx=0, want_tx=0; uint16_t i; uint32_t interface_ip;
      for (i=0;i<cfg->n_channels;i++) if (cfg->channels[i].multicast){
          if (cfg->channels[i].role!=DART_PUB_ONLY) want_rx=1;
          if (cfg->channels[i].role!=DART_SUB_ONLY) want_tx=1;
      }
      /* pin data multicast to discovery's egress interface, else multihomed hosts
         pick per-group interfaces and break peer identification by source addr */
      interface_ip = !(want_rx||want_tx) ? 0
           : cfg->net.multicast_interface ? dart_plat_parse_ip(cfg->net.multicast_interface)
           : dart_plat_route_src(dart_plat_parse_ip(cfg->net.discovery_group?cfg->net.discovery_group:"239.255.0.7"),
                                  cfg->net.discovery_port?cfg->net.discovery_port:7400);
      if (want_tx){
          unsigned char multicast_ttl = cfg->net.multicast_ttl ? cfg->net.multicast_ttl : 1;
          dart_plat_mcast_setif(n->fd, interface_ip);
          dart_plat_mcast_ttl(n->fd, multicast_ttl);
          dart_plat_mcast_loop(n->fd, 1);
      }
      if (want_rx){
          dart_sock multicast_fd = dart_plat_udp_open();
          int multicast_ok = (multicast_fd!=DART_SOCK_BAD);
          if (multicast_ok && !dart_plat_bind(multicast_fd, 0, n->multicast_port, 1)) multicast_ok=0;
          if (multicast_ok){
              /* one join per distinct group (kernels reject dups); memberships are
                 OS-capped (~20: Linux net.ipv4.igmp_max_memberships), a fail fails open */
              for (i=0;i<cfg->n_channels;i++)
                  if (cfg->channels[i].multicast && cfg->channels[i].role!=DART_PUB_ONLY){
                      uint32_t group_addr = dart__node_chan_group(cfg->domain, &cfg->channels[i]);
                      uint16_t j; int dup=0;
                      for (j=0;j<i;j++)
                          if (cfg->channels[j].multicast && cfg->channels[j].role!=DART_PUB_ONLY
                              && dart__node_chan_group(cfg->domain, &cfg->channels[j])==group_addr){
                              dup=1; break;
                          }
                      if (dup) continue;
                      if (!dart_plat_mcast_join(multicast_fd, group_addr, interface_ip)){ multicast_ok=0; break; }
                  }
          }
          if (!multicast_ok){
              if (multicast_fd!=DART_SOCK_BAD) dart_plat_close(multicast_fd);
              dart_plat_close(fd); n->fd=DART_SOCK_BAD; dart_plat_cleanup(); return NULL;
          }
          dart_plat_mcast_loop(multicast_fd, 1);
          dart_plat_set_nonblock(multicast_fd);
          if (cfg->net.recv_buffer_bytes) dart_plat_set_rcvbuf(multicast_fd, (int)cfg->net.recv_buffer_bytes);
          n->multicast_fd=multicast_fd;
      } }

    discovery_rt_cfg.discovery.on_peer_up      = dart__node_up;
    discovery_rt_cfg.discovery.on_peer_down    = dart__node_down;
    discovery_rt_cfg.discovery.on_peer_refused = dart__node_refused;
    discovery_rt_cfg.discovery.user            = n;
    /* advertise our frag size + interest list so peers reassemble our messages and
       match topics straight from discovery. The buffer lives in the node. */
    n->discovery_meta_len = dart__meta_build(n);
    discovery_rt_cfg.discovery.meta = n->discovery_meta; discovery_rt_cfg.discovery.meta_len = n->discovery_meta_len;
    n->discovery = dart_discovery_rt_open(p, discovery_bytes, &discovery_rt_cfg);
    if (!n->discovery){
        if (n->multicast_fd!=DART_SOCK_BAD){ dart_plat_close(n->multicast_fd); n->multicast_fd=DART_SOCK_BAD; }
        dart_plat_close(fd); n->fd=DART_SOCK_BAD; dart_plat_cleanup(); return NULL;
    }
    p += discovery_bytes;

    return n;
}

/* max wall-time draining RX (and running on_message) per poll tick before
 * yielding to discovery, so a slow on_message never starves it */
#ifndef DART_RX_BUDGET_US
#define DART_RX_BUDGET_US 5000u
#endif

/* drain one socket's RX queue into the transport until empty or past deadline
 * (full drain avoids NACK storms). Distinct from public dart_node_drain. */
static void dart__node_rx_drain(dart_node *n, dart_sock fd, uint64_t deadline){
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
                dart_discovery_rt_feed(n->discovery, src_ip, 4, buf, (size_t)r);
            } else {
                int peer_idx=dart__node_find_addr(n, src_ip, src_port);
                if (peer_idx>=0) dart_on_datagram(n->transport, n->peers[peer_idx].id, buf, (size_t)r, dart_plat_now_us());
            }
        }
        if (dart_plat_now_us() >= deadline) break;      /* yield to discovery/send */
    }
}

int dart_node_poll(dart_node *n, int timeout_ms){
    uint8_t buf[DART_DGRAM_MAX]; uint32_t to; size_t out_len; uint64_t now;
    dart_pollfd pfd[2]; int n_fds=1;

    dart_discovery_rt_poll(n->discovery, 0);                 /* discovery tick (non-blocking) */

    memset(pfd,0,sizeof pfd);
    pfd[0].fd=n->fd; pfd[0].events=DART_POLLIN;
    if (n->multicast_fd!=DART_SOCK_BAD){ pfd[1].fd=n->multicast_fd; pfd[1].events=DART_POLLIN; n_fds=2; }
    /* cap the wait at the next internal timer so a due ack/NACK/heartbeat fires on
       time, not after the full quantum (no traffic to wake us when a writer stalls) */
    { uint64_t next_deadline = dart_next_deadline_us(n->transport);
      if (next_deadline){ uint64_t t0 = dart_plat_now_us();
               uint64_t us = (next_deadline > t0) ? next_deadline - t0 : 0;          /* until the timer */
               int ms = (us >= (uint64_t)timeout_ms*1000u) ? timeout_ms
                                                           : (int)((us + 999u)/1000u);
               if (ms < timeout_ms) timeout_ms = ms; } }       /* round up: no busy-spin */
    if (dart_plat_poll(pfd,n_fds,timeout_ms) > 0){
        uint64_t rx_deadline = dart_plat_now_us() + DART_RX_BUDGET_US;
        if (pfd[0].revents & DART_POLLIN) dart__node_rx_drain(n, n->fd, rx_deadline);
        if (n_fds==2 && (pfd[1].revents & DART_POLLIN)) dart__node_rx_drain(n, n->multicast_fd, rx_deadline);
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

int dart_node_send(dart_node *n, uint16_t channel, const void *data, size_t len){
    /* bounded backpressure: pump the loop (on_message/on_event may fire here) until
       a slow reader acks or qos.backpressure_wait_us elapses, then send anyway */
    const dart_qos *q = dart_channel_qos(n->transport, channel);
    if (q && q->backpressure_wait_us && dart_send_would_evict(n->transport, channel)){
        uint64_t t0 = dart_plat_now_us(), deadline = t0 + q->backpressure_wait_us;
        /* in-pump diagnostic: the publisher is blocked here for the whole wait, so its
           normal per-message print sees nothing within it. When a probe is set, sample
           the writer's repair progress on a ~interval timer so the stall is visible as a
           within-block time series (resends bursty-then-flat vs steady; writer idle for
           lack of NACKs). Observational; when no probe is set this whole block is skipped. */
        uint64_t sample_last = t0; uint32_t sample_polls = 0, sample_idle = 0;
        uint64_t interval = n->pump_probe_interval_us ? n->pump_probe_interval_us : 200000u;
        dart_repair_stats_t sample_prev;
        if (n->pump_probe) dart_repair_stats(n->transport, channel, &sample_prev);
        do {
            dart_node_poll(n, 1);
            if (n->pump_probe){
                uint64_t now = dart_plat_now_us();
                sample_polls++;
                if (dart_repair_pending(n->transport, channel) == 0) sample_idle++;
                if (now - sample_last >= interval){
                    dart_repair_stats_t sample_now; dart_pump_sample sample;
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
            dart_shm_pool *pool = dart__shm_chan_pool(n, channel, k, keep_last);
            uint16_t slot = dart_channel_hist_head(n->transport, channel);
            void *chunk_ptr = pool ? dart_shm_chunk(pool, slot, NULL) : NULL;
            if (chunk_ptr){
                dart_shm_desc d; uint8_t desc[DART_SHM_DESC_WIRE];
                memcpy(chunk_ptr, data, len);                       /* one-copy write into shm */
                dart_shm_stamp(pool, slot, (uint32_t)len, &d);
                dart_shm_desc_encode(&d, desc);
                if (dart_send_shm(n->transport, channel, chunk_ptr, len, desc, dart_plat_now_us())==0){
                    n->shm_tx++;
                    return 0;
                }
            }
        }
    }
#endif
    return dart_send(n->transport, channel, data, len, dart_plat_now_us());
}

int dart_node_set_role(dart_node *n, uint16_t channel, uint8_t role){
    int r = dart_set_role(n->transport, channel, role);
    if (r == 0){   /* re-advertise our interest: peers rematch as the new blob arrives */
        n->discovery_meta_len = dart__meta_build(n);
        dart_discovery_rt_set_meta(n->discovery, n->discovery_meta, n->discovery_meta_len);
    }
    return r;
}

void dart_node_backpressure_stats(dart_node *n, uint64_t *waited_us, uint32_t *waited_sends){
    if (waited_us)    *waited_us    = n->backpressure_total_us;
    if (waited_sends) *waited_sends = n->backpressure_wait_count;
}

void dart_node_repair_stats(dart_node *n, uint16_t channel, dart_repair_stats_t *out){
    dart_repair_stats(n->transport, channel, out);
}

void dart_node_set_pump_probe(dart_node *n, dart_pump_probe_fn fn, uint64_t interval_us, void *user){
    n->pump_probe = fn; n->pump_probe_interval_us = interval_us; n->pump_probe_user = user;
}

int dart_node_reader_progress(dart_node *n, uint16_t channel, uint32_t peer,
                              uint64_t *base_seqno, uint32_t *have, uint32_t *total){
    return dart_reader_progress(n->transport, channel, peer, base_seqno, have, total);
}

#ifdef DART_SHM
void dart_node_shm_stats(dart_node *n, uint32_t *sent, uint32_t *recv){
    if (sent) *sent = n->shm_tx;
    if (recv) *recv = n->shm_rx;
}
#endif

int dart_node_drain(dart_node *n, uint16_t channel, int timeout_ms){
    uint64_t deadline = dart_plat_now_us() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000u;
    while (!dart_send_drained(n->transport, channel)){
        if (dart_plat_now_us() >= deadline) return 0;
        dart_node_poll(n, 1);
    }
    return 1;
}

int dart_node_writer_match_count(dart_node *n, uint16_t channel){
    return dart_writer_match_count(n->transport, channel);
}

void dart_node_close(dart_node *n, int send_bye){
    if (!n) return;
    if (n->discovery) dart_discovery_rt_close(n->discovery, send_bye);
    if (n->multicast_fd != DART_SOCK_BAD) dart_plat_close(n->multicast_fd);
    if (n->fd != DART_SOCK_BAD) dart_plat_close(n->fd);
    if (n->transport) dart_destroy(n->transport);     /* free hook-allocated dynamic buffers */
#ifdef DART_SHM
    if (n->shm_capable){
        size_t state_bytes = dart_shm_state_bytes(); uint32_t i, n_segments = (uint32_t)n->shm_n_channels * DART_SHM_N_CLASSES;
        for (i=0;i<n_segments;i++)
            if (n->shm_pool[i]) dart_shm_detach((dart_shm_pool*)n->shm_pool[i]);   /* unlinks ours */
        for (i=0;i<n->shm_reader_max;i++)
            if (n->shm_reader_segments[i]) dart_shm_detach((dart_shm_pool*)(n->shm_reader_pool_mem + (size_t)i*state_bytes));
        if (n->shm_scratch) n->shm_alloc(n->user_data, n->shm_scratch, 0);
    }
#endif
    dart_plat_cleanup();
}
