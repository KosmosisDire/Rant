/* Segment mapping and chunks over the platform layer. Compiles to nothing without RANT_SHM. */

#include "core.h"
#include "../common/bytes.h"
#include "../common/arena.h"

#ifdef RANT_SHM
#include <string.h>
#include <stdlib.h>

size_t i_rant_shm_desc_encode(const i_RantShmDesc *d, uint8_t out[RANT_SHM_DESC_WIRE]){
    i_rant_le_w64(out,      d->segment_id);
    i_rant_le_w32(out+8,    d->chunk);
    i_rant_le_w32(out+12, d->length);
    i_rant_le_w64(out+16, d->generation);
    return RANT_SHM_DESC_WIRE;
}
int i_rant_shm_desc_decode(i_RantShmDesc *d, const uint8_t *in, size_t len){
    if (len < RANT_SHM_DESC_WIRE) return 0;
    d->segment_id = i_rant_le_r64(in);
    d->chunk      = i_rant_le_r32(in+8);
    d->length     = i_rant_le_r32(in+12);
    d->generation = i_rant_le_r64(in+16);
    return 1;
}

void i_rant_shm_seg_name(char *buf, uint64_t segment_id){
    static const char hex_digits[] = "0123456789abcdef";
    const char prefix[] = "/rant.shm."; int i, k = 0;
    while (prefix[k]){ buf[k] = prefix[k]; k++; }
    for (i=15;i>=0;i--) buf[k++] = hex_digits[(segment_id >> (4*i)) & 0xF];
    buf[k] = 0;
}

uint32_t i_rant_shm_class_bytes(uint32_t k){ return RANT_SHM_CLASS_BASE << (k*RANT_SHM_CLASS_SHIFT); }
uint32_t i_rant_shm_class_for(uint32_t len){
    uint32_t k;
    for (k=0;k<RANT_SHM_N_CLASSES;k++) if (i_rant_shm_class_bytes(k) >= len) return k;
    return RANT_SHM_N_CLASSES;     /* bigger than the top class, the caller sends inline */
}

struct i_RantShmPool {
    void    *base;          /* mapping base */
    void    *handle;        /* OS handle for detach */
    size_t   map_bytes;     /* total mapped size */
    i_RantShmSegHdr *hdr;
    uint8_t *chunks;        /* base of the chunk region */
    uint32_t chunk_bytes;
    uint32_t n_chunks;
    uint32_t stride;        /* per chunk bytes including the header */
    int      is_creator;
};

size_t i_rant_shm_state_bytes(void){ return i_rant_align_up(sizeof(struct i_RantShmPool), 16u); }

#define RANT__SHM_HDR_SZ    ((uint32_t)i_rant_align_up(sizeof(i_RantShmSegHdr), 16u))
#define RANT__SHM_CHDR_SZ ((uint32_t)i_rant_align_up(sizeof(i_RantShmChunkHdr), 16u))

static void i_rant_shm_geom(uint32_t chunk_bytes, uint32_t n_chunks,
                           uint32_t *out_stride, size_t *out_total){
    uint32_t aligned = (uint32_t)i_rant_align_up(chunk_bytes, 16u);
    uint32_t stride = RANT__SHM_CHDR_SZ + aligned;
    *out_stride = stride;
    *out_total  = (size_t)RANT__SHM_HDR_SZ + (size_t)n_chunks * stride;
}

static i_RantShmChunkHdr *i_rant_shm_chunk_hdr(struct i_RantShmPool *p, uint32_t i){
    return (i_RantShmChunkHdr*)(p->chunks + (size_t)i * p->stride);
}
static uint8_t *i_rant_shm_chunk_pay(struct i_RantShmPool *p, uint32_t i){
    return (uint8_t*)i_rant_shm_chunk_hdr(p, i) + RANT__SHM_CHDR_SZ;
}

i_RantShmPool *i_rant_shm_create(void *pool_mem, const i_RantShmConfig *cfg){
    struct i_RantShmPool *p = (struct i_RantShmPool*)pool_mem;
    uint32_t chunk_bytes = cfg->chunk_bytes ? cfg->chunk_bytes : RANT_SHM_CHUNK_BYTES;
    uint32_t n_chunks = cfg->n_chunks    ? cfg->n_chunks    : RANT_SHM_CHUNKS;
    uint32_t stride; size_t total; void *handle = NULL, *base; uint32_t i;
    if (!p || !cfg) return NULL;
    i_rant_shm_geom(chunk_bytes, n_chunks, &stride, &total);
    base = i_rant_plat_shm_create(cfg->name, total, &handle);
    if (!base) return NULL;
    memset(p, 0, sizeof *p);
    p->base = base; p->handle = handle; p->map_bytes = total;
    p->hdr = (i_RantShmSegHdr*)base;
    p->chunks = (uint8_t*)base + RANT__SHM_HDR_SZ;
    p->chunk_bytes = chunk_bytes; p->n_chunks = n_chunks; p->stride = stride; p->is_creator = 1;
    /* the segment starts zero filled: stamp the header and clear the generations */
    p->hdr->magic = RANT_SHM_MAGIC; p->hdr->version = RANT_SHM_VERSION;
    p->hdr->segment_id = cfg->segment_id; p->hdr->chunk_bytes = chunk_bytes; p->hdr->n_chunks = n_chunks;
    p->hdr->owner_pid = i_rant_plat_pid();
    i_rant_plat_host_uuid(p->hdr->owner_host);
    for (i = 0; i < n_chunks; i++){ i_RantShmChunkHdr *c = i_rant_shm_chunk_hdr(p, i); c->generation = 0; c->length = 0; }
    return p;
}

i_RantShmPool *i_rant_shm_attach(void *pool_mem, const i_RantShmConfig *cfg){
    struct i_RantShmPool *p = (struct i_RantShmPool*)pool_mem;
    uint32_t chunk_bytes, n_chunks, stride; size_t map_bytes = 0, expect;
    void *handle = NULL, *base; uint8_t ours[16];
    if (!p || !cfg) return NULL;
    /* the geometry comes from the writer's header, cfg's create only fields are ignored */
    base = i_rant_plat_shm_attach(cfg->name, &map_bytes, &handle);
    if (!base) return NULL;
    memset(p, 0, sizeof *p);
    p->base = base; p->handle = handle; p->map_bytes = map_bytes;
    p->hdr = (i_RantShmSegHdr*)base;
    i_rant_plat_host_uuid(ours);
    chunk_bytes = p->hdr->chunk_bytes; n_chunks = p->hdr->n_chunks;
    i_rant_shm_geom(chunk_bytes, n_chunks, &stride, &expect);
    /* a stale, foreign, mismatched or truncated segment: the caller falls back to UDP */
    if (p->hdr->magic != RANT_SHM_MAGIC || p->hdr->version != RANT_SHM_VERSION ||
        memcmp(p->hdr->owner_host, ours, 16) != 0 ||
        chunk_bytes == 0 || n_chunks == 0 || expect > map_bytes){
        i_rant_plat_shm_detach(base, map_bytes, handle, 0);
        return NULL;
    }
    p->chunks = (uint8_t*)base + RANT__SHM_HDR_SZ;
    p->chunk_bytes = chunk_bytes; p->n_chunks = n_chunks; p->stride = stride; p->is_creator = 0;
    return p;
}

void *i_rant_shm_chunk(i_RantShmPool *p, uint32_t chunk, uint32_t *out_cap){
    if (!p || chunk >= p->n_chunks) return NULL;
    if (out_cap) *out_cap = p->chunk_bytes;
    return i_rant_shm_chunk_pay(p, chunk);
}

void i_rant_shm_stamp(i_RantShmPool *p, uint32_t chunk, uint32_t len, i_RantShmDesc *out){
    i_RantShmChunkHdr *c;
    uint64_t generation;
    if (!p || chunk >= p->n_chunks) return;
    c = i_rant_shm_chunk_hdr(p, chunk);
    c->length = len;
    generation = c->generation + 1u;                       /* bump so a straggler sees the reuse */
    i_rant_plat_atomic_store64(&c->generation, generation);    /* release: publishes the payload */
    if (out){ out->segment_id = p->hdr->segment_id; out->chunk = chunk; out->length = len; out->generation = generation; }
}

const void *i_rant_shm_read(i_RantShmPool *p, const i_RantShmDesc *d, uint32_t *out_len){
    i_RantShmChunkHdr *c;
    if (!p || !d || d->chunk >= p->n_chunks) return NULL;
    c = i_rant_shm_chunk_hdr(p, d->chunk);
    if (i_rant_plat_atomic_load64(&c->generation) != d->generation) return NULL;    /* recycled */
    if (d->length > p->chunk_bytes) return NULL;
    if (out_len) *out_len = d->length;
    return i_rant_shm_chunk_pay(p, d->chunk);
}

int i_rant_shm_verify(i_RantShmPool *p, const i_RantShmDesc *d){
    i_RantShmChunkHdr *c;
    if (!p || !d || d->chunk >= p->n_chunks) return 0;
    c = i_rant_shm_chunk_hdr(p, d->chunk);
    return i_rant_plat_atomic_load64(&c->generation) == d->generation;
}

void i_rant_shm_detach(i_RantShmPool *p){
    if (!p || !p->base) return;
    i_rant_plat_shm_detach(p->base, p->map_bytes, p->handle, p->is_creator);
    p->base = NULL; p->handle = NULL;
}

int i_rant_shm_host_match(const uint8_t peer_host[16], const uint8_t our_host[16]){
    return memcmp(peer_host, our_host, 16) == 0;
}

#endif /* RANT_SHM */
