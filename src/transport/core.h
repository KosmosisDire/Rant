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

/* FIXED (no-allocator) mode only: sizes the static per-peer alias tables, bounding the
 * highest peer alias that can demux. Auto-raised to 2*n_channels. Dynamic mode ignores
 * it: each peer's alias map is allocated at that peer's actual advertised size. */
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
    uint16_t keep_last;          /* recent messages retained for late join / repair. 0 = 1, or 10 on a reliable channel */
    uint16_t catch_up;           /* recent messages a new subscriber gets at once. 0 = future
                                    only, 1 = latest value. Keep small (bursts at startup) */
    uint32_t max_message_bytes;  /* biggest message. 0 = one fragment, or grow-to-fit with an allocator */
    uint32_t heartbeat_us;       /* reliable: idle-writer ping (repairs a lost final message). 0 = 250ms */
    uint32_t repair_delay_us;    /* reliable: reader's delay before requesting a resend. 0 = 50ms */
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
    DART_TRANSPORT_NAME_COLLISION,  /* a peer's name hashes to ours but differs (.identity, .channel), refused */
    DART_TRANSPORT_QOS_INCOMPATIBLE,/* a reliable subscriber refused a best-effort publisher (.channel, .peer) */
    DART_TRANSPORT_SCHEMA_MISMATCH, /* the schema_check hook refused a match (.channel, .peer) */
    DART_TRANSPORT_INTEREST_OVERFLOW,/* a peer's matched topics carry aliases we cannot map (.peer,
                                        .lost_count = entry count): their data can never demux here.
                                        Fixed mode: raise DART_META_MAX_IDS; dynamic: map alloc failed. */
    DART_TRANSPORT_META_TRUNCATED_INTEREST, /* our announce overlay overflowed: the interest list was
                                               dropped, so peers see none of our topics (needs ~13k topics
                                               at 5 B/entry against the one-datagram ceiling). */
    DART_TRANSPORT_META_TRUNCATED_SCHEMA    /* RETIRED (schemas left the announce for the detail
                                               exchange); value kept so binding enums stay aligned. */
} DartTransportEventKind;

/* The channel name for a channel-scoped event is not carried here: read it with
 * dart_transport_channel_name(st, ev->channel). Flat and self-describing: read only
 * the fields named for the .kind. */
typedef struct {
    DartTransportEventKind kind;
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
    /* optional schema gate: called at DETAIL INTAKE (dart_transport_apply_peer_details),
     * once per direction of a name-verified topic, with the peer's advertised schema
     * identity + canonical wire (hash 0 = untyped; wire {NULL,0} = not inlined, identical
     * or already known). peer_is_pub = 1 gates the read side (their publish, we would
     * decode), 0 the write side. Return 1 to allow, 0 to refuse. Both verdicts are cached
     * per (peer, alias) for the peer's lifetime (schemas are immutable per incarnation);
     * a refused direction forms no proxy and fires SCHEMA_MISMATCH when a later interest
     * apply would have used it. The transport knows nothing of schema contents; the node
     * implements this over the serialize layer. */
    int                 (*schema_check)(void *user, uint32_t peer, uint16_t channel,
                                        int peer_is_pub, uint64_t schema_hash,
                                        DartBytes schema_wire);
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
 * disseminate them however they like). The interest list is HASH-ONLY (no names, no
 * schemas): [u16 n] then one [u32 name-hash][u8 flags] entry per channel IN CHANNEL-INDEX
 * ORDER, so the entry's position IS the advertiser's alias (an INACTIVE/undefined slot
 * still occupies its position, keeping later aliases stable across role changes). The
 * hash is the low 32 bits of the 64-bit name identity; flags = role (bits 0-1, a
 * DartRole) | offered/requested reliability (bit 2).
 *
 * A 32-bit hash overlap only NOMINATES a candidate match, it never matches: names and
 * schemas are fetched pairwise via the detail exchange below, and the verified verdicts
 * (name equality, schema compatibility per direction) are cached per (peer, alias) for
 * the peer's lifetime. dart_transport_apply_peer_interest then derives the actual matches
 * from verdicts + the CURRENT flags on every apply, so a role/QoS change rematches
 * instantly with no round trip, while an unverified candidate stays PENDING (no proxy,
 * no data) until its details arrive. Aliases are append-only and their name/schema
 * immutable per peer incarnation: that is what makes the verdict cache sound.
 * dart_transport_build_interest serializes OUR set into out, returning bytes written or
 * 0 if cap is too small; size out via dart_interest_max. apply is idempotent. Re-build +
 * re-disseminate after dart_transport_set_role. */
size_t    dart_interest_max(uint16_t n_channels);
size_t    dart_transport_build_interest(DartTransportState *st, void *out, size_t cap);
void      dart_transport_apply_peer_interest(DartTransportState *st, uint32_t peer_id, DartBytes blob);

/* Discovery-announce meta blob (sans-IO codec). A versioned, opaque-to-discovery
 * payload wrapping this node's UDP fragment size, SHM capability + host uuid, and its
 * hash-only interest list: the transport's OVERLAY, carried opaquely inside discovery's
 * announce blob (the node name lives in discovery's own section, not here). Layout:
 *   v10: ['D','N',10, frag_lo, frag_hi,                <interest>]
 *   v11: ['D','N',11, frag_lo, frag_hi, shm, host[16], <interest>]
 * frag sits at [3..4] in both; v11 adds the SHM byte + host. dart_transport_meta_build
 * writes v11 when DART_SHM is compiled, v10 otherwise. NO topic names and NO schemas ride
 * the announce: those are fetched pairwise via the detail exchange ('uDTL' below), so a
 * 2000-topic announce is ~10 kB instead of overflowing the one-datagram ceiling. */

/* One channel's schema advertisement, registered by the node: the 64-bit identity plus a
 * view of the canonical wire bytes (valid for the channel's lifetime). hash 0 = none.
 * Served to peers by the detail responder (never in the announce). */
typedef struct {
    uint64_t  hash;
    DartBytes wire;
} DartMetaSchema;

/* Bytes to reserve for the overlay: prefix + the interest list at n_channels entries,
 * capped to one (IP-fragmentable) UDP datagram. Sizes discovery's meta_capacity. */
uint16_t  dart_meta_capacity(uint16_t n_channels);
/* Exact bytes the next dart_transport_meta_build will emit for the CURRENT channel state,
 * so a growable caller sizes its buffer to actual content; dart_meta_capacity stays the
 * fixed-buffer worst case (and the accept bound for peers' overlays). */
uint16_t  dart_transport_meta_size(DartTransportState *st);
/* Build the overlay into out[cap] (cap >= dart_transport_meta_size): the version prefix
 * (frag_size, plus shm_capable + host[16] when DART_SHM is compiled), then st's interest
 * list (one positional entry per channel slot up to the highest defined one). Returns
 * total bytes; host may be NULL when !shm_capable. The node NAME is not here: it rides
 * discovery's own section of the announce blob. */
uint16_t  dart_transport_meta_build(DartTransportState *st, uint8_t *out, uint16_t cap,
                       uint16_t frag_size, int shm_capable, const uint8_t host[16]);
/* A peer's advertised UDP fragment size from the overlay; 0 if malformed. */
uint16_t  dart_meta_frag(DartBytes meta);
/* The interest sub-blob inside the overlay; {NULL, 0} if absent. */
DartBytes dart_meta_interest(DartBytes meta);

/* One advertised topic, as decoded by dart_meta_interest_next. The announce carries no
 * topic name: only the 32-bit hash rides here. Fetch the name (and schema) via the
 * detail exchange below. */
typedef struct {
    uint16_t    alias;      /* the advertiser's channel index (== the entry's position) */
    uint8_t     role;       /* the advertiser's DartRole for this topic */
    uint8_t     is_pub;     /* this yield: 1 = publish direction, 0 = subscribe (a PUBSUB
                               topic yields twice, pub first, mirroring the old two-list walk) */
    uint8_t     reliable;   /* offered (pub yield) / requested (sub yield) reliability */
    uint32_t    hash;       /* low 32 bits of the topic's 64-bit name identity */
} DartTopic;

/* Iterator state for dart_meta_interest_next: zero-initialize, then call until it
 * returns 0. The fields are internal walk state, not for direct use. */
typedef struct {
    uint32_t off;        /* byte offset of the next entry within the overlay */
    uint16_t left;       /* entries still to walk */
    uint16_t alias;      /* position of the next entry */
    uint8_t  phase;      /* 1 = the current entry's pub direction was yielded */
    uint8_t  started;    /* 0 until the first call parses the [u16 n] header */
} DartInterestIter;

/* Walk a peer's interest list one advertised DIRECTION at a time (a PUBSUB topic yields
 * a pub entry then a sub entry; INACTIVE/undefined slots are skipped). Pass the same
 * overlay each call with a zeroed DartInterestIter; returns 1 and fills *out, or 0 at
 * the end (or on a malformed/truncated blob: it stops rather than reading past the end).
 * Usage:
 *   DartInterestIter it = {0}; DartTopic t;
 *   while (dart_meta_interest_next(meta, &it, &t)) { ... } */
int       dart_meta_interest_next(DartBytes meta, DartInterestIter *it, DartTopic *out);
#ifdef DART_SHM
/* A peer's SHM capability + host uuid (v3/v5 blobs only): 1 if SHM-capable (fills
 * host[16]), else 0. */
int       dart_meta_shm(DartBytes meta, uint8_t host[16]);
#endif

/* Pairwise detail exchange (sans-IO codec for the 'uDTL' datagram family). A requester
 * asks a peer for the full details of specific advertised topics (by alias): the topic
 * NAME (collision check), the SCHEMA hash, and the schema wire where the two hashes
 * differ. The responder is STATELESS: a response is a read-only answer built from the
 * request's alias list and sent back to the request's source address, so duplicates are
 * harmless and nobody stores requests; a lost response heals by the requester re-asking.
 * The node runtime routes these on its unicast data socket next to the transport
 * datagrams; sans-IO callers run the codec over their own pipe. Layout (LE):
 *   ['u','D','T','L'][kind][fam ver=1][u16 domain][u32 meta_version][u16 n][entries]
 *   REQ  entry: [u16 alias][u64 schema_hash]     the REQUESTER's hash for its matching
 *               channel (0 = none), so the responder inlines the wire only on mismatch
 *   RESP entry: [u16 alias][u8 namelen][name][u64 schema_hash][u16 wire_len][wire]
 * meta_version: on a REQ, the responder announce version the aliases were read from; on
 * a RESP, the responder's CURRENT version (what the details bind to). A RESP holds only
 * the requested aliases the responder currently advertises, truncated at an entry
 * boundary when it cannot fit the cap: the requester re-requests what it still lacks. */
#define DART_DETAIL_REQ  1
#define DART_DETAIL_RESP 2

/* One requested topic: the peer's alias + our schema hash for it (0 = untyped/none). */
typedef struct {
    uint16_t alias;
    uint64_t schema_hash;
} DartDetailWant;

/* Header accessors, safe on any buffer: kind returns DART_DETAIL_REQ/RESP, or 0 when the
 * datagram is not a well-formed detail header (wrong magic/version/too short). */
int       dart_detail_kind(DartBytes dgram);
uint16_t  dart_detail_domain(DartBytes dgram);
uint32_t  dart_detail_meta_version(DartBytes dgram);

/* Build a DETAIL_REQ for n_wants topics. Returns bytes written, or 0 if cap is too
 * small for all of them (14 + 10 per want): batch per peer, split only if huge. */
size_t    dart_detail_req_build(uint16_t domain, uint32_t peer_meta_version,
                       const DartDetailWant *wants, uint16_t n_wants,
                       void *out, size_t cap);

/* Exact bytes a full (untruncated) response to req takes, for sizing the buffer; 0 if
 * req is malformed. Same walk as dart_transport_detail_respond, byte for byte. */
size_t    dart_transport_detail_resp_size(DartTransportState *st, const DartMetaSchema *schemas,
                       DartBytes req);
/* Answer req into out[cap]: one entry per requested alias this st currently advertises
 * (unknown/INACTIVE aliases are skipped), the schema wire inlined only where the
 * request's hash differs from ours. schemas is the same per-channel array
 * dart_transport_meta_build takes (or NULL). meta_version stamps the response (pass the
 * current announce version). Fills what fits, truncating at an entry boundary (the
 * requester re-requests the rest). Returns bytes written; 0 = malformed req or cap
 * cannot hold the header. Does NOT check the domain: that is the caller's. */
size_t    dart_transport_detail_respond(DartTransportState *st, const DartMetaSchema *schemas,
                       uint32_t meta_version, DartBytes req, void *out, size_t cap);

/* One topic's details, as decoded by dart_detail_next. name/schema_wire point into the
 * source response (NOT NUL-terminated / not owned), so keep that buffer alive. */
typedef struct {
    uint16_t   alias;       /* the responder's local channel index */
    DartString name;        /* topic name (not NUL-terminated) */
    uint64_t   schema_hash; /* the responder's schema identity (0 = untyped) */
    DartBytes  schema_wire; /* canonical wire bytes, only when the request's hash differed
                               ({NULL,0} otherwise: identical hash means identical wire) */
} DartDetail;

/* Iterator state for dart_detail_next: zero-initialize, then call until it returns 0.
 * The fields are internal walk state, not for direct use. */
typedef struct {
    uint32_t off;      /* byte offset of the next entry */
    uint16_t left;     /* entries still to yield */
    uint8_t  started;  /* 0 until the first call parses the header */
} DartDetailIter;

/* Walk a DETAIL_RESP one topic at a time. Pass the same response each call with a zeroed
 * DartDetailIter; returns 1 and fills *out, or 0 at the end (or on a malformed/truncated
 * response: it stops rather than reading past the end). */
int       dart_detail_next(DartBytes resp, DartDetailIter *it, DartDetail *out);

/* The requester side of the pending-match cycle. detail_wants scans a peer's interest
 * list (its announce overlay's interest section) for CANDIDATES: entries whose 32-bit
 * hash matches a local channel, whose roles overlap ours, and whose verdict is not yet
 * cached. It fills up to max_wants request entries (each carrying OUR schema hash for
 * the responder's inline-on-mismatch rule) and returns the count: 0 means nothing is
 * pending for this peer (converged). out may be NULL to just count. Re-run it on every
 * announce from the peer while it returns nonzero (lost datagrams heal by re-asking).
 *
 * apply_peer_details ingests a DETAIL_RESP: each entry is verified (full 64-bit identity
 * recomputed from the name; a same-hash different-name peer fires NAME_COLLISION and is
 * refused; the schema_check hook gates both directions) and the verdict cached. Returns
 * the number of newly decided aliases; when nonzero, re-run
 * dart_transport_apply_peer_interest with the peer's current interest so the new verdicts
 * form their matches (proxies, catch-up replay) exactly as a fresh announce would. */
uint16_t  dart_transport_detail_wants(DartTransportState *st, const DartMetaSchema *schemas,
                       uint32_t peer_id, DartBytes interest,
                       DartDetailWant *out, uint16_t max_wants);
uint16_t  dart_transport_apply_peer_details(DartTransportState *st, uint32_t peer_id,
                       DartBytes resp);

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
