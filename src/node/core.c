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
} dart_node_peer;

struct dart_node_core {
    dart_state           *transport;
    dart_node_peer       *peers;
    uint16_t              max_peers;
    dart_event_fn         on_event;
    void                 *user;
    dart_node_is_local_fn is_local;
    void                 *is_local_user;
    int                   oob_capable;
    uint8_t               oob_host[16];
};

/* arena layout: the core struct, then the peer table. One sequence so measure and
   build agree (dart_take with a NULL base just advances the offset). */
static void dart__core_layout(dart_bump *b, uint16_t max_peers,
                              dart_node_core **out_c, uint8_t **out_peers){
    dart_node_core *c = (dart_node_core*)dart_take(b, sizeof(struct dart_node_core), 16);
    uint8_t *peers    = (uint8_t*)       dart_take(b, (size_t)max_peers * sizeof(dart_node_peer), 16);
    if (out_c)     *out_c     = c;
    if (out_peers) *out_peers = peers;
}

size_t dart_node_core_required_memory(uint16_t max_peers){
    dart_bump b; memset(&b, 0, sizeof b);
    dart__core_layout(&b, max_peers, NULL, NULL);
    return b.offset + 16u;   /* slack to align the caller's mem up to base */
}

dart_node_core *dart_node_core_init(void *mem, size_t cap, const dart_node_core_config *cfg){
    dart_bump b; dart_node_core *c; uint8_t *base, *peers;
    if (!mem || !cfg || !cfg->transport || cfg->max_peers == 0) return NULL;
    if (cap < dart_node_core_required_memory(cfg->max_peers)) return NULL;
    base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    memset(&b, 0, sizeof b); b.base = base; b.cap = cap - (size_t)(base - (uint8_t*)mem);
    dart__core_layout(&b, cfg->max_peers, &c, &peers);

    memset(c, 0, sizeof *c);
    c->transport     = cfg->transport;
    c->peers         = (dart_node_peer*)peers;
    c->max_peers     = cfg->max_peers;
    c->on_event      = cfg->on_event;      c->user = cfg->user;
    c->is_local      = cfg->is_local;      c->is_local_user = cfg->is_local_user;
    c->oob_capable   = cfg->oob_capable;
    memcpy(c->oob_host, cfg->oob_host, 16);
    memset(peers, 0, (size_t)cfg->max_peers * sizeof(dart_node_peer));
    return c;
}

static int dart__core_find_id(dart_node_core *c, uint32_t id){
    uint16_t i;
    for (i=0;i<c->max_peers;i++) if (c->peers[i].used && c->peers[i].id==id) return (int)i;
    return -1;
}

#ifdef DART_SHM
/* a peer can receive our out-of-band (SHM) payload iff we are OOB-capable, it
 * advertised an OOB host in its meta blob, and that host equals ours (same kernel).
 * Set the transport's per-peer flag. The core knows nothing of SHM beyond this. */
static void dart__core_set_peer_oob(dart_node_core *c, uint32_t id, const uint8_t *meta, uint16_t meta_len){
    uint8_t host[16];
    int oob = c->oob_capable && dart_meta_shm(meta, meta_len, host) &&
              memcmp(host, c->oob_host, 16) == 0;
    dart_peer_set_shm(c->transport, id, oob);
}
#else
#define dart__core_set_peer_oob(c, id, meta, meta_len) ((void)0)
#endif

static void dart__core_fire(dart_node_core *c, dart_event_kind kind, uint32_t id,
                            const dart_discovery_addr *addr, const char *detail){
    dart_event ev;
    if (!c->on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind; ev.peer = id; ev.detail = detail;
    if (addr){ memcpy(ev.ip, addr->ip, 16); ev.ip_len = addr->ip_len; ev.port = addr->port; }
    c->on_event(c->user, &ev);
}

void dart_node_core_peer_up(void *user, uint32_t id, const dart_discovery_addr *addr,
                            const uint8_t *meta, uint16_t meta_len){
    dart_node_core *c = (dart_node_core*)user; uint16_t i; int slot = -1;
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

void dart_node_core_peer_down(void *user, uint32_t id, dart_discovery_down_reason reason){
    dart_node_core *c = (dart_node_core*)user; int i = dart__core_find_id(c, id);
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

void dart_node_core_peer_refused(void *user, const dart_discovery_addr *addr){
    dart_node_core *c = (dart_node_core*)user;
    dart__core_fire(c, DART_PEER_REFUSED, 0, addr, "peer table full (all active)");
}

int dart_node_core_addr_for_id(dart_node_core *c, uint32_t id, uint8_t ip[4], uint16_t *port){
    int i = dart__core_find_id(c, id);
    if (i < 0) return 0;
    memcpy(ip, c->peers[i].ip, 4);
    if (port) *port = c->peers[i].port;
    return 1;
}

int dart_node_core_id_for_addr(dart_node_core *c, const uint8_t ip[4], uint16_t port, uint32_t *id){
    uint16_t i;
    for (i=0;i<c->max_peers;i++)
        if (c->peers[i].used && c->peers[i].ip_len>=4 && c->peers[i].port==port
            && memcmp(c->peers[i].ip, ip, 4)==0){ if (id) *id = c->peers[i].id; return 1; }
    return 0;
}

uint16_t dart_node_core_max_peers(dart_node_core *c){ return c->max_peers; }

int dart_node_core_peer_at(dart_node_core *c, uint16_t slot, uint32_t *id,
                           uint8_t ip[16], uint8_t *ip_len, uint16_t *port){
    dart_node_peer *p;
    if (slot >= c->max_peers) return 0;
    p = &c->peers[slot];
    if (!p->used) return 0;
    if (id)     *id = p->id;
    if (ip)     memcpy(ip, p->ip, 16);
    if (ip_len) *ip_len = p->ip_len;
    if (port)   *port = p->port;
    return 1;
}
