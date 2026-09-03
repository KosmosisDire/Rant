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
#include "../serialize/schema.h"  /* DartSchema: topic schemas + peer schema binding */

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
    DART_MSG_LOST,       /* messages skipped (best-effort loss / unrecoverable gap): .topic, .peer,
                            .lost_first .. +.lost_count-1. Not an error: expected under best-effort. */
    DART_ERROR           /* something went wrong: read .error (a DartErrorKind) and dart_event_str */
} DartEventKind;

/* The specific error carried by a DART_ERROR event (and returned by dart_last_error).
 * It IS the error code: switch on it, or feed the whole event to dart_event_str for text.
 * Named DART_E_* to stay distinct from the DartResult return codes (DART_ERR_*). */
typedef enum {
    DART_E_NONE = 0,
    /* ---- match / config (a match was refused, or advertised data cannot flow) ---- */
    DART_E_NAME_COLLISION,   /* a peer's topic name hashes to ours but differs (.identity, .topic,
                                .topic_name): the match is refused, never silently cross-wired */
    DART_E_QOS_INCOMPATIBLE, /* a reliable subscriber refused a best-effort publisher (.topic, .peer,
                                .topic_name): no silent downgrade; forms if the publisher upgrades */
    DART_E_KIND_MISMATCH,    /* a peer advertised this topic name under a different entity kind (a plain
                                topic vs a function/variable): the pairing is refused (.topic,
                                .peer, .topic_name), never silently cross-wired */
    DART_E_SCHEMA_MISMATCH,  /* incompatible schemas: a match was refused, or a message that did not fit
                                its publisher's schema was dropped (.topic, .peer, .topic_name) */
    DART_E_INTEREST_OVERFLOW,/* a peer's matched topics carry indices whose map could not be
                                allocated (.peer, .lost_count = entries): their data cannot
                                deliver here. */
    DART_E_META_TRUNCATED_INTEREST, /* our announce overlay overflowed: the interest list was dropped,
                                       so peers see none of our topics. Fewer / shorter topic names. */
    DART_E_META_TRUNCATED_SCHEMA,   /* our announce overlay overflowed: the schema section was dropped,
                                       so peers see partial schemas. Fewer / smaller schemas. */
    DART_E_PEER_META_TOO_BIG,/* a peer's announce blob exceeds our per-peer buffer (.peer 0 if not yet
                                admitted, .too_big_bytes, .ip/.port): its metadata is refused entirely */
    DART_E_MSG_TOO_BIG,      /* a received message exceeded max_message_bytes (.too_big_bytes), skipped */
    DART_E_PEER_REFUSED,     /* peer table full of active peers: a new peer was refused (.ip/.ip_len/.port).
                                Raise discovery.max_peers (dynamic mode grows automatically). */
    DART_E_EVICTED_UNSENT,   /* a send overwrote history never handed to the wire for some matched subscriber,
                                after the bounded wait (.topic, .lost_first = evicted base seqno,
                                .lost_count = fragment count): the send burst outran the TX drain. */
    DART_E_UNMATCHED_SEND,   /* a send committed with ZERO matched subscribers while a candidate match was
                                still resolving (.topic, .topic_name): the match wait timed out (or was
                                disabled / the send came from a callback), so the message likely missed a
                                subscriber that was already on the network. See dart_topic_ready. */
    DART_E_DUPLICATE_AUTHORITY, /* a peer also advertises the authoritative side of a function or
                                   variable this node handles/owns (.topic, .peer, .topic_name): two
                                   handlers/owners exist where the pattern contract expects exactly one.
                                   Diagnostic, not a refusal: calls take the first response, accessors
                                   converge on the last write. Fired once per (entity, peer). */
    /* ---- low-level IO / setup (mostly at dart_node_open; .os_error carries errno) ---- */
    DART_E_OOM,              /* allocator returned NULL / static buffer too small (.too_big_bytes = bytes needed) */
    DART_E_PLATFORM,         /* platform net init failed (WSAStartup) */
    DART_E_SOCKET,           /* opening a UDP socket failed (.os_error) */
    DART_E_BIND,             /* bind failed, port in use? (.port, .os_error) */
    DART_E_MCAST_JOIN,       /* joining the discovery multicast group failed, bad interface? (.os_error) */
    DART_E_SEND,             /* a datagram send hard-failed (.peer, .os_error, .too_big_bytes = the
                                datagram size, .topic/.topic_name = the first submessage's topic;
                                the datagram batches one peer's lanes, so more topics may ride along):
                                reliable data is repaired. A resource-starved link surfaces here (an
                                ESP32 out of WiFi TX buffers reports os_error ENOMEM even with heap free) */
    DART_E_RECV,             /* a socket receive hard-failed (.os_error) */
    DART_E_POLL,             /* the socket poll/wait failed (.os_error) */
    DART_E_WAKER,            /* the cross-thread wake loopback is unavailable; a send wakes a blocked poll
                                only at the next timer tick (still works, just less snappy) */
    DART_E_BAD_ADDRESS       /* a configured address string could not be parsed (opts.net.self_ip):
                                a config fault, refused at open rather than silently ignored, since
                                a node advertising an unreachable locator looks healthy and is not */
} DartErrorKind;

typedef struct {
    DartEventKind kind;
    DartErrorKind error;       /* DART_ERROR: which error (DART_E_NONE otherwise) */
    const char *topic_name;  /* topic-scoped events: our topic's name (a view into node state,
                                  valid for the callback; NULL when not topic-scoped) */
    void       *user;          /* your DartNodeOpts.user_data (mirrors DartMsg.user) */
    uint32_t   peer;           /* peer id, where applicable (0 = n/a) */
    uint16_t   topic;        /* local topic handle, where applicable */
    int        os_error;       /* SOCKET/BIND/MCAST_JOIN/SEND/RECV/POLL: OS errno / WSAGetLastError (0 = n/a) */
    uint8_t    ip[16];         /* PEER_UP / PEER_REFUSED / PEER_META_TOO_BIG: peer address (network order) */
    uint8_t    ip_len;         /* 4 or 16; else 0 */
    uint16_t   port;           /* peer data port, where applicable */
    uint64_t   lost_first;     /* MSG_LOST / EVICTED_UNSENT: first skipped/evicted seqno */
    uint64_t   lost_count;     /* MSG_LOST / EVICTED_UNSENT: count; INTEREST_OVERFLOW: entry count */
    uint64_t   too_big_bytes;  /* MSG_TOO_BIG / PEER_META_TOO_BIG: size; OOM: bytes needed; SEND: datagram size */
    uint64_t   identity;       /* NAME_COLLISION: the colliding 64-bit topic identity */
    uint16_t   publish_topics; /* PEER_INTEREST: topics we now publish to this peer */
    uint16_t   receive_topics; /* PEER_INTEREST: topics we now receive from this peer */
    const char *schema_detail; /* SCHEMA_MISMATCH: what exactly was incompatible, one line (a
                                  view into node state, valid for the callback; NULL when
                                  unknown, or under DART_NO_DIAG) */
    const char *peer_name;     /* peer-scoped events: the subject peer's human-readable node name
                                  (a NUL-terminated view into discovery state, valid for the
                                  callback; NULL when there is no peer or its name is unknown).
                                  Prefer it over .peer in messages: an id means nothing to a human. */
} DartEvent;
typedef void (*DartEventFn)(const DartEvent *ev);

/* Format ev as a one-line human-readable message into buf (always NUL-terminated,
 * truncated to cap). Returns buf. Covers every kind incl. DART_ERROR (per .error).
 * Under DART_NO_DIAG the descriptive text is compiled out for size and this yields a
 * terse "error N" for DART_ERROR (the numeric fields still print). */
const char *dart_event_str(const DartEvent *ev, char *buf, size_t cap);

/* ---- reflection types (the walks are in node/runtime.h) ------------------------------- */
typedef struct { uint32_t a, b; uint16_t c, d; } DartIter;   /* walk state: zero-initialize */

#define DART_SELF 0u   /* the peer id that means this node */

typedef struct {
    uint32_t         id;             /* handle, stable across a drop and return; never DART_SELF */
    uint8_t          uuid[16];       /* the process instance: a restart is a new uuid, same name */
    DartString       name;
    DartString       address;        /* "ip:port" */
    DartPeerLiveness liveness;
    uint64_t         last_heard_us;
    uint32_t         epoch;          /* bumps on every reflected change at this peer */
    uint8_t          catching_up;    /* 1 = it advertises newer state than we hold yet */
    uint16_t         fragment_size;  /* advertised UDP fragment size */
} DartPeerInfo;

typedef enum {
    DART_ENTITY_TOPIC = 0,
    DART_ENTITY_FUNCTION,
    DART_ENTITY_VARIABLE,
    DART_ENTITY_TASK
} DartEntityKind;

typedef struct {
    DartEntityKind    kind;
    DartString        name;          /* base name; {NULL,0} until the details arrive (show hash) */
    uint32_t          hash;          /* the primary channel's low-32 name hash: the placeholder */
    uint8_t           provides;      /* someone live is on the source side */
    uint8_t           consumes;      /* someone live is on the sink side */
    uint8_t           reliable;      /* the primary channel's reliability */
    uint8_t           writable;      /* VARIABLE: a set channel is advertised */
    uint8_t           forceable;     /* VARIABLE: the owner permits force */
    uint8_t           cancellable;   /* TASK: the provider honors cancel */
    uint8_t           exclusive;     /* TASK: declared serialization */
    uint8_t           multi;         /* duplicate authority is intended */
    uint8_t           incomplete;    /* a pattern half pair: surfaced, never silently dropped */
    uint8_t           conflict;      /* MESH: live endpoints declare schemas that cannot read each other */
    uint16_t          providers;     /* live endpoints on each side (a per-node walk reports 0 or 1) */
    uint16_t          consumers;
    uint32_t          provider;      /* the ranked provider's peer id (DART_SELF = this node); iff providers > 0 */
    DartString        from;          /* the node the schemas below were read from */
    const DartSchema *schema;        /* value / request / payload (NULL = untyped or unfetched) */
    uint64_t          schema_hash;
    const DartSchema *rsp_schema;    /* FUNCTION / TASK: the response */
    uint64_t          rsp_schema_hash;
    const DartSchema *progress_schema;   /* TASK: the progress channel */
    uint64_t          progress_schema_hash;
    uint64_t          generation;    /* changes iff provider identity, any schema, or an attr changed */
} DartEntityInfo;

/* Everything the core needs from the runtime, set once at init. The peer table itself
 * lives in the discovery core: the node core delegates id<->address resolution and peer
 * naming to it (dart_discovery_*), and stores its small per-peer transport-lifecycle
 * state in the discovery peer's user scratch (i_dart_node_core_peer_user_bytes). */
typedef struct {
    DartTransportState           *transport;    /* the peers are wired into this (sans-IO) */
    DartDiscoveryState  *discovery;    /* the peer table (id<->addr, name, user scratch); may be
                                          NULL at init, then bound via i_dart_node_core_bind_discovery */
    uint16_t              n_topics;    /* sizes the announce-blob buffer */
    uint16_t              frag_size;     /* our UDP fragment size, baked into the overlay */
    DartEventFn         on_event;      /* PEER_UP/DOWN/REFUSED sink (optional) */
    void                 *user;          /* passed to on_event */
    DartAllocFn           alloc;         /* optional: backs interned/rebased peer schemas (no
                                            hook = the schema gate refuses typed matches) */
    void                 *alloc_user;
    int                   oob_capable;   /* 1 = we can deliver out-of-band (SHM) payloads */
    uint8_t               oob_host[16];  /* our host id; a peer is OOB-reachable iff it matches */
    uint8_t               fetch_details; /* 1 = the detail cycle requests EVERY advertised index
                                            (observer mode) and caches name + schema per
                                            (peer, index) for i_dart_node_core_topic_detail */
} i_DartNodeCoreConfig;

typedef struct i_DartNodeCore i_DartNodeCore;

/* The announce blob is hook-allocated at actual size, so the arena holds only the
 * core struct + the per-topic schema registry. cfg.alloc is required. */
size_t          i_dart_node_core_required_memory(uint16_t n_topics);
i_DartNodeCore *i_dart_node_core_init(void *mem, size_t mem_size, const i_DartNodeCoreConfig *cfg);
/* Relocate the sans-IO core into a bigger block at grown counts. The transport, discovery,
 * and announce-blob pointers are re-pointed by the caller after those move. Dynamic growth. */
i_DartNodeCore *i_dart_node_core_migrate(i_DartNodeCore *old, void *new_mem, size_t new_cap,
                                       uint16_t new_n_topics);
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
/* Register (or clear: NULL) a topic's schema: advertised in the overlay, matched by
 * the schema gate, and the base of the subscriber's bound view. schema must outlive the
 * topic (it is the node-owned parsed copy). Rebuild the meta after; only
 * non-INACTIVE topics are advertised, so a role flip just rebuilds. */
void            i_dart_node_core_set_topic_schema(i_DartNodeCore *c, uint16_t topic_index,
                                                    const DartSchema *schema);

/* Topic slot lifecycle (dart_transport_topic_retire / _reuse at the node level).
 * retire_topic_schema drops the live schema pointers when a topic retires but KEEPS the
 * hash as the slot's binding fingerprint; topic_schema_hash reads it back, so a later
 * same-name create can tell an identical rebind (verdicts stay) from a retype (the slot
 * rebinds under a bumped generation). topic_rebound purges the per-peer decode bindings,
 * rebased views, and refusal reasons recorded for a slot's OLD occupant after a retype
 * rebind. seen_version feeds a peer's uDTL-request version to the transport's rebind
 * hold; returns 1 when the advance released a held writer lane (the interest event
 * re-fired; the runtime invalidates its match memos). */
void     i_dart_node_core_retire_topic_schema(i_DartNodeCore *c, uint16_t topic_index);
uint64_t i_dart_node_core_topic_schema_hash(i_DartNodeCore *c, uint16_t topic_index);
void     i_dart_node_core_topic_rebound(i_DartNodeCore *c, uint16_t topic_index);
int      i_dart_node_core_seen_version(i_DartNodeCore *c, uint32_t peer, uint32_t version);

/* Answer a peer's DETAIL_REQ ('uDTL', see the detail codec in transport/core.h): validate
 * kind + domain, build the response in the core's own grown buffer, and return it for the
 * runtime to send to the request's SOURCE address ({NULL,0} = not answerable: malformed,
 * wrong domain, or OOM; the requester re-asks). Stateless and idempotent: nothing is
 * recorded, so duplicate or crossing requests are harmless. The returned view is valid
 * until the next call. */
DartBytes i_dart_node_core_detail_respond(i_DartNodeCore *c, uint16_t domain, DartBytes req);

/* The uDTL interest paging (kinds 3/4): how a peer whose interest list does not fit its
 * announce (INTEREST_EXTERNAL) serves and fetches it. interest_respond answers an
 * INTEREST_REQ with one sub-datagram byte-range page of our current interest blob (same
 * contract as detail_respond: stateless, idempotent, {NULL,0} when unanswerable, view
 * valid until the next call). apply_interest_page ingests one INTEREST_RESP page:
 * appends at the peer's cursor, re-queues the next request while incomplete, and on
 * completion applies the assembled blob exactly as an inline announce would. */
DartBytes i_dart_node_core_interest_respond(i_DartNodeCore *c, uint16_t domain, DartBytes req);
void      i_dart_node_core_apply_interest_page(i_DartNodeCore *c, uint16_t domain, uint32_t peer,
                                      DartBytes resp);

/* The transport's DartConfig.schema_check, node-style (see transport/core.h): decide a
 * would-be match from the peer's advertised schema identity + wire, delivered by its
 * detail response. Typed vs typed matches iff same root name and the subscriber's fields are
 * a subset of the publisher's (dart_schema_subset); a typed subscriber refuses an untyped or
 * unverifiable publisher; an untyped (generic) subscriber accepts anything. On an allowed
 * read-side check this also interns the peer's schema and records the subscriber view for
 * delivery, keyed by the peer id. */
int i_dart_node_core_schema_check(i_DartNodeCore *c, uint32_t peer, uint16_t topic_index,
                                  int peer_is_pub, uint64_t hash, DartBytes wire);

/* Why the schema gate refused (peer, topic) in the given direction (peer_is_pub as in
 * schema_check): the reason recorded at detail intake, feeding DartEvent.schema_detail
 * when the transport fires SCHEMA_MISMATCH at interest apply. A view into node state,
 * valid until the verdict changes or the peer is removed; NULL when unknown (fixed
 * mode, or DART_NO_DIAG). */
const char *i_dart_node_core_schema_why(i_DartNodeCore *c, uint32_t peer, uint16_t topic_index,
                                        int peer_is_pub);
/* Record + return the reason for a delivery-length mismatch (a message that did not fit
 * its publisher's schema), same storage/lifetime as schema_why. NULL under DART_NO_DIAG. */
const char *i_dart_node_core_note_size_mismatch(i_DartNodeCore *c, uint32_t peer,
                                        uint16_t topic_index, uint64_t got_len, uint64_t want_len);

/* The schema to decode a delivered message with: the topic's own schema when the
 * publisher's is identical, a rebased view of the publisher's layout when it is a superset,
 * the publisher's interned schema for a generic (schema-less) topic, or NULL (raw). */
const DartSchema *i_dart_node_core_msg_schema(i_DartNodeCore *c, uint32_t peer, uint16_t topic_index);

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

/* The requester side of the uDTL cycle (the announce nominates by hash; details verify
 * and match; external interest is paged in first, since it gates candidate discovery).
 * apply_details ingests a peer's DETAIL_RESP off the data socket. detail_any says a
 * request (interest or detail) is queued somewhere; the runtime then loops
 * detail_req_next (build request + destination, send each) until it returns 0; interest
 * requests drain before detail requests. detail_rearm re-queues every ACTIVE peer: the
 * runtime's periodic retry sweep, cheap once converged (each peer costs one wants()
 * walk and sends nothing). */
void   i_dart_node_core_apply_details(i_DartNodeCore *c, uint16_t domain, uint32_t peer,
                                      DartBytes resp);
int    i_dart_node_core_detail_any(i_DartNodeCore *c);
void   i_dart_node_core_detail_rearm(i_DartNodeCore *c);
size_t i_dart_node_core_detail_req_next(i_DartNodeCore *c, uint16_t domain,
                                        void *out, size_t cap, i_DartNodeDest *to);
/* Unresolved candidate matches for one topic across every ACTIVE peer: peers whose
 * announce nominates this topic but whose detail verdicts are still in flight, plus a
 * peer admitted before its blob arrived (its interest is unknown, so it may nominate).
 * 0 = matching has converged for everyone currently known. The runtime's send-path
 * match wait and dart_topic_pending_count read; cold-path only (walks peers). */
int    i_dart_node_core_topic_unresolved(i_DartNodeCore *c, uint16_t topic_index);

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

/* ---- reflection: the tables behind node/runtime.h's walks ------------------------------
 * Per peer: a channel table dense by the peer's topic index (kind/role/reliable/hash from
 * every interest apply, name/attrs/schema from every detail response), folded lazily into
 * entities when read. The local node is one more channel source, rebuilt by the runtime
 * through self_begin/channel/end on every topology change. The mesh table folds every
 * ACTIVE peer plus self, one entity per (kind, name), rebuilt lazily after any change. */
uint16_t i_dart_node_peer_frag(const DartDiscoveryPeer *peer);
uint32_t i_dart_node_peer_interest_epoch(const DartDiscoveryPeer *peer);
int      i_dart_node_peer_interest_next(const DartDiscoveryPeer *peer,
                              DartInterestIter *it, DartTopicEntry *out);
void     i_dart_node_core_set_self_name(i_DartNodeCore *c, DartString name);
void     i_dart_node_core_self_begin(i_DartNodeCore *c);
void     i_dart_node_core_self_channel(i_DartNodeCore *c, uint16_t index, DartString name,
                              uint8_t kind, uint8_t role, uint8_t reliable, uint8_t attrs,
                              const DartSchema *schema);
void     i_dart_node_core_self_end(i_DartNodeCore *c);
int      i_dart_node_core_peers_next(i_DartNodeCore *c, DartIter *it, DartPeerInfo *out);
int      i_dart_node_core_entities_next(i_DartNodeCore *c, uint32_t peer, DartIter *it,
                              DartEntityInfo *out);
int      i_dart_node_core_mesh_next(i_DartNodeCore *c, DartIter *it, DartEntityInfo *out);
int      i_dart_node_core_mesh_find(i_DartNodeCore *c, DartEntityKind kind, const char *name,
                              DartEntityInfo *out);
uint32_t i_dart_node_core_mesh_epoch(i_DartNodeCore *c);
/* The create-time pick for ONE channel (which: 0 primary, 1 rsp, 2 prg) of an entity,
 * over every live declaration: a writer takes the widest schema every reader accepts, a
 * reader takes the provider's. reliable = the provider's offer for a reader, "any reader
 * requests reliable" for a writer. 0 when nobody advertises the entity. */
int      i_dart_node_core_reflect_pick(i_DartNodeCore *c, DartEntityKind kind, const char *name,
                              int which, int writer, const DartSchema **schema,
                              uint8_t *reliable, uint64_t *generation);

#ifdef __cplusplus
}
#endif
#endif /* DART_NODE_CORE_H */
