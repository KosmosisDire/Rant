/* The sans-IO discovery core. Feed it datagrams and the clock, it returns datagrams to
 * send and fires peer events. The rules are in spec/discovery.md. */
#ifndef RANT_DISCOVERY_H
#define RANT_DISCOVERY_H

#include <stddef.h>
#include <stdint.h>
#include "../common/types.h"
#include "../common/string.h"
#include "../common/alloc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What every discovery datagram carries in byte 4. Any change to what crosses the wire bumps
   it, discovery or not, so two nodes either speak one wire or one reports the other. */
#ifndef RANT_WIRE_VERSION
#define RANT_WIRE_VERSION 8
#endif

#define RANT_DISCOVERY_META_MAX 64     /* default per peer overlay capacity */
/* Smallest datagram buffer. The runtime grows it to fit meta_cap. */
#define RANT_DISCOVERY_WIRE_MAX 128

/* One discovery datagram. The layout is in spec/discovery.md, Wire format: the fixed part
 * runs through name_len, the name follows, then the overlay when BLOB is set. */
#define RANT_DISCOVERY_FIXED_LEN 37
#define RANT_DISCOVERY_HEAD_MAX  (RANT_DISCOVERY_FIXED_LEN + RANT_NODE_NAME_MAX)
#define RANT_DISCOVERY_FLAG_BYE  0x01u   /* leaving */
#define RANT_DISCOVERY_FLAG_REQ  0x02u   /* a solicit: answer with an announce and the blob */
#define RANT_DISCOVERY_FLAG_UNI  0x04u   /* a copy by unicast: copy your announces back */
#define RANT_DISCOVERY_FLAG_ALL  0x08u   /* an observer's solicit: every peer answers at once */
#define RANT_DISCOVERY_FLAG_BLOB 0x10u   /* the overlay follows the name, maybe empty */
#define RANT_DISCOVERY_FLAG_ONE  0x20u   /* a targeted send: a REQ is answered at once, no copies */

typedef struct {
    uint8_t        flags;
    uint16_t       domain;
    const uint8_t *uuid;            /* 16 bytes */
    uint32_t       version;         /* the sender's blob version */
    uint32_t       changed_ago_ms;  /* how long ago that blob last changed, saturating */
    uint16_t       data_port;
    RantString     name;            /* not NUL terminated */
    RantBytes      overlay;         /* the blob, when flags has BLOB */
    uint16_t       overlay_len;     /* the length the sender stated, past overlay.len when cut */
    uint8_t        wire;            /* the wire version byte, for I_RANT_DISC_WIRE */
} i_RantDiscoveryWire;

typedef enum {
    I_RANT_DISC_OK,
    I_RANT_DISC_FOREIGN,     /* not a discovery datagram */
    I_RANT_DISC_WIRE,        /* another wire version: flags, domain and wire hold */
    I_RANT_DISC_MALFORMED,
    I_RANT_DISC_CUT          /* the overlay arrived shorter than stated: all but overlay hold */
} i_RantDiscoveryDecode;

/* Writes w into out. overlay rides only with BLOB, a name past RANT_NODE_NAME_MAX is cut.
 * 0 when it does not fit. */
size_t i_rant_discovery_encode(const i_RantDiscoveryWire *w, uint8_t *out, size_t cap);
/* Views of the datagram's bytes, valid while the datagram is. */
i_RantDiscoveryDecode i_rant_discovery_decode(RantBytes datagram, i_RantDiscoveryWire *out);

#define RANT_DISCOVERY_MAX_SUBNETS 16
/* One IPv4 subnet this host is on, network order bytes. */
typedef struct { uint8_t ip[4], mask[4]; } i_RantDiscoverySubnet;

/* Why a peer leaves its slot. A DROP keeps transport state for a same uuid return. */
typedef enum {
    RANT_DISCOVERY_DROP = 0,    /* silent past peer_timeout_us, state kept */
    RANT_DISCOVERY_GONE = 1,    /* said BYE, silent past the gone timeout, or evicted: free state */
    RANT_DISCOVERY_GIVE_BACK = 2   /* still here but holds no slot now: free state, the record stays */
} i_RantDiscoveryDownReason;

/* The public face of one peer. The pointers are into discovery state, valid until the
 * next poll. The overlay is opaque here and decoded by the transport codec. */
typedef struct {
    uint32_t            id;            /* local handle, stable across a drop and return */
    uint8_t             uuid[16];
    RantAddr addr;            /* advertised unicast locator */
    RantPeerLiveness    liveness;
    uint64_t            last_heard_us;
    RantString          name;          /* {NULL,0} if none */
    RantBytes           meta;          /* the overlay, {NULL,0} if none */
    uint32_t            meta_version;  /* version of the overlay we hold */
    uint32_t            adv_meta_version; /* the highest version it advertises, above ours = stale */
    void             *user;          /* the peer's user scratch, or NULL */
} i_RantDiscoveryPeerView;

#define I_RANT_DISCOVERY_RECORDS_ANY 0xFFFFu   /* max_records bound only by the index range */

/* A record's filter of the keys its blob carries, so a new topic of ours fetches only the
 * peers it may meet. spec/discovery.md, The filter. */
#ifndef RANT_DISCOVERY_FILTER_BYTES
#define RANT_DISCOVERY_FILTER_BYTES 32
#endif
#if RANT_DISCOVERY_FILTER_BYTES < 8 || RANT_DISCOVERY_FILTER_BYTES > 64 \
    || (RANT_DISCOVERY_FILTER_BYTES & (RANT_DISCOVERY_FILTER_BYTES - 1))
#error "RANT_DISCOVERY_FILTER_BYTES must be a power of two from 8 to 64"
#endif

/* 1 when a record's filter may hold a key of ours its blob was not judged by. */
typedef int (*i_RantDiscoveryMaybe)(void *ctx, const uint8_t *filter, size_t bytes);

/* What the admit hook says of a peer's blob. A loan is for a blob that cannot be judged
 * alone: it takes only a free slot, never a match's, and rejudge settles it. */
typedef enum {
    I_RANT_ADMIT_NO = 0,    /* nothing of ours to meet: the record keeps no slot */
    I_RANT_ADMIT_YES,       /* meets us: a slot, a loan's if none is free */
    I_RANT_ADMIT_LOAN
} i_RantDiscoveryAdmit;

typedef struct {
    uint8_t  uuid[16];      /* unique per process instance */
    uint16_t domain_id;
    uint16_t data_port;     /* unicast port we advertise */
    uint32_t announce_interval_us;
    uint32_t peer_timeout_us;    /* drop a peer after this much silence */
    uint32_t gone_timeout_us;    /* DROPPED to GONE this long after the drop, 0 = never */
    uint16_t max_peers;     /* slots */
    uint16_t max_records;   /* records held at most, at least max_peers. A hook store starts at max_peers */
    uint8_t  observer;      /* 1 = our solicits carry ALL, so every peer answers at once */
    RantString name;        /* advertised name, copied at init, cut to RANT_NODE_NAME_MAX */
    RantBytes meta;         /* the initial overlay. Must stay valid, .len <= meta_cap */
    uint16_t meta_cap;      /* per peer overlay capacity, 0 = RANT_DISCOVERY_META_MAX */
    uint16_t peer_user_bytes;    /* scratch reserved per peer, 0 = none */
    /* The slot hooks, both optional, for peers holding a slot. up: admission, an address or
     * blob change, or a resume, with the locator and the overlay we hold. down: the reason
     * it leaves the slot. */
    void (*on_peer_up)(void *user, uint32_t peer, const RantAddr *addr, RantBytes meta);
    void (*on_peer_down)(void *user, uint32_t peer, i_RantDiscoveryDownReason reason);
    /* Optional. Whether a peer takes a slot, judged from its blob at version: on arrival
     * without a slot, on a newer version with one, and from rejudge and replay_peers. A NO has
     * folded the blob's keys into filter, zeroed here. Peer 0 is a source with no record, which
     * a match evicts one for. NULL admits every peer at first contact. */
    i_RantDiscoveryAdmit (*admit)(void *user, uint32_t peer, RantBytes meta, uint32_t version,
                                  uint8_t *filter);
    /* Optional. Every record, slotted or not: up at first contact and on a return, down at
     * the drop or when it goes while ACTIVE. Fires before the slot hooks. */
    void (*on_peer_seen)(void *user, uint32_t peer, const RantAddr *addr, int up);
    /* Optional, errors only: PEER_REFUSED (.ip, .port) and PEER_META_TOO_BIG (.peer, 0 for
     * an unknown one, .ip, .port, .too_big_bytes = the wanted size). .user is user below. */
    RantEventFn on_event;
    void *user;
    /* Optional hook. Per peer blobs are then allocated at their actual length instead of
     * a fixed max_peers x meta_cap pool. Pair with i_rant_discovery_destroy. */
    RantAllocFn alloc;
    void       *alloc_user;   /* both hooks' user */
    /* Optional, the record array and the peer blobs: grown through it with alloc, else their
     * fixed blocks taken from it at init. NULL = alloc, or the arena. */
    RantAllocFn bulk;
} i_RantDiscoveryCoreConfig;

typedef struct i_RantDiscoveryState i_RantDiscoveryState;

/* Fills every zero timing and size field with its default. Idempotent. */
void           i_rant_discovery_config_defaults(i_RantDiscoveryCoreConfig *cfg);

/* Bytes one datagram scratch buffer needs for meta_cap (0 = the default). */
uint32_t       i_rant_discovery_wire_size(uint16_t meta_cap);

size_t         i_rant_discovery_required_memory(const i_RantDiscoveryCoreConfig *cfg);
i_RantDiscoveryState *i_rant_discovery_init(void *mem, size_t mem_size, const i_RantDiscoveryCoreConfig *cfg);
/* Frees the hook allocated peer blobs. A no op without a hook. The arena stays the caller's. */
void           i_rant_discovery_destroy(i_RantDiscoveryState *st);
/* Relocates a live core into a bigger block, keeping the uuid, versions, ids and peers.
 * self_meta is the announce blob's new address. The caller frees the old block after. */
i_RantDiscoveryState *i_rant_discovery_core_migrate(i_RantDiscoveryState *old, void *new_mem,
                 size_t new_cap, uint16_t new_max_peers, uint16_t new_meta_cap,
                 const uint8_t *self_meta, void *peer_cb_user);
/* src NULL means the IO layer cannot say, so the locator comes from the blob alone. */
void           i_rant_discovery_on_datagram(i_RantDiscoveryState *st, const RantAddr *src,
                                        RantBytes datagram, uint64_t now_us);
/* The first call opens: it returns our blob at once, with REQ|ALL from an observer. Later
 * calls return an announce or a solicit for the group, seeds and copies, when one is due. */
size_t         i_rant_discovery_update(i_RantDiscoveryState *st, uint64_t now_us, void *out, size_t cap);
/* When update or poll_targeted next has work: an announce, a timeout sweep or owed work. A
 * time at or before now, or 0, means now. */
uint64_t       i_rant_discovery_next_due_us(const i_RantDiscoveryState *st);
size_t         i_rant_discovery_leave(i_RantDiscoveryState *st, void *out, size_t cap);
/* Queues a one shot group solicit. The next update asks peers to answer with their blob,
 * spread over a few ms, or at once for an observer. */
void           i_rant_discovery_solicit(i_RantDiscoveryState *st);
/* Judges every live slotted peer again by the blob we hold: one that no longer meets us
 * gives its slot back, the rest re fire peer up. A caller that changed its own advertised
 * data so re applies every peer's interest. */
void           i_rant_discovery_replay_peers(i_RantDiscoveryState *st);
/* Judges one slotted peer again by the blob we hold, as when what admit judges by came in
 * after the blob. 1 when it gave its slot back. */
int            i_rant_discovery_rejudge(i_RantDiscoveryState *st, uint32_t id);
/* Replaces the overlay and bumps its version so peers re fetch it. meta must stay valid.
 * now_us dates the change, which every announce reports as its age. */
void           i_rant_discovery_set_meta(i_RantDiscoveryState *st, RantBytes meta, uint64_t now_us);
/* The version our announces advertise, 0 if none. Detail responses are stamped with it. */
uint32_t       i_rant_discovery_meta_version(const i_RantDiscoveryState *st);
/* Our own subnets, for locator ranking. Re callable when the interface set changes. A
 * zero mask is ignored. */
void           i_rant_discovery_set_local_subnets(i_RantDiscoveryState *st,
                                      const i_RantDiscoverySubnet *nets, uint8_t n);
/* One owed targeted send that is due by now_us: a reply (BLOB|ONE) or a fetch
 * (REQ|ONE|BLOB). Returns bytes and fills *to, the peer's locator, or 0 when none is due.
 * Nothing goes out before the first update. */
size_t         i_rant_discovery_poll_targeted(i_RantDiscoveryState *st, uint64_t now_us,
                                      void *out, size_t cap, RantAddr *to);
/* How many records the store holds room for, the index range of copy_addr and known_at. */
uint16_t       i_rant_discovery_record_cap(const i_RantDiscoveryState *st);
/* 1 when record i takes a unicast copy of each announce, with its locator in *out. *uni is 1
 * when the copy must carry UNI: the record heard us by unicast lately, so it may hear no group. */
int            i_rant_discovery_copy_addr(const i_RantDiscoveryState *st, uint16_t i,
                                      RantAddr *out, int *uni);
/* Sets or clears UNI in a built datagram, per copy of one announce. */
void           i_rant_discovery_mark_copy(uint8_t *datagram, size_t len, int uni);
/* A source this node heard but cannot take as a peer: another wire version (value is its
 * version), a match with no slot (RANT_E_PEER_REFUSED) or a blob too big (value is its bytes). */
#ifndef RANT_DISCOVERY_TROUBLE_MAX
#define RANT_DISCOVERY_TROUBLE_MAX 16   /* sources tracked, the rest only counted as more */
#endif
typedef struct {
    RantAddr      addr;
    uint64_t      heard_us;
    uint32_t      value;
    uint32_t      peer;         /* the peer's local id when it is in the table, else 0 */
    RantErrorKind kind;
} i_RantDiscoveryTrouble;
/* The troubles heard within three announce intervals, up to max into out. Returns the count. */
int            i_rant_discovery_troubles(const i_RantDiscoveryState *st,
                                      i_RantDiscoveryTrouble *out, int max);
/* 1 while sources of kind past the table's size were heard within three announce intervals. */
int            i_rant_discovery_trouble_more(const i_RantDiscoveryState *st, RantErrorKind kind);
/* 1 when the trouble the last datagram noted was news: a source not tracked before, or the
 * first heard past the table's size. The node reports repeats of a trouble only once. */
int            i_rant_discovery_trouble_new(const i_RantDiscoveryState *st);
/* 1 while a live peer was forgotten for room within three announce intervals: one found no
 * record, or a match took its record. The node's directory partial condition. */
int            i_rant_discovery_partial(const i_RantDiscoveryState *st);
/* Live records, slotted or not, DROPPED ones excluded. */
uint16_t       i_rant_discovery_peer_count(const i_RantDiscoveryState *st);
/* Slots holding a record, DROPPED holders included. */
uint16_t       i_rant_discovery_slots_used(const i_RantDiscoveryState *st);
/* Bumps each time a record that wants a slot finds none, so a caller that can grow knows to. */
uint32_t       i_rant_discovery_seat_misses(const i_RantDiscoveryState *st);
/* Slots the live records want: those holding one and those waiting for one. */
uint16_t       i_rant_discovery_slots_wanted(const i_RantDiscoveryState *st);
/* Live records without a slot whose blob is being fetched, so nothing yet says whether
 * they meet us. The match wait counts each as resolving. */
uint16_t       i_rant_discovery_pending(const i_RantDiscoveryState *st);
/* What admit judges by grew, so a record judged no match may meet us now: each live one is
 * fetched until its blob is judged again, unless the filter of the blob it still advertises
 * makes maybe say no. NULL maybe fetches them all. One past our bound waits for the bound. */
void           i_rant_discovery_refetch_unmatched(i_RantDiscoveryState *st,
                                      i_RantDiscoveryMaybe maybe, void *ctx);
/* 1 when every live record was heard at or after since_us, 0 when one was not, -1 when
 * none is live. */
int            i_rant_discovery_heard_since(const i_RantDiscoveryState *st, uint64_t since_us);
/* Table capacity, the slot range for peer_at. */
uint16_t       i_rant_discovery_max_peers(const i_RantDiscoveryState *st);
/* Read only view of the peer in a slot, ACTIVE or DROPPED. 1 if it holds one. */
int            i_rant_discovery_peer_at(const i_RantDiscoveryState *st, uint16_t slot,
                                      i_RantDiscoveryPeerView *out);
/* The same for record i below record_cap, slotted or not. One without a slot has no blob
 * or user scratch. */
int            i_rant_discovery_known_at(const i_RantDiscoveryState *st, uint16_t i,
                                      i_RantDiscoveryPeerView *out);

/* By id lookups, DROPPED peers included. The node keys its state on the id and uses
 * these instead of a second peer table. */
void          *i_rant_discovery_peer_user(i_RantDiscoveryState *st, uint32_t id);
/* The overlay we hold and, through *version, the version it is at. A view until the next poll. */
RantBytes        i_rant_discovery_peer_meta(const i_RantDiscoveryState *st, uint32_t id,
                                      uint32_t *version);
/* The one address to send data to: the peer's locator. */
int            i_rant_discovery_addr_of_id(const i_RantDiscoveryState *st, uint32_t id,
                                      RantAddr *out);
/* {NULL,0} for an unknown peer. */
RantString       i_rant_discovery_peer_name(const i_RantDiscoveryState *st, uint32_t id);
/* The peer's uuid, a view until the next poll, or NULL for an unknown peer. */
const uint8_t   *i_rant_discovery_peer_uuid(const i_RantDiscoveryState *st, uint32_t id);
/* Maps a source back to a slotted peer's id: its locator, else its data port on a recent path. */
int            i_rant_discovery_id_for_addr(const i_RantDiscoveryState *st, const uint8_t *ip,
                                      uint8_t ip_len, uint16_t port, uint32_t *id);
/* Deterministic RFC 9562 v8 uuid from a stable input plus a boot seed. Not cryptographic. */
void           i_rant_discovery_make_uuid(uint8_t out[16], RantBytes stable, uint64_t boot_seed);
/* Our own uuid, a view valid for the state's lifetime. */
const uint8_t *i_rant_discovery_uuid(const i_RantDiscoveryState *st);

#ifdef __cplusplus
}
#endif
#endif /* RANT_DISCOVERY_H */
