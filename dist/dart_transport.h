/* GENERATED single-header build. DO NOT EDIT.
 * DART = Discovery And Realtime Transport. Amalgamated from src/ by the CMake
 * build (tools/pack.cmake). Edit the split sources in src/ and rebuild (or run
 * tools/pack.cmake) to regenerate. See the flag scheme in tools/pack.cmake.
 */
#if defined(__GNUC__)   /* let the section markers below fold quietly */
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif
#if defined(DART_TRANSPORT_IMPLEMENTATION) && !defined(DART_TRANSPORT_SANS_IO) && !defined(_WIN32)
  #ifndef _POSIX_C_SOURCE
  #define _POSIX_C_SOURCE 200809L
  #endif
  #ifndef _DEFAULT_SOURCE
  #define _DEFAULT_SOURCE 1
  #endif
  #ifndef _DARWIN_C_SOURCE
  #define _DARWIN_C_SOURCE 1   /* Darwin hides its own extensions under _POSIX_C_SOURCE without this */
  #endif
#endif

#ifndef DART_TRANSPORT_SANS_IO
  #if defined(DART_TRANSPORT_IMPLEMENTATION) && !defined(DART_DISCOVERY_IMPLEMENTATION)
  #define DART_DISCOVERY_IMPLEMENTATION
  #endif
  #include "dart_discovery.h"   /* discovery: needed by the node runtime */
#endif

#pragma region common/api.h
/* The linkage of every public entry point. spec/build.md explains the three builds. */
#ifndef DART_API_H
#define DART_API_H

/* Default: the single header build, where the caller compiles DART into its own binary
 * and needs no decoration. DART_BUILD_SHARED builds the shared library, DART_LINK_SHARED
 * consumes one. */
#if defined(DART_BUILD_SHARED)
  #if defined(_WIN32)
    #define DART_API __declspec(dllexport)
  #else
    #define DART_API __attribute__((visibility("default")))
  #endif
#elif defined(DART_LINK_SHARED) && defined(_WIN32)
  #define DART_API __declspec(dllimport)
#else
  #define DART_API
#endif

#endif /* DART_API_H */
#pragma endregion
#pragma region common/string.h
/* The two view types. Neither owns, copies or implies a NUL terminator. */
#ifndef DART_STRING_H
#define DART_STRING_H

#include <stddef.h>
#include <stdint.h>

/* A read only run of bytes. */
typedef struct {
    const uint8_t *data;
    size_t         len;
} DartBytes;

/* A run of string bytes with no NUL terminator. Never use %s or strlen on it. */
typedef struct {
    const char *data;
    size_t      len;
} DartString;

static inline DartBytes dart_bytes(const void *data, size_t len){
    DartBytes b; b.data = (const uint8_t *)data; b.len = len; return b;
}
static inline DartString dart_string(const char *data, size_t len){
    DartString s; s.data = data; s.len = len; return s;
}
/* From a C string. NULL gives an empty string. */
static inline DartString dart_cstr(const char *s){
    DartString r; size_t n = 0;
    if (s) while (s[n]) n++;
    r.data = s; r.len = n; return r;
}
static inline int dart_string_eq(DartString a, DartString b){
    size_t i;
    if (a.len != b.len) return 0;
    for (i = 0; i < a.len; i++) if (a.data[i] != b.data[i]) return 0;
    return 1;
}

#endif /* DART_STRING_H */
#pragma endregion
#pragma region common/alloc.h
/* The memory model at every layer. The rules are in spec/allocation.md. */
#ifndef DART_ALLOC_H
#define DART_ALLOC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Growable buffer hook: ptr NULL allocates, size 0 frees, else resizes. */
typedef void *(*DartAllocFn)(void *user, void *ptr, size_t size);

#ifndef DART_ALLOCATOR_PAGE
#define DART_ALLOCATOR_PAGE (64u * 1024u)
#endif

/* Page backing for dynamic mode, the DartAllocFn shape without the user pointer. */
typedef void *(*DartPageFn)(void *ptr, size_t size);

/* Header in front of every page. A shared page bumps, a freeable page holds one block. */
typedef struct i_DartPage {
    struct i_DartPage *next, *prev;
    size_t cap;    /* payload bytes after this header */
    size_t used;   /* shared: the bump cursor. freeable: the block size */
} i_DartPage;

typedef struct {
    DartPageFn  page_realloc;   /* NULL means static mode */
    i_DartPage *shared;         /* bump pages, head is current. static: the buffer */
    i_DartPage *owned;          /* freeable pages, dynamic only */
    i_DartPage *free_pool;      /* freed blocks kept for reuse */
    uint32_t    page_size;
    size_t      max_bytes;      /* runaway guard, 0 is unlimited */
    size_t      in_use, pooled, peak;
    uint64_t    alloc_calls, pages_live;
} DartAllocator;

static inline size_t i_dart_allocator_align(size_t n){ return (n + 15u) & ~(size_t)15u; }
/* Rounds up to a quarter power of two class so pooled blocks recur. Exact above 4 kB. */
static inline size_t i_dart_allocator_class(size_t n){
    size_t p = 16u, q;
    if (n <= 16u) return 16u;
    if (n >= 4096u) return i_dart_allocator_align(n);
    while ((p << 1) <= n){ if (p > (SIZE_MAX >> 2)) return n; p <<= 1; }
    q = p >> 2;
    return p + ((n - p + q - 1u) / q) * q;
}
/* The pool is held memory and counts. A pool reuse allocates nothing and skips this. */
static inline int i_dart_allocator_over(const DartAllocator *a, size_t need){
    return a->max_bytes && a->in_use + a->pooled + need > a->max_bytes;
}

static inline DartAllocator dart_allocator_static(void *buffer, size_t size){
    DartAllocator a;
    uint8_t *b = (uint8_t *)buffer;
    uintptr_t aligned = ((uintptr_t)b + 15u) & ~(uintptr_t)15u;
    size_t head = (size_t)(aligned - (uintptr_t)b);
    memset(&a, 0, sizeof a);
    if (b && size >= head + sizeof(i_DartPage)){
        i_DartPage *pg = (i_DartPage *)(b + head);
        pg->next = pg->prev = NULL;
        pg->cap = size - head - sizeof(i_DartPage);
        pg->used = 0;
        a.shared = pg; a.pages_live = 1;
    }
    return a;
}

static inline DartAllocator dart_allocator_dynamic(DartPageFn page_realloc, uint32_t page_size){
    DartAllocator a; memset(&a, 0, sizeof a);
    a.page_realloc = page_realloc;
    a.page_size = page_size ? page_size : DART_ALLOCATOR_PAGE;
    return a;
}

/* Bumps from the current shared page, or adds a page in dynamic mode. */
static inline void *i_dart_allocator_bump(DartAllocator *a, size_t need){
    i_DartPage *pg = a->shared;
    need = i_dart_allocator_align(need);
    if (!pg || i_dart_allocator_align(pg->used) + need > pg->cap){
        size_t psz, floor_sz; i_DartPage *np;
        if (!a->page_realloc || i_dart_allocator_over(a, need)) return NULL;
        psz = a->page_size;
        floor_sz = need + sizeof(i_DartPage);
        if (floor_sz > psz) psz = floor_sz;
        np = (i_DartPage *)a->page_realloc(NULL, psz);
        while (!np && psz > floor_sz){
            /* a fragmented heap may hold the bytes only in shreds: halve until a page fits */
            psz >>= 1;
            if (psz < floor_sz) psz = floor_sz;
            np = (i_DartPage *)a->page_realloc(NULL, psz);
        }
        if (!np) return NULL;
        np->prev = NULL; np->next = a->shared; if (a->shared) a->shared->prev = np;
        np->cap = psz - sizeof(i_DartPage); np->used = 0;
        a->shared = np; a->pages_live++;
        pg = np;
    }
    pg->used = i_dart_allocator_align(pg->used);
    { void *out = (uint8_t *)(pg + 1) + pg->used; pg->used += need; return out; }
}

static inline void *dart_allocator_fixed(DartAllocator *a, size_t size){
    void *p;
    if (!a || size == 0) return NULL;
    p = i_dart_allocator_bump(a, size);
    if (p){ a->alloc_calls++; a->in_use += i_dart_allocator_align(size);
            if (a->in_use > a->peak) a->peak = a->in_use; }
    return p;
}

/* A freeable block: a pooled one when it fits, else its own page, or a bump in static mode. */
static inline void *i_dart_allocator_new_owned(DartAllocator *a, size_t cap){
    i_DartPage *pg;
    {   /* the smallest pooled block within 2x, so a big block is not spent on a small ask */
        i_DartPage *it, *best = NULL;
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
        if (i_dart_allocator_over(a, cap)) return NULL;
        pg = (i_DartPage *)a->page_realloc(NULL, sizeof(i_DartPage) + cap);
        if (!pg) return NULL;
        pg->prev = NULL; pg->next = a->owned; if (a->owned) a->owned->prev = pg;
        a->owned = pg; a->pages_live++;
    } else {
        pg = (i_DartPage *)i_dart_allocator_bump(a, sizeof(i_DartPage) + cap);
        if (!pg) return NULL;
        pg->prev = pg->next = NULL;   /* not linked, reset rewinds the buffer */
    }
    pg->cap = cap; pg->used = cap;
    a->alloc_calls++; a->in_use += cap; if (a->in_use > a->peak) a->peak = a->in_use;
    return (void *)(pg + 1);
}

/* A freed block goes to the pool, never back to the backing heap. Reset reclaims it. */
static inline void i_dart_allocator_free_owned(DartAllocator *a, void *ptr){
    i_DartPage *pg = (i_DartPage *)ptr - 1;
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

/* The DartAllocFn over a DartAllocator. Pass the allocator as user. */
static inline void *dart_allocator_alloc(void *alloc, void *ptr, size_t size){
    DartAllocator *a = (DartAllocator *)alloc; size_t cap;
    if (!a) return NULL;
    if (size == 0){ if (ptr) i_dart_allocator_free_owned(a, ptr); return NULL; }
    cap = a->page_realloc ? i_dart_allocator_class(size) : i_dart_allocator_align(size);
    if (!ptr) return i_dart_allocator_new_owned(a, cap);
    {   i_DartPage *pg = (i_DartPage *)ptr - 1;
        if (cap <= pg->cap) return ptr;
        {   void *np = i_dart_allocator_new_owned(a, cap);
            if (!np) return NULL;   /* the old block stays intact */
            memcpy(np, ptr, pg->cap);
            i_dart_allocator_free_owned(a, ptr);
            return np;
        }
    }
}

/* Frees both intents and the pool. Static mode rewinds the buffer and keeps it. */
static inline void dart_allocator_reset(DartAllocator *a){
    if (!a) return;
    if (a->page_realloc){
        i_DartPage *pg, *nx;
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

static inline void dart_allocator_stats(const DartAllocator *a, size_t *in_use, size_t *peak,
                                     uint64_t *alloc_calls){
    if (!a) return;
    if (in_use)      *in_use      = a->in_use;
    if (peak)        *peak        = a->peak;
    if (alloc_calls) *alloc_calls = a->alloc_calls;
}

#endif /* DART_ALLOC_H */
#pragma endregion
#pragma region transport/core.h
/* The sans-IO transport core. Feed it datagrams, the clock and a peer set, it returns
 * datagrams to send and delivers messages. The rules are in spec/transport.md. */
#ifndef DART_TRANSPORT_H
#define DART_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DART_SHM detection. This block mirrors platform/core.h exactly. Edit both together. */
#if !defined(DART_SHM) && !defined(DART_NO_SHM)
  #if defined(_WIN32) || defined(__linux__) || defined(__APPLE__) || \
      defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
      defined(__DragonFly__)
    #define DART_SHM
  #endif
#endif
#if defined(DART_SHM) && defined(DART_NO_SHM)
  #undef DART_SHM              /* both set, the opt out wins */
#endif

#ifndef DART_FRAG_SIZE
#define DART_FRAG_SIZE 1350u          /* default message bytes per fragment */
#endif
/* A node fragments every send with one size, set at init and advertised by discovery.
 * MIN and MAX bound any node's size, and MAX sizes the datagram buffers. */
#ifndef DART_FRAG_SIZE_MAX
#define DART_FRAG_SIZE_MAX DART_FRAG_SIZE
#endif
#ifndef DART_FRAG_SIZE_MIN
#define DART_FRAG_SIZE_MIN DART_FRAG_SIZE
#endif
#define DART_DGRAM_MAX (DART_FRAG_SIZE_MAX + 40u)   /* plus the largest header */

#ifdef DART_SHM
#define DART_SHM_DESC_BYTES 24u   /* the descriptor on the wire, equals DART_SHM_DESC_WIRE */
#endif

#ifndef DART_TOPIC_NAME_MAX
#define DART_TOPIC_NAME_MAX 64u          /* topic name bytes on the wire */
#endif

#ifndef DART_NODE_NAME_MAX
#define DART_NODE_NAME_MAX 32u           /* node name bytes in the announce */
#endif

/* The source stamp in front of every sample of a stamped topic, taken once at commit so
 * repair, replay and shared memory carry the original. DartQos.no_timestamp opts out. */
#define DART_TIMESTAMP_BYTES 8u
/* A microsecond wall clock needs 51 bits, so the source stamp's top bit is free to mark
 * that a capture slot follows it. Per message, and costs nothing unset. See spec/node.md */
#define DART_CAPTURE_BYTES   8u
#define DART_STAMP_CAPTURE   0x8000000000000000ull
#define DART_STAMP_MASK      0x7FFFFFFFFFFFFFFFull

typedef enum { DART_BEST_EFFORT = 0, DART_RELIABLE = 1 } DartReliability;
/* DART_INACTIVE is declared but off. Resources stay allocated and set_role flips it. */
typedef enum { DART_PUBSUB = 0, DART_PUB_ONLY = 1, DART_SUB_ONLY = 2,
               DART_INACTIVE = 3 } DartRole;
/* Shared by every matching, announce and reflection walk, so a flipped test is impossible. */
static inline int dart_role_pubs(uint8_t role){ return role == DART_PUBSUB || role == DART_PUB_ONLY; }
static inline int dart_role_subs(uint8_t role){ return role == DART_PUBSUB || role == DART_SUB_ONLY; }

/* What a topic carries. A same name under a different kind is a disjoint entity and the
 * pairing is refused. The kind rides the interest flags and is immutable per topic. */
typedef enum {
    DART_KIND_TOPIC    = 0,   /* plain pub sub */
    DART_KIND_FUNC_REQ = 1,   /* function requests, the caller pubs */
    DART_KIND_FUNC_RSP = 2,   /* function responses, the provider pubs, directed */
    DART_KIND_VARIABLE = 3,   /* variable values, the owner pubs */
    DART_KIND_VAR_SET  = 4,   /* variable sets, writers pub, the owner subs */
    DART_KIND_TASK_REQ = 5,   /* task requests and cancel ops, callers pub */
    DART_KIND_TASK_PRG = 6,   /* task progress, the provider pubs, broadcast */
    DART_KIND_TASK_RSP = 7    /* task responses, the provider pubs, directed */
} DartTopicKind;

/* Immutable per topic facts served in the detail exchange and cached per (peer, index).
 * NO_TIMESTAMP is derived from the qos, the rest come from DartTopicDef.attrs. */
#define DART_ATTR_NO_TIMESTAMP 0x01u /* the publisher sends no source stamp */
#define DART_ATTR_MULTI        0x02u /* authority kinds: duplicate authority is intended */
#define DART_ATTR_FORCEABLE    0x04u /* variable values: the owner permits force */
#define DART_ATTR_CANCELLABLE  0x08u /* task requests: the provider honors cancel */
#define DART_ATTR_EXCLUSIVE    0x10u /* task requests: declared serialization */

/* Every field except reliability is zero means default. docs/topics.md explains each. */
typedef struct {
    DartReliability reliability;
    uint16_t keep_last;          /* messages retained. 0 = 1, or 10 on a reliable topic */
    uint16_t catch_up;           /* messages a new subscriber gets at once. 0 = future only */
    uint32_t max_message_bytes;  /* a size hint that pins the SHM class, never a cap */
    uint32_t heartbeat_us;       /* reliable idle publisher ping. 0 = 250 ms */
    uint32_t repair_delay_us;    /* the reliable re ask bound, 0 = adaptive from the round trip */
    uint32_t backpressure_wait_us;/* reliable send pause for a slow subscriber. 0 = none */
    uint32_t shm_max_bytes;      /* pin the topic to one SHM class. 0 = per message */
    uint32_t queue_bytes;        /* the consumer queue capacity, 0 = lazy up to DART_QUEUE_CAP */
    uint16_t max_rate_hz;        /* subscriber side, best effort: a delivery cap per publisher */
    uint8_t  no_timestamp;       /* publisher side: no source stamp, receivers see written_us 0 */
} DartQos;

/* The name is the cross peer identity. The local handle is the index in topics[]. */
typedef struct {
    const char *name;  /* required, the same on every node, at most DART_TOPIC_NAME_MAX */
    DartQos   qos;
    uint8_t  role;     /* DartRole, 0 = pub and sub */
    uint8_t  kind;     /* DartTopicKind, the patterns layer sets the rest */
    uint8_t  attrs;    /* DART_ATTR_* declared by the definer. NO_TIMESTAMP comes from the qos */
    uint8_t  prefix_bytes; /* pattern header bytes ahead of the payload, split off on receive */
    uint8_t  directed; /* 1 = point to point sends. Other lanes skip the seqno with no MSG_LOST */
} DartTopicDef;

/* Return 0 delivered. Nonzero refuses: a reliable subscriber parks the sample with no
 * ack, so the publisher's flow control carries the backpressure. Best effort drops it. */
typedef int (*DartMessageFn)(void *user, uint16_t topic_index, uint32_t from_peer, DartBytes data);

#ifdef DART_SHM
/* SHM delivery hands the node the descriptor. 1 delivered, 0 unresolvable (the reliability
 * layer repairs or skips), -1 refused (parked like an inline refusal). */
typedef int (*i_DartShmMsgFn)(void *user, uint16_t topic_index, uint32_t from_peer,
                             const uint8_t *desc);
#endif

/* The transport's own events. The node maps them into its DartEvent. */
typedef enum {
    DART_TRANSPORT_MSG_LOST,        /* seqnos skipped: .topic, .peer, .lost_first, .lost_count */
    DART_TRANSPORT_MSG_TOO_BIG,     /* a received message could not be buffered, .too_big_bytes */
    DART_TRANSPORT_NAME_COLLISION,  /* a peer's name hashes to ours but differs, .identity */
    DART_TRANSPORT_QOS_INCOMPATIBLE,/* a reliable subscriber refused a best effort publisher */
    DART_TRANSPORT_SCHEMA_MISMATCH, /* the schema_check hook refused a direction, .peer_is_pub */
    DART_TRANSPORT_INTEREST_OVERFLOW,/* a peer's index map failed to allocate: .lost_count */
    DART_TRANSPORT_META_TRUNCATED_INTEREST, /* our overlay overflowed, peers see no topics */
    DART_TRANSPORT_META_TRUNCATED_SCHEMA,   /* retired, kept so binding enums stay aligned */
    DART_TRANSPORT_KIND_MISMATCH    /* a verified peer advertises the name under another kind */
} DartTransportEventKind;

/* Read only the fields named for the kind. The topic name comes from dart_transport_topic_name. */
typedef struct {
    DartTransportEventKind kind;
    void       *user;          /* DartConfig.user */
    uint32_t   peer;           /* 0 = none */
    uint16_t   topic;        /* local topic handle */
    uint64_t   lost_first;     /* MSG_LOST: first skipped seqno */
    uint64_t   lost_count;     /* MSG_LOST: seqnos skipped, fragments not messages */
    uint64_t   too_big_bytes;  /* MSG_TOO_BIG: size of the dropped message */
    uint64_t   identity;       /* NAME_COLLISION: the colliding identity */
    uint8_t    peer_is_pub;    /* SCHEMA_MISMATCH: the refused direction, 1 = their publisher */
} DartTransportEvent;
typedef void (*DartTransportEventFn)(const DartTransportEvent *ev);

/* Largest message the wire can carry, 65535 fragments. */
#define DART_MESSAGE_MAX (65535u * DART_FRAG_SIZE_MAX)

/* DartConfig.allocator is required. Every growable buffer sizes through it, so memory
 * scales with traffic and matches. Static mode passes dart_allocator_alloc over a static one. */

/* topics set: defined at init. topics NULL: n_topics reserved slots, all INACTIVE, filled
 * later by dart_transport_topic_define. */
typedef struct {
    const DartTopicDef *topics;     /* NULL = reserve mode */
    uint16_t              n_topics;   /* defined count, or the reserved capacity */
    uint16_t              max_peers;
    uint16_t              frag_size; /* our fragment size, 0 = DART_FRAG_SIZE, clamped */
    DartMessageFn         on_message;
#ifdef DART_SHM
    i_DartShmMsgFn         on_shm;     /* SHM-DATA delivery, the node resolves the descriptor */
#endif
    DartTransportEventFn  on_event;   /* optional */
    DartAllocFn           allocator;  /* required, init fails without it */
    /* Optional schema gate at detail intake, once per direction. peer_is_pub 1 gates the
     * read side. Return 1 to allow. Verdicts are cached per (peer, index). */
    int                 (*schema_check)(void *user, uint32_t peer, uint16_t topic_index,
                                        int peer_is_pub, uint64_t schema_hash,
                                        DartBytes schema_wire);
    /* Optional wall clock, UTC microseconds, stamped at commit. NULL still frames the 8
     * bytes as 0 on a stamped topic. Never the monotonic clock, the stamp crosses hosts. */
    uint64_t            (*source_time)(void *user);
    void                 *user;
} DartConfig;

typedef struct DartTransportState DartTransportState;

DART_API size_t    dart_transport_required_memory(const DartConfig *cfg);
DART_API DartTransportState *dart_transport_init(void *mem, size_t mem_size, const DartConfig *cfg);
/* Relocates a live transport, re striding its tables and carrying reliability state. The
 * caller frees the old arena block but must not destroy old. NULL leaves old intact. */
DART_API DartTransportState *dart_transport_migrate(DartTransportState *old, void *new_mem, size_t new_cap,
                                 uint16_t new_max_peers, uint16_t new_n_topics);
/* Frees every hook allocation. The arena stays the caller's. */
DART_API void      dart_transport_destroy(DartTransportState *st);

/* The FNV-1a identity of a name. */
DART_API uint64_t  dart_topic_id(const char *name);
DART_API uint64_t  dart_topic_identity(const DartTopicDef *def);   /* dart_topic_id(def->name) */

/* 0 to DART_FRAG_SIZE, then clamp to [MIN, MAX]. Shared with the node's announce. */
DART_API uint16_t  dart_clamp_frag(uint16_t frag_size);

/* Our fragment size. The node uses it as the SHM cutoff. */
DART_API uint16_t  dart_transport_frag(DartTransportState *st);

/* A new peer matches nothing until apply_peer_interest. peer_frag is its advertised
 * fragment size, 0 = DART_FRAG_SIZE, clamped to [MIN, MAX]. */
DART_API void      dart_transport_peer_add   (DartTransportState *st, uint32_t peer_id, uint16_t peer_frag);
DART_API void      dart_transport_peer_remove(DartTransportState *st, uint32_t peer_id);

/* A silent peer goes DORMANT: out of flow control, proxies and positions kept. resume re
 * includes it and re reports reader positions. Both no ops for an unknown peer. */
DART_API void      dart_transport_peer_dormant(DartTransportState *st, uint32_t peer_id);
DART_API void      dart_transport_peer_resume (DartTransportState *st, uint32_t peer_id);
/* A peer's blob may arrive after first contact. Clamped, a no op for an unknown peer. */
DART_API void      dart_transport_peer_set_frag(DartTransportState *st, uint32_t peer_id, uint16_t peer_frag);

/* The interest exchange. The list is hash only and positional, a hash overlap only
 * nominates, and the detail exchange below verifies. See spec/interest.md. */
DART_API size_t    dart_interest_max(uint16_t n_topics);
DART_API size_t    dart_transport_build_interest(DartTransportState *st, void *out, size_t cap);
DART_API void      dart_transport_apply_peer_interest(DartTransportState *st, uint32_t peer_id, DartBytes blob);

/* The announce overlay: a version prefix (fragment size, SHM capability and host) plus
 * the interest list, or the INTEREST_EXTERNAL bootstrap when it does not fit one datagram. */

/* One topic's schema advertisement, served by the detail responder. hash 0 = none. */
typedef struct {
    uint64_t  hash;
    DartBytes wire;
} DartMetaSchema;

/* Worst case overlay bytes for n_topics, capped to one datagram. Sizes discovery's meta_cap. */
DART_API uint16_t  dart_meta_cap(uint16_t n_topics);
/* Exact bytes the next inline build emits, so a caller sizes to content. */
DART_API uint16_t  dart_transport_meta_size(DartTransportState *st);
DART_API uint16_t  dart_transport_meta_bootstrap_size(void);
/* Builds the overlay. interest_external writes the bootstrap only. The node inlines while
 * the whole announce fits one datagram. Returns the bytes. */
DART_API uint16_t  dart_transport_meta_build(DartTransportState *st, uint8_t *out, uint16_t cap,
                                uint16_t frag_size, int shm_capable, const uint8_t host[16],
                                int interest_external);
/* 0 if malformed. */
DART_API uint16_t  dart_meta_frag(DartBytes meta);
/* 1 = the peer serves its interest through paging. */
DART_API int       dart_meta_interest_external(DartBytes meta);
/* {NULL, 0} if absent or external, so a bootstrap never reads as an empty interest list. */
DART_API DartBytes dart_meta_interest(DartBytes meta);

/* One advertised direction of a topic. A PUBSUB topic yields twice, pub first. */
typedef struct {
    uint16_t    index;      /* the advertiser's topic index */
    uint8_t     role;       /* the advertiser's DartRole */
    uint8_t     is_pub;     /* 1 = the publish direction, 0 = subscribe */
    uint8_t     reliable;   /* offered or requested reliability */
    uint8_t     kind;       /* the advertiser's DartTopicKind */
    uint32_t    hash;       /* low 32 bits of the identity */
} DartTopicEntry;

/* Zero initialize, then call until 0. Internal walk state. */
typedef struct {
    uint32_t off;        /* byte offset of the next entry */
    uint16_t left;       /* entries still to walk */
    uint16_t index;      /* position of the next entry */
    uint8_t  phase;      /* 1 = the current entry's pub direction was yielded */
    uint8_t  started;    /* 0 until the first call parses the header */
} DartInterestIter;

/* Walks one direction at a time and stops at the end or at a malformed blob. interest_next
 * walks a bare blob, meta_interest_next the one inside an overlay. */
DART_API int       dart_interest_next(DartBytes interest, DartInterestIter *it, DartTopicEntry *out);
DART_API int       dart_meta_interest_next(DartBytes meta, DartInterestIter *it, DartTopicEntry *out);
#ifdef DART_SHM
/* 1 and host filled when the peer is SHM capable. */
DART_API int       dart_meta_shm(DartBytes meta, uint8_t host[16]);
#endif

/* The pairwise detail exchange, uDTL. The responder is stateless and answers the
 * request's source. The layout is in spec/interest.md. */
#define DART_DETAIL_REQ    1
#define DART_DETAIL_RESP   2
#define DART_INTEREST_REQ  3
#define DART_INTEREST_RESP 4

/* The peer's index plus our schema hash for it, 0 = none. */
typedef struct {
    uint16_t index;
    uint64_t schema_hash;
} DartDetailWant;

/* Safe on any buffer. kind is 0 for anything that is not a well formed uDTL header. */
DART_API int       dart_detail_kind(DartBytes dgram);
DART_API uint16_t  dart_detail_domain(DartBytes dgram);
DART_API uint32_t  dart_detail_meta_version(DartBytes dgram);

/* Interest paging carries byte ranges of the build_interest blob. The requester keeps one
 * cursor per peer and a changed version restarts it. See spec/interest.md. */
#define DART_INTEREST_RESP_HEAD 24u   /* the family header plus total, offset and chunk_len */

/* Exact bytes of our current interest blob, the total_len paging serves. */
DART_API uint32_t  dart_transport_interest_size(DartTransportState *st);
/* 18 bytes, or 0 if cap is too small. peer_meta_version is advisory. */
DART_API size_t    dart_interest_req_build(uint16_t domain, uint32_t peer_meta_version,
                                uint32_t offset, void *out, size_t cap);
/* 1 and *offset for a well formed INTEREST_REQ, else 0. */
DART_API int       dart_interest_req_offset(DartBytes dgram, uint32_t *offset);
/* Writes one page header. The caller lays the chunk right after it. */
DART_API size_t    dart_interest_resp_head(uint16_t domain, uint32_t meta_version, uint32_t total_len,
                                uint32_t offset, uint16_t chunk_len, void *out, size_t cap);
/* 1 and the fields, chunk a bounds checked view into dgram, else 0. */
DART_API int       dart_interest_resp_parse(DartBytes dgram, uint32_t *total_len, uint32_t *offset,
                                DartBytes *chunk);

/* 0 if cap cannot hold every want (14 plus 10 each). Batch per peer. */
DART_API size_t    dart_detail_req_build(uint16_t domain, uint32_t peer_meta_version,
                                const DartDetailWant *wants, uint16_t n_wants,
                                void *out, size_t cap);

/* Bytes one response page takes, 0 if req is malformed. One datagram, except that a lone
 * oversized entry rides its own page. */
DART_API size_t    dart_transport_detail_resp_size(DartTransportState *st, const DartMetaSchema *schemas,
                                DartBytes req);
/* Answers req into out with one entry per advertised requested index, the wire inlined
 * only where the hashes differ. Truncates at an entry boundary. Does not check the domain. */
DART_API size_t    dart_transport_detail_respond(DartTransportState *st, const DartMetaSchema *schemas,
                                uint32_t meta_version, DartBytes req, void *out, size_t cap);

/* One topic's details. name and schema_wire point into the response. */
typedef struct {
    uint16_t   index;       /* the responder's local topic index */
    uint8_t    attrs;       /* the topic's DART_ATTR_* byte */
    DartString name;
    uint64_t   schema_hash; /* 0 = untyped */
    DartBytes  schema_wire; /* only when the request's hash differed, else {NULL,0} */
} DartDetail;

/* Zero initialize, then call until 0. */
typedef struct {
    uint32_t off;      /* byte offset of the next entry */
    uint16_t left;     /* entries still to yield */
    uint8_t  started;  /* 0 until the first call parses the header */
} DartDetailIter;

/* Walks a DETAIL_RESP. Stops at the end or at a malformed response. */
DART_API int       dart_detail_next(DartBytes resp, DartDetailIter *it, DartDetail *out);

/* The pending candidates of a peer, up to max_wants (out NULL just counts). Re run it on
 * every announce from the peer while it returns nonzero. */
DART_API uint16_t  dart_transport_detail_wants(DartTransportState *st, const DartMetaSchema *schemas,
                                uint32_t peer_id, DartBytes interest,
                                DartDetailWant *out, uint16_t max_wants);
/* Ingests a DETAIL_RESP and caches the verdicts. Returns the newly decided count, after
 * which the caller re applies the peer's interest so the matches form. */
DART_API uint16_t  dart_transport_apply_peer_details(DartTransportState *st, uint32_t peer_id,
                                DartBytes resp);

/* Entries of a peer that nominate this topic and are still undecided. Feeds the match wait. */
DART_API uint16_t  dart_transport_topic_unresolved(DartTransportState *st, uint16_t topic_index,
                                uint32_t peer_id, DartBytes interest);
/* The bulk form: every topic's unresolved count against one peer in a single walk. */
DART_API void      dart_transport_peer_unresolved_fill(DartTransportState *st, uint32_t peer_id,
                                DartBytes interest, uint16_t *counts, uint16_t n);

/* Rematches peers locally. The caller re advertises. 0 ok, negative unknown. */
DART_API int       dart_transport_set_role(DartTransportState *st, uint16_t topic_index, uint8_t role);

/* Defines a reserved slot: name, qos, role, a history ring and a rematch. 0 ok, -1 bad
 * index, name or slot, -4 out of memory. Re advertise after. */
DART_API int       dart_transport_topic_define(DartTransportState *st, uint16_t topic_index, const DartTopicDef *def);

/* Slot lifecycle, see spec/interest.md. retire parks a defined slot: 0 ok, -1 unknown or
 * already retired. Re advertise after. */
DART_API int       dart_transport_topic_retire(DartTransportState *st, uint16_t topic_index);
/* The slot a new define should reuse: 2 = a retired slot with the same identity and
 * kind, 1 = another retired slot, 0 = none. */
DART_API int       dart_transport_topic_reuse_find(DartTransportState *st, const char *name, uint8_t kind,
                                uint16_t *index_out);
/* Re defines a retired slot. binding_changed bumps the generation and holds writer lanes
 * until each peer names rebind_version in a request. 0 ok, -1 refused, -4 out of memory. */
DART_API int       dart_transport_topic_reuse(DartTransportState *st, uint16_t topic_index,
                                const DartTopicDef *def, int binding_changed, uint32_t rebind_version);
/* Records the highest version of our blob a peer named in a request. 1 when a held lane formed. */
DART_API int       dart_transport_peer_seen_version(DartTransportState *st, uint32_t peer_id, uint32_t version);

/* The next write seqno. It continues across retire and reuse. */
DART_API uint64_t  dart_transport_topic_seqno(DartTransportState *st, uint16_t topic_index);

/* {NULL,0} if undefined. A local lookup, never on the data path. */
DART_API DartString dart_transport_topic_name(DartTransportState *st, uint16_t topic_index);

/* 0 if the peer advertised no_timestamp for this topic. An unknown peer answers 1. */
DART_API int       dart_transport_peer_timestamped(DartTransportState *st, uint16_t topic_index,
                                uint32_t peer_id);

/* The peer's cached attrs byte for its topic, 0 until its details arrive. */
DART_API uint8_t   dart_transport_peer_attrs(DartTransportState *st, uint32_t peer_id,
                                uint16_t their_index);

/* 0 ok, negative on error. */
typedef enum {
    DART_OK             =  0,
    DART_ERR_NO_TOPIC = -1,  /* topic index out of range */
    DART_ERR_TOO_BIG    = -2,  /* exceeds DART_MESSAGE_MAX */
    DART_ERR_ROLE       = -3,  /* the topic is SUB_ONLY or INACTIVE */
    DART_ERR_OOM        = -4,  /* the allocator returned NULL */
    DART_ERR_STATE      = -5,  /* wrong state, or a call not allowed from inside a callback */
    DART_ERR_NOSYS      = -6   /* not compiled in */
} DartResult;

/* Publishes to every matched subscriber. */
DART_API int       dart_transport_send(DartTransportState *st, uint16_t topic_index, DartBytes data, uint64_t now_us);

/* Matched subscribers excluding dormant peers. O(matched lanes). */
DART_API int       dart_transport_publisher_live_matches(DartTransportState *st, uint16_t topic_index);
/* Is the peer a matched subscriber lane of this topic. The patterns layer's directed backstop. */
DART_API int       dart_transport_publisher_peer_matched(DartTransportState *st, uint16_t topic_index,
                                                         uint32_t peer_id);
/* The oldest live matched subscriber, 0 = none. The auto direct target for task requests. */
DART_API uint32_t  dart_transport_publisher_oldest_match(DartTransportState *st, uint16_t topic_index);
/* Matched publishers feeding our subscription side. */
DART_API int       dart_transport_subscriber_match_count(DartTransportState *st, uint16_t topic_index);

/* hdr rides in front of data as one message, byte identical to sending the concatenation.
 * capture_us 0 sends no capture slot. */
DART_API int       dart_transport_send_hdr(DartTransportState *st, uint16_t topic_index,
                               DartBytes hdr, DartBytes data, uint64_t capture_us, uint64_t now_us);
/* Publishes to one peer. Every other matched reliable lane skips the seqno through its
 * HB floor. A no op delivery when the peer is not a matched subscriber. */
DART_API int       dart_transport_send_to(DartTransportState *st, uint16_t topic_index, uint32_t to_peer,
                               DartBytes hdr, DartBytes data, uint64_t capture_us, uint64_t now_us);

#ifdef DART_SHM
/* The payload lives in an external chunk, fragmented for remote peers and described to
 * SHM peers. The chunk is the whole wire sample and must stay valid until it leaves history. */
DART_API int       dart_transport_send_shm(DartTransportState *st, uint16_t topic_index, DartBytes chunk,
                               const uint8_t *desc, uint64_t now_us);
/* Whether a peer can receive SHM-DATA. The node sets it on attach. */
DART_API void      dart_transport_peer_set_shm(DartTransportState *st, uint32_t peer_id, int is_shm);
/* 1 if every matched subscriber is SHM capable. */
DART_API int       dart_transport_publisher_shm_eligible(DartTransportState *st, uint16_t topic_index);
/* The history slot the next publish occupies, to bind a chunk to it. */
DART_API uint16_t  dart_transport_topic_hist_head(DartTransportState *st, uint16_t topic_index);
#endif

/* NULL if unknown. */
DART_API const DartQos *dart_transport_topic_qos(DartTransportState *st, uint16_t topic_index);
/* 0 = none or undefined. */
DART_API uint8_t   dart_transport_topic_attrs(DartTransportState *st, uint16_t topic_index);

/* 1 if the next send would overwrite history not yet acked by every subscriber. */
DART_API int       dart_transport_send_would_evict(DartTransportState *st, uint16_t topic_index);

/* 1 if the next send would overwrite history never handed to the wire, best effort
 * included. Fills the evicted sample's base and count. */
DART_API int       dart_transport_send_would_evict_unsent(DartTransportState *st, uint16_t topic_index,
                                                          uint64_t *evict_base, uint32_t *evict_count);

/* 1 if every live subscriber acked everything. Best effort and unknown return 1. */
DART_API int       dart_transport_send_drained(DartTransportState *st, uint16_t topic_index);

/* Matched subscribers, dormant included. O(1). */
DART_API int       dart_transport_publisher_match_count(DartTransportState *st, uint16_t topic_index);

/* Topics we publish to and receive from this peer. Both 0 for an unknown peer. */
DART_API void      dart_transport_peer_match_counts(DartTransportState *st, uint32_t peer_id,
                                          uint16_t *publish_to, uint16_t *receive_from);

/* Cumulative repair counters per topic, summed over its lanes. Always on. */
typedef struct {
    /* publisher side */
    uint64_t nacks_recv;     /* ACKNACKs that requested missing fragments */
    uint64_t frags_resent;   /* DATA fragments retransmitted */
    uint64_t frags_sent;     /* all DATA fragments sent, new and repair */
    /* subscriber side */
    uint64_t nacks_sent;     /* repair requests emitted */
    uint64_t frags_recv;     /* accepted DATA fragments, duplicates included */
    uint64_t frags_dup;      /* fragments already held */
    uint64_t msgs_skipped;   /* the sum of DART_MSG_LOST counts */
    /* each 0 to 1 arming of the subscriber's ack, by its trigger */
    uint64_t arms_data;
    uint64_t arms_hb;
    /* the fate of every received DATA fragment that was not accepted */
    uint64_t frags_old;        /* base below deliver_upto: already delivered or skipped */
    uint64_t frags_ahead;      /* beyond the one sample held ahead: dropped, fetched in order */
    uint64_t frags_malformed;  /* count 0, frag past count, or not subscribed */
} DartRepairStats;

/* Zeroed if the topic is out of range. */
DART_API void      dart_transport_repair_stats(DartTransportState *st, uint16_t topic_index, DartRepairStats *out);

/* The per peer round trip estimate, RFC 6298 shape, fed by the reliable path with no
 * probe traffic. samples 0 means no estimate yet. See spec/transport.md. */
typedef struct {
    uint32_t rtt_us;         /* smoothed round trip */
    uint32_t rtt_jitter_us;  /* mean deviation */
    uint32_t rtt_min_us;     /* the smallest sample, the path's floor */
    uint32_t rtt_last_us;    /* the most recent sample */
    uint32_t samples;
} DartPeerRtt;
/* 1 and *out for a known peer, else 0 and *out zeroed. */
DART_API int       dart_transport_peer_rtt(DartTransportState *st, uint32_t peer_id, DartPeerRtt *out);

/* Subscriber lanes of this topic with a repair request to service. */
DART_API int       dart_transport_repair_pending(DartTransportState *st, uint16_t topic_index);

/* The in progress message from peer: its base seqno, fragments held and fragments needed.
 * 1 if a message is mid reassembly. Any out pointer may be NULL. */
DART_API int       dart_transport_subscriber_progress(DartTransportState *st, uint16_t topic_index, uint32_t peer,
                                     uint64_t *base_seqno, uint32_t *have, uint32_t *total);

/* Retries the parked samples of a topic when downstream capacity frees, then flush
 * poll_send so the acks go out. Returns the lanes still parked. */
DART_API uint32_t  dart_transport_deliver_parked(DartTransportState *st, uint16_t topic_index, uint64_t now_us);

/* Feeds a received datagram tagged with its peer. */
DART_API void      dart_transport_on_datagram(DartTransportState *st, uint32_t from_peer, DartBytes datagram,
                                  uint64_t now_us);

/* One outgoing datagram, batched per peer. 1 and the outs, or 0. Loop until 0 with a
 * DART_DGRAM_MAX cap. */
DART_API int       dart_transport_poll_send(DartTransportState *st, uint32_t *to_peer, void *out, size_t cap,
                                size_t *out_len, uint64_t now_us);

/* The next armed timer, or 0. Cap a blocking poll at it. */
DART_API uint64_t  dart_transport_next_deadline_us(DartTransportState *st);

/* 1 while any lane holds work for poll_send. O(1). */
DART_API int       dart_transport_tx_pending(DartTransportState *st);

#ifdef __cplusplus
}
#endif
#endif /* DART_TRANSPORT_H */
#pragma endregion

#ifndef DART_TRANSPORT_SANS_IO
#pragma region serialize/schema.h
/* The schema and serialization layer. Both ends share the schema, so the wire carries no
 * tags or names. Depends only on common/. The rules are in spec/schema.md. */
#ifndef DART_SCHEMA_H
#define DART_SCHEMA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DART_SCHEMA_WIRE_VERSION
#define DART_SCHEMA_WIRE_VERSION 8u    /* bumped on any schema wire change */
#endif
#ifndef DART_SCHEMA_MAX_DEPTH
#define DART_SCHEMA_MAX_DEPTH 8u        /* struct nesting the builder accepts */
#endif

/* The kind byte on the wire. Fixed kinds pack first at static offsets, the variable kinds
 * ride tail frames. The encodings are in spec/schema.md. */
typedef enum {
    DART_U8 = 0, DART_U16 = 1, DART_U32 = 2, DART_U64 = 3,
    DART_I8 = 4, DART_I16 = 5, DART_I32 = 6, DART_I64 = 7,
    DART_F32 = 8, DART_F64 = 9, DART_BOOL = 10,   /* 0 to 10: fixed scalars */
    DART_ARR    = 11,   /* fixed array, the element must be fixed */
    DART_STRUCT = 12,
    DART_STR    = 13,   /* capped string, a slot of [u16 len][cap bytes] */
    DART_VSTR   = 14,   /* variable string, a tail frame */
    DART_VARR   = 15,   /* variable array, a tail frame of packed elements */
    DART_MAP    = 16,   /* self describing map, a tail frame */
    DART_ENUM   = 17,   /* a named integer, on the wire just its backing scalar */
    DART_NAMED  = 18    /* a name tag on another type, zero message bytes, never a root */
} DartSchemaTypeKind;

/* Bytes of a fixed scalar kind, 0 otherwise. */
DART_API uint32_t dart_schema_scalar_size(DartSchemaTypeKind kind);

/* The compiled schema: wire bytes, hash, size and a flat field table in one block from
 * the hook. Free it with dart_schema_free. */
typedef struct DartSchema DartSchema;

/* One field of the flat depth first table. A struct array flattens its element once as
 * an element 0 template under the array field. See spec/schema.md. */
typedef struct {
    DartString name;      /* the field's own name, a view into the wire */
    DartString type_name; /* the field type's name, {NULL,0} when anonymous */
    DartString elem_name; /* an array element type's name, {NULL,0} when anonymous */
    uint8_t    kind;      /* DartSchemaTypeKind, a NAMED wrapper is unwrapped into type_name */
    uint8_t    elem;      /* the array element kind, or the enum backing kind, else 0 */
    uint16_t   count;     /* ARR element count, or ENUM variant count, else 0 */
    uint16_t   depth;     /* 0 = top level, n = a member of the struct n levels up */
    uint16_t   str_cap;   /* string capacity for STR fields and STR elements, else 0 */
    uint16_t   arr_parent;/* flat index of the enclosing struct array, or 0xFFFF */
    uint32_t   offset;    /* absolute byte offset, 0 for variable kinds */
    uint32_t   size;      /* byte size, 0 for variable kinds */
    uint32_t   elem_size; /* ARR and VARR: bytes of one element, else 0 */
} DartSchemaFieldInfo;

/* One enum option. */
typedef struct {
    int64_t     value;
    const char *name;     /* NUL terminated builder input, at most 255 bytes */
} DartEnumVariant;

/* The builder grows its wire buffer through the hook as fields are added. finish returns
 * the compiled schema, or NULL after any latched error. Always finish a begun builder. */
typedef struct {
    DartAllocFn alloc; void *user;
    uint8_t *buf;
    size_t   cap;
    size_t   len;                              /* wire bytes written so far */
    int      err;                              /* 0 ok, nonzero latches failure */
    uint8_t  value_root;                       /* a bare root: 1 awaits its type, 2 written */
    uint8_t  raw_type;                         /* internal: type bytes only, no header */
    uint16_t base_depth;                       /* the struct depth the root sits at */
    uint16_t arr_depth;                        /* the open array element depth, 0xFFFF = none */
    uint16_t depth;                            /* open structs, 1 = a struct root only */
    size_t   count_pos[DART_SCHEMA_MAX_DEPTH]; /* wire offset of each open struct's nfields byte */
    uint16_t field_count[DART_SCHEMA_MAX_DEPTH];
} DartSchemaBuilder;

/* Compiles DSL text, pasted verbatim on the writer and every reader. The DSL is in
 * docs/stdtypes.md and spec/schema.md. NULL on error, with *err at the offending character. */
DART_API DartSchema *dart_schema_compile(DartAllocFn alloc, void *user, const char *text, const char **err);
/* compile with an environment: each env schema is referenceable by its root name. An
 * anonymous env root is ignored. */
DART_API DartSchema *dart_schema_compile_env(DartAllocFn alloc, void *user, const char *text,
                                             const DartSchema *const *env, size_t n_env,
                                             const char **err);

DART_API DartSchemaBuilder dart_schema_begin(DartAllocFn alloc, void *user, const char *root_name);
/* A bare type root: add exactly one field with an empty name, then finish. */
DART_API DartSchemaBuilder dart_schema_begin_value(DartAllocFn alloc, void *user);
/* A named alias root, like Uuid = u8[16]: one empty named field, then finish. */
DART_API DartSchemaBuilder dart_schema_begin_alias(DartAllocFn alloc, void *user, const char *name);
/* A fixed scalar field. */
DART_API void        dart_schema_field(DartSchemaBuilder *b, const char *name, DartSchemaTypeKind kind);
/* A fixed array of count scalars. */
DART_API void        dart_schema_field_array(DartSchemaBuilder *b, const char *name,
                                             DartSchemaTypeKind elem_scalar, uint16_t count);
/* A capped string, cap at least 1. */
DART_API void        dart_schema_field_string(DartSchemaBuilder *b, const char *name, uint16_t cap);
/* A fixed array of count capped strings. */
DART_API void        dart_schema_field_string_array(DartSchemaBuilder *b, const char *name,
                                                    uint16_t cap, uint16_t count);
/* A variable string. Any depth outside an array element. */
DART_API void        dart_schema_field_var_string(DartSchemaBuilder *b, const char *name);
/* A variable array of scalars. */
DART_API void        dart_schema_field_var_array(DartSchemaBuilder *b, const char *name,
                                                 DartSchemaTypeKind elem_scalar);
/* A variable array of capped strings. */
DART_API void        dart_schema_field_var_string_array(DartSchemaBuilder *b, const char *name,
                                                        uint16_t cap);
/* A self describing map. Write it with DartMapWriter, read it with dart_map_get. */
DART_API void        dart_schema_field_map(DartSchemaBuilder *b, const char *name);
/* A named integer with an integer backing and n options, n at most 65535. Latches an
 * error on a non integer backing or a value that does not fit it. */
DART_API void        dart_schema_field_enum(DartSchemaBuilder *b, const char *name,
                                            DartSchemaTypeKind backing,
                                            const DartEnumVariant *variants, uint16_t n);
/* A field of a compiled named type. type need not outlive the call. */
DART_API void        dart_schema_field_named(DartSchemaBuilder *b, const char *name,
                                             const DartSchema *type);
/* An array of a named type, variable when count is 0. The element must be fixed. */
DART_API void        dart_schema_field_named_array(DartSchemaBuilder *b, const char *name,
                                                   const DartSchema *type, uint16_t count);
/* Opens a nested struct. Add its fields, then end_struct. */
DART_API void        dart_schema_begin_struct(DartSchemaBuilder *b, const char *name);
/* An array of anonymous structs, variable when count is 0. Add the element's fields,
 * then end_struct. The element must be fixed. */
DART_API void        dart_schema_begin_struct_array(DartSchemaBuilder *b, const char *name,
                                                    uint16_t count);
DART_API void        dart_schema_end_struct(DartSchemaBuilder *b);
/* Closes the root, compiles, and returns the schema. NULL on any latched error. */
DART_API DartSchema *dart_schema_finish(DartSchemaBuilder *b);

/* Compiles received wire, bounds checked. NULL on an overrun or a version mismatch. The
 * bytes are copied in. */
DART_API DartSchema *dart_schema_parse(const void *wire, size_t wire_len, DartAllocFn alloc, void *user);
/* The same hook the schema was made with. */
DART_API void        dart_schema_free(DartSchema *s, DartAllocFn alloc, void *user);
/* An owned copy of any schema, safe to keep and to hand to create. */
DART_API DartSchema *dart_schema_copy(const DartSchema *s, DartAllocFn alloc, void *user);

DART_API DartBytes   dart_schema_wire(const DartSchema *s);        /* the canonical bytes to advertise */
DART_API uint64_t    dart_schema_hash(const DartSchema *s);        /* FNV-1a over the wire */
DART_API DartString  dart_schema_name(const DartSchema *s);   /* the root name, "" if anonymous */
/* Spells the schema back as DSL text, the inverse of compile. Returns the full length
 * excluding the NUL, so a NULL buffer measures. */
DART_API uint32_t    dart_schema_print(const DartSchema *s, char *buf, size_t cap);
/* The fixed section size, where the variable tail starts. */
DART_API uint32_t    dart_schema_size(const DartSchema *s);
/* The smallest valid message: the fixed section plus one empty frame per variable field. */
DART_API uint32_t    dart_schema_msg_min(const DartSchema *s);
/* The live length to send. 0 on a malformed or uninitialized buffer. */
DART_API uint32_t    dart_schema_msg_len(const DartSchema *s, const void *buf, size_t cap);
/* The flat depth first table. A bare root is one field named "". */
DART_API uint16_t    dart_schema_field_count(const DartSchema *s);
/* 1 and fills out, else 0 */
DART_API int         dart_schema_field_at(const DartSchema *s, uint16_t i, DartSchemaFieldInfo *out);
/* Resolves a dotted or indexed path ("velocity.dx", "corners[2].x") to the flat index, or -1. */
DART_API int         dart_schema_field_index(const DartSchema *s, const char *path);
/* One field's type encoding, a view into the wire. Two fields have the same type exactly
 * when these agree. {NULL,0} for an unknown index. */
DART_API DartBytes   dart_schema_field_type_wire(const DartSchema *s, uint16_t field);

/* The options of an enum field by flat index. count is 0 for a non enum field. */
DART_API uint16_t    dart_schema_enum_count  (const DartSchema *s, uint16_t field);
DART_API int         dart_schema_enum_variant(const DartSchema *s, uint16_t field, uint16_t i,
                                              int64_t *value, DartString *name);
/* Resolution over the schema alone. name_of gives {NULL,0} for an unknown number, which
 * is safe, not an error. value_of returns 1 and *out for a known name. */
DART_API DartString  dart_enum_name_of (const DartSchema *s, uint16_t field, int64_t value);
DART_API int         dart_enum_value_of(const DartSchema *s, uint16_t field, const char *name, int64_t *out);

/* 1 if msg is exactly one message of this schema. */
DART_API int       dart_schema_validate(const DartSchema *s, DartBytes msg);

/* 1 if a reader declaring sub can read messages written with pub. The rules are in
 * spec/schema.md. */
DART_API int dart_schema_subset(const DartSchema *sub, const DartSchema *pub);
/* The same, with the first incompatibility written to buf as one line on refusal. */
DART_API int dart_schema_subset_why(const DartSchema *sub, const DartSchema *pub,
                                    char *buf, size_t cap);
/* The reader's fields with the writer's offsets and size. Requires subset, NULL
 * otherwise or on OOM. Free with dart_schema_free. */
DART_API DartSchema *dart_schema_rebase(const DartSchema *sub, const DartSchema *pub,
                                        DartAllocFn alloc, void *user);

/* Getters by name, nested members by dotted path and array members by indexed path,
 * widened. A mismatch or unknown field yields 0. */
DART_API uint64_t  dart_get_uint(DartBytes msg, const DartSchema *s, const char *field);  /* and BOOL */
DART_API int64_t   dart_get_int (DartBytes msg, const DartSchema *s, const char *field);  /* I8 to I64 */
DART_API double    dart_get_f64 (DartBytes msg, const DartSchema *s, const char *field);  /* F64 or F32 */
DART_API float     dart_get_f32 (DartBytes msg, const DartSchema *s, const char *field);  /* F32 */
/* ARR: the whole array. VARR: the live elements, a whole multiple of the element size. */
DART_API DartBytes dart_get_array(DartBytes msg, const DartSchema *s, const char *field);
/* STR or VSTR: the live bytes. A hostile length prefix is clamped to the cap. */
DART_API DartString dart_get_string(DartBytes msg, const DartSchema *s, const char *field);
/* One element of a string array, {NULL,0} past the live count. */
DART_API DartString dart_get_string_at(DartBytes msg, const DartSchema *s, const char *field,
                                       uint16_t index);
/* The map body, to feed to dart_map_get. An empty body is an empty map. */
DART_API DartBytes dart_get_map(DartBytes msg, const DartSchema *s, const char *field);
/* The current option name, {NULL,0} if the stored number has no option. */
DART_API DartString dart_get_enum(DartBytes msg, const DartSchema *s, const char *field);
/* The live element count of a struct array, 0 if the field is not an array. */
DART_API uint32_t  dart_get_array_count(DartBytes msg, const DartSchema *s, const char *field);

/* The canonical default message: zeros and one empty frame per variable field. Always
 * start from this, since the variable setters rely on every frame existing. */
DART_API int dart_schema_message_default(const DartSchema *s, void *buf, size_t cap);

/* Setters by name. Values narrow like a C cast. A variable setter resizes its frame in
 * place and returns 0 when the message would exceed cap. 1 ok, 0 on a mismatch. */
/* set_uint also takes BOOL */
DART_API int dart_set_uint(void *buf, size_t cap, const DartSchema *s, const char *field, uint64_t v);
DART_API int dart_set_int (void *buf, size_t cap, const DartSchema *s, const char *field, int64_t v);
/* set_f64 also takes an F32 field */
DART_API int dart_set_f64 (void *buf, size_t cap, const DartSchema *s, const char *field, double v);
DART_API int dart_set_f32 (void *buf, size_t cap, const DartSchema *s, const char *field, float v);
/* ARR: elems over the front, the rest zeroed. VARR: the frame becomes elems. Whole
 * elements only, never silently truncated. */
DART_API int dart_set_array(void *buf, size_t cap, const DartSchema *s, const char *field, DartBytes elems);
/* Grows or shrinks a variable array, zero filling new elements. */
DART_API int dart_set_array_count(void *buf, size_t cap, const DartSchema *s, const char *field,
                                  uint32_t count);
/* STR: the live bytes, the slot tail zeroed, v.len must fit the cap. VSTR: the frame becomes v. */
DART_API int dart_set_string(void *buf, size_t cap, const DartSchema *s, const char *field, DartString v);
/* One element of a string array, under the live count for a variable one. */
DART_API int dart_set_string_at(void *buf, size_t cap, const DartSchema *s, const char *field,
                                uint16_t index, DartString v);
/* The frame becomes the map body, validated first. */
DART_API int dart_set_map(void *buf, size_t cap, const DartSchema *s, const char *field, DartBytes map);
/* Writes the option by name, 0 for an unknown one. */
DART_API int dart_set_enum(void *buf, size_t cap, const DartSchema *s, const char *field, const char *name);

/* One tagged value for reflection by flat index. */
typedef struct {
    uint8_t  kind;        /* DartSchemaTypeKind */
    uint8_t  elem;        /* the array element kind, or the enum backing kind */
    uint16_t count;       /* ARR count, VARR live count, MAP entries (saturated), ENUM variants */
    uint16_t str_cap;     /* string capacity for STR fields and STR elements */
    union { uint64_t u; int64_t i; double f; } v;   /* the scalar value, BOOL in u, ENUM in i */
    DartBytes bytes;      /* the raw bytes, the live string, the live elements or the map body */
} DartValue;
DART_API int dart_get_value(DartBytes msg, const DartSchema *s, uint16_t field, DartValue *out);
DART_API int dart_set_value(void *buf, size_t cap, const DartSchema *s, uint16_t field, const DartValue *val);
/* The same under a struct array, elem selects the element. Ignored for other fields. */
DART_API int dart_get_value_at(DartBytes msg, const DartSchema *s, uint16_t field, uint32_t elem,
                               DartValue *out);
DART_API int dart_set_value_at(void *buf, size_t cap, const DartSchema *s, uint16_t field, uint32_t elem,
                               const DartValue *val);
/* The live count of the struct array at flat index field, 0 if it is not an array. */
DART_API uint32_t dart_array_count_at(DartBytes msg, const DartSchema *s, uint16_t field);

/* The map is a self describing tagged value tree, the escape from the schema. Its body
 * grammar is in spec/schema.md. The writer fills a caller buffer with no allocation. */
typedef struct {
    uint8_t *buf; size_t cap, len;             /* the caller's buffer and write position */
    int      err;                              /* 0 ok, nonzero latches failure */
    uint16_t depth;                            /* open bodies, 1 = the root map only */
    size_t   count_pos[DART_SCHEMA_MAX_DEPTH]; /* offset of each open body's count */
    uint16_t count[DART_SCHEMA_MAX_DEPTH];
    uint8_t  is_arr[DART_SCHEMA_MAX_DEPTH];
} DartMapWriter;

DART_API DartMapWriter dart_map_begin(void *buf, size_t cap);   /* opens the root map */
DART_API int dart_map_put_uint  (DartMapWriter *w, const char *key, uint64_t v);
DART_API int dart_map_put_int   (DartMapWriter *w, const char *key, int64_t v);
DART_API int dart_map_put_f64   (DartMapWriter *w, const char *key, double v);
DART_API int dart_map_put_f32   (DartMapWriter *w, const char *key, float v);
DART_API int dart_map_put_bool  (DartMapWriter *w, const char *key, int v);
DART_API int dart_map_put_string(DartMapWriter *w, const char *key, DartString v);
DART_API int dart_map_open_map  (DartMapWriter *w, const char *key);   /* a nested map, close it */
DART_API int dart_map_open_array(DartMapWriter *w, const char *key);   /* elements pass key NULL */
DART_API int dart_map_close     (DartMapWriter *w);                    /* closes the innermost open body */
DART_API uint32_t dart_map_finish(DartMapWriter *w);                   /* the body length, 0 on any error */

/* Readers over a possibly hostile body: every walk is bounds checked and a nested value
 * comes back in DartValue.bytes ready for these same functions. */
DART_API uint16_t dart_map_count(DartBytes map);                       /* 0 if empty or short */
DART_API int dart_map_at (DartBytes map, uint16_t index, DartString *key, DartValue *out);
DART_API int dart_map_get(DartBytes map, const char *key, DartValue *out);   /* 1 and fills out, else 0 */
DART_API uint16_t dart_map_array_count(DartBytes arr);
DART_API int dart_map_array_at(DartBytes arr, uint16_t index, DartValue *out);
DART_API int dart_map_valid(DartBytes map);   /* the full structural check, dart_set_map runs it */

#ifdef __cplusplus
}
#endif
#endif /* DART_SCHEMA_H */
#pragma endregion
#ifndef DART_NO_STDTYPES
#pragma region serialize/stdtypes.h
/* The standard type library: named types always in scope in the DSL, with layout
 * identical C mirrors. docs/stdtypes.md has the roster and conventions. */
#ifndef DART_STDTYPES_H
#define DART_STDTYPES_H


#ifdef __cplusplus
extern "C" {
#endif

/* The roster in table order. DART_STD_NONE is not a standard type. */
typedef enum {
    DART_STD_NONE = 0,
    DART_STD_FLOAT2, DART_STD_FLOAT3, DART_STD_FLOAT4,
    DART_STD_DOUBLE2, DART_STD_DOUBLE3, DART_STD_DOUBLE4,
    DART_STD_INT2, DART_STD_INT3, DART_STD_INT4,
    DART_STD_QUATERNION,
    DART_STD_COLOR,
    DART_STD_RECT, DART_STD_RECTI,
    DART_STD_TRANSFORM, DART_STD_TWIST,
    DART_STD_GEOPOINT,
    DART_STD_UUID,
    DART_STD_TIMESTAMP, DART_STD_DURATION,
    DART_STD_MATRIX3X3, DART_STD_MATRIX4X4,
    DART_STD_URI,
    DART_STD_IMAGE, DART_STD_VIDEOFRAME, DART_STD_EXTERNALVIDEOSTREAM,
    DART_STD_CAMERAINTRINSICS, DART_STD_JOINTSTATE, DART_STD_JOINTNAMES,
    DART_STD_COUNT
} DartStdType;

/* The type's name and its canonical spelling, the two halves of the definition the DSL
 * knows. NULL for DART_STD_NONE or an out of range value. */
DART_API const char *dart_std_name(DartStdType t);
DART_API const char *dart_std_text(DartStdType t);
/* A name lookup only. Use dart_std_recognize when a peer's shape must be verified too. */
DART_API DartStdType dart_std_by_name(DartString name);

/* One standard type compiled as a schema of its own, for a topic whose payload is one. */
DART_API DartSchema *dart_std_schema(DartStdType t, DartAllocFn alloc, void *user);

/* Recognizes a standard type in a schema we did not write: the name and the shape must
 * both match. The result is stable per schema, so cache it. */
DART_API DartStdType dart_std_recognize(const DartSchema *s, DartAllocFn alloc, void *user);
DART_API DartStdType dart_std_recognize_field(const DartSchema *s, uint16_t field,
                                              DartAllocFn alloc, void *user);
DART_API DartStdType dart_std_recognize_elem(const DartSchema *s, uint16_t field,
                                             DartAllocFn alloc, void *user);

/* The C mirrors, layout identical to the wire on a little endian target. */
typedef struct { float  x, y;       } DartFloat2;
typedef struct { float  x, y, z;    } DartFloat3;
typedef struct { float  x, y, z, w; } DartFloat4;
typedef struct { double x, y;       } DartDouble2;
typedef struct { double x, y, z;    } DartDouble3;
typedef struct { double x, y, z, w; } DartDouble4;
typedef struct { int32_t x, y;       } DartInt2;
typedef struct { int32_t x, y, z;    } DartInt3;
typedef struct { int32_t x, y, z, w; } DartInt4;
typedef struct { double x, y, z, w; } DartQuaternion;   /* x, y, z, w in that order */
typedef struct { uint8_t r, g, b, a; } DartColor;       /* sRGB, straight alpha */
typedef struct { float   x, y, w, h; } DartRect;
typedef struct { int32_t x, y, w, h; } DartRectI;
typedef struct { DartDouble3 translation; DartQuaternion rotation;
                 uint16_t parent_len; char parent[30]; } DartTransform;
typedef struct { DartDouble3 linear, angular; } DartTwist;   /* m/s and rad/s */
typedef struct { double lat, lon, alt; } DartGeoPoint;       /* degrees, degrees, meters */
typedef struct { uint8_t bytes[16]; } DartUuid;              /* RFC 4122 byte order */
typedef int64_t DartTimestamp;   /* microseconds since the Unix epoch, UTC */
typedef int64_t DartDuration;                                /* microseconds */
typedef struct { float m[9];  } DartMatrix3x3;               /* row major */
typedef struct { float m[16]; } DartMatrix4x4;               /* row major */

/* Image.format, VideoFrame.codec and ExternalVideoStream.kind as the schema declares
 * them. A format of 16 or more is a compressed container. */
typedef enum {
    DART_IMAGE_MONO8 = 0, DART_IMAGE_MONO16 = 1, DART_IMAGE_RGB8 = 2, DART_IMAGE_RGBA8 = 3,
    DART_IMAGE_BGR8 = 4, DART_IMAGE_YUYV = 5, DART_IMAGE_NV12 = 6,
    DART_IMAGE_MONOF32 = 7,
    DART_IMAGE_JPEG = 16, DART_IMAGE_PNG = 17
} DartImageFormat;
typedef enum {
    DART_VIDEO_UNKNOWN = 0, DART_VIDEO_MJPEG = 1, DART_VIDEO_H264 = 2,
    DART_VIDEO_H265 = 3, DART_VIDEO_AV1 = 4
} DartVideoCodec;
typedef enum {
    DART_DISTORTION_NONE = 0, DART_DISTORTION_BROWN_CONRADY = 1,
    DART_DISTORTION_FISHEYE = 2, DART_DISTORTION_RATIONAL = 3
} DartDistortionModel;
typedef enum {
    DART_STREAM_RTSP = 0, DART_STREAM_WEBRTC_WHEP = 1, DART_STREAM_HLS = 2,
    DART_STREAM_SRT = 3, DART_STREAM_RTP = 4, DART_STREAM_HTTP_MJPEG = 5,
    DART_STREAM_OTHER = 15
} DartStreamKind;

/* The layout pins. A mirror that ever gained padding would silently mis decode. */
typedef char i_dart_std_size_check[
      (sizeof(DartFloat3) == 12 && sizeof(DartFloat4) == 16 &&
       sizeof(DartDouble3) == 24 && sizeof(DartQuaternion) == 32 &&
       sizeof(DartColor) == 4 && sizeof(DartRect) == 16 &&
       sizeof(DartTransform) == 88 && sizeof(DartTwist) == 48 &&
       sizeof(DartUuid) == 16 && sizeof(DartMatrix4x4) == 64) ? 1 : -1];

/* The thin operations, header only. */
static inline DartFloat3 dart_float3(float x, float y, float z){
    DartFloat3 v; v.x = x; v.y = y; v.z = z; return v;
}
static inline DartDouble3 dart_double3(double x, double y, double z){
    DartDouble3 v; v.x = x; v.y = y; v.z = z; return v;
}
static inline DartDouble3 dart_double3_add(DartDouble3 a, DartDouble3 b){
    return dart_double3(a.x + b.x, a.y + b.y, a.z + b.z);
}
static inline DartDouble3 dart_double3_sub(DartDouble3 a, DartDouble3 b){
    return dart_double3(a.x - b.x, a.y - b.y, a.z - b.z);
}
static inline DartDouble3 dart_double3_scale(DartDouble3 a, double k){
    return dart_double3(a.x * k, a.y * k, a.z * k);
}
static inline double dart_double3_dot(DartDouble3 a, DartDouble3 b){
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static inline DartDouble3 dart_double3_cross(DartDouble3 a, DartDouble3 b){
    return dart_double3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
DART_API double        dart_double3_length(DartDouble3 a);        /* needs a square root, in stdtypes.c */
DART_API DartDouble3   dart_double3_normalize(DartDouble3 a);     /* the zero vector maps to itself */

static inline DartQuaternion dart_quaternion(double x, double y, double z, double w){
    DartQuaternion q; q.x = x; q.y = y; q.z = z; q.w = w; return q;
}
static inline DartQuaternion dart_quaternion_identity(void){
    return dart_quaternion(0.0, 0.0, 0.0, 1.0);
}
static inline DartQuaternion dart_quaternion_conjugate(DartQuaternion q){
    return dart_quaternion(-q.x, -q.y, -q.z, q.w);
}
/* The rotation a followed by b, the Hamilton product b times a. */
static inline DartQuaternion dart_quaternion_mul(DartQuaternion a, DartQuaternion b){
    return dart_quaternion(
        b.w * a.x + b.x * a.w + b.y * a.z - b.z * a.y,
        b.w * a.y - b.x * a.z + b.y * a.w + b.z * a.x,
        b.w * a.z + b.x * a.y - b.y * a.x + b.z * a.w,
        b.w * a.w - b.x * a.x - b.y * a.y - b.z * a.z);
}
DART_API DartQuaternion dart_quaternion_normalize(DartQuaternion q);
DART_API DartDouble3    dart_quaternion_rotate(DartQuaternion q, DartDouble3 v);

static inline DartTransform dart_transform_identity(void){
    DartTransform t = {{0}};   /* zero fills the parent name too, it reaches the wire */
    t.rotation = dart_quaternion_identity(); return t;
}
static inline DartColor dart_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a){
    DartColor c; c.r = r; c.g = g; c.b = b; c.a = a; return c;
}
/* 0xRRGGBBAA both ways, the CSS order with the alpha last. */
static inline DartColor dart_color_from_hex(uint32_t rgba){
    return dart_color((uint8_t)(rgba >> 24), (uint8_t)(rgba >> 16),
                      (uint8_t)(rgba >> 8),  (uint8_t)rgba);
}
static inline uint32_t dart_color_to_hex(DartColor c){
    return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) | ((uint32_t)c.b << 8) | c.a;
}
/* Timestamp arithmetic in microseconds. */
static inline DartDuration  dart_timestamp_diff(DartTimestamp a, DartTimestamp b){ return a - b; }
static inline DartTimestamp dart_timestamp_add (DartTimestamp t, DartDuration d){ return t + d; }
static inline DartDuration  dart_duration_ms   (int64_t ms){ return ms * 1000; }
static inline DartDuration  dart_duration_s    (double s){ return (int64_t)(s * 1000000.0); }
static inline double        dart_duration_to_s (DartDuration d){ return (double)d / 1000000.0; }
/* All zeroes, the conventional unset. */
static inline int dart_uuid_is_nil(DartUuid u){
    int i; for (i = 0; i < 16; i++) if (u.bytes[i]) return 0;
    return 1;
}

#ifdef __cplusplus
}
#endif
#endif /* DART_STDTYPES_H */
#pragma endregion
#endif /* !DART_NO_STDTYPES */
#pragma region node/core.h
/* The sans-IO node core: the peer table, the discovery to transport lifecycle and
 * address resolution. A runtime owns the IO and drives it. The rules are in spec/node.md. */
#ifndef DART_NODE_CORE_H
#define DART_NODE_CORE_H


#ifdef __cplusplus
extern "C" {
#endif

/* The node's event. Four lifecycle kinds plus DART_ERROR, whose .error says which fault.
 * Read only the fields named for the kind. dart_event_str formats any of them. */
typedef enum {
    DART_PEER_UP,        /* discovered or resumed: .peer, .ip, .ip_len, .port */
    DART_PEER_DOWN,      /* lost or fell silent: .peer */
    DART_PEER_INTEREST,  /* interest applied: .peer, .publish_topics, .receive_topics */
    DART_MSG_LOST,       /* seqnos skipped: .topic, .peer, .lost_first, .lost_count. Not an error */
    DART_ERROR           /* read .error and dart_event_str */
} DartEventKind;

/* The error carried by a DART_ERROR event and returned by dart_last_error. Named DART_E_*
 * to stay distinct from the DartResult return codes. docs/node.md lists them. */
typedef enum {
    DART_E_NONE = 0,
    /* a match was refused, or advertised data cannot flow */
    DART_E_NAME_COLLISION,   /* a peer's topic name hashes to ours but differs: .identity, .topic */
    DART_E_QOS_INCOMPATIBLE, /* a reliable subscriber met a best effort publisher: .topic, .peer */
    DART_E_KIND_MISMATCH,    /* the name is another entity kind at a peer: .topic, .peer */
    DART_E_SCHEMA_MISMATCH,  /* a refused match or an ill fitting message: .topic, .peer */
    DART_E_INTEREST_OVERFLOW,/* a peer's index map failed to allocate: .peer, .lost_count entries */
    DART_E_META_TRUNCATED_INTEREST, /* our overlay overflowed, peers see no topics */
    DART_E_META_TRUNCATED_SCHEMA,   /* retired, kept so binding enums stay aligned */
    DART_E_PEER_META_TOO_BIG,/* a peer's blob exceeds our buffer: .peer (0 = new), .too_big_bytes */
    DART_E_MSG_TOO_BIG,      /* a received message could not be buffered: .too_big_bytes */
    DART_E_PEER_REFUSED,     /* the peer table is full of active peers: .ip, .ip_len, .port */
    DART_E_EVICTED_UNSENT,   /* a send overwrote unsent history: .topic, .lost_first, .lost_count */
    DART_E_UNMATCHED_SEND,   /* a send committed to nobody while a match was resolving: .topic */
    DART_E_DUPLICATE_AUTHORITY, /* a peer also claims the handler side: .topic, .peer. Diagnostic */
    /* IO and setup, mostly at open. .os_error carries the OS code */
    DART_E_OOM,              /* the allocator returned NULL: .too_big_bytes = bytes needed */
    DART_E_PLATFORM,         /* platform net init failed */
    DART_E_SOCKET,           /* opening a UDP socket failed */
    DART_E_BIND,             /* bind failed, the port is in use: .port */
    DART_E_MCAST_JOIN,       /* joining the discovery group failed, a bad interface */
    DART_E_SEND,             /* a send hard failed: .peer, .topic, .too_big_bytes = its size */
    DART_E_RECV,             /* a receive hard failed */
    DART_E_POLL,             /* the socket wait failed */
    DART_E_WAKER,            /* no cross thread waker, wakes come at the next tick */
    DART_E_BAD_ADDRESS       /* a configured address could not be parsed, refused at open */
} DartErrorKind;

typedef struct {
    DartEventKind kind;
    DartErrorKind error;       /* DART_ERROR: which error, else DART_E_NONE */
    const char *topic_name;  /* topic scoped events: our topic's name, valid for the callback */
    void       *user;          /* DartNodeOpts.user_data */
    uint32_t   peer;           /* the peer id, 0 = none */
    uint16_t   topic;        /* the local topic handle */
    int        os_error;       /* the OS code for SOCKET, BIND, MCAST_JOIN, SEND, RECV and POLL */
    uint8_t    ip[16];         /* the peer address, network order */
    uint8_t    ip_len;         /* 4 or 16, else 0 */
    uint16_t   port;
    uint64_t   lost_first;     /* MSG_LOST and EVICTED_UNSENT: the first seqno */
    uint64_t   lost_count;     /* the count, or INTEREST_OVERFLOW's entries */
    uint64_t   too_big_bytes;  /* MSG_TOO_BIG, PEER_META_TOO_BIG, OOM and SEND: the byte count */
    uint64_t   identity;       /* NAME_COLLISION: the colliding identity */
    uint16_t   publish_topics; /* PEER_INTEREST: topics we now publish to this peer */
    uint16_t   receive_topics; /* PEER_INTEREST: topics we now receive from it */
    const char *schema_detail; /* SCHEMA_MISMATCH: one line saying what was incompatible, or NULL */
    const char *peer_name;     /* the peer's node name, NULL when unknown. Prefer it over .peer */
} DartEvent;
typedef void (*DartEventFn)(const DartEvent *ev);

/* Formats ev as one line into buf, always NUL terminated. Returns buf. DART_NO_DIAG
 * compiles the text out and yields "error N". */
DART_API const char *dart_event_str(const DartEvent *ev, char *buf, size_t cap);

/* reflection types, the walks are in node/runtime.h */
typedef struct { uint32_t a, b; uint16_t c, d; } DartIter;   /* walk state, zero initialize */

#define DART_SELF 0u   /* the peer id that means this node */

typedef struct {
    uint32_t         id;             /* stable across a drop and return, never DART_SELF */
    uint8_t          uuid[16];       /* the process instance, a restart is a new uuid */
    DartString       name;
    DartString       address;        /* "ip:port" */
    DartPeerLiveness liveness;
    uint64_t         last_heard_us;
    uint32_t         epoch;          /* bumps on every reflected change at this peer */
    uint8_t          catching_up;    /* 1 = it advertises newer state than we hold */
    uint16_t         fragment_size;  /* its advertised UDP fragment size */
    /* the round trip as our reliable traffic measured it, samples 0 = no estimate yet */
    uint32_t         rtt_us;
    uint32_t         rtt_jitter_us;
    uint32_t         rtt_min_us;
    uint32_t         rtt_samples;
} DartPeerInfo;

typedef enum {
    DART_ENTITY_TOPIC = 0,
    DART_ENTITY_FUNCTION,
    DART_ENTITY_VARIABLE,
    DART_ENTITY_TASK
} DartEntityKind;

typedef struct {
    DartEntityKind    kind;
    DartString        name;          /* the base name, {NULL,0} until the details arrive */
    uint32_t          hash;          /* the primary channel's low 32 name hash, the placeholder */
    uint8_t           provides;      /* someone live is on the source side */
    uint8_t           consumes;      /* someone live is on the sink side */
    uint8_t           reliable;      /* the primary channel's reliability */
    uint8_t           writable;      /* VARIABLE: a set channel is advertised */
    uint8_t           forceable;     /* VARIABLE: the owner permits force */
    uint8_t           cancellable;   /* TASK: the provider honors cancel */
    uint8_t           exclusive;     /* TASK: declared serialization */
    uint8_t           multi;         /* duplicate authority is intended */
    uint8_t           incomplete;    /* a pattern half pair, surfaced rather than dropped */
    uint8_t           conflict;      /* MESH: live schemas that cannot read each other */
    uint16_t          providers;     /* live endpoints per side, a per node walk reports 0 or 1 */
    uint16_t          consumers;
    uint32_t          provider;      /* the ranked provider's peer id, DART_SELF is this node */
    DartString        from;          /* the node the schemas below were read from */
    const DartSchema *schema;        /* value, request or payload, NULL = untyped or unfetched */
    uint64_t          schema_hash;
    const DartSchema *rsp_schema;    /* FUNCTION and TASK: the response */
    uint64_t          rsp_schema_hash;
    const DartSchema *progress_schema;   /* TASK: the progress channel */
    uint64_t          progress_schema_hash;
    uint64_t          generation;    /* changes iff the provider, any schema or an attr changed */
} DartEntityInfo;

/* What the core needs from the runtime, set once at init. The peer table lives in the
 * discovery core, which also holds the core's per peer lifecycle scratch. */
typedef struct {
    DartTransportState           *transport;
    DartDiscoveryState  *discovery;    /* may be NULL at init, bound later */
    uint16_t              n_topics;    /* sizes the per topic arrays */
    uint16_t              frag_size;     /* our fragment size, baked into the overlay */
    DartEventFn         on_event;      /* optional */
    void                 *user;
    DartAllocFn           alloc;         /* required, backs the schema state and the blob */
    void                 *alloc_user;
    int                   oob_capable;   /* 1 = we can deliver shared memory payloads */
    uint8_t               oob_host[16];  /* our host id, a peer is reachable iff it matches */
    uint8_t               fetch_details; /* observer mode: fetch and cache every advertised index */
} i_DartNodeCoreConfig;

typedef struct i_DartNodeCore i_DartNodeCore;

/* The arena holds only the core struct and the per topic schema registry. */
size_t          i_dart_node_core_required_memory(uint16_t n_topics);
i_DartNodeCore *i_dart_node_core_init(void *mem, size_t mem_size, const i_DartNodeCoreConfig *cfg);
/* Relocates the core. The caller re points the transport, discovery and blob after. */
i_DartNodeCore *i_dart_node_core_migrate(i_DartNodeCore *old, void *new_mem, size_t new_cap,
                                       uint16_t new_n_topics);
/* Binds the discovery core whose peer table this core delegates to, again after a migrate. */
void            i_dart_node_core_bind_discovery(i_DartNodeCore *c, DartDiscoveryState *discovery);
/* Bytes of per peer scratch the core needs in the discovery peer table. */
uint16_t        i_dart_node_core_peer_user_bytes(void);

/* The announce overlay this node sends. build rebuilds it from the core's fields and
 * returns the length, meta returns the bytes. Rebuild after a role change. */
uint16_t        i_dart_node_core_build_meta(i_DartNodeCore *c);
DartBytes       i_dart_node_core_meta(i_DartNodeCore *c);
/* Registers a topic's schema, or clears it with NULL. schema must outlive the topic. */
void            i_dart_node_core_set_topic_schema(i_DartNodeCore *c, uint16_t topic_index,
                                                    const DartSchema *schema);

/* Topic slot lifecycle. retire drops the live schema pointers but keeps the hash as the
 * slot's fingerprint, topic_rebound purges the old occupant's bindings. See spec/interest.md. */
void     i_dart_node_core_retire_topic_schema(i_DartNodeCore *c, uint16_t topic_index);
uint64_t i_dart_node_core_topic_schema_hash(i_DartNodeCore *c, uint16_t topic_index);
void     i_dart_node_core_topic_rebound(i_DartNodeCore *c, uint16_t topic_index);
/* Feeds a peer's request version to the transport's rebind hold. 1 when a held lane formed. */
int      i_dart_node_core_seen_version(i_DartNodeCore *c, uint32_t peer, uint32_t version);

/* Answers a peer's DETAIL_REQ, stateless. {NULL,0} when not answerable, the requester re
 * asks. The view is valid until the next call. */
DartBytes i_dart_node_core_detail_respond(i_DartNodeCore *c, uint16_t domain, DartBytes req);

/* Interest paging: interest_respond answers an INTEREST_REQ with one page, apply_interest_page
 * ingests one, and on completion applies the blob exactly as an inline announce would. */
DartBytes i_dart_node_core_interest_respond(i_DartNodeCore *c, uint16_t domain, DartBytes req);
void      i_dart_node_core_apply_interest_page(i_DartNodeCore *c, uint16_t domain, uint32_t peer,
                                      DartBytes resp);

/* The transport's schema_check hook. The rules are in spec/schema.md. An allowed read side
 * check also records the reader view for delivery. */
int i_dart_node_core_schema_check(i_DartNodeCore *c, uint32_t peer, uint16_t topic_index,
                                  int peer_is_pub, uint64_t hash, DartBytes wire);

/* Why the gate refused (peer, topic, direction), recorded at detail intake. NULL when
 * unknown or under DART_NO_DIAG. */
const char *i_dart_node_core_schema_why(i_DartNodeCore *c, uint32_t peer, uint16_t topic_index,
                                        int peer_is_pub);
/* Records and returns the reason for a delivery length mismatch. NULL under DART_NO_DIAG. */
const char *i_dart_node_core_note_size_mismatch(i_DartNodeCore *c, uint32_t peer,
                                        uint16_t topic_index, uint64_t got_len, uint64_t want_len);

/* The schema a delivered message decodes with: ours, a rebased view of the publisher's, the
 * publisher's own for an untyped topic, or NULL for raw. */
const DartSchema *i_dart_node_core_msg_schema(i_DartNodeCore *c, uint32_t peer, uint16_t topic_index);

/* The discovery core's on_event sink, with cfg.user = this core. Keeps the peer table and
 * the transport peer set in lockstep and fires the app's peer events. */
void i_dart_node_core_on_disc_event(const DartDiscoveryEvent *ev);

/* A resolved outbound destination. The runtime turns it into wire bytes for its link. */
typedef struct {
    uint8_t  ip[16];     /* IPv4 today */
    uint8_t  ip_len;
    uint16_t port;
} i_DartNodeDest;

/* resolve turns a peer id into an address, 1 if sendable. id_for_addr maps a source back
 * to a peer id, 1 on a hit. */
int  i_dart_node_core_resolve(i_DartNodeCore *c, uint32_t to, i_DartNodeDest *out);
int  i_dart_node_core_id_for_addr(i_DartNodeCore *c, const uint8_t ip[4], uint16_t port, uint32_t *id);

/* The requester side of the uDTL cycle: apply_details ingests a response, detail_req_next
 * builds the next queued request until 0, detail_rearm re queues every active peer. */
void   i_dart_node_core_apply_details(i_DartNodeCore *c, uint16_t domain, uint32_t peer,
                                      DartBytes resp);
int    i_dart_node_core_detail_any(i_DartNodeCore *c);
void   i_dart_node_core_detail_rearm(i_DartNodeCore *c);
size_t i_dart_node_core_detail_req_next(i_DartNodeCore *c, uint16_t domain,
                                        void *out, size_t cap, i_DartNodeDest *to);
/* Unresolved candidate matches for one topic across every active peer. 0 = converged. */
int    i_dart_node_core_topic_unresolved(i_DartNodeCore *c, uint16_t topic_index);
/* The bulk form: every topic's count in one walk of each active peer's interest. */
void   i_dart_node_core_topics_unresolved(i_DartNodeCore *c, uint16_t *counts, uint16_t n);

/* A peer's name, a view into the peer table. "unknown-peer" for a known but unnamed peer,
 * .data NULL only for an unknown id. */
DartString i_dart_node_core_peer_name(i_DartNodeCore *c, uint32_t id);

/* Read only peer table enumeration for diagnostics. Any out pointer may be NULL. */
uint16_t i_dart_node_core_max_peers(i_DartNodeCore *c);
int      i_dart_node_core_peer_at(i_DartNodeCore *c, uint16_t slot, uint32_t *id,
                                uint8_t ip[16], uint8_t *ip_len, uint16_t *port);

/* Reflection: the tables behind the runtime's walks. See spec/reflection.md. */
uint16_t i_dart_node_peer_frag(const DartDiscoveryPeer *peer);
uint32_t i_dart_node_peer_interest_epoch(const DartDiscoveryPeer *peer);
int      i_dart_node_peer_interest_next(const DartDiscoveryPeer *peer,
                              DartInterestIter *it, DartTopicEntry *out);
void     i_dart_node_core_set_self_name(i_DartNodeCore *c, DartString name);
void     i_dart_node_core_self_begin(i_DartNodeCore *c);
void     i_dart_node_core_self_channel(i_DartNodeCore *c, uint16_t index, DartString name,
                              uint8_t kind, uint8_t role, uint8_t reliable, uint8_t attrs,
                              const DartSchema *schema);
void     i_dart_node_core_self_end(i_DartNodeCore *c);
int      i_dart_node_core_peers_next(i_DartNodeCore *c, DartIter *it, DartPeerInfo *out);
int      i_dart_node_core_entities_next(i_DartNodeCore *c, uint32_t peer, DartIter *it,
                              DartEntityInfo *out);
int      i_dart_node_core_mesh_next(i_DartNodeCore *c, DartIter *it, DartEntityInfo *out);
int      i_dart_node_core_mesh_find(i_DartNodeCore *c, DartEntityKind kind, const char *name,
                              DartEntityInfo *out);
uint32_t i_dart_node_core_mesh_epoch(i_DartNodeCore *c);
/* The create time pick for one channel (which: 0 primary, 1 rsp, 2 prg) of an entity. A
 * writer takes the widest schema every reader accepts, a reader the provider's. */
int      i_dart_node_core_reflect_pick(i_DartNodeCore *c, DartEntityKind kind, const char *name,
                              int which, int writer, const DartSchema **schema,
                              uint8_t *reliable, uint64_t *generation);

#ifdef __cplusplus
}
#endif
#endif /* DART_NODE_CORE_H */
#pragma endregion
#pragma region node/runtime.h
/* The node runtime and the public dart_node_* and dart_topic_* API. It owns the sockets
 * and the clock and drives the cores. docs/node.md explains how to use it. */
#ifndef DART_NODE_H
#define DART_NODE_H

#ifndef DART_NO_STDTYPES
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Network addressing and sockets. Every field is zero means default. docs/discovery.md
 * explains the options. */
typedef struct {
    uint16_t              data_port;         /* the unicast data port, 0 = OS assigned */
    const char           *discovery_group;   /* "239.255.0.7" */
    uint16_t              discovery_port;    /* 7400 */
    const char           *multicast_interface;/* pin discovery to this interface IP. NULL = every
                                                interface, "127.0.0.1" = single host */
    uint8_t               multicast_ttl;     /* hops an announce may travel, 1 */
    const DartDiscoveryAddr *seed_peers;   /* unicast announces here too, port 0 = discovery_port */
    uint16_t              n_seed_peers;
    uint8_t               unicast_only;      /* 1 = no group join, seeds and relays only. Also for a
                                                node behind an outbound only NAT */
    uint32_t              recv_buffer_bytes; /* the data socket SO_RCVBUF, 0 = OS default */
    uint32_t              send_buffer_bytes; /* the data socket SO_SNDBUF, 0 = OS default */
    uint16_t              fragment_size;     /* UDP payload bytes per fragment, 0 = DART_FRAG_SIZE.
                                                Clamped to [MIN, MAX]. One size per node */
    /* Stating our own locator. The default states nothing and peers record the source an
     * announce arrived from. Use these for a static one to one mapping. */
    const char           *self_ip;           /* advertise this IPv4 address. Unparseable refuses the
                                                open with DART_E_BAD_ADDRESS */
    uint16_t              advertise_port;    /* advertise this data port instead of the bound one */
} DartNodeNet;

/* Discovery cadence and the peer table size, zero means default. */
typedef struct {
    uint32_t              announce_interval_us; /* 3 s */
    uint32_t              peer_timeout_us;   /* drop a peer after this silence, 12 s */
    uint16_t              max_peers;         /* 16 */
} DartNodeDiscovery;

#ifndef DART_MATCH_WAIT_MS
#define DART_MATCH_WAIT_MS 1000   /* the default send path match wait bound */
#endif

/* The node options, passed as a compound literal. NULL means all defaults. */
typedef struct {
    uint16_t              domain;
    uint16_t              max_topics;  /* topics that can be created, 0 = 8 */
    void                 *user_data;     /* DartMsg.user and DartEvent.user */
    uint8_t               disable_shm;   /* 1 = never use the same host shared memory path */
    uint8_t               fetch_details; /* 1 = fetch and cache every topic every peer advertises,
                                            for observer UIs. Costs memory per peer topic */
    int32_t               match_wait_ms; /* the send path match wait bound. 0 = DART_MATCH_WAIT_MS,
                                            negative = disabled and such a send fires UNMATCHED_SEND */
    uint8_t               disable_logs;  /* 1 = no @dart/log topics, dart_node_log gives NOSYS */
    uint8_t               disable_meta;  /* 1 = no built in @dart/meta function */
    uint8_t               disable_error_logs; /* 1 = do not mirror errors onto @dart/log/error */
    DartNodeNet         net;
    DartNodeDiscovery   discovery;
} DartNodeOpts;

/* Per topic options, passed as a compound literal. */
typedef struct {
    DartQos  qos;
    uint8_t  reflect_from_mesh;  /* fill what was left unspecified from the mesh: a NULL schema and
                                    a zero reliability follow it. See docs/reflection.md */
} DartTopicOpts;

typedef struct DartNode    DartNode;
typedef struct DartTopic DartTopic;   /* an opaque handle, stable for the node's life */

/* A delivered message. From the callback, send and read only queries on the same node
 * are allowed. poll, create, set_role, drain, start, stop and close are refused. */
typedef struct {
    DartNode      *node;
    void          *user;             /* DartNodeOpts.user_data */
    uint16_t       topic_index;
    uint32_t       publisher_id;
    DartString     publisher_name;      /* never NULL data on delivery, "unknown-peer" at worst. A
                                        view valid for the callback */
    DartString     topic_name;     /* {NULL,0} if unknown */
    DartBytes      header;           /* pattern header bytes in front of the payload, {NULL,0} on a
                                        plain topic */
    DartBytes      data;             /* the payload after the header */
    const DartSchema *schema;        /* the schema data decodes with, validated before delivery.
                                        NULL on a raw topic or an untyped publisher */
    uint64_t       recv_us;          /* the monotonic clock at receive, or at enqueue */
    uint64_t       written_us;       /* the writer's wall clock at commit, UTC microseconds. 0 = the
                                        publisher opted out. Never mix it with recv_us */
    uint64_t       capture_us;       /* when the publisher says the data was true, UTC
                                        microseconds. 0 = none given, written_us is all there is */
} DartMsg;
typedef void (*DartMsgFn)(const DartMsg *msg);

/* The process heap as an allocator, for a caller with no page source of its own.
 * page_size 0 is the default. spec/allocation.md explains the modes. */
DART_API DartAllocator dart_allocator_heap(uint32_t page_size);
/* The process heap as a DartAllocFn, for dart_schema_compile and dart_schema_free. */
DART_API void         *dart_heap_realloc(void *user, void *ptr, size_t size);

/* Opens a node on alloc, which it copies and resets on close, so use one per node. alloc
 * is required and NULL refuses the open with DART_E_OOM. name NULL gives an auto name,
 * on_message and on_event may be NULL, opts NULL means defaults. */
DART_API DartNode    *dart_node_open(DartAllocator *alloc, const char *name, DartMsgFn on_message,
                                     DartEventFn on_event, const DartNodeOpts *opts);
/* The most recent error. n NULL returns the process global slot, the reason an open
 * returned NULL. .error is DART_E_NONE if none occurred. */
DART_API DartEvent    dart_last_error(DartNode *n);
/* One loop tick, blocking up to timeout_ms. DART_ERR_STATE while a service thread runs
 * or from inside a callback. */
DART_API int          dart_node_poll(DartNode *n, int timeout_ms);
/* Stops the service thread, then tears the node down. DART_ERR_STATE from a callback. */
DART_API int          dart_node_close(DartNode *n, int send_bye);

/* Threading: every call is thread safe under one node lock never held across a wait.
 * Drive the node with dart_node_poll or with dart_node_start. See docs/node.md. */
/* Runs the background service thread. DART_ERR_NOSYS when threads are compiled out. */
DART_API int          dart_node_start(DartNode *n);
/* Stops and joins the service thread. Idempotent, implied by close. */
DART_API int          dart_node_stop(DartNode *n);
/* 1 while the service thread runs. */
DART_API int          dart_node_is_started(DartNode *n);
/* Hold the node lock from a non callback thread around reads of a view. Hold it briefly.
 * No ops from a callback and when threads are compiled out. */
DART_API void         dart_node_lock(DartNode *n);
DART_API void         dart_node_unlock(DartNode *n);
/* Sends that evicted never sent history after the bounded wait, since open. */
DART_API uint32_t     dart_node_evicted_unsent(DartNode *n);

#ifndef DART_NO_STDTYPES
/* The two standard type values that need a platform: the wall clock a Timestamp counts
 * from, and a random version 4 Uuid. Neither needs a node. */
DART_API DartTimestamp dart_timestamp_now(void);
DART_API void          dart_uuid_new(DartUuid *out);
#endif

/* Creates a topic. name is the identity, schema is copied in, NULL means raw bytes. A
 * retired slot is reused. NULL when the reserve is full, the name is bad or out of memory. */
DART_API DartTopic *dart_node_create_topic(DartNode *n, const char *name, DartRole role,
                                               const DartSchema *schema, const DartTopicOpts *opts);

/* Retires a topic and frees the handle, invalid after DART_OK. DART_ERR_STATE from a
 * callback, for a pattern channel or a builtin, or during a dispatch. See docs/topics.md. */
DART_API int dart_topic_retire(DartTopic *topic);
/* A reflect_from_mesh topic: re reads the mesh and re types the handle in place when the
 * provider moved. 1 re created, 0 current, negative on error. */
DART_API int dart_topic_refresh(DartTopic *topic);

/* Consumer queues: a topic becomes queued on its first take or dispatch, or from creation
 * with qos.queue_bytes. docs/node.md explains the rules. */
#ifndef DART_QUEUE_CAP
#define DART_QUEUE_CAP (1u << 20)   /* the default queue growth cap, bytes per topic */
#endif
/* Pops the next queued message. The views stay valid until the next take or dispatch.
 * timeout_ms 0 checks, positive waits, negative waits forever. 1 got one, 0 empty. */
DART_API int dart_topic_take(DartTopic *topic, DartMsg *out, int timeout_ms);
/* Runs on_message on the calling thread for up to max_msgs queued messages (0 = all),
 * after waiting like take. Returns the count. The callbacks run without the node lock. */
DART_API int dart_topic_dispatch(DartTopic *topic, int max_msgs, int timeout_ms);
/* dispatch across every already queued topic, oldest first per topic. */
DART_API int dart_node_dispatch(DartNode *n, int max_msgs, int timeout_ms);
/* Messages waiting, ring bytes used and capacity, and messages dropped since open. */
DART_API void dart_topic_queue_stats(DartTopic *topic, uint32_t *msgs, uint32_t *bytes,
                                       uint32_t *capacity, uint32_t *dropped);
/* Publishes to every matched subscriber. DART_OK or a negative DartResult. */
/* Optional per send config, NULL means all defaults. capture_us is when the data was
 * true rather than when it was sent, and costs 8 wire bytes only when it is set. */
typedef struct {
    uint64_t capture_us;   /* UTC microseconds, the dart_timestamp_now clock. 0 = none */
} DartSendOpts;

DART_API int          dart_topic_send(DartTopic *topic, DartBytes data, const DartSendOpts *opts);
/* Flips the role at runtime and re advertises. 0 ok, negative on error. */
DART_API int          dart_topic_set_role(DartTopic *topic, DartRole role);
/* The local index, DartMsg.topic_index for its messages. */
DART_API uint16_t     dart_topic_index(const DartTopic *topic);
/* The schema as passed to create, NULL for a raw topic. */
DART_API const DartSchema *dart_topic_schema(const DartTopic *topic);
/* The handle by creation index, NULL if out of range. */
DART_API DartTopic *dart_node_topic(DartNode *n, uint16_t index);

/* Reflection: three walks over one zero initialized DartIter, called until 0. Every view
 * is valid until the next poll. docs/reflection.md explains them. */

/* One discovered peer per call, dropped peers included. Gate on liveness. */
DART_API int dart_node_peers_next(DartNode *n, DartIter *it, DartPeerInfo *out);

/* The entities one node advertises. DART_SELF walks this node, a dropped peer serves its
 * last known view. */
DART_API int dart_node_entities_next(DartNode *n, uint32_t peer, DartIter *it, DartEntityInfo *out);

/* The whole mesh folded, one entity per kind and name across every active peer and this node. */
DART_API int dart_node_mesh_next(DartNode *n, DartIter *it, DartEntityInfo *out);
DART_API int dart_node_mesh_find(DartNode *n, DartEntityKind kind, const char *name, DartEntityInfo *out);

/* Bumps on every reflected change anywhere. Re walk iff it moved. */
DART_API uint32_t dart_node_mesh_epoch(DartNode *n);

/* Microseconds waited on slow subscribers and how many sends waited, since open. */
DART_API void     dart_node_backpressure_stats(DartNode *n, uint64_t *waited_us, uint32_t *waited_sends);

/* Live bytes, the high water mark and the count of heap allocations. A flat count over a
 * window proves the hot path is allocation free. */
DART_API void     dart_node_mem_stats(DartNode *n, size_t *in_use, size_t *peak, uint64_t *alloc_calls);
/* The topic's cumulative repair counters. Zeroed for a NULL topic. */
DART_API void     dart_topic_repair_stats(DartTopic *topic, DartRepairStats *out);

/* The in pump probe: sampled on a timer inside a blocked reliable send, so a stall becomes
 * a time series of repair progress. Observational only. */
typedef struct {
    uint16_t topic;
    uint64_t wait_elapsed_us; /* since this backpressure wait began */
    uint64_t interval_us;     /* since the previous sample */
    uint64_t frags_resent;    /* in the interval */
    uint64_t nacks_recv;      /* in the interval */
    uint32_t polls;           /* poll calls in the interval */
    uint32_t polls_idle;      /* of those, polls with no repair pending */
} DartPumpSample;
typedef void (*DartPumpProbeFn)(void *user, const DartPumpSample *s);
/* Registers the probe, NULL disables. interval_us 0 = 200 ms. */
DART_API void     dart_node_set_pump_probe(DartNode *n, DartPumpProbeFn fn, uint64_t interval_us, void *user);
/* The in progress message from peer: 1 and its base seqno, fragments held and needed. */
DART_API int      dart_topic_subscriber_progress(DartTopic *topic, uint32_t peer,
                                     uint64_t *base_seqno, uint32_t *have, uint32_t *total);
/* Waits until every subscriber acked everything, or timeout_ms. 1 drained, 0 on timeout
 * or from a callback. Call before close so a burst is not cut by the BYE. */
DART_API int      dart_topic_drain(DartTopic *topic, int timeout_ms);
/* Subscribers matched now. */
DART_API int      dart_topic_match_count(DartTopic *topic);

/* The send path match wait: a send that would reach nobody while a match is still
 * resolving blocks up to opts.match_wait_ms. See docs/topics.md and spec/interest.md. */
/* Unresolved candidate matches right now. 0 = converged for everyone known. */
DART_API int      dart_topic_pending_count(DartTopic *topic);
/* 1 when a send would not wait: matched, or converged with nobody to wait for. The async
 * form for a GUI that disables the wait. */
DART_API int      dart_topic_ready(DartTopic *topic);

/* Blocks until discovery and matching settle. Call it after creating topics. timeout_ms
 * negative = 3 announce intervals. 1 settled, 0 on timeout or from a callback. */
DART_API int      dart_node_settle(DartNode *n, int timeout_ms);
#ifdef DART_SHM
/* Messages published and delivered through shared memory since open. */
DART_API void     dart_node_shm_stats(DartNode *n, uint32_t *sent, uint32_t *recv);
#endif

/* Messages and bytes this node committed to the topic and received on it, since open. */
DART_API void     dart_topic_counts(DartTopic *topic, uint64_t *tx_msgs, uint64_t *tx_bytes,
                                    uint64_t *rx_msgs, uint64_t *rx_bytes);

/* The built in @dart/log/{error,warn,info} topics. docs/node.md explains them. */
typedef enum { DART_LOG_ERROR = 0, DART_LOG_WARN = 1, DART_LOG_INFO = 2 } DartLogLevel;
#ifndef DART_LOG_MAX
#define DART_LOG_MAX 512   /* formatted log text bytes, longer is truncated */
#endif
/* printf style publish on the level's topic. DART_ERR_NOSYS when the logs are disabled. */
DART_API int          dart_node_log(DartNode *n, DartLogLevel level, const char *fmt, ...);
/* Publishes an already formatted line, len negative = NUL terminated. The FFI entry. */
DART_API int          dart_node_log_text(DartNode *n, DartLogLevel level, const char *text, int len);
/* The node's own handle for a level, NULL when disabled. */
DART_API DartTopic   *dart_node_log_topic(DartNode *n, DartLogLevel level);

/* The @dart/meta snapshot section mask, a 4 byte LE u32 request payload, 0 = everything.
 * The response is DartMeta { info: map } with one key per section. See docs/node.md. */
#define DART_META_NODE   0x1u
#define DART_META_PROC   0x2u
#define DART_META_TOPICS 0x4u
#define DART_META_PEERS  0x8u

/* Internal hooks for the patterns layer: create a topic carrying an entity kind, route its
 * messages to a handler, and observe node wide events and a per poll tick. */
typedef void     (*i_DartSysMsgFn)(void *user, const DartMsg *msg);
typedef void     (*i_DartSysEventFn)(void *user, const DartEvent *ev);
typedef uint64_t (*i_DartSysTickFn)(void *user, uint64_t now_us);   /* next deadline, 0 = none */
typedef void     (*i_DartSysCloseFn)(void *user);   /* the node is closing: settle promises */

/* Creates a pattern topic: stamps the kind, prefix, directed flag and attrs, permits '@' in
 * the name, and routes deliveries to on_msg. Never queued. */
DartTopic *i_dart_node_create_pattern_topic(DartNode *n, const char *name, DartRole role,
                              const DartSchema *schema, const DartTopicOpts *opts,
                              uint8_t kind, uint8_t prefix_bytes, uint8_t directed, uint8_t attrs,
                              i_DartSysMsgFn on_msg, void *on_msg_user);
/* Re types a live pattern channel in place, the engine of dart_topic_refresh. Lock not held. */
int  i_dart_topic_retype(DartTopic *topic, const DartSchema *schema, uint8_t reliability);
/* The reflect_from_mesh pick for one channel (which: 0 primary, 1 rsp, 2 prg). */
int  i_dart_node_reflect_pick(DartNode *n, DartEntityKind kind, const char *name, int which, int writer,
                              const DartSchema **schema, uint8_t *reliable, uint64_t *generation);
/* Publishes hdr plus payload on a pattern topic to every matched subscriber. */
int  i_dart_topic_send_hdr(DartTopic *topic, DartBytes hdr, DartBytes data);
/* Publishes hdr plus payload to one peer, for function replies. */
int  i_dart_topic_send_to(DartTopic *topic, uint32_t to_peer, DartBytes hdr, DartBytes data);
/* Clears a pattern topic's routing before its handle is freed. Under the node lock. */
void i_dart_topic_clear_sys(DartTopic *topic);
/* Hands every committed but unsent datagram to the wire now, transmit only. The pattern
 * layer's retire and close flush their CANCELLED replies here. Takes the node lock. */
void i_dart_node_flush_tx(DartNode *n);
/* The topic's next seqno. The slot line continues across retire and reuse, so a pattern
 * layer seeds its own monotonic counters from it. */
uint64_t i_dart_topic_seqno(DartTopic *topic);
/* The send path match wait without the send or the event: the caller derives its own
 * verdict. Returns the matched count. No wait from a callback or once converged. */
int  i_dart_topic_match_wait(DartTopic *topic);
/* Registers the patterns layer's event observer, per poll tick and close hook, NULL clears.
 * The close hook fires once at the top of close, with the node still fully alive. */
void i_dart_node_set_sys_hooks(DartNode *n, i_DartSysEventFn on_event, i_DartSysTickFn tick,
                               i_DartSysCloseFn on_close, void *user);
/* Node pool allocation (size 0 frees), the layer's per node handle slot, the monotonic
 * clock and the wall clock the transport stamps written_us from. Under the node lock. */
void    *i_dart_node_sys_alloc(DartNode *n, void *ptr, size_t size);
void   **i_dart_node_sys_slot (DartNode *n);
uint64_t i_dart_node_now_us   (DartNode *n);
uint64_t i_dart_node_wall_us  (DartNode *n);
/* The node lock for a pattern call that mutates state: 1 = acquired here, 0 = already held
 * by this thread. sys_unlock never kicks. sys_poll drives one loop tick. */
int      i_dart_node_sys_lock  (DartNode *n);
void     i_dart_node_sys_unlock(DartNode *n, int acquired);
int      i_dart_node_sys_poll  (DartNode *n, int timeout_ms);
/* Fires a topic scoped DART_ERROR through the node's normal event path. Under the node lock. */
void     i_dart_node_sys_error (DartNode *n, DartErrorKind error, DartTopic *topic, uint32_t peer);
/* Matched subscribers excluding dormant peers, the provider liveness query. */
int      i_dart_topic_live_match_count(DartTopic *topic);
/* Is peer a matched subscriber of this PUB topic. The directed call severed lane backstop. */
int      i_dart_topic_peer_matched(DartTopic *topic, uint32_t peer);
/* Matched publishers feeding this topic's subscription side. */
int      i_dart_topic_source_match_count(DartTopic *topic);
/* The oldest live matched subscriber's peer id, 0 = none. The task layer's auto direct target. */
uint32_t i_dart_topic_oldest_match(DartTopic *topic);
/* This node's discovery uuid, stable for its lifetime. The task layer's progress demux filter. */
const uint8_t *i_dart_node_uuid(DartNode *n);
/* Reflection getters for the patterns layer's entity enumeration. */
uint8_t    i_dart_topic_kind (const DartTopic *topic);   /* DartTopicKind */
uint8_t    i_dart_topic_role (const DartTopic *topic);   /* the current DartRole */
DartString i_dart_topic_name (const DartTopic *topic);   /* the stable name copy */
uint8_t    i_dart_topic_reliability(const DartTopic *topic);   /* the create qos reliability */
const uint8_t *i_dart_node_peer_uuid(DartNode *n, uint32_t peer);   /* NULL if unknown, a view */
uint16_t   i_dart_node_topic_count(DartNode *n);         /* one past the highest defined index */
/* Builds the DART_META_* snapshot (0 = all sections) as a map body into a node owned grown
 * buffer. A view valid until the next call, {NULL,0} on OOM. Under the node lock. */
DartBytes i_dart_node_snapshot(DartNode *n, uint32_t sections);

#ifdef __cplusplus
}
#endif
#endif /* DART_NODE_H */
#pragma endregion
#pragma region shm/core.h
/* The same host shared memory module: segment mapping and chunks over the platform
 * layer. Compiles to nothing without DART_SHM. The rules are in spec/shm.md. */
#ifndef DART_SHM_H
#define DART_SHM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compile bounds. An SBC sets these small, a workstation large. */
#ifndef DART_SHM_CHUNK_BYTES
#define DART_SHM_CHUNK_BYTES (4u*1024u*1024u)  /* default chunk, the node overrides per class */
#endif
#ifndef DART_SHM_CHUNKS
#define DART_SHM_CHUNKS 4u                     /* default chunks per segment, the node overrides */
#endif
#ifndef DART_SHM_NAME_MAX
#define DART_SHM_NAME_MAX 64u                  /* OS object name, derived from the node uuid */
#endif

/* The size class ladder: class k holds chunks of BASE << (k * SHIFT) bytes. The class
 * rides the low 3 bits of a segment id. */
#ifndef DART_SHM_CLASS_BASE
#define DART_SHM_CLASS_BASE  (64u*1024u)
#endif
#ifndef DART_SHM_CLASS_SHIFT
#define DART_SHM_CLASS_SHIFT 2u
#endif
#ifndef DART_SHM_N_CLASSES
#define DART_SHM_N_CLASSES   7u
#endif
#define DART_SHM_CLASS_MASK  0x7u

uint32_t i_dart_shm_class_bytes(uint32_t k);      /* chunk payload bytes for class k */
uint32_t i_dart_shm_class_for(uint32_t len);   /* the smallest fitting class, N_CLASSES if none */

/* The OS object name for a segment id, "/dart.shm.<16 hex>", valid on POSIX and Windows. */
void i_dart_shm_seg_name(char *buf, uint64_t segment_id);

/* The locator inside an SHM-DATA submessage. The framing supplies seqno base and count. */
typedef struct {
    uint64_t segment_id;   /* the writer's segment */
    uint32_t chunk;        /* chunk index */
    uint32_t length;       /* payload bytes */
    uint64_t generation;   /* chunk reuse counter at publish, rechecked after reading */
} i_DartShmDesc;

#define DART_SHM_DESC_WIRE 24u   /* little endian, the SHM-DATA body */
size_t i_dart_shm_desc_encode(const i_DartShmDesc *d, uint8_t out[DART_SHM_DESC_WIRE]);
int    i_dart_shm_desc_decode(i_DartShmDesc *d, const uint8_t *in, size_t len);  /* 0 malformed */

/* Segment layout: a header, then chunks of a 16 byte aligned chunk header plus payload.
 * generation is the only cross process mutable field, written release and read acquire. */
typedef struct {
    uint32_t magic;         /* DART_SHM_MAGIC, rejects a stale or foreign mapping */
    uint32_t version;
    uint64_t segment_id;
    uint32_t chunk_bytes;
    uint32_t n_chunks;
    uint64_t owner_pid;     /* an external janitor can reclaim an orphan */
    uint8_t  owner_host[16];/* the reader confirms the same kernel */
} i_DartShmSegHdr;

typedef struct {
    uint64_t generation;    /* bumped on each reuse, matched against the descriptor */
    uint32_t length;
    uint32_t _pad;
} i_DartShmChunkHdr;

#define DART_SHM_MAGIC    0x4D484453u   /* 'DSHM' */
#define DART_SHM_VERSION  1u

/* One mapped segment, placed in caller memory. A node creates segments for its own
 * publishes and attaches those of the same host peers it subscribes to. */
typedef struct i_DartShmPool i_DartShmPool;
size_t i_dart_shm_state_bytes(void);

typedef struct {
    char     name[DART_SHM_NAME_MAX];  /* from the writer's uuid, the reader gets it via meta */
    uint64_t segment_id;
    uint32_t chunk_bytes;              /* create only, 0 = DART_SHM_CHUNK_BYTES. attach reads it */
    uint32_t n_chunks;                 /* create only, 0 = DART_SHM_CHUNKS. attach reads it */
} i_DartShmConfig;

/* The writer. NULL means stay on UDP. */
i_DartShmPool *i_dart_shm_create(void *pool_mem, const i_DartShmConfig *cfg);
/* chunk returns the writable payload of a slot's chunk. stamp bumps the generation, sets
 * the length and fills the descriptor. The node owns the chunk to slot assignment. */
void *i_dart_shm_chunk(i_DartShmPool *p, uint32_t chunk, uint32_t *out_cap);
void  i_dart_shm_stamp(i_DartShmPool *p, uint32_t chunk, uint32_t len, i_DartShmDesc *out);

/* The reader. attach maps an existing segment whole and validates it, NULL means fall
 * back to UDP. read resolves a descriptor, NULL when the chunk was recycled. */
i_DartShmPool *i_dart_shm_attach(void *pool_mem, const i_DartShmConfig *cfg);
const void    *i_dart_shm_read  (i_DartShmPool *p, const i_DartShmDesc *d, uint32_t *out_len);
/* Rechecks the generation after a one copy read. 0 means the copy may be torn, discard it. */
int            i_dart_shm_verify(i_DartShmPool *p, const i_DartShmDesc *d);

void i_dart_shm_detach(i_DartShmPool *p);  /* unmaps, the writer also unlinks the OS object */

/* Same host is a host id match. A successful attach is the real gate. */
int i_dart_shm_host_match(const uint8_t peer_host[16], const uint8_t our_host[16]);

#ifdef __cplusplus
}
#endif
#endif /* DART_SHM_H */
#pragma endregion
#ifndef DART_NO_PATTERNS
#pragma region patterns/core.h
/* The patterns layer: functions, tasks and variables over dedicated topic kinds, so they
 * never cross wire with a plain topic or each other. docs/patterns.md explains the API. */
#ifndef DART_PATTERNS_H
#define DART_PATTERNS_H


#ifdef __cplusplus
extern "C" {
#endif

#ifndef DART_PATTERN_BP_WAIT_US
#define DART_PATTERN_BP_WAIT_US 1000000u   /* the default backpressure wait, 1 s */
#endif
#ifndef DART_CALL_TIMEOUT_US
#define DART_CALL_TIMEOUT_US 5000000u      /* the default call timeout, 5 s */
#endif

/* The response message cap. The wire carries its length in one byte, so this is fixed.
 * A longer message is truncated, never refused. */
#define DART_CALL_MSG_MAX 255u

/* functions */

/* A call's outcome. OK, APP_ERROR, NO_HANDLER, CANCELLED and RUNNING travel on the wire,
 * TIMEOUT and PEER_LOST are synthesized at the caller. See spec/patterns.md. */
typedef enum {
    DART_CALL_OK        = 0,
    DART_CALL_APP_ERROR = 1,   /* the handler replied with dart_request_fail */
    DART_CALL_NO_HANDLER= 2,   /* the definition has no on_request */
    DART_CALL_TIMEOUT   = 3,   /* no response within the timeout */
    DART_CALL_PEER_LOST = 4,   /* the handler node dropped mid call */
    DART_CALL_CANCELLED = 5,   /* a provider honored a cancel or retired mid run, or the node
                                  closed or the handle retired with the call still pending */
    DART_CALL_RUNNING   = 6    /* task, the one non terminal status: the request runs, the
                                  timeout is dropped, on_progress fires once with no data */
} DartCallStatus;

typedef struct DartFunction DartFunction;   /* an opaque handle */

/* The request as delivered to the definition's on_request. Views are valid for the
 * callback only. Pass only the exact pointer received to the reply calls, never a copy. */
typedef struct DartRequest {
    DartNode         *node;          /* the node the handler runs on */
    DartString        function_name; /* the name with the @req suffix stripped */
    DartBytes         data;          /* the request payload */
    const DartSchema *schema;        /* the schema data decodes with, NULL = an untyped caller.
                                        Non NULL means data.len was validated against it */
    uint32_t          caller;        /* the calling peer's id */
    DartString        caller_name;   /* the calling node's name, .data never NULL */
    uint64_t          recv_us;       /* the monotonic clock at arrival */
    uint64_t          written_us;    /* the caller's wall clock at the write, 0 = it opted out */
} DartRequest;

/* Delivered to the caller when a response arrives or is synthesized. data is a view valid
 * for the callback only. provider is the peer that answered, 0 if synthesized. */
typedef struct {
    DartCallStatus    status;
    DartBytes         data;
    const DartSchema *schema;    /* NULL = an untyped handler or a synthesized outcome */
    uint32_t          provider;
    void             *user;      /* the pointer passed to dart_function_call_async */
    uint64_t          written_us; /* the provider's wall clock at the write, 0 if synthesized */
    DartString        message;   /* the outcome text an HMI displays: the provider's, else the
                                    default status text. Empty only on OK, .data never NULL */
} DartResponse;
typedef void (*DartResponseFn)(const DartResponse *response);

/* The handler: reply exactly once with dart_request_reply or dart_request_fail, or defer
 * with dart_request_defer. Returning without replying auto acks DART_CALL_OK. */
typedef void (*DartRequestFn)(DartRequest *request, void *user);

typedef struct {
    uint32_t backpressure_wait_us;  /* 0 = DART_PATTERN_BP_WAIT_US */
    uint32_t timeout_us;            /* the remote side call timeout, 0 = DART_CALL_TIMEOUT_US */
    uint16_t keep_last;             /* req and rsp history depth, 0 = 10. Raise it above the most
                                       requests one pass drains, or replies past it are lost */
    uint8_t  reflect_from_mesh;     /* NULL schemas take the entity's from the mesh, and refresh
                                       re types the handle when that moves. A passed schema wins */
    uint8_t  multi;                 /* many definitions are expected, so the duplicate authority
                                       diagnostic is off. An undirected call reaches every one */
} DartFunctionOpts;

/* One progress update as delivered to a task caller's on_progress. Views are valid for
 * the callback only. The RUNNING acknowledgment fires it once with zero length data. */
typedef struct {
    uint32_t          call_id;
    uint32_t          provider;   /* the peer working the call */
    DartBytes         data;       /* len 0 = the RUNNING ack */
    const DartSchema *schema;     /* NULL = untyped or the RUNNING ack */
    uint64_t          written_us; /* the provider's wall clock, 0 = unstamped */
    uint64_t          recv_us;    /* the monotonic clock at arrival */
    void             *user;       /* DartCallOpts.progress_user */
} DartProgress;
typedef void (*DartProgressFn)(const DartProgress *progress);

/* Per call options, a trailing compound literal, NULL = defaults. */
typedef struct {
    uint32_t provider;              /* direct the call at this peer only, 0 = every definition and
                                       the first answer wins. A task always directs, 0 = oldest */
    DartProgressFn on_progress;     /* task: fires per progress update on the delivering thread,
                                       NULL = updates are discarded */
    void     *progress_user;        /* DartProgress.user */
    uint32_t *id_out;               /* filled with the call id, for dart_function_cancel */
} DartCallOpts;

/* Creates the definition (the body lives here, on_request NULL answers NO_HANDLER) or a
 * remote (a reference to one). Schemas may be NULL for untyped. NULL on failure. */
DART_API DartFunction *dart_node_create_function_definition(DartNode *n, const char *name,
                             const DartSchema *req_schema, const DartSchema *rsp_schema,
                             DartRequestFn on_request, void *user, const DartFunctionOpts *opts);
DART_API DartFunction *dart_node_create_remote_function(DartNode *n, const char *name,
                             const DartSchema *req_schema, const DartSchema *rsp_schema,
                             const DartFunctionOpts *opts);

/* Calls and blocks driving the node loop. timeout_ms negative = the function's default.
 * out->data is valid until the next blocking call. docs/patterns.md has the returns. */
DART_API int  dart_function_call(DartFunction *fn, DartBytes req, DartResponse *out, int timeout_ms,
                                 const DartCallOpts *opts);
/* Returns as soon as the request is committed, then on_response fires once with the
 * outcome. NULL = fire and forget. DART_OK or a negative DartResult. */
DART_API int  dart_function_call_async(DartFunction *fn, DartBytes req, DartResponseFn on_response,
                                       void *user, const DartCallOpts *opts);
/* Providers matched at a remote, callers matched at a definition. */
DART_API int  dart_function_match_count(DartFunction *fn);
/* Parks both channels, cancels every outstanding call with one CANCELLED outcome and frees
 * the handle. DART_ERR_STATE from a callback, the handle then stays valid. */
DART_API int  dart_function_retire(DartFunction *fn);
/* A reflect_from_mesh handle: re types every channel in place when the generation moved.
 * 1 re typed, 0 current, negative on error, DART_ERR_ROLE without the flag. */
DART_API int  dart_function_refresh(DartFunction *fn);

/* The built in @dart/meta function every node hosts and can call, with .multi and directed
 * requests. Ask with DartCallOpts.provider and a DART_META_* mask. NULL when disabled. */
DART_API DartFunction *dart_node_meta_function(DartNode *n);

/* in the handler callback */
DART_API void      dart_request_reply(DartRequest *request, DartBytes rsp);   /* answers OK */
/* Answers APP_ERROR with a human readable message (NULL = the default text, truncated at
 * DART_CALL_MSG_MAX). rsp may still carry structured failure data. */
DART_API void      dart_request_fail (DartRequest *request, const char *message, DartBytes rsp);
/* Defers the reply: returns a token (0 on failure) and suppresses the auto ack. Complete it
 * later from any thread with dart_function_complete. */
DART_API uint64_t  dart_request_defer(DartRequest *request);
DART_API int       dart_function_complete(DartFunction *fn, uint64_t token, DartCallStatus status,
                                          const char *message, DartBytes rsp);

/* Tasks: a function with progress and cancellation on the same handle. docs/tasks.md and
 * spec/patterns.md explain the model. */
typedef struct {
    uint8_t  progress_best_effort;  /* 0 = reliable. The definition offers, a remote requests,
                                       and the RxO rule composes them */
    uint16_t progress_keep_last;    /* the progress ring depth, 0 = the pattern default */
    uint8_t  no_cancel;             /* definition: will not honor cancellation, so remotes refuse
                                       dart_function_cancel with DART_ERR_ROLE */
    uint8_t  exclusive;             /* definition: declared serialization, the handler enforces */
    uint8_t  multi;                 /* redundant providers intended, one executor per request */
    uint32_t timeout_us;            /* remote: the until first response bound, 0 = the default */
    uint32_t backpressure_wait_us;  /* 0 = DART_PATTERN_BP_WAIT_US */
    uint16_t keep_last;             /* req and rsp history depth, as DartFunctionOpts.keep_last */
    uint8_t  reflect_from_mesh;     /* as DartFunctionOpts.reflect_from_mesh, for all three */
} DartTaskOpts;

/* Creates the definition or a remote, exactly as for a function, with prg_schema typing the
 * progress channel. The handler answers inline, or starts, defers and works the token. */
DART_API DartFunction *dart_node_create_task_definition(DartNode *n, const char *name,
                             const DartSchema *req_schema, const DartSchema *prg_schema,
                             const DartSchema *rsp_schema, DartRequestFn on_request, void *user,
                             const DartTaskOpts *opts);
DART_API DartFunction *dart_node_create_remote_task(DartNode *n, const char *name,
                             const DartSchema *req_schema, const DartSchema *prg_schema,
                             const DartSchema *rsp_schema, const DartTaskOpts *opts);

/* Sends RUNNING to the caller now. Idempotent, and implied by defer on a task.
 * DART_ERR_STATE on a plain function or after a reply. */
DART_API int dart_request_start(DartRequest *request);
/* Token verbs, any thread. A stale token is DART_ERR_STATE. progress broadcasts on the prg
 * channel, cancelled answers 1 once a cancel arrived. Cancellation is cooperative. */
DART_API int dart_function_progress (DartFunction *fn, uint64_t token, DartBytes progress);
DART_API int dart_function_cancelled(DartFunction *fn, uint64_t token);
/* The cancel notification, one slot per definition, NULL clears. Fires on the poll thread
 * under the usual callback restrictions. Polling dart_function_cancelled alone is complete. */
typedef void (*DartCancelFn)(uint64_t token, void *user);
DART_API int dart_function_on_cancel(DartFunction *def, DartCancelFn on_cancel, void *user);
/* Requests cancellation of call_id, cooperative and never acked: the terminal status is the
 * answer. DART_ERR_ROLE when the provider declared no_cancel, DART_ERR_STATE when not pending. */
DART_API int dart_function_cancel(DartFunction *fn, uint32_t call_id);

/* Variables: replicated state with one owner, the definition, which publishes the value.
 * Writers push over a set channel with no response. Remotes cache the latest value. */
typedef struct DartVariable DartVariable;

typedef enum { DART_VAR_READWRITE = 0, DART_VAR_READONLY = 1 } DartVarAccess;

typedef struct {
    DartBytes initial;              /* definition: the value before any set, empty = none */
    uint8_t   access;              /* DartVarAccess, READONLY creates no set channel */
    uint8_t   allow_force;         /* definition: permit force, off by default */
    uint16_t  catch_up;            /* the value channel's catch_up, 0 = 1 */
    uint16_t  keep_last;           /* both channels' history depth, the repair window for a burst of
                                      writes, 0 = 10. Raised to catch_up. See spec/patterns.md */
    uint32_t  backpressure_wait_us;/* 0 = DART_PATTERN_BP_WAIT_US */
    uint8_t   reflect_from_mesh;   /* a NULL schema takes the owner's from the mesh, and refresh
                                      re types the handle when that moves */
} DartVariableOpts;

/* Creates the definition (this node holds the value) or a remote (reads see the cached
 * latest, writes go over the set channel). schema NULL = untyped. NULL on failure. */
DART_API DartVariable *dart_node_create_variable_definition(DartNode *n, const char *name,
                                       const DartSchema *schema, const DartVariableOpts *opts);
DART_API DartVariable *dart_node_create_remote_variable(DartNode *n, const char *name,
                                       const DartSchema *schema, const DartVariableOpts *opts);

/* Reads the current value into *out, a view valid until the next call on this variable or
 * the next poll. 1 if a value exists. */
DART_API int  dart_variable_get(DartVariable *var, DartBytes *out);
/* Sets the value: a definition applies and publishes, a remote sends over the set channel.
 * DART_ERR_NO_TOPIC = no owner matched, DART_ERR_ROLE = a read only owner. docs/patterns.md. */
DART_API int  dart_variable_set(DartVariable *var, DartBytes value);
/* Force overrides the value until unforce restores the latest absorbed set. A definition
 * needs .allow_force (DART_ERR_STATE without), a remote's force is ignored by one without. */
DART_API int  dart_variable_force(DartVariable *var, DartBytes value);
DART_API int  dart_variable_unforce(DartVariable *var);
DART_API int  dart_variable_forced(DartVariable *var);

/* Variable events, one slot each, NULL clears. on_write fires on every applied write,
 * on_change only when the observed state changes, with a replay at registration. */
/* Both fire inline under the node lock on the thread that applied the write, with the usual
 * callback restrictions. A set from inside a callback is safe. See spec/patterns.md. */
typedef struct {
    DartVariable     *variable;
    DartString        name;        /* a stable view */
    DartBytes         value;       /* the value just applied, valid for the callback */
    const DartSchema *schema;      /* NULL = untyped */
    uint8_t           forced;      /* the value is a forced override */
    uint32_t          write_seq;   /* the owner's write counter */
    uint32_t          source;      /* the peer the write arrived from, 0 = a local call */
    uint64_t          recv_us;     /* the monotonic clock when the write applied */
    uint64_t          written_us;  /* the writer's wall clock, ours locally, 0 = opted out */
} DartVariableUpdate;
typedef void (*DartVariableUpdateFn)(const DartVariableUpdate *update, void *user);

DART_API int  dart_variable_on_change(DartVariable *var, DartVariableUpdateFn on_change, void *user);
DART_API int  dart_variable_on_write (DartVariable *var, DartVariableUpdateFn on_write,  void *user);
/* Blocks driving the node loop until a value exists or timeout_ms elapses (negative =
 * forever). 1 = a value, 0 = timeout, or from a callback or under a service thread. */
DART_API int  dart_variable_wait(DartVariable *var, int timeout_ms);
/* Remote: owners matched. Definition: remotes matched. */
DART_API int  dart_variable_match_count(DartVariable *var);
/* Parks the channels, silences the callbacks and frees the handle, invalid after. The same
 * contract as dart_function_retire. See docs/patterns.md. */
DART_API int  dart_variable_retire(DartVariable *var);
/* As dart_function_refresh, for a reflect_from_mesh variable. */
DART_API int  dart_variable_refresh(DartVariable *var);

#ifdef __cplusplus
}
#endif
#endif /* DART_PATTERNS_H */
#pragma endregion
#endif /* !DART_NO_PATTERNS */
#endif /* !DART_TRANSPORT_SANS_IO */

#ifdef DART_TRANSPORT_IMPLEMENTATION
#pragma region common/bytes.h
/* Little endian byte packing for every wire format. */
#ifndef DART_BYTES_H
#define DART_BYTES_H

#include <stdint.h>

static inline void i_dart_le_w16(uint8_t *p, uint16_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static inline void i_dart_le_w32(uint8_t *p, uint32_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static inline void i_dart_le_w64(uint8_t *p, uint64_t v){ int i; for (i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }
static inline uint16_t i_dart_le_r16(const uint8_t *p){ return (uint16_t)(p[0] | ((uint16_t)p[1]<<8)); }
static inline uint32_t i_dart_le_r32(const uint8_t *p){ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static inline uint64_t i_dart_le_r64(const uint8_t *p){ uint64_t v=0; int i; for (i=0;i<8;i++) v|=((uint64_t)p[i])<<(8*i); return v; }

#endif /* DART_BYTES_H */
#pragma endregion
#pragma region common/arena.h
/* Packs sub blocks into one arena. With base NULL it only measures, so the sizing pass
 * and the build pass run the same code and cannot drift. */
#ifndef DART_ARENA_H
#define DART_ARENA_H

#include <stddef.h>
#include <stdint.h>

typedef struct { uint8_t *base; size_t offset; size_t cap; int oom; } i_DartBump;

/* align must be a power of two */
static inline size_t i_dart_align_up(size_t n, size_t align){ return (n + (align - 1)) & ~(align - 1); }

static inline void *i_dart_bump_take(i_DartBump *b, size_t n, size_t align){
    size_t a = i_dart_align_up(b->offset, align);
    b->offset = a + n;
    if (b->base){
        if (b->offset > b->cap){ b->oom = 1; return NULL; }
        return b->base + a;
    }
    return NULL;   /* measure mode */
}

#endif /* DART_ARENA_H */
#pragma endregion
#pragma region common/hash.h
/* FNV-1a 64 for topic and schema identities. */
#ifndef DART_HASH_H
#define DART_HASH_H

#include <stddef.h>
#include <stdint.h>

/* The basis is not the textbook one. On wire identities depend on it, so it never changes. */
static inline uint64_t i_dart_fnv1a64(const void *data, size_t n){
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 1469598103934665603ull; size_t i;
    for (i = 0; i < n; i++){ h ^= (uint64_t)p[i]; h *= 1099511628211ull; }
    return h;
}

/* Over a C string. NULL gives 0. */
static inline uint64_t i_dart_fnv1a64_str(const char *s){
    uint64_t h = 1469598103934665603ull;
    const unsigned char *p = (const unsigned char *)s;
    if (!s) return 0;
    for (; *p; p++){ h ^= (uint64_t)*p; h *= 1099511628211ull; }
    return h;
}

#endif /* DART_HASH_H */
#pragma endregion
#pragma region transport/internal.h
/* Shared internals of the transport core. Not a public header. */
#ifndef DART_TRANSPORT_INTERNAL_H
#define DART_TRANSPORT_INTERNAL_H

#include <string.h>

/* byte 0 of every submessage: the type in the low 3 bits, flags above */
#define DART_DATA 1
#define DART_HB   2
#define DART_NACK 3
#define DART_MSG_MASK 0x07u
#define DART_F_SINGLE 0x08u     /* DATA: single fragment, frag, count and len omitted */
#define DART_F_UNPOS  0x10u     /* NACK: the reader has delivered nothing yet */
#ifdef DART_SHM
#define DART_F_SHM    0x20u     /* DATA: the body is a descriptor, no payload */
#endif

#define DART_NACK_WINDOW 32u    /* seqnos covered by one ACKNACK bitmap */
#define DART__NIL 0xFFFFFFFFu

#ifndef DART_HB_SWEEP_US
#define DART_HB_SWEEP_US 25000u /* the timer sweep covers every lane this often */
#endif

#define DART__NO_DEADLINE ((uint64_t)-1)  /* next_deadline_us: nothing armed */

/* per (peer, index) detail verdict bits, the peer_astate maps */
#define DART__AST_DETAILED 0x01u  /* details received and judged, else pending */
#define DART__AST_NAME_OK  0x02u  /* identity and name verified: peer_index[a] is our topic */
#define DART__AST_READ_OK  0x04u  /* schema gate passed on the read side, their pub to our sub */
#define DART__AST_WRITE_OK 0x08u  /* schema gate passed on the write side, their sub to our pub */

/* per entry interest flags, the announce's hash list */
#define DART__INT_ROLE_MASK 0x03u /* bits 0 and 1: the advertiser's DartRole */
#define DART__INT_RELIABLE  0x04u /* bit 2: offered or requested reliability */
#define DART__INT_KIND_MASK 0x78u /* bits 3 to 6: the advertiser's DartTopicKind */
#define DART__INT_KIND_SHIFT 3u
#define DART__INT_HOLE_RUN  0x80u /* bit 7: a run of undefined slots, hash = the run length */
#ifdef DART_SHM
#ifndef DART_SHM_MAX_RETRY
#define DART_SHM_MAX_RETRY 8u   /* give up on an unresolvable descriptor after this many */
#endif
#endif

#define DART_QOS_DEF_KEEP_LAST     1u
#define DART_QOS_DEF_KEEP_LAST_REL 10u   /* reliable: room for repair before overwrite */
#define DART_QOS_DEF_HEARTBEAT_US 250000u   /* 250 ms idle writer heartbeat */
#define DART_QOS_DEF_REPAIR_US    50000u    /* 50 ms reader repair request delay */
#ifndef DART_RTO_MIN_US
#define DART_RTO_MIN_US   2000u  /* the RTT timer floor, a poll wait cannot fire sooner */
#endif
#define DART_RTO_GRAIN_US 1000u  /* RFC 6298 G: the deviation term never counts under one tick */
#ifndef DART_HB_TAIL_US
#define DART_HB_TAIL_US 20000u   /* the tail heartbeat before the peer's RTT is known */
#endif

/* Submessage layout. Byte 0 = type and flags, bytes 1 and 2 = index, then the body.
 * Builders and readers share these offsets, so moving a field is one edit. */
#define DART_OFFSET_INDEX      1u  /* u16, every submessage */
#define DART_OFFSET_SEQNO      3u  /* DATA seqno, HB first, NACK base, SHM base (u64) */
#define DART_OFFSET_PAYLOAD_LEN_SINGLE     11u  /* DATA single: u16 payload_len */
#define DART_HEADER_DATA_SINGLE   13u  /* DATA single: header bytes */
#define DART_OFFSET_FRAG      11u  /* DATA multi: u16 frag */
#define DART_OFFSET_COUNT     13u  /* DATA multi: u16 count */
#define DART_OFFSET_SAMPLE_LEN      15u  /* DATA multi: u32 sample_len */
#define DART_OFFSET_PAYLOAD_LEN      19u  /* DATA multi: u16 payload_len */
#define DART_HEADER_DATA_MULTI    21u  /* DATA multi: header bytes */
#define DART_OFFSET_HB_LAST   11u  /* HB: u64 last */
#define DART_OFFSET_HB_COUNT    19u  /* HB: u32 count */
#define DART_HEADER_HB      23u
#define DART_OFFSET_NACK_NBITS  11u  /* NACK: u16 nbits */
#define DART_OFFSET_NACK_BITMAP   13u  /* NACK: u32 bitmap */
#define DART_OFFSET_NACK_EPOCH  17u  /* NACK: u32 epoch */
#define DART_HEADER_NACK    21u
#define DART_OFFSET_SHM_COUNT 11u  /* SHM-DATA: u16 count */
#define DART_OFFSET_SHM_DESC  13u  /* SHM-DATA: the descriptor */
#ifdef DART_SHM
#define DART_SHM_DATA_BYTES (DART_OFFSET_SHM_DESC + DART_SHM_DESC_BYTES)
#endif

/* The per lane and per sample structs are laid out widest field first so they carry no
 * padding, since they are allocated per match and per history slot. */
/* A sample's destination: every matched lane, nobody, or a peer slot. A slot is safe
 * because a directed topic never replays history and a new peer joins at the head. */
#define DART__DEST_ALL  0xFFFFFFFFu
#define DART__DEST_NONE 0xFFFFFFFEu

typedef struct {
    uint64_t base;       /* seqno of frag 0 */
    uint8_t *buf;        /* hook allocated, grown to fit */
#ifdef DART_SHM
    const uint8_t *shm_buf;            /* the external chunk payload, remote peers fragment it */
#endif
    uint32_t len;        /* message bytes */
    uint32_t cap;        /* allocated bytes of buf */
    uint32_t dest_slot;  /* DART__DEST_ALL, DART__DEST_NONE, or the destination peer slot */
    uint16_t count;      /* frag count */
    uint8_t  valid;
#ifdef DART_SHM
    uint8_t  shm;        /* 1 = the bytes live in shm_buf and desc is set */
    uint8_t  desc[DART_SHM_DESC_BYTES];/* sent to SHM peers as one SHM-DATA */
#endif
} i_DartWriterSample;

typedef struct {        /* writer side, per (topic, peer) */
    uint64_t sent_upto;  /* next seqno to push as new data */
    uint64_t acked_upto; /* the peer received everything below this */
    uint64_t nack_base;
    uint64_t hb_next_us; /* heartbeat timer */
    uint64_t wire_skip;  /* a fire and forget lane stamps sent_upto minus this as its private wire
                            seqno, so its reader counts only its own loss. See spec/transport.md */
    uint64_t rate_next_us;  /* earliest time a throttled lane may send its next sample */
    uint32_t rate_interval_us;/* us between paced samples, 0 = unthrottled */
    uint32_t nack_bits;
    uint32_t hb_count;
    uint32_t reader_epoch; /* the reader incarnation from its last ACKNACK, a change re joins */
    uint8_t  used;
    uint8_t  reader_reliable; /* only a reliable reader gets backpressure and heartbeats */
    uint8_t  has_nack;   /* a pending repair request */
    uint8_t  skip_hb;    /* a directed send left this lane owing a floor HB */
    uint8_t  rtt_probe;  /* armed at rtt_probe_seq, pushed at rtt_probe_us, any repair disarms it */
    uint64_t rtt_probe_seq;
    uint64_t rtt_probe_us;
} i_DartWriterProxy;

/* One sample's reassembly state. Buffers grow through the hook and are kept across rematch. */
typedef struct {
    uint8_t *buf;           /* at least len bytes */
    uint8_t *bitmap;        /* ceil(count/8): fragments held */
    uint64_t base;          /* seqno of frag 0 */
    uint32_t len;           /* message bytes */
    uint32_t cap;           /* allocated bytes of buf */
    uint32_t bitmap_cap;    /* allocated bytes of bitmap */
    uint16_t count;         /* frag count */
    uint16_t low;           /* the lowest missing fragment, low == count is complete */
    uint8_t  active;        /* received at least one fragment */
} i_DartAssembly;

typedef struct {        /* reader side, per (topic, peer) */
    uint64_t deliver_upto;  /* base of the current sample, everything below delivered or skipped */
    uint64_t hb_last;       /* the highest seqno the writer claims to hold */
    uint64_t received_high;      /* highest seqno actually received. Never NACK past it */
    uint64_t nack_high;       /* highest seqno requested this episode, a refill asks above it */
    uint64_t nack_retransmit_us;  /* earliest re ask of a stalled floor */
    uint64_t ack_due_us;
    i_DartAssembly cur;     /* the head sample */
    i_DartAssembly next;    /* one sample held ahead of the head. See spec/transport.md */
    uint32_t epoch;         /* this incarnation's id, sent in every ACKNACK */
    uint8_t  used;
    uint8_t  no_timestamp;  /* the writer sends no source stamp, from its cached attrs */
    uint8_t  started;       /* accepted any DATA from this writer yet */
    uint8_t  ack_force;     /* owes a cumulative ack even at an unchanged repair floor */
    uint8_t  ack_pending;
    uint8_t  parked;        /* the head is held after a downstream refusal, spec/transport.md */
    uint8_t  lapped;        /* a floor skip took a sample being fetched, nothing delivered since */
    uint8_t  rtt_probe;     /* a probe is armed at rtt_probe_seq, first asked at rtt_probe_us */
    uint64_t rtt_probe_seq;
    uint64_t rtt_probe_us;
#ifdef DART_SHM
    uint8_t  parked_shm;    /* the parked hold is a descriptor, not an assembled sample */
    uint8_t  shm_fail;      /* consecutive resolve failures at deliver_upto */
#endif
} i_DartReaderProxy;

/* One matched lane: both proxies plus the scheduler links. Records move when the pool
 * grows, so durable references are pool indices, never pointers. */
typedef struct {
    i_DartWriterProxy w;
    i_DartReaderProxy r;
    uint16_t topic;     /* the lane this record serves */
    uint16_t peer_slot;
    uint32_t sched_next;  /* next on its dest's active list, or the free list link */
    uint32_t topic_next;     /* next matched lane of the same topic */
    uint8_t  queued;      /* on its dest's active list */
    uint8_t  in_use;      /* allocated to a lane, 0 = on the free list */
} i_DartLane;

typedef struct {
    DartQos    qos;
    uint64_t  identity;     /* the name hash */
    const char *name;       /* our copy, NUL terminated storage */
    uint8_t   name_len;
    uint8_t   role;         /* DartRole */
    uint8_t   kind;         /* DartTopicKind, gates matching */
    uint8_t   attrs;        /* DART_ATTR_* from the definer, NO_TIMESTAMP derives from the qos */
    uint8_t   prefix_bytes; /* pattern header bytes in front of each payload */
    uint8_t   directed;     /* point to point sends, no cross lane MSG_LOST */
    uint8_t   history_owned;/* the ring came from the allocator, so destroy frees it */
    uint8_t   retired;      /* parked for reuse: announced as a hole, never a match candidate */
    uint8_t   gen;          /* rebind generation, bumped when a reused slot's binding changed */
    uint32_t  rebind_version; /* our blob version at the last rebind, 0 = never. Writer lanes
                                 hold until the peer has seen it */
    /* writer */
    i_DartWriterSample *history;       /* [depth] ring */
    uint16_t  history_head;    /* next slot to overwrite */
    uint64_t  next_seqno;
    uint64_t  first_seqno;  /* lowest seqno still cached */
    uint8_t   have_first;
    uint32_t  matched_writers; /* exact count of used writer proxies, dormant included */
    uint32_t  matched_readers; /* the same for reader proxies */
    uint32_t  lane_head;       /* first matched lane record, the send path walks this chain */
    DartRepairStats repair_stats;
} i_DartTopic;

struct DartTransportState {
    DartConfig    cfg;
    uint32_t    *peer_ids;  /* [max_peers] */
    uint8_t     *peer_used; /* [max_peers] */
    uint8_t     *peer_dormant;/* [max_peers] silent peers, out of flow control, state kept */
    uint16_t    *peer_frag; /* [max_peers] each peer's advertised fragment size */
    DartPeerRtt *peer_rtt;  /* [max_peers] the round trip estimator */
    uint16_t     frag;      /* our fragment size */
#ifdef DART_SHM
    uint8_t     *peer_shm;  /* [max_peers] 1 = the peer can receive SHM-DATA */
#endif
    /* one bit per topic per peer, with the proxies the whole stored interest */
    uint8_t     *peer_pub_bitmap; /* [max_peers][bitmap_len] the peer publishes topic c */
    uint8_t     *peer_sub_bitmap; /* [max_peers][bitmap_len] the peer subscribes topic c */
    uint8_t     *peer_sub_reliable; /* [max_peers][bitmap_len] and requested RELIABLE */
    uint16_t     bitmap_len;       /* ceil(n_topics / 8) */
    /* per peer wire index to our topic, sized to the peer's highest advertised index */
    uint16_t   **peer_index;      /* [max_peers] the index map, 0xFFFF = unmapped */
    uint32_t    *peer_index_len;  /* [max_peers] entries in each map */
    /* per (peer, index) detail verdicts, parallel to peer_index. See spec/interest.md */
    uint8_t    **peer_astate;     /* [max_peers] DART__AST_* bits per entry */
    uint8_t    **peer_agen;       /* [max_peers] the last applied rebind generation per entry */
    uint8_t    **peer_attrs;      /* [max_peers] the peer's advertised attrs per entry */
    uint32_t    *peer_seen_version; /* [max_peers] the highest version of our blob the peer named */
    i_DartTopic  *topics;     /* [n_topics] */
    /* the lane record pool: one hook allocation grown by doubling, records per real match */
    i_DartLane  *lanes;       /* [lane_cap] */
    uint32_t     lane_cap;
    uint32_t     lane_free;   /* free list head, DART__NIL when empty */
    uint16_t    *lane_index;  /* [n_topics*max_peers] record index, 0xFFFF = unmatched */
    /* the active lane scheduler: the event that gives a lane work enqueues it */
    uint32_t    *dest_head;   /* [max_peers] record index list per dest */
    uint32_t    *dest_tail;
    uint8_t     *dest_queued;
    uint32_t    *dest_queue;       /* ring of active destinations */
    uint32_t     dest_queue_head, dest_queue_count;
    uint32_t     sweep;       /* the timer sweep cursor */
    uint64_t     sweep_time_us;     /* the clock position the sweep has paid for */
    uint64_t     next_deadline_us; /* the earliest armed timer, caps the poll wait */
    uint32_t     reader_epoch_counter;      /* starts at 1, 0 = none */
};

typedef enum { DART_ORDER_OLD, DART_ORDER_GAP, DART_ORDER_ADOPTED, DART_ORDER_INORDER } i_DartReaderOrder;

static inline void i_dart_bit_set(uint8_t*bitmap,uint32_t i){bitmap[i>>3]|=(uint8_t)(1u<<(i&7));}
static inline void i_dart_bit_clr(uint8_t*bitmap,uint32_t i){bitmap[i>>3]&=(uint8_t)~(1u<<(i&7));}
static inline int  i_dart_bit_get(const uint8_t*bitmap,uint32_t i){return (bitmap[i>>3]>>(i&7))&1;}
/* Does this slot occupy an announce entry. An undefined or retired slot rides as a hole run. */
static inline int i_dart_topic_announced(const i_DartTopic *t){
    return t->name_len != 0 && !t->retired;
}
#ifdef DART_SHM
/* where a sample's bytes live: the external chunk for SHM samples, else our buf */
static inline const uint8_t *i_dart_sample_buf(const i_DartWriterSample *s){ return s->shm ? s->shm_buf : s->buf; }
#else
static inline const uint8_t *i_dart_sample_buf(const i_DartWriterSample *s){ return s->buf; }
#endif
/* Keeps next_deadline at the minimum armed time. t 0 is an immediate ack, woken through
   the active lane queue, so it is ignored here. */
static inline void i_dart_transport_arm_deadline(DartTransportState *st, uint64_t t){
    if (t && t < st->next_deadline_us) st->next_deadline_us = t;
}

/* Source stamp bytes at the front of this topic's samples. Framing follows the qos alone. */
static inline uint32_t i_dart_topic_ts_bytes(const i_DartTopic *topic){
    return topic->qos.no_timestamp ? 0u : DART_TIMESTAMP_BYTES;
}

/* The whole stamp prefix: the source slot, plus a capture slot when the sender gave one.
 * Never a hook's presence, so a reader sizes it from the bytes it received. */
static inline uint32_t i_dart_topic_stamp_bytes(const i_DartTopic *topic, uint64_t capture_us){
    uint32_t ts = i_dart_topic_ts_bytes(topic);
    return (ts && capture_us) ? ts + DART_CAPTURE_BYTES : ts;
}

/* Only a reliable topic with catch_up replays history to a late joiner. Everywhere else
 * history with no subscriber is dead weight and the send can be skipped. */
static inline int i_dart_topic_retains_history(const i_DartTopic *topic){
    return topic->qos.reliability==DART_RELIABLE && topic->qos.catch_up>0;
}

/* Heartbeats and acks both need a reliable topic with a matched proxy, so the sweep
 * skips the whole peer row of any other topic. */
static inline int i_dart_topic_needs_sweep(const i_DartTopic *topic){
    return topic->qos.reliability==DART_RELIABLE && (topic->matched_writers || topic->matched_readers);
}

/* The lane record for a (topic, peer) pair, DART__NIL when unmatched. */
static inline uint32_t i_dart_lane_id(DartTransportState *st, uint16_t topic_index, uint32_t peer_slot){
    uint16_t lane = st->lane_index[(size_t)topic_index*st->cfg.max_peers + peer_slot];
    return lane == 0xFFFFu ? DART__NIL : (uint32_t)lane;
}
static inline i_DartLane *i_dart_lane_at(DartTransportState *st, uint16_t topic_index, uint32_t peer_slot){
    uint32_t li = i_dart_lane_id(st, topic_index, peer_slot);
    return li == DART__NIL ? NULL : &st->lanes[li];
}
/* NULL when the lane is unmatched. Every caller treats that as not used. */
static inline i_DartWriterProxy *i_dart_writer_proxy_at(DartTransportState *st, uint16_t topic_index, uint32_t peer_slot){
    i_DartLane *l = i_dart_lane_at(st, topic_index, peer_slot);
    return l ? &l->w : NULL;
}
static inline i_DartReaderProxy *i_dart_reader_proxy_at(DartTransportState *st, uint16_t topic_index, uint32_t peer_slot){
    i_DartLane *l = i_dart_lane_at(st, topic_index, peer_slot);
    return l ? &l->r : NULL;
}
/* The wire index of a local topic is its own index. The peer mapped it from our interest. */
static inline uint16_t i_dart_wire_index_of(DartTransportState *st, int topic_index){
    (void)st; return (uint16_t)topic_index;
}

/* cross file prototypes */
size_t i_dart_wire_mk_data(uint8_t *o, uint16_t index, uint64_t seqno, i_DartWriterSample *s, uint16_t frag, const uint8_t *payload, uint16_t payload_len);
#ifdef DART_SHM
size_t i_dart_wire_mk_shm(uint8_t *o, uint16_t index, uint64_t base, uint16_t count, const uint8_t *desc);
#endif
size_t i_dart_wire_mk_hb(uint8_t *o, uint16_t index, uint64_t first, uint64_t last, uint32_t cnt);
size_t i_dart_wire_mk_nack(uint8_t *o, uint16_t index, uint64_t base, uint16_t nbits, uint32_t bitmap, uint32_t epoch, uint8_t flags);
void   i_dart_lane_wake(DartTransportState *st, uint16_t topic_index, uint32_t peer_slot);
void   i_dart_lane_enqueue(DartTransportState *st, uint32_t li);
void   i_dart_sched_drop(DartTransportState *st, uint32_t rec);
size_t i_dart_writer_emit(DartTransportState *st, int topic_index, int peer_slot, uint8_t *out, size_t cap, uint64_t now);
void   i_dart_writer_nack(DartTransportState *st, int topic_index, int peer_slot, const uint8_t *p, uint64_t now);
/* the per peer RTT estimator: fold one sample, and the retransmit bound it implies */
void     i_dart_rtt_sample(DartTransportState *st, uint32_t peer_slot, uint64_t sample_us);
uint32_t i_dart_rtt_rto(DartTransportState *st, uint32_t peer_slot, uint32_t fallback_us);
void   i_dart_reader_data(DartTransportState *st, int topic_index, int peer_slot, const uint8_t *p, uint64_t now);
#ifdef DART_SHM
void   i_dart_reader_shm(DartTransportState *st, int topic_index, int peer_slot, const uint8_t *p, uint64_t now);
#endif
void   i_dart_reader_hb(DartTransportState *st, int topic_index, int peer_slot, const uint8_t *p, uint64_t now);
size_t i_dart_reader_emit(DartTransportState *st, int topic_index, int peer_slot, uint8_t *out, size_t cap, uint64_t now);
i_DartTopic *i_dart_topic_at(DartTransportState *st, uint16_t topic_index, int *idx_out);
int    i_dart_peer_slot(DartTransportState *st, uint32_t id);
void   i_dart_transport_fire_event(DartTransportState *st, DartTransportEventKind kind, uint16_t topic_index, uint32_t peer, uint64_t first, uint64_t count);
uint64_t i_dart_topic_unicast_join_seqno(const i_DartTopic *topic);

#endif /* DART_TRANSPORT_INTERNAL_H */
#pragma endregion
#pragma region transport/wire.c
/* The DATA, HB, NACK and SHM-DATA submessage builders. */


size_t i_dart_wire_mk_data(uint8_t *o, uint16_t index, uint64_t seqno, i_DartWriterSample *s,
                         uint16_t frag, const uint8_t *payload, uint16_t payload_len){
    i_dart_le_w16(o+DART_OFFSET_INDEX,index);
    if (s->count==1){                       /* frag 0, count 1 and len are implied */
        o[0]=(uint8_t)(DART_DATA|DART_F_SINGLE);
        i_dart_le_w64(o+DART_OFFSET_SEQNO,seqno); i_dart_le_w16(o+DART_OFFSET_PAYLOAD_LEN_SINGLE,payload_len);
        memcpy(o+DART_HEADER_DATA_SINGLE,payload,payload_len);
        return DART_HEADER_DATA_SINGLE+payload_len;
    }
    o[0]=DART_DATA;
    i_dart_le_w64(o+DART_OFFSET_SEQNO,seqno); i_dart_le_w16(o+DART_OFFSET_FRAG,frag); i_dart_le_w16(o+DART_OFFSET_COUNT,s->count);
    i_dart_le_w32(o+DART_OFFSET_SAMPLE_LEN,s->len); i_dart_le_w16(o+DART_OFFSET_PAYLOAD_LEN,payload_len);
    memcpy(o+DART_HEADER_DATA_MULTI,payload,payload_len);
    return DART_HEADER_DATA_MULTI+payload_len;
}

#ifdef DART_SHM
/* One submessage covers [base, base+count). The body is the descriptor, no payload. */
size_t i_dart_wire_mk_shm(uint8_t *o, uint16_t index, uint64_t base, uint16_t count,
                          const uint8_t *desc){
    o[0]=(uint8_t)(DART_DATA|DART_F_SHM); i_dart_le_w16(o+DART_OFFSET_INDEX,index);
    i_dart_le_w64(o+DART_OFFSET_SEQNO,base); i_dart_le_w16(o+DART_OFFSET_SHM_COUNT,count);
    memcpy(o+DART_OFFSET_SHM_DESC,desc,DART_SHM_DESC_BYTES);
    return DART_SHM_DATA_BYTES;
}
#endif

size_t i_dart_wire_mk_hb(uint8_t *o, uint16_t index, uint64_t first, uint64_t last, uint32_t cnt){
    o[0]=DART_HB; i_dart_le_w16(o+DART_OFFSET_INDEX,index); i_dart_le_w64(o+DART_OFFSET_SEQNO,first); i_dart_le_w64(o+DART_OFFSET_HB_LAST,last); i_dart_le_w32(o+DART_OFFSET_HB_COUNT,cnt);
    return DART_HEADER_HB;
}

size_t i_dart_wire_mk_nack(uint8_t *o, uint16_t index, uint64_t base, uint16_t nbits, uint32_t bitmap,
                         uint32_t epoch, uint8_t flags){
    o[0]=(uint8_t)(DART_NACK|flags); i_dart_le_w16(o+DART_OFFSET_INDEX,index); i_dart_le_w64(o+DART_OFFSET_SEQNO,base);
    i_dart_le_w16(o+DART_OFFSET_NACK_NBITS,nbits); i_dart_le_w32(o+DART_OFFSET_NACK_BITMAP,bitmap); i_dart_le_w32(o+DART_OFFSET_NACK_EPOCH,epoch);
    return DART_HEADER_NACK;
}
#pragma endregion
#pragma region transport/sched.c
/* The active lane scheduler and the outgoing poll. */


/* work is queued as lane record indices, the destination is the record's peer slot */
static void i_dart_dest_push(DartTransportState *st, uint32_t d){
    uint32_t ndest = st->cfg.max_peers, pos;
    if (st->dest_queued[d]) return;
    st->dest_queued[d]=1;
    pos = st->dest_queue_head + st->dest_queue_count;
    if (pos >= ndest) pos -= ndest;
    st->dest_queue[pos]=d; st->dest_queue_count++;
}


/* idempotent while queued */
void i_dart_lane_enqueue(DartTransportState *st, uint32_t li){
    i_DartLane *l=&st->lanes[li];
    uint32_t d=l->peer_slot;
    if (l->queued) return;
    l->queued=1; l->sched_next=DART__NIL;
    if (st->dest_head[d]==DART__NIL) st->dest_head[d]=li;
    else st->lanes[st->dest_tail[d]].sched_next=li;
    st->dest_tail[d]=li;
    i_dart_dest_push(st, d);
}


/* A recycled record must never linger on a dest list, or it misroutes another lane's
 * submessages to the old peer. Unmatch time only, the lists are short. */
void i_dart_sched_drop(DartTransportState *st, uint32_t li){
    i_DartLane *l=&st->lanes[li];
    uint32_t d, cur, prev;
    if (!l->queued) return;
    d = l->peer_slot;
    prev = DART__NIL; cur = st->dest_head[d];
    while (cur!=DART__NIL && cur!=li){ prev=cur; cur=st->lanes[cur].sched_next; }
    if (cur==li){
        if (prev==DART__NIL) st->dest_head[d]=l->sched_next;
        else st->lanes[prev].sched_next=l->sched_next;
        if (st->dest_tail[d]==li) st->dest_tail[d]=prev;
    }
    l->queued=0; l->sched_next=DART__NIL;
}


/* Enqueues and tracks a freshly armed reader deadline for the poll cap. The sweep uses
 * i_dart_lane_enqueue instead, since it recomputes the deadline itself. */
void i_dart_lane_wake(DartTransportState *st, uint16_t topic_index, uint32_t peer_slot){
    uint32_t li=i_dart_lane_id(st,topic_index,peer_slot);
    i_DartLane *l;
    if (li==DART__NIL) return;                     /* unmatched lane: nothing to schedule */
    l=&st->lanes[li];
    if (l->r.used && l->r.ack_pending) i_dart_transport_arm_deadline(st, l->r.ack_due_us);
    i_dart_lane_enqueue(st, li);
}


/* sendable work a popped record still owes now. Timer armed work is the sweep's job */
static int i_dart_lane_work(DartTransportState *st, const i_DartLane *l, uint64_t now){
    i_DartTopic *topic=&st->topics[l->topic];
    uint32_t peer_slot=l->peer_slot;
    if (!st->peer_used[peer_slot] || st->peer_dormant[peer_slot]) return 0;   /* dormant */
    if (l->w.used && l->w.has_nack) return 1;
    if (l->w.used && l->w.skip_hb) return 1;   /* directed floor HB still owed */
    if (l->w.used && l->w.sent_upto < topic->next_seqno
        && (l->w.rate_interval_us == 0 || now >= l->w.rate_next_us)) return 1;   /* throttled */
    if (l->r.used && topic->qos.reliability==DART_RELIABLE
        && l->r.ack_pending && now >= l->r.ack_due_us) return 1;
    return 0;
}


/* Clock driven: a cursor walks the pool waking lanes whose timers came due, covering it
 * every DART_HB_SWEEP_US. A forced full pass when next_deadline_us is due recomputes it. */
static void i_dart_hb_sweep(DartTransportState *st, uint64_t now){
    uint32_t total=st->lane_cap, due, k;
    uint64_t span = now - st->sweep_time_us;
    int forced = (now >= st->next_deadline_us);    /* a tracked timer is due */
    int full;
    uint64_t mind = DART__NO_DEADLINE;             /* earliest not yet due timer seen */
    if (total==0){                                 /* no lanes have ever matched */
        if (forced) st->next_deadline_us = DART__NO_DEADLINE;
        st->sweep_time_us = now;
        return;
    }
    due = (forced || span >= DART_HB_SWEEP_US) ? total
        : (uint32_t)(span * total / DART_HB_SWEEP_US);
    if (!due) return;              /* sweep_time_us advances only when lanes are paid */
    full = (due >= total);         /* covered every record, so mind is the global minimum */
    st->sweep_time_us = now;
    for (k=0;k<due;k++){
        uint32_t li=st->sweep;
        i_DartLane *l=&st->lanes[li];
        i_DartTopic *topic;
        uint32_t peer_slot;
        st->sweep = (st->sweep+1u>=total) ? 0u : st->sweep+1u;
        if (!l->in_use) continue;                  /* free pool slot */
        topic=&st->topics[l->topic];
        peer_slot=l->peer_slot;
        /* a throttled lane owes a send at its tick. First, since best effort skips below */
        if (l->w.used && l->w.rate_interval_us && l->w.sent_upto < topic->next_seqno
            && st->peer_used[peer_slot] && !st->peer_dormant[peer_slot]){
            if (now>=l->w.rate_next_us) i_dart_lane_enqueue(st,li);
            else if (l->w.rate_next_us < mind) mind = l->w.rate_next_us;
        }
        if (!i_dart_topic_needs_sweep(topic)) continue;   /* best effort, or nothing matched */
        /* HB gate on next_seqno, never the reader ack: a sub only node still owes acks */
        if (!st->peer_used[peer_slot] || st->peer_dormant[peer_slot]) continue;   /* dormant */
        if (l->w.used && l->w.reader_reliable && l->w.acked_upto < topic->next_seqno){
            if (now>=l->w.hb_next_us) i_dart_lane_enqueue(st,li);
            else if (l->w.hb_next_us < mind) mind = l->w.hb_next_us;
        }
        if (l->r.used && l->r.ack_pending){
            if (now>=l->r.ack_due_us) i_dart_lane_enqueue(st,li);
            else if (l->r.ack_due_us < mind) mind = l->r.ack_due_us;
        }
    }
    /* a full pass saw every timer, so mind is exact. Woken lanes re arm during emit */
    if (full) st->next_deadline_us = mind;
}


int dart_transport_poll_send(DartTransportState *st, uint32_t *to_peer, void *out, size_t cap, size_t *out_len, uint64_t now){
    uint32_t ndest=st->cfg.max_peers;
    i_dart_hb_sweep(st, now);
    while (st->dest_queue_count){
        uint32_t d; size_t offset=0;
        d = st->dest_queue[st->dest_queue_head];
        st->dest_queue_head = (st->dest_queue_head+1u>=ndest) ? 0u : st->dest_queue_head+1u;
        st->dest_queue_count--; st->dest_queued[d]=0;
        /* drain this destination's lanes into one datagram */
        while (st->dest_head[d]!=DART__NIL){
            uint32_t li=st->dest_head[d];
            i_DartLane *l=&st->lanes[li];
            uint16_t topic_index=l->topic; uint32_t peer_slot=l->peer_slot;
            size_t n;
            do {
                /* acks first: small, and they carry the NACKs that drive repair */
                n=i_dart_reader_emit(st,(int)topic_index,(int)peer_slot,(uint8_t*)out+offset,cap-offset,now);
                if (!n) n=i_dart_writer_emit(st,(int)topic_index,(int)peer_slot,(uint8_t*)out+offset,cap-offset,now);
                offset+=n;
            } while (n && offset<cap);
            st->dest_head[d]=l->sched_next;
            if (i_dart_lane_work(st,l,now)){
                /* datagram full mid lane: rotate it to the back so siblings get the next */
                if (st->dest_head[d]==DART__NIL) st->dest_head[d]=li;
                else {
                    l->sched_next=DART__NIL;
                    st->lanes[st->dest_tail[d]].sched_next=li;
                    st->dest_tail[d]=li;
                }
                break;
            }
            l->queued=0;     /* lane drained */
        }
        if (st->dest_head[d]!=DART__NIL) i_dart_dest_push(st,d);  /* fair: re queue at the tail */
        if (offset){
            *to_peer = st->peer_ids[d];
            *out_len = offset;
            return 1;
        }
        if (st->dest_head[d]!=DART__NIL)
            return 0;    /* work pending but nothing fit: the cap is too small */
    }
    return 0;
}


uint64_t dart_transport_next_deadline_us(DartTransportState *st){
    return st->next_deadline_us == DART__NO_DEADLINE ? 0 : st->next_deadline_us;
}


int dart_transport_tx_pending(DartTransportState *st){
    return st->dest_queue_count != 0;   /* the active lane queue */
}
#pragma endregion
#pragma region transport/writer.c
/* The writer path: history, send, the per lane emit and ACKNACK handling. */


/* newest first, so pushing new data is O(1) */
static i_DartWriterSample *i_dart_sample_find(i_DartTopic *topic, uint64_t seqno){
    uint16_t depth = topic->qos.keep_last, k;
    uint16_t i = topic->history_head;
    for (k=0;k<depth;k++){
        i_DartWriterSample *s;
        i = (uint16_t)(i ? i-1 : depth-1);
        s = &topic->history[i];
        if (!s->valid) break;                  /* the unwritten tail */
        if (seqno >= s->base)
            return (seqno < s->base + s->count) ? s : NULL;
    }
    return NULL;
}


/* Seals the filled head slot into history at the next seqno, stamped with its destination. */
static void i_dart_writer_seal(DartTransportState *st, i_DartTopic *topic, size_t len,
                               uint32_t dest_slot, uint64_t *base_out, uint16_t *count_out){
    uint16_t depth = topic->qos.keep_last;
    uint16_t count = (uint16_t)((len + st->frag - 1) / st->frag);
    i_DartWriterSample *slot = &topic->history[topic->history_head];
    if (count==0) count=1;
    slot->valid=1; slot->base=topic->next_seqno; slot->count=count; slot->len=(uint32_t)len;
    slot->dest_slot = dest_slot;
    *base_out = slot->base; *count_out = count;
    topic->history_head = (uint16_t)((topic->history_head+1) % depth);
    topic->next_seqno += count;
    /* oldest cached: where the head points once wrapped, else slot 0 */
    topic->first_seqno = topic->history[topic->history_head].valid ? topic->history[topic->history_head].base
                                                    : topic->history[0].base;
    topic->have_first  = 1;
}


/* Directed topics: steps a lane over samples addressed to other peers. acked_upto only
 * steps contiguously, so a lane is never marked past a sample it is still owed. */
static void i_dart_writer_lane_advance(i_DartTopic *topic, i_DartWriterProxy *w, uint32_t peer_slot){
    i_DartWriterSample *s;
    if (!topic->directed) return;
    while ((s = i_dart_sample_find(topic, w->acked_upto)) != NULL
           && s->dest_slot != DART__DEST_ALL && s->dest_slot != peer_slot){
        w->acked_upto = s->base + s->count;
        w->skip_hb = 1;
    }
    if (w->sent_upto < w->acked_upto) w->sent_upto = w->acked_upto;
    while (w->sent_upto < topic->next_seqno
           && (s = i_dart_sample_find(topic, w->sent_upto)) != NULL
           && s->dest_slot != DART__DEST_ALL && s->dest_slot != peer_slot
           && w->sent_upto == s->base)
        w->sent_upto = s->base + s->count;
}


/* Seals and wakes the lanes that carry the sample, O(matches). A directed sample wakes
 * only its lane and every other lane derives its skip. */
static void i_dart_writer_commit(DartTransportState *st, uint16_t topic_index, size_t len,
                                 uint32_t dest_slot){
    i_DartTopic *topic = &st->topics[topic_index];
    int reliable = (topic->qos.reliability==DART_RELIABLE);
    uint64_t base; uint16_t count;
    uint32_t li;
    i_dart_writer_seal(st, topic, len, dest_slot, &base, &count);
    for (li=topic->lane_head; li!=DART__NIL; li=st->lanes[li].topic_next){
        i_DartLane *l = &st->lanes[li];
        if (!l->w.used || st->peer_dormant[l->peer_slot]) continue;
        if (dest_slot == DART__DEST_ALL || l->peer_slot == dest_slot){ i_dart_lane_enqueue(st, li); continue; }
        i_dart_writer_lane_advance(topic, &l->w, l->peer_slot);
        if (reliable && l->w.reader_reliable && l->w.skip_hb) i_dart_lane_enqueue(st, li);
    }
}


/* The 65535 fragment cap, checked before the no subscriber early out. */
static int i_dart_writer_too_big(DartTransportState *st, i_DartTopic *topic, size_t len){
    (void)topic;
    return len > 65535u*(uint32_t)st->frag;
}

/* Stores ts, hdr and data into the head slot. The source stamp is taken here, once per
 * message, so a repair resend, a replay and the SHM chunk all carry the original. */
static int i_dart_writer_store(DartTransportState *st, i_DartTopic *topic,
                               DartBytes hdr, DartBytes data, uint64_t capture_us,
                               size_t *len_out){
    uint32_t ts = i_dart_topic_ts_bytes(topic);
    uint32_t cap_b = (ts && capture_us) ? DART_CAPTURE_BYTES : 0u;
    size_t len = (size_t)ts + cap_b + hdr.len + data.len;
    if (i_dart_writer_too_big(st, topic, len)) return DART_ERR_TOO_BIG;
    {   i_DartWriterSample *slot = &topic->history[topic->history_head];
        size_t need = len ? len : 1u;
        if ((size_t)slot->cap < need){                    /* grow the slot to fit */
            uint8_t *new_buf = (uint8_t*)st->cfg.allocator(st->cfg.user, slot->buf, need);
            if (!new_buf) return DART_ERR_OOM;
            slot->buf = new_buf; slot->cap = (uint32_t)need;
        }
    }
    {   uint8_t *dst = topic->history[topic->history_head].buf;    /* gather: ts, hdr, payload */
        if (ts){
            uint64_t w = st->cfg.source_time ? st->cfg.source_time(st->cfg.user) : 0u;
            w &= DART_STAMP_MASK;
            if (cap_b) w |= DART_STAMP_CAPTURE;
            i_dart_le_w64(dst, w);
            if (cap_b) i_dart_le_w64(dst + ts, capture_us);
        }
        if (hdr.len)  memcpy(dst + ts + cap_b, hdr.data, hdr.len);
        if (data.len) memcpy(dst + ts + cap_b + hdr.len, data.data, data.len);
    }
#ifdef DART_SHM
    topic->history[topic->history_head].shm = 0;   /* an inline send: not SHM backed */
#endif
    *len_out = len;
    return DART_OK;
}


/* The one send: prologue, store and commit. dest_slot is stamped onto the sample. */
static int i_dart_writer_send(DartTransportState *st, uint16_t topic_index,
                              DartBytes hdr, DartBytes data, uint64_t capture_us,
                              uint32_t dest_slot){
    i_DartTopic *topic; size_t len; int r;
    topic = i_dart_topic_at(st, topic_index, NULL);
    if (!topic) return DART_ERR_NO_TOPIC;
    /* the stamps are ordinary payload for every size rule */
    if (i_dart_writer_too_big(st, topic,
            i_dart_topic_stamp_bytes(topic, capture_us) + hdr.len + data.len))
        return DART_ERR_TOO_BIG;
    if (topic->role == DART_SUB_ONLY || topic->role == DART_INACTIVE) return DART_ERR_ROLE;
    /* nobody subscribes and nothing durable to keep: skip the grow, the copy and the commit */
    if (topic->matched_writers == 0 && !i_dart_topic_retains_history(topic)) return DART_OK;
    r = i_dart_writer_store(st, topic, hdr, data, capture_us, &len);
    if (r != DART_OK) return r;
    i_dart_writer_commit(st, topic_index, len, dest_slot);
    return DART_OK;
}


int dart_transport_send(DartTransportState *st, uint16_t topic_index, DartBytes data, uint64_t now){
    DartBytes nohdr; nohdr.data=NULL; nohdr.len=0;
    return dart_transport_send_hdr(st, topic_index, nohdr, data, 0, now);
}


int dart_transport_send_hdr(DartTransportState *st, uint16_t topic_index, DartBytes hdr,
                            DartBytes data, uint64_t capture_us, uint64_t now){
    (void)now;
    return i_dart_writer_send(st, topic_index, hdr, data, capture_us, DART__DEST_ALL);
}


int dart_transport_send_to(DartTransportState *st, uint16_t topic_index, uint32_t to_peer,
                           DartBytes hdr, DartBytes data, uint64_t capture_us, uint64_t now){
    int peer_slot = i_dart_peer_slot(st, to_peer);   /* unknown: sent to nobody, seqno consumed */
    (void)now;
    return i_dart_writer_send(st, topic_index, hdr, data, capture_us,
                              peer_slot < 0 ? DART__DEST_NONE : (uint32_t)peer_slot);
}

#ifdef DART_SHM

/* The chunk is the whole wire sample, the caller wrote the stamp and any header into it,
 * so nothing is gathered or copied here. */
int dart_transport_send_shm(DartTransportState *st, uint16_t topic_index, DartBytes chunk,
                  const uint8_t *desc, uint64_t now){
    i_DartTopic *topic; i_DartWriterSample *slot; size_t len = chunk.len;
    (void)now;
    topic = i_dart_topic_at(st, topic_index, NULL);
    if (!topic) return DART_ERR_NO_TOPIC;
    if (len > 65535u*(uint32_t)st->frag) return DART_ERR_TOO_BIG;      /* the fragment count cap */
    if (topic->role == DART_SUB_ONLY || topic->role == DART_INACTIVE) return DART_ERR_ROLE;
    slot = &topic->history[topic->history_head];
    slot->shm = 1;
    slot->shm_buf = chunk.data;
    memcpy(slot->desc, desc, DART_SHM_DESC_BYTES);
    i_dart_writer_commit(st, topic_index, len, DART__DEST_ALL);
    return DART_OK;
}
#endif


int dart_transport_send_would_evict(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    i_DartWriterSample *slot; uint32_t li;
    if (!topic || topic->qos.reliability != DART_RELIABLE) return 0;
    slot = &topic->history[topic->history_head];        /* the slot the next send overwrites */
    if (!slot->valid) return 0;
    for (li=topic->lane_head; li!=DART__NIL; li=st->lanes[li].topic_next){
        i_DartLane *l=&st->lanes[li];
        if (l->w.used && l->w.reader_reliable && !st->peer_dormant[l->peer_slot]
            && l->w.acked_upto < slot->base + slot->count) return 1;
    }
    return 0;
}


int dart_transport_send_would_evict_unsent(DartTransportState *st, uint16_t topic_index,
                                            uint64_t *evict_base, uint32_t *evict_count){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    i_DartWriterSample *slot; uint32_t li;
    if (!topic) return 0;
    slot = &topic->history[topic->history_head];        /* the slot the next send overwrites */
    if (!slot->valid) return 0;
    for (li=topic->lane_head; li!=DART__NIL; li=st->lanes[li].topic_next){
        i_DartLane *l=&st->lanes[li];
        if (l->w.used && !st->peer_dormant[l->peer_slot] && l->w.sent_upto < slot->base + slot->count){
            if (evict_base)  *evict_base  = slot->base;
            if (evict_count) *evict_count = slot->count;
            return 1;
        }
    }
    return 0;
}


int dart_transport_send_drained(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    uint32_t li;
    if (!topic || topic->qos.reliability != DART_RELIABLE) return 1;  /* no acks to await */
    for (li=topic->lane_head; li!=DART__NIL; li=st->lanes[li].topic_next){
        i_DartLane *l=&st->lanes[li];
        if (l->w.used && l->w.reader_reliable && !st->peer_dormant[l->peer_slot]
            && l->w.acked_upto < topic->next_seqno) return 0;  /* a reliable reader is behind */
    }
    return 1;
}


int dart_transport_publisher_match_count(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    return topic ? (int)topic->matched_writers : 0;   /* cached at match time, O(1) */
}


/* O(1) from the cached count. */
int dart_transport_subscriber_match_count(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    return topic ? (int)topic->matched_readers : 0;
}


/* Dormant excluded. O(matches), for liveness decisions, not the send path. */
int dart_transport_publisher_live_matches(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    uint32_t li; int cnt = 0;
    if (!topic) return 0;
    for (li=topic->lane_head; li!=DART__NIL; li=st->lanes[li].topic_next){
        i_DartLane *l=&st->lanes[li];
        if (l->w.used && !st->peer_dormant[l->peer_slot]) cnt++;
    }
    return cnt;
}


int dart_transport_publisher_peer_matched(DartTransportState *st, uint16_t topic_index,
                                          uint32_t peer_id){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    uint32_t li; int slot;
    if (!topic) return 0;
    slot = i_dart_peer_slot(st, peer_id);
    if (slot < 0) return 0;
    for (li=topic->lane_head; li!=DART__NIL; li=st->lanes[li].topic_next){
        i_DartLane *l=&st->lanes[li];
        if (l->w.used && l->peer_slot == (uint32_t)slot) return 1;
    }
    return 0;
}


/* The chain is newest first, so the last live hit is the oldest. */
uint32_t dart_transport_publisher_oldest_match(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    uint32_t li, id = 0;
    if (!topic) return 0;
    for (li=topic->lane_head; li!=DART__NIL; li=st->lanes[li].topic_next){
        i_DartLane *l=&st->lanes[li];
        if (l->w.used && !st->peer_dormant[l->peer_slot]) id = st->peer_ids[l->peer_slot];
    }
    return id;
}


int dart_transport_repair_pending(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    uint32_t li; int cnt = 0;
    if (!topic) return 0;
    for (li=topic->lane_head; li!=DART__NIL; li=st->lanes[li].topic_next){
        i_DartLane *l=&st->lanes[li];
        if (l->w.used && l->w.has_nack) cnt++;
    }
    return cnt;   /* writer lanes with a NACK to service */
}

#ifdef DART_SHM

/* One remote reader forces inline UDP for the whole message. */
int dart_transport_publisher_shm_eligible(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    uint32_t li; int any=0;
    if (!topic) return 0;
    for (li=topic->lane_head; li!=DART__NIL; li=st->lanes[li].topic_next){
        i_DartLane *l=&st->lanes[li];
        if (!l->w.used || st->peer_dormant[l->peer_slot]) continue;
        if (!st->peer_shm[l->peer_slot]) return 0;
        any = 1;
    }
    return any;
}
#endif

#ifdef DART_SHM
uint16_t dart_transport_topic_hist_head(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    return topic ? topic->history_head : 0;
}
#endif

/* An HB advertising the window. Its first is the floor, which replaces GAP: the reader
 * skips to it. Resets the idle timer so it does not double send. */
static size_t i_dart_writer_hb(DartTransportState *st, i_DartTopic *topic, i_DartWriterProxy *w, uint16_t index,
                             uint8_t *out, size_t cap, uint64_t now){
    uint64_t first = topic->have_first ? topic->first_seqno : 0;
    if (cap < DART_HEADER_HB) return 0;
    if (w->acked_upto > first) first = w->acked_upto;   /* a fresh reader adopts the join point */
    w->hb_next_us = now + topic->qos.heartbeat_us;
    i_dart_transport_arm_deadline(st, w->hb_next_us);   /* wake to send the next idle HB */
    w->hb_count++;
    return i_dart_wire_mk_hb(out, index, first, topic->next_seqno-1, w->hb_count);
}


/* The queue just drained: the next HB comes within the tail window so a lost final message
 * repairs fast. The reader's immediate ack normally suppresses it, so healthy traffic is free. */
static void i_dart_writer_arm_tail(DartTransportState *st, i_DartWriterProxy *w, uint32_t peer_slot, uint64_t now){
    uint64_t tail = now + i_dart_rtt_rto(st, peer_slot, DART_HB_TAIL_US);
    if (w->hb_next_us <= now || w->hb_next_us > tail) w->hb_next_us = tail;
    i_dart_transport_arm_deadline(st, w->hb_next_us);
}


void i_dart_writer_nack(DartTransportState *st, int topic_index, int peer_slot, const uint8_t *p, uint64_t now){
    i_DartTopic *topic=&st->topics[topic_index];
    i_DartWriterProxy *w=i_dart_writer_proxy_at(st,topic_index,peer_slot);
    uint64_t base=i_dart_le_r64(p+DART_OFFSET_SEQNO); uint16_t nbits=i_dart_le_r16(p+DART_OFFSET_NACK_NBITS); uint32_t bitmap=i_dart_le_r32(p+DART_OFFSET_NACK_BITMAP);
    uint32_t epoch=i_dart_le_r32(p+DART_OFFSET_NACK_EPOCH); uint8_t flags=p[0];
    if (!w || !w->used) return;
    if (w->reader_epoch != epoch){
        if (w->reader_epoch){
            /* The reader re incarnated while our lane survived. Rejoin at the older of the
               fresh join point and the acked floor, so the successor gets the unacked window. */
            uint64_t join = i_dart_topic_unicast_join_seqno(topic);
            w->sent_upto  = w->acked_upto < join ? w->acked_upto : join;
            w->acked_upto = w->sent_upto;
            w->has_nack   = 0; w->rtt_probe = 0;
            w->hb_next_us = 0;
            i_dart_lane_wake(st,(uint16_t)topic_index,(uint32_t)peer_slot);
            w->reader_epoch = epoch;
            return;
        }
        w->reader_epoch = epoch;      /* first contact: the lane is already fresh */
    }
    if (flags & DART_F_UNPOS){
        /* an unpositioned reader never NACKs: re push from the unacked edge */
        if (w->acked_upto < w->sent_upto){
            w->sent_upto = w->acked_upto;
            i_dart_lane_wake(st,(uint16_t)topic_index,(uint32_t)peer_slot);
        }
        w->rtt_probe = 0;
        return;                    /* no position information to apply */
    }
    /* the cumulative ack reached the armed probe. A repair request in the same ACKNACK
       disarms it, since the acks after a repair are ambiguous (Karn) */
    if (w->rtt_probe && base >= w->rtt_probe_seq){
        i_dart_rtt_sample(st, (uint32_t)peer_slot, now > w->rtt_probe_us ? now - w->rtt_probe_us : 0u);
        w->rtt_probe = 0;
    }
    if (nbits>0 && bitmap!=0) w->rtt_probe = 0;
    if (base > w->acked_upto) w->acked_upto=base;
    /* directed: step over foreign samples the ack made contiguous and owe the floor HB */
    i_dart_writer_lane_advance(topic, w, (uint32_t)peer_slot);
    if (w->skip_hb) i_dart_lane_wake(st,(uint16_t)topic_index,(uint32_t)peer_slot);
    /* Merge, never overwrite: drop what the new base acks, then OR the new bits in.
       Overwriting stalled every crossing refill on the backstop. See spec/transport.md. */
    if (w->has_nack && base > w->nack_base){
        uint64_t d = base - w->nack_base;
        w->nack_bits = d < DART_NACK_WINDOW ? w->nack_bits >> d : 0u;
        w->nack_base = base;
        if (!w->nack_bits) w->has_nack = 0;
    }
    if (nbits>0 && bitmap!=0){
        topic->repair_stats.nacks_recv++;   /* a repair request, not a bare ack */
        if (w->has_nack){                                           /* nack_base >= base here */
            uint64_t d = w->nack_base - base;
            w->nack_bits |= d < DART_NACK_WINDOW ? bitmap >> d : 0u;
        } else { w->has_nack=1; w->nack_base=base; w->nack_bits=bitmap; }
        i_dart_lane_wake(st,(uint16_t)topic_index,(uint32_t)peer_slot);
    }
}


/* One writer submessage if due and it fits cap, 0 if none. On no fit the state is
 * untouched, so the same submessage is produced next time. */
size_t i_dart_writer_emit(DartTransportState *st, int topic_index, int peer_slot, uint8_t *out, size_t cap, uint64_t now){
    i_DartTopic *topic=&st->topics[topic_index];
    i_DartWriterProxy *w=i_dart_writer_proxy_at(st,topic_index,peer_slot);
    int reliable=(topic->qos.reliability==DART_RELIABLE);
    uint16_t index = i_dart_wire_index_of(st, topic_index);
    if (!w || !w->used || st->peer_dormant[peer_slot]) return 0;   /* unmatched or dormant */

    /* a fire and forget lane stamps a private wire seqno. Reliable and directed lanes keep
       the global line, since repair and the directed skip HB index history by it */
    int per_lane = (!w->reader_reliable && !topic->directed);

    /* directed: derive owed skips first, which also catches up a lane that was dormant */
    i_dart_writer_lane_advance(topic, w, (uint32_t)peer_slot);

    /* 1. repair */
    if (reliable && w->has_nack){
        uint32_t i;
        for (i=0;i<DART_NACK_WINDOW;i++){
            if (w->nack_bits & (1u<<i)){
                uint64_t seqno=w->nack_base+i;
                i_DartWriterSample *s;
                if (seqno>=topic->next_seqno){                 /* nothing there */
                    w->nack_bits &= ~(1u<<i);
                    if (w->nack_bits==0) w->has_nack=0;
                    continue;
                }
                s=i_dart_sample_find(topic,seqno);
                if (s && topic->directed && s->dest_slot != DART__DEST_ALL
                      && s->dest_slot != (uint32_t)peer_slot){
                    /* addressed to another lane: never re serve it, a leaked directed sample
                       would reach the wrong pending call. Clear its bits and owe the floor HB. */
                    uint32_t j;
                    for (j=0;j<DART_NACK_WINDOW;j++){
                        uint64_t sq=w->nack_base+j;
                        if (sq>=s->base && sq<s->base+s->count) w->nack_bits &= ~(1u<<j);
                    }
                    if (w->nack_bits==0) w->has_nack=0;
                    i_dart_writer_lane_advance(topic,w,(uint32_t)peer_slot);
                    w->skip_hb = 1;
                    continue;
                }
                if (s){
#ifdef DART_SHM
                    if (st->peer_shm[peer_slot] && s->shm){   /* resend as one SHM-DATA */
                        uint32_t j;
                        if (cap < DART_SHM_DATA_BYTES) return 0;
                        for (j=0;j<DART_NACK_WINDOW;j++){
                            uint64_t sq=w->nack_base+j;
                            if (sq>=s->base && sq<s->base+s->count) w->nack_bits &= ~(1u<<j);
                        }
                        if (w->nack_bits==0) w->has_nack=0;
                        return i_dart_wire_mk_shm(out,index,s->base,s->count,s->desc);
                    }
#endif
                    {
                    uint16_t frag_index=(uint16_t)(seqno - s->base);
                    uint32_t offset=(uint32_t)frag_index*st->frag;
                    uint16_t payload_len=(uint16_t)((s->len-offset)<st->frag?(s->len-offset):st->frag);
                    if (cap < (size_t)(s->count==1?DART_HEADER_DATA_SINGLE:DART_HEADER_DATA_MULTI)+(size_t)payload_len) return 0;
                    w->nack_bits &= ~(1u<<i);
                    if (w->nack_bits==0) w->has_nack=0;
                    topic->repair_stats.frags_sent++; topic->repair_stats.frags_resent++;
                    return i_dart_wire_mk_data(out,index,seqno,s,frag_index,i_dart_sample_buf(s)+offset,payload_len);
                    }
                } else {
                    /* superseded: skip the reader past the dropped region with an HB whose
                       first is our floor, and keep still cached seqnos for later repair */
                    uint64_t floor = (topic->have_first?topic->first_seqno:topic->next_seqno);
                    uint32_t j;
                    if (cap < DART_HEADER_HB) return 0;
                    for (j=0;j<DART_NACK_WINDOW;j++)
                        if (w->nack_base+j < floor) w->nack_bits &= ~(1u<<j);
                    if (w->nack_bits==0) w->has_nack=0;
                    return i_dart_writer_hb(st,topic,w,index,out,cap,now);
                }
            }
        }
        w->has_nack=0;
    }

    /* the directed floor HB before any new data, so the reader's floor moves past the
       foreign seqnos first and the data that follows arrives in order */
    if (reliable && w->reader_reliable && w->skip_hb){
        size_t hb = i_dart_writer_hb(st,topic,w,index,out,cap,now);
        if (!hb) return 0;        /* did not fit: retry next pass, the flag stays */
        w->skip_hb = 0;
        return hb;
    }

    /* 2. push new data */
    if (w->sent_upto < topic->next_seqno){
        uint64_t seqno=w->sent_upto;
        i_DartWriterSample *s=i_dart_sample_find(topic,seqno);
        /* Rate throttle at a sample boundary only: hold until the tick, then decimate to the
           newest sample. wire_skip absorbs the skipped ones. See spec/transport.md. */
        if (per_lane && w->rate_interval_us && (!s || seqno == s->base)){
            if (now < w->rate_next_us){ i_dart_transport_arm_deadline(st, w->rate_next_us); return 0; }
            { i_DartWriterSample *newest = i_dart_sample_find(topic, topic->next_seqno - 1);
              if (newest && newest->base > seqno){
                  w->wire_skip += newest->base - seqno;      /* a paced skip: no perceived loss */
                  w->sent_upto = newest->base; seqno = w->sent_upto; s = newest;
              } }
            w->rate_next_us = now + w->rate_interval_us;
            i_dart_transport_arm_deadline(st, w->rate_next_us);   /* the next tick fires on time */
        }
        if (s){
#ifdef DART_SHM
            /* peer_shm is set before data flows, so this is a sample boundary: one SHM-DATA */
            if (st->peer_shm[peer_slot] && s->shm){
                if (cap < DART_SHM_DATA_BYTES) return 0;
                w->sent_upto = s->base + s->count;
                if (reliable && w->reader_reliable){
                    if (!w->rtt_probe){ w->rtt_probe = 1; w->rtt_probe_seq = w->sent_upto; w->rtt_probe_us = now; }
                    if (w->sent_upto >= topic->next_seqno) i_dart_writer_arm_tail(st, w, (uint32_t)peer_slot, now);
                }
                return i_dart_wire_mk_shm(out,index,per_lane ? s->base - w->wire_skip : s->base,
                                          s->count,s->desc);
            }
#endif
            {
            uint16_t frag_index=(uint16_t)(seqno - s->base);
            uint32_t offset=(uint32_t)frag_index*st->frag;
            uint16_t payload_len=(uint16_t)((s->len-offset)<st->frag?(s->len-offset):st->frag);
            if (cap < (size_t)(s->count==1?DART_HEADER_DATA_SINGLE:DART_HEADER_DATA_MULTI)+(size_t)payload_len) return 0;
            w->sent_upto++;
            topic->repair_stats.frags_sent++;                       /* new data */
            if (reliable && w->reader_reliable){
                /* the sample's last fragment arms the RTT probe: its in order ack comes one
                   round trip after this push */
                if (!w->rtt_probe && w->sent_upto == s->base + s->count){
                    w->rtt_probe = 1; w->rtt_probe_seq = w->sent_upto; w->rtt_probe_us = now; }
                if (w->sent_upto >= topic->next_seqno)
                    i_dart_writer_arm_tail(st, w, (uint32_t)peer_slot, now);   /* tail HB */
            }
            return i_dart_wire_mk_data(out,index,per_lane ? seqno - w->wire_skip : seqno,
                                       s,frag_index,i_dart_sample_buf(s)+offset,payload_len);
            }
        } else {
            /* fell out of the ring unsent. A fire and forget lane advances and its reader
               sees the wire seqno jump as loss. A reliable lane skips its reader with an HB. */
            if (per_lane){ w->sent_upto=(topic->have_first?topic->first_seqno:topic->next_seqno); return 0; }
            if (cap < DART_HEADER_HB) return 0;
            w->sent_upto=(topic->have_first?topic->first_seqno:topic->next_seqno);
            w->rtt_probe=0;
            return i_dart_writer_hb(st,topic,w,index,out,cap,now);
        }
    }

    /* 3. heartbeat: reliable, due, and the reader is behind. A caught up lane goes silent
       until new data or a resubscribe drops acked_upto again. */
    if (reliable && w->reader_reliable && now>=w->hb_next_us && w->acked_upto < topic->next_seqno)
        return i_dart_writer_hb(st,topic,w,index,out,cap,now);
    return 0;
}
#pragma endregion
#pragma region transport/reader.c
/* The reader path: ordering, reassembly, delivery and the ACKNACK emit. */


int dart_transport_subscriber_progress(DartTransportState *st, uint16_t topic_index, uint32_t peer,
                         uint64_t *base_seqno, uint32_t *have, uint32_t *total){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    int peer_slot; i_DartReaderProxy *r;
    if (!topic) return 0;
    peer_slot = i_dart_peer_slot(st, peer);
    if (peer_slot < 0) return 0;
    r = i_dart_reader_proxy_at(st,topic_index,peer_slot);
    if (!r || !r->used || !r->cur.active) return 0;      /* no message mid reassembly */
    if (base_seqno) *base_seqno = r->deliver_upto;       /* the head message starts here */
    if (total)      *total      = r->cur.count;
    if (have){
        uint32_t i, c=0;
        for (i=0;i<r->cur.count;i++) if (i_dart_bit_get(r->cur.bitmap,i)) c++;
        *have = c;
    }
    return 1;
}


/* Attributes each 0 to 1 arming of the ack to its trigger. Counted before ack_pending is set. */
static void i_dart_reader_arm(i_DartTopic *topic, i_DartReaderProxy *r, int is_hb){
    if (!r->ack_pending){ if (is_hb) topic->repair_stats.arms_hb++; else topic->repair_stats.arms_data++; }
}

/* Arms an immediate ACKNACK. force owes a cumulative ack even at an unchanged floor. */
static void i_dart_reader_ack_now(DartTransportState *st, i_DartTopic *topic, i_DartReaderProxy *r,
                                  uint16_t topic_index, uint32_t peer_slot, int force, int is_hb){
    i_dart_reader_arm(topic, r, is_hb);
    r->ack_pending = 1; r->ack_due_us = 0;
    if (force) r->ack_force = 1;
    i_dart_lane_wake(st, topic_index, peer_slot);
}


/* The head sample (cur) and one sample held ahead of it (next). Nothing here moves
 * deliver_upto, only a delivery, the floor and the lapped rule do. See spec/transport.md. */

/* fit a slot's buffers to a sample, 0 = allocation failed */
static int i_dart_asm_fit(DartTransportState *st, i_DartAssembly *a, uint32_t len, uint16_t count){
    uint32_t bitmap_need = ((uint32_t)count + 7u) / 8u;
    if (a->cap < len){
        uint8_t *nb = (uint8_t*)st->cfg.allocator(st->cfg.user, a->buf, len?len:1u);
        if (!nb) return 0;
        a->buf = nb; a->cap = len?len:1u;
    }
    if (a->bitmap_cap < bitmap_need){
        uint8_t *nb = (uint8_t*)st->cfg.allocator(st->cfg.user, a->bitmap, bitmap_need?bitmap_need:1u);
        if (!nb) return 0;
        a->bitmap = nb; a->bitmap_cap = bitmap_need?bitmap_need:1u;
    }
    return 1;
}

/* is seqno s not held in slot a */
static int i_dart_asm_missing(const i_DartAssembly *a, uint64_t s){
    return !(a->active && s >= a->base && s < a->base + a->count && i_dart_bit_get(a->bitmap, (uint32_t)(s - a->base)));
}

/* deliver_upto moved: the ahead slot rotates into the head when it is exactly next, and
 * drops when the floor passed it. Eviction is whole sample, so a floor inside it cannot happen. */
static void i_dart_reader_settle(i_DartReaderProxy *r){
    if (!r->next.active) return;
    if (r->next.base < r->deliver_upto){ r->next.active = 0; return; }
    if (r->next.base == r->deliver_upto && !r->cur.active){
        i_DartAssembly t = r->cur; r->cur = r->next; r->next = t;
        r->next.active = 0;
    }
}

/* the ahead slot for a future sample, NULL when another sample occupies it */
static i_DartAssembly *i_dart_reader_ahead(i_DartReaderProxy *r, uint64_t base){
    if (!r->next.active || r->next.base == base) return &r->next;
    return NULL;
}

/* Hands up every complete sample from the head on. A reliable refusal parks the head, a
 * best effort one drops. Lane pointers are re derived after each callback. */
static void i_dart_reader_deliver(DartTransportState *st, uint16_t topic_index, uint32_t peer_slot){
    i_DartTopic *topic=&st->topics[topic_index];
    i_DartReaderProxy *r=i_dart_reader_proxy_at(st,topic_index,peer_slot);
    int reliable = (topic->qos.reliability==DART_RELIABLE);
    int delivered = 0;
    while (r->cur.active && r->cur.low == r->cur.count){
        int refused = st->cfg.on_message &&
                      st->cfg.on_message(st->cfg.user, topic_index, st->peer_ids[peer_slot],
                                         dart_bytes(r->cur.buf, r->cur.len)) != 0;
        r = i_dart_reader_proxy_at(st,topic_index,peer_slot);
        if (refused && reliable){
            /* park: no advance, no ack and no repair, so the writer's flow control
               backpressures the publisher. deliver_parked retries, a writer floor skips. */
            r->parked = 1;
            break;
        }
        r->deliver_upto = r->cur.base + r->cur.count;
        r->cur.active = 0; r->lapped = 0; delivered = 1;
        i_dart_reader_settle(r);
    }
    if (delivered && reliable)              /* ack after delivery, the reader owns copy invariant */
        i_dart_reader_ack_now(st,topic,r,topic_index,peer_slot,1,0);
}


static i_DartReaderOrder i_dart_reader_order_arrival(DartTransportState *st, int topic_index, int peer_slot,
                                             i_DartReaderProxy *r, uint64_t base, uint64_t top){
    i_DartTopic *topic=&st->topics[topic_index];
    if (base < r->deliver_upto) return DART_ORDER_OLD;
    if (top > r->received_high) r->received_high = top;          /* proof these seqnos exist */
    if (base > r->deliver_upto){
        if (topic->qos.reliability==DART_RELIABLE && r->started){   /* gap: arm a repair NACK */
            /* an armed lane keeps its deadline and is only re woken. A parked lane stays silent */
            if (!r->parked){
                if (r->ack_pending) i_dart_lane_wake(st,(uint16_t)topic_index,(uint32_t)peer_slot);
                else i_dart_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,0,0);
            }
            return DART_ORDER_GAP;
        }
        if (r->started && !topic->directed){   /* adopt, a directed skip is no loss */
            i_dart_transport_fire_event(st, DART_TRANSPORT_MSG_LOST, (uint16_t)topic_index, st->peer_ids[peer_slot],
                        r->deliver_upto, base - r->deliver_upto);
            topic->repair_stats.msgs_skipped += base - r->deliver_upto;
        }
        r->deliver_upto = base;
        return DART_ORDER_ADOPTED;
    }
    return DART_ORDER_INORDER;
}

#ifdef DART_SHM

/* An SHM-DATA covers its whole range, so there is no reassembly, only ordering. The
 * descriptor goes to on_shm and the node delivers. An SHM sample is never held ahead. */
void i_dart_reader_shm(DartTransportState *st, int topic_index, int peer_slot, const uint8_t *p, uint64_t now){
    i_DartTopic *topic=&st->topics[topic_index];
    i_DartReaderProxy *r=i_dart_reader_proxy_at(st,topic_index,peer_slot);
    int reliable = (topic->qos.reliability==DART_RELIABLE);
    uint64_t base = i_dart_le_r64(p+DART_OFFSET_SEQNO);
    uint16_t count = i_dart_le_r16(p+DART_OFFSET_SHM_COUNT);
    const uint8_t *desc = p+DART_OFFSET_SHM_DESC;
    if (!r || !r->used || count==0) return;
    if (r->parked){ topic->repair_stats.frags_ahead++; return; }   /* a held sample blocks */
    {   i_DartReaderOrder ord = i_dart_reader_order_arrival(st, topic_index, peer_slot, r, base, base+count-1);
        if (ord==DART_ORDER_OLD || ord==DART_ORDER_GAP) return;   /* old, dup, or gap armed */
    }
    r->started = 1; r->cur.active = 0;   /* a partial inline assembly of it is superseded */
    if (r->rtt_probe && r->rtt_probe_seq >= base && r->rtt_probe_seq < base + count){
        i_dart_rtt_sample(st, (uint32_t)peer_slot, now > r->rtt_probe_us ? now - r->rtt_probe_us : 0u);
        r->rtt_probe = 0;
    }
    /* Advance and ack only if the node delivered. A failed resolve leaves the gap for the
       reliability layer, and after DART_SHM_MAX_RETRY tries it is skipped loudly. */
    {   int ok = st->cfg.on_shm ?
                 st->cfg.on_shm(st->cfg.user, (uint16_t)topic_index, st->peer_ids[peer_slot], desc) : 0;
        r = i_dart_reader_proxy_at(st,topic_index,peer_slot);   /* the callback may re enter */
        if (ok > 0){
            r->shm_fail = 0; r->lapped = 0;
            r->deliver_upto = base + count;
            i_dart_reader_settle(r);
            if (reliable)                   /* ack after delivery */
                i_dart_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,1,0);
            i_dart_reader_deliver(st,(uint16_t)topic_index,(uint32_t)peer_slot);   /* held ahead */
            return;
        }
        if (ok < 0){                        /* refused downstream */
            if (!reliable){                 /* best effort drops */
                r->deliver_upto = base + count;
                i_dart_reader_settle(r);
                return;
            }
            /* park the descriptor, copied since p is the shared RX buffer. An alloc failure
               falls through to the repair path: the writer resends and we retry. */
            if (i_dart_asm_fit(st, &r->cur, DART_SHM_DESC_BYTES, 1u)){
                memcpy(r->cur.buf, desc, DART_SHM_DESC_BYTES);
                r->cur.base = base; r->cur.count = count;   /* seqnos the held sample spans */
                r->parked = 1; r->parked_shm = 1;           /* silent */
                return;
            }
        }
        if (reliable && ++r->shm_fail >= DART_SHM_MAX_RETRY){
            i_dart_transport_fire_event(st, DART_TRANSPORT_MSG_LOST, (uint16_t)topic_index, st->peer_ids[peer_slot],
                        base, count);
            topic->repair_stats.msgs_skipped += count;
            r->shm_fail = 0;
            r->deliver_upto = base + count;             /* give up: skip it and ack the new edge */
            i_dart_reader_settle(r);
            i_dart_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,1,0);
        } else if (reliable){                           /* leave the gap and NACK for a resend */
            i_dart_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,0,0);
        } else {
            r->deliver_upto = base + count;             /* best effort: no repair */
            i_dart_reader_settle(r);
            i_dart_lane_wake(st,(uint16_t)topic_index,(uint32_t)peer_slot);
        }
    }
}
#endif


void i_dart_reader_data(DartTransportState *st, int topic_index, int peer_slot, const uint8_t *p,
                           uint64_t now){
    i_DartTopic *topic=&st->topics[topic_index];
    i_DartReaderProxy *r=i_dart_reader_proxy_at(st,topic_index,peer_slot);
    i_DartAssembly *a;
    int reliable = (topic->qos.reliability==DART_RELIABLE);
    int new_frag = 0;
    uint64_t seqno, base; uint16_t frag, count, payload_len; uint32_t sample_len; const uint8_t *payload;
    if (p[0] & DART_F_SINGLE){           /* single fragment: frag, count and len are implied */
        seqno=i_dart_le_r64(p+DART_OFFSET_SEQNO); frag=0; count=1; payload_len=i_dart_le_r16(p+DART_OFFSET_PAYLOAD_LEN_SINGLE); sample_len=payload_len; payload=p+DART_HEADER_DATA_SINGLE;
    } else {
        seqno=i_dart_le_r64(p+DART_OFFSET_SEQNO); frag=i_dart_le_r16(p+DART_OFFSET_FRAG); count=i_dart_le_r16(p+DART_OFFSET_COUNT);
        sample_len=i_dart_le_r32(p+DART_OFFSET_SAMPLE_LEN); payload_len=i_dart_le_r16(p+DART_OFFSET_PAYLOAD_LEN); payload=p+DART_HEADER_DATA_MULTI;
    }
    base = seqno - frag;

    if (!r || !r->used){ topic->repair_stats.frags_malformed++; return; }     /* not subscribed */
    if (count==0 || frag>=count){ topic->repair_stats.frags_malformed++; return; }  /* malformed */
    switch (i_dart_reader_order_arrival(st, topic_index, peer_slot, r, base, seqno)){
        case DART_ORDER_OLD:     topic->repair_stats.frags_old++;   return;   /* already seen */
        case DART_ORDER_GAP:   /* a future sample: hold one ahead */
            a = i_dart_reader_ahead(r, base);
            if (!a){ topic->repair_stats.frags_ahead++; return; }   /* hold taken */
            break;
        case DART_ORDER_ADOPTED: topic->repair_stats.frags_ahead++; r->cur.active=0; a=&r->cur; break;
        case DART_ORDER_INORDER:
            if (r->parked){ topic->repair_stats.frags_dup++; return; }   /* the held sample */
            a=&r->cur; break;
    }
    r->started = 1;   /* the writer is engaged */
    /* too big means the allocation failed: the head is skipped and reported, a sample
       held ahead is simply not held and comes back in order later */
    if (!i_dart_asm_fit(st, a, sample_len, count)){
        if (a == &r->next){ topic->repair_stats.frags_ahead++; return; }
        i_dart_transport_fire_event(st, DART_TRANSPORT_MSG_TOO_BIG, (uint16_t)topic_index, st->peer_ids[peer_slot],
                    0, sample_len);
        r->deliver_upto = base + count; r->cur.active = 0;
        i_dart_reader_settle(r);
        if (reliable) i_dart_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,1,0);
        i_dart_reader_deliver(st,(uint16_t)topic_index,(uint32_t)peer_slot);
        return;
    }
    if (!a->active){
        /* nack_high is not reset: a whole message request may be in flight and a fresh
           cursor would re ask all of it. The refill asks from the higher of the two. */
        a->active=1; a->base=base; a->count=count; a->len=sample_len; a->low=0;
        memset(a->bitmap,0,((uint32_t)count + 7u) / 8u);
    }
    if (count!=a->count) return;                             /* inconsistent, ignore */
    topic->repair_stats.frags_recv++;   /* every accepted fragment, dups included */
    new_frag = !i_dart_bit_get(a->bitmap,frag);
    if (new_frag){
        /* reassemble at the source peer's fragment size, the cap guard catches a stray offset */
        uint32_t offset=(uint32_t)frag*st->peer_frag[peer_slot];
        if (offset+payload_len<=a->cap) memcpy(a->buf+offset,payload,payload_len);
        i_dart_bit_set(a->bitmap,frag);
        if (frag==a->low)                                    /* extended the contiguous front */
            while (a->low<count && i_dart_bit_get(a->bitmap,a->low)) a->low++;
        if (r->rtt_probe && seqno == r->rtt_probe_seq){      /* the resend our probe timed */
            i_dart_rtt_sample(st, (uint32_t)peer_slot, now > r->rtt_probe_us ? now - r->rtt_probe_us : 0u);
            r->rtt_probe = 0;
        }
    } else topic->repair_stats.frags_dup++;                  /* already held */
    if (a != &r->cur) return;    /* held ahead: it delivers when the head does */
    /* Deliver a complete head. A partial head re arms only when this frag opened a real
       gap below received_high, so a healthy in order fill owes nothing. */
    if (a->low == count) i_dart_reader_deliver(st,(uint16_t)topic_index,(uint32_t)peer_slot);
    else if (reliable && new_frag && r->deliver_upto + a->low <= r->received_high)
        i_dart_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,0,0);
}


void i_dart_reader_hb(DartTransportState *st, int topic_index, int peer_slot, const uint8_t *p, uint64_t now){
    i_DartTopic *topic=&st->topics[topic_index];
    i_DartReaderProxy *r=i_dart_reader_proxy_at(st,topic_index,peer_slot);
    uint64_t first=i_dart_le_r64(p+DART_OFFSET_SEQNO), last=i_dart_le_r64(p+DART_OFFSET_HB_LAST);
    (void)now;
    if (!r || !r->used) return;
    if (topic->qos.reliability!=DART_RELIABLE) return;
    /* An unstarted reader adopts no position from a heartbeat. A first inside the sample
       being assembled or parked is our own mid message ack echoed back, so it is ignored. */
    if (r->started && first > r->deliver_upto &&
        (!(r->cur.active || r->parked) || first >= r->deliver_upto + r->cur.count)){
        uint64_t to = first;
        if (!topic->directed){   /* directed: not real loss, skip silently */
            /* lapped: the floor passed a sample being fetched twice with no delivery between,
               so rejoin at the writer's head. A parked skip is the consumer's, not loss. */
            if (!r->parked){
                if (r->lapped && last + 1 > to) to = last + 1;
                r->lapped = 1;
            }
            i_dart_transport_fire_event(st, DART_TRANSPORT_MSG_LOST, (uint16_t)topic_index, st->peer_ids[peer_slot],
                        r->deliver_upto, to - r->deliver_upto);
            topic->repair_stats.msgs_skipped += to - r->deliver_upto;
        }
        r->deliver_upto=to; r->cur.active=0; r->rtt_probe=0;
        r->parked=0;            /* the writer moved past the held sample */
#ifdef DART_SHM
        r->parked_shm=0;
        r->shm_fail=0;          /* a fresh count */
#endif
        /* the floor may have landed on the sample held ahead: it is the head now */
        i_dart_reader_settle(r);
        i_dart_reader_deliver(st,(uint16_t)topic_index,(uint32_t)peer_slot);
        r=i_dart_reader_proxy_at(st,topic_index,peer_slot);
    }
    if (r->parked) return;      /* still parked: silent, the writer's flow control backpressures */
    /* hb_last is the writer's claim and only the slow backstop chases it, for tail loss.
       The HB always owes a cumulative ack so a writer that lost ours stops pinging. */
    r->hb_last=last;
    i_dart_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,1,1);
}


/* The ACKNACK for a lane if due, 0 if none. Repair is gap triggered, bounded by what was
 * received and deduped in flight. See spec/transport.md. */
size_t i_dart_reader_emit(DartTransportState *st, int topic_index, int peer_slot, uint8_t *out, size_t cap, uint64_t now){
    i_DartTopic *topic=&st->topics[topic_index];
    i_DartReaderProxy *r=i_dart_reader_proxy_at(st,topic_index,peer_slot);
    uint64_t first_missing, bound, top; uint16_t nbits=0; uint32_t bitmap=0;
    uint16_t index = i_dart_wire_index_of(st, topic_index);
    int due, holes=0, repair=0, force;
    if (!r || !r->used || st->peer_dormant[peer_slot]) return 0;   /* unmatched or dormant */
    if (topic->qos.reliability!=DART_RELIABLE) return 0;
    if (cap<DART_HEADER_NACK) return 0;
    if (!r->ack_pending || now<r->ack_due_us) return 0;
    r->ack_pending=0; force=r->ack_force; r->ack_force=0;
    if (r->parked) return 0;    /* parked: silent, and the lane stays unscheduled */

    if (!r->started)   /* no position yet: F_UNPOS announces our epoch */
        return i_dart_wire_mk_nack(out,index,r->deliver_upto,0,0,r->epoch,(uint8_t)DART_F_UNPOS);

    /* the cumulative ack point and repair window base: our contiguous front */
    first_missing = r->cur.active ? r->deliver_upto + r->cur.low : r->deliver_upto;

    /* only the slow backstop may reach the writer's claim, so tail loss still repairs */
    due   = (now >= r->nack_retransmit_us);
    bound = r->received_high;
    if (due && r->hb_last > bound) bound = r->hb_last;

    if (first_missing <= bound){                         /* a hole sits below something we heard */
        holes = 1;
        top = first_missing + DART_NACK_WINDOW;          /* one window per ACKNACK */
        if (top > bound + 1) top = bound + 1;
        {   /* ask only for what we can hold: the head, and the held sample when it follows
               the head directly or the head is still unknown */
            uint64_t held_end = 0;
            if (r->cur.active) held_end = r->cur.base + r->cur.count;
            if (r->next.active && (!r->cur.active || r->next.base == held_end))
                held_end = r->next.base + r->next.count;
            if (held_end && top > held_end) top = held_end;
        }
        {   /* in flight dedup is valid only while assembling this message. With no head, and
               on the slow backstop, everything is asked from the floor. */
            uint64_t from = (due || !r->cur.active) ? first_missing
                : (r->nack_high > first_missing ? r->nack_high : first_missing);
            uint64_t s, probe = 0, asked_high = r->nack_high;   /* at or past it = never asked */
            int have_probe = 0;
            for (s=from; s<top; s++){
                if (i_dart_asm_missing(&r->cur, s) && i_dart_asm_missing(&r->next, s)){
                    bitmap |= (1u << (uint32_t)(s - first_missing));
                    if (!have_probe && s >= asked_high){ probe = s; have_probe = 1; }
                }
            }
            if (bitmap){
                /* the backstop: the peer's measured round trip unless the qos pins it */
                uint32_t rto = topic->qos.repair_delay_us ? topic->qos.repair_delay_us
                             : i_dart_rtt_rto(st, (uint32_t)peer_slot, DART_QOS_DEF_REPAIR_US);
                nbits = (uint16_t)(top - first_missing);
                repair = 1;
                topic->repair_stats.nacks_sent++;
                if (top > r->nack_high) r->nack_high = top;
                r->nack_retransmit_us = now + rto;
                /* the probe times the first ask of one seqno. A re ask or a passed floor
                   disarms it, and the lowest first ask of this request arms the next. */
                if (r->rtt_probe && (r->rtt_probe_seq < first_missing ||
                    (r->rtt_probe_seq < top && ((bitmap >> (uint32_t)(r->rtt_probe_seq - first_missing)) & 1u))))
                    r->rtt_probe = 0;
                if (!r->rtt_probe && have_probe){ r->rtt_probe = 1; r->rtt_probe_seq = probe; r->rtt_probe_us = now; }
            }
        }
    } else r->nack_high = first_missing;                   /* caught up: end the episode */

    /* keep the lane live while a hole remains so the backstop re fires */
    if (holes){ r->ack_pending=1; r->ack_due_us=r->nack_retransmit_us; i_dart_transport_arm_deadline(st,r->ack_due_us); }

    /* only a repair request or a forced cumulative ack is worth a datagram */
    if (!repair && !force) return 0;
    return i_dart_wire_mk_nack(out,index,first_missing,nbits,bitmap,r->epoch,0);
}


/* Retries every parked lane of a topic. The callbacks may re enter the transport, so lane
 * pointers are re derived after each call. */
uint32_t dart_transport_deliver_parked(DartTransportState *st, uint16_t topic_index, uint64_t now){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    uint32_t li, still = 0;
    (void)now;
    if (!topic) return 0;
    li = topic->lane_head;
    while (li != DART__NIL){
        uint32_t next = st->lanes[li].topic_next;
        if (st->lanes[li].r.used && st->lanes[li].r.parked){
            uint32_t peer_slot = st->lanes[li].peer_slot;
            uint32_t peer_id   = st->peer_ids[peer_slot];
            i_DartReaderProxy *r = &st->lanes[li].r;
            int accepted;
#ifdef DART_SHM
            if (r->parked_shm){
                int ok = st->cfg.on_shm ?
                         st->cfg.on_shm(st->cfg.user, topic_index, peer_id, r->cur.buf) : 0;
                r = &st->lanes[li].r;                  /* the callback may re enter */
                if (ok == 0){
                    /* the chunk is gone: un park, leave the gap, and let the repair path re
                       fetch or skip it */
                    r->parked = 0; r->parked_shm = 0; r->cur.active = 0;
                    i_dart_reader_ack_now(st,topic,r,topic_index,peer_slot,0,0);
                    li = next; continue;
                }
                accepted = (ok > 0);
                if (accepted){
                    r->deliver_upto = r->cur.base + r->cur.count;
                    r->cur.active = 0; r->parked = 0; r->parked_shm = 0; r->shm_fail = 0; r->lapped = 0;
                    i_dart_reader_settle(r);
                    if (topic->qos.reliability==DART_RELIABLE)    /* ack after delivery */
                        i_dart_reader_ack_now(st,topic,r,topic_index,peer_slot,1,0);
                    i_dart_reader_deliver(st,topic_index,peer_slot);
                }
            } else
#endif
            {
                /* un park and run the ordinary delivery, which re parks on a refusal */
                r->parked = 0;
                i_dart_reader_deliver(st,topic_index,peer_slot);
                accepted = !st->lanes[li].r.parked;
            }
            if (!accepted) still++;
        }
        li = next;
    }
    return still;
}
#pragma endregion
#pragma region transport/core.c
/* The transport core: state, init, peers, interest matching, the RX demux and queries.
 * The codec, scheduler, writer and reader paths live in the sibling files. */
#include <string.h>


/* The wire name is the whole name, so the identity recomputed from it equals dart_topic_id. */
static uint64_t i_dart_identity_hash(const uint8_t *name, size_t n){ return i_dart_fnv1a64(name, n); }

uint64_t dart_topic_id(const char *name){ return i_dart_fnv1a64_str(name); }

uint64_t dart_topic_identity(const DartTopicDef *def){
    return dart_topic_id(def->name);
}

static size_t i_dart_name_len(const char *s){            /* capped strlen */
    size_t n = 0;
    if (s) while (s[n] && n < DART_TOPIC_NAME_MAX) n++;
    return n;
}


/* Applied once at init so the stored qos is authoritative. repair_delay_us 0 stays
   adaptive and is resolved per NACK in i_dart_reader_emit. */
static void i_dart_qos_defaults(DartQos *q){
    if (q->keep_last == 0)        q->keep_last       = q->reliability==DART_RELIABLE
                                                     ? DART_QOS_DEF_KEEP_LAST_REL
                                                     : DART_QOS_DEF_KEEP_LAST;
    if (q->heartbeat_us == 0)     q->heartbeat_us    = DART_QOS_DEF_HEARTBEAT_US;
}


uint16_t dart_clamp_frag(uint16_t frag_size){
    uint16_t f = frag_size ? frag_size : DART_FRAG_SIZE;
    if (f < DART_FRAG_SIZE_MIN) f = DART_FRAG_SIZE_MIN;
    if (f > DART_FRAG_SIZE_MAX) f = DART_FRAG_SIZE_MAX;
    return f;
}

uint16_t dart_transport_frag(DartTransportState *st){ return st ? st->frag : dart_clamp_frag(0); }


/* Lays everything out, measure mode when b->base is NULL. The arena holds only the fixed
   tables. Message buffers, reassembly state, index maps and lane records are hook allocations. */
static DartTransportState *i_dart_transport_build(i_DartBump *b, const DartConfig *cfg){
    uint16_t c; uint32_t max_peers = cfg->max_peers, n_topics = cfg->n_topics;
    uint16_t bitmap_len = (uint16_t)((n_topics+7u)/8u);
    uint32_t name_bytes = 0; char *name_pool = NULL;
    DartTransportState *st = (DartTransportState*)i_dart_bump_take(b, sizeof(DartTransportState), 16);
    if (st && b->base) memset(st, 0, sizeof(*st));

    /* one fixed name slot per topic, so a reserve slot can be named later */
    name_bytes = (uint32_t)n_topics * (DART_TOPIC_NAME_MAX + 1u);

    { uint32_t nlanes = n_topics*max_peers, ndest = max_peers;
      uint32_t *peer_ids = (uint32_t*)i_dart_bump_take(b, max_peers*sizeof(uint32_t), 8);
      uint8_t  *peer_used = (uint8_t*) i_dart_bump_take(b, max_peers*sizeof(uint8_t), 1);
      uint8_t  *peer_dormant= (uint8_t*) i_dart_bump_take(b, max_peers*sizeof(uint8_t), 1);
      uint16_t *peer_frag = (uint16_t*)i_dart_bump_take(b, max_peers*sizeof(uint16_t), 2);
      DartPeerRtt *peer_rtt = (DartPeerRtt*)i_dart_bump_take(b, max_peers*sizeof(DartPeerRtt), 8);
#ifdef DART_SHM
      uint8_t  *peer_shm= (uint8_t*) i_dart_bump_take(b, max_peers*sizeof(uint8_t), 1);
#endif
      uint8_t  *peer_pub_bitmap = (uint8_t*) i_dart_bump_take(b, (size_t)max_peers*bitmap_len, 1);
      uint8_t  *peer_sub_bitmap = (uint8_t*) i_dart_bump_take(b, (size_t)max_peers*bitmap_len, 1);
      uint8_t  *peer_sub_reliable = (uint8_t*) i_dart_bump_take(b, (size_t)max_peers*bitmap_len, 1);
      i_DartTopic *topic = (i_DartTopic*)i_dart_bump_take(b, n_topics*sizeof(i_DartTopic), 16);
      /* only the u16 ticket table lives in the arena, records are pool allocated per match */
      uint16_t   *lane_index = (uint16_t*)i_dart_bump_take(b, (size_t)nlanes*sizeof(uint16_t), 2);
      uint32_t *dest_head = (uint32_t*)i_dart_bump_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint32_t *dest_tail = (uint32_t*)i_dart_bump_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint8_t  *dest_queued = (uint8_t*) i_dart_bump_take(b, (size_t)ndest, 1);
      uint32_t *dest_queue = (uint32_t*)i_dart_bump_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      /* index maps: a per peer pointer and length, each map allocated on demand */
      uint16_t **peer_index    = (uint16_t**)i_dart_bump_take(b, (size_t)max_peers*sizeof(uint16_t*), 8);
      uint32_t *peer_index_len = (uint32_t*) i_dart_bump_take(b, (size_t)max_peers*sizeof(uint32_t), 8);
      uint8_t **peer_astate    = (uint8_t**) i_dart_bump_take(b, (size_t)max_peers*sizeof(uint8_t*), 8);
      uint8_t **peer_agen      = (uint8_t**) i_dart_bump_take(b, (size_t)max_peers*sizeof(uint8_t*), 8);
      uint8_t **peer_attrs     = (uint8_t**) i_dart_bump_take(b, (size_t)max_peers*sizeof(uint8_t*), 8);
      uint32_t *peer_seen_version = (uint32_t*)i_dart_bump_take(b, (size_t)max_peers*sizeof(uint32_t), 8);
      name_pool = (char*)i_dart_bump_take(b, name_bytes ? name_bytes : 1u, 1);
      if (st && b->base){
          st->cfg=*cfg; st->peer_ids=peer_ids; st->peer_used=peer_used;
          st->peer_dormant=peer_dormant; st->peer_frag=peer_frag;
          st->peer_rtt=peer_rtt; memset(peer_rtt, 0, max_peers*sizeof(DartPeerRtt));
          st->frag = dart_clamp_frag(cfg->frag_size);
          st->peer_pub_bitmap=peer_pub_bitmap; st->peer_sub_bitmap=peer_sub_bitmap;
          st->peer_sub_reliable=peer_sub_reliable; st->bitmap_len=bitmap_len;
          st->topics=topic; st->reader_epoch_counter=1;
          st->next_deadline_us=DART__NO_DEADLINE;
          st->lanes=NULL; st->lane_cap=0u; st->lane_free=DART__NIL;
          st->lane_index=lane_index;
          st->dest_head=dest_head; st->dest_tail=dest_tail; st->dest_queued=dest_queued; st->dest_queue=dest_queue;
          st->peer_index=peer_index; st->peer_index_len=peer_index_len;
          st->peer_astate=peer_astate;
          st->peer_agen=peer_agen; st->peer_attrs=peer_attrs;
          st->peer_seen_version=peer_seen_version;
          memset(peer_used,0,max_peers); memset(peer_dormant,0,max_peers);
          { uint32_t k; for (k=0;k<max_peers;k++) peer_frag[k]=DART_FRAG_SIZE; }
#ifdef DART_SHM
          st->peer_shm=peer_shm; memset(peer_shm,0,max_peers);
#endif
          memset(peer_index, 0, (size_t)max_peers*sizeof(uint16_t*));
          memset(peer_index_len, 0, (size_t)max_peers*sizeof(uint32_t));
          memset(peer_astate, 0, (size_t)max_peers*sizeof(uint8_t*));
          memset(peer_agen, 0, (size_t)max_peers*sizeof(uint8_t*));
          memset(peer_attrs, 0, (size_t)max_peers*sizeof(uint8_t*));
          memset(peer_seen_version, 0, (size_t)max_peers*sizeof(uint32_t));
          memset(peer_pub_bitmap,0,(size_t)max_peers*bitmap_len); memset(peer_sub_bitmap,0,(size_t)max_peers*bitmap_len);
          memset(peer_sub_reliable,0,(size_t)max_peers*bitmap_len);
          memset(lane_index,0xFF,(size_t)nlanes*sizeof(uint16_t));   /* all unmatched */
          memset(dest_queued,0,ndest);
          memset(dest_head,0xFF,(size_t)ndest*sizeof(uint32_t));   /* all DART__NIL */
      }
    }

    for (c=0;c<n_topics;c++){
        /* every slot starts inactive with its own name slot */
        if (st && b->base){
            i_DartTopic *topic = &st->topics[c];
            memset(topic,0,sizeof(*topic));
            topic->role = DART_INACTIVE;
            topic->lane_head = DART__NIL;
            topic->name = name_pool + (size_t)c*(DART_TOPIC_NAME_MAX + 1u);
            ((char*)topic->name)[0] = '\0';
        }
        if (!cfg->topics) continue;    /* reserve mode: slots are filled by topic_define later */
        {   const DartTopicDef *def = &cfg->topics[c];
            DartQos q = def->qos;            /* a normalized copy */
            i_DartWriterSample *history; uint16_t depth;
            i_dart_qos_defaults(&q);
            depth = q.keep_last;
            history = (i_DartWriterSample*)i_dart_bump_take(b, depth*sizeof(i_DartWriterSample), 16);
            if (st && b->base){
                i_DartTopic *topic = &st->topics[c];
                size_t lane = i_dart_name_len(def->name);
                topic->qos=q;
                topic->role=def->role;
                topic->kind=def->kind; topic->prefix_bytes=def->prefix_bytes; topic->directed=def->directed;
                topic->attrs=def->attrs;
                topic->identity = dart_topic_identity(def);
                if (lane){ memcpy((char*)topic->name, def->name, lane); ((char*)topic->name)[lane]='\0'; }
                topic->name_len = (uint8_t)lane;
                topic->history=history; topic->history_owned=0; topic->history_head=0; topic->next_seqno=0; topic->have_first=0;
                memset(history,0,depth*sizeof(i_DartWriterSample));   /* buffers grow lazily */
            }
        }
    }
    return st;
}


size_t dart_transport_required_memory(const DartConfig *cfg){
    i_DartBump b; memset(&b,0,sizeof b);
    if (!cfg || cfg->n_topics==0 || cfg->max_peers==0) return 0;
    i_dart_transport_build(&b, cfg);
    return b.offset + 16;   /* slack for base alignment */
}


DartTransportState *dart_transport_init(void *mem, size_t cap, const DartConfig *cfg){
    i_DartBump b; DartTransportState *st; uint16_t i;
    if (!mem || !cfg || cfg->n_topics==0 || cfg->max_peers==0) return NULL;
    if (!cfg->allocator) return NULL;    /* the allocator is the one memory model */
    if (cfg->topics) for (i=0;i<cfg->n_topics;i++){
        const DartTopicDef *d = &cfg->topics[i];
        size_t lane = 0; uint16_t j;
        if (!d->name || !d->name[0]) return NULL;          /* the name is the identity */
        while (d->name[lane]) lane++;
        if (lane > DART_TOPIC_NAME_MAX) return NULL;           /* the wire name is the whole name */
        if (d->directed && d->qos.catch_up) return NULL;   /* directed history never replays */
        for (j=0;j<i;j++)   /* same name under a different kind: the maps bind by identity, so
                               local twins would cross bind. See spec/interest.md */
            if (cfg->topics[j].kind != d->kind &&
                dart_topic_id(cfg->topics[j].name) == dart_topic_id(d->name)) return NULL;
    }
    memset(&b,0,sizeof b);
    b.base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    b.cap  = cap - (size_t)((uint8_t*)b.base - (uint8_t*)mem);
    st = i_dart_transport_build(&b, cfg);
    if (!st || b.oom) return NULL;
    st->cfg.topics = NULL;   /* only read during init */
    return st;
}


/* Heap buffers stay put and the struct copies carry their pointers. The scheduler is
 * rebuilt from proxy state. The caller frees old's arena but must not destroy old. */
DartTransportState *dart_transport_migrate(DartTransportState *old, void *new_mem, size_t new_cap,
                        uint16_t new_max_peers, uint16_t new_n_topics){
    DartConfig nc; DartTransportState *nw; uint16_t omp, onc, c, p;
    if (!old) return NULL;
    nc = old->cfg; nc.topics = NULL;
    nc.max_peers = new_max_peers; nc.n_topics = new_n_topics;
    nw = dart_transport_init(new_mem, new_cap, &nc);
    if (!nw) return NULL;
    omp = old->cfg.max_peers; onc = old->cfg.n_topics;

    nw->reader_epoch_counter = old->reader_epoch_counter;
    nw->frag = old->frag;
    memcpy(nw->peer_ids,     old->peer_ids,     (size_t)omp*sizeof(uint32_t));
    memcpy(nw->peer_used,    old->peer_used,    omp);
    memcpy(nw->peer_dormant, old->peer_dormant, omp);
    memcpy(nw->peer_frag,    old->peer_frag,    (size_t)omp*sizeof(uint16_t));
    memcpy(nw->peer_rtt,     old->peer_rtt,     (size_t)omp*sizeof(DartPeerRtt));
#ifdef DART_SHM
    memcpy(nw->peer_shm,     old->peer_shm,     omp);
#endif
    /* keep the new name slot pointer, carry everything else, re copy the name */
    for (c=0;c<onc;c++){
        char *nm = (char*)nw->topics[c].name;
        size_t l = old->topics[c].name_len;
        nw->topics[c] = old->topics[c];
        nw->topics[c].name = nm;
        if (l) memcpy(nm, old->topics[c].name, l);
        nm[l] = '\0';
    }
    /* the pool is one hook allocation outside both arenas, so adopt it whole. Only the
       ticket table is arena memory and re strides. */
    nw->lanes = old->lanes; nw->lane_cap = old->lane_cap; nw->lane_free = old->lane_free;
    for (c=0;c<onc;c++) for (p=0;p<omp;p++)
        nw->lane_index[(size_t)c*new_max_peers+p] = old->lane_index[(size_t)c*omp+p];
    {   uint32_t li;   /* the old scheduler dies with the arena, free records keep sched_next
                       as their free link */
        for (li=0; li<nw->lane_cap; li++)
            if (nw->lanes[li].in_use){ nw->lanes[li].queued=0; nw->lanes[li].sched_next=DART__NIL; }
    }
    /* per peer interest bitmaps re stride with n_topics, the maps are stable hook allocations */
    for (p=0;p<omp;p++){
        memcpy(nw->peer_pub_bitmap + (size_t)p*nw->bitmap_len,
               old->peer_pub_bitmap + (size_t)p*old->bitmap_len, old->bitmap_len);
        memcpy(nw->peer_sub_bitmap + (size_t)p*nw->bitmap_len,
               old->peer_sub_bitmap + (size_t)p*old->bitmap_len, old->bitmap_len);
        memcpy(nw->peer_sub_reliable + (size_t)p*nw->bitmap_len,
               old->peer_sub_reliable + (size_t)p*old->bitmap_len, old->bitmap_len);
        nw->peer_index[p]     = old->peer_index[p];
        nw->peer_index_len[p] = old->peer_index_len[p];
        nw->peer_astate[p]    = old->peer_astate[p];
        nw->peer_agen[p]      = old->peer_agen[p];
        nw->peer_attrs[p]     = old->peer_attrs[p];
        nw->peer_seen_version[p] = old->peer_seen_version[p];
    }
    /* re enqueue every used lane and force a full sweep next poll, so timers re arm */
    for (c=0;c<onc;c++)
        for (p=0;p<omp;p++) if (nw->peer_used[p]) i_dart_lane_wake(nw, c, p);
    nw->next_deadline_us = 0;
    return nw;
}


int i_dart_peer_slot(DartTransportState *st, uint32_t id){
    uint16_t i;
    for (i=0;i<st->cfg.max_peers;i++) if (st->peer_used[i] && st->peer_ids[i]==id) return (int)i;
    return -1;
}

/* the local handle is the index */
i_DartTopic *i_dart_topic_at(DartTransportState *st, uint16_t topic_index, int *idx_out){
    if (topic_index >= st->cfg.n_topics) return NULL;
    if (idx_out) *idx_out = (int)topic_index;
    return &st->topics[topic_index];
}

/* Prefers a non INACTIVE match so a parked twin never shadows a live one, else the first
 * match so resolution stays deterministic. A RETIRED slot is invisible here. */
static i_DartTopic *i_dart_topic_by_identity(DartTransportState *st, uint64_t identity, int *idx_out){
    uint16_t i; int first=-1;
    for (i=0;i<st->cfg.n_topics;i++){
        if (st->topics[i].retired || st->topics[i].identity!=identity) continue;
        if (first<0) first=(int)i;
        if (st->topics[i].role!=DART_INACTIVE){ if(idx_out)*idx_out=(int)i; return &st->topics[i]; }
    }
    if (first>=0){ if(idx_out)*idx_out=first; return &st->topics[first]; }
    return NULL;
}


/* first and count are the kind's two numeric slots, routed to the named fields. */
void i_dart_transport_fire_event(DartTransportState *st, DartTransportEventKind kind, uint16_t topic_index,
                        uint32_t peer, uint64_t first, uint64_t count){
    DartTransportEvent ev;
    if (!st->cfg.on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind=kind; ev.topic=topic_index; ev.peer=peer; ev.user=st->cfg.user;
    switch (kind){
    case DART_TRANSPORT_MSG_LOST:       ev.lost_first = first; ev.lost_count = count; break;
    case DART_TRANSPORT_MSG_TOO_BIG:    ev.too_big_bytes = count; break;
    case DART_TRANSPORT_NAME_COLLISION: ev.identity = first; break;
    case DART_TRANSPORT_SCHEMA_MISMATCH: ev.peer_is_pub = (uint8_t)first; break;
    default: break;
    }
    st->cfg.on_event(&ev);
}


/* the head minus catch_up cached samples, reliable only */
uint64_t i_dart_topic_unicast_join_seqno(const i_DartTopic *topic){
    uint16_t depth = topic->qos.keep_last;   /* normalized at init */
    uint16_t want = topic->qos.catch_up, k, i;
    uint64_t s = topic->next_seqno;
    if (topic->qos.reliability != DART_RELIABLE || want == 0) return s;
    if (want > depth) want = depth;
    i = topic->history_head;
    for (k=0; k<want; k++){
        uint16_t j = (uint16_t)(i ? i-1 : depth-1);
        if (!topic->history[j].valid) break;        /* fewer than want cached */
        s = topic->history[j].base;
        i = j;
    }
    return s;
}


/* Gets or allocates the lane record. The pool doubles and records move, so callers re
 * derive pointers after this. NULL on OOM or a full ticket space, the next announce retries. */
static i_DartLane *i_dart_lane_ensure(DartTransportState *st, uint16_t c, uint32_t peer_slot){
    size_t k = (size_t)c*st->cfg.max_peers + peer_slot;
    uint32_t li;
    li = st->lane_index[k]==0xFFFFu ? DART__NIL : (uint32_t)st->lane_index[k];
    if (li != DART__NIL) return &st->lanes[li];
    if (st->lane_free == DART__NIL){                  /* pool dry: grow it */
        uint32_t ncap = st->lane_cap ? st->lane_cap*2u : 8u, i;
        i_DartLane *nl;
        if (ncap > 0xFFFFu) ncap = 0xFFFFu;           /* the u16 ticket space */
        if (ncap <= st->lane_cap) return NULL;        /* 65535 live matches: refuse */
        nl = (i_DartLane*)st->cfg.allocator(st->cfg.user, st->lanes, (size_t)ncap*sizeof(i_DartLane));
        if (!nl) return NULL;
        st->lanes = nl;
        for (i=ncap; i>st->lane_cap; i--){            /* thread the new slots onto the free list */
            nl[i-1].in_use = 0;
            nl[i-1].sched_next = st->lane_free;
            st->lane_free = i-1;
        }
        st->lane_cap = ncap;
    }
    li = st->lane_free; st->lane_free = st->lanes[li].sched_next;
    memset(&st->lanes[li], 0, sizeof(i_DartLane));
    st->lanes[li].topic = c; st->lanes[li].peer_slot = (uint16_t)peer_slot;
    st->lanes[li].sched_next = DART__NIL; st->lanes[li].topic_next = DART__NIL;
    st->lanes[li].in_use = 1;
    st->lane_index[k] = (uint16_t)li;
    return &st->lanes[li];
}

/* free one assembly slot's grown buffers */
static void i_dart_asm_free(DartTransportState *st, i_DartAssembly *a){
    if (a->buf){ st->cfg.allocator(st->cfg.user, a->buf, 0); a->buf=NULL; a->cap=0; }
    if (a->bitmap){ st->cfg.allocator(st->cfg.user, a->bitmap, 0); a->bitmap=NULL; a->bitmap_cap=0; }
    a->active=0;
}

/* A lane with neither side matched leaves the topic chain, frees its buffers, leaves the
 * scheduler and recycles its record. A no op while a side is matched. */
static void i_dart_lane_release(DartTransportState *st, uint16_t c, uint32_t peer_slot){
    uint32_t li = i_dart_lane_id(st, c, peer_slot);
    i_DartLane *l;
    if (li == DART__NIL) return;
    l = &st->lanes[li];
    if (l->w.used || l->r.used) return;
    {   uint32_t *pp = &st->topics[c].lane_head;    /* unlink from the topic chain */
        while (*pp != DART__NIL && *pp != li) pp = &st->lanes[*pp].topic_next;
        if (*pp == li) *pp = l->topic_next;
    }
    l->topic_next = DART__NIL;
    i_dart_asm_free(st, &l->r.cur); i_dart_asm_free(st, &l->r.next);
    i_dart_sched_drop(st, li);
    l->in_use = 0;
    l->sched_next = st->lane_free; st->lane_free = li;
    st->lane_index[(size_t)c*st->cfg.max_peers + peer_slot] = 0xFFFF;
}

/* match one lane side */
static void i_dart_writer_match(DartTransportState *st, uint16_t c, uint16_t peer_slot, i_DartLane *l){
    i_DartTopic *topic=&st->topics[c];
    i_DartWriterProxy *w=&l->w;
    memset(w,0,sizeof(*w));
    w->used=1;
    topic->matched_writers++;   /* a genuine 0 to 1, rematch guards on used */
    /* only a reliable reader acks. A best effort one stays out of flow control so it can
       never stall this writer */
    w->reader_reliable = i_dart_bit_get(&st->peer_sub_reliable[(size_t)peer_slot*st->bitmap_len], c) ? 1u : 0u;
    w->sent_upto = i_dart_topic_unicast_join_seqno(topic);
    w->acked_upto = w->sent_upto;
    w->wire_skip = w->sent_upto;   /* the private wire seqno starts at 0 */
    i_dart_lane_wake(st, c, peer_slot);   /* primed for new data and ack or hb */
}

static void i_dart_writer_unmatch(DartTransportState *st, uint16_t c, i_DartLane *l){
    if (!l->w.used) return;
    l->w.used=0;
    st->topics[c].matched_writers--;   /* exactly one 1 to 0 per unmatch */
}

static void i_dart_reader_match(DartTransportState *st, uint16_t c, uint16_t peer_slot, i_DartLane *l){
    i_DartReaderProxy *r=&l->r;
    i_DartAssembly cur=r->cur, next=r->next;   /* keep the grown buffers across rematch */
    memset(r,0,sizeof(*r));
    r->cur=cur; r->next=next; r->cur.active=0; r->next.active=0;
    r->epoch=st->reader_epoch_counter++;   /* a new incarnation: writers re join on seeing it */
    r->used=1;       /* started 0: the first DATA adopts the writer's position */
    st->topics[c].matched_readers++;   /* a genuine 0 to 1, rematch guards on used */
    /* announce the incarnation once so an idle writer re joins and replays. A discovery
       blip keeps its position through dormant and resume and never lands here. */
    if (st->topics[c].qos.reliability==DART_RELIABLE){
        r->ack_pending=1; r->ack_due_us=0; r->ack_force=1;
        i_dart_lane_wake(st,c,peer_slot);
    }
}

static void i_dart_reader_unmatch(DartTransportState *st, uint16_t c, i_DartLane *l){
    if (l->r.used) st->topics[c].matched_readers--;   /* peer_remove calls this unconditionally */
    l->r.used=0; l->r.cur.active=0; l->r.next.active=0;
}


/* Recomputes one lane from our role and the peer's bits. Only a changed side is touched,
 * so reader positions survive a re apply. */
static void i_dart_topic_rematch(DartTransportState *st, uint16_t c, uint16_t peer_slot){
    i_DartTopic *topic=&st->topics[c];
    const uint8_t *peer_pub_bitmap=&st->peer_pub_bitmap[(size_t)peer_slot*st->bitmap_len];
    const uint8_t *peer_sub_bitmap=&st->peer_sub_bitmap[(size_t)peer_slot*st->bitmap_len];
    int wuse = dart_role_pubs(topic->role) && i_dart_bit_get(peer_sub_bitmap,c);
    int ruse = dart_role_subs(topic->role) && i_dart_bit_get(peer_pub_bitmap,c);
    i_DartLane *l;
    /* the rebind hold: nothing goes to a peer until it proved it applied our announce at
       the slot's rebind version. Inbound needs no hold, our own verdicts were re pended. */
    if (wuse && topic->rebind_version && st->peer_seen_version[peer_slot] < topic->rebind_version)
        wuse = 0;
    l = i_dart_lane_at(st,c,peer_slot);
    int had = l && (l->w.used || l->r.used);
    if (!wuse && !ruse){
        if (!had) return;
        i_dart_writer_unmatch(st,c,l);
        i_dart_reader_unmatch(st,c,l);
        i_dart_lane_release(st,c,peer_slot);
        return;
    }
    if (!l){
        l = i_dart_lane_ensure(st,c,peer_slot);   /* may relocate the pool */
        if (!l) return;                           /* OOM: refused, the next announce retries */
    }
    if (wuse && !l->w.used) i_dart_writer_match(st,c,peer_slot,l);
    else if (!wuse && l->w.used) i_dart_writer_unmatch(st,c,l);
    if (ruse && !l->r.used) i_dart_reader_match(st,c,peer_slot,l);
    else if (!ruse && l->r.used) i_dart_reader_unmatch(st,c,l);
    if (!had && (l->w.used || l->r.used)){        /* a first match: onto the topic chain */
        l->topic_next = topic->lane_head;
        topic->lane_head = i_dart_lane_id(st,c,peer_slot);
    } else if (had && !(l->w.used || l->r.used)){
        i_dart_lane_release(st,c,peer_slot);
    }
}


void dart_transport_peer_add(DartTransportState *st, uint32_t id, uint16_t peer_frag){
    uint16_t i; int free=-1; uint32_t max_peers=st->cfg.max_peers;
    if (i_dart_peer_slot(st,id)>=0) return;
    for (i=0;i<max_peers;i++) if(!st->peer_used[i]){free=(int)i;break;}
    if (free<0) return;
    st->peer_used[free]=1; st->peer_ids[free]=id;
    memset(&st->peer_rtt[free], 0, sizeof(DartPeerRtt));   /* a new peer starts unmeasured */
    st->peer_dormant[free]=0;
    st->peer_frag[free]=dart_clamp_frag(peer_frag);
#ifdef DART_SHM
    st->peer_shm[free]=0;   /* the node sets it on attach */
#endif
    memset(&st->peer_pub_bitmap[(size_t)free*st->bitmap_len],0,st->bitmap_len);
    memset(&st->peer_sub_bitmap[(size_t)free*st->bitmap_len],0,st->bitmap_len);
    memset(&st->peer_sub_reliable[(size_t)free*st->bitmap_len],0,st->bitmap_len);
    if (st->peer_index[free] && st->peer_index_len[free]){   /* slot reuse: no stale state */
        memset(st->peer_index[free],0xFF,(size_t)st->peer_index_len[free]*sizeof(uint16_t));
        if (st->peer_astate[free]) memset(st->peer_astate[free],0,st->peer_index_len[free]);
        if (st->peer_agen[free])   memset(st->peer_agen[free],0,st->peer_index_len[free]);
        if (st->peer_attrs[free])  memset(st->peer_attrs[free],0,st->peer_index_len[free]);
    }
    st->peer_seen_version[free]=0;   /* seen versions are per incarnation */
    /* nothing matches until apply_peer_interest feeds the peer's interest */
}


void dart_transport_peer_remove(DartTransportState *st, uint32_t id){
    int s = i_dart_peer_slot(st,id); uint16_t c;
    if (s<0) return;
    for (c=0;c<st->cfg.n_topics;c++){
        i_DartLane *l = i_dart_lane_at(st,c,(uint32_t)s);
        if (!l) continue;
        i_dart_writer_unmatch(st,c,l);
        i_dart_reader_unmatch(st,c,l);
        /* release frees the lane's buffers too, so a gone peer keeps no memory */
        i_dart_lane_release(st,c,(uint32_t)s);
    }
    if (st->peer_index[s]){                        /* the index and verdict maps go too */
        st->cfg.allocator(st->cfg.user, st->peer_index[s], 0);
        if (st->peer_astate[s]) st->cfg.allocator(st->cfg.user, st->peer_astate[s], 0);
        if (st->peer_agen[s])   st->cfg.allocator(st->cfg.user, st->peer_agen[s], 0);
        if (st->peer_attrs[s])  st->cfg.allocator(st->cfg.user, st->peer_attrs[s], 0);
        st->peer_index[s]=NULL; st->peer_astate[s]=NULL; st->peer_agen[s]=NULL;
        st->peer_attrs[s]=NULL; st->peer_index_len[s]=0;
    }
    st->peer_seen_version[s]=0;
    st->peer_used[s]=0; st->peer_dormant[s]=0;
    memset(&st->peer_rtt[s], 0, sizeof(DartPeerRtt));
#ifdef DART_SHM
    st->peer_shm[s]=0;
#endif
}

/* The per peer round trip estimator. See spec/transport.md. */

/* Folds one unambiguous sample with the RFC 6298 weights. The first seeds both. */
void i_dart_rtt_sample(DartTransportState *st, uint32_t peer_slot, uint64_t sample_us){
    DartPeerRtt *e = &st->peer_rtt[peer_slot];
    uint32_t r = sample_us > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)sample_us;
    if (e->samples == 0){
        e->rtt_us = r; e->rtt_jitter_us = r / 2u; e->rtt_min_us = r;
    } else {
        uint32_t diff = e->rtt_us > r ? e->rtt_us - r : r - e->rtt_us;
        e->rtt_jitter_us = (uint32_t)((3ull * e->rtt_jitter_us + diff) / 4u);
        e->rtt_us        = (uint32_t)((7ull * e->rtt_us + r) / 8u);
        if (r < e->rtt_min_us) e->rtt_min_us = r;
    }
    e->rtt_last_us = r;
    if (e->samples != 0xFFFFFFFFu) e->samples++;
}

/* smoothed plus max(one tick, 4 x deviation), never under DART_RTO_MIN_US, and
 * fallback_us until the first sample */
uint32_t i_dart_rtt_rto(DartTransportState *st, uint32_t peer_slot, uint32_t fallback_us){
    const DartPeerRtt *e = &st->peer_rtt[peer_slot];
    uint64_t var, rto;
    if (e->samples == 0) return fallback_us;
    var = 4ull * e->rtt_jitter_us;
    if (var < DART_RTO_GRAIN_US) var = DART_RTO_GRAIN_US;
    rto = (uint64_t)e->rtt_us + var;
    if (rto < DART_RTO_MIN_US) rto = DART_RTO_MIN_US;
    return rto > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)rto;
}

int dart_transport_peer_rtt(DartTransportState *st, uint32_t peer_id, DartPeerRtt *out){
    int s = st ? i_dart_peer_slot(st, peer_id) : -1;
    if (out) memset(out, 0, sizeof *out);
    if (s < 0) return 0;
    if (out) *out = st->peer_rtt[s];
    return 1;
}


/* Keeps every proxy and reader position, drops the peer from flow control. */
void dart_transport_peer_dormant(DartTransportState *st, uint32_t id){
    int s = i_dart_peer_slot(st,id);
    if (s>=0) st->peer_dormant[s]=1;
}


/* Re includes the peer and re reports each reader position, so the writer fills any gap
 * and the reader dedups any replay. */
void dart_transport_peer_resume(DartTransportState *st, uint32_t id){
    int s = i_dart_peer_slot(st,id); uint16_t c;
    if (s<0) return;
    st->peer_dormant[s]=0;
    for (c=0;c<st->cfg.n_topics;c++){
        i_DartLane *l=i_dart_lane_at(st,c,(uint32_t)s);
        if (!l) continue;
        if (st->topics[c].qos.reliability!=DART_RELIABLE) continue;
        if (l->r.used){ l->r.ack_pending=1; l->r.ack_due_us=0; l->r.ack_force=1; }
        if (l->w.used || l->r.used) i_dart_lane_wake(st,c,(uint16_t)s);
    }
}


void dart_transport_peer_set_frag(DartTransportState *st, uint32_t id, uint16_t peer_frag){
    int s = i_dart_peer_slot(st,id);
    if (s>=0) st->peer_frag[s]=dart_clamp_frag(peer_frag);
}

#ifdef DART_SHM

void dart_transport_peer_set_shm(DartTransportState *st, uint32_t id, int is_shm){
    int s = i_dart_peer_slot(st,id);
    if (s>=0) st->peer_shm[s]=(uint8_t)(is_shm?1:0);
}
#endif


void dart_transport_destroy(DartTransportState *st){
    uint16_t c; uint32_t li;
    if (!st) return;
    for (c=0;c<st->cfg.n_topics;c++){
        i_DartTopic *topic=&st->topics[c];     /* an undefined slot has no ring */
        uint16_t depth, d;
        if (!topic->history) continue;         /* a retired slot freed its ring */
        depth = topic->qos.keep_last;
        for (d=0; d<depth; d++)
            if (topic->history[d].buf){ st->cfg.allocator(st->cfg.user, topic->history[d].buf, 0);
                                  topic->history[d].buf=NULL; topic->history[d].cap=0; }
        if (topic->history_owned && topic->history){   /* the ring came from topic_define */
            st->cfg.allocator(st->cfg.user, topic->history, 0);
            topic->history=NULL; topic->history_owned=0;
        }
    }
    for (li=0; li<st->lane_cap; li++){           /* live records' grown buffers */
        i_DartLane *l=&st->lanes[li];
        if (!l->in_use) continue;
        i_dart_asm_free(st, &l->r.cur); i_dart_asm_free(st, &l->r.next);
    }
    if (st->lanes){                              /* the record pool */
        st->cfg.allocator(st->cfg.user, st->lanes, 0);
        st->lanes=NULL; st->lane_cap=0; st->lane_free=DART__NIL;
    }
    {   uint32_t p;                              /* the per peer maps */
        for (p=0;p<st->cfg.max_peers;p++){
            if (st->peer_index[p]){
                st->cfg.allocator(st->cfg.user, st->peer_index[p], 0);
                st->peer_index[p]=NULL; st->peer_index_len[p]=0;
            }
            if (st->peer_astate[p]){
                st->cfg.allocator(st->cfg.user, st->peer_astate[p], 0);
                st->peer_astate[p]=NULL;
            }
            if (st->peer_agen[p]){
                st->cfg.allocator(st->cfg.user, st->peer_agen[p], 0);
                st->peer_agen[p]=NULL;
            }
            if (st->peer_attrs[p]){
                st->cfg.allocator(st->cfg.user, st->peer_attrs[p], 0);
                st->peer_attrs[p]=NULL;
            }
        }
    }
}


/* The peer's maps grown to cover need entries, the new tail unmapped with no verdicts.
 * NULL on OOM, the caller then counts the entries as unmappable. */
static uint16_t *i_dart_peer_index_ensure(DartTransportState *st, int peer_slot, uint32_t need){
    uint32_t have = st->peer_index_len[peer_slot];
    uint16_t *nm; uint8_t *ns, *ng, *na;
    if (need <= have) return st->peer_index[peer_slot];
    nm = (uint16_t*)st->cfg.allocator(st->cfg.user, st->peer_index[peer_slot], (size_t)need*sizeof(uint16_t));
    if (!nm) return NULL;
    st->peer_index[peer_slot] = nm;   /* len not raised yet: retryable on OOM below */
    ns = (uint8_t*)st->cfg.allocator(st->cfg.user, st->peer_astate[peer_slot], (size_t)need);
    if (!ns) return NULL;
    st->peer_astate[peer_slot] = ns;
    ng = (uint8_t*)st->cfg.allocator(st->cfg.user, st->peer_agen[peer_slot], (size_t)need);
    if (!ng) return NULL;
    st->peer_agen[peer_slot] = ng;
    na = (uint8_t*)st->cfg.allocator(st->cfg.user, st->peer_attrs[peer_slot], (size_t)need);
    if (!na) return NULL;
    st->peer_attrs[peer_slot] = na;
    memset(nm + have, 0xFF, (size_t)(need-have)*sizeof(uint16_t));   /* the grown tail: unmapped */
    memset(ns + have, 0, (size_t)(need-have));
    memset(ng + have, 0, (size_t)(need-have));
    memset(na + have, 0, (size_t)(need-have));
    st->peer_index_len[peer_slot] = need;
    return nm;
}

/* Local topics whose identity's low 32 bits match, preferring a non INACTIVE one. */
static int i_dart_hash32_candidates(DartTransportState *st, uint32_t h, int *idx_out){
    uint16_t i; int first=-1, live=-1, n=0;
    for (i=0;i<st->cfg.n_topics;i++){
        if (!i_dart_topic_announced(&st->topics[i]) || (uint32_t)st->topics[i].identity != h) continue;
        n++;
        if (first<0) first=(int)i;
        if (live<0 && st->topics[i].role!=DART_INACTIVE) live=(int)i;
    }
    if (idx_out) *idx_out = live>=0 ? live : first;
    return n;
}


/* Worst case: 5 bytes per slot plus the rate and generation sections at every topic. */
size_t dart_interest_max(uint16_t n_topics){
    return 2u + 5u * (size_t)n_topics + 2u + 4u * (size_t)n_topics
         + 2u + 3u * (size_t)n_topics;
}

/* the rate section's membership test, shared by the count and write walks */
static int i_dart_topic_rate_sub(const i_DartTopic *t){
    return i_dart_topic_announced(t) && t->qos.max_rate_hz && dart_role_subs(t->role);
}

/* the generation section's membership test, shared by the count and write walks */
static int i_dart_topic_rebound(const i_DartTopic *t){
    return i_dart_topic_announced(t) && t->gen != 0;
}


/* Serializes our interest, or measures it when out is NULL, in one walk so the two agree
 * byte for byte. The layout is in spec/interest.md. */
static size_t i_dart_interest_emit(DartTransportState *st, uint8_t *out, size_t cap){
    uint8_t *e, *rp;
    uint16_t c, n=0, n_rates=0, n_gens=0;
    uint32_t cells=0; int in_hole=0; size_t len;
    for (c=0;c<st->cfg.n_topics;c++) if (i_dart_topic_announced(&st->topics[c])) n=(uint16_t)(c+1u);
    for (c=0;c<n;c++){   /* one pass: the exact cell count and the section counts */
        const i_DartTopic *topic = &st->topics[c];
        if (i_dart_topic_announced(topic)){ cells++; in_hole=0; }
        else { if (!in_hole) cells++; in_hole=1; }
        if (i_dart_topic_rate_sub(topic)) n_rates++;
        if (i_dart_topic_rebound(topic))  n_gens++;
    }
    len = 2u + 5u*(size_t)cells + 2u + 4u*(size_t)n_rates + 2u + 3u*(size_t)n_gens;
    if (!out) return len;
    if (cap < len) return 0;
    i_dart_le_w16(out, n);
    e = out + 2u;
    for (c=0;c<n;c++){
        const i_DartTopic *topic = &st->topics[c];
        if (!i_dart_topic_announced(topic)){
            uint32_t run = 1;
            while ((uint16_t)(c+run) < n && !i_dart_topic_announced(&st->topics[c+run])) run++;
            i_dart_le_w32(e, run);
            e[4] = (uint8_t)(DART_INACTIVE | DART__INT_HOLE_RUN);
            e += 5u; c = (uint16_t)(c + run - 1u);
            continue;
        }
        i_dart_le_w32(e, (uint32_t)topic->identity);
        e[4] = (uint8_t)((topic->role & DART__INT_ROLE_MASK)
             | (topic->qos.reliability==DART_RELIABLE ? DART__INT_RELIABLE : 0u)
             | ((topic->kind << DART__INT_KIND_SHIFT) & DART__INT_KIND_MASK));
        e += 5u;
    }
    rp = e;
    i_dart_le_w16(rp, n_rates); rp += 2u;
    for (c=0;c<n;c++){
        const i_DartTopic *topic = &st->topics[c];
        if (i_dart_topic_rate_sub(topic)){
            i_dart_le_w16(rp, c); i_dart_le_w16(rp+2, topic->qos.max_rate_hz); rp += 4u;
        }
    }
    i_dart_le_w16(rp, n_gens); rp += 2u;
    for (c=0;c<n;c++){
        const i_DartTopic *topic = &st->topics[c];
        if (i_dart_topic_rebound(topic)){
            i_dart_le_w16(rp, c); rp[2] = topic->gen; rp += 3u;
        }
    }
    return (size_t)(rp - out);
}

size_t dart_transport_build_interest(DartTransportState *st, void *out, size_t cap){
    return i_dart_interest_emit(st, (uint8_t*)out, cap);
}

/* The entry stream's byte length when it accounts exactly n slots inside len, else 0.
 * Every parser walks this first, so a bad blob is rejected whole. */
static uint32_t i_dart_interest_walk_len(const uint8_t *d, size_t len, uint16_t n){
    const uint8_t *e = d + 2u, *end = d + len;
    uint32_t a = 0;
    while (a < n){
        uint32_t step = 1;
        if (e + 5 > end) return 0;
        if (e[4] & DART__INT_HOLE_RUN){
            step = i_dart_le_r32(e);
            if (step == 0 || step > (uint32_t)n - a) return 0;
        }
        e += 5u; a += step;
    }
    return (uint32_t)(e - d);
}

/* The two sparse sections after the entries, located in one pass. A section that does
 * not fit is absent along with everything after it. */
typedef struct { DartBytes rate, gen; } i_DartInterestSections;

static i_DartInterestSections i_dart_interest_sections(const uint8_t *d, size_t len,
                                                       uint32_t entries_len){
    static const uint8_t width[2] = { 4u, 3u };  /* (index, rate_hz) and (index, gen) */
    i_DartInterestSections s; DartBytes *view[2];
    size_t off = entries_len; int k;
    s.rate = s.gen = dart_bytes(NULL, 0);
    view[0] = &s.rate; view[1] = &s.gen;
    for (k=0;k<2;k++){
        size_t bytes;
        if (len < off + 2u) break;                   /* no header: this one and the rest absent */
        bytes = (size_t)i_dart_le_r16(d + off) * width[k];
        off += 2u;
        if (len < off + bytes) break;                /* entries truncated: the same */
        *view[k] = dart_bytes(d + off, bytes);
        off += bytes;
    }
    return s;
}


/* Re derives the peer's bits from cached verdicts plus the entry's current flags, then
 * rematches every topic. Idempotent. The gates are in spec/interest.md. */
void dart_transport_apply_peer_interest(DartTransportState *st, uint32_t peer_id, DartBytes blob){
    const uint8_t *d=blob.data, *e, *gp, *g_end;
    uint16_t n, c; uint32_t a, entries_len; int peer_slot=i_dart_peer_slot(st,peer_id);
    uint8_t *peer_pub_bitmap, *peer_sub_bitmap, *peer_sub_reliable;
    uint16_t *amap; uint8_t *astate;
    i_DartInterestSections sec;
    uint32_t unmappable = 0;
    if (peer_slot<0 || !d || blob.len<2) return;
    n = i_dart_le_r16(d);
    entries_len = i_dart_interest_walk_len(d, blob.len, n);
    if (!entries_len) return;                          /* truncated or malformed: reject whole */
    peer_pub_bitmap  =&st->peer_pub_bitmap[(size_t)peer_slot*st->bitmap_len];
    peer_sub_bitmap  =&st->peer_sub_bitmap[(size_t)peer_slot*st->bitmap_len];
    peer_sub_reliable=&st->peer_sub_reliable[(size_t)peer_slot*st->bitmap_len];
    memset(peer_pub_bitmap,0,st->bitmap_len); memset(peer_sub_bitmap,0,st->bitmap_len);
    memset(peer_sub_reliable,0,st->bitmap_len);
    amap   = n ? i_dart_peer_index_ensure(st, peer_slot, n) : NULL;
    astate = amap ? st->peer_astate[peer_slot] : NULL;
    /* the generation data is consumed by the entry loop with a merge cursor, the rate
       values are applied after the rematch once lanes exist */
    sec = i_dart_interest_sections(d, blob.len, entries_len);
    gp = g_end = NULL;
    if (sec.gen.data){ gp = sec.gen.data; g_end = gp + sec.gen.len; }
    e = d + 2u;
    for (a=0;a<n;a++,e+=5u){
        uint8_t flags = e[4], role = (uint8_t)(flags & DART__INT_ROLE_MASK);
        int their_pub, their_sub, rel;
        uint16_t cidx; i_DartTopic *topic;
        if (flags & DART__INT_HOLE_RUN){ a += i_dart_le_r32(e) - 1u; continue; }
        if (astate && a < st->peer_index_len[peer_slot] && st->peer_agen[peer_slot]){
            /* the generation gate: the peer rebound this position since our verdict, so it
               goes back to pending with the demux severed. Runs for INACTIVE entries too. */
            uint8_t g = 0;
            while (gp && gp + 3 <= g_end && i_dart_le_r16(gp) < a) gp += 3;
            if (gp && gp + 3 <= g_end && i_dart_le_r16(gp) == a) g = gp[2];
            if (st->peer_agen[peer_slot][a] != g){
                if (astate[a]){
                    astate[a] = 0; amap[a] = 0xFFFFu;
                    if (st->peer_attrs[peer_slot]) st->peer_attrs[peer_slot][a] = 0;
                }
                st->peer_agen[peer_slot][a] = g;
            }
        }
        if (role == DART_INACTIVE) continue;
        if (!astate || a >= st->peer_index_len[peer_slot]){
            /* no verdict storage: this entry can never verify, count it if it was a candidate */
            if (i_dart_hash32_candidates(st, i_dart_le_r32(e), NULL)) unmappable++;
            continue;
        }
        if (!(astate[a] & DART__AST_DETAILED) || !(astate[a] & DART__AST_NAME_OK))
            continue;                     /* pending, or a verified non match */
        cidx = amap[a];
        if (cidx >= st->cfg.n_topics) continue;      /* defensive: a stale map */
        topic = &st->topics[cidx];
        if ((uint32_t)topic->identity != i_dart_le_r32(e)){
            /* the hash no longer names the bound identity: the position was rebound */
            astate[a] = 0; amap[a] = 0xFFFFu;
            if (st->peer_attrs[peer_slot]) st->peer_attrs[peer_slot][a] = 0;
            continue;
        }
        if (topic->role == DART_INACTIVE || topic->retired){
            /* the verdict bound the then live twin, so re verify against the live one */
            int resolved_index = (int)cidx;
            i_dart_topic_by_identity(st, topic->identity, &resolved_index);
            if ((uint16_t)resolved_index != cidx){
                astate[a] = 0; amap[a] = 0xFFFFu;      /* pending again */
                if (st->peer_attrs[peer_slot]) st->peer_attrs[peer_slot][a] = 0;
                continue;
            }
            if (topic->retired) continue;   /* no live successor yet */
        }
        {   /* the kind gate: the same name under another kind is refused, never cross wired */
            uint8_t their_kind = (uint8_t)((flags & DART__INT_KIND_MASK) >> DART__INT_KIND_SHIFT);
            if (their_kind != topic->kind){
                i_dart_transport_fire_event(st, DART_TRANSPORT_KIND_MISMATCH, cidx, peer_id, 0, 0);
                continue;
            }
        }
        their_pub = dart_role_pubs(role);
        their_sub = dart_role_subs(role);
        rel       = (flags & DART__INT_RELIABLE) != 0;
        if (their_pub){   /* their offered qos against our subscription */
            int ours_sub = dart_role_subs(topic->role);
            if (ours_sub && topic->qos.reliability==DART_RELIABLE && !rel){
                i_dart_transport_fire_event(st, DART_TRANSPORT_QOS_INCOMPATIBLE, cidx, peer_id, 0, 0);
            } else if (!(astate[a] & DART__AST_READ_OK)){
                i_dart_transport_fire_event(st, DART_TRANSPORT_SCHEMA_MISMATCH, cidx, peer_id, 1, 0);
            } else {
                i_dart_bit_set(peer_pub_bitmap,(uint32_t)cidx);
            }
        }
        if (their_sub){                                /* their requested qos, for our writer */
            if (!(astate[a] & DART__AST_WRITE_OK)){
                i_dart_transport_fire_event(st, DART_TRANSPORT_SCHEMA_MISMATCH, cidx, peer_id, 0, 0);
            } else {
                i_dart_bit_set(peer_sub_bitmap,(uint32_t)cidx);
                if (rel) i_dart_bit_set(peer_sub_reliable,(uint32_t)cidx);
            }
        }
    }
    if (unmappable)   /* never silent: those topics can never deliver here */
        i_dart_transport_fire_event(st, DART_TRANSPORT_INTEREST_OVERFLOW, 0, peer_id, 0, unmappable);
    for (c=0;c<st->cfg.n_topics;c++) i_dart_topic_rematch(st,c,(uint16_t)peer_slot);

    /* per lane values, applied after the rematch so the lanes exist and re derived on
       every announce, since a fresh match memsets the proxy */
    if (amap && sec.rate.data){
        /* their best effort delivery cap for a topic they subscribe paces our lane */
        const uint8_t *p = sec.rate.data, *end = p + sec.rate.len;
        for (; p + 4u <= end; p += 4u){
            uint16_t their_idx = i_dart_le_r16(p), rate_hz = i_dart_le_r16(p+2), cidx;
            i_DartWriterProxy *w;
            if (their_idx >= st->peer_index_len[peer_slot]) continue;
            cidx = amap[their_idx];
            if (cidx >= st->cfg.n_topics) continue;             /* unverified */
            w = i_dart_writer_proxy_at(st, cidx, (uint32_t)peer_slot);
            if (w && w->used)
                w->rate_interval_us = rate_hz ? (1000000u / (uint32_t)rate_hz) : 0u;
        }
    }
    if (amap && st->peer_attrs[peer_slot]){
        /* NO_TIMESTAMP from the detail cache onto our reader lane, so delivery strips
           exactly what the writer prepended. Unset = stamped */
        const uint8_t *attrs = st->peer_attrs[peer_slot];
        for (a=0;a<st->peer_index_len[peer_slot];a++){
            uint16_t cidx; i_DartReaderProxy *r;
            if (!(attrs[a] & DART_ATTR_NO_TIMESTAMP)) continue;
            cidx = amap[a];
            if (cidx >= st->cfg.n_topics) continue;             /* unverified */
            r = i_dart_reader_proxy_at(st, cidx, (uint32_t)peer_slot);
            if (r && r->used) r->no_timestamp = 1;
        }
    }
}


int dart_transport_peer_timestamped(DartTransportState *st, uint16_t topic_index, uint32_t peer_id){
    int peer_slot;
    i_DartReaderProxy *r;
    if (!st || topic_index >= st->cfg.n_topics) return 1;
    peer_slot = i_dart_peer_slot(st, peer_id);
    if (peer_slot < 0) return 1;                       /* unknown peer: the default framing */
    r = i_dart_reader_proxy_at(st, topic_index, (uint32_t)peer_slot);
    return (r && r->no_timestamp) ? 0 : 1;
}


uint8_t dart_transport_peer_attrs(DartTransportState *st, uint32_t peer_id, uint16_t their_index){
    int peer_slot;
    if (!st) return 0;
    peer_slot = i_dart_peer_slot(st, peer_id);
    if (peer_slot < 0 || !st->peer_attrs[peer_slot]
        || (uint32_t)their_index >= st->peer_index_len[peer_slot]) return 0;
    return st->peer_attrs[peer_slot][their_index];
}


void dart_transport_peer_match_counts(DartTransportState *st, uint32_t peer_id,
                            uint16_t *publish_to, uint16_t *receive_from){
    int s; uint16_t c, w=0, r=0;
    if (publish_to)   *publish_to   = 0;
    if (receive_from) *receive_from = 0;
    if (!st) return;
    s = i_dart_peer_slot(st, peer_id);
    if (s < 0) return;
    for (c=0;c<st->cfg.n_topics;c++){
        i_DartLane *l = i_dart_lane_at(st,c,(uint32_t)s);
        if (!l) continue;
        if (l->w.used) w++;
        if (l->r.used) r++;
    }
    if (publish_to)   *publish_to   = w;
    if (receive_from) *receive_from = r;
}


/* The announce overlay codec. The version byte tags the one current format, and a foreign
   blob is rejected. The layout is in spec/interest.md. */
#define DART__META_BASE_NOSHM 6u    /* 'D','N',ver, frag_lo, frag_hi, iflags */
#define DART__META_BASE_SHM   23u   /* 'D','N',ver, frag_lo, frag_hi, shm, host[16], iflags */
#define DART__META_IFLAG_EXTERNAL 0x01u   /* iflags bit 0: the interest is served by paging */
#ifdef DART_SHM
#define DART__META_VER  21u                 /* odd versions carry the SHM base */
#define DART__META_BASE DART__META_BASE_SHM
#else
#define DART__META_VER  20u
#define DART__META_BASE DART__META_BASE_NOSHM
#endif

static int i_dart_meta_ok(DartBytes meta){
    return meta.data && meta.len >= DART__META_BASE_NOSHM
        && meta.data[0]=='D' && meta.data[1]=='N'
        && meta.data[2]>=20 && meta.data[2]<=21;
}
/* the base prefix through the iflags byte. The odd version carries shm and host */
static uint16_t i_dart_meta_base(const uint8_t *meta){
    return (meta[2] & 1u) ? DART__META_BASE_SHM : DART__META_BASE_NOSHM;
}
uint16_t dart_meta_cap(uint16_t n_topics){
    size_t cap = (size_t)DART__META_BASE + dart_interest_max(n_topics);
    if (cap > 65000u) cap = 65000u;
    return (uint16_t)cap;
}

uint32_t dart_transport_interest_size(DartTransportState *st){
    return (uint32_t)i_dart_interest_emit(st, NULL, 0);
}

uint16_t dart_transport_meta_size(DartTransportState *st){
    size_t len = (size_t)DART__META_BASE + dart_transport_interest_size(st);
    if (len > 65000u) len = 65000u;              /* the dart_meta_cap ceiling */
    return (uint16_t)len;
}

uint16_t dart_transport_meta_bootstrap_size(void){ return DART__META_BASE; }

uint16_t dart_transport_meta_build(DartTransportState *st, uint8_t *out, uint16_t cap,
                         uint16_t frag_size, int shm_capable, const uint8_t host[16],
                         int interest_external){
    size_t interest_len, len; uint16_t off = DART__META_BASE;
    out[0]='D'; out[1]='N'; out[2]=DART__META_VER;
    out[3]=(uint8_t)(frag_size & 0xFF); out[4]=(uint8_t)(frag_size >> 8);
#ifdef DART_SHM
    out[5]=(uint8_t)(shm_capable?1:0);
    if (host) memcpy(out+6, host, 16); else memset(out+6, 0, 16);
#else
    (void)shm_capable; (void)host;
#endif
    out[off-1] = interest_external ? DART__META_IFLAG_EXTERNAL : 0u;
    if (interest_external) return off;     /* the bootstrap: locator sized, always one datagram */
    interest_len = dart_transport_build_interest(st, out + off, cap - off);
    len = (size_t)off + interest_len;
    if (interest_len == 0)    /* did not fit: never silent */
        i_dart_transport_fire_event(st, DART_TRANSPORT_META_TRUNCATED_INTEREST, 0, 0, 0, 0);
    return (uint16_t)len;
}

uint16_t dart_meta_frag(DartBytes meta){
    if (!i_dart_meta_ok(meta)) return 0;
    return (uint16_t)(meta.data[3] | ((uint16_t)meta.data[4] << 8));
}

int dart_meta_interest_external(DartBytes meta){
    uint16_t base;
    if (!i_dart_meta_ok(meta)) return 0;
    base = i_dart_meta_base(meta.data);
    if (meta.len < base) return 0;
    return (meta.data[base-1] & DART__META_IFLAG_EXTERNAL) ? 1 : 0;
}

DartBytes dart_meta_interest(DartBytes meta){
    uint16_t off;
    if (!i_dart_meta_ok(meta)) return dart_bytes(NULL, 0);
    off = i_dart_meta_base(meta.data);     /* the interest follows the base */
    if (meta.len < off) return dart_bytes(NULL, 0);
    if (meta.data[off-1] & DART__META_IFLAG_EXTERNAL)
        return dart_bytes(NULL, 0);        /* a bootstrap never reads as an empty interest list */
    return dart_bytes(meta.data + off, meta.len - off);
}

int dart_interest_next(DartBytes interest, DartInterestIter *it, DartTopicEntry *out){
    if (!it || !out) return 0;
    if (!it->started){                    /* first call: the [u16 n] header */
        it->started = 1; it->left = 0; it->index = 0; it->phase = 0; it->off = 0;
        if (!interest.data || interest.len < 2) return 0;   /* no interest list: nothing to yield */
        it->left = (uint16_t)(interest.data[0] | ((uint16_t)interest.data[1] << 8));
        it->off  = 2u;                    /* the first entry, past n */
    }
    while (it->left){
        uint32_t off = it->off;
        uint8_t flags, role;
        if ((size_t)off + 5u > interest.len){ it->left = 0; return 0; }   /* truncated: stop */
        flags = interest.data[off + 4]; role = (uint8_t)(flags & DART__INT_ROLE_MASK);
        if (flags & DART__INT_HOLE_RUN){  /* a run of undefined slots: skip them all */
            uint32_t run = i_dart_le_r32(interest.data + off);
            if (run == 0 || run > it->left){ it->left = 0; return 0; }    /* malformed: stop */
            it->left = (uint16_t)(it->left - run); it->index = (uint16_t)(it->index + run);
            it->off = off + 5u; it->phase = 0;
            continue;
        }
        if (role == DART_INACTIVE){       /* declared but off: not advertised */
            it->left--; it->index++; it->off = off + 5u; it->phase = 0;
            continue;
        }
        out->index    = it->index;
        out->role     = role;
        out->reliable = (uint8_t)((flags & DART__INT_RELIABLE) ? 1 : 0);
        out->kind     = (uint8_t)((flags & DART__INT_KIND_MASK) >> DART__INT_KIND_SHIFT);
        out->hash     = i_dart_le_r32(interest.data + off);
        if (it->phase == 0 && dart_role_pubs(role)){
            out->is_pub = 1;
            if (role==DART_PUBSUB){ it->phase = 1; return 1; }   /* the sub direction next call */
            it->left--; it->index++; it->off = off + 5u;
            return 1;
        }
        out->is_pub = 0;                  /* SUB_ONLY, or the second yield of a PUBSUB entry */
        it->phase = 0; it->left--; it->index++; it->off = off + 5u;
        return 1;
    }
    return 0;
}

int dart_meta_interest_next(DartBytes meta, DartInterestIter *it, DartTopicEntry *out){
    return dart_interest_next(dart_meta_interest(meta), it, out);
}

#ifdef DART_SHM
int dart_meta_shm(DartBytes meta, uint8_t host[16]){
    if (!i_dart_meta_ok(meta) || !(meta.data[2] & 1u)   /* the odd version carries the SHM base */
        || meta.len < DART__META_BASE_SHM || !meta.data[5]) return 0;
    memcpy(host, meta.data+6, 16);
    return 1;
}
#endif


/* The uDTL codec. A malformed request or response is dropped whole. The responder is a
   pure read of topic and schema state. The layout is in spec/interest.md. */
#define DART__DETAIL_HDR 14u   /* magic(4) kind(1) ver(1) domain(2) meta_version(4) n(2) */
#define DART__DETAIL_VER 2u

static int i_dart_detail_hdr_ok(DartBytes d){
    return d.data && d.len >= DART__DETAIL_HDR
        && d.data[0]=='u' && d.data[1]=='D' && d.data[2]=='T' && d.data[3]=='L'
        && d.data[5]==DART__DETAIL_VER;
}

int dart_detail_kind(DartBytes dgram){
    if (!i_dart_detail_hdr_ok(dgram)) return 0;
    return (dgram.data[4]>=DART_DETAIL_REQ && dgram.data[4]<=DART_INTEREST_RESP)
         ? dgram.data[4] : 0;
}
uint16_t dart_detail_domain(DartBytes dgram){
    return i_dart_detail_hdr_ok(dgram) ? i_dart_le_r16(dgram.data+6) : 0;
}
uint32_t dart_detail_meta_version(DartBytes dgram){
    return i_dart_detail_hdr_ok(dgram) ? i_dart_le_r32(dgram.data+8) : 0;
}

static void i_dart_detail_hdr_write(uint8_t *o, uint8_t kind, uint16_t domain,
                                    uint32_t meta_version, uint16_t n){
    o[0]='u'; o[1]='D'; o[2]='T'; o[3]='L';
    o[4]=kind; o[5]=(uint8_t)DART__DETAIL_VER;
    i_dart_le_w16(o+6, domain);
    i_dart_le_w32(o+8, meta_version);
    i_dart_le_w16(o+12, n);
}

size_t dart_detail_req_build(uint16_t domain, uint32_t peer_meta_version,
                             const DartDetailWant *wants, uint16_t n_wants,
                             void *out, size_t cap){
    uint8_t *o=(uint8_t*)out; uint16_t k;
    size_t need = (size_t)DART__DETAIL_HDR + (size_t)n_wants*10u;
    if (!o || (n_wants && !wants) || cap < need) return 0;
    i_dart_detail_hdr_write(o, DART_DETAIL_REQ, domain, peer_meta_version, n_wants);
    for (k=0;k<n_wants;k++){
        uint8_t *e = o + DART__DETAIL_HDR + (size_t)k*10u;
        i_dart_le_w16(e, wants[k].index);
        i_dart_le_w64(e+2, wants[k].schema_hash);
    }
    return need;
}


/* The interest paging codec: pure header build and parse. The responder's slicing and
   the requester's cursor live in the node core. */
size_t dart_interest_req_build(uint16_t domain, uint32_t peer_meta_version,
                               uint32_t offset, void *out, size_t cap){
    uint8_t *o=(uint8_t*)out;
    if (!o || cap < (size_t)DART__DETAIL_HDR + 4u) return 0;
    i_dart_detail_hdr_write(o, DART_INTEREST_REQ, domain, peer_meta_version, 0);
    i_dart_le_w32(o + DART__DETAIL_HDR, offset);
    return (size_t)DART__DETAIL_HDR + 4u;
}

int dart_interest_req_offset(DartBytes dgram, uint32_t *offset){
    if (dart_detail_kind(dgram) != DART_INTEREST_REQ) return 0;
    if (dgram.len < (size_t)DART__DETAIL_HDR + 4u) return 0;
    if (offset) *offset = i_dart_le_r32(dgram.data + DART__DETAIL_HDR);
    return 1;
}

size_t dart_interest_resp_head(uint16_t domain, uint32_t meta_version, uint32_t total_len,
                               uint32_t offset, uint16_t chunk_len, void *out, size_t cap){
    uint8_t *o=(uint8_t*)out;
    if (!o || cap < (size_t)DART_INTEREST_RESP_HEAD) return 0;
    i_dart_detail_hdr_write(o, DART_INTEREST_RESP, domain, meta_version, 0);
    i_dart_le_w32(o + DART__DETAIL_HDR,      total_len);
    i_dart_le_w32(o + DART__DETAIL_HDR + 4u, offset);
    i_dart_le_w16(o + DART__DETAIL_HDR + 8u, chunk_len);
    return (size_t)DART_INTEREST_RESP_HEAD;
}

int dart_interest_resp_parse(DartBytes dgram, uint32_t *total_len, uint32_t *offset,
                             DartBytes *chunk){
    uint32_t total, off; uint16_t clen;
    if (dart_detail_kind(dgram) != DART_INTEREST_RESP) return 0;
    if (dgram.len < (size_t)DART_INTEREST_RESP_HEAD) return 0;
    total = i_dart_le_r32(dgram.data + DART__DETAIL_HDR);
    off   = i_dart_le_r32(dgram.data + DART__DETAIL_HDR + 4u);
    clen  = i_dart_le_r16(dgram.data + DART__DETAIL_HDR + 8u);
    if ((size_t)DART_INTEREST_RESP_HEAD + clen > dgram.len) return 0;   /* truncated: reject */
    if (off > total || (uint32_t)clen > total - off) return 0;   /* a range outside the blob */
    if (total_len) *total_len = total;
    if (offset)    *offset    = off;
    if (chunk)     *chunk     = dart_bytes(dgram.data + DART_INTEREST_RESP_HEAD, clen);
    return 1;
}

/* One walk serves size and build (out NULL measures), so the two agree byte for byte. A
   truncated build stops at an entry boundary and the requester re asks for the rest. */
static size_t i_dart_detail_answer(DartTransportState *st, const DartMetaSchema *schemas,
                                   uint32_t meta_version, DartBytes req,
                                   uint8_t *out, size_t cap){
    const uint8_t *r; uint16_t n_req, k, n_out=0;
    size_t len = DART__DETAIL_HDR;
    if (!st || dart_detail_kind(req) != DART_DETAIL_REQ) return 0;
    n_req = i_dart_le_r16(req.data+12);
    if (req.len < (size_t)DART__DETAIL_HDR + (size_t)n_req*10u) return 0;   /* truncated: reject */
    if (out){
        if (cap < DART__DETAIL_HDR) return 0;
        i_dart_detail_hdr_write(out, DART_DETAIL_RESP, dart_detail_domain(req), meta_version, 0);
    }
    r = req.data + DART__DETAIL_HDR;
    for (k=0;k<n_req;k++,r+=10){
        uint16_t index    = i_dart_le_r16(r);
        uint64_t req_hash = i_dart_le_r64(r+2);
        const i_DartTopic *topic;
        uint64_t hash; DartBytes wire; size_t need;
        if (index >= st->cfg.n_topics) continue;             /* unknown: not advertised */
        topic = &st->topics[index];
        if (topic->role == DART_INACTIVE || topic->name_len == 0) continue;
        hash = schemas ? schemas[index].hash : 0;
        wire = dart_bytes(NULL, 0);
        if (hash && hash != req_hash && schemas[index].wire.len <= 0xFFFFu)
            wire = schemas[index].wire;    /* differs: inline for the subset check */
        need = 2u + 1u + 1u + topic->name_len + 8u + 2u + wire.len;
        /* one datagram per response, but always at least one entry, so a lone oversized
           entry rides its own page instead of wedging paging with an empty reply */
        if (n_out > 0 && len + need > cap) break;
        if (out){
            uint8_t *e = out + len;
            i_dart_le_w16(e, index);
            e[2] = (uint8_t)(topic->attrs
                 | (topic->qos.no_timestamp ? DART_ATTR_NO_TIMESTAMP : 0u));
            e[3] = topic->name_len;
            memcpy(e+4, topic->name, topic->name_len);
            i_dart_le_w64(e+4+topic->name_len, hash);
            i_dart_le_w16(e+4+topic->name_len+8, (uint16_t)wire.len);
            if (wire.len) memcpy(e+4+topic->name_len+10, wire.data, wire.len);
        }
        len += need;
        n_out++;
    }
    if (out) i_dart_le_w16(out+12, n_out);
    return len;
}

size_t dart_transport_detail_resp_size(DartTransportState *st, const DartMetaSchema *schemas,
                                       DartBytes req){
    /* one page, so the response never IP fragments and the requester pages the rest */
    return i_dart_detail_answer(st, schemas, 0, req, NULL, DART_DGRAM_MAX);
}

size_t dart_transport_detail_respond(DartTransportState *st, const DartMetaSchema *schemas,
                                     uint32_t meta_version, DartBytes req,
                                     void *out, size_t cap){
    return i_dart_detail_answer(st, schemas, meta_version, req, (uint8_t*)out, cap);
}

int dart_detail_next(DartBytes resp, DartDetailIter *it, DartDetail *out){
    uint32_t off; uint8_t nlen; uint16_t wlen;
    if (!it || !out) return 0;
    if (!it->started){
        it->started = 1; it->left = 0; it->off = DART__DETAIL_HDR;
        if (dart_detail_kind(resp) != DART_DETAIL_RESP) return 0;
        it->left = i_dart_le_r16(resp.data+12);
    }
    if (!it->left) return 0;
    off = it->off;
    if ((size_t)off + 4u > resp.len){ it->left = 0; return 0; }        /* truncated: stop */
    nlen = resp.data[off+3];
    if ((size_t)off + 4u + nlen + 10u > resp.len){ it->left = 0; return 0; }
    wlen = i_dart_le_r16(resp.data + off + 4u + nlen + 8u);
    if ((size_t)off + 4u + nlen + 10u + wlen > resp.len){ it->left = 0; return 0; }
    out->index       = i_dart_le_r16(resp.data + off);
    out->attrs       = resp.data[off+2];
    out->name        = dart_string((const char*)(resp.data + off + 4u), nlen);
    out->schema_hash = i_dart_le_r64(resp.data + off + 4u + nlen);
    out->schema_wire = wlen ? dart_bytes(resp.data + off + 4u + nlen + 10u, wlen)
                            : dart_bytes(NULL, 0);
    it->off = off + 4u + nlen + 10u + wlen;
    it->left--;
    return 1;
}

/* Iterator over the live entries of an interest stream, shared by the candidate scans so
 * they cannot drift. The stream must have passed i_dart_interest_walk_len first. */
typedef struct {
    const uint8_t *e;        /* next entry */
    uint32_t a;              /* its position */
    uint32_t n;              /* slots in the stream */
    uint32_t pos;            /* yielded: the entry's position */
    uint32_t hash;           /* yielded: its 32 bit nomination */
    uint8_t  flags;          /* yielded: the raw flags */
    uint8_t  their_pub, their_sub;   /* yielded: the advertised role by direction */
} i_DartInterestScan;

static void i_dart_interest_scan_init(i_DartInterestScan *s, const uint8_t *d, uint16_t n){
    s->e = d + 2u; s->a = 0; s->n = n;
    s->pos = 0; s->hash = 0; s->flags = 0; s->their_pub = s->their_sub = 0;
}

static int i_dart_interest_scan_next(i_DartInterestScan *s){
    while (s->a < s->n){
        const uint8_t *e = s->e;
        uint32_t a = s->a, step = 1;
        uint8_t flags = e[4], role = (uint8_t)(flags & DART__INT_ROLE_MASK);
        if (flags & DART__INT_HOLE_RUN) step = i_dart_le_r32(e);
        s->e = e + 5u; s->a = a + step;
        if (flags & DART__INT_HOLE_RUN) continue;   /* a run of undefined slots */
        if (role == DART_INACTIVE) continue;        /* declared but off */
        s->pos = a; s->hash = i_dart_le_r32(e); s->flags = flags;
        s->their_pub = (uint8_t)dart_role_pubs(role);
        s->their_sub = (uint8_t)dart_role_subs(role);
        return 1;
    }
    return 0;
}

/* The pending candidates of a peer: a hash and role overlap with no cached verdict. */
uint16_t dart_transport_detail_wants(DartTransportState *st, const DartMetaSchema *schemas,
                                     uint32_t peer_id, DartBytes interest,
                                     DartDetailWant *out, uint16_t max_wants){
    const uint8_t *d=interest.data;
    i_DartInterestScan scan;
    uint16_t n, cnt=0; int slot;
    uint8_t *astate; uint32_t alen;
    if (!st || !d || interest.len < 2) return 0;
    slot = i_dart_peer_slot(st, peer_id);
    if (slot < 0) return 0;
    n = i_dart_le_r16(d);
    if (!i_dart_interest_walk_len(d, interest.len, n)) return 0;
    astate = st->peer_astate[slot]; alen = st->peer_index_len[slot];
    i_dart_interest_scan_init(&scan, d, n);
    while (i_dart_interest_scan_next(&scan)){
        int cidx = -1, nc;
        i_DartTopic *topic;
        int ours_pub, ours_sub;
        if (astate && scan.pos < alen && (astate[scan.pos] & DART__AST_DETAILED)) continue;
        nc = i_dart_hash32_candidates(st, scan.hash, &cidx);
        if (!nc) continue;                                 /* no local topic */
        topic = &st->topics[cidx];
        ours_pub = dart_role_pubs(topic->role);
        ours_sub = dart_role_subs(topic->role);
        if (!((scan.their_pub && ours_sub) || (scan.their_sub && ours_pub))) continue;
        if (out){
            if (cnt >= max_wants) break;
            out[cnt].index = (uint16_t)scan.pos;
            /* several local topics behind one hash: hash 0 forces the wire inline, so
               whichever topic the name binds to can still verify */
            out[cnt].schema_hash = (nc == 1 && schemas) ? schemas[cidx].hash : 0;
        }
        cnt++;
    }
    return cnt;
}

/* Entries with no verdict storage are not counted. They can never resolve, so a wait on
 * them would only time out. */
uint16_t dart_transport_topic_unresolved(DartTransportState *st, uint16_t topic_index,
                                         uint32_t peer_id, DartBytes interest){
    const uint8_t *d=interest.data;
    i_DartTopic *topic;
    i_DartInterestScan scan;
    uint16_t n, cnt=0; int slot;
    uint8_t *astate; uint32_t alen;
    int ours_pub, ours_sub;
    topic = i_dart_topic_at(st, topic_index, NULL);
    if (!topic || !i_dart_topic_announced(topic) || !d || interest.len < 2) return 0;
    slot = i_dart_peer_slot(st, peer_id);
    if (slot < 0) return 0;
    n = i_dart_le_r16(d);
    if (!i_dart_interest_walk_len(d, interest.len, n)) return 0;
    ours_pub = dart_role_pubs(topic->role);
    ours_sub = dart_role_subs(topic->role);
    if (!ours_pub && !ours_sub) return 0;
    astate = st->peer_astate[slot]; alen = st->peer_index_len[slot];
    i_dart_interest_scan_init(&scan, d, n);
    while (i_dart_interest_scan_next(&scan)){
        if (scan.hash != (uint32_t)topic->identity) continue;   /* not this topic */
        if (!astate || scan.pos >= alen) continue;          /* unresolvable: never counted */
        if (astate[scan.pos] & DART__AST_DETAILED){
            /* decided, but a verified subscriber whose writer lane is under the rebind hold
               is still resolving, so the match wait must cover it */
            if (scan.their_sub && ours_pub && (astate[scan.pos] & DART__AST_NAME_OK)
                && topic->rebind_version
                && st->peer_seen_version[slot] < topic->rebind_version) cnt++;
            continue;
        }
        if ((scan.their_pub && ours_sub) || (scan.their_sub && ours_pub)) cnt++;
    }
    return cnt;
}

void dart_transport_peer_unresolved_fill(DartTransportState *st, uint32_t peer_id,
                                         DartBytes interest, uint16_t *counts, uint16_t n){
    const uint8_t *d=interest.data;
    i_DartInterestScan scan;
    uint16_t nent, c; int slot;
    uint8_t *astate; uint16_t *amap; uint32_t alen;
    if (!st || !counts || !n || !d || interest.len < 2) return;
    slot = i_dart_peer_slot(st, peer_id);
    if (slot < 0) return;
    nent = i_dart_le_r16(d);
    if (!i_dart_interest_walk_len(d, interest.len, nent)) return;
    astate = st->peer_astate[slot]; amap = st->peer_index[slot]; alen = st->peer_index_len[slot];
    if (!astate || !amap) return;                        /* unresolvable entries: never counted */
    if (n > st->cfg.n_topics) n = (uint16_t)st->cfg.n_topics;
    i_dart_interest_scan_init(&scan, d, nent);
    while (i_dart_interest_scan_next(&scan)){
        if (scan.pos >= alen) continue;
        if (astate[scan.pos] & DART__AST_DETAILED){
            /* decided: the verdict names the topic, and only the rebind hold still resolves */
            uint16_t cidx = amap[scan.pos];
            const i_DartTopic *topic;
            if (!(astate[scan.pos] & DART__AST_NAME_OK) || cidx >= n) continue;
            topic = &st->topics[cidx];
            if (scan.their_sub && dart_role_pubs(topic->role) && topic->rebind_version
                && st->peer_seen_version[slot] < topic->rebind_version && counts[cidx] != 0xFFFFu)
                counts[cidx]++;
            continue;
        }
        for (c=0;c<n;c++){   /* pending: every topic the hash nominates */
            const i_DartTopic *topic = &st->topics[c];
            int ours_pub, ours_sub;
            if (!i_dart_topic_announced(topic) || (uint32_t)topic->identity != scan.hash) continue;
            ours_pub = dart_role_pubs(topic->role); ours_sub = dart_role_subs(topic->role);
            if (((scan.their_pub && ours_sub) || (scan.their_sub && ours_pub)) && counts[c] != 0xFFFFu)
                counts[c]++;
        }
    }
}

/* Verifies each entry and caches the verdict. Idempotent, decided indices are skipped, so
 * duplicate and crossing responses are harmless. */
uint16_t dart_transport_apply_peer_details(DartTransportState *st, uint32_t peer_id, DartBytes resp){
    DartDetailIter it; DartDetail dd;
    int slot; uint16_t fresh = 0;
    if (!st) return 0;
    slot = i_dart_peer_slot(st, peer_id);
    if (slot < 0) return 0;
    memset(&it, 0, sizeof it);
    while (dart_detail_next(resp, &it, &dd)){
        uint16_t *amap = st->peer_index[slot]; uint8_t *astate = st->peer_astate[slot];
        uint64_t id64; i_DartTopic *topic; int cidx = -1;
        if (!amap || !astate || (uint32_t)dd.index >= st->peer_index_len[slot])
            continue;   /* unseen: ignored, a response racing the announce re resolves */
        if (astate[dd.index] & DART__AST_DETAILED) continue;
        if (st->peer_attrs[slot]) st->peer_attrs[slot][dd.index] = dd.attrs;
        if (dd.name.len == 0){ astate[dd.index] = DART__AST_DETAILED; fresh++; continue; }
        id64 = i_dart_identity_hash((const uint8_t*)dd.name.data, dd.name.len);
        topic = i_dart_topic_by_identity(st, id64, &cidx);
        if (!topic){                              /* the 32 bit nomination was a false positive */
            astate[dd.index] = DART__AST_DETAILED;
            fresh++; continue;
        }
        if (topic->name_len != dd.name.len || memcmp(topic->name, dd.name.data, dd.name.len) != 0){
            astate[dd.index] = DART__AST_DETAILED;   /* the same id, a different name: refused */
            i_dart_transport_fire_event(st, DART_TRANSPORT_NAME_COLLISION, (uint16_t)cidx,
                        peer_id, id64, 0);
            fresh++; continue;
        }
        amap[dd.index] = (uint16_t)cidx;
        astate[dd.index] = (uint8_t)(DART__AST_DETAILED | DART__AST_NAME_OK
            | ((!st->cfg.schema_check || st->cfg.schema_check(st->cfg.user, peer_id, (uint16_t)cidx,
                    1, dd.schema_hash, dd.schema_wire)) ? DART__AST_READ_OK : 0u)
            | ((!st->cfg.schema_check || st->cfg.schema_check(st->cfg.user, peer_id, (uint16_t)cidx,
                    0, dd.schema_hash, dd.schema_wire)) ? DART__AST_WRITE_OK : 0u));
        fresh++;
    }
    return fresh;
}


int dart_transport_set_role(DartTransportState *st, uint16_t topic_index, uint8_t role){
    i_DartTopic *topic; uint16_t p;
    if (role > DART_INACTIVE) return -1;
    topic = i_dart_topic_at(st, topic_index, NULL);
    if (!topic || topic->retired) return -1;   /* a retired slot only returns through reuse */
    if (topic->role == role) return 0;
    topic->role = role;
    for (p=0;p<st->cfg.max_peers;p++)
        if (st->peer_used[p]) i_dart_topic_rematch(st,(uint16_t)topic_index,p);
    /* the caller re advertises */
    return 0;
}


int dart_transport_topic_define(DartTransportState *st, uint16_t topic_index, const DartTopicDef *def){
    i_DartTopic *topic; DartQos q; uint16_t depth, p; size_t lane;
    if (!st || !def) return -1;
    if (topic_index >= st->cfg.n_topics) return -1;            /* out of the reserved range */
    if (!def->name || !def->name[0]) return -1;             /* the name is the identity */
    lane = i_dart_name_len(def->name);
    if (def->name[lane]) return -1;                          /* longer than DART_TOPIC_NAME_MAX */
    if (def->directed && def->qos.catch_up) return -1;       /* directed history never replays */
    topic = &st->topics[topic_index];
    if (topic->identity != 0 || topic->history) return -1;        /* already defined */
    {   /* the same name under a different kind on one node is refused, since the maps bind
           by identity. Retired slots are exempt. See spec/interest.md */
        uint64_t id = dart_topic_identity(def); uint16_t c;
        for (c=0;c<st->cfg.n_topics;c++)
            if (i_dart_topic_announced(&st->topics[c]) && st->topics[c].identity == id
                && st->topics[c].kind != def->kind) return -1;
    }
    q = def->qos; i_dart_qos_defaults(&q);
    depth = q.keep_last;
    topic->history = (i_DartWriterSample*)st->cfg.allocator(st->cfg.user, NULL,
                                                         (size_t)depth*sizeof(i_DartWriterSample));
    if (!topic->history) return -4;                            /* OOM */
    memset(topic->history, 0, (size_t)depth*sizeof(i_DartWriterSample));
    topic->history_owned = 1;
    topic->qos = q;
    topic->role = def->role;
    topic->kind = def->kind; topic->prefix_bytes = def->prefix_bytes; topic->directed = def->directed;
    topic->attrs = def->attrs;
    topic->identity = dart_topic_identity(def);
    memcpy((char*)topic->name, def->name, lane); ((char*)topic->name)[lane] = '\0';
    topic->name_len = (uint8_t)lane;
    topic->history_head = 0; topic->next_seqno = 0; topic->have_first = 0;
    /* a dissolved verdict is only as durable as the topic set it was judged against, and
       that set just grew: send them back to pending. Attrs stay. See spec/interest.md */
    for (p=0;p<st->cfg.max_peers;p++){
        uint8_t *as = st->peer_astate[p]; uint32_t a, alen = st->peer_index_len[p];
        if (!st->peer_used[p] || !as) continue;
        for (a=0;a<alen;a++)
            if ((as[a] & DART__AST_DETAILED) && !(as[a] & DART__AST_NAME_OK))
                as[a] = 0;
    }
    for (p=0;p<st->cfg.max_peers;p++)        /* match the new topic to known peers */
        if (st->peer_used[p]) i_dart_topic_rematch(st, topic_index, p);
    return 0;
}


/* Parks a slot: every lane releases, the ring is freed and the slot leaves the announce.
 * Identity, name, kind, qos, gen and next_seqno stay for reuse to compare. */
int dart_transport_topic_retire(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic; uint16_t p, d, depth;
    if (!st) return -1;
    topic = i_dart_topic_at(st, topic_index, NULL);
    if (!topic || topic->name_len == 0 || topic->retired) return -1;
    topic->role = DART_INACTIVE; topic->retired = 1;
    for (p=0;p<st->cfg.max_peers;p++)          /* both sides unmatch, every lane releases */
        if (st->peer_used[p]) i_dart_topic_rematch(st, topic_index, p);
    depth = topic->qos.keep_last;
    if (topic->history){
        for (d=0; d<depth; d++)
            if (topic->history[d].buf){
                st->cfg.allocator(st->cfg.user, topic->history[d].buf, 0);
                topic->history[d].buf = NULL; topic->history[d].cap = 0;
            }
        if (topic->history_owned){
            st->cfg.allocator(st->cfg.user, topic->history, 0);
            topic->history = NULL; topic->history_owned = 0;
        } else {
            memset(topic->history, 0, (size_t)depth*sizeof(i_DartWriterSample));
        }
    }
    topic->history_head = 0; topic->have_first = 0; topic->first_seqno = 0;
    memset(&topic->repair_stats, 0, sizeof topic->repair_stats);
    /* the caller re advertises: the slot now rides the announce as a hole */
    return 0;
}


int dart_transport_topic_reuse_find(DartTransportState *st, const char *name, uint8_t kind,
                                    uint16_t *index_out){
    uint64_t id; uint16_t c; int any = -1;
    if (!st || !name || !name[0]) return 0;
    id = dart_topic_id(name);
    for (c=0;c<st->cfg.n_topics;c++){
        const i_DartTopic *t = &st->topics[c];
        if (!t->retired) continue;
        if (t->identity == id && t->kind == kind){ if (index_out) *index_out = c; return 2; }
        if (any < 0) any = (int)c;
    }
    if (any >= 0){ if (index_out) *index_out = (uint16_t)any; return 1; }
    return 0;
}


/* Rebinds a retired slot. The seqno line always continues: an identical rebind keeps a
 * peer's reader position valid, and a changed one re forms every lane anyway. */
int dart_transport_topic_reuse(DartTransportState *st, uint16_t topic_index,
                               const DartTopicDef *def, int binding_changed,
                               uint32_t rebind_version){
    i_DartTopic *topic; DartQos q; uint16_t depth, p; size_t nlen; uint64_t id;
    if (!st || !def) return -1;
    topic = i_dart_topic_at(st, topic_index, NULL);
    if (!topic || !topic->retired) return -1;
    if (!def->name || !def->name[0]) return -1;
    nlen = i_dart_name_len(def->name);
    if (def->name[nlen]) return -1;                          /* longer than DART_TOPIC_NAME_MAX */
    if (def->directed && def->qos.catch_up) return -1;       /* directed history never replays */
    id = dart_topic_identity(def);
    {   /* the same name under a different kind on one node is refused, as at define */
        uint16_t c;
        for (c=0;c<st->cfg.n_topics;c++)
            if (c != topic_index && i_dart_topic_announced(&st->topics[c])
                && st->topics[c].identity == id && st->topics[c].kind != def->kind) return -1;
    }
    if (!binding_changed && (topic->identity != id || topic->kind != def->kind))
        return -1;                       /* asserted identical, but the stored binding differs */
    q = def->qos; i_dart_qos_defaults(&q);
    depth = q.keep_last;
    if (topic->history && !topic->history_owned && depth <= topic->qos.keep_last){
        /* an arena ring that still fits is reused in place */
        memset(topic->history, 0, (size_t)depth*sizeof(i_DartWriterSample));
    } else {
        i_DartWriterSample *h = (i_DartWriterSample*)st->cfg.allocator(st->cfg.user, NULL,
                                                     (size_t)depth*sizeof(i_DartWriterSample));
        if (!h) return -4;                                   /* OOM: the slot stays retired */
        memset(h, 0, (size_t)depth*sizeof(i_DartWriterSample));
        topic->history = h; topic->history_owned = 1;
    }
    topic->qos = q;
    topic->role = def->role;
    topic->kind = def->kind; topic->prefix_bytes = def->prefix_bytes; topic->directed = def->directed;
    topic->attrs = def->attrs;
    topic->identity = id;
    memcpy((char*)topic->name, def->name, nlen); ((char*)topic->name)[nlen] = '\0';
    topic->name_len = (uint8_t)nlen;
    topic->history_head = 0; topic->have_first = 0; topic->first_seqno = 0;
    memset(&topic->repair_stats, 0, sizeof topic->repair_stats);
    topic->retired = 0;
    if (binding_changed){
        topic->gen = (uint8_t)(topic->gen + 1u);   /* peers compare equality only */
        topic->rebind_version = rebind_version;
        for (p=0;p<st->cfg.max_peers;p++){
            uint32_t a;
            if (!st->peer_used[p]) continue;
            /* sever our bindings to the old occupant: those verdicts re pend and the
               interest bits derived from them go too */
            if (st->peer_index[p])
                for (a=0;a<st->peer_index_len[p];a++)
                    if (st->peer_index[p][a] == topic_index){
                        st->peer_index[p][a] = 0xFFFFu;
                        st->peer_astate[p][a] = 0;
                        if (st->peer_attrs[p]) st->peer_attrs[p][a] = 0;
                    }
            i_dart_bit_clr(&st->peer_pub_bitmap[(size_t)p*st->bitmap_len], topic_index);
            i_dart_bit_clr(&st->peer_sub_bitmap[(size_t)p*st->bitmap_len], topic_index);
            i_dart_bit_clr(&st->peer_sub_reliable[(size_t)p*st->bitmap_len], topic_index);
            /* dissolved verdicts re pend, as at define. Attrs stay. */
            if (st->peer_astate[p]){
                uint8_t *as = st->peer_astate[p];
                for (a=0;a<st->peer_index_len[p];a++)
                    if ((as[a] & DART__AST_DETAILED) && !(as[a] & DART__AST_NAME_OK))
                        as[a] = 0;
            }
        }
    }
    for (p=0;p<st->cfg.max_peers;p++)
        if (st->peer_used[p]) i_dart_topic_rematch(st, topic_index, p);
    return 0;
}


/* Releases the writer lanes the advance takes out of the rebind hold. 1 when a lane
 * actually formed, so the caller re fires its interest event. */
int dart_transport_peer_seen_version(DartTransportState *st, uint32_t peer_id, uint32_t version){
    int s; uint16_t c; uint32_t old; int changed = 0;
    if (!st) return 0;
    s = i_dart_peer_slot(st, peer_id);
    if (s < 0) return 0;
    old = st->peer_seen_version[s];
    if (version <= old) return 0;
    st->peer_seen_version[s] = version;
    for (c=0;c<st->cfg.n_topics;c++){
        i_DartTopic *t = &st->topics[c];
        i_DartLane *l; int was;
        if (!t->rebind_version || t->rebind_version <= old || t->rebind_version > version) continue;
        if (!i_dart_topic_announced(t)) continue;
        l = i_dart_lane_at(st, c, (uint32_t)s); was = l && l->w.used;
        i_dart_topic_rematch(st, c, (uint16_t)s);
        l = i_dart_lane_at(st, c, (uint32_t)s);
        if ((l && l->w.used) != was) changed = 1;
    }
    return changed;
}


uint64_t dart_transport_topic_seqno(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = st ? i_dart_topic_at(st, topic_index, NULL) : NULL;
    return topic ? topic->next_seqno : 0;
}


DartString dart_transport_topic_name(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    if (!topic || topic->name_len == 0) return dart_string(NULL, 0);   /* undefined */
    return dart_string(topic->name, topic->name_len);
}


const DartQos *dart_transport_topic_qos(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    return topic ? &topic->qos : NULL;
}

uint8_t dart_transport_topic_attrs(DartTransportState *st, uint16_t topic_index){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    return topic ? topic->attrs : 0;
}


void dart_transport_repair_stats(DartTransportState *st, uint16_t topic_index, DartRepairStats *out){
    i_DartTopic *topic = i_dart_topic_at(st, topic_index, NULL);
    if (!out) return;
    if (topic) *out = topic->repair_stats;
    else memset(out, 0, sizeof *out);
}


void dart_transport_on_datagram(DartTransportState *st, uint32_t from, DartBytes datagram, uint64_t now){
    const uint8_t *p=datagram.data; size_t rem=datagram.len;
    int peer_slot=i_dart_peer_slot(st,from);
    if (peer_slot<0) return;
    /* concatenated submessages, each length from its own header */
    while (rem>=3){
        uint8_t b0=p[0], type=(uint8_t)(b0 & DART_MSG_MASK); uint16_t index; size_t sub; int topic_index;
        switch(type){
            case DART_DATA:
#ifdef DART_SHM
                            if (b0 & DART_F_SHM){ if (rem<DART_SHM_DATA_BYTES) return; sub=DART_SHM_DATA_BYTES; }
                            else
#endif
                            if (b0 & DART_F_SINGLE){ if (rem<DART_HEADER_DATA_SINGLE) return; sub=DART_HEADER_DATA_SINGLE+(size_t)i_dart_le_r16(p+DART_OFFSET_PAYLOAD_LEN_SINGLE); }
                            else { if (rem<DART_HEADER_DATA_MULTI) return; sub=DART_HEADER_DATA_MULTI+(size_t)i_dart_le_r16(p+DART_OFFSET_PAYLOAD_LEN); } break;
            case DART_HB:   if (rem<DART_HEADER_HB) return; sub=DART_HEADER_HB; break;
            case DART_NACK: if (rem<DART_HEADER_NACK) return; sub=DART_HEADER_NACK; break;
            default: return;             /* unknown type: cannot resync, drop the rest */
        }
        if (sub>rem) return;             /* truncated */
        index = i_dart_le_r16(p+DART_OFFSET_INDEX);
        if ((uint32_t)index < st->peer_index_len[peer_slot]){
            uint16_t m=st->peer_index[peer_slot][index]; topic_index=(m==0xFFFFu)?-1:(int)m;
        } else topic_index=-1;
        if (topic_index>=0){
            switch(type){
                case DART_DATA:
#ifdef DART_SHM
                                if (p[0] & DART_F_SHM){ i_dart_reader_shm(st,topic_index,peer_slot,p,now); break; }
#endif
                                i_dart_reader_data(st,topic_index,peer_slot,p,now); break;
                case DART_HB:   i_dart_reader_hb  (st,topic_index,peer_slot,p,now); break;
                case DART_NACK: i_dart_writer_nack(st,topic_index,peer_slot,p,now); break;
            }
        }
        p+=sub; rem-=sub;
    }
}
#pragma endregion

#ifndef DART_TRANSPORT_SANS_IO
#pragma region serialize/schema.c
#include <string.h>

/* The wire format and the layout rules are in spec/schema.md. */

/* One field of the flat table. Offsets are message absolute, except members of a
 * variable struct array, which are relative to their element. */
typedef struct {
    DartString name;      /* a view into the wire bytes */
    DartString type_name; /* the field type's NAMED tag, or {NULL,0} */
    DartString elem_name; /* an array element type's NAMED tag, or {NULL,0} */
    uint32_t   offset;    /* element 0 under an array, 0 for variable kinds */
    uint32_t   size;      /* 0 for variable kinds */
    uint32_t   elem_size; /* ARR and VARR: bytes of one element, else 0 */
    uint32_t   type_off;  /* wire offset of the type encoding, for the subset compare */
    uint32_t   type_len;
    uint16_t   count;     /* ARR element count, or ENUM variant count, else 0 */
    uint16_t   depth;     /* 0 = top level */
    uint16_t   parent;    /* flat index of the enclosing struct or array, 0xFFFF = root */
    uint16_t   arr_parent;/* flat index of the enclosing struct array, 0xFFFF = none */
    uint16_t   str_cap;   /* STR fields and STR elements, else 0 */
    uint16_t   var_ord;   /* variable kinds: the ordinal of this field's tail frame */
    uint8_t    kind;
    uint8_t    elem;      /* ARR and VARR element kind, else 0 */
} i_Field;

struct DartSchema {
    DartBytes   wire;       /* the canonical bytes, a view into the block */
    uint64_t    hash;
    uint32_t    size;       /* the fixed section size */
    DartString  name;       /* the root name, a view into the wire, "" if anonymous */
    uint16_t    nfields;
    uint16_t    n_var;      /* variable fields */
    uint8_t     value_root; /* 1 = a bare or alias root, not a struct */
    i_Field     fields[1];   /* nfields entries in the block */
};

#define I_DART_NO_PARENT 0xFFFFu

static int i_dart_kind_var(uint8_t k){
    return k == DART_VSTR || k == DART_VARR || k == DART_MAP;
}

uint32_t dart_schema_scalar_size(DartSchemaTypeKind kind){
    switch (kind){
        case DART_U8: case DART_I8: case DART_BOOL: return 1;
        case DART_U16: case DART_I16:               return 2;
        case DART_U32: case DART_I32: case DART_F32: return 4;
        case DART_U64: case DART_I64: case DART_F64: return 8;
        default: return 0;
    }
}

/* enum backing values: an integer kind, read and written like the scalar */
static int i_dart_enum_backing_ok(uint8_t backing){ return backing <= (uint8_t)DART_I64; }
/* widened to int64, sign extended for a signed kind */
static int64_t i_dart_enum_read_val(uint8_t backing, const uint8_t *p){
    switch (backing){
        case DART_U8:  return (int64_t)(uint64_t)p[0];
        case DART_U16: return (int64_t)(uint64_t)i_dart_le_r16(p);
        case DART_U32: return (int64_t)(uint64_t)i_dart_le_r32(p);
        case DART_U64: return (int64_t)i_dart_le_r64(p);
        case DART_I8:  return (int64_t)(int8_t)p[0];
        case DART_I16: return (int64_t)(int16_t)i_dart_le_r16(p);
        case DART_I32: return (int64_t)(int32_t)i_dart_le_r32(p);
        case DART_I64: return (int64_t)i_dart_le_r64(p);
        default:       return 0;
    }
}
/* truncated to the backing */
static void i_dart_enum_write_val(uint8_t backing, uint8_t *p, int64_t v){
    uint64_t u = (uint64_t)v; uint32_t bs = dart_schema_scalar_size((DartSchemaTypeKind)backing), i;
    for (i = 0; i < bs; i++) p[i] = (uint8_t)(u >> (8 * i));
}
/* a u64 above INT64_MAX is not expressible */
static int i_dart_enum_val_fits(uint8_t backing, int64_t v){
    switch (backing){
        case DART_U8:  return v >= 0 && v <= 0xFF;
        case DART_U16: return v >= 0 && v <= 0xFFFF;
        case DART_U32: return v >= 0 && v <= (int64_t)0xFFFFFFFF;
        case DART_U64: return v >= 0;
        case DART_I8:  return v >= -128 && v <= 127;
        case DART_I16: return v >= -32768 && v <= 32767;
        case DART_I32: return v >= (int64_t)(-2147483647 - 1) && v <= 2147483647;
        case DART_I64: return 1;
        default:       return 0;
    }
}

/* the bounds checked reader over possibly hostile wire bytes */
typedef struct { const uint8_t *w; size_t n, pos; int fail; } i_Rd;
static uint8_t  i_dart_rd_u8 (i_Rd *r){ if (r->pos + 1 > r->n){ r->fail = 1; return 0; } return r->w[r->pos++]; }
static uint16_t i_dart_rd_u16(i_Rd *r){ uint16_t v; if (r->pos + 2 > r->n){ r->fail = 1; return 0; } v = i_dart_le_r16(r->w + r->pos); r->pos += 2; return v; }
static void     i_dart_rd_skip(i_Rd *r, size_t k){ if (r->pos + k > r->n){ r->fail = 1; r->pos = r->n; return; } r->pos += k; }

/* The kind at r->pos with any NAMED wrappers peeled off, 0xFF if the wire runs out. */
static uint8_t i_dart_peek_kind(const i_Rd *r){
    size_t p = r->pos;
    for (;;){
        if (p >= r->n) return 0xFFu;
        if (r->w[p] != DART_NAMED) return r->w[p];
        if (p + 2u > r->n) return 0xFFu;
        p += 2u + r->w[p + 1];
    }
}

/* Where a type sits relative to an array. DIRECT is an element type, so not itself an
 * array. INSIDE is anywhere within one, so no variable kinds and no further struct arrays. */
#define I_T_ELEM_DIRECT 1u
#define I_T_ELEM_INSIDE 2u

/* The fixed byte size of the type at r->pos, advancing past it. Counts every field walked
 * into *fields and every variable field into *nvar. Fails on any rule break. */
static uint32_t i_dart_rd_type_size(i_Rd *r, uint32_t *fields, uint32_t *nvar,
                                    uint16_t depth, uint32_t fl){
    uint8_t k = i_dart_rd_u8(r);
    if (r->fail) return 0;
    switch (k){
        case DART_U8: case DART_I8: case DART_BOOL: return 1;
        case DART_U16: case DART_I16:               return 2;
        case DART_U32: case DART_I32: case DART_F32: return 4;
        case DART_U64: case DART_I64: case DART_F64: return 8;
        case DART_STR:
            return 2u + (uint32_t)i_dart_rd_u16(r);              /* [u16 len][cap bytes] */
        case DART_ARR: {
            uint16_t count; uint8_t ek; uint64_t es;
            if (fl & I_T_ELEM_DIRECT){ r->fail = 1; return 0; }  /* no array of arrays */
            count = i_dart_rd_u16(r);
            if (r->fail) return 0;
            ek = i_dart_peek_kind(r);
            if (ek == 0xFFu){ r->fail = 1; return 0; }
            if ((fl & I_T_ELEM_INSIDE) && ek == DART_STRUCT){ r->fail = 1; return 0; }
            es = i_dart_rd_type_size(r, fields, nvar, (uint16_t)(depth + 1),
                                     I_T_ELEM_DIRECT | I_T_ELEM_INSIDE);
            if (r->fail || es == 0){ r->fail = 1; return 0; }    /* elements must be fixed */
            if ((uint64_t)count * es > 0xFFFFFFFFu){ r->fail = 1; return 0; }
            return (uint32_t)((uint64_t)count * es);
        }
        case DART_ENUM: {
            uint8_t backing; uint16_t n; uint32_t bs, i;
            if (fl & I_T_ELEM_DIRECT){ r->fail = 1; return 0; }  /* the element kind is ambiguous */
            backing = i_dart_rd_u8(r);
            n = i_dart_rd_u16(r);
            bs = dart_schema_scalar_size((DartSchemaTypeKind)backing);
            if (r->fail || bs == 0 || !i_dart_enum_backing_ok(backing)){ r->fail = 1; return 0; }
            for (i = 0; i < n && !r->fail; i++){                  /* skip the option table */
                i_dart_rd_skip(r, bs);
                i_dart_rd_skip(r, i_dart_rd_u8(r));
            }
            if (r->fail) return 0;
            return bs;   /* on the wire: just the backing scalar */
        }
        case DART_VSTR: case DART_MAP:
            if (fl){ r->fail = 1; return 0; }                    /* never inside an array element */
            if (nvar) (*nvar)++;
            return 0;
        case DART_VARR: {
            uint64_t es;
            if (fl){ r->fail = 1; return 0; }
            if (i_dart_peek_kind(r) == 0xFFu){ r->fail = 1; return 0; }
            es = i_dart_rd_type_size(r, fields, nvar, (uint16_t)(depth + 1),
                                     I_T_ELEM_DIRECT | I_T_ELEM_INSIDE);
            if (r->fail || es == 0){ r->fail = 1; return 0; }
            if (nvar) (*nvar)++;
            return 0;
        }
        case DART_STRUCT: {
            uint8_t nf; uint64_t sum = 0; uint16_t i;
            if (depth >= DART_SCHEMA_MAX_DEPTH){ r->fail = 1; return 0; }
            nf = i_dart_rd_u8(r);
            for (i = 0; i < nf && !r->fail; i++){
                uint8_t fnl = i_dart_rd_u8(r);
                i_dart_rd_skip(r, fnl);
                if (fields) (*fields)++;
                sum += i_dart_rd_type_size(r, fields, nvar, (uint16_t)(depth + 1),
                                           fl & I_T_ELEM_INSIDE);
            }
            if (sum > 0xFFFFFFFFu){ r->fail = 1; return 0; }
            return (uint32_t)sum;
        }
        case DART_NAMED: {
            uint8_t nl = i_dart_rd_u8(r);
            if (r->fail || nl == 0){ r->fail = 1; return 0; }    /* a name is required */
            i_dart_rd_skip(r, nl);
            if (r->fail) return 0;
            if (r->pos < r->n && r->w[r->pos] == DART_NAMED){ r->fail = 1; return 0; }
            return i_dart_rd_type_size(r, fields, nvar, depth, fl);
        }
        default: r->fail = 1; return 0;                          /* unknown kind: reject */
    }
}

/* Offset of the compiled handle in buf: past the wire bytes, 8 aligned. */
static size_t i_dart_schema_handle_off(const uint8_t *buf, size_t wire_len){
    uintptr_t addr = (uintptr_t)buf + wire_len;
    size_t pad = (size_t)((8u - (addr & 7u)) & 7u);
    return wire_len + pad;
}

/* Counts every field of the root into *n and its variable fields into *nvar. 0 on
 * malformed wire. A bare or alias root is 1 plus whatever its type flattens to. */
static int i_dart_schema_wire_fields(const void *wire, size_t wire_len,
                                     uint32_t *n, uint32_t *nvar){
    i_Rd r; uint8_t ver, rl;
    *n = 0; *nvar = 0;
    r.w = (const uint8_t *)wire; r.n = wire_len; r.pos = 0; r.fail = 0;
    ver = i_dart_rd_u8(&r); if (r.fail || ver != DART_SCHEMA_WIRE_VERSION) return 0;
    rl = i_dart_rd_u8(&r); i_dart_rd_skip(&r, rl);
    if (r.fail || (size_t)r.pos >= r.n) return 0;
    if (r.w[r.pos] == DART_NAMED) return 0;          /* a root's name rides the header */
    if (r.w[r.pos] == DART_STRUCT){
        i_dart_rd_type_size(&r, n, nvar, 0, 0);
    } else {
        uint32_t sub = 0;
        i_dart_rd_type_size(&r, &sub, nvar, 0, 0);   /* depth 0: a variable root is allowed */
        *n = 1u + sub;                               /* the root is the first field */
    }
    return r.fail ? 0 : 1;
}

/* flattening a wire type into the field table */
static void i_dart_field_init(i_Field *f, DartString name, uint16_t depth, uint16_t parent,
                              uint32_t offset){
    memset(f, 0, sizeof *f);
    f->name = name;
    f->type_name = dart_string(NULL, 0);
    f->elem_name = dart_string(NULL, 0);
    f->depth = depth; f->parent = parent; f->arr_parent = I_DART_NO_PARENT;
    f->offset = offset;
}

static uint32_t i_dart_emit_type(uint8_t *buf, size_t wl, i_Rd *r, DartSchema *s,
                                 uint16_t *emitted, uint32_t total,
                                 uint16_t idx, uint16_t depth, uint32_t base);

/* Emits the members of the struct body at r->pos into the table, depth first. Returns
 * the struct's fixed size and sets r->fail on malformed wire. */
static uint32_t i_dart_emit_struct(uint8_t *buf, size_t wl, i_Rd *r, DartSchema *s,
                                   uint16_t *emitted, uint32_t total,
                                   uint16_t depth, uint16_t parent, uint32_t base){
    uint8_t nf = i_dart_rd_u8(r); uint16_t i;
    uint32_t running = base;
    if (depth >= DART_SCHEMA_MAX_DEPTH){ r->fail = 1; return 0; }
    for (i = 0; i < nf && !r->fail; i++){
        uint8_t fl = i_dart_rd_u8(r);
        const char *fn = (const char *)(buf + r->pos);
        uint16_t idx; uint32_t sz;
        i_dart_rd_skip(r, fl);
        if (r->fail) return 0;
        if (*emitted >= total){ r->fail = 1; return 0; }
        idx = (*emitted)++;
        i_dart_field_init(&s->fields[idx], dart_string(fn, fl), depth, parent, running);
        sz = i_dart_emit_type(buf, wl, r, s, emitted, total, idx, depth, running);
        if (r->fail) return 0;
        running += sz;
    }
    return running - base;
}

/* Fills field idx from the type at r->pos, NAMED wrappers unwrapped into type_name, and
 * emits any child fields it flattens to. base is where the field's storage starts. */
static uint32_t i_dart_emit_type(uint8_t *buf, size_t wl, i_Rd *r, DartSchema *s,
                                 uint16_t *emitted, uint32_t total,
                                 uint16_t idx, uint16_t depth, uint32_t base){
    size_t kpos = r->pos;
    uint8_t k;
    uint32_t sz = 0;
    i_Field *f = &s->fields[idx];
    f->type_off = (uint32_t)kpos;
    k = i_dart_rd_u8(r);
    if (k == DART_NAMED){
        uint8_t nl = i_dart_rd_u8(r);
        const char *tn = (const char *)(buf + r->pos);
        i_dart_rd_skip(r, nl);
        if (r->fail || nl == 0){ r->fail = 1; return 0; }
        f->type_name = dart_string(tn, nl);
        k = i_dart_rd_u8(r);
        if (k == DART_NAMED){ r->fail = 1; return 0; }
    }
    if (r->fail) return 0;
    f->kind = k;
    switch (k){
        case DART_STRUCT:
            sz = i_dart_emit_struct(buf, wl, r, s, emitted, total,
                                    (uint16_t)(depth + 1), idx, base);
            break;
        case DART_ARR: case DART_VARR: {
            uint8_t ek; uint32_t esz = 0;
            if (k == DART_ARR){
                f->count = i_dart_rd_u16(r);
                if (r->fail) return 0;
            }
            ek = i_dart_rd_u8(r);
            if (ek == DART_NAMED){
                uint8_t nl = i_dart_rd_u8(r);
                const char *tn = (const char *)(buf + r->pos);
                i_dart_rd_skip(r, nl);
                if (r->fail || nl == 0){ r->fail = 1; return 0; }
                f->elem_name = dart_string(tn, nl);
                ek = i_dart_rd_u8(r);
                if (ek == DART_NAMED){ r->fail = 1; return 0; }
            }
            if (r->fail) return 0;
            f->elem = ek;
            if (ek == DART_STRUCT){                       /* one element 0 template */
                esz = i_dart_emit_struct(buf, wl, r, s, emitted, total,
                                         (uint16_t)(depth + 1), idx,
                                         k == DART_ARR ? base : 0u);
            } else if (ek == DART_STR){
                f->str_cap = i_dart_rd_u16(r);
                esz = 2u + (uint32_t)f->str_cap;
            } else {
                esz = dart_schema_scalar_size((DartSchemaTypeKind)ek);
            }
            if (r->fail || esz == 0){ r->fail = 1; return 0; }
            f->elem_size = esz;
            if (k == DART_ARR){
                if ((uint64_t)f->count * esz > 0xFFFFFFFFu){ r->fail = 1; return 0; }
                sz = (uint32_t)((uint64_t)f->count * esz);
            }
            break;
        }
        case DART_STR:
            f->str_cap = i_dart_rd_u16(r);
            sz = 2u + (uint32_t)f->str_cap;
            break;
        case DART_ENUM: {
            uint16_t i;
            f->elem = i_dart_rd_u8(r);
            f->count = i_dart_rd_u16(r);
            sz = dart_schema_scalar_size((DartSchemaTypeKind)f->elem);
            if (r->fail || sz == 0 || !i_dart_enum_backing_ok(f->elem)){ r->fail = 1; return 0; }
            for (i = 0; i < f->count && !r->fail; i++){
                i_dart_rd_skip(r, sz);
                i_dart_rd_skip(r, i_dart_rd_u8(r));
            }
            break;
        }
        case DART_VSTR: case DART_MAP:
            sz = 0;
            break;
        default:
            sz = dart_schema_scalar_size((DartSchemaTypeKind)k);
            if (sz == 0){ r->fail = 1; return 0; }
            break;
    }
    if (r->fail) return 0;
    f = &s->fields[idx];                      /* unchanged, recursion never moves it */
    f->type_len = (uint32_t)(r->pos - kpos);
    f->size = sz;
    return sz;
}

/* Compiles the wire bytes at buf[0..wire_len] into a DartSchema placed after them in
 * buf. NULL on a malformed blob or when cap is too small. */
static DartSchema *i_dart_schema_compile(uint8_t *buf, size_t wire_len, size_t cap){
    i_Rd r; uint8_t ver, root_kind, root_namelen;
    const char *root_name; size_t hoff, need; DartSchema *s;
    uint32_t total, nvar; uint16_t emitted = 0;

    if (!i_dart_schema_wire_fields(buf, wire_len, &total, &nvar) || total > 0xFFFFu) return NULL;

    r.w = buf; r.n = wire_len; r.pos = 0; r.fail = 0;
    ver = i_dart_rd_u8(&r);
    if (r.fail || ver != DART_SCHEMA_WIRE_VERSION) return NULL;
    root_namelen = i_dart_rd_u8(&r);
    root_name = (const char *)(buf + r.pos);
    i_dart_rd_skip(&r, root_namelen);
    if (r.fail || r.pos >= r.n) return NULL;
    root_kind = buf[r.pos];

    hoff = i_dart_schema_handle_off(buf, wire_len);
    need = hoff + sizeof(DartSchema) + (size_t)(total ? total - 1u : 0u) * sizeof(i_Field);
    if (need > cap) return NULL;

    s = (DartSchema *)(buf + hoff);
    s->wire = dart_bytes(buf, wire_len);
    s->name = dart_string(root_name, root_namelen);
    s->nfields = (uint16_t)total;
    s->n_var = (uint16_t)nvar;
    s->value_root = (uint8_t)(root_kind != DART_STRUCT);
    s->hash = i_dart_fnv1a64(buf, wire_len);
    if (!s->value_root){
        r.pos++;                                 /* past the root's STRUCT kind byte */
        s->size = i_dart_emit_struct(buf, wire_len, &r, s, &emitted, total,
                                     0, I_DART_NO_PARENT, 0);
    } else {                                     /* the bare or alias root is the first field */
        if (total == 0) return NULL;
        i_dart_field_init(&s->fields[0], dart_string((const char *)(buf + r.pos), 0),
                          0, I_DART_NO_PARENT, 0);
        emitted = 1;
        s->size = i_dart_emit_type(buf, wire_len, &r, s, &emitted, total, 0, 0, 0);
    }
    if (r.fail || emitted != (uint16_t)total) return NULL;
    {   /* tail frame ordinals in depth first order, and the array ancestry */
        uint16_t i, ord = 0;
        for (i = 0; i < s->nfields; i++){
            i_Field *f = &s->fields[i];
            if (f->parent != I_DART_NO_PARENT){
                const i_Field *p = &s->fields[f->parent];
                f->arr_parent = (p->kind == DART_ARR || p->kind == DART_VARR)
                              ? f->parent : p->arr_parent;
            }
            if (i_dart_kind_var(f->kind)){
                f->offset = 0;                   /* a frame has no static offset */
                f->var_ord = ord++;
            }
        }
    }
    return s;
}
/* the builder */
/* room for extra more bytes, growing the wire buffer through the hook */
static int i_dart_schema_builder_reserve(DartSchemaBuilder *b, size_t extra){
    size_t newcap; uint8_t *nb;
    if (b->err) return 0;
    if (b->len + extra <= b->cap) return 1;
    newcap = b->cap ? b->cap : 64u;
    while (newcap < b->len + extra){
        if (newcap > ((size_t)-1) / 2u){ newcap = b->len + extra; break; }
        newcap *= 2u;
    }
    nb = (uint8_t *)b->alloc(b->user, b->buf, newcap);
    if (!nb){ b->err = -1; return 0; }              /* a failed realloc leaves b->buf intact */
    b->buf = nb; b->cap = newcap;
    return 1;
}
static void i_dart_schema_builder_put(DartSchemaBuilder *b, uint8_t v){
    if (!i_dart_schema_builder_reserve(b, 1)) return;
    b->buf[b->len++] = v;
}
static void i_dart_schema_builder_put_u16(DartSchemaBuilder *b, uint16_t v){
    if (!i_dart_schema_builder_reserve(b, 2)) return;
    i_dart_le_w16(b->buf + b->len, v); b->len += 2;
}
/* raw bytes, a compiled type encoding from elsewhere */
static void i_dart_schema_builder_put_raw(DartSchemaBuilder *b, const void *src, size_t len){
    if (!len) return;
    if (!src){ b->err = -6; return; }
    if (!i_dart_schema_builder_reserve(b, len)) return;
    memcpy(b->buf + b->len, src, len);
    b->len += len;
}
/* bytes little endian bytes of v, an enum option's backing sized value */
static void i_dart_schema_builder_put_le(DartSchemaBuilder *b, uint64_t v, uint32_t bytes){
    uint32_t i;
    if (!i_dart_schema_builder_reserve(b, bytes)) return;
    for (i = 0; i < bytes; i++) b->buf[b->len++] = (uint8_t)(v >> (8 * i));
}
static void i_dart_schema_builder_put_name(DartSchemaBuilder *b, const char *name){
    size_t n = 0, i; if (name) while (name[n]) n++;
    if (b->depth == 0 && (b->raw_type || b->value_root)){   /* the root carries no field name */
        if (n) b->err = -4;
        return;
    }
    if (n > 255){ b->err = -4; return; }
    if (!i_dart_schema_builder_reserve(b, 1 + n)) return;
    b->buf[b->len++] = (uint8_t)n;
    for (i = 0; i < n; i++) b->buf[b->len++] = (uint8_t)name[i];
}
/* count a field on the innermost open struct. A bare or alias root takes exactly one type */
static void i_dart_schema_builder_count(DartSchemaBuilder *b){
    uint16_t *c;
    if (b->err) return;
    if (b->depth > 0){
        c = &b->field_count[b->depth - 1];
        if (*c >= 255){ b->err = -5; return; }   /* nfields is a u8 */
        (*c)++;
        return;
    }
    if (b->value_root){
        if (b->value_root == 2){ b->err = -3; return; }   /* a bare root holds one type */
        b->value_root = 2;
        return;
    }
    if (b->raw_type) return;                     /* a standalone type body */
    b->err = -3;                                 /* no open struct */
}
/* open a struct type: [STRUCT][nfields placeholder], push a nesting level */
static void i_dart_schema_builder_open_struct(DartSchemaBuilder *b){
    if (b->err) return;
    if (b->depth >= DART_SCHEMA_MAX_DEPTH){ b->err = -2; return; }
    i_dart_schema_builder_put(b, (uint8_t)DART_STRUCT);
    b->count_pos[b->depth] = b->len;
    i_dart_schema_builder_put(b, 0);
    b->field_count[b->depth] = 0;
    b->depth++;
}

DartSchemaBuilder dart_schema_begin(DartAllocFn alloc, void *user, const char *root_name){
    DartSchemaBuilder b;
    memset(&b, 0, sizeof b);
    b.alloc = alloc; b.user = user; b.arr_depth = 0xFFFFu; b.base_depth = 1;
    if (!alloc){ b.err = -1; return b; }
    b.cap = 64u;
    b.buf = (uint8_t *)alloc(user, NULL, b.cap);
    if (!b.buf){ b.err = -1; b.cap = 0; return b; }
    i_dart_schema_builder_put(&b, (uint8_t)DART_SCHEMA_WIRE_VERSION);
    i_dart_schema_builder_put_name(&b, root_name);
    i_dart_schema_builder_open_struct(&b);              /* the root is a struct, depth 1 */
    return b;
}

/* the shared head of a bare type root: [version][root_namelen][root_name] */
static DartSchemaBuilder i_dart_schema_begin_bare(DartAllocFn alloc, void *user,
                                                  const char *name){
    DartSchemaBuilder b; size_t n = 0, i;
    memset(&b, 0, sizeof b);
    b.alloc = alloc; b.user = user; b.arr_depth = 0xFFFFu;
    if (name) while (name[n]) n++;
    if (!alloc || n > 255){ b.err = -1; return b; }
    b.cap = 64u;
    b.buf = (uint8_t *)alloc(user, NULL, b.cap);
    if (!b.buf){ b.err = -1; b.cap = 0; return b; }
    b.value_root = 1;                                  /* depth stays 0: no struct is open */
    i_dart_schema_builder_put(&b, (uint8_t)DART_SCHEMA_WIRE_VERSION);
    i_dart_schema_builder_put(&b, (uint8_t)n);
    for (i = 0; i < n; i++) i_dart_schema_builder_put(&b, (uint8_t)name[i]);
    return b;
}

DartSchemaBuilder dart_schema_begin_value(DartAllocFn alloc, void *user){
    return i_dart_schema_begin_bare(alloc, user, NULL);
}

DartSchemaBuilder dart_schema_begin_alias(DartAllocFn alloc, void *user, const char *name){
    DartSchemaBuilder b = i_dart_schema_begin_bare(alloc, user, name);
    if (!b.err && (!name || !name[0])) b.err = -4;     /* an alias needs a name */
    return b;
}

/* a standalone type encoding with no header and no field name: the DSL's definition arena */
static DartSchemaBuilder i_dart_schema_begin_raw(DartAllocFn alloc, void *user){
    DartSchemaBuilder b;
    memset(&b, 0, sizeof b);
    b.alloc = alloc; b.user = user; b.arr_depth = 0xFFFFu; b.raw_type = 1;
    if (!alloc){ b.err = -1; return b; }
    b.cap = 64u;
    b.buf = (uint8_t *)alloc(user, NULL, b.cap);
    if (!b.buf){ b.err = -1; b.cap = 0; }
    return b;
}

void dart_schema_field(DartSchemaBuilder *b, const char *name, DartSchemaTypeKind kind){
    if (!b || b->err) return;
    if (dart_schema_scalar_size(kind) == 0){ b->err = -6; return; }  /* fixed scalars only */
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name); i_dart_schema_builder_put(b, (uint8_t)kind);
}

void dart_schema_field_array(DartSchemaBuilder *b, const char *name,
                             DartSchemaTypeKind elem_scalar, uint16_t count){
    if (!b || b->err) return;
    if (dart_schema_scalar_size(elem_scalar) == 0){ b->err = -6; return; }  /* scalars only */
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name);
    i_dart_schema_builder_put(b, (uint8_t)DART_ARR); i_dart_schema_builder_put_u16(b, count);
    i_dart_schema_builder_put(b, (uint8_t)elem_scalar);
}

void dart_schema_field_string(DartSchemaBuilder *b, const char *name, uint16_t cap){
    if (!b || b->err) return;
    if (cap == 0){ b->err = -6; return; }
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name);
    i_dart_schema_builder_put(b, (uint8_t)DART_STR); i_dart_schema_builder_put_u16(b, cap);
}

void dart_schema_field_string_array(DartSchemaBuilder *b, const char *name,
                                    uint16_t cap, uint16_t count){
    if (!b || b->err) return;
    if (cap == 0){ b->err = -6; return; }
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name);
    i_dart_schema_builder_put(b, (uint8_t)DART_ARR); i_dart_schema_builder_put_u16(b, count);
    i_dart_schema_builder_put(b, (uint8_t)DART_STR); i_dart_schema_builder_put_u16(b, cap);
}

/* variable kinds may sit at any struct depth but never inside an array element */
static int i_dart_schema_builder_var_ok(DartSchemaBuilder *b){
    if (b->err) return 0;
    if (b->arr_depth != 0xFFFFu){ b->err = -8; return 0; }
    return 1;
}

void dart_schema_field_var_string(DartSchemaBuilder *b, const char *name){
    if (!b || !i_dart_schema_builder_var_ok(b)) return;
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name);
    i_dart_schema_builder_put(b, (uint8_t)DART_VSTR);
}

void dart_schema_field_var_array(DartSchemaBuilder *b, const char *name,
                                 DartSchemaTypeKind elem_scalar){
    if (!b || !i_dart_schema_builder_var_ok(b)) return;
    if (dart_schema_scalar_size(elem_scalar) == 0){ b->err = -6; return; }  /* scalars only */
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name);
    i_dart_schema_builder_put(b, (uint8_t)DART_VARR); i_dart_schema_builder_put(b, (uint8_t)elem_scalar);
}

void dart_schema_field_var_string_array(DartSchemaBuilder *b, const char *name, uint16_t cap){
    if (!b || !i_dart_schema_builder_var_ok(b)) return;
    if (cap == 0){ b->err = -6; return; }
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name);
    i_dart_schema_builder_put(b, (uint8_t)DART_VARR);
    i_dart_schema_builder_put(b, (uint8_t)DART_STR); i_dart_schema_builder_put_u16(b, cap);
}

void dart_schema_field_map(DartSchemaBuilder *b, const char *name){
    if (!b || !i_dart_schema_builder_var_ok(b)) return;
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name);
    i_dart_schema_builder_put(b, (uint8_t)DART_MAP);
}

/* the root type bytes of a compiled schema, past [version][namelen][name] */
static int i_dart_schema_root_type(const DartSchema *t, const uint8_t **bytes, size_t *len){
    size_t off;
    if (!t || !t->wire.data) return 0;
    off = 2u + t->name.len;
    if (off >= t->wire.len) return 0;
    *bytes = t->wire.data + off;
    *len   = t->wire.len - off;
    return 1;
}

/* emit [NAMED][len][name] plus the referenced schema's root type */
static void i_dart_schema_put_named(DartSchemaBuilder *b, const DartSchema *type){
    const uint8_t *tb; size_t tl;
    if (b->err) return;
    if (!type || !type->name.len || type->name.len > 255 ||
        !i_dart_schema_root_type(type, &tb, &tl)){ b->err = -6; return; }
    i_dart_schema_builder_put(b, (uint8_t)DART_NAMED);
    i_dart_schema_builder_put(b, (uint8_t)type->name.len);
    i_dart_schema_builder_put_raw(b, type->name.data, type->name.len);
    i_dart_schema_builder_put_raw(b, tb, tl);
}

void dart_schema_field_named(DartSchemaBuilder *b, const char *name, const DartSchema *type){
    if (!b || b->err) return;
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name);
    i_dart_schema_put_named(b, type);
}

void dart_schema_field_named_array(DartSchemaBuilder *b, const char *name,
                                   const DartSchema *type, uint16_t count){
    if (!b || b->err) return;
    if (count == 0 && !i_dart_schema_builder_var_ok(b)) return;
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name);
    if (count){
        i_dart_schema_builder_put(b, (uint8_t)DART_ARR);
        i_dart_schema_builder_put_u16(b, count);
    } else {
        i_dart_schema_builder_put(b, (uint8_t)DART_VARR);
    }
    i_dart_schema_put_named(b, type);
}

/* Streaming enum construction, shared with the DSL so it never buffers the option list:
 * open writes the head and returns the count placeholder, add appends, finish backpatches. */
static size_t i_dart_schema_field_enum_open(DartSchemaBuilder *b, const char *name,
                                            DartSchemaTypeKind backing){
    size_t count_pos;
    i_dart_schema_builder_count(b);
    i_dart_schema_builder_put_name(b, name);
    i_dart_schema_builder_put(b, (uint8_t)DART_ENUM);
    i_dart_schema_builder_put(b, (uint8_t)backing);
    count_pos = b->len;
    i_dart_schema_builder_put_u16(b, 0);               /* n, backpatched by finish */
    return count_pos;
}
static void i_dart_schema_field_enum_add(DartSchemaBuilder *b, DartSchemaTypeKind backing,
                                         int64_t value, const char *name, size_t name_len){
    size_t i;
    if (!b || b->err) return;
    if (name_len > 255){ b->err = -4; return; }
    i_dart_schema_builder_put_le(b, (uint64_t)value, dart_schema_scalar_size(backing));
    if (!i_dart_schema_builder_reserve(b, 1 + name_len)) return;
    b->buf[b->len++] = (uint8_t)name_len;
    for (i = 0; i < name_len; i++) b->buf[b->len++] = (uint8_t)name[i];
}
static void i_dart_schema_field_enum_finish(DartSchemaBuilder *b, size_t count_pos, uint16_t count){
    if (!b || b->err) return;
    i_dart_le_w16(b->buf + count_pos, count);
}

void dart_schema_field_enum(DartSchemaBuilder *b, const char *name, DartSchemaTypeKind backing,
                            const DartEnumVariant *variants, uint16_t n){
    size_t count_pos; uint16_t i;
    if (!b || b->err) return;
    if (dart_schema_scalar_size(backing) == 0 || !i_dart_enum_backing_ok((uint8_t)backing)){
        b->err = -6; return;                           /* integer backings only */
    }
    count_pos = i_dart_schema_field_enum_open(b, name, backing);
    for (i = 0; i < n; i++){
        int64_t v = variants ? variants[i].value : 0;
        const char *vn = variants ? variants[i].name : NULL;
        size_t vl = 0; if (vn) while (vn[vl]) vl++;
        if (!i_dart_enum_val_fits((uint8_t)backing, v)){ b->err = -6; return; }
        i_dart_schema_field_enum_add(b, backing, v, vn, vl);
    }
    i_dart_schema_field_enum_finish(b, count_pos, n);
}

void dart_schema_begin_struct(DartSchemaBuilder *b, const char *name){
    if (!b || b->err) return;
    if (b->value_root && b->depth == 0){ b->err = -3; return; }  /* struct roots use begin */
    i_dart_schema_builder_count(b);                 /* a field of the parent */
    i_dart_schema_builder_put_name(b, name);
    i_dart_schema_builder_open_struct(b);
}

void dart_schema_begin_struct_array(DartSchemaBuilder *b, const char *name, uint16_t count){
    if (!b || b->err) return;
    if (count == 0 && !i_dart_schema_builder_var_ok(b)) return;
    if (b->arr_depth != 0xFFFFu){ b->err = -8; return; }   /* one array level only */
    i_dart_schema_builder_count(b);
    i_dart_schema_builder_put_name(b, name);
    if (count){
        i_dart_schema_builder_put(b, (uint8_t)DART_ARR);
        i_dart_schema_builder_put_u16(b, count);
    } else {
        i_dart_schema_builder_put(b, (uint8_t)DART_VARR);
    }
    i_dart_schema_builder_open_struct(b);
    if (!b->err) b->arr_depth = b->depth;           /* this level is an array element */
}

void dart_schema_end_struct(DartSchemaBuilder *b){
    if (!b || b->err) return;
    if (b->depth <= b->base_depth){ b->err = -3; return; }   /* the root closes in finish */
    if (b->arr_depth == b->depth) b->arr_depth = 0xFFFFu;
    b->depth--;
    b->buf[b->count_pos[b->depth]] = (uint8_t)b->field_count[b->depth];
}

DartSchema *dart_schema_finish(DartSchemaBuilder *b){
    DartSchema *s = NULL;
    int closed = b && !b->err &&
                 (b->value_root ? (b->depth == 0 && b->value_root == 2)   /* the one bare type */
                                : b->depth == 1);            /* else an unbalanced begin and end */
    if (closed){
        size_t need; uint8_t *nb; uint32_t total = 0, nvar = 0;
        if (!b->value_root)
            b->buf[b->count_pos[0]] = (uint8_t)b->field_count[0]; /* backpatch the count */
        i_dart_schema_wire_fields(b->buf, b->len, &total, &nvar);
        need = b->len + 7u + sizeof(DartSchema)
             + (size_t)(total ? total - 1u : 0u) * sizeof(i_Field);
        nb = (uint8_t *)b->alloc(b->user, b->buf, need);      /* room for the handle */
        if (nb){ b->buf = nb; b->cap = need; s = i_dart_schema_compile(b->buf, b->len, b->cap); }
    }
    if (!s && b && b->buf) b->alloc(b->user, b->buf, 0);      /* free on any failure */
    if (b) b->buf = NULL;                                     /* owned by s now, or freed */
    return s;
}

/* bytes a compiled schema needs for wire: the copy, the alignment pad, the handle and
   the field table. 0 if the wire is malformed. */
static size_t i_dart_schema_compiled_size(const void *wire, size_t wire_len){
    uint32_t total, nvar;
    if (!i_dart_schema_wire_fields(wire, wire_len, &total, &nvar) || total > 0xFFFFu) return 0;
    return wire_len + 7u + sizeof(DartSchema) + (size_t)(total ? total - 1u : 0u) * sizeof(i_Field);
}

DartSchema *dart_schema_parse(const void *wire, size_t wire_len, DartAllocFn alloc, void *user){
    size_t need, i; uint8_t *buf; DartSchema *s;
    if (!wire || !alloc || wire_len == 0) return NULL;
    need = i_dart_schema_compiled_size(wire, wire_len);
    if (need == 0) return NULL;                              /* a malformed header */
    buf = (uint8_t *)alloc(user, NULL, need);
    if (!buf) return NULL;
    for (i = 0; i < wire_len; i++) buf[i] = ((const uint8_t *)wire)[i];   /* persist the bytes */
    s = i_dart_schema_compile(buf, wire_len, need);
    if (!s) alloc(user, buf, 0);                             /* a malformed body: no leak */
    return s;
}

DartSchema *dart_schema_copy(const DartSchema *s, DartAllocFn alloc, void *user){
    DartBytes w;
    if (!s || !alloc) return NULL;
    w = dart_schema_wire(s);
    return dart_schema_parse(w.data, w.len, alloc, user);
}

void dart_schema_free(DartSchema *s, DartAllocFn alloc, void *user){
    if (s && alloc) alloc(user, (void *)s->wire.data, 0);    /* wire.data is the block base */
}

/* queries */
DartBytes dart_schema_wire(const DartSchema *s){
    DartBytes b; if (s) return s->wire;
    b.data = NULL; b.len = 0; return b;
}
uint64_t   dart_schema_hash(const DartSchema *s){ return s ? s->hash : 0; }
DartString dart_schema_name(const DartSchema *s){
    DartString n; if (s) return s->name;
    n.data = NULL; n.len = 0; return n;
}
uint32_t dart_schema_size(const DartSchema *s){ return s ? s->size : 0; }
uint16_t dart_schema_field_count(const DartSchema *s){ return s ? s->nfields : 0; }

int dart_schema_field_at(const DartSchema *s, uint16_t i, DartSchemaFieldInfo *out){
    const i_Field *f;
    if (!s || i >= s->nfields) return 0;
    f = &s->fields[i];
    if (out){
        out->name = f->name;
        out->type_name = f->type_name; out->elem_name = f->elem_name;
        out->kind = f->kind; out->elem = f->elem;
        out->count = f->count; out->depth = f->depth;
        out->str_cap = f->str_cap; out->arr_parent = f->arr_parent;
        out->offset = f->offset; out->size = f->size;
        out->elem_size = f->elem_size;
    }
    return 1;
}

/* Matches a dotted path against a field: the last segment is its own name, the earlier
 * ones its ancestors. A segment may carry [N] on an array field, which *index gets. */
static int i_dart_schema_path_match(const DartSchema *s, const i_Field *f, const char *path,
                                    size_t path_len, uint32_t *index){
    const char *end = path + path_len;
    uint32_t found = 0;
    for (;;){
        const char *seg = end, *nend;
        while (seg > path && seg[-1] != '.') seg--;
        nend = end;
        if (nend - seg >= 3 && nend[-1] == ']'){          /* strip a trailing [N] */
            const char *br = nend - 1;
            while (br > seg && br[-1] != '[') br--;
            if (br > seg){
                const char *q = br; uint32_t idx = 0; int ok = 1;
                while (q < nend - 1){
                    if (*q < '0' || *q > '9'){ ok = 0; break; }
                    idx = idx * 10u + (uint32_t)(*q - '0');
                    q++;
                }
                if (ok && q > br){
                    if (f->kind != DART_ARR && f->kind != DART_VARR) return 0;
                    found = idx;
                    nend = br - 1;
                }
            }
        }
        if (f->name.len != (size_t)(nend - seg) ||
            (f->name.len && memcmp(f->name.data, seg, f->name.len) != 0)) return 0;
        if (f->parent == I_DART_NO_PARENT){                /* root: all segments consumed */
            if (seg != path) return 0;
            if (index) *index = found;
            return 1;
        }
        if (seg == path) return 0;                         /* segments ran out early */
        end = seg - 1;                                     /* past the dot */
        f = &s->fields[f->parent];
    }
}

static const i_Field *i_dart_schema_field_by_path(const DartSchema *s, const char *path,
                                                  uint32_t *index){
    uint16_t i; size_t n;
    if (index) *index = 0;
    if (!s || !path) return NULL;
    n = strlen(path);
    for (i = 0; i < s->nfields; i++)
        if (i_dart_schema_path_match(s, &s->fields[i], path, n, index)) return &s->fields[i];
    return NULL;
}

int dart_schema_field_index(const DartSchema *s, const char *path){
    const i_Field *f = i_dart_schema_field_by_path(s, path, NULL);
    return f ? (int)(f - s->fields) : -1;
}

DartBytes dart_schema_field_type_wire(const DartSchema *s, uint16_t field){
    const i_Field *f;
    if (!s || field >= s->nfields) return dart_bytes(NULL, 0);
    f = &s->fields[field];
    if ((size_t)f->type_off + f->type_len > s->wire.len) return dart_bytes(NULL, 0);
    return dart_bytes(s->wire.data + f->type_off, f->type_len);
}

uint16_t dart_schema_enum_count(const DartSchema *s, uint16_t field){
    if (!s || field >= s->nfields) return 0;
    return s->fields[field].kind == DART_ENUM ? s->fields[field].count : 0;
}

int dart_schema_enum_variant(const DartSchema *s, uint16_t field, uint16_t i,
                             int64_t *value, DartString *name){
    const i_Field *f; const uint8_t *w; uint32_t pos, end; uint16_t k; uint8_t bs;
    if (!s || field >= s->nfields) return 0;
    f = &s->fields[field];
    if (f->kind != DART_ENUM || i >= f->count) return 0;
    bs  = (uint8_t)dart_schema_scalar_size((DartSchemaTypeKind)f->elem);
    w   = s->wire.data;
    /* past an optional NAMED tag, then [ENUM][backing][u16 n], to the first option */
    pos = f->type_off;
    if (w[pos] == DART_NAMED) pos += 2u + w[pos + 1];
    pos += 4u;
    end = f->type_off + f->type_len;
    for (k = 0; k < i; k++){                 /* hop over earlier options: [value][u8 nl][name] */
        if ((size_t)pos + bs + 1u > end) return 0;
        pos += bs + 1u + w[pos + bs];
    }
    if ((size_t)pos + bs + 1u > end) return 0;
    { uint8_t nl = w[pos + bs];
      if ((size_t)pos + bs + 1u + nl > end) return 0;
      if (value) *value = i_dart_enum_read_val(f->elem, w + pos);
      if (name)  *name  = dart_string((const char *)(w + pos + bs + 1u), nl); }
    return 1;
}

DartString dart_enum_name_of(const DartSchema *s, uint16_t field, int64_t value){
    uint16_t i, n = dart_schema_enum_count(s, field);
    for (i = 0; i < n; i++){
        int64_t v; DartString nm;
        if (dart_schema_enum_variant(s, field, i, &v, &nm) && v == value) return nm;
    }
    return dart_string(NULL, 0);
}

int dart_enum_value_of(const DartSchema *s, uint16_t field, const char *name, int64_t *out){
    uint16_t i, n = dart_schema_enum_count(s, field);
    size_t want = 0;
    if (name) while (name[want]) want++;
    for (i = 0; i < n; i++){
        int64_t v; DartString nm;
        if (dart_schema_enum_variant(s, field, i, &v, &nm) && nm.len == want &&
            (want == 0 || memcmp(nm.data, name, want) == 0)){ if (out) *out = v; return 1; }
    }
    return 0;
}
/* spelling types back as DSL text */
static const char *i_dart_why_kind(uint8_t k){
    switch (k){
        case DART_U8:  return "u8";  case DART_U16: return "u16";
        case DART_U32: return "u32"; case DART_U64: return "u64";
        case DART_I8:  return "i8";  case DART_I16: return "i16";
        case DART_I32: return "i32"; case DART_I64: return "i64";
        case DART_F32: return "f32"; case DART_F64: return "f64";
        case DART_BOOL: return "bool"; case DART_STRUCT: return "struct";
        default: return "?";
    }
}

/* A counting text sink: appends into [p, end) but always tallies the full length in n,
   so a NULL or short buffer still measures. */
typedef struct { char *p, *end; uint32_t n; } i_DartTextOut;
static void i_dart_out_raw(i_DartTextOut *o, const char *s, size_t len){
    size_t i;
    o->n += (uint32_t)len;
    for (i = 0; i < len && o->p < o->end; i++) *o->p++ = s[i];
}
static void i_dart_out_str(i_DartTextOut *o, const char *s){ i_dart_out_raw(o, s, strlen(s)); }
static void i_dart_out_view(i_DartTextOut *o, DartString v){ if (v.data) i_dart_out_raw(o, v.data, v.len); }
static void i_dart_out_indent(i_DartTextOut *o, int levels){ while (levels-- > 0) i_dart_out_raw(o, "  ", 2); }
static void i_dart_out_i64(i_DartTextOut *o, int64_t v){
    char tmp[20]; int k = 0; uint64_t u = v < 0 ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    if (v < 0) i_dart_out_raw(o, "-", 1);
    do { tmp[k++] = (char)('0' + (int)(u % 10)); u /= 10; } while (u);
    while (k) { char c = tmp[--k]; i_dart_out_raw(o, &c, 1); }
}

/* Steps over the type at pos in already validated wire. Returns the position past it. */
static size_t i_dart_skip_type(const uint8_t *w, size_t n, size_t pos){
    uint8_t k;
    if (pos >= n) return n;
    k = w[pos++];
    switch (k){
        case DART_NAMED:
            if (pos >= n) return n;
            pos += 1u + w[pos];
            return i_dart_skip_type(w, n, pos);
        case DART_STR: return pos + 2u <= n ? pos + 2u : n;
        case DART_ARR: return i_dart_skip_type(w, n, pos + 2u <= n ? pos + 2u : n);
        case DART_VARR: return i_dart_skip_type(w, n, pos);
        case DART_VSTR: case DART_MAP: return pos;
        case DART_ENUM: {
            uint8_t bs; uint16_t cnt, i;
            if (pos + 3u > n) return n;
            bs = (uint8_t)dart_schema_scalar_size((DartSchemaTypeKind)w[pos]);
            cnt = i_dart_le_r16(w + pos + 1);
            pos += 3u;
            for (i = 0; i < cnt; i++){
                if (pos + bs + 1u > n) return n;
                pos += bs + 1u + w[pos + bs];
            }
            return pos <= n ? pos : n;
        }
        case DART_STRUCT: {
            uint8_t nf; uint16_t i;
            if (pos >= n) return n;
            nf = w[pos++];
            for (i = 0; i < nf; i++){
                if (pos >= n) return n;
                pos += 1u + w[pos];
                if (pos > n) return n;
                pos = i_dart_skip_type(w, n, pos);
            }
            return pos;
        }
        default: return pos;
    }
}

/* Spells the type at pos as DSL text. A NAMED type spells as its bare name and the
 * caller hoists its definition. Returns the position past the type. */
static size_t i_dart_spell_type(i_DartTextOut *o, const uint8_t *w, size_t n, size_t pos,
                                int indent){
    uint8_t k;
    if (pos >= n) return n;
    k = w[pos++];
    switch (k){
        case DART_NAMED: {
            uint8_t nl;
            if (pos >= n) return n;
            nl = w[pos++];
            i_dart_out_raw(o, (const char *)(w + pos), nl);
            return i_dart_skip_type(w, n, pos + nl);
        }
        case DART_STR:
            i_dart_out_str(o, "string<");
            i_dart_out_i64(o, (int64_t)i_dart_le_r16(w + pos));
            i_dart_out_str(o, ">");
            return pos + 2u;
        case DART_VSTR: i_dart_out_str(o, "string"); return pos;
        case DART_MAP:  i_dart_out_str(o, "map");    return pos;
        case DART_ARR: {
            uint16_t cnt = i_dart_le_r16(w + pos);
            size_t after = i_dart_spell_type(o, w, n, pos + 2u, indent);
            i_dart_out_str(o, "[");
            i_dart_out_i64(o, (int64_t)cnt);
            i_dart_out_str(o, "]");
            return after;
        }
        case DART_VARR: {
            size_t after = i_dart_spell_type(o, w, n, pos, indent);
            i_dart_out_str(o, "[]");
            return after;
        }
        case DART_ENUM: {
            uint8_t backing; uint16_t cnt, i; uint32_t bs;
            if (pos + 3u > n) return n;
            backing = w[pos]; cnt = i_dart_le_r16(w + pos + 1); pos += 3u;
            bs = dart_schema_scalar_size((DartSchemaTypeKind)backing);
            i_dart_out_str(o, "enum<");
            i_dart_out_str(o, i_dart_why_kind(backing));
            i_dart_out_str(o, "> { ");
            for (i = 0; i < cnt; i++){
                uint8_t nl;
                if (pos + bs + 1u > n) return n;
                if (i) i_dart_out_str(o, ", ");
                nl = w[pos + bs];
                i_dart_out_raw(o, (const char *)(w + pos + bs + 1u), nl);
                i_dart_out_str(o, " = ");
                i_dart_out_i64(o, i_dart_enum_read_val(backing, w + pos));
                pos += bs + 1u + nl;
            }
            i_dart_out_str(o, " }");
            return pos;
        }
        case DART_STRUCT: {
            uint8_t nf; uint16_t i;
            if (pos >= n) return n;
            nf = w[pos++];
            i_dart_out_str(o, "{\n");
            for (i = 0; i < nf; i++){
                uint8_t nl;
                if (pos >= n) return n;
                nl = w[pos++];
                i_dart_out_indent(o, indent + 1);
                i_dart_out_raw(o, (const char *)(w + pos), nl);
                pos += nl;
                i_dart_out_str(o, ": ");
                pos = i_dart_spell_type(o, w, n, pos, indent + 1);
                i_dart_out_str(o, ",\n");
            }
            i_dart_out_indent(o, indent);
            i_dart_out_str(o, "}");
            return pos;
        }
        default:
            i_dart_out_str(o, i_dart_why_kind(k));
            return pos;
    }
}

/* The named types a schema uses, in dependency order and deduped by name, so each
 * prints one leading definition. */
#define I_DART_MAX_DEFS 48u
typedef struct {
    const uint8_t *w; size_t n;
    struct { uint32_t noff, toff; uint8_t nlen; } d[I_DART_MAX_DEFS];
    uint16_t count;
} i_DartDefScan;

static void i_dart_defscan_add(i_DartDefScan *L, uint32_t noff, uint8_t nlen, uint32_t toff){
    uint16_t i;
    for (i = 0; i < L->count; i++)
        if (L->d[i].nlen == nlen && memcmp(L->w + L->d[i].noff, L->w + noff, nlen) == 0) return;
    if (L->count >= I_DART_MAX_DEFS) return;   /* past the cap the tail spells by reference */
    L->d[L->count].noff = noff; L->d[L->count].nlen = nlen; L->d[L->count].toff = toff;
    L->count++;
}

static size_t i_dart_defscan_type(i_DartDefScan *L, size_t pos){
    uint8_t k;
    if (pos >= L->n) return L->n;
    k = L->w[pos];
    if (k == DART_NAMED){
        uint8_t nl; size_t ipos, after;
        if (pos + 2u > L->n) return L->n;
        nl = L->w[pos + 1];
        ipos = pos + 2u + nl;
        if (ipos > L->n) return L->n;
        after = i_dart_defscan_type(L, ipos);        /* dependencies first */
        i_dart_defscan_add(L, (uint32_t)(pos + 2u), nl, (uint32_t)ipos);
        return after;
    }
    pos++;
    switch (k){
        case DART_ARR:  return i_dart_defscan_type(L, pos + 2u <= L->n ? pos + 2u : L->n);
        case DART_VARR: return i_dart_defscan_type(L, pos);
        case DART_STRUCT: {
            uint8_t nf; uint16_t i;
            if (pos >= L->n) return L->n;
            nf = L->w[pos++];
            for (i = 0; i < nf; i++){
                if (pos >= L->n) return L->n;
                pos += 1u + L->w[pos];
                if (pos > L->n) return L->n;
                pos = i_dart_defscan_type(L, pos);
            }
            return pos;
        }
        default: return i_dart_skip_type(L->w, L->n, pos - 1u);
    }
}

uint32_t dart_schema_print(const DartSchema *s, char *buf, size_t cap){
    i_DartTextOut o;
    i_DartDefScan scan;
    size_t root_type;
    uint16_t i;
    o.p   = (buf && cap) ? buf : NULL;
    o.end = o.p ? buf + cap - 1 : NULL;   /* one byte reserved for the NUL */
    o.n   = 0;
    if (!s){ if (buf && cap) buf[0] = '\0'; return 0; }
    root_type = 2u + s->name.len;
    scan.w = s->wire.data; scan.n = s->wire.len; scan.count = 0;
    i_dart_defscan_type(&scan, root_type);
    for (i = 0; i < scan.count; i++){                    /* each named type, once, in order */
        i_dart_out_raw(&o, (const char *)(scan.w + scan.d[i].noff), scan.d[i].nlen);
        i_dart_out_str(&o, " = ");
        i_dart_spell_type(&o, scan.w, scan.n, scan.d[i].toff, 0);
        i_dart_out_str(&o, "\n");
    }
    if (s->value_root){                    /* a bare type, or Name = type for an alias */
        if (s->name.len){
            i_dart_out_view(&o, s->name);
            i_dart_out_str(&o, " = ");
        }
        i_dart_spell_type(&o, scan.w, scan.n, root_type, 0);
        i_dart_out_str(&o, "\n");
    } else {
        i_dart_out_view(&o, s->name);
        i_dart_out_str(&o, " ");
        i_dart_spell_type(&o, scan.w, scan.n, root_type, 0);
        i_dart_out_str(&o, "\n");
    }
    if (o.p) *o.p = '\0';                   /* o.p is at most end, the reserved byte */
    else if (buf && cap) buf[0] = '\0';
    return o.n;
}

/* reader and writer compatibility */
/* a top level field by name. A nested type is compared as one exact unit */
static const i_Field *i_dart_schema_find(const DartSchema *s, DartString name){
    uint16_t i;
    for (i = 0; i < s->nfields; i++){
        const i_Field *f = &s->fields[i];
        if (f->depth == 0 && f->name.len == name.len &&
            (name.len == 0 || memcmp(f->name.data, name.data, name.len) == 0)) return f;
    }
    return NULL;
}

/* consumes a NAMED wrapper if present, returning its name, else {NULL,0} */
static DartString i_dart_rd_type_name(i_Rd *r){
    DartString nm = dart_string(NULL, 0);
    if (r->pos < r->n && r->w[r->pos] == DART_NAMED){
        uint8_t nl;
        r->pos++;
        nl = i_dart_rd_u8(r);
        nm = dart_string((const char *)(r->w + r->pos), nl);
        i_dart_rd_skip(r, nl);
        if (r->fail) return dart_string(NULL, 0);
    }
    return nm;
}

static void i_dart_rd_skip_enum(i_Rd *r, uint8_t backing){
    uint16_t n = i_dart_rd_u16(r), i;
    uint32_t bs = dart_schema_scalar_size((DartSchemaTypeKind)backing);
    for (i = 0; i < n && !r->fail; i++){
        i_dart_rd_skip(r, bs);
        i_dart_rd_skip(r, i_dart_rd_u8(r));
    }
}

/* Can a reader declaring the type at ra read a writer's type at rb. Both advance past
 * their type. Names narrow, everything else compares exactly. See spec/schema.md. */
static int i_dart_type_cmp(i_Rd *ra, i_Rd *rb, uint16_t depth){
    DartString na, nb; uint8_t ka, kb;
    if (depth > DART_SCHEMA_MAX_DEPTH + 1u) return 0;
    na = i_dart_rd_type_name(ra);
    nb = i_dart_rd_type_name(rb);
    if (ra->fail || rb->fail) return 0;
    if (na.len && !dart_string_eq(na, nb)) return 0;
    ka = i_dart_rd_u8(ra); kb = i_dart_rd_u8(rb);
    if (ra->fail || rb->fail || ka != kb) return 0;
    switch (ka){
        case DART_STR: {
            uint16_t ca = i_dart_rd_u16(ra), cb = i_dart_rd_u16(rb);
            return !ra->fail && !rb->fail && ca == cb;
        }
        case DART_ARR: {
            uint16_t ca = i_dart_rd_u16(ra), cb = i_dart_rd_u16(rb);
            if (ra->fail || rb->fail || ca != cb) return 0;
            return i_dart_type_cmp(ra, rb, (uint16_t)(depth + 1));
        }
        case DART_VARR:
            return i_dart_type_cmp(ra, rb, (uint16_t)(depth + 1));
        case DART_VSTR: case DART_MAP:
            return 1;
        case DART_ENUM: {                        /* the backing width only, names are advisory */
            uint8_t ba = i_dart_rd_u8(ra), bb = i_dart_rd_u8(rb);
            i_dart_rd_skip_enum(ra, ba);
            i_dart_rd_skip_enum(rb, bb);
            return !ra->fail && !rb->fail && ba == bb;
        }
        case DART_STRUCT: {
            uint8_t nfa = i_dart_rd_u8(ra), nfb = i_dart_rd_u8(rb); uint16_t i;
            if (ra->fail || rb->fail || nfa != nfb) return 0;
            for (i = 0; i < nfa; i++){
                uint8_t la = i_dart_rd_u8(ra), lb = i_dart_rd_u8(rb);
                const char *pa = (const char *)(ra->w + ra->pos);
                const char *pb = (const char *)(rb->w + rb->pos);
                i_dart_rd_skip(ra, la); i_dart_rd_skip(rb, lb);
                if (ra->fail || rb->fail || la != lb) return 0;
                if (la && memcmp(pa, pb, la) != 0) return 0;
                if (!i_dart_type_cmp(ra, rb, (uint16_t)(depth + 1))) return 0;
            }
            return 1;
        }
        default:
            return dart_schema_scalar_size((DartSchemaTypeKind)ka) != 0;
    }
}

/* the two fields' types, compared from the wire */
static int i_dart_field_cmp(const DartSchema *sa, const i_Field *a,
                            const DartSchema *sb, const i_Field *b){
    i_Rd ra, rb;
    ra.w = sa->wire.data; ra.n = sa->wire.len; ra.pos = a->type_off; ra.fail = 0;
    rb.w = sb->wire.data; rb.n = sb->wire.len; rb.pos = b->type_off; rb.fail = 0;
    return i_dart_type_cmp(&ra, &rb, 0);
}

/* bounded appenders for the subset why text. No stdio, and a NULL buffer skips all text */
static char *i_dart_why_str(char *p, char *end, const char *s){
    if (!p) return NULL;
    while (*s && p < end) *p++ = *s++;
    return p;
}
static char *i_dart_why_view(char *p, char *end, DartString s){
    size_t i;
    if (!p) return NULL;
    for (i = 0; i < s.len && p < end; i++) *p++ = s.data[i];
    return p;
}
static char *i_dart_why_u(char *p, char *end, uint32_t v){
    char tmp[10]; int n = 0;
    if (!p) return NULL;
    do { tmp[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n && p < end) *p++ = tmp[--n];
    return p;
}
/* a field's type in compact DSL form: "Float3[4]", "f32[8]", "string<33>", "map" */
static char *i_dart_why_type(char *p, char *end, const i_Field *f){
    uint8_t is_arr = (uint8_t)(f->kind == DART_ARR || f->kind == DART_VARR);
    if (f->type_name.len) return i_dart_why_view(p, end, f->type_name);
    if (f->kind == DART_MAP)  return i_dart_why_str(p, end, "map");
    if (f->kind == DART_VSTR) return i_dart_why_str(p, end, "string");
    if (f->kind == DART_ENUM){                            /* the width is what matters */
        p = i_dart_why_str(p, end, "enum<");
        p = i_dart_why_str(p, end, i_dart_why_kind(f->elem));
        return i_dart_why_str(p, end, ">");
    }
    if (is_arr && f->elem_name.len){
        p = i_dart_why_view(p, end, f->elem_name);
    } else {
        uint8_t elem = is_arr ? f->elem : f->kind;
        if (elem == DART_STR){
            p = i_dart_why_str(p, end, "string<");
            p = i_dart_why_u(p, end, f->str_cap);
            p = i_dart_why_str(p, end, ">");
        } else {
            p = i_dart_why_str(p, end, i_dart_why_kind(elem));
        }
    }
    if (f->kind == DART_ARR){
        p = i_dart_why_str(p, end, "[");
        p = i_dart_why_u(p, end, f->count);
        p = i_dart_why_str(p, end, "]");
    } else if (f->kind == DART_VARR){
        p = i_dart_why_str(p, end, "[]");
    }
    return p;
}
static int i_dart_why_done(char *buf, char *p){   /* NUL terminate the reason and refuse */
    if (buf) *p = '\0';
    return 0;
}
/* a schema's root in DSL words: a bare type's spelling, or struct 'Name' */
static char *i_dart_why_root(char *p, char *end, const DartSchema *s){
    if (s->value_root){
        if (s->name.len){
            p = i_dart_why_view(p, end, s->name);
            p = i_dart_why_str(p, end, " = ");
        }
        return i_dart_why_type(p, end, &s->fields[0]);
    }
    p = i_dart_why_str(p, end, "struct '");
    p = i_dart_why_view(p, end, s->name);
    return i_dart_why_str(p, end, "'");
}

int dart_schema_subset_why(const DartSchema *sub, const DartSchema *pub,
                           char *buf, size_t cap){
    uint16_t i;
    char *p = (buf && cap) ? buf : NULL, *end = p ? buf + cap - 1 : NULL;
    if (p) *p = '\0';
    if (!sub || !pub)
        return i_dart_why_done(p, i_dart_why_str(p, end, "schema missing"));
    if (sub->value_root || pub->value_root){        /* a bare root: the two roots are the types */
        int named_ok = !sub->name.len || dart_string_eq(sub->name, pub->name);
        if (sub->value_root == pub->value_root && named_ok &&
            i_dart_field_cmp(sub, &sub->fields[0], pub, &pub->fields[0])) return 1;
        p = i_dart_why_str(p, end, "root: reader ");
        p = i_dart_why_root(p, end, sub);
        p = i_dart_why_str(p, end, ", writer ");
        return i_dart_why_done(buf, i_dart_why_root(p, end, pub));
    }
    if (sub->name.len != pub->name.len ||
        (sub->name.len && memcmp(sub->name.data, pub->name.data, sub->name.len) != 0)){
        p = i_dart_why_str(p, end, "reader type '"); p = i_dart_why_view(p, end, sub->name);
        p = i_dart_why_str(p, end, "' != writer type '"); p = i_dart_why_view(p, end, pub->name);
        return i_dart_why_done(buf, i_dart_why_str(p, end, "'"));
    }
    for (i = 0; i < sub->nfields; i++){
        const i_Field *a = &sub->fields[i], *b;
        if (a->depth != 0) continue;                          /* members ride their struct */
        b = i_dart_schema_find(pub, a->name);
        if (!b){
            p = i_dart_why_str(p, end, "field '"); p = i_dart_why_view(p, end, a->name);
            return i_dart_why_done(buf, i_dart_why_str(p, end, "' missing from writer"));
        }
        if (!i_dart_field_cmp(sub, a, pub, b)){
            p = i_dart_why_str(p, end, "field '"); p = i_dart_why_view(p, end, a->name);
            p = i_dart_why_str(p, end, "': reader "); p = i_dart_why_type(p, end, a);
            p = i_dart_why_str(p, end, ", writer ");
            return i_dart_why_done(buf, i_dart_why_type(p, end, b));
        }
    }
    return 1;
}

int dart_schema_subset(const DartSchema *sub, const DartSchema *pub){
    return dart_schema_subset_why(sub, pub, NULL, 0);
}

/* the flat index just past a field's subtree */
static uint16_t i_dart_subtree_end(const DartSchema *s, uint16_t i){
    uint16_t d = s->fields[i].depth, k = (uint16_t)(i + 1u);
    while (k < s->nfields && s->fields[k].depth > d) k++;
    return k;
}

DartSchema *dart_schema_rebase(const DartSchema *sub, const DartSchema *pub,
                               DartAllocFn alloc, void *user){
    DartSchema *r; uint16_t i = 0;
    if (!alloc || !dart_schema_subset(sub, pub)) return NULL;
    r = dart_schema_parse(sub->wire.data, sub->wire.len, alloc, user);
    if (!r) return NULL;
    while (i < r->nfields){                                   /* the writer's layout */
        const i_Field *p = i_dart_schema_find(pub, r->fields[i].name);
        uint16_t re = i_dart_subtree_end(r, i), j, k;
        if (!p || r->fields[i].depth != 0){ dart_schema_free(r, alloc, user); return NULL; }
        j = (uint16_t)(p - pub->fields);
        for (k = 0; (uint16_t)(i + k) < re; k++){             /* the subtrees match exactly */
            i_Field *rf = &r->fields[i + k];
            const i_Field *pf;
            if ((uint16_t)(j + k) >= pub->nfields){ dart_schema_free(r, alloc, user); return NULL; }
            pf = &pub->fields[j + k];
            rf->offset = pf->offset;
            rf->var_ord = pf->var_ord;
        }
        i = re;
    }
    r->size = pub->size;                                      /* and the writer's bounds, so */
    r->n_var = pub->n_var;   /* validate and walk see pub's messages */
    return r;
}
/* the variable tail */
uint32_t dart_schema_msg_min(const DartSchema *s){
    return s ? s->size + 4u * s->n_var : 0;
}

uint32_t dart_schema_msg_len(const DartSchema *s, const void *buf, size_t cap){
    const uint8_t *p = (const uint8_t *)buf;
    uint64_t total; uint16_t i;
    if (!s || !p) return 0;
    total = s->size;
    for (i = 0; i < s->n_var; i++){                     /* hop the frames, bounds checked */
        if (total + 4u > cap) return 0;
        total += 4u + (uint64_t)i_dart_le_r32(p + total);
        if (total > cap) return 0;
    }
    return total <= 0xFFFFFFFFu ? (uint32_t)total : 0;
}

/* the payload view of variable field ordinal ord, {NULL,0} on any bound break */
static DartBytes i_dart_schema_frame(const DartSchema *s, DartBytes msg, uint16_t ord){
    uint64_t pos = s->size; uint32_t flen; uint16_t i;
    for (i = 0; i <= ord && i < s->n_var; i++){
        if (pos + 4u > msg.len) break;
        flen = i_dart_le_r32(msg.data + pos);
        if (pos + 4u + flen > msg.len) break;
        if (i == ord) return dart_bytes(msg.data + pos + 4u, flen);
        pos += 4u + (uint64_t)flen;
    }
    return dart_bytes(NULL, 0);
}

/* Resizes variable field ord's frame to new_len, moving the rest of the tail. src NULL
 * keeps the existing prefix and zeroes any growth. src must not alias buf. */
static int i_dart_schema_frame_write(const DartSchema *s, uint8_t *buf, size_t cap,
                                     uint16_t ord, const void *src, size_t new_len){
    uint64_t pos = s->size, total; uint32_t old; uint16_t i;
    if (new_len && !src && src != NULL) return 0;
    if (new_len > 0xFFFFFFFFu - 4u) return 0;
    total = dart_schema_msg_len(s, buf, cap);
    if (total == 0) return 0;                           /* a malformed or uninitialized buffer */
    for (i = 0; i < ord; i++) pos += 4u + (uint64_t)i_dart_le_r32(buf + pos);
    old = i_dart_le_r32(buf + pos);                     /* bounds proven by msg_len's walk */
    if (total - old + new_len > cap) return 0;          /* never silently truncate */
    memmove(buf + pos + 4u + new_len, buf + pos + 4u + old,
            (size_t)(total - (pos + 4u + old)));
    i_dart_le_w32(buf + pos, (uint32_t)new_len);
    if (src){ if (new_len) memcpy(buf + pos + 4u, src, new_len); }
    else if (new_len > old) memset(buf + pos + 4u + old, 0, new_len - old);
    return 1;
}
static int i_dart_schema_set_frame(const DartSchema *s, uint8_t *buf, size_t cap,
                                   uint16_t ord, const void *src, size_t src_len){
    if (src_len && !src) return 0;
    return i_dart_schema_frame_write(s, buf, cap, ord, src_len ? src : (const void *)"", src_len);
}

/* reading a message */
int dart_schema_validate(const DartSchema *s, DartBytes msg){
    uint32_t total;
    if (!s) return 0;
    if (s->n_var == 0) return msg.len == s->size;
    total = dart_schema_msg_len(s, msg.data, msg.len);  /* the frames must consume it exactly */
    return total != 0 && total == msg.len;
}

/* Where field f's bytes sit in msg for array element index, bounds checked. A fixed
 * struct array strides from element 0, a variable one strides inside the array's frame. */
static int i_dart_field_addr(const DartSchema *s, DartBytes msg, const i_Field *f,
                             uint32_t index, size_t *out_off){
    size_t off;
    if (i_dart_kind_var(f->kind)){ *out_off = 0; return 1; }   /* frames locate themselves */
    if (f->arr_parent == I_DART_NO_PARENT){
        off = f->offset;
    } else {
        const i_Field *a = &s->fields[f->arr_parent];
        if (a->elem_size == 0) return 0;
        if (a->kind == DART_ARR){
            if (index >= a->count) return 0;
            off = (size_t)f->offset + (size_t)index * a->elem_size;
        } else {
            DartBytes fr = i_dart_schema_frame(s, msg, a->var_ord);
            if (!fr.data || (uint64_t)index * a->elem_size + a->elem_size > fr.len) return 0;
            off = (size_t)(fr.data - msg.data) + (size_t)index * a->elem_size + f->offset;
        }
    }
    if (off + f->size > msg.len) return 0;
    *out_off = off;
    return 1;
}

/* Resolves a possibly indexed path against a message. NULL if unknown, out of range or
 * the message is too short. */
static const i_Field *i_dart_schema_read_lookup(const DartSchema *s, DartBytes msg,
                                                const char *field, size_t *off){
    uint32_t index = 0;
    const i_Field *f = i_dart_schema_field_by_path(s, field, &index);
    if (!f || !i_dart_field_addr(s, msg, f, index, off)) return NULL;
    return f;
}

static uint64_t i_dart_schema_read_uint(uint8_t kind, const uint8_t *p){
    switch (kind){
        case DART_U8: case DART_BOOL: return p[0];
        case DART_U16: return i_dart_le_r16(p);
        case DART_U32: return i_dart_le_r32(p);
        case DART_U64: return i_dart_le_r64(p);
        default: return 0;
    }
}
static int64_t i_dart_schema_read_int(uint8_t kind, const uint8_t *p){
    switch (kind){
        case DART_I8:  return (int8_t)p[0];
        case DART_I16: return (int16_t)i_dart_le_r16(p);
        case DART_I32: return (int32_t)i_dart_le_r32(p);
        case DART_I64: return (int64_t)i_dart_le_r64(p);
        default: return 0;
    }
}
static double i_dart_schema_read_f64(uint8_t kind, const uint8_t *p){
    if (kind == DART_F64){ uint64_t b = i_dart_le_r64(p); double d; memcpy(&d, &b, 8); return d; }
    if (kind == DART_F32){ uint32_t b = i_dart_le_r32(p); float  x; memcpy(&x, &b, 4); return (double)x; }
    return 0.0;
}

uint64_t dart_get_uint(DartBytes msg, const DartSchema *s, const char *field){
    size_t off; const i_Field *f = i_dart_schema_read_lookup(s, msg, field, &off);
    if (!f) return 0;
    if (f->kind == DART_ENUM) return (uint64_t)i_dart_enum_read_val(f->elem, msg.data + off);
    return i_dart_schema_read_uint(f->kind, msg.data + off);
}

int64_t dart_get_int(DartBytes msg, const DartSchema *s, const char *field){
    size_t off; const i_Field *f = i_dart_schema_read_lookup(s, msg, field, &off);
    if (!f) return 0;
    if (f->kind == DART_ENUM) return i_dart_enum_read_val(f->elem, msg.data + off);
    return i_dart_schema_read_int(f->kind, msg.data + off);
}

double dart_get_f64(DartBytes msg, const DartSchema *s, const char *field){
    size_t off; const i_Field *f = i_dart_schema_read_lookup(s, msg, field, &off);
    return f ? i_dart_schema_read_f64(f->kind, msg.data + off) : 0.0;
}

float dart_get_f32(DartBytes msg, const DartSchema *s, const char *field){
    size_t off; const i_Field *f = i_dart_schema_read_lookup(s, msg, field, &off);
    if (!f || f->kind != DART_F32) return 0.0f;
    { uint32_t b = i_dart_le_r32(msg.data + off); float x; memcpy(&x, &b, 4); return x; }
}

DartBytes dart_get_array(DartBytes msg, const DartSchema *s, const char *field){
    DartBytes out; size_t off; const i_Field *f = i_dart_schema_read_lookup(s, msg, field, &off);
    out.data = NULL; out.len = 0;
    if (!f) return out;
    if (f->kind == DART_VARR){
        DartBytes fr = i_dart_schema_frame(s, msg, f->var_ord);
        if (!fr.data || !f->elem_size) return out;
        out.data = fr.data; out.len = fr.len - fr.len % f->elem_size;   /* whole elements only */
        return out;
    }
    if (f->kind != DART_ARR) return out;
    out.data = msg.data + off; out.len = f->size;
    return out;
}

/* the live element count of an array field */
static uint32_t i_dart_array_count(const DartSchema *s, DartBytes msg, const i_Field *f){
    if (!f || !f->elem_size) return 0;
    if (f->kind == DART_ARR) return f->count;
    if (f->kind == DART_VARR){
        DartBytes fr = i_dart_schema_frame(s, msg, f->var_ord);
        return fr.data ? (uint32_t)(fr.len / f->elem_size) : 0;
    }
    return 0;
}

uint32_t dart_get_array_count(DartBytes msg, const DartSchema *s, const char *field){
    size_t off; const i_Field *f = i_dart_schema_read_lookup(s, msg, field, &off);
    return i_dart_array_count(s, msg, f);
}

uint32_t dart_array_count_at(DartBytes msg, const DartSchema *s, uint16_t field){
    if (!s || field >= s->nfields) return 0;
    return i_dart_array_count(s, msg, &s->fields[field]);
}

/* one slot's live string, the length clamped to the cap against a hostile message */
static DartString i_dart_schema_str_view(const uint8_t *slot, uint16_t cap){
    uint16_t len = i_dart_le_r16(slot);
    if (len > cap) len = cap;
    return dart_string((const char *)(slot + 2), len);
}

DartString dart_get_string(DartBytes msg, const DartSchema *s, const char *field){
    size_t off; const i_Field *f = i_dart_schema_read_lookup(s, msg, field, &off);
    if (!f) return dart_string(NULL, 0);
    if (f->kind == DART_VSTR){
        DartBytes fr = i_dart_schema_frame(s, msg, f->var_ord);   /* the frame is the string */
        return dart_string((const char *)fr.data, fr.data ? fr.len : 0);
    }
    if (f->kind != DART_STR) return dart_string(NULL, 0);
    return i_dart_schema_str_view(msg.data + off, f->str_cap);
}

DartString dart_get_string_at(DartBytes msg, const DartSchema *s, const char *field,
                              uint16_t index){
    size_t off; const i_Field *f = i_dart_schema_read_lookup(s, msg, field, &off);
    if (!f || f->elem != DART_STR) return dart_string(NULL, 0);
    if (f->kind == DART_VARR){
        DartBytes fr = i_dart_schema_frame(s, msg, f->var_ord);
        uint32_t esz = f->elem_size;
        if (!fr.data || !esz || ((uint64_t)index + 1u) * esz > fr.len) return dart_string(NULL, 0);
        return i_dart_schema_str_view(fr.data + (size_t)index * esz, f->str_cap);
    }
    if (f->kind != DART_ARR || index >= f->count) return dart_string(NULL, 0);
    return i_dart_schema_str_view(msg.data + off + (size_t)index * f->elem_size, f->str_cap);
}

DartBytes dart_get_map(DartBytes msg, const DartSchema *s, const char *field){
    size_t off; const i_Field *f = i_dart_schema_read_lookup(s, msg, field, &off);
    if (!f || f->kind != DART_MAP) return dart_bytes(NULL, 0);
    return i_dart_schema_frame(s, msg, f->var_ord);
}

DartString dart_get_enum(DartBytes msg, const DartSchema *s, const char *field){
    size_t off; const i_Field *f = i_dart_schema_read_lookup(s, msg, field, &off);
    if (!f || f->kind != DART_ENUM) return dart_string(NULL, 0);
    return dart_enum_name_of(s, (uint16_t)(f - s->fields),
                             i_dart_enum_read_val(f->elem, msg.data + off));
}

int dart_get_value_at(DartBytes msg, const DartSchema *s, uint16_t field, uint32_t elem,
                      DartValue *out){
    const i_Field *f; const uint8_t *p; size_t off;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!s || field >= s->nfields) return 0;
    f = &s->fields[field];
    if (!i_dart_field_addr(s, msg, f, elem, &off)) return 0;
    p = msg.data + off;
    out->kind = f->kind; out->elem = f->elem; out->count = f->count; out->str_cap = f->str_cap;
    switch (f->kind){
        case DART_U8: case DART_U16: case DART_U32: case DART_U64: case DART_BOOL:
            out->v.u = i_dart_schema_read_uint(f->kind, p); break;
        case DART_I8: case DART_I16: case DART_I32: case DART_I64:
            out->v.i = i_dart_schema_read_int(f->kind, p); break;
        case DART_F32: case DART_F64:
            out->v.f = i_dart_schema_read_f64(f->kind, p); break;
        case DART_ARR: case DART_STRUCT:
            out->bytes = dart_bytes(p, f->size); break;
        case DART_STR: {
            DartString sv = i_dart_schema_str_view(p, f->str_cap);
            out->bytes = dart_bytes(sv.data, sv.len); break;
        }
        case DART_VSTR: {
            DartBytes fr = i_dart_schema_frame(s, msg, f->var_ord);
            if (!fr.data) return 0;
            out->bytes = fr; break;
        }
        case DART_VARR: {
            DartBytes fr = i_dart_schema_frame(s, msg, f->var_ord);
            uint64_t n;
            if (!fr.data || !f->elem_size) return 0;
            n = fr.len / f->elem_size;
            out->bytes = dart_bytes(fr.data, (size_t)(n * f->elem_size));
            out->count = n > 0xFFFFu ? 0xFFFFu : (uint16_t)n;   /* saturated, bytes.len rules */
            break;
        }
        case DART_MAP: {
            DartBytes fr = i_dart_schema_frame(s, msg, f->var_ord);
            if (!fr.data) return 0;
            out->bytes = fr;
            out->count = dart_map_count(fr); break;
        }
        case DART_ENUM:   /* v.i is the value, elem and count carry the backing and options */
            out->v.i = i_dart_enum_read_val(f->elem, p); break;
        default: return 0;
    }
    return 1;
}

int dart_get_value(DartBytes msg, const DartSchema *s, uint16_t field, DartValue *out){
    return dart_get_value_at(msg, s, field, 0, out);
}

/* writing a message */
int dart_schema_message_default(const DartSchema *s, void *buf, size_t cap){
    size_t min;
    if (!s || !buf) return 0;
    min = (size_t)s->size + 4u * s->n_var;
    if (cap < min) return 0;
    memset(buf, 0, min);            /* zeroed fixed fields plus one empty frame per variable */
    return 1;
}

static const i_Field *i_dart_schema_set_lookup(const DartSchema *s, const void *buf, size_t cap,
                                               const char *field, size_t *off){
    if (!buf) return NULL;
    return i_dart_schema_read_lookup(s, dart_bytes(buf, cap), field, off);
}

static int i_dart_schema_write_uint(const i_Field *f, uint8_t *p, uint64_t v){
    switch (f->kind){
        case DART_U8:   p[0] = (uint8_t)v;  return 1;
        case DART_BOOL: p[0] = v ? 1u : 0u; return 1;
        case DART_U16: i_dart_le_w16(p, (uint16_t)v); return 1;
        case DART_U32: i_dart_le_w32(p, (uint32_t)v); return 1;
        case DART_U64: i_dart_le_w64(p, v); return 1;
        default: return 0;
    }
}
static int i_dart_schema_write_int(const i_Field *f, uint8_t *p, int64_t v){
    switch (f->kind){
        case DART_I8:  p[0] = (uint8_t)v; return 1;
        case DART_I16: i_dart_le_w16(p, (uint16_t)v); return 1;
        case DART_I32: i_dart_le_w32(p, (uint32_t)v); return 1;
        case DART_I64: i_dart_le_w64(p, (uint64_t)v); return 1;
        default: return 0;
    }
}
static int i_dart_schema_write_f64(const i_Field *f, uint8_t *p, double v){
    if (f->kind == DART_F64){ uint64_t b; memcpy(&b, &v, 8); i_dart_le_w64(p, b); return 1; }
    if (f->kind == DART_F32){ float x = (float)v; uint32_t b; memcpy(&b, &x, 4); i_dart_le_w32(p, b); return 1; }
    return 0;
}
/* one string slot: [u16 len][bytes][zeroed tail]. Refuses v.len over the cap */
static int i_dart_schema_write_str_slot(uint8_t *p, uint16_t cap, DartString v){
    if (v.len > cap || (v.len && !v.data)) return 0;             /* never silently truncate */
    i_dart_le_w16(p, (uint16_t)v.len);
    if (v.len) memcpy(p + 2, v.data, v.len);
    memset(p + 2 + v.len, 0, (size_t)cap - v.len);
    return 1;
}
/* element payload sanity shared by fixed and variable arrays: whole elements, and every
 * string slot's length prefix within its cap */
static int i_dart_schema_check_elems(const i_Field *f, DartBytes elems, uint32_t esz){
    if (!esz || elems.len % esz != 0) return 0;     /* never silently truncate */
    if (elems.len && !elems.data) return 0;
    if (f->elem == DART_STR){
        size_t off;
        for (off = 0; off + esz <= elems.len; off += esz)
            if (i_dart_le_r16(elems.data + off) > f->str_cap) return 0;
    }
    return 1;
}
/* copy elems over the front of a fixed array and zero the rest */
static int i_dart_schema_write_array(const i_Field *f, uint8_t *p, DartBytes elems){
    if (f->kind != DART_ARR) return 0;
    if (elems.len > f->size || !i_dart_schema_check_elems(f, elems, f->elem_size)) return 0;
    if (elems.len) memcpy(p, elems.data, elems.len);
    memset(p + elems.len, 0, f->size - elems.len);
    return 1;
}
/* a variable array's frame becomes exactly elems */
static int i_dart_schema_write_var_array(const DartSchema *s, uint8_t *buf, size_t cap,
                                         const i_Field *f, DartBytes elems){
    if (!i_dart_schema_check_elems(f, elems, f->elem_size)) return 0;
    return i_dart_schema_set_frame(s, buf, cap, f->var_ord, elems.data, elems.len);
}

int dart_set_uint(void *buf, size_t cap, const DartSchema *s, const char *field, uint64_t v){
    size_t off; const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field, &off);
    if (!f) return 0;
    if (f->kind == DART_ENUM){ i_dart_enum_write_val(f->elem, (uint8_t *)buf + off, (int64_t)v); return 1; }
    return i_dart_schema_write_uint(f, (uint8_t *)buf + off, v);
}

int dart_set_int(void *buf, size_t cap, const DartSchema *s, const char *field, int64_t v){
    size_t off; const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field, &off);
    if (!f) return 0;
    if (f->kind == DART_ENUM){ i_dart_enum_write_val(f->elem, (uint8_t *)buf + off, v); return 1; }
    return i_dart_schema_write_int(f, (uint8_t *)buf + off, v);
}

int dart_set_f64(void *buf, size_t cap, const DartSchema *s, const char *field, double v){
    size_t off; const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field, &off);
    return f ? i_dart_schema_write_f64(f, (uint8_t *)buf + off, v) : 0;
}

int dart_set_f32(void *buf, size_t cap, const DartSchema *s, const char *field, float v){
    size_t off; const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field, &off); uint32_t b;
    if (!f || f->kind != DART_F32) return 0;
    memcpy(&b, &v, 4); i_dart_le_w32((uint8_t *)buf + off, b);
    return 1;
}

int dart_set_array(void *buf, size_t cap, const DartSchema *s, const char *field, DartBytes elems){
    size_t off; const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field, &off);
    if (!f) return 0;
    if (f->kind == DART_VARR)
        return i_dart_schema_write_var_array(s, (uint8_t *)buf, cap, f, elems);
    return i_dart_schema_write_array(f, (uint8_t *)buf + off, elems);
}

int dart_set_array_count(void *buf, size_t cap, const DartSchema *s, const char *field,
                         uint32_t count){
    size_t off; const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field, &off);
    if (!f || f->kind != DART_VARR || !f->elem_size) return 0;
    if ((uint64_t)count * f->elem_size > 0xFFFFFFFFu) return 0;
    return i_dart_schema_frame_write(s, (uint8_t *)buf, cap, f->var_ord, NULL,
                                     (size_t)count * f->elem_size);
}

int dart_set_string(void *buf, size_t cap, const DartSchema *s, const char *field, DartString v){
    size_t off; const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field, &off);
    if (!f) return 0;
    if (f->kind == DART_VSTR){
        if (v.len && !v.data) return 0;
        return i_dart_schema_set_frame(s, (uint8_t *)buf, cap, f->var_ord, v.data, v.len);
    }
    if (f->kind != DART_STR) return 0;
    return i_dart_schema_write_str_slot((uint8_t *)buf + off, f->str_cap, v);
}

int dart_set_string_at(void *buf, size_t cap, const DartSchema *s, const char *field,
                       uint16_t index, DartString v){
    size_t off; const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field, &off);
    if (!f || f->elem != DART_STR) return 0;
    if (f->kind == DART_VARR){                      /* in place, under the live count */
        DartBytes fr = i_dart_schema_frame(s, dart_bytes(buf, cap), f->var_ord);
        uint32_t esz = f->elem_size;
        if (!fr.data || !esz || ((uint64_t)index + 1u) * esz > fr.len) return 0;
        return i_dart_schema_write_str_slot((uint8_t *)fr.data + (size_t)index * esz,
                                            f->str_cap, v);
    }
    if (f->kind != DART_ARR || index >= f->count) return 0;
    return i_dart_schema_write_str_slot((uint8_t *)buf + off + (size_t)index * f->elem_size,
                                        f->str_cap, v);
}

int dart_set_map(void *buf, size_t cap, const DartSchema *s, const char *field, DartBytes map){
    size_t off; const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field, &off);
    if (!f || f->kind != DART_MAP) return 0;
    if (!dart_map_valid(map)) return 0;             /* malformed bytes never enter a message */
    return i_dart_schema_set_frame(s, (uint8_t *)buf, cap, f->var_ord, map.data, map.len);
}

int dart_set_enum(void *buf, size_t cap, const DartSchema *s, const char *field, const char *name){
    size_t off; const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field, &off);
    int64_t v;
    if (!f || f->kind != DART_ENUM) return 0;
    if (!dart_enum_value_of(s, (uint16_t)(f - s->fields), name, &v)) return 0;
    i_dart_enum_write_val(f->elem, (uint8_t *)buf + off, v);
    return 1;
}

int dart_set_value_at(void *buf, size_t cap, const DartSchema *s, uint16_t field, uint32_t elem,
                      const DartValue *val){
    const i_Field *f; uint8_t *p; size_t off;
    if (!buf || !val || !s || field >= s->nfields) return 0;
    f = &s->fields[field];
    if (!i_dart_field_addr(s, dart_bytes(buf, cap), f, elem, &off)) return 0;
    p = (uint8_t *)buf + off;
    switch (f->kind){
        case DART_U8: case DART_U16: case DART_U32: case DART_U64: case DART_BOOL:
            return i_dart_schema_write_uint(f, p, val->v.u);
        case DART_I8: case DART_I16: case DART_I32: case DART_I64:
            return i_dart_schema_write_int(f, p, val->v.i);
        case DART_F32: case DART_F64:
            return i_dart_schema_write_f64(f, p, val->v.f);
        case DART_ARR:
            return i_dart_schema_write_array(f, p, val->bytes);
        case DART_STR:
            return i_dart_schema_write_str_slot(p, f->str_cap,
                       dart_string((const char *)val->bytes.data, val->bytes.len));
        case DART_STRUCT:   /* raw bytes, a short source zero fills the tail */
            if (val->bytes.len > f->size || (val->bytes.len && !val->bytes.data)) return 0;
            if (val->bytes.len) memcpy(p, val->bytes.data, val->bytes.len);
            memset(p + val->bytes.len, 0, f->size - val->bytes.len);
            return 1;
        case DART_VSTR:
            if (val->bytes.len && !val->bytes.data) return 0;
            return i_dart_schema_set_frame(s, (uint8_t *)buf, cap, f->var_ord,
                                           val->bytes.data, val->bytes.len);
        case DART_VARR:
            return i_dart_schema_write_var_array(s, (uint8_t *)buf, cap, f, val->bytes);
        case DART_MAP:
            if (!dart_map_valid(val->bytes)) return 0;
            return i_dart_schema_set_frame(s, (uint8_t *)buf, cap, f->var_ord,
                                           val->bytes.data, val->bytes.len);
        case DART_ENUM:
            i_dart_enum_write_val(f->elem, p, val->v.i);
            return 1;
        default: return 0;
    }
}

int dart_set_value(void *buf, size_t cap, const DartSchema *s, uint16_t field, const DartValue *val){
    return dart_set_value_at(buf, cap, s, field, 0, val);
}
/* The map. Readers walk hostile bytes: every read is bounds checked, the kind vocabulary
 * is closed and nesting is depth capped. The body grammar is in spec/schema.md. */

/* Reads (out set) or skips (out NULL) one value at r->pos. 1, or 0 with r->fail. */
static int i_dart_map_value(i_Rd *r, uint16_t depth, DartValue *out){
    uint8_t k = i_dart_rd_u8(r);
    uint32_t sz;
    if (r->fail) return 0;
    if (out){ memset(out, 0, sizeof *out); out->kind = k; }
    sz = dart_schema_scalar_size((DartSchemaTypeKind)k);
    if (sz){
        const uint8_t *p = r->w + r->pos;
        i_dart_rd_skip(r, sz);
        if (r->fail) return 0;
        if (out) switch (k){
            case DART_U8: case DART_U16: case DART_U32: case DART_U64: case DART_BOOL:
                out->v.u = i_dart_schema_read_uint(k, p); break;
            case DART_I8: case DART_I16: case DART_I32: case DART_I64:
                out->v.i = i_dart_schema_read_int(k, p); break;
            default:
                out->v.f = i_dart_schema_read_f64(k, p); break;
        }
        return 1;
    }
    if (k == DART_VSTR){
        uint16_t len = i_dart_rd_u16(r);
        const uint8_t *p = r->w + r->pos;
        i_dart_rd_skip(r, len);
        if (r->fail) return 0;
        if (out) out->bytes = dart_bytes(p, len);
        return 1;
    }
    if (k == DART_MAP || k == DART_VARR){
        size_t start = r->pos; uint16_t n, i;
        if (depth + 1u >= DART_SCHEMA_MAX_DEPTH){ r->fail = 1; return 0; }
        n = i_dart_rd_u16(r);
        for (i = 0; i < n && !r->fail; i++){
            if (k == DART_MAP){
                uint8_t kl = i_dart_rd_u8(r);
                i_dart_rd_skip(r, kl);
            }
            if (!i_dart_map_value(r, (uint16_t)(depth + 1), NULL)) return 0;
        }
        if (r->fail) return 0;
        if (out){ out->bytes = dart_bytes(r->w + start, r->pos - start); out->count = n; }
        return 1;
    }
    r->fail = 1;                                        /* unknown kind: reject */
    return 0;
}

uint16_t dart_map_count(DartBytes map){
    return (map.data && map.len >= 2) ? i_dart_le_r16(map.data) : 0;
}
uint16_t dart_map_array_count(DartBytes arr){ return dart_map_count(arr); }

int dart_map_at(DartBytes map, uint16_t index, DartString *key, DartValue *out){
    i_Rd r; uint16_t n, i;
    if (!map.data || map.len < 2) return 0;
    r.w = map.data; r.n = map.len; r.pos = 0; r.fail = 0;
    n = i_dart_rd_u16(&r);
    if (index >= n) return 0;
    for (i = 0; i <= index; i++){
        uint8_t kl = i_dart_rd_u8(&r);
        const char *kp = (const char *)(r.w + r.pos);
        i_dart_rd_skip(&r, kl);
        if (r.fail) return 0;
        if (i == index){
            if (!i_dart_map_value(&r, 0, out)) return 0;
            if (key) *key = dart_string(kp, kl);
            return 1;
        }
        if (!i_dart_map_value(&r, 0, NULL)) return 0;
    }
    return 0;
}

int dart_map_get(DartBytes map, const char *key, DartValue *out){
    i_Rd r; uint16_t n, i; size_t want;
    if (!map.data || map.len < 2 || !key) return 0;
    want = strlen(key);
    r.w = map.data; r.n = map.len; r.pos = 0; r.fail = 0;
    n = i_dart_rd_u16(&r);
    for (i = 0; i < n; i++){
        uint8_t kl = i_dart_rd_u8(&r);
        const char *kp = (const char *)(r.w + r.pos);
        i_dart_rd_skip(&r, kl);
        if (r.fail) return 0;
        if (kl == want && (want == 0 || memcmp(kp, key, want) == 0))
            return i_dart_map_value(&r, 0, out);
        if (!i_dart_map_value(&r, 0, NULL)) return 0;
    }
    return 0;
}

int dart_map_array_at(DartBytes arr, uint16_t index, DartValue *out){
    i_Rd r; uint16_t n, i;
    if (!arr.data || arr.len < 2) return 0;
    r.w = arr.data; r.n = arr.len; r.pos = 0; r.fail = 0;
    n = i_dart_rd_u16(&r);
    if (index >= n) return 0;
    for (i = 0; i < index; i++)
        if (!i_dart_map_value(&r, 0, NULL)) return 0;
    return i_dart_map_value(&r, 0, out);
}

int dart_map_valid(DartBytes map){
    i_Rd r; uint16_t n, i;
    if (map.len == 0) return 1;                         /* an empty body is an empty map */
    if (!map.data || map.len < 2) return 0;
    r.w = map.data; r.n = map.len; r.pos = 0; r.fail = 0;
    n = i_dart_rd_u16(&r);
    for (i = 0; i < n; i++){
        uint8_t kl = i_dart_rd_u8(&r);
        i_dart_rd_skip(&r, kl);
        if (r.fail || !i_dart_map_value(&r, 0, NULL)) return 0;
    }
    return r.pos == r.n;                                /* no trailing garbage */
}

/* the map writer */
DartMapWriter dart_map_begin(void *buf, size_t cap){
    DartMapWriter w;
    memset(&w, 0, sizeof w);
    w.buf = (uint8_t *)buf; w.cap = cap;
    if (!buf || cap < 2){ w.err = -1; return w; }
    w.buf[0] = 0; w.buf[1] = 0;                         /* the root map's count */
    w.count_pos[0] = 0; w.len = 2;
    w.depth = 1;
    return w;
}

/* room for extra more bytes. The buffer is the caller's, so no growth */
static int i_dart_map_room(DartMapWriter *w, size_t extra){
    if (w->err) return 0;
    if (w->len + extra > w->cap){ w->err = -1; return 0; }
    return 1;
}
/* start an entry: a key in a map, none in an array. Bumps the open count */
static int i_dart_map_entry(DartMapWriter *w, const char *key){
    size_t kl = 0;
    if (w->err) return 0;
    if (w->depth == 0){ w->err = -3; return 0; }        /* a finished writer */
    if (w->is_arr[w->depth - 1]){
        if (key){ w->err = -3; return 0; }              /* array elements carry no key */
    } else {
        if (!key){ w->err = -3; return 0; }
        kl = strlen(key);
        if (kl > 255){ w->err = -4; return 0; }
    }
    if (w->count[w->depth - 1] == 0xFFFFu){ w->err = -5; return 0; }
    if (key){
        if (!i_dart_map_room(w, 1 + kl)) return 0;
        w->buf[w->len++] = (uint8_t)kl;
        if (kl) memcpy(w->buf + w->len, key, kl);
        w->len += kl;
    }
    w->count[w->depth - 1]++;
    return 1;
}
static int i_dart_map_put_scalar(DartMapWriter *w, const char *key, uint8_t kind,
                                 const uint8_t *le, uint32_t sz){
    if (!w || !i_dart_map_entry(w, key) || !i_dart_map_room(w, 1u + sz)) return 0;
    w->buf[w->len++] = kind;
    memcpy(w->buf + w->len, le, sz);
    w->len += sz;
    return 1;
}

int dart_map_put_uint(DartMapWriter *w, const char *key, uint64_t v){
    uint8_t le[8];                                       /* the smallest kind that fits */
    uint8_t k = v <= 0xFFu ? DART_U8 : v <= 0xFFFFu ? DART_U16
              : v <= 0xFFFFFFFFu ? DART_U32 : DART_U64;
    i_dart_le_w64(le, v);
    return i_dart_map_put_scalar(w, key, k, le, dart_schema_scalar_size((DartSchemaTypeKind)k));
}

int dart_map_put_int(DartMapWriter *w, const char *key, int64_t v){
    uint8_t le[8];
    uint8_t k = (v >= -128 && v <= 127) ? DART_I8
              : (v >= -32768 && v <= 32767) ? DART_I16
              : (v >= -2147483647 - 1 && v <= 2147483647) ? DART_I32 : DART_I64;
    i_dart_le_w64(le, (uint64_t)v);                      /* two's complement LE: the low bytes */
    return i_dart_map_put_scalar(w, key, k, le, dart_schema_scalar_size((DartSchemaTypeKind)k));
}

int dart_map_put_f64(DartMapWriter *w, const char *key, double v){
    uint8_t le[8]; uint64_t b;
    memcpy(&b, &v, 8); i_dart_le_w64(le, b);
    return i_dart_map_put_scalar(w, key, (uint8_t)DART_F64, le, 8);
}

int dart_map_put_f32(DartMapWriter *w, const char *key, float v){
    uint8_t le[4]; uint32_t b;
    memcpy(&b, &v, 4); i_dart_le_w32(le, b);
    return i_dart_map_put_scalar(w, key, (uint8_t)DART_F32, le, 4);
}

int dart_map_put_bool(DartMapWriter *w, const char *key, int v){
    uint8_t b = v ? 1u : 0u;
    return i_dart_map_put_scalar(w, key, (uint8_t)DART_BOOL, &b, 1);
}

int dart_map_put_string(DartMapWriter *w, const char *key, DartString v){
    if (!w) return 0;
    if (v.len > 0xFFFFu || (v.len && !v.data)){ w->err = -4; return 0; }
    if (!i_dart_map_entry(w, key) || !i_dart_map_room(w, 3u + v.len)) return 0;
    w->buf[w->len++] = (uint8_t)DART_VSTR;
    i_dart_le_w16(w->buf + w->len, (uint16_t)v.len); w->len += 2;
    if (v.len) memcpy(w->buf + w->len, v.data, v.len);
    w->len += v.len;
    return 1;
}

static int i_dart_map_open(DartMapWriter *w, const char *key, uint8_t kind, uint8_t is_arr){
    if (!w || !i_dart_map_entry(w, key)) return 0;
    if (w->depth >= DART_SCHEMA_MAX_DEPTH){ w->err = -2; return 0; }
    if (!i_dart_map_room(w, 3)) return 0;
    w->buf[w->len++] = kind;
    w->count_pos[w->depth] = w->len;                    /* the nested body's count */
    w->buf[w->len++] = 0; w->buf[w->len++] = 0;
    w->count[w->depth] = 0; w->is_arr[w->depth] = is_arr;
    w->depth++;
    return 1;
}
int dart_map_open_map(DartMapWriter *w, const char *key){
    return i_dart_map_open(w, key, (uint8_t)DART_MAP, 0);
}
int dart_map_open_array(DartMapWriter *w, const char *key){
    return i_dart_map_open(w, key, (uint8_t)DART_VARR, 1);
}
int dart_map_close(DartMapWriter *w){
    if (!w || w->err) return 0;
    if (w->depth <= 1){ w->err = -3; return 0; }        /* the root closes in finish */
    w->depth--;
    i_dart_le_w16(w->buf + w->count_pos[w->depth], w->count[w->depth]);
    return 1;
}

uint32_t dart_map_finish(DartMapWriter *w){
    if (!w || w->err || w->depth != 1) return 0;        /* else an unbalanced open and close */
    i_dart_le_w16(w->buf + w->count_pos[0], w->count[0]);
    w->depth = 0;
    return (uint32_t)w->len;
}
/* The schema DSL. The grammar is in spec/schema.md. A type word that is not a built in
 * resolves against the definitions, the environment and the standard library. */
#ifndef DART_NO_STDTYPES
const char *i_dart_std_lookup(const char *name);   /* serialize/stdtypes.c */
#else
#define i_dart_std_lookup(name) ((const char *)0)
#endif

typedef struct { const char *p; const char *err; } i_DartDsl;

/* The definition registry: one arena holding each named type's name and compiled type,
 * plus an index. A definition compiles in its own scratch builder before it is appended. */
#define I_DART_DSL_MAX_DEFS 64u
typedef struct {
    DartSchemaBuilder arena;
    struct { uint32_t noff, toff, tlen; uint8_t nlen; } e[I_DART_DSL_MAX_DEFS];
    uint16_t n;
    uint16_t rec;                                  /* standard type expansion depth */
    const DartSchema *const *env; size_t n_env;
} i_DartDefs;

static void i_dart_dsl_ws(i_DartDsl *d){
    for (;;){
        while (*d->p==' ' || *d->p=='\t' || *d->p=='\r' || *d->p=='\n') d->p++;
        if (d->p[0]=='-' && d->p[1]=='-'){ while (*d->p && *d->p!='\n') d->p++; continue; }
        return;
    }
}
static void i_dart_dsl_fail(i_DartDsl *d, const char *at){ if (!d->err) d->err = at; }
/* an identifier into out[256], NUL terminated. 0 and err when missing or overlong */
static int i_dart_dsl_ident(i_DartDsl *d, char out[256]){
    const char *q = d->p; size_t n;
    if (!((*q>='A'&&*q<='Z') || (*q>='a'&&*q<='z') || *q=='_')){ i_dart_dsl_fail(d, q); return 0; }
    while ((*q>='A'&&*q<='Z') || (*q>='a'&&*q<='z') || (*q>='0'&&*q<='9') || *q=='_') q++;
    n = (size_t)(q - d->p);
    if (n > 255){ i_dart_dsl_fail(d, d->p); return 0; }
    memcpy(out, d->p, n); out[n] = '\0';
    d->p = q;
    return 1;
}
static int i_dart_dsl_expect(i_DartDsl *d, char c){
    if (*d->p == c){ d->p++; return 1; }
    i_dart_dsl_fail(d, d->p);
    return 0;
}
/* a decimal array count, 1 to 65535 */
static int i_dart_dsl_count(i_DartDsl *d, uint16_t *out){
    const char *at = d->p; uint32_t v = 0;
    while (*d->p>='0' && *d->p<='9'){
        v = v*10u + (uint32_t)(*d->p - '0');
        if (v > 0xFFFFu){ i_dart_dsl_fail(d, at); return 0; }
        d->p++;
    }
    if (d->p == at || v == 0){ i_dart_dsl_fail(d, at); return 0; }
    *out = (uint16_t)v;
    return 1;
}
static int i_dart_dsl_kind(const char *s, DartSchemaTypeKind *k){
    static const struct { const char *word; uint8_t kind; } table[] = {
        {"u8",DART_U8},{"u16",DART_U16},{"u32",DART_U32},{"u64",DART_U64},
        {"i8",DART_I8},{"i16",DART_I16},{"i32",DART_I32},{"i64",DART_I64},
        {"f32",DART_F32},{"f64",DART_F64},{"bool",DART_BOOL} };
    size_t i;
    for (i = 0; i < sizeof table / sizeof table[0]; i++)
        if (strcmp(s, table[i].word) == 0){ *k = (DartSchemaTypeKind)table[i].kind; return 1; }
    return 0;
}
/* a signed decimal enum value fitting int64 */
static int i_dart_dsl_enum_value(i_DartDsl *d, int64_t *out){
    const char *at = d->p; int neg = 0, any = 0; uint64_t v = 0, lim;
    if (*d->p == '-'){ neg = 1; d->p++; }
    lim = neg ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    while (*d->p >= '0' && *d->p <= '9'){
        v = v * 10u + (uint64_t)(*d->p - '0');
        if (v > lim){ i_dart_dsl_fail(d, at); return 0; }
        d->p++; any = 1;
    }
    if (!any){ i_dart_dsl_fail(d, at); return 0; }
    *out = neg ? -(int64_t)v : (int64_t)v;
    return 1;
}
/* enum<uN> { Name [= value], ... } after the word enum was read. Streams the options. */
static void i_dart_dsl_enum(i_DartDsl *d, DartSchemaBuilder *b, const char *name){
    char wname[256]; DartSchemaTypeKind backing; size_t count_pos; uint16_t count = 0; int64_t next = 0;
    i_dart_dsl_ws(d);
    if (!i_dart_dsl_expect(d, '<')) return;
    i_dart_dsl_ws(d);
    if (!i_dart_dsl_ident(d, wname)) return;
    if (!i_dart_dsl_kind(wname, &backing) || !i_dart_enum_backing_ok((uint8_t)backing)){
        i_dart_dsl_fail(d, d->p); return;                /* the backing must be an integer kind */
    }
    i_dart_dsl_ws(d);
    if (!i_dart_dsl_expect(d, '>')) return;
    i_dart_dsl_ws(d);
    if (!i_dart_dsl_expect(d, '{')) return;
    count_pos = i_dart_schema_field_enum_open(b, name, backing);
    for (;;){
        char vname[256]; int64_t v;
        i_dart_dsl_ws(d);
        if (*d->p == '}' || *d->p == '\0' || d->err || b->err) break;
        if (!i_dart_dsl_ident(d, vname)) return;
        i_dart_dsl_ws(d);
        if (*d->p == '='){   /* an explicit value, else auto increment */
            d->p++; i_dart_dsl_ws(d);
            if (!i_dart_dsl_enum_value(d, &v)) return;
        } else v = next;
        if (!i_dart_enum_val_fits((uint8_t)backing, v)){ i_dart_dsl_fail(d, d->p); return; }
        i_dart_schema_field_enum_add(b, backing, v, vname, strlen(vname));
        count++; next = v + 1;
        i_dart_dsl_ws(d);
        if (*d->p == ',') d->p++;                        /* an optional separator */
    }
    if (!i_dart_dsl_expect(d, '}')) return;
    i_dart_schema_field_enum_finish(b, count_pos, count);
}

static void i_dart_dsl_field_type(i_DartDsl *d, DartSchemaBuilder *b, i_DartDefs *defs,
                                  const char *name);
static void i_dart_dsl_fields(i_DartDsl *d, DartSchemaBuilder *b, i_DartDefs *defs);

/* records a named type: its name bytes then its compiled type, both in the arena */
static int i_dart_defs_add_bytes(i_DartDefs *defs, const char *name, size_t nlen,
                                 const uint8_t *type, size_t tlen){
    uint32_t noff, toff;
    if (defs->n >= I_DART_DSL_MAX_DEFS || nlen == 0 || nlen > 255 || tlen == 0) return 0;
    noff = (uint32_t)defs->arena.len;
    i_dart_schema_builder_put_raw(&defs->arena, name, nlen);
    toff = (uint32_t)defs->arena.len;
    i_dart_schema_builder_put_raw(&defs->arena, type, tlen);
    if (defs->arena.err) return 0;
    defs->e[defs->n].noff = noff; defs->e[defs->n].nlen = (uint8_t)nlen;
    defs->e[defs->n].toff = toff; defs->e[defs->n].tlen = (uint32_t)tlen;
    defs->n++;
    return 1;
}

static int i_dart_defs_find(i_DartDefs *defs, const char *name, uint32_t *toff, uint32_t *tlen){
    size_t nlen = strlen(name); uint16_t i;
    for (i = 0; i < defs->n; i++)
        if (defs->e[i].nlen == nlen &&
            memcmp(defs->arena.buf + defs->e[i].noff, name, nlen) == 0){
            *toff = defs->e[i].toff; *tlen = defs->e[i].tlen;
            return 1;
        }
    return 0;
}

/* compiles one type spelling, a standard library entry or any type text, as a definition */
static int i_dart_defs_add_text(i_DartDefs *defs, const char *name, const char *text){
    DartSchemaBuilder sb; i_DartDsl sd; int ok = 0;
    if (defs->rec >= 8u) return 0;                       /* the roster is acyclic, but be sure */
    defs->rec++;
    sb = i_dart_schema_begin_raw(defs->arena.alloc, defs->arena.user);
    sd.p = text; sd.err = NULL;
    i_dart_dsl_field_type(&sd, &sb, defs, "");
    i_dart_dsl_ws(&sd);
    if (*sd.p) i_dart_dsl_fail(&sd, sd.p);
    if (!sd.err && !sb.err && sb.len)
        ok = i_dart_defs_add_bytes(defs, name, strlen(name), sb.buf, sb.len);
    if (sb.buf) sb.alloc(sb.user, sb.buf, 0);
    defs->rec--;
    return ok;
}

/* Resolves a type name to its encoding in the arena: the text's own definitions first,
 * then the environment schemas, then the standard library. */
static int i_dart_dsl_ref(i_DartDefs *defs, const char *name, uint32_t *toff, uint32_t *tlen){
    size_t nlen = strlen(name), i;
    const char *std;
    if (i_dart_defs_find(defs, name, toff, tlen)) return 1;
    for (i = 0; i < defs->n_env; i++){
        const DartSchema *t = defs->env ? defs->env[i] : NULL;
        const uint8_t *tb; size_t tl;
        if (!t || t->name.len != nlen || memcmp(t->name.data, name, nlen) != 0) continue;
        if (!i_dart_schema_root_type(t, &tb, &tl)) continue;
        if (!i_dart_defs_add_bytes(defs, name, nlen, tb, tl)) return 0;
        return i_dart_defs_find(defs, name, toff, tlen);
    }
    std = i_dart_std_lookup(name);
    if (std && i_dart_defs_add_text(defs, name, std))
        return i_dart_defs_find(defs, name, toff, tlen);
    return 0;
}

/* turns the type just written at b->buf[head..len) into an array of itself by splicing
 * the array head in front of it, since the count follows the body in the text */
static void i_dart_dsl_splice_array(DartSchemaBuilder *b, size_t head, uint16_t count,
                                    int variable){
    size_t extra = variable ? 1u : 3u, tail;
    if (b->err) return;
    if (!i_dart_schema_builder_reserve(b, extra)) return;
    tail = b->len - head;
    memmove(b->buf + head + extra, b->buf + head, tail);
    if (variable){
        b->buf[head] = (uint8_t)DART_VARR;
    } else {
        b->buf[head] = (uint8_t)DART_ARR;
        i_dart_le_w16(b->buf + head + 1u, count);
    }
    b->len += extra;
}

/* an optional [N] or [] suffix. 1 if one was read, variable for the [] form */
static int i_dart_dsl_suffix(i_DartDsl *d, uint16_t *count, int *variable){
    *count = 0; *variable = 0;
    i_dart_dsl_ws(d);
    if (*d->p != '[') return 0;
    d->p++;
    i_dart_dsl_ws(d);
    if (*d->p == ']'){ d->p++; *variable = 1; return 1; }
    if (!i_dart_dsl_count(d, count)) return 0;
    i_dart_dsl_ws(d);
    if (!i_dart_dsl_expect(d, ']')) return 0;
    return 1;
}

/* Name, Name[N] or Name[]: a reference to an already resolved named type */
static void i_dart_dsl_emit_ref(i_DartDsl *d, DartSchemaBuilder *b, i_DartDefs *defs,
                                const char *name, const char *tname,
                                uint32_t toff, uint32_t tlen){
    uint16_t cnt; int variable, arr;
    size_t nlen = strlen(tname);
    arr = i_dart_dsl_suffix(d, &cnt, &variable);
    if (d->err) return;
    if (arr && variable && !i_dart_schema_builder_var_ok(b)) return;
    i_dart_schema_builder_count(b);
    i_dart_schema_builder_put_name(b, name);
    if (arr){
        if (variable) i_dart_schema_builder_put(b, (uint8_t)DART_VARR);
        else { i_dart_schema_builder_put(b, (uint8_t)DART_ARR); i_dart_schema_builder_put_u16(b, cnt); }
    }
    i_dart_schema_builder_put(b, (uint8_t)DART_NAMED);
    i_dart_schema_builder_put(b, (uint8_t)nlen);
    i_dart_schema_builder_put_raw(b, tname, nlen);
    i_dart_schema_builder_put_raw(b, defs->arena.buf + toff, tlen);
}

/* One type whose leading word is already in tname, with at pointing at it for errors:
 * whatever follows plus the field it defines. A bare root passes name "". */
static void i_dart_dsl_word_type(i_DartDsl *d, DartSchemaBuilder *b, i_DartDefs *defs,
                                 const char *name, const char *tname, const char *at){
    DartSchemaTypeKind k = DART_U8;
    int is_str = 0, has_cap = 0; uint16_t str_cap = 0;
    uint16_t cnt; int variable;
    if (strcmp(tname, "map") == 0){
        dart_schema_field_map(b, name);
        return;
    }
    if (strcmp(tname, "enum") == 0){
        i_dart_dsl_enum(d, b, name);
        return;
    }
    if (strcmp(tname, "string") == 0){                   /* string or string<cap> */
        is_str = 1;
        i_dart_dsl_ws(d);
        if (*d->p == '<'){
            d->p++; has_cap = 1;
            i_dart_dsl_ws(d);
            if (!i_dart_dsl_count(d, &str_cap)) return;
            i_dart_dsl_ws(d);
            if (!i_dart_dsl_expect(d, '>')) return;
        }
    } else if (!i_dart_dsl_kind(tname, &k)){             /* a named type */
        uint32_t toff, tlen;
        if (!i_dart_dsl_ref(defs, tname, &toff, &tlen)){ i_dart_dsl_fail(d, at); return; }
        i_dart_dsl_emit_ref(d, b, defs, name, tname, toff, tlen);
        return;
    }
    if (i_dart_dsl_suffix(d, &cnt, &variable)){
        if (d->err) return;
        if (is_str && !has_cap){ i_dart_dsl_fail(d, at); return; }  /* string[] is ragged */
        if (variable){
            if (is_str) dart_schema_field_var_string_array(b, name, str_cap);
            else        dart_schema_field_var_array(b, name, k);
        } else {
            if (is_str) dart_schema_field_string_array(b, name, str_cap, cnt);
            else        dart_schema_field_array(b, name, k, cnt);
        }
    } else if (d->err){
        return;
    } else if (is_str){
        if (has_cap) dart_schema_field_string(b, name, str_cap);
        else         dart_schema_field_var_string(b, name);
    } else {
        dart_schema_field(b, name, k);
    }
}

static void i_dart_dsl_field_type(i_DartDsl *d, DartSchemaBuilder *b, i_DartDefs *defs,
                                  const char *name){
    char tname[256]; const char *at;
    i_dart_dsl_ws(d);
    if (*d->p == '{'){                                   /* a struct, or an array of them */
        size_t head; uint16_t cnt; int variable;
        d->p++;
        i_dart_schema_builder_count(b);
        i_dart_schema_builder_put_name(b, name);
        head = b->len;
        i_dart_schema_builder_open_struct(b);
        i_dart_dsl_fields(d, b, defs);
        if (!i_dart_dsl_expect(d, '}')) return;
        dart_schema_end_struct(b);
        if (i_dart_dsl_suffix(d, &cnt, &variable) && !d->err)
            i_dart_dsl_splice_array(b, head, cnt, variable);
        return;
    }
    at = d->p;
    if (!i_dart_dsl_ident(d, tname)) return;
    i_dart_dsl_word_type(d, b, defs, name, tname, at);
}

/* the fields of one struct body, up to and not consuming the closing brace */
static void i_dart_dsl_fields(i_DartDsl *d, DartSchemaBuilder *b, i_DartDefs *defs){
    char name[256];
    for (;;){
        i_dart_dsl_ws(d);
        if (*d->p == '}' || *d->p == '\0' || d->err || b->err) return;
        if (!i_dart_dsl_ident(d, name)) return;
        i_dart_dsl_ws(d);
        if (!i_dart_dsl_expect(d, ':')) return;
        i_dart_dsl_field_type(d, b, defs, name);
        if (d->err) return;
        i_dart_dsl_ws(d);
        if (*d->p == ',') d->p++;                        /* an optional separator */
    }
}

/* Name = type: compiles the body on its own, then keeps it, or verifies it matches an
 * existing or reserved definition of the same name exactly. */
static void i_dart_dsl_def(i_DartDsl *d, i_DartDefs *defs, const char *name){
    DartSchemaBuilder sb; uint32_t toff = 0, tlen = 0; int ok = 0, exists;
    exists = i_dart_dsl_ref(defs, name, &toff, &tlen);
    sb = i_dart_schema_begin_raw(defs->arena.alloc, defs->arena.user);
    i_dart_dsl_field_type(d, &sb, defs, "");
    if (!d->err && !sb.err && sb.len){
        if (exists)                                      /* redefining is fine if identical */
            ok = (tlen == sb.len && memcmp(defs->arena.buf + toff, sb.buf, sb.len) == 0);
        else
            ok = i_dart_defs_add_bytes(defs, name, strlen(name), sb.buf, sb.len);
    }
    if (sb.buf) sb.alloc(sb.user, sb.buf, 0);
    if (!ok) i_dart_dsl_fail(d, d->p);
}

DartSchema *dart_schema_compile_env(DartAllocFn alloc, void *user, const char *text,
                                    const DartSchema *const *env, size_t n_env,
                                    const char **err){
    i_DartDsl d; i_DartDefs defs; DartSchemaBuilder root;
    DartSchema *s = NULL;
    char rootbuf[256];
    const char *rname = NULL; size_t rnlen = 0;
    const uint8_t *rtype = NULL; size_t rtlen = 0;
    int have_root = 0, is_struct = 0;

    if (err) *err = NULL;
    if (!alloc || !text) return NULL;
    memset(&defs, 0, sizeof defs);
    defs.env = env; defs.n_env = n_env;
    defs.arena = i_dart_schema_begin_raw(alloc, user);
    root = i_dart_schema_begin_raw(alloc, user);
    d.p = text; d.err = NULL;
    if (defs.arena.err || root.err) i_dart_dsl_fail(&d, text);

    while (!d.err){
        const char *at;
        i_dart_dsl_ws(&d);
        if (!*d.p) break;
        at = d.p;
        if (!i_dart_dsl_ident(&d, rootbuf)) break;
        i_dart_dsl_ws(&d);
        if (*d.p == '='){                                /* a definition */
            d.p++;
            i_dart_dsl_def(&d, &defs, rootbuf);
            continue;
        }
        if (*d.p == '{'){                                /* Name { fields }: a struct root */
            d.p++;
            i_dart_schema_builder_open_struct(&root);
            i_dart_dsl_fields(&d, &root, &defs);
            if (!i_dart_dsl_expect(&d, '}')) break;
            dart_schema_end_struct(&root);
            rnlen = strlen(rootbuf); rname = rootbuf;
            is_struct = 1;
        } else {                                         /* a bare type, maybe a reference */
            d.p = at;
            i_dart_dsl_field_type(&d, &root, &defs, "");
        }
        have_root = 1;
        i_dart_dsl_ws(&d);
        if (*d.p) i_dart_dsl_fail(&d, d.p);              /* the root ends the text */
        break;
    }
    if (!d.err && root.err) i_dart_dsl_fail(&d, d.p);
    if (!d.err && defs.arena.err) i_dart_dsl_fail(&d, d.p);

    if (!d.err){
        if (have_root){
            rtype = root.buf; rtlen = root.len;
            if (!is_struct && rtlen >= 2 && rtype[0] == DART_NAMED){
                size_t nl = rtype[1];                    /* a bare reference is an alias root */
                if (2u + nl <= rtlen){
                    rname = (const char *)(rtype + 2); rnlen = nl;
                    rtype += 2u + nl; rtlen -= 2u + nl;
                }
            }
        } else if (defs.n){                              /* no root: the last definition is it */
            uint16_t last = (uint16_t)(defs.n - 1u);
            rname = (const char *)(defs.arena.buf + defs.e[last].noff);
            rnlen = defs.e[last].nlen;
            rtype = defs.arena.buf + defs.e[last].toff;
            rtlen = defs.e[last].tlen;
        }
        if (!rtype || !rtlen) i_dart_dsl_fail(&d, d.p);
    }
    /* the reservation covers definitions and references, never a plain Name { ... } root,
     * since nothing can reference a root. See spec/schema.md. */
    if (!d.err){
        size_t wlen = 2u + rnlen + rtlen;
        uint8_t *w = (uint8_t *)alloc(user, NULL, wlen);
        if (w){
            w[0] = (uint8_t)DART_SCHEMA_WIRE_VERSION;
            w[1] = (uint8_t)rnlen;
            if (rnlen) memcpy(w + 2, rname, rnlen);
            memcpy(w + 2 + rnlen, rtype, rtlen);
            s = dart_schema_parse(w, wlen, alloc, user);
            alloc(user, w, 0);
        }
        if (!s) i_dart_dsl_fail(&d, d.p);
    }
    if (root.buf) alloc(user, root.buf, 0);
    if (defs.arena.buf) alloc(user, defs.arena.buf, 0);
    if (!s && err) *err = d.err ? d.err : d.p;
    return s;
}

DartSchema *dart_schema_compile(DartAllocFn alloc, void *user, const char *text, const char **err){
    return dart_schema_compile_env(alloc, user, text, NULL, 0, err);
}
#pragma endregion
#ifndef DART_NO_STDTYPES
#pragma region serialize/stdtypes.c
#include <string.h>

/* The roster: a name and the canonical spelling of its type. These strings are the one
 * definition, the DSL compiles them on demand. Order does not matter, resolution is recursive. */
typedef struct { const char *name, *text; } i_DartStdEntry;

static const i_DartStdEntry i_dart_std_table[] = {
    { "Float2",  "{ x: f32, y: f32 }" },
    { "Float3",  "{ x: f32, y: f32, z: f32 }" },
    { "Float4",  "{ x: f32, y: f32, z: f32, w: f32 }" },
    { "Double2", "{ x: f64, y: f64 }" },
    { "Double3", "{ x: f64, y: f64, z: f64 }" },
    { "Double4", "{ x: f64, y: f64, z: f64, w: f64 }" },
    { "Int2",    "{ x: i32, y: i32 }" },
    { "Int3",    "{ x: i32, y: i32, z: i32 }" },
    { "Int4",    "{ x: i32, y: i32, z: i32, w: i32 }" },
    { "Quaternion", "{ x: f64, y: f64, z: f64, w: f64 }" },
    { "Color",   "{ r: u8, g: u8, b: u8, a: u8 }" },
    { "Rect",    "{ x: f32, y: f32, w: f32, h: f32 }" },
    { "RectI",   "{ x: i32, y: i32, w: i32, h: i32 }" },
    { "Transform", "{ translation: Double3, rotation: Quaternion, parent: string<30> }" },
    { "Twist",   "{ linear: Double3, angular: Double3 }" },
    { "GeoPoint","{ lat: f64, lon: f64, alt: f64 }" },
    { "Uuid",      "u8[16]" },
    { "Timestamp", "i64" },
    { "Duration",  "i64" },
    { "Matrix3x3", "f32[9]" },
    { "Matrix4x4", "f32[16]" },
    { "Uri",       "string<256>" },
    { "Image",   "{ width: u32, height: u32, stride: u32,"
                 "  format: enum<u8> { Mono8 = 0, Mono16 = 1, Rgb8 = 2, Rgba8 = 3,"
                 "                     Bgr8 = 4, Yuyv = 5, Nv12 = 6, Monof32 = 7,"
                 "                     Jpeg = 16, Png = 17 },"
                 "  data: u8[] }" },
    { "VideoFrame", "{ codec: enum<u8> { Unknown = 0, Mjpeg = 1, H264 = 2, H265 = 3, Av1 = 4 },"
                 "  width: u32, height: u32,"
                 "  keyframe: bool, pts: Timestamp, data: u8[] }" },
    { "ExternalVideoStream",
                 "{ kind: enum<u8> { Rtsp = 0, WebrtcWhep = 1, Hls = 2, Srt = 3,"
                 "                   Rtp = 4, HttpMjpeg = 5, Other = 15 },"
                 "  codec: enum<u8> { Unknown = 0, Mjpeg = 1, H264 = 2, H265 = 3, Av1 = 4 },"
                 "  width: u32, height: u32,"
                 "  url: Uri, name: string<32> }" },
    { "CameraIntrinsics",
                 "{ width: u32, height: u32,"
                 "  fx: f64, fy: f64, cx: f64, cy: f64,"
                 "  model: enum<u8> { None = 0, BrownConrady = 1, Fisheye = 2, Rational = 3 },"
                 "  coeffs: f64[8] }" },
    { "JointState", "{ position: f64[], velocity: f64[], effort: f64[] }" },
    { "JointNames", "{ name: string<32>[] }" }
};

/* the table is indexed by DartStdType minus 1, a mismatch would shift every name */
typedef char i_dart_std_table_check[
    (sizeof i_dart_std_table / sizeof i_dart_std_table[0] == (size_t)DART_STD_COUNT - 1u) ? 1 : -1];

const char *dart_std_name(DartStdType t){
    if (t <= DART_STD_NONE || t >= DART_STD_COUNT) return 0;
    return i_dart_std_table[(int)t - 1].name;
}
const char *dart_std_text(DartStdType t){
    if (t <= DART_STD_NONE || t >= DART_STD_COUNT) return 0;
    return i_dart_std_table[(int)t - 1].text;
}

DartStdType dart_std_by_name(DartString name){
    int i;
    if (!name.data || !name.len) return DART_STD_NONE;
    for (i = 0; i < (int)DART_STD_COUNT - 1; i++){
        const char *n = i_dart_std_table[i].name;
        size_t k = strlen(n);
        if (k == name.len && memcmp(n, name.data, k) == 0) return (DartStdType)(i + 1);
    }
    return DART_STD_NONE;
}

/* The one seam the DSL calls: an unknown type word is looked up here and compiled from its text. */
const char *i_dart_std_lookup(const char *name){
    int i;
    if (!name) return 0;
    for (i = 0; i < (int)DART_STD_COUNT - 1; i++)
        if (strcmp(i_dart_std_table[i].name, name) == 0) return i_dart_std_table[i].text;
    return 0;
}

DartSchema *dart_std_schema(DartStdType t, DartAllocFn alloc, void *user){
    const char *n = dart_std_name(t);
    if (!n || !alloc) return 0;
    return dart_schema_compile(alloc, user, n, 0);   /* the name alone resolves to the type */
}

/* Does type match the canonical root type bytes of the standard type. */
static int i_dart_std_shape_eq(DartStdType t, DartBytes type, DartAllocFn alloc, void *user){
    DartSchema *c = dart_std_schema(t, alloc, user);
    DartBytes w; DartString nm; size_t off; int ok = 0;
    if (!c) return 0;
    w = dart_schema_wire(c); nm = dart_schema_name(c);
    off = 2u + nm.len;
    if (type.data && off < w.len && type.len == w.len - off)
        ok = memcmp(type.data, w.data + off, type.len) == 0;
    dart_schema_free(c, alloc, user);
    return ok;
}

/* peel a NAMED wrapper: fills *name and returns the inner type bytes */
static int i_dart_std_peel(DartBytes type, DartString *name, DartBytes *inner){
    size_t nl;
    if (!type.data || type.len < 2u || type.data[0] != DART_NAMED) return 0;
    nl = type.data[1];
    if (2u + nl > type.len || nl == 0) return 0;
    *name  = dart_string((const char *)(type.data + 2), nl);
    *inner = dart_bytes(type.data + 2 + nl, type.len - 2 - nl);
    return 1;
}

static DartStdType i_dart_std_check(DartString name, DartBytes inner,
                                    DartAllocFn alloc, void *user){
    DartStdType t = dart_std_by_name(name);
    if (t == DART_STD_NONE || !alloc) return DART_STD_NONE;
    return i_dart_std_shape_eq(t, inner, alloc, user) ? t : DART_STD_NONE;
}

DartStdType dart_std_recognize(const DartSchema *s, DartAllocFn alloc, void *user){
    DartBytes w = dart_schema_wire(s);
    DartString nm = dart_schema_name(s);
    size_t off = 2u + nm.len;
    if (!w.data || off >= w.len) return DART_STD_NONE;
    return i_dart_std_check(nm, dart_bytes(w.data + off, w.len - off), alloc, user);
}

DartStdType dart_std_recognize_field(const DartSchema *s, uint16_t field,
                                     DartAllocFn alloc, void *user){
    DartBytes type = dart_schema_field_type_wire(s, field), inner;
    DartString name;
    if (!i_dart_std_peel(type, &name, &inner)) return DART_STD_NONE;
    return i_dart_std_check(name, inner, alloc, user);
}

DartStdType dart_std_recognize_elem(const DartSchema *s, uint16_t field,
                                    DartAllocFn alloc, void *user){
    DartBytes type = dart_schema_field_type_wire(s, field), inner;
    DartSchemaFieldInfo fi;
    DartString name;
    size_t head;
    if (!dart_schema_field_at(s, field, &fi) || !type.data) return DART_STD_NONE;
    if (fi.kind == DART_ARR)       head = 3u;      /* [ARR][u16 count] */
    else if (fi.kind == DART_VARR) head = 1u;      /* [VARR] */
    else return DART_STD_NONE;
    if (head >= type.len) return DART_STD_NONE;
    if (!i_dart_std_peel(dart_bytes(type.data + head, type.len - head), &name, &inner))
        return DART_STD_NONE;
    return i_dart_std_check(name, inner, alloc, user);
}

/* No math.h, so a consumer's build line never grows an -lm. Newton from the halved
 * exponent seed converges to full double precision within five steps. */
static double i_dart_sqrt(double x){
    uint64_t b; double r; int i;
    if (!(x > 0.0)) return 0.0;                       /* 0, negatives and NaN alike */
    memcpy(&b, &x, 8);
    b = (b >> 1) + 0x1FF8000000000000ull;
    memcpy(&r, &b, 8);
    for (i = 0; i < 5; i++) r = 0.5 * (r + x / r);
    return r;
}

double dart_double3_length(DartDouble3 a){
    return i_dart_sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}

DartDouble3 dart_double3_normalize(DartDouble3 a){
    double n = dart_double3_length(a);
    return n > 0.0 ? dart_double3_scale(a, 1.0 / n) : a;
}

DartQuaternion dart_quaternion_normalize(DartQuaternion q){
    double n = i_dart_sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n <= 0.0) return dart_quaternion_identity();
    n = 1.0 / n;
    return dart_quaternion(q.x * n, q.y * n, q.z * n, q.w * n);
}

/* v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v), the branch free rotation form */
DartDouble3 dart_quaternion_rotate(DartQuaternion q, DartDouble3 v){
    DartDouble3 u = dart_double3(q.x, q.y, q.z);
    DartDouble3 t = dart_double3_cross(u, v);
    t = dart_double3_add(t, dart_double3_scale(v, q.w));
    return dart_double3_add(v, dart_double3_scale(dart_double3_cross(u, t), 2.0));
}
#pragma endregion
#endif /* !DART_NO_STDTYPES */
#pragma region shm/core.c
/* Segment mapping and chunks over the platform layer. Compiles to nothing without DART_SHM. */


#ifdef DART_SHM
#include <string.h>
#include <stdlib.h>

size_t i_dart_shm_desc_encode(const i_DartShmDesc *d, uint8_t out[DART_SHM_DESC_WIRE]){
    i_dart_le_w64(out,    d->segment_id);
    i_dart_le_w32(out+8,  d->chunk);
    i_dart_le_w32(out+12, d->length);
    i_dart_le_w64(out+16, d->generation);
    return DART_SHM_DESC_WIRE;
}
int i_dart_shm_desc_decode(i_DartShmDesc *d, const uint8_t *in, size_t len){
    if (len < DART_SHM_DESC_WIRE) return 0;
    d->segment_id = i_dart_le_r64(in);
    d->chunk      = i_dart_le_r32(in+8);
    d->length     = i_dart_le_r32(in+12);
    d->generation = i_dart_le_r64(in+16);
    return 1;
}

void i_dart_shm_seg_name(char *buf, uint64_t segment_id){
    static const char hex_digits[] = "0123456789abcdef";
    const char prefix[] = "/dart.shm."; int i, k = 0;
    while (prefix[k]){ buf[k] = prefix[k]; k++; }
    for (i=15;i>=0;i--) buf[k++] = hex_digits[(segment_id >> (4*i)) & 0xF];
    buf[k] = 0;
}

uint32_t i_dart_shm_class_bytes(uint32_t k){ return DART_SHM_CLASS_BASE << (k*DART_SHM_CLASS_SHIFT); }
uint32_t i_dart_shm_class_for(uint32_t len){
    uint32_t k;
    for (k=0;k<DART_SHM_N_CLASSES;k++) if (i_dart_shm_class_bytes(k) >= len) return k;
    return DART_SHM_N_CLASSES;   /* bigger than the top class, the caller sends inline */
}

struct i_DartShmPool {
    void    *base;          /* mapping base */
    void    *handle;        /* OS handle for detach */
    size_t   map_bytes;     /* total mapped size */
    i_DartShmSegHdr *hdr;
    uint8_t *chunks;        /* base of the chunk region */
    uint32_t chunk_bytes;
    uint32_t n_chunks;
    uint32_t stride;        /* per chunk bytes including the header */
    int      is_creator;
};

size_t i_dart_shm_state_bytes(void){ return i_dart_align_up(sizeof(struct i_DartShmPool), 16u); }

#define DART__SHM_HDR_SZ  ((uint32_t)i_dart_align_up(sizeof(i_DartShmSegHdr), 16u))
#define DART__SHM_CHDR_SZ ((uint32_t)i_dart_align_up(sizeof(i_DartShmChunkHdr), 16u))

static void i_dart_shm_geom(uint32_t chunk_bytes, uint32_t n_chunks,
                           uint32_t *out_stride, size_t *out_total){
    uint32_t aligned = (uint32_t)i_dart_align_up(chunk_bytes, 16u);
    uint32_t stride = DART__SHM_CHDR_SZ + aligned;
    *out_stride = stride;
    *out_total  = (size_t)DART__SHM_HDR_SZ + (size_t)n_chunks * stride;
}

static i_DartShmChunkHdr *i_dart_shm_chunk_hdr(struct i_DartShmPool *p, uint32_t i){
    return (i_DartShmChunkHdr*)(p->chunks + (size_t)i * p->stride);
}
static uint8_t *i_dart_shm_chunk_pay(struct i_DartShmPool *p, uint32_t i){
    return (uint8_t*)i_dart_shm_chunk_hdr(p, i) + DART__SHM_CHDR_SZ;
}

i_DartShmPool *i_dart_shm_create(void *pool_mem, const i_DartShmConfig *cfg){
    struct i_DartShmPool *p = (struct i_DartShmPool*)pool_mem;
    uint32_t chunk_bytes = cfg->chunk_bytes ? cfg->chunk_bytes : DART_SHM_CHUNK_BYTES;
    uint32_t n_chunks = cfg->n_chunks    ? cfg->n_chunks    : DART_SHM_CHUNKS;
    uint32_t stride; size_t total; void *handle = NULL, *base; uint32_t i;
    if (!p || !cfg) return NULL;
    i_dart_shm_geom(chunk_bytes, n_chunks, &stride, &total);
    base = i_dart_plat_shm_create(cfg->name, total, &handle);
    if (!base) return NULL;
    memset(p, 0, sizeof *p);
    p->base = base; p->handle = handle; p->map_bytes = total;
    p->hdr = (i_DartShmSegHdr*)base;
    p->chunks = (uint8_t*)base + DART__SHM_HDR_SZ;
    p->chunk_bytes = chunk_bytes; p->n_chunks = n_chunks; p->stride = stride; p->is_creator = 1;
    /* the segment starts zero filled: stamp the header and clear the generations */
    p->hdr->magic = DART_SHM_MAGIC; p->hdr->version = DART_SHM_VERSION;
    p->hdr->segment_id = cfg->segment_id; p->hdr->chunk_bytes = chunk_bytes; p->hdr->n_chunks = n_chunks;
    p->hdr->owner_pid = i_dart_plat_pid();
    i_dart_plat_host_uuid(p->hdr->owner_host);
    for (i = 0; i < n_chunks; i++){ i_DartShmChunkHdr *c = i_dart_shm_chunk_hdr(p, i); c->generation = 0; c->length = 0; }
    return p;
}

i_DartShmPool *i_dart_shm_attach(void *pool_mem, const i_DartShmConfig *cfg){
    struct i_DartShmPool *p = (struct i_DartShmPool*)pool_mem;
    uint32_t chunk_bytes, n_chunks, stride; size_t map_bytes = 0, expect;
    void *handle = NULL, *base; uint8_t ours[16];
    if (!p || !cfg) return NULL;
    /* the geometry comes from the writer's header, cfg's create only fields are ignored */
    base = i_dart_plat_shm_attach(cfg->name, &map_bytes, &handle);
    if (!base) return NULL;
    memset(p, 0, sizeof *p);
    p->base = base; p->handle = handle; p->map_bytes = map_bytes;
    p->hdr = (i_DartShmSegHdr*)base;
    i_dart_plat_host_uuid(ours);
    chunk_bytes = p->hdr->chunk_bytes; n_chunks = p->hdr->n_chunks;
    i_dart_shm_geom(chunk_bytes, n_chunks, &stride, &expect);
    /* a stale, foreign, mismatched or truncated segment: the caller falls back to UDP */
    if (p->hdr->magic != DART_SHM_MAGIC || p->hdr->version != DART_SHM_VERSION ||
        memcmp(p->hdr->owner_host, ours, 16) != 0 ||
        chunk_bytes == 0 || n_chunks == 0 || expect > map_bytes){
        i_dart_plat_shm_detach(base, map_bytes, handle, 0);
        return NULL;
    }
    p->chunks = (uint8_t*)base + DART__SHM_HDR_SZ;
    p->chunk_bytes = chunk_bytes; p->n_chunks = n_chunks; p->stride = stride; p->is_creator = 0;
    return p;
}

void *i_dart_shm_chunk(i_DartShmPool *p, uint32_t chunk, uint32_t *out_cap){
    if (!p || chunk >= p->n_chunks) return NULL;
    if (out_cap) *out_cap = p->chunk_bytes;
    return i_dart_shm_chunk_pay(p, chunk);
}

void i_dart_shm_stamp(i_DartShmPool *p, uint32_t chunk, uint32_t len, i_DartShmDesc *out){
    i_DartShmChunkHdr *c;
    uint64_t generation;
    if (!p || chunk >= p->n_chunks) return;
    c = i_dart_shm_chunk_hdr(p, chunk);
    c->length = len;
    generation = c->generation + 1u;                       /* bump so a straggler sees the reuse */
    i_dart_plat_atomic_store64(&c->generation, generation);  /* release: publishes the payload */
    if (out){ out->segment_id = p->hdr->segment_id; out->chunk = chunk; out->length = len; out->generation = generation; }
}

const void *i_dart_shm_read(i_DartShmPool *p, const i_DartShmDesc *d, uint32_t *out_len){
    i_DartShmChunkHdr *c;
    if (!p || !d || d->chunk >= p->n_chunks) return NULL;
    c = i_dart_shm_chunk_hdr(p, d->chunk);
    if (i_dart_plat_atomic_load64(&c->generation) != d->generation) return NULL;  /* recycled */
    if (d->length > p->chunk_bytes) return NULL;
    if (out_len) *out_len = d->length;
    return i_dart_shm_chunk_pay(p, d->chunk);
}

int i_dart_shm_verify(i_DartShmPool *p, const i_DartShmDesc *d){
    i_DartShmChunkHdr *c;
    if (!p || !d || d->chunk >= p->n_chunks) return 0;
    c = i_dart_shm_chunk_hdr(p, d->chunk);
    return i_dart_plat_atomic_load64(&c->generation) == d->generation;
}

void i_dart_shm_detach(i_DartShmPool *p){
    if (!p || !p->base) return;
    i_dart_plat_shm_detach(p->base, p->map_bytes, p->handle, p->is_creator);
    p->base = NULL; p->handle = NULL;
}

int i_dart_shm_host_match(const uint8_t peer_host[16], const uint8_t our_host[16]){
    return memcmp(peer_host, our_host, 16) == 0;
}

#endif /* DART_SHM */
#pragma endregion
#pragma region node/core.c
/* The sans-IO node core. A runtime drives it and does the IO. The rules are in spec/node.md. */

#include <string.h>

/* dart_event_str and its bounded appenders. No stdio, so it stays in the sans-IO core. */
static char *i_dart_event_append_str(char *p, char *end, const char *s){
    if (!s) return p;
    while (*s && p < end) *p++ = *s++;
    return p;
}
static char *i_dart_event_append_u64(char *p, char *end, uint64_t v){
    char tmp[20]; int n = 0;
    do { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v);
    while (n && p < end) *p++ = tmp[--n];
    return p;
}
/* dotted quad and port */
static char *i_dart_event_append_addr(char *p, char *end, const DartEvent *ev){
    int i;
    for (i = 0; i < 4; i++){ if (i) p = i_dart_event_append_str(p,end,"."); p = i_dart_event_append_u64(p,end,ev->ip[i]); }
    p = i_dart_event_append_str(p,end,":"); return i_dart_event_append_u64(p,end,ev->port);
}
/* topic=name or topic=index */
static char *i_dart_event_append_topic(char *p, char *end, const DartEvent *ev){
    p = i_dart_event_append_str(p,end,"topic=");
    if (ev->topic_name) return i_dart_event_append_str(p,end,ev->topic_name);
    return i_dart_event_append_u64(p,end,ev->topic);
}
/* the peer's name, else id=n */
static char *i_dart_event_append_peer(char *p, char *end, const DartEvent *ev){
    if (ev->peer_name && ev->peer_name[0]) return i_dart_event_append_str(p,end,ev->peer_name);
    p = i_dart_event_append_str(p,end,"id="); return i_dart_event_append_u64(p,end,ev->peer);
}
#ifndef DART_NO_DIAG   /* only the verbose error body uses these two */
static char *i_dart_event_append_hex(char *p, char *end, uint64_t v){
    char tmp[16]; int n = 0;
    do { int d = (int)(v & 0xF); tmp[n++] = (char)(d < 10 ? '0'+d : 'a'+d-10); v >>= 4; } while (v);
    while (n && p < end) *p++ = tmp[--n];
    return p;
}
static char *i_dart_event_append_oserr(char *p, char *end, const DartEvent *ev){
    if (!ev->os_error) return p;
    p = i_dart_event_append_str(p,end," (os_err="); p = i_dart_event_append_u64(p,end,(uint64_t)(unsigned int)ev->os_error);
    return i_dart_event_append_str(p,end,")");
}
#endif

/* the DART_ERROR body, split out so the text compiles away under DART_NO_DIAG */
static char *i_dart_event_error_str(char *p, char *end, const DartEvent *ev){
#ifdef DART_NO_DIAG
    p = i_dart_event_append_str(p,end,"error "); return i_dart_event_append_u64(p,end,(uint64_t)ev->error);
#else
    switch (ev->error){
    case DART_E_NAME_COLLISION:
        p=i_dart_event_append_str(p,end,"name-collision "); p=i_dart_event_append_topic(p,end,ev);
        p=i_dart_event_append_str(p,end," peer "); p=i_dart_event_append_peer(p,end,ev);
        p=i_dart_event_append_str(p,end," identity=0x"); p=i_dart_event_append_hex(p,end,ev->identity);
        p=i_dart_event_append_str(p,end,": match refused"); break;
    case DART_E_QOS_INCOMPATIBLE:
        p=i_dart_event_append_str(p,end,"qos-incompatible "); p=i_dart_event_append_topic(p,end,ev);
        p=i_dart_event_append_str(p,end," from "); p=i_dart_event_append_peer(p,end,ev);
        p=i_dart_event_append_str(p,end,": reliable subscriber refused best-effort publisher"); break;
    case DART_E_KIND_MISMATCH:
        p=i_dart_event_append_str(p,end,"kind-mismatch "); p=i_dart_event_append_topic(p,end,ev);
        p=i_dart_event_append_str(p,end," from "); p=i_dart_event_append_peer(p,end,ev);
        p=i_dart_event_append_str(p,end,": same name, different entity kind, refused"); break;
    case DART_E_SCHEMA_MISMATCH:
        p=i_dart_event_append_str(p,end,"schema-mismatch "); p=i_dart_event_append_topic(p,end,ev);
        p=i_dart_event_append_str(p,end," peer "); p=i_dart_event_append_peer(p,end,ev);
        p=i_dart_event_append_str(p,end,": ");
        p=i_dart_event_append_str(p,end, ev->schema_detail && ev->schema_detail[0]
                                  ? ev->schema_detail : "incompatible schemas, refused"); break;
    case DART_E_INTEREST_OVERFLOW:
        p=i_dart_event_append_str(p,end,"interest-overflow peer "); p=i_dart_event_append_peer(p,end,ev);
        p=i_dart_event_append_str(p,end,": "); p=i_dart_event_append_u64(p,end,ev->lost_count);
        p=i_dart_event_append_str(p,end," matched topics whose index map could not be allocated"); break;
    case DART_E_META_TRUNCATED_INTEREST:
        p=i_dart_event_append_str(p,end,"meta-truncated: interest list dropped (announce overlay full)"); break;
    case DART_E_META_TRUNCATED_SCHEMA:
        p=i_dart_event_append_str(p,end,"meta-truncated: schema section dropped (announce overlay full)"); break;
    case DART_E_PEER_META_TOO_BIG:
        p=i_dart_event_append_str(p,end,"peer-meta-too-big "); p=i_dart_event_append_peer(p,end,ev);
        if (ev->ip_len==4){ p=i_dart_event_append_str(p,end," at "); p=i_dart_event_append_addr(p,end,ev); }
        p=i_dart_event_append_str(p,end,": "); p=i_dart_event_append_u64(p,end,ev->too_big_bytes);
        p=i_dart_event_append_str(p,end," byte blob exceeds our capacity, refused"); break;
    case DART_E_MSG_TOO_BIG:
        p=i_dart_event_append_str(p,end,"msg-too-big "); p=i_dart_event_append_topic(p,end,ev);
        p=i_dart_event_append_str(p,end," from "); p=i_dart_event_append_peer(p,end,ev);
        p=i_dart_event_append_str(p,end," ("); p=i_dart_event_append_u64(p,end,ev->too_big_bytes);
        p=i_dart_event_append_str(p,end," bytes), skipped"); break;
    case DART_E_PEER_REFUSED:
        p=i_dart_event_append_str(p,end,"peer-refused at "); p=i_dart_event_append_addr(p,end,ev);
        p=i_dart_event_append_str(p,end,": peer table full of active peers (raise max_peers)"); break;
    case DART_E_EVICTED_UNSENT:
        p=i_dart_event_append_str(p,end,"evicted-unsent "); p=i_dart_event_append_topic(p,end,ev);
        p=i_dart_event_append_str(p,end," seqno "); p=i_dart_event_append_u64(p,end,ev->lost_first);
        p=i_dart_event_append_str(p,end,".."); p=i_dart_event_append_u64(p,end,ev->lost_first + ev->lost_count - 1);
        p=i_dart_event_append_str(p,end,": send burst outran the TX drain"); break;
    case DART_E_UNMATCHED_SEND:
        p=i_dart_event_append_str(p,end,"unmatched-send "); p=i_dart_event_append_topic(p,end,ev);
        p=i_dart_event_append_str(p,end,": committed with no subscriber while a match was still resolving (likely missed an already-present subscriber)"); break;
    case DART_E_DUPLICATE_AUTHORITY:
        p=i_dart_event_append_str(p,end,"duplicate-authority "); p=i_dart_event_append_topic(p,end,ev);
        p=i_dart_event_append_str(p,end,": peer "); p=i_dart_event_append_peer(p,end,ev);
        p=i_dart_event_append_str(p,end," also claims the handler/owner side (expected exactly one)"); break;
    case DART_E_OOM:
        p=i_dart_event_append_str(p,end,"out-of-memory");
        if (ev->too_big_bytes){ p=i_dart_event_append_str(p,end,": "); p=i_dart_event_append_u64(p,end,ev->too_big_bytes);
                                p=i_dart_event_append_str(p,end," bytes needed"); }
        break;
    case DART_E_PLATFORM:
        p=i_dart_event_append_str(p,end,"platform net init failed"); break;
    case DART_E_BAD_ADDRESS:
        p=i_dart_event_append_str(p,end,"configured address could not be parsed"); break;
    case DART_E_SOCKET:
        p=i_dart_event_append_str(p,end,"socket open failed"); p=i_dart_event_append_oserr(p,end,ev); break;
    case DART_E_BIND:
        p=i_dart_event_append_str(p,end,"bind failed on port "); p=i_dart_event_append_u64(p,end,ev->port);
        p=i_dart_event_append_oserr(p,end,ev); break;
    case DART_E_MCAST_JOIN:
        p=i_dart_event_append_str(p,end,"multicast join failed"); p=i_dart_event_append_oserr(p,end,ev); break;
    case DART_E_SEND:
        p=i_dart_event_append_str(p,end,"send failed to "); p=i_dart_event_append_peer(p,end,ev);
        if (ev->topic_name){ p=i_dart_event_append_str(p,end," "); p=i_dart_event_append_topic(p,end,ev); }
        if (ev->too_big_bytes){ p=i_dart_event_append_str(p,end," ("); p=i_dart_event_append_u64(p,end,ev->too_big_bytes);
                                p=i_dart_event_append_str(p,end," B)"); }
        p=i_dart_event_append_oserr(p,end,ev); break;
    case DART_E_RECV:
        p=i_dart_event_append_str(p,end,"recv failed"); p=i_dart_event_append_oserr(p,end,ev); break;
    case DART_E_POLL:
        p=i_dart_event_append_str(p,end,"poll failed"); p=i_dart_event_append_oserr(p,end,ev); break;
    case DART_E_WAKER:
        p=i_dart_event_append_str(p,end,"cross-thread waker unavailable (wakes at next tick)"); break;
    case DART_E_NONE: default:
        p=i_dart_event_append_str(p,end,"error"); break;
    }
    return p;
#endif
}

const char *dart_event_str(const DartEvent *ev, char *buf, size_t cap){
    char *p, *end;
    if (!buf || !cap) return buf;
    p = buf; end = buf + cap - 1;                  /* one byte reserved for the NUL */
    switch (ev->kind){
    case DART_PEER_UP:
        p = i_dart_event_append_str(p,end,"peer-up "); p = i_dart_event_append_peer(p,end,ev);
        if (ev->ip_len == 4){ p = i_dart_event_append_str(p,end," at "); p = i_dart_event_append_addr(p,end,ev); }
        break;
    case DART_PEER_DOWN:
        p = i_dart_event_append_str(p,end,"peer-down "); p = i_dart_event_append_peer(p,end,ev);
        break;
    case DART_PEER_INTEREST:
        p = i_dart_event_append_str(p,end,"interest from "); p = i_dart_event_append_peer(p,end,ev);
        p = i_dart_event_append_str(p,end," publish-to="); p = i_dart_event_append_u64(p,end,ev->publish_topics);
        p = i_dart_event_append_str(p,end," topics, receive-from="); p = i_dart_event_append_u64(p,end,ev->receive_topics);
        p = i_dart_event_append_str(p,end," topics");
        break;
    case DART_MSG_LOST:
        p = i_dart_event_append_str(p,end,"msg-lost "); p = i_dart_event_append_topic(p,end,ev);
        p = i_dart_event_append_str(p,end," from "); p = i_dart_event_append_peer(p,end,ev);
        p = i_dart_event_append_str(p,end," seqno "); p = i_dart_event_append_u64(p,end,ev->lost_first);
        p = i_dart_event_append_str(p,end,".."); p = i_dart_event_append_u64(p,end,ev->lost_first + ev->lost_count - 1);
        break;
    case DART_ERROR:
        p = i_dart_event_error_str(p, end, ev);
        break;
    }
    *p = '\0';                                     /* p is at most end, in range */
    return buf;
}

/* The reflection tables: one channel per advertised topic index, folded into entities by
   kind and the four byte suffix convention. See spec/reflection.md. */
#define I_DART_NONE16   0xFFFFu
#define I_DART_NAME_NONE 0xFFFFFFFFu
typedef struct {
    uint32_t hash;          /* the low 32 name id from the announce */
    uint32_t name_off;      /* into the names arena, NAME_NONE until details land */
    const DartSchema *schema;   /* interned, lives until close */
    uint64_t schema_hash;
    uint16_t entity;        /* the entity slot, NONE16 until folded */
    uint8_t  name_len, kind, role, reliable, attrs, present;
} i_DartChannel;
typedef struct {
    uint64_t id;            /* dart_topic_id of the base name, 0 while unfetched */
    uint32_t name_off, hash;
    uint16_t primary, rsp, prg, set;   /* channel indices, NONE16 where absent */
    uint8_t  name_len, kind, incomplete;
} i_DartPeerEntity;
typedef struct {
    i_DartChannel    *chan; uint32_t n_chan, cap_chan;   /* dense by the peer's topic index */
    i_DartPeerEntity *ent;  uint32_t n_ent,  cap_ent;
    char             *names; uint32_t names_len, names_cap;
    uint8_t  dirty;
    uint8_t  addr_len;
    char     addr[48];      /* "ip:port", formatted once at peer up */
} i_DartReflect;
/* one folded entity of the whole mesh, keyed by kind and the 64 bit name id */
typedef struct {
    uint64_t id, generation;
    uint32_t from_peer, provider;
    uint16_t from_slot, provider_slot, providers, consumers;
    uint8_t  kind, conflict, has_provider;
} i_DartMeshEntity;

/* The core's per peer lifecycle state, kept in the discovery peer's user scratch so the
   node holds no peer table of its own. The interest fields are the external fetch. */
typedef struct {
    uint8_t  added; uint8_t dormant; uint8_t detail_due; uint8_t interest_due;
    uint32_t interest_epoch;     /* bumps on every reflected change, the observer cache key */
    uint32_t interest_version;   /* the last fully assembled external version, 0 = none */
    uint32_t fetch_version;      /* the version the cursor is assembling, 0 = idle */
    uint32_t fetch_cursor;       /* bytes assembled so far */
    uint32_t fetch_len;          /* the blob's total length once the first page lands */
    uint8_t *interest_buf;
    uint32_t interest_cap;
    i_DartReflect refl;          /* what this peer advertises, as tables */
} i_DartNodePeerExtra;

/* The schema state for the gate and delivery: schemas interned by hash, reader views per
   (schema, topic), the decode map per (peer, topic). Flat, hook allocated, kept until close. */
typedef struct { uint64_t hash; DartSchema *parsed; } i_DartNodeSchemaIntern;
typedef struct { uint64_t hash; uint16_t topic; DartSchema *rebased; } i_DartNodeSchemaBind;
typedef struct { uint32_t peer; uint16_t topic; const DartSchema *schema; } i_DartNodePeerSchema;
#ifndef DART_NO_DIAG
/* why the gate refused (peer, topic, direction), recorded at detail intake for the event */
#define DART__SCHEMA_WHY_MAX 128
typedef struct { uint32_t peer; uint16_t topic; uint8_t peer_is_pub;
                 char text[DART__SCHEMA_WHY_MAX]; } i_DartNodeSchemaWhy;
#endif

struct i_DartNodeCore {
    DartTransportState           *transport;
    DartDiscoveryState  *discovery;   /* the peer table we delegate to */
    DartEventFn         on_event;
    void                 *user;
    int                   oob_capable;
    uint8_t               oob_host[16];
    uint8_t              *meta_buf;    /* our announce overlay, hook allocated at its actual size */
    uint16_t              meta_cap;
    uint16_t              meta_len;
    uint16_t              frag_size;   /* baked into the overlay */
    DartMetaSchema       *chan_schemas;  /* per topic schema advertisement, hash 0 = none */
    const DartSchema    **chan_compiled; /* per topic parsed schema, the gate's local side */
    uint16_t              n_topics;
    DartAllocFn           alloc;         /* required */
    void                 *alloc_user;
    uint8_t              *detail_buf;    /* detail response scratch, grown on demand */
    uint32_t              detail_cap;
    uint8_t               detail_due_any;/* some peer's request is due */
    i_DartNodeSchemaIntern *interned;     uint32_t n_interned,     cap_interned;
    i_DartNodeSchemaBind   *binds;        uint32_t n_binds,        cap_binds;
    i_DartNodePeerSchema   *peer_schemas; uint32_t n_peer_schemas, cap_peer_schemas;
    uint8_t                 fetch_details; /* observer mode */
    i_DartReflect           self;          /* this node's own channels */
    DartString              self_name;
    i_DartMeshEntity       *mesh; uint32_t n_mesh, cap_mesh;
    uint8_t                 mesh_dirty;
    uint32_t                mesh_epoch;
    void                   *scratch; uint32_t scratch_cap;   /* fold and mesh build workspace */
#ifndef DART_NO_DIAG
    i_DartNodeSchemaWhy    *schema_whys;   uint32_t n_schema_whys,   cap_schema_whys;
#endif
};

/* grow one flat array through the hook. 1 with *arr and *cap updated, else 0 */
static int i_dart_node_core_array_reserve(i_DartNodeCore *c, void **arr, uint32_t *cap,
                                          uint32_t need, size_t elem){
    void *na; uint32_t ncap;
    if (need <= *cap) return 1;
    if (!c->alloc) return 0;
    ncap = *cap ? *cap * 2u : 8u;
    if (ncap < need) ncap = need;
    na = c->alloc(c->alloc_user, *arr, (size_t)ncap * elem);
    if (!na) return 0;
    *arr = na; *cap = ncap;
    return 1;
}

uint16_t i_dart_node_core_peer_user_bytes(void){ return (uint16_t)sizeof(i_DartNodePeerExtra); }
void i_dart_node_core_bind_discovery(i_DartNodeCore *c, DartDiscoveryState *discovery){ c->discovery = discovery; }

/* the arena layout: the core struct, then the per topic schema registry. One sequence so
   measure and build agree */
static void i_dart_node_core_layout(i_DartBump *b, uint16_t n_topics,
                              i_DartNodeCore **out_c,
                              DartMetaSchema **out_schemas, const DartSchema ***out_compiled){
    i_DartNodeCore *c       = (i_DartNodeCore*)i_dart_bump_take(b, sizeof(struct i_DartNodeCore), 16);
    DartMetaSchema *schemas = (DartMetaSchema*)i_dart_bump_take(b, (size_t)n_topics*sizeof(DartMetaSchema), 16);
    const DartSchema **compiled = (const DartSchema**)i_dart_bump_take(b, (size_t)n_topics*sizeof(DartSchema*), 16);
    if (out_c)        *out_c        = c;
    if (out_schemas)  *out_schemas  = schemas;
    if (out_compiled) *out_compiled = compiled;
}

size_t i_dart_node_core_required_memory(uint16_t n_topics){
    i_DartBump b; memset(&b, 0, sizeof b);
    i_dart_node_core_layout(&b, n_topics, NULL, NULL, NULL);
    return b.offset + 16u;   /* slack to align the caller's mem up to base */
}

i_DartNodeCore *i_dart_node_core_init(void *mem, size_t cap, const i_DartNodeCoreConfig *cfg){
    i_DartBump b; i_DartNodeCore *c; uint8_t *base;
    DartMetaSchema *schemas; const DartSchema **compiled;
    if (!mem || !cfg || !cfg->transport || !cfg->alloc) return NULL;
    if (cap < i_dart_node_core_required_memory(cfg->n_topics)) return NULL;
    base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    memset(&b, 0, sizeof b); b.base = base; b.cap = cap - (size_t)(base - (uint8_t*)mem);
    i_dart_node_core_layout(&b, cfg->n_topics, &c, &schemas, &compiled);

    memset(c, 0, sizeof *c);
    c->transport     = cfg->transport;
    c->discovery     = cfg->discovery;     /* may be NULL now, bound later */
    c->on_event      = cfg->on_event;      c->user = cfg->user;
    c->alloc         = cfg->alloc;         c->alloc_user = cfg->alloc_user;
    c->oob_capable   = cfg->oob_capable;
    c->fetch_details = cfg->fetch_details;
    memcpy(c->oob_host, cfg->oob_host, 16);
    c->meta_buf      = NULL;                                /* hook allocated at the first build */
    c->meta_cap      = 0;
    c->frag_size     = cfg->frag_size;
    c->chan_schemas  = schemas;
    c->chan_compiled = compiled;
    c->n_topics    = cfg->n_topics;
    memset(schemas, 0, (size_t)cfg->n_topics * sizeof *schemas);
    memset(compiled, 0, (size_t)cfg->n_topics * sizeof *compiled);
    return c;
}

/* A struct copy carries the scalars and the stable hook allocations. The caller re
 * points the transport, discovery and blob after those move. */
i_DartNodeCore *i_dart_node_core_migrate(i_DartNodeCore *old, void *new_mem, size_t new_cap,
                                       uint16_t new_n_topics){
    i_DartBump b; i_DartNodeCore *c; uint8_t *base;
    DartMetaSchema *schemas; const DartSchema **compiled;
    uint16_t keep;
    if (!old) return NULL;
    if (new_cap < i_dart_node_core_required_memory(new_n_topics)) return NULL;
    base = (uint8_t*)(((uintptr_t)new_mem + 15u) & ~(uintptr_t)15u);
    memset(&b, 0, sizeof b); b.base = base; b.cap = new_cap - (size_t)(base - (uint8_t*)new_mem);
    i_dart_node_core_layout(&b, new_n_topics, &c, &schemas, &compiled);
    *c = *old;
    keep = old->n_topics < new_n_topics ? old->n_topics : new_n_topics;
    memset(schemas, 0, (size_t)new_n_topics * sizeof *schemas);
    memcpy(schemas, old->chan_schemas, (size_t)keep * sizeof *schemas);   /* views stay valid */
    memset(compiled, 0, (size_t)new_n_topics * sizeof *compiled);
    memcpy(compiled, old->chan_compiled, (size_t)keep * sizeof *compiled);
    c->chan_schemas  = schemas;
    c->chan_compiled = compiled;
    c->n_topics    = new_n_topics;
    return c;
}

void i_dart_node_core_set_topic_schema(i_DartNodeCore *c, uint16_t topic_index,
                                         const DartSchema *schema){
    if (!c || topic_index >= c->n_topics) return;
    c->chan_compiled[topic_index]     = schema;
    c->chan_schemas[topic_index].hash = schema ? dart_schema_hash(schema) : 0;
    c->chan_schemas[topic_index].wire = schema ? dart_schema_wire(schema) : dart_bytes(NULL, 0);
}

/* Drops the live pointers but keeps the hash as the slot's fingerprint. The responder
 * never answers a retired slot, so the stale hash is never served. */
void i_dart_node_core_retire_topic_schema(i_DartNodeCore *c, uint16_t topic_index){
    if (!c || topic_index >= c->n_topics) return;
    c->chan_compiled[topic_index]     = NULL;
    c->chan_schemas[topic_index].wire = dart_bytes(NULL, 0);
}

/* The retired slot's fingerprint, 0 = it carried no schema. */
uint64_t i_dart_node_core_topic_schema_hash(i_DartNodeCore *c, uint16_t topic_index){
    return (c && topic_index < c->n_topics) ? c->chan_schemas[topic_index].hash : 0;
}

/* the schema gate and the delivery binding */

/* A peer schema parsed once per hash. The claimed hash must equal the wire's real hash,
   or a lying peer could poison the intern for every honest one. */
static DartSchema *i_dart_node_core_intern(i_DartNodeCore *c, uint64_t hash, DartBytes wire){
    uint32_t i; DartSchema *p;
    for (i = 0; i < c->n_interned; i++)
        if (c->interned[i].hash == hash) return c->interned[i].parsed;
    if (!wire.data || wire.len == 0){
        /* identical hashes travel as zero bytes, so a schema we also hold interns from a
           fresh parse of our own wire. A retire frees ours, an interned one lives on. */
        uint16_t t;
        for (t = 0; t < c->n_topics; t++)
            if (c->chan_schemas[t].hash == hash && c->chan_schemas[t].wire.len){
                wire = c->chan_schemas[t].wire;
                break;
            }
    }
    if (!wire.data || wire.len == 0 || !c->alloc) return NULL;
    p = dart_schema_parse(wire.data, wire.len, c->alloc, c->alloc_user);
    if (!p) return NULL;
    if (dart_schema_hash(p) != hash ||
        !i_dart_node_core_array_reserve(c, (void**)&c->interned, &c->cap_interned,
                                        c->n_interned + 1u, sizeof *c->interned)){
        dart_schema_free(p, c->alloc, c->alloc_user);
        return NULL;
    }
    c->interned[c->n_interned].hash = hash;
    c->interned[c->n_interned].parsed = p;
    c->n_interned++;
    return p;
}

/* the reader view for (writer schema, topic): our fields on their layout, cached */
static DartSchema *i_dart_node_core_bind(i_DartNodeCore *c, uint64_t hash, uint16_t topic_index,
                                         const DartSchema *ours, const DartSchema *pub){
    uint32_t i; DartSchema *rb;
    for (i = 0; i < c->n_binds; i++)
        if (c->binds[i].hash == hash && c->binds[i].topic == topic_index) return c->binds[i].rebased;
    rb = dart_schema_rebase(ours, pub, c->alloc, c->alloc_user);
    if (!rb) return NULL;
    if (!i_dart_node_core_array_reserve(c, (void**)&c->binds, &c->cap_binds,
                                        c->n_binds + 1u, sizeof *c->binds)){
        dart_schema_free(rb, c->alloc, c->alloc_user);
        return NULL;
    }
    c->binds[c->n_binds].hash = hash;
    c->binds[c->n_binds].topic = topic_index;
    c->binds[c->n_binds].rebased = rb;
    c->n_binds++;
    return rb;
}

/* The delivery map. An identical schema stores this sentinel, resolved at read time, since
   our compiled copy is freed and re parsed across a retire and reuse. */
#define i_DART_NODE_SCHEMA_OURS ((const DartSchema *)(uintptr_t)1)
static void i_dart_node_core_peer_schema_set(i_DartNodeCore *c, uint32_t peer, uint16_t topic_index,
                                             const DartSchema *schema){
    uint32_t i;
    for (i = 0; i < c->n_peer_schemas; i++)
        if (c->peer_schemas[i].peer == peer && c->peer_schemas[i].topic == topic_index){
            c->peer_schemas[i].schema = schema;
            return;
        }
    if (!schema) return;
    if (!i_dart_node_core_array_reserve(c, (void**)&c->peer_schemas, &c->cap_peer_schemas,
                                        c->n_peer_schemas + 1u, sizeof *c->peer_schemas)) return;
    c->peer_schemas[c->n_peer_schemas].peer = peer;
    c->peer_schemas[c->n_peer_schemas].topic = topic_index;
    c->peer_schemas[c->n_peer_schemas].schema = schema;
    c->n_peer_schemas++;
}

/* drop a peer's map entries, on GONE or when its id is recycled */
static void i_dart_node_core_peer_schema_clear(i_DartNodeCore *c, uint32_t peer){
    uint32_t i = 0;
    while (i < c->n_peer_schemas){
        if (c->peer_schemas[i].peer == peer)
            c->peer_schemas[i] = c->peer_schemas[--c->n_peer_schemas];   /* swap remove */
        else i++;
    }
}

/* why a schema gate refused, feeding DartEvent.schema_detail */

#ifndef DART_NO_DIAG
static void i_dart_node_core_schema_why_set(i_DartNodeCore *c, uint32_t peer, uint16_t topic_index,
                                            int peer_is_pub, const char *text){
    uint32_t i; i_DartNodeSchemaWhy *w = NULL; size_t n;
    for (i = 0; i < c->n_schema_whys; i++)
        if (c->schema_whys[i].peer == peer && c->schema_whys[i].topic == topic_index &&
            c->schema_whys[i].peer_is_pub == (uint8_t)peer_is_pub){ w = &c->schema_whys[i]; break; }
    if (!w){
        if (!i_dart_node_core_array_reserve(c, (void**)&c->schema_whys, &c->cap_schema_whys,
                                            c->n_schema_whys + 1u, sizeof *c->schema_whys)) return;
        w = &c->schema_whys[c->n_schema_whys++];
        w->peer = peer; w->topic = topic_index; w->peer_is_pub = (uint8_t)peer_is_pub;
    }
    n = 0;
    while (text[n] && n < DART__SCHEMA_WHY_MAX - 1u){ w->text[n] = text[n]; n++; }
    w->text[n] = '\0';
}
static void i_dart_node_core_schema_why_drop(i_DartNodeCore *c, uint32_t peer, uint16_t topic_index,
                                             int peer_is_pub){
    uint32_t i = 0;
    while (i < c->n_schema_whys){
        if (c->schema_whys[i].peer == peer && c->schema_whys[i].topic == topic_index &&
            c->schema_whys[i].peer_is_pub == (uint8_t)peer_is_pub)
            c->schema_whys[i] = c->schema_whys[--c->n_schema_whys];   /* swap remove */
        else i++;
    }
}
static void i_dart_node_core_schema_why_clear(i_DartNodeCore *c, uint32_t peer){
    uint32_t i = 0;
    while (i < c->n_schema_whys){
        if (c->schema_whys[i].peer == peer)
            c->schema_whys[i] = c->schema_whys[--c->n_schema_whys];
        else i++;
    }
}
const char *i_dart_node_core_schema_why(i_DartNodeCore *c, uint32_t peer, uint16_t topic_index,
                                        int peer_is_pub){
    uint32_t i;
    if (!c) return NULL;
    for (i = 0; i < c->n_schema_whys; i++)
        if (c->schema_whys[i].peer == peer && c->schema_whys[i].topic == topic_index &&
            c->schema_whys[i].peer_is_pub == (uint8_t)peer_is_pub)
            return c->schema_whys[i].text;
    return NULL;
}
const char *i_dart_node_core_note_size_mismatch(i_DartNodeCore *c, uint32_t peer,
                                        uint16_t topic_index, uint64_t got_len, uint64_t want_len){
    char text[DART__SCHEMA_WHY_MAX];
    char *p = text, *end = text + sizeof text - 1;
    if (!c) return NULL;
    p = i_dart_event_append_str(p, end, "message is ");
    p = i_dart_event_append_u64(p, end, got_len);
    p = i_dart_event_append_str(p, end, " bytes, the publisher's schema says ");
    p = i_dart_event_append_u64(p, end, want_len);
    *p = '\0';
    i_dart_node_core_schema_why_set(c, peer, topic_index, 1, text);
    return i_dart_node_core_schema_why(c, peer, topic_index, 1);
}
#else
static void i_dart_node_core_schema_why_clear(i_DartNodeCore *c, uint32_t peer){
    (void)c; (void)peer;
}
const char *i_dart_node_core_schema_why(i_DartNodeCore *c, uint32_t peer, uint16_t topic_index,
                                        int peer_is_pub){
    (void)c; (void)peer; (void)topic_index; (void)peer_is_pub; return NULL;
}
const char *i_dart_node_core_note_size_mismatch(i_DartNodeCore *c, uint32_t peer,
                                        uint16_t topic_index, uint64_t got_len, uint64_t want_len){
    (void)c; (void)peer; (void)topic_index; (void)got_len; (void)want_len; return NULL;
}
#endif

const DartSchema *i_dart_node_core_msg_schema(i_DartNodeCore *c, uint32_t peer, uint16_t topic_index){
    uint32_t i;
    if (!c) return NULL;
    for (i = 0; i < c->n_peer_schemas; i++)
        if (c->peer_schemas[i].peer == peer && c->peer_schemas[i].topic == topic_index){
            const DartSchema *s = c->peer_schemas[i].schema;
            if (s == i_DART_NODE_SCHEMA_OURS)   /* an identical schema entry: resolve live */
                return topic_index < c->n_topics ? c->chan_compiled[topic_index] : NULL;
            return s;
        }
    return NULL;
}

/* The slot was rebound with a retype: every binding, view and refusal reason recorded for
 * the old occupant is stale. The verdicts re pended in the transport, so they re derive. */
void i_dart_node_core_topic_rebound(i_DartNodeCore *c, uint16_t topic_index){
    uint32_t i;
    if (!c) return;
    i = 0;
    while (i < c->n_peer_schemas){
        if (c->peer_schemas[i].topic == topic_index)
            c->peer_schemas[i] = c->peer_schemas[--c->n_peer_schemas];   /* swap remove */
        else i++;
    }
    i = 0;
    while (i < c->n_binds){
        if (c->binds[i].topic == topic_index){
            if (c->binds[i].rebased) dart_schema_free(c->binds[i].rebased, c->alloc, c->alloc_user);
            c->binds[i] = c->binds[--c->n_binds];
        } else i++;
    }
#ifndef DART_NO_DIAG
    i = 0;
    while (i < c->n_schema_whys){
        if (c->schema_whys[i].topic == topic_index)
            c->schema_whys[i] = c->schema_whys[--c->n_schema_whys];
        else i++;
    }
#endif
}

/* the reflection tables */

static DartBytes i_dart_node_core_interest_of(i_DartNodePeerExtra *ex, DartBytes meta,
                            uint32_t meta_version);

static uint64_t i_dart_node_core_fnv(uint64_t h, const void *p, size_t n){
    const uint8_t *b = (const uint8_t*)p; size_t i;
    for (i = 0; i < n; i++){ h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

static void i_dart_reflect_free(i_DartNodeCore *c, i_DartReflect *r){
    if (!c->alloc) return;
    if (r->chan)  c->alloc(c->alloc_user, r->chan, 0);
    if (r->ent)   c->alloc(c->alloc_user, r->ent, 0);
    if (r->names) c->alloc(c->alloc_user, r->names, 0);
    memset(r, 0, sizeof *r);
}

static void i_dart_node_core_mesh_bump(i_DartNodeCore *c){
    c->mesh_dirty = 1;
    c->mesh_epoch++;
}

/* the channel record for index, growing the dense table with new slots absent */
static i_DartChannel *i_dart_reflect_channel(i_DartNodeCore *c, i_DartReflect *r, uint16_t index){
    if ((uint32_t)index >= r->n_chan){
        uint32_t i, need = (uint32_t)index + 1u;
        if (!i_dart_node_core_array_reserve(c, (void**)&r->chan, &r->cap_chan, need, sizeof *r->chan))
            return NULL;
        for (i = r->n_chan; i < need; i++){
            memset(&r->chan[i], 0, sizeof r->chan[i]);
            r->chan[i].name_off = I_DART_NAME_NONE;
            r->chan[i].entity = I_DART_NONE16;
        }
        r->n_chan = need;
    }
    return &r->chan[index];
}

/* appends a NUL terminated name copy and returns its offset, NAME_NONE on OOM */
static uint32_t i_dart_reflect_name(i_DartNodeCore *c, i_DartReflect *r, DartString name){
    uint32_t off = r->names_len, need = r->names_len + (uint32_t)name.len + 1u;
    if (!i_dart_node_core_array_reserve(c, (void**)&r->names, &r->names_cap, need, 1)) return I_DART_NAME_NONE;
    memcpy(r->names + off, name.data, name.len);
    r->names[off + name.len] = '\0';
    r->names_len = need;
    return off;
}

/* an interest apply: kind, role, reliability and hash per advertised index. Absent slots clear */
static void i_dart_node_core_reflect_interest(i_DartNodeCore *c, i_DartNodePeerExtra *ex,
                                              DartBytes interest){
    i_DartReflect *r = &ex->refl;
    DartInterestIter it; DartTopicEntry e; uint32_t i;
    if (!interest.data) return;
    for (i = 0; i < r->n_chan; i++) r->chan[i].present = 0;
    memset(&it, 0, sizeof it);
    while (dart_interest_next(interest, &it, &e)){
        i_DartChannel *ch = i_dart_reflect_channel(c, r, e.index);
        if (!ch) return;
        ch->hash = e.hash; ch->kind = e.kind; ch->role = e.role;
        ch->reliable = e.reliable; ch->present = 1;
    }
    r->dirty = 1;
}

/* a detail entry: the name, the attrs and the interned schema for one index */
static void i_dart_node_core_reflect_detail(i_DartNodeCore *c, i_DartNodePeerExtra *ex,
                                            const DartDetail *d){
    i_DartReflect *r = &ex->refl;
    i_DartChannel *ch;
    if (d->name.len == 0 || d->name.len > DART_TOPIC_NAME_MAX) return;
    ch = i_dart_reflect_channel(c, r, d->index);
    if (!ch) return;
    if (ch->name_off == I_DART_NAME_NONE){
        ch->name_off = i_dart_reflect_name(c, r, d->name);
        if (ch->name_off == I_DART_NAME_NONE) return;
        ch->name_len = (uint8_t)d->name.len;
    }
    ch->attrs = d->attrs;
    ch->schema_hash = d->schema_hash;
    ch->schema = d->schema_hash ? i_dart_node_core_intern(c, d->schema_hash, d->schema_wire) : NULL;
    r->dirty = 1;
}

static void i_dart_reflect_format_addr(i_DartReflect *r, const DartDiscoveryAddr *a){
    char *p = r->addr; unsigned i;
    if (a->ip_len == 4){
        for (i = 0; i < 4; i++){
            unsigned v = a->ip[i]; char t[4]; int k = 0;
            do { t[k++] = (char)('0' + v % 10u); v /= 10u; } while (v);
            while (k) *p++ = t[--k];
            if (i < 3) *p++ = '.';
        }
    } else if (a->ip_len == 16){
        static const char hx[] = "0123456789abcdef";
        *p++ = '[';
        for (i = 0; i < 16; i += 2){
            *p++ = hx[a->ip[i] >> 4]; *p++ = hx[a->ip[i] & 15];
            *p++ = hx[a->ip[i+1] >> 4]; *p++ = hx[a->ip[i+1] & 15];
            if (i < 14) *p++ = ':';
        }
        *p++ = ']';
    }
    {   unsigned v = a->port; char t[6]; int k = 0;
        *p++ = ':';
        do { t[k++] = (char)('0' + v % 10u); v /= 10u; } while (v);
        while (k) *p++ = t[--k];
    }
    r->addr_len = (uint8_t)(p - r->addr);
}

/* the fold: channels to entities */

/* one row per DartTopicKind: its entity, whether it is the primary channel, its name
   suffix, and which role is the provider side */
typedef struct { uint8_t entity, primary, provider_pubs; const char *suffix; } i_DartKindRow;
static const i_DartKindRow i_dart_kind_rows[8] = {
    { DART_ENTITY_TOPIC,    1, 1, ""     },   /* TOPIC    */
    { DART_ENTITY_FUNCTION, 1, 0, "@req" },   /* FUNC_REQ */
    { DART_ENTITY_FUNCTION, 0, 1, "@rsp" },   /* FUNC_RSP */
    { DART_ENTITY_VARIABLE, 1, 1, ""     },   /* VARIABLE */
    { DART_ENTITY_VARIABLE, 0, 0, "@set" },   /* VAR_SET  */
    { DART_ENTITY_TASK,     1, 0, "@req" },   /* TASK_REQ */
    { DART_ENTITY_TASK,     0, 1, "@prg" },   /* TASK_PRG */
    { DART_ENTITY_TASK,     0, 1, "@rsp" }    /* TASK_RSP */
};
static const i_DartKindRow *i_dart_kind_row(uint8_t kind){
    return kind < 8 ? &i_dart_kind_rows[kind] : &i_dart_kind_rows[0];
}

static int i_dart_reflect_hidden(const char *name, size_t len){
    return len >= 6 && memcmp(name, "@dart/", 6) == 0;
}
static int i_dart_reflect_hidden_hash(uint32_t h){
    static const char *const nm[] = { "@dart/log/error", "@dart/log/warn", "@dart/log/info",
                                      "@dart/meta@req", "@dart/meta@rsp" };
    size_t i;
    for (i = 0; i < sizeof nm / sizeof nm[0]; i++)
        if (h == (uint32_t)dart_topic_id(nm[i])) return 1;
    return 0;
}

typedef struct { uint32_t hash; uint16_t index; } i_DartHashPair;

static void *i_dart_node_core_scratch(i_DartNodeCore *c, uint32_t bytes){
    if (!i_dart_node_core_array_reserve(c, &c->scratch, &c->scratch_cap, bytes, 1)) return NULL;
    return c->scratch;
}

static void i_dart_hash_sort(i_DartHashPair *a, uint32_t n){   /* shell sort, no libc */
    uint32_t gap, i, j;
    for (gap = n / 2u; gap > 0; gap /= 2u)
        for (i = gap; i < n; i++){
            i_DartHashPair v = a[i];
            for (j = i; j >= gap && (a[j-gap].hash > v.hash
                                     || (a[j-gap].hash == v.hash && a[j-gap].index > v.index)); j -= gap)
                a[j] = a[j-gap];
            a[j] = v;
        }
}

/* the channel of kind whose name hashes like base plus suffix, NONE16 when absent */
static uint16_t i_dart_reflect_partner(const i_DartReflect *r, const i_DartHashPair *sorted,
                                       uint32_t n, const char *base, size_t base_len,
                                       const char *suffix, uint8_t kind){
    char buf[DART_TOPIC_NAME_MAX + 8]; size_t sl = strlen(suffix);
    uint32_t want, lo = 0, hi = n;
    if (base_len + sl > DART_TOPIC_NAME_MAX) return I_DART_NONE16;
    memcpy(buf, base, base_len); memcpy(buf + base_len, suffix, sl + 1);
    want = (uint32_t)dart_topic_id(buf);
    while (lo < hi){ uint32_t mid = (lo + hi) / 2u; if (sorted[mid].hash < want) lo = mid + 1; else hi = mid; }
    for (; lo < n && sorted[lo].hash == want; lo++){
        const i_DartChannel *ch = &r->chan[sorted[lo].index];
        if (ch->kind != kind) continue;
        /* a fetched partner name must really be base plus suffix, 32 bit hashes collide */
        if (ch->name_off != I_DART_NAME_NONE
            && (ch->name_len != base_len + sl || memcmp(r->names + ch->name_off, buf, base_len + sl) != 0))
            continue;
        return sorted[lo].index;
    }
    return I_DART_NONE16;
}

static i_DartPeerEntity *i_dart_reflect_entity_new(i_DartNodeCore *c, i_DartReflect *r){
    i_DartPeerEntity *e;
    if (!i_dart_node_core_array_reserve(c, (void**)&r->ent, &r->cap_ent, r->n_ent + 1u, sizeof *r->ent))
        return NULL;
    e = &r->ent[r->n_ent++];
    memset(e, 0, sizeof *e);
    e->primary = e->rsp = e->prg = e->set = I_DART_NONE16;
    return e;
}

/* attaches channel idx as the entity's partner of its kind */
static void i_dart_reflect_attach(i_DartReflect *r, i_DartPeerEntity *e, uint16_t idx, uint16_t slot){
    i_DartChannel *ch = &r->chan[idx];
    ch->entity = slot;
    switch (ch->kind){
        case DART_KIND_FUNC_RSP: case DART_KIND_TASK_RSP: e->rsp = idx; break;
        case DART_KIND_TASK_PRG: e->prg = idx; break;
        case DART_KIND_VAR_SET:  e->set = idx; break;
        default: break;
    }
}

static void i_dart_reflect_set_name(i_DartPeerEntity *e, const i_DartReflect *r, uint32_t off, size_t len){
    char buf[DART_TOPIC_NAME_MAX + 1];
    e->name_off = off; e->name_len = (uint8_t)len;
    memcpy(buf, r->names + off, len); buf[len] = '\0';
    e->id = dart_topic_id(buf);
}

static void i_dart_reflect_fold(i_DartNodeCore *c, i_DartReflect *r){
    i_DartHashPair *sorted; uint32_t n = 0, i;
    r->dirty = 0;
    r->n_ent = 0;
    for (i = 0; i < r->n_chan; i++) r->chan[i].entity = I_DART_NONE16;
    sorted = (i_DartHashPair*)i_dart_node_core_scratch(c, r->n_chan * (uint32_t)sizeof *sorted + 1u);
    if (!sorted) return;
    for (i = 0; i < r->n_chan; i++)
        if (r->chan[i].present){ sorted[n].hash = r->chan[i].hash; sorted[n].index = (uint16_t)i; n++; }
    i_dart_hash_sort(sorted, n);
    /* pass 1: primaries open entities and claim their partners */
    for (i = 0; i < r->n_chan; i++){
        i_DartChannel *ch = &r->chan[i];
        const i_DartKindRow *row;
        i_DartPeerEntity *e;
        const char *nm; size_t nl;
        uint16_t slot, p;
        if (!ch->present || ch->kind >= 8 || i_dart_reflect_hidden_hash(ch->hash)) continue;
        row = i_dart_kind_row(ch->kind);
        if (!row->primary) continue;
        if (ch->name_off != I_DART_NAME_NONE
            && i_dart_reflect_hidden(r->names + ch->name_off, ch->name_len)) continue;
        e = i_dart_reflect_entity_new(c, r);
        if (!e) return;
        slot = (uint16_t)(r->n_ent - 1u);
        e->kind = row->entity; e->primary = (uint16_t)i; e->hash = ch->hash;
        ch->entity = slot;
        if (ch->name_off == I_DART_NAME_NONE){
            if (row->suffix[0]) e->incomplete = 1;   /* partners need the name */
            continue;
        }
        nm = r->names + ch->name_off; nl = ch->name_len;
        if (row->suffix[0]){
            size_t sl = strlen(row->suffix);
            if (nl > sl && memcmp(nm + nl - sl, row->suffix, sl) == 0) nl -= sl;
            else e->incomplete = 1;                  /* a pattern kind without the convention */
        }
        i_dart_reflect_set_name(e, r, ch->name_off, nl);
        if (e->incomplete) continue;
        switch (ch->kind){
        case DART_KIND_FUNC_REQ:
            p = i_dart_reflect_partner(r, sorted, n, nm, nl, "@rsp", DART_KIND_FUNC_RSP);
            if (p != I_DART_NONE16) i_dart_reflect_attach(r, e, p, slot); else e->incomplete = 1;
            break;
        case DART_KIND_TASK_REQ:
            p = i_dart_reflect_partner(r, sorted, n, nm, nl, "@rsp", DART_KIND_TASK_RSP);
            if (p != I_DART_NONE16) i_dart_reflect_attach(r, e, p, slot); else e->incomplete = 1;
            p = i_dart_reflect_partner(r, sorted, n, nm, nl, "@prg", DART_KIND_TASK_PRG);
            if (p != I_DART_NONE16) i_dart_reflect_attach(r, e, p, slot); else e->incomplete = 1;
            break;
        case DART_KIND_VARIABLE:
            p = i_dart_reflect_partner(r, sorted, n, nm, nl, "@set", DART_KIND_VAR_SET);
            if (p != I_DART_NONE16) i_dart_reflect_attach(r, e, p, slot);
            break;
        default: break;
        }
    }
    /* pass 2: an unclaimed partner is a half pair or an unknown kind, surfaced incomplete */
    for (i = 0; i < r->n_chan; i++){
        i_DartChannel *ch = &r->chan[i];
        const i_DartKindRow *row;
        i_DartPeerEntity *e;
        uint16_t slot;
        if (!ch->present || ch->entity != I_DART_NONE16 || i_dart_reflect_hidden_hash(ch->hash)) continue;
        if (ch->name_off != I_DART_NAME_NONE
            && i_dart_reflect_hidden(r->names + ch->name_off, ch->name_len)) continue;
        row = i_dart_kind_row(ch->kind);
        e = i_dart_reflect_entity_new(c, r);
        if (!e) return;
        slot = (uint16_t)(r->n_ent - 1u);
        e->kind = (ch->kind < 8) ? row->entity : DART_ENTITY_TOPIC;
        e->hash = ch->hash; e->incomplete = 1;
        if (ch->kind < 8 && !row->primary) i_dart_reflect_attach(r, e, (uint16_t)i, slot);
        else { e->primary = (uint16_t)i; ch->entity = slot; }
        if (ch->name_off != I_DART_NAME_NONE){
            const char *nm = r->names + ch->name_off; size_t nl = ch->name_len;
            size_t sl = ch->kind < 8 ? strlen(row->suffix) : 0;
            if (sl && nl > sl && memcmp(nm + nl - sl, row->suffix, sl) == 0) nl -= sl;
            i_dart_reflect_set_name(e, r, ch->name_off, nl);
        }
    }
}

/* the reflection record behind a peer id, DART_SELF is this node, NULL if unknown */
static i_DartReflect *i_dart_node_core_reflect_of(i_DartNodeCore *c, uint32_t peer){
    i_DartNodePeerExtra *ex;
    if (peer == DART_SELF) return &c->self;
    ex = c->discovery ? (i_DartNodePeerExtra*)dart_discovery_peer_user(c->discovery, peer) : NULL;
    return (ex && ex->added) ? &ex->refl : NULL;
}

/* the channel of an entity that which names: 0 primary, 1 rsp, 2 prg */
static const i_DartChannel *i_dart_reflect_which(const i_DartReflect *r, const i_DartPeerEntity *e, int which){
    uint16_t idx = which == 1 ? e->rsp : which == 2 ? e->prg : e->primary;
    return idx != I_DART_NONE16 ? &r->chan[idx] : NULL;
}

/* the channel that says which side a node is on: the primary, else whichever partner exists */
static const i_DartChannel *i_dart_reflect_side(const i_DartReflect *r, const i_DartPeerEntity *e){
    if (e->primary != I_DART_NONE16) return &r->chan[e->primary];
    if (e->rsp != I_DART_NONE16) return &r->chan[e->rsp];
    if (e->prg != I_DART_NONE16) return &r->chan[e->prg];
    if (e->set != I_DART_NONE16) return &r->chan[e->set];
    return NULL;
}

static uint64_t i_dart_reflect_generation(const uint8_t uuid[16], const i_DartReflect *r,
                                          const i_DartPeerEntity *e){
    uint64_t h = 1469598103934665603ull;
    const i_DartChannel *p = e->primary != I_DART_NONE16 ? &r->chan[e->primary] : NULL;
    uint64_t hs = p ? p->schema_hash : 0;
    uint64_t hr = e->rsp != I_DART_NONE16 ? r->chan[e->rsp].schema_hash : 0;
    uint64_t hp = e->prg != I_DART_NONE16 ? r->chan[e->prg].schema_hash : 0;
    uint8_t attrs = p ? p->attrs : 0, set = (uint8_t)(e->set != I_DART_NONE16);
    if (uuid) h = i_dart_node_core_fnv(h, uuid, 16);
    h = i_dart_node_core_fnv(h, &hs, sizeof hs);
    h = i_dart_node_core_fnv(h, &hr, sizeof hr);
    h = i_dart_node_core_fnv(h, &hp, sizeof hp);
    h = i_dart_node_core_fnv(h, &attrs, 1);
    h = i_dart_node_core_fnv(h, &set, 1);
    h = i_dart_node_core_fnv(h, &e->kind, 1);
    return h;
}

/* the uuid behind a peer id, NULL if unknown. DART_SELF is our own */
static const uint8_t *i_dart_node_core_uuid_of(i_DartNodeCore *c, uint32_t peer, DartDiscoveryPeer *scratch){
    uint16_t q, np;
    if (!c->discovery) return NULL;
    if (peer == DART_SELF) return dart_discovery_uuid(c->discovery);
    np = dart_discovery_max_peers(c->discovery);
    for (q = 0; q < np; q++)
        if (dart_discovery_peer_at(c->discovery, q, scratch) && scratch->id == peer) return scratch->uuid;
    return NULL;
}

/* fills the public view of one entity slot at one node */
static void i_dart_reflect_info(i_DartNodeCore *c, uint32_t peer, const i_DartReflect *r,
                                uint16_t slot, DartEntityInfo *out){
    const i_DartPeerEntity *e = &r->ent[slot];
    const i_DartChannel *p = e->primary != I_DART_NONE16 ? &r->chan[e->primary] : NULL;
    const i_DartChannel *side = i_dart_reflect_side(r, e);
    DartDiscoveryPeer scratch;
    memset(out, 0, sizeof *out);
    out->kind = (DartEntityKind)e->kind;
    out->name = e->name_len ? dart_string(r->names + e->name_off, e->name_len) : dart_string(NULL, 0);
    out->hash = e->hash;
    out->incomplete = e->incomplete;
    if (side){
        const i_DartKindRow *row = i_dart_kind_row(side->kind);
        int pubs = dart_role_pubs(side->role), subs = dart_role_subs(side->role);
        out->provides = (uint8_t)(row->provider_pubs ? pubs : subs);
        out->consumes = (uint8_t)(row->provider_pubs ? subs : pubs);
        out->reliable = side->reliable;
    }
    if (p){
        out->schema = p->schema; out->schema_hash = p->schema_hash;
        out->forceable   = (uint8_t)((p->attrs & DART_ATTR_FORCEABLE)   ? 1 : 0);
        out->cancellable = (uint8_t)((p->attrs & DART_ATTR_CANCELLABLE) ? 1 : 0);
        out->exclusive   = (uint8_t)((p->attrs & DART_ATTR_EXCLUSIVE)   ? 1 : 0);
        out->multi       = (uint8_t)((p->attrs & DART_ATTR_MULTI)       ? 1 : 0);
    }
    if (e->rsp != I_DART_NONE16){ out->rsp_schema = r->chan[e->rsp].schema; out->rsp_schema_hash = r->chan[e->rsp].schema_hash; }
    if (e->prg != I_DART_NONE16){ out->progress_schema = r->chan[e->prg].schema; out->progress_schema_hash = r->chan[e->prg].schema_hash; }
    out->writable = (uint8_t)(e->kind == DART_ENTITY_VARIABLE && e->set != I_DART_NONE16);
    out->providers = out->provides; out->consumers = out->consumes;
    out->provider = peer;
    out->from = peer == DART_SELF ? c->self_name : i_dart_node_core_peer_name(c, peer);
    out->generation = i_dart_reflect_generation(i_dart_node_core_uuid_of(c, peer, &scratch), r, e);
}

/* the mesh table */

typedef struct {
    uint64_t id; uint64_t last_heard; uint64_t schema_hash; const DartSchema *schema;
    uint32_t peer; uint16_t slot; uint8_t kind, provides, consumes;
} i_DartMeshRec;

static int i_dart_mesh_rec_less(const i_DartMeshRec *a, const i_DartMeshRec *b){
    if (a->kind != b->kind) return a->kind < b->kind;
    if (a->id != b->id) return a->id < b->id;
    return a->peer < b->peer;
}
static void i_dart_mesh_sort(i_DartMeshRec *a, uint32_t n){
    uint32_t gap, i, j;
    for (gap = n / 2u; gap > 0; gap /= 2u)
        for (i = gap; i < n; i++){
            i_DartMeshRec v = a[i];
            for (j = i; j >= gap && i_dart_mesh_rec_less(&v, &a[j-gap]); j -= gap) a[j] = a[j-gap];
            a[j] = v;
        }
}

/* one node's entities as records. last_heard orders rival providers by freshness */
static uint32_t i_dart_mesh_gather(i_DartNodeCore *c, i_DartMeshRec *recs, uint32_t n, uint32_t cap,
                                   uint32_t peer, i_DartReflect *r, uint64_t last_heard){
    uint32_t i;
    if (r->dirty) i_dart_reflect_fold(c, r);
    for (i = 0; i < r->n_ent && n < cap; i++){
        const i_DartPeerEntity *e = &r->ent[i];
        const i_DartChannel *side = i_dart_reflect_side(r, e);
        const i_DartKindRow *row;
        i_DartMeshRec *m;
        if (!e->id || !side) continue;               /* unnamed yet: cannot key it */
        row = i_dart_kind_row(side->kind);
        m = &recs[n++];
        m->id = e->id; m->kind = e->kind; m->peer = peer; m->slot = (uint16_t)i;
        m->last_heard = last_heard;
        m->provides = (uint8_t)(row->provider_pubs ? dart_role_pubs(side->role) : dart_role_subs(side->role));
        m->consumes = (uint8_t)(row->provider_pubs ? dart_role_subs(side->role) : dart_role_pubs(side->role));
        m->schema = e->primary != I_DART_NONE16 ? r->chan[e->primary].schema : NULL;
        m->schema_hash = e->primary != I_DART_NONE16 ? r->chan[e->primary].schema_hash : 0;
    }
    return n;
}

static void i_dart_node_core_mesh_build(i_DartNodeCore *c){
    i_DartMeshRec *recs; uint32_t total = c->self.n_ent + 16u, n = 0, i;
    uint16_t s, np = c->discovery ? dart_discovery_max_peers(c->discovery) : 0;
    c->mesh_dirty = 0;
    c->n_mesh = 0;
    if (c->self.dirty) i_dart_reflect_fold(c, &c->self);
    total = c->self.n_ent + 16u;
    for (s = 0; s < np; s++){
        DartDiscoveryPeer v; i_DartNodePeerExtra *ex;
        if (!dart_discovery_peer_at(c->discovery, s, &v) || v.liveness != DART_PEER_ACTIVE) continue;
        ex = (i_DartNodePeerExtra*)v.user;
        if (!ex || !ex->added) continue;
        if (ex->refl.dirty) i_dart_reflect_fold(c, &ex->refl);
        total += ex->refl.n_ent;
    }
    recs = (i_DartMeshRec*)i_dart_node_core_scratch(c, total * (uint32_t)sizeof *recs);
    if (!recs) return;
    n = i_dart_mesh_gather(c, recs, n, total, DART_SELF, &c->self, ~(uint64_t)0);
    for (s = 0; s < np; s++){
        DartDiscoveryPeer v; i_DartNodePeerExtra *ex;
        if (!dart_discovery_peer_at(c->discovery, s, &v) || v.liveness != DART_PEER_ACTIVE) continue;
        ex = (i_DartNodePeerExtra*)v.user;
        if (!ex || !ex->added) continue;
        n = i_dart_mesh_gather(c, recs, n, total, v.id, &ex->refl, v.last_heard_us);
    }
    i_dart_mesh_sort(recs, n);
    if (!i_dart_node_core_array_reserve(c, (void**)&c->mesh, &c->cap_mesh, n ? n : 1u, sizeof *c->mesh)) return;
    for (i = 0; i < n; ){
        i_DartMeshEntity *m = &c->mesh[c->n_mesh++];
        const i_DartMeshRec *pick = NULL, *first_consumer = NULL;
        uint32_t j, k;
        memset(m, 0, sizeof *m);
        m->id = recs[i].id; m->kind = recs[i].kind;
        for (j = i; j < n && recs[j].kind == m->kind && recs[j].id == m->id; j++){
            const i_DartMeshRec *rec = &recs[j];
            if (rec->provides){
                /* the first live provider takes the slot. A rival with a different schema
                   is a conflict and wins only if heard more recently */
                m->providers++;
                if (!pick) pick = rec;
                else if (rec->schema_hash != pick->schema_hash){
                    m->conflict = 1;
                    if (rec->last_heard > pick->last_heard) pick = rec;
                }
            }
            if (rec->consumes){ m->consumers++; if (!first_consumer) first_consumer = rec; }
        }
        if (pick){ m->has_provider = 1; m->provider = pick->peer; m->provider_slot = pick->slot; }
        else if (first_consumer){
            /* no live provider: the widest consumer declaration stands in */
            pick = first_consumer;
            for (k = i; k < j; k++){
                const i_DartMeshRec *rec = &recs[k];
                if (!rec->consumes || !rec->schema) continue;
                if (!pick->schema || (rec->schema_hash != pick->schema_hash
                                      && dart_schema_subset(pick->schema, rec->schema))) pick = rec;
            }
        }
        if (pick){
            m->from_peer = pick->peer; m->from_slot = pick->slot;
            /* a live endpoint that can neither read the pick nor be read by it is a conflict */
            for (k = i; k < j; k++){
                const i_DartMeshRec *rec = &recs[k];
                if (rec == pick || rec->schema_hash == pick->schema_hash || !rec->schema || !pick->schema) continue;
                if (!dart_schema_subset(rec->schema, pick->schema) && !dart_schema_subset(pick->schema, rec->schema))
                    m->conflict = 1;
            }
        }
        {   i_DartReflect *r = i_dart_node_core_reflect_of(c, m->from_peer);
            DartDiscoveryPeer scratch;
            const uint8_t *uuid = m->has_provider ? i_dart_node_core_uuid_of(c, m->provider, &scratch) : NULL;
            m->generation = r ? i_dart_reflect_generation(uuid, r, &r->ent[m->from_slot]) : 0;
        }
        i = j;
    }
}

static void i_dart_mesh_info(i_DartNodeCore *c, const i_DartMeshEntity *m, DartEntityInfo *out){
    i_DartReflect *r = i_dart_node_core_reflect_of(c, m->from_peer);
    if (!r){ memset(out, 0, sizeof *out); return; }
    i_dart_reflect_info(c, m->from_peer, r, m->from_slot, out);
    out->providers = m->providers; out->consumers = m->consumers;
    out->provides = (uint8_t)(m->providers > 0); out->consumes = (uint8_t)(m->consumers > 0);
    out->provider = m->has_provider ? m->provider : 0;
    out->conflict = m->conflict;
    out->generation = m->generation;
}

/* the seams the runtime's walks call, with the node lock held by the caller */

void i_dart_node_core_set_self_name(i_DartNodeCore *c, DartString name){ if (c) c->self_name = name; }

void i_dart_node_core_self_begin(i_DartNodeCore *c){
    uint32_t i;
    if (!c) return;
    for (i = 0; i < c->self.n_chan; i++) c->self.chan[i].present = 0;
    c->self.names_len = 0;
}
void i_dart_node_core_self_channel(i_DartNodeCore *c, uint16_t index, DartString name, uint8_t kind,
                                   uint8_t role, uint8_t reliable, uint8_t attrs, const DartSchema *schema){
    i_DartChannel *ch;
    char buf[DART_TOPIC_NAME_MAX + 1];
    if (!c || name.len == 0 || name.len > DART_TOPIC_NAME_MAX) return;
    ch = i_dart_reflect_channel(c, &c->self, index);
    if (!ch) return;
    memcpy(buf, name.data, name.len); buf[name.len] = '\0';
    ch->hash = (uint32_t)dart_topic_id(buf);
    ch->name_off = i_dart_reflect_name(c, &c->self, name);
    ch->name_len = (uint8_t)name.len;
    ch->kind = kind; ch->role = role; ch->reliable = reliable; ch->attrs = attrs;
    ch->schema = schema; ch->schema_hash = schema ? dart_schema_hash(schema) : 0;
    ch->present = (uint8_t)(role != DART_INACTIVE && ch->name_off != I_DART_NAME_NONE);
}
void i_dart_node_core_self_end(i_DartNodeCore *c){
    if (!c) return;
    c->self.dirty = 1;
    i_dart_node_core_mesh_bump(c);
}

int i_dart_node_core_peers_next(i_DartNodeCore *c, DartIter *it, DartPeerInfo *out){
    uint16_t np;
    if (!c || !c->discovery || !it || !out) return 0;
    np = dart_discovery_max_peers(c->discovery);
    for (; it->a < np; it->a++){
        DartDiscoveryPeer v; i_DartNodePeerExtra *ex;
        if (!dart_discovery_peer_at(c->discovery, (uint16_t)it->a, &v)) continue;
        ex = (i_DartNodePeerExtra*)v.user;
        it->a++;
        memset(out, 0, sizeof *out);
        out->id = v.id;
        memcpy(out->uuid, v.uuid, 16);
        out->name = i_dart_node_core_peer_name(c, v.id);
        out->address = ex ? dart_string(ex->refl.addr, ex->refl.addr_len) : dart_string(NULL, 0);
        out->liveness = v.liveness;
        out->last_heard_us = v.last_heard_us;
        out->epoch = ex ? ex->interest_epoch : 0;
        out->catching_up = (uint8_t)(v.adv_meta_version > v.meta_version);
        out->fragment_size = v.meta.data ? dart_meta_frag(v.meta) : 0;
        return 1;
    }
    return 0;
}

int i_dart_node_core_entities_next(i_DartNodeCore *c, uint32_t peer, DartIter *it, DartEntityInfo *out){
    i_DartReflect *r;
    if (!c || !it || !out) return 0;
    r = i_dart_node_core_reflect_of(c, peer);
    if (!r) return 0;
    if (r->dirty) i_dart_reflect_fold(c, r);
    if (it->a >= r->n_ent) return 0;
    i_dart_reflect_info(c, peer, r, (uint16_t)it->a, out);
    it->a++;
    return 1;
}

int i_dart_node_core_mesh_next(i_DartNodeCore *c, DartIter *it, DartEntityInfo *out){
    if (!c || !it || !out) return 0;
    if (c->mesh_dirty) i_dart_node_core_mesh_build(c);
    if (it->a >= c->n_mesh) return 0;
    i_dart_mesh_info(c, &c->mesh[it->a], out);
    it->a++;
    return 1;
}

static const i_DartMeshEntity *i_dart_node_core_mesh_lookup(i_DartNodeCore *c, uint8_t kind, uint64_t id){
    uint32_t lo = 0, hi;
    if (c->mesh_dirty) i_dart_node_core_mesh_build(c);
    hi = c->n_mesh;
    while (lo < hi){
        uint32_t mid = (lo + hi) / 2u;
        const i_DartMeshEntity *m = &c->mesh[mid];
        if (m->kind < kind || (m->kind == kind && m->id < id)) lo = mid + 1; else hi = mid;
    }
    return (lo < c->n_mesh && c->mesh[lo].kind == kind && c->mesh[lo].id == id) ? &c->mesh[lo] : NULL;
}

int i_dart_node_core_mesh_find(i_DartNodeCore *c, DartEntityKind kind, const char *name, DartEntityInfo *out){
    const i_DartMeshEntity *m;
    if (!c || !name || !out) return 0;
    m = i_dart_node_core_mesh_lookup(c, (uint8_t)kind, dart_topic_id(name));
    if (!m) return 0;
    i_dart_mesh_info(c, m, out);
    return 1;
}

uint32_t i_dart_node_core_mesh_epoch(i_DartNodeCore *c){ return c ? c->mesh_epoch : 0; }

int i_dart_node_core_reflect_pick(i_DartNodeCore *c, DartEntityKind kind, const char *name, int which,
                                  int writer, const DartSchema **schema, uint8_t *reliable,
                                  uint64_t *generation){
    const i_DartMeshEntity *m;
    const DartSchema *best = NULL; uint64_t best_hash = 0;
    uint8_t rel = 0; int found = 0;
    uint64_t id;
    uint16_t s, np;
    if (schema) *schema = NULL;
    if (reliable) *reliable = 0;
    if (generation) *generation = 0;
    if (!c || !name) return 0;
    id = dart_topic_id(name);
    m = i_dart_node_core_mesh_lookup(c, (uint8_t)kind, id);
    if (!m) return 0;
    if (generation) *generation = m->generation;
    /* a reader takes the provider's declaration for this channel */
    if (!writer && m->has_provider){
        i_DartReflect *r = i_dart_node_core_reflect_of(c, m->provider);
        const i_DartChannel *ch = r ? i_dart_reflect_which(r, &r->ent[m->provider_slot], which) : NULL;
        if (ch){
            if (schema) *schema = ch->schema;
            if (reliable) *reliable = ch->reliable;
            return 1;
        }
    }
    /* a writer, or a reader with no provider: the widest live declaration, reliable if
       any reader of the channel requests it */
    np = c->discovery ? dart_discovery_max_peers(c->discovery) : 0;
    for (s = 0; s <= np; s++){
        i_DartReflect *r; uint32_t i;
        if (s == np) r = &c->self;
        else {
            DartDiscoveryPeer v; i_DartNodePeerExtra *ex;
            if (!dart_discovery_peer_at(c->discovery, s, &v) || v.liveness != DART_PEER_ACTIVE) continue;
            ex = (i_DartNodePeerExtra*)v.user;
            if (!ex || !ex->added) continue;
            r = &ex->refl;
        }
        if (r->dirty) i_dart_reflect_fold(c, r);
        for (i = 0; i < r->n_ent; i++){
            const i_DartPeerEntity *e = &r->ent[i];
            const i_DartChannel *ch;
            if (e->kind != (uint8_t)kind || e->id != id) continue;
            ch = i_dart_reflect_which(r, e, which);
            if (!ch) continue;
            found = 1;
            if (dart_role_subs(ch->role) && ch->reliable) rel = 1;
            if (!ch->schema) continue;
            if (!best || (ch->schema_hash != best_hash && dart_schema_subset(best, ch->schema))){
                best = ch->schema; best_hash = ch->schema_hash;
            }
        }
    }
    if (schema) *schema = best;
    if (reliable) *reliable = rel;
    return found;
}

/* Observer mode: appends every advertised but uncached index to the want list. INACTIVE
 * entries are skipped, since the responder would not answer them. */
static i_DartNodePeerExtra *i_dart_node_core_peer_extra(i_DartNodeCore *c, uint32_t id);
static uint16_t i_dart_node_core_greedy_extend(i_DartNodeCore *c, uint32_t peer,
                            DartBytes interest, DartDetailWant *wants, uint16_t n,
                            uint16_t max_wants){
    DartInterestIter it; DartTopicEntry e;
    uint16_t k;
    memset(&it, 0, sizeof it);
    while (n < max_wants && dart_interest_next(interest, &it, &e)){
        /* the iterator skips INACTIVE entries and hole runs, a PUBSUB double yield dedupes below */
        {   i_DartNodePeerExtra *ex = i_dart_node_core_peer_extra(c, peer);
            if (ex && e.index < ex->refl.n_chan && ex->refl.chan[e.index].name_off != I_DART_NAME_NONE) continue; }
        for (k = 0; k < n; k++) if (wants[k].index == e.index) break;
        if (k < n) continue;
        wants[n].index = e.index;
        wants[n].schema_hash = 0;   /* force the wire inline, we may not hold that schema */
        n++;
    }
    return n;
}

/* why the intern returned NULL, as text */
static char *i_dart_node_core_intern_why(i_DartNodeCore *c, DartBytes wire, char *p, char *end){
    if (!c->alloc)
        return i_dart_event_append_str(p, end, "no allocator here to parse peer schemas");
    if (!wire.data || wire.len == 0)
        return i_dart_event_append_str(p, end,
            "their schema wire is unavailable (not inlined in the detail response)");
    p = i_dart_event_append_str(p, end, "their schema wire was rejected: malformed, "
            "hash-mismatched, or a different DART schema wire version than ours (v");
    p = i_dart_event_append_u64(p, end, DART_SCHEMA_WIRE_VERSION);
    return i_dart_event_append_str(p, end, ")");
}

/* The verdict. A refusal writes its reason into [p, end], where end is the last writable
 * byte, and p == end means no text. The wrapper below records or clears the reason. */
static int i_dart_node_core_schema_verdict(i_DartNodeCore *c, uint32_t peer, uint16_t topic_index,
                                           int peer_is_pub, uint64_t hash, DartBytes wire,
                                           char *p, char *end){
    const DartSchema *ours = (topic_index < c->n_topics) ? c->chan_compiled[topic_index] : NULL;
    int peer_has = hash != 0;
    if (peer_is_pub){                                   /* their publish side: we would read */
        if (!ours){                                     /* a generic reader decodes with theirs */
            i_dart_node_core_peer_schema_set(c, peer, topic_index,
                peer_has ? i_dart_node_core_intern(c, hash, wire) : NULL);
            return 1;
        }
        if (!peer_has){                                 /* a typed reader refuses untyped */
            p = i_dart_event_append_str(p, end, "their writer has no schema, our typed reader refuses");
            *p = '\0'; return 0;
        }
        if (hash == dart_schema_hash(ours)){            /* identical: our own view works */
            i_dart_node_core_peer_schema_set(c, peer, topic_index, i_DART_NODE_SCHEMA_OURS);
            return 1;
        }
        {   DartSchema *pub = i_dart_node_core_intern(c, hash, wire);   /* the wire verifies it */
            DartSchema *view;
            if (!pub){
                p = i_dart_node_core_intern_why(c, wire, p, end);
                *p = '\0'; return 0;
            }
            if (!dart_schema_subset_why(ours, pub, p, (size_t)(end - p) + 1u)) return 0;
            view = i_dart_node_core_bind(c, hash, topic_index, ours, pub);
            if (!view){                                 /* OOM: refuse rather than misdecode */
                p = i_dart_event_append_str(p, end, "out of memory binding the reader view");
                *p = '\0'; return 0;
            }
            i_dart_node_core_peer_schema_set(c, peer, topic_index, view);
            return 1;
        }
    } else {                                            /* their subscribe side: we would write */
        if (!peer_has) return 1;                        /* a generic reader takes anything */
        if (!ours){                                     /* a typed reader refuses our raw topic */
            p = i_dart_event_append_str(p, end, "their reader is typed, our topic has no schema");
            *p = '\0'; return 0;
        }
        if (hash == dart_schema_hash(ours)) return 1;
        {   DartSchema *sub = i_dart_node_core_intern(c, hash, wire);
            if (!sub){
                p = i_dart_node_core_intern_why(c, wire, p, end);
                *p = '\0'; return 0;
            }
            return dart_schema_subset_why(sub, ours, p, (size_t)(end - p) + 1u);
        }
    }
}

int i_dart_node_core_schema_check(i_DartNodeCore *c, uint32_t peer, uint16_t topic_index,
                                  int peer_is_pub, uint64_t hash, DartBytes wire){
#ifndef DART_NO_DIAG
    char why[DART__SCHEMA_WHY_MAX];
    int ok;
    why[0] = '\0';
    ok = i_dart_node_core_schema_verdict(c, peer, topic_index, peer_is_pub, hash, wire,
                                         why, why + sizeof why - 1);
    if (ok) i_dart_node_core_schema_why_drop(c, peer, topic_index, peer_is_pub);
    else    i_dart_node_core_schema_why_set (c, peer, topic_index, peer_is_pub, why);
    return ok;
#else
    char why[1];
    return i_dart_node_core_schema_verdict(c, peer, topic_index, peer_is_pub, hash, wire, why, why);
#endif
}

/* Rebuilds our overlay from the core's fields. The whole announce must fit one datagram,
   else the bootstrap form goes out and peers page the interest. See spec/interest.md. */
uint16_t i_dart_node_core_build_meta(i_DartNodeCore *c){
    uint16_t need = dart_transport_meta_size(c->transport);
    int external = ((size_t)DART_DISCOVERY_META_OFF + DART_DISCOVERY_DISC_MAX + need
                    > (size_t)DART_DGRAM_MAX);
    if (external) need = dart_transport_meta_bootstrap_size();
    if (need > c->meta_cap){   /* size the blob buffer to the exact content */
        uint8_t *nb = (uint8_t*)c->alloc(c->alloc_user, c->meta_buf, need);
        if (!nb) return c->meta_len;   /* OOM: keep the previous blob, stale but consistent */
        c->meta_buf = nb; c->meta_cap = need;
    }
    c->meta_len = dart_transport_meta_build(c->transport, c->meta_buf, c->meta_cap,
                                  c->frag_size, c->oob_capable, c->oob_host, external);
    return c->meta_len;
}

DartBytes i_dart_node_core_meta(i_DartNodeCore *c){
    return dart_bytes(c->meta_buf, c->meta_len);
}

/* Answers a DETAIL_REQ into the core's grown scratch. One page per response, so it never
   IP fragments. A header only response still tells the requester the indices are gone. */
DartBytes i_dart_node_core_detail_respond(i_DartNodeCore *c, uint16_t domain, DartBytes req){
    size_t need, len;
    if (!c || !c->alloc || !c->discovery) return dart_bytes(NULL, 0);
    if (dart_detail_kind(req) != DART_DETAIL_REQ || dart_detail_domain(req) != domain)
        return dart_bytes(NULL, 0);
    need = dart_transport_detail_resp_size(c->transport, c->chan_schemas, req);
    if (!need) return dart_bytes(NULL, 0);
    if (need > c->detail_cap){
        uint8_t *nb = (uint8_t*)c->alloc(c->alloc_user, c->detail_buf, need);
        if (!nb) return dart_bytes(NULL, 0);
        c->detail_buf = nb; c->detail_cap = (uint32_t)need;
    }
    len = dart_transport_detail_respond(c->transport, c->chan_schemas,
                                        dart_discovery_meta_version(c->discovery), req,
                                        c->detail_buf, need);
    return dart_bytes(c->detail_buf, len);
}

#ifdef DART_SHM
/* A peer can receive our shared memory payload iff we are capable, it advertised a host
 * id, and that host equals ours. The core knows nothing of SHM beyond this. */
static void i_dart_node_core_set_peer_oob(i_DartNodeCore *c, uint32_t id, DartBytes meta){
    uint8_t host[16];
    int oob = c->oob_capable && dart_meta_shm(meta, host) &&
              memcmp(host, c->oob_host, 16) == 0;
    dart_transport_peer_set_shm(c->transport, id, oob);
}
#else
#define i_dart_node_core_set_peer_oob(c, id, meta) ((void)0)
#endif

/* fires PEER_UP or PEER_DOWN */
static void i_dart_node_core_fire(i_DartNodeCore *c, DartEventKind kind, uint32_t id,
                            const DartDiscoveryAddr *addr){
    DartEvent ev;
    if (!c->on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind; ev.peer = id; ev.user = c->user;
    if (addr){ memcpy(ev.ip, addr->ip, 16); ev.ip_len = addr->ip_len; ev.port = addr->port; }
    c->on_event(&ev);
}

/* fires a DART_ERROR. too_big carries the OOM or meta too big byte count */
static void i_dart_node_core_fire_error(i_DartNodeCore *c, DartErrorKind err, uint32_t id,
                            const DartDiscoveryAddr *addr, uint64_t too_big){
    DartEvent ev;
    if (!c->on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = DART_ERROR; ev.error = err; ev.peer = id; ev.user = c->user; ev.too_big_bytes = too_big;
    if (addr){ memcpy(ev.ip, addr->ip, 16); ev.ip_len = addr->ip_len; ev.port = addr->port; }
    c->on_event(&ev);
}

/* Every reflected interest change funnels through here, so this is also where the peer's
 * interest epoch bumps. It must bump even with no event handler. */
static void i_dart_node_core_fire_interest(i_DartNodeCore *c, uint32_t id){
    DartEvent ev; uint16_t publish_to = 0, receive_from = 0;
    {   i_DartNodePeerExtra *ex = c->discovery
            ? (i_DartNodePeerExtra*)dart_discovery_peer_user(c->discovery, id) : NULL;
        if (ex){
            uint32_t mv = 0; DartBytes meta = dart_discovery_peer_meta(c->discovery, id, &mv);
            ex->interest_epoch++;
            i_dart_node_core_reflect_interest(c, ex, i_dart_node_core_interest_of(ex, meta, mv));
            i_dart_node_core_mesh_bump(c);
        }
    }
    if (!c->on_event) return;
    dart_transport_peer_match_counts(c->transport, id, &publish_to, &receive_from);
    memset(&ev, 0, sizeof ev);
    ev.kind = DART_PEER_INTEREST; ev.peer = id;
    ev.publish_topics = publish_to; ev.receive_topics = receive_from;
    ev.user = c->user;
    c->on_event(&ev);
}

/* the peer's lifecycle state in the discovery scratch. NULL only before discovery is bound */
static i_DartNodePeerExtra *i_dart_node_core_peer_extra(i_DartNodeCore *c, uint32_t id){
    return (i_DartNodePeerExtra*)dart_discovery_peer_user(c->discovery, id);
}

/* The one read point for a peer's interest: the inline section, or the assembled external
 * blob at the advertised version, or {NULL,0} while a fetch is in flight. */
static DartBytes i_dart_node_core_interest_of(i_DartNodePeerExtra *ex, DartBytes meta,
                            uint32_t meta_version){
    if (!meta.data) return dart_bytes(NULL, 0);
    if (!dart_meta_interest_external(meta)) return dart_meta_interest(meta);
    if (ex && ex->interest_buf && ex->interest_version == meta_version)
        return dart_bytes(ex->interest_buf, ex->fetch_len);
    return dart_bytes(NULL, 0);
}

/* Queues an INTEREST_REQ for an external peer whose blob is not assembled at its current
 * version. The version dedup makes a steady state announce free. */
static void i_dart_node_core_interest_check(i_DartNodeCore *c, i_DartNodePeerExtra *ex,
                            DartBytes meta, uint32_t meta_version){
    if (!ex || !meta.data || !dart_meta_interest_external(meta)) return;
    if (ex->interest_version == meta_version) return;
    ex->interest_due = 1;
    c->detail_due_any = 1;
}

/* After an apply: queue a DETAIL_REQ while unverified candidates remain. Runs on every
   apply, so the pending state is the retry state and a lost datagram heals. */
static void i_dart_node_core_detail_check(i_DartNodeCore *c, i_DartNodePeerExtra *ex,
                            uint32_t id, DartBytes interest){
    DartDetailWant probe;
    if (!interest.data) return;
    if (dart_transport_detail_wants(c->transport, c->chan_schemas, id, interest, NULL, 0)
        || (c->fetch_details
            && i_dart_node_core_greedy_extend(c, id, interest, &probe, 0, 1))){
        ex->detail_due = 1;
        c->detail_due_any = 1;
    }
}

static void i_dart_node_core_peer_up(i_DartNodeCore *c, uint32_t id, const DartDiscoveryAddr *addr,
                            DartBytes meta){
    i_DartNodePeerExtra *ex = i_dart_node_core_peer_extra(c, id);
    uint16_t frag = dart_meta_frag(meta);
    uint32_t meta_version = 0;
    DartBytes interest;
    if (!ex) return;                                  /* discovery not bound */
    dart_discovery_peer_meta(c->discovery, id, &meta_version);
    interest = i_dart_node_core_interest_of(ex, meta, meta_version);
    if (!ex->added){                                  /* a new peer: wire it into the transport */
        i_dart_node_core_peer_schema_clear(c, id);    /* a recycled id: no stale bindings */
        i_dart_reflect_free(c, &ex->refl);
        i_dart_node_core_schema_why_clear(c, id);
        dart_transport_peer_add(c->transport, id, frag);
        ex->added = 1; ex->dormant = 0; ex->detail_due = 0;
        if (addr) i_dart_reflect_format_addr(&ex->refl, addr);
        i_dart_node_core_mesh_bump(c);
        i_dart_node_core_set_peer_oob(c, id, meta);
        i_dart_node_core_fire(c, DART_PEER_UP, id, addr);
        if (interest.data){ dart_transport_apply_peer_interest(c->transport, id, interest);
                       i_dart_node_core_fire_interest(c, id);
                       i_dart_node_core_detail_check(c, ex, id, interest); }
    } else {                                          /* a known peer: an update */
        dart_transport_peer_set_frag(c->transport, id, frag);
        i_dart_node_core_set_peer_oob(c, id, meta);
        if (interest.data){ dart_transport_apply_peer_interest(c->transport, id, interest);
                       i_dart_node_core_fire_interest(c, id);
                       i_dart_node_core_detail_check(c, ex, id, interest); }
        if (ex->dormant){    /* a DROPPED peer's same incarnation returned: resume */
            ex->dormant = 0;
            dart_transport_peer_resume(c->transport, id);
            i_dart_node_core_mesh_bump(c);
            i_dart_node_core_fire(c, DART_PEER_UP, id, addr);
        }
    }
    i_dart_node_core_interest_check(c, ex, meta, meta_version);   /* external and stale: pull it */
}

/* Ingests a DETAIL_RESP: caches the verdicts, then re applies the peer's current interest
   so they form their matches exactly as a fresh announce would. Idempotent. */
void i_dart_node_core_apply_details(i_DartNodeCore *c, uint16_t domain, uint32_t peer,
                            DartBytes resp){
    i_DartNodePeerExtra *ex;
    DartBytes meta, interest;
    if (!c || dart_detail_kind(resp) != DART_DETAIL_RESP || dart_detail_domain(resp) != domain)
        return;
    ex = i_dart_node_core_peer_extra(c, peer);
    if (!ex || !ex->added) return;
    {   /* a response older than the blob we hold may describe a binding the peer has since
           rebound: refuse it, the pending state re asks */
        uint32_t held = 0;
        dart_discovery_peer_meta(c->discovery, peer, &held);
        if (held && dart_detail_meta_version(resp) < held) return;
    }
    {   /* every detail received feeds reflection */
        DartDetailIter it; DartDetail dd;
        memset(&it, 0, sizeof it);
        while (dart_detail_next(resp, &it, &dd)) i_dart_node_core_reflect_detail(c, ex, &dd);
        if (ex->refl.dirty) i_dart_node_core_mesh_bump(c);
    }
    if (!dart_transport_apply_peer_details(c->transport, peer, resp)) return;   /* nothing new */
    {   uint32_t meta_version = 0;
        meta = dart_discovery_peer_meta(c->discovery, peer, &meta_version);
        interest = i_dart_node_core_interest_of(ex, meta, meta_version);
    }
    if (!interest.data) return;
    dart_transport_apply_peer_interest(c->transport, peer, interest);
    i_dart_node_core_fire_interest(c, peer);
    i_dart_node_core_detail_check(c, ex, peer, interest);
}

/* Queues a request for every active peer: the periodic retry sweep. A converged peer
   costs one wants walk and sends nothing. */
void i_dart_node_core_detail_rearm(i_DartNodeCore *c){
    uint16_t s, n;
    if (!c || !c->discovery) return;
    n = dart_discovery_max_peers(c->discovery);
    for (s=0;s<n;s++){
        DartDiscoveryPeer v; i_DartNodePeerExtra *ex;
        if (!dart_discovery_peer_at(c->discovery, s, &v)) continue;
        if (v.liveness != DART_PEER_ACTIVE) continue;
        ex = (i_DartNodePeerExtra*)v.user;
        if (ex && ex->added){
            ex->detail_due = 1; c->detail_due_any = 1;
            i_dart_node_core_interest_check(c, ex, v.meta, v.meta_version);
        }
    }
}

int i_dart_node_core_detail_any(i_DartNodeCore *c){ return c ? c->detail_due_any : 0; }

/* A request from peer named our blob version: proof it applied our announce at it. When
 * the advance releases a held writer lane, the interest event re fires. */
int i_dart_node_core_seen_version(i_DartNodeCore *c, uint32_t peer, uint32_t version){
    if (!c || !version) return 0;
    if (!dart_transport_peer_seen_version(c->transport, peer, version)) return 0;
    i_dart_node_core_fire_interest(c, peer);
    return 1;
}

/* A peer counts once while its interest is unknown (no blob yet, or a fetch in flight),
 * else by its unresolved entries. Dropped peers are skipped, they will not answer. */
int i_dart_node_core_topic_unresolved(i_DartNodeCore *c, uint16_t topic_index){
    uint16_t s, n; int cnt = 0;
    if (!c || !c->discovery) return 0;
    n = dart_discovery_max_peers(c->discovery);
    for (s=0;s<n;s++){
        DartDiscoveryPeer v; i_DartNodePeerExtra *ex;
        DartBytes interest;
        if (!dart_discovery_peer_at(c->discovery, s, &v)) continue;
        if (v.liveness != DART_PEER_ACTIVE) continue;
        ex = (i_DartNodePeerExtra*)v.user;
        if (!ex || !ex->added) continue;
        if (!v.meta.data){ cnt++; continue; }   /* the blob is still being fetched */
        interest = i_dart_node_core_interest_of(ex, v.meta, v.meta_version);
        if (!interest.data){
            if (dart_meta_interest_external(v.meta)) cnt++;   /* a fetch in flight: resolving */
            continue;                           /* else no interest: nothing to resolve */
        }
        cnt += dart_transport_topic_unresolved(c->transport, topic_index, v.id, interest);
    }
    return cnt;
}

void i_dart_node_core_topics_unresolved(i_DartNodeCore *c, uint16_t *counts, uint16_t n){
    uint16_t s, np, i;
    if (!counts || !n) return;
    memset(counts, 0, (size_t)n * sizeof *counts);
    if (!c || !c->discovery) return;
    np = dart_discovery_max_peers(c->discovery);
    for (s=0;s<np;s++){
        DartDiscoveryPeer v; i_DartNodePeerExtra *ex;
        DartBytes interest = dart_bytes(NULL, 0); int all = 0;
        if (!dart_discovery_peer_at(c->discovery, s, &v)) continue;
        if (v.liveness != DART_PEER_ACTIVE) continue;
        ex = (i_DartNodePeerExtra*)v.user;
        if (!ex || !ex->added) continue;
        if (!v.meta.data) all = 1;                        /* the blob is still being fetched */
        else {
            interest = i_dart_node_core_interest_of(ex, v.meta, v.meta_version);
            if (!interest.data && dart_meta_interest_external(v.meta)) all = 1;   /* fetching */
        }
        if (all){   /* its interest is unknown, so it may nominate any topic */
            for (i=0;i<n;i++) if (counts[i] != 0xFFFFu) counts[i]++;
            continue;
        }
        if (!interest.data) continue;                     /* no interest: nothing to resolve */
        dart_transport_peer_unresolved_fill(c->transport, v.id, interest, counts, n);
    }
}

/* Drains one queued request: INTEREST_REQs first, since an unassembled interest gates
   candidate discovery, then DETAIL_REQs, capped at 128 wants. Loop until 0. */
size_t i_dart_node_core_detail_req_next(i_DartNodeCore *c, uint16_t domain,
                            void *out, size_t cap, i_DartNodeDest *to){
    DartDetailWant wants[128];
    uint16_t s, n;
    if (!c || !c->discovery || cap < 24u) return 0;
    n = dart_discovery_max_peers(c->discovery);
    for (s=0;s<n;s++){    /* the interest pass: one INTEREST_REQ per due peer, cursor driven */
        DartDiscoveryPeer v; i_DartNodePeerExtra *ex;
        uint32_t offset;
        if (!dart_discovery_peer_at(c->discovery, s, &v)) continue;
        ex = (i_DartNodePeerExtra*)v.user;
        if (!ex || !ex->interest_due) continue;
        ex->interest_due = 0;
        if (!ex->added || v.liveness != DART_PEER_ACTIVE) continue;
        if (!dart_meta_interest_external(v.meta)) continue;      /* the flag cleared meanwhile */
        if (ex->interest_version == v.meta_version) continue;    /* assembled meanwhile */
        offset = (ex->fetch_version == v.meta_version) ? ex->fetch_cursor : 0;
        if (!i_dart_node_core_resolve(c, v.id, to)) continue;
        {   size_t len = dart_interest_req_build(domain, v.meta_version, offset, out, cap);
            if (len) return len;
        }
    }
    for (s=0;s<n;s++){
        DartDiscoveryPeer v; i_DartNodePeerExtra *ex;
        DartBytes interest; uint16_t nw, maxw;
        if (!dart_discovery_peer_at(c->discovery, s, &v)) continue;
        ex = (i_DartNodePeerExtra*)v.user;
        if (!ex || !ex->detail_due) continue;
        ex->detail_due = 0;
        if (!ex->added || v.liveness != DART_PEER_ACTIVE) continue;
        interest = i_dart_node_core_interest_of(ex, v.meta, v.meta_version);
        if (!interest.data) continue;
        maxw = (uint16_t)((cap - 14u) / 10u);
        if (maxw > 128u) maxw = 128u;
        nw = dart_transport_detail_wants(c->transport, c->chan_schemas, v.id, interest,
                                         wants, maxw);
        if (c->fetch_details)
            nw = i_dart_node_core_greedy_extend(c, v.id, interest, wants, nw, maxw);
        if (!nw) continue;
        if (!i_dart_node_core_resolve(c, v.id, to)) continue;
        {   size_t len = dart_detail_req_build(domain, v.meta_version, wants, nw, out, cap);
            if (len) return len;
        }
    }
    c->detail_due_any = 0;
    return 0;
}

/* Answers an INTEREST_REQ with one page of our interest blob, built into the scratch after
   a header's worth of headroom so the page is returned without a copy. Stateless. */
DartBytes i_dart_node_core_interest_respond(i_DartNodeCore *c, uint16_t domain, DartBytes req){
    uint32_t offset, total; size_t chunk, need, built;
    uint8_t *blob, *head;
    if (!c || !c->discovery) return dart_bytes(NULL, 0);
    if (dart_detail_kind(req) != DART_INTEREST_REQ || dart_detail_domain(req) != domain)
        return dart_bytes(NULL, 0);
    if (!dart_interest_req_offset(req, &offset)) return dart_bytes(NULL, 0);
    total = dart_transport_interest_size(c->transport);
    need  = (size_t)DART_INTEREST_RESP_HEAD + total;
    if (need > c->detail_cap){
        uint8_t *nb = (uint8_t*)c->alloc(c->alloc_user, c->detail_buf, need);
        if (!nb) return dart_bytes(NULL, 0);
        c->detail_buf = nb; c->detail_cap = (uint32_t)need;
    }
    blob  = c->detail_buf + DART_INTEREST_RESP_HEAD;
    built = dart_transport_build_interest(c->transport, blob, total);
    if (built != total) return dart_bytes(NULL, 0);   /* drifted: never serve garbage */
    if (offset > total) offset = total;               /* clamp: a header only page reports total */
    chunk = total - offset;
    if (chunk > (size_t)DART_DGRAM_MAX - DART_INTEREST_RESP_HEAD)
        chunk = (size_t)DART_DGRAM_MAX - DART_INTEREST_RESP_HEAD;   /* one sub datagram page */
    head = blob + offset - DART_INTEREST_RESP_HEAD;   /* the header sits right before the chunk */
    dart_interest_resp_head(domain, dart_discovery_meta_version(c->discovery), total,
                            offset, (uint16_t)chunk, head, DART_INTEREST_RESP_HEAD);
    return dart_bytes(head, DART_INTEREST_RESP_HEAD + chunk);
}

/* Ingests one INTEREST_RESP page at the cursor, re asks while incomplete, and on completion
   applies the blob as an inline announce would. A version change mid fetch restarts. */
void i_dart_node_core_apply_interest_page(i_DartNodeCore *c, uint16_t domain, uint32_t peer,
                            DartBytes resp){
    i_DartNodePeerExtra *ex;
    uint32_t total, offset, resp_version; DartBytes chunk;
    if (!c || dart_detail_kind(resp) != DART_INTEREST_RESP || dart_detail_domain(resp) != domain)
        return;
    ex = i_dart_node_core_peer_extra(c, peer);
    if (!ex || !ex->added) return;
    if (!dart_interest_resp_parse(resp, &total, &offset, &chunk)) return;
    if (total > dart_interest_max(0xFFFFu)) return;   /* an absurd total: never a real blob */
    resp_version = dart_detail_meta_version(resp);
    if (ex->interest_buf && ex->interest_version == resp_version) return;   /* a dup page */
    if (ex->fetch_version != resp_version){           /* first page, or the version moved */
        ex->fetch_version = resp_version;
        ex->fetch_cursor = 0; ex->fetch_len = total;
    }
    if (total != ex->fetch_len){ ex->fetch_cursor = 0; ex->fetch_len = total; }  /* drifted */
    if (offset != ex->fetch_cursor){                  /* out of order: re ask from the cursor */
        ex->interest_due = 1; c->detail_due_any = 1;
        return;
    }
    if (ex->interest_cap < total){
        uint8_t *nb = (uint8_t*)c->alloc(c->alloc_user, ex->interest_buf, total ? total : 1u);
        if (!nb){ i_dart_node_core_fire_error(c, DART_E_OOM, peer, NULL, total); return; }
        ex->interest_buf = nb; ex->interest_cap = total ? total : 1u;
    }
    if (chunk.len){ memcpy(ex->interest_buf + offset, chunk.data, chunk.len); ex->fetch_cursor += (uint32_t)chunk.len; }
    if (ex->fetch_cursor < total){
        if (chunk.len)                                /* progress: ask for the next page now */
            { ex->interest_due = 1; c->detail_due_any = 1; }
        return;                                       /* an empty short page: the sweep re asks */
    }
    ex->interest_version = resp_version;              /* assembled: the dedup that ends the cycle */
    ex->fetch_version = 0;
    dart_transport_apply_peer_interest(c->transport, peer, dart_bytes(ex->interest_buf, total));
    i_dart_node_core_fire_interest(c, peer);
    i_dart_node_core_detail_check(c, ex, peer, dart_bytes(ex->interest_buf, total));
}

static void i_dart_node_core_peer_down(i_DartNodeCore *c, uint32_t id, DartDiscoveryDownReason reason){
    i_DartNodePeerExtra *ex = i_dart_node_core_peer_extra(c, id);   /* freed after this event */
    if (reason == DART_DISCOVERY_DROP){
        /* fell silent: keep the transport state for a same incarnation resume, drop the
           peer from flow control and tell the app once */
        if (ex && ex->added && !ex->dormant){
            ex->dormant = 1;
            dart_transport_peer_dormant(c->transport, id);
            i_dart_node_core_mesh_bump(c);
            i_dart_node_core_fire(c, DART_PEER_DOWN, id, NULL);
        }
    } else {   /* GONE: free the transport state */
        int notify = (ex && ex->added && !ex->dormant);   /* the app was not told yet */
        dart_transport_peer_remove(c->transport, id);
        i_dart_node_core_peer_schema_clear(c, id);      /* the id may be reassigned */
        if (ex) i_dart_reflect_free(c, &ex->refl);
        i_dart_node_core_schema_why_clear(c, id);
        i_dart_node_core_mesh_bump(c);
        if (ex && ex->interest_buf){                    /* the retained external interest */
            c->alloc(c->alloc_user, ex->interest_buf, 0);
            ex->interest_buf = NULL; ex->interest_cap = 0;
            ex->interest_version = 0; ex->fetch_version = 0;
            ex->fetch_cursor = 0; ex->fetch_len = 0; ex->interest_due = 0;
        }
        if (notify) i_dart_node_core_fire(c, DART_PEER_DOWN, id, NULL);
    }
}

static void i_dart_node_core_peer_refused(i_DartNodeCore *c, const DartDiscoveryAddr *addr){
    i_dart_node_core_fire_error(c, DART_E_PEER_REFUSED, 0, addr, 0);
}

/* The discovery core's event sink, demuxed into the lifecycle handlers above. */
void i_dart_node_core_on_disc_event(const DartDiscoveryEvent *ev){
    i_DartNodeCore *c = (i_DartNodeCore*)ev->user;
    switch (ev->kind){
        case DART_DISCOVERY_PEER_UP:
            i_dart_node_core_peer_up(c, ev->peer, &ev->addr, ev->meta);
            break;
        case DART_DISCOVERY_PEER_DOWN:
            i_dart_node_core_peer_down(c, ev->peer, ev->reason);
            break;
        case DART_DISCOVERY_PEER_REFUSED:
            i_dart_node_core_peer_refused(c, &ev->addr);
            break;
        case DART_DISCOVERY_META_TOO_BIG:
            i_dart_node_core_fire_error(c, DART_E_PEER_META_TOO_BIG, ev->peer, &ev->addr, ev->meta.len);
            break;
        default: break;
    }
}

int i_dart_node_core_resolve(i_DartNodeCore *c, uint32_t to, i_DartNodeDest *out){
    DartDiscoveryAddr a;
    memset(out, 0, sizeof *out);
    if (!dart_discovery_addr_of_id(c->discovery, to, &a)) return 0;   /* the peer vanished */
    memcpy(out->ip, a.ip, 16);
    out->ip_len = a.ip_len;
    out->port   = a.port;
    return 1;
}

int i_dart_node_core_id_for_addr(i_DartNodeCore *c, const uint8_t ip[4], uint16_t port, uint32_t *id){
    return dart_discovery_id_for_addr(c->discovery, ip, 4, port, id);
}

DartString i_dart_node_core_peer_name(i_DartNodeCore *c, uint32_t id){
    DartString name = dart_discovery_peer_name(c->discovery, id);
    if (!name.data) return name;                          /* not a known peer */
    if (name.len == 0) name = dart_cstr("unknown-peer");  /* known but unnamed */
    return name;
}

uint16_t i_dart_node_core_max_peers(i_DartNodeCore *c){ return dart_discovery_max_peers(c->discovery); }

int i_dart_node_core_peer_at(i_DartNodeCore *c, uint16_t slot, uint32_t *id,
                           uint8_t ip[16], uint8_t *ip_len, uint16_t *port){
    DartDiscoveryPeer v;
    if (!dart_discovery_peer_at(c->discovery, slot, &v)) return 0;
    if (id)     *id = v.id;
    if (ip)     memcpy(ip, v.addr.ip, 16);
    if (ip_len) *ip_len = v.addr.ip_len;
    if (port)   *port = v.addr.port;
    return 1;
}

uint16_t i_dart_node_peer_frag(const DartDiscoveryPeer *peer){
    return (peer && peer->meta.data) ? dart_meta_frag(peer->meta) : 0;
}

uint32_t i_dart_node_peer_interest_epoch(const DartDiscoveryPeer *peer){
    const i_DartNodePeerExtra *ex = peer ? (const i_DartNodePeerExtra*)peer->user : NULL;
    return ex ? ex->interest_epoch : 0;
}

int i_dart_node_peer_interest_next(const DartDiscoveryPeer *peer,
                                   DartInterestIter *it, DartTopicEntry *out){
    if (!peer || !peer->meta.data) return 0;
    /* one read point for both interest homes, so every walk resolves external peers alike */
    return dart_interest_next(
        i_dart_node_core_interest_of((i_DartNodePeerExtra*)peer->user, peer->meta,
                                     peer->meta_version),
        it, out);
}
#pragma endregion
#pragma region node/runtime.c
/* The node runtime: the sockets, the clock, the send and receive paths, and the public
 * dart_node_* and dart_topic_* API over the sans-IO cores. The rules are in spec/node.md. */

#ifdef DART_SHM
#endif
#include <string.h>    /* heap access goes through the node pool, no stdlib.h here */
#include <stdarg.h>    /* dart_node_log */
#include <stdio.h>     /* vsnprintf for log text only, never the data path */

#ifndef DART_NO_PATTERNS
struct DartNode;       /* patterns/core.c hosts the @dart/meta endpoint, open calls this seam */
void i_dart_patterns_meta_open(struct DartNode *n);
#endif

/* A consumer queue ring record: this header, the publisher name, then the payload at an
 * 8 aligned offset. Records never wrap, a short tail holds the DART__QWRAP sentinel. */
typedef struct {
    uint32_t rec_bytes;    /* the whole record, 8 aligned. First, the ring reads it at offset 0 */
    uint32_t data_len;
    uint64_t t_recv_us;    /* the arrival stamp, DartMsg.recv_us */
    uint64_t t_written_us; /* the publisher's source stamp, stripped at enqueue, 0 = opted out */
    uint64_t t_capture_us; /* the publisher's capture stamp, 0 = it sent none */
    uint32_t publisher_id;
    uint8_t  name_len;     /* the name is copied inline, discovery views die with the peer */
    uint8_t  pad[3];
} i_DartQRec;
#define DART__QWRAP 0xFFFFFFFFu
#define DART__QALIGN(x) (((uint32_t)(x) + 7u) & ~7u)

/* One topic's consumer queue: a byte ring filled by the poll thread and drained by take or
 * dispatch. Under the node lock. The ring and this struct are stable pool allocations. */
typedef struct {
    uint8_t  *buf;
    uint32_t  cap;         /* current ring bytes, 8 aligned, grows on demand */
    uint32_t  cap_limit;   /* the growth bound: qos.queue_bytes or DART_QUEUE_CAP */
    uint32_t  head, tail;  /* byte offsets, empty iff count == 0 */
    uint32_t  bytes;       /* queued record bytes, excludes wrap padding */
    uint32_t  count;       /* queued records, including one being viewed */
    uint32_t  dropped;     /* best effort records overwritten or refused since open */
    uint8_t   viewing;     /* the record at tail is the consumer's live take view */
    uint8_t   busy;        /* inside dispatch's unlocked callback window, no reentry */
    uint8_t   reliable;    /* the full queue policy: park (reliable) or overwrite */
    uint8_t   parked;      /* transport lanes parked on this topic, retried as we drain */
} i_DartMsgQueue;

struct DartTopic {
    DartNode *n; uint16_t index; DartSchema *schema;   /* the schema is the node's own copy */
    i_DartMsgQueue *q;                  /* the consumer queue, NULL = inline callbacks */
    i_DartSysMsgFn sys_on_message;      /* the patterns layer's routing, NULL = a normal topic */
    void    *sys_msg_user;
    uint64_t tx_msgs, tx_bytes;         /* committed by our sends */
    uint64_t rx_msgs, rx_bytes;         /* delivered to us, parked excluded */
    uint32_t resolve_epoch;             /* match_epoch at the last convergence, 0 = never */
    uint8_t  prefix_bytes;              /* pattern header bytes split into .header, 0 = plain */
    uint8_t  prefix_string;             /* [u8 len][bytes] follows the prefix on a response kind */
    uint8_t  kind;                      /* DartTopicKind */
    uint8_t  role;                      /* DartRole, mirrored at create and set_role */
    uint8_t  attrs, directed;           /* the def's declared facts, kept so refresh can redefine */
    uint8_t  reflect;                   /* created with reflect_from_mesh, so refresh applies */
    uint64_t generation;                /* the mesh generation the schema was taken at */
    DartQos  qos;
    uint8_t  name_len;                  /* stable copy, queued views never point into the arena */
    char     name[DART_TOPIC_NAME_MAX];
};

/* One pending error to log mirror entry. Errors fire deep inside receive processing where
 * a send may not re enter the transport, so emit records here and the poll pass publishes. */
#define DART_LOG_PEND 8
typedef struct {
    uint64_t wall_us, mono_us;   /* when the first occurrence fired */
    uint32_t peer, count;
    uint16_t topic;
    uint8_t  error;              /* DartErrorKind */
    char     text[192];
} i_DartLogPend;

struct DartNode {
    DartTransportState     *transport;
    i_DartNodeCore *core;     /* the peer table and discovery lifecycle, sans-IO */
    DartDiscovery     *discovery;
    i_DartSock       fd;       /* the unicast data socket */
    uint16_t      domain;
    DartNodeNet  net;         /* a copy of opts.net */
    /* the detail exchange retry cadence: queued requests drain each poll and a sweep re
       asks once per announce interval until a sweep sends nothing */
    uint32_t      announce_us;     /* the resolved discovery announce interval */
    uint64_t      next_detail_us;  /* the next retry sweep, 0 = disarmed */
    /* the datagram the socket refused, retried first next poll so it is never lost */
    uint8_t       tx_hold[DART_DGRAM_MAX];
    size_t        tx_hold_len;
    uint32_t      tx_hold_peer;
    /* the data socket RX buffer, sized for the largest discovery datagram we accept too */
    uint8_t      *rx_buf;
    size_t        rx_buf_bytes;
    /* backpressure accumulators, read via dart_node_backpressure_stats */
    uint64_t      backpressure_total_us;
    uint32_t      backpressure_wait_count;
    uint32_t      evicted_unsent;  /* the DART_E_EVICTED_UNSENT count */
#ifdef DART_THREADS
    /* the node lock and the optional service thread. See spec/node.md */
    i_DartMutex   mu;             /* the node lock: every public entry point takes it */
    i_DartCond    cv;             /* senders and drainers waiting on the poller's progress */
    uint32_t      cv_waiters;     /* under mu, broadcast skipped when 0 */
    i_DartWaker   waker;          /* interrupts the unlocked socket wait */
    i_DartThread  svc;
    volatile uint8_t svc_running; /* a service thread is alive */
    volatile uint8_t svc_stop;    /* stop requested, the service loop exits on it */
    uint8_t       svc_joining;    /* under mu: one stopper owns the join, others wait on cv */
    uint32_t      pollers_sleeping; /* under mu: pollers in or headed into the unlocked wait */
    uint8_t       wake_signaled;  /* under mu: a burst coalesces into one waker datagram */
    uint8_t       user_locked;    /* the public dart_node_lock is held */
    uint64_t      work_seq;       /* completed work passes, the unsent wait predicate */
    volatile uint8_t  lock_held;  /* the owner reentrancy check, written by the owner only */
    volatile uint64_t lock_owner;
#endif
    /* the in pump probe sampled inside the backpressure wait */
    DartPumpProbeFn pump_probe;
    void          *pump_probe_user;
    uint64_t       pump_probe_interval_us;
    /* the app callbacks */
    DartMsgFn    user_on_message;
    DartEventFn  on_event;
    void          *user_data;
    /* the patterns layer's hooks, all optional */
    i_DartSysEventFn sys_on_event;
    i_DartSysTickFn  sys_tick;
    i_DartSysCloseFn sys_on_close;
    void            *sys_user;
    uint64_t         sys_tick_next;   /* the tick's next deadline, folded into the poll wait */
    uint64_t         settle_topology_us; /* the last PEER_UP, DOWN or INTEREST change */
    /* the send path match wait: the bound, the post open gather latch and the epoch that
       stales each topic's converged memo. See spec/interest.md */
    uint32_t         match_wait_us;   /* 0 = disabled */
    uint64_t         open_us;         /* when the node opened, the gather anchor */
    uint8_t          gather_done;     /* latched once the post open gather settles */
    uint32_t         match_epoch;     /* bumps on peer or interest change, create and set_role */
    void            *patterns;        /* the patterns layer's per node manager, opaque here */
    DartEvent     last_error;  /* the most recent DART_ERROR, DART_E_NONE until one fires */
    /* the built in @dart/log topics and the error mirror ring */
    DartTopic    *log_topics[3];       /* by DartLogLevel, all NULL under opts.disable_logs */
    DartSchema   *log_schema;          /* DartLog { wall_us, mono_us, text }, node owned */
    uint8_t       log_errors;          /* the default on DART_ERROR mirror */
    uint8_t       log_flushing;        /* reentrancy guard: a flush must not re enter the ring */
    uint8_t       log_pend_n;
    uint32_t      log_pend_dropped;    /* ring overflow between flushes, summarized, never silent */
    i_DartLogPend *log_pend;           /* [DART_LOG_PEND], allocated on the first recorded error */
    /* the @dart/meta snapshot scratch, grown on demand */
    uint8_t      *snap_buf; uint32_t snap_cap;
    uint16_t     *snap_pend; uint16_t snap_pend_cap;   /* per topic pending counts, one walk */
    char          name[DART_NODE_NAME_MAX + 1];   /* our advertised node name */
    uint8_t       name_len;
    void          *arena;      /* the control structs block, relocated on grow */
    /* the node's pool, copied from the caller's allocator at open and reset on close */
    DartAllocator pool;
    uint8_t        alloc_dynamic;
    uint8_t        grow_pending;   /* a peer was refused for lack of slots, grow at the next poll */
    uint16_t       max_peers;      /* the current peer table capacity, doubles on a dynamic grow */
    /* the accept bound: the largest peer announce blob we store. It self heals up, see
       spec/discovery.md. meta_grow_failed remembers a size that would not allocate */
    uint16_t       meta_cap;
    uint16_t       meta_grow_need;
    uint16_t       meta_grow_failed;
    /* topic handles: a pointer array in the arena, each DartTopic a stable allocation */
    DartTopic **handles;    /* [max_topics] */
    uint16_t      max_topics;
    uint16_t      n_created;       /* user topics created, dense from 0, stepping over builtins */
    /* the builtins live at [builtin_lo, builtin_lo + n_builtin), above the user budget */
    uint16_t      builtin_lo;      /* the first builtin index */
    uint16_t      n_builtin;
    uint8_t       creating_builtin;/* open is creating builtins: allocate from the block */
#ifdef DART_SHM
    /* the same host path: lazy per topic and size class segments, see spec/shm.md */
    uint8_t        shm_capable;
    uint8_t        shm_host[16];  /* our host uuid, advertised for the same host check */
    uint64_t       shm_base;      /* the segment id base, low 19 bits = (topic << 3) | class */
    void          *shm_scratch; uint32_t shm_scratch_cap;   /* the one copy receive scratch */
    void         **shm_pool;      /* [n_topics * N_CLASSES] our segments, NULL = not created */
    uint16_t       shm_n_topics;
    /* the reader attach cache, grown by segments actually attached */
    uint64_t      *shm_reader_segments;  /* [shm_reader_cap] attached segment ids */
    void         **shm_reader_states;    /* [shm_reader_cap] their pool states */
    uint16_t       shm_reader_cap, shm_reader_count;
    uint32_t       shm_tx, shm_rx;/* messages published and delivered via SHM */
#endif
};

/* The node's allocator hook for the transport, schemas and handles: one freeable pool
 * allocation per call, so reset reclaims all. u is the node. */
static void *i_dart_node_alloc(void *u, void *ptr, size_t size){
    return dart_allocator_alloc(&((DartNode*)u)->pool, ptr, size);
}

#ifdef DART_THREADS
/* Raw lock ops keeping the owner id consistent. lock_owner is zeroed before the release
   so a non owner reading the pair unlocked never sees lock_held with its own id. */
static void i_dart_node_lock_raw(DartNode *n){
    i_dart_plat_mutex_lock(&n->mu);
    n->lock_owner = i_dart_plat_thread_id();
    n->lock_held = 1;
}
static void i_dart_node_unlock_raw(DartNode *n){
    n->lock_owner = 0;
    n->lock_held = 0;
    i_dart_plat_mutex_unlock(&n->mu);
}
/* The reentrant aware entry lock: 1 = this call acquired mu, 0 = the calling thread
   already held it (a callback, or the pump's nested poll) and proceeds without waiting. */
static int i_dart_node_lock(DartNode *n){
    if (n->lock_held && n->lock_owner == i_dart_plat_thread_id()) return 0;
    i_dart_node_lock_raw(n);
    return 1;
}
static void i_dart_node_unlock(DartNode *n, int acquired){
    if (acquired) i_dart_node_unlock_raw(n);
}
/* With mu held, before unlocking: cut the pollers' wait short so the change is serviced
   now. One datagram wakes every sleeping poller, and a burst coalesces to one per sleep. */
static void i_dart_node_kick(DartNode *n){
    if (n->pollers_sleeping && !n->wake_signaled && i_dart_plat_waker_signal(&n->waker))
        n->wake_signaled = 1;
}
/* A condvar wait that re stamps ownership after reacquire. Nothing cached from inside
   the node survives it: the poller may have relocated the arena. */
static void i_dart_node_cv_wait(DartNode *n, uint64_t timeout_us){
    n->lock_owner = 0;
    n->lock_held = 0;
    i_dart_plat_cond_wait(&n->cv, &n->mu,
                          timeout_us > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)timeout_us);
    n->lock_owner = i_dart_plat_thread_id();
    n->lock_held = 1;
}
#else
static int  i_dart_node_lock(DartNode *n){ (void)n; return 1; }
static void i_dart_node_unlock(DartNode *n, int acquired){ (void)n; (void)acquired; }
static void i_dart_node_kick(DartNode *n){ (void)n; }
#endif /* DART_THREADS */

/* The kick a send owes: only when the transport has a datagram to hand out or a held one
   to retry. The waker costs tens of microseconds, an idle publisher must not pay it. */
static void i_dart_node_kick_tx(DartNode *n){
#ifdef DART_THREADS
    if (n->tx_hold_len || dart_transport_tx_pending(n->transport)) i_dart_node_kick(n);
#else
    (void)n;
#endif
}

/* error reporting: one emit path stamps user_data, keeps the last error and hands on */

/* one past the highest defined topic index, the bound over handles[] (holes are NULL) */
static uint16_t i_dart_node_topic_hi(DartNode *n){
    uint16_t hi = n->n_created;
    if (n->n_builtin){
        if (hi >= n->builtin_lo) hi = (uint16_t)(hi + n->n_builtin);   /* users grew past it */
        if ((uint16_t)(n->builtin_lo + n->n_builtin) > hi) hi = (uint16_t)(n->builtin_lo + n->n_builtin);
    }
    return hi;
}

static int i_dart_node_is_log_topic(DartNode *n, uint16_t topic_index){
    DartTopic *h = (topic_index < i_dart_node_topic_hi(n)) ? n->handles[topic_index] : NULL;
    return h && (h == n->log_topics[0] || h == n->log_topics[1] || h == n->log_topics[2]);
}

static void i_dart_node_emit(DartNode *n, DartEvent *e){
    e->user = n->user_data;
    if (e->peer){    /* resolve the peer's name once so every event message prints a label */
        DartString nm = i_dart_node_core_peer_name(n->core, e->peer);
        e->peer_name = nm.data;   /* a NUL terminated view into discovery state, NULL if unknown */
    }
    if (e->kind == DART_ERROR) n->last_error = *e;
    /* the error to log mirror records only, the poll pass publishes. Errors on a log
       topic itself are excluded, and nothing is recorded during a flush */
    if (e->kind == DART_ERROR && n->log_errors && !n->log_flushing
        && !(e->topic_name && i_dart_node_is_log_topic(n, e->topic))){
        uint8_t i;
        if (!n->log_pend){                 /* the first error ever: allocate the mirror ring */
            n->log_pend = (i_DartLogPend*)i_dart_node_alloc(n, NULL,
                              sizeof(i_DartLogPend) * DART_LOG_PEND);
            if (!n->log_pend){ n->log_pend_dropped++; goto pend_done; }
        }
        for (i = 0; i < n->log_pend_n; i++)
            if (n->log_pend[i].error == (uint8_t)e->error && n->log_pend[i].topic == e->topic
                && n->log_pend[i].peer == e->peer) break;
        if (i < n->log_pend_n) n->log_pend[i].count++;
        else if (n->log_pend_n < DART_LOG_PEND){
            i_DartLogPend *p = &n->log_pend[n->log_pend_n++];
            p->error = (uint8_t)e->error; p->topic = e->topic; p->peer = e->peer; p->count = 1;
            p->wall_us = i_dart_plat_wall_us(); p->mono_us = i_dart_plat_now_us();
            dart_event_str(e, p->text, sizeof p->text);
        } else n->log_pend_dropped++;
    }
pend_done:
    if (e->kind == DART_PEER_UP || e->kind == DART_PEER_DOWN || e->kind == DART_PEER_INTEREST){
        n->settle_topology_us = i_dart_plat_now_us();   /* dart_node_settle's quiet window clock */
        n->match_epoch++;                               /* stale the topics' converged memos */
    }
    if (n->sys_on_event) n->sys_on_event(n->sys_user, e);
    if (n->on_event) n->on_event(e);
}

/* our topic's name as a C string, NULL if undefined: the topic_name view on events */
static const char *i_dart_node_topic_name(DartNode *n, uint16_t topic_index){
    DartString s = dart_transport_topic_name(n->transport, topic_index);
    return (const char*)s.data;
}

/* Strips the stamps a stamped publisher prepends, filling *written_us and *capture_us.
 * The one strip point: every delivery path runs through here before the header split. */
static DartBytes i_dart_node_strip_ts(DartBytes wire, int stamped, uint64_t *written_us,
                                      uint64_t *capture_us){
    uint64_t w;
    *written_us = 0; *capture_us = 0;
    if (!stamped || wire.len < DART_TIMESTAMP_BYTES) return wire;
    w = i_dart_le_r64(wire.data);
    *written_us = w & DART_STAMP_MASK;
    if (!(w & DART_STAMP_CAPTURE))
        return dart_bytes(wire.data + DART_TIMESTAMP_BYTES, wire.len - DART_TIMESTAMP_BYTES);
    /* the marker promises a second slot: a sample too short for it is malformed, keep the
       bytes whole rather than reading past them */
    if (wire.len < (size_t)DART_TIMESTAMP_BYTES + DART_CAPTURE_BYTES) return wire;
    *capture_us = i_dart_le_r64(wire.data + DART_TIMESTAMP_BYTES);
    return dart_bytes(wire.data + DART_TIMESTAMP_BYTES + DART_CAPTURE_BYTES,
                      wire.len - DART_TIMESTAMP_BYTES - DART_CAPTURE_BYTES);
}

/* Splits a delivered wire into its pattern header and the payload after it, so the schema
 * validates a message free payload. A plain topic yields an empty header. */
static void i_dart_node_split(DartTopic *h, DartBytes wire, DartBytes *hdr, DartBytes *payload){
    size_t pfx = (h && (uint32_t)wire.len >= h->prefix_bytes) ? h->prefix_bytes : 0u;
    if (pfx && h->prefix_string && wire.len > pfx){
        size_t ml = wire.data[pfx], avail = wire.len - pfx - 1u;
        pfx += 1u + (ml <= avail ? ml : avail);
    }
    hdr->data = pfx ? wire.data : NULL; hdr->len = pfx;
    payload->data = wire.data + pfx; payload->len = wire.len - pfx;
}

/* the consumer queue ring */

/* the oldest record with the wrap normalized into tail, NULL when empty */
static const i_DartQRec *i_dart_q_peek(i_DartMsgQueue *q){
    if (!q->count) return NULL;
    if (q->cap - q->tail < (uint32_t)sizeof(i_DartQRec) ||
        ((const i_DartQRec*)(q->buf + q->tail))->rec_bytes == DART__QWRAP)
        q->tail = 0;
    return (const i_DartQRec*)(q->buf + q->tail);
}

/* drops the oldest record, never a viewed one */
static void i_dart_q_pop(i_DartMsgQueue *q){
    const i_DartQRec *rec = i_dart_q_peek(q);
    if (!rec) return;
    q->tail += rec->rec_bytes;
    q->bytes -= rec->rec_bytes;
    if (--q->count == 0){ q->head = 0; q->tail = 0; }
}

/* contiguous space for need bytes, fills *at and pre writes the wrap sentinel */
static int i_dart_q_fit(i_DartMsgQueue *q, uint32_t need, uint32_t *at){
    if (!q->buf || need > q->cap) return 0;
    if (q->count == 0){ q->head = 0; q->tail = 0; *at = 0; return 1; }
    if (q->head > q->tail){
        if (q->cap - q->head >= need){ *at = q->head; return 1; }
        if (q->tail >= need){                                    /* wrap to the start */
            if (q->cap - q->head >= 4u)
                ((i_DartQRec*)(q->buf + q->head))->rec_bytes = DART__QWRAP;
            *at = 0; return 1;
        }
        return 0;
    }
    if (q->head < q->tail && q->tail - q->head >= need){ *at = q->head; return 1; }
    return 0;                                                    /* full (head == tail) */
}

/* Relinearizes into a bigger ring, doubling toward cap_limit. One over cap message still
 * fits. 0 = cannot grow now: at the cap, OOM, or a live take view pins the ring. */
static int i_dart_q_grow(DartNode *n, i_DartMsgQueue *q, uint32_t need_total, uint32_t need_one){
    uint32_t target = q->cap ? q->cap * 2u : 4096u;
    uint8_t *nb;
    if (q->viewing) return 0;
    if (target < 4096u) target = 4096u;
    while (target < need_total && target < 0x80000000u) target *= 2u;
    if (target > q->cap_limit) target = q->cap_limit;
    if (target < need_one) target = DART__QALIGN(need_one);      /* one message always fits */
    if (target <= q->cap) return 0;
    nb = (uint8_t*)i_dart_node_alloc(n, NULL, target);
    if (!nb) return 0;
    {   uint32_t off = 0, i, src = q->tail;                        /* compact, oldest first */
        for (i = 0; i < q->count; i++){
            const i_DartQRec *rec;
            if (q->cap - src < (uint32_t)sizeof(i_DartQRec) ||
                ((const i_DartQRec*)(q->buf + src))->rec_bytes == DART__QWRAP) src = 0;
            rec = (const i_DartQRec*)(q->buf + src);
            memcpy(nb + off, rec, rec->rec_bytes);
            off += rec->rec_bytes; src += rec->rec_bytes;
        }
        q->tail = 0; q->head = off;
    }
    if (q->buf) i_dart_node_alloc(n, q->buf, 0);
    q->buf = nb; q->cap = target;
    return 1;
}

/* best effort queue loss is loss like any other: DART_MSG_LOST, never silent */
static void i_dart_node_queue_lost(DartNode *n, uint16_t topic_index, uint32_t from, uint32_t count){
    DartEvent e; memset(&e, 0, sizeof e);
    e.kind = DART_MSG_LOST; e.topic = topic_index; e.peer = from;
    e.topic_name = i_dart_node_topic_name(n, topic_index);
    e.lost_count = count;
    i_dart_node_emit(n, &e);
}

/* Enqueues one message. 0 = accepted (stored, or dropped per the best effort contract),
 * 1 = refused, a reliable queue at cap: the transport parks and flow control backpressures. */
static int i_dart_node_queue_push(DartNode *n, uint16_t topic_index, i_DartMsgQueue *q,
                                  uint32_t from, DartBytes data, uint64_t written_us,
                                  uint64_t capture_us){
    DartString name = i_dart_node_core_peer_name(n->core, from);
    uint32_t name_len = name.len > DART_NODE_NAME_MAX ? (uint32_t)DART_NODE_NAME_MAX
                                                      : (uint32_t)name.len;
    uint32_t payload_off = DART__QALIGN(sizeof(i_DartQRec) + name_len);
    uint32_t need = DART__QALIGN(payload_off + data.len);
    uint32_t at = 0, evicted = 0;
    for (;;){
        if (i_dart_q_fit(q, need, &at)) break;
        if (i_dart_q_grow(n, q, q->bytes + need, need)) continue;
        if (q->reliable){ q->parked = 1; return 1; }
        if (!q->viewing && q->count){                        /* overwrite the oldest */
            i_dart_q_pop(q); q->dropped++; evicted++; continue;
        }
        q->dropped++;                     /* a live view pins the ring: drop the incoming */
        i_dart_node_queue_lost(n, topic_index, from, evicted + 1u);
        return 0;
    }
    {   i_DartQRec *rec = (i_DartQRec*)(q->buf + at);
        rec->rec_bytes = need; rec->data_len = (uint32_t)data.len;
        rec->t_recv_us = i_dart_plat_now_us();
        rec->t_written_us = written_us;
        rec->t_capture_us = capture_us;
        rec->publisher_id = from; rec->name_len = (uint8_t)name_len;
        rec->pad[0] = rec->pad[1] = rec->pad[2] = 0;
        if (name_len) memcpy((uint8_t*)rec + sizeof *rec, name.data, name_len);
        if (data.len) memcpy(q->buf + at + payload_off, data.data, data.len);
        q->head = at + need;
        q->bytes += need; q->count++;
    }
    if (evicted) i_dart_node_queue_lost(n, topic_index, from, evicted);
    return 0;
}

/* A queued DartMsg: every view points at stable memory, valid until the next take or
 * dispatch. The schema is re resolved now, since the delivery map may repoint. */
static void i_dart_node_queue_msg(DartNode *n, DartTopic *h, const i_DartQRec *rec, DartMsg *m){
    memset(m, 0, sizeof *m);
    m->node = n; m->user = n->user_data;
    m->topic_index = h->index;
    m->publisher_id = rec->publisher_id;
    m->publisher_name = rec->name_len ? dart_string((const char*)(rec + 1), rec->name_len)
                                   : dart_cstr("unknown-peer");
    m->topic_name = dart_string(h->name, h->name_len);
    i_dart_node_split(h, dart_bytes((const uint8_t*)rec + DART__QALIGN(sizeof *rec + rec->name_len),
                         rec->data_len), &m->header, &m->data);
    m->schema = i_dart_node_core_msg_schema(n->core, rec->publisher_id, h->index);
    if (h->prefix_bytes && m->data.len == 0) m->schema = NULL;   /* an op only pattern message */
    m->recv_us = rec->t_recv_us;
    m->written_us = rec->t_written_us;
    m->capture_us = rec->t_capture_us;
}

/* Creates the queue. An explicit queue_bytes allocates in full, the lazy default starts at
 * one page and grows toward DART_QUEUE_CAP. NULL on OOM. */
static i_DartMsgQueue *i_dart_node_queue_ensure(DartNode *n, DartTopic *h, const DartQos *qos){
    i_DartMsgQueue *q = h->q;
    uint32_t limit, initial;
    if (q) return q;
    if (!qos) qos = dart_transport_topic_qos(n->transport, h->index);
    limit = DART__QALIGN((qos && qos->queue_bytes) ? qos->queue_bytes : DART_QUEUE_CAP);
    initial = (qos && qos->queue_bytes) ? limit : (limit < 4096u ? limit : 4096u);
    q = (i_DartMsgQueue*)i_dart_node_alloc(n, NULL, sizeof *q);
    if (!q) return NULL;
    memset(q, 0, sizeof *q);
    q->buf = (uint8_t*)i_dart_node_alloc(n, NULL, initial);
    if (!q->buf){ i_dart_node_alloc(n, q, 0); return NULL; }
    q->cap = initial; q->cap_limit = limit;
    q->reliable = (qos && qos->reliability == DART_RELIABLE) ? 1u : 0u;
    h->q = q;
    return q;
}

/* Finishes an outstanding take view, then retries parked lanes into the freed space. Their
 * acks need a TX pass, so kick. Lock held. */
static void i_dart_node_queue_release(DartNode *n, DartTopic *h, i_DartMsgQueue *q){
    if (q->viewing){
        q->viewing = 0;
        i_dart_q_pop(q);
    }
    if (q->parked){
        q->parked = dart_transport_deliver_parked(n->transport, h->index,
                                                  i_dart_plat_now_us()) ? 1u : 0u;
        i_dart_node_kick(n);
    }
}

/* The process global last error slot for failures during open, before the node exists.
 * A plain global, meaningful right after a failed open on the calling thread. */
static DartEvent g_last_error;

/* Reports an open time failure into the global slot and the caller's on_event. Returns NULL. */
static DartNode *i_dart_node_open_fail(DartEventFn on_event, void *user, DartErrorKind err,
                            int os_error, uint16_t port, uint64_t need){
    DartEvent e; memset(&e, 0, sizeof e);
    e.kind = DART_ERROR; e.error = err; e.user = user;
    e.os_error = os_error; e.port = port; e.too_big_bytes = need;
    g_last_error = e;
    if (on_event) on_event(&e);
    return NULL;
}

DartEvent dart_last_error(DartNode *n){ return n ? n->last_error : g_last_error; }

/* Builds a DartMsg and hands it to the app, inline or copied into the consumer queue.
 * Returns 0 accepted, nonzero refused (a reliable queue at cap, the transport parks). */
static int i_dart_node_deliver(DartNode *n, uint16_t topic_index, uint32_t from, DartBytes data){
    DartMsg m;
    DartTopic *h = (topic_index < i_dart_node_topic_hi(n)) ? n->handles[topic_index] : NULL;
    DartBytes hdr, payload, body;
    uint64_t written_us, capture_us;
    const DartSchema *schema = i_dart_node_core_msg_schema(n->core, from, topic_index);
    /* the stamps first, then the header split, then the schema validates the payload */
    body = i_dart_node_strip_ts(data,
              dart_transport_peer_timestamped(n->transport, topic_index, from),
              &written_us, &capture_us);
    i_dart_node_split(h, body, &hdr, &payload);
    /* an op only pattern message (a zero payload, or a task op that is not CALL) skips
       the schema, the pattern layer judges it. See spec/node.md */
    if (h && h->prefix_bytes
        && (payload.len == 0
            || (h->kind == DART_KIND_TASK_REQ && hdr.len >= 5 && hdr.data[4] != 0)))
        schema = NULL;
    if (schema && !dart_schema_validate(schema, payload)){
        DartEvent e; memset(&e, 0, sizeof e);
        e.kind = DART_ERROR; e.error = DART_E_SCHEMA_MISMATCH;
        e.peer = from; e.topic = topic_index; e.topic_name = i_dart_node_topic_name(n, topic_index);
        e.schema_detail = i_dart_node_core_note_size_mismatch(n->core, from, topic_index,
                                            payload.len, dart_schema_msg_min(schema));
        i_dart_node_emit(n, &e);
        return 0;
    }
    if (h && h->q){
        /* store the wire minus the stamps, the stamps ride the record */
        if (i_dart_node_queue_push(n, topic_index, h->q, from, body, written_us, capture_us))
            return 1;   /* parked: the accepted retry re counts */
        h->rx_msgs++; h->rx_bytes += data.len;
        return 0;
    }
    if (h){ h->rx_msgs++; h->rx_bytes += data.len; }
    if (!(h && h->sys_on_message) && !n->user_on_message) return 0;
    memset(&m, 0, sizeof m);
    m.node = n; m.user = n->user_data;
    m.topic_index = topic_index; m.publisher_id = from;
    m.publisher_name = i_dart_node_core_peer_name(n->core, from);          /* a discovery view */
    if (!m.publisher_name.data) m.publisher_name = dart_cstr("unknown-peer"); /* never NULL */
    m.topic_name = dart_transport_topic_name(n->transport, topic_index);
    m.header = hdr; m.data = payload;
    m.schema = schema;
    m.recv_us = i_dart_plat_now_us();
    m.written_us = written_us;
    m.capture_us = capture_us;
    if (h && h->sys_on_message) h->sys_on_message(h->sys_msg_user, &m);    /* the patterns layer */
    else n->user_on_message(&m);
    return 0;
}
static int i_dart_node_on_message(void *u, uint16_t topic_index, uint32_t from, DartBytes data){
    return i_dart_node_deliver((DartNode*)u, topic_index, from, data);
}
/* Node core events funnel through here. The core sets ev->user to this node, swap it for
 * the app's user_data before handing on. */
static void i_dart_node_on_event(const DartEvent *ev){
    DartNode *n = (DartNode*)ev->user;
    /* dynamic mode has no peer cap: a refusal means grow at the next poll, out of this
       callback, and the peer's next announce is admitted. Static mode surfaces it */
    if (n->alloc_dynamic && ev->kind == DART_ERROR && ev->error == DART_E_PEER_REFUSED){
        n->grow_pending = 1; return;
    }
    /* a blob we cannot hold is a growth signal too: grow at the next poll, then solicit.
       Sizes past the wire ceiling and a size that already failed surface */
    if (n->alloc_dynamic && ev->kind == DART_ERROR && ev->error == DART_E_PEER_META_TOO_BIG){
        uint64_t need = ev->too_big_bytes;
        if (need > n->meta_cap && need <= 65000u && (uint16_t)need != n->meta_grow_failed){
            n->meta_grow_need = (uint16_t)need;
            return;
        }
    }
    { DartEvent e = *ev; i_dart_node_emit(n, &e); }
}

/* the transport's schema gate, answered by the node core. u is the node */
static int i_dart_node_schema_check(void *u, uint32_t peer, uint16_t topic_index,
                                    int peer_is_pub, uint64_t hash, DartBytes wire){
    return i_dart_node_core_schema_check(((DartNode*)u)->core, peer, topic_index,
                                         peer_is_pub, hash, wire);
}

/* the transport's source clock: the wall clock, since it is read on other hosts */
static uint64_t i_dart_node_source_time(void *u){ (void)u; return i_dart_plat_wall_us(); }

/* Maps a transport event onto the app DartEvent. MSG_LOST is an info kind, everything
 * else is a DART_ERROR with its DartErrorKind, and topic scoped kinds get our name. */
static void i_dart_node_on_transport_event(const DartTransportEvent *tev){
    DartNode *n = (DartNode*)tev->user;
    DartEvent e;
    memset(&e, 0, sizeof e);
    e.peer = tev->peer; e.topic = tev->topic;
    e.lost_first = tev->lost_first; e.lost_count = tev->lost_count;
    e.too_big_bytes = tev->too_big_bytes; e.identity = tev->identity;
    switch (tev->kind){
    case DART_TRANSPORT_MSG_LOST:
        e.kind = DART_MSG_LOST; e.topic_name = i_dart_node_topic_name(n, tev->topic); break;
    case DART_TRANSPORT_MSG_TOO_BIG:
        e.kind = DART_ERROR; e.error = DART_E_MSG_TOO_BIG; e.topic_name = i_dart_node_topic_name(n, tev->topic); break;
    case DART_TRANSPORT_NAME_COLLISION:
        e.kind = DART_ERROR; e.error = DART_E_NAME_COLLISION; e.topic_name = i_dart_node_topic_name(n, tev->topic); break;
    case DART_TRANSPORT_QOS_INCOMPATIBLE:
        e.kind = DART_ERROR; e.error = DART_E_QOS_INCOMPATIBLE; e.topic_name = i_dart_node_topic_name(n, tev->topic); break;
    case DART_TRANSPORT_KIND_MISMATCH:
        e.kind = DART_ERROR; e.error = DART_E_KIND_MISMATCH; e.topic_name = i_dart_node_topic_name(n, tev->topic); break;
    case DART_TRANSPORT_SCHEMA_MISMATCH:
        e.kind = DART_ERROR; e.error = DART_E_SCHEMA_MISMATCH; e.topic_name = i_dart_node_topic_name(n, tev->topic);
        e.schema_detail = i_dart_node_core_schema_why(n->core, tev->peer, tev->topic,
                                                      tev->peer_is_pub); break;
    case DART_TRANSPORT_INTEREST_OVERFLOW:
        e.kind = DART_ERROR; e.error = DART_E_INTEREST_OVERFLOW; break;
    case DART_TRANSPORT_META_TRUNCATED_INTEREST:
        e.kind = DART_ERROR; e.error = DART_E_META_TRUNCATED_INTEREST; break;
    case DART_TRANSPORT_META_TRUNCATED_SCHEMA:
        e.kind = DART_ERROR; e.error = DART_E_META_TRUNCATED_SCHEMA; break;
    default: return;
    }
    i_dart_node_emit(n, &e);
}

/* The arena's sub blocks, laid out in one place so the measure pass and the build pass
 * run the same sequence and never drift. */
typedef struct {
    uint8_t   *handles, *node_core, *transport, *discovery, *rx_buf;
#ifdef DART_SHM
    uint8_t   *shm_pool;
#endif
    size_t     node_core_bytes, transport_bytes, discovery_bytes, rx_buf_bytes;
} i_DartNodeBlocks;

static void i_dart_node_layout(i_DartBump *b, uint16_t max_peers, uint16_t max_topics,
                              const DartConfig *transport_cfg,
                              const DartDiscoveryNetConfig *discovery_rt_cfg, i_DartNodeBlocks *o){
    o->handles       = (uint8_t*)i_dart_bump_take(b, (size_t)max_topics * sizeof(DartTopic*), 16);
    o->node_core_bytes = i_dart_node_core_required_memory(max_topics);   /* no peers here */
    o->node_core = (uint8_t*)i_dart_bump_take(b, o->node_core_bytes, 16);
    o->transport_bytes = dart_transport_required_memory(transport_cfg);
    o->transport = (uint8_t*)i_dart_bump_take(b, o->transport_bytes, 16);
#ifdef DART_SHM
    /* only the (topic, class) segment pointer table lives in the arena */
    o->shm_pool = (uint8_t*)i_dart_bump_take(b, (size_t)max_topics * DART_SHM_N_CLASSES * sizeof(void*), 16);
#endif
    o->discovery_bytes = dart_discovery_placement_memory(discovery_rt_cfg);
    o->discovery = (uint8_t*)i_dart_bump_take(b, o->discovery_bytes, 16);
    /* the RX buffer fits a unicast announce carrying the largest blob we accept, since
       recvfrom drops an oversized datagram and a late joiner has no other path to it */
    o->rx_buf_bytes = dart_discovery_wire_size(discovery_rt_cfg->discovery.meta_cap);
    if (o->rx_buf_bytes < DART_DGRAM_MAX) o->rx_buf_bytes = DART_DGRAM_MAX;
    o->rx_buf = (uint8_t*)i_dart_bump_take(b, o->rx_buf_bytes, 16);
}

/* Sends one datagram to a peer. 1 when done with it, 0 only on a would block TX full. */
static int i_dart_node_tx(DartNode *n, uint32_t to, const uint8_t *buf, size_t len){
    i_DartNodeDest d;
    if (!i_dart_node_core_resolve(n->core, to, &d)) return 1;   /* the peer vanished */
    if (i_dart_plat_send(n->fd, buf, len, d.ip, d.port) < 0){
        if (i_dart_plat_would_block()) return 0;                /* TX full: retry next tick */
        {   /* a hard failure: report and drop, reliable data is repaired. Byte 0 is type and
               flags, bytes 1 and 2 the first submessage's topic index */
            DartEvent e; memset(&e, 0, sizeof e);
            e.kind = DART_ERROR; e.error = DART_E_SEND; e.peer = to;
            e.os_error = i_dart_plat_last_socket_error();
            e.too_big_bytes = len;
            if (len >= 3){
                uint16_t idx = (uint16_t)(buf[1] | ((uint16_t)buf[2] << 8));
                e.topic = idx;
                e.topic_name = i_dart_node_topic_name(n, idx);
            }
            i_dart_node_emit(n, &e);
        }
    }
    return 1;
}

#ifdef DART_SHM
/* Lazily creates our per topic segment: chunk_bytes is the size class, n_chunks the
 * keep_last, so history slot i binds chunk i. NULL on failure. */
static i_DartShmPool *i_dart_node_shm_topic_pool(DartNode *n, uint16_t topic_index, uint32_t k, uint16_t keep_last){
    i_DartShmConfig c; uint8_t *mem; uint64_t seg; size_t idx;
    if (k >= DART_SHM_N_CLASSES) return NULL;
    idx = (size_t)topic_index * DART_SHM_N_CLASSES + k;
    if (n->shm_pool[idx]) return (i_DartShmPool*)n->shm_pool[idx];
    memset(&c, 0, sizeof c);
    seg = n->shm_base | ((uint64_t)topic_index << 3) | (uint64_t)k;   /* class low, topic above */
    c.segment_id = seg;
    i_dart_shm_seg_name(c.name, seg);
    c.chunk_bytes = i_dart_shm_class_bytes(k);
    c.n_chunks = keep_last ? keep_last : 1u;
    mem = (uint8_t*)i_dart_node_alloc(n, NULL, i_dart_shm_state_bytes());   /* stable */
    if (!mem) return NULL;
    n->shm_pool[idx] = i_dart_shm_create(mem, &c);
    if (!n->shm_pool[idx]) i_dart_node_alloc(n, mem, 0);
    return (i_DartShmPool*)n->shm_pool[idx];
}
/* Lazily attaches a peer's segment by id and caches it. The cache grows with segments
 * actually attached, so a node with no same host peer holds none. */
static i_DartShmPool *i_dart_node_shm_reader_pool(DartNode *n, uint64_t seg){
    uint16_t i; i_DartShmConfig c; uint8_t *mem;
    for (i=0;i<n->shm_reader_count;i++)
        if (n->shm_reader_segments[i]==seg) return (i_DartShmPool*)n->shm_reader_states[i];
    if (n->shm_reader_count == n->shm_reader_cap){        /* grow the id and state arrays */
        uint16_t ncap = n->shm_reader_cap ? (uint16_t)(n->shm_reader_cap*2u) : 8u;
        uint64_t *nseg; void **nst;
        if (ncap <= n->shm_reader_cap) return NULL;       /* u16 wrap: an absurd segment count */
        nseg = (uint64_t*)i_dart_node_alloc(n, n->shm_reader_segments, (size_t)ncap*sizeof(uint64_t));
        if (!nseg) return NULL;
        n->shm_reader_segments = nseg;
        nst = (void**)i_dart_node_alloc(n, n->shm_reader_states, (size_t)ncap*sizeof(void*));
        if (!nst) return NULL;
        n->shm_reader_states = nst;
        n->shm_reader_cap = ncap;
    }
    memset(&c, 0, sizeof c);
    c.segment_id = seg; i_dart_shm_seg_name(c.name, seg);     /* attach reads the geometry */
    mem = (uint8_t*)i_dart_node_alloc(n, NULL, i_dart_shm_state_bytes());
    if (!mem) return NULL;
    if (!i_dart_shm_attach(mem, &c)){ i_dart_node_alloc(n, mem, 0); return NULL; }
    n->shm_reader_segments[n->shm_reader_count] = seg;
    n->shm_reader_states[n->shm_reader_count] = mem;
    n->shm_reader_count++;
    return (i_DartShmPool*)mem;
}
static int i_dart_node_on_shm(void *u, uint16_t topic_index, uint32_t from, const uint8_t *desc){
    DartNode *n = (DartNode*)u; i_DartShmDesc d; i_DartShmPool *reader_pool; const void *p; uint32_t len;
    if (!i_dart_shm_desc_decode(&d, desc, DART_SHM_DESC_WIRE)) return 0;
    reader_pool = i_dart_node_shm_reader_pool(n, d.segment_id);
    if (!reader_pool) return 0;                                     /* cannot attach: NACK */
    p = i_dart_shm_read(reader_pool, &d, &len);                       /* the seqlock head */
    if (!p) return 0;                                      /* recycled: NACK, then repair or skip */
    /* one copy out of shared memory so the user owns the bytes */
    if (len > n->shm_scratch_cap){
        void *new_buf = i_dart_node_alloc(n, n->shm_scratch, len?len:1u);
        if (!new_buf) return 0;
        n->shm_scratch = new_buf; n->shm_scratch_cap = len;
    }
    memcpy(n->shm_scratch, p, len);
    if (!i_dart_shm_verify(reader_pool, &d)) return 0;                /* the seqlock tail: torn */
    if (i_dart_node_deliver(n, topic_index, from, dart_bytes(n->shm_scratch, len)))
        return -1;                                         /* the queue is full: park */
    n->shm_rx++;
    return 1;
}
#endif

static void i_dart_node_logs_open(DartNode *n);   /* defined with the log API below */

DartAllocator dart_allocator_heap(uint32_t page_size){
    return dart_allocator_dynamic(i_dart_plat_realloc, page_size);
}

void *dart_heap_realloc(void *user, void *ptr, size_t size){
    (void)user;
    return i_dart_plat_realloc(ptr, size);
}

DartNode *dart_node_open(DartAllocator *alloc, const char *name, DartMsgFn on_message, DartEventFn on_event, const DartNodeOpts *opts){
    DartNodeOpts o; DartDiscoveryNetConfig dc; DartConfig tc; i_DartNodeBlocks blocks;
    uint16_t max_peers, max_topics, user_topics;
    uint8_t *base; void *arena; size_t need; DartAllocator pool;
    DartNode *n; i_DartSock fd; uint16_t local_port;
    char node_name[DART_NODE_NAME_MAX + 1]; uint8_t node_name_len = 0;

    memset(&o, 0, sizeof o);
    if (opts) o = *opts;
    /* a node has no memory of its own, so no allocator is the same fault as one that
       returns NULL. Reported, never a bare NULL with an empty last error. */
    if (!alloc) return i_dart_node_open_fail(on_event, o.user_data, DART_E_OOM, 0, 0, sizeof *n);
    user_topics = o.max_topics ? o.max_topics : 8;
    /* the builtins ride outside the app's budget, in a block above it */
    max_topics = (uint16_t)(user_topics + (o.disable_logs ? 0 : 3));
#ifndef DART_NO_PATTERNS
    max_topics = (uint16_t)(max_topics + (o.disable_meta ? 0 : 2));
#endif
    max_peers    = o.discovery.max_peers ? o.discovery.max_peers : 16;

    /* the sub configs. Sizing depends only on counts, the callbacks are set later */
    memset(&dc,0,sizeof dc); memset(&tc,0,sizeof tc);
    dc.discovery.domain_id   = o.domain;
    dc.discovery.data_port   = o.net.data_port;
    dc.discovery.announce_interval_us = o.discovery.announce_interval_us;
    dc.discovery.peer_timeout_us = o.discovery.peer_timeout_us;
    dc.discovery.max_peers   = max_peers;
    dc.discovery.meta_cap = dart_meta_cap(max_topics);
    dc.discovery.peer_user_bytes = i_dart_node_core_peer_user_bytes();   /* the core's scratch */
    dc.discovery.alloc = i_dart_node_alloc;   /* per peer blobs at their size. Set before sizing so
                                                 measure and place agree. alloc_user is set below */
    dc.group                 = o.net.discovery_group;
    dc.discovery_port        = o.net.discovery_port;
    dc.ttl                   = o.net.multicast_ttl;
    dc.multicast_interface   = o.net.multicast_interface;
    dc.seeds                 = o.net.seed_peers;
    dc.n_seeds               = o.net.n_seed_peers;
    dc.unicast_only          = o.net.unicast_only;
    tc.topics    = NULL;            /* reserve mode: topics created at runtime */
    tc.n_topics  = max_topics;
    tc.max_peers   = max_peers;
    tc.frag_size= o.net.fragment_size;
    tc.allocator   = i_dart_node_alloc;   /* required by dart_transport_init */

    {   i_DartBump b; memset(&b,0,sizeof b);
        i_dart_node_layout(&b, max_peers, max_topics, &tc, &dc, &blocks);
        need = b.offset + 32u; }

    /* The node copies the caller's allocator into its own pool, so the caller's may be a
       temporary. The node struct stays put across a grow, the arena is relocated. */
    pool = *alloc;
    n = (DartNode*)dart_allocator_alloc(&pool, NULL, sizeof *n);
    if (!n) return i_dart_node_open_fail(on_event, o.user_data, DART_E_OOM, 0, 0, sizeof *n);
    memset(n, 0, sizeof *n);
    n->pool = pool;                                  /* the node owns the pool now */
    n->alloc_dynamic = (alloc->page_realloc != NULL);
    arena = dart_allocator_alloc(&n->pool, NULL, need);
    if (!arena){ DartAllocator p = n->pool; dart_allocator_reset(&p);
                 return i_dart_node_open_fail(on_event, o.user_data, DART_E_OOM, 0, 0, need); }
    base = (uint8_t*)(((uintptr_t)arena+15u)&~(uintptr_t)15u);
    {   i_DartBump b; memset(&b,0,sizeof b);
        b.base = base; b.cap = need - (size_t)(base - (uint8_t*)arena);
        i_dart_node_layout(&b, max_peers, max_topics, &tc, &dc, &blocks); }

    if (!i_dart_plat_startup()){
        DartAllocator p = n->pool; dart_allocator_reset(&p);
        return i_dart_node_open_fail(on_event, o.user_data, DART_E_PLATFORM, 0, 0, 0);
    }

    n->fd = DART_SOCK_BAD;
    n->user_data = o.user_data;      /* set early so emit can stamp any open time error */
    n->on_event = on_event;
#ifdef DART_THREADS
    i_dart_plat_mutex_init(&n->mu);
    i_dart_plat_cond_init(&n->cv);
    /* opened for every node: it also wakes a plain poll when another thread sends. A
       platform whose loopback cannot carry it degrades, reported once. See spec/node.md */
    if (!i_dart_plat_waker_open(&n->waker)){
        DartEvent e; memset(&e, 0, sizeof e);
        e.kind = DART_ERROR; e.error = DART_E_WAKER;
        i_dart_node_emit(n, &e);
    }
#endif
    n->domain = o.domain;
    n->net = o.net;
    n->announce_us = o.discovery.announce_interval_us ? o.discovery.announce_interval_us
                                                      : 3000000u;   /* discovery's default */
    n->match_wait_us = o.match_wait_ms < 0 ? 0u
                     : o.match_wait_ms ? (uint32_t)o.match_wait_ms * 1000u
                                       : (uint32_t)DART_MATCH_WAIT_MS * 1000u;
    n->match_epoch = 1;   /* a topic memo at 0 = never computed, so a first send always checks */
    n->user_on_message = on_message;
    n->arena = arena;
    n->handles = (DartTopic**)blocks.handles;
    memset(n->handles, 0, (size_t)max_topics * sizeof(DartTopic*));
    n->rx_buf = blocks.rx_buf; n->rx_buf_bytes = blocks.rx_buf_bytes;
    n->max_topics = max_topics;
    n->builtin_lo = user_topics;           /* the builtin block sits above the app's budget */
    n->max_peers = max_peers;
    n->meta_cap = dc.discovery.meta_cap;   /* the initial accept bound, self heals up */

    tc.on_message = i_dart_node_on_message;     /* wrapped so on_message receives a DartMsg */
    tc.on_event   = i_dart_node_on_transport_event;
    tc.schema_check = i_dart_node_schema_check;
    tc.source_time = i_dart_node_source_time;
    tc.user       = n;
#ifdef DART_SHM
    n->shm_capable = (uint8_t)(n->alloc_dynamic && !o.disable_shm);   /* never in static mode */
    if (n->shm_capable){
        i_dart_plat_host_uuid(n->shm_host);
        if (!i_dart_plat_random(&n->shm_base, sizeof n->shm_base)) n->shm_base = i_dart_plat_pid();
        n->shm_base ^= (uint64_t)i_dart_plat_pid() << 32;    /* unique per process */
        n->shm_base &= ~(((uint64_t)1u << 19) - 1u);       /* low 19 bits: 3 class, 16 topic */
        if (n->shm_base == 0) n->shm_base = (uint64_t)1u << 19;
    }
    tc.on_shm = i_dart_node_on_shm;
#endif

    n->transport = dart_transport_init(blocks.transport, blocks.transport_bytes, &tc);
    if (!n->transport){ (void)i_dart_node_open_fail(on_event, o.user_data, DART_E_OOM, 0, 0, 0); goto fail_threads; }
#ifdef DART_SHM
    {   uint32_t n_segments = (uint32_t)max_topics * DART_SHM_N_CLASSES; uint32_t i;
        n->shm_n_topics = max_topics;
        n->shm_pool = (void**)blocks.shm_pool;
        for (i=0;i<n_segments;i++) n->shm_pool[i]=NULL;
        n->shm_reader_segments = NULL; n->shm_reader_states = NULL;   /* lazy */
        n->shm_reader_cap = 0; n->shm_reader_count = 0;
    }
#endif

    /* the sans-IO node core, bound to discovery's peer table below once it exists */
    node_name_len = dart_discovery_default_name(node_name, sizeof node_name, name);
    memcpy(n->name, node_name, node_name_len); n->name[node_name_len] = '\0';   /* snapshot copy */
    n->name_len = node_name_len;
    {   i_DartNodeCoreConfig cc;
        memset(&cc, 0, sizeof cc);
        cc.transport = n->transport;   /* cc.discovery is bound after dart_discovery_place */
        cc.n_topics = max_topics; cc.frag_size = dart_clamp_frag(o.net.fragment_size);
        cc.on_event = i_dart_node_on_event; cc.user = n;
        cc.alloc = i_dart_node_alloc; cc.alloc_user = n;   /* backs the peer schema state */
        cc.fetch_details = o.fetch_details;
#ifdef DART_SHM
        cc.oob_capable = n->shm_capable; memcpy(cc.oob_host, n->shm_host, 16);
#endif
        n->core = i_dart_node_core_init(blocks.node_core, blocks.node_core_bytes, &cc);
        if (!n->core){ (void)i_dart_node_open_fail(on_event, o.user_data, DART_E_OOM, 0, 0, 0); goto fail_threads; }
    }

    /* the data socket is bound before discovery opens so we advertise its real port. No
       reuse: a unicast endpoint owns its port, so a collision fails loudly here */
    fd = i_dart_plat_udp_open();
    if (fd==DART_SOCK_BAD){ (void)i_dart_node_open_fail(on_event, o.user_data, DART_E_SOCKET, i_dart_plat_last_socket_error(), 0, 0); goto fail_threads; }
    n->fd=fd;                       /* owned now: fail_sock closes it */
    if (!i_dart_plat_bind(fd, 0, o.net.data_port, 0)){ (void)i_dart_node_open_fail(on_event, o.user_data, DART_E_BIND, i_dart_plat_last_socket_error(), o.net.data_port, 0); goto fail_sock; }
    local_port = i_dart_plat_local_port(fd);
    if (local_port==0){ (void)i_dart_node_open_fail(on_event, o.user_data, DART_E_SOCKET, 0, 0, 0); goto fail_sock; }
    i_dart_plat_set_nonblock(fd);   /* never block in recv or send, poll drains the queue */
    i_dart_plat_suppress_connreset(fd);  /* suppress WSAECONNRESET from a bounced send */
    if (o.net.recv_buffer_bytes) i_dart_plat_set_rcvbuf(fd, (int)o.net.recv_buffer_bytes);
    if (o.net.send_buffer_bytes) i_dart_plat_set_sndbuf(fd, (int)o.net.send_buffer_bytes);
    /* our advertised locator: the port we really bound and no address, unless the caller
       states one outright. See docs/discovery.md */
    dc.discovery.data_port = o.net.advertise_port ? o.net.advertise_port : local_port;
    if (o.net.self_ip){
        uint32_t naddr = i_dart_plat_parse_ip(o.net.self_ip);
        /* 0 and 0xFFFFFFFF are inet_addr's failure value and the broadcast address, neither
           a unicast locator, so a bad string is a config error */
        if (!naddr || naddr == 0xFFFFFFFFu){
            (void)i_dart_node_open_fail(on_event, o.user_data, DART_E_BAD_ADDRESS, 0, 0, 0);
            goto fail_sock;
        }
        i_dart_plat_naddr_to_ip4(naddr, dc.discovery.self_ip);
        dc.discovery.self_ip_len = 4;
    }

    dc.discovery.on_event = i_dart_node_core_on_disc_event;   /* the core demuxes peer events */
    dc.discovery.user     = n->core;
    dc.discovery.alloc_user = n;               /* the blob hook allocates from the node's pool */
    dc.discovery.name     = dart_string(node_name, node_name_len);   /* discovery owned */
    /* the core builds our overlay, discovery wraps it in its blob after the locator and name */
    i_dart_node_core_build_meta(n->core);
    dc.discovery.meta = i_dart_node_core_meta(n->core);
    n->discovery = dart_discovery_place(blocks.discovery, blocks.discovery_bytes, &dc);
    if (!n->discovery){
        DartErrorKind err;   /* discovery's setup failure in our vocabulary */
        switch (dart_discovery_last_error()){
        case DART_DISCOVERY_E_PLATFORM:   err = DART_E_PLATFORM;   break;
        case DART_DISCOVERY_E_SOCKET:     err = DART_E_SOCKET;     break;
        case DART_DISCOVERY_E_BIND:       err = DART_E_BIND;       break;
        case DART_DISCOVERY_E_MCAST_JOIN: err = DART_E_MCAST_JOIN; break;
        default:                          err = DART_E_OOM;        break;
        }
        (void)i_dart_node_open_fail(on_event, o.user_data, err, dart_discovery_last_os_error(),
                                    o.net.discovery_port, 0);
        goto fail_sock;
    }
    /* the node core delegates address resolution and per peer scratch to discovery's table */
    i_dart_node_core_bind_discovery(n->core, dart_discovery_state(n->discovery));
    i_dart_node_core_set_self_name(n->core, dart_string(n->name, strlen(n->name)));

    /* every unicast discovery send goes out of the data socket, so a NAT's per flow
       mappings are the ones the data will use. See spec/discovery.md */
    dart_discovery_set_tx_fd(n->discovery, fd);

    /* the post open gather anchor: discovery solicits on startup, so every peer already
       out there answers within an RTT of the first poll */
    n->open_us = i_dart_plat_now_us();

    /* the builtins last, since they create topics. A failed creation degrades and never
       fails the open */
    n->log_errors = (uint8_t)(!o.disable_error_logs && !o.disable_logs);
    n->creating_builtin = 1;               /* allocate from the builtin block */
    if (!o.disable_logs) i_dart_node_logs_open(n);
#ifndef DART_NO_PATTERNS
    if (!o.disable_meta) i_dart_patterns_meta_open(n);
#endif
    n->creating_builtin = 0;
    return n;

fail_sock:
    if (n->fd != DART_SOCK_BAD) i_dart_plat_close(n->fd);
    n->fd = DART_SOCK_BAD;
fail_threads:
#ifdef DART_THREADS
    if (n->waker.fd != DART_SOCK_BAD) i_dart_plat_waker_close(&n->waker);
    i_dart_plat_cond_destroy(&n->cv);
    i_dart_plat_mutex_destroy(&n->mu);
#endif
    i_dart_plat_cleanup();
    { DartAllocator p = n->pool; dart_allocator_reset(&p); }   /* frees the node struct and arena */
    return NULL;
}

/* Dynamic mode growth: relocates the node into a bigger arena at the given counts. Message
 * buffers, SHM segments and the user held handles stay put. 0 leaves n unchanged. */
static int i_dart_node_grow(DartNode *n, uint16_t new_max_peers, uint16_t new_max_topics,
                            uint16_t want_meta_cap){
    DartConfig tc; DartDiscoveryNetConfig dc; i_DartNodeBlocks nb; i_DartBump b;
    DartTransportState *nt; i_DartNodeCore *ncore; DartDiscovery *ndisc;
    void *new_arena, *old_arena = n->arena;
    uint8_t *nbase; size_t need;
    uint16_t old_max_topics = n->max_topics;
    /* the accept bound never shrinks: topic derived, previously grown, or requested */
    uint16_t new_meta_cap = dart_meta_cap(new_max_topics);
    if (n->meta_cap > new_meta_cap) new_meta_cap = n->meta_cap;
    if (want_meta_cap    > new_meta_cap) new_meta_cap = want_meta_cap;

    if (!n->alloc_dynamic) return 0;
    if (new_max_peers <= n->max_peers && new_max_topics <= n->max_topics
        && new_meta_cap <= n->meta_cap) return 0;

    memset(&tc,0,sizeof tc); memset(&dc,0,sizeof dc);
    tc.topics=NULL; tc.n_topics=new_max_topics; tc.max_peers=new_max_peers;
    tc.allocator=i_dart_node_alloc; tc.frag_size=n->net.fragment_size;
    dc.discovery.max_peers=new_max_peers; dc.discovery.meta_cap=new_meta_cap;
    dc.discovery.peer_user_bytes = i_dart_node_core_peer_user_bytes();   /* scratch to match */
    /* sizing must match the live core's hook mode: no arena blob pool */
    dc.discovery.alloc = i_dart_node_alloc; dc.discovery.alloc_user = n;

    memset(&b,0,sizeof b);
    i_dart_node_layout(&b, new_max_peers, new_max_topics, &tc, &dc, &nb);
    need = b.offset + 32u;
    new_arena = dart_allocator_alloc(&n->pool, NULL, need);
    if (!new_arena) return 0;
    nbase = (uint8_t*)(((uintptr_t)new_arena+15u)&~(uintptr_t)15u);
    memset(&b,0,sizeof b); b.base=nbase; b.cap=need-(size_t)(nbase-(uint8_t*)new_arena);
    i_dart_node_layout(&b, new_max_peers, new_max_topics, &tc, &dc, &nb);

    /* migrate the three cores. Each leaves the old intact, so a failure frees the new
       arena and the old node keeps running, only refusing the growth */
    nt = dart_transport_migrate(n->transport, nb.transport, nb.transport_bytes, new_max_peers, new_max_topics);
    if (!nt){ dart_allocator_alloc(&n->pool, new_arena, 0); return 0; }
    ncore = i_dart_node_core_migrate(n->core, nb.node_core, nb.node_core_bytes, new_max_topics);
    if (!ncore){ dart_allocator_alloc(&n->pool, new_arena, 0); return 0; }
    ncore->transport = nt;                         /* re point the cross layer pointer */
    i_dart_node_core_build_meta(ncore);              /* rebuild the blob in the new buffer */
    ndisc = dart_discovery_migrate(n->discovery, nb.discovery, nb.discovery_bytes,
                                      new_max_peers, new_meta_cap, i_dart_node_core_meta(ncore).data, ncore);
    if (!ndisc){ dart_allocator_alloc(&n->pool, new_arena, 0); return 0; }
    i_dart_node_core_bind_discovery(ncore, dart_discovery_state(ndisc));   /* the relocated table */

    /* the handle pointer array. The handle structs are stable and do not move */
    memcpy(nb.handles, n->handles, (size_t)old_max_topics*sizeof(DartTopic*));
    memset((DartTopic**)nb.handles + old_max_topics, 0,
           (size_t)(new_max_topics-old_max_topics)*sizeof(DartTopic*));

#ifdef DART_SHM
    if (n->shm_capable){
        uint32_t old_segs=(uint32_t)n->shm_n_topics*DART_SHM_N_CLASSES;
        uint32_t new_segs=(uint32_t)new_max_topics*DART_SHM_N_CLASSES, i;
        void **np = (void**)nb.shm_pool;
        for (i=0;i<new_segs;i++) np[i]=NULL;
        for (i=0;i<old_segs;i++) np[i]=n->shm_pool[i];   /* only the table moves */
        n->shm_pool=np; n->shm_n_topics=new_max_topics;
        /* the reader attach cache is hook allocated too, so attachments survive */
    }
#endif

    n->transport=nt; n->core=ncore; n->discovery=ndisc;
    n->handles=(DartTopic**)nb.handles;
    n->rx_buf=nb.rx_buf; n->rx_buf_bytes=nb.rx_buf_bytes;
    n->max_topics=new_max_topics; n->max_peers=new_max_peers;
    n->meta_cap=new_meta_cap;
    dart_allocator_alloc(&n->pool, old_arena, 0);    /* control structs only */
    n->arena=new_arena;
    return 1;
}

/* the local channel table behind DART_SELF reflection: every live handle as a peer sees it */
static void i_dart_node_reflect_self(DartNode *n){
    uint16_t i;
    i_dart_node_core_self_begin(n->core);
    for (i = 0; i < n->max_topics; i++){
        DartTopic *h = n->handles[i];
        const DartQos *q;
        if (!h) continue;
        q = dart_transport_topic_qos(n->transport, i);
        i_dart_node_core_self_channel(n->core, i, dart_string(h->name, h->name_len), h->kind, h->role,
                                      (uint8_t)(q ? q->reliability : 0),
                                      dart_transport_topic_attrs(n->transport, i), h->schema);
    }
    i_dart_node_core_self_end(n->core);
}

/* Our own interest changed: re advertise, replay known peers' interest against the new
 * state, stale the converged memos and kick so the announce goes out now. Lock held. */
static void i_dart_node_readvertise(DartNode *n){
    i_dart_node_reflect_self(n);
    i_dart_node_core_build_meta(n->core);
    dart_discovery_advertise(n->discovery, i_dart_node_core_meta(n->core));
    dart_discovery_replay(n->discovery);
    n->match_epoch++;
    i_dart_node_kick(n);
}

/* The shared topic create: the public create and the patterns layer's both funnel here.
 * kind, prefix_bytes and directed stamp the entity, allow_at permits the reserved '@'. */
static DartTopic *i_dart_node_create_impl(DartNode *n, const char *name, DartRole role,
                              const DartSchema *schema, const DartTopicOpts *opts,
                              uint8_t kind, uint8_t prefix_bytes, uint8_t directed, uint8_t attrs,
                              i_DartSysMsgFn sys_msg, void *sys_user, int allow_at){
    DartTopicDef def; DartTopic *h; uint16_t idx; int acquired;
    int reuse = 0;
    if (!n || !name) return NULL;
    if (!allow_at){   /* '@' is reserved for pattern channels */
        const char *s = name;
        while (*s){ if (*s=='@') return NULL; s++; }
    }
    acquired = i_dart_node_lock(n);
    if (!acquired) return NULL;   /* from a callback: a grow would move the arena mid delivery */
    if (n->creating_builtin){
        /* open time builtins fill their block. A full block degrades to no builtin, never a grow */
        idx = (uint16_t)(n->builtin_lo + n->n_builtin);
        if (idx >= n->max_topics){ i_dart_node_unlock(n, acquired); return NULL; }
    } else if ((reuse = dart_transport_topic_reuse_find(n->transport, name, kind, &idx)) != 0){
        /* a retired slot takes this create so churn never grows the table. Same identity,
           kind and schema (find = 2) relinks silently, anything else rebinds. See spec/interest.md */
    } else {
        idx = n->n_created;
        if (n->n_builtin && idx >= n->builtin_lo)
            idx = (uint16_t)(idx + n->n_builtin);   /* step over the builtin block */
        if (idx >= n->max_topics){       /* the reserve is full: grow or refuse (static) */
            uint16_t want = n->max_topics < 0x8000u ? (uint16_t)(n->max_topics*2u) : 0xFFFFu;
            if (want <= n->max_topics || !i_dart_node_grow(n, n->max_peers, want, 0)){
                i_dart_node_unlock(n, acquired);
                return NULL;
            }
        }
    }
    h = (DartTopic*)i_dart_node_alloc(n, NULL, sizeof *h);   /* stable: outlives any arena grow */
    if (!h){ i_dart_node_unlock(n, acquired); return NULL; }
    memset(h, 0, sizeof *h);
    if (opts) h->qos = opts->qos;
    if (opts && opts->reflect_from_mesh && kind == DART_KIND_TOPIC){
        /* fill what the caller left unspecified from the mesh: a reader takes the
           provider's schema, a writer the widest every reader accepts */
        const DartSchema *ms = NULL; uint8_t rel = 0;
        h->reflect = 1;
        i_dart_node_core_reflect_pick(n->core, DART_ENTITY_TOPIC, name, 0,
                                      dart_role_pubs((uint8_t)role), &ms, &rel, &h->generation);
        if (!schema) schema = ms;
        if (!h->qos.reliability) h->qos.reliability = rel ? DART_RELIABLE : DART_BEST_EFFORT;
    }
    if (schema){   /* copied into node memory so the caller's schema need not outlive the topic */
        DartBytes w = dart_schema_wire(schema);
        h->schema = dart_schema_parse(w.data, w.len, i_dart_node_alloc, n);
        if (!h->schema){ i_dart_node_alloc(n, h, 0); i_dart_node_unlock(n, acquired); return NULL; }
    }
    memset(&def, 0, sizeof def);
    def.name = name; def.role = (uint8_t)role;
    def.kind = kind; def.prefix_bytes = prefix_bytes; def.directed = directed; def.attrs = attrs;
    def.qos = h->qos;
    h->attrs = attrs; h->directed = directed;
    {   int rc;
        if (reuse){
            /* an identical binding rebinds silently, anything else bumps the slot's
               generation and publishes the rebind under the version the advertise stamps */
            uint64_t new_hash = h->schema ? dart_schema_hash(h->schema) : 0;
            int changed = (reuse != 2)
                       || (new_hash != i_dart_node_core_topic_schema_hash(n->core, idx));
            uint32_t rb = changed
                ? dart_discovery_meta_version(dart_discovery_state(n->discovery)) + 1u : 0u;
            rc = dart_transport_topic_reuse(n->transport, idx, &def, changed, rb);
            if (rc == 0 && changed) i_dart_node_core_topic_rebound(n->core, idx);
        } else {
            rc = dart_transport_topic_define(n->transport, idx, &def);
        }
        if (rc != 0){
            if (h->schema) dart_schema_free(h->schema, i_dart_node_alloc, n);
            i_dart_node_alloc(n, h, 0);
            i_dart_node_unlock(n, acquired);
            return NULL;
        }
    }
    h->n = n; h->index = idx; h->prefix_bytes = prefix_bytes; h->kind = kind;
    /* a response prefix is followed by [u8 len][message] on the wire, derived from the
       kind so every creator of the kind splits alike */
    h->prefix_string = (uint8_t)(kind == DART_KIND_FUNC_RSP || kind == DART_KIND_TASK_RSP);
    h->role = (uint8_t)role;
    h->sys_on_message = sys_msg; h->sys_msg_user = sys_user;
    {   /* the stable name copy, queued views must not point into the arena */
        size_t nl = strlen(name);
        if (nl > DART_TOPIC_NAME_MAX) nl = DART_TOPIC_NAME_MAX;
        memcpy(h->name, name, nl);
        h->name_len = (uint8_t)nl;
    }
    if (def.qos.queue_bytes)   /* queued from creation, best effort on OOM (take retries) */
        (void)i_dart_node_queue_ensure(n, h, &def.qos);
    if (h->schema || reuse)   /* advertise and gate with it. A reused slot must also clear
                                 the retired occupant's fingerprint when the new topic is untyped */
        i_dart_node_core_set_topic_schema(n->core, idx, h->schema);
    n->handles[idx] = h;
    if (n->creating_builtin) n->n_builtin++;
    else if (!reuse) n->n_created++;   /* a reused slot is already inside the dense region */
    /* the handle is installed before the publish: the replay fires peer events into the
       patterns layer and the app, which must never see a half created topic */
    i_dart_node_readvertise(n);
    i_dart_node_unlock(n, acquired);
    return h;
}

DartTopic *dart_node_create_topic(DartNode *n, const char *name, DartRole role,
                                      const DartSchema *schema, const DartTopicOpts *opts){
    return i_dart_node_create_impl(n, name, role, schema, opts, DART_KIND_TOPIC, 0, 0, 0, NULL, NULL, 0);
}

DartTopic *i_dart_node_create_pattern_topic(DartNode *n, const char *name, DartRole role,
                              const DartSchema *schema, const DartTopicOpts *opts,
                              uint8_t kind, uint8_t prefix_bytes, uint8_t directed, uint8_t attrs,
                              i_DartSysMsgFn on_msg, void *on_msg_user){
    return i_dart_node_create_impl(n, name, role, schema, opts, kind, prefix_bytes, directed, attrs,
                                   on_msg, on_msg_user, 1);
}

/* the built in log topics */

static int i_dart_node_do_send(DartNode *n, uint16_t topic_index, DartBytes data,
                               uint64_t capture_us, int may_wait);

static const char *const i_dart_log_topic_names[3] =
    { "@dart/log/error", "@dart/log/warn", "@dart/log/info" };

/* Creates the three log topics at open: reliable, keep_last = catch_up as the replayable
 * history, publish only, and no backpressure wait. NULL handles on OOM, never a failed open. */
static void i_dart_node_logs_open(DartNode *n){
    DartTopicOpts topt; int lvl;
    n->log_schema = dart_schema_compile(i_dart_node_alloc, n,
        "DartLog { wall_us: u64, mono_us: u64, text: string }", NULL);
    if (!n->log_schema) return;
    for (lvl = 0; lvl < 3; lvl++){
        memset(&topt, 0, sizeof topt);
        topt.qos.reliability = DART_RELIABLE;
        topt.qos.keep_last = topt.qos.catch_up = (lvl == DART_LOG_INFO) ? 8 : 16;
        /* backpressure_wait_us stays 0, so logs never block */
        n->log_topics[lvl] = i_dart_node_create_impl(n, i_dart_log_topic_names[lvl],
                                 DART_PUB_ONLY, n->log_schema, &topt,
                                 DART_KIND_TOPIC, 0, 0, 0, NULL, NULL, 1);
    }
}

/* Builds one DartLog message and publishes it on the level's topic. locked = 1 is the
 * mirror flush under the node lock, 0 the public thread safe path. */
static int i_dart_node_log_publish(DartNode *n, DartLogLevel level, const char *text,
                                   size_t text_len, uint64_t wall_us, uint64_t mono_us,
                                   int locked){
    uint8_t msg[20u + DART_LOG_MAX];   /* the fixed section (16) and the text frame header (4) */
    uint32_t len;
    DartTopic *h = n->log_topics[level];
    if (!h || !n->log_schema) return DART_ERR_NOSYS;
    if (!dart_schema_message_default(n->log_schema, msg, sizeof msg)) return DART_ERR_OOM;
    dart_set_uint(msg, sizeof msg, n->log_schema, "wall_us", wall_us);
    dart_set_uint(msg, sizeof msg, n->log_schema, "mono_us", mono_us);
    if (!dart_set_string(msg, sizeof msg, n->log_schema, "text", dart_string(text, text_len)))
        return DART_ERR_TOO_BIG;
    len = dart_schema_msg_len(n->log_schema, msg, sizeof msg);
    if (locked) return i_dart_node_do_send(n, h->index, dart_bytes(msg, len), 0, 0);
    return dart_topic_send(h, dart_bytes(msg, len), NULL);
}

int dart_node_log(DartNode *n, DartLogLevel level, const char *fmt, ...){
    char text[DART_LOG_MAX]; int tn;
    va_list ap;
    if (!n || (int)level < 0 || level > DART_LOG_INFO || !fmt) return DART_ERR_NO_TOPIC;
    if (!n->log_topics[level]) return DART_ERR_NOSYS;
    va_start(ap, fmt);
    tn = vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (tn < 0) tn = 0;                                       /* an encoding error: an empty line */
    if (tn >= (int)sizeof text) tn = (int)sizeof text - 1;    /* truncated at DART_LOG_MAX */
    return i_dart_node_log_publish(n, level, text, (size_t)tn,
                                   i_dart_plat_wall_us(), i_dart_plat_now_us(), 0);
}

int dart_node_log_text(DartNode *n, DartLogLevel level, const char *text, int len){
    size_t tl;
    if (!n || (int)level < 0 || level > DART_LOG_INFO || !text) return DART_ERR_NO_TOPIC;
    if (!n->log_topics[level]) return DART_ERR_NOSYS;
    tl = len < 0 ? strlen(text) : (size_t)len;
    if (tl >= DART_LOG_MAX) tl = DART_LOG_MAX - 1;   /* match the variadic path's truncation */
    return i_dart_node_log_publish(n, level, text, tl,
                                   i_dart_plat_wall_us(), i_dart_plat_now_us(), 0);
}

DartTopic *dart_node_log_topic(DartNode *n, DartLogLevel level){
    if (!n || (int)level < 0 || level > DART_LOG_INFO) return NULL;
    return n->log_topics[level];
}

/* Publishes the pending error mirror ring under the lock. A coalesced burst appends its
 * count and overflow between flushes becomes one summary line. */
static void i_dart_node_log_flush(DartNode *n){
    uint8_t i, cnt;
    if (!n->log_topics[DART_LOG_ERROR]){ n->log_pend_n = 0; n->log_pend_dropped = 0; return; }
    n->log_flushing = 1;
    cnt = n->log_pend_n; n->log_pend_n = 0;
    for (i = 0; i < cnt; i++){
        i_DartLogPend *p = &n->log_pend[i];
        char line[sizeof p->text + 16]; int ln;
        ln = (p->count > 1) ? snprintf(line, sizeof line, "%s (x%u)", p->text, (unsigned)p->count)
                            : snprintf(line, sizeof line, "%s", p->text);
        if (ln < 0) ln = 0;
        if (ln >= (int)sizeof line) ln = (int)sizeof line - 1;
        i_dart_node_log_publish(n, DART_LOG_ERROR, line, (size_t)ln, p->wall_us, p->mono_us, 1);
    }
    if (n->log_pend_dropped){
        char line[64];
        int ln = snprintf(line, sizeof line, "log mirror overflow: %u error events dropped",
                          (unsigned)n->log_pend_dropped);
        n->log_pend_dropped = 0;
        i_dart_node_log_publish(n, DART_LOG_ERROR, line, (size_t)(ln < 0 ? 0 : ln),
                                i_dart_plat_wall_us(), i_dart_plat_now_us(), 1);
    }
    n->log_flushing = 0;
}

/* the longest one poll tick drains RX before yielding to discovery and send */
#ifndef DART_RX_BUDGET_US
#define DART_RX_BUDGET_US 5000u
#endif

/* Drains the socket into the transport until empty or past the deadline. A full drain
 * avoids NACK storms. Distinct from the public dart_topic_drain. */
static void i_dart_node_rx_drain(DartNode *n, i_DartSock fd, uint64_t deadline){
    uint8_t *buf = n->rx_buf;
    for (;;){
        uint8_t src_ip[4]; uint16_t src_port;
        int r = i_dart_plat_recv(fd, buf, n->rx_buf_bytes, src_ip, &src_port);
        if (r<0){
            if (i_dart_plat_would_block()) break;        /* the queue is empty */
            {   /* a hard error: report and stop this tick, never spin on a wedged socket */
                DartEvent e; memset(&e, 0, sizeof e);
                e.kind = DART_ERROR; e.error = DART_E_RECV; e.os_error = i_dart_plat_last_socket_error();
                i_dart_node_emit(n, &e);
            }
            break;
        }
        if (r>0){
            if (r>=4 && buf[0]=='u' && buf[1]=='D' && buf[2]=='S' && buf[3]=='C'){
                /* a unicast announce aimed at our data port goes to discovery with its
                   source port, so a translated peer's observed source can bind */
                DartDiscoveryAddr src;
                memset(&src, 0, sizeof src);
                memcpy(src.ip, src_ip, 4); src.ip_len = 4; src.port = src_port;
                dart_discovery_feed(n->discovery, &src, dart_bytes(buf, (size_t)r));
            } else if (r>=5 && buf[0]=='u' && buf[1]=='D' && buf[2]=='T' && buf[3]=='L'){
                /* the pairwise detail exchange, stateless: a request is answered to its
                   source, a response feeds the pending match cycle. See spec/interest.md */
                if (buf[4]==DART_DETAIL_REQ){
                    DartBytes resp = i_dart_node_core_detail_respond(n->core, n->domain,
                                                                     dart_bytes(buf, (size_t)r));
                    if (resp.len) i_dart_plat_send(fd, resp.data, resp.len, src_ip, src_port);
                    {   /* the request names the version the peer applied: feed the rebind hold */
                        uint32_t from;
                        if (i_dart_node_core_id_for_addr(n->core, src_ip, src_port, &from)
                            && i_dart_node_core_seen_version(n->core, from,
                                   dart_detail_meta_version(dart_bytes(buf, (size_t)r))))
                            n->match_epoch++;
                    }
                } else if (buf[4]==DART_DETAIL_RESP){
                    uint32_t from;
                    if (i_dart_node_core_id_for_addr(n->core, src_ip, src_port, &from))
                        i_dart_node_core_apply_details(n->core, n->domain, from,
                                                       dart_bytes(buf, (size_t)r));
                } else if (buf[4]==DART_INTEREST_REQ){
                    /* interest paging: serve one page. Not a rebind hold confirmation, the
                       peer is still fetching that version */
                    DartBytes resp = i_dart_node_core_interest_respond(n->core, n->domain,
                                                                       dart_bytes(buf, (size_t)r));
                    if (resp.len) i_dart_plat_send(fd, resp.data, resp.len, src_ip, src_port);
                } else if (buf[4]==DART_INTEREST_RESP){
                    uint32_t from;
                    if (i_dart_node_core_id_for_addr(n->core, src_ip, src_port, &from))
                        i_dart_node_core_apply_interest_page(n->core, n->domain, from,
                                                             dart_bytes(buf, (size_t)r));
                }
            } else {
                uint32_t from;
                if (i_dart_node_core_id_for_addr(n->core, src_ip, src_port, &from))
                    dart_transport_on_datagram(n->transport, from, dart_bytes(buf, (size_t)r), i_dart_plat_now_us());
            }
        }
        if (i_dart_plat_now_us() >= deadline) break;      /* yield to discovery and send */
    }
}

/* threaded mode: the longest a send waits for a poller to hand unsent history to the
 * wire before overwriting it. One kicked pass normally clears it in microseconds. */
#ifndef DART_UNSENT_WAIT_US
#define DART_UNSENT_WAIT_US 20000u
#endif

/* Caps a wait at the transport's next timer, discovery's next announce and the patterns
 * tick, so each fires on time with no traffic. Lock held, pure compute. */
static int i_dart_node_wait_ms(DartNode *n, int timeout_ms){
    uint64_t next = dart_transport_next_deadline_us(n->transport);
    uint64_t due  = dart_discovery_next_due_us(dart_discovery_state(n->discovery));
    if (timeout_ms < 0) timeout_ms = 0;
    if (!next || due < next) next = due;   /* due is a real time (0 = now), next 0 = none */
    /* the patterns tick */
    if (n->sys_tick_next && (!next || n->sys_tick_next < next)) next = n->sys_tick_next;
    if (next){
        uint64_t t0 = i_dart_plat_now_us();
        uint64_t us = (next > t0) ? next - t0 : 0;
        int ms = (us >= (uint64_t)timeout_ms*1000u) ? timeout_ms   /* rounded up: no busy spin */
                                                    : (int)((us + 999u)/1000u);
        if (ms < timeout_ms) timeout_ms = ms;
    } else timeout_ms = 0;                 /* something is due right now */
    return timeout_ms;
}

static int i_dart_node_gather_done(DartNode *n, uint64_t now);   /* defined with the match wait */

/* One poll tick under the node lock: deferred grows, the wait, discovery's tick, RX drain,
 * TX flush. The one poll body. outer = 1 lets the wait drop the lock. See spec/node.md. */
static void i_dart_node_poll_locked(DartNode *n, int timeout_ms, int outer){
    uint8_t buf[DART_DGRAM_MAX]; uint32_t to; size_t out_len; uint64_t now;
    i_DartPollfd pfd[5]; int nfds = 0, wait_ms, poll_rc;
    int disc_slot = 0, disc_n = 0;
#ifdef DART_THREADS
    int waker_slot = -1;
#endif

    /* a peer was refused last tick for lack of slots: grow now, between ticks */
    if (n->grow_pending){
        uint16_t want = n->max_peers < 0x8000u ? (uint16_t)(n->max_peers*2u) : 0xFFFFu;
        n->grow_pending = 0;
        if (want > n->max_peers) i_dart_node_grow(n, want, n->max_topics, 0);
    }
    /* a peer's blob exceeded the accept bound last tick: raise it, then solicit so the
       peer re sends into buffers that fit. A failed grow is remembered, see spec/node.md */
    if (n->meta_grow_need){
        uint16_t want = n->meta_grow_need;
        n->meta_grow_need = 0;
        if (want > n->meta_cap){
            if (i_dart_node_grow(n, n->max_peers, n->max_topics, want)){
                n->meta_grow_failed = 0;
                dart_discovery_solicit(dart_discovery_state(n->discovery));
            } else
                n->meta_grow_failed = want;
        }
    }

    /* the discovery tick runs after the wait off its revents, so a pass costs one syscall */
    wait_ms = i_dart_node_wait_ms(n, timeout_ms);
    memset(pfd, 0, sizeof pfd);
    pfd[nfds].fd = n->fd; pfd[nfds].events = DART_POLLIN; nfds++;
#ifdef DART_THREADS
    if (n->waker.fd != DART_SOCK_BAD){
        waker_slot = nfds;
        pfd[nfds].fd = n->waker.fd; pfd[nfds].events = DART_POLLIN; nfds++;
    }
#endif
    {   /* discovery's sockets join the wait so an announce cuts a long sleep short. The
           fds are stable by value: a grow relocates structs, never sockets */
        i_DartSock dfds[2]; int i;
        disc_slot = nfds; disc_n = dart_discovery_pollfds(n->discovery, dfds);
        for (i = 0; i < disc_n; i++){ pfd[nfds].fd = dfds[i]; pfd[nfds].events = DART_POLLIN; nfds++; }
    }

#ifdef DART_THREADS
    if (outer){
        /* the wait runs unlocked so a sender on another thread is never blocked behind
           it. pollers_sleeping is a counter, several threads may poll the same node */
        n->pollers_sleeping++;
        i_dart_node_unlock_raw(n);
        poll_rc = i_dart_plat_poll(pfd, nfds, wait_ms);
        i_dart_node_lock_raw(n);
        n->pollers_sleeping--;
    } else {
        poll_rc = i_dart_plat_poll(pfd, nfds, wait_ms);
    }
    if (waker_slot >= 0 && (pfd[waker_slot].revents & DART_POLLIN)){
        /* revents gated, so an idle pass costs no drain syscall. A kick still in flight
           wakes the next wait, which drains and clears it, so no kick is ever lost */
        i_dart_plat_waker_drain(&n->waker);
        n->wake_signaled = 0;
    }
#else
    (void)outer;
    poll_rc = i_dart_plat_poll(pfd, nfds, wait_ms);
#endif
    if (poll_rc < 0){   /* the wait itself failed, a real fault such as a bad fd */
        DartEvent e; memset(&e, 0, sizeof e);
        e.kind = DART_ERROR; e.error = DART_E_POLL; e.os_error = i_dart_plat_last_socket_error();
        i_dart_node_emit(n, &e);
    }

    /* the discovery tick off this wait's readiness. A failed wait leaves revents zeroed,
       so the clock driven work still runs */
    dart_discovery_service(n->discovery,
                           disc_n > 0 && (pfd[disc_slot].revents & DART_POLLIN) != 0,
                           disc_n > 1 && (pfd[disc_slot + 1].revents & DART_POLLIN) != 0);
    if (pfd[0].revents & DART_POLLIN)
        i_dart_node_rx_drain(n, n->fd, i_dart_plat_now_us() + DART_RX_BUDGET_US);

    /* mirrored errors publish now, before the TX pull, so the lines ride this pass */
    if (n->log_pend_n || n->log_pend_dropped) i_dart_node_log_flush(n);

    now=i_dart_plat_now_us();
    /* the detail requester: drain queued requests and rearm every active peer once per
       announce interval while any went out. A sweep that sends nothing disarms the timer */
    if (i_dart_node_core_detail_any(n->core) || (n->next_detail_us && now >= n->next_detail_us)){
        i_DartNodeDest dst; size_t len; int sent = 0;
        if (n->next_detail_us && now >= n->next_detail_us)
            i_dart_node_core_detail_rearm(n->core);
        while ((len = i_dart_node_core_detail_req_next(n->core, n->domain, buf, sizeof buf, &dst)) != 0){
            i_dart_plat_send(n->fd, buf, len, dst.ip, dst.port);   /* best effort: retries heal */
            sent = 1;
        }
        n->next_detail_us = sent ? now + n->announce_us : 0;
    }
    /* the core already consumed any held datagram, so retry it before pulling new */
    if (n->tx_hold_len && i_dart_node_tx(n, n->tx_hold_peer, n->tx_hold, n->tx_hold_len))
        n->tx_hold_len = 0;
    if (!n->tx_hold_len)
        while (dart_transport_poll_send(n->transport,&to,buf,sizeof buf,&out_len,now)){
            if (!i_dart_node_tx(n, to, buf, out_len)){
                memcpy(n->tx_hold, buf, out_len);
                n->tx_hold_len = out_len; n->tx_hold_peer = to;
                break;          /* the TX buffer is full: yield this tick */
            }
            now=i_dart_plat_now_us();
        }

    /* latch the post open gather the moment it settles, so a later create's replay events
       cannot reset the quiet clock and make a first send re wait */
    if (!n->gather_done) (void)i_dart_node_gather_done(n, now);

    /* the patterns tick runs under the lock like a callback and returns the next deadline */
    if (n->sys_tick) n->sys_tick_next = n->sys_tick(n->sys_user, i_dart_plat_now_us());

#ifdef DART_THREADS
    n->work_seq++;
    if (n->cv_waiters) i_dart_plat_cond_broadcast(&n->cv);   /* acks or TX may have progressed */
#endif
}

int dart_node_poll(DartNode *n, int timeout_ms){
    int acquired;
    if (!n) return DART_ERR_STATE;
    acquired = i_dart_node_lock(n);
#ifdef DART_THREADS
    if (!acquired || n->svc_running){
        /* from inside a callback, or a service thread owns the loop: refuse loudly */
        i_dart_node_unlock(n, acquired);
        return DART_ERR_STATE;
    }
#endif
    i_dart_node_poll_locked(n, timeout_ms, acquired);
    i_dart_node_unlock(n, acquired);
    return 0;
}

/* The one blocking wait skeleton: sample the clock, ask done (which states the deadline),
 * run periodic, then sleep on the condvar or pump. See spec/node.md. */
typedef struct {
    /* nonzero = the wait is over, *deadline is this iteration's bound, UINT64_MAX = none */
    int    (*done)(DartNode *n, void *ctx, uint64_t now, uint64_t *deadline);
    void   (*periodic)(DartNode *n, void *ctx, uint64_t now);   /* optional, NULL = nothing */
    void    *ctx;
    uint64_t cv_cap_us;      /* the longest single sleep, 0 = the whole remaining time */
    int      pump_ms;        /* the pump tick in ms, 0 = the remaining time */
    int      pump_outer;     /* poll_locked's outer flag: 1 lets the pump's wait drop the lock */
    int      pump_after_svc; /* 1 = a service thread stopping under us finishes in the pump,
                                0 = the wait ends there with DART__WAIT_SVC_GONE */
} i_DartWait;

/* what ended the wait: the predicate, the deadline, or the service thread going away */
#define DART__WAIT_DONE     1
#define DART__WAIT_TIMEOUT  0
#define DART__WAIT_SVC_GONE (-1)

static int i_dart_node_wait_until(DartNode *n, const i_DartWait *w){
    for (;;){
        uint64_t deadline = 0, left, now = i_dart_plat_now_us();
        if (w->done(n, w->ctx, now, &deadline)) return DART__WAIT_DONE;
        if (now >= deadline) return DART__WAIT_TIMEOUT;
        if (w->periodic) w->periodic(n, w->ctx, now);
        left = deadline - now;
        if (w->cv_cap_us && left > w->cv_cap_us) left = w->cv_cap_us;
#ifdef DART_THREADS
        if (n->svc_running){
            n->cv_waiters++;
            i_dart_node_cv_wait(n, left);   /* mu drops: nothing cached survives */
            n->cv_waiters--;
            if (!n->svc_running && !w->pump_after_svc) return DART__WAIT_SVC_GONE;
            continue;                       /* stopped under us: the next pass pumps */
        }
#endif
        {   int ms = w->pump_ms;
            if (!ms){                       /* derive the tick from the time left */
                uint64_t left_ms = left / 1000u;
                ms = left < 1000u ? 1 : (left_ms > 0x7FFFFFFFu ? 0x7FFFFFFF : (int)left_ms);
            }
            i_dart_node_poll_locked(n, ms, w->pump_outer);
        }
    }
}

/* The kick a wait owes before it sleeps: only a service thread needs telling, a pump is
 * the poller and services the change on its own next tick. */
static void i_dart_node_wait_kick(DartNode *n, void *ctx, uint64_t now){
    (void)n; (void)ctx; (void)now;
#ifdef DART_THREADS
    if (n->svc_running) i_dart_node_kick(n);
#endif
}

/* the send path match wait, see spec/interest.md */

static int i_dart_node_settled(DartNode *n, uint64_t start, uint64_t now);   /* with settle */

/* Has the post open gather completed: the settle predicate anchored at open, latched
 * once true. */
static int i_dart_node_gather_done(DartNode *n, uint64_t now){
    if (n->gather_done) return 1;
    if (!i_dart_node_settled(n, n->open_us, now)) return 0;
    n->gather_done = 1;
    return 1;
}

/* Would a send now race a forming match: 1 while the gather is unsettled or verdicts are
 * in flight, else 0, memoized against the topology epoch. Lock held. */
static int i_dart_node_topic_unsettled(DartNode *n, DartTopic *h, uint64_t now){
    if (h->resolve_epoch == n->match_epoch) return 0;   /* converged at this topology */
    if (!i_dart_node_gather_done(n, now)) return 1;
    if (i_dart_node_core_topic_unresolved(n->core, h->index) > 0) return 1;
    h->resolve_epoch = n->match_epoch;
    return 0;
}

/* The match wait's predicate: done once a subscriber matched, or once matching converged
 * with none. The match count is re read from the transport every call. */
typedef struct {
    DartTopic *h;
    uint16_t   index;
    uint64_t   deadline;
    int        matched;   /* the count the wait ended on */
} i_DartMatchWait;

static int i_dart_node_match_wait_done(DartNode *n, void *ctx, uint64_t now, uint64_t *deadline){
    i_DartMatchWait *c = (i_DartMatchWait*)ctx;
    *deadline = c->deadline;
    c->matched = dart_transport_publisher_match_count(n->transport, c->index);
    if (c->matched) return 1;
    return !i_dart_node_topic_unsettled(n, c->h, now);
}

/* Bounded wait for a forming match before a zero subscriber send commits. Returns the
 * matched count. loud fires DART_E_UNMATCHED_SEND on timeout, a pattern write passes 0. */
static int i_dart_node_match_wait(DartNode *n, uint16_t topic_index, DartTopic *h, int loud){
    i_DartMatchWait c; i_DartWait w = { 0 };
    c.h = h; c.index = topic_index; c.matched = 0;
    c.deadline = i_dart_plat_now_us() + n->match_wait_us;
    w.done = i_dart_node_match_wait_done; w.ctx = &c;
    w.cv_cap_us = 50000u;   /* re check the clock: gather settling is partly time driven */
    w.pump_ms   = 1;        /* a nested tick: the lock stays held */
    if (i_dart_node_wait_until(n, &w) == DART__WAIT_TIMEOUT && loud){
        DartEvent e; memset(&e, 0, sizeof e);
        e.kind = DART_ERROR; e.error = DART_E_UNMATCHED_SEND;
        e.topic = topic_index; e.topic_name = i_dart_node_topic_name(n, topic_index);
        i_dart_node_emit(n, &e);
    }
    return c.matched;
}

#ifdef DART_THREADS
/* The threaded flow control wait's predicate: done when neither eviction looms. Unacked
 * history is bounded by qos.backpressure_wait_us, unsent history by DART_UNSENT_WAIT_US. */
typedef struct {
    uint16_t index;
    uint64_t rel_deadline;      /* 0 = no reliable bound */
    uint64_t unsent_deadline;
    uint64_t seq0;              /* the work_seq snapshot, the completed pass test */
    int      waited;            /* at least one sleep happened, gates the stats */
} i_DartSendWait;

static int i_dart_node_send_wait_done(DartNode *n, void *ctx, uint64_t now, uint64_t *deadline){
    i_DartSendWait *c = (i_DartSendWait*)ctx;
    int evict_unacked = c->rel_deadline && dart_transport_send_would_evict(n->transport, c->index);
    int evict_unsent  = dart_transport_send_would_evict_unsent(n->transport, c->index, NULL, NULL);
    (void)now;
    *deadline = evict_unacked ? (c->rel_deadline > c->unsent_deadline ? c->rel_deadline
                                                                     : c->unsent_deadline)
                              : c->unsent_deadline;
    if (!evict_unacked && !evict_unsent) return 1;
    if (!evict_unacked && n->work_seq != c->seq0){
        if (n->tx_hold_len) return 1;
        c->seq0 = n->work_seq;
    }
    return 0;
}

/* the sleep is what the backpressure stats measure, so arm them where sleeping is decided */
static void i_dart_node_send_wait_tick(DartNode *n, void *ctx, uint64_t now){
    ((i_DartSendWait*)ctx)->waited = 1;
    i_dart_node_wait_kick(n, ctx, now);
}
#endif /* DART_THREADS */

/* The unthreaded pump's predicate: done once the send no longer evicts unacked history.
 * It also samples the in pump probe on a timer, after a tick and before the re check. */
typedef struct {
    uint16_t index;
    uint64_t deadline;
    uint64_t t0, sample_last, interval;
    uint32_t polls, polls_idle;
    int      polled;
    DartRepairStats prev;
} i_DartPumpWait;

static int i_dart_node_pump_wait_done(DartNode *n, void *ctx, uint64_t now, uint64_t *deadline){
    i_DartPumpWait *c = (i_DartPumpWait*)ctx;
    *deadline = c->deadline;
    if (n->pump_probe && c->polled){
        c->polls++;
        if (dart_transport_repair_pending(n->transport, c->index) == 0) c->polls_idle++;
        if (now - c->sample_last >= c->interval){
            DartRepairStats sample_now; DartPumpSample sample;
            dart_transport_repair_stats(n->transport, c->index, &sample_now);
            sample.topic           = c->index;
            sample.wait_elapsed_us = now - c->t0;
            sample.interval_us     = now - c->sample_last;
            sample.frags_resent    = sample_now.frags_resent - c->prev.frags_resent;
            sample.nacks_recv      = sample_now.nacks_recv  - c->prev.nacks_recv;
            sample.polls           = c->polls;
            sample.polls_idle      = c->polls_idle;
            n->pump_probe(n->pump_probe_user, &sample);
            c->prev = sample_now; c->sample_last = now; c->polls = 0; c->polls_idle = 0;
        }
    }
    return !dart_transport_send_would_evict(n->transport, c->index);
}

static void i_dart_node_pump_wait_tick(DartNode *n, void *ctx, uint64_t now){
    (void)n; (void)now;
    ((i_DartPumpWait*)ctx)->polled = 1;   /* a tick runs next: sample on the call after it */
}

/* Publishes on a topic index: the match wait, the flow control wait, then SHM or UDP.
 * may_wait = 0 is a reentrant send from a callback: it never blocks or runs the loop. */
static int i_dart_node_do_send_ex(DartNode *n, uint16_t topic_index, DartBytes hdr, DartBytes data,
                                  uint64_t capture_us, int directed, uint32_t to_peer,
                                  int may_wait){
    /* one O(1) count gates the per send fast paths: an unsubscribed topic skips them all */
    int matched = dart_transport_publisher_match_count(n->transport, topic_index);
    const DartQos *q = dart_transport_topic_qos(n->transport, topic_index);
    /* the source stamp is ordinary payload, so every size rule here counts it. qos is
       immutable, so this stays valid across the waits that may re fetch q */
    size_t ts_bytes = (q && q->no_timestamp) ? 0u : (size_t)DART_TIMESTAMP_BYTES;
    size_t cap_bytes = (ts_bytes && capture_us) ? (size_t)DART_CAPTURE_BYTES : 0u;
    size_t len = ts_bytes + cap_bytes + hdr.len + data.len;
    int guarded = 0;   /* the unsent eviction check runs after the wait */

    /* a send to zero subscribers while a match forms waits for it to converge. A topic
       that retains history is exempt, since its history replays. See spec/interest.md */
    if (!matched && topic_index < n->max_topics
        && !(q && q->reliability == DART_RELIABLE && q->catch_up > 0)){
        DartTopic *h = n->handles[topic_index];
        if (h && dart_role_pubs(h->role)
              && i_dart_node_topic_unsettled(n, h, i_dart_plat_now_us())){
            if (may_wait && n->match_wait_us){
                matched = i_dart_node_match_wait(n, topic_index, h, 1);
                q = dart_transport_topic_qos(n->transport, topic_index);   /* the arena may move */
            } else {
                DartEvent e; memset(&e, 0, sizeof e);
                e.kind = DART_ERROR; e.error = DART_E_UNMATCHED_SEND;
                e.topic = topic_index; e.topic_name = i_dart_node_topic_name(n, topic_index);
                i_dart_node_emit(n, &e);
            }
        }
    }

#ifdef DART_THREADS
    if (matched && n->svc_running){
        guarded = 1;
        /* threaded: wait on the condvar for the poller's progress. A service thread that
           stops under us ends the wait, there is no poller left */
        if (may_wait){
            i_DartSendWait c; i_DartWait w = { 0 };
            uint64_t t0 = i_dart_plat_now_us();
            c.index = topic_index;
            c.rel_deadline = (q && q->backpressure_wait_us) ? t0 + q->backpressure_wait_us : 0;
            c.unsent_deadline = t0 + DART_UNSENT_WAIT_US;
            c.seq0 = n->work_seq;
            c.waited = 0;
            w.done = i_dart_node_send_wait_done; w.periodic = i_dart_node_send_wait_tick;
            w.ctx = &c;
            w.pump_ms = 1;   /* a pump is unreachable here, bounded regardless */
            i_dart_node_wait_until(n, &w);   /* every outcome proceeds: KEEP_LAST applies */
            if (c.waited){
                n->backpressure_total_us += i_dart_plat_now_us() - t0;
                n->backpressure_wait_count++;
            }
            /* the poller may have relocated the arena while we slept */
            q = dart_transport_topic_qos(n->transport, topic_index);
            matched = dart_transport_publisher_match_count(n->transport, topic_index);
        }
    } else
#endif
    /* no service thread: the bounded backpressure pump runs the loop until a slow reader
       acks or the wait elapses, then sends anyway */
    if (matched && may_wait && q && q->backpressure_wait_us && dart_transport_send_would_evict(n->transport, topic_index)){
        i_DartPumpWait c = { 0 }; i_DartWait w = { 0 };
        c.index = topic_index;
        c.t0 = i_dart_plat_now_us();
        c.deadline = c.t0 + q->backpressure_wait_us;
        c.sample_last = c.t0;
        c.interval = n->pump_probe_interval_us ? n->pump_probe_interval_us : 200000u;
        if (n->pump_probe) dart_transport_repair_stats(n->transport, topic_index, &c.prev);
        w.done = i_dart_node_pump_wait_done; w.periodic = i_dart_node_pump_wait_tick;
        w.ctx = &c;
        w.pump_ms = 1;   /* a nested tick: the lock stays held */
        guarded = 1;
        i_dart_node_wait_until(n, &w);
        n->backpressure_total_us += i_dart_plat_now_us() - c.t0;
        n->backpressure_wait_count++;
        /* a mid pump grow relocates the arena: re derive the cached pointers */
        q = dart_transport_topic_qos(n->transport, topic_index);
        matched = dart_transport_publisher_match_count(n->transport, topic_index);
    }

    /* a send that evicts never sent history is surfaced. The state is read before the
       commit, but the event fires only if the send commits: a rejected send overwrote nothing */
    {
        uint64_t evict_base = 0; uint32_t evict_count = 0;
        int will_evict = guarded &&
            dart_transport_send_would_evict_unsent(n->transport, topic_index, &evict_base, &evict_count);
        int r;
#ifdef DART_SHM
        /* only a message that would fragment gains from SHM, below the fragment size
           inline UDP is strictly cheaper. See spec/transport.md */
        if (!directed && n->shm_capable && len > dart_transport_frag(n->transport)
            && topic_index < n->shm_n_topics && matched
            && dart_transport_publisher_shm_eligible(n->transport, topic_index)){
            uint16_t keep_last = (q && q->keep_last) ? q->keep_last : 1u;
            /* a hint pins the topic to one class, else each message uses its own size
               class's segment. Per topic either way */
            uint32_t hint = q ? (q->shm_max_bytes ? q->shm_max_bytes : q->max_message_bytes) : 0u;
            uint32_t k = i_dart_shm_class_for(hint ? hint : (uint32_t)len);
            /* fits its class: publish via SHM. The chunk index is the history slot this
               send occupies, so chunk i binds slot i */
            if (k < DART_SHM_N_CLASSES && (uint32_t)len <= i_dart_shm_class_bytes(k)){
                i_DartShmPool *pool = i_dart_node_shm_topic_pool(n, topic_index, k, keep_last);
                uint16_t slot = dart_transport_topic_hist_head(n->transport, topic_index);
                void *chunk_ptr = pool ? i_dart_shm_chunk(pool, slot, NULL) : NULL;
                if (chunk_ptr){
                    i_DartShmDesc d; uint8_t desc[DART_SHM_DESC_WIRE];
                    /* gather the whole wire sample into the chunk: stamp, header, payload,
                       exactly as the inline commit writes it */
                    if (ts_bytes){
                        uint64_t w = i_dart_plat_wall_us() & DART_STAMP_MASK;
                        if (cap_bytes) w |= DART_STAMP_CAPTURE;
                        i_dart_le_w64((uint8_t*)chunk_ptr, w);
                        if (cap_bytes) i_dart_le_w64((uint8_t*)chunk_ptr + ts_bytes, capture_us);
                    }
                    if (hdr.len) memcpy((uint8_t*)chunk_ptr + ts_bytes + cap_bytes, hdr.data, hdr.len);
                    memcpy((uint8_t*)chunk_ptr + ts_bytes + cap_bytes + hdr.len, data.data, data.len);
                    i_dart_shm_stamp(pool, slot, (uint32_t)len, &d);
                    i_dart_shm_desc_encode(&d, desc);
                    if (dart_transport_send_shm(n->transport, topic_index, dart_bytes(chunk_ptr, len), desc, i_dart_plat_now_us())==0){
                        n->shm_tx++;
                        r = DART_OK;
                        goto committed;
                    }
                }
            }
        }
#endif
        r = directed ? dart_transport_send_to(n->transport, topic_index, to_peer, hdr, data,
                                              capture_us, i_dart_plat_now_us())
                     : dart_transport_send_hdr(n->transport, topic_index, hdr, data,
                                               capture_us, i_dart_plat_now_us());
#ifdef DART_SHM
committed:
#endif
        if (r == DART_OK && topic_index < i_dart_node_topic_hi(n) && n->handles[topic_index]){
            n->handles[topic_index]->tx_msgs++;                      /* dart_topic_counts */
            n->handles[topic_index]->tx_bytes += len;
        }
        if (r == DART_OK && will_evict){
            DartEvent e; memset(&e, 0, sizeof e);
            n->evicted_unsent++;
            e.kind = DART_ERROR; e.error = DART_E_EVICTED_UNSENT;
            e.topic = topic_index; e.topic_name = i_dart_node_topic_name(n, topic_index);
            e.lost_first = evict_base; e.lost_count = evict_count;
            i_dart_node_emit(n, &e);
        }
        return r;
    }
}

/* the plain broadcast send, the hot dart_topic_send path */
static int i_dart_node_do_send(DartNode *n, uint16_t topic_index, DartBytes data,
                               uint64_t capture_us, int may_wait){
    DartBytes nohdr; nohdr.data=NULL; nohdr.len=0;
    return i_dart_node_do_send_ex(n, topic_index, nohdr, data, capture_us, 0, 0, may_wait);
}

int dart_topic_send(DartTopic *topic, DartBytes data, const DartSendOpts *opts){
    int acquired, r;
    if (!topic) return DART_ERR_NO_TOPIC;
    acquired = i_dart_node_lock(topic->n);
    r = i_dart_node_do_send(topic->n, topic->index, data,
                            opts ? opts->capture_us : 0u, acquired);
    i_dart_node_kick_tx(topic->n);           /* flush the commit now, not at the next tick */
    i_dart_node_unlock(topic->n, acquired);
    return r;
}

int i_dart_topic_send_hdr(DartTopic *topic, DartBytes hdr, DartBytes data){
    int acquired, r;
    if (!topic) return DART_ERR_NO_TOPIC;
    acquired = i_dart_node_lock(topic->n);
    r = i_dart_node_do_send_ex(topic->n, topic->index, hdr, data, 0, 0, 0, acquired);
    i_dart_node_kick_tx(topic->n);
    i_dart_node_unlock(topic->n, acquired);
    return r;
}

uint64_t i_dart_topic_seqno(DartTopic *topic){
    return topic ? dart_transport_topic_seqno(topic->n->transport, topic->index) : 0;
}

void i_dart_node_flush_tx(DartNode *n){
    uint8_t buf[DART_DGRAM_MAX]; uint32_t to; size_t out_len;
    int acquired;
    if (!n || n->fd == DART_SOCK_BAD) return;
    acquired = i_dart_node_lock(n);
    if (n->tx_hold_len && i_dart_node_tx(n, n->tx_hold_peer, n->tx_hold, n->tx_hold_len))
        n->tx_hold_len = 0;
    if (!n->tx_hold_len)
        while (dart_transport_poll_send(n->transport, &to, buf, sizeof buf, &out_len,
                                        i_dart_plat_now_us()))
            if (!i_dart_node_tx(n, to, buf, out_len)) break;   /* TX full: best effort */
    i_dart_node_unlock(n, acquired);
}

void i_dart_topic_clear_sys(DartTopic *topic){
    if (!topic) return;
    topic->sys_on_message = NULL;
    topic->sys_msg_user = NULL;
}

int i_dart_topic_match_wait(DartTopic *topic){
    DartNode *n; int acquired, matched;
    if (!topic) return 0;
    n = topic->n;
    acquired = i_dart_node_lock(n);
    matched = dart_transport_publisher_match_count(n->transport, topic->index);
    /* wait only when it can help and can run: a match still forming, the knob on, a
       publishing role, and not from a callback. Converged matching returns at once */
    if (acquired && !matched && n->match_wait_us
        && dart_role_pubs(topic->role)
        && i_dart_node_topic_unsettled(n, topic, i_dart_plat_now_us()))
        matched = i_dart_node_match_wait(n, topic->index, topic, 0);
    i_dart_node_unlock(n, acquired);
    return matched;
}

int i_dart_topic_send_to(DartTopic *topic, uint32_t to_peer, DartBytes hdr, DartBytes data){
    int acquired, r;
    if (!topic) return DART_ERR_NO_TOPIC;
    acquired = i_dart_node_lock(topic->n);
    r = i_dart_node_do_send_ex(topic->n, topic->index, hdr, data, 0, 1, to_peer, acquired);
    i_dart_node_kick_tx(topic->n);
    i_dart_node_unlock(topic->n, acquired);
    return r;
}

/* The patterns layer's seams. Not lock guarded, the layer calls them under the node lock. */
void  *i_dart_node_sys_alloc(DartNode *n, void *ptr, size_t size){ return i_dart_node_alloc(n, ptr, size); }
void **i_dart_node_sys_slot (DartNode *n){ return &n->patterns; }
uint64_t i_dart_node_now_us (DartNode *n){ (void)n; return i_dart_plat_now_us(); }
uint64_t i_dart_node_wall_us(DartNode *n){ (void)n; return i_dart_plat_wall_us(); }
/* The node lock for a pattern call, reentrancy aware like the internal entry lock. No kick
 * on unlock: every mutating path kicks at its own layer, and read only ops must not wake. */
int  i_dart_node_sys_lock  (DartNode *n){ return i_dart_node_lock(n); }
void i_dart_node_sys_unlock(DartNode *n, int acquired){ i_dart_node_unlock(n, acquired); }
int  i_dart_node_sys_poll  (DartNode *n, int timeout_ms){ return dart_node_poll(n, timeout_ms); }

/* A topic scoped DART_ERROR from the patterns layer, through the node's one event path. */
void i_dart_node_sys_error(DartNode *n, DartErrorKind error, DartTopic *topic, uint32_t peer){
    DartEvent e;
    if (!n) return;
    memset(&e, 0, sizeof e);
    e.kind = DART_ERROR; e.error = error; e.peer = peer;
    if (topic){ e.topic = topic->index; e.topic_name = i_dart_node_topic_name(n, topic->index); }
    i_dart_node_emit(n, &e);
}

/* Matched subscribers excluding dormant peers, since dart_topic_match_count keeps counting
 * a dropped but resumable peer. */
int i_dart_topic_live_match_count(DartTopic *topic){
    int acquired, r;
    if (!topic) return 0;
    acquired = i_dart_node_lock(topic->n);
    r = dart_transport_publisher_live_matches(topic->n->transport, topic->index);
    i_dart_node_unlock(topic->n, acquired);
    return r;
}
int i_dart_topic_peer_matched(DartTopic *topic, uint32_t peer){
    int acquired, r;
    if (!topic) return 0;
    acquired = i_dart_node_lock(topic->n);
    r = dart_transport_publisher_peer_matched(topic->n->transport, topic->index, peer);
    i_dart_node_unlock(topic->n, acquired);
    return r;
}

/* Matched publishers feeding this topic's subscription side. */
int i_dart_topic_source_match_count(DartTopic *topic){
    int acquired, r;
    if (!topic) return 0;
    acquired = i_dart_node_lock(topic->n);
    r = dart_transport_subscriber_match_count(topic->n->transport, topic->index);
    i_dart_node_unlock(topic->n, acquired);
    return r;
}

/* The oldest live matched subscriber's peer id, 0 = none. */
uint32_t i_dart_topic_oldest_match(DartTopic *topic){
    uint32_t r; int acquired;
    if (!topic) return 0;
    acquired = i_dart_node_lock(topic->n);
    r = dart_transport_publisher_oldest_match(topic->n->transport, topic->index);
    i_dart_node_unlock(topic->n, acquired);
    return r;
}

const uint8_t *i_dart_node_uuid(DartNode *n){
    return n ? dart_discovery_uuid(dart_discovery_state(n->discovery)) : NULL;
}

/* Reflection getters for the patterns layer's entity enumeration. */
uint8_t    i_dart_topic_kind (const DartTopic *topic){ return topic ? topic->kind : 0; }
uint8_t    i_dart_topic_role (const DartTopic *topic){ return topic ? topic->role : (uint8_t)DART_INACTIVE; }
uint8_t    i_dart_topic_reliability(const DartTopic *topic){ return topic ? (uint8_t)topic->qos.reliability : 0; }
const uint8_t *i_dart_node_peer_uuid(DartNode *n, uint32_t peer){
    const DartDiscoveryState *st; uint16_t q, np; int acquired; const uint8_t *out = NULL;
    DartDiscoveryPeer v;   /* the uuid is a view into discovery state, the copy only carries it */
    if (!n) return NULL;
    acquired = i_dart_node_lock(n);
    st = dart_discovery_state(n->discovery);
    np = dart_discovery_max_peers(st);
    for (q = 0; q < np; q++)
        if (dart_discovery_peer_at(st, q, &v) && v.id == peer){ out = v.uuid; break; }
    i_dart_node_unlock(n, acquired);
    return out;
}
DartString i_dart_topic_name (const DartTopic *topic){
    return topic ? dart_string(topic->name, topic->name_len) : dart_string(NULL, 0);
}
uint16_t   i_dart_node_topic_count(DartNode *n){ return n ? i_dart_node_topic_hi(n) : 0; }

void i_dart_node_set_sys_hooks(DartNode *n, i_DartSysEventFn on_event, i_DartSysTickFn tick,
                               i_DartSysCloseFn on_close, void *user){
    int acquired;
    if (!n) return;
    acquired = i_dart_node_lock(n);
    n->sys_on_event = on_event; n->sys_tick = tick; n->sys_on_close = on_close; n->sys_user = user;
    n->sys_tick_next = 0;
    i_dart_node_kick(n);   /* re evaluate the wait cap with the new tick */
    i_dart_node_unlock(n, acquired);
}

int dart_topic_set_role(DartTopic *topic, DartRole role){
    int r, acquired;
    if (!topic) return -1;
    acquired = i_dart_node_lock(topic->n);
    if (!acquired){
        /* from a callback: the replay would rematch the reader proxy mid delivery, refuse loudly */
        return DART_ERR_STATE;
    }
    /* a log builtin is queued before its subscribe side goes live, so the catch up replay
       can never race the first take into the inline path. See spec/node.md */
    if (dart_role_subs((uint8_t)role) && !topic->q
        && i_dart_node_is_log_topic(topic->n, topic->index))
        (void)i_dart_node_queue_ensure(topic->n, topic, NULL);
    r = dart_transport_set_role(topic->n->transport, topic->index, (uint8_t)role);
    if (r == 0){
        topic->role = (uint8_t)role;
        i_dart_node_readvertise(topic->n);   /* a role flip may raise fresh candidates */
    }
    i_dart_node_unlock(topic->n, acquired);
    return r;
}

int dart_topic_retire(DartTopic *topic){
    DartNode *n; int acquired; uint16_t idx;
    if (!topic) return DART_ERR_NO_TOPIC;
    n = topic->n;
    acquired = i_dart_node_lock(n);
    if (!acquired) return DART_ERR_STATE;   /* from a callback: lanes are live mid delivery */
    if (topic->sys_on_message                       /* a pattern channel: retire its handle */
        || i_dart_node_is_log_topic(n, topic->index)
        || (n->n_builtin && topic->index >= n->builtin_lo
            && topic->index < (uint16_t)(n->builtin_lo + n->n_builtin))
        || (topic->q && topic->q->busy)){           /* mid dispatch on another thread */
        i_dart_node_unlock(n, acquired);
        return DART_ERR_STATE;
    }
    idx = topic->index;
    if (dart_transport_topic_retire(n->transport, idx) != 0){
        i_dart_node_unlock(n, acquired);
        return DART_ERR_STATE;
    }
    /* the slot keeps its schema hash as the reuse fingerprint, the parsed copy goes */
    i_dart_node_core_retire_topic_schema(n->core, idx);
    if (topic->q){
        if (topic->q->buf) i_dart_node_alloc(n, topic->q->buf, 0);
        i_dart_node_alloc(n, topic->q, 0);
        topic->q = NULL;
    }
    if (topic->schema) dart_schema_free(topic->schema, i_dart_node_alloc, n);
    n->handles[idx] = NULL;
    i_dart_node_alloc(n, topic, 0);         /* the handle is invalid from here */
    /* the slot rides the announce as a hole from the next blob on */
    i_dart_node_readvertise(n);
    i_dart_node_unlock(n, acquired);
    return DART_OK;
}

uint16_t dart_topic_index(const DartTopic *topic){ return topic ? topic->index : 0; }

const DartSchema *dart_topic_schema(const DartTopic *topic){ return topic ? topic->schema : NULL; }

DartTopic *dart_node_topic(DartNode *n, uint16_t index){
    DartTopic *h; int acquired;
    if (!n) return NULL;
    acquired = i_dart_node_lock(n);
    h = (index < i_dart_node_topic_hi(n)) ? n->handles[index] : NULL;   /* holes yield NULL */
    i_dart_node_unlock(n, acquired);
    return h;
}

/* reflection: the walks read the core's tables under the node lock */

int dart_node_peers_next(DartNode *n, DartIter *it, DartPeerInfo *out){
    int r, acquired;
    if (!n) return 0;
    acquired = i_dart_node_lock(n);
    r = i_dart_node_core_peers_next(n->core, it, out);
    if (r){   /* the transport's path measurement, folded in since the core is sans transport */
        DartPeerRtt e;
        if (dart_transport_peer_rtt(n->transport, out->id, &e)){
            out->rtt_us = e.rtt_us; out->rtt_jitter_us = e.rtt_jitter_us;
            out->rtt_min_us = e.rtt_min_us; out->rtt_samples = e.samples;
        }
    }
    i_dart_node_unlock(n, acquired);
    return r;
}

int dart_node_entities_next(DartNode *n, uint32_t peer, DartIter *it, DartEntityInfo *out){
    int r, acquired;
    if (!n) return 0;
    acquired = i_dart_node_lock(n);
    r = i_dart_node_core_entities_next(n->core, peer, it, out);
    i_dart_node_unlock(n, acquired);
    return r;
}

int dart_node_mesh_next(DartNode *n, DartIter *it, DartEntityInfo *out){
    int r, acquired;
    if (!n) return 0;
    acquired = i_dart_node_lock(n);
    r = i_dart_node_core_mesh_next(n->core, it, out);
    i_dart_node_unlock(n, acquired);
    return r;
}

int dart_node_mesh_find(DartNode *n, DartEntityKind kind, const char *name, DartEntityInfo *out){
    int r, acquired;
    if (!n) return 0;
    acquired = i_dart_node_lock(n);
    r = i_dart_node_core_mesh_find(n->core, kind, name, out);
    i_dart_node_unlock(n, acquired);
    return r;
}

uint32_t dart_node_mesh_epoch(DartNode *n){
    uint32_t e; int acquired;
    if (!n) return 0;
    acquired = i_dart_node_lock(n);
    e = i_dart_node_core_mesh_epoch(n->core);
    i_dart_node_unlock(n, acquired);
    return e;
}

/* Re types a live topic in place: the slot is retired and reused at the same index under
 * a bumped generation, the schema copy replaced, the handle and queue kept. Lock held. */
int i_dart_topic_retype(DartTopic *topic, const DartSchema *schema, uint8_t reliability){
    DartNode *n = topic->n; DartTopicDef def; DartSchema *copy = NULL; uint16_t idx = topic->index;
    uint32_t rb; int rc;
    if (schema){
        DartBytes w = dart_schema_wire(schema);
        copy = dart_schema_parse(w.data, w.len, i_dart_node_alloc, n);
        if (!copy) return DART_ERR_OOM;
    }
    if (dart_transport_topic_retire(n->transport, idx) != 0){
        if (copy) dart_schema_free(copy, i_dart_node_alloc, n);
        return DART_ERR_STATE;
    }
    i_dart_node_core_retire_topic_schema(n->core, idx);
    if (topic->schema) dart_schema_free(topic->schema, i_dart_node_alloc, n);
    topic->schema = copy;
    topic->qos.reliability = (DartReliability)reliability;
    memset(&def, 0, sizeof def);
    def.name = topic->name; def.role = topic->role; def.kind = topic->kind;
    def.prefix_bytes = topic->prefix_bytes; def.directed = topic->directed; def.attrs = topic->attrs;
    def.qos = topic->qos;
    rb = dart_discovery_meta_version(dart_discovery_state(n->discovery)) + 1u;
    rc = dart_transport_topic_reuse(n->transport, idx, &def, 1, rb);
    if (rc != 0) return rc == -4 ? DART_ERR_OOM : DART_ERR_STATE;
    i_dart_node_core_topic_rebound(n->core, idx);
    i_dart_node_core_set_topic_schema(n->core, idx, topic->schema);
    i_dart_node_readvertise(n);
    return DART_OK;
}

int dart_topic_refresh(DartTopic *topic){
    DartNode *n; int acquired, r = 0;
    const DartSchema *ms = NULL; uint8_t rel = 0; uint64_t gen = 0;
    if (!topic) return DART_ERR_NO_TOPIC;
    if (!topic->reflect) return DART_ERR_ROLE;
    n = topic->n;
    acquired = i_dart_node_lock(n);
    if (!acquired) return DART_ERR_STATE;   /* from a callback: lanes are live mid delivery */
    if (i_dart_node_core_reflect_pick(n->core, DART_ENTITY_TOPIC, topic->name, 0,
                                      dart_role_pubs(topic->role), &ms, &rel, &gen)
        && gen != topic->generation){
        r = i_dart_topic_retype(topic, ms, rel ? DART_RELIABLE : DART_BEST_EFFORT);
        if (r == 0){ topic->generation = gen; r = 1; }
    }
    i_dart_node_unlock(n, acquired);
    return r;
}

/* the patterns layer's reflect_from_mesh: the pick for one channel of an entity */
int i_dart_node_reflect_pick(DartNode *n, DartEntityKind kind, const char *name, int which, int writer,
                             const DartSchema **schema, uint8_t *reliable, uint64_t *generation){
    int r, acquired;
    if (!n) return 0;
    acquired = i_dart_node_lock(n);
    r = i_dart_node_core_reflect_pick(n->core, kind, name, which, writer, schema, reliable, generation);
    i_dart_node_unlock(n, acquired);
    return r;
}

void dart_node_backpressure_stats(DartNode *n, uint64_t *waited_us, uint32_t *waited_sends){
    int acquired = i_dart_node_lock(n);
    if (waited_us)    *waited_us    = n->backpressure_total_us;
    if (waited_sends) *waited_sends = n->backpressure_wait_count;
    i_dart_node_unlock(n, acquired);
}

/* Sends that evicted never sent history after the bounded wait, since open. */
uint32_t dart_node_evicted_unsent(DartNode *n){
    uint32_t v; int acquired;
    if (!n) return 0;
    acquired = i_dart_node_lock(n);
    v = n->evicted_unsent;
    i_dart_node_unlock(n, acquired);
    return v;
}

#ifndef DART_NO_STDTYPES
/* The standard types that need a platform. They sit here so serialize/ keeps needing
 * nothing but memory. */
DartTimestamp dart_timestamp_now(void){
    return (DartTimestamp)i_dart_plat_wall_us();
}

void dart_uuid_new(DartUuid *out){
    /* one generator: the CSPRNG path, else the host identity mix a node's own uuid uses */
    if (out) i_dart_discovery_auto_uuid(out->bytes);
}
#endif

void dart_node_mem_stats(DartNode *n, size_t *in_use, size_t *peak, uint64_t *alloc_calls){
    int acquired;
    if (!n) return;
    acquired = i_dart_node_lock(n);
    dart_allocator_stats(&n->pool, in_use, peak, alloc_calls);
    i_dart_node_unlock(n, acquired);
}

void dart_topic_repair_stats(DartTopic *topic, DartRepairStats *out){
    if (topic){
        int acquired = i_dart_node_lock(topic->n);
        dart_transport_repair_stats(topic->n->transport, topic->index, out);
        i_dart_node_unlock(topic->n, acquired);
    } else if (out) memset(out, 0, sizeof *out);
}

void dart_topic_counts(DartTopic *topic, uint64_t *tx_msgs, uint64_t *tx_bytes,
                       uint64_t *rx_msgs, uint64_t *rx_bytes){
    uint64_t tm = 0, tb = 0, rm = 0, rb = 0;
    if (topic){
        int acquired = i_dart_node_lock(topic->n);
        tm = topic->tx_msgs; tb = topic->tx_bytes; rm = topic->rx_msgs; rb = topic->rx_bytes;
        i_dart_node_unlock(topic->n, acquired);
    }
    if (tx_msgs)  *tx_msgs  = tm;
    if (tx_bytes) *tx_bytes = tb;
    if (rx_msgs)  *rx_msgs  = rm;
    if (rx_bytes) *rx_bytes = rb;
}

/* the @dart/meta snapshot builder, see spec/node.md */

/* one pass of the map body. A latched writer error makes finish return 0 and the caller
 * grows and rebuilds */
static void i_dart_node_snapshot_fill(DartNode *n, DartMapWriter *w, uint32_t sections){
    uint64_t now = i_dart_plat_now_us();
    if (sections & DART_META_NODE){
        size_t in_use = 0, peak = 0; uint64_t allocs = 0;
        dart_allocator_stats(&n->pool, &in_use, &peak, &allocs);
        dart_map_open_map(w, "node");
        dart_map_put_string(w, "name", dart_string(n->name, n->name_len));
        dart_map_put_uint(w, "uptime_us", now - n->open_us);
        dart_map_put_uint(w, "mono_us", now);
        dart_map_put_uint(w, "wall_us", i_dart_plat_wall_us());
        dart_map_put_uint(w, "mem_in_use", (uint64_t)in_use);
        dart_map_put_uint(w, "mem_peak", (uint64_t)peak);
        dart_map_put_uint(w, "alloc_calls", allocs);
        dart_map_put_uint(w, "evicted_unsent", n->evicted_unsent);
        dart_map_put_uint(w, "bp_waited_us", n->backpressure_total_us);
        dart_map_put_uint(w, "bp_waits", n->backpressure_wait_count);
        dart_map_put_uint(w, "peers", dart_discovery_peer_count(dart_discovery_state(n->discovery)));
        dart_map_put_uint(w, "max_peers", n->max_peers);
        /* app topics only: the @dart/ builtins are hidden, so neither the counts nor the
           topics array below surface them */
        dart_map_put_uint(w, "topics", (uint64_t)n->n_created);
        dart_map_put_uint(w, "max_topics", (uint64_t)(n->max_topics - n->n_builtin));
#ifdef DART_SHM
        dart_map_put_uint(w, "shm_tx", n->shm_tx);
        dart_map_put_uint(w, "shm_rx", n->shm_rx);
#endif
        dart_map_put_uint(w, "last_error", (uint64_t)n->last_error.error);
        if (n->last_error.error != DART_E_NONE){
            /* format from a sanitized copy: the stored event's name views can outlive what
               they pointed at, so the text carries the indices instead */
            DartEvent le = n->last_error; char txt[160];
            le.topic_name = NULL; le.peer_name = NULL; le.schema_detail = NULL;
            dart_event_str(&le, txt, sizeof txt);
            dart_map_put_string(w, "last_error_text", dart_cstr(txt));
        }
        dart_map_close(w);
    }
#ifdef DART_PROC_STATS
    if (sections & DART_META_PROC){
        uint64_t cpu = 0, rss = 0, peak_rss = 0; int have_cpu = 0;
        if (i_dart_plat_proc_stats(&cpu, &rss, &peak_rss, &have_cpu)){   /* absent if unsupported */
            dart_map_open_map(w, "proc");
            dart_map_put_uint(w, "pid", i_dart_plat_pid());
            if (have_cpu) dart_map_put_uint(w, "cpu_us", cpu);
            dart_map_put_uint(w, "rss", rss);
            dart_map_put_uint(w, "peak_rss", peak_rss);
            {   uint64_t heap_total, heap_free, heap_min_free, heap_largest_free_block;
                if (i_dart_plat_heap_stats(&heap_total, &heap_free, &heap_min_free,
                                           &heap_largest_free_block)){
                    dart_map_put_uint(w, "heap_total", heap_total);
                    dart_map_put_uint(w, "heap_free", heap_free);
                    dart_map_put_uint(w, "heap_min_free", heap_min_free);
                    dart_map_put_uint(w, "heap_largest_free_block", heap_largest_free_block);
                }
            }
            dart_map_close(w);
        }
    }
#endif
    if (sections & DART_META_TOPICS){
        uint16_t i, hi = i_dart_node_topic_hi(n);
        const uint16_t *pend = NULL;
        if (hi){
            /* every topic's unresolved count in one walk of each peer's interest, since the
               per topic query is quadratic for an observer. On OOM the per topic query stands in */
            if (n->snap_pend_cap < hi){
                uint16_t *nb = (uint16_t*)i_dart_node_alloc(n, n->snap_pend, (size_t)hi * sizeof *nb);
                if (nb){ n->snap_pend = nb; n->snap_pend_cap = hi; }
            }
            if (n->snap_pend_cap >= hi){
                i_dart_node_core_topics_unresolved(n->core, n->snap_pend, hi);
                pend = n->snap_pend;
            }
        }
        dart_map_open_array(w, "topics");
        for (i = 0; i < hi; i++){
            DartTopic *h = n->handles[i];
            const DartQos *q = dart_transport_topic_qos(n->transport, i);
            DartRepairStats rs;
            if (!h) continue;
            if (n->n_builtin && i >= n->builtin_lo
                             && i < (uint16_t)(n->builtin_lo + n->n_builtin)) continue;
            dart_transport_repair_stats(n->transport, i, &rs);
            dart_map_open_map(w, NULL);
            dart_map_put_uint(w, "index", i);
            dart_map_put_string(w, "name", dart_string(h->name, h->name_len));
            dart_map_put_uint(w, "kind", h->kind);
            dart_map_put_uint(w, "role", h->role);
            dart_map_put_bool(w, "reliable", q && q->reliability == DART_RELIABLE);
            dart_map_put_uint(w, "keep_last", q ? q->keep_last : 0);
            dart_map_put_uint(w, "catch_up", q ? q->catch_up : 0);
            dart_map_put_uint(w, "subs", (uint64_t)dart_transport_publisher_match_count(n->transport, i));
            dart_map_put_uint(w, "pubs", (uint64_t)dart_transport_subscriber_match_count(n->transport, i));
            dart_map_put_uint(w, "pending", pend ? (uint64_t)pend[i]
                                                 : (uint64_t)i_dart_node_core_topic_unresolved(n->core, i));
            dart_map_put_uint(w, "tx_msgs", h->tx_msgs);
            dart_map_put_uint(w, "tx_bytes", h->tx_bytes);
            dart_map_put_uint(w, "rx_msgs", h->rx_msgs);
            dart_map_put_uint(w, "rx_bytes", h->rx_bytes);
            dart_map_put_uint(w, "nacks_recv", rs.nacks_recv);
            dart_map_put_uint(w, "frags_resent", rs.frags_resent);
            dart_map_put_uint(w, "frags_sent", rs.frags_sent);
            dart_map_put_uint(w, "nacks_sent", rs.nacks_sent);
            dart_map_put_uint(w, "frags_recv", rs.frags_recv);
            dart_map_put_uint(w, "frags_dup", rs.frags_dup);
            dart_map_put_uint(w, "msgs_skipped", rs.msgs_skipped);
            if (h->q){
                dart_map_put_uint(w, "q_msgs", h->q->count);
                dart_map_put_uint(w, "q_bytes", h->q->bytes);
                dart_map_put_uint(w, "q_cap", h->q->cap);
                dart_map_put_uint(w, "q_dropped", h->q->dropped);
            }
            dart_map_close(w);
        }
        dart_map_close(w);
    }
    if (sections & DART_META_PEERS){
        uint16_t cnt = 0, i;
        const DartDiscoveryPeer *ps = dart_discovery_peers(n->discovery, &cnt);
        dart_map_open_array(w, "peers");
        for (i = 0; i < cnt; i++){
            uint16_t pub_to = 0, recv_from = 0;
            char ip[16]; int ln = 0;
            dart_transport_peer_match_counts(n->transport, ps[i].id, &pub_to, &recv_from);
            dart_map_open_map(w, NULL);
            dart_map_put_uint(w, "id", ps[i].id);
            dart_map_put_string(w, "name", ps[i].name);
            dart_map_put_bool(w, "active", ps[i].liveness == DART_PEER_ACTIVE);
            if (ps[i].addr.ip_len == 4)
                ln = snprintf(ip, sizeof ip, "%u.%u.%u.%u", ps[i].addr.ip[0], ps[i].addr.ip[1],
                              ps[i].addr.ip[2], ps[i].addr.ip[3]);
            dart_map_put_string(w, "ip", dart_string(ip, ln > 0 ? (size_t)ln : 0));
            dart_map_put_uint(w, "port", ps[i].addr.port);
            dart_map_put_uint(w, "age_us", now > ps[i].last_heard_us ? now - ps[i].last_heard_us : 0);
            dart_map_put_uint(w, "publish_to", pub_to);
            dart_map_put_uint(w, "receive_from", recv_from);
            {   DartPeerRtt e;   /* the measured round trip, absent until the first sample */
                if (dart_transport_peer_rtt(n->transport, ps[i].id, &e) && e.samples){
                    dart_map_put_uint(w, "rtt_us", e.rtt_us);
                    dart_map_put_uint(w, "rtt_jitter_us", e.rtt_jitter_us);
                    dart_map_put_uint(w, "rtt_min_us", e.rtt_min_us);
                    dart_map_put_uint(w, "rtt_samples", e.samples);
                } }
            dart_map_close(w);
        }
        dart_map_close(w);
    }
}

DartBytes i_dart_node_snapshot(DartNode *n, uint32_t sections){
    uint32_t body;
    if (!n) return dart_bytes(NULL, 0);
    if (!sections) sections = 0xFFFFFFFFu;
    for (;;){
        DartMapWriter w;
        if (!n->snap_buf){
            /* a small node's snapshot is 1 to 2 KB: start there, the doubling finds the size */
            n->snap_buf = (uint8_t*)i_dart_node_alloc(n, NULL, 1024u);
            if (!n->snap_buf) return dart_bytes(NULL, 0);
            n->snap_cap = 1024u;
        }
        w = dart_map_begin(n->snap_buf, n->snap_cap);
        i_dart_node_snapshot_fill(n, &w, sections);
        body = dart_map_finish(&w);
        if (body) break;
        {   /* did not fit (the writer latches on any failure): double and rebuild */
            uint32_t ncap = n->snap_cap * 2u; uint8_t *nb;
            if (ncap > (1u << 22)) return dart_bytes(NULL, 0);   /* 4 MB: not a size problem */
            nb = (uint8_t*)i_dart_node_alloc(n, n->snap_buf, ncap);
            if (!nb) return dart_bytes(NULL, 0);
            n->snap_buf = nb; n->snap_cap = ncap;
        }
    }
    return dart_bytes(n->snap_buf, body);
}

void dart_node_set_pump_probe(DartNode *n, DartPumpProbeFn fn, uint64_t interval_us, void *user){
    int acquired = i_dart_node_lock(n);
    n->pump_probe = fn; n->pump_probe_interval_us = interval_us; n->pump_probe_user = user;
    i_dart_node_unlock(n, acquired);
}

int dart_topic_subscriber_progress(DartTopic *topic, uint32_t peer,
                                 uint64_t *base_seqno, uint32_t *have, uint32_t *total){
    int r, acquired;
    if (!topic) return 0;
    acquired = i_dart_node_lock(topic->n);
    r = dart_transport_subscriber_progress(topic->n->transport, topic->index, peer, base_seqno, have, total);
    i_dart_node_unlock(topic->n, acquired);
    return r;
}

#ifdef DART_SHM
void dart_node_shm_stats(DartNode *n, uint32_t *sent, uint32_t *recv){
    int acquired = i_dart_node_lock(n);
    if (sent) *sent = n->shm_tx;
    if (recv) *recv = n->shm_rx;
    i_dart_node_unlock(n, acquired);
}
#endif

/* drain's predicate: the topic's send queue is empty at the transport */
typedef struct { uint16_t index; uint64_t deadline; } i_DartDrainWait;

static int i_dart_node_drain_wait_done(DartNode *n, void *ctx, uint64_t now, uint64_t *deadline){
    i_DartDrainWait *c = (i_DartDrainWait*)ctx;
    (void)now;
    *deadline = c->deadline;
    return dart_transport_send_drained(n->transport, c->index) != 0;
}

int dart_topic_drain(DartTopic *topic, int timeout_ms){
    DartNode *n; i_DartDrainWait c; i_DartWait w = { 0 }; int acquired, drained;
    if (!topic) return 0;
    n = topic->n;
    acquired = i_dart_node_lock(n);
    if (!acquired) return 0;   /* from a callback: can neither pump nor wait */
    c.index = topic->index;
    c.deadline = i_dart_plat_now_us() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000u;
    w.done = i_dart_node_drain_wait_done; w.periodic = i_dart_node_wait_kick; w.ctx = &c;
    w.pump_ms = 1;          /* a nested tick: the lock stays held */
    w.pump_after_svc = 1;   /* a service thread stopping under us finishes with the pump */
    drained = i_dart_node_wait_until(n, &w) == DART__WAIT_DONE;
    i_dart_node_unlock(n, acquired);
    return drained;
}

int dart_topic_match_count(DartTopic *topic){
    int r, acquired;
    if (!topic) return 0;
    acquired = i_dart_node_lock(topic->n);
    r = dart_transport_publisher_match_count(topic->n->transport, topic->index);
    i_dart_node_unlock(topic->n, acquired);
    return r;
}

int dart_topic_pending_count(DartTopic *topic){
    int r, acquired;
    if (!topic) return 0;
    acquired = i_dart_node_lock(topic->n);
    r = i_dart_node_core_topic_unresolved(topic->n->core, topic->index);
    i_dart_node_unlock(topic->n, acquired);
    return r;
}

/* 1 = a send now would not wait: matched, or converged with nobody to wait for. Shares the
 * send path's predicate, so a GUI polling this then sending sees what the send decides. */
int dart_topic_ready(DartTopic *topic){
    int r, acquired;
    if (!topic) return 0;
    acquired = i_dart_node_lock(topic->n);
    r = dart_transport_publisher_match_count(topic->n->transport, topic->index) > 0
     || !i_dart_node_topic_unsettled(topic->n, topic, i_dart_plat_now_us());
    i_dart_node_unlock(topic->n, acquired);
    return r;
}

/* Settled: the network answered and went quiet. Every active peer heard since the solicit,
 * the topology quiet for one window, one window passed overall. See spec/node.md. */
static int i_dart_node_settled(DartNode *n, uint64_t start, uint64_t now){
    uint64_t quiet = n->announce_us < 300000u ? n->announce_us : 300000u;
    uint16_t i, count = 0;
    const DartDiscoveryPeer *peers = dart_discovery_peers(n->discovery, &count);
    int any = 0;
    for (i = 0; i < count; i++){
        if (peers[i].liveness != DART_PEER_ACTIVE) continue;   /* dropped: not expected to answer */
        any = 1;
        if (peers[i].last_heard_us < start) return 0;          /* not answered the solicit yet */
    }
    if (!any) return now - start >= n->announce_us;            /* alone, as far as we can know */
    if (now - start < quiet) return 0;
    return n->settle_topology_us < start || now - n->settle_topology_us >= quiet;
}

typedef struct {
    uint64_t start;         /* the settled anchor and the deadline's base */
    uint64_t deadline;
    uint64_t last_solicit;  /* 0 = never: the first iteration solicits at once */
} i_DartSettleWait;

static int i_dart_node_settle_wait_done(DartNode *n, void *ctx, uint64_t now, uint64_t *deadline){
    i_DartSettleWait *c = (i_DartSettleWait*)ctx;
    *deadline = c->deadline;
    return i_dart_node_settled(n, c->start, now);
}

/* re solicit 4 times a second so a lost one retries. The kick makes a service thread send
 * it now, a pump sends it on the tick that follows. */
static void i_dart_node_settle_wait_solicit(DartNode *n, void *ctx, uint64_t now){
    i_DartSettleWait *c = (i_DartSettleWait*)ctx;
    if (now - c->last_solicit < 250000u) return;
    dart_discovery_solicit(dart_discovery_state(n->discovery));
    c->last_solicit = now;
    i_dart_node_wait_kick(n, ctx, now);
}

int dart_node_settle(DartNode *n, int timeout_ms){
    i_DartSettleWait c; i_DartWait w = { 0 }; int acquired, settled;
    if (!n) return 0;
    acquired = i_dart_node_lock(n);
    if (!acquired){ return 0; }   /* from a callback: can neither pump nor wait */
    c.start = i_dart_plat_now_us();
    c.last_solicit = 0;
    c.deadline = c.start + (timeout_ms >= 0 ? (uint64_t)timeout_ms * 1000u
                                            : (uint64_t)n->announce_us * 3u);
    w.done = i_dart_node_settle_wait_done; w.periodic = i_dart_node_settle_wait_solicit;
    w.ctx = &c;
    w.cv_cap_us = 50000u;   /* re check the clock: settling is partly time driven */
    w.pump_ms = 1;          /* a nested tick: sends the solicit, takes in the replies */
    w.pump_after_svc = 1;   /* a service thread stopping under us finishes with the pump */
    settled = i_dart_node_wait_until(n, &w) == DART__WAIT_DONE;
    i_dart_node_unlock(n, acquired);
    return settled;
}

/* the consumer queue API, see spec/node.md */

static int i_dart_node_any_queued(DartNode *n){
    uint16_t i, hi = i_dart_node_topic_hi(n);
    for (i = 0; i < hi; i++)
        if (n->handles[i] && n->handles[i]->q && n->handles[i]->q->count) return 1;
    return 0;
}

/* the queue wait's predicate: data on q when one was given, else on any queued topic. The
 * queue struct is a stable allocation, so the pointer survives the waits. */
typedef struct { i_DartMsgQueue *q; uint64_t deadline; } i_DartQueueWait;

static int i_dart_node_queue_wait_done(DartNode *n, void *ctx, uint64_t now, uint64_t *deadline){
    i_DartQueueWait *c = (i_DartQueueWait*)ctx;
    (void)now;
    *deadline = c->deadline;
    return c->q ? (c->q->count != 0) : i_dart_node_any_queued(n);
}

/* Waits until data is queued: on the service thread's progress when one runs, else by
 * driving the poll loop itself. Lock held on entry and exit, never from a callback. */
static void i_dart_node_queue_wait(DartNode *n, i_DartMsgQueue *q, int timeout_ms){
    i_DartQueueWait c; i_DartWait w = { 0 };
    c.q = q;
    c.deadline = timeout_ms < 0 ? (uint64_t)-1
                                : i_dart_plat_now_us() + (uint64_t)timeout_ms * 1000u;
    w.done = i_dart_node_queue_wait_done; w.ctx = &c;
    w.cv_cap_us = 3600000000u;   /* an unbounded wait still re checks hourly */
    w.pump_outer = 1;            /* outer: this wait may drop the lock */
    w.pump_after_svc = 1;        /* a service thread stopping under us pumps from then on */
    i_dart_node_wait_until(n, &w);
}

/* Dispatches up to max_msgs of the messages queued at entry, callbacks on the calling
 * thread and outside the lock when this thread owns it. A reentrant caller keeps the lock. */
static int i_dart_topic_dispatch_locked(DartNode *n, DartTopic *h, int max_msgs, int acquired){
    i_DartMsgQueue *q = h->q;
    int done = 0; uint32_t todo;
    if (!q || q->busy) return 0;
    i_dart_node_queue_release(n, h, q);
    todo = q->count;                    /* a snapshot: later arrivals wait for the next call */
    if (max_msgs > 0 && todo > (uint32_t)max_msgs) todo = (uint32_t)max_msgs;
    while (todo-- && q->count){
        const i_DartQRec *rec = i_dart_q_peek(q);
        DartMsg m;
        i_dart_node_queue_msg(n, h, rec, &m);
        q->viewing = 1;
        if (n->user_on_message){
            q->busy = 1;
#ifdef DART_THREADS
            if (acquired){
                i_dart_node_unlock_raw(n);
                n->user_on_message(&m);
                i_dart_node_lock_raw(n);
            } else
#endif
            n->user_on_message(&m);
            q->busy = 0;
        }
        i_dart_node_queue_release(n, h, q);
        done++;
    }
    (void)acquired;
    return done;
}

int dart_topic_take(DartTopic *topic, DartMsg *out, int timeout_ms){
    DartNode *n; i_DartMsgQueue *q; int acquired, got = 0;
    if (!topic || !out) return DART_ERR_NO_TOPIC;
    n = topic->n;
    acquired = i_dart_node_lock(n);
    q = i_dart_node_queue_ensure(n, topic, NULL);
    if (!q){ i_dart_node_unlock(n, acquired); return DART_ERR_OOM; }
    if (q->busy){ i_dart_node_unlock(n, acquired); return DART_ERR_STATE; }
    i_dart_node_queue_release(n, topic, q);      /* finish the previous view first */
    if (!q->count && timeout_ms != 0 && acquired)
        i_dart_node_queue_wait(n, q, timeout_ms);
    {   const i_DartQRec *rec = i_dart_q_peek(q);
        if (rec){
            i_dart_node_queue_msg(n, topic, rec, out);
            q->viewing = 1;                   /* the view lives until the next take or dispatch */
            got = 1;
        }
    }
    i_dart_node_unlock(n, acquired);
    return got;
}

int dart_topic_dispatch(DartTopic *topic, int max_msgs, int timeout_ms){
    DartNode *n; i_DartMsgQueue *q; int acquired, done;
    if (!topic) return DART_ERR_NO_TOPIC;
    n = topic->n;
    acquired = i_dart_node_lock(n);
    q = i_dart_node_queue_ensure(n, topic, NULL);
    if (!q){ i_dart_node_unlock(n, acquired); return DART_ERR_OOM; }
    if (q->busy){ i_dart_node_unlock(n, acquired); return DART_ERR_STATE; }
    i_dart_node_queue_release(n, topic, q);
    if (!q->count && timeout_ms != 0 && acquired)
        i_dart_node_queue_wait(n, q, timeout_ms);
    done = i_dart_topic_dispatch_locked(n, topic, max_msgs, acquired);
    i_dart_node_unlock(n, acquired);
    return done;
}

int dart_node_dispatch(DartNode *n, int max_msgs, int timeout_ms){
    int acquired, total = 0;
    uint16_t i;
    if (!n) return DART_ERR_STATE;
    acquired = i_dart_node_lock(n);
    if (!i_dart_node_any_queued(n) && timeout_ms != 0 && acquired)
        i_dart_node_queue_wait(n, NULL, timeout_ms);
    for (i = 0; i < i_dart_node_topic_hi(n); i++){
        DartTopic *h = n->handles[i];
        if (!h || !h->q || !h->q->count) continue;
        total += i_dart_topic_dispatch_locked(n, h, max_msgs > 0 ? max_msgs - total : 0, acquired);
        if (max_msgs > 0 && total >= max_msgs) break;
    }
    i_dart_node_unlock(n, acquired);
    return total;
}

void dart_topic_queue_stats(DartTopic *topic, uint32_t *msgs, uint32_t *bytes,
                              uint32_t *capacity, uint32_t *dropped){
    uint32_t m = 0, b = 0, c = 0, d = 0;
    if (topic && topic->q){
        int acquired = i_dart_node_lock(topic->n);
        m = topic->q->count; b = topic->q->bytes; c = topic->q->cap; d = topic->q->dropped;
        i_dart_node_unlock(topic->n, acquired);
    }
    if (msgs)     *msgs = m;
    if (bytes)    *bytes = b;
    if (capacity) *capacity = c;
    if (dropped)  *dropped = d;
}

#ifdef DART_THREADS
/* The service thread: the poll body in a loop, holding the lock for every work pass and
 * dropping it inside the wait. The 250 ms cap bounds a lost wakeup. See spec/node.md. */
static void i_dart_node_service(void *arg){
    DartNode *n = (DartNode *)arg;
    i_dart_node_lock_raw(n);
    while (!n->svc_stop)
        i_dart_node_poll_locked(n, 250, 1);
    i_dart_node_unlock_raw(n);
}
#endif

int dart_node_start(DartNode *n){
#ifndef DART_THREADS
    (void)n;
    return DART_ERR_NOSYS;
#else
    int acquired;
    if (!n) return DART_ERR_STATE;
    acquired = i_dart_node_lock(n);
    if (!acquired) return DART_ERR_STATE;               /* from a callback */
    if (n->svc_running){ i_dart_node_unlock(n, acquired); return DART_ERR_STATE; }
    n->svc_stop = 0;
    n->svc_running = 1;
    if (!i_dart_plat_thread_start(&n->svc, i_dart_node_service, n)){
        n->svc_running = 0;
        i_dart_node_unlock(n, acquired);
        return DART_ERR_NOSYS;
    }
    i_dart_node_unlock(n, acquired);                    /* the service takes the lock now */
    return DART_OK;
#endif
}

int dart_node_stop(DartNode *n){
#ifndef DART_THREADS
    (void)n;
    return DART_OK;                                     /* nothing to stop, idempotent */
#else
    int acquired;
    if (!n) return DART_ERR_STATE;
    acquired = i_dart_node_lock(n);
    if (!acquired) return DART_ERR_STATE;               /* from a callback (it is the service) */
    if (n->svc_joining){
        /* another thread owns the join: wait for it rather than join twice. Counted as
           a cv waiter so its broadcasts land */
        n->cv_waiters++;
        while (n->svc_running) i_dart_node_cv_wait(n, 100000u);
        n->cv_waiters--;
        i_dart_node_unlock(n, acquired);
        return DART_OK;
    }
    if (!n->svc_running){ i_dart_node_unlock(n, acquired); return DART_OK; }
    n->svc_joining = 1;
    n->svc_stop = 1;
    i_dart_node_kick(n);
    i_dart_node_unlock(n, acquired);
    i_dart_plat_thread_join(&n->svc);                   /* never joined holding the lock */
    acquired = i_dart_node_lock(n);
    n->svc_running = 0;
    n->svc_stop = 0;
    n->svc_joining = 0;
    /* free every waiter: blocked senders proceed unwaited, a parked stopper returns */
    if (n->cv_waiters) i_dart_plat_cond_broadcast(&n->cv);
    i_dart_node_unlock(n, acquired);
    return DART_OK;
#endif
}

int dart_node_is_started(DartNode *n){
#ifndef DART_THREADS
    (void)n;
    return 0;
#else
    return (n && n->svc_running) ? 1 : 0;
#endif
}

void dart_node_lock(DartNode *n){
#ifndef DART_THREADS
    (void)n;
#else
    if (!n) return;
    if (n->lock_held && n->lock_owner == i_dart_plat_thread_id()) return;  /* already ours */
    i_dart_node_lock_raw(n);
    n->user_locked = 1;
#endif
}

void dart_node_unlock(DartNode *n){
#ifndef DART_THREADS
    (void)n;
#else
    if (!n || !n->user_locked) return;   /* only releases what dart_node_lock took */
    if (!(n->lock_held && n->lock_owner == i_dart_plat_thread_id())) return;
    n->user_locked = 0;
    i_dart_node_unlock_raw(n);
#endif
}

int dart_node_close(DartNode *n, int send_bye){
    DartAllocator pool;
    if (!n) return DART_OK;
#ifdef DART_THREADS
    /* from a callback: refuse loudly, the return code lets a binding keep its handle alive */
    if (n->lock_held && n->lock_owner == i_dart_plat_thread_id()) return DART_ERR_STATE;
    dart_node_stop(n);
    /* from here no other thread is inside, or may enter, any call on this node */
#endif
    /* settle outstanding pattern promises while the node is fully alive, cleared first so
       a callback closing the node cannot recurse */
    if (n->sys_on_close){
        i_DartSysCloseFn f = n->sys_on_close;
        n->sys_on_close = NULL;
        f(n->sys_user);
    }
    /* discovery frees peer blobs via our hook, so it must close before the pool is copied out */
    if (n->discovery) dart_discovery_close(n->discovery, send_bye);
    if (n->fd != DART_SOCK_BAD) i_dart_plat_close(n->fd);
#ifdef DART_SHM
    if (n->shm_capable){                              /* the pool reset unmaps nothing */
        uint32_t i, n_segments = (uint32_t)n->shm_n_topics * DART_SHM_N_CLASSES;
        for (i=0;i<n_segments;i++)
            if (n->shm_pool[i]) i_dart_shm_detach((i_DartShmPool*)n->shm_pool[i]);
        for (i=0;i<n->shm_reader_count;i++)
            if (n->shm_reader_states[i]) i_dart_shm_detach((i_DartShmPool*)n->shm_reader_states[i]);
    }
#endif
#ifdef DART_THREADS
    if (n->waker.fd != DART_SOCK_BAD) i_dart_plat_waker_close(&n->waker);
    i_dart_plat_cond_destroy(&n->cv);
    i_dart_plat_mutex_destroy(&n->mu);
#endif
    i_dart_plat_cleanup();
    pool = n->pool;                /* copied out last, the reset frees n itself */
    dart_allocator_reset(&pool);   /* frees the node struct, arena, buffers, schemas and handles */
    return DART_OK;
}
#pragma endregion
#ifndef DART_NO_PATTERNS
#pragma region patterns/core.c
/* The patterns layer over the node's public API and its kind agnostic seams. The node
 * knows nothing of what these patterns mean. The rules are in spec/patterns.md. */
#include <string.h>
#include <stddef.h>            /* offsetof */

/* the per node manager: fans the node wide event and tick out to every entity */

/* one authority side entity keyed the way a peer's announce names it, for the duplicate
 * authority check */
typedef struct {
    uint32_t    hash;        /* the primary channel's low 32 name hash */
    uint8_t     kind;        /* DartEntityKind */
    DartTopic  *primary;     /* the entity's primary channel */
    uint32_t  **ids; uint16_t *n_ids, *cap;   /* the entity's reported peers list */
} i_DartPatAuth;

typedef struct i_DartPatterns {
    DartNode            *n;
    struct DartFunction *funcs;   /* linked lists for the tick and event fanout */
    struct DartVariable *vars;
    /* the authorities sorted by (kind, hash), rebuilt lazily after a create or retire */
    i_DartPatAuth       *auth; uint16_t auth_n, auth_cap; uint8_t auth_dirty;
    struct DartFunction *meta;    /* the built in @dart/meta endpoint, both sides in one handle */
    DartSchema          *meta_rsp_schema;   /* DartMeta { info: map } */
    uint8_t             *meta_msg; uint32_t meta_msg_cap;   /* reply scratch, grown on demand */
} i_DartPatterns;

static void     i_dart_patterns_on_event(void *user, const DartEvent *ev);
static uint64_t i_dart_patterns_tick(void *user, uint64_t now_us);
static void     i_dart_patterns_on_close(void *user);
/* the pending call reaper, retire cancels through it */
static uint64_t i_dart_func_reap(struct DartFunction *fn, uint64_t now,
                                 DartCallStatus fail_status, int all, uint32_t dest);
/* the duplicate authority sweep for a just created provider or owner */
static void i_dart_pat_dup_sweep(DartNode *n, DartTopic *primary, DartEntityKind kind,
                                 uint32_t **ids, uint16_t *n_ids, uint16_t *cap);
/* a peer's uuid low 32, the task wire's caller discriminator */
static uint32_t i_dart_pat_peer_lo(DartNode *n, uint32_t peer){
    const uint8_t *u = i_dart_node_peer_uuid(n, peer);
    return u ? i_dart_le_r32(u) : 0;
}

/* the entity channel belongs to as peer advertises it, by kind and base name. 1 + *out */
static int i_dart_pat_peer_entity(DartNode *n, uint32_t peer, DartTopic *channel, DartEntityKind kind,
                                  DartEntityInfo *out){
    DartString nm = i_dart_topic_name(channel);
    DartIter it; size_t len = nm.len;
    char buf[DART_TOPIC_NAME_MAX + 1]; uint32_t hash;
    memcpy(buf, nm.data, nm.len); buf[nm.len] = '\0';
    hash = (uint32_t)dart_topic_id(buf);           /* the primary channel's announce hash */
    if (len >= 5 && nm.data[len - 4] == '@') len -= 4;
    memset(&it, 0, sizeof it);
    while (dart_node_entities_next(n, peer, &it, out)){
        if (out->kind != kind) continue;
        /* rivals never exchange details, so the peer's name may never arrive: the hash
           decides then, the fetched name otherwise */
        if (out->name.len ? (out->name.len == len && memcmp(out->name.data, nm.data, len) == 0)
                          : out->hash == hash)
            return 1;
    }
    return 0;
}

/* Lazily creates the manager and registers the sys hooks. Lock held. NULL on OOM. */
static i_DartPatterns *i_dart_patterns_get(DartNode *n){
    void **slot = i_dart_node_sys_slot(n);
    i_DartPatterns *pm = (i_DartPatterns*)*slot;
    if (pm) return pm;
    pm = (i_DartPatterns*)i_dart_node_sys_alloc(n, NULL, sizeof *pm);
    if (!pm) return NULL;
    memset(pm, 0, sizeof *pm);
    pm->n = n; *slot = pm;
    i_dart_node_set_sys_hooks(n, i_dart_patterns_on_event, i_dart_patterns_tick,
                              i_dart_patterns_on_close, pm);
    return pm;
}

/* the shared entity mechanics: the create prologue and the three retire phases */

/* The create prologue: take the lock, get the manager, allocate a zeroed handle, release.
 * The lock must be released before the topics are created. See spec/patterns.md. */
static void *i_dart_pat_handle_new(DartNode *n, size_t size, i_DartPatterns **pm_out){
    i_DartPatterns *pm; void *h; int acquired;
    acquired = i_dart_node_sys_lock(n);
    pm = i_dart_patterns_get(n);
    h = pm ? i_dart_node_sys_alloc(n, NULL, size) : NULL;
    if (h) memset(h, 0, size);
    i_dart_node_sys_unlock(n, acquired);
    *pm_out = h ? pm : NULL;
    return h;
}

/* A partial create: the created half may already route into the handle, so park it and
 * keep the handle allocated until close. Returns NULL, the create's verdict. */
static void *i_dart_pat_half_create_fail(DartTopic *created_half){
    dart_topic_set_role(created_half, DART_INACTIVE);
    return NULL;
}

/* Retire phase 1, before the lock: park the channels. set_role refuses from a callback
 * with nothing mutated, so the primary's verdict is the whole retire's. */
static int i_dart_pat_park_channels(DartTopic *primary, DartTopic *secondary){
    int r = dart_topic_set_role(primary, DART_INACTIVE);
    if (r != 0) return r;
    if (secondary) (void)dart_topic_set_role(secondary, DART_INACTIVE);
    return 0;
}

/* Retire phase 2, lock held: drop the delivery routing into the handle. */
static void i_dart_pat_clear_channels(DartTopic *primary, DartTopic *secondary){
    i_dart_topic_clear_sys(primary);
    if (secondary) i_dart_topic_clear_sys(secondary);
}

/* Retire phase 3, lock released: release the slots for reuse. The topics outlive the
 * handle free, since a reap callback fired under the lock may have used them. */
static void i_dart_pat_release_channels(DartTopic *primary, DartTopic *secondary){
    (void)dart_topic_retire(primary);
    if (secondary) (void)dart_topic_retire(secondary);
}

/* Unlinks elem from a manager list threaded at next_off. An absent elem is a no op. Lock held. */
static void i_dart_pat_unlink(void **head, void *elem, size_t next_off){
    void **pp = head;
    while (*pp){
        void **next = (void**)((char*)*pp + next_off);
        if (*pp == elem){ *pp = *next; return; }
        pp = next;
    }
}

/* functions */

#define DART__FN_PREFIX 5u   /* req: [u32 call_id][u8 op]. rsp: [u32 call_id][u8 status], then
                                [u8 msg_len][msg], which the node's split puts in the header */
#define DART__FN_RSP_HDR_MAX (DART__FN_PREFIX + 1u + DART_CALL_MSG_MAX)
#define DART__FN_OP_CALL   0u   /* the payload is the request */
#define DART__FN_OP_CANCEL 1u   /* task req channel, op only: an empty payload cancels the
                                   sender's call, [u32 caller_lo] another caller's */
#define DART__PRG_PREFIX 8u  /* prg: [u32 caller_lo][u32 call_id]. caller_lo demuxes the shared
                                broadcast, since call ids are per caller counters */
#define DART__NO_DEADLINE ((uint64_t)-1)   /* a RUNNING call has no timeout */

/* the default response text, so a generic consumer always has text for a failure */
static DartString i_dart_call_status_msg(DartCallStatus s){
    switch (s){
    case DART_CALL_APP_ERROR:  return dart_cstr("app error");
    case DART_CALL_NO_HANDLER: return dart_cstr("no handler");
    case DART_CALL_TIMEOUT:    return dart_cstr("timeout");
    case DART_CALL_PEER_LOST:  return dart_cstr("peer lost");
    case DART_CALL_CANCELLED:  return dart_cstr("cancelled");
    case DART_CALL_OK: default: return dart_cstr("");
    }
}

/* A caller side outstanding call. queued_req != NULL = not on the wire yet: the request
 * waits for the first provider match. See spec/patterns.md. */
typedef struct i_DartPending {
    struct i_DartPending *next;
    uint32_t        call_id;
    uint32_t        dest;        /* the peer the call is directed at, 0 = undirected */
    uint64_t        deadline_us; /* DART__NO_DEADLINE once RUNNING or progress arrived */
    DartResponseFn  on_response;
    void           *user;
    DartProgressFn  on_progress; /* task: per progress update */
    void           *progress_user;
    uint8_t         running;     /* a non terminal response arrived, the deadline is dropped */
    uint8_t        *queued_req;
    uint32_t        queued_len;
} i_DartPending;

/* a deferred provider reply, linked into the handle's live defer registry so every token
 * verb validates membership first */
typedef struct i_DartDefer {
    struct i_DartDefer *next;
    DartFunction *fn;
    uint32_t      caller;
    uint32_t      caller_lo;   /* the caller's uuid low 32 for progress and cancel */
    uint32_t      call_id;
    uint8_t       cancelled;   /* a cancel landed */
} i_DartDefer;

struct DartFunction {
    DartNode       *n;
    i_DartPatterns *pm;
    struct DartFunction *next;      /* the manager list */
    DartTopic      *req;            /* provider SUB_ONLY, caller PUB_ONLY */
    DartTopic      *rsp;            /* provider PUB_ONLY, caller SUB_ONLY, directed */
    DartTopic      *prg;            /* task: the broadcast progress channel, NULL = a function */
    DartRequestFn   on_request; void *on_request_user;   /* provider */
    uint32_t        next_call_id;   /* the caller counter */
    uint32_t        timeout_us;
    i_DartPending  *pending;        /* caller: outstanding calls */
    i_DartDefer    *defers;         /* provider: the live defer registry */
    DartCancelFn    on_cancel; void *on_cancel_user;     /* task definition, one slot */
    uint32_t        self_lo;        /* our own uuid low 32, the progress demux filter */
    uint8_t        *sync_buf; uint32_t sync_cap;   /* the sync call reply scratch */
    char            sync_msg[DART_CALL_MSG_MAX];   /* the sync call message scratch */
    uint8_t         sync_msg_len;
    uint8_t         is_provider;    /* a pure definition, 0 for a both sides handle */
    uint8_t         both;           /* both sides in one handle, the @dart/meta shape */
    uint8_t         is_task;        /* the task shape */
    uint8_t         no_cancel, exclusive;   /* task definition: the declared attrs */
    uint8_t         multi;          /* duplicate authority expected */
    uint32_t       *dup_peers;      /* provider: peers already reported for duplicate authority */
    uint16_t        dup_n, dup_cap;
    uint8_t         reflect;        /* created with reflect_from_mesh */
    uint64_t        generation;     /* the mesh generation the schemas were taken at */
};

/* the transient request handed to the handler: the public head first, so the reply entry
 * points can downcast the handler's pointer back to this */
typedef struct {
    DartRequest   pub;
    DartFunction *fn;
    uint32_t      call_id;
    uint32_t      caller_lo;   /* task: the caller's uuid low 32 */
    uint8_t       replied;     /* a reply, fail or defer happened: no auto ack */
    uint8_t       started;     /* task: RUNNING already sent */
} i_DartRequest;
/* the downcast needs pub at offset 0, and C99 has no static assert */
typedef char i_dart_request_pub_first[(offsetof(i_DartRequest, pub) == 0) ? 1 : -1];

/* builds the response header. A message over DART_CALL_MSG_MAX truncates. Returns the length */
static size_t i_dart_func_rsp_hdr(uint8_t *hdr, uint32_t call_id, uint8_t status,
                                  const char *message){
    size_t ml = message ? strlen(message) : 0;
    if (ml > DART_CALL_MSG_MAX) ml = DART_CALL_MSG_MAX;
    i_dart_le_w32(hdr, call_id); hdr[4] = status; hdr[5] = (uint8_t)ml;
    if (ml) memcpy(hdr + 6, message, ml);
    return DART__FN_PREFIX + 1u + ml;
}

/* sends a response to one caller. Runs under the lock, so the send commits reentrantly */
static void i_dart_func_send_reply(DartFunction *fn, uint32_t caller, uint32_t call_id,
                                   uint8_t status, const char *message, DartBytes rsp){
    uint8_t hdr[DART__FN_RSP_HDR_MAX];
    size_t hl = i_dart_func_rsp_hdr(hdr, call_id, status, message);
    (void)i_dart_topic_send_to(fn->rsp, caller, dart_bytes(hdr, hl), rsp);
}

/* provider: a CANCEL op landed. The target is the payload's caller_lo, else the sender's.
 * A match on a live defer sets its flag and fires on_cancel once, no match is a no op. */
static void i_dart_func_cancel_op(DartFunction *fn, const DartMsg *msg){
    uint32_t lo = msg->data.len >= 4 ? i_dart_le_r32(msg->data.data)
                                     : i_dart_pat_peer_lo(msg->node, msg->publisher_id);
    uint32_t call_id = i_dart_le_r32(msg->header.data);
    i_DartDefer *d;
    for (d = fn->defers; d; d = d->next)
        if (d->caller_lo == lo && d->call_id == call_id) break;
    if (!d || d->cancelled) return;
    d->cancelled = 1;
    if (fn->on_cancel) fn->on_cancel((uint64_t)(uintptr_t)d, fn->on_cancel_user);
}

/* provider: a request arrived, or an op */
static void i_dart_func_on_request(void *user, const DartMsg *msg){
    DartFunction *fn = (DartFunction*)user;
    i_DartRequest r;
    if (msg->header.len < DART__FN_PREFIX) return;   /* a malformed prefix */
    if (msg->header.data[4] != DART__FN_OP_CALL){    /* an op, not a request */
        if (fn->is_task && msg->header.data[4] == DART__FN_OP_CANCEL)
            i_dart_func_cancel_op(fn, msg);
        return;   /* an unknown op is dropped */
    }
    r.pub.node          = msg->node;
    r.pub.function_name = dart_string(msg->topic_name.data,
                              msg->topic_name.len > 4u ? msg->topic_name.len - 4u : 0);
    r.pub.data          = msg->data;
    r.pub.schema        = msg->schema;
    r.pub.caller        = msg->publisher_id;
    r.pub.caller_name   = msg->publisher_name;
    r.pub.recv_us       = msg->recv_us;
    r.pub.written_us    = msg->written_us;
    r.fn = fn; r.call_id = i_dart_le_r32(msg->header.data); r.replied = 0; r.started = 0;
    r.caller_lo = fn->is_task ? i_dart_pat_peer_lo(msg->node, msg->publisher_id) : 0;
    if (fn->on_request) fn->on_request(&r.pub, fn->on_request_user);
    else {   /* no handler: NO_HANDLER with no text, the caller fills the default */
        i_dart_func_send_reply(fn, r.pub.caller, r.call_id, DART_CALL_NO_HANDLER, NULL, dart_bytes(NULL,0));
        r.replied = 1;
    }
    if (!r.replied){
        /* a function's return is its answer, a task's is not: an instant empty OK on a
           long operation would read as success that never ran */
        if (fn->is_task)
            i_dart_func_send_reply(fn, r.pub.caller, r.call_id, DART_CALL_APP_ERROR,
                                   "handler returned no result", dart_bytes(NULL,0));
        else
            i_dart_func_send_reply(fn, r.pub.caller, r.call_id, DART_CALL_OK, NULL, dart_bytes(NULL,0));
    }
}

/* caller: unlinks the pending entry for call_id, so a second provider's reply finds none */
static i_DartPending *i_dart_func_take_pending(DartFunction *fn, uint32_t call_id){
    i_DartPending **pp = &fn->pending, *p;
    for (; (p = *pp) != NULL; pp = &p->next)
        if (p->call_id == call_id){ *pp = p->next; return p; }
    return NULL;
}

/* caller: the pending entry for call_id, left linked */
static i_DartPending *i_dart_func_find_pending(DartFunction *fn, uint32_t call_id){
    i_DartPending *p;
    for (p = fn->pending; p; p = p->next) if (p->call_id == call_id) return p;
    return NULL;
}

/* a non terminal response reached p: the call runs as long as it runs now */
static void i_dart_func_mark_running(i_DartPending *p){
    p->running = 1; p->deadline_us = DART__NO_DEADLINE;
}

/* frees an unlinked pending entry and any queued request bytes */
static void i_dart_func_free_pending(DartFunction *fn, i_DartPending *p){
    if (p->queued_req) i_dart_node_sys_alloc(fn->n, p->queued_req, 0);
    i_dart_node_sys_alloc(fn->n, p, 0);
}

/* Sends every call queued before a provider matched, oldest first. Runs under the lock,
 * so the sends commit reentrantly, fine on a fresh lane with empty history. */
static void i_dart_func_flush_queued(DartFunction *fn){
    if (fn->is_provider || !fn->pending) return;
    if (dart_topic_match_count(fn->req) == 0) return;   /* still no provider */
    for (;;){
        i_DartPending *pick = NULL, *p;
        uint8_t hdr[DART__FN_PREFIX];
        for (p = fn->pending; p; p = p->next) if (p->queued_req) pick = p;
        if (!pick) return;
        if (fn->is_task && !pick->dest)   /* task requests are always directed: resolve now */
            pick->dest = i_dart_topic_oldest_match(fn->req);
        i_dart_le_w32(hdr, pick->call_id); hdr[4] = DART__FN_OP_CALL;
        if (pick->dest)
            (void)i_dart_topic_send_to(fn->req, pick->dest, dart_bytes(hdr, DART__FN_PREFIX),
                                       dart_bytes(pick->queued_req, pick->queued_len));
        else
            (void)i_dart_topic_send_hdr(fn->req, dart_bytes(hdr, DART__FN_PREFIX),
                                        dart_bytes(pick->queued_req, pick->queued_len));
        i_dart_node_sys_alloc(fn->n, pick->queued_req, 0);
        pick->queued_req = NULL; pick->queued_len = 0;
    }
}

/* caller: a progress datagram. caller_lo filters other callers' calls off the broadcast.
 * Progress can beat RUNNING on its own lane, so it also drops the deadline. */
static void i_dart_func_on_progress(void *user, const DartMsg *msg){
    DartFunction *fn = (DartFunction*)user;
    i_DartPending *p;
    DartProgress pr;
    if (msg->header.len < DART__PRG_PREFIX) return;
    if (i_dart_le_r32(msg->header.data) != fn->self_lo) return;   /* another caller's call */
    p = i_dart_func_find_pending(fn, i_dart_le_r32(msg->header.data + 4));
    if (!p) return;   /* already answered: a straggler */
    i_dart_func_mark_running(p);
    if (!p->on_progress) return;
    pr.call_id = p->call_id; pr.provider = msg->publisher_id;
    pr.data = msg->data; pr.schema = msg->schema;
    pr.written_us = msg->written_us; pr.recv_us = msg->recv_us;
    pr.user = p->progress_user;
    p->on_progress(&pr);
}

/* caller: a response arrived */
static void i_dart_func_on_response(void *user, const DartMsg *msg){
    DartFunction *fn = (DartFunction*)user;
    i_DartPending *p;
    DartResponse r;
    if (msg->header.len < DART__FN_PREFIX) return;
    if (fn->is_task && (DartCallStatus)msg->header.data[4] == DART_CALL_RUNNING){
        /* non terminal: the call stays pending with no deadline and on_progress fires once
           with zero length data, unless progress already beat the status here */
        p = i_dart_func_find_pending(fn, i_dart_le_r32(msg->header.data));
        if (!p) return;
        if (!p->running && p->on_progress){
            DartProgress pr;
            pr.call_id = p->call_id; pr.provider = msg->publisher_id;
            pr.data = dart_bytes(NULL, 0); pr.schema = NULL;
            pr.written_us = msg->written_us; pr.recv_us = msg->recv_us;
            pr.user = p->progress_user;
            i_dart_func_mark_running(p);
            p->on_progress(&pr);
        } else i_dart_func_mark_running(p);
        return;
    }
    p = i_dart_func_take_pending(fn, i_dart_le_r32(msg->header.data));
    if (!p) return;                          /* an unknown or duplicate call_id: dropped */
    r.status = (DartCallStatus)msg->header.data[4];
    r.data = msg->data; r.schema = msg->schema;
    r.provider = msg->publisher_id; r.user = p->user;
    r.written_us = msg->written_us;
    /* the message follows the prefix as [u8 len][msg], and the node's split already
       bounded it, so the header length is the truth. None = the default status text */
    r.message = (msg->header.len > DART__FN_PREFIX + 1u)
              ? dart_string((const char*)msg->header.data + DART__FN_PREFIX + 1u,
                            msg->header.len - (DART__FN_PREFIX + 1u))
              : i_dart_call_status_msg(r.status);
    if (p->on_response) p->on_response(&r);
    i_dart_func_free_pending(fn, p);
}

/* a leading '@' is reserved for the @dart/ builtins, refused in every public constructor */
static int i_dart_pat_reserved(const char *name){ return name && name[0] == '@'; }

/* Creates the channels for a function or task. mode 1 = a definition, 0 = a remote, 2 =
 * both sides in one handle (the @dart/meta shape). topts non NULL = the task shape. */
static DartFunction *i_dart_function_new(DartNode *n, const char *name,
                    const DartSchema *req_schema, const DartSchema *rsp_schema,
                    DartRequestFn on_request, void *user, const DartFunctionOpts *opts, int mode,
                    const DartSchema *prg_schema, const DartTaskOpts *topts){
    i_DartPatterns *pm; DartFunction *fn;
    char rn[DART_TOPIC_NAME_MAX + 1]; size_t nl;
    DartTopicOpts topt; int acquired;
    int handles_req = (mode != 0), makes_calls = (mode != 1);
    DartRole req_role = mode == 2 ? DART_PUBSUB : (handles_req ? DART_SUB_ONLY : DART_PUB_ONLY);
    DartRole rsp_role = mode == 2 ? DART_PUBSUB : (handles_req ? DART_PUB_ONLY : DART_SUB_ONLY);
    i_DartSysMsgFn req_cb = handles_req ? i_dart_func_on_request : NULL;
    i_DartSysMsgFn rsp_cb = makes_calls ? i_dart_func_on_response : NULL;
    uint8_t req_kind = topts ? DART_KIND_TASK_REQ : DART_KIND_FUNC_REQ;
    uint8_t rsp_kind = topts ? DART_KIND_TASK_RSP : DART_KIND_FUNC_RSP;
    uint8_t req_attrs = 0;
    if (topts && handles_req)   /* only the definition declares the task facts */
        req_attrs = (uint8_t)((topts->no_cancel ? 0u : DART_ATTR_CANCELLABLE)
                            | (topts->exclusive ? DART_ATTR_EXCLUSIVE : 0u)
                            | (topts->multi     ? DART_ATTR_MULTI     : 0u));
    if (!n || !name) return NULL;
    nl = strlen(name);
    if (nl == 0 || nl + 4 > DART_TOPIC_NAME_MAX) return NULL;   /* room for the suffix */
    memset(&topt, 0, sizeof topt);
    topt.qos.reliability = DART_RELIABLE;
    topt.qos.catch_up = 0;   /* calls carry no replay: history is purely the repair window */
    /* an inline reply is a reentrant send that cannot wait for a TX pass, so this ring is
       the only thing holding a drained batch of replies. See spec/patterns.md */
    topt.qos.keep_last = (opts && opts->keep_last) ? opts->keep_last : 0u;
    topt.qos.backpressure_wait_us = (opts && opts->backpressure_wait_us) ? opts->backpressure_wait_us
                                                                         : DART_PATTERN_BP_WAIT_US;
    fn = (DartFunction*)i_dart_pat_handle_new(n, sizeof *fn, &pm);
    if (!fn) return NULL;
    if (opts && opts->reflect_from_mesh){
        /* the caller writes @req and reads @rsp and @prg, a definition the reverse */
        DartEntityKind ek = topts ? DART_ENTITY_TASK : DART_ENTITY_FUNCTION;
        const DartSchema *ms;
        fn->reflect = 1;
        i_dart_node_reflect_pick(n, ek, name, 0, !handles_req, &ms, NULL, &fn->generation);
        if (!req_schema) req_schema = ms;
        i_dart_node_reflect_pick(n, ek, name, 1, handles_req, &ms, NULL, NULL);
        if (!rsp_schema) rsp_schema = ms;
        if (topts){
            i_dart_node_reflect_pick(n, ek, name, 2, handles_req, &ms, NULL, NULL);
            if (!prg_schema) prg_schema = ms;
        }
    }
    fn->n = n; fn->pm = pm; fn->is_provider = (uint8_t)(mode == 1);
    fn->both = (uint8_t)(mode == 2);
    fn->multi = (uint8_t)(opts && opts->multi);
    fn->is_task = (uint8_t)(topts != NULL);
    if (topts){ fn->no_cancel = topts->no_cancel; fn->exclusive = topts->exclusive; }
    fn->on_request = on_request; fn->on_request_user = user;
    fn->timeout_us = (opts && opts->timeout_us) ? opts->timeout_us : DART_CALL_TIMEOUT_US;
    fn->next_call_id = 1;
    {   const uint8_t *u = i_dart_node_uuid(n);   /* the progress demux filter */
        fn->self_lo = u ? i_dart_le_r32(u) : 0;
    }

    memcpy(rn, name, nl); memcpy(rn + nl, "@req", 5);   /* NUL included */
    /* the req channel is directed in every mode, or a directed request would leak to every
       other matched provider when their lanes next wake. See spec/patterns.md */
    fn->req = i_dart_node_create_pattern_topic(n, rn, req_role, req_schema, &topt,
                              req_kind, DART__FN_PREFIX, 1 /*directed*/, req_attrs, req_cb, fn);
    if (!fn->req) return NULL;   /* fn stays pool allocated: nothing routes into it yet */
    if (topts){
        /* the broadcast progress channel: reliability is the definition's offer or the
           remote's request, the RxO rule composes them */
        DartTopicOpts popt = topt;
        popt.qos.reliability = topts->progress_best_effort ? DART_BEST_EFFORT : DART_RELIABLE;
        if (topts->progress_keep_last) popt.qos.keep_last = topts->progress_keep_last;
        memcpy(rn + nl, "@prg", 5);
        fn->prg = i_dart_node_create_pattern_topic(n, rn,
                              handles_req ? DART_PUB_ONLY : DART_SUB_ONLY, prg_schema, &popt,
                              DART_KIND_TASK_PRG, DART__PRG_PREFIX, 0, 0,
                              handles_req ? NULL : i_dart_func_on_progress, fn);
        if (!fn->prg)
            return (DartFunction*)i_dart_pat_half_create_fail(fn->req);
    }
    memcpy(rn + nl, "@rsp", 5);
    fn->rsp = i_dart_node_create_pattern_topic(n, rn, rsp_role, rsp_schema, &topt,
                              rsp_kind, DART__FN_PREFIX, 1 /*directed*/, 0, rsp_cb, fn);
    if (!fn->rsp){  /* req and prg already route into fn: keep the handle, park the halves */
        if (fn->prg) (void)dart_topic_set_role(fn->prg, DART_INACTIVE);
        return (DartFunction*)i_dart_pat_half_create_fail(fn->req);
    }

    acquired = i_dart_node_sys_lock(n);       /* publish into the manager list */
    fn->next = pm->funcs; pm->funcs = fn; pm->auth_dirty = 1;
    if (handles_req && !fn->multi)   /* a rival provider may already be on the network */
        i_dart_pat_dup_sweep(n, fn->req, topts ? DART_ENTITY_TASK : DART_ENTITY_FUNCTION,
                             &fn->dup_peers, &fn->dup_n, &fn->dup_cap);
    i_dart_node_sys_unlock(n, acquired);
    return fn;
}

DartFunction *dart_node_create_function_definition(DartNode *n, const char *name,
                    const DartSchema *req_schema, const DartSchema *rsp_schema,
                    DartRequestFn on_request, void *user, const DartFunctionOpts *opts){
    if (i_dart_pat_reserved(name)) return NULL;
    return i_dart_function_new(n, name, req_schema, rsp_schema, on_request, user, opts, 1, NULL, NULL);
}
DartFunction *dart_node_create_remote_function(DartNode *n, const char *name,
                    const DartSchema *req_schema, const DartSchema *rsp_schema,
                    const DartFunctionOpts *opts){
    if (i_dart_pat_reserved(name)) return NULL;
    return i_dart_function_new(n, name, req_schema, rsp_schema, NULL, NULL, opts, 0, NULL, NULL);
}

/* a pattern channel's entity name, the topic name minus its suffix, in a static scratch */
static const char *i_dart_pat_base_name(DartTopic *channel){
    static char buf[DART_TOPIC_NAME_MAX + 1];
    DartString nm = i_dart_topic_name(channel);
    size_t len = nm.len;
    if (len >= 5 && nm.data[len - 4] == '@') len -= 4;
    memcpy(buf, nm.data, len); buf[len] = '\0';
    return buf;
}

/* the shared function fields of DartTaskOpts, so the task constructors reuse one path */
static DartFunctionOpts i_dart_task_fn_opts(const DartTaskOpts *to){
    DartFunctionOpts fo;
    memset(&fo, 0, sizeof fo);
    fo.backpressure_wait_us = to->backpressure_wait_us;
    fo.timeout_us = to->timeout_us;
    fo.keep_last = to->keep_last;
    fo.multi = to->multi;
    return fo;
}

DartFunction *dart_node_create_task_definition(DartNode *n, const char *name,
                    const DartSchema *req_schema, const DartSchema *prg_schema,
                    const DartSchema *rsp_schema, DartRequestFn on_request, void *user,
                    const DartTaskOpts *opts){
    DartTaskOpts to; DartFunctionOpts fo;
    if (i_dart_pat_reserved(name)) return NULL;
    memset(&to, 0, sizeof to); if (opts) to = *opts;
    fo = i_dart_task_fn_opts(&to);
    return i_dart_function_new(n, name, req_schema, rsp_schema, on_request, user, &fo, 1,
                               prg_schema, &to);
}
DartFunction *dart_node_create_remote_task(DartNode *n, const char *name,
                    const DartSchema *req_schema, const DartSchema *prg_schema,
                    const DartSchema *rsp_schema, const DartTaskOpts *opts){
    DartTaskOpts to; DartFunctionOpts fo;
    if (i_dart_pat_reserved(name)) return NULL;
    memset(&to, 0, sizeof to); if (opts) to = *opts;
    fo = i_dart_task_fn_opts(&to);
    return i_dart_function_new(n, name, req_schema, rsp_schema, NULL, NULL, &fo, 0,
                               prg_schema, &to);
}

/* Links the pending entry under the lock, then sends outside it so the request engages the
 * flow control wait. A task call with no provider auto directs at the oldest matched. */
static int i_dart_function_call_id(DartFunction *fn, DartBytes req, DartResponseFn on_response,
                                   void *user, const DartCallOpts *opts, uint32_t *id_out){
    uint8_t hdr[DART__FN_PREFIX]; i_DartPending *p; int acquired, r; uint32_t id;
    uint32_t dest = opts ? opts->provider : 0;
    if (!fn || !fn->req || !fn->rsp) return DART_ERR_NO_TOPIC;
    acquired = i_dart_node_sys_lock(fn->n);
    p = (i_DartPending*)i_dart_node_sys_alloc(fn->n, NULL, sizeof *p);
    if (!p){ i_dart_node_sys_unlock(fn->n, acquired); return DART_ERR_OOM; }
    if (fn->is_task && !dest) dest = i_dart_topic_oldest_match(fn->req);   /* always directed */
    id = fn->next_call_id++;
    p->call_id = id;
    p->dest = dest;
    p->deadline_us = i_dart_node_now_us(fn->n) + fn->timeout_us;
    p->on_response = on_response; p->user = user;
    p->on_progress = opts ? opts->on_progress : NULL;
    p->progress_user = opts ? opts->progress_user : NULL;
    p->running = 0;
    p->queued_req = NULL; p->queued_len = 0;
    p->next = fn->pending; fn->pending = p;
    if (id_out) *id_out = id;
    if (opts && opts->id_out) *opts->id_out = id;   /* set before the send */
    if (dart_topic_match_count(fn->req) == 0){
        /* no provider matched yet: the transport would drop an unmatched send, so queue
           the request and flush the instant the match forms. See spec/patterns.md */
        p->queued_req = (uint8_t*)i_dart_node_sys_alloc(fn->n, NULL, req.len ? req.len : 1u);
        if (!p->queued_req){
            fn->pending = p->next;
            i_dart_node_sys_alloc(fn->n, p, 0);
            i_dart_node_sys_unlock(fn->n, acquired);
            return DART_ERR_OOM;
        }
        if (req.len) memcpy(p->queued_req, req.data, req.len);
        p->queued_len = (uint32_t)req.len;
        i_dart_node_sys_unlock(fn->n, acquired);
        return DART_OK;
    }
    i_dart_node_sys_unlock(fn->n, acquired);
    i_dart_le_w32(hdr, id); hdr[4] = DART__FN_OP_CALL;
    r = dest ? i_dart_topic_send_to(fn->req, dest, dart_bytes(hdr, DART__FN_PREFIX), req)
             : i_dart_topic_send_hdr(fn->req, dart_bytes(hdr, DART__FN_PREFIX), req);
    if (r != DART_OK){
        acquired = i_dart_node_sys_lock(fn->n);
        p = i_dart_func_take_pending(fn, id);   /* may already be reaped or answered: then gone */
        if (p) i_dart_func_free_pending(fn, p);
        i_dart_node_sys_unlock(fn->n, acquired);
        return r;
    }
    return DART_OK;
}

int dart_function_call_async(DartFunction *fn, DartBytes req, DartResponseFn on_response,
                             void *user, const DartCallOpts *opts){
    return i_dart_function_call_id(fn, req, on_response, user, opts, NULL);
}

/* Answers and frees every live deferred call of a definition with CANCELLED. Lock held,
 * the replies commit reentrantly, so run it while the rsp channel is still up. */
static void i_dart_func_drain_defers(DartFunction *fn, const char *message){
    while (fn->defers){
        i_DartDefer *d = fn->defers; fn->defers = d->next;
        i_dart_func_send_reply(fn, d->caller, d->call_id, DART_CALL_CANCELLED,
                               message, dart_bytes(NULL, 0));
        i_dart_node_sys_alloc(fn->n, d, 0);
    }
}

int dart_function_retire(DartFunction *fn){
    i_DartPatterns *pm; DartNode *n; int acquired, r;
    DartTopic *req, *rsp, *prg;
    if (!fn) return DART_ERR_NO_TOPIC;
    pm = fn->pm; n = fn->n;
    if (pm && fn == pm->meta) return DART_ERR_STATE;   /* the builtin is node infrastructure */
    acquired = i_dart_node_sys_lock(n);
    if (!acquired){   /* from a callback: refuse with nothing mutated */
        i_dart_node_sys_unlock(n, acquired);
        return DART_ERR_STATE;
    }
    /* live deferred calls answer CANCELLED while the channels are still up, and the
       replies must flush to the wire before the park below tears the lanes down */
    i_dart_func_drain_defers(fn, "provider retired");
    i_dart_node_sys_unlock(n, acquired);
    i_dart_node_flush_tx(n);
    r = i_dart_pat_park_channels(fn->req, fn->rsp);
    if (r != 0) return r;
    if (fn->prg) (void)dart_topic_set_role(fn->prg, DART_INACTIVE);
    acquired = i_dart_node_sys_lock(n);
    i_dart_pat_clear_channels(fn->req, fn->rsp);
    if (fn->prg) i_dart_topic_clear_sys(fn->prg);
    /* a request that deferred in the drain to park window: a best effort answer, and the
       entries are freed either way */
    i_dart_func_drain_defers(fn, "provider retired");
    /* every outstanding call gets its one outcome, CANCELLED. A reentrant call from a
       cancel callback queues and the next round cancels it too */
    while (fn->pending) i_dart_func_reap(fn, 0, DART_CALL_CANCELLED, 1, 0);
    if (pm){ i_dart_pat_unlink((void**)&pm->funcs, fn, offsetof(DartFunction, next)); pm->auth_dirty = 1; }
    req = fn->req; rsp = fn->rsp; prg = fn->prg;   /* outlive fn, see phase 3 */
    if (fn->sync_buf)  i_dart_node_sys_alloc(n, fn->sync_buf, 0);
    if (fn->dup_peers) i_dart_node_sys_alloc(n, fn->dup_peers, 0);
    i_dart_node_sys_alloc(n, fn, 0);
    i_dart_node_sys_unlock(n, acquired);
    i_dart_pat_release_channels(req, rsp);
    if (prg) (void)dart_topic_retire(prg);
    return DART_OK;
}

int dart_function_refresh(DartFunction *fn){
    DartNode *n; DartEntityKind ek; uint64_t gen = 0; int acquired, r;
    const DartSchema *rq = NULL, *rs = NULL, *pg = NULL;
    if (!fn) return DART_ERR_NO_TOPIC;
    if (!fn->reflect) return DART_ERR_ROLE;
    n = fn->n; ek = fn->is_task ? DART_ENTITY_TASK : DART_ENTITY_FUNCTION;
    if (!i_dart_node_reflect_pick(n, ek, i_dart_pat_base_name(fn->req), 0, !fn->is_provider, &rq, NULL, &gen)
        || gen == fn->generation) return 0;
    i_dart_node_reflect_pick(n, ek, i_dart_pat_base_name(fn->req), 1, fn->is_provider, &rs, NULL, NULL);
    if (fn->prg) i_dart_node_reflect_pick(n, ek, i_dart_pat_base_name(fn->req), 2, fn->is_provider, &pg, NULL, NULL);
    /* every outstanding call gets its one outcome before the lanes go */
    acquired = i_dart_node_sys_lock(n);
    if (!acquired){ i_dart_node_sys_unlock(n, acquired); return DART_ERR_STATE; }
    i_dart_func_drain_defers(fn, "provider re-typed");
    while (fn->pending) i_dart_func_reap(fn, 0, DART_CALL_CANCELLED, 1, 0);
    i_dart_node_sys_unlock(n, acquired);
    i_dart_node_flush_tx(n);
    r = i_dart_topic_retype(fn->req, rq, DART_RELIABLE);
    if (r != 0) return r;
    r = i_dart_topic_retype(fn->rsp, rs, DART_RELIABLE);
    if (r != 0) return r;
    if (fn->prg){
        r = i_dart_topic_retype(fn->prg, pg, i_dart_topic_reliability(fn->prg));
        if (r != 0) return r;
    }
    fn->generation = gen;
    return 1;
}

/* The blocking call's response capture: copies the payload into the function's scratch.
 * The schema pointer is safe to hold, an interned schema lives until close. */
typedef struct { DartFunction *fn; volatile int done; DartCallStatus status;
                 const DartSchema *schema; uint32_t len; uint32_t provider;
                 uint64_t written_us; } i_DartSyncCtx;
static void i_dart_func_sync_response(const DartResponse *r){
    i_DartSyncCtx *c = (i_DartSyncCtx*)r->user;
    DartFunction *fn = c->fn;
    c->status = r->status; c->schema = r->schema; c->len = (uint32_t)r->data.len;
    c->provider = r->provider; c->written_us = r->written_us;
    {   /* the message view dies with the callback: copy into the handle's scratch */
        size_t ml = r->message.len <= DART_CALL_MSG_MAX ? r->message.len : DART_CALL_MSG_MAX;
        if (ml) memcpy(fn->sync_msg, r->message.data, ml);
        fn->sync_msg_len = (uint8_t)ml;
    }
    if (r->data.len){
        if (fn->sync_cap < r->data.len){
            uint8_t *nb = (uint8_t*)i_dart_node_sys_alloc(fn->n, fn->sync_buf, r->data.len);
            if (nb){ fn->sync_buf = nb; fn->sync_cap = (uint32_t)r->data.len; }
            else c->len = 0;
        }
        if (fn->sync_cap >= r->data.len) memcpy(fn->sync_buf, r->data.data, r->data.len);
    }
    c->done = 1;
}

int dart_function_call(DartFunction *fn, DartBytes req, DartResponse *out, int timeout_ms,
                       const DartCallOpts *opts){
    i_DartSyncCtx ctx; int acquired, r; uint64_t deadline; uint32_t id;
    if (!fn) return DART_ERR_NO_TOPIC;
    acquired = i_dart_node_sys_lock(fn->n);
    if (!acquired || dart_node_is_started(fn->n)){   /* a callback or a service thread */
        i_dart_node_sys_unlock(fn->n, acquired);
        return DART_ERR_STATE;
    }
    i_dart_node_sys_unlock(fn->n, acquired);
    ctx.fn = fn; ctx.done = 0; ctx.status = DART_CALL_TIMEOUT; ctx.schema = NULL; ctx.len = 0;
    ctx.provider = 0; ctx.written_us = 0;
    r = i_dart_function_call_id(fn, req, i_dart_func_sync_response, &ctx, opts, &id);
    if (r != DART_OK) return r;
    deadline = i_dart_node_now_us(fn->n)
             + (uint64_t)(timeout_ms >= 0 ? (uint64_t)timeout_ms * 1000u : fn->timeout_us) + 20000u;
    while (!ctx.done){
        if (i_dart_node_now_us(fn->n) >= deadline){
            /* a task that reached RUNNING has no deadline: wait for the terminal outcome,
               and cancel from another thread is the impatience tool */
            int running = 0;
            acquired = i_dart_node_sys_lock(fn->n);
            {   i_DartPending *p = i_dart_func_find_pending(fn, id);
                running = p && p->running;
            }
            i_dart_node_sys_unlock(fn->n, acquired);
            if (!running) break;
            deadline = DART__NO_DEADLINE;
        }
        i_dart_node_sys_poll(fn->n, 5);            /* the tick synthesizes TIMEOUT */
    }
    if (!ctx.done){
        /* the local wait expired first: unlink the entry before this frame dies, or a late
           response fires into a reclaimed frame. A racing poller may have completed it */
        i_DartPending *p;
        acquired = i_dart_node_sys_lock(fn->n);
        if (!ctx.done && (p = i_dart_func_take_pending(fn, id)) != NULL)
            i_dart_node_sys_alloc(fn->n, p, 0);
        i_dart_node_sys_unlock(fn->n, acquired);
    }
    if (out){
        memset(out, 0, sizeof *out);
        out->status = ctx.done ? ctx.status : DART_CALL_TIMEOUT;
        out->data = dart_bytes(fn->sync_buf,
                               (ctx.done && ctx.status != DART_CALL_TIMEOUT) ? ctx.len : 0);
        out->schema = out->data.len ? ctx.schema : NULL;
        out->provider = ctx.done ? ctx.provider : 0;
        out->written_us = ctx.done ? ctx.written_us : 0;
        /* the same lifetime rule as data: the scratch holds it until the next blocking call */
        out->message = ctx.done ? dart_string(fn->sync_msg, fn->sync_msg_len)
                                : i_dart_call_status_msg(DART_CALL_TIMEOUT);
    }
    /* 0 = timed out, whichever deadline expired first, 1 = a real outcome */
    return (ctx.done && ctx.status != DART_CALL_TIMEOUT) ? 1 : 0;
}

int dart_function_match_count(DartFunction *fn){
    if (!fn) return 0;
    return dart_topic_match_count(fn->is_provider ? fn->rsp : fn->req);
}

/* The provider handler API. The handler's DartRequest is the pub head of an i_DartRequest,
 * so these recover the reply machinery by downcast. A copy answers nothing. */
static void i_dart_request_answer(DartRequest *request, uint8_t status, const char *message,
                                  DartBytes rsp){
    i_DartRequest *r = (i_DartRequest*)request;
    if (!request || r->replied) return;
    r->replied = 1;
    i_dart_func_send_reply(r->fn, request->caller, r->call_id, status, message, rsp);
}
void dart_request_reply(DartRequest *request, DartBytes rsp){
    i_dart_request_answer(request, DART_CALL_OK, NULL, rsp);
}
void dart_request_fail (DartRequest *request, const char *message, DartBytes rsp){
    i_dart_request_answer(request, DART_CALL_APP_ERROR, message, rsp);
}

int dart_request_start(DartRequest *request){
    i_DartRequest *r = (i_DartRequest*)request;
    if (!request) return DART_ERR_NO_TOPIC;
    if (!r->fn->is_task || r->replied) return DART_ERR_STATE;   /* task only, before the answer */
    if (r->started) return DART_OK;   /* idempotent */
    r->started = 1;
    i_dart_func_send_reply(r->fn, request->caller, r->call_id, DART_CALL_RUNNING,
                           NULL, dart_bytes(NULL, 0));
    return DART_OK;
}

uint64_t dart_request_defer(DartRequest *request){
    i_DartRequest *r = (i_DartRequest*)request;
    i_DartDefer *d;
    int acquired;
    if (!request || r->replied) return 0;
    if (r->fn->is_task && !r->started) (void)dart_request_start(request);   /* implies RUNNING */
    acquired = i_dart_node_sys_lock(r->fn->n);
    d = (i_DartDefer*)i_dart_node_sys_alloc(r->fn->n, NULL, sizeof *d);
    if (d){
        d->fn = r->fn; d->caller = request->caller; d->caller_lo = r->caller_lo;
        d->call_id = r->call_id; d->cancelled = 0;
        d->next = r->fn->defers; r->fn->defers = d;   /* into the live defer registry */
        r->replied = 1;   /* no auto ack, the reply comes via dart_function_complete */
    }
    i_dart_node_sys_unlock(r->fn->n, acquired);
    return (uint64_t)(uintptr_t)d;
}

/* the token as a live defer of fn, unlink_it pops it. NULL = stale. Lock held */
static i_DartDefer *i_dart_func_defer_find(DartFunction *fn, uint64_t token, int unlink_it){
    i_DartDefer **pp = &fn->defers, *d = (i_DartDefer*)(uintptr_t)token;
    for (; *pp; pp = &(*pp)->next)
        if (*pp == d){ if (unlink_it) *pp = d->next; return d; }
    return NULL;
}

int dart_function_complete(DartFunction *fn, uint64_t token, DartCallStatus status,
                           const char *message, DartBytes rsp){
    i_DartDefer *d;
    uint8_t hdr[DART__FN_RSP_HDR_MAX]; size_t hl; DartTopic *rsp_topic; uint32_t caller;
    int acquired;
    if (!fn || !token) return DART_ERR_NO_TOPIC;
    acquired = i_dart_node_sys_lock(fn->n);
    d = i_dart_func_defer_find(fn, token, 1);
    if (!d){   /* a stale token answers nothing and frees nothing */
        i_dart_node_sys_unlock(fn->n, acquired);
        return DART_ERR_STATE;
    }
    hl = i_dart_func_rsp_hdr(hdr, d->call_id, (uint8_t)status, message);
    rsp_topic = fn->rsp; caller = d->caller;
    i_dart_node_sys_alloc(fn->n, d, 0);
    i_dart_node_sys_unlock(fn->n, acquired);
    /* send outside the lock: a completion from an app thread engages backpressure, from a
       callback it commits reentrantly */
    return i_dart_topic_send_to(rsp_topic, caller, dart_bytes(hdr, hl), rsp);
}

int dart_function_progress(DartFunction *fn, uint64_t token, DartBytes progress){
    i_DartDefer *d;
    uint8_t hdr[DART__PRG_PREFIX]; DartTopic *prg;
    int acquired;
    if (!fn) return DART_ERR_NO_TOPIC;
    if (!fn->is_task) return DART_ERR_STATE;
    acquired = i_dart_node_sys_lock(fn->n);
    d = i_dart_func_defer_find(fn, token, 0);
    if (!d){ i_dart_node_sys_unlock(fn->n, acquired); return DART_ERR_STATE; }
    i_dart_le_w32(hdr, d->caller_lo); i_dart_le_w32(hdr + 4, d->call_id);
    prg = fn->prg;
    i_dart_node_sys_unlock(fn->n, acquired);
    /* broadcast outside the lock: reliable subscribers get backpressure end to end, best
       effort observers are fire and forget */
    return i_dart_topic_send_hdr(prg, dart_bytes(hdr, DART__PRG_PREFIX), progress);
}

int dart_function_cancelled(DartFunction *fn, uint64_t token){
    i_DartDefer *d; int acquired, r;
    if (!fn) return DART_ERR_NO_TOPIC;
    if (!fn->is_task) return DART_ERR_STATE;
    acquired = i_dart_node_sys_lock(fn->n);
    d = i_dart_func_defer_find(fn, token, 0);
    r = d ? (d->cancelled ? 1 : 0) : DART_ERR_STATE;
    i_dart_node_sys_unlock(fn->n, acquired);
    return r;
}

int dart_function_on_cancel(DartFunction *def, DartCancelFn on_cancel, void *user){
    int acquired;
    if (!def) return DART_ERR_NO_TOPIC;
    if (!def->is_task || !def->is_provider) return DART_ERR_STATE;
    acquired = i_dart_node_sys_lock(def->n);
    def->on_cancel = on_cancel; def->on_cancel_user = user;
    i_dart_node_sys_unlock(def->n, acquired);
    return DART_OK;
}

int dart_function_cancel(DartFunction *fn, uint32_t call_id){
    i_DartPending *p; int acquired; uint32_t dest;
    uint8_t hdr[DART__FN_PREFIX];
    if (!fn || !fn->req) return DART_ERR_NO_TOPIC;
    acquired = i_dart_node_sys_lock(fn->n);
    p = i_dart_func_find_pending(fn, call_id);
    if (!p){   /* already answered or never made: nothing to cancel */
        i_dart_node_sys_unlock(fn->n, acquired);
        return DART_ERR_STATE;
    }
    if (p->queued_req){
        /* never sent: cancel locally with the one CANCELLED outcome */
        DartResponse r;
        (void)i_dart_func_take_pending(fn, call_id);
        r.status = DART_CALL_CANCELLED; r.data = dart_bytes(NULL, 0);
        r.schema = NULL; r.provider = 0; r.user = p->user; r.written_us = 0;
        r.message = i_dart_call_status_msg(DART_CALL_CANCELLED);
        if (p->on_response) p->on_response(&r);
        i_dart_func_free_pending(fn, p);
        i_dart_node_sys_unlock(fn->n, acquired);
        return DART_OK;
    }
    dest = p->dest;
    {   /* the provider's declared attrs gate cancel locally: no CANCELLABLE bit refuses
           with nothing sent, like a read only variable */
        DartEntityInfo ei;
        int cancellable = i_dart_pat_peer_entity(fn->n, dest, fn->req, DART_ENTITY_TASK, &ei)
                          && ei.cancellable;
        if (!cancellable){
            i_dart_node_sys_unlock(fn->n, acquired);
            return DART_ERR_ROLE;
        }
    }
    i_dart_node_sys_unlock(fn->n, acquired);
    /* never acked: delivery is reliable and the terminal status is the answer. A cancel
       racing the call's own answer lands on nothing at the provider */
    i_dart_le_w32(hdr, call_id); hdr[4] = DART__FN_OP_CANCEL;
    return i_dart_topic_send_to(fn->req, dest, dart_bytes(hdr, DART__FN_PREFIX), dart_bytes(NULL, 0));
}

/* The built in @dart/meta endpoint, hosted at node open as a both sides multi function.
 * The node builds the snapshot, this layer wires it into the function machinery. */

/* the node pool DartAllocFn adapter */
static void *i_dart_pat_alloc(void *user, void *ptr, size_t size){
    return i_dart_node_sys_alloc((DartNode*)user, ptr, size);
}

static void i_dart_meta_on_request(DartRequest *request, void *user){
    DartNode *n = (DartNode*)user;
    i_DartPatterns *pm = (i_DartPatterns*)*i_dart_node_sys_slot(n);
    uint32_t mask = 0, need;
    DartBytes body;
    if (!pm || !pm->meta_rsp_schema){ dart_request_fail(request, "meta schema unavailable", dart_bytes(NULL, 0)); return; }
    if (request->data.len >= 4) mask = i_dart_le_r32(request->data.data);   /* empty = everything */
    body = i_dart_node_snapshot(n, mask);
    if (!body.data){ dart_request_fail(request, "snapshot failed", dart_bytes(NULL, 0)); return; }
    need = dart_schema_msg_min(pm->meta_rsp_schema) + (uint32_t)body.len;
    if (pm->meta_msg_cap < need){
        uint8_t *nb = (uint8_t*)i_dart_node_sys_alloc(n, pm->meta_msg, need);
        if (!nb){ dart_request_fail(request, "out of memory", dart_bytes(NULL, 0)); return; }
        pm->meta_msg = nb; pm->meta_msg_cap = need;
    }
    if (!dart_schema_message_default(pm->meta_rsp_schema, pm->meta_msg, pm->meta_msg_cap)
        || !dart_set_map(pm->meta_msg, pm->meta_msg_cap, pm->meta_rsp_schema, "info", body)){
        dart_request_fail(request, "meta encode failed", dart_bytes(NULL, 0));
        return;
    }
    dart_request_reply(request, dart_bytes(pm->meta_msg,
                dart_schema_msg_len(pm->meta_rsp_schema, pm->meta_msg, pm->meta_msg_cap)));
}

/* the node open seam declared in node/runtime.c: hosts @dart/meta on this node */
void i_dart_patterns_meta_open(DartNode *n){
    i_DartPatterns *pm;
    DartSchema *rsp;
    DartFunctionOpts fo;
    int acquired;
    if (!n) return;
    acquired = i_dart_node_sys_lock(n);
    pm = i_dart_patterns_get(n);
    if (pm && !pm->meta_rsp_schema)
        pm->meta_rsp_schema = dart_schema_compile(i_dart_pat_alloc, n, "DartMeta { info: map }", NULL);
    rsp = pm ? pm->meta_rsp_schema : NULL;
    i_dart_node_sys_unlock(n, acquired);
    if (!pm || pm->meta || !rsp) return;   /* OOM degrades: no endpoint, the node stays healthy */
    memset(&fo, 0, sizeof fo);
    fo.multi = 1;                          /* every node hosting one is the design */
    pm->meta = i_dart_function_new(n, "@dart/meta", NULL /* the raw mask */, rsp,
                                   i_dart_meta_on_request, n, &fo, 2 /* both sides */, NULL, NULL);
}

DartFunction *dart_node_meta_function(DartNode *n){
    i_DartPatterns *pm; DartFunction *meta; int acquired;
    if (!n) return NULL;
    acquired = i_dart_node_sys_lock(n);
    pm = (i_DartPatterns*)*i_dart_node_sys_slot(n);
    meta = pm ? pm->meta : NULL;
    i_dart_node_sys_unlock(n, acquired);
    return meta;
}

/* variables */

#define DART__VAR_PREFIX      5u    /* value channel: [u8 flags][u32 write_seq] */
#define DART__VAR_FLAG_FORCED 0x01u
#define DART__SET_PREFIX      1u    /* set channel: [u8 op] */
#define DART__SET_OP_FORCE    0x01u
#define DART__SET_OP_UNFORCE  0x02u

struct DartVariable {
    DartNode       *n;
    i_DartPatterns *pm;
    struct DartVariable *next;   /* the manager list */
    DartTopic      *value;   /* owner PUB_ONLY, accessor SUB_ONLY */
    DartTopic      *set;     /* owner SUB_ONLY, accessor PUB_ONLY, NULL = a read only owner */
    uint8_t         is_owner, allow_force, readonly, forced, has_value;
    uint32_t        write_seq;
    uint8_t        *store;  uint32_t store_len,  store_cap;    /* the value or the cache */
    uint8_t        *shadow; uint32_t shadow_len, shadow_cap;   /* owner: the source while forced */
    DartVariableUpdateFn on_change; void *on_change_user;   /* fires only on a state change */
    DartVariableUpdateFn on_write;  void *on_write_user;    /* fires on every applied write */
    const DartSchema *cur_schema;   /* what store decodes with */
    uint32_t        last_source;    /* the peer behind the current state, 0 = a local call */
    uint64_t        last_write_us;  /* when the current state applied, node clock */
    uint64_t        last_written_us;   /* the writer's wall clock for that write, 0 = unstamped */
    uint32_t       *dup_peers;      /* owner: peers already reported for duplicate authority */
    uint16_t        dup_n, dup_cap;
    uint8_t         reflect;        /* created with reflect_from_mesh */
    uint64_t        generation;     /* the mesh generation the schema was taken at */
};

/* copies v into a grown buffer, 0 on OOM with the buffer unchanged */
static int i_dart_buf_put(DartNode *n, uint8_t **buf, uint32_t *len, uint32_t *cap, DartBytes v){
    if (*cap < v.len){
        uint8_t *nb = (uint8_t*)i_dart_node_sys_alloc(n, *buf, v.len ? v.len : 1u);
        if (!nb) return 0;
        *buf = nb; *cap = (uint32_t)(v.len ? v.len : 1u);
    }
    if (v.len) memcpy(*buf, v.data, v.len);
    *len = (uint32_t)v.len;
    return 1;
}

static void i_dart_var_hdr(const DartVariable *v, uint8_t hdr[DART__VAR_PREFIX]){
    hdr[0] = (uint8_t)(v->forced ? DART__VAR_FLAG_FORCED : 0);
    i_dart_le_w32(hdr + 1, v->write_seq);
}

/* under the lock, before the store is overwritten: would applying (val, forced_now) change
 * the observed state? The first value, different bytes, or a forced flag flip. */
static int i_dart_var_would_change(const DartVariable *v, DartBytes val, int forced_now){
    if (!v->has_value) return 1;
    if ((v->forced != 0) != (forced_now != 0)) return 1;
    if (v->store_len != val.len) return 1;
    return val.len ? memcmp(v->store, val.data, val.len) != 0 : 0;
}

/* the current state as a DartVariableUpdate, views into manager memory */
static void i_dart_var_update_view(DartVariable *v, DartVariableUpdate *u){
    u->variable   = v;
    u->name       = i_dart_topic_name(v->value);
    u->value      = dart_bytes(v->store, v->store_len);
    u->schema     = v->cur_schema;
    u->forced     = v->forced;
    u->write_seq  = v->write_seq;
    u->source     = v->last_source;
    u->recv_us    = v->last_write_us;
    u->written_us = v->last_written_us;
}

/* A write just applied, lock held: stamp the write context, then fire on_write always and
 * on_change when the state changed, inline on this thread. */
static void i_dart_var_notify(DartVariable *v, int changed, uint32_t source, uint64_t when_us,
                              uint64_t written_us){
    DartVariableUpdate u;
    v->last_source = source; v->last_write_us = when_us; v->last_written_us = written_us;
    if (!v->on_write && !(changed && v->on_change)) return;
    i_dart_var_update_view(v, &u);
    if (v->on_write)             v->on_write(&u, v->on_write_user);
    if (changed && v->on_change) v->on_change(&u, v->on_change_user);
}

/* owner: publishes the store under a held lock, a reentrant send with no wait */
static void i_dart_var_publish_locked(DartVariable *v){
    uint8_t hdr[DART__VAR_PREFIX];
    i_dart_var_hdr(v, hdr);
    (void)i_dart_topic_send_hdr(v->value, dart_bytes(hdr, DART__VAR_PREFIX),
                                dart_bytes(v->store, v->store_len));
}

/* owner: a dumb write from a held lock context. Absorbed into the shadow while forced,
 * with no event, else stored, published and notified. */
static void i_dart_var_owner_apply(DartVariable *v, DartBytes val, uint32_t source,
                                   uint64_t when_us, uint64_t written_us){
    int changed;
    if (v->forced){ i_dart_buf_put(v->n, &v->shadow, &v->shadow_len, &v->shadow_cap, val); return; }
    changed = i_dart_var_would_change(v, val, 0);
    if (!i_dart_buf_put(v->n, &v->store, &v->store_len, &v->store_cap, val)) return;
    v->has_value = 1; v->write_seq++;
    i_dart_var_publish_locked(v);
    i_dart_var_notify(v, changed, source, when_us, written_us);
}
/* owner: force to val. Without allow_force this is a silent no op, force is opaque on the
 * wire and the local API refuses loudly before reaching here. */
static void i_dart_var_owner_force(DartVariable *v, DartBytes val, uint32_t source,
                                   uint64_t when_us, uint64_t written_us){
    int changed;
    if (!v->allow_force) return;
    changed = i_dart_var_would_change(v, val, 1);
    if (!v->forced)   /* entering force: save the current source into the shadow */
        i_dart_buf_put(v->n, &v->shadow, &v->shadow_len, &v->shadow_cap, dart_bytes(v->store, v->store_len));
    if (!i_dart_buf_put(v->n, &v->store, &v->store_len, &v->store_cap, val)) return;
    v->forced = 1; v->has_value = 1; v->write_seq++;
    i_dart_var_publish_locked(v);
    i_dart_var_notify(v, changed, source, when_us, written_us);
}
static void i_dart_var_owner_unforce(DartVariable *v, uint32_t source, uint64_t when_us,
                                     uint64_t written_us){
    if (!v->forced) return;
    v->forced = 0;
    i_dart_buf_put(v->n, &v->store, &v->store_len, &v->store_cap, dart_bytes(v->shadow, v->shadow_len));
    v->write_seq++;
    i_dart_var_publish_locked(v);
    i_dart_var_notify(v, 1, source, when_us, written_us);   /* a flag flip is always a change */
}

/* owner: a set, force or unforce op arrived, lock held. A plain set must carry a value,
 * an empty one is ignored. */
static void i_dart_var_on_set(void *user, const DartMsg *msg){
    DartVariable *v = (DartVariable*)user;
    uint8_t op = msg->header.len >= 1 ? msg->header.data[0] : 0;
    if (op & DART__SET_OP_UNFORCE)    i_dart_var_owner_unforce(v, msg->publisher_id, msg->recv_us, msg->written_us);
    else if (op & DART__SET_OP_FORCE) { if (msg->data.len) i_dart_var_owner_force(v, msg->data, msg->publisher_id, msg->recv_us, msg->written_us); }
    else                              { if (msg->data.len) i_dart_var_owner_apply(v, msg->data, msg->publisher_id, msg->recv_us, msg->written_us); }
}

/* accessor: a new value arrived. Cache it with its forced flag and write_seq, then notify
 * with the owner as the source. */
static void i_dart_var_on_value(void *user, const DartMsg *msg){
    DartVariable *v = (DartVariable*)user;
    int forced_in, changed; uint32_t seq_in;
    if (msg->header.len < DART__VAR_PREFIX) return;
    /* the stale order guard: a same owner value behind the cached seq is dropped (signed
       distance, wrap safe). A different publisher always applies. See spec/patterns.md */
    seq_in = i_dart_le_r32(msg->header.data + 1);
    if (v->has_value && msg->publisher_id == v->last_source
        && (int32_t)(seq_in - v->write_seq) < 0) return;
    forced_in = (msg->header.data[0] & DART__VAR_FLAG_FORCED) ? 1 : 0;
    changed = i_dart_var_would_change(v, msg->data, forced_in);
    if (i_dart_buf_put(v->n, &v->store, &v->store_len, &v->store_cap, msg->data)){
        v->has_value = 1;
        v->forced = (uint8_t)forced_in;
        v->write_seq = i_dart_le_r32(msg->header.data + 1);
        v->cur_schema = msg->schema;
        i_dart_var_notify(v, changed, msg->publisher_id, msg->recv_us, msg->written_us);
    }
}

static DartVariable *i_dart_variable_new(DartNode *n, const char *name, const DartSchema *schema,
                                         const DartVariableOpts *opts, int owner){
    i_DartPatterns *pm; DartVariable *v;
    char sn[DART_TOPIC_NAME_MAX + 1]; size_t nl;
    DartTopicOpts vopt, sopt; int acquired, readonly, make_set;
    if (!n || !name) return NULL;
    nl = strlen(name);
    if (nl == 0 || nl + 4 > DART_TOPIC_NAME_MAX) return NULL;   /* room for "@set" */
    readonly = (opts && opts->access == DART_VAR_READONLY);
    make_set = owner ? !readonly : 1;   /* a read only owner has no set channel */

    memset(&vopt, 0, sizeof vopt);
    vopt.qos.reliability = DART_RELIABLE;
    vopt.qos.catch_up = (opts && opts->catch_up) ? opts->catch_up : 1u;   /* for late accessors */
    /* keep_last is the repair window, not the replay window. Tying it to catch_up once left
       a reliable variable one slot deep. 0 = the reliable default (10). See spec/patterns.md */
    vopt.qos.keep_last = (opts && opts->keep_last) ? opts->keep_last : 0u;
    if (vopt.qos.keep_last && vopt.qos.keep_last < vopt.qos.catch_up)
        vopt.qos.keep_last = vopt.qos.catch_up;      /* the ring must hold what it replays */
    vopt.qos.backpressure_wait_us = (opts && opts->backpressure_wait_us) ? opts->backpressure_wait_us
                                                                         : DART_PATTERN_BP_WAIT_US;
    sopt = vopt; sopt.qos.catch_up = 0;   /* the set channel: no replay, the same repair depth */

    v = (DartVariable*)i_dart_pat_handle_new(n, sizeof *v, &pm);
    if (!v) return NULL;
    if (opts && opts->reflect_from_mesh){
        const DartSchema *ms;
        v->reflect = 1;
        i_dart_node_reflect_pick(n, DART_ENTITY_VARIABLE, name, 0, owner, &ms, NULL, &v->generation);
        if (!schema) schema = ms;
    }
    v->n = n; v->pm = pm; v->is_owner = (uint8_t)owner;
    v->allow_force = (uint8_t)(opts && opts->allow_force);
    v->readonly = (uint8_t)readonly;
    v->cur_schema = schema;
    v->value = i_dart_node_create_pattern_topic(n, name, owner ? DART_PUB_ONLY : DART_SUB_ONLY,
                              schema, &vopt, DART_KIND_VARIABLE, DART__VAR_PREFIX, 0,
                              (uint8_t)((owner && v->allow_force) ? DART_ATTR_FORCEABLE : 0u),
                              owner ? NULL : i_dart_var_on_value, v);
    if (!v->value) return NULL;   /* v stays pool allocated: nothing routes into it yet */
    if (owner)
        /* seed write_seq from the slot's continuing seqno line, so a successor definition's
           first write orders above its predecessor's last. See spec/patterns.md */
        v->write_seq = (uint32_t)i_dart_topic_seqno(v->value);
    if (make_set){
        memcpy(sn, name, nl); memcpy(sn + nl, "@set", 5);
        v->set = i_dart_node_create_pattern_topic(n, sn, owner ? DART_SUB_ONLY : DART_PUB_ONLY,
                              schema, &sopt, DART_KIND_VAR_SET, DART__SET_PREFIX, 0, 0,
                              owner ? i_dart_var_on_set : NULL, v);
        if (!v->set)   /* the value topic may route to v: keep the handle */
            return (DartVariable*)i_dart_pat_half_create_fail(v->value);
    }
    acquired = i_dart_node_sys_lock(n);       /* publish into the manager list */
    v->next = pm->vars; pm->vars = v; pm->auth_dirty = 1;
    if (owner)      /* a rival owner may already be on the network */
        i_dart_pat_dup_sweep(n, v->value, DART_ENTITY_VARIABLE,
                             &v->dup_peers, &v->dup_n, &v->dup_cap);
    i_dart_node_sys_unlock(n, acquired);
    if (owner && opts && opts->initial.len)   /* seed the store and history */
        dart_variable_set(v, opts->initial);
    return v;
}

DartVariable *dart_node_create_variable_definition(DartNode *n, const char *name,
                              const DartSchema *schema, const DartVariableOpts *opts){
    if (i_dart_pat_reserved(name)) return NULL;
    return i_dart_variable_new(n, name, schema, opts, 1);
}
DartVariable *dart_node_create_remote_variable(DartNode *n, const char *name,
                              const DartSchema *schema, const DartVariableOpts *opts){
    if (i_dart_pat_reserved(name)) return NULL;
    return i_dart_variable_new(n, name, schema, opts, 0);
}

int dart_variable_get(DartVariable *var, DartBytes *out){
    int acquired, has;
    if (!var) return 0;
    acquired = i_dart_node_sys_lock(var->n);
    has = var->has_value;
    if (out){ out->data = var->store; out->len = has ? var->store_len : 0; }
    i_dart_node_sys_unlock(var->n, acquired);
    return has;   /* out views manager memory */
}

/* accessor write routing: no owner at all versus a read only owner */
static int i_dart_var_accessor_route(DartVariable *var){
    if (i_dart_topic_source_match_count(var->value) == 0) return DART_ERR_NO_TOPIC;
    if (!var->set || dart_topic_match_count(var->set) == 0) return DART_ERR_ROLE;
    return DART_OK;
}

/* The routed form every accessor write goes through: while the owner match is still
 * forming, wait on the set channel like a first send does, then re derive the verdict. */
static int i_dart_var_accessor_route_wait(DartVariable *var){
    int r = i_dart_var_accessor_route(var);
    if (r == DART_OK || !var->set) return r;
    (void)i_dart_topic_match_wait(var->set);
    return i_dart_var_accessor_route(var);
}

int dart_variable_set(DartVariable *var, DartBytes value){
    int acquired, r = DART_OK, publish = 0;
    uint32_t my_seq = 0;
    uint8_t hdr[DART__VAR_PREFIX];
    if (!var) return DART_ERR_NO_TOPIC;
    if (var->is_owner){
        /* mutate the store under the lock, publish outside it from the caller's buffer so
           the send engages backpressure */
        acquired = i_dart_node_sys_lock(var->n);
        if (var->forced){
            i_dart_buf_put(var->n, &var->shadow, &var->shadow_len, &var->shadow_cap, value);
        } else {
            int changed = i_dart_var_would_change(var, value, 0);
            if (!i_dart_buf_put(var->n, &var->store, &var->store_len, &var->store_cap, value)){
                r = DART_ERR_OOM;
            } else {
                var->has_value = 1; var->write_seq++;
                my_seq = var->write_seq;
                i_dart_var_hdr(var, hdr);
                publish = 1;
                i_dart_var_notify(var, changed, 0, i_dart_node_now_us(var->n),
                                  i_dart_node_wall_us(var->n));   /* a local write: our clock */
                /* a reentrant set inside the callback committed first, so sending ours now
                   would put stale bytes newest in history. Skip, the write itself stands */
                if (var->write_seq != my_seq) publish = 0;
            }
        }
        i_dart_node_sys_unlock(var->n, acquired);
        return publish ? i_dart_topic_send_hdr(var->value, dart_bytes(hdr, DART__VAR_PREFIX), value) : r;
    }
    r = i_dart_var_accessor_route_wait(var);
    if (r != DART_OK) return r;
    {   uint8_t op = 0;
        return i_dart_topic_send_hdr(var->set, dart_bytes(&op, 1), value);
    }
}

int dart_variable_force(DartVariable *var, DartBytes value){
    int acquired, r = DART_OK;
    if (!var) return DART_ERR_NO_TOPIC;
    if (var->is_owner){
        if (!var->allow_force) return DART_ERR_STATE;   /* locally checkable: refuse loudly */
        acquired = i_dart_node_sys_lock(var->n);
        i_dart_var_owner_force(var, value, 0, i_dart_node_now_us(var->n),
                               i_dart_node_wall_us(var->n));   /* a rare debug op, reentrant */
        i_dart_node_sys_unlock(var->n, acquired);
        return DART_OK;
    }
    r = i_dart_var_accessor_route_wait(var);
    if (r != DART_OK) return r;
    {   uint8_t op = DART__SET_OP_FORCE;
        return i_dart_topic_send_hdr(var->set, dart_bytes(&op, 1), value);
    }
}

int dart_variable_unforce(DartVariable *var){
    int acquired, r = DART_OK;
    if (!var) return DART_ERR_NO_TOPIC;
    if (var->is_owner){
        if (!var->allow_force) return DART_ERR_STATE;
        acquired = i_dart_node_sys_lock(var->n);
        i_dart_var_owner_unforce(var, 0, i_dart_node_now_us(var->n), i_dart_node_wall_us(var->n));
        i_dart_node_sys_unlock(var->n, acquired);
        return DART_OK;
    }
    r = i_dart_var_accessor_route_wait(var);
    if (r != DART_OK) return r;
    {   uint8_t op = DART__SET_OP_UNFORCE;
        return i_dart_topic_send_hdr(var->set, dart_bytes(&op, 1), dart_bytes(NULL,0));
    }
}

int dart_variable_forced(DartVariable *var){ return var ? var->forced : 0; }

int dart_variable_retire(DartVariable *var){
    i_DartPatterns *pm; DartNode *n; int acquired, r;
    DartTopic *value, *set;
    if (!var) return DART_ERR_NO_TOPIC;
    pm = var->pm; n = var->n;
    r = i_dart_pat_park_channels(var->value, var->set);   /* set is NULL on a read only owner */
    if (r != 0) return r;
    acquired = i_dart_node_sys_lock(n);
    i_dart_pat_clear_channels(var->value, var->set);
    if (pm){ i_dart_pat_unlink((void**)&pm->vars, var, offsetof(DartVariable, next)); pm->auth_dirty = 1; }
    value = var->value; set = var->set;   /* outlive var, see phase 3 */
    if (var->store)     i_dart_node_sys_alloc(n, var->store, 0);
    if (var->shadow)    i_dart_node_sys_alloc(n, var->shadow, 0);
    if (var->dup_peers) i_dart_node_sys_alloc(n, var->dup_peers, 0);
    i_dart_node_sys_alloc(n, var, 0);
    i_dart_node_sys_unlock(n, acquired);
    i_dart_pat_release_channels(value, set);
    return DART_OK;
}

int dart_variable_refresh(DartVariable *var){
    DartNode *n; uint64_t gen = 0; int r;
    const DartSchema *ms = NULL;
    if (!var) return DART_ERR_NO_TOPIC;
    if (!var->reflect) return DART_ERR_ROLE;
    n = var->n;
    if (!i_dart_node_reflect_pick(n, DART_ENTITY_VARIABLE, i_dart_pat_base_name(var->value), 0,
                                  var->is_owner, &ms, NULL, &gen)
        || gen == var->generation) return 0;
    r = i_dart_topic_retype(var->value, ms, DART_RELIABLE);
    if (r != 0) return r;
    if (var->set){
        r = i_dart_topic_retype(var->set, ms, DART_RELIABLE);
        if (r != 0) return r;
    }
    var->generation = gen;
    return 1;
}

int dart_variable_on_change(DartVariable *var, DartVariableUpdateFn on_change, void *user){
    int acquired;
    if (!var) return DART_ERR_NO_TOPIC;
    acquired = i_dart_node_sys_lock(var->n);
    var->on_change = on_change; var->on_change_user = user;
    if (on_change && var->has_value){
        /* replay the current state once, right here, so a value that arrived between
           create and register is never missed */
        DartVariableUpdate u;
        i_dart_var_update_view(var, &u);
        on_change(&u, user);
    }
    i_dart_node_sys_unlock(var->n, acquired);
    return DART_OK;
}

int dart_variable_on_write(DartVariable *var, DartVariableUpdateFn on_write, void *user){
    int acquired;
    if (!var) return DART_ERR_NO_TOPIC;
    acquired = i_dart_node_sys_lock(var->n);
    var->on_write = on_write; var->on_write_user = user;
    i_dart_node_sys_unlock(var->n, acquired);
    return DART_OK;   /* writes are events, not state: no replay */
}

int dart_variable_wait(DartVariable *var, int timeout_ms){
    int acquired; uint64_t deadline;
    if (!var) return 0;
    acquired = i_dart_node_sys_lock(var->n);
    if (!acquired || dart_node_is_started(var->n)){ i_dart_node_sys_unlock(var->n, acquired); return var->has_value; }
    if (var->has_value){ i_dart_node_sys_unlock(var->n, acquired); return 1; }
    deadline = i_dart_node_now_us(var->n) + (uint64_t)(timeout_ms >= 0 ? (uint64_t)timeout_ms*1000u : 3600000000ull);
    i_dart_node_sys_unlock(var->n, acquired);
    while (!var->has_value){
        if (i_dart_node_now_us(var->n) >= deadline) break;
        i_dart_node_sys_poll(var->n, 5);
    }
    return var->has_value;
}

int dart_variable_match_count(DartVariable *var){
    if (!var) return 0;
    return dart_topic_match_count(var->is_owner ? var->value : var->set);
}

/* Duplicate authority detection. Two authorities never match each other, so the announce
   interest is checked instead. The rules are in spec/patterns.md. */

static int i_dart_pat_dup_reported(const uint32_t *ids, uint16_t n_ids, uint32_t peer){
    uint16_t i;
    for (i = 0; i < n_ids; i++) if (ids[i] == peer) return 1;
    return 0;
}

/* remembers peer in the entity's reported list. On OOM it may report again later */
static void i_dart_pat_dup_remember(DartNode *n, uint32_t **ids, uint16_t *n_ids,
                                    uint16_t *cap, uint32_t peer){
    if (*n_ids == *cap){
        uint16_t grown = *cap ? (uint16_t)(*cap * 2u) : 4u;
        uint32_t *nb = (uint32_t*)i_dart_node_sys_alloc(n, *ids, grown * sizeof **ids);
        if (!nb) return;
        *ids = nb; *cap = grown;
    }
    (*ids)[(*n_ids)++] = peer;
}

/* forgets peer in one entity's reported list, so a genuine return re reports */
static void i_dart_pat_dup_drop(uint32_t *ids, uint16_t *n_ids, uint32_t peer){
    uint16_t i;
    for (i = 0; i < *n_ids; i++)
        if (ids[i] == peer){ ids[i] = ids[--*n_ids]; return; }
}

/* checks one authority side entity against one active peer, reporting a fresh claim once */
static void i_dart_pat_dup_check(DartNode *n, uint32_t peer, DartTopic *primary, DartEntityKind kind,
                                 uint32_t **ids, uint16_t *n_ids, uint16_t *cap){
    DartEntityInfo ei;
    if (!primary) return;
    if (i_dart_pat_dup_reported(*ids, *n_ids, peer)) return;
    if (!i_dart_pat_peer_entity(n, peer, primary, kind, &ei) || !ei.provides) return;
    i_dart_pat_dup_remember(n, ids, n_ids, cap, peer);
    i_dart_node_sys_error(n, DART_E_DUPLICATE_AUTHORITY, primary, peer);
}

static DartEntityKind i_dart_pat_fn_entity_kind(const DartFunction *fn){
    return fn->is_task ? DART_ENTITY_TASK : DART_ENTITY_FUNCTION;
}

/* (kind, hash) order for the authority index */
static int i_dart_pat_auth_before(const i_DartPatAuth *a, const i_DartPatAuth *b){
    return a->kind != b->kind ? a->kind < b->kind : a->hash < b->hash;
}

static void i_dart_pat_auth_put(i_DartPatterns *pm, uint32_t hash, DartEntityKind kind,
                                DartTopic *primary, uint32_t **ids, uint16_t *n_ids, uint16_t *cap){
    i_DartPatAuth *a = &pm->auth[pm->auth_n++];
    a->hash = hash; a->kind = (uint8_t)kind; a->primary = primary;
    a->ids = ids; a->n_ids = n_ids; a->cap = cap;
}

static uint32_t i_dart_pat_auth_hash(DartTopic *primary){
    DartString nm = i_dart_topic_name(primary);
    char buf[DART_TOPIC_NAME_MAX + 1];
    memcpy(buf, nm.data, nm.len); buf[nm.len] = '\0';
    return (uint32_t)dart_topic_id(buf);           /* what the announce carries for it */
}

/* rebuilds the sorted authority index from the entity lists, a shell sort. 0 on OOM */
static int i_dart_pat_auth_rebuild(i_DartPatterns *pm){
    DartFunction *fn; DartVariable *v; uint16_t want = 0, gap, i, j;
    for (fn = pm->funcs; fn; fn = fn->next) if ((fn->is_provider || fn->both) && !fn->multi) want++;
    for (v = pm->vars; v; v = v->next) if (v->is_owner) want++;
    if (want > pm->auth_cap){
        i_DartPatAuth *nb = (i_DartPatAuth*)i_dart_node_sys_alloc(pm->n, pm->auth,
                                                                  (size_t)want * sizeof *nb);
        if (!nb) return 0;
        pm->auth = nb; pm->auth_cap = want;
    }
    pm->auth_n = 0;
    for (fn = pm->funcs; fn; fn = fn->next)
        if ((fn->is_provider || fn->both) && !fn->multi)   /* multi: rivals are the design */
            i_dart_pat_auth_put(pm, i_dart_pat_auth_hash(fn->req), i_dart_pat_fn_entity_kind(fn),
                                fn->req, &fn->dup_peers, &fn->dup_n, &fn->dup_cap);
    for (v = pm->vars; v; v = v->next)
        if (v->is_owner)
            i_dart_pat_auth_put(pm, i_dart_pat_auth_hash(v->value), DART_ENTITY_VARIABLE,
                                v->value, &v->dup_peers, &v->dup_n, &v->dup_cap);
    for (gap = pm->auth_n / 2u; gap > 0; gap /= 2u)
        for (i = gap; i < pm->auth_n; i++){
            i_DartPatAuth t = pm->auth[i];
            for (j = i; j >= gap && i_dart_pat_auth_before(&t, &pm->auth[j - gap]); j -= gap)
                pm->auth[j] = pm->auth[j - gap];
            pm->auth[j] = t;
        }
    pm->auth_dirty = 0;
    return 1;
}

/* the first index whose (kind, hash) is not below the key */
static uint16_t i_dart_pat_auth_lower(const i_DartPatterns *pm, DartEntityKind kind, uint32_t hash){
    uint16_t lo = 0, hi = pm->auth_n;
    i_DartPatAuth key; key.kind = (uint8_t)kind; key.hash = hash;
    while (lo < hi){
        uint16_t mid = (uint16_t)(lo + (hi - lo) / 2u);
        if (i_dart_pat_auth_before(&pm->auth[mid], &key)) lo = (uint16_t)(mid + 1u); else hi = mid;
    }
    return lo;
}

/* Every authority against one peer whose interest was just applied: one walk of the
 * peer's entities with a binary search each. On OOM the index is skipped this time. */
static void i_dart_pat_dup_check_peer(i_DartPatterns *pm, uint32_t peer){
    DartIter it; DartEntityInfo ei;
    if (pm->auth_dirty && !i_dart_pat_auth_rebuild(pm)) return;
    if (!pm->auth_n) return;
    memset(&it, 0, sizeof it);
    while (dart_node_entities_next(pm->n, peer, &it, &ei)){
        uint16_t k;
        if (!ei.provides || ei.kind == DART_ENTITY_TOPIC) continue;
        for (k = i_dart_pat_auth_lower(pm, ei.kind, ei.hash);
             k < pm->auth_n && pm->auth[k].kind == (uint8_t)ei.kind && pm->auth[k].hash == ei.hash; k++){
            i_DartPatAuth *a = &pm->auth[k];
            if (ei.name.len){
                DartString nm = i_dart_topic_name(a->primary); size_t len = nm.len;
                if (len >= 5 && nm.data[len - 4] == '@') len -= 4;   /* the entity's base name */
                if (ei.name.len != len || memcmp(ei.name.data, nm.data, len) != 0) continue;
            }
            if (i_dart_pat_dup_reported(*a->ids, *a->n_ids, peer)) continue;
            i_dart_pat_dup_remember(pm->n, a->ids, a->n_ids, a->cap, peer);
            i_dart_node_sys_error(pm->n, DART_E_DUPLICATE_AUTHORITY, a->primary, peer);
        }
    }
}

/* a just created authority against every active peer */
static void i_dart_pat_dup_sweep(DartNode *n, DartTopic *primary, DartEntityKind kind,
                                 uint32_t **ids, uint16_t *n_ids, uint16_t *cap){
    DartIter it; DartPeerInfo p;
    memset(&it, 0, sizeof it);
    while (dart_node_peers_next(n, &it, &p))
        if (p.liveness == DART_PEER_ACTIVE)
            i_dart_pat_dup_check(n, p.id, primary, kind, ids, n_ids, cap);
}

static void i_dart_pat_dup_forget_peer(i_DartPatterns *pm, uint32_t peer){
    DartFunction *fn; DartVariable *v;
    for (fn = pm->funcs; fn; fn = fn->next) i_dart_pat_dup_drop(fn->dup_peers, &fn->dup_n, peer);
    for (v = pm->vars; v; v = v->next)      i_dart_pat_dup_drop(v->dup_peers, &v->dup_n, peer);
}

/* the manager hooks: call timeouts on the tick, provider loss on the event */

/* Fails pending calls with one synthesized outcome each: those directed at dest, or all,
 * or those past now. Callbacks run after the unlink. Returns the earliest deadline left. */
static uint64_t i_dart_func_reap(DartFunction *fn, uint64_t now, DartCallStatus fail_status,
                                 int all, uint32_t dest){
    i_DartPending **pp = &fn->pending, *p;
    uint64_t soonest = 0;
    while ((p = *pp) != NULL){
        if (dest ? (p->dest == dest) : (all || now >= p->deadline_us)){
            *pp = p->next;
            {   DartResponse r; r.status = fail_status; r.data = dart_bytes(NULL,0);
                r.schema = NULL; r.provider = 0; r.user = p->user; r.written_us = 0;
                r.message = i_dart_call_status_msg(fail_status);
                if (p->on_response) p->on_response(&r);
            }
            i_dart_func_free_pending(fn, p);
            continue;
        }
        if (!soonest || p->deadline_us < soonest) soonest = p->deadline_us;
        pp = &p->next;
    }
    return soonest;
}

/* Fails every sent call directed at peer whose lane no longer exists with CANCELLED: the
 * wire CANCELLED can be lost to the very announce that severed it. See spec/patterns.md. */
static void i_dart_func_reap_severed(DartFunction *fn, uint32_t peer){
    i_DartPending **pp = &fn->pending, *p;
    while ((p = *pp) != NULL){
        if (p->dest == peer && !p->queued_req && !i_dart_topic_peer_matched(fn->req, peer)){
            *pp = p->next;
            {   DartResponse r; r.status = DART_CALL_CANCELLED; r.data = dart_bytes(NULL,0);
                r.schema = NULL; r.provider = 0; r.user = p->user; r.written_us = 0;
                r.message = dart_cstr("provider retired");
                if (p->on_response) p->on_response(&r);
            }
            i_dart_func_free_pending(fn, p);
            continue;
        }
        pp = &p->next;
    }
}

static uint64_t i_dart_patterns_tick(void *user, uint64_t now_us){
    i_DartPatterns *pm = (i_DartPatterns*)user;
    DartFunction *fn; uint64_t soonest = 0;
    for (fn = pm->funcs; fn; fn = fn->next){
        uint64_t s;
        i_dart_func_flush_queued(fn);   /* the backstop: the interest event is the fast path */
        s = i_dart_func_reap(fn, now_us, DART_CALL_TIMEOUT, 0, 0);
        if (s && (!soonest || s < soonest)) soonest = s;
    }
    return soonest;   /* the next timeout deadline for the poll wait cap */
}

/* The node is closing: every pending call gets CANCELLED and every live deferred call
 * answers CANCELLED while the channels are still up. Runs once, node still fully alive. */
static void i_dart_patterns_on_close(void *user){
    i_DartPatterns *pm = (i_DartPatterns*)user;
    DartFunction *fn;
    for (fn = pm->funcs; fn; fn = fn->next){
        if (fn->defers){
            int acquired = i_dart_node_sys_lock(pm->n);
            i_dart_func_drain_defers(fn, "node closing");
            i_dart_node_sys_unlock(pm->n, acquired);
        }
        if (!fn->is_provider && fn->pending)
            i_dart_func_reap(fn, 0, DART_CALL_CANCELLED, 1, 0);
    }
    /* the drained CANCELLED replies must leave before the socket closes: no poll pass
       follows this hook */
    i_dart_node_flush_tx(pm->n);
}

static void i_dart_patterns_on_event(void *user, const DartEvent *ev){
    i_DartPatterns *pm = (i_DartPatterns*)user;
    DartFunction *fn;
    if (ev->kind == DART_PEER_INTEREST){
        DartVariable *v;
        /* a match may just have formed: flush calls queued while no provider was matched */
        for (fn = pm->funcs; fn; fn = fn->next)
            if (!fn->is_provider && fn->pending) i_dart_func_flush_queued(fn);
        /* and one may just have been severed: a directed call there can never resolve */
        for (fn = pm->funcs; fn; fn = fn->next)
            if (!fn->is_provider && fn->pending) i_dart_func_reap_severed(fn, ev->peer);
        /* an accessor whose owner just parked or left: disarm the stale order guard, since
           a successor on the same node keeps the peer id with write_seq restarting */
        for (v = pm->vars; v; v = v->next)
            if (!v->is_owner && v->last_source
                && i_dart_topic_source_match_count(v->value) == 0) v->last_source = 0;
        i_dart_pat_dup_check_peer(pm, ev->peer);   /* a rival authority may have appeared */
        return;
    }
    if (ev->kind != DART_PEER_DOWN) return;
    i_dart_pat_dup_forget_peer(pm, ev->peer);
    /* a provider dropped: a caller with no live provider left fails its calls now with
       PEER_LOST, and a call directed at the dropped peer fails regardless */
    for (fn = pm->funcs; fn; fn = fn->next){
        if (fn->is_provider || !fn->pending) continue;
        (void)i_dart_func_reap(fn, 0, DART_CALL_PEER_LOST, 0, ev->peer);   /* directed at it */
        if (fn->pending && i_dart_topic_live_match_count(fn->req) == 0)
            i_dart_func_reap(fn, 0, DART_CALL_PEER_LOST, 1, 0);
    }
}
#pragma endregion
#endif /* !DART_NO_PATTERNS */
#endif /* !DART_TRANSPORT_SANS_IO */
#endif /* DART_TRANSPORT_IMPLEMENTATION */
