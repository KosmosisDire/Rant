/* sans-IO peer-discovery core. See dart_discovery.h. */
#include "core.h"
#include "../common/bytes.h"
#include "../common/arena.h"
#include <string.h>

#define DART_DISCOVERY_HDR_LEN 24            /* magic(4) ver(1) flags(1) domain(2) uuid(16) */
#define DART_DISCOVERY_FLAG_BYE 0x01
#define DART_DISCOVERY_FLAG_REQ 0x02         /* solicit: recipients announce back now */
#define DART_DISCOVERY_FLAG_RELAY_ME 0x04    /* sender cannot multicast: whoever hears this DIRECTLY
                                                should re-announce it on their own paths */
#define DART_DISCOVERY_FLAG_PROXIED  0x08    /* this announce was rebuilt by a relay on another
                                                node's behalf: never re-relayed (loop stop), and its
                                                embedded locator is a candidate, not authoritative */
#define DART_DISCOVERY_BLOB_RESEND 3u        /* announces that carry the full blob after a change */
#define DART_DISCOVERY_INTRODUCE_IDLE 0xFFFFu   /* no introduction walk in progress for the peer */
#define DART_DISCOVERY_INTRODUCE_SWEEP 10u      /* re-introduce every this many announce intervals
                                                   (repair for lost introductions; the real work is
                                                   change-triggered) */

struct i_DartDiscoveryPeer {
    uint8_t  used;
    uint8_t  dropped;       /* used but silent past peer_timeout_us: state kept for a same-UUID return */
    uint8_t  uuid[16];
    uint32_t local_id;
    uint8_t  ip[16];
    uint8_t  ip_len;
    uint16_t port;
    uint64_t last_heard_us;
    uint64_t last_direct_us; /* last NON-proxied announce. Liveness for the RELAY/INTRODUCE
                                decision, distinct from last_heard_us: every relay hears the
                                other relays' proxies (and its own multicast-looped echoes),
                                and a proxy carries the origin's uuid, so the self-filter
                                never drops it. If proxies sustained the relay decision, a
                                dead origin would be re-announced forever (each relay's proxy
                                refreshing the others), and the whole mesh would keep a ghost
                                alive. Proxies still refresh last_heard_us (second-hand
                                liveness for third parties), so a peer drops mesh-wide within
                                ~2x peer_timeout of its real silence: one timeout for every
                                relay to stop, one more for the proxy-fed entries to expire. */
    uint64_t addr_heard_us;  /* last datagram that actually ARRIVED from ip: an incumbent locator
                                stays put while fresh, so multi-path announces can't flap it */
    uint8_t *meta;          /* the OVERLAY only: a meta_pool slot (capacity st->meta_cap),
                               or a hook allocation grown to the largest blob this slot has held */
    uint16_t meta_cap;      /* allocated capacity of this slot's meta buffer */
    uint16_t meta_len;
    uint32_t meta_version;  /* version of the blob we hold (0 = none yet) */
    uint32_t adv_version;   /* highest version the peer has advertised (> meta_version => we hold a stale blob) */
    uint8_t *user;          /* -> user_pool slot (opaque consumer scratch), stride st->user_stride */
    char     name[DART_DISCOVERY_NAME_MAX + 1];  /* advertised peer name, parsed from the blob */
    uint8_t  name_len;
    uint8_t  reply_due;     /* owes a unicast announce+blob (this peer solicited us) */
    uint8_t  solicit_due;   /* owes a unicast REQ (re-fetch: peer's version is ahead) */
    uint8_t  wants_relay;   /* its DIRECT announces carry RELAY_ME: we re-announce it for it */
    uint8_t  relay_due;     /* a proxied announce for it is due out of poll_relay */
    uint8_t  proxies_heard; /* FOREIGN proxies for this origin heard since our last announce
                               tick (saturating). 2+ suppress our own periodic emission for
                               one interval: with N direct hearers the mesh needs 2-3 active
                               relays, not N, and the earliest timer phases win a stable
                               quorum. Self-healing: when the active relays die, the silence
                               re-enlists everyone at their next tick. Change-triggered
                               emissions (a new/changed origin) bypass it, so propagation
                               of NEWS is never delayed, only steady-state repetition. */
    uint8_t  relay_me;      /* its announces (direct, or proxied on its behalf) declare RELAY_ME:
                               a unicast-only origin, whose advertised locator is not a usable
                               endpoint identity (behind a NAT it is pure fiction) */
    uint8_t  stated_ip;     /* its blob states a self_ip: authoritative, observed never binds */
    uint8_t  heard_direct;  /* heard a non-proxied announce since (re)appearing: ours to introduce */
    /* observed sources of its direct RELAY_ME announces, one per local channel (ip_len 0 =
       none). A unicast-only peer sends everything from its data socket and a NAT may rewrite
       the source per flow, so these, not the advertised locator, are its return paths:
       obs_data is where OUR data-socket sends pass the NAT's endpoint filter, obs_disc where
       our discovery-socket sends do. Rebinding on a changed source is QUIET (a NAT mapping
       expired and the next announce reopened it): sends just follow, no peer_up refires. */
    DartDiscoveryAddr obs_disc;
    DartDiscoveryAddr obs_data;
    uint16_t introduce_cursor;   /* introductions owed TO this relay-me peer: next table slot
                                    to proxy at it, or DART_DISCOVERY_INTRODUCE_IDLE */
    uint64_t introduce_sweep_us; /* next periodic re-introduction (lost-datagram repair) */
};
typedef struct i_DartDiscoveryPeer i_DartDiscoveryPeer;

struct DartDiscoveryState {
    DartDiscoveryCoreConfig  cfg;
    uint64_t      next_announce_us;
    uint32_t      next_local_id;
    uint8_t       started;
    uint8_t       want_solicit;   /* a multicast solicit (REQ) is queued for the next update */
    uint16_t      cap_peers;
    uint16_t      meta_cap;       /* per-peer meta buffer capacity */
    uint8_t      *meta_pool;      /* [cap_peers * meta_cap]; NULL in hook mode (per-peer
                                     blobs are then cfg.alloc allocations at actual size) */
    uint16_t      user_stride;         /* per-peer user-scratch bytes, 8-aligned (0 = none) */
    uint8_t      *user_pool;      /* [cap_peers * user_stride] opaque consumer scratch */
    /* our outgoing blob + monotonic version */
    DartBytes     self_meta;        /* our OVERLAY (the node's frag/interest); discovery carries it.
                                       A read-only view of the node's buffer, not owned here. */
    uint32_t      self_meta_version;
    char          self_name[DART_DISCOVERY_NAME_MAX + 1];  /* our advertised name (copied from cfg) */
    uint8_t       self_name_len;
    uint16_t      self_blob_resend; /* announces remaining that carry the full blob */
    uint16_t      targeted_cursor;  /* round-robin over peers for poll_targeted */
    uint16_t      relay_cursor;     /* round-robin over peers for poll_relay */
    DartDiscoverySubnet local_nets[DART_DISCOVERY_MAX_SUBNETS];  /* our own subnets (locator ranking) */
    uint8_t       n_local_nets;
    i_DartDiscoveryPeer  *peers;
};

/* peer events out: build the DartDiscoveryEvent and hand it to the one on_event sink.
 * peer_up surfaces the parsed name + the opaque overlay we hold for the peer. */
static void i_dart_discovery_fire_up(DartDiscoveryState *st, const i_DartDiscoveryPeer *peer,
                               const DartDiscoveryAddr *addr){
    DartDiscoveryEvent ev;
    if (!st->cfg.on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = DART_DISCOVERY_PEER_UP; ev.user = st->cfg.user; ev.peer = peer->local_id;
    if (addr) ev.addr = *addr;
    ev.name = dart_string(peer->name_len ? peer->name : NULL, peer->name_len);
    ev.meta = dart_bytes(peer->meta_len ? peer->meta : NULL, peer->meta_len);
    st->cfg.on_event(&ev);
}
static void i_dart_discovery_fire_down(DartDiscoveryState *st, uint32_t id, DartDiscoveryDownReason reason){
    DartDiscoveryEvent ev;
    if (!st->cfg.on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = DART_DISCOVERY_PEER_DOWN; ev.user = st->cfg.user; ev.peer = id; ev.reason = reason;
    st->cfg.on_event(&ev);
}
static void i_dart_discovery_fire_refused(DartDiscoveryState *st, const DartDiscoveryAddr *addr){
    DartDiscoveryEvent ev;
    if (!st->cfg.on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = DART_DISCOVERY_PEER_REFUSED; ev.user = st->cfg.user;
    if (addr) ev.addr = *addr;
    st->cfg.on_event(&ev);
}
static void i_dart_discovery_fire_meta_too_big(DartDiscoveryState *st, uint32_t id,
                                     const DartDiscoveryAddr *addr, DartBytes overlay){
    DartDiscoveryEvent ev;
    if (!st->cfg.on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = DART_DISCOVERY_META_TOO_BIG; ev.user = st->cfg.user; ev.peer = id;
    if (addr) ev.addr = *addr;
    ev.meta = overlay;
    st->cfg.on_event(&ev);
}

static uint32_t i_dart_discovery_fnv(const uint8_t *d, size_t n){
    uint32_t h = 2166136261u; size_t i;
    for (i=0;i<n;i++){ h ^= d[i]; h *= 16777619u; }
    return h;
}

void dart_discovery_make_uuid(uint8_t out[16], DartBytes stable, uint64_t seed){
    uint64_t x = 1469598103934665603ull; size_t i; int k;
    for (i=0;i<stable.len;i++){ x = (x ^ stable.data[i]) * 1099511628211ull; }
    x ^= seed;
    for (k=0;k<2;k++){
        uint64_t z = (x += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z>>30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z>>27)) * 0x94D049BB133111EBull;
        z ^=  z>>31;
        memcpy(out + (size_t)k*8, &z, 8);
    }
    out[6] = (uint8_t)((out[6] & 0x0Fu) | 0x80u);  /* version 8 (custom) */
    out[8] = (uint8_t)((out[8] & 0x3Fu) | 0x80u);  /* variant 10x (RFC) */
}

static uint16_t i_dart_discovery_meta_cap(const DartDiscoveryCoreConfig *cfg){
    return cfg->meta_cap ? cfg->meta_cap : DART_DISCOVERY_META_MAX;
}

/* per-peer user-scratch stride: the requested bytes rounded up to 8 so every slot is
 * 8-aligned (a consumer may store a pointer there). 0 bytes => no pool. */
static uint16_t i_dart_discovery_user_stride(const DartDiscoveryCoreConfig *cfg){
    return cfg->peer_user_bytes ? (uint16_t)((cfg->peer_user_bytes + 7u) & ~7u) : 0u;
}

void dart_discovery_config_defaults(DartDiscoveryCoreConfig *cfg){
    if (!cfg) return;
    if (cfg->announce_interval_us == 0) cfg->announce_interval_us = 3000000u;
    if (cfg->peer_timeout_us == 0)      cfg->peer_timeout_us = 3000000u * 4u; // 12 sec silence => drop
    if (cfg->gone_timeout_us == 0)      cfg->gone_timeout_us = 60000000u * 2u;   /* 2 min dropped -> GONE */
    if (cfg->max_peers == 0)            cfg->max_peers = 32u;
}

uint32_t dart_discovery_wire_size(uint16_t meta_cap){
    uint32_t cap = meta_cap ? meta_cap : DART_DISCOVERY_META_MAX;
    /* the wire blob = discovery section + overlay, so the scratch buffer must hold both */
    uint32_t w = (uint32_t)DART_DISCOVERY_META_OFF + DART_DISCOVERY_DISC_MAX + cap;
    return w < DART_DISCOVERY_WIRE_MAX ? DART_DISCOVERY_WIRE_MAX : w;
}

/* Single source of the discovery arena layout: state, the peer table, the meta pool.
   measure (bump.base NULL) feeds required_memory; build feeds init -- one definition. */
typedef struct { DartDiscoveryState *st; uint8_t *peers, *meta_pool, *user_pool; } i_DartDiscoveryBlocks;
static void i_dart_discovery_layout(i_DartBump *b, const DartDiscoveryCoreConfig *cfg, i_DartDiscoveryBlocks *o){
    uint16_t meta_cap = i_dart_discovery_meta_cap(cfg);
    uint16_t user_stride   = i_dart_discovery_user_stride(cfg);
    o->st        = (DartDiscoveryState*)i_dart_bump_take(b, sizeof(struct DartDiscoveryState), 8);
    o->peers     = (uint8_t*)i_dart_bump_take(b, (size_t)cfg->max_peers * sizeof(i_DartDiscoveryPeer), 8);
    /* hook mode allocates each peer's blob on demand at its actual size; only the
       no-hook (embedded) path reserves the worst-case max_peers x meta_cap pool */
    o->meta_pool = cfg->alloc ? NULL
                 : (uint8_t*)i_dart_bump_take(b, (size_t)cfg->max_peers * meta_cap, 1);
    o->user_pool = user_stride ? (uint8_t*)i_dart_bump_take(b, (size_t)cfg->max_peers * user_stride, 8) : NULL;
}

size_t dart_discovery_required_memory(const DartDiscoveryCoreConfig *cfg){
    i_DartBump b; i_DartDiscoveryBlocks blk;
    if (!cfg) return 0;
    memset(&b, 0, sizeof b);
    i_dart_discovery_layout(&b, cfg, &blk);
    return b.offset + 8u;     /* slack to align the caller's mem up to base */
}

DartDiscoveryState *dart_discovery_init(void *mem, size_t cap, const DartDiscoveryCoreConfig *cfg){
    i_DartBump b; i_DartDiscoveryBlocks blk; DartDiscoveryState *st; uint16_t i, meta_cap;
    if (!mem || !cfg || cfg->max_peers == 0) return NULL;
    if (cfg->announce_interval_us == 0 || cfg->peer_timeout_us == 0) return NULL;
    meta_cap = i_dart_discovery_meta_cap(cfg);
    if (cfg->meta.len > meta_cap) return NULL;
    if (cfg->meta.len && !cfg->meta.data) return NULL;
    if (cap < dart_discovery_required_memory(cfg)) return NULL;

    memset(&b, 0, sizeof b);
    b.base = (uint8_t*)(((uintptr_t)mem + 7u) & ~(uintptr_t)7u);
    b.cap  = cap - (size_t)(b.base - (uint8_t*)mem);
    i_dart_discovery_layout(&b, cfg, &blk);

    st = blk.st;
    memset(st, 0, sizeof(*st));
    st->cfg           = *cfg;
    st->cap_peers     = cfg->max_peers;
    st->meta_cap      = meta_cap;
    st->peers         = (i_DartDiscoveryPeer *)blk.peers;
    st->meta_pool     = blk.meta_pool;
    st->user_stride   = i_dart_discovery_user_stride(cfg);
    st->user_pool     = blk.user_pool;
    st->next_local_id = 1;
    st->started       = 0;
    memset(st->peers, 0, (size_t)st->cap_peers * sizeof(i_DartDiscoveryPeer));
    if (st->user_pool) memset(st->user_pool, 0, (size_t)st->cap_peers * st->user_stride);
    for (i=0;i<st->cap_peers;i++){
        st->peers[i].meta     = st->meta_pool ? st->meta_pool + (size_t)i * meta_cap : NULL;
        st->peers[i].meta_cap = st->meta_pool ? meta_cap : 0;
        st->peers[i].user = st->user_pool ? st->user_pool + (size_t)i * st->user_stride : NULL;
        st->peers[i].introduce_cursor = DART_DISCOVERY_INTRODUCE_IDLE;
    }
    st->self_meta         = cfg->meta;
    st->self_meta_version = 1;
    st->self_blob_resend  = DART_DISCOVERY_BLOB_RESEND;
    {   uint8_t nl = cfg->name.len > DART_DISCOVERY_NAME_MAX ? DART_DISCOVERY_NAME_MAX : (uint8_t)cfg->name.len;
        if (cfg->name.data && nl) memcpy(st->self_name, cfg->name.data, nl);
        st->self_name[nl] = '\0'; st->self_name_len = nl; }
    return st;
}


/* Relocate a live discovery core into a bigger block at grown counts. NOT a re-init:
 * the UUID, the monotonic blob version, the local-id counter and the peer table must
 * survive (a re-init would reset them and peers would treat us as a new node). self_meta
 * is an external pointer (our announce blob, in the node core); the caller passes its new
 * address. Each peer's meta is re-pointed into the new pool and its blob bytes copied. */
DartDiscoveryState *dart_discovery_core_migrate(DartDiscoveryState *old, void *new_mem,
        size_t new_cap, uint16_t new_max_peers, uint16_t new_meta_cap,
        const uint8_t *self_meta, void *peer_cb_user){
    i_DartBump b; i_DartDiscoveryBlocks blk; DartDiscoveryState *st; DartDiscoveryCoreConfig dc; uint16_t i, omp;
    if (!old) return NULL;
    dc = old->cfg; dc.max_peers = new_max_peers; dc.meta_cap = new_meta_cap;
    if (new_cap < dart_discovery_required_memory(&dc)) return NULL;
    memset(&b,0,sizeof b);
    b.base = (uint8_t*)(((uintptr_t)new_mem + 7u) & ~(uintptr_t)7u);
    b.cap  = new_cap - (size_t)(b.base - (uint8_t*)new_mem);
    i_dart_discovery_layout(&b, &dc, &blk);
    st = blk.st;
    *st = *old;                            /* cfg (uuid!), counters, version, started, cursors */
    st->cfg.max_peers     = new_max_peers;
    st->cfg.meta_cap = new_meta_cap;
    st->cfg.user          = peer_cb_user;  /* peer callbacks fire on the relocated node core */
    st->cap_peers         = new_max_peers;
    st->meta_cap     = new_meta_cap;
    st->peers             = (i_DartDiscoveryPeer*)blk.peers;
    st->meta_pool         = blk.meta_pool;
    st->user_pool         = blk.user_pool;     /* user_stride is unchanged (copied via *st = *old) */
    st->self_meta.data    = self_meta;     /* re-point our blob (len preserved via *st=*old); version NOT bumped */
    memset(st->peers, 0, (size_t)new_max_peers * sizeof(i_DartDiscoveryPeer));
    for (i=0;i<new_max_peers;i++){
        st->peers[i].meta     = st->meta_pool ? st->meta_pool + (size_t)i * new_meta_cap : NULL;
        st->peers[i].meta_cap = st->meta_pool ? new_meta_cap : 0;
        st->peers[i].user = st->user_pool ? st->user_pool + (size_t)i * st->user_stride : NULL;
        st->peers[i].introduce_cursor = DART_DISCOVERY_INTRODUCE_IDLE;
    }
    if (st->user_pool) memset(st->user_pool, 0, (size_t)new_max_peers * st->user_stride);
    omp = old->cap_peers;
    for (i=0;i<omp;i++){
        uint8_t *nmeta = st->peers[i].meta, *nuser = st->peers[i].user;
        uint16_t ncap  = st->peers[i].meta_cap;
        st->peers[i] = old->peers[i];      /* carries old meta/user ptrs + everything */
        if (st->meta_pool){                /* pool mode: re-point into the new pool + copy the
                                              bytes. Hook mode: the blob allocation is stable
                                              (outside the arena), so the struct copy carried it. */
            st->peers[i].meta = nmeta; st->peers[i].meta_cap = ncap;
            if (old->peers[i].meta_len) memcpy(nmeta, old->peers[i].meta, old->peers[i].meta_len);
        }
        st->peers[i].user = nuser;         /* re-point into the new user pool */
        if (st->user_stride && nuser && old->peers[i].user)
            memcpy(nuser, old->peers[i].user, st->user_stride);   /* preserve consumer scratch */
    }
    return st;
}

void dart_discovery_destroy(DartDiscoveryState *st){
    uint16_t i;
    if (!st || !st->cfg.alloc) return;   /* pool mode: nothing hook-allocated */
    for (i=0;i<st->cap_peers;i++)
        if (st->peers[i].meta){
            st->cfg.alloc(st->cfg.alloc_user, st->peers[i].meta, 0);
            st->peers[i].meta = NULL; st->peers[i].meta_cap = 0; st->peers[i].meta_len = 0;
        }
}

/* find by UUID, including DROPPED entries: a same-UUID return reuses the slot (and
 * thus the local_id), so the IO layer's transport state keyed by local_id resumes. */
static int i_dart_discovery_find(DartDiscoveryState *st, const uint8_t *uuid){
    uint16_t i;
    for (i=0;i<st->cap_peers;i++)
        if (st->peers[i].used && memcmp(st->peers[i].uuid, uuid, 16)==0) return (int)i;
    return -1;
}

/* Free a peer slot as GONE: fire the event while the slot STILL EXISTS, so the handler can
 * read its scratch (the IO layer frees the transport state from there), and only then drop
 * it. Every path that reclaims a slot outright goes through here; a DROP (the peer merely
 * fell silent) keeps the slot and is not this. */
static void i_dart_discovery_peer_gone(DartDiscoveryState *st, i_DartDiscoveryPeer *peer){
    i_dart_discovery_fire_down(st, peer->local_id, DART_DISCOVERY_GONE);
    peer->used = 0;
}

/* a slot for a brand-new peer: a FREE one, else the oldest DROPPED one (evicted,
 * fired GONE so its state is freed). ACTIVE peers are never evicted; -1 = refuse. */
static int i_dart_discovery_alloc(DartDiscoveryState *st){
    uint16_t i, victim = 0; uint64_t oldest = (uint64_t)-1; int found = -1;
    for (i=0;i<st->cap_peers;i++){
        if (!st->peers[i].used) return (int)i;
        if (st->peers[i].dropped && st->peers[i].last_heard_us <= oldest){
            oldest = st->peers[i].last_heard_us; victim = i; found = 1;
        }
    }
    if (found < 0) return -1;   /* table full of ACTIVE peers: caller refuses + signals */
    i_dart_discovery_peer_gone(st, &st->peers[victim]);
    return (int)victim;
}

/* A different uuid announcing from an (ip,port) we already hold means that endpoint's
 * process restarted: one socket is one process, so the old entry is provably dead.
 * Evict it as GONE (state freed) before adopting the newcomer, so its stale transport
 * state can't shadow the new incarnation whose data routes to the same address.
 * A 0 port is "locator not advertised yet" (a peer first seen via a blob-less announce),
 * not a real socket: distinct peers awaiting their blob share that non-endpoint, so it
 * must NOT trigger eviction (else several same-host instances churn, evicting each other).
 * A peer whose locator is not its endpoint identity (a relay-me origin: behind a NAT
 * every container on one device advertises the SAME device-ip locator) neither evicts by
 * locator nor is evicted by one; its identity is its observed source, handled below. */
static void i_dart_discovery_evict_endpoint(DartDiscoveryState *st, const DartDiscoveryAddr *addr){
    uint16_t i;
    if (!addr->ip_len || !addr->port) return;
    for (i=0;i<st->cap_peers;i++){
        i_DartDiscoveryPeer *peer = &st->peers[i];
        if (!peer->used) continue;
        if (peer->relay_me || peer->obs_disc.ip_len || peer->obs_data.ip_len) continue;
        if (peer->ip_len==addr->ip_len && peer->port==addr->port && memcmp(peer->ip, addr->ip, 16)==0)
            i_dart_discovery_peer_gone(st, peer);
    }
}

static int i_dart_discovery_addr_is(const DartDiscoveryAddr *a, const uint8_t *ip, uint8_t ip_len,
                                    uint16_t port){
    return a->ip_len == ip_len && a->port == port && memcmp(a->ip, ip, ip_len) == 0;
}

/* The observed-source twin of evict_endpoint: a NEW uuid whose direct announce arrives
 * from an endpoint we hold as some peer's OBSERVED source means that socket/NAT mapping
 * now belongs to a new process, so the old occupant is provably dead there. */
static void i_dart_discovery_evict_observed(DartDiscoveryState *st, const uint8_t *ip,
                                            uint8_t ip_len, uint16_t port){
    uint16_t i;
    if (!ip || !port) return;
    for (i=0;i<st->cap_peers;i++){
        i_DartDiscoveryPeer *peer = &st->peers[i];
        if (!peer->used) continue;
        if ((peer->obs_disc.ip_len && i_dart_discovery_addr_is(&peer->obs_disc, ip, ip_len, port)) ||
            (peer->obs_data.ip_len && i_dart_discovery_addr_is(&peer->obs_data, ip, ip_len, port)))
            i_dart_discovery_peer_gone(st, peer);
    }
}

/* build an announce/solicit/bye into p. with_blob includes the meta blob =
 * [discovery section: locator + name][opaque overlay]; every datagram carries the meta
 * version so a blob-less announce still signals change. The fixed header is the small part. */
static size_t i_dart_discovery_build(DartDiscoveryState *st, uint8_t flags, int with_blob,
                                   uint8_t *p, size_t cap){
    uint16_t meta_len = 0;
    if (cap < (size_t)DART_DISCOVERY_META_OFF) return 0;
    /* ask to be relayed: a node with no multicast of its own is announced onward by
       whoever hears it, so one seeded address makes it discoverable mesh-wide */
    if (st->cfg.relay_me && !(flags & DART_DISCOVERY_FLAG_BYE)) flags |= DART_DISCOVERY_FLAG_RELAY_ME;
    p[0]='u'; p[1]='D'; p[2]='S'; p[3]='C';
    p[4]=(uint8_t)DART_DISCOVERY_PROTO_VERSION;
    p[5]=flags;
    i_dart_le_w16(p+6, st->cfg.domain_id);
    memcpy(p+8, st->cfg.uuid, 16);
    if (with_blob){
        uint8_t *b = p + DART_DISCOVERY_META_OFF, *bend = p + cap;
        uint8_t ipl = (st->cfg.self_ip_len==4 || st->cfg.self_ip_len==16) ? st->cfg.self_ip_len : 0;
        size_t need = 2u + 1u + (size_t)ipl + 1u + st->self_name_len + st->self_meta.len;
        if ((size_t)(bend - b) < need) return 0;
        i_dart_le_w16(b, st->cfg.data_port); b += 2;            /* discovery section: locator */
        *b++ = ipl;
        if (ipl){ memcpy(b, st->cfg.self_ip, ipl); b += ipl; }
        *b++ = st->self_name_len;                             /* ... + name */
        if (st->self_name_len){ memcpy(b, st->self_name, st->self_name_len); b += st->self_name_len; }
        if (st->self_meta.len){ memcpy(b, st->self_meta.data, st->self_meta.len); b += st->self_meta.len; }  /* overlay */
        meta_len = (uint16_t)(b - (p + DART_DISCOVERY_META_OFF));
    }
    i_dart_le_w32(p+DART_DISCOVERY_HDR_LEN, st->self_meta_version);
    i_dart_le_w16(p+DART_DISCOVERY_HDR_LEN+4, meta_len);
    return (size_t)DART_DISCOVERY_META_OFF + meta_len;
}

/* Build a PROXIED announce on behalf of `peer`: its uuid, the blob version we hold for
 * it, and a blob whose discovery section states its locator OUTRIGHT (a forwarded
 * datagram would carry OUR source address, which tells nobody how to reach it) followed
 * by the overlay we hold verbatim, then OUR uuid as a 16-byte trailer AFTER the blob
 * (meta_len excludes it; parsers ignore trailing bytes). The header uuid stays the
 * ORIGIN's, so a receiver files it under the origin exactly as if heard directly; the
 * trailer says WHO relayed it, which is how a relay tells another relay's proxy from
 * its own multicast-looped echo (duplicate suppression must never count ourselves, or a
 * lone relay would suppress itself off its echoes). Returns 0 for a peer we cannot
 * usefully introduce: no locator yet, or no blob received so far. */
static size_t i_dart_discovery_build_proxy(DartDiscoveryState *st, const i_DartDiscoveryPeer *peer,
                                   uint8_t *p, size_t cap){
    uint8_t *b, *bend;
    size_t need;
    if (cap < (size_t)DART_DISCOVERY_META_OFF) return 0;
    if (!peer->port || (peer->ip_len != 4 && peer->ip_len != 16)) return 0;
    if (!peer->meta_version) return 0;
    p[0]='u'; p[1]='D'; p[2]='S'; p[3]='C';
    p[4]=(uint8_t)DART_DISCOVERY_PROTO_VERSION;
    /* PROXIED is the loop stop (a proxied announce can never enlist a second hop);
       RELAY_ME is carried as origin INFO, so a receiver knows the origin is unicast-only
       and its stated locator is not a usable endpoint identity (see evict_endpoint) */
    p[5]=(uint8_t)(DART_DISCOVERY_FLAG_PROXIED |
                   (peer->relay_me ? DART_DISCOVERY_FLAG_RELAY_ME : 0));
    i_dart_le_w16(p+6, st->cfg.domain_id);
    memcpy(p+8, peer->uuid, 16);
    b = p + DART_DISCOVERY_META_OFF; bend = p + cap;
    need = 2u + 1u + (size_t)peer->ip_len + 1u + peer->name_len + peer->meta_len + 16u;
    if ((size_t)(bend - b) < need) return 0;
    i_dart_le_w16(b, peer->port); b += 2;
    *b++ = peer->ip_len;
    memcpy(b, peer->ip, peer->ip_len); b += peer->ip_len;
    *b++ = peer->name_len;
    if (peer->name_len){ memcpy(b, peer->name, peer->name_len); b += peer->name_len; }
    if (peer->meta_len){ memcpy(b, peer->meta, peer->meta_len); b += peer->meta_len; }
    i_dart_le_w32(p+DART_DISCOVERY_HDR_LEN, peer->meta_version);   /* the ORIGIN's version */
    i_dart_le_w16(p+DART_DISCOVERY_HDR_LEN+4, (uint16_t)(b - (p + DART_DISCOVERY_META_OFF)));
    memcpy(b, st->cfg.uuid, 16); b += 16;    /* relayer trailer, after the blob */
    return (size_t)(b - p);
}

/* Rank one candidate IPv4 locator for a peer; higher wins. 2 = inside one of our own
 * subnets, so it is directly reachable from here. 1 = an ordinary address we would route
 * to. 0 = 169.254/16 link-local autoconfig (an adapter with no lease: usable only if we
 * happen to share that exact link, so it is the last thing to prefer). Non-IPv4 has
 * nothing to compare against and ranks with the ordinary case. */
static int i_dart_discovery_addr_rank(const DartDiscoveryState *st, const uint8_t *ip, uint8_t ip_len){
    uint8_t i;
    if (ip_len != 4) return 1;
    /* first, because a link-local adapter's own /16 would otherwise make every other
       169.254 address on earth look like it shares our segment */
    if (ip[0] == 169 && ip[1] == 254) return 0;
    for (i = 0; i < st->n_local_nets; i++){
        const DartDiscoverySubnet *net = &st->local_nets[i];
        int k, same = 1;
        for (k = 0; k < 4; k++) if (((ip[k] ^ net->ip[k]) & net->mask[k]) != 0){ same = 0; break; }
        if (same) return 2;
    }
    return 1;
}

static void i_dart_discovery_addr_of(const i_DartDiscoveryPeer *peer, DartDiscoveryAddr *out){
    memset(out, 0, sizeof *out);
    memcpy(out->ip, peer->ip, 16);
    out->ip_len = peer->ip_len;
    out->port   = peer->port;
}

/* Discovery TX destination for a peer: an observed source when one is bound (2: send to
 * exactly that endpoint), else the advertised locator (1: the IO layer expands it to the
 * discovery + data ports). The discovery channel's source first (our discovery-socket
 * sends pass a NAT's endpoint filter there), then the data channel's (on an unfiltered
 * path it reaches the same socket, and it beats a locator that is fiction). */
static int i_dart_discovery_disc_dest(const i_DartDiscoveryPeer *peer, DartDiscoveryAddr *out){
    if (peer->obs_disc.ip_len){ *out = peer->obs_disc; return 2; }
    if (peer->obs_data.ip_len){ *out = peer->obs_data; return 2; }
    i_dart_discovery_addr_of(peer, out);
    return 1;
}

void dart_discovery_on_datagram(DartDiscoveryState *st, const DartDiscoveryAddr *src,
                       DartDiscoveryVia via, DartBytes datagram, uint64_t now){
    const uint8_t *p = datagram.data; size_t len = datagram.len;
    const uint8_t *src_ip = (src && (src->ip_len==4 || src->ip_len==16)) ? src->ip : NULL;
    uint8_t  src_ip_len = src_ip ? src->ip_len : 0;
    uint16_t src_port   = src_ip ? src->port : 0;
    uint8_t flags; uint16_t meta_len; uint32_t meta_version;
    const uint8_t *uuid, *blob;
    DartDiscoveryAddr addr; int idx, addr_changed, first_contact=0, blob_changed=0, revived=0;
    int keep_held;  /* unicast-only receiver: hold the peer's known locator against the
                       arrival source (our NAT rewrites sources; see the addr comment) */
    int proxied;   /* a relay rebuilt this for its origin: locator is second-hand (ranked, not
                      authoritative) and it can never enlist us to relay in turn */
    const uint8_t *relayer = NULL;   /* proxy trailer: WHO relayed it (NULL = old build) */
    i_DartDiscoveryPeer *peer;
    /* the blob's discovery section (valid only when have_disc): locator + name, then overlay.
       disc_name points into the datagram (NOT NUL-terminated): a non-NUL wire string. */
    int have_disc=0; uint16_t disc_port=0; uint8_t disc_ip_len=0;
    const uint8_t *disc_ip=NULL; DartBytes overlay = {NULL, 0}; DartString disc_name = {NULL, 0};

    if (len < (size_t)DART_DISCOVERY_META_OFF) return;
    if (p[0]!='u'||p[1]!='D'||p[2]!='S'||p[3]!='C') return;
    if (p[4]!=(uint8_t)DART_DISCOVERY_PROTO_VERSION) return;
    if (i_dart_le_r16(p+6) != st->cfg.domain_id) return;
    uuid = p+8;
    if (memcmp(uuid, st->cfg.uuid, 16)==0) return;  /* ignore self */
    meta_version = i_dart_le_r32(p+DART_DISCOVERY_HDR_LEN);
    meta_len = i_dart_le_r16(p+DART_DISCOVERY_HDR_LEN+4);
    if ((size_t)DART_DISCOVERY_META_OFF + meta_len > len){
        /* the OS truncated the datagram to our RX buffer: the fixed header (always
           intact) says the blob is meta_len bytes, so this peer's metadata exceeds what
           this side can currently receive. Same never-silent signal as the capacity
           refuse below, with .meta = {NULL, needed bytes}: an IO layer that can grow
           does so and re-solicits; one that cannot surfaces it. */
        DartDiscoveryAddr a; int at = i_dart_discovery_find(st, uuid);
        memset(&a, 0, sizeof a);
        if (src_ip && (src_ip_len==4 || src_ip_len==16)){ memcpy(a.ip, src_ip, src_ip_len); a.ip_len = src_ip_len; }
        i_dart_discovery_fire_meta_too_big(st, at >= 0 ? st->peers[at].local_id : 0, &a,
                                           dart_bytes(NULL, meta_len));
        return;
    }
    blob = p + DART_DISCOVERY_META_OFF;
    flags = p[5];
    proxied = (flags & DART_DISCOVERY_FLAG_PROXIED) != 0;
    if (proxied && len >= (size_t)DART_DISCOVERY_META_OFF + meta_len + 16u)
        relayer = p + DART_DISCOVERY_META_OFF + meta_len;   /* build_proxy's relayer trailer */

    /* parse the blob's discovery section (locator + name); the remainder is the opaque
       overlay we hand up. Fully bounds-checked: a malformed blob drops the datagram. */
    if (meta_len){
        const uint8_t *b = blob, *bend = blob + meta_len;
        if (b + 3 > bend) return;                          /* port(2) + ip_len(1) */
        disc_port = i_dart_le_r16(b); b += 2;
        disc_ip_len = *b++;
        if (disc_ip_len==4 || disc_ip_len==16){ if (b + disc_ip_len > bend) return; disc_ip = b; b += disc_ip_len; }
        else disc_ip_len = 0;
        if (b + 1 > bend) return;                          /* name_len(1) */
        {   uint8_t dnl = *b++;
            if (dnl){ if (b + dnl > bend) return; disc_name = dart_string((const char*)b, dnl); b += dnl; } }
        overlay = dart_bytes(b, (size_t)(bend - b));
        if (overlay.len > st->meta_cap){   /* overlay must fit the per-peer buffer: this side
                                                   can NEVER hold that peer's metadata, so say so */
            DartDiscoveryAddr a; int at = i_dart_discovery_find(st, uuid);
            memset(&a, 0, sizeof a);
            if (disc_ip_len){ memcpy(a.ip, disc_ip, disc_ip_len); a.ip_len = disc_ip_len; }
            a.port = disc_port;
            i_dart_discovery_fire_meta_too_big(st, at >= 0 ? st->peers[at].local_id : 0, &a, overlay);
            return;
        }
        have_disc = 1;
    }

    idx = i_dart_discovery_find(st, uuid);

    if (flags & DART_DISCOVERY_FLAG_BYE){
        if (idx >= 0) i_dart_discovery_peer_gone(st, &st->peers[idx]);   /* a graceful exit is GONE */
        return;
    }

    /* Address: the port comes from the blob's locator (cached between blob updates, since a
       steady-state announce carries none). The IP is a CANDIDATE path -- the source this
       datagram actually arrived from, or an explicit self_ip where the peer states one --
       and every announce offers one, so a better path than the one we first happened to
       hear can still win the ranking below. Falls back to the cached locator only when the
       datagram carries no usable source.

       EXCEPT on a unicast-only node (cfg.relay_me): its own NAT rewrites arrival sources
       (a slirp return flow keeps the real address, but a PUBLISHED-port forward stamps the
       NAT's internal gateway), and a gateway source sits on the container's OWN subnet, so
       it would even WIN the ranking against the peer's true routed address and every send
       to the peer would go to the gateway instead -- permanently, since each forwarded
       arrival refreshes the wrong incumbent. So there a source never overrides a held
       locator (an introduction's embedded address, or an earlier stated one); sources are
       still bound below as observed RETURN PATHS, which is what they really are. */
    keep_held = st->cfg.relay_me && !proxied && !disc_ip_len && idx >= 0
                && (st->peers[idx].ip_len == 4 || st->peers[idx].ip_len == 16);
    memset(&addr, 0, sizeof addr);
    if (have_disc){
        if (disc_ip_len){ addr.ip_len = disc_ip_len; memcpy(addr.ip, disc_ip, disc_ip_len); }
        else if (keep_held){ addr.ip_len = st->peers[idx].ip_len; memcpy(addr.ip, st->peers[idx].ip, 16); }
        else if (src_ip && (src_ip_len==4 || src_ip_len==16)){ addr.ip_len = src_ip_len; memcpy(addr.ip, src_ip, src_ip_len); }
        else return;
        addr.port = disc_port;
    } else if (idx >= 0){
        addr.port = st->peers[idx].port;
        if (!keep_held && src_ip && (src_ip_len==4 || src_ip_len==16)){ addr.ip_len = src_ip_len; memcpy(addr.ip, src_ip, src_ip_len); }
        else { addr.ip_len = st->peers[idx].ip_len; memcpy(addr.ip, st->peers[idx].ip, 16); }
    } else if (src_ip && (src_ip_len==4 || src_ip_len==16)){
        addr.ip_len = src_ip_len; memcpy(addr.ip, src_ip, src_ip_len);   /* port unknown until the blob */
    } else return;

    if (idx < 0){
        uint8_t *keep_meta, *keep_user; uint16_t keep_cap;
        /* new uuid from an address we already hold => the endpoint's process restarted;
           evict the dead predecessor (frees its slot for reuse) so it can't shadow us.
           A RELAY_ME announce's locator asserts no endpoint (a NAT'd origin's is fiction,
           and identical across containers on one device), so it evicts nobody; its SOURCE
           is the real endpoint, and reuse of a source we hold as some peer's observed
           address is the same restart proof. */
        if (!(flags & DART_DISCOVERY_FLAG_RELAY_ME))
            i_dart_discovery_evict_endpoint(st, &addr);
        if (!proxied && src_ip)
            i_dart_discovery_evict_observed(st, src_ip, src_ip_len, src_port);
        idx = i_dart_discovery_alloc(st);
        if (idx < 0){       /* table full of active peers: refuse, never evict a live one */
            i_dart_discovery_fire_refused(st, &addr);
            return;
        }
        keep_meta = st->peers[idx].meta;            /* preserve the blob buffer across reset: the
                                                       pool slot, or the previous occupant's grown
                                                       hook allocation (reused, not freed) */
        keep_cap  = st->peers[idx].meta_cap;
        keep_user = st->peers[idx].user;
        memset(&st->peers[idx], 0, sizeof(i_DartDiscoveryPeer));
        st->peers[idx].meta     = keep_meta;
        st->peers[idx].meta_cap = keep_cap;
        st->peers[idx].user     = keep_user;
        if (keep_user) memset(keep_user, 0, st->user_stride);   /* fresh consumer scratch for the new peer */
        st->peers[idx].used     = 1;
        memcpy(st->peers[idx].uuid, uuid, 16);
        st->peers[idx].local_id = st->next_local_id++;
        st->peers[idx].ip_len   = 0xFF;   /* force first peer_up */
        st->peers[idx].introduce_cursor = DART_DISCOVERY_INTRODUCE_IDLE;
        first_contact = 1;
    }
    peer = &st->peers[idx];
    if (peer->dropped){ peer->dropped = 0; revived = 1; }   /* a DROPPED peer returned: resume it */
    peer->last_heard_us = now;
    if (!proxied) peer->last_direct_us = now;   /* only a datagram from the peer ITSELF is
                                                   direct liveness (relay/introduce gate) */
    else if (!(relayer && memcmp(relayer, st->cfg.uuid, 16)==0) && peer->proxies_heard != 0xFFu)
        peer->proxies_heard++;   /* a FOREIGN relay's proxy (a trailer-less old build counts
                                    too); our own looped-back echo must NOT, or a lone relay
                                    would suppress itself */

    /* Which address do we keep for a peer we can hear on more than one path? It announces
       out every interface it has, so each copy reaches us with a different source address,
       and adopting the newest every time would flap the locator its data is unicast to (and
       re-fire peer_up, re-applying its whole interest) on every announce. Prefer the better
       ranked address, so a peer first heard over some link-local or VPN adapter moves to a
       shared-subnet path as soon as one announces; at equal rank stay with the incumbent
       while we are still hearing the peer there, so a genuine renumber (the old address
       goes quiet) is picked up a couple of announces later instead. A locator the peer
       states outright (self_ip) is authoritative and skips all of this -- but a PROXIED
       one does not: that address is only where a RELAY sees the origin, so it ranks like
       any other candidate. Otherwise the relay's view would stomp a direct path we share
       a subnet with on every proxy interval, flapping the locator (and re-firing peer_up,
       re-applying the peer's whole interest) forever. */
    if ((!disc_ip_len || proxied) && peer->ip_len == 4 && addr.ip_len == 4 && memcmp(peer->ip, addr.ip, 4) != 0){
        int rank_new = i_dart_discovery_addr_rank(st, addr.ip, 4);
        int rank_old = i_dart_discovery_addr_rank(st, peer->ip, 4);
        if (rank_new < rank_old ||
            (rank_new == rank_old &&
             now - peer->addr_heard_us < (uint64_t)st->cfg.announce_interval_us * 2u))
            memcpy(addr.ip, peer->ip, 4);          /* keep the incumbent */
    }

    addr_changed = (peer->ip_len != addr.ip_len)
                || (peer->port   != addr.port)
                || (memcmp(peer->ip, addr.ip, 16) != 0);
    if (addr_changed){
        peer->ip_len = addr.ip_len; peer->port = addr.port;
        memcpy(peer->ip, addr.ip, 16);
    }
    /* freshness of the locator we hold: only a datagram that really arrived FROM it counts
       (a steady-state announce carries no blob, so addr is then the cached locator, not
       where this datagram came from) */
    if (peer->ip_len == 4 && src_ip && src_ip_len == 4 && memcmp(peer->ip, src_ip, 4) == 0)
        peer->addr_heard_us = now;

    /* meta: a newer version with the blob present updates our stored overlay + name; a newer
       version without the blob (a steady-state version-only announce) means we fell behind,
       so re-fetch via a targeted solicit. adv_version tracks the highest version the peer has
       claimed (blob present or not); > meta_version means our held blob is stale. */
    if (meta_version > peer->adv_version) peer->adv_version = meta_version;
    if (have_disc){
        if (meta_version > peer->meta_version){
            uint8_t nl = disc_name.len > DART_DISCOVERY_NAME_MAX ? DART_DISCOVERY_NAME_MAX : (uint8_t)disc_name.len;
            int fits = 1;
            if (overlay.len > peer->meta_cap){   /* hook mode: (re)size to the actual blob;
                                                    pool-mode slots are pre-sized (checked above) */
                uint8_t *nb = st->cfg.alloc ? (uint8_t*)st->cfg.alloc(st->cfg.alloc_user, peer->meta, overlay.len)
                                            : NULL;
                if (nb){ peer->meta = nb; peer->meta_cap = (uint16_t)overlay.len; }
                else fits = 0;                   /* OOM: keep the stale blob + version; adv_version
                                                    stays ahead, so the re-fetch path retries */
            }
            if (fits){
                if (overlay.len) memcpy(peer->meta, overlay.data, overlay.len);
                peer->meta_len = (uint16_t)overlay.len; peer->meta_version = meta_version;
                if (disc_name.data && nl) memcpy(peer->name, disc_name.data, nl);
                peer->name[nl] = '\0'; peer->name_len = nl;
                /* a self_ip the peer ITSELF states is authoritative: observed sources
                   never bind over it (the operator asserted the reachable address). Only
                   a DIRECT announce can say so: a PROXY always embeds a locator (the
                   relay's view, since a forwarded source would be the relay's), which
                   asserts nothing and must not lock observed binding out. */
                if (!proxied){
                    peer->stated_ip = disc_ip_len ? 1u : 0u;
                    if (peer->stated_ip){
                        memset(&peer->obs_disc, 0, sizeof peer->obs_disc);
                        memset(&peer->obs_data, 0, sizeof peer->obs_data);
                    }
                }
                blob_changed = 1;
            }
        }
        peer->solicit_due = 0;
    } else if (meta_version > peer->meta_version){
        peer->solicit_due = 1;
    }

    /* Relaying is enlisted by DIRECT announces only: a proxied one is already a relay's
       work, so it can never recruit a second hop (the loop stop). Introduce a newly heard
       or changed relay-me peer at once rather than at our next announce, so a
       unicast-only node joins the mesh about a round trip after it seeds anyone. */
    if (!proxied){
        peer->heard_direct = 1;
        peer->wants_relay = (flags & DART_DISCOVERY_FLAG_RELAY_ME) ? 1u : 0u;
        peer->relay_me    = peer->wants_relay;   /* its own word beats any proxy's */
        if (!peer->relay_me && !st->cfg.relay_me){   /* an ordinary peer again: its locator is
                       its endpoint. On a unicast-only node the observed return paths stay
                       (rebound below): there every peer is reached through our own NAT. */
            memset(&peer->obs_disc, 0, sizeof peer->obs_disc);
            memset(&peer->obs_data, 0, sizeof peer->obs_data);
        }
        if (peer->wants_relay && (first_contact || blob_changed || revived || addr_changed))
            peer->relay_due = 1;
    } else if (!peer->heard_direct && (flags & DART_DISCOVERY_FLAG_RELAY_ME))
        peer->relay_me = 1;   /* known only second-hand: the proxy's origin info stands */

    /* Observed source: a unicast-only peer sends everything from its data socket, and a
       NAT between us may rewrite that source per flow, making the advertised locator a
       fiction. Its direct announces carry both its uuid and the real return path, so bind
       (or quietly REBIND: a recycled NAT mapping must redirect sends, not flap peer state)
       the source per local arrival channel. Suppressed by a stated self_ip (authoritative)
       and never from a proxy (that source is the relay's).
       A unicast-only node (cfg.relay_me) binds observed sources for EVERY direct peer,
       not just relay-me ones: everything reaching it came through its own NAT (a
       published-port forward rewrites the source to the NAT's internal gateway), so the
       arrival source is the only endpoint its replies are known to reach -- the forward
       relays them back to the peer's real socket -- while the peer's locator may not be
       routable from inside at all. */
    if (!proxied && ((flags & DART_DISCOVERY_FLAG_RELAY_ME) || st->cfg.relay_me)
        && !peer->stated_ip && src_ip && src_port){
        DartDiscoveryAddr *obs = (via == DART_DISCOVERY_VIA_DATA) ? &peer->obs_data
                                                                  : &peer->obs_disc;
        memset(obs, 0, sizeof *obs);
        memcpy(obs->ip, src_ip, src_ip_len);
        obs->ip_len = src_ip_len; obs->port = src_port;
    }

    /* Introductions (the inbound half of relaying): a relay-me peer cannot hear the
       mesh's multicast, and behind a NAT nobody can dial into it either, so it must be
       TOLD who exists and reach out itself. Change-triggered: a newly heard relay-me peer
       is walked the whole table once, and any directly heard peer that appears, resumes
       or moves is re-walked to every relay-me peer. Receivers dedup by uuid + version. */
    if (!proxied && (first_contact || revived) && peer->wants_relay)
        peer->introduce_cursor = 0;
    if (!proxied && (first_contact || revived || addr_changed)){
        uint16_t k;
        for (k=0;k<st->cap_peers;k++){
            i_DartDiscoveryPeer *rme = &st->peers[k];
            if (rme == peer || !rme->used || rme->dropped || !rme->wants_relay) continue;
            rme->introduce_cursor = 0;
        }
    }

    if (first_contact || addr_changed || blob_changed || revived)
        i_dart_discovery_fire_up(st, peer, &addr);

    if ((flags & DART_DISCOVERY_FLAG_REQ) && st->started)
        peer->reply_due = 1;   /* answer the solicit with a unicast announce + blob */
}

size_t dart_discovery_update(DartDiscoveryState *st, uint64_t now, void *out, size_t cap){
    uint16_t i;
    if (!st->started){
        st->started = 1;
        st->next_announce_us = now + (i_dart_discovery_fnv(st->cfg.uuid,16) % st->cfg.announce_interval_us);
        st->want_solicit = 1;   /* solicit on startup */
    }
    for (i=0;i<st->cap_peers;i++){
        if (!st->peers[i].used) continue;
        if (st->peers[i].dropped){
            /* dropped and still silent past the gone timeout: a same-UUID return is no longer
               expected, so promote to GONE -- free the transport state and reclaim the slot.
               0 = never promote. */
            if (st->cfg.gone_timeout_us &&
                now - st->peers[i].last_heard_us > (uint64_t)st->cfg.peer_timeout_us + st->cfg.gone_timeout_us)
                i_dart_discovery_peer_gone(st, &st->peers[i]);
            continue;
        }
        if (st->peers[i].heard_direct &&
            now - st->peers[i].last_direct_us > st->cfg.peer_timeout_us)
            st->peers[i].heard_direct = 0;   /* its direct path fell silent: relay proxies may
                                                still refresh last_heard_us, but it is no longer
                                                ours to introduce (poll_introduce's origin gate) */
        if (now - st->peers[i].last_heard_us > st->cfg.peer_timeout_us){
            /* fell silent: DEMOTE (keep the entry + local_id) so a same-UUID return
               resumes; the IO layer keeps its transport state on a DROP reason */
            st->peers[i].dropped = 1;
            st->peers[i].heard_direct = 0;   /* a proxy-only return is not ours to introduce */
            i_dart_discovery_fire_down(st, st->peers[i].local_id, DART_DISCOVERY_DROP);
        }
    }
    if (st->want_solicit){   /* multicast solicit: announce us (with blob) AND ask peers to reply */
        st->want_solicit = 0;
        return i_dart_discovery_build(st, DART_DISCOVERY_FLAG_REQ, 1, (uint8_t *)out, cap);
    }
    if (now >= st->next_announce_us){
        int with_blob = st->self_blob_resend > 0;
        uint16_t k;
        if (with_blob) st->self_blob_resend--;
        st->next_announce_us = now + st->cfg.announce_interval_us;
        /* keep every relay-me peer present on the paths it cannot reach itself: one
           proxied announce per peer per announce interval, drained by poll_relay. Two
           gates. DIRECT liveness, never last_heard_us: proxies refresh last_heard_us
           and every relay hears the other relays' proxies (and its own multicast-looped
           echoes, which carry the ORIGIN's uuid and so pass the self-filter), so a
           last_heard gate would relay a dead origin forever; on direct silence every
           relay stops within one peer_timeout, the proxies cease, and third parties
           time the peer out one timeout later. And DUPLICATE SUPPRESSION: 2+ foreign
           proxies for the origin heard since our last tick mean the mesh already has a
           relay quorum, so we sit this interval out (the counter resets every tick, so
           when the active relays die the silence re-enlists us at the next one). The
           introduction sweep rides the same cadence, much slower: a re-walk every SWEEP
           intervals repairs lost introductions (change triggers do the real work). */
        for (k=0;k<st->cap_peers;k++){
            i_DartDiscoveryPeer *rp = &st->peers[k];
            uint8_t heard = rp->proxies_heard;
            rp->proxies_heard = 0;
            if (rp->used && !rp->dropped && rp->wants_relay &&
                now - rp->last_direct_us <= st->cfg.peer_timeout_us){
                if (heard < 2u) rp->relay_due = 1;
                if (now >= rp->introduce_sweep_us){
                    rp->introduce_cursor   = 0;
                    rp->introduce_sweep_us = now +
                        (uint64_t)st->cfg.announce_interval_us * DART_DISCOVERY_INTRODUCE_SWEEP;
                }
            }
        }
        return i_dart_discovery_build(st, 0, with_blob, (uint8_t *)out, cap);
    }
    return 0;
}

uint64_t dart_discovery_next_due_us(const DartDiscoveryState *st){
    if (!st || !st->started || st->want_solicit) return 0;
    return st->next_announce_us;   /* 0 after an advertise = announce the change now */
}

void dart_discovery_set_meta(DartDiscoveryState *st, DartBytes meta){
    if (!st || meta.len > st->meta_cap) return;   /* the node sizes meta_cap to fit */
    st->self_meta         = meta;
    st->self_meta_version++;
    st->self_blob_resend  = DART_DISCOVERY_BLOB_RESEND;
    st->next_announce_us  = 0;   /* announce the change now, don't wait for the timer */
}

uint32_t dart_discovery_meta_version(const DartDiscoveryState *st){
    return st ? st->self_meta_version : 0;
}

void dart_discovery_set_data_port(DartDiscoveryState *st, uint16_t port){
    if (!st || st->cfg.data_port == port) return;
    st->cfg.data_port = port;            /* the port now rides the blob's discovery section, so */
    st->self_meta_version++;             /* bump the version + re-send so peers re-fetch it */
    st->self_blob_resend = DART_DISCOVERY_BLOB_RESEND;
    st->next_announce_us = 0;
}

void dart_discovery_set_local_subnets(DartDiscoveryState *st, const DartDiscoverySubnet *nets, uint8_t n){
    uint8_t i, k = 0;
    if (!st) return;
    if (n > DART_DISCOVERY_MAX_SUBNETS) n = DART_DISCOVERY_MAX_SUBNETS;
    for (i = 0; i < n; i++){
        const uint8_t *m = nets[i].mask;
        if (!(m[0] | m[1] | m[2] | m[3])) continue;   /* unknown prefix: it would match everything */
        st->local_nets[k++] = nets[i];
    }
    st->n_local_nets = k;
}

size_t dart_discovery_poll_targeted(DartDiscoveryState *st, void *out, size_t cap,
                                    DartDiscoveryAddr *to, int *exact){
    uint16_t n = st->cap_peers, k;
    if (n == 0) return 0;
    for (k=0;k<n;k++){
        uint16_t i = st->targeted_cursor;
        i_DartDiscoveryPeer *peer = &st->peers[i];
        st->targeted_cursor = (uint16_t)((i+1u) % n);
        if (!peer->used){ peer->reply_due = peer->solicit_due = 0; continue; }
        if (peer->reply_due){                 /* reply to a soliciter: announce + blob, unicast */
            peer->reply_due = 0;
            if (exact) *exact = i_dart_discovery_disc_dest(peer, to) == 2;
            else       i_dart_discovery_disc_dest(peer, to);
            return i_dart_discovery_build(st, 0, 1, (uint8_t *)out, cap);
        }
        if (peer->solicit_due){               /* re-fetch: ask this peer to announce back to us */
            peer->solicit_due = 0;
            if (exact) *exact = i_dart_discovery_disc_dest(peer, to) == 2;
            else       i_dart_discovery_disc_dest(peer, to);
            return i_dart_discovery_build(st, DART_DISCOVERY_FLAG_REQ, 1, (uint8_t *)out, cap);
        }
    }
    return 0;
}

size_t dart_discovery_poll_introduce(DartDiscoveryState *st, void *out, size_t cap,
                                     DartDiscoveryAddr *to, int *exact){
    uint16_t i;
    if (!st) return 0;
    for (i=0;i<st->cap_peers;i++){
        i_DartDiscoveryPeer *peer = &st->peers[i];
        if (!peer->used || peer->dropped || !peer->wants_relay) continue;
        if (peer->introduce_cursor == (uint16_t)DART_DISCOVERY_INTRODUCE_IDLE) continue;
        while (peer->introduce_cursor < st->cap_peers){
            i_DartDiscoveryPeer *origin = &st->peers[peer->introduce_cursor++];
            size_t n_bytes;
            /* only peers we hear DIRECTLY are ours to introduce (the one-hop rule:
               a second-hand peer already has its own introducer) */
            if (origin == peer || !origin->used || origin->dropped || !origin->heard_direct)
                continue;
            n_bytes = i_dart_discovery_build_proxy(st, origin, (uint8_t *)out, cap);
            if (!n_bytes) continue;           /* nothing useful to say about it yet */
            if (exact) *exact = i_dart_discovery_disc_dest(peer, to) == 2;
            else       i_dart_discovery_disc_dest(peer, to);
            return n_bytes;
        }
        peer->introduce_cursor = (uint16_t)DART_DISCOVERY_INTRODUCE_IDLE;
    }
    return 0;
}

size_t dart_discovery_poll_relay(DartDiscoveryState *st, void *out, size_t cap){
    uint16_t n, k;
    if (!st) return 0;
    n = st->cap_peers;
    if (n == 0) return 0;
    for (k=0;k<n;k++){
        uint16_t i = st->relay_cursor;
        i_DartDiscoveryPeer *peer = &st->peers[i];
        st->relay_cursor = (uint16_t)((i+1u) % n);
        /* a dropped or freed peer is not introduced to anyone: the relay's silence is
           how third parties learn it went away (they time it out one timeout later) */
        if (!peer->used || peer->dropped || !peer->wants_relay){ peer->relay_due = 0; continue; }
        if (!peer->relay_due) continue;
        peer->relay_due = 0;
        {   size_t n_bytes = i_dart_discovery_build_proxy(st, peer, (uint8_t *)out, cap);
            if (n_bytes) return n_bytes; }   /* 0 = nothing useful to say yet; try the next peer */
    }
    return 0;
}

/* queue a one-shot multicast solicit: the next update emits a REQ asking peers to announce now */
void dart_discovery_solicit(DartDiscoveryState *st){ if (st) st->want_solicit = 1; }

/* re-deliver every live peer's last-known announce to on_peer_up, so a caller that just
 * changed its own advertised data re-applies all peer interest against the new state. No
 * version change is involved: a peer's blob is unchanged, but the LOCAL side may now have
 * a topic that the blob's interest matches. */
void dart_discovery_replay_peers(DartDiscoveryState *st){
    uint16_t i;
    if (!st || !st->cfg.on_event) return;
    for (i=0;i<st->cap_peers;i++){
        i_DartDiscoveryPeer *peer = &st->peers[i];
        DartDiscoveryAddr addr;
        if (!peer->used || peer->dropped) continue;
        i_dart_discovery_addr_of(peer, &addr);
        i_dart_discovery_fire_up(st, peer, &addr);
    }
}

uint16_t dart_discovery_peer_count(const DartDiscoveryState *st){
    uint16_t i, c = 0;   /* live peers only; DROPPED entries linger for resume, not as members */
    for (i=0;i<st->cap_peers;i++) if (st->peers[i].used && !st->peers[i].dropped) c++;
    return c;
}

uint16_t dart_discovery_max_peers(const DartDiscoveryState *st){ return st->cap_peers; }

int dart_discovery_peer_at(const DartDiscoveryState *st, uint16_t slot, DartDiscoveryPeer *out){
    const i_DartDiscoveryPeer *p;
    if (slot >= st->cap_peers) return 0;
    p = &st->peers[slot];
    if (!p->used) return 0;                  /* free slot: DROPPED entries are still "used" */
    memset(out, 0, sizeof *out);
    out->id = p->local_id;
    memcpy(out->uuid, p->uuid, 16);
    i_dart_discovery_addr_of(p, &out->addr);
    out->liveness      = p->dropped ? DART_PEER_DROPPED : DART_PEER_ACTIVE;
    out->last_heard_us = p->last_heard_us;
    out->name          = dart_string(p->name_len ? p->name : NULL, p->name_len);   /* parsed from the blob */
    out->meta          = dart_bytes(p->meta_len ? p->meta : NULL, p->meta_len);
    out->meta_version  = p->meta_version;
    out->adv_meta_version = p->adv_version;
    out->user          = st->user_stride ? p->user : NULL;
    return 1;
}

/* find a used peer (incl. DROPPED) by its local id; NULL if none. */
static i_DartDiscoveryPeer *i_dart_discovery_by_id(const DartDiscoveryState *st, uint32_t id){
    uint16_t i;
    for (i=0;i<st->cap_peers;i++)
        if (st->peers[i].used && st->peers[i].local_id == id) return (i_DartDiscoveryPeer*)&st->peers[i];
    return NULL;
}

void *dart_discovery_peer_user(DartDiscoveryState *st, uint32_t id){
    i_DartDiscoveryPeer *p;
    if (!st || !st->user_stride) return NULL;
    p = i_dart_discovery_by_id(st, id);
    return p ? p->user : NULL;
}

int dart_discovery_addr_of_id(const DartDiscoveryState *st, uint32_t id, DartDiscoveryAddr *out){
    i_DartDiscoveryPeer *p = i_dart_discovery_by_id(st, id);
    if (!p) return 0;
    /* the address that reaches the peer's DATA socket: the observed source of its
       data-channel announces when bound (a translated peer: our data-socket sends pass
       the NAT's per-flow filter exactly there), else the advertised locator */
    if      (p->obs_data.ip_len) *out = p->obs_data;
    else if (p->obs_disc.ip_len) *out = p->obs_disc;
    else i_dart_discovery_addr_of(p, out);
    return 1;
}

DartBytes dart_discovery_peer_meta(const DartDiscoveryState *st, uint32_t id, uint32_t *version){
    i_DartDiscoveryPeer *p = i_dart_discovery_by_id(st, id);
    if (version) *version = p ? p->meta_version : 0;
    if (!p || !p->meta_len) return dart_bytes(NULL, 0);
    return dart_bytes(p->meta, p->meta_len);
}

DartString dart_discovery_peer_name(const DartDiscoveryState *st, uint32_t id){
    i_DartDiscoveryPeer *p = i_dart_discovery_by_id(st, id);
    if (!p) return dart_string(NULL, 0);                 /* unknown peer: .data NULL */
    return dart_string(p->name, p->name_len);            /* known: .data non-NULL, .len may be 0 */
}

int dart_discovery_id_for_addr(const DartDiscoveryState *st, const uint8_t *ip, uint8_t ip_len,
                               uint16_t port, uint32_t *id){
    uint16_t i;
    if (!ip || (ip_len != 4 && ip_len != 16)) return 0;
    for (i=0;i<st->cap_peers;i++){
        const i_DartDiscoveryPeer *p = &st->peers[i];
        int hit;
        if (!p->used) continue;
        /* a peer with observed sources matches ONLY those: its advertised locator is not
           its endpoint (and may equal another peer's, e.g. two NAT'd containers on one
           device), so it must never attract that address's traffic */
        if (p->obs_disc.ip_len || p->obs_data.ip_len)
            hit = (p->obs_disc.ip_len && i_dart_discovery_addr_is(&p->obs_disc, ip, ip_len, port)) ||
                  (p->obs_data.ip_len && i_dart_discovery_addr_is(&p->obs_data, ip, ip_len, port));
        else
            hit = p->ip_len == ip_len && p->port == port && memcmp(p->ip, ip, ip_len) == 0;
        if (hit){
            if (id) *id = p->local_id;
            return 1;
        }
    }
    return 0;
}

size_t dart_discovery_leave(DartDiscoveryState *st, void *out, size_t cap){
    return i_dart_discovery_build(st, DART_DISCOVERY_FLAG_BYE, 0, (uint8_t *)out, cap);
}

int dart_discovery_peer_addr(const DartDiscoveryState *st, uint16_t slot, DartDiscoveryAddr *out){
    const i_DartDiscoveryPeer *p;
    if (slot >= st->cap_peers) return 0;
    p = &st->peers[slot];
    if (!p->used || p->dropped) return 0;   /* ACTIVE peers only: don't reinforce announces to a
                                               dropped peer (it is silent/dead; the unicast just
                                               bounces, and a churned table fills with such ghosts) */
    return i_dart_discovery_disc_dest(p, out);   /* 2 = observed source: exact, never expanded */
}
