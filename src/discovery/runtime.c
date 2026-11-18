/* The discovery runtime. Its sockets are the node's net, the OS's or a tunnel's. */

#include "runtime.h"
#include "../platform/core.h"
#include "../common/arena.h"
#include "../common/bytes.h"
#include <string.h>

#define RANT_DISCOVERY_BYE_SENDS 3     /* one shot UDP, so resend. Receivers dedup by uuid */
#define RANT_DISCOVERY_IF_SCAN_US 3000000u     /* re enumerate interfaces this often in auto mode */

struct i_RantDiscovery {
    i_RantDiscoveryState *core;
    i_RantTunnel           *tunnel;        /* the sockets' owner, NULL for the OS */
    i_RantSock              fd;
    i_RantSock              tx_fd;         /* unicast TX leaves here: the node's data socket, else fd */
    i_RantIface            ifs[RANT_DISCOVERY_MAX_SUBNETS];    /* joined and announced out of */
    uint8_t                n_ifs;          /* 0 = none usable, the OS picks */
    uint8_t                pinned;         /* explicit multicast_interface, never rescanned */
    uint64_t               if_scan_us;     /* next interface re enumeration */
    uint32_t               group_naddr;   /* network order */
    uint16_t               discovery_port;
    uint16_t               max_peers;
    uint32_t               wire_max;    /* scratch buffer size */
    uint8_t               *rxbuf;       /* arena, wire_max */
    uint8_t               *txbuf;       /* arena, wire_max */
    i_RantDiscoveryPeerView     *peer_view;   /* arena, max_peers: the snapshot for i_rant_discovery_peers */
    RantAddr    seeds[RANT_DISCOVERY_MAX_SEEDS];
    uint16_t             n_seeds;
};

static void i_rant_discovery_tx1(i_RantDiscovery *d, const uint8_t *out, size_t n_bytes,
                          const uint8_t ip[4], uint16_t port){
    i_rant_net_send(d->tunnel, d->tx_fd, out, n_bytes, ip, port);
}

/* One copy: the data port once the blob named one, else the discovery port. A peer whose
 * data port is unreachable should look dead. */
static void i_rant_discovery_tx_to(i_RantDiscovery *d, const uint8_t *out, size_t n_bytes,
                          const RantAddr *addr){
    if (addr->ip_len != 4) return;
    i_rant_discovery_tx1(d, out, n_bytes, addr->ip, addr->port ? addr->port : d->discovery_port);
}

/* Out of every interface, so each copy carries the source correct for its path. A failed
 * sendto on one adapter does not stop the others. */
static void i_rant_discovery_tx_group(i_RantDiscovery *d, const uint8_t *out, size_t n_bytes){
    uint8_t group_ip[4], i;
    i_rant_plat_naddr_to_ip4(d->group_naddr, group_ip);
    if (!d->n_ifs){ i_rant_net_send(d->tunnel, d->fd, out, n_bytes, group_ip, d->discovery_port); return; }
    for (i = 0; i < d->n_ifs; i++){
        i_rant_net_mcast_setif(d->tunnel, d->fd, d->ifs[i].addr);
        i_rant_net_send(d->tunnel, d->fd, out, n_bytes, group_ip, d->discovery_port);
    }
}

/* The group, then copies by unicast: every seed marked UNI, and each record the core says
 * takes one, marked as it says. out is left as built, so a resend reaches the group unmarked. */
static void i_rant_discovery_tx(i_RantDiscovery *d, uint8_t *out, size_t n_bytes){
    uint16_t s, i, n_recs = i_rant_discovery_record_cap(d->core), discovery_port = d->discovery_port;
    RantAddr addr; int uni;
    i_rant_discovery_tx_group(d, out, n_bytes);
    i_rant_discovery_mark_copy(out, n_bytes, I_RANT_COPY_UNI);
    for (s=0; s<d->n_seeds; s++){
        const RantAddr *seed = &d->seeds[s];
        if (seed->ip_len != 4) continue;
        i_rant_discovery_tx1(d, out, n_bytes, seed->ip, seed->port ? seed->port : discovery_port);
    }
    for (i=0; i<n_recs; i++)
        if (i_rant_discovery_copy_addr(d->core, i, &addr, &uni)){
            i_rant_discovery_mark_copy(out, n_bytes, uni ? I_RANT_COPY_UNI : I_RANT_COPY_PLAIN);
            i_rant_discovery_tx_to(d, out, n_bytes, &addr);
        }
    i_rant_discovery_mark_copy(out, n_bytes, I_RANT_COPY_GROUP);
}

void i_rant_discovery_feed(i_RantDiscovery *d, const RantAddr *src, RantBytes datagram){
    if (!d) return;
    i_rant_discovery_on_datagram(d->core, src, datagram, i_rant_plat_now_us());
}

void i_rant_discovery_set_tx_fd(i_RantDiscovery *d, i_RantSock fd){
    if (d) d->tx_fd = (fd == RANT_SOCK_BAD) ? d->fd : fd;
}

void i_rant_discovery_advertise(i_RantDiscovery *d, RantBytes meta){
    if (d) i_rant_discovery_set_meta(d->core, meta, i_rant_plat_now_us());
}

void i_rant_discovery_replay(i_RantDiscovery *d){
    if (d) i_rant_discovery_replay_peers(d->core);
}

i_RantSock i_rant_discovery_fd(i_RantDiscovery *d){ return d ? d->fd : RANT_SOCK_BAD; }

/* The up, non loopback interfaces. Loopback is the fallback for a host with nothing else
 * up, never a member of the normal set. 0 only when the platform cannot enumerate. */
static uint8_t i_rant_discovery_if_scan(i_RantTunnel *t, i_RantIface *out, uint8_t max){
    int n = i_rant_net_local_ifaces(t, out, (int)max), i;
    uint8_t k = 0;
    for (i = 0; i < n; i++){
        uint8_t ip[4];
        i_rant_plat_naddr_to_ip4(out[i].addr, ip);
        if (!out[i].addr || ip[0] == 127) continue;   /* unspecified or loopback */
        out[k++] = out[i];
    }
    if (!k){                        /* nothing up, or a loopback only host */
        out[0].addr = i_rant_plat_ipv4(127u, 0u, 0u, 1u);
        out[0].mask = i_rant_plat_ipv4(255u, 0u, 0u, 0u);
        k = 1;
    }
    return k;
}

/* The netmask the host reports for addr, 0 if none. A pinned interface still ranks. */
static uint32_t i_rant_discovery_if_mask_of(i_RantTunnel *t, uint32_t addr){
    i_RantIface all[RANT_DISCOVERY_MAX_SUBNETS];
    int n = i_rant_net_local_ifaces(t, all, RANT_DISCOVERY_MAX_SUBNETS), i;
    for (i = 0; i < n; i++) if (all[i].addr == addr) return all[i].mask;
    return 0;
}

static int i_rant_discovery_if_has(const i_RantIface *set, uint8_t n, uint32_t addr){
    uint8_t i;
    for (i = 0; i < n; i++) if (set[i].addr == addr) return 1;
    return 0;
}

/* Leaves what went away, joins what appeared, and hands the core our subnets. A per
 * interface join failure is ordinary, so this returns how many memberships are live. */
static uint8_t i_rant_discovery_if_apply(i_RantDiscovery *d, const i_RantIface *want, uint8_t n_want){
    i_RantDiscoverySubnet nets[RANT_DISCOVERY_MAX_SUBNETS];
    uint8_t i, joined = 0;
    if (n_want > RANT_DISCOVERY_MAX_SUBNETS) n_want = RANT_DISCOVERY_MAX_SUBNETS;
    for (i = 0; i < d->n_ifs; i++)
        if (!i_rant_discovery_if_has(want, n_want, d->ifs[i].addr))
            i_rant_net_mcast_leave(d->tunnel, d->fd, d->group_naddr, d->ifs[i].addr);
    for (i = 0; i < n_want; i++){
        if (i_rant_discovery_if_has(d->ifs, d->n_ifs, want[i].addr)){ joined++; continue; }
        if (i_rant_net_mcast_join(d->tunnel, d->fd, d->group_naddr, want[i].addr)) joined++;
    }
    for (i = 0; i < n_want; i++){
        i_rant_plat_naddr_to_ip4(want[i].addr, nets[i].ip);
        i_rant_plat_naddr_to_ip4(want[i].mask, nets[i].mask);
        d->ifs[i] = want[i];
    }
    d->n_ifs = n_want;
    i_rant_discovery_set_local_subnets(d->core, nets, n_want);
    return joined;
}

/* A link that comes up after open is joined, and one that goes away drops its membership. */
static void i_rant_discovery_if_refresh(i_RantDiscovery *d){
    i_RantIface want[RANT_DISCOVERY_MAX_SUBNETS];
    uint8_t n_want = i_rant_discovery_if_scan(d->tunnel, want, RANT_DISCOVERY_MAX_SUBNETS);
    uint8_t i, same = (uint8_t)(n_want == d->n_ifs);
    if (same)
        for (i = 0; i < n_want; i++)
            if (want[i].addr != d->ifs[i].addr || want[i].mask != d->ifs[i].mask){ same = 0; break; }
    if (!same) i_rant_discovery_if_apply(d, want, n_want);
}

int i_rant_discovery_make_uuid4(uint8_t out[16]){
    if (!i_rant_plat_random(out, 16)) return 0;
    out[6] = (uint8_t)((out[6] & 0x0Fu) | 0x40u);  /* version 4 */
    out[8] = (uint8_t)((out[8] & 0x3Fu) | 0x80u);  /* variant 10x */
    return 1;
}

void i_rant_discovery_auto_uuid(uint8_t out[16]){
    char host[80]; uint64_t seed; size_t hostname_len;
    if (i_rant_discovery_make_uuid4(out)) return;

    /* no CSPRNG: hostname, pid and clock */
    hostname_len = i_rant_plat_hostname(host, sizeof host);
    seed = (i_rant_plat_pid() << 32) ^ i_rant_plat_now_us();
    i_rant_discovery_make_uuid(out, rant_bytes(host, hostname_len), seed);
}

uint8_t i_rant_discovery_default_name(char *out, size_t cap, const char *want){
    uint32_t r; size_t i, max;
    if (!out || cap == 0) return 0;
    max = cap - 1;
    if (max > RANT_NODE_NAME_MAX) max = RANT_NODE_NAME_MAX;
    if (want && *want){                              /* the caller's name, clamped */
        for (i = 0; i < max && want[i]; i++) out[i] = want[i];
        out[i] = '\0';
        return (uint8_t)i;
    }
    if (max < 13){ out[0] = '\0'; return 0; }        /* no room for "node-XXXXXXXX" */
    if (!i_rant_plat_random(&r, sizeof r)) r = (uint32_t)i_rant_plat_pid();
    memcpy(out, "node-", 5);                          /* 8 random hex digits, pid fallback */
    for (i = 0; i < 8; i++) out[5+i] = i_rant_hex(r >> ((7-i)*4));
    out[13] = '\0';
    return 13;
}

/* The one arena layout: the struct, the wire scratch, the peer view, then the core. */
typedef struct {
    i_RantDiscovery *d;
    uint8_t *rxbuf, *txbuf, *peer_view, *core;
    size_t   wire_max, core_bytes;
} i_RantRtBlocks;
static void i_rant_discovery_rt_layout(i_RantBump *b, const i_RantDiscoveryCoreConfig *c, i_RantRtBlocks *o){
    o->wire_max   = i_rant_discovery_wire_size(c->meta_cap);
    o->d     = (i_RantDiscovery*)i_rant_bump_take(b, sizeof(struct i_RantDiscovery), 16);
    o->rxbuf = (uint8_t*)i_rant_bump_take(b, o->wire_max, 16);
    o->txbuf = (uint8_t*)i_rant_bump_take(b, o->wire_max, 16);
    o->peer_view = (uint8_t*)i_rant_bump_take(b, (size_t)c->max_peers * sizeof(i_RantDiscoveryPeerView), 16);
    o->core_bytes = i_rant_discovery_required_memory(c);
    o->core  = (uint8_t*)i_rant_bump_take(b, o->core_bytes, 16);
}

size_t i_rant_discovery_placement_memory(const i_RantDiscoveryCoreConfig *cfg){
    i_RantDiscoveryCoreConfig c; i_RantBump b; i_RantRtBlocks blk;
    if (!cfg) return 0;
    c = *cfg;
    i_rant_discovery_config_defaults(&c);
    memset(&b, 0, sizeof b);
    i_rant_discovery_rt_layout(&b, &c, &blk);
    return b.offset + 16u;     /* slack to align the caller's mem up to base */
}

/* Process globals with no lock, read right after a NULL return. The node builds its
   RANT_ERROR from them. */
static RantErrorKind g_place_error = RANT_E_NONE;
static int                       g_place_os_error = 0;
static i_RantDiscovery *i_rant_discovery_fail(RantErrorKind e, int os_error){
    g_place_error = e; g_place_os_error = os_error;
    return NULL;
}
RantErrorKind i_rant_discovery_last_error(void){ return g_place_error; }
int                       i_rant_discovery_last_os_error(void){ return g_place_os_error; }

i_RantDiscovery *i_rant_discovery_place(void *mem, size_t cap, const i_RantDiscoveryCoreConfig *cfg,
                                    const RantNodeNet *net, i_RantTunnel *tunnel){
    i_RantDiscoveryCoreConfig c;
    uint16_t discovery_port;
    i_RantDiscovery *d;
    uint8_t *base, *core_mem;
    size_t need;
    i_RantRtBlocks blk;
    i_RantSock fd;
    int allzero = 1, i;
    uint8_t ttl, n_want, joined;
    uint32_t group, group_naddr, bind_naddr;
    i_RantIface want[RANT_DISCOVERY_MAX_SUBNETS];

    if (!mem || !cfg || !net) return i_rant_discovery_fail(RANT_E_OOM, 0);
    g_place_error = RANT_E_NONE; g_place_os_error = 0;
    c = *cfg;
    i_rant_discovery_config_defaults(&c);
    group = RANT_DISCOVERY_GROUP_BASE | c.domain_id;     /* one group per domain */
    discovery_port = net->discovery_port ? net->discovery_port : RANT_DISCOVERY_PORT;
    ttl   = net->multicast_ttl ? net->multicast_ttl : 1;

    need = i_rant_discovery_placement_memory(&c);
    if (cap < need) return i_rant_discovery_fail(RANT_E_OOM, 0);

    base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    {   i_RantBump b; memset(&b, 0, sizeof b);
        b.base = base; b.cap = cap - (size_t)(base - (uint8_t*)mem);
        i_rant_discovery_rt_layout(&b, &c, &blk); }
    d = blk.d;
    d->wire_max  = (uint32_t)blk.wire_max;
    d->rxbuf     = blk.rxbuf;
    d->txbuf     = blk.txbuf;
    d->peer_view = (i_RantDiscoveryPeerView*)blk.peer_view;
    core_mem     = blk.core;

    /* auto generate a uuid if the caller left it zero */
    for (i=0;i<16;i++) if (c.uuid[i]) { allzero = 0; break; }

    if (!i_rant_plat_startup()) return i_rant_discovery_fail(RANT_E_PLATFORM, 0);
    if (allzero) i_rant_discovery_auto_uuid(c.uuid);

    d->core = i_rant_discovery_init(core_mem, cap - (size_t)(core_mem - (uint8_t*)mem), &c);
    if (!d->core){ i_rant_plat_cleanup(); return i_rant_discovery_fail(RANT_E_OOM, 0); }

    d->tunnel = tunnel;
    fd = i_rant_net_udp_open(tunnel);
    if (fd == RANT_SOCK_BAD){ int e=i_rant_net_last_error(tunnel); i_rant_plat_cleanup(); return i_rant_discovery_fail(RANT_E_SOCKET, e); }
    /* a node pinned to loopback is single host and listens on no network interface */
    bind_naddr = i_rant_plat_parse_ip(net->multicast_interface);
    bind_naddr = i_rant_plat_is_loopback(bind_naddr) ? i_rant_net_loopback_group_bind(tunnel, bind_naddr) : 0;
    if (!i_rant_net_bind(tunnel, fd, bind_naddr, discovery_port, 1)){ int e=i_rant_net_last_error(tunnel); i_rant_net_close(tunnel, fd); i_rant_plat_cleanup(); return i_rant_discovery_fail(RANT_E_BIND, e); }

    /* Join and announce on every interface. A pinned interface means exactly that one. */
    group_naddr = i_rant_plat_ipv4((uint8_t)(group >> 24), (uint8_t)(group >> 16),
                                   (uint8_t)(group >> 8), (uint8_t)group);
    d->fd = fd;
    d->tx_fd = fd;
    d->group_naddr = group_naddr;
    d->n_ifs = 0;
    d->pinned = net->multicast_interface ? 1u : 0u;
    if (d->pinned){
        want[0].addr = i_rant_plat_parse_ip(net->multicast_interface);
        want[0].mask = i_rant_discovery_if_mask_of(tunnel, want[0].addr);     /* 0 if it names no real one */
        n_want = 1;
    } else n_want = i_rant_discovery_if_scan(tunnel, want, RANT_DISCOVERY_MAX_SUBNETS);
    joined = i_rant_discovery_if_apply(d, want, n_want);
    if (!joined && !(n_want == 1 && want[0].addr == 0)){
        i_RantIface any; any.addr = 0; any.mask = 0;        /* let the OS choose */
        joined = i_rant_discovery_if_apply(d, &any, 1);
    }
    if (!joined){
        int e=i_rant_net_last_error(tunnel);
        i_rant_net_close(tunnel, fd); i_rant_plat_cleanup(); return i_rant_discovery_fail(RANT_E_MCAST_JOIN, e);
    }
    i_rant_net_mcast_ttl(tunnel, fd, ttl);
    /* loop stays on for several instances per host, the uuid filter drops the echoes */
    i_rant_net_mcast_loop(tunnel, fd, 1);
    i_rant_net_set_nonblock(tunnel, fd);     /* the drain's last recv must return would block */
    i_rant_net_suppress_connreset(tunnel, fd);     /* a dead peer's ICMP bounce must not disrupt RX */

    d->if_scan_us = i_rant_plat_now_us() + RANT_DISCOVERY_IF_SCAN_US;
    d->discovery_port = discovery_port;
    d->max_peers = c.max_peers;
    d->n_seeds = 0;
    if (net->seed_peers){
        uint16_t k, seed_count = net->n_seed_peers;
        if (seed_count > RANT_DISCOVERY_MAX_SEEDS) seed_count = RANT_DISCOVERY_MAX_SEEDS;
        for (k=0;k<seed_count;k++) d->seeds[k] = net->seed_peers[k];
        d->n_seeds = seed_count;
    }
    return d;
}

/* The struct copy keeps the socket, group and seeds. The core is migrated and self_meta
 * re pointed. The caller frees the old block after. */
i_RantDiscovery *i_rant_discovery_migrate(i_RantDiscovery *old, void *new_mem, size_t new_cap,
        uint16_t new_max_peers, uint16_t new_meta_cap, const uint8_t *self_meta, void *peer_cb_user){
    i_RantDiscoveryCoreConfig dc; i_RantRtBlocks blk; i_RantBump b; i_RantDiscovery *d;
    i_RantDiscoveryState *nc; uint8_t *base; size_t need;
    if (!old) return NULL;
    dc = old->core->cfg; dc.max_peers = new_max_peers; dc.meta_cap = new_meta_cap;
    {   i_RantBump mb; memset(&mb,0,sizeof mb); i_rant_discovery_rt_layout(&mb, &dc, &blk); need = mb.offset + 16u; }
    if (new_cap < need) return NULL;
    base = (uint8_t*)(((uintptr_t)new_mem + 15u) & ~(uintptr_t)15u);
    memset(&b,0,sizeof b); b.base = base; b.cap = new_cap - (size_t)(base - (uint8_t*)new_mem);
    i_rant_discovery_rt_layout(&b, &dc, &blk);
    d = blk.d;
    *d = *old;                          /* fd, group_naddr, discovery_port, seeds, n_seeds */
    d->wire_max = (uint32_t)blk.wire_max;
    d->rxbuf = blk.rxbuf; d->txbuf = blk.txbuf;
    d->peer_view = (i_RantDiscoveryPeerView*)blk.peer_view;
    d->max_peers = new_max_peers;
    nc = i_rant_discovery_core_migrate(old->core, blk.core,
             new_cap - (size_t)(blk.core - (uint8_t*)new_mem), new_max_peers, new_meta_cap,
             self_meta, peer_cb_user);
    if (!nc) return NULL;                /* old left intact, the caller frees the new block */
    d->core = nc;
    return d;
}

/* Drains to empty, capped by the burst guard. One recv per poll would let a slow poller's
 * backlog keep a dead peer alive. */
#define RANT_DISCOVERY_RX_BURST 2048
static int i_rant_discovery_rt_drain(i_RantDiscovery *d, i_RantSock fd){
    int got = 0, guard;
    for (guard = 0; guard < RANT_DISCOVERY_RX_BURST; guard++){
        RantAddr src;
        uint8_t src_ip[4]; uint16_t src_port = 0;
        int n = i_rant_net_recv(d->tunnel, fd, d->rxbuf, d->wire_max, src_ip, &src_port);
        if (n < 0){ if (i_rant_net_would_block(d->tunnel)) break; continue; }    /* empty or transient */
        if (n > 0){
            memset(&src, 0, sizeof src);
            memcpy(src.ip, src_ip, 4); src.ip_len = 4; src.port = src_port;
            i_rant_discovery_on_datagram(d->core, &src, rant_bytes(d->rxbuf, (size_t)n), i_rant_plat_now_us());
            got = 1;
        }
    }
    return got;
}

/* One service pass with no wait. The node folds discovery into its one wait and calls
 * this every pass. */
int i_rant_discovery_service(i_RantDiscovery *d, int fd_readable){
    RantAddr to;
    uint64_t now;
    int got = 0; size_t n_bytes;
    if (!d) return 0;
    if (fd_readable) got |= i_rant_discovery_rt_drain(d, d->fd);

    now = i_rant_plat_now_us();
    if (!d->pinned && now >= d->if_scan_us){    /* pick up an interface that came up since open */
        d->if_scan_us = now + RANT_DISCOVERY_IF_SCAN_US;
        i_rant_discovery_if_refresh(d);
    }

    n_bytes = i_rant_discovery_update(d->core, now, d->txbuf, d->wire_max);
    if (n_bytes) i_rant_discovery_tx(d, d->txbuf, n_bytes);

    /* the owed targeted sends: replies and fetches */
    while ((n_bytes = i_rant_discovery_poll_targeted(d->core, now, d->txbuf, d->wire_max, &to)) != 0)
        i_rant_discovery_tx_to(d, d->txbuf, n_bytes, &to);
    return got;
}

i_RantDiscoveryState *i_rant_discovery_state(i_RantDiscovery *d){ return d ? d->core : NULL; }

const i_RantDiscoveryPeerView *i_rant_discovery_peers(i_RantDiscovery *d, uint16_t *count){
    uint16_t i, n = 0;
    if (!d){ if (count) *count = 0; return NULL; }
    for (i=0;i<d->max_peers;i++)
        if (i_rant_discovery_peer_at(d->core, i, &d->peer_view[n])) n++;     /* pack used peers */
    if (count) *count = n;
    return d->peer_view;
}

void i_rant_discovery_close(i_RantDiscovery *d, int send_bye){
    if (!d) return;
    if (send_bye){
        size_t n_bytes = i_rant_discovery_leave(d->core, d->txbuf, d->wire_max);
        int k;
        for (k = 0; n_bytes && k < RANT_DISCOVERY_BYE_SENDS; k++) i_rant_discovery_tx(d, d->txbuf, n_bytes);
    }
    i_rant_discovery_destroy(d->core);
    i_rant_net_close(d->tunnel, d->fd);
    i_rant_plat_cleanup();
}
