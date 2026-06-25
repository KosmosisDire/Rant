/* peer-discovery runtime: the one-tick loop over the dart_discovery core, plus
 * UUID generation. All OS access goes through dart_plat. */

#include "runtime.h"
#include "../platform/core.h"
#include "../common/arena.h"
#include <string.h>

struct DartDiscoveryRt {
    DartDiscoveryState *core;
    i_DartSock            fd;
    uint32_t             group_naddr;   /* discovery multicast group, network order */
    uint16_t             discovery_port;
    uint16_t             max_peers;
    uint32_t             wire_max;    /* scratch buffer size = META_OFF + meta_capacity */
    uint8_t             *rxbuf;       /* arena, wire_max */
    uint8_t             *txbuf;       /* arena, wire_max */
    DartDiscoveryAddr  seeds[DART_DISCOVERY_MAX_SEEDS];
    uint16_t             n_seeds;
};

static void dart_discovery_rt_tx1(DartDiscoveryRt *rt, const uint8_t *out, size_t n_bytes,
                          const uint8_t ip[4], uint16_t port){
    dart_plat_send(rt->fd, out, n_bytes, ip, port);
}

/* unicast one datagram to a peer: at the discovery port, and at its data port too (the
 * only per-process address when processes share the discovery port; the data-socket owner
 * forwards discovery datagrams to its core). */
static void dart_discovery_rt_tx_to(DartDiscoveryRt *rt, const uint8_t *out, size_t n_bytes,
                          const DartDiscoveryAddr *addr){
    if (addr->ip_len != 4) return;
    dart_discovery_rt_tx1(rt, out, n_bytes, addr->ip, rt->discovery_port);
    if (addr->port && addr->port != rt->discovery_port) dart_discovery_rt_tx1(rt, out, n_bytes, addr->ip, addr->port);
}

/* send to the group, every seed, and every known peer. Survives multicast outages;
 * receivers dedup by uuid. */
static void dart_discovery_rt_tx(DartDiscoveryRt *rt, const uint8_t *out, size_t n_bytes){
    uint16_t s, discovery_port = rt->discovery_port;
    DartDiscoveryAddr addr;
    uint8_t group_ip[4];
    dart_plat_naddr_to_ip4(rt->group_naddr, group_ip);
    dart_plat_send(rt->fd, out, n_bytes, group_ip, discovery_port);
    for (s=0; s<rt->n_seeds; s++){
        const DartDiscoveryAddr *seed = &rt->seeds[s];
        if (seed->ip_len != 4) continue;
        dart_discovery_rt_tx1(rt, out, n_bytes, seed->ip, seed->port ? seed->port : discovery_port);
    }
    for (s=0; s<rt->max_peers; s++){
        if (!dart_discovery_peer_addr(rt->core, s, &addr)) continue;
        dart_discovery_rt_tx_to(rt, out, n_bytes, &addr);
    }
}

void dart_discovery_rt_feed(DartDiscoveryRt *rt, const uint8_t *src_ip, uint8_t src_ip_len,
                    const void *datagram, size_t len){
    if (!rt) return;
    dart_discovery_on_datagram(rt->core, src_ip, src_ip_len, datagram, len, dart_plat_now_us());
}

void dart_discovery_rt_set_meta(DartDiscoveryRt *rt, const uint8_t *meta, uint16_t meta_len){
    if (rt) dart_discovery_set_meta(rt->core, meta, meta_len);
}

void dart_discovery_rt_replay(DartDiscoveryRt *rt){
    if (rt) dart_discovery_replay_peers(rt->core);
}

/* A loopback (127/8) or unspecified address is never a usable multicast egress. */
static int dart__if_routable(uint32_t naddr){
    uint8_t ip[4];
    if (!naddr) return 0;
    dart_plat_naddr_to_ip4(naddr, ip);
    return ip[0] != 127;
}
/* Link-local autoconfig (169.254/16): an adapter with no DHCP lease. Usable only as a
 * last resort, behind any properly addressed LAN interface. */
static int dart__if_apipa(uint32_t naddr){
    uint8_t ip[4];
    dart_plat_naddr_to_ip4(naddr, ip);
    return ip[0] == 169 && ip[1] == 254;
}

/* Every multicast send and join should pin to this one interface. Auto path (no
 * explicit --if): route-probe the group, which is correct on POSIX. But Windows
 * source-address selection for a multicast destination can resolve to loopback, which
 * would strand all traffic on 127.0.0.1; so when the group probe isn't a routable
 * address, probe the default route via a reserved global-unicast destination (connect()
 * on UDP sends nothing) to get the LAN interface, then finally enumerate and prefer a
 * real (non-APIPA) LAN address. Returns 0 only if nothing usable was found (the OS then
 * picks the default interface). */
uint32_t dart_discovery_mcast_if_for(uint32_t group_naddr, uint16_t port){
    uint32_t ip, ifs[16];
    int n, i, apipa = -1;

    ip = dart_plat_route_src(group_naddr, port);
    if (dart__if_routable(ip)) return ip;

    /* 192.0.2.1 is TEST-NET-1 (RFC 5737): never a real host, so this resolves only the
     * default-route source interface; no datagram is transmitted. */
    ip = dart_plat_route_src(dart_plat_ipv4(192u, 0u, 2u, 1u), port);
    if (dart__if_routable(ip)) return ip;

    n = dart_plat_local_ipv4s(ifs, (int)(sizeof ifs / sizeof ifs[0]));
    for (i = 0; i < n; i++){
        if (!dart__if_routable(ifs[i])) continue;
        if (dart__if_apipa(ifs[i])){ if (apipa < 0) apipa = i; continue; }
        return ifs[i];                 /* first real (DHCP/static) LAN address */
    }
    return apipa >= 0 ? ifs[apipa] : 0;  /* APIPA only if nothing better; else OS default */
}

int dart_discovery_make_uuid4(uint8_t out[16]){
    if (!dart_plat_random(out, 16)) return 0;
    out[6] = (uint8_t)((out[6] & 0x0Fu) | 0x40u);  /* version 4 */
    out[8] = (uint8_t)((out[8] & 0x3Fu) | 0x80u);  /* variant 10x */
    return 1;
}

static void dart_discovery_auto_uuid(uint8_t out[16]){
    char host[80]; uint64_t seed; size_t hostname_len;
    if (dart_discovery_make_uuid4(out)) return;     /* normal path */

    /* No CSPRNG: derive a best-effort unique id from hostname, pid and clock. */
    hostname_len = dart_plat_hostname(host, sizeof host);
    seed = (dart_plat_pid() << 32) ^ dart_plat_now_us();
    dart_discovery_make_uuid(out, (const uint8_t*)host, hostname_len, seed);
}

/* Single source of the discovery-runtime arena layout: the rt struct, the rx/tx wire
   scratch buffers, then the discovery-core sub-arena. measure feeds required_memory;
   build feeds open -- one definition. */
typedef struct {
    DartDiscoveryRt *rt;
    uint8_t *rxbuf, *txbuf, *core;
    size_t   wire_max, core_bytes;
} i_DartRtBlocks;
static void dart__rt_layout(i_DartBump *b, const DartDiscoveryConfig *c, i_DartRtBlocks *o){
    o->wire_max   = dart_discovery_wire_size(c->meta_capacity);
    o->rt    = (DartDiscoveryRt*)dart_take(b, sizeof(struct DartDiscoveryRt), 16);
    o->rxbuf = (uint8_t*)dart_take(b, o->wire_max, 16);
    o->txbuf = (uint8_t*)dart_take(b, o->wire_max, 16);
    o->core_bytes = dart_discovery_required_memory(c);
    o->core  = (uint8_t*)dart_take(b, o->core_bytes, 16);
}

size_t dart_discovery_rt_required_memory(const DartDiscoveryRtConfig *cfg){
    DartDiscoveryConfig c; i_DartBump b; i_DartRtBlocks blk;
    if (!cfg) return 0;
    c = cfg->discovery;
    dart_discovery_config_defaults(&c);
    memset(&b, 0, sizeof b);
    dart__rt_layout(&b, &c, &blk);
    return b.offset + 16u;     /* slack to align the caller's mem up to base */
}

DartDiscoveryRt *dart_discovery_rt_open(void *mem, size_t cap, const DartDiscoveryRtConfig *cfg){
    DartDiscoveryRtConfig c;
    DartDiscoveryRt *rt;
    uint8_t *base, *core_mem;
    size_t need;
    i_DartRtBlocks blk;
    i_DartSock fd;
    int allzero = 1, i;
    uint8_t ttl;
    uint32_t group_naddr, interface_ip;
    const char *group;

    if (!mem || !cfg) return NULL;
    c = *cfg;
    dart_discovery_config_defaults(&c.discovery);
    group     = c.group     ? c.group     : "239.255.0.7";
    if (c.discovery_port == 0)    c.discovery_port  = 7400;
    ttl  = c.ttl ? c.ttl : 1;

    need = dart_discovery_rt_required_memory(&c);
    if (cap < need) return NULL;

    base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    {   i_DartBump b; memset(&b, 0, sizeof b);
        b.base = base; b.cap = cap - (size_t)(base - (uint8_t*)mem);
        dart__rt_layout(&b, &c.discovery, &blk); }
    rt = blk.rt;
    rt->wire_max = (uint32_t)blk.wire_max;
    rt->rxbuf    = blk.rxbuf;
    rt->txbuf    = blk.txbuf;
    core_mem     = blk.core;

    /* auto-generate a UUID if the caller left it zero */
    for (i=0;i<16;i++) if (c.discovery.uuid[i]) { allzero = 0; break; }

    if (!dart_plat_startup()) return NULL;
    if (allzero) dart_discovery_auto_uuid(c.discovery.uuid);

    rt->core = dart_discovery_init(core_mem, cap - (size_t)(core_mem - (uint8_t*)mem), &c.discovery);
    if (!rt->core){ dart_plat_cleanup(); return NULL; }

    fd = dart_plat_udp_open();
    if (fd == DART_SOCK_BAD){ dart_plat_cleanup(); return NULL; }
    if (!dart_plat_bind(fd, 0, c.discovery_port, 1)){ dart_plat_close(fd); dart_plat_cleanup(); return NULL; }

    /* pin join and egress to one deterministic interface */
    group_naddr = dart_plat_parse_ip(group);
    interface_ip = c.multicast_interface ? dart_plat_parse_ip(c.multicast_interface)
                      : dart_discovery_mcast_if_for(group_naddr, c.discovery_port);
    if (!dart_plat_mcast_join(fd, group_naddr, interface_ip)){
        dart_plat_close(fd); dart_plat_cleanup(); return NULL;
    }
    dart_plat_mcast_setif(fd, interface_ip);
    dart_plat_mcast_ttl(fd, ttl);
    /* loop always on (uuid self-filter drops echoes); needed for multi-instance per host */
    dart_plat_mcast_loop(fd, 1);

    rt->fd = fd;
    rt->group_naddr = group_naddr;
    rt->discovery_port = c.discovery_port;
    rt->max_peers = c.discovery.max_peers;
    rt->n_seeds = 0;
    if (c.seeds){
        uint16_t k, seed_count = c.n_seeds;
        if (seed_count > DART_DISCOVERY_MAX_SEEDS) seed_count = DART_DISCOVERY_MAX_SEEDS;
        for (k=0;k<seed_count;k++) rt->seeds[k] = c.seeds[k];
        rt->n_seeds = seed_count;
    }
    return rt;
}

int dart_discovery_rt_poll(DartDiscoveryRt *rt, int timeout_ms){
    i_DartPollfd pfd;
    DartDiscoveryAddr to;
    int got = 0; size_t n_bytes;

    pfd.fd = rt->fd; pfd.events = DART_POLLIN; pfd.revents = 0;
    if (dart_plat_poll(&pfd, 1, timeout_ms) < 0) return -1;

    if (pfd.revents & DART_POLLIN){
        uint8_t src_ip[4];
        int n = dart_plat_recv(rt->fd, rt->rxbuf, rt->wire_max, src_ip, NULL);
        if (n > 0){
            dart_discovery_on_datagram(rt->core, src_ip, 4, rt->rxbuf, (size_t)n, dart_plat_now_us());
            got = 1;
        }
    }

    n_bytes = dart_discovery_update(rt->core, dart_plat_now_us(), rt->txbuf, rt->wire_max);
    if (n_bytes) dart_discovery_rt_tx(rt, rt->txbuf, n_bytes);

    /* targeted unicast: replies to soliciters + re-fetch requests for stale blobs */
    while ((n_bytes = dart_discovery_poll_targeted(rt->core, rt->txbuf, rt->wire_max, &to)) != 0)
        dart_discovery_rt_tx_to(rt, rt->txbuf, n_bytes, &to);
    return got;
}

int dart_discovery_rt_settle(DartDiscoveryRt *rt, int quiet_ms, int timeout_ms){
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

void dart_discovery_rt_close(DartDiscoveryRt *rt, int send_bye){
    if (!rt) return;
    if (send_bye){
        size_t n_bytes = dart_discovery_leave(rt->core, rt->txbuf, rt->wire_max);
        if (n_bytes) dart_discovery_rt_tx(rt, rt->txbuf, n_bytes);
    }
    dart_plat_close(rt->fd);
    dart_plat_cleanup();
}
