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
/* sans-IO peer-discovery core: no socket, clock, or heap. Feed it datagrams +
 * now_us; it returns datagrams to send and fires peer up/down callbacks. For an
 * IO-owning layer see dart_discovery_rt.h. */
#ifndef DART_DISCOVERY_H
#define DART_DISCOVERY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DART_DISCOVERY_PROTO_VERSION
#define DART_DISCOVERY_PROTO_VERSION 3     /* v3: versioned meta blob, u16 meta_len */
#endif

#define DART_DISCOVERY_META_MAX 64   /* default per-peer meta capacity (cfg.meta_cap overrides) */
/* fixed header through self_ip, then [u32 meta_version][u16 meta_len][meta...] */
#define DART_DISCOVERY_META_OFF 49   /* DART_DISCOVERY_HDR_LEN(43) + 4 (version) + 2 (len) */
/* smallest egress/ingress datagram buffer; the runtime grows it to fit meta_cap */
#define DART_DISCOVERY_WIRE_MAX 128

typedef struct {
    uint8_t  ip[16];   /* network-order bytes */
    uint8_t  ip_len;   /* 4 = IPv4, 16 = IPv6 */
    uint16_t port;     /* data port, host order */
} dart_discovery_addr;

/* Why a peer is going down, so the IO layer can keep transport state across a
 * transient blip instead of tearing it down on every silence timeout. */
typedef enum {
    DART_DISCOVERY_DROP = 0,  /* fell silent past timeout_us: same UUID may return, keep state */
    DART_DISCOVERY_GONE = 1   /* said BYE, or its slot was reclaimed for a new peer: free state */
} dart_discovery_down_reason;

/* peer_up: reachable at addr (re-fires when a known peer's addr/meta changes, and
 * when a DROPPED peer returns under the SAME peer_id, so the IO layer can resume).
 * peer_down: going down; reason says whether the state is worth keeping. peer_id is
 * a local handle, stable across a DROP/return, freed only on GONE. meta is the
 * peer's opaque payload (NULL if none), valid only for the call. */
typedef void (*dart_discovery_peer_up_fn)  (void *user, uint32_t peer_id, const dart_discovery_addr *addr,
                                   const uint8_t *meta, uint16_t meta_len);
typedef void (*dart_discovery_peer_down_fn)(void *user, uint32_t peer_id,
                                   dart_discovery_down_reason reason);
/* A new peer arrived but the table is full of ACTIVE peers (none droppable): the
 * peer is refused rather than evicting a live conversation. Diagnostic only. */
typedef void (*dart_discovery_peer_refused_fn)(void *user, const dart_discovery_addr *addr);

typedef struct {
    uint8_t  uuid[16];      /* unique per process instance (regen each boot) */
    uint16_t domain_id;     /* logical-network selector */
    uint16_t data_port;     /* unicast port we advertise */
    uint8_t  self_ip[16];   /* optional advertised IP; len 0 => use src addr */
    uint8_t  self_ip_len;   /* 0, 4, or 16 */
    uint32_t announce_us;   /* re-announce interval */
    uint32_t timeout_us;    /* drop peer after this much silence */
    uint16_t max_peers;     /* table capacity */
    const uint8_t *meta;    /* opaque versioned blob; the INITIAL value (dart_discovery_set_meta
                               updates it at runtime). Must stay valid. <= meta_cap */
    uint16_t meta_len;
    uint16_t meta_cap;      /* per-peer meta buffer capacity; 0 => DART_DISCOVERY_META_MAX */
    dart_discovery_peer_up_fn      on_peer_up;
    dart_discovery_peer_down_fn    on_peer_down;
    dart_discovery_peer_refused_fn on_peer_refused;  /* optional: table full of active peers */
    void *user;
} dart_discovery_config;

typedef struct dart_discovery_state dart_discovery_state;

size_t       dart_discovery_required_memory(const dart_discovery_config *cfg);
dart_discovery_state *dart_discovery_init(void *mem, size_t mem_size, const dart_discovery_config *cfg);
void         dart_discovery_on_datagram(dart_discovery_state *st, const uint8_t *src_ip, uint8_t src_ip_len,
                               const void *dg, size_t len, uint64_t now_us);
size_t       dart_discovery_update(dart_discovery_state *st, uint64_t now_us, void *out, size_t cap);
size_t       dart_discovery_leave(dart_discovery_state *st, void *out, size_t cap);
/* Queue a one-shot solicit: the next update asks peers to announce now (sent once at startup). */
void         dart_discovery_solicit(dart_discovery_state *st);
/* Replace the opaque meta blob and bump its version, so peers re-fetch it. The
 * blob rides the next few announces, then announces carry the version only; a peer
 * that fell behind re-fetches via a targeted solicit. meta must stay valid. */
void         dart_discovery_set_meta(dart_discovery_state *st, const uint8_t *meta, uint16_t meta_len);
/* Drain one targeted (unicast) datagram and its destination: a solicit REPLY to a
 * peer that solicited us (carries the blob), or a re-fetch REQ to a peer whose
 * advertised version is ahead of what we hold. Returns bytes + fills *to, or 0 when
 * none. Loop like dart_discovery_update; the runtime unicasts each to *to. */
size_t       dart_discovery_poll_targeted(dart_discovery_state *st, void *out, size_t cap,
                             dart_discovery_addr *to);
/* Count of live peers currently known. */
uint16_t     dart_discovery_peer_count(const dart_discovery_state *st);
/* Address of the peer in table slot (0..max_peers-1); 1 + fills *out if it holds a
 * live peer. Lets a runtime reinforce announces over unicast to survive multicast outages. */
int          dart_discovery_peer_addr(const dart_discovery_state *st, uint16_t slot,
                             dart_discovery_addr *out);
/* Deterministic UUID from a stable input (e.g. serial/MAC) + boot seed. RFC 9562 v8. NOT cryptographic. */
void         dart_discovery_make_uuid(uint8_t out[16], const uint8_t *stable, size_t stable_len,
                             uint64_t boot_seed);

#ifdef __cplusplus
}
#endif
#endif /* DART_DISCOVERY_H */

#ifndef DART_DISCOVERY_SANS_IO
/* ===== dart_plat.h ===== */
/* dart_plat: the one platform layer. Every OS dependency the runtimes need lives
 * behind this contract: a monotonic clock, UDP sockets, multicast, entropy, and
 * the source-address route probe. The layers above (discovery_rt, node) speak
 * only dart_plat_* and never touch a sockaddr, winsock, or a platform #ifdef, so
 * a new platform is one new implementation of this header. A platform that
 * already has BSD sockets needs no new code: the bundled implementation covers
 * Windows and POSIX (Linux/macOS/BSD/ESP-lwIP). Pure IO: stripped under *_SANS_IO
 * with the runtime layers. On non-MSVC Windows link -lws2_32 -lbcrypt.
 *
 * Address convention: endpoints (send/recv, peers, seeds) are a uint8_t ip[4]
 * plus a host-order uint16_t port. Multicast group and interface addresses are a
 * uint32_t in NETWORK byte order ("naddr", as from dart_plat_parse_ip /
 * dart_plat_ipv4). The two are the same four bytes; move between them with
 * dart_plat_ip4_to_naddr / dart_plat_naddr_to_ip4.
 */
#ifndef DART_PLAT_H
#define DART_PLAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque socket handle: a POSIX fd or a Windows SOCKET, both fit in intptr_t. */
typedef intptr_t dart_sock;
#define DART_SOCK_BAD ((dart_sock)-1)

/* Poll-set entry; mirrors struct pollfd but platform-neutral. */
#define DART_POLLIN 0x01
typedef struct { dart_sock fd; short events; short revents; } dart_pollfd;

/* Process-wide net init/teardown (WSAStartup/WSACleanup; no-op elsewhere).
 * Refcounted, so a node and its discovery opening/closing in turn pair safely.
 * startup returns 1 on success, 0 on failure. */
int  dart_plat_startup(void);
void dart_plat_cleanup(void);

/* Monotonic microseconds from an arbitrary epoch. */
uint64_t dart_plat_now_us(void);

/* CSPRNG fill; 1 on success, 0 if no entropy source (caller falls back). */
int      dart_plat_random(void *buf, size_t len);
/* Best-effort host identity for a UUID fallback when the CSPRNG is unavailable. */
size_t   dart_plat_hostname(char *buf, size_t cap);   /* returns bytes written */
uint64_t dart_plat_pid(void);

/* --- UDP sockets --- */
dart_sock dart_plat_udp_open(void);                   /* DART_SOCK_BAD on failure */
void      dart_plat_close(dart_sock s);
/* Bind to if_naddr (0 = INADDR_ANY) : port (0 = OS ephemeral). reuse sets
 * SO_REUSEADDR (+ SO_REUSEPORT where it exists) before binding. 1 ok, 0 fail. */
int       dart_plat_bind(dart_sock s, uint32_t if_naddr, uint16_t port, int reuse);
/* Bound port in host order (read an ephemeral bind back); 0 on failure. */
uint16_t  dart_plat_local_port(dart_sock s);
void      dart_plat_set_nonblock(dart_sock s);
void      dart_plat_set_rcvbuf(dart_sock s, int bytes);
void      dart_plat_set_sndbuf(dart_sock s, int bytes);
/* Stop a bounced datagram (ICMP port-unreachable) from failing the next recv on
 * a shared RX socket (Windows SIO_UDP_CONNRESET; no-op elsewhere). */
void      dart_plat_suppress_connreset(dart_sock s);

/* --- multicast --- */
void dart_plat_mcast_setif(dart_sock s, uint32_t if_naddr);
void dart_plat_mcast_ttl  (dart_sock s, uint8_t ttl);
void dart_plat_mcast_loop (dart_sock s, int on);
int  dart_plat_mcast_join (dart_sock s, uint32_t grp_naddr, uint32_t if_naddr); /* 1 ok */

/* --- datagram IO --- */
/* sendto: returns bytes sent, <0 on error (test dart_plat_would_block). */
int  dart_plat_send(dart_sock s, const void *buf, size_t len,
                    const uint8_t ip[4], uint16_t port);
/* recvfrom: returns bytes (>0), 0 or <0 if none. src_ip/src_port out, may be NULL. */
int  dart_plat_recv(dart_sock s, void *buf, size_t cap,
                    uint8_t src_ip[4], uint16_t *src_port);
int  dart_plat_would_block(void);
/* poll up to n fds for timeout_ms; >0 ready, 0 timeout, <0 error. */
int  dart_plat_poll(dart_pollfd *fds, int n, int timeout_ms);

/* --- address helpers (uint32_t naddr is network byte order) --- */
uint32_t dart_plat_parse_ip(const char *dotted);          /* "1.2.3.4" -> naddr */
uint32_t dart_plat_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
uint32_t dart_plat_ip4_to_naddr(const uint8_t ip[4]);
void     dart_plat_naddr_to_ip4(uint32_t naddr, uint8_t out[4]);
/* Source address the OS would use to reach dst_naddr:port (connect + getsockname
 * on an unbound UDP socket; no packet leaves). 0 on failure. Backs interface
 * pinning and the same-host check. */
uint32_t dart_plat_route_src(uint32_t dst_naddr, uint16_t port);

/* --- SHM platform surface (DESIGN ONLY; defined when dart_shm lands) ---------
 * The zero-copy same-host path (src/dart_shm.h) needs three more primitives,
 * named here so the platform contract is whole. dart_plat.c does NOT define them
 * yet; the SHM implementation adds them behind the same Windows/POSIX split.
 *
 *   shared-memory mapping (shm_open+ftruncate+mmap / CreateFileMapping+MapView):
 *     void *dart_plat_shm_create(const char *name, size_t bytes, void **handle);
 *     void *dart_plat_shm_attach(const char *name, size_t bytes, void **handle);
 *     void  dart_plat_shm_detach(void *base, size_t bytes, void *handle, int unlink_it);
 *   cross-process atomics on the chunk refcount (C11 stdatomic / Interlocked*):
 *     int32_t dart_plat_atomic_add (volatile int32_t *p, int32_t delta);
 *     int32_t dart_plat_atomic_load(volatile int32_t *p);
 *   a stable per-kernel id for the same-host check (boot id / machine GUID):
 *     void dart_plat_host_uuid(uint8_t out[16]);
 */

#ifdef __cplusplus
}
#endif
#endif /* DART_PLAT_H */
/* ===== dart_discovery_rt.h ===== */
/* peer-discovery runtime: UDP multicast, clock, UUID, and a one-tick loop over
 * the dart_discovery core. On non-MSVC Windows, link -lws2_32 -lbcrypt. */
#ifndef DART_DISCOVERY_RT_H
#define DART_DISCOVERY_RT_H


#ifdef __cplusplus
extern "C" {
#endif

#define DART_DISCOVERY_MAX_SEEDS 4

/* Zero/NULL fields get defaults; leave disc.uuid all-zero to auto-generate one. */
typedef struct {
    dart_discovery_config disc;        /* core config: ids, timing, callbacks */
    const char  *group;       /* multicast group, default "239.255.0.7" */
    uint16_t     disc_port;   /* rendezvous port, default 7400 */
    uint8_t      ttl;         /* multicast TTL, default 1 */
    const char  *mcast_if;    /* interface IP to join/send on; NULL = route probe,
                                 "127.0.0.1" = single-host */
    const dart_discovery_addr *seeds;  /* peers to also unicast announces to, for
                                 networks where multicast is filtered (max DART_DISCOVERY_MAX_SEEDS) */
    uint16_t     n_seeds;
} dart_discovery_rt_config;

typedef struct dart_discovery_rt dart_discovery_rt;

size_t     dart_discovery_rt_required_memory(const dart_discovery_rt_config *cfg);
/* Open the socket, join the group, place core state in mem. NULL on failure. */
dart_discovery_rt  *dart_discovery_rt_open(void *mem, size_t mem_size, const dart_discovery_rt_config *cfg);
/* One loop tick: wait up to timeout_ms for a datagram, feed RX, pump timers, send
 * what's due. Returns 1 if a datagram arrived, 0 if idle, <0 on socket error. */
int        dart_discovery_rt_poll(dart_discovery_rt *rt, int timeout_ms);
/* Gather membership at startup: solicit, then pump until the peer set is quiet for
 * quiet_ms or timeout_ms total. Re-solicits periodically. Returns the peer count. Blocks. */
int        dart_discovery_rt_settle(dart_discovery_rt *rt, int quiet_ms, int timeout_ms);
/* Optionally multicast a graceful BYE, then close the socket. */
void       dart_discovery_rt_close(dart_discovery_rt *rt, int send_bye);

/* Hand the core a discovery datagram that arrived on another socket (unicast
 * announces target the peer's data port, so the data-socket owner forwards them). */
void       dart_discovery_rt_feed(dart_discovery_rt *rt, const uint8_t *src_ip, uint8_t src_ip_len,
                          const void *dg, size_t len);

/* Replace the opaque meta blob carried in announces and bump its version, so peers
 * re-fetch it (e.g. after an interest change). meta must outlive the runtime. */
void       dart_discovery_rt_set_meta(dart_discovery_rt *rt, const uint8_t *meta, uint16_t meta_len);

/* Fill out[16] with a random RFC 9562 v4 UUID; 1 ok, 0 if no entropy source. */
int        dart_discovery_make_uuid4(uint8_t out[16]);

/* Egress interface the OS routes to group:port (INADDR_ANY on failure). Exposed so
 * layers above pin their multicast sockets to the same interface on multihomed hosts. */
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
#define DART_DISCOVERY_BLOB_RESEND 3u        /* announces that carry the full blob after a change */

struct dart_discovery_peer_ {
    uint8_t  used;
    uint8_t  dropped;       /* used but silent past timeout_us: state kept for a same-UUID return */
    uint8_t  uuid[16];
    uint32_t local_id;
    uint8_t  ip[16];
    uint8_t  ip_len;
    uint16_t port;
    uint64_t last_heard_us;
    uint8_t *meta;          /* -> meta_pool slot, capacity st->meta_cap */
    uint16_t meta_len;
    uint32_t meta_version;  /* version of the blob we hold (0 = none yet) */
    uint8_t  reply_due;     /* owes a unicast announce+blob (this peer solicited us) */
    uint8_t  solicit_due;   /* owes a unicast REQ (re-fetch: peer's version is ahead) */
};
typedef struct dart_discovery_peer_ dart_discovery_peer_;

struct dart_discovery_state {
    dart_discovery_config  cfg;
    uint64_t      next_announce_us;
    uint32_t      next_local_id;
    uint8_t       started;
    uint8_t       want_solicit;   /* a multicast solicit (REQ) is queued for the next update */
    uint16_t      cap_peers;
    uint16_t      meta_cap;       /* per-peer meta buffer capacity */
    uint8_t      *meta_pool;      /* [cap_peers * meta_cap] */
    /* our outgoing blob + monotonic version */
    const uint8_t *self_meta;
    uint16_t      self_meta_len;
    uint32_t      self_meta_version;
    uint16_t      self_blob_resend; /* announces remaining that carry the full blob */
    uint16_t      targeted_cursor;  /* round-robin over peers for poll_targeted */
    dart_discovery_peer_  *peers;
};

static void     dart_discovery_wr16(uint8_t *p, uint16_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static uint16_t dart_discovery_rd16(const uint8_t *p){ return (uint16_t)(p[0] | ((uint16_t)p[1]<<8)); }
static void     dart_discovery_wr32(uint8_t *p, uint32_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static uint32_t dart_discovery_rd32(const uint8_t *p){ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }

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

static uint16_t dart_discovery_meta_cap(const dart_discovery_config *cfg){
    return cfg->meta_cap ? cfg->meta_cap : DART_DISCOVERY_META_MAX;
}

size_t dart_discovery_required_memory(const dart_discovery_config *cfg){
    size_t s = (sizeof(struct dart_discovery_state) + 7u) & ~(size_t)7u;
    if (!cfg) return 0;
    return 8u + s + (size_t)cfg->max_peers * sizeof(dart_discovery_peer_)
                  + (size_t)cfg->max_peers * dart_discovery_meta_cap(cfg);
}

dart_discovery_state *dart_discovery_init(void *mem, size_t cap, const dart_discovery_config *cfg){
    uintptr_t a; uint8_t *base; size_t shdr; dart_discovery_state *st; uint16_t i, mcap;
    if (!mem || !cfg || cfg->max_peers == 0) return NULL;
    if (cfg->announce_us == 0 || cfg->timeout_us == 0) return NULL;
    mcap = dart_discovery_meta_cap(cfg);
    if (cfg->meta_len > mcap) return NULL;
    if (cfg->meta_len && !cfg->meta) return NULL;
    if (cap < dart_discovery_required_memory(cfg)) return NULL;

    a = ((uintptr_t)mem + 7u) & ~(uintptr_t)7u;
    base = (uint8_t *)a;
    shdr = (sizeof(struct dart_discovery_state) + 7u) & ~(size_t)7u;

    st = (dart_discovery_state *)base;
    memset(st, 0, sizeof(*st));
    st->cfg           = *cfg;
    st->cap_peers     = cfg->max_peers;
    st->meta_cap      = mcap;
    st->peers         = (dart_discovery_peer_ *)(base + shdr);
    st->meta_pool     = (uint8_t *)st->peers + (size_t)st->cap_peers * sizeof(dart_discovery_peer_);
    st->next_local_id = 1;
    st->started       = 0;
    memset(st->peers, 0, (size_t)st->cap_peers * sizeof(dart_discovery_peer_));
    for (i=0;i<st->cap_peers;i++) st->peers[i].meta = st->meta_pool + (size_t)i * mcap;
    st->self_meta         = cfg->meta;
    st->self_meta_len     = cfg->meta_len;
    st->self_meta_version = 1;
    st->self_blob_resend  = DART_DISCOVERY_BLOB_RESEND;
    return st;
}

/* find by UUID, including DROPPED entries: a same-UUID return reuses the slot (and
 * thus the local_id), so the IO layer's transport state keyed by local_id resumes. */
static int dart_discovery_find(dart_discovery_state *st, const uint8_t *uuid){
    uint16_t i;
    for (i=0;i<st->cap_peers;i++)
        if (st->peers[i].used && memcmp(st->peers[i].uuid, uuid, 16)==0) return (int)i;
    return -1;
}

/* a slot for a brand-new peer: a FREE one, else the oldest DROPPED one (evicted,
 * fired GONE so its state is freed). ACTIVE peers are never evicted; -1 = refuse. */
static int dart_discovery_alloc(dart_discovery_state *st){
    uint16_t i, victim = 0; uint64_t oldest = (uint64_t)-1; int found = -1;
    for (i=0;i<st->cap_peers;i++){
        if (!st->peers[i].used) return (int)i;
        if (st->peers[i].dropped && st->peers[i].last_heard_us <= oldest){
            oldest = st->peers[i].last_heard_us; victim = i; found = 1;
        }
    }
    if (found < 0) return -1;   /* table full of ACTIVE peers: caller refuses + signals */
    if (st->cfg.on_peer_down) st->cfg.on_peer_down(st->cfg.user, st->peers[victim].local_id, DART_DISCOVERY_GONE);
    st->peers[victim].used = 0;
    return (int)victim;
}

/* A different uuid announcing from an (ip,port) we already hold means that endpoint's
 * process restarted: one socket is one process, so the old entry is provably dead.
 * Evict it as GONE (state freed) before adopting the newcomer, so its stale transport
 * state can't shadow the new incarnation whose data routes to the same address. */
static void dart_discovery_evict_endpoint(dart_discovery_state *st, const dart_discovery_addr *addr){
    uint16_t i;
    if (!addr->ip_len) return;
    for (i=0;i<st->cap_peers;i++){
        dart_discovery_peer_ *pe = &st->peers[i];
        if (!pe->used) continue;
        if (pe->ip_len==addr->ip_len && pe->port==addr->port && memcmp(pe->ip, addr->ip, 16)==0){
            uint32_t lid = pe->local_id;
            pe->used = 0;
            if (st->cfg.on_peer_down) st->cfg.on_peer_down(st->cfg.user, lid, DART_DISCOVERY_GONE);
        }
    }
}

/* build an announce/solicit/bye into p. with_blob includes the current meta blob;
 * every datagram carries the meta version so a blob-less announce still signals change. */
static size_t dart_discovery_build(dart_discovery_state *st, uint8_t flags, int with_blob,
                                   uint8_t *p, size_t cap){
    uint16_t mlen = with_blob ? st->self_meta_len : 0;
    size_t need = (size_t)DART_DISCOVERY_META_OFF + mlen;
    if (cap < need) return 0;
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
    dart_discovery_wr32(p+DART_DISCOVERY_HDR_LEN, st->self_meta_version);
    dart_discovery_wr16(p+DART_DISCOVERY_HDR_LEN+4, mlen);
    if (mlen) memcpy(p+DART_DISCOVERY_META_OFF, st->self_meta, mlen);
    return need;
}

static void dart_discovery_addr_of(const dart_discovery_peer_ *pe, dart_discovery_addr *out){
    memset(out, 0, sizeof *out);
    memcpy(out->ip, pe->ip, 16);
    out->ip_len = pe->ip_len;
    out->port   = pe->port;
}

void dart_discovery_on_datagram(dart_discovery_state *st, const uint8_t *src_ip, uint8_t src_ip_len,
                       const void *dg, size_t len, uint64_t now){
    const uint8_t *p = (const uint8_t *)dg;
    uint8_t flags, sipl; uint16_t mlen, port; uint32_t mver;
    const uint8_t *uuid, *sip, *meta;
    dart_discovery_addr addr; int idx, addr_changed, first_contact=0, blob_changed=0, revived=0;
    dart_discovery_peer_ *pe;

    if (len < (size_t)DART_DISCOVERY_META_OFF) return;
    if (p[0]!='u'||p[1]!='D'||p[2]!='S'||p[3]!='C') return;
    if (p[4]!=(uint8_t)DART_DISCOVERY_PROTO_VERSION) return;
    if (dart_discovery_rd16(p+6) != st->cfg.domain_id) return;
    mver = dart_discovery_rd32(p+DART_DISCOVERY_HDR_LEN);
    mlen = dart_discovery_rd16(p+DART_DISCOVERY_HDR_LEN+4);
    if (mlen > st->meta_cap || (size_t)DART_DISCOVERY_META_OFF + mlen > len) return;
    meta = p + DART_DISCOVERY_META_OFF;

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
            if (st->cfg.on_peer_down) st->cfg.on_peer_down(st->cfg.user, lid, DART_DISCOVERY_GONE);
        }
        return;
    }

    if (idx < 0){
        uint8_t *keep_meta;
        /* new uuid from an address we already hold => the endpoint's process restarted;
           evict the dead predecessor (frees its slot for reuse) so it can't shadow us */
        dart_discovery_evict_endpoint(st, &addr);
        idx = dart_discovery_alloc(st);
        if (idx < 0){       /* table full of active peers: refuse, never evict a live one */
            if (st->cfg.on_peer_refused) st->cfg.on_peer_refused(st->cfg.user, &addr);
            return;
        }
        keep_meta = st->peers[idx].meta;            /* preserve the pool pointer across reset */
        memset(&st->peers[idx], 0, sizeof(dart_discovery_peer_));
        st->peers[idx].meta     = keep_meta;
        st->peers[idx].used     = 1;
        memcpy(st->peers[idx].uuid, uuid, 16);
        st->peers[idx].local_id = st->next_local_id++;
        st->peers[idx].ip_len   = 0xFF;   /* force first peer_up */
        first_contact = 1;
    }
    pe = &st->peers[idx];
    if (pe->dropped){ pe->dropped = 0; revived = 1; }   /* a DROPPED peer returned: resume it */
    pe->last_heard_us = now;

    addr_changed = (pe->ip_len != addr.ip_len)
                || (pe->port   != addr.port)
                || (memcmp(pe->ip, addr.ip, 16) != 0);
    if (addr_changed){
        pe->ip_len = addr.ip_len; pe->port = addr.port;
        memcpy(pe->ip, addr.ip, 16);
    }

    /* meta: a newer version with the blob present updates our copy; a newer version
       without the blob (a steady-state version-only announce) means we fell behind,
       so re-fetch via a targeted solicit. */
    if (mlen){
        if (mver > pe->meta_version){
            memcpy(pe->meta, meta, mlen);
            pe->meta_len = mlen; pe->meta_version = mver;
            blob_changed = 1;
        }
        pe->solicit_due = 0;
    } else if (mver > pe->meta_version){
        pe->solicit_due = 1;
    }

    if ((first_contact || addr_changed || blob_changed || revived) && st->cfg.on_peer_up)
        st->cfg.on_peer_up(st->cfg.user, pe->local_id, &addr,
                           pe->meta_len ? pe->meta : NULL, pe->meta_len);

    if ((flags & DART_DISCOVERY_FLAG_REQ) && st->started)
        pe->reply_due = 1;   /* answer the solicit with a unicast announce + blob */
}

size_t dart_discovery_update(dart_discovery_state *st, uint64_t now, void *out, size_t cap){
    uint16_t i;
    if (!st->started){
        st->started = 1;
        st->next_announce_us = now + (dart_discovery_fnv(st->cfg.uuid,16) % st->cfg.announce_us);
        st->want_solicit = 1;   /* solicit on startup */
    }
    for (i=0;i<st->cap_peers;i++){
        if (!st->peers[i].used || st->peers[i].dropped) continue;
        if (now - st->peers[i].last_heard_us > st->cfg.timeout_us){
            /* fell silent: DEMOTE (keep the entry + local_id) so a same-UUID return
               resumes; the IO layer keeps its transport state on a DROP reason */
            st->peers[i].dropped = 1;
            if (st->cfg.on_peer_down) st->cfg.on_peer_down(st->cfg.user, st->peers[i].local_id, DART_DISCOVERY_DROP);
        }
    }
    if (st->want_solicit){   /* multicast solicit: announce us (with blob) AND ask peers to reply */
        st->want_solicit = 0;
        return dart_discovery_build(st, DART_DISCOVERY_FLAG_REQ, 1, (uint8_t *)out, cap);
    }
    if (now >= st->next_announce_us){
        int with_blob = st->self_blob_resend > 0;
        if (with_blob) st->self_blob_resend--;
        st->next_announce_us = now + st->cfg.announce_us;
        return dart_discovery_build(st, 0, with_blob, (uint8_t *)out, cap);
    }
    return 0;
}

void dart_discovery_set_meta(dart_discovery_state *st, const uint8_t *meta, uint16_t meta_len){
    if (!st || meta_len > st->meta_cap) return;   /* the node sizes meta_cap to fit */
    st->self_meta         = meta;
    st->self_meta_len     = meta_len;
    st->self_meta_version++;
    st->self_blob_resend  = DART_DISCOVERY_BLOB_RESEND;
    st->next_announce_us  = 0;   /* announce the change now, don't wait for the timer */
}

size_t dart_discovery_poll_targeted(dart_discovery_state *st, void *out, size_t cap,
                                    dart_discovery_addr *to){
    uint16_t n = st->cap_peers, k;
    if (n == 0) return 0;
    for (k=0;k<n;k++){
        uint16_t i = st->targeted_cursor;
        dart_discovery_peer_ *pe = &st->peers[i];
        st->targeted_cursor = (uint16_t)((i+1u) % n);
        if (!pe->used){ pe->reply_due = pe->solicit_due = 0; continue; }
        if (pe->reply_due){                 /* reply to a soliciter: announce + blob, unicast */
            pe->reply_due = 0;
            dart_discovery_addr_of(pe, to);
            return dart_discovery_build(st, 0, 1, (uint8_t *)out, cap);
        }
        if (pe->solicit_due){               /* re-fetch: ask this peer to announce back to us */
            pe->solicit_due = 0;
            dart_discovery_addr_of(pe, to);
            return dart_discovery_build(st, DART_DISCOVERY_FLAG_REQ, 1, (uint8_t *)out, cap);
        }
    }
    return 0;
}

/* queue a one-shot multicast solicit: the next update emits a REQ asking peers to announce now */
void dart_discovery_solicit(dart_discovery_state *st){ if (st) st->want_solicit = 1; }

uint16_t dart_discovery_peer_count(const dart_discovery_state *st){
    uint16_t i, c = 0;   /* live peers only; DROPPED entries linger for resume, not as members */
    for (i=0;i<st->cap_peers;i++) if (st->peers[i].used && !st->peers[i].dropped) c++;
    return c;
}

size_t dart_discovery_leave(dart_discovery_state *st, void *out, size_t cap){
    return dart_discovery_build(st, DART_DISCOVERY_FLAG_BYE, 0, (uint8_t *)out, cap);
}

int dart_discovery_peer_addr(const dart_discovery_state *st, uint16_t slot, dart_discovery_addr *out){
    const dart_discovery_peer_ *p;
    if (slot >= st->cap_peers) return 0;
    p = &st->peers[slot];
    if (!p->used) return 0;
    dart_discovery_addr_of(p, out);
    return 1;
}

#ifndef DART_DISCOVERY_SANS_IO
/* ===== dart_plat.c ===== */
/* dart_plat: the Windows + POSIX implementation of the platform contract. This
 * is the only file in DART carrying an OS #ifdef. Port to a new platform by
 * adding a branch here (or a sibling file against dart_plat.h); BSD-socket
 * platforms are already covered. See dart_plat.h. */

/* feature-test macros must precede the first system header (POSIX only) */
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
  #ifndef SIO_UDP_CONNRESET
  #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
  #endif
  typedef int dart__socklen;
  #define DART__FD(s) ((SOCKET)(s))
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <poll.h>
  #include <time.h>
  #include <fcntl.h>
  #include <errno.h>
  #include <stdio.h>
  #include <stdlib.h>           /* arc4random_buf on macOS/BSD */
  #if defined(ESP_PLATFORM)
    #include <esp_random.h>     /* esp_fill_random (HW RNG) */
  #elif defined(__linux__)
    #include <sys/random.h>     /* getrandom(2) */
  #endif
  typedef socklen_t dart__socklen;
  #define DART__FD(s) ((int)(s))
#endif

/* ----------------------------------------------------------------- lifecycle */
#ifdef _WIN32
static int dart__wsa_refs = 0;
int dart_plat_startup(void){
    WSADATA w;
    if (dart__wsa_refs == 0 && WSAStartup(MAKEWORD(2,2), &w) != 0) return 0;
    dart__wsa_refs++;
    return 1;
}
void dart_plat_cleanup(void){
    if (dart__wsa_refs > 0 && --dart__wsa_refs == 0) WSACleanup();
}
#else
int  dart_plat_startup(void){ return 1; }
void dart_plat_cleanup(void){}
#endif

/* --------------------------------------------------------------------- clock */
uint64_t dart_plat_now_us(void){
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

/* ------------------------------------------------------------ entropy / host */
int dart_plat_random(void *buf, size_t len){
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

size_t dart_plat_hostname(char *buf, size_t cap){
    if (!buf || cap == 0) return 0;
    buf[0] = 0;
    gethostname(buf, (int)cap - 1);
    buf[cap - 1] = 0;
    return strlen(buf);
}

uint64_t dart_plat_pid(void){
#ifdef _WIN32
    return (uint64_t)GetCurrentProcessId();
#else
    return (uint64_t)getpid();
#endif
}

/* --------------------------------------------------------------- UDP sockets */
dart_sock dart_plat_udp_open(void){
#ifdef _WIN32
    SOCKET fd = socket(AF_INET, SOCK_DGRAM, 0);
    return (fd == INVALID_SOCKET) ? DART_SOCK_BAD : (dart_sock)fd;
#else
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    return (fd < 0) ? DART_SOCK_BAD : (dart_sock)fd;
#endif
}

void dart_plat_close(dart_sock s){
    if (s == DART_SOCK_BAD) return;
#ifdef _WIN32
    closesocket((SOCKET)s);
#else
    close((int)s);
#endif
}

int dart_plat_bind(dart_sock s, uint32_t if_naddr, uint16_t port, int reuse){
    struct sockaddr_in a;
    if (reuse){
        int on = 1;
        setsockopt(DART__FD(s), SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);
#ifdef SO_REUSEPORT
        setsockopt(DART__FD(s), SOL_SOCKET, SO_REUSEPORT, (const char*)&on, sizeof on);
#endif
    }
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = if_naddr;            /* 0 == INADDR_ANY */
    a.sin_port = htons(port);
    return bind(DART__FD(s), (struct sockaddr*)&a, sizeof a) == 0;
}

uint16_t dart_plat_local_port(dart_sock s){
    struct sockaddr_in a; dart__socklen ll = sizeof a;
    memset(&a, 0, sizeof a);
    if (getsockname(DART__FD(s), (struct sockaddr*)&a, &ll) != 0) return 0;
    return ntohs(a.sin_port);
}

void dart_plat_set_nonblock(dart_sock s){
#ifdef _WIN32
    u_long nb = 1; ioctlsocket((SOCKET)s, FIONBIO, &nb);
#else
    int fl = fcntl((int)s, F_GETFL, 0);
    if (fl != -1) fcntl((int)s, F_SETFL, fl | O_NONBLOCK);
#endif
}

void dart_plat_set_rcvbuf(dart_sock s, int bytes){
    setsockopt(DART__FD(s), SOL_SOCKET, SO_RCVBUF, (const char*)&bytes, sizeof bytes);
}
void dart_plat_set_sndbuf(dart_sock s, int bytes){
    setsockopt(DART__FD(s), SOL_SOCKET, SO_SNDBUF, (const char*)&bytes, sizeof bytes);
}

void dart_plat_suppress_connreset(dart_sock s){
#ifdef _WIN32
    BOOL off = FALSE; DWORD bv = 0;
    WSAIoctl((SOCKET)s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &bv, NULL, NULL);
#else
    (void)s;
#endif
}

/* ----------------------------------------------------------------- multicast */
void dart_plat_mcast_setif(dart_sock s, uint32_t if_naddr){
    setsockopt(DART__FD(s), IPPROTO_IP, IP_MULTICAST_IF, (const char*)&if_naddr, sizeof if_naddr);
}
void dart_plat_mcast_ttl(dart_sock s, uint8_t ttl){
    unsigned char t = ttl;
    setsockopt(DART__FD(s), IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&t, sizeof t);
}
void dart_plat_mcast_loop(dart_sock s, int on){
    unsigned char l = (unsigned char)(on ? 1 : 0);
    setsockopt(DART__FD(s), IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&l, sizeof l);
}
int dart_plat_mcast_join(dart_sock s, uint32_t grp_naddr, uint32_t if_naddr){
    struct ip_mreq mr; memset(&mr, 0, sizeof mr);
    mr.imr_multiaddr.s_addr = grp_naddr;
    mr.imr_interface.s_addr = if_naddr;
    return setsockopt(DART__FD(s), IPPROTO_IP, IP_ADD_MEMBERSHIP,
                      (const char*)&mr, sizeof mr) == 0;
}

/* --------------------------------------------------------------- datagram IO */
int dart_plat_send(dart_sock s, const void *buf, size_t len,
                   const uint8_t ip[4], uint16_t port){
    struct sockaddr_in d;
    memset(&d, 0, sizeof d);
    d.sin_family = AF_INET;
    memcpy(&d.sin_addr.s_addr, ip, 4);
    d.sin_port = htons(port);
    return (int)sendto(DART__FD(s), (const char*)buf, (int)len, 0,
                       (struct sockaddr*)&d, sizeof d);
}

int dart_plat_recv(dart_sock s, void *buf, size_t cap,
                   uint8_t src_ip[4], uint16_t *src_port){
    struct sockaddr_in src; dart__socklen sl = sizeof src;
    int n;
    memset(&src, 0, sizeof src);
    n = (int)recvfrom(DART__FD(s), (char*)buf, (int)cap, 0,
                      (struct sockaddr*)&src, &sl);
    if (n > 0){
        if (src_ip)   memcpy(src_ip, &src.sin_addr.s_addr, 4);
        if (src_port) *src_port = ntohs(src.sin_port);
    }
    return n;
}

int dart_plat_would_block(void){
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

int dart_plat_poll(dart_pollfd *fds, int n, int timeout_ms){
    /* callers poll one or two sockets; cap the on-stack translation buffer */
#ifdef _WIN32
    WSAPOLLFD p[8];
#else
    struct pollfd p[8];
#endif
    int i, r;
    if (n < 0) return -1;
    if (n > 8) n = 8;
    memset(p, 0, sizeof p);
    for (i = 0; i < n; i++){
        p[i].fd = DART__FD(fds[i].fd);
        p[i].events = (short)((fds[i].events & DART_POLLIN) ? POLLIN : 0);
    }
#ifdef _WIN32
    r = WSAPoll(p, (ULONG)n, timeout_ms);
#else
    r = poll(p, (nfds_t)n, timeout_ms);
#endif
    for (i = 0; i < n; i++)
        fds[i].revents = (short)((p[i].revents & POLLIN) ? DART_POLLIN : 0);
    return r;
}

/* ----------------------------------------------------------- address helpers */
uint32_t dart_plat_parse_ip(const char *dotted){
    return dotted ? (uint32_t)inet_addr(dotted) : 0;
}
uint32_t dart_plat_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d){
    return htonl(((uint32_t)a << 24) | ((uint32_t)b << 16) |
                 ((uint32_t)c << 8)  |  (uint32_t)d);
}
uint32_t dart_plat_ip4_to_naddr(const uint8_t ip[4]){
    uint32_t n; memcpy(&n, ip, 4); return n;
}
void dart_plat_naddr_to_ip4(uint32_t naddr, uint8_t out[4]){
    memcpy(out, &naddr, 4);
}

uint32_t dart_plat_route_src(uint32_t dst_naddr, uint16_t port){
    dart_sock s = dart_plat_udp_open();
    struct sockaddr_in a;
    uint32_t ip = 0;                         /* INADDR_ANY on failure */
    if (s == DART_SOCK_BAD) return ip;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = dst_naddr; a.sin_port = htons(port);
    if (connect(DART__FD(s), (struct sockaddr*)&a, sizeof a) == 0){
        struct sockaddr_in loc; dart__socklen ll = sizeof loc;
        if (getsockname(DART__FD(s), (struct sockaddr*)&loc, &ll) == 0)
            ip = loc.sin_addr.s_addr;
    }
    dart_plat_close(s);
    return ip;
}
/* ===== dart_discovery_rt.c ===== */
/* peer-discovery runtime: the one-tick loop over the dart_discovery core, plus
 * UUID generation. All OS access goes through dart_plat. */

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
#endif /* !DART_DISCOVERY_SANS_IO */
#endif /* DART_DISCOVERY_IMPLEMENTATION */
