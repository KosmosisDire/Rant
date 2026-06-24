/* peer-discovery runtime: UDP multicast, clock, UUID, and a one-tick loop over
 * the dart_discovery core. On non-MSVC Windows, link -lws2_32 -lbcrypt. */
#ifndef DART_DISCOVERY_RT_H
#define DART_DISCOVERY_RT_H

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DART_DISCOVERY_MAX_SEEDS 4

/* Zero/NULL fields get defaults; leave discovery.uuid all-zero to auto-generate one. */
typedef struct {
    dart_discovery_config discovery;        /* core config: ids, timing, callbacks */
    const char  *group;       /* multicast group, default "239.255.0.7" */
    uint16_t     discovery_port;   /* rendezvous port, default 7400 */
    uint8_t      ttl;         /* multicast TTL, default 1 */
    const char  *multicast_interface;    /* interface IP to join/send on; NULL = route probe,
                                 "127.0.0.1" = single-host */
    const dart_discovery_addr *seeds;  /* peers to also unicast announces to, for
                                 networks where multicast is filtered (max DART_DISCOVERY_MAX_SEEDS) */
    uint16_t     n_seeds;
} dart_discovery_rt_config;

typedef struct dart_discovery_rt dart_discovery_rt;

size_t     dart_discovery_rt_required_memory(const dart_discovery_rt_config *cfg);
/* Open the socket, join the group, place core state in mem. NULL on failure. */
dart_discovery_rt  *dart_discovery_rt_open(void *mem, size_t mem_size, const dart_discovery_rt_config *cfg);
/* One loop tick: wait up to timeout_ms for a datagram, feed RX, pump timers, send
 * what's due. Returns 1 if a datagram arrived, 0 if idle, <0 on socket error. */
int        dart_discovery_rt_poll(dart_discovery_rt *rt, int timeout_ms);
/* Gather membership at startup: solicit, then pump until the peer set is quiet for
 * quiet_ms or timeout_ms total. Re-solicits periodically. Returns the peer count. Blocks. */
int        dart_discovery_rt_settle(dart_discovery_rt *rt, int quiet_ms, int timeout_ms);
/* Optionally multicast a graceful BYE, then close the socket. */
void       dart_discovery_rt_close(dart_discovery_rt *rt, int send_bye);

/* Hand the core a discovery datagram that arrived on another socket (unicast
 * announces target the peer's data port, so the data-socket owner forwards them). */
void       dart_discovery_rt_feed(dart_discovery_rt *rt, const uint8_t *src_ip, uint8_t src_ip_len,
                          const void *datagram, size_t len);

/* Replace the opaque meta blob carried in announces and bump its version, so peers
 * re-fetch it (e.g. after an interest change). meta must outlive the runtime. */
void       dart_discovery_rt_set_meta(dart_discovery_rt *rt, const uint8_t *meta, uint16_t meta_len);

/* Fill out[16] with a random RFC 9562 v4 UUID; 1 ok, 0 if no entropy source. */
int        dart_discovery_make_uuid4(uint8_t out[16]);

/* Egress interface the OS routes to group:port (INADDR_ANY on failure). Exposed so
 * layers above pin their multicast sockets to the same interface on multihomed hosts. */
uint32_t   dart_discovery_mcast_if_for(uint32_t group_naddr, uint16_t port);

#ifdef __cplusplus
}
#endif
#endif /* DART_DISCOVERY_RT_H */
