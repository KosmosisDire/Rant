/* sans-IO reliable-UDP transport core. See dart_transport.h. */
#include "dart_transport.h"
#include "dart_bytes.h"
#include <string.h>

/* byte 0 of every submessage: type in the low 3 bits, flags above */
#define DART_DATA 1
#define DART_HB   2
#define DART_NACK 3
#define DART_MSG_MASK 0x07u
#define DART_F_SINGLE 0x08u     /* DATA: single fragment (frag/count/len omitted) */
#define DART_F_UNPOS  0x10u     /* NACK: reader has delivered nothing yet */
#ifdef DART_SHM
#define DART_F_SHM    0x20u     /* DATA: SHM-DATA -- body is a descriptor, no payload */
#endif

#define DART_NACK_WINDOW 32u    /* seqnos covered by one ACKNACK bitmap */
#define DART__NIL 0xFFFFFFFFu

#ifndef DART_HB_SWEEP_US
#define DART_HB_SWEEP_US 25000u /* the timer sweep covers every lane this often */
#endif

#define DART__NO_DEADLINE ((uint64_t)-1)  /* next_deadline_us: nothing armed */

static void dart_bset(uint8_t*bitmap,uint32_t i){bitmap[i>>3]|=(uint8_t)(1u<<(i&7));}
static int  dart_bget(const uint8_t*bitmap,uint32_t i){return (bitmap[i>>3]>>(i&7))&1;}

/* FNV-1a over n bytes: the interest blob carries length-prefixed (not NUL-term)
 * names, so the identity is recomputed from the name on receive (it was redundant
 * on the wire, == dart_topic_id of the same name; init caps names at
 * DART_TOPIC_NAME_MAX so the wire name is the whole name). */
static uint64_t dart__id_n(const uint8_t *name, size_t n){
    uint64_t h = 1469598103934665603ull; size_t i;
    for (i=0;i<n;i++){ h ^= (uint64_t)name[i]; h *= 1099511628211ull; }
    return h;
}
uint64_t dart_topic_id(const char *name){
    uint64_t h = 1469598103934665603ull;   /* FNV-1a 64 offset basis */
    const unsigned char *p = (const unsigned char*)name;
    if (!name) return 0;
    for (; *p; p++){ h ^= (uint64_t)*p; h *= 1099511628211ull; }
    return h;
}
uint64_t dart_channel_identity(const dart_channel_def *def){
    return dart_topic_id(def->name);   /* the name is the cross-peer identity */
}
static size_t dart__namelen(const char *s){            /* capped strlen */
    size_t n = 0;
    if (s) while (s[n] && n < DART_TOPIC_NAME_MAX) n++;
    return n;
}

typedef struct {
    uint8_t  valid;
    uint64_t base;       /* seqno of frag 0 */
    uint16_t count;      /* frag count */
    uint32_t len;        /* message bytes */
    uint8_t *buf;        /* >= len bytes; arena (fixed) or hook-malloc'd (dynamic) */
    uint32_t cap;        /* allocated bytes of buf (dynamic grows it) */
#ifdef DART_SHM
    uint8_t  shm;        /* 1 = SHM-backed: bytes live in shm_buf, desc set, buf unused */
    const uint8_t *shm_buf;            /* external chunk payload (remote peers fragment from it) */
    uint8_t  desc[DART_SHM_DESC_BYTES];/* descriptor sent to SHM peers as one SHM-DATA */
#endif
} dart_writer_sample;

#ifdef DART_SHM
/* where a sample's bytes live: the external chunk for SHM samples, else our buf */
static const uint8_t *dart__sbuf(const dart_writer_sample *s){ return s->shm ? s->shm_buf : s->buf; }
#else
static const uint8_t *dart__sbuf(const dart_writer_sample *s){ return s->buf; }
#endif

typedef struct {        /* writer-side, per (channel,peer) */
    uint8_t  used;
    uint32_t reader_epoch; /* reader incarnation from last ACKNACK (0 = none); a change
                              means the peer rebuilt state, so the lane re-joins */
    uint64_t sent_upto;  /* next seqno to push as new data */
    uint64_t acked_upto; /* peer received all TUs < this */
    uint8_t  has_nack;   /* pending repair request from ACKNACK */
    uint64_t nack_base;
    uint32_t nack_bits;
    uint64_t hb_next_us; /* heartbeat timer */
    uint32_t hb_count;
} dart_writer_proxy;

typedef struct {        /* reader-side, per (channel,peer) */
    uint8_t  used;
    uint8_t  started;       /* accepted any DATA from this writer yet */
    uint32_t epoch;         /* this incarnation's id, sent in every ACKNACK */
    uint64_t deliver_upto;  /* base of current sample; all below delivered/skipped */
    uint8_t  assembly_active;    /* received >=1 frag of current sample */
    uint16_t assembly_count;
    uint16_t assembly_low;        /* lowest still-missing frag index of current sample (its
                               contiguous-received front); deliver_upto+assembly_low = first hole */
    uint32_t assembly_len;
    uint8_t *assembly_buf;       /* >= assembly_len; arena (fixed) or hook-malloc'd (dynamic) */
    uint8_t *frag_bitmap;       /* ceil(assembly_count/8) */
    uint32_t assembly_cap;       /* allocated bytes of assembly_buf (dynamic grows it) */
    uint32_t bitmap_cap;        /* allocated bytes of frag_bitmap */
    uint64_t hb_last;       /* highest seqno the writer CLAIMS to hold (heartbeat only) */
    uint64_t received_high;      /* highest seqno we have actually RECEIVED a frag for. UDP is
                               assumed in-order, so a hole below this is real loss to repair
                               while anything above it is still in flight: never NACK past it. */
    uint64_t nack_high;       /* highest seqno already requested this episode; refills ask only
                               (nack_high, top] so in-flight repairs are not re-requested */
    uint64_t nack_retransmit_us;  /* earliest time to re-request a stalled floor (lost-repair backstop) */
    uint8_t  ack_force;     /* a delivery/skip/HB/(re)match owes the writer an ACKNACK even if
                               the repair floor did not move (avoids a stuck cumulative ack) */
    uint8_t  ack_pending;
    uint64_t ack_due_us;
#ifdef DART_SHM
    uint8_t  shm_fail;      /* consecutive SHM-DATA resolve failures at deliver_upto */
#endif
} dart_reader_proxy;

#ifdef DART_SHM
#ifndef DART_SHM_MAX_RETRY
#define DART_SHM_MAX_RETRY 8u   /* give up on an unresolvable descriptor after this many */
#endif
#endif

typedef struct {
    dart_qos    qos;
    uint64_t  identity;     /* cross-peer topic identity (hash of name) */
    const char *name;       /* our copy of the topic name */
    uint16_t  max_fragments;     /* ceil(max_message_bytes/FRAG) (fixed mode only) */
    uint8_t   role;         /* dart_role */
    uint8_t   multicast;
    uint8_t   dynamic;      /* 1 = buffers grow via cfg.allocator, no fixed cap */
    uint16_t  n_subscribers;        /* live matched subscribers; multicast: >0 = group mode */
    /* writer */
    dart_writer_sample *history;       /* [depth] ring */
    uint16_t  history_head;    /* next slot to overwrite */
    uint64_t  next_seqno;
    uint64_t  first_seqno;  /* lowest seqno still cached */
    uint8_t   have_first;
    /* multicast group lane: new data emitted once for all subscribers */
    uint64_t  multicast_sent_upto;
    uint64_t  multicast_hb_next_us;
    uint32_t  multicast_hb_count;
    /* cumulative repair counters, summed over proxies; read via dart_repair_stats */
    dart_repair_stats_t repair_stats;
} dart_channel;

struct dart_state {
    dart_config    cfg;       /* n_channels = user channels (no internal channel) */
    uint32_t    *peer_ids;  /* [max_peers] */
    uint8_t     *peer_used; /* [max_peers] */
    uint8_t     *peer_local;/* [max_peers] */
    uint8_t     *peer_dormant;/* [max_peers] 1 = silent (discovery DROP): out of flow control,
                                 proxies + reader position preserved for a same-incarnation resume */
    uint16_t    *peer_frag; /* [max_peers] each peer's advertised fragment size (writer side) */
    uint16_t     frag;      /* this node's fragment size: what we fragment our sends into */
#ifdef DART_SHM
    uint8_t     *peer_shm;  /* [max_peers] 1 = peer can receive SHM-DATA (same host, attached) */
#endif
    /* peer interest over OUR channel table, one bit per user channel; the proxies
       plus these bits are the whole stored interest (full peer lists are not kept).
       Fed by dart_apply_peer_interest from the peer's discovery announce. */
    uint8_t     *peer_pub_bitmap; /* [max_peers][bitmap_len] peer publishes channel c */
    uint8_t     *peer_sub_bitmap; /* [max_peers][bitmap_len] peer subscribes channel c */
    uint16_t     bitmap_len;       /* ceil(n_channels / 8) */
    /* per-peer wire alias -> our channel index; the data path carries the 2-byte
       alias instead of the topic name */
    uint16_t    *alias_to_channel;    /* [max_peers * alias_max]; 0xFFFF = unmapped */
    uint32_t     alias_max;        /* alias-table stride = effective meta_max_ids */
    dart_channel  *channels;     /* [n_channels] */
    dart_writer_proxy   *writer_proxies;     /* [n_channels*max_peers] */
    dart_reader_proxy   *reader_proxies;     /* [n_channels*max_peers] */
    /* active-lane scheduler: a lane is one (channel,peer) pair or a channel's group
       lane. The event that gives a lane work enqueues it, so poll_send pays for work
       done, not idle lanes. Timer work is found by an amortized clock-driven sweep. */
    uint32_t    *lane_next;   /* [n_channels*(max_peers+1)] next in dest list */
    uint8_t     *lane_queued;    /* [n_channels*(max_peers+1)] queued flag */
    uint32_t    *dest_head;   /* [max_peers+n_channels] lane list per dest */
    uint32_t    *dest_tail;
    uint8_t     *dest_queued;
    uint32_t    *dest_queue;       /* ring of active destinations */
    uint32_t     dest_queue_head, dest_queue_count;
    uint32_t     sweep;       /* timer-sweep lane cursor */
    uint64_t     sweep_time_us;     /* clock position the sweep has paid for */
    uint64_t     next_deadline_us; /* earliest armed timer (ack/HB); caps the poll wait.
                                      Lower bound fed at arm sites, made exact by a full sweep.
                                      DART__NO_DEADLINE = nothing deferred (the hot path). */
    uint32_t     reader_epoch_counter;      /* reader-epoch counter (starts at 1; 0 = none) */
};

/* a deferred timer was (re)armed for absolute time t: keep next_deadline as the
   minimum so the poll wakes when it is due. t==0 is an immediate ack (woken via the
   active-lane queue, not a timer), so it is ignored here. */
static void dart__deadline(dart_state *st, uint64_t t){
    if (t && t < st->next_deadline_us) st->next_deadline_us = t;
}

/* writer/reader proxy for a (channel,peer) lane. The proxies are
   [n_channels][max_peers] row-major; centralizing the index math here keeps a
   transposed channel/peer from silently corrupting a neighbor lane. */
static dart_writer_proxy *dart__wp(dart_state *st, uint16_t channel_idx, uint32_t peer_slot){
    return &st->writer_proxies[(size_t)channel_idx*st->cfg.max_peers + peer_slot];
}
static dart_reader_proxy *dart__rp(dart_state *st, uint16_t channel_idx, uint32_t peer_slot){
    return &st->reader_proxies[(size_t)channel_idx*st->cfg.max_peers + peer_slot];
}

/* bump allocator (shared by required_memory and init) */
typedef struct { uint8_t *base; size_t offset; size_t cap; int oom; } dart_bump;
static void *dart_take(dart_bump *b, size_t n, size_t align){
    size_t a = (b->offset + (align-1)) & ~(align-1);
    b->offset = a + n;
    if (b->base){
        if (b->offset > b->cap){ b->oom = 1; return NULL; }
        return b->base + a;
    }
    return NULL; /* sizing mode */
}

/* Reader-side fragment-count bound: a peer may fragment at the smallest size in
 * the deployment, so size the reassembly bitmap by DART_FRAG_PAYLOAD_MIN. */
static uint16_t dart_max_fragments(uint32_t max_message_bytes){
    uint32_t f = (max_message_bytes + DART_FRAG_PAYLOAD_MIN - 1) / DART_FRAG_PAYLOAD_MIN;
    if (f == 0) f = 1;
    return (uint16_t)f;
}

/* zero-means-default for the tunable QoS fields, applied once at init so the
 * stored qos is authoritative */
#define DART_QOS_DEF_KEEP_LAST    1u
#define DART_QOS_DEF_HEARTBEAT_US 100000u   /* 100 ms idle writer heartbeat */
#define DART_QOS_DEF_REPAIR_US    20000u    /* 20 ms reader repair-request delay */
static void dart__qos_defaults(dart_qos *q, int dynamic){
    if (q->keep_last == 0)        q->keep_last       = DART_QOS_DEF_KEEP_LAST;
    if (q->heartbeat_us == 0)     q->heartbeat_us    = DART_QOS_DEF_HEARTBEAT_US;
    if (q->repair_delay_us == 0)  q->repair_delay_us = DART_QOS_DEF_REPAIR_US;
    /* fixed mode only: a dynamic channel keeps 0 = grow-to-fit via allocator */
    if (!dynamic && q->max_message_bytes == 0) q->max_message_bytes = DART_FRAG_PAYLOAD;
}

/* lay out everything (b->base==NULL = measure only) */
static dart_state *dart_build(dart_bump *b, const dart_config *cfg){
    uint16_t c, p; uint32_t max_peers = cfg->max_peers, n_channels = cfg->n_channels;
    uint16_t bitmap_len = (uint16_t)((n_channels+7u)/8u);
    uint32_t meta_ids = DART_META_MAX_IDS;
    uint32_t name_bytes = 0; char *name_pool = NULL; uint32_t name_cursor = 0;
    dart_state *st = (dart_state*)dart_take(b, sizeof(dart_state), 16);
    if (st && b->base) memset(st, 0, sizeof(*st));

    /* name pool (our copies of the topic names) */
    for (c=0;c<n_channels;c++){
        size_t lane = dart__namelen(cfg->channels[c].name);
        if (lane) name_bytes += (uint32_t)lane + 1u;
    }
    if (meta_ids < 2u*n_channels) meta_ids = 2u*n_channels;     /* our own interest list must always fit */

    { uint32_t nlanes = n_channels*(max_peers+1u), ndest = max_peers+n_channels;
      uint32_t *peer_ids = (uint32_t*)dart_take(b, max_peers*sizeof(uint32_t), 8);
      uint8_t  *peer_used = (uint8_t*) dart_take(b, max_peers*sizeof(uint8_t), 1);
      uint8_t  *peer_local = (uint8_t*) dart_take(b, max_peers*sizeof(uint8_t), 1);
      uint8_t  *peer_dormant= (uint8_t*) dart_take(b, max_peers*sizeof(uint8_t), 1);
      uint16_t *peer_frag = (uint16_t*)dart_take(b, max_peers*sizeof(uint16_t), 2);
#ifdef DART_SHM
      uint8_t  *peer_shm= (uint8_t*) dart_take(b, max_peers*sizeof(uint8_t), 1);
#endif
      uint8_t  *peer_pub_bitmap = (uint8_t*) dart_take(b, (size_t)max_peers*bitmap_len, 1);
      uint8_t  *peer_sub_bitmap = (uint8_t*) dart_take(b, (size_t)max_peers*bitmap_len, 1);
      dart_channel *ch = (dart_channel*)dart_take(b, n_channels*sizeof(dart_channel), 16);
      dart_writer_proxy *writer_proxies = (dart_writer_proxy*)dart_take(b, (size_t)n_channels*max_peers*sizeof(dart_writer_proxy), 16);
      dart_reader_proxy *reader_proxies = (dart_reader_proxy*)dart_take(b, (size_t)n_channels*max_peers*sizeof(dart_reader_proxy), 16);
      uint32_t *lane_next = (uint32_t*)dart_take(b, (size_t)nlanes*sizeof(uint32_t), 8);
      uint8_t  *lane_queued = (uint8_t*) dart_take(b, (size_t)nlanes, 1);
      uint32_t *dest_head = (uint32_t*)dart_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint32_t *dest_tail = (uint32_t*)dart_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint8_t  *dest_queued = (uint8_t*) dart_take(b, (size_t)ndest, 1);
      uint32_t *dest_queue = (uint32_t*)dart_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint16_t *alias_to_channel = (uint16_t*)dart_take(b, (size_t)max_peers*meta_ids*sizeof(uint16_t), 2);
      name_pool = (char*)dart_take(b, name_bytes ? name_bytes : 1u, 1);
      if (st && b->base){
          st->cfg=*cfg; st->peer_ids=peer_ids; st->peer_used=peer_used; st->peer_local=peer_local;
          st->peer_dormant=peer_dormant; st->peer_frag=peer_frag;
          st->frag = cfg->frag_payload ? cfg->frag_payload : DART_FRAG_PAYLOAD;
          if (st->frag < DART_FRAG_PAYLOAD_MIN) st->frag = DART_FRAG_PAYLOAD_MIN;
          if (st->frag > DART_FRAG_PAYLOAD_MAX) st->frag = DART_FRAG_PAYLOAD_MAX;
          st->peer_pub_bitmap=peer_pub_bitmap; st->peer_sub_bitmap=peer_sub_bitmap; st->bitmap_len=bitmap_len;
          st->channels=ch; st->writer_proxies=writer_proxies; st->reader_proxies=reader_proxies; st->reader_epoch_counter=1;
          st->next_deadline_us=DART__NO_DEADLINE;
          st->lane_next=lane_next; st->lane_queued=lane_queued;
          st->dest_head=dest_head; st->dest_tail=dest_tail; st->dest_queued=dest_queued; st->dest_queue=dest_queue;
          st->alias_to_channel=alias_to_channel; st->alias_max=meta_ids;
          memset(peer_used,0,max_peers); memset(peer_local,0,max_peers); memset(peer_dormant,0,max_peers);
          { uint32_t k; for (k=0;k<max_peers;k++) peer_frag[k]=DART_FRAG_PAYLOAD; }  /* set per peer on add */
#ifdef DART_SHM
          st->peer_shm=peer_shm; memset(peer_shm,0,max_peers);
#endif
          memset(alias_to_channel,0xFF,(size_t)max_peers*meta_ids*sizeof(uint16_t));   /* all unmapped */
          memset(peer_pub_bitmap,0,(size_t)max_peers*bitmap_len); memset(peer_sub_bitmap,0,(size_t)max_peers*bitmap_len);
          memset(writer_proxies,0,(size_t)n_channels*max_peers*sizeof(dart_writer_proxy));
          memset(reader_proxies,0,(size_t)n_channels*max_peers*sizeof(dart_reader_proxy));
          memset(lane_queued,0,nlanes); memset(dest_queued,0,ndest);
          memset(dest_head,0xFF,(size_t)ndest*sizeof(uint32_t));   /* all DART__NIL */
      }
    }

    for (c=0;c<n_channels;c++){
        const dart_channel_def *def = &cfg->channels[c];
        /* dynamic = an allocator is set: buffers grow via the hook, not the arena */
        int dyn = (cfg->allocator != NULL);
        dart_qos q = def->qos;            /* local, normalized copy */
        dart_writer_sample *history; uint16_t depth, max_fragments, d;
        dart__qos_defaults(&q, dyn);
        depth = q.keep_last;
        max_fragments = dart_max_fragments(q.max_message_bytes);
        history = (dart_writer_sample*)dart_take(b, depth*sizeof(dart_writer_sample), 16);
        if (st && b->base){
            dart_channel *ch = &st->channels[c];
            size_t lane = dart__namelen(def->name);
            memset(ch,0,sizeof(*ch));
            ch->qos=q; ch->max_fragments=max_fragments;
            ch->role=def->role; ch->multicast=def->multicast; ch->dynamic=(uint8_t)dyn;
            ch->identity = dart_channel_identity(def);
            ch->name = NULL;
            if (lane){ char *dst = name_pool + name_cursor;
                    memcpy(dst, def->name, lane); dst[lane]='\0';
                    ch->name = dst; name_cursor += (uint32_t)(lane + 1u); }
            ch->history=history; ch->history_head=0; ch->next_seqno=0; ch->have_first=0;
            memset(history,0,depth*sizeof(dart_writer_sample));
        }
        for (d=0; d<depth; d++){
            uint8_t *buf = dyn ? NULL : (uint8_t*)dart_take(b, q.max_message_bytes, 8);
            if (st && b->base){ st->channels[c].history[d].buf = buf;
                                st->channels[c].history[d].cap = dyn ? 0u : q.max_message_bytes; }
        }
        /* reader asm buffers + frag bitmaps, per peer (skipped when dynamic) */
        for (p=0;p<max_peers;p++){
            uint8_t *assembly_buf = dyn ? NULL : (uint8_t*)dart_take(b, q.max_message_bytes, 8);
            uint8_t *frag_bitmap  = dyn ? NULL : (uint8_t*)dart_take(b, (max_fragments+7u)/8u, 1);
            if (st && b->base){
                dart_reader_proxy *r = dart__rp(st,c,p);
                r->assembly_buf=assembly_buf; r->frag_bitmap=frag_bitmap;
                r->assembly_cap = dyn ? 0u : q.max_message_bytes;
                r->bitmap_cap  = dyn ? 0u : (uint32_t)((max_fragments+7u)/8u);
            }
        }
    }
    return st;
}

size_t dart_required_memory(const dart_config *cfg){
    dart_bump b; memset(&b,0,sizeof b);
    if (!cfg || cfg->n_channels==0 || cfg->max_peers==0) return 0;
    dart_build(&b, cfg);
    return b.offset + 16;   /* slack for base alignment */
}

dart_state *dart_init(void *mem, size_t cap, const dart_config *cfg){
    dart_bump b; dart_state *st; uint16_t i;
    if (!mem || !cfg || cfg->n_channels==0 || cfg->max_peers==0) return NULL;
    for (i=0;i<cfg->n_channels;i++){
        const dart_channel_def *d = &cfg->channels[i];
        size_t lane = 0;
        if (!d->name || !d->name[0]) return NULL;          /* name = identity, required */
        while (d->name[lane]) lane++;
        if (lane > DART_TOPIC_NAME_MAX) return NULL;           /* the wire name is the whole name */
    }
    memset(&b,0,sizeof b);
    b.base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    b.cap  = cap - (size_t)((uint8_t*)b.base - (uint8_t*)mem);
    st = dart_build(&b, cfg);
    if (!st || b.oom) return NULL;
    st->cfg.channels = NULL;   /* only read during init; detach the caller's pointer */
    return st;
}

static int dart_peer_slot(dart_state *st, uint32_t id){
    uint16_t i;
    for (i=0;i<st->cfg.max_peers;i++) if (st->peer_used[i] && st->peer_ids[i]==id) return (int)i;
    return -1;
}
/* the local handle IS the channel's index; out-of-range rejected */
static dart_channel *dart_chan(dart_state *st, uint16_t channel, int *idx_out){
    if (channel >= st->cfg.n_channels) return NULL;
    if (idx_out) *idx_out = (int)channel;
    return &st->channels[channel];
}
/* RX demux: find the local channel for a wire identity */
static dart_channel *dart_chan_by_identity(dart_state *st, uint64_t identity, int *idx_out){
    uint16_t i;
    for (i=0;i<st->cfg.n_channels;i++) if (st->channels[i].identity==identity){ if(idx_out)*idx_out=(int)i; return &st->channels[i]; }
    return NULL;
}

/* fire one dart_event (no-op if no on_event). Transport emits MSG_LOST/TOO_BIG/COLLISION. */
static void dart__event(dart_state *st, dart_event_kind kind, uint16_t channel,
                        uint32_t peer, uint64_t first, uint64_t count, const char *detail){
    dart_event ev;
    if (!st->cfg.on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind=kind; ev.channel=channel; ev.peer=peer; ev.first=first; ev.count=count;
    ev.detail=detail;
    st->cfg.on_event(st->cfg.user, &ev);
}

/* scheduler: lane index = channel_idx*(max_peers+1)+peer_slot (peer_slot==max_peers = group lane);
 * destination = peer slot peer_slot, or max_peers+channel_idx for a group lane */
static void dart__dest_push(dart_state *st, uint32_t d){
    uint32_t ndest = st->cfg.max_peers + (uint32_t)st->cfg.n_channels, t;
    if (st->dest_queued[d]) return;
    st->dest_queued[d]=1;
    t = st->dest_queue_head + st->dest_queue_count;
    if (t >= ndest) t -= ndest;
    st->dest_queue[t]=d; st->dest_queue_count++;
}

/* enqueue a lane that just got sendable work; idempotent while queued */
static void dart__lane_enq(dart_state *st, uint16_t channel_idx, uint32_t peer_slot){
    uint32_t max_peers=st->cfg.max_peers, lanes=max_peers+1u;
    uint32_t lane=(uint32_t)channel_idx*lanes+peer_slot;
    uint32_t d=(peer_slot<max_peers) ? peer_slot : max_peers+(uint32_t)channel_idx;
    if (st->lane_queued[lane]) return;
    st->lane_queued[lane]=1; st->lane_next[lane]=DART__NIL;
    if (st->dest_head[d]==DART__NIL) st->dest_head[d]=lane;
    else st->lane_next[st->dest_tail[d]]=lane;
    st->dest_tail[d]=lane;
    dart__dest_push(st, d);
}

/* enqueue, and track a freshly-armed reader ack/NACK deadline for the poll cap. Used
 * by the arm sites (ack_due_us is future or 0); the sweep enqueues due lanes with
 * dart__lane_enq instead, since it recomputes next_deadline itself. */
static void dart__lane_wake(dart_state *st, uint16_t channel_idx, uint32_t peer_slot){
    if (peer_slot < st->cfg.max_peers){
        dart_reader_proxy *r=dart__rp(st,channel_idx,peer_slot);
        if (r->used && r->ack_pending) dart__deadline(st, r->ack_due_us);
    }
    dart__lane_enq(st, channel_idx, peer_slot);
}

/* unicast join seqno: head minus qos.catch_up cached samples (reliable only) */
static uint64_t dart_unicast_join_seqno(const dart_channel *ch){
    uint16_t depth = ch->qos.keep_last;   /* normalized at init (>=1) */
    uint16_t want = ch->qos.catch_up, k, i;
    uint64_t s = ch->next_seqno;
    if (ch->qos.reliability != DART_RELIABLE || want == 0) return s;
    if (want > depth) want = depth;
    i = ch->history_head;
    for (k=0; k<want; k++){
        uint16_t j = (uint16_t)(i ? i-1 : depth-1);
        if (!ch->history[j].valid) break;        /* fewer than want cached */
        s = ch->history[j].base;
        i = j;
    }
    return s;
}

/* match one (channel,peer) proxy: a multicast channel engages its group lane on
 * the first subscriber; per-peer lanes then carry repairs only */
static void dart__match_w(dart_state *st, uint16_t c, uint16_t peer_slot){
    dart_channel *ch=&st->channels[c];
    dart_writer_proxy *w=dart__wp(st,c,peer_slot);
    memset(w,0,sizeof(*w));
    w->used=1;
    if (!ch->multicast){
        w->sent_upto = dart_unicast_join_seqno(ch);
    } else {
        /* first subscriber engages group mode at the head; reliable-from-join-point,
           later joiners backfill via NACK */
        if (ch->n_subscribers==0) ch->multicast_sent_upto = ch->next_seqno;
        ch->n_subscribers++;
        w->sent_upto = ch->multicast_sent_upto;
    }
    w->acked_upto = w->sent_upto;
    dart__lane_wake(st, c, peer_slot);   /* unicast repair lane primed + ack/hb */
}
static void dart__unmatch_w(dart_state *st, uint16_t c, uint16_t peer_slot){
    dart_channel *ch=&st->channels[c];
    dart_writer_proxy *w=dart__wp(st,c,peer_slot);
    if (!w->used) return;
    if (ch->multicast && ch->n_subscribers) ch->n_subscribers--;
    w->used=0;
}
static void dart__match_r(dart_state *st, uint16_t c, uint16_t peer_slot){
    dart_reader_proxy *r=dart__rp(st,c,peer_slot);
    uint8_t *assembly_buf=r->assembly_buf, *frag_bitmap=r->frag_bitmap;
    uint32_t assembly_cap=r->assembly_cap, bitmap_cap=r->bitmap_cap;   /* keep grown buffers across rematch */
    memset(r,0,sizeof(*r));
    r->assembly_buf=assembly_buf; r->frag_bitmap=frag_bitmap; r->assembly_cap=assembly_cap; r->bitmap_cap=bitmap_cap;
    r->epoch=st->reader_epoch_counter++;   /* new incarnation: writers re-join on seeing it */
    r->used=1;       /* started==0: first DATA adopts the writer's position */
    /* announce this incarnation once so a caught-up (idle, non-pinging) writer
       re-joins and replays. A genuine discovery blip keeps its position through
       dart_peer_dormant/resume and never lands here, so a single ACKNACK suffices. */
    if (st->channels[c].qos.reliability==DART_RELIABLE){
        r->ack_pending=1; r->ack_due_us=0; r->ack_force=1;
        dart__lane_wake(st,c,peer_slot);
    }
}
static void dart__unmatch_r(dart_state *st, uint16_t c, uint16_t peer_slot){
    dart_reader_proxy *r=dart__rp(st,c,peer_slot);
    r->used=0; r->assembly_active=0;
}

/* recompute one (channel,peer) match from our role and the peer's interest bits */
static void dart__rematch(dart_state *st, uint16_t c, uint16_t peer_slot){
    dart_channel *ch=&st->channels[c];
    const uint8_t *peer_pub_bitmap=&st->peer_pub_bitmap[(size_t)peer_slot*st->bitmap_len];
    const uint8_t *peer_sub_bitmap=&st->peer_sub_bitmap[(size_t)peer_slot*st->bitmap_len];
    int wuse = (ch->role==DART_PUBSUB || ch->role==DART_PUB_ONLY) && dart_bget(peer_sub_bitmap,c);
    int ruse = (ch->role==DART_PUBSUB || ch->role==DART_SUB_ONLY) && dart_bget(peer_pub_bitmap,c);
    dart_writer_proxy *w=dart__wp(st,c,peer_slot);
    dart_reader_proxy *r=dart__rp(st,c,peer_slot);
    if (wuse && !w->used) dart__match_w(st,c,peer_slot);
    else if (!wuse && w->used) dart__unmatch_w(st,c,peer_slot);
    if (ruse && !r->used) dart__match_r(st,c,peer_slot);
    else if (!ruse && r->used) dart__unmatch_r(st,c,peer_slot);
}

static uint16_t dart__clamp_frag(uint16_t f){
    if (f==0) f = DART_FRAG_PAYLOAD;
    if (f < DART_FRAG_PAYLOAD_MIN) f = DART_FRAG_PAYLOAD_MIN;
    if (f > DART_FRAG_PAYLOAD_MAX) f = DART_FRAG_PAYLOAD_MAX;
    return f;
}

void dart_peer_add(dart_state *st, uint32_t id, int peer_is_local, uint16_t peer_frag){
    uint16_t i; int free=-1; uint32_t max_peers=st->cfg.max_peers;
    if (dart_peer_slot(st,id)>=0) return;
    for (i=0;i<max_peers;i++) if(!st->peer_used[i]){free=(int)i;break;}
    if (free<0) return;
    st->peer_used[free]=1; st->peer_ids[free]=id;
    st->peer_local[free]=(uint8_t)(peer_is_local?1:0);
    st->peer_dormant[free]=0;
    st->peer_frag[free]=dart__clamp_frag(peer_frag);
#ifdef DART_SHM
    st->peer_shm[free]=0;   /* node sets it once the peer's segment is attached */
#endif
    memset(&st->peer_pub_bitmap[(size_t)free*st->bitmap_len],0,st->bitmap_len);
    memset(&st->peer_sub_bitmap[(size_t)free*st->bitmap_len],0,st->bitmap_len);
    memset(&st->alias_to_channel[(size_t)free*st->alias_max],0xFF,(size_t)st->alias_max*sizeof(uint16_t));
    /* nothing matches until dart_apply_peer_interest feeds the peer's interest
       list (carried in its discovery announce) */
}

void dart_peer_remove(dart_state *st, uint32_t id){
    int s = dart_peer_slot(st,id); uint16_t c;
    if (s<0) return;
    for (c=0;c<st->cfg.n_channels;c++){
        dart__unmatch_w(st,c,(uint16_t)s);
        dart__unmatch_r(st,c,(uint16_t)s);
    }
    st->peer_used[s]=0; st->peer_dormant[s]=0;
#ifdef DART_SHM
    st->peer_shm[s]=0;
#endif
}

/* A peer fell silent (discovery timeout): keep every proxy and the reader's
 * deliver position, just drop the peer from flow control so a dead reader can't
 * stall the writer and a dead writer isn't acked. State revives via dart_peer_resume. */
void dart_peer_dormant(dart_state *st, uint32_t id){
    int s = dart_peer_slot(st,id);
    if (s>=0) st->peer_dormant[s]=1;
}

/* A dormant peer's SAME incarnation returned: re-include it in flow control and
 * re-report each reader position so the writer fills any gap (the reader dedups any
 * replay for free). The writer side needs nothing proactive; the reader's ACKNACK
 * re-arms its heartbeats. Proxies and deliver_upto were never touched, so no dup,
 * no loss. */
void dart_peer_resume(dart_state *st, uint32_t id){
    int s = dart_peer_slot(st,id); uint16_t c;
    if (s<0) return;
    st->peer_dormant[s]=0;
    for (c=0;c<st->cfg.n_channels;c++){
        dart_writer_proxy *w=dart__wp(st,c,s);
        dart_reader_proxy *r=dart__rp(st,c,s);
        if (st->channels[c].qos.reliability!=DART_RELIABLE) continue;
        if (r->used){ r->ack_pending=1; r->ack_due_us=0; r->ack_force=1; }  /* report our position now */
        if (w->used || r->used) dart__lane_wake(st,c,(uint16_t)s);
    }
}

/* update a peer's advertised fragment size (its blob may arrive after first contact) */
void dart_peer_set_frag(dart_state *st, uint32_t id, uint16_t peer_frag){
    int s = dart_peer_slot(st,id);
    if (s>=0) st->peer_frag[s]=dart__clamp_frag(peer_frag);
}

#ifdef DART_SHM
void dart_peer_set_shm(dart_state *st, uint32_t id, int is_shm){
    int s = dart_peer_slot(st,id);
    if (s>=0) st->peer_shm[s]=(uint8_t)(is_shm?1:0);
}
#endif

/* find cached sample containing seqno (newest-first, so pushing new data is O(1)) */
static dart_writer_sample *dart_find_sample(dart_channel *ch, uint64_t seqno){
    uint16_t depth = ch->qos.keep_last, k;
    uint16_t i = ch->history_head;
    for (k=0;k<depth;k++){
        dart_writer_sample *s;
        i = (uint16_t)(i ? i-1 : depth-1);
        s = &ch->history[i];
        if (!s->valid) break;                  /* reached the unwritten tail */
        if (seqno >= s->base)
            return (seqno < s->base + s->count) ? s : NULL;
    }
    return NULL;
}

/* append the filled head slot to history and wake the lanes that carry it */
static void dart__commit(dart_state *st, uint16_t channel_idx, size_t len){
    dart_channel *ch = &st->channels[channel_idx];
    uint16_t depth = ch->qos.keep_last;
    uint16_t count = (uint16_t)((len + st->frag - 1) / st->frag);
    dart_writer_sample *slot = &ch->history[ch->history_head];
    if (count==0) count=1;
    slot->valid=1; slot->base=ch->next_seqno; slot->count=count; slot->len=(uint32_t)len;
    ch->history_head = (uint16_t)((ch->history_head+1) % depth);
    ch->next_seqno += count;
    /* oldest cached: where the head points once wrapped, else slot 0 */
    ch->first_seqno = ch->history[ch->history_head].valid ? ch->history[ch->history_head].base
                                                    : ch->history[0].base;
    ch->have_first  = 1;
    if (ch->multicast && ch->n_subscribers>0)
        dart__lane_wake(st, channel_idx, st->cfg.max_peers);    /* group lane */
    else {
        uint32_t max_peers=st->cfg.max_peers, p;
        for (p=0;p<max_peers;p++)
            if (dart__wp(st,channel_idx,p)->used && !st->peer_dormant[p]) dart__lane_wake(st, channel_idx, p);
    }
}

int dart_send(dart_state *st, uint16_t channel, const void *data, size_t len, uint64_t now){
    int channel_idx; dart_channel *ch;
    (void)now;
    ch = dart_chan(st, channel, &channel_idx);                /* rejects the internal meta channel */
    if (!ch) return -1;
    if (ch->dynamic){
        dart_writer_sample *slot = &ch->history[ch->history_head];
        size_t need = len ? len : 1u;
        if (len > 65535u*(uint32_t)st->frag) return -2;   /* wire fragment-count cap */
        if ((size_t)slot->cap < need){                    /* grow the slot to fit */
            uint8_t *new_buf = (uint8_t*)st->cfg.allocator(st->cfg.user, slot->buf, need);
            if (!new_buf) return -4;                           /* out of memory */
            slot->buf = new_buf; slot->cap = (uint32_t)need;
        }
    } else if (len > ch->qos.max_message_bytes) return -2;
    if (ch->role == DART_SUB_ONLY || ch->role == DART_INACTIVE) return -3;
    if (len) memcpy(ch->history[ch->history_head].buf, data, len);
#ifdef DART_SHM
    ch->history[ch->history_head].shm = 0;   /* an inline send: this slot is not SHM-backed */
#endif
    dart__commit(st, (uint16_t)channel_idx, len);
    return 0;
}

#ifdef DART_SHM
/* publish a sample whose bytes live in an external (shared-memory) chunk: store the
 * chunk pointer + descriptor on the history slot without copying. Remote peers
 * fragment from the chunk; SHM peers get the one-submessage descriptor. */
int dart_send_shm(dart_state *st, uint16_t channel, const void *chunk, size_t len,
                  const uint8_t *desc, uint64_t now){
    int channel_idx; dart_channel *ch; dart_writer_sample *slot;
    (void)now;
    ch = dart_chan(st, channel, &channel_idx);
    if (!ch) return -1;
    if (len > 65535u*(uint32_t)st->frag) return -2;      /* wire fragment-count cap */
    if (ch->role == DART_SUB_ONLY || ch->role == DART_INACTIVE) return -3;
    slot = &ch->history[ch->history_head];
    slot->shm = 1;
    slot->shm_buf = (const uint8_t*)chunk;
    memcpy(slot->desc, desc, DART_SHM_DESC_BYTES);
    dart__commit(st, (uint16_t)channel_idx, len);
    return 0;
}
#endif

void dart_destroy(dart_state *st){
    uint16_t c; uint32_t p, max_peers;
    if (!st || !st->cfg.allocator) return;     /* fixed mode: nothing hook-allocated */
    max_peers = st->cfg.max_peers;
    for (c=0;c<st->cfg.n_channels;c++){
        dart_channel *ch=&st->channels[c];
        uint16_t depth, d;
        if (!ch->dynamic) continue;
        depth = ch->qos.keep_last;
        for (d=0; d<depth; d++)
            if (ch->history[d].buf){ st->cfg.allocator(st->cfg.user, ch->history[d].buf, 0);
                                  ch->history[d].buf=NULL; ch->history[d].cap=0; }
        for (p=0;p<max_peers;p++){
            dart_reader_proxy *r=dart__rp(st,c,p);
            if (r->assembly_buf){ st->cfg.allocator(st->cfg.user, r->assembly_buf, 0); r->assembly_buf=NULL; r->assembly_cap=0; }
            if (r->frag_bitmap){ st->cfg.allocator(st->cfg.user, r->frag_bitmap, 0); r->frag_bitmap=NULL; r->bitmap_cap=0; }
        }
    }
}

/* one interest entry: [u16 alias][u8 namelen][name]. The name rides along so a
 * hash collision is detected (not cross-wired); the identity is recomputed from it. */
static uint8_t *dart__meta_put(uint8_t *p, uint16_t alias, const dart_channel *ch){
    size_t lane = dart__namelen(ch->name);
    dart_le_w16(p, alias); p += 2;
    *p++ = (uint8_t)lane;
    if (lane){ memcpy(p, ch->name, lane); p += lane; }
    return p;
}
static int dart__meta_name_eq(const dart_channel *ch, const uint8_t *name, size_t nlen){
    size_t ours = dart__namelen(ch->name);
    if (nlen != ours) return 0;
    return nlen==0 ? 1 : (memcmp(ch->name, name, nlen)==0);
}
/* match count entries to local channels by identity (recomputed from each name),
 * recording the alias map. Same-identity-different-name is a collision: refused. */
static const uint8_t *dart__meta_scan(dart_state *st, int peer_slot, const uint8_t *p,
                                      uint32_t count, uint8_t *bitmap){
    uint32_t k;
    for (k=0;k<count;k++){
        uint16_t alias=dart_le_r16(p); uint32_t nlen=p[2]; const uint8_t *name=p+3; int channel_idx;
        uint64_t id=dart__id_n(name,nlen);
        dart_channel *ch=dart_chan_by_identity(st,id,&channel_idx);
        p = name + nlen;
        if (!ch) continue;                                  /* not ours */
        if (dart__meta_name_eq(ch,name,nlen)){
            dart_bset(bitmap,(uint32_t)channel_idx);
            if ((uint32_t)alias < st->alias_max)
                st->alias_to_channel[(size_t)peer_slot*st->alias_max + alias] = (uint16_t)channel_idx;
        }
        else
            dart__event(st, DART_NAME_COLLISION, (uint16_t)channel_idx, st->peer_ids[peer_slot],
                        id, 0, ch->name ? ch->name : "");
    }
    return p;
}

/* Upper bound on dart_build_interest output, for sizing the announce buffer: a
 * PUBSUB channel appears in both lists, so 2*n_channels max-length entries. */
size_t dart_interest_max(uint16_t n_channels){
    return 4u + (size_t)(2u+1u+DART_TOPIC_NAME_MAX) * 2u * (size_t)n_channels;
}

/* Serialize our interest into out: [u16 npub][u16 nsub][pub..][sub..], each entry
 * [u16 alias][u8 namelen][name]. Returns bytes written, or 0 if cap is too small.
 * The node carries this in its discovery announce; size out via dart_interest_max. */
size_t dart_build_interest(dart_state *st, void *out, size_t cap){
    uint8_t *o=(uint8_t*)out, *p, *end=o+cap;
    uint16_t c; uint32_t n_pub=0, n_sub=0;
    if (cap < 4) return 0;
    p=o+4;
    for (c=0;c<st->cfg.n_channels;c++){
        uint8_t d=st->channels[c].role;
        if (d==DART_PUBSUB || d==DART_PUB_ONLY){
            if (p + 3u + dart__namelen(st->channels[c].name) > end) return 0;
            p=dart__meta_put(p,c,&st->channels[c]); n_pub++;
        }
    }
    for (c=0;c<st->cfg.n_channels;c++){
        uint8_t d=st->channels[c].role;
        if (d==DART_PUBSUB || d==DART_SUB_ONLY){
            if (p + 3u + dart__namelen(st->channels[c].name) > end) return 0;
            p=dart__meta_put(p,c,&st->channels[c]); n_sub++;
        }
    }
    dart_le_w16(o,(uint16_t)n_pub); dart_le_w16(o+2,(uint16_t)n_sub);
    return (size_t)(p - o);
}

/* A peer's interest list arrived (from its discovery announce): refresh its bits
 * and rematch every channel. Idempotent; re-applying re-derives all matches. */
void dart_apply_peer_interest(dart_state *st, uint32_t peer_id, const void *blob, size_t len){
    const uint8_t *d=(const uint8_t*)blob, *p, *end=d+len;
    uint16_t n_pub, n_sub, c; int peer_slot=dart_peer_slot(st,peer_id);
    uint8_t *peer_pub_bitmap, *peer_sub_bitmap;
    if (peer_slot<0 || len<4) return;
    peer_pub_bitmap=&st->peer_pub_bitmap[(size_t)peer_slot*st->bitmap_len];
    peer_sub_bitmap=&st->peer_sub_bitmap[(size_t)peer_slot*st->bitmap_len];
    n_pub=dart_le_r16(d); n_sub=dart_le_r16(d+2);
    /* validate the whole variable-length list first: a truncated blob must not drop a match */
    { uint32_t k, tot=(uint32_t)n_pub+n_sub; p=d+4;
      for (k=0;k<tot;k++){
          if (p+3 > end) return;
          p += 3u + (uint32_t)p[2];
          if (p > end) return;
      } }
    memset(peer_pub_bitmap,0,st->bitmap_len); memset(peer_sub_bitmap,0,st->bitmap_len);
    memset(&st->alias_to_channel[(size_t)peer_slot*st->alias_max],0xFF,(size_t)st->alias_max*sizeof(uint16_t));
    p = dart__meta_scan(st, peer_slot, d+4, n_pub, peer_pub_bitmap);
    p = dart__meta_scan(st, peer_slot, p,   n_sub, peer_sub_bitmap);
    for (c=0;c<st->cfg.n_channels;c++) dart__rematch(st,c,(uint16_t)peer_slot);
}

int dart_set_role(dart_state *st, uint16_t channel, uint8_t role){
    int channel_idx; dart_channel *ch; uint16_t p;
    if (role > DART_INACTIVE) return -1;
    ch = dart_chan(st, channel, &channel_idx);
    if (!ch) return -1;
    if (ch->role == role) return 0;
    ch->role = role;
    for (p=0;p<st->cfg.max_peers;p++)
        if (st->peer_used[p]) dart__rematch(st,(uint16_t)channel_idx,p);
    /* caller re-advertises interest (the node bumps its discovery announce) */
    return 0;
}

const dart_qos *dart_channel_qos(dart_state *st, uint16_t channel){
    dart_channel *ch = dart_chan(st, channel, NULL);
    return ch ? &ch->qos : NULL;
}

int dart_send_would_evict(dart_state *st, uint16_t channel){
    int channel_idx; dart_channel *ch = dart_chan(st, channel, &channel_idx);
    dart_writer_sample *slot; uint32_t max_peers; uint16_t p;
    if (!ch || ch->qos.reliability != DART_RELIABLE) return 0;
    slot = &ch->history[ch->history_head];        /* slot the next send overwrites */
    if (!slot->valid) return 0;
    max_peers = st->cfg.max_peers;
    for (p=0;p<(uint16_t)max_peers;p++){
        dart_writer_proxy *w=dart__wp(st,channel_idx,p);
        if (w->used && !st->peer_dormant[p] && w->acked_upto < slot->base + slot->count) return 1;
    }
    return 0;
}

int dart_send_drained(dart_state *st, uint16_t channel){
    int channel_idx; dart_channel *ch = dart_chan(st, channel, &channel_idx);
    uint32_t max_peers; uint16_t p;
    if (!ch || ch->qos.reliability != DART_RELIABLE) return 1;  /* no acks to await */
    max_peers = st->cfg.max_peers;
    for (p=0;p<(uint16_t)max_peers;p++){
        dart_writer_proxy *w=dart__wp(st,channel_idx,p);
        if (w->used && !st->peer_dormant[p] && w->acked_upto < ch->next_seqno) return 0;  /* reader still behind */
    }
    return 1;
}

int dart_writer_match_count(dart_state *st, uint16_t channel){
    int channel_idx; dart_channel *ch = dart_chan(st, channel, &channel_idx);
    uint32_t max_peers, p; int cnt = 0;
    if (!ch) return 0;
    max_peers = st->cfg.max_peers;
    for (p=0;p<max_peers;p++) if (dart__wp(st,channel_idx,p)->used) cnt++;  /* matched readers */
    return cnt;
}

void dart_repair_stats(dart_state *st, uint16_t channel, dart_repair_stats_t *out){
    dart_channel *ch = dart_chan(st, channel, NULL);
    if (!out) return;
    if (ch) *out = ch->repair_stats;
    else memset(out, 0, sizeof *out);
}

int dart_repair_pending(dart_state *st, uint16_t channel){
    int channel_idx; dart_channel *ch = dart_chan(st, channel, &channel_idx);
    uint32_t max_peers, p; int cnt = 0;
    if (!ch) return 0;
    max_peers = st->cfg.max_peers;
    for (p=0;p<max_peers;p++){ dart_writer_proxy *w=dart__wp(st,channel_idx,p); if (w->used && w->has_nack) cnt++; }
    return cnt;   /* writer lanes with a NACK to service; 0 = nothing to resend right now */
}

int dart_reader_progress(dart_state *st, uint16_t channel, uint32_t peer,
                         uint64_t *base_seqno, uint32_t *have, uint32_t *total){
    int channel_idx; dart_channel *ch = dart_chan(st, channel, &channel_idx);
    int peer_slot; dart_reader_proxy *r;
    if (!ch) return 0;
    peer_slot = dart_peer_slot(st, peer);
    if (peer_slot < 0) return 0;
    r = dart__rp(st,channel_idx,peer_slot);
    if (!r->used || !r->assembly_active) return 0;            /* no message mid-reassembly */
    if (base_seqno) *base_seqno = r->deliver_upto;       /* HOL message starts here */
    if (total)      *total      = r->assembly_count;
    if (have){
        uint32_t i, c=0;
        for (i=0;i<r->assembly_count;i++) if (dart_bget(r->frag_bitmap,i)) c++;
        *have = c;
    }
    return 1;
}

#ifdef DART_SHM
/* 1 if the channel is non-multicast, has >=1 matched reader, and EVERY matched
 * (non-dormant) reader is SHM-capable -> the node may publish this message via SHM.
 * One non-SHM (remote) reader forces inline UDP for the whole message. */
int dart_writer_shm_eligible(dart_state *st, uint16_t channel){
    int channel_idx; dart_channel *ch = dart_chan(st, channel, &channel_idx);
    uint32_t max_peers, p; int any=0;
    if (!ch || ch->multicast) return 0;
    max_peers = st->cfg.max_peers;
    for (p=0;p<max_peers;p++){
        if (!dart__wp(st,channel_idx,p)->used || st->peer_dormant[p]) continue;
        if (!st->peer_shm[p]) return 0;
        any = 1;
    }
    return any;
}
/* the history slot the next publish will occupy (so the node binds a chunk to it) */
uint16_t dart_channel_hist_head(dart_state *st, uint16_t channel){
    int channel_idx; dart_channel *ch = dart_chan(st, channel, &channel_idx);
    return ch ? ch->history_head : 0;
}
#endif

/* wire alias for a local channel: its own index (the advertiser's handle). The
 * peer mapped this alias to its matching channel from our interest list. */
static uint16_t dart__alias_of(dart_state *st, int channel_idx){
    (void)st; return (uint16_t)channel_idx;
}

/* Submessage wire layout. Byte 0 = type|flags, bytes 1-2 = alias, then the body.
 * Builders (dart_mk_*) and the readers both index off these, so moving a field is one
 * edit, never a silent builder/parser drift. Several fields share an offset (distinct
 * names on purpose). Header sizes: DATA 13 (single)/21 (multi), HB 23, NACK 21. */
#define DART_O_ALIAS      1u  /* u16, every submessage */
#define DART_O_SEQNO      3u  /* DATA seqno; also HB first, NACK base, SHM base (u64) */
#define DART_O_PLEN1     11u  /* DATA single: u16 payload_len (payload at DART_HDR_DATA1) */
#define DART_HDR_DATA1   13u  /* DATA single: header bytes */
#define DART_O_FRAG      11u  /* DATA multi: u16 frag */
#define DART_O_COUNT     13u  /* DATA multi: u16 count */
#define DART_O_SLEN      15u  /* DATA multi: u32 sample_len */
#define DART_O_PLEN      19u  /* DATA multi: u16 payload_len (payload at DART_HDR_DATA) */
#define DART_HDR_DATA    21u  /* DATA multi: header bytes */
#define DART_O_HB_LAST   11u  /* HB: u64 last */
#define DART_O_HB_CNT    19u  /* HB: u32 count */
#define DART_HDR_HB      23u
#define DART_O_NK_NBITS  11u  /* NACK: u16 nbits */
#define DART_O_NK_BITS   13u  /* NACK: u32 bitmap */
#define DART_O_NK_EPOCH  17u  /* NACK: u32 epoch */
#define DART_HDR_NACK    21u
#define DART_O_SHM_COUNT 11u  /* SHM-DATA: u16 count */
#define DART_O_SHM_DESC  13u  /* SHM-DATA: descriptor (DART_SHM_DESC_BYTES follow) */

/* datagram builders (return length) */
static size_t dart_mk_data(uint8_t *o, uint16_t alias, uint64_t seqno, dart_writer_sample *s,
                         uint16_t frag, const uint8_t *payload, uint16_t payload_len){
    dart_le_w16(o+DART_O_ALIAS,alias);
    if (s->count==1){                       /* frag=0, count=1, len=payload_len implied */
        o[0]=(uint8_t)(DART_DATA|DART_F_SINGLE);
        dart_le_w64(o+DART_O_SEQNO,seqno); dart_le_w16(o+DART_O_PLEN1,payload_len);
        memcpy(o+DART_HDR_DATA1,payload,payload_len);
        return DART_HDR_DATA1+payload_len;
    }
    o[0]=DART_DATA;
    dart_le_w64(o+DART_O_SEQNO,seqno); dart_le_w16(o+DART_O_FRAG,frag); dart_le_w16(o+DART_O_COUNT,s->count);
    dart_le_w32(o+DART_O_SLEN,s->len); dart_le_w16(o+DART_O_PLEN,payload_len);
    memcpy(o+DART_HDR_DATA,payload,payload_len);
    return DART_HDR_DATA+payload_len;
}
#ifdef DART_SHM
/* SHM-DATA: one submessage covers [base, base+count); body is the descriptor, no
 * payload. 37 bytes = 1 (type|F_SHM) + 2 (alias) + 8 (base) + 2 (count) + 24 (desc). */
#define DART_SHM_DATA_BYTES (DART_O_SHM_DESC + DART_SHM_DESC_BYTES)
static size_t dart_mk_shm(uint8_t *o, uint16_t alias, uint64_t base, uint16_t count,
                          const uint8_t *desc){
    o[0]=(uint8_t)(DART_DATA|DART_F_SHM); dart_le_w16(o+DART_O_ALIAS,alias);
    dart_le_w64(o+DART_O_SEQNO,base); dart_le_w16(o+DART_O_SHM_COUNT,count);
    memcpy(o+DART_O_SHM_DESC,desc,DART_SHM_DESC_BYTES);
    return DART_SHM_DATA_BYTES;
}
#endif
static size_t dart_mk_hb(uint8_t *o, uint16_t alias, uint64_t first, uint64_t last, uint32_t cnt){
    o[0]=DART_HB; dart_le_w16(o+DART_O_ALIAS,alias); dart_le_w64(o+DART_O_SEQNO,first); dart_le_w64(o+DART_O_HB_LAST,last); dart_le_w32(o+DART_O_HB_CNT,cnt);
    return DART_HDR_HB;
}
static size_t dart_mk_nack(uint8_t *o, uint16_t alias, uint64_t base, uint16_t nbits, uint32_t bitmap,
                         uint32_t epoch, uint8_t flags){
    o[0]=(uint8_t)(DART_NACK|flags); dart_le_w16(o+DART_O_ALIAS,alias); dart_le_w64(o+DART_O_SEQNO,base);
    dart_le_w16(o+DART_O_NK_NBITS,nbits); dart_le_w32(o+DART_O_NK_BITS,bitmap); dart_le_w32(o+DART_O_NK_EPOCH,epoch);
    return DART_HDR_NACK;
}
/* emit an HB advertising this lane's current window. Doubles as the "skip past a
 * hole" signal that replaces GAP: reader_hb advances deliver_upto to `first`, so a
 * superseded NACK or a ring-overrun push answers with an HB whose first = our floor.
 * Resets the idle-HB timer so we don't double-send. */
static size_t dart_writer_hb(dart_state *st, dart_channel *ch, dart_writer_proxy *w, uint16_t alias,
                             uint8_t *out, size_t cap, uint64_t now){
    uint64_t first = ch->have_first ? ch->first_seqno : 0;
    if (cap < DART_HDR_HB) return 0;
    if (w->acked_upto > first) first = w->acked_upto;   /* fresh reader adopts join point */
    w->hb_next_us = now + ch->qos.heartbeat_us;
    dart__deadline(st, w->hb_next_us);                  /* wake to send the next idle HB */
    w->hb_count++;
    return dart_mk_hb(out, alias, first, ch->next_seqno-1, w->hb_count);
}

/* diagnostic: attribute each reader NACK-arm (a 0->1 transition of ack_pending) to
 * its cause -- a DATA/SHM-DATA arrival or a heartbeat. Pure counting, call it right
 * before any r->ack_pending=1 in the repair paths. arms_data tracks gap-triggered and
 * progress-refill arming; arms_hb tracks the writer's idle ping (which also drives the
 * tail-loss backstop). Read via dart_repair_stats. (The self-clocked retransmit backstop
 * re-fires via nack_retransmit_us without a fresh arm, so it is not counted here. Resume/
 * position-report arms are control, not counted.) */
static void dart__arm(dart_channel *ch, dart_reader_proxy *r, int is_hb){
    if (!r->ack_pending){ if (is_hb) ch->repair_stats.arms_hb++; else ch->repair_stats.arms_data++; }
}

#ifdef DART_SHM
/* reader side: handle an SHM-DATA submessage. It covers [base, base+count) in one
 * shot (payload is in shared memory), so there is no reassembly -- just ordering,
 * then hand the descriptor to on_shm (the node resolves + delivers + acks). The gap
 * case re-uses the normal NACK window (dart_reader_emit's !assembly_active branch). */
static void dart_reader_shm(dart_state *st, int channel_idx, int peer_slot, const uint8_t *p, uint64_t now){
    dart_channel *ch=&st->channels[channel_idx];
    dart_reader_proxy *r=dart__rp(st,channel_idx,peer_slot);
    int reliable = (ch->qos.reliability==DART_RELIABLE);
    uint64_t base = dart_le_r64(p+DART_O_SEQNO);
    uint16_t count = dart_le_r16(p+DART_O_SHM_COUNT);
    const uint8_t *desc = p+DART_O_SHM_DESC;          /* DART_SHM_DESC_BYTES */
    if (!r->used || count==0) return;
    if (base < r->deliver_upto) return;                 /* old/dup */
    if (base+count-1 > r->received_high) r->received_high = base+count-1;   /* proof this seqno exists */
    if (base > r->deliver_upto){
        if (reliable && r->started){                    /* gap: arm a NACK for the window */
            if (!r->ack_pending){ dart__arm(ch,r,0); r->ack_pending=1; r->ack_due_us=0; }
            dart__lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
            return;
        }
        if (r->started){                                /* best-effort / first contact: adopt */
            dart__event(st, DART_MSG_LOST, (uint16_t)channel_idx, st->peer_ids[peer_slot],
                        r->deliver_upto, base - r->deliver_upto, "message(s) lost");
            ch->repair_stats.msgs_skipped += base - r->deliver_upto;
        }
        r->deliver_upto = base;
    }
    r->started = 1; r->assembly_active = 0;
    /* in order (base == deliver_upto). Resolve the chunk; advance + ack ONLY if the
       node delivered. A failed resolve (recycled, or a transient unattachable segment)
       leaves the gap so the reliability layer repairs it (re-sent descriptor) or skips
       it (writer HB, sample evicted). A persistently unresolvable descriptor (mis-
       configured SHM constants) would loop, so after DART_SHM_MAX_RETRY tries we skip
       it loudly instead of wedging. */
    {   int ok = st->cfg.on_shm &&
                 st->cfg.on_shm(st->cfg.user, (uint16_t)channel_idx, st->peer_ids[peer_slot], desc);
        if (ok){
            r->shm_fail = 0;
            r->deliver_upto = base + count;
            if (reliable){                  /* ack now, AFTER delivery (zero-copy invariant) */
                dart__arm(ch,r,0); r->ack_pending=1; r->ack_due_us=0; r->ack_force=1;
                dart__lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
            }
            return;
        }
        if (reliable && ++r->shm_fail >= DART_SHM_MAX_RETRY){
            dart__event(st, DART_MSG_LOST, (uint16_t)channel_idx, st->peer_ids[peer_slot],
                        base, count, "SHM descriptor unresolvable (check DART_SHM_* build constants)");
            ch->repair_stats.msgs_skipped += count;
            r->shm_fail = 0;
            r->deliver_upto = base + count;             /* give up: skip past it, ack the new edge */
            dart__arm(ch,r,0); r->ack_pending=1; r->ack_due_us=0; r->ack_force=1;
        } else if (reliable){                           /* leave the gap, NACK for a re-send */
            dart__arm(ch,r,0); r->ack_pending=1; r->ack_due_us=0;
        } else {
            r->deliver_upto = base + count;             /* best-effort: no repair, drop it */
        }
        dart__lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
    }
}
#endif

/* reader side: handle DATA */
static void dart_reader_data(dart_state *st, int channel_idx, int peer_slot, const uint8_t *p,
                           uint64_t now){
    dart_channel *ch=&st->channels[channel_idx];
    dart_reader_proxy *r=dart__rp(st,channel_idx,peer_slot);
    int reliable = (ch->qos.reliability==DART_RELIABLE);
    int new_fragment = 0;
    uint64_t seqno, base; uint16_t frag, count, payload_len; uint32_t sample_len; const uint8_t *payload;
    if (p[0] & DART_F_SINGLE){           /* single fragment: frag/count/len implied */
        seqno=dart_le_r64(p+DART_O_SEQNO); frag=0; count=1; payload_len=dart_le_r16(p+DART_O_PLEN1); sample_len=payload_len; payload=p+DART_HDR_DATA1;
    } else {
        seqno=dart_le_r64(p+DART_O_SEQNO); frag=dart_le_r16(p+DART_O_FRAG); count=dart_le_r16(p+DART_O_COUNT);
        sample_len=dart_le_r32(p+DART_O_SLEN); payload_len=dart_le_r16(p+DART_O_PLEN); payload=p+DART_HDR_DATA;
    }
    base = seqno - frag;

    if (!r->used){ ch->repair_stats.frags_malformed++; return; }     /* not subscribed */
    if (count==0 || frag>=count){ ch->repair_stats.frags_malformed++; return; }  /* malformed */
    if (base < r->deliver_upto){ ch->repair_stats.frags_old++; return; }   /* old: already delivered/skipped */
    if (seqno > r->received_high) r->received_high = seqno;       /* proof this seqno exists (skip detector) */

    if (base > r->deliver_upto){
        ch->repair_stats.frags_ahead++;                          /* future message: we can't store it (no OOO buffer) */
        if (reliable && r->started){
            /* a future frag proves the head was skipped: arm a repair now (don't wait for a
               heartbeat). emit gates the actual request rate, so this can't flood. */
            if (!r->ack_pending){       /* keep the oldest due time so arrivals don't postpone it */
                dart__arm(ch,r,0); r->ack_pending=1; r->ack_due_us=0;
            }
            dart__lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
            return;
        }
        /* first contact or best-effort: adopt the writer's position */
        if (r->started){                                 /* best-effort loss */
            dart__event(st, DART_MSG_LOST, (uint16_t)channel_idx, st->peer_ids[peer_slot],
                        r->deliver_upto, base - r->deliver_upto, "message(s) lost");
            ch->repair_stats.msgs_skipped += base - r->deliver_upto;
        }
        r->deliver_upto = base; r->assembly_active=0;
    }
    r->started = 1;   /* writer engaged: position adopted */
    /* fit the reassembly buffers (dynamic grows via the hook, fixed is capped at
       max_message_bytes); "too big" skips the whole sample and reports it */
    { uint32_t bitmap_need = ((uint32_t)count + 7u) / 8u, buf_cap, bitmap_bytes; int too_big = 0;
      if (ch->dynamic){
          if (r->assembly_cap < sample_len){
              uint8_t *new_buf = (uint8_t*)st->cfg.allocator(st->cfg.user, r->assembly_buf, sample_len?sample_len:1u);
              if (!new_buf) too_big = 1; else { r->assembly_buf = new_buf; r->assembly_cap = sample_len?sample_len:1u; }
          }
          if (!too_big && r->bitmap_cap < bitmap_need){
              uint8_t *new_bitmap = (uint8_t*)st->cfg.allocator(st->cfg.user, r->frag_bitmap, bitmap_need?bitmap_need:1u);
              if (!new_bitmap) too_big = 1; else { r->frag_bitmap = new_bitmap; r->bitmap_cap = bitmap_need?bitmap_need:1u; }
          }
      } else if (sample_len > ch->qos.max_message_bytes) too_big = 1;
      if (too_big){
          dart__event(st, DART_MSG_TOO_BIG, (uint16_t)channel_idx, st->peer_ids[peer_slot],
                      0, sample_len, "message exceeds max_message_bytes");
          r->deliver_upto = base + count; r->assembly_active = 0;
          if (reliable){
              dart__arm(ch,r,0); r->ack_pending = 1; r->ack_due_us = 0; r->ack_force = 1;
              dart__lane_wake(st, (uint16_t)channel_idx, (uint32_t)peer_slot);
          }
          return;
      }
      buf_cap  = ch->dynamic ? r->assembly_cap : ch->qos.max_message_bytes;
      bitmap_bytes = ch->dynamic ? bitmap_need     : (uint32_t)((ch->max_fragments+7u)/8u);
      /* base == deliver_upto: current sample */
      if (!r->assembly_active){
          r->assembly_active=1; r->assembly_count=count; r->assembly_len=sample_len; r->assembly_low=0;
          r->nack_high=base;     /* in-flight dedup is per-message: start this one fresh */
          memset(r->frag_bitmap,0,bitmap_bytes);
      }
      if (count!=r->assembly_count) return;                  /* inconsistent, ignore */
      ch->repair_stats.frags_recv++;                             /* every accepted DATA fragment, dups included */
      new_fragment = !dart_bget(r->frag_bitmap,frag);
      if (new_fragment){
          /* reassemble at the SOURCE peer's fragment size (advertised via discovery);
             a peer staying within [MIN, MAX] keeps count <= max_fragments, so the bitmap
             can't overflow and the buf_cap guard catches any stray offset */
          uint32_t offset=(uint32_t)frag*st->peer_frag[peer_slot];
          if (offset+payload_len<=buf_cap) memcpy(r->assembly_buf+offset,payload,payload_len);
          dart_bset(r->frag_bitmap,frag);
          if (frag==r->assembly_low)                          /* extended the contiguous-received front */
              while (r->assembly_low<count && dart_bget(r->frag_bitmap,r->assembly_low)) r->assembly_low++;
      } else ch->repair_stats.frags_dup++;                        /* already held: repair overlap / waste */
    }
    /* assembly_low is the contiguous front, so the sample is complete iff it reached the end.
       Deliver in order, advance, then arm the ACKNACK. A completed sample owes an immediate
       cumulative ack (ack_force). A still-partial sample only re-arms when this frag opened or
       advanced a real gap (a hole below received_high): a healthy in-order fill owes nothing, and
       emit dedups + paces the repair request so we never re-flood the writer with in-flight
       fragments. */
    { int done = (r->assembly_low == count);
      int hole = r->assembly_active && (r->deliver_upto + r->assembly_low <= r->received_high);
      if (done){
          if (st->cfg.on_message)
              st->cfg.on_message(st->cfg.user, (uint16_t)channel_idx, st->peer_ids[peer_slot], r->assembly_buf, r->assembly_len);
          r->deliver_upto = base + count;
          r->assembly_active=0;
      }
      if (reliable){
          if (done){
              dart__arm(ch,r,0); r->ack_pending=1; r->ack_due_us=0; r->ack_force=1;
              dart__lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
          } else if (new_fragment && hole){           /* gap revealed, or repair advanced: request now */
              dart__arm(ch,r,0); r->ack_pending=1; r->ack_due_us=0;
              dart__lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
          }
      }
    }
}

static void dart_reader_hb(dart_state *st, int channel_idx, int peer_slot, const uint8_t *p, uint64_t now){
    dart_channel *ch=&st->channels[channel_idx];
    dart_reader_proxy *r=dart__rp(st,channel_idx,peer_slot);
    uint64_t first=dart_le_r64(p+DART_O_SEQNO), last=dart_le_r64(p+DART_O_HB_LAST);
    if (!r->used) return;
    if (ch->qos.reliability!=DART_RELIABLE) return;
    /* un-started readers adopt no position from heartbeats (a one-sided flap's
       advertised first may be a dead predecessor's); the ack below carries our epoch.
       A writer raises its HB `first` to our acked_upto (the join-point trick), and our
       ACKNACK acks the contiguous-received front -- which sits INSIDE the sample we are
       still assembling. Eviction is whole-message, so a genuine floor never splits a
       sample: ignore a `first` that lands in our current partial (it is just our own
       mid-message ack echoed back), else we would skip past frags we are repairing and
       reject every resend as old. */
    if (r->started && first > r->deliver_upto &&
        (!r->assembly_active || first >= r->deliver_upto + r->assembly_count)){
        dart__event(st, DART_MSG_LOST, (uint16_t)channel_idx, st->peer_ids[peer_slot],   /* superseded before repair */
                    r->deliver_upto, first - r->deliver_upto, "message(s) lost");
        ch->repair_stats.msgs_skipped += first - r->deliver_upto;
        r->deliver_upto=first; r->assembly_active=0;
#ifdef DART_SHM
        r->shm_fail=0;          /* skipped past the stuck descriptor: fresh count */
#endif
    }
    /* hb_last is the writer's CLAIM (it may exceed what we've received). It is the only
       way to learn of tail loss -- frags past received_high that no later arrival will reveal --
       so emit lets the slow retransmit backstop chase up to it, never the fast gap path.
       The HB always owes a cumulative ack so a writer that lost ours stops re-pinging. */
    r->hb_last=last;
    dart__arm(ch,r,1); r->ack_pending=1; r->ack_due_us=0; r->ack_force=1;
    dart__lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
}

/* writer side: handle ACKNACK */
static void dart_writer_nack(dart_state *st, int channel_idx, int peer_slot, const uint8_t *p){
    dart_channel *ch=&st->channels[channel_idx];
    dart_writer_proxy *w=dart__wp(st,channel_idx,peer_slot);
    uint64_t base=dart_le_r64(p+DART_O_SEQNO); uint16_t nbits=dart_le_r16(p+DART_O_NK_NBITS); uint32_t bitmap=dart_le_r32(p+DART_O_NK_BITS);
    uint32_t epoch=dart_le_r32(p+DART_O_NK_EPOCH); uint8_t flags=p[0];
    int group_mode;
    if (!w->used) return;
    group_mode = ch->multicast && ch->n_subscribers>0;
    if (w->reader_epoch != epoch){
        if (w->reader_epoch){
            /* reader is a new incarnation (one-sided flap): our positions describe its
               dead predecessor, so re-join the lane as if freshly matched */
            w->sent_upto  = group_mode ? ch->multicast_sent_upto : dart_unicast_join_seqno(ch);
            w->acked_upto = w->sent_upto;
            w->has_nack   = 0;
            w->hb_next_us = 0;
            dart__lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
            w->reader_epoch = epoch;
            return;
        }
        w->reader_epoch = epoch;      /* first contact: lane is already fresh */
    }
    if (flags & DART_F_UNPOS){
        /* reader has delivered nothing and never NACKs: re-push from the unacked
           edge (the join window) so a push that raced ahead isn't lost */
        if (!group_mode && w->acked_upto < w->sent_upto){
            w->sent_upto = w->acked_upto;
            dart__lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
        }
        return;                    /* no position information to apply */
    }
    if (base > w->acked_upto) w->acked_upto=base;
    if (nbits>0 && bitmap!=0){
        ch->repair_stats.nacks_recv++;                           /* a repair request, not a bare ack */
        w->has_nack=1; w->nack_base=base; w->nack_bits=bitmap;
        dart__lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
    }
}

void dart_on_datagram(dart_state *st, uint32_t from, const void *datagram, size_t len, uint64_t now){
    const uint8_t *p=(const uint8_t*)datagram; size_t rem=len;
    int peer_slot=dart_peer_slot(st,from);
    if (peer_slot<0) return;
    /* concatenated submessages; each length comes from its header, so no framing */
    while (rem>=3){
        uint8_t b0=p[0], type=(uint8_t)(b0 & DART_MSG_MASK); uint16_t alias; size_t sub; int channel_idx;
        switch(type){
            case DART_DATA:
#ifdef DART_SHM
                            if (b0 & DART_F_SHM){ if (rem<DART_SHM_DATA_BYTES) return; sub=DART_SHM_DATA_BYTES; }
                            else
#endif
                            if (b0 & DART_F_SINGLE){ if (rem<DART_HDR_DATA1) return; sub=DART_HDR_DATA1+(size_t)dart_le_r16(p+DART_O_PLEN1); }
                            else { if (rem<DART_HDR_DATA) return; sub=DART_HDR_DATA+(size_t)dart_le_r16(p+DART_O_PLEN); } break;
            case DART_HB:   if (rem<DART_HDR_HB) return; sub=DART_HDR_HB; break;
            case DART_NACK: if (rem<DART_HDR_NACK) return; sub=DART_HDR_NACK; break;
            default: return;             /* unknown type: cannot resync, drop rest */
        }
        if (sub>rem) return;             /* truncated/malformed */
        alias = dart_le_r16(p+DART_O_ALIAS);
        if ((uint32_t)alias < st->alias_max){
            uint16_t m=st->alias_to_channel[(size_t)peer_slot*st->alias_max+alias]; channel_idx=(m==0xFFFFu)?-1:(int)m;
        } else channel_idx=-1;
        if (channel_idx>=0){
            switch(type){
                case DART_DATA:
#ifdef DART_SHM
                                if (p[0] & DART_F_SHM){ dart_reader_shm(st,channel_idx,peer_slot,p,now); break; }
#endif
                                dart_reader_data(st,channel_idx,peer_slot,p,now); break;
                case DART_HB:   dart_reader_hb  (st,channel_idx,peer_slot,p,now); break;
                case DART_NACK: dart_writer_nack(st,channel_idx,peer_slot,p); break;
            }
        }
        p+=sub; rem-=sub;
    }
}

/* produce one writer submessage for (channel_idx,peer_slot) if due and it fits cap; 0 if none.
 * On no-fit, state is untouched so the same submessage is produced next time. */
static size_t dart_writer_emit(dart_state *st, int channel_idx, int peer_slot, uint8_t *out, size_t cap, uint64_t now){
    dart_channel *ch=&st->channels[channel_idx];
    dart_writer_proxy *w=dart__wp(st,channel_idx,peer_slot);
    int reliable=(ch->qos.reliability==DART_RELIABLE);
    /* group mode active only while a multicast channel has remote subscribers: new
       data + HBs ride the group lane, this per-peer lane only answers NACKs */
    int group_mode = ch->multicast && ch->n_subscribers>0;
    uint16_t alias = dart__alias_of(st, channel_idx);
    if (!w->used || st->peer_dormant[peer_slot]) return 0;   /* dormant: out of flow control */
    if (group_mode && (!reliable || !w->has_nack)) return 0;

    /* 1. repair (reliable only) */
    if (reliable && w->has_nack){
        uint32_t i;
        for (i=0;i<DART_NACK_WINDOW;i++){
            if (w->nack_bits & (1u<<i)){
                uint64_t seqno=w->nack_base+i;
                dart_writer_sample *s;
                if (seqno>=ch->next_seqno){                 /* nothing there */
                    w->nack_bits &= ~(1u<<i);
                    if (w->nack_bits==0) w->has_nack=0;
                    continue;
                }
                s=dart_find_sample(ch,seqno);
                if (s){
#ifdef DART_SHM
                    if (st->peer_shm[peer_slot] && s->shm){   /* re-send the whole message as one SHM-DATA */
                        uint32_t j;
                        if (cap < DART_SHM_DATA_BYTES) return 0;
                        for (j=0;j<DART_NACK_WINDOW;j++){
                            uint64_t sq=w->nack_base+j;
                            if (sq>=s->base && sq<s->base+s->count) w->nack_bits &= ~(1u<<j);
                        }
                        if (w->nack_bits==0) w->has_nack=0;
                        return dart_mk_shm(out,alias,s->base,s->count,s->desc);
                    }
#endif
                    {
                    uint16_t frag_idx=(uint16_t)(seqno - s->base);
                    uint32_t offset=(uint32_t)frag_idx*st->frag;
                    uint16_t payload_len=(uint16_t)((s->len-offset)<st->frag?(s->len-offset):st->frag);
                    if (cap < (size_t)(s->count==1?DART_HDR_DATA1:DART_HDR_DATA)+(size_t)payload_len) return 0;   /* bit stays set */
                    w->nack_bits &= ~(1u<<i);
                    if (w->nack_bits==0) w->has_nack=0;
                    ch->repair_stats.frags_sent++; ch->repair_stats.frags_resent++;   /* retransmit to satisfy a NACK */
                    return dart_mk_data(out,alias,seqno,s,frag_idx,dart__sbuf(s)+offset,payload_len);
                    }
                } else {
                    /* superseded: skip the reader past the dropped region with an HB
                       (its first = our floor); keep still-cached seqnos for later repair */
                    uint64_t floor = (ch->have_first?ch->first_seqno:ch->next_seqno);
                    uint32_t j;
                    if (cap < DART_HDR_HB) return 0;
                    for (j=0;j<DART_NACK_WINDOW;j++)
                        if (w->nack_base+j < floor) w->nack_bits &= ~(1u<<j);
                    if (w->nack_bits==0) w->has_nack=0;
                    return dart_writer_hb(st,ch,w,alias,out,cap,now);
                }
            }
        }
        w->has_nack=0;
    }
    if (group_mode) return 0;   /* group lane owns everything below */

    /* 2. push new data */
    if (w->sent_upto < ch->next_seqno){
        uint64_t seqno=w->sent_upto;
        dart_writer_sample *s=dart_find_sample(ch,seqno);
        if (s){
#ifdef DART_SHM
            /* peer_shm is set at attach (before data flows), so sent_upto sits at a
               sample boundary here: emit the whole message as one SHM-DATA */
            if (st->peer_shm[peer_slot] && s->shm){
                if (cap < DART_SHM_DATA_BYTES) return 0;
                w->sent_upto = s->base + s->count;
                return dart_mk_shm(out,alias,s->base,s->count,s->desc);
            }
#endif
            {
            uint16_t frag_idx=(uint16_t)(seqno - s->base);
            uint32_t offset=(uint32_t)frag_idx*st->frag;
            uint16_t payload_len=(uint16_t)((s->len-offset)<st->frag?(s->len-offset):st->frag);
            if (cap < (size_t)(s->count==1?DART_HDR_DATA1:DART_HDR_DATA)+(size_t)payload_len) return 0;
            w->sent_upto++;
            ch->repair_stats.frags_sent++;                       /* new data (unicast lane) */
            return dart_mk_data(out,alias,seqno,s,frag_idx,dart__sbuf(s)+offset,payload_len);
            }
        } else {
            /* fell out of the ring before we sent it: skip the reader up to first
               cached with an HB (its first = our floor) */
            if (cap < DART_HDR_HB) return 0;
            w->sent_upto=(ch->have_first?ch->first_seqno:ch->next_seqno);
            return dart_writer_hb(st,ch,w,alias,out,cap,now);
        }
    }

    /* 3. heartbeat (reliable, timer due, and this reader is behind). Once it has
       acked everything (acked_upto == next_seqno) there's nothing to repair, so the
       lane goes silent until new data or a (re)subscribe drops acked_upto again. The
       HB advertises from acked_upto so a fresh reader adopts the join point. */
    if (reliable && now>=w->hb_next_us && w->acked_upto < ch->next_seqno)
        return dart_writer_hb(st,ch,w,alias,out,cap,now);
    return 0;
}

/* produce a reader ACKNACK for (channel_idx,peer_slot) if due; 0 if none.
 *
 * The repair request is GAP-TRIGGERED and bounded by what we have actually RECEIVED.
 * UDP frags are assumed delivered in order, so a hole below received_high is real loss while
 * anything above it is still in flight and must NOT be NACKed -- that "request up to the
 * writer's heartbeat CLAIM, every poll" was the old congestion collapse. Each floor is
 * asked once: nack_high tracks how far we have already requested, so a progress refill asks
 * only (nack_high, top] and never re-requests the still-outstanding lower frags. A stalled
 * floor is re-asked only after the retransmit backstop (repair_delay), which is also the
 * one path allowed to chase the writer's claim (hb_last) so tail loss still repairs.
 * Outstanding repair is therefore capped at one DART_NACK_WINDOW and clocked to delivery,
 * so it cannot scale into a flood with the gap size or the message size. */
static size_t dart_reader_emit(dart_state *st, int channel_idx, int peer_slot, uint8_t *out, size_t cap, uint64_t now){
    dart_channel *ch=&st->channels[channel_idx];
    dart_reader_proxy *r=dart__rp(st,channel_idx,peer_slot);
    uint64_t first_missing, bound, top; uint16_t nbits=0; uint32_t bitmap=0;
    uint16_t alias = dart__alias_of(st, channel_idx);
    int due, holes=0, repair=0, force;
    if (!r->used || st->peer_dormant[peer_slot]) return 0;   /* dormant: don't ack a silent writer */
    if (ch->qos.reliability!=DART_RELIABLE) return 0;
    if (cap<DART_HDR_NACK) return 0;
    if (!r->ack_pending || now<r->ack_due_us) return 0;
    r->ack_pending=0; force=r->ack_force; r->ack_force=0;

    if (!r->started)                                     /* no position yet: F_UNPOS announces our epoch */
        return dart_mk_nack(out,alias,r->deliver_upto,0,0,r->epoch,(uint8_t)DART_F_UNPOS);

    /* the cumulative-ack point and repair-window base: our contiguous-received front */
    first_missing = r->assembly_active ? r->deliver_upto + r->assembly_low : r->deliver_upto;

    /* request ceiling = what we've received. Only the slow backstop may reach the writer's
       claim, so tail loss (no later frag will ever reveal it) still gets repaired. */
    due   = (now >= r->nack_retransmit_us);
    bound = r->received_high;
    if (due && r->hb_last > bound) bound = r->hb_last;

    if (first_missing <= bound){                         /* a hole sits below something we've heard */
        holes = 1;
        top = first_missing + DART_NACK_WINDOW;          /* one window per ACKNACK: the in-flight cap */
        if (top > bound + 1) top = bound + 1;
        if (r->assembly_active && top > r->deliver_upto + r->assembly_count)
            top = r->deliver_upto + r->assembly_count;        /* this sample's frags only (bitmap range) */
        {   /* in-flight dedup (skip the still-outstanding lower part) is only valid while we
               are assembling THIS message: its clear frag_bitmap bits are genuinely in flight. With
               no sample yet (whole message missing) there is nothing we can hold, and the slow
               backstop re-asks everything, so both ask from the floor. */
            uint64_t from = (due || !r->assembly_active) ? first_missing
                : (r->nack_high > first_missing ? r->nack_high : first_missing); /* refill: only the new part */
            uint64_t s;
            for (s=from; s<top; s++){
                int missing = r->assembly_active ? !dart_bget(r->frag_bitmap,(uint32_t)(s - r->deliver_upto)) : 1;
                if (missing) bitmap |= (1u << (uint32_t)(s - first_missing));
            }
            if (bitmap){
                nbits = (uint16_t)(top - first_missing);
                repair = 1;
                ch->repair_stats.nacks_sent++;
                if (top > r->nack_high) r->nack_high = top;
                r->nack_retransmit_us = now + ch->qos.repair_delay_us;
            }
        }
    } else r->nack_high = first_missing;                   /* caught up to received: end the episode */

    /* keep the lane live while a hole remains so the backstop re-fires; an arrival that
       advances the floor re-arms us immediately (ack_due_us=0) for the next window. */
    if (holes){ r->ack_pending=1; r->ack_due_us=r->nack_retransmit_us; dart__deadline(st,r->ack_due_us); }

    /* send only to carry a repair request or a delivery/skip/HB/(re)match cumulative ack;
       a bare re-ack at an unchanged floor would be pure noise. */
    if (!repair && !force) return 0;
    return dart_mk_nack(out,alias,first_missing,nbits,bitmap,r->epoch,0);
}

/* multicast: 1 if every matched subscriber has acked all data, so the group
 * heartbeat can stop until new data arrives or a new/lagging subscriber needs it */
static int dart__group_all_acked(dart_state *st, int channel_idx){
    uint32_t max_peers=st->cfg.max_peers, p; uint64_t seq=st->channels[channel_idx].next_seqno;
    for (p=0;p<max_peers;p++){
        dart_writer_proxy *w=dart__wp(st,channel_idx,p);
        if (w->used && !st->peer_dormant[p] && w->acked_upto < seq) return 0;
    }
    return 1;
}

/* multicast writer lane: new data once for the whole group, then a channel-level
 * heartbeat (reliable). Same per-call contract as dart_writer_emit. */
static size_t dart_group_emit(dart_state *st, int channel_idx, uint8_t *out, size_t cap, uint64_t now){
    dart_channel *ch=&st->channels[channel_idx];
    uint16_t alias = dart__alias_of(st, channel_idx);
    if (!ch->multicast || ch->role==DART_SUB_ONLY || ch->role==DART_INACTIVE) return 0;
    if (ch->n_subscribers==0){
        /* no remote subscribers: pin the cursor forward so a future join gets no stale replay */
        ch->multicast_sent_upto = ch->next_seqno;
        return 0;
    }
    if (ch->multicast_sent_upto < ch->next_seqno){
        uint64_t seqno=ch->multicast_sent_upto;
        dart_writer_sample *s=dart_find_sample(ch,seqno);
        if (s){
            uint16_t frag_idx=(uint16_t)(seqno - s->base);
            uint32_t offset=(uint32_t)frag_idx*st->frag;
            uint16_t payload_len=(uint16_t)((s->len-offset)<st->frag?(s->len-offset):st->frag);
            if (cap < (size_t)(s->count==1?DART_HDR_DATA1:DART_HDR_DATA)+(size_t)payload_len) return 0;
            ch->multicast_sent_upto++;
            ch->repair_stats.frags_sent++;                       /* new data, once for the whole group */
            return dart_mk_data(out,alias,seqno,s,frag_idx,s->buf+offset,payload_len);
        } else {
            /* overran the ring: skip the group past it with an HB (first = our floor) */
            if (cap < DART_HDR_HB) return 0;
            ch->multicast_sent_upto=(ch->have_first?ch->first_seqno:ch->next_seqno);
            ch->multicast_hb_next_us = now + ch->qos.heartbeat_us;
            dart__deadline(st, ch->multicast_hb_next_us);
            ch->multicast_hb_count++;
            return dart_mk_hb(out,alias,(ch->have_first?ch->first_seqno:0),ch->next_seqno-1,ch->multicast_hb_count);
        }
    }
    if (ch->qos.reliability==DART_RELIABLE && now>=ch->multicast_hb_next_us && ch->next_seqno>0
        && !dart__group_all_acked(st, channel_idx)){
        if (cap < DART_HDR_HB) return 0;
        ch->multicast_hb_next_us = now + ch->qos.heartbeat_us;
        dart__deadline(st, ch->multicast_hb_next_us);
        ch->multicast_hb_count++;
        return dart_mk_hb(out,alias,(ch->have_first?ch->first_seqno:0),ch->next_seqno-1,ch->multicast_hb_count);
    }
    return 0;
}

/* sendable work a popped lane still owes now (timer-armed work is the sweep's job) */
static int dart__lane_work(dart_state *st, uint16_t channel_idx, uint32_t peer_slot, uint64_t now){
    dart_channel *ch=&st->channels[channel_idx];
    uint32_t max_peers=st->cfg.max_peers;
    if (peer_slot==max_peers)
        return ch->multicast && ch->n_subscribers>0 && ch->multicast_sent_upto < ch->next_seqno;
    if (!st->peer_used[peer_slot] || st->peer_dormant[peer_slot]) return 0;   /* dormant: out of flow control */
    { dart_writer_proxy *w=dart__wp(st,channel_idx,peer_slot);
      dart_reader_proxy *r=dart__rp(st,channel_idx,peer_slot);
      int group_mode = ch->multicast && ch->n_subscribers>0;
      if (w->used && w->has_nack) return 1;
      if (w->used && !group_mode && w->sent_upto < ch->next_seqno) return 1;
      if (r->used && ch->qos.reliability==DART_RELIABLE
          && r->ack_pending && now >= r->ack_due_us) return 1;
    }
    return 0;
}

/* clock-driven counterpart of the wake calls: a cursor walks the lane table waking
 * lanes whose timers came due. Two triggers: the amortized backstop (full coverage
 * every DART_HB_SWEEP_US) and a forced full pass when next_deadline_us comes due, so
 * a deadline-capped poll that wakes for a timer actually services it. A full pass
 * also recomputes next_deadline_us exactly (the global min of not-yet-due timers).
 * Read-only; cost is bounded by table size. */
static void dart__hb_sweep(dart_state *st, uint64_t now){
    uint32_t max_peers=st->cfg.max_peers, lanes=max_peers+1u;
    uint32_t total=(uint32_t)st->cfg.n_channels*lanes, due, k;
    uint64_t span = now - st->sweep_time_us;
    int forced = (now >= st->next_deadline_us);    /* a tracked timer is due */
    int full;
    uint64_t mind = DART__NO_DEADLINE;             /* earliest not-yet-due timer seen */
    due = (forced || span >= DART_HB_SWEEP_US) ? total
        : (uint32_t)(span * total / DART_HB_SWEEP_US);
    if (!due) return;              /* sweep_time_us advances only when lanes are paid */
    full = (due >= total);         /* covered every lane -> mind is the global minimum */
    st->sweep_time_us = now;
    for (k=0;k<due;k++){
        uint32_t lane=st->sweep, peer_slot=lane%lanes;
        uint16_t channel_idx=(uint16_t)(lane/lanes);
        dart_channel *ch=&st->channels[channel_idx];
        st->sweep = (st->sweep+1u>=total) ? 0u : st->sweep+1u;
        /* gate writer/multicast heartbeats on next_seqno, never the reader ack: a
           sub-only node's data channels never advance next_seqno but still owe acks */
        if (ch->qos.reliability!=DART_RELIABLE) continue;
        if (peer_slot==max_peers){
            if (ch->next_seqno && ch->multicast && ch->n_subscribers>0 && !dart__group_all_acked(st,(int)channel_idx)){
                if (now>=ch->multicast_hb_next_us) dart__lane_enq(st,channel_idx,peer_slot);
                else if (ch->multicast_hb_next_us < mind) mind = ch->multicast_hb_next_us;
            }
            continue;
        }
        if (!st->peer_used[peer_slot] || st->peer_dormant[peer_slot]) continue;   /* dormant: out of flow control */
        { dart_writer_proxy *w=dart__wp(st,channel_idx,peer_slot);
          dart_reader_proxy *r=dart__rp(st,channel_idx,peer_slot);
          int group_mode = ch->multicast && ch->n_subscribers>0;
          if (w->used && !group_mode && w->acked_upto < ch->next_seqno){
              if (now>=w->hb_next_us) dart__lane_enq(st,channel_idx,peer_slot);
              else if (w->hb_next_us < mind) mind = w->hb_next_us;
          }
          if (r->used && r->ack_pending){
              if (now>=r->ack_due_us) dart__lane_enq(st,channel_idx,peer_slot);
              else if (r->ack_due_us < mind) mind = r->ack_due_us;
          }
        }
    }
    /* a full pass saw every timer: mind is the exact next deadline. Lanes woken above
       re-arm during emit (dart__deadline) and re-lower it; reader acks just clear. */
    if (full) st->next_deadline_us = mind;
}

int dart_poll_send(dart_state *st, uint32_t *to_peer, void *out, size_t cap, size_t *out_len, uint64_t now){
    uint32_t max_peers=st->cfg.max_peers, lanes=max_peers+1u;
    uint32_t ndest = max_peers+(uint32_t)st->cfg.n_channels;
    dart__hb_sweep(st, now);
    while (st->dest_queue_count){
        uint32_t d; size_t offset=0;
        d = st->dest_queue[st->dest_queue_head];
        st->dest_queue_head = (st->dest_queue_head+1u>=ndest) ? 0u : st->dest_queue_head+1u;
        st->dest_queue_count--; st->dest_queued[d]=0;
        /* drain this destination's lanes into one datagram */
        while (st->dest_head[d]!=DART__NIL){
            uint32_t lane=st->dest_head[d], peer_slot=lane%lanes;
            uint16_t channel_idx=(uint16_t)(lane/lanes);
            size_t n;
            do {
                if (peer_slot==max_peers) n=dart_group_emit(st,channel_idx,(uint8_t*)out+offset,cap-offset,now);
                else {
                    /* acks first: small, one-shot, and carry the NACKs that drive
                       repair, so a backlogged writer can't starve them */
                    n=dart_reader_emit(st,(int)channel_idx,(int)peer_slot,(uint8_t*)out+offset,cap-offset,now);
                    if (!n) n=dart_writer_emit(st,(int)channel_idx,(int)peer_slot,(uint8_t*)out+offset,cap-offset,now);
                }
                offset+=n;
            } while (n && offset<cap);
            st->dest_head[d]=st->lane_next[lane];
            if (dart__lane_work(st,channel_idx,peer_slot,now)){
                /* datagram full mid-lane: rotate the lane to the back so siblings get the next */
                if (st->dest_head[d]==DART__NIL) st->dest_head[d]=lane;
                else {
                    st->lane_next[lane]=DART__NIL;
                    st->lane_next[st->dest_tail[d]]=lane;
                    st->dest_tail[d]=lane;
                }
                break;
            }
            st->lane_queued[lane]=0;     /* lane drained */
        }
        if (st->dest_head[d]!=DART__NIL) dart__dest_push(st,d);  /* fair: re-queue at tail */
        if (offset){
            *to_peer = (d<max_peers) ? st->peer_ids[d]
                              : DART_DEST_GROUP(st->channels[d-max_peers].identity & 0xFFu);
            *out_len = offset;
            return 1;
        }
        if (st->dest_head[d]!=DART__NIL)
            return 0;    /* work pending but nothing fit: caller's cap too small */
    }
    return 0;
}

uint64_t dart_next_deadline_us(dart_state *st){
    return st->next_deadline_us == DART__NO_DEADLINE ? 0 : st->next_deadline_us;
}
