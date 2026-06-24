/* sans-IO reliable-UDP transport core: state, init/teardown, peer + interest matching,
 * the RX demux, and public queries. The wire codec, scheduler, and writer/reader paths
 * live in transport/{wire,sched,writer,reader}.c; shared decls in transport/internal.h. */
#include "core.h"
#include "../common/bytes.h"
#include "../common/arena.h"
#include "internal.h"
#include <string.h>


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


/* Reader-side fragment-count bound: a peer may fragment at the smallest size in
 * the deployment, so size the reassembly bitmap by DART_FRAG_PAYLOAD_MIN. */
static uint16_t dart_max_fragments(uint32_t max_message_bytes){
    uint32_t f = (max_message_bytes + DART_FRAG_PAYLOAD_MIN - 1) / DART_FRAG_PAYLOAD_MIN;
    if (f == 0) f = 1;
    return (uint16_t)f;
}


/* zero-means-default for the tunable QoS fields, applied once at init so the
 * stored qos is authoritative */
static void dart__qos_defaults(dart_qos *q, int dynamic){
    if (q->keep_last == 0)        q->keep_last       = DART_QOS_DEF_KEEP_LAST;
    if (q->heartbeat_us == 0)     q->heartbeat_us    = DART_QOS_DEF_HEARTBEAT_US;
    if (q->repair_delay_us == 0)  q->repair_delay_us = DART_QOS_DEF_REPAIR_US;
    /* fixed mode only: a dynamic channel keeps 0 = grow-to-fit via allocator */
    if (!dynamic && q->max_message_bytes == 0) q->max_message_bytes = DART_FRAG_PAYLOAD;
}


/* Normalize a node/peer UDP fragment size: 0 -> default, then clamp to [MIN,MAX].
   Public so the transport (dart_init) and the node (announce blob) clamp identically. */
uint16_t dart_clamp_frag(uint16_t frag_payload){
    uint16_t f = frag_payload ? frag_payload : DART_FRAG_PAYLOAD;
    if (f < DART_FRAG_PAYLOAD_MIN) f = DART_FRAG_PAYLOAD_MIN;
    if (f > DART_FRAG_PAYLOAD_MAX) f = DART_FRAG_PAYLOAD_MAX;
    return f;
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
          st->frag = dart_clamp_frag(cfg->frag_payload);
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
                dart_reader_proxy *r = dart__reader_proxy_at(st,c,p);
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


int dart_peer_slot(dart_state *st, uint32_t id){
    uint16_t i;
    for (i=0;i<st->cfg.max_peers;i++) if (st->peer_used[i] && st->peer_ids[i]==id) return (int)i;
    return -1;
}

/* the local handle IS the channel's index; out-of-range rejected */
dart_channel *dart_chan(dart_state *st, uint16_t channel, int *idx_out){
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
void dart__event(dart_state *st, dart_event_kind kind, uint16_t channel,
                        uint32_t peer, uint64_t first, uint64_t count, const char *detail){
    dart_event ev;
    if (!st->cfg.on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind=kind; ev.channel=channel; ev.peer=peer; ev.first=first; ev.count=count;
    ev.detail=detail;
    st->cfg.on_event(st->cfg.user, &ev);
}


/* unicast join seqno: head minus qos.catch_up cached samples (reliable only) */
uint64_t dart_unicast_join_seqno(const dart_channel *ch){
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
    dart_writer_proxy *w=dart__writer_proxy_at(st,c,peer_slot);
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
    dart_writer_proxy *w=dart__writer_proxy_at(st,c,peer_slot);
    if (!w->used) return;
    if (ch->multicast && ch->n_subscribers) ch->n_subscribers--;
    w->used=0;
}

static void dart__match_r(dart_state *st, uint16_t c, uint16_t peer_slot){
    dart_reader_proxy *r=dart__reader_proxy_at(st,c,peer_slot);
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
    dart_reader_proxy *r=dart__reader_proxy_at(st,c,peer_slot);
    r->used=0; r->assembly_active=0;
}


/* recompute one (channel,peer) match from our role and the peer's interest bits */
static void dart__rematch(dart_state *st, uint16_t c, uint16_t peer_slot){
    dart_channel *ch=&st->channels[c];
    const uint8_t *peer_pub_bitmap=&st->peer_pub_bitmap[(size_t)peer_slot*st->bitmap_len];
    const uint8_t *peer_sub_bitmap=&st->peer_sub_bitmap[(size_t)peer_slot*st->bitmap_len];
    int wuse = (ch->role==DART_PUBSUB || ch->role==DART_PUB_ONLY) && dart_bget(peer_sub_bitmap,c);
    int ruse = (ch->role==DART_PUBSUB || ch->role==DART_SUB_ONLY) && dart_bget(peer_pub_bitmap,c);
    dart_writer_proxy *w=dart__writer_proxy_at(st,c,peer_slot);
    dart_reader_proxy *r=dart__reader_proxy_at(st,c,peer_slot);
    if (wuse && !w->used) dart__match_w(st,c,peer_slot);
    else if (!wuse && w->used) dart__unmatch_w(st,c,peer_slot);
    if (ruse && !r->used) dart__match_r(st,c,peer_slot);
    else if (!ruse && r->used) dart__unmatch_r(st,c,peer_slot);
}


void dart_peer_add(dart_state *st, uint32_t id, int peer_is_local, uint16_t peer_frag){
    uint16_t i; int free=-1; uint32_t max_peers=st->cfg.max_peers;
    if (dart_peer_slot(st,id)>=0) return;
    for (i=0;i<max_peers;i++) if(!st->peer_used[i]){free=(int)i;break;}
    if (free<0) return;
    st->peer_used[free]=1; st->peer_ids[free]=id;
    st->peer_local[free]=(uint8_t)(peer_is_local?1:0);
    st->peer_dormant[free]=0;
    st->peer_frag[free]=dart_clamp_frag(peer_frag);
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
        dart_writer_proxy *w=dart__writer_proxy_at(st,c,s);
        dart_reader_proxy *r=dart__reader_proxy_at(st,c,s);
        if (st->channels[c].qos.reliability!=DART_RELIABLE) continue;
        if (r->used){ r->ack_pending=1; r->ack_due_us=0; r->ack_force=1; }  /* report our position now */
        if (w->used || r->used) dart__lane_wake(st,c,(uint16_t)s);
    }
}


/* update a peer's advertised fragment size (its blob may arrive after first contact) */
void dart_peer_set_frag(dart_state *st, uint32_t id, uint16_t peer_frag){
    int s = dart_peer_slot(st,id);
    if (s>=0) st->peer_frag[s]=dart_clamp_frag(peer_frag);
}

#ifdef DART_SHM

void dart_peer_set_shm(dart_state *st, uint32_t id, int is_shm){
    int s = dart_peer_slot(st,id);
    if (s>=0) st->peer_shm[s]=(uint8_t)(is_shm?1:0);
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
            dart_reader_proxy *r=dart__reader_proxy_at(st,c,p);
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


void dart_repair_stats(dart_state *st, uint16_t channel, dart_repair_stats_t *out){
    dart_channel *ch = dart_chan(st, channel, NULL);
    if (!out) return;
    if (ch) *out = ch->repair_stats;
    else memset(out, 0, sizeof *out);
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
                            if (b0 & DART_F_SINGLE){ if (rem<DART_HEADER_DATA_SINGLE) return; sub=DART_HEADER_DATA_SINGLE+(size_t)dart_le_r16(p+DART_OFFSET_PAYLOAD_LEN_SINGLE); }
                            else { if (rem<DART_HEADER_DATA_MULTI) return; sub=DART_HEADER_DATA_MULTI+(size_t)dart_le_r16(p+DART_OFFSET_PAYLOAD_LEN); } break;
            case DART_HB:   if (rem<DART_HEADER_HB) return; sub=DART_HEADER_HB; break;
            case DART_NACK: if (rem<DART_HEADER_NACK) return; sub=DART_HEADER_NACK; break;
            default: return;             /* unknown type: cannot resync, drop rest */
        }
        if (sub>rem) return;             /* truncated/malformed */
        alias = dart_le_r16(p+DART_OFFSET_ALIAS);
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
