/* The sans-IO node core. A runtime drives it and does the IO. The rules are in spec/node.md. */

#include "core.h"
#include "../common/arena.h"
#include <string.h>

/* dart_event_str and its bounded appenders. No stdio, so it stays in the sans-IO core. */
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
/* dotted quad and port */
static char *i_dart_event_append_addr(char *p, char *end, const DartEvent *ev){
    int i;
    for (i = 0; i < 4; i++){ if (i) p = i_dart_event_append_str(p,end,"."); p = i_dart_event_append_u64(p,end,ev->ip[i]); }
    p = i_dart_event_append_str(p,end,":"); return i_dart_event_append_u64(p,end,ev->port);
}
/* topic=name or topic=index */
static char *i_dart_event_append_topic(char *p, char *end, const DartEvent *ev){
    p = i_dart_event_append_str(p,end,"topic=");
    if (ev->topic_name) return i_dart_event_append_str(p,end,ev->topic_name);
    return i_dart_event_append_u64(p,end,ev->topic);
}
/* the peer's name, else id=n */
static char *i_dart_event_append_peer(char *p, char *end, const DartEvent *ev){
    if (ev->peer_name && ev->peer_name[0]) return i_dart_event_append_str(p,end,ev->peer_name);
    p = i_dart_event_append_str(p,end,"id="); return i_dart_event_append_u64(p,end,ev->peer);
}
#ifndef DART_NO_DIAG   /* only the verbose error body uses these two */
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

/* the DART_ERROR body, split out so the text compiles away under DART_NO_DIAG */
static char *i_dart_event_error_str(char *p, char *end, const DartEvent *ev){
#ifdef DART_NO_DIAG
    p = i_dart_event_append_str(p,end,"error "); return i_dart_event_append_u64(p,end,(uint64_t)ev->error);
#else
    switch (ev->error){
    case DART_E_NAME_COLLISION:
        p=i_dart_event_append_str(p,end,"name-collision "); p=i_dart_event_append_topic(p,end,ev);
        p=i_dart_event_append_str(p,end," peer "); p=i_dart_event_append_peer(p,end,ev);
        p=i_dart_event_append_str(p,end," identity=0x"); p=i_dart_event_append_hex(p,end,ev->identity);
        p=i_dart_event_append_str(p,end,": match refused"); break;
    case DART_E_QOS_INCOMPATIBLE:
        p=i_dart_event_append_str(p,end,"qos-incompatible "); p=i_dart_event_append_topic(p,end,ev);
        p=i_dart_event_append_str(p,end," from "); p=i_dart_event_append_peer(p,end,ev);
        p=i_dart_event_append_str(p,end,": reliable subscriber refused best-effort publisher"); break;
    case DART_E_KIND_MISMATCH:
        p=i_dart_event_append_str(p,end,"kind-mismatch "); p=i_dart_event_append_topic(p,end,ev);
        p=i_dart_event_append_str(p,end," from "); p=i_dart_event_append_peer(p,end,ev);
        p=i_dart_event_append_str(p,end,": same name, different entity kind, refused"); break;
    case DART_E_SCHEMA_MISMATCH:
        p=i_dart_event_append_str(p,end,"schema-mismatch "); p=i_dart_event_append_topic(p,end,ev);
        p=i_dart_event_append_str(p,end," peer "); p=i_dart_event_append_peer(p,end,ev);
        p=i_dart_event_append_str(p,end,": ");
        p=i_dart_event_append_str(p,end, ev->schema_detail && ev->schema_detail[0]
                                  ? ev->schema_detail : "incompatible schemas, refused"); break;
    case DART_E_INTEREST_OVERFLOW:
        p=i_dart_event_append_str(p,end,"interest-overflow peer "); p=i_dart_event_append_peer(p,end,ev);
        p=i_dart_event_append_str(p,end,": "); p=i_dart_event_append_u64(p,end,ev->lost_count);
        p=i_dart_event_append_str(p,end," matched topics whose index map could not be allocated"); break;
    case DART_E_META_TRUNCATED_INTEREST:
        p=i_dart_event_append_str(p,end,"meta-truncated: interest list dropped (announce overlay full)"); break;
    case DART_E_META_TRUNCATED_SCHEMA:
        p=i_dart_event_append_str(p,end,"meta-truncated: schema section dropped (announce overlay full)"); break;
    case DART_E_PEER_META_TOO_BIG:
        p=i_dart_event_append_str(p,end,"peer-meta-too-big "); p=i_dart_event_append_peer(p,end,ev);
        if (ev->ip_len==4){ p=i_dart_event_append_str(p,end," at "); p=i_dart_event_append_addr(p,end,ev); }
        p=i_dart_event_append_str(p,end,": "); p=i_dart_event_append_u64(p,end,ev->too_big_bytes);
        p=i_dart_event_append_str(p,end," byte blob exceeds our capacity, refused"); break;
    case DART_E_MSG_TOO_BIG:
        p=i_dart_event_append_str(p,end,"msg-too-big "); p=i_dart_event_append_topic(p,end,ev);
        p=i_dart_event_append_str(p,end," from "); p=i_dart_event_append_peer(p,end,ev);
        p=i_dart_event_append_str(p,end," ("); p=i_dart_event_append_u64(p,end,ev->too_big_bytes);
        p=i_dart_event_append_str(p,end," bytes), skipped"); break;
    case DART_E_PEER_REFUSED:
        p=i_dart_event_append_str(p,end,"peer-refused at "); p=i_dart_event_append_addr(p,end,ev);
        p=i_dart_event_append_str(p,end,": peer table full of active peers (raise max_peers)"); break;
    case DART_E_EVICTED_UNSENT:
        p=i_dart_event_append_str(p,end,"evicted-unsent "); p=i_dart_event_append_topic(p,end,ev);
        p=i_dart_event_append_str(p,end," seqno "); p=i_dart_event_append_u64(p,end,ev->lost_first);
        p=i_dart_event_append_str(p,end,".."); p=i_dart_event_append_u64(p,end,ev->lost_first + ev->lost_count - 1);
        p=i_dart_event_append_str(p,end,": send burst outran the TX drain"); break;
    case DART_E_UNMATCHED_SEND:
        p=i_dart_event_append_str(p,end,"unmatched-send "); p=i_dart_event_append_topic(p,end,ev);
        p=i_dart_event_append_str(p,end,": committed with no subscriber while a match was still resolving (likely missed an already-present subscriber)"); break;
    case DART_E_DUPLICATE_AUTHORITY:
        p=i_dart_event_append_str(p,end,"duplicate-authority "); p=i_dart_event_append_topic(p,end,ev);
        p=i_dart_event_append_str(p,end,": peer "); p=i_dart_event_append_peer(p,end,ev);
        p=i_dart_event_append_str(p,end," also claims the handler/owner side (expected exactly one)"); break;
    case DART_E_OOM:
        p=i_dart_event_append_str(p,end,"out-of-memory");
        if (ev->too_big_bytes){ p=i_dart_event_append_str(p,end,": "); p=i_dart_event_append_u64(p,end,ev->too_big_bytes);
                                p=i_dart_event_append_str(p,end," bytes needed"); }
        break;
    case DART_E_PLATFORM:
        p=i_dart_event_append_str(p,end,"platform net init failed"); break;
    case DART_E_BAD_ADDRESS:
        p=i_dart_event_append_str(p,end,"configured address could not be parsed"); break;
    case DART_E_SOCKET:
        p=i_dart_event_append_str(p,end,"socket open failed"); p=i_dart_event_append_oserr(p,end,ev); break;
    case DART_E_BIND:
        p=i_dart_event_append_str(p,end,"bind failed on port "); p=i_dart_event_append_u64(p,end,ev->port);
        p=i_dart_event_append_oserr(p,end,ev); break;
    case DART_E_MCAST_JOIN:
        p=i_dart_event_append_str(p,end,"multicast join failed"); p=i_dart_event_append_oserr(p,end,ev); break;
    case DART_E_SEND:
        p=i_dart_event_append_str(p,end,"send failed to "); p=i_dart_event_append_peer(p,end,ev);
        if (ev->topic_name){ p=i_dart_event_append_str(p,end," "); p=i_dart_event_append_topic(p,end,ev); }
        if (ev->too_big_bytes){ p=i_dart_event_append_str(p,end," ("); p=i_dart_event_append_u64(p,end,ev->too_big_bytes);
                                p=i_dart_event_append_str(p,end," B)"); }
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
    p = buf; end = buf + cap - 1;                  /* one byte reserved for the NUL */
    switch (ev->kind){
    case DART_PEER_UP:
        p = i_dart_event_append_str(p,end,"peer-up "); p = i_dart_event_append_peer(p,end,ev);
        if (ev->ip_len == 4){ p = i_dart_event_append_str(p,end," at "); p = i_dart_event_append_addr(p,end,ev); }
        break;
    case DART_PEER_DOWN:
        p = i_dart_event_append_str(p,end,"peer-down "); p = i_dart_event_append_peer(p,end,ev);
        break;
    case DART_PEER_INTEREST:
        p = i_dart_event_append_str(p,end,"interest from "); p = i_dart_event_append_peer(p,end,ev);
        p = i_dart_event_append_str(p,end," publish-to="); p = i_dart_event_append_u64(p,end,ev->publish_topics);
        p = i_dart_event_append_str(p,end," topics, receive-from="); p = i_dart_event_append_u64(p,end,ev->receive_topics);
        p = i_dart_event_append_str(p,end," topics");
        break;
    case DART_MSG_LOST:
        p = i_dart_event_append_str(p,end,"msg-lost "); p = i_dart_event_append_topic(p,end,ev);
        p = i_dart_event_append_str(p,end," from "); p = i_dart_event_append_peer(p,end,ev);
        p = i_dart_event_append_str(p,end," seqno "); p = i_dart_event_append_u64(p,end,ev->lost_first);
        p = i_dart_event_append_str(p,end,".."); p = i_dart_event_append_u64(p,end,ev->lost_first + ev->lost_count - 1);
        break;
    case DART_ERROR:
        p = i_dart_event_error_str(p, end, ev);
        break;
    }
    *p = '\0';                                     /* p is at most end, in range */
    return buf;
}

/* The reflection tables: one channel per advertised topic index, folded into entities by
   kind and the four byte suffix convention. See spec/reflection.md. */
#define I_DART_NONE16   0xFFFFu
#define I_DART_NAME_NONE 0xFFFFFFFFu
typedef struct {
    uint32_t hash;          /* the low 32 name id from the announce */
    uint32_t name_off;      /* into the names arena, NAME_NONE until details land */
    const DartSchema *schema;   /* interned, lives until close */
    uint64_t schema_hash;
    uint16_t entity;        /* the entity slot, NONE16 until folded */
    uint8_t  name_len, kind, role, reliable, attrs, present;
} i_DartChannel;
typedef struct {
    uint64_t id;            /* dart_topic_id of the base name, 0 while unfetched */
    uint32_t name_off, hash;
    uint16_t primary, rsp, prg, set;   /* channel indices, NONE16 where absent */
    uint8_t  name_len, kind, incomplete;
} i_DartPeerEntity;
typedef struct {
    i_DartChannel    *chan; uint32_t n_chan, cap_chan;   /* dense by the peer's topic index */
    i_DartPeerEntity *ent;  uint32_t n_ent,  cap_ent;
    char             *names; uint32_t names_len, names_cap;
    uint8_t  dirty;
    uint8_t  addr_len;
    char     addr[48];      /* "ip:port", formatted once at peer up */
} i_DartReflect;
/* one folded entity of the whole mesh, keyed by kind and the 64 bit name id */
typedef struct {
    uint64_t id, generation;
    uint32_t from_peer, provider;
    uint16_t from_slot, provider_slot, providers, consumers;
    uint8_t  kind, conflict, has_provider;
} i_DartMeshEntity;

/* The core's per peer lifecycle state, kept in the discovery peer's user scratch so the
   node holds no peer table of its own. The interest fields are the external fetch. */
typedef struct {
    uint8_t  added; uint8_t dormant; uint8_t detail_due; uint8_t interest_due;
    uint32_t interest_epoch;     /* bumps on every reflected change, the observer cache key */
    uint32_t interest_version;   /* the last fully assembled external version, 0 = none */
    uint32_t fetch_version;      /* the version the cursor is assembling, 0 = idle */
    uint32_t fetch_cursor;       /* bytes assembled so far */
    uint32_t fetch_len;          /* the blob's total length once the first page lands */
    uint8_t *interest_buf;
    uint32_t interest_cap;
    i_DartReflect refl;          /* what this peer advertises, as tables */
} i_DartNodePeerExtra;

/* The schema state for the gate and delivery: schemas interned by hash, reader views per
   (schema, topic), the decode map per (peer, topic). Flat, hook allocated, kept until close. */
typedef struct { uint64_t hash; DartSchema *parsed; } i_DartNodeSchemaIntern;
typedef struct { uint64_t hash; uint16_t topic; DartSchema *rebased; } i_DartNodeSchemaBind;
typedef struct { uint32_t peer; uint16_t topic; const DartSchema *schema; } i_DartNodePeerSchema;
#ifndef DART_NO_DIAG
/* why the gate refused (peer, topic, direction), recorded at detail intake for the event */
#define DART__SCHEMA_WHY_MAX 128
typedef struct { uint32_t peer; uint16_t topic; uint8_t peer_is_pub;
                 char text[DART__SCHEMA_WHY_MAX]; } i_DartNodeSchemaWhy;
#endif

struct i_DartNodeCore {
    DartTransportState           *transport;
    DartDiscoveryState  *discovery;   /* the peer table we delegate to */
    DartEventFn         on_event;
    void                 *user;
    int                   oob_capable;
    uint8_t               oob_host[16];
    uint8_t              *meta_buf;    /* our announce overlay, hook allocated at its actual size */
    uint16_t              meta_cap;
    uint16_t              meta_len;
    uint16_t              frag_size;   /* baked into the overlay */
    DartMetaSchema       *chan_schemas;  /* per topic schema advertisement, hash 0 = none */
    const DartSchema    **chan_compiled; /* per topic parsed schema, the gate's local side */
    uint16_t              n_topics;
    DartAllocFn           alloc;         /* required */
    void                 *alloc_user;
    uint8_t              *detail_buf;    /* detail response scratch, grown on demand */
    uint32_t              detail_cap;
    uint8_t               detail_due_any;/* some peer's request is due */
    i_DartNodeSchemaIntern *interned;     uint32_t n_interned,     cap_interned;
    i_DartNodeSchemaBind   *binds;        uint32_t n_binds,        cap_binds;
    i_DartNodePeerSchema   *peer_schemas; uint32_t n_peer_schemas, cap_peer_schemas;
    uint8_t                 fetch_details; /* observer mode */
    i_DartReflect           self;          /* this node's own channels */
    DartString              self_name;
    i_DartMeshEntity       *mesh; uint32_t n_mesh, cap_mesh;
    uint8_t                 mesh_dirty;
    uint32_t                mesh_epoch;
    void                   *scratch; uint32_t scratch_cap;   /* fold and mesh build workspace */
#ifndef DART_NO_DIAG
    i_DartNodeSchemaWhy    *schema_whys;   uint32_t n_schema_whys,   cap_schema_whys;
#endif
};

/* grow one flat array through the hook. 1 with *arr and *cap updated, else 0 */
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

/* the arena layout: the core struct, then the per topic schema registry. One sequence so
   measure and build agree */
static void i_dart_node_core_layout(i_DartBump *b, uint16_t n_topics,
                              i_DartNodeCore **out_c,
                              DartMetaSchema **out_schemas, const DartSchema ***out_compiled){
    i_DartNodeCore *c       = (i_DartNodeCore*)i_dart_bump_take(b, sizeof(struct i_DartNodeCore), 16);
    DartMetaSchema *schemas = (DartMetaSchema*)i_dart_bump_take(b, (size_t)n_topics*sizeof(DartMetaSchema), 16);
    const DartSchema **compiled = (const DartSchema**)i_dart_bump_take(b, (size_t)n_topics*sizeof(DartSchema*), 16);
    if (out_c)        *out_c        = c;
    if (out_schemas)  *out_schemas  = schemas;
    if (out_compiled) *out_compiled = compiled;
}

size_t i_dart_node_core_required_memory(uint16_t n_topics){
    i_DartBump b; memset(&b, 0, sizeof b);
    i_dart_node_core_layout(&b, n_topics, NULL, NULL, NULL);
    return b.offset + 16u;   /* slack to align the caller's mem up to base */
}

i_DartNodeCore *i_dart_node_core_init(void *mem, size_t cap, const i_DartNodeCoreConfig *cfg){
    i_DartBump b; i_DartNodeCore *c; uint8_t *base;
    DartMetaSchema *schemas; const DartSchema **compiled;
    if (!mem || !cfg || !cfg->transport || !cfg->alloc) return NULL;
    if (cap < i_dart_node_core_required_memory(cfg->n_topics)) return NULL;
    base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    memset(&b, 0, sizeof b); b.base = base; b.cap = cap - (size_t)(base - (uint8_t*)mem);
    i_dart_node_core_layout(&b, cfg->n_topics, &c, &schemas, &compiled);

    memset(c, 0, sizeof *c);
    c->transport     = cfg->transport;
    c->discovery     = cfg->discovery;     /* may be NULL now, bound later */
    c->on_event      = cfg->on_event;      c->user = cfg->user;
    c->alloc         = cfg->alloc;         c->alloc_user = cfg->alloc_user;
    c->oob_capable   = cfg->oob_capable;
    c->fetch_details = cfg->fetch_details;
    memcpy(c->oob_host, cfg->oob_host, 16);
    c->meta_buf      = NULL;                                /* hook allocated at the first build */
    c->meta_cap      = 0;
    c->frag_size     = cfg->frag_size;
    c->chan_schemas  = schemas;
    c->chan_compiled = compiled;
    c->n_topics    = cfg->n_topics;
    memset(schemas, 0, (size_t)cfg->n_topics * sizeof *schemas);
    memset(compiled, 0, (size_t)cfg->n_topics * sizeof *compiled);
    return c;
}

/* A struct copy carries the scalars and the stable hook allocations. The caller re
 * points the transport, discovery and blob after those move. */
i_DartNodeCore *i_dart_node_core_migrate(i_DartNodeCore *old, void *new_mem, size_t new_cap,
                                       uint16_t new_n_topics){
    i_DartBump b; i_DartNodeCore *c; uint8_t *base;
    DartMetaSchema *schemas; const DartSchema **compiled;
    uint16_t keep;
    if (!old) return NULL;
    if (new_cap < i_dart_node_core_required_memory(new_n_topics)) return NULL;
    base = (uint8_t*)(((uintptr_t)new_mem + 15u) & ~(uintptr_t)15u);
    memset(&b, 0, sizeof b); b.base = base; b.cap = new_cap - (size_t)(base - (uint8_t*)new_mem);
    i_dart_node_core_layout(&b, new_n_topics, &c, &schemas, &compiled);
    *c = *old;
    keep = old->n_topics < new_n_topics ? old->n_topics : new_n_topics;
    memset(schemas, 0, (size_t)new_n_topics * sizeof *schemas);
    memcpy(schemas, old->chan_schemas, (size_t)keep * sizeof *schemas);   /* views stay valid */
    memset(compiled, 0, (size_t)new_n_topics * sizeof *compiled);
    memcpy(compiled, old->chan_compiled, (size_t)keep * sizeof *compiled);
    c->chan_schemas  = schemas;
    c->chan_compiled = compiled;
    c->n_topics    = new_n_topics;
    return c;
}

void i_dart_node_core_set_topic_schema(i_DartNodeCore *c, uint16_t topic_index,
                                         const DartSchema *schema){
    if (!c || topic_index >= c->n_topics) return;
    c->chan_compiled[topic_index]     = schema;
    c->chan_schemas[topic_index].hash = schema ? dart_schema_hash(schema) : 0;
    c->chan_schemas[topic_index].wire = schema ? dart_schema_wire(schema) : dart_bytes(NULL, 0);
}

/* Drops the live pointers but keeps the hash as the slot's fingerprint. The responder
 * never answers a retired slot, so the stale hash is never served. */
void i_dart_node_core_retire_topic_schema(i_DartNodeCore *c, uint16_t topic_index){
    if (!c || topic_index >= c->n_topics) return;
    c->chan_compiled[topic_index]     = NULL;
    c->chan_schemas[topic_index].wire = dart_bytes(NULL, 0);
}

/* The retired slot's fingerprint, 0 = it carried no schema. */
uint64_t i_dart_node_core_topic_schema_hash(i_DartNodeCore *c, uint16_t topic_index){
    return (c && topic_index < c->n_topics) ? c->chan_schemas[topic_index].hash : 0;
}

/* the schema gate and the delivery binding */

/* A peer schema parsed once per hash. The claimed hash must equal the wire's real hash,
   or a lying peer could poison the intern for every honest one. */
static DartSchema *i_dart_node_core_intern(i_DartNodeCore *c, uint64_t hash, DartBytes wire){
    uint32_t i; DartSchema *p;
    for (i = 0; i < c->n_interned; i++)
        if (c->interned[i].hash == hash) return c->interned[i].parsed;
    if (!wire.data || wire.len == 0){
        /* identical hashes travel as zero bytes, so a schema we also hold interns from a
           fresh parse of our own wire. A retire frees ours, an interned one lives on. */
        uint16_t t;
        for (t = 0; t < c->n_topics; t++)
            if (c->chan_schemas[t].hash == hash && c->chan_schemas[t].wire.len){
                wire = c->chan_schemas[t].wire;
                break;
            }
    }
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

/* the reader view for (writer schema, topic): our fields on their layout, cached */
static DartSchema *i_dart_node_core_bind(i_DartNodeCore *c, uint64_t hash, uint16_t topic_index,
                                         const DartSchema *ours, const DartSchema *pub){
    uint32_t i; DartSchema *rb;
    for (i = 0; i < c->n_binds; i++)
        if (c->binds[i].hash == hash && c->binds[i].topic == topic_index) return c->binds[i].rebased;
    rb = dart_schema_rebase(ours, pub, c->alloc, c->alloc_user);
    if (!rb) return NULL;
    if (!i_dart_node_core_array_reserve(c, (void**)&c->binds, &c->cap_binds,
                                        c->n_binds + 1u, sizeof *c->binds)){
        dart_schema_free(rb, c->alloc, c->alloc_user);
        return NULL;
    }
    c->binds[c->n_binds].hash = hash;
    c->binds[c->n_binds].topic = topic_index;
    c->binds[c->n_binds].rebased = rb;
    c->n_binds++;
    return rb;
}

/* The delivery map. An identical schema stores this sentinel, resolved at read time, since
   our compiled copy is freed and re parsed across a retire and reuse. */
#define i_DART_NODE_SCHEMA_OURS ((const DartSchema *)(uintptr_t)1)
static void i_dart_node_core_peer_schema_set(i_DartNodeCore *c, uint32_t peer, uint16_t topic_index,
                                             const DartSchema *schema){
    uint32_t i;
    for (i = 0; i < c->n_peer_schemas; i++)
        if (c->peer_schemas[i].peer == peer && c->peer_schemas[i].topic == topic_index){
            c->peer_schemas[i].schema = schema;
            return;
        }
    if (!schema) return;
    if (!i_dart_node_core_array_reserve(c, (void**)&c->peer_schemas, &c->cap_peer_schemas,
                                        c->n_peer_schemas + 1u, sizeof *c->peer_schemas)) return;
    c->peer_schemas[c->n_peer_schemas].peer = peer;
    c->peer_schemas[c->n_peer_schemas].topic = topic_index;
    c->peer_schemas[c->n_peer_schemas].schema = schema;
    c->n_peer_schemas++;
}

/* drop a peer's map entries, on GONE or when its id is recycled */
static void i_dart_node_core_peer_schema_clear(i_DartNodeCore *c, uint32_t peer){
    uint32_t i = 0;
    while (i < c->n_peer_schemas){
        if (c->peer_schemas[i].peer == peer)
            c->peer_schemas[i] = c->peer_schemas[--c->n_peer_schemas];   /* swap remove */
        else i++;
    }
}

/* why a schema gate refused, feeding DartEvent.schema_detail */

#ifndef DART_NO_DIAG
static void i_dart_node_core_schema_why_set(i_DartNodeCore *c, uint32_t peer, uint16_t topic_index,
                                            int peer_is_pub, const char *text){
    uint32_t i; i_DartNodeSchemaWhy *w = NULL; size_t n;
    for (i = 0; i < c->n_schema_whys; i++)
        if (c->schema_whys[i].peer == peer && c->schema_whys[i].topic == topic_index &&
            c->schema_whys[i].peer_is_pub == (uint8_t)peer_is_pub){ w = &c->schema_whys[i]; break; }
    if (!w){
        if (!i_dart_node_core_array_reserve(c, (void**)&c->schema_whys, &c->cap_schema_whys,
                                            c->n_schema_whys + 1u, sizeof *c->schema_whys)) return;
        w = &c->schema_whys[c->n_schema_whys++];
        w->peer = peer; w->topic = topic_index; w->peer_is_pub = (uint8_t)peer_is_pub;
    }
    n = 0;
    while (text[n] && n < DART__SCHEMA_WHY_MAX - 1u){ w->text[n] = text[n]; n++; }
    w->text[n] = '\0';
}
static void i_dart_node_core_schema_why_drop(i_DartNodeCore *c, uint32_t peer, uint16_t topic_index,
                                             int peer_is_pub){
    uint32_t i = 0;
    while (i < c->n_schema_whys){
        if (c->schema_whys[i].peer == peer && c->schema_whys[i].topic == topic_index &&
            c->schema_whys[i].peer_is_pub == (uint8_t)peer_is_pub)
            c->schema_whys[i] = c->schema_whys[--c->n_schema_whys];   /* swap remove */
        else i++;
    }
}
static void i_dart_node_core_schema_why_clear(i_DartNodeCore *c, uint32_t peer){
    uint32_t i = 0;
    while (i < c->n_schema_whys){
        if (c->schema_whys[i].peer == peer)
            c->schema_whys[i] = c->schema_whys[--c->n_schema_whys];
        else i++;
    }
}
const char *i_dart_node_core_schema_why(i_DartNodeCore *c, uint32_t peer, uint16_t topic_index,
                                        int peer_is_pub){
    uint32_t i;
    if (!c) return NULL;
    for (i = 0; i < c->n_schema_whys; i++)
        if (c->schema_whys[i].peer == peer && c->schema_whys[i].topic == topic_index &&
            c->schema_whys[i].peer_is_pub == (uint8_t)peer_is_pub)
            return c->schema_whys[i].text;
    return NULL;
}
const char *i_dart_node_core_note_size_mismatch(i_DartNodeCore *c, uint32_t peer,
                                        uint16_t topic_index, uint64_t got_len, uint64_t want_len){
    char text[DART__SCHEMA_WHY_MAX];
    char *p = text, *end = text + sizeof text - 1;
    if (!c) return NULL;
    p = i_dart_event_append_str(p, end, "message is ");
    p = i_dart_event_append_u64(p, end, got_len);
    p = i_dart_event_append_str(p, end, " bytes, the publisher's schema says ");
    p = i_dart_event_append_u64(p, end, want_len);
    *p = '\0';
    i_dart_node_core_schema_why_set(c, peer, topic_index, 1, text);
    return i_dart_node_core_schema_why(c, peer, topic_index, 1);
}
#else
static void i_dart_node_core_schema_why_clear(i_DartNodeCore *c, uint32_t peer){
    (void)c; (void)peer;
}
const char *i_dart_node_core_schema_why(i_DartNodeCore *c, uint32_t peer, uint16_t topic_index,
                                        int peer_is_pub){
    (void)c; (void)peer; (void)topic_index; (void)peer_is_pub; return NULL;
}
const char *i_dart_node_core_note_size_mismatch(i_DartNodeCore *c, uint32_t peer,
                                        uint16_t topic_index, uint64_t got_len, uint64_t want_len){
    (void)c; (void)peer; (void)topic_index; (void)got_len; (void)want_len; return NULL;
}
#endif

const DartSchema *i_dart_node_core_msg_schema(i_DartNodeCore *c, uint32_t peer, uint16_t topic_index){
    uint32_t i;
    if (!c) return NULL;
    for (i = 0; i < c->n_peer_schemas; i++)
        if (c->peer_schemas[i].peer == peer && c->peer_schemas[i].topic == topic_index){
            const DartSchema *s = c->peer_schemas[i].schema;
            if (s == i_DART_NODE_SCHEMA_OURS)   /* an identical schema entry: resolve live */
                return topic_index < c->n_topics ? c->chan_compiled[topic_index] : NULL;
            return s;
        }
    return NULL;
}

/* The slot was rebound with a retype: every binding, view and refusal reason recorded for
 * the old occupant is stale. The verdicts re pended in the transport, so they re derive. */
void i_dart_node_core_topic_rebound(i_DartNodeCore *c, uint16_t topic_index){
    uint32_t i;
    if (!c) return;
    i = 0;
    while (i < c->n_peer_schemas){
        if (c->peer_schemas[i].topic == topic_index)
            c->peer_schemas[i] = c->peer_schemas[--c->n_peer_schemas];   /* swap remove */
        else i++;
    }
    i = 0;
    while (i < c->n_binds){
        if (c->binds[i].topic == topic_index){
            if (c->binds[i].rebased) dart_schema_free(c->binds[i].rebased, c->alloc, c->alloc_user);
            c->binds[i] = c->binds[--c->n_binds];
        } else i++;
    }
#ifndef DART_NO_DIAG
    i = 0;
    while (i < c->n_schema_whys){
        if (c->schema_whys[i].topic == topic_index)
            c->schema_whys[i] = c->schema_whys[--c->n_schema_whys];
        else i++;
    }
#endif
}

/* the reflection tables */

static DartBytes i_dart_node_core_interest_of(i_DartNodePeerExtra *ex, DartBytes meta,
                            uint32_t meta_version);

static uint64_t i_dart_node_core_fnv(uint64_t h, const void *p, size_t n){
    const uint8_t *b = (const uint8_t*)p; size_t i;
    for (i = 0; i < n; i++){ h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

static void i_dart_reflect_free(i_DartNodeCore *c, i_DartReflect *r){
    if (!c->alloc) return;
    if (r->chan)  c->alloc(c->alloc_user, r->chan, 0);
    if (r->ent)   c->alloc(c->alloc_user, r->ent, 0);
    if (r->names) c->alloc(c->alloc_user, r->names, 0);
    memset(r, 0, sizeof *r);
}

static void i_dart_node_core_mesh_bump(i_DartNodeCore *c){
    c->mesh_dirty = 1;
    c->mesh_epoch++;
}

/* the channel record for index, growing the dense table with new slots absent */
static i_DartChannel *i_dart_reflect_channel(i_DartNodeCore *c, i_DartReflect *r, uint16_t index){
    if ((uint32_t)index >= r->n_chan){
        uint32_t i, need = (uint32_t)index + 1u;
        if (!i_dart_node_core_array_reserve(c, (void**)&r->chan, &r->cap_chan, need, sizeof *r->chan))
            return NULL;
        for (i = r->n_chan; i < need; i++){
            memset(&r->chan[i], 0, sizeof r->chan[i]);
            r->chan[i].name_off = I_DART_NAME_NONE;
            r->chan[i].entity = I_DART_NONE16;
        }
        r->n_chan = need;
    }
    return &r->chan[index];
}

/* appends a NUL terminated name copy and returns its offset, NAME_NONE on OOM */
static uint32_t i_dart_reflect_name(i_DartNodeCore *c, i_DartReflect *r, DartString name){
    uint32_t off = r->names_len, need = r->names_len + (uint32_t)name.len + 1u;
    if (!i_dart_node_core_array_reserve(c, (void**)&r->names, &r->names_cap, need, 1)) return I_DART_NAME_NONE;
    memcpy(r->names + off, name.data, name.len);
    r->names[off + name.len] = '\0';
    r->names_len = need;
    return off;
}

/* an interest apply: kind, role, reliability and hash per advertised index. Absent slots clear */
static void i_dart_node_core_reflect_interest(i_DartNodeCore *c, i_DartNodePeerExtra *ex,
                                              DartBytes interest){
    i_DartReflect *r = &ex->refl;
    DartInterestIter it; DartTopicEntry e; uint32_t i;
    if (!interest.data) return;
    for (i = 0; i < r->n_chan; i++) r->chan[i].present = 0;
    memset(&it, 0, sizeof it);
    while (dart_interest_next(interest, &it, &e)){
        i_DartChannel *ch = i_dart_reflect_channel(c, r, e.index);
        if (!ch) return;
        ch->hash = e.hash; ch->kind = e.kind; ch->role = e.role;
        ch->reliable = e.reliable; ch->present = 1;
    }
    r->dirty = 1;
}

/* a detail entry: the name, the attrs and the interned schema for one index */
static void i_dart_node_core_reflect_detail(i_DartNodeCore *c, i_DartNodePeerExtra *ex,
                                            const DartDetail *d){
    i_DartReflect *r = &ex->refl;
    i_DartChannel *ch;
    if (d->name.len == 0 || d->name.len > DART_TOPIC_NAME_MAX) return;
    ch = i_dart_reflect_channel(c, r, d->index);
    if (!ch) return;
    if (ch->name_off == I_DART_NAME_NONE){
        ch->name_off = i_dart_reflect_name(c, r, d->name);
        if (ch->name_off == I_DART_NAME_NONE) return;
        ch->name_len = (uint8_t)d->name.len;
    }
    ch->attrs = d->attrs;
    ch->schema_hash = d->schema_hash;
    ch->schema = d->schema_hash ? i_dart_node_core_intern(c, d->schema_hash, d->schema_wire) : NULL;
    r->dirty = 1;
}

static void i_dart_reflect_format_addr(i_DartReflect *r, const DartDiscoveryAddr *a){
    char *p = r->addr; unsigned i;
    if (a->ip_len == 4){
        for (i = 0; i < 4; i++){
            unsigned v = a->ip[i]; char t[4]; int k = 0;
            do { t[k++] = (char)('0' + v % 10u); v /= 10u; } while (v);
            while (k) *p++ = t[--k];
            if (i < 3) *p++ = '.';
        }
    } else if (a->ip_len == 16){
        static const char hx[] = "0123456789abcdef";
        *p++ = '[';
        for (i = 0; i < 16; i += 2){
            *p++ = hx[a->ip[i] >> 4]; *p++ = hx[a->ip[i] & 15];
            *p++ = hx[a->ip[i+1] >> 4]; *p++ = hx[a->ip[i+1] & 15];
            if (i < 14) *p++ = ':';
        }
        *p++ = ']';
    }
    {   unsigned v = a->port; char t[6]; int k = 0;
        *p++ = ':';
        do { t[k++] = (char)('0' + v % 10u); v /= 10u; } while (v);
        while (k) *p++ = t[--k];
    }
    r->addr_len = (uint8_t)(p - r->addr);
}

/* the fold: channels to entities */

/* one row per DartTopicKind: its entity, whether it is the primary channel, its name
   suffix, and which role is the provider side */
typedef struct { uint8_t entity, primary, provider_pubs; const char *suffix; } i_DartKindRow;
static const i_DartKindRow i_dart_kind_rows[8] = {
    { DART_ENTITY_TOPIC,    1, 1, ""     },   /* TOPIC    */
    { DART_ENTITY_FUNCTION, 1, 0, "@req" },   /* FUNC_REQ */
    { DART_ENTITY_FUNCTION, 0, 1, "@rsp" },   /* FUNC_RSP */
    { DART_ENTITY_VARIABLE, 1, 1, ""     },   /* VARIABLE */
    { DART_ENTITY_VARIABLE, 0, 0, "@set" },   /* VAR_SET  */
    { DART_ENTITY_TASK,     1, 0, "@req" },   /* TASK_REQ */
    { DART_ENTITY_TASK,     0, 1, "@prg" },   /* TASK_PRG */
    { DART_ENTITY_TASK,     0, 1, "@rsp" }    /* TASK_RSP */
};
static const i_DartKindRow *i_dart_kind_row(uint8_t kind){
    return kind < 8 ? &i_dart_kind_rows[kind] : &i_dart_kind_rows[0];
}

static int i_dart_reflect_hidden(const char *name, size_t len){
    return len >= 6 && memcmp(name, "@dart/", 6) == 0;
}
static int i_dart_reflect_hidden_hash(uint32_t h){
    static const char *const nm[] = { "@dart/log/error", "@dart/log/warn", "@dart/log/info",
                                      "@dart/meta@req", "@dart/meta@rsp" };
    size_t i;
    for (i = 0; i < sizeof nm / sizeof nm[0]; i++)
        if (h == (uint32_t)dart_topic_id(nm[i])) return 1;
    return 0;
}

typedef struct { uint32_t hash; uint16_t index; } i_DartHashPair;

static void *i_dart_node_core_scratch(i_DartNodeCore *c, uint32_t bytes){
    if (!i_dart_node_core_array_reserve(c, &c->scratch, &c->scratch_cap, bytes, 1)) return NULL;
    return c->scratch;
}

static void i_dart_hash_sort(i_DartHashPair *a, uint32_t n){   /* shell sort, no libc */
    uint32_t gap, i, j;
    for (gap = n / 2u; gap > 0; gap /= 2u)
        for (i = gap; i < n; i++){
            i_DartHashPair v = a[i];
            for (j = i; j >= gap && (a[j-gap].hash > v.hash
                                     || (a[j-gap].hash == v.hash && a[j-gap].index > v.index)); j -= gap)
                a[j] = a[j-gap];
            a[j] = v;
        }
}

/* the channel of kind whose name hashes like base plus suffix, NONE16 when absent */
static uint16_t i_dart_reflect_partner(const i_DartReflect *r, const i_DartHashPair *sorted,
                                       uint32_t n, const char *base, size_t base_len,
                                       const char *suffix, uint8_t kind){
    char buf[DART_TOPIC_NAME_MAX + 8]; size_t sl = strlen(suffix);
    uint32_t want, lo = 0, hi = n;
    if (base_len + sl > DART_TOPIC_NAME_MAX) return I_DART_NONE16;
    memcpy(buf, base, base_len); memcpy(buf + base_len, suffix, sl + 1);
    want = (uint32_t)dart_topic_id(buf);
    while (lo < hi){ uint32_t mid = (lo + hi) / 2u; if (sorted[mid].hash < want) lo = mid + 1; else hi = mid; }
    for (; lo < n && sorted[lo].hash == want; lo++){
        const i_DartChannel *ch = &r->chan[sorted[lo].index];
        if (ch->kind != kind) continue;
        /* a fetched partner name must really be base plus suffix, 32 bit hashes collide */
        if (ch->name_off != I_DART_NAME_NONE
            && (ch->name_len != base_len + sl || memcmp(r->names + ch->name_off, buf, base_len + sl) != 0))
            continue;
        return sorted[lo].index;
    }
    return I_DART_NONE16;
}

static i_DartPeerEntity *i_dart_reflect_entity_new(i_DartNodeCore *c, i_DartReflect *r){
    i_DartPeerEntity *e;
    if (!i_dart_node_core_array_reserve(c, (void**)&r->ent, &r->cap_ent, r->n_ent + 1u, sizeof *r->ent))
        return NULL;
    e = &r->ent[r->n_ent++];
    memset(e, 0, sizeof *e);
    e->primary = e->rsp = e->prg = e->set = I_DART_NONE16;
    return e;
}

/* attaches channel idx as the entity's partner of its kind */
static void i_dart_reflect_attach(i_DartReflect *r, i_DartPeerEntity *e, uint16_t idx, uint16_t slot){
    i_DartChannel *ch = &r->chan[idx];
    ch->entity = slot;
    switch (ch->kind){
        case DART_KIND_FUNC_RSP: case DART_KIND_TASK_RSP: e->rsp = idx; break;
        case DART_KIND_TASK_PRG: e->prg = idx; break;
        case DART_KIND_VAR_SET:  e->set = idx; break;
        default: break;
    }
}

static void i_dart_reflect_set_name(i_DartPeerEntity *e, const i_DartReflect *r, uint32_t off, size_t len){
    char buf[DART_TOPIC_NAME_MAX + 1];
    e->name_off = off; e->name_len = (uint8_t)len;
    memcpy(buf, r->names + off, len); buf[len] = '\0';
    e->id = dart_topic_id(buf);
}

static void i_dart_reflect_fold(i_DartNodeCore *c, i_DartReflect *r){
    i_DartHashPair *sorted; uint32_t n = 0, i;
    r->dirty = 0;
    r->n_ent = 0;
    for (i = 0; i < r->n_chan; i++) r->chan[i].entity = I_DART_NONE16;
    sorted = (i_DartHashPair*)i_dart_node_core_scratch(c, r->n_chan * (uint32_t)sizeof *sorted + 1u);
    if (!sorted) return;
    for (i = 0; i < r->n_chan; i++)
        if (r->chan[i].present){ sorted[n].hash = r->chan[i].hash; sorted[n].index = (uint16_t)i; n++; }
    i_dart_hash_sort(sorted, n);
    /* pass 1: primaries open entities and claim their partners */
    for (i = 0; i < r->n_chan; i++){
        i_DartChannel *ch = &r->chan[i];
        const i_DartKindRow *row;
        i_DartPeerEntity *e;
        const char *nm; size_t nl;
        uint16_t slot, p;
        if (!ch->present || ch->kind >= 8 || i_dart_reflect_hidden_hash(ch->hash)) continue;
        row = i_dart_kind_row(ch->kind);
        if (!row->primary) continue;
        if (ch->name_off != I_DART_NAME_NONE
            && i_dart_reflect_hidden(r->names + ch->name_off, ch->name_len)) continue;
        e = i_dart_reflect_entity_new(c, r);
        if (!e) return;
        slot = (uint16_t)(r->n_ent - 1u);
        e->kind = row->entity; e->primary = (uint16_t)i; e->hash = ch->hash;
        ch->entity = slot;
        if (ch->name_off == I_DART_NAME_NONE){
            if (row->suffix[0]) e->incomplete = 1;   /* partners need the name */
            continue;
        }
        nm = r->names + ch->name_off; nl = ch->name_len;
        if (row->suffix[0]){
            size_t sl = strlen(row->suffix);
            if (nl > sl && memcmp(nm + nl - sl, row->suffix, sl) == 0) nl -= sl;
            else e->incomplete = 1;                  /* a pattern kind without the convention */
        }
        i_dart_reflect_set_name(e, r, ch->name_off, nl);
        if (e->incomplete) continue;
        switch (ch->kind){
        case DART_KIND_FUNC_REQ:
            p = i_dart_reflect_partner(r, sorted, n, nm, nl, "@rsp", DART_KIND_FUNC_RSP);
            if (p != I_DART_NONE16) i_dart_reflect_attach(r, e, p, slot); else e->incomplete = 1;
            break;
        case DART_KIND_TASK_REQ:
            p = i_dart_reflect_partner(r, sorted, n, nm, nl, "@rsp", DART_KIND_TASK_RSP);
            if (p != I_DART_NONE16) i_dart_reflect_attach(r, e, p, slot); else e->incomplete = 1;
            p = i_dart_reflect_partner(r, sorted, n, nm, nl, "@prg", DART_KIND_TASK_PRG);
            if (p != I_DART_NONE16) i_dart_reflect_attach(r, e, p, slot); else e->incomplete = 1;
            break;
        case DART_KIND_VARIABLE:
            p = i_dart_reflect_partner(r, sorted, n, nm, nl, "@set", DART_KIND_VAR_SET);
            if (p != I_DART_NONE16) i_dart_reflect_attach(r, e, p, slot);
            break;
        default: break;
        }
    }
    /* pass 2: an unclaimed partner is a half pair or an unknown kind, surfaced incomplete */
    for (i = 0; i < r->n_chan; i++){
        i_DartChannel *ch = &r->chan[i];
        const i_DartKindRow *row;
        i_DartPeerEntity *e;
        uint16_t slot;
        if (!ch->present || ch->entity != I_DART_NONE16 || i_dart_reflect_hidden_hash(ch->hash)) continue;
        if (ch->name_off != I_DART_NAME_NONE
            && i_dart_reflect_hidden(r->names + ch->name_off, ch->name_len)) continue;
        row = i_dart_kind_row(ch->kind);
        e = i_dart_reflect_entity_new(c, r);
        if (!e) return;
        slot = (uint16_t)(r->n_ent - 1u);
        e->kind = (ch->kind < 8) ? row->entity : DART_ENTITY_TOPIC;
        e->hash = ch->hash; e->incomplete = 1;
        if (ch->kind < 8 && !row->primary) i_dart_reflect_attach(r, e, (uint16_t)i, slot);
        else { e->primary = (uint16_t)i; ch->entity = slot; }
        if (ch->name_off != I_DART_NAME_NONE){
            const char *nm = r->names + ch->name_off; size_t nl = ch->name_len;
            size_t sl = ch->kind < 8 ? strlen(row->suffix) : 0;
            if (sl && nl > sl && memcmp(nm + nl - sl, row->suffix, sl) == 0) nl -= sl;
            i_dart_reflect_set_name(e, r, ch->name_off, nl);
        }
    }
}

/* the reflection record behind a peer id, DART_SELF is this node, NULL if unknown */
static i_DartReflect *i_dart_node_core_reflect_of(i_DartNodeCore *c, uint32_t peer){
    i_DartNodePeerExtra *ex;
    if (peer == DART_SELF) return &c->self;
    ex = c->discovery ? (i_DartNodePeerExtra*)dart_discovery_peer_user(c->discovery, peer) : NULL;
    return (ex && ex->added) ? &ex->refl : NULL;
}

/* the channel of an entity that which names: 0 primary, 1 rsp, 2 prg */
static const i_DartChannel *i_dart_reflect_which(const i_DartReflect *r, const i_DartPeerEntity *e, int which){
    uint16_t idx = which == 1 ? e->rsp : which == 2 ? e->prg : e->primary;
    return idx != I_DART_NONE16 ? &r->chan[idx] : NULL;
}

/* the channel that says which side a node is on: the primary, else whichever partner exists */
static const i_DartChannel *i_dart_reflect_side(const i_DartReflect *r, const i_DartPeerEntity *e){
    if (e->primary != I_DART_NONE16) return &r->chan[e->primary];
    if (e->rsp != I_DART_NONE16) return &r->chan[e->rsp];
    if (e->prg != I_DART_NONE16) return &r->chan[e->prg];
    if (e->set != I_DART_NONE16) return &r->chan[e->set];
    return NULL;
}

static uint64_t i_dart_reflect_generation(const uint8_t uuid[16], const i_DartReflect *r,
                                          const i_DartPeerEntity *e){
    uint64_t h = 1469598103934665603ull;
    const i_DartChannel *p = e->primary != I_DART_NONE16 ? &r->chan[e->primary] : NULL;
    uint64_t hs = p ? p->schema_hash : 0;
    uint64_t hr = e->rsp != I_DART_NONE16 ? r->chan[e->rsp].schema_hash : 0;
    uint64_t hp = e->prg != I_DART_NONE16 ? r->chan[e->prg].schema_hash : 0;
    uint8_t attrs = p ? p->attrs : 0, set = (uint8_t)(e->set != I_DART_NONE16);
    if (uuid) h = i_dart_node_core_fnv(h, uuid, 16);
    h = i_dart_node_core_fnv(h, &hs, sizeof hs);
    h = i_dart_node_core_fnv(h, &hr, sizeof hr);
    h = i_dart_node_core_fnv(h, &hp, sizeof hp);
    h = i_dart_node_core_fnv(h, &attrs, 1);
    h = i_dart_node_core_fnv(h, &set, 1);
    h = i_dart_node_core_fnv(h, &e->kind, 1);
    return h;
}

/* the uuid behind a peer id, NULL if unknown. DART_SELF is our own */
static const uint8_t *i_dart_node_core_uuid_of(i_DartNodeCore *c, uint32_t peer, DartDiscoveryPeer *scratch){
    uint16_t q, np;
    if (!c->discovery) return NULL;
    if (peer == DART_SELF) return dart_discovery_uuid(c->discovery);
    np = dart_discovery_max_peers(c->discovery);
    for (q = 0; q < np; q++)
        if (dart_discovery_peer_at(c->discovery, q, scratch) && scratch->id == peer) return scratch->uuid;
    return NULL;
}

/* fills the public view of one entity slot at one node */
static void i_dart_reflect_info(i_DartNodeCore *c, uint32_t peer, const i_DartReflect *r,
                                uint16_t slot, DartEntityInfo *out){
    const i_DartPeerEntity *e = &r->ent[slot];
    const i_DartChannel *p = e->primary != I_DART_NONE16 ? &r->chan[e->primary] : NULL;
    const i_DartChannel *side = i_dart_reflect_side(r, e);
    DartDiscoveryPeer scratch;
    memset(out, 0, sizeof *out);
    out->kind = (DartEntityKind)e->kind;
    out->name = e->name_len ? dart_string(r->names + e->name_off, e->name_len) : dart_string(NULL, 0);
    out->hash = e->hash;
    out->incomplete = e->incomplete;
    if (side){
        const i_DartKindRow *row = i_dart_kind_row(side->kind);
        int pubs = dart_role_pubs(side->role), subs = dart_role_subs(side->role);
        out->provides = (uint8_t)(row->provider_pubs ? pubs : subs);
        out->consumes = (uint8_t)(row->provider_pubs ? subs : pubs);
        out->reliable = side->reliable;
    }
    if (p){
        out->schema = p->schema; out->schema_hash = p->schema_hash;
        out->forceable   = (uint8_t)((p->attrs & DART_ATTR_FORCEABLE)   ? 1 : 0);
        out->cancellable = (uint8_t)((p->attrs & DART_ATTR_CANCELLABLE) ? 1 : 0);
        out->exclusive   = (uint8_t)((p->attrs & DART_ATTR_EXCLUSIVE)   ? 1 : 0);
        out->multi       = (uint8_t)((p->attrs & DART_ATTR_MULTI)       ? 1 : 0);
    }
    if (e->rsp != I_DART_NONE16){ out->rsp_schema = r->chan[e->rsp].schema; out->rsp_schema_hash = r->chan[e->rsp].schema_hash; }
    if (e->prg != I_DART_NONE16){ out->progress_schema = r->chan[e->prg].schema; out->progress_schema_hash = r->chan[e->prg].schema_hash; }
    out->writable = (uint8_t)(e->kind == DART_ENTITY_VARIABLE && e->set != I_DART_NONE16);
    out->providers = out->provides; out->consumers = out->consumes;
    out->provider = peer;
    out->from = peer == DART_SELF ? c->self_name : i_dart_node_core_peer_name(c, peer);
    out->generation = i_dart_reflect_generation(i_dart_node_core_uuid_of(c, peer, &scratch), r, e);
}

/* the mesh table */

typedef struct {
    uint64_t id; uint64_t last_heard; uint64_t schema_hash; const DartSchema *schema;
    uint32_t peer; uint16_t slot; uint8_t kind, provides, consumes;
} i_DartMeshRec;

static int i_dart_mesh_rec_less(const i_DartMeshRec *a, const i_DartMeshRec *b){
    if (a->kind != b->kind) return a->kind < b->kind;
    if (a->id != b->id) return a->id < b->id;
    return a->peer < b->peer;
}
static void i_dart_mesh_sort(i_DartMeshRec *a, uint32_t n){
    uint32_t gap, i, j;
    for (gap = n / 2u; gap > 0; gap /= 2u)
        for (i = gap; i < n; i++){
            i_DartMeshRec v = a[i];
            for (j = i; j >= gap && i_dart_mesh_rec_less(&v, &a[j-gap]); j -= gap) a[j] = a[j-gap];
            a[j] = v;
        }
}

/* one node's entities as records. last_heard orders rival providers by freshness */
static uint32_t i_dart_mesh_gather(i_DartNodeCore *c, i_DartMeshRec *recs, uint32_t n, uint32_t cap,
                                   uint32_t peer, i_DartReflect *r, uint64_t last_heard){
    uint32_t i;
    if (r->dirty) i_dart_reflect_fold(c, r);
    for (i = 0; i < r->n_ent && n < cap; i++){
        const i_DartPeerEntity *e = &r->ent[i];
        const i_DartChannel *side = i_dart_reflect_side(r, e);
        const i_DartKindRow *row;
        i_DartMeshRec *m;
        if (!e->id || !side) continue;               /* unnamed yet: cannot key it */
        row = i_dart_kind_row(side->kind);
        m = &recs[n++];
        m->id = e->id; m->kind = e->kind; m->peer = peer; m->slot = (uint16_t)i;
        m->last_heard = last_heard;
        m->provides = (uint8_t)(row->provider_pubs ? dart_role_pubs(side->role) : dart_role_subs(side->role));
        m->consumes = (uint8_t)(row->provider_pubs ? dart_role_subs(side->role) : dart_role_pubs(side->role));
        m->schema = e->primary != I_DART_NONE16 ? r->chan[e->primary].schema : NULL;
        m->schema_hash = e->primary != I_DART_NONE16 ? r->chan[e->primary].schema_hash : 0;
    }
    return n;
}

static void i_dart_node_core_mesh_build(i_DartNodeCore *c){
    i_DartMeshRec *recs; uint32_t total = c->self.n_ent + 16u, n = 0, i;
    uint16_t s, np = c->discovery ? dart_discovery_max_peers(c->discovery) : 0;
    c->mesh_dirty = 0;
    c->n_mesh = 0;
    if (c->self.dirty) i_dart_reflect_fold(c, &c->self);
    total = c->self.n_ent + 16u;
    for (s = 0; s < np; s++){
        DartDiscoveryPeer v; i_DartNodePeerExtra *ex;
        if (!dart_discovery_peer_at(c->discovery, s, &v) || v.liveness != DART_PEER_ACTIVE) continue;
        ex = (i_DartNodePeerExtra*)v.user;
        if (!ex || !ex->added) continue;
        if (ex->refl.dirty) i_dart_reflect_fold(c, &ex->refl);
        total += ex->refl.n_ent;
    }
    recs = (i_DartMeshRec*)i_dart_node_core_scratch(c, total * (uint32_t)sizeof *recs);
    if (!recs) return;
    n = i_dart_mesh_gather(c, recs, n, total, DART_SELF, &c->self, ~(uint64_t)0);
    for (s = 0; s < np; s++){
        DartDiscoveryPeer v; i_DartNodePeerExtra *ex;
        if (!dart_discovery_peer_at(c->discovery, s, &v) || v.liveness != DART_PEER_ACTIVE) continue;
        ex = (i_DartNodePeerExtra*)v.user;
        if (!ex || !ex->added) continue;
        n = i_dart_mesh_gather(c, recs, n, total, v.id, &ex->refl, v.last_heard_us);
    }
    i_dart_mesh_sort(recs, n);
    if (!i_dart_node_core_array_reserve(c, (void**)&c->mesh, &c->cap_mesh, n ? n : 1u, sizeof *c->mesh)) return;
    for (i = 0; i < n; ){
        i_DartMeshEntity *m = &c->mesh[c->n_mesh++];
        const i_DartMeshRec *pick = NULL, *first_consumer = NULL;
        uint32_t j, k;
        memset(m, 0, sizeof *m);
        m->id = recs[i].id; m->kind = recs[i].kind;
        for (j = i; j < n && recs[j].kind == m->kind && recs[j].id == m->id; j++){
            const i_DartMeshRec *rec = &recs[j];
            if (rec->provides){
                /* the first live provider takes the slot. A rival with a different schema
                   is a conflict and wins only if heard more recently */
                m->providers++;
                if (!pick) pick = rec;
                else if (rec->schema_hash != pick->schema_hash){
                    m->conflict = 1;
                    if (rec->last_heard > pick->last_heard) pick = rec;
                }
            }
            if (rec->consumes){ m->consumers++; if (!first_consumer) first_consumer = rec; }
        }
        if (pick){ m->has_provider = 1; m->provider = pick->peer; m->provider_slot = pick->slot; }
        else if (first_consumer){
            /* no live provider: the widest consumer declaration stands in */
            pick = first_consumer;
            for (k = i; k < j; k++){
                const i_DartMeshRec *rec = &recs[k];
                if (!rec->consumes || !rec->schema) continue;
                if (!pick->schema || (rec->schema_hash != pick->schema_hash
                                      && dart_schema_subset(pick->schema, rec->schema))) pick = rec;
            }
        }
        if (pick){
            m->from_peer = pick->peer; m->from_slot = pick->slot;
            /* a live endpoint that can neither read the pick nor be read by it is a conflict */
            for (k = i; k < j; k++){
                const i_DartMeshRec *rec = &recs[k];
                if (rec == pick || rec->schema_hash == pick->schema_hash || !rec->schema || !pick->schema) continue;
                if (!dart_schema_subset(rec->schema, pick->schema) && !dart_schema_subset(pick->schema, rec->schema))
                    m->conflict = 1;
            }
        }
        {   i_DartReflect *r = i_dart_node_core_reflect_of(c, m->from_peer);
            DartDiscoveryPeer scratch;
            const uint8_t *uuid = m->has_provider ? i_dart_node_core_uuid_of(c, m->provider, &scratch) : NULL;
            m->generation = r ? i_dart_reflect_generation(uuid, r, &r->ent[m->from_slot]) : 0;
        }
        i = j;
    }
}

static void i_dart_mesh_info(i_DartNodeCore *c, const i_DartMeshEntity *m, DartEntityInfo *out){
    i_DartReflect *r = i_dart_node_core_reflect_of(c, m->from_peer);
    if (!r){ memset(out, 0, sizeof *out); return; }
    i_dart_reflect_info(c, m->from_peer, r, m->from_slot, out);
    out->providers = m->providers; out->consumers = m->consumers;
    out->provides = (uint8_t)(m->providers > 0); out->consumes = (uint8_t)(m->consumers > 0);
    out->provider = m->has_provider ? m->provider : 0;
    out->conflict = m->conflict;
    out->generation = m->generation;
}

/* the seams the runtime's walks call, with the node lock held by the caller */

void i_dart_node_core_set_self_name(i_DartNodeCore *c, DartString name){ if (c) c->self_name = name; }

void i_dart_node_core_self_begin(i_DartNodeCore *c){
    uint32_t i;
    if (!c) return;
    for (i = 0; i < c->self.n_chan; i++) c->self.chan[i].present = 0;
    c->self.names_len = 0;
}
void i_dart_node_core_self_channel(i_DartNodeCore *c, uint16_t index, DartString name, uint8_t kind,
                                   uint8_t role, uint8_t reliable, uint8_t attrs, const DartSchema *schema){
    i_DartChannel *ch;
    char buf[DART_TOPIC_NAME_MAX + 1];
    if (!c || name.len == 0 || name.len > DART_TOPIC_NAME_MAX) return;
    ch = i_dart_reflect_channel(c, &c->self, index);
    if (!ch) return;
    memcpy(buf, name.data, name.len); buf[name.len] = '\0';
    ch->hash = (uint32_t)dart_topic_id(buf);
    ch->name_off = i_dart_reflect_name(c, &c->self, name);
    ch->name_len = (uint8_t)name.len;
    ch->kind = kind; ch->role = role; ch->reliable = reliable; ch->attrs = attrs;
    ch->schema = schema; ch->schema_hash = schema ? dart_schema_hash(schema) : 0;
    ch->present = (uint8_t)(role != DART_INACTIVE && ch->name_off != I_DART_NAME_NONE);
}
void i_dart_node_core_self_end(i_DartNodeCore *c){
    if (!c) return;
    c->self.dirty = 1;
    i_dart_node_core_mesh_bump(c);
}

int i_dart_node_core_peers_next(i_DartNodeCore *c, DartIter *it, DartPeerInfo *out){
    uint16_t np;
    if (!c || !c->discovery || !it || !out) return 0;
    np = dart_discovery_max_peers(c->discovery);
    for (; it->a < np; it->a++){
        DartDiscoveryPeer v; i_DartNodePeerExtra *ex;
        if (!dart_discovery_peer_at(c->discovery, (uint16_t)it->a, &v)) continue;
        ex = (i_DartNodePeerExtra*)v.user;
        it->a++;
        memset(out, 0, sizeof *out);
        out->id = v.id;
        memcpy(out->uuid, v.uuid, 16);
        out->name = i_dart_node_core_peer_name(c, v.id);
        out->address = ex ? dart_string(ex->refl.addr, ex->refl.addr_len) : dart_string(NULL, 0);
        out->liveness = v.liveness;
        out->last_heard_us = v.last_heard_us;
        out->epoch = ex ? ex->interest_epoch : 0;
        out->catching_up = (uint8_t)(v.adv_meta_version > v.meta_version);
        out->fragment_size = v.meta.data ? dart_meta_frag(v.meta) : 0;
        return 1;
    }
    return 0;
}

int i_dart_node_core_entities_next(i_DartNodeCore *c, uint32_t peer, DartIter *it, DartEntityInfo *out){
    i_DartReflect *r;
    if (!c || !it || !out) return 0;
    r = i_dart_node_core_reflect_of(c, peer);
    if (!r) return 0;
    if (r->dirty) i_dart_reflect_fold(c, r);
    if (it->a >= r->n_ent) return 0;
    i_dart_reflect_info(c, peer, r, (uint16_t)it->a, out);
    it->a++;
    return 1;
}

int i_dart_node_core_mesh_next(i_DartNodeCore *c, DartIter *it, DartEntityInfo *out){
    if (!c || !it || !out) return 0;
    if (c->mesh_dirty) i_dart_node_core_mesh_build(c);
    if (it->a >= c->n_mesh) return 0;
    i_dart_mesh_info(c, &c->mesh[it->a], out);
    it->a++;
    return 1;
}

static const i_DartMeshEntity *i_dart_node_core_mesh_lookup(i_DartNodeCore *c, uint8_t kind, uint64_t id){
    uint32_t lo = 0, hi;
    if (c->mesh_dirty) i_dart_node_core_mesh_build(c);
    hi = c->n_mesh;
    while (lo < hi){
        uint32_t mid = (lo + hi) / 2u;
        const i_DartMeshEntity *m = &c->mesh[mid];
        if (m->kind < kind || (m->kind == kind && m->id < id)) lo = mid + 1; else hi = mid;
    }
    return (lo < c->n_mesh && c->mesh[lo].kind == kind && c->mesh[lo].id == id) ? &c->mesh[lo] : NULL;
}

int i_dart_node_core_mesh_find(i_DartNodeCore *c, DartEntityKind kind, const char *name, DartEntityInfo *out){
    const i_DartMeshEntity *m;
    if (!c || !name || !out) return 0;
    m = i_dart_node_core_mesh_lookup(c, (uint8_t)kind, dart_topic_id(name));
    if (!m) return 0;
    i_dart_mesh_info(c, m, out);
    return 1;
}

uint32_t i_dart_node_core_mesh_epoch(i_DartNodeCore *c){ return c ? c->mesh_epoch : 0; }

int i_dart_node_core_reflect_pick(i_DartNodeCore *c, DartEntityKind kind, const char *name, int which,
                                  int writer, const DartSchema **schema, uint8_t *reliable,
                                  uint64_t *generation){
    const i_DartMeshEntity *m;
    const DartSchema *best = NULL; uint64_t best_hash = 0;
    uint8_t rel = 0; int found = 0;
    uint64_t id;
    uint16_t s, np;
    if (schema) *schema = NULL;
    if (reliable) *reliable = 0;
    if (generation) *generation = 0;
    if (!c || !name) return 0;
    id = dart_topic_id(name);
    m = i_dart_node_core_mesh_lookup(c, (uint8_t)kind, id);
    if (!m) return 0;
    if (generation) *generation = m->generation;
    /* a reader takes the provider's declaration for this channel */
    if (!writer && m->has_provider){
        i_DartReflect *r = i_dart_node_core_reflect_of(c, m->provider);
        const i_DartChannel *ch = r ? i_dart_reflect_which(r, &r->ent[m->provider_slot], which) : NULL;
        if (ch){
            if (schema) *schema = ch->schema;
            if (reliable) *reliable = ch->reliable;
            return 1;
        }
    }
    /* a writer, or a reader with no provider: the widest live declaration, reliable if
       any reader of the channel requests it */
    np = c->discovery ? dart_discovery_max_peers(c->discovery) : 0;
    for (s = 0; s <= np; s++){
        i_DartReflect *r; uint32_t i;
        if (s == np) r = &c->self;
        else {
            DartDiscoveryPeer v; i_DartNodePeerExtra *ex;
            if (!dart_discovery_peer_at(c->discovery, s, &v) || v.liveness != DART_PEER_ACTIVE) continue;
            ex = (i_DartNodePeerExtra*)v.user;
            if (!ex || !ex->added) continue;
            r = &ex->refl;
        }
        if (r->dirty) i_dart_reflect_fold(c, r);
        for (i = 0; i < r->n_ent; i++){
            const i_DartPeerEntity *e = &r->ent[i];
            const i_DartChannel *ch;
            if (e->kind != (uint8_t)kind || e->id != id) continue;
            ch = i_dart_reflect_which(r, e, which);
            if (!ch) continue;
            found = 1;
            if (dart_role_subs(ch->role) && ch->reliable) rel = 1;
            if (!ch->schema) continue;
            if (!best || (ch->schema_hash != best_hash && dart_schema_subset(best, ch->schema))){
                best = ch->schema; best_hash = ch->schema_hash;
            }
        }
    }
    if (schema) *schema = best;
    if (reliable) *reliable = rel;
    return found;
}

/* Observer mode: appends every advertised but uncached index to the want list. INACTIVE
 * entries are skipped, since the responder would not answer them. */
static i_DartNodePeerExtra *i_dart_node_core_peer_extra(i_DartNodeCore *c, uint32_t id);
static uint16_t i_dart_node_core_greedy_extend(i_DartNodeCore *c, uint32_t peer,
                            DartBytes interest, DartDetailWant *wants, uint16_t n,
                            uint16_t max_wants){
    DartInterestIter it; DartTopicEntry e;
    uint16_t k;
    memset(&it, 0, sizeof it);
    while (n < max_wants && dart_interest_next(interest, &it, &e)){
        /* the iterator skips INACTIVE entries and hole runs, a PUBSUB double yield dedupes below */
        {   i_DartNodePeerExtra *ex = i_dart_node_core_peer_extra(c, peer);
            if (ex && e.index < ex->refl.n_chan && ex->refl.chan[e.index].name_off != I_DART_NAME_NONE) continue; }
        for (k = 0; k < n; k++) if (wants[k].index == e.index) break;
        if (k < n) continue;
        wants[n].index = e.index;
        wants[n].schema_hash = 0;   /* force the wire inline, we may not hold that schema */
        n++;
    }
    return n;
}

/* why the intern returned NULL, as text */
static char *i_dart_node_core_intern_why(i_DartNodeCore *c, DartBytes wire, char *p, char *end){
    if (!c->alloc)
        return i_dart_event_append_str(p, end, "no allocator here to parse peer schemas");
    if (!wire.data || wire.len == 0)
        return i_dart_event_append_str(p, end,
            "their schema wire is unavailable (not inlined in the detail response)");
    p = i_dart_event_append_str(p, end, "their schema wire was rejected: malformed, "
            "hash-mismatched, or a different DART schema wire version than ours (v");
    p = i_dart_event_append_u64(p, end, DART_SCHEMA_WIRE_VERSION);
    return i_dart_event_append_str(p, end, ")");
}

/* The verdict. A refusal writes its reason into [p, end], where end is the last writable
 * byte, and p == end means no text. The wrapper below records or clears the reason. */
static int i_dart_node_core_schema_verdict(i_DartNodeCore *c, uint32_t peer, uint16_t topic_index,
                                           int peer_is_pub, uint64_t hash, DartBytes wire,
                                           char *p, char *end){
    const DartSchema *ours = (topic_index < c->n_topics) ? c->chan_compiled[topic_index] : NULL;
    int peer_has = hash != 0;
    if (peer_is_pub){                                   /* their publish side: we would read */
        if (!ours){                                     /* a generic reader decodes with theirs */
            i_dart_node_core_peer_schema_set(c, peer, topic_index,
                peer_has ? i_dart_node_core_intern(c, hash, wire) : NULL);
            return 1;
        }
        if (!peer_has){                                 /* a typed reader refuses untyped */
            p = i_dart_event_append_str(p, end, "their writer has no schema, our typed reader refuses");
            *p = '\0'; return 0;
        }
        if (hash == dart_schema_hash(ours)){            /* identical: our own view works */
            i_dart_node_core_peer_schema_set(c, peer, topic_index, i_DART_NODE_SCHEMA_OURS);
            return 1;
        }
        {   DartSchema *pub = i_dart_node_core_intern(c, hash, wire);   /* the wire verifies it */
            DartSchema *view;
            if (!pub){
                p = i_dart_node_core_intern_why(c, wire, p, end);
                *p = '\0'; return 0;
            }
            if (!dart_schema_subset_why(ours, pub, p, (size_t)(end - p) + 1u)) return 0;
            view = i_dart_node_core_bind(c, hash, topic_index, ours, pub);
            if (!view){                                 /* OOM: refuse rather than misdecode */
                p = i_dart_event_append_str(p, end, "out of memory binding the reader view");
                *p = '\0'; return 0;
            }
            i_dart_node_core_peer_schema_set(c, peer, topic_index, view);
            return 1;
        }
    } else {                                            /* their subscribe side: we would write */
        if (!peer_has) return 1;                        /* a generic reader takes anything */
        if (!ours){                                     /* a typed reader refuses our raw topic */
            p = i_dart_event_append_str(p, end, "their reader is typed, our topic has no schema");
            *p = '\0'; return 0;
        }
        if (hash == dart_schema_hash(ours)) return 1;
        {   DartSchema *sub = i_dart_node_core_intern(c, hash, wire);
            if (!sub){
                p = i_dart_node_core_intern_why(c, wire, p, end);
                *p = '\0'; return 0;
            }
            return dart_schema_subset_why(sub, ours, p, (size_t)(end - p) + 1u);
        }
    }
}

int i_dart_node_core_schema_check(i_DartNodeCore *c, uint32_t peer, uint16_t topic_index,
                                  int peer_is_pub, uint64_t hash, DartBytes wire){
#ifndef DART_NO_DIAG
    char why[DART__SCHEMA_WHY_MAX];
    int ok;
    why[0] = '\0';
    ok = i_dart_node_core_schema_verdict(c, peer, topic_index, peer_is_pub, hash, wire,
                                         why, why + sizeof why - 1);
    if (ok) i_dart_node_core_schema_why_drop(c, peer, topic_index, peer_is_pub);
    else    i_dart_node_core_schema_why_set (c, peer, topic_index, peer_is_pub, why);
    return ok;
#else
    char why[1];
    return i_dart_node_core_schema_verdict(c, peer, topic_index, peer_is_pub, hash, wire, why, why);
#endif
}

/* Rebuilds our overlay from the core's fields. The whole announce must fit one datagram,
   else the bootstrap form goes out and peers page the interest. See spec/interest.md. */
uint16_t i_dart_node_core_build_meta(i_DartNodeCore *c){
    uint16_t need = dart_transport_meta_size(c->transport);
    int external = ((size_t)DART_DISCOVERY_META_OFF + DART_DISCOVERY_DISC_MAX + need
                    > (size_t)DART_DGRAM_MAX);
    if (external) need = dart_transport_meta_bootstrap_size();
    if (need > c->meta_cap){   /* size the blob buffer to the exact content */
        uint8_t *nb = (uint8_t*)c->alloc(c->alloc_user, c->meta_buf, need);
        if (!nb) return c->meta_len;   /* OOM: keep the previous blob, stale but consistent */
        c->meta_buf = nb; c->meta_cap = need;
    }
    c->meta_len = dart_transport_meta_build(c->transport, c->meta_buf, c->meta_cap,
                                  c->frag_size, c->oob_capable, c->oob_host, external);
    return c->meta_len;
}

DartBytes i_dart_node_core_meta(i_DartNodeCore *c){
    return dart_bytes(c->meta_buf, c->meta_len);
}

/* Answers a DETAIL_REQ into the core's grown scratch. One page per response, so it never
   IP fragments. A header only response still tells the requester the indices are gone. */
DartBytes i_dart_node_core_detail_respond(i_DartNodeCore *c, uint16_t domain, DartBytes req){
    size_t need, len;
    if (!c || !c->alloc || !c->discovery) return dart_bytes(NULL, 0);
    if (dart_detail_kind(req) != DART_DETAIL_REQ || dart_detail_domain(req) != domain)
        return dart_bytes(NULL, 0);
    need = dart_transport_detail_resp_size(c->transport, c->chan_schemas, req);
    if (!need) return dart_bytes(NULL, 0);
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
/* A peer can receive our shared memory payload iff we are capable, it advertised a host
 * id, and that host equals ours. The core knows nothing of SHM beyond this. */
static void i_dart_node_core_set_peer_oob(i_DartNodeCore *c, uint32_t id, DartBytes meta){
    uint8_t host[16];
    int oob = c->oob_capable && dart_meta_shm(meta, host) &&
              memcmp(host, c->oob_host, 16) == 0;
    dart_transport_peer_set_shm(c->transport, id, oob);
}
#else
#define i_dart_node_core_set_peer_oob(c, id, meta) ((void)0)
#endif

/* fires PEER_UP or PEER_DOWN */
static void i_dart_node_core_fire(i_DartNodeCore *c, DartEventKind kind, uint32_t id,
                            const DartDiscoveryAddr *addr){
    DartEvent ev;
    if (!c->on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind; ev.peer = id; ev.user = c->user;
    if (addr){ memcpy(ev.ip, addr->ip, 16); ev.ip_len = addr->ip_len; ev.port = addr->port; }
    c->on_event(&ev);
}

/* fires a DART_ERROR. too_big carries the OOM or meta too big byte count */
static void i_dart_node_core_fire_error(i_DartNodeCore *c, DartErrorKind err, uint32_t id,
                            const DartDiscoveryAddr *addr, uint64_t too_big){
    DartEvent ev;
    if (!c->on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = DART_ERROR; ev.error = err; ev.peer = id; ev.user = c->user; ev.too_big_bytes = too_big;
    if (addr){ memcpy(ev.ip, addr->ip, 16); ev.ip_len = addr->ip_len; ev.port = addr->port; }
    c->on_event(&ev);
}

/* Every reflected interest change funnels through here, so this is also where the peer's
 * interest epoch bumps. It must bump even with no event handler. */
static void i_dart_node_core_fire_interest(i_DartNodeCore *c, uint32_t id){
    DartEvent ev; uint16_t publish_to = 0, receive_from = 0;
    {   i_DartNodePeerExtra *ex = c->discovery
            ? (i_DartNodePeerExtra*)dart_discovery_peer_user(c->discovery, id) : NULL;
        if (ex){
            uint32_t mv = 0; DartBytes meta = dart_discovery_peer_meta(c->discovery, id, &mv);
            ex->interest_epoch++;
            i_dart_node_core_reflect_interest(c, ex, i_dart_node_core_interest_of(ex, meta, mv));
            i_dart_node_core_mesh_bump(c);
        }
    }
    if (!c->on_event) return;
    dart_transport_peer_match_counts(c->transport, id, &publish_to, &receive_from);
    memset(&ev, 0, sizeof ev);
    ev.kind = DART_PEER_INTEREST; ev.peer = id;
    ev.publish_topics = publish_to; ev.receive_topics = receive_from;
    ev.user = c->user;
    c->on_event(&ev);
}

/* the peer's lifecycle state in the discovery scratch. NULL only before discovery is bound */
static i_DartNodePeerExtra *i_dart_node_core_peer_extra(i_DartNodeCore *c, uint32_t id){
    return (i_DartNodePeerExtra*)dart_discovery_peer_user(c->discovery, id);
}

/* The one read point for a peer's interest: the inline section, or the assembled external
 * blob at the advertised version, or {NULL,0} while a fetch is in flight. */
static DartBytes i_dart_node_core_interest_of(i_DartNodePeerExtra *ex, DartBytes meta,
                            uint32_t meta_version){
    if (!meta.data) return dart_bytes(NULL, 0);
    if (!dart_meta_interest_external(meta)) return dart_meta_interest(meta);
    if (ex && ex->interest_buf && ex->interest_version == meta_version)
        return dart_bytes(ex->interest_buf, ex->fetch_len);
    return dart_bytes(NULL, 0);
}

/* Queues an INTEREST_REQ for an external peer whose blob is not assembled at its current
 * version. The version dedup makes a steady state announce free. */
static void i_dart_node_core_interest_check(i_DartNodeCore *c, i_DartNodePeerExtra *ex,
                            DartBytes meta, uint32_t meta_version){
    if (!ex || !meta.data || !dart_meta_interest_external(meta)) return;
    if (ex->interest_version == meta_version) return;
    ex->interest_due = 1;
    c->detail_due_any = 1;
}

/* After an apply: queue a DETAIL_REQ while unverified candidates remain. Runs on every
   apply, so the pending state is the retry state and a lost datagram heals. */
static void i_dart_node_core_detail_check(i_DartNodeCore *c, i_DartNodePeerExtra *ex,
                            uint32_t id, DartBytes interest){
    DartDetailWant probe;
    if (!interest.data) return;
    if (dart_transport_detail_wants(c->transport, c->chan_schemas, id, interest, NULL, 0)
        || (c->fetch_details
            && i_dart_node_core_greedy_extend(c, id, interest, &probe, 0, 1))){
        ex->detail_due = 1;
        c->detail_due_any = 1;
    }
}

static void i_dart_node_core_peer_up(i_DartNodeCore *c, uint32_t id, const DartDiscoveryAddr *addr,
                            DartBytes meta){
    i_DartNodePeerExtra *ex = i_dart_node_core_peer_extra(c, id);
    uint16_t frag = dart_meta_frag(meta);
    uint32_t meta_version = 0;
    DartBytes interest;
    if (!ex) return;                                  /* discovery not bound */
    dart_discovery_peer_meta(c->discovery, id, &meta_version);
    interest = i_dart_node_core_interest_of(ex, meta, meta_version);
    if (!ex->added){                                  /* a new peer: wire it into the transport */
        i_dart_node_core_peer_schema_clear(c, id);    /* a recycled id: no stale bindings */
        i_dart_reflect_free(c, &ex->refl);
        i_dart_node_core_schema_why_clear(c, id);
        dart_transport_peer_add(c->transport, id, frag);
        ex->added = 1; ex->dormant = 0; ex->detail_due = 0;
        if (addr) i_dart_reflect_format_addr(&ex->refl, addr);
        i_dart_node_core_mesh_bump(c);
        i_dart_node_core_set_peer_oob(c, id, meta);
        i_dart_node_core_fire(c, DART_PEER_UP, id, addr);
        if (interest.data){ dart_transport_apply_peer_interest(c->transport, id, interest);
                       i_dart_node_core_fire_interest(c, id);
                       i_dart_node_core_detail_check(c, ex, id, interest); }
    } else {                                          /* a known peer: an update */
        dart_transport_peer_set_frag(c->transport, id, frag);
        i_dart_node_core_set_peer_oob(c, id, meta);
        if (interest.data){ dart_transport_apply_peer_interest(c->transport, id, interest);
                       i_dart_node_core_fire_interest(c, id);
                       i_dart_node_core_detail_check(c, ex, id, interest); }
        if (ex->dormant){    /* a DROPPED peer's same incarnation returned: resume */
            ex->dormant = 0;
            dart_transport_peer_resume(c->transport, id);
            i_dart_node_core_mesh_bump(c);
            i_dart_node_core_fire(c, DART_PEER_UP, id, addr);
        }
    }
    i_dart_node_core_interest_check(c, ex, meta, meta_version);   /* external and stale: pull it */
}

/* Ingests a DETAIL_RESP: caches the verdicts, then re applies the peer's current interest
   so they form their matches exactly as a fresh announce would. Idempotent. */
void i_dart_node_core_apply_details(i_DartNodeCore *c, uint16_t domain, uint32_t peer,
                            DartBytes resp){
    i_DartNodePeerExtra *ex;
    DartBytes meta, interest;
    if (!c || dart_detail_kind(resp) != DART_DETAIL_RESP || dart_detail_domain(resp) != domain)
        return;
    ex = i_dart_node_core_peer_extra(c, peer);
    if (!ex || !ex->added) return;
    {   /* a response older than the blob we hold may describe a binding the peer has since
           rebound: refuse it, the pending state re asks */
        uint32_t held = 0;
        dart_discovery_peer_meta(c->discovery, peer, &held);
        if (held && dart_detail_meta_version(resp) < held) return;
    }
    {   /* every detail received feeds reflection */
        DartDetailIter it; DartDetail dd;
        memset(&it, 0, sizeof it);
        while (dart_detail_next(resp, &it, &dd)) i_dart_node_core_reflect_detail(c, ex, &dd);
        if (ex->refl.dirty) i_dart_node_core_mesh_bump(c);
    }
    if (!dart_transport_apply_peer_details(c->transport, peer, resp)) return;   /* nothing new */
    {   uint32_t meta_version = 0;
        meta = dart_discovery_peer_meta(c->discovery, peer, &meta_version);
        interest = i_dart_node_core_interest_of(ex, meta, meta_version);
    }
    if (!interest.data) return;
    dart_transport_apply_peer_interest(c->transport, peer, interest);
    i_dart_node_core_fire_interest(c, peer);
    i_dart_node_core_detail_check(c, ex, peer, interest);
}

/* Queues a request for every active peer: the periodic retry sweep. A converged peer
   costs one wants walk and sends nothing. */
void i_dart_node_core_detail_rearm(i_DartNodeCore *c){
    uint16_t s, n;
    if (!c || !c->discovery) return;
    n = dart_discovery_max_peers(c->discovery);
    for (s=0;s<n;s++){
        DartDiscoveryPeer v; i_DartNodePeerExtra *ex;
        if (!dart_discovery_peer_at(c->discovery, s, &v)) continue;
        if (v.liveness != DART_PEER_ACTIVE) continue;
        ex = (i_DartNodePeerExtra*)v.user;
        if (ex && ex->added){
            ex->detail_due = 1; c->detail_due_any = 1;
            i_dart_node_core_interest_check(c, ex, v.meta, v.meta_version);
        }
    }
}

int i_dart_node_core_detail_any(i_DartNodeCore *c){ return c ? c->detail_due_any : 0; }

/* A request from peer named our blob version: proof it applied our announce at it. When
 * the advance releases a held writer lane, the interest event re fires. */
int i_dart_node_core_seen_version(i_DartNodeCore *c, uint32_t peer, uint32_t version){
    if (!c || !version) return 0;
    if (!dart_transport_peer_seen_version(c->transport, peer, version)) return 0;
    i_dart_node_core_fire_interest(c, peer);
    return 1;
}

/* A peer counts once while its interest is unknown (no blob yet, or a fetch in flight),
 * else by its unresolved entries. Dropped peers are skipped, they will not answer. */
int i_dart_node_core_topic_unresolved(i_DartNodeCore *c, uint16_t topic_index){
    uint16_t s, n; int cnt = 0;
    if (!c || !c->discovery) return 0;
    n = dart_discovery_max_peers(c->discovery);
    for (s=0;s<n;s++){
        DartDiscoveryPeer v; i_DartNodePeerExtra *ex;
        DartBytes interest;
        if (!dart_discovery_peer_at(c->discovery, s, &v)) continue;
        if (v.liveness != DART_PEER_ACTIVE) continue;
        ex = (i_DartNodePeerExtra*)v.user;
        if (!ex || !ex->added) continue;
        if (!v.meta.data){ cnt++; continue; }   /* the blob is still being fetched */
        interest = i_dart_node_core_interest_of(ex, v.meta, v.meta_version);
        if (!interest.data){
            if (dart_meta_interest_external(v.meta)) cnt++;   /* a fetch in flight: resolving */
            continue;                           /* else no interest: nothing to resolve */
        }
        cnt += dart_transport_topic_unresolved(c->transport, topic_index, v.id, interest);
    }
    return cnt;
}

void i_dart_node_core_topics_unresolved(i_DartNodeCore *c, uint16_t *counts, uint16_t n){
    uint16_t s, np, i;
    if (!counts || !n) return;
    memset(counts, 0, (size_t)n * sizeof *counts);
    if (!c || !c->discovery) return;
    np = dart_discovery_max_peers(c->discovery);
    for (s=0;s<np;s++){
        DartDiscoveryPeer v; i_DartNodePeerExtra *ex;
        DartBytes interest = dart_bytes(NULL, 0); int all = 0;
        if (!dart_discovery_peer_at(c->discovery, s, &v)) continue;
        if (v.liveness != DART_PEER_ACTIVE) continue;
        ex = (i_DartNodePeerExtra*)v.user;
        if (!ex || !ex->added) continue;
        if (!v.meta.data) all = 1;                        /* the blob is still being fetched */
        else {
            interest = i_dart_node_core_interest_of(ex, v.meta, v.meta_version);
            if (!interest.data && dart_meta_interest_external(v.meta)) all = 1;   /* fetching */
        }
        if (all){   /* its interest is unknown, so it may nominate any topic */
            for (i=0;i<n;i++) if (counts[i] != 0xFFFFu) counts[i]++;
            continue;
        }
        if (!interest.data) continue;                     /* no interest: nothing to resolve */
        dart_transport_peer_unresolved_fill(c->transport, v.id, interest, counts, n);
    }
}

/* Drains one queued request: INTEREST_REQs first, since an unassembled interest gates
   candidate discovery, then DETAIL_REQs, capped at 128 wants. Loop until 0. */
size_t i_dart_node_core_detail_req_next(i_DartNodeCore *c, uint16_t domain,
                            void *out, size_t cap, i_DartNodeDest *to){
    DartDetailWant wants[128];
    uint16_t s, n;
    if (!c || !c->discovery || cap < 24u) return 0;
    n = dart_discovery_max_peers(c->discovery);
    for (s=0;s<n;s++){    /* the interest pass: one INTEREST_REQ per due peer, cursor driven */
        DartDiscoveryPeer v; i_DartNodePeerExtra *ex;
        uint32_t offset;
        if (!dart_discovery_peer_at(c->discovery, s, &v)) continue;
        ex = (i_DartNodePeerExtra*)v.user;
        if (!ex || !ex->interest_due) continue;
        ex->interest_due = 0;
        if (!ex->added || v.liveness != DART_PEER_ACTIVE) continue;
        if (!dart_meta_interest_external(v.meta)) continue;      /* the flag cleared meanwhile */
        if (ex->interest_version == v.meta_version) continue;    /* assembled meanwhile */
        offset = (ex->fetch_version == v.meta_version) ? ex->fetch_cursor : 0;
        if (!i_dart_node_core_resolve(c, v.id, to)) continue;
        {   size_t len = dart_interest_req_build(domain, v.meta_version, offset, out, cap);
            if (len) return len;
        }
    }
    for (s=0;s<n;s++){
        DartDiscoveryPeer v; i_DartNodePeerExtra *ex;
        DartBytes interest; uint16_t nw, maxw;
        if (!dart_discovery_peer_at(c->discovery, s, &v)) continue;
        ex = (i_DartNodePeerExtra*)v.user;
        if (!ex || !ex->detail_due) continue;
        ex->detail_due = 0;
        if (!ex->added || v.liveness != DART_PEER_ACTIVE) continue;
        interest = i_dart_node_core_interest_of(ex, v.meta, v.meta_version);
        if (!interest.data) continue;
        maxw = (uint16_t)((cap - 14u) / 10u);
        if (maxw > 128u) maxw = 128u;
        nw = dart_transport_detail_wants(c->transport, c->chan_schemas, v.id, interest,
                                         wants, maxw);
        if (c->fetch_details)
            nw = i_dart_node_core_greedy_extend(c, v.id, interest, wants, nw, maxw);
        if (!nw) continue;
        if (!i_dart_node_core_resolve(c, v.id, to)) continue;
        {   size_t len = dart_detail_req_build(domain, v.meta_version, wants, nw, out, cap);
            if (len) return len;
        }
    }
    c->detail_due_any = 0;
    return 0;
}

/* Answers an INTEREST_REQ with one page of our interest blob, built into the scratch after
   a header's worth of headroom so the page is returned without a copy. Stateless. */
DartBytes i_dart_node_core_interest_respond(i_DartNodeCore *c, uint16_t domain, DartBytes req){
    uint32_t offset, total; size_t chunk, need, built;
    uint8_t *blob, *head;
    if (!c || !c->discovery) return dart_bytes(NULL, 0);
    if (dart_detail_kind(req) != DART_INTEREST_REQ || dart_detail_domain(req) != domain)
        return dart_bytes(NULL, 0);
    if (!dart_interest_req_offset(req, &offset)) return dart_bytes(NULL, 0);
    total = dart_transport_interest_size(c->transport);
    need  = (size_t)DART_INTEREST_RESP_HEAD + total;
    if (need > c->detail_cap){
        uint8_t *nb = (uint8_t*)c->alloc(c->alloc_user, c->detail_buf, need);
        if (!nb) return dart_bytes(NULL, 0);
        c->detail_buf = nb; c->detail_cap = (uint32_t)need;
    }
    blob  = c->detail_buf + DART_INTEREST_RESP_HEAD;
    built = dart_transport_build_interest(c->transport, blob, total);
    if (built != total) return dart_bytes(NULL, 0);   /* drifted: never serve garbage */
    if (offset > total) offset = total;               /* clamp: a header only page reports total */
    chunk = total - offset;
    if (chunk > (size_t)DART_DGRAM_MAX - DART_INTEREST_RESP_HEAD)
        chunk = (size_t)DART_DGRAM_MAX - DART_INTEREST_RESP_HEAD;   /* one sub datagram page */
    head = blob + offset - DART_INTEREST_RESP_HEAD;   /* the header sits right before the chunk */
    dart_interest_resp_head(domain, dart_discovery_meta_version(c->discovery), total,
                            offset, (uint16_t)chunk, head, DART_INTEREST_RESP_HEAD);
    return dart_bytes(head, DART_INTEREST_RESP_HEAD + chunk);
}

/* Ingests one INTEREST_RESP page at the cursor, re asks while incomplete, and on completion
   applies the blob as an inline announce would. A version change mid fetch restarts. */
void i_dart_node_core_apply_interest_page(i_DartNodeCore *c, uint16_t domain, uint32_t peer,
                            DartBytes resp){
    i_DartNodePeerExtra *ex;
    uint32_t total, offset, resp_version; DartBytes chunk;
    if (!c || dart_detail_kind(resp) != DART_INTEREST_RESP || dart_detail_domain(resp) != domain)
        return;
    ex = i_dart_node_core_peer_extra(c, peer);
    if (!ex || !ex->added) return;
    if (!dart_interest_resp_parse(resp, &total, &offset, &chunk)) return;
    if (total > dart_interest_max(0xFFFFu)) return;   /* an absurd total: never a real blob */
    resp_version = dart_detail_meta_version(resp);
    if (ex->interest_buf && ex->interest_version == resp_version) return;   /* a dup page */
    if (ex->fetch_version != resp_version){           /* first page, or the version moved */
        ex->fetch_version = resp_version;
        ex->fetch_cursor = 0; ex->fetch_len = total;
    }
    if (total != ex->fetch_len){ ex->fetch_cursor = 0; ex->fetch_len = total; }  /* drifted */
    if (offset != ex->fetch_cursor){                  /* out of order: re ask from the cursor */
        ex->interest_due = 1; c->detail_due_any = 1;
        return;
    }
    if (ex->interest_cap < total){
        uint8_t *nb = (uint8_t*)c->alloc(c->alloc_user, ex->interest_buf, total ? total : 1u);
        if (!nb){ i_dart_node_core_fire_error(c, DART_E_OOM, peer, NULL, total); return; }
        ex->interest_buf = nb; ex->interest_cap = total ? total : 1u;
    }
    if (chunk.len){ memcpy(ex->interest_buf + offset, chunk.data, chunk.len); ex->fetch_cursor += (uint32_t)chunk.len; }
    if (ex->fetch_cursor < total){
        if (chunk.len)                                /* progress: ask for the next page now */
            { ex->interest_due = 1; c->detail_due_any = 1; }
        return;                                       /* an empty short page: the sweep re asks */
    }
    ex->interest_version = resp_version;              /* assembled: the dedup that ends the cycle */
    ex->fetch_version = 0;
    dart_transport_apply_peer_interest(c->transport, peer, dart_bytes(ex->interest_buf, total));
    i_dart_node_core_fire_interest(c, peer);
    i_dart_node_core_detail_check(c, ex, peer, dart_bytes(ex->interest_buf, total));
}

static void i_dart_node_core_peer_down(i_DartNodeCore *c, uint32_t id, DartDiscoveryDownReason reason){
    i_DartNodePeerExtra *ex = i_dart_node_core_peer_extra(c, id);   /* freed after this event */
    if (reason == DART_DISCOVERY_DROP){
        /* fell silent: keep the transport state for a same incarnation resume, drop the
           peer from flow control and tell the app once */
        if (ex && ex->added && !ex->dormant){
            ex->dormant = 1;
            dart_transport_peer_dormant(c->transport, id);
            i_dart_node_core_mesh_bump(c);
            i_dart_node_core_fire(c, DART_PEER_DOWN, id, NULL);
        }
    } else {   /* GONE: free the transport state */
        int notify = (ex && ex->added && !ex->dormant);   /* the app was not told yet */
        dart_transport_peer_remove(c->transport, id);
        i_dart_node_core_peer_schema_clear(c, id);      /* the id may be reassigned */
        if (ex) i_dart_reflect_free(c, &ex->refl);
        i_dart_node_core_schema_why_clear(c, id);
        i_dart_node_core_mesh_bump(c);
        if (ex && ex->interest_buf){                    /* the retained external interest */
            c->alloc(c->alloc_user, ex->interest_buf, 0);
            ex->interest_buf = NULL; ex->interest_cap = 0;
            ex->interest_version = 0; ex->fetch_version = 0;
            ex->fetch_cursor = 0; ex->fetch_len = 0; ex->interest_due = 0;
        }
        if (notify) i_dart_node_core_fire(c, DART_PEER_DOWN, id, NULL);
    }
}

static void i_dart_node_core_peer_refused(i_DartNodeCore *c, const DartDiscoveryAddr *addr){
    i_dart_node_core_fire_error(c, DART_E_PEER_REFUSED, 0, addr, 0);
}

/* The discovery core's event sink, demuxed into the lifecycle handlers above. */
void i_dart_node_core_on_disc_event(const DartDiscoveryEvent *ev){
    i_DartNodeCore *c = (i_DartNodeCore*)ev->user;
    switch (ev->kind){
        case DART_DISCOVERY_PEER_UP:
            i_dart_node_core_peer_up(c, ev->peer, &ev->addr, ev->meta);
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
    if (!dart_discovery_addr_of_id(c->discovery, to, &a)) return 0;   /* the peer vanished */
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
    if (!name.data) return name;                          /* not a known peer */
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

uint16_t i_dart_node_peer_frag(const DartDiscoveryPeer *peer){
    return (peer && peer->meta.data) ? dart_meta_frag(peer->meta) : 0;
}

uint32_t i_dart_node_peer_interest_epoch(const DartDiscoveryPeer *peer){
    const i_DartNodePeerExtra *ex = peer ? (const i_DartNodePeerExtra*)peer->user : NULL;
    return ex ? ex->interest_epoch : 0;
}

int i_dart_node_peer_interest_next(const DartDiscoveryPeer *peer,
                                   DartInterestIter *it, DartTopicEntry *out){
    if (!peer || !peer->meta.data) return 0;
    /* one read point for both interest homes, so every walk resolves external peers alike */
    return dart_interest_next(
        i_dart_node_core_interest_of((i_DartNodePeerExtra*)peer->user, peer->meta,
                                     peer->meta_version),
        it, out);
}
