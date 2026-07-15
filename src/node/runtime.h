/* NODE runtime (the public dart_node_* / dart_topic_* API): owns the data sockets,
 * drives discovery and the clock, and wires peers into the transport via the sans-IO
 * node core (node/core.h). Topics are created at runtime and handed back as opaque
 * handles; a second transport would be a new runtime over that same core. */
#ifndef DART_NODE_H
#define DART_NODE_H

#include "../transport/core.h"
#include "../discovery/core.h"    /* DartDiscoveryAddr (seed peers) */
#include "../serialize/schema.h"  /* DartSchema (a topic's optional data schema) */
#include "../common/alloc.h"      /* DartAllocator (the node's memory) */
#include "core.h"                 /* DartEvent / DartEventFn / dart_event_str (node's app event) */

#ifdef __cplusplus
extern "C" {
#endif

/* Network addressing and sockets; every field is zero-means-default (defaults shown). */
typedef struct {
    uint16_t              data_port;         /* unicast data port; 0 = OS-assigned */
    const char           *discovery_group;   /* "239.255.0.7" */
    uint16_t              discovery_port;    /* 7400 */
    const char           *multicast_interface;/* interface IP for discovery multicast; NULL = auto,
                                                "127.0.0.1" = single-host. Pin on multihomed hosts */
    uint8_t               multicast_ttl;     /* hops discovery announces may travel; 1 */
    const DartDiscoveryAddr *seed_peers;   /* peers to also unicast announces to (port 0 =
                                                discovery_port), so discovery works without multicast */
    uint16_t              n_seed_peers;
    uint32_t              recv_buffer_bytes; /* data-socket SO_RCVBUF; 0 = OS default */
    uint32_t              send_buffer_bytes; /* data-socket SO_SNDBUF; 0 = OS default */
    uint16_t              fragment_size;     /* UDP payload bytes per fragment this node sends;
                                                0 = DART_FRAG_PAYLOAD. Advertised via discovery so
                                                peers reassemble at our size. Clamp [MIN, MAX]; raise
                                                MAX (compile) for jumbo frames. One size per node. */
} DartNodeNet;

/* Discovery cadence and peer-table size; zero-means-default (defaults shown). */
typedef struct {
    uint32_t              announce_interval_us; /* "I'm here" broadcast period; 1s */
    uint32_t              peer_timeout_us;   /* drop a peer after this silence; 3.5s */
    uint16_t              max_peers;         /* peer-table capacity; 16 */
} DartNodeDiscovery;

/* Optional node config, passed to dart_node_open as a compound literal (every field is
 * zero-means-default, so &(DartNodeOpts){0} or NULL is "all defaults"):
 *   dart_node_open(mem, "robot1", on_message, on_event, &(DartNodeOpts){ .domain = 7 });
 */
typedef struct {
    uint16_t              domain;        /* logical-network selector */
    uint16_t              max_topics;  /* how many topics can be created; 0 = 8 */
    void                 *user_data;     /* surfaced as DartMsg.user and DartEvent.user */
    uint8_t               disable_shm;   /* 1 = never use the same-host shared-memory fast path
                                            (force on-wire UDP even to a same-host peer; dynamic
                                            mode only, the static path never uses SHM) */
    uint8_t               fetch_details; /* 1 = greedily fetch full topic details (name + schema)
                                            for EVERY topic every peer advertises, not just topics
                                            this node shares, and cache them for the
                                            dart_node_peer_topic_* queries. For observer/debugger
                                            UIs (the explorer, the bridge); costs memory in
                                            proportion to the peers' topic counts. */
    DartNodeNet         net;           /* addressing/sockets (optional) */
    DartNodeDiscovery   discovery;     /* discovery cadence (optional) */
} DartNodeOpts;

/* Optional per-topic config, passed to dart_node_create_topic as a compound literal:
 *   dart_node_create_topic(n, "msg", DART_PUBSUB, pose_schema,
 *                            &(DartTopicOpts){ .qos = { .reliability = DART_RELIABLE } });
 */
typedef struct {
    DartQos  qos;
} DartTopicOpts;

typedef struct DartNode    DartNode;
typedef struct DartTopic DartTopic;   /* opaque topic handle (stable for the node's life) */

/* A delivered message: all of its properties in one place. topic_name is a local
 * lookup (never on the wire). From the callback, dart_topic_send and read-only
 * queries on the SAME node are allowed; poll/create_topic/set_role/drain/start/
 * stop/close are not (refused with DART_ERR_STATE / NULL / 0; under DART_NO_THREADS
 * the guards are gone, so simply do not call back in). See "threading" below. */
typedef struct {
    DartNode      *node;
    void          *user;             /* DartNodeOpts.user_data */
    uint16_t       topic_index;       /* local topic index */
    uint32_t       publisher_id;        /* peer id the message came from */
    DartString     publisher_name;      /* publisher's node name (not NUL-terminated; use .data/.len).
                                        .data is never NULL for a delivered message ("unknown-peer"
                                        if somehow unavailable), so no null check is needed. A view
                                        into discovery state, never on the per-message wire; valid
                                        for the callback's duration. */
    DartString     topic_name;     /* topic name (not NUL-terminated; use .data/.len), or {NULL,0} */
    DartBytes      data;             /* the message payload (data.data, data.len) */
    const DartSchema *schema;        /* the schema data decodes with: this topic's fields bound
                                        to the publisher's layout (a typed topic), or the publisher's
                                        own schema (a NULL-schema topic; may still be NULL if
                                        the publisher advertised none). Non-NULL means data.len was
                                        validated against it before delivery. */
    uint64_t       recv_us;          /* the node's monotonic clock when the POLL received this
                                        message (for a queued topic: when it was enqueued, not
                                        when it was taken), so rates and inter-arrival jitter
                                        measured by a frame-paced consumer reflect true arrival
                                        times, never the consumer's own cadence. */
} DartMsg;
typedef void (*DartMsgFn)(const DartMsg *msg);

/* Open a node backed by `alloc` (required): a static or dynamic DartAllocator (common/alloc.h)
 * the node allocates all its memory from and RESETS on close, so construct one per node and do
 * not reuse or touch it after close. name is this node's human-readable label, synced via
 * discovery and surfaced as DartMsg.publisher_name; NULL/empty => an auto-generated "node-XXXXXXXX".
 * on_message (delivered messages) and on_event (peer/loss/QoS events) may each be NULL. opts may
 * be NULL for all defaults. Returns NULL on failure (incl. a static buffer too small). */
DartNode    *dart_node_open(DartAllocator *alloc, const char *name, DartMsgFn on_message,
                            DartEventFn on_event, const DartNodeOpts *opts);
/* The most recent error as a DartEvent (kind DART_ERROR; read .error / dart_event_str).
 * n == NULL returns the process-global slot: the reason a dart_node_open just returned
 * NULL (there is no handle then), so `if (!dart_node_open(...)) e = dart_last_error(NULL);`.
 * A non-NULL n returns that node's most recent runtime error. .kind is DART_PEER_UP (0)
 * with .error == DART_E_NONE if none has occurred. The global slot is best-effort under
 * threads (open is rare); a node's slot is only written from its own poll/callbacks. */
DartEvent    dart_last_error(DartNode *n);
/* One loop tick: discovery, RX + delivery, timers, TX flush; blocks up to timeout_ms
 * waiting for traffic (capped to the next internal timer; negative is treated as 0,
 * there is no infinite wait). Returns 0, or DART_ERR_STATE if a service thread is
 * running (dart_node_start owns the loop then) or when called from inside a callback. */
int          dart_node_poll(DartNode *n, int timeout_ms);
/* Stops the service thread if one runs, then tears the node down. Returns DART_OK, or
 * DART_ERR_STATE when called from inside a callback (refused: the node keeps running
 * and the handle stays valid, so close it from another thread). No other thread may be
 * inside (or later enter) any dart_* call on this node once close begins. */
int          dart_node_close(DartNode *n, int send_bye);

/* ---- threading --------------------------------------------------------------------
 * Every dart_node_* / dart_topic_* call is thread-safe: a node-level lock serializes
 * them, and it is never held across a blocking wait (a poller drops it around the
 * socket wait; mutating calls wake the poller through a loopback waker, so a send hits
 * the wire in microseconds, not at the next tick). Two ways to drive a node:
 *   - call dart_node_poll yourself (from one thread or several; sends from other
 *     threads interleave safely and wake a blocked poll), or
 *   - dart_node_start: a background SERVICE THREAD owns the loop; dart_node_poll then
 *     returns DART_ERR_STATE. Callbacks fire on the service thread (events triggered
 *     directly by one of your calls fire on that calling thread), and never two at
 *     once for one node.
 * From inside on_message/on_event: dart_topic_send and the read-only queries are
 * allowed ON THE SAME NODE; dart_node_poll / create_topic / set_role / drain /
 * start / stop / close are refused with DART_ERR_STATE (or NULL/0). Do NOT call into
 * a DIFFERENT in-process node from a callback: its lock is a plain acquisition, so
 * two nodes whose callbacks each send to the other deadlock (relay between in-process
 * nodes by queueing to your own thread instead). Flow control without a poll cadence:
 * a send that would overwrite reliable history not yet acked waits (condvar, bounded
 * by qos.backpressure_wait_us), and one that would overwrite history NEVER YET SENT
 * to a matched subscriber waits for one TX pass (bounded by DART_UNSENT_WAIT_US); if it
 * still must evict, it proceeds KEEP_LAST-style and fires DART_E_EVICTED_UNSENT, never
 * silently. All of this exists only under DART_THREADS, auto-detected where the
 * platform layer provides threads (Windows; POSIX with pthreads) and off elsewhere:
 * single-threaded contract, start returns DART_ERR_NOSYS. DART_NO_THREADS forces it
 * off; a new platform layer defines DART_THREADS to declare support (platform/core.h). */
/* Run the background service thread. DART_OK; DART_ERR_STATE if already running or
 * called from a callback; DART_ERR_NOSYS when threads are compiled out (DART_THREADS
 * off) or if the thread could not be created. */
int          dart_node_start(DartNode *n);
/* Stop and join the service thread (idempotent and safe to race: concurrent stoppers
 * elect one joiner; implied by dart_node_close). Senders blocked in a flow-control
 * wait wake and proceed (KEEP_LAST + the unsent guard, no pumping). */
int          dart_node_stop(DartNode *n);
/* 1 while the service thread runs. */
int          dart_node_is_started(DartNode *n);
/* Hold / release the node lock from a NON-callback thread around reads of a zero-copy
 * view (dart_node_peers) while a poller runs elsewhere. Hold it briefly: the node makes
 * no progress meanwhile, and while this thread holds it the node treats its calls like
 * callback calls (sends commit without waiting; poll/create_topic/set_role/drain/
 * start/stop/close refuse). Not recursion-counted: one lock pairs with one unlock.
 * No-ops from inside a callback (the lock is already held) and when threads are
 * compiled out. */
void         dart_node_lock(DartNode *n);
void         dart_node_unlock(DartNode *n);
/* Sends that evicted never-sent history after the bounded wait (the DART_E_EVICTED_UNSENT
 * count) since open: the send-burst/overload indicator. The guard runs when a service
 * thread drives the node (and for the reliable backpressure path when it does not);
 * classic poll-it-yourself best-effort keeps plain KEEP_LAST overwrite semantics. */
uint32_t     dart_node_evicted_unsent(DartNode *n);

/* Create a topic. name is the cross-peer identity (same on every node, copied
 * in). role is DART_PUBSUB / DART_PUB_ONLY / DART_SUB_ONLY / DART_INACTIVE. schema is this
 * topic's data schema (built via dart_schema_*), required and held by reference so it
 * must outlive the topic; pass NULL for a raw-bytes topic (no serialization). opts may
 * be NULL for defaults. Returns a handle, or NULL if the reserve (opts.max_topics) is
 * full, the name is bad/too long, or out of memory. */
DartTopic *dart_node_create_topic(DartNode *n, const char *name, DartRole role,
                                      const DartSchema *schema, const DartTopicOpts *opts);

/* ---- consumer queues (take / dispatch) ----------------------------------------------
 * By default a topic's messages fire on_message on whichever thread polls, and a heavy
 * handler lags the whole node. A topic becomes QUEUED on its first dart_topic_take /
 * dart_topic_dispatch (or from creation when qos.queue_bytes is set): from then on the
 * poll thread only memcpys its messages into a per-topic ring, and any thread YOU
 * choose consumes them. One consumer thread per topic (the ring is single-consumer);
 * different topics can go to different threads. The ring starts small and grows on
 * demand to qos.queue_bytes (0 = DART_QUEUE_CAP, 1 MB), like the message buffers do; a
 * single message larger than the cap still fits (the cap yields to it). At the cap:
 *   best-effort: the oldest queued message is overwritten (KEEP_LAST) and DART_MSG_LOST
 *                fires -- never silent, never a stalled poll.
 *   reliable:    delivery PARKS in the transport instead: no ack reaches the publisher, its
 *                history fills, and the publisher's send blocks on the existing flow
 *                control (bounded by qos.backpressure_wait_us) -- backpressure end to
 *                end, no new loss mode. Draining the queue resumes delivery.
 * Waiting: when a service thread (or another poller) drives the node, take/dispatch
 * sleep on its progress; otherwise they drive the poll loop THEMSELVES, so they work
 * with dart_node_start, with a manual poll thread, single-threaded, and under
 * DART_NO_THREADS (where a wait that cannot be serviced by anyone else simply pumps).
 * From inside a callback they cannot wait or run the loop: timeout behaves as 0. */
#ifndef DART_QUEUE_CAP
#define DART_QUEUE_CAP (1u << 20)   /* default consumer-queue growth cap, bytes per topic */
#endif
/* Pop the next queued message into *out. The views in *out (data + names) point into the
 * topic's ring and stay valid until the NEXT take/dispatch on this topic (hold them
 * briefly: an outstanding view pins the oldest ring slot). timeout_ms: 0 = just check,
 * >0 = wait up to that long for a message, negative = wait indefinitely. Returns 1 (got
 * one), 0 (empty at timeout), or a negative DartResult (DART_ERR_STATE = called while a
 * dispatch on this topic runs its callback; DART_ERR_OOM = the ring could not be
 * allocated). First use switches the topic to queued delivery. */
int dart_topic_take(DartTopic *topic, DartMsg *out, int timeout_ms);
/* Drain the queue by running the node's on_message on the CALLING thread, oldest first:
 * up to max_msgs of the messages queued at entry (0 = all of them), first waiting up to
 * timeout_ms like take. Returns messages dispatched or a negative DartResult. The
 * callbacks run WITHOUT the node lock, so unlike poll-thread callbacks they may use the
 * whole API (create_topic, set_role, ...) -- except take/dispatch on THIS topic
 * (refused with DART_ERR_STATE). */
int dart_topic_dispatch(DartTopic *topic, int max_msgs, int timeout_ms);
/* Dispatch across every already-queued topic, oldest first per topic: the one-liner
 * for a frame-paced consumer that owns all queues (the Unity Update() / a UI frame).
 * Waits up to timeout_ms for ANY queued topic to hold data; does NOT switch topics
 * to queued delivery. max_msgs bounds the total (0 = all queued at entry). */
int dart_node_dispatch(DartNode *n, int max_msgs, int timeout_ms);
/* Queue observability: messages waiting (an un-released take view counts), ring bytes
 * used / current capacity, and messages dropped (best-effort overwrite or refusal) since
 * open. Any out-pointer may be NULL; all zeros for a topic that is not queued. */
void dart_topic_queue_stats(DartTopic *topic, uint32_t *msgs, uint32_t *bytes,
                              uint32_t *capacity, uint32_t *dropped);
/* Publish to all matched subscribers. Returns DART_OK or a negative DartResult. */
int          dart_topic_send(DartTopic *topic, DartBytes data);
/* Flip a topic's role at runtime (re-advertises interest). Returns 0 ok, <0 on error. */
int          dart_topic_set_role(DartTopic *topic, DartRole role);
/* This topic's local index (== DartMsg.topic_index for its messages). */
uint16_t     dart_topic_index(const DartTopic *topic);
/* This topic's schema, as passed to create (NULL for a raw-bytes topic). */
const DartSchema *dart_topic_schema(const DartTopic *topic);
/* Recover an already-created topic handle by its creation index (0-based), or NULL if
 * out of range. Lets a caller use a handle without storing the create_topic result. */
DartTopic *dart_node_topic(DartNode *n, uint16_t index);

/* ---- read-only peer inspection (diagnostics / a discovery explorer) ----------------
 * The live peer table as a zero-copy array, valid until the next dart_node_poll. A node
 * peer IS a discovery peer: identity, locator, liveness, name, uuid, and the OPAQUE
 * announce overlay are exactly what discovery already holds, so this hands back discovery's
 * own view rather than copying into a parallel struct. The overlay's transport meaning (the
 * peer's UDP fragment size and pub/sub interest) is decoded on demand via dart_node_peer_frag
 * / dart_node_peer_interest_next (node/core.h), so a caller never touches dart_meta_*.
 * Returns the packed array + *count (used peers, ACTIVE or DROPPED); NULL if n is NULL. */
const DartDiscoveryPeer *dart_node_peers(DartNode *n, uint16_t *count);

/* A peer topic's full details from the greedy cache (opts.fetch_details; without it only
 * topics this node shares are ever fetched, so most queries yield nothing). Key by the
 * peer id and the index from the interest walk. topic_name returns {NULL,0} until the
 * peer's detail response arrives (the announce carries only hashes; show the hash until
 * then). topic_schema returns the parsed, node-owned schema the peer advertises (NULL =
 * untyped or not yet fetched; do NOT free) and fills the optional schema_hash out-param
 * (0 = untyped). Views into node state: with a poller on another thread bracket call + use
 * with dart_node_lock/dart_node_unlock, like dart_node_peers. */
DartString        dart_node_peer_topic_name(DartNode *n, uint32_t peer, uint16_t index);
const DartSchema *dart_node_peer_topic_schema(DartNode *n, uint32_t peer, uint16_t index,
                                              uint64_t *schema_hash);

/* Cumulative backpressure since open: us waited on slow subscribers and how many sends
 * waited. Either out-pointer may be NULL. */
void     dart_node_backpressure_stats(DartNode *n, uint64_t *waited_us, uint32_t *waited_sends);

/* Message-buffer memory (dynamic mode): in_use = live bytes, peak = high-water, alloc_calls
 * = how many heap (re)allocations have happened. alloc_calls stops rising once buffers reach
 * their steady-state sizes, so a flat count over a window proves the hot path is alloc-free.
 * Any out-pointer may be NULL. (Static mode: in_use/peak are 0; alloc_calls counts bumps.) */
void     dart_node_mem_stats(DartNode *n, size_t *in_use, size_t *peak, uint64_t *alloc_calls);
/* Cumulative reliable-repair counters for a topic (see DartRepairStats). The
 * per-second deltas are repair throughput; *out is zeroed for a NULL topic. */
void     dart_topic_repair_stats(DartTopic *topic, DartRepairStats *out);

/* In-pump diagnostic probe. A reliable publisher blocks inside dart_topic_send for the
 * whole backpressure wait, so its normal once-a-second print can't see within a stall.
 * A registered probe is called on a ~interval_us timer DURING that wait with the
 * publisher's repair progress over each interval -- making the stall a within-block time
 * series (resends bursty-then-flat => subscriber stopped asking; steady => resends dropped).
 * Observational only; deltas are since the previous sample in the same wait. */
typedef struct {
    uint16_t topic;         /* topic index being pumped */
    uint64_t wait_elapsed_us; /* us since this backpressure wait began */
    uint64_t interval_us;     /* us since the previous sample (normalise deltas by this for true /s) */
    uint64_t frags_resent;    /* publisher DATA fragments resent in the interval */
    uint64_t nacks_recv;      /* repair NACKs received in the interval */
    uint32_t polls;           /* dart_node_poll calls in the interval */
    uint32_t polls_idle;      /* of those, polls with no repair pending (publisher idle for lack of NACKs) */
} DartPumpSample;
typedef void (*DartPumpProbeFn)(void *user, const DartPumpSample *s);
/* Register the in-pump probe (NULL fn disables). interval_us 0 => default 200ms. */
void     dart_node_set_pump_probe(DartNode *n, DartPumpProbeFn fn, uint64_t interval_us, void *user);
/* Head-of-line reassembly snapshot for the in-progress message from `peer` on this
 * topic: returns 1 + fills base_seqno/have/total if one is mid-reassembly, else 0.
 * `have` rising across calls = repair crawling; flat = wedged. Any pointer may be NULL. */
int      dart_topic_subscriber_progress(DartTopic *topic, uint32_t peer,
                            uint64_t *base_seqno, uint32_t *have, uint32_t *total);
/* Wait until every subscriber has acked all messages on this topic, or timeout_ms
 * elapses: with a service thread it sleeps on its progress, otherwise it pumps the
 * loop. Returns 1 if drained, 0 on timeout (or when called from a callback). Call
 * before close so a burst isn't cut by the BYE. */
int      dart_topic_drain(DartTopic *topic, int timeout_ms);
/* Subscribers matched on this topic now; a one-shot publisher polls it before sending. */
int      dart_topic_match_count(DartTopic *topic);
#ifdef DART_SHM
/* Messages published / delivered via the zero-fragment shared-memory path since open
 * (observability; same-host subscribers only). Either out-pointer may be NULL. */
void     dart_node_shm_stats(DartNode *n, uint32_t *sent, uint32_t *recv);
#endif

#ifdef __cplusplus
}
#endif
#endif /* DART_NODE_H */
