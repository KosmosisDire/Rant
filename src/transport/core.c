/* The transport core: state, init, peers, interest matching, the RX demux and queries.
 * The codec, scheduler, writer and reader paths live in the sibling files. */
#include "core.h"
#include "../common/bytes.h"
#include "../common/arena.h"
#include "../common/hash.h"
#include "internal.h"
#include <string.h>


/* The wire name is the whole name, so the identity recomputed from it equals rant_topic_id. */
static uint64_t i_rant_identity_hash(const uint8_t *name, size_t n){ return i_rant_fnv1a64(name, n); }

uint64_t rant_topic_id(const char *name){ return i_rant_fnv1a64_str(name); }

uint64_t rant_topic_identity(const RantTopicDef *def){
    return rant_topic_id(def->name);
}

static size_t i_rant_name_len(const char *s){              /* capped strlen */
    size_t n = 0;
    if (s) while (s[n] && n < RANT_TOPIC_NAME_MAX) n++;
    return n;
}


/* Applied once at init so the stored qos is authoritative. repair_delay_us 0 stays
   adaptive and is resolved per NACK in i_rant_reader_emit. */
static void i_rant_qos_defaults(RantQos *q){
    if (q->keep_last == 0)        q->keep_last       = q->reliability==RANT_RELIABLE
                                                     ? RANT_QOS_DEF_KEEP_LAST_REL
                                                     : RANT_QOS_DEF_KEEP_LAST;
    if (q->heartbeat_us == 0)     q->heartbeat_us    = RANT_QOS_DEF_HEARTBEAT_US;
}


uint16_t rant_clamp_frag(uint16_t frag_size){
    uint16_t f = frag_size ? frag_size : RANT_FRAG_SIZE;
    if (f < RANT_FRAG_SIZE_MIN) f = RANT_FRAG_SIZE_MIN;
    if (f > RANT_FRAG_SIZE_MAX) f = RANT_FRAG_SIZE_MAX;
    return f;
}

uint16_t rant_transport_frag(RantTransportState *st){ return st ? st->frag : rant_clamp_frag(0); }


/* Lays everything out, measure mode when b->base is NULL. The arena holds only the fixed
   tables. Message buffers, reassembly state, index maps and lane records are hook allocations. */
static RantTransportState *i_rant_transport_build(i_RantBump *b, const RantConfig *cfg){
    uint16_t c; uint32_t max_peers = cfg->max_peers, n_topics = cfg->n_topics;
    uint16_t bitmap_len = (uint16_t)((n_topics+7u)/8u);
    uint32_t name_bytes = 0; char *name_pool = NULL;
    RantTransportState *st = (RantTransportState*)i_rant_bump_take(b, sizeof(RantTransportState), 16);
    if (st && b->base) memset(st, 0, sizeof(*st));

    /* one fixed name slot per topic, so a reserve slot can be named later */
    name_bytes = (uint32_t)n_topics * (RANT_TOPIC_NAME_MAX + 1u);

    { uint32_t nlanes = n_topics*max_peers, ndest = max_peers;
      uint32_t *peer_ids = (uint32_t*)i_rant_bump_take(b, max_peers*sizeof(uint32_t), 8);
      uint8_t  *peer_used = (uint8_t*) i_rant_bump_take(b, max_peers*sizeof(uint8_t), 1);
      uint8_t  *peer_dormant= (uint8_t*) i_rant_bump_take(b, max_peers*sizeof(uint8_t), 1);
      uint16_t *peer_frag = (uint16_t*)i_rant_bump_take(b, max_peers*sizeof(uint16_t), 2);
      RantPeerRtt *peer_rtt = (RantPeerRtt*)i_rant_bump_take(b, max_peers*sizeof(RantPeerRtt), 8);
#ifdef RANT_SHM
      uint8_t  *peer_shm= (uint8_t*) i_rant_bump_take(b, max_peers*sizeof(uint8_t), 1);
#endif
      uint8_t  *peer_pub_bitmap = (uint8_t*) i_rant_bump_take(b, (size_t)max_peers*bitmap_len, 1);
      uint8_t  *peer_sub_bitmap = (uint8_t*) i_rant_bump_take(b, (size_t)max_peers*bitmap_len, 1);
      uint8_t  *peer_sub_reliable = (uint8_t*) i_rant_bump_take(b, (size_t)max_peers*bitmap_len, 1);
      i_RantTopic *topic = (i_RantTopic*)i_rant_bump_take(b, n_topics*sizeof(i_RantTopic), 16);
      /* only the u16 ticket table lives in the arena, records are pool allocated per match */
      uint16_t   *lane_index = (uint16_t*)i_rant_bump_take(b, (size_t)nlanes*sizeof(uint16_t), 2);
      uint32_t *dest_head = (uint32_t*)i_rant_bump_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint32_t *dest_tail = (uint32_t*)i_rant_bump_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint8_t  *dest_queued = (uint8_t*) i_rant_bump_take(b, (size_t)ndest, 1);
      uint32_t *dest_queue = (uint32_t*)i_rant_bump_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      /* index maps: a per peer pointer and length, each map allocated on demand */
      uint16_t **peer_index    = (uint16_t**)i_rant_bump_take(b, (size_t)max_peers*sizeof(uint16_t*), 8);
      uint32_t *peer_index_len = (uint32_t*) i_rant_bump_take(b, (size_t)max_peers*sizeof(uint32_t), 8);
      uint8_t **peer_astate    = (uint8_t**) i_rant_bump_take(b, (size_t)max_peers*sizeof(uint8_t*), 8);
      uint8_t **peer_agen      = (uint8_t**) i_rant_bump_take(b, (size_t)max_peers*sizeof(uint8_t*), 8);
      uint8_t **peer_attrs     = (uint8_t**) i_rant_bump_take(b, (size_t)max_peers*sizeof(uint8_t*), 8);
      uint32_t *peer_seen_version = (uint32_t*)i_rant_bump_take(b, (size_t)max_peers*sizeof(uint32_t), 8);
      name_pool = (char*)i_rant_bump_take(b, name_bytes ? name_bytes : 1u, 1);
      if (st && b->base){
          st->cfg=*cfg; st->peer_ids=peer_ids; st->peer_used=peer_used;
          st->peer_dormant=peer_dormant; st->peer_frag=peer_frag;
          st->peer_rtt=peer_rtt; memset(peer_rtt, 0, max_peers*sizeof(RantPeerRtt));
          st->frag = rant_clamp_frag(cfg->frag_size);
          st->peer_pub_bitmap=peer_pub_bitmap; st->peer_sub_bitmap=peer_sub_bitmap;
          st->peer_sub_reliable=peer_sub_reliable; st->bitmap_len=bitmap_len;
          st->topics=topic; st->reader_epoch_counter=1;
          st->next_deadline_us=RANT__NO_DEADLINE;
          st->lanes=NULL; st->lane_cap=0u; st->lane_free=RANT__NIL;
          st->lane_index=lane_index;
          st->dest_head=dest_head; st->dest_tail=dest_tail; st->dest_queued=dest_queued; st->dest_queue=dest_queue;
          st->peer_index=peer_index; st->peer_index_len=peer_index_len;
          st->peer_astate=peer_astate;
          st->peer_agen=peer_agen; st->peer_attrs=peer_attrs;
          st->peer_seen_version=peer_seen_version;
          memset(peer_used,0,max_peers); memset(peer_dormant,0,max_peers);
          { uint32_t k; for (k=0;k<max_peers;k++) peer_frag[k]=RANT_FRAG_SIZE; }
#ifdef RANT_SHM
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
          memset(dest_head,0xFF,(size_t)ndest*sizeof(uint32_t));   /* all RANT__NIL */
      }
    }

    for (c=0;c<n_topics;c++){
        /* every slot starts inactive with its own name slot */
        if (st && b->base){
            i_RantTopic *topic = &st->topics[c];
            memset(topic,0,sizeof(*topic));
            topic->role = RANT_INACTIVE;
            topic->lane_head = RANT__NIL;
            topic->name = name_pool + (size_t)c*(RANT_TOPIC_NAME_MAX + 1u);
            ((char*)topic->name)[0] = '\0';
        }
        if (!cfg->topics) continue;    /* reserve mode: slots are filled by topic_define later */
        {   const RantTopicDef *def = &cfg->topics[c];
            RantQos q = def->qos;              /* a normalized copy */
            i_RantWriterSample *history; uint16_t depth;
            i_rant_qos_defaults(&q);
            depth = q.keep_last;
            history = (i_RantWriterSample*)i_rant_bump_take(b, depth*sizeof(i_RantWriterSample), 16);
            if (st && b->base){
                i_RantTopic *topic = &st->topics[c];
                size_t lane = i_rant_name_len(def->name);
                topic->qos=q;
                topic->role=def->role;
                topic->kind=def->kind; topic->prefix_bytes=def->prefix_bytes; topic->directed=def->directed;
                topic->attrs=def->attrs;
                topic->identity = rant_topic_identity(def);
                if (lane){ memcpy((char*)topic->name, def->name, lane); ((char*)topic->name)[lane]='\0'; }
                topic->name_len = (uint8_t)lane;
                topic->history=history; topic->history_owned=0; topic->history_head=0; topic->next_seqno=0; topic->have_first=0;
                memset(history,0,depth*sizeof(i_RantWriterSample));     /* buffers grow lazily */
            }
        }
    }
    return st;
}


size_t rant_transport_required_memory(const RantConfig *cfg){
    i_RantBump b; memset(&b,0,sizeof b);
    if (!cfg || cfg->n_topics==0 || cfg->max_peers==0) return 0;
    i_rant_transport_build(&b, cfg);
    return b.offset + 16;   /* slack for base alignment */
}


RantTransportState *rant_transport_init(void *mem, size_t cap, const RantConfig *cfg){
    i_RantBump b; RantTransportState *st; uint16_t i;
    if (!mem || !cfg || cfg->n_topics==0 || cfg->max_peers==0) return NULL;
    if (!cfg->allocator) return NULL;    /* the allocator is the one memory model */
    if (cfg->topics) for (i=0;i<cfg->n_topics;i++){
        const RantTopicDef *d = &cfg->topics[i];
        size_t lane = 0; uint16_t j;
        if (!d->name || !d->name[0]) return NULL;          /* the name is the identity */
        while (d->name[lane]) lane++;
        if (lane > RANT_TOPIC_NAME_MAX) return NULL;             /* the wire name is the whole name */
        if (d->directed && d->qos.catch_up) return NULL;   /* directed history never replays */
        for (j=0;j<i;j++)   /* same name under a different kind: the maps bind by identity, so
                               local twins would cross bind. See spec/interest.md */
            if (cfg->topics[j].kind != d->kind &&
                rant_topic_id(cfg->topics[j].name) == rant_topic_id(d->name)) return NULL;
    }
    memset(&b,0,sizeof b);
    b.base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    b.cap  = cap - (size_t)((uint8_t*)b.base - (uint8_t*)mem);
    st = i_rant_transport_build(&b, cfg);
    if (!st || b.oom) return NULL;
    st->cfg.topics = NULL;   /* only read during init */
    return st;
}


/* Heap buffers stay put and the struct copies carry their pointers. The scheduler is
 * rebuilt from proxy state. The caller frees old's arena but must not destroy old. */
RantTransportState *rant_transport_migrate(RantTransportState *old, void *new_mem, size_t new_cap,
                        uint16_t new_max_peers, uint16_t new_n_topics){
    RantConfig nc; RantTransportState *nw; uint16_t omp, onc, c, p;
    if (!old) return NULL;
    nc = old->cfg; nc.topics = NULL;
    nc.max_peers = new_max_peers; nc.n_topics = new_n_topics;
    nw = rant_transport_init(new_mem, new_cap, &nc);
    if (!nw) return NULL;
    omp = old->cfg.max_peers; onc = old->cfg.n_topics;

    nw->reader_epoch_counter = old->reader_epoch_counter;
    nw->frag = old->frag;
    memcpy(nw->peer_ids,     old->peer_ids,     (size_t)omp*sizeof(uint32_t));
    memcpy(nw->peer_used,    old->peer_used,    omp);
    memcpy(nw->peer_dormant, old->peer_dormant, omp);
    memcpy(nw->peer_frag,    old->peer_frag,    (size_t)omp*sizeof(uint16_t));
    memcpy(nw->peer_rtt,     old->peer_rtt,     (size_t)omp*sizeof(RantPeerRtt));
#ifdef RANT_SHM
    memcpy(nw->peer_shm,     old->peer_shm,     omp);
#endif
    /* keep the new name slot pointer, carry everything else, re copy the name */
    for (c=0;c<onc;c++){
        char *nm = (char*)nw->topics[c].name;
        size_t l = old->topics[c].name_len;
        nw->topics[c] = old->topics[c];
        nw->topics[c].name = nm;
        if (l) memcpy(nm, old->topics[c].name, l);
        nm[l] = '\0';
    }
    /* the pool is one hook allocation outside both arenas, so adopt it whole. Only the
       ticket table is arena memory and re strides. */
    nw->lanes = old->lanes; nw->lane_cap = old->lane_cap; nw->lane_free = old->lane_free;
    for (c=0;c<onc;c++) for (p=0;p<omp;p++)
        nw->lane_index[(size_t)c*new_max_peers+p] = old->lane_index[(size_t)c*omp+p];
    {   uint32_t li;   /* the old scheduler dies with the arena, free records keep sched_next
                       as their free link */
        for (li=0; li<nw->lane_cap; li++)
            if (nw->lanes[li].in_use){ nw->lanes[li].queued=0; nw->lanes[li].sched_next=RANT__NIL; }
    }
    /* per peer interest bitmaps re stride with n_topics, the maps are stable hook allocations */
    for (p=0;p<omp;p++){
        memcpy(nw->peer_pub_bitmap + (size_t)p*nw->bitmap_len,
               old->peer_pub_bitmap + (size_t)p*old->bitmap_len, old->bitmap_len);
        memcpy(nw->peer_sub_bitmap + (size_t)p*nw->bitmap_len,
               old->peer_sub_bitmap + (size_t)p*old->bitmap_len, old->bitmap_len);
        memcpy(nw->peer_sub_reliable + (size_t)p*nw->bitmap_len,
               old->peer_sub_reliable + (size_t)p*old->bitmap_len, old->bitmap_len);
        nw->peer_index[p]     = old->peer_index[p];
        nw->peer_index_len[p] = old->peer_index_len[p];
        nw->peer_astate[p]    = old->peer_astate[p];
        nw->peer_agen[p]      = old->peer_agen[p];
        nw->peer_attrs[p]     = old->peer_attrs[p];
        nw->peer_seen_version[p] = old->peer_seen_version[p];
    }
    /* re enqueue every used lane and force a full sweep next poll, so timers re arm */
    for (c=0;c<onc;c++)
        for (p=0;p<omp;p++) if (nw->peer_used[p]) i_rant_lane_wake(nw, c, p);
    nw->next_deadline_us = 0;
    return nw;
}


int i_rant_peer_slot(RantTransportState *st, uint32_t id){
    uint16_t i;
    for (i=0;i<st->cfg.max_peers;i++) if (st->peer_used[i] && st->peer_ids[i]==id) return (int)i;
    return -1;
}

/* the local handle is the index */
i_RantTopic *i_rant_topic_at(RantTransportState *st, uint16_t topic_index, int *idx_out){
    if (topic_index >= st->cfg.n_topics) return NULL;
    if (idx_out) *idx_out = (int)topic_index;
    return &st->topics[topic_index];
}

/* Prefers a non INACTIVE match so a parked twin never shadows a live one, else the first
 * match so resolution stays deterministic. A RETIRED slot is invisible here. */
static i_RantTopic *i_rant_topic_by_identity(RantTransportState *st, uint64_t identity, int *idx_out){
    uint16_t i; int first=-1;
    for (i=0;i<st->cfg.n_topics;i++){
        if (st->topics[i].retired || st->topics[i].identity!=identity) continue;
        if (first<0) first=(int)i;
        if (st->topics[i].role!=RANT_INACTIVE){ if(idx_out)*idx_out=(int)i; return &st->topics[i]; }
    }
    if (first>=0){ if(idx_out)*idx_out=first; return &st->topics[first]; }
    return NULL;
}


/* first and count are the kind's two numeric slots, routed to the named fields. */
void i_rant_transport_fire_event(RantTransportState *st, RantTransportEventKind kind, uint16_t topic_index,
                        uint32_t peer, uint64_t first, uint64_t count){
    RantTransportEvent ev;
    if (!st->cfg.on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind=kind; ev.topic=topic_index; ev.peer=peer; ev.user=st->cfg.user;
    switch (kind){
    case RANT_TRANSPORT_MSG_LOST:         ev.lost_first = first; ev.lost_count = count; break;
    case RANT_TRANSPORT_MSG_TOO_BIG:      ev.too_big_bytes = count; break;
    case RANT_TRANSPORT_NAME_COLLISION: ev.identity = first; break;
    case RANT_TRANSPORT_SCHEMA_MISMATCH: ev.peer_is_pub = (uint8_t)first; break;
    default: break;
    }
    st->cfg.on_event(&ev);
}


/* the head minus catch_up cached samples, reliable only */
uint64_t i_rant_topic_unicast_join_seqno(const i_RantTopic *topic){
    uint16_t depth = topic->qos.keep_last;   /* normalized at init */
    uint16_t want = topic->qos.catch_up, k, i;
    uint64_t s = topic->next_seqno;
    if (topic->qos.reliability != RANT_RELIABLE || want == 0) return s;
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


/* Gets or allocates the lane record. The pool doubles and records move, so callers re
 * derive pointers after this. NULL on OOM or a full ticket space, the next announce retries. */
static i_RantLane *i_rant_lane_ensure(RantTransportState *st, uint16_t c, uint32_t peer_slot){
    size_t k = (size_t)c*st->cfg.max_peers + peer_slot;
    uint32_t li;
    li = st->lane_index[k]==0xFFFFu ? RANT__NIL : (uint32_t)st->lane_index[k];
    if (li != RANT__NIL) return &st->lanes[li];
    if (st->lane_free == RANT__NIL){                    /* pool dry: grow it */
        uint32_t ncap = st->lane_cap ? st->lane_cap*2u : 8u, i;
        i_RantLane *nl;
        if (ncap > 0xFFFFu) ncap = 0xFFFFu;           /* the u16 ticket space */
        if (ncap <= st->lane_cap) return NULL;        /* 65535 live matches: refuse */
        nl = (i_RantLane*)st->cfg.allocator(st->cfg.user, st->lanes, (size_t)ncap*sizeof(i_RantLane));
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
    memset(&st->lanes[li], 0, sizeof(i_RantLane));
    st->lanes[li].topic = c; st->lanes[li].peer_slot = (uint16_t)peer_slot;
    st->lanes[li].sched_next = RANT__NIL; st->lanes[li].topic_next = RANT__NIL;
    st->lanes[li].in_use = 1;
    st->lane_index[k] = (uint16_t)li;
    return &st->lanes[li];
}

/* free one assembly slot's grown buffers */
static void i_rant_asm_free(RantTransportState *st, i_RantAssembly *a){
    if (a->buf){ st->cfg.allocator(st->cfg.user, a->buf, 0); a->buf=NULL; a->cap=0; }
    if (a->bitmap){ st->cfg.allocator(st->cfg.user, a->bitmap, 0); a->bitmap=NULL; a->bitmap_cap=0; }
    a->active=0;
}

/* A lane with neither side matched leaves the topic chain, frees its buffers, leaves the
 * scheduler and recycles its record. A no op while a side is matched. */
static void i_rant_lane_release(RantTransportState *st, uint16_t c, uint32_t peer_slot){
    uint32_t li = i_rant_lane_id(st, c, peer_slot);
    i_RantLane *l;
    if (li == RANT__NIL) return;
    l = &st->lanes[li];
    if (l->w.used || l->r.used) return;
    {   uint32_t *pp = &st->topics[c].lane_head;    /* unlink from the topic chain */
        while (*pp != RANT__NIL && *pp != li) pp = &st->lanes[*pp].topic_next;
        if (*pp == li) *pp = l->topic_next;
    }
    l->topic_next = RANT__NIL;
    i_rant_asm_free(st, &l->r.cur); i_rant_asm_free(st, &l->r.next);
    i_rant_sched_drop(st, li);
    l->in_use = 0;
    l->sched_next = st->lane_free; st->lane_free = li;
    st->lane_index[(size_t)c*st->cfg.max_peers + peer_slot] = 0xFFFF;
}

/* match one lane side */
static void i_rant_writer_match(RantTransportState *st, uint16_t c, uint16_t peer_slot, i_RantLane *l){
    i_RantTopic *topic=&st->topics[c];
    i_RantWriterProxy *w=&l->w;
    memset(w,0,sizeof(*w));
    w->used=1;
    topic->matched_writers++;   /* a genuine 0 to 1, rematch guards on used */
    /* only a reliable reader acks. A best effort one stays out of flow control so it can
       never stall this writer */
    w->reader_reliable = i_rant_bit_get(&st->peer_sub_reliable[(size_t)peer_slot*st->bitmap_len], c) ? 1u : 0u;
    w->sent_upto = i_rant_topic_unicast_join_seqno(topic);
    w->acked_upto = w->sent_upto;
    w->wire_skip = w->sent_upto;   /* the private wire seqno starts at 0 */
    i_rant_lane_wake(st, c, peer_slot);     /* primed for new data and ack or hb */
}

static void i_rant_writer_unmatch(RantTransportState *st, uint16_t c, i_RantLane *l){
    if (!l->w.used) return;
    l->w.used=0;
    st->topics[c].matched_writers--;   /* exactly one 1 to 0 per unmatch */
}

static void i_rant_reader_match(RantTransportState *st, uint16_t c, uint16_t peer_slot, i_RantLane *l){
    i_RantReaderProxy *r=&l->r;
    i_RantAssembly cur=r->cur, next=r->next;     /* keep the grown buffers across rematch */
    memset(r,0,sizeof(*r));
    r->cur=cur; r->next=next; r->cur.active=0; r->next.active=0;
    r->epoch=st->reader_epoch_counter++;   /* a new incarnation: writers re join on seeing it */
    r->used=1;       /* started 0: the first DATA adopts the writer's position */
    st->topics[c].matched_readers++;   /* a genuine 0 to 1, rematch guards on used */
    /* announce the incarnation once so an idle writer re joins and replays. A discovery
       blip keeps its position through dormant and resume and never lands here. */
    if (st->topics[c].qos.reliability==RANT_RELIABLE){
        r->ack_pending=1; r->ack_due_us=0; r->ack_force=1;
        i_rant_lane_wake(st,c,peer_slot);
    }
}

static void i_rant_reader_unmatch(RantTransportState *st, uint16_t c, i_RantLane *l){
    if (l->r.used) st->topics[c].matched_readers--;   /* peer_remove calls this unconditionally */
    l->r.used=0; l->r.cur.active=0; l->r.next.active=0;
}


/* Recomputes one lane from our role and the peer's bits. Only a changed side is touched,
 * so reader positions survive a re apply. */
static void i_rant_topic_rematch(RantTransportState *st, uint16_t c, uint16_t peer_slot){
    i_RantTopic *topic=&st->topics[c];
    const uint8_t *peer_pub_bitmap=&st->peer_pub_bitmap[(size_t)peer_slot*st->bitmap_len];
    const uint8_t *peer_sub_bitmap=&st->peer_sub_bitmap[(size_t)peer_slot*st->bitmap_len];
    int wuse = rant_role_pubs(topic->role) && i_rant_bit_get(peer_sub_bitmap,c);
    int ruse = rant_role_subs(topic->role) && i_rant_bit_get(peer_pub_bitmap,c);
    i_RantLane *l;
    /* the rebind hold: nothing goes to a peer until it proved it applied our announce at
       the slot's rebind version. Inbound needs no hold, our own verdicts were re pended. */
    if (wuse && topic->rebind_version && st->peer_seen_version[peer_slot] < topic->rebind_version)
        wuse = 0;
    l = i_rant_lane_at(st,c,peer_slot);
    int had = l && (l->w.used || l->r.used);
    if (!wuse && !ruse){
        if (!had) return;
        i_rant_writer_unmatch(st,c,l);
        i_rant_reader_unmatch(st,c,l);
        i_rant_lane_release(st,c,peer_slot);
        return;
    }
    if (!l){
        l = i_rant_lane_ensure(st,c,peer_slot);     /* may relocate the pool */
        if (!l) return;                             /* OOM: refused, the next announce retries */
    }
    if (wuse && !l->w.used) i_rant_writer_match(st,c,peer_slot,l);
    else if (!wuse && l->w.used) i_rant_writer_unmatch(st,c,l);
    if (ruse && !l->r.used) i_rant_reader_match(st,c,peer_slot,l);
    else if (!ruse && l->r.used) i_rant_reader_unmatch(st,c,l);
    if (!had && (l->w.used || l->r.used)){        /* a first match: onto the topic chain */
        l->topic_next = topic->lane_head;
        topic->lane_head = i_rant_lane_id(st,c,peer_slot);
    } else if (had && !(l->w.used || l->r.used)){
        i_rant_lane_release(st,c,peer_slot);
    }
}


void rant_transport_peer_add(RantTransportState *st, uint32_t id, uint16_t peer_frag){
    uint16_t i; int free=-1; uint32_t max_peers=st->cfg.max_peers;
    if (i_rant_peer_slot(st,id)>=0) return;
    for (i=0;i<max_peers;i++) if(!st->peer_used[i]){free=(int)i;break;}
    if (free<0) return;
    st->peer_used[free]=1; st->peer_ids[free]=id;
    memset(&st->peer_rtt[free], 0, sizeof(RantPeerRtt));     /* a new peer starts unmeasured */
    st->peer_dormant[free]=0;
    st->peer_frag[free]=rant_clamp_frag(peer_frag);
#ifdef RANT_SHM
    st->peer_shm[free]=0;   /* the node sets it on attach */
#endif
    memset(&st->peer_pub_bitmap[(size_t)free*st->bitmap_len],0,st->bitmap_len);
    memset(&st->peer_sub_bitmap[(size_t)free*st->bitmap_len],0,st->bitmap_len);
    memset(&st->peer_sub_reliable[(size_t)free*st->bitmap_len],0,st->bitmap_len);
    if (st->peer_index[free] && st->peer_index_len[free]){   /* slot reuse: no stale state */
        memset(st->peer_index[free],0xFF,(size_t)st->peer_index_len[free]*sizeof(uint16_t));
        if (st->peer_astate[free]) memset(st->peer_astate[free],0,st->peer_index_len[free]);
        if (st->peer_agen[free])   memset(st->peer_agen[free],0,st->peer_index_len[free]);
        if (st->peer_attrs[free])  memset(st->peer_attrs[free],0,st->peer_index_len[free]);
    }
    st->peer_seen_version[free]=0;   /* seen versions are per incarnation */
    /* nothing matches until apply_peer_interest feeds the peer's interest */
}


void rant_transport_peer_remove(RantTransportState *st, uint32_t id){
    int s = i_rant_peer_slot(st,id); uint16_t c;
    if (s<0) return;
    for (c=0;c<st->cfg.n_topics;c++){
        i_RantLane *l = i_rant_lane_at(st,c,(uint32_t)s);
        if (!l) continue;
        i_rant_writer_unmatch(st,c,l);
        i_rant_reader_unmatch(st,c,l);
        /* release frees the lane's buffers too, so a gone peer keeps no memory */
        i_rant_lane_release(st,c,(uint32_t)s);
    }
    if (st->peer_index[s]){                        /* the index and verdict maps go too */
        st->cfg.allocator(st->cfg.user, st->peer_index[s], 0);
        if (st->peer_astate[s]) st->cfg.allocator(st->cfg.user, st->peer_astate[s], 0);
        if (st->peer_agen[s])   st->cfg.allocator(st->cfg.user, st->peer_agen[s], 0);
        if (st->peer_attrs[s])  st->cfg.allocator(st->cfg.user, st->peer_attrs[s], 0);
        st->peer_index[s]=NULL; st->peer_astate[s]=NULL; st->peer_agen[s]=NULL;
        st->peer_attrs[s]=NULL; st->peer_index_len[s]=0;
    }
    st->peer_seen_version[s]=0;
    st->peer_used[s]=0; st->peer_dormant[s]=0;
    memset(&st->peer_rtt[s], 0, sizeof(RantPeerRtt));
#ifdef RANT_SHM
    st->peer_shm[s]=0;
#endif
}

/* The per peer round trip estimator. See spec/transport.md. */

/* Folds one unambiguous sample with the RFC 6298 weights. The first seeds both. */
void i_rant_rtt_sample(RantTransportState *st, uint32_t peer_slot, uint64_t sample_us){
    RantPeerRtt *e = &st->peer_rtt[peer_slot];
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

/* smoothed plus max(one tick, 4 x deviation), never under RANT_RTO_MIN_US, and
 * fallback_us until the first sample */
uint32_t i_rant_rtt_rto(RantTransportState *st, uint32_t peer_slot, uint32_t fallback_us){
    const RantPeerRtt *e = &st->peer_rtt[peer_slot];
    uint64_t var, rto;
    if (e->samples == 0) return fallback_us;
    var = 4ull * e->rtt_jitter_us;
    if (var < RANT_RTO_GRAIN_US) var = RANT_RTO_GRAIN_US;
    rto = (uint64_t)e->rtt_us + var;
    if (rto < RANT_RTO_MIN_US) rto = RANT_RTO_MIN_US;
    return rto > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)rto;
}

int rant_transport_peer_rtt(RantTransportState *st, uint32_t peer_id, RantPeerRtt *out){
    int s = st ? i_rant_peer_slot(st, peer_id) : -1;
    if (out) memset(out, 0, sizeof *out);
    if (s < 0) return 0;
    if (out) *out = st->peer_rtt[s];
    return 1;
}


/* Keeps every proxy and reader position, drops the peer from flow control. */
void rant_transport_peer_dormant(RantTransportState *st, uint32_t id){
    int s = i_rant_peer_slot(st,id);
    if (s>=0) st->peer_dormant[s]=1;
}


/* Re includes the peer and re reports each reader position, so the writer fills any gap
 * and the reader dedups any replay. */
void rant_transport_peer_resume(RantTransportState *st, uint32_t id){
    int s = i_rant_peer_slot(st,id); uint16_t c;
    if (s<0) return;
    st->peer_dormant[s]=0;
    for (c=0;c<st->cfg.n_topics;c++){
        i_RantLane *l=i_rant_lane_at(st,c,(uint32_t)s);
        if (!l) continue;
        if (st->topics[c].qos.reliability!=RANT_RELIABLE) continue;
        if (l->r.used){ l->r.ack_pending=1; l->r.ack_due_us=0; l->r.ack_force=1; }
        if (l->w.used || l->r.used) i_rant_lane_wake(st,c,(uint16_t)s);
    }
}


void rant_transport_peer_set_frag(RantTransportState *st, uint32_t id, uint16_t peer_frag){
    int s = i_rant_peer_slot(st,id);
    if (s>=0) st->peer_frag[s]=rant_clamp_frag(peer_frag);
}

#ifdef RANT_SHM

void rant_transport_peer_set_shm(RantTransportState *st, uint32_t id, int is_shm){
    int s = i_rant_peer_slot(st,id);
    if (s>=0) st->peer_shm[s]=(uint8_t)(is_shm?1:0);
}
#endif


void rant_transport_destroy(RantTransportState *st){
    uint16_t c; uint32_t li;
    if (!st) return;
    for (c=0;c<st->cfg.n_topics;c++){
        i_RantTopic *topic=&st->topics[c];       /* an undefined slot has no ring */
        uint16_t depth, d;
        if (!topic->history) continue;         /* a retired slot freed its ring */
        depth = topic->qos.keep_last;
        for (d=0; d<depth; d++)
            if (topic->history[d].buf){ st->cfg.allocator(st->cfg.user, topic->history[d].buf, 0);
                                  topic->history[d].buf=NULL; topic->history[d].cap=0; }
        if (topic->history_owned && topic->history){   /* the ring came from topic_define */
            st->cfg.allocator(st->cfg.user, topic->history, 0);
            topic->history=NULL; topic->history_owned=0;
        }
    }
    for (li=0; li<st->lane_cap; li++){           /* live records' grown buffers */
        i_RantLane *l=&st->lanes[li];
        if (!l->in_use) continue;
        i_rant_asm_free(st, &l->r.cur); i_rant_asm_free(st, &l->r.next);
    }
    if (st->lanes){                              /* the record pool */
        st->cfg.allocator(st->cfg.user, st->lanes, 0);
        st->lanes=NULL; st->lane_cap=0; st->lane_free=RANT__NIL;
    }
    {   uint32_t p;                              /* the per peer maps */
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


/* The peer's maps grown to cover need entries, the new tail unmapped with no verdicts.
 * NULL on OOM, the caller then counts the entries as unmappable. */
static uint16_t *i_rant_peer_index_ensure(RantTransportState *st, int peer_slot, uint32_t need){
    uint32_t have = st->peer_index_len[peer_slot];
    uint16_t *nm; uint8_t *ns, *ng, *na;
    if (need <= have) return st->peer_index[peer_slot];
    nm = (uint16_t*)st->cfg.allocator(st->cfg.user, st->peer_index[peer_slot], (size_t)need*sizeof(uint16_t));
    if (!nm) return NULL;
    st->peer_index[peer_slot] = nm;   /* len not raised yet: retryable on OOM below */
    ns = (uint8_t*)st->cfg.allocator(st->cfg.user, st->peer_astate[peer_slot], (size_t)need);
    if (!ns) return NULL;
    st->peer_astate[peer_slot] = ns;
    ng = (uint8_t*)st->cfg.allocator(st->cfg.user, st->peer_agen[peer_slot], (size_t)need);
    if (!ng) return NULL;
    st->peer_agen[peer_slot] = ng;
    na = (uint8_t*)st->cfg.allocator(st->cfg.user, st->peer_attrs[peer_slot], (size_t)need);
    if (!na) return NULL;
    st->peer_attrs[peer_slot] = na;
    memset(nm + have, 0xFF, (size_t)(need-have)*sizeof(uint16_t));   /* the grown tail: unmapped */
    memset(ns + have, 0, (size_t)(need-have));
    memset(ng + have, 0, (size_t)(need-have));
    memset(na + have, 0, (size_t)(need-have));
    st->peer_index_len[peer_slot] = need;
    return nm;
}

/* Local topics whose identity's low 32 bits match, preferring a non INACTIVE one. */
static int i_rant_hash32_candidates(RantTransportState *st, uint32_t h, int *idx_out){
    uint16_t i; int first=-1, live=-1, n=0;
    for (i=0;i<st->cfg.n_topics;i++){
        if (!i_rant_topic_announced(&st->topics[i]) || (uint32_t)st->topics[i].identity != h) continue;
        n++;
        if (first<0) first=(int)i;
        if (live<0 && st->topics[i].role!=RANT_INACTIVE) live=(int)i;
    }
    if (idx_out) *idx_out = live>=0 ? live : first;
    return n;
}


/* Worst case: 5 bytes per slot plus the rate and generation sections at every topic. */
size_t rant_interest_max(uint16_t n_topics){
    return 2u + 5u * (size_t)n_topics + 2u + 4u * (size_t)n_topics
         + 2u + 3u * (size_t)n_topics;
}

/* the rate section's membership test, shared by the count and write walks */
static int i_rant_topic_rate_sub(const i_RantTopic *t){
    return i_rant_topic_announced(t) && t->qos.max_rate_hz && rant_role_subs(t->role);
}

/* the generation section's membership test, shared by the count and write walks */
static int i_rant_topic_rebound(const i_RantTopic *t){
    return i_rant_topic_announced(t) && t->gen != 0;
}


/* Serializes our interest, or measures it when out is NULL, in one walk so the two agree
 * byte for byte. The layout is in spec/interest.md. */
static size_t i_rant_interest_emit(RantTransportState *st, uint8_t *out, size_t cap){
    uint8_t *e, *rp;
    uint16_t c, n=0, n_rates=0, n_gens=0;
    uint32_t cells=0; int in_hole=0; size_t len;
    for (c=0;c<st->cfg.n_topics;c++) if (i_rant_topic_announced(&st->topics[c])) n=(uint16_t)(c+1u);
    for (c=0;c<n;c++){   /* one pass: the exact cell count and the section counts */
        const i_RantTopic *topic = &st->topics[c];
        if (i_rant_topic_announced(topic)){ cells++; in_hole=0; }
        else { if (!in_hole) cells++; in_hole=1; }
        if (i_rant_topic_rate_sub(topic)) n_rates++;
        if (i_rant_topic_rebound(topic))    n_gens++;
    }
    len = 2u + 5u*(size_t)cells + 2u + 4u*(size_t)n_rates + 2u + 3u*(size_t)n_gens;
    if (!out) return len;
    if (cap < len) return 0;
    i_rant_le_w16(out, n);
    e = out + 2u;
    for (c=0;c<n;c++){
        const i_RantTopic *topic = &st->topics[c];
        if (!i_rant_topic_announced(topic)){
            uint32_t run = 1;
            while ((uint16_t)(c+run) < n && !i_rant_topic_announced(&st->topics[c+run])) run++;
            i_rant_le_w32(e, run);
            e[4] = (uint8_t)(RANT_INACTIVE | RANT__INT_HOLE_RUN);
            e += 5u; c = (uint16_t)(c + run - 1u);
            continue;
        }
        i_rant_le_w32(e, (uint32_t)topic->identity);
        e[4] = (uint8_t)((topic->role & RANT__INT_ROLE_MASK)
             | (topic->qos.reliability==RANT_RELIABLE ? RANT__INT_RELIABLE : 0u)
             | ((topic->kind << RANT__INT_KIND_SHIFT) & RANT__INT_KIND_MASK));
        e += 5u;
    }
    rp = e;
    i_rant_le_w16(rp, n_rates); rp += 2u;
    for (c=0;c<n;c++){
        const i_RantTopic *topic = &st->topics[c];
        if (i_rant_topic_rate_sub(topic)){
            i_rant_le_w16(rp, c); i_rant_le_w16(rp+2, topic->qos.max_rate_hz); rp += 4u;
        }
    }
    i_rant_le_w16(rp, n_gens); rp += 2u;
    for (c=0;c<n;c++){
        const i_RantTopic *topic = &st->topics[c];
        if (i_rant_topic_rebound(topic)){
            i_rant_le_w16(rp, c); rp[2] = topic->gen; rp += 3u;
        }
    }
    return (size_t)(rp - out);
}

size_t rant_transport_build_interest(RantTransportState *st, void *out, size_t cap){
    return i_rant_interest_emit(st, (uint8_t*)out, cap);
}

/* The entry stream's byte length when it accounts exactly n slots inside len, else 0.
 * Every parser walks this first, so a bad blob is rejected whole. */
static uint32_t i_rant_interest_walk_len(const uint8_t *d, size_t len, uint16_t n){
    const uint8_t *e = d + 2u, *end = d + len;
    uint32_t a = 0;
    while (a < n){
        uint32_t step = 1;
        if (e + 5 > end) return 0;
        if (e[4] & RANT__INT_HOLE_RUN){
            step = i_rant_le_r32(e);
            if (step == 0 || step > (uint32_t)n - a) return 0;
        }
        e += 5u; a += step;
    }
    return (uint32_t)(e - d);
}

/* The two sparse sections after the entries, located in one pass. A section that does
 * not fit is absent along with everything after it. */
typedef struct { RantBytes rate, gen; } i_RantInterestSections;

static i_RantInterestSections i_rant_interest_sections(const uint8_t *d, size_t len,
                                                       uint32_t entries_len){
    static const uint8_t width[2] = { 4u, 3u };  /* (index, rate_hz) and (index, gen) */
    i_RantInterestSections s; RantBytes *view[2];
    size_t off = entries_len; int k;
    s.rate = s.gen = rant_bytes(NULL, 0);
    view[0] = &s.rate; view[1] = &s.gen;
    for (k=0;k<2;k++){
        size_t bytes;
        if (len < off + 2u) break;                   /* no header: this one and the rest absent */
        bytes = (size_t)i_rant_le_r16(d + off) * width[k];
        off += 2u;
        if (len < off + bytes) break;                /* entries truncated: the same */
        *view[k] = rant_bytes(d + off, bytes);
        off += bytes;
    }
    return s;
}


/* Re derives the peer's bits from cached verdicts plus the entry's current flags, then
 * rematches every topic. Idempotent. The gates are in spec/interest.md. */
void rant_transport_apply_peer_interest(RantTransportState *st, uint32_t peer_id, RantBytes blob){
    const uint8_t *d=blob.data, *e, *gp, *g_end;
    uint16_t n, c; uint32_t a, entries_len; int peer_slot=i_rant_peer_slot(st,peer_id);
    uint8_t *peer_pub_bitmap, *peer_sub_bitmap, *peer_sub_reliable;
    uint16_t *amap; uint8_t *astate;
    i_RantInterestSections sec;
    uint32_t unmappable = 0;
    if (peer_slot<0 || !d || blob.len<2) return;
    n = i_rant_le_r16(d);
    entries_len = i_rant_interest_walk_len(d, blob.len, n);
    if (!entries_len) return;                          /* truncated or malformed: reject whole */
    peer_pub_bitmap  =&st->peer_pub_bitmap[(size_t)peer_slot*st->bitmap_len];
    peer_sub_bitmap  =&st->peer_sub_bitmap[(size_t)peer_slot*st->bitmap_len];
    peer_sub_reliable=&st->peer_sub_reliable[(size_t)peer_slot*st->bitmap_len];
    memset(peer_pub_bitmap,0,st->bitmap_len); memset(peer_sub_bitmap,0,st->bitmap_len);
    memset(peer_sub_reliable,0,st->bitmap_len);
    amap   = n ? i_rant_peer_index_ensure(st, peer_slot, n) : NULL;
    astate = amap ? st->peer_astate[peer_slot] : NULL;
    /* the generation data is consumed by the entry loop with a merge cursor, the rate
       values are applied after the rematch once lanes exist */
    sec = i_rant_interest_sections(d, blob.len, entries_len);
    gp = g_end = NULL;
    if (sec.gen.data){ gp = sec.gen.data; g_end = gp + sec.gen.len; }
    e = d + 2u;
    for (a=0;a<n;a++,e+=5u){
        uint8_t flags = e[4], role = (uint8_t)(flags & RANT__INT_ROLE_MASK);
        int their_pub, their_sub, rel;
        uint16_t cidx; i_RantTopic *topic;
        if (flags & RANT__INT_HOLE_RUN){ a += i_rant_le_r32(e) - 1u; continue; }
        if (astate && a < st->peer_index_len[peer_slot] && st->peer_agen[peer_slot]){
            /* the generation gate: the peer rebound this position since our verdict, so it
               goes back to pending with the demux severed. Runs for INACTIVE entries too. */
            uint8_t g = 0;
            while (gp && gp + 3 <= g_end && i_rant_le_r16(gp) < a) gp += 3;
            if (gp && gp + 3 <= g_end && i_rant_le_r16(gp) == a) g = gp[2];
            if (st->peer_agen[peer_slot][a] != g){
                if (astate[a]){
                    astate[a] = 0; amap[a] = 0xFFFFu;
                    if (st->peer_attrs[peer_slot]) st->peer_attrs[peer_slot][a] = 0;
                }
                st->peer_agen[peer_slot][a] = g;
            }
        }
        if (role == RANT_INACTIVE) continue;
        if (!astate || a >= st->peer_index_len[peer_slot]){
            /* no verdict storage: this entry can never verify, count it if it was a candidate */
            if (i_rant_hash32_candidates(st, i_rant_le_r32(e), NULL)) unmappable++;
            continue;
        }
        if (!(astate[a] & RANT__AST_DETAILED) || !(astate[a] & RANT__AST_NAME_OK))
            continue;                     /* pending, or a verified non match */
        cidx = amap[a];
        if (cidx >= st->cfg.n_topics) continue;      /* defensive: a stale map */
        topic = &st->topics[cidx];
        if ((uint32_t)topic->identity != i_rant_le_r32(e)){
            /* the hash no longer names the bound identity: the position was rebound */
            astate[a] = 0; amap[a] = 0xFFFFu;
            if (st->peer_attrs[peer_slot]) st->peer_attrs[peer_slot][a] = 0;
            continue;
        }
        if (topic->role == RANT_INACTIVE || topic->retired){
            /* the verdict bound the then live twin, so re verify against the live one */
            int resolved_index = (int)cidx;
            i_rant_topic_by_identity(st, topic->identity, &resolved_index);
            if ((uint16_t)resolved_index != cidx){
                astate[a] = 0; amap[a] = 0xFFFFu;      /* pending again */
                if (st->peer_attrs[peer_slot]) st->peer_attrs[peer_slot][a] = 0;
                continue;
            }
            if (topic->retired) continue;   /* no live successor yet */
        }
        {   /* the kind gate: the same name under another kind is refused, never cross wired */
            uint8_t their_kind = (uint8_t)((flags & RANT__INT_KIND_MASK) >> RANT__INT_KIND_SHIFT);
            if (their_kind != topic->kind){
                i_rant_transport_fire_event(st, RANT_TRANSPORT_KIND_MISMATCH, cidx, peer_id, 0, 0);
                continue;
            }
        }
        their_pub = rant_role_pubs(role);
        their_sub = rant_role_subs(role);
        rel       = (flags & RANT__INT_RELIABLE) != 0;
        if (their_pub){   /* their offered qos against our subscription */
            int ours_sub = rant_role_subs(topic->role);
            if (ours_sub && topic->qos.reliability==RANT_RELIABLE && !rel){
                i_rant_transport_fire_event(st, RANT_TRANSPORT_QOS_INCOMPATIBLE, cidx, peer_id, 0, 0);
            } else if (!(astate[a] & RANT__AST_READ_OK)){
                i_rant_transport_fire_event(st, RANT_TRANSPORT_SCHEMA_MISMATCH, cidx, peer_id, 1, 0);
            } else {
                i_rant_bit_set(peer_pub_bitmap,(uint32_t)cidx);
            }
        }
        if (their_sub){                                /* their requested qos, for our writer */
            if (!(astate[a] & RANT__AST_WRITE_OK)){
                i_rant_transport_fire_event(st, RANT_TRANSPORT_SCHEMA_MISMATCH, cidx, peer_id, 0, 0);
            } else {
                i_rant_bit_set(peer_sub_bitmap,(uint32_t)cidx);
                if (rel) i_rant_bit_set(peer_sub_reliable,(uint32_t)cidx);
            }
        }
    }
    if (unmappable)   /* never silent: those topics can never deliver here */
        i_rant_transport_fire_event(st, RANT_TRANSPORT_INTEREST_OVERFLOW, 0, peer_id, 0, unmappable);
    for (c=0;c<st->cfg.n_topics;c++) i_rant_topic_rematch(st,c,(uint16_t)peer_slot);

    /* per lane values, applied after the rematch so the lanes exist and re derived on
       every announce, since a fresh match memsets the proxy */
    if (amap && sec.rate.data){
        /* their best effort delivery cap for a topic they subscribe paces our lane */
        const uint8_t *p = sec.rate.data, *end = p + sec.rate.len;
        for (; p + 4u <= end; p += 4u){
            uint16_t their_idx = i_rant_le_r16(p), rate_hz = i_rant_le_r16(p+2), cidx;
            i_RantWriterProxy *w;
            if (their_idx >= st->peer_index_len[peer_slot]) continue;
            cidx = amap[their_idx];
            if (cidx >= st->cfg.n_topics) continue;             /* unverified */
            w = i_rant_writer_proxy_at(st, cidx, (uint32_t)peer_slot);
            if (w && w->used)
                w->rate_interval_us = rate_hz ? (1000000u / (uint32_t)rate_hz) : 0u;
        }
    }
    if (amap && st->peer_attrs[peer_slot]){
        /* NO_TIMESTAMP from the detail cache onto our reader lane, so delivery strips
           exactly what the writer prepended. Unset = stamped */
        const uint8_t *attrs = st->peer_attrs[peer_slot];
        for (a=0;a<st->peer_index_len[peer_slot];a++){
            uint16_t cidx; i_RantReaderProxy *r;
            if (!(attrs[a] & RANT_ATTR_NO_TIMESTAMP)) continue;
            cidx = amap[a];
            if (cidx >= st->cfg.n_topics) continue;             /* unverified */
            r = i_rant_reader_proxy_at(st, cidx, (uint32_t)peer_slot);
            if (r && r->used) r->no_timestamp = 1;
        }
    }
}


int rant_transport_peer_timestamped(RantTransportState *st, uint16_t topic_index, uint32_t peer_id){
    int peer_slot;
    i_RantReaderProxy *r;
    if (!st || topic_index >= st->cfg.n_topics) return 1;
    peer_slot = i_rant_peer_slot(st, peer_id);
    if (peer_slot < 0) return 1;                       /* unknown peer: the default framing */
    r = i_rant_reader_proxy_at(st, topic_index, (uint32_t)peer_slot);
    return (r && r->no_timestamp) ? 0 : 1;
}


uint8_t rant_transport_peer_attrs(RantTransportState *st, uint32_t peer_id, uint16_t their_index){
    int peer_slot;
    if (!st) return 0;
    peer_slot = i_rant_peer_slot(st, peer_id);
    if (peer_slot < 0 || !st->peer_attrs[peer_slot]
        || (uint32_t)their_index >= st->peer_index_len[peer_slot]) return 0;
    return st->peer_attrs[peer_slot][their_index];
}


void rant_transport_peer_match_counts(RantTransportState *st, uint32_t peer_id,
                            uint16_t *publish_to, uint16_t *receive_from){
    int s; uint16_t c, w=0, r=0;
    if (publish_to)   *publish_to   = 0;
    if (receive_from) *receive_from = 0;
    if (!st) return;
    s = i_rant_peer_slot(st, peer_id);
    if (s < 0) return;
    for (c=0;c<st->cfg.n_topics;c++){
        i_RantLane *l = i_rant_lane_at(st,c,(uint32_t)s);
        if (!l) continue;
        if (l->w.used) w++;
        if (l->r.used) r++;
    }
    if (publish_to)   *publish_to   = w;
    if (receive_from) *receive_from = r;
}


/* The announce overlay codec. The version byte tags the one current format, and a foreign
   blob is rejected. The layout is in spec/interest.md. */
#define RANT__META_BASE_NOSHM 6u      /* 'D','N',ver, frag_lo, frag_hi, iflags */
#define RANT__META_BASE_SHM     23u   /* 'D','N',ver, frag_lo, frag_hi, shm, host[16], iflags */
#define RANT__META_IFLAG_EXTERNAL 0x01u     /* iflags bit 0: the interest is served by paging */
#ifdef RANT_SHM
#define RANT__META_VER    21u                 /* odd versions carry the SHM base */
#define RANT__META_BASE RANT__META_BASE_SHM
#else
#define RANT__META_VER    20u
#define RANT__META_BASE RANT__META_BASE_NOSHM
#endif

static int i_rant_meta_ok(RantBytes meta){
    return meta.data && meta.len >= RANT__META_BASE_NOSHM
        && meta.data[0]=='D' && meta.data[1]=='N'
        && meta.data[2]>=20 && meta.data[2]<=21;
}
/* the base prefix through the iflags byte. The odd version carries shm and host */
static uint16_t i_rant_meta_base(const uint8_t *meta){
    return (meta[2] & 1u) ? RANT__META_BASE_SHM : RANT__META_BASE_NOSHM;
}
uint16_t rant_meta_cap(uint16_t n_topics){
    size_t cap = (size_t)RANT__META_BASE + rant_interest_max(n_topics);
    if (cap > 65000u) cap = 65000u;
    return (uint16_t)cap;
}

uint32_t rant_transport_interest_size(RantTransportState *st){
    return (uint32_t)i_rant_interest_emit(st, NULL, 0);
}

uint16_t rant_transport_meta_size(RantTransportState *st){
    size_t len = (size_t)RANT__META_BASE + rant_transport_interest_size(st);
    if (len > 65000u) len = 65000u;              /* the rant_meta_cap ceiling */
    return (uint16_t)len;
}

uint16_t rant_transport_meta_bootstrap_size(void){ return RANT__META_BASE; }

uint16_t rant_transport_meta_build(RantTransportState *st, uint8_t *out, uint16_t cap,
                         uint16_t frag_size, int shm_capable, const uint8_t host[16],
                         int interest_external){
    size_t interest_len, len; uint16_t off = RANT__META_BASE;
    out[0]='D'; out[1]='N'; out[2]=RANT__META_VER;
    out[3]=(uint8_t)(frag_size & 0xFF); out[4]=(uint8_t)(frag_size >> 8);
#ifdef RANT_SHM
    out[5]=(uint8_t)(shm_capable?1:0);
    if (host) memcpy(out+6, host, 16); else memset(out+6, 0, 16);
#else
    (void)shm_capable; (void)host;
#endif
    out[off-1] = interest_external ? RANT__META_IFLAG_EXTERNAL : 0u;
    if (interest_external) return off;     /* the bootstrap: locator sized, always one datagram */
    interest_len = rant_transport_build_interest(st, out + off, cap - off);
    len = (size_t)off + interest_len;
    if (interest_len == 0)    /* did not fit: never silent */
        i_rant_transport_fire_event(st, RANT_TRANSPORT_META_TRUNCATED_INTEREST, 0, 0, 0, 0);
    return (uint16_t)len;
}

uint16_t rant_meta_frag(RantBytes meta){
    if (!i_rant_meta_ok(meta)) return 0;
    return (uint16_t)(meta.data[3] | ((uint16_t)meta.data[4] << 8));
}

int rant_meta_interest_external(RantBytes meta){
    uint16_t base;
    if (!i_rant_meta_ok(meta)) return 0;
    base = i_rant_meta_base(meta.data);
    if (meta.len < base) return 0;
    return (meta.data[base-1] & RANT__META_IFLAG_EXTERNAL) ? 1 : 0;
}

RantBytes rant_meta_interest(RantBytes meta){
    uint16_t off;
    if (!i_rant_meta_ok(meta)) return rant_bytes(NULL, 0);
    off = i_rant_meta_base(meta.data);       /* the interest follows the base */
    if (meta.len < off) return rant_bytes(NULL, 0);
    if (meta.data[off-1] & RANT__META_IFLAG_EXTERNAL)
        return rant_bytes(NULL, 0);          /* a bootstrap never reads as an empty interest list */
    return rant_bytes(meta.data + off, meta.len - off);
}

int rant_interest_next(RantBytes interest, RantInterestIter *it, RantTopicEntry *out){
    if (!it || !out) return 0;
    if (!it->started){                    /* first call: the [u16 n] header */
        it->started = 1; it->left = 0; it->index = 0; it->phase = 0; it->off = 0;
        if (!interest.data || interest.len < 2) return 0;   /* no interest list: nothing to yield */
        it->left = (uint16_t)(interest.data[0] | ((uint16_t)interest.data[1] << 8));
        it->off  = 2u;                    /* the first entry, past n */
    }
    while (it->left){
        uint32_t off = it->off;
        uint8_t flags, role;
        if ((size_t)off + 5u > interest.len){ it->left = 0; return 0; }   /* truncated: stop */
        flags = interest.data[off + 4]; role = (uint8_t)(flags & RANT__INT_ROLE_MASK);
        if (flags & RANT__INT_HOLE_RUN){    /* a run of undefined slots: skip them all */
            uint32_t run = i_rant_le_r32(interest.data + off);
            if (run == 0 || run > it->left){ it->left = 0; return 0; }    /* malformed: stop */
            it->left = (uint16_t)(it->left - run); it->index = (uint16_t)(it->index + run);
            it->off = off + 5u; it->phase = 0;
            continue;
        }
        if (role == RANT_INACTIVE){         /* declared but off: not advertised */
            it->left--; it->index++; it->off = off + 5u; it->phase = 0;
            continue;
        }
        out->index    = it->index;
        out->role     = role;
        out->reliable = (uint8_t)((flags & RANT__INT_RELIABLE) ? 1 : 0);
        out->kind     = (uint8_t)((flags & RANT__INT_KIND_MASK) >> RANT__INT_KIND_SHIFT);
        out->hash     = i_rant_le_r32(interest.data + off);
        if (it->phase == 0 && rant_role_pubs(role)){
            out->is_pub = 1;
            if (role==RANT_PUBSUB){ it->phase = 1; return 1; }     /* the sub direction next call */
            it->left--; it->index++; it->off = off + 5u;
            return 1;
        }
        out->is_pub = 0;                  /* SUB_ONLY, or the second yield of a PUBSUB entry */
        it->phase = 0; it->left--; it->index++; it->off = off + 5u;
        return 1;
    }
    return 0;
}

int rant_meta_interest_next(RantBytes meta, RantInterestIter *it, RantTopicEntry *out){
    return rant_interest_next(rant_meta_interest(meta), it, out);
}

#ifdef RANT_SHM
int rant_meta_shm(RantBytes meta, uint8_t host[16]){
    if (!i_rant_meta_ok(meta) || !(meta.data[2] & 1u)     /* the odd version carries the SHM base */
        || meta.len < RANT__META_BASE_SHM || !meta.data[5]) return 0;
    memcpy(host, meta.data+6, 16);
    return 1;
}
#endif


/* The uDTL codec. A malformed request or response is dropped whole. The responder is a
   pure read of topic and schema state. The layout is in spec/interest.md. */
#define RANT__DETAIL_HDR 14u     /* magic(4) kind(1) ver(1) domain(2) meta_version(4) n(2) */
#define RANT__DETAIL_VER 2u

static int i_rant_detail_hdr_ok(RantBytes d){
    return d.data && d.len >= RANT__DETAIL_HDR
        && d.data[0]=='u' && d.data[1]=='D' && d.data[2]=='T' && d.data[3]=='L'
        && d.data[5]==RANT__DETAIL_VER;
}

int rant_detail_kind(RantBytes dgram){
    if (!i_rant_detail_hdr_ok(dgram)) return 0;
    return (dgram.data[4]>=RANT_DETAIL_REQ && dgram.data[4]<=RANT_INTEREST_RESP)
         ? dgram.data[4] : 0;
}
uint16_t rant_detail_domain(RantBytes dgram){
    return i_rant_detail_hdr_ok(dgram) ? i_rant_le_r16(dgram.data+6) : 0;
}
uint32_t rant_detail_meta_version(RantBytes dgram){
    return i_rant_detail_hdr_ok(dgram) ? i_rant_le_r32(dgram.data+8) : 0;
}

static void i_rant_detail_hdr_write(uint8_t *o, uint8_t kind, uint16_t domain,
                                    uint32_t meta_version, uint16_t n){
    o[0]='u'; o[1]='D'; o[2]='T'; o[3]='L';
    o[4]=kind; o[5]=(uint8_t)RANT__DETAIL_VER;
    i_rant_le_w16(o+6, domain);
    i_rant_le_w32(o+8, meta_version);
    i_rant_le_w16(o+12, n);
}

size_t rant_detail_req_build(uint16_t domain, uint32_t peer_meta_version,
                             const RantDetailWant *wants, uint16_t n_wants,
                             void *out, size_t cap){
    uint8_t *o=(uint8_t*)out; uint16_t k;
    size_t need = (size_t)RANT__DETAIL_HDR + (size_t)n_wants*10u;
    if (!o || (n_wants && !wants) || cap < need) return 0;
    i_rant_detail_hdr_write(o, RANT_DETAIL_REQ, domain, peer_meta_version, n_wants);
    for (k=0;k<n_wants;k++){
        uint8_t *e = o + RANT__DETAIL_HDR + (size_t)k*10u;
        i_rant_le_w16(e, wants[k].index);
        i_rant_le_w64(e+2, wants[k].schema_hash);
    }
    return need;
}


/* The interest paging codec: pure header build and parse. The responder's slicing and
   the requester's cursor live in the node core. */
size_t rant_interest_req_build(uint16_t domain, uint32_t peer_meta_version,
                               uint32_t offset, void *out, size_t cap){
    uint8_t *o=(uint8_t*)out;
    if (!o || cap < (size_t)RANT__DETAIL_HDR + 4u) return 0;
    i_rant_detail_hdr_write(o, RANT_INTEREST_REQ, domain, peer_meta_version, 0);
    i_rant_le_w32(o + RANT__DETAIL_HDR, offset);
    return (size_t)RANT__DETAIL_HDR + 4u;
}

int rant_interest_req_offset(RantBytes dgram, uint32_t *offset){
    if (rant_detail_kind(dgram) != RANT_INTEREST_REQ) return 0;
    if (dgram.len < (size_t)RANT__DETAIL_HDR + 4u) return 0;
    if (offset) *offset = i_rant_le_r32(dgram.data + RANT__DETAIL_HDR);
    return 1;
}

size_t rant_interest_resp_head(uint16_t domain, uint32_t meta_version, uint32_t total_len,
                               uint32_t offset, uint16_t chunk_len, void *out, size_t cap){
    uint8_t *o=(uint8_t*)out;
    if (!o || cap < (size_t)RANT_INTEREST_RESP_HEAD) return 0;
    i_rant_detail_hdr_write(o, RANT_INTEREST_RESP, domain, meta_version, 0);
    i_rant_le_w32(o + RANT__DETAIL_HDR,          total_len);
    i_rant_le_w32(o + RANT__DETAIL_HDR + 4u, offset);
    i_rant_le_w16(o + RANT__DETAIL_HDR + 8u, chunk_len);
    return (size_t)RANT_INTEREST_RESP_HEAD;
}

int rant_interest_resp_parse(RantBytes dgram, uint32_t *total_len, uint32_t *offset,
                             RantBytes *chunk){
    uint32_t total, off; uint16_t clen;
    if (rant_detail_kind(dgram) != RANT_INTEREST_RESP) return 0;
    if (dgram.len < (size_t)RANT_INTEREST_RESP_HEAD) return 0;
    total = i_rant_le_r32(dgram.data + RANT__DETAIL_HDR);
    off   = i_rant_le_r32(dgram.data + RANT__DETAIL_HDR + 4u);
    clen  = i_rant_le_r16(dgram.data + RANT__DETAIL_HDR + 8u);
    if ((size_t)RANT_INTEREST_RESP_HEAD + clen > dgram.len) return 0;     /* truncated: reject */
    if (off > total || (uint32_t)clen > total - off) return 0;   /* a range outside the blob */
    if (total_len) *total_len = total;
    if (offset)    *offset    = off;
    if (chunk)     *chunk     = rant_bytes(dgram.data + RANT_INTEREST_RESP_HEAD, clen);
    return 1;
}

/* One walk serves size and build (out NULL measures), so the two agree byte for byte. A
   truncated build stops at an entry boundary and the requester re asks for the rest. */
static size_t i_rant_detail_answer(RantTransportState *st, const RantMetaSchema *schemas,
                                   uint32_t meta_version, RantBytes req,
                                   uint8_t *out, size_t cap){
    const uint8_t *r; uint16_t n_req, k, n_out=0;
    size_t len = RANT__DETAIL_HDR;
    if (!st || rant_detail_kind(req) != RANT_DETAIL_REQ) return 0;
    n_req = i_rant_le_r16(req.data+12);
    if (req.len < (size_t)RANT__DETAIL_HDR + (size_t)n_req*10u) return 0;     /* truncated: reject */
    if (out){
        if (cap < RANT__DETAIL_HDR) return 0;
        i_rant_detail_hdr_write(out, RANT_DETAIL_RESP, rant_detail_domain(req), meta_version, 0);
    }
    r = req.data + RANT__DETAIL_HDR;
    for (k=0;k<n_req;k++,r+=10){
        uint16_t index    = i_rant_le_r16(r);
        uint64_t req_hash = i_rant_le_r64(r+2);
        const i_RantTopic *topic;
        uint64_t hash; RantBytes wire; size_t need;
        if (index >= st->cfg.n_topics) continue;             /* unknown: not advertised */
        topic = &st->topics[index];
        if (topic->role == RANT_INACTIVE || topic->name_len == 0) continue;
        hash = schemas ? schemas[index].hash : 0;
        wire = rant_bytes(NULL, 0);
        if (hash && hash != req_hash && schemas[index].wire.len <= 0xFFFFu)
            wire = schemas[index].wire;    /* differs: inline for the subset check */
        need = 2u + 1u + 1u + topic->name_len + 8u + 2u + wire.len;
        /* one datagram per response, but always at least one entry, so a lone oversized
           entry rides its own page instead of wedging paging with an empty reply */
        if (n_out > 0 && len + need > cap) break;
        if (out){
            uint8_t *e = out + len;
            i_rant_le_w16(e, index);
            e[2] = (uint8_t)(topic->attrs
                 | (topic->qos.no_timestamp ? RANT_ATTR_NO_TIMESTAMP : 0u));
            e[3] = topic->name_len;
            memcpy(e+4, topic->name, topic->name_len);
            i_rant_le_w64(e+4+topic->name_len, hash);
            i_rant_le_w16(e+4+topic->name_len+8, (uint16_t)wire.len);
            if (wire.len) memcpy(e+4+topic->name_len+10, wire.data, wire.len);
        }
        len += need;
        n_out++;
    }
    if (out) i_rant_le_w16(out+12, n_out);
    return len;
}

size_t rant_transport_detail_resp_size(RantTransportState *st, const RantMetaSchema *schemas,
                                       RantBytes req){
    /* one page, so the response never IP fragments and the requester pages the rest */
    return i_rant_detail_answer(st, schemas, 0, req, NULL, RANT_DGRAM_MAX);
}

size_t rant_transport_detail_respond(RantTransportState *st, const RantMetaSchema *schemas,
                                     uint32_t meta_version, RantBytes req,
                                     void *out, size_t cap){
    return i_rant_detail_answer(st, schemas, meta_version, req, (uint8_t*)out, cap);
}

int rant_detail_next(RantBytes resp, RantDetailIter *it, RantDetail *out){
    uint32_t off; uint8_t nlen; uint16_t wlen;
    if (!it || !out) return 0;
    if (!it->started){
        it->started = 1; it->left = 0; it->off = RANT__DETAIL_HDR;
        if (rant_detail_kind(resp) != RANT_DETAIL_RESP) return 0;
        it->left = i_rant_le_r16(resp.data+12);
    }
    if (!it->left) return 0;
    off = it->off;
    if ((size_t)off + 4u > resp.len){ it->left = 0; return 0; }        /* truncated: stop */
    nlen = resp.data[off+3];
    if ((size_t)off + 4u + nlen + 10u > resp.len){ it->left = 0; return 0; }
    wlen = i_rant_le_r16(resp.data + off + 4u + nlen + 8u);
    if ((size_t)off + 4u + nlen + 10u + wlen > resp.len){ it->left = 0; return 0; }
    out->index       = i_rant_le_r16(resp.data + off);
    out->attrs       = resp.data[off+2];
    out->name        = rant_string((const char*)(resp.data + off + 4u), nlen);
    out->schema_hash = i_rant_le_r64(resp.data + off + 4u + nlen);
    out->schema_wire = wlen ? rant_bytes(resp.data + off + 4u + nlen + 10u, wlen)
                            : rant_bytes(NULL, 0);
    it->off = off + 4u + nlen + 10u + wlen;
    it->left--;
    return 1;
}

/* Iterator over the live entries of an interest stream, shared by the candidate scans so
 * they cannot drift. The stream must have passed i_rant_interest_walk_len first. */
typedef struct {
    const uint8_t *e;        /* next entry */
    uint32_t a;              /* its position */
    uint32_t n;              /* slots in the stream */
    uint32_t pos;            /* yielded: the entry's position */
    uint32_t hash;           /* yielded: its 32 bit nomination */
    uint8_t  flags;          /* yielded: the raw flags */
    uint8_t  their_pub, their_sub;   /* yielded: the advertised role by direction */
} i_RantInterestScan;

static void i_rant_interest_scan_init(i_RantInterestScan *s, const uint8_t *d, uint16_t n){
    s->e = d + 2u; s->a = 0; s->n = n;
    s->pos = 0; s->hash = 0; s->flags = 0; s->their_pub = s->their_sub = 0;
}

static int i_rant_interest_scan_next(i_RantInterestScan *s){
    while (s->a < s->n){
        const uint8_t *e = s->e;
        uint32_t a = s->a, step = 1;
        uint8_t flags = e[4], role = (uint8_t)(flags & RANT__INT_ROLE_MASK);
        if (flags & RANT__INT_HOLE_RUN) step = i_rant_le_r32(e);
        s->e = e + 5u; s->a = a + step;
        if (flags & RANT__INT_HOLE_RUN) continue;     /* a run of undefined slots */
        if (role == RANT_INACTIVE) continue;          /* declared but off */
        s->pos = a; s->hash = i_rant_le_r32(e); s->flags = flags;
        s->their_pub = (uint8_t)rant_role_pubs(role);
        s->their_sub = (uint8_t)rant_role_subs(role);
        return 1;
    }
    return 0;
}

/* The pending candidates of a peer: a hash and role overlap with no cached verdict. */
uint16_t rant_transport_detail_wants(RantTransportState *st, const RantMetaSchema *schemas,
                                     uint32_t peer_id, RantBytes interest,
                                     RantDetailWant *out, uint16_t max_wants){
    const uint8_t *d=interest.data;
    i_RantInterestScan scan;
    uint16_t n, cnt=0; int slot;
    uint8_t *astate; uint32_t alen;
    if (!st || !d || interest.len < 2) return 0;
    slot = i_rant_peer_slot(st, peer_id);
    if (slot < 0) return 0;
    n = i_rant_le_r16(d);
    if (!i_rant_interest_walk_len(d, interest.len, n)) return 0;
    astate = st->peer_astate[slot]; alen = st->peer_index_len[slot];
    i_rant_interest_scan_init(&scan, d, n);
    while (i_rant_interest_scan_next(&scan)){
        int cidx = -1, nc;
        i_RantTopic *topic;
        int ours_pub, ours_sub;
        if (astate && scan.pos < alen && (astate[scan.pos] & RANT__AST_DETAILED)) continue;
        nc = i_rant_hash32_candidates(st, scan.hash, &cidx);
        if (!nc) continue;                                 /* no local topic */
        topic = &st->topics[cidx];
        ours_pub = rant_role_pubs(topic->role);
        ours_sub = rant_role_subs(topic->role);
        if (!((scan.their_pub && ours_sub) || (scan.their_sub && ours_pub))) continue;
        if (out){
            if (cnt >= max_wants) break;
            out[cnt].index = (uint16_t)scan.pos;
            /* several local topics behind one hash: hash 0 forces the wire inline, so
               whichever topic the name binds to can still verify */
            out[cnt].schema_hash = (nc == 1 && schemas) ? schemas[cidx].hash : 0;
        }
        cnt++;
    }
    return cnt;
}

/* Entries with no verdict storage are not counted. They can never resolve, so a wait on
 * them would only time out. */
uint16_t rant_transport_topic_unresolved(RantTransportState *st, uint16_t topic_index,
                                         uint32_t peer_id, RantBytes interest){
    const uint8_t *d=interest.data;
    i_RantTopic *topic;
    i_RantInterestScan scan;
    uint16_t n, cnt=0; int slot;
    uint8_t *astate; uint32_t alen;
    int ours_pub, ours_sub;
    topic = i_rant_topic_at(st, topic_index, NULL);
    if (!topic || !i_rant_topic_announced(topic) || !d || interest.len < 2) return 0;
    slot = i_rant_peer_slot(st, peer_id);
    if (slot < 0) return 0;
    n = i_rant_le_r16(d);
    if (!i_rant_interest_walk_len(d, interest.len, n)) return 0;
    ours_pub = rant_role_pubs(topic->role);
    ours_sub = rant_role_subs(topic->role);
    if (!ours_pub && !ours_sub) return 0;
    astate = st->peer_astate[slot]; alen = st->peer_index_len[slot];
    i_rant_interest_scan_init(&scan, d, n);
    while (i_rant_interest_scan_next(&scan)){
        if (scan.hash != (uint32_t)topic->identity) continue;   /* not this topic */
        if (!astate || scan.pos >= alen) continue;          /* unresolvable: never counted */
        if (astate[scan.pos] & RANT__AST_DETAILED){
            /* decided, but a verified subscriber whose writer lane is under the rebind hold
               is still resolving, so the match wait must cover it */
            if (scan.their_sub && ours_pub && (astate[scan.pos] & RANT__AST_NAME_OK)
                && topic->rebind_version
                && st->peer_seen_version[slot] < topic->rebind_version) cnt++;
            continue;
        }
        if ((scan.their_pub && ours_sub) || (scan.their_sub && ours_pub)) cnt++;
    }
    return cnt;
}

void rant_transport_peer_unresolved_fill(RantTransportState *st, uint32_t peer_id,
                                         RantBytes interest, uint16_t *counts, uint16_t n){
    const uint8_t *d=interest.data;
    i_RantInterestScan scan;
    uint16_t nent, c; int slot;
    uint8_t *astate; uint16_t *amap; uint32_t alen;
    if (!st || !counts || !n || !d || interest.len < 2) return;
    slot = i_rant_peer_slot(st, peer_id);
    if (slot < 0) return;
    nent = i_rant_le_r16(d);
    if (!i_rant_interest_walk_len(d, interest.len, nent)) return;
    astate = st->peer_astate[slot]; amap = st->peer_index[slot]; alen = st->peer_index_len[slot];
    if (!astate || !amap) return;                        /* unresolvable entries: never counted */
    if (n > st->cfg.n_topics) n = (uint16_t)st->cfg.n_topics;
    i_rant_interest_scan_init(&scan, d, nent);
    while (i_rant_interest_scan_next(&scan)){
        if (scan.pos >= alen) continue;
        if (astate[scan.pos] & RANT__AST_DETAILED){
            /* decided: the verdict names the topic, and only the rebind hold still resolves */
            uint16_t cidx = amap[scan.pos];
            const i_RantTopic *topic;
            if (!(astate[scan.pos] & RANT__AST_NAME_OK) || cidx >= n) continue;
            topic = &st->topics[cidx];
            if (scan.their_sub && rant_role_pubs(topic->role) && topic->rebind_version
                && st->peer_seen_version[slot] < topic->rebind_version && counts[cidx] != 0xFFFFu)
                counts[cidx]++;
            continue;
        }
        for (c=0;c<n;c++){   /* pending: every topic the hash nominates */
            const i_RantTopic *topic = &st->topics[c];
            int ours_pub, ours_sub;
            if (!i_rant_topic_announced(topic) || (uint32_t)topic->identity != scan.hash) continue;
            ours_pub = rant_role_pubs(topic->role); ours_sub = rant_role_subs(topic->role);
            if (((scan.their_pub && ours_sub) || (scan.their_sub && ours_pub)) && counts[c] != 0xFFFFu)
                counts[c]++;
        }
    }
}

/* Verifies each entry and caches the verdict. Idempotent, decided indices are skipped, so
 * duplicate and crossing responses are harmless. */
uint16_t rant_transport_apply_peer_details(RantTransportState *st, uint32_t peer_id, RantBytes resp){
    RantDetailIter it; RantDetail dd;
    int slot; uint16_t fresh = 0;
    if (!st) return 0;
    slot = i_rant_peer_slot(st, peer_id);
    if (slot < 0) return 0;
    memset(&it, 0, sizeof it);
    while (rant_detail_next(resp, &it, &dd)){
        uint16_t *amap = st->peer_index[slot]; uint8_t *astate = st->peer_astate[slot];
        uint64_t id64; i_RantTopic *topic; int cidx = -1;
        if (!amap || !astate || (uint32_t)dd.index >= st->peer_index_len[slot])
            continue;   /* unseen: ignored, a response racing the announce re resolves */
        if (astate[dd.index] & RANT__AST_DETAILED) continue;
        if (st->peer_attrs[slot]) st->peer_attrs[slot][dd.index] = dd.attrs;
        if (dd.name.len == 0){ astate[dd.index] = RANT__AST_DETAILED; fresh++; continue; }
        id64 = i_rant_identity_hash((const uint8_t*)dd.name.data, dd.name.len);
        topic = i_rant_topic_by_identity(st, id64, &cidx);
        if (!topic){                              /* the 32 bit nomination was a false positive */
            astate[dd.index] = RANT__AST_DETAILED;
            fresh++; continue;
        }
        if (topic->name_len != dd.name.len || memcmp(topic->name, dd.name.data, dd.name.len) != 0){
            astate[dd.index] = RANT__AST_DETAILED;     /* the same id, a different name: refused */
            i_rant_transport_fire_event(st, RANT_TRANSPORT_NAME_COLLISION, (uint16_t)cidx,
                        peer_id, id64, 0);
            fresh++; continue;
        }
        amap[dd.index] = (uint16_t)cidx;
        astate[dd.index] = (uint8_t)(RANT__AST_DETAILED | RANT__AST_NAME_OK
            | ((!st->cfg.schema_check || st->cfg.schema_check(st->cfg.user, peer_id, (uint16_t)cidx,
                    1, dd.schema_hash, dd.schema_wire)) ? RANT__AST_READ_OK : 0u)
            | ((!st->cfg.schema_check || st->cfg.schema_check(st->cfg.user, peer_id, (uint16_t)cidx,
                    0, dd.schema_hash, dd.schema_wire)) ? RANT__AST_WRITE_OK : 0u));
        fresh++;
    }
    return fresh;
}


int rant_transport_set_role(RantTransportState *st, uint16_t topic_index, uint8_t role){
    i_RantTopic *topic; uint16_t p;
    if (role > RANT_INACTIVE) return -1;
    topic = i_rant_topic_at(st, topic_index, NULL);
    if (!topic || topic->retired) return -1;   /* a retired slot only returns through reuse */
    if (topic->role == role) return 0;
    topic->role = role;
    for (p=0;p<st->cfg.max_peers;p++)
        if (st->peer_used[p]) i_rant_topic_rematch(st,(uint16_t)topic_index,p);
    /* the caller re advertises */
    return 0;
}


int rant_transport_topic_define(RantTransportState *st, uint16_t topic_index, const RantTopicDef *def){
    i_RantTopic *topic; RantQos q; uint16_t depth, p; size_t lane;
    if (!st || !def) return -1;
    if (topic_index >= st->cfg.n_topics) return -1;            /* out of the reserved range */
    if (!def->name || !def->name[0]) return -3;             /* the name is the identity */
    lane = i_rant_name_len(def->name);
    if (def->name[lane]) return -3;                          /* longer than RANT_TOPIC_NAME_MAX */
    if (def->directed && def->qos.catch_up) return -1;       /* directed history never replays */
    topic = &st->topics[topic_index];
    if (topic->identity != 0 || topic->history) return -1;        /* already defined */
    {   /* a same name slot on one node is refused under another kind, and under the same
           kind while both would be live, since the peer maps bind a name to one local topic
           and a live twin would only be shadowed. An INACTIVE twin is the QoS switch of
           spec/interest.md, and retired slots are exempt */
        uint64_t id = rant_topic_identity(def); uint16_t c;
        for (c=0;c<st->cfg.n_topics;c++){
            const i_RantTopic *t = &st->topics[c];
            if (!i_rant_topic_announced(t) || t->identity != id) continue;
            if (t->kind != def->kind) return -2;
            if (t->role != RANT_INACTIVE && def->role != RANT_INACTIVE) return -2;
        }
    }
    q = def->qos; i_rant_qos_defaults(&q);
    depth = q.keep_last;
    topic->history = (i_RantWriterSample*)st->cfg.allocator(st->cfg.user, NULL,
                                                         (size_t)depth*sizeof(i_RantWriterSample));
    if (!topic->history) return -4;                            /* OOM */
    memset(topic->history, 0, (size_t)depth*sizeof(i_RantWriterSample));
    topic->history_owned = 1;
    topic->qos = q;
    topic->role = def->role;
    topic->kind = def->kind; topic->prefix_bytes = def->prefix_bytes; topic->directed = def->directed;
    topic->attrs = def->attrs;
    topic->identity = rant_topic_identity(def);
    memcpy((char*)topic->name, def->name, lane); ((char*)topic->name)[lane] = '\0';
    topic->name_len = (uint8_t)lane;
    topic->history_head = 0; topic->next_seqno = 0; topic->have_first = 0;
    /* a dissolved verdict is only as durable as the topic set it was judged against, and
       that set just grew: send them back to pending. Attrs stay. See spec/interest.md */
    for (p=0;p<st->cfg.max_peers;p++){
        uint8_t *as = st->peer_astate[p]; uint32_t a, alen = st->peer_index_len[p];
        if (!st->peer_used[p] || !as) continue;
        for (a=0;a<alen;a++)
            if ((as[a] & RANT__AST_DETAILED) && !(as[a] & RANT__AST_NAME_OK))
                as[a] = 0;
    }
    for (p=0;p<st->cfg.max_peers;p++)        /* match the new topic to known peers */
        if (st->peer_used[p]) i_rant_topic_rematch(st, topic_index, p);
    return 0;
}


/* Parks a slot: every lane releases, the ring is freed and the slot leaves the announce.
 * Identity, name, kind, qos, gen and next_seqno stay for reuse to compare. */
int rant_transport_topic_retire(RantTransportState *st, uint16_t topic_index){
    i_RantTopic *topic; uint16_t p, d, depth;
    if (!st) return -1;
    topic = i_rant_topic_at(st, topic_index, NULL);
    if (!topic || topic->name_len == 0 || topic->retired) return -1;
    topic->role = RANT_INACTIVE; topic->retired = 1;
    for (p=0;p<st->cfg.max_peers;p++)          /* both sides unmatch, every lane releases */
        if (st->peer_used[p]) i_rant_topic_rematch(st, topic_index, p);
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
            memset(topic->history, 0, (size_t)depth*sizeof(i_RantWriterSample));
        }
    }
    topic->history_head = 0; topic->have_first = 0; topic->first_seqno = 0;
    memset(&topic->repair_stats, 0, sizeof topic->repair_stats);
    /* the caller re advertises: the slot now rides the announce as a hole */
    return 0;
}


int rant_transport_topic_reuse_find(RantTransportState *st, const char *name, uint8_t kind,
                                    uint16_t *index_out){
    uint64_t id; uint16_t c; int any = -1;
    if (!st || !name || !name[0]) return 0;
    id = rant_topic_id(name);
    for (c=0;c<st->cfg.n_topics;c++){
        const i_RantTopic *t = &st->topics[c];
        if (!t->retired) continue;
        if (t->identity == id && t->kind == kind){ if (index_out) *index_out = c; return 2; }
        if (any < 0) any = (int)c;
    }
    if (any >= 0){ if (index_out) *index_out = (uint16_t)any; return 1; }
    return 0;
}


/* Rebinds a retired slot. The seqno line always continues: an identical rebind keeps a
 * peer's reader position valid, and a changed one re forms every lane anyway. */
int rant_transport_topic_reuse(RantTransportState *st, uint16_t topic_index,
                               const RantTopicDef *def, int binding_changed,
                               uint32_t rebind_version){
    i_RantTopic *topic; RantQos q; uint16_t depth, p; size_t nlen; uint64_t id;
    if (!st || !def) return -1;
    topic = i_rant_topic_at(st, topic_index, NULL);
    if (!topic || !topic->retired) return -1;
    if (!def->name || !def->name[0]) return -3;
    nlen = i_rant_name_len(def->name);
    if (def->name[nlen]) return -3;                          /* longer than RANT_TOPIC_NAME_MAX */
    if (def->directed && def->qos.catch_up) return -1;       /* directed history never replays */
    id = rant_topic_identity(def);
    {   /* the same name rule as at define */
        uint16_t c;
        for (c=0;c<st->cfg.n_topics;c++){
            const i_RantTopic *t = &st->topics[c];
            if (c == topic_index || !i_rant_topic_announced(t) || t->identity != id) continue;
            if (t->kind != def->kind) return -2;
            if (t->role != RANT_INACTIVE && def->role != RANT_INACTIVE) return -2;
        }
    }
    if (!binding_changed && (topic->identity != id || topic->kind != def->kind))
        return -1;                       /* asserted identical, but the stored binding differs */
    q = def->qos; i_rant_qos_defaults(&q);
    depth = q.keep_last;
    if (topic->history && !topic->history_owned && depth <= topic->qos.keep_last){
        /* an arena ring that still fits is reused in place */
        memset(topic->history, 0, (size_t)depth*sizeof(i_RantWriterSample));
    } else {
        i_RantWriterSample *h = (i_RantWriterSample*)st->cfg.allocator(st->cfg.user, NULL,
                                                     (size_t)depth*sizeof(i_RantWriterSample));
        if (!h) return -4;                                   /* OOM: the slot stays retired */
        memset(h, 0, (size_t)depth*sizeof(i_RantWriterSample));
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
        topic->gen = (uint8_t)(topic->gen + 1u);   /* peers compare equality only */
        topic->rebind_version = rebind_version;
        for (p=0;p<st->cfg.max_peers;p++){
            uint32_t a;
            if (!st->peer_used[p]) continue;
            /* sever our bindings to the old occupant: those verdicts re pend and the
               interest bits derived from them go too */
            if (st->peer_index[p])
                for (a=0;a<st->peer_index_len[p];a++)
                    if (st->peer_index[p][a] == topic_index){
                        st->peer_index[p][a] = 0xFFFFu;
                        st->peer_astate[p][a] = 0;
                        if (st->peer_attrs[p]) st->peer_attrs[p][a] = 0;
                    }
            i_rant_bit_clr(&st->peer_pub_bitmap[(size_t)p*st->bitmap_len], topic_index);
            i_rant_bit_clr(&st->peer_sub_bitmap[(size_t)p*st->bitmap_len], topic_index);
            i_rant_bit_clr(&st->peer_sub_reliable[(size_t)p*st->bitmap_len], topic_index);
            /* dissolved verdicts re pend, as at define. Attrs stay. */
            if (st->peer_astate[p]){
                uint8_t *as = st->peer_astate[p];
                for (a=0;a<st->peer_index_len[p];a++)
                    if ((as[a] & RANT__AST_DETAILED) && !(as[a] & RANT__AST_NAME_OK))
                        as[a] = 0;
            }
        }
    }
    for (p=0;p<st->cfg.max_peers;p++)
        if (st->peer_used[p]) i_rant_topic_rematch(st, topic_index, p);
    return 0;
}


/* Releases the writer lanes the advance takes out of the rebind hold. 1 when a lane
 * actually formed, so the caller re fires its interest event. */
int rant_transport_peer_seen_version(RantTransportState *st, uint32_t peer_id, uint32_t version){
    int s; uint16_t c; uint32_t old; int changed = 0;
    if (!st) return 0;
    s = i_rant_peer_slot(st, peer_id);
    if (s < 0) return 0;
    old = st->peer_seen_version[s];
    if (version <= old) return 0;
    st->peer_seen_version[s] = version;
    for (c=0;c<st->cfg.n_topics;c++){
        i_RantTopic *t = &st->topics[c];
        i_RantLane *l; int was;
        if (!t->rebind_version || t->rebind_version <= old || t->rebind_version > version) continue;
        if (!i_rant_topic_announced(t)) continue;
        l = i_rant_lane_at(st, c, (uint32_t)s); was = l && l->w.used;
        i_rant_topic_rematch(st, c, (uint16_t)s);
        l = i_rant_lane_at(st, c, (uint32_t)s);
        if ((l && l->w.used) != was) changed = 1;
    }
    return changed;
}


uint64_t rant_transport_topic_seqno(RantTransportState *st, uint16_t topic_index){
    i_RantTopic *topic = st ? i_rant_topic_at(st, topic_index, NULL) : NULL;
    return topic ? topic->next_seqno : 0;
}


RantString rant_transport_topic_name(RantTransportState *st, uint16_t topic_index){
    i_RantTopic *topic = i_rant_topic_at(st, topic_index, NULL);
    if (!topic || topic->name_len == 0) return rant_string(NULL, 0);     /* undefined */
    return rant_string(topic->name, topic->name_len);
}


const RantQos *rant_transport_topic_qos(RantTransportState *st, uint16_t topic_index){
    i_RantTopic *topic = i_rant_topic_at(st, topic_index, NULL);
    return topic ? &topic->qos : NULL;
}

uint8_t rant_transport_topic_attrs(RantTransportState *st, uint16_t topic_index){
    i_RantTopic *topic = i_rant_topic_at(st, topic_index, NULL);
    return topic ? topic->attrs : 0;
}


void rant_transport_repair_stats(RantTransportState *st, uint16_t topic_index, RantRepairStats *out){
    i_RantTopic *topic = i_rant_topic_at(st, topic_index, NULL);
    if (!out) return;
    if (topic) *out = topic->repair_stats;
    else memset(out, 0, sizeof *out);
}


void rant_transport_on_datagram(RantTransportState *st, uint32_t from, RantBytes datagram, uint64_t now){
    const uint8_t *p=datagram.data; size_t rem=datagram.len;
    int peer_slot=i_rant_peer_slot(st,from);
    if (peer_slot<0) return;
    /* concatenated submessages, each length from its own header */
    while (rem>=3){
        uint8_t b0=p[0], type=(uint8_t)(b0 & RANT_MSG_MASK); uint16_t index; size_t sub; int topic_index;
        switch(type){
            case RANT_DATA:
#ifdef RANT_SHM
                            if (b0 & RANT_F_SHM){ if (rem<RANT_SHM_DATA_BYTES) return; sub=RANT_SHM_DATA_BYTES; }
                            else
#endif
                            if (b0 & RANT_F_SINGLE){ if (rem<RANT_HEADER_DATA_SINGLE) return; sub=RANT_HEADER_DATA_SINGLE+(size_t)i_rant_le_r16(p+RANT_OFFSET_PAYLOAD_LEN_SINGLE); }
                            else { if (rem<RANT_HEADER_DATA_MULTI) return; sub=RANT_HEADER_DATA_MULTI+(size_t)i_rant_le_r16(p+RANT_OFFSET_PAYLOAD_LEN); } break;
            case RANT_HB:     if (rem<RANT_HEADER_HB) return; sub=RANT_HEADER_HB; break;
            case RANT_NACK: if (rem<RANT_HEADER_NACK) return; sub=RANT_HEADER_NACK; break;
            default: return;             /* unknown type: cannot resync, drop the rest */
        }
        if (sub>rem) return;             /* truncated */
        index = i_rant_le_r16(p+RANT_OFFSET_INDEX);
        if ((uint32_t)index < st->peer_index_len[peer_slot]){
            uint16_t m=st->peer_index[peer_slot][index]; topic_index=(m==0xFFFFu)?-1:(int)m;
        } else topic_index=-1;
        if (topic_index>=0){
            switch(type){
                case RANT_DATA:
#ifdef RANT_SHM
                                if (p[0] & RANT_F_SHM){ i_rant_reader_shm(st,topic_index,peer_slot,p,now); break; }
#endif
                                i_rant_reader_data(st,topic_index,peer_slot,p,now); break;
                case RANT_HB:     i_rant_reader_hb    (st,topic_index,peer_slot,p,now); break;
                case RANT_NACK: i_rant_writer_nack(st,topic_index,peer_slot,p,now); break;
            }
        }
        p+=sub; rem-=sub;
    }
}
