/* The active lane scheduler and the outgoing poll. */
#include "internal.h"


/* work is queued as lane record indices, the destination is the record's peer slot */
static void i_rant_dest_push(RantTransportState *st, uint32_t d){
    uint32_t ndest = st->cfg.max_peers, pos;
    if (st->dest_queued[d]) return;
    st->dest_queued[d]=1;
    pos = st->dest_queue_head + st->dest_queue_count;
    if (pos >= ndest) pos -= ndest;
    st->dest_queue[pos]=d; st->dest_queue_count++;
}


/* idempotent while queued */
void i_rant_lane_enqueue(RantTransportState *st, uint32_t li){
    i_RantLane *l=&st->lanes[li];
    uint32_t d=l->peer_slot;
    if (l->queued) return;
    l->queued=1; l->sched_next=RANT__NIL;
    if (st->dest_head[d]==RANT__NIL) st->dest_head[d]=li;
    else st->lanes[st->dest_tail[d]].sched_next=li;
    st->dest_tail[d]=li;
    i_rant_dest_push(st, d);
}


/* A recycled record must never linger on a dest list, or it misroutes another lane's
 * submessages to the old peer. Unmatch time only, the lists are short. */
void i_rant_sched_drop(RantTransportState *st, uint32_t li){
    i_RantLane *l=&st->lanes[li];
    uint32_t d, cur, prev;
    if (!l->queued) return;
    d = l->peer_slot;
    prev = RANT__NIL; cur = st->dest_head[d];
    while (cur!=RANT__NIL && cur!=li){ prev=cur; cur=st->lanes[cur].sched_next; }
    if (cur==li){
        if (prev==RANT__NIL) st->dest_head[d]=l->sched_next;
        else st->lanes[prev].sched_next=l->sched_next;
        if (st->dest_tail[d]==li) st->dest_tail[d]=prev;
    }
    l->queued=0; l->sched_next=RANT__NIL;
}


/* Enqueues and tracks a freshly armed reader deadline for the poll cap. The sweep uses
 * i_rant_lane_enqueue instead, since it recomputes the deadline itself. */
void i_rant_lane_wake(RantTransportState *st, uint16_t topic_index, uint32_t peer_slot){
    uint32_t li=i_rant_lane_id(st,topic_index,peer_slot);
    i_RantLane *l;
    if (li==RANT__NIL) return;                       /* unmatched lane: nothing to schedule */
    l=&st->lanes[li];
    if (l->r.used && l->r.ack_pending) i_rant_transport_arm_deadline(st, l->r.ack_due_us);
    i_rant_lane_enqueue(st, li);
}


/* sendable work a popped record still owes now. Timer armed work is the sweep's job */
static int i_rant_lane_work(RantTransportState *st, const i_RantLane *l, uint64_t now){
    i_RantTopic *topic=&st->topics[l->topic];
    uint32_t peer_slot=l->peer_slot;
    if (!st->peer_used[peer_slot] || st->peer_dormant[peer_slot]) return 0;   /* dormant */
    if (l->w.used && l->w.has_nack) return 1;
    if (l->w.used && l->w.skip_hb) return 1;   /* directed floor HB still owed */
    if (l->w.used && l->w.sent_upto < topic->next_seqno
        && (l->w.rate_interval_us == 0 || now >= l->w.rate_next_us)) return 1;   /* throttled */
    if (l->r.used && topic->qos.reliability==RANT_RELIABLE
        && l->r.ack_pending && now >= l->r.ack_due_us) return 1;
    return 0;
}


/* Clock driven: a cursor walks the pool waking lanes whose timers came due, covering it
 * every RANT_HB_SWEEP_US. A forced full pass when next_deadline_us is due recomputes it. */
static void i_rant_hb_sweep(RantTransportState *st, uint64_t now){
    uint32_t total=st->lane_cap, due, k;
    uint64_t span = now - st->sweep_time_us;
    int forced = (now >= st->next_deadline_us);    /* a tracked timer is due */
    int full;
    uint64_t mind = RANT__NO_DEADLINE;               /* earliest not yet due timer seen */
    if (total==0){                                   /* no lanes have ever matched */
        if (forced) st->next_deadline_us = RANT__NO_DEADLINE;
        st->sweep_time_us = now;
        return;
    }
    due = (forced || span >= RANT_HB_SWEEP_US) ? total
        : (uint32_t)(span * total / RANT_HB_SWEEP_US);
    if (!due) return;              /* sweep_time_us advances only when lanes are paid */
    full = (due >= total);         /* covered every record, so mind is the global minimum */
    st->sweep_time_us = now;
    for (k=0;k<due;k++){
        uint32_t li=st->sweep;
        i_RantLane *l=&st->lanes[li];
        i_RantTopic *topic;
        uint32_t peer_slot;
        st->sweep = (st->sweep+1u>=total) ? 0u : st->sweep+1u;
        if (!l->in_use) continue;                  /* free pool slot */
        topic=&st->topics[l->topic];
        peer_slot=l->peer_slot;
        /* a throttled lane owes a send at its tick. First, since best effort skips below */
        if (l->w.used && l->w.rate_interval_us && l->w.sent_upto < topic->next_seqno
            && st->peer_used[peer_slot] && !st->peer_dormant[peer_slot]){
            if (now>=l->w.rate_next_us) i_rant_lane_enqueue(st,li);
            else if (l->w.rate_next_us < mind) mind = l->w.rate_next_us;
        }
        if (!i_rant_topic_needs_sweep(topic)) continue;     /* best effort, or nothing matched */
        /* HB gate on next_seqno, never the reader ack: a sub only node still owes acks */
        if (!st->peer_used[peer_slot] || st->peer_dormant[peer_slot]) continue;   /* dormant */
        if (l->w.used && l->w.reader_reliable && l->w.acked_upto < topic->next_seqno){
            if (now>=l->w.hb_next_us) i_rant_lane_enqueue(st,li);
            else if (l->w.hb_next_us < mind) mind = l->w.hb_next_us;
        }
        if (l->r.used && l->r.ack_pending){
            if (now>=l->r.ack_due_us) i_rant_lane_enqueue(st,li);
            else if (l->r.ack_due_us < mind) mind = l->r.ack_due_us;
        }
    }
    /* a full pass saw every timer, so mind is exact. Woken lanes re arm during emit */
    if (full) st->next_deadline_us = mind;
}


int rant_transport_poll_send(RantTransportState *st, uint32_t *to_peer, void *out, size_t cap, size_t *out_len, uint64_t now){
    uint32_t ndest=st->cfg.max_peers;
    i_rant_hb_sweep(st, now);
    while (st->dest_queue_count){
        uint32_t d; size_t offset=0;
        d = st->dest_queue[st->dest_queue_head];
        st->dest_queue_head = (st->dest_queue_head+1u>=ndest) ? 0u : st->dest_queue_head+1u;
        st->dest_queue_count--; st->dest_queued[d]=0;
        /* drain this destination's lanes into one datagram */
        while (st->dest_head[d]!=RANT__NIL){
            uint32_t li=st->dest_head[d];
            i_RantLane *l=&st->lanes[li];
            uint16_t topic_index=l->topic; uint32_t peer_slot=l->peer_slot;
            size_t n;
            do {
                /* acks first: small, and they carry the NACKs that drive repair */
                n=i_rant_reader_emit(st,(int)topic_index,(int)peer_slot,(uint8_t*)out+offset,cap-offset,now);
                if (!n) n=i_rant_writer_emit(st,(int)topic_index,(int)peer_slot,(uint8_t*)out+offset,cap-offset,now);
                offset+=n;
            } while (n && offset<cap);
            st->dest_head[d]=l->sched_next;
            if (i_rant_lane_work(st,l,now)){
                /* datagram full mid lane: rotate it to the back so siblings get the next */
                if (st->dest_head[d]==RANT__NIL) st->dest_head[d]=li;
                else {
                    l->sched_next=RANT__NIL;
                    st->lanes[st->dest_tail[d]].sched_next=li;
                    st->dest_tail[d]=li;
                }
                break;
            }
            l->queued=0;     /* lane drained */
        }
        if (st->dest_head[d]!=RANT__NIL) i_rant_dest_push(st,d);      /* fair: re queue at the tail */
        if (offset){
            *to_peer = st->peer_ids[d];
            *out_len = offset;
            return 1;
        }
        if (st->dest_head[d]!=RANT__NIL)
            return 0;    /* work pending but nothing fit: the cap is too small */
    }
    return 0;
}


uint64_t rant_transport_next_deadline_us(RantTransportState *st){
    return st->next_deadline_us == RANT__NO_DEADLINE ? 0 : st->next_deadline_us;
}


int rant_transport_tx_pending(RantTransportState *st){
    return st->dest_queue_count != 0;   /* the active lane queue */
}
