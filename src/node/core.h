/* The sans-IO node core: the peer table, the discovery to transport lifecycle and
 * address resolution. A runtime owns the IO and drives it. The rules are in spec/node.md. */
#ifndef DART_NODE_CORE_H
#define DART_NODE_CORE_H

#include "../common/api.h"
#include "../transport/core.h"
#include "../discovery/core.h"
#include "../serialize/schema.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The node's event. Four lifecycle kinds plus DART_ERROR, whose .error says which fault.
 * Read only the fields named for the kind. dart_event_str formats any of them. */
typedef enum {
    DART_PEER_UP,        /* discovered or resumed: .peer, .ip, .ip_len, .port */
    DART_PEER_DOWN,      /* lost or fell silent: .peer */
    DART_PEER_INTEREST,  /* interest applied: .peer, .publish_topics, .receive_topics */
    DART_MSG_LOST,       /* seqnos skipped: .topic, .peer, .lost_first, .lost_count. Not an error */
    DART_ERROR           /* read .error and dart_event_str */
} DartEventKind;

/* The error carried by a DART_ERROR event and returned by dart_last_error. Named DART_E_*
 * to stay distinct from the DartResult return codes. docs/node.md lists them. */
typedef enum {
    DART_E_NONE = 0,
    /* a match was refused, or advertised data cannot flow */
    DART_E_NAME_COLLISION,   /* a peer's topic name hashes to ours but differs: .identity, .topic */
    DART_E_QOS_INCOMPATIBLE, /* a reliable subscriber met a best effort publisher: .topic, .peer */
    DART_E_KIND_MISMATCH,    /* the name is another entity kind at a peer: .topic, .peer */
    DART_E_SCHEMA_MISMATCH,  /* a refused match or an ill fitting message: .topic, .peer */
    DART_E_INTEREST_OVERFLOW,/* a peer's index map failed to allocate: .peer, .lost_count entries */
    DART_E_META_TRUNCATED_INTEREST, /* our overlay overflowed, peers see no topics */
    DART_E_META_TRUNCATED_SCHEMA,   /* retired, kept so binding enums stay aligned */
    DART_E_PEER_META_TOO_BIG,/* a peer's blob exceeds our buffer: .peer (0 = new), .too_big_bytes */
    DART_E_MSG_TOO_BIG,      /* a received message could not be buffered: .too_big_bytes */
    DART_E_PEER_REFUSED,     /* the peer table is full of active peers: .ip, .ip_len, .port */
    DART_E_EVICTED_UNSENT,   /* a send overwrote unsent history: .topic, .lost_first, .lost_count */
    DART_E_UNMATCHED_SEND,   /* a send committed to nobody while a match was resolving: .topic */
    DART_E_DUPLICATE_AUTHORITY, /* a peer also claims the handler side: .topic, .peer. Diagnostic */
    /* IO and setup, mostly at open. .os_error carries the OS code */
    DART_E_OOM,              /* the allocator returned NULL: .too_big_bytes = bytes needed */
    DART_E_PLATFORM,         /* platform net init failed */
    DART_E_SOCKET,           /* opening a UDP socket failed */
    DART_E_BIND,             /* bind failed, the port is in use: .port */
    DART_E_MCAST_JOIN,       /* joining the discovery group failed, a bad interface */
    DART_E_SEND,             /* a send hard failed: .peer, .topic, .too_big_bytes = its size */
    DART_E_RECV,             /* a receive hard failed */
    DART_E_POLL,             /* the socket wait failed */
    DART_E_WAKER,            /* no cross thread waker, wakes come at the next tick */
    DART_E_BAD_ADDRESS       /* a configured address could not be parsed, refused at open */
} DartErrorKind;

typedef struct {
    DartEventKind kind;
    DartErrorKind error;       /* DART_ERROR: which error, else DART_E_NONE */
    const char *topic_name;  /* topic scoped events: our topic's name, valid for the callback */
    void       *user;          /* DartNodeOpts.user_data */
    uint32_t   peer;           /* the peer id, 0 = none */
    uint16_t   topic;        /* the local topic handle */
    int        os_error;       /* the OS code for SOCKET, BIND, MCAST_JOIN, SEND, RECV and POLL */
    uint8_t    ip[16];         /* the peer address, network order */
    uint8_t    ip_len;         /* 4 or 16, else 0 */
    uint16_t   port;
    uint64_t   lost_first;     /* MSG_LOST and EVICTED_UNSENT: the first seqno */
    uint64_t   lost_count;     /* the count, or INTEREST_OVERFLOW's entries */
    uint64_t   too_big_bytes;  /* MSG_TOO_BIG, PEER_META_TOO_BIG, OOM and SEND: the byte count */
    uint64_t   identity;       /* NAME_COLLISION: the colliding identity */
    uint16_t   publish_topics; /* PEER_INTEREST: topics we now publish to this peer */
    uint16_t   receive_topics; /* PEER_INTEREST: topics we now receive from it */
    const char *schema_detail; /* SCHEMA_MISMATCH: one line saying what was incompatible, or NULL */
    const char *peer_name;     /* the peer's node name, NULL when unknown. Prefer it over .peer */
} DartEvent;
typedef void (*DartEventFn)(const DartEvent *ev);

/* Formats ev as one line into buf, always NUL terminated. Returns buf. DART_NO_DIAG
 * compiles the text out and yields "error N". */
DART_API const char *dart_event_str(const DartEvent *ev, char *buf, size_t cap);

/* reflection types, the walks are in node/runtime.h */
typedef struct { uint32_t a, b; uint16_t c, d; } DartIter;   /* walk state, zero initialize */

#define DART_SELF 0u   /* the peer id that means this node */

typedef struct {
    uint32_t         id;             /* stable across a drop and return, never DART_SELF */
    uint8_t          uuid[16];       /* the process instance, a restart is a new uuid */
    DartString       name;
    DartString       address;        /* "ip:port" */
    DartPeerLiveness liveness;
    uint64_t         last_heard_us;
    uint32_t         epoch;          /* bumps on every reflected change at this peer */
    uint8_t          catching_up;    /* 1 = it advertises newer state than we hold */
    uint16_t         fragment_size;  /* its advertised UDP fragment size */
    /* the round trip as our reliable traffic measured it, samples 0 = no estimate yet */
    uint32_t         rtt_us;
    uint32_t         rtt_jitter_us;
    uint32_t         rtt_min_us;
    uint32_t         rtt_samples;
} DartPeerInfo;

typedef enum {
    DART_ENTITY_TOPIC = 0,
    DART_ENTITY_FUNCTION,
    DART_ENTITY_VARIABLE,
    DART_ENTITY_TASK
} DartEntityKind;

typedef struct {
    DartEntityKind    kind;
    DartString        name;          /* the base name, {NULL,0} until the details arrive */
    uint32_t          hash;          /* the primary channel's low 32 name hash, the placeholder */
    uint8_t           provides;      /* someone live is on the source side */
    uint8_t           consumes;      /* someone live is on the sink side */
    uint8_t           reliable;      /* the primary channel's reliability */
    uint8_t           writable;      /* VARIABLE: a set channel is advertised */
    uint8_t           forceable;     /* VARIABLE: the owner permits force */
    uint8_t           cancellable;   /* TASK: the provider honors cancel */
    uint8_t           exclusive;     /* TASK: declared serialization */
    uint8_t           multi;         /* duplicate authority is intended */
    uint8_t           incomplete;    /* a pattern half pair, surfaced rather than dropped */
    uint8_t           conflict;      /* MESH: live schemas that cannot read each other */
    uint16_t          providers;     /* live endpoints per side, a per node walk reports 0 or 1 */
    uint16_t          consumers;
    uint32_t          provider;      /* the ranked provider's peer id, DART_SELF is this node */
    DartString        from;          /* the node the schemas below were read from */
    const DartSchema *schema;        /* value, request or payload, NULL = untyped or unfetched */
    uint64_t          schema_hash;
    const DartSchema *rsp_schema;    /* FUNCTION and TASK: the response */
    uint64_t          rsp_schema_hash;
    const DartSchema *progress_schema;   /* TASK: the progress channel */
    uint64_t          progress_schema_hash;
    uint64_t          generation;    /* changes iff the provider, any schema or an attr changed */
} DartEntityInfo;

/* What the core needs from the runtime, set once at init. The peer table lives in the
 * discovery core, which also holds the core's per peer lifecycle scratch. */
typedef struct {
    DartTransportState           *transport;
    DartDiscoveryState  *discovery;    /* may be NULL at init, bound later */
    uint16_t              n_topics;    /* sizes the per topic arrays */
    uint16_t              frag_size;     /* our fragment size, baked into the overlay */
    DartEventFn         on_event;      /* optional */
    void                 *user;
    DartAllocFn           alloc;         /* required, backs the schema state and the blob */
    void                 *alloc_user;
    int                   oob_capable;   /* 1 = we can deliver shared memory payloads */
    uint8_t               oob_host[16];  /* our host id, a peer is reachable iff it matches */
    uint8_t               fetch_details; /* observer mode: fetch and cache every advertised index */
} i_DartNodeCoreConfig;

typedef struct i_DartNodeCore i_DartNodeCore;

/* The arena holds only the core struct and the per topic schema registry. */
size_t          i_dart_node_core_required_memory(uint16_t n_topics);
i_DartNodeCore *i_dart_node_core_init(void *mem, size_t mem_size, const i_DartNodeCoreConfig *cfg);
/* Relocates the core. The caller re points the transport, discovery and blob after. */
i_DartNodeCore *i_dart_node_core_migrate(i_DartNodeCore *old, void *new_mem, size_t new_cap,
                                       uint16_t new_n_topics);
/* Binds the discovery core whose peer table this core delegates to, again after a migrate. */
void            i_dart_node_core_bind_discovery(i_DartNodeCore *c, DartDiscoveryState *discovery);
/* Bytes of per peer scratch the core needs in the discovery peer table. */
uint16_t        i_dart_node_core_peer_user_bytes(void);

/* The announce overlay this node sends. build rebuilds it from the core's fields and
 * returns the length, meta returns the bytes. Rebuild after a role change. */
uint16_t        i_dart_node_core_build_meta(i_DartNodeCore *c);
DartBytes       i_dart_node_core_meta(i_DartNodeCore *c);
/* Registers a topic's schema, or clears it with NULL. schema must outlive the topic. */
void            i_dart_node_core_set_topic_schema(i_DartNodeCore *c, uint16_t topic_index,
                                                    const DartSchema *schema);

/* Topic slot lifecycle. retire drops the live schema pointers but keeps the hash as the
 * slot's fingerprint, topic_rebound purges the old occupant's bindings. See spec/interest.md. */
void     i_dart_node_core_retire_topic_schema(i_DartNodeCore *c, uint16_t topic_index);
uint64_t i_dart_node_core_topic_schema_hash(i_DartNodeCore *c, uint16_t topic_index);
void     i_dart_node_core_topic_rebound(i_DartNodeCore *c, uint16_t topic_index);
/* Feeds a peer's request version to the transport's rebind hold. 1 when a held lane formed. */
int      i_dart_node_core_seen_version(i_DartNodeCore *c, uint32_t peer, uint32_t version);

/* Answers a peer's DETAIL_REQ, stateless. {NULL,0} when not answerable, the requester re
 * asks. The view is valid until the next call. */
DartBytes i_dart_node_core_detail_respond(i_DartNodeCore *c, uint16_t domain, DartBytes req);

/* Interest paging: interest_respond answers an INTEREST_REQ with one page, apply_interest_page
 * ingests one, and on completion applies the blob exactly as an inline announce would. */
DartBytes i_dart_node_core_interest_respond(i_DartNodeCore *c, uint16_t domain, DartBytes req);
void      i_dart_node_core_apply_interest_page(i_DartNodeCore *c, uint16_t domain, uint32_t peer,
                                      DartBytes resp);

/* The transport's schema_check hook. The rules are in spec/schema.md. An allowed read side
 * check also records the reader view for delivery. */
int i_dart_node_core_schema_check(i_DartNodeCore *c, uint32_t peer, uint16_t topic_index,
                                  int peer_is_pub, uint64_t hash, DartBytes wire);

/* Why the gate refused (peer, topic, direction), recorded at detail intake. NULL when
 * unknown or under DART_NO_DIAG. */
const char *i_dart_node_core_schema_why(i_DartNodeCore *c, uint32_t peer, uint16_t topic_index,
                                        int peer_is_pub);
/* Records and returns the reason for a delivery length mismatch. NULL under DART_NO_DIAG. */
const char *i_dart_node_core_note_size_mismatch(i_DartNodeCore *c, uint32_t peer,
                                        uint16_t topic_index, uint64_t got_len, uint64_t want_len);

/* The schema a delivered message decodes with: ours, a rebased view of the publisher's, the
 * publisher's own for an untyped topic, or NULL for raw. */
const DartSchema *i_dart_node_core_msg_schema(i_DartNodeCore *c, uint32_t peer, uint16_t topic_index);

/* The discovery core's on_event sink, with cfg.user = this core. Keeps the peer table and
 * the transport peer set in lockstep and fires the app's peer events. */
void i_dart_node_core_on_disc_event(const DartDiscoveryEvent *ev);

/* A resolved outbound destination. The runtime turns it into wire bytes for its link. */
typedef struct {
    uint8_t  ip[16];     /* IPv4 today */
    uint8_t  ip_len;
    uint16_t port;
} i_DartNodeDest;

/* resolve turns a peer id into an address, 1 if sendable. id_for_addr maps a source back
 * to a peer id, 1 on a hit. */
int  i_dart_node_core_resolve(i_DartNodeCore *c, uint32_t to, i_DartNodeDest *out);
int  i_dart_node_core_id_for_addr(i_DartNodeCore *c, const uint8_t ip[4], uint16_t port, uint32_t *id);

/* The requester side of the uDTL cycle: apply_details ingests a response, detail_req_next
 * builds the next queued request until 0, detail_rearm re queues every active peer. */
void   i_dart_node_core_apply_details(i_DartNodeCore *c, uint16_t domain, uint32_t peer,
                                      DartBytes resp);
int    i_dart_node_core_detail_any(i_DartNodeCore *c);
void   i_dart_node_core_detail_rearm(i_DartNodeCore *c);
size_t i_dart_node_core_detail_req_next(i_DartNodeCore *c, uint16_t domain,
                                        void *out, size_t cap, i_DartNodeDest *to);
/* Unresolved candidate matches for one topic across every active peer. 0 = converged. */
int    i_dart_node_core_topic_unresolved(i_DartNodeCore *c, uint16_t topic_index);
/* The bulk form: every topic's count in one walk of each active peer's interest. */
void   i_dart_node_core_topics_unresolved(i_DartNodeCore *c, uint16_t *counts, uint16_t n);

/* A peer's name, a view into the peer table. "unknown-peer" for a known but unnamed peer,
 * .data NULL only for an unknown id. */
DartString i_dart_node_core_peer_name(i_DartNodeCore *c, uint32_t id);

/* Read only peer table enumeration for diagnostics. Any out pointer may be NULL. */
uint16_t i_dart_node_core_max_peers(i_DartNodeCore *c);
int      i_dart_node_core_peer_at(i_DartNodeCore *c, uint16_t slot, uint32_t *id,
                                uint8_t ip[16], uint8_t *ip_len, uint16_t *port);

/* Reflection: the tables behind the runtime's walks. See spec/reflection.md. */
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
/* The create time pick for one channel (which: 0 primary, 1 rsp, 2 prg) of an entity. A
 * writer takes the widest schema every reader accepts, a reader the provider's. */
int      i_dart_node_core_reflect_pick(i_DartNodeCore *c, DartEntityKind kind, const char *name,
                              int which, int writer, const DartSchema **schema,
                              uint8_t *reliable, uint64_t *generation);

#ifdef __cplusplus
}
#endif
#endif /* DART_NODE_CORE_H */
