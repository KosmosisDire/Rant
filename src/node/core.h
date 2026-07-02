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

/* The node's app-facing event: the union the user receives via DartNodeOpts.on_event.
 * The node maps discovery's DartDiscoveryEvent (the peer kinds) and the transport's
 * DartTransportEvent (the message/QoS kinds) into this one type, and adds its own
 * (PEER_INTEREST). Flat and self-describing: read only the fields named for the .kind.
 * dart_event_str formats any of them as a one-line message. */
typedef enum {
    DART_PEER_UP,        /* peer discovered or resumed: .peer, .ip/.ip_len/.port */
    DART_PEER_DOWN,      /* peer lost or fell silent: .peer */
    DART_PEER_INTEREST,  /* a peer's interest list was (re)applied: .peer, .publish_topics, .receive_topics */
    DART_MSG_LOST,       /* messages skipped: .channel, .peer, .lost_first .. +.lost_count-1 */
    DART_MSG_TOO_BIG,    /* a received message exceeded max_message_bytes (.too_big_bytes), skipped */
    DART_NAME_COLLISION, /* a peer's name hashes to ours but differs (.identity, .detail = our name), refused */
    DART_QOS_INCOMPATIBLE, /* a reliable subscriber refused a best-effort publisher (.channel, .peer); .detail = our channel name */
    DART_SCHEMA_MISMATCH,  /* incompatible schemas: a match was refused, or a message that did not
                              fit its sender's schema was dropped (.channel, .peer; .detail = our channel name) */
    DART_PEER_REFUSED    /* peer table full of active peers: a new peer was refused (.ip/.ip_len/.port) */
} DartEventKind;

typedef struct {
    DartEventKind kind;
    const char *detail;        /* short human-readable label (NAME_COLLISION / QOS_INCOMPATIBLE: our channel name) */
    void       *user;          /* your DartNodeOpts.user_data (mirrors DartMsg.user) */
    uint32_t   peer;           /* peer id, where applicable (0 = n/a) */
    uint16_t   channel;        /* local channel handle, where applicable */
    uint8_t    ip[16];         /* PEER_UP / PEER_REFUSED: peer address (network order) */
    uint8_t    ip_len;         /* PEER_UP / PEER_REFUSED: 4 or 16; else 0 */
    uint16_t   port;           /* PEER_UP / PEER_REFUSED: peer data port */
    uint64_t   lost_first;     /* MSG_LOST: first skipped seqno */
    uint64_t   lost_count;     /* MSG_LOST: number of messages skipped */
    uint64_t   too_big_bytes;  /* MSG_TOO_BIG: size of the dropped message */
    uint64_t   identity;       /* NAME_COLLISION: the colliding 64-bit topic identity */
    uint16_t   publish_topics; /* PEER_INTEREST: topics we now publish to this peer */
    uint16_t   receive_topics; /* PEER_INTEREST: topics we now receive from this peer */
} DartEvent;
typedef void (*DartEventFn)(const DartEvent *ev);

/* Format ev as a one-line human-readable message into buf (always NUL-terminated,
 * truncated to cap). Returns buf. */
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
