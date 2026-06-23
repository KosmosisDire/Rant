/* sans-IO reliable-UDP transport core. See dart_transport.h. */
#include "dart_transport.h"
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

/* little-endian pack helpers */
static void dart_w16(uint8_t*p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void dart_w32(uint8_t*p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}
static void dart_w64(uint8_t*p,uint64_t v){int i;for(i=0;i<8;i++)p[i]=(uint8_t)(v>>(8*i));}
static uint16_t dart_r16(const uint8_t*p){return (uint16_t)(p[0]|((uint16_t)p[1]<<8));}
static uint32_t dart_r32(const uint8_t*p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t dart_r64(const uint8_t*p){uint64_t v=0;int i;for(i=0;i<8;i++)v|=((uint64_t)p[i])<<(8*i);return v;}

static void dart_bset(uint8_t*bm,uint32_t i){bm[i>>3]|=(uint8_t)(1u<<(i&7));}
static int  dart_bget(const uint8_t*bm,uint32_t i){return (bm[i>>3]>>(i&7))&1;}

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
} dart_wsample;

#ifdef DART_SHM
/* where a sample's bytes live: the external chunk for SHM samples, else our buf */
static const uint8_t *dart__sbuf(const dart_wsample *s){ return s->shm ? s->shm_buf : s->buf; }
#else
static const uint8_t *dart__sbuf(const dart_wsample *s){ return s->buf; }
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
} dart_wproxy;

typedef struct {        /* reader-side, per (channel,peer) */
    uint8_t  used;
    uint8_t  started;       /* accepted any DATA from this writer yet */
    uint32_t epoch;         /* this incarnation's id, sent in every ACKNACK */
    uint64_t deliver_upto;  /* base of current sample; all below delivered/skipped */
    uint8_t  asm_active;    /* received >=1 frag of current sample */
    uint16_t asm_count;
    uint32_t asm_len;
    uint8_t *asm_buf;       /* >= asm_len; arena (fixed) or hook-malloc'd (dynamic) */
    uint8_t *frag_bm;       /* ceil(asm_count/8) */
    uint32_t asm_cap;       /* allocated bytes of asm_buf (dynamic grows it) */
    uint32_t bm_cap;        /* allocated bytes of frag_bm */
    uint64_t hb_last;       /* highest seqno writer claims to hold */
    uint8_t  ack_pending;
    uint64_t ack_due_us;
#ifdef DART_SHM
    uint8_t  shm_fail;      /* consecutive SHM-DATA resolve failures at deliver_upto */
#endif
} dart_rproxy;

#ifdef DART_SHM
#ifndef DART_SHM_MAX_RETRY
#define DART_SHM_MAX_RETRY 8u   /* give up on an unresolvable descriptor after this many */
#endif
#endif

typedef struct {
    dart_qos    qos;
    uint64_t  identity;     /* cross-peer topic identity (hash of name) */
    const char *name;       /* our copy of the topic name */
    uint16_t  maxfrags;     /* ceil(max_message_bytes/FRAG) (fixed mode only) */
    uint8_t   role;         /* dart_role */
    uint8_t   multicast;
    uint8_t   dynamic;      /* 1 = buffers grow via cfg.allocator, no fixed cap */
    uint16_t  nsubs;        /* live matched subscribers; multicast: >0 = group mode */
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
    uint8_t     *peer_pub_bm; /* [max_peers][bmlen] peer publishes channel c */
    uint8_t     *peer_sub_bm; /* [max_peers][bmlen] peer subscribes channel c */
    uint16_t     bmlen;       /* ceil(n_channels / 8) */
    /* per-peer wire alias -> our channel index; the data path carries the 2-byte
       alias instead of the topic name */
    uint16_t    *alias_ci;    /* [max_peers * amax]; 0xFFFF = unmapped */
    uint32_t     amax;        /* alias-table stride = effective meta_max_ids */
    dart_channel  *chans;     /* [n_channels] */
    dart_wproxy   *wprox;     /* [n_channels*max_peers] */
    dart_rproxy   *rprox;     /* [n_channels*max_peers] */
    /* active-lane scheduler: a lane is one (channel,peer) pair or a channel's group
       lane. The event that gives a lane work enqueues it, so poll_send pays for work
       done, not idle lanes. Timer work is found by an amortized clock-driven sweep. */
    uint32_t    *lane_next;   /* [n_channels*(max_peers+1)] next in dest list */
    uint8_t     *lane_inq;    /* [n_channels*(max_peers+1)] queued flag */
    uint32_t    *dest_head;   /* [max_peers+n_channels] lane list per dest */
    uint32_t    *dest_tail;
    uint8_t     *dest_inq;
    uint32_t    *destq;       /* ring of active destinations */
    uint32_t     destq_head, destq_n;
    uint32_t     sweep;       /* timer-sweep lane cursor */
    uint64_t     sweep_t;     /* clock position the sweep has paid for */
    uint64_t     next_deadline_us; /* earliest armed timer (ack/HB); caps the poll wait.
                                      Lower bound fed at arm sites, made exact by a full sweep.
                                      DART__NO_DEADLINE = nothing deferred (the hot path). */
    uint32_t     repoch;      /* reader-epoch counter (starts at 1; 0 = none) */
};

/* a deferred timer was (re)armed for absolute time t: keep next_deadline as the
   minimum so the poll wakes when it is due. t==0 is an immediate ack (woken via the
   active-lane queue, not a timer), so it is ignored here. */
static void dart__deadline(dart_state *st, uint64_t t){
    if (t && t < st->next_deadline_us) st->next_deadline_us = t;
}

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

/* Reader-side fragment-count bound: a peer may fragment at the smallest size in
 * the deployment, so size the reassembly bitmap by DART_FRAG_PAYLOAD_MIN. */
static uint16_t dart_maxfrags(uint32_t max_message_bytes){
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
    uint16_t c, p; uint32_t np = cfg->max_peers, nc = cfg->n_channels;
    uint16_t bml = (uint16_t)((nc+7u)/8u);
    uint32_t mids = DART_META_MAX_IDS;
    uint32_t name_bytes = 0; char *npool = NULL; uint32_t ncur = 0;
    dart_state *st = (dart_state*)dart_take(b, sizeof(dart_state), 16);
    if (st && b->base) memset(st, 0, sizeof(*st));

    /* name pool (our copies of the topic names) */
    for (c=0;c<nc;c++){
        size_t L = dart__namelen(cfg->channels[c].name);
        if (L) name_bytes += (uint32_t)L + 1u;
    }
    if (mids < 2u*nc) mids = 2u*nc;     /* our own interest list must always fit */

    { uint32_t nlanes = nc*(np+1u), ndest = np+nc;
      uint32_t *pi = (uint32_t*)dart_take(b, np*sizeof(uint32_t), 8);
      uint8_t  *pu = (uint8_t*) dart_take(b, np*sizeof(uint8_t), 1);
      uint8_t  *pl = (uint8_t*) dart_take(b, np*sizeof(uint8_t), 1);
      uint8_t  *pdm= (uint8_t*) dart_take(b, np*sizeof(uint8_t), 1);
      uint16_t *pf = (uint16_t*)dart_take(b, np*sizeof(uint16_t), 2);
#ifdef DART_SHM
      uint8_t  *psh= (uint8_t*) dart_take(b, np*sizeof(uint8_t), 1);
#endif
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
          st->peer_dormant=pdm; st->peer_frag=pf;
          st->frag = cfg->frag_payload ? cfg->frag_payload : DART_FRAG_PAYLOAD;
          if (st->frag < DART_FRAG_PAYLOAD_MIN) st->frag = DART_FRAG_PAYLOAD_MIN;
          if (st->frag > DART_FRAG_PAYLOAD_MAX) st->frag = DART_FRAG_PAYLOAD_MAX;
          st->peer_pub_bm=pb; st->peer_sub_bm=sb; st->bmlen=bml;
          st->chans=ch; st->wprox=wp; st->rprox=rp; st->repoch=1;
          st->next_deadline_us=DART__NO_DEADLINE;
          st->lane_next=ln; st->lane_inq=li;
          st->dest_head=dh; st->dest_tail=dt; st->dest_inq=di; st->destq=dq;
          st->alias_ci=ac; st->amax=mids;
          memset(pu,0,np); memset(pl,0,np); memset(pdm,0,np);
          { uint32_t k; for (k=0;k<np;k++) pf[k]=DART_FRAG_PAYLOAD; }  /* set per peer on add */
#ifdef DART_SHM
          st->peer_shm=psh; memset(psh,0,np);
#endif
          memset(ac,0xFF,(size_t)np*mids*sizeof(uint16_t));   /* all unmapped */
          memset(pb,0,(size_t)np*bml); memset(sb,0,(size_t)np*bml);
          memset(wp,0,(size_t)nc*np*sizeof(dart_wproxy));
          memset(rp,0,(size_t)nc*np*sizeof(dart_rproxy));
          memset(li,0,nlanes); memset(di,0,ndest);
          memset(dh,0xFF,(size_t)ndest*sizeof(uint32_t));   /* all DART__NIL */
      }
    }

    for (c=0;c<nc;c++){
        const dart_channel_def *def = &cfg->channels[c];
        /* dynamic = an allocator is set: buffers grow via the hook, not the arena */
        int dyn = (cfg->allocator != NULL);
        dart_qos q = def->qos;            /* local, normalized copy */
        dart_wsample *hist; uint16_t depth, mf, d;
        dart__qos_defaults(&q, dyn);
        depth = q.keep_last;
        mf = dart_maxfrags(q.max_message_bytes);
        hist = (dart_wsample*)dart_take(b, depth*sizeof(dart_wsample), 16);
        if (st && b->base){
            dart_channel *ch = &st->chans[c];
            size_t L = dart__namelen(def->name);
            memset(ch,0,sizeof(*ch));
            ch->qos=q; ch->maxfrags=mf;
            ch->role=def->role; ch->multicast=def->multicast; ch->dynamic=(uint8_t)dyn;
            ch->identity = dart_channel_identity(def);
            ch->name = NULL;
            if (L){ char *dst = npool + ncur;
                    memcpy(dst, def->name, L); dst[L]='\0';
                    ch->name = dst; ncur += (uint32_t)(L + 1u); }
            ch->hist=hist; ch->hist_head=0; ch->next_seqno=0; ch->have_first=0;
            memset(hist,0,depth*sizeof(dart_wsample));
        }
        for (d=0; d<depth; d++){
            uint8_t *buf = dyn ? NULL : (uint8_t*)dart_take(b, q.max_message_bytes, 8);
            if (st && b->base){ st->chans[c].hist[d].buf = buf;
                                st->chans[c].hist[d].cap = dyn ? 0u : q.max_message_bytes; }
        }
        /* reader asm buffers + frag bitmaps, per peer (skipped when dynamic) */
        for (p=0;p<np;p++){
            uint8_t *abuf = dyn ? NULL : (uint8_t*)dart_take(b, q.max_message_bytes, 8);
            uint8_t *fbm  = dyn ? NULL : (uint8_t*)dart_take(b, (mf+7u)/8u, 1);
            if (st && b->base){
                dart_rproxy *r = &st->rprox[(size_t)c*np+p];
                r->asm_buf=abuf; r->frag_bm=fbm;
                r->asm_cap = dyn ? 0u : q.max_message_bytes;
                r->bm_cap  = dyn ? 0u : (uint32_t)((mf+7u)/8u);
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

dart_state *dart_init(void *mem, size_t cap, const dart_config *cfg){
    dart_bump b; dart_state *st; uint16_t i;
    if (!mem || !cfg || cfg->n_channels==0 || cfg->max_peers==0) return NULL;
    for (i=0;i<cfg->n_channels;i++){
        const dart_channel_def *d = &cfg->channels[i];
        size_t L = 0;
        if (!d->name || !d->name[0]) return NULL;          /* name = identity, required */
        while (d->name[L]) L++;
        if (L > DART_TOPIC_NAME_MAX) return NULL;           /* the wire name is the whole name */
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
    return &st->chans[channel];
}
/* RX demux: find the local channel for a wire identity */
static dart_channel *dart_chan_by_identity(dart_state *st, uint64_t identity, int *idx_out){
    uint16_t i;
    for (i=0;i<st->cfg.n_channels;i++) if (st->chans[i].identity==identity){ if(idx_out)*idx_out=(int)i; return &st->chans[i]; }
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

/* scheduler: lane index = ci*(max_peers+1)+ps (ps==max_peers = group lane);
 * destination = peer slot ps, or max_peers+ci for a group lane */
static void dart__dest_push(dart_state *st, uint32_t d){
    uint32_t ndest = st->cfg.max_peers + (uint32_t)st->cfg.n_channels, t;
    if (st->dest_inq[d]) return;
    st->dest_inq[d]=1;
    t = st->destq_head + st->destq_n;
    if (t >= ndest) t -= ndest;
    st->destq[t]=d; st->destq_n++;
}

/* enqueue a lane that just got sendable work; idempotent while queued */
static void dart__lane_enq(dart_state *st, uint16_t ci, uint32_t ps){
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

/* enqueue, and track a freshly-armed reader ack/NACK deadline for the poll cap. Used
 * by the arm sites (ack_due_us is future or 0); the sweep enqueues due lanes with
 * dart__lane_enq instead, since it recomputes next_deadline itself. */
static void dart__lane_wake(dart_state *st, uint16_t ci, uint32_t ps){
    if (ps < st->cfg.max_peers){
        dart_rproxy *r=&st->rprox[(size_t)ci*st->cfg.max_peers+ps];
        if (r->used && r->ack_pending) dart__deadline(st, r->ack_due_us);
    }
    dart__lane_enq(st, ci, ps);
}

/* unicast join seqno: head minus qos.catch_up cached samples (reliable only) */
static uint64_t dart_unicast_join_seqno(const dart_channel *chn){
    uint16_t depth = chn->qos.keep_last;   /* normalized at init (>=1) */
    uint16_t want = chn->qos.catch_up, k, i;
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

/* match one (channel,peer) proxy: a multicast channel engages its group lane on
 * the first subscriber; per-peer lanes then carry repairs only */
static void dart__match_w(dart_state *st, uint16_t c, uint16_t ps){
    dart_channel *chn=&st->chans[c];
    dart_wproxy *w=&st->wprox[(size_t)c*st->cfg.max_peers+ps];
    memset(w,0,sizeof(*w));
    w->used=1;
    if (!chn->multicast){
        w->sent_upto = dart_unicast_join_seqno(chn);
    } else {
        /* first subscriber engages group mode at the head; reliable-from-join-point,
           later joiners backfill via NACK */
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
    if (chn->multicast && chn->nsubs) chn->nsubs--;
    w->used=0;
}
static void dart__match_r(dart_state *st, uint16_t c, uint16_t ps){
    dart_rproxy *r=&st->rprox[(size_t)c*st->cfg.max_peers+ps];
    uint8_t *abuf=r->asm_buf, *fbm=r->frag_bm;
    uint32_t acap=r->asm_cap, bcap=r->bm_cap;   /* keep grown buffers across rematch */
    memset(r,0,sizeof(*r));
    r->asm_buf=abuf; r->frag_bm=fbm; r->asm_cap=acap; r->bm_cap=bcap;
    r->epoch=st->repoch++;   /* new incarnation: writers re-join on seeing it */
    r->used=1;       /* started==0: first DATA adopts the writer's position */
    /* announce this incarnation once so a caught-up (idle, non-pinging) writer
       re-joins and replays. A genuine discovery blip keeps its position through
       dart_peer_dormant/resume and never lands here, so a single ACKNACK suffices. */
    if (st->chans[c].qos.reliability==DART_RELIABLE){
        r->ack_pending=1; r->ack_due_us=0;
        dart__lane_wake(st,c,ps);
    }
}
static void dart__unmatch_r(dart_state *st, uint16_t c, uint16_t ps){
    dart_rproxy *r=&st->rprox[(size_t)c*st->cfg.max_peers+ps];
    r->used=0; r->asm_active=0;
}

/* recompute one (channel,peer) match from our role and the peer's interest bits */
static void dart__rematch(dart_state *st, uint16_t c, uint16_t ps){
    dart_channel *chn=&st->chans[c];
    const uint8_t *pb=&st->peer_pub_bm[(size_t)ps*st->bmlen];
    const uint8_t *sb=&st->peer_sub_bm[(size_t)ps*st->bmlen];
    int wuse = (chn->role==DART_PUBSUB || chn->role==DART_PUB_ONLY) && dart_bget(sb,c);
    int ruse = (chn->role==DART_PUBSUB || chn->role==DART_SUB_ONLY) && dart_bget(pb,c);
    dart_wproxy *w=&st->wprox[(size_t)c*st->cfg.max_peers+ps];
    dart_rproxy *r=&st->rprox[(size_t)c*st->cfg.max_peers+ps];
    if (wuse && !w->used) dart__match_w(st,c,ps);
    else if (!wuse && w->used) dart__unmatch_w(st,c,ps);
    if (ruse && !r->used) dart__match_r(st,c,ps);
    else if (!ruse && r->used) dart__unmatch_r(st,c,ps);
}

static uint16_t dart__clamp_frag(uint16_t f){
    if (f==0) f = DART_FRAG_PAYLOAD;
    if (f < DART_FRAG_PAYLOAD_MIN) f = DART_FRAG_PAYLOAD_MIN;
    if (f > DART_FRAG_PAYLOAD_MAX) f = DART_FRAG_PAYLOAD_MAX;
    return f;
}

void dart_peer_add(dart_state *st, uint32_t id, int peer_is_local, uint16_t peer_frag){
    uint16_t i; int free=-1; uint32_t np=st->cfg.max_peers;
    if (dart_peer_slot(st,id)>=0) return;
    for (i=0;i<np;i++) if(!st->peer_used[i]){free=(int)i;break;}
    if (free<0) return;
    st->peer_used[free]=1; st->peer_ids[free]=id;
    st->peer_local[free]=(uint8_t)(peer_is_local?1:0);
    st->peer_dormant[free]=0;
    st->peer_frag[free]=dart__clamp_frag(peer_frag);
#ifdef DART_SHM
    st->peer_shm[free]=0;   /* node sets it once the peer's segment is attached */
#endif
    memset(&st->peer_pub_bm[(size_t)free*st->bmlen],0,st->bmlen);
    memset(&st->peer_sub_bm[(size_t)free*st->bmlen],0,st->bmlen);
    memset(&st->alias_ci[(size_t)free*st->amax],0xFF,(size_t)st->amax*sizeof(uint16_t));
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
    int s = dart_peer_slot(st,id); uint16_t c; uint32_t np;
    if (s<0) return;
    st->peer_dormant[s]=0;
    np = st->cfg.max_peers;
    for (c=0;c<st->cfg.n_channels;c++){
        dart_wproxy *w=&st->wprox[(size_t)c*np+s];
        dart_rproxy *r=&st->rprox[(size_t)c*np+s];
        if (st->chans[c].qos.reliability!=DART_RELIABLE) continue;
        if (r->used){ r->ack_pending=1; r->ack_due_us=0; }  /* report our position now */
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
static dart_wsample *dart_find_sample(dart_channel *ch, uint64_t seqno){
    uint16_t depth = ch->qos.keep_last, k;
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

/* append the filled head slot to history and wake the lanes that carry it */
static void dart__commit(dart_state *st, uint16_t ci, size_t len){
    dart_channel *ch = &st->chans[ci];
    uint16_t depth = ch->qos.keep_last;
    uint16_t count = (uint16_t)((len + st->frag - 1) / st->frag);
    dart_wsample *slot = &ch->hist[ch->hist_head];
    if (count==0) count=1;
    slot->valid=1; slot->base=ch->next_seqno; slot->count=count; slot->len=(uint32_t)len;
    ch->hist_head = (uint16_t)((ch->hist_head+1) % depth);
    ch->next_seqno += count;
    /* oldest cached: where the head points once wrapped, else slot 0 */
    ch->first_seqno = ch->hist[ch->hist_head].valid ? ch->hist[ch->hist_head].base
                                                    : ch->hist[0].base;
    ch->have_first  = 1;
    if (ch->multicast && ch->nsubs>0)
        dart__lane_wake(st, ci, st->cfg.max_peers);    /* group lane */
    else {
        uint32_t np=st->cfg.max_peers, p;
        for (p=0;p<np;p++)
            if (st->wprox[(size_t)ci*np+p].used && !st->peer_dormant[p]) dart__lane_wake(st, ci, p);
    }
}

int dart_send(dart_state *st, uint16_t channel, const void *data, size_t len, uint64_t now){
    int ci; dart_channel *ch;
    (void)now;
    ch = dart_chan(st, channel, &ci);                /* rejects the internal meta channel */
    if (!ch) return -1;
    if (ch->dynamic){
        dart_wsample *slot = &ch->hist[ch->hist_head];
        size_t need = len ? len : 1u;
        if (len > 65535u*(uint32_t)st->frag) return -2;   /* wire fragment-count cap */
        if ((size_t)slot->cap < need){                    /* grow the slot to fit */
            uint8_t *nb = (uint8_t*)st->cfg.allocator(st->cfg.user, slot->buf, need);
            if (!nb) return -4;                           /* out of memory */
            slot->buf = nb; slot->cap = (uint32_t)need;
        }
    } else if (len > ch->qos.max_message_bytes) return -2;
    if (ch->role == DART_SUB_ONLY || ch->role == DART_INACTIVE) return -3;
    if (len) memcpy(ch->hist[ch->hist_head].buf, data, len);
#ifdef DART_SHM
    ch->hist[ch->hist_head].shm = 0;   /* an inline send: this slot is not SHM-backed */
#endif
    dart__commit(st, (uint16_t)ci, len);
    return 0;
}

#ifdef DART_SHM
/* publish a sample whose bytes live in an external (shared-memory) chunk: store the
 * chunk pointer + descriptor on the history slot without copying. Remote peers
 * fragment from the chunk; SHM peers get the one-submessage descriptor. */
int dart_send_shm(dart_state *st, uint16_t channel, const void *chunk, size_t len,
                  const uint8_t *desc, uint64_t now){
    int ci; dart_channel *ch; dart_wsample *slot;
    (void)now;
    ch = dart_chan(st, channel, &ci);
    if (!ch) return -1;
    if (len > 65535u*(uint32_t)st->frag) return -2;      /* wire fragment-count cap */
    if (ch->role == DART_SUB_ONLY || ch->role == DART_INACTIVE) return -3;
    slot = &ch->hist[ch->hist_head];
    slot->shm = 1;
    slot->shm_buf = (const uint8_t*)chunk;
    memcpy(slot->desc, desc, DART_SHM_DESC_BYTES);
    dart__commit(st, (uint16_t)ci, len);
    return 0;
}
#endif

void dart_destroy(dart_state *st){
    uint16_t c; uint32_t p, np;
    if (!st || !st->cfg.allocator) return;     /* fixed mode: nothing hook-allocated */
    np = st->cfg.max_peers;
    for (c=0;c<st->cfg.n_channels;c++){
        dart_channel *ch=&st->chans[c];
        uint16_t depth, d;
        if (!ch->dynamic) continue;
        depth = ch->qos.keep_last;
        for (d=0; d<depth; d++)
            if (ch->hist[d].buf){ st->cfg.allocator(st->cfg.user, ch->hist[d].buf, 0);
                                  ch->hist[d].buf=NULL; ch->hist[d].cap=0; }
        for (p=0;p<np;p++){
            dart_rproxy *r=&st->rprox[(size_t)c*np+p];
            if (r->asm_buf){ st->cfg.allocator(st->cfg.user, r->asm_buf, 0); r->asm_buf=NULL; r->asm_cap=0; }
            if (r->frag_bm){ st->cfg.allocator(st->cfg.user, r->frag_bm, 0); r->frag_bm=NULL; r->bm_cap=0; }
        }
    }
}

/* one interest entry: [u16 alias][u8 namelen][name]. The name rides along so a
 * hash collision is detected (not cross-wired); the identity is recomputed from it. */
static uint8_t *dart__meta_put(uint8_t *p, uint16_t alias, const dart_channel *ch){
    size_t L = dart__namelen(ch->name);
    dart_w16(p, alias); p += 2;
    *p++ = (uint8_t)L;
    if (L){ memcpy(p, ch->name, L); p += L; }
    return p;
}
static int dart__meta_name_eq(const dart_channel *ch, const uint8_t *name, size_t nlen){
    size_t ours = dart__namelen(ch->name);
    if (nlen != ours) return 0;
    return nlen==0 ? 1 : (memcmp(ch->name, name, nlen)==0);
}
/* match count entries to local channels by identity (recomputed from each name),
 * recording the alias map. Same-identity-different-name is a collision: refused. */
static const uint8_t *dart__meta_scan(dart_state *st, int ps, const uint8_t *p,
                                      uint32_t count, uint8_t *bm){
    uint32_t k;
    for (k=0;k<count;k++){
        uint16_t alias=dart_r16(p); uint32_t nlen=p[2]; const uint8_t *name=p+3; int ci;
        uint64_t id=dart__id_n(name,nlen);
        dart_channel *ch=dart_chan_by_identity(st,id,&ci);
        p = name + nlen;
        if (!ch) continue;                                  /* not ours */
        if (dart__meta_name_eq(ch,name,nlen)){
            dart_bset(bm,(uint32_t)ci);
            if ((uint32_t)alias < st->amax)
                st->alias_ci[(size_t)ps*st->amax + alias] = (uint16_t)ci;
        }
        else
            dart__event(st, DART_NAME_COLLISION, (uint16_t)ci, st->peer_ids[ps],
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
    uint16_t c; uint32_t np=0, ns=0;
    if (cap < 4) return 0;
    p=o+4;
    for (c=0;c<st->cfg.n_channels;c++){
        uint8_t d=st->chans[c].role;
        if (d==DART_PUBSUB || d==DART_PUB_ONLY){
            if (p + 3u + dart__namelen(st->chans[c].name) > end) return 0;
            p=dart__meta_put(p,c,&st->chans[c]); np++;
        }
    }
    for (c=0;c<st->cfg.n_channels;c++){
        uint8_t d=st->chans[c].role;
        if (d==DART_PUBSUB || d==DART_SUB_ONLY){
            if (p + 3u + dart__namelen(st->chans[c].name) > end) return 0;
            p=dart__meta_put(p,c,&st->chans[c]); ns++;
        }
    }
    dart_w16(o,(uint16_t)np); dart_w16(o+2,(uint16_t)ns);
    return (size_t)(p - o);
}

/* A peer's interest list arrived (from its discovery announce): refresh its bits
 * and rematch every channel. Idempotent; re-applying re-derives all matches. */
void dart_apply_peer_interest(dart_state *st, uint32_t peer_id, const void *blob, size_t len){
    const uint8_t *d=(const uint8_t*)blob, *p, *end=d+len;
    uint16_t np, ns, c; int ps=dart_peer_slot(st,peer_id);
    uint8_t *pb, *sb;
    if (ps<0 || len<4) return;
    pb=&st->peer_pub_bm[(size_t)ps*st->bmlen];
    sb=&st->peer_sub_bm[(size_t)ps*st->bmlen];
    np=dart_r16(d); ns=dart_r16(d+2);
    /* validate the whole variable-length list first: a truncated blob must not drop a match */
    { uint32_t k, tot=(uint32_t)np+ns; p=d+4;
      for (k=0;k<tot;k++){
          if (p+3 > end) return;
          p += 3u + (uint32_t)p[2];
          if (p > end) return;
      } }
    memset(pb,0,st->bmlen); memset(sb,0,st->bmlen);
    memset(&st->alias_ci[(size_t)ps*st->amax],0xFF,(size_t)st->amax*sizeof(uint16_t));
    p = dart__meta_scan(st, ps, d+4, np, pb);
    p = dart__meta_scan(st, ps, p,   ns, sb);
    for (c=0;c<st->cfg.n_channels;c++) dart__rematch(st,c,(uint16_t)ps);
}

int dart_set_role(dart_state *st, uint16_t channel, uint8_t role){
    int ci; dart_channel *ch; uint16_t p;
    if (role > DART_INACTIVE) return -1;
    ch = dart_chan(st, channel, &ci);
    if (!ch) return -1;
    if (ch->role == role) return 0;
    ch->role = role;
    for (p=0;p<st->cfg.max_peers;p++)
        if (st->peer_used[p]) dart__rematch(st,(uint16_t)ci,p);
    /* caller re-advertises interest (the node bumps its discovery announce) */
    return 0;
}

const dart_qos *dart_channel_qos(dart_state *st, uint16_t channel){
    dart_channel *ch = dart_chan(st, channel, NULL);
    return ch ? &ch->qos : NULL;
}

int dart_send_would_evict(dart_state *st, uint16_t channel){
    int ci; dart_channel *ch = dart_chan(st, channel, &ci);
    dart_wsample *slot; uint32_t np; uint16_t p;
    if (!ch || ch->qos.reliability != DART_RELIABLE) return 0;
    slot = &ch->hist[ch->hist_head];        /* slot the next send overwrites */
    if (!slot->valid) return 0;
    np = st->cfg.max_peers;
    for (p=0;p<(uint16_t)np;p++){
        dart_wproxy *w=&st->wprox[(size_t)ci*np+p];
        if (w->used && !st->peer_dormant[p] && w->acked_upto < slot->base + slot->count) return 1;
    }
    return 0;
}

int dart_send_drained(dart_state *st, uint16_t channel){
    int ci; dart_channel *ch = dart_chan(st, channel, &ci);
    uint32_t np; uint16_t p;
    if (!ch || ch->qos.reliability != DART_RELIABLE) return 1;  /* no acks to await */
    np = st->cfg.max_peers;
    for (p=0;p<(uint16_t)np;p++){
        dart_wproxy *w=&st->wprox[(size_t)ci*np+p];
        if (w->used && !st->peer_dormant[p] && w->acked_upto < ch->next_seqno) return 0;  /* reader still behind */
    }
    return 1;
}

int dart_writer_match_count(dart_state *st, uint16_t channel){
    int ci; dart_channel *ch = dart_chan(st, channel, &ci);
    uint32_t np, p; int cnt = 0;
    if (!ch) return 0;
    np = st->cfg.max_peers;
    for (p=0;p<np;p++) if (st->wprox[(size_t)ci*np+p].used) cnt++;  /* matched readers */
    return cnt;
}

#ifdef DART_SHM
/* 1 if the channel is non-multicast, has >=1 matched reader, and EVERY matched
 * (non-dormant) reader is SHM-capable -> the node may publish this message via SHM.
 * One non-SHM (remote) reader forces inline UDP for the whole message. */
int dart_writer_shm_eligible(dart_state *st, uint16_t channel){
    int ci; dart_channel *ch = dart_chan(st, channel, &ci);
    uint32_t np, p; int any=0;
    if (!ch || ch->multicast) return 0;
    np = st->cfg.max_peers;
    for (p=0;p<np;p++){
        if (!st->wprox[(size_t)ci*np+p].used || st->peer_dormant[p]) continue;
        if (!st->peer_shm[p]) return 0;
        any = 1;
    }
    return any;
}
/* the history slot the next publish will occupy (so the node binds a chunk to it) */
uint16_t dart_channel_hist_head(dart_state *st, uint16_t channel){
    int ci; dart_channel *ch = dart_chan(st, channel, &ci);
    return ch ? ch->hist_head : 0;
}
#endif

/* wire alias for a local channel: its own index (the advertiser's handle). The
 * peer mapped this alias to its matching channel from our interest list. */
static uint16_t dart__alias_of(dart_state *st, int ci){
    (void)st; return (uint16_t)ci;
}

/* datagram builders (return length). Byte 0 = type | flags; alias (u16) at o+1.
 * Header sizes: DATA 13 (single) or 21 (multi), HB 23, NACK 21. */
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
#ifdef DART_SHM
/* SHM-DATA: one submessage covers [base, base+count); body is the descriptor, no
 * payload. 37 bytes = 1 (type|F_SHM) + 2 (alias) + 8 (base) + 2 (count) + 24 (desc). */
#define DART_SHM_DATA_BYTES (13u + DART_SHM_DESC_BYTES)
static size_t dart_mk_shm(uint8_t *o, uint16_t alias, uint64_t base, uint16_t count,
                          const uint8_t *desc){
    o[0]=(uint8_t)(DART_DATA|DART_F_SHM); dart_w16(o+1,alias);
    dart_w64(o+3,base); dart_w16(o+11,count);
    memcpy(o+13,desc,DART_SHM_DESC_BYTES);
    return DART_SHM_DATA_BYTES;
}
#endif
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
/* emit an HB advertising this lane's current window. Doubles as the "skip past a
 * hole" signal that replaces GAP: reader_hb advances deliver_upto to `first`, so a
 * superseded NACK or a ring-overrun push answers with an HB whose first = our floor.
 * Resets the idle-HB timer so we don't double-send. */
static size_t dart_writer_hb(dart_state *st, dart_channel *ch, dart_wproxy *w, uint16_t alias,
                             uint8_t *out, size_t cap, uint64_t now){
    uint64_t first = ch->have_first ? ch->first_seqno : 0;
    if (cap < 23) return 0;
    if (w->acked_upto > first) first = w->acked_upto;   /* fresh reader adopts join point */
    w->hb_next_us = now + ch->qos.heartbeat_us;
    dart__deadline(st, w->hb_next_us);                  /* wake to send the next idle HB */
    w->hb_count++;
    return dart_mk_hb(out, alias, first, ch->next_seqno-1, w->hb_count);
}

#ifdef DART_SHM
/* reader side: handle an SHM-DATA submessage. It covers [base, base+count) in one
 * shot (payload is in shared memory), so there is no reassembly -- just ordering,
 * then hand the descriptor to on_shm (the node resolves + delivers + acks). The gap
 * case re-uses the normal NACK window (dart_reader_emit's !asm_active branch). */
static void dart_reader_shm(dart_state *st, int ci, int pslot, const uint8_t *p, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    dart_rproxy *r=&st->rprox[(size_t)ci*st->cfg.max_peers+pslot];
    int reliable = (ch->qos.reliability==DART_RELIABLE);
    uint64_t base = dart_r64(p+3);
    uint16_t count = dart_r16(p+11);
    const uint8_t *desc = p+13;          /* DART_SHM_DESC_BYTES */
    if (!r->used || count==0) return;
    if (base < r->deliver_upto) return;                 /* old/dup */
    if (base > r->deliver_upto){
        if (reliable && r->started){                    /* gap: arm a NACK for the window */
            if (base+count-1 > r->hb_last) r->hb_last = base+count-1;
            if (!r->ack_pending){ r->ack_pending=1; r->ack_due_us=now + ch->qos.repair_delay_us; }
            dart__lane_wake(st,(uint16_t)ci,(uint32_t)pslot);
            return;
        }
        if (r->started)                                 /* best-effort / first contact: adopt */
            dart__event(st, DART_MSG_LOST, (uint16_t)ci, st->peer_ids[pslot],
                        r->deliver_upto, base - r->deliver_upto, "message(s) lost");
        r->deliver_upto = base;
    }
    r->started = 1; r->asm_active = 0;
    /* in order (base == deliver_upto). Resolve the chunk; advance + ack ONLY if the
       node delivered. A failed resolve (recycled, or a transient unattachable segment)
       leaves the gap so the reliability layer repairs it (re-sent descriptor) or skips
       it (writer HB, sample evicted). A persistently unresolvable descriptor (mis-
       configured SHM constants) would loop, so after DART_SHM_MAX_RETRY tries we skip
       it loudly instead of wedging. */
    {   int ok = st->cfg.on_shm &&
                 st->cfg.on_shm(st->cfg.user, (uint16_t)ci, st->peer_ids[pslot], desc);
        if (ok){
            r->shm_fail = 0;
            r->deliver_upto = base + count;
            if (reliable){                  /* ack now, AFTER delivery (zero-copy invariant) */
                r->ack_pending=1; r->ack_due_us=0;      /* in-order: no gap to coalesce */
                dart__lane_wake(st,(uint16_t)ci,(uint32_t)pslot);
            }
            return;
        }
        if (reliable && ++r->shm_fail >= DART_SHM_MAX_RETRY){
            dart__event(st, DART_MSG_LOST, (uint16_t)ci, st->peer_ids[pslot],
                        base, count, "SHM descriptor unresolvable (check DART_SHM_* build constants)");
            r->shm_fail = 0;
            r->deliver_upto = base + count;             /* give up: skip past it, ack the new edge */
            r->ack_pending=1; r->ack_due_us=now + ch->qos.repair_delay_us;
        } else if (reliable){                           /* leave the gap, NACK for a re-send */
            if (base+count-1 > r->hb_last) r->hb_last = base+count-1;
            r->ack_pending=1; r->ack_due_us=now + ch->qos.repair_delay_us;
        } else {
            r->deliver_upto = base + count;             /* best-effort: no repair, drop it */
        }
        dart__lane_wake(st,(uint16_t)ci,(uint32_t)pslot);
    }
}
#endif

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
    if (count==0 || frag>=count) return;                /* malformed */
    if (base < r->deliver_upto) return;                 /* old/dup */

    if (base > r->deliver_upto){
        if (reliable && r->started){
            /* out-of-order: arm the NACK immediately, don't wait for a heartbeat
               (a busy writer defers HBs and the ring may wrap before one arrives) */
            if (seqno > r->hb_last) r->hb_last = seqno;
            if (!r->ack_pending){       /* keep the oldest due time so arrivals don't postpone it */
                r->ack_pending=1; r->ack_due_us=now + ch->qos.repair_delay_us;
            }
            dart__lane_wake(st,(uint16_t)ci,(uint32_t)pslot);
            return;
        }
        /* first contact or best-effort: adopt the writer's position */
        if (r->started)                                  /* best-effort loss */
            dart__event(st, DART_MSG_LOST, (uint16_t)ci, st->peer_ids[pslot],
                        r->deliver_upto, base - r->deliver_upto, "message(s) lost");
        r->deliver_upto = base; r->asm_active=0;
    }
    r->started = 1;   /* writer engaged: position adopted */
    /* fit the reassembly buffers (dynamic grows via the hook, fixed is capped at
       max_message_bytes); "too big" skips the whole sample and reports it */
    { uint32_t bmneed = ((uint32_t)count + 7u) / 8u, bufcap, bmbytes; int toobig = 0;
      if (ch->dynamic){
          if (r->asm_cap < slen){
              uint8_t *nb = (uint8_t*)st->cfg.allocator(st->cfg.user, r->asm_buf, slen?slen:1u);
              if (!nb) toobig = 1; else { r->asm_buf = nb; r->asm_cap = slen?slen:1u; }
          }
          if (!toobig && r->bm_cap < bmneed){
              uint8_t *nbm = (uint8_t*)st->cfg.allocator(st->cfg.user, r->frag_bm, bmneed?bmneed:1u);
              if (!nbm) toobig = 1; else { r->frag_bm = nbm; r->bm_cap = bmneed?bmneed:1u; }
          }
      } else if (slen > ch->qos.max_message_bytes) toobig = 1;
      if (toobig){
          dart__event(st, DART_MSG_TOO_BIG, (uint16_t)ci, st->peer_ids[pslot],
                      0, slen, "message exceeds max_message_bytes");
          r->deliver_upto = base + count; r->asm_active = 0;
          if (reliable){
              r->ack_pending = 1; r->ack_due_us = now + ch->qos.repair_delay_us;
              dart__lane_wake(st, (uint16_t)ci, (uint32_t)pslot);
          }
          return;
      }
      bufcap  = ch->dynamic ? r->asm_cap : ch->qos.max_message_bytes;
      bmbytes = ch->dynamic ? bmneed     : (uint32_t)((ch->maxfrags+7u)/8u);
      /* base == deliver_upto: current sample */
      if (!r->asm_active){
          r->asm_active=1; r->asm_count=count; r->asm_len=slen;
          memset(r->frag_bm,0,bmbytes);
      }
      if (count!=r->asm_count) return;                  /* inconsistent, ignore */
      if (!dart_bget(r->frag_bm,frag)){
          /* reassemble at the SOURCE peer's fragment size (advertised via discovery);
             a peer staying within [MIN, MAX] keeps count <= maxfrags, so the bitmap
             can't overflow and the bufcap guard catches any stray offset */
          uint32_t off=(uint32_t)frag*st->peer_frag[pslot];
          if (off+plen<=bufcap) memcpy(r->asm_buf+off,pay,plen);
          dart_bset(r->frag_bm,frag);
      }
    }
    /* complete? in order -> deliver, advance, ack NOW: an in-order cumulative ack
       has no gap to coalesce, so repair_delay would only stall the writer's window.
       A partial sample keeps the deferred timer; ack stays armed after delivery. */
    { uint16_t i; int done=1;
      for (i=0;i<count;i++) if(!dart_bget(r->frag_bm,i)){done=0;break;}
      if (done){
          if (st->cfg.on_message)
              st->cfg.on_message(st->cfg.user, (uint16_t)ci, st->peer_ids[pslot], r->asm_buf, r->asm_len);
          r->deliver_upto = base + count;
          r->asm_active=0;
      }
      if (reliable){
          r->ack_pending=1;
          r->ack_due_us = done ? 0 : now + ch->qos.repair_delay_us;
          dart__lane_wake(st,(uint16_t)ci,(uint32_t)pslot);
      }
    }
}

static void dart_reader_hb(dart_state *st, int ci, int pslot, const uint8_t *p, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    dart_rproxy *r=&st->rprox[(size_t)ci*st->cfg.max_peers+pslot];
    uint64_t first=dart_r64(p+3), last=dart_r64(p+11);
    if (!r->used) return;
    if (ch->qos.reliability!=DART_RELIABLE) return;
    /* un-started readers adopt no position from heartbeats (a one-sided flap's
       advertised first may be a dead predecessor's); the ack below carries our epoch */
    if (r->started && first > r->deliver_upto){
        dart__event(st, DART_MSG_LOST, (uint16_t)ci, st->peer_ids[pslot],   /* superseded before repair */
                    r->deliver_upto, first - r->deliver_upto, "message(s) lost");
        r->deliver_upto=first; r->asm_active=0;
#ifdef DART_SHM
        r->shm_fail=0;          /* skipped past the stuck descriptor: fresh count */
#endif
    }
    r->hb_last=last;
    r->ack_pending=1; r->ack_due_us = now + ch->qos.repair_delay_us;
    dart__lane_wake(st,(uint16_t)ci,(uint32_t)pslot);
}

/* writer side: handle ACKNACK */
static void dart_writer_nack(dart_state *st, int ci, int pslot, const uint8_t *p){
    dart_channel *ch=&st->chans[ci];
    dart_wproxy *w=&st->wprox[(size_t)ci*st->cfg.max_peers+pslot];
    uint64_t base=dart_r64(p+3); uint16_t nbits=dart_r16(p+11); uint32_t bm=dart_r32(p+13);
    uint32_t ep=dart_r32(p+17); uint8_t fl=p[0];
    int group_mode;
    if (!w->used) return;
    group_mode = ch->multicast && ch->nsubs>0;
    if (w->reader_epoch != ep){
        if (w->reader_epoch){
            /* reader is a new incarnation (one-sided flap): our positions describe its
               dead predecessor, so re-join the lane as if freshly matched */
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
        /* reader has delivered nothing and never NACKs: re-push from the unacked
           edge (the join window) so a push that raced ahead isn't lost */
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
    /* concatenated submessages; each length comes from its header, so no framing */
    while (rem>=3){
        uint8_t b0=p[0], type=(uint8_t)(b0 & DART_MSG_MASK); uint16_t alias; size_t sub; int ci;
        switch(type){
            case DART_DATA:
#ifdef DART_SHM
                            if (b0 & DART_F_SHM){ if (rem<DART_SHM_DATA_BYTES) return; sub=DART_SHM_DATA_BYTES; }
                            else
#endif
                            if (b0 & DART_F_SINGLE){ if (rem<13) return; sub=13u+(size_t)dart_r16(p+11); }
                            else { if (rem<21) return; sub=21u+(size_t)dart_r16(p+19); } break;
            case DART_HB:   if (rem<23) return; sub=23; break;
            case DART_NACK: if (rem<21) return; sub=21; break;
            default: return;             /* unknown type: cannot resync, drop rest */
        }
        if (sub>rem) return;             /* truncated/malformed */
        alias = dart_r16(p+1);
        if ((uint32_t)alias < st->amax){
            uint16_t m=st->alias_ci[(size_t)ps*st->amax+alias]; ci=(m==0xFFFFu)?-1:(int)m;
        } else ci=-1;
        if (ci>=0){
            switch(type){
                case DART_DATA:
#ifdef DART_SHM
                                if (p[0] & DART_F_SHM){ dart_reader_shm(st,ci,ps,p,now); break; }
#endif
                                dart_reader_data(st,ci,ps,p,now); break;
                case DART_HB:   dart_reader_hb  (st,ci,ps,p,now); break;
                case DART_NACK: dart_writer_nack(st,ci,ps,p); break;
            }
        }
        p+=sub; rem-=sub;
    }
}

/* produce one writer submessage for (ci,pslot) if due and it fits cap; 0 if none.
 * On no-fit, state is untouched so the same submessage is produced next time. */
static size_t dart_writer_emit(dart_state *st, int ci, int pslot, uint8_t *out, size_t cap, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    dart_wproxy *w=&st->wprox[(size_t)ci*st->cfg.max_peers+pslot];
    int reliable=(ch->qos.reliability==DART_RELIABLE);
    /* group mode active only while a multicast channel has remote subscribers: new
       data + HBs ride the group lane, this per-peer lane only answers NACKs */
    int group_mode = ch->multicast && ch->nsubs>0;
    uint16_t alias = dart__alias_of(st, ci);
    if (!w->used || st->peer_dormant[pslot]) return 0;   /* dormant: out of flow control */
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
#ifdef DART_SHM
                    if (st->peer_shm[pslot] && s->shm){   /* re-send the whole message as one SHM-DATA */
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
                    uint16_t fi=(uint16_t)(seqno - s->base);
                    uint32_t off=(uint32_t)fi*st->frag;
                    uint16_t plen=(uint16_t)((s->len-off)<st->frag?(s->len-off):st->frag);
                    if (cap < (size_t)(s->count==1?13u:21u)+(size_t)plen) return 0;   /* bit stays set */
                    w->nack_bits &= ~(1u<<i);
                    if (w->nack_bits==0) w->has_nack=0;
                    return dart_mk_data(out,alias,seqno,s,fi,dart__sbuf(s)+off,plen);
                    }
                } else {
                    /* superseded: skip the reader past the dropped region with an HB
                       (its first = our floor); keep still-cached seqnos for later repair */
                    uint64_t floor = (ch->have_first?ch->first_seqno:ch->next_seqno);
                    uint32_t j;
                    if (cap < 23) return 0;
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
        dart_wsample *s=dart_find_sample(ch,seqno);
        if (s){
#ifdef DART_SHM
            /* peer_shm is set at attach (before data flows), so sent_upto sits at a
               sample boundary here: emit the whole message as one SHM-DATA */
            if (st->peer_shm[pslot] && s->shm){
                if (cap < DART_SHM_DATA_BYTES) return 0;
                w->sent_upto = s->base + s->count;
                return dart_mk_shm(out,alias,s->base,s->count,s->desc);
            }
#endif
            {
            uint16_t fi=(uint16_t)(seqno - s->base);
            uint32_t off=(uint32_t)fi*st->frag;
            uint16_t plen=(uint16_t)((s->len-off)<st->frag?(s->len-off):st->frag);
            if (cap < (size_t)(s->count==1?13u:21u)+(size_t)plen) return 0;
            w->sent_upto++;
            return dart_mk_data(out,alias,seqno,s,fi,dart__sbuf(s)+off,plen);
            }
        } else {
            /* fell out of the ring before we sent it: skip the reader up to first
               cached with an HB (its first = our floor) */
            if (cap < 23) return 0;
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

/* produce a reader ACKNACK for (ci,pslot) if due; 0 if none */
static size_t dart_reader_emit(dart_state *st, int ci, int pslot, uint8_t *out, size_t cap, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    dart_rproxy *r=&st->rprox[(size_t)ci*st->cfg.max_peers+pslot];
    uint64_t base; uint16_t nbits=0; uint32_t bm=0;
    uint16_t alias = dart__alias_of(st, ci);
    if (!r->used || st->peer_dormant[pslot]) return 0;   /* dormant: don't ack a silent writer */
    if (ch->qos.reliability!=DART_RELIABLE) return 0;
    if (cap<21) return 0;
    if (!r->ack_pending || now<r->ack_due_us) return 0;
    r->ack_pending=0;

    if (!r->asm_active){
        base=r->deliver_upto;
        if (!r->started){ nbits=0; bm=0; }   /* no position yet: F_UNPOS announces our epoch */
        else if (r->deliver_upto<=r->hb_last){
            /* request the whole missing window so one round-trip repairs a burst */
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

/* multicast: 1 if every matched subscriber has acked all data, so the group
 * heartbeat can stop until new data arrives or a new/lagging subscriber needs it */
static int dart__group_all_acked(dart_state *st, int ci){
    uint32_t np=st->cfg.max_peers, p; uint64_t seq=st->chans[ci].next_seqno;
    for (p=0;p<np;p++){
        dart_wproxy *w=&st->wprox[(size_t)ci*np+p];
        if (w->used && !st->peer_dormant[p] && w->acked_upto < seq) return 0;
    }
    return 1;
}

/* multicast writer lane: new data once for the whole group, then a channel-level
 * heartbeat (reliable). Same per-call contract as dart_writer_emit. */
static size_t dart_group_emit(dart_state *st, int ci, uint8_t *out, size_t cap, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    uint16_t alias = dart__alias_of(st, ci);
    if (!ch->multicast || ch->role==DART_SUB_ONLY || ch->role==DART_INACTIVE) return 0;
    if (ch->nsubs==0){
        /* no remote subscribers: pin the cursor forward so a future join gets no stale replay */
        ch->mc_sent_upto = ch->next_seqno;
        return 0;
    }
    if (ch->mc_sent_upto < ch->next_seqno){
        uint64_t seqno=ch->mc_sent_upto;
        dart_wsample *s=dart_find_sample(ch,seqno);
        if (s){
            uint16_t fi=(uint16_t)(seqno - s->base);
            uint32_t off=(uint32_t)fi*st->frag;
            uint16_t plen=(uint16_t)((s->len-off)<st->frag?(s->len-off):st->frag);
            if (cap < (size_t)(s->count==1?13u:21u)+(size_t)plen) return 0;
            ch->mc_sent_upto++;
            return dart_mk_data(out,alias,seqno,s,fi,s->buf+off,plen);
        } else {
            /* overran the ring: skip the group past it with an HB (first = our floor) */
            if (cap < 23) return 0;
            ch->mc_sent_upto=(ch->have_first?ch->first_seqno:ch->next_seqno);
            ch->mc_hb_next_us = now + ch->qos.heartbeat_us;
            dart__deadline(st, ch->mc_hb_next_us);
            ch->mc_hb_count++;
            return dart_mk_hb(out,alias,(ch->have_first?ch->first_seqno:0),ch->next_seqno-1,ch->mc_hb_count);
        }
    }
    if (ch->qos.reliability==DART_RELIABLE && now>=ch->mc_hb_next_us && ch->next_seqno>0
        && !dart__group_all_acked(st, ci)){
        if (cap < 23) return 0;
        ch->mc_hb_next_us = now + ch->qos.heartbeat_us;
        dart__deadline(st, ch->mc_hb_next_us);
        ch->mc_hb_count++;
        return dart_mk_hb(out,alias,(ch->have_first?ch->first_seqno:0),ch->next_seqno-1,ch->mc_hb_count);
    }
    return 0;
}

/* sendable work a popped lane still owes now (timer-armed work is the sweep's job) */
static int dart__lane_work(dart_state *st, uint16_t ci, uint32_t ps, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    uint32_t np=st->cfg.max_peers;
    if (ps==np)
        return ch->multicast && ch->nsubs>0 && ch->mc_sent_upto < ch->next_seqno;
    if (!st->peer_used[ps] || st->peer_dormant[ps]) return 0;   /* dormant: out of flow control */
    { dart_wproxy *w=&st->wprox[(size_t)ci*np+ps];
      dart_rproxy *r=&st->rprox[(size_t)ci*np+ps];
      int group_mode = ch->multicast && ch->nsubs>0;
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
    uint32_t np=st->cfg.max_peers, lanes=np+1u;
    uint32_t total=(uint32_t)st->cfg.n_channels*lanes, due, k;
    uint64_t span = now - st->sweep_t;
    int forced = (now >= st->next_deadline_us);    /* a tracked timer is due */
    int full;
    uint64_t mind = DART__NO_DEADLINE;             /* earliest not-yet-due timer seen */
    due = (forced || span >= DART_HB_SWEEP_US) ? total
        : (uint32_t)(span * total / DART_HB_SWEEP_US);
    if (!due) return;              /* sweep_t advances only when lanes are paid */
    full = (due >= total);         /* covered every lane -> mind is the global minimum */
    st->sweep_t = now;
    for (k=0;k<due;k++){
        uint32_t L=st->sweep, ps=L%lanes;
        uint16_t ci=(uint16_t)(L/lanes);
        dart_channel *ch=&st->chans[ci];
        st->sweep = (st->sweep+1u>=total) ? 0u : st->sweep+1u;
        /* gate writer/multicast heartbeats on next_seqno, never the reader ack: a
           sub-only node's data channels never advance next_seqno but still owe acks */
        if (ch->qos.reliability!=DART_RELIABLE) continue;
        if (ps==np){
            if (ch->next_seqno && ch->multicast && ch->nsubs>0 && !dart__group_all_acked(st,(int)ci)){
                if (now>=ch->mc_hb_next_us) dart__lane_enq(st,ci,ps);
                else if (ch->mc_hb_next_us < mind) mind = ch->mc_hb_next_us;
            }
            continue;
        }
        if (!st->peer_used[ps] || st->peer_dormant[ps]) continue;   /* dormant: out of flow control */
        { dart_wproxy *w=&st->wprox[(size_t)ci*np+ps];
          dart_rproxy *r=&st->rprox[(size_t)ci*np+ps];
          int group_mode = ch->multicast && ch->nsubs>0;
          if (w->used && !group_mode && w->acked_upto < ch->next_seqno){
              if (now>=w->hb_next_us) dart__lane_enq(st,ci,ps);
              else if (w->hb_next_us < mind) mind = w->hb_next_us;
          }
          if (r->used && r->ack_pending){
              if (now>=r->ack_due_us) dart__lane_enq(st,ci,ps);
              else if (r->ack_due_us < mind) mind = r->ack_due_us;
          }
        }
    }
    /* a full pass saw every timer: mind is the exact next deadline. Lanes woken above
       re-arm during emit (dart__deadline) and re-lower it; reader acks just clear. */
    if (full) st->next_deadline_us = mind;
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
                    /* acks first: small, one-shot, and carry the NACKs that drive
                       repair, so a backlogged writer can't starve them */
                    n=dart_reader_emit(st,(int)ci,(int)ps,(uint8_t*)out+off,cap-off,now);
                    if (!n) n=dart_writer_emit(st,(int)ci,(int)ps,(uint8_t*)out+off,cap-off,now);
                }
                off+=n;
            } while (n && off<cap);
            st->dest_head[d]=st->lane_next[L];
            if (dart__lane_work(st,ci,ps,now)){
                /* datagram full mid-lane: rotate the lane to the back so siblings get the next */
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

uint64_t dart_next_deadline_us(dart_state *st){
    return st->next_deadline_us == DART__NO_DEADLINE ? 0 : st->next_deadline_us;
}
