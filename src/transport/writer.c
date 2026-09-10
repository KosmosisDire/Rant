/* The writer path: history, send, the per lane emit and ACKNACK handling. */
#include "internal.h"


/* newest first, so pushing new data is O(1) */
static i_DartWriterSample *i_dart_sample_find(i_DartTopic *topic, uint64_t seqno){
    uint16_t depth = topic->qos.keep_last, k;
    uint16_t i = topic->history_head;
    for (k=0;k<depth;k++){
        i_DartWriterSample *s;
        i = (uint16_t)(i ? i-1 : depth-1);
        s = &topic->history[i];
        if (!s->valid) break;                  /* the unwritten tail */
        if (seqno >= s->base)
            return (seqno < s->base + s->count) ? s : NULL;
    }
    return NULL;
}


/* Seals the filled head slot into history at the next seqno, stamped with its destination. */
static void i_dart_writer_seal(DartTransportState *st, i_DartTopic *topic, size_t len,
                               uint32_t dest_slot, uint64_t *base_out, uint16_t *count_out){
    uint16_t depth = topic->qos.keep_last;
    uint16_t count = (uint16_t)((len + st->frag - 1) / st->frag);
    i_DartWriterSample *slot = &topic->history[topic->history_head];
    if (count==0) count=1;
    slot->valid=1; slot->base=topic->next_seqno; slot->count=count; slot->len=(uint32_t)len;
    slot->dest_slot = dest_slot;
    *base_out = slot->base; *count_out = count;
    topic->history_head = (uint16_t)((topic->history_head+1) % depth);
    topic->next_seqno += count;
    /* oldest cached: where the head points once wrapped, else slot 0 */
    topic->first_seqno = topic->history[topic->history_head].valid ? topic->history[topic->history_head].base
                                                    : topic->history[0].base;
    topic->have_first  = 1;
}


/* Directed topics: steps a lane over samples addressed to other peers. acked_upto only
 * steps contiguously, so a lane is never marked past a sample it is still owed. */
static void i_dart_writer_lane_advance(i_DartTopic *topic, i_DartWriterProxy *w, uint32_t peer_slot){
    i_DartWriterSample *s;
    if (!topic->directed) return;
    while ((s = i_dart_sample_find(topic, w->acked_upto)) != NULL
           && s->dest_slot != DART__DEST_ALL && s->dest_slot != peer_slot){
        w->acked_upto = s->base + s->count;
        w->skip_hb = 1;
    }
    if (w->sent_upto < w->acked_upto) w->sent_upto = w->acked_upto;
    while (w->sent_upto < topic->next_seqno
           && (s = i_dart_sample_find(topic, w->sent_upto)) != NULL
           && s->dest_slot != DART__DEST_ALL && s->dest_slot != peer_slot
           && w->sent_upto == s->base)
        w->sent_upto = s->base + s->count;
}


/* Seals and wakes the lanes that carry the sample, O(matches). A directed sample wakes
 * only its lane and every other lane derives its skip. */
static void i_dart_writer_commit(DartTransportState *st, uint16_t topic_index, size_t len,
                                 uint32_t dest_slot){
    i_DartTopic *topic = &st->topics[topic_index];
    int reliable = (topic->qos.reliability==DART_RELIABLE);
    uint64_t base; uint16_t count;
    uint32_t li;
    i_dart_writer_seal(st, topic, len, dest_slot, &base, &count);
    for (li=topic->lane_head; li!=DART__NIL; li=st->lanes[li].topic_next){
        i_DartLane *l = &st->lanes[li];
        if (!l->w.used || st->peer_dormant[l->peer_slot]) continue;
        if (dest_slot == DART__DEST_ALL || l->peer_slot == dest_slot){ i_dart_lane_enqueue(st, li); continue; }
        i_dart_writer_lane_advance(topic, &l->w, l->peer_slot);
        if (reliable && l->w.reader_reliable && l->w.skip_hb) i_dart_lane_enqueue(st, li);
    }
}


/* The 65535 fragment cap, checked before the no subscriber early out. */
static int i_dart_writer_too_big(DartTransportState *st, i_DartTopic *topic, size_t len){
    (void)topic;
    return len > 65535u*(uint32_t)st->frag;
}

/* Stores ts, hdr and data into the head slot. The source stamp is taken here, once per
 * message, so a repair resend, a replay and the SHM chunk all carry the original. */
static int i_dart_writer_store(DartTransportState *st, i_DartTopic *topic,
                               DartBytes hdr, DartBytes data, uint64_t capture_us,
                               size_t *len_out){
    uint32_t ts = i_dart_topic_ts_bytes(topic);
    uint32_t cap_b = (ts && capture_us) ? DART_CAPTURE_BYTES : 0u;
    size_t len = (size_t)ts + cap_b + hdr.len + data.len;
    if (i_dart_writer_too_big(st, topic, len)) return DART_ERR_TOO_BIG;
    {   i_DartWriterSample *slot = &topic->history[topic->history_head];
        size_t need = len ? len : 1u;
        if ((size_t)slot->cap < need){                    /* grow the slot to fit */
            uint8_t *new_buf = (uint8_t*)st->cfg.allocator(st->cfg.user, slot->buf, need);
            if (!new_buf) return DART_ERR_OOM;
            slot->buf = new_buf; slot->cap = (uint32_t)need;
        }
    }
    {   uint8_t *dst = topic->history[topic->history_head].buf;    /* gather: ts, hdr, payload */
        if (ts){
            uint64_t w = st->cfg.source_time ? st->cfg.source_time(st->cfg.user) : 0u;
            w &= DART_STAMP_MASK;
            if (cap_b) w |= DART_STAMP_CAPTURE;
            i_dart_le_w64(dst, w);
            if (cap_b) i_dart_le_w64(dst + ts, capture_us);
        }
        if (hdr.len)  memcpy(dst + ts + cap_b, hdr.data, hdr.len);
        if (data.len) memcpy(dst + ts + cap_b + hdr.len, data.data, data.len);
    }
#ifdef DART_SHM
    topic->history[topic->history_head].shm = 0;   /* an inline send: not SHM backed */
#endif
    *len_out = len;
    return DART_OK;
}


/* The one send: prologue, store and commit. dest_slot is stamped onto the sample. */
static int i_dart_writer_send(DartTransportState *st, uint16_t topic_index,
                              DartBytes hdr, DartBytes data, uint64_t capture_us,
                              uint32_t dest_slot){
    i_DartTopic *topic; size_t len; int r;
    topic = i_dart_topic_at(st, topic_index, NULL);
    if (!topic) return DART_ERR_NO_TOPIC;
    /* the stamps are ordinary payload for every size rule */
    if (i_dart_writer_too_big(st, topic,
            i_dart_topic_stamp_bytes(topic, capture_us) + hdr.len + data.len))
        return DART_ERR_TOO_BIG;
    if (topic->role == DART_SUB_ONLY || topic->role == DART_INACTIVE) return DART_ERR_ROLE;
    /* nobody subscribes and nothing durable to keep: skip the grow, the copy and the commit */
    if (topic->matched_writers == 0 && !i_dart_topic_retains_history(topic)) return DART_OK;
    r = i_dart_writer_store(st, topic, hdr, data, capture_us, &len);
    if (r != DART_OK) return r;
    i_dart_writer_commit(st, topic_index, len, dest_slot);
    return DART_OK;
}


int dart_transport_send(DartTransportState *st, uint16_t topic_index, DartBytes data, uint64_t now){
    DartBytes nohdr; nohdr.data=NULL; nohdr.len=0;
    return dart_transport_send_hdr(st, topic_index, nohdr, data, 0, now);
}


int dart_transport_send_hdr(DartTransportState *st, uint16_t topic_index, DartBytes hdr,
                            DartBytes data, uint64_t capture_us, uint64_t now){
    (void)now;
    return i_dart_writer_send(st, topic_index, hdr, data, capture_us, DART__DEST_ALL);
}


int dart_transport_send_to(DartTransportState *st, uint16_t topic_index, uint32_t to_peer,
                           DartBytes hdr, DartBytes data, uint64_t capture_us, uint64_t now){
    int peer_slot = i_dart_peer_slot(st, to_peer);   /* unknown: sent to nobody, seqno consumed */
    (void)now;
    return i_dart_writer_send(st, topic_index, hdr, data, capture_us,
                              peer_slot < 0 ? DART__DEST_NONE : (uint32_t)peer_slot);
}

#ifdef DART_SHM

/* The chunk is the whole wire sample, the caller wrote the stamp and any header into it,
 * so nothing is gathered or copied here. */
int dart_transport_send_shm(DartTransportState *st, uint16_t topic_index, DartBytes chunk,
                  const uint8_t *desc, uint64_t now){
    i_DartTopic *topic; i_DartWriterSample *slot; size_t len = chunk.len;
    (void)now;
    topic = i_dart_topic_at(st, topic_index, NULL);
    if (!topic) return DART_ERR_NO_TOPIC;
    if (len > 65535u*(uint32_t)st->frag) return DART_ERR_TOO_BIG;      /* the fragment count cap */
    if (topic->role == DART_SUB_ONLY || topic->role == DART_INACTIVE) return DART_ERR_ROLE;
    slot = &topic->history[topic->history_head];
    slot->shm = 1;
    slot->shm_buf = chunk.data;
    memcpy(slot->desc, desc, DART_SHM_DESC_BYTES);
    i_dart_writer_commit(st, topic_index, len, DART__DEST_ALL);
    return DART_OK;
}
#endif


int dart_transport_send_would_evict(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    i_DartWriterSample *slot; uint32_t li;
    if (!topic || topic->qos.reliability != DART_RELIABLE) return 0;
    slot = &topic->history[topic->history_head];        /* the slot the next send overwrites */
    if (!slot->valid) return 0;
    for (li=topic->lane_head; li!=DART__NIL; li=st->lanes[li].topic_next){
        i_DartLane *l=&st->lanes[li];
        if (l->w.used && l->w.reader_reliable && !st->peer_dormant[l->peer_slot]
            && l->w.acked_upto < slot->base + slot->count) return 1;
    }
    return 0;
}


int dart_transport_send_would_evict_unsent(DartTransportState *st, uint16_t topic_index,
                                            uint64_t *evict_base, uint32_t *evict_count){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    i_DartWriterSample *slot; uint32_t li;
    if (!topic) return 0;
    slot = &topic->history[topic->history_head];        /* the slot the next send overwrites */
    if (!slot->valid) return 0;
    for (li=topic->lane_head; li!=DART__NIL; li=st->lanes[li].topic_next){
        i_DartLane *l=&st->lanes[li];
        if (l->w.used && !st->peer_dormant[l->peer_slot] && l->w.sent_upto < slot->base + slot->count){
            if (evict_base)  *evict_base  = slot->base;
            if (evict_count) *evict_count = slot->count;
            return 1;
        }
    }
    return 0;
}


int dart_transport_send_drained(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    uint32_t li;
    if (!topic || topic->qos.reliability != DART_RELIABLE) return 1;  /* no acks to await */
    for (li=topic->lane_head; li!=DART__NIL; li=st->lanes[li].topic_next){
        i_DartLane *l=&st->lanes[li];
        if (l->w.used && l->w.reader_reliable && !st->peer_dormant[l->peer_slot]
            && l->w.acked_upto < topic->next_seqno) return 0;  /* a reliable reader is behind */
    }
    return 1;
}


int dart_transport_publisher_match_count(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    return topic ? (int)topic->matched_writers : 0;   /* cached at match time, O(1) */
}


/* O(1) from the cached count. */
int dart_transport_subscriber_match_count(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    return topic ? (int)topic->matched_readers : 0;
}


/* Dormant excluded. O(matches), for liveness decisions, not the send path. */
int dart_transport_publisher_live_matches(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    uint32_t li; int cnt = 0;
    if (!topic) return 0;
    for (li=topic->lane_head; li!=DART__NIL; li=st->lanes[li].topic_next){
        i_DartLane *l=&st->lanes[li];
        if (l->w.used && !st->peer_dormant[l->peer_slot]) cnt++;
    }
    return cnt;
}


int dart_transport_publisher_peer_matched(DartTransportState *st, uint16_t topic_index,
                                          uint32_t peer_id){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    uint32_t li; int slot;
    if (!topic) return 0;
    slot = i_dart_peer_slot(st, peer_id);
    if (slot < 0) return 0;
    for (li=topic->lane_head; li!=DART__NIL; li=st->lanes[li].topic_next){
        i_DartLane *l=&st->lanes[li];
        if (l->w.used && l->peer_slot == (uint32_t)slot) return 1;
    }
    return 0;
}


/* The chain is newest first, so the last live hit is the oldest. */
uint32_t dart_transport_publisher_oldest_match(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    uint32_t li, id = 0;
    if (!topic) return 0;
    for (li=topic->lane_head; li!=DART__NIL; li=st->lanes[li].topic_next){
        i_DartLane *l=&st->lanes[li];
        if (l->w.used && !st->peer_dormant[l->peer_slot]) id = st->peer_ids[l->peer_slot];
    }
    return id;
}


int dart_transport_repair_pending(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    uint32_t li; int cnt = 0;
    if (!topic) return 0;
    for (li=topic->lane_head; li!=DART__NIL; li=st->lanes[li].topic_next){
        i_DartLane *l=&st->lanes[li];
        if (l->w.used && l->w.has_nack) cnt++;
    }
    return cnt;   /* writer lanes with a NACK to service */
}

#ifdef DART_SHM

/* One remote reader forces inline UDP for the whole message. */
int dart_transport_publisher_shm_eligible(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    uint32_t li; int any=0;
    if (!topic) return 0;
    for (li=topic->lane_head; li!=DART__NIL; li=st->lanes[li].topic_next){
        i_DartLane *l=&st->lanes[li];
        if (!l->w.used || st->peer_dormant[l->peer_slot]) continue;
        if (!st->peer_shm[l->peer_slot]) return 0;
        any = 1;
    }
    return any;
}
#endif

#ifdef DART_SHM
uint16_t dart_transport_topic_hist_head(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    return topic ? topic->history_head : 0;
}
#endif

/* An HB advertising the window. Its first is the floor, which replaces GAP: the reader
 * skips to it. Resets the idle timer so it does not double send. */
static size_t i_dart_writer_hb(DartTransportState *st, i_DartTopic *topic, i_DartWriterProxy *w, uint16_t index,
                             uint8_t *out, size_t cap, uint64_t now){
    uint64_t first = topic->have_first ? topic->first_seqno : 0;
    if (cap < DART_HEADER_HB) return 0;
    if (w->acked_upto > first) first = w->acked_upto;   /* a fresh reader adopts the join point */
    w->hb_next_us = now + topic->qos.heartbeat_us;
    i_dart_transport_arm_deadline(st, w->hb_next_us);   /* wake to send the next idle HB */
    w->hb_count++;
    return i_dart_wire_mk_hb(out, index, first, topic->next_seqno-1, w->hb_count);
}


/* The queue just drained: the next HB comes within the tail window so a lost final message
 * repairs fast. The reader's immediate ack normally suppresses it, so healthy traffic is free. */
static void i_dart_writer_arm_tail(DartTransportState *st, i_DartWriterProxy *w, uint32_t peer_slot, uint64_t now){
    uint64_t tail = now + i_dart_rtt_rto(st, peer_slot, DART_HB_TAIL_US);
    if (w->hb_next_us <= now || w->hb_next_us > tail) w->hb_next_us = tail;
    i_dart_transport_arm_deadline(st, w->hb_next_us);
}


void i_dart_writer_nack(DartTransportState *st, int topic_index, int peer_slot, const uint8_t *p, uint64_t now){
    i_DartTopic *topic=&st->topics[topic_index];
    i_DartWriterProxy *w=i_dart_writer_proxy_at(st,topic_index,peer_slot);
    uint64_t base=i_dart_le_r64(p+DART_OFFSET_SEQNO); uint16_t nbits=i_dart_le_r16(p+DART_OFFSET_NACK_NBITS); uint32_t bitmap=i_dart_le_r32(p+DART_OFFSET_NACK_BITMAP);
    uint32_t epoch=i_dart_le_r32(p+DART_OFFSET_NACK_EPOCH); uint8_t flags=p[0];
    if (!w || !w->used) return;
    if (w->reader_epoch != epoch){
        if (w->reader_epoch){
            /* The reader re incarnated while our lane survived. Rejoin at the older of the
               fresh join point and the acked floor, so the successor gets the unacked window. */
            uint64_t join = i_dart_topic_unicast_join_seqno(topic);
            w->sent_upto  = w->acked_upto < join ? w->acked_upto : join;
            w->acked_upto = w->sent_upto;
            w->has_nack   = 0; w->rtt_probe = 0;
            w->hb_next_us = 0;
            i_dart_lane_wake(st,(uint16_t)topic_index,(uint32_t)peer_slot);
            w->reader_epoch = epoch;
            return;
        }
        w->reader_epoch = epoch;      /* first contact: the lane is already fresh */
    }
    if (flags & DART_F_UNPOS){
        /* an unpositioned reader never NACKs: re push from the unacked edge */
        if (w->acked_upto < w->sent_upto){
            w->sent_upto = w->acked_upto;
            i_dart_lane_wake(st,(uint16_t)topic_index,(uint32_t)peer_slot);
        }
        w->rtt_probe = 0;
        return;                    /* no position information to apply */
    }
    /* the cumulative ack reached the armed probe. A repair request in the same ACKNACK
       disarms it, since the acks after a repair are ambiguous (Karn) */
    if (w->rtt_probe && base >= w->rtt_probe_seq){
        i_dart_rtt_sample(st, (uint32_t)peer_slot, now > w->rtt_probe_us ? now - w->rtt_probe_us : 0u);
        w->rtt_probe = 0;
    }
    if (nbits>0 && bitmap!=0) w->rtt_probe = 0;
    if (base > w->acked_upto) w->acked_upto=base;
    /* directed: step over foreign samples the ack made contiguous and owe the floor HB */
    i_dart_writer_lane_advance(topic, w, (uint32_t)peer_slot);
    if (w->skip_hb) i_dart_lane_wake(st,(uint16_t)topic_index,(uint32_t)peer_slot);
    /* Merge, never overwrite: drop what the new base acks, then OR the new bits in.
       Overwriting stalled every crossing refill on the backstop. See spec/transport.md. */
    if (w->has_nack && base > w->nack_base){
        uint64_t d = base - w->nack_base;
        w->nack_bits = d < DART_NACK_WINDOW ? w->nack_bits >> d : 0u;
        w->nack_base = base;
        if (!w->nack_bits) w->has_nack = 0;
    }
    if (nbits>0 && bitmap!=0){
        topic->repair_stats.nacks_recv++;   /* a repair request, not a bare ack */
        if (w->has_nack){                                           /* nack_base >= base here */
            uint64_t d = w->nack_base - base;
            w->nack_bits |= d < DART_NACK_WINDOW ? bitmap >> d : 0u;
        } else { w->has_nack=1; w->nack_base=base; w->nack_bits=bitmap; }
        i_dart_lane_wake(st,(uint16_t)topic_index,(uint32_t)peer_slot);
    }
}


/* One writer submessage if due and it fits cap, 0 if none. On no fit the state is
 * untouched, so the same submessage is produced next time. */
size_t i_dart_writer_emit(DartTransportState *st, int topic_index, int peer_slot, uint8_t *out, size_t cap, uint64_t now){
    i_DartTopic *topic=&st->topics[topic_index];
    i_DartWriterProxy *w=i_dart_writer_proxy_at(st,topic_index,peer_slot);
    int reliable=(topic->qos.reliability==DART_RELIABLE);
    uint16_t index = i_dart_wire_index_of(st, topic_index);
    if (!w || !w->used || st->peer_dormant[peer_slot]) return 0;   /* unmatched or dormant */

    /* a fire and forget lane stamps a private wire seqno. Reliable and directed lanes keep
       the global line, since repair and the directed skip HB index history by it */
    int per_lane = (!w->reader_reliable && !topic->directed);

    /* directed: derive owed skips first, which also catches up a lane that was dormant */
    i_dart_writer_lane_advance(topic, w, (uint32_t)peer_slot);

    /* 1. repair */
    if (reliable && w->has_nack){
        uint32_t i;
        for (i=0;i<DART_NACK_WINDOW;i++){
            if (w->nack_bits & (1u<<i)){
                uint64_t seqno=w->nack_base+i;
                i_DartWriterSample *s;
                if (seqno>=topic->next_seqno){                 /* nothing there */
                    w->nack_bits &= ~(1u<<i);
                    if (w->nack_bits==0) w->has_nack=0;
                    continue;
                }
                s=i_dart_sample_find(topic,seqno);
                if (s && topic->directed && s->dest_slot != DART__DEST_ALL
                      && s->dest_slot != (uint32_t)peer_slot){
                    /* addressed to another lane: never re serve it, a leaked directed sample
                       would reach the wrong pending call. Clear its bits and owe the floor HB. */
                    uint32_t j;
                    for (j=0;j<DART_NACK_WINDOW;j++){
                        uint64_t sq=w->nack_base+j;
                        if (sq>=s->base && sq<s->base+s->count) w->nack_bits &= ~(1u<<j);
                    }
                    if (w->nack_bits==0) w->has_nack=0;
                    i_dart_writer_lane_advance(topic,w,(uint32_t)peer_slot);
                    w->skip_hb = 1;
                    continue;
                }
                if (s){
#ifdef DART_SHM
                    if (st->peer_shm[peer_slot] && s->shm){   /* resend as one SHM-DATA */
                        uint32_t j;
                        if (cap < DART_SHM_DATA_BYTES) return 0;
                        for (j=0;j<DART_NACK_WINDOW;j++){
                            uint64_t sq=w->nack_base+j;
                            if (sq>=s->base && sq<s->base+s->count) w->nack_bits &= ~(1u<<j);
                        }
                        if (w->nack_bits==0) w->has_nack=0;
                        return i_dart_wire_mk_shm(out,index,s->base,s->count,s->desc);
                    }
#endif
                    {
                    uint16_t frag_index=(uint16_t)(seqno - s->base);
                    uint32_t offset=(uint32_t)frag_index*st->frag;
                    uint16_t payload_len=(uint16_t)((s->len-offset)<st->frag?(s->len-offset):st->frag);
                    if (cap < (size_t)(s->count==1?DART_HEADER_DATA_SINGLE:DART_HEADER_DATA_MULTI)+(size_t)payload_len) return 0;
                    w->nack_bits &= ~(1u<<i);
                    if (w->nack_bits==0) w->has_nack=0;
                    topic->repair_stats.frags_sent++; topic->repair_stats.frags_resent++;
                    return i_dart_wire_mk_data(out,index,seqno,s,frag_index,i_dart_sample_buf(s)+offset,payload_len);
                    }
                } else {
                    /* superseded: skip the reader past the dropped region with an HB whose
                       first is our floor, and keep still cached seqnos for later repair */
                    uint64_t floor = (topic->have_first?topic->first_seqno:topic->next_seqno);
                    uint32_t j;
                    if (cap < DART_HEADER_HB) return 0;
                    for (j=0;j<DART_NACK_WINDOW;j++)
                        if (w->nack_base+j < floor) w->nack_bits &= ~(1u<<j);
                    if (w->nack_bits==0) w->has_nack=0;
                    return i_dart_writer_hb(st,topic,w,index,out,cap,now);
                }
            }
        }
        w->has_nack=0;
    }

    /* the directed floor HB before any new data, so the reader's floor moves past the
       foreign seqnos first and the data that follows arrives in order */
    if (reliable && w->reader_reliable && w->skip_hb){
        size_t hb = i_dart_writer_hb(st,topic,w,index,out,cap,now);
        if (!hb) return 0;        /* did not fit: retry next pass, the flag stays */
        w->skip_hb = 0;
        return hb;
    }

    /* 2. push new data */
    if (w->sent_upto < topic->next_seqno){
        uint64_t seqno=w->sent_upto;
        i_DartWriterSample *s=i_dart_sample_find(topic,seqno);
        /* Rate throttle at a sample boundary only: hold until the tick, then decimate to the
           newest sample. wire_skip absorbs the skipped ones. See spec/transport.md. */
        if (per_lane && w->rate_interval_us && (!s || seqno == s->base)){
            if (now < w->rate_next_us){ i_dart_transport_arm_deadline(st, w->rate_next_us); return 0; }
            { i_DartWriterSample *newest = i_dart_sample_find(topic, topic->next_seqno - 1);
              if (newest && newest->base > seqno){
                  w->wire_skip += newest->base - seqno;      /* a paced skip: no perceived loss */
                  w->sent_upto = newest->base; seqno = w->sent_upto; s = newest;
              } }
            w->rate_next_us = now + w->rate_interval_us;
            i_dart_transport_arm_deadline(st, w->rate_next_us);   /* the next tick fires on time */
        }
        if (s){
#ifdef DART_SHM
            /* peer_shm is set before data flows, so this is a sample boundary: one SHM-DATA */
            if (st->peer_shm[peer_slot] && s->shm){
                if (cap < DART_SHM_DATA_BYTES) return 0;
                w->sent_upto = s->base + s->count;
                if (reliable && w->reader_reliable){
                    if (!w->rtt_probe){ w->rtt_probe = 1; w->rtt_probe_seq = w->sent_upto; w->rtt_probe_us = now; }
                    if (w->sent_upto >= topic->next_seqno) i_dart_writer_arm_tail(st, w, (uint32_t)peer_slot, now);
                }
                return i_dart_wire_mk_shm(out,index,per_lane ? s->base - w->wire_skip : s->base,
                                          s->count,s->desc);
            }
#endif
            {
            uint16_t frag_index=(uint16_t)(seqno - s->base);
            uint32_t offset=(uint32_t)frag_index*st->frag;
            uint16_t payload_len=(uint16_t)((s->len-offset)<st->frag?(s->len-offset):st->frag);
            if (cap < (size_t)(s->count==1?DART_HEADER_DATA_SINGLE:DART_HEADER_DATA_MULTI)+(size_t)payload_len) return 0;
            w->sent_upto++;
            topic->repair_stats.frags_sent++;                       /* new data */
            if (reliable && w->reader_reliable){
                /* the sample's last fragment arms the RTT probe: its in order ack comes one
                   round trip after this push */
                if (!w->rtt_probe && w->sent_upto == s->base + s->count){
                    w->rtt_probe = 1; w->rtt_probe_seq = w->sent_upto; w->rtt_probe_us = now; }
                if (w->sent_upto >= topic->next_seqno)
                    i_dart_writer_arm_tail(st, w, (uint32_t)peer_slot, now);   /* tail HB */
            }
            return i_dart_wire_mk_data(out,index,per_lane ? seqno - w->wire_skip : seqno,
                                       s,frag_index,i_dart_sample_buf(s)+offset,payload_len);
            }
        } else {
            /* fell out of the ring unsent. A fire and forget lane advances and its reader
               sees the wire seqno jump as loss. A reliable lane skips its reader with an HB. */
            if (per_lane){ w->sent_upto=(topic->have_first?topic->first_seqno:topic->next_seqno); return 0; }
            if (cap < DART_HEADER_HB) return 0;
            w->sent_upto=(topic->have_first?topic->first_seqno:topic->next_seqno);
            w->rtt_probe=0;
            return i_dart_writer_hb(st,topic,w,index,out,cap,now);
        }
    }

    /* 3. heartbeat: reliable, due, and the reader is behind. A caught up lane goes silent
       until new data or a resubscribe drops acked_upto again. */
    if (reliable && w->reader_reliable && now>=w->hb_next_us && w->acked_upto < topic->next_seqno)
        return i_dart_writer_hb(st,topic,w,index,out,cap,now);
    return 0;
}
