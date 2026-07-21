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

uint64_t dart_topic_identity(const DartTopicDef *def){
    return dart_topic_id(def->name);   /* the name is the cross-peer identity */
}

static size_t i_dart_name_len(const char *s){            /* capped strlen */
    size_t n = 0;
    if (s) while (s[n] && n < DART_TOPIC_NAME_MAX) n++;
    return n;
}


/* zero-means-default for the tunable QoS fields, applied once at init so the
 * stored qos is authoritative */
static void i_dart_qos_defaults(DartQos *q){
    if (q->keep_last == 0)        q->keep_last       = q->reliability==DART_RELIABLE
                                                     ? DART_QOS_DEF_KEEP_LAST_REL
                                                     : DART_QOS_DEF_KEEP_LAST;
    if (q->heartbeat_us == 0)     q->heartbeat_us    = DART_QOS_DEF_HEARTBEAT_US;
    if (q->repair_delay_us == 0)  q->repair_delay_us = DART_QOS_DEF_REPAIR_US;
}


/* Normalize a node/peer UDP fragment size: 0 -> default, then clamp to [MIN,MAX].
   Public so the transport (dart_transport_init) and the node (announce blob) clamp identically. */
uint16_t dart_clamp_frag(uint16_t frag_size){
    uint16_t f = frag_size ? frag_size : DART_FRAG_SIZE;
    if (f < DART_FRAG_SIZE_MIN) f = DART_FRAG_SIZE_MIN;
    if (f > DART_FRAG_SIZE_MAX) f = DART_FRAG_SIZE_MAX;
    return f;
}

uint16_t dart_transport_frag(DartTransportState *st){ return st ? st->frag : dart_clamp_frag(0); }


/* lay out everything (b->base==NULL = measure only). The arena holds only the fixed
   tables (peer arrays, bitmaps, the lane ticket table, the name pool); message buffers,
   reassembly state, index maps, and lane records are hook allocations sized to actual
   traffic. */
static DartTransportState *i_dart_transport_build(i_DartBump *b, const DartConfig *cfg){
    uint16_t c; uint32_t max_peers = cfg->max_peers, n_topics = cfg->n_topics;
    uint16_t bitmap_len = (uint16_t)((n_topics+7u)/8u);
    uint32_t name_bytes = 0; char *name_pool = NULL;
    DartTransportState *st = (DartTransportState*)i_dart_bump_take(b, sizeof(DartTransportState), 16);
    if (st && b->base) memset(st, 0, sizeof(*st));

    /* name pool: one fixed-size slot per topic so a reserve-mode slot can be named
       later by dart_transport_topic_define without repacking. topic->name points at its slot. */
    name_bytes = (uint32_t)n_topics * (DART_TOPIC_NAME_MAX + 1u);

    { uint32_t nlanes = n_topics*max_peers, ndest = max_peers;
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
      i_DartTopic *topic = (i_DartTopic*)i_dart_bump_take(b, n_topics*sizeof(i_DartTopic), 16);
      /* lanes: only the u16 ticket table lives in the arena; records are pool-allocated
         per real match, so lane memory scales with matches */
      uint16_t   *lane_index = (uint16_t*)i_dart_bump_take(b, (size_t)nlanes*sizeof(uint16_t), 2);
      uint32_t *dest_head = (uint32_t*)i_dart_bump_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint32_t *dest_tail = (uint32_t*)i_dart_bump_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint8_t  *dest_queued = (uint8_t*) i_dart_bump_take(b, (size_t)ndest, 1);
      uint32_t *dest_queue = (uint32_t*)i_dart_bump_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      /* index maps: per-peer pointer + length; each map allocated on demand at the
         peer's advertised size */
      uint16_t **peer_index    = (uint16_t**)i_dart_bump_take(b, (size_t)max_peers*sizeof(uint16_t*), 8);
      uint32_t *peer_index_len = (uint32_t*) i_dart_bump_take(b, (size_t)max_peers*sizeof(uint32_t), 8);
      uint8_t **peer_astate    = (uint8_t**) i_dart_bump_take(b, (size_t)max_peers*sizeof(uint8_t*), 8);
      name_pool = (char*)i_dart_bump_take(b, name_bytes ? name_bytes : 1u, 1);
      if (st && b->base){
          st->cfg=*cfg; st->peer_ids=peer_ids; st->peer_used=peer_used;
          st->peer_dormant=peer_dormant; st->peer_frag=peer_frag;
          st->frag = dart_clamp_frag(cfg->frag_size);
          st->peer_pub_bitmap=peer_pub_bitmap; st->peer_sub_bitmap=peer_sub_bitmap;
          st->peer_sub_reliable=peer_sub_reliable; st->bitmap_len=bitmap_len;
          st->topics=topic; st->reader_epoch_counter=1;
          st->next_deadline_us=DART__NO_DEADLINE;
          st->lanes=NULL; st->lane_cap=0u; st->lane_free=DART__NIL;
          st->lane_index=lane_index;
          st->dest_head=dest_head; st->dest_tail=dest_tail; st->dest_queued=dest_queued; st->dest_queue=dest_queue;
          st->peer_index=peer_index; st->peer_index_len=peer_index_len;
          st->peer_astate=peer_astate;
          memset(peer_used,0,max_peers); memset(peer_dormant,0,max_peers);
          { uint32_t k; for (k=0;k<max_peers;k++) peer_frag[k]=DART_FRAG_SIZE; }  /* set per peer on add */
#ifdef DART_SHM
          st->peer_shm=peer_shm; memset(peer_shm,0,max_peers);
#endif
          memset(peer_index, 0, (size_t)max_peers*sizeof(uint16_t*));
          memset(peer_index_len, 0, (size_t)max_peers*sizeof(uint32_t));
          memset(peer_astate, 0, (size_t)max_peers*sizeof(uint8_t*));
          memset(peer_pub_bitmap,0,(size_t)max_peers*bitmap_len); memset(peer_sub_bitmap,0,(size_t)max_peers*bitmap_len);
          memset(peer_sub_reliable,0,(size_t)max_peers*bitmap_len);
          memset(lane_index,0xFF,(size_t)nlanes*sizeof(uint16_t));   /* all unmatched */
          memset(dest_queued,0,ndest);
          memset(dest_head,0xFF,(size_t)ndest*sizeof(uint32_t));   /* all DART__NIL */
      }
    }

    for (c=0;c<n_topics;c++){
        /* every slot starts inactive with its own name-pool slot; reserve-mode slots
           stay this way until dart_transport_topic_define fills them. */
        if (st && b->base){
            i_DartTopic *topic = &st->topics[c];
            memset(topic,0,sizeof(*topic));
            topic->role = DART_INACTIVE;
            topic->lane_head = DART__NIL;
            topic->name = name_pool + (size_t)c*(DART_TOPIC_NAME_MAX + 1u);
            ((char*)topic->name)[0] = '\0';
        }
        if (!cfg->topics) continue;    /* reserve mode: slots filled by topic_define later */
        {   const DartTopicDef *def = &cfg->topics[c];
            DartQos q = def->qos;            /* local, normalized copy */
            i_DartWriterSample *history; uint16_t depth;
            i_dart_qos_defaults(&q);
            depth = q.keep_last;
            history = (i_DartWriterSample*)i_dart_bump_take(b, depth*sizeof(i_DartWriterSample), 16);
            if (st && b->base){
                i_DartTopic *topic = &st->topics[c];
                size_t lane = i_dart_name_len(def->name);
                topic->qos=q;
                topic->role=def->role;
                topic->kind=def->kind; topic->prefix_bytes=def->prefix_bytes; topic->directed=def->directed;
                topic->forceable=def->forceable;
                topic->identity = dart_topic_identity(def);
                if (lane){ memcpy((char*)topic->name, def->name, lane); ((char*)topic->name)[lane]='\0'; }
                topic->name_len = (uint8_t)lane;
                topic->history=history; topic->history_owned=0; topic->history_head=0; topic->next_seqno=0; topic->have_first=0;
                memset(history,0,depth*sizeof(i_DartWriterSample));   /* buf/cap grow via the hook on first use */
            }
        }
    }
    return st;
}


size_t dart_transport_required_memory(const DartConfig *cfg){
    i_DartBump b; memset(&b,0,sizeof b);
    if (!cfg || cfg->n_topics==0 || cfg->max_peers==0) return 0;
    i_dart_transport_build(&b, cfg);
    return b.offset + 16;   /* slack for base alignment */
}


DartTransportState *dart_transport_init(void *mem, size_t cap, const DartConfig *cfg){
    i_DartBump b; DartTransportState *st; uint16_t i;
    if (!mem || !cfg || cfg->n_topics==0 || cfg->max_peers==0) return NULL;
    if (!cfg->allocator) return NULL;    /* the allocator is the one memory model (see core.h) */
    if (cfg->topics) for (i=0;i<cfg->n_topics;i++){
        const DartTopicDef *d = &cfg->topics[i];
        size_t lane = 0; uint16_t j;
        if (!d->name || !d->name[0]) return NULL;          /* name = identity, required */
        while (d->name[lane]) lane++;
        if (lane > DART_TOPIC_NAME_MAX) return NULL;           /* the wire name is the whole name */
        if (d->directed && d->qos.catch_up) return NULL;   /* directed history never replays: a late
                                                              joiner would receive other peers' samples */
        for (j=0;j<i;j++)   /* same name under a different kind: the alias maps bind a peer's
                               entry to ONE local topic by identity, so cross-kind twins on one
                               node would cross-bind; coexistence is a cross-node property */
            if (cfg->topics[j].kind != d->kind &&
                dart_topic_id(cfg->topics[j].name) == dart_topic_id(d->name)) return NULL;
    }
    memset(&b,0,sizeof b);
    b.base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    b.cap  = cap - (size_t)((uint8_t*)b.base - (uint8_t*)mem);
    st = i_dart_transport_build(&b, cfg);
    if (!st || b.oom) return NULL;
    st->cfg.topics = NULL;   /* only read during init; detach the caller's pointer */
    return st;
}


/* Relocate a live transport into a bigger block at grown counts (dynamic-mode growth).
 * Heap buffers (history rings, sample/assembly bufs, frag bitmaps) are NOT in the arena,
 * so the struct copies carry their pointers across and the OLD arena can be freed without
 * touching them. The 2D tables are re-strided into the new max_peers/n_topics; the
 * active-lane scheduler (indices encode the old strides) is dropped and rebuilt from the
 * proxy state. The caller frees old's arena block afterward; it must NOT dart_transport_destroy old
 * (that would free the heap buffers now owned by the new state). Returns the new state. */
DartTransportState *dart_transport_migrate(DartTransportState *old, void *new_mem, size_t new_cap,
                        uint16_t new_max_peers, uint16_t new_n_topics){
    DartConfig nc; DartTransportState *nw; uint16_t omp, onc, c, p;
    if (!old) return NULL;
    nc = old->cfg; nc.topics = NULL;
    nc.max_peers = new_max_peers; nc.n_topics = new_n_topics;
    nw = dart_transport_init(new_mem, new_cap, &nc);
    if (!nw) return NULL;
    omp = old->cfg.max_peers; onc = old->cfg.n_topics;

    nw->reader_epoch_counter = old->reader_epoch_counter;
    nw->frag = old->frag;
    memcpy(nw->peer_ids,     old->peer_ids,     (size_t)omp*sizeof(uint32_t));
    memcpy(nw->peer_used,    old->peer_used,    omp);
    memcpy(nw->peer_dormant, old->peer_dormant, omp);
    memcpy(nw->peer_frag,    old->peer_frag,    (size_t)omp*sizeof(uint16_t));
#ifdef DART_SHM
    memcpy(nw->peer_shm,     old->peer_shm,     omp);
#endif
    /* topics: keep the new name-pool slot pointer, carry everything else (incl. the
       heap history ring pointer) and re-copy the name string into the new pool */
    for (c=0;c<onc;c++){
        char *nm = (char*)nw->topics[c].name;
        size_t l = old->topics[c].name_len;
        nw->topics[c] = old->topics[c];   /* struct copy carries name_len */
        nw->topics[c].name = nm;
        if (l) memcpy(nm, old->topics[c].name, l);
        nm[l] = '\0';
    }
    /* lane records: the pool is ONE hook allocation outside both arenas, so adopt it
       wholesale (record backrefs use topic indices + peer slots, both preserved; the
       topics' lane_head chains were carried by the struct copies above). Only the
       ticket table is arena memory: re-stride it into the new max_peers. */
    nw->lanes = old->lanes; nw->lane_cap = old->lane_cap; nw->lane_free = old->lane_free;
    for (c=0;c<onc;c++) for (p=0;p<omp;p++)
        nw->lane_index[(size_t)c*new_max_peers+p] = old->lane_index[(size_t)c*omp+p];
    {   uint32_t li;   /* the old scheduler dies with the old arena: clear per-record state
                          (free-list records keep sched_next: it is their free link) */
        for (li=0; li<nw->lane_cap; li++)
            if (nw->lanes[li].in_use){ nw->lanes[li].queued=0; nw->lanes[li].sched_next=DART__NIL; }
    }
    /* per-peer interest bitmaps (stride grows with n_topics) + index table */
    for (p=0;p<omp;p++){
        memcpy(nw->peer_pub_bitmap + (size_t)p*nw->bitmap_len,
               old->peer_pub_bitmap + (size_t)p*old->bitmap_len, old->bitmap_len);
        memcpy(nw->peer_sub_bitmap + (size_t)p*nw->bitmap_len,
               old->peer_sub_bitmap + (size_t)p*old->bitmap_len, old->bitmap_len);
        memcpy(nw->peer_sub_reliable + (size_t)p*nw->bitmap_len,
               old->peer_sub_reliable + (size_t)p*old->bitmap_len, old->bitmap_len);
        nw->peer_index[p]     = old->peer_index[p];   /* hook allocations: stable across the move */
        nw->peer_index_len[p] = old->peer_index_len[p];
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

/* the local handle IS the topic's index; out-of-range rejected */
i_DartTopic *i_dart_topic_at(DartTransportState *st, uint16_t topic_index, int *idx_out){
    if (topic_index >= st->cfg.n_topics) return NULL;
    if (idx_out) *idx_out = (int)topic_index;
    return &st->topics[topic_index];
}

/* Find the local topic for a wire identity. An INACTIVE topic (declared but off) must
 * not shadow an active same-identity topic, so prefer a non-INACTIVE match; fall back to
 * the first match (e.g. all inactive) so resolution stays deterministic. Lets a caller hold
 * two topics of one identity (different QoS) and switch which is live by role. */
static i_DartTopic *i_dart_topic_by_identity(DartTransportState *st, uint64_t identity, int *idx_out){
    uint16_t i; int first=-1;
    for (i=0;i<st->cfg.n_topics;i++){
        if (st->topics[i].identity!=identity) continue;
        if (first<0) first=(int)i;
        if (st->topics[i].role!=DART_INACTIVE){ if(idx_out)*idx_out=(int)i; return &st->topics[i]; }
    }
    if (first>=0){ if(idx_out)*idx_out=first; return &st->topics[first]; }
    return NULL;
}


/* fire one DartTransportEvent (no-op if no on_event). first/count are the kind's two
 * numeric slots; route them to named fields. A topic name is not carried: a consumer
 * reads it with dart_transport_topic_name(st, ev.topic). */
void i_dart_transport_fire_event(DartTransportState *st, DartTransportEventKind kind, uint16_t topic_index,
                        uint32_t peer, uint64_t first, uint64_t count){
    DartTransportEvent ev;
    if (!st->cfg.on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind=kind; ev.topic=topic_index; ev.peer=peer; ev.user=st->cfg.user;
    switch (kind){
    case DART_TRANSPORT_MSG_LOST:       ev.lost_first = first; ev.lost_count = count; break;
    case DART_TRANSPORT_MSG_TOO_BIG:    ev.too_big_bytes = count; break;
    case DART_TRANSPORT_NAME_COLLISION: ev.identity = first; break;
    case DART_TRANSPORT_SCHEMA_MISMATCH: ev.peer_is_pub = (uint8_t)first; break;
    default: break;
    }
    st->cfg.on_event(&ev);
}

/* dart_event_str (and its bounded appenders) moved to the node (node/core.c): the
   formatter covers the node's app-facing DartEvent union, not the transport's own
   events. The transport stays independent of the node's event vocabulary. */


/* unicast join seqno: head minus qos.catch_up cached samples (reliable only) */
uint64_t i_dart_topic_unicast_join_seqno(const i_DartTopic *topic){
    uint16_t depth = topic->qos.keep_last;   /* normalized at init (>=1) */
    uint16_t want = topic->qos.catch_up, k, i;
    uint64_t s = topic->next_seqno;
    if (topic->qos.reliability != DART_RELIABLE || want == 0) return s;
    if (want > depth) want = depth;
    i = topic->history_head;
    for (k=0; k<want; k++){
        uint16_t j = (uint16_t)(i ? i-1 : depth-1);
        if (!topic->history[j].valid) break;        /* fewer than want cached */
        s = topic->history[j].base;
        i = j;
    }
    return s;
}


/* Get-or-allocate the lane record for (c,peer_slot). The pool grows by doubling
 * through the hook; records MOVE on growth, so callers re-derive any lane pointer
 * after this call. Returns NULL on OOM or a full u16 ticket space: the match is
 * refused for now (the peer's next announce re-applies and retries). */
static i_DartLane *i_dart_lane_ensure(DartTransportState *st, uint16_t c, uint32_t peer_slot){
    size_t k = (size_t)c*st->cfg.max_peers + peer_slot;
    uint32_t li;
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
    st->lanes[li].topic = c; st->lanes[li].peer_slot = (uint16_t)peer_slot;
    st->lanes[li].sched_next = DART__NIL; st->lanes[li].topic_next = DART__NIL;
    st->lanes[li].in_use = 1;
    st->lane_index[k] = (uint16_t)li;
    return &st->lanes[li];
}

/* A lane with neither side matched leaves the topic chain; its grown reassembly
 * buffers are freed, any scheduler entry dropped (a recycled record must never sit
 * on another peer's dest list), and the record recycled. No-op while a side is matched. */
static void i_dart_lane_release(DartTransportState *st, uint16_t c, uint32_t peer_slot){
    uint32_t li = i_dart_lane_id(st, c, peer_slot);
    i_DartLane *l;
    if (li == DART__NIL) return;
    l = &st->lanes[li];
    if (l->w.used || l->r.used) return;
    {   uint32_t *pp = &st->topics[c].lane_head;    /* unlink from the topic chain */
        while (*pp != DART__NIL && *pp != li) pp = &st->lanes[*pp].topic_next;
        if (*pp == li) *pp = l->topic_next;
    }
    l->topic_next = DART__NIL;
    if (l->r.assembly_buf){ st->cfg.allocator(st->cfg.user, l->r.assembly_buf, 0); l->r.assembly_buf=NULL; l->r.assembly_cap=0; }
    if (l->r.frag_bitmap){ st->cfg.allocator(st->cfg.user, l->r.frag_bitmap, 0); l->r.frag_bitmap=NULL; l->r.bitmap_cap=0; }
    i_dart_sched_drop(st, li);
    l->in_use = 0;
    l->sched_next = st->lane_free; st->lane_free = li;
    st->lane_index[(size_t)c*st->cfg.max_peers + peer_slot] = 0xFFFF;
}

/* match one (topic,peer) lane side: it carries new data, repairs, acks/HB */
static void i_dart_writer_match(DartTransportState *st, uint16_t c, uint16_t peer_slot, i_DartLane *l){
    i_DartTopic *topic=&st->topics[c];
    i_DartWriterProxy *w=&l->w;
    memset(w,0,sizeof(*w));
    w->used=1;
    topic->matched_writers++;   /* only reached on a genuine 0->1 (rematch guards on !used) */
    /* only a reader that advertised RELIABLE acks; a best-effort reader stays out of
       flow control so it can't stall this writer (it gets new data, never repairs/HB) */
    w->reader_reliable = i_dart_bit_get(&st->peer_sub_reliable[(size_t)peer_slot*st->bitmap_len], c) ? 1u : 0u;
    w->sent_upto = i_dart_topic_unicast_join_seqno(topic);
    w->acked_upto = w->sent_upto;
    w->wire_skip = w->sent_upto;   /* fire-and-forget wire seqno starts at 0 (see i_DartWriterProxy) */
    i_dart_lane_wake(st, c, peer_slot);   /* lane primed for new data + ack/hb */
}

static void i_dart_writer_unmatch(DartTransportState *st, uint16_t c, i_DartLane *l){
    if (!l->w.used) return;
    l->w.used=0;
    st->topics[c].matched_writers--;   /* guarded on used above: exactly one 1->0 per unmatch */
}

static void i_dart_reader_match(DartTransportState *st, uint16_t c, uint16_t peer_slot, i_DartLane *l){
    i_DartReaderProxy *r=&l->r;
    uint8_t *assembly_buf=r->assembly_buf, *frag_bitmap=r->frag_bitmap;
    uint32_t assembly_cap=r->assembly_cap, bitmap_cap=r->bitmap_cap;   /* keep grown buffers across rematch */
    memset(r,0,sizeof(*r));
    r->assembly_buf=assembly_buf; r->frag_bitmap=frag_bitmap; r->assembly_cap=assembly_cap; r->bitmap_cap=bitmap_cap;
    r->epoch=st->reader_epoch_counter++;   /* new incarnation: writers re-join on seeing it */
    r->used=1;       /* started==0: first DATA adopts the writer's position */
    st->topics[c].matched_readers++;   /* only reached on a genuine 0->1 (rematch guards on !used) */
    /* announce this incarnation once so a caught-up (idle, non-pinging) writer
       re-joins and replays. A genuine discovery blip keeps its position through
       dart_transport_peer_dormant/resume and never lands here, so a single ACKNACK suffices. */
    if (st->topics[c].qos.reliability==DART_RELIABLE){
        r->ack_pending=1; r->ack_due_us=0; r->ack_force=1;
        i_dart_lane_wake(st,c,peer_slot);
    }
}

static void i_dart_reader_unmatch(DartTransportState *st, uint16_t c, i_DartLane *l){
    if (l->r.used) st->topics[c].matched_readers--;   /* peer_remove calls this unconditionally */
    l->r.used=0; l->r.assembly_active=0;
}


/* recompute one (topic,peer) match from our role and the peer's interest bits.
 * Lane records exist only while a side is matched: the unmatched->matched edge allocates
 * (and links the topic chain), the matched->unmatched edge releases. Idempotent
 * re-application never touches a lane whose match state did not change, so reader
 * positions survive it exactly as before. */
static void i_dart_topic_rematch(DartTransportState *st, uint16_t c, uint16_t peer_slot){
    i_DartTopic *topic=&st->topics[c];
    const uint8_t *peer_pub_bitmap=&st->peer_pub_bitmap[(size_t)peer_slot*st->bitmap_len];
    const uint8_t *peer_sub_bitmap=&st->peer_sub_bitmap[(size_t)peer_slot*st->bitmap_len];
    int wuse = (topic->role==DART_PUBSUB || topic->role==DART_PUB_ONLY) && i_dart_bit_get(peer_sub_bitmap,c);
    int ruse = (topic->role==DART_PUBSUB || topic->role==DART_SUB_ONLY) && i_dart_bit_get(peer_pub_bitmap,c);
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
    if (!had && (l->w.used || l->r.used)){        /* first match on this lane: onto the topic chain */
        l->topic_next = topic->lane_head;
        topic->lane_head = i_dart_lane_id(st,c,peer_slot);
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
    if (st->peer_index[free] && st->peer_index_len[free]){   /* slot reuse: no stale mappings/verdicts */
        memset(st->peer_index[free],0xFF,(size_t)st->peer_index_len[free]*sizeof(uint16_t));
        if (st->peer_astate[free]) memset(st->peer_astate[free],0,st->peer_index_len[free]);
    }
    /* nothing matches until dart_transport_apply_peer_interest feeds the peer's interest
       list (carried in its discovery announce) */
}


void dart_transport_peer_remove(DartTransportState *st, uint32_t id){
    int s = i_dart_peer_slot(st,id); uint16_t c;
    if (s<0) return;
    for (c=0;c<st->cfg.n_topics;c++){
        i_DartLane *l = i_dart_lane_at(st,c,(uint32_t)s);
        if (!l) continue;
        i_dart_writer_unmatch(st,c,l);
        i_dart_reader_unmatch(st,c,l);
        /* release recycles the record AND frees the lane's grown reassembly buffers, so a
           gone peer keeps no per-lane memory at all */
        i_dart_lane_release(st,c,(uint32_t)s);
    }
    if (st->peer_index[s]){                        /* the index + verdict maps go too */
        st->cfg.allocator(st->cfg.user, st->peer_index[s], 0);
        if (st->peer_astate[s]) st->cfg.allocator(st->cfg.user, st->peer_astate[s], 0);
        st->peer_index[s]=NULL; st->peer_astate[s]=NULL; st->peer_index_len[s]=0;
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
    for (c=0;c<st->cfg.n_topics;c++){
        i_DartLane *l=i_dart_lane_at(st,c,(uint32_t)s);
        if (!l) continue;
        if (st->topics[c].qos.reliability!=DART_RELIABLE) continue;
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
    if (!st) return;
    for (c=0;c<st->cfg.n_topics;c++){
        i_DartTopic *topic=&st->topics[c];     /* an undefined reserve slot: depth 0, no ring */
        uint16_t depth, d;
        depth = topic->qos.keep_last;
        for (d=0; d<depth; d++)
            if (topic->history[d].buf){ st->cfg.allocator(st->cfg.user, topic->history[d].buf, 0);
                                  topic->history[d].buf=NULL; topic->history[d].cap=0; }
        if (topic->history_owned && topic->history){   /* ring allocated by dart_transport_topic_define */
            st->cfg.allocator(st->cfg.user, topic->history, 0);
            topic->history=NULL; topic->history_owned=0;
        }
    }
    for (li=0; li<st->lane_cap; li++){           /* live records' grown reassembly buffers */
        i_DartLane *l=&st->lanes[li];
        if (!l->in_use) continue;
        if (l->r.assembly_buf){ st->cfg.allocator(st->cfg.user, l->r.assembly_buf, 0); l->r.assembly_buf=NULL; l->r.assembly_cap=0; }
        if (l->r.frag_bitmap){ st->cfg.allocator(st->cfg.user, l->r.frag_bitmap, 0); l->r.frag_bitmap=NULL; l->r.bitmap_cap=0; }
    }
    if (st->lanes){                              /* the record pool itself (one hook allocation) */
        st->cfg.allocator(st->cfg.user, st->lanes, 0);
        st->lanes=NULL; st->lane_cap=0; st->lane_free=DART__NIL;
    }
    {   uint32_t p;                              /* per-peer index + verdict maps (hook allocations) */
        for (p=0;p<st->cfg.max_peers;p++){
            if (st->peer_index[p]){
                st->cfg.allocator(st->cfg.user, st->peer_index[p], 0);
                st->peer_index[p]=NULL; st->peer_index_len[p]=0;
            }
            if (st->peer_astate[p]){
                st->cfg.allocator(st->cfg.user, st->peer_astate[p], 0);
                st->peer_astate[p]=NULL;
            }
        }
    }
}


/* The peer's index + verdict maps, guaranteed to cover `need` entries: the existing
 * maps, or grown/new hook allocations (the grown tail starts unmapped, verdicts
 * cleared), or NULL on OOM: the caller then counts the entries as unmappable. Sized
 * once per apply to the peer's whole positional list, so any advertised index is
 * covered. */
static uint16_t *i_dart_peer_index_ensure(DartTransportState *st, int peer_slot, uint32_t need){
    uint32_t have = st->peer_index_len[peer_slot];
    uint16_t *nm; uint8_t *ns;
    if (need <= have) return st->peer_index[peer_slot];
    nm = (uint16_t*)st->cfg.allocator(st->cfg.user, st->peer_index[peer_slot], (size_t)need*sizeof(uint16_t));
    if (!nm) return NULL;
    st->peer_index[peer_slot] = nm;                     /* len not yet raised: retryable on OOM below */
    ns = (uint8_t*)st->cfg.allocator(st->cfg.user, st->peer_astate[peer_slot], (size_t)need);
    if (!ns) return NULL;
    memset(nm + have, 0xFF, (size_t)(need-have)*sizeof(uint16_t));   /* grown tail: unmapped */
    memset(ns + have, 0, (size_t)(need-have));                       /* ...no verdicts */
    st->peer_astate[peer_slot] = ns; st->peer_index_len[peer_slot] = need;
    return nm;
}

/* Candidate topics for a 32-bit interest hash: the count of local topics whose
 * identity's low 32 bits match, and the preferred one (non-INACTIVE first, mirroring
 * i_dart_topic_by_identity). Same linear resolution the identity lookup pays. */
static int i_dart_hash32_candidates(DartTransportState *st, uint32_t h, int *idx_out){
    uint16_t i; int first=-1, live=-1, n=0;
    for (i=0;i<st->cfg.n_topics;i++){
        if (st->topics[i].name_len==0 || (uint32_t)st->topics[i].identity != h) continue;
        n++;
        if (first<0) first=(int)i;
        if (live<0 && st->topics[i].role!=DART_INACTIVE) live=(int)i;
    }
    if (idx_out) *idx_out = live>=0 ? live : first;
    return n;
}


/* Upper bound on dart_transport_build_interest output, for sizing the announce buffer:
 * one positional [u32 hash][u8 flags] entry per topic slot, plus the worst-case rate
 * section (every topic could advertise a max_rate_hz: [u16 n_rates] + 4 B per entry). */
size_t dart_interest_max(uint16_t n_topics){
    return 2u + 5u * (size_t)n_topics + 2u + 4u * (size_t)n_topics;
}

/* subscriber topics carrying a best-effort rate cap: the count for the interest rate
 * section (build + meta_size must agree byte for byte, so both go through here). n =
 * the highest defined slot + 1 (the entry count), so this matches the entry walk. */
static uint16_t i_dart_rate_count(DartTransportState *st, uint16_t n){
    uint16_t c, r=0;
    for (c=0;c<n;c++){
        const i_DartTopic *t=&st->topics[c];
        if (t->name_len && t->qos.max_rate_hz
            && (t->role==DART_SUB_ONLY || t->role==DART_PUBSUB)) r++;
    }
    return r;
}


/* Serialize our interest into out: [u16 n], then one [u32 hash][u8 flags] entry per
 * topic IN INDEX ORDER up to the highest defined slot (position = index; an undefined
 * reserve slot rides as an INACTIVE hole so later indices stay stable), then a SPARSE
 * rate section [u16 n_rates][(u16 topic_index)(u16 rate_hz)]* for the subscriber topics
 * that cap their best-effort delivery rate (see DartQos.max_rate_hz). Returns bytes
 * written, or 0 if cap is too small; size out via dart_interest_max. */
size_t dart_transport_build_interest(DartTransportState *st, void *out, size_t cap){
    uint8_t *o=(uint8_t*)out, *rp;
    uint16_t c, n=0, n_rates;
    for (c=0;c<st->cfg.n_topics;c++) if (st->topics[c].name_len) n=(uint16_t)(c+1u);
    n_rates = i_dart_rate_count(st, n);
    if (cap < 2u + 5u*(size_t)n + 2u + 4u*(size_t)n_rates) return 0;
    i_dart_le_w16(o, n);
    for (c=0;c<n;c++){
        const i_DartTopic *topic = &st->topics[c];
        uint8_t *e = o + 2u + 5u*(size_t)c;
        uint8_t role = topic->name_len ? topic->role : (uint8_t)DART_INACTIVE;
        i_dart_le_w32(e, topic->name_len ? (uint32_t)topic->identity : 0u);
        e[4] = (uint8_t)((role & DART__INT_ROLE_MASK)
             | (topic->qos.reliability==DART_RELIABLE ? DART__INT_RELIABLE : 0u)
             | (topic->name_len ? ((topic->kind << DART__INT_KIND_SHIFT) & DART__INT_KIND_MASK) : 0u)
             | (topic->name_len && topic->forceable ? DART__INT_FORCEABLE : 0u));
    }
    rp = o + 2u + 5u*(size_t)n;
    i_dart_le_w16(rp, n_rates); rp += 2u;
    for (c=0;c<n;c++){
        const i_DartTopic *topic = &st->topics[c];
        if (topic->name_len && topic->qos.max_rate_hz
            && (topic->role==DART_SUB_ONLY || topic->role==DART_PUBSUB)){
            i_dart_le_w16(rp, c); i_dart_le_w16(rp+2, topic->qos.max_rate_hz); rp += 4u;
        }
    }
    return (size_t)(rp - o);
}


/* A peer's interest list arrived (from its discovery announce): re-derive its bits from
 * the cached per-index VERDICTS + the entry's current flags, then rematch every topic.
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
    amap   = n ? i_dart_peer_index_ensure(st, peer_slot, n) : NULL;
    astate = amap ? st->peer_astate[peer_slot] : NULL;
    for (a=0;a<n;a++){
        const uint8_t *e = d + 2u + 5u*a;
        uint8_t flags = e[4], role = (uint8_t)(flags & DART__INT_ROLE_MASK);
        int their_pub, their_sub, rel;
        uint16_t cidx; i_DartTopic *topic;
        if (role == DART_INACTIVE) continue;
        if (!astate || a >= st->peer_index_len[peer_slot]){
            /* no verdict storage (the map allocation failed): this entry can never
               verify or demux here; count it if it would have been a candidate */
            if (i_dart_hash32_candidates(st, i_dart_le_r32(e), NULL)) unmappable++;
            continue;
        }
        if (!(astate[a] & DART__AST_DETAILED) || !(astate[a] & DART__AST_NAME_OK))
            continue;                     /* PENDING (details on their way) or a verified non-match */
        cidx = amap[a];
        if (cidx >= st->cfg.n_topics) continue;      /* defensive: stale map */
        topic = &st->topics[cidx];
        if (topic->role == DART_INACTIVE){
            /* dual same-identity topics switched by role: the verdict (incl. its schema
               gates) bound the then-live twin, so re-verify against the one live now */
            int resolved_index = (int)cidx;
            i_dart_topic_by_identity(st, topic->identity, &resolved_index);
            if ((uint16_t)resolved_index != cidx){
                astate[a] = 0; amap[a] = 0xFFFFu;      /* pending again; wants() re-asks */
                continue;
            }
        }
        {   /* entity-kind gate: a name-verified peer that advertises this name under a
               different kind is a disjoint entity (a plain topic vs a function, etc.). Refuse
               the pairing (no proxy, never cross-wired) and surface it, like QOS_INCOMPATIBLE. */
            uint8_t their_kind = (uint8_t)((flags & DART__INT_KIND_MASK) >> DART__INT_KIND_SHIFT);
            if (their_kind != topic->kind){
                i_dart_transport_fire_event(st, DART_TRANSPORT_KIND_MISMATCH, cidx, peer_id, 0, 0);
                continue;
            }
        }
        their_pub = (role==DART_PUBSUB || role==DART_PUB_ONLY);
        their_sub = (role==DART_PUBSUB || role==DART_SUB_ONLY);
        rel       = (flags & DART__INT_RELIABLE) != 0;
        if (their_pub){                                /* their offered QoS vs our subscription */
            int ours_sub = (topic->role==DART_PUBSUB || topic->role==DART_SUB_ONLY);
            if (ours_sub && topic->qos.reliability==DART_RELIABLE && !rel){
                i_dart_transport_fire_event(st, DART_TRANSPORT_QOS_INCOMPATIBLE, cidx, peer_id, 0, 0);
            } else if (!(astate[a] & DART__AST_READ_OK)){
                i_dart_transport_fire_event(st, DART_TRANSPORT_SCHEMA_MISMATCH, cidx, peer_id, 1, 0);
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
    for (c=0;c<st->cfg.n_topics;c++) i_dart_topic_rematch(st,c,(uint16_t)peer_slot);

    /* rate section (after the n entries): [u16 n_rates][(u16 their_index)(u16 rate_hz)]*.
       Applied AFTER rematch so the writer lanes exist; a fire-and-forget lane (best-effort,
       so w->used with the sub bit) then paces its sends. Re-applied every announce (the rate
       is in the blob, immutable per topic), so a lane re-formed by a role flip re-derives it. */
    if (amap){
        const uint8_t *rp = d + 2u + 5u*(size_t)n;
        if (blob.len >= (size_t)(rp - d) + 2u){
            uint16_t nr = i_dart_le_r16(rp), k; rp += 2u;
            for (k=0;k<nr;k++){
                uint16_t their_idx, rate_hz, cidx; i_DartWriterProxy *w;
                if ((size_t)(rp - d) + 4u > blob.len) break;   /* truncated: stop */
                their_idx = i_dart_le_r16(rp); rate_hz = i_dart_le_r16(rp+2); rp += 4u;
                if (their_idx >= st->peer_index_len[peer_slot]) continue;
                cidx = amap[their_idx];
                if (cidx >= st->cfg.n_topics) continue;         /* unverified/unmapped */
                w = i_dart_writer_proxy_at(st, cidx, (uint32_t)peer_slot);
                if (w && w->used)
                    w->rate_interval_us = rate_hz ? (1000000u / (uint32_t)rate_hz) : 0u;
            }
        }
    }
}


/* diagnostic: how many topics we now publish to / receive from this peer (unicast
 * lanes). Surfaced on DART_PEER_INTEREST so a caller can see a match form (or not). */
void dart_transport_peer_match_counts(DartTransportState *st, uint32_t peer_id,
                            uint16_t *publish_to, uint16_t *receive_from){
    int s; uint16_t c, w=0, r=0;
    if (publish_to)   *publish_to   = 0;
    if (receive_from) *receive_from = 0;
    if (!st) return;
    s = i_dart_peer_slot(st, peer_id);
    if (s < 0) return;
    for (c=0;c<st->cfg.n_topics;c++){
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
#define DART__META_BASE_NOSHM 6u    /* 'D','N',ver, frag_lo, frag_hi, iflags */
#define DART__META_BASE_SHM   23u   /* 'D','N',ver, frag_lo, frag_hi, shm, host[16], iflags */
#define DART__META_IFLAG_EXTERNAL 0x01u   /* iflags bit 0: interest not inlined, pull via uDTL */
#ifdef DART_SHM
#define DART__META_VER  13u                 /* what WE write */
#define DART__META_BASE DART__META_BASE_SHM
#else
#define DART__META_VER  12u
#define DART__META_BASE DART__META_BASE_NOSHM
#endif

static int i_dart_meta_ok(DartBytes meta){
    return meta.data && meta.len >= DART__META_BASE_NOSHM
        && meta.data[0]=='D' && meta.data[1]=='N'
        && meta.data[2]>=12 && meta.data[2]<=13;
}
/* base prefix through the iflags byte, by version (odd v13 carries shm+host, even v12
 * doesn't). iflags is always the base's last byte. */
static uint16_t i_dart_meta_base(const uint8_t *meta){
    return (meta[2] & 1u) ? DART__META_BASE_SHM : DART__META_BASE_NOSHM;
}
uint16_t dart_meta_cap(uint16_t n_topics){
    size_t cap = (size_t)DART__META_BASE + dart_interest_max(n_topics);
    if (cap > 65000u) cap = 65000u;
    return (uint16_t)cap;
}

/* Exact bytes of our current interest blob (the total_len interest paging serves).
 * The same walk dart_transport_build_interest emits, byte for byte. */
uint32_t dart_transport_interest_size(DartTransportState *st){
    uint16_t c, n=0, n_rates;
    for (c=0;c<st->cfg.n_topics;c++) if (st->topics[c].name_len) n=(uint16_t)(c+1u);
    n_rates = i_dart_rate_count(st, n);
    return 2u + 5u*(uint32_t)n + 2u + 4u*(uint32_t)n_rates;
}

/* Exact overlay size the next INLINE dart_transport_meta_build will emit for the
 * current topic state, so a caller can size the buffer to the actual content instead
 * of dart_meta_cap's full-reserve worst case. */
uint16_t dart_transport_meta_size(DartTransportState *st){
    size_t len = (size_t)DART__META_BASE + dart_transport_interest_size(st);
    if (len > 65000u) len = 65000u;              /* the dart_meta_cap ceiling; past it the build truncates */
    return (uint16_t)len;
}

uint16_t dart_transport_meta_bootstrap_size(void){ return DART__META_BASE; }

uint16_t dart_transport_meta_build(DartTransportState *st, uint8_t *out, uint16_t cap,
                         uint16_t frag_size, int shm_capable, const uint8_t host[16],
                         int interest_external){
    size_t interest_len, len; uint16_t off = DART__META_BASE;
    out[0]='D'; out[1]='N'; out[2]=DART__META_VER;
    out[3]=(uint8_t)(frag_size & 0xFF); out[4]=(uint8_t)(frag_size >> 8);
#ifdef DART_SHM
    out[5]=(uint8_t)(shm_capable?1:0);
    if (host) memcpy(out+6, host, 16); else memset(out+6, 0, 16);
#else
    (void)shm_capable; (void)host;
#endif
    out[off-1] = interest_external ? DART__META_IFLAG_EXTERNAL : 0u;
    if (interest_external) return off;     /* bootstrap: locator-sized, always one datagram */
    interest_len = dart_transport_build_interest(st, out + off, cap - off);   /* no name here: that is discovery's */
    len = (size_t)off + interest_len;
    if (interest_len == 0)    /* did not fit (returns >= 2 even with zero topics): never silent */
        i_dart_transport_fire_event(st, DART_TRANSPORT_META_TRUNCATED_INTEREST, 0, 0, 0, 0);
    return (uint16_t)len;
}

uint16_t dart_meta_frag(DartBytes meta){
    if (!i_dart_meta_ok(meta)) return 0;
    return (uint16_t)(meta.data[3] | ((uint16_t)meta.data[4] << 8));
}

int dart_meta_interest_external(DartBytes meta){
    uint16_t base;
    if (!i_dart_meta_ok(meta)) return 0;
    base = i_dart_meta_base(meta.data);
    if (meta.len < base) return 0;
    return (meta.data[base-1] & DART__META_IFLAG_EXTERNAL) ? 1 : 0;
}

DartBytes dart_meta_interest(DartBytes meta){
    uint16_t off;
    if (!i_dart_meta_ok(meta)) return dart_bytes(NULL, 0);
    off = i_dart_meta_base(meta.data);     /* interest follows the base prefix (no name in the overlay) */
    if (meta.len < off) return dart_bytes(NULL, 0);
    if (meta.data[off-1] & DART__META_IFLAG_EXTERNAL)
        return dart_bytes(NULL, 0);        /* bootstrap: never misread as an empty interest list */
    return dart_bytes(meta.data + off, meta.len - off);
}

int dart_interest_next(DartBytes interest, DartInterestIter *it, DartTopicEntry *out){
    if (!it || !out) return 0;
    if (!it->started){                    /* first call: parse the [u16 n] header */
        it->started = 1; it->left = 0; it->index = 0; it->phase = 0; it->off = 0;
        if (!interest.data || interest.len < 2) return 0;   /* no/short interest list: nothing to yield */
        it->left = (uint16_t)(interest.data[0] | ((uint16_t)interest.data[1] << 8));
        it->off  = 2u;                    /* first entry, past n */
    }
    while (it->left){
        uint32_t off = it->off;
        uint8_t flags, role;
        if ((size_t)off + 5u > interest.len){ it->left = 0; return 0; }   /* truncated: stop */
        flags = interest.data[off + 4]; role = (uint8_t)(flags & DART__INT_ROLE_MASK);
        if (role == DART_INACTIVE){       /* declared-but-off / undefined hole: not advertised */
            it->left--; it->index++; it->off = off + 5u; it->phase = 0;
            continue;
        }
        out->index    = it->index;
        out->role     = role;
        out->reliable = (uint8_t)((flags & DART__INT_RELIABLE) ? 1 : 0);
        out->kind     = (uint8_t)((flags & DART__INT_KIND_MASK) >> DART__INT_KIND_SHIFT);
        out->forceable = (uint8_t)((flags & DART__INT_FORCEABLE) ? 1 : 0);
        out->hash     = i_dart_le_r32(interest.data + off);
        if (it->phase == 0 && (role==DART_PUBSUB || role==DART_PUB_ONLY)){
            out->is_pub = 1;
            if (role==DART_PUBSUB){ it->phase = 1; return 1; }   /* sub direction next call */
            it->left--; it->index++; it->off = off + 5u;
            return 1;
        }
        out->is_pub = 0;                  /* SUB_ONLY, or the second yield of a PUBSUB entry */
        it->phase = 0; it->left--; it->index++; it->off = off + 5u;
        return 1;
    }
    return 0;
}

int dart_meta_interest_next(DartBytes meta, DartInterestIter *it, DartTopicEntry *out){
    return dart_interest_next(dart_meta_interest(meta), it, out);
}

#ifdef DART_SHM
int dart_meta_shm(DartBytes meta, uint8_t host[16]){
    if (!i_dart_meta_ok(meta) || meta.data[2]!=13
        || meta.len < DART__META_BASE_SHM || !meta.data[5]) return 0;
    memcpy(host, meta.data+6, 16);
    return 1;
}
#endif


/* Pairwise detail exchange codec ('uDTL', see core.h for the layout and contract).
   Parsing is fully bounds-checked: a malformed request or response is dropped wholesale,
   never trusted. The responder side is a pure read of topic + schema state. */
#define DART__DETAIL_HDR 14u   /* magic(4) kind(1) ver(1) domain(2) meta_version(4) n(2) */
#define DART__DETAIL_VER 1u

static int i_dart_detail_hdr_ok(DartBytes d){
    return d.data && d.len >= DART__DETAIL_HDR
        && d.data[0]=='u' && d.data[1]=='D' && d.data[2]=='T' && d.data[3]=='L'
        && d.data[5]==DART__DETAIL_VER;
}

int dart_detail_kind(DartBytes dgram){
    if (!i_dart_detail_hdr_ok(dgram)) return 0;
    return (dgram.data[4]>=DART_DETAIL_REQ && dgram.data[4]<=DART_INTEREST_RESP)
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
        i_dart_le_w16(e, wants[k].index);
        i_dart_le_w64(e+2, wants[k].schema_hash);
    }
    return need;
}


/* Interest paging codec (uDTL kinds 3/4, see core.h): byte-range pages of the exact
   build_interest blob. Pure header build/parse; the responder's slicing and the
   requester's cursor live in the node core (a sans-IO caller runs its own). */
size_t dart_interest_req_build(uint16_t domain, uint32_t peer_meta_version,
                               uint32_t offset, void *out, size_t cap){
    uint8_t *o=(uint8_t*)out;
    if (!o || cap < (size_t)DART__DETAIL_HDR + 4u) return 0;
    i_dart_detail_hdr_write(o, DART_INTEREST_REQ, domain, peer_meta_version, 0);
    i_dart_le_w32(o + DART__DETAIL_HDR, offset);
    return (size_t)DART__DETAIL_HDR + 4u;
}

int dart_interest_req_offset(DartBytes dgram, uint32_t *offset){
    if (dart_detail_kind(dgram) != DART_INTEREST_REQ) return 0;
    if (dgram.len < (size_t)DART__DETAIL_HDR + 4u) return 0;
    if (offset) *offset = i_dart_le_r32(dgram.data + DART__DETAIL_HDR);
    return 1;
}

size_t dart_interest_resp_head(uint16_t domain, uint32_t meta_version, uint32_t total_len,
                               uint32_t offset, uint16_t chunk_len, void *out, size_t cap){
    uint8_t *o=(uint8_t*)out;
    if (!o || cap < (size_t)DART_INTEREST_RESP_HEAD) return 0;
    i_dart_detail_hdr_write(o, DART_INTEREST_RESP, domain, meta_version, 0);
    i_dart_le_w32(o + DART__DETAIL_HDR,      total_len);
    i_dart_le_w32(o + DART__DETAIL_HDR + 4u, offset);
    i_dart_le_w16(o + DART__DETAIL_HDR + 8u, chunk_len);
    return (size_t)DART_INTEREST_RESP_HEAD;
}

int dart_interest_resp_parse(DartBytes dgram, uint32_t *total_len, uint32_t *offset,
                             DartBytes *chunk){
    uint32_t total, off; uint16_t clen;
    if (dart_detail_kind(dgram) != DART_INTEREST_RESP) return 0;
    if (dgram.len < (size_t)DART_INTEREST_RESP_HEAD) return 0;
    total = i_dart_le_r32(dgram.data + DART__DETAIL_HDR);
    off   = i_dart_le_r32(dgram.data + DART__DETAIL_HDR + 4u);
    clen  = i_dart_le_r16(dgram.data + DART__DETAIL_HDR + 8u);
    if ((size_t)DART_INTEREST_RESP_HEAD + clen > dgram.len) return 0;   /* truncated: reject */
    if (off > total || (uint32_t)clen > total - off) return 0;          /* range out of the blob */
    if (total_len) *total_len = total;
    if (offset)    *offset    = off;
    if (chunk)     *chunk     = dart_bytes(dgram.data + DART_INTEREST_RESP_HEAD, clen);
    return 1;
}

/* One walk serves size and build (out NULL = measure), so the two agree byte for byte.
   A truncated build stops at an entry boundary: the response stays parseable and the
   requester re-requests the indices it still lacks (the paging seam). */
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
        uint16_t index    = i_dart_le_r16(r);
        uint64_t req_hash = i_dart_le_r64(r+2);
        const i_DartTopic *topic;
        uint64_t hash; DartBytes wire; size_t need;
        if (index >= st->cfg.n_topics) continue;             /* unknown: not advertised */
        topic = &st->topics[index];
        if (topic->role == DART_INACTIVE || topic->name_len == 0) continue;
        hash = schemas ? schemas[index].hash : 0;
        wire = dart_bytes(NULL, 0);
        if (hash && hash != req_hash && schemas[index].wire.len <= 0xFFFFu)
            wire = schemas[index].wire;    /* differs: inline for the subset check */
        need = 2u + 1u + topic->name_len + 8u + 2u + wire.len;
        /* One un-fragmented datagram per response: stop at cap, but ALWAYS include at least
           one entry (n_out>0 gate) so a lone entry larger than a datagram rides its own
           (fragmenting) page instead of wedging paging with an endless header-only reply.
           Measure (out==NULL, cap=DART_DGRAM_MAX) and build (cap=the measured size) apply
           the identical bound, so the sized buffer always holds exactly what is built. */
        if (n_out > 0 && len + need > cap) break;
        if (out){
            uint8_t *e = out + len;
            i_dart_le_w16(e, index);
            e[2] = topic->name_len;
            memcpy(e+3, topic->name, topic->name_len);
            i_dart_le_w64(e+3+topic->name_len, hash);
            i_dart_le_w16(e+3+topic->name_len+8, (uint16_t)wire.len);
            if (wire.len) memcpy(e+3+topic->name_len+10, wire.data, wire.len);
        }
        len += need;
        n_out++;
    }
    if (out) i_dart_le_w16(out+12, n_out);
    return len;
}

size_t dart_transport_detail_resp_size(DartTransportState *st, const DartMetaSchema *schemas,
                                       DartBytes req){
    /* ONE page (DART_DGRAM_MAX), so the response never IP-fragments; the requester pages
       the rest. A lone entry over the cap is still measured whole (force-first above). */
    return i_dart_detail_answer(st, schemas, 0, req, NULL, DART_DGRAM_MAX);
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
    out->index       = i_dart_le_r16(resp.data + off);
    out->name        = dart_string((const char*)(resp.data + off + 3u), nlen);
    out->schema_hash = i_dart_le_r64(resp.data + off + 3u + nlen);
    out->schema_wire = wlen ? dart_bytes(resp.data + off + 3u + nlen + 10u, wlen)
                            : dart_bytes(NULL, 0);
    it->off = off + 3u + nlen + 10u + wlen;
    it->left--;
    return 1;
}

/* The indices still PENDING for a peer: candidates (32-bit hash overlap + role overlap)
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
    astate = st->peer_astate[slot]; alen = st->peer_index_len[slot];
    for (a=0;a<n;a++){
        const uint8_t *e = d + 2u + 5u*a;
        uint8_t flags = e[4], role = (uint8_t)(flags & DART__INT_ROLE_MASK);
        int cidx = -1, nc;
        i_DartTopic *topic;
        int their_pub, their_sub, ours_pub, ours_sub;
        if (role == DART_INACTIVE) continue;
        if (astate && a < alen && (astate[a] & DART__AST_DETAILED)) continue;   /* decided */
        nc = i_dart_hash32_candidates(st, i_dart_le_r32(e), &cidx);
        if (!nc) continue;                                 /* no local topic: not a candidate */
        topic = &st->topics[cidx];
        their_pub = (role==DART_PUBSUB || role==DART_PUB_ONLY);
        their_sub = (role==DART_PUBSUB || role==DART_SUB_ONLY);
        ours_pub  = (topic->role==DART_PUBSUB || topic->role==DART_PUB_ONLY);
        ours_sub  = (topic->role==DART_PUBSUB || topic->role==DART_SUB_ONLY);
        if (!((their_pub && ours_sub) || (their_sub && ours_pub))) continue;   /* roles never meet */
        if (out){
            if (cnt >= max_wants) break;
            out[cnt].index = (uint16_t)a;
            /* several local topics behind one 32-bit hash: send hash 0 to force the
               wire inline, so whichever topic the name binds to can still verify */
            out[cnt].schema_hash = (nc == 1 && schemas) ? schemas[cidx].hash : 0;
        }
        cnt++;
    }
    return cnt;
}

/* Unresolved candidates for one topic in a peer's interest (see core.h). Same entry walk
 * as detail_wants, filtered to entries that nominate THIS topic by 32-bit hash. Entries
 * with no verdict storage (map alloc failed, already surfaced as INTEREST_OVERFLOW)
 * are NOT counted: they can never resolve, so a wait on them would only ever time out. */
uint16_t dart_transport_topic_unresolved(DartTransportState *st, uint16_t topic_index,
                                         uint32_t peer_id, DartBytes interest){
    const uint8_t *d=interest.data;
    i_DartTopic *topic;
    uint16_t n, cnt=0; uint32_t a; int slot;
    uint8_t *astate; uint32_t alen;
    int ours_pub, ours_sub;
    topic = i_dart_topic_at(st, topic_index, NULL);
    if (!topic || topic->name_len==0 || !d || interest.len < 2) return 0;
    slot = i_dart_peer_slot(st, peer_id);
    if (slot < 0) return 0;
    n = i_dart_le_r16(d);
    if (interest.len < 2u + 5u*(uint32_t)n) return 0;
    ours_pub = (topic->role==DART_PUBSUB || topic->role==DART_PUB_ONLY);
    ours_sub = (topic->role==DART_PUBSUB || topic->role==DART_SUB_ONLY);
    if (!ours_pub && !ours_sub) return 0;
    astate = st->peer_astate[slot]; alen = st->peer_index_len[slot];
    for (a=0;a<n;a++){
        const uint8_t *e = d + 2u + 5u*a;
        uint8_t flags = e[4], role = (uint8_t)(flags & DART__INT_ROLE_MASK);
        int their_pub, their_sub;
        if (role == DART_INACTIVE) continue;
        if (i_dart_le_r32(e) != (uint32_t)topic->identity) continue;   /* not this topic */
        if (!astate || a >= alen) continue;                     /* unresolvable: never counted */
        if (astate[a] & DART__AST_DETAILED) continue;           /* decided (matched or refused) */
        their_pub = (role==DART_PUBSUB || role==DART_PUB_ONLY);
        their_sub = (role==DART_PUBSUB || role==DART_SUB_ONLY);
        if ((their_pub && ours_sub) || (their_sub && ours_pub)) cnt++;
    }
    return cnt;
}

/* Ingest a DETAIL_RESP: verify each entry (full identity from the name; the schema gate
 * per direction) and cache the verdict. Returns newly decided indices; the caller then
 * re-applies the peer's interest so the verdicts form their matches. Idempotent: decided
 * indices are skipped, so duplicate/crossing responses are harmless. */
uint16_t dart_transport_apply_peer_details(DartTransportState *st, uint32_t peer_id, DartBytes resp){
    DartDetailIter it; DartDetail dd;
    int slot; uint16_t fresh = 0;
    if (!st) return 0;
    slot = i_dart_peer_slot(st, peer_id);
    if (slot < 0) return 0;
    memset(&it, 0, sizeof it);
    while (dart_detail_next(resp, &it, &dd)){
        uint16_t *amap = st->peer_index[slot]; uint8_t *astate = st->peer_astate[slot];
        uint64_t id64; i_DartTopic *topic; int cidx = -1;
        if (!amap || !astate || (uint32_t)dd.index >= st->peer_index_len[slot])
            continue;   /* maps are sized at interest apply; an index we never saw is ignored
                           (a response that raced ahead of the announce re-resolves later) */
        if (astate[dd.index] & DART__AST_DETAILED) continue;
        if (dd.name.len == 0){ astate[dd.index] = DART__AST_DETAILED; fresh++; continue; }
        id64 = i_dart_identity_hash((const uint8_t*)dd.name.data, dd.name.len);
        topic = i_dart_topic_by_identity(st, id64, &cidx);
        if (!topic){                              /* the 32-bit nomination was a false positive */
            astate[dd.index] = DART__AST_DETAILED;
            fresh++; continue;
        }
        if (topic->name_len != dd.name.len || memcmp(topic->name, dd.name.data, dd.name.len) != 0){
            astate[dd.index] = DART__AST_DETAILED;   /* same 64-bit id, different name: refused */
            i_dart_transport_fire_event(st, DART_TRANSPORT_NAME_COLLISION, (uint16_t)cidx,
                        peer_id, id64, 0);
            fresh++; continue;
        }
        amap[dd.index] = (uint16_t)cidx;
        astate[dd.index] = (uint8_t)(DART__AST_DETAILED | DART__AST_NAME_OK
            | ((!st->cfg.schema_check || st->cfg.schema_check(st->cfg.user, peer_id, (uint16_t)cidx,
                    1, dd.schema_hash, dd.schema_wire)) ? DART__AST_READ_OK : 0u)
            | ((!st->cfg.schema_check || st->cfg.schema_check(st->cfg.user, peer_id, (uint16_t)cidx,
                    0, dd.schema_hash, dd.schema_wire)) ? DART__AST_WRITE_OK : 0u));
        fresh++;
    }
    return fresh;
}


int dart_transport_set_role(DartTransportState *st, uint16_t topic_index, uint8_t role){
    i_DartTopic *topic; uint16_t p;
    if (role > DART_INACTIVE) return -1;
    topic = i_dart_topic_at(st, topic_index, NULL);
    if (!topic) return -1;
    if (topic->role == role) return 0;
    topic->role = role;
    for (p=0;p<st->cfg.max_peers;p++)
        if (st->peer_used[p]) i_dart_topic_rematch(st,(uint16_t)topic_index,p);
    /* caller re-advertises interest (the node bumps its discovery announce) */
    return 0;
}


int dart_transport_topic_define(DartTransportState *st, uint16_t topic_index, const DartTopicDef *def){
    i_DartTopic *topic; DartQos q; uint16_t depth, p; size_t lane;
    if (!st || !def) return -1;
    if (topic_index >= st->cfg.n_topics) return -1;            /* out of reserved range */
    if (!def->name || !def->name[0]) return -1;             /* name = identity, required */
    lane = i_dart_name_len(def->name);
    if (def->name[lane]) return -1;                          /* longer than DART_TOPIC_NAME_MAX */
    if (def->directed && def->qos.catch_up) return -1;       /* directed history never replays */
    topic = &st->topics[topic_index];
    if (topic->identity != 0 || topic->history) return -1;        /* slot already defined */
    {   /* same name under a different kind on ONE node: refuse at define. The per-peer alias
           maps bind an entry to one local topic by identity (kind-blind), so local cross-kind
           twins would cross-bind and refuse forever; coexistence is a cross-node property. */
        uint64_t id = dart_topic_identity(def); uint16_t c;
        for (c=0;c<st->cfg.n_topics;c++)
            if (st->topics[c].name_len && st->topics[c].identity == id
                && st->topics[c].kind != def->kind) return -1;
    }
    q = def->qos; i_dart_qos_defaults(&q);
    depth = q.keep_last;
    topic->history = (i_DartWriterSample*)st->cfg.allocator(st->cfg.user, NULL,
                                                         (size_t)depth*sizeof(i_DartWriterSample));
    if (!topic->history) return -4;                            /* OOM */
    memset(topic->history, 0, (size_t)depth*sizeof(i_DartWriterSample));
    topic->history_owned = 1;
    topic->qos = q;
    topic->role = def->role;
    topic->kind = def->kind; topic->prefix_bytes = def->prefix_bytes; topic->directed = def->directed;
    topic->forceable = def->forceable;
    topic->identity = dart_topic_identity(def);
    memcpy((char*)topic->name, def->name, lane); ((char*)topic->name)[lane] = '\0';
    topic->name_len = (uint8_t)lane;
    topic->history_head = 0; topic->next_seqno = 0; topic->have_first = 0;
    /* A DISSOLVED verdict (details arrived, no local topic matched) is only as durable
       as the topic set it was judged against, and that set just grew: send those
       verdicts back to PENDING so the next interest apply re-requests and re-verifies
       them against the new identity. A NAME_OK verdict is bound to an immutable name and
       stays. Without this, an observer that fetched details before subscribing (the
       explorer's flow) could never match a topic it learned about first. */
    for (p=0;p<st->cfg.max_peers;p++){
        uint8_t *as = st->peer_astate[p]; uint32_t a, alen = st->peer_index_len[p];
        if (!st->peer_used[p] || !as) continue;
        for (a=0;a<alen;a++)
            if ((as[a] & DART__AST_DETAILED) && !(as[a] & DART__AST_NAME_OK)) as[a] = 0;
    }
    for (p=0;p<st->cfg.max_peers;p++)        /* match the newly active topic to known peers */
        if (st->peer_used[p]) i_dart_topic_rematch(st, topic_index, p);
    return 0;
}


DartString dart_transport_topic_name(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    if (!topic || topic->name_len == 0) return dart_string(NULL, 0);   /* undefined / reserve slot */
    return dart_string(topic->name, topic->name_len);
}


const DartQos *dart_transport_topic_qos(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    return topic ? &topic->qos : NULL;
}


void dart_transport_repair_stats(DartTransportState *st, uint16_t topic_index, DartRepairStats *out){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    if (!out) return;
    if (topic) *out = topic->repair_stats;
    else memset(out, 0, sizeof *out);
}


void dart_transport_on_datagram(DartTransportState *st, uint32_t from, DartBytes datagram, uint64_t now){
    const uint8_t *p=datagram.data; size_t rem=datagram.len;
    int peer_slot=i_dart_peer_slot(st,from);
    if (peer_slot<0) return;
    /* concatenated submessages; each length comes from its header, so no framing */
    while (rem>=3){
        uint8_t b0=p[0], type=(uint8_t)(b0 & DART_MSG_MASK); uint16_t index; size_t sub; int topic_index;
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
        index = i_dart_le_r16(p+DART_OFFSET_INDEX);
        if ((uint32_t)index < st->peer_index_len[peer_slot]){
            uint16_t m=st->peer_index[peer_slot][index]; topic_index=(m==0xFFFFu)?-1:(int)m;
        } else topic_index=-1;
        if (topic_index>=0){
            switch(type){
                case DART_DATA:
#ifdef DART_SHM
                                if (p[0] & DART_F_SHM){ i_dart_reader_shm(st,topic_index,peer_slot,p,now); break; }
#endif
                                i_dart_reader_data(st,topic_index,peer_slot,p,now); break;
                case DART_HB:   i_dart_reader_hb  (st,topic_index,peer_slot,p,now); break;
                case DART_NACK: i_dart_writer_nack(st,topic_index,peer_slot,p); break;
            }
        }
        p+=sub; rem-=sub;
    }
}
