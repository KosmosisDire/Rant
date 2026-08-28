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
static char *i_dart_event_append_topic(char *p, char *end, const DartEvent *ev){   /* topic=<name> or topic=<index> */
    p = i_dart_event_append_str(p,end,"topic=");
    if (ev->topic_name) return i_dart_event_append_str(p,end,ev->topic_name);
    return i_dart_event_append_u64(p,end,ev->topic);
}
static char *i_dart_event_append_peer(char *p, char *end, const DartEvent *ev){   /* the peer's node name, else id=<n> */
    if (ev->peer_name && ev->peer_name[0]) return i_dart_event_append_str(p,end,ev->peer_name);
    p = i_dart_event_append_str(p,end,"id="); return i_dart_event_append_u64(p,end,ev->peer);
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
    p = buf; end = buf + cap - 1;                  /* reserve one byte for the NUL */
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
    *p = '\0';                                     /* p <= end = buf+cap-1, in range */
    return buf;
}

/* the node core's per-peer transport-lifecycle state, kept in the discovery peer's user
   scratch (so the node holds NO peer table of its own). added = wired into the transport
   yet (dart_transport_peer_add called); dormant = discovery DROPPED it, kept for a same-incarnation
   resume; detail_due / interest_due = a DETAIL_REQ / INTEREST_REQ should go to this peer.
   Discovery zeroes this when a new UUID takes the slot, preserves it on resume.

   The interest_* fields are the EXTERNAL-interest fetch (a peer whose announce sets
   INTEREST_EXTERNAL): interest_buf assembles the peer's paged interest blob (a hook
   allocation, freed on peer GONE; at node close the pool reset reclaims it);
   fetch_cursor/fetch_len walk it; fetch_version is the version being assembled (the
   RESP stamp; a differing stamp restarts at 0); interest_version is the last version
   FULLY assembled -- the dedup that keeps a steady-state announce from re-triggering
   the fetch. For an inline-interest peer all of it stays zero. */
typedef struct {
    uint8_t  added; uint8_t dormant; uint8_t detail_due; uint8_t interest_due;
    uint32_t interest_epoch;     /* bumped on EVERY reflected-interest change (interest apply,
                                    external assembly, fresh detail verdicts/names): the ONE
                                    cache key an observer needs (dart_node_peer_interest_epoch),
                                    covering changes the announce version cannot see. 0 = nothing
                                    applied yet. */
    uint32_t interest_version;   /* last fully assembled external version (0 = none) */
    uint32_t fetch_version;      /* version the cursor is assembling (0 = idle) */
    uint32_t fetch_cursor;       /* bytes assembled so far */
    uint32_t fetch_len;          /* the blob's total_len (valid once the first page lands;
                                    after assembly it is the retained blob's length) */
    uint8_t *interest_buf;
    uint32_t interest_cap;
} i_DartNodePeerExtra;

/* schema state for the gate + delivery: peer schemas interned by hash (parsed once),
   reader views cached per (peer schema, topic), and the per-(peer, topic) schema a
   delivered message decodes with. All flat, linearly scanned (counts stay small), and
   allocated through the injected hook so they survive an arena migrate untouched. An
   interned/rebased schema lives until node close; a stale map pointer therefore never
   dangles. */
typedef struct { uint64_t hash; DartSchema *parsed; } i_DartNodeSchemaIntern;
typedef struct { uint64_t hash; uint16_t topic; DartSchema *rebased; } i_DartNodeSchemaBind;
typedef struct { uint32_t peer; uint16_t topic; const DartSchema *schema; } i_DartNodePeerSchema;
/* the greedy detail cache (cfg.fetch_details): one fetched topic of one peer. The name
   is a hook allocation owned here; the schema is interned (lives until close). */
typedef struct { uint32_t peer; uint16_t index; uint8_t name_len; char *name;
                 uint64_t schema_hash; const DartSchema *schema; } i_DartNodeTopicDetail;
#ifndef DART_NO_DIAG
/* why the schema gate refused (peer, topic, direction): recorded at detail intake so
   the SCHEMA_MISMATCH event (fired later, at every interest apply) can say what was
   incompatible. Refusals are rare, so a flat scanned array; stripped by DART_NO_DIAG. */
#define DART__SCHEMA_WHY_MAX 128
typedef struct { uint32_t peer; uint16_t topic; uint8_t peer_is_pub;
                 char text[DART__SCHEMA_WHY_MAX]; } i_DartNodeSchemaWhy;
#endif

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
    DartMetaSchema       *chan_schemas;  /* per-topic schema advertisement (hash 0 = none) */
    const DartSchema    **chan_compiled; /* per-topic parsed schema (the gate's local side) */
    uint16_t              n_topics;    /* sizes the per-topic arrays (and the meta buffer) */
    DartAllocFn           alloc;         /* backs the schema state + announce blob (required) */
    void                 *alloc_user;
    uint8_t              *detail_buf;    /* detail-response scratch, hook-allocated + grown on
                                            demand (stable across a migrate; pool reset frees it) */
    uint32_t              detail_cap;
    uint8_t               detail_due_any;/* some peer's detail_due is set: the runtime drains
                                            i_dart_node_core_detail_req_next this poll */
    i_DartNodeSchemaIntern *interned;     uint32_t n_interned,     cap_interned;
    i_DartNodeSchemaBind   *binds;        uint32_t n_binds,        cap_binds;
    i_DartNodePeerSchema   *peer_schemas; uint32_t n_peer_schemas, cap_peer_schemas;
    uint8_t                 fetch_details; /* observer mode: fetch + cache every index */
    i_DartNodeTopicDetail  *topic_details; uint32_t n_topic_details, cap_topic_details;
#ifndef DART_NO_DIAG
    i_DartNodeSchemaWhy    *schema_whys;   uint32_t n_schema_whys,   cap_schema_whys;
#endif
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

/* arena layout: the core struct, then the per-topic schema registry (no peer table --
   that lives in the discovery core; the announce blob is hook-allocated at its ACTUAL
   size and grown at build time). One sequence so measure and build agree. */
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
    c->discovery     = cfg->discovery;     /* may be NULL now, bound via bind_discovery later */
    c->on_event      = cfg->on_event;      c->user = cfg->user;
    c->alloc         = cfg->alloc;         c->alloc_user = cfg->alloc_user;
    c->oob_capable   = cfg->oob_capable;
    c->fetch_details = cfg->fetch_details;
    memcpy(c->oob_host, cfg->oob_host, 16);
    c->meta_buf      = NULL;                                /* hook-allocated at the first build */
    c->meta_cap      = 0;
    c->frag_size     = cfg->frag_size;
    c->chan_schemas  = schemas;
    c->chan_compiled = compiled;
    c->n_topics    = cfg->n_topics;
    memset(schemas, 0, (size_t)cfg->n_topics * sizeof *schemas);
    memset(compiled, 0, (size_t)cfg->n_topics * sizeof *compiled);
    return c;
}

/* Relocate the sans-IO node core into a bigger block at grown counts. No peer table (it
 * lives in discovery); a struct copy carries the scalars. The transport, discovery, and
 * announce-blob pointers are re-pointed by the caller after those move. */
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
    *c = *old;                          /* scalars + transport/discovery ptrs (caller re-points);
                                           the hook-allocated schema arrays and announce blob are
                                           stable and ride along (the caller's rebuild grows the
                                           blob if the new counts need more) */
    keep = old->n_topics < new_n_topics ? old->n_topics : new_n_topics;
    memset(schemas, 0, (size_t)new_n_topics * sizeof *schemas);
    memcpy(schemas, old->chan_schemas, (size_t)keep * sizeof *schemas);   /* wire views stay valid: the
                                          DartSchema blocks live outside the arena and do not move */
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

/* The topic is being RETIRED: drop the live schema pointers (the node frees the parsed
 * copy) but KEEP the hash as the slot's binding fingerprint, so a later create of the
 * same name can tell an identical rebind (reuse with cached verdicts intact) from a
 * retype (reuse with a generation bump). The detail responder never answers a retired
 * slot, so the stale hash is never served. */
void i_dart_node_core_retire_topic_schema(i_DartNodeCore *c, uint16_t topic_index){
    if (!c || topic_index >= c->n_topics) return;
    c->chan_compiled[topic_index]     = NULL;
    c->chan_schemas[topic_index].wire = dart_bytes(NULL, 0);
}

/* The retired slot's binding fingerprint (0 = it carried no schema). */
uint64_t i_dart_node_core_topic_schema_hash(i_DartNodeCore *c, uint16_t topic_index){
    return (c && topic_index < c->n_topics) ? c->chan_schemas[topic_index].hash : 0;
}

/* ---- the schema gate + delivery binding ---------------------------------------------- */

/* a peer schema, parsed once per distinct hash. The claimed hash must equal the wire's
   real hash, or a lying peer could poison the intern for every honest one. NULL when the
   wire is absent (hash-only advert) or malformed. */
static DartSchema *i_dart_node_core_intern(i_DartNodeCore *c, uint64_t hash, DartBytes wire){
    uint32_t i; DartSchema *p;
    for (i = 0; i < c->n_interned; i++)
        if (c->interned[i].hash == hash) return c->interned[i].parsed;
    if (!wire.data || wire.len == 0){
        /* no wire inlined: identical hashes travel as ZERO bytes, so a channel whose
           schema we also hold (a matched channel, greedily cached) can still intern.
           Parse our OWN wire into a fresh copy: a topic retire frees its parsed schema,
           an interned one lives until close. */
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

/* the delivery map: which schema decodes (peer, topic). NULL entries are stored too
   (they overwrite an older binding when a peer re-advertises without a schema). An entry
   whose publisher's schema is IDENTICAL to ours stores this sentinel instead of the
   compiled pointer, resolved to chan_compiled at read time: the compiled copy is freed
   and re-parsed across a topic retire/reuse cycle, and a live pointer here would dangle
   across it while the (kept) verdicts still expect the entry to decode. */
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

/* drop a peer's map entries (GONE, or its id recycled onto a new peer) */
static void i_dart_node_core_peer_schema_clear(i_DartNodeCore *c, uint32_t peer){
    uint32_t i = 0;
    while (i < c->n_peer_schemas){
        if (c->peer_schemas[i].peer == peer)
            c->peer_schemas[i] = c->peer_schemas[--c->n_peer_schemas];   /* swap-remove */
        else i++;
    }
}

/* ---- why a schema gate refused (feeds DartEvent.schema_detail) ------------------------ */

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
            c->schema_whys[i] = c->schema_whys[--c->n_schema_whys];   /* swap-remove */
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
            if (s == i_DART_NODE_SCHEMA_OURS)   /* identical-schema entry: resolve live */
                return topic_index < c->n_topics ? c->chan_compiled[topic_index] : NULL;
            return s;
        }
    return NULL;
}

/* The topic's slot was REBOUND to a different binding (topic reuse with a retype): every
 * per-peer decode binding, rebased view, and refusal reason recorded for the old
 * occupant is stale. The verdicts were re-pended in the transport, so these re-derive
 * as details re-verify; here the caches just forget the old ones. */
void i_dart_node_core_topic_rebound(i_DartNodeCore *c, uint16_t topic_index){
    uint32_t i;
    if (!c) return;
    i = 0;
    while (i < c->n_peer_schemas){
        if (c->peer_schemas[i].topic == topic_index)
            c->peer_schemas[i] = c->peer_schemas[--c->n_peer_schemas];   /* swap-remove */
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

/* ---- the greedy detail cache (cfg.fetch_details) -------------------------------------- */

static i_DartNodeTopicDetail *i_dart_node_core_topic_find(i_DartNodeCore *c, uint32_t peer,
                                                          uint16_t index){
    uint32_t i;
    for (i = 0; i < c->n_topic_details; i++)
        if (c->topic_details[i].peer == peer && c->topic_details[i].index == index)
            return &c->topic_details[i];
    return NULL;
}

/* cache one fetched topic (idempotent; an OOM just leaves it for the retry sweep) */
static void i_dart_node_core_topic_set(i_DartNodeCore *c, uint32_t peer, const DartDetail *d){
    i_DartNodeTopicDetail *e; char *nm;
    if (!c->alloc || d->name.len == 0) return;
    if (i_dart_node_core_topic_find(c, peer, d->index)) return;
    nm = (char*)c->alloc(c->alloc_user, NULL, d->name.len);
    if (!nm) return;
    memcpy(nm, d->name.data, d->name.len);
    if (!i_dart_node_core_array_reserve(c, (void**)&c->topic_details, &c->cap_topic_details,
                                        c->n_topic_details + 1u, sizeof *c->topic_details)){
        c->alloc(c->alloc_user, nm, 0);
        return;
    }
    e = &c->topic_details[c->n_topic_details++];
    e->peer = peer; e->index = d->index;
    e->name = nm; e->name_len = (uint8_t)d->name.len;
    e->schema_hash = d->schema_hash;
    e->schema = d->schema_hash
              ? i_dart_node_core_intern(c, d->schema_hash, d->schema_wire) : NULL;
}

/* drop a peer's cached topics (GONE, or its id recycled onto a new peer) */
static void i_dart_node_core_topic_details_clear(i_DartNodeCore *c, uint32_t peer){
    uint32_t i = 0;
    while (i < c->n_topic_details){
        if (c->topic_details[i].peer == peer){
            if (c->alloc && c->topic_details[i].name)
                c->alloc(c->alloc_user, c->topic_details[i].name, 0);
            c->topic_details[i] = c->topic_details[--c->n_topic_details];   /* swap-remove */
        } else i++;
    }
}

int i_dart_node_core_topic_detail(i_DartNodeCore *c, uint32_t peer, uint16_t index,
                                  DartString *name, const DartSchema **schema,
                                  uint64_t *schema_hash){
    i_DartNodeTopicDetail *e = c ? i_dart_node_core_topic_find(c, peer, index) : NULL;
    if (name)        *name        = e ? dart_string(e->name, e->name_len) : dart_string(NULL, 0);
    if (schema)      *schema      = e ? e->schema : NULL;
    if (schema_hash) *schema_hash = e ? e->schema_hash : 0;
    return e != NULL;
}

/* Observer mode: append every advertised-but-uncached index to the want list (the
 * greedy superset of the transport's candidate wants). INACTIVE entries are skipped:
 * the responder would not answer them, which would re-request forever. Pass max_wants
 * n+1 with a scratch slot to just probe whether anything is missing. */
static uint16_t i_dart_node_core_greedy_extend(i_DartNodeCore *c, uint32_t peer,
                            DartBytes interest, DartDetailWant *wants, uint16_t n,
                            uint16_t max_wants){
    DartInterestIter it; DartTopicEntry e;
    uint16_t k;
    memset(&it, 0, sizeof it);
    while (n < max_wants && dart_interest_next(interest, &it, &e)){
        /* the iterator skips INACTIVE entries and hole runs; a PUBSUB double-yield
           dedupes against the want list below like any repeat */
        if (i_dart_node_core_topic_find(c, peer, e.index)) continue;
        for (k = 0; k < n; k++) if (wants[k].index == e.index) break;
        if (k < n) continue;
        wants[n].index = e.index;
        wants[n].schema_hash = 0;   /* force the wire inline: we may not hold that schema */
        n++;
    }
    return n;
}

/* why i_dart_node_core_intern returned NULL, as text (shared by both directions) */
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

/* the verdict itself; a refusal writes its reason into [p, end] (end = last writable
 * byte; pass p == end for no text). The wrapper below records/clears the reason. */
static int i_dart_node_core_schema_verdict(i_DartNodeCore *c, uint32_t peer, uint16_t topic_index,
                                           int peer_is_pub, uint64_t hash, DartBytes wire,
                                           char *p, char *end){
    const DartSchema *ours = (topic_index < c->n_topics) ? c->chan_compiled[topic_index] : NULL;
    int peer_has = hash != 0;   /* the peer's schema, from its detail response */
    if (peer_is_pub){                                   /* their publish side: we would read */
        if (!ours){                                     /* generic reader: accept, decode with theirs */
            i_dart_node_core_peer_schema_set(c, peer, topic_index,
                peer_has ? i_dart_node_core_intern(c, hash, wire) : NULL);
            return 1;
        }
        if (!peer_has){                                 /* typed reader refuses an untyped writer */
            p = i_dart_event_append_str(p, end, "their writer has no schema, our typed reader refuses");
            *p = '\0'; return 0;
        }
        if (hash == dart_schema_hash(ours)){            /* identical schema: our own view works */
            i_dart_node_core_peer_schema_set(c, peer, topic_index, i_DART_NODE_SCHEMA_OURS);
            return 1;
        }
        {   DartSchema *pub = i_dart_node_core_intern(c, hash, wire);   /* need the wire to verify */
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
        if (!ours){                                     /* typed reader refuses our raw topic */
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

/* (Re)build our discovery OVERLAY (frag size + OOB host + interest) from the core's
   current fields. The codec lives in the transport core; the OOB fields default to 0 in a
   non-SHM build. The node NAME is not here: the runtime hands it to discovery directly. */
uint16_t i_dart_node_core_build_meta(i_DartNodeCore *c){
    uint16_t need = dart_transport_meta_size(c->transport);
    /* the ANNOUNCE BUDGET: the whole announce datagram (discovery framing + its locator/
       name section + this overlay) must fit one un-fragmented datagram, so no receiver
       ever depends on IP reassembly (lwIP commonly cannot). Over budget -> the bootstrap
       form: INTEREST_EXTERNAL set, no interest inlined, peers pull the identical blob
       over the uDTL interest paging (i_dart_node_core_interest_respond). */
    int external = ((size_t)DART_DISCOVERY_META_OFF + DART_DISCOVERY_DISC_MAX + need
                    > (size_t)DART_DGRAM_MAX);
    if (external) need = dart_transport_meta_bootstrap_size();
    if (need > c->meta_cap){   /* (re)size the blob buffer to the exact content */
        uint8_t *nb = (uint8_t*)c->alloc(c->alloc_user, c->meta_buf, need);
        if (!nb) return c->meta_len;   /* OOM: keep the previous blob (stale but consistent) */
        c->meta_buf = nb; c->meta_cap = need;
    }
    c->meta_len = dart_transport_meta_build(c->transport, c->meta_buf, c->meta_cap,
                                  c->frag_size, c->oob_capable, c->oob_host, external);
    return c->meta_len;
}

DartBytes i_dart_node_core_meta(i_DartNodeCore *c){
    return dart_bytes(c->meta_buf, c->meta_len);
}

/* Answer a peer's DETAIL_REQ: validate kind + domain, then build the response (the codec
   is dart_transport_detail_* in the transport core) into the core's grown scratch buffer.
   Stateless: a pure read of topic + schema state, idempotent under duplicate requests.
   Returns the response bytes to send to the request's source, or {NULL,0} when not
   answerable (malformed, wrong domain, no alloc hook, or OOM: the requester just
   re-asks). An all-skipped response (header only) is still sent: it tells the requester
   those indices are not advertised at our current version. */
DartBytes i_dart_node_core_detail_respond(i_DartNodeCore *c, uint16_t domain, DartBytes req){
    size_t need, len;
    if (!c || !c->alloc || !c->discovery) return dart_bytes(NULL, 0);
    if (dart_detail_kind(req) != DART_DETAIL_REQ || dart_detail_domain(req) != domain)
        return dart_bytes(NULL, 0);
    /* resp_size already returns ONE page: dart_transport_detail_respond truncates the
       response at a single un-fragmented datagram (DART_DGRAM_MAX) and the requester pages
       the remainder, reply-clocked (a page's arrival re-arms the next request at once; the
       periodic rearm is only the lost-datagram fallback). This is why a big-topology peer
       must NOT be answered in one multi-KB blob: that blob IP-fragments, and a peer whose
       OS or RX buffer cannot reassemble it drops the WHOLE thing, so those entries could
       never resolve while single (one-at-a-time) requests still worked. A lone entry larger
       than a datagram still rides its own page, so a big schema can never wedge paging. */
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
 * many topics now flow each way, so an app/example can watch a connection form. Every
 * reflected-interest change funnels through here (inline apply, external assembly,
 * fresh detail verdicts re-applying), so this is also where the peer's interest EPOCH
 * bumps -- the observer cache key -- and it must bump even with no event handler. */
static void i_dart_node_core_fire_interest(i_DartNodeCore *c, uint32_t id){
    DartEvent ev; uint16_t publish_to = 0, receive_from = 0;
    {   i_DartNodePeerExtra *ex = c->discovery
            ? (i_DartNodePeerExtra*)dart_discovery_peer_user(c->discovery, id) : NULL;
        if (ex) ex->interest_epoch++;
    }
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

/* The peer's AUTHORITATIVE interest blob, wherever it lives: the announce's inline
 * section, or the assembled external fetch (only at the version the blob advertises),
 * or {NULL,0} while an external fetch is still in flight -- callers already treat a
 * missing blob as "interest unknown", which is exactly that state. The ONE read point,
 * so inline and external peers run identical apply/wants/unresolved logic. */
static DartBytes i_dart_node_core_interest_of(i_DartNodePeerExtra *ex, DartBytes meta,
                            uint32_t meta_version){
    if (!meta.data) return dart_bytes(NULL, 0);
    if (!dart_meta_interest_external(meta)) return dart_meta_interest(meta);
    if (ex && ex->interest_buf && ex->interest_version == meta_version)
        return dart_bytes(ex->interest_buf, ex->fetch_len);
    return dart_bytes(NULL, 0);
}

/* An external-interest peer whose blob we have not assembled at its current version:
 * queue an INTEREST_REQ (the runtime drains it with the detail requests). The
 * version dedup here is what makes a steady-state announce free: an already-assembled
 * version queues nothing. */
static void i_dart_node_core_interest_check(i_DartNodeCore *c, i_DartNodePeerExtra *ex,
                            DartBytes meta, uint32_t meta_version){
    if (!ex || !meta.data || !dart_meta_interest_external(meta)) return;
    if (ex->interest_version == meta_version) return;
    ex->interest_due = 1;
    c->detail_due_any = 1;
}

/* After an interest apply: if unverified candidates remain (hash overlap, no verdict
   yet), queue a DETAIL_REQ for the runtime to send (i_dart_node_core_detail_req_next).
   Runs on EVERY apply, so a lost request or response heals on the peer's next announce:
   the pending state itself is the retry state. */
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
    if (!ex) return;                                  /* discovery not bound / no scratch */
    dart_discovery_peer_meta(c->discovery, id, &meta_version);   /* version of the blob we hold */
    interest = i_dart_node_core_interest_of(ex, meta, meta_version);
    if (!ex->added){                                  /* brand-new peer: wire it into the transport */
        i_dart_node_core_peer_schema_clear(c, id);    /* its id may be recycled: no stale bindings */
        i_dart_node_core_topic_details_clear(c, id);
        i_dart_node_core_schema_why_clear(c, id);
        dart_transport_peer_add(c->transport, id, frag);          /* blob carries frag + pub/sub interest */
        ex->added = 1; ex->dormant = 0; ex->detail_due = 0;
        i_dart_node_core_set_peer_oob(c, id, meta);
        i_dart_node_core_fire(c, DART_PEER_UP, id, addr);
        if (interest.data){ dart_transport_apply_peer_interest(c->transport, id, interest);
                       i_dart_node_core_fire_interest(c, id);
                       i_dart_node_core_detail_check(c, ex, id, interest); }
    } else {                                          /* known peer: addr/interest update */
        dart_transport_peer_set_frag(c->transport, id, frag);
        i_dart_node_core_set_peer_oob(c, id, meta);
        if (interest.data){ dart_transport_apply_peer_interest(c->transport, id, interest);
                       i_dart_node_core_fire_interest(c, id);
                       i_dart_node_core_detail_check(c, ex, id, interest); }
        if (ex->dormant){    /* a DROPPED peer's same incarnation returned: resume */
            ex->dormant = 0;
            dart_transport_peer_resume(c->transport, id);   /* keeps reader position; writer fills any gap */
            i_dart_node_core_fire(c, DART_PEER_UP, id, addr);
        }
    }
    i_dart_node_core_interest_check(c, ex, meta, meta_version);   /* external + stale: pull it */
}

/* Ingest a peer's DETAIL_RESP (routed here by the runtime off the data socket): validate
   kind + domain, cache the verdicts, then re-apply the peer's CURRENT interest so the new
   verdicts form their matches exactly as a fresh announce would (proxies, catch-up
   replay, PEER_INTEREST event). A paged response leaves candidates pending: re-queue the
   request for the remainder. Idempotent under duplicate/crossing responses. */
void i_dart_node_core_apply_details(i_DartNodeCore *c, uint16_t domain, uint32_t peer,
                            DartBytes resp){
    i_DartNodePeerExtra *ex;
    DartBytes meta, interest;
    if (!c || dart_detail_kind(resp) != DART_DETAIL_RESP || dart_detail_domain(resp) != domain)
        return;
    ex = i_dart_node_core_peer_extra(c, peer);
    if (!ex || !ex->added) return;
    {   /* a response older than the blob we hold may describe a binding the peer has
           since REBOUND (topic reuse): refuse it rather than cache a stale verdict; the
           pending state re-asks and the fresh answer carries the current version.
           (Bindings were immutable per entry before rebinds existed, which is why this
           gate was never needed.) */
        uint32_t held = 0;
        dart_discovery_peer_meta(c->discovery, peer, &held);
        if (held && dart_detail_meta_version(resp) < held) return;
    }
    if (c->fetch_details){   /* observer mode: cache every fetched topic for the queries */
        DartDetailIter it; DartDetail dd;
        memset(&it, 0, sizeof it);
        while (dart_detail_next(resp, &it, &dd)) i_dart_node_core_topic_set(c, peer, &dd);
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

/* Queue a DETAIL_REQ for every ACTIVE peer (and an INTEREST_REQ for every external
   peer still unassembled): the runtime's periodic retry sweep, the lost-datagram
   backstop for both cycles. Peers with nothing pending cost one wants() walk and
   send nothing. */
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

/* A uDTL request from `peer` named OUR blob version `version`: proof it applied our
 * announce at that version. Feeds the transport's rebind hold; when the advance releases
 * a held writer lane, the interest event re-fires (match counts changed) and the caller
 * invalidates its match memos. Returns 1 exactly then. */
int i_dart_node_core_seen_version(i_DartNodeCore *c, uint32_t peer, uint32_t version){
    if (!c || !version) return 0;
    if (!dart_transport_peer_seen_version(c->transport, peer, version)) return 0;
    i_dart_node_core_fire_interest(c, peer);
    return 1;
}

/* Unresolved candidates for one topic across active peers (see core.h). A peer counts
 * once while its interest is UNKNOWN: its blob has not arrived, or its external
 * interest fetch is still assembling (both self-heal: the blob and every fetch page
 * are single sub-MTU datagrams). Once known, it counts by its unresolved entries for
 * this topic. Dropped peers are skipped: they are not expected to answer, and waiting
 * on one would only ever time out. */
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
        if (!v.meta.data){ cnt++; continue; }   /* announce heard, blob still being fetched */
        interest = i_dart_node_core_interest_of(ex, v.meta, v.meta_version);
        if (!interest.data){
            if (dart_meta_interest_external(v.meta)) cnt++;   /* fetch in flight: resolving */
            continue;                           /* else a blob with no interest: nothing to resolve */
        }
        cnt += dart_transport_topic_unresolved(c->transport, topic_index, v.id, interest);
    }
    return cnt;
}

/* Drain one queued uDTL request: INTEREST_REQs first (an unassembled interest gates
   candidate discovery, so it outranks details), then DETAIL_REQs. Returns bytes (loop
   until 0); a peer whose pending work emptied (or that dropped) just clears its flag.
   Detail requests are capped at 128 wants: a bigger pending set converges over
   successive request/response rounds (each response retires its indices from wants). */
size_t i_dart_node_core_detail_req_next(i_DartNodeCore *c, uint16_t domain,
                            void *out, size_t cap, i_DartNodeDest *to){
    DartDetailWant wants[128];
    uint16_t s, n;
    if (!c || !c->discovery || cap < 24u) return 0;
    n = dart_discovery_max_peers(c->discovery);
    for (s=0;s<n;s++){    /* interest pass: one INTEREST_REQ per due peer, cursor-driven */
        DartDiscoveryPeer v; i_DartNodePeerExtra *ex;
        uint32_t offset;
        if (!dart_discovery_peer_at(c->discovery, s, &v)) continue;
        ex = (i_DartNodePeerExtra*)v.user;
        if (!ex || !ex->interest_due) continue;
        ex->interest_due = 0;
        if (!ex->added || v.liveness != DART_PEER_ACTIVE) continue;
        if (!dart_meta_interest_external(v.meta)) continue;      /* flag cleared meanwhile */
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

/* Answer a peer's INTEREST_REQ: build our current interest blob into the scratch
   (after a page-header's worth of headroom) and return ONE page from the requested
   offset. Stateless and idempotent, like the detail responder: the header is written
   immediately before the chunk inside the scratch, so the page is returned zero-copy.
   {NULL,0} when not answerable (malformed, wrong domain, or OOM: the requester
   just re-asks). */
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
    if (built != total) return dart_bytes(NULL, 0);   /* size/build drifted: never serve garbage */
    if (offset > total) offset = total;               /* clamp: a header-only page reports total */
    chunk = total - offset;
    if (chunk > (size_t)DART_DGRAM_MAX - DART_INTEREST_RESP_HEAD)
        chunk = (size_t)DART_DGRAM_MAX - DART_INTEREST_RESP_HEAD;   /* one sub-datagram page */
    head = blob + offset - DART_INTEREST_RESP_HEAD;   /* header right before the chunk (clobbers
                                                         already-shipped bytes; rebuilt per request) */
    dart_interest_resp_head(domain, dart_discovery_meta_version(c->discovery), total,
                            offset, (uint16_t)chunk, head, DART_INTEREST_RESP_HEAD);
    return dart_bytes(head, DART_INTEREST_RESP_HEAD + chunk);
}

/* Ingest one INTEREST_RESP page (routed here by the runtime off the data socket):
   append it at the cursor, re-ask while incomplete (reply-clocked, the periodic sweep
   is only the lost-datagram fallback), and on completion apply the assembled blob
   exactly as an inline announce would (matches, PEER_INTEREST, detail cycle). The
   version stamp drives everything: a duplicate of an assembled version is a no-op, a
   stamp differing from the fetch in progress restarts the cursor (the peer's interest
   changed mid-fetch). */
void i_dart_node_core_apply_interest_page(i_DartNodeCore *c, uint16_t domain, uint32_t peer,
                            DartBytes resp){
    i_DartNodePeerExtra *ex;
    uint32_t total, offset, resp_version; DartBytes chunk;
    if (!c || dart_detail_kind(resp) != DART_INTEREST_RESP || dart_detail_domain(resp) != domain)
        return;
    ex = i_dart_node_core_peer_extra(c, peer);
    if (!ex || !ex->added) return;
    if (!dart_interest_resp_parse(resp, &total, &offset, &chunk)) return;
    if (total > dart_interest_max(0xFFFFu)) return;   /* absurd total: never a real blob */
    resp_version = dart_detail_meta_version(resp);
    if (ex->interest_buf && ex->interest_version == resp_version) return;   /* assembled: dup page */
    if (ex->fetch_version != resp_version){           /* first page, or the version moved: restart */
        ex->fetch_version = resp_version;
        ex->fetch_cursor = 0; ex->fetch_len = total;
    }
    if (total != ex->fetch_len){ ex->fetch_cursor = 0; ex->fetch_len = total; }  /* drifted: restart */
    if (offset != ex->fetch_cursor){                  /* out-of-order/stale page: re-ask from cursor */
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
        return;                                       /* an empty short page: the sweep re-asks */
    }
    ex->interest_version = resp_version;              /* assembled: the dedup that ends the cycle */
    ex->fetch_version = 0;
    /* apply exactly as an inline announce would (idempotent; len is ex->fetch_len) */
    dart_transport_apply_peer_interest(c->transport, peer, dart_bytes(ex->interest_buf, total));
    i_dart_node_core_fire_interest(c, peer);
    i_dart_node_core_detail_check(c, ex, peer, dart_bytes(ex->interest_buf, total));
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
        i_dart_node_core_topic_details_clear(c, id);
        i_dart_node_core_schema_why_clear(c, id);
        if (ex && ex->interest_buf){                    /* retained external interest: hook memory */
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

uint32_t dart_node_peer_interest_epoch(const DartDiscoveryPeer *peer){
    const i_DartNodePeerExtra *ex = peer ? (const i_DartNodePeerExtra*)peer->user : NULL;
    return ex ? ex->interest_epoch : 0;
}

int dart_node_peer_interest_next(const DartDiscoveryPeer *peer,
                                 DartInterestIter *it, DartTopicEntry *out){
    if (!peer || !peer->meta.data) return 0;
    /* one read point for BOTH interest homes: the announce's inline section, or the
       assembled external fetch (nothing while that fetch is still in flight, exactly
       like a blob that has not arrived). Reflection (the entity fold), the patterns
       authority check, and any app walk all resolve external peers through here. */
    return dart_interest_next(
        i_dart_node_core_interest_of((i_DartNodePeerExtra*)peer->user, peer->meta,
                                     peer->meta_version),
        it, out);
}
