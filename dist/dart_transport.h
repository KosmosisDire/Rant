/* GENERATED single-header build. DO NOT EDIT.
 * DART = Discovery And Realtime Transport. Amalgamated from src/ by
 * tools/pack.c. Edit the split sources in src/ and re-run pack to regenerate.
 * See the flag scheme at the top of tools/pack.c.
 */
#if defined(DART_TRANSPORT_IMPLEMENTATION) && !defined(DART_TRANSPORT_SANS_IO) && !defined(_WIN32)
  #ifndef _POSIX_C_SOURCE
  #define _POSIX_C_SOURCE 200809L
  #endif
  #ifndef _DEFAULT_SOURCE
  #define _DEFAULT_SOURCE 1
  #endif
#endif

#ifndef DART_TRANSPORT_SANS_IO
  #if defined(DART_TRANSPORT_IMPLEMENTATION) && !defined(DART_DISCOVERY_IMPLEMENTATION)
  #define DART_DISCOVERY_IMPLEMENTATION
  #endif
  #include "dart_discovery.h"   /* discovery: needed by the node runtime */
#endif

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

/* The zero-fragment same-host shared-memory path is ON by default. Opt out with
 * DART_NO_SHM for a slimmer build or a target without shm_open/mmap (on POSIX, link
 * -lrt on older glibc). It is used only between same-host nodes that set an allocator;
 * a node with no local SHM peer creates no segment and pays nothing at runtime. */
#if !defined(DART_SHM) && !defined(DART_NO_SHM)
#define DART_SHM
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

#ifdef DART_SHM
#define DART_SHM_DESC_BYTES 24u   /* opaque SHM descriptor on the wire; == dart_shm.h DART_SHM_DESC_WIRE */
#endif

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
    uint32_t shm_max_bytes;      /* same-host SHM: pin this channel to one size class big enough for
                                    this many bytes, so same-sized traffic reuses one pre-sized
                                    segment (a larger message falls back to UDP). 0 = each message
                                    uses its own size class's segment, created on demand. */
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

#ifdef DART_SHM
/* SHM delivery: the transport reassembled nothing -- it hands the node the
 * DART_SHM_DESC_BYTES descriptor from an SHM-DATA submessage and the node resolves it
 * to bytes and calls the user's on_message. Returns 1 if delivered, 0 if it could not
 * resolve the chunk (recycled / unattachable) -- then the reader leaves the gap so the
 * reliability layer repairs or skips it. Internal (transport->node); the user's
 * on_message is unchanged and never sees this. */
typedef int (*dart_shm_msg_fn)(void *user, uint16_t channel, uint32_t from_peer,
                             const uint8_t *desc);
#endif

/* Everything that isn't message delivery, as one notification (optional). The
 * meaningful dart_event fields depend on .kind. */
typedef enum {
    DART_PEER_UP,        /* peer discovered or resumed: .peer, .ip/.ip_len/.port (node) */
    DART_PEER_DOWN,      /* peer lost or fell silent: .peer (node) */
    DART_MSG_LOST,       /* messages skipped: .channel, .peer, .first .. .first+.count-1 */
    DART_MSG_TOO_BIG,    /* a received message exceeded max_message_bytes (.count = its size), skipped */
    DART_NAME_COLLISION, /* a peer's name hashes to ours but differs (.first = identity, .detail = our name), refused */
    DART_PEER_REFUSED    /* peer table full of active peers: a new peer was refused (.ip/.ip_len/.port) (node) */
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
#ifdef DART_SHM
    dart_shm_msg_fn         on_shm;     /* SHM-DATA delivery (descriptor); the node resolves it */
#endif
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

/* Discovery-blip lifecycle: a peer that fell silent (discovery timeout) is made
 * DORMANT instead of removed, so its reader position survives and a same-incarnation
 * return resumes losslessly. Dormant peers are dropped from flow control (the writer
 * stops heartbeating/draining them so a dead reader can't stall it; the reader stops
 * acking them), but their proxies and deliver position are preserved. dart_peer_resume
 * re-includes the peer and re-reports reader positions so the writer fills any gap.
 * Both no-op for an unknown peer; the node drives them off discovery DROP/return. */
void      dart_peer_dormant(dart_state *st, uint32_t peer_id);
void      dart_peer_resume (dart_state *st, uint32_t peer_id);
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

#ifdef DART_SHM
/* Publish a message whose payload lives in an external shared-memory buffer: the
 * transport stores the sample referencing chunk (NOT copied) plus the descriptor,
 * fragments from chunk for non-SHM peers, and sends ONE SHM-DATA (the descriptor) to
 * SHM-capable peers. desc is DART_SHM_DESC_BYTES. Same return as dart_send. The chunk
 * must stay valid until the sample leaves history (acked / evicted). */
int       dart_send_shm(dart_state *st, uint16_t channel, const void *chunk, size_t len,
                      const uint8_t *desc, uint64_t now_us);
/* Mark whether a peer can receive SHM-DATA (same host AND its segment is attached).
 * Off by default; the node sets it on attach, clears it on dormant/remove. */
void      dart_peer_set_shm(dart_state *st, uint32_t peer_id, int is_shm);
/* 1 if every matched reader of channel is SHM-capable and it is non-multicast, so a
 * publish may go via SHM (else inline). The node checks this per message. */
int       dart_writer_shm_eligible(dart_state *st, uint16_t channel);
/* The history slot the next publish to channel will occupy (binds chunk<->slot). */
uint16_t  dart_channel_hist_head(dart_state *st, uint16_t channel);
#endif

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
#ifdef DART_SHM
/* Messages published / delivered via the zero-fragment shared-memory path since open
 * (observability; same-host readers only). Either out-pointer may be NULL. */
void     dart_node_shm_stats(dart_node *n, uint32_t *sent, uint32_t *recv);
#endif
void     dart_node_close(dart_node *n, int send_bye);

#ifdef __cplusplus
}
#endif
#endif /* DART_NODE_H */
/* ===== dart_shm.h ===== */
/* dart_shm: zero-copy same-host payload path. OPT-IN -- nothing here compiles or
 * links unless you define DART_SHM, so embedded / non-SHM targets carry zero cost
 * and need no shared-memory platform support. Speaks only dart_plat_* (shm mapping,
 * host uuid, an atomic for the generation stamp).
 *
 * Model (per-peer, inside the transport's reliable stream -- NOT a side channel).
 * A published message occupies count seqnos on the writer's per-channel line, as
 * today. The per-peer LANE picks the wire form:
 *   - remote peer  -> count DATA fragments, read from the message buffer (as now)
 *   - same-host peer-> ONE SHM-DATA submessage (a DATA flag) covering [base,count),
 *                      carrying a 24-byte descriptor (segment+chunk+gen+len); the
 *                      reader marks the whole range delivered and reads the chunk
 *                      in place (zero copy), then ACKs the range like any reader.
 * The shared seqno line is untouched, so a channel serves local and remote
 * subscribers at once (and multicast: group-multicast to remote, unicast SHM-DATA
 * to each local sub). Eligibility is automatic: a lane uses SHM iff that peer is
 * same-host and attached.
 *
 * Lifecycle reuses reliability, so there is NO separate refcount/reclaim protocol:
 *   - the chunk IS the writer's history slot's buffer (one chunk per keep_last slot)
 *   - the reader delivers SYNCHRONOUSLY (on_message reads the chunk in place) and only
 *     THEN arms its ACK, which leaves on a later poll_send -> an ACK provably means
 *     "the user finished reading." The writer holds the chunk until that ACK.
 *   - so the writer recycles a slot only when the reader ACKED (done) or discovery
 *     declared it dormant/gone (a live reader mid-read is announcing, never dormant).
 *     SHM eviction is gated on ACK-or-LIVENESS, NOT the short backpressure_wait_us
 *     timer -- a slow-but-alive reader applies backpressure instead of having its
 *     chunk yanked mid-read. That is the torn-free guarantee for RELIABLE SHM.
 *   - generation is the backstop: a straggler that reads a reused chunk sees a
 *     generation mismatch and counts the sample lost (repaired on reliable, dropped
 *     on best-effort) instead of delivering torn bytes. Best-effort SHM has no ACKs,
 *     so a too-slow reader misses lapped samples, exactly like best-effort UDP.
 *   - contract: the on_message pointer is valid FOR THE CALL ONLY (already true for
 *     UDP); consume or copy it there. SHM just makes honoring it matter for safety.
 *
 * Zero copy both ways: the app loans a chunk and writes into it (dart_node_loan),
 * remote peers fragment straight from that chunk, local peers read it in place in
 * on_message (valid-for-the-call, the existing contract). One-copy fallback:
 * plain dart_node_send memcpys into the chunk.
 *
 * Read modes (a future toggle; ship the safe one first):
 *   - ONE-COPY SHM (default): the reader memcpys the chunk into its own asm_buf, then
 *     OWNS the bytes -- so it acks like UDP (ack timing is free, no deliver-before-ack
 *     coupling), the writer is released immediately, and there is no slow-reader stall
 *     or torn-read window. Still a big win: one SHM-DATA submessage + one local bulk
 *     copy replaces N fragment datagrams + reassembly.
 *   - ZERO-COPY SHM (opt-in): no copy, on_message reads the chunk in place; REQUIRES
 *     deliver-before-ack and holds the writer until the read completes (see below).
 *     The last increment, for latency/throughput-critical paths that accept the
 *     coupling. The difference is purely read-side: same wire format, same descriptor.
 *
 * This header is the portable mapping + chunk module. Its hooks into the transport
 * (the SHM-DATA submessage, the per-peer lane choice, chunk-backed history) and the
 * node (advertise, attach, deliver) are the contract in "INTEGRATION" below.
 */
#ifndef DART_SHM_H
#define DART_SHM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compile bounds (a fixed segment; an SBC sets these small, a workstation large). */
#ifndef DART_SHM_CHUNK_BYTES
#define DART_SHM_CHUNK_BYTES (4u*1024u*1024u)  /* default chunk; node overrides per size class */
#endif
#ifndef DART_SHM_CHUNKS
#define DART_SHM_CHUNKS 4u                     /* default chunks/segment; node overrides per class */
#endif
#ifndef DART_SHM_NAME_MAX
#define DART_SHM_NAME_MAX 64u                  /* OS object name, derived from the node uuid */
#endif

/* Size-class ladder (iceoryx-style): class k chunk payload = BASE << (k*SHIFT).
 * Defaults 64K,256K,1M,4M,16M,64M,256M (k=0..6) at SHIFT=2. The node lazily creates
 * one segment PER CHANNEL at that channel's size class (n_chunks = its keep_last), and
 * encodes the class in the low 3 bits of segment_id, the channel in the next 16. */
#ifndef DART_SHM_CLASS_BASE
#define DART_SHM_CLASS_BASE  (64u*1024u)
#endif
#ifndef DART_SHM_CLASS_SHIFT
#define DART_SHM_CLASS_SHIFT 2u
#endif
#ifndef DART_SHM_N_CLASSES
#define DART_SHM_N_CLASSES   7u
#endif
#define DART_SHM_CLASS_MASK  0x7u               /* class lives in the low 3 bits of segment_id */

uint32_t dart_shm_class_bytes(uint32_t k);      /* chunk payload bytes for class k */
uint32_t dart_shm_class_for(uint32_t len);      /* smallest class fitting len; N_CLASSES if too big */

/* ----------------------------------------------------------------- descriptor
 * The SHM locator. Travels INSIDE an SHM-DATA submessage, whose framing supplies
 * the seqno base + count (the transport fills those from the history sample), so
 * the descriptor itself is just where-to-read. generation lets a straggling reader
 * detect a recycled chunk and fall back to reliable repair. */
typedef struct {
    uint64_t segment_id;   /* writer's segment (its discovery uuid, hashed to 64) */
    uint32_t chunk;        /* chunk index in [0, n_chunks) */
    uint32_t length;       /* payload bytes */
    uint64_t generation;   /* chunk reuse counter at publish; reader rechecks after reading */
} dart_shm_desc;

#define DART_SHM_DESC_WIRE 24u   /* little-endian; rides the SHM-DATA submessage body */
size_t dart_shm_desc_encode(const dart_shm_desc *d, uint8_t out[DART_SHM_DESC_WIRE]);
int    dart_shm_desc_decode(dart_shm_desc *d, const uint8_t *in, size_t len);  /* 1 ok, 0 malformed */

/* ------------------------------------------------------------- segment layout
 *   [ dart_shm_seg_hdr ][ chunk 0 ] ... [ chunk N-1 ]
 *   chunk = [ dart_shm_chunk_hdr (padded to 16) ][ chunk_bytes payload ]
 * generation is the only cross-process mutable field: written (atomic release) by
 * the writer before the descriptor is sent, read (atomic acquire) by the reader
 * after reading the payload. No refcount -- reliability owns the lifecycle. */
typedef struct {
    uint32_t magic;         /* DART_SHM_MAGIC; reject a stale/foreign mapping */
    uint32_t version;
    uint64_t segment_id;
    uint32_t chunk_bytes;   /* must equal the reader's compile bound, else reject */
    uint32_t n_chunks;
    uint64_t owner_pid;     /* writer pid: external janitor can reclaim an orphan */
    uint8_t  owner_host[16];/* writer host uuid: reader confirms same kernel */
} dart_shm_seg_hdr;

typedef struct {
    uint64_t generation;    /* bumped each reuse; matched against the descriptor */
    uint32_t length;
    uint32_t _pad;
} dart_shm_chunk_hdr;

#define DART_SHM_MAGIC    0x4D484453u   /* 'DSHM' */
#define DART_SHM_VERSION  1u

/* ------------------------------------------------------------------ pool (API)
 * Opaque per-process handle over one mapped segment, placed in caller memory
 * (the node arena; size via dart_shm_state_bytes). A node CREATEs one segment for
 * its own publishes and ATTACHes one per same-host peer it subscribes to. */
typedef struct dart_shm_pool dart_shm_pool;
size_t dart_shm_state_bytes(void);

typedef struct {
    char     name[DART_SHM_NAME_MAX];  /* writer makes it from its uuid; reader gets it via meta */
    uint64_t segment_id;
    uint32_t chunk_bytes;              /* create only (0 => DART_SHM_CHUNK_BYTES); attach reads it from the header */
    uint32_t n_chunks;                 /* create only (0 => DART_SHM_CHUNKS); attach reads it from the header */
} dart_shm_config;

/* Writer. create maps a fresh segment (dart_plat_shm_create); NULL => stay on UDP. */
dart_shm_pool *dart_shm_create(void *pool_mem, const dart_shm_config *cfg);
/* The chunk backing a history slot: loan returns a writable pointer (app fills it),
 * stamp bumps generation + sets length and fills *out for the transport to frame in
 * the SHM-DATA submessage. The node owns chunk<->slot assignment (1 chunk per
 * keep_last slot), so there is no free list here. */
void *dart_shm_chunk(dart_shm_pool *p, uint32_t chunk, uint32_t *out_cap);
void  dart_shm_stamp(dart_shm_pool *p, uint32_t chunk, uint32_t len, dart_shm_desc *out);

/* Reader. attach maps an existing segment WHOLE by name and reads its geometry
 * (chunk_bytes/n_chunks) from the header the writer stamped, so the reader needs to
 * know nothing about its size; validates magic/version/owner_host==ours and that the
 * geometry fits the mapped object. NULL => fall back to the UDP path. read resolves a
 * descriptor to an in-segment pointer and verifies generation still matches (else
 * recycled -> NULL, reliable repair covers it). No release call: the reader's
 * transport ACK of the range is the release. */
dart_shm_pool *dart_shm_attach(void *pool_mem, const dart_shm_config *cfg);
const void    *dart_shm_read  (dart_shm_pool *p, const dart_shm_desc *d, uint32_t *out_len);
/* re-check the chunk generation AFTER a one-copy read (seqlock tail): 1 if it still
 * matches d (the copy is clean), 0 if a best-effort writer recycled it mid-copy (the
 * copy may be torn -> discard). Lock-free: the writer never blocks. */
int            dart_shm_verify(dart_shm_pool *p, const dart_shm_desc *d);

void dart_shm_detach(dart_shm_pool *p);  /* unmap; the writer also unlinks the OS object */

/* Same-host id: SHM is valid only between processes sharing one kernel AND able to
 * map the object (loopback addr alone is not sufficient -- containers/namespaces).
 * The node advertises dart_plat_host_uuid() + segment name in discovery; a peer is
 * SHM-reachable iff its host uuid equals ours and dart_shm_attach succeeds. */
int dart_shm_host_match(const uint8_t peer_host[16], const uint8_t our_host[16]);

/* === INTEGRATION (implemented under #ifdef DART_SHM) ========================
 *
 * dart_plat (add behind the existing Windows/POSIX split):
 *   void *dart_plat_shm_create(const char *name, size_t bytes, void **handle);
 *   void *dart_plat_shm_attach(const char *name, size_t *out_bytes, void **handle);
 *   void  dart_plat_shm_detach(void *base, size_t bytes, void *handle, int unlink_it);
 *   void  dart_plat_host_uuid(uint8_t out[16]);             (boot id / machine guid)
 *   uint64_t dart_plat_atomic_load64 / _store64(volatile uint64_t*[, v]);  (generation)
 *
 * transport (the per-peer lane + the new submessage; the only core change):
 *   - per-peer flag peer_shm[] (node sets it; like the existing peer_local/peer_frag)
 *   - a history sample may be chunk-backed: a publish that hands in an external
 *     buffer (the chunk) + its descriptor, so dart_send does not memcpy (zero copy)
 *   - SHM-DATA submessage: byte0 = DATA | DART_F_SHM, [alias][base seqno][count]
 *     [24-byte descriptor]. Writer lane emits it for an SHM peer instead of frags;
 *     reader marks [base,base+count) delivered, hands the descriptor up flagged.
 *   - delivery carries an "is SHM descriptor" flag to the node (the public app
 *     on_message is unchanged; the node wraps it -- see below)
 *
 * node (wiring):
 *   - advertise: meta blob -> ver 3, insert [u8 shm][u8 host[16]][u64 segment_id]
 *     between the frag prefix and the interest list (ver-2 peers ignore it)
 *   - on peer up: if peer.shm and host matches ours, dart_shm_attach its segment and
 *     set peer_shm in the transport; on down/dormant, detach / clear it
 *   - dart_node_loan(n, ch, len, &ptr) / dart_node_publish(n, ch): loan a chunk for
 *     the channel's next history slot, app fills ptr, publish hands the chunk +
 *     descriptor to the transport. dart_node_send keeps working (one-copy into the
 *     chunk when the channel has any SHM peer, else plain inline)
 *   - on receive: the node's on_message wrapper sees the SHM flag, dart_shm_read the
 *     descriptor, calls the app on_message with the in-place pointer
 */

#ifdef __cplusplus
}
#endif
#endif /* DART_SHM_H */
#endif /* !DART_TRANSPORT_SANS_IO */

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
#ifdef DART_SHM
#define DART_F_SHM    0x20u     /* DATA: SHM-DATA -- body is a descriptor, no payload */
#endif

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
#ifdef DART_SHM
    uint8_t  shm;        /* 1 = SHM-backed: bytes live in shm_buf, desc set, buf unused */
    const uint8_t *shm_buf;            /* external chunk payload (remote peers fragment from it) */
    uint8_t  desc[DART_SHM_DESC_BYTES];/* descriptor sent to SHM peers as one SHM-DATA */
#endif
} dart_wsample;

#ifdef DART_SHM
/* where a sample's bytes live: the external chunk for SHM samples, else our buf */
static const uint8_t *dart__sbuf(const dart_wsample *s){ return s->shm ? s->shm_buf : s->buf; }
#else
static const uint8_t *dart__sbuf(const dart_wsample *s){ return s->buf; }
#endif

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
#ifdef DART_SHM
    uint8_t  shm_fail;      /* consecutive SHM-DATA resolve failures at deliver_upto */
#endif
} dart_rproxy;

#ifdef DART_SHM
#ifndef DART_SHM_MAX_RETRY
#define DART_SHM_MAX_RETRY 8u   /* give up on an unresolvable descriptor after this many */
#endif
#endif

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
    uint8_t     *peer_dormant;/* [max_peers] 1 = silent (discovery DROP): out of flow control,
                                 proxies + reader position preserved for a same-incarnation resume */
    uint16_t    *peer_frag; /* [max_peers] each peer's advertised fragment size (writer side) */
    uint16_t     frag;      /* this node's fragment size: what we fragment our sends into */
#ifdef DART_SHM
    uint8_t     *peer_shm;  /* [max_peers] 1 = peer can receive SHM-DATA (same host, attached) */
#endif
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
      uint8_t  *pdm= (uint8_t*) dart_take(b, np*sizeof(uint8_t), 1);
      uint16_t *pf = (uint16_t*)dart_take(b, np*sizeof(uint16_t), 2);
#ifdef DART_SHM
      uint8_t  *psh= (uint8_t*) dart_take(b, np*sizeof(uint8_t), 1);
#endif
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
          st->peer_dormant=pdm; st->peer_frag=pf;
          st->frag = cfg->frag_payload ? cfg->frag_payload : DART_FRAG_PAYLOAD;
          if (st->frag < DART_FRAG_PAYLOAD_MIN) st->frag = DART_FRAG_PAYLOAD_MIN;
          if (st->frag > DART_FRAG_PAYLOAD_MAX) st->frag = DART_FRAG_PAYLOAD_MAX;
          st->peer_pub_bm=pb; st->peer_sub_bm=sb; st->bmlen=bml;
          st->chans=ch; st->wprox=wp; st->rprox=rp; st->repoch=1;
          st->lane_next=ln; st->lane_inq=li;
          st->dest_head=dh; st->dest_tail=dt; st->dest_inq=di; st->destq=dq;
          st->alias_ci=ac; st->amax=mids;
          memset(pu,0,np); memset(pl,0,np); memset(pdm,0,np);
          { uint32_t k; for (k=0;k<np;k++) pf[k]=DART_FRAG_PAYLOAD; }  /* set per peer on add */
#ifdef DART_SHM
          st->peer_shm=psh; memset(psh,0,np);
#endif
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
    /* announce this incarnation once so a caught-up (idle, non-pinging) writer
       re-joins and replays. A genuine discovery blip keeps its position through
       dart_peer_dormant/resume and never lands here, so a single ACKNACK suffices. */
    if (st->chans[c].qos.reliability==DART_RELIABLE){
        r->ack_pending=1; r->ack_due_us=0;
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
    st->peer_dormant[free]=0;
    st->peer_frag[free]=dart__clamp_frag(peer_frag);
#ifdef DART_SHM
    st->peer_shm[free]=0;   /* node sets it once the peer's segment is attached */
#endif
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
    st->peer_used[s]=0; st->peer_dormant[s]=0;
#ifdef DART_SHM
    st->peer_shm[s]=0;
#endif
}

/* A peer fell silent (discovery timeout): keep every proxy and the reader's
 * deliver position, just drop the peer from flow control so a dead reader can't
 * stall the writer and a dead writer isn't acked. State revives via dart_peer_resume. */
void dart_peer_dormant(dart_state *st, uint32_t id){
    int s = dart_peer_slot(st,id);
    if (s>=0) st->peer_dormant[s]=1;
}

/* A dormant peer's SAME incarnation returned: re-include it in flow control and
 * re-report each reader position so the writer fills any gap (the reader dedups any
 * replay for free). The writer side needs nothing proactive; the reader's ACKNACK
 * re-arms its heartbeats. Proxies and deliver_upto were never touched, so no dup,
 * no loss. */
void dart_peer_resume(dart_state *st, uint32_t id){
    int s = dart_peer_slot(st,id); uint16_t c; uint32_t np;
    if (s<0) return;
    st->peer_dormant[s]=0;
    np = st->cfg.max_peers;
    for (c=0;c<st->cfg.n_channels;c++){
        dart_wproxy *w=&st->wprox[(size_t)c*np+s];
        dart_rproxy *r=&st->rprox[(size_t)c*np+s];
        if (st->chans[c].qos.reliability!=DART_RELIABLE) continue;
        if (r->used){ r->ack_pending=1; r->ack_due_us=0; }  /* report our position now */
        if (w->used || r->used) dart__lane_wake(st,c,(uint16_t)s);
    }
}

/* update a peer's advertised fragment size (its blob may arrive after first contact) */
void dart_peer_set_frag(dart_state *st, uint32_t id, uint16_t peer_frag){
    int s = dart_peer_slot(st,id);
    if (s>=0) st->peer_frag[s]=dart__clamp_frag(peer_frag);
}

#ifdef DART_SHM
void dart_peer_set_shm(dart_state *st, uint32_t id, int is_shm){
    int s = dart_peer_slot(st,id);
    if (s>=0) st->peer_shm[s]=(uint8_t)(is_shm?1:0);
}
#endif

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
            if (st->wprox[(size_t)ci*np+p].used && !st->peer_dormant[p]) dart__lane_wake(st, ci, p);
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
#ifdef DART_SHM
    ch->hist[ch->hist_head].shm = 0;   /* an inline send: this slot is not SHM-backed */
#endif
    dart__commit(st, (uint16_t)ci, len);
    return 0;
}

#ifdef DART_SHM
/* publish a sample whose bytes live in an external (shared-memory) chunk: store the
 * chunk pointer + descriptor on the history slot without copying. Remote peers
 * fragment from the chunk; SHM peers get the one-submessage descriptor. */
int dart_send_shm(dart_state *st, uint16_t channel, const void *chunk, size_t len,
                  const uint8_t *desc, uint64_t now){
    int ci; dart_channel *ch; dart_wsample *slot;
    (void)now;
    ch = dart_chan(st, channel, &ci);
    if (!ch) return -1;
    if (len > 65535u*(uint32_t)st->frag) return -2;      /* wire fragment-count cap */
    if (ch->role == DART_SUB_ONLY || ch->role == DART_INACTIVE) return -3;
    slot = &ch->hist[ch->hist_head];
    slot->shm = 1;
    slot->shm_buf = (const uint8_t*)chunk;
    memcpy(slot->desc, desc, DART_SHM_DESC_BYTES);
    dart__commit(st, (uint16_t)ci, len);
    return 0;
}
#endif

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
        if (w->used && !st->peer_dormant[p] && w->acked_upto < slot->base + slot->count) return 1;
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
        if (w->used && !st->peer_dormant[p] && w->acked_upto < ch->next_seqno) return 0;  /* reader still behind */
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

#ifdef DART_SHM
/* 1 if the channel is non-multicast, has >=1 matched reader, and EVERY matched
 * (non-dormant) reader is SHM-capable -> the node may publish this message via SHM.
 * One non-SHM (remote) reader forces inline UDP for the whole message. */
int dart_writer_shm_eligible(dart_state *st, uint16_t channel){
    int ci; dart_channel *ch = dart_chan(st, channel, &ci);
    uint32_t np, p; int any=0;
    if (!ch || ch->multicast) return 0;
    np = st->cfg.max_peers;
    for (p=0;p<np;p++){
        if (!st->wprox[(size_t)ci*np+p].used || st->peer_dormant[p]) continue;
        if (!st->peer_shm[p]) return 0;
        any = 1;
    }
    return any;
}
/* the history slot the next publish will occupy (so the node binds a chunk to it) */
uint16_t dart_channel_hist_head(dart_state *st, uint16_t channel){
    int ci; dart_channel *ch = dart_chan(st, channel, &ci);
    return ch ? ch->hist_head : 0;
}
#endif

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
#ifdef DART_SHM
/* SHM-DATA: one submessage covers [base, base+count); body is the descriptor, no
 * payload. 37 bytes = 1 (type|F_SHM) + 2 (alias) + 8 (base) + 2 (count) + 24 (desc). */
#define DART_SHM_DATA_BYTES (13u + DART_SHM_DESC_BYTES)
static size_t dart_mk_shm(uint8_t *o, uint16_t alias, uint64_t base, uint16_t count,
                          const uint8_t *desc){
    o[0]=(uint8_t)(DART_DATA|DART_F_SHM); dart_w16(o+1,alias);
    dart_w64(o+3,base); dart_w16(o+11,count);
    memcpy(o+13,desc,DART_SHM_DESC_BYTES);
    return DART_SHM_DATA_BYTES;
}
#endif
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

#ifdef DART_SHM
/* reader side: handle an SHM-DATA submessage. It covers [base, base+count) in one
 * shot (payload is in shared memory), so there is no reassembly -- just ordering,
 * then hand the descriptor to on_shm (the node resolves + delivers + acks). The gap
 * case re-uses the normal NACK window (dart_reader_emit's !asm_active branch). */
static void dart_reader_shm(dart_state *st, int ci, int pslot, const uint8_t *p, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    dart_rproxy *r=&st->rprox[(size_t)ci*st->cfg.max_peers+pslot];
    int reliable = (ch->qos.reliability==DART_RELIABLE);
    uint64_t base = dart_r64(p+3);
    uint16_t count = dart_r16(p+11);
    const uint8_t *desc = p+13;          /* DART_SHM_DESC_BYTES */
    if (!r->used || count==0) return;
    if (base < r->deliver_upto) return;                 /* old/dup */
    if (base > r->deliver_upto){
        if (reliable && r->started){                    /* gap: arm a NACK for the window */
            if (base+count-1 > r->hb_last) r->hb_last = base+count-1;
            if (!r->ack_pending){ r->ack_pending=1; r->ack_due_us=now + ch->qos.repair_delay_us; }
            dart__lane_wake(st,(uint16_t)ci,(uint32_t)pslot);
            return;
        }
        if (r->started)                                 /* best-effort / first contact: adopt */
            dart__event(st, DART_MSG_LOST, (uint16_t)ci, st->peer_ids[pslot],
                        r->deliver_upto, base - r->deliver_upto, "message(s) lost");
        r->deliver_upto = base;
    }
    r->started = 1; r->asm_active = 0;
    /* in order (base == deliver_upto). Resolve the chunk; advance + ack ONLY if the
       node delivered. A failed resolve (recycled, or a transient unattachable segment)
       leaves the gap so the reliability layer repairs it (re-sent descriptor) or skips
       it (writer HB, sample evicted). A persistently unresolvable descriptor (mis-
       configured SHM constants) would loop, so after DART_SHM_MAX_RETRY tries we skip
       it loudly instead of wedging. */
    {   int ok = st->cfg.on_shm &&
                 st->cfg.on_shm(st->cfg.user, (uint16_t)ci, st->peer_ids[pslot], desc);
        if (ok){
            r->shm_fail = 0;
            r->deliver_upto = base + count;
            if (reliable){                              /* ack AFTER delivery (zero-copy invariant) */
                r->ack_pending=1; r->ack_due_us=now + ch->qos.repair_delay_us;
                dart__lane_wake(st,(uint16_t)ci,(uint32_t)pslot);
            }
            return;
        }
        if (reliable && ++r->shm_fail >= DART_SHM_MAX_RETRY){
            dart__event(st, DART_MSG_LOST, (uint16_t)ci, st->peer_ids[pslot],
                        base, count, "SHM descriptor unresolvable (check DART_SHM_* build constants)");
            r->shm_fail = 0;
            r->deliver_upto = base + count;             /* give up: skip past it, ack the new edge */
            r->ack_pending=1; r->ack_due_us=now + ch->qos.repair_delay_us;
        } else if (reliable){                           /* leave the gap, NACK for a re-send */
            if (base+count-1 > r->hb_last) r->hb_last = base+count-1;
            r->ack_pending=1; r->ack_due_us=now + ch->qos.repair_delay_us;
        } else {
            r->deliver_upto = base + count;             /* best-effort: no repair, drop it */
        }
        dart__lane_wake(st,(uint16_t)ci,(uint32_t)pslot);
    }
}
#endif

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
    r->started = 1;   /* writer engaged: position adopted */
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
    /* FUTURE OPT: the ack is armed AFTER on_message, but for UDP that order is
       INCIDENTAL -- the payload is already copied into r->asm_buf, which the reader
       owns, so the ack could be armed BEFORE delivery to release the writer's
       backpressure sooner. Safe to reorder for UDP and for one-copy SHM (reader still
       owns a copy). DO NOT reorder for zero-copy SHM: there on_message reads the
       writer's chunk in place and MUST finish before the ack, else the chunk can be
       recycled under the user. Gate any ack-early change on the delivery mode. */
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
#ifdef DART_SHM
        r->shm_fail=0;          /* skipped past the stuck descriptor: fresh count */
#endif
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
            case DART_DATA:
#ifdef DART_SHM
                            if (b0 & DART_F_SHM){ if (rem<DART_SHM_DATA_BYTES) return; sub=DART_SHM_DATA_BYTES; }
                            else
#endif
                            if (b0 & DART_F_SINGLE){ if (rem<13) return; sub=13u+(size_t)dart_r16(p+11); }
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
                case DART_DATA:
#ifdef DART_SHM
                                if (p[0] & DART_F_SHM){ dart_reader_shm(st,ci,ps,p,now); break; }
#endif
                                dart_reader_data(st,ci,ps,p,now); break;
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
    if (!w->used || st->peer_dormant[pslot]) return 0;   /* dormant: out of flow control */
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
#ifdef DART_SHM
                    if (st->peer_shm[pslot] && s->shm){   /* re-send the whole message as one SHM-DATA */
                        uint32_t j;
                        if (cap < DART_SHM_DATA_BYTES) return 0;
                        for (j=0;j<DART_NACK_WINDOW;j++){
                            uint64_t sq=w->nack_base+j;
                            if (sq>=s->base && sq<s->base+s->count) w->nack_bits &= ~(1u<<j);
                        }
                        if (w->nack_bits==0) w->has_nack=0;
                        return dart_mk_shm(out,alias,s->base,s->count,s->desc);
                    }
#endif
                    {
                    uint16_t fi=(uint16_t)(seqno - s->base);
                    uint32_t off=(uint32_t)fi*st->frag;
                    uint16_t plen=(uint16_t)((s->len-off)<st->frag?(s->len-off):st->frag);
                    if (cap < (size_t)(s->count==1?13u:21u)+(size_t)plen) return 0;   /* bit stays set */
                    w->nack_bits &= ~(1u<<i);
                    if (w->nack_bits==0) w->has_nack=0;
                    return dart_mk_data(out,alias,seqno,s,fi,dart__sbuf(s)+off,plen);
                    }
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
#ifdef DART_SHM
            /* peer_shm is set at attach (before data flows), so sent_upto sits at a
               sample boundary here: emit the whole message as one SHM-DATA */
            if (st->peer_shm[pslot] && s->shm){
                if (cap < DART_SHM_DATA_BYTES) return 0;
                w->sent_upto = s->base + s->count;
                return dart_mk_shm(out,alias,s->base,s->count,s->desc);
            }
#endif
            {
            uint16_t fi=(uint16_t)(seqno - s->base);
            uint32_t off=(uint32_t)fi*st->frag;
            uint16_t plen=(uint16_t)((s->len-off)<st->frag?(s->len-off):st->frag);
            if (cap < (size_t)(s->count==1?13u:21u)+(size_t)plen) return 0;
            w->sent_upto++;
            return dart_mk_data(out,alias,seqno,s,fi,dart__sbuf(s)+off,plen);
            }
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
    if (!r->used || st->peer_dormant[pslot]) return 0;   /* dormant: don't ack a silent writer */
    if (ch->qos.reliability!=DART_RELIABLE) return 0;
    if (cap<21) return 0;
    if (!r->ack_pending || now<r->ack_due_us) return 0;
    r->ack_pending=0;

    if (!r->asm_active){
        base=r->deliver_upto;
        if (!r->started){ nbits=0; bm=0; }   /* no position yet: F_UNPOS announces our epoch */
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
    return dart_mk_nack(out,alias,base,nbits,bm,r->epoch,
                      r->started ? 0 : (uint8_t)DART_F_UNPOS);
}

/* multicast: 1 if every matched subscriber has acked all data, so the group
 * heartbeat can stop until new data arrives or a new/lagging subscriber needs it */
static int dart__group_all_acked(dart_state *st, int ci){
    uint32_t np=st->cfg.max_peers, p; uint64_t seq=st->chans[ci].next_seqno;
    for (p=0;p<np;p++){
        dart_wproxy *w=&st->wprox[(size_t)ci*np+p];
        if (w->used && !st->peer_dormant[p] && w->acked_upto < seq) return 0;
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
    if (!st->peer_used[ps] || st->peer_dormant[ps]) return 0;   /* dormant: out of flow control */
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
        if (!st->peer_used[ps] || st->peer_dormant[ps]) continue;   /* dormant: out of flow control */
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
/* ===== dart_shm.c ===== */
/* dart_shm: the portable segment-mapping + chunk module behind dart_shm.h. Pure
 * over dart_plat (shm mapping, host uuid, the generation atomic); no transport or
 * node knowledge. Compiles to nothing without DART_SHM. See dart_shm.h. */


#ifdef DART_SHM
#include <string.h>
#include <stdlib.h>

/* little-endian scalar IO for the wire descriptor */
static void shm_w32(uint8_t *p, uint32_t v){ p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24); }
static void shm_w64(uint8_t *p, uint64_t v){ int i; for(i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }
static uint32_t shm_r32(const uint8_t *p){ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static uint64_t shm_r64(const uint8_t *p){ uint64_t v=0; int i; for(i=0;i<8;i++) v|=((uint64_t)p[i])<<(8*i); return v; }

size_t dart_shm_desc_encode(const dart_shm_desc *d, uint8_t out[DART_SHM_DESC_WIRE]){
    shm_w64(out,    d->segment_id);
    shm_w32(out+8,  d->chunk);
    shm_w32(out+12, d->length);
    shm_w64(out+16, d->generation);
    return DART_SHM_DESC_WIRE;
}
int dart_shm_desc_decode(dart_shm_desc *d, const uint8_t *in, size_t len){
    if (len < DART_SHM_DESC_WIRE) return 0;
    d->segment_id = shm_r64(in);
    d->chunk      = shm_r32(in+8);
    d->length     = shm_r32(in+12);
    d->generation = shm_r64(in+16);
    return 1;
}

uint32_t dart_shm_class_bytes(uint32_t k){ return DART_SHM_CLASS_BASE << (k*DART_SHM_CLASS_SHIFT); }
uint32_t dart_shm_class_for(uint32_t len){
    uint32_t k;
    for (k=0;k<DART_SHM_N_CLASSES;k++) if (dart_shm_class_bytes(k) >= len) return k;
    return DART_SHM_N_CLASSES;   /* bigger than the top class -> caller sends inline */
}

struct dart_shm_pool {
    void    *base;          /* mapping base */
    void    *handle;        /* OS handle for detach */
    size_t   map_bytes;     /* total mapped size */
    dart_shm_seg_hdr *hdr;
    uint8_t *chunks;        /* base of the chunk region */
    uint32_t chunk_bytes;
    uint32_t n_chunks;
    uint32_t stride;        /* per-chunk bytes incl. header */
    int      is_creator;
};

size_t dart_shm_state_bytes(void){ return (sizeof(struct dart_shm_pool) + 15u) & ~(size_t)15u; }

#define DART__SHM_HDR_SZ  ((uint32_t)((sizeof(dart_shm_seg_hdr) + 15u) & ~(size_t)15u))
#define DART__SHM_CHDR_SZ ((uint32_t)((sizeof(dart_shm_chunk_hdr) + 15u) & ~(size_t)15u))

static void dart__shm_geom(uint32_t chunk_bytes, uint32_t n_chunks,
                           uint32_t *out_stride, size_t *out_total){
    uint32_t cb = (chunk_bytes + 15u) & ~15u;
    uint32_t stride = DART__SHM_CHDR_SZ + cb;
    *out_stride = stride;
    *out_total  = (size_t)DART__SHM_HDR_SZ + (size_t)n_chunks * stride;
}

static dart_shm_chunk_hdr *dart__shm_chunk_hdr(struct dart_shm_pool *p, uint32_t i){
    return (dart_shm_chunk_hdr*)(p->chunks + (size_t)i * p->stride);
}
static uint8_t *dart__shm_chunk_pay(struct dart_shm_pool *p, uint32_t i){
    return (uint8_t*)dart__shm_chunk_hdr(p, i) + DART__SHM_CHDR_SZ;
}

dart_shm_pool *dart_shm_create(void *pool_mem, const dart_shm_config *cfg){
    struct dart_shm_pool *p = (struct dart_shm_pool*)pool_mem;
    uint32_t cb = cfg->chunk_bytes ? cfg->chunk_bytes : DART_SHM_CHUNK_BYTES;
    uint32_t nc = cfg->n_chunks    ? cfg->n_chunks    : DART_SHM_CHUNKS;
    uint32_t stride; size_t total; void *handle = NULL, *base; uint32_t i;
    if (!p || !cfg) return NULL;
    dart__shm_geom(cb, nc, &stride, &total);
    base = dart_plat_shm_create(cfg->name, total, &handle);
    if (!base) return NULL;
    memset(p, 0, sizeof *p);
    p->base = base; p->handle = handle; p->map_bytes = total;
    p->hdr = (dart_shm_seg_hdr*)base;
    p->chunks = (uint8_t*)base + DART__SHM_HDR_SZ;
    p->chunk_bytes = cb; p->n_chunks = nc; p->stride = stride; p->is_creator = 1;
    /* the segment starts zero-filled; stamp the header and clear generations */
    p->hdr->magic = DART_SHM_MAGIC; p->hdr->version = DART_SHM_VERSION;
    p->hdr->segment_id = cfg->segment_id; p->hdr->chunk_bytes = cb; p->hdr->n_chunks = nc;
    p->hdr->owner_pid = dart_plat_pid();
    dart_plat_host_uuid(p->hdr->owner_host);
    for (i = 0; i < nc; i++){ dart_shm_chunk_hdr *c = dart__shm_chunk_hdr(p, i); c->generation = 0; c->length = 0; }
    return p;
}

dart_shm_pool *dart_shm_attach(void *pool_mem, const dart_shm_config *cfg){
    struct dart_shm_pool *p = (struct dart_shm_pool*)pool_mem;
    uint32_t cb, nc, stride; size_t map_bytes = 0, expect;
    void *handle = NULL, *base; uint8_t ours[16];
    if (!p || !cfg) return NULL;
    /* map the whole OS object; its geometry (chunk_bytes/n_chunks) comes from the
       header the writer stamped, so the reader needs to know nothing up front --
       cfg's chunk_bytes/n_chunks are create-only. */
    base = dart_plat_shm_attach(cfg->name, &map_bytes, &handle);
    if (!base) return NULL;
    memset(p, 0, sizeof *p);
    p->base = base; p->handle = handle; p->map_bytes = map_bytes;
    p->hdr = (dart_shm_seg_hdr*)base;
    dart_plat_host_uuid(ours);
    cb = p->hdr->chunk_bytes; nc = p->hdr->n_chunks;
    dart__shm_geom(cb, nc, &stride, &expect);
    /* reject a stale/foreign/mismatched/truncated segment -> caller falls back to UDP */
    if (p->hdr->magic != DART_SHM_MAGIC || p->hdr->version != DART_SHM_VERSION ||
        memcmp(p->hdr->owner_host, ours, 16) != 0 ||
        cb == 0 || nc == 0 || expect > map_bytes){
        dart_plat_shm_detach(base, map_bytes, handle, 0);
        return NULL;
    }
    p->chunks = (uint8_t*)base + DART__SHM_HDR_SZ;
    p->chunk_bytes = cb; p->n_chunks = nc; p->stride = stride; p->is_creator = 0;
    return p;
}

void *dart_shm_chunk(dart_shm_pool *p, uint32_t chunk, uint32_t *out_cap){
    if (!p || chunk >= p->n_chunks) return NULL;
    if (out_cap) *out_cap = p->chunk_bytes;
    return dart__shm_chunk_pay(p, chunk);
}

void dart_shm_stamp(dart_shm_pool *p, uint32_t chunk, uint32_t len, dart_shm_desc *out){
    dart_shm_chunk_hdr *c;
    uint64_t gen;
    if (!p || chunk >= p->n_chunks) return;
    c = dart__shm_chunk_hdr(p, chunk);
    c->length = len;
    gen = c->generation + 1u;                       /* bump so a straggler sees the reuse */
    dart_plat_atomic_store64(&c->generation, gen);  /* release: publishes the payload writes */
    if (out){ out->segment_id = p->hdr->segment_id; out->chunk = chunk; out->length = len; out->generation = gen; }
}

const void *dart_shm_read(dart_shm_pool *p, const dart_shm_desc *d, uint32_t *out_len){
    dart_shm_chunk_hdr *c;
    if (!p || !d || d->chunk >= p->n_chunks) return NULL;
    c = dart__shm_chunk_hdr(p, d->chunk);
    if (dart_plat_atomic_load64(&c->generation) != d->generation) return NULL;  /* recycled */
    if (d->length > p->chunk_bytes) return NULL;
    if (out_len) *out_len = d->length;
    return dart__shm_chunk_pay(p, d->chunk);
}

int dart_shm_verify(dart_shm_pool *p, const dart_shm_desc *d){
    dart_shm_chunk_hdr *c;
    if (!p || !d || d->chunk >= p->n_chunks) return 0;
    c = dart__shm_chunk_hdr(p, d->chunk);
    return dart_plat_atomic_load64(&c->generation) == d->generation;
}

void dart_shm_detach(dart_shm_pool *p){
    if (!p || !p->base) return;
    dart_plat_shm_detach(p->base, p->map_bytes, p->handle, p->is_creator);
    p->base = NULL; p->handle = NULL;
}

int dart_shm_host_match(const uint8_t peer_host[16], const uint8_t our_host[16]){
    return memcmp(peer_host, our_host, 16) == 0;
}

#endif /* DART_SHM */
/* ===== dart_node.c ===== */
/* NODE runtime: owns the data socket, drives discovery, wires peers into the
 * transport. All OS access goes through dart_plat. See dart_node.h. */

#ifdef DART_SHM
#endif
#include <string.h>

typedef struct {
    uint8_t  used;
    uint8_t  dormant;  /* discovery DROPPED it: kept for a same-incarnation resume */
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
#ifdef DART_SHM
    /* zero-fragment same-host path: lazy per-size-class segments (see dart_shm.h) */
    uint8_t        shm_cap;       /* 1 = an allocator is set, so SHM is usable */
    uint8_t        shm_host[16];  /* our host uuid (advertised; same-host check) */
    uint64_t       shm_base;      /* per-node segment id base; low 19 bits = (channel<<3)|class */
    dart_message_fn shm_user_on_message;  /* the app's real callback (we wrap it) */
    dart_alloc_fn  shm_alloc;     /* one-copy receive scratch (== cfg.allocator) */
    void          *shm_scratch; uint32_t shm_scratch_cap;
    void         **shm_pool;      /* [nchan*N_CLASSES] our (channel,class) segments (NULL = not created) */
    uint8_t       *shm_pool_mem;  /* arena: nchan * N_CLASSES * dart_shm_state_bytes */
    uint16_t       shm_nchan;
    uint64_t      *shm_rseg;      /* [rmax] attached reader-segment ids (0 = empty) */
    uint8_t       *shm_rpool_mem; /* [rmax * dart_shm_state_bytes] */
    uint16_t       shm_rmax;
    uint32_t       shm_tx, shm_rx;/* messages published / delivered via SHM (observability) */
#endif
};

/* Discovery-announce metadata: a versioned blob carrying this node's fragment size,
 * (v3) its SHM capability + host uuid, then its interest list. Layout:
 *   v2: ['D','N',2, frag_lo, frag_hi,                      <interest>]   prefix 5
 *   v3: ['D','N',3, frag_lo, frag_hi, shm, host[16],       <interest>]   prefix 22
 * Decode is version-aware so v2 (non-SHM) and v3 nodes interop; frag is at [3..4] in
 * both. We write v3 when DART_SHM is compiled, v2 otherwise. */
#define DART__META_PREFIX_V2 5u
#define DART__META_PREFIX_V3 22u
#ifdef DART_SHM
#define DART__META_PREFIX DART__META_PREFIX_V3   /* what WE write */
#else
#define DART__META_PREFIX DART__META_PREFIX_V2
#endif

static int dart__meta_ok(const uint8_t *meta, uint16_t mlen){
    return meta && mlen >= 5 && meta[0]=='D' && meta[1]=='N' && (meta[2]==2 || meta[2]==3);
}
static uint16_t dart__meta_pfx(const uint8_t *meta){
    return meta[2]==3 ? DART__META_PREFIX_V3 : DART__META_PREFIX_V2;
}
static uint16_t dart__meta_frag(const uint8_t *meta, uint16_t mlen){
    if (!dart__meta_ok(meta, mlen)) return 0;
    return (uint16_t)(meta[3] | ((uint16_t)meta[4] << 8));
}
/* Locate the interest sub-blob within a peer's meta; NULL/0 if absent. */
static const uint8_t *dart__meta_interest(const uint8_t *meta, uint16_t mlen, size_t *out_len){
    uint16_t pfx;
    if (!dart__meta_ok(meta, mlen) || mlen < (pfx = dart__meta_pfx(meta))){ *out_len = 0; return NULL; }
    *out_len = (size_t)(mlen - pfx);
    return meta + pfx;
}
#ifdef DART_SHM
/* A peer's SHM capability + host uuid (v3 only); 1 if SHM-capable, fills host[16]. */
static int dart__meta_shm(const uint8_t *meta, uint16_t mlen, uint8_t host[16]){
    if (!dart__meta_ok(meta, mlen) || meta[2]!=3 || mlen < DART__META_PREFIX_V3 || !meta[5]) return 0;
    memcpy(host, meta+6, 16);
    return 1;
}
#endif
/* (Re)build our blob: prefix (frag [+ shm/host]) + current interest list. */
static uint16_t dart__meta_build(dart_node *n){
    uint8_t *o = n->disc_meta; size_t il; uint16_t pre = DART__META_PREFIX;
    o[0]='D'; o[1]='N'; o[2]=(uint8_t)(DART__META_PREFIX==DART__META_PREFIX_V3 ? 3 : 2);
    o[3]=(uint8_t)(n->frag_size & 0xFF); o[4]=(uint8_t)(n->frag_size >> 8);
#ifdef DART_SHM
    o[5]=(uint8_t)(n->shm_cap?1:0); memcpy(o+6, n->shm_host, 16);
#endif
    il = dart_build_interest(n->tr, o + pre, n->disc_meta_cap - pre);
    return (uint16_t)(pre + il);
}

/* announce blob capacity: frag prefix + the largest interest list our channels can
 * produce, capped to fit one (IP-fragmentable) UDP datagram */
static uint16_t dart__node_meta_cap(const dart_node_config *cfg){
    size_t mc = DART__META_PREFIX + dart_interest_max(cfg->n_channels);
    if (mc > 65000u) mc = 65000u;
    return (uint16_t)mc;
}

#ifdef DART_SHM
/* reader-pool cache capacity: a same-host peer publishes on its channels, one segment
 * each, so size to peers x channels (clamped to the u16 the cache index uses) */
static uint16_t dart__node_shm_rmax(uint16_t mp, uint16_t nch){
    uint32_t r = (uint32_t)mp * (nch ? nch : 1u) * DART_SHM_N_CLASSES;
    return (uint16_t)(r > 0xFFFFu ? 0xFFFFu : r);
}
/* arena bytes for the SHM bookkeeping (per-channel pool ptrs + handles + locked class,
 * plus the reader caches; the segments themselves are OS-mapped, lazily, outside it) */
static size_t dart__node_shm_bytes(const dart_node_config *cfg, uint16_t mp){
    size_t sb = dart_shm_state_bytes();
    uint32_t np = (uint32_t)cfg->n_channels * DART_SHM_N_CLASSES;   /* (channel,class) segments */
    uint16_t rmax = dart__node_shm_rmax(mp, cfg->n_channels);
    size_t s = 0;
    s += ((size_t)np*sizeof(void*) + 15u)&~(size_t)15u;      /* (channel,class) pool ptr array */
    s += ((size_t)np*sb + 15u)&~(size_t)15u;                 /* (channel,class) pool handles */
    s += ((size_t)rmax*8u + 15u)&~(size_t)15u;               /* reader segment ids */
    s += ((size_t)rmax*sb + 15u)&~(size_t)15u;               /* reader pool handles */
    return s;
}
#endif

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

static int dart__node_find_id(dart_node *n, uint32_t id);   /* peer table lookups, defined below */

#ifdef DART_SHM
/* a peer can receive our SHM-DATA iff we are SHM-capable, it advertised SHM, and its
 * host uuid equals ours (same kernel). Set the transport's per-peer flag. */
static void dart__node_set_peer_shm(dart_node *n, uint32_t id, const uint8_t *meta, uint16_t mlen){
    uint8_t host[16];
    int shm = n->shm_cap && dart__meta_shm(meta, mlen, host) &&
              dart_shm_host_match(host, n->shm_host);
    dart_peer_set_shm(n->tr, id, shm);
}
#endif

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
#ifdef DART_SHM
            dart__node_set_peer_shm(n, id, meta, mlen);
#endif
            if (interest) dart_apply_peer_interest(n->tr, id, interest, ilen);
            if (n->peers[i].dormant){    /* a DROPPED peer's same incarnation returned: resume */
                n->peers[i].dormant = 0;
                dart_peer_resume(n->tr, id);   /* keeps reader position; writer fills any gap */
                if (n->on_event){
                    dart_event ev; memset(&ev, 0, sizeof ev);
                    ev.kind=DART_PEER_UP; ev.peer=id; ev.detail="peer resumed";
                    memcpy(ev.ip, addr->ip, 16); ev.ip_len=addr->ip_len; ev.port=addr->port;
                    n->on_event(n->user_data, &ev);
                }
            }
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
#ifdef DART_SHM
    dart__node_set_peer_shm(n, id, meta, mlen);
#endif
    if (interest) dart_apply_peer_interest(n->tr, id, interest, ilen);
    if (n->on_event){
        dart_event ev; memset(&ev, 0, sizeof ev);
        ev.kind=DART_PEER_UP; ev.peer=id; ev.detail="peer discovered";
        memcpy(ev.ip, addr->ip, 16); ev.ip_len=addr->ip_len; ev.port=addr->port;
        n->on_event(n->user_data, &ev);
    }
}
static void dart__node_down(void *u, uint32_t id, dart_discovery_down_reason reason){
    dart_node *n=(dart_node*)u; int i = dart__node_find_id(n, id);
    if (reason == DART_DISCOVERY_DROP){
        /* fell silent: keep transport state so a same-incarnation return resumes
           losslessly; just stop flow-controlling it and tell the app once */
        if (i>=0 && !n->peers[i].dormant){
            n->peers[i].dormant = 1;
            dart_peer_dormant(n->tr, id);
            if (n->on_event){
                dart_event ev; memset(&ev, 0, sizeof ev);
                ev.kind=DART_PEER_DOWN; ev.peer=id; ev.detail="peer dropped";
                n->on_event(n->user_data, &ev);
            }
        }
    } else {   /* GONE: said BYE or its slot was reclaimed; free the transport state */
        int notify = (i>=0 && !n->peers[i].dormant);   /* active->gone: app not yet told */
        if (i>=0) n->peers[i].used=0;
        dart_peer_remove(n->tr, id);
        if (notify && n->on_event){
            dart_event ev; memset(&ev, 0, sizeof ev);
            ev.kind=DART_PEER_DOWN; ev.peer=id; ev.detail="peer lost";
            n->on_event(n->user_data, &ev);
        }
    }
}
static void dart__node_refused(void *u, const dart_discovery_addr *addr){
    dart_node *n=(dart_node*)u;
    if (n->on_event){
        dart_event ev; memset(&ev, 0, sizeof ev);
        ev.kind=DART_PEER_REFUSED; ev.detail="peer table full (all active)";
        memcpy(ev.ip, addr->ip, 16); ev.ip_len=addr->ip_len; ev.port=addr->port;
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

#ifdef DART_SHM
/* OS object name "/dart.shm.<16 hex>" -- valid on POSIX (leading /) and Windows. */
static void dart__shm_name(char *buf, uint64_t seg){
    static const char hx[] = "0123456789abcdef";
    const char pre[] = "/dart.shm."; int i, k = 0;
    while (pre[k]){ buf[k] = pre[k]; k++; }
    for (i=15;i>=0;i--) buf[k++] = hx[(seg >> (4*i)) & 0xF];
    buf[k] = 0;
}
/* lazily create our per-channel segment: chunk_bytes = the channel's locked size class,
 * n_chunks = its keep_last, so history slot i binds chunk i (no free list). NULL on fail. */
static dart_shm_pool *dart__shm_chan_pool(dart_node *n, uint16_t ch, uint32_t k, uint16_t keep_last){
    dart_shm_config c; uint8_t *mem; uint64_t seg; size_t idx;
    if (k >= DART_SHM_N_CLASSES) return NULL;
    idx = (size_t)ch * DART_SHM_N_CLASSES + k;
    if (n->shm_pool[idx]) return (dart_shm_pool*)n->shm_pool[idx];
    memset(&c, 0, sizeof c);
    seg = n->shm_base | ((uint64_t)ch << 3) | (uint64_t)k;   /* low 3 bits class, next 16 channel */
    c.segment_id = seg;
    dart__shm_name(c.name, seg);
    c.chunk_bytes = dart_shm_class_bytes(k);
    c.n_chunks = keep_last ? keep_last : 1u;
    mem = n->shm_pool_mem + idx * dart_shm_state_bytes();
    n->shm_pool[idx] = dart_shm_create(mem, &c);
    return (dart_shm_pool*)n->shm_pool[idx];
}
/* lazily attach a peer's segment by id (class is in the low bits); cache it. */
static dart_shm_pool *dart__shm_reader_pool(dart_node *n, uint64_t seg){
    uint16_t i, slot = 0xFFFF; dart_shm_config c; uint8_t *mem; size_t sb;
    sb = dart_shm_state_bytes();
    for (i=0;i<n->shm_rmax;i++){
        if (n->shm_rseg[i]==seg) return (dart_shm_pool*)(n->shm_rpool_mem + (size_t)i*sb);
        if (n->shm_rseg[i]==0 && slot==0xFFFF) slot = i;
    }
    if (slot==0xFFFF) return NULL;                         /* cache full */
    memset(&c, 0, sizeof c);
    c.segment_id = seg; dart__shm_name(c.name, seg);        /* attach maps whole + reads geometry */
    mem = n->shm_rpool_mem + (size_t)slot*sb;
    if (!dart_shm_attach(mem, &c)) return NULL;
    n->shm_rseg[slot] = seg;
    return (dart_shm_pool*)mem;
}
/* transport->node delivery wrappers (the transport's user is the node so on_shm can
 * reach SHM state; the app's single on_message sees identical bytes inline or SHM).
 * Because the transport shares one user across on_message/on_shm/allocator, we also
 * wrap the allocator to forward the app's real user_data. */
static void *dart__node_alloc(void *u, void *ptr, size_t size){
    dart_node *n = (dart_node*)u;
    return n->shm_alloc(n->user_data, ptr, size);
}
static void dart__node_on_message(void *u, uint16_t ch, uint32_t from, const void *data, size_t len){
    dart_node *n = (dart_node*)u;
    if (n->shm_user_on_message) n->shm_user_on_message(n->user_data, ch, from, data, len);
}
static int dart__node_on_shm(void *u, uint16_t ch, uint32_t from, const uint8_t *desc){
    dart_node *n = (dart_node*)u; dart_shm_desc d; dart_shm_pool *rp; const void *p; uint32_t len;
    if (!dart_shm_desc_decode(&d, desc, DART_SHM_DESC_WIRE)) return 0;
    rp = dart__shm_reader_pool(n, d.segment_id);
    if (!rp) return 0;                                     /* can't attach -> reader NACKs (repair) */
    p = dart_shm_read(rp, &d, &len);                       /* seqlock head: generation == descriptor? */
    if (!p) return 0;                                      /* recycled -> NACK -> repair or skip */
    /* one-copy: copy out of shared memory so the user owns the bytes (ack-timing safe) */
    if (len > n->shm_scratch_cap){
        void *nb = n->shm_alloc(n->user_data, n->shm_scratch, len?len:1u);
        if (!nb) return 0;
        n->shm_scratch = nb; n->shm_scratch_cap = len;
    }
    memcpy(n->shm_scratch, p, len);
    if (!dart_shm_verify(rp, &d)) return 0;                /* seqlock tail: writer recycled mid-copy -> torn -> drop */
    n->shm_rx++;
    if (n->shm_user_on_message) n->shm_user_on_message(n->user_data, ch, from, n->shm_scratch, len);
    return 1;
}
#endif

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
    {   size_t shm_sz = 0;
#ifdef DART_SHM
        shm_sz = dart__node_shm_bytes(cfg, mp);
#endif
        return 32u + node_sz + tbl_sz + meta_sz + disc_sz + tr_sz + shm_sz;
    }
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
#ifdef DART_SHM
    /* SHM is usable only with an allocator (the one-copy receive scratch + dynamic
       buffers); advertise capability accordingly and wrap the transport callbacks so
       on_shm can reach node state. The big segments are OS-mapped lazily, not here. */
    n->shm_cap = (uint8_t)(cfg->allocator != NULL);
    if (n->shm_cap){
        dart_plat_host_uuid(n->shm_host);
        if (!dart_plat_random(&n->shm_base, sizeof n->shm_base)) n->shm_base = dart_plat_pid();
        n->shm_base ^= (uint64_t)dart_plat_pid() << 32;    /* fold in pid for cross-process uniqueness */
        n->shm_base &= ~(((uint64_t)1u << 19) - 1u);       /* low 19 bits: 3 class + 16 channel index */
        if (n->shm_base == 0) n->shm_base = (uint64_t)1u << 19;
        n->shm_user_on_message = cfg->on_message;
        n->shm_alloc = cfg->allocator;
        tc.on_message = dart__node_on_message;
        tc.on_shm     = dart__node_on_shm;
        tc.allocator  = dart__node_alloc;
        tc.user       = n;
    }
#endif
    p = base + node_sz;
    n->peers=(dart__nodepeer*)p; memset(n->peers,0,(size_t)mp*sizeof(dart__nodepeer));
    p += tbl_sz;
    n->disc_meta = p; n->disc_meta_cap = dart__node_meta_cap(cfg);
    p += meta_sz;

    n->tr = dart_init(p, tr_sz, &tc);
    if (!n->tr){ dart_plat_cleanup(); return NULL; }
    p += tr_sz;
#ifdef DART_SHM
    {   size_t sb = dart_shm_state_bytes();
        uint32_t np = (uint32_t)cfg->n_channels * DART_SHM_N_CLASSES;
        uint16_t rmax = dart__node_shm_rmax(mp, cfg->n_channels); uint32_t i;
        n->shm_nchan = cfg->n_channels; n->shm_rmax = rmax;
        n->shm_pool = (void**)p;     p += ((size_t)np*sizeof(void*) + 15u)&~(size_t)15u;
        n->shm_pool_mem = p;         p += ((size_t)np*sb + 15u)&~(size_t)15u;
        n->shm_rseg = (uint64_t*)p;  p += ((size_t)rmax*8u + 15u)&~(size_t)15u;
        n->shm_rpool_mem = p;        p += ((size_t)rmax*sb + 15u)&~(size_t)15u;
        for (i=0;i<np;i++) n->shm_pool[i]=NULL;
        for (i=0;i<rmax;i++) n->shm_rseg[i]=0;
    }
#endif

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

    dc.disc.on_peer_up      = dart__node_up;
    dc.disc.on_peer_down    = dart__node_down;
    dc.disc.on_peer_refused = dart__node_refused;
    dc.disc.user            = n;
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
#ifdef DART_SHM
    if (n->shm_cap && len>0 && channel < n->shm_nchan && dart_writer_shm_eligible(n->tr, channel)){
        uint16_t keep_last = (q && q->keep_last) ? q->keep_last : 1u;
        /* a hint (shm_max_bytes / max_message_bytes) pins the channel to one class, so
           same-sized traffic reuses a single pre-sized segment; without it each message
           uses its own size class's segment, created on demand. Per channel either way. */
        uint32_t hint = q ? (q->shm_max_bytes ? q->shm_max_bytes : q->max_message_bytes) : 0u;
        uint32_t k = dart_shm_class_for(hint ? hint : (uint32_t)len);
        /* fits its class (with a hint, the hinted class) -> publish via SHM; chunk index =
           the history slot this send will occupy, so chunk i binds slot i (no free list) */
        if (k < DART_SHM_N_CLASSES && (uint32_t)len <= dart_shm_class_bytes(k)){
            dart_shm_pool *pool = dart__shm_chan_pool(n, channel, k, keep_last);
            uint16_t slot = dart_channel_hist_head(n->tr, channel);
            void *cp = pool ? dart_shm_chunk(pool, slot, NULL) : NULL;
            if (cp){
                dart_shm_desc d; uint8_t desc[DART_SHM_DESC_WIRE];
                memcpy(cp, data, len);                       /* one-copy write into shm */
                dart_shm_stamp(pool, slot, (uint32_t)len, &d);
                dart_shm_desc_encode(&d, desc);
                if (dart_send_shm(n->tr, channel, cp, len, desc, dart_plat_now_us())==0){
                    n->shm_tx++;
                    return 0;
                }
            }
        }
    }
#endif
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

#ifdef DART_SHM
void dart_node_shm_stats(dart_node *n, uint32_t *sent, uint32_t *recv){
    if (sent) *sent = n->shm_tx;
    if (recv) *recv = n->shm_rx;
}
#endif

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
#ifdef DART_SHM
    if (n->shm_cap){
        size_t sb = dart_shm_state_bytes(); uint32_t i, np = (uint32_t)n->shm_nchan * DART_SHM_N_CLASSES;
        for (i=0;i<np;i++)
            if (n->shm_pool[i]) dart_shm_detach((dart_shm_pool*)n->shm_pool[i]);   /* unlinks ours */
        for (i=0;i<n->shm_rmax;i++)
            if (n->shm_rseg[i]) dart_shm_detach((dart_shm_pool*)(n->shm_rpool_mem + (size_t)i*sb));
        if (n->shm_scratch) n->shm_alloc(n->user_data, n->shm_scratch, 0);
    }
#endif
    dart_plat_cleanup();
}
#endif /* !DART_TRANSPORT_SANS_IO */
#endif /* DART_TRANSPORT_IMPLEMENTATION */
