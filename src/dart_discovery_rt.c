/* peer-discovery runtime: the one-tick loop over the dart_discovery core, plus
 * UUID generation. All OS access goes through dart_plat. */

#include "dart_discovery_rt.h"
#include "dart_plat.h"
#include <string.h>

struct dart_discovery_rt {
    dart_discovery_state *core;
    dart_sock            fd;
    uint32_t             grp_naddr;   /* discovery multicast group, network order */
    uint16_t             disc_port;
    uint16_t             max_peers;
    uint32_t             wire_max;    /* scratch buffer size = META_OFF + meta_cap */
    uint8_t             *rxbuf;       /* arena, wire_max */
    uint8_t             *txbuf;       /* arena, wire_max */
    dart_discovery_addr  seeds[DART_DISCOVERY_MAX_SEEDS];
    uint16_t             n_seeds;
};

static void dart_discovery_rt_tx1(dart_discovery_rt *rt, const uint8_t *out, size_t m,
                          const uint8_t ip[4], uint16_t port){
    dart_plat_send(rt->fd, out, m, ip, port);
}

/* unicast one datagram to a peer: at the disc port, and at its data port too (the
 * only per-process address when processes share the disc port; the data-socket owner
 * forwards discovery datagrams to its core). */
static void dart_discovery_rt_tx_to(dart_discovery_rt *rt, const uint8_t *out, size_t m,
                          const dart_discovery_addr *a){
    if (a->ip_len != 4) return;
    dart_discovery_rt_tx1(rt, out, m, a->ip, rt->disc_port);
    if (a->port && a->port != rt->disc_port) dart_discovery_rt_tx1(rt, out, m, a->ip, a->port);
}

/* send to the group, every seed, and every known peer. Survives multicast outages;
 * receivers dedup by uuid. */
static void dart_discovery_rt_tx(dart_discovery_rt *rt, const uint8_t *out, size_t m){
    uint16_t s, dport = rt->disc_port;
    dart_discovery_addr a;
    uint8_t gip[4];
    dart_plat_naddr_to_ip4(rt->grp_naddr, gip);
    dart_plat_send(rt->fd, out, m, gip, dport);
    for (s=0; s<rt->n_seeds; s++){
        const dart_discovery_addr *sd = &rt->seeds[s];
        if (sd->ip_len != 4) continue;
        dart_discovery_rt_tx1(rt, out, m, sd->ip, sd->port ? sd->port : dport);
    }
    for (s=0; s<rt->max_peers; s++){
        if (!dart_discovery_peer_addr(rt->core, s, &a)) continue;
        dart_discovery_rt_tx_to(rt, out, m, &a);
    }
}

void dart_discovery_rt_feed(dart_discovery_rt *rt, const uint8_t *src_ip, uint8_t src_ip_len,
                    const void *dg, size_t len){
    if (!rt) return;
    dart_discovery_on_datagram(rt->core, src_ip, src_ip_len, dg, len, dart_plat_now_us());
}

void dart_discovery_rt_set_meta(dart_discovery_rt *rt, const uint8_t *meta, uint16_t meta_len){
    if (rt) dart_discovery_set_meta(rt->core, meta, meta_len);
}

/* Every multicast send and join should pin to this one interface. */
uint32_t dart_discovery_mcast_if_for(uint32_t grp_naddr, uint16_t port){
    return dart_plat_route_src(grp_naddr, port);
}

int dart_discovery_make_uuid4(uint8_t out[16]){
    if (!dart_plat_random(out, 16)) return 0;
    out[6] = (uint8_t)((out[6] & 0x0Fu) | 0x40u);  /* version 4 */
    out[8] = (uint8_t)((out[8] & 0x3Fu) | 0x80u);  /* variant 10x */
    return 1;
}

static void dart_discovery_auto_uuid(uint8_t out[16]){
    char host[80]; uint64_t seed; size_t hl;
    if (dart_discovery_make_uuid4(out)) return;     /* normal path */

    /* No CSPRNG: derive a best-effort unique id from hostname, pid and clock. */
    hl = dart_plat_hostname(host, sizeof host);
    seed = (dart_plat_pid() << 32) ^ dart_plat_now_us();
    dart_discovery_make_uuid(out, (const uint8_t*)host, hl, seed);
}

static uint32_t dart_discovery_rt_wire_max(const dart_discovery_config *c){
    uint32_t cap = c->meta_cap ? c->meta_cap : DART_DISCOVERY_META_MAX;
    uint32_t w = (uint32_t)DART_DISCOVERY_META_OFF + cap;
    return w < DART_DISCOVERY_WIRE_MAX ? DART_DISCOVERY_WIRE_MAX : w;
}

size_t dart_discovery_rt_required_memory(const dart_discovery_rt_config *cfg){
    dart_discovery_config c;
    size_t rt = (sizeof(struct dart_discovery_rt) + 15u) & ~(size_t)15u;
    size_t wmax;
    if (!cfg) return 0;
    c = cfg->disc;
    if (c.announce_us == 0) c.announce_us = 1000000u;
    if (c.timeout_us  == 0) c.timeout_us  = c.announce_us * 7u / 2u;
    if (c.max_peers   == 0) c.max_peers   = 32u;
    wmax = ((size_t)dart_discovery_rt_wire_max(&c) + 15u) & ~(size_t)15u;
    return 16u + rt + 2u*wmax + dart_discovery_required_memory(&c);
}

dart_discovery_rt *dart_discovery_rt_open(void *mem, size_t cap, const dart_discovery_rt_config *cfg){
    dart_discovery_rt_config c;
    dart_discovery_rt *rt;
    uint8_t *base, *core_mem;
    size_t rtsz, wmax, need;
    dart_sock fd;
    int allzero = 1, i;
    uint8_t ttl;
    uint32_t grp_naddr, ifip;
    const char *group;

    if (!mem || !cfg) return NULL;
    c = *cfg;
    if (c.disc.announce_us == 0) c.disc.announce_us = 1000000u;
    if (c.disc.timeout_us  == 0) c.disc.timeout_us  = c.disc.announce_us * 7u / 2u;
    if (c.disc.max_peers   == 0) c.disc.max_peers   = 32u;
    group     = c.group     ? c.group     : "239.255.0.7";
    if (c.disc_port == 0)    c.disc_port  = 7400;
    ttl  = c.ttl ? c.ttl : 1;

    need = dart_discovery_rt_required_memory(&c);
    if (cap < need) return NULL;

    base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    rt   = (dart_discovery_rt*)base;
    rtsz = (sizeof(struct dart_discovery_rt) + 15u) & ~(size_t)15u;
    wmax = ((size_t)dart_discovery_rt_wire_max(&c.disc) + 15u) & ~(size_t)15u;

    rt->wire_max = (uint32_t)dart_discovery_rt_wire_max(&c.disc);
    rt->rxbuf    = base + rtsz;
    rt->txbuf    = rt->rxbuf + wmax;
    core_mem     = rt->txbuf + wmax;

    /* auto-generate a UUID if the caller left it zero */
    for (i=0;i<16;i++) if (c.disc.uuid[i]) { allzero = 0; break; }

    if (!dart_plat_startup()) return NULL;
    if (allzero) dart_discovery_auto_uuid(c.disc.uuid);

    rt->core = dart_discovery_init(core_mem, cap - (size_t)(core_mem - (uint8_t*)mem), &c.disc);
    if (!rt->core){ dart_plat_cleanup(); return NULL; }

    fd = dart_plat_udp_open();
    if (fd == DART_SOCK_BAD){ dart_plat_cleanup(); return NULL; }
    if (!dart_plat_bind(fd, 0, c.disc_port, 1)){ dart_plat_close(fd); dart_plat_cleanup(); return NULL; }

    /* pin join and egress to one deterministic interface */
    grp_naddr = dart_plat_parse_ip(group);
    ifip = c.mcast_if ? dart_plat_parse_ip(c.mcast_if)
                      : dart_plat_route_src(grp_naddr, c.disc_port);
    if (!dart_plat_mcast_join(fd, grp_naddr, ifip)){
        dart_plat_close(fd); dart_plat_cleanup(); return NULL;
    }
    dart_plat_mcast_setif(fd, ifip);
    dart_plat_mcast_ttl(fd, ttl);
    /* loop always on (uuid self-filter drops echoes); needed for multi-instance per host */
    dart_plat_mcast_loop(fd, 1);

    rt->fd = fd;
    rt->grp_naddr = grp_naddr;
    rt->disc_port = c.disc_port;
    rt->max_peers = c.disc.max_peers;
    rt->n_seeds = 0;
    if (c.seeds){
        uint16_t k, ns = c.n_seeds;
        if (ns > DART_DISCOVERY_MAX_SEEDS) ns = DART_DISCOVERY_MAX_SEEDS;
        for (k=0;k<ns;k++) rt->seeds[k] = c.seeds[k];
        rt->n_seeds = ns;
    }
    return rt;
}

int dart_discovery_rt_poll(dart_discovery_rt *rt, int timeout_ms){
    dart_pollfd pfd;
    dart_discovery_addr to;
    int got = 0; size_t m;

    pfd.fd = rt->fd; pfd.events = DART_POLLIN; pfd.revents = 0;
    if (dart_plat_poll(&pfd, 1, timeout_ms) < 0) return -1;

    if (pfd.revents & DART_POLLIN){
        uint8_t sip[4];
        int n = dart_plat_recv(rt->fd, rt->rxbuf, rt->wire_max, sip, NULL);
        if (n > 0){
            dart_discovery_on_datagram(rt->core, sip, 4, rt->rxbuf, (size_t)n, dart_plat_now_us());
            got = 1;
        }
    }

    m = dart_discovery_update(rt->core, dart_plat_now_us(), rt->txbuf, rt->wire_max);
    if (m) dart_discovery_rt_tx(rt, rt->txbuf, m);

    /* targeted unicast: replies to soliciters + re-fetch requests for stale blobs */
    while ((m = dart_discovery_poll_targeted(rt->core, rt->txbuf, rt->wire_max, &to)) != 0)
        dart_discovery_rt_tx_to(rt, rt->txbuf, m, &to);
    return got;
}

int dart_discovery_rt_settle(dart_discovery_rt *rt, int quiet_ms, int timeout_ms){
    uint64_t start, last_change, last_solicit = 0;
    uint16_t count;
    if (!rt) return 0;
    start = dart_plat_now_us(); last_change = start;
    count = dart_discovery_peer_count(rt->core);
    for (;;){
        uint64_t now = dart_plat_now_us(); uint16_t c;
        if (now - last_solicit >= 250000u){     /* (re)solicit ~4x/s so a lost one retries */
            dart_discovery_solicit(rt->core); last_solicit = now;
        }
        dart_discovery_rt_poll(rt, 10);          /* sends the solicit, takes in replies */
        now = dart_plat_now_us();
        c = dart_discovery_peer_count(rt->core);
        if (c > count){ count = c; last_change = now; }   /* grew: keep waiting */
        if (count > 0 && now - last_change >= (uint64_t)quiet_ms*1000u) break;
        if (now - start >= (uint64_t)timeout_ms*1000u) break;
    }
    return (int)count;
}

void dart_discovery_rt_close(dart_discovery_rt *rt, int send_bye){
    if (!rt) return;
    if (send_bye){
        size_t m = dart_discovery_leave(rt->core, rt->txbuf, rt->wire_max);
        if (m) dart_discovery_rt_tx(rt, rt->txbuf, m);
    }
    dart_plat_close(rt->fd);
    dart_plat_cleanup();
}
