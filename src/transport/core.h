/* sans-IO reliable-UDP transport core: no socket, clock, or heap. Feed it
 * datagrams + now_us + a peer set; it returns datagrams to send and delivers
 * reassembled messages. RTPS-inspired, not wire-compatible. Layer DartNode.h
 * on top for a socket-owning node. */
#ifndef DART_TRANSPORT_H
#define DART_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>
#include "../common/string.h"   /* DartBytes (payloads, wire blobs), DartString (wire names) */
#include "../common/alloc.h"    /* DartAllocFn (the growable-message hook) */

#ifdef __cplusplus
extern "C" {
#endif

/* The zero-fragment same-host shared-memory path: DART_SHM is AUTO-DETECTED on where
 * the platform layer provides it (Windows file mappings; shm_open/mmap on Linux/
 * macOS/BSD, -lrt on older glibc), off elsewhere, and DART_NO_SHM always wins (strip
 * it explicitly, e.g. for a slimmer build). A new platform layer that implements the
 * i_dart_plat_shm_* contract declares support by defining DART_SHM itself. It is used
 * only between same-host nodes that set an allocator; a node with no local SHM peer
 * creates no segment and pays nothing at runtime. This block mirrors platform/core.h
 * EXACTLY so every TU agrees whichever header it saw first. */
#if !defined(DART_SHM) && !defined(DART_NO_SHM)
  #if defined(_WIN32) || defined(__linux__) || defined(__APPLE__) || \
      defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
      defined(__DragonFly__)
    #define DART_SHM
  #endif
#endif
#if defined(DART_SHM) && defined(DART_NO_SHM)
  #undef DART_SHM              /* both set: the opt-out wins */
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
/* DART_INACTIVE = declared but off (resources stay allocated; dart_transport_set_role flips it) */
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
 * handle for dart_transport_send / on_message is the channel's index in channels[]. */
typedef struct {
    const char *name;  /* topic name = cross-peer identity. Required, same on every node, <= DART_TOPIC_NAME_MAX */
    DartQos   qos;
    uint8_t  role;     /* DartRole; 0 = pub+sub */
} DartChannelDef;

/* dart_transport_poll_send destination: a peer id. Data is unicast point-to-point per matched reader. */

/* A complete message; channel is the local handle. Do not call back into dart_*. */
typedef void (*DartMessageFn)(void *user, uint16_t channel, uint32_t from_peer, DartBytes data);

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
    DART_TRANSPORT_QOS_INCOMPATIBLE,/* a reliable subscriber refused a best-effort publisher (.channel, .peer); .detail = our channel name */
    DART_TRANSPORT_SCHEMA_MISMATCH, /* the schema_check hook refused a match (.channel, .peer); .detail = our channel name */
    DART_TRANSPORT_INTEREST_OVERFLOW,/* a peer's matched topics carry aliases beyond our alias table
                                        (.peer, .lost_count = entry count): their data can never demux
                                        here. Raise DART_META_MAX_IDS. */
    DART_TRANSPORT_META_TRUNCATED   /* our own announce overlay overflowed its buffer: a section was
                                       dropped (.detail names it), so peers see partial interest or
                                       schemas. Fewer channels, or shorter names/schemas. */
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

/* DartConfig.allocator is a DartAllocFn (common/alloc.h): set it and user channels grow to
 * fit (max_message_bytes may be 0); NULL (default, embedded) keeps fixed buffers and a
 * bigger message is refused/skipped. Pair a set allocator with dart_transport_destroy to free it. */

/* Two ways to populate the channel table:
 *   fixed/at-init : channels != NULL, n_channels = its length. Slots are defined now;
 *                   buffers come from the arena (or the allocator if one is set).
 *   reserve/lazy  : channels == NULL, n_channels = the reserved capacity, allocator set.
 *                   All slots start DART_INACTIVE; fill them later with dart_transport_channel_define
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
    /* optional schema gate: called while a peer's interest is applied, once per would-be
     * match, with the local channel and the peer's advertised alias (peer_is_pub: their
     * entry is a publish). Return 1 to allow, 0 to refuse (no proxy either way, and the
     * transport fires SCHEMA_MISMATCH). The transport knows nothing of schema contents;
     * the node implements this over the serialize layer with the overlay it is applying. */
    int                 (*schema_check)(void *user, uint16_t channel, uint16_t peer_alias,
                                        int peer_is_pub);
    void                 *user;
} DartConfig;

typedef struct DartTransportState DartTransportState;

size_t    dart_transport_required_memory(const DartConfig *cfg);
DartTransportState *dart_transport_init(void *mem, size_t mem_size, const DartConfig *cfg);
/* Relocate a live transport into new_mem (>= dart_transport_required_memory at the grown counts),
 * re-striding its tables to new_max_peers/new_n_channels and carrying live reliability
 * state (positions, history, in-flight repair) across. Heap buffers are not in the arena,
 * so the caller frees old's arena block afterward but must NOT dart_transport_destroy old. Returns
 * the new state, or NULL on failure (old is left intact). Dynamic-mode growth only. */
DartTransportState *dart_transport_migrate(DartTransportState *old, void *new_mem, size_t new_cap,
                        uint16_t new_max_peers, uint16_t new_n_channels);
/* Free allocator-allocated buffers (dynamic channels). No-op in fixed mode; the
 * arena stays the caller's. The node calls it from close. */
void      dart_transport_destroy(DartTransportState *st);

/* 64-bit topic identity from a name (FNV-1a): matches topics across peers. */
uint64_t  dart_topic_id(const char *name);
uint64_t  dart_channel_identity(const DartChannelDef *def);   /* = dart_topic_id(def->name) */

/* Normalize a UDP fragment size: 0 -> DART_FRAG_PAYLOAD, then clamp to [MIN,MAX].
 * The rule dart_transport_init and the node's announce blob both apply (single source). */
uint16_t  dart_clamp_frag(uint16_t frag_payload);

/* A new peer matches nothing until dart_transport_apply_peer_interest feeds its interest
 * list (carried in its discovery announce). peer_frag: that peer's advertised UDP
 * fragment size (from discovery), used to reassemble its messages; 0 = DART_FRAG_PAYLOAD.
 * Clamped to [MIN, MAX]. */
void      dart_transport_peer_add   (DartTransportState *st, uint32_t peer_id, uint16_t peer_frag);
void      dart_transport_peer_remove(DartTransportState *st, uint32_t peer_id);

/* Discovery-blip lifecycle: a peer that fell silent (discovery timeout) is made
 * DORMANT instead of removed, so its reader position survives and a same-incarnation
 * return resumes losslessly. Dormant peers are dropped from flow control (the writer
 * stops heartbeating/draining them so a dead reader can't stall it; the reader stops
 * acking them), but their proxies and deliver position are preserved. dart_transport_peer_resume
 * re-includes the peer and re-reports reader positions so the writer fills any gap.
 * Both no-op for an unknown peer; the node drives them off discovery DROP/return. */
void      dart_transport_peer_dormant(DartTransportState *st, uint32_t peer_id);
void      dart_transport_peer_resume (DartTransportState *st, uint32_t peer_id);
/* Update a peer's advertised UDP fragment size (its announce blob may arrive after
 * first contact). Clamped to [MIN, MAX]; no-op for an unknown peer. */
void      dart_transport_peer_set_frag(DartTransportState *st, uint32_t peer_id, uint16_t peer_frag);

/* Interest exchange (the node carries these in discovery announces; sans-IO callers
 * disseminate them however they like). dart_transport_build_interest serializes OUR pub/sub
 * set into out ([u16 npub][u16 nsub] then [u16 alias][u8 namelen][name] entries),
 * returning bytes written or 0 if cap is too small; size out via dart_interest_max.
 * dart_transport_apply_peer_interest applies a peer's serialized set, (re)matching channels;
 * it is idempotent. Re-build + re-disseminate after dart_transport_set_role. */
size_t    dart_interest_max(uint16_t n_channels);
size_t    dart_transport_build_interest(DartTransportState *st, void *out, size_t cap);
void      dart_transport_apply_peer_interest(DartTransportState *st, uint32_t peer_id, DartBytes blob);

/* Discovery-announce meta blob (sans-IO codec). A versioned, opaque-to-discovery
 * payload wrapping this node's UDP fragment size, SHM capability + host uuid, its
 * interest list, and its published channels' schemas: the transport's OVERLAY, carried
 * opaquely inside discovery's announce blob (the node name lives in discovery's own
 * section, not here). Layout:
 *   v8: ['D','N',8, frag_lo, frag_hi,                <interest> <schemas>]
 *   v9: ['D','N',9, frag_lo, frag_hi, shm, host[16], <interest> <schemas>]
 * frag sits at [3..4] in both; v9 adds the SHM byte + host. dart_transport_meta_build writes v9 when
 * DART_SHM is compiled, v8 otherwise. The schema section follows the (self-delimiting)
 * interest list:
 *   [u16 n_map]  ( [u16 alias][u64 hash] )*          advertised alias -> schema identity
 *   [u16 n_wire] ( [u64 hash][u16 len][bytes] )*     distinct schema wires, inlined when small
 * The wire bytes are OPAQUE here (serialize/schema.h builds and parses them); the
 * transport just frames them, so it keeps no serialize dependency. */

/* Largest schema wire the overlay inlines (and reserves capacity for, per channel); a
 * bigger schema is advertised by hash alone. Define before the include to raise it. */
#ifndef DART_META_SCHEMA_INLINE_MAX
#define DART_META_SCHEMA_INLINE_MAX 512u
#endif

/* One channel's schema advertisement, registered by the node: the 64-bit identity plus a
 * view of the canonical wire bytes (valid for the channel's lifetime). hash 0 = none. */
typedef struct {
    uint64_t  hash;
    DartBytes wire;
} DartMetaSchema;

/* Bytes to reserve for the overlay: prefix + the largest interest list and schema section
 * n_channels can produce, capped to one (IP-fragmentable) UDP datagram. Sizes discovery's
 * meta_capacity. */
uint16_t  dart_meta_capacity(uint16_t n_channels);
/* Build the overlay into out[cap] (cap >= dart_meta_capacity): the version prefix (frag_size,
 * plus shm_capable + host[16] when DART_SHM is compiled), st's interest list, then the
 * schema section. schemas is one entry per channel (index = channel index) or NULL; every
 * non-INACTIVE channel with a nonzero hash is advertised (publishers offer their layout,
 * subscribers their required subset), each distinct wire once. Returns total bytes; host
 * may be NULL when !shm_capable. The node NAME is not here: it rides discovery's own
 * section of the announce blob. */
uint16_t  dart_transport_meta_build(DartTransportState *st, uint8_t *out, uint16_t cap,
                       uint16_t frag_size, int shm_capable, const uint8_t host[16],
                       const DartMetaSchema *schemas);
/* A peer's advertised UDP fragment size from the overlay; 0 if malformed. */
uint16_t  dart_meta_frag(DartBytes meta);
/* The interest sub-blob inside the overlay; {NULL, 0} if absent. */
DartBytes dart_meta_interest(DartBytes meta);

/* One advertised topic, as decoded by dart_meta_interest_next. name points into the
 * source overlay (NOT NUL-terminated), so keep that blob alive while reading it. */
typedef struct {
    uint16_t    alias;      /* the advertiser's local channel index (opaque to us) */
    uint8_t     reliable;   /* flags bit 0: offered (pub) / requested (sub) reliability */
    uint8_t     is_pub;     /* 1 = a publish entry, 0 = a subscribe entry */
    DartString  name;       /* topic name in the source blob (not NUL-terminated) */
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
 *   while (dart_meta_interest_next(meta, &it, &t)) { ... } */
int       dart_meta_interest_next(DartBytes meta, DartInterestIter *it, DartTopic *out);
/* An advertised alias's schema: returns 1 and fills *hash if the alias advertises one,
 * else 0. *wire is the inlined canonical bytes (a view into the overlay, parse with
 * dart_schema_parse), or {NULL,0} when the peer advertised the hash alone (schema too
 * big to inline). hash/wire may be NULL. */
int       dart_meta_schema(DartBytes meta, uint16_t alias, uint64_t *hash, DartBytes *wire);
#ifdef DART_SHM
/* A peer's SHM capability + host uuid (v3/v5 blobs only): 1 if SHM-capable (fills
 * host[16]), else 0. */
int       dart_meta_shm(DartBytes meta, uint8_t host[16]);
#endif

/* Change a channel's role at runtime (rematches peers locally; caller re-advertises
 * interest). A (re)subscribe joins like a late joiner. Returns 0 ok, <0 unknown. */
int       dart_transport_set_role(DartTransportState *st, uint16_t channel, uint8_t role);

/* Define a reserved (currently inactive) channel slot at runtime: set its name/qos/
 * role, allocate its history ring via the allocator, and rematch known peers.
 * Reserve mode only (an allocator is required). Returns 0 ok, or negative: -1 bad index/
 * name / slot already defined / no allocator, -4 out of memory. Re-advertise interest
 * after (the node bumps its discovery announce). */
int       dart_transport_channel_define(DartTransportState *st, uint16_t channel, const DartChannelDef *def);

/* The channel's topic name ({NULL,0} if undefined or out of range), for surfacing it on a
 * delivered message. Not NUL-terminated: use .data/.len. The name is a local lookup; it is
 * never on the data path. */
DartString dart_transport_channel_name(DartTransportState *st, uint16_t channel);

/* dart_transport_send / dart_transport_send_shm result: 0 ok, negative on error (returned as int). */
typedef enum {
    DART_OK             =  0,
    DART_ERR_NO_CHANNEL = -1,  /* channel index out of range */
    DART_ERR_TOO_BIG    = -2,  /* exceeds max_message_bytes or the wire fragment cap */
    DART_ERR_ROLE       = -3,  /* channel is SUB_ONLY or INACTIVE: cannot publish */
    DART_ERR_OOM        = -4,  /* dynamic allocator returned NULL */
    DART_ERR_STATE      = -5,  /* wrong state: poll while a service thread runs, start while
                                  started, or a call not allowed from inside a callback */
    DART_ERR_NOSYS      = -6   /* not compiled in (dart_node_start with DART_THREADS off) */
} DartResult;

/* Publish a message to all peers. Returns DART_OK, or a negative DartResult. */
int       dart_transport_send(DartTransportState *st, uint16_t channel, DartBytes data, uint64_t now_us);

#ifdef DART_SHM
/* Publish a message whose payload lives in an external shared-memory buffer: the
 * transport stores the sample referencing chunk (NOT copied) plus the descriptor,
 * fragments from chunk for non-SHM peers, and sends ONE SHM-DATA (the descriptor) to
 * SHM-capable peers. desc is DART_SHM_DESC_BYTES. Same return as dart_transport_send. The chunk
 * must stay valid until the sample leaves history (acked / evicted). */
int       dart_transport_send_shm(DartTransportState *st, uint16_t channel, DartBytes chunk,
                      const uint8_t *desc, uint64_t now_us);
/* Mark whether a peer can receive SHM-DATA (same host AND its segment is attached).
 * Off by default; the node sets it on attach, clears it on dormant/remove. */
void      dart_transport_peer_set_shm(DartTransportState *st, uint32_t peer_id, int is_shm);
/* 1 if every matched reader of channel is SHM-capable, so a publish may go via SHM
 * (else inline). The node checks this per message. */
int       dart_transport_writer_shm_eligible(DartTransportState *st, uint16_t channel);
/* The history slot the next publish to channel will occupy (binds chunk<->slot). */
uint16_t  dart_transport_channel_hist_head(DartTransportState *st, uint16_t channel);
#endif

/* The channel's qos as stored at init; NULL if unknown. */
const DartQos *dart_transport_channel_qos(DartTransportState *st, uint16_t channel);

/* 1 if appending here would overwrite history not yet acked by every reader. A
 * writer pumps while this is 1, then sends anyway after qos.backpressure_wait_us. */
int       dart_transport_send_would_evict(DartTransportState *st, uint16_t channel);

/* 1 if appending here would overwrite history some matched, live reader was never
 * HANDED TO THE WIRE (committed but not yet emitted by poll_send). Unlike
 * would_evict this applies to best-effort lanes too: it detects a send burst
 * outrunning the TX drain, not slow-reader flow control. The node waits on it so
 * a send cannot silently vaporize data that never left the process. On 1, the
 * evicted sample's base seqno / fragment count are written to the (NULLable) outs. */
int       dart_transport_send_would_evict_unsent(DartTransportState *st, uint16_t channel,
                                                 uint64_t *evict_base, uint32_t *evict_count);

/* 1 if every live reader has acked all messages on this reliable channel (so a
 * writer may close without truncating). Best-effort/unknown return 1. Wrapped as
 * dart_channel_drain. */
int       dart_transport_send_drained(DartTransportState *st, uint16_t channel);

/* Peers currently matched as readers (subscribers) of this channel. 0 = a publish
 * goes nowhere; a one-shot publisher can poll this before sending. */
int       dart_transport_writer_match_count(DartTransportState *st, uint16_t channel);

/* Per-peer match summary (diagnostic): how many channels we now PUBLISH to this peer
 * (it subscribes and we publish) and how many we RECEIVE from it (it publishes and we
 * subscribe). Counts unicast lanes; either out-pointer may be NULL, both 0 for an
 * unknown peer. Surfaced on DART_PEER_INTEREST so a caller can watch a connection form. */
void      dart_transport_peer_match_counts(DartTransportState *st, uint32_t peer_id,
                                 uint16_t *publish_to, uint16_t *receive_from);

/* Cumulative reliable-repair counters for a channel, summed over its peer/reader
 * proxies (writer side = this node publishing; reader side = subscribing). Always on;
 * each field is a plain bump on a path that already runs. The per-second deltas of
 * frags_resent (writer) and non-dup frags_recv (reader) are repair throughput; a flat
 * HOL snapshot (dart_transport_reader_progress) with rising nacks_sent is a wedged stream. */
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
void      dart_transport_repair_stats(DartTransportState *st, uint16_t channel, DartRepairStats *out);

/* Writer-side: number of reader lanes on this channel with a pending repair NACK to
 * service. 0 => the writer has nothing to resend right now (idle for lack of NACKs).
 * Diagnostic for the backpressure stall (distinguishes "no NACKs" from "resends
 * dropped"); sampled by the node's in-pump probe. */
int       dart_transport_repair_pending(DartTransportState *st, uint16_t channel);

/* Head-of-line reassembly snapshot for the in-progress message from `peer` on
 * `channel` (the message at the reader's deliver_upto). Returns 1 and fills the
 * out-params if a message is mid-reassembly, else 0.
 *   base_seqno : first seqno of the in-progress message (= reader deliver_upto)
 *   have       : fragments received so far (popcount of the reassembly bitmap)
 *   total      : fragments the message needs
 * `have` rising across calls => repair is crawling forward; flat => wedged. Any
 * out-pointer may be NULL. Wrapped as dart_channel_reader_progress. */
int       dart_transport_reader_progress(DartTransportState *st, uint16_t channel, uint32_t peer,
                            uint64_t *base_seqno, uint32_t *have, uint32_t *total);

/* Feed a received datagram, tagged with the peer it came from. */
void      dart_transport_on_datagram(DartTransportState *st, uint32_t from_peer, DartBytes datagram,
                         uint64_t now_us);

/* Pull one outgoing datagram (may batch submessages for one peer). Returns 1 and
 * fills *to_peer/out/out_len, or 0 when nothing is due. Loop until 0; pass DART_DGRAM_MAX cap. */
int       dart_transport_poll_send(DartTransportState *st, uint32_t *to_peer, void *out, size_t cap,
                       size_t *out_len, uint64_t now_us);

/* Absolute us of the next internal timer (deferred ack / NACK / heartbeat), or 0 if
 * none is pending. Cap a blocking poll at this so a due timer is serviced on time
 * instead of waiting out the poll quantum or the amortized sweep. */
uint64_t  dart_transport_next_deadline_us(DartTransportState *st);

#ifdef __cplusplus
}
#endif
#endif /* DART_TRANSPORT_H */
