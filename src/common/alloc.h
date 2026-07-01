/* The allocation hook: a realloc-style callback a memory-taking core calls for growable
 * buffers (ptr NULL = alloc, size 0 = free, else resize). A runtime supplies one derived
 * from its DartAllocator (below), so the core stays memory-policy agnostic. Shared by the
 * transport core and the serializer. */
#ifndef DART_ALLOC_H
#define DART_ALLOC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>   /* memcpy on a freeable grow */

typedef void *(*DartAllocFn)(void *user, void *ptr, size_t size);

/* ===========================================================================
 * DartAllocator: a paged region allocator. Two modes and two allocation intents.
 *
 * Modes (set by the constructor):
 *   static  - one caller buffer, no growth. Overflow returns NULL. For embedded.
 *   dynamic - bump within pages; a new page is malloc'd (via an injected backing)
 *             when the current one is full. It does not touch the platform itself,
 *             so common/ stays portable and each subsystem runs standalone.
 *
 * Intents (chosen per allocation, not by size):
 *   fixed    - dart_allocator_fixed: bump into a shared page, cheapest, never freed
 *              individually. For allocate-once, live-until-reset data.
 *   freeable - dart_allocator_alloc (a DartAllocFn): its own reclaimable block, so free
 *              and resize work. For anything that grows or is released early.
 *
 * dart_allocator_reset frees EVERYTHING (both intents), so no per-allocation free is ever
 * required; free a freeable block only to reclaim it mid-run. In static mode there are no
 * pages, so both intents just bump the buffer, free is a no-op, and reset rewinds. All
 * static-inline: like arena.h, the amalgamator emits it once and a layer that does not use
 * it pays nothing. */

#ifndef DART_ALLOCATOR_PAGE
#define DART_ALLOCATOR_PAGE (64u * 1024u)   /* default shared-page size (dynamic mode) */
#endif

/* Page backing (dynamic only): allocate/grow/free whole pages, ptr NULL = alloc, size 0 =
 * free. The runtime injects dart_plat_realloc; a test injects stdlib realloc. */
typedef void *(*DartPageFn)(void *ptr, size_t size);

/* Header at the front of every page: a shared bump page, or a freeable one-allocation page. */
typedef struct i_DartPage {
    struct i_DartPage *next, *prev;
    size_t cap;    /* usable payload bytes after this header */
    size_t used;   /* shared: bump cursor; freeable: the allocation's size */
} i_DartPage;

typedef struct {
    DartPageFn  page_realloc;   /* NULL => static (one buffer, no new pages) */
    i_DartPage *shared;         /* shared bump pages (head = current); static: the buffer */
    i_DartPage *owned;          /* freeable one-allocation pages (dynamic only) */
    uint32_t    page_size;
    size_t      max_bytes;      /* dynamic runaway guard (0 = unlimited) */
    size_t      in_use, peak;
    uint64_t    alloc_calls, pages_live;
} DartAllocator;

static inline size_t i_alloc_align(size_t n){ return (n + 15u) & ~(size_t)15u; }
static inline size_t i_alloc_pow2(size_t n){                 /* round up to a power of two, >= 16 */
    size_t p = 16u;
    while (p < n){ if (p > (SIZE_MAX >> 1)) return n; p <<= 1; }
    return p;
}
/* dynamic runaway guard: 1 if `need` more bytes would breach max_bytes */
static inline int i_alloc_over(const DartAllocator *a, size_t need){
    return a->max_bytes && a->in_use + need > a->max_bytes;
}

static inline DartAllocator dart_allocator_static(void *buffer, size_t size){
    DartAllocator a;
    uint8_t *b = (uint8_t *)buffer;
    uintptr_t aligned = ((uintptr_t)b + 15u) & ~(uintptr_t)15u;   /* 16-align the buffer front */
    size_t head = (size_t)(aligned - (uintptr_t)b);
    memset(&a, 0, sizeof a);
    if (b && size >= head + sizeof(i_DartPage)){
        i_DartPage *pg = (i_DartPage *)(b + head);
        pg->next = pg->prev = NULL;
        pg->cap = size - head - sizeof(i_DartPage);
        pg->used = 0;
        a.shared = pg; a.pages_live = 1;
    }
    return a;   /* page_realloc NULL => static */
}

static inline DartAllocator dart_allocator_dynamic(DartPageFn page_realloc, uint32_t page_size){
    DartAllocator a; memset(&a, 0, sizeof a);
    a.page_realloc = page_realloc;
    a.page_size = page_size ? page_size : DART_ALLOCATOR_PAGE;
    return a;
}

/* bump `need` bytes (16-aligned) from a shared page, adding one on overflow (dynamic). */
static inline void *i_alloc_bump(DartAllocator *a, size_t need){
    i_DartPage *pg = a->shared;
    need = i_alloc_align(need);
    if (!pg || i_alloc_align(pg->used) + need > pg->cap){
        size_t psz; i_DartPage *np;
        if (!a->page_realloc || i_alloc_over(a, need)) return NULL;   /* static full, or over the guard */
        psz = a->page_size;
        if (need + sizeof(i_DartPage) > psz) psz = need + sizeof(i_DartPage);   /* oversized page */
        np = (i_DartPage *)a->page_realloc(NULL, psz);
        if (!np) return NULL;
        np->prev = NULL; np->next = a->shared; if (a->shared) a->shared->prev = np;
        np->cap = psz - sizeof(i_DartPage); np->used = 0;
        a->shared = np; a->pages_live++;
        pg = np;
    }
    pg->used = i_alloc_align(pg->used);
    { void *out = (uint8_t *)(pg + 1) + pg->used; pg->used += need; return out; }
}

static inline void *dart_allocator_fixed(DartAllocator *a, size_t size){
    void *p;
    if (!a || size == 0) return NULL;
    p = i_alloc_bump(a, size);
    if (p){ a->alloc_calls++; a->in_use += i_alloc_align(size);
            if (a->in_use > a->peak) a->peak = a->in_use; }
    return p;
}

/* a freeable block of `cap` payload bytes: its own page (dynamic) or a header+payload bumped
 * from the buffer (static, where free is a no-op). */
static inline void *i_alloc_new_owned(DartAllocator *a, size_t cap){
    i_DartPage *pg;
    if (a->page_realloc){
        if (i_alloc_over(a, cap)) return NULL;
        pg = (i_DartPage *)a->page_realloc(NULL, sizeof(i_DartPage) + cap);
        if (!pg) return NULL;
        pg->prev = NULL; pg->next = a->owned; if (a->owned) a->owned->prev = pg;
        a->owned = pg; a->pages_live++;
    } else {
        pg = (i_DartPage *)i_alloc_bump(a, sizeof(i_DartPage) + cap);
        if (!pg) return NULL;
        pg->prev = pg->next = NULL;   /* not linked; reset rewinds the buffer */
    }
    pg->cap = cap; pg->used = cap;
    a->alloc_calls++; a->in_use += cap; if (a->in_use > a->peak) a->peak = a->in_use;
    return (void *)(pg + 1);
}

static inline void i_alloc_free_owned(DartAllocator *a, void *ptr){
    i_DartPage *pg = (i_DartPage *)ptr - 1;
    a->in_use -= pg->cap;
    if (a->page_realloc){                              /* dynamic: unlink + free the page */
        if (pg->prev) pg->prev->next = pg->next; else a->owned = pg->next;
        if (pg->next) pg->next->prev = pg->prev;
        a->page_realloc(pg, 0); a->pages_live--;
    }                                                  /* static: no-op (reset reclaims it) */
}

/* The freeable allocator function: a DartAllocFn (pass &allocator as `user`).
 *   ptr NULL -> allocate    size 0 -> free (reclaims iff dynamic)    else -> resize. */
static inline void *dart_allocator_alloc(void *alloc, void *ptr, size_t size){
    DartAllocator *a = (DartAllocator *)alloc; size_t cap;
    if (!a) return NULL;
    if (size == 0){ if (ptr) i_alloc_free_owned(a, ptr); return NULL; }
    cap = a->page_realloc ? i_alloc_pow2(size) : i_alloc_align(size);   /* pow2 dynamic, tight static */
    if (!ptr) return i_alloc_new_owned(a, cap);
    {   i_DartPage *pg = (i_DartPage *)ptr - 1;
        if (cap <= pg->cap) return ptr;                          /* still fits: keep it */
        {   void *np = i_alloc_new_owned(a, cap);                   /* grow: new + copy + free old */
            if (!np) return NULL;                                /* old left intact */
            memcpy(np, ptr, pg->cap);
            i_alloc_free_owned(a, ptr);
            return np;
        }
    }
}

/* Free everything (both intents). Static: rewind the buffer (keeps it). */
static inline void dart_allocator_reset(DartAllocator *a){
    if (!a) return;
    if (a->page_realloc){
        i_DartPage *pg, *nx;
        for (pg = a->owned;  pg; pg = nx){ nx = pg->next; a->page_realloc(pg, 0); }
        for (pg = a->shared; pg; pg = nx){ nx = pg->next; a->page_realloc(pg, 0); }
        a->owned = a->shared = NULL; a->pages_live = 0;
    } else if (a->shared){
        a->shared->used = 0; a->pages_live = 1;
    }
    a->in_use = 0;
}

static inline void dart_allocator_stats(const DartAllocator *a, size_t *in_use, size_t *peak,
                                     uint64_t *alloc_calls){
    if (!a) return;
    if (in_use)      *in_use      = a->in_use;
    if (peak)        *peak        = a->peak;
    if (alloc_calls) *alloc_calls = a->alloc_calls;
}

#endif /* DART_ALLOC_H */
