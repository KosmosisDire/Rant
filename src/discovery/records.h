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

/* What a peer is, whether or not it holds a slot. */
typedef struct {
    uint8_t  uuid[16];
    uint32_t local_id;               /* stable across a drop and return */
    uint64_t last_heard_us;
    uint32_t meta_version;           /* the version of its blob judged, 0 = none yet */
    uint32_t adv_version;            /* the highest it advertised, above meta_version = stale */
    uint8_t  ip[16];                 /* the locator */
    uint16_t port;
    uint8_t  ip_len;
    uint8_t  state;                  /* i_RantRecState */
    uint16_t slot;                   /* I_RANT_REC_NONE without one */
    uint16_t uuid_next;              /* the uuid chain, or the free list while FREE */
    uint16_t id_next;                /* the id chain */
    uint8_t  reply_due;              /* owes a unicast announce plus blob, it solicited us */
    uint8_t  solicit_due;            /* owes a unicast REQ, its version is ahead of ours */
    uint8_t  name_len;
    char     name[RANT_NODE_NAME_MAX + 1];
} i_RantDiscoveryRecord;

/* Indices are the durable references: the records move when a hook store grows. */
typedef struct {
    i_RantDiscoveryRecord *recs;     /* [cap] */
    uint16_t   *heads;               /* uuid chains [mask + 1], then id chains [mask + 1] */
    uint16_t    cap, count, free_head, mask;
    uint32_t    next_id;
    RantAllocFn alloc;               /* NULL: placed in the arena, fixed at its reserve */
    void       *alloc_user;
} i_RantDiscoveryRecords;

typedef struct { uint8_t *recs, *heads; } i_RantDiscoveryRecordsBlocks;

/* The arena part of a store of reserve records: all of it without a hook, none with one. */
void     i_rant_records_layout(i_RantBump *b, uint16_t reserve, int hooked,
                               i_RantDiscoveryRecordsBlocks *o);
/* With a hook, reserve records come from it now and more as the store fills. 0 on OOM. */
int      i_rant_records_init(i_RantDiscoveryRecords *r, const i_RantDiscoveryRecordsBlocks *blk,
                             uint16_t reserve, RantAllocFn alloc, void *alloc_user);
void     i_rant_records_destroy(i_RantDiscoveryRecords *r);
/* A zeroed ACTIVE record with a fresh local id and no slot. NONE when full or out of memory. */
uint16_t i_rant_records_add(i_RantDiscoveryRecords *r, const uint8_t uuid[16]);
void     i_rant_records_remove(i_RantDiscoveryRecords *r, uint16_t i);
uint16_t i_rant_records_find(const i_RantDiscoveryRecords *r, const uint8_t uuid[16]);
uint16_t i_rant_records_by_id(const i_RantDiscoveryRecords *r, uint32_t id);

/* The record at i, NULL when i holds none. A pointer is valid until the next add. */
static inline i_RantDiscoveryRecord *i_rant_records_at(const i_RantDiscoveryRecords *r, uint16_t i){
    return (i < r->cap && r->recs[i].state != I_RANT_REC_FREE) ? &r->recs[i] : NULL;
}

#endif /* RANT_DISCOVERY_RECORDS_H */
