/* Transport reader path: ordering, reassembly, delivery, the ACKNACK emit. */
#include "internal.h"


int dart_transport_subscriber_progress(DartTransportState *st, uint16_t topic_index, uint32_t peer,
                         uint64_t *base_seqno, uint32_t *have, uint32_t *total){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    int peer_slot; i_DartReaderProxy *r;
    if (!topic) return 0;
    peer_slot = i_dart_peer_slot(st, peer);
    if (peer_slot < 0) return 0;
    r = i_dart_reader_proxy_at(st,topic_index,peer_slot);
    if (!r || !r->used || !r->assembly_active) return 0;      /* no message mid-reassembly */
    if (base_seqno) *base_seqno = r->deliver_upto;       /* HOL message starts here */
    if (total)      *total      = r->assembly_count;
    if (have){
        uint32_t i, c=0;
        for (i=0;i<r->assembly_count;i++) if (i_dart_bit_get(r->frag_bitmap,i)) c++;
        *have = c;
    }
    return 1;
}


/* diagnostic: attribute each reader NACK-arm (a 0->1 transition of ack_pending) to
 * its cause -- a DATA/SHM-DATA arrival or a heartbeat. Pure counting, done by
 * i_dart_reader_ack_now before it sets r->ack_pending. arms_data tracks gap-triggered and
 * progress-refill arming; arms_hb tracks the writer's idle ping (which also drives the
 * tail-loss backstop). Read via dart_transport_repair_stats. (The self-clocked retransmit backstop
 * re-fires via nack_retransmit_us without a fresh arm, so it is not counted here. Resume/
 * position-report arms are control, not counted.) */
static void i_dart_reader_arm(i_DartTopic *topic, i_DartReaderProxy *r, int is_hb){
    if (!r->ack_pending){ if (is_hb) topic->repair_stats.arms_hb++; else topic->repair_stats.arms_data++; }
}

/* Arm an immediate ACKNACK on this lane: attribute the arm, mark it pending and due NOW,
 * optionally force it, then wake the lane so the next poll emits it. The attribution must
 * run BEFORE ack_pending is set (that ordering is the whole point of the arm counter: it
 * counts 0->1 transitions). force = this arm owes a cumulative ack even at an unchanged
 * repair floor (delivery, skip, HB, (re)match); is_hb attributes it to the writer's ping.
 * Every counted arm in this file goes through here; the control arms in transport/core.c
 * (resume, position report) stay direct and uncounted on purpose. */
static void i_dart_reader_ack_now(DartTransportState *st, i_DartTopic *topic, i_DartReaderProxy *r,
                                  uint16_t topic_index, uint32_t peer_slot, int force, int is_hb){
    i_dart_reader_arm(topic, r, is_hb);
    r->ack_pending = 1; r->ack_due_us = 0;
    if (force) r->ack_force = 1;
    i_dart_lane_wake(st, topic_index, peer_slot);
}

static i_DartReaderOrder i_dart_reader_order_arrival(DartTransportState *st, int topic_index, int peer_slot,
                                             i_DartReaderProxy *r, uint64_t base, uint64_t top){
    i_DartTopic *topic=&st->topics[topic_index];
    if (base < r->deliver_upto) return DART_ORDER_OLD;
    if (top > r->received_high) r->received_high = top;          /* proof these seqnos exist */
    if (base > r->deliver_upto){
        if (topic->qos.reliability==DART_RELIABLE && r->started){   /* gap: arm a repair NACK */
            /* an ALREADY-armed lane keeps its ack_due_us (re-arming would drop a paced
               retransmit deadline back to now): just re-wake it */
            if (r->ack_pending) i_dart_lane_wake(st,(uint16_t)topic_index,(uint32_t)peer_slot);
            else i_dart_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,0,0);
            return DART_ORDER_GAP;
        }
        if (r->started && !topic->directed){                     /* best-effort / first contact: adopt.
                                                                    directed: skipping a seqno addressed
                                                                    elsewhere is not loss */
            i_dart_transport_fire_event(st, DART_TRANSPORT_MSG_LOST, (uint16_t)topic_index, st->peer_ids[peer_slot],
                        r->deliver_upto, base - r->deliver_upto);
            topic->repair_stats.msgs_skipped += base - r->deliver_upto;
        }
        r->deliver_upto = base;
        return DART_ORDER_ADOPTED;
    }
    return DART_ORDER_INORDER;
}

#ifdef DART_SHM

/* reader side: handle an SHM-DATA submessage. It covers [base, base+count) in one
 * shot (payload is in shared memory), so there is no reassembly -- just ordering,
 * then hand the descriptor to on_shm (the node resolves + delivers + acks). The gap
 * case re-uses the normal NACK window (i_dart_reader_emit's !assembly_active branch). */
void i_dart_reader_shm(DartTransportState *st, int topic_index, int peer_slot, const uint8_t *p, uint64_t now){
    i_DartTopic *topic=&st->topics[topic_index];
    i_DartReaderProxy *r=i_dart_reader_proxy_at(st,topic_index,peer_slot);
    int reliable = (topic->qos.reliability==DART_RELIABLE);
    uint64_t base = i_dart_le_r64(p+DART_OFFSET_SEQNO);
    uint16_t count = i_dart_le_r16(p+DART_OFFSET_SHM_COUNT);
    const uint8_t *desc = p+DART_OFFSET_SHM_DESC;          /* DART_SHM_DESC_BYTES */
    if (!r || !r->used || count==0) return;
    if (r->parked){ topic->repair_stats.frags_ahead++; return; }   /* held sample blocks the line:
                                                                   retry is deliver_parked's job */
    {   i_DartReaderOrder ord = i_dart_reader_order_arrival(st, topic_index, peer_slot, r, base, base+count-1);
        if (ord==DART_ORDER_OLD || ord==DART_ORDER_GAP) return;   /* old/dup, or repair armed for a gap */
    }
    r->started = 1; r->assembly_active = 0;
    /* in order (base == deliver_upto). Resolve the chunk; advance + ack ONLY if the
       node delivered. A failed resolve (recycled, or a transient unattachable segment)
       leaves the gap so the reliability layer repairs it (re-sent descriptor) or skips
       it (writer HB, sample evicted). A persistently unresolvable descriptor (mis-
       configured SHM constants) would loop, so after DART_SHM_MAX_RETRY tries we skip
       it loudly instead of wedging. */
    {   int ok = st->cfg.on_shm ?
                 st->cfg.on_shm(st->cfg.user, (uint16_t)topic_index, st->peer_ids[peer_slot], desc) : 0;
        if (ok > 0){
            r->shm_fail = 0;
            r->deliver_upto = base + count;
            if (reliable)                   /* ack now, AFTER delivery (zero-copy invariant) */
                i_dart_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,1,0);
            return;
        }
        if (ok < 0){                        /* refused downstream (consumer queue full) */
            if (!reliable){                 /* best-effort: KEEP_LAST drop */
                r->deliver_upto = base + count;
                return;
            }
            /* park the DESCRIPTOR (p is the shared RX buffer: it must be copied). Held in
               assembly_buf; the chunk itself stays valid while unacked (the writer's flow
               control pins its history slot). An alloc failure falls through to the
               resolve-fail repair path: the writer re-sends and we retry. */
            if (r->assembly_cap < DART_SHM_DESC_BYTES){
                uint8_t *nb = (uint8_t*)st->cfg.allocator(st->cfg.user, r->assembly_buf, DART_SHM_DESC_BYTES);
                if (nb){ r->assembly_buf = nb; r->assembly_cap = DART_SHM_DESC_BYTES; }
            }
            if (r->assembly_cap >= DART_SHM_DESC_BYTES){
                memcpy(r->assembly_buf, desc, DART_SHM_DESC_BYTES);
                r->assembly_count = count;             /* seqnos the held sample spans */
                r->parked = 1; r->parked_shm = 1;      /* silent: no ack, no NACK, no advance */
                return;
            }
        }
        if (reliable && ++r->shm_fail >= DART_SHM_MAX_RETRY){
            i_dart_transport_fire_event(st, DART_TRANSPORT_MSG_LOST, (uint16_t)topic_index, st->peer_ids[peer_slot],
                        base, count);
            topic->repair_stats.msgs_skipped += count;
            r->shm_fail = 0;
            r->deliver_upto = base + count;             /* give up: skip past it, ack the new edge */
            i_dart_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,1,0);
        } else if (reliable){                           /* leave the gap, NACK for a re-send */
            i_dart_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,0,0);
        } else {
            r->deliver_upto = base + count;             /* best-effort: no repair, drop it */
            i_dart_lane_wake(st,(uint16_t)topic_index,(uint32_t)peer_slot);
        }
    }
}
#endif


/* reader side: handle DATA */
void i_dart_reader_data(DartTransportState *st, int topic_index, int peer_slot, const uint8_t *p,
                           uint64_t now){
    i_DartTopic *topic=&st->topics[topic_index];
    i_DartReaderProxy *r=i_dart_reader_proxy_at(st,topic_index,peer_slot);
    int reliable = (topic->qos.reliability==DART_RELIABLE);
    int new_frag = 0;
    uint64_t seqno, base; uint16_t frag, count, payload_len; uint32_t sample_len; const uint8_t *payload;
    if (p[0] & DART_F_SINGLE){           /* single fragment: frag/count/len implied */
        seqno=i_dart_le_r64(p+DART_OFFSET_SEQNO); frag=0; count=1; payload_len=i_dart_le_r16(p+DART_OFFSET_PAYLOAD_LEN_SINGLE); sample_len=payload_len; payload=p+DART_HEADER_DATA_SINGLE;
    } else {
        seqno=i_dart_le_r64(p+DART_OFFSET_SEQNO); frag=i_dart_le_r16(p+DART_OFFSET_FRAG); count=i_dart_le_r16(p+DART_OFFSET_COUNT);
        sample_len=i_dart_le_r32(p+DART_OFFSET_SAMPLE_LEN); payload_len=i_dart_le_r16(p+DART_OFFSET_PAYLOAD_LEN); payload=p+DART_HEADER_DATA_MULTI;
    }
    base = seqno - frag;

    if (!r || !r->used){ topic->repair_stats.frags_malformed++; return; }     /* not subscribed */
    if (count==0 || frag>=count){ topic->repair_stats.frags_malformed++; return; }  /* malformed */
    if (r->parked){ topic->repair_stats.frags_ahead++; return; }   /* held sample blocks the line:
                                                                   retry is deliver_parked's job */
    switch (i_dart_reader_order_arrival(st, topic_index, peer_slot, r, base, seqno)){
        case DART_ORDER_OLD:     topic->repair_stats.frags_old++;   return;   /* already delivered/skipped */
        case DART_ORDER_GAP:     topic->repair_stats.frags_ahead++; return;   /* future frag; repair armed */
        case DART_ORDER_ADOPTED: topic->repair_stats.frags_ahead++; r->assembly_active=0; break;  /* skipped past loss */
        case DART_ORDER_INORDER: break;
    }
    r->started = 1;   /* writer engaged: position adopted */
    /* fit the reassembly buffers (grown to fit via the hook); "too big" = the
       allocation failed: skip the whole sample and report it */
    { uint32_t bitmap_need = ((uint32_t)count + 7u) / 8u, buf_cap, bitmap_bytes; int too_big = 0;
      if (r->assembly_cap < sample_len){
          uint8_t *new_buf = (uint8_t*)st->cfg.allocator(st->cfg.user, r->assembly_buf, sample_len?sample_len:1u);
          if (!new_buf) too_big = 1; else { r->assembly_buf = new_buf; r->assembly_cap = sample_len?sample_len:1u; }
      }
      if (!too_big && r->bitmap_cap < bitmap_need){
          uint8_t *new_bitmap = (uint8_t*)st->cfg.allocator(st->cfg.user, r->frag_bitmap, bitmap_need?bitmap_need:1u);
          if (!new_bitmap) too_big = 1; else { r->frag_bitmap = new_bitmap; r->bitmap_cap = bitmap_need?bitmap_need:1u; }
      }
      if (too_big){
          i_dart_transport_fire_event(st, DART_TRANSPORT_MSG_TOO_BIG, (uint16_t)topic_index, st->peer_ids[peer_slot],
                      0, sample_len);
          r->deliver_upto = base + count; r->assembly_active = 0;
          if (reliable) i_dart_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,1,0);
          return;
      }
      buf_cap  = r->assembly_cap;
      bitmap_bytes = bitmap_need;
      /* base == deliver_upto: current sample */
      if (!r->assembly_active){
          r->assembly_active=1; r->assembly_count=count; r->assembly_len=sample_len; r->assembly_low=0;
          r->nack_high=base;     /* in-flight dedup is per-message: start this one fresh */
          memset(r->frag_bitmap,0,bitmap_bytes);
      }
      if (count!=r->assembly_count) return;                  /* inconsistent, ignore */
      topic->repair_stats.frags_recv++;                             /* every accepted DATA fragment, dups included */
      new_frag = !i_dart_bit_get(r->frag_bitmap,frag);
      if (new_frag){
          /* reassemble at the SOURCE peer's fragment size (advertised via discovery);
             the bitmap was sized to this sample's count and the buf_cap guard
             catches any stray offset */
          uint32_t offset=(uint32_t)frag*st->peer_frag[peer_slot];
          if (offset+payload_len<=buf_cap) memcpy(r->assembly_buf+offset,payload,payload_len);
          i_dart_bit_set(r->frag_bitmap,frag);
          if (frag==r->assembly_low)                          /* extended the contiguous-received front */
              while (r->assembly_low<count && i_dart_bit_get(r->frag_bitmap,r->assembly_low)) r->assembly_low++;
      } else topic->repair_stats.frags_dup++;                        /* already held: repair overlap / waste */
    }
    /* assembly_low is the contiguous front, so the sample is complete iff it reached the end.
       Deliver in order, advance, then arm the ACKNACK. A completed sample owes an immediate
       cumulative ack (ack_force). A still-partial sample only re-arms when this frag opened or
       advanced a real gap (a hole below received_high): a healthy in-order fill owes nothing, and
       emit dedups + paces the repair request so we never re-flood the writer with in-flight
       fragments. */
    { int done = (r->assembly_low == count);
      int hole = r->assembly_active && (r->deliver_upto + r->assembly_low <= r->received_high);
      if (done){
          if (st->cfg.on_message &&
              st->cfg.on_message(st->cfg.user, (uint16_t)topic_index, st->peer_ids[peer_slot],
                                 dart_bytes(r->assembly_buf, r->assembly_len)) != 0 && reliable){
              /* refused downstream (consumer queue full): PARK the assembled sample.
                 No advance, no ack, no repair traffic -- our silence keeps the writer's
                 acked_upto put, so its own flow control backpressures the publisher.
                 deliver_parked retries; a writer floor past us (HB) gives up + skips.
                 A best-effort refusal falls through: KEEP_LAST drop. */
              r->parked = 1;
              return;
          }
          r->deliver_upto = base + count;
          r->assembly_active=0;
      }
      if (reliable){
          if (done)
              i_dart_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,1,0);
          else if (new_frag && hole)              /* gap revealed, or repair advanced: request now */
              i_dart_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,0,0);
      }
    }
}


void i_dart_reader_hb(DartTransportState *st, int topic_index, int peer_slot, const uint8_t *p, uint64_t now){
    i_DartTopic *topic=&st->topics[topic_index];
    i_DartReaderProxy *r=i_dart_reader_proxy_at(st,topic_index,peer_slot);
    uint64_t first=i_dart_le_r64(p+DART_OFFSET_SEQNO), last=i_dart_le_r64(p+DART_OFFSET_HB_LAST);
    if (!r || !r->used) return;
    if (topic->qos.reliability!=DART_RELIABLE) return;
    /* un-started readers adopt no position from heartbeats (a one-sided flap's
       advertised first may be a dead predecessor's); the ack below carries our epoch.
       A writer raises its HB `first` to our acked_upto (the join-point trick), and our
       ACKNACK acks the contiguous-received front -- which sits INSIDE the sample we are
       still assembling. Eviction is whole-message, so a genuine floor never splits a
       sample: ignore a `first` that lands in our current partial (it is just our own
       mid-message ack echoed back), else we would skip past frags we are repairing and
       reject every resend as old. */
    /* a PARKED hold occupies [deliver_upto, deliver_upto+assembly_count) exactly like a
       partial assembly, so the same mid-sample guard protects it from our own ack echo */
    if (r->started && first > r->deliver_upto &&
        (!(r->assembly_active || r->parked) || first >= r->deliver_upto + r->assembly_count)){
        if (!topic->directed){   /* directed: the floor advanced because a seqno was addressed
                                    to another peer, not real loss -- skip silently */
            i_dart_transport_fire_event(st, DART_TRANSPORT_MSG_LOST, (uint16_t)topic_index, st->peer_ids[peer_slot],   /* superseded before repair */
                        r->deliver_upto, first - r->deliver_upto);
            topic->repair_stats.msgs_skipped += first - r->deliver_upto;
        }
        r->deliver_upto=first; r->assembly_active=0;
        r->parked=0;            /* the writer moved past the held sample: give it up */
#ifdef DART_SHM
        r->parked_shm=0;
        r->shm_fail=0;          /* skipped past the stuck descriptor: fresh count */
#endif
    }
    if (r->parked) return;      /* still parked: stay silent (no ack) -- the writer's
                                   flow control is the backpressure */
    /* hb_last is the writer's CLAIM (it may exceed what we've received). It is the only
       way to learn of tail loss -- frags past received_high that no later arrival will reveal --
       so emit lets the slow retransmit backstop chase up to it, never the fast gap path.
       The HB always owes a cumulative ack so a writer that lost ours stops re-pinging. */
    r->hb_last=last;
    i_dart_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,1,1);
}


/* produce a reader ACKNACK for (topic_index,peer_slot) if due; 0 if none.
 *
 * The repair request is GAP-TRIGGERED and bounded by what we have actually RECEIVED.
 * UDP frags are assumed delivered in order, so a hole below received_high is real loss while
 * anything above it is still in flight and must NOT be NACKed -- that "request up to the
 * writer's heartbeat CLAIM, every poll" was the old congestion collapse. Each floor is
 * asked once: nack_high tracks how far we have already requested, so a progress refill asks
 * only (nack_high, top] and never re-requests the still-outstanding lower frags. A stalled
 * floor is re-asked only after the retransmit backstop (repair_delay), which is also the
 * one path allowed to chase the writer's claim (hb_last) so tail loss still repairs.
 * Outstanding repair is therefore capped at one DART_NACK_WINDOW and clocked to delivery,
 * so it cannot scale into a flood with the gap size or the message size. */
size_t i_dart_reader_emit(DartTransportState *st, int topic_index, int peer_slot, uint8_t *out, size_t cap, uint64_t now){
    i_DartTopic *topic=&st->topics[topic_index];
    i_DartReaderProxy *r=i_dart_reader_proxy_at(st,topic_index,peer_slot);
    uint64_t first_missing, bound, top; uint16_t nbits=0; uint32_t bitmap=0;
    uint16_t index = i_dart_wire_index_of(st, topic_index);
    int due, holes=0, repair=0, force;
    if (!r || !r->used || st->peer_dormant[peer_slot]) return 0;   /* unmatched/dormant: don't ack */
    if (topic->qos.reliability!=DART_RELIABLE) return 0;
    if (cap<DART_HEADER_NACK) return 0;
    if (!r->ack_pending || now<r->ack_due_us) return 0;
    r->ack_pending=0; force=r->ack_force; r->ack_force=0;
    if (r->parked) return 0;    /* parked: silent (no ack, no NACK). Consuming ack_pending
                                   above keeps the drained lane off the scheduler. */

    if (!r->started)                                     /* no position yet: F_UNPOS announces our epoch */
        return i_dart_wire_mk_nack(out,index,r->deliver_upto,0,0,r->epoch,(uint8_t)DART_F_UNPOS);

    /* the cumulative-ack point and repair-window base: our contiguous-received front */
    first_missing = r->assembly_active ? r->deliver_upto + r->assembly_low : r->deliver_upto;

    /* request ceiling = what we've received. Only the slow backstop may reach the writer's
       claim, so tail loss (no later frag will ever reveal it) still gets repaired. */
    due   = (now >= r->nack_retransmit_us);
    bound = r->received_high;
    if (due && r->hb_last > bound) bound = r->hb_last;

    if (first_missing <= bound){                         /* a hole sits below something we've heard */
        holes = 1;
        top = first_missing + DART_NACK_WINDOW;          /* one window per ACKNACK: the in-flight cap */
        if (top > bound + 1) top = bound + 1;
        if (r->assembly_active && top > r->deliver_upto + r->assembly_count)
            top = r->deliver_upto + r->assembly_count;        /* this sample's frags only (bitmap range) */
        {   /* in-flight dedup (skip the still-outstanding lower part) is only valid while we
               are assembling THIS message: its clear frag_bitmap bits are genuinely in flight. With
               no sample yet (whole message missing) there is nothing we can hold, and the slow
               backstop re-asks everything, so both ask from the floor. */
            uint64_t from = (due || !r->assembly_active) ? first_missing
                : (r->nack_high > first_missing ? r->nack_high : first_missing); /* refill: only the new part */
            uint64_t s;
            for (s=from; s<top; s++){
                int missing = r->assembly_active ? !i_dart_bit_get(r->frag_bitmap,(uint32_t)(s - r->deliver_upto)) : 1;
                if (missing) bitmap |= (1u << (uint32_t)(s - first_missing));
            }
            if (bitmap){
                nbits = (uint16_t)(top - first_missing);
                repair = 1;
                topic->repair_stats.nacks_sent++;
                if (top > r->nack_high) r->nack_high = top;
                r->nack_retransmit_us = now + topic->qos.repair_delay_us;
            }
        }
    } else r->nack_high = first_missing;                   /* caught up to received: end the episode */

    /* keep the lane live while a hole remains so the backstop re-fires; an arrival that
       advances the floor re-arms us immediately (ack_due_us=0) for the next window. */
    if (holes){ r->ack_pending=1; r->ack_due_us=r->nack_retransmit_us; i_dart_transport_arm_deadline(st,r->ack_due_us); }

    /* send only to carry a repair request or a delivery/skip/HB/(re)match cumulative ack;
       a bare re-ack at an unchanged floor would be pure noise. */
    if (!repair && !force) return 0;
    return i_dart_wire_mk_nack(out,index,first_missing,nbits,bitmap,r->epoch,0);
}


/* Retry every parked lane of a topic (see i_DartReaderProxy.parked). The callbacks may
 * re-enter the transport (a send from an event handler grows buffers), so lane pointers
 * are re-derived after each call; the held buffer itself is a stable heap allocation. */
uint32_t dart_transport_deliver_parked(DartTransportState *st, uint16_t topic_index, uint64_t now){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    uint32_t li, still = 0;
    (void)now;
    if (!topic) return 0;
    li = topic->lane_head;
    while (li != DART__NIL){
        uint32_t next = st->lanes[li].topic_next;
        if (st->lanes[li].r.used && st->lanes[li].r.parked){
            uint32_t peer_slot = st->lanes[li].peer_slot;
            uint32_t peer_id   = st->peer_ids[peer_slot];
            const uint8_t *held = st->lanes[li].r.assembly_buf;
            uint32_t held_len   = st->lanes[li].r.assembly_len;
            i_DartReaderProxy *r;
            int accepted;
#ifdef DART_SHM
            if (st->lanes[li].r.parked_shm){
                int ok = st->cfg.on_shm ?
                         st->cfg.on_shm(st->cfg.user, topic_index, peer_id, held) : 0;
                r = &st->lanes[li].r;                  /* the callback may have re-entered */
                if (ok == 0){
                    /* chunk gone (the writer moved on after its backpressure bound):
                       un-park, leave the gap, and let the normal repair path re-fetch
                       or skip it (re-sent descriptor / writer HB floor) */
                    r->parked = 0; r->parked_shm = 0; r->assembly_active = 0;
                    i_dart_reader_ack_now(st,topic,r,topic_index,peer_slot,0,0);
                    li = next; continue;
                }
                accepted = (ok > 0);
            } else
#endif
            {
                accepted = !st->cfg.on_message ||
                           st->cfg.on_message(st->cfg.user, topic_index, peer_id,
                                              dart_bytes(held, held_len)) == 0;
                r = &st->lanes[li].r;                  /* the callback may have re-entered */
            }
            if (accepted){
                r->deliver_upto += r->assembly_count;
                r->assembly_active = 0;
                r->parked = 0;
#ifdef DART_SHM
                r->parked_shm = 0; r->shm_fail = 0;
#endif
                if (topic->qos.reliability==DART_RELIABLE)    /* ack now, AFTER delivery */
                    i_dart_reader_ack_now(st,topic,r,topic_index,peer_slot,1,0);
            } else still++;
        }
        li = next;
    }
    return still;
}
