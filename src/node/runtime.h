/* The node runtime and the public ramble_node_* and ramble_topic_* API. It owns the sockets
 * and the clock and drives the cores. docs/node.md explains how to use it. */
#ifndef RAMBLE_NODE_H
#define RAMBLE_NODE_H

#include "../common/api.h"
#include "../transport/core.h"
#include "../discovery/core.h"
#include "../serialize/schema.h"
#ifndef RAMBLE_NO_STDTYPES
#include "../serialize/stdtypes.h"
#endif
#include "../common/alloc.h"
#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Network addressing and sockets. Every field is zero means default. docs/discovery.md
 * explains the options. */
typedef struct {
    uint16_t              data_port;         /* the unicast data port, 0 = OS assigned */
    const char           *discovery_group;   /* "239.255.0.7" */
    uint16_t              discovery_port;    /* 7400 */
    const char           *multicast_interface;/* pin discovery to this interface IP. NULL = every
                                                interface, "127.0.0.1" = single host */
    uint8_t               multicast_ttl;     /* hops an announce may travel, 1 */
    const RambleDiscoveryAddr *seed_peers;   /* unicast announces here too, port 0 = discovery_port */
    uint16_t              n_seed_peers;
    uint8_t               unicast_only;      /* 1 = no group join, seeds and relays only. Also for a
                                                node behind an outbound only NAT */
    uint32_t              recv_buffer_bytes; /* the data socket SO_RCVBUF, 0 = OS default */
    uint32_t              send_buffer_bytes; /* the data socket SO_SNDBUF, 0 = OS default */
    uint16_t              fragment_size;     /* UDP payload bytes per fragment, 0 = RAMBLE_FRAG_SIZE.
                                                Clamped to [MIN, MAX]. One size per node */
    /* Stating our own locator. The default states nothing and peers record the source an
     * announce arrived from. Use these for a static one to one mapping. */
    const char           *self_ip;           /* advertise this IPv4 address. Unparseable refuses the
                                                open with RAMBLE_E_BAD_ADDRESS */
    uint16_t              advertise_port;    /* advertise this data port instead of the bound one */
} RambleNodeNet;

/* Discovery cadence and the peer table size, zero means default. */
typedef struct {
    uint32_t              announce_interval_us; /* 3 s */
    uint32_t              peer_timeout_us;   /* drop a peer after this silence, 12 s */
    uint16_t              max_peers;         /* 16 */
} RambleNodeDiscovery;

#ifndef RAMBLE_MATCH_WAIT_MS
#define RAMBLE_MATCH_WAIT_MS 1000   /* the default send path match wait bound */
#endif

/* The node options, passed as a compound literal. NULL means all defaults. */
typedef struct {
    uint16_t              domain;
    uint16_t              max_topics;  /* topics that can be created, 0 = 8 */
    void                 *user_data;     /* RambleMsg.user and RambleEvent.user */
    uint8_t               disable_shm;   /* 1 = never use the same host shared memory path */
    uint8_t               fetch_details; /* 1 = fetch and cache every topic every peer advertises,
                                            for observer UIs. Costs memory per peer topic */
    int32_t               match_wait_ms; /* the send path match wait bound. 0 = RAMBLE_MATCH_WAIT_MS,
                                            negative = disabled and such a send fires UNMATCHED_SEND */
    uint8_t               disable_logs;  /* 1 = no @ramble/log topics, ramble_node_log gives NOSYS */
    uint8_t               disable_meta;  /* 1 = no built in @ramble/meta function */
    uint8_t               disable_error_logs; /* 1 = do not mirror errors onto @ramble/log/error */
    RambleNodeNet         net;
    RambleNodeDiscovery   discovery;
} RambleNodeOpts;

/* Per topic options, passed as a compound literal. */
typedef struct {
    RambleQos  qos;
    uint8_t    reflect_from_mesh;  /* fill what was left unspecified from the mesh: a NULL schema and
                                    a zero reliability follow it. See docs/reflection.md */
} RambleTopicOpts;

typedef struct RambleNode    RambleNode;
typedef struct RambleTopic RambleTopic;   /* an opaque handle, stable for the node's life */

/* A delivered message. From the callback, send and read only queries on the same node
 * are allowed. poll, create, set_role, drain, start, stop and close are refused. */
typedef struct {
    RambleNode      *node;
    void            *user;             /* RambleNodeOpts.user_data */
    uint16_t         topic_index;
    uint32_t         publisher_id;
    RambleString     publisher_name;      /* never NULL data on delivery, "unknown-peer" at worst. A
                                        view valid for the callback */
    RambleString     topic_name;     /* {NULL,0} if unknown */
    RambleBytes      header;           /* pattern header bytes in front of the payload, {NULL,0} on a
                                        plain topic */
    RambleBytes      data;             /* the payload after the header */
    const RambleSchema *schema;        /* the schema data decodes with, validated before delivery.
                                        NULL on a raw topic or an untyped publisher */
    uint64_t       recv_us;          /* the monotonic clock at receive, or at enqueue */
    uint64_t       written_us;       /* the writer's wall clock at commit, UTC microseconds. 0 = the
                                        publisher opted out. Never mix it with recv_us */
    uint64_t       capture_us;       /* when the publisher says the data was true, UTC
                                        microseconds. 0 = none given, written_us is all there is */
} RambleMsg;
typedef void (*RambleMsgFn)(const RambleMsg *msg);

/* The process heap as an allocator, for a caller with no page source of its own.
 * page_size 0 is the default. spec/allocation.md explains the modes. */
RAMBLE_API RambleAllocator ramble_allocator_heap(uint32_t page_size);
/* The process heap as a RambleAllocFn, for ramble_schema_compile and ramble_schema_free. */
RAMBLE_API void         *ramble_heap_realloc(void *user, void *ptr, size_t size);

/* Opens a node on alloc, which it copies and resets on close, so use one per node. alloc
 * is required and NULL refuses the open with RAMBLE_E_OOM. name NULL gives an auto name,
 * on_message and on_event may be NULL, opts NULL means defaults. */
RAMBLE_API RambleNode    *ramble_node_open(RambleAllocator *alloc, const char *name, RambleMsgFn on_message,
                                     RambleEventFn on_event, const RambleNodeOpts *opts);
/* The most recent error. n NULL returns the process global slot, the reason an open
 * returned NULL. .error is RAMBLE_E_NONE if none occurred. */
RAMBLE_API RambleEvent    ramble_last_error(RambleNode *n);
/* One loop tick, blocking up to timeout_ms. RAMBLE_ERR_STATE while a service thread runs
 * or from inside a callback. */
RAMBLE_API int          ramble_node_poll(RambleNode *n, int timeout_ms);
/* Stops the service thread, then tears the node down. RAMBLE_ERR_STATE from a callback. */
RAMBLE_API int          ramble_node_close(RambleNode *n, int send_bye);

/* Threading: every call is thread safe under one node lock never held across a wait.
 * Drive the node with ramble_node_poll or with ramble_node_start. See docs/node.md. */
/* Runs the background service thread. RAMBLE_ERR_NOSYS when threads are compiled out. */
RAMBLE_API int          ramble_node_start(RambleNode *n);
/* Stops and joins the service thread. Idempotent, implied by close. */
RAMBLE_API int          ramble_node_stop(RambleNode *n);
/* 1 while the service thread runs. */
RAMBLE_API int          ramble_node_is_started(RambleNode *n);
/* Hold the node lock from a non callback thread around reads of a view. Hold it briefly.
 * No ops from a callback and when threads are compiled out. */
RAMBLE_API void         ramble_node_lock(RambleNode *n);
RAMBLE_API void         ramble_node_unlock(RambleNode *n);
/* Sends that evicted never sent history after the bounded wait, since open. */
RAMBLE_API uint32_t     ramble_node_evicted_unsent(RambleNode *n);

#ifndef RAMBLE_NO_STDTYPES
/* The two standard type values that need a platform: the wall clock a Timestamp counts
 * from, and a random version 4 Uuid. Neither needs a node. */
RAMBLE_API RambleTimestamp ramble_timestamp_now(void);
RAMBLE_API void            ramble_uuid_new(RambleUuid *out);
#endif

/* Creates a topic. name is the identity, schema is copied in, NULL means raw bytes. A
 * retired slot is reused. NULL when the reserve is full, the name is bad or out of memory. */
RAMBLE_API RambleTopic *ramble_node_create_topic(RambleNode *n, const char *name, RambleRole role,
                                               const RambleSchema *schema, const RambleTopicOpts *opts);

/* Retires a topic and frees the handle, invalid after RAMBLE_OK. RAMBLE_ERR_STATE from a
 * callback, for a pattern channel or a builtin, or during a dispatch. See docs/topics.md. */
RAMBLE_API int ramble_topic_retire(RambleTopic *topic);
/* A reflect_from_mesh topic: re reads the mesh and re types the handle in place when the
 * provider moved. 1 re created, 0 current, negative on error. */
RAMBLE_API int ramble_topic_refresh(RambleTopic *topic);

/* Consumer queues: a topic becomes queued on its first take or dispatch, or from creation
 * with qos.queue_bytes. docs/node.md explains the rules. */
#ifndef RAMBLE_QUEUE_CAP
#define RAMBLE_QUEUE_CAP (1u << 20)   /* the default queue growth cap, bytes per topic */
#endif
/* Pops the next queued message. The views stay valid until the next take or dispatch.
 * timeout_ms 0 checks, positive waits, negative waits forever. 1 got one, 0 empty. */
RAMBLE_API int ramble_topic_take(RambleTopic *topic, RambleMsg *out, int timeout_ms);
/* Runs on_message on the calling thread for up to max_msgs queued messages (0 = all),
 * after waiting like take. Returns the count. The callbacks run without the node lock. */
RAMBLE_API int ramble_topic_dispatch(RambleTopic *topic, int max_msgs, int timeout_ms);
/* dispatch across every already queued topic, oldest first per topic. */
RAMBLE_API int ramble_node_dispatch(RambleNode *n, int max_msgs, int timeout_ms);
/* Messages waiting, ring bytes used and capacity, and messages dropped since open. */
RAMBLE_API void ramble_topic_queue_stats(RambleTopic *topic, uint32_t *msgs, uint32_t *bytes,
                                       uint32_t *capacity, uint32_t *dropped);
/* Publishes to every matched subscriber. RAMBLE_OK or a negative RambleResult. */
/* Optional per send config, NULL means all defaults. capture_us is when the data was
 * true rather than when it was sent, and costs 8 wire bytes only when it is set. */
typedef struct {
    uint64_t capture_us;   /* UTC microseconds, the ramble_timestamp_now clock. 0 = none */
} RambleSendOpts;

RAMBLE_API int          ramble_topic_send(RambleTopic *topic, RambleBytes data, const RambleSendOpts *opts);
/* Flips the role at runtime and re advertises. 0 ok, negative on error. */
RAMBLE_API int          ramble_topic_set_role(RambleTopic *topic, RambleRole role);
/* The local index, RambleMsg.topic_index for its messages. */
RAMBLE_API uint16_t     ramble_topic_index(const RambleTopic *topic);
/* The schema as passed to create, NULL for a raw topic. */
RAMBLE_API const RambleSchema *ramble_topic_schema(const RambleTopic *topic);
/* The handle by creation index, NULL if out of range. */
RAMBLE_API RambleTopic *ramble_node_topic(RambleNode *n, uint16_t index);

/* Reflection: three walks over one zero initialized RambleIter, called until 0. Every view
 * is valid until the next poll. docs/reflection.md explains them. */

/* One discovered peer per call, dropped peers included. Gate on liveness. */
RAMBLE_API int ramble_node_peers_next(RambleNode *n, RambleIter *it, RamblePeerInfo *out);

/* The entities one node advertises. RAMBLE_SELF walks this node, a dropped peer serves its
 * last known view. */
RAMBLE_API int ramble_node_entities_next(RambleNode *n, uint32_t peer, RambleIter *it, RambleEntityInfo *out);

/* The whole mesh folded, one entity per kind and name across every active peer and this node. */
RAMBLE_API int ramble_node_mesh_next(RambleNode *n, RambleIter *it, RambleEntityInfo *out);
RAMBLE_API int ramble_node_mesh_find(RambleNode *n, RambleEntityKind kind, const char *name, RambleEntityInfo *out);

/* Bumps on every reflected change anywhere. Re walk iff it moved. */
RAMBLE_API uint32_t ramble_node_mesh_epoch(RambleNode *n);

/* Microseconds waited on slow subscribers and how many sends waited, since open. */
RAMBLE_API void     ramble_node_backpressure_stats(RambleNode *n, uint64_t *waited_us, uint32_t *waited_sends);

/* Live bytes, the high water mark and the count of heap allocations. A flat count over a
 * window proves the hot path is allocation free. */
RAMBLE_API void     ramble_node_mem_stats(RambleNode *n, size_t *in_use, size_t *peak, uint64_t *alloc_calls);
/* The topic's cumulative repair counters. Zeroed for a NULL topic. */
RAMBLE_API void     ramble_topic_repair_stats(RambleTopic *topic, RambleRepairStats *out);

/* The in pump probe: sampled on a timer inside a blocked reliable send, so a stall becomes
 * a time series of repair progress. Observational only. */
typedef struct {
    uint16_t topic;
    uint64_t wait_elapsed_us; /* since this backpressure wait began */
    uint64_t interval_us;     /* since the previous sample */
    uint64_t frags_resent;    /* in the interval */
    uint64_t nacks_recv;      /* in the interval */
    uint32_t polls;           /* poll calls in the interval */
    uint32_t polls_idle;      /* of those, polls with no repair pending */
} RamblePumpSample;
typedef void (*RamblePumpProbeFn)(void *user, const RamblePumpSample *s);
/* Registers the probe, NULL disables. interval_us 0 = 200 ms. */
RAMBLE_API void     ramble_node_set_pump_probe(RambleNode *n, RamblePumpProbeFn fn, uint64_t interval_us, void *user);
/* The in progress message from peer: 1 and its base seqno, fragments held and needed. */
RAMBLE_API int      ramble_topic_subscriber_progress(RambleTopic *topic, uint32_t peer,
                                     uint64_t *base_seqno, uint32_t *have, uint32_t *total);
/* Waits until every subscriber acked everything, or timeout_ms. 1 drained, 0 on timeout
 * or from a callback. Call before close so a burst is not cut by the BYE. */
RAMBLE_API int      ramble_topic_drain(RambleTopic *topic, int timeout_ms);
/* Subscribers matched now. */
RAMBLE_API int      ramble_topic_match_count(RambleTopic *topic);

/* The send path match wait: a send that would reach nobody while a match is still
 * resolving blocks up to opts.match_wait_ms. See docs/topics.md and spec/interest.md. */
/* Unresolved candidate matches right now. 0 = converged for everyone known. */
RAMBLE_API int      ramble_topic_pending_count(RambleTopic *topic);
/* 1 when a send would not wait: matched, or converged with nobody to wait for. The async
 * form for a GUI that disables the wait. */
RAMBLE_API int      ramble_topic_ready(RambleTopic *topic);

/* Blocks until discovery and matching settle. Call it after creating topics. timeout_ms
 * negative = 3 announce intervals. 1 settled, 0 on timeout or from a callback. */
RAMBLE_API int      ramble_node_settle(RambleNode *n, int timeout_ms);
#ifdef RAMBLE_SHM
/* Messages published and delivered through shared memory since open. */
RAMBLE_API void     ramble_node_shm_stats(RambleNode *n, uint32_t *sent, uint32_t *recv);
#endif

/* Messages and bytes this node committed to the topic and received on it, since open. */
RAMBLE_API void     ramble_topic_counts(RambleTopic *topic, uint64_t *tx_msgs, uint64_t *tx_bytes,
                                    uint64_t *rx_msgs, uint64_t *rx_bytes);

/* The built in @ramble/log/{error,warn,info} topics. docs/node.md explains them. */
typedef enum { RAMBLE_LOG_ERROR = 0, RAMBLE_LOG_WARN = 1, RAMBLE_LOG_INFO = 2 } RambleLogLevel;
#ifndef RAMBLE_LOG_MAX
#define RAMBLE_LOG_MAX 512   /* formatted log text bytes, longer is truncated */
#endif
/* printf style publish on the level's topic. RAMBLE_ERR_NOSYS when the logs are disabled. */
RAMBLE_API int          ramble_node_log(RambleNode *n, RambleLogLevel level, const char *fmt, ...);
/* Publishes an already formatted line, len negative = NUL terminated. The FFI entry. */
RAMBLE_API int          ramble_node_log_text(RambleNode *n, RambleLogLevel level, const char *text, int len);
/* The node's own handle for a level, NULL when disabled. */
RAMBLE_API RambleTopic   *ramble_node_log_topic(RambleNode *n, RambleLogLevel level);

/* The @ramble/meta snapshot section mask, a 4 byte LE u32 request payload, 0 = everything.
 * The response is RambleMeta { info: map } with one key per section. See docs/node.md. */
#define RAMBLE_META_NODE   0x1u
#define RAMBLE_META_PROC   0x2u
#define RAMBLE_META_TOPICS 0x4u
#define RAMBLE_META_PEERS  0x8u

/* Internal hooks for the patterns layer: create a topic carrying an entity kind, route its
 * messages to a handler, and observe node wide events and a per poll tick. */
typedef void     (*i_RambleSysMsgFn)(void *user, const RambleMsg *msg);
typedef void     (*i_RambleSysEventFn)(void *user, const RambleEvent *ev);
typedef uint64_t (*i_RambleSysTickFn)(void *user, uint64_t now_us);   /* next deadline, 0 = none */
typedef void     (*i_RambleSysCloseFn)(void *user);   /* the node is closing: settle promises */

/* Creates a pattern topic: stamps the kind, prefix, directed flag and attrs, permits '@' in
 * the name, and routes deliveries to on_msg. Never queued. */
RambleTopic *i_ramble_node_create_pattern_topic(RambleNode *n, const char *name, RambleRole role,
                              const RambleSchema *schema, const RambleTopicOpts *opts,
                              uint8_t kind, uint8_t prefix_bytes, uint8_t directed, uint8_t attrs,
                              i_RambleSysMsgFn on_msg, void *on_msg_user);
/* Re types a live pattern channel in place, the engine of ramble_topic_refresh. Lock not held. */
int  i_ramble_topic_retype(RambleTopic *topic, const RambleSchema *schema, uint8_t reliability);
/* The reflect_from_mesh pick for one channel (which: 0 primary, 1 rsp, 2 prg). */
int  i_ramble_node_reflect_pick(RambleNode *n, RambleEntityKind kind, const char *name, int which, int writer,
                              const RambleSchema **schema, uint8_t *reliable, uint64_t *generation);
/* Publishes hdr plus payload on a pattern topic to every matched subscriber. */
int  i_ramble_topic_send_hdr(RambleTopic *topic, RambleBytes hdr, RambleBytes data);
/* Publishes hdr plus payload to one peer, for function replies. */
int  i_ramble_topic_send_to(RambleTopic *topic, uint32_t to_peer, RambleBytes hdr, RambleBytes data);
/* Clears a pattern topic's routing before its handle is freed. Under the node lock. */
void i_ramble_topic_clear_sys(RambleTopic *topic);
/* Hands every committed but unsent datagram to the wire now, transmit only. The pattern
 * layer's retire and close flush their CANCELLED replies here. Takes the node lock. */
void i_ramble_node_flush_tx(RambleNode *n);
/* The topic's next seqno. The slot line continues across retire and reuse, so a pattern
 * layer seeds its own monotonic counters from it. */
uint64_t i_ramble_topic_seqno(RambleTopic *topic);
/* The send path match wait without the send or the event: the caller derives its own
 * verdict. Returns the matched count. No wait from a callback or once converged. */
int  i_ramble_topic_match_wait(RambleTopic *topic);
/* Registers the patterns layer's event observer, per poll tick and close hook, NULL clears.
 * The close hook fires once at the top of close, with the node still fully alive. */
void i_ramble_node_set_sys_hooks(RambleNode *n, i_RambleSysEventFn on_event, i_RambleSysTickFn tick,
                               i_RambleSysCloseFn on_close, void *user);
/* Node pool allocation (size 0 frees), the layer's per node handle slot, the monotonic
 * clock and the wall clock the transport stamps written_us from. Under the node lock. */
void    *i_ramble_node_sys_alloc(RambleNode *n, void *ptr, size_t size);
void   **i_ramble_node_sys_slot (RambleNode *n);
uint64_t i_ramble_node_now_us   (RambleNode *n);
uint64_t i_ramble_node_wall_us  (RambleNode *n);
/* The node lock for a pattern call that mutates state: 1 = acquired here, 0 = already held
 * by this thread. sys_unlock never kicks. sys_poll drives one loop tick. */
int      i_ramble_node_sys_lock  (RambleNode *n);
void     i_ramble_node_sys_unlock(RambleNode *n, int acquired);
int      i_ramble_node_sys_poll  (RambleNode *n, int timeout_ms);
/* Fires a topic scoped RAMBLE_ERROR through the node's normal event path. Under the node lock. */
void     i_ramble_node_sys_error (RambleNode *n, RambleErrorKind error, RambleTopic *topic, uint32_t peer);
/* Matched subscribers excluding dormant peers, the provider liveness query. */
int      i_ramble_topic_live_match_count(RambleTopic *topic);
/* Is peer a matched subscriber of this PUB topic. The directed call severed lane backstop. */
int      i_ramble_topic_peer_matched(RambleTopic *topic, uint32_t peer);
/* Matched publishers feeding this topic's subscription side. */
int      i_ramble_topic_source_match_count(RambleTopic *topic);
/* The oldest live matched subscriber's peer id, 0 = none. The task layer's auto direct target. */
uint32_t i_ramble_topic_oldest_match(RambleTopic *topic);
/* This node's discovery uuid, stable for its lifetime. The task layer's progress demux filter. */
const uint8_t *i_ramble_node_uuid(RambleNode *n);
/* Reflection getters for the patterns layer's entity enumeration. */
uint8_t      i_ramble_topic_kind (const RambleTopic *topic);   /* RambleTopicKind */
uint8_t      i_ramble_topic_role (const RambleTopic *topic);   /* the current RambleRole */
RambleString i_ramble_topic_name (const RambleTopic *topic);   /* the stable name copy */
uint8_t      i_ramble_topic_reliability(const RambleTopic *topic);   /* the create qos reliability */
const uint8_t *i_ramble_node_peer_uuid(RambleNode *n, uint32_t peer);   /* NULL if unknown, a view */
uint16_t   i_ramble_node_topic_count(RambleNode *n);         /* one past the highest defined index */
/* Builds the RAMBLE_META_* snapshot (0 = all sections) as a map body into a node owned grown
 * buffer. A view valid until the next call, {NULL,0} on OOM. Under the node lock. */
RambleBytes i_ramble_node_snapshot(RambleNode *n, uint32_t sections);

#ifdef __cplusplus
}
#endif
#endif /* RAMBLE_NODE_H */
