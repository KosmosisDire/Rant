/* sans-IO peer-discovery core: no socket, clock, or heap. Feed it datagrams +
 * now_us; it returns datagrams to send and fires peer up/down callbacks. For an
 * IO-owning layer see DartDiscoveryRt.h. */
#ifndef DART_DISCOVERY_H
#define DART_DISCOVERY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DART_DISCOVERY_PROTO_VERSION
#define DART_DISCOVERY_PROTO_VERSION 3     /* v3: versioned meta blob, u16 meta_len */
#endif

#define DART_DISCOVERY_META_MAX 64   /* default per-peer meta capacity (cfg.meta_capacity overrides) */
/* fixed header through self_ip, then [u32 meta_version][u16 meta_len][meta...] */
#define DART_DISCOVERY_META_OFF 49   /* DART_DISCOVERY_HDR_LEN(43) + 4 (version) + 2 (len) */
/* smallest egress/ingress datagram buffer; the runtime grows it to fit meta_capacity */
#define DART_DISCOVERY_WIRE_MAX 128

typedef struct {
    uint8_t  ip[16];   /* network-order bytes */
    uint8_t  ip_len;   /* 4 = IPv4, 16 = IPv6 */
    uint16_t port;     /* data port, host order */
} DartDiscoveryAddr;

/* Why a peer is going down, so the IO layer can keep transport state across a
 * transient blip instead of tearing it down on every silence timeout. */
typedef enum {
    DART_DISCOVERY_DROP = 0,  /* fell silent past peer_timeout_us: same UUID may return, keep state */
    DART_DISCOVERY_GONE = 1   /* said BYE, or its slot was reclaimed for a new peer: free state */
} DartDiscoveryDownReason;

/* peer_up: reachable at addr (re-fires when a known peer's addr/meta changes, and
 * when a DROPPED peer returns under the SAME peer_id, so the IO layer can resume).
 * peer_down: going down; reason says whether the state is worth keeping. peer_id is
 * a local handle, stable across a DROP/return, freed only on GONE. meta is the
 * peer's opaque payload (NULL if none), valid only for the call. */
typedef void (*DartDiscoveryPeerUpFn)  (void *user, uint32_t peer_id, const DartDiscoveryAddr *addr,
                                   const uint8_t *meta, uint16_t meta_len);
typedef void (*DartDiscoveryPeerDownFn)(void *user, uint32_t peer_id,
                                   DartDiscoveryDownReason reason);
/* A new peer arrived but the table is full of ACTIVE peers (none droppable): the
 * peer is refused rather than evicting a live conversation. Diagnostic only. */
typedef void (*DartDiscoveryPeerRefusedFn)(void *user, const DartDiscoveryAddr *addr);

typedef struct {
    uint8_t  uuid[16];      /* unique per process instance (regen each boot) */
    uint16_t domain_id;     /* logical-network selector */
    uint16_t data_port;     /* unicast port we advertise */
    uint8_t  self_ip[16];   /* optional advertised IP; len 0 => use src addr */
    uint8_t  self_ip_len;   /* 0, 4, or 16 */
    uint32_t announce_interval_us;   /* re-announce interval */
    uint32_t peer_timeout_us;    /* drop peer after this much silence */
    uint16_t max_peers;     /* table capacity */
    const uint8_t *meta;    /* opaque versioned blob; the INITIAL value (dart_discovery_set_meta
                               updates it at runtime). Must stay valid. <= meta_capacity */
    uint16_t meta_len;
    uint16_t meta_capacity;      /* per-peer meta buffer capacity; 0 => DART_DISCOVERY_META_MAX */
    DartDiscoveryPeerUpFn      on_peer_up;
    DartDiscoveryPeerDownFn    on_peer_down;
    DartDiscoveryPeerRefusedFn on_peer_refused;  /* optional: table full of active peers */
    void *user;
} DartDiscoveryConfig;

typedef struct DartDiscoveryState DartDiscoveryState;

/* Fill any zero (unset) timing/size field with its default: announce_interval_us
 * (1s), peer_timeout_us (3.5x the interval), max_peers (32). dart_discovery_init
 * REQUIRES these non-zero (it rejects a zero), so an IO layer applies this once before
 * both sizing and init so the two always agree. Idempotent. */
void         dart_discovery_config_defaults(DartDiscoveryConfig *cfg);

/* Bytes an IO layer must allocate for one rx/tx datagram scratch buffer: the fixed
 * header + version + len + meta_capacity (0 => DART_DISCOVERY_META_MAX), floored at
 * DART_DISCOVERY_WIRE_MAX. The core constants that size it live here, so it owns the math. */
uint32_t     dart_discovery_wire_size(uint16_t meta_capacity);

size_t       dart_discovery_required_memory(const DartDiscoveryConfig *cfg);
DartDiscoveryState *dart_discovery_init(void *mem, size_t mem_size, const DartDiscoveryConfig *cfg);
/* Relocate a live core into a bigger block at grown counts, preserving UUID, blob version,
 * local-id counter and the peer table (NOT a re-init). self_meta = the announce blob's new
 * address (the node core moved). Caller frees the old block afterward. Dynamic growth only. */
DartDiscoveryState *dart_discovery_core_migrate(DartDiscoveryState *old, void *new_mem,
        size_t new_cap, uint16_t new_max_peers, uint16_t new_meta_capacity,
        const uint8_t *self_meta, void *peer_cb_user);
void         dart_discovery_on_datagram(DartDiscoveryState *st, const uint8_t *src_ip, uint8_t src_ip_len,
                               const void *datagram, size_t len, uint64_t now_us);
size_t       dart_discovery_update(DartDiscoveryState *st, uint64_t now_us, void *out, size_t cap);
size_t       dart_discovery_leave(DartDiscoveryState *st, void *out, size_t cap);
/* Queue a one-shot solicit: the next update asks peers to announce now (sent once at startup). */
void         dart_discovery_solicit(DartDiscoveryState *st);
/* Re-fire on_peer_up for every live (non-dropped) peer with the meta blob we already
 * hold, without any version change. A caller that just changed its OWN advertised data
 * (e.g. added a local channel / changed a role) uses this to re-apply every peer's
 * interest, so the new local state matches interest the peers advertised earlier --
 * which the peer would otherwise only re-send on its own next change. */
void         dart_discovery_replay_peers(DartDiscoveryState *st);
/* Replace the opaque meta blob and bump its version, so peers re-fetch it. The
 * blob rides the next few announces, then announces carry the version only; a peer
 * that fell behind re-fetches via a targeted solicit. meta must stay valid. */
void         dart_discovery_set_meta(DartDiscoveryState *st, const uint8_t *meta, uint16_t meta_len);
/* Set the unicast locator port advertised in announces (the header data_port). The IO
 * runtime calls this when it binds its own same-host unicast RX socket, so peers reply
 * to a port unique to THIS process instead of the shared discovery port (which the OS
 * hands to one arbitrary same-port socket). 0 = none (peers fall back to the disc port). */
void         dart_discovery_set_data_port(DartDiscoveryState *st, uint16_t port);
/* Drain one targeted (unicast) datagram and its destination: a solicit REPLY to a
 * peer that solicited us (carries the blob), or a re-fetch REQ to a peer whose
 * advertised version is ahead of what we hold. Returns bytes + fills *to, or 0 when
 * none. Loop like dart_discovery_update; the runtime unicasts each to *to. */
size_t       dart_discovery_poll_targeted(DartDiscoveryState *st, void *out, size_t cap,
                             DartDiscoveryAddr *to);
/* Count of live peers currently known. */
uint16_t     dart_discovery_peer_count(const DartDiscoveryState *st);
/* Address of the peer in table slot (0..max_peers-1); 1 + fills *out if it holds a
 * live peer. Lets a runtime reinforce announces over unicast to survive multicast outages. */
int          dart_discovery_peer_addr(const DartDiscoveryState *st, uint16_t slot,
                             DartDiscoveryAddr *out);
/* Deterministic UUID from a stable input (e.g. serial/MAC) + boot seed. RFC 9562 v8. NOT cryptographic. */
void         dart_discovery_make_uuid(uint8_t out[16], const uint8_t *stable, size_t stable_len,
                             uint64_t boot_seed);

#ifdef __cplusplus
}
#endif
#endif /* DART_DISCOVERY_H */
