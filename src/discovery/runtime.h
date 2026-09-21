/* The discovery runtime: sockets, clock and uuid over the core. rant_discovery_place runs
 * in a caller buffer, which is how the node embeds it. */
#ifndef RANT_DISCOVERY_RT_H
#define RANT_DISCOVERY_RT_H

#include "core.h"
#include "../platform/core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RANT_DISCOVERY_MAX_SEEDS 4

typedef struct RantDiscovery RantDiscovery;

/* The live peer list by pointer, valid until the next service pass. Each .user is
 * writable in place. */
const RantDiscoveryPeer *rant_discovery_peers(RantDiscovery *d, uint16_t *count);

/* The sans-IO core, for the by id lookups. Valid for the runtime's life. */
RantDiscoveryState *rant_discovery_state(RantDiscovery *d);

size_t       rant_discovery_placement_memory(const RantDiscoveryCoreConfig *cfg);
/* Places a runtime in caller memory, which the caller owns. cfg is the core config, a zero
 * uuid is auto generated. net gives the group, port, ttl, interface, seeds and
 * unicast_only, where unicast_only implies cfg.relay_me. NULL on failure, and
 * rant_discovery_last_error names the step. */
RantDiscovery       *rant_discovery_place(void *mem, size_t mem_size,
                                          const RantDiscoveryCoreConfig *cfg, const RantNodeNet *net);

/* Why the last place returned NULL: OOM (the buffer was too small), PLATFORM, SOCKET, BIND
 * or MCAST_JOIN. A process global with no lock, read it right after. */
RantErrorKind rant_discovery_last_error(void);
/* The OS socket error captured with the last failure, 0 if none. */
int          rant_discovery_last_os_error(void);
/* Relocates a placed runtime into a bigger block, keeping the socket, uuid and peers.
 * self_meta is the new announce blob address. The caller frees the old block after. */
RantDiscovery       *rant_discovery_migrate(RantDiscovery *old, void *new_mem, size_t new_cap,
                 uint16_t new_max_peers, uint16_t new_meta_cap, const uint8_t *self_meta, void *peer_cb_user);

/* Optionally multicasts a BYE, then closes the sockets. The caller frees mem after. */
void         rant_discovery_close(RantDiscovery *d, int send_bye);

/* node integration */
/* A discovery datagram that arrived on the data socket. src carries the port too, so the
 * core can bind an observed source. */
void         rant_discovery_feed(RantDiscovery *d, const RantAddr *src, RantBytes datagram);
/* Routes unicast discovery TX out of fd, the node's data socket. RANT_SOCK_BAD restores
 * the own socket. Group TX stays on the own socket. */
void         rant_discovery_set_tx_fd(RantDiscovery *d, i_RantSock fd);
/* Replaces the overlay and bumps its version. meta must outlive the runtime. */
void         rant_discovery_advertise(RantDiscovery *d, RantBytes meta);
/* Re applies every peer's interest. Call after changing our own advertised meta. */
void         rant_discovery_replay(RantDiscovery *d);
/* The receive sockets, 1 or 2, for the caller's wait. Stable across a migrate. */
int          rant_discovery_pollfds(RantDiscovery *d, i_RantSock out[2]);
/* One service pass with no wait. Pass each fd's readability in pollfds order. Call it
 * every pass, the clock driven work needs no readable fd. */
int          rant_discovery_service(RantDiscovery *d, int fd_readable, int unicast_readable);

/* uuid */
/* A random RFC 9562 v4 uuid. 0 if there is no entropy source. */
int          rant_discovery_make_uuid4(uint8_t out[16]);
/* The CSPRNG path, else a host identity fallback. Shared with the node's RantUuid. */
void       i_rant_discovery_auto_uuid(uint8_t out[16]);

/* Resolves an advertised name into out: want clamped, or an auto "node-XXXXXXXX" when
 * want is NULL or empty. Returns the length. */
uint8_t      rant_discovery_default_name(char *out, size_t cap, const char *want);

#ifdef __cplusplus
}
#endif
#endif /* RANT_DISCOVERY_RT_H */
