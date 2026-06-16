/* NODE runtime over dart_transport: owns the data socket, drives discovery,
 * wires peers into the transport. */
#ifndef DART_NODE_H
#define DART_NODE_H

#include "dart_transport.h"
#include "dart_discovery.h"    /* dart_discovery_addr (seed peers) */

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
    const dart_discovery_addr *seed_peers;   /* peers to also unicast announces to (port 0 =
                                                discovery_port), so discovery works without multicast */
    uint16_t              n_seed_peers;
    uint32_t              recv_buffer_bytes; /* data-socket SO_RCVBUF; 0 = OS default */
    uint32_t              send_buffer_bytes; /* data-socket SO_SNDBUF; 0 = OS default */
    uint16_t              fragment_size;     /* UDP payload bytes per fragment this node sends;
                                                0 = DART_FRAG_PAYLOAD. Advertised via discovery so
                                                peers reassemble at our size. Clamp [MIN, MAX]; raise
                                                MAX (compile) for jumbo frames. One size per node. */
} dart_node_net;

/* Discovery cadence and peer-table size; zero-means-default (defaults shown). */
typedef struct {
    uint32_t              announce_interval_us; /* "I'm here" broadcast period; 1s */
    uint32_t              peer_timeout_us;   /* drop a peer after this silence; 3.5s */
    uint16_t              max_peers;         /* peer-table capacity; 16 */
} dart_node_discovery;

/* The top fields are what most nodes set; the two sub-structs default whole when
 * zero-initialized:
 *   dart_node_config cfg = {
 *       .domain = 7, .channels = ch, .n_channels = 2, .on_message = on_message };
 */
typedef struct {
    uint16_t                domain;        /* logical-network selector */
    const dart_channel_def *channels;
    uint16_t                n_channels;
    dart_message_fn         on_message;
    dart_event_fn           on_event;      /* optional: loss/too-big/collision/peer up/down */
    void                   *user_data;     /* passed to every callback */
    dart_alloc_fn           allocator;     /* optional: set => dynamic message sizing */
    dart_node_net           net;           /* addressing/sockets (optional) */
    dart_node_discovery     discovery;     /* discovery cadence (optional) */
} dart_node_config;

typedef struct dart_node dart_node;

size_t   dart_node_required_memory(const dart_node_config *cfg);
dart_node *dart_node_open(void *mem, size_t mem_size, const dart_node_config *cfg);
int      dart_node_poll(dart_node *n, int timeout_ms);          /* one loop tick */
int      dart_node_send(dart_node *n, uint16_t channel, const void *data, size_t len);
/* Change a channel's role at runtime (DART_INACTIVE = off). Returns 0 ok, <0 unknown. */
int      dart_node_set_role(dart_node *n, uint16_t channel, uint8_t role);
/* Cumulative backpressure since open: us waited on slow readers and how many sends
 * waited. Either out-pointer may be NULL. */
void     dart_node_backpressure_stats(dart_node *n, uint64_t *waited_us, uint32_t *waited_sends);
/* Pump until every reader has acked all messages on channel, or timeout_ms elapses.
 * Returns 1 if drained, 0 on timeout. Call before close so a burst isn't cut by the BYE. */
int      dart_node_drain(dart_node *n, uint16_t channel, int timeout_ms);
/* Subscribers matched on this channel now; a one-shot publisher polls it before sending. */
int      dart_node_writer_match_count(dart_node *n, uint16_t channel);
void     dart_node_close(dart_node *n, int send_bye);

#ifdef __cplusplus
}
#endif
#endif /* DART_NODE_H */
