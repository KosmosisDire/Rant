/* The public vocabulary every layer shares: limits, roles, the QoS, result codes, the
 * repair counters, the network options and the event. */
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

/* One network address. Every layer names a peer, a seed or a destination with it. */
typedef struct {
    uint8_t  ip[16];   /* network order bytes, IPv4 today */
    uint8_t  ip_len;   /* 4 or 16, 0 = none */
    uint16_t port;     /* host order */
} RantAddr;

/* Whether a listed peer is here now or is a dropped peer's last known view. */
typedef enum {
    RANT_PEER_ACTIVE    = 0,   /* heard within peer_timeout_us */
    RANT_PEER_DROPPED = 1       /* silent, state kept, the same uuid may return */
} RantPeerLiveness;

/* The discovery defaults, stated once for every layer. */
#define RANT_DISCOVERY_GROUP      "239.255.0.7"
#define RANT_DISCOVERY_PORT       7400
#define RANT_ANNOUNCE_INTERVAL_US 3000000u     /* 3 s. The peer timeout defaults to 4 of them */
#define RANT_MAX_PEERS            16

/* Network addressing and sockets. Every field is zero means default. docs/discovery.md
 * explains the options. */
typedef struct {
    uint16_t              data_port;         /* the unicast data port, 0 = OS assigned */
    const char           *discovery_group;   /* RANT_DISCOVERY_GROUP */
    uint16_t              discovery_port;    /* RANT_DISCOVERY_PORT */
    const char           *multicast_interface;/* pin discovery to this interface IP. NULL = every
                                                interface, "127.0.0.1" = single host */
    uint8_t               multicast_ttl;     /* hops an announce may travel, 1 */
    const RantAddr       *seed_peers;        /* unicast announces here too, port 0 = discovery_port */
    uint16_t              n_seed_peers;
    uint8_t               unicast_only;      /* 1 = no group join, seeds and relays only. Also for a
                                                node behind an outbound only NAT */
    uint32_t              recv_buffer_bytes; /* the data socket SO_RCVBUF, 0 = OS default */
    uint32_t              send_buffer_bytes; /* the data socket SO_SNDBUF, 0 = OS default */
    uint16_t              fragment_size;     /* UDP payload bytes per fragment, 0 = RANT_FRAG_SIZE.
                                                Clamped to [MIN, MAX]. One size per node */
    /* Stating our own locator. The default states nothing and peers record the source an
     * announce arrived from. Use these for a static one to one mapping. */
    const char           *self_ip;           /* advertise this IPv4 address. Unparseable refuses the
                                                open with RANT_E_BAD_ADDRESS */
    uint16_t              advertise_port;    /* advertise this data port instead of the bound one */
} RantNodeNet;

/* Discovery cadence and the peer table size, zero means default. */
typedef struct {
    uint32_t              announce_interval_us; /* RANT_ANNOUNCE_INTERVAL_US */
    uint32_t              peer_timeout_us;   /* drop a peer after this silence, 12 s */
    uint16_t              max_peers;         /* RANT_MAX_PEERS */
} RantNodeDiscovery;

/* The one event type, fired by every layer. Four lifecycle kinds plus RANT_ERROR, whose
 * .error says which fault. Read only the fields named for the kind. */
typedef enum {
    RANT_PEER_UP,          /* discovered or resumed: .peer, .ip, .ip_len, .port */
    RANT_PEER_DOWN,        /* lost or fell silent: .peer */
    RANT_PEER_INTEREST,    /* interest applied: .peer, .publish_topics, .receive_topics */
    RANT_MSG_LOST,         /* seqnos skipped: .topic, .peer, .lost_first, .lost_count. Not an error */
    RANT_ERROR             /* read .error and rant_event_str */
} RantEventKind;

/* The error carried by a RANT_ERROR event and returned by rant_last_error. Named RANT_E_*
 * to stay distinct from the RantResult return codes. docs/node.md lists them. */
typedef enum {
    RANT_E_NONE = 0,
    /* a match was refused, or advertised data cannot flow */
    RANT_E_NAME_COLLISION,     /* a peer's topic name hashes to ours but differs: .identity, .topic.
                                  Or, with .peer 0, a create found a live same name topic on this node */
    RANT_E_QOS_INCOMPATIBLE, /* a reliable subscriber met a best effort publisher: .topic, .peer */
    RANT_E_KIND_MISMATCH,      /* the name is another entity kind at a peer: .topic, .peer */
    RANT_E_SCHEMA_MISMATCH,    /* a refused match or an ill fitting message: .topic, .peer */
    RANT_E_INTEREST_OVERFLOW,/* a peer's index map failed to allocate: .peer, .lost_count entries */
    RANT_E_META_TRUNCATED_INTEREST, /* our overlay overflowed, peers see no topics */
    RANT_E_PEER_META_TOO_BIG,/* a peer's blob exceeds our buffer: .peer (0 = new), .too_big_bytes */
    RANT_E_MSG_TOO_BIG,        /* a received message could not be buffered: .too_big_bytes */
    RANT_E_PEER_REFUSED,       /* the peer table is full of active peers: .ip, .ip_len, .port */
    RANT_E_EVICTED_UNSENT,     /* a send overwrote unsent history: .topic, .lost_first, .lost_count */
    RANT_E_UNMATCHED_SEND,     /* a send committed to nobody while a match was resolving: .topic */
    RANT_E_DUPLICATE_AUTHORITY, /* a peer also claims the handler side: .topic, .peer. Diagnostic */
    /* IO and setup, mostly at open. .os_error carries the OS code */
    RANT_E_OOM,                /* the allocator returned NULL: .too_big_bytes = bytes needed */
    RANT_E_PLATFORM,           /* platform net init failed */
    RANT_E_SOCKET,             /* opening a UDP socket failed */
    RANT_E_BIND,               /* bind failed, the port is in use: .port */
    RANT_E_MCAST_JOIN,         /* joining the discovery group failed, a bad interface */
    RANT_E_SEND,               /* a send hard failed: .peer, .topic, .too_big_bytes = its size */
    RANT_E_RECV,               /* a receive hard failed */
    RANT_E_POLL,               /* the socket wait failed */
    RANT_E_WAKER,              /* no cross thread waker, wakes come at the next tick */
    RANT_E_BAD_ADDRESS,        /* a configured address could not be parsed, refused at open */
    RANT_E_BAD_NAME,           /* a create's name is empty, too long or carries '@': .topic_name */
    RANT_E_STATE,              /* a create refused in this state: from a callback, or the reserve is full */
    RANT_E_BAD_SCHEMA          /* rant_node_schema refused a text or wire: .schema_detail says why and where.
                                  Or a create's schema failed to parse: .topic_name */
} RantErrorKind;

typedef struct {
    RantEventKind kind;
    RantErrorKind error;         /* RANT_ERROR: which error, else RANT_E_NONE */
    const char *topic_name;  /* topic scoped events: our topic's name, valid for the callback */
    void       *user;          /* RantNodeOpts.user_data */
    uint32_t   peer;           /* the peer id, 0 = none */
    uint16_t   topic;        /* the local topic handle */
    int        os_error;       /* the OS code for SOCKET, BIND, MCAST_JOIN, SEND, RECV and POLL */
    uint8_t    ip[16];         /* the peer address, network order */
    uint8_t    ip_len;         /* 4 or 16, else 0 */
    uint16_t   port;
    uint64_t   lost_first;     /* MSG_LOST and EVICTED_UNSENT: the first seqno */
    uint64_t   lost_count;     /* the count, or INTEREST_OVERFLOW's entries */
    uint64_t   too_big_bytes;  /* MSG_TOO_BIG, PEER_META_TOO_BIG, OOM and SEND: the byte count */
    uint64_t   identity;       /* NAME_COLLISION: the colliding identity */
    uint16_t   publish_topics; /* PEER_INTEREST: topics we now publish to this peer */
    uint16_t   receive_topics; /* PEER_INTEREST: topics we now receive from it */
    const char *schema_detail; /* SCHEMA_MISMATCH: one line saying what was incompatible, or NULL */
    const char *peer_name;     /* the peer's node name, NULL when unknown. Prefer it over .peer */
} RantEvent;
typedef void (*RantEventFn)(const RantEvent *ev);

#ifdef __cplusplus
}
#endif
#endif /* RANT_TYPES_H */
