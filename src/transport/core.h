/* The sans-IO transport core. Feed it datagrams, the clock and a peer set, it returns
 * datagrams to send and delivers messages. The rules are in spec/transport.md. */
#ifndef RAMBLE_TRANSPORT_H
#define RAMBLE_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>
#include "../common/api.h"
#include "../common/string.h"
#include "../common/alloc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* RAMBLE_SHM detection. This block mirrors platform/core.h exactly. Edit both together. */
#if !defined(RAMBLE_SHM) && !defined(RAMBLE_NO_SHM)
  #if defined(_WIN32) || defined(__linux__) || defined(__APPLE__) || \
      defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
      defined(__DragonFly__)
    #define RAMBLE_SHM
  #endif
#endif
#if defined(RAMBLE_SHM) && defined(RAMBLE_NO_SHM)
  #undef RAMBLE_SHM              /* both set, the opt out wins */
#endif

#ifndef RAMBLE_FRAG_SIZE
#define RAMBLE_FRAG_SIZE 1350u          /* default message bytes per fragment */
#endif
/* A node fragments every send with one size, set at init and advertised by discovery.
 * MIN and MAX bound any node's size, and MAX sizes the datagram buffers. */
#ifndef RAMBLE_FRAG_SIZE_MAX
#define RAMBLE_FRAG_SIZE_MAX RAMBLE_FRAG_SIZE
#endif
#ifndef RAMBLE_FRAG_SIZE_MIN
#define RAMBLE_FRAG_SIZE_MIN RAMBLE_FRAG_SIZE
#endif
#define RAMBLE_DGRAM_MAX (RAMBLE_FRAG_SIZE_MAX + 40u)   /* plus the largest header */

#ifdef RAMBLE_SHM
#define RAMBLE_SHM_DESC_BYTES 24u   /* the descriptor on the wire, equals RAMBLE_SHM_DESC_WIRE */
#endif

#ifndef RAMBLE_TOPIC_NAME_MAX
#define RAMBLE_TOPIC_NAME_MAX 64u          /* topic name bytes on the wire */
#endif

#ifndef RAMBLE_NODE_NAME_MAX
#define RAMBLE_NODE_NAME_MAX 32u           /* node name bytes in the announce */
#endif

/* The source stamp in front of every sample of a stamped topic, taken once at commit so
 * repair, replay and shared memory carry the original. RambleQos.no_timestamp opts out. */
#define RAMBLE_TIMESTAMP_BYTES 8u
/* A microsecond wall clock needs 51 bits, so the source stamp's top bit is free to mark
 * that a capture slot follows it. Per message, and costs nothing unset. See spec/node.md */
#define RAMBLE_CAPTURE_BYTES   8u
#define RAMBLE_STAMP_CAPTURE   0x8000000000000000ull
#define RAMBLE_STAMP_MASK      0x7FFFFFFFFFFFFFFFull

typedef enum { RAMBLE_BEST_EFFORT = 0, RAMBLE_RELIABLE = 1 } RambleReliability;
/* RAMBLE_INACTIVE is declared but off. Resources stay allocated and set_role flips it. */
typedef enum { RAMBLE_PUBSUB = 0, RAMBLE_PUB_ONLY = 1, RAMBLE_SUB_ONLY = 2,
               RAMBLE_INACTIVE = 3 } RambleRole;
/* Shared by every matching, announce and reflection walk, so a flipped test is impossible. */
static inline int ramble_role_pubs(uint8_t role){ return role == RAMBLE_PUBSUB || role == RAMBLE_PUB_ONLY; }
static inline int ramble_role_subs(uint8_t role){ return role == RAMBLE_PUBSUB || role == RAMBLE_SUB_ONLY; }

/* What a topic carries. A same name under a different kind is a disjoint entity and the
 * pairing is refused. The kind rides the interest flags and is immutable per topic. */
typedef enum {
    RAMBLE_KIND_TOPIC    = 0,   /* plain pub sub */
    RAMBLE_KIND_FUNC_REQ = 1,   /* function requests, the caller pubs */
    RAMBLE_KIND_FUNC_RSP = 2,   /* function responses, the provider pubs, directed */
    RAMBLE_KIND_VARIABLE = 3,   /* variable values, the owner pubs */
    RAMBLE_KIND_VAR_SET  = 4,   /* variable sets, writers pub, the owner subs */
    RAMBLE_KIND_TASK_REQ = 5,   /* task requests and cancel ops, callers pub */
    RAMBLE_KIND_TASK_PRG = 6,   /* task progress, the provider pubs, broadcast */
    RAMBLE_KIND_TASK_RSP = 7    /* task responses, the provider pubs, directed */
} RambleTopicKind;

/* Immutable per topic facts served in the detail exchange and cached per (peer, index).
 * NO_TIMESTAMP is derived from the qos, the rest come from RambleTopicDef.attrs. */
#define RAMBLE_ATTR_NO_TIMESTAMP 0x01u /* the publisher sends no source stamp */
#define RAMBLE_ATTR_MULTI        0x02u /* authority kinds: duplicate authority is intended */
#define RAMBLE_ATTR_FORCEABLE    0x04u /* variable values: the owner permits force */
#define RAMBLE_ATTR_CANCELLABLE  0x08u /* task requests: the provider honors cancel */
#define RAMBLE_ATTR_EXCLUSIVE    0x10u /* task requests: declared serialization */

/* Every field except reliability is zero means default. docs/topics.md explains each. */
typedef struct {
    RambleReliability reliability;
    uint16_t keep_last;          /* messages retained. 0 = 1, or 10 on a reliable topic */
    uint16_t catch_up;           /* messages a new subscriber gets at once. 0 = future only */
    uint32_t max_message_bytes;  /* a size hint that pins the SHM class, never a cap */
    uint32_t heartbeat_us;       /* reliable idle publisher ping. 0 = 250 ms */
    uint32_t repair_delay_us;    /* the reliable re ask bound, 0 = adaptive from the round trip */
    uint32_t backpressure_wait_us;/* reliable send pause for a slow subscriber. 0 = none */
    uint32_t shm_max_bytes;      /* pin the topic to one SHM class. 0 = per message */
    uint32_t queue_bytes;        /* the consumer queue capacity, 0 = lazy up to RAMBLE_QUEUE_CAP */
    uint16_t max_rate_hz;        /* subscriber side, best effort: a delivery cap per publisher */
    uint8_t  no_timestamp;       /* publisher side: no source stamp, receivers see written_us 0 */
} RambleQos;

/* The name is the cross peer identity. The local handle is the index in topics[]. */
typedef struct {
    const char *name;  /* required, the same on every node, at most RAMBLE_TOPIC_NAME_MAX */
    RambleQos   qos;
    uint8_t  role;     /* RambleRole, 0 = pub and sub */
    uint8_t  kind;     /* RambleTopicKind, the patterns layer sets the rest */
    uint8_t  attrs;    /* RAMBLE_ATTR_* declared by the definer. NO_TIMESTAMP comes from the qos */
    uint8_t  prefix_bytes; /* pattern header bytes ahead of the payload, split off on receive */
    uint8_t  directed; /* 1 = point to point sends. Other lanes skip the seqno with no MSG_LOST */
} RambleTopicDef;

/* Return 0 delivered. Nonzero refuses: a reliable subscriber parks the sample with no
 * ack, so the publisher's flow control carries the backpressure. Best effort drops it. */
typedef int (*RambleMessageFn)(void *user, uint16_t topic_index, uint32_t from_peer, RambleBytes data);

#ifdef RAMBLE_SHM
/* SHM delivery hands the node the descriptor. 1 delivered, 0 unresolvable (the reliability
 * layer repairs or skips), -1 refused (parked like an inline refusal). */
typedef int (*i_RambleShmMsgFn)(void *user, uint16_t topic_index, uint32_t from_peer,
                             const uint8_t *desc);
#endif

/* The transport's own events. The node maps them into its RambleEvent. */
typedef enum {
    RAMBLE_TRANSPORT_MSG_LOST,        /* seqnos skipped: .topic, .peer, .lost_first, .lost_count */
    RAMBLE_TRANSPORT_MSG_TOO_BIG,     /* a received message could not be buffered, .too_big_bytes */
    RAMBLE_TRANSPORT_NAME_COLLISION,  /* a peer's name hashes to ours but differs, .identity */
    RAMBLE_TRANSPORT_QOS_INCOMPATIBLE,/* a reliable subscriber refused a best effort publisher */
    RAMBLE_TRANSPORT_SCHEMA_MISMATCH, /* the schema_check hook refused a direction, .peer_is_pub */
    RAMBLE_TRANSPORT_INTEREST_OVERFLOW,/* a peer's index map failed to allocate: .lost_count */
    RAMBLE_TRANSPORT_META_TRUNCATED_INTEREST, /* our overlay overflowed, peers see no topics */
    RAMBLE_TRANSPORT_META_TRUNCATED_SCHEMA,   /* retired, kept so binding enums stay aligned */
    RAMBLE_TRANSPORT_KIND_MISMATCH    /* a verified peer advertises the name under another kind */
} RambleTransportEventKind;

/* Read only the fields named for the kind. The topic name comes from ramble_transport_topic_name. */
typedef struct {
    RambleTransportEventKind kind;
    void       *user;          /* RambleConfig.user */
    uint32_t   peer;           /* 0 = none */
    uint16_t   topic;        /* local topic handle */
    uint64_t   lost_first;     /* MSG_LOST: first skipped seqno */
    uint64_t   lost_count;     /* MSG_LOST: seqnos skipped, fragments not messages */
    uint64_t   too_big_bytes;  /* MSG_TOO_BIG: size of the dropped message */
    uint64_t   identity;       /* NAME_COLLISION: the colliding identity */
    uint8_t    peer_is_pub;    /* SCHEMA_MISMATCH: the refused direction, 1 = their publisher */
} RambleTransportEvent;
typedef void (*RambleTransportEventFn)(const RambleTransportEvent *ev);

/* Largest message the wire can carry, 65535 fragments. */
#define RAMBLE_MESSAGE_MAX (65535u * RAMBLE_FRAG_SIZE_MAX)

/* RambleConfig.allocator is required. Every growable buffer sizes through it, so memory
 * scales with traffic and matches. Static mode passes ramble_allocator_alloc over a static one. */

/* topics set: defined at init. topics NULL: n_topics reserved slots, all INACTIVE, filled
 * later by ramble_transport_topic_define. */
typedef struct {
    const RambleTopicDef *topics;     /* NULL = reserve mode */
    uint16_t                n_topics;   /* defined count, or the reserved capacity */
    uint16_t                max_peers;
    uint16_t                frag_size; /* our fragment size, 0 = RAMBLE_FRAG_SIZE, clamped */
    RambleMessageFn         on_message;
#ifdef RAMBLE_SHM
    i_RambleShmMsgFn         on_shm;     /* SHM-DATA delivery, the node resolves the descriptor */
#endif
    RambleTransportEventFn  on_event;   /* optional */
    RambleAllocFn           allocator;  /* required, init fails without it */
    /* Optional schema gate at detail intake, once per direction. peer_is_pub 1 gates the
     * read side. Return 1 to allow. Verdicts are cached per (peer, index). */
    int                 (*schema_check)(void *user, uint32_t peer, uint16_t topic_index,
                                        int peer_is_pub, uint64_t schema_hash,
                                        RambleBytes schema_wire);
    /* Optional wall clock, UTC microseconds, stamped at commit. NULL still frames the 8
     * bytes as 0 on a stamped topic. Never the monotonic clock, the stamp crosses hosts. */
    uint64_t            (*source_time)(void *user);
    void                 *user;
} RambleConfig;

typedef struct RambleTransportState RambleTransportState;

RAMBLE_API size_t    ramble_transport_required_memory(const RambleConfig *cfg);
RAMBLE_API RambleTransportState *ramble_transport_init(void *mem, size_t mem_size, const RambleConfig *cfg);
/* Relocates a live transport, re striding its tables and carrying reliability state. The
 * caller frees the old arena block but must not destroy old. NULL leaves old intact. */
RAMBLE_API RambleTransportState *ramble_transport_migrate(RambleTransportState *old, void *new_mem, size_t new_cap,
                                 uint16_t new_max_peers, uint16_t new_n_topics);
/* Frees every hook allocation. The arena stays the caller's. */
RAMBLE_API void      ramble_transport_destroy(RambleTransportState *st);

/* The FNV-1a identity of a name. */
RAMBLE_API uint64_t  ramble_topic_id(const char *name);
RAMBLE_API uint64_t  ramble_topic_identity(const RambleTopicDef *def);   /* ramble_topic_id(def->name) */

/* 0 to RAMBLE_FRAG_SIZE, then clamp to [MIN, MAX]. Shared with the node's announce. */
RAMBLE_API uint16_t  ramble_clamp_frag(uint16_t frag_size);

/* Our fragment size. The node uses it as the SHM cutoff. */
RAMBLE_API uint16_t  ramble_transport_frag(RambleTransportState *st);

/* A new peer matches nothing until apply_peer_interest. peer_frag is its advertised
 * fragment size, 0 = RAMBLE_FRAG_SIZE, clamped to [MIN, MAX]. */
RAMBLE_API void      ramble_transport_peer_add   (RambleTransportState *st, uint32_t peer_id, uint16_t peer_frag);
RAMBLE_API void      ramble_transport_peer_remove(RambleTransportState *st, uint32_t peer_id);

/* A silent peer goes DORMANT: out of flow control, proxies and positions kept. resume re
 * includes it and re reports reader positions. Both no ops for an unknown peer. */
RAMBLE_API void      ramble_transport_peer_dormant(RambleTransportState *st, uint32_t peer_id);
RAMBLE_API void      ramble_transport_peer_resume (RambleTransportState *st, uint32_t peer_id);
/* A peer's blob may arrive after first contact. Clamped, a no op for an unknown peer. */
RAMBLE_API void      ramble_transport_peer_set_frag(RambleTransportState *st, uint32_t peer_id, uint16_t peer_frag);

/* The interest exchange. The list is hash only and positional, a hash overlap only
 * nominates, and the detail exchange below verifies. See spec/interest.md. */
RAMBLE_API size_t    ramble_interest_max(uint16_t n_topics);
RAMBLE_API size_t    ramble_transport_build_interest(RambleTransportState *st, void *out, size_t cap);
RAMBLE_API void      ramble_transport_apply_peer_interest(RambleTransportState *st, uint32_t peer_id, RambleBytes blob);

/* The announce overlay: a version prefix (fragment size, SHM capability and host) plus
 * the interest list, or the INTEREST_EXTERNAL bootstrap when it does not fit one datagram. */

/* One topic's schema advertisement, served by the detail responder. hash 0 = none. */
typedef struct {
    uint64_t    hash;
    RambleBytes wire;
} RambleMetaSchema;

/* Worst case overlay bytes for n_topics, capped to one datagram. Sizes discovery's meta_cap. */
RAMBLE_API uint16_t  ramble_meta_cap(uint16_t n_topics);
/* Exact bytes the next inline build emits, so a caller sizes to content. */
RAMBLE_API uint16_t  ramble_transport_meta_size(RambleTransportState *st);
RAMBLE_API uint16_t  ramble_transport_meta_bootstrap_size(void);
/* Builds the overlay. interest_external writes the bootstrap only. The node inlines while
 * the whole announce fits one datagram. Returns the bytes. */
RAMBLE_API uint16_t  ramble_transport_meta_build(RambleTransportState *st, uint8_t *out, uint16_t cap,
                                uint16_t frag_size, int shm_capable, const uint8_t host[16],
                                int interest_external);
/* 0 if malformed. */
RAMBLE_API uint16_t  ramble_meta_frag(RambleBytes meta);
/* 1 = the peer serves its interest through paging. */
RAMBLE_API int       ramble_meta_interest_external(RambleBytes meta);
/* {NULL, 0} if absent or external, so a bootstrap never reads as an empty interest list. */
RAMBLE_API RambleBytes ramble_meta_interest(RambleBytes meta);

/* One advertised direction of a topic. A PUBSUB topic yields twice, pub first. */
typedef struct {
    uint16_t    index;      /* the advertiser's topic index */
    uint8_t     role;       /* the advertiser's RambleRole */
    uint8_t     is_pub;     /* 1 = the publish direction, 0 = subscribe */
    uint8_t     reliable;   /* offered or requested reliability */
    uint8_t     kind;       /* the advertiser's RambleTopicKind */
    uint32_t    hash;       /* low 32 bits of the identity */
} RambleTopicEntry;

/* Zero initialize, then call until 0. Internal walk state. */
typedef struct {
    uint32_t off;        /* byte offset of the next entry */
    uint16_t left;       /* entries still to walk */
    uint16_t index;      /* position of the next entry */
    uint8_t  phase;      /* 1 = the current entry's pub direction was yielded */
    uint8_t  started;    /* 0 until the first call parses the header */
} RambleInterestIter;

/* Walks one direction at a time and stops at the end or at a malformed blob. interest_next
 * walks a bare blob, meta_interest_next the one inside an overlay. */
RAMBLE_API int       ramble_interest_next(RambleBytes interest, RambleInterestIter *it, RambleTopicEntry *out);
RAMBLE_API int       ramble_meta_interest_next(RambleBytes meta, RambleInterestIter *it, RambleTopicEntry *out);
#ifdef RAMBLE_SHM
/* 1 and host filled when the peer is SHM capable. */
RAMBLE_API int       ramble_meta_shm(RambleBytes meta, uint8_t host[16]);
#endif

/* The pairwise detail exchange, uDTL. The responder is stateless and answers the
 * request's source. The layout is in spec/interest.md. */
#define RAMBLE_DETAIL_REQ    1
#define RAMBLE_DETAIL_RESP   2
#define RAMBLE_INTEREST_REQ  3
#define RAMBLE_INTEREST_RESP 4

/* The peer's index plus our schema hash for it, 0 = none. */
typedef struct {
    uint16_t index;
    uint64_t schema_hash;
} RambleDetailWant;

/* Safe on any buffer. kind is 0 for anything that is not a well formed uDTL header. */
RAMBLE_API int       ramble_detail_kind(RambleBytes dgram);
RAMBLE_API uint16_t  ramble_detail_domain(RambleBytes dgram);
RAMBLE_API uint32_t  ramble_detail_meta_version(RambleBytes dgram);

/* Interest paging carries byte ranges of the build_interest blob. The requester keeps one
 * cursor per peer and a changed version restarts it. See spec/interest.md. */
#define RAMBLE_INTEREST_RESP_HEAD 24u   /* the family header plus total, offset and chunk_len */

/* Exact bytes of our current interest blob, the total_len paging serves. */
RAMBLE_API uint32_t  ramble_transport_interest_size(RambleTransportState *st);
/* 18 bytes, or 0 if cap is too small. peer_meta_version is advisory. */
RAMBLE_API size_t    ramble_interest_req_build(uint16_t domain, uint32_t peer_meta_version,
                                uint32_t offset, void *out, size_t cap);
/* 1 and *offset for a well formed INTEREST_REQ, else 0. */
RAMBLE_API int       ramble_interest_req_offset(RambleBytes dgram, uint32_t *offset);
/* Writes one page header. The caller lays the chunk right after it. */
RAMBLE_API size_t    ramble_interest_resp_head(uint16_t domain, uint32_t meta_version, uint32_t total_len,
                                uint32_t offset, uint16_t chunk_len, void *out, size_t cap);
/* 1 and the fields, chunk a bounds checked view into dgram, else 0. */
RAMBLE_API int       ramble_interest_resp_parse(RambleBytes dgram, uint32_t *total_len, uint32_t *offset,
                                RambleBytes *chunk);

/* 0 if cap cannot hold every want (14 plus 10 each). Batch per peer. */
RAMBLE_API size_t    ramble_detail_req_build(uint16_t domain, uint32_t peer_meta_version,
                                const RambleDetailWant *wants, uint16_t n_wants,
                                void *out, size_t cap);

/* Bytes one response page takes, 0 if req is malformed. One datagram, except that a lone
 * oversized entry rides its own page. */
RAMBLE_API size_t    ramble_transport_detail_resp_size(RambleTransportState *st, const RambleMetaSchema *schemas,
                                RambleBytes req);
/* Answers req into out with one entry per advertised requested index, the wire inlined
 * only where the hashes differ. Truncates at an entry boundary. Does not check the domain. */
RAMBLE_API size_t    ramble_transport_detail_respond(RambleTransportState *st, const RambleMetaSchema *schemas,
                                uint32_t meta_version, RambleBytes req, void *out, size_t cap);

/* One topic's details. name and schema_wire point into the response. */
typedef struct {
    uint16_t     index;       /* the responder's local topic index */
    uint8_t      attrs;       /* the topic's RAMBLE_ATTR_* byte */
    RambleString name;
    uint64_t     schema_hash; /* 0 = untyped */
    RambleBytes  schema_wire; /* only when the request's hash differed, else {NULL,0} */
} RambleDetail;

/* Zero initialize, then call until 0. */
typedef struct {
    uint32_t off;      /* byte offset of the next entry */
    uint16_t left;     /* entries still to yield */
    uint8_t  started;  /* 0 until the first call parses the header */
} RambleDetailIter;

/* Walks a DETAIL_RESP. Stops at the end or at a malformed response. */
RAMBLE_API int       ramble_detail_next(RambleBytes resp, RambleDetailIter *it, RambleDetail *out);

/* The pending candidates of a peer, up to max_wants (out NULL just counts). Re run it on
 * every announce from the peer while it returns nonzero. */
RAMBLE_API uint16_t  ramble_transport_detail_wants(RambleTransportState *st, const RambleMetaSchema *schemas,
                                uint32_t peer_id, RambleBytes interest,
                                RambleDetailWant *out, uint16_t max_wants);
/* Ingests a DETAIL_RESP and caches the verdicts. Returns the newly decided count, after
 * which the caller re applies the peer's interest so the matches form. */
RAMBLE_API uint16_t  ramble_transport_apply_peer_details(RambleTransportState *st, uint32_t peer_id,
                                RambleBytes resp);

/* Entries of a peer that nominate this topic and are still undecided. Feeds the match wait. */
RAMBLE_API uint16_t  ramble_transport_topic_unresolved(RambleTransportState *st, uint16_t topic_index,
                                uint32_t peer_id, RambleBytes interest);
/* The bulk form: every topic's unresolved count against one peer in a single walk. */
RAMBLE_API void      ramble_transport_peer_unresolved_fill(RambleTransportState *st, uint32_t peer_id,
                                RambleBytes interest, uint16_t *counts, uint16_t n);

/* Rematches peers locally. The caller re advertises. 0 ok, negative unknown. */
RAMBLE_API int       ramble_transport_set_role(RambleTransportState *st, uint16_t topic_index, uint8_t role);

/* Defines a reserved slot: name, qos, role, a history ring and a rematch. 0 ok, -1 bad
 * index, name or slot, -4 out of memory. Re advertise after. */
RAMBLE_API int       ramble_transport_topic_define(RambleTransportState *st, uint16_t topic_index, const RambleTopicDef *def);

/* Slot lifecycle, see spec/interest.md. retire parks a defined slot: 0 ok, -1 unknown or
 * already retired. Re advertise after. */
RAMBLE_API int       ramble_transport_topic_retire(RambleTransportState *st, uint16_t topic_index);
/* The slot a new define should reuse: 2 = a retired slot with the same identity and
 * kind, 1 = another retired slot, 0 = none. */
RAMBLE_API int       ramble_transport_topic_reuse_find(RambleTransportState *st, const char *name, uint8_t kind,
                                uint16_t *index_out);
/* Re defines a retired slot. binding_changed bumps the generation and holds writer lanes
 * until each peer names rebind_version in a request. 0 ok, -1 refused, -4 out of memory. */
RAMBLE_API int       ramble_transport_topic_reuse(RambleTransportState *st, uint16_t topic_index,
                                const RambleTopicDef *def, int binding_changed, uint32_t rebind_version);
/* Records the highest version of our blob a peer named in a request. 1 when a held lane formed. */
RAMBLE_API int       ramble_transport_peer_seen_version(RambleTransportState *st, uint32_t peer_id, uint32_t version);

/* The next write seqno. It continues across retire and reuse. */
RAMBLE_API uint64_t  ramble_transport_topic_seqno(RambleTransportState *st, uint16_t topic_index);

/* {NULL,0} if undefined. A local lookup, never on the data path. */
RAMBLE_API RambleString ramble_transport_topic_name(RambleTransportState *st, uint16_t topic_index);

/* 0 if the peer advertised no_timestamp for this topic. An unknown peer answers 1. */
RAMBLE_API int       ramble_transport_peer_timestamped(RambleTransportState *st, uint16_t topic_index,
                                uint32_t peer_id);

/* The peer's cached attrs byte for its topic, 0 until its details arrive. */
RAMBLE_API uint8_t   ramble_transport_peer_attrs(RambleTransportState *st, uint32_t peer_id,
                                uint16_t their_index);

/* 0 ok, negative on error. */
typedef enum {
    RAMBLE_OK             =  0,
    RAMBLE_ERR_NO_TOPIC = -1,  /* topic index out of range */
    RAMBLE_ERR_TOO_BIG    = -2,  /* exceeds RAMBLE_MESSAGE_MAX */
    RAMBLE_ERR_ROLE       = -3,  /* the topic is SUB_ONLY or INACTIVE */
    RAMBLE_ERR_OOM        = -4,  /* the allocator returned NULL */
    RAMBLE_ERR_STATE      = -5,  /* wrong state, or a call not allowed from inside a callback */
    RAMBLE_ERR_NOSYS      = -6   /* not compiled in */
} RambleResult;

/* Publishes to every matched subscriber. */
RAMBLE_API int       ramble_transport_send(RambleTransportState *st, uint16_t topic_index, RambleBytes data, uint64_t now_us);

/* Matched subscribers excluding dormant peers. O(matched lanes). */
RAMBLE_API int       ramble_transport_publisher_live_matches(RambleTransportState *st, uint16_t topic_index);
/* Is the peer a matched subscriber lane of this topic. The patterns layer's directed backstop. */
RAMBLE_API int       ramble_transport_publisher_peer_matched(RambleTransportState *st, uint16_t topic_index,
                                                         uint32_t peer_id);
/* The oldest live matched subscriber, 0 = none. The auto direct target for task requests. */
RAMBLE_API uint32_t  ramble_transport_publisher_oldest_match(RambleTransportState *st, uint16_t topic_index);
/* Matched publishers feeding our subscription side. */
RAMBLE_API int       ramble_transport_subscriber_match_count(RambleTransportState *st, uint16_t topic_index);

/* hdr rides in front of data as one message, byte identical to sending the concatenation.
 * capture_us 0 sends no capture slot. */
RAMBLE_API int       ramble_transport_send_hdr(RambleTransportState *st, uint16_t topic_index,
                               RambleBytes hdr, RambleBytes data, uint64_t capture_us, uint64_t now_us);
/* Publishes to one peer. Every other matched reliable lane skips the seqno through its
 * HB floor. A no op delivery when the peer is not a matched subscriber. */
RAMBLE_API int       ramble_transport_send_to(RambleTransportState *st, uint16_t topic_index, uint32_t to_peer,
                               RambleBytes hdr, RambleBytes data, uint64_t capture_us, uint64_t now_us);

#ifdef RAMBLE_SHM
/* The payload lives in an external chunk, fragmented for remote peers and described to
 * SHM peers. The chunk is the whole wire sample and must stay valid until it leaves history. */
RAMBLE_API int       ramble_transport_send_shm(RambleTransportState *st, uint16_t topic_index, RambleBytes chunk,
                               const uint8_t *desc, uint64_t now_us);
/* Whether a peer can receive SHM-DATA. The node sets it on attach. */
RAMBLE_API void      ramble_transport_peer_set_shm(RambleTransportState *st, uint32_t peer_id, int is_shm);
/* 1 if every matched subscriber is SHM capable. */
RAMBLE_API int       ramble_transport_publisher_shm_eligible(RambleTransportState *st, uint16_t topic_index);
/* The history slot the next publish occupies, to bind a chunk to it. */
RAMBLE_API uint16_t  ramble_transport_topic_hist_head(RambleTransportState *st, uint16_t topic_index);
#endif

/* NULL if unknown. */
RAMBLE_API const RambleQos *ramble_transport_topic_qos(RambleTransportState *st, uint16_t topic_index);
/* 0 = none or undefined. */
RAMBLE_API uint8_t   ramble_transport_topic_attrs(RambleTransportState *st, uint16_t topic_index);

/* 1 if the next send would overwrite history not yet acked by every subscriber. */
RAMBLE_API int       ramble_transport_send_would_evict(RambleTransportState *st, uint16_t topic_index);

/* 1 if the next send would overwrite history never handed to the wire, best effort
 * included. Fills the evicted sample's base and count. */
RAMBLE_API int       ramble_transport_send_would_evict_unsent(RambleTransportState *st, uint16_t topic_index,
                                                          uint64_t *evict_base, uint32_t *evict_count);

/* 1 if every live subscriber acked everything. Best effort and unknown return 1. */
RAMBLE_API int       ramble_transport_send_drained(RambleTransportState *st, uint16_t topic_index);

/* Matched subscribers, dormant included. O(1). */
RAMBLE_API int       ramble_transport_publisher_match_count(RambleTransportState *st, uint16_t topic_index);

/* Topics we publish to and receive from this peer. Both 0 for an unknown peer. */
RAMBLE_API void      ramble_transport_peer_match_counts(RambleTransportState *st, uint32_t peer_id,
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
    uint64_t msgs_skipped;   /* the sum of RAMBLE_MSG_LOST counts */
    /* each 0 to 1 arming of the subscriber's ack, by its trigger */
    uint64_t arms_data;
    uint64_t arms_hb;
    /* the fate of every received DATA fragment that was not accepted */
    uint64_t frags_old;        /* base below deliver_upto: already delivered or skipped */
    uint64_t frags_ahead;      /* beyond the one sample held ahead: dropped, fetched in order */
    uint64_t frags_malformed;  /* count 0, frag past count, or not subscribed */
} RambleRepairStats;

/* Zeroed if the topic is out of range. */
RAMBLE_API void      ramble_transport_repair_stats(RambleTransportState *st, uint16_t topic_index, RambleRepairStats *out);

/* The per peer round trip estimate, RFC 6298 shape, fed by the reliable path with no
 * probe traffic. samples 0 means no estimate yet. See spec/transport.md. */
typedef struct {
    uint32_t rtt_us;         /* smoothed round trip */
    uint32_t rtt_jitter_us;  /* mean deviation */
    uint32_t rtt_min_us;     /* the smallest sample, the path's floor */
    uint32_t rtt_last_us;    /* the most recent sample */
    uint32_t samples;
} RamblePeerRtt;
/* 1 and *out for a known peer, else 0 and *out zeroed. */
RAMBLE_API int       ramble_transport_peer_rtt(RambleTransportState *st, uint32_t peer_id, RamblePeerRtt *out);

/* Subscriber lanes of this topic with a repair request to service. */
RAMBLE_API int       ramble_transport_repair_pending(RambleTransportState *st, uint16_t topic_index);

/* The in progress message from peer: its base seqno, fragments held and fragments needed.
 * 1 if a message is mid reassembly. Any out pointer may be NULL. */
RAMBLE_API int       ramble_transport_subscriber_progress(RambleTransportState *st, uint16_t topic_index, uint32_t peer,
                                     uint64_t *base_seqno, uint32_t *have, uint32_t *total);

/* Retries the parked samples of a topic when downstream capacity frees, then flush
 * poll_send so the acks go out. Returns the lanes still parked. */
RAMBLE_API uint32_t  ramble_transport_deliver_parked(RambleTransportState *st, uint16_t topic_index, uint64_t now_us);

/* Feeds a received datagram tagged with its peer. */
RAMBLE_API void      ramble_transport_on_datagram(RambleTransportState *st, uint32_t from_peer, RambleBytes datagram,
                                  uint64_t now_us);

/* One outgoing datagram, batched per peer. 1 and the outs, or 0. Loop until 0 with a
 * RAMBLE_DGRAM_MAX cap. */
RAMBLE_API int       ramble_transport_poll_send(RambleTransportState *st, uint32_t *to_peer, void *out, size_t cap,
                                size_t *out_len, uint64_t now_us);

/* The next armed timer, or 0. Cap a blocking poll at it. */
RAMBLE_API uint64_t  ramble_transport_next_deadline_us(RambleTransportState *st);

/* 1 while any lane holds work for poll_send. O(1). */
RAMBLE_API int       ramble_transport_tx_pending(RambleTransportState *st);

#ifdef __cplusplus
}
#endif
#endif /* RAMBLE_TRANSPORT_H */
