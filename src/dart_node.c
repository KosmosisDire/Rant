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
} dart__nodepeer;

struct dart_node {
    dart_state     *tr;
    dart_discovery_rt     *disc;
    dart_sock       fd;       /* unicast data socket (also group TX) */
    dart_sock       mcfd;     /* multicast data RX socket (DART_SOCK_BAD if unused) */
    dart__nodepeer *peers;
    uint16_t      max_peers;
    uint16_t      domain;
    uint16_t      mc_port;
    /* datagram the socket refused; retried first next poll so it is never lost */
    uint8_t       txhold[DART_DGRAM_MAX];
    size_t        txhold_len;
    uint32_t      txhold_peer;
    /* backpressure accumulators, read via dart_node_backpressure_stats */
    uint64_t      backpressure_accum_us;
    uint32_t      backpressure_accum_n;
    /* one app callback for everything but message delivery; node fills PEER_UP/DOWN */
    dart_event_fn  on_event;
    void          *user_data;
    /* opaque blob carried in every discovery announce; must outlive the node. Holds
       this node's UDP fragment size + pub/sub interest list (see dart__meta_*). */
    uint8_t       *disc_meta;     /* arena, disc_meta_cap bytes */
    uint16_t       disc_meta_cap;
    uint16_t       disc_meta_len;
    uint16_t       frag_size;     /* our clamped UDP fragment size, baked into the blob */
#ifdef DART_SHM
    /* zero-fragment same-host path: lazy per-size-class segments (see dart_shm.h) */
    uint8_t        shm_cap;       /* 1 = an allocator is set, so SHM is usable */
    uint8_t        shm_host[16];  /* our host uuid (advertised; same-host check) */
    uint64_t       shm_base;      /* per-node segment id base; low 19 bits = (channel<<3)|class */
    dart_message_fn shm_user_on_message;  /* the app's real callback (we wrap it) */
    dart_alloc_fn  shm_alloc;     /* one-copy receive scratch (== cfg.allocator) */
    void          *shm_scratch; uint32_t shm_scratch_cap;
    void         **shm_pool;      /* [nchan*N_CLASSES] our (channel,class) segments (NULL = not created) */
    uint8_t       *shm_pool_mem;  /* arena: nchan * N_CLASSES * dart_shm_state_bytes */
    uint16_t       shm_nchan;
    uint64_t      *shm_rseg;      /* [rmax] attached reader-segment ids (0 = empty) */
    uint8_t       *shm_rpool_mem; /* [rmax * dart_shm_state_bytes] */
    uint16_t       shm_rmax;
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

static int dart__meta_ok(const uint8_t *meta, uint16_t mlen){
    return meta && mlen >= 5 && meta[0]=='D' && meta[1]=='N' && (meta[2]==2 || meta[2]==3);
}
static uint16_t dart__meta_pfx(const uint8_t *meta){
    return meta[2]==3 ? DART__META_PREFIX_V3 : DART__META_PREFIX_V2;
}
static uint16_t dart__meta_frag(const uint8_t *meta, uint16_t mlen){
    if (!dart__meta_ok(meta, mlen)) return 0;
    return (uint16_t)(meta[3] | ((uint16_t)meta[4] << 8));
}
/* Locate the interest sub-blob within a peer's meta; NULL/0 if absent. */
static const uint8_t *dart__meta_interest(const uint8_t *meta, uint16_t mlen, size_t *out_len){
    uint16_t pfx;
    if (!dart__meta_ok(meta, mlen) || mlen < (pfx = dart__meta_pfx(meta))){ *out_len = 0; return NULL; }
    *out_len = (size_t)(mlen - pfx);
    return meta + pfx;
}
#ifdef DART_SHM
/* A peer's SHM capability + host uuid (v3 only); 1 if SHM-capable, fills host[16]. */
static int dart__meta_shm(const uint8_t *meta, uint16_t mlen, uint8_t host[16]){
    if (!dart__meta_ok(meta, mlen) || meta[2]!=3 || mlen < DART__META_PREFIX_V3 || !meta[5]) return 0;
    memcpy(host, meta+6, 16);
    return 1;
}
#endif
/* (Re)build our blob: prefix (frag [+ shm/host]) + current interest list. */
static uint16_t dart__meta_build(dart_node *n){
    uint8_t *o = n->disc_meta; size_t il; uint16_t pre = DART__META_PREFIX;
    o[0]='D'; o[1]='N'; o[2]=(uint8_t)(DART__META_PREFIX==DART__META_PREFIX_V3 ? 3 : 2);
    o[3]=(uint8_t)(n->frag_size & 0xFF); o[4]=(uint8_t)(n->frag_size >> 8);
#ifdef DART_SHM
    o[5]=(uint8_t)(n->shm_cap?1:0); memcpy(o+6, n->shm_host, 16);
#endif
    il = dart_build_interest(n->tr, o + pre, n->disc_meta_cap - pre);
    return (uint16_t)(pre + il);
}

/* announce blob capacity: frag prefix + the largest interest list our channels can
 * produce, capped to fit one (IP-fragmentable) UDP datagram */
static uint16_t dart__node_meta_cap(const dart_node_config *cfg){
    size_t mc = DART__META_PREFIX + dart_interest_max(cfg->n_channels);
    if (mc > 65000u) mc = 65000u;
    return (uint16_t)mc;
}

#ifdef DART_SHM
/* reader-pool cache capacity: a same-host peer publishes on its channels, one segment
 * each, so size to peers x channels (clamped to the u16 the cache index uses) */
static uint16_t dart__node_shm_rmax(uint16_t mp, uint16_t nch){
    uint32_t r = (uint32_t)mp * (nch ? nch : 1u) * DART_SHM_N_CLASSES;
    return (uint16_t)(r > 0xFFFFu ? 0xFFFFu : r);
}
/* arena bytes for the SHM bookkeeping (per-channel pool ptrs + handles + locked class,
 * plus the reader caches; the segments themselves are OS-mapped, lazily, outside it) */
static size_t dart__node_shm_bytes(const dart_node_config *cfg, uint16_t mp){
    size_t sb = dart_shm_state_bytes();
    uint32_t np = (uint32_t)cfg->n_channels * DART_SHM_N_CLASSES;   /* (channel,class) segments */
    uint16_t rmax = dart__node_shm_rmax(mp, cfg->n_channels);
    size_t s = 0;
    s += ((size_t)np*sizeof(void*) + 15u)&~(size_t)15u;      /* (channel,class) pool ptr array */
    s += ((size_t)np*sb + 15u)&~(size_t)15u;                 /* (channel,class) pool handles */
    s += ((size_t)rmax*8u + 15u)&~(size_t)15u;               /* reader segment ids */
    s += ((size_t)rmax*sb + 15u)&~(size_t)15u;               /* reader pool handles */
    return s;
}
#endif

/* split node config into discovery + transport sub-configs */
static void dart__node_cfgs(const dart_node_config *cfg, dart_discovery_rt_config *dc,
                          dart_config *tc, uint16_t *mp_out){
    uint16_t mp = cfg->discovery.max_peers ? cfg->discovery.max_peers : 16;
    memset(dc, 0, sizeof *dc); memset(tc, 0, sizeof *tc);
    dc->disc.domain_id   = cfg->domain;
    dc->disc.data_port   = cfg->net.data_port;
    dc->disc.announce_us = cfg->discovery.announce_interval_us; /* 0 uses default */
    dc->disc.timeout_us  = cfg->discovery.peer_timeout_us;
    dc->disc.max_peers   = mp;
    dc->disc.meta_cap    = dart__node_meta_cap(cfg);
    dc->group            = cfg->net.discovery_group;
    dc->disc_port        = cfg->net.discovery_port;
    dc->ttl              = cfg->net.multicast_ttl;
    dc->mcast_if         = cfg->net.multicast_interface;
    dc->seeds            = cfg->net.seed_peers;
    dc->n_seeds          = cfg->net.n_seed_peers;
    tc->channels   = cfg->channels;
    tc->n_channels = cfg->n_channels;
    tc->max_peers  = mp;
    tc->frag_payload = cfg->net.fragment_size;   /* 0 = default; dart_init clamps to [MIN,MAX] */
    tc->on_message = cfg->on_message;
    tc->on_event   = cfg->on_event;
    tc->allocator  = cfg->allocator;
    tc->user       = cfg->user_data;
    if (mp_out) *mp_out = mp;
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
static void dart__node_set_peer_shm(dart_node *n, uint32_t id, const uint8_t *meta, uint16_t mlen){
    uint8_t host[16];
    int shm = n->shm_cap && dart__meta_shm(meta, mlen, host) &&
              dart_shm_host_match(host, n->shm_host);
    dart_peer_set_shm(n->tr, id, shm);
}
#endif

static void dart__node_up(void *u, uint32_t id, const dart_discovery_addr *addr,
                        const uint8_t *meta, uint16_t mlen){
    dart_node *n = (dart_node*)u; uint16_t i; int slot = -1;
    uint16_t frag = dart__meta_frag(meta, mlen);
    size_t ilen = 0; const uint8_t *interest = dart__meta_interest(meta, mlen, &ilen);
    for (i=0;i<n->max_peers;i++){
        if (n->peers[i].used && n->peers[i].id==id){      /* known peer: addr/interest update */
            memcpy(n->peers[i].ip, addr->ip, 16);
            n->peers[i].ip_len = addr->ip_len; n->peers[i].port = addr->port;
            dart_peer_set_frag(n->tr, id, frag);
#ifdef DART_SHM
            dart__node_set_peer_shm(n, id, meta, mlen);
#endif
            if (interest) dart_apply_peer_interest(n->tr, id, interest, ilen);
            if (n->peers[i].dormant){    /* a DROPPED peer's same incarnation returned: resume */
                n->peers[i].dormant = 0;
                dart_peer_resume(n->tr, id);   /* keeps reader position; writer fills any gap */
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
    dart_peer_add(n->tr, id, (addr->ip_len==4) && dart__node_is_local_ip(addr->ip), frag);
#ifdef DART_SHM
    dart__node_set_peer_shm(n, id, meta, mlen);
#endif
    if (interest) dart_apply_peer_interest(n->tr, id, interest, ilen);
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
            dart_peer_dormant(n->tr, id);
            if (n->on_event){
                dart_event ev; memset(&ev, 0, sizeof ev);
                ev.kind=DART_PEER_DOWN; ev.peer=id; ev.detail="peer dropped";
                n->on_event(n->user_data, &ev);
            }
        }
    } else {   /* GONE: said BYE or its slot was reclaimed; free the transport state */
        int notify = (i>=0 && !n->peers[i].dormant);   /* active->gone: app not yet told */
        if (i>=0) n->peers[i].used=0;
        dart_peer_remove(n->tr, id);
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
        port = n->mc_port;
    } else {
        int pi = dart__node_find_id(n, to);
        if (pi < 0) return 1;              /* peer vanished */
        memcpy(ip, n->peers[pi].ip, 4);
        port = n->peers[pi].port;
    }
    if (dart_plat_send(n->fd, buf, len, ip, port) < 0 && dart_plat_would_block())
        return 0;
    return 1;
}

#ifdef DART_SHM
/* OS object name "/dart.shm.<16 hex>" -- valid on POSIX (leading /) and Windows. */
static void dart__shm_name(char *buf, uint64_t seg){
    static const char hx[] = "0123456789abcdef";
    const char pre[] = "/dart.shm."; int i, k = 0;
    while (pre[k]){ buf[k] = pre[k]; k++; }
    for (i=15;i>=0;i--) buf[k++] = hx[(seg >> (4*i)) & 0xF];
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
    uint16_t i, slot = 0xFFFF; dart_shm_config c; uint8_t *mem; size_t sb;
    sb = dart_shm_state_bytes();
    for (i=0;i<n->shm_rmax;i++){
        if (n->shm_rseg[i]==seg) return (dart_shm_pool*)(n->shm_rpool_mem + (size_t)i*sb);
        if (n->shm_rseg[i]==0 && slot==0xFFFF) slot = i;
    }
    if (slot==0xFFFF) return NULL;                         /* cache full */
    memset(&c, 0, sizeof c);
    c.segment_id = seg; dart__shm_name(c.name, seg);        /* attach maps whole + reads geometry */
    mem = n->shm_rpool_mem + (size_t)slot*sb;
    if (!dart_shm_attach(mem, &c)) return NULL;
    n->shm_rseg[slot] = seg;
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
    dart_node *n = (dart_node*)u; dart_shm_desc d; dart_shm_pool *rp; const void *p; uint32_t len;
    if (!dart_shm_desc_decode(&d, desc, DART_SHM_DESC_WIRE)) return 0;
    rp = dart__shm_reader_pool(n, d.segment_id);
    if (!rp) return 0;                                     /* can't attach -> reader NACKs (repair) */
    p = dart_shm_read(rp, &d, &len);                       /* seqlock head: generation == descriptor? */
    if (!p) return 0;                                      /* recycled -> NACK -> repair or skip */
    /* one-copy: copy out of shared memory so the user owns the bytes (ack-timing safe) */
    if (len > n->shm_scratch_cap){
        void *nb = n->shm_alloc(n->user_data, n->shm_scratch, len?len:1u);
        if (!nb) return 0;
        n->shm_scratch = nb; n->shm_scratch_cap = len;
    }
    memcpy(n->shm_scratch, p, len);
    if (!dart_shm_verify(rp, &d)) return 0;                /* seqlock tail: writer recycled mid-copy -> torn -> drop */
    n->shm_rx++;
    if (n->shm_user_on_message) n->shm_user_on_message(n->user_data, ch, from, n->shm_scratch, len);
    return 1;
}
#endif

size_t dart_node_required_memory(const dart_node_config *cfg){
    dart_discovery_rt_config dc; dart_config tc; uint16_t mp;
    size_t node_sz, tbl_sz, meta_sz, disc_sz, tr_sz;
    if (!cfg || cfg->n_channels==0) return 0;
    dart__node_cfgs(cfg,&dc,&tc,&mp);
    node_sz = (sizeof(struct dart_node)+15u)&~(size_t)15u;
    tbl_sz  = ((size_t)mp*sizeof(dart__nodepeer)+15u)&~(size_t)15u;
    meta_sz = ((size_t)dart__node_meta_cap(cfg)+15u)&~(size_t)15u;
    disc_sz = (dart_discovery_rt_required_memory(&dc)+15u)&~(size_t)15u;
    tr_sz   = (dart_required_memory(&tc)+15u)&~(size_t)15u;
    {   size_t shm_sz = 0;
#ifdef DART_SHM
        shm_sz = dart__node_shm_bytes(cfg, mp);
#endif
        return 32u + node_sz + tbl_sz + meta_sz + disc_sz + tr_sz + shm_sz;
    }
}

dart_node *dart_node_open(void *mem, size_t cap, const dart_node_config *cfg){
    dart_discovery_rt_config dc; dart_config tc; uint16_t mp;
    uint8_t *base, *p; size_t node_sz, tbl_sz, meta_sz, disc_sz, tr_sz;
    dart_node *n; dart_sock fd; uint16_t lp, f;
    if (!mem || !cfg || cfg->n_channels==0) return NULL;
    if (cap < dart_node_required_memory(cfg)) return NULL;
    dart__node_cfgs(cfg,&dc,&tc,&mp);

    if (!dart_plat_startup()) return NULL;

    base = (uint8_t*)(((uintptr_t)mem+15u)&~(uintptr_t)15u);
    node_sz = (sizeof(struct dart_node)+15u)&~(size_t)15u;
    tbl_sz  = ((size_t)mp*sizeof(dart__nodepeer)+15u)&~(size_t)15u;
    meta_sz = ((size_t)dart__node_meta_cap(cfg)+15u)&~(size_t)15u;
    disc_sz = (dart_discovery_rt_required_memory(&dc)+15u)&~(size_t)15u;
    tr_sz   = (dart_required_memory(&tc)+15u)&~(size_t)15u;

    n=(dart_node*)base; memset(n,0,sizeof *n);
    n->fd = DART_SOCK_BAD; n->mcfd = DART_SOCK_BAD; n->max_peers=mp;
    n->domain = cfg->domain;
    n->on_event = cfg->on_event; n->user_data = cfg->user_data;
    n->mc_port = cfg->net.multicast_port ? cfg->net.multicast_port
               : (uint16_t)((cfg->net.discovery_port ? cfg->net.discovery_port : 7400) + 1);
    /* our fragment size, clamped exactly as dart_init clamps it, baked into the blob */
    f = cfg->net.fragment_size ? cfg->net.fragment_size : DART_FRAG_PAYLOAD;
    if (f < DART_FRAG_PAYLOAD_MIN) f = DART_FRAG_PAYLOAD_MIN;
    if (f > DART_FRAG_PAYLOAD_MAX) f = DART_FRAG_PAYLOAD_MAX;
    n->frag_size = f;
#ifdef DART_SHM
    /* SHM is usable only with an allocator (the one-copy receive scratch + dynamic
       buffers); advertise capability accordingly and wrap the transport callbacks so
       on_shm can reach node state. The big segments are OS-mapped lazily, not here. */
    n->shm_cap = (uint8_t)(cfg->allocator != NULL);
    if (n->shm_cap){
        dart_plat_host_uuid(n->shm_host);
        if (!dart_plat_random(&n->shm_base, sizeof n->shm_base)) n->shm_base = dart_plat_pid();
        n->shm_base ^= (uint64_t)dart_plat_pid() << 32;    /* fold in pid for cross-process uniqueness */
        n->shm_base &= ~(((uint64_t)1u << 19) - 1u);       /* low 19 bits: 3 class + 16 channel index */
        if (n->shm_base == 0) n->shm_base = (uint64_t)1u << 19;
        n->shm_user_on_message = cfg->on_message;
        n->shm_alloc = cfg->allocator;
        tc.on_message = dart__node_on_message;
        tc.on_shm     = dart__node_on_shm;
        tc.allocator  = dart__node_alloc;
        tc.user       = n;
    }
#endif
    p = base + node_sz;
    n->peers=(dart__nodepeer*)p; memset(n->peers,0,(size_t)mp*sizeof(dart__nodepeer));
    p += tbl_sz;
    n->disc_meta = p; n->disc_meta_cap = dart__node_meta_cap(cfg);
    p += meta_sz;

    n->tr = dart_init(p, tr_sz, &tc);
    if (!n->tr){ dart_plat_cleanup(); return NULL; }
    p += tr_sz;
#ifdef DART_SHM
    {   size_t sb = dart_shm_state_bytes();
        uint32_t np = (uint32_t)cfg->n_channels * DART_SHM_N_CLASSES;
        uint16_t rmax = dart__node_shm_rmax(mp, cfg->n_channels); uint32_t i;
        n->shm_nchan = cfg->n_channels; n->shm_rmax = rmax;
        n->shm_pool = (void**)p;     p += ((size_t)np*sizeof(void*) + 15u)&~(size_t)15u;
        n->shm_pool_mem = p;         p += ((size_t)np*sb + 15u)&~(size_t)15u;
        n->shm_rseg = (uint64_t*)p;  p += ((size_t)rmax*8u + 15u)&~(size_t)15u;
        n->shm_rpool_mem = p;        p += ((size_t)rmax*sb + 15u)&~(size_t)15u;
        for (i=0;i<np;i++) n->shm_pool[i]=NULL;
        for (i=0;i<rmax;i++) n->shm_rseg[i]=0;
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
    lp = dart_plat_local_port(fd);
    if (lp==0){ dart_plat_close(fd); dart_plat_cleanup(); return NULL; }
    dart_plat_set_nonblock(fd);   /* never block in recv/send; poll drains the queue */
    /* suppress WSAECONNRESET from a bounced send leaking into the shared RX path */
    dart_plat_suppress_connreset(fd);
    if (cfg->net.recv_buffer_bytes) dart_plat_set_rcvbuf(fd, (int)cfg->net.recv_buffer_bytes);
    if (cfg->net.send_buffer_bytes) dart_plat_set_sndbuf(fd, (int)cfg->net.send_buffer_bytes);
    n->fd=fd;
    dc.disc.data_port = lp;       /* advertise the actual port */

    /* multicast data: group TX rides the unicast socket; group RX needs its own
       socket on the shared mc_port with one IGMP join per subscribed channel */
    { int want_rx=0, want_tx=0; uint16_t i; uint32_t ifip;
      for (i=0;i<cfg->n_channels;i++) if (cfg->channels[i].multicast){
          if (cfg->channels[i].role!=DART_PUB_ONLY) want_rx=1;
          if (cfg->channels[i].role!=DART_SUB_ONLY) want_tx=1;
      }
      /* pin data multicast to discovery's egress interface, else multihomed hosts
         pick per-group interfaces and break peer identification by source addr */
      ifip = !(want_rx||want_tx) ? 0
           : cfg->net.multicast_interface ? dart_plat_parse_ip(cfg->net.multicast_interface)
           : dart_plat_route_src(dart_plat_parse_ip(cfg->net.discovery_group?cfg->net.discovery_group:"239.255.0.7"),
                                  cfg->net.discovery_port?cfg->net.discovery_port:7400);
      if (want_tx){
          unsigned char mttl = cfg->net.multicast_ttl ? cfg->net.multicast_ttl : 1;
          dart_plat_mcast_setif(n->fd, ifip);
          dart_plat_mcast_ttl(n->fd, mttl);
          dart_plat_mcast_loop(n->fd, 1);
      }
      if (want_rx){
          dart_sock mfd = dart_plat_udp_open();
          int mc_ok = (mfd!=DART_SOCK_BAD);
          if (mc_ok && !dart_plat_bind(mfd, 0, n->mc_port, 1)) mc_ok=0;
          if (mc_ok){
              /* one join per distinct group (kernels reject dups); memberships are
                 OS-capped (~20: Linux net.ipv4.igmp_max_memberships), a fail fails open */
              for (i=0;i<cfg->n_channels;i++)
                  if (cfg->channels[i].multicast && cfg->channels[i].role!=DART_PUB_ONLY){
                      uint32_t g = dart__node_chan_group(cfg->domain, &cfg->channels[i]);
                      uint16_t j; int dup=0;
                      for (j=0;j<i;j++)
                          if (cfg->channels[j].multicast && cfg->channels[j].role!=DART_PUB_ONLY
                              && dart__node_chan_group(cfg->domain, &cfg->channels[j])==g){
                              dup=1; break;
                          }
                      if (dup) continue;
                      if (!dart_plat_mcast_join(mfd, g, ifip)){ mc_ok=0; break; }
                  }
          }
          if (!mc_ok){
              if (mfd!=DART_SOCK_BAD) dart_plat_close(mfd);
              dart_plat_close(fd); n->fd=DART_SOCK_BAD; dart_plat_cleanup(); return NULL;
          }
          dart_plat_mcast_loop(mfd, 1);
          dart_plat_set_nonblock(mfd);
          if (cfg->net.recv_buffer_bytes) dart_plat_set_rcvbuf(mfd, (int)cfg->net.recv_buffer_bytes);
          n->mcfd=mfd;
      } }

    dc.disc.on_peer_up      = dart__node_up;
    dc.disc.on_peer_down    = dart__node_down;
    dc.disc.on_peer_refused = dart__node_refused;
    dc.disc.user            = n;
    /* advertise our frag size + interest list so peers reassemble our messages and
       match topics straight from discovery. The buffer lives in the node. */
    n->disc_meta_len = dart__meta_build(n);
    dc.disc.meta = n->disc_meta; dc.disc.meta_len = n->disc_meta_len;
    n->disc = dart_discovery_rt_open(p, disc_sz, &dc);
    if (!n->disc){
        if (n->mcfd!=DART_SOCK_BAD){ dart_plat_close(n->mcfd); n->mcfd=DART_SOCK_BAD; }
        dart_plat_close(fd); n->fd=DART_SOCK_BAD; dart_plat_cleanup(); return NULL;
    }
    p += disc_sz;

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
        uint8_t sip[4]; uint16_t sport;
        int r = dart_plat_recv(fd, buf, sizeof buf, sip, &sport);
        if (r<0){
            if (dart_plat_would_block()) break;        /* queue empty */
            continue;   /* per-datagram error (e.g. bounced send); keep draining */
        }
        if (r>0){
            if (r>=4 && buf[0]=='u' && buf[1]=='D' && buf[2]=='S' && buf[3]=='C'){
                /* unicast announce aimed at our data port: hand it to discovery */
                dart_discovery_rt_feed(n->disc, sip, 4, buf, (size_t)r);
            } else {
                int pi=dart__node_find_addr(n, sip, sport);
                if (pi>=0) dart_on_datagram(n->tr, n->peers[pi].id, buf, (size_t)r, dart_plat_now_us());
            }
        }
        if (dart_plat_now_us() >= deadline) break;      /* yield to discovery/send */
    }
}

int dart_node_poll(dart_node *n, int timeout_ms){
    uint8_t buf[DART_DGRAM_MAX]; uint32_t to; size_t ol; uint64_t now;
    dart_pollfd pfd[2]; int nf=1;

    dart_discovery_rt_poll(n->disc, 0);                 /* discovery tick (non-blocking) */

    memset(pfd,0,sizeof pfd);
    pfd[0].fd=n->fd; pfd[0].events=DART_POLLIN;
    if (n->mcfd!=DART_SOCK_BAD){ pfd[1].fd=n->mcfd; pfd[1].events=DART_POLLIN; nf=2; }
    /* cap the wait at the next internal timer so a due ack/NACK/heartbeat fires on
       time, not after the full quantum (no traffic to wake us when a writer stalls) */
    { uint64_t nd = dart_next_deadline_us(n->tr);
      if (nd){ uint64_t t0 = dart_plat_now_us();
               uint64_t us = (nd > t0) ? nd - t0 : 0;          /* until the timer */
               int ms = (us >= (uint64_t)timeout_ms*1000u) ? timeout_ms
                                                           : (int)((us + 999u)/1000u);
               if (ms < timeout_ms) timeout_ms = ms; } }       /* round up: no busy-spin */
    if (dart_plat_poll(pfd,nf,timeout_ms) > 0){
        uint64_t rx_deadline = dart_plat_now_us() + DART_RX_BUDGET_US;
        if (pfd[0].revents & DART_POLLIN) dart__node_rx_drain(n, n->fd, rx_deadline);
        if (nf==2 && (pfd[1].revents & DART_POLLIN)) dart__node_rx_drain(n, n->mcfd, rx_deadline);
    }

    now=dart_plat_now_us();
    /* the core already consumed any held datagram, so retry it before pulling new */
    if (n->txhold_len && dart__node_tx(n, n->txhold_peer, n->txhold, n->txhold_len))
        n->txhold_len = 0;
    if (!n->txhold_len)
        while (dart_poll_send(n->tr,&to,buf,sizeof buf,&ol,now)){
            if (!dart__node_tx(n, to, buf, ol)){
                memcpy(n->txhold, buf, ol);
                n->txhold_len = ol; n->txhold_peer = to;
                break;          /* TX buffer full: yield this tick */
            }
            now=dart_plat_now_us();
        }
    return 0;
}

int dart_node_send(dart_node *n, uint16_t channel, const void *data, size_t len){
    /* bounded backpressure: pump the loop (on_message/on_event may fire here) until
       a slow reader acks or qos.backpressure_wait_us elapses, then send anyway */
    const dart_qos *q = dart_channel_qos(n->tr, channel);
    if (q && q->backpressure_wait_us && dart_send_would_evict(n->tr, channel)){
        uint64_t t0 = dart_plat_now_us(), deadline = t0 + q->backpressure_wait_us;
        do {
            dart_node_poll(n, 1);
            if (!dart_send_would_evict(n->tr, channel)) break;
        } while (dart_plat_now_us() < deadline);
        n->backpressure_accum_us += dart_plat_now_us() - t0;
        n->backpressure_accum_n++;
    }
#ifdef DART_SHM
    if (n->shm_cap && len>0 && channel < n->shm_nchan && dart_writer_shm_eligible(n->tr, channel)){
        uint16_t keep_last = (q && q->keep_last) ? q->keep_last : 1u;
        /* a hint (shm_max_bytes / max_message_bytes) pins the channel to one class, so
           same-sized traffic reuses a single pre-sized segment; without it each message
           uses its own size class's segment, created on demand. Per channel either way. */
        uint32_t hint = q ? (q->shm_max_bytes ? q->shm_max_bytes : q->max_message_bytes) : 0u;
        uint32_t k = dart_shm_class_for(hint ? hint : (uint32_t)len);
        /* fits its class (with a hint, the hinted class) -> publish via SHM; chunk index =
           the history slot this send will occupy, so chunk i binds slot i (no free list) */
        if (k < DART_SHM_N_CLASSES && (uint32_t)len <= dart_shm_class_bytes(k)){
            dart_shm_pool *pool = dart__shm_chan_pool(n, channel, k, keep_last);
            uint16_t slot = dart_channel_hist_head(n->tr, channel);
            void *cp = pool ? dart_shm_chunk(pool, slot, NULL) : NULL;
            if (cp){
                dart_shm_desc d; uint8_t desc[DART_SHM_DESC_WIRE];
                memcpy(cp, data, len);                       /* one-copy write into shm */
                dart_shm_stamp(pool, slot, (uint32_t)len, &d);
                dart_shm_desc_encode(&d, desc);
                if (dart_send_shm(n->tr, channel, cp, len, desc, dart_plat_now_us())==0){
                    n->shm_tx++;
                    return 0;
                }
            }
        }
    }
#endif
    return dart_send(n->tr, channel, data, len, dart_plat_now_us());
}

int dart_node_set_role(dart_node *n, uint16_t channel, uint8_t role){
    int r = dart_set_role(n->tr, channel, role);
    if (r == 0){   /* re-advertise our interest: peers rematch as the new blob arrives */
        n->disc_meta_len = dart__meta_build(n);
        dart_discovery_rt_set_meta(n->disc, n->disc_meta, n->disc_meta_len);
    }
    return r;
}

void dart_node_backpressure_stats(dart_node *n, uint64_t *waited_us, uint32_t *waited_sends){
    if (waited_us)    *waited_us    = n->backpressure_accum_us;
    if (waited_sends) *waited_sends = n->backpressure_accum_n;
}

void dart_node_repair_stats(dart_node *n, uint16_t channel, dart_repair_stats_t *out){
    dart_repair_stats(n->tr, channel, out);
}

int dart_node_reader_progress(dart_node *n, uint16_t channel, uint32_t peer,
                              uint64_t *base_seqno, uint32_t *have, uint32_t *total){
    return dart_reader_progress(n->tr, channel, peer, base_seqno, have, total);
}

#ifdef DART_SHM
void dart_node_shm_stats(dart_node *n, uint32_t *sent, uint32_t *recv){
    if (sent) *sent = n->shm_tx;
    if (recv) *recv = n->shm_rx;
}
#endif

int dart_node_drain(dart_node *n, uint16_t channel, int timeout_ms){
    uint64_t deadline = dart_plat_now_us() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000u;
    while (!dart_send_drained(n->tr, channel)){
        if (dart_plat_now_us() >= deadline) return 0;
        dart_node_poll(n, 1);
    }
    return 1;
}

int dart_node_writer_match_count(dart_node *n, uint16_t channel){
    return dart_writer_match_count(n->tr, channel);
}

void dart_node_close(dart_node *n, int send_bye){
    if (!n) return;
    if (n->disc) dart_discovery_rt_close(n->disc, send_bye);
    if (n->mcfd != DART_SOCK_BAD) dart_plat_close(n->mcfd);
    if (n->fd != DART_SOCK_BAD) dart_plat_close(n->fd);
    if (n->tr) dart_destroy(n->tr);     /* free hook-allocated dynamic buffers */
#ifdef DART_SHM
    if (n->shm_cap){
        size_t sb = dart_shm_state_bytes(); uint32_t i, np = (uint32_t)n->shm_nchan * DART_SHM_N_CLASSES;
        for (i=0;i<np;i++)
            if (n->shm_pool[i]) dart_shm_detach((dart_shm_pool*)n->shm_pool[i]);   /* unlinks ours */
        for (i=0;i<n->shm_rmax;i++)
            if (n->shm_rseg[i]) dart_shm_detach((dart_shm_pool*)(n->shm_rpool_mem + (size_t)i*sb));
        if (n->shm_scratch) n->shm_alloc(n->user_data, n->shm_scratch, 0);
    }
#endif
    dart_plat_cleanup();
}
