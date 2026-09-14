/* Packs sub blocks into one arena. With base NULL it only measures, so the sizing pass
 * and the build pass run the same code and cannot drift. */
#ifndef RAMBLE_ARENA_H
#define RAMBLE_ARENA_H

#include <stddef.h>
#include <stdint.h>

typedef struct { uint8_t *base; size_t offset; size_t cap; int oom; } i_RambleBump;

/* align must be a power of two */
static inline size_t i_ramble_align_up(size_t n, size_t align){ return (n + (align - 1)) & ~(align - 1); }

static inline void *i_ramble_bump_take(i_RambleBump *b, size_t n, size_t align){
    size_t a = i_ramble_align_up(b->offset, align);
    b->offset = a + n;
    if (b->base){
        if (b->offset > b->cap){ b->oom = 1; return NULL; }
        return b->base + a;
    }
    return NULL;   /* measure mode */
}

#endif /* RAMBLE_ARENA_H */
