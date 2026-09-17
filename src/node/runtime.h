/* The node runtime and the public rant_node_* and rant_topic_* API. It owns the sockets
 * and the clock and drives the cores. docs/node.md explains how to use it. */
#ifndef RANT_NODE_H
#define RANT_NODE_H

#include "../common/api.h"
#include "../transport/core.h"
#include "../discovery/core.h"
#include "../serialize/schema.h"
#ifndef RANT_NO_STDTYPES
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
    const RantDiscoveryAddr *seed_peers;     /* unicast announces here too, port 0 = discovery_port */
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
    uint32_t              announce_interval_us; /* 3 s */
    uint32_t              peer_timeout_us;   /* drop a peer after this silence, 12 s */
    uint16_t              max_peers;         /* 16 */
} RantNodeDiscovery;

#ifndef RANT_MATCH_WAIT_MS
#define RANT_MATCH_WAIT_MS 1000     /* the default send path match wait bound */
#endif

/* The node options, passed as a compound literal. NULL means all defaults. */
typedef struct {
    uint16_t              domain;
    uint16_t              max_topics;  /* topics that can be created, 0 = 8 */
    void                 *user_data;     /* RantMsg.user and RantEvent.user */
    uint8_t               disable_shm;   /* 1 = never use the same host shared memory path */
    uint8_t               fetch_details; /* 1 = fetch and cache every topic every peer advertises,
                                            for observer UIs. Costs memory per peer topic */
    int32_t               match_wait_ms; /* the send path match wait bound. 0 = RANT_MATCH_WAIT_MS,
                                            negative = disabled and such a send fires UNMATCHED_SEND */
    uint8_t               disable_logs;  /* 1 = no @rant/log topics, rant_node_log gives NOSYS */
    uint8_t               disable_meta;  /* 1 = no built in @rant/meta function */
    uint8_t               disable_error_logs; /* 1 = do not mirror errors onto @rant/log/error */
    RantNodeNet           net;
    RantNodeDiscovery     discovery;
    uint32_t              event_queue_bytes; /* the event ring cap once rant_node_set_event_queue is
                                                called, 0 = 64 KB. Overflow drops the oldest event */
} RantNodeOpts;

typedef struct RantQueue RantQueue;       /* a callback queue, owned by the node, freed at close */

/* Per topic options, passed as a compound literal. */
typedef struct {
    RantQos    qos;
    uint8_t    reflect_from_mesh;  /* fill what was left unspecified from the mesh: a NULL schema and
                                    a zero reliability follow it. See docs/reflection.md */
    RantQueue *queue;              /* callbacks park here and run on rant_queue_dispatch. NULL =
                                    inline on the loop thread. Fixed for the topic's life */
} RantTopicOpts;

typedef struct RantNode      RantNode;
typedef struct RantTopic RantTopic;       /* an opaque handle, stable for the node's life */

/* A delivered message. From the callback, send and read only queries on the same node
 * are allowed. poll, create, set_role, drain, start, stop and close are refused. */
typedef struct {
    RantNode        *node;
    void            *user;             /* RantNodeOpts.user_data */
    uint16_t         topic_index;
    uint32_t         publisher_id;
    RantString       publisher_name;      /* never NULL data on delivery, "unknown-peer" at worst. A
                                        view valid for the callback */
    RantString       topic_name;     /* {NULL,0} if unknown */
    RantBytes        header;           /* pattern header bytes in front of the payload, {NULL,0} on a
                                        plain topic */
    RantBytes        data;             /* the payload after the header */
    const RantSchema *schema;          /* the schema data decodes with, validated before delivery.
                                        NULL on a raw topic or an untyped publisher */
    uint64_t       recv_us;          /* the monotonic clock at receive, or at enqueue */
    uint64_t       written_us;       /* the writer's wall clock at commit, UTC microseconds. 0 = the
                                        publisher opted out. Never mix it with recv_us */
    uint64_t       capture_us;       /* when the publisher says the data was true, UTC
                                        microseconds. 0 = none given, written_us is all there is */
} RantMsg;
typedef void (*RantMsgFn)(const RantMsg *msg);

/* The process heap as an allocator, for a caller with no page source of its own.
 * page_size 0 is the default. spec/allocation.md explains the modes. */
RANT_API RantAllocator rant_allocator_heap(uint32_t page_size);
/* The process heap as a RantAllocFn, for rant_schema_compile and rant_schema_free. */
RANT_API void           *rant_heap_realloc(void *user, void *ptr, size_t size);

/* Opens a node on alloc, which it copies and resets on close, so use one per node. alloc
 * is required and NULL refuses the open with RANT_E_OOM. name NULL gives an auto name,
 * on_message and on_event may be NULL, opts NULL means defaults. */
RANT_API RantNode        *rant_node_open(RantAllocator *alloc, const char *name, RantMsgFn on_message,
                                     RantEventFn on_event, const RantNodeOpts *opts);
/* The most recent error. n NULL returns the process global slot, the reason an open
 * returned NULL. .error is RANT_E_NONE if none occurred. */
RANT_API RantEvent        rant_last_error(RantNode *n);
/* One loop tick, blocking up to timeout_ms. RANT_ERR_STATE while a service thread runs
 * or from inside a callback. */
RANT_API int            rant_node_poll(RantNode *n, int timeout_ms);
/* Stops the service thread, then tears the node down. RANT_ERR_STATE from a callback or
 * while a queue is being dispatched: stop the draining threads first. */
RANT_API int            rant_node_close(RantNode *n, int send_bye);

/* Threading: every call is thread safe under one node lock never held across a wait.
 * Drive the node with rant_node_poll or with rant_node_start. See docs/node.md. */
/* Runs the background service thread. RANT_ERR_NOSYS when threads are compiled out. */
RANT_API int            rant_node_start(RantNode *n);
/* Stops and joins the service thread. Idempotent, implied by close. */
RANT_API int            rant_node_stop(RantNode *n);
/* 1 while the service thread runs. */
RANT_API int            rant_node_is_started(RantNode *n);
/* Hold the node lock from a non callback thread around reads of a view. Hold it briefly.
 * No ops from a callback and when threads are compiled out. */
RANT_API void           rant_node_lock(RantNode *n);
RANT_API void           rant_node_unlock(RantNode *n);
/* Sends that evicted never sent history after the bounded wait, since open. */
RANT_API uint32_t       rant_node_evicted_unsent(RantNode *n);

#ifndef RANT_NO_STDTYPES
/* The two standard type values that need a platform: the wall clock a Timestamp counts
 * from, and a random version 4 Uuid. Neither needs a node. */
RANT_API RantTimestamp rant_timestamp_now(void);
RANT_API void              rant_uuid_new(RantUuid *out);
#endif

/* Creates a topic. name is the identity, schema is copied in, NULL means raw bytes. A
 * retired slot is reused. NULL when the reserve is full, the name is bad or out of memory. */
RANT_API RantTopic *rant_node_create_topic(RantNode *n, const char *name, RantRole role,
                                               const RantSchema *schema, const RantTopicOpts *opts);

/* Retires a topic and frees the handle, invalid after RANT_OK. RANT_ERR_STATE from a
 * callback, for a pattern channel or a builtin, or during a dispatch. See docs/topics.md. */
RANT_API int rant_topic_retire(RantTopic *topic);
/* A reflect_from_mesh topic: re reads the mesh and re types the handle in place when the
 * provider moved. 1 re created, 0 current, negative on error. */
RANT_API int rant_topic_refresh(RantTopic *topic);

/* Callback queues: a handle created with opts.queue parks each callback as a record on its
 * own ring, and rant_queue_dispatch runs the parked callbacks on the calling thread with
 * the node lock released. docs/node.md explains the rules. */
#ifndef RANT_QUEUE_CAP
#define RANT_QUEUE_CAP (1u << 20)     /* the default ring growth cap, bytes per topic */
#endif
#ifndef RANT_QUEUES_MAX
#define RANT_QUEUES_MAX 8             /* callback queues per node */
#endif
/* NULL with RANT_E_STATE from a callback or past RANT_QUEUES_MAX, RANT_E_OOM otherwise. */
RANT_API RantQueue *rant_node_create_queue(RantNode *n);
/* Runs the callbacks parked at entry, oldest first across the queue's handles, up to
 * max_callbacks (0 = all). timeout_ms waits for the first record like take: 0 checks,
 * positive waits, negative forever, driving the loop itself when no service thread runs.
 * Returns the count run. RANT_ERR_STATE from an inline callback, from one of its own
 * callbacks, or while another thread dispatches the same queue. */
RANT_API int        rant_queue_dispatch(RantQueue *q, int max_callbacks, int timeout_ms);
/* Records waiting across the queue's handles, and records dropped since open. */
RANT_API void       rant_queue_stats(RantQueue *q, uint32_t *waiting, uint32_t *dropped);
/* Parks on_event on q from now on, NULL = inline again and the parked events are dropped.
 * rant_last_error stays current either way. RANT_ERR_STATE from a callback, for another
 * node's queue or while the events are being dispatched, RANT_ERR_OOM for the ring. */
RANT_API int        rant_node_set_event_queue(RantNode *n, RantQueue *q);

/* Consumer queues: a topic becomes queued on its first take or dispatch, or from creation
 * with qos.queue_bytes. docs/node.md explains the rules. */
/* Pops the next queued message. The views stay valid until the next take or dispatch.
 * timeout_ms 0 checks, positive waits, negative waits forever. 1 got one, 0 empty. */
RANT_API int rant_topic_take(RantTopic *topic, RantMsg *out, int timeout_ms);
/* Runs on_message on the calling thread for up to max_msgs queued messages (0 = all),
 * after waiting like take. Returns the count. The callbacks run without the node lock. */
RANT_API int rant_topic_dispatch(RantTopic *topic, int max_msgs, int timeout_ms);
/* dispatch across every already queued topic, oldest first per topic. */
RANT_API int rant_node_dispatch(RantNode *n, int max_msgs, int timeout_ms);
/* Messages waiting, ring bytes used and capacity, and messages dropped since open. */
RANT_API void rant_topic_queue_stats(RantTopic *topic, uint32_t *msgs, uint32_t *bytes,
                                       uint32_t *capacity, uint32_t *dropped);
/* Publishes to every matched subscriber. RANT_OK or a negative RantResult. */
/* Optional per send config, NULL means all defaults. capture_us is when the data was
 * true rather than when it was sent, and costs 8 wire bytes only when it is set. */
typedef struct {
    uint64_t capture_us;   /* UTC microseconds, the rant_timestamp_now clock. 0 = none */
} RantSendOpts;

RANT_API int            rant_topic_send(RantTopic *topic, RantBytes data, const RantSendOpts *opts);
/* Flips the role at runtime and re advertises. 0 ok, negative on error. */
RANT_API int            rant_topic_set_role(RantTopic *topic, RantRole role);
/* The local index, RantMsg.topic_index for its messages. */
RANT_API uint16_t       rant_topic_index(const RantTopic *topic);
/* The schema as passed to create, NULL for a raw topic. */
RANT_API const RantSchema *rant_topic_schema(const RantTopic *topic);
/* The handle by creation index, NULL if out of range. */
RANT_API RantTopic *rant_node_topic(RantNode *n, uint16_t index);

/* Reflection: three walks over one zero initialized RantIter, called until 0. Every view
 * is valid until the next poll. docs/reflection.md explains them. */

/* One discovered peer per call, dropped peers included. Gate on liveness. */
RANT_API int rant_node_peers_next(RantNode *n, RantIter *it, RantPeerInfo *out);

/* The entities one node advertises. RANT_SELF walks this node, a dropped peer serves its
 * last known view. */
RANT_API int rant_node_entities_next(RantNode *n, uint32_t peer, RantIter *it, RantEntityInfo *out);

/* The whole mesh folded, one entity per kind and name across every active peer and this node. */
RANT_API int rant_node_mesh_next(RantNode *n, RantIter *it, RantEntityInfo *out);
RANT_API int rant_node_mesh_find(RantNode *n, RantEntityKind kind, const char *name, RantEntityInfo *out);

/* Bumps on every reflected change anywhere. Re walk iff it moved. */
RANT_API uint32_t rant_node_mesh_epoch(RantNode *n);

/* Microseconds waited on slow subscribers and how many sends waited, since open. */
RANT_API void       rant_node_backpressure_stats(RantNode *n, uint64_t *waited_us, uint32_t *waited_sends);

/* Live bytes, the high water mark and the count of heap allocations. A flat count over a
 * window proves the hot path is allocation free. */
RANT_API void       rant_node_mem_stats(RantNode *n, size_t *in_use, size_t *peak, uint64_t *alloc_calls);
/* The topic's cumulative repair counters. Zeroed for a NULL topic. */
RANT_API void       rant_topic_repair_stats(RantTopic *topic, RantRepairStats *out);

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
} RantPumpSample;
typedef void (*RantPumpProbeFn)(void *user, const RantPumpSample *s);
/* Registers the probe, NULL disables. interval_us 0 = 200 ms. */
RANT_API void       rant_node_set_pump_probe(RantNode *n, RantPumpProbeFn fn, uint64_t interval_us, void *user);
/* The in progress message from peer: 1 and its base seqno, fragments held and needed. */
RANT_API int        rant_topic_subscriber_progress(RantTopic *topic, uint32_t peer,
                                     uint64_t *base_seqno, uint32_t *have, uint32_t *total);
/* Waits until every subscriber acked everything, or timeout_ms. 1 drained, 0 on timeout
 * or from a callback. Call before close so a burst is not cut by the BYE. */
RANT_API int        rant_topic_drain(RantTopic *topic, int timeout_ms);
/* Subscribers matched now. */
RANT_API int        rant_topic_match_count(RantTopic *topic);

/* The send path match wait: a send that would reach nobody while a match is still
 * resolving blocks up to opts.match_wait_ms. See docs/topics.md and spec/interest.md. */
/* Unresolved candidate matches right now. 0 = converged for everyone known. */
RANT_API int        rant_topic_pending_count(RantTopic *topic);
/* 1 when a send would not wait: matched, or converged with nobody to wait for. The async
 * form for a GUI that disables the wait. */
RANT_API int        rant_topic_ready(RantTopic *topic);

/* Blocks until discovery and matching settle. Call it after creating topics. timeout_ms
 * negative = 3 announce intervals. 1 settled, 0 on timeout or from a callback. */
RANT_API int        rant_node_settle(RantNode *n, int timeout_ms);
#ifdef RANT_SHM
/* Messages published and delivered through shared memory since open. */
RANT_API void       rant_node_shm_stats(RantNode *n, uint32_t *sent, uint32_t *recv);
#endif

/* Messages and bytes this node committed to the topic and received on it, since open. */
RANT_API void       rant_topic_counts(RantTopic *topic, uint64_t *tx_msgs, uint64_t *tx_bytes,
                                    uint64_t *rx_msgs, uint64_t *rx_bytes);

/* The built in @rant/log/{error,warn,info} topics. docs/node.md explains them. */
typedef enum { RANT_LOG_ERROR = 0, RANT_LOG_WARN = 1, RANT_LOG_INFO = 2 } RantLogLevel;
#ifndef RANT_LOG_MAX
#define RANT_LOG_MAX 512     /* formatted log text bytes, longer is truncated */
#endif
/* printf style publish on the level's topic. RANT_ERR_NOSYS when the logs are disabled. */
RANT_API int            rant_node_log(RantNode *n, RantLogLevel level, const char *fmt, ...);
/* Publishes an already formatted line, len negative = NUL terminated. The FFI entry. */
RANT_API int            rant_node_log_text(RantNode *n, RantLogLevel level, const char *text, int len);
/* The node's own handle for a level, NULL when disabled. */
RANT_API RantTopic       *rant_node_log_topic(RantNode *n, RantLogLevel level);

/* The @rant/meta snapshot section mask, a 4 byte LE u32 request payload, 0 = everything.
 * The response is RantMeta { info: map } with one key per section. See docs/node.md. */
#define RANT_META_NODE     0x1u
#define RANT_META_PROC     0x2u
#define RANT_META_TOPICS 0x4u
#define RANT_META_PEERS    0x8u

/* Internal hooks for the patterns layer: create a topic carrying an entity kind, route its
 * messages to a handler, and observe node wide events and a per poll tick. */
typedef void     (*i_RantSysMsgFn)(void *user, const RantMsg *msg);
typedef void     (*i_RantSysEventFn)(void *user, const RantEvent *ev);
typedef uint64_t (*i_RantSysTickFn)(void *user, uint64_t now_us);     /* next deadline, 0 = none */
typedef void     (*i_RantSysCloseFn)(void *user);     /* the node is closing: settle promises */

/* Creates a pattern topic: stamps the kind, prefix, directed flag and attrs, permits '@' in
 * the name, and routes deliveries to on_msg. opts.queue gives it a ring, see the seams below. */
RantTopic *i_rant_node_create_pattern_topic(RantNode *n, const char *name, RantRole role,
                              const RantSchema *schema, const RantTopicOpts *opts,
                              uint8_t kind, uint8_t prefix_bytes, uint8_t directed, uint8_t attrs,
                              i_RantSysMsgFn on_msg, void *on_msg_user);
/* Re types a live pattern channel in place, the engine of rant_topic_refresh. Lock not held. */
int  i_rant_topic_retype(RantTopic *topic, const RantSchema *schema, uint8_t reliability);
/* The reflect_from_mesh pick for one channel (which: 0 primary, 1 rsp, 2 prg). */
int  i_rant_node_reflect_pick(RantNode *n, RantEntityKind kind, const char *name, int which, int writer,
                              const RantSchema **schema, uint8_t *reliable, uint64_t *generation);
/* Publishes hdr plus payload on a pattern topic to every matched subscriber. */
int  i_rant_topic_send_hdr(RantTopic *topic, RantBytes hdr, RantBytes data);
/* Publishes hdr plus payload to one peer, for function replies. */
int  i_rant_topic_send_to(RantTopic *topic, uint32_t to_peer, RantBytes hdr, RantBytes data);
/* Clears a pattern topic's routing before its handle is freed. Under the node lock. */
void i_rant_topic_clear_sys(RantTopic *topic);
/* Callback queue seams. A pattern topic created with opts.queue keeps inline delivery: its
 * handler does the bookkeeping, parks what the user callback needs as a record with a kind
 * of its own, and on_dispatch gets the record back on the draining thread. A kind with the
 * high bit set is synthetic: the body is the layer's own blob, handed back whole in
 * msg->data with no prefix split and no schema. */
#define I_RANT_REC_SYNTH 0x80u
typedef void (*i_RantSysDispatchFn)(void *user, const RantMsg *msg, uint8_t kind);
void       i_rant_topic_set_dispatch(RantTopic *topic, i_RantSysDispatchFn on_dispatch);
RantQueue *i_rant_topic_queue(RantTopic *topic);      /* NULL = inline */
int        i_rant_topic_busy(RantTopic *topic);       /* 1 while a dispatched callback of it runs */
/* Parks hdr then body as one record on the topic's ring. A pattern ring never parks the
 * transport: at the cap the oldest record goes and MSG_LOST fires. recv_us 0 = now.
 * Lock held. RANT_ERR_STATE when the topic has no ring. */
int        i_rant_topic_park(RantTopic *topic, uint8_t kind, uint32_t from, RantBytes hdr,
                             RantBytes body, uint64_t written_us, uint64_t recv_us);
/* Hands every committed but unsent datagram to the wire now, transmit only. The pattern
 * layer's retire and close flush their CANCELLED replies here. Takes the node lock. */
void i_rant_node_flush_tx(RantNode *n);
/* The topic's next seqno. The slot line continues across retire and reuse, so a pattern
 * layer seeds its own monotonic counters from it. */
uint64_t i_rant_topic_seqno(RantTopic *topic);
/* The send path match wait without the send or the event: the caller derives its own
 * verdict. Returns the matched count. No wait from a callback or once converged. */
int  i_rant_topic_match_wait(RantTopic *topic);
/* Registers the patterns layer's event observer, per poll tick and close hook, NULL clears.
 * The close hook fires once at the top of close, with the node still fully alive. */
void i_rant_node_set_sys_hooks(RantNode *n, i_RantSysEventFn on_event, i_RantSysTickFn tick,
                               i_RantSysCloseFn on_close, void *user);
/* Node pool allocation (size 0 frees), the layer's per node handle slot, the monotonic
 * clock and the wall clock the transport stamps written_us from. Under the node lock. */
void    *i_rant_node_sys_alloc(RantNode *n, void *ptr, size_t size);
void   **i_rant_node_sys_slot (RantNode *n);
uint64_t i_rant_node_now_us     (RantNode *n);
uint64_t i_rant_node_wall_us    (RantNode *n);
/* The node lock for a pattern call that mutates state: 1 = acquired here, 0 = already held
 * by this thread. sys_unlock never kicks. sys_poll drives one loop tick. */
int      i_rant_node_sys_lock    (RantNode *n);
void     i_rant_node_sys_unlock(RantNode *n, int acquired);
int      i_rant_node_sys_poll    (RantNode *n, int timeout_ms);
/* Blocks until done says so or its deadline passes, under the node lock, on the service
 * thread's progress when one runs and by pumping the loop otherwise. done runs under the
 * lock and states this iteration's deadline, (uint64_t)-1 = none. 1 done, 0 timed out,
 * -1 refused from a callback. */
typedef int (*i_RantSysWaitFn)(RantNode *n, void *ctx, uint64_t now_us, uint64_t *deadline_us);
int      i_rant_node_sys_wait    (RantNode *n, i_RantSysWaitFn done, void *ctx);
/* Fires a topic scoped RANT_ERROR through the node's normal event path. Under the node lock. */
void     i_rant_node_sys_error (RantNode *n, RantErrorKind error, RantTopic *topic, uint32_t peer);
/* Matched subscribers excluding dormant peers, the provider liveness query. */
int      i_rant_topic_live_match_count(RantTopic *topic);
/* Is peer a matched subscriber of this PUB topic. The directed call severed lane backstop. */
int      i_rant_topic_peer_matched(RantTopic *topic, uint32_t peer);
/* Matched publishers feeding this topic's subscription side. */
int      i_rant_topic_source_match_count(RantTopic *topic);
/* The oldest live matched subscriber's peer id, 0 = none. The task layer's auto direct target. */
uint32_t i_rant_topic_oldest_match(RantTopic *topic);
/* This node's discovery uuid, stable for its lifetime. The task layer's progress demux filter. */
const uint8_t *i_rant_node_uuid(RantNode *n);
/* Reflection getters for the patterns layer's entity enumeration. */
uint8_t      i_rant_topic_kind (const RantTopic *topic);       /* RantTopicKind */
uint8_t      i_rant_topic_role (const RantTopic *topic);       /* the current RantRole */
RantString i_rant_topic_name (const RantTopic *topic);         /* the stable name copy */
uint8_t      i_rant_topic_reliability(const RantTopic *topic);       /* the create qos reliability */
int          i_rant_node_peer_uuid(RantNode *n, uint32_t peer, uint8_t out[16]);       /* 1 = found, 0 = unknown */
uint16_t   i_rant_node_topic_count(RantNode *n);             /* one past the highest defined index */
/* Builds the RANT_META_* snapshot (0 = all sections) as a map body into a node owned grown
 * buffer. A view valid until the next call, {NULL,0} on OOM. Under the node lock. */
RantBytes i_rant_node_snapshot(RantNode *n, uint32_t sections);

#ifdef __cplusplus
}
#endif
#endif /* RANT_NODE_H */
