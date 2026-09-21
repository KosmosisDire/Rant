/* The sans-IO node core: the peer table, the discovery to transport lifecycle and
 * address resolution. A runtime owns the IO and drives it. The rules are in spec/node.md. */
#ifndef RANT_NODE_CORE_H
#define RANT_NODE_CORE_H

#include "../common/api.h"
#include "../transport/core.h"
#include "../discovery/core.h"
#include "../serialize/schema.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Formats ev as one line into buf, always NUL terminated. Returns buf. RANT_NO_DIAG
 * compiles the text out and yields "error N". */
RANT_API const char *rant_event_str(const RantEvent *ev, char *buf, size_t cap);

/* reflection types, the walks are in node/runtime.h */
typedef struct { uint32_t a, b; uint16_t c, d; } RantIter;     /* walk state, zero initialize */

#define RANT_SELF 0u     /* the peer id that means this node */

typedef struct {
    uint32_t           id;             /* stable across a drop and return, never RANT_SELF */
    uint8_t            uuid[16];       /* the process instance, a restart is a new uuid */
    RantString         name;
    RantString         address;        /* "ip:port" */
    RantPeerLiveness liveness;
    uint64_t           last_heard_us;
    uint32_t           epoch;          /* bumps on every reflected change at this peer */
    uint8_t            catching_up;    /* 1 = it advertises newer state than we hold */
    uint16_t           fragment_size;  /* its advertised UDP fragment size */
    /* the round trip as our reliable traffic measured it, samples 0 = no estimate yet */
    uint32_t         rtt_us;
    uint32_t         rtt_jitter_us;
    uint32_t         rtt_min_us;
    uint32_t         rtt_samples;
} RantPeerInfo;

typedef enum {
    RANT_ENTITY_TOPIC = 0,
    RANT_ENTITY_FUNCTION,
    RANT_ENTITY_VARIABLE,
    RANT_ENTITY_TASK
} RantEntityKind;

typedef struct {
    RantEntityKind      kind;
    RantString          name;          /* the base name, {NULL,0} until the details arrive */
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
    uint32_t            provider;      /* the ranked provider's peer id, RANT_SELF is this node */
    RantString          from;          /* the node the schemas below were read from */
    const RantSchema *schema;          /* value, request or payload, NULL = untyped or unfetched */
    uint64_t          schema_hash;
    const RantSchema *rsp_schema;      /* FUNCTION and TASK: the response */
    uint64_t          rsp_schema_hash;
    const RantSchema *progress_schema;     /* TASK: the progress channel */
    uint64_t          progress_schema_hash;
    uint64_t          generation;    /* changes iff the provider, any schema or an attr changed */
} RantEntityInfo;

/* What the core needs from the runtime, set once at init. The peer table lives in the
 * discovery core, which also holds the core's per peer lifecycle scratch. */
typedef struct {
    RantTransportState             *transport;
    RantDiscoveryState    *discovery;    /* may be NULL at init, bound later */
    uint16_t              n_topics;      /* sizes the per topic arrays */
    uint16_t              frag_size;     /* our fragment size, baked into the overlay */
    RantEventFn           on_event;      /* optional */
    void                 *user;
    RantAllocFn             alloc;         /* required, backs the schema state and the blob */
    void                 *alloc_user;
    int                   oob_capable;   /* 1 = we can deliver shared memory payloads */
    uint8_t               oob_host[16];  /* our host id, a peer is reachable iff it matches */
    uint8_t               fetch_details; /* observer mode: fetch and cache every advertised index */
} i_RantNodeCoreConfig;

typedef struct i_RantNodeCore i_RantNodeCore;

/* The arena holds only the core struct and the per topic schema registry. */
size_t          i_rant_node_core_required_memory(uint16_t n_topics);
i_RantNodeCore *i_rant_node_core_init(void *mem, size_t mem_size, const i_RantNodeCoreConfig *cfg);
/* Relocates the core. The caller re points the transport, discovery and blob after. */
i_RantNodeCore *i_rant_node_core_migrate(i_RantNodeCore *old, void *new_mem, size_t new_cap,
                                       uint16_t new_n_topics);
/* Binds the discovery core whose peer table this core delegates to, again after a migrate. */
void            i_rant_node_core_bind_discovery(i_RantNodeCore *c, RantDiscoveryState *discovery);
/* Bytes of per peer scratch the core needs in the discovery peer table. */
uint16_t        i_rant_node_core_peer_user_bytes(void);

/* The announce overlay this node sends. build rebuilds it from the core's fields and
 * returns the length, meta returns the bytes. Rebuild after a role change. */
uint16_t          i_rant_node_core_build_meta(i_RantNodeCore *c);
RantBytes         i_rant_node_core_meta(i_RantNodeCore *c);
/* Registers a topic's schema, or clears it with NULL. schema must outlive the topic. */
void            i_rant_node_core_set_topic_schema(i_RantNodeCore *c, uint16_t topic_index,
                                                    const RantSchema *schema);

/* Topic slot lifecycle. retire drops the live schema pointers but keeps the hash as the
 * slot's fingerprint, topic_rebound purges the old occupant's bindings. See spec/interest.md. */
void     i_rant_node_core_retire_topic_schema(i_RantNodeCore *c, uint16_t topic_index);
uint64_t i_rant_node_core_topic_schema_hash(i_RantNodeCore *c, uint16_t topic_index);
void     i_rant_node_core_topic_rebound(i_RantNodeCore *c, uint16_t topic_index);
/* Feeds a peer's request version to the transport's rebind hold. 1 when a held lane formed. */
int      i_rant_node_core_seen_version(i_RantNodeCore *c, uint32_t peer, uint32_t version);

/* Answers a peer's DETAIL_REQ, stateless. {NULL,0} when not answerable, the requester re
 * asks. The view is valid until the next call. */
RantBytes i_rant_node_core_detail_respond(i_RantNodeCore *c, uint16_t domain, RantBytes req);

/* Interest paging: interest_respond answers an INTEREST_REQ with one page, apply_interest_page
 * ingests one, and on completion applies the blob exactly as an inline announce would. */
RantBytes i_rant_node_core_interest_respond(i_RantNodeCore *c, uint16_t domain, RantBytes req);
void        i_rant_node_core_apply_interest_page(i_RantNodeCore *c, uint16_t domain, uint32_t peer,
                                      RantBytes resp);

/* The transport's schema_check hook. The rules are in spec/schema.md. An allowed read side
 * check also records the reader view for delivery. */
int i_rant_node_core_schema_check(i_RantNodeCore *c, uint32_t peer, uint16_t topic_index,
                                  int peer_is_pub, uint64_t hash, RantBytes wire);

/* Why the gate refused (peer, topic, direction), recorded at detail intake. NULL when
 * unknown or under RANT_NO_DIAG. */
const char *i_rant_node_core_schema_why(i_RantNodeCore *c, uint32_t peer, uint16_t topic_index,
                                        int peer_is_pub);
/* Records and returns the reason for a delivery length mismatch. NULL under RANT_NO_DIAG. */
const char *i_rant_node_core_note_size_mismatch(i_RantNodeCore *c, uint32_t peer,
                                        uint16_t topic_index, uint64_t got_len, uint64_t want_len);

/* The schema a delivered message decodes with: ours, a rebased view of the publisher's, the
 * publisher's own for an untyped topic, or NULL for raw. */
const RantSchema *i_rant_node_core_msg_schema(i_RantNodeCore *c, uint32_t peer, uint16_t topic_index);

/* Points cfg's peer hooks, error sink and user at this core. The hooks keep the peer table
 * and the transport peer set in lockstep and fire the app's peer events. */
void i_rant_node_core_discovery_hooks(i_RantNodeCore *c, RantDiscoveryCoreConfig *cfg);

/* A resolved outbound destination. The runtime turns it into wire bytes for its link. */
typedef struct {
    uint8_t  ip[16];     /* IPv4 today */
    uint8_t  ip_len;
    uint16_t port;
} i_RantNodeDest;

/* resolve turns a peer id into an address, 1 if sendable. id_for_addr maps a source back
 * to a peer id, 1 on a hit. */
int  i_rant_node_core_resolve(i_RantNodeCore *c, uint32_t to, i_RantNodeDest *out);
int  i_rant_node_core_id_for_addr(i_RantNodeCore *c, const uint8_t ip[4], uint16_t port, uint32_t *id);

/* The requester side of the uDTL cycle: apply_details ingests a response, detail_req_next
 * builds the next queued request until 0, detail_rearm re queues every active peer. */
void   i_rant_node_core_apply_details(i_RantNodeCore *c, uint16_t domain, uint32_t peer,
                                      RantBytes resp);
int    i_rant_node_core_detail_any(i_RantNodeCore *c);
void   i_rant_node_core_detail_rearm(i_RantNodeCore *c);
size_t i_rant_node_core_detail_req_next(i_RantNodeCore *c, uint16_t domain,
                                        void *out, size_t cap, i_RantNodeDest *to);
/* Unresolved candidate matches for one topic across every active peer. 0 = converged. */
int    i_rant_node_core_topic_unresolved(i_RantNodeCore *c, uint16_t topic_index);
/* The bulk form: every topic's count in one walk of each active peer's interest. */
void   i_rant_node_core_topics_unresolved(i_RantNodeCore *c, uint16_t *counts, uint16_t n);

/* A peer's name, a view into the peer table. "unknown-peer" for a known but unnamed peer,
 * .data NULL only for an unknown id. */
RantString i_rant_node_core_peer_name(i_RantNodeCore *c, uint32_t id);

/* Read only peer table enumeration for diagnostics. Any out pointer may be NULL. */
uint16_t i_rant_node_core_max_peers(i_RantNodeCore *c);
int      i_rant_node_core_peer_at(i_RantNodeCore *c, uint16_t slot, uint32_t *id,
                                uint8_t ip[16], uint8_t *ip_len, uint16_t *port);

/* Reflection: the tables behind the runtime's walks. See spec/reflection.md. */
uint16_t i_rant_node_peer_frag(const RantDiscoveryPeer *peer);
uint32_t i_rant_node_peer_interest_epoch(const RantDiscoveryPeer *peer);
int      i_rant_node_peer_interest_next(const RantDiscoveryPeer *peer,
                              RantInterestIter *it, RantTopicEntry *out);
void     i_rant_node_core_set_self_name(i_RantNodeCore *c, RantString name);
void     i_rant_node_core_self_begin(i_RantNodeCore *c);
void     i_rant_node_core_self_channel(i_RantNodeCore *c, uint16_t index, RantString name,
                              uint8_t kind, uint8_t role, uint8_t reliable, uint8_t attrs,
                              const RantSchema *schema);
void     i_rant_node_core_self_end(i_RantNodeCore *c);
int      i_rant_node_core_peers_next(i_RantNodeCore *c, RantIter *it, RantPeerInfo *out);
int      i_rant_node_core_entities_next(i_RantNodeCore *c, uint32_t peer, RantIter *it,
                              RantEntityInfo *out);
int      i_rant_node_core_mesh_next(i_RantNodeCore *c, RantIter *it, RantEntityInfo *out);
int      i_rant_node_core_mesh_find(i_RantNodeCore *c, RantEntityKind kind, const char *name,
                              RantEntityInfo *out);
uint32_t i_rant_node_core_mesh_epoch(i_RantNodeCore *c);
/* The create time pick for one channel (which: 0 primary, 1 rsp, 2 prg) of an entity. A
 * writer takes the widest schema every reader accepts, a reader the provider's. */
int      i_rant_node_core_reflect_pick(i_RantNodeCore *c, RantEntityKind kind, const char *name,
                              int which, int writer, const RantSchema **schema,
                              uint8_t *reliable, uint64_t *generation);

#ifdef __cplusplus
}
#endif
#endif /* RANT_NODE_CORE_H */
