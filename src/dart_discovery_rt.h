/* peer-discovery runtime API: UDP multicast, clock, UUID
 * and one-tick loop over the dart_discovery core. On non-MSVC Windows, link
 * -lws2_32 -lbcrypt. */
#ifndef DART_DISCOVERY_RT_H
#define DART_DISCOVERY_RT_H

#include "dart_discovery.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Zero/NULL fields get defaults. Leave disc.uuid all-zero to auto-generate a
 * per-boot random v4 UUID. */
typedef struct {
    dart_discovery_config disc;        /* core config: ids, timing, callbacks */
    const char  *group;       /* multicast group, default "239.255.0.7" */
    uint16_t     disc_port;   /* rendezvous port, default 7400 */
    uint8_t      ttl;         /* multicast TTL, default 1 */
    const char  *mcast_if;    /* interface IP to join/send on; NULL = route probe.
                                 "127.0.0.1" keeps single-host setups off the
                                 network. */
} dart_discovery_rt_config;

typedef struct dart_discovery_rt dart_discovery_rt;

size_t     dart_discovery_rt_required_memory(const dart_discovery_rt_config *cfg);
/* Opens the socket, joins the group, places core state in mem. NULL on failure. */
dart_discovery_rt  *dart_discovery_rt_open(void *mem, size_t mem_size, const dart_discovery_rt_config *cfg);
/* One loop tick: waits up to timeout_ms for a datagram, feeds RX to the core,
 * pumps timeouts and announcements, sends what's due. Returns 1 if a datagram
 * arrived, 0 if idle, <0 on socket error. */
int        dart_discovery_rt_poll(dart_discovery_rt *rt, int timeout_ms);
/* Optionally multicast a graceful BYE, then close the socket. */
void       dart_discovery_rt_close(dart_discovery_rt *rt, int send_bye);

/* Fill out[16] with a random RFC 9562 v4 UUID from the platform CSPRNG.
 * Returns 1 on success, 0 if no entropy source was available. */
int        dart_discovery_make_uuid4(uint8_t out[16]);

/* Egress interface the OS routes to group:port (INADDR_ANY on failure). Exposed
 * so layers above can pin their multicast sockets to the same interface:
 * multihomed hosts otherwise pick different ones per socket and break peer
 * matching by source address. */
uint32_t   dart_discovery_mcast_if_for(uint32_t group_naddr, uint16_t port);

#ifdef __cplusplus
}
#endif
#endif /* DART_DISCOVERY_RT_H */
