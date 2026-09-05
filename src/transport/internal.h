/* Shared internals of the transport core. Not a public header. */
#ifndef DART_TRANSPORT_INTERNAL_H
#define DART_TRANSPORT_INTERNAL_H

#include "core.h"
#include "../common/bytes.h"
#include <string.h>

/* byte 0 of every submessage: the type in the low 3 bits, flags above */
#define DART_DATA 1
#define DART_HB   2
#define DART_NACK 3
#define DART_MSG_MASK 0x07u
#define DART_F_SINGLE 0x08u     /* DATA: single fragment, frag, count and len omitted */
#define DART_F_UNPOS  0x10u     /* NACK: the reader has delivered nothing yet */
#ifdef DART_SHM
#define DART_F_SHM    0x20u     /* DATA: the body is a descriptor, no payload */
#endif

#define DART_NACK_WINDOW 32u    /* seqnos covered by one ACKNACK bitmap */
#define DART__NIL 0xFFFFFFFFu

#ifndef DART_HB_SWEEP_US
#define DART_HB_SWEEP_US 25000u /* the timer sweep covers every lane this often */
#endif

#define DART__NO_DEADLINE ((uint64_t)-1)  /* next_deadline_us: nothing armed */

/* per (peer, index) detail verdict bits, the peer_astate maps */
#define DART__AST_DETAILED 0x01u  /* details received and judged, else pending */
#define DART__AST_NAME_OK  0x02u  /* identity and name verified: peer_index[a] is our topic */
#define DART__AST_READ_OK  0x04u  /* schema gate passed on the read side, their pub to our sub */
#define DART__AST_WRITE_OK 0x08u  /* schema gate passed on the write side, their sub to our pub */

/* per entry interest flags, the announce's hash list */
#define DART__INT_ROLE_MASK 0x03u /* bits 0 and 1: the advertiser's DartRole */
#define DART__INT_RELIABLE  0x04u /* bit 2: offered or requested reliability */
#define DART__INT_KIND_MASK 0x78u /* bits 3 to 6: the advertiser's DartTopicKind */
#define DART__INT_KIND_SHIFT 3u
#define DART__INT_HOLE_RUN  0x80u /* bit 7: a run of undefined slots, hash = the run length */
#ifdef DART_SHM
#ifndef DART_SHM_MAX_RETRY
#define DART_SHM_MAX_RETRY 8u   /* give up on an unresolvable descriptor after this many */
#endif
#endif

#define DART_QOS_DEF_KEEP_LAST     1u
#define DART_QOS_DEF_KEEP_LAST_REL 10u   /* reliable: room for repair before overwrite */
#define DART_QOS_DEF_HEARTBEAT_US 250000u   /* 250 ms idle writer heartbeat */
#define DART_QOS_DEF_REPAIR_US    50000u    /* 50 ms reader repair request delay */
#ifndef DART_RTO_MIN_US
#define DART_RTO_MIN_US   2000u  /* the RTT timer floor, a poll wait cannot fire sooner */
#endif
#define DART_RTO_GRAIN_US 1000u  /* RFC 6298 G: the deviation term never counts under one tick */
#ifndef DART_HB_TAIL_US
#define DART_HB_TAIL_US 20000u   /* the tail heartbeat before the peer's RTT is known */
#endif

/* Submessage layout. Byte 0 = type and flags, bytes 1 and 2 = index, then the body.
 * Builders and readers share these offsets, so moving a field is one edit. */
#define DART_OFFSET_INDEX      1u  /* u16, every submessage */
#define DART_OFFSET_SEQNO      3u  /* DATA seqno, HB first, NACK base, SHM base (u64) */
#define DART_OFFSET_PAYLOAD_LEN_SINGLE     11u  /* DATA single: u16 payload_len */
#define DART_HEADER_DATA_SINGLE   13u  /* DATA single: header bytes */
#define DART_OFFSET_FRAG      11u  /* DATA multi: u16 frag */
#define DART_OFFSET_COUNT     13u  /* DATA multi: u16 count */
#define DART_OFFSET_SAMPLE_LEN      15u  /* DATA multi: u32 sample_len */
#define DART_OFFSET_PAYLOAD_LEN      19u  /* DATA multi: u16 payload_len */
#define DART_HEADER_DATA_MULTI    21u  /* DATA multi: header bytes */
#define DART_OFFSET_HB_LAST   11u  /* HB: u64 last */
#define DART_OFFSET_HB_COUNT    19u  /* HB: u32 count */
#define DART_HEADER_HB      23u
#define DART_OFFSET_NACK_NBITS  11u  /* NACK: u16 nbits */
#define DART_OFFSET_NACK_BITMAP   13u  /* NACK: u32 bitmap */
#define DART_OFFSET_NACK_EPOCH  17u  /* NACK: u32 epoch */
#define DART_HEADER_NACK    21u
#define DART_OFFSET_SHM_COUNT 11u  /* SHM-DATA: u16 count */
#define DART_OFFSET_SHM_DESC  13u  /* SHM-DATA: the descriptor */
#ifdef DART_SHM
#define DART_SHM_DATA_BYTES (DART_OFFSET_SHM_DESC + DART_SHM_DESC_BYTES)
#endif

/* The per lane and per sample structs are laid out widest field first so they carry no
 * padding, since they are allocated per match and per history slot. */
/* A sample's destination: every matched lane, nobody, or a peer slot. A slot is safe
 * because a directed topic never replays history and a new peer joins at the head. */
#define DART__DEST_ALL  0xFFFFFFFFu
#define DART__DEST_NONE 0xFFFFFFFEu

typedef struct {
    uint64_t base;       /* seqno of frag 0 */
    uint8_t *buf;        /* hook allocated, grown to fit */
#ifdef DART_SHM
    const uint8_t *shm_buf;            /* the external chunk payload, remote peers fragment it */
#endif
    uint32_t len;        /* message bytes */
    uint32_t cap;        /* allocated bytes of buf */
    uint32_t dest_slot;  /* DART__DEST_ALL, DART__DEST_NONE, or the destination peer slot */
    uint16_t count;      /* frag count */
    uint8_t  valid;
#ifdef DART_SHM
    uint8_t  shm;        /* 1 = the bytes live in shm_buf and desc is set */
    uint8_t  desc[DART_SHM_DESC_BYTES];/* sent to SHM peers as one SHM-DATA */
#endif
} i_DartWriterSample;

typedef struct {        /* writer side, per (topic, peer) */
    uint64_t sent_upto;  /* next seqno to push as new data */
    uint64_t acked_upto; /* the peer received everything below this */
    uint64_t nack_base;
    uint64_t hb_next_us; /* heartbeat timer */
    uint64_t wire_skip;  /* a fire and forget lane stamps sent_upto minus this as its private wire
                            seqno, so its reader counts only its own loss. See spec/transport.md */
    uint64_t rate_next_us;  /* earliest time a throttled lane may send its next sample */
    uint32_t rate_interval_us;/* us between paced samples, 0 = unthrottled */
    uint32_t nack_bits;
    uint32_t hb_count;
    uint32_t reader_epoch; /* the reader incarnation from its last ACKNACK, a change re joins */
    uint8_t  used;
    uint8_t  reader_reliable; /* only a reliable reader gets backpressure and heartbeats */
    uint8_t  has_nack;   /* a pending repair request */
    uint8_t  skip_hb;    /* a directed send left this lane owing a floor HB */
    uint8_t  rtt_probe;  /* armed at rtt_probe_seq, pushed at rtt_probe_us, any repair disarms it */
    uint64_t rtt_probe_seq;
    uint64_t rtt_probe_us;
} i_DartWriterProxy;

/* One sample's reassembly state. Buffers grow through the hook and are kept across rematch. */
typedef struct {
    uint8_t *buf;           /* at least len bytes */
    uint8_t *bitmap;        /* ceil(count/8): fragments held */
    uint64_t base;          /* seqno of frag 0 */
    uint32_t len;           /* message bytes */
    uint32_t cap;           /* allocated bytes of buf */
    uint32_t bitmap_cap;    /* allocated bytes of bitmap */
    uint16_t count;         /* frag count */
    uint16_t low;           /* the lowest missing fragment, low == count is complete */
    uint8_t  active;        /* received at least one fragment */
} i_DartAssembly;

typedef struct {        /* reader side, per (topic, peer) */
    uint64_t deliver_upto;  /* base of the current sample, everything below delivered or skipped */
    uint64_t hb_last;       /* the highest seqno the writer claims to hold */
    uint64_t received_high;      /* highest seqno actually received. Never NACK past it */
    uint64_t nack_high;       /* highest seqno requested this episode, a refill asks above it */
    uint64_t nack_retransmit_us;  /* earliest re ask of a stalled floor */
    uint64_t ack_due_us;
    i_DartAssembly cur;     /* the head sample */
    i_DartAssembly next;    /* one sample held ahead of the head. See spec/transport.md */
    uint32_t epoch;         /* this incarnation's id, sent in every ACKNACK */
    uint8_t  used;
    uint8_t  no_timestamp;  /* the writer sends no source stamp, from its cached attrs */
    uint8_t  started;       /* accepted any DATA from this writer yet */
    uint8_t  ack_force;     /* owes a cumulative ack even at an unchanged repair floor */
    uint8_t  ack_pending;
    uint8_t  parked;        /* the head is held after a downstream refusal, spec/transport.md */
    uint8_t  lapped;        /* a floor skip took a sample being fetched, nothing delivered since */
    uint8_t  rtt_probe;     /* a probe is armed at rtt_probe_seq, first asked at rtt_probe_us */
    uint64_t rtt_probe_seq;
    uint64_t rtt_probe_us;
#ifdef DART_SHM
    uint8_t  parked_shm;    /* the parked hold is a descriptor, not an assembled sample */
    uint8_t  shm_fail;      /* consecutive resolve failures at deliver_upto */
#endif
} i_DartReaderProxy;

/* One matched lane: both proxies plus the scheduler links. Records move when the pool
 * grows, so durable references are pool indices, never pointers. */
typedef struct {
    i_DartWriterProxy w;
    i_DartReaderProxy r;
    uint16_t topic;     /* the lane this record serves */
    uint16_t peer_slot;
    uint32_t sched_next;  /* next on its dest's active list, or the free list link */
    uint32_t topic_next;     /* next matched lane of the same topic */
    uint8_t  queued;      /* on its dest's active list */
    uint8_t  in_use;      /* allocated to a lane, 0 = on the free list */
} i_DartLane;

typedef struct {
    DartQos    qos;
    uint64_t  identity;     /* the name hash */
    const char *name;       /* our copy, NUL terminated storage */
    uint8_t   name_len;
    uint8_t   role;         /* DartRole */
    uint8_t   kind;         /* DartTopicKind, gates matching */
    uint8_t   attrs;        /* DART_ATTR_* from the definer, NO_TIMESTAMP derives from the qos */
    uint8_t   prefix_bytes; /* pattern header bytes in front of each payload */
    uint8_t   directed;     /* point to point sends, no cross lane MSG_LOST */
    uint8_t   history_owned;/* the ring came from the allocator, so destroy frees it */
    uint8_t   retired;      /* parked for reuse: announced as a hole, never a match candidate */
    uint8_t   gen;          /* rebind generation, bumped when a reused slot's binding changed */
    uint32_t  rebind_version; /* our blob version at the last rebind, 0 = never. Writer lanes
                                 hold until the peer has seen it */
    /* writer */
    i_DartWriterSample *history;       /* [depth] ring */
    uint16_t  history_head;    /* next slot to overwrite */
    uint64_t  next_seqno;
    uint64_t  first_seqno;  /* lowest seqno still cached */
    uint8_t   have_first;
    uint32_t  matched_writers; /* exact count of used writer proxies, dormant included */
    uint32_t  matched_readers; /* the same for reader proxies */
    uint32_t  lane_head;       /* first matched lane record, the send path walks this chain */
    DartRepairStats repair_stats;
} i_DartTopic;

struct DartTransportState {
    DartConfig    cfg;
    uint32_t    *peer_ids;  /* [max_peers] */
    uint8_t     *peer_used; /* [max_peers] */
    uint8_t     *peer_dormant;/* [max_peers] silent peers, out of flow control, state kept */
    uint16_t    *peer_frag; /* [max_peers] each peer's advertised fragment size */
    DartPeerRtt *peer_rtt;  /* [max_peers] the round trip estimator */
    uint16_t     frag;      /* our fragment size */
#ifdef DART_SHM
    uint8_t     *peer_shm;  /* [max_peers] 1 = the peer can receive SHM-DATA */
#endif
    /* one bit per topic per peer, with the proxies the whole stored interest */
    uint8_t     *peer_pub_bitmap; /* [max_peers][bitmap_len] the peer publishes topic c */
    uint8_t     *peer_sub_bitmap; /* [max_peers][bitmap_len] the peer subscribes topic c */
    uint8_t     *peer_sub_reliable; /* [max_peers][bitmap_len] and requested RELIABLE */
    uint16_t     bitmap_len;       /* ceil(n_topics / 8) */
    /* per peer wire index to our topic, sized to the peer's highest advertised index */
    uint16_t   **peer_index;      /* [max_peers] the index map, 0xFFFF = unmapped */
    uint32_t    *peer_index_len;  /* [max_peers] entries in each map */
    /* per (peer, index) detail verdicts, parallel to peer_index. See spec/interest.md */
    uint8_t    **peer_astate;     /* [max_peers] DART__AST_* bits per entry */
    uint8_t    **peer_agen;       /* [max_peers] the last applied rebind generation per entry */
    uint8_t    **peer_attrs;      /* [max_peers] the peer's advertised attrs per entry */
    uint32_t    *peer_seen_version; /* [max_peers] the highest version of our blob the peer named */
    i_DartTopic  *topics;     /* [n_topics] */
    /* the lane record pool: one hook allocation grown by doubling, records per real match */
    i_DartLane  *lanes;       /* [lane_cap] */
    uint32_t     lane_cap;
    uint32_t     lane_free;   /* free list head, DART__NIL when empty */
    uint16_t    *lane_index;  /* [n_topics*max_peers] record index, 0xFFFF = unmatched */
    /* the active lane scheduler: the event that gives a lane work enqueues it */
    uint32_t    *dest_head;   /* [max_peers] record index list per dest */
    uint32_t    *dest_tail;
    uint8_t     *dest_queued;
    uint32_t    *dest_queue;       /* ring of active destinations */
    uint32_t     dest_queue_head, dest_queue_count;
    uint32_t     sweep;       /* the timer sweep cursor */
    uint64_t     sweep_time_us;     /* the clock position the sweep has paid for */
    uint64_t     next_deadline_us; /* the earliest armed timer, caps the poll wait */
    uint32_t     reader_epoch_counter;      /* starts at 1, 0 = none */
};

typedef enum { DART_ORDER_OLD, DART_ORDER_GAP, DART_ORDER_ADOPTED, DART_ORDER_INORDER } i_DartReaderOrder;

static inline void i_dart_bit_set(uint8_t*bitmap,uint32_t i){bitmap[i>>3]|=(uint8_t)(1u<<(i&7));}
static inline void i_dart_bit_clr(uint8_t*bitmap,uint32_t i){bitmap[i>>3]&=(uint8_t)~(1u<<(i&7));}
static inline int  i_dart_bit_get(const uint8_t*bitmap,uint32_t i){return (bitmap[i>>3]>>(i&7))&1;}
/* Does this slot occupy an announce entry. An undefined or retired slot rides as a hole run. */
static inline int i_dart_topic_announced(const i_DartTopic *t){
    return t->name_len != 0 && !t->retired;
}
#ifdef DART_SHM
/* where a sample's bytes live: the external chunk for SHM samples, else our buf */
static inline const uint8_t *i_dart_sample_buf(const i_DartWriterSample *s){ return s->shm ? s->shm_buf : s->buf; }
#else
static inline const uint8_t *i_dart_sample_buf(const i_DartWriterSample *s){ return s->buf; }
#endif
/* Keeps next_deadline at the minimum armed time. t 0 is an immediate ack, woken through
   the active lane queue, so it is ignored here. */
static inline void i_dart_transport_arm_deadline(DartTransportState *st, uint64_t t){
    if (t && t < st->next_deadline_us) st->next_deadline_us = t;
}

/* Source stamp bytes at the front of this topic's samples. Framing follows the qos alone. */
static inline uint32_t i_dart_topic_ts_bytes(const i_DartTopic *topic){
    return topic->qos.no_timestamp ? 0u : DART_TIMESTAMP_BYTES;
}

/* Only a reliable topic with catch_up replays history to a late joiner. Everywhere else
 * history with no subscriber is dead weight and the send can be skipped. */
static inline int i_dart_topic_retains_history(const i_DartTopic *topic){
    return topic->qos.reliability==DART_RELIABLE && topic->qos.catch_up>0;
}

/* Heartbeats and acks both need a reliable topic with a matched proxy, so the sweep
 * skips the whole peer row of any other topic. */
static inline int i_dart_topic_needs_sweep(const i_DartTopic *topic){
    return topic->qos.reliability==DART_RELIABLE && (topic->matched_writers || topic->matched_readers);
}

/* The lane record for a (topic, peer) pair, DART__NIL when unmatched. */
static inline uint32_t i_dart_lane_id(DartTransportState *st, uint16_t topic_index, uint32_t peer_slot){
    uint16_t lane = st->lane_index[(size_t)topic_index*st->cfg.max_peers + peer_slot];
    return lane == 0xFFFFu ? DART__NIL : (uint32_t)lane;
}
static inline i_DartLane *i_dart_lane_at(DartTransportState *st, uint16_t topic_index, uint32_t peer_slot){
    uint32_t li = i_dart_lane_id(st, topic_index, peer_slot);
    return li == DART__NIL ? NULL : &st->lanes[li];
}
/* NULL when the lane is unmatched. Every caller treats that as not used. */
static inline i_DartWriterProxy *i_dart_writer_proxy_at(DartTransportState *st, uint16_t topic_index, uint32_t peer_slot){
    i_DartLane *l = i_dart_lane_at(st, topic_index, peer_slot);
    return l ? &l->w : NULL;
}
static inline i_DartReaderProxy *i_dart_reader_proxy_at(DartTransportState *st, uint16_t topic_index, uint32_t peer_slot){
    i_DartLane *l = i_dart_lane_at(st, topic_index, peer_slot);
    return l ? &l->r : NULL;
}
/* The wire index of a local topic is its own index. The peer mapped it from our interest. */
static inline uint16_t i_dart_wire_index_of(DartTransportState *st, int topic_index){
    (void)st; return (uint16_t)topic_index;
}

/* cross file prototypes */
size_t i_dart_wire_mk_data(uint8_t *o, uint16_t index, uint64_t seqno, i_DartWriterSample *s, uint16_t frag, const uint8_t *payload, uint16_t payload_len);
#ifdef DART_SHM
size_t i_dart_wire_mk_shm(uint8_t *o, uint16_t index, uint64_t base, uint16_t count, const uint8_t *desc);
#endif
size_t i_dart_wire_mk_hb(uint8_t *o, uint16_t index, uint64_t first, uint64_t last, uint32_t cnt);
size_t i_dart_wire_mk_nack(uint8_t *o, uint16_t index, uint64_t base, uint16_t nbits, uint32_t bitmap, uint32_t epoch, uint8_t flags);
void   i_dart_lane_wake(DartTransportState *st, uint16_t topic_index, uint32_t peer_slot);
void   i_dart_lane_enqueue(DartTransportState *st, uint32_t li);
void   i_dart_sched_drop(DartTransportState *st, uint32_t rec);
size_t i_dart_writer_emit(DartTransportState *st, int topic_index, int peer_slot, uint8_t *out, size_t cap, uint64_t now);
void   i_dart_writer_nack(DartTransportState *st, int topic_index, int peer_slot, const uint8_t *p, uint64_t now);
/* the per peer RTT estimator: fold one sample, and the retransmit bound it implies */
void     i_dart_rtt_sample(DartTransportState *st, uint32_t peer_slot, uint64_t sample_us);
uint32_t i_dart_rtt_rto(DartTransportState *st, uint32_t peer_slot, uint32_t fallback_us);
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
