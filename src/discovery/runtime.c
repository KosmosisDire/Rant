/* The discovery runtime. All OS access goes through the platform layer. */

#include "runtime.h"
#include "../platform/core.h"
#include "../common/arena.h"
#include <string.h>

#define DART_DISCOVERY_BYE_SENDS 3   /* one shot UDP, so resend. Receivers dedup by uuid */
#define DART_DISCOVERY_IF_SCAN_US 3000000u   /* re enumerate interfaces this often in auto mode */

struct DartDiscovery {
    DartDiscoveryState *core;
    i_DartSock            fd;
    i_DartSock            tx_fd;         /* unicast TX leaves here: the node's data socket, the own
                                            unicast_fd, else fd */
    i_DartSock            unicast_fd;    /* our unicast RX port, DART_SOCK_BAD with a data_port */
    i_DartIface          ifs[DART_DISCOVERY_MAX_SUBNETS];  /* joined and announced out of */
    uint8_t              n_ifs;          /* 0 = none usable, the OS picks */
    uint8_t              pinned;         /* explicit multicast_interface, never rescanned */
    uint8_t              unicast_only;   /* no joins and no group sends */
    uint64_t             if_scan_us;     /* next interface re enumeration */
    uint32_t             group_naddr;   /* network order */
    uint16_t             discovery_port;
    uint16_t             max_peers;
    uint32_t             wire_max;    /* scratch buffer size */
    uint8_t             *rxbuf;       /* arena, wire_max */
    uint8_t             *txbuf;       /* arena, wire_max */
    DartDiscoveryPeer   *peer_view;   /* arena, max_peers: the snapshot for dart_discovery_peers */
    DartAllocator       pool;        /* the open path's allocator, reset at close. Zeroed on the
                                         place path, where the caller owns the memory */
    DartDiscoveryAddr  seeds[DART_DISCOVERY_MAX_SEEDS];
    uint16_t             n_seeds;
};

static void i_dart_discovery_tx1(DartDiscovery *d, const uint8_t *out, size_t n_bytes,
                          const uint8_t ip[4], uint16_t port){
    i_dart_plat_send(d->tx_fd, out, n_bytes, ip, port);
}

/* One copy: the data port once the blob named one, else the discovery port. A peer whose
 * data port is unreachable should look dead. */
static void i_dart_discovery_tx_to(DartDiscovery *d, const uint8_t *out, size_t n_bytes,
                          const DartDiscoveryAddr *addr){
    if (addr->ip_len != 4) return;
    i_dart_discovery_tx1(d, out, n_bytes, addr->ip, addr->port ? addr->port : d->discovery_port);
}

/* An observed source exactly, else the locator. */
static void i_dart_discovery_tx_peer(DartDiscovery *d, const uint8_t *out, size_t n_bytes,
                          const DartDiscoveryAddr *addr, int kind){
    if (kind == 2){ if (addr->ip_len == 4) i_dart_discovery_tx1(d, out, n_bytes, addr->ip, addr->port); }
    else if (kind == 1) i_dart_discovery_tx_to(d, out, n_bytes, addr);
}

/* Out of every interface, so each copy carries the source correct for its path. A failed
 * sendto on one adapter does not stop the others. */
static void i_dart_discovery_tx_group(DartDiscovery *d, const uint8_t *out, size_t n_bytes){
    uint8_t group_ip[4], i;
    if (d->unicast_only) return;   /* seeds and known peers are the only paths */
    i_dart_plat_naddr_to_ip4(d->group_naddr, group_ip);
    if (!d->n_ifs){ i_dart_plat_send(d->fd, out, n_bytes, group_ip, d->discovery_port); return; }
    for (i = 0; i < d->n_ifs; i++){
        i_dart_plat_mcast_setif(d->fd, d->ifs[i].addr);
        i_dart_plat_send(d->fd, out, n_bytes, group_ip, d->discovery_port);
    }
}

/* The group, every seed and every known peer. skip_uuid leaves out a proxy's own origin,
 * whose self filter would only discard it. */
static void i_dart_discovery_tx(DartDiscovery *d, const uint8_t *out, size_t n_bytes,
                          const uint8_t *skip_uuid){
    uint16_t s, discovery_port = d->discovery_port;
    DartDiscoveryAddr addr;
    i_dart_discovery_tx_group(d, out, n_bytes);
    for (s=0; s<d->n_seeds; s++){
        const DartDiscoveryAddr *seed = &d->seeds[s];
        if (seed->ip_len != 4) continue;
        i_dart_discovery_tx1(d, out, n_bytes, seed->ip, seed->port ? seed->port : discovery_port);
    }
    for (s=0; s<d->max_peers; s++){
        int kind;
        if (skip_uuid){
            DartDiscoveryPeer v;
            if (dart_discovery_peer_at(d->core, s, &v) && memcmp(v.uuid, skip_uuid, 16)==0)
                continue;
        }
        kind = dart_discovery_peer_addr(d->core, s, &addr);
        if (kind) i_dart_discovery_tx_peer(d, out, n_bytes, &addr, kind);
    }
}

void dart_discovery_feed(DartDiscovery *d, const DartDiscoveryAddr *src, DartBytes datagram){
    if (!d) return;
    /* the data socket is the DATA channel */
    dart_discovery_on_datagram(d->core, src, DART_DISCOVERY_VIA_DATA, datagram, i_dart_plat_now_us());
}

void dart_discovery_set_tx_fd(DartDiscovery *d, i_DartSock fd){
    if (d) d->tx_fd = (fd == DART_SOCK_BAD) ? d->fd : fd;
}

void dart_discovery_advertise(DartDiscovery *d, DartBytes meta){
    if (d) dart_discovery_set_meta(d->core, meta);
}

void dart_discovery_replay(DartDiscovery *d){
    if (d) dart_discovery_replay_peers(d->core);
}

int dart_discovery_pollfds(DartDiscovery *d, i_DartSock out[2]){
    int n = 0;
    if (!d) return 0;
    out[n++] = d->fd;
    if (d->unicast_fd != DART_SOCK_BAD) out[n++] = d->unicast_fd;
    return n;
}

/* The up, non loopback interfaces. Loopback is the fallback for a host with nothing else
 * up, never a member of the normal set. 0 only when the platform cannot enumerate. */
static uint8_t i_dart_discovery_if_scan(i_DartIface *out, uint8_t max){
    int n = i_dart_plat_local_ifaces(out, (int)max), i;
    uint8_t k = 0;
    for (i = 0; i < n; i++){
        uint8_t ip[4];
        i_dart_plat_naddr_to_ip4(out[i].addr, ip);
        if (!out[i].addr || ip[0] == 127) continue;   /* unspecified or loopback */
        out[k++] = out[i];
    }
    if (!k){                        /* nothing up, or a loopback only host */
        out[0].addr = i_dart_plat_ipv4(127u, 0u, 0u, 1u);
        out[0].mask = i_dart_plat_ipv4(255u, 0u, 0u, 0u);
        k = 1;
    }
    return k;
}

/* The netmask the host reports for addr, 0 if none. A pinned interface still ranks. */
static uint32_t i_dart_discovery_if_mask_of(uint32_t addr){
    i_DartIface all[DART_DISCOVERY_MAX_SUBNETS];
    int n = i_dart_plat_local_ifaces(all, DART_DISCOVERY_MAX_SUBNETS), i;
    for (i = 0; i < n; i++) if (all[i].addr == addr) return all[i].mask;
    return 0;
}

static int i_dart_discovery_if_has(const i_DartIface *set, uint8_t n, uint32_t addr){
    uint8_t i;
    for (i = 0; i < n; i++) if (set[i].addr == addr) return 1;
    return 0;
}

/* Leaves what went away, joins what appeared, and hands the core our subnets. A per
 * interface join failure is ordinary, so this returns how many memberships are live. */
static uint8_t i_dart_discovery_if_apply(DartDiscovery *d, const i_DartIface *want, uint8_t n_want){
    DartDiscoverySubnet nets[DART_DISCOVERY_MAX_SUBNETS];
    uint8_t i, joined = 0;
    if (n_want > DART_DISCOVERY_MAX_SUBNETS) n_want = DART_DISCOVERY_MAX_SUBNETS;
    if (d->unicast_only) joined = n_want;   /* no membership, but the core still gets our subnets */
    else {
        for (i = 0; i < d->n_ifs; i++)
            if (!i_dart_discovery_if_has(want, n_want, d->ifs[i].addr))
                i_dart_plat_mcast_leave(d->fd, d->group_naddr, d->ifs[i].addr);
        for (i = 0; i < n_want; i++){
            if (i_dart_discovery_if_has(d->ifs, d->n_ifs, want[i].addr)){ joined++; continue; }
            if (i_dart_plat_mcast_join(d->fd, d->group_naddr, want[i].addr)) joined++;
        }
    }
    for (i = 0; i < n_want; i++){
        i_dart_plat_naddr_to_ip4(want[i].addr, nets[i].ip);
        i_dart_plat_naddr_to_ip4(want[i].mask, nets[i].mask);
        d->ifs[i] = want[i];
    }
    d->n_ifs = n_want;
    dart_discovery_set_local_subnets(d->core, nets, n_want);
    return joined;
}

/* A link that comes up after open is joined, and one that goes away drops its membership. */
static void i_dart_discovery_if_refresh(DartDiscovery *d){
    i_DartIface want[DART_DISCOVERY_MAX_SUBNETS];
    uint8_t n_want = i_dart_discovery_if_scan(want, DART_DISCOVERY_MAX_SUBNETS);
    uint8_t i, same = (uint8_t)(n_want == d->n_ifs);
    if (same)
        for (i = 0; i < n_want; i++)
            if (want[i].addr != d->ifs[i].addr || want[i].mask != d->ifs[i].mask){ same = 0; break; }
    if (!same) i_dart_discovery_if_apply(d, want, n_want);
}

int dart_discovery_make_uuid4(uint8_t out[16]){
    if (!i_dart_plat_random(out, 16)) return 0;
    out[6] = (uint8_t)((out[6] & 0x0Fu) | 0x40u);  /* version 4 */
    out[8] = (uint8_t)((out[8] & 0x3Fu) | 0x80u);  /* variant 10x */
    return 1;
}

void i_dart_discovery_auto_uuid(uint8_t out[16]){
    char host[80]; uint64_t seed; size_t hostname_len;
    if (dart_discovery_make_uuid4(out)) return;

    /* no CSPRNG: hostname, pid and clock */
    hostname_len = i_dart_plat_hostname(host, sizeof host);
    seed = (i_dart_plat_pid() << 32) ^ i_dart_plat_now_us();
    dart_discovery_make_uuid(out, dart_bytes(host, hostname_len), seed);
}

uint8_t dart_discovery_default_name(char *out, size_t cap, const char *want){
    static const char hex[] = "0123456789abcdef";
    uint32_t r; size_t i, max;
    if (!out || cap == 0) return 0;
    max = cap - 1;
    if (max > DART_DISCOVERY_NAME_MAX) max = DART_DISCOVERY_NAME_MAX;
    if (want && *want){                              /* the caller's name, clamped */
        for (i = 0; i < max && want[i]; i++) out[i] = want[i];
        out[i] = '\0';
        return (uint8_t)i;
    }
    if (max < 13){ out[0] = '\0'; return 0; }        /* no room for "node-XXXXXXXX" */
    if (!i_dart_plat_random(&r, sizeof r)) r = (uint32_t)i_dart_plat_pid();
    memcpy(out, "node-", 5);                          /* 8 random hex digits, pid fallback */
    for (i = 0; i < 8; i++) out[5+i] = hex[(r >> ((7-i)*4)) & 0xF];
    out[13] = '\0';
    return 13;
}

/* The one arena layout: the struct, the wire scratch, the peer view, then the core. */
typedef struct {
    DartDiscovery *d;
    uint8_t *rxbuf, *txbuf, *peer_view, *core;
    size_t   wire_max, core_bytes;
} i_DartRtBlocks;
static void i_dart_discovery_rt_layout(i_DartBump *b, const DartDiscoveryCoreConfig *c, i_DartRtBlocks *o){
    uint16_t max_peers = c->max_peers ? c->max_peers : 32u;
    o->wire_max   = dart_discovery_wire_size(c->meta_cap);
    o->d     = (DartDiscovery*)i_dart_bump_take(b, sizeof(struct DartDiscovery), 16);
    o->rxbuf = (uint8_t*)i_dart_bump_take(b, o->wire_max, 16);
    o->txbuf = (uint8_t*)i_dart_bump_take(b, o->wire_max, 16);
    o->peer_view = (uint8_t*)i_dart_bump_take(b, (size_t)max_peers * sizeof(DartDiscoveryPeer), 16);
    o->core_bytes = dart_discovery_required_memory(c);
    o->core  = (uint8_t*)i_dart_bump_take(b, o->core_bytes, 16);
}

size_t dart_discovery_placement_memory(const DartDiscoveryNetConfig *cfg){
    DartDiscoveryCoreConfig c; i_DartBump b; i_DartRtBlocks blk;
    if (!cfg) return 0;
    c = cfg->discovery;
    dart_discovery_config_defaults(&c);
    memset(&b, 0, sizeof b);
    i_dart_discovery_rt_layout(&b, &c, &blk);
    return b.offset + 16u;     /* slack to align the caller's mem up to base */
}

/* Process globals with no lock, read right after a NULL return. The node builds its
   DART_ERROR from them. */
static DartDiscoveryPlaceError g_place_error = DART_DISCOVERY_OK;
static int                     g_place_os_error = 0;
static DartDiscovery *i_dart_discovery_fail(DartDiscoveryPlaceError e, int os_error){
    g_place_error = e; g_place_os_error = os_error;
    return NULL;
}
DartDiscoveryPlaceError dart_discovery_last_error(void){ return g_place_error; }
int                     dart_discovery_last_os_error(void){ return g_place_os_error; }

DartDiscovery *dart_discovery_place(void *mem, size_t cap, const DartDiscoveryNetConfig *cfg){
    DartDiscoveryNetConfig c;
    DartDiscovery *d;
    uint8_t *base, *core_mem;
    size_t need;
    i_DartRtBlocks blk;
    i_DartSock fd;
    int allzero = 1, i;
    uint8_t ttl, n_want, joined;
    uint32_t group_naddr;
    i_DartIface want[DART_DISCOVERY_MAX_SUBNETS];
    const char *group;

    if (!mem || !cfg) return NULL;
    g_place_error = DART_DISCOVERY_OK; g_place_os_error = 0;
    c = *cfg;
    dart_discovery_config_defaults(&c.discovery);
    /* a node with no multicast asks whoever hears it to announce it onward */
    if (c.unicast_only) c.discovery.relay_me = 1;
    group     = c.group     ? c.group     : "239.255.0.7";
    if (c.discovery_port == 0)    c.discovery_port  = 7400;
    ttl  = c.ttl ? c.ttl : 1;

    need = dart_discovery_placement_memory(&c);
    if (cap < need) return i_dart_discovery_fail(DART_DISCOVERY_E_MEMORY, 0);

    base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    {   i_DartBump b; memset(&b, 0, sizeof b);
        b.base = base; b.cap = cap - (size_t)(base - (uint8_t*)mem);
        i_dart_discovery_rt_layout(&b, &c.discovery, &blk); }
    d = blk.d;
    d->wire_max  = (uint32_t)blk.wire_max;
    d->rxbuf     = blk.rxbuf;
    d->txbuf     = blk.txbuf;
    d->peer_view = (DartDiscoveryPeer*)blk.peer_view;
    memset(&d->pool, 0, sizeof d->pool); /* the caller owns mem, the open path overwrites this */
    core_mem     = blk.core;

    /* auto generate a uuid if the caller left it zero */
    for (i=0;i<16;i++) if (c.discovery.uuid[i]) { allzero = 0; break; }

    if (!i_dart_plat_startup()) return i_dart_discovery_fail(DART_DISCOVERY_E_PLATFORM, 0);
    if (allzero) i_dart_discovery_auto_uuid(c.discovery.uuid);

    d->core = dart_discovery_init(core_mem, cap - (size_t)(core_mem - (uint8_t*)mem), &c.discovery);
    if (!d->core){ i_dart_plat_cleanup(); return i_dart_discovery_fail(DART_DISCOVERY_E_MEMORY, 0); }

    fd = i_dart_plat_udp_open();
    if (fd == DART_SOCK_BAD){ int e=i_dart_plat_last_socket_error(); i_dart_plat_cleanup(); return i_dart_discovery_fail(DART_DISCOVERY_E_SOCKET, e); }
    if (!i_dart_plat_bind(fd, 0, c.discovery_port, 1)){ int e=i_dart_plat_last_socket_error(); i_dart_plat_close(fd); i_dart_plat_cleanup(); return i_dart_discovery_fail(DART_DISCOVERY_E_BIND, e); }

    /* Join and announce on every interface. A pinned interface means exactly that one. */
    group_naddr = i_dart_plat_parse_ip(group);
    d->fd = fd;
    d->tx_fd = fd;
    d->group_naddr = group_naddr;
    d->n_ifs = 0;
    d->unicast_only = c.unicast_only ? 1u : 0u;
    d->pinned = c.multicast_interface ? 1u : 0u;
    if (d->pinned){
        want[0].addr = i_dart_plat_parse_ip(c.multicast_interface);
        want[0].mask = i_dart_discovery_if_mask_of(want[0].addr);   /* 0 if it names no real one */
        n_want = 1;
    } else n_want = i_dart_discovery_if_scan(want, DART_DISCOVERY_MAX_SUBNETS);
    joined = i_dart_discovery_if_apply(d, want, n_want);
    if (!d->unicast_only){
        if (!joined && !(n_want == 1 && want[0].addr == 0)){
            i_DartIface any; any.addr = 0; any.mask = 0;      /* let the OS choose */
            joined = i_dart_discovery_if_apply(d, &any, 1);
        }
        if (!joined){
            int e=i_dart_plat_last_socket_error();
            i_dart_plat_close(fd); i_dart_plat_cleanup(); return i_dart_discovery_fail(DART_DISCOVERY_E_MCAST_JOIN, e);
        }
        i_dart_plat_mcast_ttl(fd, ttl);
        /* loop stays on for several instances per host, the uuid filter drops the echoes */
        i_dart_plat_mcast_loop(fd, 1);
    }   /* unicast_only sets no group option, so a stack with no multicast opens */
    i_dart_plat_set_nonblock(fd);   /* the drain's last recv must return would block */
    i_dart_plat_suppress_connreset(fd);   /* a dead peer's ICMP bounce must not disrupt RX */

    d->unicast_fd = DART_SOCK_BAD;
    d->if_scan_us = i_dart_plat_now_us() + DART_DISCOVERY_IF_SCAN_US;
    d->discovery_port = c.discovery_port;
    d->max_peers = c.discovery.max_peers;
    d->n_seeds = 0;
    if (c.seeds){
        uint16_t k, seed_count = c.n_seeds;
        if (seed_count > DART_DISCOVERY_MAX_SEEDS) seed_count = DART_DISCOVERY_MAX_SEEDS;
        for (k=0;k<seed_count;k++) d->seeds[k] = c.seeds[k];
        d->n_seeds = seed_count;
    }

    /* No locator (a discovery only instance): bind an own unicast RX port and advertise it,
       so a same host reply reaches this process, not the shared port's arbitrary owner. */
    if (c.discovery.data_port == 0){
        i_DartSock uc = i_dart_plat_udp_open();
        if (uc != DART_SOCK_BAD){
            uint16_t uport = i_dart_plat_bind(uc, 0, 0, 0) ? i_dart_plat_local_port(uc) : 0;
            if (uport){
                i_dart_plat_set_nonblock(uc);
                i_dart_plat_suppress_connreset(uc);   /* same as above */
                d->unicast_fd = uc;
                d->tx_fd = uc;   /* a reply to the arrival source then reaches this process */
                dart_discovery_set_data_port(d->core, uport);
            } else i_dart_plat_close(uc);
        }
    }
    return d;
}

/* The defaults first constructor. No automatic growth, a full peer table refuses. */
DartDiscovery *dart_discovery_open(DartAllocator *alloc, const char *name, const DartDiscoveryConfig *cfg){
    DartDiscoveryNetConfig nc; DartDiscoveryConfig o; DartDiscovery *d;
    char namebuf[DART_DISCOVERY_NAME_MAX + 1]; uint8_t namelen;
    void *block; size_t need; DartAllocator pool;
    if (!alloc) return NULL;
    memset(&o, 0, sizeof o); if (cfg) o = *cfg;
    namelen = dart_discovery_default_name(namebuf, sizeof namebuf, name);   /* auto if NULL */

    memset(&nc, 0, sizeof nc);
    nc.discovery.domain_id     = o.domain;
    nc.discovery.max_peers     = o.max_peers;          /* 0 = default */
    nc.discovery.meta_cap = o.meta_cap;
    nc.discovery.peer_user_bytes = o.peer_user_bytes;
    nc.discovery.meta          = o.meta;
    nc.discovery.on_event      = o.on_event;
    nc.discovery.user          = o.user;
    nc.discovery.name          = dart_string(namebuf, namelen);   /* the core copies it at init */
    nc.group               = o.discovery_group;
    nc.discovery_port      = o.discovery_port;
    nc.ttl                 = o.multicast_ttl;
    nc.multicast_interface = o.multicast_interface;
    nc.seeds               = o.seed_peers;
    nc.n_seeds             = o.n_seed_peers;
    nc.unicast_only        = o.unicast_only;

    need = dart_discovery_placement_memory(&nc);
    pool = *alloc;                                 /* copied, the caller's may be a temporary */
    block = dart_allocator_alloc(&pool, NULL, need);
    if (!block) return i_dart_discovery_fail(DART_DISCOVERY_E_MEMORY, 0);
    d = dart_discovery_place(block, need, &nc);
    if (!d){ DartAllocator p = pool; dart_allocator_reset(&p); return NULL; }
    d->pool = pool;                                /* the block's pool, close resets it */
    return d;
}

/* The struct copy keeps the socket, group and seeds. The core is migrated and self_meta
 * re pointed. The caller frees the old block after. */
DartDiscovery *dart_discovery_migrate(DartDiscovery *old, void *new_mem, size_t new_cap,
        uint16_t new_max_peers, uint16_t new_meta_cap, const uint8_t *self_meta, void *peer_cb_user){
    DartDiscoveryCoreConfig dc; i_DartRtBlocks blk; i_DartBump b; DartDiscovery *d;
    DartDiscoveryState *nc; uint8_t *base; size_t need;
    if (!old) return NULL;
    dc = old->core->cfg; dc.max_peers = new_max_peers; dc.meta_cap = new_meta_cap;
    {   i_DartBump mb; memset(&mb,0,sizeof mb); i_dart_discovery_rt_layout(&mb, &dc, &blk); need = mb.offset + 16u; }
    if (new_cap < need) return NULL;
    base = (uint8_t*)(((uintptr_t)new_mem + 15u) & ~(uintptr_t)15u);
    memset(&b,0,sizeof b); b.base = base; b.cap = new_cap - (size_t)(base - (uint8_t*)new_mem);
    i_dart_discovery_rt_layout(&b, &dc, &blk);
    d = blk.d;
    *d = *old;                          /* fd, group_naddr, discovery_port, seeds, n_seeds, pool */
    d->wire_max = (uint32_t)blk.wire_max;
    d->rxbuf = blk.rxbuf; d->txbuf = blk.txbuf;
    d->peer_view = (DartDiscoveryPeer*)blk.peer_view;
    d->max_peers = new_max_peers;
    nc = dart_discovery_core_migrate(old->core, blk.core,
             new_cap - (size_t)(blk.core - (uint8_t*)new_mem), new_max_peers, new_meta_cap,
             self_meta, peer_cb_user);
    if (!nc) return NULL;                /* old left intact, the caller frees the new block */
    d->core = nc;
    return d;
}

/* Drains to empty, capped by the burst guard. One recv per poll would let a slow poller's
 * backlog keep a dead peer alive. */
#define DART_DISCOVERY_RX_BURST 2048
static int i_dart_discovery_rt_drain(DartDiscovery *d, i_DartSock fd, DartDiscoveryVia via){
    int got = 0, guard;
    for (guard = 0; guard < DART_DISCOVERY_RX_BURST; guard++){
        DartDiscoveryAddr src;
        uint8_t src_ip[4]; uint16_t src_port = 0;
        int n = i_dart_plat_recv(fd, d->rxbuf, d->wire_max, src_ip, &src_port);
        if (n < 0){ if (i_dart_plat_would_block()) break; continue; }  /* empty or transient */
        if (n > 0){
            memset(&src, 0, sizeof src);
            memcpy(src.ip, src_ip, 4); src.ip_len = 4; src.port = src_port;
            dart_discovery_on_datagram(d->core, &src, via, dart_bytes(d->rxbuf, (size_t)n), i_dart_plat_now_us());
            got = 1;
        }
    }
    return got;
}

/* The poll body without the wait. The node folds discovery into its one wait and calls
 * this every pass. */
int dart_discovery_service(DartDiscovery *d, int fd_readable, int unicast_readable){
    DartDiscoveryAddr to;
    uint64_t now;
    int got = 0, exact; size_t n_bytes;
    if (!d) return 0;
    if (fd_readable) got |= i_dart_discovery_rt_drain(d, d->fd, DART_DISCOVERY_VIA_DISCOVERY);
    /* our own unicast port is the port we advertise, so it is the DATA channel */
    if (unicast_readable && d->unicast_fd != DART_SOCK_BAD)
        got |= i_dart_discovery_rt_drain(d, d->unicast_fd, DART_DISCOVERY_VIA_DATA);

    now = i_dart_plat_now_us();
    if (!d->pinned && now >= d->if_scan_us){    /* pick up an interface that came up since open */
        d->if_scan_us = now + DART_DISCOVERY_IF_SCAN_US;
        i_dart_discovery_if_refresh(d);
    }

    n_bytes = dart_discovery_update(d->core, now, d->txbuf, d->wire_max);
    if (n_bytes) i_dart_discovery_tx(d, d->txbuf, n_bytes, NULL);

    /* targeted unicast: replies to soliciters and re fetch requests */
    while ((n_bytes = dart_discovery_poll_targeted(d->core, d->txbuf, d->wire_max, &to, &exact)) != 0)
        i_dart_discovery_tx_peer(d, d->txbuf, n_bytes, &to, exact ? 2 : 1);

    /* relay: proxied announces for peers that cannot multicast, out of every path. The
       origin itself is skipped, bytes 8 to 23 carry its uuid. */
    while ((n_bytes = dart_discovery_poll_relay(d->core, d->txbuf, d->wire_max)) != 0)
        i_dart_discovery_tx(d, d->txbuf, n_bytes, d->txbuf + 8);

    /* introductions: proxied announces of the peers we hear directly, unicast to one relay
       me peer, which behind a NAT must always speak first */
    while ((n_bytes = dart_discovery_poll_introduce(d->core, d->txbuf, d->wire_max, &to, &exact)) != 0)
        i_dart_discovery_tx_peer(d, d->txbuf, n_bytes, &to, exact ? 2 : 1);
    return got;
}

int dart_discovery_poll(DartDiscovery *d, int timeout_ms){
    i_DartPollfd pfd[2];
    int nfds = 1;
    memset(pfd, 0, sizeof pfd);
    pfd[0].fd = d->fd; pfd[0].events = DART_POLLIN;
    if (d->unicast_fd != DART_SOCK_BAD){ pfd[1].fd = d->unicast_fd; pfd[1].events = DART_POLLIN; nfds = 2; }
    if (i_dart_plat_poll(pfd, nfds, timeout_ms) < 0) return -1;
    return dart_discovery_service(d, (pfd[0].revents & DART_POLLIN) != 0,
                                  nfds == 2 && (pfd[1].revents & DART_POLLIN) != 0);
}

int dart_discovery_gather(DartDiscovery *d, int quiet_ms, int timeout_ms){
    uint64_t start, last_change, last_solicit = 0;
    uint16_t count;
    if (!d) return 0;
    start = i_dart_plat_now_us(); last_change = start;
    count = dart_discovery_peer_count(d->core);
    for (;;){
        uint64_t now = i_dart_plat_now_us(); uint16_t c;
        if (now - last_solicit >= 250000u){     /* re solicit 4 times a second */
            dart_discovery_solicit(d->core); last_solicit = now;
        }
        dart_discovery_poll(d, 10);              /* sends the solicit, takes in replies */
        now = i_dart_plat_now_us();
        c = dart_discovery_peer_count(d->core);
        if (c > count){ count = c; last_change = now; }   /* grew: keep waiting */
        if (count > 0 && now - last_change >= (uint64_t)quiet_ms*1000u) break;
        if (now - start >= (uint64_t)timeout_ms*1000u) break;
    }
    return (int)count;
}

DartDiscoveryState *dart_discovery_state(DartDiscovery *d){ return d ? d->core : NULL; }

const DartDiscoveryPeer *dart_discovery_peers(DartDiscovery *d, uint16_t *count){
    uint16_t i, n = 0;
    if (!d){ if (count) *count = 0; return NULL; }
    for (i=0;i<d->max_peers;i++)
        if (dart_discovery_peer_at(d->core, i, &d->peer_view[n])) n++;   /* pack used peers */
    if (count) *count = n;
    return d->peer_view;
}

void dart_discovery_close(DartDiscovery *d, int send_bye){
    DartAllocator pool;
    if (!d) return;
    if (send_bye){
        size_t n_bytes = dart_discovery_leave(d->core, d->txbuf, d->wire_max);
        int k;
        for (k = 0; n_bytes && k < DART_DISCOVERY_BYE_SENDS; k++) i_dart_discovery_tx(d, d->txbuf, n_bytes, NULL);
    }
    dart_discovery_destroy(d->core);   /* may mutate the pool, so it runs before the copy out */
    i_dart_plat_close(d->fd);
    if (d->unicast_fd != DART_SOCK_BAD) i_dart_plat_close(d->unicast_fd);
    i_dart_plat_cleanup();
    pool = d->pool;                /* copy out last: the reset frees the block holding d */
    dart_allocator_reset(&pool);   /* the place path has an empty pool, a no op */
}
