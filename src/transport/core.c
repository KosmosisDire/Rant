/* sans-IO reliable-UDP transport core: state, init/teardown, peer + interest matching,
 * the RX demux, and public queries. The wire codec, scheduler, and writer/reader paths
 * live in transport/{wire,sched,writer,reader}.c; shared decls in transport/internal.h. */
#include "core.h"
#include "../common/bytes.h"
#include "../common/arena.h"
#include "../common/hash.h"
#include "internal.h"
#include <string.h>


/* Topic identity = FNV-1a 64 of the name (i_dart_fnv1a64, common/hash.h). The interest blob
 * carries length-prefixed (not NUL-term) names, so the identity is recomputed from the
 * name on receive; init caps names at DART_TOPIC_NAME_MAX so the wire name is the whole
 * name and i_dart_identity_hash (over the wire bytes) == dart_topic_id of the same name. */
static uint64_t i_dart_identity_hash(const uint8_t *name, size_t n){ return i_dart_fnv1a64(name, n); }

uint64_t dart_topic_id(const char *name){ return i_dart_fnv1a64_str(name); }

uint64_t dart_channel_identity(const DartChannelDef *def){
    return dart_topic_id(def->name);   /* the name is the cross-peer identity */
}

static size_t i_dart_name_len(const char *s){            /* capped strlen */
    size_t n = 0;
    if (s) while (s[n] && n < DART_TOPIC_NAME_MAX) n++;
    return n;
}


/* Reader-side fragment-count bound: a peer may fragment at the smallest size in
 * the deployment, so size the reassembly bitmap by DART_FRAG_PAYLOAD_MIN. */
static uint16_t i_dart_max_fragments(uint32_t max_message_bytes){
    uint32_t f = (max_message_bytes + DART_FRAG_PAYLOAD_MIN - 1) / DART_FRAG_PAYLOAD_MIN;
    if (f == 0) f = 1;
    return (uint16_t)f;
}


/* zero-means-default for the tunable QoS fields, applied once at init so the
 * stored qos is authoritative */
static void i_dart_qos_defaults(DartQos *q, int dynamic){
    if (q->keep_last == 0)        q->keep_last       = DART_QOS_DEF_KEEP_LAST;
    if (q->heartbeat_us == 0)     q->heartbeat_us    = DART_QOS_DEF_HEARTBEAT_US;
    if (q->repair_delay_us == 0)  q->repair_delay_us = DART_QOS_DEF_REPAIR_US;
    /* fixed mode only: a dynamic channel keeps 0 = grow-to-fit via allocator */
    if (!dynamic && q->max_message_bytes == 0) q->max_message_bytes = DART_FRAG_PAYLOAD;
}


/* Normalize a node/peer UDP fragment size: 0 -> default, then clamp to [MIN,MAX].
   Public so the transport (dart_transport_init) and the node (announce blob) clamp identically. */
uint16_t dart_clamp_frag(uint16_t frag_payload){
    uint16_t f = frag_payload ? frag_payload : DART_FRAG_PAYLOAD;
    if (f < DART_FRAG_PAYLOAD_MIN) f = DART_FRAG_PAYLOAD_MIN;
    if (f > DART_FRAG_PAYLOAD_MAX) f = DART_FRAG_PAYLOAD_MAX;
    return f;
}


/* lay out everything (b->base==NULL = measure only) */
static DartTransportState *i_dart_transport_build(i_DartBump *b, const DartConfig *cfg){
    uint16_t c, p; uint32_t max_peers = cfg->max_peers, n_channels = cfg->n_channels;
    uint16_t bitmap_len = (uint16_t)((n_channels+7u)/8u);
    uint32_t meta_ids = DART_META_MAX_IDS;
    uint32_t name_bytes = 0; char *name_pool = NULL;
    DartTransportState *st = (DartTransportState*)i_dart_bump_take(b, sizeof(DartTransportState), 16);
    if (st && b->base) memset(st, 0, sizeof(*st));

    /* name pool: one fixed-size slot per channel so a reserve-mode slot can be named
       later by dart_transport_channel_define without repacking. ch->name points at its slot. */
    name_bytes = (uint32_t)n_channels * (DART_TOPIC_NAME_MAX + 1u);
    if (meta_ids < 2u*n_channels) meta_ids = 2u*n_channels;     /* our own interest list must always fit */

    { uint32_t nlanes = n_channels*max_peers, ndest = max_peers;
      int dyn = (cfg->allocator != NULL);
      uint32_t *peer_ids = (uint32_t*)i_dart_bump_take(b, max_peers*sizeof(uint32_t), 8);
      uint8_t  *peer_used = (uint8_t*) i_dart_bump_take(b, max_peers*sizeof(uint8_t), 1);
      uint8_t  *peer_dormant= (uint8_t*) i_dart_bump_take(b, max_peers*sizeof(uint8_t), 1);
      uint16_t *peer_frag = (uint16_t*)i_dart_bump_take(b, max_peers*sizeof(uint16_t), 2);
#ifdef DART_SHM
      uint8_t  *peer_shm= (uint8_t*) i_dart_bump_take(b, max_peers*sizeof(uint8_t), 1);
#endif
      uint8_t  *peer_pub_bitmap = (uint8_t*) i_dart_bump_take(b, (size_t)max_peers*bitmap_len, 1);
      uint8_t  *peer_sub_bitmap = (uint8_t*) i_dart_bump_take(b, (size_t)max_peers*bitmap_len, 1);
      uint8_t  *peer_sub_reliable = (uint8_t*) i_dart_bump_take(b, (size_t)max_peers*bitmap_len, 1);
      i_DartChannel *ch = (i_DartChannel*)i_dart_bump_take(b, n_channels*sizeof(i_DartChannel), 16);
      /* lanes: dynamic keeps only the u16 ticket table (records are pool-allocated per real
         match, so memory scales with matches); fixed embeds the dense record array, whose
         reassembly buffers are pre-bound per channel below */
      uint16_t   *lane_index = dyn ? (uint16_t*)i_dart_bump_take(b, (size_t)nlanes*sizeof(uint16_t), 2) : NULL;
      i_DartLane *lanes      = dyn ? NULL : (i_DartLane*)i_dart_bump_take(b, (size_t)nlanes*sizeof(i_DartLane), 16);
      uint32_t *dest_head = (uint32_t*)i_dart_bump_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint32_t *dest_tail = (uint32_t*)i_dart_bump_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint8_t  *dest_queued = (uint8_t*) i_dart_bump_take(b, (size_t)ndest, 1);
      uint32_t *dest_queue = (uint32_t*)i_dart_bump_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      /* alias maps: per-peer pointer + length; dynamic allocates each map on demand at the
         peer's advertised size, fixed pre-slices a dense meta_ids-stride pool (as before) */
      uint16_t **peer_alias    = (uint16_t**)i_dart_bump_take(b, (size_t)max_peers*sizeof(uint16_t*), 8);
      uint32_t *peer_alias_len = (uint32_t*) i_dart_bump_take(b, (size_t)max_peers*sizeof(uint32_t), 8);
      uint8_t **peer_astate    = (uint8_t**) i_dart_bump_take(b, (size_t)max_peers*sizeof(uint8_t*), 8);
      uint16_t *alias_pool     = dyn ? NULL
                               : (uint16_t*)i_dart_bump_take(b, (size_t)max_peers*meta_ids*sizeof(uint16_t), 2);
      uint8_t  *astate_pool    = dyn ? NULL
                               : (uint8_t*) i_dart_bump_take(b, (size_t)max_peers*meta_ids, 1);
      name_pool = (char*)i_dart_bump_take(b, name_bytes ? name_bytes : 1u, 1);
      if (st && b->base){
          st->cfg=*cfg; st->peer_ids=peer_ids; st->peer_used=peer_used;
          st->peer_dormant=peer_dormant; st->peer_frag=peer_frag;
          st->frag = dart_clamp_frag(cfg->frag_payload);
          st->peer_pub_bitmap=peer_pub_bitmap; st->peer_sub_bitmap=peer_sub_bitmap;
          st->peer_sub_reliable=peer_sub_reliable; st->bitmap_len=bitmap_len;
          st->channels=ch; st->reader_epoch_counter=1;
          st->next_deadline_us=DART__NO_DEADLINE;
          st->lanes=lanes; st->lane_cap = dyn ? 0u : nlanes; st->lane_free=DART__NIL;
          st->lane_index=lane_index;
          st->dest_head=dest_head; st->dest_tail=dest_tail; st->dest_queued=dest_queued; st->dest_queue=dest_queue;
          st->peer_alias=peer_alias; st->peer_alias_len=peer_alias_len; st->alias_max=meta_ids;
          st->peer_astate=peer_astate;
          memset(peer_used,0,max_peers); memset(peer_dormant,0,max_peers);
          { uint32_t k; for (k=0;k<max_peers;k++) peer_frag[k]=DART_FRAG_PAYLOAD; }  /* set per peer on add */
#ifdef DART_SHM
          st->peer_shm=peer_shm; memset(peer_shm,0,max_peers);
#endif
          if (dyn){
              memset(peer_alias, 0, (size_t)max_peers*sizeof(uint16_t*));
              memset(peer_alias_len, 0, (size_t)max_peers*sizeof(uint32_t));
              memset(peer_astate, 0, (size_t)max_peers*sizeof(uint8_t*));
          } else {
              uint32_t k;
              memset(alias_pool,0xFF,(size_t)max_peers*meta_ids*sizeof(uint16_t));   /* all unmapped */
              memset(astate_pool,0,(size_t)max_peers*meta_ids);                      /* no verdicts */
              for (k=0;k<max_peers;k++){
                  peer_alias[k] = alias_pool + (size_t)k*meta_ids;
                  peer_alias_len[k] = meta_ids;
                  peer_astate[k] = astate_pool + (size_t)k*meta_ids;
              }
          }
          memset(peer_pub_bitmap,0,(size_t)max_peers*bitmap_len); memset(peer_sub_bitmap,0,(size_t)max_peers*bitmap_len);
          memset(peer_sub_reliable,0,(size_t)max_peers*bitmap_len);
          if (dyn) memset(lane_index,0xFF,(size_t)nlanes*sizeof(uint16_t));   /* all unmatched */
          else {
              uint32_t li;
              memset(lanes,0,(size_t)nlanes*sizeof(i_DartLane));
              for (li=0;li<nlanes;li++){          /* fixed: identity records, permanent */
                  lanes[li].channel   = (uint16_t)(li / max_peers);
                  lanes[li].peer_slot = (uint16_t)(li % max_peers);
                  lanes[li].sched_next = DART__NIL; lanes[li].ch_next = DART__NIL;
                  lanes[li].in_use = 1;
              }
          }
          memset(dest_queued,0,ndest);
          memset(dest_head,0xFF,(size_t)ndest*sizeof(uint32_t));   /* all DART__NIL */
      }
    }

    for (c=0;c<n_channels;c++){
        /* every slot starts inactive with its own name-pool slot; reserve-mode slots
           stay this way until dart_transport_channel_define fills them. */
        if (st && b->base){
            i_DartChannel *ch = &st->channels[c];
            memset(ch,0,sizeof(*ch));
            ch->role = DART_INACTIVE;
            ch->lane_head = DART__NIL;
            ch->name = name_pool + (size_t)c*(DART_TOPIC_NAME_MAX + 1u);
            ((char*)ch->name)[0] = '\0';
        }
        if (!cfg->channels) continue;    /* reserve mode: arena holds no per-channel buffers */
        {   const DartChannelDef *def = &cfg->channels[c];
            /* dynamic = an allocator is set: buffers grow via the hook, not the arena */
            int dyn = (cfg->allocator != NULL);
            DartQos q = def->qos;            /* local, normalized copy */
            i_DartWriterSample *history; uint16_t depth, max_fragments, d;
            i_dart_qos_defaults(&q, dyn);
            depth = q.keep_last;
            max_fragments = i_dart_max_fragments(q.max_message_bytes);
            history = (i_DartWriterSample*)i_dart_bump_take(b, depth*sizeof(i_DartWriterSample), 16);
            if (st && b->base){
                i_DartChannel *ch = &st->channels[c];
                size_t lane = i_dart_name_len(def->name);
                ch->qos=q; ch->max_fragments=max_fragments;
                ch->role=def->role; ch->dynamic=(uint8_t)dyn;
                ch->identity = dart_channel_identity(def);
                if (lane){ memcpy((char*)ch->name, def->name, lane); ((char*)ch->name)[lane]='\0'; }
                ch->name_len = (uint8_t)lane;
                ch->history=history; ch->history_owned=0; ch->history_head=0; ch->next_seqno=0; ch->have_first=0;
                memset(history,0,depth*sizeof(i_DartWriterSample));
            }
            for (d=0; d<depth; d++){
                uint8_t *buf = dyn ? NULL : (uint8_t*)i_dart_bump_take(b, q.max_message_bytes, 8);
                if (st && b->base){ st->channels[c].history[d].buf = buf;
                                    st->channels[c].history[d].cap = dyn ? 0u : q.max_message_bytes; }
            }
            /* reader asm buffers + frag bitmaps, per peer. Fixed mode only: it binds into
               the permanent identity records; a dynamic record starts empty and grows via
               the hook (and does not exist yet here). */
            if (!dyn) for (p=0;p<max_peers;p++){
                uint8_t *assembly_buf = (uint8_t*)i_dart_bump_take(b, q.max_message_bytes, 8);
                uint8_t *frag_bitmap  = (uint8_t*)i_dart_bump_take(b, (max_fragments+7u)/8u, 1);
                if (st && b->base){
                    i_DartReaderProxy *r = i_dart_reader_proxy_at(st,c,p);
                    r->assembly_buf=assembly_buf; r->frag_bitmap=frag_bitmap;
                    r->assembly_cap = q.max_message_bytes;
                    r->bitmap_cap  = (uint32_t)((max_fragments+7u)/8u);
                }
            }
        }
    }
    return st;
}


size_t dart_transport_required_memory(const DartConfig *cfg){
    i_DartBump b; memset(&b,0,sizeof b);
    if (!cfg || cfg->n_channels==0 || cfg->max_peers==0) return 0;
    i_dart_transport_build(&b, cfg);
    return b.offset + 16;   /* slack for base alignment */
}


DartTransportState *dart_transport_init(void *mem, size_t cap, const DartConfig *cfg){
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
    st = i_dart_transport_build(&b, cfg);
    if (!st || b.oom) return NULL;
    st->cfg.channels = NULL;   /* only read during init; detach the caller's pointer */
    return st;
}


/* Relocate a live transport into a bigger block at grown counts (dynamic-mode growth).
 * Heap buffers (history rings, sample/assembly bufs, frag bitmaps) are NOT in the arena,
 * so the struct copies carry their pointers across and the OLD arena can be freed without
 * touching them. The 2D tables are re-strided into the new max_peers/n_channels; the
 * active-lane scheduler (indices encode the old strides) is dropped and rebuilt from the
 * proxy state. The caller frees old's arena block afterward; it must NOT dart_transport_destroy old
 * (that would free the heap buffers now owned by the new state). Returns the new state. */
DartTransportState *dart_transport_migrate(DartTransportState *old, void *new_mem, size_t new_cap,
                        uint16_t new_max_peers, uint16_t new_n_channels){
    DartConfig nc; DartTransportState *nw; uint16_t omp, onc, c, p;
    if (!old) return NULL;
    nc = old->cfg; nc.channels = NULL;
    nc.max_peers = new_max_peers; nc.n_channels = new_n_channels;
    nw = dart_transport_init(new_mem, new_cap, &nc);
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
        size_t l = old->channels[c].name_len;
        nw->channels[c] = old->channels[c];   /* struct copy carries name_len */
        nw->channels[c].name = nm;
        if (l) memcpy(nm, old->channels[c].name, l);
        nm[l] = '\0';
    }
    /* lane records: the pool is ONE hook allocation outside both arenas, so adopt it
       wholesale (record backrefs use channel indices + peer slots, both preserved; the
       channels' lane_head chains were carried by the struct copies above). Only the
       ticket table is arena memory: re-stride it into the new max_peers. */
    nw->lanes = old->lanes; nw->lane_cap = old->lane_cap; nw->lane_free = old->lane_free;
    for (c=0;c<onc;c++) for (p=0;p<omp;p++)
        nw->lane_index[(size_t)c*new_max_peers+p] = old->lane_index[(size_t)c*omp+p];
    {   uint32_t li;   /* the old scheduler dies with the old arena: clear per-record state
                          (free-list records keep sched_next: it is their free link) */
        for (li=0; li<nw->lane_cap; li++)
            if (nw->lanes[li].in_use){ nw->lanes[li].queued=0; nw->lanes[li].sched_next=DART__NIL; }
    }
    /* per-peer interest bitmaps (stride grows with n_channels) + alias table */
    for (p=0;p<omp;p++){
        memcpy(nw->peer_pub_bitmap + (size_t)p*nw->bitmap_len,
               old->peer_pub_bitmap + (size_t)p*old->bitmap_len, old->bitmap_len);
        memcpy(nw->peer_sub_bitmap + (size_t)p*nw->bitmap_len,
               old->peer_sub_bitmap + (size_t)p*old->bitmap_len, old->bitmap_len);
        memcpy(nw->peer_sub_reliable + (size_t)p*nw->bitmap_len,
               old->peer_sub_reliable + (size_t)p*old->bitmap_len, old->bitmap_len);
        nw->peer_alias[p]     = old->peer_alias[p];   /* hook allocations: stable across the move */
        nw->peer_alias_len[p] = old->peer_alias_len[p];
        nw->peer_astate[p]    = old->peer_astate[p];
    }
    /* scheduler is fresh/empty: re-enqueue every used lane, then force a full sweep
       next poll so timers re-arm and next_deadline is recomputed exactly */
    for (c=0;c<onc;c++)
        for (p=0;p<omp;p++) if (nw->peer_used[p]) i_dart_lane_wake(nw, c, p);
    nw->next_deadline_us = 0;
    return nw;
}


int i_dart_peer_slot(DartTransportState *st, uint32_t id){
    uint16_t i;
    for (i=0;i<st->cfg.max_peers;i++) if (st->peer_used[i] && st->peer_ids[i]==id) return (int)i;
    return -1;
}

/* the local handle IS the channel's index; out-of-range rejected */
i_DartChannel *i_dart_channel_at(DartTransportState *st, uint16_t channel, int *idx_out){
    if (channel >= st->cfg.n_channels) return NULL;
    if (idx_out) *idx_out = (int)channel;
    return &st->channels[channel];
}

/* Find the local channel for a wire identity. An INACTIVE channel (declared but off) must
 * not shadow an active same-identity channel, so prefer a non-INACTIVE match; fall back to
 * the first match (e.g. all inactive) so resolution stays deterministic. Lets a caller hold
 * two channels of one identity (different QoS) and switch which is live by role. */
static i_DartChannel *i_dart_channel_by_identity(DartTransportState *st, uint64_t identity, int *idx_out){
    uint16_t i; int first=-1;
    for (i=0;i<st->cfg.n_channels;i++){
        if (st->channels[i].identity!=identity) continue;
        if (first<0) first=(int)i;
        if (st->channels[i].role!=DART_INACTIVE){ if(idx_out)*idx_out=(int)i; return &st->channels[i]; }
    }
    if (first>=0){ if(idx_out)*idx_out=first; return &st->channels[first]; }
    return NULL;
}


/* fire one DartTransportEvent (no-op if no on_event). first/count are the kind's two
 * numeric slots; route them to named fields. A channel name is not carried: a consumer
 * reads it with dart_transport_channel_name(st, ev.channel). */
void i_dart_transport_fire_event(DartTransportState *st, DartTransportEventKind kind, uint16_t channel,
                        uint32_t peer, uint64_t first, uint64_t count){
    DartTransportEvent ev;
    if (!st->cfg.on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind=kind; ev.channel=channel; ev.peer=peer; ev.user=st->cfg.user;
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
uint64_t i_dart_channel_unicast_join_seqno(const i_DartChannel *ch){
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


/* Get-or-allocate the lane record for (c,peer_slot). Dynamic mode grows the pool by
 * doubling through the hook; records MOVE on growth, so callers re-derive any lane
 * pointer after this call. Returns NULL on OOM or a full u16 ticket space: the match is
 * refused for now (the peer's next announce re-applies and retries). Fixed mode always
 * succeeds (the identity record is permanent). */
static i_DartLane *i_dart_lane_ensure(DartTransportState *st, uint16_t c, uint32_t peer_slot){
    size_t k = (size_t)c*st->cfg.max_peers + peer_slot;
    uint32_t li;
    if (!st->lane_index) return &st->lanes[k];        /* fixed: identity */
    li = st->lane_index[k]==0xFFFFu ? DART__NIL : (uint32_t)st->lane_index[k];
    if (li != DART__NIL) return &st->lanes[li];
    if (st->lane_free == DART__NIL){                  /* pool dry: grow it */
        uint32_t ncap = st->lane_cap ? st->lane_cap*2u : 8u, i;
        i_DartLane *nl;
        if (ncap > 0xFFFFu) ncap = 0xFFFFu;           /* the u16 ticket space */
        if (ncap <= st->lane_cap) return NULL;        /* 65535 live matches: refuse */
        nl = (i_DartLane*)st->cfg.allocator(st->cfg.user, st->lanes, (size_t)ncap*sizeof(i_DartLane));
        if (!nl) return NULL;
        st->lanes = nl;
        for (i=ncap; i>st->lane_cap; i--){            /* thread the new slots onto the free list */
            nl[i-1].in_use = 0;
            nl[i-1].sched_next = st->lane_free;
            st->lane_free = i-1;
        }
        st->lane_cap = ncap;
    }
    li = st->lane_free; st->lane_free = st->lanes[li].sched_next;
    memset(&st->lanes[li], 0, sizeof(i_DartLane));
    st->lanes[li].channel = c; st->lanes[li].peer_slot = (uint16_t)peer_slot;
    st->lanes[li].sched_next = DART__NIL; st->lanes[li].ch_next = DART__NIL;
    st->lanes[li].in_use = 1;
    st->lane_index[k] = (uint16_t)li;
    return &st->lanes[li];
}

/* A lane with neither side matched leaves the channel chain; dynamic mode also frees its
 * grown reassembly buffers, drops any scheduler entry (a recycled record must never sit
 * on another peer's dest list), and recycles the record. No-op while a side is matched. */
static void i_dart_lane_release(DartTransportState *st, uint16_t c, uint32_t peer_slot){
    uint32_t li = i_dart_lane_id(st, c, peer_slot);
    i_DartLane *l;
    if (li == DART__NIL) return;
    l = &st->lanes[li];
    if (l->w.used || l->r.used) return;
    {   uint32_t *pp = &st->channels[c].lane_head;    /* unlink from the channel chain */
        while (*pp != DART__NIL && *pp != li) pp = &st->lanes[*pp].ch_next;
        if (*pp == li) *pp = l->ch_next;
    }
    l->ch_next = DART__NIL;
    if (!st->lane_index) return;                      /* fixed: the record itself is permanent */
    if (l->r.assembly_buf){ st->cfg.allocator(st->cfg.user, l->r.assembly_buf, 0); l->r.assembly_buf=NULL; l->r.assembly_cap=0; }
    if (l->r.frag_bitmap){ st->cfg.allocator(st->cfg.user, l->r.frag_bitmap, 0); l->r.frag_bitmap=NULL; l->r.bitmap_cap=0; }
    i_dart_sched_drop(st, li);
    l->in_use = 0;
    l->sched_next = st->lane_free; st->lane_free = li;
    st->lane_index[(size_t)c*st->cfg.max_peers + peer_slot] = 0xFFFF;
}

/* match one (channel,peer) lane side: it carries new data, repairs, acks/HB */
static void i_dart_writer_match(DartTransportState *st, uint16_t c, uint16_t peer_slot, i_DartLane *l){
    i_DartChannel *ch=&st->channels[c];
    i_DartWriterProxy *w=&l->w;
    memset(w,0,sizeof(*w));
    w->used=1;
    ch->matched_writers++;   /* only reached on a genuine 0->1 (rematch guards on !used) */
    /* only a reader that advertised RELIABLE acks; a best-effort reader stays out of
       flow control so it can't stall this writer (it gets new data, never repairs/HB) */
    w->reader_reliable = i_dart_bit_get(&st->peer_sub_reliable[(size_t)peer_slot*st->bitmap_len], c) ? 1u : 0u;
    w->sent_upto = i_dart_channel_unicast_join_seqno(ch);
    w->acked_upto = w->sent_upto;
    i_dart_lane_wake(st, c, peer_slot);   /* lane primed for new data + ack/hb */
}

static void i_dart_writer_unmatch(DartTransportState *st, uint16_t c, i_DartLane *l){
    if (!l->w.used) return;
    l->w.used=0;
    st->channels[c].matched_writers--;   /* guarded on used above: exactly one 1->0 per unmatch */
}

static void i_dart_reader_match(DartTransportState *st, uint16_t c, uint16_t peer_slot, i_DartLane *l){
    i_DartReaderProxy *r=&l->r;
    uint8_t *assembly_buf=r->assembly_buf, *frag_bitmap=r->frag_bitmap;
    uint32_t assembly_cap=r->assembly_cap, bitmap_cap=r->bitmap_cap;   /* keep grown buffers across rematch */
    memset(r,0,sizeof(*r));
    r->assembly_buf=assembly_buf; r->frag_bitmap=frag_bitmap; r->assembly_cap=assembly_cap; r->bitmap_cap=bitmap_cap;
    r->epoch=st->reader_epoch_counter++;   /* new incarnation: writers re-join on seeing it */
    r->used=1;       /* started==0: first DATA adopts the writer's position */
    st->channels[c].matched_readers++;   /* only reached on a genuine 0->1 (rematch guards on !used) */
    /* announce this incarnation once so a caught-up (idle, non-pinging) writer
       re-joins and replays. A genuine discovery blip keeps its position through
       dart_transport_peer_dormant/resume and never lands here, so a single ACKNACK suffices. */
    if (st->channels[c].qos.reliability==DART_RELIABLE){
        r->ack_pending=1; r->ack_due_us=0; r->ack_force=1;
        i_dart_lane_wake(st,c,peer_slot);
    }
}

static void i_dart_reader_unmatch(DartTransportState *st, uint16_t c, i_DartLane *l){
    if (l->r.used) st->channels[c].matched_readers--;   /* peer_remove calls this unconditionally */
    l->r.used=0; l->r.assembly_active=0;
}


/* recompute one (channel,peer) match from our role and the peer's interest bits.
 * Lane records exist only while a side is matched: the unmatched->matched edge allocates
 * (and links the channel chain), the matched->unmatched edge releases. Idempotent
 * re-application never touches a lane whose match state did not change, so reader
 * positions survive it exactly as before. */
static void i_dart_channel_rematch(DartTransportState *st, uint16_t c, uint16_t peer_slot){
    i_DartChannel *ch=&st->channels[c];
    const uint8_t *peer_pub_bitmap=&st->peer_pub_bitmap[(size_t)peer_slot*st->bitmap_len];
    const uint8_t *peer_sub_bitmap=&st->peer_sub_bitmap[(size_t)peer_slot*st->bitmap_len];
    int wuse = (ch->role==DART_PUBSUB || ch->role==DART_PUB_ONLY) && i_dart_bit_get(peer_sub_bitmap,c);
    int ruse = (ch->role==DART_PUBSUB || ch->role==DART_SUB_ONLY) && i_dart_bit_get(peer_pub_bitmap,c);
    i_DartLane *l = i_dart_lane_at(st,c,peer_slot);
    int had = l && (l->w.used || l->r.used);
    if (!wuse && !ruse){
        if (!had) return;
        i_dart_writer_unmatch(st,c,l);
        i_dart_reader_unmatch(st,c,l);
        i_dart_lane_release(st,c,peer_slot);
        return;
    }
    if (!l){
        l = i_dart_lane_ensure(st,c,peer_slot);   /* may relocate the pool: l is fresh */
        if (!l) return;                           /* OOM: refused; the peer's next announce retries */
    }
    if (wuse && !l->w.used) i_dart_writer_match(st,c,peer_slot,l);
    else if (!wuse && l->w.used) i_dart_writer_unmatch(st,c,l);
    if (ruse && !l->r.used) i_dart_reader_match(st,c,peer_slot,l);
    else if (!ruse && l->r.used) i_dart_reader_unmatch(st,c,l);
    if (!had && (l->w.used || l->r.used)){        /* first match on this lane: onto the channel chain */
        l->ch_next = ch->lane_head;
        ch->lane_head = i_dart_lane_id(st,c,peer_slot);
    } else if (had && !(l->w.used || l->r.used)){
        i_dart_lane_release(st,c,peer_slot);
    }
}


void dart_transport_peer_add(DartTransportState *st, uint32_t id, uint16_t peer_frag){
    uint16_t i; int free=-1; uint32_t max_peers=st->cfg.max_peers;
    if (i_dart_peer_slot(st,id)>=0) return;
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
    if (st->peer_alias[free] && st->peer_alias_len[free]){   /* slot reuse: no stale mappings/verdicts */
        memset(st->peer_alias[free],0xFF,(size_t)st->peer_alias_len[free]*sizeof(uint16_t));
        if (st->peer_astate[free]) memset(st->peer_astate[free],0,st->peer_alias_len[free]);
    }
    /* nothing matches until dart_transport_apply_peer_interest feeds the peer's interest
       list (carried in its discovery announce) */
}


void dart_transport_peer_remove(DartTransportState *st, uint32_t id){
    int s = i_dart_peer_slot(st,id); uint16_t c;
    if (s<0) return;
    for (c=0;c<st->cfg.n_channels;c++){
        i_DartLane *l = i_dart_lane_at(st,c,(uint32_t)s);
        if (!l) continue;
        i_dart_writer_unmatch(st,c,l);
        i_dart_reader_unmatch(st,c,l);
        /* release recycles the record AND frees the lane's grown reassembly buffers, so a
           gone peer keeps no per-lane memory at all */
        i_dart_lane_release(st,c,(uint32_t)s);
    }
    if (st->cfg.allocator && st->peer_alias[s]){   /* dynamic: the alias + verdict maps go too */
        st->cfg.allocator(st->cfg.user, st->peer_alias[s], 0);
        if (st->peer_astate[s]) st->cfg.allocator(st->cfg.user, st->peer_astate[s], 0);
        st->peer_alias[s]=NULL; st->peer_astate[s]=NULL; st->peer_alias_len[s]=0;
    }
    st->peer_used[s]=0; st->peer_dormant[s]=0;
#ifdef DART_SHM
    st->peer_shm[s]=0;
#endif
}


/* A peer fell silent (discovery timeout): keep every proxy and the reader's
 * deliver position, just drop the peer from flow control so a dead reader can't
 * stall the writer and a dead writer isn't acked. State revives via dart_transport_peer_resume. */
void dart_transport_peer_dormant(DartTransportState *st, uint32_t id){
    int s = i_dart_peer_slot(st,id);
    if (s>=0) st->peer_dormant[s]=1;
}


/* A dormant peer's SAME incarnation returned: re-include it in flow control and
 * re-report each reader position so the writer fills any gap (the reader dedups any
 * replay for free). The writer side needs nothing proactive; the reader's ACKNACK
 * re-arms its heartbeats. Proxies and deliver_upto were never touched, so no dup,
 * no loss. */
void dart_transport_peer_resume(DartTransportState *st, uint32_t id){
    int s = i_dart_peer_slot(st,id); uint16_t c;
    if (s<0) return;
    st->peer_dormant[s]=0;
    for (c=0;c<st->cfg.n_channels;c++){
        i_DartLane *l=i_dart_lane_at(st,c,(uint32_t)s);
        if (!l) continue;
        if (st->channels[c].qos.reliability!=DART_RELIABLE) continue;
        if (l->r.used){ l->r.ack_pending=1; l->r.ack_due_us=0; l->r.ack_force=1; }  /* report our position now */
        if (l->w.used || l->r.used) i_dart_lane_wake(st,c,(uint16_t)s);
    }
}


/* update a peer's advertised fragment size (its blob may arrive after first contact) */
void dart_transport_peer_set_frag(DartTransportState *st, uint32_t id, uint16_t peer_frag){
    int s = i_dart_peer_slot(st,id);
    if (s>=0) st->peer_frag[s]=dart_clamp_frag(peer_frag);
}

#ifdef DART_SHM

void dart_transport_peer_set_shm(DartTransportState *st, uint32_t id, int is_shm){
    int s = i_dart_peer_slot(st,id);
    if (s>=0) st->peer_shm[s]=(uint8_t)(is_shm?1:0);
}
#endif


void dart_transport_destroy(DartTransportState *st){
    uint16_t c; uint32_t li;
    if (!st || !st->cfg.allocator) return;     /* fixed mode: nothing hook-allocated */
    for (c=0;c<st->cfg.n_channels;c++){
        i_DartChannel *ch=&st->channels[c];
        uint16_t depth, d;
        if (!ch->dynamic) continue;
        depth = ch->qos.keep_last;
        for (d=0; d<depth; d++)
            if (ch->history[d].buf){ st->cfg.allocator(st->cfg.user, ch->history[d].buf, 0);
                                  ch->history[d].buf=NULL; ch->history[d].cap=0; }
        if (ch->history_owned && ch->history){   /* ring allocated by dart_transport_channel_define */
            st->cfg.allocator(st->cfg.user, ch->history, 0);
            ch->history=NULL; ch->history_owned=0;
        }
    }
    for (li=0; li<st->lane_cap; li++){           /* live records' grown reassembly buffers */
        i_DartLane *l=&st->lanes[li];
        if (!l->in_use) continue;
        if (l->r.assembly_buf){ st->cfg.allocator(st->cfg.user, l->r.assembly_buf, 0); l->r.assembly_buf=NULL; l->r.assembly_cap=0; }
        if (l->r.frag_bitmap){ st->cfg.allocator(st->cfg.user, l->r.frag_bitmap, 0); l->r.frag_bitmap=NULL; l->r.bitmap_cap=0; }
    }
    if (st->lane_index && st->lanes){            /* the record pool itself (one hook allocation) */
        st->cfg.allocator(st->cfg.user, st->lanes, 0);
        st->lanes=NULL; st->lane_cap=0; st->lane_free=DART__NIL;
    }
    {   uint32_t p;                              /* per-peer alias + verdict maps (hook allocations) */
        for (p=0;p<st->cfg.max_peers;p++){
            if (st->peer_alias[p]){
                st->cfg.allocator(st->cfg.user, st->peer_alias[p], 0);
                st->peer_alias[p]=NULL; st->peer_alias_len[p]=0;
            }
            if (st->peer_astate[p]){
                st->cfg.allocator(st->cfg.user, st->peer_astate[p], 0);
                st->peer_astate[p]=NULL;
            }
        }
    }
}


/* The peer's alias + verdict maps, guaranteed to cover `need` entries: the existing
 * maps, grown/new hook allocations (dynamic; the grown tail starts unmapped, verdicts
 * cleared), or NULL when they cannot grow (fixed-mode arena slice too small, or OOM):
 * the caller then counts the entries as unmappable. Sized once per apply to the peer's
 * whole positional list, so any advertised alias is covered. */
static uint16_t *i_dart_peer_alias_ensure(DartTransportState *st, int peer_slot, uint32_t need){
    uint32_t have = st->peer_alias_len[peer_slot];
    uint16_t *nm; uint8_t *ns;
    if (need <= have) return st->peer_alias[peer_slot];
    if (!st->cfg.allocator) return NULL;                /* fixed: the arena slice is the limit */
    nm = (uint16_t*)st->cfg.allocator(st->cfg.user, st->peer_alias[peer_slot], (size_t)need*sizeof(uint16_t));
    if (!nm) return NULL;
    st->peer_alias[peer_slot] = nm;                     /* len not yet raised: retryable on OOM below */
    ns = (uint8_t*)st->cfg.allocator(st->cfg.user, st->peer_astate[peer_slot], (size_t)need);
    if (!ns) return NULL;
    memset(nm + have, 0xFF, (size_t)(need-have)*sizeof(uint16_t));   /* grown tail: unmapped */
    memset(ns + have, 0, (size_t)(need-have));                       /* ...no verdicts */
    st->peer_astate[peer_slot] = ns; st->peer_alias_len[peer_slot] = need;
    return nm;
}

/* Candidate channels for a 32-bit interest hash: the count of local channels whose
 * identity's low 32 bits match, and the preferred one (non-INACTIVE first, mirroring
 * i_dart_channel_by_identity). Same linear resolution the identity lookup pays. */
static int i_dart_hash32_candidates(DartTransportState *st, uint32_t h, int *idx_out){
    uint16_t i; int first=-1, live=-1, n=0;
    for (i=0;i<st->cfg.n_channels;i++){
        if (st->channels[i].name_len==0 || (uint32_t)st->channels[i].identity != h) continue;
        n++;
        if (first<0) first=(int)i;
        if (live<0 && st->channels[i].role!=DART_INACTIVE) live=(int)i;
    }
    if (idx_out) *idx_out = live>=0 ? live : first;
    return n;
}


/* Upper bound on dart_transport_build_interest output, for sizing the announce buffer:
 * one positional [u32 hash][u8 flags] entry per channel slot. */
size_t dart_interest_max(uint16_t n_channels){
    return 2u + 5u * (size_t)n_channels;
}


/* Serialize our interest into out: [u16 n], then one [u32 hash][u8 flags] entry per
 * channel IN INDEX ORDER up to the highest defined slot (position = alias; an undefined
 * reserve slot rides as an INACTIVE hole so later aliases stay stable). Returns bytes
 * written, or 0 if cap is too small; size out via dart_interest_max. */
size_t dart_transport_build_interest(DartTransportState *st, void *out, size_t cap){
    uint8_t *o=(uint8_t*)out;
    uint16_t c, n=0;
    for (c=0;c<st->cfg.n_channels;c++) if (st->channels[c].name_len) n=(uint16_t)(c+1u);
    if (cap < 2u + 5u*(size_t)n) return 0;
    i_dart_le_w16(o, n);
    for (c=0;c<n;c++){
        const i_DartChannel *ch = &st->channels[c];
        uint8_t *e = o + 2u + 5u*(size_t)c;
        uint8_t role = ch->name_len ? ch->role : (uint8_t)DART_INACTIVE;
        i_dart_le_w32(e, ch->name_len ? (uint32_t)ch->identity : 0u);
        e[4] = (uint8_t)((role & DART__INT_ROLE_MASK)
             | (ch->qos.reliability==DART_RELIABLE ? DART__INT_RELIABLE : 0u));
    }
    return 2u + 5u*(size_t)n;
}


/* A peer's interest list arrived (from its discovery announce): re-derive its bits from
 * the cached per-alias VERDICTS + the entry's current flags, then rematch every channel.
 * Idempotent. An entry without a verdict stays PENDING (no bit, no proxy, no demux):
 * dart_transport_detail_wants names it and the verdict arrives via
 * dart_transport_apply_peer_details, after which the caller re-runs this. The gates
 * mirror the old inline scan: RxO reliability (a reliable subscriber refuses a
 * best-effort publisher, never silently downgraded; the match forms automatically if
 * the publisher upgrades and re-advertises) and the cached schema verdict per
 * direction, each refusal fired as its event on every apply that would have used it. */
void dart_transport_apply_peer_interest(DartTransportState *st, uint32_t peer_id, DartBytes blob){
    const uint8_t *d=blob.data;
    uint16_t n, c; uint32_t a; int peer_slot=i_dart_peer_slot(st,peer_id);
    uint8_t *peer_pub_bitmap, *peer_sub_bitmap, *peer_sub_reliable;
    uint16_t *amap; uint8_t *astate;
    uint32_t unmappable = 0;
    if (peer_slot<0 || !d || blob.len<2) return;
    n = i_dart_le_r16(d);
    if (blob.len < 2u + 5u*(uint32_t)n) return;        /* truncated: reject wholesale */
    peer_pub_bitmap  =&st->peer_pub_bitmap[(size_t)peer_slot*st->bitmap_len];
    peer_sub_bitmap  =&st->peer_sub_bitmap[(size_t)peer_slot*st->bitmap_len];
    peer_sub_reliable=&st->peer_sub_reliable[(size_t)peer_slot*st->bitmap_len];
    memset(peer_pub_bitmap,0,st->bitmap_len); memset(peer_sub_bitmap,0,st->bitmap_len);
    memset(peer_sub_reliable,0,st->bitmap_len);
    amap   = n ? i_dart_peer_alias_ensure(st, peer_slot, n) : NULL;
    astate = amap ? st->peer_astate[peer_slot] : NULL;
    for (a=0;a<n;a++){
        const uint8_t *e = d + 2u + 5u*a;
        uint8_t flags = e[4], role = (uint8_t)(flags & DART__INT_ROLE_MASK);
        int their_pub, their_sub, rel;
        uint16_t cidx; i_DartChannel *ch;
        if (role == DART_INACTIVE) continue;
        if (!astate || a >= st->peer_alias_len[peer_slot]){
            /* no verdict storage (fixed-mode table too small, or map OOM): this entry
               can never verify or demux here; count it if it would have been a candidate */
            if (i_dart_hash32_candidates(st, i_dart_le_r32(e), NULL)) unmappable++;
            continue;
        }
        if (!(astate[a] & DART__AST_DETAILED) || !(astate[a] & DART__AST_NAME_OK))
            continue;                     /* PENDING (details on their way) or a verified non-match */
        cidx = amap[a];
        if (cidx >= st->cfg.n_channels) continue;      /* defensive: stale map */
        ch = &st->channels[cidx];
        if (ch->role == DART_INACTIVE){
            /* dual same-identity channels switched by role: the verdict (incl. its schema
               gates) bound the then-live twin, so re-verify against the one live now */
            int t = (int)cidx;
            i_dart_channel_by_identity(st, ch->identity, &t);
            if ((uint16_t)t != cidx){
                astate[a] = 0; amap[a] = 0xFFFFu;      /* pending again; wants() re-asks */
                continue;
            }
        }
        their_pub = (role==DART_PUBSUB || role==DART_PUB_ONLY);
        their_sub = (role==DART_PUBSUB || role==DART_SUB_ONLY);
        rel       = (flags & DART__INT_RELIABLE) != 0;
        if (their_pub){                                /* their offered QoS vs our subscription */
            int ours_sub = (ch->role==DART_PUBSUB || ch->role==DART_SUB_ONLY);
            if (ours_sub && ch->qos.reliability==DART_RELIABLE && !rel){
                i_dart_transport_fire_event(st, DART_TRANSPORT_QOS_INCOMPATIBLE, cidx, peer_id, 0, 0);
            } else if (!(astate[a] & DART__AST_READ_OK)){
                i_dart_transport_fire_event(st, DART_TRANSPORT_SCHEMA_MISMATCH, cidx, peer_id, 0, 0);
            } else {
                i_dart_bit_set(peer_pub_bitmap,(uint32_t)cidx);
            }
        }
        if (their_sub){                                /* their requested QoS, for our writer */
            if (!(astate[a] & DART__AST_WRITE_OK)){
                i_dart_transport_fire_event(st, DART_TRANSPORT_SCHEMA_MISMATCH, cidx, peer_id, 0, 0);
            } else {
                i_dart_bit_set(peer_sub_bitmap,(uint32_t)cidx);
                if (rel) i_dart_bit_set(peer_sub_reliable,(uint32_t)cidx);
            }
        }
    }
    if (unmappable)   /* never silent: those topics can never deliver here */
        i_dart_transport_fire_event(st, DART_TRANSPORT_INTEREST_OVERFLOW, 0, peer_id, 0, unmappable);
    for (c=0;c<st->cfg.n_channels;c++) i_dart_channel_rematch(st,c,(uint16_t)peer_slot);
}


/* diagnostic: how many channels we now publish to / receive from this peer (unicast
 * lanes). Surfaced on DART_PEER_INTEREST so a caller can see a match form (or not). */
void dart_transport_peer_match_counts(DartTransportState *st, uint32_t peer_id,
                            uint16_t *publish_to, uint16_t *receive_from){
    int s; uint16_t c, w=0, r=0;
    if (publish_to)   *publish_to   = 0;
    if (receive_from) *receive_from = 0;
    if (!st) return;
    s = i_dart_peer_slot(st, peer_id);
    if (s < 0) return;
    for (c=0;c<st->cfg.n_channels;c++){
        i_DartLane *l = i_dart_lane_at(st,c,(uint32_t)s);
        if (!l) continue;
        if (l->w.used) w++;
        if (l->r.used) r++;
    }
    if (publish_to)   *publish_to   = w;
    if (receive_from) *receive_from = r;
}


/* Discovery-announce meta blob codec (see dart_meta_* in core.h for the layout). The
   hash-only interest list is wrapped in a prefix carrying frag size and (odd ver) SHM
   info. No back-compat: the version byte just tags the one current format, and a blob
   whose magic/version we don't expect is rejected, not reinterpreted. Parsing is fully
   bounds-checked (see dart_transport_apply_peer_interest), so a malformed or foreign
   blob is dropped wholesale, never trusted. Names and schemas are NOT here: they ride
   the pairwise detail exchange below. */
#define DART__META_BASE_NOSHM 5u    /* 'D','N',ver, frag_lo, frag_hi */
#define DART__META_BASE_SHM   22u   /* ... + shm(1) + host[16] */
#ifdef DART_SHM
#define DART__META_VER  11u                 /* what WE write */
#define DART__META_BASE DART__META_BASE_SHM
#else
#define DART__META_VER  10u
#define DART__META_BASE DART__META_BASE_NOSHM
#endif

static int i_dart_meta_ok(DartBytes meta){
    return meta.data && meta.len >= 5 && meta.data[0]=='D' && meta.data[1]=='N'
        && meta.data[2]>=10 && meta.data[2]<=11;
}
/* base prefix through host[16], by version (odd v11 carries shm+host, even v10 doesn't). */
static uint16_t i_dart_meta_base(const uint8_t *meta){
    return (meta[2] & 1u) ? DART__META_BASE_SHM : DART__META_BASE_NOSHM;
}
uint16_t dart_meta_capacity(uint16_t n_channels){
    size_t cap = (size_t)DART__META_BASE + dart_interest_max(n_channels);
    if (cap > 65000u) cap = 65000u;
    return (uint16_t)cap;
}

/* Exact overlay size the next dart_transport_meta_build will emit for the current
 * channel state (the same walk, byte for byte), so a caller can size the buffer to the
 * actual content instead of dart_meta_capacity's full-reserve worst case. */
uint16_t dart_transport_meta_size(DartTransportState *st){
    uint16_t c, n=0;
    size_t len;
    for (c=0;c<st->cfg.n_channels;c++) if (st->channels[c].name_len) n=(uint16_t)(c+1u);
    len = (size_t)DART__META_BASE + 2u + 5u*(size_t)n;
    if (len > 65000u) len = 65000u;              /* the dart_meta_capacity ceiling; past it the build truncates */
    return (uint16_t)len;
}

uint16_t dart_transport_meta_build(DartTransportState *st, uint8_t *out, uint16_t cap,
                         uint16_t frag_size, int shm_capable, const uint8_t host[16]){
    size_t interest_len, len; uint16_t off = DART__META_BASE;
    out[0]='D'; out[1]='N'; out[2]=DART__META_VER;
    out[3]=(uint8_t)(frag_size & 0xFF); out[4]=(uint8_t)(frag_size >> 8);
#ifdef DART_SHM
    out[5]=(uint8_t)(shm_capable?1:0);
    if (host) memcpy(out+6, host, 16); else memset(out+6, 0, 16);
#else
    (void)shm_capable; (void)host;
#endif
    interest_len = dart_transport_build_interest(st, out + off, cap - off);   /* no name here: that is discovery's */
    len = (size_t)off + interest_len;
    if (interest_len == 0)    /* did not fit (returns >= 2 even with zero channels): never silent */
        i_dart_transport_fire_event(st, DART_TRANSPORT_META_TRUNCATED_INTEREST, 0, 0, 0, 0);
    return (uint16_t)len;
}

uint16_t dart_meta_frag(DartBytes meta){
    if (!i_dart_meta_ok(meta)) return 0;
    return (uint16_t)(meta.data[3] | ((uint16_t)meta.data[4] << 8));
}

DartBytes dart_meta_interest(DartBytes meta){
    uint16_t off;
    if (!i_dart_meta_ok(meta)) return dart_bytes(NULL, 0);
    off = i_dart_meta_base(meta.data);     /* interest follows the base prefix (no name in the overlay) */
    if (meta.len < off) return dart_bytes(NULL, 0);
    return dart_bytes(meta.data + off, meta.len - off);
}

int dart_meta_interest_next(DartBytes meta, DartInterestIter *it, DartTopic *out){
    if (!it || !out) return 0;
    if (!it->started){                    /* first call: parse the [u16 n] header */
        DartBytes in = dart_meta_interest(meta);
        it->started = 1; it->left = 0; it->alias = 0; it->phase = 0; it->off = 0;
        if (!in.data || in.len < 2) return 0;      /* no/short interest list: nothing to yield */
        it->left = (uint16_t)(in.data[0] | ((uint16_t)in.data[1] << 8));
        it->off  = (uint32_t)(in.data - meta.data) + 2u;   /* first entry, past n */
    }
    while (it->left){
        uint32_t off = it->off;
        uint8_t flags, role;
        if ((size_t)off + 5u > meta.len){ it->left = 0; return 0; }   /* truncated: stop */
        flags = meta.data[off + 4]; role = (uint8_t)(flags & DART__INT_ROLE_MASK);
        if (role == DART_INACTIVE){       /* declared-but-off / undefined hole: not advertised */
            it->left--; it->alias++; it->off = off + 5u; it->phase = 0;
            continue;
        }
        out->alias    = it->alias;
        out->role     = role;
        out->reliable = (uint8_t)((flags & DART__INT_RELIABLE) ? 1 : 0);
        out->hash     = i_dart_le_r32(meta.data + off);
        if (it->phase == 0 && (role==DART_PUBSUB || role==DART_PUB_ONLY)){
            out->is_pub = 1;
            if (role==DART_PUBSUB){ it->phase = 1; return 1; }   /* sub direction next call */
            it->left--; it->alias++; it->off = off + 5u;
            return 1;
        }
        out->is_pub = 0;                  /* SUB_ONLY, or the second yield of a PUBSUB entry */
        it->phase = 0; it->left--; it->alias++; it->off = off + 5u;
        return 1;
    }
    return 0;
}

#ifdef DART_SHM
int dart_meta_shm(DartBytes meta, uint8_t host[16]){
    if (!i_dart_meta_ok(meta) || meta.data[2]!=11
        || meta.len < DART__META_BASE_SHM || !meta.data[5]) return 0;
    memcpy(host, meta.data+6, 16);
    return 1;
}
#endif


/* Pairwise detail exchange codec ('uDTL', see core.h for the layout and contract).
   Parsing is fully bounds-checked: a malformed request or response is dropped wholesale,
   never trusted. The responder side is a pure read of channel + schema state. */
#define DART__DETAIL_HDR 14u   /* magic(4) kind(1) ver(1) domain(2) meta_version(4) n(2) */
#define DART__DETAIL_VER 1u

static int i_dart_detail_hdr_ok(DartBytes d){
    return d.data && d.len >= DART__DETAIL_HDR
        && d.data[0]=='u' && d.data[1]=='D' && d.data[2]=='T' && d.data[3]=='L'
        && d.data[5]==DART__DETAIL_VER;
}

int dart_detail_kind(DartBytes dgram){
    if (!i_dart_detail_hdr_ok(dgram)) return 0;
    return (dgram.data[4]==DART_DETAIL_REQ || dgram.data[4]==DART_DETAIL_RESP)
         ? dgram.data[4] : 0;
}
uint16_t dart_detail_domain(DartBytes dgram){
    return i_dart_detail_hdr_ok(dgram) ? i_dart_le_r16(dgram.data+6) : 0;
}
uint32_t dart_detail_meta_version(DartBytes dgram){
    return i_dart_detail_hdr_ok(dgram) ? i_dart_le_r32(dgram.data+8) : 0;
}

static void i_dart_detail_hdr_write(uint8_t *o, uint8_t kind, uint16_t domain,
                                    uint32_t meta_version, uint16_t n){
    o[0]='u'; o[1]='D'; o[2]='T'; o[3]='L';
    o[4]=kind; o[5]=(uint8_t)DART__DETAIL_VER;
    i_dart_le_w16(o+6, domain);
    i_dart_le_w32(o+8, meta_version);
    i_dart_le_w16(o+12, n);
}

size_t dart_detail_req_build(uint16_t domain, uint32_t peer_meta_version,
                             const DartDetailWant *wants, uint16_t n_wants,
                             void *out, size_t cap){
    uint8_t *o=(uint8_t*)out; uint16_t k;
    size_t need = (size_t)DART__DETAIL_HDR + (size_t)n_wants*10u;
    if (!o || (n_wants && !wants) || cap < need) return 0;
    i_dart_detail_hdr_write(o, DART_DETAIL_REQ, domain, peer_meta_version, n_wants);
    for (k=0;k<n_wants;k++){
        uint8_t *e = o + DART__DETAIL_HDR + (size_t)k*10u;
        i_dart_le_w16(e, wants[k].alias);
        i_dart_le_w64(e+2, wants[k].schema_hash);
    }
    return need;
}

/* One walk serves size and build (out NULL = measure), so the two agree byte for byte.
   A truncated build stops at an entry boundary: the response stays parseable and the
   requester re-requests the aliases it still lacks (the paging seam). */
static size_t i_dart_detail_answer(DartTransportState *st, const DartMetaSchema *schemas,
                                   uint32_t meta_version, DartBytes req,
                                   uint8_t *out, size_t cap){
    const uint8_t *r; uint16_t n_req, k, n_out=0;
    size_t len = DART__DETAIL_HDR;
    if (!st || dart_detail_kind(req) != DART_DETAIL_REQ) return 0;
    n_req = i_dart_le_r16(req.data+12);
    if (req.len < (size_t)DART__DETAIL_HDR + (size_t)n_req*10u) return 0;   /* truncated: reject */
    if (out){
        if (cap < DART__DETAIL_HDR) return 0;
        i_dart_detail_hdr_write(out, DART_DETAIL_RESP, dart_detail_domain(req), meta_version, 0);
    }
    r = req.data + DART__DETAIL_HDR;
    for (k=0;k<n_req;k++,r+=10){
        uint16_t alias    = i_dart_le_r16(r);
        uint64_t req_hash = i_dart_le_r64(r+2);
        const i_DartChannel *ch;
        uint64_t hash; DartBytes wire; size_t need;
        if (alias >= st->cfg.n_channels) continue;             /* unknown: not advertised */
        ch = &st->channels[alias];
        if (ch->role == DART_INACTIVE || ch->name_len == 0) continue;
        hash = schemas ? schemas[alias].hash : 0;
        wire = dart_bytes(NULL, 0);
        if (hash && hash != req_hash && schemas[alias].wire.len <= 0xFFFFu)
            wire = schemas[alias].wire;    /* differs: inline for the subset check */
        need = 2u + 1u + ch->name_len + 8u + 2u + wire.len;
        if (out){
            uint8_t *e = out + len;
            if (len + need > cap) break;
            i_dart_le_w16(e, alias);
            e[2] = ch->name_len;
            memcpy(e+3, ch->name, ch->name_len);
            i_dart_le_w64(e+3+ch->name_len, hash);
            i_dart_le_w16(e+3+ch->name_len+8, (uint16_t)wire.len);
            if (wire.len) memcpy(e+3+ch->name_len+10, wire.data, wire.len);
        }
        len += need;
        n_out++;
    }
    if (out) i_dart_le_w16(out+12, n_out);
    return len;
}

size_t dart_transport_detail_resp_size(DartTransportState *st, const DartMetaSchema *schemas,
                                       DartBytes req){
    return i_dart_detail_answer(st, schemas, 0, req, NULL, 0);
}

size_t dart_transport_detail_respond(DartTransportState *st, const DartMetaSchema *schemas,
                                     uint32_t meta_version, DartBytes req,
                                     void *out, size_t cap){
    return i_dart_detail_answer(st, schemas, meta_version, req, (uint8_t*)out, cap);
}

int dart_detail_next(DartBytes resp, DartDetailIter *it, DartDetail *out){
    uint32_t off; uint8_t nlen; uint16_t wlen;
    if (!it || !out) return 0;
    if (!it->started){
        it->started = 1; it->left = 0; it->off = DART__DETAIL_HDR;
        if (dart_detail_kind(resp) != DART_DETAIL_RESP) return 0;
        it->left = i_dart_le_r16(resp.data+12);
    }
    if (!it->left) return 0;
    off = it->off;
    if ((size_t)off + 3u > resp.len){ it->left = 0; return 0; }        /* truncated: stop */
    nlen = resp.data[off+2];
    if ((size_t)off + 3u + nlen + 10u > resp.len){ it->left = 0; return 0; }
    wlen = i_dart_le_r16(resp.data + off + 3u + nlen + 8u);
    if ((size_t)off + 3u + nlen + 10u + wlen > resp.len){ it->left = 0; return 0; }
    out->alias       = i_dart_le_r16(resp.data + off);
    out->name        = dart_string((const char*)(resp.data + off + 3u), nlen);
    out->schema_hash = i_dart_le_r64(resp.data + off + 3u + nlen);
    out->schema_wire = wlen ? dart_bytes(resp.data + off + 3u + nlen + 10u, wlen)
                            : dart_bytes(NULL, 0);
    it->off = off + 3u + nlen + 10u + wlen;
    it->left--;
    return 1;
}

/* The aliases still PENDING for a peer: candidates (32-bit hash overlap + role overlap)
 * without a cached verdict. See core.h for the request-on-every-announce retry contract. */
uint16_t dart_transport_detail_wants(DartTransportState *st, const DartMetaSchema *schemas,
                                     uint32_t peer_id, DartBytes interest,
                                     DartDetailWant *out, uint16_t max_wants){
    const uint8_t *d=interest.data;
    uint16_t n, cnt=0; uint32_t a; int slot;
    uint8_t *astate; uint32_t alen;
    if (!st || !d || interest.len < 2) return 0;
    slot = i_dart_peer_slot(st, peer_id);
    if (slot < 0) return 0;
    n = i_dart_le_r16(d);
    if (interest.len < 2u + 5u*(uint32_t)n) return 0;
    astate = st->peer_astate[slot]; alen = st->peer_alias_len[slot];
    for (a=0;a<n;a++){
        const uint8_t *e = d + 2u + 5u*a;
        uint8_t flags = e[4], role = (uint8_t)(flags & DART__INT_ROLE_MASK);
        int cidx = -1, nc;
        i_DartChannel *ch;
        int their_pub, their_sub, ours_pub, ours_sub;
        if (role == DART_INACTIVE) continue;
        if (astate && a < alen && (astate[a] & DART__AST_DETAILED)) continue;   /* decided */
        nc = i_dart_hash32_candidates(st, i_dart_le_r32(e), &cidx);
        if (!nc) continue;                                 /* no local channel: not a candidate */
        ch = &st->channels[cidx];
        their_pub = (role==DART_PUBSUB || role==DART_PUB_ONLY);
        their_sub = (role==DART_PUBSUB || role==DART_SUB_ONLY);
        ours_pub  = (ch->role==DART_PUBSUB || ch->role==DART_PUB_ONLY);
        ours_sub  = (ch->role==DART_PUBSUB || ch->role==DART_SUB_ONLY);
        if (!((their_pub && ours_sub) || (their_sub && ours_pub))) continue;   /* roles never meet */
        if (out){
            if (cnt >= max_wants) break;
            out[cnt].alias = (uint16_t)a;
            /* several local channels behind one 32-bit hash: send hash 0 to force the
               wire inline, so whichever channel the name binds to can still verify */
            out[cnt].schema_hash = (nc == 1 && schemas) ? schemas[cidx].hash : 0;
        }
        cnt++;
    }
    return cnt;
}

/* Ingest a DETAIL_RESP: verify each entry (full identity from the name; the schema gate
 * per direction) and cache the verdict. Returns newly decided aliases; the caller then
 * re-applies the peer's interest so the verdicts form their matches. Idempotent: decided
 * aliases are skipped, so duplicate/crossing responses are harmless. */
uint16_t dart_transport_apply_peer_details(DartTransportState *st, uint32_t peer_id, DartBytes resp){
    DartDetailIter it; DartDetail dd;
    int slot; uint16_t fresh = 0;
    if (!st) return 0;
    slot = i_dart_peer_slot(st, peer_id);
    if (slot < 0) return 0;
    memset(&it, 0, sizeof it);
    while (dart_detail_next(resp, &it, &dd)){
        uint16_t *amap = st->peer_alias[slot]; uint8_t *astate = st->peer_astate[slot];
        uint64_t id64; i_DartChannel *ch; int cidx = -1;
        if (!amap || !astate || (uint32_t)dd.alias >= st->peer_alias_len[slot])
            continue;   /* maps are sized at interest apply; an alias we never saw is ignored
                           (a response that raced ahead of the announce re-resolves later) */
        if (astate[dd.alias] & DART__AST_DETAILED) continue;
        if (dd.name.len == 0){ astate[dd.alias] = DART__AST_DETAILED; fresh++; continue; }
        id64 = i_dart_identity_hash((const uint8_t*)dd.name.data, dd.name.len);
        ch = i_dart_channel_by_identity(st, id64, &cidx);
        if (!ch){                              /* the 32-bit nomination was a false positive */
            astate[dd.alias] = DART__AST_DETAILED;
            fresh++; continue;
        }
        if (ch->name_len != dd.name.len || memcmp(ch->name, dd.name.data, dd.name.len) != 0){
            astate[dd.alias] = DART__AST_DETAILED;   /* same 64-bit id, different name: refused */
            i_dart_transport_fire_event(st, DART_TRANSPORT_NAME_COLLISION, (uint16_t)cidx,
                        peer_id, id64, 0);
            fresh++; continue;
        }
        amap[dd.alias] = (uint16_t)cidx;
        astate[dd.alias] = (uint8_t)(DART__AST_DETAILED | DART__AST_NAME_OK
            | ((!st->cfg.schema_check || st->cfg.schema_check(st->cfg.user, peer_id, (uint16_t)cidx,
                    1, dd.schema_hash, dd.schema_wire)) ? DART__AST_READ_OK : 0u)
            | ((!st->cfg.schema_check || st->cfg.schema_check(st->cfg.user, peer_id, (uint16_t)cidx,
                    0, dd.schema_hash, dd.schema_wire)) ? DART__AST_WRITE_OK : 0u));
        fresh++;
    }
    return fresh;
}


int dart_transport_set_role(DartTransportState *st, uint16_t channel, uint8_t role){
    int channel_idx; i_DartChannel *ch; uint16_t p;
    if (role > DART_INACTIVE) return -1;
    ch = i_dart_channel_at(st, channel, &channel_idx);
    if (!ch) return -1;
    if (ch->role == role) return 0;
    ch->role = role;
    for (p=0;p<st->cfg.max_peers;p++)
        if (st->peer_used[p]) i_dart_channel_rematch(st,(uint16_t)channel_idx,p);
    /* caller re-advertises interest (the node bumps its discovery announce) */
    return 0;
}


int dart_transport_channel_define(DartTransportState *st, uint16_t channel, const DartChannelDef *def){
    i_DartChannel *ch; DartQos q; uint16_t depth, p; size_t lane;
    if (!st || !def || !st->cfg.allocator) return -1;       /* dynamic (reserve) mode only */
    if (channel >= st->cfg.n_channels) return -1;            /* out of reserved range */
    if (!def->name || !def->name[0]) return -1;             /* name = identity, required */
    lane = i_dart_name_len(def->name);
    if (def->name[lane]) return -1;                          /* longer than DART_TOPIC_NAME_MAX */
    ch = &st->channels[channel];
    if (ch->identity != 0 || ch->history) return -1;        /* slot already defined */
    q = def->qos; i_dart_qos_defaults(&q, 1);                 /* dynamic: grow-to-fit buffers */
    depth = q.keep_last;
    ch->history = (i_DartWriterSample*)st->cfg.allocator(st->cfg.user, NULL,
                                                         (size_t)depth*sizeof(i_DartWriterSample));
    if (!ch->history) return -4;                            /* OOM */
    memset(ch->history, 0, (size_t)depth*sizeof(i_DartWriterSample));
    ch->history_owned = 1; ch->dynamic = 1;
    ch->qos = q; ch->max_fragments = i_dart_max_fragments(q.max_message_bytes);
    ch->role = def->role;
    ch->identity = dart_channel_identity(def);
    memcpy((char*)ch->name, def->name, lane); ((char*)ch->name)[lane] = '\0';
    ch->name_len = (uint8_t)lane;
    ch->history_head = 0; ch->next_seqno = 0; ch->have_first = 0;
    for (p=0;p<st->cfg.max_peers;p++)        /* match the newly active channel to known peers */
        if (st->peer_used[p]) i_dart_channel_rematch(st, channel, p);
    return 0;
}


DartString dart_transport_channel_name(DartTransportState *st, uint16_t channel){
    i_DartChannel *ch = i_dart_channel_at(st, channel, NULL);
    if (!ch || ch->name_len == 0) return dart_string(NULL, 0);   /* undefined / reserve slot */
    return dart_string(ch->name, ch->name_len);
}


const DartQos *dart_transport_channel_qos(DartTransportState *st, uint16_t channel){
    i_DartChannel *ch = i_dart_channel_at(st, channel, NULL);
    return ch ? &ch->qos : NULL;
}


void dart_transport_repair_stats(DartTransportState *st, uint16_t channel, DartRepairStats *out){
    i_DartChannel *ch = i_dart_channel_at(st, channel, NULL);
    if (!out) return;
    if (ch) *out = ch->repair_stats;
    else memset(out, 0, sizeof *out);
}


void dart_transport_on_datagram(DartTransportState *st, uint32_t from, DartBytes datagram, uint64_t now){
    const uint8_t *p=datagram.data; size_t rem=datagram.len;
    int peer_slot=i_dart_peer_slot(st,from);
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
                            if (b0 & DART_F_SINGLE){ if (rem<DART_HEADER_DATA_SINGLE) return; sub=DART_HEADER_DATA_SINGLE+(size_t)i_dart_le_r16(p+DART_OFFSET_PAYLOAD_LEN_SINGLE); }
                            else { if (rem<DART_HEADER_DATA_MULTI) return; sub=DART_HEADER_DATA_MULTI+(size_t)i_dart_le_r16(p+DART_OFFSET_PAYLOAD_LEN); } break;
            case DART_HB:   if (rem<DART_HEADER_HB) return; sub=DART_HEADER_HB; break;
            case DART_NACK: if (rem<DART_HEADER_NACK) return; sub=DART_HEADER_NACK; break;
            default: return;             /* unknown type: cannot resync, drop rest */
        }
        if (sub>rem) return;             /* truncated/malformed */
        alias = i_dart_le_r16(p+DART_OFFSET_ALIAS);
        if ((uint32_t)alias < st->peer_alias_len[peer_slot]){
            uint16_t m=st->peer_alias[peer_slot][alias]; channel_idx=(m==0xFFFFu)?-1:(int)m;
        } else channel_idx=-1;
        if (channel_idx>=0){
            switch(type){
                case DART_DATA:
#ifdef DART_SHM
                                if (p[0] & DART_F_SHM){ i_dart_reader_shm(st,channel_idx,peer_slot,p,now); break; }
#endif
                                i_dart_reader_data(st,channel_idx,peer_slot,p,now); break;
                case DART_HB:   i_dart_reader_hb  (st,channel_idx,peer_slot,p,now); break;
                case DART_NACK: i_dart_writer_nack(st,channel_idx,peer_slot,p); break;
            }
        }
        p+=sub; rem-=sub;
    }
}
