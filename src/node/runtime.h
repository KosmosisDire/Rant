/* NODE runtime (the public dart_node_* API): owns the data sockets, drives
 * discovery, and the clock; it wires peers into the transport via the sans-IO
 * node core (node/core.h). A second transport would be a new runtime over that
 * same core. */
#ifndef DART_NODE_H
#define DART_NODE_H

#include "../transport/core.h"
#include "../discovery/core.h"    /* DartDiscoveryAddr (seed peers) */

#ifdef __cplusplus
extern "C" {
#endif

/* Network addressing and sockets; every field is zero-means-default (defaults shown). */
typedef struct {
    uint16_t              data_port;         /* unicast data port; 0 = OS-assigned */
    const char           *discovery_group;   /* "239.255.0.7" */
    uint16_t              discovery_port;    /* 7400 */
    uint16_t              multicast_port;    /* shared multicast data port; discovery_port+1 */
    const char           *multicast_interface;/* interface IP for all multicast; NULL = auto,
                                                "127.0.0.1" = single-host. Pin on multihomed hosts */
    uint8_t               multicast_ttl;     /* hops multicast may travel; 1 */
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

/* The top fields are what most nodes set; the two sub-structs default whole when
 * zero-initialized:
 *   DartNodeConfig cfg = {
 *       .domain = 7, .channels = ch, .n_channels = 2, .on_message = on_message };
 */
typedef struct {
    uint16_t                domain;        /* logical-network selector */
    const DartChannelDef *channels;
    uint16_t                n_channels;
    DartMessageFn         on_message;
    DartEventFn           on_event;      /* optional: loss/too-big/collision/peer up/down */
    void                   *user_data;     /* passed to every callback */
    DartAllocFn           allocator;     /* optional: set => dynamic message sizing */
    DartNodeNet           net;           /* addressing/sockets (optional) */
    DartNodeDiscovery     discovery;     /* discovery cadence (optional) */
} DartNodeConfig;

typedef struct DartNode DartNode;

size_t   dart_node_required_memory(const DartNodeConfig *cfg);
DartNode *dart_node_open(void *mem, size_t mem_size, const DartNodeConfig *cfg);
int      dart_node_poll(DartNode *n, int timeout_ms);          /* one loop tick */
int      dart_node_send(DartNode *n, uint16_t channel, const void *data, size_t len);
/* Change a channel's role at runtime (DART_INACTIVE = off). Returns 0 ok, <0 unknown. */
int      dart_node_set_role(DartNode *n, uint16_t channel, uint8_t role);
/* Cumulative backpressure since open: us waited on slow readers and how many sends
 * waited. Either out-pointer may be NULL. */
void     dart_node_backpressure_stats(DartNode *n, uint64_t *waited_us, uint32_t *waited_sends);
/* Cumulative reliable-repair counters for a channel (see DartRepairStats). The
 * per-second deltas are repair throughput; *out is zeroed for an unknown channel. */
void     dart_node_repair_stats(DartNode *n, uint16_t channel, DartRepairStats *out);

/* In-pump diagnostic probe. A reliable publisher blocks inside dart_node_send for the
 * whole backpressure wait, so its normal once-a-second print can't see within a stall.
 * A registered probe is called on a ~interval_us timer DURING that wait with the
 * writer's repair progress over each interval -- making the stall a within-block time
 * series (resends bursty-then-flat => reader stopped asking; steady => resends dropped).
 * Observational only; deltas are since the previous sample in the same wait. */
typedef struct {
    uint16_t channel;         /* channel being pumped */
    uint64_t wait_elapsed_us; /* us since this backpressure wait began */
    uint64_t interval_us;     /* us since the previous sample (normalise deltas by this for true /s) */
    uint64_t frags_resent;    /* writer DATA fragments resent in the interval */
    uint64_t nacks_recv;      /* repair NACKs received in the interval */
    uint32_t polls;           /* dart_node_poll calls in the interval */
    uint32_t polls_idle;      /* of those, polls with no repair pending (writer idle for lack of NACKs) */
} DartPumpSample;
typedef void (*DartPumpProbeFn)(void *user, const DartPumpSample *s);
/* Register the in-pump probe (NULL fn disables). interval_us 0 => default 200ms. */
void     dart_node_set_pump_probe(DartNode *n, DartPumpProbeFn fn, uint64_t interval_us, void *user);
/* Head-of-line reassembly snapshot for the in-progress message from `peer` on
 * `channel`: returns 1 + fills base_seqno/have/total if one is mid-reassembly, else 0.
 * `have` rising across calls = repair crawling; flat = wedged. Any pointer may be NULL. */
int      dart_node_reader_progress(DartNode *n, uint16_t channel, uint32_t peer,
                            uint64_t *base_seqno, uint32_t *have, uint32_t *total);
/* Pump until every reader has acked all messages on channel, or timeout_ms elapses.
 * Returns 1 if drained, 0 on timeout. Call before close so a burst isn't cut by the BYE. */
int      dart_node_drain(DartNode *n, uint16_t channel, int timeout_ms);
/* Subscribers matched on this channel now; a one-shot publisher polls it before sending. */
int      dart_node_writer_match_count(DartNode *n, uint16_t channel);
#ifdef DART_SHM
/* Messages published / delivered via the zero-fragment shared-memory path since open
 * (observability; same-host readers only). Either out-pointer may be NULL. */
void     dart_node_shm_stats(DartNode *n, uint32_t *sent, uint32_t *recv);
#endif
void     dart_node_close(DartNode *n, int send_bye);

#ifdef __cplusplus
}
#endif
#endif /* DART_NODE_H */
