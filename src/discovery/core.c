/* sans-IO peer-discovery core. See dart_discovery.h. */
#include "core.h"
#include "../common/bytes.h"
#include "../common/arena.h"
#include <string.h>

#define DART_DISCOVERY_HDR_LEN 24            /* magic(4) ver(1) flags(1) domain(2) uuid(16) */
/* the blob's discovery section: [u16 data_port][u8 self_ip_len][self_ip..][u8 name_len][name..] */
#define DART_DISCOVERY_DISC_MAX (2u + 1u + 16u + 1u + DART_DISCOVERY_NAME_MAX)
#define DART_DISCOVERY_FLAG_BYE 0x01
#define DART_DISCOVERY_FLAG_REQ 0x02         /* solicit: recipients announce back now */
#define DART_DISCOVERY_BLOB_RESEND 3u        /* announces that carry the full blob after a change */

struct i_DartDiscoveryPeer {
    uint8_t  used;
    uint8_t  dropped;       /* used but silent past peer_timeout_us: state kept for a same-UUID return */
    uint8_t  uuid[16];
    uint32_t local_id;
    uint8_t  ip[16];
    uint8_t  ip_len;
    uint16_t port;
    uint64_t last_heard_us;
    uint8_t *meta;          /* -> meta_pool slot (the OVERLAY only), capacity st->meta_capacity */
    uint16_t meta_len;
    uint32_t meta_version;  /* version of the blob we hold (0 = none yet) */
    uint32_t adv_version;   /* highest version the peer has advertised (> meta_version => we hold a stale blob) */
    uint8_t *user;          /* -> user_pool slot (opaque consumer scratch), stride st->user_stride */
    char     name[DART_DISCOVERY_NAME_MAX + 1];  /* advertised peer name, parsed from the blob */
    uint8_t  name_len;
    uint8_t  reply_due;     /* owes a unicast announce+blob (this peer solicited us) */
    uint8_t  solicit_due;   /* owes a unicast REQ (re-fetch: peer's version is ahead) */
};
typedef struct i_DartDiscoveryPeer i_DartDiscoveryPeer;

struct DartDiscoveryState {
    DartDiscoveryCoreConfig  cfg;
    uint64_t      next_announce_us;
    uint32_t      next_local_id;
    uint8_t       started;
    uint8_t       want_solicit;   /* a multicast solicit (REQ) is queued for the next update */
    uint16_t      cap_peers;
    uint16_t      meta_capacity;       /* per-peer meta buffer capacity */
    uint8_t      *meta_pool;      /* [cap_peers * meta_capacity] */
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

static uint16_t i_dart_discovery_meta_capacity(const DartDiscoveryCoreConfig *cfg){
    return cfg->meta_capacity ? cfg->meta_capacity : DART_DISCOVERY_META_MAX;
}

/* per-peer user-scratch stride: the requested bytes rounded up to 8 so every slot is
 * 8-aligned (a consumer may store a pointer there). 0 bytes => no pool. */
static uint16_t i_dart_discovery_user_stride(const DartDiscoveryCoreConfig *cfg){
    return cfg->peer_user_bytes ? (uint16_t)((cfg->peer_user_bytes + 7u) & ~7u) : 0u;
}

void dart_discovery_config_defaults(DartDiscoveryCoreConfig *cfg){
    if (!cfg) return;
    if (cfg->announce_interval_us == 0) cfg->announce_interval_us = 1000000u;
    if (cfg->peer_timeout_us == 0)      cfg->peer_timeout_us = cfg->announce_interval_us * 7u / 2u;
    if (cfg->max_peers == 0)            cfg->max_peers = 32u;
}

uint32_t dart_discovery_wire_size(uint16_t meta_capacity){
    uint32_t cap = meta_capacity ? meta_capacity : DART_DISCOVERY_META_MAX;
    /* the wire blob = discovery section + overlay, so the scratch buffer must hold both */
    uint32_t w = (uint32_t)DART_DISCOVERY_META_OFF + DART_DISCOVERY_DISC_MAX + cap;
    return w < DART_DISCOVERY_WIRE_MAX ? DART_DISCOVERY_WIRE_MAX : w;
}

/* Single source of the discovery arena layout: state, the peer table, the meta pool.
   measure (bump.base NULL) feeds required_memory; build feeds init -- one definition. */
typedef struct { DartDiscoveryState *st; uint8_t *peers, *meta_pool, *user_pool; } i_DartDiscoveryBlocks;
static void i_dart_discovery_layout(i_DartBump *b, const DartDiscoveryCoreConfig *cfg, i_DartDiscoveryBlocks *o){
    uint16_t meta_capacity = i_dart_discovery_meta_capacity(cfg);
    uint16_t user_stride   = i_dart_discovery_user_stride(cfg);
    o->st        = (DartDiscoveryState*)i_dart_bump_take(b, sizeof(struct DartDiscoveryState), 8);
    o->peers     = (uint8_t*)i_dart_bump_take(b, (size_t)cfg->max_peers * sizeof(i_DartDiscoveryPeer), 8);
    o->meta_pool = (uint8_t*)i_dart_bump_take(b, (size_t)cfg->max_peers * meta_capacity, 1);
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
    i_DartBump b; i_DartDiscoveryBlocks blk; DartDiscoveryState *st; uint16_t i, meta_capacity;
    if (!mem || !cfg || cfg->max_peers == 0) return NULL;
    if (cfg->announce_interval_us == 0 || cfg->peer_timeout_us == 0) return NULL;
    meta_capacity = i_dart_discovery_meta_capacity(cfg);
    if (cfg->meta.len > meta_capacity) return NULL;
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
    st->meta_capacity      = meta_capacity;
    st->peers         = (i_DartDiscoveryPeer *)blk.peers;
    st->meta_pool     = blk.meta_pool;
    st->user_stride   = i_dart_discovery_user_stride(cfg);
    st->user_pool     = blk.user_pool;
    st->next_local_id = 1;
    st->started       = 0;
    memset(st->peers, 0, (size_t)st->cap_peers * sizeof(i_DartDiscoveryPeer));
    if (st->user_pool) memset(st->user_pool, 0, (size_t)st->cap_peers * st->user_stride);
    for (i=0;i<st->cap_peers;i++){
        st->peers[i].meta = st->meta_pool + (size_t)i * meta_capacity;
        st->peers[i].user = st->user_pool ? st->user_pool + (size_t)i * st->user_stride : NULL;
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
        size_t new_cap, uint16_t new_max_peers, uint16_t new_meta_capacity,
        const uint8_t *self_meta, void *peer_cb_user){
    i_DartBump b; i_DartDiscoveryBlocks blk; DartDiscoveryState *st; DartDiscoveryCoreConfig dc; uint16_t i, omp;
    if (!old) return NULL;
    dc = old->cfg; dc.max_peers = new_max_peers; dc.meta_capacity = new_meta_capacity;
    if (new_cap < dart_discovery_required_memory(&dc)) return NULL;
    memset(&b,0,sizeof b);
    b.base = (uint8_t*)(((uintptr_t)new_mem + 7u) & ~(uintptr_t)7u);
    b.cap  = new_cap - (size_t)(b.base - (uint8_t*)new_mem);
    i_dart_discovery_layout(&b, &dc, &blk);
    st = blk.st;
    *st = *old;                            /* cfg (uuid!), counters, version, started, cursors */
    st->cfg.max_peers     = new_max_peers;
    st->cfg.meta_capacity = new_meta_capacity;
    st->cfg.user          = peer_cb_user;  /* peer callbacks fire on the relocated node core */
    st->cap_peers         = new_max_peers;
    st->meta_capacity     = new_meta_capacity;
    st->peers             = (i_DartDiscoveryPeer*)blk.peers;
    st->meta_pool         = blk.meta_pool;
    st->user_pool         = blk.user_pool;     /* user_stride is unchanged (copied via *st = *old) */
    st->self_meta.data    = self_meta;     /* re-point our blob (len preserved via *st=*old); version NOT bumped */
    memset(st->peers, 0, (size_t)new_max_peers * sizeof(i_DartDiscoveryPeer));
    for (i=0;i<new_max_peers;i++){
        st->peers[i].meta = st->meta_pool + (size_t)i * new_meta_capacity;
        st->peers[i].user = st->user_pool ? st->user_pool + (size_t)i * st->user_stride : NULL;
    }
    if (st->user_pool) memset(st->user_pool, 0, (size_t)new_max_peers * st->user_stride);
    omp = old->cap_peers;
    for (i=0;i<omp;i++){
        uint8_t *nmeta = st->peers[i].meta, *nuser = st->peers[i].user;
        st->peers[i] = old->peers[i];      /* carries old meta/user ptrs + everything */
        st->peers[i].meta = nmeta;
        st->peers[i].user = nuser;         /* re-point into the new user pool */
        if (old->peers[i].meta_len) memcpy(nmeta, old->peers[i].meta, old->peers[i].meta_len);
        if (st->user_stride && nuser && old->peers[i].user)
            memcpy(nuser, old->peers[i].user, st->user_stride);   /* preserve consumer scratch */
    }
    return st;
}

/* find by UUID, including DROPPED entries: a same-UUID return reuses the slot (and
 * thus the local_id), so the IO layer's transport state keyed by local_id resumes. */
static int i_dart_discovery_find(DartDiscoveryState *st, const uint8_t *uuid){
    uint16_t i;
    for (i=0;i<st->cap_peers;i++)
        if (st->peers[i].used && memcmp(st->peers[i].uuid, uuid, 16)==0) return (int)i;
    return -1;
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
    i_dart_discovery_fire_down(st, st->peers[victim].local_id, DART_DISCOVERY_GONE);
    st->peers[victim].used = 0;
    return (int)victim;
}

/* A different uuid announcing from an (ip,port) we already hold means that endpoint's
 * process restarted: one socket is one process, so the old entry is provably dead.
 * Evict it as GONE (state freed) before adopting the newcomer, so its stale transport
 * state can't shadow the new incarnation whose data routes to the same address.
 * A 0 port is "locator not advertised yet" (a peer first seen via a blob-less announce),
 * not a real socket: distinct peers awaiting their blob share that non-endpoint, so it
 * must NOT trigger eviction (else several same-host instances churn, evicting each other). */
static void i_dart_discovery_evict_endpoint(DartDiscoveryState *st, const DartDiscoveryAddr *addr){
    uint16_t i;
    if (!addr->ip_len || !addr->port) return;
    for (i=0;i<st->cap_peers;i++){
        i_DartDiscoveryPeer *peer = &st->peers[i];
        if (!peer->used) continue;
        if (peer->ip_len==addr->ip_len && peer->port==addr->port && memcmp(peer->ip, addr->ip, 16)==0){
            i_dart_discovery_fire_down(st, peer->local_id, DART_DISCOVERY_GONE);   /* fire, then free */
            peer->used = 0;
        }
    }
}

/* build an announce/solicit/bye into p. with_blob includes the meta blob =
 * [discovery section: locator + name][opaque overlay]; every datagram carries the meta
 * version so a blob-less announce still signals change. The fixed header is the small part. */
static size_t i_dart_discovery_build(DartDiscoveryState *st, uint8_t flags, int with_blob,
                                   uint8_t *p, size_t cap){
    uint16_t meta_len = 0;
    if (cap < (size_t)DART_DISCOVERY_META_OFF) return 0;
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

static void i_dart_discovery_addr_of(const i_DartDiscoveryPeer *peer, DartDiscoveryAddr *out){
    memset(out, 0, sizeof *out);
    memcpy(out->ip, peer->ip, 16);
    out->ip_len = peer->ip_len;
    out->port   = peer->port;
}

void dart_discovery_on_datagram(DartDiscoveryState *st, const uint8_t *src_ip, uint8_t src_ip_len,
                       DartBytes datagram, uint64_t now){
    const uint8_t *p = datagram.data; size_t len = datagram.len;
    uint8_t flags; uint16_t meta_len; uint32_t meta_version;
    const uint8_t *uuid, *blob;
    DartDiscoveryAddr addr; int idx, addr_changed, first_contact=0, blob_changed=0, revived=0;
    i_DartDiscoveryPeer *peer;
    /* the blob's discovery section (valid only when have_disc): locator + name, then overlay.
       disc_name points into the datagram (NOT NUL-terminated): a non-NUL wire string. */
    int have_disc=0; uint16_t disc_port=0; uint8_t disc_ip_len=0;
    const uint8_t *disc_ip=NULL; DartBytes overlay = {NULL, 0}; DartString disc_name = {NULL, 0};

    if (len < (size_t)DART_DISCOVERY_META_OFF) return;
    if (p[0]!='u'||p[1]!='D'||p[2]!='S'||p[3]!='C') return;
    if (p[4]!=(uint8_t)DART_DISCOVERY_PROTO_VERSION) return;
    if (i_dart_le_r16(p+6) != st->cfg.domain_id) return;
    meta_version = i_dart_le_r32(p+DART_DISCOVERY_HDR_LEN);
    meta_len = i_dart_le_r16(p+DART_DISCOVERY_HDR_LEN+4);
    if ((size_t)DART_DISCOVERY_META_OFF + meta_len > len) return;
    blob = p + DART_DISCOVERY_META_OFF;

    uuid = p+8;
    if (memcmp(uuid, st->cfg.uuid, 16)==0) return;  /* ignore self */
    flags = p[5];

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
        if (overlay.len > st->meta_capacity){   /* overlay must fit the per-peer buffer: this side
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
        if (idx >= 0){
            /* fire GONE while the slot still exists, so the handler can read its scratch,
               then free it */
            i_dart_discovery_fire_down(st, st->peers[idx].local_id, DART_DISCOVERY_GONE);
            st->peers[idx].used = 0;
        }
        return;
    }

    /* address: from the blob's locator when present (self_ip override, else the datagram
       source); otherwise the cached locator (it is static between blob updates), or the
       datagram source on first contact with no blob. */
    memset(&addr, 0, sizeof addr);
    if (have_disc){
        if (disc_ip_len){ addr.ip_len = disc_ip_len; memcpy(addr.ip, disc_ip, disc_ip_len); }
        else if (src_ip && (src_ip_len==4 || src_ip_len==16)){ addr.ip_len = src_ip_len; memcpy(addr.ip, src_ip, src_ip_len); }
        else return;
        addr.port = disc_port;
    } else if (idx >= 0){
        addr.ip_len = st->peers[idx].ip_len; addr.port = st->peers[idx].port;
        memcpy(addr.ip, st->peers[idx].ip, 16);
    } else if (src_ip && (src_ip_len==4 || src_ip_len==16)){
        addr.ip_len = src_ip_len; memcpy(addr.ip, src_ip, src_ip_len);   /* port unknown until the blob */
    } else return;

    if (idx < 0){
        uint8_t *keep_meta, *keep_user;
        /* new uuid from an address we already hold => the endpoint's process restarted;
           evict the dead predecessor (frees its slot for reuse) so it can't shadow us */
        i_dart_discovery_evict_endpoint(st, &addr);
        idx = i_dart_discovery_alloc(st);
        if (idx < 0){       /* table full of active peers: refuse, never evict a live one */
            i_dart_discovery_fire_refused(st, &addr);
            return;
        }
        keep_meta = st->peers[idx].meta;            /* preserve the pool pointers across reset */
        keep_user = st->peers[idx].user;
        memset(&st->peers[idx], 0, sizeof(i_DartDiscoveryPeer));
        st->peers[idx].meta     = keep_meta;
        st->peers[idx].user     = keep_user;
        if (keep_user) memset(keep_user, 0, st->user_stride);   /* fresh consumer scratch for the new peer */
        st->peers[idx].used     = 1;
        memcpy(st->peers[idx].uuid, uuid, 16);
        st->peers[idx].local_id = st->next_local_id++;
        st->peers[idx].ip_len   = 0xFF;   /* force first peer_up */
        first_contact = 1;
    }
    peer = &st->peers[idx];
    if (peer->dropped){ peer->dropped = 0; revived = 1; }   /* a DROPPED peer returned: resume it */
    peer->last_heard_us = now;

    addr_changed = (peer->ip_len != addr.ip_len)
                || (peer->port   != addr.port)
                || (memcmp(peer->ip, addr.ip, 16) != 0);
    if (addr_changed){
        peer->ip_len = addr.ip_len; peer->port = addr.port;
        memcpy(peer->ip, addr.ip, 16);
    }

    /* meta: a newer version with the blob present updates our stored overlay + name; a newer
       version without the blob (a steady-state version-only announce) means we fell behind,
       so re-fetch via a targeted solicit. adv_version tracks the highest version the peer has
       claimed (blob present or not); > meta_version means our held blob is stale. */
    if (meta_version > peer->adv_version) peer->adv_version = meta_version;
    if (have_disc){
        if (meta_version > peer->meta_version){
            uint8_t nl = disc_name.len > DART_DISCOVERY_NAME_MAX ? DART_DISCOVERY_NAME_MAX : (uint8_t)disc_name.len;
            if (overlay.len) memcpy(peer->meta, overlay.data, overlay.len);
            peer->meta_len = (uint16_t)overlay.len; peer->meta_version = meta_version;
            if (disc_name.data && nl) memcpy(peer->name, disc_name.data, nl);
            peer->name[nl] = '\0'; peer->name_len = nl;
            blob_changed = 1;
        }
        peer->solicit_due = 0;
    } else if (meta_version > peer->meta_version){
        peer->solicit_due = 1;
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
        if (!st->peers[i].used || st->peers[i].dropped) continue;
        if (now - st->peers[i].last_heard_us > st->cfg.peer_timeout_us){
            /* fell silent: DEMOTE (keep the entry + local_id) so a same-UUID return
               resumes; the IO layer keeps its transport state on a DROP reason */
            st->peers[i].dropped = 1;
            i_dart_discovery_fire_down(st, st->peers[i].local_id, DART_DISCOVERY_DROP);
        }
    }
    if (st->want_solicit){   /* multicast solicit: announce us (with blob) AND ask peers to reply */
        st->want_solicit = 0;
        return i_dart_discovery_build(st, DART_DISCOVERY_FLAG_REQ, 1, (uint8_t *)out, cap);
    }
    if (now >= st->next_announce_us){
        int with_blob = st->self_blob_resend > 0;
        if (with_blob) st->self_blob_resend--;
        st->next_announce_us = now + st->cfg.announce_interval_us;
        return i_dart_discovery_build(st, 0, with_blob, (uint8_t *)out, cap);
    }
    return 0;
}

void dart_discovery_set_meta(DartDiscoveryState *st, DartBytes meta){
    if (!st || meta.len > st->meta_capacity) return;   /* the node sizes meta_capacity to fit */
    st->self_meta         = meta;
    st->self_meta_version++;
    st->self_blob_resend  = DART_DISCOVERY_BLOB_RESEND;
    st->next_announce_us  = 0;   /* announce the change now, don't wait for the timer */
}

void dart_discovery_set_data_port(DartDiscoveryState *st, uint16_t port){
    if (!st || st->cfg.data_port == port) return;
    st->cfg.data_port = port;            /* the port now rides the blob's discovery section, so */
    st->self_meta_version++;             /* bump the version + re-send so peers re-fetch it */
    st->self_blob_resend = DART_DISCOVERY_BLOB_RESEND;
    st->next_announce_us = 0;
}

size_t dart_discovery_poll_targeted(DartDiscoveryState *st, void *out, size_t cap,
                                    DartDiscoveryAddr *to){
    uint16_t n = st->cap_peers, k;
    if (n == 0) return 0;
    for (k=0;k<n;k++){
        uint16_t i = st->targeted_cursor;
        i_DartDiscoveryPeer *peer = &st->peers[i];
        st->targeted_cursor = (uint16_t)((i+1u) % n);
        if (!peer->used){ peer->reply_due = peer->solicit_due = 0; continue; }
        if (peer->reply_due){                 /* reply to a soliciter: announce + blob, unicast */
            peer->reply_due = 0;
            i_dart_discovery_addr_of(peer, to);
            return i_dart_discovery_build(st, 0, 1, (uint8_t *)out, cap);
        }
        if (peer->solicit_due){               /* re-fetch: ask this peer to announce back to us */
            peer->solicit_due = 0;
            i_dart_discovery_addr_of(peer, to);
            return i_dart_discovery_build(st, DART_DISCOVERY_FLAG_REQ, 1, (uint8_t *)out, cap);
        }
    }
    return 0;
}

/* queue a one-shot multicast solicit: the next update emits a REQ asking peers to announce now */
void dart_discovery_solicit(DartDiscoveryState *st){ if (st) st->want_solicit = 1; }

/* re-deliver every live peer's last-known announce to on_peer_up, so a caller that just
 * changed its own advertised data re-applies all peer interest against the new state. No
 * version change is involved: a peer's blob is unchanged, but the LOCAL side may now have
 * a channel that the blob's interest matches. */
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
    i_dart_discovery_addr_of(p, out);
    return 1;
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
        if (p->used && p->ip_len == ip_len && p->port == port && memcmp(p->ip, ip, ip_len) == 0){
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
    i_dart_discovery_addr_of(p, out);
    return 1;
}
