/* The same host shared memory module: segment mapping and chunks over the platform
 * layer. Compiles to nothing without RAMBLE_SHM. The rules are in spec/shm.md. */
#ifndef RAMBLE_SHM_H
#define RAMBLE_SHM_H

#include <stddef.h>
#include <stdint.h>
#include "../platform/core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Compile bounds. An SBC sets these small, a workstation large. */
#ifndef RAMBLE_SHM_CHUNK_BYTES
#define RAMBLE_SHM_CHUNK_BYTES (4u*1024u*1024u)  /* default chunk, the node overrides per class */
#endif
#ifndef RAMBLE_SHM_CHUNKS
#define RAMBLE_SHM_CHUNKS 4u                     /* default chunks per segment, the node overrides */
#endif
#ifndef RAMBLE_SHM_NAME_MAX
#define RAMBLE_SHM_NAME_MAX 64u                  /* OS object name, derived from the node uuid */
#endif

/* The size class ladder: class k holds chunks of BASE << (k * SHIFT) bytes. The class
 * rides the low 3 bits of a segment id. */
#ifndef RAMBLE_SHM_CLASS_BASE
#define RAMBLE_SHM_CLASS_BASE  (64u*1024u)
#endif
#ifndef RAMBLE_SHM_CLASS_SHIFT
#define RAMBLE_SHM_CLASS_SHIFT 2u
#endif
#ifndef RAMBLE_SHM_N_CLASSES
#define RAMBLE_SHM_N_CLASSES   7u
#endif
#define RAMBLE_SHM_CLASS_MASK  0x7u

uint32_t i_ramble_shm_class_bytes(uint32_t k);      /* chunk payload bytes for class k */
uint32_t i_ramble_shm_class_for(uint32_t len);   /* the smallest fitting class, N_CLASSES if none */

/* The OS object name for a segment id, "/ramble.shm.<16 hex>", valid on POSIX and Windows. */
void i_ramble_shm_seg_name(char *buf, uint64_t segment_id);

/* The locator inside an SHM-DATA submessage. The framing supplies seqno base and count. */
typedef struct {
    uint64_t segment_id;   /* the writer's segment */
    uint32_t chunk;        /* chunk index */
    uint32_t length;       /* payload bytes */
    uint64_t generation;   /* chunk reuse counter at publish, rechecked after reading */
} i_RambleShmDesc;

#define RAMBLE_SHM_DESC_WIRE 24u   /* little endian, the SHM-DATA body */
size_t i_ramble_shm_desc_encode(const i_RambleShmDesc *d, uint8_t out[RAMBLE_SHM_DESC_WIRE]);
int    i_ramble_shm_desc_decode(i_RambleShmDesc *d, const uint8_t *in, size_t len);  /* 0 malformed */

/* Segment layout: a header, then chunks of a 16 byte aligned chunk header plus payload.
 * generation is the only cross process mutable field, written release and read acquire. */
typedef struct {
    uint32_t magic;         /* RAMBLE_SHM_MAGIC, rejects a stale or foreign mapping */
    uint32_t version;
    uint64_t segment_id;
    uint32_t chunk_bytes;
    uint32_t n_chunks;
    uint64_t owner_pid;     /* an external janitor can reclaim an orphan */
    uint8_t  owner_host[16];/* the reader confirms the same kernel */
} i_RambleShmSegHdr;

typedef struct {
    uint64_t generation;    /* bumped on each reuse, matched against the descriptor */
    uint32_t length;
    uint32_t _pad;
} i_RambleShmChunkHdr;

#define RAMBLE_SHM_MAGIC    0x4D484453u   /* 'DSHM' */
#define RAMBLE_SHM_VERSION  1u

/* One mapped segment, placed in caller memory. A node creates segments for its own
 * publishes and attaches those of the same host peers it subscribes to. */
typedef struct i_RambleShmPool i_RambleShmPool;
size_t i_ramble_shm_state_bytes(void);

typedef struct {
    char     name[RAMBLE_SHM_NAME_MAX];  /* from the writer's uuid, the reader gets it via meta */
    uint64_t segment_id;
    uint32_t chunk_bytes;              /* create only, 0 = RAMBLE_SHM_CHUNK_BYTES. attach reads it */
    uint32_t n_chunks;                 /* create only, 0 = RAMBLE_SHM_CHUNKS. attach reads it */
} i_RambleShmConfig;

/* The writer. NULL means stay on UDP. */
i_RambleShmPool *i_ramble_shm_create(void *pool_mem, const i_RambleShmConfig *cfg);
/* chunk returns the writable payload of a slot's chunk. stamp bumps the generation, sets
 * the length and fills the descriptor. The node owns the chunk to slot assignment. */
void *i_ramble_shm_chunk(i_RambleShmPool *p, uint32_t chunk, uint32_t *out_cap);
void  i_ramble_shm_stamp(i_RambleShmPool *p, uint32_t chunk, uint32_t len, i_RambleShmDesc *out);

/* The reader. attach maps an existing segment whole and validates it, NULL means fall
 * back to UDP. read resolves a descriptor, NULL when the chunk was recycled. */
i_RambleShmPool *i_ramble_shm_attach(void *pool_mem, const i_RambleShmConfig *cfg);
const void    *i_ramble_shm_read  (i_RambleShmPool *p, const i_RambleShmDesc *d, uint32_t *out_len);
/* Rechecks the generation after a one copy read. 0 means the copy may be torn, discard it. */
int            i_ramble_shm_verify(i_RambleShmPool *p, const i_RambleShmDesc *d);

void i_ramble_shm_detach(i_RambleShmPool *p);  /* unmaps, the writer also unlinks the OS object */

/* Same host is a host id match. A successful attach is the real gate. */
int i_ramble_shm_host_match(const uint8_t peer_host[16], const uint8_t our_host[16]);

#ifdef __cplusplus
}
#endif
#endif /* RAMBLE_SHM_H */
