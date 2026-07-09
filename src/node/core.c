/* sans-IO NODE core: peer table + the discovery->transport lifecycle. No platform
 * access; a runtime drives it (see node/runtime.c) and does the IO. See node/core.h. */

#include "core.h"
#include "../common/arena.h"
#include <string.h>

/* dart_event_str + bounded appenders: format the node's app DartEvent as one line.
   No stdio, so it stays in the sans-IO core. (It covers the union event, including the
   peer/interest/mcast kinds the node adds; the transport keeps no formatter of its own.) */
static char *i_dart_event_append_str(char *p, char *end, const char *s){
    if (!s) return p;
    while (*s && p < end) *p++ = *s++;
    return p;
}
static char *i_dart_event_append_u64(char *p, char *end, uint64_t v){
    char tmp[20]; int n = 0;
    do { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v);
    while (n && p < end) *p++ = tmp[--n];
    return p;
}
static char *i_dart_event_append_addr(char *p, char *end, const DartEvent *ev){   /* dotted quad + :port (IPv4 only) */
    int i;
    for (i = 0; i < 4; i++){ if (i) p = i_dart_event_append_str(p,end,"."); p = i_dart_event_append_u64(p,end,ev->ip[i]); }
    p = i_dart_event_append_str(p,end,":"); return i_dart_event_append_u64(p,end,ev->port);
}
static char *i_dart_event_append_ch(char *p, char *end, const DartEvent *ev){   /* ch=<name> or ch=<index> */
    p = i_dart_event_append_str(p,end,"ch=");
    if (ev->channel_name) return i_dart_event_append_str(p,end,ev->channel_name);
    return i_dart_event_append_u64(p,end,ev->channel);
}
#ifndef DART_NO_DIAG   /* only the verbose error body below uses these two */
static char *i_dart_event_append_hex(char *p, char *end, uint64_t v){
    char tmp[16]; int n = 0;
    do { int d = (int)(v & 0xF); tmp[n++] = (char)(d < 10 ? '0'+d : 'a'+d-10); v >>= 4; } while (v);
    while (n && p < end) *p++ = tmp[--n];
    return p;
}
static char *i_dart_event_append_oserr(char *p, char *end, const DartEvent *ev){
    if (!ev->os_error) return p;
    p = i_dart_event_append_str(p,end," (os_err="); p = i_dart_event_append_u64(p,end,(uint64_t)(unsigned int)ev->os_error);
    return i_dart_event_append_str(p,end,")");
}
#endif

/* the DART_ERROR body, split out so the descriptive text can compile away under
   DART_NO_DIAG (leaving only the numeric error code) with no other change. */
static char *i_dart_event_error_str(char *p, char *end, const DartEvent *ev){
#ifdef DART_NO_DIAG
    p = i_dart_event_append_str(p,end,"error "); return i_dart_event_append_u64(p,end,(uint64_t)ev->error);
#else
    switch (ev->error){
    case DART_E_NAME_COLLISION:
        p=i_dart_event_append_str(p,end,"name-collision "); p=i_dart_event_append_ch(p,end,ev);
        p=i_dart_event_append_str(p,end," id=0x"); p=i_dart_event_append_hex(p,end,ev->identity);
        p=i_dart_event_append_str(p,end,": match refused"); break;
    case DART_E_QOS_INCOMPATIBLE:
        p=i_dart_event_append_str(p,end,"qos-incompatible "); p=i_dart_event_append_ch(p,end,ev);
        p=i_dart_event_append_str(p,end," from id="); p=i_dart_event_append_u64(p,end,ev->peer);
        p=i_dart_event_append_str(p,end,": reliable subscriber refused best-effort publisher"); break;
    case DART_E_SCHEMA_MISMATCH:
        p=i_dart_event_append_str(p,end,"schema-mismatch "); p=i_dart_event_append_ch(p,end,ev);
        p=i_dart_event_append_str(p,end," peer id="); p=i_dart_event_append_u64(p,end,ev->peer);
        p=i_dart_event_append_str(p,end,": incompatible schemas, refused"); break;
    case DART_E_INTEREST_OVERFLOW:
        p=i_dart_event_append_str(p,end,"interest-overflow peer id="); p=i_dart_event_append_u64(p,end,ev->peer);
        p=i_dart_event_append_str(p,end,": "); p=i_dart_event_append_u64(p,end,ev->lost_count);
        p=i_dart_event_append_str(p,end," matched topics beyond our alias table (raise DART_META_MAX_IDS)"); break;
    case DART_E_META_TRUNCATED_INTEREST:
        p=i_dart_event_append_str(p,end,"meta-truncated: interest list dropped (announce overlay full)"); break;
    case DART_E_META_TRUNCATED_SCHEMA:
        p=i_dart_event_append_str(p,end,"meta-truncated: schema section dropped (announce overlay full)"); break;
    case DART_E_PEER_META_TOO_BIG:
        p=i_dart_event_append_str(p,end,"peer-meta-too-big id="); p=i_dart_event_append_u64(p,end,ev->peer);
        if (ev->ip_len==4){ p=i_dart_event_append_str(p,end," at "); p=i_dart_event_append_addr(p,end,ev); }
        p=i_dart_event_append_str(p,end,": "); p=i_dart_event_append_u64(p,end,ev->too_big_bytes);
        p=i_dart_event_append_str(p,end," byte blob exceeds our capacity, refused"); break;
    case DART_E_MSG_TOO_BIG:
        p=i_dart_event_append_str(p,end,"msg-too-big "); p=i_dart_event_append_ch(p,end,ev);
        p=i_dart_event_append_str(p,end," from id="); p=i_dart_event_append_u64(p,end,ev->peer);
        p=i_dart_event_append_str(p,end," ("); p=i_dart_event_append_u64(p,end,ev->too_big_bytes);
        p=i_dart_event_append_str(p,end," bytes), skipped"); break;
    case DART_E_PEER_REFUSED:
        p=i_dart_event_append_str(p,end,"peer-refused at "); p=i_dart_event_append_addr(p,end,ev);
        p=i_dart_event_append_str(p,end,": peer table full of active peers (raise max_peers)"); break;
    case DART_E_EVICTED_UNSENT:
        p=i_dart_event_append_str(p,end,"evicted-unsent "); p=i_dart_event_append_ch(p,end,ev);
        p=i_dart_event_append_str(p,end," seqno "); p=i_dart_event_append_u64(p,end,ev->lost_first);
        p=i_dart_event_append_str(p,end,".."); p=i_dart_event_append_u64(p,end,ev->lost_first + ev->lost_count - 1);
        p=i_dart_event_append_str(p,end,": send burst outran the TX drain"); break;
    case DART_E_OOM:
        p=i_dart_event_append_str(p,end,"out-of-memory");
        if (ev->too_big_bytes){ p=i_dart_event_append_str(p,end,": "); p=i_dart_event_append_u64(p,end,ev->too_big_bytes);
                                p=i_dart_event_append_str(p,end," bytes needed"); }
        break;
    case DART_E_PLATFORM:
        p=i_dart_event_append_str(p,end,"platform net init failed"); break;
    case DART_E_SOCKET:
        p=i_dart_event_append_str(p,end,"socket open failed"); p=i_dart_event_append_oserr(p,end,ev); break;
    case DART_E_BIND:
        p=i_dart_event_append_str(p,end,"bind failed on port "); p=i_dart_event_append_u64(p,end,ev->port);
        p=i_dart_event_append_oserr(p,end,ev); break;
    case DART_E_MCAST_JOIN:
        p=i_dart_event_append_str(p,end,"multicast join failed"); p=i_dart_event_append_oserr(p,end,ev); break;
    case DART_E_SEND:
        p=i_dart_event_append_str(p,end,"send failed to id="); p=i_dart_event_append_u64(p,end,ev->peer);
        p=i_dart_event_append_oserr(p,end,ev); break;
    case DART_E_RECV:
        p=i_dart_event_append_str(p,end,"recv failed"); p=i_dart_event_append_oserr(p,end,ev); break;
    case DART_E_POLL:
        p=i_dart_event_append_str(p,end,"poll failed"); p=i_dart_event_append_oserr(p,end,ev); break;
    case DART_E_WAKER:
        p=i_dart_event_append_str(p,end,"cross-thread waker unavailable (wakes at next tick)"); break;
    case DART_E_NONE: default:
        p=i_dart_event_append_str(p,end,"error"); break;
    }
    return p;
#endif
}

const char *dart_event_str(const DartEvent *ev, char *buf, size_t cap){
    char *p, *end;
    if (!buf || !cap) return buf;
    p = buf; end = buf + cap - 1;                  /* reserve one byte for the NUL */
    switch (ev->kind){
    case DART_PEER_UP:
        p = i_dart_event_append_str(p,end,"peer-up id="); p = i_dart_event_append_u64(p,end,ev->peer);
        if (ev->ip_len == 4){ p = i_dart_event_append_str(p,end," at "); p = i_dart_event_append_addr(p,end,ev); }
        break;
    case DART_PEER_DOWN:
        p = i_dart_event_append_str(p,end,"peer-down id="); p = i_dart_event_append_u64(p,end,ev->peer);
        break;
    case DART_PEER_INTEREST:
        p = i_dart_event_append_str(p,end,"interest id="); p = i_dart_event_append_u64(p,end,ev->peer);
        p = i_dart_event_append_str(p,end," publish-to="); p = i_dart_event_append_u64(p,end,ev->publish_topics);
        p = i_dart_event_append_str(p,end," topics, receive-from="); p = i_dart_event_append_u64(p,end,ev->receive_topics);
        p = i_dart_event_append_str(p,end," topics");
        break;
    case DART_MSG_LOST:
        p = i_dart_event_append_str(p,end,"msg-lost "); p = i_dart_event_append_ch(p,end,ev);
        p = i_dart_event_append_str(p,end," from id="); p = i_dart_event_append_u64(p,end,ev->peer);
        p = i_dart_event_append_str(p,end," seqno "); p = i_dart_event_append_u64(p,end,ev->lost_first);
        p = i_dart_event_append_str(p,end,".."); p = i_dart_event_append_u64(p,end,ev->lost_first + ev->lost_count - 1);
        break;
    case DART_ERROR:
        p = i_dart_event_error_str(p, end, ev);
        break;
    }
    *p = '\0';                                     /* p <= end = buf+cap-1, in range */
    return buf;
}

/* the node core's per-peer transport-lifecycle state, kept in the discovery peer's user
   scratch (so the node holds NO peer table of its own). added = wired into the transport
   yet (dart_transport_peer_add called); dormant = discovery DROPPED it, kept for a same-incarnation
   resume. Discovery zeroes this when a new UUID takes the slot, preserves it on resume. */
typedef struct { uint8_t added; uint8_t dormant; } i_DartNodePeerExtra;

/* schema state for the gate + delivery: peer schemas interned by hash (parsed once),
   reader views cached per (peer schema, channel), and the per-(peer, channel) schema a
   delivered message decodes with. All flat, linearly scanned (counts stay small), and
   allocated through the injected hook so they survive an arena migrate untouched. An
   interned/rebased schema lives until node close; a stale map pointer therefore never
   dangles. */
typedef struct { uint64_t hash; DartSchema *parsed; } i_DartNodeSchemaIntern;
typedef struct { uint64_t hash; uint16_t channel; DartSchema *rebased; } i_DartNodeSchemaBind;
typedef struct { uint32_t peer; uint16_t channel; const DartSchema *schema; } i_DartNodePeerSchema;

struct i_DartNodeCore {
    DartTransportState           *transport;
    DartDiscoveryState  *discovery;   /* the peer table (id<->addr, name, user scratch) we delegate to */
    DartEventFn         on_event;
    void                 *user;
    int                   oob_capable;
    uint8_t               oob_host[16];
    uint8_t              *meta_buf;    /* our outgoing discovery announce blob */
    uint16_t              meta_cap;
    uint16_t              meta_len;
    uint16_t              frag_size;   /* baked into the overlay */
    DartMetaSchema       *chan_schemas;  /* per-channel schema advertisement (hash 0 = none) */
    const DartSchema    **chan_compiled; /* per-channel parsed schema (the gate's local side) */
    uint16_t              n_channels;    /* sizes the per-channel arrays (and the meta buffer) */
    DartAllocFn           alloc;         /* backs the schema state below (may be NULL) */
    void                 *alloc_user;
    DartBytes             applying_meta; /* overlay being applied right now (schema-gate context) */
    uint32_t              applying_peer;
    uint8_t              *detail_buf;    /* detail-response scratch, hook-allocated + grown on
                                            demand (stable across a migrate; pool reset frees it) */
    uint32_t              detail_cap;
    i_DartNodeSchemaIntern *interned;     uint32_t n_interned,     cap_interned;
    i_DartNodeSchemaBind   *binds;        uint32_t n_binds,        cap_binds;
    i_DartNodePeerSchema   *peer_schemas; uint32_t n_peer_schemas, cap_peer_schemas;
};

/* grow one of the flat schema arrays through the hook; 1 + *arr/cap updated, or 0 */
static int i_dart_node_core_array_reserve(i_DartNodeCore *c, void **arr, uint32_t *cap,
                                          uint32_t need, size_t elem){
    void *na; uint32_t ncap;
    if (need <= *cap) return 1;
    if (!c->alloc) return 0;
    ncap = *cap ? *cap * 2u : 8u;
    if (ncap < need) ncap = need;
    na = c->alloc(c->alloc_user, *arr, (size_t)ncap * elem);
    if (!na) return 0;
    *arr = na; *cap = ncap;
    return 1;
}

uint16_t i_dart_node_core_peer_user_bytes(void){ return (uint16_t)sizeof(i_DartNodePeerExtra); }
void i_dart_node_core_bind_discovery(i_DartNodeCore *c, DartDiscoveryState *discovery){ c->discovery = discovery; }

/* arena layout: the core struct, the announce-blob buffer (fixed mode only: with an alloc
   hook the blob is hook-allocated at its ACTUAL size and grown at build time, instead of
   reserving dart_meta_capacity's worst case), then the per-channel schema registry (no peer
   table -- that lives in the discovery core). One sequence so measure and build agree. */
static void i_dart_node_core_layout(i_DartBump *b, uint16_t n_channels, int dynamic_meta,
                              i_DartNodeCore **out_c, uint8_t **out_meta,
                              DartMetaSchema **out_schemas, const DartSchema ***out_compiled){
    i_DartNodeCore *c       = (i_DartNodeCore*)i_dart_bump_take(b, sizeof(struct i_DartNodeCore), 16);
    uint8_t *meta           = dynamic_meta ? NULL
                            : (uint8_t*)   i_dart_bump_take(b, dart_meta_capacity(n_channels), 16);
    DartMetaSchema *schemas = (DartMetaSchema*)i_dart_bump_take(b, (size_t)n_channels*sizeof(DartMetaSchema), 16);
    const DartSchema **compiled = (const DartSchema**)i_dart_bump_take(b, (size_t)n_channels*sizeof(DartSchema*), 16);
    if (out_c)        *out_c        = c;
    if (out_meta)     *out_meta     = meta;
    if (out_schemas)  *out_schemas  = schemas;
    if (out_compiled) *out_compiled = compiled;
}

size_t i_dart_node_core_required_memory(uint16_t n_channels, int dynamic_meta){
    i_DartBump b; memset(&b, 0, sizeof b);
    i_dart_node_core_layout(&b, n_channels, dynamic_meta, NULL, NULL, NULL, NULL);
    return b.offset + 16u;   /* slack to align the caller's mem up to base */
}

i_DartNodeCore *i_dart_node_core_init(void *mem, size_t cap, const i_DartNodeCoreConfig *cfg){
    i_DartBump b; i_DartNodeCore *c; uint8_t *base, *meta;
    DartMetaSchema *schemas; const DartSchema **compiled;
    if (!mem || !cfg || !cfg->transport) return NULL;
    if (cap < i_dart_node_core_required_memory(cfg->n_channels, cfg->alloc != NULL)) return NULL;
    base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    memset(&b, 0, sizeof b); b.base = base; b.cap = cap - (size_t)(base - (uint8_t*)mem);
    i_dart_node_core_layout(&b, cfg->n_channels, cfg->alloc != NULL, &c, &meta, &schemas, &compiled);

    memset(c, 0, sizeof *c);
    c->transport     = cfg->transport;
    c->discovery     = cfg->discovery;     /* may be NULL now, bound via bind_discovery later */
    c->on_event      = cfg->on_event;      c->user = cfg->user;
    c->alloc         = cfg->alloc;         c->alloc_user = cfg->alloc_user;
    c->oob_capable   = cfg->oob_capable;
    memcpy(c->oob_host, cfg->oob_host, 16);
    c->meta_buf      = meta;                                            /* dynamic: NULL until the first build */
    c->meta_cap      = meta ? dart_meta_capacity(cfg->n_channels) : 0;
    c->frag_size     = cfg->frag_size;
    c->chan_schemas  = schemas;
    c->chan_compiled = compiled;
    c->n_channels    = cfg->n_channels;
    memset(schemas, 0, (size_t)cfg->n_channels * sizeof *schemas);
    memset(compiled, 0, (size_t)cfg->n_channels * sizeof *compiled);
    return c;
}

/* Relocate the sans-IO node core into a bigger block at grown counts. No peer table (it
 * lives in discovery); a struct copy carries the scalars. The transport, discovery, and
 * announce-blob pointers are re-pointed by the caller after those move. */
i_DartNodeCore *i_dart_node_core_migrate(i_DartNodeCore *old, void *new_mem, size_t new_cap,
                                       uint16_t new_n_channels){
    i_DartBump b; i_DartNodeCore *c; uint8_t *base, *meta;
    DartMetaSchema *schemas; const DartSchema **compiled;
    uint16_t keep;
    if (!old) return NULL;
    if (new_cap < i_dart_node_core_required_memory(new_n_channels, old->alloc != NULL)) return NULL;
    base = (uint8_t*)(((uintptr_t)new_mem + 15u) & ~(uintptr_t)15u);
    memset(&b, 0, sizeof b); b.base = base; b.cap = new_cap - (size_t)(base - (uint8_t*)new_mem);
    i_dart_node_core_layout(&b, new_n_channels, old->alloc != NULL, &c, &meta, &schemas, &compiled);
    *c = *old;                          /* scalars + transport/discovery ptrs (caller re-points);
                                           the hook-allocated schema arrays ride along untouched */
    if (meta){                          /* fixed mode: caller rebuilds the blob into the new slot */
        c->meta_buf = meta;
        c->meta_cap = dart_meta_capacity(new_n_channels);
    }                                   /* dynamic: the hook-allocated blob is stable, carried by *c = *old
                                           (the caller's rebuild grows it if the new counts need more) */
    keep = old->n_channels < new_n_channels ? old->n_channels : new_n_channels;
    memset(schemas, 0, (size_t)new_n_channels * sizeof *schemas);
    memcpy(schemas, old->chan_schemas, (size_t)keep * sizeof *schemas);   /* wire views stay valid: the
                                          DartSchema blocks live outside the arena and do not move */
    memset(compiled, 0, (size_t)new_n_channels * sizeof *compiled);
    memcpy(compiled, old->chan_compiled, (size_t)keep * sizeof *compiled);
    c->chan_schemas  = schemas;
    c->chan_compiled = compiled;
    c->n_channels    = new_n_channels;
    return c;
}

void i_dart_node_core_set_channel_schema(i_DartNodeCore *c, uint16_t channel,
                                         const DartSchema *schema){
    if (!c || channel >= c->n_channels) return;
    c->chan_compiled[channel]     = schema;
    c->chan_schemas[channel].hash = schema ? dart_schema_hash(schema) : 0;
    c->chan_schemas[channel].wire = schema ? dart_schema_wire(schema) : dart_bytes(NULL, 0);
}

/* ---- the schema gate + delivery binding ---------------------------------------------- */

/* a peer schema, parsed once per distinct hash. The claimed hash must equal the wire's
   real hash, or a lying peer could poison the intern for every honest one. NULL when the
   wire is absent (hash-only advert) or malformed. */
static DartSchema *i_dart_node_core_intern(i_DartNodeCore *c, uint64_t hash, DartBytes wire){
    uint32_t i; DartSchema *p;
    for (i = 0; i < c->n_interned; i++)
        if (c->interned[i].hash == hash) return c->interned[i].parsed;
    if (!wire.data || wire.len == 0 || !c->alloc) return NULL;
    p = dart_schema_parse(wire.data, wire.len, c->alloc, c->alloc_user);
    if (!p) return NULL;
    if (dart_schema_hash(p) != hash ||
        !i_dart_node_core_array_reserve(c, (void**)&c->interned, &c->cap_interned,
                                        c->n_interned + 1u, sizeof *c->interned)){
        dart_schema_free(p, c->alloc, c->alloc_user);
        return NULL;
    }
    c->interned[c->n_interned].hash = hash;
    c->interned[c->n_interned].parsed = p;
    c->n_interned++;
    return p;
}

/* the reader view for (writer schema, channel): our fields on their layout, cached */
static DartSchema *i_dart_node_core_bind(i_DartNodeCore *c, uint64_t hash, uint16_t channel,
                                         const DartSchema *ours, const DartSchema *pub){
    uint32_t i; DartSchema *rb;
    for (i = 0; i < c->n_binds; i++)
        if (c->binds[i].hash == hash && c->binds[i].channel == channel) return c->binds[i].rebased;
    rb = dart_schema_rebase(ours, pub, c->alloc, c->alloc_user);
    if (!rb) return NULL;
    if (!i_dart_node_core_array_reserve(c, (void**)&c->binds, &c->cap_binds,
                                        c->n_binds + 1u, sizeof *c->binds)){
        dart_schema_free(rb, c->alloc, c->alloc_user);
        return NULL;
    }
    c->binds[c->n_binds].hash = hash;
    c->binds[c->n_binds].channel = channel;
    c->binds[c->n_binds].rebased = rb;
    c->n_binds++;
    return rb;
}

/* the delivery map: which schema decodes (peer, channel). NULL entries are stored too
   (they overwrite an older binding when a peer re-advertises without a schema). */
static void i_dart_node_core_peer_schema_set(i_DartNodeCore *c, uint32_t peer, uint16_t channel,
                                             const DartSchema *schema){
    uint32_t i;
    for (i = 0; i < c->n_peer_schemas; i++)
        if (c->peer_schemas[i].peer == peer && c->peer_schemas[i].channel == channel){
            c->peer_schemas[i].schema = schema;
            return;
        }
    if (!schema) return;
    if (!i_dart_node_core_array_reserve(c, (void**)&c->peer_schemas, &c->cap_peer_schemas,
                                        c->n_peer_schemas + 1u, sizeof *c->peer_schemas)) return;
    c->peer_schemas[c->n_peer_schemas].peer = peer;
    c->peer_schemas[c->n_peer_schemas].channel = channel;
    c->peer_schemas[c->n_peer_schemas].schema = schema;
    c->n_peer_schemas++;
}

/* drop a peer's map entries (GONE, or its id recycled onto a new peer) */
static void i_dart_node_core_peer_schema_clear(i_DartNodeCore *c, uint32_t peer){
    uint32_t i = 0;
    while (i < c->n_peer_schemas){
        if (c->peer_schemas[i].peer == peer)
            c->peer_schemas[i] = c->peer_schemas[--c->n_peer_schemas];   /* swap-remove */
        else i++;
    }
}

const DartSchema *i_dart_node_core_msg_schema(i_DartNodeCore *c, uint32_t peer, uint16_t channel){
    uint32_t i;
    if (!c) return NULL;
    for (i = 0; i < c->n_peer_schemas; i++)
        if (c->peer_schemas[i].peer == peer && c->peer_schemas[i].channel == channel)
            return c->peer_schemas[i].schema;
    return NULL;
}

int i_dart_node_core_schema_check(i_DartNodeCore *c, uint16_t channel, uint16_t alias,
                                  int peer_is_pub){
    const DartSchema *ours = (channel < c->n_channels) ? c->chan_compiled[channel] : NULL;
    uint64_t hash = 0; DartBytes wire = dart_bytes(NULL, 0);
    int peer_has = c->applying_meta.data
                && dart_meta_schema(c->applying_meta, alias, &hash, &wire);
    if (peer_is_pub){                                   /* their publish entry: we would read */
        if (!ours){                                     /* generic reader: accept, decode with theirs */
            i_dart_node_core_peer_schema_set(c, c->applying_peer, channel,
                peer_has ? i_dart_node_core_intern(c, hash, wire) : NULL);
            return 1;
        }
        if (!peer_has) return 0;                        /* typed reader refuses an untyped writer */
        if (hash == dart_schema_hash(ours)){            /* identical schema: our own view works */
            i_dart_node_core_peer_schema_set(c, c->applying_peer, channel, ours);
            return 1;
        }
        {   DartSchema *pub = i_dart_node_core_intern(c, hash, wire);   /* need the wire to verify */
            DartSchema *view;
            if (!pub || !dart_schema_subset(ours, pub)) return 0;
            view = i_dart_node_core_bind(c, hash, channel, ours, pub);
            if (!view) return 0;                        /* OOM: refuse rather than misdecode */
            i_dart_node_core_peer_schema_set(c, c->applying_peer, channel, view);
            return 1;
        }
    } else {                                            /* their subscribe entry: we would write */
        if (!peer_has) return 1;                        /* a generic reader takes anything */
        if (!ours) return 0;                            /* typed reader refuses our raw channel */
        if (hash == dart_schema_hash(ours)) return 1;
        {   DartSchema *sub = i_dart_node_core_intern(c, hash, wire);
            return sub != NULL && dart_schema_subset(sub, ours);
        }
    }
}

/* (Re)build our discovery OVERLAY (frag size + OOB host + interest) from the core's
   current fields. The codec lives in the transport core; the OOB fields default to 0 in a
   non-SHM build. The node NAME is not here: the runtime hands it to discovery directly. */
uint16_t i_dart_node_core_build_meta(i_DartNodeCore *c){
    if (c->alloc){   /* dynamic: (re)size the blob buffer to the exact content first */
        uint16_t need = dart_transport_meta_size(c->transport, c->chan_schemas);
        if (need > c->meta_cap){
            uint8_t *nb = (uint8_t*)c->alloc(c->alloc_user, c->meta_buf, need);
            if (!nb) return c->meta_len;   /* OOM: keep the previous blob (stale but consistent) */
            c->meta_buf = nb; c->meta_cap = need;
        }
    }
    c->meta_len = dart_transport_meta_build(c->transport, c->meta_buf, c->meta_cap,
                                  c->frag_size, c->oob_capable, c->oob_host,
                                  c->chan_schemas);
    return c->meta_len;
}

DartBytes i_dart_node_core_meta(i_DartNodeCore *c){
    return dart_bytes(c->meta_buf, c->meta_len);
}

/* Answer a peer's DETAIL_REQ: validate kind + domain, then build the response (the codec
   is dart_transport_detail_* in the transport core) into the core's grown scratch buffer.
   Stateless: a pure read of channel + schema state, idempotent under duplicate requests.
   Returns the response bytes to send to the request's source, or {NULL,0} when not
   answerable (malformed, wrong domain, no alloc hook, or OOM: the requester just
   re-asks). An all-skipped response (header only) is still sent: it tells the requester
   those aliases are not advertised at our current version. */
DartBytes i_dart_node_core_detail_respond(i_DartNodeCore *c, uint16_t domain, DartBytes req){
    size_t need, len;
    if (!c || !c->alloc || !c->discovery) return dart_bytes(NULL, 0);
    if (dart_detail_kind(req) != DART_DETAIL_REQ || dart_detail_domain(req) != domain)
        return dart_bytes(NULL, 0);
    need = dart_transport_detail_resp_size(c->transport, c->chan_schemas, req);
    if (!need) return dart_bytes(NULL, 0);
    if (need > 65000u) need = 65000u;   /* one datagram: the build truncates at an entry
                                           boundary and the requester re-requests the rest */
    if (need > c->detail_cap){
        uint8_t *nb = (uint8_t*)c->alloc(c->alloc_user, c->detail_buf, need);
        if (!nb) return dart_bytes(NULL, 0);
        c->detail_buf = nb; c->detail_cap = (uint32_t)need;
    }
    len = dart_transport_detail_respond(c->transport, c->chan_schemas,
                                        dart_discovery_meta_version(c->discovery), req,
                                        c->detail_buf, need);
    return dart_bytes(c->detail_buf, len);
}

#ifdef DART_SHM
/* a peer can receive our out-of-band (SHM) payload iff we are OOB-capable, it
 * advertised an OOB host in its meta blob, and that host equals ours (same kernel).
 * Set the transport's per-peer flag. The core knows nothing of SHM beyond this. */
static void i_dart_node_core_set_peer_oob(i_DartNodeCore *c, uint32_t id, DartBytes meta){
    uint8_t host[16];
    int oob = c->oob_capable && dart_meta_shm(meta, host) &&
              memcmp(host, c->oob_host, 16) == 0;
    dart_transport_peer_set_shm(c->transport, id, oob);
}
#else
#define i_dart_node_core_set_peer_oob(c, id, meta) ((void)0)
#endif

/* fire an info peer event (PEER_UP / PEER_DOWN). */
static void i_dart_node_core_fire(i_DartNodeCore *c, DartEventKind kind, uint32_t id,
                            const DartDiscoveryAddr *addr){
    DartEvent ev;
    if (!c->on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind; ev.peer = id; ev.user = c->user;
    if (addr){ memcpy(ev.ip, addr->ip, 16); ev.ip_len = addr->ip_len; ev.port = addr->port; }
    c->on_event(&ev);
}

/* fire a DART_ERROR event (err = which). too_big carries the OOM/meta-too-big byte count. */
static void i_dart_node_core_fire_error(i_DartNodeCore *c, DartErrorKind err, uint32_t id,
                            const DartDiscoveryAddr *addr, uint64_t too_big){
    DartEvent ev;
    if (!c->on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = DART_ERROR; ev.error = err; ev.peer = id; ev.user = c->user; ev.too_big_bytes = too_big;
    if (addr){ memcpy(ev.ip, addr->ip, 16); ev.ip_len = addr->ip_len; ev.port = addr->port; }
    c->on_event(&ev);
}

/* fired whenever a peer's interest list is (re)applied to the transport: reports how
 * many topics now flow each way, so an app/example can watch a connection form. */
static void i_dart_node_core_fire_interest(i_DartNodeCore *c, uint32_t id){
    DartEvent ev; uint16_t publish_to = 0, receive_from = 0;
    if (!c->on_event) return;
    dart_transport_peer_match_counts(c->transport, id, &publish_to, &receive_from);
    memset(&ev, 0, sizeof ev);
    ev.kind = DART_PEER_INTEREST; ev.peer = id;
    ev.publish_topics = publish_to; ev.receive_topics = receive_from;
    ev.user = c->user;
    c->on_event(&ev);
}

/* the peer's transport-lifecycle state lives in the discovery peer's user scratch; the
 * node core keeps no table of its own. NULL only before discovery is bound (no events yet). */
static i_DartNodePeerExtra *i_dart_node_core_peer_extra(i_DartNodeCore *c, uint32_t id){
    return (i_DartNodePeerExtra*)dart_discovery_peer_user(c->discovery, id);
}

static void i_dart_node_core_peer_up(i_DartNodeCore *c, uint32_t id, const DartDiscoveryAddr *addr,
                            DartBytes meta){
    i_DartNodePeerExtra *ex = i_dart_node_core_peer_extra(c, id);
    uint16_t frag = dart_meta_frag(meta);
    DartBytes interest = dart_meta_interest(meta);
    if (!ex) return;                                  /* discovery not bound / no scratch */
    c->applying_meta = meta; c->applying_peer = id;   /* schema-gate context for the applies below */
    if (!ex->added){                                  /* brand-new peer: wire it into the transport */
        i_dart_node_core_peer_schema_clear(c, id);    /* its id may be recycled: no stale bindings */
        dart_transport_peer_add(c->transport, id, frag);          /* blob carries frag + pub/sub interest */
        ex->added = 1; ex->dormant = 0;
        i_dart_node_core_set_peer_oob(c, id, meta);
        i_dart_node_core_fire(c, DART_PEER_UP, id, addr);
        if (interest.data){ dart_transport_apply_peer_interest(c->transport, id, interest);
                       i_dart_node_core_fire_interest(c, id); }
    } else {                                          /* known peer: addr/interest update */
        dart_transport_peer_set_frag(c->transport, id, frag);
        i_dart_node_core_set_peer_oob(c, id, meta);
        if (interest.data){ dart_transport_apply_peer_interest(c->transport, id, interest);
                       i_dart_node_core_fire_interest(c, id); }
        if (ex->dormant){    /* a DROPPED peer's same incarnation returned: resume */
            ex->dormant = 0;
            dart_transport_peer_resume(c->transport, id);   /* keeps reader position; writer fills any gap */
            i_dart_node_core_fire(c, DART_PEER_UP, id, addr);
        }
    }
    c->applying_meta = dart_bytes(NULL, 0); c->applying_peer = 0;
}

static void i_dart_node_core_peer_down(i_DartNodeCore *c, uint32_t id, DartDiscoveryDownReason reason){
    i_DartNodePeerExtra *ex = i_dart_node_core_peer_extra(c, id);   /* discovery frees the slot AFTER this event */
    if (reason == DART_DISCOVERY_DROP){
        /* fell silent: keep transport state so a same-incarnation return resumes
           losslessly; just stop flow-controlling it and tell the app once */
        if (ex && ex->added && !ex->dormant){
            ex->dormant = 1;
            dart_transport_peer_dormant(c->transport, id);
            i_dart_node_core_fire(c, DART_PEER_DOWN, id, NULL);
        }
    } else {   /* GONE: said BYE or its slot was reclaimed; free the transport state */
        int notify = (ex && ex->added && !ex->dormant);   /* active->gone: app not yet told */
        dart_transport_peer_remove(c->transport, id);   /* discovery zeroes the scratch on slot reuse */
        i_dart_node_core_peer_schema_clear(c, id);      /* the id may be reassigned */
        if (notify) i_dart_node_core_fire(c, DART_PEER_DOWN, id, NULL);
    }
}

static void i_dart_node_core_peer_refused(i_DartNodeCore *c, const DartDiscoveryAddr *addr){
    i_dart_node_core_fire_error(c, DART_E_PEER_REFUSED, 0, addr, 0);
}

/* The discovery core's on_event sink (cfg.user = this core): demux the generic
 * DartDiscoveryEvent into the lifecycle handlers above, which fire the app DartEvents. */
void i_dart_node_core_on_disc_event(const DartDiscoveryEvent *ev){
    i_DartNodeCore *c = (i_DartNodeCore*)ev->user;
    switch (ev->kind){
        case DART_DISCOVERY_PEER_UP:
            i_dart_node_core_peer_up(c, ev->peer, &ev->addr, ev->meta);  /* name lives in discovery */
            break;
        case DART_DISCOVERY_PEER_DOWN:
            i_dart_node_core_peer_down(c, ev->peer, ev->reason);
            break;
        case DART_DISCOVERY_PEER_REFUSED:
            i_dart_node_core_peer_refused(c, &ev->addr);
            break;
        case DART_DISCOVERY_META_TOO_BIG:
            i_dart_node_core_fire_error(c, DART_E_PEER_META_TOO_BIG, ev->peer, &ev->addr, ev->meta.len);
            break;
        default: break;
    }
}

int i_dart_node_core_resolve(i_DartNodeCore *c, uint32_t to, i_DartNodeDest *out){
    DartDiscoveryAddr a;
    memset(out, 0, sizeof *out);
    if (!dart_discovery_addr_of_id(c->discovery, to, &a)) return 0;   /* peer vanished */
    memcpy(out->ip, a.ip, 16);
    out->ip_len = a.ip_len;
    out->port   = a.port;
    return 1;
}

int i_dart_node_core_id_for_addr(i_DartNodeCore *c, const uint8_t ip[4], uint16_t port, uint32_t *id){
    return dart_discovery_id_for_addr(c->discovery, ip, 4, port, id);
}

DartString i_dart_node_core_peer_name(i_DartNodeCore *c, uint32_t id){
    DartString name = dart_discovery_peer_name(c->discovery, id);
    if (!name.data) return name;                          /* not a known peer: {NULL,0} */
    if (name.len == 0) name = dart_cstr("unknown-peer");  /* known but unnamed */
    return name;
}

uint16_t i_dart_node_core_max_peers(i_DartNodeCore *c){ return dart_discovery_max_peers(c->discovery); }

int i_dart_node_core_peer_at(i_DartNodeCore *c, uint16_t slot, uint32_t *id,
                           uint8_t ip[16], uint8_t *ip_len, uint16_t *port){
    DartDiscoveryPeer v;
    if (!dart_discovery_peer_at(c->discovery, slot, &v)) return 0;
    if (id)     *id = v.id;
    if (ip)     memcpy(ip, v.addr.ip, 16);
    if (ip_len) *ip_len = v.addr.ip_len;
    if (port)   *port = v.addr.port;
    return 1;
}

uint16_t dart_node_peer_frag(const DartDiscoveryPeer *peer){
    return (peer && peer->meta.data) ? dart_meta_frag(peer->meta) : 0;
}

int dart_node_peer_interest_next(const DartDiscoveryPeer *peer,
                                 DartInterestIter *it, DartTopic *out){
    if (!peer || !peer->meta.data) return 0;
    return dart_meta_interest_next(peer->meta, it, out);
}

int dart_node_peer_schema(const DartDiscoveryPeer *peer, uint16_t alias,
                          uint64_t *hash, DartBytes *wire){
    if (!peer || !peer->meta.data){
        if (hash) *hash = 0;
        if (wire) *wire = dart_bytes(NULL, 0);
        return 0;
    }
    return dart_meta_schema(peer->meta, alias, hash, wire);
}
