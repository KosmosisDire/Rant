/* NODE runtime over the dart_transport core: owns the data socket,
 * drives discovery, wires peers into the transport. */
#ifndef DART_NODE_H
#define DART_NODE_H

#include "dart_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t              domain_id;     /* discovery domain                  */
    uint16_t              data_port;     /* unicast data port; 0 = OS-assigned */
    const char           *disc_group;    /* default "239.255.0.7"             */
    uint16_t              disc_port;     /* default 7400                      */
    uint16_t              mc_port;       /* shared multicast DATA port; default
                                            disc_port+1. Per-channel group is
                                            239.255.<domain&255>.<chan&255>   */
    const char           *mcast_if;      /* interface IP for all multicast;
                                            NULL = auto. "127.0.0.1" keeps a
                                            single-host run off the network   */
    uint32_t              announce_us;   /* default 1s                        */
    uint32_t              timeout_us;    /* default 3.5s                      */
    uint8_t               ttl;           /* multicast TTL, default 1          */
    uint16_t              max_peers;     /* default 16                        */
    uint32_t              so_rcvbuf;     /* data-socket SO_RCVBUF bytes; 0 = OS
                                            default. Bigger absorbs bursts at
                                            the cost of queuing delay         */
    uint32_t              so_sndbuf;     /* data-socket SO_SNDBUF; 0 = default */
    const dart_channel_def *channels;      /* transport channels                */
    uint16_t              n_channels;
    dart_sample_fn          on_sample;     /* sample delivery                   */
    dart_gap_fn             on_gap;        /* optional: permanently skipped TUs */
    void                 *user;
} dart_node_config;

typedef struct dart_node dart_node;

size_t   dart_node_required_memory(const dart_node_config *cfg);
dart_node *dart_node_open(void *mem, size_t mem_size, const dart_node_config *cfg);
int      dart_node_poll(dart_node *n, int timeout_ms);          /* one loop tick   */
int      dart_node_send(dart_node *n, uint16_t channel_id, const void *data, size_t len);
/* Cumulative backpressure since open: microseconds dart_node_send waited on
 * slow readers and how many sends waited. Either out-pointer may be NULL. */
void     dart_node_block_stats(dart_node *n, uint64_t *block_us, uint32_t *blocked_sends);
void     dart_node_close(dart_node *n, int send_bye);

#ifdef __cplusplus
}
#endif
#endif /* DART_NODE_H */
