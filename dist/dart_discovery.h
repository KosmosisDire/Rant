/* GENERATED single-header build. DO NOT EDIT.
 * DART = Discovery And Realtime Transport. Amalgamated from src/ by
 * tools/pack.c. Edit the split sources in src/ and re-run pack to regenerate.
 * See the flag scheme at the top of tools/pack.c.
 */
#if defined(DART_DISCOVERY_IMPLEMENTATION) && !defined(DART_DISCOVERY_SANS_IO) && !defined(_WIN32)
  #ifndef _POSIX_C_SOURCE
  #define _POSIX_C_SOURCE 200809L
  #endif
  #ifndef _DEFAULT_SOURCE
  #define _DEFAULT_SOURCE 1
  #endif
#endif

/* ===== dart_discovery.h ===== */
/* sans-IO peer-discovery core (no socket, clock, or heap).
 * Feed it datagrams plus now_us; it returns datagrams to send and fires peer
 * up/down callbacks. C99. For an IO-owning layer, see dart_discovery_rt.h. */
#ifndef DART_DISCOVERY_H
#define DART_DISCOVERY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DART_DISCOVERY_PROTO_VERSION
#define DART_DISCOVERY_PROTO_VERSION 2     /* v2: announces carry opaque meta */
#endif

#define DART_DISCOVERY_WIRE_MAX 128
#define DART_DISCOVERY_META_MAX 64   /* max app payload bytes per announce */

typedef struct {
    uint8_t  ip[16];   /* network-order bytes */
    uint8_t  ip_len;   /* 4 = IPv4, 16 = IPv6 */
    uint16_t port;     /* data port, host order */
} dart_discovery_addr;

/* peer_up: peer reachable at addr; re-fires when a known peer's addr or meta
 * changes. peer_down: gone (timeout or BYE). peer_id is a local handle, stable
 * only while the peer stays alive; one that times out and returns gets a new
 * handle even with the same uuid. meta is the peer's opaque payload (NULL if
 * none), valid only for the call. */
typedef void (*dart_discovery_peer_up_fn)  (void *user, uint32_t peer_id, const dart_discovery_addr *addr,
                                   const uint8_t *meta, uint8_t meta_len);
typedef void (*dart_discovery_peer_down_fn)(void *user, uint32_t peer_id);

typedef struct {
    uint8_t  uuid[16];      /* unique per process instance (regen each boot) */
    uint16_t domain_id;     /* logical-network selector */
    uint16_t data_port;     /* unicast port we advertise */
    uint8_t  self_ip[16];   /* optional advertised IP; len 0 => use src addr */
    uint8_t  self_ip_len;   /* 0, 4, or 16 */
    uint32_t announce_us;   /* re-announce interval */
    uint32_t timeout_us;    /* drop peer after this much silence */
    uint16_t max_peers;     /* table capacity */
    const uint8_t *meta;    /* opaque payload appended to every announce;
                               must stay valid for the state's lifetime */
    uint8_t  meta_len;      /* <= DART_DISCOVERY_META_MAX */
    dart_discovery_peer_up_fn   on_peer_up;
    dart_discovery_peer_down_fn on_peer_down;
    void *user;
} dart_discovery_config;

typedef struct dart_discovery_state dart_discovery_state;

size_t       dart_discovery_required_memory(const dart_discovery_config *cfg);
dart_discovery_state *dart_discovery_init(void *mem, size_t mem_size, const dart_discovery_config *cfg);
void         dart_discovery_on_datagram(dart_discovery_state *st, const uint8_t *src_ip, uint8_t src_ip_len,
                               const void *dg, size_t len, uint64_t now_us);
size_t       dart_discovery_update(dart_discovery_state *st, uint64_t now_us, void *out, size_t cap);
size_t       dart_discovery_leave(dart_discovery_state *st, void *out, size_t cap);
/* Address of the peer in table slot `slot` (0..max_peers-1); returns 1 and
 * fills *out when the slot holds a live peer. Lets a runtime reinforce
 * announces over unicast so established peerings survive multicast outages
 * (WiFi floods, IGMP snooping pruning); bootstrap still needs the group. */
int          dart_discovery_peer_addr(const dart_discovery_state *st, uint16_t slot,
                             dart_discovery_addr *out);
/* Deterministic UUID from a stable input (e.g. serial/MAC) plus a boot seed,
 * for reproducible identity. RFC 9562 version-8 (custom). NOT cryptographic. */
void         dart_discovery_make_uuid(uint8_t out[16], const uint8_t *stable, size_t stable_len,
                             uint64_t boot_seed);

#ifdef __cplusplus
}
#endif
#endif /* DART_DISCOVERY_H */

#ifndef DART_DISCOVERY_SANS_IO
/* ===== dart_discovery_rt.h ===== */
/* peer-discovery runtime API: UDP multicast, clock, UUID
 * and one-tick loop over the dart_discovery core. On non-MSVC Windows, link
 * -lws2_32 -lbcrypt. */
#ifndef DART_DISCOVERY_RT_H
#define DART_DISCOVERY_RT_H


#ifdef __cplusplus
extern "C" {
#endif

#define DART_DISCOVERY_MAX_SEEDS 4

/* Zero/NULL fields get defaults. Leave disc.uuid all-zero to auto-generate a
 * per-boot random v4 UUID. */
typedef struct {
    dart_discovery_config disc;        /* core config: ids, timing, callbacks */
    const char  *group;       /* multicast group, default "239.255.0.7" */
    uint16_t     disc_port;   /* rendezvous port, default 7400 */
    uint8_t      ttl;         /* multicast TTL, default 1 */
    const char  *mcast_if;    /* interface IP to join/send on; NULL = route probe.
                                 "127.0.0.1" keeps single-host setups off the
                                 network. */
    const dart_discovery_addr *seeds;  /* initial peers: every announce is also
                                 unicast to these, so discovery bootstraps on
                                 networks where multicast is filtered or flaky
                                 (copied at open; max DART_DISCOVERY_MAX_SEEDS) */
    uint16_t     n_seeds;
} dart_discovery_rt_config;

typedef struct dart_discovery_rt dart_discovery_rt;

size_t     dart_discovery_rt_required_memory(const dart_discovery_rt_config *cfg);
/* Opens the socket, joins the group, places core state in mem. NULL on failure. */
dart_discovery_rt  *dart_discovery_rt_open(void *mem, size_t mem_size, const dart_discovery_rt_config *cfg);
/* One loop tick: waits up to timeout_ms for a datagram, feeds RX to the core,
 * pumps timeouts and announcements, sends what's due. Returns 1 if a datagram
 * arrived, 0 if idle, <0 on socket error. */
int        dart_discovery_rt_poll(dart_discovery_rt *rt, int timeout_ms);
/* Optionally multicast a graceful BYE, then close the socket. */
void       dart_discovery_rt_close(dart_discovery_rt *rt, int send_bye);

/* Hand the core a discovery datagram that arrived on some other socket.
 * Unicast announces to known peers target the peer's advertised DATA port
 * (the only per-process address when several processes share the discovery
 * port), so the layer owning the data socket forwards them here. */
void       dart_discovery_rt_feed(dart_discovery_rt *rt, const uint8_t *src_ip, uint8_t src_ip_len,
                          const void *dg, size_t len);

/* Fill out[16] with a random RFC 9562 v4 UUID from the platform CSPRNG.
 * Returns 1 on success, 0 if no entropy source was available. */
int        dart_discovery_make_uuid4(uint8_t out[16]);

/* Egress interface the OS routes to group:port (INADDR_ANY on failure). Exposed
 * so layers above can pin their multicast sockets to the same interface:
 * multihomed hosts otherwise pick different ones per socket and break peer
 * matching by source address. */
uint32_t   dart_discovery_mcast_if_for(uint32_t group_naddr, uint16_t port);

#ifdef __cplusplus
}
#endif
#endif /* DART_DISCOVERY_RT_H */
#endif /* !DART_DISCOVERY_SANS_IO */

#ifdef DART_DISCOVERY_IMPLEMENTATION
/* ===== dart_discovery.c ===== */
/* sans-IO peer-discovery core. See dart_discovery.h. */
#include <string.h>

#define DART_DISCOVERY_HDR_LEN 43
#define DART_DISCOVERY_FLAG_BYE 0x01
#define DART_DISCOVERY_FLAG_REQ 0x02         /* solicit: recipients announce back now */
#define DART_DISCOVERY_SOLICIT_JITTER_US 20000u  /* spread replies so they don't storm */

struct dart_discovery_peer_ {
    uint8_t  used;
    uint8_t  uuid[16];
    uint32_t local_id;
    uint8_t  ip[16];
    uint8_t  ip_len;
    uint16_t port;
    uint64_t last_heard_us;
    uint8_t  meta[DART_DISCOVERY_META_MAX];
    uint8_t  meta_len;
};
typedef struct dart_discovery_peer_ dart_discovery_peer_;

struct dart_discovery_state {
    dart_discovery_config  cfg;
    uint64_t      next_announce_us;
    uint32_t      next_local_id;
    uint8_t       started;
    uint16_t      cap_peers;
    dart_discovery_peer_  *peers;
};

static void     dart_discovery_wr16(uint8_t *p, uint16_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static uint16_t dart_discovery_rd16(const uint8_t *p){ return (uint16_t)(p[0] | ((uint16_t)p[1]<<8)); }

static uint32_t dart_discovery_fnv(const uint8_t *d, size_t n){
    uint32_t h = 2166136261u; size_t i;
    for (i=0;i<n;i++){ h ^= d[i]; h *= 16777619u; }
    return h;
}

void dart_discovery_make_uuid(uint8_t out[16], const uint8_t *stable, size_t n, uint64_t seed){
    uint64_t x = 1469598103934665603ull; size_t i; int k;
    for (i=0;i<n;i++){ x = (x ^ stable[i]) * 1099511628211ull; }
    x ^= seed;
    for (k=0;k<2;k++){
        uint64_t z = (x += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z>>30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z>>27)) * 0x94D049BB133111EBull;
        z ^=  z>>31;
        memcpy(out + (size_t)k*8, &z, 8);
    }
    out[6] = (uint8_t)((out[6] & 0x0Fu) | 0x80u);  /* version 8 (custom) */
    out[8] = (uint8_t)((out[8] & 0x3Fu) | 0x80u);  /* variant 10x (RFC) */
}

size_t dart_discovery_required_memory(const dart_discovery_config *cfg){
    size_t s = (sizeof(struct dart_discovery_state) + 7u) & ~(size_t)7u;
    if (!cfg) return 0;
    return 8u + s + (size_t)cfg->max_peers * sizeof(dart_discovery_peer_);
}

dart_discovery_state *dart_discovery_init(void *mem, size_t cap, const dart_discovery_config *cfg){
    uintptr_t a; uint8_t *base; size_t shdr; dart_discovery_state *st;
    if (!mem || !cfg || cfg->max_peers == 0) return NULL;
    if (cfg->announce_us == 0 || cfg->timeout_us == 0) return NULL;
    if (cfg->meta_len > DART_DISCOVERY_META_MAX) return NULL;
    if (cfg->meta_len && !cfg->meta) return NULL;
    if (cap < dart_discovery_required_memory(cfg)) return NULL;

    a = ((uintptr_t)mem + 7u) & ~(uintptr_t)7u;
    base = (uint8_t *)a;
    shdr = (sizeof(struct dart_discovery_state) + 7u) & ~(size_t)7u;

    st = (dart_discovery_state *)base;
    memset(st, 0, sizeof(*st));
    st->cfg           = *cfg;
    st->peers         = (dart_discovery_peer_ *)(base + shdr);
    st->cap_peers     = cfg->max_peers;
    st->next_local_id = 1;
    st->started       = 0;
    memset(st->peers, 0, (size_t)st->cap_peers * sizeof(dart_discovery_peer_));
    return st;
}

static int dart_discovery_find(dart_discovery_state *st, const uint8_t *uuid){
    uint16_t i;
    for (i=0;i<st->cap_peers;i++)
        if (st->peers[i].used && memcmp(st->peers[i].uuid, uuid, 16)==0) return (int)i;
    return -1;
}

static int dart_discovery_alloc(dart_discovery_state *st){
    uint16_t i, stalest = 0; uint64_t oldest = (uint64_t)-1; int any = -1;
    for (i=0;i<st->cap_peers;i++){
        if (!st->peers[i].used) return (int)i;
        if (st->peers[i].last_heard_us <= oldest){ oldest = st->peers[i].last_heard_us; stalest = i; }
        any = 1;
    }
    if (any < 0) return -1;
    if (st->cfg.on_peer_down) st->cfg.on_peer_down(st->cfg.user, st->peers[stalest].local_id);
    st->peers[stalest].used = 0;
    return (int)stalest;
}

static size_t dart_discovery_build(dart_discovery_state *st, uint8_t flags, uint8_t *p, size_t cap){
    if (cap < (size_t)DART_DISCOVERY_HDR_LEN + 1u + st->cfg.meta_len) return 0;
    p[0]='u'; p[1]='D'; p[2]='S'; p[3]='C';
    p[4]=(uint8_t)DART_DISCOVERY_PROTO_VERSION;
    p[5]=flags;
    dart_discovery_wr16(p+6, st->cfg.domain_id);
    memcpy(p+8, st->cfg.uuid, 16);
    dart_discovery_wr16(p+24, st->cfg.data_port);
    p[26]= st->cfg.self_ip_len;
    memset(p+27, 0, 16);
    if (st->cfg.self_ip_len==4 || st->cfg.self_ip_len==16)
        memcpy(p+27, st->cfg.self_ip, st->cfg.self_ip_len);
    p[DART_DISCOVERY_HDR_LEN] = st->cfg.meta_len;
    if (st->cfg.meta_len) memcpy(p+DART_DISCOVERY_HDR_LEN+1, st->cfg.meta, st->cfg.meta_len);
    return (size_t)DART_DISCOVERY_HDR_LEN + 1u + st->cfg.meta_len;
}

void dart_discovery_on_datagram(dart_discovery_state *st, const uint8_t *src_ip, uint8_t src_ip_len,
                       const void *dg, size_t len, uint64_t now){
    const uint8_t *p = (const uint8_t *)dg;
    uint8_t flags, sipl, mlen; const uint8_t *uuid, *sip, *meta;
    uint16_t port; dart_discovery_addr addr; int idx, changed;

    if (len < (size_t)DART_DISCOVERY_HDR_LEN + 1u) return;
    if (p[0]!='u'||p[1]!='D'||p[2]!='S'||p[3]!='C') return;
    if (p[4]!=(uint8_t)DART_DISCOVERY_PROTO_VERSION) return;
    if (dart_discovery_rd16(p+6) != st->cfg.domain_id) return;
    mlen = p[DART_DISCOVERY_HDR_LEN];
    if (mlen > DART_DISCOVERY_META_MAX || (size_t)DART_DISCOVERY_HDR_LEN + 1u + mlen > len) return;
    meta = p + DART_DISCOVERY_HDR_LEN + 1;

    uuid = p+8;
    if (memcmp(uuid, st->cfg.uuid, 16)==0) return;  /* ignore self */

    flags = p[5];
    port  = dart_discovery_rd16(p+24);
    sipl  = p[26];
    sip   = p+27;

    memset(&addr, 0, sizeof addr);
    if (sipl==4 || sipl==16){ addr.ip_len = sipl; memcpy(addr.ip, sip, sipl); }
    else if (src_ip && (src_ip_len==4 || src_ip_len==16)){ addr.ip_len = src_ip_len; memcpy(addr.ip, src_ip, src_ip_len); }
    else return;
    addr.port = port;

    idx = dart_discovery_find(st, uuid);

    if (flags & DART_DISCOVERY_FLAG_BYE){
        if (idx >= 0){
            uint32_t lid = st->peers[idx].local_id;
            st->peers[idx].used = 0;
            if (st->cfg.on_peer_down) st->cfg.on_peer_down(st->cfg.user, lid);
        }
        return;
    }

    if (idx < 0){
        idx = dart_discovery_alloc(st);
        if (idx < 0) return;
        memset(&st->peers[idx], 0, sizeof(dart_discovery_peer_));
        st->peers[idx].used     = 1;
        memcpy(st->peers[idx].uuid, uuid, 16);
        st->peers[idx].local_id = st->next_local_id++;
        st->peers[idx].ip_len   = 0xFF;   /* force first peer_up */
    }

    st->peers[idx].last_heard_us = now;

    changed = (st->peers[idx].ip_len != addr.ip_len)
           || (st->peers[idx].port   != addr.port)
           || (memcmp(st->peers[idx].ip, addr.ip, 16) != 0)
           || (st->peers[idx].meta_len != mlen)
           || (mlen && memcmp(st->peers[idx].meta, meta, mlen) != 0);
    if (changed){
        st->peers[idx].ip_len = addr.ip_len;
        st->peers[idx].port   = addr.port;
        memcpy(st->peers[idx].ip, addr.ip, 16);
        st->peers[idx].meta_len = mlen;
        if (mlen) memcpy(st->peers[idx].meta, meta, mlen);
        if (st->cfg.on_peer_up)
            st->cfg.on_peer_up(st->cfg.user, st->peers[idx].local_id, &addr,
                               mlen ? st->peers[idx].meta : NULL, mlen);
    }

    if ((flags & DART_DISCOVERY_FLAG_REQ) && st->started){
        /* peer is soliciting: reply with our announce sooner than the next
           periodic one, jittered by our uuid so many peers don't reply at once. */
        uint64_t when = now + (dart_discovery_fnv(st->cfg.uuid,16) % DART_DISCOVERY_SOLICIT_JITTER_US);
        if (when < st->next_announce_us) st->next_announce_us = when;
    }
}

size_t dart_discovery_update(dart_discovery_state *st, uint64_t now, void *out, size_t cap){
    uint16_t i; int first = !st->started;
    if (first){
        st->started = 1;
        st->next_announce_us = now + (dart_discovery_fnv(st->cfg.uuid,16) % st->cfg.announce_us);
    }
    for (i=0;i<st->cap_peers;i++){
        if (!st->peers[i].used) continue;
        if (now - st->peers[i].last_heard_us > st->cfg.timeout_us){
            uint32_t lid = st->peers[i].local_id;
            st->peers[i].used = 0;
            if (st->cfg.on_peer_down) st->cfg.on_peer_down(st->cfg.user, lid);
        }
    }
    if (first)   /* solicit on startup: announces us AND asks peers to reply now,
                    so discovery is ~instant instead of waiting an announce interval */
        return dart_discovery_build(st, DART_DISCOVERY_FLAG_REQ, (uint8_t *)out, cap);
    if (now >= st->next_announce_us){
        st->next_announce_us = now + st->cfg.announce_us;
        return dart_discovery_build(st, 0, (uint8_t *)out, cap);
    }
    return 0;
}

size_t dart_discovery_leave(dart_discovery_state *st, void *out, size_t cap){
    return dart_discovery_build(st, DART_DISCOVERY_FLAG_BYE, (uint8_t *)out, cap);
}

int dart_discovery_peer_addr(const dart_discovery_state *st, uint16_t slot, dart_discovery_addr *out){
    const dart_discovery_peer_ *p;
    if (slot >= st->cap_peers) return 0;
    p = &st->peers[slot];
    if (!p->used) return 0;
    memset(out, 0, sizeof *out);
    memcpy(out->ip, p->ip, 16);
    out->ip_len = p->ip_len;
    out->port   = p->port;
    return 1;
}

#ifndef DART_DISCOVERY_SANS_IO
/* ===== dart_discovery_rt.c ===== */
/* peer-discovery runtime: sockets, clock, UUID and the
 * one-tick loop. Platform socket headers stay in this file. */

/* Feature-test macros must precede the first system header. POSIX only. */
#if !defined(_WIN32)
  #ifndef _POSIX_C_SOURCE
  #define _POSIX_C_SOURCE 200809L
  #endif
  #ifndef _DEFAULT_SOURCE
  #define _DEFAULT_SOURCE 1
  #endif
#endif

#include <string.h>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <bcrypt.h>            /* BCryptGenRandom (CSPRNG) */
  #ifdef _MSC_VER
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "bcrypt.lib")
  #endif
  typedef SOCKET    dart_discovery_sock_t;
  typedef WSAPOLLFD dart_discovery_pollfd_t;
  #define DART_DISCOVERY_BADSOCK  INVALID_SOCKET
  #define DART_DISCOVERY_CLOSESOCK closesocket
  #define DART_DISCOVERY_POLL     WSAPoll
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <poll.h>
  #include <time.h>
  #include <stdio.h>
  #include <stdlib.h>           /* arc4random_buf on macOS/BSD */
  #if defined(ESP_PLATFORM)
    #include <esp_random.h>     /* esp_fill_random (HW RNG) */
  #elif defined(__linux__)
    #include <errno.h>
    #include <sys/random.h>     /* getrandom(2) */
  #endif
  typedef int           dart_discovery_sock_t;
  typedef struct pollfd dart_discovery_pollfd_t;
  #define DART_DISCOVERY_BADSOCK  (-1)
  #define DART_DISCOVERY_CLOSESOCK close
  #define DART_DISCOVERY_POLL     poll
#endif

struct dart_discovery_rt {
    dart_discovery_state        *core;
    dart_discovery_sock_t        fd;
    struct sockaddr_in  grp;
    uint16_t            max_peers;
    dart_discovery_addr seeds[DART_DISCOVERY_MAX_SEEDS];
    uint16_t            n_seeds;
};

static void dart_discovery_rt_tx1(dart_discovery_rt *rt, const uint8_t *out, size_t m,
                          const uint8_t ip[4], uint16_t port){
    struct sockaddr_in d;
    memset(&d, 0, sizeof d);
    d.sin_family = AF_INET;
    memcpy(&d.sin_addr.s_addr, ip, 4);
    d.sin_port = htons(port);
    sendto(rt->fd, (const char*)out, (int)m, 0, (struct sockaddr*)&d, sizeof d);
}

/* send a built announce/BYE to the group, to every configured seed, and to
 * every known peer. Known peers get a copy at the shared disc port and one at
 * their data port: the latter is the only per-process address when several
 * processes on one host share the disc port (the data-socket owner forwards
 * it via dart_discovery_rt_feed). Discovery then survives multicast outages
 * and, with seeds, bootstraps without multicast. Receivers dedup by uuid. */
static void dart_discovery_rt_tx(dart_discovery_rt *rt, const uint8_t *out, size_t m){
    uint16_t s, dport = ntohs(rt->grp.sin_port);
    dart_discovery_addr a;
    sendto(rt->fd, (const char*)out, (int)m, 0,
           (struct sockaddr*)&rt->grp, sizeof rt->grp);
    for (s=0; s<rt->n_seeds; s++){
        const dart_discovery_addr *sd = &rt->seeds[s];
        if (sd->ip_len != 4) continue;
        dart_discovery_rt_tx1(rt, out, m, sd->ip, sd->port ? sd->port : dport);
    }
    for (s=0; s<rt->max_peers; s++){
        if (!dart_discovery_peer_addr(rt->core, s, &a) || a.ip_len != 4) continue;
        dart_discovery_rt_tx1(rt, out, m, a.ip, dport);
        if (a.port && a.port != dport) dart_discovery_rt_tx1(rt, out, m, a.ip, a.port);
    }
}

static uint64_t dart_discovery_now_us(void){
#ifdef _WIN32
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart * 1000000ull) / (uint64_t)f.QuadPart);
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
#endif
}

static void dart_discovery_net_startup(void){
#ifdef _WIN32
    WSADATA w; WSAStartup(MAKEWORD(2,2), &w);
#endif
}

void dart_discovery_rt_feed(dart_discovery_rt *rt, const uint8_t *src_ip, uint8_t src_ip_len,
                    const void *dg, size_t len){
    if (!rt) return;
    dart_discovery_on_datagram(rt->core, src_ip, src_ip_len, dg, len, dart_discovery_now_us());
}

/* Every multicast send and join should pin to this one interface. */
uint32_t dart_discovery_mcast_if_for(uint32_t grp_naddr, uint16_t port){
    dart_discovery_sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a;
    uint32_t ip = htonl(INADDR_ANY);
    if (s == DART_DISCOVERY_BADSOCK) return ip;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = grp_naddr; a.sin_port = htons(port);
    if (connect(s, (struct sockaddr*)&a, sizeof a) == 0){
        struct sockaddr_in loc;
#ifdef _WIN32
        int ll = (int)sizeof loc;
#else
        socklen_t ll = sizeof loc;
#endif
        if (getsockname(s, (struct sockaddr*)&loc, &ll) == 0)
            ip = loc.sin_addr.s_addr;
    }
    DART_DISCOVERY_CLOSESOCK(s);
    return ip;
}

/* Fill buf from the platform CSPRNG. Returns 1 on success, 0 if unavailable. */
static int dart_discovery_os_random(void *buf, size_t len){
#if defined(_WIN32)
    /* NULL handle selects the system-preferred RNG. 0 == SUCCESS. */
    return BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(ESP_PLATFORM)
    esp_fill_random(buf, len);       /* HW RNG, true random while RF is up */
    return 1;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
      defined(__NetBSD__) || defined(__DragonFly__)
    arc4random_buf(buf, len);        /* CSPRNG, cannot fail */
    return 1;
#else
    {
        uint8_t *p = (uint8_t*)buf; size_t got = 0;
  #if defined(__linux__)
        while (got < len){           /* getrandom(2) */
            ssize_t r = getrandom(p + got, len - got, 0);
            if (r < 0){ if (errno == EINTR) continue; break; }
            got += (size_t)r;
        }
        if (got == len) return 1;
  #endif
        {                            /* fallback: /dev/urandom */
            FILE *f = fopen("/dev/urandom", "rb");
            if (f){
                size_t n = fread(p + got, 1, len - got, f);
                fclose(f);
                if (got + n == len) return 1;
            }
        }
        return 0;
    }
#endif
}

int dart_discovery_make_uuid4(uint8_t out[16]){
    if (!dart_discovery_os_random(out, 16)) return 0;
    out[6] = (uint8_t)((out[6] & 0x0Fu) | 0x40u);  /* version 4 */
    out[8] = (uint8_t)((out[8] & 0x3Fu) | 0x80u);  /* variant 10x */
    return 1;
}

static void dart_discovery_auto_uuid(uint8_t out[16]){
    char host[80]; uint64_t seed; size_t hl;
    if (dart_discovery_make_uuid4(out)) return;     /* normal path */

    /* No CSPRNG: derive a best-effort unique id from hostname, pid and clock. */
    memset(host, 0, sizeof host);
    gethostname(host, (int)sizeof host - 1);
    hl = strlen(host);
#ifdef _WIN32
    seed = ((uint64_t)GetCurrentProcessId() << 32) ^ dart_discovery_now_us();
#else
    seed = (uint64_t)getpid() ^ dart_discovery_now_us();
#endif
    dart_discovery_make_uuid(out, (const uint8_t*)host, hl, seed);
}

size_t dart_discovery_rt_required_memory(const dart_discovery_rt_config *cfg){
    dart_discovery_config c;
    size_t rt = (sizeof(struct dart_discovery_rt) + 15u) & ~(size_t)15u;
    if (!cfg) return 0;
    c = cfg->disc;
    if (c.announce_us == 0) c.announce_us = 1000000u;
    if (c.timeout_us  == 0) c.timeout_us  = c.announce_us * 7u / 2u;
    if (c.max_peers   == 0) c.max_peers   = 32u;
    return 16u + rt + dart_discovery_required_memory(&c);
}

dart_discovery_rt *dart_discovery_rt_open(void *mem, size_t cap, const dart_discovery_rt_config *cfg){
    dart_discovery_rt_config c;
    dart_discovery_rt *rt;
    uint8_t *base, *core_mem;
    size_t rtsz, need;
    dart_discovery_sock_t fd;
    int allzero = 1, i;
    int on = 1; unsigned char ttl, loop;
    struct sockaddr_in addr; struct ip_mreq mr;
    const char *group;

    if (!mem || !cfg) return NULL;
    c = *cfg;
    if (c.disc.announce_us == 0) c.disc.announce_us = 1000000u;
    if (c.disc.timeout_us  == 0) c.disc.timeout_us  = c.disc.announce_us * 7u / 2u;
    if (c.disc.max_peers   == 0) c.disc.max_peers   = 32u;
    group     = c.group     ? c.group     : "239.255.0.7";
    if (c.disc_port == 0)    c.disc_port  = 7400;
    ttl  = c.ttl ? c.ttl : 1;
    /* loopback always on: the UUID self-filter drops our echoes, and it's
     * required for multiple instances per host. */
    loop = 1;

    need = dart_discovery_rt_required_memory(&c);
    if (cap < need) return NULL;

    base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    rt   = (dart_discovery_rt*)base;
    rtsz = (sizeof(struct dart_discovery_rt) + 15u) & ~(size_t)15u;
    core_mem = base + rtsz;

    /* auto-generate a UUID if the caller left it zero */
    for (i=0;i<16;i++) if (c.disc.uuid[i]) { allzero = 0; break; }
    if (allzero) dart_discovery_auto_uuid(c.disc.uuid);

    dart_discovery_net_startup();

    rt->core = dart_discovery_init(core_mem, cap - (size_t)(core_mem - (uint8_t*)mem), &c.disc);
    if (!rt->core) return NULL;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == DART_DISCOVERY_BADSOCK) return NULL;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const char*)&on, sizeof on);
#endif
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(c.disc_port);
    if (bind(fd, (struct sockaddr*)&addr, sizeof addr) != 0){ DART_DISCOVERY_CLOSESOCK(fd); return NULL; }

    /* pin join and egress to one deterministic interface */
    { uint32_t ifip = c.mcast_if ? inet_addr(c.mcast_if)
                                 : dart_discovery_mcast_if_for(inet_addr(group), c.disc_port);
      memset(&mr, 0, sizeof mr);
      mr.imr_multiaddr.s_addr = inet_addr(group);
      mr.imr_interface.s_addr = ifip;
      if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mr, sizeof mr) != 0){
          DART_DISCOVERY_CLOSESOCK(fd); return NULL;
      }
      setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&ifip, sizeof ifip); }
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL,  (const char*)&ttl,  sizeof ttl);
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof loop);

    rt->fd = fd;
    rt->max_peers = c.disc.max_peers;
    rt->n_seeds = 0;
    if (c.seeds){
        uint16_t k, ns = c.n_seeds;
        if (ns > DART_DISCOVERY_MAX_SEEDS) ns = DART_DISCOVERY_MAX_SEEDS;
        for (k=0;k<ns;k++) rt->seeds[k] = c.seeds[k];
        rt->n_seeds = ns;
    }
    memset(&rt->grp, 0, sizeof rt->grp);
    rt->grp.sin_family = AF_INET;
    rt->grp.sin_addr.s_addr = inet_addr(group);
    rt->grp.sin_port = htons(c.disc_port);
    return rt;
}

int dart_discovery_rt_poll(dart_discovery_rt *rt, int timeout_ms){
    dart_discovery_pollfd_t pfd;
    uint8_t out[DART_DISCOVERY_WIRE_MAX];
    int got = 0; size_t m;

    memset(&pfd, 0, sizeof pfd);
    pfd.fd = rt->fd; pfd.events = POLLIN;
    if (DART_DISCOVERY_POLL(&pfd, 1, timeout_ms) < 0) return -1;

    if (pfd.revents & POLLIN){
        uint8_t buf[DART_DISCOVERY_WIRE_MAX];
        struct sockaddr_in src; socklen_t slen = sizeof src;
        int n = (int)recvfrom(rt->fd, (char*)buf, (int)sizeof buf, 0,
                              (struct sockaddr*)&src, &slen);
        if (n > 0){
            uint8_t sip[4];
            memcpy(sip, &src.sin_addr.s_addr, 4);
            dart_discovery_on_datagram(rt->core, sip, 4, buf, (size_t)n, dart_discovery_now_us());
            got = 1;
        }
    }

    m = dart_discovery_update(rt->core, dart_discovery_now_us(), out, sizeof out);
    if (m) dart_discovery_rt_tx(rt, out, m);
    return got;
}

void dart_discovery_rt_close(dart_discovery_rt *rt, int send_bye){
    if (!rt) return;
    if (send_bye){
        uint8_t out[DART_DISCOVERY_WIRE_MAX];
        size_t m = dart_discovery_leave(rt->core, out, sizeof out);
        if (m) dart_discovery_rt_tx(rt, out, m);
    }
    DART_DISCOVERY_CLOSESOCK(rt->fd);
#ifdef _WIN32
    WSACleanup();
#endif
}
#endif /* !DART_DISCOVERY_SANS_IO */
#endif /* DART_DISCOVERY_IMPLEMENTATION */
