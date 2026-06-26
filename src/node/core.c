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

typedef struct {
    uint8_t  used;
    uint8_t  dormant;  /* discovery DROPPED it: kept for a same-incarnation resume */
    uint32_t id;
    uint8_t  ip[16];   /* peer's physical address (IPv4 today; opaque to the core) */
    uint8_t  ip_len;
    uint16_t port;     /* peer's advertised data port */
    char     name[DART_NODE_NAME_MAX + 1];  /* from its announce blob (NUL-terminated, debug) */
    uint8_t  name_len;
} i_DartNodePeer;

struct i_DartNodeCore {
    DartState           *transport;
    i_DartNodePeer       *peers;
    uint16_t              max_peers;
    DartEventFn         on_event;
    void                 *user;
    i_DartNodeIsLocalFn is_local;
    void                 *is_local_user;
    int                   oob_capable;
    uint8_t               oob_host[16];
    uint8_t              *meta_buf;    /* our outgoing discovery announce blob */
    uint16_t              meta_cap;
    uint16_t              meta_len;
    uint16_t              frag_size;   /* baked into the blob */
    char                  name[DART_NODE_NAME_MAX + 1];  /* our node name, baked into the blob */
    uint8_t               name_len;
};

/* arena layout: the core struct, the peer table, then the announce-blob buffer. One
   sequence so measure and build agree (dart_take with a NULL base just advances). */
static void dart__core_layout(i_DartBump *b, uint16_t max_peers, uint16_t n_channels,
                              i_DartNodeCore **out_c, uint8_t **out_peers, uint8_t **out_meta){
    i_DartNodeCore *c = (i_DartNodeCore*)dart_take(b, sizeof(struct i_DartNodeCore), 16);
    uint8_t *peers    = (uint8_t*)       dart_take(b, (size_t)max_peers * sizeof(i_DartNodePeer), 16);
    uint8_t *meta     = (uint8_t*)       dart_take(b, dart_meta_capacity(n_channels), 16);
    if (out_c)     *out_c     = c;
    if (out_peers) *out_peers = peers;
    if (out_meta)  *out_meta  = meta;
}

size_t dart_node_core_required_memory(uint16_t max_peers, uint16_t n_channels){
    i_DartBump b; memset(&b, 0, sizeof b);
    dart__core_layout(&b, max_peers, n_channels, NULL, NULL, NULL);
    return b.offset + 16u;   /* slack to align the caller's mem up to base */
}

i_DartNodeCore *dart_node_core_init(void *mem, size_t cap, const i_DartNodeCoreConfig *cfg){
    i_DartBump b; i_DartNodeCore *c; uint8_t *base, *peers, *meta;
    if (!mem || !cfg || !cfg->transport || cfg->max_peers == 0) return NULL;
    if (cap < dart_node_core_required_memory(cfg->max_peers, cfg->n_channels)) return NULL;
    base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    memset(&b, 0, sizeof b); b.base = base; b.cap = cap - (size_t)(base - (uint8_t*)mem);
    dart__core_layout(&b, cfg->max_peers, cfg->n_channels, &c, &peers, &meta);

    memset(c, 0, sizeof *c);
    c->transport     = cfg->transport;
    c->peers         = (i_DartNodePeer*)peers;
    c->max_peers     = cfg->max_peers;
    c->on_event      = cfg->on_event;      c->user = cfg->user;
    c->is_local      = cfg->is_local;      c->is_local_user = cfg->is_local_user;
    c->oob_capable   = cfg->oob_capable;
    memcpy(c->oob_host, cfg->oob_host, 16);
    c->meta_buf      = meta;
    c->meta_cap      = dart_meta_capacity(cfg->n_channels);
    c->frag_size     = cfg->frag_size;
    {   uint8_t nl = cfg->name_len;
        if (nl > DART_NODE_NAME_MAX) nl = DART_NODE_NAME_MAX;
        if (cfg->name && nl) memcpy(c->name, cfg->name, nl);
        c->name[nl] = '\0'; c->name_len = nl; }
    memset(peers, 0, (size_t)cfg->max_peers * sizeof(i_DartNodePeer));
    return c;
}

/* Relocate the sans-IO node core into a bigger block at grown counts. Peers are inline
 * (no internal pointers) so a struct copy carries them; the transport pointer and the
 * announce-blob pointer are re-pointed by the caller after the transport/blob move. */
i_DartNodeCore *dart_node_core_migrate(i_DartNodeCore *old, void *new_mem, size_t new_cap,
                                       uint16_t new_max_peers, uint16_t new_n_channels){
    i_DartBump b; i_DartNodeCore *c; uint8_t *base, *peers, *meta;
    if (!old) return NULL;
    if (new_cap < dart_node_core_required_memory(new_max_peers, new_n_channels)) return NULL;
    base = (uint8_t*)(((uintptr_t)new_mem + 15u) & ~(uintptr_t)15u);
    memset(&b, 0, sizeof b); b.base = base; b.cap = new_cap - (size_t)(base - (uint8_t*)new_mem);
    dart__core_layout(&b, new_max_peers, new_n_channels, &c, &peers, &meta);
    *c = *old;                          /* scalars, name, transport ptr (caller re-points) */
    c->peers     = (i_DartNodePeer*)peers;
    c->max_peers = new_max_peers;
    c->meta_buf  = meta;                /* caller rebuilds the blob into it */
    c->meta_cap  = dart_meta_capacity(new_n_channels);
    memset(peers, 0, (size_t)new_max_peers * sizeof(i_DartNodePeer));
    memcpy(peers, old->peers, (size_t)old->max_peers * sizeof(i_DartNodePeer));
    return c;
}

/* (Re)build our announce blob from the core's current fields. The codec lives in the
   transport core; the OOB fields default to 0/zero in a non-SHM build, so the codec
   just writes the v2 (non-SHM) prefix. */
uint16_t dart_node_core_build_meta(i_DartNodeCore *c){
    c->meta_len = dart_meta_build(c->transport, c->meta_buf, c->meta_cap,
                                  c->frag_size, c->oob_capable, c->oob_host,
                                  c->name, c->name_len);
    return c->meta_len;
}

const uint8_t *dart_node_core_meta(i_DartNodeCore *c, uint16_t *len){
    if (len) *len = c->meta_len;
    return c->meta_buf;
}

static int dart__core_find_id(i_DartNodeCore *c, uint32_t id){
    uint16_t i;
    for (i=0;i<c->max_peers;i++) if (c->peers[i].used && c->peers[i].id==id) return (int)i;
    return -1;
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

/* copy the peer's advertised name out of its (call-lifetime) announce blob into the
 * peer slot, so DartMsg.sender_name can point at stable storage. A peer that somehow
 * advertised no name gets "unknown-peer", so a slot is never empty-named. */
static void dart__core_set_peer_name(i_DartNodePeer *p, const uint8_t *meta, uint16_t meta_len){
    uint8_t nl = 0;
    const char *nm = dart_meta_name(meta, meta_len, &nl);
    if (!nm || nl == 0){ nm = "unknown-peer"; nl = 12; }
    if (nl > DART_NODE_NAME_MAX) nl = DART_NODE_NAME_MAX;
    memcpy(p->name, nm, nl);
    p->name[nl] = '\0'; p->name_len = nl;
}

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

static void dart__core_peer_up(i_DartNodeCore *c, uint32_t id, const DartDiscoveryAddr *addr,
                            const uint8_t *meta, uint16_t meta_len){
    uint16_t i; int slot = -1;
    uint16_t frag = dart_meta_frag(meta, meta_len);
    size_t interest_len = 0; const uint8_t *interest = dart_meta_interest(meta, meta_len, &interest_len);
    for (i=0;i<c->max_peers;i++){
        if (c->peers[i].used && c->peers[i].id==id){      /* known peer: addr/interest update */
            memcpy(c->peers[i].ip, addr->ip, 16);
            c->peers[i].ip_len = addr->ip_len; c->peers[i].port = addr->port;
            dart__core_set_peer_name(&c->peers[i], meta, meta_len);
            dart_peer_set_frag(c->transport, id, frag);
            dart__core_set_peer_oob(c, id, meta, meta_len);
            if (interest){ dart_apply_peer_interest(c->transport, id, interest, interest_len);
                           dart__core_fire_interest(c, id); }
            if (c->peers[i].dormant){    /* a DROPPED peer's same incarnation returned: resume */
                c->peers[i].dormant = 0;
                dart_peer_resume(c->transport, id);   /* keeps reader position; writer fills any gap */
                dart__core_fire(c, DART_PEER_UP, id, addr, "peer resumed");
            }
            return;
        }
        if (!c->peers[i].used && slot<0) slot=(int)i;
    }
    if (slot<0) return;
    c->peers[slot].used=1; c->peers[slot].id=id;
    memcpy(c->peers[slot].ip, addr->ip, 16);
    c->peers[slot].ip_len=addr->ip_len; c->peers[slot].port=addr->port;
    dart__core_set_peer_name(&c->peers[slot], meta, meta_len);
    /* the announce blob carries the peer's frag size + pub/sub interest list */
    {   int local = (addr->ip_len==4) && c->is_local && c->is_local(c->is_local_user, addr->ip, addr->ip_len);
        dart_peer_add(c->transport, id, local, frag); }
    dart__core_set_peer_oob(c, id, meta, meta_len);
    dart__core_fire(c, DART_PEER_UP, id, addr, "peer discovered");
    if (interest){ dart_apply_peer_interest(c->transport, id, interest, interest_len);
                   dart__core_fire_interest(c, id); }
}

static void dart__core_peer_down(i_DartNodeCore *c, uint32_t id, DartDiscoveryDownReason reason){
    int i = dart__core_find_id(c, id);
    if (reason == DART_DISCOVERY_DROP){
        /* fell silent: keep transport state so a same-incarnation return resumes
           losslessly; just stop flow-controlling it and tell the app once */
        if (i>=0 && !c->peers[i].dormant){
            c->peers[i].dormant = 1;
            dart_peer_dormant(c->transport, id);
            dart__core_fire(c, DART_PEER_DOWN, id, NULL, "peer dropped");
        }
    } else {   /* GONE: said BYE or its slot was reclaimed; free the transport state */
        int notify = (i>=0 && !c->peers[i].dormant);   /* active->gone: app not yet told */
        if (i>=0) c->peers[i].used=0;
        dart_peer_remove(c->transport, id);
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
            dart__core_peer_up(c, ev->peer, &ev->addr, ev->meta, ev->meta_len);
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
    int i;
    memset(out, 0, sizeof *out);
    if (DART_DEST_IS_GROUP(to)){            /* the transport's group encoding stays inside the core */
        out->is_group = 1;
        out->group_sel = DART_DEST_GROUP_CHAN(to);
        return 1;
    }
    i = dart__core_find_id(c, to);
    if (i < 0) return 0;                    /* peer vanished */
    memcpy(out->ip, c->peers[i].ip, 16);
    out->ip_len = c->peers[i].ip_len;
    out->port   = c->peers[i].port;
    return 1;
}

int dart_node_core_id_for_addr(i_DartNodeCore *c, const uint8_t ip[4], uint16_t port, uint32_t *id){
    uint16_t i;
    for (i=0;i<c->max_peers;i++)
        if (c->peers[i].used && c->peers[i].ip_len>=4 && c->peers[i].port==port
            && memcmp(c->peers[i].ip, ip, 4)==0){ if (id) *id = c->peers[i].id; return 1; }
    return 0;
}

const char *dart_node_core_peer_name(i_DartNodeCore *c, uint32_t id, uint8_t *out_len){
    int i = dart__core_find_id(c, id);
    if (i < 0){ if (out_len) *out_len = 0; return NULL; }   /* not a known peer */
    if (out_len) *out_len = c->peers[i].name_len;           /* always non-empty (see set_peer_name) */
    return c->peers[i].name;
}

uint16_t dart_node_core_max_peers(i_DartNodeCore *c){ return c->max_peers; }

int dart_node_core_peer_at(i_DartNodeCore *c, uint16_t slot, uint32_t *id,
                           uint8_t ip[16], uint8_t *ip_len, uint16_t *port){
    i_DartNodePeer *p;
    if (slot >= c->max_peers) return 0;
    p = &c->peers[slot];
    if (!p->used) return 0;
    if (id)     *id = p->id;
    if (ip)     memcpy(ip, p->ip, 16);
    if (ip_len) *ip_len = p->ip_len;
    if (port)   *port = p->port;
    return 1;
}
