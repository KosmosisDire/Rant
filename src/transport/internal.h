/* Shared internals of the split transport core: wire/scheduler constants, the state
 * structs, the small cross-file inline helpers, and prototypes for the helpers that
 * cross the wire/sched/writer/reader file boundaries. Not a public header. */
#ifndef DART_TRANSPORT_INTERNAL_H
#define DART_TRANSPORT_INTERNAL_H

#include "core.h"
#include "../common/bytes.h"
#include <string.h>

/* byte 0 of every submessage: type in the low 3 bits, flags above */
#define DART_DATA 1
#define DART_HB   2
#define DART_NACK 3
#define DART_MSG_MASK 0x07u
#define DART_F_SINGLE 0x08u     /* DATA: single fragment (frag/count/len omitted) */
#define DART_F_UNPOS  0x10u     /* NACK: reader has delivered nothing yet */
#ifdef DART_SHM
#define DART_F_SHM    0x20u     /* DATA: SHM-DATA -- body is a descriptor, no payload */
#endif

#define DART_NACK_WINDOW 32u    /* seqnos covered by one ACKNACK bitmap */
#define DART__NIL 0xFFFFFFFFu

#ifndef DART_HB_SWEEP_US
#define DART_HB_SWEEP_US 25000u /* the timer sweep covers every lane this often */
#endif

#define DART__NO_DEADLINE ((uint64_t)-1)  /* next_deadline_us: nothing armed */

/* per-(peer, index) detail-verdict bits (peer_astate maps; see DartTransportState) */
#define DART__AST_DETAILED 0x01u  /* details received and judged (else PENDING/none) */
#define DART__AST_NAME_OK  0x02u  /* full identity + name verified: peer_index[a] is our topic */
#define DART__AST_READ_OK  0x04u  /* schema gate passed, read side (their pub -> our sub) */
#define DART__AST_WRITE_OK 0x08u  /* schema gate passed, write side (their sub -> our pub) */

/* per-entry interest flags (the announce's hash list; see core.h "Interest exchange") */
#define DART__INT_ROLE_MASK 0x03u /* bits 0-1: the advertiser's DartRole */
#define DART__INT_RELIABLE  0x04u /* bit 2: offered (pub) / requested (sub) reliability */
#define DART__INT_KIND_MASK 0x38u /* bits 3-5: the advertiser's DartTopicKind */
#define DART__INT_KIND_SHIFT 3u
#define DART__INT_FORCEABLE 0x40u /* bit 6: a per-topic patterns flag (variable value channel: owner permits force) */
#ifdef DART_SHM
#ifndef DART_SHM_MAX_RETRY
#define DART_SHM_MAX_RETRY 8u   /* give up on an unresolvable descriptor after this many */
#endif
#endif

#define DART_QOS_DEF_KEEP_LAST     1u
#define DART_QOS_DEF_KEEP_LAST_REL 10u   /* reliable: room for repair before overwrite */
#define DART_QOS_DEF_HEARTBEAT_US 250000u   /* 250 ms idle writer heartbeat */
#define DART_QOS_DEF_REPAIR_US    50000u    /* 50 ms reader repair-request delay */
#ifndef DART_HB_TAIL_US
#define DART_HB_TAIL_US 20000u   /* tail heartbeat: when a lane's send queue drains, the next HB
                                    comes this soon (not heartbeat_us) so a lost FINAL message is
                                    detected fast. The reader's immediate ack normally clears
                                    acked_upto first, suppressing it: no wire cost without loss. */
#endif

/* Submessage wire layout. Byte 0 = type|flags, bytes 1-2 = index, then the body.
 * Builders (dart_mk_*) and the readers both index off these, so moving a field is one
 * edit, never a silent builder/parser drift. Several fields share an offset (distinct
 * names on purpose). Header sizes: DATA 13 (single)/21 (multi), HB 23, NACK 21. */
#define DART_OFFSET_INDEX      1u  /* u16, every submessage */
#define DART_OFFSET_SEQNO      3u  /* DATA seqno; also HB first, NACK base, SHM base (u64) */
#define DART_OFFSET_PAYLOAD_LEN_SINGLE     11u  /* DATA single: u16 payload_len (payload at DART_HEADER_DATA_SINGLE) */
#define DART_HEADER_DATA_SINGLE   13u  /* DATA single: header bytes */
#define DART_OFFSET_FRAG      11u  /* DATA multi: u16 frag */
#define DART_OFFSET_COUNT     13u  /* DATA multi: u16 count */
#define DART_OFFSET_SAMPLE_LEN      15u  /* DATA multi: u32 sample_len */
#define DART_OFFSET_PAYLOAD_LEN      19u  /* DATA multi: u16 payload_len (payload at DART_HEADER_DATA_MULTI) */
#define DART_HEADER_DATA_MULTI    21u  /* DATA multi: header bytes */
#define DART_OFFSET_HB_LAST   11u  /* HB: u64 last */
#define DART_OFFSET_HB_COUNT    19u  /* HB: u32 count */
#define DART_HEADER_HB      23u
#define DART_OFFSET_NACK_NBITS  11u  /* NACK: u16 nbits */
#define DART_OFFSET_NACK_BITMAP   13u  /* NACK: u32 bitmap */
#define DART_OFFSET_NACK_EPOCH  17u  /* NACK: u32 epoch */
#define DART_HEADER_NACK    21u
#define DART_OFFSET_SHM_COUNT 11u  /* SHM-DATA: u16 count */
#define DART_OFFSET_SHM_DESC  13u  /* SHM-DATA: descriptor (DART_SHM_DESC_BYTES follow) */
#ifdef DART_SHM
#define DART_SHM_DATA_BYTES (DART_OFFSET_SHM_DESC + DART_SHM_DESC_BYTES)
#endif

/* The three per-lane/per-slot structs below are laid out widest-field-first (u64s,
 * pointers, u32s, u16s, then u8s) so they carry no padding holes: they are allocated
 * n_topics*max_peers (proxies) and keep_last (samples) times, so padding multiplies. */
/* i_DartWriterSample.dest_slot: which lane a sample is addressed to. DART__DEST_ALL =
 * broadcast (every matched lane); DART__DEST_NONE = directed at a peer that was unknown/
 * unmatched at send time (nobody receives it, every lane steps over it). A peer SLOT is
 * stored, not a peer id: slots are reused after eviction, which is safe here because a
 * directed topic never replays history to a fresh reader (catch_up is refused at define)
 * and a new peer joins at next_seqno, above every stamped sample. */
#define DART__DEST_ALL  0xFFFFFFFFu
#define DART__DEST_NONE 0xFFFFFFFEu

typedef struct {
    uint64_t base;       /* seqno of frag 0 */
    uint8_t *buf;        /* >= len bytes; arena (fixed) or hook-malloc'd (dynamic) */
#ifdef DART_SHM
    const uint8_t *shm_buf;            /* external chunk payload (remote peers fragment from it) */
#endif
    uint32_t len;        /* message bytes */
    uint32_t cap;        /* allocated bytes of buf (dynamic grows it) */
    uint32_t dest_slot;  /* DART__DEST_ALL, DART__DEST_NONE, or the destination peer slot */
    uint16_t count;      /* frag count */
    uint8_t  valid;
#ifdef DART_SHM
    uint8_t  shm;        /* 1 = SHM-backed: bytes live in shm_buf, desc set, buf unused */
    uint8_t  desc[DART_SHM_DESC_BYTES];/* descriptor sent to SHM peers as one SHM-DATA */
#endif
} i_DartWriterSample;

typedef struct {        /* writer-side, per (topic,peer) */
    uint64_t sent_upto;  /* next seqno to push as new data */
    uint64_t acked_upto; /* peer received all TUs < this */
    uint64_t nack_base;
    uint64_t hb_next_us; /* heartbeat timer */
    uint64_t wire_skip;  /* fire-and-forget (best-effort, non-directed) lanes stamp a PER-LANE
                            wire seqno = sent_upto - wire_skip, so this peer sees a private
                            contiguous line and its loss detection counts only samples meant
                            for IT -- never seqnos consumed by other peers, joined-before, or
                            skipped by a rate pacer. = the join seqno at match (pre-join samples
                            were never this lane's loss). A paced skip (rate throttle) adds the
                            skipped count so it leaves NO gap; an eviction jumps sent_upto WITHOUT
                            touching wire_skip, so it DOES leave a gap -> reported as loss.
                            Reliable/directed lanes ignore it: they keep the shared global line
                            so repair maps a NACK straight into history. */
    uint64_t rate_next_us;  /* fire-and-forget throttle (qos.max_rate_hz): earliest time this lane may
                               send its next sample. 0 until the first send, then now + rate_interval_us. */
    uint32_t rate_interval_us;/* us between paced samples (1e6 / max_rate_hz); 0 = unthrottled (full rate).
                               Sourced from the peer's advertised rate at interest apply. */
    uint32_t nack_bits;
    uint32_t hb_count;
    uint32_t reader_epoch; /* reader incarnation from last ACKNACK (0 = none); a change
                              means the peer rebuilt state, so the lane re-joins */
    uint8_t  used;
    uint8_t  reader_reliable; /* the matched reader requested RELIABLE: only then does this
                                 lane impose backpressure + heartbeats. A best-effort reader
                                 never acks, so it must stay out of flow control (fire-and-
                                 forget), else it stalls a reliable writer forever. */
    uint8_t  has_nack;   /* pending repair request from ACKNACK */
    uint8_t  skip_hb;    /* directed send: this non-destination lane owes a one-shot HB whose
                            first advertises the advanced floor, so its reader skips past the
                            seqno addressed to another peer (see dart_transport_send_to) */
} i_DartWriterProxy;

typedef struct {        /* reader-side, per (topic,peer) */
    uint64_t deliver_upto;  /* base of current sample; all below delivered/skipped */
    uint64_t hb_last;       /* highest seqno the writer CLAIMS to hold (heartbeat only) */
    uint64_t received_high;      /* highest seqno we have actually RECEIVED a frag for. UDP is
                               assumed in-order, so a hole below this is real loss to repair
                               while anything above it is still in flight: never NACK past it. */
    uint64_t nack_high;       /* highest seqno already requested this episode; refills ask only
                               (nack_high, top] so in-flight repairs are not re-requested */
    uint64_t nack_retransmit_us;  /* earliest time to re-request a stalled floor (lost-repair backstop) */
    uint64_t ack_due_us;
    uint8_t *assembly_buf;       /* >= assembly_len; arena (fixed) or hook-malloc'd (dynamic) */
    uint8_t *frag_bitmap;       /* ceil(assembly_count/8) */
    uint32_t epoch;         /* this incarnation's id, sent in every ACKNACK */
    uint32_t assembly_len;
    uint32_t assembly_cap;       /* allocated bytes of assembly_buf (dynamic grows it) */
    uint32_t bitmap_cap;        /* allocated bytes of frag_bitmap */
    uint16_t assembly_count;
    uint16_t assembly_low;        /* lowest still-missing frag index of current sample (its
                               contiguous-received front); deliver_upto+assembly_low = first hole */
    uint8_t  used;
    uint8_t  started;       /* accepted any DATA from this writer yet */
    uint8_t  assembly_active;    /* received >=1 frag of current sample */
    uint8_t  ack_force;     /* a delivery/skip/HB/(re)match owes the writer an ACKNACK even if
                               the repair floor did not move (avoids a stuck cumulative ack) */
    uint8_t  ack_pending;
    uint8_t  parked;        /* on_message/on_shm REFUSED the head sample: it is held (inline:
                               assembled in assembly_buf; SHM: the descriptor copied there), with no
                               advance, no ack and no repair traffic, so the writer's flow control
                               backpressures the publisher. dart_transport_deliver_parked retries;
                               a writer floor past it (HB) gives up and skips (bounded loss). */
#ifdef DART_SHM
    uint8_t  parked_shm;    /* the parked hold is a descriptor, not an assembled sample */
    uint8_t  shm_fail;      /* consecutive SHM-DATA resolve failures at deliver_upto */
#endif
} i_DartReaderProxy;

/* One matched lane: both direction proxies plus the scheduler's chain fields. In dynamic
 * mode records are pool-allocated only while the (topic,peer) pair actually matches, so
 * lane memory scales with real matches, not topics x peers; fixed mode (no allocator)
 * keeps the dense identity layout with per-record buffers pre-bound at init. Records move
 * when the pool grows, so nothing holds an i_DartLane* across a call that can allocate:
 * durable references are pool INDICES. */
typedef struct {
    i_DartWriterProxy w;
    i_DartReaderProxy r;
    uint16_t topic;     /* backref: the lane this record serves */
    uint16_t peer_slot;
    uint32_t sched_next;  /* next record on its dest's active list (DART__NIL);
                             doubles as the free-list link while not in_use */
    uint32_t topic_next;     /* next matched lane of the same topic (DART__NIL) */
    uint8_t  queued;      /* on its dest's active list */
    uint8_t  in_use;      /* dynamic: allocated to a lane (0 = free-list); fixed: always 1 */
} i_DartLane;

typedef struct {
    DartQos    qos;
    uint64_t  identity;     /* cross-peer topic identity (hash of name) */
    const char *name;       /* our copy of the topic name (NUL-terminated storage) */
    uint8_t   name_len;     /* its length, stored so it is never re-derived (dart_transport_topic_name is per-delivery) */
    uint16_t  max_frags;     /* ceil(max_message_bytes/FRAG) (fixed mode only) */
    uint8_t   role;         /* DartRole */
    uint8_t   kind;         /* DartTopicKind: gates matching (same kind only) */
    uint8_t   forceable;    /* patterns aux flag advertised in interest bit 6 (variable value channel: owner permits force) */
    uint8_t   prefix_bytes; /* pattern-header bytes in front of each payload (0 = plain) */
    uint8_t   directed;     /* 1 = point-to-point sends; suppress the cross-lane skip MSG_LOST */
    uint8_t   dynamic;      /* 1 = buffers grow via cfg.allocator, no fixed cap */
    uint8_t   history_owned;/* 1 = history ring was allocator-allocated (reserve-mode
                               dart_transport_topic_define), so dart_transport_destroy frees it */
    /* writer */
    i_DartWriterSample *history;       /* [depth] ring */
    uint16_t  history_head;    /* next slot to overwrite */
    uint64_t  next_seqno;
    uint64_t  first_seqno;  /* lowest seqno still cached */
    uint8_t   have_first;
    uint32_t  matched_writers; /* count of used writer proxies (matched subscribers, dormant
                                  included); kept exact at writer match/unmatch so the send path
                                  can skip the copy+commit for a publisher no one subscribes to */
    uint32_t  matched_readers; /* same, for reader proxies; with matched_writers it lets the
                                  timer sweep skip a whole topic row that owes no timer work */
    uint32_t  lane_head;       /* first matched lane record of this topic (DART__NIL = none);
                                  the send path walks this chain instead of scanning peer slots */
    /* cumulative repair counters, summed over proxies; read via dart_transport_repair_stats */
    DartRepairStats repair_stats;
} i_DartTopic;

struct DartTransportState {
    DartConfig    cfg;       /* n_topics = user topics (no internal topic) */
    uint32_t    *peer_ids;  /* [max_peers] */
    uint8_t     *peer_used; /* [max_peers] */
    uint8_t     *peer_dormant;/* [max_peers] 1 = silent (discovery DROP): out of flow control,
                                 proxies + reader position preserved for a same-incarnation resume */
    uint16_t    *peer_frag; /* [max_peers] each peer's advertised fragment size (writer side) */
    uint16_t     frag;      /* this node's fragment size: what we fragment our sends into */
#ifdef DART_SHM
    uint8_t     *peer_shm;  /* [max_peers] 1 = peer can receive SHM-DATA (same host, attached) */
#endif
    /* peer interest over OUR topic table, one bit per user topic; the proxies
       plus these bits are the whole stored interest (full peer lists are not kept).
       Fed by dart_transport_apply_peer_interest from the peer's discovery announce. */
    uint8_t     *peer_pub_bitmap; /* [max_peers][bitmap_len] peer publishes topic c */
    uint8_t     *peer_sub_bitmap; /* [max_peers][bitmap_len] peer subscribes topic c */
    uint8_t     *peer_sub_reliable; /* [max_peers][bitmap_len] ...and requested RELIABLE; sourced
                                       at match time into i_DartWriterProxy.reader_reliable */
    uint16_t     bitmap_len;       /* ceil(n_topics / 8) */
    /* per-peer wire index -> our topic index; the data path carries the 2-byte
       index instead of the topic name. Dynamic mode: each map is a hook allocation
       sized to that peer's highest ADVERTISED index, made on its first matched topic
       (an irrelevant peer costs nothing; a later local subscribe re-applies interest
       and the map already covers every advertised index). Fixed mode: every map is a
       fixed arena slice of index_max entries, exactly the old dense table. */
    uint16_t   **peer_index;      /* [max_peers] -> index map (0xFFFF = unmapped) */
    uint32_t    *peer_index_len;  /* [max_peers] entries in each map */
    uint32_t     index_max;       /* fixed-mode stride = effective DART_META_MAX_IDS */
    /* per-(peer, index) detail verdicts, parallel to peer_index (same length/lifetime).
       An announce entry only NOMINATES by 32-bit hash; a verdict is written once at
       detail intake (name verified against the full identity, schema gated per
       direction) and holds for the peer's incarnation: indices are append-only and
       their name/schema immutable, so re-applying interest derives matches from
       verdict + current flags with no round trip. 0 = no details yet (PENDING if a
       candidate). */
    uint8_t    **peer_astate;     /* [max_peers] -> verdict map (DART__AST_* bits) */
    i_DartTopic  *topics;     /* [n_topics] */
    /* matched-lane records (the proxies live inside). Dynamic mode: one hook allocation
       grown by doubling, records allocated per real match, lane_index maps (topic,peer)
       -> record. Fixed mode: a dense arena array in identity order (record c*max_peers+p),
       lane_index NULL, buffers pre-bound at init. */
    i_DartLane  *lanes;       /* [lane_cap] */
    uint32_t     lane_cap;
    uint32_t     lane_free;   /* free-list head (dynamic), DART__NIL when empty */
    uint16_t    *lane_index;  /* [n_topics*max_peers] record idx, 0xFFFF = unmatched;
                                 NULL = fixed mode (identity mapping, no table) */
    /* active-lane scheduler: a lane is one (topic,peer) pair. The event that gives a
       lane work enqueues it, so poll_send pays for work done, not idle lanes. Timer work
       is found by an amortized clock-driven sweep over the record pool. */
    uint32_t    *dest_head;   /* [max_peers] record-index list per dest */
    uint32_t    *dest_tail;
    uint8_t     *dest_queued;
    uint32_t    *dest_queue;       /* ring of active destinations */
    uint32_t     dest_queue_head, dest_queue_count;
    uint32_t     sweep;       /* timer-sweep lane cursor */
    uint64_t     sweep_time_us;     /* clock position the sweep has paid for */
    uint64_t     next_deadline_us; /* earliest armed timer (ack/HB); caps the poll wait.
                                      Lower bound fed at arm sites, made exact by a full sweep.
                                      DART__NO_DEADLINE = nothing deferred (the hot path). */
    uint32_t     reader_epoch_counter;      /* reader-epoch counter (starts at 1; 0 = none) */
};

typedef enum { DART_ORDER_OLD, DART_ORDER_GAP, DART_ORDER_ADOPTED, DART_ORDER_INORDER } i_DartReaderOrder;

/* small shared helpers (kept inline so every fragment can use them) */
static inline void i_dart_bit_set(uint8_t*bitmap,uint32_t i){bitmap[i>>3]|=(uint8_t)(1u<<(i&7));}
static inline int  i_dart_bit_get(const uint8_t*bitmap,uint32_t i){return (bitmap[i>>3]>>(i&7))&1;}
#ifdef DART_SHM
/* where a sample's bytes live: the external chunk for SHM samples, else our buf */
static inline const uint8_t *i_dart_sample_buf(const i_DartWriterSample *s){ return s->shm ? s->shm_buf : s->buf; }
#else
static inline const uint8_t *i_dart_sample_buf(const i_DartWriterSample *s){ return s->buf; }
#endif
/* a deferred timer was (re)armed for absolute time t: keep next_deadline as the
   minimum so the poll wakes when it is due. t==0 is an immediate ack (woken via the
   active-lane queue, not a timer), so it is ignored here. */
static inline void i_dart_transport_arm_deadline(DartTransportState *st, uint64_t t){
    if (t && t < st->next_deadline_us) st->next_deadline_us = t;
}

/* Does a late joiner ever receive samples published before it matched? Only a reliable
 * topic with catch_up>0 replays cached history (i_dart_topic_unicast_join_seqno reaches
 * back); every other topic joins at next_seqno. So when no subscriber is matched, history
 * on any other topic is dead weight and the send can be skipped outright. */
static inline int i_dart_topic_retains_history(const i_DartTopic *topic){
    return topic->qos.reliability==DART_RELIABLE && topic->qos.catch_up>0;
}

/* Does a topic owe the timer sweep any work? Writer heartbeats and reader acks both
 * require a reliable topic with a used proxy, so a best-effort topic (the common
 * high-rate case) or a reliable one no peer has matched owes nothing: the sweep skips its
 * whole peer row. Reliability is fixed at define and the counts are exact, so this never
 * skips a lane that has a live HB/ack timer. */
static inline int i_dart_topic_needs_sweep(const i_DartTopic *topic){
    return topic->qos.reliability==DART_RELIABLE && (topic->matched_writers || topic->matched_readers);
}

/* lane record for a (topic,peer) pair: pool index, or DART__NIL when the lane has
   never matched (dynamic mode; fixed mode maps every lane by identity). Centralizing
   the index math here keeps a transposed topic/peer from silently corrupting a
   neighbor lane. */
static inline uint32_t i_dart_lane_id(DartTransportState *st, uint16_t topic_index, uint32_t peer_slot){
    size_t k = (size_t)topic_index*st->cfg.max_peers + peer_slot;
    uint16_t lane;
    if (!st->lane_index) return (uint32_t)k;          /* fixed: identity */
    lane = st->lane_index[k];
    return lane == 0xFFFFu ? DART__NIL : (uint32_t)lane;
}
static inline i_DartLane *i_dart_lane_at(DartTransportState *st, uint16_t topic_index, uint32_t peer_slot){
    uint32_t li = i_dart_lane_id(st, topic_index, peer_slot);
    return li == DART__NIL ? NULL : &st->lanes[li];
}
/* NULL when the lane is unmatched (no record): every caller treats that as !used. */
static inline i_DartWriterProxy *i_dart_writer_proxy_at(DartTransportState *st, uint16_t topic_index, uint32_t peer_slot){
    i_DartLane *l = i_dart_lane_at(st, topic_index, peer_slot);
    return l ? &l->w : NULL;
}
static inline i_DartReaderProxy *i_dart_reader_proxy_at(DartTransportState *st, uint16_t topic_index, uint32_t peer_slot){
    i_DartLane *l = i_dart_lane_at(st, topic_index, peer_slot);
    return l ? &l->r : NULL;
}
/* wire index for a local topic: its own index (the advertiser's handle). The
 * peer mapped this index to its matching topic from our interest list. */
static inline uint16_t i_dart_wire_index_of(DartTransportState *st, int topic_index){
    (void)st; return (uint16_t)topic_index;
}

/* cross-file helper prototypes (definitions in wire/sched/writer/reader.c + transport.c) */
size_t i_dart_wire_mk_data(uint8_t *o, uint16_t index, uint64_t seqno, i_DartWriterSample *s, uint16_t frag, const uint8_t *payload, uint16_t payload_len);
#ifdef DART_SHM
size_t i_dart_wire_mk_shm(uint8_t *o, uint16_t index, uint64_t base, uint16_t count, const uint8_t *desc);
#endif
size_t i_dart_wire_mk_hb(uint8_t *o, uint16_t index, uint64_t first, uint64_t last, uint32_t cnt);
size_t i_dart_wire_mk_nack(uint8_t *o, uint16_t index, uint64_t base, uint16_t nbits, uint32_t bitmap, uint32_t epoch, uint8_t flags);
void   i_dart_lane_wake(DartTransportState *st, uint16_t topic_index, uint32_t peer_slot);
void   i_dart_lane_enqueue(DartTransportState *st, uint32_t li);   /* enqueue a lane record by index */
void   i_dart_sched_drop(DartTransportState *st, uint32_t rec);     /* unlink a record from its dest list */
size_t i_dart_writer_emit(DartTransportState *st, int topic_index, int peer_slot, uint8_t *out, size_t cap, uint64_t now);
void   i_dart_writer_nack(DartTransportState *st, int topic_index, int peer_slot, const uint8_t *p);
void   i_dart_reader_data(DartTransportState *st, int topic_index, int peer_slot, const uint8_t *p, uint64_t now);
#ifdef DART_SHM
void   i_dart_reader_shm(DartTransportState *st, int topic_index, int peer_slot, const uint8_t *p, uint64_t now);
#endif
void   i_dart_reader_hb(DartTransportState *st, int topic_index, int peer_slot, const uint8_t *p, uint64_t now);
size_t i_dart_reader_emit(DartTransportState *st, int topic_index, int peer_slot, uint8_t *out, size_t cap, uint64_t now);
i_DartTopic *i_dart_topic_at(DartTransportState *st, uint16_t topic_index, int *idx_out);
int    i_dart_peer_slot(DartTransportState *st, uint32_t id);
void   i_dart_transport_fire_event(DartTransportState *st, DartTransportEventKind kind, uint16_t topic_index, uint32_t peer, uint64_t first, uint64_t count);
uint64_t i_dart_topic_unicast_join_seqno(const i_DartTopic *topic);

#endif /* DART_TRANSPORT_INTERNAL_H */
