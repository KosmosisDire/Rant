/* The memory model at every layer. The rules are in spec/allocation.md. */
#ifndef RANT_ALLOC_H
#define RANT_ALLOC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Growable buffer hook: ptr NULL allocates, size 0 frees, else resizes. */
typedef void *(*RantAllocFn)(void *user, void *ptr, size_t size);

#ifndef RANT_ALLOCATOR_PAGE
#define RANT_ALLOCATOR_PAGE (64u * 1024u)
#endif

/* Page backing for dynamic mode, the RantAllocFn shape without the user pointer. */
typedef void *(*RantPageFn)(void *ptr, size_t size);

/* Header in front of every page. A shared page bumps, a freeable page holds one block. */
typedef struct i_RantPage {
    struct i_RantPage *next, *prev;
    size_t cap;    /* payload bytes after this header */
    size_t used;   /* shared: the bump cursor. freeable: the block size */
} i_RantPage;

typedef struct {
    RantPageFn    page_realloc;   /* NULL means static mode */
    i_RantPage *shared;           /* bump pages, head is current. static: the buffer */
    i_RantPage *owned;            /* freeable pages, dynamic only */
    i_RantPage *free_pool;        /* freed blocks kept for reuse */
    uint32_t    page_size;
    size_t      max_bytes;      /* runaway guard, 0 is unlimited */
    size_t      in_use, pooled, peak;
    uint64_t    alloc_calls, pages_live;
} RantAllocator;

static inline size_t i_rant_allocator_align(size_t n){ return (n + 15u) & ~(size_t)15u; }
/* Rounds up to a quarter power of two class so pooled blocks recur. Exact above 4 kB. */
static inline size_t i_rant_allocator_class(size_t n){
    size_t p = 16u, q;
    if (n <= 16u) return 16u;
    if (n >= 4096u) return i_rant_allocator_align(n);
    while ((p << 1) <= n){ if (p > (SIZE_MAX >> 2)) return n; p <<= 1; }
    q = p >> 2;
    return p + ((n - p + q - 1u) / q) * q;
}
/* The pool is held memory and counts. A pool reuse allocates nothing and skips this. */
static inline int i_rant_allocator_over(const RantAllocator *a, size_t need){
    return a->max_bytes && a->in_use + a->pooled + need > a->max_bytes;
}

static inline RantAllocator rant_allocator_static(void *buffer, size_t size){
    RantAllocator a;
    uint8_t *b = (uint8_t *)buffer;
    uintptr_t aligned = ((uintptr_t)b + 15u) & ~(uintptr_t)15u;
    size_t head = (size_t)(aligned - (uintptr_t)b);
    memset(&a, 0, sizeof a);
    if (b && size >= head + sizeof(i_RantPage)){
        i_RantPage *pg = (i_RantPage *)(b + head);
        pg->next = pg->prev = NULL;
        pg->cap = size - head - sizeof(i_RantPage);
        pg->used = 0;
        a.shared = pg; a.pages_live = 1;
    }
    return a;
}

static inline RantAllocator rant_allocator_dynamic(RantPageFn page_realloc, uint32_t page_size){
    RantAllocator a; memset(&a, 0, sizeof a);
    a.page_realloc = page_realloc;
    a.page_size = page_size ? page_size : RANT_ALLOCATOR_PAGE;
    return a;
}

/* Bumps from the current shared page, or adds a page in dynamic mode. */
static inline void *i_rant_allocator_bump(RantAllocator *a, size_t need){
    i_RantPage *pg = a->shared;
    need = i_rant_allocator_align(need);
    if (!pg || i_rant_allocator_align(pg->used) + need > pg->cap){
        size_t psz, floor_sz; i_RantPage *np;
        if (!a->page_realloc || i_rant_allocator_over(a, need)) return NULL;
        psz = a->page_size;
        floor_sz = need + sizeof(i_RantPage);
        if (floor_sz > psz) psz = floor_sz;
        np = (i_RantPage *)a->page_realloc(NULL, psz);
        while (!np && psz > floor_sz){
            /* a fragmented heap may hold the bytes only in shreds: halve until a page fits */
            psz >>= 1;
            if (psz < floor_sz) psz = floor_sz;
            np = (i_RantPage *)a->page_realloc(NULL, psz);
        }
        if (!np) return NULL;
        np->prev = NULL; np->next = a->shared; if (a->shared) a->shared->prev = np;
        np->cap = psz - sizeof(i_RantPage); np->used = 0;
        a->shared = np; a->pages_live++;
        pg = np;
    }
    pg->used = i_rant_allocator_align(pg->used);
    { void *out = (uint8_t *)(pg + 1) + pg->used; pg->used += need; return out; }
}

static inline void *rant_allocator_fixed(RantAllocator *a, size_t size){
    void *p;
    if (!a || size == 0) return NULL;
    p = i_rant_allocator_bump(a, size);
    if (p){ a->alloc_calls++; a->in_use += i_rant_allocator_align(size);
            if (a->in_use > a->peak) a->peak = a->in_use; }
    return p;
}

/* A freeable block: a pooled one when it fits, else its own page, or a bump in static mode. */
static inline void *i_rant_allocator_new_owned(RantAllocator *a, size_t cap){
    i_RantPage *pg;
    {   /* the smallest pooled block within 2x, so a big block is not spent on a small ask */
        i_RantPage *it, *best = NULL;
        for (it = a->free_pool; it; it = it->next)
            if (it->cap >= cap && (!best || it->cap < best->cap)) best = it;
        if (best && best->cap - cap <= cap){
            if (best->prev) best->prev->next = best->next; else a->free_pool = best->next;
            if (best->next) best->next->prev = best->prev;
            a->pooled -= best->cap;
            if (a->page_realloc){
                best->prev = NULL; best->next = a->owned;
                if (a->owned) a->owned->prev = best;
                a->owned = best;
            } else best->prev = best->next = NULL;
            best->used = best->cap;
            a->alloc_calls++; a->in_use += best->cap;
            if (a->in_use > a->peak) a->peak = a->in_use;
            return (void *)(best + 1);
        }
    }
    if (a->page_realloc){
        if (i_rant_allocator_over(a, cap)) return NULL;
        pg = (i_RantPage *)a->page_realloc(NULL, sizeof(i_RantPage) + cap);
        if (!pg) return NULL;
        pg->prev = NULL; pg->next = a->owned; if (a->owned) a->owned->prev = pg;
        a->owned = pg; a->pages_live++;
    } else {
        pg = (i_RantPage *)i_rant_allocator_bump(a, sizeof(i_RantPage) + cap);
        if (!pg) return NULL;
        pg->prev = pg->next = NULL;   /* not linked, reset rewinds the buffer */
    }
    pg->cap = cap; pg->used = cap;
    a->alloc_calls++; a->in_use += cap; if (a->in_use > a->peak) a->peak = a->in_use;
    return (void *)(pg + 1);
}

/* A freed block goes to the pool, never back to the backing heap. Reset reclaims it. */
static inline void i_rant_allocator_free_owned(RantAllocator *a, void *ptr){
    i_RantPage *pg = (i_RantPage *)ptr - 1;
    a->in_use -= pg->cap;
    if (a->page_realloc){
        if (pg->prev) pg->prev->next = pg->next; else a->owned = pg->next;
        if (pg->next) pg->next->prev = pg->prev;
    }
    pg->prev = NULL; pg->next = a->free_pool;
    if (a->free_pool) a->free_pool->prev = pg;
    a->free_pool = pg;
    a->pooled += pg->cap;
}

/* The RantAllocFn over a RantAllocator. Pass the allocator as user. */
static inline void *rant_allocator_alloc(void *alloc, void *ptr, size_t size){
    RantAllocator *a = (RantAllocator *)alloc; size_t cap;
    if (!a) return NULL;
    if (size == 0){ if (ptr) i_rant_allocator_free_owned(a, ptr); return NULL; }
    cap = a->page_realloc ? i_rant_allocator_class(size) : i_rant_allocator_align(size);
    if (!ptr) return i_rant_allocator_new_owned(a, cap);
    {   i_RantPage *pg = (i_RantPage *)ptr - 1;
        if (cap <= pg->cap) return ptr;
        {   void *np = i_rant_allocator_new_owned(a, cap);
            if (!np) return NULL;   /* the old block stays intact */
            memcpy(np, ptr, pg->cap);
            i_rant_allocator_free_owned(a, ptr);
            return np;
        }
    }
}

/* Frees both intents and the pool. Static mode rewinds the buffer and keeps it. */
static inline void rant_allocator_reset(RantAllocator *a){
    if (!a) return;
    if (a->page_realloc){
        i_RantPage *pg, *nx;
        for (pg = a->owned;     pg; pg = nx){ nx = pg->next; a->page_realloc(pg, 0); }
        for (pg = a->free_pool; pg; pg = nx){ nx = pg->next; a->page_realloc(pg, 0); }
        for (pg = a->shared;    pg; pg = nx){ nx = pg->next; a->page_realloc(pg, 0); }
        a->owned = a->free_pool = a->shared = NULL; a->pages_live = 0;
    } else if (a->shared){
        a->shared->used = 0; a->pages_live = 1;
        a->free_pool = NULL;
    }
    a->in_use = 0; a->pooled = 0;
}

static inline void rant_allocator_stats(const RantAllocator *a, size_t *in_use, size_t *peak,
                                     uint64_t *alloc_calls){
    if (!a) return;
    if (in_use)      *in_use      = a->in_use;
    if (peak)        *peak        = a->peak;
    if (alloc_calls) *alloc_calls = a->alloc_calls;
}

#endif /* RANT_ALLOC_H */
