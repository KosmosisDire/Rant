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
    /* repair_delay_us stays 0 = adaptive (the peer's RTT bound, DART_QOS_DEF_REPAIR_US before
       the first sample); a nonzero value pins it. Resolved per NACK in i_dart_reader_emit. */
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
      DartPeerRtt *peer_rtt = (DartPeerRtt*)i_dart_bump_take(b, max_peers*sizeof(DartPeerRtt), 8);
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
      uint8_t **peer_agen      = (uint8_t**) i_dart_bump_take(b, (size_t)max_peers*sizeof(uint8_t*), 8);
      uint8_t **peer_attrs     = (uint8_t**) i_dart_bump_take(b, (size_t)max_peers*sizeof(uint8_t*), 8);
      uint32_t *peer_seen_version = (uint32_t*)i_dart_bump_take(b, (size_t)max_peers*sizeof(uint32_t), 8);
      name_pool = (char*)i_dart_bump_take(b, name_bytes ? name_bytes : 1u, 1);
      if (st && b->base){
          st->cfg=*cfg; st->peer_ids=peer_ids; st->peer_used=peer_used;
          st->peer_dormant=peer_dormant; st->peer_frag=peer_frag;
          st->peer_rtt=peer_rtt; memset(peer_rtt, 0, max_peers*sizeof(DartPeerRtt));
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
          st->peer_agen=peer_agen; st->peer_attrs=peer_attrs;
          st->peer_seen_version=peer_seen_version;
          memset(peer_used,0,max_peers); memset(peer_dormant,0,max_peers);
          { uint32_t k; for (k=0;k<max_peers;k++) peer_frag[k]=DART_FRAG_SIZE; }  /* set per peer on add */
#ifdef DART_SHM
          st->peer_shm=peer_shm; memset(peer_shm,0,max_peers);
#endif
          memset(peer_index, 0, (size_t)max_peers*sizeof(uint16_t*));
          memset(peer_index_len, 0, (size_t)max_peers*sizeof(uint32_t));
          memset(peer_astate, 0, (size_t)max_peers*sizeof(uint8_t*));
          memset(peer_agen, 0, (size_t)max_peers*sizeof(uint8_t*));
          memset(peer_attrs, 0, (size_t)max_peers*sizeof(uint8_t*));
          memset(peer_seen_version, 0, (size_t)max_peers*sizeof(uint32_t));
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
                topic->attrs=def->attrs;
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
    memcpy(nw->peer_rtt,     old->peer_rtt,     (size_t)omp*sizeof(DartPeerRtt));
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
        nw->peer_agen[p]      = old->peer_agen[p];
        nw->peer_attrs[p]     = old->peer_attrs[p];
        nw->peer_seen_version[p] = old->peer_seen_version[p];
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
 * two topics of one identity (different QoS) and switch which is live by role. A RETIRED
 * slot is invisible here: it can never resolve, verify, or demux again. */
static i_DartTopic *i_dart_topic_by_identity(DartTransportState *st, uint64_t identity, int *idx_out){
    uint16_t i; int first=-1;
    for (i=0;i<st->cfg.n_topics;i++){
        if (st->topics[i].retired || st->topics[i].identity!=identity) continue;
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

/* free one assembly slot's grown buffers (lane release / destroy) */
static void i_dart_asm_free(DartTransportState *st, i_DartAssembly *a){
    if (a->buf){ st->cfg.allocator(st->cfg.user, a->buf, 0); a->buf=NULL; a->cap=0; }
    if (a->bitmap){ st->cfg.allocator(st->cfg.user, a->bitmap, 0); a->bitmap=NULL; a->bitmap_cap=0; }
    a->active=0;
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
    i_dart_asm_free(st, &l->r.cur); i_dart_asm_free(st, &l->r.next);
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
    i_DartAssembly cur=r->cur, next=r->next;   /* keep grown buffers across rematch */
    memset(r,0,sizeof(*r));
    r->cur=cur; r->next=next; r->cur.active=0; r->next.active=0;
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
    l->r.used=0; l->r.cur.active=0; l->r.next.active=0;
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
    int wuse = dart_role_pubs(topic->role) && i_dart_bit_get(peer_sub_bitmap,c);
    int ruse = dart_role_subs(topic->role) && i_dart_bit_get(peer_pub_bitmap,c);
    i_DartLane *l;
    /* rebind hold (writer side only): until this peer proves it applied our announce at
       the slot's rebind version (dart_transport_peer_seen_version), its demux map may
       still bind our index to the OLD occupant, so nothing may be sent to it. Inbound
       needs no hold: our own verdicts of its entries were re-pended at the rebind. */
    if (wuse && topic->rebind_version && st->peer_seen_version[peer_slot] < topic->rebind_version)
        wuse = 0;
    l = i_dart_lane_at(st,c,peer_slot);
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
    memset(&st->peer_rtt[free], 0, sizeof(DartPeerRtt));   /* a new peer starts unmeasured */
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
        if (st->peer_agen[free])   memset(st->peer_agen[free],0,st->peer_index_len[free]);
        if (st->peer_attrs[free])  memset(st->peer_attrs[free],0,st->peer_index_len[free]);
    }
    st->peer_seen_version[free]=0;   /* seen versions are per incarnation */
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
        if (st->peer_agen[s])   st->cfg.allocator(st->cfg.user, st->peer_agen[s], 0);
        if (st->peer_attrs[s])  st->cfg.allocator(st->cfg.user, st->peer_attrs[s], 0);
        st->peer_index[s]=NULL; st->peer_astate[s]=NULL; st->peer_agen[s]=NULL;
        st->peer_attrs[s]=NULL; st->peer_index_len[s]=0;
    }
    st->peer_seen_version[s]=0;
    st->peer_used[s]=0; st->peer_dormant[s]=0;
    memset(&st->peer_rtt[s], 0, sizeof(DartPeerRtt));
#ifdef DART_SHM
    st->peer_shm[s]=0;
#endif
}

/* ---- the per-peer round-trip estimator (core.h DartPeerRtt) ---------------------------- */

/* Fold one sample in (RFC 6298: alpha 1/8 on the mean, beta 1/4 on the deviation; the
 * first sample seeds both). Callers hand in only unambiguous samples: a seqno that was asked
 * exactly once, a sample that was never resent. */
void i_dart_rtt_sample(DartTransportState *st, uint32_t peer_slot, uint64_t sample_us){
    DartPeerRtt *e = &st->peer_rtt[peer_slot];
    uint32_t r = sample_us > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)sample_us;
    if (e->samples == 0){
        e->rtt_us = r; e->rtt_jitter_us = r / 2u; e->rtt_min_us = r;
    } else {
        uint32_t diff = e->rtt_us > r ? e->rtt_us - r : r - e->rtt_us;
        e->rtt_jitter_us = (uint32_t)((3ull * e->rtt_jitter_us + diff) / 4u);
        e->rtt_us        = (uint32_t)((7ull * e->rtt_us + r) / 8u);
        if (r < e->rtt_min_us) e->rtt_min_us = r;
    }
    e->rtt_last_us = r;
    if (e->samples != 0xFFFFFFFFu) e->samples++;
}

/* The retransmit bound this peer's estimate implies: smoothed + max(one tick, 4 x deviation),
 * never under DART_RTO_MIN_US; fallback_us (a QoS default) until the first sample. */
uint32_t i_dart_rtt_rto(DartTransportState *st, uint32_t peer_slot, uint32_t fallback_us){
    const DartPeerRtt *e = &st->peer_rtt[peer_slot];
    uint64_t var, rto;
    if (e->samples == 0) return fallback_us;
    var = 4ull * e->rtt_jitter_us;
    if (var < DART_RTO_GRAIN_US) var = DART_RTO_GRAIN_US;
    rto = (uint64_t)e->rtt_us + var;
    if (rto < DART_RTO_MIN_US) rto = DART_RTO_MIN_US;
    return rto > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)rto;
}

int dart_transport_peer_rtt(DartTransportState *st, uint32_t peer_id, DartPeerRtt *out){
    int s = st ? i_dart_peer_slot(st, peer_id) : -1;
    if (out) memset(out, 0, sizeof *out);
    if (s < 0) return 0;
    if (out) *out = st->peer_rtt[s];
    return 1;
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
        if (!topic->history) continue;         /* retired slot: ring already freed */
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
        i_dart_asm_free(st, &l->r.cur); i_dart_asm_free(st, &l->r.next);
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
            if (st->peer_agen[p]){
                st->cfg.allocator(st->cfg.user, st->peer_agen[p], 0);
                st->peer_agen[p]=NULL;
            }
            if (st->peer_attrs[p]){
                st->cfg.allocator(st->cfg.user, st->peer_attrs[p], 0);
                st->peer_attrs[p]=NULL;
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
    uint16_t *nm; uint8_t *ns, *ng, *na;
    if (need <= have) return st->peer_index[peer_slot];
    nm = (uint16_t*)st->cfg.allocator(st->cfg.user, st->peer_index[peer_slot], (size_t)need*sizeof(uint16_t));
    if (!nm) return NULL;
    st->peer_index[peer_slot] = nm;                     /* len not yet raised: retryable on OOM below */
    ns = (uint8_t*)st->cfg.allocator(st->cfg.user, st->peer_astate[peer_slot], (size_t)need);
    if (!ns) return NULL;
    st->peer_astate[peer_slot] = ns;
    ng = (uint8_t*)st->cfg.allocator(st->cfg.user, st->peer_agen[peer_slot], (size_t)need);
    if (!ng) return NULL;
    st->peer_agen[peer_slot] = ng;
    na = (uint8_t*)st->cfg.allocator(st->cfg.user, st->peer_attrs[peer_slot], (size_t)need);
    if (!na) return NULL;
    st->peer_attrs[peer_slot] = na;
    memset(nm + have, 0xFF, (size_t)(need-have)*sizeof(uint16_t));   /* grown tail: unmapped */
    memset(ns + have, 0, (size_t)(need-have));                       /* ...no verdicts */
    memset(ng + have, 0, (size_t)(need-have));                       /* ...generation 0 */
    memset(na + have, 0, (size_t)(need-have));                       /* ...no attrs */
    st->peer_index_len[peer_slot] = need;
    return nm;
}

/* Candidate topics for a 32-bit interest hash: the count of local topics whose
 * identity's low 32 bits match, and the preferred one (non-INACTIVE first, mirroring
 * i_dart_topic_by_identity). Same linear resolution the identity lookup pays. */
static int i_dart_hash32_candidates(DartTransportState *st, uint32_t h, int *idx_out){
    uint16_t i; int first=-1, live=-1, n=0;
    for (i=0;i<st->cfg.n_topics;i++){
        if (!i_dart_topic_announced(&st->topics[i]) || (uint32_t)st->topics[i].identity != h) continue;
        n++;
        if (first<0) first=(int)i;
        if (live<0 && st->topics[i].role!=DART_INACTIVE) live=(int)i;
    }
    if (idx_out) *idx_out = live>=0 ? live : first;
    return n;
}


/* Upper bound on dart_transport_build_interest output, for sizing the announce buffer:
 * one positional [u32 hash][u8 flags] entry per topic slot, plus the worst-case rate
 * section (every topic could advertise a max_rate_hz: [u16 n_rates] + 4 B per entry)
 * and the worst-case generation section ([u16 n] + 3 B per rebound topic). */
size_t dart_interest_max(uint16_t n_topics){
    return 2u + 5u * (size_t)n_topics + 2u + 4u * (size_t)n_topics
         + 2u + 3u * (size_t)n_topics;
}

/* is this a topic we SUBSCRIBE with a best-effort rate cap? (the rate section's
 * membership test, shared by the count and the write walk so they cannot drift) */
static int i_dart_topic_rate_sub(const i_DartTopic *t){
    return i_dart_topic_announced(t) && t->qos.max_rate_hz && dart_role_subs(t->role);
}

/* was this slot ever REBOUND to a different binding? (the generation section's membership
 * test, shared by the count and the write walk so they cannot drift) */
static int i_dart_topic_rebound(const i_DartTopic *t){
    return i_dart_topic_announced(t) && t->gen != 0;
}


/* Serialize our interest into out, or MEASURE it (out NULL): ONE walk serves both, so
 * dart_transport_build_interest and dart_transport_interest_size agree byte for byte.
 * Layout: [u16 n] (n = SLOTS, holes included), then one [u32 hash][u8 flags] entry per
 * topic IN INDEX ORDER up to the highest announced slot (position = index). A run of
 * UNDEFINED (or RETIRED) reserve slots collapses to ONE entry flagged DART__INT_HOLE_RUN
 * whose hash field is the run length, so later indices stay stable without a big reserve
 * padding the announce to its full size. A defined-but-INACTIVE topic still rides as a
 * normal entry (its identity survives role flips). Then two SPARSE sections: a rate
 * section [u16 n_rates][(u16 topic_index)(u16 rate_hz)]* for the subscriber topics that
 * cap their best-effort delivery rate (see DartQos.max_rate_hz), and a generation section
 * [u16 n_gens][(u16 topic_index)(u8 gen)]* naming every announced slot that was ever
 * REBOUND to a different name/kind/schema (dart_transport_topic_reuse), so a receiver
 * holding a verdict formed under an older generation re-verifies instead of applying it
 * to the new occupant. Both default to zero entries, so each costs 2 bytes on a node
 * that caps no rate and never rebinds. Returns the exact byte size (measure), or the
 * bytes written / 0 when cap is too small (build). */
static size_t i_dart_interest_emit(DartTransportState *st, uint8_t *out, size_t cap){
    uint8_t *e, *rp;
    uint16_t c, n=0, n_rates=0, n_gens=0;
    uint32_t cells=0; int in_hole=0; size_t len;
    for (c=0;c<st->cfg.n_topics;c++) if (i_dart_topic_announced(&st->topics[c])) n=(uint16_t)(c+1u);
    for (c=0;c<n;c++){                       /* one pass: exact cell count (runs collapse)
                                                plus the two sparse section counts */
        const i_DartTopic *topic = &st->topics[c];
        if (i_dart_topic_announced(topic)){ cells++; in_hole=0; }
        else { if (!in_hole) cells++; in_hole=1; }
        if (i_dart_topic_rate_sub(topic)) n_rates++;
        if (i_dart_topic_rebound(topic))  n_gens++;
    }
    len = 2u + 5u*(size_t)cells + 2u + 4u*(size_t)n_rates + 2u + 3u*(size_t)n_gens;
    if (!out) return len;
    if (cap < len) return 0;
    i_dart_le_w16(out, n);
    e = out + 2u;
    for (c=0;c<n;c++){
        const i_DartTopic *topic = &st->topics[c];
        if (!i_dart_topic_announced(topic)){
            uint32_t run = 1;
            while ((uint16_t)(c+run) < n && !i_dart_topic_announced(&st->topics[c+run])) run++;
            i_dart_le_w32(e, run);
            e[4] = (uint8_t)(DART_INACTIVE | DART__INT_HOLE_RUN);
            e += 5u; c = (uint16_t)(c + run - 1u);
            continue;
        }
        i_dart_le_w32(e, (uint32_t)topic->identity);
        e[4] = (uint8_t)((topic->role & DART__INT_ROLE_MASK)
             | (topic->qos.reliability==DART_RELIABLE ? DART__INT_RELIABLE : 0u)
             | ((topic->kind << DART__INT_KIND_SHIFT) & DART__INT_KIND_MASK));
        e += 5u;
    }
    rp = e;
    i_dart_le_w16(rp, n_rates); rp += 2u;
    for (c=0;c<n;c++){
        const i_DartTopic *topic = &st->topics[c];
        if (i_dart_topic_rate_sub(topic)){
            i_dart_le_w16(rp, c); i_dart_le_w16(rp+2, topic->qos.max_rate_hz); rp += 4u;
        }
    }
    i_dart_le_w16(rp, n_gens); rp += 2u;
    for (c=0;c<n;c++){
        const i_DartTopic *topic = &st->topics[c];
        if (i_dart_topic_rebound(topic)){
            i_dart_le_w16(rp, c); rp[2] = topic->gen; rp += 3u;
        }
    }
    return (size_t)(rp - out);
}

/* Build mode of i_dart_interest_emit (the layout lives there). Returns bytes written, or
 * 0 if cap is too small; size out via dart_interest_max (worst case) or
 * dart_transport_interest_size (exact). */
size_t dart_transport_build_interest(DartTransportState *st, void *out, size_t cap){
    return i_dart_interest_emit(st, (uint8_t*)out, cap);
}

/* Serial validation walk over an interest entry stream: returns the stream's byte length
 * (2 + 5 per cell) when it accounts exactly n slots inside len, else 0 (truncated or a
 * malformed run). Every parser walks through this first, so a bad blob is rejected
 * wholesale, matching the old fixed-stride length check. */
static uint32_t i_dart_interest_walk_len(const uint8_t *d, size_t len, uint16_t n){
    const uint8_t *e = d + 2u, *end = d + len;
    uint32_t a = 0;
    while (a < n){
        uint32_t step = 1;
        if (e + 5 > end) return 0;
        if (e[4] & DART__INT_HOLE_RUN){
            step = i_dart_le_r32(e);
            if (step == 0 || step > (uint32_t)n - a) return 0;
        }
        e += 5u; a += step;
    }
    return (uint32_t)(e - d);
}

/* The two sparse sections that follow the entry stream, located in ONE pass: rate, then
 * generation. Each view points at that section's ENTRIES (past its [u16 n] header) and
 * spans n * entry_bytes. Sections are POSITIONAL, so one policy covers both: a section
 * whose header or whose whole entry array does not fit inside len is ABSENT ({NULL,0}),
 * and so is every section after it (a truncated one leaves the rest unlocatable). A
 * well-formed blob yields both exactly. */
typedef struct { DartBytes rate, gen; } i_DartInterestSections;

static i_DartInterestSections i_dart_interest_sections(const uint8_t *d, size_t len,
                                                       uint32_t entries_len){
    static const uint8_t width[2] = { 4u, 3u };  /* (index,rate_hz) (index,gen) */
    i_DartInterestSections s; DartBytes *view[2];
    size_t off = entries_len; int k;
    s.rate = s.gen = dart_bytes(NULL, 0);
    view[0] = &s.rate; view[1] = &s.gen;
    for (k=0;k<2;k++){
        size_t bytes;
        if (len < off + 2u) break;                   /* no header: this one and the rest absent */
        bytes = (size_t)i_dart_le_r16(d + off) * width[k];
        off += 2u;
        if (len < off + bytes) break;                /* entries truncated: same */
        *view[k] = dart_bytes(d + off, bytes);
        off += bytes;
    }
    return s;
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
    const uint8_t *d=blob.data, *e, *gp, *g_end;
    uint16_t n, c; uint32_t a, entries_len; int peer_slot=i_dart_peer_slot(st,peer_id);
    uint8_t *peer_pub_bitmap, *peer_sub_bitmap, *peer_sub_reliable;
    uint16_t *amap; uint8_t *astate;
    i_DartInterestSections sec;
    uint32_t unmappable = 0;
    if (peer_slot<0 || !d || blob.len<2) return;
    n = i_dart_le_r16(d);
    entries_len = i_dart_interest_walk_len(d, blob.len, n);
    if (!entries_len) return;                          /* truncated/malformed: reject wholesale */
    peer_pub_bitmap  =&st->peer_pub_bitmap[(size_t)peer_slot*st->bitmap_len];
    peer_sub_bitmap  =&st->peer_sub_bitmap[(size_t)peer_slot*st->bitmap_len];
    peer_sub_reliable=&st->peer_sub_reliable[(size_t)peer_slot*st->bitmap_len];
    memset(peer_pub_bitmap,0,st->bitmap_len); memset(peer_sub_bitmap,0,st->bitmap_len);
    memset(peer_sub_reliable,0,st->bitmap_len);
    amap   = n ? i_dart_peer_index_ensure(st, peer_slot, n) : NULL;
    astate = amap ? st->peer_astate[peer_slot] : NULL;
    /* The sparse sections, located once, up front. The generation data must be in hand
       BEFORE the entry loop (its per-entry gate below consumes it with a merge cursor);
       the rate VALUES are applied after the rematch, once lanes exist. */
    sec = i_dart_interest_sections(d, blob.len, entries_len);
    gp = g_end = NULL;
    if (sec.gen.data){ gp = sec.gen.data; g_end = gp + sec.gen.len; }
    e = d + 2u;
    for (a=0;a<n;a++,e+=5u){
        uint8_t flags = e[4], role = (uint8_t)(flags & DART__INT_ROLE_MASK);
        int their_pub, their_sub, rel;
        uint16_t cidx; i_DartTopic *topic;
        if (flags & DART__INT_HOLE_RUN){ a += i_dart_le_r32(e) - 1u; continue; }
        if (astate && a < st->peer_index_len[peer_slot] && st->peer_agen[peer_slot]){
            /* rebind generation gate: the peer rebound this position (a retired slot
               reused with a different name/kind/schema) since our verdict was formed, so
               the verdict describes the OLD occupant: back to pending, demux severed,
               details re-verify the new binding. Runs for INACTIVE entries too, so a
               rebind noticed while parked never wires stale state when the role returns.
               The stored gen tracks the latest APPLIED blob either way, so a verdict
               formed after this apply binds to the generation it was judged under. */
            uint8_t g = 0;
            while (gp && gp + 3 <= g_end && i_dart_le_r16(gp) < a) gp += 3;
            if (gp && gp + 3 <= g_end && i_dart_le_r16(gp) == a) g = gp[2];
            if (st->peer_agen[peer_slot][a] != g){
                if (astate[a]){
                    astate[a] = 0; amap[a] = 0xFFFFu;
                    if (st->peer_attrs[peer_slot]) st->peer_attrs[peer_slot][a] = 0;
                }
                st->peer_agen[peer_slot][a] = g;
            }
        }
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
        if ((uint32_t)topic->identity != i_dart_le_r32(e)){
            /* the entry's hash no longer names the identity this verdict bound: the
               position was rebound to a different name (the gen gate's belt) */
            astate[a] = 0; amap[a] = 0xFFFFu;
            if (st->peer_attrs[peer_slot]) st->peer_attrs[peer_slot][a] = 0;
            continue;
        }
        if (topic->role == DART_INACTIVE || topic->retired){
            /* dual same-identity topics switched by role (or the bound slot retired):
               the verdict (incl. its schema gates) bound the then-live twin, so
               re-verify against the one live now */
            int resolved_index = (int)cidx;
            i_dart_topic_by_identity(st, topic->identity, &resolved_index);
            if ((uint16_t)resolved_index != cidx){
                astate[a] = 0; amap[a] = 0xFFFFu;      /* pending again; wants() re-asks */
                if (st->peer_attrs[peer_slot]) st->peer_attrs[peer_slot][a] = 0;
                continue;
            }
            if (topic->retired) continue;   /* no live successor yet: no bits, no lanes */
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
        their_pub = dart_role_pubs(role);
        their_sub = dart_role_subs(role);
        rel       = (flags & DART__INT_RELIABLE) != 0;
        if (their_pub){                                /* their offered QoS vs our subscription */
            int ours_sub = dart_role_subs(topic->role);
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

    /* Per-lane VALUES (the rate section + the cached detail attrs), applied AFTER rematch
       so the lanes exist, and re-applied on every announce: each value is immutable per
       topic, so a lane re-formed by a role flip simply re-derives it (a fresh match memsets
       the proxy, so nothing stale survives). */
    if (amap && sec.rate.data){
        /* rate entries [(u16 their_index)(u16 rate_hz)]*: their best-effort delivery cap for
           a topic they SUBSCRIBE, so it paces our fire-and-forget writer lane (best-effort,
           so w->used with the sub bit). */
        const uint8_t *p = sec.rate.data, *end = p + sec.rate.len;
        for (; p + 4u <= end; p += 4u){
            uint16_t their_idx = i_dart_le_r16(p), rate_hz = i_dart_le_r16(p+2), cidx;
            i_DartWriterProxy *w;
            if (their_idx >= st->peer_index_len[peer_slot]) continue;
            cidx = amap[their_idx];
            if (cidx >= st->cfg.n_topics) continue;             /* unverified/unmapped */
            w = i_dart_writer_proxy_at(st, cidx, (uint32_t)peer_slot);
            if (w && w->used)
                w->rate_interval_us = rate_hz ? (1000000u / (uint32_t)rate_hz) : 0u;
        }
    }
    if (amap && st->peer_attrs[peer_slot]){
        /* DART_ATTR_NO_TIMESTAMP from the detail cache onto our reader lane, so delivery
           strips exactly what the writer prepended. Only a NAME_OK entry maps, and the
           caller re-runs this apply after details land, so a lane never delivers before
           its attrs are in hand. Unset = stamped (the default). */
        const uint8_t *attrs = st->peer_attrs[peer_slot];
        for (a=0;a<st->peer_index_len[peer_slot];a++){
            uint16_t cidx; i_DartReaderProxy *r;
            if (!(attrs[a] & DART_ATTR_NO_TIMESTAMP)) continue;
            cidx = amap[a];
            if (cidx >= st->cfg.n_topics) continue;             /* unverified/unmapped */
            r = i_dart_reader_proxy_at(st, cidx, (uint32_t)peer_slot);
            if (r && r->used) r->no_timestamp = 1;
        }
    }
}


int dart_transport_peer_timestamped(DartTransportState *st, uint16_t topic_index, uint32_t peer_id){
    int peer_slot;
    i_DartReaderProxy *r;
    if (!st || topic_index >= st->cfg.n_topics) return 1;
    peer_slot = i_dart_peer_slot(st, peer_id);
    if (peer_slot < 0) return 1;                       /* unknown peer: the default framing */
    r = i_dart_reader_proxy_at(st, topic_index, (uint32_t)peer_slot);
    return (r && r->no_timestamp) ? 0 : 1;
}


uint8_t dart_transport_peer_attrs(DartTransportState *st, uint32_t peer_id, uint16_t their_index){
    int peer_slot;
    if (!st) return 0;
    peer_slot = i_dart_peer_slot(st, peer_id);
    if (peer_slot < 0 || !st->peer_attrs[peer_slot]
        || (uint32_t)their_index >= st->peer_index_len[peer_slot]) return 0;
    return st->peer_attrs[peer_slot][their_index];
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
#define DART__META_VER  21u                 /* what WE write (odd versions carry the SHM base) */
#define DART__META_BASE DART__META_BASE_SHM
#else
#define DART__META_VER  20u
#define DART__META_BASE DART__META_BASE_NOSHM
#endif

static int i_dart_meta_ok(DartBytes meta){
    return meta.data && meta.len >= DART__META_BASE_NOSHM
        && meta.data[0]=='D' && meta.data[1]=='N'
        && meta.data[2]>=20 && meta.data[2]<=21;
}
/* base prefix through the iflags byte, by version (the odd one carries shm+host, the even
 * one doesn't). iflags is always the base's last byte. */
static uint16_t i_dart_meta_base(const uint8_t *meta){
    return (meta[2] & 1u) ? DART__META_BASE_SHM : DART__META_BASE_NOSHM;
}
uint16_t dart_meta_cap(uint16_t n_topics){
    size_t cap = (size_t)DART__META_BASE + dart_interest_max(n_topics);
    if (cap > 65000u) cap = 65000u;
    return (uint16_t)cap;
}

/* Exact bytes of our current interest blob (the total_len interest paging serves): the
 * MEASURE mode of the one walk dart_transport_build_interest emits, so it matches byte for
 * byte (defined slots cost one 5 B entry each, every maximal run of undefined slots costs
 * one, then the three sparse sections). */
uint32_t dart_transport_interest_size(DartTransportState *st){
    return (uint32_t)i_dart_interest_emit(st, NULL, 0);
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
        if (flags & DART__INT_HOLE_RUN){  /* a run of undefined reserve slots: skip them all */
            uint32_t run = i_dart_le_r32(interest.data + off);
            if (run == 0 || run > it->left){ it->left = 0; return 0; }    /* malformed: stop */
            it->left = (uint16_t)(it->left - run); it->index = (uint16_t)(it->index + run);
            it->off = off + 5u; it->phase = 0;
            continue;
        }
        if (role == DART_INACTIVE){       /* declared-but-off: not advertised */
            it->left--; it->index++; it->off = off + 5u; it->phase = 0;
            continue;
        }
        out->index    = it->index;
        out->role     = role;
        out->reliable = (uint8_t)((flags & DART__INT_RELIABLE) ? 1 : 0);
        out->kind     = (uint8_t)((flags & DART__INT_KIND_MASK) >> DART__INT_KIND_SHIFT);
        out->hash     = i_dart_le_r32(interest.data + off);
        if (it->phase == 0 && dart_role_pubs(role)){
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
    if (!i_dart_meta_ok(meta) || !(meta.data[2] & 1u)   /* odd version = the SHM-carrying base */
        || meta.len < DART__META_BASE_SHM || !meta.data[5]) return 0;
    memcpy(host, meta.data+6, 16);
    return 1;
}
#endif


/* Pairwise detail exchange codec ('uDTL', see core.h for the layout and contract).
   Parsing is fully bounds-checked: a malformed request or response is dropped wholesale,
   never trusted. The responder side is a pure read of topic + schema state. */
#define DART__DETAIL_HDR 14u   /* magic(4) kind(1) ver(1) domain(2) meta_version(4) n(2) */
#define DART__DETAIL_VER 2u

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
        need = 2u + 1u + 1u + topic->name_len + 8u + 2u + wire.len;
        /* One un-fragmented datagram per response: stop at cap, but ALWAYS include at least
           one entry (n_out>0 gate) so a lone entry larger than a datagram rides its own
           (fragmenting) page instead of wedging paging with an endless header-only reply.
           Measure (out==NULL, cap=DART_DGRAM_MAX) and build (cap=the measured size) apply
           the identical bound, so the sized buffer always holds exactly what is built. */
        if (n_out > 0 && len + need > cap) break;
        if (out){
            uint8_t *e = out + len;
            i_dart_le_w16(e, index);
            e[2] = (uint8_t)(topic->attrs
                 | (topic->qos.no_timestamp ? DART_ATTR_NO_TIMESTAMP : 0u));
            e[3] = topic->name_len;
            memcpy(e+4, topic->name, topic->name_len);
            i_dart_le_w64(e+4+topic->name_len, hash);
            i_dart_le_w16(e+4+topic->name_len+8, (uint16_t)wire.len);
            if (wire.len) memcpy(e+4+topic->name_len+10, wire.data, wire.len);
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
    if ((size_t)off + 4u > resp.len){ it->left = 0; return 0; }        /* truncated: stop */
    nlen = resp.data[off+3];
    if ((size_t)off + 4u + nlen + 10u > resp.len){ it->left = 0; return 0; }
    wlen = i_dart_le_r16(resp.data + off + 4u + nlen + 8u);
    if ((size_t)off + 4u + nlen + 10u + wlen > resp.len){ it->left = 0; return 0; }
    out->index       = i_dart_le_r16(resp.data + off);
    out->attrs       = resp.data[off+2];
    out->name        = dart_string((const char*)(resp.data + off + 4u), nlen);
    out->schema_hash = i_dart_le_r64(resp.data + off + 4u + nlen);
    out->schema_wire = wlen ? dart_bytes(resp.data + off + 4u + nlen + 10u, wlen)
                            : dart_bytes(NULL, 0);
    it->off = off + 4u + nlen + 10u + wlen;
    it->left--;
    return 1;
}

/* Iterator over the LIVE entries of an interest stream: hole runs collapsed, INACTIVE
 * entries skipped, each survivor yielded with its position (the advertiser's topic index),
 * its 32-bit hash, its flag byte and its role overlap. That is the whole entry walk the
 * candidate scans below (detail_wants and its topic-scoped slice topic_unresolved) share,
 * so their stepping cannot drift. The stream must have passed i_dart_interest_walk_len
 * first: that is what makes every 5 B step and every run length in bounds. */
typedef struct {
    const uint8_t *e;        /* next entry */
    uint32_t a;              /* its position */
    uint32_t n;              /* slots in the stream */
    uint32_t pos;            /* yielded: the entry's own position */
    uint32_t hash;           /* yielded: its 32-bit identity nomination */
    uint8_t  flags;          /* yielded: the raw entry flags (role, reliability, kind...) */
    uint8_t  their_pub, their_sub;   /* yielded: the advertised role, split by direction */
} i_DartInterestScan;

static void i_dart_interest_scan_init(i_DartInterestScan *s, const uint8_t *d, uint16_t n){
    s->e = d + 2u; s->a = 0; s->n = n;
    s->pos = 0; s->hash = 0; s->flags = 0; s->their_pub = s->their_sub = 0;
}

static int i_dart_interest_scan_next(i_DartInterestScan *s){
    while (s->a < s->n){
        const uint8_t *e = s->e;
        uint32_t a = s->a, step = 1;
        uint8_t flags = e[4], role = (uint8_t)(flags & DART__INT_ROLE_MASK);
        if (flags & DART__INT_HOLE_RUN) step = i_dart_le_r32(e);
        s->e = e + 5u; s->a = a + step;
        if (flags & DART__INT_HOLE_RUN) continue;   /* a run of undefined reserve slots */
        if (role == DART_INACTIVE) continue;        /* declared but off: not advertised */
        s->pos = a; s->hash = i_dart_le_r32(e); s->flags = flags;
        s->their_pub = (uint8_t)dart_role_pubs(role);
        s->their_sub = (uint8_t)dart_role_subs(role);
        return 1;
    }
    return 0;
}

/* The indices still PENDING for a peer: candidates (32-bit hash overlap + role overlap)
 * without a cached verdict. See core.h for the request-on-every-announce retry contract. */
uint16_t dart_transport_detail_wants(DartTransportState *st, const DartMetaSchema *schemas,
                                     uint32_t peer_id, DartBytes interest,
                                     DartDetailWant *out, uint16_t max_wants){
    const uint8_t *d=interest.data;
    i_DartInterestScan scan;
    uint16_t n, cnt=0; int slot;
    uint8_t *astate; uint32_t alen;
    if (!st || !d || interest.len < 2) return 0;
    slot = i_dart_peer_slot(st, peer_id);
    if (slot < 0) return 0;
    n = i_dart_le_r16(d);
    if (!i_dart_interest_walk_len(d, interest.len, n)) return 0;
    astate = st->peer_astate[slot]; alen = st->peer_index_len[slot];
    i_dart_interest_scan_init(&scan, d, n);
    while (i_dart_interest_scan_next(&scan)){
        int cidx = -1, nc;
        i_DartTopic *topic;
        int ours_pub, ours_sub;
        if (astate && scan.pos < alen && (astate[scan.pos] & DART__AST_DETAILED)) continue; /* decided */
        nc = i_dart_hash32_candidates(st, scan.hash, &cidx);
        if (!nc) continue;                                 /* no local topic: not a candidate */
        topic = &st->topics[cidx];
        ours_pub = dart_role_pubs(topic->role);
        ours_sub = dart_role_subs(topic->role);
        if (!((scan.their_pub && ours_sub) || (scan.their_sub && ours_pub))) continue; /* roles never meet */
        if (out){
            if (cnt >= max_wants) break;
            out[cnt].index = (uint16_t)scan.pos;
            /* several local topics behind one 32-bit hash: send hash 0 to force the
               wire inline, so whichever topic the name binds to can still verify */
            out[cnt].schema_hash = (nc == 1 && schemas) ? schemas[cidx].hash : 0;
        }
        cnt++;
    }
    return cnt;
}

/* Unresolved candidates for one topic in a peer's interest (see core.h). The same entry
 * walk as detail_wants, filtered to entries that nominate THIS topic by 32-bit hash.
 * Entries with no verdict storage (map alloc failed, already surfaced as
 * INTEREST_OVERFLOW) are NOT counted: they can never resolve, so a wait on them would only
 * ever time out. */
uint16_t dart_transport_topic_unresolved(DartTransportState *st, uint16_t topic_index,
                                         uint32_t peer_id, DartBytes interest){
    const uint8_t *d=interest.data;
    i_DartTopic *topic;
    i_DartInterestScan scan;
    uint16_t n, cnt=0; int slot;
    uint8_t *astate; uint32_t alen;
    int ours_pub, ours_sub;
    topic = i_dart_topic_at(st, topic_index, NULL);
    if (!topic || !i_dart_topic_announced(topic) || !d || interest.len < 2) return 0;
    slot = i_dart_peer_slot(st, peer_id);
    if (slot < 0) return 0;
    n = i_dart_le_r16(d);
    if (!i_dart_interest_walk_len(d, interest.len, n)) return 0;
    ours_pub = dart_role_pubs(topic->role);
    ours_sub = dart_role_subs(topic->role);
    if (!ours_pub && !ours_sub) return 0;
    astate = st->peer_astate[slot]; alen = st->peer_index_len[slot];
    i_dart_interest_scan_init(&scan, d, n);
    while (i_dart_interest_scan_next(&scan)){
        if (scan.hash != (uint32_t)topic->identity) continue;   /* not this topic */
        if (!astate || scan.pos >= alen) continue;          /* unresolvable: never counted */
        if (astate[scan.pos] & DART__AST_DETAILED){
            /* decided (matched or refused) -- but a verified subscriber whose writer lane
               is under the REBIND HOLD is still resolving: it re-forms the moment the
               peer's uDTL request confirms the rebind version, so a send-path match wait
               must cover that window exactly like a pending verdict */
            if (scan.their_sub && ours_pub && (astate[scan.pos] & DART__AST_NAME_OK)
                && topic->rebind_version
                && st->peer_seen_version[slot] < topic->rebind_version) cnt++;
            continue;
        }
        if ((scan.their_pub && ours_sub) || (scan.their_sub && ours_pub)) cnt++;
    }
    return cnt;
}

void dart_transport_peer_unresolved_fill(DartTransportState *st, uint32_t peer_id,
                                         DartBytes interest, uint16_t *counts, uint16_t n){
    const uint8_t *d=interest.data;
    i_DartInterestScan scan;
    uint16_t nent, c; int slot;
    uint8_t *astate; uint16_t *amap; uint32_t alen;
    if (!st || !counts || !n || !d || interest.len < 2) return;
    slot = i_dart_peer_slot(st, peer_id);
    if (slot < 0) return;
    nent = i_dart_le_r16(d);
    if (!i_dart_interest_walk_len(d, interest.len, nent)) return;
    astate = st->peer_astate[slot]; amap = st->peer_index[slot]; alen = st->peer_index_len[slot];
    if (!astate || !amap) return;                        /* unresolvable entries: never counted */
    if (n > st->cfg.n_topics) n = (uint16_t)st->cfg.n_topics;
    i_dart_interest_scan_init(&scan, d, nent);
    while (i_dart_interest_scan_next(&scan)){
        if (scan.pos >= alen) continue;
        if (astate[scan.pos] & DART__AST_DETAILED){
            /* decided: the verdict names the topic. Only a verified subscriber whose writer
               lane sits under the REBIND HOLD still resolves (as in topic_unresolved) */
            uint16_t cidx = amap[scan.pos];
            const i_DartTopic *topic;
            if (!(astate[scan.pos] & DART__AST_NAME_OK) || cidx >= n) continue;
            topic = &st->topics[cidx];
            if (scan.their_sub && dart_role_pubs(topic->role) && topic->rebind_version
                && st->peer_seen_version[slot] < topic->rebind_version && counts[cidx] != 0xFFFFu)
                counts[cidx]++;
            continue;
        }
        for (c=0;c<n;c++){                               /* pending: every topic the hash nominates */
            const i_DartTopic *topic = &st->topics[c];
            int ours_pub, ours_sub;
            if (!i_dart_topic_announced(topic) || (uint32_t)topic->identity != scan.hash) continue;
            ours_pub = dart_role_pubs(topic->role); ours_sub = dart_role_subs(topic->role);
            if (((scan.their_pub && ours_sub) || (scan.their_sub && ours_pub)) && counts[c] != 0xFFFFu)
                counts[c]++;
        }
    }
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
        if (st->peer_attrs[slot]) st->peer_attrs[slot][dd.index] = dd.attrs;
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
    if (!topic || topic->retired) return -1;   /* a retired slot only returns via reuse */
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
           twins would cross-bind and refuse forever; coexistence is a cross-node property.
           RETIRED slots are exempt: they are invisible to matching and either get reused by
           this very name later or stay parked. */
        uint64_t id = dart_topic_identity(def); uint16_t c;
        for (c=0;c<st->cfg.n_topics;c++)
            if (i_dart_topic_announced(&st->topics[c]) && st->topics[c].identity == id
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
    topic->attrs = def->attrs;
    topic->identity = dart_topic_identity(def);
    memcpy((char*)topic->name, def->name, lane); ((char*)topic->name)[lane] = '\0';
    topic->name_len = (uint8_t)lane;
    topic->history_head = 0; topic->next_seqno = 0; topic->have_first = 0;
    /* A DISSOLVED verdict (details arrived, no local topic matched) is only as durable
       as the topic set it was judged against, and that set just grew: send those
       verdicts back to PENDING so the next interest apply re-requests and re-verifies
       them against the new identity. A NAME_OK verdict is bound to an immutable name and
       stays. Without this, an observer that fetched details before subscribing (the
       explorer's flow) could never match a topic it learned about first. The cached
       attrs STAY: they describe the peer's binding (invalidated only by its generation
       gate), and a fetch_details observer's greedy cache never re-asks a fetched index,
       so clearing them here would lose a non-candidate entry's attrs for good. */
    for (p=0;p<st->cfg.max_peers;p++){
        uint8_t *as = st->peer_astate[p]; uint32_t a, alen = st->peer_index_len[p];
        if (!st->peer_used[p] || !as) continue;
        for (a=0;a<alen;a++)
            if ((as[a] & DART__AST_DETAILED) && !(as[a] & DART__AST_NAME_OK))
                as[a] = 0;
    }
    for (p=0;p<st->cfg.max_peers;p++)        /* match the newly active topic to known peers */
        if (st->peer_used[p]) i_dart_topic_rematch(st, topic_index, p);
    return 0;
}


/* Park a defined slot for reuse (see core.h). Every lane releases (rematch under
 * INACTIVE+retired), the history ring and its sample buffers are freed, and the slot
 * leaves the announce. Identity/name/kind/qos/gen/next_seqno stay: reuse compares the
 * old binding against the new one, and an identical rebind continues the seqno line. */
int dart_transport_topic_retire(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic; uint16_t p, d, depth;
    if (!st) return -1;
    topic = i_dart_topic_at(st, topic_index, NULL);
    if (!topic || topic->name_len == 0 || topic->retired) return -1;
    topic->role = DART_INACTIVE; topic->retired = 1;
    for (p=0;p<st->cfg.max_peers;p++)          /* both sides unmatch; every lane releases */
        if (st->peer_used[p]) i_dart_topic_rematch(st, topic_index, p);
    depth = topic->qos.keep_last;
    if (topic->history){
        for (d=0; d<depth; d++)
            if (topic->history[d].buf){
                st->cfg.allocator(st->cfg.user, topic->history[d].buf, 0);
                topic->history[d].buf = NULL; topic->history[d].cap = 0;
            }
        if (topic->history_owned){
            st->cfg.allocator(st->cfg.user, topic->history, 0);
            topic->history = NULL; topic->history_owned = 0;
        } else {
            memset(topic->history, 0, (size_t)depth*sizeof(i_DartWriterSample));
        }
    }
    topic->history_head = 0; topic->have_first = 0; topic->first_seqno = 0;
    memset(&topic->repair_stats, 0, sizeof topic->repair_stats);
    /* caller re-advertises: the slot now rides the announce as a hole */
    return 0;
}


/* The slot a new define should reuse (see core.h): 2 = retired slot with the same
 * identity + kind (an identical-binding candidate), 1 = some retired slot (a rebind),
 * 0 = none. */
int dart_transport_topic_reuse_find(DartTransportState *st, const char *name, uint8_t kind,
                                    uint16_t *index_out){
    uint64_t id; uint16_t c; int any = -1;
    if (!st || !name || !name[0]) return 0;
    id = dart_topic_id(name);
    for (c=0;c<st->cfg.n_topics;c++){
        const i_DartTopic *t = &st->topics[c];
        if (!t->retired) continue;
        if (t->identity == id && t->kind == kind){ if (index_out) *index_out = c; return 2; }
        if (any < 0) any = (int)c;
    }
    if (any >= 0){ if (index_out) *index_out = (uint16_t)any; return 1; }
    return 0;
}


/* Rebind a retired slot to a (possibly new) definition (see core.h for the contract).
 * The seqno line always CONTINUES: for an identical rebind a peer that never observed
 * the retired interval keeps its reader position and sees an idle gap, not a restart;
 * for a changed rebind every lane re-forms fresh anyway, so continuation is harmless. */
int dart_transport_topic_reuse(DartTransportState *st, uint16_t topic_index,
                               const DartTopicDef *def, int binding_changed,
                               uint32_t rebind_version){
    i_DartTopic *topic; DartQos q; uint16_t depth, p; size_t nlen; uint64_t id;
    if (!st || !def) return -1;
    topic = i_dart_topic_at(st, topic_index, NULL);
    if (!topic || !topic->retired) return -1;
    if (!def->name || !def->name[0]) return -1;
    nlen = i_dart_name_len(def->name);
    if (def->name[nlen]) return -1;                          /* longer than DART_TOPIC_NAME_MAX */
    if (def->directed && def->qos.catch_up) return -1;       /* directed history never replays */
    id = dart_topic_identity(def);
    {   /* same name under a different kind on ONE node: refuse, exactly as at define
           (other retired slots are exempt; this slot is the one being rebound) */
        uint16_t c;
        for (c=0;c<st->cfg.n_topics;c++)
            if (c != topic_index && i_dart_topic_announced(&st->topics[c])
                && st->topics[c].identity == id && st->topics[c].kind != def->kind) return -1;
    }
    if (!binding_changed && (topic->identity != id || topic->kind != def->kind))
        return -1;                       /* asserted identical, but the stored binding differs */
    q = def->qos; i_dart_qos_defaults(&q);
    depth = q.keep_last;
    if (topic->history && !topic->history_owned && depth <= topic->qos.keep_last){
        /* an at-init arena ring that still fits: reuse it in place */
        memset(topic->history, 0, (size_t)depth*sizeof(i_DartWriterSample));
    } else {
        i_DartWriterSample *h = (i_DartWriterSample*)st->cfg.allocator(st->cfg.user, NULL,
                                                     (size_t)depth*sizeof(i_DartWriterSample));
        if (!h) return -4;                                   /* OOM: the slot stays retired */
        memset(h, 0, (size_t)depth*sizeof(i_DartWriterSample));
        topic->history = h; topic->history_owned = 1;
    }
    topic->qos = q;
    topic->role = def->role;
    topic->kind = def->kind; topic->prefix_bytes = def->prefix_bytes; topic->directed = def->directed;
    topic->attrs = def->attrs;
    topic->identity = id;
    memcpy((char*)topic->name, def->name, nlen); ((char*)topic->name)[nlen] = '\0';
    topic->name_len = (uint8_t)nlen;
    topic->history_head = 0; topic->have_first = 0; topic->first_seqno = 0;
    memset(&topic->repair_stats, 0, sizeof topic->repair_stats);
    topic->retired = 0;
    if (binding_changed){
        topic->gen = (uint8_t)(topic->gen + 1u);   /* announced; peers compare equality only */
        topic->rebind_version = rebind_version;
        for (p=0;p<st->cfg.max_peers;p++){
            uint32_t a;
            if (!st->peer_used[p]) continue;
            /* sever OUR bindings to the old occupant: verdicts of peer entries bound to
               this slot re-pend (their announces re-verify against the new binding), and
               the interest bits derived from them go too */
            if (st->peer_index[p])
                for (a=0;a<st->peer_index_len[p];a++)
                    if (st->peer_index[p][a] == topic_index){
                        st->peer_index[p][a] = 0xFFFFu;
                        st->peer_astate[p][a] = 0;
                        if (st->peer_attrs[p]) st->peer_attrs[p][a] = 0;
                    }
            i_dart_bit_clr(&st->peer_pub_bitmap[(size_t)p*st->bitmap_len], topic_index);
            i_dart_bit_clr(&st->peer_sub_bitmap[(size_t)p*st->bitmap_len], topic_index);
            i_dart_bit_clr(&st->peer_sub_reliable[(size_t)p*st->bitmap_len], topic_index);
            /* a DISSOLVED verdict is only as durable as the topic set it was judged
               against (see dart_transport_topic_define), and that set just changed.
               Attrs stay, as there. */
            if (st->peer_astate[p]){
                uint8_t *as = st->peer_astate[p];
                for (a=0;a<st->peer_index_len[p];a++)
                    if ((as[a] & DART__AST_DETAILED) && !(as[a] & DART__AST_NAME_OK))
                        as[a] = 0;
            }
        }
    }
    for (p=0;p<st->cfg.max_peers;p++)
        if (st->peer_used[p]) i_dart_topic_rematch(st, topic_index, p);
    return 0;
}


/* A peer named OUR blob version `version` in a uDTL request: record it and release any
 * writer lanes its advance takes out of the rebind hold. Returns 1 when a lane actually
 * formed (the caller then re-fires its interest event / invalidates match memos). */
int dart_transport_peer_seen_version(DartTransportState *st, uint32_t peer_id, uint32_t version){
    int s; uint16_t c; uint32_t old; int changed = 0;
    if (!st) return 0;
    s = i_dart_peer_slot(st, peer_id);
    if (s < 0) return 0;
    old = st->peer_seen_version[s];
    if (version <= old) return 0;
    st->peer_seen_version[s] = version;
    for (c=0;c<st->cfg.n_topics;c++){
        i_DartTopic *t = &st->topics[c];
        i_DartLane *l; int was;
        if (!t->rebind_version || t->rebind_version <= old || t->rebind_version > version) continue;
        if (!i_dart_topic_announced(t)) continue;
        l = i_dart_lane_at(st, c, (uint32_t)s); was = l && l->w.used;
        i_dart_topic_rematch(st, c, (uint16_t)s);
        l = i_dart_lane_at(st, c, (uint32_t)s);
        if ((l && l->w.used) != was) changed = 1;
    }
    return changed;
}


/* The topic's next write seqno (== samples ever committed on this slot's line, which
 * CONTINUES across a retire/reuse cycle). A pattern layer seeds its own per-slot
 * monotonic counters from it so a successor's first write never orders below its
 * predecessor's last at a peer that kept state across the cycle. */
uint64_t dart_transport_topic_seqno(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = st ? i_dart_topic_at(st, topic_index, NULL) : NULL;
    return topic ? topic->next_seqno : 0;
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

uint8_t dart_transport_topic_attrs(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    return topic ? topic->attrs : 0;
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
                case DART_NACK: i_dart_writer_nack(st,topic_index,peer_slot,p,now); break;
            }
        }
        p+=sub; rem-=sub;
    }
}
