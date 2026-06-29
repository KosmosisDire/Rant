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

uint64_t dart_channel_identity(const DartChannelDef *def){
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
static void dart__qos_defaults(DartQos *q, int dynamic){
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
static DartTransportState *dart_build(i_DartBump *b, const DartConfig *cfg){
    uint16_t c, p; uint32_t max_peers = cfg->max_peers, n_channels = cfg->n_channels;
    uint16_t bitmap_len = (uint16_t)((n_channels+7u)/8u);
    uint32_t meta_ids = DART_META_MAX_IDS;
    uint32_t name_bytes = 0; char *name_pool = NULL;
    DartTransportState *st = (DartTransportState*)dart_take(b, sizeof(DartTransportState), 16);
    if (st && b->base) memset(st, 0, sizeof(*st));

    /* name pool: one fixed-size slot per channel so a reserve-mode slot can be named
       later by dart_channel_define without repacking. ch->name points at its slot. */
    name_bytes = (uint32_t)n_channels * (DART_TOPIC_NAME_MAX + 1u);
    if (meta_ids < 2u*n_channels) meta_ids = 2u*n_channels;     /* our own interest list must always fit */

    { uint32_t nlanes = n_channels*max_peers, ndest = max_peers;
      uint32_t *peer_ids = (uint32_t*)dart_take(b, max_peers*sizeof(uint32_t), 8);
      uint8_t  *peer_used = (uint8_t*) dart_take(b, max_peers*sizeof(uint8_t), 1);
      uint8_t  *peer_dormant= (uint8_t*) dart_take(b, max_peers*sizeof(uint8_t), 1);
      uint16_t *peer_frag = (uint16_t*)dart_take(b, max_peers*sizeof(uint16_t), 2);
#ifdef DART_SHM
      uint8_t  *peer_shm= (uint8_t*) dart_take(b, max_peers*sizeof(uint8_t), 1);
#endif
      uint8_t  *peer_pub_bitmap = (uint8_t*) dart_take(b, (size_t)max_peers*bitmap_len, 1);
      uint8_t  *peer_sub_bitmap = (uint8_t*) dart_take(b, (size_t)max_peers*bitmap_len, 1);
      uint8_t  *peer_sub_reliable = (uint8_t*) dart_take(b, (size_t)max_peers*bitmap_len, 1);
      i_DartChannel *ch = (i_DartChannel*)dart_take(b, n_channels*sizeof(i_DartChannel), 16);
      i_DartWriterProxy *writer_proxies = (i_DartWriterProxy*)dart_take(b, (size_t)n_channels*max_peers*sizeof(i_DartWriterProxy), 16);
      i_DartReaderProxy *reader_proxies = (i_DartReaderProxy*)dart_take(b, (size_t)n_channels*max_peers*sizeof(i_DartReaderProxy), 16);
      uint32_t *lane_next = (uint32_t*)dart_take(b, (size_t)nlanes*sizeof(uint32_t), 8);
      uint8_t  *lane_queued = (uint8_t*) dart_take(b, (size_t)nlanes, 1);
      uint32_t *dest_head = (uint32_t*)dart_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint32_t *dest_tail = (uint32_t*)dart_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint8_t  *dest_queued = (uint8_t*) dart_take(b, (size_t)ndest, 1);
      uint32_t *dest_queue = (uint32_t*)dart_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint16_t *alias_to_channel = (uint16_t*)dart_take(b, (size_t)max_peers*meta_ids*sizeof(uint16_t), 2);
      name_pool = (char*)dart_take(b, name_bytes ? name_bytes : 1u, 1);
      if (st && b->base){
          st->cfg=*cfg; st->peer_ids=peer_ids; st->peer_used=peer_used;
          st->peer_dormant=peer_dormant; st->peer_frag=peer_frag;
          st->frag = dart_clamp_frag(cfg->frag_payload);
          st->peer_pub_bitmap=peer_pub_bitmap; st->peer_sub_bitmap=peer_sub_bitmap;
          st->peer_sub_reliable=peer_sub_reliable; st->bitmap_len=bitmap_len;
          st->channels=ch; st->writer_proxies=writer_proxies; st->reader_proxies=reader_proxies; st->reader_epoch_counter=1;
          st->next_deadline_us=DART__NO_DEADLINE;
          st->lane_next=lane_next; st->lane_queued=lane_queued;
          st->dest_head=dest_head; st->dest_tail=dest_tail; st->dest_queued=dest_queued; st->dest_queue=dest_queue;
          st->alias_to_channel=alias_to_channel; st->alias_max=meta_ids;
          memset(peer_used,0,max_peers); memset(peer_dormant,0,max_peers);
          { uint32_t k; for (k=0;k<max_peers;k++) peer_frag[k]=DART_FRAG_PAYLOAD; }  /* set per peer on add */
#ifdef DART_SHM
          st->peer_shm=peer_shm; memset(peer_shm,0,max_peers);
#endif
          memset(alias_to_channel,0xFF,(size_t)max_peers*meta_ids*sizeof(uint16_t));   /* all unmapped */
          memset(peer_pub_bitmap,0,(size_t)max_peers*bitmap_len); memset(peer_sub_bitmap,0,(size_t)max_peers*bitmap_len);
          memset(peer_sub_reliable,0,(size_t)max_peers*bitmap_len);
          memset(writer_proxies,0,(size_t)n_channels*max_peers*sizeof(i_DartWriterProxy));
          memset(reader_proxies,0,(size_t)n_channels*max_peers*sizeof(i_DartReaderProxy));
          memset(lane_queued,0,nlanes); memset(dest_queued,0,ndest);
          memset(dest_head,0xFF,(size_t)ndest*sizeof(uint32_t));   /* all DART__NIL */
      }
    }

    for (c=0;c<n_channels;c++){
        /* every slot starts inactive with its own name-pool slot; reserve-mode slots
           stay this way until dart_channel_define fills them. */
        if (st && b->base){
            i_DartChannel *ch = &st->channels[c];
            memset(ch,0,sizeof(*ch));
            ch->role = DART_INACTIVE;
            ch->name = name_pool + (size_t)c*(DART_TOPIC_NAME_MAX + 1u);
            ((char*)ch->name)[0] = '\0';
        }
        if (!cfg->channels) continue;    /* reserve mode: arena holds no per-channel buffers */
        {   const DartChannelDef *def = &cfg->channels[c];
            /* dynamic = an allocator is set: buffers grow via the hook, not the arena */
            int dyn = (cfg->allocator != NULL);
            DartQos q = def->qos;            /* local, normalized copy */
            i_DartWriterSample *history; uint16_t depth, max_fragments, d;
            dart__qos_defaults(&q, dyn);
            depth = q.keep_last;
            max_fragments = dart_max_fragments(q.max_message_bytes);
            history = (i_DartWriterSample*)dart_take(b, depth*sizeof(i_DartWriterSample), 16);
            if (st && b->base){
                i_DartChannel *ch = &st->channels[c];
                size_t lane = dart__namelen(def->name);
                ch->qos=q; ch->max_fragments=max_fragments;
                ch->role=def->role; ch->dynamic=(uint8_t)dyn;
                ch->identity = dart_channel_identity(def);
                if (lane){ memcpy((char*)ch->name, def->name, lane); ((char*)ch->name)[lane]='\0'; }
                ch->history=history; ch->history_owned=0; ch->history_head=0; ch->next_seqno=0; ch->have_first=0;
                memset(history,0,depth*sizeof(i_DartWriterSample));
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
                    i_DartReaderProxy *r = dart__reader_proxy_at(st,c,p);
                    r->assembly_buf=assembly_buf; r->frag_bitmap=frag_bitmap;
                    r->assembly_cap = dyn ? 0u : q.max_message_bytes;
                    r->bitmap_cap  = dyn ? 0u : (uint32_t)((max_fragments+7u)/8u);
                }
            }
        }
    }
    return st;
}


size_t dart_required_memory(const DartConfig *cfg){
    i_DartBump b; memset(&b,0,sizeof b);
    if (!cfg || cfg->n_channels==0 || cfg->max_peers==0) return 0;
    dart_build(&b, cfg);
    return b.offset + 16;   /* slack for base alignment */
}


DartTransportState *dart_init(void *mem, size_t cap, const DartConfig *cfg){
    i_DartBump b; DartTransportState *st; uint16_t i;
    if (!mem || !cfg || cfg->n_channels==0 || cfg->max_peers==0) return NULL;
    if (!cfg->channels && !cfg->allocator) return NULL;    /* reserve mode needs an allocator */
    if (cfg->channels) for (i=0;i<cfg->n_channels;i++){
        const DartChannelDef *d = &cfg->channels[i];
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


/* Relocate a live transport into a bigger block at grown counts (dynamic-mode growth).
 * Heap buffers (history rings, sample/assembly bufs, frag bitmaps) are NOT in the arena,
 * so the struct copies carry their pointers across and the OLD arena can be freed without
 * touching them. The 2D tables are re-strided into the new max_peers/n_channels; the
 * active-lane scheduler (indices encode the old strides) is dropped and rebuilt from the
 * proxy state. The caller frees old's arena block afterward; it must NOT dart_destroy old
 * (that would free the heap buffers now owned by the new state). Returns the new state. */
DartTransportState *dart_migrate(DartTransportState *old, void *new_mem, size_t new_cap,
                        uint16_t new_max_peers, uint16_t new_n_channels){
    DartConfig nc; DartTransportState *nw; uint16_t omp, onc, c, p;
    if (!old) return NULL;
    nc = old->cfg; nc.channels = NULL;
    nc.max_peers = new_max_peers; nc.n_channels = new_n_channels;
    nw = dart_init(new_mem, new_cap, &nc);
    if (!nw) return NULL;
    omp = old->cfg.max_peers; onc = old->cfg.n_channels;

    nw->reader_epoch_counter = old->reader_epoch_counter;
    nw->frag = old->frag;
    memcpy(nw->peer_ids,     old->peer_ids,     (size_t)omp*sizeof(uint32_t));
    memcpy(nw->peer_used,    old->peer_used,    omp);
    memcpy(nw->peer_dormant, old->peer_dormant, omp);
    memcpy(nw->peer_frag,    old->peer_frag,    (size_t)omp*sizeof(uint16_t));
#ifdef DART_SHM
    memcpy(nw->peer_shm,     old->peer_shm,     omp);
#endif
    /* channels: keep the new name-pool slot pointer, carry everything else (incl. the
       heap history ring pointer) and re-copy the name string into the new pool */
    for (c=0;c<onc;c++){
        char *nm = (char*)nw->channels[c].name;
        size_t l = dart__namelen(old->channels[c].name);
        nw->channels[c] = old->channels[c];
        nw->channels[c].name = nm;
        if (l) memcpy(nm, old->channels[c].name, l);
        nm[l] = '\0';
    }
    /* per-(channel,peer) proxies: re-stride into the new max_peers (heap bufs ride along) */
    for (c=0;c<onc;c++) for (p=0;p<omp;p++){
        *dart__writer_proxy_at(nw,c,p) = *dart__writer_proxy_at(old,c,p);
        *dart__reader_proxy_at(nw,c,p) = *dart__reader_proxy_at(old,c,p);
    }
    /* per-peer interest bitmaps (stride grows with n_channels) + alias table */
    for (p=0;p<omp;p++){
        memcpy(nw->peer_pub_bitmap + (size_t)p*nw->bitmap_len,
               old->peer_pub_bitmap + (size_t)p*old->bitmap_len, old->bitmap_len);
        memcpy(nw->peer_sub_bitmap + (size_t)p*nw->bitmap_len,
               old->peer_sub_bitmap + (size_t)p*old->bitmap_len, old->bitmap_len);
        memcpy(nw->peer_sub_reliable + (size_t)p*nw->bitmap_len,
               old->peer_sub_reliable + (size_t)p*old->bitmap_len, old->bitmap_len);
        memcpy(nw->alias_to_channel + (size_t)p*nw->alias_max,
               old->alias_to_channel + (size_t)p*old->alias_max,
               (size_t)old->alias_max*sizeof(uint16_t));
    }
    /* scheduler is fresh/empty: re-enqueue every used lane, then force a full sweep
       next poll so timers re-arm and next_deadline is recomputed exactly */
    for (c=0;c<onc;c++)
        for (p=0;p<omp;p++) if (nw->peer_used[p]) dart__lane_wake(nw, c, p);
    nw->next_deadline_us = 0;
    return nw;
}


int dart_peer_slot(DartTransportState *st, uint32_t id){
    uint16_t i;
    for (i=0;i<st->cfg.max_peers;i++) if (st->peer_used[i] && st->peer_ids[i]==id) return (int)i;
    return -1;
}

/* the local handle IS the channel's index; out-of-range rejected */
i_DartChannel *dart_chan(DartTransportState *st, uint16_t channel, int *idx_out){
    if (channel >= st->cfg.n_channels) return NULL;
    if (idx_out) *idx_out = (int)channel;
    return &st->channels[channel];
}

/* Find the local channel for a wire identity. An INACTIVE channel (declared but off) must
 * not shadow an active same-identity channel, so prefer a non-INACTIVE match; fall back to
 * the first match (e.g. all inactive) so resolution stays deterministic. Lets a caller hold
 * two channels of one identity (different QoS) and switch which is live by role. */
static i_DartChannel *dart_chan_by_identity(DartTransportState *st, uint64_t identity, int *idx_out){
    uint16_t i; int first=-1;
    for (i=0;i<st->cfg.n_channels;i++){
        if (st->channels[i].identity!=identity) continue;
        if (first<0) first=(int)i;
        if (st->channels[i].role!=DART_INACTIVE){ if(idx_out)*idx_out=(int)i; return &st->channels[i]; }
    }
    if (first>=0){ if(idx_out)*idx_out=first; return &st->channels[first]; }
    return NULL;
}


/* fire one DartTransportEvent (no-op if no on_event). Transport emits MSG_LOST/TOO_BIG/
 * COLLISION/QOS. first/count are the kind's two numeric slots; route them to named fields. */
void dart__event(DartTransportState *st, DartTransportEventKind kind, uint16_t channel,
                        uint32_t peer, uint64_t first, uint64_t count, const char *detail){
    DartTransportEvent ev;
    if (!st->cfg.on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind=kind; ev.channel=channel; ev.peer=peer; ev.detail=detail; ev.user=st->cfg.user;
    switch (kind){
    case DART_TRANSPORT_MSG_LOST:       ev.lost_first = first; ev.lost_count = count; break;
    case DART_TRANSPORT_MSG_TOO_BIG:    ev.too_big_bytes = count; break;
    case DART_TRANSPORT_NAME_COLLISION: ev.identity = first; break;
    default: break;
    }
    st->cfg.on_event(&ev);
}

/* dart_event_str (and its bounded appenders) moved to the node (node/core.c): the
   formatter covers the node's app-facing DartEvent union, not the transport's own
   events. The transport stays independent of the node's event vocabulary. */


/* unicast join seqno: head minus qos.catch_up cached samples (reliable only) */
uint64_t dart_unicast_join_seqno(const i_DartChannel *ch){
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


/* match one (channel,peer) proxy: the per-peer lane carries new data, repairs, acks/HB */
static void dart__match_w(DartTransportState *st, uint16_t c, uint16_t peer_slot){
    i_DartChannel *ch=&st->channels[c];
    i_DartWriterProxy *w=dart__writer_proxy_at(st,c,peer_slot);
    memset(w,0,sizeof(*w));
    w->used=1;
    /* only a reader that advertised RELIABLE acks; a best-effort reader stays out of
       flow control so it can't stall this writer (it gets new data, never repairs/HB) */
    w->reader_reliable = dart_bget(&st->peer_sub_reliable[(size_t)peer_slot*st->bitmap_len], c) ? 1u : 0u;
    w->sent_upto = dart_unicast_join_seqno(ch);
    w->acked_upto = w->sent_upto;
    dart__lane_wake(st, c, peer_slot);   /* lane primed for new data + ack/hb */
}

static void dart__unmatch_w(DartTransportState *st, uint16_t c, uint16_t peer_slot){
    i_DartWriterProxy *w=dart__writer_proxy_at(st,c,peer_slot);
    if (!w->used) return;
    w->used=0;
}

static void dart__match_r(DartTransportState *st, uint16_t c, uint16_t peer_slot){
    i_DartReaderProxy *r=dart__reader_proxy_at(st,c,peer_slot);
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

static void dart__unmatch_r(DartTransportState *st, uint16_t c, uint16_t peer_slot){
    i_DartReaderProxy *r=dart__reader_proxy_at(st,c,peer_slot);
    r->used=0; r->assembly_active=0;
}


/* recompute one (channel,peer) match from our role and the peer's interest bits */
static void dart__rematch(DartTransportState *st, uint16_t c, uint16_t peer_slot){
    i_DartChannel *ch=&st->channels[c];
    const uint8_t *peer_pub_bitmap=&st->peer_pub_bitmap[(size_t)peer_slot*st->bitmap_len];
    const uint8_t *peer_sub_bitmap=&st->peer_sub_bitmap[(size_t)peer_slot*st->bitmap_len];
    int wuse = (ch->role==DART_PUBSUB || ch->role==DART_PUB_ONLY) && dart_bget(peer_sub_bitmap,c);
    int ruse = (ch->role==DART_PUBSUB || ch->role==DART_SUB_ONLY) && dart_bget(peer_pub_bitmap,c);
    i_DartWriterProxy *w=dart__writer_proxy_at(st,c,peer_slot);
    i_DartReaderProxy *r=dart__reader_proxy_at(st,c,peer_slot);
    if (wuse && !w->used) dart__match_w(st,c,peer_slot);
    else if (!wuse && w->used) dart__unmatch_w(st,c,peer_slot);
    if (ruse && !r->used) dart__match_r(st,c,peer_slot);
    else if (!ruse && r->used) dart__unmatch_r(st,c,peer_slot);
}


void dart_peer_add(DartTransportState *st, uint32_t id, uint16_t peer_frag){
    uint16_t i; int free=-1; uint32_t max_peers=st->cfg.max_peers;
    if (dart_peer_slot(st,id)>=0) return;
    for (i=0;i<max_peers;i++) if(!st->peer_used[i]){free=(int)i;break;}
    if (free<0) return;
    st->peer_used[free]=1; st->peer_ids[free]=id;
    st->peer_dormant[free]=0;
    st->peer_frag[free]=dart_clamp_frag(peer_frag);
#ifdef DART_SHM
    st->peer_shm[free]=0;   /* node sets it once the peer's segment is attached */
#endif
    memset(&st->peer_pub_bitmap[(size_t)free*st->bitmap_len],0,st->bitmap_len);
    memset(&st->peer_sub_bitmap[(size_t)free*st->bitmap_len],0,st->bitmap_len);
    memset(&st->peer_sub_reliable[(size_t)free*st->bitmap_len],0,st->bitmap_len);
    memset(&st->alias_to_channel[(size_t)free*st->alias_max],0xFF,(size_t)st->alias_max*sizeof(uint16_t));
    /* nothing matches until dart_apply_peer_interest feeds the peer's interest
       list (carried in its discovery announce) */
}


void dart_peer_remove(DartTransportState *st, uint32_t id){
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
void dart_peer_dormant(DartTransportState *st, uint32_t id){
    int s = dart_peer_slot(st,id);
    if (s>=0) st->peer_dormant[s]=1;
}


/* A dormant peer's SAME incarnation returned: re-include it in flow control and
 * re-report each reader position so the writer fills any gap (the reader dedups any
 * replay for free). The writer side needs nothing proactive; the reader's ACKNACK
 * re-arms its heartbeats. Proxies and deliver_upto were never touched, so no dup,
 * no loss. */
void dart_peer_resume(DartTransportState *st, uint32_t id){
    int s = dart_peer_slot(st,id); uint16_t c;
    if (s<0) return;
    st->peer_dormant[s]=0;
    for (c=0;c<st->cfg.n_channels;c++){
        i_DartWriterProxy *w=dart__writer_proxy_at(st,c,s);
        i_DartReaderProxy *r=dart__reader_proxy_at(st,c,s);
        if (st->channels[c].qos.reliability!=DART_RELIABLE) continue;
        if (r->used){ r->ack_pending=1; r->ack_due_us=0; r->ack_force=1; }  /* report our position now */
        if (w->used || r->used) dart__lane_wake(st,c,(uint16_t)s);
    }
}


/* update a peer's advertised fragment size (its blob may arrive after first contact) */
void dart_peer_set_frag(DartTransportState *st, uint32_t id, uint16_t peer_frag){
    int s = dart_peer_slot(st,id);
    if (s>=0) st->peer_frag[s]=dart_clamp_frag(peer_frag);
}

#ifdef DART_SHM

void dart_peer_set_shm(DartTransportState *st, uint32_t id, int is_shm){
    int s = dart_peer_slot(st,id);
    if (s>=0) st->peer_shm[s]=(uint8_t)(is_shm?1:0);
}
#endif


void dart_destroy(DartTransportState *st){
    uint16_t c; uint32_t p, max_peers;
    if (!st || !st->cfg.allocator) return;     /* fixed mode: nothing hook-allocated */
    max_peers = st->cfg.max_peers;
    for (c=0;c<st->cfg.n_channels;c++){
        i_DartChannel *ch=&st->channels[c];
        uint16_t depth, d;
        if (!ch->dynamic) continue;
        depth = ch->qos.keep_last;
        for (d=0; d<depth; d++)
            if (ch->history[d].buf){ st->cfg.allocator(st->cfg.user, ch->history[d].buf, 0);
                                  ch->history[d].buf=NULL; ch->history[d].cap=0; }
        for (p=0;p<max_peers;p++){
            i_DartReaderProxy *r=dart__reader_proxy_at(st,c,p);
            if (r->assembly_buf){ st->cfg.allocator(st->cfg.user, r->assembly_buf, 0); r->assembly_buf=NULL; r->assembly_cap=0; }
            if (r->frag_bitmap){ st->cfg.allocator(st->cfg.user, r->frag_bitmap, 0); r->frag_bitmap=NULL; r->bitmap_cap=0; }
        }
        if (ch->history_owned && ch->history){   /* ring allocated by dart_channel_define */
            st->cfg.allocator(st->cfg.user, ch->history, 0);
            ch->history=NULL; ch->history_owned=0;
        }
    }
}


/* per-entry flags byte (interest is sent rarely, so a whole byte, not a stolen bit) */
#define DART_META_F_RELIABLE 0x01u   /* advertiser offers reliable delivery on this topic */

/* one interest entry: [u16 alias][u8 flags][u8 namelen][name]. The name rides along so
 * a hash collision is detected (not cross-wired); the identity is recomputed from it. */
static uint8_t *dart__meta_put(uint8_t *p, uint16_t alias, const i_DartChannel *ch){
    size_t lane = dart__namelen(ch->name);
    dart_le_w16(p, alias); p += 2;
    *p++ = (uint8_t)(ch->qos.reliability==DART_RELIABLE ? DART_META_F_RELIABLE : 0u);
    *p++ = (uint8_t)lane;
    if (lane){ memcpy(p, ch->name, lane); p += lane; }
    return p;
}

static int dart__meta_name_eq(const i_DartChannel *ch, const uint8_t *name, size_t nlen){
    size_t ours = dart__namelen(ch->name);
    if (nlen != ours) return 0;
    return nlen==0 ? 1 : (memcmp(ch->name, name, nlen)==0);
}

/* match count entries to local channels by identity (recomputed from each name),
 * recording the alias map. Same-identity-different-name is a collision: refused.
 * is_pub: the peer's publish list, so each entry's flags carry its OFFERED QoS, which
 * the RxO check uses to refuse a reliable subscriber a best-effort publisher. rel_bitmap
 * (sub list only, else NULL): records which subscribed channels the peer requested RELIABLE,
 * so the writer can keep best-effort readers out of flow control. */
static const uint8_t *dart__meta_scan(DartTransportState *st, int peer_slot, const uint8_t *p,
                                      uint32_t count, uint8_t *bitmap, int is_pub, uint8_t *rel_bitmap){
    uint32_t k;
    for (k=0;k<count;k++){
        uint16_t alias=dart_le_r16(p); uint8_t flags=p[2]; uint32_t nlen=p[3];
        const uint8_t *name=p+4; int channel_idx;
        uint64_t id=dart__id_n(name,nlen);
        i_DartChannel *ch=dart_chan_by_identity(st,id,&channel_idx);
        p = name + nlen;
        if (!ch) continue;                                  /* not ours */
        if (!dart__meta_name_eq(ch,name,nlen)){
            dart__event(st, DART_TRANSPORT_NAME_COLLISION, (uint16_t)channel_idx, st->peer_ids[peer_slot],
                        id, 0, ch->name ? ch->name : "");
            continue;
        }
        /* RxO QoS: a reliable subscriber refuses a best-effort publisher (no silent
           downgrade). We keep requesting reliable, so the match forms automatically if
           the publisher later upgrades and re-advertises. */
        if (is_pub && (ch->role==DART_PUBSUB || ch->role==DART_SUB_ONLY) &&
            ch->qos.reliability==DART_RELIABLE && !(flags & DART_META_F_RELIABLE)){
            dart__event(st, DART_TRANSPORT_QOS_INCOMPATIBLE, (uint16_t)channel_idx,
                        st->peer_ids[peer_slot], 0, 0, ch->name ? ch->name : "");
            continue;                                       /* refuse: no bit, no alias map */
        }
        dart_bset(bitmap,(uint32_t)channel_idx);
        if (rel_bitmap && (flags & DART_META_F_RELIABLE)) dart_bset(rel_bitmap,(uint32_t)channel_idx);
        if ((uint32_t)alias < st->alias_max)
            st->alias_to_channel[(size_t)peer_slot*st->alias_max + alias] = (uint16_t)channel_idx;
    }
    return p;
}


/* Upper bound on dart_build_interest output, for sizing the announce buffer: a
 * PUBSUB channel appears in both lists, so 2*n_channels max-length entries. */
size_t dart_interest_max(uint16_t n_channels){
    return 4u + (size_t)(2u+1u+1u+DART_TOPIC_NAME_MAX) * 2u * (size_t)n_channels;  /* alias+flags+namelen+name */
}


/* Serialize our interest into out: [u16 npub][u16 nsub][pub..][sub..], each entry
 * [u16 alias][u8 namelen][name]. Returns bytes written, or 0 if cap is too small.
 * The node carries this in its discovery announce; size out via dart_interest_max. */
size_t dart_build_interest(DartTransportState *st, void *out, size_t cap){
    uint8_t *o=(uint8_t*)out, *p, *end=o+cap;
    uint16_t c; uint32_t n_pub=0, n_sub=0;
    if (cap < 4) return 0;
    p=o+4;
    for (c=0;c<st->cfg.n_channels;c++){
        uint8_t d=st->channels[c].role;
        if (d==DART_PUBSUB || d==DART_PUB_ONLY){
            if (p + 4u + dart__namelen(st->channels[c].name) > end) return 0;
            p=dart__meta_put(p,c,&st->channels[c]); n_pub++;
        }
    }
    for (c=0;c<st->cfg.n_channels;c++){
        uint8_t d=st->channels[c].role;
        if (d==DART_PUBSUB || d==DART_SUB_ONLY){
            if (p + 4u + dart__namelen(st->channels[c].name) > end) return 0;
            p=dart__meta_put(p,c,&st->channels[c]); n_sub++;
        }
    }
    dart_le_w16(o,(uint16_t)n_pub); dart_le_w16(o+2,(uint16_t)n_sub);
    return (size_t)(p - o);
}


/* A peer's interest list arrived (from its discovery announce): refresh its bits
 * and rematch every channel. Idempotent; re-applying re-derives all matches. */
void dart_apply_peer_interest(DartTransportState *st, uint32_t peer_id, const void *blob, size_t len){
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
          if (p+4 > end) return;
          p += 4u + (uint32_t)p[3];
          if (p > end) return;
      } }
    memset(peer_pub_bitmap,0,st->bitmap_len); memset(peer_sub_bitmap,0,st->bitmap_len);
    memset(&st->peer_sub_reliable[(size_t)peer_slot*st->bitmap_len],0,st->bitmap_len);
    memset(&st->alias_to_channel[(size_t)peer_slot*st->alias_max],0xFF,(size_t)st->alias_max*sizeof(uint16_t));
    p = dart__meta_scan(st, peer_slot, d+4, n_pub, peer_pub_bitmap, 1, NULL);   /* pub list: offered QoS */
    p = dart__meta_scan(st, peer_slot, p,   n_sub, peer_sub_bitmap, 0,         /* sub list: requested QoS */
                        &st->peer_sub_reliable[(size_t)peer_slot*st->bitmap_len]);
    for (c=0;c<st->cfg.n_channels;c++) dart__rematch(st,c,(uint16_t)peer_slot);
}


/* diagnostic: how many channels we now publish to / receive from this peer (unicast
 * lanes). Surfaced on DART_PEER_INTEREST so a caller can see a match form (or not). */
void dart_peer_match_counts(DartTransportState *st, uint32_t peer_id,
                            uint16_t *publish_to, uint16_t *receive_from){
    int s; uint16_t c, w=0, r=0;
    if (publish_to)   *publish_to   = 0;
    if (receive_from) *receive_from = 0;
    if (!st) return;
    s = dart_peer_slot(st, peer_id);
    if (s < 0) return;
    for (c=0;c<st->cfg.n_channels;c++){
        if (dart__writer_proxy_at(st,c,(uint32_t)s)->used) w++;
        if (dart__reader_proxy_at(st,c,(uint32_t)s)->used) r++;
    }
    if (publish_to)   *publish_to   = w;
    if (receive_from) *receive_from = r;
}


/* Discovery-announce meta blob codec (see dart_meta_* in core.h for the layout). The
   interest list is wrapped in a prefix carrying frag size, (odd ver) SHM info, and the
   node name. No back-compat: the version byte just tags the one current format, and a
   blob whose magic/version we don't expect is rejected, not reinterpreted. An interest
   entry is [u16 alias][u8 flags][u8 namelen][name]; flags bit 0 = offered reliability.
   Parsing is fully bounds-checked (see dart_apply_peer_interest), so a malformed or
   foreign blob is dropped wholesale, never trusted. */
#define DART__META_BASE_NOSHM 5u    /* 'D','N',ver, frag_lo, frag_hi */
#define DART__META_BASE_SHM   22u   /* ... + shm(1) + host[16] */
#ifdef DART_SHM
#define DART__META_VER  7u                  /* what WE write */
#define DART__META_BASE DART__META_BASE_SHM
#else
#define DART__META_VER  6u
#define DART__META_BASE DART__META_BASE_NOSHM
#endif

static int dart__meta_ok(const uint8_t *meta, uint16_t meta_len){
    return meta && meta_len >= 5 && meta[0]=='D' && meta[1]=='N' && meta[2]>=6 && meta[2]<=7;
}
/* base prefix through host[16], by version (odd v7 carries shm+host, even v6 doesn't). */
static uint16_t dart__meta_base(const uint8_t *meta){
    return (meta[2]==7) ? DART__META_BASE_SHM : DART__META_BASE_NOSHM;
}
uint16_t dart_meta_capacity(uint16_t n_channels){
    size_t cap = (size_t)DART__META_BASE + dart_interest_max(n_channels);  /* overlay: no name (it's discovery's) */
    if (cap > 65000u) cap = 65000u;
    return (uint16_t)cap;
}

uint16_t dart_meta_build(DartTransportState *st, uint8_t *out, uint16_t cap,
                         uint16_t frag_size, int shm_capable, const uint8_t host[16]){
    size_t interest_len; uint16_t off = DART__META_BASE;
    out[0]='D'; out[1]='N'; out[2]=DART__META_VER;
    out[3]=(uint8_t)(frag_size & 0xFF); out[4]=(uint8_t)(frag_size >> 8);
#ifdef DART_SHM
    out[5]=(uint8_t)(shm_capable?1:0);
    if (host) memcpy(out+6, host, 16); else memset(out+6, 0, 16);
#else
    (void)shm_capable; (void)host;
#endif
    interest_len = dart_build_interest(st, out + off, cap - off);   /* no name here: that is discovery's */
    return (uint16_t)(off + interest_len);
}

uint16_t dart_meta_frag(const uint8_t *meta, uint16_t meta_len){
    if (!dart__meta_ok(meta, meta_len)) return 0;
    return (uint16_t)(meta[3] | ((uint16_t)meta[4] << 8));
}

const uint8_t *dart_meta_interest(const uint8_t *meta, uint16_t meta_len, size_t *out_len){
    uint16_t off;
    *out_len = 0;
    if (!dart__meta_ok(meta, meta_len)) return NULL;
    off = dart__meta_base(meta);          /* interest follows the base prefix (no name in the overlay) */
    if (meta_len < off) return NULL;
    *out_len = (size_t)(meta_len - off);
    return meta + off;
}

int dart_meta_interest_next(const uint8_t *meta, uint16_t meta_len,
                            DartInterestIter *it, DartTopic *out){
    uint32_t off; uint8_t nlen;
    if (!it || !out) return 0;
    if (!it->started){                    /* first call: parse the [npub][nsub] header */
        size_t il = 0;
        const uint8_t *in = dart_meta_interest(meta, meta_len, &il);
        it->started = 1; it->pub_left = it->sub_left = 0; it->off = 0;
        if (!in || il < 4) return 0;      /* no/short interest list: nothing to yield */
        it->pub_left = (uint16_t)(in[0] | ((uint16_t)in[1] << 8));
        it->sub_left = (uint16_t)(in[2] | ((uint16_t)in[3] << 8));
        it->off = (uint32_t)(in - meta) + 4u;   /* first entry, past npub/nsub */
    }
    if (it->pub_left == 0 && it->sub_left == 0) return 0;
    off = it->off;
    if (off + 4u > meta_len){ it->pub_left = it->sub_left = 0; return 0; }   /* truncated: stop */
    nlen = meta[off + 3];
    if (off + 4u + nlen > meta_len){ it->pub_left = it->sub_left = 0; return 0; }
    out->alias    = (uint16_t)(meta[off] | ((uint16_t)meta[off + 1] << 8));
    out->reliable = (uint8_t)(meta[off + 2] & DART_META_F_RELIABLE);
    out->is_pub   = (uint8_t)(it->pub_left > 0);   /* pub list first, then sub */
    out->name     = (const char *)(meta + off + 4u);
    out->name_len = nlen;
    it->off = off + 4u + nlen;
    if (it->pub_left > 0) it->pub_left--; else it->sub_left--;
    return 1;
}

#ifdef DART_SHM
int dart_meta_shm(const uint8_t *meta, uint16_t meta_len, uint8_t host[16]){
    if (!dart__meta_ok(meta, meta_len) || meta[2]!=7
        || meta_len < DART__META_BASE_SHM || !meta[5]) return 0;
    memcpy(host, meta+6, 16);
    return 1;
}
#endif


int dart_set_role(DartTransportState *st, uint16_t channel, uint8_t role){
    int channel_idx; i_DartChannel *ch; uint16_t p;
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


int dart_channel_define(DartTransportState *st, uint16_t channel, const DartChannelDef *def){
    i_DartChannel *ch; DartQos q; uint16_t depth, p; size_t lane;
    if (!st || !def || !st->cfg.allocator) return -1;       /* dynamic (reserve) mode only */
    if (channel >= st->cfg.n_channels) return -1;            /* out of reserved range */
    if (!def->name || !def->name[0]) return -1;             /* name = identity, required */
    lane = dart__namelen(def->name);
    if (def->name[lane]) return -1;                          /* longer than DART_TOPIC_NAME_MAX */
    ch = &st->channels[channel];
    if (ch->identity != 0 || ch->history) return -1;        /* slot already defined */
    q = def->qos; dart__qos_defaults(&q, 1);                 /* dynamic: grow-to-fit buffers */
    depth = q.keep_last;
    ch->history = (i_DartWriterSample*)st->cfg.allocator(st->cfg.user, NULL,
                                                         (size_t)depth*sizeof(i_DartWriterSample));
    if (!ch->history) return -4;                            /* OOM */
    memset(ch->history, 0, (size_t)depth*sizeof(i_DartWriterSample));
    ch->history_owned = 1; ch->dynamic = 1;
    ch->qos = q; ch->max_fragments = dart_max_fragments(q.max_message_bytes);
    ch->role = def->role;
    ch->identity = dart_channel_identity(def);
    memcpy((char*)ch->name, def->name, lane); ((char*)ch->name)[lane] = '\0';
    ch->history_head = 0; ch->next_seqno = 0; ch->have_first = 0;
    for (p=0;p<st->cfg.max_peers;p++)        /* match the newly active channel to known peers */
        if (st->peer_used[p]) dart__rematch(st, channel, p);
    return 0;
}


const char *dart_channel_name(DartTransportState *st, uint16_t channel, uint8_t *len){
    i_DartChannel *ch = dart_chan(st, channel, NULL);
    if (!ch || !ch->name || !ch->name[0]){ if (len) *len = 0; return NULL; }
    if (len) *len = (uint8_t)dart__namelen(ch->name);
    return ch->name;
}


const DartQos *dart_channel_qos(DartTransportState *st, uint16_t channel){
    i_DartChannel *ch = dart_chan(st, channel, NULL);
    return ch ? &ch->qos : NULL;
}


void dart_repair_stats(DartTransportState *st, uint16_t channel, DartRepairStats *out){
    i_DartChannel *ch = dart_chan(st, channel, NULL);
    if (!out) return;
    if (ch) *out = ch->repair_stats;
    else memset(out, 0, sizeof *out);
}


void dart_on_datagram(DartTransportState *st, uint32_t from, const void *datagram, size_t len, uint64_t now){
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
