/* GENERATED single-header build. DO NOT EDIT.
 * DART = Discovery And Realtime Transport. Amalgamated from src/ by
 * tools/pack.c. Edit the split sources in src/ and re-run pack to regenerate.
 * See the flag scheme at the top of tools/pack.c.
 */
#if defined(DART_TRANSPORT_IMPLEMENTATION) && !defined(DART_TRANSPORT_SANS_IO) && !defined(_WIN32)
  #ifndef _POSIX_C_SOURCE
  #define _POSIX_C_SOURCE 200809L
  #endif
  #ifndef _DEFAULT_SOURCE
  #define _DEFAULT_SOURCE 1
  #endif
#endif

#ifndef DART_TRANSPORT_SANS_IO
  #if defined(DART_TRANSPORT_IMPLEMENTATION) && !defined(DART_DISCOVERY_IMPLEMENTATION)
  #define DART_DISCOVERY_IMPLEMENTATION
  #endif
  #include "dart_discovery.h"   /* discovery: needed by the node runtime */
#endif

/* ===== dart_transport.h ===== */
/* sans-IO reliable-UDP transport core. Owns no socket,
 * clock, or heap: feed it datagrams plus now_us and a peer set; it returns
 * datagrams to send and delivers reassembled samples via callback. RTPS-inspired
 * but not wire-compatible. Layer dart_node.h on top for a socket-owning node. */
#ifndef DART_TRANSPORT_H
#define DART_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DART_FRAG_PAYLOAD
#define DART_FRAG_PAYLOAD 1024u          /* bytes of sample data per TU */
#endif
#define DART_DGRAM_MAX (DART_FRAG_PAYLOAD + 40u)   /* + largest header */

#ifndef DART_TOPIC_NAME_MAX
#define DART_TOPIC_NAME_MAX 64u          /* max topic-name bytes carried in the
                                            interest list; bounds meta buffers */
#endif

typedef enum { DART_BEST_EFFORT = 0, DART_RELIABLE = 1 } dart_reliability;
/* DART_NONE: declared but inactive. All resources stay allocated at init;
 * dart_set_dir flips interest at runtime. */
typedef enum { DART_PUBSUB = 0, DART_PUB_ONLY = 1, DART_SUB_ONLY = 2,
               DART_NONE = 3 } dart_direction;

/* Built-in channel carrying pub/sub interest between peers, appended to every
 * channel table. Interest lists ride it as ordinary reliable KEEP_LAST(1)
 * samples: latest list wins, late joiners get a replay, subscription changes
 * are just new samples. The id is reserved; dart_init rejects it. This is the
 * channel's local handle; its cross-peer identity is a reserved sentinel. */
#define DART_CHAN_META 0xFFFFu

typedef struct {
    dart_reliability reliability;
    uint16_t history_depth;     /* KEEP_LAST depth in samples (>=1)        */
    uint16_t join_replay;       /* reliable: cached samples replayed to a late-
                                   joining reader (capped at what history still
                                   holds). 0 (default) = join at the head and
                                   see only future samples; 1 = latest-value on
                                   join. Deep replays burst-multiply at startup
                                   (every joiner gets depth x sample_bytes at
                                   once), so keep this small.               */
    uint32_t max_sample_bytes;  /* largest sample on this channel          */
    uint32_t heartbeat_us;      /* reliable: writer heartbeat cadence      */
    uint32_t nack_delay_us;     /* reader: delay before NACK (suppression) */
    uint32_t max_block_us;      /* reliable: how long a send may wait for slow
                                   readers to ack before overwriting un-acked
                                   history. 0 (default) = pure KEEP_LAST. Worst
                                   case is min(max_block_us, peer timeout), since
                                   a dead reader's proxy also releases pressure.
                                   sans-IO callers wait via dart_send_would_evict. */
} dart_qos;

typedef struct {
    uint16_t channel_id;  /* LOCAL handle for the API (dart_send, on_sample, ...).
                             Need not match across peers; the cross-peer identity
                             is the name (or, if name is NULL, this id). */
    const char *name;     /* topic name: the cross-peer identity. Peers match on
                             its 64-bit hash and carry the name to detect a hash
                             collision (dart_collision_fn). NULL = identify by
                             channel_id instead (back-compat). <= DART_TOPIC_NAME_MAX
                             bytes. Use names consistently across all nodes. */
    dart_qos   qos;
    uint8_t  dir;     /* dart_direction; 0 (zero-init) = publish + subscribe   */
    uint8_t  mcast;   /* 1 = this channel uses its multicast group for data.
                         Publisher opt-in: a flagged writer sends new data and
                         heartbeats to the group whenever >=1 subscriber is
                         matched (local subscribers included). A flagged reader
                         joins the group, so it also hears unicast writers of the
                         same topic: multicast and unicast publishers can mix.
                         Repairs, acks, and gaps stay unicast. Reliable mcast is
                         reliable-from-join-point: history replay on join is
                         unicast-only. The group is 239.255.<domain&255>.<id&255>
                         where id is the topic's 64-bit identity. */
} dart_channel_def;

/* dart_poll_send destination: peer id, or a multicast group flagged by the top
 * bit (peer ids are small and nonzero). The low byte is the group selector
 * (topic identity & 0xFF); the node maps it to 239.255.<domain&255>.<selector>. */
#define DART_DEST_GROUP_BIT      0x80000000u
#define DART_DEST_GROUP(sel)     (DART_DEST_GROUP_BIT | (uint32_t)(sel))
#define DART_DEST_IS_GROUP(d)    (((d) & DART_DEST_GROUP_BIT) != 0u)
#define DART_DEST_GROUP_CHAN(d)  ((uint16_t)((d) & 0xFFFFu))

/* Complete sample delivered. Do not call back into dart_* for this state. */
typedef void (*dart_sample_fn)(void *user, uint16_t channel_id, uint32_t from_peer,
                             const void *data, size_t len);

/* Reader permanently skips TUs from a peer: writer superseded them (KEEP_LAST
 * eviction) or best-effort data was lost. first..first+count-1 are TU seqnos.
 * Not called for the initial join position: a late joiner adopting the writer's
 * head has lost nothing it expected. Same reentrancy rule as dart_sample_fn. */
typedef void (*dart_gap_fn)(void *user, uint16_t channel_id, uint32_t from_peer,
                          uint64_t first_tu, uint64_t count);

/* Optional: a peer advertised interest in a topic whose name hashes to the same
 * 64-bit identity as one of ours but whose name differs (a hash collision). The
 * match is refused (never silently cross-wired); this reports it for logging.
 * peer_name is the raw wire bytes (not NUL-terminated); our_name is. */
typedef void (*dart_collision_fn)(void *user, uint64_t identity,
                                  const char *our_name,
                                  const char *peer_name, size_t peer_name_len);

typedef struct {
    const dart_channel_def *channels;
    uint16_t              n_channels;
    uint16_t              max_peers;
    uint16_t              meta_max_ids; /* largest pub+sub topic count accepted in
                                           a peer's interest list. 0 = 1024. Sizes
                                           the meta channel buffers; with named
                                           topics each entry is up to
                                           9+DART_TOPIC_NAME_MAX bytes, so tune
                                           this down on small targets. */
    dart_sample_fn          on_sample;
    dart_gap_fn             on_gap;     /* optional; NULL = no gap reporting */
    dart_collision_fn       on_collision; /* optional; NULL = silent refuse */
    void                 *user;
} dart_config;

typedef struct dart_state dart_state;

size_t    dart_required_memory(const dart_config *cfg);
dart_state *dart_init(void *mem, size_t mem_size, const dart_config *cfg);

/* Canonical 64-bit topic identity from a name (FNV-1a). Used to match topics
 * across peers and to derive their multicast group. */
uint64_t  dart_topic_id(const char *name);
/* A channel def's identity: dart_topic_id(name), or channel_id if name is NULL.
 * Core and node both use this so they agree on matching and group address. */
uint64_t  dart_channel_identity(const dart_channel_def *def);

/* A new peer matches only the meta channel; data channels match as its
 * interest list arrives over it, and rematch on every change (theirs via new
 * lists, ours via dart_set_dir). peer_is_local: 1 if on this host; local
 * subscribers of mcast channels stay unicast, and the group lane engages only
 * with a remote one. */
void      dart_peer_add   (dart_state *st, uint32_t peer_id, int peer_is_local);
void      dart_peer_remove(dart_state *st, uint32_t peer_id);

/* Change our own interest in a channel at runtime. Creates/destroys proxies
 * against every live peer and announces the new list on the meta channel.
 * A (re)subscribe joins like a late joiner: reliable channels replay cached
 * history, nothing is reported as a gap. Returns 0 ok, <0 unknown channel. */
int       dart_set_dir(dart_state *st, uint16_t channel_id, uint8_t dir);

/* Publish a sample to all peers. Returns 0 ok, <0 on error. */
int       dart_send(dart_state *st, uint16_t channel_id, const void *data, size_t len,
                  uint64_t now_us);

/* The channel's qos as stored at init; NULL if the channel is unknown. */
const dart_qos *dart_channel_qos(dart_state *st, uint16_t channel_id);

/* Backpressure signal. 1 if appending a sample on this reliable channel would
 * overwrite history not yet acked by every live reader; 0 otherwise. A writer
 * accepting pressure pumps its loop while this returns 1, then sends anyway
 * after qos.max_block_us (KEEP_LAST eviction is the fallback, never refusal). */
int       dart_send_would_evict(dart_state *st, uint16_t channel_id);

/* Feed a received datagram, tagged with the peer it came from. */
void      dart_on_datagram(dart_state *st, uint32_t from_peer, const void *dg, size_t len,
                         uint64_t now_us);

/* Pull one outgoing datagram (may batch several submessages for one peer).
 * Returns 1 and fills *to_peer/out/out_len, or 0 when nothing is due. Loop until
 * 0. Pass DART_DGRAM_MAX of cap; smaller caps just batch less. */
int       dart_poll_send(dart_state *st, uint32_t *to_peer, void *out, size_t cap,
                       size_t *out_len, uint64_t now_us);

#ifdef __cplusplus
}
#endif
#endif /* DART_TRANSPORT_H */

#ifndef DART_TRANSPORT_SANS_IO
/* ===== dart_node.h ===== */
/* NODE runtime over the dart_transport core: owns the data socket,
 * drives discovery, wires peers into the transport. */
#ifndef DART_NODE_H
#define DART_NODE_H


#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t              domain_id;     /* discovery domain                  */
    uint16_t              data_port;     /* unicast data port; 0 = OS-assigned */
    const char           *disc_group;    /* default "239.255.0.7"             */
    uint16_t              disc_port;     /* default 7400                      */
    uint16_t              mc_port;       /* shared multicast DATA port; default
                                            disc_port+1. Per-channel group is
                                            239.255.<domain&255>.<chan&255>   */
    const char           *mcast_if;      /* interface IP for all multicast;
                                            NULL = auto. "127.0.0.1" keeps a
                                            single-host run off the network.
                                            Pin this on multihomed hosts: the
                                            auto route probe follows whatever
                                            the OS routes 239.x to (VPN, WSL,
                                            docker bridges all candidates)    */
    const dart_discovery_addr *seeds;    /* initial peers: announces are also
                                            unicast here (port 0 = disc_port),
                                            so discovery works where multicast
                                            is filtered or flaky              */
    uint16_t              n_seeds;
    uint32_t              announce_us;   /* default 1s                        */
    uint32_t              timeout_us;    /* default 3.5s                      */
    uint8_t               ttl;           /* multicast TTL, default 1          */
    uint16_t              max_peers;     /* default 16                        */
    uint32_t              so_rcvbuf;     /* data-socket SO_RCVBUF bytes; 0 = OS
                                            default. Bigger absorbs bursts at
                                            the cost of queuing delay         */
    uint32_t              so_sndbuf;     /* data-socket SO_SNDBUF; 0 = default */
    const dart_channel_def *channels;      /* transport channels                */
    uint16_t              n_channels;
    uint16_t              meta_max_ids;  /* largest peer interest list accepted
                                            (pub+sub ids); 0 = 1024. Sizes the
                                            meta channel buffers: tune down on
                                            small targets                      */
    dart_sample_fn          on_sample;     /* sample delivery                   */
    dart_gap_fn             on_gap;        /* optional: permanently skipped TUs */
    dart_collision_fn       on_collision;  /* optional: two topic names hashed to
                                              one identity; the match is refused */
    void                 *user;
} dart_node_config;

typedef struct dart_node dart_node;

size_t   dart_node_required_memory(const dart_node_config *cfg);
dart_node *dart_node_open(void *mem, size_t mem_size, const dart_node_config *cfg);
int      dart_node_poll(dart_node *n, int timeout_ms);          /* one loop tick   */
int      dart_node_send(dart_node *n, uint16_t channel_id, const void *data, size_t len);
/* Change our interest in a channel at runtime (dart_direction; DART_NONE =
 * inactive). Peers rematch as the change reaches them; a (re)subscribe joins
 * like a late joiner. Returns 0 ok, <0 unknown channel. */
int      dart_node_set_dir(dart_node *n, uint16_t channel_id, uint8_t dir);
/* Cumulative backpressure since open: microseconds dart_node_send waited on
 * slow readers and how many sends waited. Either out-pointer may be NULL. */
void     dart_node_block_stats(dart_node *n, uint64_t *block_us, uint32_t *blocked_sends);
void     dart_node_close(dart_node *n, int send_bye);

#ifdef __cplusplus
}
#endif
#endif /* DART_NODE_H */
#endif /* !DART_TRANSPORT_SANS_IO */

#ifdef DART_TRANSPORT_IMPLEMENTATION
/* ===== dart_transport.c ===== */
/* sans-IO reliable-UDP transport core. See dart_transport.h. */
#include <string.h>

/* byte 0 of every submessage: message type in the low 3 bits, flags above */
#define DART_DATA 1
#define DART_HB   2
#define DART_NACK 3
#define DART_GAP  4
#define DART_MSG_MASK 0x07u     /* type = byte0 & mask                       */
#define DART_F_SINGLE 0x08u     /* DATA: single fragment (frag/count/len omitted) */
#define DART_F_UNPOS  0x10u     /* NACK: reader has delivered nothing yet     */

#define DART_NACK_WINDOW 32u    /* seqnos covered by one ACKNACK bitmap */
#define DART__NIL 0xFFFFFFFFu
/* reserved identity of the built-in meta channel; dart_init rejects a user
 * channel that hashes to it (astronomically unlikely, but never silent) */
#define DART_META_IDENTITY 0xFFFFFFFFFFFFFFFFull

#ifndef DART_HB_SWEEP_US
#define DART_HB_SWEEP_US 25000u /* the timer sweep covers every lane this often */
#endif

/* little-endian pack helpers */
static void dart_w16(uint8_t*p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void dart_w32(uint8_t*p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}
static void dart_w64(uint8_t*p,uint64_t v){int i;for(i=0;i<8;i++)p[i]=(uint8_t)(v>>(8*i));}
static uint16_t dart_r16(const uint8_t*p){return (uint16_t)(p[0]|((uint16_t)p[1]<<8));}
static uint32_t dart_r32(const uint8_t*p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t dart_r64(const uint8_t*p){uint64_t v=0;int i;for(i=0;i<8;i++)v|=((uint64_t)p[i])<<(8*i);return v;}

static void dart_bset(uint8_t*bm,uint32_t i){bm[i>>3]|=(uint8_t)(1u<<(i&7));}
static int  dart_bget(const uint8_t*bm,uint32_t i){return (bm[i>>3]>>(i&7))&1;}

uint64_t dart_topic_id(const char *name){
    uint64_t h = 1469598103934665603ull;   /* FNV-1a 64 offset basis */
    const unsigned char *p = (const unsigned char*)name;
    if (!name) return 0;
    for (; *p; p++){ h ^= (uint64_t)*p; h *= 1099511628211ull; }
    return h;
}
uint64_t dart_channel_identity(const dart_channel_def *def){
    return (def->name && def->name[0]) ? dart_topic_id(def->name)
                                       : (uint64_t)def->channel_id;
}
static size_t dart__namelen(const char *s){            /* capped strlen */
    size_t n = 0;
    if (s) while (s[n] && n < DART_TOPIC_NAME_MAX) n++;
    return n;
}

/* internal structures */
typedef struct {
    uint8_t  valid;
    uint64_t base;       /* seqno of frag 0 */
    uint16_t count;      /* frag count      */
    uint32_t len;        /* sample bytes    */
    uint8_t *buf;        /* len bytes        */
} dart_wsample;

typedef struct {        /* writer-side, per (channel,peer) */
    uint8_t  used;
    uint32_t reader_epoch; /* reader incarnation last seen in an ACKNACK; 0 =
                              none yet. A change means the peer rebuilt its
                              state (e.g. one-sided discovery flap): our acked/
                              sent positions describe a dead reader, so the
                              lane re-joins as if freshly matched. */
    uint64_t sent_upto;  /* next seqno to push as new data  */
    uint64_t acked_upto; /* peer received all TUs < this    */
    /* pending repair request (from ACKNACK) */
    uint8_t  has_nack;
    uint64_t nack_base;
    uint32_t nack_bits;
    /* heartbeat timer */
    uint64_t hb_next_us;
    uint32_t hb_count;
} dart_wproxy;

typedef struct {        /* reader-side, per (channel,peer) */
    uint8_t  used;
    uint8_t  started;       /* accepted any DATA from this writer yet          */
    uint32_t epoch;         /* this incarnation's id, sent in every ACKNACK    */
    uint64_t deliver_upto;  /* base of current sample; all below delivered/skipped */
    uint8_t  asm_active;    /* received >=1 frag of current sample */
    uint16_t asm_count;
    uint32_t asm_len;
    uint8_t *asm_buf;       /* max_sample_bytes */
    uint8_t *frag_bm;       /* ceil(maxfrags/8) */
    uint64_t hb_last;       /* highest seqno writer claims to hold */
    uint8_t  ack_pending;
    uint64_t ack_due_us;
} dart_rproxy;

typedef struct {
    dart_qos    qos;
    uint64_t  identity;     /* cross-peer topic identity (hash of name or id) */
    const char *name;       /* our copy of the topic name; NULL if id-identified */
    uint16_t  id;           /* local handle (API + on_sample) */
    uint16_t  maxfrags;     /* ceil(max_sample_bytes/FRAG) */
    uint8_t   dir;          /* dart_direction */
    uint8_t   mcast;
    uint16_t  nsubs;        /* live matched subscribers; mcast: >0 = group mode */
    /* writer */
    dart_wsample *hist;       /* [depth] ring */
    uint16_t  hist_head;    /* next slot to overwrite */
    uint64_t  next_seqno;
    uint64_t  first_seqno;  /* lowest seqno still cached */
    uint8_t   have_first;
    /* multicast group lane: new data emitted once for all subscribers */
    uint64_t  mc_sent_upto;
    uint64_t  mc_hb_next_us;
    uint32_t  mc_hb_count;
} dart_channel;

struct dart_state {
    dart_config    cfg;       /* n_channels here includes the meta channel */
    uint32_t    *peer_ids;  /* [max_peers] */
    uint8_t     *peer_used; /* [max_peers] */
    uint8_t     *peer_local;/* [max_peers] */
    /* peer interest over OUR channel table, bit per user channel index; filled
       from meta samples. The proxies plus these bits are the whole stored
       interest: full peer lists are never kept. */
    uint8_t     *peer_pub_bm; /* [max_peers][bmlen] peer publishes channel c  */
    uint8_t     *peer_sub_bm; /* [max_peers][bmlen] peer subscribes channel c */
    uint16_t     bmlen;       /* ceil(user n_channels / 8) */
    uint16_t     meta_ci;     /* channel index of the built-in meta channel */
    /* per-peer wire alias -> our channel index, built from interest lists. The
       data path carries a 2-byte alias (the sender's channel index) instead of
       the 8-byte identity; we translate (peer, alias) back to our channel. */
    uint16_t    *alias_ci;    /* [max_peers * amax]; 0xFFFF = unmapped */
    uint32_t     amax;        /* alias-table stride = effective meta_max_ids */
    dart_channel  *chans;     /* [n_channels] */
    dart_wproxy   *wprox;     /* [n_channels*max_peers] */
    dart_rproxy   *rprox;     /* [n_channels*max_peers] */
    /* active-lane scheduler. A lane is one (channel, peer) proxy pair, or a
       channel's multicast group lane; the event that gives a lane sendable
       work also enqueues it, so poll_send pays for work done, never for idle
       lanes. Lanes queue at most once, grouped per destination so one pop
       drains one datagram's worth. Timer work (heartbeats, delayed acks) has
       no event to ride and is found by an amortized clock-driven sweep. */
    uint32_t    *lane_next;   /* [n_channels*(max_peers+1)] next in dest list */
    uint8_t     *lane_inq;    /* [n_channels*(max_peers+1)] queued flag */
    uint32_t    *dest_head;   /* [max_peers+n_channels] lane list per dest */
    uint32_t    *dest_tail;
    uint8_t     *dest_inq;
    uint32_t    *destq;       /* ring of active destinations */
    uint32_t     destq_head, destq_n;
    uint32_t     sweep;       /* timer-sweep lane cursor */
    uint64_t     sweep_t;     /* clock position the sweep has paid for */
    uint32_t     repoch;      /* reader-epoch counter (starts at 1; 0 = none) */
};

/* bump allocator (shared by required_memory and init) */
typedef struct { uint8_t *base; size_t off; size_t cap; int oom; } dart_bump;
static void *dart_take(dart_bump *b, size_t n, size_t align){
    size_t a = (b->off + (align-1)) & ~(align-1);
    b->off = a + n;
    if (b->base){
        if (b->off > b->cap){ b->oom = 1; return NULL; }
        return b->base + a;
    }
    return NULL; /* sizing mode */
}

static uint16_t dart_maxfrags(uint32_t max_sample_bytes){
    uint32_t f = (max_sample_bytes + DART_FRAG_PAYLOAD - 1) / DART_FRAG_PAYLOAD;
    if (f == 0) f = 1;
    return (uint16_t)f;
}

/* Lay out everything; b->base==NULL means measure only. Returns state ptr.
 * Appends the built-in meta channel after the user table. */
static dart_state *dart_build(dart_bump *b, const dart_config *cfg){
    uint16_t c, p; uint32_t np = cfg->max_peers, ncu = cfg->n_channels, nc = ncu+1u;
    uint16_t bml = (uint16_t)((ncu+7u)/8u);
    uint32_t mids = cfg->meta_max_ids ? cfg->meta_max_ids : 1024u;
    uint32_t name_bytes = 0, name_max = 0, entry_max;
    char *npool = NULL; uint32_t ncur = 0;
    dart_channel_def metadef;
    dart_state *st = (dart_state*)dart_take(b, sizeof(dart_state), 16);
    if (st && b->base) memset(st, 0, sizeof(*st));

    /* name pool (our copies) and the per-entry name budget for meta sizing:
       a named system carries names up to DART_TOPIC_NAME_MAX, so accept that
       much from peers; a purely id-identified table pays nothing for names. */
    for (c=0;c<ncu;c++){
        size_t L = dart__namelen(cfg->channels[c].name);
        if (L){ name_bytes += (uint32_t)L + 1u; name_max = DART_TOPIC_NAME_MAX; }
    }
    entry_max = 2u + 8u + 1u + name_max;   /* [u16 alias][u64 id][u8 len][name...] */

    if (mids < 2u*ncu) mids = 2u*ncu;     /* our own list must always fit */
    memset(&metadef, 0, sizeof metadef);
    metadef.channel_id          = DART_CHAN_META;
    metadef.qos.reliability     = DART_RELIABLE;
    metadef.qos.history_depth   = 1;       /* latest interest list wins */
    metadef.qos.join_replay     = 1;       /* joiners get the current list */
    metadef.qos.max_sample_bytes= 4u + entry_max*mids;

    { uint32_t nlanes = nc*(np+1u), ndest = np+nc;
      uint32_t *pi = (uint32_t*)dart_take(b, np*sizeof(uint32_t), 8);
      uint8_t  *pu = (uint8_t*) dart_take(b, np*sizeof(uint8_t), 1);
      uint8_t  *pl = (uint8_t*) dart_take(b, np*sizeof(uint8_t), 1);
      uint8_t  *pb = (uint8_t*) dart_take(b, (size_t)np*bml, 1);
      uint8_t  *sb = (uint8_t*) dart_take(b, (size_t)np*bml, 1);
      dart_channel *ch = (dart_channel*)dart_take(b, nc*sizeof(dart_channel), 16);
      dart_wproxy *wp = (dart_wproxy*)dart_take(b, (size_t)nc*np*sizeof(dart_wproxy), 16);
      dart_rproxy *rp = (dart_rproxy*)dart_take(b, (size_t)nc*np*sizeof(dart_rproxy), 16);
      uint32_t *ln = (uint32_t*)dart_take(b, (size_t)nlanes*sizeof(uint32_t), 8);
      uint8_t  *li = (uint8_t*) dart_take(b, (size_t)nlanes, 1);
      uint32_t *dh = (uint32_t*)dart_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint32_t *dt = (uint32_t*)dart_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint8_t  *di = (uint8_t*) dart_take(b, (size_t)ndest, 1);
      uint32_t *dq = (uint32_t*)dart_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint16_t *ac = (uint16_t*)dart_take(b, (size_t)np*mids*sizeof(uint16_t), 2);
      npool = (char*)dart_take(b, name_bytes ? name_bytes : 1u, 1);
      if (st && b->base){
          st->cfg=*cfg; st->peer_ids=pi; st->peer_used=pu; st->peer_local=pl;
          st->peer_pub_bm=pb; st->peer_sub_bm=sb; st->bmlen=bml;
          st->cfg.n_channels=(uint16_t)nc; st->meta_ci=(uint16_t)ncu;
          st->chans=ch; st->wprox=wp; st->rprox=rp; st->repoch=1;
          st->lane_next=ln; st->lane_inq=li;
          st->dest_head=dh; st->dest_tail=dt; st->dest_inq=di; st->destq=dq;
          st->alias_ci=ac; st->amax=mids;
          memset(pu,0,np); memset(pl,0,np);
          memset(ac,0xFF,(size_t)np*mids*sizeof(uint16_t));   /* all unmapped */
          memset(pb,0,(size_t)np*bml); memset(sb,0,(size_t)np*bml);
          memset(wp,0,(size_t)nc*np*sizeof(dart_wproxy));
          memset(rp,0,(size_t)nc*np*sizeof(dart_rproxy));
          memset(li,0,nlanes); memset(di,0,ndest);
          memset(dh,0xFF,(size_t)ndest*sizeof(uint32_t));   /* all DART__NIL */
      }
    }

    for (c=0;c<nc;c++){
        const dart_channel_def *def = (c < ncu) ? &cfg->channels[c] : &metadef;
        const dart_qos *q = &def->qos;
        uint16_t depth = q->history_depth ? q->history_depth : 1;
        uint16_t mf = dart_maxfrags(q->max_sample_bytes);
        dart_wsample *hist = (dart_wsample*)dart_take(b, depth*sizeof(dart_wsample), 16);
        uint16_t d;
        if (st && b->base){
            dart_channel *ch = &st->chans[c];
            memset(ch,0,sizeof(*ch));
            ch->qos=*q; ch->id=def->channel_id; ch->maxfrags=mf;
            ch->dir=def->dir; ch->mcast=def->mcast;
            ch->identity = (c < ncu) ? dart_channel_identity(def) : DART_META_IDENTITY;
            ch->name = NULL;
            if (c < ncu){
                size_t L = dart__namelen(def->name);
                if (L){ char *dst = npool + ncur;
                        memcpy(dst, def->name, L); dst[L]='\0';
                        ch->name = dst; ncur += (uint32_t)(L + 1u); }
            }
            ch->hist=hist; ch->hist_head=0; ch->next_seqno=0; ch->have_first=0;
            memset(hist,0,depth*sizeof(dart_wsample));
        }
        for (d=0; d<depth; d++){
            uint8_t *buf = (uint8_t*)dart_take(b, q->max_sample_bytes, 8);
            if (st && b->base) st->chans[c].hist[d].buf = buf;
        }
        /* reader asm buffers and frag bitmaps, per peer */
        for (p=0;p<np;p++){
            uint8_t *abuf = (uint8_t*)dart_take(b, q->max_sample_bytes, 8);
            uint8_t *fbm  = (uint8_t*)dart_take(b, (mf+7u)/8u, 1);
            if (st && b->base){
                dart_rproxy *r = &st->rprox[(size_t)c*np+p];
                r->asm_buf=abuf; r->frag_bm=fbm;
            }
        }
    }
    return st;
}

size_t dart_required_memory(const dart_config *cfg){
    dart_bump b; memset(&b,0,sizeof b);
    if (!cfg || cfg->n_channels==0 || cfg->max_peers==0) return 0;
    dart_build(&b, cfg);
    return b.off + 16;   /* slack for base alignment */
}

static void dart__meta_publish(dart_state *st);

dart_state *dart_init(void *mem, size_t cap, const dart_config *cfg){
    dart_bump b; dart_state *st; uint16_t i;
    if (!mem || !cfg || cfg->n_channels==0 || cfg->max_peers==0) return NULL;
    for (i=0;i<cfg->n_channels;i++){
        if (cfg->channels[i].channel_id==DART_CHAN_META) return NULL;  /* reserved handle */
        if (dart_channel_identity(&cfg->channels[i])==DART_META_IDENTITY) return NULL; /* reserved identity */
    }
    memset(&b,0,sizeof b);
    b.base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    b.cap  = cap - (size_t)((uint8_t*)b.base - (uint8_t*)mem);
    st = dart_build(&b, cfg);
    if (!st || b.oom) return NULL;
    /* cfg.channels still points at caller memory; only read during init,
       so detach it now. */
    st->cfg.channels = NULL;
    dart__meta_publish(st);    /* late joiners replay this initial list */
    return st;
}

/* helpers */
static int dart_peer_slot(dart_state *st, uint32_t id){
    uint16_t i;
    for (i=0;i<st->cfg.max_peers;i++) if (st->peer_used[i] && st->peer_ids[i]==id) return (int)i;
    return -1;
}
static dart_channel *dart_chan(dart_state *st, uint16_t id, int *idx_out){
    uint16_t i;
    for (i=0;i<st->cfg.n_channels;i++) if (st->chans[i].id==id){ if(idx_out)*idx_out=(int)i; return &st->chans[i]; }
    return NULL;
}
/* RX demux: find the local channel for a wire identity (the cross-peer key). */
static dart_channel *dart_chan_by_identity(dart_state *st, uint64_t identity, int *idx_out){
    uint16_t i;
    for (i=0;i<st->cfg.n_channels;i++) if (st->chans[i].identity==identity){ if(idx_out)*idx_out=(int)i; return &st->chans[i]; }
    return NULL;
}

/* active-lane scheduler. Lane index = ci*(max_peers+1)+ps; ps==max_peers is
 * the channel's multicast group lane. Destination = peer slot ps, or
 * max_peers+ci for a group lane. */
static void dart__dest_push(dart_state *st, uint32_t d){
    uint32_t ndest = st->cfg.max_peers + (uint32_t)st->cfg.n_channels, t;
    if (st->dest_inq[d]) return;
    st->dest_inq[d]=1;
    t = st->destq_head + st->destq_n;
    if (t >= ndest) t -= ndest;
    st->destq[t]=d; st->destq_n++;
}

/* enqueue a lane that just got sendable work; idempotent while queued */
static void dart__lane_wake(dart_state *st, uint16_t ci, uint32_t ps){
    uint32_t np=st->cfg.max_peers, lanes=np+1u;
    uint32_t L=(uint32_t)ci*lanes+ps;
    uint32_t d=(ps<np) ? ps : np+(uint32_t)ci;
    if (st->lane_inq[L]) return;
    st->lane_inq[L]=1; st->lane_next[L]=DART__NIL;
    if (st->dest_head[d]==DART__NIL) st->dest_head[d]=L;
    else st->lane_next[st->dest_tail[d]]=L;
    st->dest_tail[d]=L;
    dart__dest_push(st, d);
}

/* unicast join seqno: the head minus qos.join_replay cached samples (reliable
 * only). Best-effort and join_replay 0 start at the head, so a late joiner
 * sees only future samples. */
static uint64_t dart_unicast_join_seqno(const dart_channel *chn){
    uint16_t depth = chn->qos.history_depth ? chn->qos.history_depth : 1;
    uint16_t want = chn->qos.join_replay, k, i;
    uint64_t s = chn->next_seqno;
    if (chn->qos.reliability != DART_RELIABLE || want == 0) return s;
    if (want > depth) want = depth;
    i = chn->hist_head;
    for (k=0; k<want; k++){
        uint16_t j = (uint16_t)(i ? i-1 : depth-1);
        if (!chn->hist[j].valid) break;        /* fewer than want cached */
        s = chn->hist[j].base;
        i = j;
    }
    return s;
}

/* per-channel match/unmatch: create or destroy one proxy, keeping the mcast
 * subscriber accounting and join-seqno rules in one place. Publisher opt-in,
 * always-group: a mcast channel engages its group lane as soon as it has any
 * subscriber (local included); per-peer lanes then carry repairs only. */
static void dart__match_w(dart_state *st, uint16_t c, uint16_t ps){
    dart_channel *chn=&st->chans[c];
    dart_wproxy *w=&st->wprox[(size_t)c*st->cfg.max_peers+ps];
    memset(w,0,sizeof(*w));
    w->used=1;
    if (!chn->mcast){
        w->sent_upto = dart_unicast_join_seqno(chn);
    } else {
        /* first subscriber engages group mode: the group cursor takes over at
           the current head and carries all new data. Reliable-from-join-point,
           so this and later joiners start there and backfill via NACK. */
        if (chn->nsubs==0) chn->mc_sent_upto = chn->next_seqno;
        chn->nsubs++;
        w->sent_upto = chn->mc_sent_upto;
    }
    w->acked_upto = w->sent_upto;
    dart__lane_wake(st, c, ps);   /* unicast repair lane primed + ack/hb */
}
static void dart__unmatch_w(dart_state *st, uint16_t c, uint16_t ps){
    dart_channel *chn=&st->chans[c];
    dart_wproxy *w=&st->wprox[(size_t)c*st->cfg.max_peers+ps];
    if (!w->used) return;
    if (chn->mcast && chn->nsubs) chn->nsubs--;
    w->used=0;
}
static void dart__match_r(dart_state *st, uint16_t c, uint16_t ps){
    dart_rproxy *r=&st->rprox[(size_t)c*st->cfg.max_peers+ps];
    uint8_t *abuf=r->asm_buf, *fbm=r->frag_bm;
    memset(r,0,sizeof(*r));
    r->asm_buf=abuf; r->frag_bm=fbm;
    r->epoch=st->repoch++;   /* new incarnation: writers re-join on seeing it */
    r->used=1;       /* started==0: first DATA adopts the writer's position */
}
static void dart__unmatch_r(dart_state *st, uint16_t c, uint16_t ps){
    dart_rproxy *r=&st->rprox[(size_t)c*st->cfg.max_peers+ps];
    r->used=0; r->asm_active=0;
}

/* recompute one (channel, peer) match from our dir and the peer's interest
 * bits; transition only on change so live streams never churn */
static void dart__rematch(dart_state *st, uint16_t c, uint16_t ps){
    dart_channel *chn=&st->chans[c];
    const uint8_t *pb=&st->peer_pub_bm[(size_t)ps*st->bmlen];
    const uint8_t *sb=&st->peer_sub_bm[(size_t)ps*st->bmlen];
    int wuse = (chn->dir==DART_PUBSUB || chn->dir==DART_PUB_ONLY) && dart_bget(sb,c);
    int ruse = (chn->dir==DART_PUBSUB || chn->dir==DART_SUB_ONLY) && dart_bget(pb,c);
    dart_wproxy *w=&st->wprox[(size_t)c*st->cfg.max_peers+ps];
    dart_rproxy *r=&st->rprox[(size_t)c*st->cfg.max_peers+ps];
    if (wuse && !w->used) dart__match_w(st,c,ps);
    else if (!wuse && w->used) dart__unmatch_w(st,c,ps);
    if (ruse && !r->used) dart__match_r(st,c,ps);
    else if (!ruse && r->used) dart__unmatch_r(st,c,ps);
}

void dart_peer_add(dart_state *st, uint32_t id, int peer_is_local){
    uint16_t i; int free=-1; uint32_t np=st->cfg.max_peers;
    if (dart_peer_slot(st,id)>=0) return;
    for (i=0;i<np;i++) if(!st->peer_used[i]){free=(int)i;break;}
    if (free<0) return;
    st->peer_used[free]=1; st->peer_ids[free]=id;
    st->peer_local[free]=(uint8_t)(peer_is_local?1:0);
    memset(&st->peer_pub_bm[(size_t)free*st->bmlen],0,st->bmlen);
    memset(&st->peer_sub_bm[(size_t)free*st->bmlen],0,st->bmlen);
    memset(&st->alias_ci[(size_t)free*st->amax],0xFF,(size_t)st->amax*sizeof(uint16_t));
    /* only the meta channel matches up front; everything else waits for the
       peer's interest list to arrive over it */
    dart__match_w(st, st->meta_ci, (uint16_t)free);
    dart__match_r(st, st->meta_ci, (uint16_t)free);
}

void dart_peer_remove(dart_state *st, uint32_t id){
    int s = dart_peer_slot(st,id); uint16_t c;
    if (s<0) return;
    for (c=0;c<st->cfg.n_channels;c++){
        dart__unmatch_w(st,c,(uint16_t)s);
        dart__unmatch_r(st,c,(uint16_t)s);
    }
    st->peer_used[s]=0;
}

/* find cached sample containing seqno; NULL if not cached. Walks newest-first,
 * so the fast path (pushing new data at the head) hits in O(1). */
static dart_wsample *dart_find_sample(dart_channel *ch, uint64_t seqno){
    uint16_t depth = ch->qos.history_depth ? ch->qos.history_depth : 1, k;
    uint16_t i = ch->hist_head;
    for (k=0;k<depth;k++){
        dart_wsample *s;
        i = (uint16_t)(i ? i-1 : depth-1);
        s = &ch->hist[i];
        if (!s->valid) break;                  /* reached the unwritten tail */
        if (seqno >= s->base)
            return (seqno < s->base + s->count) ? s : NULL;
    }
    return NULL;
}

/* append the (already filled) head slot to history and wake the lanes that
 * will carry it */
static void dart__commit(dart_state *st, uint16_t ci, size_t len){
    dart_channel *ch = &st->chans[ci];
    uint16_t depth = ch->qos.history_depth ? ch->qos.history_depth : 1;
    uint16_t count = (uint16_t)((len + DART_FRAG_PAYLOAD - 1) / DART_FRAG_PAYLOAD);
    dart_wsample *slot = &ch->hist[ch->hist_head];
    if (count==0) count=1;
    slot->valid=1; slot->base=ch->next_seqno; slot->count=count; slot->len=(uint32_t)len;
    ch->hist_head = (uint16_t)((ch->hist_head+1) % depth);
    ch->next_seqno += count;
    /* oldest cached: the slot the head now points at once the ring wrapped,
       else slot 0 */
    ch->first_seqno = ch->hist[ch->hist_head].valid ? ch->hist[ch->hist_head].base
                                                    : ch->hist[0].base;
    ch->have_first  = 1;
    if (ch->mcast && ch->nsubs>0)
        dart__lane_wake(st, ci, st->cfg.max_peers);    /* group lane */
    else {
        uint32_t np=st->cfg.max_peers, p;
        for (p=0;p<np;p++)
            if (st->wprox[(size_t)ci*np+p].used) dart__lane_wake(st, ci, p);
    }
}

int dart_send(dart_state *st, uint16_t channel_id, const void *data, size_t len, uint64_t now){
    int ci; dart_channel *ch;
    (void)now;
    if (channel_id == DART_CHAN_META) return -1;     /* internal */
    ch = dart_chan(st, channel_id, &ci);
    if (!ch) return -1;
    if (len > ch->qos.max_sample_bytes) return -2;
    if (ch->dir == DART_SUB_ONLY || ch->dir == DART_NONE) return -3;
    if (len) memcpy(ch->hist[ch->hist_head].buf, data, len);
    dart__commit(st, (uint16_t)ci, len);
    return 0;
}

/* one interest entry: [u16 alias][u64 identity][u8 namelen][name bytes]. The
 * alias is the advertiser's channel index, used to compress the data path; the
 * name rides along so a 64-bit hash collision is detected, not cross-wired. */
static uint8_t *dart__meta_put(uint8_t *p, uint16_t alias, const dart_channel *ch){
    size_t L = dart__namelen(ch->name);
    dart_w16(p, alias); p += 2;
    dart_w64(p, ch->identity); p += 8;
    *p++ = (uint8_t)L;
    if (L){ memcpy(p, ch->name, L); p += L; }
    return p;
}
static int dart__meta_name_eq(const dart_channel *ch, const uint8_t *name, size_t nlen){
    size_t ours = dart__namelen(ch->name);
    if (nlen != ours) return 0;
    return nlen==0 ? 1 : (memcmp(ch->name, name, nlen)==0);
}
/* match `count` entries from p to local channels by identity, setting bits in
 * bm and recording the peer's alias -> our channel for the data path. Same-
 * identity-different-name is a hash collision: refused, reported. */
static const uint8_t *dart__meta_scan(dart_state *st, int ps, const uint8_t *p,
                                      uint32_t count, uint8_t *bm){
    uint32_t k;
    for (k=0;k<count;k++){
        uint16_t alias=dart_r16(p); uint64_t id=dart_r64(p+2);
        uint32_t nlen=p[10]; const uint8_t *name=p+11; int ci;
        dart_channel *ch=dart_chan_by_identity(st,id,&ci);
        p = name + nlen;
        if (!ch || ci==(int)st->meta_ci) continue;          /* not ours */
        if (dart__meta_name_eq(ch,name,nlen)){
            dart_bset(bm,(uint32_t)ci);
            if ((uint32_t)alias < st->amax)
                st->alias_ci[(size_t)ps*st->amax + alias] = (uint16_t)ci;
        }
        else if (st->cfg.on_collision)
            st->cfg.on_collision(st->cfg.user, id, ch->name?ch->name:"",
                                 (const char*)name, nlen);
    }
    return p;
}

/* publish our interest list on the meta channel: [npub u16][nsub u16] then the
 * pub entries, then the sub entries. Encoded straight into the history slot;
 * with depth 1 the newest list is all any joiner ever replays. */
static void dart__meta_publish(dart_state *st){
    dart_channel *mc=&st->chans[st->meta_ci];
    uint8_t *o=mc->hist[mc->hist_head].buf, *p=o+4;
    uint16_t c; uint32_t np=0, ns=0;
    for (c=0;c<st->meta_ci;c++){
        uint8_t d=st->chans[c].dir;
        if (d==DART_PUBSUB || d==DART_PUB_ONLY){ p=dart__meta_put(p,c,&st->chans[c]); np++; }
    }
    for (c=0;c<st->meta_ci;c++){
        uint8_t d=st->chans[c].dir;
        if (d==DART_PUBSUB || d==DART_SUB_ONLY){ p=dart__meta_put(p,c,&st->chans[c]); ns++; }
    }
    dart_w16(o,(uint16_t)np); dart_w16(o+2,(uint16_t)ns);
    dart__commit(st, st->meta_ci, (size_t)(p - o));
}

/* a peer's interest list arrived: refresh its bits, rematch every channel */
static void dart__meta_apply(dart_state *st, int ps, const uint8_t *d, size_t len){
    uint16_t np, ns, c; const uint8_t *p, *end=d+len;
    uint8_t *pb=&st->peer_pub_bm[(size_t)ps*st->bmlen];
    uint8_t *sb=&st->peer_sub_bm[(size_t)ps*st->bmlen];
    if (len < 4) return;
    np=dart_r16(d); ns=dart_r16(d+2);
    /* validate the whole variable-length list before touching any interest:
       a truncated sample must not drop a live match. */
    { uint32_t k, tot=(uint32_t)np+ns; p=d+4;
      for (k=0;k<tot;k++){
          if (p+11 > end) return;
          p += 11u + (uint32_t)p[10];
          if (p > end) return;
      } }
    memset(pb,0,st->bmlen); memset(sb,0,st->bmlen);
    memset(&st->alias_ci[(size_t)ps*st->amax],0xFF,(size_t)st->amax*sizeof(uint16_t));
    p = dart__meta_scan(st, ps, d+4, np, pb);
    p = dart__meta_scan(st, ps, p,   ns, sb);
    for (c=0;c<st->meta_ci;c++) dart__rematch(st,c,(uint16_t)ps);
}

int dart_set_dir(dart_state *st, uint16_t channel_id, uint8_t dir){
    int ci; dart_channel *ch; uint16_t p;
    if (channel_id == DART_CHAN_META || dir > DART_NONE) return -1;
    ch = dart_chan(st, channel_id, &ci);
    if (!ch) return -1;
    if (ch->dir == dir) return 0;
    ch->dir = dir;
    for (p=0;p<st->cfg.max_peers;p++)
        if (st->peer_used[p]) dart__rematch(st,(uint16_t)ci,p);
    dart__meta_publish(st);
    return 0;
}

const dart_qos *dart_channel_qos(dart_state *st, uint16_t channel_id){
    dart_channel *ch = dart_chan(st, channel_id, NULL);
    return ch ? &ch->qos : NULL;
}

int dart_send_would_evict(dart_state *st, uint16_t channel_id){
    int ci; dart_channel *ch = dart_chan(st, channel_id, &ci);
    dart_wsample *slot; uint32_t np; uint16_t p;
    if (!ch || ch->qos.reliability != DART_RELIABLE) return 0;
    slot = &ch->hist[ch->hist_head];        /* slot the next send overwrites */
    if (!slot->valid) return 0;
    np = st->cfg.max_peers;
    for (p=0;p<(uint16_t)np;p++){
        dart_wproxy *w=&st->wprox[(size_t)ci*np+p];
        if (w->used && w->acked_upto < slot->base + slot->count) return 1;
    }
    return 0;
}

/* wire alias for a local channel: its index, or 0xFFFF for the meta channel
 * (reserved and known a priori, so meta works before any alias is learned). */
static uint16_t dart__alias_of(dart_state *st, int ci){
    return (ci==(int)st->meta_ci) ? 0xFFFFu : (uint16_t)ci;
}

/* datagram builders (return length). Byte 0 = type (low 3 bits) | flags; the
 * sender's topic alias (u16) is at o+1, translated by the receiver. Header
 * sizes: DATA 13 (single fragment) or 21 (multi), HB 23, NACK 21, GAP 19. */
static size_t dart_mk_data(uint8_t *o, uint16_t alias, uint64_t seqno, dart_wsample *s,
                         uint16_t frag, const uint8_t *payload, uint16_t plen){
    dart_w16(o+1,alias);
    if (s->count==1){                       /* frag=0, count=1, len=plen implied */
        o[0]=(uint8_t)(DART_DATA|DART_F_SINGLE);
        dart_w64(o+3,seqno); dart_w16(o+11,plen);
        memcpy(o+13,payload,plen);
        return 13u+plen;
    }
    o[0]=DART_DATA;
    dart_w64(o+3,seqno); dart_w16(o+11,frag); dart_w16(o+13,s->count);
    dart_w32(o+15,s->len); dart_w16(o+19,plen);
    memcpy(o+21,payload,plen);
    return 21u+plen;
}
static size_t dart_mk_hb(uint8_t *o, uint16_t alias, uint64_t first, uint64_t last, uint32_t cnt){
    o[0]=DART_HB; dart_w16(o+1,alias); dart_w64(o+3,first); dart_w64(o+11,last); dart_w32(o+19,cnt);
    return 23;
}
static size_t dart_mk_nack(uint8_t *o, uint16_t alias, uint64_t base, uint16_t nbits, uint32_t bm,
                         uint32_t epoch, uint8_t flags){
    o[0]=(uint8_t)(DART_NACK|flags); dart_w16(o+1,alias); dart_w64(o+3,base);
    dart_w16(o+11,nbits); dart_w32(o+13,bm); dart_w32(o+17,epoch);
    return 21;
}
static size_t dart_mk_gap(uint8_t *o, uint16_t alias, uint64_t s, uint64_t e){
    o[0]=DART_GAP; dart_w16(o+1,alias); dart_w64(o+3,s); dart_w64(o+11,e);
    return 19;
}

/* reader side: handle DATA */
static void dart_reader_data(dart_state *st, int ci, int pslot, const uint8_t *p,
                           uint64_t now){
    dart_channel *ch=&st->chans[ci];
    dart_rproxy *r=&st->rprox[(size_t)ci*st->cfg.max_peers+pslot];
    int reliable = (ch->qos.reliability==DART_RELIABLE);
    uint64_t seqno, base; uint16_t frag, count, plen; uint32_t slen; const uint8_t *pay;
    if (p[0] & DART_F_SINGLE){           /* single fragment: frag/count/len implied */
        seqno=dart_r64(p+3); frag=0; count=1; plen=dart_r16(p+11); slen=plen; pay=p+13;
    } else {
        seqno=dart_r64(p+3); frag=dart_r16(p+11); count=dart_r16(p+13);
        slen=dart_r32(p+15); plen=dart_r16(p+19); pay=p+21;
    }
    base = seqno - frag;

    if (!r->used) return;                               /* not subscribed */
    if (slen > ch->qos.max_sample_bytes) return;        /* malformed */
    if (count==0 || frag>=count) return;
    if (base < r->deliver_upto) return;                 /* old/dup */

    if (base > r->deliver_upto){
        if (reliable && r->started){
            /* out-of-order: a hole exists right now, and this datagram proves
               the writer holds up to seqno. Arm the NACK immediately instead
               of waiting for a heartbeat: a busy writer defers heartbeats, and
               at high rates the ring wraps before one arrives. */
            if (seqno > r->hb_last) r->hb_last = seqno;
            if (!r->ack_pending){       /* keep the oldest due time: arrivals
                                           must not keep postponing the NACK */
                r->ack_pending=1; r->ack_due_us=now + ch->qos.nack_delay_us;
            }
            dart__lane_wake(st,(uint16_t)ci,(uint32_t)pslot);
            return;
        }
        /* first contact (reliable late-join) or best-effort: adopt the writer's
           position instead of waiting for seqnos it no longer holds */
        if (r->started && st->cfg.on_gap)                /* best-effort loss */
            st->cfg.on_gap(st->cfg.user, ch->id, st->peer_ids[pslot],
                           r->deliver_upto, base - r->deliver_upto);
        r->deliver_upto = base; r->asm_active=0;
    }
    r->started = 1;
    /* base == deliver_upto: current sample */
    if (!r->asm_active){
        r->asm_active=1; r->asm_count=count; r->asm_len=slen;
        memset(r->frag_bm,0,(ch->maxfrags+7u)/8u);
    }
    if (count!=r->asm_count) return;                    /* inconsistent, ignore */
    if (!dart_bget(r->frag_bm,frag)){
        uint32_t off=(uint32_t)frag*DART_FRAG_PAYLOAD;
        if (off+plen<=ch->qos.max_sample_bytes) memcpy(r->asm_buf+off,pay,plen);
        dart_bset(r->frag_bm,frag);
    }
    /* complete? */
    { uint16_t i; int done=1;
      for (i=0;i<count;i++) if(!dart_bget(r->frag_bm,i)){done=0;break;}
      if (done){
          if (ci==(int)st->meta_ci)
              dart__meta_apply(st, pslot, r->asm_buf, r->asm_len);
          else if (st->cfg.on_sample)
              st->cfg.on_sample(st->cfg.user, ch->id, st->peer_ids[pslot], r->asm_buf, r->asm_len);
          r->deliver_upto = base + count;
          r->asm_active=0;
      }
    }
    if (reliable){
        r->ack_pending=1; r->ack_due_us=now + ch->qos.nack_delay_us;
        dart__lane_wake(st,(uint16_t)ci,(uint32_t)pslot);
    }
}

static void dart_reader_hb(dart_state *st, int ci, int pslot, const uint8_t *p, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    dart_rproxy *r=&st->rprox[(size_t)ci*st->cfg.max_peers+pslot];
    uint64_t first=dart_r64(p+3), last=dart_r64(p+11);
    if (!r->used) return;
    if (ch->qos.reliability!=DART_RELIABLE) return;
    /* un-started readers adopt no position from heartbeats: the advertised
       first may be a dead predecessor's acked position (one-sided flap). The
       ack armed below carries our epoch; the writer re-joins on seeing it and
       the first pushed DATA sets the start. */
    if (r->started && first > r->deliver_upto){
        if (st->cfg.on_gap && ci!=(int)st->meta_ci)        /* superseded before repair */
            st->cfg.on_gap(st->cfg.user, ch->id, st->peer_ids[pslot],
                           r->deliver_upto, first - r->deliver_upto);
        r->deliver_upto=first; r->asm_active=0;
    }
    r->hb_last=last;
    r->ack_pending=1; r->ack_due_us = now + ch->qos.nack_delay_us;
    dart__lane_wake(st,(uint16_t)ci,(uint32_t)pslot);
}

static void dart_reader_gap(dart_state *st, int ci, int pslot, const uint8_t *p){
    dart_rproxy *r=&st->rprox[(size_t)ci*st->cfg.max_peers+pslot];
    uint64_t e=dart_r64(p+11);    /* gap start at p+3 is implied by deliver_upto */
    if (!r->used) return;
    if (e+1 > r->deliver_upto){
        if (r->started && st->cfg.on_gap && ci!=(int)st->meta_ci)
            st->cfg.on_gap(st->cfg.user, st->chans[ci].id, st->peer_ids[pslot],
                           r->deliver_upto, e+1 - r->deliver_upto);
        r->deliver_upto = e+1; r->asm_active=0;
    }
}

/* writer side: handle ACKNACK */
static void dart_writer_nack(dart_state *st, int ci, int pslot, const uint8_t *p){
    dart_channel *ch=&st->chans[ci];
    dart_wproxy *w=&st->wprox[(size_t)ci*st->cfg.max_peers+pslot];
    uint64_t base=dart_r64(p+3); uint16_t nbits=dart_r16(p+11); uint32_t bm=dart_r32(p+13);
    uint32_t ep=dart_r32(p+17); uint8_t fl=p[0];
    int group_mode;
    if (!w->used) return;
    group_mode = ch->mcast && ch->nsubs>0;
    if (w->reader_epoch != ep){
        if (w->reader_epoch){
            /* the reader is a new incarnation (e.g. the peer dropped us in a
               one-sided discovery flap and re-added): our acked/sent positions
               describe its dead predecessor and would tell it nothing is
               missing. Re-join the lane as if freshly matched; this ack's
               content belongs to a position the lane no longer has. */
            w->sent_upto  = group_mode ? ch->mc_sent_upto : dart_unicast_join_seqno(ch);
            w->acked_upto = w->sent_upto;
            w->has_nack   = 0;
            w->hb_next_us = 0;
            dart__lane_wake(st,(uint16_t)ci,(uint32_t)pslot);
            w->reader_epoch = ep;
            return;
        }
        w->reader_epoch = ep;      /* first contact: lane is already fresh */
    }
    if (fl & DART_F_UNPOS){
        /* the reader has delivered nothing yet, and un-started readers never
           NACK: any push it missed (e.g. one that raced ahead of the peer
           adding us) is otherwise lost for good. Re-push from the unacked
           edge, which is exactly the join window. */
        if (!group_mode && w->acked_upto < w->sent_upto){
            w->sent_upto = w->acked_upto;
            dart__lane_wake(st,(uint16_t)ci,(uint32_t)pslot);
        }
        return;                    /* no position information to apply */
    }
    if (base > w->acked_upto) w->acked_upto=base;
    if (nbits>0 && bm!=0){
        w->has_nack=1; w->nack_base=base; w->nack_bits=bm;
        dart__lane_wake(st,(uint16_t)ci,(uint32_t)pslot);
    }
}

void dart_on_datagram(dart_state *st, uint32_t from, const void *dg, size_t len, uint64_t now){
    const uint8_t *p=(const uint8_t*)dg; size_t rem=len;
    int ps=dart_peer_slot(st,from);
    if (ps<0) return;
    /* concatenated submessages; each length comes from its header (byte 0 type +
       flags, then the 2-byte alias), so no container framing is needed. */
    while (rem>=3){
        uint8_t b0=p[0], type=(uint8_t)(b0 & DART_MSG_MASK); uint16_t alias; size_t sub; int ci;
        switch(type){
            case DART_DATA: if (b0 & DART_F_SINGLE){ if (rem<13) return; sub=13u+(size_t)dart_r16(p+11); }
                            else { if (rem<21) return; sub=21u+(size_t)dart_r16(p+19); } break;
            case DART_HB:   if (rem<23) return; sub=23; break;
            case DART_NACK: if (rem<21) return; sub=21; break;
            case DART_GAP:  if (rem<19) return; sub=19; break;
            default: return;             /* unknown type: cannot resync, drop rest */
        }
        if (sub>rem) return;             /* truncated/malformed */
        alias = dart_r16(p+1);
        if (alias==0xFFFFu) ci=(int)st->meta_ci;             /* reserved meta alias */
        else if ((uint32_t)alias < st->amax){
            uint16_t m=st->alias_ci[(size_t)ps*st->amax+alias]; ci=(m==0xFFFFu)?-1:(int)m;
        } else ci=-1;
        if (ci>=0){
            switch(type){
                case DART_DATA: dart_reader_data(st,ci,ps,p,now); break;
                case DART_HB:   dart_reader_hb  (st,ci,ps,p,now); break;
                case DART_NACK: dart_writer_nack(st,ci,ps,p); break;
                case DART_GAP:  dart_reader_gap (st,ci,ps,p); break;
            }
        }
        p+=sub; rem-=sub;
    }
}

/* produce one writer submessage for (ci,pslot) into out if due and it fits in
 * cap; 0 if none. When nothing fits, state is untouched so the same submessage
 * is produced next time. Call repeatedly with a shrinking cap to pack several. */
static size_t dart_writer_emit(dart_state *st, int ci, int pslot, uint8_t *out, size_t cap, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    dart_wproxy *w=&st->wprox[(size_t)ci*st->cfg.max_peers+pslot];
    int reliable=(ch->qos.reliability==DART_RELIABLE);
    /* group mode is active only while a mcast channel has remote subscribers.
       New data and heartbeats then ride the group lane; this per-peer lane only
       answers NACKs with unicast repairs. Local-only subscribers stay unicast. */
    int group_mode = ch->mcast && ch->nsubs>0;
    uint16_t alias = dart__alias_of(st, ci);
    if (!w->used) return 0;
    if (group_mode && (!reliable || !w->has_nack)) return 0;

    /* 1. repair (reliable only) */
    if (reliable && w->has_nack){
        uint32_t i;
        for (i=0;i<DART_NACK_WINDOW;i++){
            if (w->nack_bits & (1u<<i)){
                uint64_t seqno=w->nack_base+i;
                dart_wsample *s;
                if (seqno>=ch->next_seqno){                 /* nothing there */
                    w->nack_bits &= ~(1u<<i);
                    if (w->nack_bits==0) w->has_nack=0;
                    continue;
                }
                s=dart_find_sample(ch,seqno);
                if (s){
                    uint16_t fi=(uint16_t)(seqno - s->base);
                    uint32_t off=(uint32_t)fi*DART_FRAG_PAYLOAD;
                    uint16_t plen=(uint16_t)((s->len-off)<DART_FRAG_PAYLOAD?(s->len-off):DART_FRAG_PAYLOAD);
                    if (cap < (size_t)(s->count==1?13u:21u)+(size_t)plen) return 0;   /* bit stays set */
                    w->nack_bits &= ~(1u<<i);
                    if (w->nack_bits==0) w->has_nack=0;
                    return dart_mk_data(out,alias,seqno,s,fi,s->buf+off,plen);
                } else {
                    /* superseded: GAP the dropped region below the cache, but
                       keep still-cached requested seqnos for repair on the
                       following calls */
                    uint64_t e = (ch->have_first?ch->first_seqno:ch->next_seqno);
                    uint32_t j;
                    if (cap < 19) return 0;
                    if (e>0) e-=1; else e=seqno;
                    if (e<seqno) e=seqno;
                    for (j=0;j<DART_NACK_WINDOW;j++)
                        if (w->nack_base+j <= e) w->nack_bits &= ~(1u<<j);
                    if (w->nack_bits==0) w->has_nack=0;
                    return dart_mk_gap(out,alias,w->nack_base,e);
                }
            }
        }
        w->has_nack=0;
    }
    if (group_mode) return 0;   /* group lane owns everything below */

    /* 2. push new data */
    if (w->sent_upto < ch->next_seqno){
        uint64_t seqno=w->sent_upto;
        dart_wsample *s=dart_find_sample(ch,seqno);
        if (s){
            uint16_t fi=(uint16_t)(seqno - s->base);
            uint32_t off=(uint32_t)fi*DART_FRAG_PAYLOAD;
            uint16_t plen=(uint16_t)((s->len-off)<DART_FRAG_PAYLOAD?(s->len-off):DART_FRAG_PAYLOAD);
            if (cap < (size_t)(s->count==1?13u:21u)+(size_t)plen) return 0;
            w->sent_upto++;
            return dart_mk_data(out,alias,seqno,s,fi,s->buf+off,plen);
        } else {
            /* fell out of the ring before we sent it: GAP up to first cached */
            uint64_t e=(ch->have_first?ch->first_seqno:ch->next_seqno);
            uint64_t gs=w->sent_upto;
            if (cap < 19) return 0;
            if (e>0) e-=1; else e=ch->next_seqno-1;
            w->sent_upto=(ch->have_first?ch->first_seqno:ch->next_seqno);
            if (e<gs) e=gs;
            return dart_mk_gap(out,alias,gs,e);
        }
    }

    /* 3. heartbeat (reliable, when caught up and timer due) */
    if (reliable && now>=w->hb_next_us && ch->next_seqno>0){
        uint64_t first = ch->have_first ? ch->first_seqno : 0;
        if (cap < 23) return 0;
        /* nothing below acked_upto ever needs repair, so advertise from there:
           a fresh reader then adopts the join point instead of NACKing the
           whole cached ring past its join_replay window */
        if (w->acked_upto > first) first = w->acked_upto;
        w->hb_next_us = now + (ch->qos.heartbeat_us?ch->qos.heartbeat_us:100000u);
        w->hb_count++;
        return dart_mk_hb(out,alias,first,ch->next_seqno-1,w->hb_count);
    }
    return 0;
}

/* produce a reader ACKNACK for (ci,pslot) if due; 0 if none */
static size_t dart_reader_emit(dart_state *st, int ci, int pslot, uint8_t *out, size_t cap, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    dart_rproxy *r=&st->rprox[(size_t)ci*st->cfg.max_peers+pslot];
    uint64_t base; uint16_t nbits=0; uint32_t bm=0;
    uint16_t alias = dart__alias_of(st, ci);
    if (!r->used) return 0;
    if (ch->qos.reliability!=DART_RELIABLE) return 0;
    if (cap<21) return 0;
    if (!r->ack_pending || now<r->ack_due_us) return 0;
    r->ack_pending=0;

    if (!r->asm_active){
        base=r->deliver_upto;
        if (!r->started){ nbits=0; bm=0; }   /* no position yet: epoch hello */
        else if (r->deliver_upto<=r->hb_last){
            /* everything in (deliver_upto..hb_last] is missing here, so request
               the whole window: one round-trip repairs a burst loss instead of
               one TU per nack_delay */
            uint64_t miss = r->hb_last - r->deliver_upto + 1;
            nbits = (uint16_t)(miss < DART_NACK_WINDOW ? miss : DART_NACK_WINDOW);
            bm = (nbits >= 32) ? 0xFFFFFFFFu : (uint32_t)((1u<<nbits)-1u);
        }
        else { nbits=0; bm=0; }                                /* caught up */
    } else {
        uint16_t lm=0, i; int found=-1;
        for (i=0;i<r->asm_count;i++) if(!dart_bget(r->frag_bm,i)){found=(int)i;break;}
        if (found<0){ base=r->deliver_upto; nbits=0; bm=0; }
        else {
            lm=(uint16_t)found; base=r->deliver_upto+lm;
            for (i=0;i<DART_NACK_WINDOW && (lm+i)<r->asm_count;i++)
                if(!dart_bget(r->frag_bm,(uint32_t)(lm+i))){ bm|=(1u<<i); }
            { uint32_t rem=(uint32_t)(r->asm_count-lm);
              nbits=(uint16_t)(rem<DART_NACK_WINDOW?rem:DART_NACK_WINDOW); }
        }
    }
    return dart_mk_nack(out,alias,base,nbits,bm,r->epoch,
                      r->started ? 0 : (uint8_t)DART_F_UNPOS);
}

/* multicast writer lane for channel ci: new data once for the whole group,
 * then a channel-level heartbeat (reliable). Same per-call contract as
 * dart_writer_emit. */
static size_t dart_group_emit(dart_state *st, int ci, uint8_t *out, size_t cap, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    uint16_t alias = dart__alias_of(st, ci);
    if (!ch->mcast || ch->dir==DART_SUB_ONLY || ch->dir==DART_NONE) return 0;
    if (ch->nsubs==0){
        /* no remote subscribers: pin the cursor forward so a future remote
           join never gets a stale replay */
        ch->mc_sent_upto = ch->next_seqno;
        return 0;
    }
    if (ch->mc_sent_upto < ch->next_seqno){
        uint64_t seqno=ch->mc_sent_upto;
        dart_wsample *s=dart_find_sample(ch,seqno);
        if (s){
            uint16_t fi=(uint16_t)(seqno - s->base);
            uint32_t off=(uint32_t)fi*DART_FRAG_PAYLOAD;
            uint16_t plen=(uint16_t)((s->len-off)<DART_FRAG_PAYLOAD?(s->len-off):DART_FRAG_PAYLOAD);
            if (cap < (size_t)(s->count==1?13u:21u)+(size_t)plen) return 0;
            ch->mc_sent_upto++;
            return dart_mk_data(out,alias,seqno,s,fi,s->buf+off,plen);
        } else {
            uint64_t e=(ch->have_first?ch->first_seqno:ch->next_seqno);
            uint64_t gs=ch->mc_sent_upto;
            if (cap < 19) return 0;
            if (e>0) e-=1; else e=ch->next_seqno-1;
            ch->mc_sent_upto=(ch->have_first?ch->first_seqno:ch->next_seqno);
            if (e<gs) e=gs;
            return dart_mk_gap(out,alias,gs,e);
        }
    }
    if (ch->qos.reliability==DART_RELIABLE && now>=ch->mc_hb_next_us && ch->next_seqno>0){
        if (cap < 23) return 0;
        ch->mc_hb_next_us = now + (ch->qos.heartbeat_us?ch->qos.heartbeat_us:100000u);
        ch->mc_hb_count++;
        return dart_mk_hb(out,alias,(ch->have_first?ch->first_seqno:0),ch->next_seqno-1,ch->mc_hb_count);
    }
    return 0;
}

/* event work a popped lane still owes right now (timer-armed work is the
 * sweep's job, so a lane never camps in the queue waiting on a clock) */
static int dart__lane_work(dart_state *st, uint16_t ci, uint32_t ps, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    uint32_t np=st->cfg.max_peers;
    if (ps==np)
        return ch->mcast && ch->nsubs>0 && ch->mc_sent_upto < ch->next_seqno;
    if (!st->peer_used[ps]) return 0;
    { dart_wproxy *w=&st->wprox[(size_t)ci*np+ps];
      dart_rproxy *r=&st->rprox[(size_t)ci*np+ps];
      int group_mode = ch->mcast && ch->nsubs>0;
      if (w->used && w->has_nack) return 1;
      if (w->used && !group_mode && w->sent_upto < ch->next_seqno) return 1;
      if (r->used && ch->qos.reliability==DART_RELIABLE
          && r->ack_pending && now >= r->ack_due_us) return 1;
    }
    return 0;
}

/* clock-driven counterpart of the wake calls: heartbeats and delayed acks have
 * no triggering event, so a cursor walks the lane table at a fixed TIME rate
 * (full coverage every DART_HB_SWEEP_US, independent of poll frequency) and
 * wakes lanes whose timers came due. Read-only checks; cost is bounded by
 * table size per sweep period, not per poll call. */
static void dart__hb_sweep(dart_state *st, uint64_t now){
    uint32_t np=st->cfg.max_peers, lanes=np+1u;
    uint32_t total=(uint32_t)st->cfg.n_channels*lanes, due, k;
    uint64_t span = now - st->sweep_t;
    due = (span >= DART_HB_SWEEP_US) ? total
        : (uint32_t)(span * total / DART_HB_SWEEP_US);
    if (!due) return;              /* sweep_t advances only when lanes are paid */
    st->sweep_t = now;
    for (k=0;k<due;k++){
        uint32_t L=st->sweep, ps=L%lanes;
        uint16_t ci=(uint16_t)(L/lanes);
        dart_channel *ch=&st->chans[ci];
        st->sweep = (st->sweep+1u>=total) ? 0u : st->sweep+1u;
        /* next_seqno==0 means we've never published here, so there are no
           heartbeats to send; but a reader on this channel can still owe an
           ACKNACK, and its timer must be swept even on a sub-only node (whose
           data channels never advance next_seqno). Gate only the writer/mcast
           heartbeats on next_seqno, never the reader ack. */
        if (ch->qos.reliability!=DART_RELIABLE) continue;
        if (ps==np){
            if (ch->next_seqno && ch->mcast && ch->nsubs>0 && now>=ch->mc_hb_next_us)
                dart__lane_wake(st,ci,ps);
            continue;
        }
        if (!st->peer_used[ps]) continue;
        { dart_wproxy *w=&st->wprox[(size_t)ci*np+ps];
          dart_rproxy *r=&st->rprox[(size_t)ci*np+ps];
          int group_mode = ch->mcast && ch->nsubs>0;
          if ((ch->next_seqno && w->used && !group_mode && now>=w->hb_next_us)
           || (r->used && r->ack_pending && now>=r->ack_due_us))
              dart__lane_wake(st,ci,ps);
        }
    }
}

int dart_poll_send(dart_state *st, uint32_t *to_peer, void *out, size_t cap, size_t *out_len, uint64_t now){
    uint32_t np=st->cfg.max_peers, lanes=np+1u;
    uint32_t ndest = np+(uint32_t)st->cfg.n_channels;
    dart__hb_sweep(st, now);
    while (st->destq_n){
        uint32_t d; size_t off=0;
        d = st->destq[st->destq_head];
        st->destq_head = (st->destq_head+1u>=ndest) ? 0u : st->destq_head+1u;
        st->destq_n--; st->dest_inq[d]=0;
        /* drain this destination's lanes into one datagram */
        while (st->dest_head[d]!=DART__NIL){
            uint32_t L=st->dest_head[d], ps=L%lanes;
            uint16_t ci=(uint16_t)(L/lanes);
            size_t n;
            do {
                if (ps==np) n=dart_group_emit(st,ci,(uint8_t*)out+off,cap-off,now);
                else {
                    /* acks first: they are small, one-shot, and carry the
                       NACKs that drive repair. A backlogged writer would
                       otherwise fill every datagram and starve them. */
                    n=dart_reader_emit(st,(int)ci,(int)ps,(uint8_t*)out+off,cap-off,now);
                    if (!n) n=dart_writer_emit(st,(int)ci,(int)ps,(uint8_t*)out+off,cap-off,now);
                }
                off+=n;
            } while (n && off<cap);
            st->dest_head[d]=st->lane_next[L];
            if (dart__lane_work(st,ci,ps,now)){
                /* datagram full mid-lane: rotate the lane to the back of its
                   destination so sibling lanes get the next datagram */
                if (st->dest_head[d]==DART__NIL) st->dest_head[d]=L;
                else {
                    st->lane_next[L]=DART__NIL;
                    st->lane_next[st->dest_tail[d]]=L;
                    st->dest_tail[d]=L;
                }
                break;
            }
            st->lane_inq[L]=0;     /* lane drained */
        }
        if (st->dest_head[d]!=DART__NIL) dart__dest_push(st,d);  /* fair: re-queue at tail */
        if (off){
            *to_peer = (d<np) ? st->peer_ids[d]
                              : DART_DEST_GROUP(st->chans[d-np].identity & 0xFFu);
            *out_len = off;
            return 1;
        }
        if (st->dest_head[d]!=DART__NIL)
            return 0;    /* work pending but nothing fit: caller's cap too small */
    }
    return 0;
}

#ifndef DART_TRANSPORT_SANS_IO
/* ===== dart_node.c ===== */
/* NODE runtime: owns the data socket, drives discovery, wires
 * peers into the transport. See dart_node.h. */

/* Feature-test macros must precede the first system header in the TU. */
#if !defined(_WIN32)
  #ifndef _POSIX_C_SOURCE
  #define _POSIX_C_SOURCE 200809L
  #endif
  #ifndef _DEFAULT_SOURCE
  #define _DEFAULT_SOURCE 1
  #endif
#endif

#include <string.h>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #ifdef _MSC_VER
    #pragma comment(lib, "ws2_32.lib")
  #endif
  typedef SOCKET    dart_sock_t;
  typedef WSAPOLLFD dart_pollfd_t;
  #define DART_BADSOCK   INVALID_SOCKET
  #define DART_CLOSESOCK closesocket
  #define DART_POLL      WSAPoll
  #ifndef SIO_UDP_CONNRESET
  #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
  #endif
  static void dart__net_startup(void){ WSADATA w; WSAStartup(MAKEWORD(2,2), &w); }
  static void dart__net_cleanup(void){ WSACleanup(); }
  static void dart__set_nonblock(dart_sock_t fd){ u_long nb=1; ioctlsocket(fd, FIONBIO, &nb); }
  static int  dart__would_block(void){ return WSAGetLastError()==WSAEWOULDBLOCK; }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <poll.h>
  #include <time.h>
  #include <fcntl.h>
  #include <errno.h>
  typedef int           dart_sock_t;
  typedef struct pollfd dart_pollfd_t;
  #define DART_BADSOCK   (-1)
  #define DART_CLOSESOCK close
  #define DART_POLL      poll
  static void dart__net_startup(void){}
  static void dart__net_cleanup(void){}
  static void dart__set_nonblock(dart_sock_t fd){ int fl=fcntl(fd,F_GETFL,0); if(fl!=-1) fcntl(fd,F_SETFL,fl|O_NONBLOCK); }
  static int  dart__would_block(void){ return errno==EAGAIN || errno==EWOULDBLOCK; }
#endif

static uint64_t dart_now_us(void){
#ifdef _WIN32
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart * 1000000ull) / (uint64_t)f.QuadPart);
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
#endif
}

typedef struct {
    uint8_t  used;
    uint32_t id;
    uint8_t  ip[16];
    uint8_t  ip_len;
    uint16_t port;     /* peer's advertised transport (data) port */
} dart__nodepeer;

struct dart_node {
    dart_state     *tr;
    dart_discovery_rt     *disc;
    dart_sock_t     fd;       /* unicast transport data socket (also group TX) */
    dart_sock_t     mcfd;     /* multicast data RX socket (DART_BADSOCK if unused) */
    dart__nodepeer *peers;
    uint16_t      max_peers;
    uint16_t      domain;
    uint16_t      mc_port;
    /* datagram consumed from the core but refused by the socket; retried first
     * next poll so it is never lost */
    uint8_t       txhold[DART_DGRAM_MAX];
    size_t        txhold_len;
    uint32_t      txhold_peer;
    /* cumulative backpressure (max_block_us waits in dart_node_send) */
    uint64_t      block_us;
    uint32_t      block_n;
};

/* split node config into discovery + transport sub-configs (callbacks/user
 * installed later by open) */
static void dart__node_cfgs(const dart_node_config *cfg, dart_discovery_rt_config *dc,
                          dart_config *tc, uint16_t *mp_out){
    uint16_t mp = cfg->max_peers ? cfg->max_peers : 16;
    memset(dc, 0, sizeof *dc); memset(tc, 0, sizeof *tc);
    dc->disc.domain_id   = cfg->domain_id;
    dc->disc.data_port   = cfg->data_port;     /* advertised to peers */
    dc->disc.announce_us = cfg->announce_us;   /* 0 uses dart_discovery default */
    dc->disc.timeout_us  = cfg->timeout_us;
    dc->disc.max_peers   = mp;
    dc->group            = cfg->disc_group;
    dc->disc_port        = cfg->disc_port;
    dc->ttl              = cfg->ttl;
    dc->mcast_if         = cfg->mcast_if;
    dc->seeds            = cfg->seeds;
    dc->n_seeds          = cfg->n_seeds;
    tc->channels     = cfg->channels;
    tc->n_channels   = cfg->n_channels;
    tc->max_peers    = mp;
    tc->meta_max_ids = cfg->meta_max_ids;
    tc->on_sample    = cfg->on_sample;
    tc->on_gap       = cfg->on_gap;
    tc->on_collision = cfg->on_collision;
    tc->user         = cfg->user;
    if (mp_out) *mp_out = mp;
}

/* A peer address is local iff a route probe to it selects that same address as
 * source: true for 127.0.0.1 and this host's own addresses, never for another machine. */
static int dart__node_is_local_ip(const uint8_t ip[4]){
    dart_sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a; int local = 0;
    if (s==DART_BADSOCK) return 0;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; memcpy(&a.sin_addr.s_addr, ip, 4); a.sin_port = htons(7);
    if (connect(s, (struct sockaddr*)&a, sizeof a)==0){
        struct sockaddr_in loc;
#ifdef _WIN32
        int ll = (int)sizeof loc;
#else
        socklen_t ll = sizeof loc;
#endif
        if (getsockname(s, (struct sockaddr*)&loc, &ll)==0)
            local = (memcmp(&loc.sin_addr.s_addr, ip, 4)==0);
    }
    DART_CLOSESOCK(s);
    return local;
}

static void dart__node_up(void *u, uint32_t id, const dart_discovery_addr *addr,
                        const uint8_t *meta, uint8_t mlen){
    dart_node *n = (dart_node*)u; uint16_t i; int slot = -1;
    for (i=0;i<n->max_peers;i++){
        if (n->peers[i].used && n->peers[i].id==id){      /* address update */
            memcpy(n->peers[i].ip, addr->ip, 16);
            n->peers[i].ip_len = addr->ip_len; n->peers[i].port = addr->port;
            return;
        }
        if (!n->peers[i].used && slot<0) slot=(int)i;
    }
    if (slot<0) return;
    n->peers[slot].used=1; n->peers[slot].id=id;
    memcpy(n->peers[slot].ip, addr->ip, 16);
    n->peers[slot].ip_len=addr->ip_len; n->peers[slot].port=addr->port;
    /* interest arrives over the transport's meta channel, not the announce */
    (void)meta; (void)mlen;
    dart_peer_add(n->tr, id, (addr->ip_len==4) && dart__node_is_local_ip(addr->ip));
}
static void dart__node_down(void *u, uint32_t id){
    dart_node *n=(dart_node*)u; uint16_t i;
    for (i=0;i<n->max_peers;i++) if (n->peers[i].used && n->peers[i].id==id){ n->peers[i].used=0; break; }
    dart_peer_remove(n->tr, id);
}

static int dart__node_find_addr(dart_node *n, const struct sockaddr_in *s){
    uint16_t i, port=ntohs(s->sin_port);
    for (i=0;i<n->max_peers;i++)
        if (n->peers[i].used && n->peers[i].ip_len>=4 && n->peers[i].port==port
            && memcmp(n->peers[i].ip, &s->sin_addr.s_addr, 4)==0) return (int)i;
    return -1;
}
static int dart__node_find_id(dart_node *n, uint32_t id){
    uint16_t i;
    for (i=0;i<n->max_peers;i++) if (n->peers[i].used && n->peers[i].id==id) return (int)i;
    return -1;
}

/* deterministic data multicast group from a group selector (topic identity &
 * 0xFF). &0xFF wrap collisions are harmless: the receive path filters by peer
 * table and topic identity. */
static uint32_t dart__node_group_addr(uint16_t domain, uint16_t sel){
    uint32_t a = (239u<<24)|(255u<<16)|((uint32_t)(domain&0xFFu)<<8)|(uint32_t)(sel&0xFFu);
    return htonl(a);
}
/* the group a channel def joins/sends on, derived from its topic identity so it
 * matches the core's DART_DEST_GROUP selector and every peer agrees. */
static uint32_t dart__node_chan_group(uint16_t domain, const dart_channel_def *def){
    return dart__node_group_addr(domain, (uint16_t)(dart_channel_identity(def) & 0xFFu));
}

/* Send one datagram to a peer or multicast group. Returns 1 when the datagram
 * is done with (sent, peer unknown, or hard error), 0 only on a would-block
 * TX-buffer-full condition, where the caller must keep it. */
static int dart__node_tx(dart_node *n, uint32_t to, const uint8_t *buf, size_t len){
    struct sockaddr_in d;
    memset(&d, 0, sizeof d);
    d.sin_family = AF_INET;
    if (DART_DEST_IS_GROUP(to)){
        d.sin_addr.s_addr = dart__node_group_addr(n->domain, DART_DEST_GROUP_CHAN(to));
        d.sin_port = htons(n->mc_port);
    } else {
        int pi = dart__node_find_id(n, to);
        if (pi < 0) return 1;              /* peer vanished */
        memcpy(&d.sin_addr.s_addr, n->peers[pi].ip, 4);
        d.sin_port = htons(n->peers[pi].port);
    }
    if (sendto(n->fd, (const char*)buf, (int)len, 0, (struct sockaddr*)&d, sizeof d) < 0
        && dart__would_block())
        return 0;
    return 1;
}

size_t dart_node_required_memory(const dart_node_config *cfg){
    dart_discovery_rt_config dc; dart_config tc; uint16_t mp;
    size_t node_sz, tbl_sz, disc_sz, tr_sz;
    if (!cfg || cfg->n_channels==0) return 0;
    dart__node_cfgs(cfg,&dc,&tc,&mp);
    node_sz = (sizeof(struct dart_node)+15u)&~(size_t)15u;
    tbl_sz  = ((size_t)mp*sizeof(dart__nodepeer)+15u)&~(size_t)15u;
    disc_sz = (dart_discovery_rt_required_memory(&dc)+15u)&~(size_t)15u;
    tr_sz   = (dart_required_memory(&tc)+15u)&~(size_t)15u;
    return 32u + node_sz + tbl_sz + disc_sz + tr_sz;
}

dart_node *dart_node_open(void *mem, size_t cap, const dart_node_config *cfg){
    dart_discovery_rt_config dc; dart_config tc; uint16_t mp;
    uint8_t *base, *p; size_t node_sz, tbl_sz, disc_sz, tr_sz;
    dart_node *n; dart_sock_t fd; struct sockaddr_in a;
#ifdef _WIN32
    int alen;
#else
    socklen_t alen;
#endif
    if (!mem || !cfg || cfg->n_channels==0) return NULL;
    if (cap < dart_node_required_memory(cfg)) return NULL;
    dart__node_cfgs(cfg,&dc,&tc,&mp);

    dart__net_startup();

    base = (uint8_t*)(((uintptr_t)mem+15u)&~(uintptr_t)15u);
    node_sz = (sizeof(struct dart_node)+15u)&~(size_t)15u;
    tbl_sz  = ((size_t)mp*sizeof(dart__nodepeer)+15u)&~(size_t)15u;
    disc_sz = (dart_discovery_rt_required_memory(&dc)+15u)&~(size_t)15u;
    tr_sz   = (dart_required_memory(&tc)+15u)&~(size_t)15u;

    n=(dart_node*)base; memset(n,0,sizeof *n);
    n->fd = DART_BADSOCK; n->mcfd = DART_BADSOCK; n->max_peers=mp;
    n->domain = cfg->domain_id;
    n->mc_port = cfg->mc_port ? cfg->mc_port
               : (uint16_t)((cfg->disc_port ? cfg->disc_port : 7400) + 1);
    p = base + node_sz;
    n->peers=(dart__nodepeer*)p; memset(n->peers,0,(size_t)mp*sizeof(dart__nodepeer));
    p += tbl_sz;

    n->tr = dart_init(p, tr_sz, &tc);
    if (!n->tr){ dart__net_cleanup(); return NULL; }
    p += tr_sz;

    /* Bind the unicast data socket before opening discovery so we can advertise
       its real port (data_port == 0 gets an OS ephemeral, read back via
       getsockname). No SO_REUSEADDR: a unicast endpoint owns its port
       exclusively, so a port collision fails loudly here. */
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd==DART_BADSOCK){ dart__net_cleanup(); return NULL; }
    memset(&a,0,sizeof a);
    a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_ANY);
    a.sin_port=htons(cfg->data_port);          /* 0 => ephemeral */
    if (bind(fd,(struct sockaddr*)&a,sizeof a)!=0){
        DART_CLOSESOCK(fd); dart__net_cleanup(); return NULL;
    }
    memset(&a,0,sizeof a); alen=(int)sizeof a;
    if (getsockname(fd,(struct sockaddr*)&a,&alen)!=0){
        DART_CLOSESOCK(fd); dart__net_cleanup(); return NULL;
    }
    dart__set_nonblock(fd);   /* never block in recv/send: poll drains the whole
                               RX queue and a full TX buffer never deadlocks */
#ifdef _WIN32
    /* Without this, a bounced send (peer gone, ICMP port unreachable) surfaces
       as WSAECONNRESET on a later recvfrom, injecting one peer's error into the
       shared RX path. Turn the reporting off. */
    { BOOL off = FALSE; DWORD bv = 0;
      WSAIoctl(fd, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &bv, NULL, NULL); }
#endif
    if (cfg->so_rcvbuf){ int v=(int)cfg->so_rcvbuf; setsockopt(fd,SOL_SOCKET,SO_RCVBUF,(const char*)&v,sizeof v); }
    if (cfg->so_sndbuf){ int v=(int)cfg->so_sndbuf; setsockopt(fd,SOL_SOCKET,SO_SNDBUF,(const char*)&v,sizeof v); }
    n->fd=fd;
    dc.disc.data_port = ntohs(a.sin_port);     /* advertise the actual port */

    /* multicast data: group TX goes out the unicast data socket (source port
       still identifies the sender); group RX needs its own socket on the shared
       mc_port, one IGMP join per subscribed channel. */
    { int want_rx=0, want_tx=0; uint16_t i; uint32_t ifip;
      for (i=0;i<cfg->n_channels;i++) if (cfg->channels[i].mcast){
          if (cfg->channels[i].dir!=DART_PUB_ONLY) want_rx=1;
          if (cfg->channels[i].dir!=DART_SUB_ONLY) want_tx=1;
      }
      /* pin data multicast to the same egress interface discovery uses, so the
         source address peers learned from announces matches group data. Otherwise
         multihomed hosts pick per-group interfaces and break peer identification. */
      ifip = !(want_rx||want_tx) ? htonl(INADDR_ANY)
           : cfg->mcast_if       ? inet_addr(cfg->mcast_if)
           : dart_discovery_mcast_if_for(inet_addr(cfg->disc_group?cfg->disc_group:"239.255.0.7"),
                                cfg->disc_port?cfg->disc_port:7400);
      if (want_tx){
          unsigned char mttl = cfg->ttl ? cfg->ttl : 1, mloop = 1;
          setsockopt(n->fd, IPPROTO_IP, IP_MULTICAST_IF,   (const char*)&ifip,  sizeof ifip);
          setsockopt(n->fd, IPPROTO_IP, IP_MULTICAST_TTL,  (const char*)&mttl,  sizeof mttl);
          setsockopt(n->fd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&mloop, sizeof mloop);
      }
      if (want_rx){
          dart_sock_t mfd = socket(AF_INET, SOCK_DGRAM, 0);
          int on=1, mc_ok=(mfd!=DART_BADSOCK);
          unsigned char mloop = 1;
          struct sockaddr_in ma;
          if (mc_ok){
              setsockopt(mfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);
#ifdef SO_REUSEPORT
              setsockopt(mfd, SOL_SOCKET, SO_REUSEPORT, (const char*)&on, sizeof on);
#endif
              memset(&ma,0,sizeof ma);
              ma.sin_family=AF_INET; ma.sin_addr.s_addr=htonl(INADDR_ANY);
              ma.sin_port=htons(n->mc_port);
              if (bind(mfd,(struct sockaddr*)&ma,sizeof ma)!=0) mc_ok=0;
          }
          if (mc_ok){
              /* one join per distinct group: the &0xFF group mapping lets
                 channels share a group, and kernels reject duplicate
                 memberships. Memberships per socket are also OS-capped
                 (often ~20: Linux net.ipv4.igmp_max_memberships), so keep
                 mcast channels few; a failed join fails the open. */
              for (i=0;i<cfg->n_channels;i++)
                  if (cfg->channels[i].mcast && cfg->channels[i].dir!=DART_PUB_ONLY){
                      uint32_t g = dart__node_chan_group(cfg->domain_id, &cfg->channels[i]);
                      uint16_t j; int dup=0;
                      for (j=0;j<i;j++)
                          if (cfg->channels[j].mcast && cfg->channels[j].dir!=DART_PUB_ONLY
                              && dart__node_chan_group(cfg->domain_id, &cfg->channels[j])==g){
                              dup=1; break;
                          }
                      if (dup) continue;
                      { struct ip_mreq mr; memset(&mr,0,sizeof mr);
                        mr.imr_multiaddr.s_addr=g;
                        mr.imr_interface.s_addr=ifip;
                        if (setsockopt(mfd,IPPROTO_IP,IP_ADD_MEMBERSHIP,(const char*)&mr,sizeof mr)!=0){
                            mc_ok=0; break;
                        } }
                  }
          }
          if (!mc_ok){
              if (mfd!=DART_BADSOCK) DART_CLOSESOCK(mfd);
              DART_CLOSESOCK(fd); n->fd=DART_BADSOCK; dart__net_cleanup(); return NULL;
          }
          setsockopt(mfd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&mloop, sizeof mloop);
          dart__set_nonblock(mfd);
          if (cfg->so_rcvbuf){ int v=(int)cfg->so_rcvbuf; setsockopt(mfd,SOL_SOCKET,SO_RCVBUF,(const char*)&v,sizeof v); }
          n->mcfd=mfd;
      } }

    dc.disc.on_peer_up   = dart__node_up;
    dc.disc.on_peer_down = dart__node_down;
    dc.disc.user         = n;
    n->disc = dart_discovery_rt_open(p, disc_sz, &dc);
    if (!n->disc){
        if (n->mcfd!=DART_BADSOCK){ DART_CLOSESOCK(n->mcfd); n->mcfd=DART_BADSOCK; }
        DART_CLOSESOCK(fd); n->fd=DART_BADSOCK; dart__net_cleanup(); return NULL;
    }
    p += disc_sz;

    return n;
}

/* Max wall-time draining the RX queue (and running on_sample) per poll tick
 * before yielding to discovery. Bounds how long a slow on_sample starves the
 * single-threaded loop, keeping discovery alive so peers never time out. */
#ifndef DART_RX_BUDGET_US
#define DART_RX_BUDGET_US 5000u
#endif

/* Drain one socket's receive queue into the transport until empty or the
 * deadline passes. Draining fully (vs one-per-tick) avoids NACK storms.
 * Sockets are non-blocking, so recvfrom <=0 means empty. */
static void dart__node_drain(dart_node *n, dart_sock_t fd, uint64_t deadline){
    uint8_t buf[DART_DGRAM_MAX];
    for (;;){
        struct sockaddr_in src;
#ifdef _WIN32
        int sl=(int)sizeof src;
#else
        socklen_t sl=sizeof src;
#endif
        int r=(int)recvfrom(fd,(char*)buf,(int)sizeof buf,0,(struct sockaddr*)&src,&sl);
        if (r<0){
            if (dart__would_block()) break;        /* queue empty */
            continue;   /* per-datagram error (e.g. a bounced send surfaces as
                           WSAECONNRESET); datagrams behind it are fine, keep draining */
        }
        if (r>0){
            if (r>=4 && buf[0]=='u' && buf[1]=='D' && buf[2]=='S' && buf[3]=='C'){
                /* unicast announce aimed at our data port: the only address
                   that reaches THIS process when several share the disc port.
                   Hand it to discovery. */
                uint8_t sip[4]; memcpy(sip, &src.sin_addr.s_addr, 4);
                dart_discovery_rt_feed(n->disc, sip, 4, buf, (size_t)r);
            } else {
                int pi=dart__node_find_addr(n,&src);
                if (pi>=0) dart_on_datagram(n->tr, n->peers[pi].id, buf, (size_t)r, dart_now_us());
            }
        }
        if (dart_now_us() >= deadline) break;      /* yield to discovery/send */
    }
}

int dart_node_poll(dart_node *n, int timeout_ms){
    uint8_t buf[DART_DGRAM_MAX]; uint32_t to; size_t ol; uint64_t now;
    dart_pollfd_t pfd[2]; int nf=1;

    dart_discovery_rt_poll(n->disc, 0);                 /* discovery tick (non-blocking) */

    memset(pfd,0,sizeof pfd);
    pfd[0].fd=n->fd; pfd[0].events=POLLIN;
    if (n->mcfd!=DART_BADSOCK){ pfd[1].fd=n->mcfd; pfd[1].events=POLLIN; nf=2; }
    if (DART_POLL(pfd,nf,timeout_ms) > 0){
        uint64_t rx_deadline = dart_now_us() + DART_RX_BUDGET_US;
        if (pfd[0].revents & POLLIN) dart__node_drain(n, n->fd, rx_deadline);
        if (nf==2 && (pfd[1].revents & POLLIN)) dart__node_drain(n, n->mcfd, rx_deadline);
    }

    now=dart_now_us();
    /* a datagram the socket refused last tick was already consumed from the core,
       so dropping it would lose best-effort data. Retry it before pulling new. */
    if (n->txhold_len && dart__node_tx(n, n->txhold_peer, n->txhold, n->txhold_len))
        n->txhold_len = 0;
    if (!n->txhold_len)
        while (dart_poll_send(n->tr,&to,buf,sizeof buf,&ol,now)){
            if (!dart__node_tx(n, to, buf, ol)){
                memcpy(n->txhold, buf, ol);
                n->txhold_len = ol; n->txhold_peer = to;
                break;          /* TX buffer full: yield this tick */
            }
            now=dart_now_us();
        }
    return 0;
}

int dart_node_send(dart_node *n, uint16_t channel_id, const void *data, size_t len){
    /* bounded backpressure (qos.max_block_us): let slow-but-live readers ack
       before un-acked history is overwritten. The wait pumps the node loop, so
       on_sample/on_gap may fire from inside this call. Releases on ack, reader
       death, or deadline; then the send proceeds. max_block_us == 0 never waits. */
    const dart_qos *q = dart_channel_qos(n->tr, channel_id);
    if (q && q->max_block_us && dart_send_would_evict(n->tr, channel_id)){
        uint64_t t0 = dart_now_us(), deadline = t0 + q->max_block_us;
        do {
            dart_node_poll(n, 1);
            if (!dart_send_would_evict(n->tr, channel_id)) break;
        } while (dart_now_us() < deadline);
        n->block_us += dart_now_us() - t0;
        n->block_n++;
    }
    return dart_send(n->tr, channel_id, data, len, dart_now_us());
}

int dart_node_set_dir(dart_node *n, uint16_t channel_id, uint8_t dir){
    return dart_set_dir(n->tr, channel_id, dir);
}

void dart_node_block_stats(dart_node *n, uint64_t *block_us, uint32_t *blocked_sends){
    if (block_us)      *block_us      = n->block_us;
    if (blocked_sends) *blocked_sends = n->block_n;
}

void dart_node_close(dart_node *n, int send_bye){
    if (!n) return;
    if (n->disc) dart_discovery_rt_close(n->disc, send_bye);
    if (n->mcfd != DART_BADSOCK) DART_CLOSESOCK(n->mcfd);
    if (n->fd != DART_BADSOCK) DART_CLOSESOCK(n->fd);
    dart__net_cleanup();
}
#endif /* !DART_TRANSPORT_SANS_IO */
#endif /* DART_TRANSPORT_IMPLEMENTATION */
