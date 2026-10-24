/* The sans-IO discovery core. The rules are in spec/discovery.md. */
#include "records.h"
#include "../common/bytes.h"
#include "../common/arena.h"
#include "../common/hash.h"
#include <string.h>

#define RANT_DISCOVERY_BLOB_RESEND 1u          /* announces that carry the full blob after a change */
#define RANT_DISCOVERY_OPEN_REPEATS 2u       /* group sends that repeat the opening, for multicast loss */
#define RANT_DISCOVERY_OPEN_GAP_US 300000u     /* between them, all inside the first match window */
#define RANT_DISCOVERY_ASK_TRIES 4u            /* sends of one ask, a later datagram starts a fetch over */
#define RANT_DISCOVERY_ASK_RETRY_US 250000u    /* an unanswered ask goes again this soon */
#define RANT_DISCOVERY_SWEEPS 4u               /* timeout sweeps per announce interval */
#define RANT_DISCOVERY_JUDGE_MARGIN_US 250000u /* a peer's change this close to ours is not assumed judged */
#define RANT_DISCOVERY_LOOSE 8u                /* replies owed to askers that have no record */

/* What only a peer holding a slot needs. max_peers counts slots. */
typedef struct {
    uint16_t rec;            /* the record here, I_RANT_REC_NONE when free */
    uint16_t meta_cap;
    uint16_t meta_len;
    uint8_t  loan;           /* held on a LOAN verdict, which a match may take */
    uint8_t *meta;           /* the overlay: a pool slot or a grown hook block, kept across occupants */
    uint8_t *user;           /* consumer scratch, stride st->user_stride */
    uint64_t addr_heard_us;  /* last datagram that arrived from the locator. A fresh incumbent stays put */
    /* Sources its own announces came from lately, one per path on a multihomed host. Its
       data may leave from any of them, whichever locator it holds for us. */
    struct { uint8_t ip[16]; uint8_t ip_len; uint64_t heard_us; } paths[4];
} i_RantDiscoverySlot;

/* A reply owed to an asker that has no record here, sent as a record's reply. */
typedef struct { uint8_t ip[16]; uint8_t ip_len; uint16_t port; uint64_t due_us; } i_RantDiscoveryLoose;

struct i_RantDiscoveryState {
    i_RantDiscoveryCoreConfig    cfg;
    uint64_t      next_announce_us;
    uint64_t      next_sweep_us;
    uint8_t       started;
    uint8_t       want_solicit;   /* a group solicit is queued for the next update */
    uint8_t       distrust;       /* a peer was refused or forgotten for room: no first contact is assumed judged */
    uint8_t       slots_opened;   /* a slot came open: offer the open slots before returning */
    uint32_t      seat_misses;    /* records that wanted a slot and found none */
    uint16_t      cap_peers;      /* slots */
    uint16_t      meta_cap;       /* per peer meta buffer capacity */
    uint8_t      *meta_pool;      /* cap_peers x meta_cap, in the arena or the bulk. NULL in hook mode */
    uint16_t      user_stride;         /* per peer user scratch bytes, 8 aligned, 0 = none */
    uint8_t      *user_pool;      /* cap_peers x user_stride */
    RantBytes       self_meta;        /* our overlay, a view of the node's buffer */
    uint32_t        self_meta_version;
    uint64_t        changed_us;       /* when our overlay last changed, our start at first */
    char            self_name[RANT_NODE_NAME_MAX + 1];
    uint8_t         self_name_len;
    uint16_t        self_blob_resend; /* announces remaining that carry the full blob */
    uint8_t         open_repeats;     /* repeats of the opening still to send */
    uint64_t        next_repeat_us;
    i_RantDiscoverySubnet local_nets[RANT_DISCOVERY_MAX_SUBNETS];
    uint8_t       n_local_nets;
    /* troubled sources, a live one never evicted, so the set stays stable on a big mesh */
    i_RantDiscoveryTrouble trouble[RANT_DISCOVERY_TROUBLE_MAX];
    uint8_t       n_trouble;
    uint8_t       trouble_new;      /* the last note was news, see i_rant_discovery_trouble_new */
    uint64_t      trouble_more_us[3];   /* when a source past the table was last heard, by kind */
    uint64_t      now_us;         /* the latest time fed in, which ages the paths */
    uint64_t      forgot_us;      /* when a live peer was last forgotten for room, 0 = never */
    i_RantDiscoveryLoose loose[RANT_DISCOVERY_LOOSE];
    uint8_t       n_loose;
    i_RantDiscoveryRecords records;
    i_RantDiscoverySlot   *slots;   /* [cap_peers] */
};

static i_RantDiscoveryRecord *i_rant_discovery_rec(const i_RantDiscoveryState *st, uint16_t i){
    return i_rant_records_at(&st->records, i);
}
static i_RantDiscoverySlot *i_rant_discovery_slot_of(const i_RantDiscoveryState *st,
                                                     const i_RantDiscoveryRecord *rec){
    return rec->slot != I_RANT_REC_NONE ? &st->slots[rec->slot] : NULL;
}

static void i_rant_discovery_addr_of(const i_RantDiscoveryRecord *rec, RantAddr *out){
    memset(out, 0, sizeof *out);
    memcpy(out->ip, rec->ip, 16);
    out->ip_len = rec->ip_len;
    out->port   = rec->port;
}

/* The hook and event builders. peer_up carries the overlay we hold. */
static void i_rant_discovery_fire_up(i_RantDiscoveryState *st, const i_RantDiscoveryRecord *rec,
                               const RantAddr *addr){
    const i_RantDiscoverySlot *slot = i_rant_discovery_slot_of(st, rec);
    if (!st->cfg.on_peer_up) return;
    st->cfg.on_peer_up(st->cfg.user, rec->local_id, addr,
                       rant_bytes(slot && slot->meta_len ? slot->meta : NULL, slot ? slot->meta_len : 0u));
}
static void i_rant_discovery_fire_down(i_RantDiscoveryState *st, uint32_t id, i_RantDiscoveryDownReason reason){
    if (st->cfg.on_peer_down) st->cfg.on_peer_down(st->cfg.user, id, reason);
}
static void i_rant_discovery_fire_seen(i_RantDiscoveryState *st, const i_RantDiscoveryRecord *rec, int up){
    RantAddr addr;
    if (!st->cfg.on_peer_seen) return;
    i_rant_discovery_addr_of(rec, &addr);
    st->cfg.on_peer_seen(st->cfg.user, rec->local_id, &addr, up);
}
static void i_rant_discovery_fire_error(i_RantDiscoveryState *st, RantErrorKind error, uint32_t id,
                                  const RantAddr *addr, uint64_t too_big){
    RantEvent ev;
    if (!st->cfg.on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = RANT_ERROR; ev.error = error; ev.user = st->cfg.user; ev.peer = id;
    ev.too_big_bytes = too_big;
    if (addr){ memcpy(ev.ip, addr->ip, 16); ev.ip_len = addr->ip_len; ev.port = addr->port; }
    st->cfg.on_event(&ev);
}

void i_rant_discovery_make_uuid(uint8_t out[16], RantBytes stable, uint64_t seed){
    uint64_t x = 1469598103934665603ull; size_t i; int k;
    for (i=0;i<stable.len;i++){ x = (x ^ stable.data[i]) * 1099511628211ull; }
    x ^= seed;
    for (k=0;k<2;k++){
        uint64_t z = i_rant_hash_mix64(x += 0x9E3779B97F4A7C15ull);
        memcpy(out + (size_t)k*8, &z, 8);
    }
    out[6] = (uint8_t)((out[6] & 0x0Fu) | 0x80u);  /* version 8 */
    out[8] = (uint8_t)((out[8] & 0x3Fu) | 0x80u);  /* variant 10x */
}

const uint8_t *i_rant_discovery_uuid(const i_RantDiscoveryState *st){
    return st ? st->cfg.uuid : NULL;
}

static uint16_t i_rant_discovery_meta_cap(const i_RantDiscoveryCoreConfig *cfg){
    return cfg->meta_cap ? cfg->meta_cap : RANT_DISCOVERY_META_MAX;
}

/* The records a store may hold. Every slot holds a record, so never fewer than slots. */
static uint16_t i_rant_discovery_record_bound(const i_RantDiscoveryCoreConfig *cfg){
    return cfg->max_records > cfg->max_peers ? cfg->max_records : cfg->max_peers;
}
/* An arena store sits at its bound, a hook store starts at the slots and grows to it. */
static uint16_t i_rant_discovery_record_reserve(const i_RantDiscoveryCoreConfig *cfg){
    return cfg->alloc ? cfg->max_peers : i_rant_discovery_record_bound(cfg);
}

/* The hook for the record array and the peer blobs. */
static RantAllocFn i_rant_discovery_bulk(const i_RantDiscoveryCoreConfig *cfg){
    return cfg->bulk ? cfg->bulk : cfg->alloc;
}

/* Rounded up to 8 so a consumer may store a pointer in its slot. */
static uint16_t i_rant_discovery_user_stride(const i_RantDiscoveryCoreConfig *cfg){
    return cfg->peer_user_bytes ? (uint16_t)((cfg->peer_user_bytes + 7u) & ~7u) : 0u;
}

void i_rant_discovery_config_defaults(i_RantDiscoveryCoreConfig *cfg){
    if (!cfg) return;
    if (cfg->announce_interval_us == 0) cfg->announce_interval_us = RANT_ANNOUNCE_INTERVAL_US;
    if (cfg->peer_timeout_us == 0)      cfg->peer_timeout_us = RANT_ANNOUNCE_INTERVAL_US * 4u;   /* 12 s */
    if (cfg->gone_timeout_us == 0)      cfg->gone_timeout_us = 60000000u * 2u;   /* 2 min */
    if (cfg->max_peers == 0)            cfg->max_peers = RANT_MAX_PEERS;
}

uint32_t i_rant_discovery_wire_size(uint16_t meta_cap){
    uint32_t cap = meta_cap ? meta_cap : RANT_DISCOVERY_META_MAX;
    uint32_t w = (uint32_t)RANT_DISCOVERY_HEAD_MAX + cap;
    return w < RANT_DISCOVERY_WIRE_MAX ? RANT_DISCOVERY_WIRE_MAX : w;
}

/* The codec. Bytes 0 to 7 (magic, wire version, flags, domain) keep their place in every
 * version, so a node of this domain on another wire is still recognized. */
size_t i_rant_discovery_encode(const i_RantDiscoveryWire *w, uint8_t *out, size_t cap){
    uint8_t nl = w->name.len > RANT_NODE_NAME_MAX ? (uint8_t)RANT_NODE_NAME_MAX : (uint8_t)w->name.len;
    size_t ol = (w->flags & RANT_DISCOVERY_FLAG_BLOB) ? w->overlay.len : 0u;
    size_t len = (size_t)RANT_DISCOVERY_FIXED_LEN + nl + ol;
    if (!out || !w->uuid || ol > 0xFFFFu || len > cap) return 0;
    out[0]='u'; out[1]='D'; out[2]='S'; out[3]='C';
    out[4] = (uint8_t)RANT_WIRE_VERSION;
    out[5] = w->flags;
    i_rant_le_w16(out + 6, w->domain);
    memcpy(out + 8, w->uuid, 16);
    i_rant_le_w32(out + 24, w->version);
    i_rant_le_w32(out + 28, w->changed_ago_ms);
    i_rant_le_w16(out + 32, (uint16_t)ol);
    i_rant_le_w16(out + 34, w->data_port);
    out[36] = nl;
    if (nl) memcpy(out + RANT_DISCOVERY_FIXED_LEN, w->name.data, nl);
    if (ol) memcpy(out + RANT_DISCOVERY_FIXED_LEN + nl, w->overlay.data, ol);
    return len;
}

i_RantDiscoveryDecode i_rant_discovery_decode(RantBytes datagram, i_RantDiscoveryWire *w){
    const uint8_t *p = datagram.data; size_t len = datagram.len, at; uint8_t nl;
    memset(w, 0, sizeof *w);
    if (!p || len < 8 || p[0]!='u' || p[1]!='D' || p[2]!='S' || p[3]!='C') return I_RANT_DISC_FOREIGN;
    w->wire = p[4]; w->flags = p[5]; w->domain = i_rant_le_r16(p + 6);
    if (w->wire != (uint8_t)RANT_WIRE_VERSION) return I_RANT_DISC_WIRE;
    if (len < (size_t)RANT_DISCOVERY_FIXED_LEN) return I_RANT_DISC_MALFORMED;
    w->uuid           = p + 8;
    w->version        = i_rant_le_r32(p + 24);
    w->changed_ago_ms = i_rant_le_r32(p + 28);
    w->overlay_len    = i_rant_le_r16(p + 32);
    w->data_port      = i_rant_le_r16(p + 34);
    nl = p[36];
    at = (size_t)RANT_DISCOVERY_FIXED_LEN + nl;
    if (nl > RANT_NODE_NAME_MAX || len < at) return I_RANT_DISC_MALFORMED;
    w->name = rant_string((const char*)(p + RANT_DISCOVERY_FIXED_LEN), nl);
    if (!(w->flags & RANT_DISCOVERY_FLAG_BLOB))
        return (w->overlay_len || len != at) ? I_RANT_DISC_MALFORMED : I_RANT_DISC_OK;
    if (len < at + w->overlay_len) return I_RANT_DISC_CUT;      /* the OS cut it to our buffer */
    if (len > at + w->overlay_len) return I_RANT_DISC_MALFORMED;
    w->overlay = rant_bytes(p + at, w->overlay_len);
    return I_RANT_DISC_OK;
}

/* The one arena layout. Measure mode feeds required_memory and build mode feeds init. */
typedef struct {
    i_RantDiscoveryState *st; uint8_t *slots, *meta_pool, *user_pool;
    i_RantDiscoveryRecordsBlocks records;
} i_RantDiscoveryBlocks;
static void i_rant_discovery_layout(i_RantBump *b, const i_RantDiscoveryCoreConfig *cfg, i_RantDiscoveryBlocks *o){
    uint16_t meta_cap = i_rant_discovery_meta_cap(cfg);
    uint16_t user_stride   = i_rant_discovery_user_stride(cfg);
    o->st        = (i_RantDiscoveryState*)i_rant_bump_take(b, sizeof(struct i_RantDiscoveryState), 8);
    o->slots     = (uint8_t*)i_rant_bump_take(b, (size_t)cfg->max_peers * sizeof(i_RantDiscoverySlot), 8);
    /* hook mode allocates each blob on demand, only the pool path reserves the worst case,
       in the bulk when there is one */
    o->meta_pool = (cfg->alloc || cfg->bulk) ? NULL
                 : (uint8_t*)i_rant_bump_take(b, (size_t)cfg->max_peers * meta_cap, 1);
    o->user_pool = user_stride ? (uint8_t*)i_rant_bump_take(b, (size_t)cfg->max_peers * user_stride, 8) : NULL;
    i_rant_records_layout(b, i_rant_discovery_record_reserve(cfg), cfg->alloc, cfg->bulk, &o->records);
}

size_t i_rant_discovery_required_memory(const i_RantDiscoveryCoreConfig *cfg){
    i_RantBump b; i_RantDiscoveryBlocks blk;
    if (!cfg) return 0;
    memset(&b, 0, sizeof b);
    i_rant_discovery_layout(&b, cfg, &blk);
    return b.offset + 8u;     /* slack to align the caller's mem up to base */
}

/* Empties the slots from first up, each with its pool blob and zeroed scratch. */
static void i_rant_discovery_slots_reset(i_RantDiscoveryState *st, uint16_t first){
    uint16_t i;
    for (i = first; i < st->cap_peers; i++){
        i_RantDiscoverySlot *slot = &st->slots[i];
        memset(slot, 0, sizeof *slot);
        slot->rec      = I_RANT_REC_NONE;
        slot->meta     = st->meta_pool ? st->meta_pool + (size_t)i * st->meta_cap : NULL;
        slot->meta_cap = st->meta_pool ? st->meta_cap : 0;
        slot->user     = st->user_pool ? st->user_pool + (size_t)i * st->user_stride : NULL;
        if (slot->user) memset(slot->user, 0, st->user_stride);
    }
}

i_RantDiscoveryState *i_rant_discovery_init(void *mem, size_t cap, const i_RantDiscoveryCoreConfig *cfg){
    i_RantBump b; i_RantDiscoveryBlocks blk; i_RantDiscoveryState *st; uint16_t meta_cap;
    if (!mem || !cfg || cfg->max_peers == 0) return NULL;
    if (cfg->announce_interval_us == 0 || cfg->peer_timeout_us == 0) return NULL;
    meta_cap = i_rant_discovery_meta_cap(cfg);
    if (cfg->meta.len > meta_cap) return NULL;
    if (cfg->meta.len && !cfg->meta.data) return NULL;
    if (cap < i_rant_discovery_required_memory(cfg)) return NULL;

    memset(&b, 0, sizeof b);
    b.base = (uint8_t*)(((uintptr_t)mem + 7u) & ~(uintptr_t)7u);
    b.cap  = cap - (size_t)(b.base - (uint8_t*)mem);
    i_rant_discovery_layout(&b, cfg, &blk);

    st = blk.st;
    memset(st, 0, sizeof(*st));
    st->cfg           = *cfg;
    st->cap_peers     = cfg->max_peers;
    st->meta_cap      = meta_cap;
    st->slots         = (i_RantDiscoverySlot *)blk.slots;
    st->meta_pool     = blk.meta_pool;
    if (!cfg->alloc && cfg->bulk){
        st->meta_pool = (uint8_t*)cfg->bulk(cfg->alloc_user, NULL, (size_t)cfg->max_peers * meta_cap);
        if (!st->meta_pool) return NULL;
    }
    st->user_stride   = i_rant_discovery_user_stride(cfg);
    st->user_pool     = blk.user_pool;
    st->started       = 0;
    i_rant_discovery_slots_reset(st, 0);
    if (!i_rant_records_init(&st->records, &blk.records, i_rant_discovery_record_reserve(cfg),
                             i_rant_discovery_record_bound(cfg), cfg->alloc, cfg->bulk, cfg->alloc_user)){
        i_rant_discovery_destroy(st);
        return NULL;
    }
    st->self_meta         = cfg->meta;
    st->self_meta_version = 1;
    st->self_blob_resend  = RANT_DISCOVERY_BLOB_RESEND;
    {   uint8_t nl = cfg->name.len > RANT_NODE_NAME_MAX ? RANT_NODE_NAME_MAX : (uint8_t)cfg->name.len;
        if (cfg->name.data && nl) memcpy(st->self_name, cfg->name.data, nl);
        st->self_name[nl] = '\0'; st->self_name_len = nl; }
    return st;
}

static void i_rant_discovery_regrown(i_RantDiscoveryState *st, uint16_t old_slots, uint16_t old_meta_cap);

/* Not a re init: the uuid, blob version, records and slots survive, or peers would treat
 * us as a new node. The records and blobs are hook memory and carry over as they are. */
i_RantDiscoveryState *i_rant_discovery_core_migrate(i_RantDiscoveryState *old, void *new_mem,
        size_t new_cap, uint16_t new_max_peers, uint16_t new_meta_cap,
        const uint8_t *self_meta, void *peer_cb_user){
    i_RantBump b; i_RantDiscoveryBlocks blk; i_RantDiscoveryState *st; i_RantDiscoveryCoreConfig dc; uint16_t i;
    if (!old || !old->cfg.alloc || new_max_peers < old->cap_peers) return NULL;
    dc = old->cfg; dc.max_peers = new_max_peers; dc.meta_cap = new_meta_cap;
    if (new_cap < i_rant_discovery_required_memory(&dc)) return NULL;
    memset(&b,0,sizeof b);
    b.base = (uint8_t*)(((uintptr_t)new_mem + 7u) & ~(uintptr_t)7u);
    b.cap  = new_cap - (size_t)(b.base - (uint8_t*)new_mem);
    i_rant_discovery_layout(&b, &dc, &blk);
    st = blk.st;
    *st = *old;                            /* cfg, counters, version, cursors, records */
    st->cfg.max_peers     = new_max_peers;
    st->cfg.meta_cap      = new_meta_cap;
    st->cfg.user          = peer_cb_user;  /* peer callbacks fire on the relocated node core */
    st->cap_peers         = new_max_peers;
    if (st->records.max < new_max_peers) st->records.max = new_max_peers;   /* a record per slot */
    st->meta_cap          = i_rant_discovery_meta_cap(&dc);
    st->slots             = (i_RantDiscoverySlot*)blk.slots;
    st->user_pool         = blk.user_pool;
    st->self_meta.data    = self_meta;     /* the new blob address, version not bumped */
    for (i = 0; i < old->cap_peers; i++){
        st->slots[i] = old->slots[i];      /* a hook blob comes with the copy */
        st->slots[i].user = st->user_pool ? st->user_pool + (size_t)i * st->user_stride : NULL;
        if (st->slots[i].user) memcpy(st->slots[i].user, old->slots[i].user, st->user_stride);
    }
    i_rant_discovery_slots_reset(st, old->cap_peers);
    i_rant_discovery_regrown(st, old->cap_peers, old->meta_cap);
    return st;
}

void i_rant_discovery_destroy(i_RantDiscoveryState *st){
    uint16_t i;
    if (!st) return;
    if (st->cfg.alloc){                    /* pool mode has no hook allocated blob */
        for (i=0;i<st->cap_peers;i++)
            if (st->slots[i].meta){
                i_rant_discovery_bulk(&st->cfg)(st->cfg.alloc_user, st->slots[i].meta, 0);
                st->slots[i].meta = NULL; st->slots[i].meta_cap = 0; st->slots[i].meta_len = 0;
            }
    } else if (st->cfg.bulk && st->meta_pool){
        st->cfg.bulk(st->cfg.alloc_user, st->meta_pool, 0);
        st->meta_pool = NULL;
    }
    i_rant_records_destroy(&st->records);
}

/* Fires the hooks while the record and its slot still exist, so a handler can read them,
 * then frees both. A slot it held is open after. */
static void i_rant_discovery_peer_gone(i_RantDiscoveryState *st, uint16_t ri){
    i_RantDiscoveryRecord *rec = i_rant_discovery_rec(st, ri);
    if (!rec) return;
    if (rec->state == I_RANT_REC_ACTIVE) i_rant_discovery_fire_seen(st, rec, 0);
    if (rec->slot != I_RANT_REC_NONE){
        i_rant_discovery_fire_down(st, rec->local_id, RANT_DISCOVERY_GONE);
        st->slots[rec->slot].rec = I_RANT_REC_NONE;
        st->slots_opened = 1;
    }
    i_rant_records_remove(&st->records, ri);
}

/* A slotted record leaves its slot and keeps its record. The slot hooks see it go, the seen
 * hook nothing, and a match ask owed ends, as it is no match now. */
static void i_rant_discovery_give_back(i_RantDiscoveryState *st, uint16_t ri){
    i_RantDiscoveryRecord *rec = i_rant_discovery_rec(st, ri);
    if (!rec || rec->slot == I_RANT_REC_NONE) return;
    i_rant_records_settle(&st->records, ri, I_RANT_OWE_MATCH);
    i_rant_discovery_fire_down(st, rec->local_id, RANT_DISCOVERY_GIVE_BACK);
    st->slots[rec->slot].rec = I_RANT_REC_NONE;
    rec->slot = I_RANT_REC_NONE;
}

/* A slot for a record judged verdict: a free one, else the oldest DROPPED peer's, forgotten
 * as GONE, else for a match a live loan's, given back. A live match is never evicted. */
static uint16_t i_rant_discovery_slot_take(i_RantDiscoveryState *st, i_RantDiscoveryAdmit verdict){
    uint16_t s, victim = I_RANT_REC_NONE, loan = I_RANT_REC_NONE; uint64_t oldest = (uint64_t)-1;
    for (s = 0; s < st->cap_peers; s++){
        const i_RantDiscoveryRecord *rec;
        if (st->slots[s].rec == I_RANT_REC_NONE) return s;
        rec = i_rant_discovery_rec(st, st->slots[s].rec);
        if (rec->state == I_RANT_REC_DROPPED){
            if (rec->last_heard_us <= oldest){ oldest = rec->last_heard_us; victim = st->slots[s].rec; }
        } else if (st->slots[s].loan && loan == I_RANT_REC_NONE) loan = s;
    }
    if (victim != I_RANT_REC_NONE){
        s = i_rant_discovery_rec(st, victim)->slot;
        i_rant_discovery_peer_gone(st, victim);
        st->distrust = 1;                    /* a peer forgotten for room may still hold us */
        return s;
    }
    if (verdict != I_RANT_ADMIT_YES || loan == I_RANT_REC_NONE) return I_RANT_REC_NONE;
    victim = st->slots[loan].rec;
    i_rant_discovery_give_back(st, victim);
    i_rant_discovery_rec(st, victim)->want = I_RANT_ADMIT_LOAN;     /* waits for a free slot */
    return loan;
}

/* Gives record ri the free slot s: fresh scratch, the blob buffer reused. A slotted record
 * wants nothing more, and holds no blob until one is stored, so its version is fetched. */
static void i_rant_discovery_slot_bind(i_RantDiscoveryState *st, uint16_t s, uint16_t ri){
    i_RantDiscoverySlot *slot = &st->slots[s];
    i_RantDiscoveryRecord *rec = i_rant_discovery_rec(st, ri);
    uint8_t *meta = slot->meta, *user = slot->user; uint16_t meta_cap = slot->meta_cap;
    memset(slot, 0, sizeof *slot);
    slot->meta = meta; slot->meta_cap = meta_cap; slot->user = user;
    if (user) memset(user, 0, st->user_stride);
    slot->rec = ri;
    rec->slot = s;
    rec->meta_version = 0;
    rec->want = I_RANT_ADMIT_NO;
    rec->flags = (uint8_t)(rec->flags & ~I_RANT_REC_REFETCH);
}

/* A record for a new uuid: a fresh one, else the place of the oldest DROPPED record,
 * forgotten as GONE. NONE when every record is live or memory ran out. */
static uint16_t i_rant_discovery_record_take(i_RantDiscoveryState *st, const uint8_t uuid[16]){
    uint16_t ri = i_rant_records_add(&st->records, uuid), i, victim = I_RANT_REC_NONE;
    uint64_t oldest = (uint64_t)-1;
    if (ri != I_RANT_REC_NONE) return ri;
    for (i = 0; i < st->records.cap; i++){
        const i_RantDiscoveryRecord *rec = i_rant_discovery_rec(st, i);
        if (rec && rec->state == I_RANT_REC_DROPPED && rec->last_heard_us <= oldest){
            oldest = rec->last_heard_us; victim = i;
        }
    }
    if (victim == I_RANT_REC_NONE) return I_RANT_REC_NONE;
    i_rant_discovery_peer_gone(st, victim);
    st->distrust = 1;
    return i_rant_records_add(&st->records, uuid);
}

/* A live peer was forgotten for room. It may hold us and be a first contact again, so none
 * is assumed judged from now, and the node shows the directory partial for a while. */
static void i_rant_discovery_forgot(i_RantDiscoveryState *st){
    st->distrust = 1;
    st->forgot_us = st->now_us;
}

/* A new uuid from an (ip, port) we hold means that process restarted, so the old record is
 * evicted as GONE. Port 0 is not an endpoint. */
static void i_rant_discovery_evict_endpoint(i_RantDiscoveryState *st, const RantAddr *addr){
    uint16_t i;
    if (!addr->ip_len || !addr->port) return;
    for (i=0;i<st->records.cap;i++){
        const i_RantDiscoveryRecord *rec = i_rant_discovery_rec(st, i);
        if (rec && rec->ip_len==addr->ip_len && rec->port==addr->port && memcmp(rec->ip, addr->ip, 16)==0)
            i_rant_discovery_peer_gone(st, i);
    }
}

static int i_rant_discovery_addr_is(const RantAddr *a, const uint8_t *ip, uint8_t ip_len,
                                    uint16_t port){
    return a->ip_len == ip_len && a->port == port && memcmp(a->ip, ip, ip_len) == 0;
}

/* How long ago our overlay changed, from the latest time fed in. A stale clock reads it
 * younger, the safe side: a peer then fetches what it might otherwise assume judged. */
static uint32_t i_rant_discovery_change_age_ms(const i_RantDiscoveryState *st){
    uint64_t ms = st->now_us > st->changed_us ? (st->now_us - st->changed_us) / 1000u : 0u;
    return ms > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)ms;
}

/* Builds an announce, solicit or bye. with_blob adds the overlay. */
static size_t i_rant_discovery_build(i_RantDiscoveryState *st, uint8_t flags, int with_blob,
                                   uint8_t *p, size_t cap){
    i_RantDiscoveryWire w;
    memset(&w, 0, sizeof w);
    w.flags          = (uint8_t)(flags | (with_blob ? RANT_DISCOVERY_FLAG_BLOB : 0u));
    w.domain         = st->cfg.domain_id;
    w.uuid           = st->cfg.uuid;
    w.version        = st->self_meta_version;
    w.changed_ago_ms = i_rant_discovery_change_age_ms(st);
    w.data_port      = st->cfg.data_port;
    w.name           = rant_string(st->self_name, st->self_name_len);
    w.overlay        = st->self_meta;
    return i_rant_discovery_encode(&w, p, cap);
}

/* Higher wins: 2 on one of our subnets, 1 routed, 0 link local. See spec/discovery.md. */
static int i_rant_discovery_addr_rank(const i_RantDiscoveryState *st, const uint8_t *ip, uint8_t ip_len){
    uint8_t i;
    if (ip_len != 4) return 1;
    /* first, or a link local adapter's own /16 makes every 169.254 address look shared */
    if (ip[0] == 169 && ip[1] == 254) return 0;
    for (i = 0; i < st->n_local_nets; i++){
        const i_RantDiscoverySubnet *net = &st->local_nets[i];
        int k, same = 1;
        for (k = 0; k < 4; k++) if (((ip[k] ^ net->ip[k]) & net->mask[k]) != 0){ same = 0; break; }
        if (same) return 2;
    }
    return 1;
}

/* A path to the peer: the entry already holding ip, else the stalest, empty ones first. */
static void i_rant_discovery_note_path(i_RantDiscoverySlot *slot, const uint8_t *ip, uint8_t ip_len,
                                       uint64_t now){
    int i, at = 0;
    for (i = 0; i < 4; i++){
        if (slot->paths[i].ip_len == ip_len && memcmp(slot->paths[i].ip, ip, ip_len) == 0){ at = i; break; }
        if (slot->paths[i].heard_us < slot->paths[at].heard_us) at = i;
    }
    memcpy(slot->paths[at].ip, ip, ip_len);
    slot->paths[at].ip_len = ip_len;
    slot->paths[at].heard_us = now;
}

/* ip was a path to the peer within three announce intervals, so two lost announces in a
   row keep it and a path that went away stops counting. */
static int i_rant_discovery_on_path(const i_RantDiscoveryState *st, const i_RantDiscoverySlot *slot,
                                    const uint8_t *ip, uint8_t ip_len){
    int i;
    for (i = 0; i < 4; i++)
        if (slot->paths[i].ip_len == ip_len && memcmp(slot->paths[i].ip, ip, ip_len) == 0)
            return st->now_us - slot->paths[i].heard_us <= (uint64_t)st->cfg.announce_interval_us * 3u;
    return 0;
}

/* a trouble or a past the table mark is live while heard within three announce intervals */
static int i_rant_discovery_trouble_live(const i_RantDiscoveryState *st, uint64_t heard_us){
    return heard_us && st->now_us - heard_us <= (uint64_t)st->cfg.announce_interval_us * 3u;
}

static int i_rant_discovery_trouble_kind(RantErrorKind kind){
    return kind == RANT_E_WIRE_VERSION ? 0 : kind == RANT_E_PEER_REFUSED ? 1 : 2;
}

/* Records a troubled source heard now, so the node can hold a condition while it lasts. A
   full table takes a newcomer only in a stale entry's place, else it is counted as more.
   1 when it was news, see i_rant_discovery_trouble_new. */
static int i_rant_discovery_trouble_note(i_RantDiscoveryState *st, RantErrorKind kind,
                                         const RantAddr *a, uint32_t value, uint32_t peer){
    i_RantDiscoveryTrouble *t = NULL; int i;
    uint64_t *more = &st->trouble_more_us[i_rant_discovery_trouble_kind(kind)];
    for (i = 0; i < st->n_trouble; i++){
        t = &st->trouble[i];
        if (t->kind == kind && i_rant_discovery_addr_is(&t->addr, a->ip, a->ip_len, a->port)){
            t->heard_us = st->now_us; t->value = value; t->peer = peer;
            return st->trouble_new = 0;
        }
    }
    t = NULL;
    if (st->n_trouble < RANT_DISCOVERY_TROUBLE_MAX) t = &st->trouble[st->n_trouble++];
    else for (i = 0; i < st->n_trouble && !t; i++)
        if (!i_rant_discovery_trouble_live(st, st->trouble[i].heard_us)) t = &st->trouble[i];
    if (!t){
        st->trouble_new = (uint8_t)!i_rant_discovery_trouble_live(st, *more);
        *more = st->now_us;
        return st->trouble_new;
    }
    t->addr = *a; t->heard_us = st->now_us; t->value = value; t->peer = peer; t->kind = kind;
    return st->trouble_new = 1;
}

/* A source taken as a peer is no longer turned away. */
static void i_rant_discovery_trouble_drop(i_RantDiscoveryState *st, RantErrorKind kind, const RantAddr *a){
    int i;
    for (i = 0; i < st->n_trouble; i++)
        if (st->trouble[i].kind == kind && i_rant_discovery_addr_is(&st->trouble[i].addr, a->ip, a->ip_len, a->port))
            st->trouble[i].heard_us = 0;
}

int i_rant_discovery_troubles(const i_RantDiscoveryState *st, i_RantDiscoveryTrouble *out, int max){
    int i, n = 0;
    for (i = 0; i < st->n_trouble && n < max; i++)
        if (i_rant_discovery_trouble_live(st, st->trouble[i].heard_us)) out[n++] = st->trouble[i];
    return n;
}

int i_rant_discovery_trouble_more(const i_RantDiscoveryState *st, RantErrorKind kind){
    return i_rant_discovery_trouble_live(st, st->trouble_more_us[i_rant_discovery_trouble_kind(kind)]);
}

int i_rant_discovery_trouble_new(const i_RantDiscoveryState *st){
    return st->trouble_new;
}

int i_rant_discovery_partial(const i_RantDiscoveryState *st){
    return i_rant_discovery_trouble_live(st, st->forgot_us);
}

/* A datagram of this domain on another wire version. Its sender announces every interval, so
   each source is reported once while the trouble table remembers it, and once for the rest. */
static void i_rant_discovery_wire_mismatch(i_RantDiscoveryState *st, const RantAddr *src,
                                           uint8_t version){
    RantAddr a;
    memset(&a, 0, sizeof a);
    if (src && (src->ip_len==4 || src->ip_len==16)){ memcpy(a.ip, src->ip, src->ip_len); a.ip_len = src->ip_len; a.port = src->port; }
    if (i_rant_discovery_trouble_note(st, RANT_E_WIRE_VERSION, &a, version, 0))
        i_rant_discovery_fire_error(st, RANT_E_WIRE_VERSION, 0, &a, 0);
}

/* REFETCH ends: a blob came, or the record went silent or too big. The slot promised to a
 * record that wanted one is spare again. */
static void i_rant_discovery_refetch_done(i_RantDiscoveryState *st, i_RantDiscoveryRecord *rec){
    if (!(rec->flags & I_RANT_REC_REFETCH)) return;
    rec->flags = (uint8_t)(rec->flags & ~I_RANT_REC_REFETCH);
    if (rec->want != I_RANT_ADMIT_NO && rec->slot == I_RANT_REC_NONE) st->slots_opened = 1;
}

/* A blob bigger than we hold: the OS cut it to our buffer, or it is past the accept bound.
 * Its fetch stops until a newer version or a grown bound, since a retry brings the same
 * bytes. The node grows the bound on the event. wanted is the stated size. */
static void i_rant_discovery_too_big(i_RantDiscoveryState *st, uint16_t ri, const RantAddr *addr,
                                     uint32_t wanted){
    i_RantDiscoveryRecord *rec = i_rant_discovery_rec(st, ri);
    rec->flags |= I_RANT_REC_TOO_BIG;
    i_rant_discovery_refetch_done(st, rec);
    i_rant_records_settle(&st->records, ri, I_RANT_OWE_FETCH);
    (void)i_rant_discovery_trouble_note(st, RANT_E_PEER_META_TOO_BIG, addr, wanted, rec->local_id);
    i_rant_discovery_fire_error(st, RANT_E_PEER_META_TOO_BIG, rec->local_id, addr, wanted);
}

/* Takes the name every datagram carries. A uuid keeps its name for life, so this only fills it. */
static void i_rant_discovery_take_name(i_RantDiscoveryRecord *rec, RantString name){
    uint8_t nl = name.len > RANT_NODE_NAME_MAX ? (uint8_t)RANT_NODE_NAME_MAX : (uint8_t)name.len;
    if (rec->name_len == nl && (!nl || memcmp(rec->name, name.data, nl) == 0)) return;
    if (nl) memcpy(rec->name, name.data, nl);
    rec->name[nl] = '\0'; rec->name_len = nl;
}

/* Silent for more than limit_us. A time fed out of order reads as heard. */
static int i_rant_discovery_silent_past(uint64_t now, uint64_t heard_us, uint64_t limit_us){
    return now > heard_us && now - heard_us > limit_us;
}

/* Heard by a UNI copy within the peer timeout, so it may hear no group: only its record's
 * copies carry our changes to it. */
static int i_rant_discovery_heard_uni(const i_RantDiscoveryState *st, const i_RantDiscoveryRecord *rec){
    return rec->uni_us && !i_rant_discovery_silent_past(st->now_us, rec->uni_us, st->cfg.peer_timeout_us);
}

/* A record for a source that has none: the place of a live record that holds no slot and
 * wants none, forgotten, oldest heard first and one the group reaches before one heard by
 * UNI. A match takes either, a UNI source only the first. NONE without one. */
static uint16_t i_rant_discovery_record_evict(i_RantDiscoveryState *st, const uint8_t uuid[16], int match){
    uint16_t i, victim = I_RANT_REC_NONE;
    int victim_uni = 2;
    uint64_t oldest = (uint64_t)-1;
    for (i = 0; i < st->records.cap; i++){
        const i_RantDiscoveryRecord *rec = i_rant_discovery_rec(st, i);
        int uni;
        if (!rec || rec->state != I_RANT_REC_ACTIVE || rec->slot != I_RANT_REC_NONE
            || rec->want != I_RANT_ADMIT_NO) continue;
        uni = i_rant_discovery_heard_uni(st, rec);
        if (uni > victim_uni || (uni == victim_uni && rec->last_heard_us > oldest)) continue;
        victim = i; victim_uni = uni; oldest = rec->last_heard_us;
    }
    if (victim == I_RANT_REC_NONE || (victim_uni && !match)) return I_RANT_REC_NONE;
    i_rant_discovery_peer_gone(st, victim);
    i_rant_discovery_forgot(st);
    return i_rant_records_add(&st->records, uuid);
}

/* A solicit's flags. An observer's carries ALL, which every peer answers at once. */
static uint8_t i_rant_discovery_req_flags(const i_RantDiscoveryState *st){
    return (uint8_t)(RANT_DISCOVERY_FLAG_REQ | (st->cfg.observer ? RANT_DISCOVERY_FLAG_ALL : 0u));
}

/* When to answer a solicit with these flags. A targeted, unicast or observer one at once. A
 * group one within one ms per live peer, placed by both uuids, so a crowd's answers spread. */
static uint64_t i_rant_discovery_reply_at(const i_RantDiscoveryState *st, const uint8_t *uuid,
                                          uint8_t flags, uint64_t now){
    uint8_t pair[32];
    const uint8_t at_once = RANT_DISCOVERY_FLAG_UNI | RANT_DISCOVERY_FLAG_ALL | RANT_DISCOVERY_FLAG_ONE;
    if ((flags & at_once) || st->records.live <= 1u) return now;
    memcpy(pair, st->cfg.uuid, 16); memcpy(pair + 16, uuid, 16);
    return now + (uint64_t)i_rant_hash_below(i_rant_fnv1a64(pair, 32), st->records.live) * 1000u;
}

/* Owes record ri an ask: our blob with REQ|ONE, sent again until settled. One already owed
 * keeps its tries. After an ask that went unanswered, likely to a peer that refused us, the
 * next sends once. Nothing is owed to a silent peer. */
static void i_rant_discovery_ask(i_RantDiscoveryState *st, uint16_t ri, uint8_t bits, uint64_t now){
    i_RantDiscoveryRecord *rec = i_rant_discovery_rec(st, ri);
    if (!rec || rec->state != I_RANT_REC_ACTIVE) return;
    if (!(rec->owed & I_RANT_OWE_ASK))
        rec->ask_tries = (rec->flags & I_RANT_REC_ASK_MISSED) ? 1u : RANT_DISCOVERY_ASK_TRIES;
    i_rant_records_owe(&st->records, ri, bits, now);
}

/* Its blob is owed to us while it advertises a version past the one judged, unless that one
 * is past our bound, and while REFETCH says so. */
static int i_rant_discovery_wants_blob(const i_RantDiscoveryRecord *rec){
    return (rec->meta_version < rec->adv_version && !(rec->flags & I_RANT_REC_TOO_BIG))
        || (rec->flags & I_RANT_REC_REFETCH) != 0;
}

/* Starts a fetch while its blob is owed to us, ends one when not. One already owed keeps
 * its tries, so a datagram starts it over only once they ran out. */
static void i_rant_discovery_fetch_check(i_RantDiscoveryState *st, uint16_t ri, uint64_t now){
    i_RantDiscoveryRecord *rec = i_rant_discovery_rec(st, ri);
    if (!i_rant_discovery_wants_blob(rec)) i_rant_records_settle(&st->records, ri, I_RANT_OWE_FETCH);
    else if (!(rec->owed & I_RANT_OWE_FETCH)) i_rant_discovery_ask(st, ri, I_RANT_OWE_FETCH, now);
}

/* A live record without a slot that wanted one. */
static int i_rant_discovery_waiting(const i_RantDiscoveryRecord *rec){
    return rec && rec->state == I_RANT_REC_ACTIVE && rec->slot == I_RANT_REC_NONE
        && rec->want != I_RANT_ADMIT_NO;
}

/* Open slots, free or held by a DROPPED record, less those promised to a waiting record
 * whose fetch runs. */
static uint16_t i_rant_discovery_spare(const i_RantDiscoveryState *st){
    uint16_t i, spare = 0;
    for (i = 0; i < st->cap_peers; i++){
        const i_RantDiscoveryRecord *rec = i_rant_discovery_rec(st, st->slots[i].rec);
        if (!rec || rec->state == I_RANT_REC_DROPPED) spare++;
    }
    for (i = 0; i < st->records.cap && spare; i++){
        const i_RantDiscoveryRecord *rec = i_rant_discovery_rec(st, i);
        if (i_rant_discovery_waiting(rec) && (rec->flags & I_RANT_REC_REFETCH)) spare--;
    }
    return spare;
}

/* Promises waiting record ri a spare slot: its blob is fetched, and its answer takes the slot. */
static void i_rant_discovery_offer(i_RantDiscoveryState *st, uint16_t ri, uint64_t now){
    i_rant_discovery_rec(st, ri)->flags |= I_RANT_REC_REFETCH;
    i_rant_discovery_fetch_check(st, ri, now);
}

/* Open slots go to the records that wanted one, as many as are spare. One whose last ask
 * went unanswered is offered one when next heard, so a silent peer never holds them all. */
static void i_rant_discovery_offer_slots(i_RantDiscoveryState *st, uint64_t now){
    uint16_t i, spare;
    st->slots_opened = 0;
    spare = i_rant_discovery_spare(st);
    for (i = 0; i < st->records.cap && spare; i++){
        const i_RantDiscoveryRecord *rec = i_rant_discovery_rec(st, i);
        if (!i_rant_discovery_waiting(rec) || (rec->flags & (I_RANT_REC_REFETCH | I_RANT_REC_ASK_MISSED))) continue;
        i_rant_discovery_offer(st, i, now);
        spare--;
    }
}

/* After a migrate: grown slots go to the records that wanted one, and a grown bound fetches
 * each blob that was past it, from that peer alone. */
static void i_rant_discovery_regrown(i_RantDiscoveryState *st, uint16_t old_slots, uint16_t old_meta_cap){
    uint16_t i;
    if (st->meta_cap > old_meta_cap)
        for (i = 0; i < st->records.cap; i++){
            i_RantDiscoveryRecord *rec = i_rant_discovery_rec(st, i);
            if (!rec || !(rec->flags & I_RANT_REC_TOO_BIG)) continue;
            rec->flags = (uint8_t)((rec->flags & ~I_RANT_REC_TOO_BIG) | I_RANT_REC_REFETCH);
            i_rant_discovery_fetch_check(st, i, st->now_us);
        }
    if (st->cap_peers > old_slots || st->slots_opened) i_rant_discovery_offer_slots(st, st->now_us);
}

/* A record that meets us found no slot. Noted for the node's condition each time it is
 * heard, and fetched once a slot opens. */
static void i_rant_discovery_refuse(i_RantDiscoveryState *st, i_RantDiscoveryRecord *rec){
    RantAddr addr;
    rec->want = I_RANT_ADMIT_YES;
    st->seat_misses++;
    i_rant_discovery_addr_of(rec, &addr);
    (void)i_rant_discovery_trouble_note(st, RANT_E_PEER_REFUSED, &addr, st->cap_peers, rec->local_id);
    i_rant_discovery_fire_error(st, RANT_E_PEER_REFUSED, rec->local_id, &addr, 0);
}

/* A slot for record ri judged verdict. 0 when none can be had. */
static int i_rant_discovery_seat(i_RantDiscoveryState *st, uint16_t ri, i_RantDiscoveryAdmit verdict){
    uint16_t s = i_rant_discovery_slot_take(st, verdict);
    RantAddr addr;
    if (s == I_RANT_REC_NONE) return 0;
    i_rant_discovery_slot_bind(st, s, ri);
    st->slots[s].loan = (uint8_t)(verdict == I_RANT_ADMIT_LOAN);
    i_rant_discovery_addr_of(i_rant_discovery_rec(st, ri), &addr);
    i_rant_discovery_trouble_drop(st, RANT_E_PEER_REFUSED, &addr);
    return 1;
}

/* Copies a blob into a slot, growing a hook block to fit. 0 when hook memory ran out. A pool
 * slot holds meta_cap, which on_datagram checked. */
static int i_rant_discovery_slot_store(i_RantDiscoveryState *st, i_RantDiscoverySlot *slot, RantBytes blob){
    if (blob.len > slot->meta_cap){
        uint8_t *nb = st->cfg.alloc ? (uint8_t*)i_rant_discovery_bulk(&st->cfg)(st->cfg.alloc_user, slot->meta, blob.len) : NULL;
        if (!nb) return 0;
        slot->meta = nb; slot->meta_cap = (uint16_t)blob.len;
    }
    if (blob.len) memcpy(slot->meta, blob.data, blob.len);
    slot->meta_len = (uint16_t)blob.len;
    return 1;
}

/* The admit hook's word on a blob at version. Without a hook every peer meets us. The one
 * writer of FILTER: a NO keeps what the hook folded, any other verdict holds no filter. */
static i_RantDiscoveryAdmit i_rant_discovery_verdict(i_RantDiscoveryState *st, i_RantDiscoveryRecord *rec,
                                                     RantBytes blob, uint32_t version){
    i_RantDiscoveryAdmit verdict = I_RANT_ADMIT_YES;
    memset(rec->filter, 0, sizeof rec->filter);
    if (st->cfg.admit) verdict = st->cfg.admit(st->cfg.user, rec->local_id, blob, version, rec->filter);
    if (verdict == I_RANT_ADMIT_NO) rec->flags |= I_RANT_REC_FILTER;
    else rec->flags = (uint8_t)(rec->flags & ~I_RANT_REC_FILTER);
    return verdict;
}

/* Judges a blob of record ri that arrived at version, with the datagram's flags: a match
 * takes or keeps a slot, no match gives one back, a match without room is refused. A match
 * the peer may not know of is told by an ask. Sets *up when the slot hooks owe a peer up. */
static void i_rant_discovery_judge(i_RantDiscoveryState *st, uint16_t ri, RantBytes blob, uint32_t version,
                                   uint8_t flags, int *up){
    i_RantDiscoveryRecord *rec = i_rant_discovery_rec(st, ri);
    i_RantDiscoverySlot *slot = i_rant_discovery_slot_of(st, rec);
    /* a held blob was a match, and the peer may have given us back on a version we missed */
    int was_match = slot && !slot->loan && rec->meta_version, skipped = version > rec->meta_version + 1u;
    i_RantDiscoveryAdmit verdict;
    /* an older blob arrived out of order, and a held one is judged once per version */
    if (version < rec->meta_version || (slot && version == rec->meta_version)) return;
    i_rant_discovery_refetch_done(st, rec);
    rec->flags = (uint8_t)(rec->flags & ~I_RANT_REC_TOO_BIG);
    verdict = i_rant_discovery_verdict(st, rec, blob, version);
    if (verdict == I_RANT_ADMIT_NO){
        rec->meta_version = version; rec->want = I_RANT_ADMIT_NO;
        if (slot){ i_rant_discovery_give_back(st, ri); st->slots_opened = 1; }
        return;
    }
    if (!slot){
        if (!i_rant_discovery_seat(st, ri, verdict)){
            rec->meta_version = version;
            if (verdict == I_RANT_ADMIT_YES) i_rant_discovery_refuse(st, rec);
            else { rec->want = I_RANT_ADMIT_LOAN; st->seat_misses++; }
            return;
        }
        slot = i_rant_discovery_slot_of(st, rec);
        if (!i_rant_discovery_slot_store(st, slot, blob)){
            slot->rec = I_RANT_REC_NONE; rec->slot = I_RANT_REC_NONE;   /* no memory: the version gap fetches it again */
            st->slots_opened = 1;
            return;
        }
    } else if (!i_rant_discovery_slot_store(st, slot, blob)) return;    /* the stale blob stays, the fetch retries */
    rec->meta_version = version;
    if (verdict == I_RANT_ADMIT_YES) slot->loan = 0;     /* a held match judged a loan stays a match */
    *up = 1;
    /* ONE: the sender holds our blob or asks for it. ALL: an observer never assumes */
    if (verdict == I_RANT_ADMIT_YES && (!was_match || skipped)
        && !(flags & (RANT_DISCOVERY_FLAG_ONE | RANT_DISCOVERY_FLAG_ALL)))
        i_rant_discovery_ask(st, ri, I_RANT_OWE_MATCH, st->now_us);
}

/* Judges a slotted record again by the blob it holds. No match gives the slot back, a loan
 * that now meets us tells the peer. 1 when it gave the slot back. */
static int i_rant_discovery_rejudge_at(i_RantDiscoveryState *st, uint16_t ri){
    i_RantDiscoveryRecord *rec = i_rant_discovery_rec(st, ri);
    i_RantDiscoverySlot *slot = rec ? i_rant_discovery_slot_of(st, rec) : NULL;
    i_RantDiscoveryAdmit verdict;
    if (!slot) return 0;
    verdict = i_rant_discovery_verdict(st, rec, rant_bytes(slot->meta_len ? slot->meta : NULL, slot->meta_len),
                                       rec->meta_version);
    if (verdict == I_RANT_ADMIT_NO){
        i_rant_discovery_give_back(st, ri);
        st->slots_opened = 1;
        return 1;
    }
    if (verdict == I_RANT_ADMIT_YES && slot->loan){
        slot->loan = 0;
        i_rant_discovery_ask(st, ri, I_RANT_OWE_MATCH, st->now_us);
    }
    return 0;
}

/* A first contact heard without a blob is judged by assumption when its blob last changed
 * before ours did, by the margin: it judged our current blob since and asks us if it meets
 * us. Before our opening, ours is about to change. Only between symmetric judges, so never
 * without a hook, as an observer, or while distrusting, and never from a plain copy (ONE),
 * whose sender holds us and asks nothing. A comparison, so it never wraps. */
static int i_rant_discovery_assume_judged(const i_RantDiscoveryState *st, const i_RantDiscoveryWire *w, uint64_t now){
    uint64_t ours = st->started && now > st->changed_us ? now - st->changed_us : 0u;
    if (!st->cfg.admit || st->cfg.observer || st->distrust || (w->flags & RANT_DISCOVERY_FLAG_ONE)) return 0;
    return (uint64_t)w->changed_ago_ms * 1000u > ours + RANT_DISCOVERY_JUDGE_MARGIN_US;
}

/* The word on a source with no record, its blob judged with no id into a scratch filter. A
 * header alone says nothing of it, unless no hook judges, when every peer meets us. */
static i_RantDiscoveryAdmit i_rant_discovery_stranger_verdict(i_RantDiscoveryState *st,
                                                              const i_RantDiscoveryWire *w, int have_blob){
    uint8_t scratch[RANT_DISCOVERY_FILTER_BYTES];
    if (!st->cfg.admit) return I_RANT_ADMIT_YES;
    if (!have_blob) return I_RANT_ADMIT_NO;
    memset(scratch, 0, sizeof scratch);
    return st->cfg.admit(st->cfg.user, 0, w->overlay, w->version, scratch);
}

/* Owes an asker with no record a reply. One already queued keeps the earlier time, and a
 * full queue drops it, since the asker asks again. */
static void i_rant_discovery_loose_owe(i_RantDiscoveryState *st, const RantAddr *a, uint64_t due_us){
    i_RantDiscoveryLoose *l;
    uint8_t i;
    for (i = 0; i < st->n_loose; i++){
        l = &st->loose[i];
        if (!i_rant_discovery_addr_is(a, l->ip, l->ip_len, l->port)) continue;
        if (due_us < l->due_us) l->due_us = due_us;
        return;
    }
    if (st->n_loose >= RANT_DISCOVERY_LOOSE) return;
    l = &st->loose[st->n_loose++];
    memcpy(l->ip, a->ip, 16); l->ip_len = a->ip_len; l->port = a->port; l->due_us = due_us;
}

/* A source that found no record is forgotten, and judged again each time its blob passes
 * by. A match among them is refused, and a solicit is answered from the loose queue. */
static void i_rant_discovery_stranger(i_RantDiscoveryState *st, const i_RantDiscoveryWire *w,
                                      const RantAddr *addr, i_RantDiscoveryAdmit verdict, uint64_t now){
    i_rant_discovery_forgot(st);
    if (verdict == I_RANT_ADMIT_YES){
        (void)i_rant_discovery_trouble_note(st, RANT_E_PEER_REFUSED, addr, st->cap_peers, 0);
        i_rant_discovery_fire_error(st, RANT_E_PEER_REFUSED, 0, addr, 0);
    }
    if (w->flags & RANT_DISCOVERY_FLAG_REQ)
        i_rant_discovery_loose_owe(st, addr, i_rant_discovery_reply_at(st, w->uuid, w->flags, now));
    if (st->slots_opened) i_rant_discovery_offer_slots(st, now);
}

void i_rant_discovery_on_datagram(i_RantDiscoveryState *st, const RantAddr *src,
                       RantBytes datagram, uint64_t now){
    const uint8_t *src_ip = (src && (src->ip_len==4 || src->ip_len==16)) ? src->ip : NULL;
    uint8_t  src_ip_len = src_ip ? src->ip_len : 0;
    i_RantDiscoveryWire w; i_RantDiscoveryDecode dr;
    RantAddr addr; int addr_changed, first_contact=0, revived=0, have_blob, up=0;
    uint32_t too_big = 0;
    uint16_t ri;
    i_RantDiscoveryRecord *rec;
    i_RantDiscoverySlot *slot;

    st->now_us = now;
    dr = i_rant_discovery_decode(datagram, &w);
    if (dr == I_RANT_DISC_FOREIGN || w.domain != st->cfg.domain_id) return;
    if (dr == I_RANT_DISC_WIRE){ i_rant_discovery_wire_mismatch(st, src, w.wire); return; }
    if (dr == I_RANT_DISC_MALFORMED || memcmp(w.uuid, st->cfg.uuid, 16) == 0) return;   /* or self */
    /* a blob we cannot hold leaves the header, which still counts */
    have_blob = dr == I_RANT_DISC_OK && (w.flags & RANT_DISCOVERY_FLAG_BLOB);
    if (dr == I_RANT_DISC_CUT) too_big = w.overlay_len;
    else if (have_blob && w.overlay.len > st->meta_cap) too_big = (uint32_t)w.overlay.len;
    if (too_big) have_blob = 0;

    ri  = i_rant_records_find(&st->records, w.uuid);
    rec = i_rant_discovery_rec(st, ri);

    if (w.flags & RANT_DISCOVERY_FLAG_BYE){
        if (rec) i_rant_discovery_peer_gone(st, ri);     /* a BYE is GONE */
        if (st->slots_opened) i_rant_discovery_offer_slots(st, now);
        return;
    }

    /* The IP is a candidate from the source, the port the one every datagram names. See
       spec/discovery.md. */
    memset(&addr, 0, sizeof addr);
    if (src_ip){ addr.ip_len = src_ip_len; memcpy(addr.ip, src_ip, src_ip_len); }
    else if (rec){ addr.ip_len = rec->ip_len; memcpy(addr.ip, rec->ip, 16); }
    else return;
    addr.port = w.data_port;

    if (!rec){
        i_RantDiscoveryAdmit verdict = I_RANT_ADMIT_NO;
        /* a new uuid from a held endpoint means a restart: evict the dead predecessor */
        i_rant_discovery_evict_endpoint(st, &addr);
        ri = i_rant_discovery_record_take(st, w.uuid);
        if (ri == I_RANT_REC_NONE){      /* every record is live: a match or a UNI source takes one's place */
            verdict = i_rant_discovery_stranger_verdict(st, &w, have_blob);
            if (verdict == I_RANT_ADMIT_YES || (w.flags & RANT_DISCOVERY_FLAG_UNI))
                ri = i_rant_discovery_record_evict(st, w.uuid, verdict == I_RANT_ADMIT_YES);
        }
        rec = i_rant_discovery_rec(st, ri);
        if (!rec){
            i_rant_discovery_stranger(st, &w, &addr, verdict, now);
            return;
        }
        first_contact = 1;
    }
    slot = i_rant_discovery_slot_of(st, rec);
    i_rant_discovery_take_name(rec, w.name);
    if (rec->state == I_RANT_REC_DROPPED){     /* resume it */
        i_rant_records_set_state(&st->records, ri, I_RANT_REC_ACTIVE);
        revived = 1;
    }
    rec->last_heard_us = now;
    if (w.flags & RANT_DISCOVERY_FLAG_UNI) rec->uni_us = now;
    if (w.flags & RANT_DISCOVERY_FLAG_ONE){    /* it holds our blob, or asks and our reply follows */
        i_rant_records_settle(&st->records, ri, I_RANT_OWE_MATCH);
        rec->flags = (uint8_t)(rec->flags & ~I_RANT_REC_ASK_MISSED);
    }

    /* Rank the candidate against the incumbent so a multi path locator never flaps. Only a
       slot keeps the freshness, as only its locator carries data. See spec/discovery.md. */
    if (rec->ip_len == 4 && addr.ip_len == 4 && memcmp(rec->ip, addr.ip, 4) != 0){
        int rank_new = i_rant_discovery_addr_rank(st, addr.ip, 4);
        int rank_old = i_rant_discovery_addr_rank(st, rec->ip, 4);
        uint64_t fresh = slot ? slot->addr_heard_us : 0u;
        if (rank_new < rank_old ||
            (rank_new == rank_old && fresh &&
             now - fresh < (uint64_t)st->cfg.announce_interval_us * 2u))
            memcpy(addr.ip, rec->ip, 4);          /* keep the incumbent */
    }

    addr_changed = (rec->ip_len != addr.ip_len)
                || (rec->port   != addr.port)
                || (memcmp(rec->ip, addr.ip, 16) != 0);
    if (addr_changed){
        rec->ip_len = addr.ip_len; rec->port = addr.port;
        memcpy(rec->ip, addr.ip, 16);
    }
    if (first_contact || revived) i_rant_discovery_fire_seen(st, rec, 1);

    /* A blob is judged on arrival. Without one, a peer that meets us takes a slot when heard
       if there is no hook to judge by, a first contact may be judged by assumption, and a
       record that wanted a slot asks for one again. */
    if (w.version > rec->adv_version){
        rec->adv_version = w.version;
        rec->flags = (uint8_t)(rec->flags & ~I_RANT_REC_TOO_BIG);   /* a newer blob may fit */
    }
    if (too_big) i_rant_discovery_too_big(st, ri, &addr, too_big);
    if (have_blob) i_rant_discovery_judge(st, ri, w.overlay, w.version, w.flags, &up);
    else if (!st->cfg.admit){
        if (!slot){
            if (i_rant_discovery_seat(st, ri, I_RANT_ADMIT_YES)) up = 1;
            else i_rant_discovery_refuse(st, rec);
        }
    } else if (first_contact && !too_big && w.version && i_rant_discovery_assume_judged(st, &w, now))
        rec->meta_version = w.version;
    else if (i_rant_discovery_waiting(rec) && !(rec->flags & I_RANT_REC_REFETCH)){
        if (i_rant_discovery_spare(st)) i_rant_discovery_offer(st, ri, now);
        else if (rec->want == I_RANT_ADMIT_YES) i_rant_discovery_refuse(st, rec);
    }
    i_rant_discovery_fetch_check(st, ri, now);
    if (w.flags & RANT_DISCOVERY_FLAG_REQ)
        i_rant_records_owe(&st->records, ri, I_RANT_OWE_REPLY,
                           i_rant_discovery_reply_at(st, rec->uuid, w.flags, now));

    slot = i_rant_discovery_slot_of(st, rec);
    if (slot){
        if (src_ip) i_rant_discovery_note_path(slot, src_ip, src_ip_len, now);
        /* only a datagram that really arrived from the held locator counts as freshness */
        if (rec->ip_len == 4 && src_ip && src_ip_len == 4 && memcmp(rec->ip, src_ip, 4) == 0)
            slot->addr_heard_us = now;
        if (up || addr_changed || revived) i_rant_discovery_fire_up(st, rec, &addr);
    }
    if (st->slots_opened) i_rant_discovery_offer_slots(st, now);
}

/* Demotes the records silent past the timeout and frees the DROPPED ones silent past the
 * gone timeout too. */
static void i_rant_discovery_sweep(i_RantDiscoveryState *st, uint64_t now){
    uint16_t i;
    for (i=0;i<st->records.cap;i++){
        i_RantDiscoveryRecord *rec = i_rant_discovery_rec(st, i);
        if (!rec) continue;
        if (rec->state == I_RANT_REC_DROPPED){
            if (st->cfg.gone_timeout_us && i_rant_discovery_silent_past(now, rec->last_heard_us,
                    (uint64_t)st->cfg.peer_timeout_us + st->cfg.gone_timeout_us))
                i_rant_discovery_peer_gone(st, i);
            continue;
        }
        if (i_rant_discovery_silent_past(now, rec->last_heard_us, st->cfg.peer_timeout_us)){
            /* the record, slot and id stay so a same uuid return resumes. Nothing owed goes
               to a silent peer, and a slot it holds is open to a newcomer */
            i_rant_records_set_state(&st->records, i, I_RANT_REC_DROPPED);
            i_rant_records_settle(&st->records, i, 0xFFu);
            i_rant_discovery_refetch_done(st, rec);
            i_rant_discovery_fire_seen(st, rec, 0);
            if (rec->slot != I_RANT_REC_NONE){
                i_rant_discovery_fire_down(st, rec->local_id, RANT_DISCOVERY_DROP);
                st->slots_opened = 1;
            }
        }
    }
}

size_t i_rant_discovery_update(i_RantDiscoveryState *st, uint64_t now, void *out, size_t cap){
    uint32_t sweep_us = st->cfg.announce_interval_us / RANT_DISCOVERY_SWEEPS;
    st->now_us = now;
    if (sweep_us == 0) sweep_us = 1;
    if (!st->started){
        /* the opening carries our blob, so the resend and a solicit asked before it ride it */
        int ask = st->cfg.observer || st->want_solicit;
        st->started = 1;
        st->changed_us = now;   /* our overlay is new to the mesh from here */
        st->next_announce_us = now + i_rant_hash_below(i_rant_fnv1a64(st->cfg.uuid, 16), st->cfg.announce_interval_us);
        st->next_sweep_us = now + sweep_us;
        st->want_solicit = 0;
        st->self_blob_resend = 0;
        st->open_repeats = RANT_DISCOVERY_OPEN_REPEATS;
        st->next_repeat_us = now + RANT_DISCOVERY_OPEN_GAP_US;
        return i_rant_discovery_build(st, ask ? i_rant_discovery_req_flags(st) : 0u, 1, (uint8_t *)out, cap);
    }
    if (now >= st->next_sweep_us){
        st->next_sweep_us = now + sweep_us;
        i_rant_discovery_sweep(st, now);
    }
    if (st->slots_opened) i_rant_discovery_offer_slots(st, now);
    if (st->want_solicit){   /* the group solicit: our blob, and peers answer with theirs */
        st->want_solicit = 0;
        return i_rant_discovery_build(st, i_rant_discovery_req_flags(st), 1, (uint8_t *)out, cap);
    }
    if (st->open_repeats && now >= st->next_repeat_us){   /* for a peer that lost the opening and was assumed */
        st->open_repeats--;
        st->next_repeat_us = now + RANT_DISCOVERY_OPEN_GAP_US;
        return i_rant_discovery_build(st, 0, 1, (uint8_t *)out, cap);
    }
    if (now >= st->next_announce_us){
        int with_blob = st->self_blob_resend > 0;
        if (with_blob) st->self_blob_resend--;
        st->next_announce_us = now + st->cfg.announce_interval_us;
        return i_rant_discovery_build(st, 0, with_blob, (uint8_t *)out, cap);
    }
    return 0;
}

uint64_t i_rant_discovery_next_due_us(const i_RantDiscoveryState *st){
    uint64_t due; uint16_t i;
    if (!st || !st->started || st->want_solicit) return 0;
    /* next_announce_us is 0 after an advertise: announce now */
    due = st->next_announce_us < st->next_sweep_us ? st->next_announce_us : st->next_sweep_us;
    if (st->open_repeats && st->next_repeat_us < due) due = st->next_repeat_us;
    for (i = st->records.due_head; i != I_RANT_REC_NONE; i = st->records.recs[i].due_next)
        if (st->records.recs[i].due_us < due) due = st->records.recs[i].due_us;
    for (i = 0; i < st->n_loose; i++)
        if (st->loose[i].due_us < due) due = st->loose[i].due_us;
    return due;
}

void i_rant_discovery_set_meta(i_RantDiscoveryState *st, RantBytes meta, uint64_t now_us){
    if (!st || meta.len > st->meta_cap) return;   /* the node sizes meta_cap to fit */
    st->self_meta         = meta;
    st->self_meta_version++;
    st->changed_us        = now_us;
    st->self_blob_resend  = RANT_DISCOVERY_BLOB_RESEND;
    st->next_announce_us  = 0;   /* announce the change now */
}

uint32_t i_rant_discovery_meta_version(const i_RantDiscoveryState *st){
    return st ? st->self_meta_version : 0;
}

void i_rant_discovery_set_local_subnets(i_RantDiscoveryState *st, const i_RantDiscoverySubnet *nets, uint8_t n){
    uint8_t i, k = 0;
    if (!st) return;
    if (n > RANT_DISCOVERY_MAX_SUBNETS) n = RANT_DISCOVERY_MAX_SUBNETS;
    for (i = 0; i < n; i++){
        const uint8_t *m = nets[i].mask;
        if (!(m[0] | m[1] | m[2] | m[3])) continue;   /* an unknown prefix matches everything */
        st->local_nets[k++] = nets[i];
    }
    st->n_local_nets = k;
}

/* Each send settles its record or moves it past now, so a caller that drains this to 0 sends
 * every due record once. */
size_t i_rant_discovery_poll_targeted(i_RantDiscoveryState *st, uint64_t now, void *out, size_t cap,
                                    RantAddr *to){
    uint16_t i;
    st->now_us = now;
    if (!st->started) return 0;
    for (i = 0; i < st->n_loose; i++){
        const i_RantDiscoveryLoose l = st->loose[i];
        if (l.due_us > now) continue;
        memset(to, 0, sizeof *to);
        memcpy(to->ip, l.ip, 16); to->ip_len = l.ip_len; to->port = l.port;
        st->loose[i] = st->loose[--st->n_loose];
        return i_rant_discovery_build(st, RANT_DISCOVERY_FLAG_ONE, 1, (uint8_t *)out, cap);
    }
    for (i = st->records.due_head; i != I_RANT_REC_NONE; i = st->records.recs[i].due_next){
        i_RantDiscoveryRecord *rec = &st->records.recs[i];
        int ask = (rec->owed & I_RANT_OWE_ASK) != 0;
        if (rec->due_us > now) continue;
        i_rant_discovery_addr_of(rec, to);
        i_rant_records_settle(&st->records, i, I_RANT_OWE_REPLY);   /* an ask carries our blob too */
        if (ask){
            if (rec->ask_tries > 1u){
                rec->ask_tries--;
                i_rant_records_requeue(&st->records, i, now + RANT_DISCOVERY_ASK_RETRY_US);
            } else {
                rec->flags |= I_RANT_REC_ASK_MISSED;     /* an answer to this last send clears it */
                i_rant_records_settle(&st->records, i, I_RANT_OWE_ASK);
                if (i_rant_discovery_waiting(rec)) i_rant_discovery_refetch_done(st, rec);  /* its promised slot goes to the next */
            }
        }
        return i_rant_discovery_build(st, ask ? RANT_DISCOVERY_FLAG_REQ | RANT_DISCOVERY_FLAG_ONE
                                              : RANT_DISCOVERY_FLAG_ONE, 1, (uint8_t *)out, cap);
    }
    return 0;
}

uint16_t i_rant_discovery_record_cap(const i_RantDiscoveryState *st){ return st->records.cap; }

/* A slotted peer gets plain copies, which never start the receiver's own, so a pair's copies
 * end with the slot. Copies marked UNI keep a pair in touch where the group does not reach. */
int i_rant_discovery_copy_addr(const i_RantDiscoveryState *st, uint16_t i, RantAddr *out, int *uni){
    const i_RantDiscoveryRecord *rec = i_rant_discovery_rec(st, i);
    if (!rec || rec->state != I_RANT_REC_ACTIVE) return 0;
    *uni = i_rant_discovery_heard_uni(st, rec);
    if (!*uni && rec->slot == I_RANT_REC_NONE) return 0;
    i_rant_discovery_addr_of(rec, out);
    return 1;
}

void i_rant_discovery_mark_copy(uint8_t *datagram, size_t len, i_RantCopyMark mark){
    const uint8_t marks = RANT_DISCOVERY_FLAG_UNI | RANT_DISCOVERY_FLAG_ONE;
    uint8_t set = mark == I_RANT_COPY_UNI ? RANT_DISCOVERY_FLAG_UNI
                : mark == I_RANT_COPY_PLAIN ? RANT_DISCOVERY_FLAG_ONE : 0u;
    if (len <= 5) return;                    /* byte 5 is the flags, see the codec */
    datagram[5] = (uint8_t)((datagram[5] & ~marks) | set);
}

void i_rant_discovery_solicit(i_RantDiscoveryState *st){ if (st) st->want_solicit = 1; }

/* No version changes. The local side may now have a topic a held blob's interest matches,
 * or none it did. */
void i_rant_discovery_replay_peers(i_RantDiscoveryState *st){
    uint16_t s;
    if (!st) return;
    for (s=0;s<st->cap_peers;s++){
        uint16_t ri = st->slots[s].rec;
        const i_RantDiscoveryRecord *rec = i_rant_discovery_rec(st, ri);
        RantAddr addr;
        if (!rec || rec->state != I_RANT_REC_ACTIVE || i_rant_discovery_rejudge_at(st, ri)) continue;
        i_rant_discovery_addr_of(rec, &addr);
        i_rant_discovery_fire_up(st, rec, &addr);
    }
    if (st->slots_opened) i_rant_discovery_offer_slots(st, st->now_us);
}

int i_rant_discovery_rejudge(i_RantDiscoveryState *st, uint32_t id){
    int gave;
    if (!st) return 0;
    gave = i_rant_discovery_rejudge_at(st, i_rant_records_by_id(&st->records, id));
    if (st->slots_opened) i_rant_discovery_offer_slots(st, st->now_us);
    return gave;
}

/* DROPPED records linger for resume and are not members. */
uint16_t i_rant_discovery_peer_count(const i_RantDiscoveryState *st){ return st->records.live; }

uint16_t i_rant_discovery_slots_used(const i_RantDiscoveryState *st){
    uint16_t s, n = 0;
    for (s = 0; s < st->cap_peers; s++) if (st->slots[s].rec != I_RANT_REC_NONE) n++;
    return n;
}

uint32_t i_rant_discovery_seat_misses(const i_RantDiscoveryState *st){ return st->seat_misses; }

uint16_t i_rant_discovery_slots_wanted(const i_RantDiscoveryState *st){
    uint16_t i, n = 0;
    for (i = 0; i < st->records.cap; i++){
        const i_RantDiscoveryRecord *rec = i_rant_discovery_rec(st, i);
        if (rec && rec->state == I_RANT_REC_ACTIVE && (rec->slot != I_RANT_REC_NONE || rec->want != I_RANT_ADMIT_NO)) n++;
    }
    return n;
}

uint16_t i_rant_discovery_pending(const i_RantDiscoveryState *st){
    uint16_t i, n = 0;
    for (i = 0; i < st->records.cap; i++){
        const i_RantDiscoveryRecord *rec = i_rant_discovery_rec(st, i);
        if (rec && rec->state == I_RANT_REC_ACTIVE && rec->slot == I_RANT_REC_NONE
            && (rec->owed & I_RANT_OWE_FETCH)) n++;
    }
    return n;
}

void i_rant_discovery_refetch_unmatched(i_RantDiscoveryState *st, i_RantDiscoveryMaybe maybe, void *ctx){
    uint16_t i;
    for (i = 0; i < st->records.cap; i++){
        i_RantDiscoveryRecord *rec = i_rant_discovery_rec(st, i);
        if (!rec || rec->state != I_RANT_REC_ACTIVE || rec->slot != I_RANT_REC_NONE
            || rec->want != I_RANT_ADMIT_NO || (rec->flags & I_RANT_REC_TOO_BIG)) continue;
        /* judged by assumption, or advertising past the blob judged: nothing says it cannot meet */
        if (maybe && (rec->flags & I_RANT_REC_FILTER) && rec->adv_version <= rec->meta_version
            && !maybe(ctx, rec->filter, sizeof rec->filter)) continue;
        rec->flags |= I_RANT_REC_REFETCH;
        i_rant_discovery_fetch_check(st, i, st->now_us);
    }
}

int i_rant_discovery_heard_since(const i_RantDiscoveryState *st, uint64_t since_us){
    uint16_t i; int live = 0;
    for (i = 0; i < st->records.cap; i++){
        const i_RantDiscoveryRecord *rec = i_rant_discovery_rec(st, i);
        if (!rec || rec->state != I_RANT_REC_ACTIVE) continue;
        if (rec->last_heard_us < since_us) return 0;
        live = 1;
    }
    return live ? 1 : -1;
}

uint16_t i_rant_discovery_max_peers(const i_RantDiscoveryState *st){ return st->cap_peers; }

static void i_rant_discovery_view(const i_RantDiscoveryState *st, const i_RantDiscoveryRecord *rec,
                                  i_RantDiscoveryPeerView *out){
    const i_RantDiscoverySlot *slot = i_rant_discovery_slot_of(st, rec);
    memset(out, 0, sizeof *out);
    out->id = rec->local_id;
    memcpy(out->uuid, rec->uuid, 16);
    i_rant_discovery_addr_of(rec, &out->addr);
    out->liveness      = rec->state == I_RANT_REC_DROPPED ? RANT_PEER_DROPPED : RANT_PEER_ACTIVE;
    out->last_heard_us = rec->last_heard_us;
    out->name          = rant_string(rec->name_len ? rec->name : NULL, rec->name_len);
    out->adv_meta_version = rec->adv_version;
    if (!slot) return;
    out->meta          = rant_bytes(slot->meta_len ? slot->meta : NULL, slot->meta_len);
    out->meta_version  = rec->meta_version;
    out->user          = st->user_stride ? slot->user : NULL;
}

int i_rant_discovery_peer_at(const i_RantDiscoveryState *st, uint16_t s, i_RantDiscoveryPeerView *out){
    const i_RantDiscoveryRecord *rec;
    if (s >= st->cap_peers) return 0;
    rec = i_rant_discovery_rec(st, st->slots[s].rec);
    if (!rec) return 0;                      /* a DROPPED record still holds its slot */
    i_rant_discovery_view(st, rec, out);
    return 1;
}

int i_rant_discovery_known_at(const i_RantDiscoveryState *st, uint16_t i, i_RantDiscoveryPeerView *out){
    const i_RantDiscoveryRecord *rec = i_rant_discovery_rec(st, i);
    if (!rec) return 0;
    i_rant_discovery_view(st, rec, out);
    return 1;
}

/* DROPPED included. */
static i_RantDiscoveryRecord *i_rant_discovery_by_id(const i_RantDiscoveryState *st, uint32_t id){
    return i_rant_discovery_rec(st, i_rant_records_by_id(&st->records, id));
}

void *i_rant_discovery_peer_user(i_RantDiscoveryState *st, uint32_t id){
    const i_RantDiscoveryRecord *rec;
    const i_RantDiscoverySlot *slot;
    if (!st || !st->user_stride) return NULL;
    rec  = i_rant_discovery_by_id(st, id);
    slot = rec ? i_rant_discovery_slot_of(st, rec) : NULL;
    return slot ? slot->user : NULL;
}

int i_rant_discovery_addr_of_id(const i_RantDiscoveryState *st, uint32_t id, RantAddr *out){
    const i_RantDiscoveryRecord *rec = i_rant_discovery_by_id(st, id);
    if (!rec) return 0;
    i_rant_discovery_addr_of(rec, out);
    return 1;
}

RantBytes i_rant_discovery_peer_meta(const i_RantDiscoveryState *st, uint32_t id, uint32_t *version){
    const i_RantDiscoveryRecord *rec = i_rant_discovery_by_id(st, id);
    const i_RantDiscoverySlot *slot = rec ? i_rant_discovery_slot_of(st, rec) : NULL;
    if (version) *version = slot ? rec->meta_version : 0;
    if (!slot || !slot->meta_len) return rant_bytes(NULL, 0);
    return rant_bytes(slot->meta, slot->meta_len);
}

RantString i_rant_discovery_peer_name(const i_RantDiscoveryState *st, uint32_t id){
    const i_RantDiscoveryRecord *rec = i_rant_discovery_by_id(st, id);
    if (!rec) return rant_string(NULL, 0);                   /* unknown peer: .data NULL */
    return rant_string(rec->name, rec->name_len);            /* known: .data set, .len may be 0 */
}

const uint8_t *i_rant_discovery_peer_uuid(const i_RantDiscoveryState *st, uint32_t id){
    const i_RantDiscoveryRecord *rec = i_rant_discovery_by_id(st, id);
    return rec ? rec->uuid : NULL;
}

int i_rant_discovery_id_for_addr(const i_RantDiscoveryState *st, const uint8_t *ip, uint8_t ip_len,
                               uint16_t port, uint32_t *id){
    uint16_t i;
    const i_RantDiscoveryRecord *found = NULL;
    if (!ip || (ip_len != 4 && ip_len != 16)) return 0;
    for (i=0;i<st->cap_peers;i++){
        const i_RantDiscoveryRecord *rec = i_rant_discovery_rec(st, st->slots[i].rec);
        if (rec && rec->ip_len == ip_len && rec->port == port && memcmp(rec->ip, ip, ip_len) == 0){
            if (id) *id = rec->local_id;
            return 1;
        }
    }
    /* Else the data port on any recent path. One peer or none: an address two peers share
       attributes to neither. */
    for (i=0;i<st->cap_peers;i++){
        const i_RantDiscoveryRecord *rec = i_rant_discovery_rec(st, st->slots[i].rec);
        if (!rec || rec->state != I_RANT_REC_ACTIVE || rec->port != port) continue;
        if (!i_rant_discovery_on_path(st, &st->slots[i], ip, ip_len)) continue;
        if (found) return 0;
        found = rec;
    }
    if (!found) return 0;
    if (id) *id = found->local_id;
    return 1;
}

size_t i_rant_discovery_leave(i_RantDiscoveryState *st, void *out, size_t cap){
    return i_rant_discovery_build(st, RANT_DISCOVERY_FLAG_BYE, 0, (uint8_t *)out, cap);
}
