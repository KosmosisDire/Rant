/* sans-IO peer-discovery core: no socket, clock, or heap. Feed it datagrams +
 * now_us; it returns datagrams to send and fires peer up/down callbacks. For an
 * IO-owning layer see discovery/runtime.h (DartDiscovery). */
#ifndef DART_DISCOVERY_H
#define DART_DISCOVERY_H

#include <stddef.h>
#include <stdint.h>
#include "../common/string.h"   /* DartBytes (the opaque meta/overlay blob, datagrams) */
#include "../common/alloc.h"    /* DartAllocFn (the optional per-peer blob hook) */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DART_DISCOVERY_PROTO_VERSION
#define DART_DISCOVERY_PROTO_VERSION 3     /* v3: versioned meta blob, u16 meta_len */
#endif

#define DART_DISCOVERY_META_MAX 64   /* default per-peer OVERLAY capacity (cfg.meta_cap overrides) */
#define DART_DISCOVERY_NAME_MAX 32   /* max advertised peer-name bytes (blob's discovery section) */
/* Fixed header (magic, ver, flags, domain, uuid) is sent EVERY announce, then
 * [u32 meta_version][u16 meta_len][meta...]. The meta blob = [discovery section: locator +
 * name][opaque overlay]: the locator + name moved out of the per-announce header into the
 * on-change blob so steady-state announces stay small (cached on the other side). */
#define DART_DISCOVERY_META_OFF 30   /* HDR_LEN(24) + 4 (version) + 2 (len) */
/* largest blob discovery section: [u16 data_port][u8 self_ip_len][self_ip..][u8 name_len]
 * [name..]. Public so an overlay producer can budget its announce against one datagram
 * (META_OFF + this + overlay). */
#define DART_DISCOVERY_DISC_MAX (2u + 1u + 16u + 1u + DART_DISCOVERY_NAME_MAX)
/* smallest egress/ingress datagram buffer; the runtime grows it to fit meta_cap */
#define DART_DISCOVERY_WIRE_MAX 128

typedef struct {
    uint8_t  ip[16];   /* network-order bytes */
    uint8_t  ip_len;   /* 4 = IPv4, 16 = IPv6 */
    uint16_t port;     /* data port, host order */
} DartDiscoveryAddr;

#define DART_DISCOVERY_MAX_SUBNETS 16   /* local IPv4 subnets the locator ranking considers */
/* One IPv4 subnet this host is directly on: address + netmask, network-order bytes.
 * See dart_discovery_set_local_subnets. */
typedef struct { uint8_t ip[4], mask[4]; } DartDiscoverySubnet;

/* Why a peer is going down, so the IO layer can keep transport state across a
 * transient blip instead of tearing it down on every silence timeout. */
typedef enum {
    DART_DISCOVERY_DROP = 0,  /* fell silent past peer_timeout_us: same UUID may return, keep state */
    DART_DISCOVERY_GONE = 1   /* said BYE, stayed silent past the gone timeout, or its slot was
                                 reclaimed for a new peer: free state */
} DartDiscoveryDownReason;

/* Discovery's own event, delivered through one on_event. Discovery is generic: it
 * carries an opaque meta blob and knows nothing of the overlay (transport/node), so it
 * has its own event type rather than sharing one. The node translates these into its
 * app-facing DartEvent.
 *   DART_DISCOVERY_PEER_UP      reachable at .addr; .name is the advertised peer name and
 *                               .meta/.meta_len the opaque overlay (NULL if none, valid only
 *                               for the call). Re-fires on a known peer's addr/meta change and
 *                               when a DROPPED peer returns under the SAME .peer (resume).
 *   DART_DISCOVERY_PEER_DOWN    going down; .reason (DROP keep / GONE freed). .peer is a
 *                               local handle, stable across a DROP/return, freed on GONE.
 *   DART_DISCOVERY_PEER_REFUSED table full of ACTIVE peers: a new peer at .addr was
 *                               refused rather than evicting a live one. Diagnostic. */
typedef enum {
    DART_DISCOVERY_PEER_UP,
    DART_DISCOVERY_PEER_DOWN,
    DART_DISCOVERY_PEER_REFUSED,
    DART_DISCOVERY_META_TOO_BIG   /* a peer's announce blob exceeds what this side can hold or even
                                     receive, so the whole announce was dropped: .addr = its locator
                                     (or the datagram source), .peer = its id (0 if not yet in the
                                     table), .meta.len = the bytes it wanted to send (.meta.data is
                                     the oversized overlay when the datagram arrived whole, NULL when
                                     the OS truncated it to our RX buffer). An IO layer that can grow
                                     raises its capacity and re-solicits; otherwise raise capacity. */
} DartDiscoveryEventKind;

typedef struct {
    DartDiscoveryEventKind   kind;
    void                    *user;     /* DartDiscoveryCoreConfig.user */
    uint32_t                 peer;     /* local peer id (UP / DOWN) */
    DartDiscoveryAddr        addr;     /* UP / REFUSED: advertised locator */
    DartDiscoveryDownReason  reason;   /* DOWN: DROP vs GONE */
    DartString               name;     /* UP: advertised peer name (not NUL-terminated; {NULL,0} if none) */
    DartBytes                meta;     /* UP: opaque overlay blob ({NULL,0} if none) */
} DartDiscoveryEvent;
typedef void (*DartDiscoveryEventFn)(const DartDiscoveryEvent *ev);

/* A peer's liveness, for the read-only peer view (dart_discovery_peer_at). */
typedef enum {
    DART_PEER_ACTIVE  = 0,   /* heard within peer_timeout_us */
    DART_PEER_DROPPED = 1     /* fell silent; state kept, the same UUID may return */
} DartPeerLiveness;

/* The public face of one discovered peer, filled from the internal table by
 * dart_discovery_peer_at. Discovery is generic, so this carries only what discovery
 * itself knows: identity, locator, liveness, name, and the OPAQUE overlay blob (the
 * higher layer's data). The overlay's transport meaning (frag size, interest topics)
 * is decoded by the consumer via the transport codec (dart_meta_frag /
 * dart_meta_interest_next), never by discovery. The pointers are into discovery state,
 * valid until the next poll mutates the table. */
typedef struct {
    uint32_t          id;            /* local handle, stable across a drop/return */
    uint8_t           uuid[16];      /* the peer's real GUID */
    DartDiscoveryAddr addr;          /* advertised unicast locator */
    DartPeerLiveness  liveness;      /* ACTIVE, or DROPPED (silent, may return) */
    uint64_t          last_heard_us; /* timestamp of its last announce (caller derives age) */
    DartString        name;          /* advertised name (not NUL-terminated; {NULL,0} if none) */
    DartBytes         meta;          /* opaque overlay blob ({NULL,0} if none) */
    uint32_t          meta_version;  /* version of the overlay we hold */
    uint32_t          adv_meta_version; /* highest version the peer advertises (> meta_version => held blob is stale) */
    void             *user;          /* this peer's user scratch (cfg.peer_user_bytes), or NULL */
} DartDiscoveryPeer;

typedef struct {
    uint8_t  uuid[16];      /* unique per process instance (regen each boot) */
    uint16_t domain_id;     /* logical-network selector */
    uint16_t data_port;     /* unicast port we advertise */
    uint8_t  self_ip[16];   /* optional advertised IP; len 0 => use src addr */
    uint8_t  self_ip_len;   /* 0, 4, or 16 */
    uint32_t announce_interval_us;   /* re-announce interval */
    uint32_t peer_timeout_us;    /* drop peer after this much silence */
    uint32_t gone_timeout_us;    /* promote a DROPPED (silent) peer to GONE this long after it dropped:
                                    its transport state is freed and the slot reclaimed, so a long-dead
                                    peer can't hold a slot indefinitely. 0 = never promote (linger for
                                    resume until the slot is needed for a new peer). */
    uint16_t max_peers;     /* table capacity */
    DartString name;        /* advertised peer name (goes in the blob's discovery section);
                               {NULL,0} = none. Copied at init, so it need not outlive the call. */
    DartBytes meta;         /* opaque OVERLAY blob (the higher layer's data, e.g. transport
                               frag/interest); discovery carries it after its own section. The
                               INITIAL value (dart_discovery_set_meta updates it). Must stay
                               valid. .len <= meta_cap */
    uint16_t meta_cap;      /* per-peer OVERLAY buffer capacity; 0 => DART_DISCOVERY_META_MAX */
    uint16_t peer_user_bytes;    /* opaque scratch reserved per peer (0 = none); see dart_discovery_peer_user.
                                    Zeroed when a new UUID takes a slot, preserved across a drop -> resume. */
    DartDiscoveryEventFn on_event;   /* optional: PEER_UP / PEER_DOWN / PEER_REFUSED */
    void *user;
    /* Optional allocation hook. When set, per-peer overlay blobs are allocated on demand
     * at each blob's ACTUAL length (grown per peer as bigger blobs arrive) instead of a
     * fixed max_peers x meta_cap arena pool -- typically a >90% cut, since real blobs
     * are far smaller than the worst-case capacity. meta_cap stays the accept bound
     * (META_TOO_BIG above it) either way. NULL (embedded default) keeps the fixed pool.
     * Pair with dart_discovery_destroy unless the hook's owner reclaims everything itself
     * (the node's allocator reset does). */
    DartAllocFn alloc;
    void       *alloc_user;
} DartDiscoveryCoreConfig;

typedef struct DartDiscoveryState DartDiscoveryState;

/* Fill any zero (unset) timing/size field with its default: announce_interval_us
 * (1s), peer_timeout_us (3.5x the interval), gone_timeout_us (60s), max_peers (32).
 * dart_discovery_init REQUIRES announce_interval_us + peer_timeout_us non-zero (it rejects
 * a zero); gone_timeout_us may stay 0 (never promote a dropped peer). An IO layer applies
 * this once before both sizing and init so the two always agree. Idempotent. */
void         dart_discovery_config_defaults(DartDiscoveryCoreConfig *cfg);

/* Bytes an IO layer must allocate for one rx/tx datagram scratch buffer: the fixed
 * header + version + len + meta_cap (0 => DART_DISCOVERY_META_MAX), floored at
 * DART_DISCOVERY_WIRE_MAX. The core constants that size it live here, so it owns the math. */
uint32_t     dart_discovery_wire_size(uint16_t meta_cap);

size_t       dart_discovery_required_memory(const DartDiscoveryCoreConfig *cfg);
DartDiscoveryState *dart_discovery_init(void *mem, size_t mem_size, const DartDiscoveryCoreConfig *cfg);
/* Free the hook-allocated per-peer blobs (cfg.alloc mode only; no-op otherwise). The
 * arena stays the caller's. Skippable when the hook's owner reclaims wholesale (the
 * node resets its allocator instead). */
void         dart_discovery_destroy(DartDiscoveryState *st);
/* Relocate a live core into a bigger block at grown counts, preserving UUID, blob version,
 * local-id counter and the peer table (NOT a re-init). self_meta = the announce blob's new
 * address (the node core moved). Caller frees the old block afterward. Dynamic growth only. */
DartDiscoveryState *dart_discovery_core_migrate(DartDiscoveryState *old, void *new_mem,
        size_t new_cap, uint16_t new_max_peers, uint16_t new_meta_cap,
        const uint8_t *self_meta, void *peer_cb_user);
void         dart_discovery_on_datagram(DartDiscoveryState *st, const uint8_t *src_ip, uint8_t src_ip_len,
                               DartBytes datagram, uint64_t now_us);
size_t       dart_discovery_update(DartDiscoveryState *st, uint64_t now_us, void *out, size_t cap);
/* The next monotonic time dart_discovery_update wants to run its timers (announce due;
 * 0 = immediately, e.g. a pending solicit or a just-advertised blob). Lets a driving
 * loop sleep exactly until due instead of ticking. Peer-timeout sweeps ride the same
 * cadence, so detection lags by at most one announce interval (well inside the
 * default peer timeout). */
uint64_t     dart_discovery_next_due_us(const DartDiscoveryState *st);
size_t       dart_discovery_leave(DartDiscoveryState *st, void *out, size_t cap);
/* Queue a one-shot solicit: the next update asks peers to announce now (sent once at startup). */
void         dart_discovery_solicit(DartDiscoveryState *st);
/* Re-fire on_peer_up for every live (non-dropped) peer with the meta blob we already
 * hold, without any version change. A caller that just changed its OWN advertised data
 * (e.g. added a local topic / changed a role) uses this to re-apply every peer's
 * interest, so the new local state matches interest the peers advertised earlier --
 * which the peer would otherwise only re-send on its own next change. */
void         dart_discovery_replay_peers(DartDiscoveryState *st);
/* Replace the opaque meta blob and bump its version, so peers re-fetch it. The
 * blob rides the next few announces, then announces carry the version only; a peer
 * that fell behind re-fetches via a targeted solicit. meta must stay valid. */
void         dart_discovery_set_meta(DartDiscoveryState *st, DartBytes meta);
/* The version our announces currently advertise (bumped by each set_meta; 0 = none
 * set). A consumer stamps data derived from the blob with it (e.g. detail responses). */
uint32_t     dart_discovery_meta_version(const DartDiscoveryState *st);
/* Set the unicast locator port advertised in announces (the header data_port). The IO
 * runtime calls this when it binds its own same-host unicast RX socket, so peers reply
 * to a port unique to THIS process instead of the shared discovery port (which the OS
 * hands to one arbitrary same-port socket). 0 = none (peers fall back to the disc port). */
void         dart_discovery_set_data_port(DartDiscoveryState *st, uint16_t port);
/* Tell the core which IPv4 subnets this host sits on directly (the IO layer's interface
 * list; n = 0 means unknown, which is fine). An announce carries no address of its own, so
 * a peer's locator is learned from the datagram source: a multihomed peer announcing out
 * every interface is then heard at a DIFFERENT address per path, and taking the newest each
 * time would flap the locator its data is unicast to. With this the core ranks candidates
 * (on one of our subnets > ordinary routed > 169.254 link-local) and, at equal rank, stays
 * with the address it is already hearing the peer at until that one goes quiet. Re-callable
 * whenever the interface set changes; entries with a zero mask are ignored. */
void         dart_discovery_set_local_subnets(DartDiscoveryState *st,
                             const DartDiscoverySubnet *nets, uint8_t n);
/* Drain one targeted (unicast) datagram and its destination: a solicit REPLY to a
 * peer that solicited us (carries the blob), or a re-fetch REQ to a peer whose
 * advertised version is ahead of what we hold. Returns bytes + fills *to, or 0 when
 * none. Loop like dart_discovery_update; the runtime unicasts each to *to. */
size_t       dart_discovery_poll_targeted(DartDiscoveryState *st, void *out, size_t cap,
                             DartDiscoveryAddr *to);
/* Count of live peers currently known. */
uint16_t     dart_discovery_peer_count(const DartDiscoveryState *st);
/* Table capacity (the slot range for dart_discovery_peer_addr / dart_discovery_peer_at). */
uint16_t     dart_discovery_max_peers(const DartDiscoveryState *st);
/* Address of the peer in table slot (0..max_peers-1); 1 + fills *out only if it holds an
 * ACTIVE peer (not a dropped/silent one). Lets a runtime reinforce announces over unicast to
 * survive multicast outages, without bouncing them off peers that have gone away. */
int          dart_discovery_peer_addr(const DartDiscoveryState *st, uint16_t slot,
                             DartDiscoveryAddr *out);
/* Read-only peer view: fill *out for the peer in table slot (0..max_peers-1) and return
 * 1 if it holds one (ACTIVE or DROPPED), else 0. The filled name/meta pointers point into
 * discovery state, valid until the next poll. Scan slot 0..dart_discovery_max_peers()-1 to
 * enumerate; the overlay is opaque (decode it with the transport codec). */
int          dart_discovery_peer_at(const DartDiscoveryState *st, uint16_t slot,
                             DartDiscoveryPeer *out);

/* By-id lookups (a higher layer keys its state on the peer id). Each scans the table
 * for the peer whose local id == id, including DROPPED peers (a dropped peer keeps its
 * slot for a same-UUID return, so it still resolves). The peer table is discovery's; a
 * consumer (e.g. the node core) uses these instead of duplicating it.
 *   peer_user  -> pointer to the peer's opaque scratch (cfg.peer_user_bytes), or NULL.
 *   addr_of_id -> 1 + fills *out with the advertised locator, else 0.
 *   peer_name  -> advertised name as a DartString (into discovery state; {NULL,0} if unknown).
 *   peer_meta  -> the opaque overlay blob we hold for the peer ({NULL,0} if none) and, via
 *                 *version (optional), the version it is at. A view into discovery state,
 *                 valid until the next poll.
 *   id_for_addr-> reverse map an (ip, port) back to a peer id: 1 + *id on a hit, else 0. */
void        *dart_discovery_peer_user(DartDiscoveryState *st, uint32_t id);
DartBytes    dart_discovery_peer_meta(const DartDiscoveryState *st, uint32_t id,
                             uint32_t *version);
int          dart_discovery_addr_of_id(const DartDiscoveryState *st, uint32_t id,
                             DartDiscoveryAddr *out);
DartString   dart_discovery_peer_name(const DartDiscoveryState *st, uint32_t id);
int          dart_discovery_id_for_addr(const DartDiscoveryState *st, const uint8_t *ip,
                             uint8_t ip_len, uint16_t port, uint32_t *id);
/* Deterministic UUID from a stable input (e.g. serial/MAC) + boot seed. RFC 9562 v8. NOT cryptographic. */
void         dart_discovery_make_uuid(uint8_t out[16], DartBytes stable, uint64_t boot_seed);

#ifdef __cplusplus
}
#endif
#endif /* DART_DISCOVERY_H */
