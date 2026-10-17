/* The sans-IO transport core. Feed it datagrams, the clock and a peer set, it returns
 * datagrams to send and delivers messages. The rules are in spec/transport.md. */
#ifndef RANT_TRANSPORT_H
#define RANT_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>
#include "../common/features.h"
#include "../common/types.h"
#include "../common/string.h"
#include "../common/alloc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RANT_DGRAM_MAX (RANT_FRAG_SIZE_MAX + 40u)       /* plus the largest header */

#ifdef RANT_SHM
#define RANT_SHM_DESC_BYTES 24u     /* the descriptor on the wire, equals RANT_SHM_DESC_WIRE */
#endif

/* The source stamp in front of every sample of a stamped topic, taken once at commit so
 * repair, replay and shared memory carry the original. RantQos.no_timestamp opts out. */
#define RANT_TIMESTAMP_BYTES 8u
/* A microsecond wall clock needs 51 bits, so the source stamp's top bit is free to mark
 * that a capture slot follows it. Per message, and costs nothing unset. See spec/node.md */
#define RANT_CAPTURE_BYTES     8u
#define RANT_STAMP_CAPTURE     0x8000000000000000ull
#define RANT_STAMP_MASK        0x7FFFFFFFFFFFFFFFull

/* Shared by every matching, announce and reflection walk, so a flipped test is impossible. */
static inline int i_rant_role_pubs(uint8_t role){ return role == RANT_PUBSUB || role == RANT_PUB_ONLY; }
static inline int i_rant_role_subs(uint8_t role){ return role == RANT_PUBSUB || role == RANT_SUB_ONLY; }

/* What a topic carries. A same name under a different kind is a disjoint entity and the
 * pairing is refused. The kind rides the interest flags and is immutable per topic. */
typedef enum {
    RANT_KIND_TOPIC      = 0,   /* plain pub sub */
    RANT_KIND_FUNC_REQ = 1,     /* function requests, the caller pubs */
    RANT_KIND_FUNC_RSP = 2,     /* function responses, the provider pubs, directed */
    RANT_KIND_VARIABLE = 3,     /* variable values, the owner pubs */
    RANT_KIND_VAR_SET    = 4,   /* variable sets, writers pub, the owner subs */
    RANT_KIND_TASK_REQ = 5,     /* task requests and cancel ops, callers pub */
    RANT_KIND_TASK_PRG = 6,     /* task progress, the provider pubs, broadcast */
    RANT_KIND_TASK_RSP = 7      /* task responses, the provider pubs, directed */
} i_RantTopicKind;

/* Immutable per topic facts served in the detail exchange and cached per (peer, index).
 * NO_TIMESTAMP is derived from the qos, the rest come from i_RantTopicDef.attrs. */
#define RANT_ATTR_NO_TIMESTAMP 0x01u /* the publisher sends no source stamp */
#define RANT_ATTR_MULTI          0x02u /* authority kinds: duplicate authority is intended */
#define RANT_ATTR_FORCEABLE      0x04u /* variable values: the owner permits force */
#define RANT_ATTR_CANCELLABLE    0x08u /* task requests: the provider honors cancel */
#define RANT_ATTR_EXCLUSIVE      0x10u /* task requests: declared serialization */

/* The name is the cross peer identity. The local handle is the index in topics[]. */
typedef struct {
    const char *name;  /* required, the same on every node, at most RANT_TOPIC_NAME_MAX */
    RantQos     qos;
    uint8_t  role;     /* RantRole, 0 = pub and sub */
    uint8_t  kind;     /* i_RantTopicKind, the patterns layer sets the rest */
    uint8_t  attrs;    /* RANT_ATTR_* declared by the definer. NO_TIMESTAMP comes from the qos */
    uint8_t  prefix_bytes; /* pattern header bytes ahead of the payload, split off on receive */
    uint8_t  directed; /* 1 = point to point sends. Other lanes skip the seqno with no MSG_LOST */
} i_RantTopicDef;

/* Return 0 delivered. Nonzero refuses: a reliable subscriber parks the sample with no
 * ack, so the publisher's flow control carries the backpressure. Best effort drops it. */
typedef int (*i_RantMessageFn)(void *user, uint16_t topic_index, uint32_t from_peer, RantBytes data);

#ifdef RANT_SHM
/* SHM delivery hands the node the descriptor. 1 delivered, 0 unresolvable (the reliability
 * layer repairs or skips), -1 refused (parked like an inline refusal). */
typedef int (*i_RantShmMsgFn)(void *user, uint16_t topic_index, uint32_t from_peer,
                             const uint8_t *desc);
#endif

/* i_RantTransportConfig.allocator is required. Every growable buffer sizes through it, so memory
 * scales with traffic and matches. Static mode passes rant_allocator_alloc over a static one. */

/* topics set: defined at init. topics NULL: n_topics reserved slots, all INACTIVE, filled
 * later by i_rant_transport_topic_define. */
typedef struct {
    const i_RantTopicDef *topics;       /* NULL = reserve mode */
    uint16_t                n_topics;   /* defined count, or the reserved capacity */
    uint16_t                max_peers;
    uint16_t                frag_size; /* our fragment size, 0 = RANT_FRAG_SIZE, clamped */
    i_RantMessageFn           on_message;
#ifdef RANT_SHM
    i_RantShmMsgFn           on_shm;     /* SHM-DATA delivery, the node resolves the descriptor */
#endif
    RantEventFn             on_event;   /* optional, .user is i_RantTransportConfig.user */
    RantAllocFn             allocator;  /* required, init fails without it */
    /* Optional schema gate at detail intake, once per direction. peer_is_pub 1 gates the
     * read side. Return 1 to allow. Verdicts are cached per (peer, index). */
    int                 (*schema_check)(void *user, uint32_t peer, uint16_t topic_index,
                                        int peer_is_pub, uint64_t schema_hash,
                                        RantBytes schema_wire);
    /* Optional: one line saying why schema_check refused that direction, or NULL. It
     * rides a SCHEMA_MISMATCH event as .schema_detail. */
    const char         *(*schema_why)(void *user, uint32_t peer, uint16_t topic_index,
                                      int peer_is_pub);
    /* Optional wall clock, UTC microseconds, stamped at commit. NULL still frames the 8
     * bytes as 0 on a stamped topic. Never the monotonic clock, the stamp crosses hosts. */
    uint64_t            (*source_time)(void *user);
    void                 *user;
} i_RantTransportConfig;

typedef struct i_RantTransportState i_RantTransportState;

size_t      i_rant_transport_required_memory(const i_RantTransportConfig *cfg);
i_RantTransportState *i_rant_transport_init(void *mem, size_t mem_size, const i_RantTransportConfig *cfg);
/* Relocates a live transport, re striding its tables and carrying reliability state. The
 * caller frees the old arena block but must not destroy old. NULL leaves old intact. */
i_RantTransportState *i_rant_transport_migrate(i_RantTransportState *old, void *new_mem, size_t new_cap,
                                 uint16_t new_max_peers, uint16_t new_n_topics);
/* Frees every hook allocation. The arena stays the caller's. */
void        i_rant_transport_destroy(i_RantTransportState *st);

/* The FNV-1a identity of a name. */
uint64_t    i_rant_topic_id(const char *name);
uint64_t    i_rant_topic_identity(const i_RantTopicDef *def);       /* i_rant_topic_id(def->name) */

/* 0 to RANT_FRAG_SIZE, then clamp to [MIN, MAX]. Shared with the node's announce. */
uint16_t    i_rant_clamp_frag(uint16_t frag_size);

/* Our fragment size. The node uses it as the SHM cutoff. */
uint16_t    i_rant_transport_frag(i_RantTransportState *st);

/* A new peer matches nothing until apply_peer_interest. peer_frag is its advertised
 * fragment size, 0 = RANT_FRAG_SIZE, clamped to [MIN, MAX]. */
void        i_rant_transport_peer_add     (i_RantTransportState *st, uint32_t peer_id, uint16_t peer_frag);
void        i_rant_transport_peer_remove(i_RantTransportState *st, uint32_t peer_id);

/* A silent peer goes DORMANT: out of flow control, proxies and positions kept. resume re
 * includes it and re reports reader positions. Both no ops for an unknown peer. */
void        i_rant_transport_peer_dormant(i_RantTransportState *st, uint32_t peer_id);
void        i_rant_transport_peer_resume (i_RantTransportState *st, uint32_t peer_id);
/* A peer's blob may arrive after first contact. Clamped, a no op for an unknown peer. */
void        i_rant_transport_peer_set_frag(i_RantTransportState *st, uint32_t peer_id, uint16_t peer_frag);

/* The interest exchange. The list is hash only and positional, a hash overlap only
 * nominates, and the detail exchange below verifies. See spec/interest.md. */
size_t      i_rant_interest_max(uint16_t n_topics);
size_t      i_rant_transport_build_interest(i_RantTransportState *st, void *out, size_t cap);
void        i_rant_transport_apply_peer_interest(i_RantTransportState *st, uint32_t peer_id, RantBytes blob);

/* The announce overlay: a version prefix (fragment size, SHM capability and host) plus
 * the interest list, or the INTEREST_EXTERNAL bootstrap when it does not fit one datagram. */

/* One topic's schema advertisement, served by the detail responder. hash 0 = none. */
typedef struct {
    uint64_t    hash;
    RantBytes wire;
} i_RantMetaSchema;

/* Worst case overlay bytes for n_topics, capped to one datagram. Sizes discovery's meta_cap. */
uint16_t    i_rant_meta_cap(uint16_t n_topics);
/* Exact bytes the next inline build emits, so a caller sizes to content. */
uint16_t    i_rant_transport_meta_size(i_RantTransportState *st);
uint16_t    i_rant_transport_meta_bootstrap_size(void);
/* Builds the overlay. interest_external writes the bootstrap only. The node inlines while
 * the whole announce fits one datagram. Returns the bytes. */
uint16_t    i_rant_transport_meta_build(i_RantTransportState *st, uint8_t *out, uint16_t cap,
                                uint16_t frag_size, int shm_capable, const uint8_t host[16],
                                int interest_external);
/* 0 if malformed. */
uint16_t    i_rant_meta_frag(RantBytes meta);
/* 1 = the peer serves its interest through paging. */
int         i_rant_meta_interest_external(RantBytes meta);
/* {NULL, 0} if absent or external, so a bootstrap never reads as an empty interest list. */
RantBytes i_rant_meta_interest(RantBytes meta);

/* One advertised direction of a topic. A PUBSUB topic yields twice, pub first. */
typedef struct {
    uint16_t    index;      /* the advertiser's topic index */
    uint8_t     role;       /* the advertiser's RantRole */
    uint8_t     is_pub;     /* 1 = the publish direction, 0 = subscribe */
    uint8_t     reliable;   /* offered or requested reliability */
    uint8_t     kind;       /* the advertiser's i_RantTopicKind */
    uint32_t    hash;       /* low 32 bits of the identity */
} i_RantTopicEntry;

/* Zero initialize, then call until 0. Internal walk state. */
typedef struct {
    uint32_t off;        /* byte offset of the next entry */
    uint16_t left;       /* entries still to walk */
    uint16_t index;      /* position of the next entry */
    uint8_t  phase;      /* 1 = the current entry's pub direction was yielded */
    uint8_t  started;    /* 0 until the first call parses the header */
} i_RantInterestIter;

/* Walks one direction at a time and stops at the end or at a malformed blob. interest_next
 * walks a bare blob, meta_interest_next the one inside an overlay. */
int         i_rant_interest_next(RantBytes interest, i_RantInterestIter *it, i_RantTopicEntry *out);
int         i_rant_meta_interest_next(RantBytes meta, i_RantInterestIter *it, i_RantTopicEntry *out);
#ifdef RANT_SHM
/* 1 and host filled when the peer is SHM capable. */
int         i_rant_meta_shm(RantBytes meta, uint8_t host[16]);
#endif

/* The pairwise detail exchange, uDTL. The responder is stateless and answers the
 * request's source. The layout is in spec/interest.md. */
#define RANT_DETAIL_REQ      1
#define RANT_DETAIL_RESP     2
#define RANT_INTEREST_REQ    3
#define RANT_INTEREST_RESP 4

/* The peer's index plus our schema hash for it, 0 = none. */
typedef struct {
    uint16_t index;
    uint64_t schema_hash;
} i_RantDetailWant;

/* Safe on any buffer. kind is 0 for anything that is not a well formed uDTL header. */
int         i_rant_detail_kind(RantBytes dgram);
uint16_t    i_rant_detail_domain(RantBytes dgram);
uint32_t    i_rant_detail_meta_version(RantBytes dgram);

/* Interest paging carries byte ranges of the build_interest blob. The requester keeps one
 * cursor per peer and a changed version restarts it. See spec/interest.md. */
#define RANT_INTEREST_RESP_HEAD 24u     /* the family header plus total, offset and chunk_len */

/* Exact bytes of our current interest blob, the total_len paging serves. */
uint32_t    i_rant_transport_interest_size(i_RantTransportState *st);
/* 18 bytes, or 0 if cap is too small. peer_meta_version is advisory. */
size_t      i_rant_interest_req_build(uint16_t domain, uint32_t peer_meta_version,
                                uint32_t offset, void *out, size_t cap);
/* 1 and *offset for a well formed INTEREST_REQ, else 0. */
int         i_rant_interest_req_offset(RantBytes dgram, uint32_t *offset);
/* Writes one page header. The caller lays the chunk right after it. */
size_t      i_rant_interest_resp_head(uint16_t domain, uint32_t meta_version, uint32_t total_len,
                                uint32_t offset, uint16_t chunk_len, void *out, size_t cap);
/* 1 and the fields, chunk a bounds checked view into dgram, else 0. */
int         i_rant_interest_resp_parse(RantBytes dgram, uint32_t *total_len, uint32_t *offset,
                                RantBytes *chunk);

/* 0 if cap cannot hold every want (14 plus 10 each). Batch per peer. */
size_t      i_rant_detail_req_build(uint16_t domain, uint32_t peer_meta_version,
                                const i_RantDetailWant *wants, uint16_t n_wants,
                                void *out, size_t cap);

/* Bytes one response page takes, 0 if req is malformed. One datagram, except that a lone
 * oversized entry rides its own page. */
size_t      i_rant_transport_detail_resp_size(i_RantTransportState *st, const i_RantMetaSchema *schemas,
                                RantBytes req);
/* Answers req into out with one entry per advertised requested index, the wire inlined
 * only where the hashes differ. Truncates at an entry boundary. Does not check the domain. */
size_t      i_rant_transport_detail_respond(i_RantTransportState *st, const i_RantMetaSchema *schemas,
                                uint32_t meta_version, RantBytes req, void *out, size_t cap);

/* One topic's details. name and schema_wire point into the response. */
typedef struct {
    uint16_t     index;       /* the responder's local topic index */
    uint8_t      attrs;       /* the topic's RANT_ATTR_* byte */
    RantString name;
    uint64_t     schema_hash; /* 0 = untyped */
    RantBytes    schema_wire; /* only when the request's hash differed, else {NULL,0} */
} i_RantDetail;

/* Zero initialize, then call until 0. */
typedef struct {
    uint32_t off;      /* byte offset of the next entry */
    uint16_t left;     /* entries still to yield */
    uint8_t  started;  /* 0 until the first call parses the header */
} i_RantDetailIter;

/* Walks a DETAIL_RESP. Stops at the end or at a malformed response. */
int         i_rant_detail_next(RantBytes resp, i_RantDetailIter *it, i_RantDetail *out);

/* The pending candidates of a peer, up to max_wants (out NULL just counts). Re run it on
 * every announce from the peer while it returns nonzero. */
uint16_t    i_rant_transport_detail_wants(i_RantTransportState *st, const i_RantMetaSchema *schemas,
                                uint32_t peer_id, RantBytes interest,
                                i_RantDetailWant *out, uint16_t max_wants);
/* Ingests a DETAIL_RESP and caches the verdicts. Returns the newly decided count, after
 * which the caller re applies the peer's interest so the matches form. */
uint16_t    i_rant_transport_apply_peer_details(i_RantTransportState *st, uint32_t peer_id,
                                RantBytes resp);

/* Entries of a peer that nominate this topic and are still undecided. Feeds the match wait. */
uint16_t    i_rant_transport_topic_unresolved(i_RantTransportState *st, uint16_t topic_index,
                                uint32_t peer_id, RantBytes interest);
/* The bulk form: every topic's unresolved count against one peer in a single walk. */
void        i_rant_transport_peer_unresolved_fill(i_RantTransportState *st, uint32_t peer_id,
                                RantBytes interest, uint16_t *counts, uint16_t n);

/* Rematches peers locally. The caller re advertises. 0 ok, negative unknown. */
int         i_rant_transport_set_role(i_RantTransportState *st, uint16_t topic_index, uint8_t role);

/* Defines a reserved slot: name, qos, role, a history ring and a rematch. 0 ok, -1 bad
 * index, name or slot, -4 out of memory. Re advertise after. */
int         i_rant_transport_topic_define(i_RantTransportState *st, uint16_t topic_index, const i_RantTopicDef *def);

/* Slot lifecycle, see spec/interest.md. retire parks a defined slot: 0 ok, -1 unknown or
 * already retired. Re advertise after. */
int         i_rant_transport_topic_retire(i_RantTransportState *st, uint16_t topic_index);
/* The slot a new define should reuse: 2 = a retired slot with the same identity and
 * kind, 1 = another retired slot, 0 = none. */
int         i_rant_transport_topic_reuse_find(i_RantTransportState *st, const char *name, uint8_t kind,
                                uint16_t *index_out);
/* Re defines a retired slot. binding_changed bumps the generation and holds writer lanes
 * until each peer names rebind_version in a request. 0 ok, -1 refused, -4 out of memory. */
int         i_rant_transport_topic_reuse(i_RantTransportState *st, uint16_t topic_index,
                                const i_RantTopicDef *def, int binding_changed, uint32_t rebind_version);
/* Records the highest version of our blob a peer named in a request. 1 when a held lane formed. */
int         i_rant_transport_peer_seen_version(i_RantTransportState *st, uint32_t peer_id, uint32_t version);

/* The next write seqno. It continues across retire and reuse. */
uint64_t    i_rant_transport_topic_seqno(i_RantTransportState *st, uint16_t topic_index);

/* {NULL,0} if undefined. A local lookup, never on the data path. */
RantString i_rant_transport_topic_name(i_RantTransportState *st, uint16_t topic_index);

/* 0 if the peer advertised no_timestamp for this topic. An unknown peer answers 1. */
int         i_rant_transport_peer_timestamped(i_RantTransportState *st, uint16_t topic_index,
                                uint32_t peer_id);

/* The peer's cached attrs byte for its topic, 0 until its details arrive. */
uint8_t     i_rant_transport_peer_attrs(i_RantTransportState *st, uint32_t peer_id,
                                uint16_t their_index);

/* Publishes to every matched subscriber. */
int         i_rant_transport_send(i_RantTransportState *st, uint16_t topic_index, RantBytes data, uint64_t now_us);

/* Matched subscribers excluding dormant peers. O(matched lanes). */
int         i_rant_transport_publisher_live_matches(i_RantTransportState *st, uint16_t topic_index);
/* Is the peer a matched subscriber lane of this topic. The patterns layer's directed backstop. */
int         i_rant_transport_publisher_peer_matched(i_RantTransportState *st, uint16_t topic_index,
                                                         uint32_t peer_id);
/* The oldest live matched subscriber, 0 = none. The auto direct target for task requests. */
uint32_t    i_rant_transport_publisher_oldest_match(i_RantTransportState *st, uint16_t topic_index);
/* Matched publishers feeding our subscription side. */
int         i_rant_transport_subscriber_match_count(i_RantTransportState *st, uint16_t topic_index);

/* hdr rides in front of data as one message, byte identical to sending the concatenation.
 * capture_us 0 sends no capture slot. */
int         i_rant_transport_send_hdr(i_RantTransportState *st, uint16_t topic_index,
                               RantBytes hdr, RantBytes data, uint64_t capture_us, uint64_t now_us);
/* Publishes to one peer. Every other matched reliable lane skips the seqno through its
 * HB floor. A no op delivery when the peer is not a matched subscriber. */
int         i_rant_transport_send_to(i_RantTransportState *st, uint16_t topic_index, uint32_t to_peer,
                               RantBytes hdr, RantBytes data, uint64_t capture_us, uint64_t now_us);

#ifdef RANT_SHM
/* The payload lives in an external chunk, fragmented for remote peers and described to
 * SHM peers. The chunk is the whole wire sample and must stay valid until it leaves history. */
int         i_rant_transport_send_shm(i_RantTransportState *st, uint16_t topic_index, RantBytes chunk,
                               const uint8_t *desc, uint64_t now_us);
/* Whether a peer can receive SHM-DATA. The node sets it on attach. */
void        i_rant_transport_peer_set_shm(i_RantTransportState *st, uint32_t peer_id, int is_shm);
/* 1 if every matched subscriber is SHM capable. */
int         i_rant_transport_publisher_shm_eligible(i_RantTransportState *st, uint16_t topic_index);
/* The history slot the next publish occupies, to bind a chunk to it. */
uint16_t    i_rant_transport_topic_hist_head(i_RantTransportState *st, uint16_t topic_index);
#endif

/* NULL if unknown. */
const RantQos *i_rant_transport_topic_qos(i_RantTransportState *st, uint16_t topic_index);
/* 0 = none or undefined. */
uint8_t     i_rant_transport_topic_attrs(i_RantTransportState *st, uint16_t topic_index);

/* 1 if the next send would overwrite history not yet acked by every subscriber. */
int         i_rant_transport_send_would_evict(i_RantTransportState *st, uint16_t topic_index);

/* 1 if the next send would overwrite history never handed to the wire, best effort
 * included. Fills the evicted sample's base and count. */
int         i_rant_transport_send_would_evict_unsent(i_RantTransportState *st, uint16_t topic_index,
                                                          uint64_t *evict_base, uint32_t *evict_count);

/* 1 if every live subscriber acked everything. Best effort and unknown return 1. */
int         i_rant_transport_send_drained(i_RantTransportState *st, uint16_t topic_index);

/* Matched subscribers, dormant included. O(1). */
int         i_rant_transport_publisher_match_count(i_RantTransportState *st, uint16_t topic_index);

/* Topics we publish to and receive from this peer. Both 0 for an unknown peer. */
void        i_rant_transport_peer_match_counts(i_RantTransportState *st, uint32_t peer_id,
                                          uint16_t *publish_to, uint16_t *receive_from);

/* Zeroed if the topic is out of range. */
void        i_rant_transport_repair_stats(i_RantTransportState *st, uint16_t topic_index, RantRepairStats *out);

/* The per peer round trip estimate, RFC 6298 shape, fed by the reliable path with no
 * probe traffic. samples 0 means no estimate yet. See spec/transport.md. */
typedef struct {
    uint32_t rtt_us;         /* smoothed round trip */
    uint32_t rtt_jitter_us;  /* mean deviation */
    uint32_t rtt_min_us;     /* the smallest sample, the path's floor */
    uint32_t rtt_last_us;    /* the most recent sample */
    uint32_t samples;
} i_RantPeerRtt;
/* 1 and *out for a known peer, else 0 and *out zeroed. */
int         i_rant_transport_peer_rtt(i_RantTransportState *st, uint32_t peer_id, i_RantPeerRtt *out);

/* Subscriber lanes of this topic with a repair request to service. */
int         i_rant_transport_repair_pending(i_RantTransportState *st, uint16_t topic_index);

/* The in progress message from peer: its base seqno, fragments held and fragments needed.
 * 1 if a message is mid reassembly. Any out pointer may be NULL. */
int         i_rant_transport_subscriber_progress(i_RantTransportState *st, uint16_t topic_index, uint32_t peer,
                                     uint64_t *base_seqno, uint32_t *have, uint32_t *total);

/* Retries the parked samples of a topic when downstream capacity frees, then flush
 * poll_send so the acks go out. Returns the lanes still parked. */
uint32_t    i_rant_transport_deliver_parked(i_RantTransportState *st, uint16_t topic_index, uint64_t now_us);

/* Feeds a received datagram tagged with its peer. */
void        i_rant_transport_on_datagram(i_RantTransportState *st, uint32_t from_peer, RantBytes datagram,
                                  uint64_t now_us);

/* One outgoing datagram, batched per peer. 1 and the outs, or 0. Loop until 0 with a
 * RANT_DGRAM_MAX cap. */
int         i_rant_transport_poll_send(i_RantTransportState *st, uint32_t *to_peer, void *out, size_t cap,
                                size_t *out_len, uint64_t now_us);

/* The next armed timer, or 0. Cap a blocking poll at it. */
uint64_t    i_rant_transport_next_deadline_us(i_RantTransportState *st);

/* 1 while any lane holds work for poll_send. O(1). */
int         i_rant_transport_tx_pending(i_RantTransportState *st);

#ifdef __cplusplus
}
#endif
#endif /* RANT_TRANSPORT_H */
