/* Transport active-lane scheduler and the outgoing poll. */
#include "internal.h"


/* scheduler: lane index = channel_idx*max_peers + peer_slot; destination = peer slot */
static void i_dart_dest_push(DartTransportState *st, uint32_t d){
    uint32_t ndest = st->cfg.max_peers, t;
    if (st->dest_queued[d]) return;
    st->dest_queued[d]=1;
    t = st->dest_queue_head + st->dest_queue_count;
    if (t >= ndest) t -= ndest;
    st->dest_queue[t]=d; st->dest_queue_count++;
}


/* enqueue a lane that just got sendable work; idempotent while queued */
static void i_dart_lane_enq(DartTransportState *st, uint16_t channel_idx, uint32_t peer_slot){
    uint32_t max_peers=st->cfg.max_peers;
    uint32_t lane=(uint32_t)channel_idx*max_peers+peer_slot;
    uint32_t d=peer_slot;
    if (st->lane_queued[lane]) return;
    st->lane_queued[lane]=1; st->lane_next[lane]=DART__NIL;
    if (st->dest_head[d]==DART__NIL) st->dest_head[d]=lane;
    else st->lane_next[st->dest_tail[d]]=lane;
    st->dest_tail[d]=lane;
    i_dart_dest_push(st, d);
}


/* enqueue, and track a freshly-armed reader ack/NACK deadline for the poll cap. Used
 * by the arm sites (ack_due_us is future or 0); the sweep enqueues due lanes with
 * i_dart_lane_enq instead, since it recomputes next_deadline itself. */
void i_dart_lane_wake(DartTransportState *st, uint16_t channel_idx, uint32_t peer_slot){
    i_DartReaderProxy *r=i_dart_reader_proxy_at(st,channel_idx,peer_slot);
    if (r->used && r->ack_pending) i_dart_transport_arm_deadline(st, r->ack_due_us);
    i_dart_lane_enq(st, channel_idx, peer_slot);
}


/* sendable work a popped lane still owes now (timer-armed work is the sweep's job) */
static int i_dart_lane_work(DartTransportState *st, uint16_t channel_idx, uint32_t peer_slot, uint64_t now){
    i_DartChannel *ch=&st->channels[channel_idx];
    if (!st->peer_used[peer_slot] || st->peer_dormant[peer_slot]) return 0;   /* dormant: out of flow control */
    { i_DartWriterProxy *w=i_dart_writer_proxy_at(st,channel_idx,peer_slot);
      i_DartReaderProxy *r=i_dart_reader_proxy_at(st,channel_idx,peer_slot);
      if (w->used && w->has_nack) return 1;
      if (w->used && w->sent_upto < ch->next_seqno) return 1;
      if (r->used && ch->qos.reliability==DART_RELIABLE
          && r->ack_pending && now >= r->ack_due_us) return 1;
    }
    return 0;
}


/* clock-driven counterpart of the wake calls: a cursor walks the lane table waking
 * lanes whose timers came due. Two triggers: the amortized backstop (full coverage
 * every DART_HB_SWEEP_US) and a forced full pass when next_deadline_us comes due, so
 * a deadline-capped poll that wakes for a timer actually services it. A full pass
 * also recomputes next_deadline_us exactly (the global min of not-yet-due timers).
 * Read-only; cost is bounded by table size. */
static void i_dart_hb_sweep(DartTransportState *st, uint64_t now){
    uint32_t max_peers=st->cfg.max_peers;
    uint32_t total=(uint32_t)st->cfg.n_channels*max_peers, due, k;
    uint64_t span = now - st->sweep_time_us;
    int forced = (now >= st->next_deadline_us);    /* a tracked timer is due */
    int full;
    uint64_t mind = DART__NO_DEADLINE;             /* earliest not-yet-due timer seen */
    due = (forced || span >= DART_HB_SWEEP_US) ? total
        : (uint32_t)(span * total / DART_HB_SWEEP_US);
    if (!due) return;              /* sweep_time_us advances only when lanes are paid */
    full = (due >= total);         /* covered every lane -> mind is the global minimum */
    st->sweep_time_us = now;
    for (k=0;k<due;k++){
        uint32_t lane=st->sweep, peer_slot=lane%max_peers;
        uint16_t channel_idx=(uint16_t)(lane/max_peers);
        i_DartChannel *ch=&st->channels[channel_idx];
        st->sweep = (st->sweep+1u>=total) ? 0u : st->sweep+1u;
        /* gate writer heartbeats on next_seqno, never the reader ack: a sub-only
           node's data channels never advance next_seqno but still owe acks */
        if (ch->qos.reliability!=DART_RELIABLE) continue;
        if (!st->peer_used[peer_slot] || st->peer_dormant[peer_slot]) continue;   /* dormant: out of flow control */
        { i_DartWriterProxy *w=i_dart_writer_proxy_at(st,channel_idx,peer_slot);
          i_DartReaderProxy *r=i_dart_reader_proxy_at(st,channel_idx,peer_slot);
          if (w->used && w->reader_reliable && w->acked_upto < ch->next_seqno){
              if (now>=w->hb_next_us) i_dart_lane_enq(st,channel_idx,peer_slot);
              else if (w->hb_next_us < mind) mind = w->hb_next_us;
          }
          if (r->used && r->ack_pending){
              if (now>=r->ack_due_us) i_dart_lane_enq(st,channel_idx,peer_slot);
              else if (r->ack_due_us < mind) mind = r->ack_due_us;
          }
        }
    }
    /* a full pass saw every timer: mind is the exact next deadline. Lanes woken above
       re-arm during emit (i_dart_transport_arm_deadline) and re-lower it; reader acks just clear. */
    if (full) st->next_deadline_us = mind;
}


int dart_transport_poll_send(DartTransportState *st, uint32_t *to_peer, void *out, size_t cap, size_t *out_len, uint64_t now){
    uint32_t max_peers=st->cfg.max_peers, ndest=max_peers;
    i_dart_hb_sweep(st, now);
    while (st->dest_queue_count){
        uint32_t d; size_t offset=0;
        d = st->dest_queue[st->dest_queue_head];
        st->dest_queue_head = (st->dest_queue_head+1u>=ndest) ? 0u : st->dest_queue_head+1u;
        st->dest_queue_count--; st->dest_queued[d]=0;
        /* drain this destination's lanes into one datagram */
        while (st->dest_head[d]!=DART__NIL){
            uint32_t lane=st->dest_head[d], peer_slot=lane%max_peers;
            uint16_t channel_idx=(uint16_t)(lane/max_peers);
            size_t n;
            do {
                /* acks first: small, one-shot, and carry the NACKs that drive
                   repair, so a backlogged writer can't starve them */
                n=i_dart_reader_emit(st,(int)channel_idx,(int)peer_slot,(uint8_t*)out+offset,cap-offset,now);
                if (!n) n=i_dart_writer_emit(st,(int)channel_idx,(int)peer_slot,(uint8_t*)out+offset,cap-offset,now);
                offset+=n;
            } while (n && offset<cap);
            st->dest_head[d]=st->lane_next[lane];
            if (i_dart_lane_work(st,channel_idx,peer_slot,now)){
                /* datagram full mid-lane: rotate the lane to the back so siblings get the next */
                if (st->dest_head[d]==DART__NIL) st->dest_head[d]=lane;
                else {
                    st->lane_next[lane]=DART__NIL;
                    st->lane_next[st->dest_tail[d]]=lane;
                    st->dest_tail[d]=lane;
                }
                break;
            }
            st->lane_queued[lane]=0;     /* lane drained */
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
