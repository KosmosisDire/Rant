/* The discovery record store: one record per peer heard, found by uuid or by local id.
 * Not a public header. spec/discovery.md, Records and slots. */
#ifndef RANT_DISCOVERY_RECORDS_H
#define RANT_DISCOVERY_RECORDS_H

#include "core.h"
#include "../common/arena.h"

#define I_RANT_REC_NONE 0xFFFFu
#define I_RANT_REC_MAX  0xFFFEu      /* records a store holds, so NONE is never an index */

typedef enum {
    I_RANT_REC_FREE = 0,
    I_RANT_REC_ACTIVE,
    I_RANT_REC_DROPPED              /* silent past peer_timeout_us, kept for a same uuid return */
} i_RantRecState;

/* The work a record is owed, the bits of its owed field. A fetch and a match are both an
 * ask: our blob with REQ|ONE, sent again until settled. */
#define I_RANT_OWE_REPLY 0x01u      /* our announce and blob, once */
#define I_RANT_OWE_FETCH 0x02u      /* its blob, until it comes */
#define I_RANT_OWE_MATCH 0x04u      /* our blob to a peer that may not know it meets us, until it answers */
#define I_RANT_OWE_ASK   (I_RANT_OWE_FETCH | I_RANT_OWE_MATCH)

/* The bits of a record's flags field. */
#define I_RANT_REC_TOO_BIG    0x01u /* its blob is past our bound: not fetched until a newer version or the bound grows */
#define I_RANT_REC_REFETCH    0x02u /* fetched until its blob is judged: a slot opened for it, or the bound grew */
#define I_RANT_REC_ASK_MISSED 0x04u /* the last ask went unanswered */
#define I_RANT_REC_FILTER     0x08u /* filter holds the keys of the blob judged no match at meta_version */

/* What a peer is, whether or not it holds a slot. */
typedef struct {
    uint8_t  uuid[16];
    uint32_t local_id;               /* stable across a drop and return */
    uint32_t meta_version;           /* the version of its blob judged and, with a slot, held. 0 = none yet */
    uint64_t last_heard_us;
    uint64_t uni_us;                 /* last heard by a unicast copy, 0 = never */
    uint64_t due_us;                 /* when its owed work goes out, while queued */
    uint32_t adv_version;            /* the highest it advertised, above meta_version = stale */
    uint8_t  ip[16];                 /* the locator */
    uint16_t port;
    uint8_t  ip_len;
    uint8_t  state;                  /* i_RantRecState */
    uint16_t slot;                   /* I_RANT_REC_NONE without one */
    uint16_t uuid_next;              /* the uuid chain, or the free list while FREE */
    uint16_t id_next;                /* the id chain */
    uint16_t due_prev, due_next;     /* the due queue, which holds exactly the records owed work */
    uint8_t  owed;                   /* I_RANT_OWE_* */
    uint8_t  ask_tries;              /* sends left of the ask owed */
    uint8_t  want;                   /* without a slot, the verdict that wanted one: I_RANT_ADMIT_* */
    uint8_t  flags;                  /* I_RANT_REC_* */
    uint8_t  filter[RANT_DISCOVERY_FILTER_BYTES];   /* what the admit hook folded, see FILTER */
    uint8_t  name_len;
    char     name[RANT_NODE_NAME_MAX + 1];
} i_RantDiscoveryRecord;

/* Indices are the durable references: the records move when a hook store grows. */
typedef struct {
    i_RantDiscoveryRecord *recs;     /* [cap] */
    uint16_t   *heads;               /* uuid chains [mask + 1], then id chains [mask + 1] */
    uint16_t    cap, count, free_head, mask;
    uint16_t    max;                 /* the cap a hook store grows to at most */
    uint16_t    live;                /* ACTIVE records */
    uint16_t    due_head, due_tail;  /* the due queue, oldest owed first */
    uint32_t    next_id;
    RantAllocFn alloc;               /* the chains, and growth. NULL: chains in the arena, fixed at the reserve */
    RantAllocFn bulk;                /* the records. NULL: in the arena */
    void       *alloc_user;          /* both hooks' user */
} i_RantDiscoveryRecords;

typedef struct { uint8_t *recs, *heads; } i_RantDiscoveryRecordsBlocks;

/* The arena part of a store of reserve records: the chains without alloc, the records
 * without alloc or bulk. */
void     i_rant_records_layout(i_RantBump *b, uint16_t reserve, RantAllocFn alloc, RantAllocFn bulk,
                               i_RantDiscoveryRecordsBlocks *o);
/* With alloc, reserve records come from it now and more as the store fills, up to max.
 * bulk, when set, holds the records and alloc only the chains, and without alloc the
 * reserve comes from bulk now and never grows. 0 on OOM. */
int      i_rant_records_init(i_RantDiscoveryRecords *r, const i_RantDiscoveryRecordsBlocks *blk,
                             uint16_t reserve, uint16_t max, RantAllocFn alloc, RantAllocFn bulk,
                             void *alloc_user);
void     i_rant_records_destroy(i_RantDiscoveryRecords *r);
/* A zeroed ACTIVE record with a fresh local id and no slot. NONE when full or out of memory. */
uint16_t i_rant_records_add(i_RantDiscoveryRecords *r, const uint8_t uuid[16]);
void     i_rant_records_remove(i_RantDiscoveryRecords *r, uint16_t i);
uint16_t i_rant_records_find(const i_RantDiscoveryRecords *r, const uint8_t uuid[16]);
uint16_t i_rant_records_by_id(const i_RantDiscoveryRecords *r, uint32_t id);
/* The one writer of a record's state, so live stays exact. */
void     i_rant_records_set_state(i_RantDiscoveryRecords *r, uint16_t i, i_RantRecState state);
/* Owes record i the work in bits by due_us at the latest. A record joins the queue's tail
 * when it starts owing, and keeps its place and the earlier due time after. */
void     i_rant_records_owe(i_RantDiscoveryRecords *r, uint16_t i, uint8_t bits, uint64_t due_us);
/* Clears bits. A record that owes nothing more leaves the queue. */
void     i_rant_records_settle(i_RantDiscoveryRecords *r, uint16_t i, uint8_t bits);
/* Moves an owing record to the queue's tail, due at due_us, after a send that left work. */
void     i_rant_records_requeue(i_RantDiscoveryRecords *r, uint16_t i, uint64_t due_us);

/* The record at i, NULL when i holds none. A pointer is valid until the next add. */
static inline i_RantDiscoveryRecord *i_rant_records_at(const i_RantDiscoveryRecords *r, uint16_t i){
    return (i < r->cap && r->recs[i].state != I_RANT_REC_FREE) ? &r->recs[i] : NULL;
}

#endif /* RANT_DISCOVERY_RECORDS_H */
