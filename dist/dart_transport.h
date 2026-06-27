/* GENERATED single-header build. DO NOT EDIT.
 * DART = Discovery And Realtime Transport. Amalgamated from src/ by the CMake
 * build (tools/pack.cmake). Edit the split sources in src/ and rebuild (or run
 * tools/pack.cmake) to regenerate. See the flag scheme in tools/pack.cmake.
 */
#if defined(__GNUC__)   /* let the section markers below fold quietly */
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif
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

#pragma region transport/core.h
/* sans-IO reliable-UDP transport core: no socket, clock, or heap. Feed it
 * datagrams + now_us + a peer set; it returns datagrams to send and delivers
 * reassembled messages. RTPS-inspired, not wire-compatible. Layer DartNode.h
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
/* The UDP fragment size is set PER NODE at init (DartConfig.frag_payload) and
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

#ifndef DART_NODE_NAME_MAX
#define DART_NODE_NAME_MAX 32u           /* max node-name bytes carried in the announce meta blob */
#endif

/* Max pub+sub topic count accepted in a peer's interest list; sizes the per-peer
 * alias table. Auto-raised to 2*n_channels; raise (a compile bound) only to accept
 * a peer with more topics. */
#ifndef DART_META_MAX_IDS
#define DART_META_MAX_IDS 256u
#endif

typedef enum { DART_BEST_EFFORT = 0, DART_RELIABLE = 1 } DartReliability;
/* DART_INACTIVE = declared but off (resources stay allocated; dart_set_role flips it) */
typedef enum { DART_PUBSUB = 0, DART_PUB_ONLY = 1, DART_SUB_ONLY = 2,
               DART_INACTIVE = 3 } DartRole;

/* Every field except reliability is zero-means-default, so a reliable channel is
 * just { .reliability = DART_RELIABLE }. */
typedef struct {
    DartReliability reliability;
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
} DartQos;

/* A channel (topic). Cross-peer identity is the name (64-bit hash); the LOCAL
 * handle for dart_send / on_message is the channel's index in channels[]. */
typedef struct {
    const char *name;  /* topic name = cross-peer identity. Required, same on every node, <= DART_TOPIC_NAME_MAX */
    DartQos   qos;
    uint8_t  role;     /* DartRole; 0 = pub+sub */
    uint8_t  multicast;/* 1 = use this topic's multicast group for data (publisher opt-in, reader
                          joins). Repairs/acks stay unicast. Group 239.255.<domain&255>.<id&255> */
} DartChannelDef;

/* dart_poll_send destination: a peer id, or a multicast group flagged by the top
 * bit (low byte = topic identity & 0xFF; node maps it to 239.255.<domain&255>.<sel>). */
#define DART_DEST_GROUP_BIT      0x80000000u
#define DART_DEST_GROUP(sel)     (DART_DEST_GROUP_BIT | (uint32_t)(sel))
#define DART_DEST_IS_GROUP(d)    (((d) & DART_DEST_GROUP_BIT) != 0u)
#define DART_DEST_GROUP_CHAN(d)  ((uint16_t)((d) & 0xFFFFu))
/* group selector = low byte of the topic identity. One source for the core's group
 * send and the node's group join, so the two can never derive it differently. */
#define DART_DEST_GROUP_SEL(identity)  ((uint16_t)((identity) & 0xFFu))

/* A complete message; channel is the local handle. Do not call back into dart_*. */
typedef void (*DartMessageFn)(void *user, uint16_t channel, uint32_t from_peer,
                             const void *data, size_t len);

#ifdef DART_SHM
/* SHM delivery: the transport reassembled nothing -- it hands the node the
 * DART_SHM_DESC_BYTES descriptor from an SHM-DATA submessage and the node resolves it
 * to bytes and calls the user's on_message. Returns 1 if delivered, 0 if it could not
 * resolve the chunk (recycled / unattachable) -- then the reader leaves the gap so the
 * reliability layer repairs or skips it. Internal (transport->node); the user's
 * on_message is unchanged and never sees this. */
typedef int (*i_DartShmMsgFn)(void *user, uint16_t channel, uint32_t from_peer,
                             const uint8_t *desc);
#endif

/* Transport events (optional), delivered through one on_event. The transport is
 * independent of discovery/node: it emits only its own kinds. The node maps these
 * into its app-facing DartEvent (node/core.h); a sans-IO transport user handles them
 * directly. Flat and self-describing: read only the fields named for the .kind. */
typedef enum {
    DART_TRANSPORT_MSG_LOST,        /* messages skipped: .channel, .peer, .lost_first .. +.lost_count-1 */
    DART_TRANSPORT_MSG_TOO_BIG,     /* a received message exceeded max_message_bytes (.too_big_bytes), skipped */
    DART_TRANSPORT_NAME_COLLISION,  /* a peer's name hashes to ours but differs (.identity, .detail = our name), refused */
    DART_TRANSPORT_QOS_INCOMPATIBLE /* a reliable subscriber refused a best-effort publisher (.channel, .peer); .detail = our channel name */
} DartTransportEventKind;

typedef struct {
    DartTransportEventKind kind;
    const char *detail;        /* short human label (NAME_COLLISION / QOS_INCOMPATIBLE: our channel name) */
    void       *user;          /* DartConfig.user */
    uint32_t   peer;           /* peer id (0 = n/a) */
    uint16_t   channel;        /* local channel handle */
    uint64_t   lost_first;     /* MSG_LOST: first skipped seqno */
    uint64_t   lost_count;     /* MSG_LOST: number of messages skipped */
    uint64_t   too_big_bytes;  /* MSG_TOO_BIG: size of the dropped message */
    uint64_t   identity;       /* NAME_COLLISION: the colliding 64-bit topic identity */
} DartTransportEvent;
typedef void (*DartTransportEventFn)(const DartTransportEvent *ev);

/* Largest message the wire can carry (65535 fragments, ~64 MB by default). */
#define DART_MESSAGE_MAX (65535u * DART_FRAG_PAYLOAD_MAX)

/* Optional realloc-style hook for growable messages (ptr NULL = alloc, size 0 =
 * free). Set => user channels grow to fit, max_message_bytes may be 0. NULL
 * (default, embedded) => fixed buffers, a bigger message is refused/skipped.
 * Pair with dart_destroy to free what it allocated. */
typedef void *(*DartAllocFn)(void *user, void *ptr, size_t size);

/* Two ways to populate the channel table:
 *   fixed/at-init : channels != NULL, n_channels = its length. Slots are defined now;
 *                   buffers come from the arena (or the allocator if one is set).
 *   reserve/lazy  : channels == NULL, n_channels = the reserved capacity, allocator set.
 *                   All slots start DART_INACTIVE; fill them later with dart_channel_define
 *                   (this is how the node's runtime dart_node_create_channel works). */
typedef struct {
    const DartChannelDef *channels;     /* NULL = reserve mode (see above) */
    uint16_t              n_channels;   /* defined count, or reserved capacity in reserve mode */
    uint16_t              max_peers;
    uint16_t              frag_payload; /* UDP fragment size this node sends with; 0 =
                                           DART_FRAG_PAYLOAD. Clamped to [MIN, MAX]. */
    DartMessageFn         on_message;
#ifdef DART_SHM
    i_DartShmMsgFn         on_shm;     /* SHM-DATA delivery (descriptor); the node resolves it */
#endif
    DartTransportEventFn  on_event;   /* optional: transport events (loss/too-big/collision/qos) */
    DartAllocFn           allocator;  /* optional: set => dynamic message sizing */
    void                 *user;
} DartConfig;

typedef struct DartTransportState DartTransportState;

size_t    dart_required_memory(const DartConfig *cfg);
DartTransportState *dart_init(void *mem, size_t mem_size, const DartConfig *cfg);
/* Relocate a live transport into new_mem (>= dart_required_memory at the grown counts),
 * re-striding its tables to new_max_peers/new_n_channels and carrying live reliability
 * state (positions, history, in-flight repair) across. Heap buffers are not in the arena,
 * so the caller frees old's arena block afterward but must NOT dart_destroy old. Returns
 * the new state, or NULL on failure (old is left intact). Dynamic-mode growth only. */
DartTransportState *dart_migrate(DartTransportState *old, void *new_mem, size_t new_cap,
                        uint16_t new_max_peers, uint16_t new_n_channels);
/* Free allocator-allocated buffers (dynamic channels). No-op in fixed mode; the
 * arena stays the caller's. The node calls it from close. */
void      dart_destroy(DartTransportState *st);

/* 64-bit topic identity from a name (FNV-1a): matches topics, derives the group. */
uint64_t  dart_topic_id(const char *name);
uint64_t  dart_channel_identity(const DartChannelDef *def);   /* = dart_topic_id(def->name) */

/* Normalize a UDP fragment size: 0 -> DART_FRAG_PAYLOAD, then clamp to [MIN,MAX].
 * The rule dart_init and the node's announce blob both apply (single source). */
uint16_t  dart_clamp_frag(uint16_t frag_payload);

/* A new peer matches nothing until dart_apply_peer_interest feeds its interest
 * list (carried in its discovery announce). peer_is_local: 1 if on this host.
 * peer_frag: that peer's advertised UDP fragment size (from discovery), used to
 * reassemble its messages; 0 = DART_FRAG_PAYLOAD. Clamped to [MIN, MAX]. */
void      dart_peer_add   (DartTransportState *st, uint32_t peer_id, int peer_is_local, uint16_t peer_frag);
void      dart_peer_remove(DartTransportState *st, uint32_t peer_id);

/* Discovery-blip lifecycle: a peer that fell silent (discovery timeout) is made
 * DORMANT instead of removed, so its reader position survives and a same-incarnation
 * return resumes losslessly. Dormant peers are dropped from flow control (the writer
 * stops heartbeating/draining them so a dead reader can't stall it; the reader stops
 * acking them), but their proxies and deliver position are preserved. dart_peer_resume
 * re-includes the peer and re-reports reader positions so the writer fills any gap.
 * Both no-op for an unknown peer; the node drives them off discovery DROP/return. */
void      dart_peer_dormant(DartTransportState *st, uint32_t peer_id);
void      dart_peer_resume (DartTransportState *st, uint32_t peer_id);
/* Update a peer's advertised UDP fragment size (its announce blob may arrive after
 * first contact). Clamped to [MIN, MAX]; no-op for an unknown peer. */
void      dart_peer_set_frag(DartTransportState *st, uint32_t peer_id, uint16_t peer_frag);

/* Interest exchange (the node carries these in discovery announces; sans-IO callers
 * disseminate them however they like). dart_build_interest serializes OUR pub/sub
 * set into out ([u16 npub][u16 nsub] then [u16 alias][u8 namelen][name] entries),
 * returning bytes written or 0 if cap is too small; size out via dart_interest_max.
 * dart_apply_peer_interest applies a peer's serialized set, (re)matching channels;
 * it is idempotent. Re-build + re-disseminate after dart_set_role. */
size_t    dart_interest_max(uint16_t n_channels);
size_t    dart_build_interest(DartTransportState *st, void *out, size_t cap);
void      dart_apply_peer_interest(DartTransportState *st, uint32_t peer_id, const void *blob, size_t len);

/* Discovery-announce meta blob (sans-IO codec). A versioned, opaque-to-discovery
 * payload wrapping this node's UDP fragment size, SHM capability + host uuid, and its
 * interest list: the transport's OVERLAY, carried opaquely inside discovery's announce blob
 * (the node name lives in discovery's own section, not here). Layout:
 *   v6: ['D','N',6, frag_lo, frag_hi,                <interest>]
 *   v7: ['D','N',7, frag_lo, frag_hi, shm, host[16], <interest>]
 * frag sits at [3..4] in both; v7 adds the SHM byte + host. dart_meta_build writes v7 when
 * DART_SHM is compiled, v6 otherwise. */

/* Bytes to reserve for the overlay: prefix + the largest interest list n_channels can
 * produce, capped to one (IP-fragmentable) UDP datagram. Sizes discovery's meta_capacity. */
uint16_t  dart_meta_capacity(uint16_t n_channels);
/* Build the overlay into out[cap] (cap >= dart_meta_capacity): the version prefix (frag_size,
 * plus shm_capable + host[16] when DART_SHM is compiled), then st's interest list. Returns
 * total bytes; host may be NULL when !shm_capable. The node NAME is not here: it rides
 * discovery's own section of the announce blob. */
uint16_t  dart_meta_build(DartTransportState *st, uint8_t *out, uint16_t cap,
                       uint16_t frag_size, int shm_capable, const uint8_t host[16]);
/* A peer's advertised UDP fragment size from the overlay; 0 if malformed. */
uint16_t  dart_meta_frag(const uint8_t *meta, uint16_t meta_len);
/* Locate the interest sub-blob inside the overlay; NULL + *out_len 0 if absent. */
const uint8_t *dart_meta_interest(const uint8_t *meta, uint16_t meta_len, size_t *out_len);

/* One advertised topic, as decoded by dart_meta_interest_next. name points into the
 * source overlay (NOT NUL-terminated), so keep that blob alive while reading it. */
typedef struct {
    uint16_t    alias;      /* the advertiser's local channel index (opaque to us) */
    uint8_t     reliable;   /* flags bit 0: offered (pub) / requested (sub) reliability */
    uint8_t     is_pub;     /* 1 = a publish entry, 0 = a subscribe entry */
    const char *name;       /* topic name in the source blob, name_len bytes (not NUL-terminated) */
    uint8_t     name_len;
} DartTopic;

/* Iterator state for dart_meta_interest_next: zero-initialize, then call until it
 * returns 0. The fields are internal walk state, not for direct use. */
typedef struct {
    uint16_t pub_left;   /* publish entries still to yield */
    uint16_t sub_left;   /* subscribe entries still to yield */
    uint32_t off;        /* byte offset of the next entry within the overlay */
    uint8_t  started;    /* 0 until the first call parses the [npub][nsub] header */
} DartInterestIter;

/* Walk a peer's interest list (the publish entries, then the subscribe entries) one
 * topic at a time, so consumers stop re-implementing the [u16 npub][u16 nsub] +
 * [u16 alias][u8 flags][u8 namelen][name] format. Pass the same overlay/len each call
 * with a zeroed DartInterestIter; returns 1 and fills *out, or 0 at the end (or on a
 * malformed/truncated blob: it stops rather than reading past the end). Usage:
 *   DartInterestIter it = {0}; DartTopic t;
 *   while (dart_meta_interest_next(meta, meta_len, &it, &t)) { ... } */
int       dart_meta_interest_next(const uint8_t *meta, uint16_t meta_len,
                       DartInterestIter *it, DartTopic *out);
#ifdef DART_SHM
/* A peer's SHM capability + host uuid (v3/v5 blobs only): 1 if SHM-capable (fills
 * host[16]), else 0. */
int       dart_meta_shm(const uint8_t *meta, uint16_t meta_len, uint8_t host[16]);
#endif

/* Change a channel's role at runtime (rematches peers locally; caller re-advertises
 * interest). A (re)subscribe joins like a late joiner. Returns 0 ok, <0 unknown. */
int       dart_set_role(DartTransportState *st, uint16_t channel, uint8_t role);

/* Define a reserved (currently inactive) channel slot at runtime: set its name/qos/
 * role/multicast, allocate its history ring via the allocator, and rematch known peers.
 * Reserve mode only (an allocator is required). Returns 0 ok, or negative: -1 bad index/
 * name / slot already defined / no allocator, -4 out of memory. Re-advertise interest
 * after (the node bumps its discovery announce). */
int       dart_channel_define(DartTransportState *st, uint16_t channel, const DartChannelDef *def);

/* The channel's topic name (NULL + *len 0 if undefined or out of range), for surfacing
 * it on a delivered message. *len (may be NULL) gets the name length. The name is a
 * local lookup; it is never on the data path. */
const char *dart_channel_name(DartTransportState *st, uint16_t channel, uint8_t *len);

/* dart_send / dart_send_shm result: 0 ok, negative on error (returned as int). */
typedef enum {
    DART_OK             =  0,
    DART_ERR_NO_CHANNEL = -1,  /* channel index out of range */
    DART_ERR_TOO_BIG    = -2,  /* exceeds max_message_bytes or the wire fragment cap */
    DART_ERR_ROLE       = -3,  /* channel is SUB_ONLY or INACTIVE: cannot publish */
    DART_ERR_OOM        = -4   /* dynamic allocator returned NULL */
} DartResult;

/* Publish a message to all peers. Returns DART_OK, or a negative DartResult. */
int       dart_send(DartTransportState *st, uint16_t channel, const void *data, size_t len,
                  uint64_t now_us);

#ifdef DART_SHM
/* Publish a message whose payload lives in an external shared-memory buffer: the
 * transport stores the sample referencing chunk (NOT copied) plus the descriptor,
 * fragments from chunk for non-SHM peers, and sends ONE SHM-DATA (the descriptor) to
 * SHM-capable peers. desc is DART_SHM_DESC_BYTES. Same return as dart_send. The chunk
 * must stay valid until the sample leaves history (acked / evicted). */
int       dart_send_shm(DartTransportState *st, uint16_t channel, const void *chunk, size_t len,
                      const uint8_t *desc, uint64_t now_us);
/* Mark whether a peer can receive SHM-DATA (same host AND its segment is attached).
 * Off by default; the node sets it on attach, clears it on dormant/remove. */
void      dart_peer_set_shm(DartTransportState *st, uint32_t peer_id, int is_shm);
/* 1 if every matched reader of channel is SHM-capable and it is non-multicast, so a
 * publish may go via SHM (else inline). The node checks this per message. */
int       dart_writer_shm_eligible(DartTransportState *st, uint16_t channel);
/* The history slot the next publish to channel will occupy (binds chunk<->slot). */
uint16_t  dart_channel_hist_head(DartTransportState *st, uint16_t channel);
#endif

/* The channel's qos as stored at init; NULL if unknown. */
const DartQos *dart_channel_qos(DartTransportState *st, uint16_t channel);

/* 1 if appending here would overwrite history not yet acked by every reader. A
 * writer pumps while this is 1, then sends anyway after qos.backpressure_wait_us. */
int       dart_send_would_evict(DartTransportState *st, uint16_t channel);

/* 1 if every live reader has acked all messages on this reliable channel (so a
 * writer may close without truncating). Best-effort/unknown return 1. Wrapped as
 * dart_channel_drain. */
int       dart_send_drained(DartTransportState *st, uint16_t channel);

/* Peers currently matched as readers (subscribers) of this channel. 0 = a publish
 * goes nowhere; a one-shot publisher can poll this before sending. */
int       dart_writer_match_count(DartTransportState *st, uint16_t channel);

/* Per-peer match summary (diagnostic): how many channels we now PUBLISH to this peer
 * (it subscribes and we publish) and how many we RECEIVE from it (it publishes and we
 * subscribe). Counts unicast lanes; either out-pointer may be NULL, both 0 for an
 * unknown peer. Surfaced on DART_PEER_INTEREST so a caller can watch a connection form. */
void      dart_peer_match_counts(DartTransportState *st, uint32_t peer_id,
                                 uint16_t *publish_to, uint16_t *receive_from);

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
} DartRepairStats;

/* Fill *out with the channel's cumulative repair counters (zeroed if channel is
 * out of range). Per-channel aggregate; a per-peer breakdown is a later extension. */
void      dart_repair_stats(DartTransportState *st, uint16_t channel, DartRepairStats *out);

/* Writer-side: number of reader lanes on this channel with a pending repair NACK to
 * service. 0 => the writer has nothing to resend right now (idle for lack of NACKs).
 * Diagnostic for the backpressure stall (distinguishes "no NACKs" from "resends
 * dropped"); sampled by the node's in-pump probe. */
int       dart_repair_pending(DartTransportState *st, uint16_t channel);

/* Head-of-line reassembly snapshot for the in-progress message from `peer` on
 * `channel` (the message at the reader's deliver_upto). Returns 1 and fills the
 * out-params if a message is mid-reassembly, else 0.
 *   base_seqno : first seqno of the in-progress message (= reader deliver_upto)
 *   have       : fragments received so far (popcount of the reassembly bitmap)
 *   total      : fragments the message needs
 * `have` rising across calls => repair is crawling forward; flat => wedged. Any
 * out-pointer may be NULL. Wrapped as dart_channel_reader_progress. */
int       dart_reader_progress(DartTransportState *st, uint16_t channel, uint32_t peer,
                            uint64_t *base_seqno, uint32_t *have, uint32_t *total);

/* Feed a received datagram, tagged with the peer it came from. */
void      dart_on_datagram(DartTransportState *st, uint32_t from_peer, const void *datagram, size_t len,
                         uint64_t now_us);

/* Pull one outgoing datagram (may batch submessages for one peer). Returns 1 and
 * fills *to_peer/out/out_len, or 0 when nothing is due. Loop until 0; pass DART_DGRAM_MAX cap. */
int       dart_poll_send(DartTransportState *st, uint32_t *to_peer, void *out, size_t cap,
                       size_t *out_len, uint64_t now_us);

/* Absolute us of the next internal timer (deferred ack / NACK / heartbeat), or 0 if
 * none is pending. Cap a blocking poll at this so a due timer is serviced on time
 * instead of waiting out the poll quantum or the amortized sweep. */
uint64_t  dart_next_deadline_us(DartTransportState *st);

#ifdef __cplusplus
}
#endif
#endif /* DART_TRANSPORT_H */
#pragma endregion

#ifndef DART_TRANSPORT_SANS_IO
#pragma region node/core.h
/* sans-IO NODE core: the peer table (peer id <-> physical address), the
 * discovery->transport lifecycle (add / resume / dormant / remove + interest +
 * frag + out-of-band capability, plus PEER_UP/DOWN/REFUSED events), and address
 * resolution. No socket, clock, or platform: a node RUNTIME owns IO and drives
 * this, so a second transport (serial, Bluetooth, ...) is a new runtime over this
 * same core with no sans-IO change. See node/runtime.h for the IO layer and the
 * public dart_node_* API.
 *
 * The one transport-specific thing the core does NOT own is mapping the abstract
 * destination to wire bytes: it resolves a peer id to a physical address record and
 * hands that to the runtime, which sends it however its link works. The group case
 * (DART_DEST_GROUP) is resolved entirely by the runtime, since the multicast-group
 * address is a per-transport convention (UDP: 239.255.<domain>.<sel>). */
#ifndef DART_NODE_CORE_H
#define DART_NODE_CORE_H


#ifdef __cplusplus
extern "C" {
#endif

/* The node's app-facing event: the union the user receives via DartNodeOpts.on_event.
 * The node maps discovery's DartDiscoveryEvent (the peer kinds) and the transport's
 * DartTransportEvent (the message/QoS kinds) into this one type, and adds its own
 * (PEER_INTEREST, MCAST_JOIN_FAILED). Flat and self-describing: read only the fields
 * named for the .kind. dart_event_str formats any of them as a one-line message. */
typedef enum {
    DART_PEER_UP,        /* peer discovered or resumed: .peer, .ip/.ip_len/.port */
    DART_PEER_DOWN,      /* peer lost or fell silent: .peer */
    DART_PEER_INTEREST,  /* a peer's interest list was (re)applied: .peer, .publish_topics, .receive_topics */
    DART_MSG_LOST,       /* messages skipped: .channel, .peer, .lost_first .. +.lost_count-1 */
    DART_MSG_TOO_BIG,    /* a received message exceeded max_message_bytes (.too_big_bytes), skipped */
    DART_NAME_COLLISION, /* a peer's name hashes to ours but differs (.identity, .detail = our name), refused */
    DART_QOS_INCOMPATIBLE, /* a reliable subscriber refused a best-effort publisher (.channel, .peer); .detail = our channel name */
    DART_PEER_REFUSED,   /* peer table full of active peers: a new peer was refused (.ip/.ip_len/.port) */
    DART_MCAST_JOIN_FAILED /* a channel's multicast group join failed, over the OS membership cap (.channel) */
} DartEventKind;

typedef struct {
    DartEventKind kind;
    const char *detail;        /* short human-readable label (NAME_COLLISION / QOS_INCOMPATIBLE: our channel name) */
    void       *user;          /* your DartNodeOpts.user_data (mirrors DartMsg.user) */
    uint32_t   peer;           /* peer id, where applicable (0 = n/a) */
    uint16_t   channel;        /* local channel handle, where applicable */
    uint8_t    ip[16];         /* PEER_UP / PEER_REFUSED: peer address (network order) */
    uint8_t    ip_len;         /* PEER_UP / PEER_REFUSED: 4 or 16; else 0 */
    uint16_t   port;           /* PEER_UP / PEER_REFUSED: peer data port */
    uint64_t   lost_first;     /* MSG_LOST: first skipped seqno */
    uint64_t   lost_count;     /* MSG_LOST: number of messages skipped */
    uint64_t   too_big_bytes;  /* MSG_TOO_BIG: size of the dropped message */
    uint64_t   identity;       /* NAME_COLLISION: the colliding 64-bit topic identity */
    uint16_t   publish_topics; /* PEER_INTEREST: topics we now publish to this peer */
    uint16_t   receive_topics; /* PEER_INTEREST: topics we now receive from this peer */
} DartEvent;
typedef void (*DartEventFn)(const DartEvent *ev);

/* Format ev as a one-line human-readable message into buf (always NUL-terminated,
 * truncated to cap). Returns buf. */
const char *dart_event_str(const DartEvent *ev, char *buf, size_t cap);

/* Runtime hook: 1 if a physical address is on this host (a route probe, on UDP),
 * so the peer is flagged out-of-band (SHM) eligible. NULL => every peer is remote. */
typedef int (*i_DartNodeIsLocalFn)(void *user, const uint8_t *ip, uint8_t ip_len);

/* Everything the core needs from the runtime, set once at init. The peer table itself
 * lives in the discovery core: the node core delegates id<->address resolution and peer
 * naming to it (dart_discovery_*), and stores its small per-peer transport-lifecycle
 * state in the discovery peer's user scratch (dart_node_core_peer_user_bytes). */
typedef struct {
    DartTransportState           *transport;    /* the peers are wired into this (sans-IO) */
    DartDiscoveryState  *discovery;    /* the peer table (id<->addr, name, user scratch); may be
                                          NULL at init, then bound via dart_node_core_bind_discovery */
    uint16_t              n_channels;    /* sizes the announce-blob buffer */
    uint16_t              frag_size;     /* our UDP fragment size, baked into the overlay */
    DartEventFn         on_event;      /* PEER_UP/DOWN/REFUSED sink (optional) */
    void                 *user;          /* passed to on_event */
    i_DartNodeIsLocalFn is_local;      /* runtime route probe (optional) */
    void                 *is_local_user;
    int                   oob_capable;   /* 1 = we can deliver out-of-band (SHM) payloads */
    uint8_t               oob_host[16];  /* our host id; a peer is OOB-reachable iff it matches */
} i_DartNodeCoreConfig;

typedef struct i_DartNodeCore i_DartNodeCore;

size_t          dart_node_core_required_memory(uint16_t n_channels);
i_DartNodeCore *dart_node_core_init(void *mem, size_t mem_size, const i_DartNodeCoreConfig *cfg);
/* Relocate the sans-IO core into a bigger block at grown counts. The transport, discovery,
 * and announce-blob pointers are re-pointed by the caller after those move. Dynamic growth. */
i_DartNodeCore *dart_node_core_migrate(i_DartNodeCore *old, void *new_mem, size_t new_cap,
                                       uint16_t new_n_channels);
/* Bind (or rebind, after a migrate) the discovery core whose peer table this core delegates
 * to. The runtime calls it once discovery exists, and again after discovery relocates. */
void            dart_node_core_bind_discovery(i_DartNodeCore *c, DartDiscoveryState *discovery);
/* Bytes of per-peer scratch the core needs in the discovery peer table (its transport-
 * lifecycle state). The runtime sets discovery's cfg.peer_user_bytes to this. */
uint16_t        dart_node_core_peer_user_bytes(void);

/* The discovery announce blob this node sends: its frag size, OOB host, and interest
 * list. The core owns the buffer and builds it (the codec is dart_meta_* in the
 * transport core). build_meta (re)builds it from the core's current fields and returns
 * the length. meta returns the bytes + length for the runtime to feed to discovery.
 * Rebuild after a role change, then re-feed discovery. */
uint16_t        dart_node_core_build_meta(i_DartNodeCore *c);
const uint8_t  *dart_node_core_meta(i_DartNodeCore *c, uint16_t *len);

/* Discovery event sink: register as the discovery core's on_event (cfg.user = this
 * core). Demuxes the generic DartDiscoveryEvent (PEER_UP/DOWN/REFUSED), keeps the peer
 * table and transport peer set in lockstep (dormant/resume/evict), and fires the app's
 * PEER_UP/DOWN/INTEREST/REFUSED DartEvents (the node maps discovery's events to its own). */
void dart_node_core_on_disc_event(const DartDiscoveryEvent *ev);

/* A resolved outbound destination: a multicast group (by selector), or a unicast
 * peer (by physical address). The runtime turns this into wire bytes for its link. */
typedef struct {
    uint8_t  is_group;   /* 1 = group send, 0 = unicast peer */
    uint16_t group_sel;  /* group: the selector (low byte of topic identity) */
    uint8_t  ip[16];     /* unicast: peer physical address (IPv4 today) */
    uint8_t  ip_len;
    uint16_t port;       /* unicast: peer data port */
} i_DartNodeDest;

/* Destination resolution: the node-core/runtime boundary. resolve turns the transport's
 * abstract destination (dart_poll_send's to_peer) into a group or a peer address, so the
 * runtime only maps the result to wire bytes. The group address convention is the
 * runtime's (UDP uses 239.255.<domain>.<sel>). Returns 1 if sendable, 0 if a unicast
 * peer is unknown. id_for_addr maps an inbound source address back to a peer id (1 + *id
 * on a hit, else 0). */
int  dart_node_core_resolve(i_DartNodeCore *c, uint32_t to, i_DartNodeDest *out);
int  dart_node_core_id_for_addr(i_DartNodeCore *c, const uint8_t ip[4], uint16_t port, uint32_t *id);

/* A peer's human-readable name, learned from its announce blob: a NUL-terminated
 * pointer into the peer-table slot (stable until the peer is evicted). Non-empty for
 * any known peer ("unknown-peer" if its announce carried none); NULL only when id is
 * not a known peer. *out_len gets its length. For debug/observability only. */
const char *dart_node_core_peer_name(i_DartNodeCore *c, uint32_t id, uint8_t *out_len);

/* Read-only peer-table enumeration (diagnostics / tests). max_peers is the capacity;
 * peer_at fills the out-params for table slot in [0, max_peers) and returns 1 if it
 * holds a live peer, else 0. Any out-pointer may be NULL. */
uint16_t dart_node_core_max_peers(i_DartNodeCore *c);
int      dart_node_core_peer_at(i_DartNodeCore *c, uint16_t slot, uint32_t *id,
                                uint8_t ip[16], uint8_t *ip_len, uint16_t *port);

#ifdef __cplusplus
}
#endif
#endif /* DART_NODE_CORE_H */
#pragma endregion
#pragma region node/runtime.h
/* NODE runtime (the public dart_node_* / dart_channel_* API): owns the data sockets,
 * drives discovery and the clock, and wires peers into the transport via the sans-IO
 * node core (node/core.h). Channels are created at runtime and handed back as opaque
 * handles; a second transport would be a new runtime over that same core. */
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
    const DartDiscoveryAddr *seed_peers;   /* peers to also unicast announces to (port 0 =
                                                discovery_port), so discovery works without multicast */
    uint16_t              n_seed_peers;
    uint32_t              recv_buffer_bytes; /* data-socket SO_RCVBUF; 0 = OS default */
    uint32_t              send_buffer_bytes; /* data-socket SO_SNDBUF; 0 = OS default */
    uint16_t              fragment_size;     /* UDP payload bytes per fragment this node sends;
                                                0 = DART_FRAG_PAYLOAD. Advertised via discovery so
                                                peers reassemble at our size. Clamp [MIN, MAX]; raise
                                                MAX (compile) for jumbo frames. One size per node. */
} DartNodeNet;

/* Discovery cadence and peer-table size; zero-means-default (defaults shown). */
typedef struct {
    uint32_t              announce_interval_us; /* "I'm here" broadcast period; 1s */
    uint32_t              peer_timeout_us;   /* drop a peer after this silence; 3.5s */
    uint16_t              max_peers;         /* peer-table capacity; 16 */
} DartNodeDiscovery;

/* Optional node config, passed to dart_node_open as a compound literal (every field is
 * zero-means-default, so &(DartNodeOpts){0} or NULL is "all defaults"):
 *   dart_node_open(1<<20, "robot1", on_message, &(DartNodeOpts){ .domain = 7, .max_channels = 16 });
 */
typedef struct {
    uint16_t              domain;        /* logical-network selector */
    uint16_t              max_channels;  /* how many channels can be created; 0 = 8 */
    DartEventFn         on_event;      /* optional: loss/too-big/collision/peer up/down */
    void                 *user_data;     /* surfaced as DartMsg.user and DartEvent.user */
    uint8_t               disable_shm;   /* 1 = never use the same-host shared-memory fast path
                                            (force on-wire UDP even to a same-host peer; dynamic
                                            mode only, the static path never uses SHM) */
    DartNodeNet         net;           /* addressing/sockets (optional) */
    DartNodeDiscovery   discovery;     /* discovery cadence (optional) */
} DartNodeOpts;

/* Optional per-channel config, passed to dart_node_create_channel as a compound literal:
 *   dart_node_create_channel(n, "msg", DART_PUBSUB,
 *                            &(DartChannelOpts){ .qos = { .reliability = DART_RELIABLE } });
 */
typedef struct {
    DartQos  qos;
    uint8_t  multicast;   /* 1 = publish to / join this topic's multicast group (see DartChannelDef) */
} DartChannelOpts;

typedef struct DartNode    DartNode;
typedef struct DartChannel DartChannel;   /* opaque channel handle (stable for the node's life) */

/* A delivered message: all of its properties in one place. channel_name is a local
 * lookup (never on the wire). Do not call back into dart_* from the callback. */
typedef struct {
    DartNode      *node;
    void          *user;             /* DartNodeOpts.user_data */
    uint16_t       channel_id;       /* local channel index */
    uint32_t       sender_id;        /* peer id the message came from */
    const char    *sender_name;      /* sender's node name, NUL-terminated and never NULL
                                        ("unknown-peer" if somehow unavailable), so no null check
                                        is needed. A pointer into discovery state, never on the
                                        per-message wire; valid for the callback's duration. */
    uint8_t        sender_name_len;  /* its length */
    const char    *channel_name;     /* topic name (NUL-terminated), or NULL */
    uint8_t        channel_name_len; /* its length */
    const void    *data;
    size_t         len;
} DartMsg;
typedef void (*DartMsgFn)(const DartMsg *msg);

/* Open a node backed by mem (required): a static or dynamic DartAllocator, taken over
 * by this node (mem->claimed is set; reuse it for another node is refused). name is this
 * node's human-readable label, synced via discovery and surfaced as DartMsg.sender_name;
 * NULL/empty => an auto-generated "node-XXXXXXXX". on_message may be NULL for a publish-only
 * node. opts may be NULL for all defaults. Returns NULL on failure (incl. a static buffer
 * too small for the node, or an already-claimed allocator). */
DartNode    *dart_node_open(DartAllocator *mem, const char *name, DartMsgFn on_message,
                            const DartNodeOpts *opts);
int          dart_node_poll(DartNode *n, int timeout_ms);          /* one loop tick */
void         dart_node_close(DartNode *n, int send_bye);

/* Create a channel (topic). name is the cross-peer identity (same on every node, copied
 * in). role is DART_PUBSUB / DART_PUB_ONLY / DART_SUB_ONLY / DART_INACTIVE. opts may be
 * NULL for defaults. Returns a handle, or NULL if the reserve (opts.max_channels) is
 * full, the name is bad/too long, or out of memory. */
DartChannel *dart_node_create_channel(DartNode *n, const char *name, DartRole role,
                                      const DartChannelOpts *opts);
/* Publish to all matched subscribers. Returns DART_OK or a negative DartResult. */
int          dart_channel_send(DartChannel *ch, const void *data, size_t len);
/* Flip a channel's role at runtime (re-advertises interest). Returns 0 ok, <0 on error. */
int          dart_channel_set_role(DartChannel *ch, DartRole role);
/* This channel's local index (== DartMsg.channel_id for its messages). */
uint16_t     dart_channel_index(const DartChannel *ch);
/* Recover an already-created channel handle by its creation index (0-based), or NULL if
 * out of range. Lets a caller use a handle without storing the create_channel result. */
DartChannel *dart_node_channel(DartNode *n, uint16_t index);

/* Cumulative backpressure since open: us waited on slow readers and how many sends
 * waited. Either out-pointer may be NULL. */
void     dart_node_backpressure_stats(DartNode *n, uint64_t *waited_us, uint32_t *waited_sends);

/* Message-buffer memory (dynamic mode): in_use = live bytes, peak = high-water, alloc_calls
 * = how many heap (re)allocations have happened. alloc_calls stops rising once buffers reach
 * their steady-state sizes, so a flat count over a window proves the hot path is alloc-free.
 * Any out-pointer may be NULL. (Static mode: in_use/peak are 0; alloc_calls counts bumps.) */
void     dart_node_mem_stats(DartNode *n, size_t *in_use, size_t *peak, uint64_t *alloc_calls);
/* Cumulative reliable-repair counters for a channel (see DartRepairStats). The
 * per-second deltas are repair throughput; *out is zeroed for a NULL channel. */
void     dart_channel_repair_stats(DartChannel *ch, DartRepairStats *out);

/* In-pump diagnostic probe. A reliable publisher blocks inside dart_channel_send for the
 * whole backpressure wait, so its normal once-a-second print can't see within a stall.
 * A registered probe is called on a ~interval_us timer DURING that wait with the
 * writer's repair progress over each interval -- making the stall a within-block time
 * series (resends bursty-then-flat => reader stopped asking; steady => resends dropped).
 * Observational only; deltas are since the previous sample in the same wait. */
typedef struct {
    uint16_t channel;         /* channel index being pumped */
    uint64_t wait_elapsed_us; /* us since this backpressure wait began */
    uint64_t interval_us;     /* us since the previous sample (normalise deltas by this for true /s) */
    uint64_t frags_resent;    /* writer DATA fragments resent in the interval */
    uint64_t nacks_recv;      /* repair NACKs received in the interval */
    uint32_t polls;           /* dart_node_poll calls in the interval */
    uint32_t polls_idle;      /* of those, polls with no repair pending (writer idle for lack of NACKs) */
} DartPumpSample;
typedef void (*DartPumpProbeFn)(void *user, const DartPumpSample *s);
/* Register the in-pump probe (NULL fn disables). interval_us 0 => default 200ms. */
void     dart_node_set_pump_probe(DartNode *n, DartPumpProbeFn fn, uint64_t interval_us, void *user);
/* Head-of-line reassembly snapshot for the in-progress message from `peer` on this
 * channel: returns 1 + fills base_seqno/have/total if one is mid-reassembly, else 0.
 * `have` rising across calls = repair crawling; flat = wedged. Any pointer may be NULL. */
int      dart_channel_reader_progress(DartChannel *ch, uint32_t peer,
                            uint64_t *base_seqno, uint32_t *have, uint32_t *total);
/* Pump until every reader has acked all messages on this channel, or timeout_ms elapses.
 * Returns 1 if drained, 0 on timeout. Call before close so a burst isn't cut by the BYE. */
int      dart_channel_drain(DartChannel *ch, int timeout_ms);
/* Subscribers matched on this channel now; a one-shot publisher polls it before sending. */
int      dart_channel_match_count(DartChannel *ch);
#ifdef DART_SHM
/* Messages published / delivered via the zero-fragment shared-memory path since open
 * (observability; same-host readers only). Either out-pointer may be NULL. */
void     dart_node_shm_stats(DartNode *n, uint32_t *sent, uint32_t *recv);
#endif

#ifdef __cplusplus
}
#endif
#endif /* DART_NODE_H */
#pragma endregion
#pragma region shm/core.h
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
 * plain dart_channel_send memcpys into the chunk.
 *
 * Read modes (a future toggle; ship the safe one first):
 *   - ONE-COPY SHM (default): the reader memcpys the chunk into its own assembly_buf, then
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

/* Derive the OS object name for a segment id into buf[DART_SHM_NAME_MAX]:
 * "/dart.shm.<16 hex>", valid on POSIX (leading /) and Windows. The node fills
 * i_DartShmConfig.name with this for create/attach. */
void dart_shm_seg_name(char *buf, uint64_t segment_id);

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
} i_DartShmDesc;

#define DART_SHM_DESC_WIRE 24u   /* little-endian; rides the SHM-DATA submessage body */
size_t dart_shm_desc_encode(const i_DartShmDesc *d, uint8_t out[DART_SHM_DESC_WIRE]);
int    dart_shm_desc_decode(i_DartShmDesc *d, const uint8_t *in, size_t len);  /* 1 ok, 0 malformed */

/* ------------------------------------------------------------- segment layout
 *   [ i_DartShmSegHdr ][ chunk 0 ] ... [ chunk N-1 ]
 *   chunk = [ i_DartShmChunkHdr (padded to 16) ][ chunk_bytes payload ]
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
} i_DartShmSegHdr;

typedef struct {
    uint64_t generation;    /* bumped each reuse; matched against the descriptor */
    uint32_t length;
    uint32_t _pad;
} i_DartShmChunkHdr;

#define DART_SHM_MAGIC    0x4D484453u   /* 'DSHM' */
#define DART_SHM_VERSION  1u

/* ------------------------------------------------------------------ pool (API)
 * Opaque per-process handle over one mapped segment, placed in caller memory
 * (the node arena; size via dart_shm_state_bytes). A node CREATEs one segment for
 * its own publishes and ATTACHes one per same-host peer it subscribes to. */
typedef struct i_DartShmPool i_DartShmPool;
size_t dart_shm_state_bytes(void);

typedef struct {
    char     name[DART_SHM_NAME_MAX];  /* writer makes it from its uuid; reader gets it via meta */
    uint64_t segment_id;
    uint32_t chunk_bytes;              /* create only (0 => DART_SHM_CHUNK_BYTES); attach reads it from the header */
    uint32_t n_chunks;                 /* create only (0 => DART_SHM_CHUNKS); attach reads it from the header */
} i_DartShmConfig;

/* Writer. create maps a fresh segment (dart_plat_shm_create); NULL => stay on UDP. */
i_DartShmPool *dart_shm_create(void *pool_mem, const i_DartShmConfig *cfg);
/* The chunk backing a history slot: loan returns a writable pointer (app fills it),
 * stamp bumps generation + sets length and fills *out for the transport to frame in
 * the SHM-DATA submessage. The node owns chunk<->slot assignment (1 chunk per
 * keep_last slot), so there is no free list here. */
void *dart_shm_chunk(i_DartShmPool *p, uint32_t chunk, uint32_t *out_cap);
void  dart_shm_stamp(i_DartShmPool *p, uint32_t chunk, uint32_t len, i_DartShmDesc *out);

/* Reader. attach maps an existing segment WHOLE by name and reads its geometry
 * (chunk_bytes/n_chunks) from the header the writer stamped, so the reader needs to
 * know nothing about its size; validates magic/version/owner_host==ours and that the
 * geometry fits the mapped object. NULL => fall back to the UDP path. read resolves a
 * descriptor to an in-segment pointer and verifies generation still matches (else
 * recycled -> NULL, reliable repair covers it). No release call: the reader's
 * transport ACK of the range is the release. */
i_DartShmPool *dart_shm_attach(void *pool_mem, const i_DartShmConfig *cfg);
const void    *dart_shm_read  (i_DartShmPool *p, const i_DartShmDesc *d, uint32_t *out_len);
/* re-check the chunk generation AFTER a one-copy read (seqlock tail): 1 if it still
 * matches d (the copy is clean), 0 if a best-effort writer recycled it mid-copy (the
 * copy may be torn -> discard). Lock-free: the writer never blocks. */
int            dart_shm_verify(i_DartShmPool *p, const i_DartShmDesc *d);

void dart_shm_detach(i_DartShmPool *p);  /* unmap; the writer also unlinks the OS object */

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
 *     descriptor to the transport. dart_channel_send keeps working (one-copy into the
 *     chunk when the channel has any SHM peer, else plain inline)
 *   - on receive: the node's on_message wrapper sees the SHM flag, dart_shm_read the
 *     descriptor, calls the app on_message with the in-place pointer
 */

#ifdef __cplusplus
}
#endif
#endif /* DART_SHM_H */
#pragma endregion
#endif /* !DART_TRANSPORT_SANS_IO */

#ifdef DART_TRANSPORT_IMPLEMENTATION
#pragma region common/bytes.h
/* Shared little-endian byte packing, used by the discovery, transport, and SHM
 * layers (each formerly carried its own copy). static inline: no link symbol and
 * no unused-function warning in a layer that doesn't use a given width. The
 * amalgamator emits this once per implementation TU; the local #include is for
 * standalone compilation of a single layer. */
#ifndef DART_BYTES_H
#define DART_BYTES_H

#include <stdint.h>

static inline void dart_le_w16(uint8_t *p, uint16_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static inline void dart_le_w32(uint8_t *p, uint32_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static inline void dart_le_w64(uint8_t *p, uint64_t v){ int i; for (i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }
static inline uint16_t dart_le_r16(const uint8_t *p){ return (uint16_t)(p[0] | ((uint16_t)p[1]<<8)); }
static inline uint32_t dart_le_r32(const uint8_t *p){ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static inline uint64_t dart_le_r64(const uint8_t *p){ uint64_t v=0; int i; for (i=0;i<8;i++) v|=((uint64_t)p[i])<<(8*i); return v; }

#endif /* DART_BYTES_H */
#pragma endregion
#pragma region common/arena.h
/* Bump allocator shared by the layers that pack sub-blocks into one caller-provided
 * arena (transport state, node). Measure mode (base==NULL): dart_take returns NULL but
 * still advances offset, so the sizing pass and the build pass run the SAME code and
 * cannot drift. Build mode (base set): returns base + aligned offset, or sets oom and
 * returns NULL once the offset passes cap. static inline: no link symbol and no unused
 * warning in a layer that doesn't use it. The amalgamator emits this once per
 * implementation TU; the local #include is for standalone compilation of a layer. */
#ifndef DART_ARENA_H
#define DART_ARENA_H

#include <stddef.h>
#include <stdint.h>

typedef struct { uint8_t *base; size_t offset; size_t cap; int oom; } i_DartBump;

/* round n up to the next multiple of align (a power of two): names the (x+15)&~15 idiom */
static inline size_t dart_align_up(size_t n, size_t align){ return (n + (align - 1)) & ~(align - 1); }

static inline void *dart_take(i_DartBump *b, size_t n, size_t align){
    size_t a = dart_align_up(b->offset, align);
    b->offset = a + n;
    if (b->base){
        if (b->offset > b->cap){ b->oom = 1; return NULL; }
        return b->base + a;
    }
    return NULL;   /* measure mode */
}

#endif /* DART_ARENA_H */
#pragma endregion
#pragma region transport/internal.h
/* Shared internals of the split transport core: wire/scheduler constants, the state
 * structs, the small cross-file inline helpers, and prototypes for the helpers that
 * cross the wire/sched/writer/reader file boundaries. Not a public header. */
#ifndef DART_TRANSPORT_INTERNAL_H
#define DART_TRANSPORT_INTERNAL_H

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

#define DART__NO_DEADLINE ((uint64_t)-1)  /* next_deadline_us: nothing armed */
#ifdef DART_SHM
#ifndef DART_SHM_MAX_RETRY
#define DART_SHM_MAX_RETRY 8u   /* give up on an unresolvable descriptor after this many */
#endif
#endif

#define DART_QOS_DEF_KEEP_LAST    1u
#define DART_QOS_DEF_HEARTBEAT_US 100000u   /* 100 ms idle writer heartbeat */
#define DART_QOS_DEF_REPAIR_US    20000u    /* 20 ms reader repair-request delay */

/* Submessage wire layout. Byte 0 = type|flags, bytes 1-2 = alias, then the body.
 * Builders (dart_mk_*) and the readers both index off these, so moving a field is one
 * edit, never a silent builder/parser drift. Several fields share an offset (distinct
 * names on purpose). Header sizes: DATA 13 (single)/21 (multi), HB 23, NACK 21. */
#define DART_OFFSET_ALIAS      1u  /* u16, every submessage */
#define DART_OFFSET_SEQNO      3u  /* DATA seqno; also HB first, NACK base, SHM base (u64) */
#define DART_OFFSET_PAYLOAD_LEN_SINGLE     11u  /* DATA single: u16 payload_len (payload at DART_HEADER_DATA_SINGLE) */
#define DART_HEADER_DATA_SINGLE   13u  /* DATA single: header bytes */
#define DART_OFFSET_FRAG      11u  /* DATA multi: u16 frag */
#define DART_OFFSET_COUNT     13u  /* DATA multi: u16 count */
#define DART_OFFSET_SAMPLE_LEN      15u  /* DATA multi: u32 sample_len */
#define DART_OFFSET_PAYLOAD_LEN      19u  /* DATA multi: u16 payload_len (payload at DART_HEADER_DATA_MULTI) */
#define DART_HEADER_DATA_MULTI    21u  /* DATA multi: header bytes */
#define DART_OFFSET_HB_LAST   11u  /* HB: u64 last */
#define DART_OFFSET_HB_COUNT    19u  /* HB: u32 count */
#define DART_HEADER_HB      23u
#define DART_OFFSET_NACK_NBITS  11u  /* NACK: u16 nbits */
#define DART_OFFSET_NACK_BITMAP   13u  /* NACK: u32 bitmap */
#define DART_OFFSET_NACK_EPOCH  17u  /* NACK: u32 epoch */
#define DART_HEADER_NACK    21u
#define DART_OFFSET_SHM_COUNT 11u  /* SHM-DATA: u16 count */
#define DART_OFFSET_SHM_DESC  13u  /* SHM-DATA: descriptor (DART_SHM_DESC_BYTES follow) */
#ifdef DART_SHM
#define DART_SHM_DATA_BYTES (DART_OFFSET_SHM_DESC + DART_SHM_DESC_BYTES)
#endif

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
} i_DartWriterSample;

typedef struct {        /* writer-side, per (channel,peer) */
    uint8_t  used;
    uint8_t  reader_reliable; /* the matched reader requested RELIABLE: only then does this
                                 lane impose backpressure + heartbeats. A best-effort reader
                                 never acks, so it must stay out of flow control (fire-and-
                                 forget), else it stalls a reliable writer forever. */
    uint32_t reader_epoch; /* reader incarnation from last ACKNACK (0 = none); a change
                              means the peer rebuilt state, so the lane re-joins */
    uint64_t sent_upto;  /* next seqno to push as new data */
    uint64_t acked_upto; /* peer received all TUs < this */
    uint8_t  has_nack;   /* pending repair request from ACKNACK */
    uint64_t nack_base;
    uint32_t nack_bits;
    uint64_t hb_next_us; /* heartbeat timer */
    uint32_t hb_count;
} i_DartWriterProxy;

typedef struct {        /* reader-side, per (channel,peer) */
    uint8_t  used;
    uint8_t  started;       /* accepted any DATA from this writer yet */
    uint32_t epoch;         /* this incarnation's id, sent in every ACKNACK */
    uint64_t deliver_upto;  /* base of current sample; all below delivered/skipped */
    uint8_t  assembly_active;    /* received >=1 frag of current sample */
    uint16_t assembly_count;
    uint16_t assembly_low;        /* lowest still-missing frag index of current sample (its
                               contiguous-received front); deliver_upto+assembly_low = first hole */
    uint32_t assembly_len;
    uint8_t *assembly_buf;       /* >= assembly_len; arena (fixed) or hook-malloc'd (dynamic) */
    uint8_t *frag_bitmap;       /* ceil(assembly_count/8) */
    uint32_t assembly_cap;       /* allocated bytes of assembly_buf (dynamic grows it) */
    uint32_t bitmap_cap;        /* allocated bytes of frag_bitmap */
    uint64_t hb_last;       /* highest seqno the writer CLAIMS to hold (heartbeat only) */
    uint64_t received_high;      /* highest seqno we have actually RECEIVED a frag for. UDP is
                               assumed in-order, so a hole below this is real loss to repair
                               while anything above it is still in flight: never NACK past it. */
    uint64_t nack_high;       /* highest seqno already requested this episode; refills ask only
                               (nack_high, top] so in-flight repairs are not re-requested */
    uint64_t nack_retransmit_us;  /* earliest time to re-request a stalled floor (lost-repair backstop) */
    uint8_t  ack_force;     /* a delivery/skip/HB/(re)match owes the writer an ACKNACK even if
                               the repair floor did not move (avoids a stuck cumulative ack) */
    uint8_t  ack_pending;
    uint64_t ack_due_us;
#ifdef DART_SHM
    uint8_t  shm_fail;      /* consecutive SHM-DATA resolve failures at deliver_upto */
#endif
} i_DartReaderProxy;

typedef struct {
    DartQos    qos;
    uint64_t  identity;     /* cross-peer topic identity (hash of name) */
    const char *name;       /* our copy of the topic name */
    uint16_t  max_fragments;     /* ceil(max_message_bytes/FRAG) (fixed mode only) */
    uint8_t   role;         /* DartRole */
    uint8_t   multicast;
    uint8_t   dynamic;      /* 1 = buffers grow via cfg.allocator, no fixed cap */
    uint8_t   history_owned;/* 1 = history ring was allocator-allocated (reserve-mode
                               dart_channel_define), so dart_destroy frees it */
    uint16_t  n_subscribers;        /* live matched subscribers; multicast: >0 = group mode */
    /* writer */
    i_DartWriterSample *history;       /* [depth] ring */
    uint16_t  history_head;    /* next slot to overwrite */
    uint64_t  next_seqno;
    uint64_t  first_seqno;  /* lowest seqno still cached */
    uint8_t   have_first;
    /* multicast group lane: new data emitted once for all subscribers */
    uint64_t  multicast_sent_upto;
    uint64_t  multicast_hb_next_us;
    uint32_t  multicast_hb_count;
    /* cumulative repair counters, summed over proxies; read via dart_repair_stats */
    DartRepairStats repair_stats;
} i_DartChannel;

struct DartTransportState {
    DartConfig    cfg;       /* n_channels = user channels (no internal channel) */
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
    uint8_t     *peer_pub_bitmap; /* [max_peers][bitmap_len] peer publishes channel c */
    uint8_t     *peer_sub_bitmap; /* [max_peers][bitmap_len] peer subscribes channel c */
    uint8_t     *peer_sub_reliable; /* [max_peers][bitmap_len] ...and requested RELIABLE; sourced
                                       at match time into i_DartWriterProxy.reader_reliable */
    uint16_t     bitmap_len;       /* ceil(n_channels / 8) */
    /* per-peer wire alias -> our channel index; the data path carries the 2-byte
       alias instead of the topic name */
    uint16_t    *alias_to_channel;    /* [max_peers * alias_max]; 0xFFFF = unmapped */
    uint32_t     alias_max;        /* alias-table stride = effective meta_max_ids */
    i_DartChannel  *channels;     /* [n_channels] */
    i_DartWriterProxy   *writer_proxies;     /* [n_channels*max_peers] */
    i_DartReaderProxy   *reader_proxies;     /* [n_channels*max_peers] */
    /* active-lane scheduler: a lane is one (channel,peer) pair or a channel's group
       lane. The event that gives a lane work enqueues it, so poll_send pays for work
       done, not idle lanes. Timer work is found by an amortized clock-driven sweep. */
    uint32_t    *lane_next;   /* [n_channels*(max_peers+1)] next in dest list */
    uint8_t     *lane_queued;    /* [n_channels*(max_peers+1)] queued flag */
    uint32_t    *dest_head;   /* [max_peers+n_channels] lane list per dest */
    uint32_t    *dest_tail;
    uint8_t     *dest_queued;
    uint32_t    *dest_queue;       /* ring of active destinations */
    uint32_t     dest_queue_head, dest_queue_count;
    uint32_t     sweep;       /* timer-sweep lane cursor */
    uint64_t     sweep_time_us;     /* clock position the sweep has paid for */
    uint64_t     next_deadline_us; /* earliest armed timer (ack/HB); caps the poll wait.
                                      Lower bound fed at arm sites, made exact by a full sweep.
                                      DART__NO_DEADLINE = nothing deferred (the hot path). */
    uint32_t     reader_epoch_counter;      /* reader-epoch counter (starts at 1; 0 = none) */
};

typedef enum { DART_ORDER_OLD, DART_ORDER_GAP, DART_ORDER_ADOPTED, DART_ORDER_INORDER } i_DartReaderOrder;

/* small shared helpers (kept inline so every fragment can use them) */
static inline void dart_bset(uint8_t*bitmap,uint32_t i){bitmap[i>>3]|=(uint8_t)(1u<<(i&7));}
static inline int  dart_bget(const uint8_t*bitmap,uint32_t i){return (bitmap[i>>3]>>(i&7))&1;}
#ifdef DART_SHM
/* where a sample's bytes live: the external chunk for SHM samples, else our buf */
static inline const uint8_t *dart__sbuf(const i_DartWriterSample *s){ return s->shm ? s->shm_buf : s->buf; }
#else
static inline const uint8_t *dart__sbuf(const i_DartWriterSample *s){ return s->buf; }
#endif
/* a deferred timer was (re)armed for absolute time t: keep next_deadline as the
   minimum so the poll wakes when it is due. t==0 is an immediate ack (woken via the
   active-lane queue, not a timer), so it is ignored here. */
static inline void dart__deadline(DartTransportState *st, uint64_t t){
    if (t && t < st->next_deadline_us) st->next_deadline_us = t;
}

/* writer/reader proxy for a (channel,peer) lane. The proxies are
   [n_channels][max_peers] row-major; centralizing the index math here keeps a
   transposed channel/peer from silently corrupting a neighbor lane. */
static inline i_DartWriterProxy *dart__writer_proxy_at(DartTransportState *st, uint16_t channel_idx, uint32_t peer_slot){
    return &st->writer_proxies[(size_t)channel_idx*st->cfg.max_peers + peer_slot];
}
static inline i_DartReaderProxy *dart__reader_proxy_at(DartTransportState *st, uint16_t channel_idx, uint32_t peer_slot){
    return &st->reader_proxies[(size_t)channel_idx*st->cfg.max_peers + peer_slot];
}
/* wire alias for a local channel: its own index (the advertiser's handle). The
 * peer mapped this alias to its matching channel from our interest list. */
static inline uint16_t dart__alias_of(DartTransportState *st, int channel_idx){
    (void)st; return (uint16_t)channel_idx;
}

/* cross-file helper prototypes (definitions in wire/sched/writer/reader.c + transport.c) */
size_t dart_mk_data(uint8_t *o, uint16_t alias, uint64_t seqno, i_DartWriterSample *s, uint16_t frag, const uint8_t *payload, uint16_t payload_len);
#ifdef DART_SHM
size_t dart_mk_shm(uint8_t *o, uint16_t alias, uint64_t base, uint16_t count, const uint8_t *desc);
#endif
size_t dart_mk_hb(uint8_t *o, uint16_t alias, uint64_t first, uint64_t last, uint32_t cnt);
size_t dart_mk_nack(uint8_t *o, uint16_t alias, uint64_t base, uint16_t nbits, uint32_t bitmap, uint32_t epoch, uint8_t flags);
void   dart__lane_wake(DartTransportState *st, uint16_t channel_idx, uint32_t peer_slot);
size_t dart_writer_emit(DartTransportState *st, int channel_idx, int peer_slot, uint8_t *out, size_t cap, uint64_t now);
size_t dart_group_emit(DartTransportState *st, int channel_idx, uint8_t *out, size_t cap, uint64_t now);
int    dart__group_all_acked(DartTransportState *st, int channel_idx);
void   dart_writer_nack(DartTransportState *st, int channel_idx, int peer_slot, const uint8_t *p);
void   dart_reader_data(DartTransportState *st, int channel_idx, int peer_slot, const uint8_t *p, uint64_t now);
#ifdef DART_SHM
void   dart_reader_shm(DartTransportState *st, int channel_idx, int peer_slot, const uint8_t *p, uint64_t now);
#endif
void   dart_reader_hb(DartTransportState *st, int channel_idx, int peer_slot, const uint8_t *p, uint64_t now);
size_t dart_reader_emit(DartTransportState *st, int channel_idx, int peer_slot, uint8_t *out, size_t cap, uint64_t now);
i_DartChannel *dart_chan(DartTransportState *st, uint16_t channel, int *idx_out);
int    dart_peer_slot(DartTransportState *st, uint32_t id);
void   dart__event(DartTransportState *st, DartTransportEventKind kind, uint16_t channel, uint32_t peer, uint64_t first, uint64_t count, const char *detail);
uint64_t dart_unicast_join_seqno(const i_DartChannel *ch);

#endif /* DART_TRANSPORT_INTERNAL_H */
#pragma endregion
#pragma region transport/wire.c
/* Transport wire codec: the DATA/HB/NACK[/SHM-DATA] submessage builders. */


/* Submessage wire layout. Byte 0 = type|flags, bytes 1-2 = alias, then the body.
 * Builders (dart_mk_*) and the readers both index off these, so moving a field is one
 * edit, never a silent builder/parser drift. Several fields share an offset (distinct
 * names on purpose). Header sizes: DATA 13 (single)/21 (multi), HB 23, NACK 21. */

/* datagram builders (return length) */
size_t dart_mk_data(uint8_t *o, uint16_t alias, uint64_t seqno, i_DartWriterSample *s,
                         uint16_t frag, const uint8_t *payload, uint16_t payload_len){
    dart_le_w16(o+DART_OFFSET_ALIAS,alias);
    if (s->count==1){                       /* frag=0, count=1, len=payload_len implied */
        o[0]=(uint8_t)(DART_DATA|DART_F_SINGLE);
        dart_le_w64(o+DART_OFFSET_SEQNO,seqno); dart_le_w16(o+DART_OFFSET_PAYLOAD_LEN_SINGLE,payload_len);
        memcpy(o+DART_HEADER_DATA_SINGLE,payload,payload_len);
        return DART_HEADER_DATA_SINGLE+payload_len;
    }
    o[0]=DART_DATA;
    dart_le_w64(o+DART_OFFSET_SEQNO,seqno); dart_le_w16(o+DART_OFFSET_FRAG,frag); dart_le_w16(o+DART_OFFSET_COUNT,s->count);
    dart_le_w32(o+DART_OFFSET_SAMPLE_LEN,s->len); dart_le_w16(o+DART_OFFSET_PAYLOAD_LEN,payload_len);
    memcpy(o+DART_HEADER_DATA_MULTI,payload,payload_len);
    return DART_HEADER_DATA_MULTI+payload_len;
}

#ifdef DART_SHM
/* SHM-DATA: one submessage covers [base, base+count); body is the descriptor, no
 * payload. 37 bytes = 1 (type|F_SHM) + 2 (alias) + 8 (base) + 2 (count) + 24 (desc). */
size_t dart_mk_shm(uint8_t *o, uint16_t alias, uint64_t base, uint16_t count,
                          const uint8_t *desc){
    o[0]=(uint8_t)(DART_DATA|DART_F_SHM); dart_le_w16(o+DART_OFFSET_ALIAS,alias);
    dart_le_w64(o+DART_OFFSET_SEQNO,base); dart_le_w16(o+DART_OFFSET_SHM_COUNT,count);
    memcpy(o+DART_OFFSET_SHM_DESC,desc,DART_SHM_DESC_BYTES);
    return DART_SHM_DATA_BYTES;
}
#endif

size_t dart_mk_hb(uint8_t *o, uint16_t alias, uint64_t first, uint64_t last, uint32_t cnt){
    o[0]=DART_HB; dart_le_w16(o+DART_OFFSET_ALIAS,alias); dart_le_w64(o+DART_OFFSET_SEQNO,first); dart_le_w64(o+DART_OFFSET_HB_LAST,last); dart_le_w32(o+DART_OFFSET_HB_COUNT,cnt);
    return DART_HEADER_HB;
}

size_t dart_mk_nack(uint8_t *o, uint16_t alias, uint64_t base, uint16_t nbits, uint32_t bitmap,
                         uint32_t epoch, uint8_t flags){
    o[0]=(uint8_t)(DART_NACK|flags); dart_le_w16(o+DART_OFFSET_ALIAS,alias); dart_le_w64(o+DART_OFFSET_SEQNO,base);
    dart_le_w16(o+DART_OFFSET_NACK_NBITS,nbits); dart_le_w32(o+DART_OFFSET_NACK_BITMAP,bitmap); dart_le_w32(o+DART_OFFSET_NACK_EPOCH,epoch);
    return DART_HEADER_NACK;
}
#pragma endregion
#pragma region transport/sched.c
/* Transport active-lane scheduler and the outgoing poll. */


/* scheduler: lane index = channel_idx*(max_peers+1)+peer_slot (peer_slot==max_peers = group lane);
 * destination = peer slot peer_slot, or max_peers+channel_idx for a group lane */
static void dart__dest_push(DartTransportState *st, uint32_t d){
    uint32_t ndest = st->cfg.max_peers + (uint32_t)st->cfg.n_channels, t;
    if (st->dest_queued[d]) return;
    st->dest_queued[d]=1;
    t = st->dest_queue_head + st->dest_queue_count;
    if (t >= ndest) t -= ndest;
    st->dest_queue[t]=d; st->dest_queue_count++;
}


/* enqueue a lane that just got sendable work; idempotent while queued */
static void dart__lane_enq(DartTransportState *st, uint16_t channel_idx, uint32_t peer_slot){
    uint32_t max_peers=st->cfg.max_peers, lanes=max_peers+1u;
    uint32_t lane=(uint32_t)channel_idx*lanes+peer_slot;
    uint32_t d=(peer_slot<max_peers) ? peer_slot : max_peers+(uint32_t)channel_idx;
    if (st->lane_queued[lane]) return;
    st->lane_queued[lane]=1; st->lane_next[lane]=DART__NIL;
    if (st->dest_head[d]==DART__NIL) st->dest_head[d]=lane;
    else st->lane_next[st->dest_tail[d]]=lane;
    st->dest_tail[d]=lane;
    dart__dest_push(st, d);
}


/* enqueue, and track a freshly-armed reader ack/NACK deadline for the poll cap. Used
 * by the arm sites (ack_due_us is future or 0); the sweep enqueues due lanes with
 * dart__lane_enq instead, since it recomputes next_deadline itself. */
void dart__lane_wake(DartTransportState *st, uint16_t channel_idx, uint32_t peer_slot){
    if (peer_slot < st->cfg.max_peers){
        i_DartReaderProxy *r=dart__reader_proxy_at(st,channel_idx,peer_slot);
        if (r->used && r->ack_pending) dart__deadline(st, r->ack_due_us);
    }
    dart__lane_enq(st, channel_idx, peer_slot);
}


/* sendable work a popped lane still owes now (timer-armed work is the sweep's job) */
static int dart__lane_work(DartTransportState *st, uint16_t channel_idx, uint32_t peer_slot, uint64_t now){
    i_DartChannel *ch=&st->channels[channel_idx];
    uint32_t max_peers=st->cfg.max_peers;
    if (peer_slot==max_peers)
        return ch->multicast && ch->n_subscribers>0 && ch->multicast_sent_upto < ch->next_seqno;
    if (!st->peer_used[peer_slot] || st->peer_dormant[peer_slot]) return 0;   /* dormant: out of flow control */
    { i_DartWriterProxy *w=dart__writer_proxy_at(st,channel_idx,peer_slot);
      i_DartReaderProxy *r=dart__reader_proxy_at(st,channel_idx,peer_slot);
      int group_mode = ch->multicast && ch->n_subscribers>0;
      if (w->used && w->has_nack) return 1;
      if (w->used && !group_mode && w->sent_upto < ch->next_seqno) return 1;
      if (r->used && ch->qos.reliability==DART_RELIABLE
          && r->ack_pending && now >= r->ack_due_us) return 1;
    }
    return 0;
}


/* clock-driven counterpart of the wake calls: a cursor walks the lane table waking
 * lanes whose timers came due. Two triggers: the amortized backstop (full coverage
 * every DART_HB_SWEEP_US) and a forced full pass when next_deadline_us comes due, so
 * a deadline-capped poll that wakes for a timer actually services it. A full pass
 * also recomputes next_deadline_us exactly (the global min of not-yet-due timers).
 * Read-only; cost is bounded by table size. */
static void dart__hb_sweep(DartTransportState *st, uint64_t now){
    uint32_t max_peers=st->cfg.max_peers, lanes=max_peers+1u;
    uint32_t total=(uint32_t)st->cfg.n_channels*lanes, due, k;
    uint64_t span = now - st->sweep_time_us;
    int forced = (now >= st->next_deadline_us);    /* a tracked timer is due */
    int full;
    uint64_t mind = DART__NO_DEADLINE;             /* earliest not-yet-due timer seen */
    due = (forced || span >= DART_HB_SWEEP_US) ? total
        : (uint32_t)(span * total / DART_HB_SWEEP_US);
    if (!due) return;              /* sweep_time_us advances only when lanes are paid */
    full = (due >= total);         /* covered every lane -> mind is the global minimum */
    st->sweep_time_us = now;
    for (k=0;k<due;k++){
        uint32_t lane=st->sweep, peer_slot=lane%lanes;
        uint16_t channel_idx=(uint16_t)(lane/lanes);
        i_DartChannel *ch=&st->channels[channel_idx];
        st->sweep = (st->sweep+1u>=total) ? 0u : st->sweep+1u;
        /* gate writer/multicast heartbeats on next_seqno, never the reader ack: a
           sub-only node's data channels never advance next_seqno but still owe acks */
        if (ch->qos.reliability!=DART_RELIABLE) continue;
        if (peer_slot==max_peers){
            if (ch->next_seqno && ch->multicast && ch->n_subscribers>0 && !dart__group_all_acked(st,(int)channel_idx)){
                if (now>=ch->multicast_hb_next_us) dart__lane_enq(st,channel_idx,peer_slot);
                else if (ch->multicast_hb_next_us < mind) mind = ch->multicast_hb_next_us;
            }
            continue;
        }
        if (!st->peer_used[peer_slot] || st->peer_dormant[peer_slot]) continue;   /* dormant: out of flow control */
        { i_DartWriterProxy *w=dart__writer_proxy_at(st,channel_idx,peer_slot);
          i_DartReaderProxy *r=dart__reader_proxy_at(st,channel_idx,peer_slot);
          int group_mode = ch->multicast && ch->n_subscribers>0;
          if (w->used && w->reader_reliable && !group_mode && w->acked_upto < ch->next_seqno){
              if (now>=w->hb_next_us) dart__lane_enq(st,channel_idx,peer_slot);
              else if (w->hb_next_us < mind) mind = w->hb_next_us;
          }
          if (r->used && r->ack_pending){
              if (now>=r->ack_due_us) dart__lane_enq(st,channel_idx,peer_slot);
              else if (r->ack_due_us < mind) mind = r->ack_due_us;
          }
        }
    }
    /* a full pass saw every timer: mind is the exact next deadline. Lanes woken above
       re-arm during emit (dart__deadline) and re-lower it; reader acks just clear. */
    if (full) st->next_deadline_us = mind;
}


int dart_poll_send(DartTransportState *st, uint32_t *to_peer, void *out, size_t cap, size_t *out_len, uint64_t now){
    uint32_t max_peers=st->cfg.max_peers, lanes=max_peers+1u;
    uint32_t ndest = max_peers+(uint32_t)st->cfg.n_channels;
    dart__hb_sweep(st, now);
    while (st->dest_queue_count){
        uint32_t d; size_t offset=0;
        d = st->dest_queue[st->dest_queue_head];
        st->dest_queue_head = (st->dest_queue_head+1u>=ndest) ? 0u : st->dest_queue_head+1u;
        st->dest_queue_count--; st->dest_queued[d]=0;
        /* drain this destination's lanes into one datagram */
        while (st->dest_head[d]!=DART__NIL){
            uint32_t lane=st->dest_head[d], peer_slot=lane%lanes;
            uint16_t channel_idx=(uint16_t)(lane/lanes);
            size_t n;
            do {
                if (peer_slot==max_peers) n=dart_group_emit(st,channel_idx,(uint8_t*)out+offset,cap-offset,now);
                else {
                    /* acks first: small, one-shot, and carry the NACKs that drive
                       repair, so a backlogged writer can't starve them */
                    n=dart_reader_emit(st,(int)channel_idx,(int)peer_slot,(uint8_t*)out+offset,cap-offset,now);
                    if (!n) n=dart_writer_emit(st,(int)channel_idx,(int)peer_slot,(uint8_t*)out+offset,cap-offset,now);
                }
                offset+=n;
            } while (n && offset<cap);
            st->dest_head[d]=st->lane_next[lane];
            if (dart__lane_work(st,channel_idx,peer_slot,now)){
                /* datagram full mid-lane: rotate the lane to the back so siblings get the next */
                if (st->dest_head[d]==DART__NIL) st->dest_head[d]=lane;
                else {
                    st->lane_next[lane]=DART__NIL;
                    st->lane_next[st->dest_tail[d]]=lane;
                    st->dest_tail[d]=lane;
                }
                break;
            }
            st->lane_queued[lane]=0;     /* lane drained */
        }
        if (st->dest_head[d]!=DART__NIL) dart__dest_push(st,d);  /* fair: re-queue at tail */
        if (offset){
            *to_peer = (d<max_peers) ? st->peer_ids[d]
                              : DART_DEST_GROUP(DART_DEST_GROUP_SEL(st->channels[d-max_peers].identity));
            *out_len = offset;
            return 1;
        }
        if (st->dest_head[d]!=DART__NIL)
            return 0;    /* work pending but nothing fit: caller's cap too small */
    }
    return 0;
}


uint64_t dart_next_deadline_us(DartTransportState *st){
    return st->next_deadline_us == DART__NO_DEADLINE ? 0 : st->next_deadline_us;
}
#pragma endregion
#pragma region transport/writer.c
/* Transport writer path: history, send, per-lane + group emit, ACKNACK handling. */


/* find cached sample containing seqno (newest-first, so pushing new data is O(1)) */
static i_DartWriterSample *dart_find_sample(i_DartChannel *ch, uint64_t seqno){
    uint16_t depth = ch->qos.keep_last, k;
    uint16_t i = ch->history_head;
    for (k=0;k<depth;k++){
        i_DartWriterSample *s;
        i = (uint16_t)(i ? i-1 : depth-1);
        s = &ch->history[i];
        if (!s->valid) break;                  /* reached the unwritten tail */
        if (seqno >= s->base)
            return (seqno < s->base + s->count) ? s : NULL;
    }
    return NULL;
}


/* append the filled head slot to history and wake the lanes that carry it */
static void dart__commit(DartTransportState *st, uint16_t channel_idx, size_t len){
    i_DartChannel *ch = &st->channels[channel_idx];
    uint16_t depth = ch->qos.keep_last;
    uint16_t count = (uint16_t)((len + st->frag - 1) / st->frag);
    i_DartWriterSample *slot = &ch->history[ch->history_head];
    if (count==0) count=1;
    slot->valid=1; slot->base=ch->next_seqno; slot->count=count; slot->len=(uint32_t)len;
    ch->history_head = (uint16_t)((ch->history_head+1) % depth);
    ch->next_seqno += count;
    /* oldest cached: where the head points once wrapped, else slot 0 */
    ch->first_seqno = ch->history[ch->history_head].valid ? ch->history[ch->history_head].base
                                                    : ch->history[0].base;
    ch->have_first  = 1;
    if (ch->multicast && ch->n_subscribers>0)
        dart__lane_wake(st, channel_idx, st->cfg.max_peers);    /* group lane */
    else {
        uint32_t max_peers=st->cfg.max_peers, p;
        for (p=0;p<max_peers;p++)
            if (dart__writer_proxy_at(st,channel_idx,p)->used && !st->peer_dormant[p]) dart__lane_wake(st, channel_idx, p);
    }
}


int dart_send(DartTransportState *st, uint16_t channel, const void *data, size_t len, uint64_t now){
    int channel_idx; i_DartChannel *ch;
    (void)now;
    ch = dart_chan(st, channel, &channel_idx);                /* rejects the internal meta channel */
    if (!ch) return DART_ERR_NO_CHANNEL;
    if (ch->dynamic){
        i_DartWriterSample *slot = &ch->history[ch->history_head];
        size_t need = len ? len : 1u;
        if (len > 65535u*(uint32_t)st->frag) return DART_ERR_TOO_BIG;   /* wire fragment-count cap */
        if ((size_t)slot->cap < need){                    /* grow the slot to fit */
            uint8_t *new_buf = (uint8_t*)st->cfg.allocator(st->cfg.user, slot->buf, need);
            if (!new_buf) return DART_ERR_OOM;                 /* out of memory */
            slot->buf = new_buf; slot->cap = (uint32_t)need;
        }
    } else if (len > ch->qos.max_message_bytes) return DART_ERR_TOO_BIG;
    if (ch->role == DART_SUB_ONLY || ch->role == DART_INACTIVE) return DART_ERR_ROLE;
    if (len) memcpy(ch->history[ch->history_head].buf, data, len);
#ifdef DART_SHM
    ch->history[ch->history_head].shm = 0;   /* an inline send: this slot is not SHM-backed */
#endif
    dart__commit(st, (uint16_t)channel_idx, len);
    return DART_OK;
}

#ifdef DART_SHM

/* publish a sample whose bytes live in an external (shared-memory) chunk: store the
 * chunk pointer + descriptor on the history slot without copying. Remote peers
 * fragment from the chunk; SHM peers get the one-submessage descriptor. */
int dart_send_shm(DartTransportState *st, uint16_t channel, const void *chunk, size_t len,
                  const uint8_t *desc, uint64_t now){
    int channel_idx; i_DartChannel *ch; i_DartWriterSample *slot;
    (void)now;
    ch = dart_chan(st, channel, &channel_idx);
    if (!ch) return DART_ERR_NO_CHANNEL;
    if (len > 65535u*(uint32_t)st->frag) return DART_ERR_TOO_BIG;      /* wire fragment-count cap */
    if (ch->role == DART_SUB_ONLY || ch->role == DART_INACTIVE) return DART_ERR_ROLE;
    slot = &ch->history[ch->history_head];
    slot->shm = 1;
    slot->shm_buf = (const uint8_t*)chunk;
    memcpy(slot->desc, desc, DART_SHM_DESC_BYTES);
    dart__commit(st, (uint16_t)channel_idx, len);
    return DART_OK;
}
#endif


int dart_send_would_evict(DartTransportState *st, uint16_t channel){
    int channel_idx; i_DartChannel *ch = dart_chan(st, channel, &channel_idx);
    i_DartWriterSample *slot; uint32_t max_peers; uint16_t p;
    if (!ch || ch->qos.reliability != DART_RELIABLE) return 0;
    slot = &ch->history[ch->history_head];        /* slot the next send overwrites */
    if (!slot->valid) return 0;
    max_peers = st->cfg.max_peers;
    for (p=0;p<(uint16_t)max_peers;p++){
        i_DartWriterProxy *w=dart__writer_proxy_at(st,channel_idx,p);
        if (w->used && w->reader_reliable && !st->peer_dormant[p] && w->acked_upto < slot->base + slot->count) return 1;
    }
    return 0;
}


int dart_send_drained(DartTransportState *st, uint16_t channel){
    int channel_idx; i_DartChannel *ch = dart_chan(st, channel, &channel_idx);
    uint32_t max_peers; uint16_t p;
    if (!ch || ch->qos.reliability != DART_RELIABLE) return 1;  /* no acks to await */
    max_peers = st->cfg.max_peers;
    for (p=0;p<(uint16_t)max_peers;p++){
        i_DartWriterProxy *w=dart__writer_proxy_at(st,channel_idx,p);
        if (w->used && w->reader_reliable && !st->peer_dormant[p] && w->acked_upto < ch->next_seqno) return 0;  /* reliable reader still behind */
    }
    return 1;
}


int dart_writer_match_count(DartTransportState *st, uint16_t channel){
    int channel_idx; i_DartChannel *ch = dart_chan(st, channel, &channel_idx);
    uint32_t max_peers, p; int cnt = 0;
    if (!ch) return 0;
    max_peers = st->cfg.max_peers;
    for (p=0;p<max_peers;p++) if (dart__writer_proxy_at(st,channel_idx,p)->used) cnt++;  /* matched readers */
    return cnt;
}


int dart_repair_pending(DartTransportState *st, uint16_t channel){
    int channel_idx; i_DartChannel *ch = dart_chan(st, channel, &channel_idx);
    uint32_t max_peers, p; int cnt = 0;
    if (!ch) return 0;
    max_peers = st->cfg.max_peers;
    for (p=0;p<max_peers;p++){ i_DartWriterProxy *w=dart__writer_proxy_at(st,channel_idx,p); if (w->used && w->has_nack) cnt++; }
    return cnt;   /* writer lanes with a NACK to service; 0 = nothing to resend right now */
}

#ifdef DART_SHM

/* 1 if the channel is non-multicast, has >=1 matched reader, and EVERY matched
 * (non-dormant) reader is SHM-capable -> the node may publish this message via SHM.
 * One non-SHM (remote) reader forces inline UDP for the whole message. */
int dart_writer_shm_eligible(DartTransportState *st, uint16_t channel){
    int channel_idx; i_DartChannel *ch = dart_chan(st, channel, &channel_idx);
    uint32_t max_peers, p; int any=0;
    if (!ch || ch->multicast) return 0;
    max_peers = st->cfg.max_peers;
    for (p=0;p<max_peers;p++){
        if (!dart__writer_proxy_at(st,channel_idx,p)->used || st->peer_dormant[p]) continue;
        if (!st->peer_shm[p]) return 0;
        any = 1;
    }
    return any;
}
#endif

#ifdef DART_SHM
/* the history slot the next publish will occupy (so the node binds a chunk to it) */
uint16_t dart_channel_hist_head(DartTransportState *st, uint16_t channel){
    int channel_idx; i_DartChannel *ch = dart_chan(st, channel, &channel_idx);
    return ch ? ch->history_head : 0;
}
#endif

/* emit an HB advertising this lane's current window. Doubles as the "skip past a
 * hole" signal that replaces GAP: reader_hb advances deliver_upto to `first`, so a
 * superseded NACK or a ring-overrun push answers with an HB whose first = our floor.
 * Resets the idle-HB timer so we don't double-send. */
static size_t dart_writer_hb(DartTransportState *st, i_DartChannel *ch, i_DartWriterProxy *w, uint16_t alias,
                             uint8_t *out, size_t cap, uint64_t now){
    uint64_t first = ch->have_first ? ch->first_seqno : 0;
    if (cap < DART_HEADER_HB) return 0;
    if (w->acked_upto > first) first = w->acked_upto;   /* fresh reader adopts join point */
    w->hb_next_us = now + ch->qos.heartbeat_us;
    dart__deadline(st, w->hb_next_us);                  /* wake to send the next idle HB */
    w->hb_count++;
    return dart_mk_hb(out, alias, first, ch->next_seqno-1, w->hb_count);
}


/* writer side: handle ACKNACK */
void dart_writer_nack(DartTransportState *st, int channel_idx, int peer_slot, const uint8_t *p){
    i_DartChannel *ch=&st->channels[channel_idx];
    i_DartWriterProxy *w=dart__writer_proxy_at(st,channel_idx,peer_slot);
    uint64_t base=dart_le_r64(p+DART_OFFSET_SEQNO); uint16_t nbits=dart_le_r16(p+DART_OFFSET_NACK_NBITS); uint32_t bitmap=dart_le_r32(p+DART_OFFSET_NACK_BITMAP);
    uint32_t epoch=dart_le_r32(p+DART_OFFSET_NACK_EPOCH); uint8_t flags=p[0];
    int group_mode;
    if (!w->used) return;
    group_mode = ch->multicast && ch->n_subscribers>0;
    if (w->reader_epoch != epoch){
        if (w->reader_epoch){
            /* reader is a new incarnation (one-sided flap): our positions describe its
               dead predecessor, so re-join the lane as if freshly matched */
            w->sent_upto  = group_mode ? ch->multicast_sent_upto : dart_unicast_join_seqno(ch);
            w->acked_upto = w->sent_upto;
            w->has_nack   = 0;
            w->hb_next_us = 0;
            dart__lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
            w->reader_epoch = epoch;
            return;
        }
        w->reader_epoch = epoch;      /* first contact: lane is already fresh */
    }
    if (flags & DART_F_UNPOS){
        /* reader has delivered nothing and never NACKs: re-push from the unacked
           edge (the join window) so a push that raced ahead isn't lost */
        if (!group_mode && w->acked_upto < w->sent_upto){
            w->sent_upto = w->acked_upto;
            dart__lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
        }
        return;                    /* no position information to apply */
    }
    if (base > w->acked_upto) w->acked_upto=base;
    if (nbits>0 && bitmap!=0){
        ch->repair_stats.nacks_recv++;                           /* a repair request, not a bare ack */
        w->has_nack=1; w->nack_base=base; w->nack_bits=bitmap;
        dart__lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
    }
}


/* produce one writer submessage for (channel_idx,peer_slot) if due and it fits cap; 0 if none.
 * On no-fit, state is untouched so the same submessage is produced next time. */
size_t dart_writer_emit(DartTransportState *st, int channel_idx, int peer_slot, uint8_t *out, size_t cap, uint64_t now){
    i_DartChannel *ch=&st->channels[channel_idx];
    i_DartWriterProxy *w=dart__writer_proxy_at(st,channel_idx,peer_slot);
    int reliable=(ch->qos.reliability==DART_RELIABLE);
    /* group mode active only while a multicast channel has remote subscribers: new
       data + HBs ride the group lane, this per-peer lane only answers NACKs */
    int group_mode = ch->multicast && ch->n_subscribers>0;
    uint16_t alias = dart__alias_of(st, channel_idx);
    if (!w->used || st->peer_dormant[peer_slot]) return 0;   /* dormant: out of flow control */
    if (group_mode && (!reliable || !w->has_nack)) return 0;

    /* 1. repair (reliable only) */
    if (reliable && w->has_nack){
        uint32_t i;
        for (i=0;i<DART_NACK_WINDOW;i++){
            if (w->nack_bits & (1u<<i)){
                uint64_t seqno=w->nack_base+i;
                i_DartWriterSample *s;
                if (seqno>=ch->next_seqno){                 /* nothing there */
                    w->nack_bits &= ~(1u<<i);
                    if (w->nack_bits==0) w->has_nack=0;
                    continue;
                }
                s=dart_find_sample(ch,seqno);
                if (s){
#ifdef DART_SHM
                    if (st->peer_shm[peer_slot] && s->shm){   /* re-send the whole message as one SHM-DATA */
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
                    uint16_t frag_idx=(uint16_t)(seqno - s->base);
                    uint32_t offset=(uint32_t)frag_idx*st->frag;
                    uint16_t payload_len=(uint16_t)((s->len-offset)<st->frag?(s->len-offset):st->frag);
                    if (cap < (size_t)(s->count==1?DART_HEADER_DATA_SINGLE:DART_HEADER_DATA_MULTI)+(size_t)payload_len) return 0;   /* bit stays set */
                    w->nack_bits &= ~(1u<<i);
                    if (w->nack_bits==0) w->has_nack=0;
                    ch->repair_stats.frags_sent++; ch->repair_stats.frags_resent++;   /* retransmit to satisfy a NACK */
                    return dart_mk_data(out,alias,seqno,s,frag_idx,dart__sbuf(s)+offset,payload_len);
                    }
                } else {
                    /* superseded: skip the reader past the dropped region with an HB
                       (its first = our floor); keep still-cached seqnos for later repair */
                    uint64_t floor = (ch->have_first?ch->first_seqno:ch->next_seqno);
                    uint32_t j;
                    if (cap < DART_HEADER_HB) return 0;
                    for (j=0;j<DART_NACK_WINDOW;j++)
                        if (w->nack_base+j < floor) w->nack_bits &= ~(1u<<j);
                    if (w->nack_bits==0) w->has_nack=0;
                    return dart_writer_hb(st,ch,w,alias,out,cap,now);
                }
            }
        }
        w->has_nack=0;
    }
    if (group_mode) return 0;   /* group lane owns everything below */

    /* 2. push new data */
    if (w->sent_upto < ch->next_seqno){
        uint64_t seqno=w->sent_upto;
        i_DartWriterSample *s=dart_find_sample(ch,seqno);
        if (s){
#ifdef DART_SHM
            /* peer_shm is set at attach (before data flows), so sent_upto sits at a
               sample boundary here: emit the whole message as one SHM-DATA */
            if (st->peer_shm[peer_slot] && s->shm){
                if (cap < DART_SHM_DATA_BYTES) return 0;
                w->sent_upto = s->base + s->count;
                return dart_mk_shm(out,alias,s->base,s->count,s->desc);
            }
#endif
            {
            uint16_t frag_idx=(uint16_t)(seqno - s->base);
            uint32_t offset=(uint32_t)frag_idx*st->frag;
            uint16_t payload_len=(uint16_t)((s->len-offset)<st->frag?(s->len-offset):st->frag);
            if (cap < (size_t)(s->count==1?DART_HEADER_DATA_SINGLE:DART_HEADER_DATA_MULTI)+(size_t)payload_len) return 0;
            w->sent_upto++;
            ch->repair_stats.frags_sent++;                       /* new data (unicast lane) */
            return dart_mk_data(out,alias,seqno,s,frag_idx,dart__sbuf(s)+offset,payload_len);
            }
        } else {
            /* fell out of the ring before we sent it: skip the reader up to first
               cached with an HB (its first = our floor) */
            if (cap < DART_HEADER_HB) return 0;
            w->sent_upto=(ch->have_first?ch->first_seqno:ch->next_seqno);
            return dart_writer_hb(st,ch,w,alias,out,cap,now);
        }
    }

    /* 3. heartbeat (reliable, timer due, and this reader is behind). Once it has
       acked everything (acked_upto == next_seqno) there's nothing to repair, so the
       lane goes silent until new data or a (re)subscribe drops acked_upto again. The
       HB advertises from acked_upto so a fresh reader adopts the join point. */
    if (reliable && w->reader_reliable && now>=w->hb_next_us && w->acked_upto < ch->next_seqno)
        return dart_writer_hb(st,ch,w,alias,out,cap,now);
    return 0;
}


/* multicast: 1 if every matched subscriber has acked all data, so the group
 * heartbeat can stop until new data arrives or a new/lagging subscriber needs it */
int dart__group_all_acked(DartTransportState *st, int channel_idx){
    uint32_t max_peers=st->cfg.max_peers, p; uint64_t seq=st->channels[channel_idx].next_seqno;
    for (p=0;p<max_peers;p++){
        i_DartWriterProxy *w=dart__writer_proxy_at(st,channel_idx,p);
        if (w->used && w->reader_reliable && !st->peer_dormant[p] && w->acked_upto < seq) return 0;
    }
    return 1;
}


/* multicast writer lane: new data once for the whole group, then a channel-level
 * heartbeat (reliable). Same per-call contract as dart_writer_emit. */
size_t dart_group_emit(DartTransportState *st, int channel_idx, uint8_t *out, size_t cap, uint64_t now){
    i_DartChannel *ch=&st->channels[channel_idx];
    uint16_t alias = dart__alias_of(st, channel_idx);
    if (!ch->multicast || ch->role==DART_SUB_ONLY || ch->role==DART_INACTIVE) return 0;
    if (ch->n_subscribers==0){
        /* no remote subscribers: pin the cursor forward so a future join gets no stale replay */
        ch->multicast_sent_upto = ch->next_seqno;
        return 0;
    }
    if (ch->multicast_sent_upto < ch->next_seqno){
        uint64_t seqno=ch->multicast_sent_upto;
        i_DartWriterSample *s=dart_find_sample(ch,seqno);
        if (s){
            uint16_t frag_idx=(uint16_t)(seqno - s->base);
            uint32_t offset=(uint32_t)frag_idx*st->frag;
            uint16_t payload_len=(uint16_t)((s->len-offset)<st->frag?(s->len-offset):st->frag);
            if (cap < (size_t)(s->count==1?DART_HEADER_DATA_SINGLE:DART_HEADER_DATA_MULTI)+(size_t)payload_len) return 0;
            ch->multicast_sent_upto++;
            ch->repair_stats.frags_sent++;                       /* new data, once for the whole group */
            return dart_mk_data(out,alias,seqno,s,frag_idx,dart__sbuf(s)+offset,payload_len);
        } else {
            /* overran the ring: skip the group past it with an HB (first = our floor) */
            if (cap < DART_HEADER_HB) return 0;
            ch->multicast_sent_upto=(ch->have_first?ch->first_seqno:ch->next_seqno);
            ch->multicast_hb_next_us = now + ch->qos.heartbeat_us;
            dart__deadline(st, ch->multicast_hb_next_us);
            ch->multicast_hb_count++;
            return dart_mk_hb(out,alias,(ch->have_first?ch->first_seqno:0),ch->next_seqno-1,ch->multicast_hb_count);
        }
    }
    if (ch->qos.reliability==DART_RELIABLE && now>=ch->multicast_hb_next_us && ch->next_seqno>0
        && !dart__group_all_acked(st, channel_idx)){
        if (cap < DART_HEADER_HB) return 0;
        ch->multicast_hb_next_us = now + ch->qos.heartbeat_us;
        dart__deadline(st, ch->multicast_hb_next_us);
        ch->multicast_hb_count++;
        return dart_mk_hb(out,alias,(ch->have_first?ch->first_seqno:0),ch->next_seqno-1,ch->multicast_hb_count);
    }
    return 0;
}
#pragma endregion
#pragma region transport/reader.c
/* Transport reader path: ordering, reassembly, delivery, the ACKNACK emit. */


int dart_reader_progress(DartTransportState *st, uint16_t channel, uint32_t peer,
                         uint64_t *base_seqno, uint32_t *have, uint32_t *total){
    int channel_idx; i_DartChannel *ch = dart_chan(st, channel, &channel_idx);
    int peer_slot; i_DartReaderProxy *r;
    if (!ch) return 0;
    peer_slot = dart_peer_slot(st, peer);
    if (peer_slot < 0) return 0;
    r = dart__reader_proxy_at(st,channel_idx,peer_slot);
    if (!r->used || !r->assembly_active) return 0;            /* no message mid-reassembly */
    if (base_seqno) *base_seqno = r->deliver_upto;       /* HOL message starts here */
    if (total)      *total      = r->assembly_count;
    if (have){
        uint32_t i, c=0;
        for (i=0;i<r->assembly_count;i++) if (dart_bget(r->frag_bitmap,i)) c++;
        *have = c;
    }
    return 1;
}


/* diagnostic: attribute each reader NACK-arm (a 0->1 transition of ack_pending) to
 * its cause -- a DATA/SHM-DATA arrival or a heartbeat. Pure counting, call it right
 * before any r->ack_pending=1 in the repair paths. arms_data tracks gap-triggered and
 * progress-refill arming; arms_hb tracks the writer's idle ping (which also drives the
 * tail-loss backstop). Read via dart_repair_stats. (The self-clocked retransmit backstop
 * re-fires via nack_retransmit_us without a fresh arm, so it is not counted here. Resume/
 * position-report arms are control, not counted.) */
static void dart__arm(i_DartChannel *ch, i_DartReaderProxy *r, int is_hb){
    if (!r->ack_pending){ if (is_hb) ch->repair_stats.arms_hb++; else ch->repair_stats.arms_data++; }
}

static i_DartReaderOrder dart__order_arrival(DartTransportState *st, int channel_idx, int peer_slot,
                                             i_DartReaderProxy *r, uint64_t base, uint64_t top){
    i_DartChannel *ch=&st->channels[channel_idx];
    if (base < r->deliver_upto) return DART_ORDER_OLD;
    if (top > r->received_high) r->received_high = top;          /* proof these seqnos exist */
    if (base > r->deliver_upto){
        if (ch->qos.reliability==DART_RELIABLE && r->started){   /* gap: arm a repair NACK */
            if (!r->ack_pending){ dart__arm(ch,r,0); r->ack_pending=1; r->ack_due_us=0; }
            dart__lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
            return DART_ORDER_GAP;
        }
        if (r->started){                                         /* best-effort / first contact: adopt */
            dart__event(st, DART_TRANSPORT_MSG_LOST, (uint16_t)channel_idx, st->peer_ids[peer_slot],
                        r->deliver_upto, base - r->deliver_upto, "message(s) lost");
            ch->repair_stats.msgs_skipped += base - r->deliver_upto;
        }
        r->deliver_upto = base;
        return DART_ORDER_ADOPTED;
    }
    return DART_ORDER_INORDER;
}

#ifdef DART_SHM

/* reader side: handle an SHM-DATA submessage. It covers [base, base+count) in one
 * shot (payload is in shared memory), so there is no reassembly -- just ordering,
 * then hand the descriptor to on_shm (the node resolves + delivers + acks). The gap
 * case re-uses the normal NACK window (dart_reader_emit's !assembly_active branch). */
void dart_reader_shm(DartTransportState *st, int channel_idx, int peer_slot, const uint8_t *p, uint64_t now){
    i_DartChannel *ch=&st->channels[channel_idx];
    i_DartReaderProxy *r=dart__reader_proxy_at(st,channel_idx,peer_slot);
    int reliable = (ch->qos.reliability==DART_RELIABLE);
    uint64_t base = dart_le_r64(p+DART_OFFSET_SEQNO);
    uint16_t count = dart_le_r16(p+DART_OFFSET_SHM_COUNT);
    const uint8_t *desc = p+DART_OFFSET_SHM_DESC;          /* DART_SHM_DESC_BYTES */
    if (!r->used || count==0) return;
    {   i_DartReaderOrder ord = dart__order_arrival(st, channel_idx, peer_slot, r, base, base+count-1);
        if (ord==DART_ORDER_OLD || ord==DART_ORDER_GAP) return;   /* old/dup, or repair armed for a gap */
    }
    r->started = 1; r->assembly_active = 0;
    /* in order (base == deliver_upto). Resolve the chunk; advance + ack ONLY if the
       node delivered. A failed resolve (recycled, or a transient unattachable segment)
       leaves the gap so the reliability layer repairs it (re-sent descriptor) or skips
       it (writer HB, sample evicted). A persistently unresolvable descriptor (mis-
       configured SHM constants) would loop, so after DART_SHM_MAX_RETRY tries we skip
       it loudly instead of wedging. */
    {   int ok = st->cfg.on_shm &&
                 st->cfg.on_shm(st->cfg.user, (uint16_t)channel_idx, st->peer_ids[peer_slot], desc);
        if (ok){
            r->shm_fail = 0;
            r->deliver_upto = base + count;
            if (reliable){                  /* ack now, AFTER delivery (zero-copy invariant) */
                dart__arm(ch,r,0); r->ack_pending=1; r->ack_due_us=0; r->ack_force=1;
                dart__lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
            }
            return;
        }
        if (reliable && ++r->shm_fail >= DART_SHM_MAX_RETRY){
            dart__event(st, DART_TRANSPORT_MSG_LOST, (uint16_t)channel_idx, st->peer_ids[peer_slot],
                        base, count, "SHM descriptor unresolvable (check DART_SHM_* build constants)");
            ch->repair_stats.msgs_skipped += count;
            r->shm_fail = 0;
            r->deliver_upto = base + count;             /* give up: skip past it, ack the new edge */
            dart__arm(ch,r,0); r->ack_pending=1; r->ack_due_us=0; r->ack_force=1;
        } else if (reliable){                           /* leave the gap, NACK for a re-send */
            dart__arm(ch,r,0); r->ack_pending=1; r->ack_due_us=0;
        } else {
            r->deliver_upto = base + count;             /* best-effort: no repair, drop it */
        }
        dart__lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
    }
}
#endif


/* reader side: handle DATA */
void dart_reader_data(DartTransportState *st, int channel_idx, int peer_slot, const uint8_t *p,
                           uint64_t now){
    i_DartChannel *ch=&st->channels[channel_idx];
    i_DartReaderProxy *r=dart__reader_proxy_at(st,channel_idx,peer_slot);
    int reliable = (ch->qos.reliability==DART_RELIABLE);
    int new_fragment = 0;
    uint64_t seqno, base; uint16_t frag, count, payload_len; uint32_t sample_len; const uint8_t *payload;
    if (p[0] & DART_F_SINGLE){           /* single fragment: frag/count/len implied */
        seqno=dart_le_r64(p+DART_OFFSET_SEQNO); frag=0; count=1; payload_len=dart_le_r16(p+DART_OFFSET_PAYLOAD_LEN_SINGLE); sample_len=payload_len; payload=p+DART_HEADER_DATA_SINGLE;
    } else {
        seqno=dart_le_r64(p+DART_OFFSET_SEQNO); frag=dart_le_r16(p+DART_OFFSET_FRAG); count=dart_le_r16(p+DART_OFFSET_COUNT);
        sample_len=dart_le_r32(p+DART_OFFSET_SAMPLE_LEN); payload_len=dart_le_r16(p+DART_OFFSET_PAYLOAD_LEN); payload=p+DART_HEADER_DATA_MULTI;
    }
    base = seqno - frag;

    if (!r->used){ ch->repair_stats.frags_malformed++; return; }     /* not subscribed */
    if (count==0 || frag>=count){ ch->repair_stats.frags_malformed++; return; }  /* malformed */
    switch (dart__order_arrival(st, channel_idx, peer_slot, r, base, seqno)){
        case DART_ORDER_OLD:     ch->repair_stats.frags_old++;   return;   /* already delivered/skipped */
        case DART_ORDER_GAP:     ch->repair_stats.frags_ahead++; return;   /* future frag; repair armed */
        case DART_ORDER_ADOPTED: ch->repair_stats.frags_ahead++; r->assembly_active=0; break;  /* skipped past loss */
        case DART_ORDER_INORDER: break;
    }
    r->started = 1;   /* writer engaged: position adopted */
    /* fit the reassembly buffers (dynamic grows via the hook, fixed is capped at
       max_message_bytes); "too big" skips the whole sample and reports it */
    { uint32_t bitmap_need = ((uint32_t)count + 7u) / 8u, buf_cap, bitmap_bytes; int too_big = 0;
      if (ch->dynamic){
          if (r->assembly_cap < sample_len){
              uint8_t *new_buf = (uint8_t*)st->cfg.allocator(st->cfg.user, r->assembly_buf, sample_len?sample_len:1u);
              if (!new_buf) too_big = 1; else { r->assembly_buf = new_buf; r->assembly_cap = sample_len?sample_len:1u; }
          }
          if (!too_big && r->bitmap_cap < bitmap_need){
              uint8_t *new_bitmap = (uint8_t*)st->cfg.allocator(st->cfg.user, r->frag_bitmap, bitmap_need?bitmap_need:1u);
              if (!new_bitmap) too_big = 1; else { r->frag_bitmap = new_bitmap; r->bitmap_cap = bitmap_need?bitmap_need:1u; }
          }
      } else if (sample_len > ch->qos.max_message_bytes) too_big = 1;
      if (too_big){
          dart__event(st, DART_TRANSPORT_MSG_TOO_BIG, (uint16_t)channel_idx, st->peer_ids[peer_slot],
                      0, sample_len, "message exceeds max_message_bytes");
          r->deliver_upto = base + count; r->assembly_active = 0;
          if (reliable){
              dart__arm(ch,r,0); r->ack_pending = 1; r->ack_due_us = 0; r->ack_force = 1;
              dart__lane_wake(st, (uint16_t)channel_idx, (uint32_t)peer_slot);
          }
          return;
      }
      buf_cap  = ch->dynamic ? r->assembly_cap : ch->qos.max_message_bytes;
      bitmap_bytes = ch->dynamic ? bitmap_need     : (uint32_t)((ch->max_fragments+7u)/8u);
      /* base == deliver_upto: current sample */
      if (!r->assembly_active){
          r->assembly_active=1; r->assembly_count=count; r->assembly_len=sample_len; r->assembly_low=0;
          r->nack_high=base;     /* in-flight dedup is per-message: start this one fresh */
          memset(r->frag_bitmap,0,bitmap_bytes);
      }
      if (count!=r->assembly_count) return;                  /* inconsistent, ignore */
      ch->repair_stats.frags_recv++;                             /* every accepted DATA fragment, dups included */
      new_fragment = !dart_bget(r->frag_bitmap,frag);
      if (new_fragment){
          /* reassemble at the SOURCE peer's fragment size (advertised via discovery);
             a peer staying within [MIN, MAX] keeps count <= max_fragments, so the bitmap
             can't overflow and the buf_cap guard catches any stray offset */
          uint32_t offset=(uint32_t)frag*st->peer_frag[peer_slot];
          if (offset+payload_len<=buf_cap) memcpy(r->assembly_buf+offset,payload,payload_len);
          dart_bset(r->frag_bitmap,frag);
          if (frag==r->assembly_low)                          /* extended the contiguous-received front */
              while (r->assembly_low<count && dart_bget(r->frag_bitmap,r->assembly_low)) r->assembly_low++;
      } else ch->repair_stats.frags_dup++;                        /* already held: repair overlap / waste */
    }
    /* assembly_low is the contiguous front, so the sample is complete iff it reached the end.
       Deliver in order, advance, then arm the ACKNACK. A completed sample owes an immediate
       cumulative ack (ack_force). A still-partial sample only re-arms when this frag opened or
       advanced a real gap (a hole below received_high): a healthy in-order fill owes nothing, and
       emit dedups + paces the repair request so we never re-flood the writer with in-flight
       fragments. */
    { int done = (r->assembly_low == count);
      int hole = r->assembly_active && (r->deliver_upto + r->assembly_low <= r->received_high);
      if (done){
          if (st->cfg.on_message)
              st->cfg.on_message(st->cfg.user, (uint16_t)channel_idx, st->peer_ids[peer_slot], r->assembly_buf, r->assembly_len);
          r->deliver_upto = base + count;
          r->assembly_active=0;
      }
      if (reliable){
          if (done){
              dart__arm(ch,r,0); r->ack_pending=1; r->ack_due_us=0; r->ack_force=1;
              dart__lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
          } else if (new_fragment && hole){           /* gap revealed, or repair advanced: request now */
              dart__arm(ch,r,0); r->ack_pending=1; r->ack_due_us=0;
              dart__lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
          }
      }
    }
}


void dart_reader_hb(DartTransportState *st, int channel_idx, int peer_slot, const uint8_t *p, uint64_t now){
    i_DartChannel *ch=&st->channels[channel_idx];
    i_DartReaderProxy *r=dart__reader_proxy_at(st,channel_idx,peer_slot);
    uint64_t first=dart_le_r64(p+DART_OFFSET_SEQNO), last=dart_le_r64(p+DART_OFFSET_HB_LAST);
    if (!r->used) return;
    if (ch->qos.reliability!=DART_RELIABLE) return;
    /* un-started readers adopt no position from heartbeats (a one-sided flap's
       advertised first may be a dead predecessor's); the ack below carries our epoch.
       A writer raises its HB `first` to our acked_upto (the join-point trick), and our
       ACKNACK acks the contiguous-received front -- which sits INSIDE the sample we are
       still assembling. Eviction is whole-message, so a genuine floor never splits a
       sample: ignore a `first` that lands in our current partial (it is just our own
       mid-message ack echoed back), else we would skip past frags we are repairing and
       reject every resend as old. */
    if (r->started && first > r->deliver_upto &&
        (!r->assembly_active || first >= r->deliver_upto + r->assembly_count)){
        dart__event(st, DART_TRANSPORT_MSG_LOST, (uint16_t)channel_idx, st->peer_ids[peer_slot],   /* superseded before repair */
                    r->deliver_upto, first - r->deliver_upto, "message(s) lost");
        ch->repair_stats.msgs_skipped += first - r->deliver_upto;
        r->deliver_upto=first; r->assembly_active=0;
#ifdef DART_SHM
        r->shm_fail=0;          /* skipped past the stuck descriptor: fresh count */
#endif
    }
    /* hb_last is the writer's CLAIM (it may exceed what we've received). It is the only
       way to learn of tail loss -- frags past received_high that no later arrival will reveal --
       so emit lets the slow retransmit backstop chase up to it, never the fast gap path.
       The HB always owes a cumulative ack so a writer that lost ours stops re-pinging. */
    r->hb_last=last;
    dart__arm(ch,r,1); r->ack_pending=1; r->ack_due_us=0; r->ack_force=1;
    dart__lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
}


/* produce a reader ACKNACK for (channel_idx,peer_slot) if due; 0 if none.
 *
 * The repair request is GAP-TRIGGERED and bounded by what we have actually RECEIVED.
 * UDP frags are assumed delivered in order, so a hole below received_high is real loss while
 * anything above it is still in flight and must NOT be NACKed -- that "request up to the
 * writer's heartbeat CLAIM, every poll" was the old congestion collapse. Each floor is
 * asked once: nack_high tracks how far we have already requested, so a progress refill asks
 * only (nack_high, top] and never re-requests the still-outstanding lower frags. A stalled
 * floor is re-asked only after the retransmit backstop (repair_delay), which is also the
 * one path allowed to chase the writer's claim (hb_last) so tail loss still repairs.
 * Outstanding repair is therefore capped at one DART_NACK_WINDOW and clocked to delivery,
 * so it cannot scale into a flood with the gap size or the message size. */
size_t dart_reader_emit(DartTransportState *st, int channel_idx, int peer_slot, uint8_t *out, size_t cap, uint64_t now){
    i_DartChannel *ch=&st->channels[channel_idx];
    i_DartReaderProxy *r=dart__reader_proxy_at(st,channel_idx,peer_slot);
    uint64_t first_missing, bound, top; uint16_t nbits=0; uint32_t bitmap=0;
    uint16_t alias = dart__alias_of(st, channel_idx);
    int due, holes=0, repair=0, force;
    if (!r->used || st->peer_dormant[peer_slot]) return 0;   /* dormant: don't ack a silent writer */
    if (ch->qos.reliability!=DART_RELIABLE) return 0;
    if (cap<DART_HEADER_NACK) return 0;
    if (!r->ack_pending || now<r->ack_due_us) return 0;
    r->ack_pending=0; force=r->ack_force; r->ack_force=0;

    if (!r->started)                                     /* no position yet: F_UNPOS announces our epoch */
        return dart_mk_nack(out,alias,r->deliver_upto,0,0,r->epoch,(uint8_t)DART_F_UNPOS);

    /* the cumulative-ack point and repair-window base: our contiguous-received front */
    first_missing = r->assembly_active ? r->deliver_upto + r->assembly_low : r->deliver_upto;

    /* request ceiling = what we've received. Only the slow backstop may reach the writer's
       claim, so tail loss (no later frag will ever reveal it) still gets repaired. */
    due   = (now >= r->nack_retransmit_us);
    bound = r->received_high;
    if (due && r->hb_last > bound) bound = r->hb_last;

    if (first_missing <= bound){                         /* a hole sits below something we've heard */
        holes = 1;
        top = first_missing + DART_NACK_WINDOW;          /* one window per ACKNACK: the in-flight cap */
        if (top > bound + 1) top = bound + 1;
        if (r->assembly_active && top > r->deliver_upto + r->assembly_count)
            top = r->deliver_upto + r->assembly_count;        /* this sample's frags only (bitmap range) */
        {   /* in-flight dedup (skip the still-outstanding lower part) is only valid while we
               are assembling THIS message: its clear frag_bitmap bits are genuinely in flight. With
               no sample yet (whole message missing) there is nothing we can hold, and the slow
               backstop re-asks everything, so both ask from the floor. */
            uint64_t from = (due || !r->assembly_active) ? first_missing
                : (r->nack_high > first_missing ? r->nack_high : first_missing); /* refill: only the new part */
            uint64_t s;
            for (s=from; s<top; s++){
                int missing = r->assembly_active ? !dart_bget(r->frag_bitmap,(uint32_t)(s - r->deliver_upto)) : 1;
                if (missing) bitmap |= (1u << (uint32_t)(s - first_missing));
            }
            if (bitmap){
                nbits = (uint16_t)(top - first_missing);
                repair = 1;
                ch->repair_stats.nacks_sent++;
                if (top > r->nack_high) r->nack_high = top;
                r->nack_retransmit_us = now + ch->qos.repair_delay_us;
            }
        }
    } else r->nack_high = first_missing;                   /* caught up to received: end the episode */

    /* keep the lane live while a hole remains so the backstop re-fires; an arrival that
       advances the floor re-arms us immediately (ack_due_us=0) for the next window. */
    if (holes){ r->ack_pending=1; r->ack_due_us=r->nack_retransmit_us; dart__deadline(st,r->ack_due_us); }

    /* send only to carry a repair request or a delivery/skip/HB/(re)match cumulative ack;
       a bare re-ack at an unchanged floor would be pure noise. */
    if (!repair && !force) return 0;
    return dart_mk_nack(out,alias,first_missing,nbits,bitmap,r->epoch,0);
}
#pragma endregion
#pragma region transport/core.c
/* sans-IO reliable-UDP transport core: state, init/teardown, peer + interest matching,
 * the RX demux, and public queries. The wire codec, scheduler, and writer/reader paths
 * live in transport/{wire,sched,writer,reader}.c; shared decls in transport/internal.h. */
#include <string.h>


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

uint64_t dart_channel_identity(const DartChannelDef *def){
    return dart_topic_id(def->name);   /* the name is the cross-peer identity */
}

static size_t dart__namelen(const char *s){            /* capped strlen */
    size_t n = 0;
    if (s) while (s[n] && n < DART_TOPIC_NAME_MAX) n++;
    return n;
}


/* Reader-side fragment-count bound: a peer may fragment at the smallest size in
 * the deployment, so size the reassembly bitmap by DART_FRAG_PAYLOAD_MIN. */
static uint16_t dart_max_fragments(uint32_t max_message_bytes){
    uint32_t f = (max_message_bytes + DART_FRAG_PAYLOAD_MIN - 1) / DART_FRAG_PAYLOAD_MIN;
    if (f == 0) f = 1;
    return (uint16_t)f;
}


/* zero-means-default for the tunable QoS fields, applied once at init so the
 * stored qos is authoritative */
static void dart__qos_defaults(DartQos *q, int dynamic){
    if (q->keep_last == 0)        q->keep_last       = DART_QOS_DEF_KEEP_LAST;
    if (q->heartbeat_us == 0)     q->heartbeat_us    = DART_QOS_DEF_HEARTBEAT_US;
    if (q->repair_delay_us == 0)  q->repair_delay_us = DART_QOS_DEF_REPAIR_US;
    /* fixed mode only: a dynamic channel keeps 0 = grow-to-fit via allocator */
    if (!dynamic && q->max_message_bytes == 0) q->max_message_bytes = DART_FRAG_PAYLOAD;
}


/* Normalize a node/peer UDP fragment size: 0 -> default, then clamp to [MIN,MAX].
   Public so the transport (dart_init) and the node (announce blob) clamp identically. */
uint16_t dart_clamp_frag(uint16_t frag_payload){
    uint16_t f = frag_payload ? frag_payload : DART_FRAG_PAYLOAD;
    if (f < DART_FRAG_PAYLOAD_MIN) f = DART_FRAG_PAYLOAD_MIN;
    if (f > DART_FRAG_PAYLOAD_MAX) f = DART_FRAG_PAYLOAD_MAX;
    return f;
}


/* lay out everything (b->base==NULL = measure only) */
static DartTransportState *dart_build(i_DartBump *b, const DartConfig *cfg){
    uint16_t c, p; uint32_t max_peers = cfg->max_peers, n_channels = cfg->n_channels;
    uint16_t bitmap_len = (uint16_t)((n_channels+7u)/8u);
    uint32_t meta_ids = DART_META_MAX_IDS;
    uint32_t name_bytes = 0; char *name_pool = NULL;
    DartTransportState *st = (DartTransportState*)dart_take(b, sizeof(DartTransportState), 16);
    if (st && b->base) memset(st, 0, sizeof(*st));

    /* name pool: one fixed-size slot per channel so a reserve-mode slot can be named
       later by dart_channel_define without repacking. ch->name points at its slot. */
    name_bytes = (uint32_t)n_channels * (DART_TOPIC_NAME_MAX + 1u);
    if (meta_ids < 2u*n_channels) meta_ids = 2u*n_channels;     /* our own interest list must always fit */

    { uint32_t nlanes = n_channels*(max_peers+1u), ndest = max_peers+n_channels;
      uint32_t *peer_ids = (uint32_t*)dart_take(b, max_peers*sizeof(uint32_t), 8);
      uint8_t  *peer_used = (uint8_t*) dart_take(b, max_peers*sizeof(uint8_t), 1);
      uint8_t  *peer_local = (uint8_t*) dart_take(b, max_peers*sizeof(uint8_t), 1);
      uint8_t  *peer_dormant= (uint8_t*) dart_take(b, max_peers*sizeof(uint8_t), 1);
      uint16_t *peer_frag = (uint16_t*)dart_take(b, max_peers*sizeof(uint16_t), 2);
#ifdef DART_SHM
      uint8_t  *peer_shm= (uint8_t*) dart_take(b, max_peers*sizeof(uint8_t), 1);
#endif
      uint8_t  *peer_pub_bitmap = (uint8_t*) dart_take(b, (size_t)max_peers*bitmap_len, 1);
      uint8_t  *peer_sub_bitmap = (uint8_t*) dart_take(b, (size_t)max_peers*bitmap_len, 1);
      uint8_t  *peer_sub_reliable = (uint8_t*) dart_take(b, (size_t)max_peers*bitmap_len, 1);
      i_DartChannel *ch = (i_DartChannel*)dart_take(b, n_channels*sizeof(i_DartChannel), 16);
      i_DartWriterProxy *writer_proxies = (i_DartWriterProxy*)dart_take(b, (size_t)n_channels*max_peers*sizeof(i_DartWriterProxy), 16);
      i_DartReaderProxy *reader_proxies = (i_DartReaderProxy*)dart_take(b, (size_t)n_channels*max_peers*sizeof(i_DartReaderProxy), 16);
      uint32_t *lane_next = (uint32_t*)dart_take(b, (size_t)nlanes*sizeof(uint32_t), 8);
      uint8_t  *lane_queued = (uint8_t*) dart_take(b, (size_t)nlanes, 1);
      uint32_t *dest_head = (uint32_t*)dart_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint32_t *dest_tail = (uint32_t*)dart_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint8_t  *dest_queued = (uint8_t*) dart_take(b, (size_t)ndest, 1);
      uint32_t *dest_queue = (uint32_t*)dart_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint16_t *alias_to_channel = (uint16_t*)dart_take(b, (size_t)max_peers*meta_ids*sizeof(uint16_t), 2);
      name_pool = (char*)dart_take(b, name_bytes ? name_bytes : 1u, 1);
      if (st && b->base){
          st->cfg=*cfg; st->peer_ids=peer_ids; st->peer_used=peer_used; st->peer_local=peer_local;
          st->peer_dormant=peer_dormant; st->peer_frag=peer_frag;
          st->frag = dart_clamp_frag(cfg->frag_payload);
          st->peer_pub_bitmap=peer_pub_bitmap; st->peer_sub_bitmap=peer_sub_bitmap;
          st->peer_sub_reliable=peer_sub_reliable; st->bitmap_len=bitmap_len;
          st->channels=ch; st->writer_proxies=writer_proxies; st->reader_proxies=reader_proxies; st->reader_epoch_counter=1;
          st->next_deadline_us=DART__NO_DEADLINE;
          st->lane_next=lane_next; st->lane_queued=lane_queued;
          st->dest_head=dest_head; st->dest_tail=dest_tail; st->dest_queued=dest_queued; st->dest_queue=dest_queue;
          st->alias_to_channel=alias_to_channel; st->alias_max=meta_ids;
          memset(peer_used,0,max_peers); memset(peer_local,0,max_peers); memset(peer_dormant,0,max_peers);
          { uint32_t k; for (k=0;k<max_peers;k++) peer_frag[k]=DART_FRAG_PAYLOAD; }  /* set per peer on add */
#ifdef DART_SHM
          st->peer_shm=peer_shm; memset(peer_shm,0,max_peers);
#endif
          memset(alias_to_channel,0xFF,(size_t)max_peers*meta_ids*sizeof(uint16_t));   /* all unmapped */
          memset(peer_pub_bitmap,0,(size_t)max_peers*bitmap_len); memset(peer_sub_bitmap,0,(size_t)max_peers*bitmap_len);
          memset(peer_sub_reliable,0,(size_t)max_peers*bitmap_len);
          memset(writer_proxies,0,(size_t)n_channels*max_peers*sizeof(i_DartWriterProxy));
          memset(reader_proxies,0,(size_t)n_channels*max_peers*sizeof(i_DartReaderProxy));
          memset(lane_queued,0,nlanes); memset(dest_queued,0,ndest);
          memset(dest_head,0xFF,(size_t)ndest*sizeof(uint32_t));   /* all DART__NIL */
      }
    }

    for (c=0;c<n_channels;c++){
        /* every slot starts inactive with its own name-pool slot; reserve-mode slots
           stay this way until dart_channel_define fills them. */
        if (st && b->base){
            i_DartChannel *ch = &st->channels[c];
            memset(ch,0,sizeof(*ch));
            ch->role = DART_INACTIVE;
            ch->name = name_pool + (size_t)c*(DART_TOPIC_NAME_MAX + 1u);
            ((char*)ch->name)[0] = '\0';
        }
        if (!cfg->channels) continue;    /* reserve mode: arena holds no per-channel buffers */
        {   const DartChannelDef *def = &cfg->channels[c];
            /* dynamic = an allocator is set: buffers grow via the hook, not the arena */
            int dyn = (cfg->allocator != NULL);
            DartQos q = def->qos;            /* local, normalized copy */
            i_DartWriterSample *history; uint16_t depth, max_fragments, d;
            dart__qos_defaults(&q, dyn);
            depth = q.keep_last;
            max_fragments = dart_max_fragments(q.max_message_bytes);
            history = (i_DartWriterSample*)dart_take(b, depth*sizeof(i_DartWriterSample), 16);
            if (st && b->base){
                i_DartChannel *ch = &st->channels[c];
                size_t lane = dart__namelen(def->name);
                ch->qos=q; ch->max_fragments=max_fragments;
                ch->role=def->role; ch->multicast=def->multicast; ch->dynamic=(uint8_t)dyn;
                ch->identity = dart_channel_identity(def);
                if (lane){ memcpy((char*)ch->name, def->name, lane); ((char*)ch->name)[lane]='\0'; }
                ch->history=history; ch->history_owned=0; ch->history_head=0; ch->next_seqno=0; ch->have_first=0;
                memset(history,0,depth*sizeof(i_DartWriterSample));
            }
            for (d=0; d<depth; d++){
                uint8_t *buf = dyn ? NULL : (uint8_t*)dart_take(b, q.max_message_bytes, 8);
                if (st && b->base){ st->channels[c].history[d].buf = buf;
                                    st->channels[c].history[d].cap = dyn ? 0u : q.max_message_bytes; }
            }
            /* reader asm buffers + frag bitmaps, per peer (skipped when dynamic) */
            for (p=0;p<max_peers;p++){
                uint8_t *assembly_buf = dyn ? NULL : (uint8_t*)dart_take(b, q.max_message_bytes, 8);
                uint8_t *frag_bitmap  = dyn ? NULL : (uint8_t*)dart_take(b, (max_fragments+7u)/8u, 1);
                if (st && b->base){
                    i_DartReaderProxy *r = dart__reader_proxy_at(st,c,p);
                    r->assembly_buf=assembly_buf; r->frag_bitmap=frag_bitmap;
                    r->assembly_cap = dyn ? 0u : q.max_message_bytes;
                    r->bitmap_cap  = dyn ? 0u : (uint32_t)((max_fragments+7u)/8u);
                }
            }
        }
    }
    return st;
}


size_t dart_required_memory(const DartConfig *cfg){
    i_DartBump b; memset(&b,0,sizeof b);
    if (!cfg || cfg->n_channels==0 || cfg->max_peers==0) return 0;
    dart_build(&b, cfg);
    return b.offset + 16;   /* slack for base alignment */
}


DartTransportState *dart_init(void *mem, size_t cap, const DartConfig *cfg){
    i_DartBump b; DartTransportState *st; uint16_t i;
    if (!mem || !cfg || cfg->n_channels==0 || cfg->max_peers==0) return NULL;
    if (!cfg->channels && !cfg->allocator) return NULL;    /* reserve mode needs an allocator */
    if (cfg->channels) for (i=0;i<cfg->n_channels;i++){
        const DartChannelDef *d = &cfg->channels[i];
        size_t lane = 0;
        if (!d->name || !d->name[0]) return NULL;          /* name = identity, required */
        while (d->name[lane]) lane++;
        if (lane > DART_TOPIC_NAME_MAX) return NULL;           /* the wire name is the whole name */
    }
    memset(&b,0,sizeof b);
    b.base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    b.cap  = cap - (size_t)((uint8_t*)b.base - (uint8_t*)mem);
    st = dart_build(&b, cfg);
    if (!st || b.oom) return NULL;
    st->cfg.channels = NULL;   /* only read during init; detach the caller's pointer */
    return st;
}


/* Relocate a live transport into a bigger block at grown counts (dynamic-mode growth).
 * Heap buffers (history rings, sample/assembly bufs, frag bitmaps) are NOT in the arena,
 * so the struct copies carry their pointers across and the OLD arena can be freed without
 * touching them. The 2D tables are re-strided into the new max_peers/n_channels; the
 * active-lane scheduler (indices encode the old strides) is dropped and rebuilt from the
 * proxy state. The caller frees old's arena block afterward; it must NOT dart_destroy old
 * (that would free the heap buffers now owned by the new state). Returns the new state. */
DartTransportState *dart_migrate(DartTransportState *old, void *new_mem, size_t new_cap,
                        uint16_t new_max_peers, uint16_t new_n_channels){
    DartConfig nc; DartTransportState *nw; uint16_t omp, onc, c, p;
    if (!old) return NULL;
    nc = old->cfg; nc.channels = NULL;
    nc.max_peers = new_max_peers; nc.n_channels = new_n_channels;
    nw = dart_init(new_mem, new_cap, &nc);
    if (!nw) return NULL;
    omp = old->cfg.max_peers; onc = old->cfg.n_channels;

    nw->reader_epoch_counter = old->reader_epoch_counter;
    nw->frag = old->frag;
    memcpy(nw->peer_ids,     old->peer_ids,     (size_t)omp*sizeof(uint32_t));
    memcpy(nw->peer_used,    old->peer_used,    omp);
    memcpy(nw->peer_local,   old->peer_local,   omp);
    memcpy(nw->peer_dormant, old->peer_dormant, omp);
    memcpy(nw->peer_frag,    old->peer_frag,    (size_t)omp*sizeof(uint16_t));
#ifdef DART_SHM
    memcpy(nw->peer_shm,     old->peer_shm,     omp);
#endif
    /* channels: keep the new name-pool slot pointer, carry everything else (incl. the
       heap history ring pointer) and re-copy the name string into the new pool */
    for (c=0;c<onc;c++){
        char *nm = (char*)nw->channels[c].name;
        size_t l = dart__namelen(old->channels[c].name);
        nw->channels[c] = old->channels[c];
        nw->channels[c].name = nm;
        if (l) memcpy(nm, old->channels[c].name, l);
        nm[l] = '\0';
    }
    /* per-(channel,peer) proxies: re-stride into the new max_peers (heap bufs ride along) */
    for (c=0;c<onc;c++) for (p=0;p<omp;p++){
        *dart__writer_proxy_at(nw,c,p) = *dart__writer_proxy_at(old,c,p);
        *dart__reader_proxy_at(nw,c,p) = *dart__reader_proxy_at(old,c,p);
    }
    /* per-peer interest bitmaps (stride grows with n_channels) + alias table */
    for (p=0;p<omp;p++){
        memcpy(nw->peer_pub_bitmap + (size_t)p*nw->bitmap_len,
               old->peer_pub_bitmap + (size_t)p*old->bitmap_len, old->bitmap_len);
        memcpy(nw->peer_sub_bitmap + (size_t)p*nw->bitmap_len,
               old->peer_sub_bitmap + (size_t)p*old->bitmap_len, old->bitmap_len);
        memcpy(nw->peer_sub_reliable + (size_t)p*nw->bitmap_len,
               old->peer_sub_reliable + (size_t)p*old->bitmap_len, old->bitmap_len);
        memcpy(nw->alias_to_channel + (size_t)p*nw->alias_max,
               old->alias_to_channel + (size_t)p*old->alias_max,
               (size_t)old->alias_max*sizeof(uint16_t));
    }
    /* scheduler is fresh/empty: re-enqueue every used lane + group lane, then force a full
       sweep next poll so timers re-arm and next_deadline is recomputed exactly */
    for (c=0;c<onc;c++){
        for (p=0;p<omp;p++) if (nw->peer_used[p]) dart__lane_wake(nw, c, p);
        dart__lane_wake(nw, c, new_max_peers);
    }
    nw->next_deadline_us = 0;
    return nw;
}


int dart_peer_slot(DartTransportState *st, uint32_t id){
    uint16_t i;
    for (i=0;i<st->cfg.max_peers;i++) if (st->peer_used[i] && st->peer_ids[i]==id) return (int)i;
    return -1;
}

/* the local handle IS the channel's index; out-of-range rejected */
i_DartChannel *dart_chan(DartTransportState *st, uint16_t channel, int *idx_out){
    if (channel >= st->cfg.n_channels) return NULL;
    if (idx_out) *idx_out = (int)channel;
    return &st->channels[channel];
}

/* RX demux: find the local channel for a wire identity */
static i_DartChannel *dart_chan_by_identity(DartTransportState *st, uint64_t identity, int *idx_out){
    uint16_t i;
    for (i=0;i<st->cfg.n_channels;i++) if (st->channels[i].identity==identity){ if(idx_out)*idx_out=(int)i; return &st->channels[i]; }
    return NULL;
}


/* fire one DartTransportEvent (no-op if no on_event). Transport emits MSG_LOST/TOO_BIG/
 * COLLISION/QOS. first/count are the kind's two numeric slots; route them to named fields. */
void dart__event(DartTransportState *st, DartTransportEventKind kind, uint16_t channel,
                        uint32_t peer, uint64_t first, uint64_t count, const char *detail){
    DartTransportEvent ev;
    if (!st->cfg.on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind=kind; ev.channel=channel; ev.peer=peer; ev.detail=detail; ev.user=st->cfg.user;
    switch (kind){
    case DART_TRANSPORT_MSG_LOST:       ev.lost_first = first; ev.lost_count = count; break;
    case DART_TRANSPORT_MSG_TOO_BIG:    ev.too_big_bytes = count; break;
    case DART_TRANSPORT_NAME_COLLISION: ev.identity = first; break;
    default: break;
    }
    st->cfg.on_event(&ev);
}

/* dart_event_str (and its bounded appenders) moved to the node (node/core.c): the
   formatter covers the node's app-facing DartEvent union, not the transport's own
   events. The transport stays independent of the node's event vocabulary. */


/* unicast join seqno: head minus qos.catch_up cached samples (reliable only) */
uint64_t dart_unicast_join_seqno(const i_DartChannel *ch){
    uint16_t depth = ch->qos.keep_last;   /* normalized at init (>=1) */
    uint16_t want = ch->qos.catch_up, k, i;
    uint64_t s = ch->next_seqno;
    if (ch->qos.reliability != DART_RELIABLE || want == 0) return s;
    if (want > depth) want = depth;
    i = ch->history_head;
    for (k=0; k<want; k++){
        uint16_t j = (uint16_t)(i ? i-1 : depth-1);
        if (!ch->history[j].valid) break;        /* fewer than want cached */
        s = ch->history[j].base;
        i = j;
    }
    return s;
}


/* match one (channel,peer) proxy: a multicast channel engages its group lane on
 * the first subscriber; per-peer lanes then carry repairs only */
static void dart__match_w(DartTransportState *st, uint16_t c, uint16_t peer_slot){
    i_DartChannel *ch=&st->channels[c];
    i_DartWriterProxy *w=dart__writer_proxy_at(st,c,peer_slot);
    memset(w,0,sizeof(*w));
    w->used=1;
    /* only a reader that advertised RELIABLE acks; a best-effort reader stays out of
       flow control so it can't stall this writer (it gets new data, never repairs/HB) */
    w->reader_reliable = dart_bget(&st->peer_sub_reliable[(size_t)peer_slot*st->bitmap_len], c) ? 1u : 0u;
    if (!ch->multicast){
        w->sent_upto = dart_unicast_join_seqno(ch);
    } else {
        /* first subscriber engages group mode at the head; reliable-from-join-point,
           later joiners backfill via NACK */
        if (ch->n_subscribers==0) ch->multicast_sent_upto = ch->next_seqno;
        ch->n_subscribers++;
        w->sent_upto = ch->multicast_sent_upto;
    }
    w->acked_upto = w->sent_upto;
    dart__lane_wake(st, c, peer_slot);   /* unicast repair lane primed + ack/hb */
}

static void dart__unmatch_w(DartTransportState *st, uint16_t c, uint16_t peer_slot){
    i_DartChannel *ch=&st->channels[c];
    i_DartWriterProxy *w=dart__writer_proxy_at(st,c,peer_slot);
    if (!w->used) return;
    if (ch->multicast && ch->n_subscribers) ch->n_subscribers--;
    w->used=0;
}

static void dart__match_r(DartTransportState *st, uint16_t c, uint16_t peer_slot){
    i_DartReaderProxy *r=dart__reader_proxy_at(st,c,peer_slot);
    uint8_t *assembly_buf=r->assembly_buf, *frag_bitmap=r->frag_bitmap;
    uint32_t assembly_cap=r->assembly_cap, bitmap_cap=r->bitmap_cap;   /* keep grown buffers across rematch */
    memset(r,0,sizeof(*r));
    r->assembly_buf=assembly_buf; r->frag_bitmap=frag_bitmap; r->assembly_cap=assembly_cap; r->bitmap_cap=bitmap_cap;
    r->epoch=st->reader_epoch_counter++;   /* new incarnation: writers re-join on seeing it */
    r->used=1;       /* started==0: first DATA adopts the writer's position */
    /* announce this incarnation once so a caught-up (idle, non-pinging) writer
       re-joins and replays. A genuine discovery blip keeps its position through
       dart_peer_dormant/resume and never lands here, so a single ACKNACK suffices. */
    if (st->channels[c].qos.reliability==DART_RELIABLE){
        r->ack_pending=1; r->ack_due_us=0; r->ack_force=1;
        dart__lane_wake(st,c,peer_slot);
    }
}

static void dart__unmatch_r(DartTransportState *st, uint16_t c, uint16_t peer_slot){
    i_DartReaderProxy *r=dart__reader_proxy_at(st,c,peer_slot);
    r->used=0; r->assembly_active=0;
}


/* recompute one (channel,peer) match from our role and the peer's interest bits */
static void dart__rematch(DartTransportState *st, uint16_t c, uint16_t peer_slot){
    i_DartChannel *ch=&st->channels[c];
    const uint8_t *peer_pub_bitmap=&st->peer_pub_bitmap[(size_t)peer_slot*st->bitmap_len];
    const uint8_t *peer_sub_bitmap=&st->peer_sub_bitmap[(size_t)peer_slot*st->bitmap_len];
    int wuse = (ch->role==DART_PUBSUB || ch->role==DART_PUB_ONLY) && dart_bget(peer_sub_bitmap,c);
    int ruse = (ch->role==DART_PUBSUB || ch->role==DART_SUB_ONLY) && dart_bget(peer_pub_bitmap,c);
    i_DartWriterProxy *w=dart__writer_proxy_at(st,c,peer_slot);
    i_DartReaderProxy *r=dart__reader_proxy_at(st,c,peer_slot);
    if (wuse && !w->used) dart__match_w(st,c,peer_slot);
    else if (!wuse && w->used) dart__unmatch_w(st,c,peer_slot);
    if (ruse && !r->used) dart__match_r(st,c,peer_slot);
    else if (!ruse && r->used) dart__unmatch_r(st,c,peer_slot);
}


void dart_peer_add(DartTransportState *st, uint32_t id, int peer_is_local, uint16_t peer_frag){
    uint16_t i; int free=-1; uint32_t max_peers=st->cfg.max_peers;
    if (dart_peer_slot(st,id)>=0) return;
    for (i=0;i<max_peers;i++) if(!st->peer_used[i]){free=(int)i;break;}
    if (free<0) return;
    st->peer_used[free]=1; st->peer_ids[free]=id;
    st->peer_local[free]=(uint8_t)(peer_is_local?1:0);
    st->peer_dormant[free]=0;
    st->peer_frag[free]=dart_clamp_frag(peer_frag);
#ifdef DART_SHM
    st->peer_shm[free]=0;   /* node sets it once the peer's segment is attached */
#endif
    memset(&st->peer_pub_bitmap[(size_t)free*st->bitmap_len],0,st->bitmap_len);
    memset(&st->peer_sub_bitmap[(size_t)free*st->bitmap_len],0,st->bitmap_len);
    memset(&st->peer_sub_reliable[(size_t)free*st->bitmap_len],0,st->bitmap_len);
    memset(&st->alias_to_channel[(size_t)free*st->alias_max],0xFF,(size_t)st->alias_max*sizeof(uint16_t));
    /* nothing matches until dart_apply_peer_interest feeds the peer's interest
       list (carried in its discovery announce) */
}


void dart_peer_remove(DartTransportState *st, uint32_t id){
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
void dart_peer_dormant(DartTransportState *st, uint32_t id){
    int s = dart_peer_slot(st,id);
    if (s>=0) st->peer_dormant[s]=1;
}


/* A dormant peer's SAME incarnation returned: re-include it in flow control and
 * re-report each reader position so the writer fills any gap (the reader dedups any
 * replay for free). The writer side needs nothing proactive; the reader's ACKNACK
 * re-arms its heartbeats. Proxies and deliver_upto were never touched, so no dup,
 * no loss. */
void dart_peer_resume(DartTransportState *st, uint32_t id){
    int s = dart_peer_slot(st,id); uint16_t c;
    if (s<0) return;
    st->peer_dormant[s]=0;
    for (c=0;c<st->cfg.n_channels;c++){
        i_DartWriterProxy *w=dart__writer_proxy_at(st,c,s);
        i_DartReaderProxy *r=dart__reader_proxy_at(st,c,s);
        if (st->channels[c].qos.reliability!=DART_RELIABLE) continue;
        if (r->used){ r->ack_pending=1; r->ack_due_us=0; r->ack_force=1; }  /* report our position now */
        if (w->used || r->used) dart__lane_wake(st,c,(uint16_t)s);
    }
}


/* update a peer's advertised fragment size (its blob may arrive after first contact) */
void dart_peer_set_frag(DartTransportState *st, uint32_t id, uint16_t peer_frag){
    int s = dart_peer_slot(st,id);
    if (s>=0) st->peer_frag[s]=dart_clamp_frag(peer_frag);
}

#ifdef DART_SHM

void dart_peer_set_shm(DartTransportState *st, uint32_t id, int is_shm){
    int s = dart_peer_slot(st,id);
    if (s>=0) st->peer_shm[s]=(uint8_t)(is_shm?1:0);
}
#endif


void dart_destroy(DartTransportState *st){
    uint16_t c; uint32_t p, max_peers;
    if (!st || !st->cfg.allocator) return;     /* fixed mode: nothing hook-allocated */
    max_peers = st->cfg.max_peers;
    for (c=0;c<st->cfg.n_channels;c++){
        i_DartChannel *ch=&st->channels[c];
        uint16_t depth, d;
        if (!ch->dynamic) continue;
        depth = ch->qos.keep_last;
        for (d=0; d<depth; d++)
            if (ch->history[d].buf){ st->cfg.allocator(st->cfg.user, ch->history[d].buf, 0);
                                  ch->history[d].buf=NULL; ch->history[d].cap=0; }
        for (p=0;p<max_peers;p++){
            i_DartReaderProxy *r=dart__reader_proxy_at(st,c,p);
            if (r->assembly_buf){ st->cfg.allocator(st->cfg.user, r->assembly_buf, 0); r->assembly_buf=NULL; r->assembly_cap=0; }
            if (r->frag_bitmap){ st->cfg.allocator(st->cfg.user, r->frag_bitmap, 0); r->frag_bitmap=NULL; r->bitmap_cap=0; }
        }
        if (ch->history_owned && ch->history){   /* ring allocated by dart_channel_define */
            st->cfg.allocator(st->cfg.user, ch->history, 0);
            ch->history=NULL; ch->history_owned=0;
        }
    }
}


/* per-entry flags byte (interest is sent rarely, so a whole byte, not a stolen bit) */
#define DART_META_F_RELIABLE 0x01u   /* advertiser offers reliable delivery on this topic */

/* one interest entry: [u16 alias][u8 flags][u8 namelen][name]. The name rides along so
 * a hash collision is detected (not cross-wired); the identity is recomputed from it. */
static uint8_t *dart__meta_put(uint8_t *p, uint16_t alias, const i_DartChannel *ch){
    size_t lane = dart__namelen(ch->name);
    dart_le_w16(p, alias); p += 2;
    *p++ = (uint8_t)(ch->qos.reliability==DART_RELIABLE ? DART_META_F_RELIABLE : 0u);
    *p++ = (uint8_t)lane;
    if (lane){ memcpy(p, ch->name, lane); p += lane; }
    return p;
}

static int dart__meta_name_eq(const i_DartChannel *ch, const uint8_t *name, size_t nlen){
    size_t ours = dart__namelen(ch->name);
    if (nlen != ours) return 0;
    return nlen==0 ? 1 : (memcmp(ch->name, name, nlen)==0);
}

/* match count entries to local channels by identity (recomputed from each name),
 * recording the alias map. Same-identity-different-name is a collision: refused.
 * is_pub: the peer's publish list, so each entry's flags carry its OFFERED QoS, which
 * the RxO check uses to refuse a reliable subscriber a best-effort publisher. rel_bitmap
 * (sub list only, else NULL): records which subscribed channels the peer requested RELIABLE,
 * so the writer can keep best-effort readers out of flow control. */
static const uint8_t *dart__meta_scan(DartTransportState *st, int peer_slot, const uint8_t *p,
                                      uint32_t count, uint8_t *bitmap, int is_pub, uint8_t *rel_bitmap){
    uint32_t k;
    for (k=0;k<count;k++){
        uint16_t alias=dart_le_r16(p); uint8_t flags=p[2]; uint32_t nlen=p[3];
        const uint8_t *name=p+4; int channel_idx;
        uint64_t id=dart__id_n(name,nlen);
        i_DartChannel *ch=dart_chan_by_identity(st,id,&channel_idx);
        p = name + nlen;
        if (!ch) continue;                                  /* not ours */
        if (!dart__meta_name_eq(ch,name,nlen)){
            dart__event(st, DART_TRANSPORT_NAME_COLLISION, (uint16_t)channel_idx, st->peer_ids[peer_slot],
                        id, 0, ch->name ? ch->name : "");
            continue;
        }
        /* RxO QoS: a reliable subscriber refuses a best-effort publisher (no silent
           downgrade). We keep requesting reliable, so the match forms automatically if
           the publisher later upgrades and re-advertises. */
        if (is_pub && (ch->role==DART_PUBSUB || ch->role==DART_SUB_ONLY) &&
            ch->qos.reliability==DART_RELIABLE && !(flags & DART_META_F_RELIABLE)){
            dart__event(st, DART_TRANSPORT_QOS_INCOMPATIBLE, (uint16_t)channel_idx,
                        st->peer_ids[peer_slot], 0, 0, ch->name ? ch->name : "");
            continue;                                       /* refuse: no bit, no alias map */
        }
        dart_bset(bitmap,(uint32_t)channel_idx);
        if (rel_bitmap && (flags & DART_META_F_RELIABLE)) dart_bset(rel_bitmap,(uint32_t)channel_idx);
        if ((uint32_t)alias < st->alias_max)
            st->alias_to_channel[(size_t)peer_slot*st->alias_max + alias] = (uint16_t)channel_idx;
    }
    return p;
}


/* Upper bound on dart_build_interest output, for sizing the announce buffer: a
 * PUBSUB channel appears in both lists, so 2*n_channels max-length entries. */
size_t dart_interest_max(uint16_t n_channels){
    return 4u + (size_t)(2u+1u+1u+DART_TOPIC_NAME_MAX) * 2u * (size_t)n_channels;  /* alias+flags+namelen+name */
}


/* Serialize our interest into out: [u16 npub][u16 nsub][pub..][sub..], each entry
 * [u16 alias][u8 namelen][name]. Returns bytes written, or 0 if cap is too small.
 * The node carries this in its discovery announce; size out via dart_interest_max. */
size_t dart_build_interest(DartTransportState *st, void *out, size_t cap){
    uint8_t *o=(uint8_t*)out, *p, *end=o+cap;
    uint16_t c; uint32_t n_pub=0, n_sub=0;
    if (cap < 4) return 0;
    p=o+4;
    for (c=0;c<st->cfg.n_channels;c++){
        uint8_t d=st->channels[c].role;
        if (d==DART_PUBSUB || d==DART_PUB_ONLY){
            if (p + 4u + dart__namelen(st->channels[c].name) > end) return 0;
            p=dart__meta_put(p,c,&st->channels[c]); n_pub++;
        }
    }
    for (c=0;c<st->cfg.n_channels;c++){
        uint8_t d=st->channels[c].role;
        if (d==DART_PUBSUB || d==DART_SUB_ONLY){
            if (p + 4u + dart__namelen(st->channels[c].name) > end) return 0;
            p=dart__meta_put(p,c,&st->channels[c]); n_sub++;
        }
    }
    dart_le_w16(o,(uint16_t)n_pub); dart_le_w16(o+2,(uint16_t)n_sub);
    return (size_t)(p - o);
}


/* A peer's interest list arrived (from its discovery announce): refresh its bits
 * and rematch every channel. Idempotent; re-applying re-derives all matches. */
void dart_apply_peer_interest(DartTransportState *st, uint32_t peer_id, const void *blob, size_t len){
    const uint8_t *d=(const uint8_t*)blob, *p, *end=d+len;
    uint16_t n_pub, n_sub, c; int peer_slot=dart_peer_slot(st,peer_id);
    uint8_t *peer_pub_bitmap, *peer_sub_bitmap;
    if (peer_slot<0 || len<4) return;
    peer_pub_bitmap=&st->peer_pub_bitmap[(size_t)peer_slot*st->bitmap_len];
    peer_sub_bitmap=&st->peer_sub_bitmap[(size_t)peer_slot*st->bitmap_len];
    n_pub=dart_le_r16(d); n_sub=dart_le_r16(d+2);
    /* validate the whole variable-length list first: a truncated blob must not drop a match */
    { uint32_t k, tot=(uint32_t)n_pub+n_sub; p=d+4;
      for (k=0;k<tot;k++){
          if (p+4 > end) return;
          p += 4u + (uint32_t)p[3];
          if (p > end) return;
      } }
    memset(peer_pub_bitmap,0,st->bitmap_len); memset(peer_sub_bitmap,0,st->bitmap_len);
    memset(&st->peer_sub_reliable[(size_t)peer_slot*st->bitmap_len],0,st->bitmap_len);
    memset(&st->alias_to_channel[(size_t)peer_slot*st->alias_max],0xFF,(size_t)st->alias_max*sizeof(uint16_t));
    p = dart__meta_scan(st, peer_slot, d+4, n_pub, peer_pub_bitmap, 1, NULL);   /* pub list: offered QoS */
    p = dart__meta_scan(st, peer_slot, p,   n_sub, peer_sub_bitmap, 0,         /* sub list: requested QoS */
                        &st->peer_sub_reliable[(size_t)peer_slot*st->bitmap_len]);
    for (c=0;c<st->cfg.n_channels;c++) dart__rematch(st,c,(uint16_t)peer_slot);
}


/* diagnostic: how many channels we now publish to / receive from this peer (unicast
 * lanes). Surfaced on DART_PEER_INTEREST so a caller can see a match form (or not). */
void dart_peer_match_counts(DartTransportState *st, uint32_t peer_id,
                            uint16_t *publish_to, uint16_t *receive_from){
    int s; uint16_t c, w=0, r=0;
    if (publish_to)   *publish_to   = 0;
    if (receive_from) *receive_from = 0;
    if (!st) return;
    s = dart_peer_slot(st, peer_id);
    if (s < 0) return;
    for (c=0;c<st->cfg.n_channels;c++){
        if (dart__writer_proxy_at(st,c,(uint32_t)s)->used) w++;
        if (dart__reader_proxy_at(st,c,(uint32_t)s)->used) r++;
    }
    if (publish_to)   *publish_to   = w;
    if (receive_from) *receive_from = r;
}


/* Discovery-announce meta blob codec (see dart_meta_* in core.h for the layout). The
   interest list is wrapped in a prefix carrying frag size, (odd ver) SHM info, and the
   node name. No back-compat: the version byte just tags the one current format, and a
   blob whose magic/version we don't expect is rejected, not reinterpreted. An interest
   entry is [u16 alias][u8 flags][u8 namelen][name]; flags bit 0 = offered reliability.
   Parsing is fully bounds-checked (see dart_apply_peer_interest), so a malformed or
   foreign blob is dropped wholesale, never trusted. */
#define DART__META_BASE_NOSHM 5u    /* 'D','N',ver, frag_lo, frag_hi */
#define DART__META_BASE_SHM   22u   /* ... + shm(1) + host[16] */
#ifdef DART_SHM
#define DART__META_VER  7u                  /* what WE write */
#define DART__META_BASE DART__META_BASE_SHM
#else
#define DART__META_VER  6u
#define DART__META_BASE DART__META_BASE_NOSHM
#endif

static int dart__meta_ok(const uint8_t *meta, uint16_t meta_len){
    return meta && meta_len >= 5 && meta[0]=='D' && meta[1]=='N' && meta[2]>=6 && meta[2]<=7;
}
/* base prefix through host[16], by version (odd v7 carries shm+host, even v6 doesn't). */
static uint16_t dart__meta_base(const uint8_t *meta){
    return (meta[2]==7) ? DART__META_BASE_SHM : DART__META_BASE_NOSHM;
}
uint16_t dart_meta_capacity(uint16_t n_channels){
    size_t cap = (size_t)DART__META_BASE + dart_interest_max(n_channels);  /* overlay: no name (it's discovery's) */
    if (cap > 65000u) cap = 65000u;
    return (uint16_t)cap;
}

uint16_t dart_meta_build(DartTransportState *st, uint8_t *out, uint16_t cap,
                         uint16_t frag_size, int shm_capable, const uint8_t host[16]){
    size_t interest_len; uint16_t off = DART__META_BASE;
    out[0]='D'; out[1]='N'; out[2]=DART__META_VER;
    out[3]=(uint8_t)(frag_size & 0xFF); out[4]=(uint8_t)(frag_size >> 8);
#ifdef DART_SHM
    out[5]=(uint8_t)(shm_capable?1:0);
    if (host) memcpy(out+6, host, 16); else memset(out+6, 0, 16);
#else
    (void)shm_capable; (void)host;
#endif
    interest_len = dart_build_interest(st, out + off, cap - off);   /* no name here: that is discovery's */
    return (uint16_t)(off + interest_len);
}

uint16_t dart_meta_frag(const uint8_t *meta, uint16_t meta_len){
    if (!dart__meta_ok(meta, meta_len)) return 0;
    return (uint16_t)(meta[3] | ((uint16_t)meta[4] << 8));
}

const uint8_t *dart_meta_interest(const uint8_t *meta, uint16_t meta_len, size_t *out_len){
    uint16_t off;
    *out_len = 0;
    if (!dart__meta_ok(meta, meta_len)) return NULL;
    off = dart__meta_base(meta);          /* interest follows the base prefix (no name in the overlay) */
    if (meta_len < off) return NULL;
    *out_len = (size_t)(meta_len - off);
    return meta + off;
}

int dart_meta_interest_next(const uint8_t *meta, uint16_t meta_len,
                            DartInterestIter *it, DartTopic *out){
    uint32_t off; uint8_t nlen;
    if (!it || !out) return 0;
    if (!it->started){                    /* first call: parse the [npub][nsub] header */
        size_t il = 0;
        const uint8_t *in = dart_meta_interest(meta, meta_len, &il);
        it->started = 1; it->pub_left = it->sub_left = 0; it->off = 0;
        if (!in || il < 4) return 0;      /* no/short interest list: nothing to yield */
        it->pub_left = (uint16_t)(in[0] | ((uint16_t)in[1] << 8));
        it->sub_left = (uint16_t)(in[2] | ((uint16_t)in[3] << 8));
        it->off = (uint32_t)(in - meta) + 4u;   /* first entry, past npub/nsub */
    }
    if (it->pub_left == 0 && it->sub_left == 0) return 0;
    off = it->off;
    if (off + 4u > meta_len){ it->pub_left = it->sub_left = 0; return 0; }   /* truncated: stop */
    nlen = meta[off + 3];
    if (off + 4u + nlen > meta_len){ it->pub_left = it->sub_left = 0; return 0; }
    out->alias    = (uint16_t)(meta[off] | ((uint16_t)meta[off + 1] << 8));
    out->reliable = (uint8_t)(meta[off + 2] & DART_META_F_RELIABLE);
    out->is_pub   = (uint8_t)(it->pub_left > 0);   /* pub list first, then sub */
    out->name     = (const char *)(meta + off + 4u);
    out->name_len = nlen;
    it->off = off + 4u + nlen;
    if (it->pub_left > 0) it->pub_left--; else it->sub_left--;
    return 1;
}

#ifdef DART_SHM
int dart_meta_shm(const uint8_t *meta, uint16_t meta_len, uint8_t host[16]){
    if (!dart__meta_ok(meta, meta_len) || meta[2]!=7
        || meta_len < DART__META_BASE_SHM || !meta[5]) return 0;
    memcpy(host, meta+6, 16);
    return 1;
}
#endif


int dart_set_role(DartTransportState *st, uint16_t channel, uint8_t role){
    int channel_idx; i_DartChannel *ch; uint16_t p;
    if (role > DART_INACTIVE) return -1;
    ch = dart_chan(st, channel, &channel_idx);
    if (!ch) return -1;
    if (ch->role == role) return 0;
    ch->role = role;
    for (p=0;p<st->cfg.max_peers;p++)
        if (st->peer_used[p]) dart__rematch(st,(uint16_t)channel_idx,p);
    /* caller re-advertises interest (the node bumps its discovery announce) */
    return 0;
}


int dart_channel_define(DartTransportState *st, uint16_t channel, const DartChannelDef *def){
    i_DartChannel *ch; DartQos q; uint16_t depth, p; size_t lane;
    if (!st || !def || !st->cfg.allocator) return -1;       /* dynamic (reserve) mode only */
    if (channel >= st->cfg.n_channels) return -1;            /* out of reserved range */
    if (!def->name || !def->name[0]) return -1;             /* name = identity, required */
    lane = dart__namelen(def->name);
    if (def->name[lane]) return -1;                          /* longer than DART_TOPIC_NAME_MAX */
    ch = &st->channels[channel];
    if (ch->identity != 0 || ch->history) return -1;        /* slot already defined */
    q = def->qos; dart__qos_defaults(&q, 1);                 /* dynamic: grow-to-fit buffers */
    depth = q.keep_last;
    ch->history = (i_DartWriterSample*)st->cfg.allocator(st->cfg.user, NULL,
                                                         (size_t)depth*sizeof(i_DartWriterSample));
    if (!ch->history) return -4;                            /* OOM */
    memset(ch->history, 0, (size_t)depth*sizeof(i_DartWriterSample));
    ch->history_owned = 1; ch->dynamic = 1;
    ch->qos = q; ch->max_fragments = dart_max_fragments(q.max_message_bytes);
    ch->role = def->role; ch->multicast = def->multicast;
    ch->identity = dart_channel_identity(def);
    memcpy((char*)ch->name, def->name, lane); ((char*)ch->name)[lane] = '\0';
    ch->history_head = 0; ch->next_seqno = 0; ch->have_first = 0;
    for (p=0;p<st->cfg.max_peers;p++)        /* match the newly active channel to known peers */
        if (st->peer_used[p]) dart__rematch(st, channel, p);
    return 0;
}


const char *dart_channel_name(DartTransportState *st, uint16_t channel, uint8_t *len){
    i_DartChannel *ch = dart_chan(st, channel, NULL);
    if (!ch || !ch->name || !ch->name[0]){ if (len) *len = 0; return NULL; }
    if (len) *len = (uint8_t)dart__namelen(ch->name);
    return ch->name;
}


const DartQos *dart_channel_qos(DartTransportState *st, uint16_t channel){
    i_DartChannel *ch = dart_chan(st, channel, NULL);
    return ch ? &ch->qos : NULL;
}


void dart_repair_stats(DartTransportState *st, uint16_t channel, DartRepairStats *out){
    i_DartChannel *ch = dart_chan(st, channel, NULL);
    if (!out) return;
    if (ch) *out = ch->repair_stats;
    else memset(out, 0, sizeof *out);
}


void dart_on_datagram(DartTransportState *st, uint32_t from, const void *datagram, size_t len, uint64_t now){
    const uint8_t *p=(const uint8_t*)datagram; size_t rem=len;
    int peer_slot=dart_peer_slot(st,from);
    if (peer_slot<0) return;
    /* concatenated submessages; each length comes from its header, so no framing */
    while (rem>=3){
        uint8_t b0=p[0], type=(uint8_t)(b0 & DART_MSG_MASK); uint16_t alias; size_t sub; int channel_idx;
        switch(type){
            case DART_DATA:
#ifdef DART_SHM
                            if (b0 & DART_F_SHM){ if (rem<DART_SHM_DATA_BYTES) return; sub=DART_SHM_DATA_BYTES; }
                            else
#endif
                            if (b0 & DART_F_SINGLE){ if (rem<DART_HEADER_DATA_SINGLE) return; sub=DART_HEADER_DATA_SINGLE+(size_t)dart_le_r16(p+DART_OFFSET_PAYLOAD_LEN_SINGLE); }
                            else { if (rem<DART_HEADER_DATA_MULTI) return; sub=DART_HEADER_DATA_MULTI+(size_t)dart_le_r16(p+DART_OFFSET_PAYLOAD_LEN); } break;
            case DART_HB:   if (rem<DART_HEADER_HB) return; sub=DART_HEADER_HB; break;
            case DART_NACK: if (rem<DART_HEADER_NACK) return; sub=DART_HEADER_NACK; break;
            default: return;             /* unknown type: cannot resync, drop rest */
        }
        if (sub>rem) return;             /* truncated/malformed */
        alias = dart_le_r16(p+DART_OFFSET_ALIAS);
        if ((uint32_t)alias < st->alias_max){
            uint16_t m=st->alias_to_channel[(size_t)peer_slot*st->alias_max+alias]; channel_idx=(m==0xFFFFu)?-1:(int)m;
        } else channel_idx=-1;
        if (channel_idx>=0){
            switch(type){
                case DART_DATA:
#ifdef DART_SHM
                                if (p[0] & DART_F_SHM){ dart_reader_shm(st,channel_idx,peer_slot,p,now); break; }
#endif
                                dart_reader_data(st,channel_idx,peer_slot,p,now); break;
                case DART_HB:   dart_reader_hb  (st,channel_idx,peer_slot,p,now); break;
                case DART_NACK: dart_writer_nack(st,channel_idx,peer_slot,p); break;
            }
        }
        p+=sub; rem-=sub;
    }
}
#pragma endregion

#ifndef DART_TRANSPORT_SANS_IO
#pragma region shm/core.c
/* dart_shm: the portable segment-mapping + chunk module behind dart_shm.h. Pure
 * over dart_plat (shm mapping, host uuid, the generation atomic); no transport or
 * node knowledge. Compiles to nothing without DART_SHM. See dart_shm.h. */


#ifdef DART_SHM
#include <string.h>
#include <stdlib.h>

size_t dart_shm_desc_encode(const i_DartShmDesc *d, uint8_t out[DART_SHM_DESC_WIRE]){
    dart_le_w64(out,    d->segment_id);
    dart_le_w32(out+8,  d->chunk);
    dart_le_w32(out+12, d->length);
    dart_le_w64(out+16, d->generation);
    return DART_SHM_DESC_WIRE;
}
int dart_shm_desc_decode(i_DartShmDesc *d, const uint8_t *in, size_t len){
    if (len < DART_SHM_DESC_WIRE) return 0;
    d->segment_id = dart_le_r64(in);
    d->chunk      = dart_le_r32(in+8);
    d->length     = dart_le_r32(in+12);
    d->generation = dart_le_r64(in+16);
    return 1;
}

/* OS object name "/dart.shm.<16 hex>" -- valid on POSIX (leading /) and Windows. */
void dart_shm_seg_name(char *buf, uint64_t segment_id){
    static const char hex_digits[] = "0123456789abcdef";
    const char prefix[] = "/dart.shm."; int i, k = 0;
    while (prefix[k]){ buf[k] = prefix[k]; k++; }
    for (i=15;i>=0;i--) buf[k++] = hex_digits[(segment_id >> (4*i)) & 0xF];
    buf[k] = 0;
}

uint32_t dart_shm_class_bytes(uint32_t k){ return DART_SHM_CLASS_BASE << (k*DART_SHM_CLASS_SHIFT); }
uint32_t dart_shm_class_for(uint32_t len){
    uint32_t k;
    for (k=0;k<DART_SHM_N_CLASSES;k++) if (dart_shm_class_bytes(k) >= len) return k;
    return DART_SHM_N_CLASSES;   /* bigger than the top class -> caller sends inline */
}

struct i_DartShmPool {
    void    *base;          /* mapping base */
    void    *handle;        /* OS handle for detach */
    size_t   map_bytes;     /* total mapped size */
    i_DartShmSegHdr *hdr;
    uint8_t *chunks;        /* base of the chunk region */
    uint32_t chunk_bytes;
    uint32_t n_chunks;
    uint32_t stride;        /* per-chunk bytes incl. header */
    int      is_creator;
};

size_t dart_shm_state_bytes(void){ return dart_align_up(sizeof(struct i_DartShmPool), 16u); }

#define DART__SHM_HDR_SZ  ((uint32_t)dart_align_up(sizeof(i_DartShmSegHdr), 16u))
#define DART__SHM_CHDR_SZ ((uint32_t)dart_align_up(sizeof(i_DartShmChunkHdr), 16u))

static void dart__shm_geom(uint32_t chunk_bytes, uint32_t n_chunks,
                           uint32_t *out_stride, size_t *out_total){
    uint32_t aligned = (uint32_t)dart_align_up(chunk_bytes, 16u);
    uint32_t stride = DART__SHM_CHDR_SZ + aligned;
    *out_stride = stride;
    *out_total  = (size_t)DART__SHM_HDR_SZ + (size_t)n_chunks * stride;
}

static i_DartShmChunkHdr *dart__shm_chunk_hdr(struct i_DartShmPool *p, uint32_t i){
    return (i_DartShmChunkHdr*)(p->chunks + (size_t)i * p->stride);
}
static uint8_t *dart__shm_chunk_pay(struct i_DartShmPool *p, uint32_t i){
    return (uint8_t*)dart__shm_chunk_hdr(p, i) + DART__SHM_CHDR_SZ;
}

i_DartShmPool *dart_shm_create(void *pool_mem, const i_DartShmConfig *cfg){
    struct i_DartShmPool *p = (struct i_DartShmPool*)pool_mem;
    uint32_t chunk_bytes = cfg->chunk_bytes ? cfg->chunk_bytes : DART_SHM_CHUNK_BYTES;
    uint32_t n_chunks = cfg->n_chunks    ? cfg->n_chunks    : DART_SHM_CHUNKS;
    uint32_t stride; size_t total; void *handle = NULL, *base; uint32_t i;
    if (!p || !cfg) return NULL;
    dart__shm_geom(chunk_bytes, n_chunks, &stride, &total);
    base = dart_plat_shm_create(cfg->name, total, &handle);
    if (!base) return NULL;
    memset(p, 0, sizeof *p);
    p->base = base; p->handle = handle; p->map_bytes = total;
    p->hdr = (i_DartShmSegHdr*)base;
    p->chunks = (uint8_t*)base + DART__SHM_HDR_SZ;
    p->chunk_bytes = chunk_bytes; p->n_chunks = n_chunks; p->stride = stride; p->is_creator = 1;
    /* the segment starts zero-filled; stamp the header and clear generations */
    p->hdr->magic = DART_SHM_MAGIC; p->hdr->version = DART_SHM_VERSION;
    p->hdr->segment_id = cfg->segment_id; p->hdr->chunk_bytes = chunk_bytes; p->hdr->n_chunks = n_chunks;
    p->hdr->owner_pid = dart_plat_pid();
    dart_plat_host_uuid(p->hdr->owner_host);
    for (i = 0; i < n_chunks; i++){ i_DartShmChunkHdr *c = dart__shm_chunk_hdr(p, i); c->generation = 0; c->length = 0; }
    return p;
}

i_DartShmPool *dart_shm_attach(void *pool_mem, const i_DartShmConfig *cfg){
    struct i_DartShmPool *p = (struct i_DartShmPool*)pool_mem;
    uint32_t chunk_bytes, n_chunks, stride; size_t map_bytes = 0, expect;
    void *handle = NULL, *base; uint8_t ours[16];
    if (!p || !cfg) return NULL;
    /* map the whole OS object; its geometry (chunk_bytes/n_chunks) comes from the
       header the writer stamped, so the reader needs to know nothing up front --
       cfg's chunk_bytes/n_chunks are create-only. */
    base = dart_plat_shm_attach(cfg->name, &map_bytes, &handle);
    if (!base) return NULL;
    memset(p, 0, sizeof *p);
    p->base = base; p->handle = handle; p->map_bytes = map_bytes;
    p->hdr = (i_DartShmSegHdr*)base;
    dart_plat_host_uuid(ours);
    chunk_bytes = p->hdr->chunk_bytes; n_chunks = p->hdr->n_chunks;
    dart__shm_geom(chunk_bytes, n_chunks, &stride, &expect);
    /* reject a stale/foreign/mismatched/truncated segment -> caller falls back to UDP */
    if (p->hdr->magic != DART_SHM_MAGIC || p->hdr->version != DART_SHM_VERSION ||
        memcmp(p->hdr->owner_host, ours, 16) != 0 ||
        chunk_bytes == 0 || n_chunks == 0 || expect > map_bytes){
        dart_plat_shm_detach(base, map_bytes, handle, 0);
        return NULL;
    }
    p->chunks = (uint8_t*)base + DART__SHM_HDR_SZ;
    p->chunk_bytes = chunk_bytes; p->n_chunks = n_chunks; p->stride = stride; p->is_creator = 0;
    return p;
}

void *dart_shm_chunk(i_DartShmPool *p, uint32_t chunk, uint32_t *out_cap){
    if (!p || chunk >= p->n_chunks) return NULL;
    if (out_cap) *out_cap = p->chunk_bytes;
    return dart__shm_chunk_pay(p, chunk);
}

void dart_shm_stamp(i_DartShmPool *p, uint32_t chunk, uint32_t len, i_DartShmDesc *out){
    i_DartShmChunkHdr *c;
    uint64_t generation;
    if (!p || chunk >= p->n_chunks) return;
    c = dart__shm_chunk_hdr(p, chunk);
    c->length = len;
    generation = c->generation + 1u;                       /* bump so a straggler sees the reuse */
    dart_plat_atomic_store64(&c->generation, generation);  /* release: publishes the payload writes */
    if (out){ out->segment_id = p->hdr->segment_id; out->chunk = chunk; out->length = len; out->generation = generation; }
}

const void *dart_shm_read(i_DartShmPool *p, const i_DartShmDesc *d, uint32_t *out_len){
    i_DartShmChunkHdr *c;
    if (!p || !d || d->chunk >= p->n_chunks) return NULL;
    c = dart__shm_chunk_hdr(p, d->chunk);
    if (dart_plat_atomic_load64(&c->generation) != d->generation) return NULL;  /* recycled */
    if (d->length > p->chunk_bytes) return NULL;
    if (out_len) *out_len = d->length;
    return dart__shm_chunk_pay(p, d->chunk);
}

int dart_shm_verify(i_DartShmPool *p, const i_DartShmDesc *d){
    i_DartShmChunkHdr *c;
    if (!p || !d || d->chunk >= p->n_chunks) return 0;
    c = dart__shm_chunk_hdr(p, d->chunk);
    return dart_plat_atomic_load64(&c->generation) == d->generation;
}

void dart_shm_detach(i_DartShmPool *p){
    if (!p || !p->base) return;
    dart_plat_shm_detach(p->base, p->map_bytes, p->handle, p->is_creator);
    p->base = NULL; p->handle = NULL;
}

int dart_shm_host_match(const uint8_t peer_host[16], const uint8_t our_host[16]){
    return memcmp(peer_host, our_host, 16) == 0;
}

#endif /* DART_SHM */
#pragma endregion
#pragma region node/core.c
/* sans-IO NODE core: peer table + the discovery->transport lifecycle. No platform
 * access; a runtime drives it (see node/runtime.c) and does the IO. See node/core.h. */

#include <string.h>

/* dart_event_str + bounded appenders: format the node's app DartEvent as one line.
   No stdio, so it stays in the sans-IO core. (It covers the union event, including the
   peer/interest/mcast kinds the node adds; the transport keeps no formatter of its own.) */
static char *i_ev_str(char *p, char *end, const char *s){
    if (!s) return p;
    while (*s && p < end) *p++ = *s++;
    return p;
}
static char *i_ev_u64(char *p, char *end, uint64_t v){
    char tmp[20]; int n = 0;
    do { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v);
    while (n && p < end) *p++ = tmp[--n];
    return p;
}
static char *i_ev_hex(char *p, char *end, uint64_t v){
    char tmp[16]; int n = 0;
    do { int d = (int)(v & 0xF); tmp[n++] = (char)(d < 10 ? '0'+d : 'a'+d-10); v >>= 4; } while (v);
    while (n && p < end) *p++ = tmp[--n];
    return p;
}
static char *i_ev_addr(char *p, char *end, const DartEvent *ev){   /* dotted quad + :port (IPv4 only) */
    int i;
    for (i = 0; i < 4; i++){ if (i) p = i_ev_str(p,end,"."); p = i_ev_u64(p,end,ev->ip[i]); }
    p = i_ev_str(p,end,":"); return i_ev_u64(p,end,ev->port);
}

const char *dart_event_str(const DartEvent *ev, char *buf, size_t cap){
    char *p, *end;
    if (!buf || !cap) return buf;
    p = buf; end = buf + cap - 1;                  /* reserve one byte for the NUL */
    switch (ev->kind){
    case DART_PEER_UP:
        p = i_ev_str(p,end,"peer-up id="); p = i_ev_u64(p,end,ev->peer);
        if (ev->ip_len == 4){ p = i_ev_str(p,end," at "); p = i_ev_addr(p,end,ev); }
        p = i_ev_str(p,end," ("); p = i_ev_str(p,end,ev->detail); p = i_ev_str(p,end,")");
        break;
    case DART_PEER_DOWN:
        p = i_ev_str(p,end,"peer-down id="); p = i_ev_u64(p,end,ev->peer);
        p = i_ev_str(p,end," ("); p = i_ev_str(p,end,ev->detail); p = i_ev_str(p,end,")");
        break;
    case DART_PEER_INTEREST:
        p = i_ev_str(p,end,"interest id="); p = i_ev_u64(p,end,ev->peer);
        p = i_ev_str(p,end," publish-to="); p = i_ev_u64(p,end,ev->publish_topics);
        p = i_ev_str(p,end," topics, receive-from="); p = i_ev_u64(p,end,ev->receive_topics);
        p = i_ev_str(p,end," topics");
        break;
    case DART_PEER_REFUSED:
        p = i_ev_str(p,end,"peer-refused at "); p = i_ev_addr(p,end,ev);
        p = i_ev_str(p,end," (table full of active peers)");
        break;
    case DART_NAME_COLLISION:
        p = i_ev_str(p,end,"name-collision ch="); p = i_ev_u64(p,end,ev->channel);
        p = i_ev_str(p,end," id=0x"); p = i_ev_hex(p,end,ev->identity);
        p = i_ev_str(p,end," ("); p = i_ev_str(p,end,ev->detail);
        p = i_ev_str(p,end,"): match refused");
        break;
    case DART_QOS_INCOMPATIBLE:
        p = i_ev_str(p,end,"qos-incompatible ch="); p = i_ev_u64(p,end,ev->channel);
        p = i_ev_str(p,end," from id="); p = i_ev_u64(p,end,ev->peer);
        p = i_ev_str(p,end," ("); p = i_ev_str(p,end,ev->detail);
        p = i_ev_str(p,end,"): reliable subscriber refused best-effort publisher");
        break;
    case DART_MSG_LOST:
        p = i_ev_str(p,end,"msg-lost ch="); p = i_ev_u64(p,end,ev->channel);
        p = i_ev_str(p,end," from id="); p = i_ev_u64(p,end,ev->peer);
        p = i_ev_str(p,end," seqno "); p = i_ev_u64(p,end,ev->lost_first);
        p = i_ev_str(p,end,".."); p = i_ev_u64(p,end,ev->lost_first + ev->lost_count - 1);
        break;
    case DART_MSG_TOO_BIG:
        p = i_ev_str(p,end,"msg-too-big ch="); p = i_ev_u64(p,end,ev->channel);
        p = i_ev_str(p,end," from id="); p = i_ev_u64(p,end,ev->peer);
        p = i_ev_str(p,end," ("); p = i_ev_u64(p,end,ev->too_big_bytes);
        p = i_ev_str(p,end," bytes), skipped");
        break;
    case DART_MCAST_JOIN_FAILED:
        p = i_ev_str(p,end,"mcast-join-failed ch="); p = i_ev_u64(p,end,ev->channel);
        p = i_ev_str(p,end," ("); p = i_ev_str(p,end,ev->detail); p = i_ev_str(p,end,")");
        break;
    }
    *p = '\0';                                     /* p <= end = buf+cap-1, in range */
    return buf;
}

/* the node core's per-peer transport-lifecycle state, kept in the discovery peer's user
   scratch (so the node holds NO peer table of its own). added = wired into the transport
   yet (dart_peer_add called); dormant = discovery DROPPED it, kept for a same-incarnation
   resume. Discovery zeroes this when a new UUID takes the slot, preserves it on resume. */
typedef struct { uint8_t added; uint8_t dormant; } i_DartNodePeerExtra;

struct i_DartNodeCore {
    DartTransportState           *transport;
    DartDiscoveryState  *discovery;   /* the peer table (id<->addr, name, user scratch) we delegate to */
    DartEventFn         on_event;
    void                 *user;
    i_DartNodeIsLocalFn is_local;
    void                 *is_local_user;
    int                   oob_capable;
    uint8_t               oob_host[16];
    uint8_t              *meta_buf;    /* our outgoing discovery announce blob */
    uint16_t              meta_cap;
    uint16_t              meta_len;
    uint16_t              frag_size;   /* baked into the overlay */
};

uint16_t dart_node_core_peer_user_bytes(void){ return (uint16_t)sizeof(i_DartNodePeerExtra); }
void dart_node_core_bind_discovery(i_DartNodeCore *c, DartDiscoveryState *discovery){ c->discovery = discovery; }

/* arena layout: the core struct, then the announce-blob buffer (no peer table -- that
   lives in the discovery core). One sequence so measure and build agree. */
static void dart__core_layout(i_DartBump *b, uint16_t n_channels,
                              i_DartNodeCore **out_c, uint8_t **out_meta){
    i_DartNodeCore *c = (i_DartNodeCore*)dart_take(b, sizeof(struct i_DartNodeCore), 16);
    uint8_t *meta     = (uint8_t*)       dart_take(b, dart_meta_capacity(n_channels), 16);
    if (out_c)    *out_c    = c;
    if (out_meta) *out_meta = meta;
}

size_t dart_node_core_required_memory(uint16_t n_channels){
    i_DartBump b; memset(&b, 0, sizeof b);
    dart__core_layout(&b, n_channels, NULL, NULL);
    return b.offset + 16u;   /* slack to align the caller's mem up to base */
}

i_DartNodeCore *dart_node_core_init(void *mem, size_t cap, const i_DartNodeCoreConfig *cfg){
    i_DartBump b; i_DartNodeCore *c; uint8_t *base, *meta;
    if (!mem || !cfg || !cfg->transport) return NULL;
    if (cap < dart_node_core_required_memory(cfg->n_channels)) return NULL;
    base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    memset(&b, 0, sizeof b); b.base = base; b.cap = cap - (size_t)(base - (uint8_t*)mem);
    dart__core_layout(&b, cfg->n_channels, &c, &meta);

    memset(c, 0, sizeof *c);
    c->transport     = cfg->transport;
    c->discovery     = cfg->discovery;     /* may be NULL now, bound via bind_discovery later */
    c->on_event      = cfg->on_event;      c->user = cfg->user;
    c->is_local      = cfg->is_local;      c->is_local_user = cfg->is_local_user;
    c->oob_capable   = cfg->oob_capable;
    memcpy(c->oob_host, cfg->oob_host, 16);
    c->meta_buf      = meta;
    c->meta_cap      = dart_meta_capacity(cfg->n_channels);
    c->frag_size     = cfg->frag_size;
    return c;
}

/* Relocate the sans-IO node core into a bigger block at grown counts. No peer table (it
 * lives in discovery); a struct copy carries the scalars. The transport, discovery, and
 * announce-blob pointers are re-pointed by the caller after those move. */
i_DartNodeCore *dart_node_core_migrate(i_DartNodeCore *old, void *new_mem, size_t new_cap,
                                       uint16_t new_n_channels){
    i_DartBump b; i_DartNodeCore *c; uint8_t *base, *meta;
    if (!old) return NULL;
    if (new_cap < dart_node_core_required_memory(new_n_channels)) return NULL;
    base = (uint8_t*)(((uintptr_t)new_mem + 15u) & ~(uintptr_t)15u);
    memset(&b, 0, sizeof b); b.base = base; b.cap = new_cap - (size_t)(base - (uint8_t*)new_mem);
    dart__core_layout(&b, new_n_channels, &c, &meta);
    *c = *old;                          /* scalars + transport/discovery ptrs (caller re-points) */
    c->meta_buf  = meta;                /* caller rebuilds the blob into it */
    c->meta_cap  = dart_meta_capacity(new_n_channels);
    return c;
}

/* (Re)build our discovery OVERLAY (frag size + OOB host + interest) from the core's
   current fields. The codec lives in the transport core; the OOB fields default to 0 in a
   non-SHM build. The node NAME is not here: the runtime hands it to discovery directly. */
uint16_t dart_node_core_build_meta(i_DartNodeCore *c){
    c->meta_len = dart_meta_build(c->transport, c->meta_buf, c->meta_cap,
                                  c->frag_size, c->oob_capable, c->oob_host);
    return c->meta_len;
}

const uint8_t *dart_node_core_meta(i_DartNodeCore *c, uint16_t *len){
    if (len) *len = c->meta_len;
    return c->meta_buf;
}

#ifdef DART_SHM
/* a peer can receive our out-of-band (SHM) payload iff we are OOB-capable, it
 * advertised an OOB host in its meta blob, and that host equals ours (same kernel).
 * Set the transport's per-peer flag. The core knows nothing of SHM beyond this. */
static void dart__core_set_peer_oob(i_DartNodeCore *c, uint32_t id, const uint8_t *meta, uint16_t meta_len){
    uint8_t host[16];
    int oob = c->oob_capable && dart_meta_shm(meta, meta_len, host) &&
              memcmp(host, c->oob_host, 16) == 0;
    dart_peer_set_shm(c->transport, id, oob);
}
#else
#define dart__core_set_peer_oob(c, id, meta, meta_len) ((void)0)
#endif

static void dart__core_fire(i_DartNodeCore *c, DartEventKind kind, uint32_t id,
                            const DartDiscoveryAddr *addr, const char *detail){
    DartEvent ev;
    if (!c->on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind; ev.peer = id; ev.detail = detail; ev.user = c->user;
    if (addr){ memcpy(ev.ip, addr->ip, 16); ev.ip_len = addr->ip_len; ev.port = addr->port; }
    c->on_event(&ev);
}

/* fired whenever a peer's interest list is (re)applied to the transport: reports how
 * many topics now flow each way, so an app/example can watch a connection form. */
static void dart__core_fire_interest(i_DartNodeCore *c, uint32_t id){
    DartEvent ev; uint16_t publish_to = 0, receive_from = 0;
    if (!c->on_event) return;
    dart_peer_match_counts(c->transport, id, &publish_to, &receive_from);
    memset(&ev, 0, sizeof ev);
    ev.kind = DART_PEER_INTEREST; ev.peer = id;
    ev.publish_topics = publish_to; ev.receive_topics = receive_from;
    ev.detail = "interest applied"; ev.user = c->user;
    c->on_event(&ev);
}

/* the peer's transport-lifecycle state lives in the discovery peer's user scratch; the
 * node core keeps no table of its own. NULL only before discovery is bound (no events yet). */
static i_DartNodePeerExtra *dart__core_extra(i_DartNodeCore *c, uint32_t id){
    return (i_DartNodePeerExtra*)dart_discovery_peer_user(c->discovery, id);
}

static void dart__core_peer_up(i_DartNodeCore *c, uint32_t id, const DartDiscoveryAddr *addr,
                            const uint8_t *meta, uint16_t meta_len){
    i_DartNodePeerExtra *ex = dart__core_extra(c, id);
    uint16_t frag = dart_meta_frag(meta, meta_len);
    size_t interest_len = 0; const uint8_t *interest = dart_meta_interest(meta, meta_len, &interest_len);
    if (!ex) return;                                  /* discovery not bound / no scratch */
    if (!ex->added){                                  /* brand-new peer: wire it into the transport */
        int local = (addr->ip_len==4) && c->is_local && c->is_local(c->is_local_user, addr->ip, addr->ip_len);
        dart_peer_add(c->transport, id, local, frag);   /* blob carries frag + pub/sub interest */
        ex->added = 1; ex->dormant = 0;
        dart__core_set_peer_oob(c, id, meta, meta_len);
        dart__core_fire(c, DART_PEER_UP, id, addr, "peer discovered");
        if (interest){ dart_apply_peer_interest(c->transport, id, interest, interest_len);
                       dart__core_fire_interest(c, id); }
    } else {                                          /* known peer: addr/interest update */
        dart_peer_set_frag(c->transport, id, frag);
        dart__core_set_peer_oob(c, id, meta, meta_len);
        if (interest){ dart_apply_peer_interest(c->transport, id, interest, interest_len);
                       dart__core_fire_interest(c, id); }
        if (ex->dormant){    /* a DROPPED peer's same incarnation returned: resume */
            ex->dormant = 0;
            dart_peer_resume(c->transport, id);   /* keeps reader position; writer fills any gap */
            dart__core_fire(c, DART_PEER_UP, id, addr, "peer resumed");
        }
    }
}

static void dart__core_peer_down(i_DartNodeCore *c, uint32_t id, DartDiscoveryDownReason reason){
    i_DartNodePeerExtra *ex = dart__core_extra(c, id);   /* discovery frees the slot AFTER this event */
    if (reason == DART_DISCOVERY_DROP){
        /* fell silent: keep transport state so a same-incarnation return resumes
           losslessly; just stop flow-controlling it and tell the app once */
        if (ex && ex->added && !ex->dormant){
            ex->dormant = 1;
            dart_peer_dormant(c->transport, id);
            dart__core_fire(c, DART_PEER_DOWN, id, NULL, "peer dropped");
        }
    } else {   /* GONE: said BYE or its slot was reclaimed; free the transport state */
        int notify = (ex && ex->added && !ex->dormant);   /* active->gone: app not yet told */
        dart_peer_remove(c->transport, id);   /* discovery zeroes the scratch on slot reuse */
        if (notify) dart__core_fire(c, DART_PEER_DOWN, id, NULL, "peer lost");
    }
}

static void dart__core_peer_refused(i_DartNodeCore *c, const DartDiscoveryAddr *addr){
    dart__core_fire(c, DART_PEER_REFUSED, 0, addr, "peer table full (all active)");
}

/* The discovery core's on_event sink (cfg.user = this core): demux the generic
 * DartDiscoveryEvent into the lifecycle handlers above, which fire the app DartEvents. */
void dart_node_core_on_disc_event(const DartDiscoveryEvent *ev){
    i_DartNodeCore *c = (i_DartNodeCore*)ev->user;
    switch (ev->kind){
        case DART_DISCOVERY_PEER_UP:
            dart__core_peer_up(c, ev->peer, &ev->addr, ev->meta, ev->meta_len);  /* name lives in discovery */
            break;
        case DART_DISCOVERY_PEER_DOWN:
            dart__core_peer_down(c, ev->peer, ev->reason);
            break;
        case DART_DISCOVERY_PEER_REFUSED:
            dart__core_peer_refused(c, &ev->addr);
            break;
        default: break;
    }
}

int dart_node_core_resolve(i_DartNodeCore *c, uint32_t to, i_DartNodeDest *out){
    DartDiscoveryAddr a;
    memset(out, 0, sizeof *out);
    if (DART_DEST_IS_GROUP(to)){            /* the transport's group encoding stays inside the core */
        out->is_group = 1;
        out->group_sel = DART_DEST_GROUP_CHAN(to);
        return 1;
    }
    if (!dart_discovery_addr_of_id(c->discovery, to, &a)) return 0;   /* peer vanished */
    memcpy(out->ip, a.ip, 16);
    out->ip_len = a.ip_len;
    out->port   = a.port;
    return 1;
}

int dart_node_core_id_for_addr(i_DartNodeCore *c, const uint8_t ip[4], uint16_t port, uint32_t *id){
    return dart_discovery_id_for_addr(c->discovery, ip, 4, port, id);
}

const char *dart_node_core_peer_name(i_DartNodeCore *c, uint32_t id, uint8_t *out_len){
    uint8_t nl = 0;
    const char *name = dart_discovery_peer_name(c->discovery, id, &nl);
    if (!name){ if (out_len) *out_len = 0; return NULL; }   /* not a known peer */
    if (nl == 0){ name = "unknown-peer"; nl = 12; }         /* never empty for a known peer */
    if (out_len) *out_len = nl;
    return name;
}

uint16_t dart_node_core_max_peers(i_DartNodeCore *c){ return dart_discovery_max_peers(c->discovery); }

int dart_node_core_peer_at(i_DartNodeCore *c, uint16_t slot, uint32_t *id,
                           uint8_t ip[16], uint8_t *ip_len, uint16_t *port){
    DartDiscoveryPeer v;
    if (!dart_discovery_peer_at(c->discovery, slot, &v)) return 0;
    if (id)     *id = v.id;
    if (ip)     memcpy(ip, v.addr.ip, 16);
    if (ip_len) *ip_len = v.addr.ip_len;
    if (port)   *port = v.addr.port;
    return 1;
}
#pragma endregion
#pragma region node/runtime.c
/* NODE runtime: owns the data sockets, drives discovery, and the clock; it wires
 * peers into the transport via the sans-IO node core (node/core.h). Channels are
 * created at runtime and handed back as opaque handles. All OS access goes through
 * dart_plat. See node/runtime.h for the public dart_node_* / dart_channel_* API. */

#ifdef DART_SHM
#endif
#include <string.h>    /* heap access goes through dart_plat_realloc (no <stdlib.h> here) */

struct DartChannel { DartNode *n; uint16_t index; };

struct DartNode {
    DartTransportState     *transport;
    i_DartNodeCore *core;     /* peer table (id<->address) + discovery lifecycle (sans-IO) */
    DartDiscovery     *discovery;
    i_DartSock       fd;       /* unicast data socket (also group TX) */
    i_DartSock       multicast_fd;     /* multicast data RX socket (DART_SOCK_BAD until needed) */
    uint16_t      domain;
    uint16_t      multicast_port;
    DartNodeNet  net;         /* copy of opts.net: lazy multicast setup + socket buffers */
    /* datagram the socket refused; retried first next poll so it is never lost */
    uint8_t       tx_hold[DART_DGRAM_MAX];
    size_t        tx_hold_len;
    uint32_t      tx_hold_peer;
    /* backpressure accumulators, read via dart_node_backpressure_stats */
    uint64_t      backpressure_total_us;
    uint32_t      backpressure_wait_count;
    /* diagnostic: in-pump probe sampled inside the backpressure wait (dart_channel_send) */
    DartPumpProbeFn pump_probe;
    void          *pump_probe_user;
    uint64_t       pump_probe_interval_us;
    /* app callbacks: one message sink (wrapped to build a DartMsg) + everything-else */
    DartMsgFn    user_on_message;
    DartEventFn  on_event;
    void          *user_data;
    void          *arena;      /* the memory block; freed at close iff we own it */
    int            owns_arena;
    /* message-buffer allocation behind dart__node_alloc: static bumps from the arena
       tail [static_pos,static_end); dynamic uses dart_plat_realloc, bounded by mem_cap */
    uint8_t        alloc_dynamic;
    uint8_t        grow_pending;   /* a peer was refused for lack of slots; grow at next poll */
    uint16_t       max_peers;      /* current peer-table capacity (doubles on a dynamic grow) */
    uint8_t       *static_pos, *static_end;
    size_t         mem_cap, mem_used, mem_peak;
    uint64_t       mem_alloc_calls;  /* message-buffer (re)allocations: ~0 in steady state */
    /* channel handles + lazy multicast bookkeeping. handles is a pointer array in the
       arena; each DartChannel struct is a separate stable allocation, so a grow that
       relocates the arena never moves a handle the user holds. */
    DartChannel **handles;    /* [max_channels] -> stable per-channel structs */
    uint16_t      max_channels;
    uint16_t      n_created;
    uint32_t     *joined_groups;/* [max_channels] multicast groups already joined (dedup) */
    uint16_t      n_joined;
    uint32_t      mcast_if;    /* multicast egress/join interface (resolved once) */
    uint8_t       mcast_if_set;
    uint8_t       mcast_tx_setup;
#ifdef DART_SHM
    /* zero-fragment same-host path: lazy per-size-class segments (see shm/core.h) */
    uint8_t        shm_capable;       /* always 1: the node always has an allocator */
    uint8_t        shm_host[16];  /* our host uuid (advertised; same-host check) */
    uint64_t       shm_base;      /* per-node segment id base; low 19 bits = (channel<<3)|class */
    void          *shm_scratch; uint32_t shm_scratch_cap;   /* one-copy receive scratch */
    void         **shm_pool;      /* [nchan*N_CLASSES] our (channel,class) segments (NULL = not created) */
    uint8_t       *shm_pool_mem;  /* arena: nchan * N_CLASSES * dart_shm_state_bytes */
    uint16_t       shm_n_channels;
    uint64_t      *shm_reader_segments;      /* [reader_max] attached reader-segment ids (0 = empty) */
    uint8_t       *shm_reader_pool_mem; /* [reader_max * dart_shm_state_bytes] */
    uint16_t       shm_reader_max;
    uint32_t       shm_tx, shm_rx;/* messages published / delivered via SHM (observability) */
#endif
};

/* The node's message-buffer allocator, handed to the transport as its realloc hook
 * (u is the node). Every block carries a DART_ALLOC_HDR-byte header storing its
 * payload size so realloc/free know the old size: the static path needs it to copy
 * on grow, the dynamic path to maintain the memory cap. The header keeps the
 * returned pointer 16-aligned. Static: bump from the arena tail, free is a no-op
 * (the whole arena is reclaimed at node close). Dynamic: dart_plat_realloc, refused
 * past mem_cap. */
#define DART_ALLOC_HDR 16u
static void *dart__node_alloc(void *u, void *ptr, size_t size){
    DartNode *n = (DartNode*)u;
    uint8_t *base = ptr ? (uint8_t*)ptr - DART_ALLOC_HDR : NULL;
    size_t   old  = base ? *(size_t*)base : 0;

    if (size == 0){                                   /* free */
        if (base && n->alloc_dynamic){
            n->mem_used -= old + DART_ALLOC_HDR;
            dart_plat_realloc(base, 0);
        }
        return NULL;
    }
    if (n->alloc_dynamic){
        size_t newtot = size + DART_ALLOC_HDR, used = n->mem_used;
        uint8_t *nb;
        if (base) used -= old + DART_ALLOC_HDR;
        if (used + newtot > n->mem_cap) return NULL;   /* runaway guard */
        nb = (uint8_t*)dart_plat_realloc(base, newtot);
        if (!nb) return NULL;
        n->mem_used = used + newtot;
        if (n->mem_used > n->mem_peak) n->mem_peak = n->mem_used;
        n->mem_alloc_calls++;                          /* a real heap (re)alloc -- cold in steady state */
        *(size_t*)nb = size;
        return nb + DART_ALLOC_HDR;
    } else {                                           /* static: bump from the arena tail */
        size_t need = DART_ALLOC_HDR + ((size + 15u) & ~(size_t)15u);
        uint8_t *nb;
        if (base && old >= size) return ptr;           /* fits in place: no re-bump */
        if ((size_t)(n->static_end - n->static_pos) < need) return NULL;
        n->mem_alloc_calls++;
        nb = n->static_pos; n->static_pos += need;
        *(size_t*)nb = size;
        if (base) memcpy(nb + DART_ALLOC_HDR, ptr, old);
        return nb + DART_ALLOC_HDR;
    }
}

/* build a DartMsg and hand it to the app (the channel name is a local lookup, never
 * on the wire). Shared by the inline and SHM delivery paths. */
static void dart__deliver(DartNode *n, uint16_t ch, uint32_t from, const void *data, size_t len){
    DartMsg m; uint8_t nl = 0, snl = 0;
    if (!n->user_on_message) return;
    memset(&m, 0, sizeof m);
    m.node = n; m.user = n->user_data;
    m.channel_id = ch; m.sender_id = from;
    m.sender_name = dart_node_core_peer_name(n->core, from, &snl);   /* pointer into discovery state */
    if (!m.sender_name){ m.sender_name = "unknown-peer"; snl = 12; } /* never NULL: no caller null-check */
    m.sender_name_len = snl;
    m.channel_name = dart_channel_name(n->transport, ch, &nl);
    m.channel_name_len = nl;
    m.data = data; m.len = len;
    n->user_on_message(&m);
}
static void dart__node_on_message(void *u, uint16_t ch, uint32_t from, const void *data, size_t len){
    dart__deliver((DartNode*)u, ch, from, data, len);
}
/* node-core events (the app DartEvent: PEER_UP/DOWN/INTEREST/REFUSED) funnel through
 * here; transport events arrive separately via dart__node_on_transport_event. The core
 * sets ev->user to this node; swap it for the app's real user_data before handing on. */
static void dart__node_on_event(const DartEvent *ev){
    DartNode *n = (DartNode*)ev->user;
    /* dynamic mode has no peer cap: a refusal means grow the table (deferred to the next
       poll, out of this callback); the peer re-announces and is admitted, so the app is
       never told it was refused. Static mode keeps the cap and surfaces the event. */
    if (n->alloc_dynamic && ev->kind == DART_PEER_REFUSED){ n->grow_pending = 1; return; }
    if (n->on_event){ DartEvent e = *ev; e.user = n->user_data; n->on_event(&e); }
}

/* transport events arrive as a DartTransportEvent; the node maps them onto its app
 * DartEvent union and hands them on. This is the node combining the two lower layers'
 * events into one app callback (peer events come via the node core, above). */
static void dart__node_on_transport_event(const DartTransportEvent *tev){
    DartNode *n = (DartNode*)tev->user;
    DartEvent e;
    if (!n->on_event) return;
    memset(&e, 0, sizeof e);
    switch (tev->kind){
    case DART_TRANSPORT_MSG_LOST:        e.kind = DART_MSG_LOST; break;
    case DART_TRANSPORT_MSG_TOO_BIG:     e.kind = DART_MSG_TOO_BIG; break;
    case DART_TRANSPORT_NAME_COLLISION:  e.kind = DART_NAME_COLLISION; break;
    case DART_TRANSPORT_QOS_INCOMPATIBLE:e.kind = DART_QOS_INCOMPATIBLE; break;
    default: return;
    }
    e.detail = tev->detail; e.user = n->user_data; e.peer = tev->peer; e.channel = tev->channel;
    e.lost_first = tev->lost_first; e.lost_count = tev->lost_count;
    e.too_big_bytes = tev->too_big_bytes; e.identity = tev->identity;
    n->on_event(&e);
}

#ifdef DART_SHM
/* reader-pool cache capacity: a same-host peer publishes on its channels, one segment
 * each, so size to peers x channels (clamped to the u16 the cache index uses) */
static uint16_t dart__node_shm_reader_max(uint16_t max_peers, uint16_t n_channels){
    uint32_t r = (uint32_t)max_peers * (n_channels ? n_channels : 1u) * DART_SHM_N_CLASSES;
    return (uint16_t)(r > 0xFFFFu ? 0xFFFFu : r);
}
#endif

/* The node arena's sub-blocks, laid out in ONE place so the measure pass (bump.base
 * NULL, read bump.offset) and the build pass (read the pointers) run the same dart_take
 * sequence and can never drift. */
typedef struct {
    uint8_t   *handles, *joined_groups, *node_core, *transport, *discovery;
#ifdef DART_SHM
    uint8_t   *shm_pool, *shm_pool_mem, *shm_reader_segments, *shm_reader_pool;
#endif
    size_t     node_core_bytes, transport_bytes, discovery_bytes;
} i_DartNodeBlocks;

static void dart__node_layout(i_DartBump *b, uint16_t max_peers, uint16_t max_channels,
                              const DartConfig *transport_cfg,
                              const DartDiscoveryNetConfig *discovery_rt_cfg, i_DartNodeBlocks *o){
    o->handles       = (uint8_t*)dart_take(b, (size_t)max_channels * sizeof(DartChannel*), 16);
    o->joined_groups = (uint8_t*)dart_take(b, (size_t)max_channels * sizeof(uint32_t), 8);
    o->node_core_bytes = dart_node_core_required_memory(max_channels);   /* peers live in discovery now */
    o->node_core = (uint8_t*)dart_take(b, o->node_core_bytes, 16);
    o->transport_bytes = dart_required_memory(transport_cfg);
    o->transport = (uint8_t*)dart_take(b, o->transport_bytes, 16);
#ifdef DART_SHM
    {   size_t state_bytes  = dart_shm_state_bytes();
        uint32_t n_segments = (uint32_t)max_channels * DART_SHM_N_CLASSES;
        uint16_t reader_max = dart__node_shm_reader_max(max_peers, max_channels);
        o->shm_pool            = (uint8_t*)dart_take(b, (size_t)n_segments * sizeof(void*), 16);
        o->shm_pool_mem        = (uint8_t*)dart_take(b, (size_t)n_segments * state_bytes, 16);
        o->shm_reader_segments = (uint8_t*)dart_take(b, (size_t)reader_max * 8u, 16);
        o->shm_reader_pool     = (uint8_t*)dart_take(b, (size_t)reader_max * state_bytes, 16);
    }
#endif
    o->discovery_bytes = dart_discovery_placement_memory(discovery_rt_cfg);
    o->discovery = (uint8_t*)dart_take(b, o->discovery_bytes, 16);
}

/* node-core hook: 1 if a peer address is on this host (a route probe selecting that
 * same address as source). Drives OOB/SHM eligibility; the core stays platform-free. */
static int dart__node_is_local(void *user, const uint8_t *ip, uint8_t ip_len){
    uint32_t d;
    (void)user;
    if (ip_len != 4) return 0;
    d = dart_plat_ip4_to_naddr(ip);
    return dart_plat_route_src(d, 7) == d;
}

/* data multicast group from a selector (topic identity & 0xFF); &0xFF wrap is
 * harmless, RX filters by peer table + identity */
static uint32_t dart__node_group_addr(uint16_t domain, uint16_t sel){
    return dart_plat_ipv4(239u, 255u, (uint8_t)(domain & 0xFFu), (uint8_t)(sel & 0xFFu));
}
/* a channel's group, from its identity so it matches the core's group send and every peer */
static uint32_t dart__node_chan_group(uint16_t domain, const DartChannelDef *def){
    return dart__node_group_addr(domain, DART_DEST_GROUP_SEL(dart_channel_identity(def)));
}

/* multicast egress/join interface, resolved once: explicit, else discovery's egress
 * (so multihomed hosts don't pick per-group interfaces and break source matching). */
static uint32_t dart__node_mcast_if(DartNode *n){
    if (!n->mcast_if_set){
        n->mcast_if = n->net.multicast_interface ? dart_plat_parse_ip(n->net.multicast_interface)
            : dart_discovery_mcast_if_for(dart_plat_parse_ip(n->net.discovery_group ? n->net.discovery_group : "239.255.0.7"),
                                   n->net.discovery_port ? n->net.discovery_port : 7400);
        n->mcast_if_set = 1;
    }
    return n->mcast_if;
}

/* set up multicast for a newly created multicast channel: egress on the unicast socket
 * (once), plus a lazily-created RX socket with one IGMP join per distinct group. Over
 * the OS membership cap the join fails: degrade to unicast-only and fire a diagnostic. */
static void dart__node_channel_mcast(DartNode *n, uint16_t index, const DartChannelDef *def){
    uint32_t interface_ip = dart__node_mcast_if(n);
    uint32_t group = dart__node_chan_group(n->domain, def);
    if (def->role != DART_SUB_ONLY && !n->mcast_tx_setup){     /* we publish: set egress once */
        unsigned char ttl = n->net.multicast_ttl ? n->net.multicast_ttl : 1;
        dart_plat_mcast_setif(n->fd, interface_ip);
        dart_plat_mcast_ttl(n->fd, ttl);
        dart_plat_mcast_loop(n->fd, 1);
        n->mcast_tx_setup = 1;
    }
    if (def->role != DART_PUB_ONLY){                           /* we receive: ensure RX socket + join */
        uint16_t j; int dup = 0;
        if (n->multicast_fd == DART_SOCK_BAD){
            i_DartSock m = dart_plat_udp_open();
            if (m == DART_SOCK_BAD) return;
            if (!dart_plat_bind(m, 0, n->multicast_port, 1)){ dart_plat_close(m); return; }
            dart_plat_mcast_loop(m, 1);
            dart_plat_set_nonblock(m);
            if (n->net.recv_buffer_bytes) dart_plat_set_rcvbuf(m, (int)n->net.recv_buffer_bytes);
            n->multicast_fd = m;
        }
        for (j=0;j<n->n_joined;j++) if (n->joined_groups[j]==group){ dup=1; break; }
        if (!dup){
            if (dart_plat_mcast_join(n->multicast_fd, group, interface_ip)){
                n->joined_groups[n->n_joined++] = group;
            } else if (n->on_event){
                DartEvent ev; memset(&ev, 0, sizeof ev);
                ev.kind=DART_MCAST_JOIN_FAILED; ev.channel=index;
                ev.detail="multicast group join failed (over OS membership cap); channel receives unicast only";
                ev.user=n->user_data;
                n->on_event(&ev);
            }
        }
    }
}

/* send one datagram; returns 1 when done with it, 0 only on a would-block TX-full. The
 * core resolves the abstract destination; we just map it to a UDP address. */
static int dart__node_tx(DartNode *n, uint32_t to, const uint8_t *buf, size_t len){
    i_DartNodeDest d; uint8_t ip[4]; uint16_t port;
    if (!dart_node_core_resolve(n->core, to, &d)) return 1;   /* peer vanished */
    if (d.is_group){
        dart_plat_naddr_to_ip4(dart__node_group_addr(n->domain, d.group_sel), ip);
        port = n->multicast_port;
    } else {
        memcpy(ip, d.ip, 4);
        port = d.port;
    }
    if (dart_plat_send(n->fd, buf, len, ip, port) < 0 && dart_plat_would_block())
        return 0;
    return 1;
}

#ifdef DART_SHM
/* lazily create our per-channel segment: chunk_bytes = the channel's locked size class,
 * n_chunks = its keep_last, so history slot i binds chunk i (no free list). NULL on fail. */
static i_DartShmPool *dart__shm_chan_pool(DartNode *n, uint16_t ch, uint32_t k, uint16_t keep_last){
    i_DartShmConfig c; uint8_t *mem; uint64_t seg; size_t idx;
    if (k >= DART_SHM_N_CLASSES) return NULL;
    idx = (size_t)ch * DART_SHM_N_CLASSES + k;
    if (n->shm_pool[idx]) return (i_DartShmPool*)n->shm_pool[idx];
    memset(&c, 0, sizeof c);
    seg = n->shm_base | ((uint64_t)ch << 3) | (uint64_t)k;   /* low 3 bits class, next 16 channel */
    c.segment_id = seg;
    dart_shm_seg_name(c.name, seg);
    c.chunk_bytes = dart_shm_class_bytes(k);
    c.n_chunks = keep_last ? keep_last : 1u;
    mem = n->shm_pool_mem + idx * dart_shm_state_bytes();
    n->shm_pool[idx] = dart_shm_create(mem, &c);
    return (i_DartShmPool*)n->shm_pool[idx];
}
/* lazily attach a peer's segment by id (class is in the low bits); cache it. */
static i_DartShmPool *dart__shm_reader_pool(DartNode *n, uint64_t seg){
    uint16_t i, slot = 0xFFFF; i_DartShmConfig c; uint8_t *mem; size_t state_bytes;
    state_bytes = dart_shm_state_bytes();
    for (i=0;i<n->shm_reader_max;i++){
        if (n->shm_reader_segments[i]==seg) return (i_DartShmPool*)(n->shm_reader_pool_mem + (size_t)i*state_bytes);
        if (n->shm_reader_segments[i]==0 && slot==0xFFFF) slot = i;
    }
    if (slot==0xFFFF) return NULL;                         /* cache full */
    memset(&c, 0, sizeof c);
    c.segment_id = seg; dart_shm_seg_name(c.name, seg);     /* attach maps whole + reads geometry */
    mem = n->shm_reader_pool_mem + (size_t)slot*state_bytes;
    if (!dart_shm_attach(mem, &c)) return NULL;
    n->shm_reader_segments[slot] = seg;
    return (i_DartShmPool*)mem;
}
static int dart__node_on_shm(void *u, uint16_t ch, uint32_t from, const uint8_t *desc){
    DartNode *n = (DartNode*)u; i_DartShmDesc d; i_DartShmPool *reader_pool; const void *p; uint32_t len;
    if (!dart_shm_desc_decode(&d, desc, DART_SHM_DESC_WIRE)) return 0;
    reader_pool = dart__shm_reader_pool(n, d.segment_id);
    if (!reader_pool) return 0;                                     /* can't attach -> reader NACKs (repair) */
    p = dart_shm_read(reader_pool, &d, &len);                       /* seqlock head: generation == descriptor? */
    if (!p) return 0;                                      /* recycled -> NACK -> repair or skip */
    /* one-copy: copy out of shared memory so the user owns the bytes (ack-timing safe) */
    if (len > n->shm_scratch_cap){
        void *new_buf = dart__node_alloc(n, n->shm_scratch, len?len:1u);
        if (!new_buf) return 0;
        n->shm_scratch = new_buf; n->shm_scratch_cap = len;
    }
    memcpy(n->shm_scratch, p, len);
    if (!dart_shm_verify(reader_pool, &d)) return 0;                /* seqlock tail: writer recycled mid-copy -> torn -> drop */
    n->shm_rx++;
    dart__deliver(n, ch, from, n->shm_scratch, len);
    return 1;
}
#endif

/* effective node name into buf[DART_NODE_NAME_MAX+1]: the caller's (clamped), or an
 * auto-generated "node-XXXXXXXX" debug default (random suffix, pid fallback). Returns
 * its length. A node always has a name, so peers always have one to reference. */
static uint8_t dart__node_name(const char *want, char *buf){
    static const char hex[] = "0123456789abcdef";
    uint32_t r; size_t i;
    if (want && *want){
        for (i=0; i<DART_NODE_NAME_MAX && want[i]; i++) buf[i] = want[i];
        buf[i] = '\0';
        return (uint8_t)i;
    }
    if (!dart_plat_random(&r, sizeof r)) r = (uint32_t)dart_plat_pid();
    memcpy(buf, "node-", 5);
    for (i=0; i<8; i++) buf[5+i] = hex[(r >> ((7-i)*4)) & 0xF];
    buf[13] = '\0';
    return 13;
}

DartNode *dart_node_open(DartAllocator *mem, const char *name, DartMsgFn on_message, const DartNodeOpts *opts){
    DartNodeOpts o; DartDiscoveryNetConfig dc; DartConfig tc; i_DartNodeBlocks blocks;
    uint16_t max_peers, max_channels;
    uint8_t *base; void *arena; int owns; size_t need, arena_size, ctrl_end, ctrl_cap;
    DartNode *n; i_DartSock fd; uint16_t local_port;
    char node_name[DART_NODE_NAME_MAX + 1]; uint8_t node_name_len = 0;   /* name is discovery-level */

    if (!mem || mem->claimed) return NULL;             /* required, and one allocator per node */
    memset(&o, 0, sizeof o);
    if (opts) o = *opts;
    max_channels = o.max_channels ? o.max_channels : 8;
    max_peers    = o.discovery.max_peers ? o.discovery.max_peers : 16;

    /* sub-configs (sizing only depends on counts; the callback wrappers are set later) */
    memset(&dc,0,sizeof dc); memset(&tc,0,sizeof tc);
    dc.discovery.domain_id   = o.domain;
    dc.discovery.data_port   = o.net.data_port;
    dc.discovery.announce_interval_us = o.discovery.announce_interval_us;
    dc.discovery.peer_timeout_us = o.discovery.peer_timeout_us;
    dc.discovery.max_peers   = max_peers;
    dc.discovery.meta_capacity = dart_meta_capacity(max_channels);
    dc.discovery.peer_user_bytes = dart_node_core_peer_user_bytes();   /* node-core lifecycle state per peer */
    dc.group                 = o.net.discovery_group;
    dc.discovery_port        = o.net.discovery_port;
    dc.ttl                   = o.net.multicast_ttl;
    dc.multicast_interface   = o.net.multicast_interface;
    dc.seeds                 = o.net.seed_peers;
    dc.n_seeds               = o.net.n_seed_peers;
    tc.channels    = NULL;            /* reserve mode: channels created at runtime */
    tc.n_channels  = max_channels;
    tc.max_peers   = max_peers;
    tc.frag_payload= o.net.fragment_size;
    tc.allocator   = dart__node_alloc;   /* non-NULL => reserve/dynamic mode in dart_init */

    {   i_DartBump b; memset(&b,0,sizeof b);
        dart__node_layout(&b, max_peers, max_channels, &tc, &dc, &blocks);
        need = b.offset + 32u; }

    /* The node struct lives OUTSIDE the "layers" arena (node_core+transport+discovery+
       handle-pointer-array), because the user holds DartNode* across a grow that relocates
       the arena. In static mode the struct sits at the buffer front and the layers arena
       follows; in dynamic mode each is its own heap block and the arena can be grown. */
    if (mem->dynamic){
        n = (DartNode*)dart_plat_realloc(NULL, sizeof *n);
        if (!n) return NULL;
        arena_size = mem->size > need ? mem->size : need;   /* hint floors at the layout need */
        arena = dart_plat_realloc(NULL, arena_size); owns = 1;
        if (!arena){ dart_plat_realloc(n, 0); return NULL; }
    } else {
        size_t nsz = (sizeof(struct DartNode) + 15u) & ~(size_t)15u;
        uint8_t *bb = (uint8_t*)(((uintptr_t)mem->buffer + 15u) & ~(uintptr_t)15u);
        size_t head = (size_t)(bb - (uint8_t*)mem->buffer) + nsz;
        if (!mem->buffer || mem->size < head + need) return NULL;
        n = (DartNode*)bb; arena = bb + nsz; arena_size = mem->size - head; owns = 0;
    }
    base = (uint8_t*)(((uintptr_t)arena+15u)&~(uintptr_t)15u);
    {   i_DartBump b; memset(&b,0,sizeof b);
        b.base = base; b.cap = arena_size - (size_t)(base - (uint8_t*)arena);
        dart__node_layout(&b, max_peers, max_channels, &tc, &dc, &blocks);
        ctrl_end = (b.offset + 15u) & ~(size_t)15u;   /* 16-aligned tail: static bump start */
        ctrl_cap = b.cap; }

    if (!dart_plat_startup()){
        if (owns){ dart_plat_realloc(arena, 0); dart_plat_realloc(n, 0); }
        return NULL;
    }

    memset(n, 0, sizeof *n);
    n->fd = DART_SOCK_BAD; n->multicast_fd = DART_SOCK_BAD;
    n->domain = o.domain;
    n->net = o.net;
    n->user_on_message = on_message; n->on_event = o.on_event;
    n->user_data = o.user_data;
    n->arena = arena; n->owns_arena = owns;
    n->alloc_dynamic = mem->dynamic;
    if (mem->dynamic){
        n->mem_cap = mem->max_bytes ? mem->max_bytes : DART_MEM_DEFAULT_MAX;
        n->static_pos = n->static_end = NULL;
    } else {
        n->static_pos = base + ctrl_end;     /* message buffers bump from the arena tail */
        n->static_end = base + ctrl_cap;
    }
    n->handles = (DartChannel**)blocks.handles;
    memset(n->handles, 0, (size_t)max_channels * sizeof(DartChannel*));
    n->joined_groups = (uint32_t*)blocks.joined_groups;
    n->max_channels = max_channels;
    n->max_peers = max_peers;
    n->multicast_port = o.net.multicast_port ? o.net.multicast_port
               : (uint16_t)((o.net.discovery_port ? o.net.discovery_port : 7400) + 1);

    tc.on_message = dart__node_on_message;     /* wrap so on_message receives a DartMsg */
    tc.on_event   = dart__node_on_transport_event;  /* map DartTransportEvent -> app DartEvent */
    tc.user       = n;
#ifdef DART_SHM
    n->shm_capable = (uint8_t)(mem->dynamic && !o.disable_shm);   /* static mode never uses SHM */
    if (n->shm_capable){
        dart_plat_host_uuid(n->shm_host);
        if (!dart_plat_random(&n->shm_base, sizeof n->shm_base)) n->shm_base = dart_plat_pid();
        n->shm_base ^= (uint64_t)dart_plat_pid() << 32;    /* fold in pid for cross-process uniqueness */
        n->shm_base &= ~(((uint64_t)1u << 19) - 1u);       /* low 19 bits: 3 class + 16 channel index */
        if (n->shm_base == 0) n->shm_base = (uint64_t)1u << 19;
    }
    tc.on_shm = dart__node_on_shm;
#endif

    n->transport = dart_init(blocks.transport, blocks.transport_bytes, &tc);
    if (!n->transport) goto fail_startup;
#ifdef DART_SHM
    {   uint32_t n_segments = (uint32_t)max_channels * DART_SHM_N_CLASSES;
        uint16_t reader_max = dart__node_shm_reader_max(max_peers, max_channels); uint32_t i;
        n->shm_n_channels = max_channels; n->shm_reader_max = reader_max;
        n->shm_pool            = (void**)blocks.shm_pool;
        n->shm_pool_mem        = blocks.shm_pool_mem;
        n->shm_reader_segments = (uint64_t*)blocks.shm_reader_segments;
        n->shm_reader_pool_mem = blocks.shm_reader_pool;
        for (i=0;i<n_segments;i++) n->shm_pool[i]=NULL;
        for (i=0;i<reader_max;i++) n->shm_reader_segments[i]=0;
    }
#endif

    /* sans-IO node core: drives the discovery->transport lifecycle and resolves addresses
       over the discovery core's peer table (bound below, once discovery exists). */
    node_name_len = dart__node_name(name, node_name);   /* handed to discovery (the name's owner) below */
    {   i_DartNodeCoreConfig cc;
        memset(&cc, 0, sizeof cc);
        cc.transport = n->transport;   /* cc.discovery bound after dart_discovery_place */
        cc.n_channels = max_channels; cc.frag_size = dart_clamp_frag(o.net.fragment_size);
        cc.on_event = dart__node_on_event; cc.user = n;
        cc.is_local = dart__node_is_local; cc.is_local_user = NULL;
#ifdef DART_SHM
        cc.oob_capable = n->shm_capable; memcpy(cc.oob_host, n->shm_host, 16);
#endif
        n->core = dart_node_core_init(blocks.node_core, blocks.node_core_bytes, &cc);
        if (!n->core) goto fail_startup;
    }

    /* Bind the data socket before opening discovery so we can advertise its real
       port (0 => OS ephemeral, read back via getsockname). No reuse: a unicast
       endpoint owns its port, so a collision fails loudly here. */
    fd = dart_plat_udp_open();
    if (fd==DART_SOCK_BAD) goto fail_startup;
    n->fd=fd;                       /* owned now: fail_sock closes it */
    if (!dart_plat_bind(fd, 0, o.net.data_port, 0)) goto fail_sock;
    local_port = dart_plat_local_port(fd);
    if (local_port==0) goto fail_sock;
    dart_plat_set_nonblock(fd);   /* never block in recv/send; poll drains the queue */
    dart_plat_suppress_connreset(fd);  /* suppress WSAECONNRESET from a bounced send */
    if (o.net.recv_buffer_bytes) dart_plat_set_rcvbuf(fd, (int)o.net.recv_buffer_bytes);
    if (o.net.send_buffer_bytes) dart_plat_set_sndbuf(fd, (int)o.net.send_buffer_bytes);
    dc.discovery.data_port = local_port;       /* advertise the actual port */

    /* multicast sockets/joins are set up lazily by dart_node_create_channel, since no
       channel exists yet at open. */

    dc.discovery.on_event = dart_node_core_on_disc_event;   /* node core demuxes PEER_UP/DOWN/REFUSED */
    dc.discovery.user     = n->core;
    dc.discovery.name     = node_name;        /* name is discovery-owned (its own blob section) */
    dc.discovery.name_len = node_name_len;
    /* the core builds our OVERLAY (frag size + OOB host + interest); discovery wraps it in
       its blob (after the locator + name) so peers reassemble and match from discovery */
    dart_node_core_build_meta(n->core);
    dc.discovery.meta = dart_node_core_meta(n->core, &dc.discovery.meta_len);
    n->discovery = dart_discovery_place(blocks.discovery, blocks.discovery_bytes, &dc);
    if (!n->discovery) goto fail_mcast;
    /* the node core delegates id<->address resolution + per-peer scratch to discovery's table */
    dart_node_core_bind_discovery(n->core, dart_discovery_state(n->discovery));

    mem->claimed = 1;          /* taken over; the allocator can't back a second node */
    return n;

fail_mcast:
    if (n->multicast_fd != DART_SOCK_BAD) dart_plat_close(n->multicast_fd);
    n->multicast_fd = DART_SOCK_BAD;
fail_sock:
    if (n->fd != DART_SOCK_BAD) dart_plat_close(n->fd);
    n->fd = DART_SOCK_BAD;
fail_startup:
    dart_plat_cleanup();
    if (owns){ dart_plat_realloc(arena, 0); dart_plat_realloc(n, 0); }
    return NULL;
}

/* Dynamic-mode growth: relocate the whole node into a bigger arena at the given counts so
 * a full peer table or channel reserve stops being a hard cap. Heap message buffers and SHM
 * writer segments stay put (only their owning control structures move); the user-held
 * DartNode and DartChannel handles live outside the arena, so they survive. Returns 1 with n
 * now on the new arena, or 0 if the bigger arena couldn't be allocated (n left unchanged). */
static int dart__node_grow(DartNode *n, uint16_t new_max_peers, uint16_t new_max_channels){
    DartConfig tc; DartDiscoveryNetConfig dc; i_DartNodeBlocks nb; i_DartBump b;
    DartTransportState *nt; i_DartNodeCore *ncore; DartDiscovery *ndisc;
    void *new_arena, *old_arena = n->arena;
    uint8_t *nbase; size_t need;
    uint16_t old_max_channels = n->max_channels, new_meta_cap = dart_meta_capacity(new_max_channels);

    if (!n->alloc_dynamic) return 0;
    if (new_max_peers <= n->max_peers && new_max_channels <= n->max_channels) return 0;

    memset(&tc,0,sizeof tc); memset(&dc,0,sizeof dc);
    tc.channels=NULL; tc.n_channels=new_max_channels; tc.max_peers=new_max_peers;
    tc.allocator=dart__node_alloc; tc.frag_payload=n->net.fragment_size;
    dc.discovery.max_peers=new_max_peers; dc.discovery.meta_capacity=new_meta_cap;
    dc.discovery.peer_user_bytes = dart_node_core_peer_user_bytes();   /* size discovery's scratch to match */

    memset(&b,0,sizeof b);
    dart__node_layout(&b, new_max_peers, new_max_channels, &tc, &dc, &nb);
    need = b.offset + 32u;
    new_arena = dart_plat_realloc(NULL, need);
    if (!new_arena) return 0;
    nbase = (uint8_t*)(((uintptr_t)new_arena+15u)&~(uintptr_t)15u);
    memset(&b,0,sizeof b); b.base=nbase; b.cap=need-(size_t)(nbase-(uint8_t*)new_arena);
    dart__node_layout(&b, new_max_peers, new_max_channels, &tc, &dc, &nb);

    /* migrate the three cores; each leaves the old intact, so a failure just frees the new
       arena and bails (the old node keeps running, only refusing the would-be growth) */
    nt = dart_migrate(n->transport, nb.transport, nb.transport_bytes, new_max_peers, new_max_channels);
    if (!nt){ dart_plat_realloc(new_arena,0); return 0; }
    ncore = dart_node_core_migrate(n->core, nb.node_core, nb.node_core_bytes, new_max_channels);
    if (!ncore){ dart_plat_realloc(new_arena,0); return 0; }
    ncore->transport = nt;                         /* re-point cross-layer pointer */
    dart_node_core_build_meta(ncore);              /* rebuild the announce blob into the new buf */
    ndisc = dart_discovery_migrate(n->discovery, nb.discovery, nb.discovery_bytes,
                                      new_max_peers, new_meta_cap, dart_node_core_meta(ncore,NULL), ncore);
    if (!ndisc){ dart_plat_realloc(new_arena,0); return 0; }
    dart_node_core_bind_discovery(ncore, dart_discovery_state(ndisc));   /* re-point to the relocated table */

    /* handle pointer array + joined-group table (handle structs are stable, not moved) */
    memcpy(nb.handles, n->handles, (size_t)old_max_channels*sizeof(DartChannel*));
    memset((DartChannel**)nb.handles + old_max_channels, 0,
           (size_t)(new_max_channels-old_max_channels)*sizeof(DartChannel*));
    memcpy(nb.joined_groups, n->joined_groups, (size_t)old_max_channels*sizeof(uint32_t));

#ifdef DART_SHM
    if (n->shm_capable){
        size_t sb = dart_shm_state_bytes();
        uint32_t old_segs=(uint32_t)n->shm_n_channels*DART_SHM_N_CLASSES;
        uint32_t new_segs=(uint32_t)new_max_channels*DART_SHM_N_CLASSES, i;
        uint16_t new_reader_max = dart__node_shm_reader_max(new_max_peers, new_max_channels);
        void **np = (void**)nb.shm_pool;
        for (i=0;i<new_segs;i++) np[i]=NULL;
        for (i=0;i<old_segs;i++)        /* writer pool: keep segments mapped, relocate the state */
            if (n->shm_pool[i]){
                memcpy(nb.shm_pool_mem + (size_t)i*sb, n->shm_pool[i], sb);
                np[i] = nb.shm_pool_mem + (size_t)i*sb;
            }
        for (i=0;i<n->shm_reader_max;i++)  /* reader pool: detach + reset (re-attaches lazily) */
            if (n->shm_reader_segments[i]) dart_shm_detach((i_DartShmPool*)(n->shm_reader_pool_mem+(size_t)i*sb));
        n->shm_pool=np; n->shm_pool_mem=nb.shm_pool_mem;
        n->shm_reader_segments=(uint64_t*)nb.shm_reader_segments;
        n->shm_reader_pool_mem=nb.shm_reader_pool;
        for (i=0;i<new_reader_max;i++) n->shm_reader_segments[i]=0;
        n->shm_reader_max=new_reader_max; n->shm_n_channels=new_max_channels;
    }
#endif

    n->transport=nt; n->core=ncore; n->discovery=ndisc;
    n->handles=(DartChannel**)nb.handles; n->joined_groups=(uint32_t*)nb.joined_groups;
    n->max_channels=new_max_channels; n->max_peers=new_max_peers;
    dart_plat_realloc(old_arena,0);    /* control structs only; heap bufs + segments moved by ref */
    n->arena=new_arena;
    return 1;
}

DartChannel *dart_node_create_channel(DartNode *n, const char *name, DartRole role,
                                      const DartChannelOpts *opts){
    DartChannelDef def; DartChannel *h; uint16_t idx, mlen;
    if (!n || !name) return NULL;
    if (n->n_created >= n->max_channels){       /* reserve full: grow (dynamic) or refuse (static) */
        uint16_t want = n->max_channels < 0x8000u ? (uint16_t)(n->max_channels*2u) : 0xFFFFu;
        if (want <= n->max_channels || !dart__node_grow(n, n->max_peers, want)) return NULL;
    }
    idx = n->n_created;
    h = (DartChannel*)dart__node_alloc(n, NULL, sizeof *h);   /* stable: outlives any arena grow */
    if (!h) return NULL;
    memset(&def, 0, sizeof def);
    def.name = name; def.role = (uint8_t)role;
    if (opts){ def.qos = opts->qos; def.multicast = opts->multicast; }
    if (dart_channel_define(n->transport, idx, &def) != 0){ dart__node_alloc(n, h, 0); return NULL; }
    if (def.multicast) dart__node_channel_mcast(n, idx, &def);
    /* re-advertise our interest so peers match the new channel as the blob arrives, and
       replay known peers' interest so this channel matches what they already advertised */
    mlen = dart_node_core_build_meta(n->core);
    dart_discovery_advertise(n->discovery, dart_node_core_meta(n->core, NULL), mlen);
    dart_discovery_replay(n->discovery);
    h->n = n; h->index = idx; n->handles[idx] = h;
    n->n_created++;
    return h;
}

/* max wall-time draining RX (and running on_message) per poll tick before
 * yielding to discovery, so a slow on_message never starves it */
#ifndef DART_RX_BUDGET_US
#define DART_RX_BUDGET_US 5000u
#endif

/* drain one socket's RX queue into the transport until empty or past deadline
 * (full drain avoids NACK storms). Distinct from public dart_channel_drain. */
static void dart__node_rx_drain(DartNode *n, i_DartSock fd, uint64_t deadline){
    uint8_t buf[DART_DGRAM_MAX];
    for (;;){
        uint8_t src_ip[4]; uint16_t src_port;
        int r = dart_plat_recv(fd, buf, sizeof buf, src_ip, &src_port);
        if (r<0){
            if (dart_plat_would_block()) break;        /* queue empty */
            continue;   /* per-datagram error (e.g. bounced send); keep draining */
        }
        if (r>0){
            if (r>=4 && buf[0]=='u' && buf[1]=='D' && buf[2]=='S' && buf[3]=='C'){
                /* unicast announce aimed at our data port: hand it to discovery */
                dart_discovery_feed(n->discovery, src_ip, 4, buf, (size_t)r);
            } else {
                uint32_t from;
                if (dart_node_core_id_for_addr(n->core, src_ip, src_port, &from))
                    dart_on_datagram(n->transport, from, buf, (size_t)r, dart_plat_now_us());
            }
        }
        if (dart_plat_now_us() >= deadline) break;      /* yield to discovery/send */
    }
}

int dart_node_poll(DartNode *n, int timeout_ms){
    uint8_t buf[DART_DGRAM_MAX]; uint32_t to; size_t out_len; uint64_t now;
    i_DartPollfd pfd[2]; int n_fds=1;

    /* a peer was refused last tick for lack of slots: grow the table now, between ticks
       (safe: not inside any layer's processing), then the peer's next announce is admitted */
    if (n->grow_pending){
        uint16_t want = n->max_peers < 0x8000u ? (uint16_t)(n->max_peers*2u) : 0xFFFFu;
        n->grow_pending = 0;
        if (want > n->max_peers) dart__node_grow(n, want, n->max_channels);
    }

    dart_discovery_poll(n->discovery, 0);                 /* discovery tick (non-blocking) */

    memset(pfd,0,sizeof pfd);
    pfd[0].fd=n->fd; pfd[0].events=DART_POLLIN;
    if (n->multicast_fd!=DART_SOCK_BAD){ pfd[1].fd=n->multicast_fd; pfd[1].events=DART_POLLIN; n_fds=2; }
    /* cap the wait at the next internal timer so a due ack/NACK/heartbeat fires on
       time, not after the full quantum (no traffic to wake us when a writer stalls) */
    { uint64_t next_deadline = dart_next_deadline_us(n->transport);
      if (next_deadline){ uint64_t t0 = dart_plat_now_us();
               uint64_t us = (next_deadline > t0) ? next_deadline - t0 : 0;          /* until the timer */
               int ms = (us >= (uint64_t)timeout_ms*1000u) ? timeout_ms
                                                           : (int)((us + 999u)/1000u);
               if (ms < timeout_ms) timeout_ms = ms; } }       /* round up: no busy-spin */
    if (dart_plat_poll(pfd,n_fds,timeout_ms) > 0){
        uint64_t rx_deadline = dart_plat_now_us() + DART_RX_BUDGET_US;
        if (pfd[0].revents & DART_POLLIN) dart__node_rx_drain(n, n->fd, rx_deadline);
        if (n_fds==2 && (pfd[1].revents & DART_POLLIN)) dart__node_rx_drain(n, n->multicast_fd, rx_deadline);
    }

    now=dart_plat_now_us();
    /* the core already consumed any held datagram, so retry it before pulling new */
    if (n->tx_hold_len && dart__node_tx(n, n->tx_hold_peer, n->tx_hold, n->tx_hold_len))
        n->tx_hold_len = 0;
    if (!n->tx_hold_len)
        while (dart_poll_send(n->transport,&to,buf,sizeof buf,&out_len,now)){
            if (!dart__node_tx(n, to, buf, out_len)){
                memcpy(n->tx_hold, buf, out_len);
                n->tx_hold_len = out_len; n->tx_hold_peer = to;
                break;          /* TX buffer full: yield this tick */
            }
            now=dart_plat_now_us();
        }
    return 0;
}

/* publish on a channel index: bounded backpressure pump, then SHM fast path, then UDP */
static int dart__node_do_send(DartNode *n, uint16_t channel, const void *data, size_t len){
    /* bounded backpressure: pump the loop (on_message/on_event may fire here) until
       a slow reader acks or qos.backpressure_wait_us elapses, then send anyway */
    const DartQos *q = dart_channel_qos(n->transport, channel);
    if (q && q->backpressure_wait_us && dart_send_would_evict(n->transport, channel)){
        uint64_t t0 = dart_plat_now_us(), deadline = t0 + q->backpressure_wait_us;
        /* in-pump diagnostic: the publisher is blocked here for the whole wait, so its
           normal per-message print sees nothing within it. When a probe is set, sample
           the writer's repair progress on a ~interval timer so the stall is visible as a
           within-block time series (resends bursty-then-flat vs steady; writer idle for
           lack of NACKs). Observational; when no probe is set this whole block is skipped. */
        uint64_t sample_last = t0; uint32_t sample_polls = 0, sample_idle = 0;
        uint64_t interval = n->pump_probe_interval_us ? n->pump_probe_interval_us : 200000u;
        DartRepairStats sample_prev;
        if (n->pump_probe) dart_repair_stats(n->transport, channel, &sample_prev);
        do {
            dart_node_poll(n, 1);
            if (n->pump_probe){
                uint64_t now = dart_plat_now_us();
                sample_polls++;
                if (dart_repair_pending(n->transport, channel) == 0) sample_idle++;
                if (now - sample_last >= interval){
                    DartRepairStats sample_now; DartPumpSample sample;
                    dart_repair_stats(n->transport, channel, &sample_now);
                    sample.channel         = channel;
                    sample.wait_elapsed_us = now - t0;
                    sample.interval_us     = now - sample_last;
                    sample.frags_resent    = sample_now.frags_resent - sample_prev.frags_resent;
                    sample.nacks_recv      = sample_now.nacks_recv  - sample_prev.nacks_recv;
                    sample.polls           = sample_polls;
                    sample.polls_idle      = sample_idle;
                    n->pump_probe(n->pump_probe_user, &sample);
                    sample_prev = sample_now; sample_last = now; sample_polls = 0; sample_idle = 0;
                }
            }
            if (!dart_send_would_evict(n->transport, channel)) break;
        } while (dart_plat_now_us() < deadline);
        n->backpressure_total_us += dart_plat_now_us() - t0;
        n->backpressure_wait_count++;
    }
#ifdef DART_SHM
    if (n->shm_capable && len>0 && channel < n->shm_n_channels && dart_writer_shm_eligible(n->transport, channel)){
        uint16_t keep_last = (q && q->keep_last) ? q->keep_last : 1u;
        /* a hint (shm_max_bytes / max_message_bytes) pins the channel to one class, so
           same-sized traffic reuses a single prefix-sized segment; without it each message
           uses its own size class's segment, created on demand. Per channel either way. */
        uint32_t hint = q ? (q->shm_max_bytes ? q->shm_max_bytes : q->max_message_bytes) : 0u;
        uint32_t k = dart_shm_class_for(hint ? hint : (uint32_t)len);
        /* fits its class (with a hint, the hinted class) -> publish via SHM; chunk index =
           the history slot this send will occupy, so chunk i binds slot i (no free list) */
        if (k < DART_SHM_N_CLASSES && (uint32_t)len <= dart_shm_class_bytes(k)){
            i_DartShmPool *pool = dart__shm_chan_pool(n, channel, k, keep_last);
            uint16_t slot = dart_channel_hist_head(n->transport, channel);
            void *chunk_ptr = pool ? dart_shm_chunk(pool, slot, NULL) : NULL;
            if (chunk_ptr){
                i_DartShmDesc d; uint8_t desc[DART_SHM_DESC_WIRE];
                memcpy(chunk_ptr, data, len);                       /* one-copy write into shm */
                dart_shm_stamp(pool, slot, (uint32_t)len, &d);
                dart_shm_desc_encode(&d, desc);
                if (dart_send_shm(n->transport, channel, chunk_ptr, len, desc, dart_plat_now_us())==0){
                    n->shm_tx++;
                    return 0;
                }
            }
        }
    }
#endif
    return dart_send(n->transport, channel, data, len, dart_plat_now_us());
}

int dart_channel_send(DartChannel *ch, const void *data, size_t len){
    if (!ch) return DART_ERR_NO_CHANNEL;
    return dart__node_do_send(ch->n, ch->index, data, len);
}

int dart_channel_set_role(DartChannel *ch, DartRole role){
    int r; uint16_t mlen;
    if (!ch) return -1;
    r = dart_set_role(ch->n->transport, ch->index, (uint8_t)role);
    if (r == 0){   /* re-advertise our interest: peers rematch as the new blob arrives */
        mlen = dart_node_core_build_meta(ch->n->core);
        dart_discovery_advertise(ch->n->discovery, dart_node_core_meta(ch->n->core, NULL), mlen);
        dart_discovery_replay(ch->n->discovery);   /* re-apply peers' interest to our new role */
    }
    return r;
}

uint16_t dart_channel_index(const DartChannel *ch){ return ch ? ch->index : 0; }

DartChannel *dart_node_channel(DartNode *n, uint16_t index){
    if (!n || index >= n->n_created) return NULL;
    return n->handles[index];
}

void dart_node_backpressure_stats(DartNode *n, uint64_t *waited_us, uint32_t *waited_sends){
    if (waited_us)    *waited_us    = n->backpressure_total_us;
    if (waited_sends) *waited_sends = n->backpressure_wait_count;
}

void dart_node_mem_stats(DartNode *n, size_t *in_use, size_t *peak, uint64_t *alloc_calls){
    if (!n) return;
    if (in_use)      *in_use      = n->mem_used;        /* live message-buffer bytes (dynamic) */
    if (peak)        *peak        = n->mem_peak;        /* high-water of the above */
    if (alloc_calls) *alloc_calls = n->mem_alloc_calls; /* (re)allocations so far; flat in steady state */
}

void dart_channel_repair_stats(DartChannel *ch, DartRepairStats *out){
    if (ch) dart_repair_stats(ch->n->transport, ch->index, out);
    else if (out) memset(out, 0, sizeof *out);
}

void dart_node_set_pump_probe(DartNode *n, DartPumpProbeFn fn, uint64_t interval_us, void *user){
    n->pump_probe = fn; n->pump_probe_interval_us = interval_us; n->pump_probe_user = user;
}

int dart_channel_reader_progress(DartChannel *ch, uint32_t peer,
                                 uint64_t *base_seqno, uint32_t *have, uint32_t *total){
    return ch ? dart_reader_progress(ch->n->transport, ch->index, peer, base_seqno, have, total) : 0;
}

#ifdef DART_SHM
void dart_node_shm_stats(DartNode *n, uint32_t *sent, uint32_t *recv){
    if (sent) *sent = n->shm_tx;
    if (recv) *recv = n->shm_rx;
}
#endif

int dart_channel_drain(DartChannel *ch, int timeout_ms){
    DartNode *n; uint64_t deadline;
    if (!ch) return 0;
    n = ch->n;
    deadline = dart_plat_now_us() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000u;
    while (!dart_send_drained(n->transport, ch->index)){
        if (dart_plat_now_us() >= deadline) return 0;
        dart_node_poll(n, 1);
    }
    return 1;
}

int dart_channel_match_count(DartChannel *ch){
    return ch ? dart_writer_match_count(ch->n->transport, ch->index) : 0;
}

void dart_node_close(DartNode *n, int send_bye){
    void *arena; int owns;
    if (!n) return;
    arena = n->arena; owns = n->owns_arena;
    if (n->discovery) dart_discovery_close(n->discovery, send_bye);
    if (n->multicast_fd != DART_SOCK_BAD) dart_plat_close(n->multicast_fd);
    if (n->fd != DART_SOCK_BAD) dart_plat_close(n->fd);
    if (n->transport) dart_destroy(n->transport);     /* free hook-allocated dynamic buffers + rings */
#ifdef DART_SHM
    if (n->shm_capable){
        size_t state_bytes = dart_shm_state_bytes(); uint32_t i, n_segments = (uint32_t)n->shm_n_channels * DART_SHM_N_CLASSES;
        for (i=0;i<n_segments;i++)
            if (n->shm_pool[i]) dart_shm_detach((i_DartShmPool*)n->shm_pool[i]);   /* unlinks ours */
        for (i=0;i<n->shm_reader_max;i++)
            if (n->shm_reader_segments[i]) dart_shm_detach((i_DartShmPool*)(n->shm_reader_pool_mem + (size_t)i*state_bytes));
        if (n->shm_scratch) dart__node_alloc(n, n->shm_scratch, 0);
    }
#endif
    {   uint16_t i; for (i=0;i<n->n_created;i++) if (n->handles[i]) dart__node_alloc(n, n->handles[i], 0); }
    dart_plat_cleanup();
    if (owns){ dart_plat_realloc(arena, 0); dart_plat_realloc(n, 0); }   /* touch nothing after */
}
#pragma endregion
#endif /* !DART_TRANSPORT_SANS_IO */
#endif /* DART_TRANSPORT_IMPLEMENTATION */
