/* The discovery record store. spec/discovery.md, Records and slots. */
#include "records.h"
#include "../common/hash.h"
#include <string.h>

/* A power of two at least cap, so a chain averages one record. */
static uint32_t i_rant_records_buckets(uint16_t cap){
    uint32_t n = 4u;
    while (n < cap) n <<= 1;
    return n;
}

static uint32_t i_rant_records_uuid_bucket(const i_RantDiscoveryRecords *r, const uint8_t uuid[16]){
    return (uint32_t)i_rant_fnv1a64(uuid, 16) & r->mask;
}

/* Ids are sequential, so the low bits spread them evenly. */
static uint32_t i_rant_records_id_bucket(const i_RantDiscoveryRecords *r, uint32_t id){
    return id & r->mask;
}

static uint16_t *i_rant_records_id_heads(const i_RantDiscoveryRecords *r){
    return r->heads + (size_t)r->mask + 1u;
}

static void i_rant_records_link(i_RantDiscoveryRecords *r, uint16_t i){
    i_RantDiscoveryRecord *rec = &r->recs[i];
    uint16_t *uh = &r->heads[i_rant_records_uuid_bucket(r, rec->uuid)];
    uint16_t *ih = &i_rant_records_id_heads(r)[i_rant_records_id_bucket(r, rec->local_id)];
    rec->uuid_next = *uh; *uh = i;
    rec->id_next   = *ih; *ih = i;
}

static void i_rant_records_rehash(i_RantDiscoveryRecords *r){
    uint32_t nb = r->mask + 1u, k;
    uint16_t i;
    for (k = 0; k < 2u * nb; k++) r->heads[k] = I_RANT_REC_NONE;
    for (i = 0; i < r->cap; i++)
        if (r->recs[i].state != I_RANT_REC_FREE) i_rant_records_link(r, i);
}

/* Threads the records from first up onto the free list, the lowest taken first. */
static void i_rant_records_free_from(i_RantDiscoveryRecords *r, uint16_t first){
    uint16_t i;
    for (i = r->cap; i > first; i--){
        r->recs[i - 1u].state = I_RANT_REC_FREE;
        r->recs[i - 1u].uuid_next = r->free_head;
        r->free_head = (uint16_t)(i - 1u);
    }
}

void i_rant_records_layout(i_RantBump *b, uint16_t reserve, int hooked, i_RantDiscoveryRecordsBlocks *o){
    o->recs = o->heads = NULL;
    if (hooked) return;
    o->recs  = (uint8_t*)i_rant_bump_take(b, (size_t)reserve * sizeof(i_RantDiscoveryRecord), 8);
    o->heads = (uint8_t*)i_rant_bump_take(b, 2u * i_rant_records_buckets(reserve) * sizeof(uint16_t), 2);
}

int i_rant_records_init(i_RantDiscoveryRecords *r, const i_RantDiscoveryRecordsBlocks *blk,
                        uint16_t reserve, RantAllocFn alloc, void *alloc_user){
    uint32_t nb = i_rant_records_buckets(reserve);
    memset(r, 0, sizeof *r);
    if (reserve > I_RANT_REC_MAX) return 0;
    r->alloc = alloc; r->alloc_user = alloc_user;
    if (alloc){
        r->recs  = (i_RantDiscoveryRecord*)alloc(alloc_user, NULL, (size_t)reserve * sizeof(i_RantDiscoveryRecord));
        r->heads = (uint16_t*)alloc(alloc_user, NULL, 2u * nb * sizeof(uint16_t));
        if (!r->recs || !r->heads){ i_rant_records_destroy(r); return 0; }
    } else {
        r->recs  = (i_RantDiscoveryRecord*)blk->recs;
        r->heads = (uint16_t*)blk->heads;
        if (!r->recs || !r->heads) return 0;
    }
    r->cap = reserve; r->mask = (uint16_t)(nb - 1u);
    r->free_head = I_RANT_REC_NONE;
    r->next_id = 1;
    memset(r->recs, 0, (size_t)reserve * sizeof(i_RantDiscoveryRecord));
    i_rant_records_free_from(r, 0);
    i_rant_records_rehash(r);
    return 1;
}

void i_rant_records_destroy(i_RantDiscoveryRecords *r){
    if (!r->alloc) return;      /* arena memory stays the caller's */
    if (r->recs) r->alloc(r->alloc_user, r->recs, 0);
    if (r->heads) r->alloc(r->alloc_user, r->heads, 0);
    r->recs = NULL; r->heads = NULL; r->cap = 0; r->count = 0; r->free_head = I_RANT_REC_NONE;
}

/* Doubles a hook store. The chains grow with it when the hook allows, else they lengthen. */
static int i_rant_records_grow(i_RantDiscoveryRecords *r){
    uint32_t ncap = r->cap ? (uint32_t)r->cap * 2u : 8u, nb;
    uint16_t old = r->cap;
    i_RantDiscoveryRecord *nr;
    if (!r->alloc) return 0;
    if (ncap > I_RANT_REC_MAX) ncap = I_RANT_REC_MAX;
    if (ncap <= r->cap) return 0;
    nr = (i_RantDiscoveryRecord*)r->alloc(r->alloc_user, r->recs, ncap * sizeof(i_RantDiscoveryRecord));
    if (!nr) return 0;
    r->recs = nr;
    memset(nr + old, 0, (size_t)(ncap - old) * sizeof(i_RantDiscoveryRecord));
    r->cap = (uint16_t)ncap;
    i_rant_records_free_from(r, old);
    nb = i_rant_records_buckets(r->cap);
    if (nb > r->mask + 1u){
        uint16_t *nh = (uint16_t*)r->alloc(r->alloc_user, r->heads, 2u * nb * sizeof(uint16_t));
        if (nh){ r->heads = nh; r->mask = (uint16_t)(nb - 1u); i_rant_records_rehash(r); }
    }
    return 1;
}

uint16_t i_rant_records_add(i_RantDiscoveryRecords *r, const uint8_t uuid[16]){
    i_RantDiscoveryRecord *rec; uint16_t i;
    if (r->free_head == I_RANT_REC_NONE && !i_rant_records_grow(r)) return I_RANT_REC_NONE;
    i = r->free_head;
    rec = &r->recs[i];
    r->free_head = rec->uuid_next;
    memset(rec, 0, sizeof *rec);
    memcpy(rec->uuid, uuid, 16);
    if (r->next_id == 0) r->next_id = 1;     /* 0 is RANT_SELF */
    rec->local_id = r->next_id++;
    rec->state = I_RANT_REC_ACTIVE;
    rec->slot = I_RANT_REC_NONE;
    i_rant_records_link(r, i);
    r->count++;
    return i;
}

/* Unlinks i from the chain starting at *head that next links. */
static void i_rant_records_unlink(i_RantDiscoveryRecords *r, uint16_t *head, uint16_t i, int by_id){
    while (*head != I_RANT_REC_NONE && *head != i)
        head = by_id ? &r->recs[*head].id_next : &r->recs[*head].uuid_next;
    if (*head == i) *head = by_id ? r->recs[i].id_next : r->recs[i].uuid_next;
}

void i_rant_records_remove(i_RantDiscoveryRecords *r, uint16_t i){
    i_RantDiscoveryRecord *rec = i_rant_records_at(r, i);
    if (!rec) return;
    i_rant_records_unlink(r, &r->heads[i_rant_records_uuid_bucket(r, rec->uuid)], i, 0);
    i_rant_records_unlink(r, &i_rant_records_id_heads(r)[i_rant_records_id_bucket(r, rec->local_id)], i, 1);
    rec->state = I_RANT_REC_FREE;
    rec->uuid_next = r->free_head; r->free_head = i;
    r->count--;
}

uint16_t i_rant_records_find(const i_RantDiscoveryRecords *r, const uint8_t uuid[16]){
    uint16_t i = r->heads[i_rant_records_uuid_bucket(r, uuid)];
    while (i != I_RANT_REC_NONE && memcmp(r->recs[i].uuid, uuid, 16) != 0) i = r->recs[i].uuid_next;
    return i;
}

uint16_t i_rant_records_by_id(const i_RantDiscoveryRecords *r, uint32_t id){
    uint16_t i = i_rant_records_id_heads(r)[i_rant_records_id_bucket(r, id)];
    while (i != I_RANT_REC_NONE && r->recs[i].local_id != id) i = r->recs[i].id_next;
    return i;
}
