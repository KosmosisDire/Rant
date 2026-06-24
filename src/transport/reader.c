/* Transport reader path: ordering, reassembly, delivery, the ACKNACK emit. */
#include "internal.h"


int dart_reader_progress(dart_state *st, uint16_t channel, uint32_t peer,
                         uint64_t *base_seqno, uint32_t *have, uint32_t *total){
    int channel_idx; dart_channel *ch = dart_chan(st, channel, &channel_idx);
    int peer_slot; dart_reader_proxy *r;
    if (!ch) return 0;
    peer_slot = dart_peer_slot(st, peer);
    if (peer_slot < 0) return 0;
    r = dart__reader_proxy_at(st,channel_idx,peer_slot);
    if (!r->used || !r->assembly_active) return 0;            /* no message mid-reassembly */
    if (base_seqno) *base_seqno = r->deliver_upto;       /* HOL message starts here */
    if (total)      *total      = r->assembly_count;
    if (have){
        uint32_t i, c=0;
        for (i=0;i<r->assembly_count;i++) if (dart_bget(r->frag_bitmap,i)) c++;
        *have = c;
    }
    return 1;
}


/* diagnostic: attribute each reader NACK-arm (a 0->1 transition of ack_pending) to
 * its cause -- a DATA/SHM-DATA arrival or a heartbeat. Pure counting, call it right
 * before any r->ack_pending=1 in the repair paths. arms_data tracks gap-triggered and
 * progress-refill arming; arms_hb tracks the writer's idle ping (which also drives the
 * tail-loss backstop). Read via dart_repair_stats. (The self-clocked retransmit backstop
 * re-fires via nack_retransmit_us without a fresh arm, so it is not counted here. Resume/
 * position-report arms are control, not counted.) */
static void dart__arm(dart_channel *ch, dart_reader_proxy *r, int is_hb){
    if (!r->ack_pending){ if (is_hb) ch->repair_stats.arms_hb++; else ch->repair_stats.arms_data++; }
}

static dart_reader_order dart__order_arrival(dart_state *st, int channel_idx, int peer_slot,
                                             dart_reader_proxy *r, uint64_t base, uint64_t top){
    dart_channel *ch=&st->channels[channel_idx];
    if (base < r->deliver_upto) return DART_ORDER_OLD;
    if (top > r->received_high) r->received_high = top;          /* proof these seqnos exist */
    if (base > r->deliver_upto){
        if (ch->qos.reliability==DART_RELIABLE && r->started){   /* gap: arm a repair NACK */
            if (!r->ack_pending){ dart__arm(ch,r,0); r->ack_pending=1; r->ack_due_us=0; }
            dart__lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
            return DART_ORDER_GAP;
        }
        if (r->started){                                         /* best-effort / first contact: adopt */
            dart__event(st, DART_MSG_LOST, (uint16_t)channel_idx, st->peer_ids[peer_slot],
                        r->deliver_upto, base - r->deliver_upto, "message(s) lost");
            ch->repair_stats.msgs_skipped += base - r->deliver_upto;
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
 * case re-uses the normal NACK window (dart_reader_emit's !assembly_active branch). */
void dart_reader_shm(dart_state *st, int channel_idx, int peer_slot, const uint8_t *p, uint64_t now){
    dart_channel *ch=&st->channels[channel_idx];
    dart_reader_proxy *r=dart__reader_proxy_at(st,channel_idx,peer_slot);
    int reliable = (ch->qos.reliability==DART_RELIABLE);
    uint64_t base = dart_le_r64(p+DART_OFFSET_SEQNO);
    uint16_t count = dart_le_r16(p+DART_OFFSET_SHM_COUNT);
    const uint8_t *desc = p+DART_OFFSET_SHM_DESC;          /* DART_SHM_DESC_BYTES */
    if (!r->used || count==0) return;
    {   dart_reader_order ord = dart__order_arrival(st, channel_idx, peer_slot, r, base, base+count-1);
        if (ord==DART_ORDER_OLD || ord==DART_ORDER_GAP) return;   /* old/dup, or repair armed for a gap */
    }
    r->started = 1; r->assembly_active = 0;
    /* in order (base == deliver_upto). Resolve the chunk; advance + ack ONLY if the
       node delivered. A failed resolve (recycled, or a transient unattachable segment)
       leaves the gap so the reliability layer repairs it (re-sent descriptor) or skips
       it (writer HB, sample evicted). A persistently unresolvable descriptor (mis-
       configured SHM constants) would loop, so after DART_SHM_MAX_RETRY tries we skip
       it loudly instead of wedging. */
    {   int ok = st->cfg.on_shm &&
                 st->cfg.on_shm(st->cfg.user, (uint16_t)channel_idx, st->peer_ids[peer_slot], desc);
        if (ok){
            r->shm_fail = 0;
            r->deliver_upto = base + count;
            if (reliable){                  /* ack now, AFTER delivery (zero-copy invariant) */
                dart__arm(ch,r,0); r->ack_pending=1; r->ack_due_us=0; r->ack_force=1;
                dart__lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
            }
            return;
        }
        if (reliable && ++r->shm_fail >= DART_SHM_MAX_RETRY){
            dart__event(st, DART_MSG_LOST, (uint16_t)channel_idx, st->peer_ids[peer_slot],
                        base, count, "SHM descriptor unresolvable (check DART_SHM_* build constants)");
            ch->repair_stats.msgs_skipped += count;
            r->shm_fail = 0;
            r->deliver_upto = base + count;             /* give up: skip past it, ack the new edge */
            dart__arm(ch,r,0); r->ack_pending=1; r->ack_due_us=0; r->ack_force=1;
        } else if (reliable){                           /* leave the gap, NACK for a re-send */
            dart__arm(ch,r,0); r->ack_pending=1; r->ack_due_us=0;
        } else {
            r->deliver_upto = base + count;             /* best-effort: no repair, drop it */
        }
        dart__lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
    }
}
#endif


/* reader side: handle DATA */
void dart_reader_data(dart_state *st, int channel_idx, int peer_slot, const uint8_t *p,
                           uint64_t now){
    dart_channel *ch=&st->channels[channel_idx];
    dart_reader_proxy *r=dart__reader_proxy_at(st,channel_idx,peer_slot);
    int reliable = (ch->qos.reliability==DART_RELIABLE);
    int new_fragment = 0;
    uint64_t seqno, base; uint16_t frag, count, payload_len; uint32_t sample_len; const uint8_t *payload;
    if (p[0] & DART_F_SINGLE){           /* single fragment: frag/count/len implied */
        seqno=dart_le_r64(p+DART_OFFSET_SEQNO); frag=0; count=1; payload_len=dart_le_r16(p+DART_OFFSET_PAYLOAD_LEN_SINGLE); sample_len=payload_len; payload=p+DART_HEADER_DATA_SINGLE;
    } else {
        seqno=dart_le_r64(p+DART_OFFSET_SEQNO); frag=dart_le_r16(p+DART_OFFSET_FRAG); count=dart_le_r16(p+DART_OFFSET_COUNT);
        sample_len=dart_le_r32(p+DART_OFFSET_SAMPLE_LEN); payload_len=dart_le_r16(p+DART_OFFSET_PAYLOAD_LEN); payload=p+DART_HEADER_DATA_MULTI;
    }
    base = seqno - frag;

    if (!r->used){ ch->repair_stats.frags_malformed++; return; }     /* not subscribed */
    if (count==0 || frag>=count){ ch->repair_stats.frags_malformed++; return; }  /* malformed */
    switch (dart__order_arrival(st, channel_idx, peer_slot, r, base, seqno)){
        case DART_ORDER_OLD:     ch->repair_stats.frags_old++;   return;   /* already delivered/skipped */
        case DART_ORDER_GAP:     ch->repair_stats.frags_ahead++; return;   /* future frag; repair armed */
        case DART_ORDER_ADOPTED: ch->repair_stats.frags_ahead++; r->assembly_active=0; break;  /* skipped past loss */
        case DART_ORDER_INORDER: break;
    }
    r->started = 1;   /* writer engaged: position adopted */
    /* fit the reassembly buffers (dynamic grows via the hook, fixed is capped at
       max_message_bytes); "too big" skips the whole sample and reports it */
    { uint32_t bitmap_need = ((uint32_t)count + 7u) / 8u, buf_cap, bitmap_bytes; int too_big = 0;
      if (ch->dynamic){
          if (r->assembly_cap < sample_len){
              uint8_t *new_buf = (uint8_t*)st->cfg.allocator(st->cfg.user, r->assembly_buf, sample_len?sample_len:1u);
              if (!new_buf) too_big = 1; else { r->assembly_buf = new_buf; r->assembly_cap = sample_len?sample_len:1u; }
          }
          if (!too_big && r->bitmap_cap < bitmap_need){
              uint8_t *new_bitmap = (uint8_t*)st->cfg.allocator(st->cfg.user, r->frag_bitmap, bitmap_need?bitmap_need:1u);
              if (!new_bitmap) too_big = 1; else { r->frag_bitmap = new_bitmap; r->bitmap_cap = bitmap_need?bitmap_need:1u; }
          }
      } else if (sample_len > ch->qos.max_message_bytes) too_big = 1;
      if (too_big){
          dart__event(st, DART_MSG_TOO_BIG, (uint16_t)channel_idx, st->peer_ids[peer_slot],
                      0, sample_len, "message exceeds max_message_bytes");
          r->deliver_upto = base + count; r->assembly_active = 0;
          if (reliable){
              dart__arm(ch,r,0); r->ack_pending = 1; r->ack_due_us = 0; r->ack_force = 1;
              dart__lane_wake(st, (uint16_t)channel_idx, (uint32_t)peer_slot);
          }
          return;
      }
      buf_cap  = ch->dynamic ? r->assembly_cap : ch->qos.max_message_bytes;
      bitmap_bytes = ch->dynamic ? bitmap_need     : (uint32_t)((ch->max_fragments+7u)/8u);
      /* base == deliver_upto: current sample */
      if (!r->assembly_active){
          r->assembly_active=1; r->assembly_count=count; r->assembly_len=sample_len; r->assembly_low=0;
          r->nack_high=base;     /* in-flight dedup is per-message: start this one fresh */
          memset(r->frag_bitmap,0,bitmap_bytes);
      }
      if (count!=r->assembly_count) return;                  /* inconsistent, ignore */
      ch->repair_stats.frags_recv++;                             /* every accepted DATA fragment, dups included */
      new_fragment = !dart_bget(r->frag_bitmap,frag);
      if (new_fragment){
          /* reassemble at the SOURCE peer's fragment size (advertised via discovery);
             a peer staying within [MIN, MAX] keeps count <= max_fragments, so the bitmap
             can't overflow and the buf_cap guard catches any stray offset */
          uint32_t offset=(uint32_t)frag*st->peer_frag[peer_slot];
          if (offset+payload_len<=buf_cap) memcpy(r->assembly_buf+offset,payload,payload_len);
          dart_bset(r->frag_bitmap,frag);
          if (frag==r->assembly_low)                          /* extended the contiguous-received front */
              while (r->assembly_low<count && dart_bget(r->frag_bitmap,r->assembly_low)) r->assembly_low++;
      } else ch->repair_stats.frags_dup++;                        /* already held: repair overlap / waste */
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
          if (st->cfg.on_message)
              st->cfg.on_message(st->cfg.user, (uint16_t)channel_idx, st->peer_ids[peer_slot], r->assembly_buf, r->assembly_len);
          r->deliver_upto = base + count;
          r->assembly_active=0;
      }
      if (reliable){
          if (done){
              dart__arm(ch,r,0); r->ack_pending=1; r->ack_due_us=0; r->ack_force=1;
              dart__lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
          } else if (new_fragment && hole){           /* gap revealed, or repair advanced: request now */
              dart__arm(ch,r,0); r->ack_pending=1; r->ack_due_us=0;
              dart__lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
          }
      }
    }
}


void dart_reader_hb(dart_state *st, int channel_idx, int peer_slot, const uint8_t *p, uint64_t now){
    dart_channel *ch=&st->channels[channel_idx];
    dart_reader_proxy *r=dart__reader_proxy_at(st,channel_idx,peer_slot);
    uint64_t first=dart_le_r64(p+DART_OFFSET_SEQNO), last=dart_le_r64(p+DART_OFFSET_HB_LAST);
    if (!r->used) return;
    if (ch->qos.reliability!=DART_RELIABLE) return;
    /* un-started readers adopt no position from heartbeats (a one-sided flap's
       advertised first may be a dead predecessor's); the ack below carries our epoch.
       A writer raises its HB `first` to our acked_upto (the join-point trick), and our
       ACKNACK acks the contiguous-received front -- which sits INSIDE the sample we are
       still assembling. Eviction is whole-message, so a genuine floor never splits a
       sample: ignore a `first` that lands in our current partial (it is just our own
       mid-message ack echoed back), else we would skip past frags we are repairing and
       reject every resend as old. */
    if (r->started && first > r->deliver_upto &&
        (!r->assembly_active || first >= r->deliver_upto + r->assembly_count)){
        dart__event(st, DART_MSG_LOST, (uint16_t)channel_idx, st->peer_ids[peer_slot],   /* superseded before repair */
                    r->deliver_upto, first - r->deliver_upto, "message(s) lost");
        ch->repair_stats.msgs_skipped += first - r->deliver_upto;
        r->deliver_upto=first; r->assembly_active=0;
#ifdef DART_SHM
        r->shm_fail=0;          /* skipped past the stuck descriptor: fresh count */
#endif
    }
    /* hb_last is the writer's CLAIM (it may exceed what we've received). It is the only
       way to learn of tail loss -- frags past received_high that no later arrival will reveal --
       so emit lets the slow retransmit backstop chase up to it, never the fast gap path.
       The HB always owes a cumulative ack so a writer that lost ours stops re-pinging. */
    r->hb_last=last;
    dart__arm(ch,r,1); r->ack_pending=1; r->ack_due_us=0; r->ack_force=1;
    dart__lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
}


/* produce a reader ACKNACK for (channel_idx,peer_slot) if due; 0 if none.
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
size_t dart_reader_emit(dart_state *st, int channel_idx, int peer_slot, uint8_t *out, size_t cap, uint64_t now){
    dart_channel *ch=&st->channels[channel_idx];
    dart_reader_proxy *r=dart__reader_proxy_at(st,channel_idx,peer_slot);
    uint64_t first_missing, bound, top; uint16_t nbits=0; uint32_t bitmap=0;
    uint16_t alias = dart__alias_of(st, channel_idx);
    int due, holes=0, repair=0, force;
    if (!r->used || st->peer_dormant[peer_slot]) return 0;   /* dormant: don't ack a silent writer */
    if (ch->qos.reliability!=DART_RELIABLE) return 0;
    if (cap<DART_HEADER_NACK) return 0;
    if (!r->ack_pending || now<r->ack_due_us) return 0;
    r->ack_pending=0; force=r->ack_force; r->ack_force=0;

    if (!r->started)                                     /* no position yet: F_UNPOS announces our epoch */
        return dart_mk_nack(out,alias,r->deliver_upto,0,0,r->epoch,(uint8_t)DART_F_UNPOS);

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
                int missing = r->assembly_active ? !dart_bget(r->frag_bitmap,(uint32_t)(s - r->deliver_upto)) : 1;
                if (missing) bitmap |= (1u << (uint32_t)(s - first_missing));
            }
            if (bitmap){
                nbits = (uint16_t)(top - first_missing);
                repair = 1;
                ch->repair_stats.nacks_sent++;
                if (top > r->nack_high) r->nack_high = top;
                r->nack_retransmit_us = now + ch->qos.repair_delay_us;
            }
        }
    } else r->nack_high = first_missing;                   /* caught up to received: end the episode */

    /* keep the lane live while a hole remains so the backstop re-fires; an arrival that
       advances the floor re-arms us immediately (ack_due_us=0) for the next window. */
    if (holes){ r->ack_pending=1; r->ack_due_us=r->nack_retransmit_us; dart__deadline(st,r->ack_due_us); }

    /* send only to carry a repair request or a delivery/skip/HB/(re)match cumulative ack;
       a bare re-ack at an unchanged floor would be pure noise. */
    if (!repair && !force) return 0;
    return dart_mk_nack(out,alias,first_missing,nbits,bitmap,r->epoch,0);
}
