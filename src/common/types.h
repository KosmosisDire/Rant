/* The public vocabulary every layer shares: limits, roles, the QoS, result codes and the
 * repair counters. */
#ifndef RANT_TYPES_H
#define RANT_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RANT_FRAG_SIZE
#define RANT_FRAG_SIZE 1350u            /* default message bytes per fragment */
#endif
/* A node fragments every send with one size, set at init and advertised by discovery.
 * MIN and MAX bound any node's size, and MAX sizes the datagram buffers. */
#ifndef RANT_FRAG_SIZE_MAX
#define RANT_FRAG_SIZE_MAX RANT_FRAG_SIZE
#endif
#ifndef RANT_FRAG_SIZE_MIN
#define RANT_FRAG_SIZE_MIN RANT_FRAG_SIZE
#endif
/* Largest message the wire can carry, 65535 fragments. */
#define RANT_MESSAGE_MAX (65535u * RANT_FRAG_SIZE_MAX)

#ifndef RANT_TOPIC_NAME_MAX
#define RANT_TOPIC_NAME_MAX 64u            /* topic name bytes on the wire */
#endif

#ifndef RANT_NODE_NAME_MAX
#define RANT_NODE_NAME_MAX 32u             /* node name bytes in the announce */
#endif

typedef enum { RANT_BEST_EFFORT = 0, RANT_RELIABLE = 1 } RantReliability;
/* RANT_INACTIVE is declared but off. Resources stay allocated and set_role flips it. */
typedef enum { RANT_PUBSUB = 0, RANT_PUB_ONLY = 1, RANT_SUB_ONLY = 2,
               RANT_INACTIVE = 3 } RantRole;

/* Every field except reliability is zero means default. docs/topics.md explains each. */
typedef struct {
    RantReliability reliability;
    uint16_t keep_last;          /* messages retained. 0 = 1, or 10 on a reliable topic */
    uint16_t catch_up;           /* messages a new subscriber gets at once. 0 = future only */
    uint32_t max_message_bytes;  /* a size hint that pins the SHM class, never a cap */
    uint32_t heartbeat_us;       /* reliable idle publisher ping. 0 = 250 ms */
    uint32_t repair_delay_us;    /* the reliable re ask bound, 0 = adaptive from the round trip */
    uint32_t backpressure_wait_us;/* reliable send pause for a slow subscriber. 0 = none */
    uint32_t shm_max_bytes;      /* pin the topic to one SHM class. 0 = per message */
    uint32_t queue_bytes;        /* the consumer queue capacity, 0 = lazy up to RANT_QUEUE_CAP */
    uint16_t max_rate_hz;        /* subscriber side, best effort: a delivery cap per publisher */
    uint8_t  no_timestamp;       /* publisher side: no source stamp, receivers see written_us 0 */
} RantQos;

/* 0 ok, negative on error. */
typedef enum {
    RANT_OK               =  0,
    RANT_ERR_NO_TOPIC = -1,    /* topic index out of range */
    RANT_ERR_TOO_BIG      = -2,  /* exceeds RANT_MESSAGE_MAX */
    RANT_ERR_ROLE         = -3,  /* the topic is SUB_ONLY or INACTIVE */
    RANT_ERR_OOM          = -4,  /* the allocator returned NULL */
    RANT_ERR_STATE        = -5,  /* wrong state, or a call not allowed from inside a callback */
    RANT_ERR_NOSYS        = -6,  /* not compiled in */
    RANT_ERR_SCHEMA       = -7   /* the payload is not a message of the topic's schema */
} RantResult;

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
    uint64_t msgs_skipped;   /* the sum of RANT_MSG_LOST counts */
    /* each 0 to 1 arming of the subscriber's ack, by its trigger */
    uint64_t arms_data;
    uint64_t arms_hb;
    /* the fate of every received DATA fragment that was not accepted */
    uint64_t frags_old;        /* base below deliver_upto: already delivered or skipped */
    uint64_t frags_ahead;      /* beyond the one sample held ahead: dropped, fetched in order */
    uint64_t frags_malformed;  /* count 0, frag past count, or not subscribed */
} RantRepairStats;

#ifdef __cplusplus
}
#endif
#endif /* RANT_TYPES_H */
