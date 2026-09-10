/* The node runtime and the public dart_node_* and dart_topic_* API. It owns the sockets
 * and the clock and drives the cores. docs/node.md explains how to use it. */
#ifndef DART_NODE_H
#define DART_NODE_H

#include "../transport/core.h"
#include "../discovery/core.h"
#include "../serialize/schema.h"
#ifndef DART_NO_STDTYPES
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
    const DartDiscoveryAddr *seed_peers;   /* unicast announces here too, port 0 = discovery_port */
    uint16_t              n_seed_peers;
    uint8_t               unicast_only;      /* 1 = no group join, seeds and relays only. Also for a
                                                node behind an outbound only NAT */
    uint32_t              recv_buffer_bytes; /* the data socket SO_RCVBUF, 0 = OS default */
    uint32_t              send_buffer_bytes; /* the data socket SO_SNDBUF, 0 = OS default */
    uint16_t              fragment_size;     /* UDP payload bytes per fragment, 0 = DART_FRAG_SIZE.
                                                Clamped to [MIN, MAX]. One size per node */
    /* Stating our own locator. The default states nothing and peers record the source an
     * announce arrived from. Use these for a static one to one mapping. */
    const char           *self_ip;           /* advertise this IPv4 address. Unparseable refuses the
                                                open with DART_E_BAD_ADDRESS */
    uint16_t              advertise_port;    /* advertise this data port instead of the bound one */
} DartNodeNet;

/* Discovery cadence and the peer table size, zero means default. */
typedef struct {
    uint32_t              announce_interval_us; /* 3 s */
    uint32_t              peer_timeout_us;   /* drop a peer after this silence, 12 s */
    uint16_t              max_peers;         /* 16 */
} DartNodeDiscovery;

#ifndef DART_MATCH_WAIT_MS
#define DART_MATCH_WAIT_MS 1000   /* the default send path match wait bound */
#endif

/* The node options, passed as a compound literal. NULL means all defaults. */
typedef struct {
    uint16_t              domain;
    uint16_t              max_topics;  /* topics that can be created, 0 = 8 */
    void                 *user_data;     /* DartMsg.user and DartEvent.user */
    uint8_t               disable_shm;   /* 1 = never use the same host shared memory path */
    uint8_t               fetch_details; /* 1 = fetch and cache every topic every peer advertises,
                                            for observer UIs. Costs memory per peer topic */
    int32_t               match_wait_ms; /* the send path match wait bound. 0 = DART_MATCH_WAIT_MS,
                                            negative = disabled and such a send fires UNMATCHED_SEND */
    uint8_t               disable_logs;  /* 1 = no @dart/log topics, dart_node_log gives NOSYS */
    uint8_t               disable_meta;  /* 1 = no built in @dart/meta function */
    uint8_t               disable_error_logs; /* 1 = do not mirror errors onto @dart/log/error */
    DartNodeNet         net;
    DartNodeDiscovery   discovery;
} DartNodeOpts;

/* Per topic options, passed as a compound literal. */
typedef struct {
    DartQos  qos;
    uint8_t  reflect_from_mesh;  /* fill what was left unspecified from the mesh: a NULL schema and
                                    a zero reliability follow it. See docs/reflection.md */
} DartTopicOpts;

typedef struct DartNode    DartNode;
typedef struct DartTopic DartTopic;   /* an opaque handle, stable for the node's life */

/* A delivered message. From the callback, send and read only queries on the same node
 * are allowed. poll, create, set_role, drain, start, stop and close are refused. */
typedef struct {
    DartNode      *node;
    void          *user;             /* DartNodeOpts.user_data */
    uint16_t       topic_index;
    uint32_t       publisher_id;
    DartString     publisher_name;      /* never NULL data on delivery, "unknown-peer" at worst. A
                                        view valid for the callback */
    DartString     topic_name;     /* {NULL,0} if unknown */
    DartBytes      header;           /* pattern header bytes in front of the payload, {NULL,0} on a
                                        plain topic */
    DartBytes      data;             /* the payload after the header */
    const DartSchema *schema;        /* the schema data decodes with, validated before delivery.
                                        NULL on a raw topic or an untyped publisher */
    uint64_t       recv_us;          /* the monotonic clock at receive, or at enqueue */
    uint64_t       written_us;       /* the writer's wall clock at commit, UTC microseconds. 0 = the
                                        publisher opted out. Never mix it with recv_us */
    uint64_t       capture_us;       /* when the publisher says the data was true, UTC
                                        microseconds. 0 = none given, written_us is all there is */
} DartMsg;
typedef void (*DartMsgFn)(const DartMsg *msg);

/* Opens a node on alloc, which it copies and resets on close, so use one per node. name
 * NULL gives an auto name, on_message and on_event may be NULL, opts NULL means defaults. */
DartNode    *dart_node_open(DartAllocator *alloc, const char *name, DartMsgFn on_message,
                            DartEventFn on_event, const DartNodeOpts *opts);
/* The most recent error. n NULL returns the process global slot, the reason an open
 * returned NULL. .error is DART_E_NONE if none occurred. */
DartEvent    dart_last_error(DartNode *n);
/* One loop tick, blocking up to timeout_ms. DART_ERR_STATE while a service thread runs
 * or from inside a callback. */
int          dart_node_poll(DartNode *n, int timeout_ms);
/* Stops the service thread, then tears the node down. DART_ERR_STATE from a callback. */
int          dart_node_close(DartNode *n, int send_bye);

/* Threading: every call is thread safe under one node lock never held across a wait.
 * Drive the node with dart_node_poll or with dart_node_start. See docs/node.md. */
/* Runs the background service thread. DART_ERR_NOSYS when threads are compiled out. */
int          dart_node_start(DartNode *n);
/* Stops and joins the service thread. Idempotent, implied by close. */
int          dart_node_stop(DartNode *n);
/* 1 while the service thread runs. */
int          dart_node_is_started(DartNode *n);
/* Hold the node lock from a non callback thread around reads of a view. Hold it briefly.
 * No ops from a callback and when threads are compiled out. */
void         dart_node_lock(DartNode *n);
void         dart_node_unlock(DartNode *n);
/* Sends that evicted never sent history after the bounded wait, since open. */
uint32_t     dart_node_evicted_unsent(DartNode *n);

#ifndef DART_NO_STDTYPES
/* The two standard type values that need a platform: the wall clock a Timestamp counts
 * from, and a random version 4 Uuid. Neither needs a node. */
DartTimestamp dart_timestamp_now(void);
void          dart_uuid_new(DartUuid *out);
#endif

/* Creates a topic. name is the identity, schema is copied in, NULL means raw bytes. A
 * retired slot is reused. NULL when the reserve is full, the name is bad or out of memory. */
DartTopic *dart_node_create_topic(DartNode *n, const char *name, DartRole role,
                                      const DartSchema *schema, const DartTopicOpts *opts);

/* Retires a topic and frees the handle, invalid after DART_OK. DART_ERR_STATE from a
 * callback, for a pattern channel or a builtin, or during a dispatch. See docs/topics.md. */
int dart_topic_retire(DartTopic *topic);
/* A reflect_from_mesh topic: re reads the mesh and re types the handle in place when the
 * provider moved. 1 re created, 0 current, negative on error. */
int dart_topic_refresh(DartTopic *topic);

/* Consumer queues: a topic becomes queued on its first take or dispatch, or from creation
 * with qos.queue_bytes. docs/node.md explains the rules. */
#ifndef DART_QUEUE_CAP
#define DART_QUEUE_CAP (1u << 20)   /* the default queue growth cap, bytes per topic */
#endif
/* Pops the next queued message. The views stay valid until the next take or dispatch.
 * timeout_ms 0 checks, positive waits, negative waits forever. 1 got one, 0 empty. */
int dart_topic_take(DartTopic *topic, DartMsg *out, int timeout_ms);
/* Runs on_message on the calling thread for up to max_msgs queued messages (0 = all),
 * after waiting like take. Returns the count. The callbacks run without the node lock. */
int dart_topic_dispatch(DartTopic *topic, int max_msgs, int timeout_ms);
/* dispatch across every already queued topic, oldest first per topic. */
int dart_node_dispatch(DartNode *n, int max_msgs, int timeout_ms);
/* Messages waiting, ring bytes used and capacity, and messages dropped since open. */
void dart_topic_queue_stats(DartTopic *topic, uint32_t *msgs, uint32_t *bytes,
                              uint32_t *capacity, uint32_t *dropped);
/* Publishes to every matched subscriber. DART_OK or a negative DartResult. */
/* Optional per send config, NULL means all defaults. capture_us is when the data was
 * true rather than when it was sent, and costs 8 wire bytes only when it is set. */
typedef struct {
    uint64_t capture_us;   /* UTC microseconds, the dart_timestamp_now clock. 0 = none */
} DartSendOpts;

int          dart_topic_send(DartTopic *topic, DartBytes data, const DartSendOpts *opts);
/* Flips the role at runtime and re advertises. 0 ok, negative on error. */
int          dart_topic_set_role(DartTopic *topic, DartRole role);
/* The local index, DartMsg.topic_index for its messages. */
uint16_t     dart_topic_index(const DartTopic *topic);
/* The schema as passed to create, NULL for a raw topic. */
const DartSchema *dart_topic_schema(const DartTopic *topic);
/* The handle by creation index, NULL if out of range. */
DartTopic *dart_node_topic(DartNode *n, uint16_t index);

/* Reflection: three walks over one zero initialized DartIter, called until 0. Every view
 * is valid until the next poll. docs/reflection.md explains them. */

/* One discovered peer per call, dropped peers included. Gate on liveness. */
int dart_node_peers_next(DartNode *n, DartIter *it, DartPeerInfo *out);

/* The entities one node advertises. DART_SELF walks this node, a dropped peer serves its
 * last known view. */
int dart_node_entities_next(DartNode *n, uint32_t peer, DartIter *it, DartEntityInfo *out);

/* The whole mesh folded, one entity per kind and name across every active peer and this node. */
int dart_node_mesh_next(DartNode *n, DartIter *it, DartEntityInfo *out);
int dart_node_mesh_find(DartNode *n, DartEntityKind kind, const char *name, DartEntityInfo *out);

/* Bumps on every reflected change anywhere. Re walk iff it moved. */
uint32_t dart_node_mesh_epoch(DartNode *n);

/* Microseconds waited on slow subscribers and how many sends waited, since open. */
void     dart_node_backpressure_stats(DartNode *n, uint64_t *waited_us, uint32_t *waited_sends);

/* Live bytes, the high water mark and the count of heap allocations. A flat count over a
 * window proves the hot path is allocation free. */
void     dart_node_mem_stats(DartNode *n, size_t *in_use, size_t *peak, uint64_t *alloc_calls);
/* The topic's cumulative repair counters. Zeroed for a NULL topic. */
void     dart_topic_repair_stats(DartTopic *topic, DartRepairStats *out);

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
} DartPumpSample;
typedef void (*DartPumpProbeFn)(void *user, const DartPumpSample *s);
/* Registers the probe, NULL disables. interval_us 0 = 200 ms. */
void     dart_node_set_pump_probe(DartNode *n, DartPumpProbeFn fn, uint64_t interval_us, void *user);
/* The in progress message from peer: 1 and its base seqno, fragments held and needed. */
int      dart_topic_subscriber_progress(DartTopic *topic, uint32_t peer,
                            uint64_t *base_seqno, uint32_t *have, uint32_t *total);
/* Waits until every subscriber acked everything, or timeout_ms. 1 drained, 0 on timeout
 * or from a callback. Call before close so a burst is not cut by the BYE. */
int      dart_topic_drain(DartTopic *topic, int timeout_ms);
/* Subscribers matched now. */
int      dart_topic_match_count(DartTopic *topic);

/* The send path match wait: a send that would reach nobody while a match is still
 * resolving blocks up to opts.match_wait_ms. See docs/topics.md and spec/interest.md. */
/* Unresolved candidate matches right now. 0 = converged for everyone known. */
int      dart_topic_pending_count(DartTopic *topic);
/* 1 when a send would not wait: matched, or converged with nobody to wait for. The async
 * form for a GUI that disables the wait. */
int      dart_topic_ready(DartTopic *topic);

/* Blocks until discovery and matching settle. Call it after creating topics. timeout_ms
 * negative = 3 announce intervals. 1 settled, 0 on timeout or from a callback. */
int      dart_node_settle(DartNode *n, int timeout_ms);
#ifdef DART_SHM
/* Messages published and delivered through shared memory since open. */
void     dart_node_shm_stats(DartNode *n, uint32_t *sent, uint32_t *recv);
#endif

/* Messages and bytes this node committed to the topic and received on it, since open. */
void     dart_topic_counts(DartTopic *topic, uint64_t *tx_msgs, uint64_t *tx_bytes,
                           uint64_t *rx_msgs, uint64_t *rx_bytes);

/* The built in @dart/log/{error,warn,info} topics. docs/node.md explains them. */
typedef enum { DART_LOG_ERROR = 0, DART_LOG_WARN = 1, DART_LOG_INFO = 2 } DartLogLevel;
#ifndef DART_LOG_MAX
#define DART_LOG_MAX 512   /* formatted log text bytes, longer is truncated */
#endif
/* printf style publish on the level's topic. DART_ERR_NOSYS when the logs are disabled. */
int          dart_node_log(DartNode *n, DartLogLevel level, const char *fmt, ...);
/* Publishes an already formatted line, len negative = NUL terminated. The FFI entry. */
int          dart_node_log_text(DartNode *n, DartLogLevel level, const char *text, int len);
/* The node's own handle for a level, NULL when disabled. */
DartTopic   *dart_node_log_topic(DartNode *n, DartLogLevel level);

/* The @dart/meta snapshot section mask, a 4 byte LE u32 request payload, 0 = everything.
 * The response is DartMeta { info: map } with one key per section. See docs/node.md. */
#define DART_META_NODE   0x1u
#define DART_META_PROC   0x2u
#define DART_META_TOPICS 0x4u
#define DART_META_PEERS  0x8u

/* Internal hooks for the patterns layer: create a topic carrying an entity kind, route its
 * messages to a handler, and observe node wide events and a per poll tick. */
typedef void     (*i_DartSysMsgFn)(void *user, const DartMsg *msg);
typedef void     (*i_DartSysEventFn)(void *user, const DartEvent *ev);
typedef uint64_t (*i_DartSysTickFn)(void *user, uint64_t now_us);   /* next deadline, 0 = none */
typedef void     (*i_DartSysCloseFn)(void *user);   /* the node is closing: settle promises */

/* Creates a pattern topic: stamps the kind, prefix, directed flag and attrs, permits '@' in
 * the name, and routes deliveries to on_msg. Never queued. */
DartTopic *i_dart_node_create_pattern_topic(DartNode *n, const char *name, DartRole role,
                              const DartSchema *schema, const DartTopicOpts *opts,
                              uint8_t kind, uint8_t prefix_bytes, uint8_t directed, uint8_t attrs,
                              i_DartSysMsgFn on_msg, void *on_msg_user);
/* Re types a live pattern channel in place, the engine of dart_topic_refresh. Lock not held. */
int  i_dart_topic_retype(DartTopic *topic, const DartSchema *schema, uint8_t reliability);
/* The reflect_from_mesh pick for one channel (which: 0 primary, 1 rsp, 2 prg). */
int  i_dart_node_reflect_pick(DartNode *n, DartEntityKind kind, const char *name, int which, int writer,
                              const DartSchema **schema, uint8_t *reliable, uint64_t *generation);
/* Publishes hdr plus payload on a pattern topic to every matched subscriber. */
int  i_dart_topic_send_hdr(DartTopic *topic, DartBytes hdr, DartBytes data);
/* Publishes hdr plus payload to one peer, for function replies. */
int  i_dart_topic_send_to(DartTopic *topic, uint32_t to_peer, DartBytes hdr, DartBytes data);
/* Clears a pattern topic's routing before its handle is freed. Under the node lock. */
void i_dart_topic_clear_sys(DartTopic *topic);
/* Hands every committed but unsent datagram to the wire now, transmit only. The pattern
 * layer's retire and close flush their CANCELLED replies here. Takes the node lock. */
void i_dart_node_flush_tx(DartNode *n);
/* The topic's next seqno. The slot line continues across retire and reuse, so a pattern
 * layer seeds its own monotonic counters from it. */
uint64_t i_dart_topic_seqno(DartTopic *topic);
/* The send path match wait without the send or the event: the caller derives its own
 * verdict. Returns the matched count. No wait from a callback or once converged. */
int  i_dart_topic_match_wait(DartTopic *topic);
/* Registers the patterns layer's event observer, per poll tick and close hook, NULL clears.
 * The close hook fires once at the top of close, with the node still fully alive. */
void i_dart_node_set_sys_hooks(DartNode *n, i_DartSysEventFn on_event, i_DartSysTickFn tick,
                               i_DartSysCloseFn on_close, void *user);
/* Node pool allocation (size 0 frees), the layer's per node handle slot, the monotonic
 * clock and the wall clock the transport stamps written_us from. Under the node lock. */
void    *i_dart_node_sys_alloc(DartNode *n, void *ptr, size_t size);
void   **i_dart_node_sys_slot (DartNode *n);
uint64_t i_dart_node_now_us   (DartNode *n);
uint64_t i_dart_node_wall_us  (DartNode *n);
/* The node lock for a pattern call that mutates state: 1 = acquired here, 0 = already held
 * by this thread. sys_unlock never kicks. sys_poll drives one loop tick. */
int      i_dart_node_sys_lock  (DartNode *n);
void     i_dart_node_sys_unlock(DartNode *n, int acquired);
int      i_dart_node_sys_poll  (DartNode *n, int timeout_ms);
/* Fires a topic scoped DART_ERROR through the node's normal event path. Under the node lock. */
void     i_dart_node_sys_error (DartNode *n, DartErrorKind error, DartTopic *topic, uint32_t peer);
/* Matched subscribers excluding dormant peers, the provider liveness query. */
int      i_dart_topic_live_match_count(DartTopic *topic);
/* Is peer a matched subscriber of this PUB topic. The directed call severed lane backstop. */
int      i_dart_topic_peer_matched(DartTopic *topic, uint32_t peer);
/* Matched publishers feeding this topic's subscription side. */
int      i_dart_topic_source_match_count(DartTopic *topic);
/* The oldest live matched subscriber's peer id, 0 = none. The task layer's auto direct target. */
uint32_t i_dart_topic_oldest_match(DartTopic *topic);
/* This node's discovery uuid, stable for its lifetime. The task layer's progress demux filter. */
const uint8_t *i_dart_node_uuid(DartNode *n);
/* Reflection getters for the patterns layer's entity enumeration. */
uint8_t    i_dart_topic_kind (const DartTopic *topic);   /* DartTopicKind */
uint8_t    i_dart_topic_role (const DartTopic *topic);   /* the current DartRole */
DartString i_dart_topic_name (const DartTopic *topic);   /* the stable name copy */
uint8_t    i_dart_topic_reliability(const DartTopic *topic);   /* the create qos reliability */
const uint8_t *i_dart_node_peer_uuid(DartNode *n, uint32_t peer);   /* NULL if unknown, a view */
uint16_t   i_dart_node_topic_count(DartNode *n);         /* one past the highest defined index */
/* Builds the DART_META_* snapshot (0 = all sections) as a map body into a node owned grown
 * buffer. A view valid until the next call, {NULL,0} on OOM. Under the node lock. */
DartBytes i_dart_node_snapshot(DartNode *n, uint32_t sections);

#ifdef __cplusplus
}
#endif
#endif /* DART_NODE_H */
