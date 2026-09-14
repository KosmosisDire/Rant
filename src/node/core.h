/* The sans-IO node core: the peer table, the discovery to transport lifecycle and
 * address resolution. A runtime owns the IO and drives it. The rules are in spec/node.md. */
#ifndef RAMBLE_NODE_CORE_H
#define RAMBLE_NODE_CORE_H

#include "../common/api.h"
#include "../transport/core.h"
#include "../discovery/core.h"
#include "../serialize/schema.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The node's event. Four lifecycle kinds plus RAMBLE_ERROR, whose .error says which fault.
 * Read only the fields named for the kind. ramble_event_str formats any of them. */
typedef enum {
    RAMBLE_PEER_UP,        /* discovered or resumed: .peer, .ip, .ip_len, .port */
    RAMBLE_PEER_DOWN,      /* lost or fell silent: .peer */
    RAMBLE_PEER_INTEREST,  /* interest applied: .peer, .publish_topics, .receive_topics */
    RAMBLE_MSG_LOST,       /* seqnos skipped: .topic, .peer, .lost_first, .lost_count. Not an error */
    RAMBLE_ERROR           /* read .error and ramble_event_str */
} RambleEventKind;

/* The error carried by a RAMBLE_ERROR event and returned by ramble_last_error. Named RAMBLE_E_*
 * to stay distinct from the RambleResult return codes. docs/node.md lists them. */
typedef enum {
    RAMBLE_E_NONE = 0,
    /* a match was refused, or advertised data cannot flow */
    RAMBLE_E_NAME_COLLISION,   /* a peer's topic name hashes to ours but differs: .identity, .topic */
    RAMBLE_E_QOS_INCOMPATIBLE, /* a reliable subscriber met a best effort publisher: .topic, .peer */
    RAMBLE_E_KIND_MISMATCH,    /* the name is another entity kind at a peer: .topic, .peer */
    RAMBLE_E_SCHEMA_MISMATCH,  /* a refused match or an ill fitting message: .topic, .peer */
    RAMBLE_E_INTEREST_OVERFLOW,/* a peer's index map failed to allocate: .peer, .lost_count entries */
    RAMBLE_E_META_TRUNCATED_INTEREST, /* our overlay overflowed, peers see no topics */
    RAMBLE_E_META_TRUNCATED_SCHEMA,   /* retired, kept so binding enums stay aligned */
    RAMBLE_E_PEER_META_TOO_BIG,/* a peer's blob exceeds our buffer: .peer (0 = new), .too_big_bytes */
    RAMBLE_E_MSG_TOO_BIG,      /* a received message could not be buffered: .too_big_bytes */
    RAMBLE_E_PEER_REFUSED,     /* the peer table is full of active peers: .ip, .ip_len, .port */
    RAMBLE_E_EVICTED_UNSENT,   /* a send overwrote unsent history: .topic, .lost_first, .lost_count */
    RAMBLE_E_UNMATCHED_SEND,   /* a send committed to nobody while a match was resolving: .topic */
    RAMBLE_E_DUPLICATE_AUTHORITY, /* a peer also claims the handler side: .topic, .peer. Diagnostic */
    /* IO and setup, mostly at open. .os_error carries the OS code */
    RAMBLE_E_OOM,              /* the allocator returned NULL: .too_big_bytes = bytes needed */
    RAMBLE_E_PLATFORM,         /* platform net init failed */
    RAMBLE_E_SOCKET,           /* opening a UDP socket failed */
    RAMBLE_E_BIND,             /* bind failed, the port is in use: .port */
    RAMBLE_E_MCAST_JOIN,       /* joining the discovery group failed, a bad interface */
    RAMBLE_E_SEND,             /* a send hard failed: .peer, .topic, .too_big_bytes = its size */
    RAMBLE_E_RECV,             /* a receive hard failed */
    RAMBLE_E_POLL,             /* the socket wait failed */
    RAMBLE_E_WAKER,            /* no cross thread waker, wakes come at the next tick */
    RAMBLE_E_BAD_ADDRESS       /* a configured address could not be parsed, refused at open */
} RambleErrorKind;

typedef struct {
    RambleEventKind kind;
    RambleErrorKind error;       /* RAMBLE_ERROR: which error, else RAMBLE_E_NONE */
    const char *topic_name;  /* topic scoped events: our topic's name, valid for the callback */
    void       *user;          /* RambleNodeOpts.user_data */
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
} RambleEvent;
typedef void (*RambleEventFn)(const RambleEvent *ev);

/* Formats ev as one line into buf, always NUL terminated. Returns buf. RAMBLE_NO_DIAG
 * compiles the text out and yields "error N". */
RAMBLE_API const char *ramble_event_str(const RambleEvent *ev, char *buf, size_t cap);

/* reflection types, the walks are in node/runtime.h */
typedef struct { uint32_t a, b; uint16_t c, d; } RambleIter;   /* walk state, zero initialize */

#define RAMBLE_SELF 0u   /* the peer id that means this node */

typedef struct {
    uint32_t           id;             /* stable across a drop and return, never RAMBLE_SELF */
    uint8_t            uuid[16];       /* the process instance, a restart is a new uuid */
    RambleString       name;
    RambleString       address;        /* "ip:port" */
    RamblePeerLiveness liveness;
    uint64_t           last_heard_us;
    uint32_t           epoch;          /* bumps on every reflected change at this peer */
    uint8_t            catching_up;    /* 1 = it advertises newer state than we hold */
    uint16_t           fragment_size;  /* its advertised UDP fragment size */
    /* the round trip as our reliable traffic measured it, samples 0 = no estimate yet */
    uint32_t         rtt_us;
    uint32_t         rtt_jitter_us;
    uint32_t         rtt_min_us;
    uint32_t         rtt_samples;
} RamblePeerInfo;

typedef enum {
    RAMBLE_ENTITY_TOPIC = 0,
    RAMBLE_ENTITY_FUNCTION,
    RAMBLE_ENTITY_VARIABLE,
    RAMBLE_ENTITY_TASK
} RambleEntityKind;

typedef struct {
    RambleEntityKind    kind;
    RambleString        name;          /* the base name, {NULL,0} until the details arrive */
    uint32_t            hash;          /* the primary channel's low 32 name hash, the placeholder */
    uint8_t             provides;      /* someone live is on the source side */
    uint8_t             consumes;      /* someone live is on the sink side */
    uint8_t             reliable;      /* the primary channel's reliability */
    uint8_t             writable;      /* VARIABLE: a set channel is advertised */
    uint8_t             forceable;     /* VARIABLE: the owner permits force */
    uint8_t             cancellable;   /* TASK: the provider honors cancel */
    uint8_t             exclusive;     /* TASK: declared serialization */
    uint8_t             multi;         /* duplicate authority is intended */
    uint8_t             incomplete;    /* a pattern half pair, surfaced rather than dropped */
    uint8_t             conflict;      /* MESH: live schemas that cannot read each other */
    uint16_t            providers;     /* live endpoints per side, a per node walk reports 0 or 1 */
    uint16_t            consumers;
    uint32_t            provider;      /* the ranked provider's peer id, RAMBLE_SELF is this node */
    RambleString        from;          /* the node the schemas below were read from */
    const RambleSchema *schema;        /* value, request or payload, NULL = untyped or unfetched */
    uint64_t          schema_hash;
    const RambleSchema *rsp_schema;    /* FUNCTION and TASK: the response */
    uint64_t          rsp_schema_hash;
    const RambleSchema *progress_schema;   /* TASK: the progress channel */
    uint64_t          progress_schema_hash;
    uint64_t          generation;    /* changes iff the provider, any schema or an attr changed */
} RambleEntityInfo;

/* What the core needs from the runtime, set once at init. The peer table lives in the
 * discovery core, which also holds the core's per peer lifecycle scratch. */
typedef struct {
    RambleTransportState           *transport;
    RambleDiscoveryState  *discovery;    /* may be NULL at init, bound later */
    uint16_t              n_topics;      /* sizes the per topic arrays */
    uint16_t              frag_size;     /* our fragment size, baked into the overlay */
    RambleEventFn         on_event;      /* optional */
    void                 *user;
    RambleAllocFn           alloc;         /* required, backs the schema state and the blob */
    void                 *alloc_user;
    int                   oob_capable;   /* 1 = we can deliver shared memory payloads */
    uint8_t               oob_host[16];  /* our host id, a peer is reachable iff it matches */
    uint8_t               fetch_details; /* observer mode: fetch and cache every advertised index */
} i_RambleNodeCoreConfig;

typedef struct i_RambleNodeCore i_RambleNodeCore;

/* The arena holds only the core struct and the per topic schema registry. */
size_t          i_ramble_node_core_required_memory(uint16_t n_topics);
i_RambleNodeCore *i_ramble_node_core_init(void *mem, size_t mem_size, const i_RambleNodeCoreConfig *cfg);
/* Relocates the core. The caller re points the transport, discovery and blob after. */
i_RambleNodeCore *i_ramble_node_core_migrate(i_RambleNodeCore *old, void *new_mem, size_t new_cap,
                                       uint16_t new_n_topics);
/* Binds the discovery core whose peer table this core delegates to, again after a migrate. */
void            i_ramble_node_core_bind_discovery(i_RambleNodeCore *c, RambleDiscoveryState *discovery);
/* Bytes of per peer scratch the core needs in the discovery peer table. */
uint16_t        i_ramble_node_core_peer_user_bytes(void);

/* The announce overlay this node sends. build rebuilds it from the core's fields and
 * returns the length, meta returns the bytes. Rebuild after a role change. */
uint16_t          i_ramble_node_core_build_meta(i_RambleNodeCore *c);
RambleBytes       i_ramble_node_core_meta(i_RambleNodeCore *c);
/* Registers a topic's schema, or clears it with NULL. schema must outlive the topic. */
void            i_ramble_node_core_set_topic_schema(i_RambleNodeCore *c, uint16_t topic_index,
                                                    const RambleSchema *schema);

/* Topic slot lifecycle. retire drops the live schema pointers but keeps the hash as the
 * slot's fingerprint, topic_rebound purges the old occupant's bindings. See spec/interest.md. */
void     i_ramble_node_core_retire_topic_schema(i_RambleNodeCore *c, uint16_t topic_index);
uint64_t i_ramble_node_core_topic_schema_hash(i_RambleNodeCore *c, uint16_t topic_index);
void     i_ramble_node_core_topic_rebound(i_RambleNodeCore *c, uint16_t topic_index);
/* Feeds a peer's request version to the transport's rebind hold. 1 when a held lane formed. */
int      i_ramble_node_core_seen_version(i_RambleNodeCore *c, uint32_t peer, uint32_t version);

/* Answers a peer's DETAIL_REQ, stateless. {NULL,0} when not answerable, the requester re
 * asks. The view is valid until the next call. */
RambleBytes i_ramble_node_core_detail_respond(i_RambleNodeCore *c, uint16_t domain, RambleBytes req);

/* Interest paging: interest_respond answers an INTEREST_REQ with one page, apply_interest_page
 * ingests one, and on completion applies the blob exactly as an inline announce would. */
RambleBytes i_ramble_node_core_interest_respond(i_RambleNodeCore *c, uint16_t domain, RambleBytes req);
void        i_ramble_node_core_apply_interest_page(i_RambleNodeCore *c, uint16_t domain, uint32_t peer,
                                      RambleBytes resp);

/* The transport's schema_check hook. The rules are in spec/schema.md. An allowed read side
 * check also records the reader view for delivery. */
int i_ramble_node_core_schema_check(i_RambleNodeCore *c, uint32_t peer, uint16_t topic_index,
                                  int peer_is_pub, uint64_t hash, RambleBytes wire);

/* Why the gate refused (peer, topic, direction), recorded at detail intake. NULL when
 * unknown or under RAMBLE_NO_DIAG. */
const char *i_ramble_node_core_schema_why(i_RambleNodeCore *c, uint32_t peer, uint16_t topic_index,
                                        int peer_is_pub);
/* Records and returns the reason for a delivery length mismatch. NULL under RAMBLE_NO_DIAG. */
const char *i_ramble_node_core_note_size_mismatch(i_RambleNodeCore *c, uint32_t peer,
                                        uint16_t topic_index, uint64_t got_len, uint64_t want_len);

/* The schema a delivered message decodes with: ours, a rebased view of the publisher's, the
 * publisher's own for an untyped topic, or NULL for raw. */
const RambleSchema *i_ramble_node_core_msg_schema(i_RambleNodeCore *c, uint32_t peer, uint16_t topic_index);

/* The discovery core's on_event sink, with cfg.user = this core. Keeps the peer table and
 * the transport peer set in lockstep and fires the app's peer events. */
void i_ramble_node_core_on_disc_event(const RambleDiscoveryEvent *ev);

/* A resolved outbound destination. The runtime turns it into wire bytes for its link. */
typedef struct {
    uint8_t  ip[16];     /* IPv4 today */
    uint8_t  ip_len;
    uint16_t port;
} i_RambleNodeDest;

/* resolve turns a peer id into an address, 1 if sendable. id_for_addr maps a source back
 * to a peer id, 1 on a hit. */
int  i_ramble_node_core_resolve(i_RambleNodeCore *c, uint32_t to, i_RambleNodeDest *out);
int  i_ramble_node_core_id_for_addr(i_RambleNodeCore *c, const uint8_t ip[4], uint16_t port, uint32_t *id);

/* The requester side of the uDTL cycle: apply_details ingests a response, detail_req_next
 * builds the next queued request until 0, detail_rearm re queues every active peer. */
void   i_ramble_node_core_apply_details(i_RambleNodeCore *c, uint16_t domain, uint32_t peer,
                                      RambleBytes resp);
int    i_ramble_node_core_detail_any(i_RambleNodeCore *c);
void   i_ramble_node_core_detail_rearm(i_RambleNodeCore *c);
size_t i_ramble_node_core_detail_req_next(i_RambleNodeCore *c, uint16_t domain,
                                        void *out, size_t cap, i_RambleNodeDest *to);
/* Unresolved candidate matches for one topic across every active peer. 0 = converged. */
int    i_ramble_node_core_topic_unresolved(i_RambleNodeCore *c, uint16_t topic_index);
/* The bulk form: every topic's count in one walk of each active peer's interest. */
void   i_ramble_node_core_topics_unresolved(i_RambleNodeCore *c, uint16_t *counts, uint16_t n);

/* A peer's name, a view into the peer table. "unknown-peer" for a known but unnamed peer,
 * .data NULL only for an unknown id. */
RambleString i_ramble_node_core_peer_name(i_RambleNodeCore *c, uint32_t id);

/* Read only peer table enumeration for diagnostics. Any out pointer may be NULL. */
uint16_t i_ramble_node_core_max_peers(i_RambleNodeCore *c);
int      i_ramble_node_core_peer_at(i_RambleNodeCore *c, uint16_t slot, uint32_t *id,
                                uint8_t ip[16], uint8_t *ip_len, uint16_t *port);

/* Reflection: the tables behind the runtime's walks. See spec/reflection.md. */
uint16_t i_ramble_node_peer_frag(const RambleDiscoveryPeer *peer);
uint32_t i_ramble_node_peer_interest_epoch(const RambleDiscoveryPeer *peer);
int      i_ramble_node_peer_interest_next(const RambleDiscoveryPeer *peer,
                              RambleInterestIter *it, RambleTopicEntry *out);
void     i_ramble_node_core_set_self_name(i_RambleNodeCore *c, RambleString name);
void     i_ramble_node_core_self_begin(i_RambleNodeCore *c);
void     i_ramble_node_core_self_channel(i_RambleNodeCore *c, uint16_t index, RambleString name,
                              uint8_t kind, uint8_t role, uint8_t reliable, uint8_t attrs,
                              const RambleSchema *schema);
void     i_ramble_node_core_self_end(i_RambleNodeCore *c);
int      i_ramble_node_core_peers_next(i_RambleNodeCore *c, RambleIter *it, RamblePeerInfo *out);
int      i_ramble_node_core_entities_next(i_RambleNodeCore *c, uint32_t peer, RambleIter *it,
                              RambleEntityInfo *out);
int      i_ramble_node_core_mesh_next(i_RambleNodeCore *c, RambleIter *it, RambleEntityInfo *out);
int      i_ramble_node_core_mesh_find(i_RambleNodeCore *c, RambleEntityKind kind, const char *name,
                              RambleEntityInfo *out);
uint32_t i_ramble_node_core_mesh_epoch(i_RambleNodeCore *c);
/* The create time pick for one channel (which: 0 primary, 1 rsp, 2 prg) of an entity. A
 * writer takes the widest schema every reader accepts, a reader the provider's. */
int      i_ramble_node_core_reflect_pick(i_RambleNodeCore *c, RambleEntityKind kind, const char *name,
                              int which, int writer, const RambleSchema **schema,
                              uint8_t *reliable, uint64_t *generation);

#ifdef __cplusplus
}
#endif
#endif /* RAMBLE_NODE_CORE_H */
