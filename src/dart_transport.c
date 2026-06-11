/* sans-IO reliable-UDP transport core. See dart_transport.h. */
#include "dart_transport.h"
#include <string.h>

#define DART_DATA 1
#define DART_HB   2
#define DART_NACK 3
#define DART_GAP  4

#define DART_NACK_WINDOW 32u    /* seqnos covered by one ACKNACK bitmap */

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
    dart_config    cfg;
    uint32_t    *peer_ids;  /* [max_peers] */
    uint8_t     *peer_used; /* [max_peers] */
    dart_channel  *chans;     /* [n_channels] */
    dart_wproxy   *wprox;     /* [n_channels*max_peers] */
    dart_rproxy   *rprox;     /* [n_channels*max_peers] */
    /* poll_send round-robin cursor */
    uint32_t     scan;
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

/* Lay out everything; b->base==NULL means measure only. Returns state ptr. */
static dart_state *dart_build(dart_bump *b, const dart_config *cfg){
    uint16_t c, p; uint32_t np = cfg->max_peers, nc = cfg->n_channels;
    dart_state *st = (dart_state*)dart_take(b, sizeof(dart_state), 16);
    if (st && b->base) memset(st, 0, sizeof(*st));

    { uint32_t *pi = (uint32_t*)dart_take(b, np*sizeof(uint32_t), 8);
      uint8_t  *pu = (uint8_t*) dart_take(b, np*sizeof(uint8_t), 1);
      dart_channel *ch = (dart_channel*)dart_take(b, nc*sizeof(dart_channel), 16);
      dart_wproxy *wp = (dart_wproxy*)dart_take(b, (size_t)nc*np*sizeof(dart_wproxy), 16);
      dart_rproxy *rp = (dart_rproxy*)dart_take(b, (size_t)nc*np*sizeof(dart_rproxy), 16);
      if (st && b->base){
          st->cfg=*cfg; st->peer_ids=pi; st->peer_used=pu; st->chans=ch;
          st->wprox=wp; st->rprox=rp; st->scan=0;
          memset(pu,0,np); memset(wp,0,(size_t)nc*np*sizeof(dart_wproxy));
          memset(rp,0,(size_t)nc*np*sizeof(dart_rproxy));
      }
    }

    for (c=0;c<nc;c++){
        const dart_qos *q = &cfg->channels[c].qos;
        uint16_t depth = q->history_depth ? q->history_depth : 1;
        uint16_t mf = dart_maxfrags(q->max_sample_bytes);
        dart_wsample *hist = (dart_wsample*)dart_take(b, depth*sizeof(dart_wsample), 16);
        uint16_t d;
        if (st && b->base){
            dart_channel *ch = &st->chans[c];
            memset(ch,0,sizeof(*ch));
            ch->qos=*q; ch->id=cfg->channels[c].channel_id; ch->maxfrags=mf;
            ch->dir=cfg->channels[c].dir; ch->mcast=cfg->channels[c].mcast;
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

dart_state *dart_init(void *mem, size_t cap, const dart_config *cfg){
    dart_bump b; dart_state *st;
    if (!mem || !cfg || cfg->n_channels==0 || cfg->max_peers==0) return NULL;
    memset(&b,0,sizeof b);
    b.base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    b.cap  = cap - (size_t)((uint8_t*)b.base - (uint8_t*)mem);
    st = dart_build(&b, cfg);
    if (!st || b.oom) return NULL;
    /* cfg.channels still points at caller memory; only read during init,
       so detach it now. */
    st->cfg.channels = NULL;
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

static int dart_idlist_has(const uint16_t *ids, uint16_t n, uint16_t id){
    uint16_t i;
    if (!ids) return 1;                 /* NULL list = all channels */
    for (i=0;i<n;i++) if (ids[i]==id) return 1;
    return 0;
}

/* unicast join seqno: reliable replays cached history (latest-value on join),
 * best-effort starts at the head so a late joiner sees only future samples. */
static uint64_t dart_unicast_join_seqno(const dart_channel *chn){
    return (chn->qos.reliability==DART_RELIABLE && chn->have_first)
           ? chn->first_seqno : chn->next_seqno;
}

/* group mode toggled: local subscriber lanes hand over to or resume from the
 * group cursor (reliable readers backfill any seam via NACK). */
static void dart_sync_local_lanes(dart_state *st, uint16_t c, dart_channel *chn){
    uint32_t np=st->cfg.max_peers; uint16_t pj;
    for (pj=0;pj<np;pj++){
        dart_wproxy *lw=&st->wprox[(size_t)c*np+pj];
        if (lw->used && lw->local && lw->sent_upto < chn->mc_sent_upto)
            lw->sent_upto = chn->mc_sent_upto;
    }
}

void dart_peer_add(dart_state *st, uint32_t id,
                 const uint16_t *peer_pubs, uint16_t npub,
                 const uint16_t *peer_subs, uint16_t nsub,
                 int peer_is_local){
    uint16_t i; int free=-1; uint16_t c; uint32_t np=st->cfg.max_peers;
    if (dart_peer_slot(st,id)>=0) return;
    for (i=0;i<np;i++) if(!st->peer_used[i]){free=(int)i;break;}
    if (free<0) return;
    st->peer_used[free]=1; st->peer_ids[free]=id;
    for (c=0;c<st->cfg.n_channels;c++){
        dart_channel *chn=&st->chans[c];
        dart_wproxy *w=&st->wprox[(size_t)c*np+free];
        dart_rproxy *r=&st->rprox[(size_t)c*np+free];
        uint8_t *abuf=r->asm_buf, *fbm=r->frag_bm;
        /* writer proxy only when we publish and the peer subscribes; reader
           proxy only when the peer publishes and we subscribe */
        int wuse = (chn->dir!=DART_SUB_ONLY) && dart_idlist_has(peer_subs,nsub,chn->id);
        int ruse = (chn->dir!=DART_PUB_ONLY) && dart_idlist_has(peer_pubs,npub,chn->id);
        memset(w,0,sizeof(*w)); memset(r,0,sizeof(*r));
        r->asm_buf=abuf; r->frag_bm=fbm;
        w->used=(uint8_t)wuse; r->used=(uint8_t)ruse;
        if (wuse){
            w->local = (uint8_t)(peer_is_local?1:0);
            if (!chn->mcast){
                w->sent_upto = dart_unicast_join_seqno(chn);
            } else if (peer_is_local){
                chn->nsubs_local++;
                /* local subscriber: unicast lane while no remote subscribers
                   exist, else it rides the already joined group */
                w->sent_upto = (chn->nsubs_remote==0)
                               ? dart_unicast_join_seqno(chn) : chn->mc_sent_upto;
            } else {
                if (chn->nsubs_remote==0){
                    /* first remote subscriber: group cursor takes over at the
                       head, local lanes stop pushing */
                    chn->mc_sent_upto = chn->next_seqno;
                    dart_sync_local_lanes(st, c, chn);
                }
                chn->nsubs_remote++;
                /* group lane carries new data; this lane is repairs only.
                   Reliable-from-join-point: replay only via NACK backfill. */
                w->sent_upto = chn->mc_sent_upto;
            }
            w->acked_upto = w->sent_upto;
            w->hb_next_us = 0;
        }
        r->deliver_upto = 0;
    }
}

void dart_peer_remove(dart_state *st, uint32_t id){
    int s = dart_peer_slot(st,id); uint16_t c; uint32_t np=st->cfg.max_peers;
    if (s<0) return;
    st->peer_used[s]=0;
    for (c=0;c<st->cfg.n_channels;c++){
        dart_channel *chn=&st->chans[c];
        dart_wproxy *w=&st->wprox[(size_t)c*np+s];
        if (w->used && chn->mcast){
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
        st->rprox[(size_t)c*np+s].used=0;
    }
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

int dart_send(dart_state *st, uint16_t channel_id, const void *data, size_t len, uint64_t now){
    int ci; dart_channel *ch = dart_chan(st, channel_id, &ci);
    uint16_t depth, count; dart_wsample *slot;
    (void)now;
    if (!ch) return -1;
    if (len > ch->qos.max_sample_bytes) return -2;
    if (ch->dir == DART_SUB_ONLY) return -3;
    depth = ch->qos.history_depth ? ch->qos.history_depth : 1;
    count = (uint16_t)((len + DART_FRAG_PAYLOAD - 1) / DART_FRAG_PAYLOAD);
    if (count==0) count=1;

    slot = &ch->hist[ch->hist_head];
    slot->valid=1; slot->base=ch->next_seqno; slot->count=count; slot->len=(uint32_t)len;
    if (len) memcpy(slot->buf, data, len);
    ch->hist_head = (uint16_t)((ch->hist_head+1) % depth);
    ch->next_seqno += count;

    /* oldest cached: the slot the head now points at once the ring wrapped,
       else slot 0 */
    ch->first_seqno = ch->hist[ch->hist_head].valid ? ch->hist[ch->hist_head].base
                                                    : ch->hist[0].base;
    ch->have_first  = 1;

    /* poll_send picks this up via sent_upto < next_seqno */
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
            return;                                      /* gap in stream: wait/NACK */
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
          if (st->cfg.on_sample)
              st->cfg.on_sample(st->cfg.user, chan, st->peer_ids[pslot], r->asm_buf, r->asm_len);
          r->deliver_upto = base + count;
          r->asm_active=0;
      }
    }
    if (reliable){ r->ack_pending=1; r->ack_due_us=now + ch->qos.nack_delay_us; }
}

static void dart_reader_hb(dart_state *st, int ci, int pslot, const uint8_t *p, uint64_t now){
    dart_channel *ch=&st->chans[ci];
    dart_rproxy *r=&st->rprox[(size_t)ci*st->cfg.max_peers+pslot];
    uint64_t first=dart_r64(p+4), last=dart_r64(p+12);
    if (!r->used) return;
    if (ch->qos.reliability!=DART_RELIABLE) return;
    if (first > r->deliver_upto){
        if (r->started && st->cfg.on_gap)        /* superseded before repair */
            st->cfg.on_gap(st->cfg.user, ch->id, st->peer_ids[pslot],
                           r->deliver_upto, first - r->deliver_upto);
        r->deliver_upto=first; r->asm_active=0;
    }
    r->hb_last=last;
    r->ack_pending=1; r->ack_due_us = now + ch->qos.nack_delay_us;
}

static void dart_reader_gap(dart_state *st, int ci, int pslot, const uint8_t *p){
    dart_rproxy *r=&st->rprox[(size_t)ci*st->cfg.max_peers+pslot];
    uint64_t e=dart_r64(p+12);    /* gap start at p+4 is implied by deliver_upto */
    if (!r->used) return;
    if (e+1 > r->deliver_upto){
        if (r->started && st->cfg.on_gap)
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
    if (nbits>0 && bm!=0){ w->has_nack=1; w->nack_base=base; w->nack_bits=bm; }
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
        if (cap < 24) return 0;
        w->hb_next_us = now + (ch->qos.heartbeat_us?ch->qos.heartbeat_us:100000u);
        w->hb_count++;
        return dart_mk_hb(out,ch->id,(ch->have_first?ch->first_seqno:0),ch->next_seqno-1,w->hb_count);
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
    if (!ch->mcast || ch->dir==DART_SUB_ONLY) return 0;
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

int dart_poll_send(dart_state *st, uint32_t *to_peer, void *out, size_t cap, size_t *out_len, uint64_t now){
    uint32_t np=st->cfg.max_peers, nc=st->cfg.n_channels;
    uint32_t lanes=np+1u, total=nc*lanes, k;
    for (k=0;k<total;k++){
        uint32_t idx=(st->scan+k)%total;
        uint16_t ci=(uint16_t)(idx/lanes), ps=(uint16_t)(idx%lanes);
        size_t n;
        if (ps==np){
            /* group lane for channel ci: batch only same-channel submessages,
               since each channel maps to its own multicast group */
            n=dart_group_emit(st,ci,(uint8_t*)out,cap,now);
            if (n){
                size_t off=n;
                while (off<cap){
                    size_t m=dart_group_emit(st,ci,(uint8_t*)out+off,cap-off,now);
                    if (!m) break;
                    off+=m;
                }
                *to_peer=DART_DEST_GROUP(st->chans[ci].id); *out_len=off;
                st->scan=(idx+1)%total;
                return 1;
            }
            continue;
        }
        if (!st->peer_used[ps]) continue;
        n=dart_writer_emit(st,ci,ps,(uint8_t*)out,cap,now);
        if (!n) n=dart_reader_emit(st,ci,ps,(uint8_t*)out,cap,now);
        if (n){
            /* opportunistic batching: append whatever else is already due for
               this peer until the datagram is full. Never waits for future
               data, so latency is unaffected: it only makes fuller datagrams. */
            size_t off=n; uint16_t c2; int progress=1;
            while (progress && off<cap){
                progress=0;
                for (c2=0;c2<nc;c2++){
                    size_t m=dart_writer_emit(st,c2,ps,(uint8_t*)out+off,cap-off,now);
                    if (!m) m=dart_reader_emit(st,c2,ps,(uint8_t*)out+off,cap-off,now);
                    if (m){ off+=m; progress=1; }
                }
            }
            *to_peer=st->peer_ids[ps]; *out_len=off;
            st->scan=(idx+1)%total;
            return 1;
        }
    }
    return 0;
}
