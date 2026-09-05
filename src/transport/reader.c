/* The reader path: ordering, reassembly, delivery and the ACKNACK emit. */
#include "internal.h"


int dart_transport_subscriber_progress(DartTransportState *st, uint16_t topic_index, uint32_t peer,
                         uint64_t *base_seqno, uint32_t *have, uint32_t *total){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    int peer_slot; i_DartReaderProxy *r;
    if (!topic) return 0;
    peer_slot = i_dart_peer_slot(st, peer);
    if (peer_slot < 0) return 0;
    r = i_dart_reader_proxy_at(st,topic_index,peer_slot);
    if (!r || !r->used || !r->cur.active) return 0;      /* no message mid reassembly */
    if (base_seqno) *base_seqno = r->deliver_upto;       /* the head message starts here */
    if (total)      *total      = r->cur.count;
    if (have){
        uint32_t i, c=0;
        for (i=0;i<r->cur.count;i++) if (i_dart_bit_get(r->cur.bitmap,i)) c++;
        *have = c;
    }
    return 1;
}


/* Attributes each 0 to 1 arming of the ack to its trigger. Counted before ack_pending is set. */
static void i_dart_reader_arm(i_DartTopic *topic, i_DartReaderProxy *r, int is_hb){
    if (!r->ack_pending){ if (is_hb) topic->repair_stats.arms_hb++; else topic->repair_stats.arms_data++; }
}

/* Arms an immediate ACKNACK. force owes a cumulative ack even at an unchanged floor. */
static void i_dart_reader_ack_now(DartTransportState *st, i_DartTopic *topic, i_DartReaderProxy *r,
                                  uint16_t topic_index, uint32_t peer_slot, int force, int is_hb){
    i_dart_reader_arm(topic, r, is_hb);
    r->ack_pending = 1; r->ack_due_us = 0;
    if (force) r->ack_force = 1;
    i_dart_lane_wake(st, topic_index, peer_slot);
}


/* The head sample (cur) and one sample held ahead of it (next). Nothing here moves
 * deliver_upto, only a delivery, the floor and the lapped rule do. See spec/transport.md. */

/* fit a slot's buffers to a sample, 0 = allocation failed */
static int i_dart_asm_fit(DartTransportState *st, i_DartAssembly *a, uint32_t len, uint16_t count){
    uint32_t bitmap_need = ((uint32_t)count + 7u) / 8u;
    if (a->cap < len){
        uint8_t *nb = (uint8_t*)st->cfg.allocator(st->cfg.user, a->buf, len?len:1u);
        if (!nb) return 0;
        a->buf = nb; a->cap = len?len:1u;
    }
    if (a->bitmap_cap < bitmap_need){
        uint8_t *nb = (uint8_t*)st->cfg.allocator(st->cfg.user, a->bitmap, bitmap_need?bitmap_need:1u);
        if (!nb) return 0;
        a->bitmap = nb; a->bitmap_cap = bitmap_need?bitmap_need:1u;
    }
    return 1;
}

/* is seqno s not held in slot a */
static int i_dart_asm_missing(const i_DartAssembly *a, uint64_t s){
    return !(a->active && s >= a->base && s < a->base + a->count && i_dart_bit_get(a->bitmap, (uint32_t)(s - a->base)));
}

/* deliver_upto moved: the ahead slot rotates into the head when it is exactly next, and
 * drops when the floor passed it. Eviction is whole sample, so a floor inside it cannot happen. */
static void i_dart_reader_settle(i_DartReaderProxy *r){
    if (!r->next.active) return;
    if (r->next.base < r->deliver_upto){ r->next.active = 0; return; }
    if (r->next.base == r->deliver_upto && !r->cur.active){
        i_DartAssembly t = r->cur; r->cur = r->next; r->next = t;
        r->next.active = 0;
    }
}

/* the ahead slot for a future sample, NULL when another sample occupies it */
static i_DartAssembly *i_dart_reader_ahead(i_DartReaderProxy *r, uint64_t base){
    if (!r->next.active || r->next.base == base) return &r->next;
    return NULL;
}

/* Hands up every complete sample from the head on. A reliable refusal parks the head, a
 * best effort one drops. Lane pointers are re derived after each callback. */
static void i_dart_reader_deliver(DartTransportState *st, uint16_t topic_index, uint32_t peer_slot){
    i_DartTopic *topic=&st->topics[topic_index];
    i_DartReaderProxy *r=i_dart_reader_proxy_at(st,topic_index,peer_slot);
    int reliable = (topic->qos.reliability==DART_RELIABLE);
    int delivered = 0;
    while (r->cur.active && r->cur.low == r->cur.count){
        int refused = st->cfg.on_message &&
                      st->cfg.on_message(st->cfg.user, topic_index, st->peer_ids[peer_slot],
                                         dart_bytes(r->cur.buf, r->cur.len)) != 0;
        r = i_dart_reader_proxy_at(st,topic_index,peer_slot);
        if (refused && reliable){
            /* park: no advance, no ack and no repair, so the writer's flow control
               backpressures the publisher. deliver_parked retries, a writer floor skips. */
            r->parked = 1;
            break;
        }
        r->deliver_upto = r->cur.base + r->cur.count;
        r->cur.active = 0; r->lapped = 0; delivered = 1;
        i_dart_reader_settle(r);
    }
    if (delivered && reliable)              /* ack after delivery, the reader owns copy invariant */
        i_dart_reader_ack_now(st,topic,r,topic_index,peer_slot,1,0);
}


static i_DartReaderOrder i_dart_reader_order_arrival(DartTransportState *st, int topic_index, int peer_slot,
                                             i_DartReaderProxy *r, uint64_t base, uint64_t top){
    i_DartTopic *topic=&st->topics[topic_index];
    if (base < r->deliver_upto) return DART_ORDER_OLD;
    if (top > r->received_high) r->received_high = top;          /* proof these seqnos exist */
    if (base > r->deliver_upto){
        if (topic->qos.reliability==DART_RELIABLE && r->started){   /* gap: arm a repair NACK */
            /* an armed lane keeps its deadline and is only re woken. A parked lane stays silent */
            if (!r->parked){
                if (r->ack_pending) i_dart_lane_wake(st,(uint16_t)topic_index,(uint32_t)peer_slot);
                else i_dart_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,0,0);
            }
            return DART_ORDER_GAP;
        }
        if (r->started && !topic->directed){   /* adopt, a directed skip is no loss */
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

/* An SHM-DATA covers its whole range, so there is no reassembly, only ordering. The
 * descriptor goes to on_shm and the node delivers. An SHM sample is never held ahead. */
void i_dart_reader_shm(DartTransportState *st, int topic_index, int peer_slot, const uint8_t *p, uint64_t now){
    i_DartTopic *topic=&st->topics[topic_index];
    i_DartReaderProxy *r=i_dart_reader_proxy_at(st,topic_index,peer_slot);
    int reliable = (topic->qos.reliability==DART_RELIABLE);
    uint64_t base = i_dart_le_r64(p+DART_OFFSET_SEQNO);
    uint16_t count = i_dart_le_r16(p+DART_OFFSET_SHM_COUNT);
    const uint8_t *desc = p+DART_OFFSET_SHM_DESC;
    if (!r || !r->used || count==0) return;
    if (r->parked){ topic->repair_stats.frags_ahead++; return; }   /* a held sample blocks */
    {   i_DartReaderOrder ord = i_dart_reader_order_arrival(st, topic_index, peer_slot, r, base, base+count-1);
        if (ord==DART_ORDER_OLD || ord==DART_ORDER_GAP) return;   /* old, dup, or gap armed */
    }
    r->started = 1; r->cur.active = 0;   /* a partial inline assembly of it is superseded */
    if (r->rtt_probe && r->rtt_probe_seq >= base && r->rtt_probe_seq < base + count){
        i_dart_rtt_sample(st, (uint32_t)peer_slot, now > r->rtt_probe_us ? now - r->rtt_probe_us : 0u);
        r->rtt_probe = 0;
    }
    /* Advance and ack only if the node delivered. A failed resolve leaves the gap for the
       reliability layer, and after DART_SHM_MAX_RETRY tries it is skipped loudly. */
    {   int ok = st->cfg.on_shm ?
                 st->cfg.on_shm(st->cfg.user, (uint16_t)topic_index, st->peer_ids[peer_slot], desc) : 0;
        r = i_dart_reader_proxy_at(st,topic_index,peer_slot);   /* the callback may re enter */
        if (ok > 0){
            r->shm_fail = 0; r->lapped = 0;
            r->deliver_upto = base + count;
            i_dart_reader_settle(r);
            if (reliable)                   /* ack after delivery */
                i_dart_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,1,0);
            i_dart_reader_deliver(st,(uint16_t)topic_index,(uint32_t)peer_slot);   /* held ahead */
            return;
        }
        if (ok < 0){                        /* refused downstream */
            if (!reliable){                 /* best effort drops */
                r->deliver_upto = base + count;
                i_dart_reader_settle(r);
                return;
            }
            /* park the descriptor, copied since p is the shared RX buffer. An alloc failure
               falls through to the repair path: the writer resends and we retry. */
            if (i_dart_asm_fit(st, &r->cur, DART_SHM_DESC_BYTES, 1u)){
                memcpy(r->cur.buf, desc, DART_SHM_DESC_BYTES);
                r->cur.base = base; r->cur.count = count;   /* seqnos the held sample spans */
                r->parked = 1; r->parked_shm = 1;           /* silent */
                return;
            }
        }
        if (reliable && ++r->shm_fail >= DART_SHM_MAX_RETRY){
            i_dart_transport_fire_event(st, DART_TRANSPORT_MSG_LOST, (uint16_t)topic_index, st->peer_ids[peer_slot],
                        base, count);
            topic->repair_stats.msgs_skipped += count;
            r->shm_fail = 0;
            r->deliver_upto = base + count;             /* give up: skip it and ack the new edge */
            i_dart_reader_settle(r);
            i_dart_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,1,0);
        } else if (reliable){                           /* leave the gap and NACK for a resend */
            i_dart_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,0,0);
        } else {
            r->deliver_upto = base + count;             /* best effort: no repair */
            i_dart_reader_settle(r);
            i_dart_lane_wake(st,(uint16_t)topic_index,(uint32_t)peer_slot);
        }
    }
}
#endif


void i_dart_reader_data(DartTransportState *st, int topic_index, int peer_slot, const uint8_t *p,
                           uint64_t now){
    i_DartTopic *topic=&st->topics[topic_index];
    i_DartReaderProxy *r=i_dart_reader_proxy_at(st,topic_index,peer_slot);
    i_DartAssembly *a;
    int reliable = (topic->qos.reliability==DART_RELIABLE);
    int new_frag = 0;
    uint64_t seqno, base; uint16_t frag, count, payload_len; uint32_t sample_len; const uint8_t *payload;
    if (p[0] & DART_F_SINGLE){           /* single fragment: frag, count and len are implied */
        seqno=i_dart_le_r64(p+DART_OFFSET_SEQNO); frag=0; count=1; payload_len=i_dart_le_r16(p+DART_OFFSET_PAYLOAD_LEN_SINGLE); sample_len=payload_len; payload=p+DART_HEADER_DATA_SINGLE;
    } else {
        seqno=i_dart_le_r64(p+DART_OFFSET_SEQNO); frag=i_dart_le_r16(p+DART_OFFSET_FRAG); count=i_dart_le_r16(p+DART_OFFSET_COUNT);
        sample_len=i_dart_le_r32(p+DART_OFFSET_SAMPLE_LEN); payload_len=i_dart_le_r16(p+DART_OFFSET_PAYLOAD_LEN); payload=p+DART_HEADER_DATA_MULTI;
    }
    base = seqno - frag;

    if (!r || !r->used){ topic->repair_stats.frags_malformed++; return; }     /* not subscribed */
    if (count==0 || frag>=count){ topic->repair_stats.frags_malformed++; return; }  /* malformed */
    switch (i_dart_reader_order_arrival(st, topic_index, peer_slot, r, base, seqno)){
        case DART_ORDER_OLD:     topic->repair_stats.frags_old++;   return;   /* already seen */
        case DART_ORDER_GAP:   /* a future sample: hold one ahead */
            a = i_dart_reader_ahead(r, base);
            if (!a){ topic->repair_stats.frags_ahead++; return; }   /* hold taken */
            break;
        case DART_ORDER_ADOPTED: topic->repair_stats.frags_ahead++; r->cur.active=0; a=&r->cur; break;
        case DART_ORDER_INORDER:
            if (r->parked){ topic->repair_stats.frags_dup++; return; }   /* the held sample */
            a=&r->cur; break;
    }
    r->started = 1;   /* the writer is engaged */
    /* too big means the allocation failed: the head is skipped and reported, a sample
       held ahead is simply not held and comes back in order later */
    if (!i_dart_asm_fit(st, a, sample_len, count)){
        if (a == &r->next){ topic->repair_stats.frags_ahead++; return; }
        i_dart_transport_fire_event(st, DART_TRANSPORT_MSG_TOO_BIG, (uint16_t)topic_index, st->peer_ids[peer_slot],
                    0, sample_len);
        r->deliver_upto = base + count; r->cur.active = 0;
        i_dart_reader_settle(r);
        if (reliable) i_dart_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,1,0);
        i_dart_reader_deliver(st,(uint16_t)topic_index,(uint32_t)peer_slot);
        return;
    }
    if (!a->active){
        /* nack_high is not reset: a whole message request may be in flight and a fresh
           cursor would re ask all of it. The refill asks from the higher of the two. */
        a->active=1; a->base=base; a->count=count; a->len=sample_len; a->low=0;
        memset(a->bitmap,0,((uint32_t)count + 7u) / 8u);
    }
    if (count!=a->count) return;                             /* inconsistent, ignore */
    topic->repair_stats.frags_recv++;   /* every accepted fragment, dups included */
    new_frag = !i_dart_bit_get(a->bitmap,frag);
    if (new_frag){
        /* reassemble at the source peer's fragment size, the cap guard catches a stray offset */
        uint32_t offset=(uint32_t)frag*st->peer_frag[peer_slot];
        if (offset+payload_len<=a->cap) memcpy(a->buf+offset,payload,payload_len);
        i_dart_bit_set(a->bitmap,frag);
        if (frag==a->low)                                    /* extended the contiguous front */
            while (a->low<count && i_dart_bit_get(a->bitmap,a->low)) a->low++;
        if (r->rtt_probe && seqno == r->rtt_probe_seq){      /* the resend our probe timed */
            i_dart_rtt_sample(st, (uint32_t)peer_slot, now > r->rtt_probe_us ? now - r->rtt_probe_us : 0u);
            r->rtt_probe = 0;
        }
    } else topic->repair_stats.frags_dup++;                  /* already held */
    if (a != &r->cur) return;    /* held ahead: it delivers when the head does */
    /* Deliver a complete head. A partial head re arms only when this frag opened a real
       gap below received_high, so a healthy in order fill owes nothing. */
    if (a->low == count) i_dart_reader_deliver(st,(uint16_t)topic_index,(uint32_t)peer_slot);
    else if (reliable && new_frag && r->deliver_upto + a->low <= r->received_high)
        i_dart_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,0,0);
}


void i_dart_reader_hb(DartTransportState *st, int topic_index, int peer_slot, const uint8_t *p, uint64_t now){
    i_DartTopic *topic=&st->topics[topic_index];
    i_DartReaderProxy *r=i_dart_reader_proxy_at(st,topic_index,peer_slot);
    uint64_t first=i_dart_le_r64(p+DART_OFFSET_SEQNO), last=i_dart_le_r64(p+DART_OFFSET_HB_LAST);
    (void)now;
    if (!r || !r->used) return;
    if (topic->qos.reliability!=DART_RELIABLE) return;
    /* An unstarted reader adopts no position from a heartbeat. A first inside the sample
       being assembled or parked is our own mid message ack echoed back, so it is ignored. */
    if (r->started && first > r->deliver_upto &&
        (!(r->cur.active || r->parked) || first >= r->deliver_upto + r->cur.count)){
        uint64_t to = first;
        if (!topic->directed){   /* directed: not real loss, skip silently */
            /* lapped: the floor passed a sample being fetched twice with no delivery between,
               so rejoin at the writer's head. A parked skip is the consumer's, not loss. */
            if (!r->parked){
                if (r->lapped && last + 1 > to) to = last + 1;
                r->lapped = 1;
            }
            i_dart_transport_fire_event(st, DART_TRANSPORT_MSG_LOST, (uint16_t)topic_index, st->peer_ids[peer_slot],
                        r->deliver_upto, to - r->deliver_upto);
            topic->repair_stats.msgs_skipped += to - r->deliver_upto;
        }
        r->deliver_upto=to; r->cur.active=0; r->rtt_probe=0;
        r->parked=0;            /* the writer moved past the held sample */
#ifdef DART_SHM
        r->parked_shm=0;
        r->shm_fail=0;          /* a fresh count */
#endif
        /* the floor may have landed on the sample held ahead: it is the head now */
        i_dart_reader_settle(r);
        i_dart_reader_deliver(st,(uint16_t)topic_index,(uint32_t)peer_slot);
        r=i_dart_reader_proxy_at(st,topic_index,peer_slot);
    }
    if (r->parked) return;      /* still parked: silent, the writer's flow control backpressures */
    /* hb_last is the writer's claim and only the slow backstop chases it, for tail loss.
       The HB always owes a cumulative ack so a writer that lost ours stops pinging. */
    r->hb_last=last;
    i_dart_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,1,1);
}


/* The ACKNACK for a lane if due, 0 if none. Repair is gap triggered, bounded by what was
 * received and deduped in flight. See spec/transport.md. */
size_t i_dart_reader_emit(DartTransportState *st, int topic_index, int peer_slot, uint8_t *out, size_t cap, uint64_t now){
    i_DartTopic *topic=&st->topics[topic_index];
    i_DartReaderProxy *r=i_dart_reader_proxy_at(st,topic_index,peer_slot);
    uint64_t first_missing, bound, top; uint16_t nbits=0; uint32_t bitmap=0;
    uint16_t index = i_dart_wire_index_of(st, topic_index);
    int due, holes=0, repair=0, force;
    if (!r || !r->used || st->peer_dormant[peer_slot]) return 0;   /* unmatched or dormant */
    if (topic->qos.reliability!=DART_RELIABLE) return 0;
    if (cap<DART_HEADER_NACK) return 0;
    if (!r->ack_pending || now<r->ack_due_us) return 0;
    r->ack_pending=0; force=r->ack_force; r->ack_force=0;
    if (r->parked) return 0;    /* parked: silent, and the lane stays unscheduled */

    if (!r->started)   /* no position yet: F_UNPOS announces our epoch */
        return i_dart_wire_mk_nack(out,index,r->deliver_upto,0,0,r->epoch,(uint8_t)DART_F_UNPOS);

    /* the cumulative ack point and repair window base: our contiguous front */
    first_missing = r->cur.active ? r->deliver_upto + r->cur.low : r->deliver_upto;

    /* only the slow backstop may reach the writer's claim, so tail loss still repairs */
    due   = (now >= r->nack_retransmit_us);
    bound = r->received_high;
    if (due && r->hb_last > bound) bound = r->hb_last;

    if (first_missing <= bound){                         /* a hole sits below something we heard */
        holes = 1;
        top = first_missing + DART_NACK_WINDOW;          /* one window per ACKNACK */
        if (top > bound + 1) top = bound + 1;
        {   /* ask only for what we can hold: the head, and the held sample when it follows
               the head directly or the head is still unknown */
            uint64_t held_end = 0;
            if (r->cur.active) held_end = r->cur.base + r->cur.count;
            if (r->next.active && (!r->cur.active || r->next.base == held_end))
                held_end = r->next.base + r->next.count;
            if (held_end && top > held_end) top = held_end;
        }
        {   /* in flight dedup is valid only while assembling this message. With no head, and
               on the slow backstop, everything is asked from the floor. */
            uint64_t from = (due || !r->cur.active) ? first_missing
                : (r->nack_high > first_missing ? r->nack_high : first_missing);
            uint64_t s, probe = 0, asked_high = r->nack_high;   /* at or past it = never asked */
            int have_probe = 0;
            for (s=from; s<top; s++){
                if (i_dart_asm_missing(&r->cur, s) && i_dart_asm_missing(&r->next, s)){
                    bitmap |= (1u << (uint32_t)(s - first_missing));
                    if (!have_probe && s >= asked_high){ probe = s; have_probe = 1; }
                }
            }
            if (bitmap){
                /* the backstop: the peer's measured round trip unless the qos pins it */
                uint32_t rto = topic->qos.repair_delay_us ? topic->qos.repair_delay_us
                             : i_dart_rtt_rto(st, (uint32_t)peer_slot, DART_QOS_DEF_REPAIR_US);
                nbits = (uint16_t)(top - first_missing);
                repair = 1;
                topic->repair_stats.nacks_sent++;
                if (top > r->nack_high) r->nack_high = top;
                r->nack_retransmit_us = now + rto;
                /* the probe times the first ask of one seqno. A re ask or a passed floor
                   disarms it, and the lowest first ask of this request arms the next. */
                if (r->rtt_probe && (r->rtt_probe_seq < first_missing ||
                    (r->rtt_probe_seq < top && ((bitmap >> (uint32_t)(r->rtt_probe_seq - first_missing)) & 1u))))
                    r->rtt_probe = 0;
                if (!r->rtt_probe && have_probe){ r->rtt_probe = 1; r->rtt_probe_seq = probe; r->rtt_probe_us = now; }
            }
        }
    } else r->nack_high = first_missing;                   /* caught up: end the episode */

    /* keep the lane live while a hole remains so the backstop re fires */
    if (holes){ r->ack_pending=1; r->ack_due_us=r->nack_retransmit_us; i_dart_transport_arm_deadline(st,r->ack_due_us); }

    /* only a repair request or a forced cumulative ack is worth a datagram */
    if (!repair && !force) return 0;
    return i_dart_wire_mk_nack(out,index,first_missing,nbits,bitmap,r->epoch,0);
}


/* Retries every parked lane of a topic. The callbacks may re enter the transport, so lane
 * pointers are re derived after each call. */
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
            i_DartReaderProxy *r = &st->lanes[li].r;
            int accepted;
#ifdef DART_SHM
            if (r->parked_shm){
                int ok = st->cfg.on_shm ?
                         st->cfg.on_shm(st->cfg.user, topic_index, peer_id, r->cur.buf) : 0;
                r = &st->lanes[li].r;                  /* the callback may re enter */
                if (ok == 0){
                    /* the chunk is gone: un park, leave the gap, and let the repair path re
                       fetch or skip it */
                    r->parked = 0; r->parked_shm = 0; r->cur.active = 0;
                    i_dart_reader_ack_now(st,topic,r,topic_index,peer_slot,0,0);
                    li = next; continue;
                }
                accepted = (ok > 0);
                if (accepted){
                    r->deliver_upto = r->cur.base + r->cur.count;
                    r->cur.active = 0; r->parked = 0; r->parked_shm = 0; r->shm_fail = 0; r->lapped = 0;
                    i_dart_reader_settle(r);
                    if (topic->qos.reliability==DART_RELIABLE)    /* ack after delivery */
                        i_dart_reader_ack_now(st,topic,r,topic_index,peer_slot,1,0);
                    i_dart_reader_deliver(st,topic_index,peer_slot);
                }
            } else
#endif
            {
                /* un park and run the ordinary delivery, which re parks on a refusal */
                r->parked = 0;
                i_dart_reader_deliver(st,topic_index,peer_slot);
                accepted = !st->lanes[li].r.parked;
            }
            if (!accepted) still++;
        }
        li = next;
    }
    return still;
}
