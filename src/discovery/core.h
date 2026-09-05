/* The sans-IO discovery core. Feed it datagrams and the clock, it returns datagrams to
 * send and fires peer events. The rules are in spec/discovery.md. */
#ifndef DART_DISCOVERY_H
#define DART_DISCOVERY_H

#include <stddef.h>
#include <stdint.h>
#include "../common/string.h"
#include "../common/alloc.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DART_DISCOVERY_PROTO_VERSION
#define DART_DISCOVERY_PROTO_VERSION 5
#endif

#define DART_DISCOVERY_META_MAX 64   /* default per peer overlay capacity */
#define DART_DISCOVERY_NAME_MAX 32   /* advertised peer name bytes */
/* The fixed header is 24 bytes, then [u32 meta_version][u16 meta_len][meta]. */
#define DART_DISCOVERY_META_OFF 30
/* Largest discovery section of the blob: port, self ip and name. The overlay follows. */
#define DART_DISCOVERY_DISC_MAX (2u + 1u + 16u + 1u + DART_DISCOVERY_NAME_MAX)
/* Smallest datagram buffer. The runtime grows it to fit meta_cap. */
#define DART_DISCOVERY_WIRE_MAX 128

typedef struct {
    uint8_t  ip[16];   /* network order bytes */
    uint8_t  ip_len;   /* 4 or 16 */
    uint16_t port;     /* data port, host order */
} DartDiscoveryAddr;

/* Which local socket a datagram arrived on. A NAT may rewrite a peer's source per flow,
 * so the core keeps one observed source per channel. See spec/discovery.md. */
typedef enum {
    DART_DISCOVERY_VIA_DISCOVERY = 0,   /* the shared discovery port */
    DART_DISCOVERY_VIA_DATA      = 1    /* the port we advertise as our locator */
} DartDiscoveryVia;

#define DART_DISCOVERY_MAX_SUBNETS 16
/* One IPv4 subnet this host is on, network order bytes. */
typedef struct { uint8_t ip[4], mask[4]; } DartDiscoverySubnet;

/* Why a peer is going down. A DROP keeps transport state for a same uuid return. */
typedef enum {
    DART_DISCOVERY_DROP = 0,  /* silent past peer_timeout_us, state kept */
    DART_DISCOVERY_GONE = 1   /* said BYE, silent past the gone timeout, or evicted: free state */
} DartDiscoveryDownReason;

/* Discovery is generic and carries an opaque blob, so it has its own event type. The
 * node translates these into its DartEvent. */
typedef enum {
    DART_DISCOVERY_PEER_UP,        /* first contact, an address or blob change, or a resume */
    DART_DISCOVERY_PEER_DOWN,      /* .reason says DROP or GONE */
    DART_DISCOVERY_PEER_REFUSED,   /* table full of active peers, the newcomer was refused */
    DART_DISCOVERY_META_TOO_BIG    /* a peer's blob exceeds meta_cap or the receive buffer */
} DartDiscoveryEventKind;

typedef struct {
    DartDiscoveryEventKind   kind;
    void                    *user;     /* DartDiscoveryCoreConfig.user */
    uint32_t                 peer;     /* local peer id, 0 in META_TOO_BIG for an unknown peer */
    DartDiscoveryAddr        addr;     /* UP and REFUSED: the locator. TOO_BIG: locator or source */
    DartDiscoveryDownReason  reason;   /* DOWN only */
    DartString               name;     /* UP: the advertised name, {NULL,0} if none */
    DartBytes                meta;     /* UP: the overlay. TOO_BIG: .len is the wanted size */
} DartDiscoveryEvent;
typedef void (*DartDiscoveryEventFn)(const DartDiscoveryEvent *ev);

/* Liveness for the read only peer view. */
typedef enum {
    DART_PEER_ACTIVE  = 0,   /* heard within peer_timeout_us */
    DART_PEER_DROPPED = 1     /* silent, state kept, the same uuid may return */
} DartPeerLiveness;

/* The public face of one peer. The pointers are into discovery state, valid until the
 * next poll. The overlay is opaque here and decoded by the transport codec. */
typedef struct {
    uint32_t          id;            /* local handle, stable across a drop and return */
    uint8_t           uuid[16];
    DartDiscoveryAddr addr;          /* advertised unicast locator */
    DartPeerLiveness  liveness;
    uint64_t          last_heard_us;
    DartString        name;          /* {NULL,0} if none */
    DartBytes         meta;          /* the overlay, {NULL,0} if none */
    uint32_t          meta_version;  /* version of the overlay we hold */
    uint32_t          adv_meta_version; /* the highest version it advertises, above ours = stale */
    void             *user;          /* the peer's user scratch, or NULL */
} DartDiscoveryPeer;

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
    DartString name;        /* advertised name, copied at init */
    DartBytes meta;         /* the initial overlay. Must stay valid, .len <= meta_cap */
    uint16_t meta_cap;      /* per peer overlay capacity, 0 = DART_DISCOVERY_META_MAX */
    uint16_t peer_user_bytes;    /* scratch reserved per peer, 0 = none */
    uint8_t  relay_me;      /* mark our announces relay me, for a node with no multicast */
    DartDiscoveryEventFn on_event;
    void *user;
    /* Optional hook. Per peer blobs are then allocated at their actual length instead of
     * a fixed max_peers x meta_cap pool. Pair with dart_discovery_destroy. */
    DartAllocFn alloc;
    void       *alloc_user;
} DartDiscoveryCoreConfig;

typedef struct DartDiscoveryState DartDiscoveryState;

/* Fills every zero timing and size field with its default. Idempotent. */
void         dart_discovery_config_defaults(DartDiscoveryCoreConfig *cfg);

/* Bytes one datagram scratch buffer needs for meta_cap (0 = the default). */
uint32_t     dart_discovery_wire_size(uint16_t meta_cap);

size_t       dart_discovery_required_memory(const DartDiscoveryCoreConfig *cfg);
DartDiscoveryState *dart_discovery_init(void *mem, size_t mem_size, const DartDiscoveryCoreConfig *cfg);
/* Frees the hook allocated peer blobs. A no op without a hook. The arena stays the caller's. */
void         dart_discovery_destroy(DartDiscoveryState *st);
/* Relocates a live core into a bigger block, keeping the uuid, versions, ids and peers.
 * self_meta is the announce blob's new address. The caller frees the old block after. */
DartDiscoveryState *dart_discovery_core_migrate(DartDiscoveryState *old, void *new_mem,
        size_t new_cap, uint16_t new_max_peers, uint16_t new_meta_cap,
        const uint8_t *self_meta, void *peer_cb_user);
/* src NULL or port 0 means the IO layer cannot say, so the locator comes from the blob
 * alone and no observed source binds. via says which local socket it arrived on. */
void         dart_discovery_on_datagram(DartDiscoveryState *st, const DartDiscoveryAddr *src,
                               DartDiscoveryVia via, DartBytes datagram, uint64_t now_us);
size_t       dart_discovery_update(DartDiscoveryState *st, uint64_t now_us, void *out, size_t cap);
/* When update next wants to run its timers. 0 means now. Peer timeout sweeps ride the
 * announce cadence. */
uint64_t     dart_discovery_next_due_us(const DartDiscoveryState *st);
size_t       dart_discovery_leave(DartDiscoveryState *st, void *out, size_t cap);
/* Queues a one shot solicit. The next update asks peers to announce now. */
void         dart_discovery_solicit(DartDiscoveryState *st);
/* Re fires peer up for every live peer with the blob we hold, so a caller that changed
 * its own advertised data re applies every peer's interest. */
void         dart_discovery_replay_peers(DartDiscoveryState *st);
/* Replaces the overlay and bumps its version so peers re fetch it. meta must stay valid. */
void         dart_discovery_set_meta(DartDiscoveryState *st, DartBytes meta);
/* The version our announces advertise, 0 if none. Detail responses are stamped with it. */
uint32_t     dart_discovery_meta_version(const DartDiscoveryState *st);
/* Sets the advertised locator port. 0 = none, peers then use the discovery port. */
void         dart_discovery_set_data_port(DartDiscoveryState *st, uint16_t port);
/* Our own subnets, for locator ranking. Re callable when the interface set changes. A
 * zero mask is ignored. */
void         dart_discovery_set_local_subnets(DartDiscoveryState *st,
                             const DartDiscoverySubnet *nets, uint8_t n);
/* Drains one unicast datagram: a solicit reply or a re fetch request. Returns bytes and
 * fills *to, or 0. *exact 1 means *to is an observed source, send exactly there. */
size_t       dart_discovery_poll_targeted(DartDiscoveryState *st, void *out, size_t cap,
                             DartDiscoveryAddr *to, int *exact);
/* Drains one proxied announce built on behalf of a relay me peer. The runtime sends it
 * on every path. Loop until 0. */
size_t       dart_discovery_poll_relay(DartDiscoveryState *st, void *out, size_t cap);
/* Drains one introduction: a proxied announce of a peer we hear directly, addressed to
 * one relay me peer. *to and *exact as in poll_targeted. Loop until 0. */
size_t       dart_discovery_poll_introduce(DartDiscoveryState *st, void *out, size_t cap,
                             DartDiscoveryAddr *to, int *exact);
/* Live peers, DROPPED entries excluded. */
uint16_t     dart_discovery_peer_count(const DartDiscoveryState *st);
/* Table capacity, the slot range for peer_addr and peer_at. */
uint16_t     dart_discovery_max_peers(const DartDiscoveryState *st);
/* Discovery TX destination for the ACTIVE peer in a slot. 0 none, 1 the locator, which
 * the runtime expands to both ports, 2 an observed source, send exactly there. */
int          dart_discovery_peer_addr(const DartDiscoveryState *st, uint16_t slot,
                             DartDiscoveryAddr *out);
/* Read only view of the peer in a slot, ACTIVE or DROPPED. 1 if it holds one. */
int          dart_discovery_peer_at(const DartDiscoveryState *st, uint16_t slot,
                             DartDiscoveryPeer *out);

/* By id lookups, DROPPED peers included. The node keys its state on the id and uses
 * these instead of a second peer table. */
void        *dart_discovery_peer_user(DartDiscoveryState *st, uint32_t id);
/* The overlay we hold and, through *version, the version it is at. A view until the next poll. */
DartBytes    dart_discovery_peer_meta(const DartDiscoveryState *st, uint32_t id,
                             uint32_t *version);
/* The one address to send data to: the observed data source when bound, else the locator. */
int          dart_discovery_addr_of_id(const DartDiscoveryState *st, uint32_t id,
                             DartDiscoveryAddr *out);
/* {NULL,0} for an unknown peer. */
DartString   dart_discovery_peer_name(const DartDiscoveryState *st, uint32_t id);
/* Maps a source back to a peer id. A peer with observed sources matches only those. */
int          dart_discovery_id_for_addr(const DartDiscoveryState *st, const uint8_t *ip,
                             uint8_t ip_len, uint16_t port, uint32_t *id);
/* Deterministic RFC 9562 v8 uuid from a stable input plus a boot seed. Not cryptographic. */
void         dart_discovery_make_uuid(uint8_t out[16], DartBytes stable, uint64_t boot_seed);
/* Our own uuid, a view valid for the state's lifetime. */
const uint8_t *dart_discovery_uuid(const DartDiscoveryState *st);

#ifdef __cplusplus
}
#endif
#endif /* DART_DISCOVERY_H */
