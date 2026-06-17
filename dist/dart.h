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

/* peer_up: reachable at addr (re-fires when a known peer's addr/meta changes).
 * peer_down: gone. peer_id is a local handle, stable only while the peer lives.
 * meta is the peer's opaque payload (NULL if none), valid only for the call. */
typedef void (*dart_discovery_peer_up_fn)  (void *user, uint32_t peer_id, const dart_discovery_addr *addr,
                                   const uint8_t *meta, uint16_t meta_len);
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
    const uint8_t *meta;    /* opaque versioned blob; the INITIAL value (dart_discovery_set_meta
                               updates it at runtime). Must stay valid. <= meta_cap */
    uint16_t meta_len;
    uint16_t meta_cap;      /* per-peer meta buffer capacity; 0 => DART_DISCOVERY_META_MAX */
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
/* ===== dart_transport.h ===== */
/* sans-IO reliable-UDP transport core: no socket, clock, or heap. Feed it
 * datagrams + now_us + a peer set; it returns datagrams to send and delivers
 * reassembled messages. RTPS-inspired, not wire-compatible. Layer dart_node.h
 * on top for a socket-owning node. */
#ifndef DART_TRANSPORT_H
#define DART_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DART_FRAG_PAYLOAD
#define DART_FRAG_PAYLOAD 1024u          /* default bytes of message data per fragment */
#endif
/* The UDP fragment size is set PER NODE at init (dart_config.frag_payload) and
 * advertised via discovery, so a receiver reassembles each message at the SOURCE
 * node's size -- a writer always fragments with one size, so its seqno line stays
 * self-consistent (no per-message field on the wire). These two compile bounds
 * frame the runtime range so fixed buffers can be sized; both default to
 * DART_FRAG_PAYLOAD, i.e. no change unless you opt in. MAX sizes the datagram
 * buffers (raise it for jumbo frames / a bigger same-LAN size); MIN sizes the
 * reassembly bitmaps (lower it only if some node uses a smaller size). Every
 * node's frag_payload must lie in [MIN, MAX]. */
#ifndef DART_FRAG_PAYLOAD_MAX
#define DART_FRAG_PAYLOAD_MAX DART_FRAG_PAYLOAD
#endif
#ifndef DART_FRAG_PAYLOAD_MIN
#define DART_FRAG_PAYLOAD_MIN DART_FRAG_PAYLOAD
#endif
#define DART_DGRAM_MAX (DART_FRAG_PAYLOAD_MAX + 40u)   /* + largest header */

#ifndef DART_TOPIC_NAME_MAX
#define DART_TOPIC_NAME_MAX 64u          /* max topic-name bytes on the wire */
#endif

/* Max pub+sub topic count accepted in a peer's interest list; sizes the per-peer
 * alias table. Auto-raised to 2*n_channels; raise (a compile bound) only to accept
 * a peer with more topics. */
#ifndef DART_META_MAX_IDS
#define DART_META_MAX_IDS 256u
#endif

typedef enum { DART_BEST_EFFORT = 0, DART_RELIABLE = 1 } dart_reliability;
/* DART_INACTIVE = declared but off (resources stay allocated; dart_set_role flips it) */
typedef enum { DART_PUBSUB = 0, DART_PUB_ONLY = 1, DART_SUB_ONLY = 2,
               DART_INACTIVE = 3 } dart_role;

/* Every field except reliability is zero-means-default, so a reliable channel is
 * just { .reliability = DART_RELIABLE }. */
typedef struct {
    dart_reliability reliability;
    uint16_t keep_last;          /* recent messages retained for late join / repair. 0 = 1 */
    uint16_t catch_up;           /* recent messages a new subscriber gets at once. 0 = future
                                    only, 1 = latest value. Keep small (bursts at startup) */
    uint32_t max_message_bytes;  /* biggest message. 0 = one fragment, or grow-to-fit with an allocator */
    uint32_t heartbeat_us;       /* reliable: idle-writer ping (repairs a lost final message). 0 = 100ms */
    uint32_t repair_delay_us;    /* reliable: reader's delay before requesting a resend. 0 = 20ms */
    uint32_t backpressure_wait_us;/* reliable: how long a send pauses for a slow reader before
                                    evicting un-acked history. 0 = none (pure KEEP_LAST) */
} dart_qos;

/* A channel (topic). Cross-peer identity is the name (64-bit hash); the LOCAL
 * handle for dart_send / on_message is the channel's index in channels[]. */
typedef struct {
    const char *name;  /* topic name = cross-peer identity. Required, same on every node, <= DART_TOPIC_NAME_MAX */
    dart_qos   qos;
    uint8_t  role;     /* dart_role; 0 = pub+sub */
    uint8_t  multicast;/* 1 = use this topic's multicast group for data (publisher opt-in, reader
                          joins). Repairs/acks stay unicast. Group 239.255.<domain&255>.<id&255> */
} dart_channel_def;

/* dart_poll_send destination: a peer id, or a multicast group flagged by the top
 * bit (low byte = topic identity & 0xFF; node maps it to 239.255.<domain&255>.<sel>). */
#define DART_DEST_GROUP_BIT      0x80000000u
#define DART_DEST_GROUP(sel)     (DART_DEST_GROUP_BIT | (uint32_t)(sel))
#define DART_DEST_IS_GROUP(d)    (((d) & DART_DEST_GROUP_BIT) != 0u)
#define DART_DEST_GROUP_CHAN(d)  ((uint16_t)((d) & 0xFFFFu))

/* A complete message; channel is the local handle. Do not call back into dart_*. */
typedef void (*dart_message_fn)(void *user, uint16_t channel, uint32_t from_peer,
                             const void *data, size_t len);

/* Everything that isn't message delivery, as one notification (optional). The
 * meaningful dart_event fields depend on .kind. */
typedef enum {
    DART_PEER_UP,        /* peer discovered: .peer, .ip/.ip_len/.port (node) */
    DART_PEER_DOWN,      /* peer lost: .peer (node) */
    DART_MSG_LOST,       /* messages skipped: .channel, .peer, .first .. .first+.count-1 */
    DART_MSG_TOO_BIG,    /* a received message exceeded max_message_bytes (.count = its size), skipped */
    DART_NAME_COLLISION  /* a peer's name hashes to ours but differs (.first = identity, .detail = our name), refused */
} dart_event_kind;

typedef struct {
    dart_event_kind kind;
    uint32_t   peer;     /* peer id (0 = n/a) */
    uint16_t   channel;  /* local handle, where applicable */
    uint64_t   first;    /* MSG_LOST: first lost seqno; NAME_COLLISION: identity */
    uint64_t   count;    /* MSG_LOST: # lost; MSG_TOO_BIG: message bytes */
    uint8_t    ip[16];   /* PEER_UP: peer address (network order) */
    uint8_t    ip_len;   /* PEER_UP: 4 or 16; else 0 */
    uint16_t   port;     /* PEER_UP: peer data port */
    const char *detail;  /* short human-readable label */
} dart_event;
typedef void (*dart_event_fn)(void *user, const dart_event *ev);

/* Largest message the wire can carry (65535 fragments, ~64 MB by default). */
#define DART_MESSAGE_MAX (65535u * DART_FRAG_PAYLOAD_MAX)

/* Optional realloc-style hook for growable messages (ptr NULL = alloc, size 0 =
 * free). Set => user channels grow to fit, max_message_bytes may be 0. NULL
 * (default, embedded) => fixed buffers, a bigger message is refused/skipped.
 * Pair with dart_destroy to free what it allocated. */
typedef void *(*dart_alloc_fn)(void *user, void *ptr, size_t size);

typedef struct {
    const dart_channel_def *channels;
    uint16_t              n_channels;
    uint16_t              max_peers;
    uint16_t              frag_payload; /* UDP fragment size this node sends with; 0 =
                                           DART_FRAG_PAYLOAD. Clamped to [MIN, MAX]. */
    dart_message_fn         on_message;
    dart_event_fn           on_event;   /* optional: loss/too-big/name-collision */
    dart_alloc_fn           allocator;  /* optional: set => dynamic message sizing */
    void                 *user;
} dart_config;

typedef struct dart_state dart_state;

size_t    dart_required_memory(const dart_config *cfg);
dart_state *dart_init(void *mem, size_t mem_size, const dart_config *cfg);
/* Free allocator-allocated buffers (dynamic channels). No-op in fixed mode; the
 * arena stays the caller's. The node calls it from close. */
void      dart_destroy(dart_state *st);

/* 64-bit topic identity from a name (FNV-1a): matches topics, derives the group. */
uint64_t  dart_topic_id(const char *name);
uint64_t  dart_channel_identity(const dart_channel_def *def);   /* = dart_topic_id(def->name) */

/* A new peer matches nothing until dart_apply_peer_interest feeds its interest
 * list (carried in its discovery announce). peer_is_local: 1 if on this host.
 * peer_frag: that peer's advertised UDP fragment size (from discovery), used to
 * reassemble its messages; 0 = DART_FRAG_PAYLOAD. Clamped to [MIN, MAX]. */
void      dart_peer_add   (dart_state *st, uint32_t peer_id, int peer_is_local, uint16_t peer_frag);
void      dart_peer_remove(dart_state *st, uint32_t peer_id);
/* Update a peer's advertised UDP fragment size (its announce blob may arrive after
 * first contact). Clamped to [MIN, MAX]; no-op for an unknown peer. */
void      dart_peer_set_frag(dart_state *st, uint32_t peer_id, uint16_t peer_frag);

/* Interest exchange (the node carries these in discovery announces; sans-IO callers
 * disseminate them however they like). dart_build_interest serializes OUR pub/sub
 * set into out ([u16 npub][u16 nsub] then [u16 alias][u8 namelen][name] entries),
 * returning bytes written or 0 if cap is too small; size out via dart_interest_max.
 * dart_apply_peer_interest applies a peer's serialized set, (re)matching channels;
 * it is idempotent. Re-build + re-disseminate after dart_set_role. */
size_t    dart_interest_max(uint16_t n_channels);
size_t    dart_build_interest(dart_state *st, void *out, size_t cap);
void      dart_apply_peer_interest(dart_state *st, uint32_t peer_id, const void *blob, size_t len);

/* Change a channel's role at runtime (rematches peers locally; caller re-advertises
 * interest). A (re)subscribe joins like a late joiner. Returns 0 ok, <0 unknown. */
int       dart_set_role(dart_state *st, uint16_t channel, uint8_t role);

/* Publish a message to all peers. Returns 0 ok, <0 on error. */
int       dart_send(dart_state *st, uint16_t channel, const void *data, size_t len,
                  uint64_t now_us);

/* The channel's qos as stored at init; NULL if unknown. */
const dart_qos *dart_channel_qos(dart_state *st, uint16_t channel);

/* 1 if appending here would overwrite history not yet acked by every reader. A
 * writer pumps while this is 1, then sends anyway after qos.backpressure_wait_us. */
int       dart_send_would_evict(dart_state *st, uint16_t channel);

/* 1 if every live reader has acked all messages on this reliable channel (so a
 * writer may close without truncating). Best-effort/unknown return 1. Wrapped as
 * dart_node_drain. */
int       dart_send_drained(dart_state *st, uint16_t channel);

/* Peers currently matched as readers (subscribers) of this channel. 0 = a publish
 * goes nowhere; a one-shot publisher can poll this before sending. */
int       dart_writer_match_count(dart_state *st, uint16_t channel);

/* Feed a received datagram, tagged with the peer it came from. */
void      dart_on_datagram(dart_state *st, uint32_t from_peer, const void *dg, size_t len,
                         uint64_t now_us);

/* Pull one outgoing datagram (may batch submessages for one peer). Returns 1 and
 * fills *to_peer/out/out_len, or 0 when nothing is due. Loop until 0; pass DART_DGRAM_MAX cap. */
int       dart_poll_send(dart_state *st, uint32_t *to_peer, void *out, size_t cap,
                       size_t *out_len, uint64_t now_us);

#ifdef __cplusplus
}
#endif
#endif /* DART_TRANSPORT_H */

#ifndef DART_TRANSPORT_SANS_IO
/* ===== dart_node.h ===== */
/* NODE runtime over dart_transport: owns the data socket, drives discovery,
 * wires peers into the transport. */
#ifndef DART_NODE_H
#define DART_NODE_H


#ifdef __cplusplus
extern "C" {
#endif

/* Network addressing and sockets; every field is zero-means-default (defaults shown). */
typedef struct {
    uint16_t              data_port;         /* unicast data port; 0 = OS-assigned */
    const char           *discovery_group;   /* "239.255.0.7" */
    uint16_t              discovery_port;    /* 7400 */
    uint16_t              multicast_port;    /* shared multicast data port; discovery_port+1 */
    const char           *multicast_interface;/* interface IP for all multicast; NULL = auto,
                                                "127.0.0.1" = single-host. Pin on multihomed hosts */
    uint8_t               multicast_ttl;     /* hops multicast may travel; 1 */
    const dart_discovery_addr *seed_peers;   /* peers to also unicast announces to (port 0 =
                                                discovery_port), so discovery works without multicast */
    uint16_t              n_seed_peers;
    uint32_t              recv_buffer_bytes; /* data-socket SO_RCVBUF; 0 = OS default */
    uint32_t              send_buffer_bytes; /* data-socket SO_SNDBUF; 0 = OS default */
    uint16_t              fragment_size;     /* UDP payload bytes per fragment this node sends;
                                                0 = DART_FRAG_PAYLOAD. Advertised via discovery so
                                                peers reassemble at our size. Clamp [MIN, MAX]; raise
                                                MAX (compile) for jumbo frames. One size per node. */
} dart_node_net;

/* Discovery cadence and peer-table size; zero-means-default (defaults shown). */
typedef struct {
    uint32_t              announce_interval_us; /* "I'm here" broadcast period; 1s */
    uint32_t              peer_timeout_us;   /* drop a peer after this silence; 3.5s */
    uint16_t              max_peers;         /* peer-table capacity; 16 */
} dart_node_discovery;

/* The top fields are what most nodes set; the two sub-structs default whole when
 * zero-initialized:
 *   dart_node_config cfg = {
 *       .domain = 7, .channels = ch, .n_channels = 2, .on_message = on_message };
 */
typedef struct {
    uint16_t                domain;        /* logical-network selector */
    const dart_channel_def *channels;
    uint16_t                n_channels;
    dart_message_fn         on_message;
    dart_event_fn           on_event;      /* optional: loss/too-big/collision/peer up/down */
    void                   *user_data;     /* passed to every callback */
    dart_alloc_fn           allocator;     /* optional: set => dynamic message sizing */
    dart_node_net           net;           /* addressing/sockets (optional) */
    dart_node_discovery     discovery;     /* discovery cadence (optional) */
} dart_node_config;

typedef struct dart_node dart_node;

size_t   dart_node_required_memory(const dart_node_config *cfg);
dart_node *dart_node_open(void *mem, size_t mem_size, const dart_node_config *cfg);
int      dart_node_poll(dart_node *n, int timeout_ms);          /* one loop tick */
int      dart_node_send(dart_node *n, uint16_t channel, const void *data, size_t len);
/* Change a channel's role at runtime (DART_INACTIVE = off). Returns 0 ok, <0 unknown. */
int      dart_node_set_role(dart_node *n, uint16_t channel, uint8_t role);
/* Cumulative backpressure since open: us waited on slow readers and how many sends
 * waited. Either out-pointer may be NULL. */
void     dart_node_backpressure_stats(dart_node *n, uint64_t *waited_us, uint32_t *waited_sends);
/* Pump until every reader has acked all messages on channel, or timeout_ms elapses.
 * Returns 1 if drained, 0 on timeout. Call before close so a burst isn't cut by the BYE. */
int      dart_node_drain(dart_node *n, uint16_t channel, int timeout_ms);
/* Subscribers matched on this channel now; a one-shot publisher polls it before sending. */
int      dart_node_writer_match_count(dart_node *n, uint16_t channel);
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
#define DART_DISCOVERY_FLAG_REQ 0x02         /* solicit: recipients announce back now */
#define DART_DISCOVERY_BLOB_RESEND 3u        /* announces that carry the full blob after a change */

struct dart_discovery_peer_ {
    uint8_t  used;
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
    dart_discovery_addr addr; int idx, addr_changed, first_contact=0, blob_changed=0;
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
            if (st->cfg.on_peer_down) st->cfg.on_peer_down(st->cfg.user, lid);
        }
        return;
    }

    if (idx < 0){
        uint8_t *keep_meta;
        idx = dart_discovery_alloc(st);
        if (idx < 0) return;
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

    if ((first_contact || addr_changed || blob_changed) && st->cfg.on_peer_up)
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
        if (!st->peers[i].used) continue;
        if (now - st->peers[i].last_heard_us > st->cfg.timeout_us){
            uint32_t lid = st->peers[i].local_id;
            st->peers[i].used = 0;
            if (st->cfg.on_peer_down) st->cfg.on_peer_down(st->cfg.user, lid);
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
    uint16_t i, c = 0;
    for (i=0;i<st->cap_peers;i++) if (st->peers[i].used) c++;
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

#ifdef DART_TRANSPORT_IMPLEMENTATION
/* ===== dart_transport.c ===== */
/* sans-IO reliable-UDP transport core. See dart_transport.h. */
#include <string.h>

/* byte 0 of every submessage: type in the low 3 bits, flags above */
#define DART_DATA 1
#define DART_HB   2
#define DART_NACK 3
#define DART_MSG_MASK 0x07u
#define DART_F_SINGLE 0x08u     /* DATA: single fragment (frag/count/len omitted) */
#define DART_F_UNPOS  0x10u     /* NACK: reader has delivered nothing yet */

#define DART_NACK_WINDOW 32u    /* seqnos covered by one ACKNACK bitmap */
#define DART_HELLO_TRIES 4u     /* (re)subscribe announces, bounded against loss */
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

/* FNV-1a over n bytes: the interest blob carries length-prefixed (not NUL-term)
 * names, so the identity is recomputed from the name on receive (it was redundant
 * on the wire, == dart_topic_id of the same name; init caps names at
 * DART_TOPIC_NAME_MAX so the wire name is the whole name). */
static uint64_t dart__id_n(const uint8_t *name, size_t n){
    uint64_t h = 1469598103934665603ull; size_t i;
    for (i=0;i<n;i++){ h ^= (uint64_t)name[i]; h *= 1099511628211ull; }
    return h;
}
uint64_t dart_topic_id(const char *name){
    uint64_t h = 1469598103934665603ull;   /* FNV-1a 64 offset basis */
    const unsigned char *p = (const unsigned char*)name;
    if (!name) return 0;
    for (; *p; p++){ h ^= (uint64_t)*p; h *= 1099511628211ull; }
    return h;
}
uint64_t dart_channel_identity(const dart_channel_def *def){
    return dart_topic_id(def->name);   /* the name is the cross-peer identity */
}
static size_t dart__namelen(const char *s){            /* capped strlen */
    size_t n = 0;
    if (s) while (s[n] && n < DART_TOPIC_NAME_MAX) n++;
    return n;
}

typedef struct {
    uint8_t  valid;
    uint64_t base;       /* seqno of frag 0 */
    uint16_t count;      /* frag count */
    uint32_t len;        /* message bytes */
    uint8_t *buf;        /* >= len bytes; arena (fixed) or hook-malloc'd (dynamic) */
    uint32_t cap;        /* allocated bytes of buf (dynamic grows it) */
} dart_wsample;

typedef struct {        /* writer-side, per (channel,peer) */
    uint8_t  used;
    uint32_t reader_epoch; /* reader incarnation from last ACKNACK (0 = none); a change
                              means the peer rebuilt state, so the lane re-joins */
    uint64_t sent_upto;  /* next seqno to push as new data */
    uint64_t acked_upto; /* peer received all TUs < this */
    uint8_t  has_nack;   /* pending repair request from ACKNACK */
    uint64_t nack_base;
    uint32_t nack_bits;
    uint64_t hb_next_us; /* heartbeat timer */
    uint32_t hb_count;
} dart_wproxy;

typedef struct {        /* reader-side, per (channel,peer) */
    uint8_t  used;
    uint8_t  started;       /* accepted any DATA from this writer yet */
    uint32_t epoch;         /* this incarnation's id, sent in every ACKNACK */
    uint64_t deliver_upto;  /* base of current sample; all below delivered/skipped */
    uint8_t  asm_active;    /* received >=1 frag of current sample */
    uint16_t asm_count;
    uint32_t asm_len;
    uint8_t *asm_buf;       /* >= asm_len; arena (fixed) or hook-malloc'd (dynamic) */
    uint8_t *frag_bm;       /* ceil(asm_count/8) */
    uint32_t asm_cap;       /* allocated bytes of asm_buf (dynamic grows it) */
    uint32_t bm_cap;        /* allocated bytes of frag_bm */
    uint64_t hb_last;       /* highest seqno writer claims to hold */
    uint8_t  ack_pending;
    uint64_t ack_due_us;
    uint8_t  hello_left;    /* (re)subscribe announces still owed: a bounded burst that
                               tells a caught-up writer about this new incarnation so it
                               re-joins, since it no longer idle-pings us to ask */
} dart_rproxy;

typedef struct {
    dart_qos    qos;
    uint64_t  identity;     /* cross-peer topic identity (hash of name) */
    const char *name;       /* our copy of the topic name */
    uint16_t  maxfrags;     /* ceil(max_message_bytes/FRAG) (fixed mode only) */
    uint8_t   role;         /* dart_role */
    uint8_t   multicast;
    uint8_t   dynamic;      /* 1 = buffers grow via cfg.allocator, no fixed cap */
    uint16_t  nsubs;        /* live matched subscribers; multicast: >0 = group mode */
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
    dart_config    cfg;       /* n_channels = user channels (no internal channel) */
    uint32_t    *peer_ids;  /* [max_peers] */
    uint8_t     *peer_used; /* [max_peers] */
    uint8_t     *peer_local;/* [max_peers] */
    uint16_t    *peer_frag; /* [max_peers] each peer's advertised fragment size (writer side) */
    uint16_t     frag;      /* this node's fragment size: what we fragment our sends into */
    /* peer interest over OUR channel table, one bit per user channel; the proxies
       plus these bits are the whole stored interest (full peer lists are not kept).
       Fed by dart_apply_peer_interest from the peer's discovery announce. */
    uint8_t     *peer_pub_bm; /* [max_peers][bmlen] peer publishes channel c */
    uint8_t     *peer_sub_bm; /* [max_peers][bmlen] peer subscribes channel c */
    uint16_t     bmlen;       /* ceil(n_channels / 8) */
    /* per-peer wire alias -> our channel index; the data path carries the 2-byte
       alias instead of the topic name */
    uint16_t    *alias_ci;    /* [max_peers * amax]; 0xFFFF = unmapped */
    uint32_t     amax;        /* alias-table stride = effective meta_max_ids */
    dart_channel  *chans;     /* [n_channels] */
    dart_wproxy   *wprox;     /* [n_channels*max_peers] */
    dart_rproxy   *rprox;     /* [n_channels*max_peers] */
    /* active-lane scheduler: a lane is one (channel,peer) pair or a channel's group
       lane. The event that gives a lane work enqueues it, so poll_send pays for work
       done, not idle lanes. Timer work is found by an amortized clock-driven sweep. */
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

/* Reader-side fragment-count bound: a peer may fragment at the smallest size in
 * the deployment, so size the reassembly bitmap by DART_FRAG_PAYLOAD_MIN. */
static uint16_t dart_maxfrags(uint32_t max_message_bytes){
    uint32_t f = (max_message_bytes + DART_FRAG_PAYLOAD_MIN - 1) / DART_FRAG_PAYLOAD_MIN;
    if (f == 0) f = 1;
    return (uint16_t)f;
}

/* zero-means-default for the tunable QoS fields, applied once at init so the
 * stored qos is authoritative */
#define DART_QOS_DEF_KEEP_LAST    1u
#define DART_QOS_DEF_HEARTBEAT_US 100000u   /* 100 ms idle writer heartbeat */
#define DART_QOS_DEF_REPAIR_US    20000u    /* 20 ms reader repair-request delay */
static void dart__qos_defaults(dart_qos *q, int dynamic){
    if (q->keep_last == 0)        q->keep_last       = DART_QOS_DEF_KEEP_LAST;
    if (q->heartbeat_us == 0)     q->heartbeat_us    = DART_QOS_DEF_HEARTBEAT_US;
    if (q->repair_delay_us == 0)  q->repair_delay_us = DART_QOS_DEF_REPAIR_US;
    /* fixed mode only: a dynamic channel keeps 0 = grow-to-fit via allocator */
    if (!dynamic && q->max_message_bytes == 0) q->max_message_bytes = DART_FRAG_PAYLOAD;
}

/* lay out everything (b->base==NULL = measure only) */
static dart_state *dart_build(dart_bump *b, const dart_config *cfg){
    uint16_t c, p; uint32_t np = cfg->max_peers, nc = cfg->n_channels;
    uint16_t bml = (uint16_t)((nc+7u)/8u);
    uint32_t mids = DART_META_MAX_IDS;
    uint32_t name_bytes = 0; char *npool = NULL; uint32_t ncur = 0;
    dart_state *st = (dart_state*)dart_take(b, sizeof(dart_state), 16);
    if (st && b->base) memset(st, 0, sizeof(*st));

    /* name pool (our copies of the topic names) */
    for (c=0;c<nc;c++){
        size_t L = dart__namelen(cfg->channels[c].name);
        if (L) name_bytes += (uint32_t)L + 1u;
    }
    if (mids < 2u*nc) mids = 2u*nc;     /* our own interest list must always fit */

    { uint32_t nlanes = nc*(np+1u), ndest = np+nc;
      uint32_t *pi = (uint32_t*)dart_take(b, np*sizeof(uint32_t), 8);
      uint8_t  *pu = (uint8_t*) dart_take(b, np*sizeof(uint8_t), 1);
      uint8_t  *pl = (uint8_t*) dart_take(b, np*sizeof(uint8_t), 1);
      uint16_t *pf = (uint16_t*)dart_take(b, np*sizeof(uint16_t), 2);
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
      uint16_t *ac = (uint16_t*)dart_take(b, (size_t)np*mids*sizeof(uint16_t), 2);
      npool = (char*)dart_take(b, name_bytes ? name_bytes : 1u, 1);
      if (st && b->base){
          st->cfg=*cfg; st->peer_ids=pi; st->peer_used=pu; st->peer_local=pl;
          st->peer_frag=pf;
          st->frag = cfg->frag_payload ? cfg->frag_payload : DART_FRAG_PAYLOAD;
          if (st->frag < DART_FRAG_PAYLOAD_MIN) st->frag = DART_FRAG_PAYLOAD_MIN;
          if (st->frag > DART_FRAG_PAYLOAD_MAX) st->frag = DART_FRAG_PAYLOAD_MAX;
          st->peer_pub_bm=pb; st->peer_sub_bm=sb; st->bmlen=bml;
          st->chans=ch; st->wprox=wp; st->rprox=rp; st->repoch=1;
          st->lane_next=ln; st->lane_inq=li;
          st->dest_head=dh; st->dest_tail=dt; st->dest_inq=di; st->destq=dq;
          st->alias_ci=ac; st->amax=mids;
          memset(pu,0,np); memset(pl,0,np);
          { uint32_t k; for (k=0;k<np;k++) pf[k]=DART_FRAG_PAYLOAD; }  /* set per peer on add */
          memset(ac,0xFF,(size_t)np*mids*sizeof(uint16_t));   /* all unmapped */
          memset(pb,0,(size_t)np*bml); memset(sb,0,(size_t)np*bml);
          memset(wp,0,(size_t)nc*np*sizeof(dart_wproxy));
          memset(rp,0,(size_t)nc*np*sizeof(dart_rproxy));
          memset(li,0,nlanes); memset(di,0,ndest);
          memset(dh,0xFF,(size_t)ndest*sizeof(uint32_t));   /* all DART__NIL */
      }
    }

    for (c=0;c<nc;c++){
        const dart_channel_def *def = &cfg->channels[c];
        /* dynamic = an allocator is set: buffers grow via the hook, not the arena */
        int dyn = (cfg->allocator != NULL);
        dart_qos q = def->qos;            /* local, normalized copy */
        dart_wsample *hist; uint16_t depth, mf, d;
        dart__qos_defaults(&q, dyn);
        depth = q.keep_last;
        mf = dart_maxfrags(q.max_message_bytes);
        hist = (dart_wsample*)dart_take(b, depth*sizeof(dart_wsample), 16);
        if (st && b->base){
            dart_channel *ch = &st->chans[c];
            size_t L = dart__namelen(def->name);
            memset(ch,0,sizeof(*ch));
            ch->qos=q; ch->maxfrags=mf;
            ch->role=def->role; ch->multicast=def->multicast; ch->dynamic=(uint8_t)dyn;
            ch->identity = dart_channel_identity(def);
            ch->name = NULL;
            if (L){ char *dst = npool + ncur;
                    memcpy(dst, def->name, L); dst[L]='\0';
                    ch->name = dst; ncur += (uint32_t)(L + 1u); }
            ch->hist=hist; ch->hist_head=0; ch->next_seqno=0; ch->have_first=0;
            memset(hist,0,depth*sizeof(dart_wsample));
        }
        for (d=0; d<depth; d++){
            uint8_t *buf = dyn ? NULL : (uint8_t*)dart_take(b, q.max_message_bytes, 8);
            if (st && b->base){ st->chans[c].hist[d].buf = buf;
                                st->chans[c].hist[d].cap = dyn ? 0u : q.max_message_bytes; }
        }
        /* reader asm buffers + frag bitmaps, per peer (skipped when dynamic) */
        for (p=0;p<np;p++){
            uint8_t *abuf = dyn ? NULL : (uint8_t*)dart_take(b, q.max_message_bytes, 8);
            uint8_t *fbm  = dyn ? NULL : (uint8_t*)dart_take(b, (mf+7u)/8u, 1);
            if (st && b->base){
                dart_rproxy *r = &st->rprox[(size_t)c*np+p];
                r->asm_buf=abuf; r->frag_bm=fbm;
                r->asm_cap = dyn ? 0u : q.max_message_bytes;
                r->bm_cap  = dyn ? 0u : (uint32_t)((mf+7u)/8u);
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

dart_state *dart_init(void *mem, size_t cap, const dart_config *cfg){
    dart_bump b; dart_state *st; uint16_t i;
    if (!mem || !cfg || cfg->n_channels==0 || cfg->max_peers==0) return NULL;
    for (i=0;i<cfg->n_channels;i++){
        const dart_channel_def *d = &cfg->channels[i];
        size_t L = 0;
        if (!d->name || !d->name[0]) return NULL;          /* name = identity, required */
        while (d->name[L]) L++;
        if (L > DART_TOPIC_NAME_MAX) return NULL;           /* the wire name is the whole name */
    }
    memset(&b,0,sizeof b);
    b.base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    b.cap  = cap - (size_t)((uint8_t*)b.base - (uint8_t*)mem);
    st = dart_build(&b, cfg);
    if (!st || b.oom) return NULL;
    st->cfg.channels = NULL;   /* only read during init; detach the caller's pointer */
    return st;
}

static int dart_peer_slot(dart_state *st, uint32_t id){
    uint16_t i;
    for (i=0;i<st->cfg.max_peers;i++) if (st->peer_used[i] && st->peer_ids[i]==id) return (int)i;
    return -1;
}
/* the local handle IS the channel's index; out-of-range rejected */
static dart_channel *dart_chan(dart_state *st, uint16_t channel, int *idx_out){
    if (channel >= st->cfg.n_channels) return NULL;
    if (idx_out) *idx_out = (int)channel;
    return &st->chans[channel];
}
/* RX demux: find the local channel for a wire identity */
static dart_channel *dart_chan_by_identity(dart_state *st, uint64_t identity, int *idx_out){
    uint16_t i;
    for (i=0;i<st->cfg.n_channels;i++) if (st->chans[i].identity==identity){ if(idx_out)*idx_out=(int)i; return &st->chans[i]; }
    return NULL;
}

/* fire one dart_event (no-op if no on_event). Transport emits MSG_LOST/TOO_BIG/COLLISION. */
static void dart__event(dart_state *st, dart_event_kind kind, uint16_t channel,
                        uint32_t peer, uint64_t first, uint64_t count, const char *detail){
    dart_event ev;
    if (!st->cfg.on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind=kind; ev.channel=channel; ev.peer=peer; ev.first=first; ev.count=count;
    ev.detail=detail;
    st->cfg.on_event(st->cfg.user, &ev);
}

/* scheduler: lane index = ci*(max_peers+1)+ps (ps==max_peers = group lane);
 * destination = peer slot ps, or max_peers+ci for a group lane */
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

/* unicast join seqno: head minus qos.catch_up cached samples (reliable only) */
static uint64_t dart_unicast_join_seqno(const dart_channel *chn){
    uint16_t depth = chn->qos.keep_last;   /* normalized at init (>=1) */
    uint16_t want = chn->qos.catch_up, k, i;
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

/* match one (channel,peer) proxy: a multicast channel engages its group lane on
 * the first subscriber; per-peer lanes then carry repairs only */
static void dart__match_w(dart_state *st, uint16_t c, uint16_t ps){
    dart_channel *chn=&st->chans[c];
    dart_wproxy *w=&st->wprox[(size_t)c*st->cfg.max_peers+ps];
    memset(w,0,sizeof(*w));
    w->used=1;
    if (!chn->multicast){
        w->sent_upto = dart_unicast_join_seqno(chn);
    } else {
        /* first subscriber engages group mode at the head; reliable-from-join-point,
           later joiners backfill via NACK */
        if (chn->nsubs==0) chn->mc_sent_upto = chn->next_seqno;
        chn->nsubs++;
        w->sent_upto = chn->mc_sent_upto;
    }
    w->acked_upto = w->sent_upto;
    dart__lane_wake(st, c, ps);   /* unicast repair lane primed + ack/hb */
}
static void dart__unmatch_w(dart_state *st, uint16_t c, uint16_t ps){
    dart_channel *chn=&st->chans[c];
    dart_wproxy *w=&st->wprox[(size_t)c*st->cfg.max_peers+ps];
    if (!w->used) return;
    if (chn->multicast && chn->nsubs) chn->nsubs--;
    w->used=0;
}
static void dart__match_r(dart_state *st, uint16_t c, uint16_t ps){
    dart_rproxy *r=&st->rprox[(size_t)c*st->cfg.max_peers+ps];
    uint8_t *abuf=r->asm_buf, *fbm=r->frag_bm;
    uint32_t acap=r->asm_cap, bcap=r->bm_cap;   /* keep grown buffers across rematch */
    memset(r,0,sizeof(*r));
    r->asm_buf=abuf; r->frag_bm=fbm; r->asm_cap=acap; r->bm_cap=bcap;
    r->epoch=st->repoch++;   /* new incarnation: writers re-join on seeing it */
    r->used=1;       /* started==0: first DATA adopts the writer's position */
    /* announce this incarnation so a caught-up (idle, non-pinging) writer re-joins
       and replays; a small bounded burst rides out a lost announce */
    if (st->chans[c].qos.reliability==DART_RELIABLE){
        r->hello_left=(uint8_t)DART_HELLO_TRIES; r->ack_pending=1; r->ack_due_us=0;
        dart__lane_wake(st,c,ps);
    }
}
static void dart__unmatch_r(dart_state *st, uint16_t c, uint16_t ps){
    dart_rproxy *r=&st->rprox[(size_t)c*st->cfg.max_peers+ps];
    r->used=0; r->asm_active=0;
}

/* recompute one (channel,peer) match from our role and the peer's interest bits */
static void dart__rematch(dart_state *st, uint16_t c, uint16_t ps){
    dart_channel *chn=&st->chans[c];
    const uint8_t *pb=&st->peer_pub_bm[(size_t)ps*st->bmlen];
    const uint8_t *sb=&st->peer_sub_bm[(size_t)ps*st->bmlen];
    int wuse = (chn->role==DART_PUBSUB || chn->role==DART_PUB_ONLY) && dart_bget(sb,c);
    int ruse = (chn->role==DART_PUBSUB || chn->role==DART_SUB_ONLY) && dart_bget(pb,c);
    dart_wproxy *w=&st->wprox[(size_t)c*st->cfg.max_peers+ps];
    dart_rproxy *r=&st->rprox[(size_t)c*st->cfg.max_peers+ps];
    if (wuse && !w->used) dart__match_w(st,c,ps);
    else if (!wuse && w->used) dart__unmatch_w(st,c,ps);
    if (ruse && !r->used) dart__match_r(st,c,ps);
    else if (!ruse && r->used) dart__unmatch_r(st,c,ps);
}

static uint16_t dart__clamp_frag(uint16_t f){
    if (f==0) f = DART_FRAG_PAYLOAD;
    if (f < DART_FRAG_PAYLOAD_MIN) f = DART_FRAG_PAYLOAD_MIN;
    if (f > DART_FRAG_PAYLOAD_MAX) f = DART_FRAG_PAYLOAD_MAX;
    return f;
}

void dart_peer_add(dart_state *st, uint32_t id, int peer_is_local, uint16_t peer_frag){
    uint16_t i; int free=-1; uint32_t np=st->cfg.max_peers;
    if (dart_peer_slot(st,id)>=0) return;
    for (i=0;i<np;i++) if(!st->peer_used[i]){free=(int)i;break;}
    if (free<0) return;
    st->peer_used[free]=1; st->peer_ids[free]=id;
    st->peer_local[free]=(uint8_t)(peer_is_local?1:0);
    st->peer_frag[free]=dart__clamp_frag(peer_frag);
    memset(&st->peer_pub_bm[(size_t)free*st->bmlen],0,st->bmlen);
    memset(&st->peer_sub_bm[(size_t)free*st->bmlen],0,st->bmlen);
    memset(&st->alias_ci[(size_t)free*st->amax],0xFF,(size_t)st->amax*sizeof(uint16_t));
    /* nothing matches until dart_apply_peer_interest feeds the peer's interest
       list (carried in its discovery announce) */
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

/* update a peer's advertised fragment size (its blob may arrive after first contact) */
void dart_peer_set_frag(dart_state *st, uint32_t id, uint16_t peer_frag){
    int s = dart_peer_slot(st,id);
    if (s>=0) st->peer_frag[s]=dart__clamp_frag(peer_frag);
}

/* find cached sample containing seqno (newest-first, so pushing new data is O(1)) */
static dart_wsample *dart_find_sample(dart_channel *ch, uint64_t seqno){
    uint16_t depth = ch->qos.keep_last, k;
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

/* append the filled head slot to history and wake the lanes that carry it */
static void dart__commit(dart_state *st, uint16_t ci, size_t len){
    dart_channel *ch = &st->chans[ci];
    uint16_t depth = ch->qos.keep_last;
    uint16_t count = (uint16_t)((len + st->frag - 1) / st->frag);
    dart_wsample *slot = &ch->hist[ch->hist_head];
    if (count==0) count=1;
    slot->valid=1; slot->base=ch->next_seqno; slot->count=count; slot->len=(uint32_t)len;
    ch->hist_head = (uint16_t)((ch->hist_head+1) % depth);
    ch->next_seqno += count;
    /* oldest cached: where the head points once wrapped, else slot 0 */
    ch->first_seqno = ch->hist[ch->hist_head].valid ? ch->hist[ch->hist_head].base
                                                    : ch->hist[0].base;
    ch->have_first  = 1;
    if (ch->multicast && ch->nsubs>0)
        dart__lane_wake(st, ci, st->cfg.max_peers);    /* group lane */
    else {
        uint32_t np=st->cfg.max_peers, p;
        for (p=0;p<np;p++)
            if (st->wprox[(size_t)ci*np+p].used) dart__lane_wake(st, ci, p);
    }
}

int dart_send(dart_state *st, uint16_t channel, const void *data, size_t len, uint64_t now){
    int ci; dart_channel *ch;
    (void)now;
    ch = dart_chan(st, channel, &ci);                /* rejects the internal meta channel */
    if (!ch) return -1;
    if (ch->dynamic){
        dart_wsample *slot = &ch->hist[ch->hist_head];
        size_t need = len ? len : 1u;
        if (len > 65535u*(uint32_t)st->frag) return -2;   /* wire fragment-count cap */
        if ((size_t)slot->cap < need){                    /* grow the slot to fit */
            uint8_t *nb = (uint8_t*)st->cfg.allocator(st->cfg.user, slot->buf, need);
            if (!nb) return -4;                           /* out of memory */
            slot->buf = nb; slot->cap = (uint32_t)need;
        }
    } else if (len > ch->qos.max_message_bytes) return -2;
    if (ch->role == DART_SUB_ONLY || ch->role == DART_INACTIVE) return -3;
    if (len) memcpy(ch->hist[ch->hist_head].buf, data, len);
    dart__commit(st, (uint16_t)ci, len);
    return 0;
}

void dart_destroy(dart_state *st){
    uint16_t c; uint32_t p, np;
    if (!st || !st->cfg.allocator) return;     /* fixed mode: nothing hook-allocated */
    np = st->cfg.max_peers;
    for (c=0;c<st->cfg.n_channels;c++){
        dart_channel *ch=&st->chans[c];
        uint16_t depth, d;
        if (!ch->dynamic) continue;
        depth = ch->qos.keep_last;
        for (d=0; d<depth; d++)
            if (ch->hist[d].buf){ st->cfg.allocator(st->cfg.user, ch->hist[d].buf, 0);
                                  ch->hist[d].buf=NULL; ch->hist[d].cap=0; }
        for (p=0;p<np;p++){
            dart_rproxy *r=&st->rprox[(size_t)c*np+p];
            if (r->asm_buf){ st->cfg.allocator(st->cfg.user, r->asm_buf, 0); r->asm_buf=NULL; r->asm_cap=0; }
            if (r->frag_bm){ st->cfg.allocator(st->cfg.user, r->frag_bm, 0); r->frag_bm=NULL; r->bm_cap=0; }
        }
    }
}

/* one interest entry: [u16 alias][u8 namelen][name]. The name rides along so a
 * hash collision is detected (not cross-wired); the identity is recomputed from it. */
static uint8_t *dart__meta_put(uint8_t *p, uint16_t alias, const dart_channel *ch){
    size_t L = dart__namelen(ch->name);
    dart_w16(p, alias); p += 2;
    *p++ = (uint8_t)L;
    if (L){ memcpy(p, ch->name, L); p += L; }
    return p;
}
static int dart__meta_name_eq(const dart_channel *ch, const uint8_t *name, size_t nlen){
    size_t ours = dart__namelen(ch->name);
    if (nlen != ours) return 0;
    return nlen==0 ? 1 : (memcmp(ch->name, name, nlen)==0);
}
/* match count entries to local channels by identity (recomputed from each name),
 * recording the alias map. Same-identity-different-name is a collision: refused. */
static const uint8_t *dart__meta_scan(dart_state *st, int ps, const uint8_t *p,
                                      uint32_t count, uint8_t *bm){
    uint32_t k;
    for (k=0;k<count;k++){
        uint16_t alias=dart_r16(p); uint32_t nlen=p[2]; const uint8_t *name=p+3; int ci;
        uint64_t id=dart__id_n(name,nlen);
        dart_channel *ch=dart_chan_by_identity(st,id,&ci);
        p = name + nlen;
        if (!ch) continue;                                  /* not ours */
        if (dart__meta_name_eq(ch,name,nlen)){
            dart_bset(bm,(uint32_t)ci);
            if ((uint32_t)alias < st->amax)
                st->alias_ci[(size_t)ps*st->amax + alias] = (uint16_t)ci;
        }
        else
            dart__event(st, DART_NAME_COLLISION, (uint16_t)ci, st->peer_ids[ps],
                        id, 0, ch->name ? ch->name : "");
    }
    return p;
}

/* Upper bound on dart_build_interest output, for sizing the announce buffer: a
 * PUBSUB channel appears in both lists, so 2*n_channels max-length entries. */
size_t dart_interest_max(uint16_t n_channels){
    return 4u + (size_t)(2u+1u+DART_TOPIC_NAME_MAX) * 2u * (size_t)n_channels;
}

/* Serialize our interest into out: [u16 npub][u16 nsub][pub..][sub..], each entry
 * [u16 alias][u8 namelen][name]. Returns bytes written, or 0 if cap is too small.
 * The node carries this in its discovery announce; size out via dart_interest_max. */
size_t dart_build_interest(dart_state *st, void *out, size_t cap){
    uint8_t *o=(uint8_t*)out, *p, *end=o+cap;
    uint16_t c; uint32_t np=0, ns=0;
    if (cap < 4) return 0;
    p=o+4;
    for (c=0;c<st->cfg.n_channels;c++){
        uint8_t d=st->chans[c].role;
        if (d==DART_PUBSUB || d==DART_PUB_ONLY){
            if (p + 3u + dart__namelen(st->chans[c].name) > end) return 0;
            p=dart__meta_put(p,c,&st->chans[c]); np++;
        }
    }
    for (c=0;c<st->cfg.n_channels;c++){
        uint8_t d=st->chans[c].role;
        if (d==DART_PUBSUB || d==DART_SUB_ONLY){
            if (p + 3u + dart__namelen(st->chans[c].name) > end) return 0;
            p=dart__meta_put(p,c,&st->chans[c]); ns++;
        }
    }
    dart_w16(o,(uint16_t)np); dart_w16(o+2,(uint16_t)ns);
    return (size_t)(p - o);
}

/* A peer's interest list arrived (from its discovery announce): refresh its bits
 * and rematch every channel. Idempotent; re-applying re-derives all matches. */
void dart_apply_peer_interest(dart_state *st, uint32_t peer_id, const void *blob, size_t len){
    const uint8_t *d=(const uint8_t*)blob, *p, *end=d+len;
    uint16_t np, ns, c; int ps=dart_peer_slot(st,peer_id);
    uint8_t *pb, *sb;
    if (ps<0 || len<4) return;
    pb=&st->peer_pub_bm[(size_t)ps*st->bmlen];
    sb=&st->peer_sub_bm[(size_t)ps*st->bmlen];
    np=dart_r16(d); ns=dart_r16(d+2);
    /* validate the whole variable-length list first: a truncated blob must not drop a match */
    { uint32_t k, tot=(uint32_t)np+ns; p=d+4;
      for (k=0;k<tot;k++){
          if (p+3 > end) return;
          p += 3u + (uint32_t)p[2];
          if (p > end) return;
      } }
    memset(pb,0,st->bmlen); memset(sb,0,st->bmlen);
    memset(&st->alias_ci[(size_t)ps*st->amax],0xFF,(size_t)st->amax*sizeof(uint16_t));
    p = dart__meta_scan(st, ps, d+4, np, pb);
    p = dart__meta_scan(st, ps, p,   ns, sb);
    for (c=0;c<st->cfg.n_channels;c++) dart__rematch(st,c,(uint16_t)ps);
}

int dart_set_role(dart_state *st, uint16_t channel, uint8_t role){
    int ci; dart_channel *ch; uint16_t p;
    if (role > DART_INACTIVE) return -1;
    ch = dart_chan(st, channel, &ci);
    if (!ch) return -1;
    if (ch->role == role) return 0;
    ch->role = role;
    for (p=0;p<st->cfg.max_peers;p++)
        if (st->peer_used[p]) dart__rematch(st,(uint16_t)ci,p);
    /* caller re-advertises interest (the node bumps its discovery announce) */
    return 0;
}

const dart_qos *dart_channel_qos(dart_state *st, uint16_t channel){
    dart_channel *ch = dart_chan(st, channel, NULL);
    return ch ? &ch->qos : NULL;
}

int dart_send_would_evict(dart_state *st, uint16_t channel){
    int ci; dart_channel *ch = dart_chan(st, channel, &ci);
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

int dart_send_drained(dart_state *st, uint16_t channel){
    int ci; dart_channel *ch = dart_chan(st, channel, &ci);
    uint32_t np; uint16_t p;
    if (!ch || ch->qos.reliability != DART_RELIABLE) return 1;  /* no acks to await */
    np = st->cfg.max_peers;
    for (p=0;p<(uint16_t)np;p++){
        dart_wproxy *w=&st->wprox[(size_t)ci*np+p];
        if (w->used && w->acked_upto < ch->next_seqno) return 0;  /* reader still behind */
    }
    return 1;
}

int dart_writer_match_count(dart_state *st, uint16_t channel){
    int ci; dart_channel *ch = dart_chan(st, channel, &ci);
    uint32_t np, p; int cnt = 0;
    if (!ch) return 0;
    np = st->cfg.max_peers;
    for (p=0;p<np;p++) if (st->wprox[(size_t)ci*np+p].used) cnt++;  /* matched readers */
    return cnt;
}

/* wire alias for a local channel: its own index (the advertiser's handle). The
 * peer mapped this alias to its matching channel from our interest list. */
static uint16_t dart__alias_of(dart_state *st, int ci){
    (void)st; return (uint16_t)ci;
}

/* datagram builders (return length). Byte 0 = type | flags; alias (u16) at o+1.
 * Header sizes: DATA 13 (single) or 21 (multi), HB 23, NACK 21. */
static size_t dart_mk_data(uint8_t *o, uint16_t alias, uint64_t seqno, dart_wsample *s,
                         uint16_t frag, const uint8_t *payload, uint16_t plen){
    dart_w16(o+1,alias);
    if (s->count==1){                       /* frag=0, count=1, len=plen implied */
        o[0]=(uint8_t)(DART_DATA|DART_F_SINGLE);
        dart_w64(o+3,seqno); dart_w16(o+11,plen);
        memcpy(o+13,payload,plen);
        return 13u+plen;
    }
    o[0]=DART_DATA;
    dart_w64(o+3,seqno); dart_w16(o+11,frag); dart_w16(o+13,s->count);
    dart_w32(o+15,s->len); dart_w16(o+19,plen);
    memcpy(o+21,payload,plen);
    return 21u+plen;
}
static size_t dart_mk_hb(uint8_t *o, uint16_t alias, uint64_t first, uint64_t last, uint32_t cnt){
    o[0]=DART_HB; dart_w16(o+1,alias); dart_w64(o+3,first); dart_w64(o+11,last); dart_w32(o+19,cnt);
    return 23;
}
static size_t dart_mk_nack(uint8_t *o, uint16_t alias, uint64_t base, uint16_t nbits, uint32_t bm,
                         uint32_t epoch, uint8_t flags){
    o[0]=(uint8_t)(DART_NACK|flags); dart_w16(o+1,alias); dart_w64(o+3,base);
    dart_w16(o+11,nbits); dart_w32(o+13,bm); dart_w32(o+17,epoch);
    return 21;
}
/* emit an HB advertising this lane's current window. Doubles as the "skip past a
 * hole" signal that replaces GAP: reader_hb advances deliver_upto to `first`, so a
 * superseded NACK or a ring-overrun push answers with an HB whose first = our floor.
 * Resets the idle-HB timer so we don't double-send. */
static size_t dart_writer_hb(dart_channel *ch, dart_wproxy *w, uint16_t alias,
                             uint8_t *out, size_t cap, uint64_t now){
    uint64_t first = ch->have_first ? ch->first_seqno : 0;
    if (cap < 23) return 0;
    if (w->acked_upto > first) first = w->acked_upto;   /* fresh reader adopts join point */
    w->hb_next_us = now + ch->qos.heartbeat_us;
    w->hb_count++;
    return dart_mk_hb(out, alias, first, ch->next_seqno-1, w->hb_count);
}

/* reader side: handle DATA */
static void dart_reader_data(dart_state *st, int ci, int pslot, const uint8_t *p,
                           uint64_t now){
    dart_channel *ch=&st->chans[ci];
    dart_rproxy *r=&st->rprox[(size_t)ci*st->cfg.max_peers+pslot];
    int reliable = (ch->qos.reliability==DART_RELIABLE);
    uint64_t seqno, base; uint16_t frag, count, plen; uint32_t slen; const uint8_t *pay;
    if (p[0] & DART_F_SINGLE){           /* single fragment: frag/count/len implied */
        seqno=dart_r64(p+3); frag=0; count=1; plen=dart_r16(p+11); slen=plen; pay=p+13;
    } else {
        seqno=dart_r64(p+3); frag=dart_r16(p+11); count=dart_r16(p+13);
        slen=dart_r32(p+15); plen=dart_r16(p+19); pay=p+21;
    }
    base = seqno - frag;

    if (!r->used) return;                               /* not subscribed */
    if (count==0 || frag>=count) return;                /* malformed */
    if (base < r->deliver_upto) return;                 /* old/dup */

    if (base > r->deliver_upto){
        if (reliable && r->started){
            /* out-of-order: arm the NACK immediately, don't wait for a heartbeat
               (a busy writer defers HBs and the ring may wrap before one arrives) */
            if (seqno > r->hb_last) r->hb_last = seqno;
            if (!r->ack_pending){       /* keep the oldest due time so arrivals don't postpone it */
                r->ack_pending=1; r->ack_due_us=now + ch->qos.repair_delay_us;
            }
            dart__lane_wake(st,(uint16_t)ci,(uint32_t)pslot);
            return;
        }
        /* first contact or best-effort: adopt the writer's position */
        if (r->started)                                  /* best-effort loss */
            dart__event(st, DART_MSG_LOST, (uint16_t)ci, st->peer_ids[pslot],
                        r->deliver_upto, base - r->deliver_upto, "message(s) lost");
        r->deliver_upto = base; r->asm_active=0;
    }
    r->started = 1; r->hello_left = 0;   /* writer re-engaged: stop the (re)subscribe burst */
    /* fit the reassembly buffers (dynamic grows via the hook, fixed is capped at
       max_message_bytes); "too big" skips the whole sample and reports it */
    { uint32_t bmneed = ((uint32_t)count + 7u) / 8u, bufcap, bmbytes; int toobig = 0;
      if (ch->dynamic){
          if (r->asm_cap < slen){
              uint8_t *nb = (uint8_t*)st->cfg.allocator(st->cfg.user, r->asm_buf, slen?slen:1u);
              if (!nb) toobig = 1; else { r->asm_buf = nb; r->asm_cap = slen?slen:1u; }
          }
          if (!toobig && r->bm_cap < bmneed){
              uint8_t *nbm = (uint8_t*)st->cfg.allocator(st->cfg.user, r->frag_bm, bmneed?bmneed:1u);
              if (!nbm) toobig = 1; else { r->frag_bm = nbm; r->bm_cap = bmneed?bmneed:1u; }
          }
      } else if (slen > ch->qos.max_message_bytes) toobig = 1;
      if (toobig){
          dart__event(st, DART_MSG_TOO_BIG, (uint16_t)ci, st->peer_ids[pslot],
                      0, slen, "message exceeds max_message_bytes");
          r->deliver_upto = base + count; r->asm_active = 0;
          if (reliable){
              r->ack_pending = 1; r->ack_due_us = now + ch->qos.repair_delay_us;
              dart__lane_wake(st, (uint16_t)ci, (uint32_t)pslot);
          }
          return;
      }
      bufcap  = ch->dynamic ? r->asm_cap : ch->qos.max_message_bytes;
      bmbytes = ch->dynamic ? bmneed     : (uint32_t)((ch->maxfrags+7u)/8u);
      /* base == deliver_upto: current sample */
      if (!r->asm_active){
          r->asm_active=1; r->asm_count=count; r->asm_len=slen;
          memset(r->frag_bm,0,bmbytes);
      }
      if (count!=r->asm_count) return;                  /* inconsistent, ignore */
      if (!dart_bget(r->frag_bm,frag)){
          /* reassemble at the SOURCE peer's fragment size (advertised via discovery);
             a peer staying within [MIN, MAX] keeps count <= maxfrags, so the bitmap
             can't overflow and the bufcap guard catches any stray offset */
          uint32_t off=(uint32_t)frag*st->peer_frag[pslot];
          if (off+plen<=bufcap) memcpy(r->asm_buf+off,pay,plen);
          dart_bset(r->frag_bm,frag);
      }
    }
    /* complete? */
    { uint16_t i; int done=1;
      for (i=0;i<count;i++) if(!dart_bget(r->frag_bm,i)){done=0;break;}
      if (done){
          if (st->cfg.on_message)
              st->cfg.on_message(st->cfg.user, (uint16_t)ci, st->peer_ids[pslot], r->asm_buf, r->asm_len);
          r->deliver_upto = base + count;
          r->asm_active=0;
      }
    }
    if (reliable){
        r->ack_pending=1; r->ack_due_us=now + ch->qos.repair_delay_us;
        dart__lane_wake(st,(uint16_t)ci,(uint32_t)pslot);
    }
}

static void dart_reader_hb(dart_state *st, int ci, int pslot, const uint8_t *p, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    dart_rproxy *r=&st->rprox[(size_t)ci*st->cfg.max_peers+pslot];
    uint64_t first=dart_r64(p+3), last=dart_r64(p+11);
    if (!r->used) return;
    if (ch->qos.reliability!=DART_RELIABLE) return;
    /* un-started readers adopt no position from heartbeats (a one-sided flap's
       advertised first may be a dead predecessor's); the ack below carries our epoch */
    if (r->started && first > r->deliver_upto){
        dart__event(st, DART_MSG_LOST, (uint16_t)ci, st->peer_ids[pslot],   /* superseded before repair */
                    r->deliver_upto, first - r->deliver_upto, "message(s) lost");
        r->deliver_upto=first; r->asm_active=0;
    }
    r->hb_last=last;
    r->ack_pending=1; r->ack_due_us = now + ch->qos.repair_delay_us;
    dart__lane_wake(st,(uint16_t)ci,(uint32_t)pslot);
}

/* writer side: handle ACKNACK */
static void dart_writer_nack(dart_state *st, int ci, int pslot, const uint8_t *p){
    dart_channel *ch=&st->chans[ci];
    dart_wproxy *w=&st->wprox[(size_t)ci*st->cfg.max_peers+pslot];
    uint64_t base=dart_r64(p+3); uint16_t nbits=dart_r16(p+11); uint32_t bm=dart_r32(p+13);
    uint32_t ep=dart_r32(p+17); uint8_t fl=p[0];
    int group_mode;
    if (!w->used) return;
    group_mode = ch->multicast && ch->nsubs>0;
    if (w->reader_epoch != ep){
        if (w->reader_epoch){
            /* reader is a new incarnation (one-sided flap): our positions describe its
               dead predecessor, so re-join the lane as if freshly matched */
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
    if (fl & DART_F_UNPOS){
        /* reader has delivered nothing and never NACKs: re-push from the unacked
           edge (the join window) so a push that raced ahead isn't lost */
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
    /* concatenated submessages; each length comes from its header, so no framing */
    while (rem>=3){
        uint8_t b0=p[0], type=(uint8_t)(b0 & DART_MSG_MASK); uint16_t alias; size_t sub; int ci;
        switch(type){
            case DART_DATA: if (b0 & DART_F_SINGLE){ if (rem<13) return; sub=13u+(size_t)dart_r16(p+11); }
                            else { if (rem<21) return; sub=21u+(size_t)dart_r16(p+19); } break;
            case DART_HB:   if (rem<23) return; sub=23; break;
            case DART_NACK: if (rem<21) return; sub=21; break;
            default: return;             /* unknown type: cannot resync, drop rest */
        }
        if (sub>rem) return;             /* truncated/malformed */
        alias = dart_r16(p+1);
        if ((uint32_t)alias < st->amax){
            uint16_t m=st->alias_ci[(size_t)ps*st->amax+alias]; ci=(m==0xFFFFu)?-1:(int)m;
        } else ci=-1;
        if (ci>=0){
            switch(type){
                case DART_DATA: dart_reader_data(st,ci,ps,p,now); break;
                case DART_HB:   dart_reader_hb  (st,ci,ps,p,now); break;
                case DART_NACK: dart_writer_nack(st,ci,ps,p); break;
            }
        }
        p+=sub; rem-=sub;
    }
}

/* produce one writer submessage for (ci,pslot) if due and it fits cap; 0 if none.
 * On no-fit, state is untouched so the same submessage is produced next time. */
static size_t dart_writer_emit(dart_state *st, int ci, int pslot, uint8_t *out, size_t cap, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    dart_wproxy *w=&st->wprox[(size_t)ci*st->cfg.max_peers+pslot];
    int reliable=(ch->qos.reliability==DART_RELIABLE);
    /* group mode active only while a multicast channel has remote subscribers: new
       data + HBs ride the group lane, this per-peer lane only answers NACKs */
    int group_mode = ch->multicast && ch->nsubs>0;
    uint16_t alias = dart__alias_of(st, ci);
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
                    uint32_t off=(uint32_t)fi*st->frag;
                    uint16_t plen=(uint16_t)((s->len-off)<st->frag?(s->len-off):st->frag);
                    if (cap < (size_t)(s->count==1?13u:21u)+(size_t)plen) return 0;   /* bit stays set */
                    w->nack_bits &= ~(1u<<i);
                    if (w->nack_bits==0) w->has_nack=0;
                    return dart_mk_data(out,alias,seqno,s,fi,s->buf+off,plen);
                } else {
                    /* superseded: skip the reader past the dropped region with an HB
                       (its first = our floor); keep still-cached seqnos for later repair */
                    uint64_t floor = (ch->have_first?ch->first_seqno:ch->next_seqno);
                    uint32_t j;
                    if (cap < 23) return 0;
                    for (j=0;j<DART_NACK_WINDOW;j++)
                        if (w->nack_base+j < floor) w->nack_bits &= ~(1u<<j);
                    if (w->nack_bits==0) w->has_nack=0;
                    return dart_writer_hb(ch,w,alias,out,cap,now);
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
            uint32_t off=(uint32_t)fi*st->frag;
            uint16_t plen=(uint16_t)((s->len-off)<st->frag?(s->len-off):st->frag);
            if (cap < (size_t)(s->count==1?13u:21u)+(size_t)plen) return 0;
            w->sent_upto++;
            return dart_mk_data(out,alias,seqno,s,fi,s->buf+off,plen);
        } else {
            /* fell out of the ring before we sent it: skip the reader up to first
               cached with an HB (its first = our floor) */
            if (cap < 23) return 0;
            w->sent_upto=(ch->have_first?ch->first_seqno:ch->next_seqno);
            return dart_writer_hb(ch,w,alias,out,cap,now);
        }
    }

    /* 3. heartbeat (reliable, timer due, and this reader is behind). Once it has
       acked everything (acked_upto == next_seqno) there's nothing to repair, so the
       lane goes silent until new data or a (re)subscribe drops acked_upto again. The
       HB advertises from acked_upto so a fresh reader adopts the join point. */
    if (reliable && now>=w->hb_next_us && w->acked_upto < ch->next_seqno)
        return dart_writer_hb(ch,w,alias,out,cap,now);
    return 0;
}

/* produce a reader ACKNACK for (ci,pslot) if due; 0 if none */
static size_t dart_reader_emit(dart_state *st, int ci, int pslot, uint8_t *out, size_t cap, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    dart_rproxy *r=&st->rprox[(size_t)ci*st->cfg.max_peers+pslot];
    uint64_t base; uint16_t nbits=0; uint32_t bm=0;
    uint16_t alias = dart__alias_of(st, ci);
    if (!r->used) return 0;
    if (ch->qos.reliability!=DART_RELIABLE) return 0;
    if (cap<21) return 0;
    if (!r->ack_pending || now<r->ack_due_us) return 0;
    r->ack_pending=0;

    if (!r->asm_active){
        base=r->deliver_upto;
        if (!r->started){ nbits=0; bm=0; }   /* no position yet: epoch hello */
        else if (r->deliver_upto<=r->hb_last){
            /* request the whole missing window so one round-trip repairs a burst */
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
    /* un-started: re-arm the (re)subscribe announce a bounded number of times so a
       lost one still reaches the writer; data arriving clears hello_left (started) */
    if (!r->started && r->hello_left){
        if (--r->hello_left){ r->ack_pending=1; r->ack_due_us = now + ch->qos.heartbeat_us; }
    }
    return dart_mk_nack(out,alias,base,nbits,bm,r->epoch,
                      r->started ? 0 : (uint8_t)DART_F_UNPOS);
}

/* multicast: 1 if every matched subscriber has acked all data, so the group
 * heartbeat can stop until new data arrives or a new/lagging subscriber needs it */
static int dart__group_all_acked(dart_state *st, int ci){
    uint32_t np=st->cfg.max_peers, p; uint64_t seq=st->chans[ci].next_seqno;
    for (p=0;p<np;p++){
        dart_wproxy *w=&st->wprox[(size_t)ci*np+p];
        if (w->used && w->acked_upto < seq) return 0;
    }
    return 1;
}

/* multicast writer lane: new data once for the whole group, then a channel-level
 * heartbeat (reliable). Same per-call contract as dart_writer_emit. */
static size_t dart_group_emit(dart_state *st, int ci, uint8_t *out, size_t cap, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    uint16_t alias = dart__alias_of(st, ci);
    if (!ch->multicast || ch->role==DART_SUB_ONLY || ch->role==DART_INACTIVE) return 0;
    if (ch->nsubs==0){
        /* no remote subscribers: pin the cursor forward so a future join gets no stale replay */
        ch->mc_sent_upto = ch->next_seqno;
        return 0;
    }
    if (ch->mc_sent_upto < ch->next_seqno){
        uint64_t seqno=ch->mc_sent_upto;
        dart_wsample *s=dart_find_sample(ch,seqno);
        if (s){
            uint16_t fi=(uint16_t)(seqno - s->base);
            uint32_t off=(uint32_t)fi*st->frag;
            uint16_t plen=(uint16_t)((s->len-off)<st->frag?(s->len-off):st->frag);
            if (cap < (size_t)(s->count==1?13u:21u)+(size_t)plen) return 0;
            ch->mc_sent_upto++;
            return dart_mk_data(out,alias,seqno,s,fi,s->buf+off,plen);
        } else {
            /* overran the ring: skip the group past it with an HB (first = our floor) */
            if (cap < 23) return 0;
            ch->mc_sent_upto=(ch->have_first?ch->first_seqno:ch->next_seqno);
            ch->mc_hb_next_us = now + ch->qos.heartbeat_us;
            ch->mc_hb_count++;
            return dart_mk_hb(out,alias,(ch->have_first?ch->first_seqno:0),ch->next_seqno-1,ch->mc_hb_count);
        }
    }
    if (ch->qos.reliability==DART_RELIABLE && now>=ch->mc_hb_next_us && ch->next_seqno>0
        && !dart__group_all_acked(st, ci)){
        if (cap < 23) return 0;
        ch->mc_hb_next_us = now + ch->qos.heartbeat_us;
        ch->mc_hb_count++;
        return dart_mk_hb(out,alias,(ch->have_first?ch->first_seqno:0),ch->next_seqno-1,ch->mc_hb_count);
    }
    return 0;
}

/* sendable work a popped lane still owes now (timer-armed work is the sweep's job) */
static int dart__lane_work(dart_state *st, uint16_t ci, uint32_t ps, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    uint32_t np=st->cfg.max_peers;
    if (ps==np)
        return ch->multicast && ch->nsubs>0 && ch->mc_sent_upto < ch->next_seqno;
    if (!st->peer_used[ps]) return 0;
    { dart_wproxy *w=&st->wprox[(size_t)ci*np+ps];
      dart_rproxy *r=&st->rprox[(size_t)ci*np+ps];
      int group_mode = ch->multicast && ch->nsubs>0;
      if (w->used && w->has_nack) return 1;
      if (w->used && !group_mode && w->sent_upto < ch->next_seqno) return 1;
      if (r->used && ch->qos.reliability==DART_RELIABLE
          && r->ack_pending && now >= r->ack_due_us) return 1;
    }
    return 0;
}

/* clock-driven counterpart of the wake calls: a cursor walks the lane table at a
 * fixed TIME rate (full coverage every DART_HB_SWEEP_US) and wakes lanes whose
 * timers came due. Read-only; cost is bounded by table size per sweep period. */
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
        /* gate writer/multicast heartbeats on next_seqno, never the reader ack: a
           sub-only node's data channels never advance next_seqno but still owe acks */
        if (ch->qos.reliability!=DART_RELIABLE) continue;
        if (ps==np){
            if (ch->next_seqno && ch->multicast && ch->nsubs>0 && now>=ch->mc_hb_next_us
                && !dart__group_all_acked(st,(int)ci))
                dart__lane_wake(st,ci,ps);
            continue;
        }
        if (!st->peer_used[ps]) continue;
        { dart_wproxy *w=&st->wprox[(size_t)ci*np+ps];
          dart_rproxy *r=&st->rprox[(size_t)ci*np+ps];
          int group_mode = ch->multicast && ch->nsubs>0;
          if ((w->used && !group_mode && now>=w->hb_next_us && w->acked_upto < ch->next_seqno)
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
                    /* acks first: small, one-shot, and carry the NACKs that drive
                       repair, so a backlogged writer can't starve them */
                    n=dart_reader_emit(st,(int)ci,(int)ps,(uint8_t*)out+off,cap-off,now);
                    if (!n) n=dart_writer_emit(st,(int)ci,(int)ps,(uint8_t*)out+off,cap-off,now);
                }
                off+=n;
            } while (n && off<cap);
            st->dest_head[d]=st->lane_next[L];
            if (dart__lane_work(st,ci,ps,now)){
                /* datagram full mid-lane: rotate the lane to the back so siblings get the next */
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
                              : DART_DEST_GROUP(st->chans[d-np].identity & 0xFFu);
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
/* NODE runtime: owns the data socket, drives discovery, wires peers into the
 * transport. All OS access goes through dart_plat. See dart_node.h. */

#include <string.h>

typedef struct {
    uint8_t  used;
    uint32_t id;
    uint8_t  ip[16];
    uint8_t  ip_len;
    uint16_t port;     /* peer's advertised data port */
} dart__nodepeer;

struct dart_node {
    dart_state     *tr;
    dart_discovery_rt     *disc;
    dart_sock       fd;       /* unicast data socket (also group TX) */
    dart_sock       mcfd;     /* multicast data RX socket (DART_SOCK_BAD if unused) */
    dart__nodepeer *peers;
    uint16_t      max_peers;
    uint16_t      domain;
    uint16_t      mc_port;
    /* datagram the socket refused; retried first next poll so it is never lost */
    uint8_t       txhold[DART_DGRAM_MAX];
    size_t        txhold_len;
    uint32_t      txhold_peer;
    /* backpressure accumulators, read via dart_node_backpressure_stats */
    uint64_t      backpressure_accum_us;
    uint32_t      backpressure_accum_n;
    /* one app callback for everything but message delivery; node fills PEER_UP/DOWN */
    dart_event_fn  on_event;
    void          *user_data;
    /* opaque blob carried in every discovery announce; must outlive the node. Holds
       this node's UDP fragment size + pub/sub interest list (see dart__meta_*). */
    uint8_t       *disc_meta;     /* arena, disc_meta_cap bytes */
    uint16_t       disc_meta_cap;
    uint16_t       disc_meta_len;
    uint16_t       frag_size;     /* our clamped UDP fragment size, baked into the blob */
};

/* Discovery-announce metadata: a versioned blob carrying this node's fragment size
 * then its interest list (dart_build_interest output). Layout:
 * ['D','N', ver=2, frag_lo, frag_hi, <interest blob...>]. Laid out to grow (e.g. a
 * future SHM segment id between the prefix and the interest). */
#define DART__META_PREFIX 5u

static uint16_t dart__meta_frag(const uint8_t *meta, uint16_t mlen){
    if (!meta || mlen < DART__META_PREFIX || meta[0]!='D' || meta[1]!='N' || meta[2]!=2) return 0;
    return (uint16_t)(meta[3] | ((uint16_t)meta[4] << 8));
}
/* Locate the interest sub-blob within a peer's meta; NULL/0 if absent. */
static const uint8_t *dart__meta_interest(const uint8_t *meta, uint16_t mlen, size_t *out_len){
    if (!meta || mlen < DART__META_PREFIX || meta[0]!='D' || meta[1]!='N' || meta[2]!=2){
        *out_len = 0; return NULL;
    }
    *out_len = (size_t)(mlen - DART__META_PREFIX);
    return meta + DART__META_PREFIX;
}
/* (Re)build our blob: frag prefix + current interest list. Returns its length. */
static uint16_t dart__meta_build(dart_node *n){
    uint8_t *o = n->disc_meta; size_t il;
    o[0]='D'; o[1]='N'; o[2]=2;
    o[3]=(uint8_t)(n->frag_size & 0xFF); o[4]=(uint8_t)(n->frag_size >> 8);
    il = dart_build_interest(n->tr, o + DART__META_PREFIX, n->disc_meta_cap - DART__META_PREFIX);
    return (uint16_t)(DART__META_PREFIX + il);
}

/* announce blob capacity: frag prefix + the largest interest list our channels can
 * produce, capped to fit one (IP-fragmentable) UDP datagram */
static uint16_t dart__node_meta_cap(const dart_node_config *cfg){
    size_t mc = DART__META_PREFIX + dart_interest_max(cfg->n_channels);
    if (mc > 65000u) mc = 65000u;
    return (uint16_t)mc;
}

/* split node config into discovery + transport sub-configs */
static void dart__node_cfgs(const dart_node_config *cfg, dart_discovery_rt_config *dc,
                          dart_config *tc, uint16_t *mp_out){
    uint16_t mp = cfg->discovery.max_peers ? cfg->discovery.max_peers : 16;
    memset(dc, 0, sizeof *dc); memset(tc, 0, sizeof *tc);
    dc->disc.domain_id   = cfg->domain;
    dc->disc.data_port   = cfg->net.data_port;
    dc->disc.announce_us = cfg->discovery.announce_interval_us; /* 0 uses default */
    dc->disc.timeout_us  = cfg->discovery.peer_timeout_us;
    dc->disc.max_peers   = mp;
    dc->disc.meta_cap    = dart__node_meta_cap(cfg);
    dc->group            = cfg->net.discovery_group;
    dc->disc_port        = cfg->net.discovery_port;
    dc->ttl              = cfg->net.multicast_ttl;
    dc->mcast_if         = cfg->net.multicast_interface;
    dc->seeds            = cfg->net.seed_peers;
    dc->n_seeds          = cfg->net.n_seed_peers;
    tc->channels   = cfg->channels;
    tc->n_channels = cfg->n_channels;
    tc->max_peers  = mp;
    tc->frag_payload = cfg->net.fragment_size;   /* 0 = default; dart_init clamps to [MIN,MAX] */
    tc->on_message = cfg->on_message;
    tc->on_event   = cfg->on_event;
    tc->allocator  = cfg->allocator;
    tc->user       = cfg->user_data;
    if (mp_out) *mp_out = mp;
}

/* local iff a route probe to the address selects that same address as source */
static int dart__node_is_local_ip(const uint8_t ip[4]){
    uint32_t d = dart_plat_ip4_to_naddr(ip);
    return dart_plat_route_src(d, 7) == d;
}

static void dart__node_up(void *u, uint32_t id, const dart_discovery_addr *addr,
                        const uint8_t *meta, uint16_t mlen){
    dart_node *n = (dart_node*)u; uint16_t i; int slot = -1;
    uint16_t frag = dart__meta_frag(meta, mlen);
    size_t ilen = 0; const uint8_t *interest = dart__meta_interest(meta, mlen, &ilen);
    for (i=0;i<n->max_peers;i++){
        if (n->peers[i].used && n->peers[i].id==id){      /* known peer: addr/interest update */
            memcpy(n->peers[i].ip, addr->ip, 16);
            n->peers[i].ip_len = addr->ip_len; n->peers[i].port = addr->port;
            dart_peer_set_frag(n->tr, id, frag);
            if (interest) dart_apply_peer_interest(n->tr, id, interest, ilen);
            return;
        }
        if (!n->peers[i].used && slot<0) slot=(int)i;
    }
    if (slot<0) return;
    n->peers[slot].used=1; n->peers[slot].id=id;
    memcpy(n->peers[slot].ip, addr->ip, 16);
    n->peers[slot].ip_len=addr->ip_len; n->peers[slot].port=addr->port;
    /* the announce blob carries the peer's frag size + pub/sub interest list */
    dart_peer_add(n->tr, id, (addr->ip_len==4) && dart__node_is_local_ip(addr->ip), frag);
    if (interest) dart_apply_peer_interest(n->tr, id, interest, ilen);
    if (n->on_event){
        dart_event ev; memset(&ev, 0, sizeof ev);
        ev.kind=DART_PEER_UP; ev.peer=id; ev.detail="peer discovered";
        memcpy(ev.ip, addr->ip, 16); ev.ip_len=addr->ip_len; ev.port=addr->port;
        n->on_event(n->user_data, &ev);
    }
}
static void dart__node_down(void *u, uint32_t id){
    dart_node *n=(dart_node*)u; uint16_t i; int found=0;
    for (i=0;i<n->max_peers;i++) if (n->peers[i].used && n->peers[i].id==id){ n->peers[i].used=0; found=1; break; }
    dart_peer_remove(n->tr, id);
    if (found && n->on_event){
        dart_event ev; memset(&ev, 0, sizeof ev);
        ev.kind=DART_PEER_DOWN; ev.peer=id; ev.detail="peer lost";
        n->on_event(n->user_data, &ev);
    }
}

static int dart__node_find_addr(dart_node *n, const uint8_t ip[4], uint16_t port){
    uint16_t i;
    for (i=0;i<n->max_peers;i++)
        if (n->peers[i].used && n->peers[i].ip_len>=4 && n->peers[i].port==port
            && memcmp(n->peers[i].ip, ip, 4)==0) return (int)i;
    return -1;
}
static int dart__node_find_id(dart_node *n, uint32_t id){
    uint16_t i;
    for (i=0;i<n->max_peers;i++) if (n->peers[i].used && n->peers[i].id==id) return (int)i;
    return -1;
}

/* data multicast group from a selector (topic identity & 0xFF); &0xFF wrap is
 * harmless, RX filters by peer table + identity */
static uint32_t dart__node_group_addr(uint16_t domain, uint16_t sel){
    return dart_plat_ipv4(239u, 255u, (uint8_t)(domain & 0xFFu), (uint8_t)(sel & 0xFFu));
}
/* a channel's group, from its identity so it matches DART_DEST_GROUP and every peer */
static uint32_t dart__node_chan_group(uint16_t domain, const dart_channel_def *def){
    return dart__node_group_addr(domain, (uint16_t)(dart_channel_identity(def) & 0xFFu));
}

/* send one datagram; returns 1 when done with it, 0 only on a would-block TX-full */
static int dart__node_tx(dart_node *n, uint32_t to, const uint8_t *buf, size_t len){
    uint8_t ip[4]; uint16_t port;
    if (DART_DEST_IS_GROUP(to)){
        dart_plat_naddr_to_ip4(dart__node_group_addr(n->domain, DART_DEST_GROUP_CHAN(to)), ip);
        port = n->mc_port;
    } else {
        int pi = dart__node_find_id(n, to);
        if (pi < 0) return 1;              /* peer vanished */
        memcpy(ip, n->peers[pi].ip, 4);
        port = n->peers[pi].port;
    }
    if (dart_plat_send(n->fd, buf, len, ip, port) < 0 && dart_plat_would_block())
        return 0;
    return 1;
}

size_t dart_node_required_memory(const dart_node_config *cfg){
    dart_discovery_rt_config dc; dart_config tc; uint16_t mp;
    size_t node_sz, tbl_sz, meta_sz, disc_sz, tr_sz;
    if (!cfg || cfg->n_channels==0) return 0;
    dart__node_cfgs(cfg,&dc,&tc,&mp);
    node_sz = (sizeof(struct dart_node)+15u)&~(size_t)15u;
    tbl_sz  = ((size_t)mp*sizeof(dart__nodepeer)+15u)&~(size_t)15u;
    meta_sz = ((size_t)dart__node_meta_cap(cfg)+15u)&~(size_t)15u;
    disc_sz = (dart_discovery_rt_required_memory(&dc)+15u)&~(size_t)15u;
    tr_sz   = (dart_required_memory(&tc)+15u)&~(size_t)15u;
    return 32u + node_sz + tbl_sz + meta_sz + disc_sz + tr_sz;
}

dart_node *dart_node_open(void *mem, size_t cap, const dart_node_config *cfg){
    dart_discovery_rt_config dc; dart_config tc; uint16_t mp;
    uint8_t *base, *p; size_t node_sz, tbl_sz, meta_sz, disc_sz, tr_sz;
    dart_node *n; dart_sock fd; uint16_t lp, f;
    if (!mem || !cfg || cfg->n_channels==0) return NULL;
    if (cap < dart_node_required_memory(cfg)) return NULL;
    dart__node_cfgs(cfg,&dc,&tc,&mp);

    if (!dart_plat_startup()) return NULL;

    base = (uint8_t*)(((uintptr_t)mem+15u)&~(uintptr_t)15u);
    node_sz = (sizeof(struct dart_node)+15u)&~(size_t)15u;
    tbl_sz  = ((size_t)mp*sizeof(dart__nodepeer)+15u)&~(size_t)15u;
    meta_sz = ((size_t)dart__node_meta_cap(cfg)+15u)&~(size_t)15u;
    disc_sz = (dart_discovery_rt_required_memory(&dc)+15u)&~(size_t)15u;
    tr_sz   = (dart_required_memory(&tc)+15u)&~(size_t)15u;

    n=(dart_node*)base; memset(n,0,sizeof *n);
    n->fd = DART_SOCK_BAD; n->mcfd = DART_SOCK_BAD; n->max_peers=mp;
    n->domain = cfg->domain;
    n->on_event = cfg->on_event; n->user_data = cfg->user_data;
    n->mc_port = cfg->net.multicast_port ? cfg->net.multicast_port
               : (uint16_t)((cfg->net.discovery_port ? cfg->net.discovery_port : 7400) + 1);
    /* our fragment size, clamped exactly as dart_init clamps it, baked into the blob */
    f = cfg->net.fragment_size ? cfg->net.fragment_size : DART_FRAG_PAYLOAD;
    if (f < DART_FRAG_PAYLOAD_MIN) f = DART_FRAG_PAYLOAD_MIN;
    if (f > DART_FRAG_PAYLOAD_MAX) f = DART_FRAG_PAYLOAD_MAX;
    n->frag_size = f;
    p = base + node_sz;
    n->peers=(dart__nodepeer*)p; memset(n->peers,0,(size_t)mp*sizeof(dart__nodepeer));
    p += tbl_sz;
    n->disc_meta = p; n->disc_meta_cap = dart__node_meta_cap(cfg);
    p += meta_sz;

    n->tr = dart_init(p, tr_sz, &tc);
    if (!n->tr){ dart_plat_cleanup(); return NULL; }
    p += tr_sz;

    /* Bind the data socket before opening discovery so we can advertise its real
       port (0 => OS ephemeral, read back via getsockname). No reuse: a unicast
       endpoint owns its port, so a collision fails loudly here. */
    fd = dart_plat_udp_open();
    if (fd==DART_SOCK_BAD){ dart_plat_cleanup(); return NULL; }
    if (!dart_plat_bind(fd, 0, cfg->net.data_port, 0)){
        dart_plat_close(fd); dart_plat_cleanup(); return NULL;
    }
    lp = dart_plat_local_port(fd);
    if (lp==0){ dart_plat_close(fd); dart_plat_cleanup(); return NULL; }
    dart_plat_set_nonblock(fd);   /* never block in recv/send; poll drains the queue */
    /* suppress WSAECONNRESET from a bounced send leaking into the shared RX path */
    dart_plat_suppress_connreset(fd);
    if (cfg->net.recv_buffer_bytes) dart_plat_set_rcvbuf(fd, (int)cfg->net.recv_buffer_bytes);
    if (cfg->net.send_buffer_bytes) dart_plat_set_sndbuf(fd, (int)cfg->net.send_buffer_bytes);
    n->fd=fd;
    dc.disc.data_port = lp;       /* advertise the actual port */

    /* multicast data: group TX rides the unicast socket; group RX needs its own
       socket on the shared mc_port with one IGMP join per subscribed channel */
    { int want_rx=0, want_tx=0; uint16_t i; uint32_t ifip;
      for (i=0;i<cfg->n_channels;i++) if (cfg->channels[i].multicast){
          if (cfg->channels[i].role!=DART_PUB_ONLY) want_rx=1;
          if (cfg->channels[i].role!=DART_SUB_ONLY) want_tx=1;
      }
      /* pin data multicast to discovery's egress interface, else multihomed hosts
         pick per-group interfaces and break peer identification by source addr */
      ifip = !(want_rx||want_tx) ? 0
           : cfg->net.multicast_interface ? dart_plat_parse_ip(cfg->net.multicast_interface)
           : dart_plat_route_src(dart_plat_parse_ip(cfg->net.discovery_group?cfg->net.discovery_group:"239.255.0.7"),
                                  cfg->net.discovery_port?cfg->net.discovery_port:7400);
      if (want_tx){
          unsigned char mttl = cfg->net.multicast_ttl ? cfg->net.multicast_ttl : 1;
          dart_plat_mcast_setif(n->fd, ifip);
          dart_plat_mcast_ttl(n->fd, mttl);
          dart_plat_mcast_loop(n->fd, 1);
      }
      if (want_rx){
          dart_sock mfd = dart_plat_udp_open();
          int mc_ok = (mfd!=DART_SOCK_BAD);
          if (mc_ok && !dart_plat_bind(mfd, 0, n->mc_port, 1)) mc_ok=0;
          if (mc_ok){
              /* one join per distinct group (kernels reject dups); memberships are
                 OS-capped (~20: Linux net.ipv4.igmp_max_memberships), a fail fails open */
              for (i=0;i<cfg->n_channels;i++)
                  if (cfg->channels[i].multicast && cfg->channels[i].role!=DART_PUB_ONLY){
                      uint32_t g = dart__node_chan_group(cfg->domain, &cfg->channels[i]);
                      uint16_t j; int dup=0;
                      for (j=0;j<i;j++)
                          if (cfg->channels[j].multicast && cfg->channels[j].role!=DART_PUB_ONLY
                              && dart__node_chan_group(cfg->domain, &cfg->channels[j])==g){
                              dup=1; break;
                          }
                      if (dup) continue;
                      if (!dart_plat_mcast_join(mfd, g, ifip)){ mc_ok=0; break; }
                  }
          }
          if (!mc_ok){
              if (mfd!=DART_SOCK_BAD) dart_plat_close(mfd);
              dart_plat_close(fd); n->fd=DART_SOCK_BAD; dart_plat_cleanup(); return NULL;
          }
          dart_plat_mcast_loop(mfd, 1);
          dart_plat_set_nonblock(mfd);
          if (cfg->net.recv_buffer_bytes) dart_plat_set_rcvbuf(mfd, (int)cfg->net.recv_buffer_bytes);
          n->mcfd=mfd;
      } }

    dc.disc.on_peer_up   = dart__node_up;
    dc.disc.on_peer_down = dart__node_down;
    dc.disc.user         = n;
    /* advertise our frag size + interest list so peers reassemble our messages and
       match topics straight from discovery. The buffer lives in the node. */
    n->disc_meta_len = dart__meta_build(n);
    dc.disc.meta = n->disc_meta; dc.disc.meta_len = n->disc_meta_len;
    n->disc = dart_discovery_rt_open(p, disc_sz, &dc);
    if (!n->disc){
        if (n->mcfd!=DART_SOCK_BAD){ dart_plat_close(n->mcfd); n->mcfd=DART_SOCK_BAD; }
        dart_plat_close(fd); n->fd=DART_SOCK_BAD; dart_plat_cleanup(); return NULL;
    }
    p += disc_sz;

    return n;
}

/* max wall-time draining RX (and running on_message) per poll tick before
 * yielding to discovery, so a slow on_message never starves it */
#ifndef DART_RX_BUDGET_US
#define DART_RX_BUDGET_US 5000u
#endif

/* drain one socket's RX queue into the transport until empty or past deadline
 * (full drain avoids NACK storms). Distinct from public dart_node_drain. */
static void dart__node_rx_drain(dart_node *n, dart_sock fd, uint64_t deadline){
    uint8_t buf[DART_DGRAM_MAX];
    for (;;){
        uint8_t sip[4]; uint16_t sport;
        int r = dart_plat_recv(fd, buf, sizeof buf, sip, &sport);
        if (r<0){
            if (dart_plat_would_block()) break;        /* queue empty */
            continue;   /* per-datagram error (e.g. bounced send); keep draining */
        }
        if (r>0){
            if (r>=4 && buf[0]=='u' && buf[1]=='D' && buf[2]=='S' && buf[3]=='C'){
                /* unicast announce aimed at our data port: hand it to discovery */
                dart_discovery_rt_feed(n->disc, sip, 4, buf, (size_t)r);
            } else {
                int pi=dart__node_find_addr(n, sip, sport);
                if (pi>=0) dart_on_datagram(n->tr, n->peers[pi].id, buf, (size_t)r, dart_plat_now_us());
            }
        }
        if (dart_plat_now_us() >= deadline) break;      /* yield to discovery/send */
    }
}

int dart_node_poll(dart_node *n, int timeout_ms){
    uint8_t buf[DART_DGRAM_MAX]; uint32_t to; size_t ol; uint64_t now;
    dart_pollfd pfd[2]; int nf=1;

    dart_discovery_rt_poll(n->disc, 0);                 /* discovery tick (non-blocking) */

    memset(pfd,0,sizeof pfd);
    pfd[0].fd=n->fd; pfd[0].events=DART_POLLIN;
    if (n->mcfd!=DART_SOCK_BAD){ pfd[1].fd=n->mcfd; pfd[1].events=DART_POLLIN; nf=2; }
    if (dart_plat_poll(pfd,nf,timeout_ms) > 0){
        uint64_t rx_deadline = dart_plat_now_us() + DART_RX_BUDGET_US;
        if (pfd[0].revents & DART_POLLIN) dart__node_rx_drain(n, n->fd, rx_deadline);
        if (nf==2 && (pfd[1].revents & DART_POLLIN)) dart__node_rx_drain(n, n->mcfd, rx_deadline);
    }

    now=dart_plat_now_us();
    /* the core already consumed any held datagram, so retry it before pulling new */
    if (n->txhold_len && dart__node_tx(n, n->txhold_peer, n->txhold, n->txhold_len))
        n->txhold_len = 0;
    if (!n->txhold_len)
        while (dart_poll_send(n->tr,&to,buf,sizeof buf,&ol,now)){
            if (!dart__node_tx(n, to, buf, ol)){
                memcpy(n->txhold, buf, ol);
                n->txhold_len = ol; n->txhold_peer = to;
                break;          /* TX buffer full: yield this tick */
            }
            now=dart_plat_now_us();
        }
    return 0;
}

int dart_node_send(dart_node *n, uint16_t channel, const void *data, size_t len){
    /* bounded backpressure: pump the loop (on_message/on_event may fire here) until
       a slow reader acks or qos.backpressure_wait_us elapses, then send anyway */
    const dart_qos *q = dart_channel_qos(n->tr, channel);
    if (q && q->backpressure_wait_us && dart_send_would_evict(n->tr, channel)){
        uint64_t t0 = dart_plat_now_us(), deadline = t0 + q->backpressure_wait_us;
        do {
            dart_node_poll(n, 1);
            if (!dart_send_would_evict(n->tr, channel)) break;
        } while (dart_plat_now_us() < deadline);
        n->backpressure_accum_us += dart_plat_now_us() - t0;
        n->backpressure_accum_n++;
    }
    return dart_send(n->tr, channel, data, len, dart_plat_now_us());
}

int dart_node_set_role(dart_node *n, uint16_t channel, uint8_t role){
    int r = dart_set_role(n->tr, channel, role);
    if (r == 0){   /* re-advertise our interest: peers rematch as the new blob arrives */
        n->disc_meta_len = dart__meta_build(n);
        dart_discovery_rt_set_meta(n->disc, n->disc_meta, n->disc_meta_len);
    }
    return r;
}

void dart_node_backpressure_stats(dart_node *n, uint64_t *waited_us, uint32_t *waited_sends){
    if (waited_us)    *waited_us    = n->backpressure_accum_us;
    if (waited_sends) *waited_sends = n->backpressure_accum_n;
}

int dart_node_drain(dart_node *n, uint16_t channel, int timeout_ms){
    uint64_t deadline = dart_plat_now_us() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000u;
    while (!dart_send_drained(n->tr, channel)){
        if (dart_plat_now_us() >= deadline) return 0;
        dart_node_poll(n, 1);
    }
    return 1;
}

int dart_node_writer_match_count(dart_node *n, uint16_t channel){
    return dart_writer_match_count(n->tr, channel);
}

void dart_node_close(dart_node *n, int send_bye){
    if (!n) return;
    if (n->disc) dart_discovery_rt_close(n->disc, send_bye);
    if (n->mcfd != DART_SOCK_BAD) dart_plat_close(n->mcfd);
    if (n->fd != DART_SOCK_BAD) dart_plat_close(n->fd);
    if (n->tr) dart_destroy(n->tr);     /* free hook-allocated dynamic buffers */
    dart_plat_cleanup();
}
#endif /* !DART_TRANSPORT_SANS_IO */
#endif /* DART_TRANSPORT_IMPLEMENTATION */
