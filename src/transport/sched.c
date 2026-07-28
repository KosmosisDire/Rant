/* Transport active-lane scheduler and the outgoing poll. */
#include "internal.h"


/* scheduler: work is queued as lane-RECORD indices; destination = the record's peer slot */
static void i_dart_dest_push(DartTransportState *st, uint32_t d){
    uint32_t ndest = st->cfg.max_peers, pos;
    if (st->dest_queued[d]) return;
    st->dest_queued[d]=1;
    pos = st->dest_queue_head + st->dest_queue_count;
    if (pos >= ndest) pos -= ndest;
    st->dest_queue[pos]=d; st->dest_queue_count++;
}


/* enqueue a lane record that just got sendable work; idempotent while queued */
void i_dart_lane_enqueue(DartTransportState *st, uint32_t li){
    i_DartLane *l=&st->lanes[li];
    uint32_t d=l->peer_slot;
    if (l->queued) return;
    l->queued=1; l->sched_next=DART__NIL;
    if (st->dest_head[d]==DART__NIL) st->dest_head[d]=li;
    else st->lanes[st->dest_tail[d]].sched_next=li;
    st->dest_tail[d]=li;
    i_dart_dest_push(st, d);
}


/* remove a record from its dest list (a record being recycled must never linger on a
 * list: reallocated to another peer's lane, it would misroute that lane's submessages
 * into a datagram addressed to the OLD peer). Unmatch-time only; lists are short. */
void i_dart_sched_drop(DartTransportState *st, uint32_t li){
    i_DartLane *l=&st->lanes[li];
    uint32_t d, cur, prev;
    if (!l->queued) return;
    d = l->peer_slot;
    prev = DART__NIL; cur = st->dest_head[d];
    while (cur!=DART__NIL && cur!=li){ prev=cur; cur=st->lanes[cur].sched_next; }
    if (cur==li){
        if (prev==DART__NIL) st->dest_head[d]=l->sched_next;
        else st->lanes[prev].sched_next=l->sched_next;
        if (st->dest_tail[d]==li) st->dest_tail[d]=prev;
    }
    l->queued=0; l->sched_next=DART__NIL;
}


/* enqueue, and track a freshly-armed reader ack/NACK deadline for the poll cap. Used
 * by the arm sites (ack_due_us is future or 0); the sweep enqueues due lanes with
 * i_dart_lane_enqueue instead, since it recomputes next_deadline itself. */
void i_dart_lane_wake(DartTransportState *st, uint16_t topic_index, uint32_t peer_slot){
    uint32_t li=i_dart_lane_id(st,topic_index,peer_slot);
    i_DartLane *l;
    if (li==DART__NIL) return;                     /* unmatched lane: nothing to schedule */
    l=&st->lanes[li];
    if (l->r.used && l->r.ack_pending) i_dart_transport_arm_deadline(st, l->r.ack_due_us);
    i_dart_lane_enqueue(st, li);
}


/* sendable work a popped record still owes now (timer-armed work is the sweep's job) */
static int i_dart_lane_work(DartTransportState *st, const i_DartLane *l, uint64_t now){
    i_DartTopic *topic=&st->topics[l->topic];
    uint32_t peer_slot=l->peer_slot;
    if (!st->peer_used[peer_slot] || st->peer_dormant[peer_slot]) return 0;   /* dormant: out of flow control */
    if (l->w.used && l->w.has_nack) return 1;
    if (l->w.used && l->w.skip_hb) return 1;   /* directed floor HB still owed */
    if (l->w.used && l->w.sent_upto < topic->next_seqno
        && (l->w.rate_interval_us == 0 || now >= l->w.rate_next_us)) return 1;   /* throttled: not before the tick */
    if (l->r.used && topic->qos.reliability==DART_RELIABLE
        && l->r.ack_pending && now >= l->r.ack_due_us) return 1;
    return 0;
}


/* clock-driven counterpart of the wake calls: a cursor walks the record POOL waking
 * lanes whose timers came due. Two triggers: the amortized backstop (full coverage
 * every DART_HB_SWEEP_US) and a forced full pass when next_deadline_us comes due, so
 * a deadline-capped poll that wakes for a timer actually services it. A full pass
 * also recomputes next_deadline_us exactly (the global min of not-yet-due timers).
 * Read-only; cost is bounded by the pool (i.e. by matched lanes, not topics x peers:
 * an idle topic has no records to visit at all in dynamic mode). */
static void i_dart_hb_sweep(DartTransportState *st, uint64_t now){
    uint32_t total=st->lane_cap, due, k;
    uint64_t span = now - st->sweep_time_us;
    int forced = (now >= st->next_deadline_us);    /* a tracked timer is due */
    int full;
    uint64_t mind = DART__NO_DEADLINE;             /* earliest not-yet-due timer seen */
    if (total==0){                                 /* no lanes have ever matched */
        if (forced) st->next_deadline_us = DART__NO_DEADLINE;
        st->sweep_time_us = now;
        return;
    }
    due = (forced || span >= DART_HB_SWEEP_US) ? total
        : (uint32_t)(span * total / DART_HB_SWEEP_US);
    if (!due) return;              /* sweep_time_us advances only when lanes are paid */
    full = (due >= total);         /* covered every record -> mind is the global minimum */
    st->sweep_time_us = now;
    for (k=0;k<due;k++){
        uint32_t li=st->sweep;
        i_DartLane *l=&st->lanes[li];
        i_DartTopic *topic;
        uint32_t peer_slot;
        st->sweep = (st->sweep+1u>=total) ? 0u : st->sweep+1u;
        if (!l->in_use) continue;                  /* free pool slot */
        topic=&st->topics[l->topic];
        peer_slot=l->peer_slot;
        /* fire-and-forget rate throttle: a held lane owes a send when its tick comes due.
           Best-effort owes no HB/ack so needs_sweep skips it below -- do this FIRST. */
        if (l->w.used && l->w.rate_interval_us && l->w.sent_upto < topic->next_seqno
            && st->peer_used[peer_slot] && !st->peer_dormant[peer_slot]){
            if (now>=l->w.rate_next_us) i_dart_lane_enqueue(st,li);
            else if (l->w.rate_next_us < mind) mind = l->w.rate_next_us;
        }
        if (!i_dart_topic_needs_sweep(topic)) continue;   /* best-effort, or nothing matched */
        /* gate writer heartbeats on next_seqno, never the reader ack: a sub-only
           node's data topics never advance next_seqno but still owe acks */
        if (!st->peer_used[peer_slot] || st->peer_dormant[peer_slot]) continue;   /* dormant: out of flow control */
        if (l->w.used && l->w.reader_reliable && l->w.acked_upto < topic->next_seqno){
            if (now>=l->w.hb_next_us) i_dart_lane_enqueue(st,li);
            else if (l->w.hb_next_us < mind) mind = l->w.hb_next_us;
        }
        if (l->r.used && l->r.ack_pending){
            if (now>=l->r.ack_due_us) i_dart_lane_enqueue(st,li);
            else if (l->r.ack_due_us < mind) mind = l->r.ack_due_us;
        }
    }
    /* a full pass saw every timer: mind is the exact next deadline. Lanes woken above
       re-arm during emit (i_dart_transport_arm_deadline) and re-lower it; reader acks just clear. */
    if (full) st->next_deadline_us = mind;
}


int dart_transport_poll_send(DartTransportState *st, uint32_t *to_peer, void *out, size_t cap, size_t *out_len, uint64_t now){
    uint32_t ndest=st->cfg.max_peers;
    i_dart_hb_sweep(st, now);
    while (st->dest_queue_count){
        uint32_t d; size_t offset=0;
        d = st->dest_queue[st->dest_queue_head];
        st->dest_queue_head = (st->dest_queue_head+1u>=ndest) ? 0u : st->dest_queue_head+1u;
        st->dest_queue_count--; st->dest_queued[d]=0;
        /* drain this destination's lanes into one datagram */
        while (st->dest_head[d]!=DART__NIL){
            uint32_t li=st->dest_head[d];
            i_DartLane *l=&st->lanes[li];
            uint16_t topic_index=l->topic; uint32_t peer_slot=l->peer_slot;
            size_t n;
            do {
                /* acks first: small, one-shot, and carry the NACKs that drive
                   repair, so a backlogged writer can't starve them */
                n=i_dart_reader_emit(st,(int)topic_index,(int)peer_slot,(uint8_t*)out+offset,cap-offset,now);
                if (!n) n=i_dart_writer_emit(st,(int)topic_index,(int)peer_slot,(uint8_t*)out+offset,cap-offset,now);
                offset+=n;
            } while (n && offset<cap);
            st->dest_head[d]=l->sched_next;
            if (i_dart_lane_work(st,l,now)){
                /* datagram full mid-lane: rotate the lane to the back so siblings get the next */
                if (st->dest_head[d]==DART__NIL) st->dest_head[d]=li;
                else {
                    l->sched_next=DART__NIL;
                    st->lanes[st->dest_tail[d]].sched_next=li;
                    st->dest_tail[d]=li;
                }
                break;
            }
            l->queued=0;     /* lane drained */
        }
        if (st->dest_head[d]!=DART__NIL) i_dart_dest_push(st,d);  /* fair: re-queue at tail */
        if (offset){
            *to_peer = st->peer_ids[d];
            *out_len = offset;
            return 1;
        }
        if (st->dest_head[d]!=DART__NIL)
            return 0;    /* work pending but nothing fit: caller's cap too small */
    }
    return 0;
}


uint64_t dart_transport_next_deadline_us(DartTransportState *st){
    return st->next_deadline_us == DART__NO_DEADLINE ? 0 : st->next_deadline_us;
}


int dart_transport_tx_pending(DartTransportState *st){
    return st->dest_queue_count != 0;   /* the active-lane queue: empty = nothing to emit */
}
