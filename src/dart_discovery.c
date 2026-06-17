/* sans-IO peer-discovery core. See dart_discovery.h. */
#include "dart_discovery.h"
#include <string.h>

#define DART_DISCOVERY_HDR_LEN 43
#define DART_DISCOVERY_FLAG_BYE 0x01
#define DART_DISCOVERY_FLAG_REQ 0x02         /* solicit: recipients announce back now */
#define DART_DISCOVERY_BLOB_RESEND 3u        /* announces that carry the full blob after a change */

struct dart_discovery_peer_ {
    uint8_t  used;
    uint8_t  uuid[16];
    uint32_t local_id;
    uint8_t  ip[16];
    uint8_t  ip_len;
    uint16_t port;
    uint64_t last_heard_us;
    uint8_t *meta;          /* -> meta_pool slot, capacity st->meta_cap */
    uint16_t meta_len;
    uint32_t meta_version;  /* version of the blob we hold (0 = none yet) */
    uint8_t  reply_due;     /* owes a unicast announce+blob (this peer solicited us) */
    uint8_t  solicit_due;   /* owes a unicast REQ (re-fetch: peer's version is ahead) */
};
typedef struct dart_discovery_peer_ dart_discovery_peer_;

struct dart_discovery_state {
    dart_discovery_config  cfg;
    uint64_t      next_announce_us;
    uint32_t      next_local_id;
    uint8_t       started;
    uint8_t       want_solicit;   /* a multicast solicit (REQ) is queued for the next update */
    uint16_t      cap_peers;
    uint16_t      meta_cap;       /* per-peer meta buffer capacity */
    uint8_t      *meta_pool;      /* [cap_peers * meta_cap] */
    /* our outgoing blob + monotonic version */
    const uint8_t *self_meta;
    uint16_t      self_meta_len;
    uint32_t      self_meta_version;
    uint16_t      self_blob_resend; /* announces remaining that carry the full blob */
    uint16_t      targeted_cursor;  /* round-robin over peers for poll_targeted */
    dart_discovery_peer_  *peers;
};

static void     dart_discovery_wr16(uint8_t *p, uint16_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static uint16_t dart_discovery_rd16(const uint8_t *p){ return (uint16_t)(p[0] | ((uint16_t)p[1]<<8)); }
static void     dart_discovery_wr32(uint8_t *p, uint32_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static uint32_t dart_discovery_rd32(const uint8_t *p){ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }

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

static uint16_t dart_discovery_meta_cap(const dart_discovery_config *cfg){
    return cfg->meta_cap ? cfg->meta_cap : DART_DISCOVERY_META_MAX;
}

size_t dart_discovery_required_memory(const dart_discovery_config *cfg){
    size_t s = (sizeof(struct dart_discovery_state) + 7u) & ~(size_t)7u;
    if (!cfg) return 0;
    return 8u + s + (size_t)cfg->max_peers * sizeof(dart_discovery_peer_)
                  + (size_t)cfg->max_peers * dart_discovery_meta_cap(cfg);
}

dart_discovery_state *dart_discovery_init(void *mem, size_t cap, const dart_discovery_config *cfg){
    uintptr_t a; uint8_t *base; size_t shdr; dart_discovery_state *st; uint16_t i, mcap;
    if (!mem || !cfg || cfg->max_peers == 0) return NULL;
    if (cfg->announce_us == 0 || cfg->timeout_us == 0) return NULL;
    mcap = dart_discovery_meta_cap(cfg);
    if (cfg->meta_len > mcap) return NULL;
    if (cfg->meta_len && !cfg->meta) return NULL;
    if (cap < dart_discovery_required_memory(cfg)) return NULL;

    a = ((uintptr_t)mem + 7u) & ~(uintptr_t)7u;
    base = (uint8_t *)a;
    shdr = (sizeof(struct dart_discovery_state) + 7u) & ~(size_t)7u;

    st = (dart_discovery_state *)base;
    memset(st, 0, sizeof(*st));
    st->cfg           = *cfg;
    st->cap_peers     = cfg->max_peers;
    st->meta_cap      = mcap;
    st->peers         = (dart_discovery_peer_ *)(base + shdr);
    st->meta_pool     = (uint8_t *)st->peers + (size_t)st->cap_peers * sizeof(dart_discovery_peer_);
    st->next_local_id = 1;
    st->started       = 0;
    memset(st->peers, 0, (size_t)st->cap_peers * sizeof(dart_discovery_peer_));
    for (i=0;i<st->cap_peers;i++) st->peers[i].meta = st->meta_pool + (size_t)i * mcap;
    st->self_meta         = cfg->meta;
    st->self_meta_len     = cfg->meta_len;
    st->self_meta_version = 1;
    st->self_blob_resend  = DART_DISCOVERY_BLOB_RESEND;
    return st;
}

static int dart_discovery_find(dart_discovery_state *st, const uint8_t *uuid){
    uint16_t i;
    for (i=0;i<st->cap_peers;i++)
        if (st->peers[i].used && memcmp(st->peers[i].uuid, uuid, 16)==0) return (int)i;
    return -1;
}

static int dart_discovery_alloc(dart_discovery_state *st){
    uint16_t i, stalest = 0; uint64_t oldest = (uint64_t)-1; int any = -1;
    for (i=0;i<st->cap_peers;i++){
        if (!st->peers[i].used) return (int)i;
        if (st->peers[i].last_heard_us <= oldest){ oldest = st->peers[i].last_heard_us; stalest = i; }
        any = 1;
    }
    if (any < 0) return -1;
    if (st->cfg.on_peer_down) st->cfg.on_peer_down(st->cfg.user, st->peers[stalest].local_id);
    st->peers[stalest].used = 0;
    return (int)stalest;
}

/* build an announce/solicit/bye into p. with_blob includes the current meta blob;
 * every datagram carries the meta version so a blob-less announce still signals change. */
static size_t dart_discovery_build(dart_discovery_state *st, uint8_t flags, int with_blob,
                                   uint8_t *p, size_t cap){
    uint16_t mlen = with_blob ? st->self_meta_len : 0;
    size_t need = (size_t)DART_DISCOVERY_META_OFF + mlen;
    if (cap < need) return 0;
    p[0]='u'; p[1]='D'; p[2]='S'; p[3]='C';
    p[4]=(uint8_t)DART_DISCOVERY_PROTO_VERSION;
    p[5]=flags;
    dart_discovery_wr16(p+6, st->cfg.domain_id);
    memcpy(p+8, st->cfg.uuid, 16);
    dart_discovery_wr16(p+24, st->cfg.data_port);
    p[26]= st->cfg.self_ip_len;
    memset(p+27, 0, 16);
    if (st->cfg.self_ip_len==4 || st->cfg.self_ip_len==16)
        memcpy(p+27, st->cfg.self_ip, st->cfg.self_ip_len);
    dart_discovery_wr32(p+DART_DISCOVERY_HDR_LEN, st->self_meta_version);
    dart_discovery_wr16(p+DART_DISCOVERY_HDR_LEN+4, mlen);
    if (mlen) memcpy(p+DART_DISCOVERY_META_OFF, st->self_meta, mlen);
    return need;
}

static void dart_discovery_addr_of(const dart_discovery_peer_ *pe, dart_discovery_addr *out){
    memset(out, 0, sizeof *out);
    memcpy(out->ip, pe->ip, 16);
    out->ip_len = pe->ip_len;
    out->port   = pe->port;
}

void dart_discovery_on_datagram(dart_discovery_state *st, const uint8_t *src_ip, uint8_t src_ip_len,
                       const void *dg, size_t len, uint64_t now){
    const uint8_t *p = (const uint8_t *)dg;
    uint8_t flags, sipl; uint16_t mlen, port; uint32_t mver;
    const uint8_t *uuid, *sip, *meta;
    dart_discovery_addr addr; int idx, addr_changed, first_contact=0, blob_changed=0;
    dart_discovery_peer_ *pe;

    if (len < (size_t)DART_DISCOVERY_META_OFF) return;
    if (p[0]!='u'||p[1]!='D'||p[2]!='S'||p[3]!='C') return;
    if (p[4]!=(uint8_t)DART_DISCOVERY_PROTO_VERSION) return;
    if (dart_discovery_rd16(p+6) != st->cfg.domain_id) return;
    mver = dart_discovery_rd32(p+DART_DISCOVERY_HDR_LEN);
    mlen = dart_discovery_rd16(p+DART_DISCOVERY_HDR_LEN+4);
    if (mlen > st->meta_cap || (size_t)DART_DISCOVERY_META_OFF + mlen > len) return;
    meta = p + DART_DISCOVERY_META_OFF;

    uuid = p+8;
    if (memcmp(uuid, st->cfg.uuid, 16)==0) return;  /* ignore self */

    flags = p[5];
    port  = dart_discovery_rd16(p+24);
    sipl  = p[26];
    sip   = p+27;

    memset(&addr, 0, sizeof addr);
    if (sipl==4 || sipl==16){ addr.ip_len = sipl; memcpy(addr.ip, sip, sipl); }
    else if (src_ip && (src_ip_len==4 || src_ip_len==16)){ addr.ip_len = src_ip_len; memcpy(addr.ip, src_ip, src_ip_len); }
    else return;
    addr.port = port;

    idx = dart_discovery_find(st, uuid);

    if (flags & DART_DISCOVERY_FLAG_BYE){
        if (idx >= 0){
            uint32_t lid = st->peers[idx].local_id;
            st->peers[idx].used = 0;
            if (st->cfg.on_peer_down) st->cfg.on_peer_down(st->cfg.user, lid);
        }
        return;
    }

    if (idx < 0){
        uint8_t *keep_meta;
        idx = dart_discovery_alloc(st);
        if (idx < 0) return;
        keep_meta = st->peers[idx].meta;            /* preserve the pool pointer across reset */
        memset(&st->peers[idx], 0, sizeof(dart_discovery_peer_));
        st->peers[idx].meta     = keep_meta;
        st->peers[idx].used     = 1;
        memcpy(st->peers[idx].uuid, uuid, 16);
        st->peers[idx].local_id = st->next_local_id++;
        st->peers[idx].ip_len   = 0xFF;   /* force first peer_up */
        first_contact = 1;
    }
    pe = &st->peers[idx];
    pe->last_heard_us = now;

    addr_changed = (pe->ip_len != addr.ip_len)
                || (pe->port   != addr.port)
                || (memcmp(pe->ip, addr.ip, 16) != 0);
    if (addr_changed){
        pe->ip_len = addr.ip_len; pe->port = addr.port;
        memcpy(pe->ip, addr.ip, 16);
    }

    /* meta: a newer version with the blob present updates our copy; a newer version
       without the blob (a steady-state version-only announce) means we fell behind,
       so re-fetch via a targeted solicit. */
    if (mlen){
        if (mver > pe->meta_version){
            memcpy(pe->meta, meta, mlen);
            pe->meta_len = mlen; pe->meta_version = mver;
            blob_changed = 1;
        }
        pe->solicit_due = 0;
    } else if (mver > pe->meta_version){
        pe->solicit_due = 1;
    }

    if ((first_contact || addr_changed || blob_changed) && st->cfg.on_peer_up)
        st->cfg.on_peer_up(st->cfg.user, pe->local_id, &addr,
                           pe->meta_len ? pe->meta : NULL, pe->meta_len);

    if ((flags & DART_DISCOVERY_FLAG_REQ) && st->started)
        pe->reply_due = 1;   /* answer the solicit with a unicast announce + blob */
}

size_t dart_discovery_update(dart_discovery_state *st, uint64_t now, void *out, size_t cap){
    uint16_t i;
    if (!st->started){
        st->started = 1;
        st->next_announce_us = now + (dart_discovery_fnv(st->cfg.uuid,16) % st->cfg.announce_us);
        st->want_solicit = 1;   /* solicit on startup */
    }
    for (i=0;i<st->cap_peers;i++){
        if (!st->peers[i].used) continue;
        if (now - st->peers[i].last_heard_us > st->cfg.timeout_us){
            uint32_t lid = st->peers[i].local_id;
            st->peers[i].used = 0;
            if (st->cfg.on_peer_down) st->cfg.on_peer_down(st->cfg.user, lid);
        }
    }
    if (st->want_solicit){   /* multicast solicit: announce us (with blob) AND ask peers to reply */
        st->want_solicit = 0;
        return dart_discovery_build(st, DART_DISCOVERY_FLAG_REQ, 1, (uint8_t *)out, cap);
    }
    if (now >= st->next_announce_us){
        int with_blob = st->self_blob_resend > 0;
        if (with_blob) st->self_blob_resend--;
        st->next_announce_us = now + st->cfg.announce_us;
        return dart_discovery_build(st, 0, with_blob, (uint8_t *)out, cap);
    }
    return 0;
}

void dart_discovery_set_meta(dart_discovery_state *st, const uint8_t *meta, uint16_t meta_len){
    if (!st || meta_len > st->meta_cap) return;   /* the node sizes meta_cap to fit */
    st->self_meta         = meta;
    st->self_meta_len     = meta_len;
    st->self_meta_version++;
    st->self_blob_resend  = DART_DISCOVERY_BLOB_RESEND;
    st->next_announce_us  = 0;   /* announce the change now, don't wait for the timer */
}

size_t dart_discovery_poll_targeted(dart_discovery_state *st, void *out, size_t cap,
                                    dart_discovery_addr *to){
    uint16_t n = st->cap_peers, k;
    if (n == 0) return 0;
    for (k=0;k<n;k++){
        uint16_t i = st->targeted_cursor;
        dart_discovery_peer_ *pe = &st->peers[i];
        st->targeted_cursor = (uint16_t)((i+1u) % n);
        if (!pe->used){ pe->reply_due = pe->solicit_due = 0; continue; }
        if (pe->reply_due){                 /* reply to a soliciter: announce + blob, unicast */
            pe->reply_due = 0;
            dart_discovery_addr_of(pe, to);
            return dart_discovery_build(st, 0, 1, (uint8_t *)out, cap);
        }
        if (pe->solicit_due){               /* re-fetch: ask this peer to announce back to us */
            pe->solicit_due = 0;
            dart_discovery_addr_of(pe, to);
            return dart_discovery_build(st, DART_DISCOVERY_FLAG_REQ, 1, (uint8_t *)out, cap);
        }
    }
    return 0;
}

/* queue a one-shot multicast solicit: the next update emits a REQ asking peers to announce now */
void dart_discovery_solicit(dart_discovery_state *st){ if (st) st->want_solicit = 1; }

uint16_t dart_discovery_peer_count(const dart_discovery_state *st){
    uint16_t i, c = 0;
    for (i=0;i<st->cap_peers;i++) if (st->peers[i].used) c++;
    return c;
}

size_t dart_discovery_leave(dart_discovery_state *st, void *out, size_t cap){
    return dart_discovery_build(st, DART_DISCOVERY_FLAG_BYE, 0, (uint8_t *)out, cap);
}

int dart_discovery_peer_addr(const dart_discovery_state *st, uint16_t slot, dart_discovery_addr *out){
    const dart_discovery_peer_ *p;
    if (slot >= st->cap_peers) return 0;
    p = &st->peers[slot];
    if (!p->used) return 0;
    dart_discovery_addr_of(p, out);
    return 1;
}
