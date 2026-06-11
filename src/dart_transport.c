/* sans-IO reliable-UDP transport core. See dart_transport.h. */
#include "dart_transport.h"
#include <string.h>

#define DART_DATA 1
#define DART_HB   2
#define DART_NACK 3
#define DART_GAP  4

#define DART_NACK_WINDOW 32u    /* seqnos covered by one ACKNACK bitmap */
#define DART__NIL 0xFFFFFFFFu

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
    uint8_t  local;      /* peer lives on this host         */
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
    uint16_t  id;
    uint16_t  maxfrags;     /* ceil(max_sample_bytes/FRAG) */
    uint8_t   dir;          /* dart_direction */
    uint8_t   mcast;
    uint16_t  nsubs_local;  /* live local  subscribers of our writer */
    uint16_t  nsubs_remote; /* live remote subscribers; >0 = group mode */
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
    dart_channel_def metadef;
    dart_state *st = (dart_state*)dart_take(b, sizeof(dart_state), 16);
    if (st && b->base) memset(st, 0, sizeof(*st));

    if (mids < 2u*ncu) mids = 2u*ncu;     /* our own list must always fit */
    memset(&metadef, 0, sizeof metadef);
    metadef.channel_id          = DART_CHAN_META;
    metadef.qos.reliability     = DART_RELIABLE;
    metadef.qos.history_depth   = 1;       /* latest interest list wins */
    metadef.qos.join_replay     = 1;       /* joiners get the current list */
    metadef.qos.max_sample_bytes= 4u + 2u*mids;

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
      if (st && b->base){
          st->cfg=*cfg; st->peer_ids=pi; st->peer_used=pu; st->peer_local=pl;
          st->peer_pub_bm=pb; st->peer_sub_bm=sb; st->bmlen=bml;
          st->cfg.n_channels=(uint16_t)nc; st->meta_ci=(uint16_t)ncu;
          st->chans=ch; st->wprox=wp; st->rprox=rp;
          st->lane_next=ln; st->lane_inq=li;
          st->dest_head=dh; st->dest_tail=dt; st->dest_inq=di; st->destq=dq;
          memset(pu,0,np); memset(pl,0,np);
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
    for (i=0;i<cfg->n_channels;i++)
        if (cfg->channels[i].channel_id==DART_CHAN_META) return NULL;  /* reserved */
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

/* group mode toggled: local subscriber lanes hand over to or resume from the
 * group cursor (reliable readers backfill any seam via NACK). A resuming lane
 * may inherit backlog the group never sent, so wake it. */
static void dart_sync_local_lanes(dart_state *st, uint16_t c, dart_channel *chn){
    uint32_t np=st->cfg.max_peers; uint16_t pj;
    for (pj=0;pj<np;pj++){
        dart_wproxy *lw=&st->wprox[(size_t)c*np+pj];
        if (!lw->used || !lw->local) continue;
        if (lw->sent_upto < chn->mc_sent_upto) lw->sent_upto = chn->mc_sent_upto;
        if (lw->sent_upto < chn->next_seqno) dart__lane_wake(st, c, pj);
    }
}

/* per-channel match/unmatch: create or destroy one proxy, keeping the mcast
 * subscriber accounting and join-seqno rules in one place */
static void dart__match_w(dart_state *st, uint16_t c, uint16_t ps){
    dart_channel *chn=&st->chans[c];
    dart_wproxy *w=&st->wprox[(size_t)c*st->cfg.max_peers+ps];
    memset(w,0,sizeof(*w));
    w->used=1; w->local=st->peer_local[ps];
    if (!chn->mcast){
        w->sent_upto = dart_unicast_join_seqno(chn);
    } else if (w->local){
        chn->nsubs_local++;
        /* local subscriber: unicast lane while no remote subscribers exist,
           else it rides the already joined group */
        w->sent_upto = (chn->nsubs_remote==0)
                       ? dart_unicast_join_seqno(chn) : chn->mc_sent_upto;
    } else {
        if (chn->nsubs_remote==0){
            /* first remote subscriber: group cursor takes over at the head,
               local lanes stop pushing */
            chn->mc_sent_upto = chn->next_seqno;
            dart_sync_local_lanes(st, c, chn);
        }
        chn->nsubs_remote++;
        /* group lane carries new data; this lane is repairs only.
           Reliable-from-join-point: replay only via NACK backfill. */
        w->sent_upto = chn->mc_sent_upto;
    }
    w->acked_upto = w->sent_upto;
    dart__lane_wake(st, c, ps);   /* join replay + first heartbeat */
}
static void dart__unmatch_w(dart_state *st, uint16_t c, uint16_t ps){
    dart_channel *chn=&st->chans[c];
    dart_wproxy *w=&st->wprox[(size_t)c*st->cfg.max_peers+ps];
    if (!w->used) return;
    if (chn->mcast){
        if (w->local){
            if (chn->nsubs_local) chn->nsubs_local--;
        } else if (chn->nsubs_remote){
            chn->nsubs_remote--;
            if (chn->nsubs_remote==0)
                /* last remote subscriber gone: local lanes resume from
                   where the group cursor stopped */
                dart_sync_local_lanes(st, c, chn);
        }
    }
    w->used=0; w->local=0;
}
static void dart__match_r(dart_state *st, uint16_t c, uint16_t ps){
    dart_rproxy *r=&st->rprox[(size_t)c*st->cfg.max_peers+ps];
    uint8_t *abuf=r->asm_buf, *fbm=r->frag_bm;
    memset(r,0,sizeof(*r));
    r->asm_buf=abuf; r->frag_bm=fbm;
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
    if (ch->mcast && ch->nsubs_remote>0)
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

/* publish our interest list on the meta channel: [npub u16][nsub u16] then LE
 * u16 channel ids, pubs first. Encoded straight into the history slot; with
 * depth 1 the newest list is all any joiner ever replays. */
static void dart__meta_publish(dart_state *st){
    dart_channel *mc=&st->chans[st->meta_ci];
    uint8_t *o=mc->hist[mc->hist_head].buf, *p=o+4;
    uint16_t c; uint32_t np=0, ns=0;
    for (c=0;c<st->meta_ci;c++){
        uint8_t d=st->chans[c].dir;
        if (d==DART_PUBSUB || d==DART_PUB_ONLY){ dart_w16(p,st->chans[c].id); p+=2; np++; }
    }
    for (c=0;c<st->meta_ci;c++){
        uint8_t d=st->chans[c].dir;
        if (d==DART_PUBSUB || d==DART_SUB_ONLY){ dart_w16(p,st->chans[c].id); p+=2; ns++; }
    }
    dart_w16(o,(uint16_t)np); dart_w16(o+2,(uint16_t)ns);
    dart__commit(st, st->meta_ci, 4u + 2u*(np+ns));
}

/* a peer's interest list arrived: refresh its bits, rematch every channel */
static void dart__meta_apply(dart_state *st, int ps, const uint8_t *d, size_t len){
    uint16_t np, ns, c; uint32_t i; const uint8_t *pubs, *subs;
    uint8_t *pb=&st->peer_pub_bm[(size_t)ps*st->bmlen];
    uint8_t *sb=&st->peer_sub_bm[(size_t)ps*st->bmlen];
    if (len < 4) return;
    np=dart_r16(d); ns=dart_r16(d+2);
    if (4u + 2u*((uint32_t)np+ns) > len) return;     /* malformed */
    pubs=d+4; subs=pubs+2u*np;
    memset(pb,0,st->bmlen); memset(sb,0,st->bmlen);
    for (c=0;c<st->meta_ci;c++){
        uint16_t id=st->chans[c].id;
        for (i=0;i<np;i++) if (dart_r16(pubs+2u*i)==id){ dart_bset(pb,c); break; }
        for (i=0;i<ns;i++) if (dart_r16(subs+2u*i)==id){ dart_bset(sb,c); break; }
        dart__rematch(st,c,(uint16_t)ps);
    }
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

/* datagram builders (return length) */
static size_t dart_mk_data(uint8_t *o, uint16_t chan, uint64_t seqno, dart_wsample *s,
                         uint16_t frag, const uint8_t *payload, uint16_t plen){
    o[0]=DART_DATA; o[1]=0; dart_w16(o+2,chan);
    dart_w64(o+4,seqno); dart_w16(o+12,frag); dart_w16(o+14,s->count);
    dart_w32(o+16,s->len); dart_w16(o+20,plen);
    memcpy(o+22,payload,plen);
    return 22u+plen;
}
static size_t dart_mk_hb(uint8_t *o, uint16_t chan, uint64_t first, uint64_t last, uint32_t cnt){
    o[0]=DART_HB; o[1]=0; dart_w16(o+2,chan); dart_w64(o+4,first); dart_w64(o+12,last); dart_w32(o+20,cnt);
    return 24;
}
static size_t dart_mk_nack(uint8_t *o, uint16_t chan, uint64_t base, uint16_t nbits, uint32_t bm){
    o[0]=DART_NACK; o[1]=0; dart_w16(o+2,chan); dart_w64(o+4,base); dart_w16(o+12,nbits); dart_w32(o+14,bm);
    return 18;
}
static size_t dart_mk_gap(uint8_t *o, uint16_t chan, uint64_t s, uint64_t e){
    o[0]=DART_GAP; o[1]=0; dart_w16(o+2,chan); dart_w64(o+4,s); dart_w64(o+12,e);
    return 20;
}

/* reader side: handle DATA */
static void dart_reader_data(dart_state *st, int ci, int pslot, const uint8_t *p,
                           uint16_t chan, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    dart_rproxy *r=&st->rprox[(size_t)ci*st->cfg.max_peers+pslot];
    uint64_t seqno=dart_r64(p+4); uint16_t frag=dart_r16(p+12), count=dart_r16(p+14);
    uint32_t slen=dart_r32(p+16); uint16_t plen=dart_r16(p+20);
    uint64_t base = seqno - frag;
    int reliable = (ch->qos.reliability==DART_RELIABLE);

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
            st->cfg.on_gap(st->cfg.user, chan, st->peer_ids[pslot],
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
        if (off+plen<=ch->qos.max_sample_bytes) memcpy(r->asm_buf+off,p+22,plen);
        dart_bset(r->frag_bm,frag);
    }
    /* complete? */
    { uint16_t i; int done=1;
      for (i=0;i<count;i++) if(!dart_bget(r->frag_bm,i)){done=0;break;}
      if (done){
          if (ci==(int)st->meta_ci)
              dart__meta_apply(st, pslot, r->asm_buf, r->asm_len);
          else if (st->cfg.on_sample)
              st->cfg.on_sample(st->cfg.user, chan, st->peer_ids[pslot], r->asm_buf, r->asm_len);
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
    uint64_t first=dart_r64(p+4), last=dart_r64(p+12);
    if (!r->used) return;
    if (ch->qos.reliability!=DART_RELIABLE) return;
    if (first > r->deliver_upto){
        if (r->started && st->cfg.on_gap && ci!=(int)st->meta_ci)  /* superseded before repair */
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
    uint64_t e=dart_r64(p+12);    /* gap start at p+4 is implied by deliver_upto */
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
    dart_wproxy *w=&st->wprox[(size_t)ci*st->cfg.max_peers+pslot];
    uint64_t base=dart_r64(p+4); uint16_t nbits=dart_r16(p+12); uint32_t bm=dart_r32(p+14);
    if (!w->used) return;
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
    /* concatenated submessages; each length comes from its fixed header,
       so no container framing is needed */
    while (rem>=4){
        uint8_t type=p[0]; uint16_t chan=dart_r16(p+2);
        size_t sub; int ci;
        switch(type){
            case DART_DATA: if (rem<22) return; sub=22u+(size_t)dart_r16(p+20); break;
            case DART_HB:   sub=24; break;
            case DART_NACK: sub=18; break;
            case DART_GAP:  sub=20; break;
            default: return;             /* unknown type: cannot resync, drop rest */
        }
        if (sub>rem) return;             /* truncated/malformed */
        if (dart_chan(st,chan,&ci)){
            switch(type){
                case DART_DATA: dart_reader_data(st,ci,ps,p,chan,now); break;
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
    int group_mode = ch->mcast && ch->nsubs_remote>0;
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
                    if (cap < 22u+(size_t)plen) return 0;   /* bit stays set */
                    w->nack_bits &= ~(1u<<i);
                    if (w->nack_bits==0) w->has_nack=0;
                    return dart_mk_data(out,ch->id,seqno,s,fi,s->buf+off,plen);
                } else {
                    /* superseded: GAP the dropped region below the cache, but
                       keep still-cached requested seqnos for repair on the
                       following calls */
                    uint64_t e = (ch->have_first?ch->first_seqno:ch->next_seqno);
                    uint32_t j;
                    if (cap < 20) return 0;
                    if (e>0) e-=1; else e=seqno;
                    if (e<seqno) e=seqno;
                    for (j=0;j<DART_NACK_WINDOW;j++)
                        if (w->nack_base+j <= e) w->nack_bits &= ~(1u<<j);
                    if (w->nack_bits==0) w->has_nack=0;
                    return dart_mk_gap(out,ch->id,w->nack_base,e);
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
            if (cap < 22u+(size_t)plen) return 0;
            w->sent_upto++;
            return dart_mk_data(out,ch->id,seqno,s,fi,s->buf+off,plen);
        } else {
            /* fell out of the ring before we sent it: GAP up to first cached */
            uint64_t e=(ch->have_first?ch->first_seqno:ch->next_seqno);
            uint64_t gs=w->sent_upto;
            if (cap < 20) return 0;
            if (e>0) e-=1; else e=ch->next_seqno-1;
            w->sent_upto=(ch->have_first?ch->first_seqno:ch->next_seqno);
            if (e<gs) e=gs;
            return dart_mk_gap(out,ch->id,gs,e);
        }
    }

    /* 3. heartbeat (reliable, when caught up and timer due) */
    if (reliable && now>=w->hb_next_us && ch->next_seqno>0){
        uint64_t first = ch->have_first ? ch->first_seqno : 0;
        if (cap < 24) return 0;
        /* nothing below acked_upto ever needs repair, so advertise from there:
           a fresh reader then adopts the join point instead of NACKing the
           whole cached ring past its join_replay window */
        if (w->acked_upto > first) first = w->acked_upto;
        w->hb_next_us = now + (ch->qos.heartbeat_us?ch->qos.heartbeat_us:100000u);
        w->hb_count++;
        return dart_mk_hb(out,ch->id,first,ch->next_seqno-1,w->hb_count);
    }
    return 0;
}

/* produce a reader ACKNACK for (ci,pslot) if due; 0 if none */
static size_t dart_reader_emit(dart_state *st, int ci, int pslot, uint8_t *out, size_t cap, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    dart_rproxy *r=&st->rprox[(size_t)ci*st->cfg.max_peers+pslot];
    uint64_t base; uint16_t nbits=0; uint32_t bm=0;
    if (!r->used) return 0;
    if (ch->qos.reliability!=DART_RELIABLE) return 0;
    if (cap<18) return 0;
    if (!r->ack_pending || now<r->ack_due_us) return 0;
    r->ack_pending=0;

    if (!r->asm_active){
        base=r->deliver_upto;
        if (r->deliver_upto<=r->hb_last){
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
    return dart_mk_nack(out,ch->id,base,nbits,bm);
}

/* multicast writer lane for channel ci: new data once for the whole group,
 * then a channel-level heartbeat (reliable). Same per-call contract as
 * dart_writer_emit. */
static size_t dart_group_emit(dart_state *st, int ci, uint8_t *out, size_t cap, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    if (!ch->mcast || ch->dir==DART_SUB_ONLY || ch->dir==DART_NONE) return 0;
    if (ch->nsubs_remote==0){
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
            if (cap < 22u+(size_t)plen) return 0;
            ch->mc_sent_upto++;
            return dart_mk_data(out,ch->id,seqno,s,fi,s->buf+off,plen);
        } else {
            uint64_t e=(ch->have_first?ch->first_seqno:ch->next_seqno);
            uint64_t gs=ch->mc_sent_upto;
            if (cap < 20) return 0;
            if (e>0) e-=1; else e=ch->next_seqno-1;
            ch->mc_sent_upto=(ch->have_first?ch->first_seqno:ch->next_seqno);
            if (e<gs) e=gs;
            return dart_mk_gap(out,ch->id,gs,e);
        }
    }
    if (ch->qos.reliability==DART_RELIABLE && now>=ch->mc_hb_next_us && ch->next_seqno>0){
        if (cap < 24) return 0;
        ch->mc_hb_next_us = now + (ch->qos.heartbeat_us?ch->qos.heartbeat_us:100000u);
        ch->mc_hb_count++;
        return dart_mk_hb(out,ch->id,(ch->have_first?ch->first_seqno:0),ch->next_seqno-1,ch->mc_hb_count);
    }
    return 0;
}

/* event work a popped lane still owes right now (timer-armed work is the
 * sweep's job, so a lane never camps in the queue waiting on a clock) */
static int dart__lane_work(dart_state *st, uint16_t ci, uint32_t ps, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    uint32_t np=st->cfg.max_peers;
    if (ps==np)
        return ch->mcast && ch->nsubs_remote>0 && ch->mc_sent_upto < ch->next_seqno;
    if (!st->peer_used[ps]) return 0;
    { dart_wproxy *w=&st->wprox[(size_t)ci*np+ps];
      dart_rproxy *r=&st->rprox[(size_t)ci*np+ps];
      int group_mode = ch->mcast && ch->nsubs_remote>0;
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
        if (ch->qos.reliability!=DART_RELIABLE || ch->next_seqno==0) continue;
        if (ps==np){
            if (ch->mcast && ch->nsubs_remote>0 && now>=ch->mc_hb_next_us)
                dart__lane_wake(st,ci,ps);
            continue;
        }
        if (!st->peer_used[ps]) continue;
        { dart_wproxy *w=&st->wprox[(size_t)ci*np+ps];
          dart_rproxy *r=&st->rprox[(size_t)ci*np+ps];
          int group_mode = ch->mcast && ch->nsubs_remote>0;
          if ((w->used && !group_mode && now>=w->hb_next_us)
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
                    /* acks first: they are 18 bytes, one-shot, and carry the
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
                              : DART_DEST_GROUP(st->chans[d-np].id);
            *out_len = off;
            return 1;
        }
        if (st->dest_head[d]!=DART__NIL)
            return 0;    /* work pending but nothing fit: caller's cap too small */
    }
    return 0;
}
