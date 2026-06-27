/* sans-IO NODE core: peer table + the discovery->transport lifecycle. No platform
 * access; a runtime drives it (see node/runtime.c) and does the IO. See node/core.h. */

#include "core.h"
#include "../common/arena.h"
#include <string.h>

/* dart_event_str + bounded appenders: format the node's app DartEvent as one line.
   No stdio, so it stays in the sans-IO core. (It covers the union event, including the
   peer/interest/mcast kinds the node adds; the transport keeps no formatter of its own.) */
static char *i_ev_str(char *p, char *end, const char *s){
    if (!s) return p;
    while (*s && p < end) *p++ = *s++;
    return p;
}
static char *i_ev_u64(char *p, char *end, uint64_t v){
    char tmp[20]; int n = 0;
    do { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v);
    while (n && p < end) *p++ = tmp[--n];
    return p;
}
static char *i_ev_hex(char *p, char *end, uint64_t v){
    char tmp[16]; int n = 0;
    do { int d = (int)(v & 0xF); tmp[n++] = (char)(d < 10 ? '0'+d : 'a'+d-10); v >>= 4; } while (v);
    while (n && p < end) *p++ = tmp[--n];
    return p;
}
static char *i_ev_addr(char *p, char *end, const DartEvent *ev){   /* dotted quad + :port (IPv4 only) */
    int i;
    for (i = 0; i < 4; i++){ if (i) p = i_ev_str(p,end,"."); p = i_ev_u64(p,end,ev->ip[i]); }
    p = i_ev_str(p,end,":"); return i_ev_u64(p,end,ev->port);
}

const char *dart_event_str(const DartEvent *ev, char *buf, size_t cap){
    char *p, *end;
    if (!buf || !cap) return buf;
    p = buf; end = buf + cap - 1;                  /* reserve one byte for the NUL */
    switch (ev->kind){
    case DART_PEER_UP:
        p = i_ev_str(p,end,"peer-up id="); p = i_ev_u64(p,end,ev->peer);
        if (ev->ip_len == 4){ p = i_ev_str(p,end," at "); p = i_ev_addr(p,end,ev); }
        p = i_ev_str(p,end," ("); p = i_ev_str(p,end,ev->detail); p = i_ev_str(p,end,")");
        break;
    case DART_PEER_DOWN:
        p = i_ev_str(p,end,"peer-down id="); p = i_ev_u64(p,end,ev->peer);
        p = i_ev_str(p,end," ("); p = i_ev_str(p,end,ev->detail); p = i_ev_str(p,end,")");
        break;
    case DART_PEER_INTEREST:
        p = i_ev_str(p,end,"interest id="); p = i_ev_u64(p,end,ev->peer);
        p = i_ev_str(p,end," publish-to="); p = i_ev_u64(p,end,ev->publish_topics);
        p = i_ev_str(p,end," topics, receive-from="); p = i_ev_u64(p,end,ev->receive_topics);
        p = i_ev_str(p,end," topics");
        break;
    case DART_PEER_REFUSED:
        p = i_ev_str(p,end,"peer-refused at "); p = i_ev_addr(p,end,ev);
        p = i_ev_str(p,end," (table full of active peers)");
        break;
    case DART_NAME_COLLISION:
        p = i_ev_str(p,end,"name-collision ch="); p = i_ev_u64(p,end,ev->channel);
        p = i_ev_str(p,end," id=0x"); p = i_ev_hex(p,end,ev->identity);
        p = i_ev_str(p,end," ("); p = i_ev_str(p,end,ev->detail);
        p = i_ev_str(p,end,"): match refused");
        break;
    case DART_QOS_INCOMPATIBLE:
        p = i_ev_str(p,end,"qos-incompatible ch="); p = i_ev_u64(p,end,ev->channel);
        p = i_ev_str(p,end," from id="); p = i_ev_u64(p,end,ev->peer);
        p = i_ev_str(p,end," ("); p = i_ev_str(p,end,ev->detail);
        p = i_ev_str(p,end,"): reliable subscriber refused best-effort publisher");
        break;
    case DART_MSG_LOST:
        p = i_ev_str(p,end,"msg-lost ch="); p = i_ev_u64(p,end,ev->channel);
        p = i_ev_str(p,end," from id="); p = i_ev_u64(p,end,ev->peer);
        p = i_ev_str(p,end," seqno "); p = i_ev_u64(p,end,ev->lost_first);
        p = i_ev_str(p,end,".."); p = i_ev_u64(p,end,ev->lost_first + ev->lost_count - 1);
        break;
    case DART_MSG_TOO_BIG:
        p = i_ev_str(p,end,"msg-too-big ch="); p = i_ev_u64(p,end,ev->channel);
        p = i_ev_str(p,end," from id="); p = i_ev_u64(p,end,ev->peer);
        p = i_ev_str(p,end," ("); p = i_ev_u64(p,end,ev->too_big_bytes);
        p = i_ev_str(p,end," bytes), skipped");
        break;
    case DART_MCAST_JOIN_FAILED:
        p = i_ev_str(p,end,"mcast-join-failed ch="); p = i_ev_u64(p,end,ev->channel);
        p = i_ev_str(p,end," ("); p = i_ev_str(p,end,ev->detail); p = i_ev_str(p,end,")");
        break;
    }
    *p = '\0';                                     /* p <= end = buf+cap-1, in range */
    return buf;
}

/* the node core's per-peer transport-lifecycle state, kept in the discovery peer's user
   scratch (so the node holds NO peer table of its own). added = wired into the transport
   yet (dart_peer_add called); dormant = discovery DROPPED it, kept for a same-incarnation
   resume. Discovery zeroes this when a new UUID takes the slot, preserves it on resume. */
typedef struct { uint8_t added; uint8_t dormant; } i_DartNodePeerExtra;

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
};

uint16_t dart_node_core_peer_user_bytes(void){ return (uint16_t)sizeof(i_DartNodePeerExtra); }
void dart_node_core_bind_discovery(i_DartNodeCore *c, DartDiscoveryState *discovery){ c->discovery = discovery; }

/* arena layout: the core struct, then the announce-blob buffer (no peer table -- that
   lives in the discovery core). One sequence so measure and build agree. */
static void dart__core_layout(i_DartBump *b, uint16_t n_channels,
                              i_DartNodeCore **out_c, uint8_t **out_meta){
    i_DartNodeCore *c = (i_DartNodeCore*)dart_take(b, sizeof(struct i_DartNodeCore), 16);
    uint8_t *meta     = (uint8_t*)       dart_take(b, dart_meta_capacity(n_channels), 16);
    if (out_c)    *out_c    = c;
    if (out_meta) *out_meta = meta;
}

size_t dart_node_core_required_memory(uint16_t n_channels){
    i_DartBump b; memset(&b, 0, sizeof b);
    dart__core_layout(&b, n_channels, NULL, NULL);
    return b.offset + 16u;   /* slack to align the caller's mem up to base */
}

i_DartNodeCore *dart_node_core_init(void *mem, size_t cap, const i_DartNodeCoreConfig *cfg){
    i_DartBump b; i_DartNodeCore *c; uint8_t *base, *meta;
    if (!mem || !cfg || !cfg->transport) return NULL;
    if (cap < dart_node_core_required_memory(cfg->n_channels)) return NULL;
    base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    memset(&b, 0, sizeof b); b.base = base; b.cap = cap - (size_t)(base - (uint8_t*)mem);
    dart__core_layout(&b, cfg->n_channels, &c, &meta);

    memset(c, 0, sizeof *c);
    c->transport     = cfg->transport;
    c->discovery     = cfg->discovery;     /* may be NULL now, bound via bind_discovery later */
    c->on_event      = cfg->on_event;      c->user = cfg->user;
    c->oob_capable   = cfg->oob_capable;
    memcpy(c->oob_host, cfg->oob_host, 16);
    c->meta_buf      = meta;
    c->meta_cap      = dart_meta_capacity(cfg->n_channels);
    c->frag_size     = cfg->frag_size;
    return c;
}

/* Relocate the sans-IO node core into a bigger block at grown counts. No peer table (it
 * lives in discovery); a struct copy carries the scalars. The transport, discovery, and
 * announce-blob pointers are re-pointed by the caller after those move. */
i_DartNodeCore *dart_node_core_migrate(i_DartNodeCore *old, void *new_mem, size_t new_cap,
                                       uint16_t new_n_channels){
    i_DartBump b; i_DartNodeCore *c; uint8_t *base, *meta;
    if (!old) return NULL;
    if (new_cap < dart_node_core_required_memory(new_n_channels)) return NULL;
    base = (uint8_t*)(((uintptr_t)new_mem + 15u) & ~(uintptr_t)15u);
    memset(&b, 0, sizeof b); b.base = base; b.cap = new_cap - (size_t)(base - (uint8_t*)new_mem);
    dart__core_layout(&b, new_n_channels, &c, &meta);
    *c = *old;                          /* scalars + transport/discovery ptrs (caller re-points) */
    c->meta_buf  = meta;                /* caller rebuilds the blob into it */
    c->meta_cap  = dart_meta_capacity(new_n_channels);
    return c;
}

/* (Re)build our discovery OVERLAY (frag size + OOB host + interest) from the core's
   current fields. The codec lives in the transport core; the OOB fields default to 0 in a
   non-SHM build. The node NAME is not here: the runtime hands it to discovery directly. */
uint16_t dart_node_core_build_meta(i_DartNodeCore *c){
    c->meta_len = dart_meta_build(c->transport, c->meta_buf, c->meta_cap,
                                  c->frag_size, c->oob_capable, c->oob_host);
    return c->meta_len;
}

const uint8_t *dart_node_core_meta(i_DartNodeCore *c, uint16_t *len){
    if (len) *len = c->meta_len;
    return c->meta_buf;
}

#ifdef DART_SHM
/* a peer can receive our out-of-band (SHM) payload iff we are OOB-capable, it
 * advertised an OOB host in its meta blob, and that host equals ours (same kernel).
 * Set the transport's per-peer flag. The core knows nothing of SHM beyond this. */
static void dart__core_set_peer_oob(i_DartNodeCore *c, uint32_t id, const uint8_t *meta, uint16_t meta_len){
    uint8_t host[16];
    int oob = c->oob_capable && dart_meta_shm(meta, meta_len, host) &&
              memcmp(host, c->oob_host, 16) == 0;
    dart_peer_set_shm(c->transport, id, oob);
}
#else
#define dart__core_set_peer_oob(c, id, meta, meta_len) ((void)0)
#endif

static void dart__core_fire(i_DartNodeCore *c, DartEventKind kind, uint32_t id,
                            const DartDiscoveryAddr *addr, const char *detail){
    DartEvent ev;
    if (!c->on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind; ev.peer = id; ev.detail = detail; ev.user = c->user;
    if (addr){ memcpy(ev.ip, addr->ip, 16); ev.ip_len = addr->ip_len; ev.port = addr->port; }
    c->on_event(&ev);
}

/* fired whenever a peer's interest list is (re)applied to the transport: reports how
 * many topics now flow each way, so an app/example can watch a connection form. */
static void dart__core_fire_interest(i_DartNodeCore *c, uint32_t id){
    DartEvent ev; uint16_t publish_to = 0, receive_from = 0;
    if (!c->on_event) return;
    dart_peer_match_counts(c->transport, id, &publish_to, &receive_from);
    memset(&ev, 0, sizeof ev);
    ev.kind = DART_PEER_INTEREST; ev.peer = id;
    ev.publish_topics = publish_to; ev.receive_topics = receive_from;
    ev.detail = "interest applied"; ev.user = c->user;
    c->on_event(&ev);
}

/* the peer's transport-lifecycle state lives in the discovery peer's user scratch; the
 * node core keeps no table of its own. NULL only before discovery is bound (no events yet). */
static i_DartNodePeerExtra *dart__core_extra(i_DartNodeCore *c, uint32_t id){
    return (i_DartNodePeerExtra*)dart_discovery_peer_user(c->discovery, id);
}

static void dart__core_peer_up(i_DartNodeCore *c, uint32_t id, const DartDiscoveryAddr *addr,
                            const uint8_t *meta, uint16_t meta_len){
    i_DartNodePeerExtra *ex = dart__core_extra(c, id);
    uint16_t frag = dart_meta_frag(meta, meta_len);
    size_t interest_len = 0; const uint8_t *interest = dart_meta_interest(meta, meta_len, &interest_len);
    if (!ex) return;                                  /* discovery not bound / no scratch */
    if (!ex->added){                                  /* brand-new peer: wire it into the transport */
        dart_peer_add(c->transport, id, frag);          /* blob carries frag + pub/sub interest */
        ex->added = 1; ex->dormant = 0;
        dart__core_set_peer_oob(c, id, meta, meta_len);
        dart__core_fire(c, DART_PEER_UP, id, addr, "peer discovered");
        if (interest){ dart_apply_peer_interest(c->transport, id, interest, interest_len);
                       dart__core_fire_interest(c, id); }
    } else {                                          /* known peer: addr/interest update */
        dart_peer_set_frag(c->transport, id, frag);
        dart__core_set_peer_oob(c, id, meta, meta_len);
        if (interest){ dart_apply_peer_interest(c->transport, id, interest, interest_len);
                       dart__core_fire_interest(c, id); }
        if (ex->dormant){    /* a DROPPED peer's same incarnation returned: resume */
            ex->dormant = 0;
            dart_peer_resume(c->transport, id);   /* keeps reader position; writer fills any gap */
            dart__core_fire(c, DART_PEER_UP, id, addr, "peer resumed");
        }
    }
}

static void dart__core_peer_down(i_DartNodeCore *c, uint32_t id, DartDiscoveryDownReason reason){
    i_DartNodePeerExtra *ex = dart__core_extra(c, id);   /* discovery frees the slot AFTER this event */
    if (reason == DART_DISCOVERY_DROP){
        /* fell silent: keep transport state so a same-incarnation return resumes
           losslessly; just stop flow-controlling it and tell the app once */
        if (ex && ex->added && !ex->dormant){
            ex->dormant = 1;
            dart_peer_dormant(c->transport, id);
            dart__core_fire(c, DART_PEER_DOWN, id, NULL, "peer dropped");
        }
    } else {   /* GONE: said BYE or its slot was reclaimed; free the transport state */
        int notify = (ex && ex->added && !ex->dormant);   /* active->gone: app not yet told */
        dart_peer_remove(c->transport, id);   /* discovery zeroes the scratch on slot reuse */
        if (notify) dart__core_fire(c, DART_PEER_DOWN, id, NULL, "peer lost");
    }
}

static void dart__core_peer_refused(i_DartNodeCore *c, const DartDiscoveryAddr *addr){
    dart__core_fire(c, DART_PEER_REFUSED, 0, addr, "peer table full (all active)");
}

/* The discovery core's on_event sink (cfg.user = this core): demux the generic
 * DartDiscoveryEvent into the lifecycle handlers above, which fire the app DartEvents. */
void dart_node_core_on_disc_event(const DartDiscoveryEvent *ev){
    i_DartNodeCore *c = (i_DartNodeCore*)ev->user;
    switch (ev->kind){
        case DART_DISCOVERY_PEER_UP:
            dart__core_peer_up(c, ev->peer, &ev->addr, ev->meta, ev->meta_len);  /* name lives in discovery */
            break;
        case DART_DISCOVERY_PEER_DOWN:
            dart__core_peer_down(c, ev->peer, ev->reason);
            break;
        case DART_DISCOVERY_PEER_REFUSED:
            dart__core_peer_refused(c, &ev->addr);
            break;
        default: break;
    }
}

int dart_node_core_resolve(i_DartNodeCore *c, uint32_t to, i_DartNodeDest *out){
    DartDiscoveryAddr a;
    memset(out, 0, sizeof *out);
    if (DART_DEST_IS_GROUP(to)){            /* the transport's group encoding stays inside the core */
        out->is_group = 1;
        out->group_sel = DART_DEST_GROUP_CHAN(to);
        return 1;
    }
    if (!dart_discovery_addr_of_id(c->discovery, to, &a)) return 0;   /* peer vanished */
    memcpy(out->ip, a.ip, 16);
    out->ip_len = a.ip_len;
    out->port   = a.port;
    return 1;
}

int dart_node_core_id_for_addr(i_DartNodeCore *c, const uint8_t ip[4], uint16_t port, uint32_t *id){
    return dart_discovery_id_for_addr(c->discovery, ip, 4, port, id);
}

const char *dart_node_core_peer_name(i_DartNodeCore *c, uint32_t id, uint8_t *out_len){
    uint8_t nl = 0;
    const char *name = dart_discovery_peer_name(c->discovery, id, &nl);
    if (!name){ if (out_len) *out_len = 0; return NULL; }   /* not a known peer */
    if (nl == 0){ name = "unknown-peer"; nl = 12; }         /* never empty for a known peer */
    if (out_len) *out_len = nl;
    return name;
}

uint16_t dart_node_core_max_peers(i_DartNodeCore *c){ return dart_discovery_max_peers(c->discovery); }

int dart_node_core_peer_at(i_DartNodeCore *c, uint16_t slot, uint32_t *id,
                           uint8_t ip[16], uint8_t *ip_len, uint16_t *port){
    DartDiscoveryPeer v;
    if (!dart_discovery_peer_at(c->discovery, slot, &v)) return 0;
    if (id)     *id = v.id;
    if (ip)     memcpy(ip, v.addr.ip, 16);
    if (ip_len) *ip_len = v.addr.ip_len;
    if (port)   *port = v.addr.port;
    return 1;
}
