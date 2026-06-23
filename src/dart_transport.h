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
#define DART_FRAG_PAYLOAD 1350u          /* default bytes of message data per fragment */
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

/* Cumulative reliable-repair counters for a channel, summed over its peer/reader
 * proxies (writer side = this node publishing; reader side = subscribing). Always on;
 * each field is a plain bump on a path that already runs. The per-second deltas of
 * frags_resent (writer) and non-dup frags_recv (reader) are repair throughput; a flat
 * HOL snapshot (dart_reader_progress) with rising nacks_sent is a wedged stream. */
typedef struct {
    /* writer side (node as publisher) */
    uint64_t nacks_recv;     /* ACKNACKs received that requested missing fragments (nbits>0) */
    uint64_t frags_resent;   /* DATA fragments retransmitted to satisfy a NACK */
    uint64_t frags_sent;     /* all DATA fragments sent (new + repair); repair fraction = resent/sent */
    /* reader side (node as subscriber) */
    uint64_t nacks_sent;     /* repair requests we emitted (ACKNACK with nbits>0) */
    uint64_t frags_recv;     /* all DATA fragments received, including duplicates */
    uint64_t frags_dup;      /* fragments received that we already held (repair overlap / waste) */
    uint64_t msgs_skipped;   /* messages given up on (sum of DART_MSG_LOST counts) */
    /* repair-arm attribution (diagnostic): each counts a 0->1 arming of the reader's
       pending-ACK, by what triggered it. arms_data = a DATA/SHM-DATA arrival re-armed
       it; arms_hb = a heartbeat did. A stall where arms_hb ticks at the heartbeat rate
       while arms_data is flat means the reader only re-asks on arrivals, not on a timer. */
    uint64_t arms_data;
    uint64_t arms_hb;
    /* RX disposition of received DATA fragments (diagnostic): every DATA fragment that
       reaches the reader is one of these. frags_recv counts ACCEPTED only (base ==
       deliver_upto), so "recv 0" while the writer floods can mean the fragments are
       landing but being rejected as old/ahead, not that they aren't arriving. */
    uint64_t frags_old;        /* base < deliver_upto: whole message already delivered/skipped */
    uint64_t frags_ahead;      /* base > deliver_upto: a future message (no out-of-order buffer) */
    uint64_t frags_malformed;  /* count==0 || frag>=count, or not subscribed */
} dart_repair_stats_t;

/* Fill *out with the channel's cumulative repair counters (zeroed if channel is
 * out of range). Per-channel aggregate; a per-peer breakdown is a later extension. */
void      dart_repair_stats(dart_state *st, uint16_t channel, dart_repair_stats_t *out);

/* Writer-side: number of reader lanes on this channel with a pending repair NACK to
 * service. 0 => the writer has nothing to resend right now (idle for lack of NACKs).
 * Diagnostic for the backpressure stall (distinguishes "no NACKs" from "resends
 * dropped"); sampled by the node's in-pump probe. */
int       dart_repair_pending(dart_state *st, uint16_t channel);

/* Head-of-line reassembly snapshot for the in-progress message from `peer` on
 * `channel` (the message at the reader's deliver_upto). Returns 1 and fills the
 * out-params if a message is mid-reassembly, else 0.
 *   base_seqno : first seqno of the in-progress message (= reader deliver_upto)
 *   have       : fragments received so far (popcount of the reassembly bitmap)
 *   total      : fragments the message needs
 * `have` rising across calls => repair is crawling forward; flat => wedged. Any
 * out-pointer may be NULL. Wrapped as dart_node_reader_progress. */
int       dart_reader_progress(dart_state *st, uint16_t channel, uint32_t peer,
                            uint64_t *base_seqno, uint32_t *have, uint32_t *total);

/* Feed a received datagram, tagged with the peer it came from. */
void      dart_on_datagram(dart_state *st, uint32_t from_peer, const void *dg, size_t len,
                         uint64_t now_us);

/* Pull one outgoing datagram (may batch submessages for one peer). Returns 1 and
 * fills *to_peer/out/out_len, or 0 when nothing is due. Loop until 0; pass DART_DGRAM_MAX cap. */
int       dart_poll_send(dart_state *st, uint32_t *to_peer, void *out, size_t cap,
                       size_t *out_len, uint64_t now_us);

/* Absolute us of the next internal timer (deferred ack / NACK / heartbeat), or 0 if
 * none is pending. Cap a blocking poll at this so a due timer is serviced on time
 * instead of waiting out the poll quantum or the amortized sweep. */
uint64_t  dart_next_deadline_us(dart_state *st);

#ifdef __cplusplus
}
#endif
#endif /* DART_TRANSPORT_H */
