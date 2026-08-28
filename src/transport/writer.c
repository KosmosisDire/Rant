/* Transport writer path: history, send, per-lane emit, ACKNACK handling. */
#include "internal.h"


/* find cached sample containing seqno (newest-first, so pushing new data is O(1)) */
static i_DartWriterSample *i_dart_sample_find(i_DartTopic *topic, uint64_t seqno){
    uint16_t depth = topic->qos.keep_last, k;
    uint16_t i = topic->history_head;
    for (k=0;k<depth;k++){
        i_DartWriterSample *s;
        i = (uint16_t)(i ? i-1 : depth-1);
        s = &topic->history[i];
        if (!s->valid) break;                  /* reached the unwritten tail */
        if (seqno >= s->base)
            return (seqno < s->base + s->count) ? s : NULL;
    }
    return NULL;
}


/* seal the filled head slot into history at the next seqno, stamped with its destination
 * (DART__DEST_ALL = broadcast); a commit variant then wakes the lanes that carry it */
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


/* Directed topics: step a lane's counters over samples addressed to OTHER peers, deriving
 * the skips from the history stamps instead of pushing them at commit time. The invariant
 * that makes every directed path safe: acked_upto only ever steps CONTIGUOUSLY, so a lane
 * can never be marked past a sample it is still owed (in flight, lost, or unsent), while
 * sent_upto additionally steps over foreign samples it reaches at a sample boundary (a
 * best-effort lane never acks, so its skips ride sent_upto alone). Sets skip_hb when the
 * acked floor moved: the lane then owes its reader one HB advertising the new floor.
 * Runs at commit, after each ack, and at the top of emit (covering dormant lanes on
 * resume with no commit-time bookkeeping); O(1) when there is nothing to step over. */
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


/* Append the filled head slot to history (stamped with dest_slot) and wake the lanes that
 * carry it: O(matches), not O(max_peers).
 * BROADCAST (dest_slot == DART__DEST_ALL): every live lane carries the sample, so each is
 * simply enqueued and nothing below the branch is ever reached.
 * DIRECTED (a peer slot, or DART__DEST_NONE for a peer nobody holds): only that lane is
 * pushed the data. Every other live lane derives its skip via i_dart_writer_lane_advance (a
 * reliable one then owes the one-shot floor HB); dormant lanes need nothing here, they
 * derive their skips when they next advance after resume. */
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


/* Reject a message larger than the wire can carry, i.e. the 65535-fragment cap
 * (checked before the no-subscriber early-out so an oversize send is refused even
 * when nobody is listening). */
static int i_dart_writer_too_big(DartTransportState *st, i_DartTopic *topic, size_t len){
    (void)topic;
    return len > 65535u*(uint32_t)st->frag;
}

/* Store ts+hdr+data into the head slot (grown to fit via the hook). Fills *len_out with the
 * stored byte count. Returns DART_OK or a negative DartResult; on a negative return nothing
 * was committed. Shared by the broadcast and directed send paths.
 * THE STAMP POINT: a stamped topic (see DartQos.no_timestamp) gets its source timestamp
 * written here, in front of the pattern header, so it is taken ONCE per message and every
 * later use of the sample (repair resend, catch_up replay, the SHM chunk, the consumer
 * queue) carries the ORIGINAL value: this is a SOURCE timestamp, not a transmit one. */
static int i_dart_writer_store(DartTransportState *st, i_DartTopic *topic,
                               DartBytes hdr, DartBytes data, size_t *len_out){
    uint32_t ts = i_dart_topic_ts_bytes(topic);
    size_t len = (size_t)ts + hdr.len + data.len;
    if (i_dart_writer_too_big(st, topic, len)) return DART_ERR_TOO_BIG;
    {   i_DartWriterSample *slot = &topic->history[topic->history_head];
        size_t need = len ? len : 1u;
        if ((size_t)slot->cap < need){                    /* grow the slot to fit (size checked above) */
            uint8_t *new_buf = (uint8_t*)st->cfg.allocator(st->cfg.user, slot->buf, need);
            if (!new_buf) return DART_ERR_OOM;                 /* out of memory */
            slot->buf = new_buf; slot->cap = (uint32_t)need;
        }
    }
    {   uint8_t *dst = topic->history[topic->history_head].buf;    /* gather: ts, hdr, payload */
        if (ts)       i_dart_le_w64(dst, st->cfg.source_time ? st->cfg.source_time(st->cfg.user) : 0u);
        if (hdr.len)  memcpy(dst + ts, hdr.data, hdr.len);
        if (data.len) memcpy(dst + ts + hdr.len, data.data, data.len);
    }
#ifdef DART_SHM
    topic->history[topic->history_head].shm = 0;   /* an inline send: this slot is not SHM-backed */
#endif
    *len_out = len;
    return DART_OK;
}


/* THE send: one prologue (topic lookup, wire cap, role gate, no-subscriber early-out) plus
 * store + commit, shared by the broadcast and directed entry points. dest_slot is stamped
 * onto the sample: DART__DEST_ALL = every matched lane, a peer slot = that lane only. */
static int i_dart_writer_send(DartTransportState *st, uint16_t topic_index,
                              DartBytes hdr, DartBytes data, uint32_t dest_slot){
    i_DartTopic *topic; size_t len; int r;
    topic = i_dart_topic_at(st, topic_index, NULL);                /* rejects the internal meta topic */
    if (!topic) return DART_ERR_NO_TOPIC;
    /* the source stamp is ordinary payload for every size rule: count it in the wire cap */
    if (i_dart_writer_too_big(st, topic, i_dart_topic_ts_bytes(topic) + hdr.len + data.len))
        return DART_ERR_TOO_BIG;
    if (topic->role == DART_SUB_ONLY || topic->role == DART_INACTIVE) return DART_ERR_ROLE;
    /* Nobody subscribes and nothing durable to keep: the sample would land in the ring and
       be orphaned (a fresh match joins at next_seqno unless reliable+catch_up), so skip the
       grow, the copy, and the commit sweep entirely. The many-idle-publishers fast path. */
    if (topic->matched_writers == 0 && !i_dart_topic_retains_history(topic)) return DART_OK;
    r = i_dart_writer_store(st, topic, hdr, data, &len);
    if (r != DART_OK) return r;
    i_dart_writer_commit(st, topic_index, len, dest_slot);
    return DART_OK;
}


int dart_transport_send(DartTransportState *st, uint16_t topic_index, DartBytes data, uint64_t now){
    DartBytes nohdr; nohdr.data=NULL; nohdr.len=0;
    return dart_transport_send_hdr(st, topic_index, nohdr, data, now);
}


int dart_transport_send_hdr(DartTransportState *st, uint16_t topic_index, DartBytes hdr, DartBytes data, uint64_t now){
    (void)now;
    return i_dart_writer_send(st, topic_index, hdr, data, DART__DEST_ALL);
}


int dart_transport_send_to(DartTransportState *st, uint16_t topic_index, uint32_t to_peer,
                           DartBytes hdr, DartBytes data, uint64_t now){
    int peer_slot = i_dart_peer_slot(st, to_peer);   /* unknown peer: the sample is addressed to
                                                        nobody but still consumes its seqnos */
    (void)now;
    return i_dart_writer_send(st, topic_index, hdr, data,
                              peer_slot < 0 ? DART__DEST_NONE : (uint32_t)peer_slot);
}

#ifdef DART_SHM

/* publish a sample whose bytes live in an external (shared-memory) chunk: store the
 * chunk pointer + descriptor on the history slot without copying. Remote peers
 * fragment from the chunk; SHM peers get the one-submessage descriptor. The chunk is
 * already the WHOLE wire sample, so a stamped topic's caller wrote the source timestamp
 * (and any pattern header) into it: there is nothing to gather here. */
int dart_transport_send_shm(DartTransportState *st, uint16_t topic_index, DartBytes chunk,
                  const uint8_t *desc, uint64_t now){
    i_DartTopic *topic; i_DartWriterSample *slot; size_t len = chunk.len;
    (void)now;
    topic = i_dart_topic_at(st, topic_index, NULL);
    if (!topic) return DART_ERR_NO_TOPIC;
    if (len > 65535u*(uint32_t)st->frag) return DART_ERR_TOO_BIG;      /* wire fragment-count cap */
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
    slot = &topic->history[topic->history_head];        /* slot the next send overwrites */
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
    slot = &topic->history[topic->history_head];        /* slot the next send overwrites */
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
            && l->w.acked_upto < topic->next_seqno) return 0;  /* reliable reader still behind */
    }
    return 1;
}


int dart_transport_publisher_match_count(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    return topic ? (int)topic->matched_writers : 0;   /* cached at match/unmatch, so O(1) */
}


/* Matched PUBLISHERS on our subscription side (reader proxies), the mirror of
 * dart_transport_publisher_match_count: how many peers currently feed this topic to us.
 * O(1) from the cached count. */
int dart_transport_subscriber_match_count(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    return topic ? (int)topic->matched_readers : 0;
}


/* Matched subscriber lanes that are LIVE right now (dormant excluded): the liveness-
 * sensitive variant of dart_transport_publisher_match_count, which counts a dropped-but-
 * resumable peer as matched. O(matches); for liveness decisions (a caller failing its
 * outstanding calls when the last provider drops), not for the send fast path. */
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


/* Peer id of the OLDEST live matched subscriber lane (0 = none). The topic chain is
 * newest-first (lanes head-insert at match), so the last live hit is the oldest. */
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
    return cnt;   /* writer lanes with a NACK to service; 0 = nothing to resend right now */
}

#ifdef DART_SHM

/* 1 if the topic has >=1 matched reader and EVERY matched (non-dormant) reader is
 * SHM-capable -> the node may publish this message via SHM. One non-SHM (remote)
 * reader forces inline UDP for the whole message. */
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
/* the history slot the next publish will occupy (so the node binds a chunk to it) */
uint16_t dart_transport_topic_hist_head(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    return topic ? topic->history_head : 0;
}
#endif

/* emit an HB advertising this lane's current window. Doubles as the "skip past a
 * hole" signal that replaces GAP: reader_hb advances deliver_upto to `first`, so a
 * superseded NACK or a ring-overrun push answers with an HB whose first = our floor.
 * Resets the idle-HB timer so we don't double-send. */
static size_t i_dart_writer_hb(DartTransportState *st, i_DartTopic *topic, i_DartWriterProxy *w, uint16_t index,
                             uint8_t *out, size_t cap, uint64_t now){
    uint64_t first = topic->have_first ? topic->first_seqno : 0;
    if (cap < DART_HEADER_HB) return 0;
    if (w->acked_upto > first) first = w->acked_upto;   /* fresh reader adopts join point */
    w->hb_next_us = now + topic->qos.heartbeat_us;
    i_dart_transport_arm_deadline(st, w->hb_next_us);                  /* wake to send the next idle HB */
    w->hb_count++;
    return i_dart_wire_mk_hb(out, index, first, topic->next_seqno-1, w->hb_count);
}


/* The lane's send queue just drained: clamp the next HB into (now, now + DART_HB_TAIL_US]
 * so a lost final message is detected in ~one tail window, not a full heartbeat_us. Fired
 * on a later poll pass, it rides its OWN datagram and never shares the last DATA's fate
 * (a past hb_next_us is replaced too: it would fire in this same drain, coalesced). The
 * reader's immediate ack normally raises acked_upto before the window elapses, which
 * suppresses the HB at step 3, so healthy traffic sends nothing extra. */
static void i_dart_writer_arm_tail(DartTransportState *st, i_DartWriterProxy *w, uint64_t now){
    uint64_t tail = now + DART_HB_TAIL_US;
    if (w->hb_next_us <= now || w->hb_next_us > tail) w->hb_next_us = tail;
    i_dart_transport_arm_deadline(st, w->hb_next_us);
}


/* writer side: handle ACKNACK */
void i_dart_writer_nack(DartTransportState *st, int topic_index, int peer_slot, const uint8_t *p){
    i_DartTopic *topic=&st->topics[topic_index];
    i_DartWriterProxy *w=i_dart_writer_proxy_at(st,topic_index,peer_slot);
    uint64_t base=i_dart_le_r64(p+DART_OFFSET_SEQNO); uint16_t nbits=i_dart_le_r16(p+DART_OFFSET_NACK_NBITS); uint32_t bitmap=i_dart_le_r32(p+DART_OFFSET_NACK_BITMAP);
    uint32_t epoch=i_dart_le_r32(p+DART_OFFSET_NACK_EPOCH); uint8_t flags=p[0];
    if (!w || !w->used) return;
    if (w->reader_epoch != epoch){
        if (w->reader_epoch){
            /* The reader re-incarnated while OUR lane survived: its side retired and
               re-created the topic under the same slot (an identical reuse continues
               the seqno line, and the far side may never even observe the retired
               announce), or a one-sided flap. Re-join at the OLDER of the fresh-match
               join point (so a catch_up window still replays to the new incarnation)
               and the acked floor: everything past acked_upto was committed while the
               lane was continuously matched from our view, so the successor gets the
               un-acked window too (RESUME semantics). Joining at the head alone would
               silently skip a sample committed between its re-create and this ACKNACK
               -- a directed function response races exactly that window. A genuine
               late joiner (unsubscribe, then resubscribe later) never lands here: its
               announce change re-forms our proxy too (fresh, epoch 0 = first contact
               below), keeping plain late-joiner semantics. */
            uint64_t join = i_dart_topic_unicast_join_seqno(topic);
            w->sent_upto  = w->acked_upto < join ? w->acked_upto : join;
            w->acked_upto = w->sent_upto;
            w->has_nack   = 0;
            w->hb_next_us = 0;
            i_dart_lane_wake(st,(uint16_t)topic_index,(uint32_t)peer_slot);
            w->reader_epoch = epoch;
            return;
        }
        w->reader_epoch = epoch;      /* first contact: lane is already fresh */
    }
    if (flags & DART_F_UNPOS){
        /* reader has delivered nothing and never NACKs: re-push from the unacked
           edge (the join window) so a push that raced ahead isn't lost */
        if (w->acked_upto < w->sent_upto){
            w->sent_upto = w->acked_upto;
            i_dart_lane_wake(st,(uint16_t)topic_index,(uint32_t)peer_slot);
        }
        return;                    /* no position information to apply */
    }
    if (base > w->acked_upto) w->acked_upto=base;
    /* directed: the ack may have made foreign samples contiguous from the new floor;
       step over them now and schedule the floor HB the reader is owed */
    i_dart_writer_lane_advance(topic, w, (uint32_t)peer_slot);
    if (w->skip_hb) i_dart_lane_wake(st,(uint16_t)topic_index,(uint32_t)peer_slot);
    if (nbits>0 && bitmap!=0){
        topic->repair_stats.nacks_recv++;                           /* a repair request, not a bare ack */
        w->has_nack=1; w->nack_base=base; w->nack_bits=bitmap;
        i_dart_lane_wake(st,(uint16_t)topic_index,(uint32_t)peer_slot);
    }
}


/* produce one writer submessage for (topic_index,peer_slot) if due and it fits cap; 0 if none.
 * On no-fit, state is untouched so the same submessage is produced next time. */
size_t i_dart_writer_emit(DartTransportState *st, int topic_index, int peer_slot, uint8_t *out, size_t cap, uint64_t now){
    i_DartTopic *topic=&st->topics[topic_index];
    i_DartWriterProxy *w=i_dart_writer_proxy_at(st,topic_index,peer_slot);
    int reliable=(topic->qos.reliability==DART_RELIABLE);
    uint16_t index = i_dart_wire_index_of(st, topic_index);
    if (!w || !w->used || st->peer_dormant[peer_slot]) return 0;   /* unmatched/dormant: nothing to emit */

    /* a FIRE-AND-FORGET lane (best-effort reader, not a directed topic) carries its own
       per-peer wire seqno = sent_upto - wire_skip, so its reader's loss detection counts
       only what was meant for IT. Reliable/directed lanes keep the shared global line
       (repair + directed skip-HB both index history by the global seqno). */
    int per_lane = (!w->reader_reliable && !topic->directed);

    /* directed: derive any owed skips before deciding what to emit (this is also where a
       lane that was dormant during directed sends catches up after resume) */
    i_dart_writer_lane_advance(topic, w, (uint32_t)peer_slot);

    /* 1. repair (reliable only) */
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
                    /* addressed to another lane: NEVER re-serve it here (per-caller call ids
                       make a leaked directed sample deliverable to the wrong pending call).
                       Clear every requested bit inside this sample and owe the floor HB so
                       the reader skips instead. */
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
                    if (st->peer_shm[peer_slot] && s->shm){   /* re-send the whole message as one SHM-DATA */
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
                    if (cap < (size_t)(s->count==1?DART_HEADER_DATA_SINGLE:DART_HEADER_DATA_MULTI)+(size_t)payload_len) return 0;   /* bit stays set */
                    w->nack_bits &= ~(1u<<i);
                    if (w->nack_bits==0) w->has_nack=0;
                    topic->repair_stats.frags_sent++; topic->repair_stats.frags_resent++;   /* retransmit to satisfy a NACK */
                    return i_dart_wire_mk_data(out,index,seqno,s,frag_index,i_dart_sample_buf(s)+offset,payload_len);
                    }
                } else {
                    /* superseded: skip the reader past the dropped region with an HB
                       (its first = our floor); keep still-cached seqnos for later repair */
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

    /* directed-send floor HB, BEFORE any new data: advertise the advanced acked_upto so
       the reader's floor moves past seqnos addressed elsewhere first and the data that
       follows arrives in order (no perceived gap, no NACK round trip). */
    if (reliable && w->reader_reliable && w->skip_hb){
        size_t hb = i_dart_writer_hb(st,topic,w,index,out,cap,now);
        if (!hb) return 0;        /* did not fit this datagram: retry next pass, flag intact */
        w->skip_hb = 0;
        return hb;
    }

    /* 2. push new data */
    if (w->sent_upto < topic->next_seqno){
        uint64_t seqno=w->sent_upto;
        i_DartWriterSample *s=i_dart_sample_find(topic,seqno);
        /* RATE THROTTLE (fire-and-forget lanes), at a sample BOUNDARY only -- once a message's
           first fragment goes out we finish it. A throttled lane only ever HOLDS at a boundary
           (it sends a whole sample per tick), so a NULL s here means the sample we were about to
           start evicted while we held: still a boundary. Before the tick, hold (i_dart_lane_work
           gates on the same clock so this can't spin; the sweep re-wakes at rate_next_us). At the
           tick, DECIMATE to the newest sample (always in history): wire_skip absorbs the skipped
           older ones so the reader sees no gap, while a dropped SENT sample still shows as one. */
        if (per_lane && w->rate_interval_us && (!s || seqno == s->base)){
            if (now < w->rate_next_us){ i_dart_transport_arm_deadline(st, w->rate_next_us); return 0; }
            { i_DartWriterSample *newest = i_dart_sample_find(topic, topic->next_seqno - 1);
              if (newest && newest->base > seqno){
                  w->wire_skip += newest->base - seqno;      /* paced skip: no perceived loss */
                  w->sent_upto = newest->base; seqno = w->sent_upto; s = newest;
              } }
            w->rate_next_us = now + w->rate_interval_us;
            i_dart_transport_arm_deadline(st, w->rate_next_us);   /* next tick fires on time */
        }
        if (s){
#ifdef DART_SHM
            /* peer_shm is set at attach (before data flows), so sent_upto sits at a
               sample boundary here: emit the whole message as one SHM-DATA */
            if (st->peer_shm[peer_slot] && s->shm){
                if (cap < DART_SHM_DATA_BYTES) return 0;
                w->sent_upto = s->base + s->count;
                if (reliable && w->reader_reliable && w->sent_upto >= topic->next_seqno)
                    i_dart_writer_arm_tail(st, w, now);
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
            topic->repair_stats.frags_sent++;                       /* new data (unicast lane) */
            if (reliable && w->reader_reliable && w->sent_upto >= topic->next_seqno)
                i_dart_writer_arm_tail(st, w, now);              /* queue drained: fast tail HB */
            return i_dart_wire_mk_data(out,index,per_lane ? seqno - w->wire_skip : seqno,
                                       s,frag_index,i_dart_sample_buf(s)+offset,payload_len);
            }
        } else {
            /* fell out of the ring before we sent it. A fire-and-forget lane just advances
               past it: wire_skip is untouched, so the NEXT DATA's per-lane seqno jumps, and
               the reader reads that gap as loss (an HB here would carry GLOBAL seqnos and
               corrupt the lane-local reader). A reliable/directed lane skips its reader up to
               first-cached with an HB (its first = our floor). */
            if (per_lane){ w->sent_upto=(topic->have_first?topic->first_seqno:topic->next_seqno); return 0; }
            if (cap < DART_HEADER_HB) return 0;
            w->sent_upto=(topic->have_first?topic->first_seqno:topic->next_seqno);
            return i_dart_writer_hb(st,topic,w,index,out,cap,now);
        }
    }

    /* 3. heartbeat (reliable, timer due, and this reader is behind). Once it has
       acked everything (acked_upto == next_seqno) there's nothing to repair, so the
       lane goes silent until new data or a (re)subscribe drops acked_upto again. The
       HB advertises from acked_upto so a fresh reader adopts the join point. */
    if (reliable && w->reader_reliable && now>=w->hb_next_us && w->acked_upto < topic->next_seqno)
        return i_dart_writer_hb(st,topic,w,index,out,cap,now);
    return 0;
}
