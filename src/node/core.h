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
 * hands that to the runtime, which sends it however its link works (data is unicast). */
#ifndef DART_NODE_CORE_H
#define DART_NODE_CORE_H

#include "../transport/core.h"
#include "../discovery/core.h"    /* DartDiscoveryAddr, DartDiscoveryDownReason */
#include "../serialize/schema.h"  /* DartSchema: channel schemas + peer schema binding */

#ifdef __cplusplus
extern "C" {
#endif

/* The node's app-facing event: what the user receives via DartNodeOpts.on_event. Four
 * lifecycle/info kinds plus ONE catch-all DART_ERROR: everything that went wrong (a
 * refused match, an overflow, a socket/setup failure) arrives as DART_ERROR, and
 * ev->error (a DartErrorKind) says which. So "handle every error the same" is just
 * `case DART_ERROR:` (print dart_event_str), and `switch (ev->error)` drills in when you
 * care. The node maps discovery's DartDiscoveryEvent and the transport's
 * DartTransportEvent into this one type. Flat and self-describing: read only the fields
 * named for the .kind / .error. dart_event_str formats any of them as a one-line message. */
typedef enum {
    DART_PEER_UP,        /* peer discovered or resumed: .peer, .ip/.ip_len/.port */
    DART_PEER_DOWN,      /* peer lost or fell silent: .peer */
    DART_PEER_INTEREST,  /* a peer's interest list was (re)applied: .peer, .publish_topics, .receive_topics */
    DART_MSG_LOST,       /* messages skipped (best-effort loss / unrecoverable gap): .channel, .peer,
                            .lost_first .. +.lost_count-1. Not an error: expected under best-effort. */
    DART_ERROR           /* something went wrong: read .error (a DartErrorKind) and dart_event_str */
} DartEventKind;

/* The specific error carried by a DART_ERROR event (and returned by dart_last_error).
 * It IS the error code: switch on it, or feed the whole event to dart_event_str for text.
 * Named DART_E_* to stay distinct from the DartResult return codes (DART_ERR_*). */
typedef enum {
    DART_E_NONE = 0,
    /* ---- match / config (a match was refused, or advertised data cannot flow) ---- */
    DART_E_NAME_COLLISION,   /* a peer's topic name hashes to ours but differs (.identity, .channel,
                                .channel_name): the match is refused, never silently cross-wired */
    DART_E_QOS_INCOMPATIBLE, /* a reliable subscriber refused a best-effort publisher (.channel, .peer,
                                .channel_name): no silent downgrade; forms if the publisher upgrades */
    DART_E_SCHEMA_MISMATCH,  /* incompatible schemas: a match was refused, or a message that did not fit
                                its sender's schema was dropped (.channel, .peer, .channel_name) */
    DART_E_INTEREST_OVERFLOW,/* a peer's matched topics exceed our alias table (.peer, .lost_count =
                                entries): their data cannot deliver here. Raise DART_META_MAX_IDS. */
    DART_E_META_TRUNCATED_INTEREST, /* our announce overlay overflowed: the interest list was dropped,
                                       so peers see none of our topics. Fewer / shorter topic names. */
    DART_E_META_TRUNCATED_SCHEMA,   /* our announce overlay overflowed: the schema section was dropped,
                                       so peers see partial schemas. Fewer / smaller schemas. */
    DART_E_PEER_META_TOO_BIG,/* a peer's announce blob exceeds our per-peer buffer (.peer 0 if not yet
                                admitted, .too_big_bytes, .ip/.port): its metadata is refused entirely */
    DART_E_MSG_TOO_BIG,      /* a received message exceeded max_message_bytes (.too_big_bytes), skipped */
    DART_E_PEER_REFUSED,     /* peer table full of active peers: a new peer was refused (.ip/.ip_len/.port).
                                Raise discovery.max_peers (dynamic mode grows automatically). */
    DART_E_EVICTED_UNSENT,   /* a send overwrote history never handed to the wire for some matched reader,
                                after the bounded wait (.channel, .lost_first = evicted base seqno,
                                .lost_count = fragment count): the send burst outran the TX drain. */
    /* ---- low-level IO / setup (mostly at dart_node_open; .os_error carries errno) ---- */
    DART_E_OOM,              /* allocator returned NULL / static buffer too small (.too_big_bytes = bytes needed) */
    DART_E_PLATFORM,         /* platform net init failed (WSAStartup) */
    DART_E_SOCKET,           /* opening a UDP socket failed (.os_error) */
    DART_E_BIND,             /* bind failed, port in use? (.port, .os_error) */
    DART_E_MCAST_JOIN,       /* joining the discovery multicast group failed, bad interface? (.os_error) */
    DART_E_SEND,             /* a datagram send hard-failed (.peer, .os_error); reliable data is repaired */
    DART_E_RECV,             /* a socket receive hard-failed (.os_error) */
    DART_E_POLL,             /* the socket poll/wait failed (.os_error) */
    DART_E_WAKER             /* the cross-thread wake loopback is unavailable; a send wakes a blocked poll
                                only at the next timer tick (still works, just less snappy) */
} DartErrorKind;

typedef struct {
    DartEventKind kind;
    DartErrorKind error;       /* DART_ERROR: which error (DART_E_NONE otherwise) */
    const char *channel_name;  /* channel-scoped events: our channel's name (a view into node state,
                                  valid for the callback; NULL when not channel-scoped) */
    void       *user;          /* your DartNodeOpts.user_data (mirrors DartMsg.user) */
    uint32_t   peer;           /* peer id, where applicable (0 = n/a) */
    uint16_t   channel;        /* local channel handle, where applicable */
    int        os_error;       /* SOCKET/BIND/MCAST_JOIN/SEND/RECV/POLL: OS errno / WSAGetLastError (0 = n/a) */
    uint8_t    ip[16];         /* PEER_UP / PEER_REFUSED / PEER_META_TOO_BIG: peer address (network order) */
    uint8_t    ip_len;         /* 4 or 16; else 0 */
    uint16_t   port;           /* peer data port, where applicable */
    uint64_t   lost_first;     /* MSG_LOST / EVICTED_UNSENT: first skipped/evicted seqno */
    uint64_t   lost_count;     /* MSG_LOST / EVICTED_UNSENT: count; INTEREST_OVERFLOW: entry count */
    uint64_t   too_big_bytes;  /* MSG_TOO_BIG / PEER_META_TOO_BIG: size; OOM: bytes needed */
    uint64_t   identity;       /* NAME_COLLISION: the colliding 64-bit topic identity */
    uint16_t   publish_topics; /* PEER_INTEREST: topics we now publish to this peer */
    uint16_t   receive_topics; /* PEER_INTEREST: topics we now receive from this peer */
} DartEvent;
typedef void (*DartEventFn)(const DartEvent *ev);

/* Format ev as a one-line human-readable message into buf (always NUL-terminated,
 * truncated to cap). Returns buf. Covers every kind incl. DART_ERROR (per .error).
 * Under DART_NO_DIAG the descriptive text is compiled out for size and this yields a
 * terse "error N" for DART_ERROR (the numeric fields still print). */
const char *dart_event_str(const DartEvent *ev, char *buf, size_t cap);

/* Everything the core needs from the runtime, set once at init. The peer table itself
 * lives in the discovery core: the node core delegates id<->address resolution and peer
 * naming to it (dart_discovery_*), and stores its small per-peer transport-lifecycle
 * state in the discovery peer's user scratch (i_dart_node_core_peer_user_bytes). */
typedef struct {
    DartTransportState           *transport;    /* the peers are wired into this (sans-IO) */
    DartDiscoveryState  *discovery;    /* the peer table (id<->addr, name, user scratch); may be
                                          NULL at init, then bound via i_dart_node_core_bind_discovery */
    uint16_t              n_channels;    /* sizes the announce-blob buffer */
    uint16_t              frag_size;     /* our UDP fragment size, baked into the overlay */
    DartEventFn         on_event;      /* PEER_UP/DOWN/REFUSED sink (optional) */
    void                 *user;          /* passed to on_event */
    DartAllocFn           alloc;         /* optional: backs interned/rebased peer schemas (no
                                            hook = the schema gate refuses typed matches) */
    void                 *alloc_user;
    int                   oob_capable;   /* 1 = we can deliver out-of-band (SHM) payloads */
    uint8_t               oob_host[16];  /* our host id; a peer is OOB-reachable iff it matches */
} i_DartNodeCoreConfig;

typedef struct i_DartNodeCore i_DartNodeCore;

size_t          i_dart_node_core_required_memory(uint16_t n_channels);
i_DartNodeCore *i_dart_node_core_init(void *mem, size_t mem_size, const i_DartNodeCoreConfig *cfg);
/* Relocate the sans-IO core into a bigger block at grown counts. The transport, discovery,
 * and announce-blob pointers are re-pointed by the caller after those move. Dynamic growth. */
i_DartNodeCore *i_dart_node_core_migrate(i_DartNodeCore *old, void *new_mem, size_t new_cap,
                                       uint16_t new_n_channels);
/* Bind (or rebind, after a migrate) the discovery core whose peer table this core delegates
 * to. The runtime calls it once discovery exists, and again after discovery relocates. */
void            i_dart_node_core_bind_discovery(i_DartNodeCore *c, DartDiscoveryState *discovery);
/* Bytes of per-peer scratch the core needs in the discovery peer table (its transport-
 * lifecycle state). The runtime sets discovery's cfg.peer_user_bytes to this. */
uint16_t        i_dart_node_core_peer_user_bytes(void);

/* The discovery announce blob this node sends: its frag size, OOB host, interest
 * list, and published schemas. The core owns the buffer and builds it (the codec is
 * dart_meta_* in the transport core). build_meta (re)builds it from the core's current
 * fields and returns the length. meta returns the bytes (a view of the core's buffer)
 * for the runtime to feed to discovery. Rebuild after a role change, then re-feed
 * discovery. */
uint16_t        i_dart_node_core_build_meta(i_DartNodeCore *c);
DartBytes       i_dart_node_core_meta(i_DartNodeCore *c);
/* Register (or clear: NULL) a channel's schema: advertised in the overlay, matched by
 * the schema gate, and the base of the reader's bound view. schema must outlive the
 * channel (it is the node-owned parsed copy). Rebuild the meta after; only
 * non-INACTIVE channels are advertised, so a role flip just rebuilds. */
void            i_dart_node_core_set_channel_schema(i_DartNodeCore *c, uint16_t channel,
                                                    const DartSchema *schema);

/* The transport's DartConfig.schema_check, node-style (see transport/core.h): decide a
 * would-be match against the overlay currently being applied (peer_up stashes it).
 * Typed vs typed matches iff same root name and the reader's fields are a subset of the
 * writer's (dart_schema_subset); a typed reader refuses an untyped or unverifiable
 * writer; an untyped (generic) reader accepts anything. On an allowed read-side match
 * this also interns the peer's schema and records the reader view for delivery. */
int i_dart_node_core_schema_check(i_DartNodeCore *c, uint16_t channel, uint16_t alias,
                                  int peer_is_pub);

/* The schema to decode a delivered message with: the channel's own schema when the
 * sender's is identical, a rebased view of the sender's layout when it is a superset,
 * the sender's interned schema for a generic (schema-less) channel, or NULL (raw). */
const DartSchema *i_dart_node_core_msg_schema(i_DartNodeCore *c, uint32_t peer, uint16_t channel);

/* Discovery event sink: register as the discovery core's on_event (cfg.user = this
 * core). Demuxes the generic DartDiscoveryEvent (PEER_UP/DOWN/REFUSED), keeps the peer
 * table and transport peer set in lockstep (dormant/resume/evict), and fires the app's
 * PEER_UP/DOWN/INTEREST/REFUSED DartEvents (the node maps discovery's events to its own). */
void i_dart_node_core_on_disc_event(const DartDiscoveryEvent *ev);

/* A resolved outbound destination: a unicast peer by physical address. The runtime
 * turns this into wire bytes for its link. */
typedef struct {
    uint8_t  ip[16];     /* peer physical address (IPv4 today) */
    uint8_t  ip_len;
    uint16_t port;       /* peer data port */
} i_DartNodeDest;

/* Destination resolution: the node-core/runtime boundary. resolve turns the transport's
 * abstract destination (dart_transport_poll_send's to_peer = a peer id) into a peer address, so the
 * runtime only maps the result to wire bytes. Returns 1 if sendable, 0 if the peer is
 * unknown. id_for_addr maps an inbound source address back to a peer id (1 + *id on a
 * hit, else 0). */
int  i_dart_node_core_resolve(i_DartNodeCore *c, uint32_t to, i_DartNodeDest *out);
int  i_dart_node_core_id_for_addr(i_DartNodeCore *c, const uint8_t ip[4], uint16_t port, uint32_t *id);

/* A peer's human-readable name, learned from its announce blob: a DartString viewing the
 * peer-table slot (not NUL-terminated; stable until the peer is evicted). Non-empty for any
 * known peer ("unknown-peer" if its announce carried none); .data is NULL only when id is
 * not a known peer. For debug/observability only. */
DartString i_dart_node_core_peer_name(i_DartNodeCore *c, uint32_t id);

/* Read-only peer-table enumeration (diagnostics / tests). max_peers is the capacity;
 * peer_at fills the out-params for table slot in [0, max_peers) and returns 1 if it
 * holds a live peer, else 0. Any out-pointer may be NULL. */
uint16_t i_dart_node_core_max_peers(i_DartNodeCore *c);
int      i_dart_node_core_peer_at(i_DartNodeCore *c, uint16_t slot, uint32_t *id,
                                uint8_t ip[16], uint8_t *ip_len, uint16_t *port);

/* Decode helpers for a peer's announce overlay (the transport meta blob discovery carries
 * opaquely). The node owns the transport codec, so a diagnostics caller reads a peer's
 * fragment size + interest off a DartDiscoveryPeer (from dart_node_peers) without ever
 * touching dart_meta_*. Both read the peer's raw overlay pointer, valid until the next poll. */
uint16_t dart_node_peer_frag(const DartDiscoveryPeer *peer);   /* advertised UDP fragment size; 0 if none/malformed */
/* Walk a peer's interest list one topic at a time (publishes, then subscribes): zero a
 * DartInterestIter, then call until it returns 0. Fills *out (out->name points into the
 * peer's overlay, NOT NUL-terminated). 0 when the peer carries no overlay or at the end. */
int      dart_node_peer_interest_next(const DartDiscoveryPeer *peer,
                              DartInterestIter *it, DartTopic *out);
/* A peer's advertised schema for one of its publish topics (alias = the DartTopic.alias
 * from the interest walk): 1 + fills *hash if the topic advertises one, else 0. *wire is
 * the schema's canonical bytes when the peer inlined them (a view into the overlay,
 * decode with dart_schema_parse), or {NULL,0} when only the hash was advertised. */
int      dart_node_peer_schema(const DartDiscoveryPeer *peer, uint16_t alias,
                              uint64_t *hash, DartBytes *wire);

#ifdef __cplusplus
}
#endif
#endif /* DART_NODE_CORE_H */
