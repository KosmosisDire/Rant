/* sans-IO peer-discovery core: no socket, clock, or heap. Feed it datagrams +
 * now_us; it returns datagrams to send and fires peer up/down callbacks. For an
 * IO-owning layer see dart_discovery_rt.h. */
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
} dart_discovery_addr;

/* Why a peer is going down, so the IO layer can keep transport state across a
 * transient blip instead of tearing it down on every silence timeout. */
typedef enum {
    DART_DISCOVERY_DROP = 0,  /* fell silent past peer_timeout_us: same UUID may return, keep state */
    DART_DISCOVERY_GONE = 1   /* said BYE, or its slot was reclaimed for a new peer: free state */
} dart_discovery_down_reason;

/* peer_up: reachable at addr (re-fires when a known peer's addr/meta changes, and
 * when a DROPPED peer returns under the SAME peer_id, so the IO layer can resume).
 * peer_down: going down; reason says whether the state is worth keeping. peer_id is
 * a local handle, stable across a DROP/return, freed only on GONE. meta is the
 * peer's opaque payload (NULL if none), valid only for the call. */
typedef void (*dart_discovery_peer_up_fn)  (void *user, uint32_t peer_id, const dart_discovery_addr *addr,
                                   const uint8_t *meta, uint16_t meta_len);
typedef void (*dart_discovery_peer_down_fn)(void *user, uint32_t peer_id,
                                   dart_discovery_down_reason reason);
/* A new peer arrived but the table is full of ACTIVE peers (none droppable): the
 * peer is refused rather than evicting a live conversation. Diagnostic only. */
typedef void (*dart_discovery_peer_refused_fn)(void *user, const dart_discovery_addr *addr);

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
    dart_discovery_peer_up_fn      on_peer_up;
    dart_discovery_peer_down_fn    on_peer_down;
    dart_discovery_peer_refused_fn on_peer_refused;  /* optional: table full of active peers */
    void *user;
} dart_discovery_config;

typedef struct dart_discovery_state dart_discovery_state;

size_t       dart_discovery_required_memory(const dart_discovery_config *cfg);
dart_discovery_state *dart_discovery_init(void *mem, size_t mem_size, const dart_discovery_config *cfg);
void         dart_discovery_on_datagram(dart_discovery_state *st, const uint8_t *src_ip, uint8_t src_ip_len,
                               const void *datagram, size_t len, uint64_t now_us);
size_t       dart_discovery_update(dart_discovery_state *st, uint64_t now_us, void *out, size_t cap);
size_t       dart_discovery_leave(dart_discovery_state *st, void *out, size_t cap);
/* Queue a one-shot solicit: the next update asks peers to announce now (sent once at startup). */
void         dart_discovery_solicit(dart_discovery_state *st);
/* Replace the opaque meta blob and bump its version, so peers re-fetch it. The
 * blob rides the next few announces, then announces carry the version only; a peer
 * that fell behind re-fetches via a targeted solicit. meta must stay valid. */
void         dart_discovery_set_meta(dart_discovery_state *st, const uint8_t *meta, uint16_t meta_len);
/* Drain one targeted (unicast) datagram and its destination: a solicit REPLY to a
 * peer that solicited us (carries the blob), or a re-fetch REQ to a peer whose
 * advertised version is ahead of what we hold. Returns bytes + fills *to, or 0 when
 * none. Loop like dart_discovery_update; the runtime unicasts each to *to. */
size_t       dart_discovery_poll_targeted(dart_discovery_state *st, void *out, size_t cap,
                             dart_discovery_addr *to);
/* Count of live peers currently known. */
uint16_t     dart_discovery_peer_count(const dart_discovery_state *st);
/* Address of the peer in table slot (0..max_peers-1); 1 + fills *out if it holds a
 * live peer. Lets a runtime reinforce announces over unicast to survive multicast outages. */
int          dart_discovery_peer_addr(const dart_discovery_state *st, uint16_t slot,
                             dart_discovery_addr *out);
/* Deterministic UUID from a stable input (e.g. serial/MAC) + boot seed. RFC 9562 v8. NOT cryptographic. */
void         dart_discovery_make_uuid(uint8_t out[16], const uint8_t *stable, size_t stable_len,
                             uint64_t boot_seed);

#ifdef __cplusplus
}
#endif
#endif /* DART_DISCOVERY_H */
