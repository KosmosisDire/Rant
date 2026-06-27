/* peer-discovery runtime: UDP multicast, clock, UUID, and a one-tick loop over the
 * dart_discovery core. Two ways to construct one DartDiscovery:
 *   dart_discovery_open  - defaults-first, owns its memory via a DartAllocator
 *                          (mirrors dart_node_open; what standalone observers use).
 *   dart_discovery_place - advanced: place the runtime in a caller-provided buffer
 *                          (the node embeds discovery in its own arena this way).
 * On non-MSVC Windows, link -lws2_32 -lbcrypt. */
#ifndef DART_DISCOVERY_RT_H
#define DART_DISCOVERY_RT_H

#include "core.h"
#include "../common/allocator.h"   /* DartAllocator (shared with the node runtime) */

#ifdef __cplusplus
extern "C" {
#endif

#define DART_DISCOVERY_MAX_SEEDS 4

typedef struct DartDiscovery DartDiscovery;

/* -------------------------------------------------------- defaults-first open */
/* Flat, zero-means-default options for dart_discovery_open. Discovery is generic:
 * the optional meta blob is OPAQUE (a higher layer's overlay), carried verbatim. */
typedef struct {
    uint16_t              domain;               /* logical-network selector; 0 */
    const char           *discovery_group;      /* multicast group; "239.255.0.7" */
    uint16_t              discovery_port;       /* rendezvous port; 7400 */
    const char           *multicast_interface;  /* interface IP; NULL = auto (pin on multihomed) */
    uint8_t               multicast_ttl;        /* hops; 1 */
    uint16_t              max_peers;            /* table capacity; 32 */
    DartDiscoveryEventFn  on_event;             /* optional: PEER_UP / PEER_DOWN / PEER_REFUSED */
    void                 *user;                 /* passed to on_event */
    const DartDiscoveryAddr *seed_peers;        /* unicast seeds for multicast-filtered nets */
    uint16_t              n_seed_peers;
    const uint8_t        *meta;                 /* optional OPAQUE overlay to advertise; NULL = none */
    uint16_t              meta_len;
    uint16_t              meta_capacity;        /* per-peer INCOMING overlay buffer; 0 = default */
    uint16_t              peer_user_bytes;      /* opaque scratch reserved per peer; 0 = none
                                                   (see dart_discovery_peer_user) */
} DartDiscoveryConfig;

/* Open a discovery runtime backed by mem (a static or dynamic DartAllocator, taken
 * over here: mem->claimed is set). name is this instance's advertised peer name (a
 * primary arg, like dart_node_open); NULL/empty => an auto-generated "node-XXXXXXXX".
 * cfg may be NULL for all defaults. The UUID is auto-generated. Returns NULL on failure
 * (allocator too small / already claimed / socket setup failed). Close with
 * dart_discovery_close. */
DartDiscovery   *dart_discovery_open(DartAllocator *mem, const char *name, const DartDiscoveryConfig *cfg);

/* ------------------------------------------------------------------ lifecycle */
/* One loop tick: wait up to timeout_ms for a datagram, feed RX, pump timers, send
 * what's due. Returns 1 if a datagram arrived, 0 if idle, <0 on socket error. */
int        dart_discovery_poll(DartDiscovery *d, int timeout_ms);
/* Gather membership at startup: solicit, then pump until the peer set is quiet for
 * quiet_ms or timeout_ms total. Re-solicits periodically. Returns the peer count. Blocks. */
int        dart_discovery_gather(DartDiscovery *d, int quiet_ms, int timeout_ms);
/* Optionally multicast a graceful BYE, then close the socket and free owned memory. */
void       dart_discovery_close(DartDiscovery *d, int send_bye);

/* The live peer list, by pointer (zero copy). *count gets the length; the array is
 * valid until the next dart_discovery_poll mutates the table. Iterate it to find peers
 * by name/addr/overlay; the overlay is opaque (decode with the transport codec). Each
 * entry's .user points at that peer's scratch (cfg.peer_user_bytes), writable in place. */
const DartDiscoveryPeer *dart_discovery_peers(DartDiscovery *d, uint16_t *count);

/* The underlying sans-IO core. Advanced: a higher layer (e.g. the node core) uses it for
 * the by-id lookups (dart_discovery_peer_user / addr_of_id / peer_name / id_for_addr)
 * instead of keeping a parallel peer table. Valid for the runtime's life (NULL if d is). */
DartDiscoveryState *dart_discovery_state(DartDiscovery *d);

/* ---------------------------------------------------- advanced: placement open */
/* Network addressing + the embedded core config; zero/NULL fields get defaults. Leave
 * discovery.uuid all-zero to auto-generate one. Used by dart_discovery_place when a
 * caller (e.g. the node) supplies the memory and needs the full config surface. */
typedef struct {
    DartDiscoveryCoreConfig discovery;        /* core config: ids, timing, callbacks, meta */
    const char  *group;       /* multicast group, default "239.255.0.7" */
    uint16_t     discovery_port;   /* rendezvous port, default 7400 */
    uint8_t      ttl;         /* multicast TTL, default 1 */
    const char  *multicast_interface;    /* interface IP to join/send on; NULL = auto
                                 (route probe, falling back to a real LAN interface),
                                 "127.0.0.1" = single-host */
    const DartDiscoveryAddr *seeds;  /* peers to also unicast announces to, for
                                 networks where multicast is filtered (max DART_DISCOVERY_MAX_SEEDS) */
    uint16_t     n_seeds;
} DartDiscoveryNetConfig;

size_t     dart_discovery_placement_memory(const DartDiscoveryNetConfig *cfg);
/* Place a runtime in caller memory (mem[0..mem_size)): open the socket, join the group,
 * init core state. NULL on failure. The caller owns mem (dart_discovery_close frees only
 * the socket, not mem). */
DartDiscovery   *dart_discovery_place(void *mem, size_t mem_size, const DartDiscoveryNetConfig *cfg);
/* Relocate a placed runtime into a bigger block at grown counts, preserving the live
 * socket, UUID and peer table. self_meta = the new announce-blob address. Caller frees
 * the old block afterward. Placement (caller-owned) path only. */
DartDiscovery   *dart_discovery_migrate(DartDiscovery *old, void *new_mem, size_t new_cap,
        uint16_t new_max_peers, uint16_t new_meta_capacity, const uint8_t *self_meta, void *peer_cb_user);

/* ----------------------------------------------------------- node integration */
/* Hand the core a discovery datagram that arrived on another socket (unicast announces
 * target the peer's data port, so the data-socket owner forwards them). */
void       dart_discovery_feed(DartDiscovery *d, const uint8_t *src_ip, uint8_t src_ip_len,
                          const void *datagram, size_t len);
/* Replace the opaque overlay carried in announces and bump its version, so peers
 * re-fetch it (e.g. after an interest change). meta must outlive the runtime. */
void       dart_discovery_advertise(DartDiscovery *d, const uint8_t *meta, uint16_t meta_len);
/* Re-apply every known peer's interest against our current local state (see
 * dart_discovery_replay_peers). Call after changing our own advertised meta so a newly
 * added local channel matches interest peers advertised before it existed. */
void       dart_discovery_replay(DartDiscovery *d);

/* ---------------------------------------------------------------- UUID / iface */
/* Fill out[16] with a random RFC 9562 v4 UUID; 1 ok, 0 if no entropy source. */
int        dart_discovery_make_uuid4(uint8_t out[16]);

/* Resolve an advertised peer name into out[cap]: the caller's want (clamped to cap-1 and
 * DART_DISCOVERY_NAME_MAX), or an auto-generated "node-XXXXXXXX" if want is NULL/empty.
 * Returns its length. Shared by dart_discovery_open and the node so both name peers the
 * same way. */
uint8_t    dart_discovery_default_name(char *out, size_t cap, const char *want);

/* The one interface every multicast socket should pin to: route-probe group:port,
 * falling back to the default-route LAN interface (a multicast route can resolve to
 * loopback on Windows) and then to interface enumeration. INADDR_ANY (0) only if nothing
 * usable is found. Exposed so layers above pin to the same interface on multihomed hosts. */
uint32_t   dart_discovery_mcast_if_for(uint32_t group_naddr, uint16_t port);

#ifdef __cplusplus
}
#endif
#endif /* DART_DISCOVERY_RT_H */
