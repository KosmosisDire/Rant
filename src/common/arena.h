/* Bump allocator shared by the layers that pack sub-blocks into one caller-provided
 * arena (transport state, node). Measure mode (base==NULL): dart_take returns NULL but
 * still advances offset, so the sizing pass and the build pass run the SAME code and
 * cannot drift. Build mode (base set): returns base + aligned offset, or sets oom and
 * returns NULL once the offset passes cap. static inline: no link symbol and no unused
 * warning in a layer that doesn't use it. The amalgamator emits this once per
 * implementation TU; the local #include is for standalone compilation of a layer. */
#ifndef DART_ARENA_H
#define DART_ARENA_H

#include <stddef.h>
#include <stdint.h>

typedef struct { uint8_t *base; size_t offset; size_t cap; int oom; } i_DartBump;

/* round n up to the next multiple of align (a power of two): names the (x+15)&~15 idiom */
static inline size_t dart_align_up(size_t n, size_t align){ return (n + (align - 1)) & ~(align - 1); }

static inline void *dart_take(i_DartBump *b, size_t n, size_t align){
    size_t a = dart_align_up(b->offset, align);
    b->offset = a + n;
    if (b->base){
        if (b->offset > b->cap){ b->oom = 1; return NULL; }
        return b->base + a;
    }
    return NULL;   /* measure mode */
}

#endif /* DART_ARENA_H */
