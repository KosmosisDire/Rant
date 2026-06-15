/* GENERATED single-header build. DO NOT EDIT.
 * DART = Discovery And Realtime Transport. Amalgamated from src/ by
 * tools/pack.c. Edit the split sources in src/ and re-run pack to regenerate.
 * See the flag scheme at the top of tools/pack.c.
 */
#ifdef DART_IMPLEMENTATION
  #ifndef DART_DISCOVERY_IMPLEMENTATION
  #define DART_DISCOVERY_IMPLEMENTATION
  #endif
  #ifndef DART_TRANSPORT_IMPLEMENTATION
  #define DART_TRANSPORT_IMPLEMENTATION
  #endif
#endif
#ifdef DART_SANS_IO
  #ifndef DART_DISCOVERY_SANS_IO
  #define DART_DISCOVERY_SANS_IO
  #endif
  #ifndef DART_TRANSPORT_SANS_IO
  #define DART_TRANSPORT_SANS_IO
  #endif
#endif

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
/* ===== dart_transport.h ===== */
/* sans-IO reliable-UDP transport core. Owns no socket,
 * clock, or heap: feed it datagrams plus now_us and a peer set; it returns
 * datagrams to send and delivers reassembled samples via callback. RTPS-inspired
 * but not wire-compatible. Layer dart_node.h on top for a socket-owning node. */
#ifndef DART_TRANSPORT_H
#define DART_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DART_FRAG_PAYLOAD
#define DART_FRAG_PAYLOAD 1024u          /* bytes of sample data per TU */
#endif
#define DART_DGRAM_MAX (DART_FRAG_PAYLOAD + 32u)   /* + largest header */

typedef enum { DART_BEST_EFFORT = 0, DART_RELIABLE = 1 } dart_reliability;
/* DART_NONE: declared but inactive. All resources stay allocated at init;
 * dart_set_dir flips interest at runtime. */
typedef enum { DART_PUBSUB = 0, DART_PUB_ONLY = 1, DART_SUB_ONLY = 2,
               DART_NONE = 3 } dart_direction;

/* Built-in channel carrying pub/sub interest between peers, appended to every
 * channel table. Interest lists ride it as ordinary reliable KEEP_LAST(1)
 * samples: latest list wins, late joiners get a replay, subscription changes
 * are just new samples. The id is reserved; dart_init rejects it. */
#define DART_CHAN_META 0xFFFFu

typedef struct {
    dart_reliability reliability;
    uint16_t history_depth;     /* KEEP_LAST depth in samples (>=1)        */
    uint16_t join_replay;       /* reliable: cached samples replayed to a late-
                                   joining reader (capped at what history still
                                   holds). 0 (default) = join at the head and
                                   see only future samples; 1 = latest-value on
                                   join. Deep replays burst-multiply at startup
                                   (every joiner gets depth x sample_bytes at
                                   once), so keep this small.               */
    uint32_t max_sample_bytes;  /* largest sample on this channel          */
    uint32_t heartbeat_us;      /* reliable: writer heartbeat cadence      */
    uint32_t nack_delay_us;     /* reader: delay before NACK (suppression) */
    uint32_t max_block_us;      /* reliable: how long a send may wait for slow
                                   readers to ack before overwriting un-acked
                                   history. 0 (default) = pure KEEP_LAST. Worst
                                   case is min(max_block_us, peer timeout), since
                                   a dead reader's proxy also releases pressure.
                                   sans-IO callers wait via dart_send_would_evict. */
} dart_qos;

typedef struct {
    uint16_t channel_id;
    dart_qos   qos;
    uint8_t  dir;     /* dart_direction; 0 (zero-init) = publish + subscribe   */
    uint8_t  mcast;   /* 1 = new data and heartbeats may use a multicast group,
                         engaged only while remote subscribers exist. Same-host
                         subscribers always stay unicast. Repairs, acks, and
                         gap-to-one-reader stay unicast. Reliable mcast is
                         reliable-from-join-point: history replay on join is
                         unicast-only. */
} dart_channel_def;

/* dart_poll_send destination: peer id, or a per-channel multicast group flagged
 * by the top bit (peer ids are small and nonzero). */
#define DART_DEST_GROUP_BIT      0x80000000u
#define DART_DEST_GROUP(ch)      (DART_DEST_GROUP_BIT | (uint32_t)(ch))
#define DART_DEST_IS_GROUP(d)    (((d) & DART_DEST_GROUP_BIT) != 0u)
#define DART_DEST_GROUP_CHAN(d)  ((uint16_t)((d) & 0xFFFFu))

/* Complete sample delivered. Do not call back into dart_* for this state. */
typedef void (*dart_sample_fn)(void *user, uint16_t channel_id, uint32_t from_peer,
                             const void *data, size_t len);

/* Reader permanently skips TUs from a peer: writer superseded them (KEEP_LAST
 * eviction) or best-effort data was lost. first..first+count-1 are TU seqnos.
 * Not called for the initial join position: a late joiner adopting the writer's
 * head has lost nothing it expected. Same reentrancy rule as dart_sample_fn. */
typedef void (*dart_gap_fn)(void *user, uint16_t channel_id, uint32_t from_peer,
                          uint64_t first_tu, uint64_t count);

typedef struct {
    const dart_channel_def *channels;
    uint16_t              n_channels;
    uint16_t              max_peers;
    uint16_t              meta_max_ids; /* largest pub+sub id count accepted in a
                                           peer's interest list; bounds the meta
                                           channel's sample size (2 bytes per id).
                                           0 = 1024. Raised to fit our own table. */
    dart_sample_fn          on_sample;
    dart_gap_fn             on_gap;     /* optional; NULL = no gap reporting */
    void                 *user;
} dart_config;

typedef struct dart_state dart_state;

size_t    dart_required_memory(const dart_config *cfg);
dart_state *dart_init(void *mem, size_t mem_size, const dart_config *cfg);

/* A new peer matches only the meta channel; data channels match as its
 * interest list arrives over it, and rematch on every change (theirs via new
 * lists, ours via dart_set_dir). peer_is_local: 1 if on this host; local
 * subscribers of mcast channels stay unicast, and the group lane engages only
 * with a remote one. */
void      dart_peer_add   (dart_state *st, uint32_t peer_id, int peer_is_local);
void      dart_peer_remove(dart_state *st, uint32_t peer_id);

/* Change our own interest in a channel at runtime. Creates/destroys proxies
 * against every live peer and announces the new list on the meta channel.
 * A (re)subscribe joins like a late joiner: reliable channels replay cached
 * history, nothing is reported as a gap. Returns 0 ok, <0 unknown channel. */
int       dart_set_dir(dart_state *st, uint16_t channel_id, uint8_t dir);

/* Publish a sample to all peers. Returns 0 ok, <0 on error. */
int       dart_send(dart_state *st, uint16_t channel_id, const void *data, size_t len,
                  uint64_t now_us);

/* The channel's qos as stored at init; NULL if the channel is unknown. */
const dart_qos *dart_channel_qos(dart_state *st, uint16_t channel_id);

/* Backpressure signal. 1 if appending a sample on this reliable channel would
 * overwrite history not yet acked by every live reader; 0 otherwise. A writer
 * accepting pressure pumps its loop while this returns 1, then sends anyway
 * after qos.max_block_us (KEEP_LAST eviction is the fallback, never refusal). */
int       dart_send_would_evict(dart_state *st, uint16_t channel_id);

/* Feed a received datagram, tagged with the peer it came from. */
void      dart_on_datagram(dart_state *st, uint32_t from_peer, const void *dg, size_t len,
                         uint64_t now_us);

/* Pull one outgoing datagram (may batch several submessages for one peer).
 * Returns 1 and fills *to_peer/out/out_len, or 0 when nothing is due. Loop until
 * 0. Pass DART_DGRAM_MAX of cap; smaller caps just batch less. */
int       dart_poll_send(dart_state *st, uint32_t *to_peer, void *out, size_t cap,
                       size_t *out_len, uint64_t now_us);

#ifdef __cplusplus
}
#endif
#endif /* DART_TRANSPORT_H */

#ifndef DART_TRANSPORT_SANS_IO
/* ===== dart_node.h ===== */
/* NODE runtime over the dart_transport core: owns the data socket,
 * drives discovery, wires peers into the transport. */
#ifndef DART_NODE_H
#define DART_NODE_H


#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t              domain_id;     /* discovery domain                  */
    uint16_t              data_port;     /* unicast data port; 0 = OS-assigned */
    const char           *disc_group;    /* default "239.255.0.7"             */
    uint16_t              disc_port;     /* default 7400                      */
    uint16_t              mc_port;       /* shared multicast DATA port; default
                                            disc_port+1. Per-channel group is
                                            239.255.<domain&255>.<chan&255>   */
    const char           *mcast_if;      /* interface IP for all multicast;
                                            NULL = auto. "127.0.0.1" keeps a
                                            single-host run off the network.
                                            Pin this on multihomed hosts: the
                                            auto route probe follows whatever
                                            the OS routes 239.x to (VPN, WSL,
                                            docker bridges all candidates)    */
    const dart_discovery_addr *seeds;    /* initial peers: announces are also
                                            unicast here (port 0 = disc_port),
                                            so discovery works where multicast
                                            is filtered or flaky              */
    uint16_t              n_seeds;
    uint32_t              announce_us;   /* default 1s                        */
    uint32_t              timeout_us;    /* default 3.5s                      */
    uint8_t               ttl;           /* multicast TTL, default 1          */
    uint16_t              max_peers;     /* default 16                        */
    uint32_t              so_rcvbuf;     /* data-socket SO_RCVBUF bytes; 0 = OS
                                            default. Bigger absorbs bursts at
                                            the cost of queuing delay         */
    uint32_t              so_sndbuf;     /* data-socket SO_SNDBUF; 0 = default */
    const dart_channel_def *channels;      /* transport channels                */
    uint16_t              n_channels;
    uint16_t              meta_max_ids;  /* largest peer interest list accepted
                                            (pub+sub ids); 0 = 1024. Sizes the
                                            meta channel buffers: tune down on
                                            small targets                      */
    dart_sample_fn          on_sample;     /* sample delivery                   */
    dart_gap_fn             on_gap;        /* optional: permanently skipped TUs */
    void                 *user;
} dart_node_config;

typedef struct dart_node dart_node;

size_t   dart_node_required_memory(const dart_node_config *cfg);
dart_node *dart_node_open(void *mem, size_t mem_size, const dart_node_config *cfg);
int      dart_node_poll(dart_node *n, int timeout_ms);          /* one loop tick   */
int      dart_node_send(dart_node *n, uint16_t channel_id, const void *data, size_t len);
/* Change our interest in a channel at runtime (dart_direction; DART_NONE =
 * inactive). Peers rematch as the change reaches them; a (re)subscribe joins
 * like a late joiner. Returns 0 ok, <0 unknown channel. */
int      dart_node_set_dir(dart_node *n, uint16_t channel_id, uint8_t dir);
/* Cumulative backpressure since open: microseconds dart_node_send waited on
 * slow readers and how many sends waited. Either out-pointer may be NULL. */
void     dart_node_block_stats(dart_node *n, uint64_t *block_us, uint32_t *blocked_sends);
void     dart_node_close(dart_node *n, int send_bye);

#ifdef __cplusplus
}
#endif
#endif /* DART_NODE_H */
#endif /* !DART_TRANSPORT_SANS_IO */

#ifdef DART_DISCOVERY_IMPLEMENTATION
/* ===== dart_discovery.c ===== */
/* sans-IO peer-discovery core. See dart_discovery.h. */
#include <string.h>

#define DART_DISCOVERY_HDR_LEN 43
#define DART_DISCOVERY_FLAG_BYE 0x01

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

static size_t dart_discovery_build(dart_discovery_state *st, int bye, uint8_t *p, size_t cap){
    if (cap < (size_t)DART_DISCOVERY_HDR_LEN + 1u + st->cfg.meta_len) return 0;
    p[0]='u'; p[1]='D'; p[2]='S'; p[3]='C';
    p[4]=(uint8_t)DART_DISCOVERY_PROTO_VERSION;
    p[5]=(uint8_t)(bye ? DART_DISCOVERY_FLAG_BYE : 0);
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
}

size_t dart_discovery_update(dart_discovery_state *st, uint64_t now, void *out, size_t cap){
    uint16_t i;
    if (!st->started){
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
    if (now >= st->next_announce_us){
        st->next_announce_us = now + st->cfg.announce_us;
        return dart_discovery_build(st, 0, (uint8_t *)out, cap);
    }
    return 0;
}

size_t dart_discovery_leave(dart_discovery_state *st, void *out, size_t cap){
    return dart_discovery_build(st, 1, (uint8_t *)out, cap);
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

#ifdef DART_TRANSPORT_IMPLEMENTATION
/* ===== dart_transport.c ===== */
/* sans-IO reliable-UDP transport core. See dart_transport.h. */
#include <string.h>

#define DART_DATA 1
#define DART_HB   2
#define DART_NACK 3
#define DART_GAP  4

#define DART_NACK_WINDOW 32u    /* seqnos covered by one ACKNACK bitmap */
#define DART__NIL 0xFFFFFFFFu

#ifndef DART_HB_SWEEP_US
#define DART_HB_SWEEP_US 25000u /* the timer sweep covers every lane this often */
#endif

/* little-endian pack helpers */
static void dart_w16(uint8_t*p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void dart_w32(uint8_t*p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}
static void dart_w64(uint8_t*p,uint64_t v){int i;for(i=0;i<8;i++)p[i]=(uint8_t)(v>>(8*i));}
static uint16_t dart_r16(const uint8_t*p){return (uint16_t)(p[0]|((uint16_t)p[1]<<8));}
static uint32_t dart_r32(const uint8_t*p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t dart_r64(const uint8_t*p){uint64_t v=0;int i;for(i=0;i<8;i++)v|=((uint64_t)p[i])<<(8*i);return v;}

static void dart_bset(uint8_t*bm,uint32_t i){bm[i>>3]|=(uint8_t)(1u<<(i&7));}
static int  dart_bget(const uint8_t*bm,uint32_t i){return (bm[i>>3]>>(i&7))&1;}

/* internal structures */
typedef struct {
    uint8_t  valid;
    uint64_t base;       /* seqno of frag 0 */
    uint16_t count;      /* frag count      */
    uint32_t len;        /* sample bytes    */
    uint8_t *buf;        /* len bytes        */
} dart_wsample;

typedef struct {        /* writer-side, per (channel,peer) */
    uint8_t  used;
    uint8_t  local;      /* peer lives on this host         */
    uint32_t reader_epoch; /* reader incarnation last seen in an ACKNACK; 0 =
                              none yet. A change means the peer rebuilt its
                              state (e.g. one-sided discovery flap): our acked/
                              sent positions describe a dead reader, so the
                              lane re-joins as if freshly matched. */
    uint64_t sent_upto;  /* next seqno to push as new data  */
    uint64_t acked_upto; /* peer received all TUs < this    */
    /* pending repair request (from ACKNACK) */
    uint8_t  has_nack;
    uint64_t nack_base;
    uint32_t nack_bits;
    /* heartbeat timer */
    uint64_t hb_next_us;
    uint32_t hb_count;
} dart_wproxy;

typedef struct {        /* reader-side, per (channel,peer) */
    uint8_t  used;
    uint8_t  started;       /* accepted any DATA from this writer yet          */
    uint32_t epoch;         /* this incarnation's id, sent in every ACKNACK    */
    uint64_t deliver_upto;  /* base of current sample; all below delivered/skipped */
    uint8_t  asm_active;    /* received >=1 frag of current sample */
    uint16_t asm_count;
    uint32_t asm_len;
    uint8_t *asm_buf;       /* max_sample_bytes */
    uint8_t *frag_bm;       /* ceil(maxfrags/8) */
    uint64_t hb_last;       /* highest seqno writer claims to hold */
    uint8_t  ack_pending;
    uint64_t ack_due_us;
} dart_rproxy;

typedef struct {
    dart_qos    qos;
    uint16_t  id;
    uint16_t  maxfrags;     /* ceil(max_sample_bytes/FRAG) */
    uint8_t   dir;          /* dart_direction */
    uint8_t   mcast;
    uint16_t  nsubs_local;  /* live local  subscribers of our writer */
    uint16_t  nsubs_remote; /* live remote subscribers; >0 = group mode */
    /* writer */
    dart_wsample *hist;       /* [depth] ring */
    uint16_t  hist_head;    /* next slot to overwrite */
    uint64_t  next_seqno;
    uint64_t  first_seqno;  /* lowest seqno still cached */
    uint8_t   have_first;
    /* multicast group lane: new data emitted once for all subscribers */
    uint64_t  mc_sent_upto;
    uint64_t  mc_hb_next_us;
    uint32_t  mc_hb_count;
} dart_channel;

struct dart_state {
    dart_config    cfg;       /* n_channels here includes the meta channel */
    uint32_t    *peer_ids;  /* [max_peers] */
    uint8_t     *peer_used; /* [max_peers] */
    uint8_t     *peer_local;/* [max_peers] */
    /* peer interest over OUR channel table, bit per user channel index; filled
       from meta samples. The proxies plus these bits are the whole stored
       interest: full peer lists are never kept. */
    uint8_t     *peer_pub_bm; /* [max_peers][bmlen] peer publishes channel c  */
    uint8_t     *peer_sub_bm; /* [max_peers][bmlen] peer subscribes channel c */
    uint16_t     bmlen;       /* ceil(user n_channels / 8) */
    uint16_t     meta_ci;     /* channel index of the built-in meta channel */
    dart_channel  *chans;     /* [n_channels] */
    dart_wproxy   *wprox;     /* [n_channels*max_peers] */
    dart_rproxy   *rprox;     /* [n_channels*max_peers] */
    /* active-lane scheduler. A lane is one (channel, peer) proxy pair, or a
       channel's multicast group lane; the event that gives a lane sendable
       work also enqueues it, so poll_send pays for work done, never for idle
       lanes. Lanes queue at most once, grouped per destination so one pop
       drains one datagram's worth. Timer work (heartbeats, delayed acks) has
       no event to ride and is found by an amortized clock-driven sweep. */
    uint32_t    *lane_next;   /* [n_channels*(max_peers+1)] next in dest list */
    uint8_t     *lane_inq;    /* [n_channels*(max_peers+1)] queued flag */
    uint32_t    *dest_head;   /* [max_peers+n_channels] lane list per dest */
    uint32_t    *dest_tail;
    uint8_t     *dest_inq;
    uint32_t    *destq;       /* ring of active destinations */
    uint32_t     destq_head, destq_n;
    uint32_t     sweep;       /* timer-sweep lane cursor */
    uint64_t     sweep_t;     /* clock position the sweep has paid for */
    uint32_t     repoch;      /* reader-epoch counter (starts at 1; 0 = none) */
};

/* bump allocator (shared by required_memory and init) */
typedef struct { uint8_t *base; size_t off; size_t cap; int oom; } dart_bump;
static void *dart_take(dart_bump *b, size_t n, size_t align){
    size_t a = (b->off + (align-1)) & ~(align-1);
    b->off = a + n;
    if (b->base){
        if (b->off > b->cap){ b->oom = 1; return NULL; }
        return b->base + a;
    }
    return NULL; /* sizing mode */
}

static uint16_t dart_maxfrags(uint32_t max_sample_bytes){
    uint32_t f = (max_sample_bytes + DART_FRAG_PAYLOAD - 1) / DART_FRAG_PAYLOAD;
    if (f == 0) f = 1;
    return (uint16_t)f;
}

/* Lay out everything; b->base==NULL means measure only. Returns state ptr.
 * Appends the built-in meta channel after the user table. */
static dart_state *dart_build(dart_bump *b, const dart_config *cfg){
    uint16_t c, p; uint32_t np = cfg->max_peers, ncu = cfg->n_channels, nc = ncu+1u;
    uint16_t bml = (uint16_t)((ncu+7u)/8u);
    uint32_t mids = cfg->meta_max_ids ? cfg->meta_max_ids : 1024u;
    dart_channel_def metadef;
    dart_state *st = (dart_state*)dart_take(b, sizeof(dart_state), 16);
    if (st && b->base) memset(st, 0, sizeof(*st));

    if (mids < 2u*ncu) mids = 2u*ncu;     /* our own list must always fit */
    memset(&metadef, 0, sizeof metadef);
    metadef.channel_id          = DART_CHAN_META;
    metadef.qos.reliability     = DART_RELIABLE;
    metadef.qos.history_depth   = 1;       /* latest interest list wins */
    metadef.qos.join_replay     = 1;       /* joiners get the current list */
    metadef.qos.max_sample_bytes= 4u + 2u*mids;

    { uint32_t nlanes = nc*(np+1u), ndest = np+nc;
      uint32_t *pi = (uint32_t*)dart_take(b, np*sizeof(uint32_t), 8);
      uint8_t  *pu = (uint8_t*) dart_take(b, np*sizeof(uint8_t), 1);
      uint8_t  *pl = (uint8_t*) dart_take(b, np*sizeof(uint8_t), 1);
      uint8_t  *pb = (uint8_t*) dart_take(b, (size_t)np*bml, 1);
      uint8_t  *sb = (uint8_t*) dart_take(b, (size_t)np*bml, 1);
      dart_channel *ch = (dart_channel*)dart_take(b, nc*sizeof(dart_channel), 16);
      dart_wproxy *wp = (dart_wproxy*)dart_take(b, (size_t)nc*np*sizeof(dart_wproxy), 16);
      dart_rproxy *rp = (dart_rproxy*)dart_take(b, (size_t)nc*np*sizeof(dart_rproxy), 16);
      uint32_t *ln = (uint32_t*)dart_take(b, (size_t)nlanes*sizeof(uint32_t), 8);
      uint8_t  *li = (uint8_t*) dart_take(b, (size_t)nlanes, 1);
      uint32_t *dh = (uint32_t*)dart_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint32_t *dt = (uint32_t*)dart_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint8_t  *di = (uint8_t*) dart_take(b, (size_t)ndest, 1);
      uint32_t *dq = (uint32_t*)dart_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      if (st && b->base){
          st->cfg=*cfg; st->peer_ids=pi; st->peer_used=pu; st->peer_local=pl;
          st->peer_pub_bm=pb; st->peer_sub_bm=sb; st->bmlen=bml;
          st->cfg.n_channels=(uint16_t)nc; st->meta_ci=(uint16_t)ncu;
          st->chans=ch; st->wprox=wp; st->rprox=rp; st->repoch=1;
          st->lane_next=ln; st->lane_inq=li;
          st->dest_head=dh; st->dest_tail=dt; st->dest_inq=di; st->destq=dq;
          memset(pu,0,np); memset(pl,0,np);
          memset(pb,0,(size_t)np*bml); memset(sb,0,(size_t)np*bml);
          memset(wp,0,(size_t)nc*np*sizeof(dart_wproxy));
          memset(rp,0,(size_t)nc*np*sizeof(dart_rproxy));
          memset(li,0,nlanes); memset(di,0,ndest);
          memset(dh,0xFF,(size_t)ndest*sizeof(uint32_t));   /* all DART__NIL */
      }
    }

    for (c=0;c<nc;c++){
        const dart_channel_def *def = (c < ncu) ? &cfg->channels[c] : &metadef;
        const dart_qos *q = &def->qos;
        uint16_t depth = q->history_depth ? q->history_depth : 1;
        uint16_t mf = dart_maxfrags(q->max_sample_bytes);
        dart_wsample *hist = (dart_wsample*)dart_take(b, depth*sizeof(dart_wsample), 16);
        uint16_t d;
        if (st && b->base){
            dart_channel *ch = &st->chans[c];
            memset(ch,0,sizeof(*ch));
            ch->qos=*q; ch->id=def->channel_id; ch->maxfrags=mf;
            ch->dir=def->dir; ch->mcast=def->mcast;
            ch->hist=hist; ch->hist_head=0; ch->next_seqno=0; ch->have_first=0;
            memset(hist,0,depth*sizeof(dart_wsample));
        }
        for (d=0; d<depth; d++){
            uint8_t *buf = (uint8_t*)dart_take(b, q->max_sample_bytes, 8);
            if (st && b->base) st->chans[c].hist[d].buf = buf;
        }
        /* reader asm buffers and frag bitmaps, per peer */
        for (p=0;p<np;p++){
            uint8_t *abuf = (uint8_t*)dart_take(b, q->max_sample_bytes, 8);
            uint8_t *fbm  = (uint8_t*)dart_take(b, (mf+7u)/8u, 1);
            if (st && b->base){
                dart_rproxy *r = &st->rprox[(size_t)c*np+p];
                r->asm_buf=abuf; r->frag_bm=fbm;
            }
        }
    }
    return st;
}

size_t dart_required_memory(const dart_config *cfg){
    dart_bump b; memset(&b,0,sizeof b);
    if (!cfg || cfg->n_channels==0 || cfg->max_peers==0) return 0;
    dart_build(&b, cfg);
    return b.off + 16;   /* slack for base alignment */
}

static void dart__meta_publish(dart_state *st);

dart_state *dart_init(void *mem, size_t cap, const dart_config *cfg){
    dart_bump b; dart_state *st; uint16_t i;
    if (!mem || !cfg || cfg->n_channels==0 || cfg->max_peers==0) return NULL;
    for (i=0;i<cfg->n_channels;i++)
        if (cfg->channels[i].channel_id==DART_CHAN_META) return NULL;  /* reserved */
    memset(&b,0,sizeof b);
    b.base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    b.cap  = cap - (size_t)((uint8_t*)b.base - (uint8_t*)mem);
    st = dart_build(&b, cfg);
    if (!st || b.oom) return NULL;
    /* cfg.channels still points at caller memory; only read during init,
       so detach it now. */
    st->cfg.channels = NULL;
    dart__meta_publish(st);    /* late joiners replay this initial list */
    return st;
}

/* helpers */
static int dart_peer_slot(dart_state *st, uint32_t id){
    uint16_t i;
    for (i=0;i<st->cfg.max_peers;i++) if (st->peer_used[i] && st->peer_ids[i]==id) return (int)i;
    return -1;
}
static dart_channel *dart_chan(dart_state *st, uint16_t id, int *idx_out){
    uint16_t i;
    for (i=0;i<st->cfg.n_channels;i++) if (st->chans[i].id==id){ if(idx_out)*idx_out=(int)i; return &st->chans[i]; }
    return NULL;
}

/* active-lane scheduler. Lane index = ci*(max_peers+1)+ps; ps==max_peers is
 * the channel's multicast group lane. Destination = peer slot ps, or
 * max_peers+ci for a group lane. */
static void dart__dest_push(dart_state *st, uint32_t d){
    uint32_t ndest = st->cfg.max_peers + (uint32_t)st->cfg.n_channels, t;
    if (st->dest_inq[d]) return;
    st->dest_inq[d]=1;
    t = st->destq_head + st->destq_n;
    if (t >= ndest) t -= ndest;
    st->destq[t]=d; st->destq_n++;
}

/* enqueue a lane that just got sendable work; idempotent while queued */
static void dart__lane_wake(dart_state *st, uint16_t ci, uint32_t ps){
    uint32_t np=st->cfg.max_peers, lanes=np+1u;
    uint32_t L=(uint32_t)ci*lanes+ps;
    uint32_t d=(ps<np) ? ps : np+(uint32_t)ci;
    if (st->lane_inq[L]) return;
    st->lane_inq[L]=1; st->lane_next[L]=DART__NIL;
    if (st->dest_head[d]==DART__NIL) st->dest_head[d]=L;
    else st->lane_next[st->dest_tail[d]]=L;
    st->dest_tail[d]=L;
    dart__dest_push(st, d);
}

/* unicast join seqno: the head minus qos.join_replay cached samples (reliable
 * only). Best-effort and join_replay 0 start at the head, so a late joiner
 * sees only future samples. */
static uint64_t dart_unicast_join_seqno(const dart_channel *chn){
    uint16_t depth = chn->qos.history_depth ? chn->qos.history_depth : 1;
    uint16_t want = chn->qos.join_replay, k, i;
    uint64_t s = chn->next_seqno;
    if (chn->qos.reliability != DART_RELIABLE || want == 0) return s;
    if (want > depth) want = depth;
    i = chn->hist_head;
    for (k=0; k<want; k++){
        uint16_t j = (uint16_t)(i ? i-1 : depth-1);
        if (!chn->hist[j].valid) break;        /* fewer than want cached */
        s = chn->hist[j].base;
        i = j;
    }
    return s;
}

/* group mode toggled: local subscriber lanes hand over to or resume from the
 * group cursor (reliable readers backfill any seam via NACK). A resuming lane
 * may inherit backlog the group never sent, so wake it. */
static void dart_sync_local_lanes(dart_state *st, uint16_t c, dart_channel *chn){
    uint32_t np=st->cfg.max_peers; uint16_t pj;
    for (pj=0;pj<np;pj++){
        dart_wproxy *lw=&st->wprox[(size_t)c*np+pj];
        if (!lw->used || !lw->local) continue;
        if (lw->sent_upto < chn->mc_sent_upto) lw->sent_upto = chn->mc_sent_upto;
        if (lw->sent_upto < chn->next_seqno) dart__lane_wake(st, c, pj);
    }
}

/* per-channel match/unmatch: create or destroy one proxy, keeping the mcast
 * subscriber accounting and join-seqno rules in one place */
static void dart__match_w(dart_state *st, uint16_t c, uint16_t ps){
    dart_channel *chn=&st->chans[c];
    dart_wproxy *w=&st->wprox[(size_t)c*st->cfg.max_peers+ps];
    memset(w,0,sizeof(*w));
    w->used=1; w->local=st->peer_local[ps];
    if (!chn->mcast){
        w->sent_upto = dart_unicast_join_seqno(chn);
    } else if (w->local){
        chn->nsubs_local++;
        /* local subscriber: unicast lane while no remote subscribers exist,
           else it rides the already joined group */
        w->sent_upto = (chn->nsubs_remote==0)
                       ? dart_unicast_join_seqno(chn) : chn->mc_sent_upto;
    } else {
        if (chn->nsubs_remote==0){
            /* first remote subscriber: group cursor takes over at the head,
               local lanes stop pushing */
            chn->mc_sent_upto = chn->next_seqno;
            dart_sync_local_lanes(st, c, chn);
        }
        chn->nsubs_remote++;
        /* group lane carries new data; this lane is repairs only.
           Reliable-from-join-point: replay only via NACK backfill. */
        w->sent_upto = chn->mc_sent_upto;
    }
    w->acked_upto = w->sent_upto;
    dart__lane_wake(st, c, ps);   /* join replay + first heartbeat */
}
static void dart__unmatch_w(dart_state *st, uint16_t c, uint16_t ps){
    dart_channel *chn=&st->chans[c];
    dart_wproxy *w=&st->wprox[(size_t)c*st->cfg.max_peers+ps];
    if (!w->used) return;
    if (chn->mcast){
        if (w->local){
            if (chn->nsubs_local) chn->nsubs_local--;
        } else if (chn->nsubs_remote){
            chn->nsubs_remote--;
            if (chn->nsubs_remote==0)
                /* last remote subscriber gone: local lanes resume from
                   where the group cursor stopped */
                dart_sync_local_lanes(st, c, chn);
        }
    }
    w->used=0; w->local=0;
}
static void dart__match_r(dart_state *st, uint16_t c, uint16_t ps){
    dart_rproxy *r=&st->rprox[(size_t)c*st->cfg.max_peers+ps];
    uint8_t *abuf=r->asm_buf, *fbm=r->frag_bm;
    memset(r,0,sizeof(*r));
    r->asm_buf=abuf; r->frag_bm=fbm;
    r->epoch=st->repoch++;   /* new incarnation: writers re-join on seeing it */
    r->used=1;       /* started==0: first DATA adopts the writer's position */
}
static void dart__unmatch_r(dart_state *st, uint16_t c, uint16_t ps){
    dart_rproxy *r=&st->rprox[(size_t)c*st->cfg.max_peers+ps];
    r->used=0; r->asm_active=0;
}

/* recompute one (channel, peer) match from our dir and the peer's interest
 * bits; transition only on change so live streams never churn */
static void dart__rematch(dart_state *st, uint16_t c, uint16_t ps){
    dart_channel *chn=&st->chans[c];
    const uint8_t *pb=&st->peer_pub_bm[(size_t)ps*st->bmlen];
    const uint8_t *sb=&st->peer_sub_bm[(size_t)ps*st->bmlen];
    int wuse = (chn->dir==DART_PUBSUB || chn->dir==DART_PUB_ONLY) && dart_bget(sb,c);
    int ruse = (chn->dir==DART_PUBSUB || chn->dir==DART_SUB_ONLY) && dart_bget(pb,c);
    dart_wproxy *w=&st->wprox[(size_t)c*st->cfg.max_peers+ps];
    dart_rproxy *r=&st->rprox[(size_t)c*st->cfg.max_peers+ps];
    if (wuse && !w->used) dart__match_w(st,c,ps);
    else if (!wuse && w->used) dart__unmatch_w(st,c,ps);
    if (ruse && !r->used) dart__match_r(st,c,ps);
    else if (!ruse && r->used) dart__unmatch_r(st,c,ps);
}

void dart_peer_add(dart_state *st, uint32_t id, int peer_is_local){
    uint16_t i; int free=-1; uint32_t np=st->cfg.max_peers;
    if (dart_peer_slot(st,id)>=0) return;
    for (i=0;i<np;i++) if(!st->peer_used[i]){free=(int)i;break;}
    if (free<0) return;
    st->peer_used[free]=1; st->peer_ids[free]=id;
    st->peer_local[free]=(uint8_t)(peer_is_local?1:0);
    memset(&st->peer_pub_bm[(size_t)free*st->bmlen],0,st->bmlen);
    memset(&st->peer_sub_bm[(size_t)free*st->bmlen],0,st->bmlen);
    /* only the meta channel matches up front; everything else waits for the
       peer's interest list to arrive over it */
    dart__match_w(st, st->meta_ci, (uint16_t)free);
    dart__match_r(st, st->meta_ci, (uint16_t)free);
}

void dart_peer_remove(dart_state *st, uint32_t id){
    int s = dart_peer_slot(st,id); uint16_t c;
    if (s<0) return;
    for (c=0;c<st->cfg.n_channels;c++){
        dart__unmatch_w(st,c,(uint16_t)s);
        dart__unmatch_r(st,c,(uint16_t)s);
    }
    st->peer_used[s]=0;
}

/* find cached sample containing seqno; NULL if not cached. Walks newest-first,
 * so the fast path (pushing new data at the head) hits in O(1). */
static dart_wsample *dart_find_sample(dart_channel *ch, uint64_t seqno){
    uint16_t depth = ch->qos.history_depth ? ch->qos.history_depth : 1, k;
    uint16_t i = ch->hist_head;
    for (k=0;k<depth;k++){
        dart_wsample *s;
        i = (uint16_t)(i ? i-1 : depth-1);
        s = &ch->hist[i];
        if (!s->valid) break;                  /* reached the unwritten tail */
        if (seqno >= s->base)
            return (seqno < s->base + s->count) ? s : NULL;
    }
    return NULL;
}

/* append the (already filled) head slot to history and wake the lanes that
 * will carry it */
static void dart__commit(dart_state *st, uint16_t ci, size_t len){
    dart_channel *ch = &st->chans[ci];
    uint16_t depth = ch->qos.history_depth ? ch->qos.history_depth : 1;
    uint16_t count = (uint16_t)((len + DART_FRAG_PAYLOAD - 1) / DART_FRAG_PAYLOAD);
    dart_wsample *slot = &ch->hist[ch->hist_head];
    if (count==0) count=1;
    slot->valid=1; slot->base=ch->next_seqno; slot->count=count; slot->len=(uint32_t)len;
    ch->hist_head = (uint16_t)((ch->hist_head+1) % depth);
    ch->next_seqno += count;
    /* oldest cached: the slot the head now points at once the ring wrapped,
       else slot 0 */
    ch->first_seqno = ch->hist[ch->hist_head].valid ? ch->hist[ch->hist_head].base
                                                    : ch->hist[0].base;
    ch->have_first  = 1;
    if (ch->mcast && ch->nsubs_remote>0)
        dart__lane_wake(st, ci, st->cfg.max_peers);    /* group lane */
    else {
        uint32_t np=st->cfg.max_peers, p;
        for (p=0;p<np;p++)
            if (st->wprox[(size_t)ci*np+p].used) dart__lane_wake(st, ci, p);
    }
}

int dart_send(dart_state *st, uint16_t channel_id, const void *data, size_t len, uint64_t now){
    int ci; dart_channel *ch;
    (void)now;
    if (channel_id == DART_CHAN_META) return -1;     /* internal */
    ch = dart_chan(st, channel_id, &ci);
    if (!ch) return -1;
    if (len > ch->qos.max_sample_bytes) return -2;
    if (ch->dir == DART_SUB_ONLY || ch->dir == DART_NONE) return -3;
    if (len) memcpy(ch->hist[ch->hist_head].buf, data, len);
    dart__commit(st, (uint16_t)ci, len);
    return 0;
}

/* publish our interest list on the meta channel: [npub u16][nsub u16] then LE
 * u16 channel ids, pubs first. Encoded straight into the history slot; with
 * depth 1 the newest list is all any joiner ever replays. */
static void dart__meta_publish(dart_state *st){
    dart_channel *mc=&st->chans[st->meta_ci];
    uint8_t *o=mc->hist[mc->hist_head].buf, *p=o+4;
    uint16_t c; uint32_t np=0, ns=0;
    for (c=0;c<st->meta_ci;c++){
        uint8_t d=st->chans[c].dir;
        if (d==DART_PUBSUB || d==DART_PUB_ONLY){ dart_w16(p,st->chans[c].id); p+=2; np++; }
    }
    for (c=0;c<st->meta_ci;c++){
        uint8_t d=st->chans[c].dir;
        if (d==DART_PUBSUB || d==DART_SUB_ONLY){ dart_w16(p,st->chans[c].id); p+=2; ns++; }
    }
    dart_w16(o,(uint16_t)np); dart_w16(o+2,(uint16_t)ns);
    dart__commit(st, st->meta_ci, 4u + 2u*(np+ns));
}

/* a peer's interest list arrived: refresh its bits, rematch every channel */
static void dart__meta_apply(dart_state *st, int ps, const uint8_t *d, size_t len){
    uint16_t np, ns, c; uint32_t i; const uint8_t *pubs, *subs;
    uint8_t *pb=&st->peer_pub_bm[(size_t)ps*st->bmlen];
    uint8_t *sb=&st->peer_sub_bm[(size_t)ps*st->bmlen];
    if (len < 4) return;
    np=dart_r16(d); ns=dart_r16(d+2);
    if (4u + 2u*((uint32_t)np+ns) > len) return;     /* malformed */
    pubs=d+4; subs=pubs+2u*np;
    memset(pb,0,st->bmlen); memset(sb,0,st->bmlen);
    for (c=0;c<st->meta_ci;c++){
        uint16_t id=st->chans[c].id;
        for (i=0;i<np;i++) if (dart_r16(pubs+2u*i)==id){ dart_bset(pb,c); break; }
        for (i=0;i<ns;i++) if (dart_r16(subs+2u*i)==id){ dart_bset(sb,c); break; }
        dart__rematch(st,c,(uint16_t)ps);
    }
}

int dart_set_dir(dart_state *st, uint16_t channel_id, uint8_t dir){
    int ci; dart_channel *ch; uint16_t p;
    if (channel_id == DART_CHAN_META || dir > DART_NONE) return -1;
    ch = dart_chan(st, channel_id, &ci);
    if (!ch) return -1;
    if (ch->dir == dir) return 0;
    ch->dir = dir;
    for (p=0;p<st->cfg.max_peers;p++)
        if (st->peer_used[p]) dart__rematch(st,(uint16_t)ci,p);
    dart__meta_publish(st);
    return 0;
}

const dart_qos *dart_channel_qos(dart_state *st, uint16_t channel_id){
    dart_channel *ch = dart_chan(st, channel_id, NULL);
    return ch ? &ch->qos : NULL;
}

int dart_send_would_evict(dart_state *st, uint16_t channel_id){
    int ci; dart_channel *ch = dart_chan(st, channel_id, &ci);
    dart_wsample *slot; uint32_t np; uint16_t p;
    if (!ch || ch->qos.reliability != DART_RELIABLE) return 0;
    slot = &ch->hist[ch->hist_head];        /* slot the next send overwrites */
    if (!slot->valid) return 0;
    np = st->cfg.max_peers;
    for (p=0;p<(uint16_t)np;p++){
        dart_wproxy *w=&st->wprox[(size_t)ci*np+p];
        if (w->used && w->acked_upto < slot->base + slot->count) return 1;
    }
    return 0;
}

/* datagram builders (return length) */
static size_t dart_mk_data(uint8_t *o, uint16_t chan, uint64_t seqno, dart_wsample *s,
                         uint16_t frag, const uint8_t *payload, uint16_t plen){
    o[0]=DART_DATA; o[1]=0; dart_w16(o+2,chan);
    dart_w64(o+4,seqno); dart_w16(o+12,frag); dart_w16(o+14,s->count);
    dart_w32(o+16,s->len); dart_w16(o+20,plen);
    memcpy(o+22,payload,plen);
    return 22u+plen;
}
static size_t dart_mk_hb(uint8_t *o, uint16_t chan, uint64_t first, uint64_t last, uint32_t cnt){
    o[0]=DART_HB; o[1]=0; dart_w16(o+2,chan); dart_w64(o+4,first); dart_w64(o+12,last); dart_w32(o+20,cnt);
    return 24;
}
static size_t dart_mk_nack(uint8_t *o, uint16_t chan, uint64_t base, uint16_t nbits, uint32_t bm,
                         uint32_t epoch, uint8_t flags){
    o[0]=DART_NACK; o[1]=flags; dart_w16(o+2,chan); dart_w64(o+4,base); dart_w16(o+12,nbits); dart_w32(o+14,bm);
    dart_w32(o+18,epoch);
    return 22;
}
#define DART_NACKF_UNPOSITIONED 1u   /* reader has not delivered anything yet */
static size_t dart_mk_gap(uint8_t *o, uint16_t chan, uint64_t s, uint64_t e){
    o[0]=DART_GAP; o[1]=0; dart_w16(o+2,chan); dart_w64(o+4,s); dart_w64(o+12,e);
    return 20;
}

/* reader side: handle DATA */
static void dart_reader_data(dart_state *st, int ci, int pslot, const uint8_t *p,
                           uint16_t chan, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    dart_rproxy *r=&st->rprox[(size_t)ci*st->cfg.max_peers+pslot];
    uint64_t seqno=dart_r64(p+4); uint16_t frag=dart_r16(p+12), count=dart_r16(p+14);
    uint32_t slen=dart_r32(p+16); uint16_t plen=dart_r16(p+20);
    uint64_t base = seqno - frag;
    int reliable = (ch->qos.reliability==DART_RELIABLE);

    if (!r->used) return;                               /* not subscribed */
    if (slen > ch->qos.max_sample_bytes) return;        /* malformed */
    if (count==0 || frag>=count) return;
    if (base < r->deliver_upto) return;                 /* old/dup */

    if (base > r->deliver_upto){
        if (reliable && r->started){
            /* out-of-order: a hole exists right now, and this datagram proves
               the writer holds up to seqno. Arm the NACK immediately instead
               of waiting for a heartbeat: a busy writer defers heartbeats, and
               at high rates the ring wraps before one arrives. */
            if (seqno > r->hb_last) r->hb_last = seqno;
            if (!r->ack_pending){       /* keep the oldest due time: arrivals
                                           must not keep postponing the NACK */
                r->ack_pending=1; r->ack_due_us=now + ch->qos.nack_delay_us;
            }
            dart__lane_wake(st,(uint16_t)ci,(uint32_t)pslot);
            return;
        }
        /* first contact (reliable late-join) or best-effort: adopt the writer's
           position instead of waiting for seqnos it no longer holds */
        if (r->started && st->cfg.on_gap)                /* best-effort loss */
            st->cfg.on_gap(st->cfg.user, chan, st->peer_ids[pslot],
                           r->deliver_upto, base - r->deliver_upto);
        r->deliver_upto = base; r->asm_active=0;
    }
    r->started = 1;
    /* base == deliver_upto: current sample */
    if (!r->asm_active){
        r->asm_active=1; r->asm_count=count; r->asm_len=slen;
        memset(r->frag_bm,0,(ch->maxfrags+7u)/8u);
    }
    if (count!=r->asm_count) return;                    /* inconsistent, ignore */
    if (!dart_bget(r->frag_bm,frag)){
        uint32_t off=(uint32_t)frag*DART_FRAG_PAYLOAD;
        if (off+plen<=ch->qos.max_sample_bytes) memcpy(r->asm_buf+off,p+22,plen);
        dart_bset(r->frag_bm,frag);
    }
    /* complete? */
    { uint16_t i; int done=1;
      for (i=0;i<count;i++) if(!dart_bget(r->frag_bm,i)){done=0;break;}
      if (done){
          if (ci==(int)st->meta_ci)
              dart__meta_apply(st, pslot, r->asm_buf, r->asm_len);
          else if (st->cfg.on_sample)
              st->cfg.on_sample(st->cfg.user, chan, st->peer_ids[pslot], r->asm_buf, r->asm_len);
          r->deliver_upto = base + count;
          r->asm_active=0;
      }
    }
    if (reliable){
        r->ack_pending=1; r->ack_due_us=now + ch->qos.nack_delay_us;
        dart__lane_wake(st,(uint16_t)ci,(uint32_t)pslot);
    }
}

static void dart_reader_hb(dart_state *st, int ci, int pslot, const uint8_t *p, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    dart_rproxy *r=&st->rprox[(size_t)ci*st->cfg.max_peers+pslot];
    uint64_t first=dart_r64(p+4), last=dart_r64(p+12);
    if (!r->used) return;
    if (ch->qos.reliability!=DART_RELIABLE) return;
    /* un-started readers adopt no position from heartbeats: the advertised
       first may be a dead predecessor's acked position (one-sided flap). The
       ack armed below carries our epoch; the writer re-joins on seeing it and
       the first pushed DATA sets the start. */
    if (r->started && first > r->deliver_upto){
        if (st->cfg.on_gap && ci!=(int)st->meta_ci)        /* superseded before repair */
            st->cfg.on_gap(st->cfg.user, ch->id, st->peer_ids[pslot],
                           r->deliver_upto, first - r->deliver_upto);
        r->deliver_upto=first; r->asm_active=0;
    }
    r->hb_last=last;
    r->ack_pending=1; r->ack_due_us = now + ch->qos.nack_delay_us;
    dart__lane_wake(st,(uint16_t)ci,(uint32_t)pslot);
}

static void dart_reader_gap(dart_state *st, int ci, int pslot, const uint8_t *p){
    dart_rproxy *r=&st->rprox[(size_t)ci*st->cfg.max_peers+pslot];
    uint64_t e=dart_r64(p+12);    /* gap start at p+4 is implied by deliver_upto */
    if (!r->used) return;
    if (e+1 > r->deliver_upto){
        if (r->started && st->cfg.on_gap && ci!=(int)st->meta_ci)
            st->cfg.on_gap(st->cfg.user, st->chans[ci].id, st->peer_ids[pslot],
                           r->deliver_upto, e+1 - r->deliver_upto);
        r->deliver_upto = e+1; r->asm_active=0;
    }
}

/* writer side: handle ACKNACK */
static void dart_writer_nack(dart_state *st, int ci, int pslot, const uint8_t *p){
    dart_channel *ch=&st->chans[ci];
    dart_wproxy *w=&st->wprox[(size_t)ci*st->cfg.max_peers+pslot];
    uint64_t base=dart_r64(p+4); uint16_t nbits=dart_r16(p+12); uint32_t bm=dart_r32(p+14);
    uint32_t ep=dart_r32(p+18); uint8_t fl=p[1];
    int group_mode;
    if (!w->used) return;
    group_mode = ch->mcast && ch->nsubs_remote>0;
    if (w->reader_epoch != ep){
        if (w->reader_epoch){
            /* the reader is a new incarnation (e.g. the peer dropped us in a
               one-sided discovery flap and re-added): our acked/sent positions
               describe its dead predecessor and would tell it nothing is
               missing. Re-join the lane as if freshly matched; this ack's
               content belongs to a position the lane no longer has. */
            w->sent_upto  = group_mode ? ch->mc_sent_upto : dart_unicast_join_seqno(ch);
            w->acked_upto = w->sent_upto;
            w->has_nack   = 0;
            w->hb_next_us = 0;
            dart__lane_wake(st,(uint16_t)ci,(uint32_t)pslot);
            w->reader_epoch = ep;
            return;
        }
        w->reader_epoch = ep;      /* first contact: lane is already fresh */
    }
    if (fl & DART_NACKF_UNPOSITIONED){
        /* the reader has delivered nothing yet, and un-started readers never
           NACK: any push it missed (e.g. one that raced ahead of the peer
           adding us) is otherwise lost for good. Re-push from the unacked
           edge, which is exactly the join window. */
        if (!group_mode && w->acked_upto < w->sent_upto){
            w->sent_upto = w->acked_upto;
            dart__lane_wake(st,(uint16_t)ci,(uint32_t)pslot);
        }
        return;                    /* no position information to apply */
    }
    if (base > w->acked_upto) w->acked_upto=base;
    if (nbits>0 && bm!=0){
        w->has_nack=1; w->nack_base=base; w->nack_bits=bm;
        dart__lane_wake(st,(uint16_t)ci,(uint32_t)pslot);
    }
}

void dart_on_datagram(dart_state *st, uint32_t from, const void *dg, size_t len, uint64_t now){
    const uint8_t *p=(const uint8_t*)dg; size_t rem=len;
    int ps=dart_peer_slot(st,from);
    if (ps<0) return;
    /* concatenated submessages; each length comes from its fixed header,
       so no container framing is needed */
    while (rem>=4){
        uint8_t type=p[0]; uint16_t chan=dart_r16(p+2);
        size_t sub; int ci;
        switch(type){
            case DART_DATA: if (rem<22) return; sub=22u+(size_t)dart_r16(p+20); break;
            case DART_HB:   sub=24; break;
            case DART_NACK: sub=22; break;
            case DART_GAP:  sub=20; break;
            default: return;             /* unknown type: cannot resync, drop rest */
        }
        if (sub>rem) return;             /* truncated/malformed */
        if (dart_chan(st,chan,&ci)){
            switch(type){
                case DART_DATA: dart_reader_data(st,ci,ps,p,chan,now); break;
                case DART_HB:   dart_reader_hb  (st,ci,ps,p,now); break;
                case DART_NACK: dart_writer_nack(st,ci,ps,p); break;
                case DART_GAP:  dart_reader_gap (st,ci,ps,p); break;
            }
        }
        p+=sub; rem-=sub;
    }
}

/* produce one writer submessage for (ci,pslot) into out if due and it fits in
 * cap; 0 if none. When nothing fits, state is untouched so the same submessage
 * is produced next time. Call repeatedly with a shrinking cap to pack several. */
static size_t dart_writer_emit(dart_state *st, int ci, int pslot, uint8_t *out, size_t cap, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    dart_wproxy *w=&st->wprox[(size_t)ci*st->cfg.max_peers+pslot];
    int reliable=(ch->qos.reliability==DART_RELIABLE);
    /* group mode is active only while a mcast channel has remote subscribers.
       New data and heartbeats then ride the group lane; this per-peer lane only
       answers NACKs with unicast repairs. Local-only subscribers stay unicast. */
    int group_mode = ch->mcast && ch->nsubs_remote>0;
    if (!w->used) return 0;
    if (group_mode && (!reliable || !w->has_nack)) return 0;

    /* 1. repair (reliable only) */
    if (reliable && w->has_nack){
        uint32_t i;
        for (i=0;i<DART_NACK_WINDOW;i++){
            if (w->nack_bits & (1u<<i)){
                uint64_t seqno=w->nack_base+i;
                dart_wsample *s;
                if (seqno>=ch->next_seqno){                 /* nothing there */
                    w->nack_bits &= ~(1u<<i);
                    if (w->nack_bits==0) w->has_nack=0;
                    continue;
                }
                s=dart_find_sample(ch,seqno);
                if (s){
                    uint16_t fi=(uint16_t)(seqno - s->base);
                    uint32_t off=(uint32_t)fi*DART_FRAG_PAYLOAD;
                    uint16_t plen=(uint16_t)((s->len-off)<DART_FRAG_PAYLOAD?(s->len-off):DART_FRAG_PAYLOAD);
                    if (cap < 22u+(size_t)plen) return 0;   /* bit stays set */
                    w->nack_bits &= ~(1u<<i);
                    if (w->nack_bits==0) w->has_nack=0;
                    return dart_mk_data(out,ch->id,seqno,s,fi,s->buf+off,plen);
                } else {
                    /* superseded: GAP the dropped region below the cache, but
                       keep still-cached requested seqnos for repair on the
                       following calls */
                    uint64_t e = (ch->have_first?ch->first_seqno:ch->next_seqno);
                    uint32_t j;
                    if (cap < 20) return 0;
                    if (e>0) e-=1; else e=seqno;
                    if (e<seqno) e=seqno;
                    for (j=0;j<DART_NACK_WINDOW;j++)
                        if (w->nack_base+j <= e) w->nack_bits &= ~(1u<<j);
                    if (w->nack_bits==0) w->has_nack=0;
                    return dart_mk_gap(out,ch->id,w->nack_base,e);
                }
            }
        }
        w->has_nack=0;
    }
    if (group_mode) return 0;   /* group lane owns everything below */

    /* 2. push new data */
    if (w->sent_upto < ch->next_seqno){
        uint64_t seqno=w->sent_upto;
        dart_wsample *s=dart_find_sample(ch,seqno);
        if (s){
            uint16_t fi=(uint16_t)(seqno - s->base);
            uint32_t off=(uint32_t)fi*DART_FRAG_PAYLOAD;
            uint16_t plen=(uint16_t)((s->len-off)<DART_FRAG_PAYLOAD?(s->len-off):DART_FRAG_PAYLOAD);
            if (cap < 22u+(size_t)plen) return 0;
            w->sent_upto++;
            return dart_mk_data(out,ch->id,seqno,s,fi,s->buf+off,plen);
        } else {
            /* fell out of the ring before we sent it: GAP up to first cached */
            uint64_t e=(ch->have_first?ch->first_seqno:ch->next_seqno);
            uint64_t gs=w->sent_upto;
            if (cap < 20) return 0;
            if (e>0) e-=1; else e=ch->next_seqno-1;
            w->sent_upto=(ch->have_first?ch->first_seqno:ch->next_seqno);
            if (e<gs) e=gs;
            return dart_mk_gap(out,ch->id,gs,e);
        }
    }

    /* 3. heartbeat (reliable, when caught up and timer due) */
    if (reliable && now>=w->hb_next_us && ch->next_seqno>0){
        uint64_t first = ch->have_first ? ch->first_seqno : 0;
        if (cap < 24) return 0;
        /* nothing below acked_upto ever needs repair, so advertise from there:
           a fresh reader then adopts the join point instead of NACKing the
           whole cached ring past its join_replay window */
        if (w->acked_upto > first) first = w->acked_upto;
        w->hb_next_us = now + (ch->qos.heartbeat_us?ch->qos.heartbeat_us:100000u);
        w->hb_count++;
        return dart_mk_hb(out,ch->id,first,ch->next_seqno-1,w->hb_count);
    }
    return 0;
}

/* produce a reader ACKNACK for (ci,pslot) if due; 0 if none */
static size_t dart_reader_emit(dart_state *st, int ci, int pslot, uint8_t *out, size_t cap, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    dart_rproxy *r=&st->rprox[(size_t)ci*st->cfg.max_peers+pslot];
    uint64_t base; uint16_t nbits=0; uint32_t bm=0;
    if (!r->used) return 0;
    if (ch->qos.reliability!=DART_RELIABLE) return 0;
    if (cap<22) return 0;
    if (!r->ack_pending || now<r->ack_due_us) return 0;
    r->ack_pending=0;

    if (!r->asm_active){
        base=r->deliver_upto;
        if (!r->started){ nbits=0; bm=0; }   /* no position yet: epoch hello */
        else if (r->deliver_upto<=r->hb_last){
            /* everything in (deliver_upto..hb_last] is missing here, so request
               the whole window: one round-trip repairs a burst loss instead of
               one TU per nack_delay */
            uint64_t miss = r->hb_last - r->deliver_upto + 1;
            nbits = (uint16_t)(miss < DART_NACK_WINDOW ? miss : DART_NACK_WINDOW);
            bm = (nbits >= 32) ? 0xFFFFFFFFu : (uint32_t)((1u<<nbits)-1u);
        }
        else { nbits=0; bm=0; }                                /* caught up */
    } else {
        uint16_t lm=0, i; int found=-1;
        for (i=0;i<r->asm_count;i++) if(!dart_bget(r->frag_bm,i)){found=(int)i;break;}
        if (found<0){ base=r->deliver_upto; nbits=0; bm=0; }
        else {
            lm=(uint16_t)found; base=r->deliver_upto+lm;
            for (i=0;i<DART_NACK_WINDOW && (lm+i)<r->asm_count;i++)
                if(!dart_bget(r->frag_bm,(uint32_t)(lm+i))){ bm|=(1u<<i); }
            { uint32_t rem=(uint32_t)(r->asm_count-lm);
              nbits=(uint16_t)(rem<DART_NACK_WINDOW?rem:DART_NACK_WINDOW); }
        }
    }
    return dart_mk_nack(out,ch->id,base,nbits,bm,r->epoch,
                      r->started ? 0 : (uint8_t)DART_NACKF_UNPOSITIONED);
}

/* multicast writer lane for channel ci: new data once for the whole group,
 * then a channel-level heartbeat (reliable). Same per-call contract as
 * dart_writer_emit. */
static size_t dart_group_emit(dart_state *st, int ci, uint8_t *out, size_t cap, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    if (!ch->mcast || ch->dir==DART_SUB_ONLY || ch->dir==DART_NONE) return 0;
    if (ch->nsubs_remote==0){
        /* no remote subscribers: pin the cursor forward so a future remote
           join never gets a stale replay */
        ch->mc_sent_upto = ch->next_seqno;
        return 0;
    }
    if (ch->mc_sent_upto < ch->next_seqno){
        uint64_t seqno=ch->mc_sent_upto;
        dart_wsample *s=dart_find_sample(ch,seqno);
        if (s){
            uint16_t fi=(uint16_t)(seqno - s->base);
            uint32_t off=(uint32_t)fi*DART_FRAG_PAYLOAD;
            uint16_t plen=(uint16_t)((s->len-off)<DART_FRAG_PAYLOAD?(s->len-off):DART_FRAG_PAYLOAD);
            if (cap < 22u+(size_t)plen) return 0;
            ch->mc_sent_upto++;
            return dart_mk_data(out,ch->id,seqno,s,fi,s->buf+off,plen);
        } else {
            uint64_t e=(ch->have_first?ch->first_seqno:ch->next_seqno);
            uint64_t gs=ch->mc_sent_upto;
            if (cap < 20) return 0;
            if (e>0) e-=1; else e=ch->next_seqno-1;
            ch->mc_sent_upto=(ch->have_first?ch->first_seqno:ch->next_seqno);
            if (e<gs) e=gs;
            return dart_mk_gap(out,ch->id,gs,e);
        }
    }
    if (ch->qos.reliability==DART_RELIABLE && now>=ch->mc_hb_next_us && ch->next_seqno>0){
        if (cap < 24) return 0;
        ch->mc_hb_next_us = now + (ch->qos.heartbeat_us?ch->qos.heartbeat_us:100000u);
        ch->mc_hb_count++;
        return dart_mk_hb(out,ch->id,(ch->have_first?ch->first_seqno:0),ch->next_seqno-1,ch->mc_hb_count);
    }
    return 0;
}

/* event work a popped lane still owes right now (timer-armed work is the
 * sweep's job, so a lane never camps in the queue waiting on a clock) */
static int dart__lane_work(dart_state *st, uint16_t ci, uint32_t ps, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    uint32_t np=st->cfg.max_peers;
    if (ps==np)
        return ch->mcast && ch->nsubs_remote>0 && ch->mc_sent_upto < ch->next_seqno;
    if (!st->peer_used[ps]) return 0;
    { dart_wproxy *w=&st->wprox[(size_t)ci*np+ps];
      dart_rproxy *r=&st->rprox[(size_t)ci*np+ps];
      int group_mode = ch->mcast && ch->nsubs_remote>0;
      if (w->used && w->has_nack) return 1;
      if (w->used && !group_mode && w->sent_upto < ch->next_seqno) return 1;
      if (r->used && ch->qos.reliability==DART_RELIABLE
          && r->ack_pending && now >= r->ack_due_us) return 1;
    }
    return 0;
}

/* clock-driven counterpart of the wake calls: heartbeats and delayed acks have
 * no triggering event, so a cursor walks the lane table at a fixed TIME rate
 * (full coverage every DART_HB_SWEEP_US, independent of poll frequency) and
 * wakes lanes whose timers came due. Read-only checks; cost is bounded by
 * table size per sweep period, not per poll call. */
static void dart__hb_sweep(dart_state *st, uint64_t now){
    uint32_t np=st->cfg.max_peers, lanes=np+1u;
    uint32_t total=(uint32_t)st->cfg.n_channels*lanes, due, k;
    uint64_t span = now - st->sweep_t;
    due = (span >= DART_HB_SWEEP_US) ? total
        : (uint32_t)(span * total / DART_HB_SWEEP_US);
    if (!due) return;              /* sweep_t advances only when lanes are paid */
    st->sweep_t = now;
    for (k=0;k<due;k++){
        uint32_t L=st->sweep, ps=L%lanes;
        uint16_t ci=(uint16_t)(L/lanes);
        dart_channel *ch=&st->chans[ci];
        st->sweep = (st->sweep+1u>=total) ? 0u : st->sweep+1u;
        if (ch->qos.reliability!=DART_RELIABLE || ch->next_seqno==0) continue;
        if (ps==np){
            if (ch->mcast && ch->nsubs_remote>0 && now>=ch->mc_hb_next_us)
                dart__lane_wake(st,ci,ps);
            continue;
        }
        if (!st->peer_used[ps]) continue;
        { dart_wproxy *w=&st->wprox[(size_t)ci*np+ps];
          dart_rproxy *r=&st->rprox[(size_t)ci*np+ps];
          int group_mode = ch->mcast && ch->nsubs_remote>0;
          if ((w->used && !group_mode && now>=w->hb_next_us)
           || (r->used && r->ack_pending && now>=r->ack_due_us))
              dart__lane_wake(st,ci,ps);
        }
    }
}

int dart_poll_send(dart_state *st, uint32_t *to_peer, void *out, size_t cap, size_t *out_len, uint64_t now){
    uint32_t np=st->cfg.max_peers, lanes=np+1u;
    uint32_t ndest = np+(uint32_t)st->cfg.n_channels;
    dart__hb_sweep(st, now);
    while (st->destq_n){
        uint32_t d; size_t off=0;
        d = st->destq[st->destq_head];
        st->destq_head = (st->destq_head+1u>=ndest) ? 0u : st->destq_head+1u;
        st->destq_n--; st->dest_inq[d]=0;
        /* drain this destination's lanes into one datagram */
        while (st->dest_head[d]!=DART__NIL){
            uint32_t L=st->dest_head[d], ps=L%lanes;
            uint16_t ci=(uint16_t)(L/lanes);
            size_t n;
            do {
                if (ps==np) n=dart_group_emit(st,ci,(uint8_t*)out+off,cap-off,now);
                else {
                    /* acks first: they are 18 bytes, one-shot, and carry the
                       NACKs that drive repair. A backlogged writer would
                       otherwise fill every datagram and starve them. */
                    n=dart_reader_emit(st,(int)ci,(int)ps,(uint8_t*)out+off,cap-off,now);
                    if (!n) n=dart_writer_emit(st,(int)ci,(int)ps,(uint8_t*)out+off,cap-off,now);
                }
                off+=n;
            } while (n && off<cap);
            st->dest_head[d]=st->lane_next[L];
            if (dart__lane_work(st,ci,ps,now)){
                /* datagram full mid-lane: rotate the lane to the back of its
                   destination so sibling lanes get the next datagram */
                if (st->dest_head[d]==DART__NIL) st->dest_head[d]=L;
                else {
                    st->lane_next[L]=DART__NIL;
                    st->lane_next[st->dest_tail[d]]=L;
                    st->dest_tail[d]=L;
                }
                break;
            }
            st->lane_inq[L]=0;     /* lane drained */
        }
        if (st->dest_head[d]!=DART__NIL) dart__dest_push(st,d);  /* fair: re-queue at tail */
        if (off){
            *to_peer = (d<np) ? st->peer_ids[d]
                              : DART_DEST_GROUP(st->chans[d-np].id);
            *out_len = off;
            return 1;
        }
        if (st->dest_head[d]!=DART__NIL)
            return 0;    /* work pending but nothing fit: caller's cap too small */
    }
    return 0;
}

#ifndef DART_TRANSPORT_SANS_IO
/* ===== dart_node.c ===== */
/* NODE runtime: owns the data socket, drives discovery, wires
 * peers into the transport. See dart_node.h. */

/* Feature-test macros must precede the first system header in the TU. */
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
  #ifdef _MSC_VER
    #pragma comment(lib, "ws2_32.lib")
  #endif
  typedef SOCKET    dart_sock_t;
  typedef WSAPOLLFD dart_pollfd_t;
  #define DART_BADSOCK   INVALID_SOCKET
  #define DART_CLOSESOCK closesocket
  #define DART_POLL      WSAPoll
  #ifndef SIO_UDP_CONNRESET
  #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
  #endif
  static void dart__net_startup(void){ WSADATA w; WSAStartup(MAKEWORD(2,2), &w); }
  static void dart__net_cleanup(void){ WSACleanup(); }
  static void dart__set_nonblock(dart_sock_t fd){ u_long nb=1; ioctlsocket(fd, FIONBIO, &nb); }
  static int  dart__would_block(void){ return WSAGetLastError()==WSAEWOULDBLOCK; }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <poll.h>
  #include <time.h>
  #include <fcntl.h>
  #include <errno.h>
  typedef int           dart_sock_t;
  typedef struct pollfd dart_pollfd_t;
  #define DART_BADSOCK   (-1)
  #define DART_CLOSESOCK close
  #define DART_POLL      poll
  static void dart__net_startup(void){}
  static void dart__net_cleanup(void){}
  static void dart__set_nonblock(dart_sock_t fd){ int fl=fcntl(fd,F_GETFL,0); if(fl!=-1) fcntl(fd,F_SETFL,fl|O_NONBLOCK); }
  static int  dart__would_block(void){ return errno==EAGAIN || errno==EWOULDBLOCK; }
#endif

static uint64_t dart_now_us(void){
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

typedef struct {
    uint8_t  used;
    uint32_t id;
    uint8_t  ip[16];
    uint8_t  ip_len;
    uint16_t port;     /* peer's advertised transport (data) port */
} dart__nodepeer;

struct dart_node {
    dart_state     *tr;
    dart_discovery_rt     *disc;
    dart_sock_t     fd;       /* unicast transport data socket (also group TX) */
    dart_sock_t     mcfd;     /* multicast data RX socket (DART_BADSOCK if unused) */
    dart__nodepeer *peers;
    uint16_t      max_peers;
    uint16_t      domain;
    uint16_t      mc_port;
    /* datagram consumed from the core but refused by the socket; retried first
     * next poll so it is never lost */
    uint8_t       txhold[DART_DGRAM_MAX];
    size_t        txhold_len;
    uint32_t      txhold_peer;
    /* cumulative backpressure (max_block_us waits in dart_node_send) */
    uint64_t      block_us;
    uint32_t      block_n;
};

/* split node config into discovery + transport sub-configs (callbacks/user
 * installed later by open) */
static void dart__node_cfgs(const dart_node_config *cfg, dart_discovery_rt_config *dc,
                          dart_config *tc, uint16_t *mp_out){
    uint16_t mp = cfg->max_peers ? cfg->max_peers : 16;
    memset(dc, 0, sizeof *dc); memset(tc, 0, sizeof *tc);
    dc->disc.domain_id   = cfg->domain_id;
    dc->disc.data_port   = cfg->data_port;     /* advertised to peers */
    dc->disc.announce_us = cfg->announce_us;   /* 0 uses dart_discovery default */
    dc->disc.timeout_us  = cfg->timeout_us;
    dc->disc.max_peers   = mp;
    dc->group            = cfg->disc_group;
    dc->disc_port        = cfg->disc_port;
    dc->ttl              = cfg->ttl;
    dc->mcast_if         = cfg->mcast_if;
    dc->seeds            = cfg->seeds;
    dc->n_seeds          = cfg->n_seeds;
    tc->channels     = cfg->channels;
    tc->n_channels   = cfg->n_channels;
    tc->max_peers    = mp;
    tc->meta_max_ids = cfg->meta_max_ids;
    tc->on_sample    = cfg->on_sample;
    tc->on_gap       = cfg->on_gap;
    tc->user         = cfg->user;
    if (mp_out) *mp_out = mp;
}

/* A peer address is local iff a route probe to it selects that same address as
 * source: true for 127.0.0.1 and this host's own addresses, never for another machine. */
static int dart__node_is_local_ip(const uint8_t ip[4]){
    dart_sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a; int local = 0;
    if (s==DART_BADSOCK) return 0;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; memcpy(&a.sin_addr.s_addr, ip, 4); a.sin_port = htons(7);
    if (connect(s, (struct sockaddr*)&a, sizeof a)==0){
        struct sockaddr_in loc;
#ifdef _WIN32
        int ll = (int)sizeof loc;
#else
        socklen_t ll = sizeof loc;
#endif
        if (getsockname(s, (struct sockaddr*)&loc, &ll)==0)
            local = (memcmp(&loc.sin_addr.s_addr, ip, 4)==0);
    }
    DART_CLOSESOCK(s);
    return local;
}

static void dart__node_up(void *u, uint32_t id, const dart_discovery_addr *addr,
                        const uint8_t *meta, uint8_t mlen){
    dart_node *n = (dart_node*)u; uint16_t i; int slot = -1;
    for (i=0;i<n->max_peers;i++){
        if (n->peers[i].used && n->peers[i].id==id){      /* address update */
            memcpy(n->peers[i].ip, addr->ip, 16);
            n->peers[i].ip_len = addr->ip_len; n->peers[i].port = addr->port;
            return;
        }
        if (!n->peers[i].used && slot<0) slot=(int)i;
    }
    if (slot<0) return;
    n->peers[slot].used=1; n->peers[slot].id=id;
    memcpy(n->peers[slot].ip, addr->ip, 16);
    n->peers[slot].ip_len=addr->ip_len; n->peers[slot].port=addr->port;
    /* interest arrives over the transport's meta channel, not the announce */
    (void)meta; (void)mlen;
    dart_peer_add(n->tr, id, (addr->ip_len==4) && dart__node_is_local_ip(addr->ip));
}
static void dart__node_down(void *u, uint32_t id){
    dart_node *n=(dart_node*)u; uint16_t i;
    for (i=0;i<n->max_peers;i++) if (n->peers[i].used && n->peers[i].id==id){ n->peers[i].used=0; break; }
    dart_peer_remove(n->tr, id);
}

static int dart__node_find_addr(dart_node *n, const struct sockaddr_in *s){
    uint16_t i, port=ntohs(s->sin_port);
    for (i=0;i<n->max_peers;i++)
        if (n->peers[i].used && n->peers[i].ip_len>=4 && n->peers[i].port==port
            && memcmp(n->peers[i].ip, &s->sin_addr.s_addr, 4)==0) return (int)i;
    return -1;
}
static int dart__node_find_id(dart_node *n, uint32_t id){
    uint16_t i;
    for (i=0;i<n->max_peers;i++) if (n->peers[i].used && n->peers[i].id==id) return (int)i;
    return -1;
}

/* deterministic per-(domain, channel) data multicast group. &0xFF wrap
 * collisions are harmless: the receive path filters by peer table and channel id. */
static uint32_t dart__node_group_addr(uint16_t domain, uint16_t chan){
    uint32_t a = (239u<<24)|(255u<<16)|((uint32_t)(domain&0xFFu)<<8)|(uint32_t)(chan&0xFFu);
    return htonl(a);
}

/* Send one datagram to a peer or multicast group. Returns 1 when the datagram
 * is done with (sent, peer unknown, or hard error), 0 only on a would-block
 * TX-buffer-full condition, where the caller must keep it. */
static int dart__node_tx(dart_node *n, uint32_t to, const uint8_t *buf, size_t len){
    struct sockaddr_in d;
    memset(&d, 0, sizeof d);
    d.sin_family = AF_INET;
    if (DART_DEST_IS_GROUP(to)){
        d.sin_addr.s_addr = dart__node_group_addr(n->domain, DART_DEST_GROUP_CHAN(to));
        d.sin_port = htons(n->mc_port);
    } else {
        int pi = dart__node_find_id(n, to);
        if (pi < 0) return 1;              /* peer vanished */
        memcpy(&d.sin_addr.s_addr, n->peers[pi].ip, 4);
        d.sin_port = htons(n->peers[pi].port);
    }
    if (sendto(n->fd, (const char*)buf, (int)len, 0, (struct sockaddr*)&d, sizeof d) < 0
        && dart__would_block())
        return 0;
    return 1;
}

size_t dart_node_required_memory(const dart_node_config *cfg){
    dart_discovery_rt_config dc; dart_config tc; uint16_t mp;
    size_t node_sz, tbl_sz, disc_sz, tr_sz;
    if (!cfg || cfg->n_channels==0) return 0;
    dart__node_cfgs(cfg,&dc,&tc,&mp);
    node_sz = (sizeof(struct dart_node)+15u)&~(size_t)15u;
    tbl_sz  = ((size_t)mp*sizeof(dart__nodepeer)+15u)&~(size_t)15u;
    disc_sz = (dart_discovery_rt_required_memory(&dc)+15u)&~(size_t)15u;
    tr_sz   = (dart_required_memory(&tc)+15u)&~(size_t)15u;
    return 32u + node_sz + tbl_sz + disc_sz + tr_sz;
}

dart_node *dart_node_open(void *mem, size_t cap, const dart_node_config *cfg){
    dart_discovery_rt_config dc; dart_config tc; uint16_t mp;
    uint8_t *base, *p; size_t node_sz, tbl_sz, disc_sz, tr_sz;
    dart_node *n; dart_sock_t fd; struct sockaddr_in a;
#ifdef _WIN32
    int alen;
#else
    socklen_t alen;
#endif
    if (!mem || !cfg || cfg->n_channels==0) return NULL;
    if (cap < dart_node_required_memory(cfg)) return NULL;
    dart__node_cfgs(cfg,&dc,&tc,&mp);

    dart__net_startup();

    base = (uint8_t*)(((uintptr_t)mem+15u)&~(uintptr_t)15u);
    node_sz = (sizeof(struct dart_node)+15u)&~(size_t)15u;
    tbl_sz  = ((size_t)mp*sizeof(dart__nodepeer)+15u)&~(size_t)15u;
    disc_sz = (dart_discovery_rt_required_memory(&dc)+15u)&~(size_t)15u;
    tr_sz   = (dart_required_memory(&tc)+15u)&~(size_t)15u;

    n=(dart_node*)base; memset(n,0,sizeof *n);
    n->fd = DART_BADSOCK; n->mcfd = DART_BADSOCK; n->max_peers=mp;
    n->domain = cfg->domain_id;
    n->mc_port = cfg->mc_port ? cfg->mc_port
               : (uint16_t)((cfg->disc_port ? cfg->disc_port : 7400) + 1);
    p = base + node_sz;
    n->peers=(dart__nodepeer*)p; memset(n->peers,0,(size_t)mp*sizeof(dart__nodepeer));
    p += tbl_sz;

    n->tr = dart_init(p, tr_sz, &tc);
    if (!n->tr){ dart__net_cleanup(); return NULL; }
    p += tr_sz;

    /* Bind the unicast data socket before opening discovery so we can advertise
       its real port (data_port == 0 gets an OS ephemeral, read back via
       getsockname). No SO_REUSEADDR: a unicast endpoint owns its port
       exclusively, so a port collision fails loudly here. */
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd==DART_BADSOCK){ dart__net_cleanup(); return NULL; }
    memset(&a,0,sizeof a);
    a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_ANY);
    a.sin_port=htons(cfg->data_port);          /* 0 => ephemeral */
    if (bind(fd,(struct sockaddr*)&a,sizeof a)!=0){
        DART_CLOSESOCK(fd); dart__net_cleanup(); return NULL;
    }
    memset(&a,0,sizeof a); alen=(int)sizeof a;
    if (getsockname(fd,(struct sockaddr*)&a,&alen)!=0){
        DART_CLOSESOCK(fd); dart__net_cleanup(); return NULL;
    }
    dart__set_nonblock(fd);   /* never block in recv/send: poll drains the whole
                               RX queue and a full TX buffer never deadlocks */
#ifdef _WIN32
    /* Without this, a bounced send (peer gone, ICMP port unreachable) surfaces
       as WSAECONNRESET on a later recvfrom, injecting one peer's error into the
       shared RX path. Turn the reporting off. */
    { BOOL off = FALSE; DWORD bv = 0;
      WSAIoctl(fd, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &bv, NULL, NULL); }
#endif
    if (cfg->so_rcvbuf){ int v=(int)cfg->so_rcvbuf; setsockopt(fd,SOL_SOCKET,SO_RCVBUF,(const char*)&v,sizeof v); }
    if (cfg->so_sndbuf){ int v=(int)cfg->so_sndbuf; setsockopt(fd,SOL_SOCKET,SO_SNDBUF,(const char*)&v,sizeof v); }
    n->fd=fd;
    dc.disc.data_port = ntohs(a.sin_port);     /* advertise the actual port */

    /* multicast data: group TX goes out the unicast data socket (source port
       still identifies the sender); group RX needs its own socket on the shared
       mc_port, one IGMP join per subscribed channel. */
    { int want_rx=0, want_tx=0; uint16_t i; uint32_t ifip;
      for (i=0;i<cfg->n_channels;i++) if (cfg->channels[i].mcast){
          if (cfg->channels[i].dir!=DART_PUB_ONLY) want_rx=1;
          if (cfg->channels[i].dir!=DART_SUB_ONLY) want_tx=1;
      }
      /* pin data multicast to the same egress interface discovery uses, so the
         source address peers learned from announces matches group data. Otherwise
         multihomed hosts pick per-group interfaces and break peer identification. */
      ifip = !(want_rx||want_tx) ? htonl(INADDR_ANY)
           : cfg->mcast_if       ? inet_addr(cfg->mcast_if)
           : dart_discovery_mcast_if_for(inet_addr(cfg->disc_group?cfg->disc_group:"239.255.0.7"),
                                cfg->disc_port?cfg->disc_port:7400);
      if (want_tx){
          unsigned char mttl = cfg->ttl ? cfg->ttl : 1, mloop = 1;
          setsockopt(n->fd, IPPROTO_IP, IP_MULTICAST_IF,   (const char*)&ifip,  sizeof ifip);
          setsockopt(n->fd, IPPROTO_IP, IP_MULTICAST_TTL,  (const char*)&mttl,  sizeof mttl);
          setsockopt(n->fd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&mloop, sizeof mloop);
      }
      if (want_rx){
          dart_sock_t mfd = socket(AF_INET, SOCK_DGRAM, 0);
          int on=1, mc_ok=(mfd!=DART_BADSOCK);
          unsigned char mloop = 1;
          struct sockaddr_in ma;
          if (mc_ok){
              setsockopt(mfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);
#ifdef SO_REUSEPORT
              setsockopt(mfd, SOL_SOCKET, SO_REUSEPORT, (const char*)&on, sizeof on);
#endif
              memset(&ma,0,sizeof ma);
              ma.sin_family=AF_INET; ma.sin_addr.s_addr=htonl(INADDR_ANY);
              ma.sin_port=htons(n->mc_port);
              if (bind(mfd,(struct sockaddr*)&ma,sizeof ma)!=0) mc_ok=0;
          }
          if (mc_ok){
              /* one join per distinct group: the &0xFF group mapping lets
                 channels share a group, and kernels reject duplicate
                 memberships. Memberships per socket are also OS-capped
                 (often ~20: Linux net.ipv4.igmp_max_memberships), so keep
                 mcast channels few; a failed join fails the open. */
              for (i=0;i<cfg->n_channels;i++)
                  if (cfg->channels[i].mcast && cfg->channels[i].dir!=DART_PUB_ONLY){
                      uint32_t g = dart__node_group_addr(cfg->domain_id, cfg->channels[i].channel_id);
                      uint16_t j; int dup=0;
                      for (j=0;j<i;j++)
                          if (cfg->channels[j].mcast && cfg->channels[j].dir!=DART_PUB_ONLY
                              && dart__node_group_addr(cfg->domain_id, cfg->channels[j].channel_id)==g){
                              dup=1; break;
                          }
                      if (dup) continue;
                      { struct ip_mreq mr; memset(&mr,0,sizeof mr);
                        mr.imr_multiaddr.s_addr=g;
                        mr.imr_interface.s_addr=ifip;
                        if (setsockopt(mfd,IPPROTO_IP,IP_ADD_MEMBERSHIP,(const char*)&mr,sizeof mr)!=0){
                            mc_ok=0; break;
                        } }
                  }
          }
          if (!mc_ok){
              if (mfd!=DART_BADSOCK) DART_CLOSESOCK(mfd);
              DART_CLOSESOCK(fd); n->fd=DART_BADSOCK; dart__net_cleanup(); return NULL;
          }
          setsockopt(mfd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&mloop, sizeof mloop);
          dart__set_nonblock(mfd);
          if (cfg->so_rcvbuf){ int v=(int)cfg->so_rcvbuf; setsockopt(mfd,SOL_SOCKET,SO_RCVBUF,(const char*)&v,sizeof v); }
          n->mcfd=mfd;
      } }

    dc.disc.on_peer_up   = dart__node_up;
    dc.disc.on_peer_down = dart__node_down;
    dc.disc.user         = n;
    n->disc = dart_discovery_rt_open(p, disc_sz, &dc);
    if (!n->disc){
        if (n->mcfd!=DART_BADSOCK){ DART_CLOSESOCK(n->mcfd); n->mcfd=DART_BADSOCK; }
        DART_CLOSESOCK(fd); n->fd=DART_BADSOCK; dart__net_cleanup(); return NULL;
    }
    p += disc_sz;

    return n;
}

/* Max wall-time draining the RX queue (and running on_sample) per poll tick
 * before yielding to discovery. Bounds how long a slow on_sample starves the
 * single-threaded loop, keeping discovery alive so peers never time out. */
#ifndef DART_RX_BUDGET_US
#define DART_RX_BUDGET_US 5000u
#endif

/* Drain one socket's receive queue into the transport until empty or the
 * deadline passes. Draining fully (vs one-per-tick) avoids NACK storms.
 * Sockets are non-blocking, so recvfrom <=0 means empty. */
static void dart__node_drain(dart_node *n, dart_sock_t fd, uint64_t deadline){
    uint8_t buf[DART_DGRAM_MAX];
    for (;;){
        struct sockaddr_in src;
#ifdef _WIN32
        int sl=(int)sizeof src;
#else
        socklen_t sl=sizeof src;
#endif
        int r=(int)recvfrom(fd,(char*)buf,(int)sizeof buf,0,(struct sockaddr*)&src,&sl);
        if (r<0){
            if (dart__would_block()) break;        /* queue empty */
            continue;   /* per-datagram error (e.g. a bounced send surfaces as
                           WSAECONNRESET); datagrams behind it are fine, keep draining */
        }
        if (r>0){
            if (r>=4 && buf[0]=='u' && buf[1]=='D' && buf[2]=='S' && buf[3]=='C'){
                /* unicast announce aimed at our data port: the only address
                   that reaches THIS process when several share the disc port.
                   Hand it to discovery. */
                uint8_t sip[4]; memcpy(sip, &src.sin_addr.s_addr, 4);
                dart_discovery_rt_feed(n->disc, sip, 4, buf, (size_t)r);
            } else {
                int pi=dart__node_find_addr(n,&src);
                if (pi>=0) dart_on_datagram(n->tr, n->peers[pi].id, buf, (size_t)r, dart_now_us());
            }
        }
        if (dart_now_us() >= deadline) break;      /* yield to discovery/send */
    }
}

int dart_node_poll(dart_node *n, int timeout_ms){
    uint8_t buf[DART_DGRAM_MAX]; uint32_t to; size_t ol; uint64_t now;
    dart_pollfd_t pfd[2]; int nf=1;

    dart_discovery_rt_poll(n->disc, 0);                 /* discovery tick (non-blocking) */

    memset(pfd,0,sizeof pfd);
    pfd[0].fd=n->fd; pfd[0].events=POLLIN;
    if (n->mcfd!=DART_BADSOCK){ pfd[1].fd=n->mcfd; pfd[1].events=POLLIN; nf=2; }
    if (DART_POLL(pfd,nf,timeout_ms) > 0){
        uint64_t rx_deadline = dart_now_us() + DART_RX_BUDGET_US;
        if (pfd[0].revents & POLLIN) dart__node_drain(n, n->fd, rx_deadline);
        if (nf==2 && (pfd[1].revents & POLLIN)) dart__node_drain(n, n->mcfd, rx_deadline);
    }

    now=dart_now_us();
    /* a datagram the socket refused last tick was already consumed from the core,
       so dropping it would lose best-effort data. Retry it before pulling new. */
    if (n->txhold_len && dart__node_tx(n, n->txhold_peer, n->txhold, n->txhold_len))
        n->txhold_len = 0;
    if (!n->txhold_len)
        while (dart_poll_send(n->tr,&to,buf,sizeof buf,&ol,now)){
            if (!dart__node_tx(n, to, buf, ol)){
                memcpy(n->txhold, buf, ol);
                n->txhold_len = ol; n->txhold_peer = to;
                break;          /* TX buffer full: yield this tick */
            }
            now=dart_now_us();
        }
    return 0;
}

int dart_node_send(dart_node *n, uint16_t channel_id, const void *data, size_t len){
    /* bounded backpressure (qos.max_block_us): let slow-but-live readers ack
       before un-acked history is overwritten. The wait pumps the node loop, so
       on_sample/on_gap may fire from inside this call. Releases on ack, reader
       death, or deadline; then the send proceeds. max_block_us == 0 never waits. */
    const dart_qos *q = dart_channel_qos(n->tr, channel_id);
    if (q && q->max_block_us && dart_send_would_evict(n->tr, channel_id)){
        uint64_t t0 = dart_now_us(), deadline = t0 + q->max_block_us;
        do {
            dart_node_poll(n, 1);
            if (!dart_send_would_evict(n->tr, channel_id)) break;
        } while (dart_now_us() < deadline);
        n->block_us += dart_now_us() - t0;
        n->block_n++;
    }
    return dart_send(n->tr, channel_id, data, len, dart_now_us());
}

int dart_node_set_dir(dart_node *n, uint16_t channel_id, uint8_t dir){
    return dart_set_dir(n->tr, channel_id, dir);
}

void dart_node_block_stats(dart_node *n, uint64_t *block_us, uint32_t *blocked_sends){
    if (block_us)      *block_us      = n->block_us;
    if (blocked_sends) *blocked_sends = n->block_n;
}

void dart_node_close(dart_node *n, int send_bye){
    if (!n) return;
    if (n->disc) dart_discovery_rt_close(n->disc, send_bye);
    if (n->mcfd != DART_BADSOCK) DART_CLOSESOCK(n->mcfd);
    if (n->fd != DART_BADSOCK) DART_CLOSESOCK(n->fd);
    dart__net_cleanup();
}
#endif /* !DART_TRANSPORT_SANS_IO */
#endif /* DART_TRANSPORT_IMPLEMENTATION */
