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
 * only between same-host nodes; a node with no local SHM peer
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

#ifndef DART_FRAG_SIZE
#define DART_FRAG_SIZE 1350u          /* default bytes of message data per fragment */
#endif
/* The UDP fragment size is set PER NODE at init (DartConfig.frag_size) and
 * advertised via discovery, so a receiver reassembles each message at the SOURCE
 * node's size -- a publisher always fragments with one size, so its seqno line stays
 * self-consistent (no per-message field on the wire). These two compile bounds
 * frame the runtime range; both default to DART_FRAG_SIZE, i.e. no change unless
 * you opt in. MAX sizes the datagram buffers (raise it for jumbo frames / a
 * bigger same-LAN size); MIN is the lower clamp bound for any node's advertised
 * size. Every node's frag_size must lie in [MIN, MAX]. */
#ifndef DART_FRAG_SIZE_MAX
#define DART_FRAG_SIZE_MAX DART_FRAG_SIZE
#endif
#ifndef DART_FRAG_SIZE_MIN
#define DART_FRAG_SIZE_MIN DART_FRAG_SIZE
#endif
#define DART_DGRAM_MAX (DART_FRAG_SIZE_MAX + 40u)   /* + largest header */

#ifdef DART_SHM
#define DART_SHM_DESC_BYTES 24u   /* opaque SHM descriptor on the wire; == dart_shm.h DART_SHM_DESC_WIRE */
#endif

#ifndef DART_TOPIC_NAME_MAX
#define DART_TOPIC_NAME_MAX 64u          /* max topic-name bytes on the wire */
#endif

#ifndef DART_NODE_NAME_MAX
#define DART_NODE_NAME_MAX 32u           /* max node-name bytes carried in the announce meta blob */
#endif

/* Bytes of the SOURCE TIMESTAMP prepended inside every sample a stamped topic commits,
 * ahead of the pattern header: wire payload = [sent_us u64 LE][pattern hdr][user payload].
 * Stamped once at the writer's commit point from DartConfig.source_time, so a repair
 * resend, a catch_up replay and an SHM chunk all carry the ORIGINAL stamp. A topic opts
 * out with DartQos.no_timestamp (then nothing is prepended and receivers are told so via
 * the announce). Ordinary payload bytes for fragmentation and size accounting. */
#define DART_TIMESTAMP_BYTES 8u

typedef enum { DART_BEST_EFFORT = 0, DART_RELIABLE = 1 } DartReliability;
/* DART_INACTIVE = declared but off (resources stay allocated; dart_transport_set_role flips it) */
typedef enum { DART_PUBSUB = 0, DART_PUB_ONLY = 1, DART_SUB_ONLY = 2,
               DART_INACTIVE = 3 } DartRole;

/* Entity kind: what a topic carries. Plain pub/sub is DART_KIND_TOPIC (0); the patterns
 * layer (src/patterns/) builds functions, variables, and signals over dedicated kinds,
 * each a distinct channel that only pairs with the same kind. The kind rides the announce
 * interest flags (bits 3-5, so 8 values) and gates matching like role/reliability: a same
 * name with a different kind is a disjoint entity, its pairing refused (KIND_MISMATCH), not
 * silently cross-wired. Kind is immutable per topic (like name/schema). */
typedef enum {
    DART_KIND_TOPIC    = 0,   /* plain pub/sub */
    DART_KIND_FUNC_REQ = 1,   /* function request channel  (caller pubs, provider subs) */
    DART_KIND_FUNC_RSP = 2,   /* function response channel  (provider pubs, caller subs; directed) */
    DART_KIND_VARIABLE = 3,   /* variable value channel     (owner pubs, observers sub) */
    DART_KIND_VAR_SET  = 4,   /* variable set channel       (writers pub, owner subs) */
    DART_KIND_SIGNAL   = 5    /* signal channel             (emitters pub, listeners sub) */
} DartTopicKind;

/* Every field except reliability is zero-means-default, so a reliable topic is
 * just { .reliability = DART_RELIABLE }. */
typedef struct {
    DartReliability reliability;
    uint16_t keep_last;          /* recent messages retained for late join / repair. 0 = 1, or 10 on a reliable topic */
    uint16_t catch_up;           /* recent messages a new subscriber gets at once. 0 = future
                                    only, 1 = latest value. Keep small (bursts at startup) */
    uint32_t max_message_bytes;  /* size HINT, not a cap: pins the same-host SHM class when
                                    shm_max_bytes is 0. Buffers grow to fit any message up to
                                    the wire cap (DART_MESSAGE_MAX) regardless. */
    uint32_t heartbeat_us;       /* reliable: idle-publisher ping (repairs a lost final message). 0 = 250ms */
    uint32_t repair_delay_us;    /* reliable: subscriber's delay before requesting a resend. 0 = 50ms */
    uint32_t backpressure_wait_us;/* reliable: how long a send pauses for a slow subscriber before
                                    evicting un-acked history. 0 = none (pure KEEP_LAST) */
    uint32_t shm_max_bytes;      /* same-host SHM: pin this topic to one size class big enough for
                                    this many bytes, so same-sized traffic reuses one pre-sized
                                    segment (a larger message falls back to UDP). 0 = each message
                                    uses its own size class's segment, created on demand. */
    uint32_t queue_bytes;        /* NODE-level consumer queue capacity (dart_topic_take /
                                    dart_topic_dispatch): the byte bound on how far a consumer
                                    may fall behind the poll. Setting it makes the topic queued
                                    from creation; 0 = the queue appears lazily on the first take/
                                    dispatch, capped at DART_QUEUE_CAP. The ring starts small and
                                    grows on demand to the cap, like the message buffers. The
                                    transport core itself ignores this field. */
    uint16_t max_rate_hz;        /* SUBSCRIBER side, BEST-EFFORT only: cap delivery of this topic
                                    from each publisher to this many samples/sec. The publisher
                                    paces its fire-and-forget lane to us: it DECIMATES (sends the
                                    NEWEST sample each tick and drops older un-sent ones at the
                                    source, so less wire + reader work), while our per-peer wire
                                    seqno keeps loss detection honest (a paced skip is not loss; a
                                    dropped SENT sample still is). 0 = unlimited (full rate).
                                    Advertised in the announce; ignored on a reliable topic and on
                                    the publish side. Set at create (immutable per topic). */
    uint8_t  no_timestamp;       /* PUBLISHER side: publish this topic WITHOUT the 8-byte source
                                    timestamp (DART_TIMESTAMP_BYTES), so a receiver sees sent_us 0.
                                    0 (the default) stamps every message with the sender's wall
                                    clock at the commit point. The opt-out is advertised in the
                                    announce, so a receiver always knows whether the stream carries
                                    the stamp. Set at create (immutable per topic). */
} DartQos;

/* A topic. Cross-peer identity is the name (64-bit hash); the LOCAL
 * handle for dart_transport_send / on_message is the topic's index in topics[]. */
typedef struct {
    const char *name;  /* topic name = cross-peer identity. Required, same on every node, <= DART_TOPIC_NAME_MAX */
    DartQos   qos;
    uint8_t  role;     /* DartRole; 0 = pub+sub */
    uint8_t  kind;     /* DartTopicKind; 0 = plain DART_KIND_TOPIC (the patterns layer sets the rest) */
    uint8_t  forceable; /* patterns aux flag ridden in interest bit 6 (the variable value channel sets it
                           when the owner permits force); opaque to the transport, 0 = plain topic. */
    uint8_t  prefix_bytes; /* pattern-header bytes prepended to every payload on this topic (the wire
                              carries hdr+payload as one message; the receiver splits at this offset and
                              validates the schema against the payload only). 0 = none. */
    uint8_t  directed; /* 1 = messages are addressed point-to-point (dart_transport_send_to): a
                          non-destination reliable lane is skipped past the seqno via its HB floor
                          without surfacing MSG_LOST. Used by function-response channels. */
} DartTopicDef;

/* dart_transport_poll_send destination: a peer id. Data is unicast point-to-point per matched subscriber. */

/* A complete message; topic is the local handle. Do not call back into dart_*.
 * Return 0 = delivered. Nonzero = REFUSED (downstream has nowhere to put it, e.g. the
 * node's consumer queue is full): on a reliable topic the subscriber PARKS the assembled
 * message -- no advance, no ack, no repair traffic -- so the publisher's own flow control
 * carries the backpressure to the publisher; retry with dart_transport_deliver_parked.
 * On a best-effort topic a refusal is a drop (KEEP_LAST semantics). Callers with
 * nothing to refuse just return 0. */
typedef int (*DartMessageFn)(void *user, uint16_t topic_index, uint32_t from_peer, DartBytes data);

#ifdef DART_SHM
/* SHM delivery: the transport reassembled nothing -- it hands the node the
 * DART_SHM_DESC_BYTES descriptor from an SHM-DATA submessage and the node resolves it
 * to bytes and calls the user's on_message. Returns 1 if delivered; 0 if it could not
 * resolve the chunk (recycled / unattachable) -- then the subscriber leaves the gap so the
 * reliability layer repairs or skips it; -1 if delivery was REFUSED downstream (consumer
 * queue full) -- then a reliable subscriber parks the descriptor exactly like a refused
 * inline message (see DartMessageFn). Internal (transport->node); the user's on_message
 * is unchanged and never sees this. */
typedef int (*i_DartShmMsgFn)(void *user, uint16_t topic_index, uint32_t from_peer,
                             const uint8_t *desc);
#endif

/* Transport events (optional), delivered through one on_event. The transport is
 * independent of discovery/node: it emits only its own kinds. The node maps these
 * into its app-facing DartEvent (node/core.h); a sans-IO transport user handles them
 * directly. Flat and self-describing: read only the fields named for the .kind. */
typedef enum {
    DART_TRANSPORT_MSG_LOST,        /* messages skipped: .topic, .peer, .lost_first .. +.lost_count-1 */
    DART_TRANSPORT_MSG_TOO_BIG,     /* a received message could not be buffered (allocation
                                       failed at .too_big_bytes), skipped */
    DART_TRANSPORT_NAME_COLLISION,  /* a peer's name hashes to ours but differs (.identity, .topic), refused */
    DART_TRANSPORT_QOS_INCOMPATIBLE,/* a reliable subscriber refused a best-effort publisher (.topic, .peer) */
    DART_TRANSPORT_SCHEMA_MISMATCH, /* the schema_check hook refused a match (.topic, .peer,
                                       .peer_is_pub = the refused direction) */
    DART_TRANSPORT_INTEREST_OVERFLOW,/* a peer's matched topics carry indices we cannot map (.peer,
                                        .lost_count = entry count): the index-map allocation failed,
                                        so their data can never demux here. */
    DART_TRANSPORT_META_TRUNCATED_INTEREST, /* our announce overlay overflowed: the interest list was
                                               dropped, so peers see none of our topics (needs ~13k topics
                                               at 5 B/entry against the one-datagram ceiling). */
    DART_TRANSPORT_META_TRUNCATED_SCHEMA,   /* RETIRED (schemas left the announce for the detail
                                               exchange); value kept so binding enums stay aligned. */
    DART_TRANSPORT_KIND_MISMATCH    /* a name-verified peer advertised the same topic name under a
                                       different entity kind (.topic, .peer): the pairing is refused,
                                       never silently cross-wired (a plain topic vs a function, etc.) */
} DartTransportEventKind;

/* The topic name for a topic-scoped event is not carried here: read it with
 * dart_transport_topic_name(st, ev->topic). Flat and self-describing: read only
 * the fields named for the .kind. */
typedef struct {
    DartTransportEventKind kind;
    void       *user;          /* DartConfig.user */
    uint32_t   peer;           /* peer id (0 = n/a) */
    uint16_t   topic;        /* local topic handle */
    uint64_t   lost_first;     /* MSG_LOST: first skipped seqno */
    uint64_t   lost_count;     /* MSG_LOST: number of messages skipped */
    uint64_t   too_big_bytes;  /* MSG_TOO_BIG: size of the dropped message */
    uint64_t   identity;       /* NAME_COLLISION: the colliding 64-bit topic identity */
    uint8_t    peer_is_pub;    /* SCHEMA_MISMATCH: the refused direction, as in the
                                  schema_check hook (1 = their publisher, our read side) */
} DartTransportEvent;
typedef void (*DartTransportEventFn)(const DartTransportEvent *ev);

/* Largest message the wire can carry (65535 fragments, ~64 MB by default). */
#define DART_MESSAGE_MAX (65535u * DART_FRAG_SIZE_MAX)

/* DartConfig.allocator is REQUIRED (a DartAllocFn, common/alloc.h): message buffers,
 * reassembly state, per-peer index maps, and lane records all size dynamically through
 * it, so memory scales with actual traffic and matches, never with worst-case tables.
 * An embedded no-heap deployment passes dart_allocator_alloc over a DartAllocator in
 * STATIC mode (one caller buffer, no growth, overflow refused) -- same contract, no heap.
 * Pair with dart_transport_destroy to free the hook allocations at teardown. */

/* Two ways to populate the topic table:
 *   at-init      : topics != NULL, n_topics = its length. Slots are defined now.
 *   reserve/lazy : topics == NULL, n_topics = the reserved capacity. All slots start
 *                  DART_INACTIVE; fill them later with dart_transport_topic_define
 *                  (this is how the node's runtime dart_node_create_topic works). */
typedef struct {
    const DartTopicDef *topics;     /* NULL = reserve mode (see above) */
    uint16_t              n_topics;   /* defined count, or reserved capacity in reserve mode */
    uint16_t              max_peers;
    uint16_t              frag_size; /* UDP fragment size this node sends with; 0 =
                                           DART_FRAG_SIZE. Clamped to [MIN, MAX]. */
    DartMessageFn         on_message;
#ifdef DART_SHM
    i_DartShmMsgFn         on_shm;     /* SHM-DATA delivery (descriptor); the node resolves it */
#endif
    DartTransportEventFn  on_event;   /* optional: transport events (loss/too-big/collision/qos) */
    DartAllocFn           allocator;  /* REQUIRED (see above); init fails without it */
    /* optional schema gate: called at DETAIL INTAKE (dart_transport_apply_peer_details),
     * once per direction of a name-verified topic, with the peer's advertised schema
     * identity + canonical wire (hash 0 = untyped; wire {NULL,0} = not inlined, identical
     * or already known). peer_is_pub = 1 gates the read side (their publish, we would
     * decode), 0 the write side. Return 1 to allow, 0 to refuse. Both verdicts are cached
     * per (peer, index) for the peer's lifetime (schemas are immutable per incarnation);
     * a refused direction forms no proxy and fires SCHEMA_MISMATCH when a later interest
     * apply would have used it. The transport knows nothing of schema contents; the node
     * implements this over the serialize layer. */
    int                 (*schema_check)(void *user, uint32_t peer, uint16_t topic_index,
                                        int peer_is_pub, uint64_t schema_hash,
                                        DartBytes schema_wire);
    /* optional source clock: WALL time in UTC microseconds, called with `user` at the
     * writer's commit point and stamped into the sample (DART_TIMESTAMP_BYTES). The core is
     * sans-IO and owns no clock, so this is how a message gets its send timestamp; the node
     * runtime wires it to the platform wall clock. NULL = the 8 bytes are still framed on a
     * stamped topic but carry 0, so the wire layout never depends on the hook. Never the
     * monotonic now_us: the stamp is meant to be compared across hosts. */
    uint64_t            (*source_time)(void *user);
    void                 *user;
} DartConfig;

typedef struct DartTransportState DartTransportState;

size_t    dart_transport_required_memory(const DartConfig *cfg);
DartTransportState *dart_transport_init(void *mem, size_t mem_size, const DartConfig *cfg);
/* Relocate a live transport into new_mem (>= dart_transport_required_memory at the grown counts),
 * re-striding its tables to new_max_peers/new_n_topics and carrying live reliability
 * state (positions, history, in-flight repair) across. Heap buffers are not in the arena,
 * so the caller frees old's arena block afterward but must NOT dart_transport_destroy old. Returns
 * the new state, or NULL on failure (old is left intact). Dynamic-mode growth only. */
DartTransportState *dart_transport_migrate(DartTransportState *old, void *new_mem, size_t new_cap,
                        uint16_t new_max_peers, uint16_t new_n_topics);
/* Free every hook allocation (message buffers, reassembly state, index maps, lane
 * records); the arena stays the caller's. The node calls it from close. */
void      dart_transport_destroy(DartTransportState *st);

/* 64-bit topic identity from a name (FNV-1a): matches topics across peers. */
uint64_t  dart_topic_id(const char *name);
uint64_t  dart_topic_identity(const DartTopicDef *def);   /* = dart_topic_id(def->name) */

/* Normalize a UDP fragment size: 0 -> DART_FRAG_SIZE, then clamp to [MIN,MAX].
 * The rule dart_transport_init and the node's announce blob both apply (single source). */
uint16_t  dart_clamp_frag(uint16_t frag_size);

/* This node's own (clamped) UDP fragment size: a send whose payload exceeds it
 * fragments into 2+ datagrams. The node uses it as the SHM cutoff: a message that
 * fits one datagram gains nothing from SHM (SHM still sends a descriptor datagram),
 * so only messages larger than this take the shared-memory path. */
uint16_t  dart_transport_frag(DartTransportState *st);

/* A new peer matches nothing until dart_transport_apply_peer_interest feeds its interest
 * list (carried in its discovery announce). peer_frag: that peer's advertised UDP
 * fragment size (from discovery), used to reassemble its messages; 0 = DART_FRAG_SIZE.
 * Clamped to [MIN, MAX]. */
void      dart_transport_peer_add   (DartTransportState *st, uint32_t peer_id, uint16_t peer_frag);
void      dart_transport_peer_remove(DartTransportState *st, uint32_t peer_id);

/* Discovery-blip lifecycle: a peer that fell silent (discovery timeout) is made
 * DORMANT instead of removed, so its subscriber position survives and a same-incarnation
 * return resumes losslessly. Dormant peers are dropped from flow control (the publisher
 * stops heartbeating/draining them so a dead subscriber can't stall it; the subscriber stops
 * acking them), but their proxies and deliver position are preserved. dart_transport_peer_resume
 * re-includes the peer and re-reports subscriber positions so the publisher fills any gap.
 * Both no-op for an unknown peer; the node drives them off discovery DROP/return. */
void      dart_transport_peer_dormant(DartTransportState *st, uint32_t peer_id);
void      dart_transport_peer_resume (DartTransportState *st, uint32_t peer_id);
/* Update a peer's advertised UDP fragment size (its announce blob may arrive after
 * first contact). Clamped to [MIN, MAX]; no-op for an unknown peer. */
void      dart_transport_peer_set_frag(DartTransportState *st, uint32_t peer_id, uint16_t peer_frag);

/* Interest exchange (the node carries these in discovery announces; sans-IO callers
 * disseminate them however they like). The interest list is HASH-ONLY (no names, no
 * schemas): [u16 n] then one [u32 name-hash][u8 flags] entry per topic IN TOPIC-INDEX
 * ORDER, so the entry's position IS the advertiser's index (an INACTIVE/undefined slot
 * still occupies its position, keeping later indices stable across role changes). The
 * hash is the low 32 bits of the 64-bit name identity; flags = role (bits 0-1, a
 * DartRole) | offered/requested reliability (bit 2).
 *
 * A 32-bit hash overlap only NOMINATES a candidate match, it never matches: names and
 * schemas are fetched pairwise via the detail exchange below, and the verified verdicts
 * (name equality, schema compatibility per direction) are cached per (peer, index) for
 * the peer's lifetime. dart_transport_apply_peer_interest then derives the actual matches
 * from verdicts + the CURRENT flags on every apply, so a role/QoS change rematches
 * instantly with no round trip, while an unverified candidate stays PENDING (no proxy,
 * no data) until its details arrive. Indices are append-only and their name/schema
 * immutable per peer incarnation: that is what makes the verdict cache sound.
 * dart_transport_build_interest serializes OUR set into out, returning bytes written or
 * 0 if cap is too small; size out via dart_interest_max. apply is idempotent. Re-build +
 * re-disseminate after dart_transport_set_role.
 *
 * Two SPARSE sections follow the positional entries, each defaulting to zero entries so a
 * plain node pays 4 bytes total: the best-effort rate caps (DartQos.max_rate_hz) and the
 * topics published WITHOUT a source timestamp (DartQos.no_timestamp), so a receiver knows
 * per writer whether the stream carries the 8-byte stamp. */
size_t    dart_interest_max(uint16_t n_topics);
size_t    dart_transport_build_interest(DartTransportState *st, void *out, size_t cap);
void      dart_transport_apply_peer_interest(DartTransportState *st, uint32_t peer_id, DartBytes blob);

/* Discovery-announce meta blob (sans-IO codec). A versioned, opaque-to-discovery
 * payload wrapping this node's UDP fragment size, SHM capability + host uuid, an iflags
 * byte, and (inline case) its hash-only interest list: the transport's OVERLAY, carried
 * opaquely inside discovery's announce blob (the node name lives in discovery's own
 * section, not here). Layout:
 *   v16: ['D','N',16, frag_lo, frag_hi, iflags,                <interest>]
 *   v17: ['D','N',17, frag_lo, frag_hi, shm, host[16], iflags, <interest>]
 * frag sits at [3..4] in both; v17 adds the SHM byte + host. dart_transport_meta_build
 * writes v17 when DART_SHM is compiled, v16 otherwise. iflags bit 0 = INTEREST_EXTERNAL:
 * the interest list did NOT fit the announce datagram, nothing follows the base, and
 * peers pull the identical interest blob over the unicast 'uDTL' paging below
 * (DART_INTEREST_REQ/RESP). Clear = the inlined interest (even when empty) is
 * authoritative. So the announce NEVER exceeds one datagram and never relies on IP
 * reassembly, no matter how many topics a node has (a constrained receiver, e.g. lwIP
 * with reassembly off, always gets the bootstrap: locator + version + this flag).
 * NO topic names and NO schemas ride the announce either: those are fetched pairwise
 * via the detail exchange ('uDTL' below). */

/* One topic's schema advertisement, registered by the node: the 64-bit identity plus a
 * view of the canonical wire bytes (valid for the topic's lifetime). hash 0 = none.
 * Served to peers by the detail responder (never in the announce). */
typedef struct {
    uint64_t  hash;
    DartBytes wire;
} DartMetaSchema;

/* Bytes to reserve for the overlay: prefix + the interest list at n_topics entries,
 * capped to one (IP-fragmentable) UDP datagram. Sizes discovery's meta_cap. */
uint16_t  dart_meta_cap(uint16_t n_topics);
/* Exact bytes the next INLINE dart_transport_meta_build will emit for the CURRENT topic
 * state, so a growable caller sizes its buffer to actual content; dart_meta_cap stays the
 * worst-case bound (and the accept bound for peers' overlays). The bootstrap (external)
 * form is always dart_transport_meta_bootstrap_size(). */
uint16_t  dart_transport_meta_size(DartTransportState *st);
uint16_t  dart_transport_meta_bootstrap_size(void);
/* Build the overlay into out[cap] (cap >= dart_transport_meta_size, or the bootstrap
 * size when interest_external): the version prefix (frag_size, plus shm_capable +
 * host[16] when DART_SHM is compiled) and the iflags byte, then st's interest list (one
 * positional entry per topic slot up to the highest defined one) -- unless
 * interest_external is nonzero, which sets the INTEREST_EXTERNAL bit and writes NO
 * interest (peers pull it via DART_INTEREST_REQ). The caller owns the policy: inline
 * whenever the whole announce fits one datagram (the node compares
 * dart_transport_meta_size against its announce budget). Returns total bytes; host may
 * be NULL when !shm_capable. The node NAME is not here: it rides discovery's own
 * section of the announce blob. */
uint16_t  dart_transport_meta_build(DartTransportState *st, uint8_t *out, uint16_t cap,
                       uint16_t frag_size, int shm_capable, const uint8_t host[16],
                       int interest_external);
/* A peer's advertised UDP fragment size from the overlay; 0 if malformed. */
uint16_t  dart_meta_frag(DartBytes meta);
/* The INTEREST_EXTERNAL bit: 1 = the overlay carries no interest and the peer serves it
 * via the uDTL interest paging; 0 = the inline interest (even empty) is authoritative. */
int       dart_meta_interest_external(DartBytes meta);
/* The interest sub-blob inside the overlay; {NULL, 0} if absent OR external (so a
 * bootstrap can never be misread as an empty interest list). */
DartBytes dart_meta_interest(DartBytes meta);

/* One advertised topic, as decoded by dart_meta_interest_next. The announce carries no
 * topic name: only the 32-bit hash rides here. Fetch the name (and schema) via the
 * detail exchange below. */
typedef struct {
    uint16_t    index;      /* the advertiser's topic index (== the entry's position) */
    uint8_t     role;       /* the advertiser's DartRole for this topic */
    uint8_t     is_pub;     /* this yield: 1 = publish direction, 0 = subscribe (a PUBSUB
                               topic yields twice, pub first, mirroring the old two-list walk) */
    uint8_t     reliable;   /* offered (pub yield) / requested (sub yield) reliability */
    uint8_t     kind;       /* the advertiser's DartTopicKind for this topic (0 = plain) */
    uint8_t     forceable;  /* patterns aux flag (interest bit 6): the variable value channel's owner permits force */
    uint32_t    hash;       /* low 32 bits of the topic's 64-bit name identity */
} DartTopicEntry;

/* Iterator state for dart_meta_interest_next: zero-initialize, then call until it
 * returns 0. The fields are internal walk state, not for direct use. */
typedef struct {
    uint32_t off;        /* byte offset of the next entry within the overlay */
    uint16_t left;       /* entries still to walk */
    uint16_t index;      /* position of the next entry */
    uint8_t  phase;      /* 1 = the current entry's pub direction was yielded */
    uint8_t  started;    /* 0 until the first call parses the [u16 n] header */
} DartInterestIter;

/* Walk an interest list one advertised DIRECTION at a time (a PUBSUB topic yields
 * a pub entry then a sub entry; INACTIVE/undefined slots are skipped). Pass the same
 * blob each call with a zeroed DartInterestIter; returns 1 and fills *out, or 0 at
 * the end (or on a malformed/truncated blob: it stops rather than reading past the end).
 * dart_interest_next walks a BARE interest blob (dart_transport_build_interest output,
 * e.g. one assembled from external-interest pages); dart_meta_interest_next walks the
 * one inlined in an announce overlay (nothing for a bootstrap). Usage:
 *   DartInterestIter it = {0}; DartTopicEntry t;
 *   while (dart_meta_interest_next(meta, &it, &t)) { ... } */
int       dart_interest_next(DartBytes interest, DartInterestIter *it, DartTopicEntry *out);
int       dart_meta_interest_next(DartBytes meta, DartInterestIter *it, DartTopicEntry *out);
#ifdef DART_SHM
/* A peer's SHM capability + host uuid (odd-version blobs only): 1 if SHM-capable (fills
 * host[16]), else 0. */
int       dart_meta_shm(DartBytes meta, uint8_t host[16]);
#endif

/* Pairwise detail exchange (sans-IO codec for the 'uDTL' datagram family). A requester
 * asks a peer for the full details of specific advertised topics (by index): the topic
 * NAME (collision check), the SCHEMA hash, and the schema wire where the two hashes
 * differ. The responder is STATELESS: a response is a read-only answer built from the
 * request's index list and sent back to the request's source address, so duplicates are
 * harmless and nobody stores requests; a lost response heals by the requester re-asking.
 * The node runtime routes these on its unicast data socket next to the transport
 * datagrams; sans-IO callers run the codec over their own pipe. Layout (LE):
 *   ['u','D','T','L'][kind][fam ver=1][u16 domain][u32 meta_version][u16 n][entries]
 *   REQ  entry: [u16 index][u64 schema_hash]     the REQUESTER's hash for its matching
 *               topic (0 = none), so the responder inlines the wire only on mismatch
 *   RESP entry: [u16 index][u8 namelen][name][u64 schema_hash][u16 wire_len][wire]
 * meta_version: on a REQ, the responder announce version the indices were read from; on
 * a RESP, the responder's CURRENT version (what the details bind to). A RESP holds only
 * the requested indices the responder currently advertises, truncated at an entry
 * boundary when it cannot fit the cap: the requester re-requests what it still lacks.
 * Kinds 3/4 (interest paging) share the family header with [u16 n] = 0. */
#define DART_DETAIL_REQ    1
#define DART_DETAIL_RESP   2
#define DART_INTEREST_REQ  3
#define DART_INTEREST_RESP 4

/* One requested topic: the peer's index + our schema hash for it (0 = untyped/none). */
typedef struct {
    uint16_t index;
    uint64_t schema_hash;
} DartDetailWant;

/* Header accessors, safe on any buffer: kind returns one of the four uDTL kinds above,
 * or 0 when the datagram is not a well-formed family header (wrong magic/version/short). */
int       dart_detail_kind(DartBytes dgram);
uint16_t  dart_detail_domain(DartBytes dgram);
uint32_t  dart_detail_meta_version(DartBytes dgram);

/* Interest paging (uDTL kinds 3/4): how a peer whose interest list does not fit its
 * announce (INTEREST_EXTERNAL, see the overlay codec above) serves it. The pages carry
 * BYTE RANGES of the exact dart_transport_build_interest blob, so the assembled bytes
 * feed the unchanged dart_transport_apply_peer_interest. The responder is STATELESS
 * (slices its current interest at the requested offset, one sub-datagram page); the
 * requester keeps one cursor per peer: request offset=cursor, append the chunk, re-ask
 * until cursor == total_len. A RESP is stamped with the responder's CURRENT
 * meta_version; a stamp differing from the version being assembled restarts the cursor
 * at 0 (the interest changed mid-fetch). Lost datagrams heal requester-driven, exactly
 * like details (re-ask on the peer's announces + the periodic sweep). Bodies (LE):
 *   REQ  body: [u32 offset]
 *   RESP body: [u32 total_len][u32 offset][u16 chunk_len][chunk bytes]
 * Interest entries are 5 B, so a page can never hit the oversized-single-entry case the
 * detail exchange allows: interest paging is unconditionally sub-datagram. */
#define DART_INTEREST_RESP_HEAD 24u   /* family header (14) + total/offset/chunk_len (10) */

/* Exact bytes of this node's current dart_transport_build_interest output (the
 * total_len a responder advertises; the same walk, byte for byte). */
uint32_t  dart_transport_interest_size(DartTransportState *st);
/* Build an INTEREST_REQ asking for the blob from `offset`. Returns bytes (18), or 0 if
 * cap is too small. peer_meta_version = the version being fetched (advisory: the
 * responder always answers with its current one). */
size_t    dart_interest_req_build(uint16_t domain, uint32_t peer_meta_version,
                       uint32_t offset, void *out, size_t cap);
/* The offset a well-formed INTEREST_REQ asks from: 1 + *offset, else 0. */
int       dart_interest_req_offset(DartBytes dgram, uint32_t *offset);
/* Write one page's header into out[cap] (>= DART_INTEREST_RESP_HEAD); the caller lays
 * the chunk bytes immediately after it. Returns DART_INTEREST_RESP_HEAD, or 0. */
size_t    dart_interest_resp_head(uint16_t domain, uint32_t meta_version, uint32_t total_len,
                       uint32_t offset, uint16_t chunk_len, void *out, size_t cap);
/* Decode a page: 1 + total_len/offset/chunk (a view into dgram, bounds-checked), else 0. */
int       dart_interest_resp_parse(DartBytes dgram, uint32_t *total_len, uint32_t *offset,
                       DartBytes *chunk);

/* Build a DETAIL_REQ for n_wants topics. Returns bytes written, or 0 if cap is too
 * small for all of them (14 + 10 per want): batch per peer, split only if huge. */
size_t    dart_detail_req_build(uint16_t domain, uint32_t peer_meta_version,
                       const DartDetailWant *wants, uint16_t n_wants,
                       void *out, size_t cap);

/* Bytes ONE response PAGE to req takes, for sizing the buffer; 0 if req is malformed.
 * Same walk as dart_transport_detail_respond, byte for byte: the response is capped to a
 * single un-fragmented datagram (DART_DGRAM_MAX) so it never IP-fragments, and the
 * requester re-asks for the indices that did not fit (paging). At least one entry is always
 * included, so a lone entry larger than a datagram rides its own (fragmenting) page rather
 * than wedging paging -- size the buffer to the returned value, which may exceed
 * DART_DGRAM_MAX only in that case. */
size_t    dart_transport_detail_resp_size(DartTransportState *st, const DartMetaSchema *schemas,
                       DartBytes req);
/* Answer req into out[cap]: one entry per requested index this st currently advertises
 * (unknown/INACTIVE indices are skipped), the schema wire inlined only where the
 * request's hash differs from ours. schemas is the same per-topic array
 * dart_transport_meta_build takes (or NULL). meta_version stamps the response (pass the
 * current announce version). Fills ONE datagram, truncating at an entry boundary (the
 * requester re-requests the rest); at least one entry is always emitted, so a lone
 * over-a-datagram entry rides its own page. Size cap via dart_transport_detail_resp_size.
 * Returns bytes written; 0 = malformed req or cap cannot hold the header. Does NOT check
 * the domain: that is the caller's. */
size_t    dart_transport_detail_respond(DartTransportState *st, const DartMetaSchema *schemas,
                       uint32_t meta_version, DartBytes req, void *out, size_t cap);

/* One topic's details, as decoded by dart_detail_next. name/schema_wire point into the
 * source response (NOT NUL-terminated / not owned), so keep that buffer alive. */
typedef struct {
    uint16_t   index;       /* the responder's local topic index */
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
 * hash matches a local topic, whose roles overlap ours, and whose verdict is not yet
 * cached. It fills up to max_wants request entries (each carrying OUR schema hash for
 * the responder's inline-on-mismatch rule) and returns the count: 0 means nothing is
 * pending for this peer (converged). out may be NULL to just count. Re-run it on every
 * announce from the peer while it returns nonzero (lost datagrams heal by re-asking).
 *
 * apply_peer_details ingests a DETAIL_RESP: each entry is verified (full 64-bit identity
 * recomputed from the name; a same-hash different-name peer fires NAME_COLLISION and is
 * refused; the schema_check hook gates both directions) and the verdict cached. Returns
 * the number of newly decided indices; when nonzero, re-run
 * dart_transport_apply_peer_interest with the peer's current interest so the new verdicts
 * form their matches (proxies, catch-up replay) exactly as a fresh announce would. */
uint16_t  dart_transport_detail_wants(DartTransportState *st, const DartMetaSchema *schemas,
                       uint32_t peer_id, DartBytes interest,
                       DartDetailWant *out, uint16_t max_wants);
uint16_t  dart_transport_apply_peer_details(DartTransportState *st, uint32_t peer_id,
                       DartBytes resp);

/* Unresolved candidates for ONE topic in a peer's advertised interest: entries whose
 * 32-bit hash nominates this topic and whose role could pair with ours, but whose detail
 * verdict has not arrived yet. >0 = a match with this peer may still form with no further
 * action here (the detail request/response cycle is in flight); 0 = every entry this peer
 * advertises is decided for this topic. The topic-scoped slice of detail_wants; feeds the
 * node's send-path match wait and dart_topic_pending_count. */
uint16_t  dart_transport_topic_unresolved(DartTransportState *st, uint16_t topic_index,
                       uint32_t peer_id, DartBytes interest);

/* Change a topic's role at runtime (rematches peers locally; caller re-advertises
 * interest). A (re)subscribe joins like a late joiner. Returns 0 ok, <0 unknown. */
int       dart_transport_set_role(DartTransportState *st, uint16_t topic_index, uint8_t role);

/* Define a reserved (currently inactive) topic slot at runtime: set its name/qos/
 * role, allocate its history ring via the allocator, and rematch known peers.
 * Returns 0 ok, or negative: -1 bad index / name / slot already defined, -4 out of
 * memory. Re-advertise interest after (the node bumps its discovery announce). */
int       dart_transport_topic_define(DartTransportState *st, uint16_t topic_index, const DartTopicDef *def);

/* The topic's topic name ({NULL,0} if undefined or out of range), for surfacing it on a
 * delivered message. Not NUL-terminated: use .data/.len. The name is a local lookup; it is
 * never on the data path. */
DartString dart_transport_topic_name(DartTransportState *st, uint16_t topic_index);

/* 1 if the messages this peer publishes on topic_index carry the DART_TIMESTAMP_BYTES
 * source stamp (the default), 0 if it advertised the opt-out (DartQos.no_timestamp) in its
 * interest. An unknown peer / unmatched lane answers 1 (the default framing). The receiving
 * side asks per delivered message so it strips exactly what the writer prepended. */
int       dart_transport_peer_timestamped(DartTransportState *st, uint16_t topic_index,
                       uint32_t peer_id);

/* dart_transport_send / dart_transport_send_shm result: 0 ok, negative on error (returned as int). */
typedef enum {
    DART_OK             =  0,
    DART_ERR_NO_TOPIC = -1,  /* topic index out of range */
    DART_ERR_TOO_BIG    = -2,  /* exceeds the wire fragment cap (DART_MESSAGE_MAX) */
    DART_ERR_ROLE       = -3,  /* topic is SUB_ONLY or INACTIVE: cannot publish */
    DART_ERR_OOM        = -4,  /* allocator returned NULL */
    DART_ERR_STATE      = -5,  /* wrong state: poll while a service thread runs, start while
                                  started, or a call not allowed from inside a callback */
    DART_ERR_NOSYS      = -6   /* not compiled in (dart_node_start with DART_THREADS off) */
} DartResult;

/* Publish a message to all peers. Returns DART_OK, or a negative DartResult. */
int       dart_transport_send(DartTransportState *st, uint16_t topic_index, DartBytes data, uint64_t now_us);

/* Matched subscribers excluding dormant peers: the liveness-sensitive variant of
 * dart_transport_publisher_match_count (which keeps counting a dropped-but-resumable peer).
 * O(matched lanes). */
int       dart_transport_publisher_live_matches(DartTransportState *st, uint16_t topic_index);
/* Matched publishers feeding OUR subscription side of this topic (the mirror count). */
int       dart_transport_subscriber_match_count(DartTransportState *st, uint16_t topic_index);

/* Publish hdr followed by data as one message (the pattern-header gather; see
 * DartTopicDef.prefix_bytes). hdr rides in front of the payload on the wire, byte-identical
 * to a plain send of the concatenation. hdr {NULL,0} == dart_transport_send. Same return. */
int       dart_transport_send_hdr(DartTransportState *st, uint16_t topic_index,
                      DartBytes hdr, DartBytes data, uint64_t now_us);
/* Publish hdr+data to ONE peer (unicast point-to-point): only that peer's lane carries the
 * message; every other matched reliable lane is advanced past its seqno and skipped via its
 * HB floor (no cross-delivery, no repair traffic, no MSG_LOST when the topic is `directed`).
 * The topic shares one seqno line, so a directed send still consumes a seqno everywhere.
 * No-op delivery if the peer is not a matched subscriber. Same return as dart_transport_send. */
int       dart_transport_send_to(DartTransportState *st, uint16_t topic_index, uint32_t to_peer,
                      DartBytes hdr, DartBytes data, uint64_t now_us);

#ifdef DART_SHM
/* Publish a message whose payload lives in an external shared-memory buffer: the
 * transport stores the message referencing chunk (NOT copied) plus the descriptor,
 * fragments from chunk for non-SHM peers, and sends ONE SHM-DATA (the descriptor) to
 * SHM-capable peers. desc is DART_SHM_DESC_BYTES. Same return as dart_transport_send. The chunk
 * must stay valid until the message leaves history (acked / evicted). chunk is the whole
 * wire sample, so on a stamped topic the CALLER writes the DART_TIMESTAMP_BYTES source
 * stamp (then any pattern header, then the payload) into it. */
int       dart_transport_send_shm(DartTransportState *st, uint16_t topic_index, DartBytes chunk,
                      const uint8_t *desc, uint64_t now_us);
/* Mark whether a peer can receive SHM-DATA (same host AND its segment is attached).
 * Off by default; the node sets it on attach, clears it on dormant/remove. */
void      dart_transport_peer_set_shm(DartTransportState *st, uint32_t peer_id, int is_shm);
/* 1 if every matched subscriber of topic is SHM-capable, so a publish may go via SHM
 * (else inline). The node checks this per message. */
int       dart_transport_publisher_shm_eligible(DartTransportState *st, uint16_t topic_index);
/* The history slot the next publish to topic will occupy (binds chunk<->slot). */
uint16_t  dart_transport_topic_hist_head(DartTransportState *st, uint16_t topic_index);
#endif

/* The topic's qos as stored at init; NULL if unknown. */
const DartQos *dart_transport_topic_qos(DartTransportState *st, uint16_t topic_index);

/* 1 if appending here would overwrite history not yet acked by every subscriber. A
 * publisher pumps while this is 1, then sends anyway after qos.backpressure_wait_us. */
int       dart_transport_send_would_evict(DartTransportState *st, uint16_t topic_index);

/* 1 if appending here would overwrite history some matched, live subscriber was never
 * HANDED TO THE WIRE (committed but not yet emitted by poll_send). Unlike
 * would_evict this applies to best-effort lanes too: it detects a send burst
 * outrunning the TX drain, not slow-subscriber flow control. The node waits on it so
 * a send cannot silently vaporize data that never left the process. On 1, the
 * evicted message's base seqno / fragment count are written to the (NULLable) outs. */
int       dart_transport_send_would_evict_unsent(DartTransportState *st, uint16_t topic_index,
                                                 uint64_t *evict_base, uint32_t *evict_count);

/* 1 if every live subscriber has acked all messages on this reliable topic (so a
 * publisher may close without truncating). Best-effort/unknown return 1. Wrapped as
 * dart_topic_drain. */
int       dart_transport_send_drained(DartTransportState *st, uint16_t topic_index);

/* Peers currently matched as subscribers (subscribers) of this topic. 0 = a publish
 * goes nowhere; a one-shot publisher can poll this before sending. */
int       dart_transport_publisher_match_count(DartTransportState *st, uint16_t topic_index);

/* Per-peer match summary (diagnostic): how many topics we now PUBLISH to this peer
 * (it subscribes and we publish) and how many we RECEIVE from it (it publishes and we
 * subscribe). Counts unicast lanes; either out-pointer may be NULL, both 0 for an
 * unknown peer. Surfaced on DART_PEER_INTEREST so a caller can watch a connection form. */
void      dart_transport_peer_match_counts(DartTransportState *st, uint32_t peer_id,
                                 uint16_t *publish_to, uint16_t *receive_from);

/* Cumulative reliable-repair counters for a topic, summed over its peer/subscriber
 * proxies (publisher side = this node publishing; subscriber side = subscribing). Always on;
 * each field is a plain bump on a path that already runs. The per-second deltas of
 * frags_resent (publisher) and non-dup frags_recv (subscriber) are repair throughput; a flat
 * HOL snapshot (dart_transport_subscriber_progress) with rising nacks_sent is a wedged stream. */
typedef struct {
    /* publisher side (node as publisher) */
    uint64_t nacks_recv;     /* ACKNACKs received that requested missing fragments (nbits>0) */
    uint64_t frags_resent;   /* DATA fragments retransmitted to satisfy a NACK */
    uint64_t frags_sent;     /* all DATA fragments sent (new + repair); repair fraction = resent/sent */
    /* subscriber side (node as subscriber) */
    uint64_t nacks_sent;     /* repair requests we emitted (ACKNACK with nbits>0) */
    uint64_t frags_recv;     /* all DATA fragments received, including duplicates */
    uint64_t frags_dup;      /* fragments received that we already held (repair overlap / waste) */
    uint64_t msgs_skipped;   /* messages given up on (sum of DART_MSG_LOST counts) */
    /* repair-arm attribution (diagnostic): each counts a 0->1 arming of the subscriber's
       pending-ACK, by what triggered it. arms_data = a DATA/SHM-DATA arrival re-armed
       it; arms_hb = a heartbeat did. A stall where arms_hb ticks at the heartbeat rate
       while arms_data is flat means the subscriber only re-asks on arrivals, not on a timer. */
    uint64_t arms_data;
    uint64_t arms_hb;
    /* RX disposition of received DATA fragments (diagnostic): every DATA fragment that
       reaches the subscriber is one of these. frags_recv counts ACCEPTED only (base ==
       deliver_upto), so "recv 0" while the publisher floods can mean the fragments are
       landing but being rejected as old/ahead, not that they aren't arriving. */
    uint64_t frags_old;        /* base < deliver_upto: whole message already delivered/skipped */
    uint64_t frags_ahead;      /* base > deliver_upto: a future message (no out-of-order buffer) */
    uint64_t frags_malformed;  /* count==0 || frag>=count, or not subscribed */
} DartRepairStats;

/* Fill *out with the topic's cumulative repair counters (zeroed if topic is
 * out of range). Per-topic aggregate; a per-peer breakdown is a later extension. */
void      dart_transport_repair_stats(DartTransportState *st, uint16_t topic_index, DartRepairStats *out);

/* Publisher-side: number of subscriber lanes on this topic with a pending repair NACK to
 * service. 0 => the publisher has nothing to resend right now (idle for lack of NACKs).
 * Diagnostic for the backpressure stall (distinguishes "no NACKs" from "resends
 * dropped"); sampled by the node's in-pump probe. */
int       dart_transport_repair_pending(DartTransportState *st, uint16_t topic_index);

/* Head-of-line reassembly snapshot for the in-progress message from `peer` on
 * `topic` (the message at the subscriber's deliver_upto). Returns 1 and fills the
 * out-params if a message is mid-reassembly, else 0.
 *   base_seqno : first seqno of the in-progress message (= subscriber deliver_upto)
 *   have       : fragments received so far (popcount of the reassembly bitmap)
 *   total      : fragments the message needs
 * `have` rising across calls => repair is crawling forward; flat => wedged. Any
 * out-pointer may be NULL. Wrapped as dart_topic_subscriber_progress. */
int       dart_transport_subscriber_progress(DartTransportState *st, uint16_t topic_index, uint32_t peer,
                            uint64_t *base_seqno, uint32_t *have, uint32_t *total);

/* Retry delivery of samples PARKED after a refused on_message/on_shm (see DartMessageFn):
 * re-attempts each parked lane of the topic, advancing + arming the ack for every message
 * now accepted. Call it when downstream capacity frees (the node calls it as its consumer
 * queue drains), then flush poll_send so the acks reach the publisher. Returns the number of
 * lanes still parked (0 = fully drained). */
uint32_t  dart_transport_deliver_parked(DartTransportState *st, uint16_t topic_index, uint64_t now_us);

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

/* 1 while any lane holds work dart_transport_poll_send would hand out. The "is there
 * anything to flush?" test for a caller that must decide whether to wake a sleeping
 * poller: a send that committed nothing (no matched subscriber, so the send early-outs)
 * leaves the poller nothing to do. O(1). */
int       dart_transport_tx_pending(DartTransportState *st);

#ifdef __cplusplus
}
#endif
#endif /* DART_TRANSPORT_H */
