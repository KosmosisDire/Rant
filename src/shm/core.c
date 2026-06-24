/* dart_shm: the portable segment-mapping + chunk module behind dart_shm.h. Pure
 * over dart_plat (shm mapping, host uuid, the generation atomic); no transport or
 * node knowledge. Compiles to nothing without DART_SHM. See dart_shm.h. */

#include "core.h"
#include "../common/bytes.h"
#include "../common/arena.h"

#ifdef DART_SHM
#include <string.h>
#include <stdlib.h>

size_t dart_shm_desc_encode(const dart_shm_desc *d, uint8_t out[DART_SHM_DESC_WIRE]){
    dart_le_w64(out,    d->segment_id);
    dart_le_w32(out+8,  d->chunk);
    dart_le_w32(out+12, d->length);
    dart_le_w64(out+16, d->generation);
    return DART_SHM_DESC_WIRE;
}
int dart_shm_desc_decode(dart_shm_desc *d, const uint8_t *in, size_t len){
    if (len < DART_SHM_DESC_WIRE) return 0;
    d->segment_id = dart_le_r64(in);
    d->chunk      = dart_le_r32(in+8);
    d->length     = dart_le_r32(in+12);
    d->generation = dart_le_r64(in+16);
    return 1;
}

/* OS object name "/dart.shm.<16 hex>" -- valid on POSIX (leading /) and Windows. */
void dart_shm_seg_name(char *buf, uint64_t segment_id){
    static const char hex_digits[] = "0123456789abcdef";
    const char prefix[] = "/dart.shm."; int i, k = 0;
    while (prefix[k]){ buf[k] = prefix[k]; k++; }
    for (i=15;i>=0;i--) buf[k++] = hex_digits[(segment_id >> (4*i)) & 0xF];
    buf[k] = 0;
}

uint32_t dart_shm_class_bytes(uint32_t k){ return DART_SHM_CLASS_BASE << (k*DART_SHM_CLASS_SHIFT); }
uint32_t dart_shm_class_for(uint32_t len){
    uint32_t k;
    for (k=0;k<DART_SHM_N_CLASSES;k++) if (dart_shm_class_bytes(k) >= len) return k;
    return DART_SHM_N_CLASSES;   /* bigger than the top class -> caller sends inline */
}

struct dart_shm_pool {
    void    *base;          /* mapping base */
    void    *handle;        /* OS handle for detach */
    size_t   map_bytes;     /* total mapped size */
    dart_shm_seg_hdr *hdr;
    uint8_t *chunks;        /* base of the chunk region */
    uint32_t chunk_bytes;
    uint32_t n_chunks;
    uint32_t stride;        /* per-chunk bytes incl. header */
    int      is_creator;
};

size_t dart_shm_state_bytes(void){ return dart_align_up(sizeof(struct dart_shm_pool), 16u); }

#define DART__SHM_HDR_SZ  ((uint32_t)dart_align_up(sizeof(dart_shm_seg_hdr), 16u))
#define DART__SHM_CHDR_SZ ((uint32_t)dart_align_up(sizeof(dart_shm_chunk_hdr), 16u))

static void dart__shm_geom(uint32_t chunk_bytes, uint32_t n_chunks,
                           uint32_t *out_stride, size_t *out_total){
    uint32_t aligned = (uint32_t)dart_align_up(chunk_bytes, 16u);
    uint32_t stride = DART__SHM_CHDR_SZ + aligned;
    *out_stride = stride;
    *out_total  = (size_t)DART__SHM_HDR_SZ + (size_t)n_chunks * stride;
}

static dart_shm_chunk_hdr *dart__shm_chunk_hdr(struct dart_shm_pool *p, uint32_t i){
    return (dart_shm_chunk_hdr*)(p->chunks + (size_t)i * p->stride);
}
static uint8_t *dart__shm_chunk_pay(struct dart_shm_pool *p, uint32_t i){
    return (uint8_t*)dart__shm_chunk_hdr(p, i) + DART__SHM_CHDR_SZ;
}

dart_shm_pool *dart_shm_create(void *pool_mem, const dart_shm_config *cfg){
    struct dart_shm_pool *p = (struct dart_shm_pool*)pool_mem;
    uint32_t chunk_bytes = cfg->chunk_bytes ? cfg->chunk_bytes : DART_SHM_CHUNK_BYTES;
    uint32_t n_chunks = cfg->n_chunks    ? cfg->n_chunks    : DART_SHM_CHUNKS;
    uint32_t stride; size_t total; void *handle = NULL, *base; uint32_t i;
    if (!p || !cfg) return NULL;
    dart__shm_geom(chunk_bytes, n_chunks, &stride, &total);
    base = dart_plat_shm_create(cfg->name, total, &handle);
    if (!base) return NULL;
    memset(p, 0, sizeof *p);
    p->base = base; p->handle = handle; p->map_bytes = total;
    p->hdr = (dart_shm_seg_hdr*)base;
    p->chunks = (uint8_t*)base + DART__SHM_HDR_SZ;
    p->chunk_bytes = chunk_bytes; p->n_chunks = n_chunks; p->stride = stride; p->is_creator = 1;
    /* the segment starts zero-filled; stamp the header and clear generations */
    p->hdr->magic = DART_SHM_MAGIC; p->hdr->version = DART_SHM_VERSION;
    p->hdr->segment_id = cfg->segment_id; p->hdr->chunk_bytes = chunk_bytes; p->hdr->n_chunks = n_chunks;
    p->hdr->owner_pid = dart_plat_pid();
    dart_plat_host_uuid(p->hdr->owner_host);
    for (i = 0; i < n_chunks; i++){ dart_shm_chunk_hdr *c = dart__shm_chunk_hdr(p, i); c->generation = 0; c->length = 0; }
    return p;
}

dart_shm_pool *dart_shm_attach(void *pool_mem, const dart_shm_config *cfg){
    struct dart_shm_pool *p = (struct dart_shm_pool*)pool_mem;
    uint32_t chunk_bytes, n_chunks, stride; size_t map_bytes = 0, expect;
    void *handle = NULL, *base; uint8_t ours[16];
    if (!p || !cfg) return NULL;
    /* map the whole OS object; its geometry (chunk_bytes/n_chunks) comes from the
       header the writer stamped, so the reader needs to know nothing up front --
       cfg's chunk_bytes/n_chunks are create-only. */
    base = dart_plat_shm_attach(cfg->name, &map_bytes, &handle);
    if (!base) return NULL;
    memset(p, 0, sizeof *p);
    p->base = base; p->handle = handle; p->map_bytes = map_bytes;
    p->hdr = (dart_shm_seg_hdr*)base;
    dart_plat_host_uuid(ours);
    chunk_bytes = p->hdr->chunk_bytes; n_chunks = p->hdr->n_chunks;
    dart__shm_geom(chunk_bytes, n_chunks, &stride, &expect);
    /* reject a stale/foreign/mismatched/truncated segment -> caller falls back to UDP */
    if (p->hdr->magic != DART_SHM_MAGIC || p->hdr->version != DART_SHM_VERSION ||
        memcmp(p->hdr->owner_host, ours, 16) != 0 ||
        chunk_bytes == 0 || n_chunks == 0 || expect > map_bytes){
        dart_plat_shm_detach(base, map_bytes, handle, 0);
        return NULL;
    }
    p->chunks = (uint8_t*)base + DART__SHM_HDR_SZ;
    p->chunk_bytes = chunk_bytes; p->n_chunks = n_chunks; p->stride = stride; p->is_creator = 0;
    return p;
}

void *dart_shm_chunk(dart_shm_pool *p, uint32_t chunk, uint32_t *out_cap){
    if (!p || chunk >= p->n_chunks) return NULL;
    if (out_cap) *out_cap = p->chunk_bytes;
    return dart__shm_chunk_pay(p, chunk);
}

void dart_shm_stamp(dart_shm_pool *p, uint32_t chunk, uint32_t len, dart_shm_desc *out){
    dart_shm_chunk_hdr *c;
    uint64_t generation;
    if (!p || chunk >= p->n_chunks) return;
    c = dart__shm_chunk_hdr(p, chunk);
    c->length = len;
    generation = c->generation + 1u;                       /* bump so a straggler sees the reuse */
    dart_plat_atomic_store64(&c->generation, generation);  /* release: publishes the payload writes */
    if (out){ out->segment_id = p->hdr->segment_id; out->chunk = chunk; out->length = len; out->generation = generation; }
}

const void *dart_shm_read(dart_shm_pool *p, const dart_shm_desc *d, uint32_t *out_len){
    dart_shm_chunk_hdr *c;
    if (!p || !d || d->chunk >= p->n_chunks) return NULL;
    c = dart__shm_chunk_hdr(p, d->chunk);
    if (dart_plat_atomic_load64(&c->generation) != d->generation) return NULL;  /* recycled */
    if (d->length > p->chunk_bytes) return NULL;
    if (out_len) *out_len = d->length;
    return dart__shm_chunk_pay(p, d->chunk);
}

int dart_shm_verify(dart_shm_pool *p, const dart_shm_desc *d){
    dart_shm_chunk_hdr *c;
    if (!p || !d || d->chunk >= p->n_chunks) return 0;
    c = dart__shm_chunk_hdr(p, d->chunk);
    return dart_plat_atomic_load64(&c->generation) == d->generation;
}

void dart_shm_detach(dart_shm_pool *p){
    if (!p || !p->base) return;
    dart_plat_shm_detach(p->base, p->map_bytes, p->handle, p->is_creator);
    p->base = NULL; p->handle = NULL;
}

int dart_shm_host_match(const uint8_t peer_host[16], const uint8_t our_host[16]){
    return memcmp(peer_host, our_host, 16) == 0;
}

#endif /* DART_SHM */
