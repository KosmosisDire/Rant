/* The discovery runtime: sockets, clock and uuid over the core. dart_discovery_open owns
 * its memory. dart_discovery_place uses a caller buffer, which is how the node embeds it. */
#ifndef DART_DISCOVERY_RT_H
#define DART_DISCOVERY_RT_H

#include "../common/api.h"
#include "core.h"
#include "../common/alloc.h"
#include "../platform/core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DART_DISCOVERY_MAX_SEEDS 4

typedef struct DartDiscovery DartDiscovery;

/* Options for dart_discovery_open. Zero means default. The meta blob is opaque. */
typedef struct {
    uint16_t              domain;               /* default 0 */
    const char           *discovery_group;      /* default "239.255.0.7" */
    uint16_t              discovery_port;       /* default 7400 */
    const char           *multicast_interface;  /* pin to this interface IP, NULL = all */
    uint8_t               multicast_ttl;        /* default 1 */
    uint16_t              max_peers;            /* default 32 */
    DartDiscoveryEventFn  on_event;             /* optional */
    void                 *user;                 /* passed to on_event */
    const DartDiscoveryAddr *seed_peers;        /* unicast seeds for multicast filtered nets */
    uint16_t              n_seed_peers;
    uint8_t               unicast_only;         /* 1 = never touch multicast */
    DartBytes             meta;                 /* optional opaque overlay to advertise */
    uint16_t              meta_cap;        /* per peer incoming overlay buffer, 0 = default */
    uint16_t              peer_user_bytes;      /* scratch reserved per peer, 0 = none */
} DartDiscoveryConfig;

/* alloc is copied in and reset on close, so it may be a temporary. A NULL name is auto
 * generated and a NULL cfg means all defaults. NULL on failure, see dart_discovery_last_error. */
DART_API DartDiscovery   *dart_discovery_open(DartAllocator *alloc, const char *name, const DartDiscoveryConfig *cfg);

/* lifecycle */
/* One loop tick. 1 if a datagram arrived, 0 if idle, negative on a socket error. */
DART_API int        dart_discovery_poll(DartDiscovery *d, int timeout_ms);
/* Blocks: solicits, then pumps until the peer set is quiet for quiet_ms or timeout_ms
 * passes. Returns the peer count. */
DART_API int        dart_discovery_gather(DartDiscovery *d, int quiet_ms, int timeout_ms);
/* Optionally multicasts a BYE, then closes the sockets and frees owned memory. */
DART_API void       dart_discovery_close(DartDiscovery *d, int send_bye);

/* The live peer list by pointer, valid until the next poll. Each .user is writable in place. */
DART_API const DartDiscoveryPeer *dart_discovery_peers(DartDiscovery *d, uint16_t *count);

/* The sans-IO core, for the by id lookups. Valid for the runtime's life. */
DART_API DartDiscoveryState *dart_discovery_state(DartDiscovery *d);

/* advanced: placement open */
/* The placement config. Zero and NULL fields get defaults. A zero uuid is auto generated. */
typedef struct {
    DartDiscoveryCoreConfig discovery;        /* the core config */
    const char  *group;       /* default "239.255.0.7" */
    uint16_t     discovery_port;   /* default 7400 */
    uint8_t      ttl;         /* default 1 */
    const char  *multicast_interface;    /* this interface IP only, never rescanned. NULL = every
                                 interface, "127.0.0.1" = single host isolation */
    const DartDiscoveryAddr *seeds;  /* also unicast announces here, at most MAX_SEEDS */
    uint16_t     n_seeds;
    uint8_t      unicast_only;  /* 1 = no multicast at all. Implies discovery.relay_me. Seed at
                                 least one peer, or be seeded by one, or nothing can find us. */
} DartDiscoveryNetConfig;

DART_API size_t     dart_discovery_placement_memory(const DartDiscoveryNetConfig *cfg);
/* Places a runtime in caller memory. The caller owns mem. NULL on failure, and
 * dart_discovery_last_error names the step. */
DART_API DartDiscovery   *dart_discovery_place(void *mem, size_t mem_size, const DartDiscoveryNetConfig *cfg);

/* Why the last open or place returned NULL. A process global with no lock, read it right after. */
typedef enum {
    DART_DISCOVERY_OK = 0,
    DART_DISCOVERY_E_MEMORY,      /* the buffer was too small */
    DART_DISCOVERY_E_PLATFORM,    /* net startup failed */
    DART_DISCOVERY_E_SOCKET,      /* socket open failed */
    DART_DISCOVERY_E_BIND,        /* bind to the discovery port failed */
    DART_DISCOVERY_E_MCAST_JOIN   /* joining the group failed */
} DartDiscoveryPlaceError;
DART_API DartDiscoveryPlaceError dart_discovery_last_error(void);
/* The OS socket error captured with the last failure, 0 if none. */
DART_API int        dart_discovery_last_os_error(void);
/* Relocates a placed runtime into a bigger block, keeping the socket, uuid and peers.
 * self_meta is the new announce blob address. The caller frees the old block after. */
DART_API DartDiscovery   *dart_discovery_migrate(DartDiscovery *old, void *new_mem, size_t new_cap,
                 uint16_t new_max_peers, uint16_t new_meta_cap, const uint8_t *self_meta, void *peer_cb_user);

/* node integration */
/* A discovery datagram that arrived on the data socket. src carries the port too, so the
 * core can bind an observed source. */
DART_API void       dart_discovery_feed(DartDiscovery *d, const DartDiscoveryAddr *src, DartBytes datagram);
/* Routes unicast discovery TX out of fd, the node's data socket. DART_SOCK_BAD restores
 * the own socket. Group TX stays on the own socket. */
DART_API void       dart_discovery_set_tx_fd(DartDiscovery *d, i_DartSock fd);
/* Replaces the overlay and bumps its version. meta must outlive the runtime. */
DART_API void       dart_discovery_advertise(DartDiscovery *d, DartBytes meta);
/* Re applies every peer's interest. Call after changing our own advertised meta. */
DART_API void       dart_discovery_replay(DartDiscovery *d);
/* The receive sockets, 1 or 2, for a caller with its own wait. Stable across a migrate. */
DART_API int        dart_discovery_pollfds(DartDiscovery *d, i_DartSock out[2]);
/* dart_discovery_poll without the wait. Pass each fd's readability in pollfds order. Call
 * it every pass, the clock driven work needs no readable fd. */
DART_API int        dart_discovery_service(DartDiscovery *d, int fd_readable, int unicast_readable);

/* uuid */
/* A random RFC 9562 v4 uuid. 0 if there is no entropy source. */
DART_API int        dart_discovery_make_uuid4(uint8_t out[16]);
/* The CSPRNG path, else a host identity fallback. Shared with the node's DartUuid. */
void       i_dart_discovery_auto_uuid(uint8_t out[16]);

/* Resolves an advertised name into out: want clamped, or an auto "node-XXXXXXXX" when
 * want is NULL or empty. Returns the length. */
DART_API uint8_t    dart_discovery_default_name(char *out, size_t cap, const char *want);

#ifdef __cplusplus
}
#endif
#endif /* DART_DISCOVERY_RT_H */
