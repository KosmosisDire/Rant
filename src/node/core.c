/* sans-IO NODE core: peer table + the discovery->transport lifecycle. No platform
 * access; a runtime drives it (see node/runtime.c) and does the IO. See node/core.h. */

#include "core.h"
#include "../common/arena.h"
#include <string.h>

typedef struct {
    uint8_t  used;
    uint8_t  dormant;  /* discovery DROPPED it: kept for a same-incarnation resume */
    uint32_t id;
    uint8_t  ip[16];   /* peer's physical address (IPv4 today; opaque to the core) */
    uint8_t  ip_len;
    uint16_t port;     /* peer's advertised data port */
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
    memset(peers, 0, (size_t)cfg->max_peers * sizeof(i_DartNodePeer));
    return c;
}

/* (Re)build our announce blob from the core's current fields. The codec lives in the
   transport core; the OOB fields default to 0/zero in a non-SHM build, so the codec
   just writes the v2 (non-SHM) prefix. */
uint16_t dart_node_core_build_meta(i_DartNodeCore *c){
    c->meta_len = dart_meta_build(c->transport, c->meta_buf, c->meta_cap,
                                  c->frag_size, c->oob_capable, c->oob_host);
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

static void dart__core_fire(i_DartNodeCore *c, DartEventKind kind, uint32_t id,
                            const DartDiscoveryAddr *addr, const char *detail){
    DartEvent ev;
    if (!c->on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind; ev.peer = id; ev.detail = detail;
    if (addr){ memcpy(ev.ip, addr->ip, 16); ev.ip_len = addr->ip_len; ev.port = addr->port; }
    c->on_event(c->user, &ev);
}

void dart_node_core_peer_up(void *user, uint32_t id, const DartDiscoveryAddr *addr,
                            const uint8_t *meta, uint16_t meta_len){
    i_DartNodeCore *c = (i_DartNodeCore*)user; uint16_t i; int slot = -1;
    uint16_t frag = dart_meta_frag(meta, meta_len);
    size_t interest_len = 0; const uint8_t *interest = dart_meta_interest(meta, meta_len, &interest_len);
    for (i=0;i<c->max_peers;i++){
        if (c->peers[i].used && c->peers[i].id==id){      /* known peer: addr/interest update */
            memcpy(c->peers[i].ip, addr->ip, 16);
            c->peers[i].ip_len = addr->ip_len; c->peers[i].port = addr->port;
            dart_peer_set_frag(c->transport, id, frag);
            dart__core_set_peer_oob(c, id, meta, meta_len);
            if (interest) dart_apply_peer_interest(c->transport, id, interest, interest_len);
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
    /* the announce blob carries the peer's frag size + pub/sub interest list */
    {   int local = (addr->ip_len==4) && c->is_local && c->is_local(c->is_local_user, addr->ip, addr->ip_len);
        dart_peer_add(c->transport, id, local, frag); }
    dart__core_set_peer_oob(c, id, meta, meta_len);
    if (interest) dart_apply_peer_interest(c->transport, id, interest, interest_len);
    dart__core_fire(c, DART_PEER_UP, id, addr, "peer discovered");
}

void dart_node_core_peer_down(void *user, uint32_t id, DartDiscoveryDownReason reason){
    i_DartNodeCore *c = (i_DartNodeCore*)user; int i = dart__core_find_id(c, id);
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

void dart_node_core_peer_refused(void *user, const DartDiscoveryAddr *addr){
    i_DartNodeCore *c = (i_DartNodeCore*)user;
    dart__core_fire(c, DART_PEER_REFUSED, 0, addr, "peer table full (all active)");
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
