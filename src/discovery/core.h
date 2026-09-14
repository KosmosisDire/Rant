/* The sans-IO discovery core. Feed it datagrams and the clock, it returns datagrams to
 * send and fires peer events. The rules are in spec/discovery.md. */
#ifndef RANT_DISCOVERY_H
#define RANT_DISCOVERY_H

#include <stddef.h>
#include <stdint.h>
#include "../common/api.h"
#include "../common/string.h"
#include "../common/alloc.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RANT_DISCOVERY_PROTO_VERSION
#define RANT_DISCOVERY_PROTO_VERSION 5
#endif

#define RANT_DISCOVERY_META_MAX 64     /* default per peer overlay capacity */
#define RANT_DISCOVERY_NAME_MAX 32     /* advertised peer name bytes */
/* The fixed header is 24 bytes, then [u32 meta_version][u16 meta_len][meta]. */
#define RANT_DISCOVERY_META_OFF 30
/* Largest discovery section of the blob: port, self ip and name. The overlay follows. */
#define RANT_DISCOVERY_DISC_MAX (2u + 1u + 16u + 1u + RANT_DISCOVERY_NAME_MAX)
/* Smallest datagram buffer. The runtime grows it to fit meta_cap. */
#define RANT_DISCOVERY_WIRE_MAX 128

typedef struct {
    uint8_t  ip[16];   /* network order bytes */
    uint8_t  ip_len;   /* 4 or 16 */
    uint16_t port;     /* data port, host order */
} RantDiscoveryAddr;

/* Which local socket a datagram arrived on. A NAT may rewrite a peer's source per flow,
 * so the core keeps one observed source per channel. See spec/discovery.md. */
typedef enum {
    RANT_DISCOVERY_VIA_DISCOVERY = 0,     /* the shared discovery port */
    RANT_DISCOVERY_VIA_DATA        = 1    /* the port we advertise as our locator */
} RantDiscoveryVia;

#define RANT_DISCOVERY_MAX_SUBNETS 16
/* One IPv4 subnet this host is on, network order bytes. */
typedef struct { uint8_t ip[4], mask[4]; } RantDiscoverySubnet;

/* Why a peer is going down. A DROP keeps transport state for a same uuid return. */
typedef enum {
    RANT_DISCOVERY_DROP = 0,    /* silent past peer_timeout_us, state kept */
    RANT_DISCOVERY_GONE = 1     /* said BYE, silent past the gone timeout, or evicted: free state */
} RantDiscoveryDownReason;

/* Discovery is generic and carries an opaque blob, so it has its own event type. The
 * node translates these into its RantEvent. */
typedef enum {
    RANT_DISCOVERY_PEER_UP,          /* first contact, an address or blob change, or a resume */
    RANT_DISCOVERY_PEER_DOWN,        /* .reason says DROP or GONE */
    RANT_DISCOVERY_PEER_REFUSED,     /* table full of active peers, the newcomer was refused */
    RANT_DISCOVERY_META_TOO_BIG      /* a peer's blob exceeds meta_cap or the receive buffer */
} RantDiscoveryEventKind;

typedef struct {
    RantDiscoveryEventKind     kind;
    void                    *user;       /* RantDiscoveryCoreConfig.user */
    uint32_t                   peer;     /* local peer id, 0 in META_TOO_BIG for an unknown peer */
    RantDiscoveryAddr          addr;     /* UP and REFUSED: the locator. TOO_BIG: locator or source */
    RantDiscoveryDownReason    reason;   /* DOWN only */
    RantString                 name;     /* UP: the advertised name, {NULL,0} if none */
    RantBytes                  meta;     /* UP: the overlay. TOO_BIG: .len is the wanted size */
} RantDiscoveryEvent;
typedef void (*RantDiscoveryEventFn)(const RantDiscoveryEvent *ev);

/* Liveness for the read only peer view. */
typedef enum {
    RANT_PEER_ACTIVE    = 0,   /* heard within peer_timeout_us */
    RANT_PEER_DROPPED = 1       /* silent, state kept, the same uuid may return */
} RantPeerLiveness;

/* The public face of one peer. The pointers are into discovery state, valid until the
 * next poll. The overlay is opaque here and decoded by the transport codec. */
typedef struct {
    uint32_t            id;            /* local handle, stable across a drop and return */
    uint8_t             uuid[16];
    RantDiscoveryAddr addr;            /* advertised unicast locator */
    RantPeerLiveness    liveness;
    uint64_t            last_heard_us;
    RantString          name;          /* {NULL,0} if none */
    RantBytes           meta;          /* the overlay, {NULL,0} if none */
    uint32_t            meta_version;  /* version of the overlay we hold */
    uint32_t            adv_meta_version; /* the highest version it advertises, above ours = stale */
    void             *user;          /* the peer's user scratch, or NULL */
} RantDiscoveryPeer;

typedef struct {
    uint8_t  uuid[16];      /* unique per process instance */
    uint16_t domain_id;
    uint16_t data_port;     /* unicast port we advertise */
    uint8_t  self_ip[16];   /* optional stated IP. len 0 means peers use the source address */
    uint8_t  self_ip_len;   /* 0, 4 or 16 */
    uint32_t announce_interval_us;
    uint32_t peer_timeout_us;    /* drop a peer after this much silence */
    uint32_t gone_timeout_us;    /* DROPPED to GONE this long after the drop, 0 = never */
    uint16_t max_peers;
    RantString name;        /* advertised name, copied at init */
    RantBytes meta;         /* the initial overlay. Must stay valid, .len <= meta_cap */
    uint16_t meta_cap;      /* per peer overlay capacity, 0 = RANT_DISCOVERY_META_MAX */
    uint16_t peer_user_bytes;    /* scratch reserved per peer, 0 = none */
    uint8_t  relay_me;      /* mark our announces relay me, for a node with no multicast */
    RantDiscoveryEventFn on_event;
    void *user;
    /* Optional hook. Per peer blobs are then allocated at their actual length instead of
     * a fixed max_peers x meta_cap pool. Pair with rant_discovery_destroy. */
    RantAllocFn alloc;
    void       *alloc_user;
} RantDiscoveryCoreConfig;

typedef struct RantDiscoveryState RantDiscoveryState;

/* Fills every zero timing and size field with its default. Idempotent. */
RANT_API void           rant_discovery_config_defaults(RantDiscoveryCoreConfig *cfg);

/* Bytes one datagram scratch buffer needs for meta_cap (0 = the default). */
RANT_API uint32_t       rant_discovery_wire_size(uint16_t meta_cap);

RANT_API size_t         rant_discovery_required_memory(const RantDiscoveryCoreConfig *cfg);
RANT_API RantDiscoveryState *rant_discovery_init(void *mem, size_t mem_size, const RantDiscoveryCoreConfig *cfg);
/* Frees the hook allocated peer blobs. A no op without a hook. The arena stays the caller's. */
RANT_API void           rant_discovery_destroy(RantDiscoveryState *st);
/* Relocates a live core into a bigger block, keeping the uuid, versions, ids and peers.
 * self_meta is the announce blob's new address. The caller frees the old block after. */
RANT_API RantDiscoveryState *rant_discovery_core_migrate(RantDiscoveryState *old, void *new_mem,
                 size_t new_cap, uint16_t new_max_peers, uint16_t new_meta_cap,
                 const uint8_t *self_meta, void *peer_cb_user);
/* src NULL or port 0 means the IO layer cannot say, so the locator comes from the blob
 * alone and no observed source binds. via says which local socket it arrived on. */
RANT_API void           rant_discovery_on_datagram(RantDiscoveryState *st, const RantDiscoveryAddr *src,
                                        RantDiscoveryVia via, RantBytes datagram, uint64_t now_us);
RANT_API size_t         rant_discovery_update(RantDiscoveryState *st, uint64_t now_us, void *out, size_t cap);
/* When update next wants to run its timers. 0 means now. Peer timeout sweeps ride the
 * announce cadence. */
RANT_API uint64_t       rant_discovery_next_due_us(const RantDiscoveryState *st);
RANT_API size_t         rant_discovery_leave(RantDiscoveryState *st, void *out, size_t cap);
/* Queues a one shot solicit. The next update asks peers to announce now. */
RANT_API void           rant_discovery_solicit(RantDiscoveryState *st);
/* Re fires peer up for every live peer with the blob we hold, so a caller that changed
 * its own advertised data re applies every peer's interest. */
RANT_API void           rant_discovery_replay_peers(RantDiscoveryState *st);
/* Replaces the overlay and bumps its version so peers re fetch it. meta must stay valid. */
RANT_API void           rant_discovery_set_meta(RantDiscoveryState *st, RantBytes meta);
/* The version our announces advertise, 0 if none. Detail responses are stamped with it. */
RANT_API uint32_t       rant_discovery_meta_version(const RantDiscoveryState *st);
/* Sets the advertised locator port. 0 = none, peers then use the discovery port. */
RANT_API void           rant_discovery_set_data_port(RantDiscoveryState *st, uint16_t port);
/* Our own subnets, for locator ranking. Re callable when the interface set changes. A
 * zero mask is ignored. */
RANT_API void           rant_discovery_set_local_subnets(RantDiscoveryState *st,
                                      const RantDiscoverySubnet *nets, uint8_t n);
/* Drains one unicast datagram: a solicit reply or a re fetch request. Returns bytes and
 * fills *to, or 0. *exact 1 means *to is an observed source, send exactly there. */
RANT_API size_t         rant_discovery_poll_targeted(RantDiscoveryState *st, void *out, size_t cap,
                                      RantDiscoveryAddr *to, int *exact);
/* Drains one proxied announce built on behalf of a relay me peer. The runtime sends it
 * on every path. Loop until 0. */
RANT_API size_t         rant_discovery_poll_relay(RantDiscoveryState *st, void *out, size_t cap);
/* Drains one introduction: a proxied announce of a peer we hear directly, addressed to
 * one relay me peer. *to and *exact as in poll_targeted. Loop until 0. */
RANT_API size_t         rant_discovery_poll_introduce(RantDiscoveryState *st, void *out, size_t cap,
                                      RantDiscoveryAddr *to, int *exact);
/* Live peers, DROPPED entries excluded. */
RANT_API uint16_t       rant_discovery_peer_count(const RantDiscoveryState *st);
/* Table capacity, the slot range for peer_addr and peer_at. */
RANT_API uint16_t       rant_discovery_max_peers(const RantDiscoveryState *st);
/* Discovery TX destination for the ACTIVE peer in a slot. 0 none, 1 the locator, which
 * the runtime expands to both ports, 2 an observed source, send exactly there. */
RANT_API int            rant_discovery_peer_addr(const RantDiscoveryState *st, uint16_t slot,
                                      RantDiscoveryAddr *out);
/* Read only view of the peer in a slot, ACTIVE or DROPPED. 1 if it holds one. */
RANT_API int            rant_discovery_peer_at(const RantDiscoveryState *st, uint16_t slot,
                                      RantDiscoveryPeer *out);

/* By id lookups, DROPPED peers included. The node keys its state on the id and uses
 * these instead of a second peer table. */
RANT_API void          *rant_discovery_peer_user(RantDiscoveryState *st, uint32_t id);
/* The overlay we hold and, through *version, the version it is at. A view until the next poll. */
RANT_API RantBytes        rant_discovery_peer_meta(const RantDiscoveryState *st, uint32_t id,
                                      uint32_t *version);
/* The one address to send data to: the observed data source when bound, else the locator. */
RANT_API int            rant_discovery_addr_of_id(const RantDiscoveryState *st, uint32_t id,
                                      RantDiscoveryAddr *out);
/* {NULL,0} for an unknown peer. */
RANT_API RantString       rant_discovery_peer_name(const RantDiscoveryState *st, uint32_t id);
/* Maps a source back to a peer id. A peer with observed sources matches only those. */
RANT_API int            rant_discovery_id_for_addr(const RantDiscoveryState *st, const uint8_t *ip,
                                      uint8_t ip_len, uint16_t port, uint32_t *id);
/* Deterministic RFC 9562 v8 uuid from a stable input plus a boot seed. Not cryptographic. */
RANT_API void           rant_discovery_make_uuid(uint8_t out[16], RantBytes stable, uint64_t boot_seed);
/* Our own uuid, a view valid for the state's lifetime. */
RANT_API const uint8_t *rant_discovery_uuid(const RantDiscoveryState *st);

#ifdef __cplusplus
}
#endif
#endif /* RANT_DISCOVERY_H */
