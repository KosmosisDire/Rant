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
#include "../common/alloc.h"        /* DartAllocator (shared with the node runtime) */
#include "../platform/core.h"       /* i_DartSock (dart_discovery_pollfds) */

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
    const char           *multicast_interface;  /* pin to this interface IP; NULL = every interface */
    uint8_t               multicast_ttl;        /* hops; 1 */
    uint16_t              max_peers;            /* table capacity; 32 */
    DartDiscoveryEventFn  on_event;             /* optional: PEER_UP / PEER_DOWN / PEER_REFUSED */
    void                 *user;                 /* passed to on_event */
    const DartDiscoveryAddr *seed_peers;        /* unicast seeds for multicast-filtered nets */
    uint16_t              n_seed_peers;
    uint8_t               unicast_only;         /* 1 = never touch multicast (see the net config) */
    DartBytes             meta;                 /* optional OPAQUE overlay to advertise; {NULL,0} = none */
    uint16_t              meta_cap;        /* per-peer INCOMING overlay buffer; 0 = default */
    uint16_t              peer_user_bytes;      /* opaque scratch reserved per peer; 0 = none
                                                   (see dart_discovery_peer_user) */
} DartDiscoveryConfig;

/* Open a discovery runtime backed by `alloc` (a static or dynamic DartAllocator, copied
 * in and reset on close, so it may be a temporary). name is this instance's advertised peer
 * name (a primary arg, like dart_node_open); NULL/empty => an auto-generated "node-XXXXXXXX".
 * cfg may be NULL for all defaults. The UUID is auto-generated. Returns NULL on failure
 * (allocator too small / socket setup failed). Close with dart_discovery_close. */
DartDiscovery   *dart_discovery_open(DartAllocator *alloc, const char *name, const DartDiscoveryConfig *cfg);

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
    const char  *multicast_interface;    /* join/send on THIS interface IP only (and never
                                 rescan); NULL = every interface this host has,
                                 "127.0.0.1" = single-host isolation */
    const DartDiscoveryAddr *seeds;  /* peers to also unicast announces to, for
                                 networks where multicast is filtered (max DART_DISCOVERY_MAX_SEEDS) */
    uint16_t     n_seeds;
    uint8_t      unicast_only;  /* 1 = this host cannot multicast at all (a stack without IGMP, a
                                 segment that filters it): join nothing, send to seeds and known
                                 peers only, and never fail open for want of a membership. Implies
                                 discovery.relay_me, so peers that CAN multicast re-announce us on
                                 their paths (dart_discovery_poll_relay) and one seeded address is
                                 enough to become discoverable mesh-wide. Seed at least one peer,
                                 or be seeded BY one: with neither, nothing can ever find us. */
} DartDiscoveryNetConfig;

size_t     dart_discovery_placement_memory(const DartDiscoveryNetConfig *cfg);
/* Place a runtime in caller memory (mem[0..mem_size)): open the socket, join the group,
 * init core state. NULL on failure. The caller owns mem (dart_discovery_close frees only
 * the socket, not mem). On NULL, dart_discovery_last_error names the failed step. */
DartDiscovery   *dart_discovery_place(void *mem, size_t mem_size, const DartDiscoveryNetConfig *cfg);

/* Why the most recent dart_discovery_open / dart_discovery_place returned NULL, so a
 * caller (or the node, translating to its own DART_ERROR event) can report the step.
 * Best-effort: a process-global with no lock, meaningful right after a NULL return. */
typedef enum {
    DART_DISCOVERY_OK = 0,
    DART_DISCOVERY_E_MEMORY,      /* the allocator/buffer was too small for the core */
    DART_DISCOVERY_E_PLATFORM,    /* platform net startup failed (WSAStartup) */
    DART_DISCOVERY_E_SOCKET,      /* udp socket open failed */
    DART_DISCOVERY_E_BIND,        /* bind to the discovery port failed (in use?) */
    DART_DISCOVERY_E_MCAST_JOIN   /* joining the multicast group failed (bad interface?) */
} DartDiscoveryPlaceError;
DartDiscoveryPlaceError dart_discovery_last_error(void);
/* The OS socket errno captured alongside the last SOCKET/BIND/MCAST_JOIN failure (0 if
 * none / not applicable). */
int        dart_discovery_last_os_error(void);
/* Relocate a placed runtime into a bigger block at grown counts, preserving the live
 * socket, UUID and peer table. self_meta = the new announce-blob address. Caller frees
 * the old block afterward. Placement (caller-owned) path only. */
DartDiscovery   *dart_discovery_migrate(DartDiscovery *old, void *new_mem, size_t new_cap,
        uint16_t new_max_peers, uint16_t new_meta_cap, const uint8_t *self_meta, void *peer_cb_user);

/* ----------------------------------------------------------- node integration */
/* Hand the core a discovery datagram that arrived on the ADVERTISED (data) port's socket
 * (unicast announces target the peer's data port, so the data-socket owner forwards
 * them). src is the datagram's source address; the core needs the port too, to bind a
 * translated peer's observed source. */
void       dart_discovery_feed(DartDiscovery *d, const DartDiscoveryAddr *src, DartBytes datagram);
/* Route all discovery TX out of `fd` instead of the runtime's own socket (DART_SOCK_BAD
 * restores it). A unicast-only node passes its DATA socket: every announce then leaves
 * the socket its data will use, so behind a NAT the announces themselves open, identify
 * (by uuid) and keep alive the per-peer mappings, and a peer's reply to the announce's
 * source is already on the data path. Unicast-only runtimes only: group TX stays on the
 * runtime's socket (its multicast options live there), and unicast_only sends none. */
void       dart_discovery_set_tx_fd(DartDiscovery *d, i_DartSock fd);
/* Replace the opaque overlay carried in announces and bump its version, so peers
 * re-fetch it (e.g. after an interest change). meta must outlive the runtime. */
void       dart_discovery_advertise(DartDiscovery *d, DartBytes meta);
/* Re-apply every known peer's interest against our current local state (see
 * dart_discovery_replay_peers). Call after changing our own advertised meta so a newly
 * added local topic matches interest peers advertised before it existed. */
void       dart_discovery_replay(DartDiscovery *d);
/* This runtime's receive sockets (the multicast group fd, plus the own unicast RX fd
 * when one exists), for a caller embedding discovery in its own blocking wait. Fills
 * out[0..1] and returns the count (1 or 2). The fds are stable across a migrate. */
int        dart_discovery_pollfds(DartDiscovery *d, i_DartSock out[2]);
/* dart_discovery_poll without its own socket wait, for a caller whose OWN wait covers
 * the dart_discovery_pollfds sockets: pass each fd's readability (same order) and the
 * drains, the announce/timeout update, and the targeted replies all run with zero
 * extra syscalls. Call every pass: the clock-driven work needs no readable fd. */
int        dart_discovery_service(DartDiscovery *d, int fd_readable, int unicast_readable);

/* ----------------------------------------------------------------------- UUID */
/* Fill out[16] with a random RFC 9562 v4 UUID; 1 ok, 0 if no entropy source. */
int        dart_discovery_make_uuid4(uint8_t out[16]);

/* Resolve an advertised peer name into out[cap]: the caller's want (clamped to cap-1 and
 * DART_DISCOVERY_NAME_MAX), or an auto-generated "node-XXXXXXXX" if want is NULL/empty.
 * Returns its length. Shared by dart_discovery_open and the node so both name peers the
 * same way. */
uint8_t    dart_discovery_default_name(char *out, size_t cap, const char *want);

#ifdef __cplusplus
}
#endif
#endif /* DART_DISCOVERY_RT_H */
