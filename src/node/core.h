/* sans-IO NODE core: the peer table (peer id <-> physical address), the
 * discovery->transport lifecycle (add / resume / dormant / remove + interest +
 * frag + out-of-band capability, plus PEER_UP/DOWN/REFUSED events), and address
 * resolution. No socket, clock, or platform: a node RUNTIME owns IO and drives
 * this, so a second transport (serial, Bluetooth, ...) is a new runtime over this
 * same core with no sans-IO change. See node/runtime.h for the IO layer and the
 * public dart_node_* API.
 *
 * The one transport-specific thing the core does NOT own is mapping the abstract
 * destination to wire bytes: it resolves a peer id to a physical address record and
 * hands that to the runtime, which sends it however its link works. The group case
 * (DART_DEST_GROUP) is resolved entirely by the runtime, since the multicast-group
 * address is a per-transport convention (UDP: 239.255.<domain>.<sel>). */
#ifndef DART_NODE_CORE_H
#define DART_NODE_CORE_H

#include "../transport/core.h"
#include "../discovery/core.h"    /* dart_discovery_addr, dart_discovery_down_reason */

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime hook: 1 if a physical address is on this host (a route probe, on UDP),
 * so the peer is flagged out-of-band (SHM) eligible. NULL => every peer is remote. */
typedef int (*dart_node_is_local_fn)(void *user, const uint8_t *ip, uint8_t ip_len);

/* Everything the core needs from the runtime, set once at init. */
typedef struct {
    dart_state           *transport;    /* the peers are wired into this (sans-IO) */
    uint16_t              max_peers;     /* peer-table capacity */
    uint16_t              n_channels;    /* sizes the announce-blob buffer */
    uint16_t              frag_size;     /* our UDP fragment size, baked into the announce blob */
    dart_event_fn         on_event;      /* PEER_UP/DOWN/REFUSED sink (optional) */
    void                 *user;          /* passed to on_event */
    dart_node_is_local_fn is_local;      /* runtime route probe (optional) */
    void                 *is_local_user;
    int                   oob_capable;   /* 1 = we can deliver out-of-band (SHM) payloads */
    uint8_t               oob_host[16];  /* our host id; a peer is OOB-reachable iff it matches */
} dart_node_core_config;

typedef struct dart_node_core dart_node_core;

size_t          dart_node_core_required_memory(uint16_t max_peers, uint16_t n_channels);
dart_node_core *dart_node_core_init(void *mem, size_t mem_size, const dart_node_core_config *cfg);

/* The discovery announce blob this node sends: its frag size, OOB host, and interest
 * list. The core owns the buffer and builds it (the codec is dart_meta_* in the
 * transport core). build_meta (re)builds it from the core's current fields and returns
 * the length. meta returns the bytes + length for the runtime to feed to discovery.
 * Rebuild after a role change, then re-feed discovery. */
uint16_t        dart_node_core_build_meta(dart_node_core *c);
const uint8_t  *dart_node_core_meta(dart_node_core *c, uint16_t *len);

/* Discovery callbacks: register these with the discovery runtime, user = the core.
 * They keep the peer table and the transport's peer set in lockstep (handling the
 * dormant/resume/evict lifecycle) and fire the app's PEER_UP/DOWN/REFUSED events. */
void dart_node_core_peer_up     (void *user, uint32_t id, const dart_discovery_addr *addr,
                                 const uint8_t *meta, uint16_t meta_len);
void dart_node_core_peer_down   (void *user, uint32_t id, dart_discovery_down_reason reason);
void dart_node_core_peer_refused(void *user, const dart_discovery_addr *addr);

/* A resolved outbound destination: a multicast group (by selector), or a unicast
 * peer (by physical address). The runtime turns this into wire bytes for its link. */
typedef struct {
    uint8_t  is_group;   /* 1 = group send, 0 = unicast peer */
    uint16_t group_sel;  /* group: the selector (low byte of topic identity) */
    uint8_t  ip[16];     /* unicast: peer physical address (IPv4 today) */
    uint8_t  ip_len;
    uint16_t port;       /* unicast: peer data port */
} dart_node_dest;

/* Destination resolution: the node-core/runtime boundary. resolve turns the transport's
 * abstract destination (dart_poll_send's to_peer) into a group or a peer address, so the
 * runtime only maps the result to wire bytes. The group address convention is the
 * runtime's (UDP uses 239.255.<domain>.<sel>). Returns 1 if sendable, 0 if a unicast
 * peer is unknown. id_for_addr maps an inbound source address back to a peer id (1 + *id
 * on a hit, else 0). */
int  dart_node_core_resolve(dart_node_core *c, uint32_t to, dart_node_dest *out);
int  dart_node_core_id_for_addr(dart_node_core *c, const uint8_t ip[4], uint16_t port, uint32_t *id);

/* Read-only peer-table enumeration (diagnostics / tests). max_peers is the capacity;
 * peer_at fills the out-params for table slot in [0, max_peers) and returns 1 if it
 * holds a live peer, else 0. Any out-pointer may be NULL. */
uint16_t dart_node_core_max_peers(dart_node_core *c);
int      dart_node_core_peer_at(dart_node_core *c, uint16_t slot, uint32_t *id,
                                uint8_t ip[16], uint8_t *ip_len, uint16_t *port);

#ifdef __cplusplus
}
#endif
#endif /* DART_NODE_CORE_H */
