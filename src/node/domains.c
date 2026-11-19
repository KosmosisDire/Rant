/* The census watch. spec/discovery.md, Census. */

#include "domains.h"
#include "../discovery/runtime.h"
#include "../platform/core.h"
#include <string.h>

#define RANT_DOMAINS_FIRST   32u        /* table entries at open, doubled as it fills */
#define RANT_DOMAINS_RX      256u       /* a census copy is at most RANT_DISCOVERY_HEAD_MAX */
#define RANT_DOMAINS_BURST   2048       /* datagrams read per walk, as discovery drains */
#define RANT_DOMAINS_TIMEOUT_US (RANT_ANNOUNCE_INTERVAL_US * 4u)    /* the default peer timeout */

typedef struct {
    uint8_t  uuid[16];
    uint16_t domain;
    uint64_t heard_us;
} i_RantDomainsEntry;

struct RantDomains {
    RantAllocator       pool;           /* owned, reset on close */
    i_RantGroupJoin     join;
    i_RantDomainsEntry *entries;
    uint32_t            n_entries, cap;
};

static i_RantDomainsEntry *i_rant_domains_find(RantDomains *w, const uint8_t uuid[16]){
    uint32_t i;
    for (i = 0; i < w->n_entries; i++)
        if (!memcmp(w->entries[i].uuid, uuid, 16)) return &w->entries[i];
    return NULL;
}

static void i_rant_domains_drop(RantDomains *w, i_RantDomainsEntry *e){
    *e = w->entries[--w->n_entries];
}

/* A full table that cannot grow counts no newcomer until one leaves. */
static void i_rant_domains_heard(RantDomains *w, RantBytes datagram, uint64_t now_us){
    i_RantDiscoveryWire wire;
    i_RantDomainsEntry *e;
    if (i_rant_discovery_decode(datagram, &wire) != I_RANT_DISC_OK) return;
    e = i_rant_domains_find(w, wire.uuid);
    if (wire.flags & RANT_DISCOVERY_FLAG_BYE){ if (e) i_rant_domains_drop(w, e); return; }
    if (!e){
        if (w->n_entries == w->cap){
            uint32_t cap = w->cap * 2u;
            void *grown;
            if (cap > RANT_DOMAINS_MAX_NODES) cap = RANT_DOMAINS_MAX_NODES;
            grown = cap > w->cap ? rant_allocator_alloc(&w->pool, w->entries, cap * sizeof *w->entries) : NULL;
            if (!grown) return;
            w->entries = (i_RantDomainsEntry*)grown;
            w->cap = cap;
        }
        e = &w->entries[w->n_entries++];
        memcpy(e->uuid, wire.uuid, 16);
    }
    e->domain = wire.domain;
    e->heard_us = now_us;
}

static void i_rant_domains_refresh(RantDomains *w){
    uint8_t buf[RANT_DOMAINS_RX], ip[4];
    uint16_t port;
    uint64_t now = i_rant_plat_now_us();
    uint32_t i;
    int guard, n;
    i_rant_group_rescan(&w->join, now);
    for (guard = 0; guard < RANT_DOMAINS_BURST; guard++){
        n = i_rant_net_recv(NULL, w->join.fd, buf, sizeof buf, ip, &port);
        if (n < 0){ if (i_rant_net_would_block(NULL)) break; continue; }
        if (n > 0) i_rant_domains_heard(w, rant_bytes(buf, (size_t)n), now);
    }
    for (i = w->n_entries; i-- > 0;)
        if (now - w->entries[i].heard_us > RANT_DOMAINS_TIMEOUT_US) i_rant_domains_drop(w, &w->entries[i]);
}

RantDomains *rant_domains_open(RantAllocator *alloc, const RantDomainsOpts *opts){
    RantDomainsOpts o;
    RantAllocator pool;
    RantDomains *w;
    i_RantSock fd;
    uint32_t bind_naddr;
    uint16_t port;
    if (!alloc) return (RantDomains*)i_rant_node_open_fail(NULL, NULL, RANT_E_OOM, 0, 0, 0);
    memset(&o, 0, sizeof o);
    if (opts) o = *opts;
    port = o.discovery_port ? o.discovery_port : RANT_DISCOVERY_PORT;
    if (port > 65535u - RANT_CENSUS_PORT_OFFSET)
        return (RantDomains*)i_rant_node_open_fail_why(NULL, NULL, RANT_E_BAD_ADDRESS, "the census port is past 65535");
    port = (uint16_t)(port + RANT_CENSUS_PORT_OFFSET);

    pool = *alloc;
    w = (RantDomains*)rant_allocator_alloc(&pool, NULL, sizeof *w);
    if (!w) return (RantDomains*)i_rant_node_open_fail(NULL, NULL, RANT_E_OOM, 0, 0, sizeof *w);
    memset(w, 0, sizeof *w);
    w->pool = pool;
    w->cap = RANT_DOMAINS_FIRST;
    w->entries = (i_RantDomainsEntry*)rant_allocator_alloc(&w->pool, NULL, w->cap * sizeof *w->entries);
    if (!w->entries){ pool = w->pool; rant_allocator_reset(&pool);
                      return (RantDomains*)i_rant_node_open_fail(NULL, NULL, RANT_E_OOM, 0, 0, 0); }

    if (!i_rant_plat_startup()){ pool = w->pool; rant_allocator_reset(&pool);
                                 return (RantDomains*)i_rant_node_open_fail(NULL, NULL, RANT_E_PLATFORM, 0, 0, 0); }
    fd = i_rant_net_udp_open(NULL);
    if (fd == RANT_SOCK_BAD){
        int e = i_rant_net_last_error(NULL);
        i_rant_plat_cleanup(); pool = w->pool; rant_allocator_reset(&pool);
        return (RantDomains*)i_rant_node_open_fail(NULL, NULL, RANT_E_SOCKET, e, 0, 0);
    }
    /* a loopback watch hears this host alone, as a loopback node does */
    bind_naddr = i_rant_plat_parse_ip(o.multicast_interface);
    bind_naddr = i_rant_plat_is_loopback(bind_naddr) ? i_rant_net_loopback_group_bind(NULL, bind_naddr) : 0;
    {   RantErrorKind why = RANT_E_NONE;
        if (!i_rant_net_bind(NULL, fd, bind_naddr, port, 1)) why = RANT_E_BIND;
        else if (!i_rant_group_join(&w->join, NULL, fd, i_rant_census_group_naddr(), o.multicast_interface))
            why = RANT_E_MCAST_JOIN;
        if (why != RANT_E_NONE){
            int e = i_rant_net_last_error(NULL);
            i_rant_net_close(NULL, fd); i_rant_plat_cleanup(); pool = w->pool; rant_allocator_reset(&pool);
            return (RantDomains*)i_rant_node_open_fail(NULL, NULL, why, e, port, 0);
        }
    }
    i_rant_net_set_nonblock(NULL, fd);
    i_rant_net_suppress_connreset(NULL, fd);
    return w;
}

int rant_domains_next(RantDomains *w, RantIter *it, RantDomainInfo *out){
    uint32_t i, next = 0x10000u, from;
    uint16_t nodes = 0;
    if (!w || !it || !out) return 0;
    if (!it->a) i_rant_domains_refresh(w);
    from = it->a ? (uint32_t)it->c + 1u : 0u;        /* past the domain the last call gave */
    for (i = 0; i < w->n_entries; i++){
        uint32_t d = w->entries[i].domain;
        if (d < from || d > next) continue;
        if (d < next){ next = d; nodes = 0; }
        nodes++;
    }
    if (next > 0xFFFFu) return 0;
    it->a = 1; it->c = (uint16_t)next;
    out->domain = (uint16_t)next;
    out->nodes = nodes;
    return 1;
}

void rant_domains_close(RantDomains *w){
    RantAllocator pool;
    if (!w) return;
    i_rant_net_close(NULL, w->join.fd);
    i_rant_plat_cleanup();
    pool = w->pool;
    rant_allocator_reset(&pool);
}
