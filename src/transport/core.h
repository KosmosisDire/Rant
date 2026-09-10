/* The sans-IO transport core. Feed it datagrams, the clock and a peer set, it returns
 * datagrams to send and delivers messages. The rules are in spec/transport.md. */
#ifndef DART_TRANSPORT_H
#define DART_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>
#include "../common/string.h"
#include "../common/alloc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* DART_SHM detection. This block mirrors platform/core.h exactly. Edit both together. */
#if !defined(DART_SHM) && !defined(DART_NO_SHM)
  #if defined(_WIN32) || defined(__linux__) || defined(__APPLE__) || \
      defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
      defined(__DragonFly__)
    #define DART_SHM
  #endif
#endif
#if defined(DART_SHM) && defined(DART_NO_SHM)
  #undef DART_SHM              /* both set, the opt out wins */
#endif

#ifndef DART_FRAG_SIZE
#define DART_FRAG_SIZE 1350u          /* default message bytes per fragment */
#endif
/* A node fragments every send with one size, set at init and advertised by discovery.
 * MIN and MAX bound any node's size, and MAX sizes the datagram buffers. */
#ifndef DART_FRAG_SIZE_MAX
#define DART_FRAG_SIZE_MAX DART_FRAG_SIZE
#endif
#ifndef DART_FRAG_SIZE_MIN
#define DART_FRAG_SIZE_MIN DART_FRAG_SIZE
#endif
#define DART_DGRAM_MAX (DART_FRAG_SIZE_MAX + 40u)   /* plus the largest header */

#ifdef DART_SHM
#define DART_SHM_DESC_BYTES 24u   /* the descriptor on the wire, equals DART_SHM_DESC_WIRE */
#endif

#ifndef DART_TOPIC_NAME_MAX
#define DART_TOPIC_NAME_MAX 64u          /* topic name bytes on the wire */
#endif

#ifndef DART_NODE_NAME_MAX
#define DART_NODE_NAME_MAX 32u           /* node name bytes in the announce */
#endif

/* The source stamp in front of every sample of a stamped topic, taken once at commit so
 * repair, replay and shared memory carry the original. DartQos.no_timestamp opts out. */
#define DART_TIMESTAMP_BYTES 8u
/* A microsecond wall clock needs 51 bits, so the source stamp's top bit is free to mark
 * that a capture slot follows it. Per message, and costs nothing unset. See spec/node.md */
#define DART_CAPTURE_BYTES   8u
#define DART_STAMP_CAPTURE   0x8000000000000000ull
#define DART_STAMP_MASK      0x7FFFFFFFFFFFFFFFull

typedef enum { DART_BEST_EFFORT = 0, DART_RELIABLE = 1 } DartReliability;
/* DART_INACTIVE is declared but off. Resources stay allocated and set_role flips it. */
typedef enum { DART_PUBSUB = 0, DART_PUB_ONLY = 1, DART_SUB_ONLY = 2,
               DART_INACTIVE = 3 } DartRole;
/* Shared by every matching, announce and reflection walk, so a flipped test is impossible. */
static inline int dart_role_pubs(uint8_t role){ return role == DART_PUBSUB || role == DART_PUB_ONLY; }
static inline int dart_role_subs(uint8_t role){ return role == DART_PUBSUB || role == DART_SUB_ONLY; }

/* What a topic carries. A same name under a different kind is a disjoint entity and the
 * pairing is refused. The kind rides the interest flags and is immutable per topic. */
typedef enum {
    DART_KIND_TOPIC    = 0,   /* plain pub sub */
    DART_KIND_FUNC_REQ = 1,   /* function requests, the caller pubs */
    DART_KIND_FUNC_RSP = 2,   /* function responses, the provider pubs, directed */
    DART_KIND_VARIABLE = 3,   /* variable values, the owner pubs */
    DART_KIND_VAR_SET  = 4,   /* variable sets, writers pub, the owner subs */
    DART_KIND_TASK_REQ = 5,   /* task requests and cancel ops, callers pub */
    DART_KIND_TASK_PRG = 6,   /* task progress, the provider pubs, broadcast */
    DART_KIND_TASK_RSP = 7    /* task responses, the provider pubs, directed */
} DartTopicKind;

/* Immutable per topic facts served in the detail exchange and cached per (peer, index).
 * NO_TIMESTAMP is derived from the qos, the rest come from DartTopicDef.attrs. */
#define DART_ATTR_NO_TIMESTAMP 0x01u /* the publisher sends no source stamp */
#define DART_ATTR_MULTI        0x02u /* authority kinds: duplicate authority is intended */
#define DART_ATTR_FORCEABLE    0x04u /* variable values: the owner permits force */
#define DART_ATTR_CANCELLABLE  0x08u /* task requests: the provider honors cancel */
#define DART_ATTR_EXCLUSIVE    0x10u /* task requests: declared serialization */

/* Every field except reliability is zero means default. docs/topics.md explains each. */
typedef struct {
    DartReliability reliability;
    uint16_t keep_last;          /* messages retained. 0 = 1, or 10 on a reliable topic */
    uint16_t catch_up;           /* messages a new subscriber gets at once. 0 = future only */
    uint32_t max_message_bytes;  /* a size hint that pins the SHM class, never a cap */
    uint32_t heartbeat_us;       /* reliable idle publisher ping. 0 = 250 ms */
    uint32_t repair_delay_us;    /* the reliable re ask bound, 0 = adaptive from the round trip */
    uint32_t backpressure_wait_us;/* reliable send pause for a slow subscriber. 0 = none */
    uint32_t shm_max_bytes;      /* pin the topic to one SHM class. 0 = per message */
    uint32_t queue_bytes;        /* the consumer queue capacity, 0 = lazy up to DART_QUEUE_CAP */
    uint16_t max_rate_hz;        /* subscriber side, best effort: a delivery cap per publisher */
    uint8_t  no_timestamp;       /* publisher side: no source stamp, receivers see written_us 0 */
} DartQos;

/* The name is the cross peer identity. The local handle is the index in topics[]. */
typedef struct {
    const char *name;  /* required, the same on every node, at most DART_TOPIC_NAME_MAX */
    DartQos   qos;
    uint8_t  role;     /* DartRole, 0 = pub and sub */
    uint8_t  kind;     /* DartTopicKind, the patterns layer sets the rest */
    uint8_t  attrs;    /* DART_ATTR_* declared by the definer. NO_TIMESTAMP comes from the qos */
    uint8_t  prefix_bytes; /* pattern header bytes ahead of the payload, split off on receive */
    uint8_t  directed; /* 1 = point to point sends. Other lanes skip the seqno with no MSG_LOST */
} DartTopicDef;

/* Return 0 delivered. Nonzero refuses: a reliable subscriber parks the sample with no
 * ack, so the publisher's flow control carries the backpressure. Best effort drops it. */
typedef int (*DartMessageFn)(void *user, uint16_t topic_index, uint32_t from_peer, DartBytes data);

#ifdef DART_SHM
/* SHM delivery hands the node the descriptor. 1 delivered, 0 unresolvable (the reliability
 * layer repairs or skips), -1 refused (parked like an inline refusal). */
typedef int (*i_DartShmMsgFn)(void *user, uint16_t topic_index, uint32_t from_peer,
                             const uint8_t *desc);
#endif

/* The transport's own events. The node maps them into its DartEvent. */
typedef enum {
    DART_TRANSPORT_MSG_LOST,        /* seqnos skipped: .topic, .peer, .lost_first, .lost_count */
    DART_TRANSPORT_MSG_TOO_BIG,     /* a received message could not be buffered, .too_big_bytes */
    DART_TRANSPORT_NAME_COLLISION,  /* a peer's name hashes to ours but differs, .identity */
    DART_TRANSPORT_QOS_INCOMPATIBLE,/* a reliable subscriber refused a best effort publisher */
    DART_TRANSPORT_SCHEMA_MISMATCH, /* the schema_check hook refused a direction, .peer_is_pub */
    DART_TRANSPORT_INTEREST_OVERFLOW,/* a peer's index map failed to allocate: .lost_count */
    DART_TRANSPORT_META_TRUNCATED_INTEREST, /* our overlay overflowed, peers see no topics */
    DART_TRANSPORT_META_TRUNCATED_SCHEMA,   /* retired, kept so binding enums stay aligned */
    DART_TRANSPORT_KIND_MISMATCH    /* a verified peer advertises the name under another kind */
} DartTransportEventKind;

/* Read only the fields named for the kind. The topic name comes from dart_transport_topic_name. */
typedef struct {
    DartTransportEventKind kind;
    void       *user;          /* DartConfig.user */
    uint32_t   peer;           /* 0 = none */
    uint16_t   topic;        /* local topic handle */
    uint64_t   lost_first;     /* MSG_LOST: first skipped seqno */
    uint64_t   lost_count;     /* MSG_LOST: seqnos skipped, fragments not messages */
    uint64_t   too_big_bytes;  /* MSG_TOO_BIG: size of the dropped message */
    uint64_t   identity;       /* NAME_COLLISION: the colliding identity */
    uint8_t    peer_is_pub;    /* SCHEMA_MISMATCH: the refused direction, 1 = their publisher */
} DartTransportEvent;
typedef void (*DartTransportEventFn)(const DartTransportEvent *ev);

/* Largest message the wire can carry, 65535 fragments. */
#define DART_MESSAGE_MAX (65535u * DART_FRAG_SIZE_MAX)

/* DartConfig.allocator is required. Every growable buffer sizes through it, so memory
 * scales with traffic and matches. Static mode passes dart_allocator_alloc over a static one. */

/* topics set: defined at init. topics NULL: n_topics reserved slots, all INACTIVE, filled
 * later by dart_transport_topic_define. */
typedef struct {
    const DartTopicDef *topics;     /* NULL = reserve mode */
    uint16_t              n_topics;   /* defined count, or the reserved capacity */
    uint16_t              max_peers;
    uint16_t              frag_size; /* our fragment size, 0 = DART_FRAG_SIZE, clamped */
    DartMessageFn         on_message;
#ifdef DART_SHM
    i_DartShmMsgFn         on_shm;     /* SHM-DATA delivery, the node resolves the descriptor */
#endif
    DartTransportEventFn  on_event;   /* optional */
    DartAllocFn           allocator;  /* required, init fails without it */
    /* Optional schema gate at detail intake, once per direction. peer_is_pub 1 gates the
     * read side. Return 1 to allow. Verdicts are cached per (peer, index). */
    int                 (*schema_check)(void *user, uint32_t peer, uint16_t topic_index,
                                        int peer_is_pub, uint64_t schema_hash,
                                        DartBytes schema_wire);
    /* Optional wall clock, UTC microseconds, stamped at commit. NULL still frames the 8
     * bytes as 0 on a stamped topic. Never the monotonic clock, the stamp crosses hosts. */
    uint64_t            (*source_time)(void *user);
    void                 *user;
} DartConfig;

typedef struct DartTransportState DartTransportState;

size_t    dart_transport_required_memory(const DartConfig *cfg);
DartTransportState *dart_transport_init(void *mem, size_t mem_size, const DartConfig *cfg);
/* Relocates a live transport, re striding its tables and carrying reliability state. The
 * caller frees the old arena block but must not destroy old. NULL leaves old intact. */
DartTransportState *dart_transport_migrate(DartTransportState *old, void *new_mem, size_t new_cap,
                        uint16_t new_max_peers, uint16_t new_n_topics);
/* Frees every hook allocation. The arena stays the caller's. */
void      dart_transport_destroy(DartTransportState *st);

/* The FNV-1a identity of a name. */
uint64_t  dart_topic_id(const char *name);
uint64_t  dart_topic_identity(const DartTopicDef *def);   /* dart_topic_id(def->name) */

/* 0 to DART_FRAG_SIZE, then clamp to [MIN, MAX]. Shared with the node's announce. */
uint16_t  dart_clamp_frag(uint16_t frag_size);

/* Our fragment size. The node uses it as the SHM cutoff. */
uint16_t  dart_transport_frag(DartTransportState *st);

/* A new peer matches nothing until apply_peer_interest. peer_frag is its advertised
 * fragment size, 0 = DART_FRAG_SIZE, clamped to [MIN, MAX]. */
void      dart_transport_peer_add   (DartTransportState *st, uint32_t peer_id, uint16_t peer_frag);
void      dart_transport_peer_remove(DartTransportState *st, uint32_t peer_id);

/* A silent peer goes DORMANT: out of flow control, proxies and positions kept. resume re
 * includes it and re reports reader positions. Both no ops for an unknown peer. */
void      dart_transport_peer_dormant(DartTransportState *st, uint32_t peer_id);
void      dart_transport_peer_resume (DartTransportState *st, uint32_t peer_id);
/* A peer's blob may arrive after first contact. Clamped, a no op for an unknown peer. */
void      dart_transport_peer_set_frag(DartTransportState *st, uint32_t peer_id, uint16_t peer_frag);

/* The interest exchange. The list is hash only and positional, a hash overlap only
 * nominates, and the detail exchange below verifies. See spec/interest.md. */
size_t    dart_interest_max(uint16_t n_topics);
size_t    dart_transport_build_interest(DartTransportState *st, void *out, size_t cap);
void      dart_transport_apply_peer_interest(DartTransportState *st, uint32_t peer_id, DartBytes blob);

/* The announce overlay: a version prefix (fragment size, SHM capability and host) plus
 * the interest list, or the INTEREST_EXTERNAL bootstrap when it does not fit one datagram. */

/* One topic's schema advertisement, served by the detail responder. hash 0 = none. */
typedef struct {
    uint64_t  hash;
    DartBytes wire;
} DartMetaSchema;

/* Worst case overlay bytes for n_topics, capped to one datagram. Sizes discovery's meta_cap. */
uint16_t  dart_meta_cap(uint16_t n_topics);
/* Exact bytes the next inline build emits, so a caller sizes to content. */
uint16_t  dart_transport_meta_size(DartTransportState *st);
uint16_t  dart_transport_meta_bootstrap_size(void);
/* Builds the overlay. interest_external writes the bootstrap only. The node inlines while
 * the whole announce fits one datagram. Returns the bytes. */
uint16_t  dart_transport_meta_build(DartTransportState *st, uint8_t *out, uint16_t cap,
                       uint16_t frag_size, int shm_capable, const uint8_t host[16],
                       int interest_external);
/* 0 if malformed. */
uint16_t  dart_meta_frag(DartBytes meta);
/* 1 = the peer serves its interest through paging. */
int       dart_meta_interest_external(DartBytes meta);
/* {NULL, 0} if absent or external, so a bootstrap never reads as an empty interest list. */
DartBytes dart_meta_interest(DartBytes meta);

/* One advertised direction of a topic. A PUBSUB topic yields twice, pub first. */
typedef struct {
    uint16_t    index;      /* the advertiser's topic index */
    uint8_t     role;       /* the advertiser's DartRole */
    uint8_t     is_pub;     /* 1 = the publish direction, 0 = subscribe */
    uint8_t     reliable;   /* offered or requested reliability */
    uint8_t     kind;       /* the advertiser's DartTopicKind */
    uint32_t    hash;       /* low 32 bits of the identity */
} DartTopicEntry;

/* Zero initialize, then call until 0. Internal walk state. */
typedef struct {
    uint32_t off;        /* byte offset of the next entry */
    uint16_t left;       /* entries still to walk */
    uint16_t index;      /* position of the next entry */
    uint8_t  phase;      /* 1 = the current entry's pub direction was yielded */
    uint8_t  started;    /* 0 until the first call parses the header */
} DartInterestIter;

/* Walks one direction at a time and stops at the end or at a malformed blob. interest_next
 * walks a bare blob, meta_interest_next the one inside an overlay. */
int       dart_interest_next(DartBytes interest, DartInterestIter *it, DartTopicEntry *out);
int       dart_meta_interest_next(DartBytes meta, DartInterestIter *it, DartTopicEntry *out);
#ifdef DART_SHM
/* 1 and host filled when the peer is SHM capable. */
int       dart_meta_shm(DartBytes meta, uint8_t host[16]);
#endif

/* The pairwise detail exchange, uDTL. The responder is stateless and answers the
 * request's source. The layout is in spec/interest.md. */
#define DART_DETAIL_REQ    1
#define DART_DETAIL_RESP   2
#define DART_INTEREST_REQ  3
#define DART_INTEREST_RESP 4

/* The peer's index plus our schema hash for it, 0 = none. */
typedef struct {
    uint16_t index;
    uint64_t schema_hash;
} DartDetailWant;

/* Safe on any buffer. kind is 0 for anything that is not a well formed uDTL header. */
int       dart_detail_kind(DartBytes dgram);
uint16_t  dart_detail_domain(DartBytes dgram);
uint32_t  dart_detail_meta_version(DartBytes dgram);

/* Interest paging carries byte ranges of the build_interest blob. The requester keeps one
 * cursor per peer and a changed version restarts it. See spec/interest.md. */
#define DART_INTEREST_RESP_HEAD 24u   /* the family header plus total, offset and chunk_len */

/* Exact bytes of our current interest blob, the total_len paging serves. */
uint32_t  dart_transport_interest_size(DartTransportState *st);
/* 18 bytes, or 0 if cap is too small. peer_meta_version is advisory. */
size_t    dart_interest_req_build(uint16_t domain, uint32_t peer_meta_version,
                       uint32_t offset, void *out, size_t cap);
/* 1 and *offset for a well formed INTEREST_REQ, else 0. */
int       dart_interest_req_offset(DartBytes dgram, uint32_t *offset);
/* Writes one page header. The caller lays the chunk right after it. */
size_t    dart_interest_resp_head(uint16_t domain, uint32_t meta_version, uint32_t total_len,
                       uint32_t offset, uint16_t chunk_len, void *out, size_t cap);
/* 1 and the fields, chunk a bounds checked view into dgram, else 0. */
int       dart_interest_resp_parse(DartBytes dgram, uint32_t *total_len, uint32_t *offset,
                       DartBytes *chunk);

/* 0 if cap cannot hold every want (14 plus 10 each). Batch per peer. */
size_t    dart_detail_req_build(uint16_t domain, uint32_t peer_meta_version,
                       const DartDetailWant *wants, uint16_t n_wants,
                       void *out, size_t cap);

/* Bytes one response page takes, 0 if req is malformed. One datagram, except that a lone
 * oversized entry rides its own page. */
size_t    dart_transport_detail_resp_size(DartTransportState *st, const DartMetaSchema *schemas,
                       DartBytes req);
/* Answers req into out with one entry per advertised requested index, the wire inlined
 * only where the hashes differ. Truncates at an entry boundary. Does not check the domain. */
size_t    dart_transport_detail_respond(DartTransportState *st, const DartMetaSchema *schemas,
                       uint32_t meta_version, DartBytes req, void *out, size_t cap);

/* One topic's details. name and schema_wire point into the response. */
typedef struct {
    uint16_t   index;       /* the responder's local topic index */
    uint8_t    attrs;       /* the topic's DART_ATTR_* byte */
    DartString name;
    uint64_t   schema_hash; /* 0 = untyped */
    DartBytes  schema_wire; /* only when the request's hash differed, else {NULL,0} */
} DartDetail;

/* Zero initialize, then call until 0. */
typedef struct {
    uint32_t off;      /* byte offset of the next entry */
    uint16_t left;     /* entries still to yield */
    uint8_t  started;  /* 0 until the first call parses the header */
} DartDetailIter;

/* Walks a DETAIL_RESP. Stops at the end or at a malformed response. */
int       dart_detail_next(DartBytes resp, DartDetailIter *it, DartDetail *out);

/* The pending candidates of a peer, up to max_wants (out NULL just counts). Re run it on
 * every announce from the peer while it returns nonzero. */
uint16_t  dart_transport_detail_wants(DartTransportState *st, const DartMetaSchema *schemas,
                       uint32_t peer_id, DartBytes interest,
                       DartDetailWant *out, uint16_t max_wants);
/* Ingests a DETAIL_RESP and caches the verdicts. Returns the newly decided count, after
 * which the caller re applies the peer's interest so the matches form. */
uint16_t  dart_transport_apply_peer_details(DartTransportState *st, uint32_t peer_id,
                       DartBytes resp);

/* Entries of a peer that nominate this topic and are still undecided. Feeds the match wait. */
uint16_t  dart_transport_topic_unresolved(DartTransportState *st, uint16_t topic_index,
                       uint32_t peer_id, DartBytes interest);
/* The bulk form: every topic's unresolved count against one peer in a single walk. */
void      dart_transport_peer_unresolved_fill(DartTransportState *st, uint32_t peer_id,
                       DartBytes interest, uint16_t *counts, uint16_t n);

/* Rematches peers locally. The caller re advertises. 0 ok, negative unknown. */
int       dart_transport_set_role(DartTransportState *st, uint16_t topic_index, uint8_t role);

/* Defines a reserved slot: name, qos, role, a history ring and a rematch. 0 ok, -1 bad
 * index, name or slot, -4 out of memory. Re advertise after. */
int       dart_transport_topic_define(DartTransportState *st, uint16_t topic_index, const DartTopicDef *def);

/* Slot lifecycle, see spec/interest.md. retire parks a defined slot: 0 ok, -1 unknown or
 * already retired. Re advertise after. */
int       dart_transport_topic_retire(DartTransportState *st, uint16_t topic_index);
/* The slot a new define should reuse: 2 = a retired slot with the same identity and
 * kind, 1 = another retired slot, 0 = none. */
int       dart_transport_topic_reuse_find(DartTransportState *st, const char *name, uint8_t kind,
                       uint16_t *index_out);
/* Re defines a retired slot. binding_changed bumps the generation and holds writer lanes
 * until each peer names rebind_version in a request. 0 ok, -1 refused, -4 out of memory. */
int       dart_transport_topic_reuse(DartTransportState *st, uint16_t topic_index,
                       const DartTopicDef *def, int binding_changed, uint32_t rebind_version);
/* Records the highest version of our blob a peer named in a request. 1 when a held lane formed. */
int       dart_transport_peer_seen_version(DartTransportState *st, uint32_t peer_id, uint32_t version);

/* The next write seqno. It continues across retire and reuse. */
uint64_t  dart_transport_topic_seqno(DartTransportState *st, uint16_t topic_index);

/* {NULL,0} if undefined. A local lookup, never on the data path. */
DartString dart_transport_topic_name(DartTransportState *st, uint16_t topic_index);

/* 0 if the peer advertised no_timestamp for this topic. An unknown peer answers 1. */
int       dart_transport_peer_timestamped(DartTransportState *st, uint16_t topic_index,
                       uint32_t peer_id);

/* The peer's cached attrs byte for its topic, 0 until its details arrive. */
uint8_t   dart_transport_peer_attrs(DartTransportState *st, uint32_t peer_id,
                       uint16_t their_index);

/* 0 ok, negative on error. */
typedef enum {
    DART_OK             =  0,
    DART_ERR_NO_TOPIC = -1,  /* topic index out of range */
    DART_ERR_TOO_BIG    = -2,  /* exceeds DART_MESSAGE_MAX */
    DART_ERR_ROLE       = -3,  /* the topic is SUB_ONLY or INACTIVE */
    DART_ERR_OOM        = -4,  /* the allocator returned NULL */
    DART_ERR_STATE      = -5,  /* wrong state, or a call not allowed from inside a callback */
    DART_ERR_NOSYS      = -6   /* not compiled in */
} DartResult;

/* Publishes to every matched subscriber. */
int       dart_transport_send(DartTransportState *st, uint16_t topic_index, DartBytes data, uint64_t now_us);

/* Matched subscribers excluding dormant peers. O(matched lanes). */
int       dart_transport_publisher_live_matches(DartTransportState *st, uint16_t topic_index);
/* Is the peer a matched subscriber lane of this topic. The patterns layer's directed backstop. */
int       dart_transport_publisher_peer_matched(DartTransportState *st, uint16_t topic_index,
                                                uint32_t peer_id);
/* The oldest live matched subscriber, 0 = none. The auto direct target for task requests. */
uint32_t  dart_transport_publisher_oldest_match(DartTransportState *st, uint16_t topic_index);
/* Matched publishers feeding our subscription side. */
int       dart_transport_subscriber_match_count(DartTransportState *st, uint16_t topic_index);

/* hdr rides in front of data as one message, byte identical to sending the concatenation.
 * capture_us 0 sends no capture slot. */
int       dart_transport_send_hdr(DartTransportState *st, uint16_t topic_index,
                      DartBytes hdr, DartBytes data, uint64_t capture_us, uint64_t now_us);
/* Publishes to one peer. Every other matched reliable lane skips the seqno through its
 * HB floor. A no op delivery when the peer is not a matched subscriber. */
int       dart_transport_send_to(DartTransportState *st, uint16_t topic_index, uint32_t to_peer,
                      DartBytes hdr, DartBytes data, uint64_t capture_us, uint64_t now_us);

#ifdef DART_SHM
/* The payload lives in an external chunk, fragmented for remote peers and described to
 * SHM peers. The chunk is the whole wire sample and must stay valid until it leaves history. */
int       dart_transport_send_shm(DartTransportState *st, uint16_t topic_index, DartBytes chunk,
                      const uint8_t *desc, uint64_t now_us);
/* Whether a peer can receive SHM-DATA. The node sets it on attach. */
void      dart_transport_peer_set_shm(DartTransportState *st, uint32_t peer_id, int is_shm);
/* 1 if every matched subscriber is SHM capable. */
int       dart_transport_publisher_shm_eligible(DartTransportState *st, uint16_t topic_index);
/* The history slot the next publish occupies, to bind a chunk to it. */
uint16_t  dart_transport_topic_hist_head(DartTransportState *st, uint16_t topic_index);
#endif

/* NULL if unknown. */
const DartQos *dart_transport_topic_qos(DartTransportState *st, uint16_t topic_index);
/* 0 = none or undefined. */
uint8_t   dart_transport_topic_attrs(DartTransportState *st, uint16_t topic_index);

/* 1 if the next send would overwrite history not yet acked by every subscriber. */
int       dart_transport_send_would_evict(DartTransportState *st, uint16_t topic_index);

/* 1 if the next send would overwrite history never handed to the wire, best effort
 * included. Fills the evicted sample's base and count. */
int       dart_transport_send_would_evict_unsent(DartTransportState *st, uint16_t topic_index,
                                                 uint64_t *evict_base, uint32_t *evict_count);

/* 1 if every live subscriber acked everything. Best effort and unknown return 1. */
int       dart_transport_send_drained(DartTransportState *st, uint16_t topic_index);

/* Matched subscribers, dormant included. O(1). */
int       dart_transport_publisher_match_count(DartTransportState *st, uint16_t topic_index);

/* Topics we publish to and receive from this peer. Both 0 for an unknown peer. */
void      dart_transport_peer_match_counts(DartTransportState *st, uint32_t peer_id,
                                 uint16_t *publish_to, uint16_t *receive_from);

/* Cumulative repair counters per topic, summed over its lanes. Always on. */
typedef struct {
    /* publisher side */
    uint64_t nacks_recv;     /* ACKNACKs that requested missing fragments */
    uint64_t frags_resent;   /* DATA fragments retransmitted */
    uint64_t frags_sent;     /* all DATA fragments sent, new and repair */
    /* subscriber side */
    uint64_t nacks_sent;     /* repair requests emitted */
    uint64_t frags_recv;     /* accepted DATA fragments, duplicates included */
    uint64_t frags_dup;      /* fragments already held */
    uint64_t msgs_skipped;   /* the sum of DART_MSG_LOST counts */
    /* each 0 to 1 arming of the subscriber's ack, by its trigger */
    uint64_t arms_data;
    uint64_t arms_hb;
    /* the fate of every received DATA fragment that was not accepted */
    uint64_t frags_old;        /* base below deliver_upto: already delivered or skipped */
    uint64_t frags_ahead;      /* beyond the one sample held ahead: dropped, fetched in order */
    uint64_t frags_malformed;  /* count 0, frag past count, or not subscribed */
} DartRepairStats;

/* Zeroed if the topic is out of range. */
void      dart_transport_repair_stats(DartTransportState *st, uint16_t topic_index, DartRepairStats *out);

/* The per peer round trip estimate, RFC 6298 shape, fed by the reliable path with no
 * probe traffic. samples 0 means no estimate yet. See spec/transport.md. */
typedef struct {
    uint32_t rtt_us;         /* smoothed round trip */
    uint32_t rtt_jitter_us;  /* mean deviation */
    uint32_t rtt_min_us;     /* the smallest sample, the path's floor */
    uint32_t rtt_last_us;    /* the most recent sample */
    uint32_t samples;
} DartPeerRtt;
/* 1 and *out for a known peer, else 0 and *out zeroed. */
int       dart_transport_peer_rtt(DartTransportState *st, uint32_t peer_id, DartPeerRtt *out);

/* Subscriber lanes of this topic with a repair request to service. */
int       dart_transport_repair_pending(DartTransportState *st, uint16_t topic_index);

/* The in progress message from peer: its base seqno, fragments held and fragments needed.
 * 1 if a message is mid reassembly. Any out pointer may be NULL. */
int       dart_transport_subscriber_progress(DartTransportState *st, uint16_t topic_index, uint32_t peer,
                            uint64_t *base_seqno, uint32_t *have, uint32_t *total);

/* Retries the parked samples of a topic when downstream capacity frees, then flush
 * poll_send so the acks go out. Returns the lanes still parked. */
uint32_t  dart_transport_deliver_parked(DartTransportState *st, uint16_t topic_index, uint64_t now_us);

/* Feeds a received datagram tagged with its peer. */
void      dart_transport_on_datagram(DartTransportState *st, uint32_t from_peer, DartBytes datagram,
                         uint64_t now_us);

/* One outgoing datagram, batched per peer. 1 and the outs, or 0. Loop until 0 with a
 * DART_DGRAM_MAX cap. */
int       dart_transport_poll_send(DartTransportState *st, uint32_t *to_peer, void *out, size_t cap,
                       size_t *out_len, uint64_t now_us);

/* The next armed timer, or 0. Cap a blocking poll at it. */
uint64_t  dart_transport_next_deadline_us(DartTransportState *st);

/* 1 while any lane holds work for poll_send. O(1). */
int       dart_transport_tx_pending(DartTransportState *st);

#ifdef __cplusplus
}
#endif
#endif /* DART_TRANSPORT_H */
