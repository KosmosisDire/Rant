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
    const char           *multicast_interface;/* pin discovery multicast to THIS interface IP;
                                                NULL = every interface (the default: no interface
                                                is ever guessed at), "127.0.0.1" = single-host */
    uint8_t               multicast_ttl;     /* hops discovery announces may travel; 1 */
    const DartDiscoveryAddr *seed_peers;   /* peers to also unicast announces to (port 0 =
                                                discovery_port), so discovery works without multicast */
    uint16_t              n_seed_peers;
    uint8_t               unicast_only;      /* 1 = this node cannot multicast at all (an IGMP-less
                                                stack, a segment that filters it): join no group and
                                                announce only to seed_peers + peers already known.
                                                It also asks every node that hears it to RE-ANNOUNCE
                                                it on their paths, so seeding ONE reachable node
                                                makes it discoverable across the whole mesh (the
                                                relay only introduces: data stays unicast end to
                                                end, and established pairs survive its death).
                                                Set seed_peers, or have one node seed this one. */
    uint32_t              recv_buffer_bytes; /* data-socket SO_RCVBUF; 0 = OS default */
    uint32_t              send_buffer_bytes; /* data-socket SO_SNDBUF; 0 = OS default */
    uint16_t              fragment_size;     /* UDP payload bytes per fragment this node sends;
                                                0 = DART_FRAG_SIZE. Advertised via discovery so
                                                peers reassemble at our size. Clamp [MIN, MAX]; raise
                                                MAX (compile) for jumbo frames. One size per node. */
} DartNodeNet;

/* Discovery cadence and peer-table size; zero-means-default (defaults shown). */
typedef struct {
    uint32_t              announce_interval_us; /* "I'm here" broadcast period; 1s */
    uint32_t              peer_timeout_us;   /* drop a peer after this silence; 3.5s */
    uint16_t              max_peers;         /* peer-table capacity; 16 */
} DartNodeDiscovery;

#ifndef DART_MATCH_WAIT_MS
#define DART_MATCH_WAIT_MS 1000   /* default send-path match-wait bound (opts.match_wait_ms = 0) */
#endif

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
    int32_t               match_wait_ms; /* the send-path MATCH WAIT bound (see dart_topic_ready):
                                            a send that would reach ZERO subscribers while a match
                                            is still resolving (the announce/detail round trip
                                            after create_topic, or discovery still gathering after
                                            open) blocks up to this long for it to form, instead
                                            of silently dropping the first message. 0 = default
                                            (DART_MATCH_WAIT_MS, 1000); negative = disabled: such
                                            a send commits immediately (dropped as before, but
                                            loudly: DART_E_UNMATCHED_SEND). GUIs disable it and
                                            poll dart_topic_ready instead. */
    uint8_t               disable_logs;  /* 1 = do not create the built-in @dart/log/{error,warn,
                                            info} topics (dart_node_log then returns
                                            DART_ERR_NOSYS). Saves their history memory on
                                            embedded; see "logs" below. */
    uint8_t               disable_meta;  /* 1 = do not host the built-in @dart/meta introspection
                                            function (also implied by DART_NO_PATTERNS). */
    uint8_t               disable_error_logs; /* 1 = do not mirror this node's internal DART_ERROR
                                            events onto @dart/log/error. Mirroring is on by default
                                            and is also disabled by disable_logs. */
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
    DartBytes      header;           /* pattern-header bytes in front of the payload (the patterns
                                        layer's call-id/status/flags prefix); {NULL,0} on a plain
                                        topic. A view valid for the callback / take view. */
    DartBytes      data;             /* the message payload after the header (data.data, data.len) */
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
    uint64_t       sent_us;          /* the SENDER's wall clock (UTC microseconds) at the moment
                                        its send committed into writer history: a source timestamp,
                                        so a repaired or replayed message keeps its original value.
                                        0 = the publisher opted out (DartQos.no_timestamp).
                                        Comparability across hosts is only as good as their clock
                                        sync; never mix it with the monotonic recv_us. */
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

/* ---- the send-path MATCH WAIT (first send vs the forming match) ---------------------
 * Matching a fresh topic to an already-present peer costs one announce/detail round trip
 * after create_topic, so a send inside that window would commit to ZERO subscribers and
 * be dropped: invisible on a stream, fatal for one-shot / last-value semantics. The node
 * closes that window itself: discovery solicits at open (existing peers answer within an
 * RTT), create_topic kicks the detail cycle, and a send that would reach zero subscribers
 * WHILE a candidate match is still resolving (a peer's announce nominates this topic but
 * its verdicts are in flight, or the post-open gather has not settled) blocks -- bounded
 * by opts.match_wait_ms -- until the match forms or matching converges. A topic nobody
 * advertises interest in proceeds immediately (fire-and-forget is the contract), so only
 * a genuine race ever waits, once, for about a round trip; steady-state sends are
 * untouched (the converged state is memoized until the topology changes). On timeout the
 * send proceeds and DART_E_UNMATCHED_SEND fires, never silent. Reentrant sends (from a
 * callback) cannot wait, as ever. */
/* Unresolved candidate matches for this topic right now: peers whose announces nominate
 * it but whose name/schema verdicts are still in flight (or whose blob is still being
 * fetched). 0 = matching has converged for everyone currently known, so
 * dart_topic_match_count is the final answer until the topology changes. */
int      dart_topic_pending_count(DartTopic *topic);
/* 1 when a send on this topic would not wait: it has a matched subscriber, or matching
 * has converged (post-open gather settled + no unresolved candidates) so there is nobody
 * to wait for. The async form of the match wait: a GUI that must not block disables the
 * wait (opts.match_wait_ms < 0), parks the payload while this is 0, and flushes when it
 * flips to 1 (poll it per frame, or on DART_PEER_INTEREST events). */
int      dart_topic_ready(DartTopic *topic);

/* Block until discovery + matching SETTLE. Solicits (like dart_discovery_gather: existing
 * peers answer immediately), then waits until every active peer has answered and the
 * peer/match topology has been quiet for a beat, so a populated network settles in a few
 * hundred ms; only a (seemingly) empty one waits a full announce interval to rule out a
 * slow peer. The explicit form of the startup guarantee ("everything I now send reaches
 * everyone who was already out there"): open, create topics, settle, publish. Call it
 * AFTER creating your topics: matching is per topic, so settling before they exist
 * guarantees nothing (which is also why open does not auto-settle). Most apps never need
 * it: the per-send match wait above gives the same guarantee lazily and more precisely.
 * Waits by sleeping on a running service thread's progress, else by driving the poll
 * loop. timeout_ms < 0 = 3 announce intervals. Returns 1 settled, 0 on timeout (or from
 * a callback). */
int      dart_node_settle(DartNode *n, int timeout_ms);
#ifdef DART_SHM
/* Messages published / delivered via the zero-fragment shared-memory path since open
 * (observability; same-host subscribers only). Either out-pointer may be NULL. */
void     dart_node_shm_stats(DartNode *n, uint32_t *sent, uint32_t *recv);
#endif

/* Cumulative per-topic message counters, always on: messages/bytes this node's app
 * committed to the topic (send calls that returned DART_OK, zero-subscriber early-outs
 * included) and messages/bytes delivered to this node (accepted into the callback or
 * the consumer queue). Any out-pointer may be NULL. Also carried in the @dart/meta
 * snapshot's topics section. */
void     dart_topic_counts(DartTopic *topic, uint64_t *tx_msgs, uint64_t *tx_bytes,
                           uint64_t *rx_msgs, uint64_t *rx_bytes);

/* ---- logs: the built-in @dart/log/{error,warn,info} topics --------------------------
 * Every node hosts three SHARED reliable log topics (plain topics, created at open,
 * default on; opts.disable_logs strips them). Identity and history come free from the
 * transport: DartMsg.publisher_name/id says who logged, and catch_up replay is PER
 * WRITER LANE, so a late subscriber (the explorer) receives each node's last
 * keep_last lines per level the moment it joins. The record schema is
 *   DartLog { wall_us: u64, mono_us: u64, text: string }
 * (level implied by the topic; wall_us compares across nodes, mono_us orders within
 * one). qos: reliable, keep_last = catch_up = 16 (8 for info), NO backpressure wait:
 * a slow or absent subscriber can never block the app; it just loses the oldest lines
 * (KEEP_LAST). dart_node_log is thread-safe (the ordinary send path) and legal from
 * callbacks. To CONSUME a level, take your node's own handle from dart_node_log_topic,
 * widen its role to DART_PUBSUB (dart_topic_set_role), and read it like any topic
 * (on_message, or take/dispatch): you then receive every OTHER node's lines at that
 * level, never your own (a node does not deliver to itself). */
typedef enum { DART_LOG_ERROR = 0, DART_LOG_WARN = 1, DART_LOG_INFO = 2 } DartLogLevel;
#ifndef DART_LOG_MAX
#define DART_LOG_MAX 512   /* max formatted log-text bytes (longer output is truncated) */
#endif
/* printf-style publish on the level's log topic. Returns DART_OK, DART_ERR_NOSYS when
 * the log topics are disabled, or a negative DartResult from the send. */
int          dart_node_log(DartNode *n, DartLogLevel level, const char *fmt, ...);
/* Publish an already-formatted line (len<0 = NUL-terminated): the FFI-friendly entry
 * language bindings call after formatting in their own runtime, so the variadic
 * dart_node_log stays a C convenience. Same return values, truncated at DART_LOG_MAX. */
int          dart_node_log_text(DartNode *n, DartLogLevel level, const char *text, int len);
/* The node's own handle for a level's log topic (NULL when disabled): subscribe,
 * take/dispatch, or query it like any other topic. */
DartTopic   *dart_node_log_topic(DartNode *n, DartLogLevel level);

/* ---- the @dart/meta introspection snapshot ------------------------------------------
 * Section mask bits for a @dart/meta request (a 4-byte LE u32 payload; empty or 0 =
 * everything). The response is a message of the schema `DartMeta { info: map }`: the
 * map body is self-describing (dart_map_get / dart_map_at decode it with no schema),
 * one top-level key per requested section:
 *   "node"   uptime_us, wall_us, name, mem in_use/peak/alloc_calls, evicted_unsent,
 *            backpressure waited_us/waits, peers/max_peers, topics/max_topics,
 *            shm tx/rx, last_error (+ text)
 *   "proc"   pid, cpu_us, rss, peak_rss -- PER PROCESS (dedup by pid across nodes); ESP also
 *            reports heap_total/free/min_free/largest_free_block for MALLOC_CAP_DEFAULT;
 *            absent where the platform offers no measurement (DART_PROC_STATS off:
 *            auto-detected like DART_SHM, DART_NO_PROC_STATS forces it off, and a
 *            platform layer without it implements nothing; see platform/core.h)
 *   "topics" an array, one nested map per created topic: index, name, kind, role,
 *            reliability/keep_last/catch_up, matched subs/pubs, pending candidates,
 *            tx/rx msgs+bytes, the repair counters, consumer-queue stats
 *   "peers"  an array, one nested map per known peer: id, name, active, ip, port,
 *            age_us, publish_to/receive_from match counts
 * The endpoint itself lives in the patterns layer (dart_node_meta_function,
 * patterns/core.h); this mask and the snapshot builder seam below are node-level so
 * the data works without patterns compiled in. */
#define DART_META_NODE   0x1u
#define DART_META_PROC   0x2u
#define DART_META_TOPICS 0x4u
#define DART_META_PEERS  0x8u

/* ---- internal hooks for the patterns layer (src/patterns) ---------------------------
 * The patterns layer (functions / variables / signals) builds on a node but needs three
 * node-internal seams the public API does not expose: create a topic carrying an entity
 * kind + payload prefix (and a reserved '@' name), route that topic's messages to a
 * pattern handler instead of the app's on_message, and observe node-wide events + a
 * per-poll tick for call timeouts. These are i_-prefixed and kind-agnostic; the node
 * knows nothing of what functions/variables/signals mean. */
typedef void     (*i_DartSysMsgFn)(void *user, const DartMsg *msg);
typedef void     (*i_DartSysEventFn)(void *user, const DartEvent *ev);
typedef uint64_t (*i_DartSysTickFn)(void *user, uint64_t now_us);   /* returns next deadline us (0 = none) */
typedef void     (*i_DartSysCloseFn)(void *user);   /* node closing: settle outstanding promises */

/* Create a pattern topic: like dart_node_create_topic, but stamps the entity kind, the
 * per-payload prefix, the directed flag, and the forceable aux flag (advertised in interest
 * bit 6), permits '@' in the name (reserved for pattern channels), and routes this topic's
 * deliveries to on_msg (may be NULL) instead of the node's on_message. Never queued. Returns
 * a handle or NULL. */
DartTopic *i_dart_node_create_pattern_topic(DartNode *n, const char *name, DartRole role,
                              const DartSchema *schema, const DartTopicOpts *opts,
                              uint8_t kind, uint8_t prefix_bytes, uint8_t directed, uint8_t forceable,
                              i_DartSysMsgFn on_msg, void *on_msg_user);
/* Publish hdr+payload on a pattern topic (broadcast to all matched subscribers). */
int  i_dart_topic_send_hdr(DartTopic *topic, DartBytes hdr, DartBytes data);
/* Publish hdr+payload to ONE peer, point-to-point (function replies). */
int  i_dart_topic_send_to(DartTopic *topic, uint32_t to_peer, DartBytes hdr, DartBytes data);
/* Register the patterns layer's node-wide event observer + per-poll tick + close hook
 * (NULL clears). The tick runs each poll pass with now_us and returns its next deadline,
 * folded into the poll wait cap so call timeouts fire on time with no traffic. The close
 * hook fires ONCE at the top of dart_node_close (service thread already joined, node still
 * fully alive) so pending call outcomes can be synthesized before teardown. */
void i_dart_node_set_sys_hooks(DartNode *n, i_DartSysEventFn on_event, i_DartSysTickFn tick,
                               i_DartSysCloseFn on_close, void *user);
/* Node-pool alloc/realloc/free (size 0 = free) for the patterns layer; its per-node manager
 * handle slot; the node's monotonic clock (us); and its WALL clock (UTC us), the same source
 * the transport stamps DartMsg.sent_us from, for a locally-applied write that never rode the
 * wire. Call only under the node lock. */
void    *i_dart_node_sys_alloc(DartNode *n, void *ptr, size_t size);
void   **i_dart_node_sys_slot (DartNode *n);
uint64_t i_dart_node_now_us   (DartNode *n);
uint64_t i_dart_node_wall_us  (DartNode *n);
/* Node lock for a pattern call that mutates manager state: 1 = acquired here, 0 = already
 * held by this thread (a pattern call from inside a callback). sys_unlock releases WITHOUT
 * kicking (the send helpers and create kick for themselves; read-only ops must stay silent).
 * sys_poll drives one loop tick (for a sync call that owns no service thread). */
int      i_dart_node_sys_lock  (DartNode *n);
void     i_dart_node_sys_unlock(DartNode *n, int acquired);
int      i_dart_node_sys_poll  (DartNode *n, int timeout_ms);
/* Fire a topic-scoped DART_ERROR from the patterns layer (the duplicate-authority
 * diagnostic): fills .topic/.topic_name/.peer and routes through the node's normal
 * event path (last-error slot + on_event). Call under the node lock. */
void     i_dart_node_sys_error (DartNode *n, DartErrorKind error, DartTopic *topic, uint32_t peer);
/* Matched subscribers excluding dormant peers: the patterns layer's provider-liveness query
 * (dart_topic_match_count counts a dropped-but-resumable peer as still matched). */
int      i_dart_topic_live_match_count(DartTopic *topic);
/* Matched publishers feeding this topic's subscription side (mirror of dart_topic_match_count). */
int      i_dart_topic_source_match_count(DartTopic *topic);
/* Reflection getters for the patterns layer's entity enumeration. */
uint8_t    i_dart_topic_kind (const DartTopic *topic);   /* DartTopicKind */
uint8_t    i_dart_topic_role (const DartTopic *topic);   /* DartRole (current) */
DartString i_dart_topic_name (const DartTopic *topic);   /* the stable name copy */
uint16_t   i_dart_node_topic_count(DartNode *n);         /* created topics (handles 0..count) */
/* Build the introspection snapshot for the DART_META_* mask (0 = all sections) into a
 * node-owned grown buffer: a serialize-layer MAP body (feed to dart_map_* or wrap in a
 * schema message). A view valid until the next call; {NULL,0} on OOM. The @dart/meta
 * handler (patterns) serves this; call under the node lock. */
DartBytes i_dart_node_snapshot(DartNode *n, uint32_t sections);

#ifdef __cplusplus
}
#endif
#endif /* DART_NODE_H */
