/* sans-IO peer-discovery core. See dart_discovery.h. */
#include "core.h"
#include "../common/bytes.h"
#include "../common/arena.h"
#include <string.h>

#define DART_DISCOVERY_HDR_LEN 43
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
    uint8_t *meta;          /* -> meta_pool slot, capacity st->meta_capacity */
    uint16_t meta_len;
    uint32_t meta_version;  /* version of the blob we hold (0 = none yet) */
    uint8_t  reply_due;     /* owes a unicast announce+blob (this peer solicited us) */
    uint8_t  solicit_due;   /* owes a unicast REQ (re-fetch: peer's version is ahead) */
};
typedef struct i_DartDiscoveryPeer i_DartDiscoveryPeer;

struct DartDiscoveryState {
    DartDiscoveryConfig  cfg;
    uint64_t      next_announce_us;
    uint32_t      next_local_id;
    uint8_t       started;
    uint8_t       want_solicit;   /* a multicast solicit (REQ) is queued for the next update */
    uint16_t      cap_peers;
    uint16_t      meta_capacity;       /* per-peer meta buffer capacity */
    uint8_t      *meta_pool;      /* [cap_peers * meta_capacity] */
    /* our outgoing blob + monotonic version */
    const uint8_t *self_meta;
    uint16_t      self_meta_len;
    uint32_t      self_meta_version;
    uint16_t      self_blob_resend; /* announces remaining that carry the full blob */
    uint16_t      targeted_cursor;  /* round-robin over peers for poll_targeted */
    i_DartDiscoveryPeer  *peers;
};

static uint32_t dart_discovery_fnv(const uint8_t *d, size_t n){
    uint32_t h = 2166136261u; size_t i;
    for (i=0;i<n;i++){ h ^= d[i]; h *= 16777619u; }
    return h;
}

void dart_discovery_make_uuid(uint8_t out[16], const uint8_t *stable, size_t n, uint64_t seed){
    uint64_t x = 1469598103934665603ull; size_t i; int k;
    for (i=0;i<n;i++){ x = (x ^ stable[i]) * 1099511628211ull; }
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

static uint16_t dart_discovery_meta_capacity(const DartDiscoveryConfig *cfg){
    return cfg->meta_capacity ? cfg->meta_capacity : DART_DISCOVERY_META_MAX;
}

void dart_discovery_config_defaults(DartDiscoveryConfig *cfg){
    if (!cfg) return;
    if (cfg->announce_interval_us == 0) cfg->announce_interval_us = 1000000u;
    if (cfg->peer_timeout_us == 0)      cfg->peer_timeout_us = cfg->announce_interval_us * 7u / 2u;
    if (cfg->max_peers == 0)            cfg->max_peers = 32u;
}

uint32_t dart_discovery_wire_size(uint16_t meta_capacity){
    uint32_t cap = meta_capacity ? meta_capacity : DART_DISCOVERY_META_MAX;
    uint32_t w = (uint32_t)DART_DISCOVERY_META_OFF + cap;
    return w < DART_DISCOVERY_WIRE_MAX ? DART_DISCOVERY_WIRE_MAX : w;
}

/* Single source of the discovery arena layout: state, the peer table, the meta pool.
   measure (bump.base NULL) feeds required_memory; build feeds init -- one definition. */
typedef struct { DartDiscoveryState *st; uint8_t *peers, *meta_pool; } i_DartDiscoveryBlocks;
static void dart__discovery_layout(i_DartBump *b, const DartDiscoveryConfig *cfg, i_DartDiscoveryBlocks *o){
    uint16_t meta_capacity = dart_discovery_meta_capacity(cfg);
    o->st        = (DartDiscoveryState*)dart_take(b, sizeof(struct DartDiscoveryState), 8);
    o->peers     = (uint8_t*)dart_take(b, (size_t)cfg->max_peers * sizeof(i_DartDiscoveryPeer), 8);
    o->meta_pool = (uint8_t*)dart_take(b, (size_t)cfg->max_peers * meta_capacity, 1);
}

size_t dart_discovery_required_memory(const DartDiscoveryConfig *cfg){
    i_DartBump b; i_DartDiscoveryBlocks blk;
    if (!cfg) return 0;
    memset(&b, 0, sizeof b);
    dart__discovery_layout(&b, cfg, &blk);
    return b.offset + 8u;     /* slack to align the caller's mem up to base */
}

DartDiscoveryState *dart_discovery_init(void *mem, size_t cap, const DartDiscoveryConfig *cfg){
    i_DartBump b; i_DartDiscoveryBlocks blk; DartDiscoveryState *st; uint16_t i, meta_capacity;
    if (!mem || !cfg || cfg->max_peers == 0) return NULL;
    if (cfg->announce_interval_us == 0 || cfg->peer_timeout_us == 0) return NULL;
    meta_capacity = dart_discovery_meta_capacity(cfg);
    if (cfg->meta_len > meta_capacity) return NULL;
    if (cfg->meta_len && !cfg->meta) return NULL;
    if (cap < dart_discovery_required_memory(cfg)) return NULL;

    memset(&b, 0, sizeof b);
    b.base = (uint8_t*)(((uintptr_t)mem + 7u) & ~(uintptr_t)7u);
    b.cap  = cap - (size_t)(b.base - (uint8_t*)mem);
    dart__discovery_layout(&b, cfg, &blk);

    st = blk.st;
    memset(st, 0, sizeof(*st));
    st->cfg           = *cfg;
    st->cap_peers     = cfg->max_peers;
    st->meta_capacity      = meta_capacity;
    st->peers         = (i_DartDiscoveryPeer *)blk.peers;
    st->meta_pool     = blk.meta_pool;
    st->next_local_id = 1;
    st->started       = 0;
    memset(st->peers, 0, (size_t)st->cap_peers * sizeof(i_DartDiscoveryPeer));
    for (i=0;i<st->cap_peers;i++) st->peers[i].meta = st->meta_pool + (size_t)i * meta_capacity;
    st->self_meta         = cfg->meta;
    st->self_meta_len     = cfg->meta_len;
    st->self_meta_version = 1;
    st->self_blob_resend  = DART_DISCOVERY_BLOB_RESEND;
    return st;
}

/* find by UUID, including DROPPED entries: a same-UUID return reuses the slot (and
 * thus the local_id), so the IO layer's transport state keyed by local_id resumes. */
static int dart_discovery_find(DartDiscoveryState *st, const uint8_t *uuid){
    uint16_t i;
    for (i=0;i<st->cap_peers;i++)
        if (st->peers[i].used && memcmp(st->peers[i].uuid, uuid, 16)==0) return (int)i;
    return -1;
}

/* a slot for a brand-new peer: a FREE one, else the oldest DROPPED one (evicted,
 * fired GONE so its state is freed). ACTIVE peers are never evicted; -1 = refuse. */
static int dart_discovery_alloc(DartDiscoveryState *st){
    uint16_t i, victim = 0; uint64_t oldest = (uint64_t)-1; int found = -1;
    for (i=0;i<st->cap_peers;i++){
        if (!st->peers[i].used) return (int)i;
        if (st->peers[i].dropped && st->peers[i].last_heard_us <= oldest){
            oldest = st->peers[i].last_heard_us; victim = i; found = 1;
        }
    }
    if (found < 0) return -1;   /* table full of ACTIVE peers: caller refuses + signals */
    if (st->cfg.on_peer_down) st->cfg.on_peer_down(st->cfg.user, st->peers[victim].local_id, DART_DISCOVERY_GONE);
    st->peers[victim].used = 0;
    return (int)victim;
}

/* A different uuid announcing from an (ip,port) we already hold means that endpoint's
 * process restarted: one socket is one process, so the old entry is provably dead.
 * Evict it as GONE (state freed) before adopting the newcomer, so its stale transport
 * state can't shadow the new incarnation whose data routes to the same address. */
static void dart_discovery_evict_endpoint(DartDiscoveryState *st, const DartDiscoveryAddr *addr){
    uint16_t i;
    if (!addr->ip_len) return;
    for (i=0;i<st->cap_peers;i++){
        i_DartDiscoveryPeer *peer = &st->peers[i];
        if (!peer->used) continue;
        if (peer->ip_len==addr->ip_len && peer->port==addr->port && memcmp(peer->ip, addr->ip, 16)==0){
            uint32_t local_id = peer->local_id;
            peer->used = 0;
            if (st->cfg.on_peer_down) st->cfg.on_peer_down(st->cfg.user, local_id, DART_DISCOVERY_GONE);
        }
    }
}

/* build an announce/solicit/bye into p. with_blob includes the current meta blob;
 * every datagram carries the meta version so a blob-less announce still signals change. */
static size_t dart_discovery_build(DartDiscoveryState *st, uint8_t flags, int with_blob,
                                   uint8_t *p, size_t cap){
    uint16_t meta_len = with_blob ? st->self_meta_len : 0;
    size_t need = (size_t)DART_DISCOVERY_META_OFF + meta_len;
    if (cap < need) return 0;
    p[0]='u'; p[1]='D'; p[2]='S'; p[3]='C';
    p[4]=(uint8_t)DART_DISCOVERY_PROTO_VERSION;
    p[5]=flags;
    dart_le_w16(p+6, st->cfg.domain_id);
    memcpy(p+8, st->cfg.uuid, 16);
    dart_le_w16(p+24, st->cfg.data_port);
    p[26]= st->cfg.self_ip_len;
    memset(p+27, 0, 16);
    if (st->cfg.self_ip_len==4 || st->cfg.self_ip_len==16)
        memcpy(p+27, st->cfg.self_ip, st->cfg.self_ip_len);
    dart_le_w32(p+DART_DISCOVERY_HDR_LEN, st->self_meta_version);
    dart_le_w16(p+DART_DISCOVERY_HDR_LEN+4, meta_len);
    if (meta_len) memcpy(p+DART_DISCOVERY_META_OFF, st->self_meta, meta_len);
    return need;
}

static void dart_discovery_addr_of(const i_DartDiscoveryPeer *peer, DartDiscoveryAddr *out){
    memset(out, 0, sizeof *out);
    memcpy(out->ip, peer->ip, 16);
    out->ip_len = peer->ip_len;
    out->port   = peer->port;
}

void dart_discovery_on_datagram(DartDiscoveryState *st, const uint8_t *src_ip, uint8_t src_ip_len,
                       const void *datagram, size_t len, uint64_t now){
    const uint8_t *p = (const uint8_t *)datagram;
    uint8_t flags, self_ip_len; uint16_t meta_len, port; uint32_t meta_version;
    const uint8_t *uuid, *self_ip, *meta;
    DartDiscoveryAddr addr; int idx, addr_changed, first_contact=0, blob_changed=0, revived=0;
    i_DartDiscoveryPeer *peer;

    if (len < (size_t)DART_DISCOVERY_META_OFF) return;
    if (p[0]!='u'||p[1]!='D'||p[2]!='S'||p[3]!='C') return;
    if (p[4]!=(uint8_t)DART_DISCOVERY_PROTO_VERSION) return;
    if (dart_le_r16(p+6) != st->cfg.domain_id) return;
    meta_version = dart_le_r32(p+DART_DISCOVERY_HDR_LEN);
    meta_len = dart_le_r16(p+DART_DISCOVERY_HDR_LEN+4);
    if (meta_len > st->meta_capacity || (size_t)DART_DISCOVERY_META_OFF + meta_len > len) return;
    meta = p + DART_DISCOVERY_META_OFF;

    uuid = p+8;
    if (memcmp(uuid, st->cfg.uuid, 16)==0) return;  /* ignore self */

    flags = p[5];
    port  = dart_le_r16(p+24);
    self_ip_len  = p[26];
    self_ip   = p+27;

    memset(&addr, 0, sizeof addr);
    if (self_ip_len==4 || self_ip_len==16){ addr.ip_len = self_ip_len; memcpy(addr.ip, self_ip, self_ip_len); }
    else if (src_ip && (src_ip_len==4 || src_ip_len==16)){ addr.ip_len = src_ip_len; memcpy(addr.ip, src_ip, src_ip_len); }
    else return;
    addr.port = port;

    idx = dart_discovery_find(st, uuid);

    if (flags & DART_DISCOVERY_FLAG_BYE){
        if (idx >= 0){
            uint32_t local_id = st->peers[idx].local_id;
            st->peers[idx].used = 0;
            if (st->cfg.on_peer_down) st->cfg.on_peer_down(st->cfg.user, local_id, DART_DISCOVERY_GONE);
        }
        return;
    }

    if (idx < 0){
        uint8_t *keep_meta;
        /* new uuid from an address we already hold => the endpoint's process restarted;
           evict the dead predecessor (frees its slot for reuse) so it can't shadow us */
        dart_discovery_evict_endpoint(st, &addr);
        idx = dart_discovery_alloc(st);
        if (idx < 0){       /* table full of active peers: refuse, never evict a live one */
            if (st->cfg.on_peer_refused) st->cfg.on_peer_refused(st->cfg.user, &addr);
            return;
        }
        keep_meta = st->peers[idx].meta;            /* preserve the pool pointer across reset */
        memset(&st->peers[idx], 0, sizeof(i_DartDiscoveryPeer));
        st->peers[idx].meta     = keep_meta;
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

    /* meta: a newer version with the blob present updates our copy; a newer version
       without the blob (a steady-state version-only announce) means we fell behind,
       so re-fetch via a targeted solicit. */
    if (meta_len){
        if (meta_version > peer->meta_version){
            memcpy(peer->meta, meta, meta_len);
            peer->meta_len = meta_len; peer->meta_version = meta_version;
            blob_changed = 1;
        }
        peer->solicit_due = 0;
    } else if (meta_version > peer->meta_version){
        peer->solicit_due = 1;
    }

    if ((first_contact || addr_changed || blob_changed || revived) && st->cfg.on_peer_up)
        st->cfg.on_peer_up(st->cfg.user, peer->local_id, &addr,
                           peer->meta_len ? peer->meta : NULL, peer->meta_len);

    if ((flags & DART_DISCOVERY_FLAG_REQ) && st->started)
        peer->reply_due = 1;   /* answer the solicit with a unicast announce + blob */
}

size_t dart_discovery_update(DartDiscoveryState *st, uint64_t now, void *out, size_t cap){
    uint16_t i;
    if (!st->started){
        st->started = 1;
        st->next_announce_us = now + (dart_discovery_fnv(st->cfg.uuid,16) % st->cfg.announce_interval_us);
        st->want_solicit = 1;   /* solicit on startup */
    }
    for (i=0;i<st->cap_peers;i++){
        if (!st->peers[i].used || st->peers[i].dropped) continue;
        if (now - st->peers[i].last_heard_us > st->cfg.peer_timeout_us){
            /* fell silent: DEMOTE (keep the entry + local_id) so a same-UUID return
               resumes; the IO layer keeps its transport state on a DROP reason */
            st->peers[i].dropped = 1;
            if (st->cfg.on_peer_down) st->cfg.on_peer_down(st->cfg.user, st->peers[i].local_id, DART_DISCOVERY_DROP);
        }
    }
    if (st->want_solicit){   /* multicast solicit: announce us (with blob) AND ask peers to reply */
        st->want_solicit = 0;
        return dart_discovery_build(st, DART_DISCOVERY_FLAG_REQ, 1, (uint8_t *)out, cap);
    }
    if (now >= st->next_announce_us){
        int with_blob = st->self_blob_resend > 0;
        if (with_blob) st->self_blob_resend--;
        st->next_announce_us = now + st->cfg.announce_interval_us;
        return dart_discovery_build(st, 0, with_blob, (uint8_t *)out, cap);
    }
    return 0;
}

void dart_discovery_set_meta(DartDiscoveryState *st, const uint8_t *meta, uint16_t meta_len){
    if (!st || meta_len > st->meta_capacity) return;   /* the node sizes meta_capacity to fit */
    st->self_meta         = meta;
    st->self_meta_len     = meta_len;
    st->self_meta_version++;
    st->self_blob_resend  = DART_DISCOVERY_BLOB_RESEND;
    st->next_announce_us  = 0;   /* announce the change now, don't wait for the timer */
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
            dart_discovery_addr_of(peer, to);
            return dart_discovery_build(st, 0, 1, (uint8_t *)out, cap);
        }
        if (peer->solicit_due){               /* re-fetch: ask this peer to announce back to us */
            peer->solicit_due = 0;
            dart_discovery_addr_of(peer, to);
            return dart_discovery_build(st, DART_DISCOVERY_FLAG_REQ, 1, (uint8_t *)out, cap);
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
    if (!st || !st->cfg.on_peer_up) return;
    for (i=0;i<st->cap_peers;i++){
        i_DartDiscoveryPeer *peer = &st->peers[i];
        DartDiscoveryAddr addr;
        if (!peer->used || peer->dropped) continue;
        dart_discovery_addr_of(peer, &addr);
        st->cfg.on_peer_up(st->cfg.user, peer->local_id, &addr,
                           peer->meta_len ? peer->meta : NULL, peer->meta_len);
    }
}

uint16_t dart_discovery_peer_count(const DartDiscoveryState *st){
    uint16_t i, c = 0;   /* live peers only; DROPPED entries linger for resume, not as members */
    for (i=0;i<st->cap_peers;i++) if (st->peers[i].used && !st->peers[i].dropped) c++;
    return c;
}

size_t dart_discovery_leave(DartDiscoveryState *st, void *out, size_t cap){
    return dart_discovery_build(st, DART_DISCOVERY_FLAG_BYE, 0, (uint8_t *)out, cap);
}

int dart_discovery_peer_addr(const DartDiscoveryState *st, uint16_t slot, DartDiscoveryAddr *out){
    const i_DartDiscoveryPeer *p;
    if (slot >= st->cap_peers) return 0;
    p = &st->peers[slot];
    if (!p->used) return 0;
    dart_discovery_addr_of(p, out);
    return 1;
}
