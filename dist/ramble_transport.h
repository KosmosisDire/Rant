/* GENERATED single-header build. DO NOT EDIT.
 * Ramble, the networking layer of RANT. Amalgamated from src/ by the CMake
 * build (tools/pack.cmake). Edit the split sources in src/ and rebuild (or run
 * tools/pack.cmake) to regenerate. See the flag scheme in tools/pack.cmake.
 */
#if defined(__GNUC__)   /* let the section markers below fold quietly */
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif
#if defined(RAMBLE_TRANSPORT_IMPLEMENTATION) && !defined(RAMBLE_TRANSPORT_SANS_IO) && !defined(_WIN32)
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

#ifndef RAMBLE_TRANSPORT_SANS_IO
  #if defined(RAMBLE_TRANSPORT_IMPLEMENTATION) && !defined(RAMBLE_DISCOVERY_IMPLEMENTATION)
  #define RAMBLE_DISCOVERY_IMPLEMENTATION
  #endif
  #include "ramble_discovery.h"   /* discovery: needed by the node runtime */
#endif

#pragma region common/api.h
/* The linkage of every public entry point. spec/build.md explains the three builds. */
#ifndef RAMBLE_API_H
#define RAMBLE_API_H

/* Default: the single header build, where the caller compiles Ramble into its own binary
 * and needs no decoration. RAMBLE_BUILD_SHARED builds the shared library, RAMBLE_LINK_SHARED
 * consumes one. */
#if defined(RAMBLE_BUILD_SHARED)
  #if defined(_WIN32)
    #define RAMBLE_API __declspec(dllexport)
  #else
    #define RAMBLE_API __attribute__((visibility("default")))
  #endif
#elif defined(RAMBLE_LINK_SHARED) && defined(_WIN32)
  #define RAMBLE_API __declspec(dllimport)
#else
  #define RAMBLE_API
#endif

#endif /* RAMBLE_API_H */
#pragma endregion
#pragma region common/string.h
/* The two view types. Neither owns, copies or implies a NUL terminator. */
#ifndef RAMBLE_STRING_H
#define RAMBLE_STRING_H

#include <stddef.h>
#include <stdint.h>

/* A read only run of bytes. */
typedef struct {
    const uint8_t *data;
    size_t         len;
} RambleBytes;

/* A run of string bytes with no NUL terminator. Never use %s or strlen on it. */
typedef struct {
    const char *data;
    size_t      len;
} RambleString;

static inline RambleBytes ramble_bytes(const void *data, size_t len){
    RambleBytes b; b.data = (const uint8_t *)data; b.len = len; return b;
}
static inline RambleString ramble_string(const char *data, size_t len){
    RambleString s; s.data = data; s.len = len; return s;
}
/* From a C string. NULL gives an empty string. */
static inline RambleString ramble_cstr(const char *s){
    RambleString r; size_t n = 0;
    if (s) while (s[n]) n++;
    r.data = s; r.len = n; return r;
}
static inline int ramble_string_eq(RambleString a, RambleString b){
    size_t i;
    if (a.len != b.len) return 0;
    for (i = 0; i < a.len; i++) if (a.data[i] != b.data[i]) return 0;
    return 1;
}

#endif /* RAMBLE_STRING_H */
#pragma endregion
#pragma region common/alloc.h
/* The memory model at every layer. The rules are in spec/allocation.md. */
#ifndef RAMBLE_ALLOC_H
#define RAMBLE_ALLOC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Growable buffer hook: ptr NULL allocates, size 0 frees, else resizes. */
typedef void *(*RambleAllocFn)(void *user, void *ptr, size_t size);

#ifndef RAMBLE_ALLOCATOR_PAGE
#define RAMBLE_ALLOCATOR_PAGE (64u * 1024u)
#endif

/* Page backing for dynamic mode, the RambleAllocFn shape without the user pointer. */
typedef void *(*RamblePageFn)(void *ptr, size_t size);

/* Header in front of every page. A shared page bumps, a freeable page holds one block. */
typedef struct i_RamblePage {
    struct i_RamblePage *next, *prev;
    size_t cap;    /* payload bytes after this header */
    size_t used;   /* shared: the bump cursor. freeable: the block size */
} i_RamblePage;

typedef struct {
    RamblePageFn  page_realloc;   /* NULL means static mode */
    i_RamblePage *shared;         /* bump pages, head is current. static: the buffer */
    i_RamblePage *owned;          /* freeable pages, dynamic only */
    i_RamblePage *free_pool;      /* freed blocks kept for reuse */
    uint32_t    page_size;
    size_t      max_bytes;      /* runaway guard, 0 is unlimited */
    size_t      in_use, pooled, peak;
    uint64_t    alloc_calls, pages_live;
} RambleAllocator;

static inline size_t i_ramble_allocator_align(size_t n){ return (n + 15u) & ~(size_t)15u; }
/* Rounds up to a quarter power of two class so pooled blocks recur. Exact above 4 kB. */
static inline size_t i_ramble_allocator_class(size_t n){
    size_t p = 16u, q;
    if (n <= 16u) return 16u;
    if (n >= 4096u) return i_ramble_allocator_align(n);
    while ((p << 1) <= n){ if (p > (SIZE_MAX >> 2)) return n; p <<= 1; }
    q = p >> 2;
    return p + ((n - p + q - 1u) / q) * q;
}
/* The pool is held memory and counts. A pool reuse allocates nothing and skips this. */
static inline int i_ramble_allocator_over(const RambleAllocator *a, size_t need){
    return a->max_bytes && a->in_use + a->pooled + need > a->max_bytes;
}

static inline RambleAllocator ramble_allocator_static(void *buffer, size_t size){
    RambleAllocator a;
    uint8_t *b = (uint8_t *)buffer;
    uintptr_t aligned = ((uintptr_t)b + 15u) & ~(uintptr_t)15u;
    size_t head = (size_t)(aligned - (uintptr_t)b);
    memset(&a, 0, sizeof a);
    if (b && size >= head + sizeof(i_RamblePage)){
        i_RamblePage *pg = (i_RamblePage *)(b + head);
        pg->next = pg->prev = NULL;
        pg->cap = size - head - sizeof(i_RamblePage);
        pg->used = 0;
        a.shared = pg; a.pages_live = 1;
    }
    return a;
}

static inline RambleAllocator ramble_allocator_dynamic(RamblePageFn page_realloc, uint32_t page_size){
    RambleAllocator a; memset(&a, 0, sizeof a);
    a.page_realloc = page_realloc;
    a.page_size = page_size ? page_size : RAMBLE_ALLOCATOR_PAGE;
    return a;
}

/* Bumps from the current shared page, or adds a page in dynamic mode. */
static inline void *i_ramble_allocator_bump(RambleAllocator *a, size_t need){
    i_RamblePage *pg = a->shared;
    need = i_ramble_allocator_align(need);
    if (!pg || i_ramble_allocator_align(pg->used) + need > pg->cap){
        size_t psz, floor_sz; i_RamblePage *np;
        if (!a->page_realloc || i_ramble_allocator_over(a, need)) return NULL;
        psz = a->page_size;
        floor_sz = need + sizeof(i_RamblePage);
        if (floor_sz > psz) psz = floor_sz;
        np = (i_RamblePage *)a->page_realloc(NULL, psz);
        while (!np && psz > floor_sz){
            /* a fragmented heap may hold the bytes only in shreds: halve until a page fits */
            psz >>= 1;
            if (psz < floor_sz) psz = floor_sz;
            np = (i_RamblePage *)a->page_realloc(NULL, psz);
        }
        if (!np) return NULL;
        np->prev = NULL; np->next = a->shared; if (a->shared) a->shared->prev = np;
        np->cap = psz - sizeof(i_RamblePage); np->used = 0;
        a->shared = np; a->pages_live++;
        pg = np;
    }
    pg->used = i_ramble_allocator_align(pg->used);
    { void *out = (uint8_t *)(pg + 1) + pg->used; pg->used += need; return out; }
}

static inline void *ramble_allocator_fixed(RambleAllocator *a, size_t size){
    void *p;
    if (!a || size == 0) return NULL;
    p = i_ramble_allocator_bump(a, size);
    if (p){ a->alloc_calls++; a->in_use += i_ramble_allocator_align(size);
            if (a->in_use > a->peak) a->peak = a->in_use; }
    return p;
}

/* A freeable block: a pooled one when it fits, else its own page, or a bump in static mode. */
static inline void *i_ramble_allocator_new_owned(RambleAllocator *a, size_t cap){
    i_RamblePage *pg;
    {   /* the smallest pooled block within 2x, so a big block is not spent on a small ask */
        i_RamblePage *it, *best = NULL;
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
        if (i_ramble_allocator_over(a, cap)) return NULL;
        pg = (i_RamblePage *)a->page_realloc(NULL, sizeof(i_RamblePage) + cap);
        if (!pg) return NULL;
        pg->prev = NULL; pg->next = a->owned; if (a->owned) a->owned->prev = pg;
        a->owned = pg; a->pages_live++;
    } else {
        pg = (i_RamblePage *)i_ramble_allocator_bump(a, sizeof(i_RamblePage) + cap);
        if (!pg) return NULL;
        pg->prev = pg->next = NULL;   /* not linked, reset rewinds the buffer */
    }
    pg->cap = cap; pg->used = cap;
    a->alloc_calls++; a->in_use += cap; if (a->in_use > a->peak) a->peak = a->in_use;
    return (void *)(pg + 1);
}

/* A freed block goes to the pool, never back to the backing heap. Reset reclaims it. */
static inline void i_ramble_allocator_free_owned(RambleAllocator *a, void *ptr){
    i_RamblePage *pg = (i_RamblePage *)ptr - 1;
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

/* The RambleAllocFn over a RambleAllocator. Pass the allocator as user. */
static inline void *ramble_allocator_alloc(void *alloc, void *ptr, size_t size){
    RambleAllocator *a = (RambleAllocator *)alloc; size_t cap;
    if (!a) return NULL;
    if (size == 0){ if (ptr) i_ramble_allocator_free_owned(a, ptr); return NULL; }
    cap = a->page_realloc ? i_ramble_allocator_class(size) : i_ramble_allocator_align(size);
    if (!ptr) return i_ramble_allocator_new_owned(a, cap);
    {   i_RamblePage *pg = (i_RamblePage *)ptr - 1;
        if (cap <= pg->cap) return ptr;
        {   void *np = i_ramble_allocator_new_owned(a, cap);
            if (!np) return NULL;   /* the old block stays intact */
            memcpy(np, ptr, pg->cap);
            i_ramble_allocator_free_owned(a, ptr);
            return np;
        }
    }
}

/* Frees both intents and the pool. Static mode rewinds the buffer and keeps it. */
static inline void ramble_allocator_reset(RambleAllocator *a){
    if (!a) return;
    if (a->page_realloc){
        i_RamblePage *pg, *nx;
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

static inline void ramble_allocator_stats(const RambleAllocator *a, size_t *in_use, size_t *peak,
                                     uint64_t *alloc_calls){
    if (!a) return;
    if (in_use)      *in_use      = a->in_use;
    if (peak)        *peak        = a->peak;
    if (alloc_calls) *alloc_calls = a->alloc_calls;
}

#endif /* RAMBLE_ALLOC_H */
#pragma endregion
#pragma region transport/core.h
/* The sans-IO transport core. Feed it datagrams, the clock and a peer set, it returns
 * datagrams to send and delivers messages. The rules are in spec/transport.md. */
#ifndef RAMBLE_TRANSPORT_H
#define RAMBLE_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RAMBLE_SHM detection. This block mirrors platform/core.h exactly. Edit both together. */
#if !defined(RAMBLE_SHM) && !defined(RAMBLE_NO_SHM)
  #if defined(_WIN32) || defined(__linux__) || defined(__APPLE__) || \
      defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
      defined(__DragonFly__)
    #define RAMBLE_SHM
  #endif
#endif
#if defined(RAMBLE_SHM) && defined(RAMBLE_NO_SHM)
  #undef RAMBLE_SHM              /* both set, the opt out wins */
#endif

#ifndef RAMBLE_FRAG_SIZE
#define RAMBLE_FRAG_SIZE 1350u          /* default message bytes per fragment */
#endif
/* A node fragments every send with one size, set at init and advertised by discovery.
 * MIN and MAX bound any node's size, and MAX sizes the datagram buffers. */
#ifndef RAMBLE_FRAG_SIZE_MAX
#define RAMBLE_FRAG_SIZE_MAX RAMBLE_FRAG_SIZE
#endif
#ifndef RAMBLE_FRAG_SIZE_MIN
#define RAMBLE_FRAG_SIZE_MIN RAMBLE_FRAG_SIZE
#endif
#define RAMBLE_DGRAM_MAX (RAMBLE_FRAG_SIZE_MAX + 40u)   /* plus the largest header */

#ifdef RAMBLE_SHM
#define RAMBLE_SHM_DESC_BYTES 24u   /* the descriptor on the wire, equals RAMBLE_SHM_DESC_WIRE */
#endif

#ifndef RAMBLE_TOPIC_NAME_MAX
#define RAMBLE_TOPIC_NAME_MAX 64u          /* topic name bytes on the wire */
#endif

#ifndef RAMBLE_NODE_NAME_MAX
#define RAMBLE_NODE_NAME_MAX 32u           /* node name bytes in the announce */
#endif

/* The source stamp in front of every sample of a stamped topic, taken once at commit so
 * repair, replay and shared memory carry the original. RambleQos.no_timestamp opts out. */
#define RAMBLE_TIMESTAMP_BYTES 8u
/* A microsecond wall clock needs 51 bits, so the source stamp's top bit is free to mark
 * that a capture slot follows it. Per message, and costs nothing unset. See spec/node.md */
#define RAMBLE_CAPTURE_BYTES   8u
#define RAMBLE_STAMP_CAPTURE   0x8000000000000000ull
#define RAMBLE_STAMP_MASK      0x7FFFFFFFFFFFFFFFull

typedef enum { RAMBLE_BEST_EFFORT = 0, RAMBLE_RELIABLE = 1 } RambleReliability;
/* RAMBLE_INACTIVE is declared but off. Resources stay allocated and set_role flips it. */
typedef enum { RAMBLE_PUBSUB = 0, RAMBLE_PUB_ONLY = 1, RAMBLE_SUB_ONLY = 2,
               RAMBLE_INACTIVE = 3 } RambleRole;
/* Shared by every matching, announce and reflection walk, so a flipped test is impossible. */
static inline int ramble_role_pubs(uint8_t role){ return role == RAMBLE_PUBSUB || role == RAMBLE_PUB_ONLY; }
static inline int ramble_role_subs(uint8_t role){ return role == RAMBLE_PUBSUB || role == RAMBLE_SUB_ONLY; }

/* What a topic carries. A same name under a different kind is a disjoint entity and the
 * pairing is refused. The kind rides the interest flags and is immutable per topic. */
typedef enum {
    RAMBLE_KIND_TOPIC    = 0,   /* plain pub sub */
    RAMBLE_KIND_FUNC_REQ = 1,   /* function requests, the caller pubs */
    RAMBLE_KIND_FUNC_RSP = 2,   /* function responses, the provider pubs, directed */
    RAMBLE_KIND_VARIABLE = 3,   /* variable values, the owner pubs */
    RAMBLE_KIND_VAR_SET  = 4,   /* variable sets, writers pub, the owner subs */
    RAMBLE_KIND_TASK_REQ = 5,   /* task requests and cancel ops, callers pub */
    RAMBLE_KIND_TASK_PRG = 6,   /* task progress, the provider pubs, broadcast */
    RAMBLE_KIND_TASK_RSP = 7    /* task responses, the provider pubs, directed */
} RambleTopicKind;

/* Immutable per topic facts served in the detail exchange and cached per (peer, index).
 * NO_TIMESTAMP is derived from the qos, the rest come from RambleTopicDef.attrs. */
#define RAMBLE_ATTR_NO_TIMESTAMP 0x01u /* the publisher sends no source stamp */
#define RAMBLE_ATTR_MULTI        0x02u /* authority kinds: duplicate authority is intended */
#define RAMBLE_ATTR_FORCEABLE    0x04u /* variable values: the owner permits force */
#define RAMBLE_ATTR_CANCELLABLE  0x08u /* task requests: the provider honors cancel */
#define RAMBLE_ATTR_EXCLUSIVE    0x10u /* task requests: declared serialization */

/* Every field except reliability is zero means default. docs/topics.md explains each. */
typedef struct {
    RambleReliability reliability;
    uint16_t keep_last;          /* messages retained. 0 = 1, or 10 on a reliable topic */
    uint16_t catch_up;           /* messages a new subscriber gets at once. 0 = future only */
    uint32_t max_message_bytes;  /* a size hint that pins the SHM class, never a cap */
    uint32_t heartbeat_us;       /* reliable idle publisher ping. 0 = 250 ms */
    uint32_t repair_delay_us;    /* the reliable re ask bound, 0 = adaptive from the round trip */
    uint32_t backpressure_wait_us;/* reliable send pause for a slow subscriber. 0 = none */
    uint32_t shm_max_bytes;      /* pin the topic to one SHM class. 0 = per message */
    uint32_t queue_bytes;        /* the consumer queue capacity, 0 = lazy up to RAMBLE_QUEUE_CAP */
    uint16_t max_rate_hz;        /* subscriber side, best effort: a delivery cap per publisher */
    uint8_t  no_timestamp;       /* publisher side: no source stamp, receivers see written_us 0 */
} RambleQos;

/* The name is the cross peer identity. The local handle is the index in topics[]. */
typedef struct {
    const char *name;  /* required, the same on every node, at most RAMBLE_TOPIC_NAME_MAX */
    RambleQos   qos;
    uint8_t  role;     /* RambleRole, 0 = pub and sub */
    uint8_t  kind;     /* RambleTopicKind, the patterns layer sets the rest */
    uint8_t  attrs;    /* RAMBLE_ATTR_* declared by the definer. NO_TIMESTAMP comes from the qos */
    uint8_t  prefix_bytes; /* pattern header bytes ahead of the payload, split off on receive */
    uint8_t  directed; /* 1 = point to point sends. Other lanes skip the seqno with no MSG_LOST */
} RambleTopicDef;

/* Return 0 delivered. Nonzero refuses: a reliable subscriber parks the sample with no
 * ack, so the publisher's flow control carries the backpressure. Best effort drops it. */
typedef int (*RambleMessageFn)(void *user, uint16_t topic_index, uint32_t from_peer, RambleBytes data);

#ifdef RAMBLE_SHM
/* SHM delivery hands the node the descriptor. 1 delivered, 0 unresolvable (the reliability
 * layer repairs or skips), -1 refused (parked like an inline refusal). */
typedef int (*i_RambleShmMsgFn)(void *user, uint16_t topic_index, uint32_t from_peer,
                             const uint8_t *desc);
#endif

/* The transport's own events. The node maps them into its RambleEvent. */
typedef enum {
    RAMBLE_TRANSPORT_MSG_LOST,        /* seqnos skipped: .topic, .peer, .lost_first, .lost_count */
    RAMBLE_TRANSPORT_MSG_TOO_BIG,     /* a received message could not be buffered, .too_big_bytes */
    RAMBLE_TRANSPORT_NAME_COLLISION,  /* a peer's name hashes to ours but differs, .identity */
    RAMBLE_TRANSPORT_QOS_INCOMPATIBLE,/* a reliable subscriber refused a best effort publisher */
    RAMBLE_TRANSPORT_SCHEMA_MISMATCH, /* the schema_check hook refused a direction, .peer_is_pub */
    RAMBLE_TRANSPORT_INTEREST_OVERFLOW,/* a peer's index map failed to allocate: .lost_count */
    RAMBLE_TRANSPORT_META_TRUNCATED_INTEREST, /* our overlay overflowed, peers see no topics */
    RAMBLE_TRANSPORT_META_TRUNCATED_SCHEMA,   /* retired, kept so binding enums stay aligned */
    RAMBLE_TRANSPORT_KIND_MISMATCH    /* a verified peer advertises the name under another kind */
} RambleTransportEventKind;

/* Read only the fields named for the kind. The topic name comes from ramble_transport_topic_name. */
typedef struct {
    RambleTransportEventKind kind;
    void       *user;          /* RambleConfig.user */
    uint32_t   peer;           /* 0 = none */
    uint16_t   topic;        /* local topic handle */
    uint64_t   lost_first;     /* MSG_LOST: first skipped seqno */
    uint64_t   lost_count;     /* MSG_LOST: seqnos skipped, fragments not messages */
    uint64_t   too_big_bytes;  /* MSG_TOO_BIG: size of the dropped message */
    uint64_t   identity;       /* NAME_COLLISION: the colliding identity */
    uint8_t    peer_is_pub;    /* SCHEMA_MISMATCH: the refused direction, 1 = their publisher */
} RambleTransportEvent;
typedef void (*RambleTransportEventFn)(const RambleTransportEvent *ev);

/* Largest message the wire can carry, 65535 fragments. */
#define RAMBLE_MESSAGE_MAX (65535u * RAMBLE_FRAG_SIZE_MAX)

/* RambleConfig.allocator is required. Every growable buffer sizes through it, so memory
 * scales with traffic and matches. Static mode passes ramble_allocator_alloc over a static one. */

/* topics set: defined at init. topics NULL: n_topics reserved slots, all INACTIVE, filled
 * later by ramble_transport_topic_define. */
typedef struct {
    const RambleTopicDef *topics;     /* NULL = reserve mode */
    uint16_t                n_topics;   /* defined count, or the reserved capacity */
    uint16_t                max_peers;
    uint16_t                frag_size; /* our fragment size, 0 = RAMBLE_FRAG_SIZE, clamped */
    RambleMessageFn         on_message;
#ifdef RAMBLE_SHM
    i_RambleShmMsgFn         on_shm;     /* SHM-DATA delivery, the node resolves the descriptor */
#endif
    RambleTransportEventFn  on_event;   /* optional */
    RambleAllocFn           allocator;  /* required, init fails without it */
    /* Optional schema gate at detail intake, once per direction. peer_is_pub 1 gates the
     * read side. Return 1 to allow. Verdicts are cached per (peer, index). */
    int                 (*schema_check)(void *user, uint32_t peer, uint16_t topic_index,
                                        int peer_is_pub, uint64_t schema_hash,
                                        RambleBytes schema_wire);
    /* Optional wall clock, UTC microseconds, stamped at commit. NULL still frames the 8
     * bytes as 0 on a stamped topic. Never the monotonic clock, the stamp crosses hosts. */
    uint64_t            (*source_time)(void *user);
    void                 *user;
} RambleConfig;

typedef struct RambleTransportState RambleTransportState;

RAMBLE_API size_t    ramble_transport_required_memory(const RambleConfig *cfg);
RAMBLE_API RambleTransportState *ramble_transport_init(void *mem, size_t mem_size, const RambleConfig *cfg);
/* Relocates a live transport, re striding its tables and carrying reliability state. The
 * caller frees the old arena block but must not destroy old. NULL leaves old intact. */
RAMBLE_API RambleTransportState *ramble_transport_migrate(RambleTransportState *old, void *new_mem, size_t new_cap,
                                 uint16_t new_max_peers, uint16_t new_n_topics);
/* Frees every hook allocation. The arena stays the caller's. */
RAMBLE_API void      ramble_transport_destroy(RambleTransportState *st);

/* The FNV-1a identity of a name. */
RAMBLE_API uint64_t  ramble_topic_id(const char *name);
RAMBLE_API uint64_t  ramble_topic_identity(const RambleTopicDef *def);   /* ramble_topic_id(def->name) */

/* 0 to RAMBLE_FRAG_SIZE, then clamp to [MIN, MAX]. Shared with the node's announce. */
RAMBLE_API uint16_t  ramble_clamp_frag(uint16_t frag_size);

/* Our fragment size. The node uses it as the SHM cutoff. */
RAMBLE_API uint16_t  ramble_transport_frag(RambleTransportState *st);

/* A new peer matches nothing until apply_peer_interest. peer_frag is its advertised
 * fragment size, 0 = RAMBLE_FRAG_SIZE, clamped to [MIN, MAX]. */
RAMBLE_API void      ramble_transport_peer_add   (RambleTransportState *st, uint32_t peer_id, uint16_t peer_frag);
RAMBLE_API void      ramble_transport_peer_remove(RambleTransportState *st, uint32_t peer_id);

/* A silent peer goes DORMANT: out of flow control, proxies and positions kept. resume re
 * includes it and re reports reader positions. Both no ops for an unknown peer. */
RAMBLE_API void      ramble_transport_peer_dormant(RambleTransportState *st, uint32_t peer_id);
RAMBLE_API void      ramble_transport_peer_resume (RambleTransportState *st, uint32_t peer_id);
/* A peer's blob may arrive after first contact. Clamped, a no op for an unknown peer. */
RAMBLE_API void      ramble_transport_peer_set_frag(RambleTransportState *st, uint32_t peer_id, uint16_t peer_frag);

/* The interest exchange. The list is hash only and positional, a hash overlap only
 * nominates, and the detail exchange below verifies. See spec/interest.md. */
RAMBLE_API size_t    ramble_interest_max(uint16_t n_topics);
RAMBLE_API size_t    ramble_transport_build_interest(RambleTransportState *st, void *out, size_t cap);
RAMBLE_API void      ramble_transport_apply_peer_interest(RambleTransportState *st, uint32_t peer_id, RambleBytes blob);

/* The announce overlay: a version prefix (fragment size, SHM capability and host) plus
 * the interest list, or the INTEREST_EXTERNAL bootstrap when it does not fit one datagram. */

/* One topic's schema advertisement, served by the detail responder. hash 0 = none. */
typedef struct {
    uint64_t    hash;
    RambleBytes wire;
} RambleMetaSchema;

/* Worst case overlay bytes for n_topics, capped to one datagram. Sizes discovery's meta_cap. */
RAMBLE_API uint16_t  ramble_meta_cap(uint16_t n_topics);
/* Exact bytes the next inline build emits, so a caller sizes to content. */
RAMBLE_API uint16_t  ramble_transport_meta_size(RambleTransportState *st);
RAMBLE_API uint16_t  ramble_transport_meta_bootstrap_size(void);
/* Builds the overlay. interest_external writes the bootstrap only. The node inlines while
 * the whole announce fits one datagram. Returns the bytes. */
RAMBLE_API uint16_t  ramble_transport_meta_build(RambleTransportState *st, uint8_t *out, uint16_t cap,
                                uint16_t frag_size, int shm_capable, const uint8_t host[16],
                                int interest_external);
/* 0 if malformed. */
RAMBLE_API uint16_t  ramble_meta_frag(RambleBytes meta);
/* 1 = the peer serves its interest through paging. */
RAMBLE_API int       ramble_meta_interest_external(RambleBytes meta);
/* {NULL, 0} if absent or external, so a bootstrap never reads as an empty interest list. */
RAMBLE_API RambleBytes ramble_meta_interest(RambleBytes meta);

/* One advertised direction of a topic. A PUBSUB topic yields twice, pub first. */
typedef struct {
    uint16_t    index;      /* the advertiser's topic index */
    uint8_t     role;       /* the advertiser's RambleRole */
    uint8_t     is_pub;     /* 1 = the publish direction, 0 = subscribe */
    uint8_t     reliable;   /* offered or requested reliability */
    uint8_t     kind;       /* the advertiser's RambleTopicKind */
    uint32_t    hash;       /* low 32 bits of the identity */
} RambleTopicEntry;

/* Zero initialize, then call until 0. Internal walk state. */
typedef struct {
    uint32_t off;        /* byte offset of the next entry */
    uint16_t left;       /* entries still to walk */
    uint16_t index;      /* position of the next entry */
    uint8_t  phase;      /* 1 = the current entry's pub direction was yielded */
    uint8_t  started;    /* 0 until the first call parses the header */
} RambleInterestIter;

/* Walks one direction at a time and stops at the end or at a malformed blob. interest_next
 * walks a bare blob, meta_interest_next the one inside an overlay. */
RAMBLE_API int       ramble_interest_next(RambleBytes interest, RambleInterestIter *it, RambleTopicEntry *out);
RAMBLE_API int       ramble_meta_interest_next(RambleBytes meta, RambleInterestIter *it, RambleTopicEntry *out);
#ifdef RAMBLE_SHM
/* 1 and host filled when the peer is SHM capable. */
RAMBLE_API int       ramble_meta_shm(RambleBytes meta, uint8_t host[16]);
#endif

/* The pairwise detail exchange, uDTL. The responder is stateless and answers the
 * request's source. The layout is in spec/interest.md. */
#define RAMBLE_DETAIL_REQ    1
#define RAMBLE_DETAIL_RESP   2
#define RAMBLE_INTEREST_REQ  3
#define RAMBLE_INTEREST_RESP 4

/* The peer's index plus our schema hash for it, 0 = none. */
typedef struct {
    uint16_t index;
    uint64_t schema_hash;
} RambleDetailWant;

/* Safe on any buffer. kind is 0 for anything that is not a well formed uDTL header. */
RAMBLE_API int       ramble_detail_kind(RambleBytes dgram);
RAMBLE_API uint16_t  ramble_detail_domain(RambleBytes dgram);
RAMBLE_API uint32_t  ramble_detail_meta_version(RambleBytes dgram);

/* Interest paging carries byte ranges of the build_interest blob. The requester keeps one
 * cursor per peer and a changed version restarts it. See spec/interest.md. */
#define RAMBLE_INTEREST_RESP_HEAD 24u   /* the family header plus total, offset and chunk_len */

/* Exact bytes of our current interest blob, the total_len paging serves. */
RAMBLE_API uint32_t  ramble_transport_interest_size(RambleTransportState *st);
/* 18 bytes, or 0 if cap is too small. peer_meta_version is advisory. */
RAMBLE_API size_t    ramble_interest_req_build(uint16_t domain, uint32_t peer_meta_version,
                                uint32_t offset, void *out, size_t cap);
/* 1 and *offset for a well formed INTEREST_REQ, else 0. */
RAMBLE_API int       ramble_interest_req_offset(RambleBytes dgram, uint32_t *offset);
/* Writes one page header. The caller lays the chunk right after it. */
RAMBLE_API size_t    ramble_interest_resp_head(uint16_t domain, uint32_t meta_version, uint32_t total_len,
                                uint32_t offset, uint16_t chunk_len, void *out, size_t cap);
/* 1 and the fields, chunk a bounds checked view into dgram, else 0. */
RAMBLE_API int       ramble_interest_resp_parse(RambleBytes dgram, uint32_t *total_len, uint32_t *offset,
                                RambleBytes *chunk);

/* 0 if cap cannot hold every want (14 plus 10 each). Batch per peer. */
RAMBLE_API size_t    ramble_detail_req_build(uint16_t domain, uint32_t peer_meta_version,
                                const RambleDetailWant *wants, uint16_t n_wants,
                                void *out, size_t cap);

/* Bytes one response page takes, 0 if req is malformed. One datagram, except that a lone
 * oversized entry rides its own page. */
RAMBLE_API size_t    ramble_transport_detail_resp_size(RambleTransportState *st, const RambleMetaSchema *schemas,
                                RambleBytes req);
/* Answers req into out with one entry per advertised requested index, the wire inlined
 * only where the hashes differ. Truncates at an entry boundary. Does not check the domain. */
RAMBLE_API size_t    ramble_transport_detail_respond(RambleTransportState *st, const RambleMetaSchema *schemas,
                                uint32_t meta_version, RambleBytes req, void *out, size_t cap);

/* One topic's details. name and schema_wire point into the response. */
typedef struct {
    uint16_t     index;       /* the responder's local topic index */
    uint8_t      attrs;       /* the topic's RAMBLE_ATTR_* byte */
    RambleString name;
    uint64_t     schema_hash; /* 0 = untyped */
    RambleBytes  schema_wire; /* only when the request's hash differed, else {NULL,0} */
} RambleDetail;

/* Zero initialize, then call until 0. */
typedef struct {
    uint32_t off;      /* byte offset of the next entry */
    uint16_t left;     /* entries still to yield */
    uint8_t  started;  /* 0 until the first call parses the header */
} RambleDetailIter;

/* Walks a DETAIL_RESP. Stops at the end or at a malformed response. */
RAMBLE_API int       ramble_detail_next(RambleBytes resp, RambleDetailIter *it, RambleDetail *out);

/* The pending candidates of a peer, up to max_wants (out NULL just counts). Re run it on
 * every announce from the peer while it returns nonzero. */
RAMBLE_API uint16_t  ramble_transport_detail_wants(RambleTransportState *st, const RambleMetaSchema *schemas,
                                uint32_t peer_id, RambleBytes interest,
                                RambleDetailWant *out, uint16_t max_wants);
/* Ingests a DETAIL_RESP and caches the verdicts. Returns the newly decided count, after
 * which the caller re applies the peer's interest so the matches form. */
RAMBLE_API uint16_t  ramble_transport_apply_peer_details(RambleTransportState *st, uint32_t peer_id,
                                RambleBytes resp);

/* Entries of a peer that nominate this topic and are still undecided. Feeds the match wait. */
RAMBLE_API uint16_t  ramble_transport_topic_unresolved(RambleTransportState *st, uint16_t topic_index,
                                uint32_t peer_id, RambleBytes interest);
/* The bulk form: every topic's unresolved count against one peer in a single walk. */
RAMBLE_API void      ramble_transport_peer_unresolved_fill(RambleTransportState *st, uint32_t peer_id,
                                RambleBytes interest, uint16_t *counts, uint16_t n);

/* Rematches peers locally. The caller re advertises. 0 ok, negative unknown. */
RAMBLE_API int       ramble_transport_set_role(RambleTransportState *st, uint16_t topic_index, uint8_t role);

/* Defines a reserved slot: name, qos, role, a history ring and a rematch. 0 ok, -1 bad
 * index, name or slot, -4 out of memory. Re advertise after. */
RAMBLE_API int       ramble_transport_topic_define(RambleTransportState *st, uint16_t topic_index, const RambleTopicDef *def);

/* Slot lifecycle, see spec/interest.md. retire parks a defined slot: 0 ok, -1 unknown or
 * already retired. Re advertise after. */
RAMBLE_API int       ramble_transport_topic_retire(RambleTransportState *st, uint16_t topic_index);
/* The slot a new define should reuse: 2 = a retired slot with the same identity and
 * kind, 1 = another retired slot, 0 = none. */
RAMBLE_API int       ramble_transport_topic_reuse_find(RambleTransportState *st, const char *name, uint8_t kind,
                                uint16_t *index_out);
/* Re defines a retired slot. binding_changed bumps the generation and holds writer lanes
 * until each peer names rebind_version in a request. 0 ok, -1 refused, -4 out of memory. */
RAMBLE_API int       ramble_transport_topic_reuse(RambleTransportState *st, uint16_t topic_index,
                                const RambleTopicDef *def, int binding_changed, uint32_t rebind_version);
/* Records the highest version of our blob a peer named in a request. 1 when a held lane formed. */
RAMBLE_API int       ramble_transport_peer_seen_version(RambleTransportState *st, uint32_t peer_id, uint32_t version);

/* The next write seqno. It continues across retire and reuse. */
RAMBLE_API uint64_t  ramble_transport_topic_seqno(RambleTransportState *st, uint16_t topic_index);

/* {NULL,0} if undefined. A local lookup, never on the data path. */
RAMBLE_API RambleString ramble_transport_topic_name(RambleTransportState *st, uint16_t topic_index);

/* 0 if the peer advertised no_timestamp for this topic. An unknown peer answers 1. */
RAMBLE_API int       ramble_transport_peer_timestamped(RambleTransportState *st, uint16_t topic_index,
                                uint32_t peer_id);

/* The peer's cached attrs byte for its topic, 0 until its details arrive. */
RAMBLE_API uint8_t   ramble_transport_peer_attrs(RambleTransportState *st, uint32_t peer_id,
                                uint16_t their_index);

/* 0 ok, negative on error. */
typedef enum {
    RAMBLE_OK             =  0,
    RAMBLE_ERR_NO_TOPIC = -1,  /* topic index out of range */
    RAMBLE_ERR_TOO_BIG    = -2,  /* exceeds RAMBLE_MESSAGE_MAX */
    RAMBLE_ERR_ROLE       = -3,  /* the topic is SUB_ONLY or INACTIVE */
    RAMBLE_ERR_OOM        = -4,  /* the allocator returned NULL */
    RAMBLE_ERR_STATE      = -5,  /* wrong state, or a call not allowed from inside a callback */
    RAMBLE_ERR_NOSYS      = -6   /* not compiled in */
} RambleResult;

/* Publishes to every matched subscriber. */
RAMBLE_API int       ramble_transport_send(RambleTransportState *st, uint16_t topic_index, RambleBytes data, uint64_t now_us);

/* Matched subscribers excluding dormant peers. O(matched lanes). */
RAMBLE_API int       ramble_transport_publisher_live_matches(RambleTransportState *st, uint16_t topic_index);
/* Is the peer a matched subscriber lane of this topic. The patterns layer's directed backstop. */
RAMBLE_API int       ramble_transport_publisher_peer_matched(RambleTransportState *st, uint16_t topic_index,
                                                         uint32_t peer_id);
/* The oldest live matched subscriber, 0 = none. The auto direct target for task requests. */
RAMBLE_API uint32_t  ramble_transport_publisher_oldest_match(RambleTransportState *st, uint16_t topic_index);
/* Matched publishers feeding our subscription side. */
RAMBLE_API int       ramble_transport_subscriber_match_count(RambleTransportState *st, uint16_t topic_index);

/* hdr rides in front of data as one message, byte identical to sending the concatenation.
 * capture_us 0 sends no capture slot. */
RAMBLE_API int       ramble_transport_send_hdr(RambleTransportState *st, uint16_t topic_index,
                               RambleBytes hdr, RambleBytes data, uint64_t capture_us, uint64_t now_us);
/* Publishes to one peer. Every other matched reliable lane skips the seqno through its
 * HB floor. A no op delivery when the peer is not a matched subscriber. */
RAMBLE_API int       ramble_transport_send_to(RambleTransportState *st, uint16_t topic_index, uint32_t to_peer,
                               RambleBytes hdr, RambleBytes data, uint64_t capture_us, uint64_t now_us);

#ifdef RAMBLE_SHM
/* The payload lives in an external chunk, fragmented for remote peers and described to
 * SHM peers. The chunk is the whole wire sample and must stay valid until it leaves history. */
RAMBLE_API int       ramble_transport_send_shm(RambleTransportState *st, uint16_t topic_index, RambleBytes chunk,
                               const uint8_t *desc, uint64_t now_us);
/* Whether a peer can receive SHM-DATA. The node sets it on attach. */
RAMBLE_API void      ramble_transport_peer_set_shm(RambleTransportState *st, uint32_t peer_id, int is_shm);
/* 1 if every matched subscriber is SHM capable. */
RAMBLE_API int       ramble_transport_publisher_shm_eligible(RambleTransportState *st, uint16_t topic_index);
/* The history slot the next publish occupies, to bind a chunk to it. */
RAMBLE_API uint16_t  ramble_transport_topic_hist_head(RambleTransportState *st, uint16_t topic_index);
#endif

/* NULL if unknown. */
RAMBLE_API const RambleQos *ramble_transport_topic_qos(RambleTransportState *st, uint16_t topic_index);
/* 0 = none or undefined. */
RAMBLE_API uint8_t   ramble_transport_topic_attrs(RambleTransportState *st, uint16_t topic_index);

/* 1 if the next send would overwrite history not yet acked by every subscriber. */
RAMBLE_API int       ramble_transport_send_would_evict(RambleTransportState *st, uint16_t topic_index);

/* 1 if the next send would overwrite history never handed to the wire, best effort
 * included. Fills the evicted sample's base and count. */
RAMBLE_API int       ramble_transport_send_would_evict_unsent(RambleTransportState *st, uint16_t topic_index,
                                                          uint64_t *evict_base, uint32_t *evict_count);

/* 1 if every live subscriber acked everything. Best effort and unknown return 1. */
RAMBLE_API int       ramble_transport_send_drained(RambleTransportState *st, uint16_t topic_index);

/* Matched subscribers, dormant included. O(1). */
RAMBLE_API int       ramble_transport_publisher_match_count(RambleTransportState *st, uint16_t topic_index);

/* Topics we publish to and receive from this peer. Both 0 for an unknown peer. */
RAMBLE_API void      ramble_transport_peer_match_counts(RambleTransportState *st, uint32_t peer_id,
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
    uint64_t msgs_skipped;   /* the sum of RAMBLE_MSG_LOST counts */
    /* each 0 to 1 arming of the subscriber's ack, by its trigger */
    uint64_t arms_data;
    uint64_t arms_hb;
    /* the fate of every received DATA fragment that was not accepted */
    uint64_t frags_old;        /* base below deliver_upto: already delivered or skipped */
    uint64_t frags_ahead;      /* beyond the one sample held ahead: dropped, fetched in order */
    uint64_t frags_malformed;  /* count 0, frag past count, or not subscribed */
} RambleRepairStats;

/* Zeroed if the topic is out of range. */
RAMBLE_API void      ramble_transport_repair_stats(RambleTransportState *st, uint16_t topic_index, RambleRepairStats *out);

/* The per peer round trip estimate, RFC 6298 shape, fed by the reliable path with no
 * probe traffic. samples 0 means no estimate yet. See spec/transport.md. */
typedef struct {
    uint32_t rtt_us;         /* smoothed round trip */
    uint32_t rtt_jitter_us;  /* mean deviation */
    uint32_t rtt_min_us;     /* the smallest sample, the path's floor */
    uint32_t rtt_last_us;    /* the most recent sample */
    uint32_t samples;
} RamblePeerRtt;
/* 1 and *out for a known peer, else 0 and *out zeroed. */
RAMBLE_API int       ramble_transport_peer_rtt(RambleTransportState *st, uint32_t peer_id, RamblePeerRtt *out);

/* Subscriber lanes of this topic with a repair request to service. */
RAMBLE_API int       ramble_transport_repair_pending(RambleTransportState *st, uint16_t topic_index);

/* The in progress message from peer: its base seqno, fragments held and fragments needed.
 * 1 if a message is mid reassembly. Any out pointer may be NULL. */
RAMBLE_API int       ramble_transport_subscriber_progress(RambleTransportState *st, uint16_t topic_index, uint32_t peer,
                                     uint64_t *base_seqno, uint32_t *have, uint32_t *total);

/* Retries the parked samples of a topic when downstream capacity frees, then flush
 * poll_send so the acks go out. Returns the lanes still parked. */
RAMBLE_API uint32_t  ramble_transport_deliver_parked(RambleTransportState *st, uint16_t topic_index, uint64_t now_us);

/* Feeds a received datagram tagged with its peer. */
RAMBLE_API void      ramble_transport_on_datagram(RambleTransportState *st, uint32_t from_peer, RambleBytes datagram,
                                  uint64_t now_us);

/* One outgoing datagram, batched per peer. 1 and the outs, or 0. Loop until 0 with a
 * RAMBLE_DGRAM_MAX cap. */
RAMBLE_API int       ramble_transport_poll_send(RambleTransportState *st, uint32_t *to_peer, void *out, size_t cap,
                                size_t *out_len, uint64_t now_us);

/* The next armed timer, or 0. Cap a blocking poll at it. */
RAMBLE_API uint64_t  ramble_transport_next_deadline_us(RambleTransportState *st);

/* 1 while any lane holds work for poll_send. O(1). */
RAMBLE_API int       ramble_transport_tx_pending(RambleTransportState *st);

#ifdef __cplusplus
}
#endif
#endif /* RAMBLE_TRANSPORT_H */
#pragma endregion

#ifndef RAMBLE_TRANSPORT_SANS_IO
#pragma region serialize/schema.h
/* The schema and serialization layer. Both ends share the schema, so the wire carries no
 * tags or names. Depends only on common/. The rules are in spec/schema.md. */
#ifndef RAMBLE_SCHEMA_H
#define RAMBLE_SCHEMA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RAMBLE_SCHEMA_WIRE_VERSION
#define RAMBLE_SCHEMA_WIRE_VERSION 8u    /* bumped on any schema wire change */
#endif
#ifndef RAMBLE_SCHEMA_MAX_DEPTH
#define RAMBLE_SCHEMA_MAX_DEPTH 8u        /* struct nesting the builder accepts */
#endif

/* The kind byte on the wire. Fixed kinds pack first at static offsets, the variable kinds
 * ride tail frames. The encodings are in spec/schema.md. */
typedef enum {
    RAMBLE_U8 = 0, RAMBLE_U16 = 1, RAMBLE_U32 = 2, RAMBLE_U64 = 3,
    RAMBLE_I8 = 4, RAMBLE_I16 = 5, RAMBLE_I32 = 6, RAMBLE_I64 = 7,
    RAMBLE_F32 = 8, RAMBLE_F64 = 9, RAMBLE_BOOL = 10,   /* 0 to 10: fixed scalars */
    RAMBLE_ARR    = 11,   /* fixed array, the element must be fixed */
    RAMBLE_STRUCT = 12,
    RAMBLE_STR    = 13,   /* capped string, a slot of [u16 len][cap bytes] */
    RAMBLE_VSTR   = 14,   /* variable string, a tail frame */
    RAMBLE_VARR   = 15,   /* variable array, a tail frame of packed elements */
    RAMBLE_MAP    = 16,   /* self describing map, a tail frame */
    RAMBLE_ENUM   = 17,   /* a named integer, on the wire just its backing scalar */
    RAMBLE_NAMED  = 18    /* a name tag on another type, zero message bytes, never a root */
} RambleSchemaTypeKind;

/* Bytes of a fixed scalar kind, 0 otherwise. */
RAMBLE_API uint32_t ramble_schema_scalar_size(RambleSchemaTypeKind kind);

/* The compiled schema: wire bytes, hash, size and a flat field table in one block from
 * the hook. Free it with ramble_schema_free. */
typedef struct RambleSchema RambleSchema;

/* One field of the flat depth first table. A struct array flattens its element once as
 * an element 0 template under the array field. See spec/schema.md. */
typedef struct {
    RambleString name;      /* the field's own name, a view into the wire */
    RambleString type_name; /* the field type's name, {NULL,0} when anonymous */
    RambleString elem_name; /* an array element type's name, {NULL,0} when anonymous */
    uint8_t      kind;      /* RambleSchemaTypeKind, a NAMED wrapper is unwrapped into type_name */
    uint8_t      elem;      /* the array element kind, or the enum backing kind, else 0 */
    uint16_t     count;     /* ARR element count, or ENUM variant count, else 0 */
    uint16_t     depth;     /* 0 = top level, n = a member of the struct n levels up */
    uint16_t     str_cap;   /* string capacity for STR fields and STR elements, else 0 */
    uint16_t     arr_parent;/* flat index of the enclosing struct array, or 0xFFFF */
    uint32_t     offset;    /* absolute byte offset, 0 for variable kinds */
    uint32_t     size;      /* byte size, 0 for variable kinds */
    uint32_t     elem_size; /* ARR and VARR: bytes of one element, else 0 */
} RambleSchemaFieldInfo;

/* One enum option. */
typedef struct {
    int64_t     value;
    const char *name;     /* NUL terminated builder input, at most 255 bytes */
} RambleEnumVariant;

/* The builder grows its wire buffer through the hook as fields are added. finish returns
 * the compiled schema, or NULL after any latched error. Always finish a begun builder. */
typedef struct {
    RambleAllocFn alloc; void *user;
    uint8_t *buf;
    size_t   cap;
    size_t   len;                                /* wire bytes written so far */
    int      err;                                /* 0 ok, nonzero latches failure */
    uint8_t  value_root;                         /* a bare root: 1 awaits its type, 2 written */
    uint8_t  raw_type;                           /* internal: type bytes only, no header */
    uint16_t base_depth;                         /* the struct depth the root sits at */
    uint16_t arr_depth;                          /* the open array element depth, 0xFFFF = none */
    uint16_t depth;                              /* open structs, 1 = a struct root only */
    size_t   count_pos[RAMBLE_SCHEMA_MAX_DEPTH]; /* wire offset of each open struct's nfields byte */
    uint16_t field_count[RAMBLE_SCHEMA_MAX_DEPTH];
} RambleSchemaBuilder;

/* Compiles DSL text, pasted verbatim on the writer and every reader. The DSL is in
 * docs/stdtypes.md and spec/schema.md. NULL on error, with *err at the offending character. */
RAMBLE_API RambleSchema *ramble_schema_compile(RambleAllocFn alloc, void *user, const char *text, const char **err);
/* compile with an environment: each env schema is referenceable by its root name. An
 * anonymous env root is ignored. */
RAMBLE_API RambleSchema *ramble_schema_compile_env(RambleAllocFn alloc, void *user, const char *text,
                                             const RambleSchema *const *env, size_t n_env,
                                             const char **err);

RAMBLE_API RambleSchemaBuilder ramble_schema_begin(RambleAllocFn alloc, void *user, const char *root_name);
/* A bare type root: add exactly one field with an empty name, then finish. */
RAMBLE_API RambleSchemaBuilder ramble_schema_begin_value(RambleAllocFn alloc, void *user);
/* A named alias root, like Uuid = u8[16]: one empty named field, then finish. */
RAMBLE_API RambleSchemaBuilder ramble_schema_begin_alias(RambleAllocFn alloc, void *user, const char *name);
/* A fixed scalar field. */
RAMBLE_API void        ramble_schema_field(RambleSchemaBuilder *b, const char *name, RambleSchemaTypeKind kind);
/* A fixed array of count scalars. */
RAMBLE_API void        ramble_schema_field_array(RambleSchemaBuilder *b, const char *name,
                                             RambleSchemaTypeKind elem_scalar, uint16_t count);
/* A capped string, cap at least 1. */
RAMBLE_API void        ramble_schema_field_string(RambleSchemaBuilder *b, const char *name, uint16_t cap);
/* A fixed array of count capped strings. */
RAMBLE_API void        ramble_schema_field_string_array(RambleSchemaBuilder *b, const char *name,
                                                    uint16_t cap, uint16_t count);
/* A variable string. Any depth outside an array element. */
RAMBLE_API void        ramble_schema_field_var_string(RambleSchemaBuilder *b, const char *name);
/* A variable array of scalars. */
RAMBLE_API void        ramble_schema_field_var_array(RambleSchemaBuilder *b, const char *name,
                                                 RambleSchemaTypeKind elem_scalar);
/* A variable array of capped strings. */
RAMBLE_API void        ramble_schema_field_var_string_array(RambleSchemaBuilder *b, const char *name,
                                                        uint16_t cap);
/* A self describing map. Write it with RambleMapWriter, read it with ramble_map_get. */
RAMBLE_API void        ramble_schema_field_map(RambleSchemaBuilder *b, const char *name);
/* A named integer with an integer backing and n options, n at most 65535. Latches an
 * error on a non integer backing or a value that does not fit it. */
RAMBLE_API void        ramble_schema_field_enum(RambleSchemaBuilder *b, const char *name,
                                            RambleSchemaTypeKind backing,
                                            const RambleEnumVariant *variants, uint16_t n);
/* A field of a compiled named type. type need not outlive the call. */
RAMBLE_API void        ramble_schema_field_named(RambleSchemaBuilder *b, const char *name,
                                             const RambleSchema *type);
/* An array of a named type, variable when count is 0. The element must be fixed. */
RAMBLE_API void        ramble_schema_field_named_array(RambleSchemaBuilder *b, const char *name,
                                                   const RambleSchema *type, uint16_t count);
/* Opens a nested struct. Add its fields, then end_struct. */
RAMBLE_API void        ramble_schema_begin_struct(RambleSchemaBuilder *b, const char *name);
/* An array of anonymous structs, variable when count is 0. Add the element's fields,
 * then end_struct. The element must be fixed. */
RAMBLE_API void        ramble_schema_begin_struct_array(RambleSchemaBuilder *b, const char *name,
                                                    uint16_t count);
RAMBLE_API void        ramble_schema_end_struct(RambleSchemaBuilder *b);
/* Closes the root, compiles, and returns the schema. NULL on any latched error. */
RAMBLE_API RambleSchema *ramble_schema_finish(RambleSchemaBuilder *b);

/* Compiles received wire, bounds checked. NULL on an overrun or a version mismatch. The
 * bytes are copied in. */
RAMBLE_API RambleSchema *ramble_schema_parse(const void *wire, size_t wire_len, RambleAllocFn alloc, void *user);
/* The same hook the schema was made with. */
RAMBLE_API void        ramble_schema_free(RambleSchema *s, RambleAllocFn alloc, void *user);
/* An owned copy of any schema, safe to keep and to hand to create. */
RAMBLE_API RambleSchema *ramble_schema_copy(const RambleSchema *s, RambleAllocFn alloc, void *user);

RAMBLE_API RambleBytes   ramble_schema_wire(const RambleSchema *s);        /* the canonical bytes to advertise */
RAMBLE_API uint64_t      ramble_schema_hash(const RambleSchema *s);        /* FNV-1a over the wire */
RAMBLE_API RambleString  ramble_schema_name(const RambleSchema *s);   /* the root name, "" if anonymous */
/* Spells the schema back as DSL text, the inverse of compile. Returns the full length
 * excluding the NUL, so a NULL buffer measures. */
RAMBLE_API uint32_t    ramble_schema_print(const RambleSchema *s, char *buf, size_t cap);
/* The fixed section size, where the variable tail starts. */
RAMBLE_API uint32_t    ramble_schema_size(const RambleSchema *s);
/* The smallest valid message: the fixed section plus one empty frame per variable field. */
RAMBLE_API uint32_t    ramble_schema_msg_min(const RambleSchema *s);
/* The live length to send. 0 on a malformed or uninitialized buffer. */
RAMBLE_API uint32_t    ramble_schema_msg_len(const RambleSchema *s, const void *buf, size_t cap);
/* The flat depth first table. A bare root is one field named "". */
RAMBLE_API uint16_t    ramble_schema_field_count(const RambleSchema *s);
/* 1 and fills out, else 0 */
RAMBLE_API int         ramble_schema_field_at(const RambleSchema *s, uint16_t i, RambleSchemaFieldInfo *out);
/* Resolves a dotted or indexed path ("velocity.dx", "corners[2].x") to the flat index, or -1. */
RAMBLE_API int         ramble_schema_field_index(const RambleSchema *s, const char *path);
/* One field's type encoding, a view into the wire. Two fields have the same type exactly
 * when these agree. {NULL,0} for an unknown index. */
RAMBLE_API RambleBytes   ramble_schema_field_type_wire(const RambleSchema *s, uint16_t field);

/* The options of an enum field by flat index. count is 0 for a non enum field. */
RAMBLE_API uint16_t    ramble_schema_enum_count  (const RambleSchema *s, uint16_t field);
RAMBLE_API int         ramble_schema_enum_variant(const RambleSchema *s, uint16_t field, uint16_t i,
                                              int64_t *value, RambleString *name);
/* Resolution over the schema alone. name_of gives {NULL,0} for an unknown number, which
 * is safe, not an error. value_of returns 1 and *out for a known name. */
RAMBLE_API RambleString  ramble_enum_name_of (const RambleSchema *s, uint16_t field, int64_t value);
RAMBLE_API int           ramble_enum_value_of(const RambleSchema *s, uint16_t field, const char *name, int64_t *out);

/* 1 if msg is exactly one message of this schema. */
RAMBLE_API int       ramble_schema_validate(const RambleSchema *s, RambleBytes msg);

/* 1 if a reader declaring sub can read messages written with pub. The rules are in
 * spec/schema.md. */
RAMBLE_API int ramble_schema_subset(const RambleSchema *sub, const RambleSchema *pub);
/* The same, with the first incompatibility written to buf as one line on refusal. */
RAMBLE_API int ramble_schema_subset_why(const RambleSchema *sub, const RambleSchema *pub,
                                    char *buf, size_t cap);
/* The reader's fields with the writer's offsets and size. Requires subset, NULL
 * otherwise or on OOM. Free with ramble_schema_free. */
RAMBLE_API RambleSchema *ramble_schema_rebase(const RambleSchema *sub, const RambleSchema *pub,
                                        RambleAllocFn alloc, void *user);

/* Getters by name, nested members by dotted path and array members by indexed path,
 * widened. A mismatch or unknown field yields 0. */
RAMBLE_API uint64_t  ramble_get_uint(RambleBytes msg, const RambleSchema *s, const char *field);  /* and BOOL */
RAMBLE_API int64_t   ramble_get_int (RambleBytes msg, const RambleSchema *s, const char *field);  /* I8 to I64 */
RAMBLE_API double    ramble_get_f64 (RambleBytes msg, const RambleSchema *s, const char *field);  /* F64 or F32 */
RAMBLE_API float     ramble_get_f32 (RambleBytes msg, const RambleSchema *s, const char *field);  /* F32 */
/* ARR: the whole array. VARR: the live elements, a whole multiple of the element size. */
RAMBLE_API RambleBytes ramble_get_array(RambleBytes msg, const RambleSchema *s, const char *field);
/* STR or VSTR: the live bytes. A hostile length prefix is clamped to the cap. */
RAMBLE_API RambleString ramble_get_string(RambleBytes msg, const RambleSchema *s, const char *field);
/* One element of a string array, {NULL,0} past the live count. */
RAMBLE_API RambleString ramble_get_string_at(RambleBytes msg, const RambleSchema *s, const char *field,
                                       uint16_t index);
/* The map body, to feed to ramble_map_get. An empty body is an empty map. */
RAMBLE_API RambleBytes ramble_get_map(RambleBytes msg, const RambleSchema *s, const char *field);
/* The current option name, {NULL,0} if the stored number has no option. */
RAMBLE_API RambleString ramble_get_enum(RambleBytes msg, const RambleSchema *s, const char *field);
/* The live element count of a struct array, 0 if the field is not an array. */
RAMBLE_API uint32_t  ramble_get_array_count(RambleBytes msg, const RambleSchema *s, const char *field);

/* The canonical default message: zeros and one empty frame per variable field. Always
 * start from this, since the variable setters rely on every frame existing. */
RAMBLE_API int ramble_schema_message_default(const RambleSchema *s, void *buf, size_t cap);

/* Setters by name. Values narrow like a C cast. A variable setter resizes its frame in
 * place and returns 0 when the message would exceed cap. 1 ok, 0 on a mismatch. */
/* set_uint also takes BOOL */
RAMBLE_API int ramble_set_uint(void *buf, size_t cap, const RambleSchema *s, const char *field, uint64_t v);
RAMBLE_API int ramble_set_int (void *buf, size_t cap, const RambleSchema *s, const char *field, int64_t v);
/* set_f64 also takes an F32 field */
RAMBLE_API int ramble_set_f64 (void *buf, size_t cap, const RambleSchema *s, const char *field, double v);
RAMBLE_API int ramble_set_f32 (void *buf, size_t cap, const RambleSchema *s, const char *field, float v);
/* ARR: elems over the front, the rest zeroed. VARR: the frame becomes elems. Whole
 * elements only, never silently truncated. */
RAMBLE_API int ramble_set_array(void *buf, size_t cap, const RambleSchema *s, const char *field, RambleBytes elems);
/* Grows or shrinks a variable array, zero filling new elements. */
RAMBLE_API int ramble_set_array_count(void *buf, size_t cap, const RambleSchema *s, const char *field,
                                  uint32_t count);
/* STR: the live bytes, the slot tail zeroed, v.len must fit the cap. VSTR: the frame becomes v. */
RAMBLE_API int ramble_set_string(void *buf, size_t cap, const RambleSchema *s, const char *field, RambleString v);
/* One element of a string array, under the live count for a variable one. */
RAMBLE_API int ramble_set_string_at(void *buf, size_t cap, const RambleSchema *s, const char *field,
                                uint16_t index, RambleString v);
/* The frame becomes the map body, validated first. */
RAMBLE_API int ramble_set_map(void *buf, size_t cap, const RambleSchema *s, const char *field, RambleBytes map);
/* Writes the option by name, 0 for an unknown one. */
RAMBLE_API int ramble_set_enum(void *buf, size_t cap, const RambleSchema *s, const char *field, const char *name);

/* One tagged value for reflection by flat index. */
typedef struct {
    uint8_t  kind;        /* RambleSchemaTypeKind */
    uint8_t  elem;        /* the array element kind, or the enum backing kind */
    uint16_t count;       /* ARR count, VARR live count, MAP entries (saturated), ENUM variants */
    uint16_t str_cap;     /* string capacity for STR fields and STR elements */
    union { uint64_t u; int64_t i; double f; } v;   /* the scalar value, BOOL in u, ENUM in i */
    RambleBytes bytes;      /* the raw bytes, the live string, the live elements or the map body */
} RambleValue;
RAMBLE_API int ramble_get_value(RambleBytes msg, const RambleSchema *s, uint16_t field, RambleValue *out);
RAMBLE_API int ramble_set_value(void *buf, size_t cap, const RambleSchema *s, uint16_t field, const RambleValue *val);
/* The same under a struct array, elem selects the element. Ignored for other fields. */
RAMBLE_API int ramble_get_value_at(RambleBytes msg, const RambleSchema *s, uint16_t field, uint32_t elem,
                               RambleValue *out);
RAMBLE_API int ramble_set_value_at(void *buf, size_t cap, const RambleSchema *s, uint16_t field, uint32_t elem,
                               const RambleValue *val);
/* The live count of the struct array at flat index field, 0 if it is not an array. */
RAMBLE_API uint32_t ramble_array_count_at(RambleBytes msg, const RambleSchema *s, uint16_t field);

/* The map is a self describing tagged value tree, the escape from the schema. Its body
 * grammar is in spec/schema.md. The writer fills a caller buffer with no allocation. */
typedef struct {
    uint8_t *buf; size_t cap, len;               /* the caller's buffer and write position */
    int      err;                                /* 0 ok, nonzero latches failure */
    uint16_t depth;                              /* open bodies, 1 = the root map only */
    size_t   count_pos[RAMBLE_SCHEMA_MAX_DEPTH]; /* offset of each open body's count */
    uint16_t count[RAMBLE_SCHEMA_MAX_DEPTH];
    uint8_t  is_arr[RAMBLE_SCHEMA_MAX_DEPTH];
} RambleMapWriter;

RAMBLE_API RambleMapWriter ramble_map_begin(void *buf, size_t cap);   /* opens the root map */
RAMBLE_API int ramble_map_put_uint  (RambleMapWriter *w, const char *key, uint64_t v);
RAMBLE_API int ramble_map_put_int   (RambleMapWriter *w, const char *key, int64_t v);
RAMBLE_API int ramble_map_put_f64   (RambleMapWriter *w, const char *key, double v);
RAMBLE_API int ramble_map_put_f32   (RambleMapWriter *w, const char *key, float v);
RAMBLE_API int ramble_map_put_bool  (RambleMapWriter *w, const char *key, int v);
RAMBLE_API int ramble_map_put_string(RambleMapWriter *w, const char *key, RambleString v);
RAMBLE_API int ramble_map_open_map  (RambleMapWriter *w, const char *key);   /* a nested map, close it */
RAMBLE_API int ramble_map_open_array(RambleMapWriter *w, const char *key);   /* elements pass key NULL */
RAMBLE_API int ramble_map_close     (RambleMapWriter *w);                    /* closes the innermost open body */
RAMBLE_API uint32_t ramble_map_finish(RambleMapWriter *w);                   /* the body length, 0 on any error */

/* Readers over a possibly hostile body: every walk is bounds checked and a nested value
 * comes back in RambleValue.bytes ready for these same functions. */
RAMBLE_API uint16_t ramble_map_count(RambleBytes map);                       /* 0 if empty or short */
RAMBLE_API int ramble_map_at (RambleBytes map, uint16_t index, RambleString *key, RambleValue *out);
RAMBLE_API int ramble_map_get(RambleBytes map, const char *key, RambleValue *out);   /* 1 and fills out, else 0 */
RAMBLE_API uint16_t ramble_map_array_count(RambleBytes arr);
RAMBLE_API int ramble_map_array_at(RambleBytes arr, uint16_t index, RambleValue *out);
RAMBLE_API int ramble_map_valid(RambleBytes map);   /* the full structural check, ramble_set_map runs it */

#ifdef __cplusplus
}
#endif
#endif /* RAMBLE_SCHEMA_H */
#pragma endregion
#ifndef RAMBLE_NO_STDTYPES
#pragma region serialize/stdtypes.h
/* The standard type library: named types always in scope in the DSL, with layout
 * identical C mirrors. docs/stdtypes.md has the roster and conventions. */
#ifndef RAMBLE_STDTYPES_H
#define RAMBLE_STDTYPES_H


#ifdef __cplusplus
extern "C" {
#endif

/* The roster in table order. RAMBLE_STD_NONE is not a standard type. */
typedef enum {
    RAMBLE_STD_NONE = 0,
    RAMBLE_STD_FLOAT2, RAMBLE_STD_FLOAT3, RAMBLE_STD_FLOAT4,
    RAMBLE_STD_DOUBLE2, RAMBLE_STD_DOUBLE3, RAMBLE_STD_DOUBLE4,
    RAMBLE_STD_INT2, RAMBLE_STD_INT3, RAMBLE_STD_INT4,
    RAMBLE_STD_QUATERNION,
    RAMBLE_STD_COLOR,
    RAMBLE_STD_RECT, RAMBLE_STD_RECTI,
    RAMBLE_STD_TRANSFORM, RAMBLE_STD_TWIST,
    RAMBLE_STD_GEOPOINT,
    RAMBLE_STD_UUID,
    RAMBLE_STD_TIMESTAMP, RAMBLE_STD_DURATION,
    RAMBLE_STD_MATRIX3X3, RAMBLE_STD_MATRIX4X4,
    RAMBLE_STD_URI,
    RAMBLE_STD_IMAGE, RAMBLE_STD_VIDEOFRAME, RAMBLE_STD_EXTERNALVIDEOSTREAM,
    RAMBLE_STD_CAMERAINTRINSICS, RAMBLE_STD_JOINTSTATE, RAMBLE_STD_JOINTNAMES,
    RAMBLE_STD_COUNT
} RambleStdType;

/* The type's name and its canonical spelling, the two halves of the definition the DSL
 * knows. NULL for RAMBLE_STD_NONE or an out of range value. */
RAMBLE_API const char *ramble_std_name(RambleStdType t);
RAMBLE_API const char *ramble_std_text(RambleStdType t);
/* A name lookup only. Use ramble_std_recognize when a peer's shape must be verified too. */
RAMBLE_API RambleStdType ramble_std_by_name(RambleString name);

/* One standard type compiled as a schema of its own, for a topic whose payload is one. */
RAMBLE_API RambleSchema *ramble_std_schema(RambleStdType t, RambleAllocFn alloc, void *user);

/* Recognizes a standard type in a schema we did not write: the name and the shape must
 * both match. The result is stable per schema, so cache it. */
RAMBLE_API RambleStdType ramble_std_recognize(const RambleSchema *s, RambleAllocFn alloc, void *user);
RAMBLE_API RambleStdType ramble_std_recognize_field(const RambleSchema *s, uint16_t field,
                                              RambleAllocFn alloc, void *user);
RAMBLE_API RambleStdType ramble_std_recognize_elem(const RambleSchema *s, uint16_t field,
                                             RambleAllocFn alloc, void *user);

/* The C mirrors, layout identical to the wire on a little endian target. */
typedef struct { float  x, y;       } RambleFloat2;
typedef struct { float  x, y, z;    } RambleFloat3;
typedef struct { float  x, y, z, w; } RambleFloat4;
typedef struct { double x, y;       } RambleDouble2;
typedef struct { double x, y, z;    } RambleDouble3;
typedef struct { double x, y, z, w; } RambleDouble4;
typedef struct { int32_t x, y;       } RambleInt2;
typedef struct { int32_t x, y, z;    } RambleInt3;
typedef struct { int32_t x, y, z, w; } RambleInt4;
typedef struct { double x, y, z, w; } RambleQuaternion;   /* x, y, z, w in that order */
typedef struct { uint8_t r, g, b, a; } RambleColor;       /* sRGB, straight alpha */
typedef struct { float   x, y, w, h; } RambleRect;
typedef struct { int32_t x, y, w, h; } RambleRectI;
typedef struct { RambleDouble3 translation; RambleQuaternion rotation;
                 uint16_t parent_len; char parent[30]; } RambleTransform;
typedef struct { RambleDouble3 linear, angular; } RambleTwist;   /* m/s and rad/s */
typedef struct { double lat, lon, alt; } RambleGeoPoint;         /* degrees, degrees, meters */
typedef struct { uint8_t bytes[16]; } RambleUuid;                /* RFC 4122 byte order */
typedef int64_t RambleTimestamp;   /* microseconds since the Unix epoch, UTC */
typedef int64_t RambleDuration;                                /* microseconds */
typedef struct { float m[9];  } RambleMatrix3x3;               /* row major */
typedef struct { float m[16]; } RambleMatrix4x4;               /* row major */

/* Image.format, VideoFrame.codec and ExternalVideoStream.kind as the schema declares
 * them. A format of 16 or more is a compressed container. */
typedef enum {
    RAMBLE_IMAGE_MONO8 = 0, RAMBLE_IMAGE_MONO16 = 1, RAMBLE_IMAGE_RGB8 = 2, RAMBLE_IMAGE_RGBA8 = 3,
    RAMBLE_IMAGE_BGR8 = 4, RAMBLE_IMAGE_YUYV = 5, RAMBLE_IMAGE_NV12 = 6,
    RAMBLE_IMAGE_MONOF32 = 7,
    RAMBLE_IMAGE_JPEG = 16, RAMBLE_IMAGE_PNG = 17
} RambleImageFormat;
typedef enum {
    RAMBLE_VIDEO_UNKNOWN = 0, RAMBLE_VIDEO_MJPEG = 1, RAMBLE_VIDEO_H264 = 2,
    RAMBLE_VIDEO_H265 = 3, RAMBLE_VIDEO_AV1 = 4
} RambleVideoCodec;
typedef enum {
    RAMBLE_DISTORTION_NONE = 0, RAMBLE_DISTORTION_BROWN_CONRADY = 1,
    RAMBLE_DISTORTION_FISHEYE = 2, RAMBLE_DISTORTION_RATIONAL = 3
} RambleDistortionModel;
typedef enum {
    RAMBLE_STREAM_RTSP = 0, RAMBLE_STREAM_WEBRTC_WHEP = 1, RAMBLE_STREAM_HLS = 2,
    RAMBLE_STREAM_SRT = 3, RAMBLE_STREAM_RTP = 4, RAMBLE_STREAM_HTTP_MJPEG = 5,
    RAMBLE_STREAM_OTHER = 15
} RambleStreamKind;

/* The layout pins. A mirror that ever gained padding would silently mis decode. */
typedef char i_ramble_std_size_check[
      (sizeof(RambleFloat3) == 12 && sizeof(RambleFloat4) == 16 &&
       sizeof(RambleDouble3) == 24 && sizeof(RambleQuaternion) == 32 &&
       sizeof(RambleColor) == 4 && sizeof(RambleRect) == 16 &&
       sizeof(RambleTransform) == 88 && sizeof(RambleTwist) == 48 &&
       sizeof(RambleUuid) == 16 && sizeof(RambleMatrix4x4) == 64) ? 1 : -1];

/* The thin operations, header only. */
static inline RambleFloat3 ramble_float3(float x, float y, float z){
    RambleFloat3 v; v.x = x; v.y = y; v.z = z; return v;
}
static inline RambleDouble3 ramble_double3(double x, double y, double z){
    RambleDouble3 v; v.x = x; v.y = y; v.z = z; return v;
}
static inline RambleDouble3 ramble_double3_add(RambleDouble3 a, RambleDouble3 b){
    return ramble_double3(a.x + b.x, a.y + b.y, a.z + b.z);
}
static inline RambleDouble3 ramble_double3_sub(RambleDouble3 a, RambleDouble3 b){
    return ramble_double3(a.x - b.x, a.y - b.y, a.z - b.z);
}
static inline RambleDouble3 ramble_double3_scale(RambleDouble3 a, double k){
    return ramble_double3(a.x * k, a.y * k, a.z * k);
}
static inline double ramble_double3_dot(RambleDouble3 a, RambleDouble3 b){
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static inline RambleDouble3 ramble_double3_cross(RambleDouble3 a, RambleDouble3 b){
    return ramble_double3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
RAMBLE_API double          ramble_double3_length(RambleDouble3 a);        /* needs a square root, in stdtypes.c */
RAMBLE_API RambleDouble3   ramble_double3_normalize(RambleDouble3 a);     /* the zero vector maps to itself */

static inline RambleQuaternion ramble_quaternion(double x, double y, double z, double w){
    RambleQuaternion q; q.x = x; q.y = y; q.z = z; q.w = w; return q;
}
static inline RambleQuaternion ramble_quaternion_identity(void){
    return ramble_quaternion(0.0, 0.0, 0.0, 1.0);
}
static inline RambleQuaternion ramble_quaternion_conjugate(RambleQuaternion q){
    return ramble_quaternion(-q.x, -q.y, -q.z, q.w);
}
/* The rotation a followed by b, the Hamilton product b times a. */
static inline RambleQuaternion ramble_quaternion_mul(RambleQuaternion a, RambleQuaternion b){
    return ramble_quaternion(
        b.w * a.x + b.x * a.w + b.y * a.z - b.z * a.y,
        b.w * a.y - b.x * a.z + b.y * a.w + b.z * a.x,
        b.w * a.z + b.x * a.y - b.y * a.x + b.z * a.w,
        b.w * a.w - b.x * a.x - b.y * a.y - b.z * a.z);
}
RAMBLE_API RambleQuaternion ramble_quaternion_normalize(RambleQuaternion q);
RAMBLE_API RambleDouble3    ramble_quaternion_rotate(RambleQuaternion q, RambleDouble3 v);

static inline RambleTransform ramble_transform_identity(void){
    RambleTransform t = {{0}};   /* zero fills the parent name too, it reaches the wire */
    t.rotation = ramble_quaternion_identity(); return t;
}
static inline RambleColor ramble_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a){
    RambleColor c; c.r = r; c.g = g; c.b = b; c.a = a; return c;
}
/* 0xRRGGBBAA both ways, the CSS order with the alpha last. */
static inline RambleColor ramble_color_from_hex(uint32_t rgba){
    return ramble_color((uint8_t)(rgba >> 24), (uint8_t)(rgba >> 16),
                      (uint8_t)(rgba >> 8),  (uint8_t)rgba);
}
static inline uint32_t ramble_color_to_hex(RambleColor c){
    return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) | ((uint32_t)c.b << 8) | c.a;
}
/* Timestamp arithmetic in microseconds. */
static inline RambleDuration  ramble_timestamp_diff(RambleTimestamp a, RambleTimestamp b){ return a - b; }
static inline RambleTimestamp ramble_timestamp_add (RambleTimestamp t, RambleDuration d){ return t + d; }
static inline RambleDuration  ramble_duration_ms   (int64_t ms){ return ms * 1000; }
static inline RambleDuration  ramble_duration_s    (double s){ return (int64_t)(s * 1000000.0); }
static inline double          ramble_duration_to_s (RambleDuration d){ return (double)d / 1000000.0; }
/* All zeroes, the conventional unset. */
static inline int ramble_uuid_is_nil(RambleUuid u){
    int i; for (i = 0; i < 16; i++) if (u.bytes[i]) return 0;
    return 1;
}

#ifdef __cplusplus
}
#endif
#endif /* RAMBLE_STDTYPES_H */
#pragma endregion
#endif /* !RAMBLE_NO_STDTYPES */
#pragma region node/core.h
/* The sans-IO node core: the peer table, the discovery to transport lifecycle and
 * address resolution. A runtime owns the IO and drives it. The rules are in spec/node.md. */
#ifndef RAMBLE_NODE_CORE_H
#define RAMBLE_NODE_CORE_H


#ifdef __cplusplus
extern "C" {
#endif

/* The node's event. Four lifecycle kinds plus RAMBLE_ERROR, whose .error says which fault.
 * Read only the fields named for the kind. ramble_event_str formats any of them. */
typedef enum {
    RAMBLE_PEER_UP,        /* discovered or resumed: .peer, .ip, .ip_len, .port */
    RAMBLE_PEER_DOWN,      /* lost or fell silent: .peer */
    RAMBLE_PEER_INTEREST,  /* interest applied: .peer, .publish_topics, .receive_topics */
    RAMBLE_MSG_LOST,       /* seqnos skipped: .topic, .peer, .lost_first, .lost_count. Not an error */
    RAMBLE_ERROR           /* read .error and ramble_event_str */
} RambleEventKind;

/* The error carried by a RAMBLE_ERROR event and returned by ramble_last_error. Named RAMBLE_E_*
 * to stay distinct from the RambleResult return codes. docs/node.md lists them. */
typedef enum {
    RAMBLE_E_NONE = 0,
    /* a match was refused, or advertised data cannot flow */
    RAMBLE_E_NAME_COLLISION,   /* a peer's topic name hashes to ours but differs: .identity, .topic */
    RAMBLE_E_QOS_INCOMPATIBLE, /* a reliable subscriber met a best effort publisher: .topic, .peer */
    RAMBLE_E_KIND_MISMATCH,    /* the name is another entity kind at a peer: .topic, .peer */
    RAMBLE_E_SCHEMA_MISMATCH,  /* a refused match or an ill fitting message: .topic, .peer */
    RAMBLE_E_INTEREST_OVERFLOW,/* a peer's index map failed to allocate: .peer, .lost_count entries */
    RAMBLE_E_META_TRUNCATED_INTEREST, /* our overlay overflowed, peers see no topics */
    RAMBLE_E_META_TRUNCATED_SCHEMA,   /* retired, kept so binding enums stay aligned */
    RAMBLE_E_PEER_META_TOO_BIG,/* a peer's blob exceeds our buffer: .peer (0 = new), .too_big_bytes */
    RAMBLE_E_MSG_TOO_BIG,      /* a received message could not be buffered: .too_big_bytes */
    RAMBLE_E_PEER_REFUSED,     /* the peer table is full of active peers: .ip, .ip_len, .port */
    RAMBLE_E_EVICTED_UNSENT,   /* a send overwrote unsent history: .topic, .lost_first, .lost_count */
    RAMBLE_E_UNMATCHED_SEND,   /* a send committed to nobody while a match was resolving: .topic */
    RAMBLE_E_DUPLICATE_AUTHORITY, /* a peer also claims the handler side: .topic, .peer. Diagnostic */
    /* IO and setup, mostly at open. .os_error carries the OS code */
    RAMBLE_E_OOM,              /* the allocator returned NULL: .too_big_bytes = bytes needed */
    RAMBLE_E_PLATFORM,         /* platform net init failed */
    RAMBLE_E_SOCKET,           /* opening a UDP socket failed */
    RAMBLE_E_BIND,             /* bind failed, the port is in use: .port */
    RAMBLE_E_MCAST_JOIN,       /* joining the discovery group failed, a bad interface */
    RAMBLE_E_SEND,             /* a send hard failed: .peer, .topic, .too_big_bytes = its size */
    RAMBLE_E_RECV,             /* a receive hard failed */
    RAMBLE_E_POLL,             /* the socket wait failed */
    RAMBLE_E_WAKER,            /* no cross thread waker, wakes come at the next tick */
    RAMBLE_E_BAD_ADDRESS       /* a configured address could not be parsed, refused at open */
} RambleErrorKind;

typedef struct {
    RambleEventKind kind;
    RambleErrorKind error;       /* RAMBLE_ERROR: which error, else RAMBLE_E_NONE */
    const char *topic_name;  /* topic scoped events: our topic's name, valid for the callback */
    void       *user;          /* RambleNodeOpts.user_data */
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
} RambleEvent;
typedef void (*RambleEventFn)(const RambleEvent *ev);

/* Formats ev as one line into buf, always NUL terminated. Returns buf. RAMBLE_NO_DIAG
 * compiles the text out and yields "error N". */
RAMBLE_API const char *ramble_event_str(const RambleEvent *ev, char *buf, size_t cap);

/* reflection types, the walks are in node/runtime.h */
typedef struct { uint32_t a, b; uint16_t c, d; } RambleIter;   /* walk state, zero initialize */

#define RAMBLE_SELF 0u   /* the peer id that means this node */

typedef struct {
    uint32_t           id;             /* stable across a drop and return, never RAMBLE_SELF */
    uint8_t            uuid[16];       /* the process instance, a restart is a new uuid */
    RambleString       name;
    RambleString       address;        /* "ip:port" */
    RamblePeerLiveness liveness;
    uint64_t           last_heard_us;
    uint32_t           epoch;          /* bumps on every reflected change at this peer */
    uint8_t            catching_up;    /* 1 = it advertises newer state than we hold */
    uint16_t           fragment_size;  /* its advertised UDP fragment size */
    /* the round trip as our reliable traffic measured it, samples 0 = no estimate yet */
    uint32_t         rtt_us;
    uint32_t         rtt_jitter_us;
    uint32_t         rtt_min_us;
    uint32_t         rtt_samples;
} RamblePeerInfo;

typedef enum {
    RAMBLE_ENTITY_TOPIC = 0,
    RAMBLE_ENTITY_FUNCTION,
    RAMBLE_ENTITY_VARIABLE,
    RAMBLE_ENTITY_TASK
} RambleEntityKind;

typedef struct {
    RambleEntityKind    kind;
    RambleString        name;          /* the base name, {NULL,0} until the details arrive */
    uint32_t            hash;          /* the primary channel's low 32 name hash, the placeholder */
    uint8_t             provides;      /* someone live is on the source side */
    uint8_t             consumes;      /* someone live is on the sink side */
    uint8_t             reliable;      /* the primary channel's reliability */
    uint8_t             writable;      /* VARIABLE: a set channel is advertised */
    uint8_t             forceable;     /* VARIABLE: the owner permits force */
    uint8_t             cancellable;   /* TASK: the provider honors cancel */
    uint8_t             exclusive;     /* TASK: declared serialization */
    uint8_t             multi;         /* duplicate authority is intended */
    uint8_t             incomplete;    /* a pattern half pair, surfaced rather than dropped */
    uint8_t             conflict;      /* MESH: live schemas that cannot read each other */
    uint16_t            providers;     /* live endpoints per side, a per node walk reports 0 or 1 */
    uint16_t            consumers;
    uint32_t            provider;      /* the ranked provider's peer id, RAMBLE_SELF is this node */
    RambleString        from;          /* the node the schemas below were read from */
    const RambleSchema *schema;        /* value, request or payload, NULL = untyped or unfetched */
    uint64_t          schema_hash;
    const RambleSchema *rsp_schema;    /* FUNCTION and TASK: the response */
    uint64_t          rsp_schema_hash;
    const RambleSchema *progress_schema;   /* TASK: the progress channel */
    uint64_t          progress_schema_hash;
    uint64_t          generation;    /* changes iff the provider, any schema or an attr changed */
} RambleEntityInfo;

/* What the core needs from the runtime, set once at init. The peer table lives in the
 * discovery core, which also holds the core's per peer lifecycle scratch. */
typedef struct {
    RambleTransportState           *transport;
    RambleDiscoveryState  *discovery;    /* may be NULL at init, bound later */
    uint16_t              n_topics;      /* sizes the per topic arrays */
    uint16_t              frag_size;     /* our fragment size, baked into the overlay */
    RambleEventFn         on_event;      /* optional */
    void                 *user;
    RambleAllocFn           alloc;         /* required, backs the schema state and the blob */
    void                 *alloc_user;
    int                   oob_capable;   /* 1 = we can deliver shared memory payloads */
    uint8_t               oob_host[16];  /* our host id, a peer is reachable iff it matches */
    uint8_t               fetch_details; /* observer mode: fetch and cache every advertised index */
} i_RambleNodeCoreConfig;

typedef struct i_RambleNodeCore i_RambleNodeCore;

/* The arena holds only the core struct and the per topic schema registry. */
size_t          i_ramble_node_core_required_memory(uint16_t n_topics);
i_RambleNodeCore *i_ramble_node_core_init(void *mem, size_t mem_size, const i_RambleNodeCoreConfig *cfg);
/* Relocates the core. The caller re points the transport, discovery and blob after. */
i_RambleNodeCore *i_ramble_node_core_migrate(i_RambleNodeCore *old, void *new_mem, size_t new_cap,
                                       uint16_t new_n_topics);
/* Binds the discovery core whose peer table this core delegates to, again after a migrate. */
void            i_ramble_node_core_bind_discovery(i_RambleNodeCore *c, RambleDiscoveryState *discovery);
/* Bytes of per peer scratch the core needs in the discovery peer table. */
uint16_t        i_ramble_node_core_peer_user_bytes(void);

/* The announce overlay this node sends. build rebuilds it from the core's fields and
 * returns the length, meta returns the bytes. Rebuild after a role change. */
uint16_t          i_ramble_node_core_build_meta(i_RambleNodeCore *c);
RambleBytes       i_ramble_node_core_meta(i_RambleNodeCore *c);
/* Registers a topic's schema, or clears it with NULL. schema must outlive the topic. */
void            i_ramble_node_core_set_topic_schema(i_RambleNodeCore *c, uint16_t topic_index,
                                                    const RambleSchema *schema);

/* Topic slot lifecycle. retire drops the live schema pointers but keeps the hash as the
 * slot's fingerprint, topic_rebound purges the old occupant's bindings. See spec/interest.md. */
void     i_ramble_node_core_retire_topic_schema(i_RambleNodeCore *c, uint16_t topic_index);
uint64_t i_ramble_node_core_topic_schema_hash(i_RambleNodeCore *c, uint16_t topic_index);
void     i_ramble_node_core_topic_rebound(i_RambleNodeCore *c, uint16_t topic_index);
/* Feeds a peer's request version to the transport's rebind hold. 1 when a held lane formed. */
int      i_ramble_node_core_seen_version(i_RambleNodeCore *c, uint32_t peer, uint32_t version);

/* Answers a peer's DETAIL_REQ, stateless. {NULL,0} when not answerable, the requester re
 * asks. The view is valid until the next call. */
RambleBytes i_ramble_node_core_detail_respond(i_RambleNodeCore *c, uint16_t domain, RambleBytes req);

/* Interest paging: interest_respond answers an INTEREST_REQ with one page, apply_interest_page
 * ingests one, and on completion applies the blob exactly as an inline announce would. */
RambleBytes i_ramble_node_core_interest_respond(i_RambleNodeCore *c, uint16_t domain, RambleBytes req);
void        i_ramble_node_core_apply_interest_page(i_RambleNodeCore *c, uint16_t domain, uint32_t peer,
                                      RambleBytes resp);

/* The transport's schema_check hook. The rules are in spec/schema.md. An allowed read side
 * check also records the reader view for delivery. */
int i_ramble_node_core_schema_check(i_RambleNodeCore *c, uint32_t peer, uint16_t topic_index,
                                  int peer_is_pub, uint64_t hash, RambleBytes wire);

/* Why the gate refused (peer, topic, direction), recorded at detail intake. NULL when
 * unknown or under RAMBLE_NO_DIAG. */
const char *i_ramble_node_core_schema_why(i_RambleNodeCore *c, uint32_t peer, uint16_t topic_index,
                                        int peer_is_pub);
/* Records and returns the reason for a delivery length mismatch. NULL under RAMBLE_NO_DIAG. */
const char *i_ramble_node_core_note_size_mismatch(i_RambleNodeCore *c, uint32_t peer,
                                        uint16_t topic_index, uint64_t got_len, uint64_t want_len);

/* The schema a delivered message decodes with: ours, a rebased view of the publisher's, the
 * publisher's own for an untyped topic, or NULL for raw. */
const RambleSchema *i_ramble_node_core_msg_schema(i_RambleNodeCore *c, uint32_t peer, uint16_t topic_index);

/* The discovery core's on_event sink, with cfg.user = this core. Keeps the peer table and
 * the transport peer set in lockstep and fires the app's peer events. */
void i_ramble_node_core_on_disc_event(const RambleDiscoveryEvent *ev);

/* A resolved outbound destination. The runtime turns it into wire bytes for its link. */
typedef struct {
    uint8_t  ip[16];     /* IPv4 today */
    uint8_t  ip_len;
    uint16_t port;
} i_RambleNodeDest;

/* resolve turns a peer id into an address, 1 if sendable. id_for_addr maps a source back
 * to a peer id, 1 on a hit. */
int  i_ramble_node_core_resolve(i_RambleNodeCore *c, uint32_t to, i_RambleNodeDest *out);
int  i_ramble_node_core_id_for_addr(i_RambleNodeCore *c, const uint8_t ip[4], uint16_t port, uint32_t *id);

/* The requester side of the uDTL cycle: apply_details ingests a response, detail_req_next
 * builds the next queued request until 0, detail_rearm re queues every active peer. */
void   i_ramble_node_core_apply_details(i_RambleNodeCore *c, uint16_t domain, uint32_t peer,
                                      RambleBytes resp);
int    i_ramble_node_core_detail_any(i_RambleNodeCore *c);
void   i_ramble_node_core_detail_rearm(i_RambleNodeCore *c);
size_t i_ramble_node_core_detail_req_next(i_RambleNodeCore *c, uint16_t domain,
                                        void *out, size_t cap, i_RambleNodeDest *to);
/* Unresolved candidate matches for one topic across every active peer. 0 = converged. */
int    i_ramble_node_core_topic_unresolved(i_RambleNodeCore *c, uint16_t topic_index);
/* The bulk form: every topic's count in one walk of each active peer's interest. */
void   i_ramble_node_core_topics_unresolved(i_RambleNodeCore *c, uint16_t *counts, uint16_t n);

/* A peer's name, a view into the peer table. "unknown-peer" for a known but unnamed peer,
 * .data NULL only for an unknown id. */
RambleString i_ramble_node_core_peer_name(i_RambleNodeCore *c, uint32_t id);

/* Read only peer table enumeration for diagnostics. Any out pointer may be NULL. */
uint16_t i_ramble_node_core_max_peers(i_RambleNodeCore *c);
int      i_ramble_node_core_peer_at(i_RambleNodeCore *c, uint16_t slot, uint32_t *id,
                                uint8_t ip[16], uint8_t *ip_len, uint16_t *port);

/* Reflection: the tables behind the runtime's walks. See spec/reflection.md. */
uint16_t i_ramble_node_peer_frag(const RambleDiscoveryPeer *peer);
uint32_t i_ramble_node_peer_interest_epoch(const RambleDiscoveryPeer *peer);
int      i_ramble_node_peer_interest_next(const RambleDiscoveryPeer *peer,
                              RambleInterestIter *it, RambleTopicEntry *out);
void     i_ramble_node_core_set_self_name(i_RambleNodeCore *c, RambleString name);
void     i_ramble_node_core_self_begin(i_RambleNodeCore *c);
void     i_ramble_node_core_self_channel(i_RambleNodeCore *c, uint16_t index, RambleString name,
                              uint8_t kind, uint8_t role, uint8_t reliable, uint8_t attrs,
                              const RambleSchema *schema);
void     i_ramble_node_core_self_end(i_RambleNodeCore *c);
int      i_ramble_node_core_peers_next(i_RambleNodeCore *c, RambleIter *it, RamblePeerInfo *out);
int      i_ramble_node_core_entities_next(i_RambleNodeCore *c, uint32_t peer, RambleIter *it,
                              RambleEntityInfo *out);
int      i_ramble_node_core_mesh_next(i_RambleNodeCore *c, RambleIter *it, RambleEntityInfo *out);
int      i_ramble_node_core_mesh_find(i_RambleNodeCore *c, RambleEntityKind kind, const char *name,
                              RambleEntityInfo *out);
uint32_t i_ramble_node_core_mesh_epoch(i_RambleNodeCore *c);
/* The create time pick for one channel (which: 0 primary, 1 rsp, 2 prg) of an entity. A
 * writer takes the widest schema every reader accepts, a reader the provider's. */
int      i_ramble_node_core_reflect_pick(i_RambleNodeCore *c, RambleEntityKind kind, const char *name,
                              int which, int writer, const RambleSchema **schema,
                              uint8_t *reliable, uint64_t *generation);

#ifdef __cplusplus
}
#endif
#endif /* RAMBLE_NODE_CORE_H */
#pragma endregion
#pragma region node/runtime.h
/* The node runtime and the public ramble_node_* and ramble_topic_* API. It owns the sockets
 * and the clock and drives the cores. docs/node.md explains how to use it. */
#ifndef RAMBLE_NODE_H
#define RAMBLE_NODE_H

#ifndef RAMBLE_NO_STDTYPES
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
    const RambleDiscoveryAddr *seed_peers;   /* unicast announces here too, port 0 = discovery_port */
    uint16_t              n_seed_peers;
    uint8_t               unicast_only;      /* 1 = no group join, seeds and relays only. Also for a
                                                node behind an outbound only NAT */
    uint32_t              recv_buffer_bytes; /* the data socket SO_RCVBUF, 0 = OS default */
    uint32_t              send_buffer_bytes; /* the data socket SO_SNDBUF, 0 = OS default */
    uint16_t              fragment_size;     /* UDP payload bytes per fragment, 0 = RAMBLE_FRAG_SIZE.
                                                Clamped to [MIN, MAX]. One size per node */
    /* Stating our own locator. The default states nothing and peers record the source an
     * announce arrived from. Use these for a static one to one mapping. */
    const char           *self_ip;           /* advertise this IPv4 address. Unparseable refuses the
                                                open with RAMBLE_E_BAD_ADDRESS */
    uint16_t              advertise_port;    /* advertise this data port instead of the bound one */
} RambleNodeNet;

/* Discovery cadence and the peer table size, zero means default. */
typedef struct {
    uint32_t              announce_interval_us; /* 3 s */
    uint32_t              peer_timeout_us;   /* drop a peer after this silence, 12 s */
    uint16_t              max_peers;         /* 16 */
} RambleNodeDiscovery;

#ifndef RAMBLE_MATCH_WAIT_MS
#define RAMBLE_MATCH_WAIT_MS 1000   /* the default send path match wait bound */
#endif

/* The node options, passed as a compound literal. NULL means all defaults. */
typedef struct {
    uint16_t              domain;
    uint16_t              max_topics;  /* topics that can be created, 0 = 8 */
    void                 *user_data;     /* RambleMsg.user and RambleEvent.user */
    uint8_t               disable_shm;   /* 1 = never use the same host shared memory path */
    uint8_t               fetch_details; /* 1 = fetch and cache every topic every peer advertises,
                                            for observer UIs. Costs memory per peer topic */
    int32_t               match_wait_ms; /* the send path match wait bound. 0 = RAMBLE_MATCH_WAIT_MS,
                                            negative = disabled and such a send fires UNMATCHED_SEND */
    uint8_t               disable_logs;  /* 1 = no @ramble/log topics, ramble_node_log gives NOSYS */
    uint8_t               disable_meta;  /* 1 = no built in @ramble/meta function */
    uint8_t               disable_error_logs; /* 1 = do not mirror errors onto @ramble/log/error */
    RambleNodeNet         net;
    RambleNodeDiscovery   discovery;
} RambleNodeOpts;

/* Per topic options, passed as a compound literal. */
typedef struct {
    RambleQos  qos;
    uint8_t    reflect_from_mesh;  /* fill what was left unspecified from the mesh: a NULL schema and
                                    a zero reliability follow it. See docs/reflection.md */
} RambleTopicOpts;

typedef struct RambleNode    RambleNode;
typedef struct RambleTopic RambleTopic;   /* an opaque handle, stable for the node's life */

/* A delivered message. From the callback, send and read only queries on the same node
 * are allowed. poll, create, set_role, drain, start, stop and close are refused. */
typedef struct {
    RambleNode      *node;
    void            *user;             /* RambleNodeOpts.user_data */
    uint16_t         topic_index;
    uint32_t         publisher_id;
    RambleString     publisher_name;      /* never NULL data on delivery, "unknown-peer" at worst. A
                                        view valid for the callback */
    RambleString     topic_name;     /* {NULL,0} if unknown */
    RambleBytes      header;           /* pattern header bytes in front of the payload, {NULL,0} on a
                                        plain topic */
    RambleBytes      data;             /* the payload after the header */
    const RambleSchema *schema;        /* the schema data decodes with, validated before delivery.
                                        NULL on a raw topic or an untyped publisher */
    uint64_t       recv_us;          /* the monotonic clock at receive, or at enqueue */
    uint64_t       written_us;       /* the writer's wall clock at commit, UTC microseconds. 0 = the
                                        publisher opted out. Never mix it with recv_us */
    uint64_t       capture_us;       /* when the publisher says the data was true, UTC
                                        microseconds. 0 = none given, written_us is all there is */
} RambleMsg;
typedef void (*RambleMsgFn)(const RambleMsg *msg);

/* The process heap as an allocator, for a caller with no page source of its own.
 * page_size 0 is the default. spec/allocation.md explains the modes. */
RAMBLE_API RambleAllocator ramble_allocator_heap(uint32_t page_size);
/* The process heap as a RambleAllocFn, for ramble_schema_compile and ramble_schema_free. */
RAMBLE_API void         *ramble_heap_realloc(void *user, void *ptr, size_t size);

/* Opens a node on alloc, which it copies and resets on close, so use one per node. alloc
 * is required and NULL refuses the open with RAMBLE_E_OOM. name NULL gives an auto name,
 * on_message and on_event may be NULL, opts NULL means defaults. */
RAMBLE_API RambleNode    *ramble_node_open(RambleAllocator *alloc, const char *name, RambleMsgFn on_message,
                                     RambleEventFn on_event, const RambleNodeOpts *opts);
/* The most recent error. n NULL returns the process global slot, the reason an open
 * returned NULL. .error is RAMBLE_E_NONE if none occurred. */
RAMBLE_API RambleEvent    ramble_last_error(RambleNode *n);
/* One loop tick, blocking up to timeout_ms. RAMBLE_ERR_STATE while a service thread runs
 * or from inside a callback. */
RAMBLE_API int          ramble_node_poll(RambleNode *n, int timeout_ms);
/* Stops the service thread, then tears the node down. RAMBLE_ERR_STATE from a callback. */
RAMBLE_API int          ramble_node_close(RambleNode *n, int send_bye);

/* Threading: every call is thread safe under one node lock never held across a wait.
 * Drive the node with ramble_node_poll or with ramble_node_start. See docs/node.md. */
/* Runs the background service thread. RAMBLE_ERR_NOSYS when threads are compiled out. */
RAMBLE_API int          ramble_node_start(RambleNode *n);
/* Stops and joins the service thread. Idempotent, implied by close. */
RAMBLE_API int          ramble_node_stop(RambleNode *n);
/* 1 while the service thread runs. */
RAMBLE_API int          ramble_node_is_started(RambleNode *n);
/* Hold the node lock from a non callback thread around reads of a view. Hold it briefly.
 * No ops from a callback and when threads are compiled out. */
RAMBLE_API void         ramble_node_lock(RambleNode *n);
RAMBLE_API void         ramble_node_unlock(RambleNode *n);
/* Sends that evicted never sent history after the bounded wait, since open. */
RAMBLE_API uint32_t     ramble_node_evicted_unsent(RambleNode *n);

#ifndef RAMBLE_NO_STDTYPES
/* The two standard type values that need a platform: the wall clock a Timestamp counts
 * from, and a random version 4 Uuid. Neither needs a node. */
RAMBLE_API RambleTimestamp ramble_timestamp_now(void);
RAMBLE_API void            ramble_uuid_new(RambleUuid *out);
#endif

/* Creates a topic. name is the identity, schema is copied in, NULL means raw bytes. A
 * retired slot is reused. NULL when the reserve is full, the name is bad or out of memory. */
RAMBLE_API RambleTopic *ramble_node_create_topic(RambleNode *n, const char *name, RambleRole role,
                                               const RambleSchema *schema, const RambleTopicOpts *opts);

/* Retires a topic and frees the handle, invalid after RAMBLE_OK. RAMBLE_ERR_STATE from a
 * callback, for a pattern channel or a builtin, or during a dispatch. See docs/topics.md. */
RAMBLE_API int ramble_topic_retire(RambleTopic *topic);
/* A reflect_from_mesh topic: re reads the mesh and re types the handle in place when the
 * provider moved. 1 re created, 0 current, negative on error. */
RAMBLE_API int ramble_topic_refresh(RambleTopic *topic);

/* Consumer queues: a topic becomes queued on its first take or dispatch, or from creation
 * with qos.queue_bytes. docs/node.md explains the rules. */
#ifndef RAMBLE_QUEUE_CAP
#define RAMBLE_QUEUE_CAP (1u << 20)   /* the default queue growth cap, bytes per topic */
#endif
/* Pops the next queued message. The views stay valid until the next take or dispatch.
 * timeout_ms 0 checks, positive waits, negative waits forever. 1 got one, 0 empty. */
RAMBLE_API int ramble_topic_take(RambleTopic *topic, RambleMsg *out, int timeout_ms);
/* Runs on_message on the calling thread for up to max_msgs queued messages (0 = all),
 * after waiting like take. Returns the count. The callbacks run without the node lock. */
RAMBLE_API int ramble_topic_dispatch(RambleTopic *topic, int max_msgs, int timeout_ms);
/* dispatch across every already queued topic, oldest first per topic. */
RAMBLE_API int ramble_node_dispatch(RambleNode *n, int max_msgs, int timeout_ms);
/* Messages waiting, ring bytes used and capacity, and messages dropped since open. */
RAMBLE_API void ramble_topic_queue_stats(RambleTopic *topic, uint32_t *msgs, uint32_t *bytes,
                                       uint32_t *capacity, uint32_t *dropped);
/* Publishes to every matched subscriber. RAMBLE_OK or a negative RambleResult. */
/* Optional per send config, NULL means all defaults. capture_us is when the data was
 * true rather than when it was sent, and costs 8 wire bytes only when it is set. */
typedef struct {
    uint64_t capture_us;   /* UTC microseconds, the ramble_timestamp_now clock. 0 = none */
} RambleSendOpts;

RAMBLE_API int          ramble_topic_send(RambleTopic *topic, RambleBytes data, const RambleSendOpts *opts);
/* Flips the role at runtime and re advertises. 0 ok, negative on error. */
RAMBLE_API int          ramble_topic_set_role(RambleTopic *topic, RambleRole role);
/* The local index, RambleMsg.topic_index for its messages. */
RAMBLE_API uint16_t     ramble_topic_index(const RambleTopic *topic);
/* The schema as passed to create, NULL for a raw topic. */
RAMBLE_API const RambleSchema *ramble_topic_schema(const RambleTopic *topic);
/* The handle by creation index, NULL if out of range. */
RAMBLE_API RambleTopic *ramble_node_topic(RambleNode *n, uint16_t index);

/* Reflection: three walks over one zero initialized RambleIter, called until 0. Every view
 * is valid until the next poll. docs/reflection.md explains them. */

/* One discovered peer per call, dropped peers included. Gate on liveness. */
RAMBLE_API int ramble_node_peers_next(RambleNode *n, RambleIter *it, RamblePeerInfo *out);

/* The entities one node advertises. RAMBLE_SELF walks this node, a dropped peer serves its
 * last known view. */
RAMBLE_API int ramble_node_entities_next(RambleNode *n, uint32_t peer, RambleIter *it, RambleEntityInfo *out);

/* The whole mesh folded, one entity per kind and name across every active peer and this node. */
RAMBLE_API int ramble_node_mesh_next(RambleNode *n, RambleIter *it, RambleEntityInfo *out);
RAMBLE_API int ramble_node_mesh_find(RambleNode *n, RambleEntityKind kind, const char *name, RambleEntityInfo *out);

/* Bumps on every reflected change anywhere. Re walk iff it moved. */
RAMBLE_API uint32_t ramble_node_mesh_epoch(RambleNode *n);

/* Microseconds waited on slow subscribers and how many sends waited, since open. */
RAMBLE_API void     ramble_node_backpressure_stats(RambleNode *n, uint64_t *waited_us, uint32_t *waited_sends);

/* Live bytes, the high water mark and the count of heap allocations. A flat count over a
 * window proves the hot path is allocation free. */
RAMBLE_API void     ramble_node_mem_stats(RambleNode *n, size_t *in_use, size_t *peak, uint64_t *alloc_calls);
/* The topic's cumulative repair counters. Zeroed for a NULL topic. */
RAMBLE_API void     ramble_topic_repair_stats(RambleTopic *topic, RambleRepairStats *out);

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
} RamblePumpSample;
typedef void (*RamblePumpProbeFn)(void *user, const RamblePumpSample *s);
/* Registers the probe, NULL disables. interval_us 0 = 200 ms. */
RAMBLE_API void     ramble_node_set_pump_probe(RambleNode *n, RamblePumpProbeFn fn, uint64_t interval_us, void *user);
/* The in progress message from peer: 1 and its base seqno, fragments held and needed. */
RAMBLE_API int      ramble_topic_subscriber_progress(RambleTopic *topic, uint32_t peer,
                                     uint64_t *base_seqno, uint32_t *have, uint32_t *total);
/* Waits until every subscriber acked everything, or timeout_ms. 1 drained, 0 on timeout
 * or from a callback. Call before close so a burst is not cut by the BYE. */
RAMBLE_API int      ramble_topic_drain(RambleTopic *topic, int timeout_ms);
/* Subscribers matched now. */
RAMBLE_API int      ramble_topic_match_count(RambleTopic *topic);

/* The send path match wait: a send that would reach nobody while a match is still
 * resolving blocks up to opts.match_wait_ms. See docs/topics.md and spec/interest.md. */
/* Unresolved candidate matches right now. 0 = converged for everyone known. */
RAMBLE_API int      ramble_topic_pending_count(RambleTopic *topic);
/* 1 when a send would not wait: matched, or converged with nobody to wait for. The async
 * form for a GUI that disables the wait. */
RAMBLE_API int      ramble_topic_ready(RambleTopic *topic);

/* Blocks until discovery and matching settle. Call it after creating topics. timeout_ms
 * negative = 3 announce intervals. 1 settled, 0 on timeout or from a callback. */
RAMBLE_API int      ramble_node_settle(RambleNode *n, int timeout_ms);
#ifdef RAMBLE_SHM
/* Messages published and delivered through shared memory since open. */
RAMBLE_API void     ramble_node_shm_stats(RambleNode *n, uint32_t *sent, uint32_t *recv);
#endif

/* Messages and bytes this node committed to the topic and received on it, since open. */
RAMBLE_API void     ramble_topic_counts(RambleTopic *topic, uint64_t *tx_msgs, uint64_t *tx_bytes,
                                    uint64_t *rx_msgs, uint64_t *rx_bytes);

/* The built in @ramble/log/{error,warn,info} topics. docs/node.md explains them. */
typedef enum { RAMBLE_LOG_ERROR = 0, RAMBLE_LOG_WARN = 1, RAMBLE_LOG_INFO = 2 } RambleLogLevel;
#ifndef RAMBLE_LOG_MAX
#define RAMBLE_LOG_MAX 512   /* formatted log text bytes, longer is truncated */
#endif
/* printf style publish on the level's topic. RAMBLE_ERR_NOSYS when the logs are disabled. */
RAMBLE_API int          ramble_node_log(RambleNode *n, RambleLogLevel level, const char *fmt, ...);
/* Publishes an already formatted line, len negative = NUL terminated. The FFI entry. */
RAMBLE_API int          ramble_node_log_text(RambleNode *n, RambleLogLevel level, const char *text, int len);
/* The node's own handle for a level, NULL when disabled. */
RAMBLE_API RambleTopic   *ramble_node_log_topic(RambleNode *n, RambleLogLevel level);

/* The @ramble/meta snapshot section mask, a 4 byte LE u32 request payload, 0 = everything.
 * The response is RambleMeta { info: map } with one key per section. See docs/node.md. */
#define RAMBLE_META_NODE   0x1u
#define RAMBLE_META_PROC   0x2u
#define RAMBLE_META_TOPICS 0x4u
#define RAMBLE_META_PEERS  0x8u

/* Internal hooks for the patterns layer: create a topic carrying an entity kind, route its
 * messages to a handler, and observe node wide events and a per poll tick. */
typedef void     (*i_RambleSysMsgFn)(void *user, const RambleMsg *msg);
typedef void     (*i_RambleSysEventFn)(void *user, const RambleEvent *ev);
typedef uint64_t (*i_RambleSysTickFn)(void *user, uint64_t now_us);   /* next deadline, 0 = none */
typedef void     (*i_RambleSysCloseFn)(void *user);   /* the node is closing: settle promises */

/* Creates a pattern topic: stamps the kind, prefix, directed flag and attrs, permits '@' in
 * the name, and routes deliveries to on_msg. Never queued. */
RambleTopic *i_ramble_node_create_pattern_topic(RambleNode *n, const char *name, RambleRole role,
                              const RambleSchema *schema, const RambleTopicOpts *opts,
                              uint8_t kind, uint8_t prefix_bytes, uint8_t directed, uint8_t attrs,
                              i_RambleSysMsgFn on_msg, void *on_msg_user);
/* Re types a live pattern channel in place, the engine of ramble_topic_refresh. Lock not held. */
int  i_ramble_topic_retype(RambleTopic *topic, const RambleSchema *schema, uint8_t reliability);
/* The reflect_from_mesh pick for one channel (which: 0 primary, 1 rsp, 2 prg). */
int  i_ramble_node_reflect_pick(RambleNode *n, RambleEntityKind kind, const char *name, int which, int writer,
                              const RambleSchema **schema, uint8_t *reliable, uint64_t *generation);
/* Publishes hdr plus payload on a pattern topic to every matched subscriber. */
int  i_ramble_topic_send_hdr(RambleTopic *topic, RambleBytes hdr, RambleBytes data);
/* Publishes hdr plus payload to one peer, for function replies. */
int  i_ramble_topic_send_to(RambleTopic *topic, uint32_t to_peer, RambleBytes hdr, RambleBytes data);
/* Clears a pattern topic's routing before its handle is freed. Under the node lock. */
void i_ramble_topic_clear_sys(RambleTopic *topic);
/* Hands every committed but unsent datagram to the wire now, transmit only. The pattern
 * layer's retire and close flush their CANCELLED replies here. Takes the node lock. */
void i_ramble_node_flush_tx(RambleNode *n);
/* The topic's next seqno. The slot line continues across retire and reuse, so a pattern
 * layer seeds its own monotonic counters from it. */
uint64_t i_ramble_topic_seqno(RambleTopic *topic);
/* The send path match wait without the send or the event: the caller derives its own
 * verdict. Returns the matched count. No wait from a callback or once converged. */
int  i_ramble_topic_match_wait(RambleTopic *topic);
/* Registers the patterns layer's event observer, per poll tick and close hook, NULL clears.
 * The close hook fires once at the top of close, with the node still fully alive. */
void i_ramble_node_set_sys_hooks(RambleNode *n, i_RambleSysEventFn on_event, i_RambleSysTickFn tick,
                               i_RambleSysCloseFn on_close, void *user);
/* Node pool allocation (size 0 frees), the layer's per node handle slot, the monotonic
 * clock and the wall clock the transport stamps written_us from. Under the node lock. */
void    *i_ramble_node_sys_alloc(RambleNode *n, void *ptr, size_t size);
void   **i_ramble_node_sys_slot (RambleNode *n);
uint64_t i_ramble_node_now_us   (RambleNode *n);
uint64_t i_ramble_node_wall_us  (RambleNode *n);
/* The node lock for a pattern call that mutates state: 1 = acquired here, 0 = already held
 * by this thread. sys_unlock never kicks. sys_poll drives one loop tick. */
int      i_ramble_node_sys_lock  (RambleNode *n);
void     i_ramble_node_sys_unlock(RambleNode *n, int acquired);
int      i_ramble_node_sys_poll  (RambleNode *n, int timeout_ms);
/* Fires a topic scoped RAMBLE_ERROR through the node's normal event path. Under the node lock. */
void     i_ramble_node_sys_error (RambleNode *n, RambleErrorKind error, RambleTopic *topic, uint32_t peer);
/* Matched subscribers excluding dormant peers, the provider liveness query. */
int      i_ramble_topic_live_match_count(RambleTopic *topic);
/* Is peer a matched subscriber of this PUB topic. The directed call severed lane backstop. */
int      i_ramble_topic_peer_matched(RambleTopic *topic, uint32_t peer);
/* Matched publishers feeding this topic's subscription side. */
int      i_ramble_topic_source_match_count(RambleTopic *topic);
/* The oldest live matched subscriber's peer id, 0 = none. The task layer's auto direct target. */
uint32_t i_ramble_topic_oldest_match(RambleTopic *topic);
/* This node's discovery uuid, stable for its lifetime. The task layer's progress demux filter. */
const uint8_t *i_ramble_node_uuid(RambleNode *n);
/* Reflection getters for the patterns layer's entity enumeration. */
uint8_t      i_ramble_topic_kind (const RambleTopic *topic);   /* RambleTopicKind */
uint8_t      i_ramble_topic_role (const RambleTopic *topic);   /* the current RambleRole */
RambleString i_ramble_topic_name (const RambleTopic *topic);   /* the stable name copy */
uint8_t      i_ramble_topic_reliability(const RambleTopic *topic);   /* the create qos reliability */
const uint8_t *i_ramble_node_peer_uuid(RambleNode *n, uint32_t peer);   /* NULL if unknown, a view */
uint16_t   i_ramble_node_topic_count(RambleNode *n);         /* one past the highest defined index */
/* Builds the RAMBLE_META_* snapshot (0 = all sections) as a map body into a node owned grown
 * buffer. A view valid until the next call, {NULL,0} on OOM. Under the node lock. */
RambleBytes i_ramble_node_snapshot(RambleNode *n, uint32_t sections);

#ifdef __cplusplus
}
#endif
#endif /* RAMBLE_NODE_H */
#pragma endregion
#pragma region shm/core.h
/* The same host shared memory module: segment mapping and chunks over the platform
 * layer. Compiles to nothing without RAMBLE_SHM. The rules are in spec/shm.md. */
#ifndef RAMBLE_SHM_H
#define RAMBLE_SHM_H

#include <stddef.h>
#include <stdint.h>

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
#pragma endregion
#ifndef RAMBLE_NO_PATTERNS
#pragma region patterns/core.h
/* The patterns layer: functions, tasks and variables over dedicated topic kinds, so they
 * never cross wire with a plain topic or each other. docs/patterns.md explains the API. */
#ifndef RAMBLE_PATTERNS_H
#define RAMBLE_PATTERNS_H


#ifdef __cplusplus
extern "C" {
#endif

#ifndef RAMBLE_PATTERN_BP_WAIT_US
#define RAMBLE_PATTERN_BP_WAIT_US 1000000u   /* the default backpressure wait, 1 s */
#endif
#ifndef RAMBLE_CALL_TIMEOUT_US
#define RAMBLE_CALL_TIMEOUT_US 5000000u      /* the default call timeout, 5 s */
#endif

/* The response message cap. The wire carries its length in one byte, so this is fixed.
 * A longer message is truncated, never refused. */
#define RAMBLE_CALL_MSG_MAX 255u

/* functions */

/* A call's outcome. OK, APP_ERROR, NO_HANDLER, CANCELLED and RUNNING travel on the wire,
 * TIMEOUT and PEER_LOST are synthesized at the caller. See spec/patterns.md. */
typedef enum {
    RAMBLE_CALL_OK        = 0,
    RAMBLE_CALL_APP_ERROR = 1,   /* the handler replied with ramble_request_fail */
    RAMBLE_CALL_NO_HANDLER= 2,   /* the definition has no on_request */
    RAMBLE_CALL_TIMEOUT   = 3,   /* no response within the timeout */
    RAMBLE_CALL_PEER_LOST = 4,   /* the handler node dropped mid call */
    RAMBLE_CALL_CANCELLED = 5,   /* a provider honored a cancel or retired mid run, or the node
                                  closed or the handle retired with the call still pending */
    RAMBLE_CALL_RUNNING   = 6    /* task, the one non terminal status: the request runs, the
                                  timeout is dropped, on_progress fires once with no data */
} RambleCallStatus;

typedef struct RambleFunction RambleFunction;   /* an opaque handle */

/* The request as delivered to the definition's on_request. Views are valid for the
 * callback only. Pass only the exact pointer received to the reply calls, never a copy. */
typedef struct RambleRequest {
    RambleNode         *node;          /* the node the handler runs on */
    RambleString        function_name; /* the name with the @req suffix stripped */
    RambleBytes         data;          /* the request payload */
    const RambleSchema *schema;        /* the schema data decodes with, NULL = an untyped caller.
                                        Non NULL means data.len was validated against it */
    uint32_t            caller;        /* the calling peer's id */
    RambleString        caller_name;   /* the calling node's name, .data never NULL */
    uint64_t            recv_us;       /* the monotonic clock at arrival */
    uint64_t            written_us;    /* the caller's wall clock at the write, 0 = it opted out */
} RambleRequest;

/* Delivered to the caller when a response arrives or is synthesized. data is a view valid
 * for the callback only. provider is the peer that answered, 0 if synthesized. */
typedef struct {
    RambleCallStatus    status;
    RambleBytes         data;
    const RambleSchema *schema;    /* NULL = an untyped handler or a synthesized outcome */
    uint32_t          provider;
    void             *user;      /* the pointer passed to ramble_function_call_async */
    uint64_t            written_us; /* the provider's wall clock at the write, 0 if synthesized */
    RambleString        message;   /* the outcome text an HMI displays: the provider's, else the
                                    default status text. Empty only on OK, .data never NULL */
} RambleResponse;
typedef void (*RambleResponseFn)(const RambleResponse *response);

/* The handler: reply exactly once with ramble_request_reply or ramble_request_fail, or defer
 * with ramble_request_defer. Returning without replying auto acks RAMBLE_CALL_OK. */
typedef void (*RambleRequestFn)(RambleRequest *request, void *user);

typedef struct {
    uint32_t backpressure_wait_us;  /* 0 = RAMBLE_PATTERN_BP_WAIT_US */
    uint32_t timeout_us;            /* the remote side call timeout, 0 = RAMBLE_CALL_TIMEOUT_US */
    uint16_t keep_last;             /* req and rsp history depth, 0 = 10. Raise it above the most
                                       requests one pass drains, or replies past it are lost */
    uint8_t  reflect_from_mesh;     /* NULL schemas take the entity's from the mesh, and refresh
                                       re types the handle when that moves. A passed schema wins */
    uint8_t  multi;                 /* many definitions are expected, so the duplicate authority
                                       diagnostic is off. An undirected call reaches every one */
} RambleFunctionOpts;

/* One progress update as delivered to a task caller's on_progress. Views are valid for
 * the callback only. The RUNNING acknowledgment fires it once with zero length data. */
typedef struct {
    uint32_t            call_id;
    uint32_t            provider; /* the peer working the call */
    RambleBytes         data;     /* len 0 = the RUNNING ack */
    const RambleSchema *schema;   /* NULL = untyped or the RUNNING ack */
    uint64_t          written_us; /* the provider's wall clock, 0 = unstamped */
    uint64_t          recv_us;    /* the monotonic clock at arrival */
    void             *user;       /* RambleCallOpts.progress_user */
} RambleProgress;
typedef void (*RambleProgressFn)(const RambleProgress *progress);

/* Per call options, a trailing compound literal, NULL = defaults. */
typedef struct {
    uint32_t provider;              /* direct the call at this peer only, 0 = every definition and
                                       the first answer wins. A task always directs, 0 = oldest */
    RambleProgressFn on_progress;     /* task: fires per progress update on the delivering thread,
                                       NULL = updates are discarded */
    void     *progress_user;        /* RambleProgress.user */
    uint32_t *id_out;               /* filled with the call id, for ramble_function_cancel */
} RambleCallOpts;

/* Creates the definition (the body lives here, on_request NULL answers NO_HANDLER) or a
 * remote (a reference to one). Schemas may be NULL for untyped. NULL on failure. */
RAMBLE_API RambleFunction *ramble_node_create_function_definition(RambleNode *n, const char *name,
                             const RambleSchema *req_schema, const RambleSchema *rsp_schema,
                             RambleRequestFn on_request, void *user, const RambleFunctionOpts *opts);
RAMBLE_API RambleFunction *ramble_node_create_remote_function(RambleNode *n, const char *name,
                             const RambleSchema *req_schema, const RambleSchema *rsp_schema,
                             const RambleFunctionOpts *opts);

/* Calls and blocks driving the node loop. timeout_ms negative = the function's default.
 * out->data is valid until the next blocking call. docs/patterns.md has the returns. */
RAMBLE_API int  ramble_function_call(RambleFunction *fn, RambleBytes req, RambleResponse *out, int timeout_ms,
                                 const RambleCallOpts *opts);
/* Returns as soon as the request is committed, then on_response fires once with the
 * outcome. NULL = fire and forget. RAMBLE_OK or a negative RambleResult. */
RAMBLE_API int  ramble_function_call_async(RambleFunction *fn, RambleBytes req, RambleResponseFn on_response,
                                       void *user, const RambleCallOpts *opts);
/* Providers matched at a remote, callers matched at a definition. */
RAMBLE_API int  ramble_function_match_count(RambleFunction *fn);
/* Parks both channels, cancels every outstanding call with one CANCELLED outcome and frees
 * the handle. RAMBLE_ERR_STATE from a callback, the handle then stays valid. */
RAMBLE_API int  ramble_function_retire(RambleFunction *fn);
/* A reflect_from_mesh handle: re types every channel in place when the generation moved.
 * 1 re typed, 0 current, negative on error, RAMBLE_ERR_ROLE without the flag. */
RAMBLE_API int  ramble_function_refresh(RambleFunction *fn);

/* The built in @ramble/meta function every node hosts and can call, with .multi and directed
 * requests. Ask with RambleCallOpts.provider and a RAMBLE_META_* mask. NULL when disabled. */
RAMBLE_API RambleFunction *ramble_node_meta_function(RambleNode *n);

/* in the handler callback */
RAMBLE_API void      ramble_request_reply(RambleRequest *request, RambleBytes rsp);   /* answers OK */
/* Answers APP_ERROR with a human readable message (NULL = the default text, truncated at
 * RAMBLE_CALL_MSG_MAX). rsp may still carry structured failure data. */
RAMBLE_API void      ramble_request_fail (RambleRequest *request, const char *message, RambleBytes rsp);
/* Defers the reply: returns a token (0 on failure) and suppresses the auto ack. Complete it
 * later from any thread with ramble_function_complete. */
RAMBLE_API uint64_t  ramble_request_defer(RambleRequest *request);
RAMBLE_API int       ramble_function_complete(RambleFunction *fn, uint64_t token, RambleCallStatus status,
                                          const char *message, RambleBytes rsp);

/* Tasks: a function with progress and cancellation on the same handle. docs/tasks.md and
 * spec/patterns.md explain the model. */
typedef struct {
    uint8_t  progress_best_effort;  /* 0 = reliable. The definition offers, a remote requests,
                                       and the RxO rule composes them */
    uint16_t progress_keep_last;    /* the progress ring depth, 0 = the pattern default */
    uint8_t  no_cancel;             /* definition: will not honor cancellation, so remotes refuse
                                       ramble_function_cancel with RAMBLE_ERR_ROLE */
    uint8_t  exclusive;             /* definition: declared serialization, the handler enforces */
    uint8_t  multi;                 /* redundant providers intended, one executor per request */
    uint32_t timeout_us;            /* remote: the until first response bound, 0 = the default */
    uint32_t backpressure_wait_us;  /* 0 = RAMBLE_PATTERN_BP_WAIT_US */
    uint16_t keep_last;             /* req and rsp history depth, as RambleFunctionOpts.keep_last */
    uint8_t  reflect_from_mesh;     /* as RambleFunctionOpts.reflect_from_mesh, for all three */
} RambleTaskOpts;

/* Creates the definition or a remote, exactly as for a function, with prg_schema typing the
 * progress channel. The handler answers inline, or starts, defers and works the token. */
RAMBLE_API RambleFunction *ramble_node_create_task_definition(RambleNode *n, const char *name,
                             const RambleSchema *req_schema, const RambleSchema *prg_schema,
                             const RambleSchema *rsp_schema, RambleRequestFn on_request, void *user,
                             const RambleTaskOpts *opts);
RAMBLE_API RambleFunction *ramble_node_create_remote_task(RambleNode *n, const char *name,
                             const RambleSchema *req_schema, const RambleSchema *prg_schema,
                             const RambleSchema *rsp_schema, const RambleTaskOpts *opts);

/* Sends RUNNING to the caller now. Idempotent, and implied by defer on a task.
 * RAMBLE_ERR_STATE on a plain function or after a reply. */
RAMBLE_API int ramble_request_start(RambleRequest *request);
/* Token verbs, any thread. A stale token is RAMBLE_ERR_STATE. progress broadcasts on the prg
 * channel, cancelled answers 1 once a cancel arrived. Cancellation is cooperative. */
RAMBLE_API int ramble_function_progress (RambleFunction *fn, uint64_t token, RambleBytes progress);
RAMBLE_API int ramble_function_cancelled(RambleFunction *fn, uint64_t token);
/* The cancel notification, one slot per definition, NULL clears. Fires on the poll thread
 * under the usual callback restrictions. Polling ramble_function_cancelled alone is complete. */
typedef void (*RambleCancelFn)(uint64_t token, void *user);
RAMBLE_API int ramble_function_on_cancel(RambleFunction *def, RambleCancelFn on_cancel, void *user);
/* Requests cancellation of call_id, cooperative and never acked: the terminal status is the
 * answer. RAMBLE_ERR_ROLE when the provider declared no_cancel, RAMBLE_ERR_STATE when not pending. */
RAMBLE_API int ramble_function_cancel(RambleFunction *fn, uint32_t call_id);

/* Variables: replicated state with one owner, the definition, which publishes the value.
 * Writers push over a set channel with no response. Remotes cache the latest value. */
typedef struct RambleVariable RambleVariable;

typedef enum { RAMBLE_VAR_READWRITE = 0, RAMBLE_VAR_READONLY = 1 } RambleVarAccess;

typedef struct {
    RambleBytes initial;              /* definition: the value before any set, empty = none */
    uint8_t     access;              /* RambleVarAccess, READONLY creates no set channel */
    uint8_t     allow_force;         /* definition: permit force, off by default */
    uint16_t    catch_up;            /* the value channel's catch_up, 0 = 1 */
    uint16_t    keep_last;           /* both channels' history depth, the repair window for a burst of
                                      writes, 0 = 10. Raised to catch_up. See spec/patterns.md */
    uint32_t  backpressure_wait_us;/* 0 = RAMBLE_PATTERN_BP_WAIT_US */
    uint8_t   reflect_from_mesh;   /* a NULL schema takes the owner's from the mesh, and refresh
                                      re types the handle when that moves */
} RambleVariableOpts;

/* Creates the definition (this node holds the value) or a remote (reads see the cached
 * latest, writes go over the set channel). schema NULL = untyped. NULL on failure. */
RAMBLE_API RambleVariable *ramble_node_create_variable_definition(RambleNode *n, const char *name,
                                       const RambleSchema *schema, const RambleVariableOpts *opts);
RAMBLE_API RambleVariable *ramble_node_create_remote_variable(RambleNode *n, const char *name,
                                       const RambleSchema *schema, const RambleVariableOpts *opts);

/* Reads the current value into *out, a view valid until the next call on this variable or
 * the next poll. 1 if a value exists. */
RAMBLE_API int  ramble_variable_get(RambleVariable *var, RambleBytes *out);
/* Sets the value: a definition applies and publishes, a remote sends over the set channel.
 * RAMBLE_ERR_NO_TOPIC = no owner matched, RAMBLE_ERR_ROLE = a read only owner. docs/patterns.md. */
RAMBLE_API int  ramble_variable_set(RambleVariable *var, RambleBytes value);
/* Force overrides the value until unforce restores the latest absorbed set. A definition
 * needs .allow_force (RAMBLE_ERR_STATE without), a remote's force is ignored by one without. */
RAMBLE_API int  ramble_variable_force(RambleVariable *var, RambleBytes value);
RAMBLE_API int  ramble_variable_unforce(RambleVariable *var);
RAMBLE_API int  ramble_variable_forced(RambleVariable *var);

/* Variable events, one slot each, NULL clears. on_write fires on every applied write,
 * on_change only when the observed state changes, with a replay at registration. */
/* Both fire inline under the node lock on the thread that applied the write, with the usual
 * callback restrictions. A set from inside a callback is safe. See spec/patterns.md. */
typedef struct {
    RambleVariable     *variable;
    RambleString        name;      /* a stable view */
    RambleBytes         value;     /* the value just applied, valid for the callback */
    const RambleSchema *schema;    /* NULL = untyped */
    uint8_t           forced;      /* the value is a forced override */
    uint32_t          write_seq;   /* the owner's write counter */
    uint32_t          source;      /* the peer the write arrived from, 0 = a local call */
    uint64_t          recv_us;     /* the monotonic clock when the write applied */
    uint64_t          written_us;  /* the writer's wall clock, ours locally, 0 = opted out */
} RambleVariableUpdate;
typedef void (*RambleVariableUpdateFn)(const RambleVariableUpdate *update, void *user);

RAMBLE_API int  ramble_variable_on_change(RambleVariable *var, RambleVariableUpdateFn on_change, void *user);
RAMBLE_API int  ramble_variable_on_write (RambleVariable *var, RambleVariableUpdateFn on_write,  void *user);
/* Blocks driving the node loop until a value exists or timeout_ms elapses (negative =
 * forever). 1 = a value, 0 = timeout, or from a callback or under a service thread. */
RAMBLE_API int  ramble_variable_wait(RambleVariable *var, int timeout_ms);
/* Remote: owners matched. Definition: remotes matched. */
RAMBLE_API int  ramble_variable_match_count(RambleVariable *var);
/* Parks the channels, silences the callbacks and frees the handle, invalid after. The same
 * contract as ramble_function_retire. See docs/patterns.md. */
RAMBLE_API int  ramble_variable_retire(RambleVariable *var);
/* As ramble_function_refresh, for a reflect_from_mesh variable. */
RAMBLE_API int  ramble_variable_refresh(RambleVariable *var);

#ifdef __cplusplus
}
#endif
#endif /* RAMBLE_PATTERNS_H */
#pragma endregion
#endif /* !RAMBLE_NO_PATTERNS */
#endif /* !RAMBLE_TRANSPORT_SANS_IO */

#ifdef RAMBLE_TRANSPORT_IMPLEMENTATION
#pragma region common/bytes.h
/* Little endian byte packing for every wire format. */
#ifndef RAMBLE_BYTES_H
#define RAMBLE_BYTES_H

#include <stdint.h>

static inline void i_ramble_le_w16(uint8_t *p, uint16_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static inline void i_ramble_le_w32(uint8_t *p, uint32_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static inline void i_ramble_le_w64(uint8_t *p, uint64_t v){ int i; for (i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }
static inline uint16_t i_ramble_le_r16(const uint8_t *p){ return (uint16_t)(p[0] | ((uint16_t)p[1]<<8)); }
static inline uint32_t i_ramble_le_r32(const uint8_t *p){ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static inline uint64_t i_ramble_le_r64(const uint8_t *p){ uint64_t v=0; int i; for (i=0;i<8;i++) v|=((uint64_t)p[i])<<(8*i); return v; }

#endif /* RAMBLE_BYTES_H */
#pragma endregion
#pragma region common/arena.h
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
#pragma endregion
#pragma region common/hash.h
/* FNV-1a 64 for topic and schema identities. */
#ifndef RAMBLE_HASH_H
#define RAMBLE_HASH_H

#include <stddef.h>
#include <stdint.h>

/* The basis is not the textbook one. On wire identities depend on it, so it never changes. */
static inline uint64_t i_ramble_fnv1a64(const void *data, size_t n){
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 1469598103934665603ull; size_t i;
    for (i = 0; i < n; i++){ h ^= (uint64_t)p[i]; h *= 1099511628211ull; }
    return h;
}

/* Over a C string. NULL gives 0. */
static inline uint64_t i_ramble_fnv1a64_str(const char *s){
    uint64_t h = 1469598103934665603ull;
    const unsigned char *p = (const unsigned char *)s;
    if (!s) return 0;
    for (; *p; p++){ h ^= (uint64_t)*p; h *= 1099511628211ull; }
    return h;
}

#endif /* RAMBLE_HASH_H */
#pragma endregion
#pragma region transport/internal.h
/* Shared internals of the transport core. Not a public header. */
#ifndef RAMBLE_TRANSPORT_INTERNAL_H
#define RAMBLE_TRANSPORT_INTERNAL_H

#include <string.h>

/* byte 0 of every submessage: the type in the low 3 bits, flags above */
#define RAMBLE_DATA 1
#define RAMBLE_HB   2
#define RAMBLE_NACK 3
#define RAMBLE_MSG_MASK 0x07u
#define RAMBLE_F_SINGLE 0x08u     /* DATA: single fragment, frag, count and len omitted */
#define RAMBLE_F_UNPOS  0x10u     /* NACK: the reader has delivered nothing yet */
#ifdef RAMBLE_SHM
#define RAMBLE_F_SHM    0x20u     /* DATA: the body is a descriptor, no payload */
#endif

#define RAMBLE_NACK_WINDOW 32u    /* seqnos covered by one ACKNACK bitmap */
#define RAMBLE__NIL 0xFFFFFFFFu

#ifndef RAMBLE_HB_SWEEP_US
#define RAMBLE_HB_SWEEP_US 25000u /* the timer sweep covers every lane this often */
#endif

#define RAMBLE__NO_DEADLINE ((uint64_t)-1)  /* next_deadline_us: nothing armed */

/* per (peer, index) detail verdict bits, the peer_astate maps */
#define RAMBLE__AST_DETAILED 0x01u  /* details received and judged, else pending */
#define RAMBLE__AST_NAME_OK  0x02u  /* identity and name verified: peer_index[a] is our topic */
#define RAMBLE__AST_READ_OK  0x04u  /* schema gate passed on the read side, their pub to our sub */
#define RAMBLE__AST_WRITE_OK 0x08u  /* schema gate passed on the write side, their sub to our pub */

/* per entry interest flags, the announce's hash list */
#define RAMBLE__INT_ROLE_MASK 0x03u /* bits 0 and 1: the advertiser's RambleRole */
#define RAMBLE__INT_RELIABLE  0x04u /* bit 2: offered or requested reliability */
#define RAMBLE__INT_KIND_MASK 0x78u /* bits 3 to 6: the advertiser's RambleTopicKind */
#define RAMBLE__INT_KIND_SHIFT 3u
#define RAMBLE__INT_HOLE_RUN  0x80u /* bit 7: a run of undefined slots, hash = the run length */
#ifdef RAMBLE_SHM
#ifndef RAMBLE_SHM_MAX_RETRY
#define RAMBLE_SHM_MAX_RETRY 8u   /* give up on an unresolvable descriptor after this many */
#endif
#endif

#define RAMBLE_QOS_DEF_KEEP_LAST     1u
#define RAMBLE_QOS_DEF_KEEP_LAST_REL 10u   /* reliable: room for repair before overwrite */
#define RAMBLE_QOS_DEF_HEARTBEAT_US 250000u   /* 250 ms idle writer heartbeat */
#define RAMBLE_QOS_DEF_REPAIR_US    50000u    /* 50 ms reader repair request delay */
#ifndef RAMBLE_RTO_MIN_US
#define RAMBLE_RTO_MIN_US   2000u  /* the RTT timer floor, a poll wait cannot fire sooner */
#endif
#define RAMBLE_RTO_GRAIN_US 1000u  /* RFC 6298 G: the deviation term never counts under one tick */
#ifndef RAMBLE_HB_TAIL_US
#define RAMBLE_HB_TAIL_US 20000u   /* the tail heartbeat before the peer's RTT is known */
#endif

/* Submessage layout. Byte 0 = type and flags, bytes 1 and 2 = index, then the body.
 * Builders and readers share these offsets, so moving a field is one edit. */
#define RAMBLE_OFFSET_INDEX      1u  /* u16, every submessage */
#define RAMBLE_OFFSET_SEQNO      3u  /* DATA seqno, HB first, NACK base, SHM base (u64) */
#define RAMBLE_OFFSET_PAYLOAD_LEN_SINGLE     11u  /* DATA single: u16 payload_len */
#define RAMBLE_HEADER_DATA_SINGLE   13u  /* DATA single: header bytes */
#define RAMBLE_OFFSET_FRAG      11u  /* DATA multi: u16 frag */
#define RAMBLE_OFFSET_COUNT     13u  /* DATA multi: u16 count */
#define RAMBLE_OFFSET_SAMPLE_LEN      15u  /* DATA multi: u32 sample_len */
#define RAMBLE_OFFSET_PAYLOAD_LEN      19u  /* DATA multi: u16 payload_len */
#define RAMBLE_HEADER_DATA_MULTI    21u  /* DATA multi: header bytes */
#define RAMBLE_OFFSET_HB_LAST   11u  /* HB: u64 last */
#define RAMBLE_OFFSET_HB_COUNT    19u  /* HB: u32 count */
#define RAMBLE_HEADER_HB      23u
#define RAMBLE_OFFSET_NACK_NBITS  11u  /* NACK: u16 nbits */
#define RAMBLE_OFFSET_NACK_BITMAP   13u  /* NACK: u32 bitmap */
#define RAMBLE_OFFSET_NACK_EPOCH  17u  /* NACK: u32 epoch */
#define RAMBLE_HEADER_NACK    21u
#define RAMBLE_OFFSET_SHM_COUNT 11u  /* SHM-DATA: u16 count */
#define RAMBLE_OFFSET_SHM_DESC  13u  /* SHM-DATA: the descriptor */
#ifdef RAMBLE_SHM
#define RAMBLE_SHM_DATA_BYTES (RAMBLE_OFFSET_SHM_DESC + RAMBLE_SHM_DESC_BYTES)
#endif

/* The per lane and per sample structs are laid out widest field first so they carry no
 * padding, since they are allocated per match and per history slot. */
/* A sample's destination: every matched lane, nobody, or a peer slot. A slot is safe
 * because a directed topic never replays history and a new peer joins at the head. */
#define RAMBLE__DEST_ALL  0xFFFFFFFFu
#define RAMBLE__DEST_NONE 0xFFFFFFFEu

typedef struct {
    uint64_t base;       /* seqno of frag 0 */
    uint8_t *buf;        /* hook allocated, grown to fit */
#ifdef RAMBLE_SHM
    const uint8_t *shm_buf;            /* the external chunk payload, remote peers fragment it */
#endif
    uint32_t len;        /* message bytes */
    uint32_t cap;        /* allocated bytes of buf */
    uint32_t dest_slot;  /* RAMBLE__DEST_ALL, RAMBLE__DEST_NONE, or the destination peer slot */
    uint16_t count;      /* frag count */
    uint8_t  valid;
#ifdef RAMBLE_SHM
    uint8_t  shm;        /* 1 = the bytes live in shm_buf and desc is set */
    uint8_t  desc[RAMBLE_SHM_DESC_BYTES];/* sent to SHM peers as one SHM-DATA */
#endif
} i_RambleWriterSample;

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
} i_RambleWriterProxy;

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
} i_RambleAssembly;

typedef struct {        /* reader side, per (topic, peer) */
    uint64_t deliver_upto;  /* base of the current sample, everything below delivered or skipped */
    uint64_t hb_last;       /* the highest seqno the writer claims to hold */
    uint64_t received_high;      /* highest seqno actually received. Never NACK past it */
    uint64_t nack_high;       /* highest seqno requested this episode, a refill asks above it */
    uint64_t nack_retransmit_us;  /* earliest re ask of a stalled floor */
    uint64_t ack_due_us;
    i_RambleAssembly cur;     /* the head sample */
    i_RambleAssembly next;    /* one sample held ahead of the head. See spec/transport.md */
    uint32_t epoch;           /* this incarnation's id, sent in every ACKNACK */
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
#ifdef RAMBLE_SHM
    uint8_t  parked_shm;    /* the parked hold is a descriptor, not an assembled sample */
    uint8_t  shm_fail;      /* consecutive resolve failures at deliver_upto */
#endif
} i_RambleReaderProxy;

/* One matched lane: both proxies plus the scheduler links. Records move when the pool
 * grows, so durable references are pool indices, never pointers. */
typedef struct {
    i_RambleWriterProxy w;
    i_RambleReaderProxy r;
    uint16_t topic;     /* the lane this record serves */
    uint16_t peer_slot;
    uint32_t sched_next;  /* next on its dest's active list, or the free list link */
    uint32_t topic_next;     /* next matched lane of the same topic */
    uint8_t  queued;      /* on its dest's active list */
    uint8_t  in_use;      /* allocated to a lane, 0 = on the free list */
} i_RambleLane;

typedef struct {
    RambleQos    qos;
    uint64_t  identity;     /* the name hash */
    const char *name;       /* our copy, NUL terminated storage */
    uint8_t   name_len;
    uint8_t   role;         /* RambleRole */
    uint8_t   kind;         /* RambleTopicKind, gates matching */
    uint8_t   attrs;        /* RAMBLE_ATTR_* from the definer, NO_TIMESTAMP derives from the qos */
    uint8_t   prefix_bytes; /* pattern header bytes in front of each payload */
    uint8_t   directed;     /* point to point sends, no cross lane MSG_LOST */
    uint8_t   history_owned;/* the ring came from the allocator, so destroy frees it */
    uint8_t   retired;      /* parked for reuse: announced as a hole, never a match candidate */
    uint8_t   gen;          /* rebind generation, bumped when a reused slot's binding changed */
    uint32_t  rebind_version; /* our blob version at the last rebind, 0 = never. Writer lanes
                                 hold until the peer has seen it */
    /* writer */
    i_RambleWriterSample *history;       /* [depth] ring */
    uint16_t  history_head;    /* next slot to overwrite */
    uint64_t  next_seqno;
    uint64_t  first_seqno;  /* lowest seqno still cached */
    uint8_t   have_first;
    uint32_t  matched_writers; /* exact count of used writer proxies, dormant included */
    uint32_t  matched_readers; /* the same for reader proxies */
    uint32_t  lane_head;       /* first matched lane record, the send path walks this chain */
    RambleRepairStats repair_stats;
} i_RambleTopic;

struct RambleTransportState {
    RambleConfig    cfg;
    uint32_t      *peer_ids;  /* [max_peers] */
    uint8_t       *peer_used; /* [max_peers] */
    uint8_t       *peer_dormant;/* [max_peers] silent peers, out of flow control, state kept */
    uint16_t      *peer_frag; /* [max_peers] each peer's advertised fragment size */
    RamblePeerRtt *peer_rtt;  /* [max_peers] the round trip estimator */
    uint16_t     frag;        /* our fragment size */
#ifdef RAMBLE_SHM
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
    uint8_t    **peer_astate;     /* [max_peers] RAMBLE__AST_* bits per entry */
    uint8_t    **peer_agen;       /* [max_peers] the last applied rebind generation per entry */
    uint8_t    **peer_attrs;      /* [max_peers] the peer's advertised attrs per entry */
    uint32_t    *peer_seen_version; /* [max_peers] the highest version of our blob the peer named */
    i_RambleTopic  *topics;     /* [n_topics] */
    /* the lane record pool: one hook allocation grown by doubling, records per real match */
    i_RambleLane  *lanes;       /* [lane_cap] */
    uint32_t     lane_cap;
    uint32_t     lane_free;   /* free list head, RAMBLE__NIL when empty */
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

typedef enum { RAMBLE_ORDER_OLD, RAMBLE_ORDER_GAP, RAMBLE_ORDER_ADOPTED, RAMBLE_ORDER_INORDER } i_RambleReaderOrder;

static inline void i_ramble_bit_set(uint8_t*bitmap,uint32_t i){bitmap[i>>3]|=(uint8_t)(1u<<(i&7));}
static inline void i_ramble_bit_clr(uint8_t*bitmap,uint32_t i){bitmap[i>>3]&=(uint8_t)~(1u<<(i&7));}
static inline int  i_ramble_bit_get(const uint8_t*bitmap,uint32_t i){return (bitmap[i>>3]>>(i&7))&1;}
/* Does this slot occupy an announce entry. An undefined or retired slot rides as a hole run. */
static inline int i_ramble_topic_announced(const i_RambleTopic *t){
    return t->name_len != 0 && !t->retired;
}
#ifdef RAMBLE_SHM
/* where a sample's bytes live: the external chunk for SHM samples, else our buf */
static inline const uint8_t *i_ramble_sample_buf(const i_RambleWriterSample *s){ return s->shm ? s->shm_buf : s->buf; }
#else
static inline const uint8_t *i_ramble_sample_buf(const i_RambleWriterSample *s){ return s->buf; }
#endif
/* Keeps next_deadline at the minimum armed time. t 0 is an immediate ack, woken through
   the active lane queue, so it is ignored here. */
static inline void i_ramble_transport_arm_deadline(RambleTransportState *st, uint64_t t){
    if (t && t < st->next_deadline_us) st->next_deadline_us = t;
}

/* Source stamp bytes at the front of this topic's samples. Framing follows the qos alone. */
static inline uint32_t i_ramble_topic_ts_bytes(const i_RambleTopic *topic){
    return topic->qos.no_timestamp ? 0u : RAMBLE_TIMESTAMP_BYTES;
}

/* The whole stamp prefix: the source slot, plus a capture slot when the sender gave one.
 * Never a hook's presence, so a reader sizes it from the bytes it received. */
static inline uint32_t i_ramble_topic_stamp_bytes(const i_RambleTopic *topic, uint64_t capture_us){
    uint32_t ts = i_ramble_topic_ts_bytes(topic);
    return (ts && capture_us) ? ts + RAMBLE_CAPTURE_BYTES : ts;
}

/* Only a reliable topic with catch_up replays history to a late joiner. Everywhere else
 * history with no subscriber is dead weight and the send can be skipped. */
static inline int i_ramble_topic_retains_history(const i_RambleTopic *topic){
    return topic->qos.reliability==RAMBLE_RELIABLE && topic->qos.catch_up>0;
}

/* Heartbeats and acks both need a reliable topic with a matched proxy, so the sweep
 * skips the whole peer row of any other topic. */
static inline int i_ramble_topic_needs_sweep(const i_RambleTopic *topic){
    return topic->qos.reliability==RAMBLE_RELIABLE && (topic->matched_writers || topic->matched_readers);
}

/* The lane record for a (topic, peer) pair, RAMBLE__NIL when unmatched. */
static inline uint32_t i_ramble_lane_id(RambleTransportState *st, uint16_t topic_index, uint32_t peer_slot){
    uint16_t lane = st->lane_index[(size_t)topic_index*st->cfg.max_peers + peer_slot];
    return lane == 0xFFFFu ? RAMBLE__NIL : (uint32_t)lane;
}
static inline i_RambleLane *i_ramble_lane_at(RambleTransportState *st, uint16_t topic_index, uint32_t peer_slot){
    uint32_t li = i_ramble_lane_id(st, topic_index, peer_slot);
    return li == RAMBLE__NIL ? NULL : &st->lanes[li];
}
/* NULL when the lane is unmatched. Every caller treats that as not used. */
static inline i_RambleWriterProxy *i_ramble_writer_proxy_at(RambleTransportState *st, uint16_t topic_index, uint32_t peer_slot){
    i_RambleLane *l = i_ramble_lane_at(st, topic_index, peer_slot);
    return l ? &l->w : NULL;
}
static inline i_RambleReaderProxy *i_ramble_reader_proxy_at(RambleTransportState *st, uint16_t topic_index, uint32_t peer_slot){
    i_RambleLane *l = i_ramble_lane_at(st, topic_index, peer_slot);
    return l ? &l->r : NULL;
}
/* The wire index of a local topic is its own index. The peer mapped it from our interest. */
static inline uint16_t i_ramble_wire_index_of(RambleTransportState *st, int topic_index){
    (void)st; return (uint16_t)topic_index;
}

/* cross file prototypes */
size_t i_ramble_wire_mk_data(uint8_t *o, uint16_t index, uint64_t seqno, i_RambleWriterSample *s, uint16_t frag, const uint8_t *payload, uint16_t payload_len);
#ifdef RAMBLE_SHM
size_t i_ramble_wire_mk_shm(uint8_t *o, uint16_t index, uint64_t base, uint16_t count, const uint8_t *desc);
#endif
size_t i_ramble_wire_mk_hb(uint8_t *o, uint16_t index, uint64_t first, uint64_t last, uint32_t cnt);
size_t i_ramble_wire_mk_nack(uint8_t *o, uint16_t index, uint64_t base, uint16_t nbits, uint32_t bitmap, uint32_t epoch, uint8_t flags);
void   i_ramble_lane_wake(RambleTransportState *st, uint16_t topic_index, uint32_t peer_slot);
void   i_ramble_lane_enqueue(RambleTransportState *st, uint32_t li);
void   i_ramble_sched_drop(RambleTransportState *st, uint32_t rec);
size_t i_ramble_writer_emit(RambleTransportState *st, int topic_index, int peer_slot, uint8_t *out, size_t cap, uint64_t now);
void   i_ramble_writer_nack(RambleTransportState *st, int topic_index, int peer_slot, const uint8_t *p, uint64_t now);
/* the per peer RTT estimator: fold one sample, and the retransmit bound it implies */
void     i_ramble_rtt_sample(RambleTransportState *st, uint32_t peer_slot, uint64_t sample_us);
uint32_t i_ramble_rtt_rto(RambleTransportState *st, uint32_t peer_slot, uint32_t fallback_us);
void   i_ramble_reader_data(RambleTransportState *st, int topic_index, int peer_slot, const uint8_t *p, uint64_t now);
#ifdef RAMBLE_SHM
void   i_ramble_reader_shm(RambleTransportState *st, int topic_index, int peer_slot, const uint8_t *p, uint64_t now);
#endif
void   i_ramble_reader_hb(RambleTransportState *st, int topic_index, int peer_slot, const uint8_t *p, uint64_t now);
size_t i_ramble_reader_emit(RambleTransportState *st, int topic_index, int peer_slot, uint8_t *out, size_t cap, uint64_t now);
i_RambleTopic *i_ramble_topic_at(RambleTransportState *st, uint16_t topic_index, int *idx_out);
int    i_ramble_peer_slot(RambleTransportState *st, uint32_t id);
void   i_ramble_transport_fire_event(RambleTransportState *st, RambleTransportEventKind kind, uint16_t topic_index, uint32_t peer, uint64_t first, uint64_t count);
uint64_t i_ramble_topic_unicast_join_seqno(const i_RambleTopic *topic);

#endif /* RAMBLE_TRANSPORT_INTERNAL_H */
#pragma endregion
#pragma region transport/wire.c
/* The DATA, HB, NACK and SHM-DATA submessage builders. */


size_t i_ramble_wire_mk_data(uint8_t *o, uint16_t index, uint64_t seqno, i_RambleWriterSample *s,
                         uint16_t frag, const uint8_t *payload, uint16_t payload_len){
    i_ramble_le_w16(o+RAMBLE_OFFSET_INDEX,index);
    if (s->count==1){                       /* frag 0, count 1 and len are implied */
        o[0]=(uint8_t)(RAMBLE_DATA|RAMBLE_F_SINGLE);
        i_ramble_le_w64(o+RAMBLE_OFFSET_SEQNO,seqno); i_ramble_le_w16(o+RAMBLE_OFFSET_PAYLOAD_LEN_SINGLE,payload_len);
        memcpy(o+RAMBLE_HEADER_DATA_SINGLE,payload,payload_len);
        return RAMBLE_HEADER_DATA_SINGLE+payload_len;
    }
    o[0]=RAMBLE_DATA;
    i_ramble_le_w64(o+RAMBLE_OFFSET_SEQNO,seqno); i_ramble_le_w16(o+RAMBLE_OFFSET_FRAG,frag); i_ramble_le_w16(o+RAMBLE_OFFSET_COUNT,s->count);
    i_ramble_le_w32(o+RAMBLE_OFFSET_SAMPLE_LEN,s->len); i_ramble_le_w16(o+RAMBLE_OFFSET_PAYLOAD_LEN,payload_len);
    memcpy(o+RAMBLE_HEADER_DATA_MULTI,payload,payload_len);
    return RAMBLE_HEADER_DATA_MULTI+payload_len;
}

#ifdef RAMBLE_SHM
/* One submessage covers [base, base+count). The body is the descriptor, no payload. */
size_t i_ramble_wire_mk_shm(uint8_t *o, uint16_t index, uint64_t base, uint16_t count,
                          const uint8_t *desc){
    o[0]=(uint8_t)(RAMBLE_DATA|RAMBLE_F_SHM); i_ramble_le_w16(o+RAMBLE_OFFSET_INDEX,index);
    i_ramble_le_w64(o+RAMBLE_OFFSET_SEQNO,base); i_ramble_le_w16(o+RAMBLE_OFFSET_SHM_COUNT,count);
    memcpy(o+RAMBLE_OFFSET_SHM_DESC,desc,RAMBLE_SHM_DESC_BYTES);
    return RAMBLE_SHM_DATA_BYTES;
}
#endif

size_t i_ramble_wire_mk_hb(uint8_t *o, uint16_t index, uint64_t first, uint64_t last, uint32_t cnt){
    o[0]=RAMBLE_HB; i_ramble_le_w16(o+RAMBLE_OFFSET_INDEX,index); i_ramble_le_w64(o+RAMBLE_OFFSET_SEQNO,first); i_ramble_le_w64(o+RAMBLE_OFFSET_HB_LAST,last); i_ramble_le_w32(o+RAMBLE_OFFSET_HB_COUNT,cnt);
    return RAMBLE_HEADER_HB;
}

size_t i_ramble_wire_mk_nack(uint8_t *o, uint16_t index, uint64_t base, uint16_t nbits, uint32_t bitmap,
                         uint32_t epoch, uint8_t flags){
    o[0]=(uint8_t)(RAMBLE_NACK|flags); i_ramble_le_w16(o+RAMBLE_OFFSET_INDEX,index); i_ramble_le_w64(o+RAMBLE_OFFSET_SEQNO,base);
    i_ramble_le_w16(o+RAMBLE_OFFSET_NACK_NBITS,nbits); i_ramble_le_w32(o+RAMBLE_OFFSET_NACK_BITMAP,bitmap); i_ramble_le_w32(o+RAMBLE_OFFSET_NACK_EPOCH,epoch);
    return RAMBLE_HEADER_NACK;
}
#pragma endregion
#pragma region transport/sched.c
/* The active lane scheduler and the outgoing poll. */


/* work is queued as lane record indices, the destination is the record's peer slot */
static void i_ramble_dest_push(RambleTransportState *st, uint32_t d){
    uint32_t ndest = st->cfg.max_peers, pos;
    if (st->dest_queued[d]) return;
    st->dest_queued[d]=1;
    pos = st->dest_queue_head + st->dest_queue_count;
    if (pos >= ndest) pos -= ndest;
    st->dest_queue[pos]=d; st->dest_queue_count++;
}


/* idempotent while queued */
void i_ramble_lane_enqueue(RambleTransportState *st, uint32_t li){
    i_RambleLane *l=&st->lanes[li];
    uint32_t d=l->peer_slot;
    if (l->queued) return;
    l->queued=1; l->sched_next=RAMBLE__NIL;
    if (st->dest_head[d]==RAMBLE__NIL) st->dest_head[d]=li;
    else st->lanes[st->dest_tail[d]].sched_next=li;
    st->dest_tail[d]=li;
    i_ramble_dest_push(st, d);
}


/* A recycled record must never linger on a dest list, or it misroutes another lane's
 * submessages to the old peer. Unmatch time only, the lists are short. */
void i_ramble_sched_drop(RambleTransportState *st, uint32_t li){
    i_RambleLane *l=&st->lanes[li];
    uint32_t d, cur, prev;
    if (!l->queued) return;
    d = l->peer_slot;
    prev = RAMBLE__NIL; cur = st->dest_head[d];
    while (cur!=RAMBLE__NIL && cur!=li){ prev=cur; cur=st->lanes[cur].sched_next; }
    if (cur==li){
        if (prev==RAMBLE__NIL) st->dest_head[d]=l->sched_next;
        else st->lanes[prev].sched_next=l->sched_next;
        if (st->dest_tail[d]==li) st->dest_tail[d]=prev;
    }
    l->queued=0; l->sched_next=RAMBLE__NIL;
}


/* Enqueues and tracks a freshly armed reader deadline for the poll cap. The sweep uses
 * i_ramble_lane_enqueue instead, since it recomputes the deadline itself. */
void i_ramble_lane_wake(RambleTransportState *st, uint16_t topic_index, uint32_t peer_slot){
    uint32_t li=i_ramble_lane_id(st,topic_index,peer_slot);
    i_RambleLane *l;
    if (li==RAMBLE__NIL) return;                     /* unmatched lane: nothing to schedule */
    l=&st->lanes[li];
    if (l->r.used && l->r.ack_pending) i_ramble_transport_arm_deadline(st, l->r.ack_due_us);
    i_ramble_lane_enqueue(st, li);
}


/* sendable work a popped record still owes now. Timer armed work is the sweep's job */
static int i_ramble_lane_work(RambleTransportState *st, const i_RambleLane *l, uint64_t now){
    i_RambleTopic *topic=&st->topics[l->topic];
    uint32_t peer_slot=l->peer_slot;
    if (!st->peer_used[peer_slot] || st->peer_dormant[peer_slot]) return 0;   /* dormant */
    if (l->w.used && l->w.has_nack) return 1;
    if (l->w.used && l->w.skip_hb) return 1;   /* directed floor HB still owed */
    if (l->w.used && l->w.sent_upto < topic->next_seqno
        && (l->w.rate_interval_us == 0 || now >= l->w.rate_next_us)) return 1;   /* throttled */
    if (l->r.used && topic->qos.reliability==RAMBLE_RELIABLE
        && l->r.ack_pending && now >= l->r.ack_due_us) return 1;
    return 0;
}


/* Clock driven: a cursor walks the pool waking lanes whose timers came due, covering it
 * every RAMBLE_HB_SWEEP_US. A forced full pass when next_deadline_us is due recomputes it. */
static void i_ramble_hb_sweep(RambleTransportState *st, uint64_t now){
    uint32_t total=st->lane_cap, due, k;
    uint64_t span = now - st->sweep_time_us;
    int forced = (now >= st->next_deadline_us);    /* a tracked timer is due */
    int full;
    uint64_t mind = RAMBLE__NO_DEADLINE;             /* earliest not yet due timer seen */
    if (total==0){                                   /* no lanes have ever matched */
        if (forced) st->next_deadline_us = RAMBLE__NO_DEADLINE;
        st->sweep_time_us = now;
        return;
    }
    due = (forced || span >= RAMBLE_HB_SWEEP_US) ? total
        : (uint32_t)(span * total / RAMBLE_HB_SWEEP_US);
    if (!due) return;              /* sweep_time_us advances only when lanes are paid */
    full = (due >= total);         /* covered every record, so mind is the global minimum */
    st->sweep_time_us = now;
    for (k=0;k<due;k++){
        uint32_t li=st->sweep;
        i_RambleLane *l=&st->lanes[li];
        i_RambleTopic *topic;
        uint32_t peer_slot;
        st->sweep = (st->sweep+1u>=total) ? 0u : st->sweep+1u;
        if (!l->in_use) continue;                  /* free pool slot */
        topic=&st->topics[l->topic];
        peer_slot=l->peer_slot;
        /* a throttled lane owes a send at its tick. First, since best effort skips below */
        if (l->w.used && l->w.rate_interval_us && l->w.sent_upto < topic->next_seqno
            && st->peer_used[peer_slot] && !st->peer_dormant[peer_slot]){
            if (now>=l->w.rate_next_us) i_ramble_lane_enqueue(st,li);
            else if (l->w.rate_next_us < mind) mind = l->w.rate_next_us;
        }
        if (!i_ramble_topic_needs_sweep(topic)) continue;   /* best effort, or nothing matched */
        /* HB gate on next_seqno, never the reader ack: a sub only node still owes acks */
        if (!st->peer_used[peer_slot] || st->peer_dormant[peer_slot]) continue;   /* dormant */
        if (l->w.used && l->w.reader_reliable && l->w.acked_upto < topic->next_seqno){
            if (now>=l->w.hb_next_us) i_ramble_lane_enqueue(st,li);
            else if (l->w.hb_next_us < mind) mind = l->w.hb_next_us;
        }
        if (l->r.used && l->r.ack_pending){
            if (now>=l->r.ack_due_us) i_ramble_lane_enqueue(st,li);
            else if (l->r.ack_due_us < mind) mind = l->r.ack_due_us;
        }
    }
    /* a full pass saw every timer, so mind is exact. Woken lanes re arm during emit */
    if (full) st->next_deadline_us = mind;
}


int ramble_transport_poll_send(RambleTransportState *st, uint32_t *to_peer, void *out, size_t cap, size_t *out_len, uint64_t now){
    uint32_t ndest=st->cfg.max_peers;
    i_ramble_hb_sweep(st, now);
    while (st->dest_queue_count){
        uint32_t d; size_t offset=0;
        d = st->dest_queue[st->dest_queue_head];
        st->dest_queue_head = (st->dest_queue_head+1u>=ndest) ? 0u : st->dest_queue_head+1u;
        st->dest_queue_count--; st->dest_queued[d]=0;
        /* drain this destination's lanes into one datagram */
        while (st->dest_head[d]!=RAMBLE__NIL){
            uint32_t li=st->dest_head[d];
            i_RambleLane *l=&st->lanes[li];
            uint16_t topic_index=l->topic; uint32_t peer_slot=l->peer_slot;
            size_t n;
            do {
                /* acks first: small, and they carry the NACKs that drive repair */
                n=i_ramble_reader_emit(st,(int)topic_index,(int)peer_slot,(uint8_t*)out+offset,cap-offset,now);
                if (!n) n=i_ramble_writer_emit(st,(int)topic_index,(int)peer_slot,(uint8_t*)out+offset,cap-offset,now);
                offset+=n;
            } while (n && offset<cap);
            st->dest_head[d]=l->sched_next;
            if (i_ramble_lane_work(st,l,now)){
                /* datagram full mid lane: rotate it to the back so siblings get the next */
                if (st->dest_head[d]==RAMBLE__NIL) st->dest_head[d]=li;
                else {
                    l->sched_next=RAMBLE__NIL;
                    st->lanes[st->dest_tail[d]].sched_next=li;
                    st->dest_tail[d]=li;
                }
                break;
            }
            l->queued=0;     /* lane drained */
        }
        if (st->dest_head[d]!=RAMBLE__NIL) i_ramble_dest_push(st,d);  /* fair: re queue at the tail */
        if (offset){
            *to_peer = st->peer_ids[d];
            *out_len = offset;
            return 1;
        }
        if (st->dest_head[d]!=RAMBLE__NIL)
            return 0;    /* work pending but nothing fit: the cap is too small */
    }
    return 0;
}


uint64_t ramble_transport_next_deadline_us(RambleTransportState *st){
    return st->next_deadline_us == RAMBLE__NO_DEADLINE ? 0 : st->next_deadline_us;
}


int ramble_transport_tx_pending(RambleTransportState *st){
    return st->dest_queue_count != 0;   /* the active lane queue */
}
#pragma endregion
#pragma region transport/writer.c
/* The writer path: history, send, the per lane emit and ACKNACK handling. */


/* newest first, so pushing new data is O(1) */
static i_RambleWriterSample *i_ramble_sample_find(i_RambleTopic *topic, uint64_t seqno){
    uint16_t depth = topic->qos.keep_last, k;
    uint16_t i = topic->history_head;
    for (k=0;k<depth;k++){
        i_RambleWriterSample *s;
        i = (uint16_t)(i ? i-1 : depth-1);
        s = &topic->history[i];
        if (!s->valid) break;                  /* the unwritten tail */
        if (seqno >= s->base)
            return (seqno < s->base + s->count) ? s : NULL;
    }
    return NULL;
}


/* Seals the filled head slot into history at the next seqno, stamped with its destination. */
static void i_ramble_writer_seal(RambleTransportState *st, i_RambleTopic *topic, size_t len,
                               uint32_t dest_slot, uint64_t *base_out, uint16_t *count_out){
    uint16_t depth = topic->qos.keep_last;
    uint16_t count = (uint16_t)((len + st->frag - 1) / st->frag);
    i_RambleWriterSample *slot = &topic->history[topic->history_head];
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
static void i_ramble_writer_lane_advance(i_RambleTopic *topic, i_RambleWriterProxy *w, uint32_t peer_slot){
    i_RambleWriterSample *s;
    if (!topic->directed) return;
    while ((s = i_ramble_sample_find(topic, w->acked_upto)) != NULL
           && s->dest_slot != RAMBLE__DEST_ALL && s->dest_slot != peer_slot){
        w->acked_upto = s->base + s->count;
        w->skip_hb = 1;
    }
    if (w->sent_upto < w->acked_upto) w->sent_upto = w->acked_upto;
    while (w->sent_upto < topic->next_seqno
           && (s = i_ramble_sample_find(topic, w->sent_upto)) != NULL
           && s->dest_slot != RAMBLE__DEST_ALL && s->dest_slot != peer_slot
           && w->sent_upto == s->base)
        w->sent_upto = s->base + s->count;
}


/* Seals and wakes the lanes that carry the sample, O(matches). A directed sample wakes
 * only its lane and every other lane derives its skip. */
static void i_ramble_writer_commit(RambleTransportState *st, uint16_t topic_index, size_t len,
                                 uint32_t dest_slot){
    i_RambleTopic *topic = &st->topics[topic_index];
    int reliable = (topic->qos.reliability==RAMBLE_RELIABLE);
    uint64_t base; uint16_t count;
    uint32_t li;
    i_ramble_writer_seal(st, topic, len, dest_slot, &base, &count);
    for (li=topic->lane_head; li!=RAMBLE__NIL; li=st->lanes[li].topic_next){
        i_RambleLane *l = &st->lanes[li];
        if (!l->w.used || st->peer_dormant[l->peer_slot]) continue;
        if (dest_slot == RAMBLE__DEST_ALL || l->peer_slot == dest_slot){ i_ramble_lane_enqueue(st, li); continue; }
        i_ramble_writer_lane_advance(topic, &l->w, l->peer_slot);
        if (reliable && l->w.reader_reliable && l->w.skip_hb) i_ramble_lane_enqueue(st, li);
    }
}


/* The 65535 fragment cap, checked before the no subscriber early out. */
static int i_ramble_writer_too_big(RambleTransportState *st, i_RambleTopic *topic, size_t len){
    (void)topic;
    return len > 65535u*(uint32_t)st->frag;
}

/* Stores ts, hdr and data into the head slot. The source stamp is taken here, once per
 * message, so a repair resend, a replay and the SHM chunk all carry the original. */
static int i_ramble_writer_store(RambleTransportState *st, i_RambleTopic *topic,
                               RambleBytes hdr, RambleBytes data, uint64_t capture_us,
                               size_t *len_out){
    uint32_t ts = i_ramble_topic_ts_bytes(topic);
    uint32_t cap_b = (ts && capture_us) ? RAMBLE_CAPTURE_BYTES : 0u;
    size_t len = (size_t)ts + cap_b + hdr.len + data.len;
    if (i_ramble_writer_too_big(st, topic, len)) return RAMBLE_ERR_TOO_BIG;
    {   i_RambleWriterSample *slot = &topic->history[topic->history_head];
        size_t need = len ? len : 1u;
        if ((size_t)slot->cap < need){                    /* grow the slot to fit */
            uint8_t *new_buf = (uint8_t*)st->cfg.allocator(st->cfg.user, slot->buf, need);
            if (!new_buf) return RAMBLE_ERR_OOM;
            slot->buf = new_buf; slot->cap = (uint32_t)need;
        }
    }
    {   uint8_t *dst = topic->history[topic->history_head].buf;    /* gather: ts, hdr, payload */
        if (ts){
            uint64_t w = st->cfg.source_time ? st->cfg.source_time(st->cfg.user) : 0u;
            w &= RAMBLE_STAMP_MASK;
            if (cap_b) w |= RAMBLE_STAMP_CAPTURE;
            i_ramble_le_w64(dst, w);
            if (cap_b) i_ramble_le_w64(dst + ts, capture_us);
        }
        if (hdr.len)  memcpy(dst + ts + cap_b, hdr.data, hdr.len);
        if (data.len) memcpy(dst + ts + cap_b + hdr.len, data.data, data.len);
    }
#ifdef RAMBLE_SHM
    topic->history[topic->history_head].shm = 0;   /* an inline send: not SHM backed */
#endif
    *len_out = len;
    return RAMBLE_OK;
}


/* The one send: prologue, store and commit. dest_slot is stamped onto the sample. */
static int i_ramble_writer_send(RambleTransportState *st, uint16_t topic_index,
                              RambleBytes hdr, RambleBytes data, uint64_t capture_us,
                              uint32_t dest_slot){
    i_RambleTopic *topic; size_t len; int r;
    topic = i_ramble_topic_at(st, topic_index, NULL);
    if (!topic) return RAMBLE_ERR_NO_TOPIC;
    /* the stamps are ordinary payload for every size rule */
    if (i_ramble_writer_too_big(st, topic,
            i_ramble_topic_stamp_bytes(topic, capture_us) + hdr.len + data.len))
        return RAMBLE_ERR_TOO_BIG;
    if (topic->role == RAMBLE_SUB_ONLY || topic->role == RAMBLE_INACTIVE) return RAMBLE_ERR_ROLE;
    /* nobody subscribes and nothing durable to keep: skip the grow, the copy and the commit */
    if (topic->matched_writers == 0 && !i_ramble_topic_retains_history(topic)) return RAMBLE_OK;
    r = i_ramble_writer_store(st, topic, hdr, data, capture_us, &len);
    if (r != RAMBLE_OK) return r;
    i_ramble_writer_commit(st, topic_index, len, dest_slot);
    return RAMBLE_OK;
}


int ramble_transport_send(RambleTransportState *st, uint16_t topic_index, RambleBytes data, uint64_t now){
    RambleBytes nohdr; nohdr.data=NULL; nohdr.len=0;
    return ramble_transport_send_hdr(st, topic_index, nohdr, data, 0, now);
}


int ramble_transport_send_hdr(RambleTransportState *st, uint16_t topic_index, RambleBytes hdr,
                            RambleBytes data, uint64_t capture_us, uint64_t now){
    (void)now;
    return i_ramble_writer_send(st, topic_index, hdr, data, capture_us, RAMBLE__DEST_ALL);
}


int ramble_transport_send_to(RambleTransportState *st, uint16_t topic_index, uint32_t to_peer,
                           RambleBytes hdr, RambleBytes data, uint64_t capture_us, uint64_t now){
    int peer_slot = i_ramble_peer_slot(st, to_peer);   /* unknown: sent to nobody, seqno consumed */
    (void)now;
    return i_ramble_writer_send(st, topic_index, hdr, data, capture_us,
                              peer_slot < 0 ? RAMBLE__DEST_NONE : (uint32_t)peer_slot);
}

#ifdef RAMBLE_SHM

/* The chunk is the whole wire sample, the caller wrote the stamp and any header into it,
 * so nothing is gathered or copied here. */
int ramble_transport_send_shm(RambleTransportState *st, uint16_t topic_index, RambleBytes chunk,
                  const uint8_t *desc, uint64_t now){
    i_RambleTopic *topic; i_RambleWriterSample *slot; size_t len = chunk.len;
    (void)now;
    topic = i_ramble_topic_at(st, topic_index, NULL);
    if (!topic) return RAMBLE_ERR_NO_TOPIC;
    if (len > 65535u*(uint32_t)st->frag) return RAMBLE_ERR_TOO_BIG;      /* the fragment count cap */
    if (topic->role == RAMBLE_SUB_ONLY || topic->role == RAMBLE_INACTIVE) return RAMBLE_ERR_ROLE;
    slot = &topic->history[topic->history_head];
    slot->shm = 1;
    slot->shm_buf = chunk.data;
    memcpy(slot->desc, desc, RAMBLE_SHM_DESC_BYTES);
    i_ramble_writer_commit(st, topic_index, len, RAMBLE__DEST_ALL);
    return RAMBLE_OK;
}
#endif


int ramble_transport_send_would_evict(RambleTransportState *st, uint16_t topic_index){
    i_RambleTopic *topic = i_ramble_topic_at(st, topic_index, NULL);
    i_RambleWriterSample *slot; uint32_t li;
    if (!topic || topic->qos.reliability != RAMBLE_RELIABLE) return 0;
    slot = &topic->history[topic->history_head];        /* the slot the next send overwrites */
    if (!slot->valid) return 0;
    for (li=topic->lane_head; li!=RAMBLE__NIL; li=st->lanes[li].topic_next){
        i_RambleLane *l=&st->lanes[li];
        if (l->w.used && l->w.reader_reliable && !st->peer_dormant[l->peer_slot]
            && l->w.acked_upto < slot->base + slot->count) return 1;
    }
    return 0;
}


int ramble_transport_send_would_evict_unsent(RambleTransportState *st, uint16_t topic_index,
                                            uint64_t *evict_base, uint32_t *evict_count){
    i_RambleTopic *topic = i_ramble_topic_at(st, topic_index, NULL);
    i_RambleWriterSample *slot; uint32_t li;
    if (!topic) return 0;
    slot = &topic->history[topic->history_head];        /* the slot the next send overwrites */
    if (!slot->valid) return 0;
    for (li=topic->lane_head; li!=RAMBLE__NIL; li=st->lanes[li].topic_next){
        i_RambleLane *l=&st->lanes[li];
        if (l->w.used && !st->peer_dormant[l->peer_slot] && l->w.sent_upto < slot->base + slot->count){
            if (evict_base)  *evict_base  = slot->base;
            if (evict_count) *evict_count = slot->count;
            return 1;
        }
    }
    return 0;
}


int ramble_transport_send_drained(RambleTransportState *st, uint16_t topic_index){
    i_RambleTopic *topic = i_ramble_topic_at(st, topic_index, NULL);
    uint32_t li;
    if (!topic || topic->qos.reliability != RAMBLE_RELIABLE) return 1;  /* no acks to await */
    for (li=topic->lane_head; li!=RAMBLE__NIL; li=st->lanes[li].topic_next){
        i_RambleLane *l=&st->lanes[li];
        if (l->w.used && l->w.reader_reliable && !st->peer_dormant[l->peer_slot]
            && l->w.acked_upto < topic->next_seqno) return 0;  /* a reliable reader is behind */
    }
    return 1;
}


int ramble_transport_publisher_match_count(RambleTransportState *st, uint16_t topic_index){
    i_RambleTopic *topic = i_ramble_topic_at(st, topic_index, NULL);
    return topic ? (int)topic->matched_writers : 0;   /* cached at match time, O(1) */
}


/* O(1) from the cached count. */
int ramble_transport_subscriber_match_count(RambleTransportState *st, uint16_t topic_index){
    i_RambleTopic *topic = i_ramble_topic_at(st, topic_index, NULL);
    return topic ? (int)topic->matched_readers : 0;
}


/* Dormant excluded. O(matches), for liveness decisions, not the send path. */
int ramble_transport_publisher_live_matches(RambleTransportState *st, uint16_t topic_index){
    i_RambleTopic *topic = i_ramble_topic_at(st, topic_index, NULL);
    uint32_t li; int cnt = 0;
    if (!topic) return 0;
    for (li=topic->lane_head; li!=RAMBLE__NIL; li=st->lanes[li].topic_next){
        i_RambleLane *l=&st->lanes[li];
        if (l->w.used && !st->peer_dormant[l->peer_slot]) cnt++;
    }
    return cnt;
}


int ramble_transport_publisher_peer_matched(RambleTransportState *st, uint16_t topic_index,
                                          uint32_t peer_id){
    i_RambleTopic *topic = i_ramble_topic_at(st, topic_index, NULL);
    uint32_t li; int slot;
    if (!topic) return 0;
    slot = i_ramble_peer_slot(st, peer_id);
    if (slot < 0) return 0;
    for (li=topic->lane_head; li!=RAMBLE__NIL; li=st->lanes[li].topic_next){
        i_RambleLane *l=&st->lanes[li];
        if (l->w.used && l->peer_slot == (uint32_t)slot) return 1;
    }
    return 0;
}


/* The chain is newest first, so the last live hit is the oldest. */
uint32_t ramble_transport_publisher_oldest_match(RambleTransportState *st, uint16_t topic_index){
    i_RambleTopic *topic = i_ramble_topic_at(st, topic_index, NULL);
    uint32_t li, id = 0;
    if (!topic) return 0;
    for (li=topic->lane_head; li!=RAMBLE__NIL; li=st->lanes[li].topic_next){
        i_RambleLane *l=&st->lanes[li];
        if (l->w.used && !st->peer_dormant[l->peer_slot]) id = st->peer_ids[l->peer_slot];
    }
    return id;
}


int ramble_transport_repair_pending(RambleTransportState *st, uint16_t topic_index){
    i_RambleTopic *topic = i_ramble_topic_at(st, topic_index, NULL);
    uint32_t li; int cnt = 0;
    if (!topic) return 0;
    for (li=topic->lane_head; li!=RAMBLE__NIL; li=st->lanes[li].topic_next){
        i_RambleLane *l=&st->lanes[li];
        if (l->w.used && l->w.has_nack) cnt++;
    }
    return cnt;   /* writer lanes with a NACK to service */
}

#ifdef RAMBLE_SHM

/* One remote reader forces inline UDP for the whole message. */
int ramble_transport_publisher_shm_eligible(RambleTransportState *st, uint16_t topic_index){
    i_RambleTopic *topic = i_ramble_topic_at(st, topic_index, NULL);
    uint32_t li; int any=0;
    if (!topic) return 0;
    for (li=topic->lane_head; li!=RAMBLE__NIL; li=st->lanes[li].topic_next){
        i_RambleLane *l=&st->lanes[li];
        if (!l->w.used || st->peer_dormant[l->peer_slot]) continue;
        if (!st->peer_shm[l->peer_slot]) return 0;
        any = 1;
    }
    return any;
}
#endif

#ifdef RAMBLE_SHM
uint16_t ramble_transport_topic_hist_head(RambleTransportState *st, uint16_t topic_index){
    i_RambleTopic *topic = i_ramble_topic_at(st, topic_index, NULL);
    return topic ? topic->history_head : 0;
}
#endif

/* An HB advertising the window. Its first is the floor, which replaces GAP: the reader
 * skips to it. Resets the idle timer so it does not double send. */
static size_t i_ramble_writer_hb(RambleTransportState *st, i_RambleTopic *topic, i_RambleWriterProxy *w, uint16_t index,
                             uint8_t *out, size_t cap, uint64_t now){
    uint64_t first = topic->have_first ? topic->first_seqno : 0;
    if (cap < RAMBLE_HEADER_HB) return 0;
    if (w->acked_upto > first) first = w->acked_upto;   /* a fresh reader adopts the join point */
    w->hb_next_us = now + topic->qos.heartbeat_us;
    i_ramble_transport_arm_deadline(st, w->hb_next_us);   /* wake to send the next idle HB */
    w->hb_count++;
    return i_ramble_wire_mk_hb(out, index, first, topic->next_seqno-1, w->hb_count);
}


/* The queue just drained: the next HB comes within the tail window so a lost final message
 * repairs fast. The reader's immediate ack normally suppresses it, so healthy traffic is free. */
static void i_ramble_writer_arm_tail(RambleTransportState *st, i_RambleWriterProxy *w, uint32_t peer_slot, uint64_t now){
    uint64_t tail = now + i_ramble_rtt_rto(st, peer_slot, RAMBLE_HB_TAIL_US);
    if (w->hb_next_us <= now || w->hb_next_us > tail) w->hb_next_us = tail;
    i_ramble_transport_arm_deadline(st, w->hb_next_us);
}


void i_ramble_writer_nack(RambleTransportState *st, int topic_index, int peer_slot, const uint8_t *p, uint64_t now){
    i_RambleTopic *topic=&st->topics[topic_index];
    i_RambleWriterProxy *w=i_ramble_writer_proxy_at(st,topic_index,peer_slot);
    uint64_t base=i_ramble_le_r64(p+RAMBLE_OFFSET_SEQNO); uint16_t nbits=i_ramble_le_r16(p+RAMBLE_OFFSET_NACK_NBITS); uint32_t bitmap=i_ramble_le_r32(p+RAMBLE_OFFSET_NACK_BITMAP);
    uint32_t epoch=i_ramble_le_r32(p+RAMBLE_OFFSET_NACK_EPOCH); uint8_t flags=p[0];
    if (!w || !w->used) return;
    if (w->reader_epoch != epoch){
        if (w->reader_epoch){
            /* The reader re incarnated while our lane survived. Rejoin at the older of the
               fresh join point and the acked floor, so the successor gets the unacked window. */
            uint64_t join = i_ramble_topic_unicast_join_seqno(topic);
            w->sent_upto  = w->acked_upto < join ? w->acked_upto : join;
            w->acked_upto = w->sent_upto;
            w->has_nack   = 0; w->rtt_probe = 0;
            w->hb_next_us = 0;
            i_ramble_lane_wake(st,(uint16_t)topic_index,(uint32_t)peer_slot);
            w->reader_epoch = epoch;
            return;
        }
        w->reader_epoch = epoch;      /* first contact: the lane is already fresh */
    }
    if (flags & RAMBLE_F_UNPOS){
        /* an unpositioned reader never NACKs: re push from the unacked edge */
        if (w->acked_upto < w->sent_upto){
            w->sent_upto = w->acked_upto;
            i_ramble_lane_wake(st,(uint16_t)topic_index,(uint32_t)peer_slot);
        }
        w->rtt_probe = 0;
        return;                    /* no position information to apply */
    }
    /* the cumulative ack reached the armed probe. A repair request in the same ACKNACK
       disarms it, since the acks after a repair are ambiguous (Karn) */
    if (w->rtt_probe && base >= w->rtt_probe_seq){
        i_ramble_rtt_sample(st, (uint32_t)peer_slot, now > w->rtt_probe_us ? now - w->rtt_probe_us : 0u);
        w->rtt_probe = 0;
    }
    if (nbits>0 && bitmap!=0) w->rtt_probe = 0;
    if (base > w->acked_upto) w->acked_upto=base;
    /* directed: step over foreign samples the ack made contiguous and owe the floor HB */
    i_ramble_writer_lane_advance(topic, w, (uint32_t)peer_slot);
    if (w->skip_hb) i_ramble_lane_wake(st,(uint16_t)topic_index,(uint32_t)peer_slot);
    /* Merge, never overwrite: drop what the new base acks, then OR the new bits in.
       Overwriting stalled every crossing refill on the backstop. See spec/transport.md. */
    if (w->has_nack && base > w->nack_base){
        uint64_t d = base - w->nack_base;
        w->nack_bits = d < RAMBLE_NACK_WINDOW ? w->nack_bits >> d : 0u;
        w->nack_base = base;
        if (!w->nack_bits) w->has_nack = 0;
    }
    if (nbits>0 && bitmap!=0){
        topic->repair_stats.nacks_recv++;   /* a repair request, not a bare ack */
        if (w->has_nack){                                           /* nack_base >= base here */
            uint64_t d = w->nack_base - base;
            w->nack_bits |= d < RAMBLE_NACK_WINDOW ? bitmap >> d : 0u;
        } else { w->has_nack=1; w->nack_base=base; w->nack_bits=bitmap; }
        i_ramble_lane_wake(st,(uint16_t)topic_index,(uint32_t)peer_slot);
    }
}


/* One writer submessage if due and it fits cap, 0 if none. On no fit the state is
 * untouched, so the same submessage is produced next time. */
size_t i_ramble_writer_emit(RambleTransportState *st, int topic_index, int peer_slot, uint8_t *out, size_t cap, uint64_t now){
    i_RambleTopic *topic=&st->topics[topic_index];
    i_RambleWriterProxy *w=i_ramble_writer_proxy_at(st,topic_index,peer_slot);
    int reliable=(topic->qos.reliability==RAMBLE_RELIABLE);
    uint16_t index = i_ramble_wire_index_of(st, topic_index);
    if (!w || !w->used || st->peer_dormant[peer_slot]) return 0;   /* unmatched or dormant */

    /* a fire and forget lane stamps a private wire seqno. Reliable and directed lanes keep
       the global line, since repair and the directed skip HB index history by it */
    int per_lane = (!w->reader_reliable && !topic->directed);

    /* directed: derive owed skips first, which also catches up a lane that was dormant */
    i_ramble_writer_lane_advance(topic, w, (uint32_t)peer_slot);

    /* 1. repair */
    if (reliable && w->has_nack){
        uint32_t i;
        for (i=0;i<RAMBLE_NACK_WINDOW;i++){
            if (w->nack_bits & (1u<<i)){
                uint64_t seqno=w->nack_base+i;
                i_RambleWriterSample *s;
                if (seqno>=topic->next_seqno){                 /* nothing there */
                    w->nack_bits &= ~(1u<<i);
                    if (w->nack_bits==0) w->has_nack=0;
                    continue;
                }
                s=i_ramble_sample_find(topic,seqno);
                if (s && topic->directed && s->dest_slot != RAMBLE__DEST_ALL
                      && s->dest_slot != (uint32_t)peer_slot){
                    /* addressed to another lane: never re serve it, a leaked directed sample
                       would reach the wrong pending call. Clear its bits and owe the floor HB. */
                    uint32_t j;
                    for (j=0;j<RAMBLE_NACK_WINDOW;j++){
                        uint64_t sq=w->nack_base+j;
                        if (sq>=s->base && sq<s->base+s->count) w->nack_bits &= ~(1u<<j);
                    }
                    if (w->nack_bits==0) w->has_nack=0;
                    i_ramble_writer_lane_advance(topic,w,(uint32_t)peer_slot);
                    w->skip_hb = 1;
                    continue;
                }
                if (s){
#ifdef RAMBLE_SHM
                    if (st->peer_shm[peer_slot] && s->shm){   /* resend as one SHM-DATA */
                        uint32_t j;
                        if (cap < RAMBLE_SHM_DATA_BYTES) return 0;
                        for (j=0;j<RAMBLE_NACK_WINDOW;j++){
                            uint64_t sq=w->nack_base+j;
                            if (sq>=s->base && sq<s->base+s->count) w->nack_bits &= ~(1u<<j);
                        }
                        if (w->nack_bits==0) w->has_nack=0;
                        return i_ramble_wire_mk_shm(out,index,s->base,s->count,s->desc);
                    }
#endif
                    {
                    uint16_t frag_index=(uint16_t)(seqno - s->base);
                    uint32_t offset=(uint32_t)frag_index*st->frag;
                    uint16_t payload_len=(uint16_t)((s->len-offset)<st->frag?(s->len-offset):st->frag);
                    if (cap < (size_t)(s->count==1?RAMBLE_HEADER_DATA_SINGLE:RAMBLE_HEADER_DATA_MULTI)+(size_t)payload_len) return 0;
                    w->nack_bits &= ~(1u<<i);
                    if (w->nack_bits==0) w->has_nack=0;
                    topic->repair_stats.frags_sent++; topic->repair_stats.frags_resent++;
                    return i_ramble_wire_mk_data(out,index,seqno,s,frag_index,i_ramble_sample_buf(s)+offset,payload_len);
                    }
                } else {
                    /* superseded: skip the reader past the dropped region with an HB whose
                       first is our floor, and keep still cached seqnos for later repair */
                    uint64_t floor = (topic->have_first?topic->first_seqno:topic->next_seqno);
                    uint32_t j;
                    if (cap < RAMBLE_HEADER_HB) return 0;
                    for (j=0;j<RAMBLE_NACK_WINDOW;j++)
                        if (w->nack_base+j < floor) w->nack_bits &= ~(1u<<j);
                    if (w->nack_bits==0) w->has_nack=0;
                    return i_ramble_writer_hb(st,topic,w,index,out,cap,now);
                }
            }
        }
        w->has_nack=0;
    }

    /* the directed floor HB before any new data, so the reader's floor moves past the
       foreign seqnos first and the data that follows arrives in order */
    if (reliable && w->reader_reliable && w->skip_hb){
        size_t hb = i_ramble_writer_hb(st,topic,w,index,out,cap,now);
        if (!hb) return 0;        /* did not fit: retry next pass, the flag stays */
        w->skip_hb = 0;
        return hb;
    }

    /* 2. push new data */
    if (w->sent_upto < topic->next_seqno){
        uint64_t seqno=w->sent_upto;
        i_RambleWriterSample *s=i_ramble_sample_find(topic,seqno);
        /* Rate throttle at a sample boundary only: hold until the tick, then decimate to the
           newest sample. wire_skip absorbs the skipped ones. See spec/transport.md. */
        if (per_lane && w->rate_interval_us && (!s || seqno == s->base)){
            if (now < w->rate_next_us){ i_ramble_transport_arm_deadline(st, w->rate_next_us); return 0; }
            { i_RambleWriterSample *newest = i_ramble_sample_find(topic, topic->next_seqno - 1);
              if (newest && newest->base > seqno){
                  w->wire_skip += newest->base - seqno;      /* a paced skip: no perceived loss */
                  w->sent_upto = newest->base; seqno = w->sent_upto; s = newest;
              } }
            w->rate_next_us = now + w->rate_interval_us;
            i_ramble_transport_arm_deadline(st, w->rate_next_us);   /* the next tick fires on time */
        }
        if (s){
#ifdef RAMBLE_SHM
            /* peer_shm is set before data flows, so this is a sample boundary: one SHM-DATA */
            if (st->peer_shm[peer_slot] && s->shm){
                if (cap < RAMBLE_SHM_DATA_BYTES) return 0;
                w->sent_upto = s->base + s->count;
                if (reliable && w->reader_reliable){
                    if (!w->rtt_probe){ w->rtt_probe = 1; w->rtt_probe_seq = w->sent_upto; w->rtt_probe_us = now; }
                    if (w->sent_upto >= topic->next_seqno) i_ramble_writer_arm_tail(st, w, (uint32_t)peer_slot, now);
                }
                return i_ramble_wire_mk_shm(out,index,per_lane ? s->base - w->wire_skip : s->base,
                                          s->count,s->desc);
            }
#endif
            {
            uint16_t frag_index=(uint16_t)(seqno - s->base);
            uint32_t offset=(uint32_t)frag_index*st->frag;
            uint16_t payload_len=(uint16_t)((s->len-offset)<st->frag?(s->len-offset):st->frag);
            if (cap < (size_t)(s->count==1?RAMBLE_HEADER_DATA_SINGLE:RAMBLE_HEADER_DATA_MULTI)+(size_t)payload_len) return 0;
            w->sent_upto++;
            topic->repair_stats.frags_sent++;                       /* new data */
            if (reliable && w->reader_reliable){
                /* the sample's last fragment arms the RTT probe: its in order ack comes one
                   round trip after this push */
                if (!w->rtt_probe && w->sent_upto == s->base + s->count){
                    w->rtt_probe = 1; w->rtt_probe_seq = w->sent_upto; w->rtt_probe_us = now; }
                if (w->sent_upto >= topic->next_seqno)
                    i_ramble_writer_arm_tail(st, w, (uint32_t)peer_slot, now);   /* tail HB */
            }
            return i_ramble_wire_mk_data(out,index,per_lane ? seqno - w->wire_skip : seqno,
                                       s,frag_index,i_ramble_sample_buf(s)+offset,payload_len);
            }
        } else {
            /* fell out of the ring unsent. A fire and forget lane advances and its reader
               sees the wire seqno jump as loss. A reliable lane skips its reader with an HB. */
            if (per_lane){ w->sent_upto=(topic->have_first?topic->first_seqno:topic->next_seqno); return 0; }
            if (cap < RAMBLE_HEADER_HB) return 0;
            w->sent_upto=(topic->have_first?topic->first_seqno:topic->next_seqno);
            w->rtt_probe=0;
            return i_ramble_writer_hb(st,topic,w,index,out,cap,now);
        }
    }

    /* 3. heartbeat: reliable, due, and the reader is behind. A caught up lane goes silent
       until new data or a resubscribe drops acked_upto again. */
    if (reliable && w->reader_reliable && now>=w->hb_next_us && w->acked_upto < topic->next_seqno)
        return i_ramble_writer_hb(st,topic,w,index,out,cap,now);
    return 0;
}
#pragma endregion
#pragma region transport/reader.c
/* The reader path: ordering, reassembly, delivery and the ACKNACK emit. */


int ramble_transport_subscriber_progress(RambleTransportState *st, uint16_t topic_index, uint32_t peer,
                         uint64_t *base_seqno, uint32_t *have, uint32_t *total){
    i_RambleTopic *topic = i_ramble_topic_at(st, topic_index, NULL);
    int peer_slot; i_RambleReaderProxy *r;
    if (!topic) return 0;
    peer_slot = i_ramble_peer_slot(st, peer);
    if (peer_slot < 0) return 0;
    r = i_ramble_reader_proxy_at(st,topic_index,peer_slot);
    if (!r || !r->used || !r->cur.active) return 0;      /* no message mid reassembly */
    if (base_seqno) *base_seqno = r->deliver_upto;       /* the head message starts here */
    if (total)      *total      = r->cur.count;
    if (have){
        uint32_t i, c=0;
        for (i=0;i<r->cur.count;i++) if (i_ramble_bit_get(r->cur.bitmap,i)) c++;
        *have = c;
    }
    return 1;
}


/* Attributes each 0 to 1 arming of the ack to its trigger. Counted before ack_pending is set. */
static void i_ramble_reader_arm(i_RambleTopic *topic, i_RambleReaderProxy *r, int is_hb){
    if (!r->ack_pending){ if (is_hb) topic->repair_stats.arms_hb++; else topic->repair_stats.arms_data++; }
}

/* Arms an immediate ACKNACK. force owes a cumulative ack even at an unchanged floor. */
static void i_ramble_reader_ack_now(RambleTransportState *st, i_RambleTopic *topic, i_RambleReaderProxy *r,
                                  uint16_t topic_index, uint32_t peer_slot, int force, int is_hb){
    i_ramble_reader_arm(topic, r, is_hb);
    r->ack_pending = 1; r->ack_due_us = 0;
    if (force) r->ack_force = 1;
    i_ramble_lane_wake(st, topic_index, peer_slot);
}


/* The head sample (cur) and one sample held ahead of it (next). Nothing here moves
 * deliver_upto, only a delivery, the floor and the lapped rule do. See spec/transport.md. */

/* fit a slot's buffers to a sample, 0 = allocation failed */
static int i_ramble_asm_fit(RambleTransportState *st, i_RambleAssembly *a, uint32_t len, uint16_t count){
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
static int i_ramble_asm_missing(const i_RambleAssembly *a, uint64_t s){
    return !(a->active && s >= a->base && s < a->base + a->count && i_ramble_bit_get(a->bitmap, (uint32_t)(s - a->base)));
}

/* deliver_upto moved: the ahead slot rotates into the head when it is exactly next, and
 * drops when the floor passed it. Eviction is whole sample, so a floor inside it cannot happen. */
static void i_ramble_reader_settle(i_RambleReaderProxy *r){
    if (!r->next.active) return;
    if (r->next.base < r->deliver_upto){ r->next.active = 0; return; }
    if (r->next.base == r->deliver_upto && !r->cur.active){
        i_RambleAssembly t = r->cur; r->cur = r->next; r->next = t;
        r->next.active = 0;
    }
}

/* the ahead slot for a future sample, NULL when another sample occupies it */
static i_RambleAssembly *i_ramble_reader_ahead(i_RambleReaderProxy *r, uint64_t base){
    if (!r->next.active || r->next.base == base) return &r->next;
    return NULL;
}

/* Hands up every complete sample from the head on. A reliable refusal parks the head, a
 * best effort one drops. Lane pointers are re derived after each callback. */
static void i_ramble_reader_deliver(RambleTransportState *st, uint16_t topic_index, uint32_t peer_slot){
    i_RambleTopic *topic=&st->topics[topic_index];
    i_RambleReaderProxy *r=i_ramble_reader_proxy_at(st,topic_index,peer_slot);
    int reliable = (topic->qos.reliability==RAMBLE_RELIABLE);
    int delivered = 0;
    while (r->cur.active && r->cur.low == r->cur.count){
        int refused = st->cfg.on_message &&
                      st->cfg.on_message(st->cfg.user, topic_index, st->peer_ids[peer_slot],
                                         ramble_bytes(r->cur.buf, r->cur.len)) != 0;
        r = i_ramble_reader_proxy_at(st,topic_index,peer_slot);
        if (refused && reliable){
            /* park: no advance, no ack and no repair, so the writer's flow control
               backpressures the publisher. deliver_parked retries, a writer floor skips. */
            r->parked = 1;
            break;
        }
        r->deliver_upto = r->cur.base + r->cur.count;
        r->cur.active = 0; r->lapped = 0; delivered = 1;
        i_ramble_reader_settle(r);
    }
    if (delivered && reliable)              /* ack after delivery, the reader owns copy invariant */
        i_ramble_reader_ack_now(st,topic,r,topic_index,peer_slot,1,0);
}


static i_RambleReaderOrder i_ramble_reader_order_arrival(RambleTransportState *st, int topic_index, int peer_slot,
                                             i_RambleReaderProxy *r, uint64_t base, uint64_t top){
    i_RambleTopic *topic=&st->topics[topic_index];
    if (base < r->deliver_upto) return RAMBLE_ORDER_OLD;
    if (top > r->received_high) r->received_high = top;          /* proof these seqnos exist */
    if (base > r->deliver_upto){
        if (topic->qos.reliability==RAMBLE_RELIABLE && r->started){   /* gap: arm a repair NACK */
            /* an armed lane keeps its deadline and is only re woken. A parked lane stays silent */
            if (!r->parked){
                if (r->ack_pending) i_ramble_lane_wake(st,(uint16_t)topic_index,(uint32_t)peer_slot);
                else i_ramble_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,0,0);
            }
            return RAMBLE_ORDER_GAP;
        }
        if (r->started && !topic->directed){   /* adopt, a directed skip is no loss */
            i_ramble_transport_fire_event(st, RAMBLE_TRANSPORT_MSG_LOST, (uint16_t)topic_index, st->peer_ids[peer_slot],
                        r->deliver_upto, base - r->deliver_upto);
            topic->repair_stats.msgs_skipped += base - r->deliver_upto;
        }
        r->deliver_upto = base;
        return RAMBLE_ORDER_ADOPTED;
    }
    return RAMBLE_ORDER_INORDER;
}

#ifdef RAMBLE_SHM

/* An SHM-DATA covers its whole range, so there is no reassembly, only ordering. The
 * descriptor goes to on_shm and the node delivers. An SHM sample is never held ahead. */
void i_ramble_reader_shm(RambleTransportState *st, int topic_index, int peer_slot, const uint8_t *p, uint64_t now){
    i_RambleTopic *topic=&st->topics[topic_index];
    i_RambleReaderProxy *r=i_ramble_reader_proxy_at(st,topic_index,peer_slot);
    int reliable = (topic->qos.reliability==RAMBLE_RELIABLE);
    uint64_t base = i_ramble_le_r64(p+RAMBLE_OFFSET_SEQNO);
    uint16_t count = i_ramble_le_r16(p+RAMBLE_OFFSET_SHM_COUNT);
    const uint8_t *desc = p+RAMBLE_OFFSET_SHM_DESC;
    if (!r || !r->used || count==0) return;
    if (r->parked){ topic->repair_stats.frags_ahead++; return; }   /* a held sample blocks */
    {   i_RambleReaderOrder ord = i_ramble_reader_order_arrival(st, topic_index, peer_slot, r, base, base+count-1);
        if (ord==RAMBLE_ORDER_OLD || ord==RAMBLE_ORDER_GAP) return;   /* old, dup, or gap armed */
    }
    r->started = 1; r->cur.active = 0;   /* a partial inline assembly of it is superseded */
    if (r->rtt_probe && r->rtt_probe_seq >= base && r->rtt_probe_seq < base + count){
        i_ramble_rtt_sample(st, (uint32_t)peer_slot, now > r->rtt_probe_us ? now - r->rtt_probe_us : 0u);
        r->rtt_probe = 0;
    }
    /* Advance and ack only if the node delivered. A failed resolve leaves the gap for the
       reliability layer, and after RAMBLE_SHM_MAX_RETRY tries it is skipped loudly. */
    {   int ok = st->cfg.on_shm ?
                 st->cfg.on_shm(st->cfg.user, (uint16_t)topic_index, st->peer_ids[peer_slot], desc) : 0;
        r = i_ramble_reader_proxy_at(st,topic_index,peer_slot);   /* the callback may re enter */
        if (ok > 0){
            r->shm_fail = 0; r->lapped = 0;
            r->deliver_upto = base + count;
            i_ramble_reader_settle(r);
            if (reliable)                   /* ack after delivery */
                i_ramble_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,1,0);
            i_ramble_reader_deliver(st,(uint16_t)topic_index,(uint32_t)peer_slot);   /* held ahead */
            return;
        }
        if (ok < 0){                        /* refused downstream */
            if (!reliable){                 /* best effort drops */
                r->deliver_upto = base + count;
                i_ramble_reader_settle(r);
                return;
            }
            /* park the descriptor, copied since p is the shared RX buffer. An alloc failure
               falls through to the repair path: the writer resends and we retry. */
            if (i_ramble_asm_fit(st, &r->cur, RAMBLE_SHM_DESC_BYTES, 1u)){
                memcpy(r->cur.buf, desc, RAMBLE_SHM_DESC_BYTES);
                r->cur.base = base; r->cur.count = count;   /* seqnos the held sample spans */
                r->parked = 1; r->parked_shm = 1;           /* silent */
                return;
            }
        }
        if (reliable && ++r->shm_fail >= RAMBLE_SHM_MAX_RETRY){
            i_ramble_transport_fire_event(st, RAMBLE_TRANSPORT_MSG_LOST, (uint16_t)topic_index, st->peer_ids[peer_slot],
                        base, count);
            topic->repair_stats.msgs_skipped += count;
            r->shm_fail = 0;
            r->deliver_upto = base + count;             /* give up: skip it and ack the new edge */
            i_ramble_reader_settle(r);
            i_ramble_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,1,0);
        } else if (reliable){                           /* leave the gap and NACK for a resend */
            i_ramble_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,0,0);
        } else {
            r->deliver_upto = base + count;             /* best effort: no repair */
            i_ramble_reader_settle(r);
            i_ramble_lane_wake(st,(uint16_t)topic_index,(uint32_t)peer_slot);
        }
    }
}
#endif


void i_ramble_reader_data(RambleTransportState *st, int topic_index, int peer_slot, const uint8_t *p,
                           uint64_t now){
    i_RambleTopic *topic=&st->topics[topic_index];
    i_RambleReaderProxy *r=i_ramble_reader_proxy_at(st,topic_index,peer_slot);
    i_RambleAssembly *a;
    int reliable = (topic->qos.reliability==RAMBLE_RELIABLE);
    int new_frag = 0;
    uint64_t seqno, base; uint16_t frag, count, payload_len; uint32_t sample_len; const uint8_t *payload;
    if (p[0] & RAMBLE_F_SINGLE){           /* single fragment: frag, count and len are implied */
        seqno=i_ramble_le_r64(p+RAMBLE_OFFSET_SEQNO); frag=0; count=1; payload_len=i_ramble_le_r16(p+RAMBLE_OFFSET_PAYLOAD_LEN_SINGLE); sample_len=payload_len; payload=p+RAMBLE_HEADER_DATA_SINGLE;
    } else {
        seqno=i_ramble_le_r64(p+RAMBLE_OFFSET_SEQNO); frag=i_ramble_le_r16(p+RAMBLE_OFFSET_FRAG); count=i_ramble_le_r16(p+RAMBLE_OFFSET_COUNT);
        sample_len=i_ramble_le_r32(p+RAMBLE_OFFSET_SAMPLE_LEN); payload_len=i_ramble_le_r16(p+RAMBLE_OFFSET_PAYLOAD_LEN); payload=p+RAMBLE_HEADER_DATA_MULTI;
    }
    base = seqno - frag;

    if (!r || !r->used){ topic->repair_stats.frags_malformed++; return; }     /* not subscribed */
    if (count==0 || frag>=count){ topic->repair_stats.frags_malformed++; return; }  /* malformed */
    switch (i_ramble_reader_order_arrival(st, topic_index, peer_slot, r, base, seqno)){
        case RAMBLE_ORDER_OLD:     topic->repair_stats.frags_old++;   return;   /* already seen */
        case RAMBLE_ORDER_GAP:   /* a future sample: hold one ahead */
            a = i_ramble_reader_ahead(r, base);
            if (!a){ topic->repair_stats.frags_ahead++; return; }   /* hold taken */
            break;
        case RAMBLE_ORDER_ADOPTED: topic->repair_stats.frags_ahead++; r->cur.active=0; a=&r->cur; break;
        case RAMBLE_ORDER_INORDER:
            if (r->parked){ topic->repair_stats.frags_dup++; return; }   /* the held sample */
            a=&r->cur; break;
    }
    r->started = 1;   /* the writer is engaged */
    /* too big means the allocation failed: the head is skipped and reported, a sample
       held ahead is simply not held and comes back in order later */
    if (!i_ramble_asm_fit(st, a, sample_len, count)){
        if (a == &r->next){ topic->repair_stats.frags_ahead++; return; }
        i_ramble_transport_fire_event(st, RAMBLE_TRANSPORT_MSG_TOO_BIG, (uint16_t)topic_index, st->peer_ids[peer_slot],
                    0, sample_len);
        r->deliver_upto = base + count; r->cur.active = 0;
        i_ramble_reader_settle(r);
        if (reliable) i_ramble_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,1,0);
        i_ramble_reader_deliver(st,(uint16_t)topic_index,(uint32_t)peer_slot);
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
    new_frag = !i_ramble_bit_get(a->bitmap,frag);
    if (new_frag){
        /* reassemble at the source peer's fragment size, the cap guard catches a stray offset */
        uint32_t offset=(uint32_t)frag*st->peer_frag[peer_slot];
        if (offset+payload_len<=a->cap) memcpy(a->buf+offset,payload,payload_len);
        i_ramble_bit_set(a->bitmap,frag);
        if (frag==a->low)                                    /* extended the contiguous front */
            while (a->low<count && i_ramble_bit_get(a->bitmap,a->low)) a->low++;
        if (r->rtt_probe && seqno == r->rtt_probe_seq){      /* the resend our probe timed */
            i_ramble_rtt_sample(st, (uint32_t)peer_slot, now > r->rtt_probe_us ? now - r->rtt_probe_us : 0u);
            r->rtt_probe = 0;
        }
    } else topic->repair_stats.frags_dup++;                  /* already held */
    if (a != &r->cur) return;    /* held ahead: it delivers when the head does */
    /* Deliver a complete head. A partial head re arms only when this frag opened a real
       gap below received_high, so a healthy in order fill owes nothing. */
    if (a->low == count) i_ramble_reader_deliver(st,(uint16_t)topic_index,(uint32_t)peer_slot);
    else if (reliable && new_frag && r->deliver_upto + a->low <= r->received_high)
        i_ramble_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,0,0);
}


void i_ramble_reader_hb(RambleTransportState *st, int topic_index, int peer_slot, const uint8_t *p, uint64_t now){
    i_RambleTopic *topic=&st->topics[topic_index];
    i_RambleReaderProxy *r=i_ramble_reader_proxy_at(st,topic_index,peer_slot);
    uint64_t first=i_ramble_le_r64(p+RAMBLE_OFFSET_SEQNO), last=i_ramble_le_r64(p+RAMBLE_OFFSET_HB_LAST);
    (void)now;
    if (!r || !r->used) return;
    if (topic->qos.reliability!=RAMBLE_RELIABLE) return;
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
            i_ramble_transport_fire_event(st, RAMBLE_TRANSPORT_MSG_LOST, (uint16_t)topic_index, st->peer_ids[peer_slot],
                        r->deliver_upto, to - r->deliver_upto);
            topic->repair_stats.msgs_skipped += to - r->deliver_upto;
        }
        r->deliver_upto=to; r->cur.active=0; r->rtt_probe=0;
        r->parked=0;            /* the writer moved past the held sample */
#ifdef RAMBLE_SHM
        r->parked_shm=0;
        r->shm_fail=0;          /* a fresh count */
#endif
        /* the floor may have landed on the sample held ahead: it is the head now */
        i_ramble_reader_settle(r);
        i_ramble_reader_deliver(st,(uint16_t)topic_index,(uint32_t)peer_slot);
        r=i_ramble_reader_proxy_at(st,topic_index,peer_slot);
    }
    if (r->parked) return;      /* still parked: silent, the writer's flow control backpressures */
    /* hb_last is the writer's claim and only the slow backstop chases it, for tail loss.
       The HB always owes a cumulative ack so a writer that lost ours stops pinging. */
    r->hb_last=last;
    i_ramble_reader_ack_now(st,topic,r,(uint16_t)topic_index,(uint32_t)peer_slot,1,1);
}


/* The ACKNACK for a lane if due, 0 if none. Repair is gap triggered, bounded by what was
 * received and deduped in flight. See spec/transport.md. */
size_t i_ramble_reader_emit(RambleTransportState *st, int topic_index, int peer_slot, uint8_t *out, size_t cap, uint64_t now){
    i_RambleTopic *topic=&st->topics[topic_index];
    i_RambleReaderProxy *r=i_ramble_reader_proxy_at(st,topic_index,peer_slot);
    uint64_t first_missing, bound, top; uint16_t nbits=0; uint32_t bitmap=0;
    uint16_t index = i_ramble_wire_index_of(st, topic_index);
    int due, holes=0, repair=0, force;
    if (!r || !r->used || st->peer_dormant[peer_slot]) return 0;   /* unmatched or dormant */
    if (topic->qos.reliability!=RAMBLE_RELIABLE) return 0;
    if (cap<RAMBLE_HEADER_NACK) return 0;
    if (!r->ack_pending || now<r->ack_due_us) return 0;
    r->ack_pending=0; force=r->ack_force; r->ack_force=0;
    if (r->parked) return 0;    /* parked: silent, and the lane stays unscheduled */

    if (!r->started)   /* no position yet: F_UNPOS announces our epoch */
        return i_ramble_wire_mk_nack(out,index,r->deliver_upto,0,0,r->epoch,(uint8_t)RAMBLE_F_UNPOS);

    /* the cumulative ack point and repair window base: our contiguous front */
    first_missing = r->cur.active ? r->deliver_upto + r->cur.low : r->deliver_upto;

    /* only the slow backstop may reach the writer's claim, so tail loss still repairs */
    due   = (now >= r->nack_retransmit_us);
    bound = r->received_high;
    if (due && r->hb_last > bound) bound = r->hb_last;

    if (first_missing <= bound){                         /* a hole sits below something we heard */
        holes = 1;
        top = first_missing + RAMBLE_NACK_WINDOW;          /* one window per ACKNACK */
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
                if (i_ramble_asm_missing(&r->cur, s) && i_ramble_asm_missing(&r->next, s)){
                    bitmap |= (1u << (uint32_t)(s - first_missing));
                    if (!have_probe && s >= asked_high){ probe = s; have_probe = 1; }
                }
            }
            if (bitmap){
                /* the backstop: the peer's measured round trip unless the qos pins it */
                uint32_t rto = topic->qos.repair_delay_us ? topic->qos.repair_delay_us
                             : i_ramble_rtt_rto(st, (uint32_t)peer_slot, RAMBLE_QOS_DEF_REPAIR_US);
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
    if (holes){ r->ack_pending=1; r->ack_due_us=r->nack_retransmit_us; i_ramble_transport_arm_deadline(st,r->ack_due_us); }

    /* only a repair request or a forced cumulative ack is worth a datagram */
    if (!repair && !force) return 0;
    return i_ramble_wire_mk_nack(out,index,first_missing,nbits,bitmap,r->epoch,0);
}


/* Retries every parked lane of a topic. The callbacks may re enter the transport, so lane
 * pointers are re derived after each call. */
uint32_t ramble_transport_deliver_parked(RambleTransportState *st, uint16_t topic_index, uint64_t now){
    i_RambleTopic *topic = i_ramble_topic_at(st, topic_index, NULL);
    uint32_t li, still = 0;
    (void)now;
    if (!topic) return 0;
    li = topic->lane_head;
    while (li != RAMBLE__NIL){
        uint32_t next = st->lanes[li].topic_next;
        if (st->lanes[li].r.used && st->lanes[li].r.parked){
            uint32_t peer_slot = st->lanes[li].peer_slot;
            uint32_t peer_id   = st->peer_ids[peer_slot];
            i_RambleReaderProxy *r = &st->lanes[li].r;
            int accepted;
#ifdef RAMBLE_SHM
            if (r->parked_shm){
                int ok = st->cfg.on_shm ?
                         st->cfg.on_shm(st->cfg.user, topic_index, peer_id, r->cur.buf) : 0;
                r = &st->lanes[li].r;                  /* the callback may re enter */
                if (ok == 0){
                    /* the chunk is gone: un park, leave the gap, and let the repair path re
                       fetch or skip it */
                    r->parked = 0; r->parked_shm = 0; r->cur.active = 0;
                    i_ramble_reader_ack_now(st,topic,r,topic_index,peer_slot,0,0);
                    li = next; continue;
                }
                accepted = (ok > 0);
                if (accepted){
                    r->deliver_upto = r->cur.base + r->cur.count;
                    r->cur.active = 0; r->parked = 0; r->parked_shm = 0; r->shm_fail = 0; r->lapped = 0;
                    i_ramble_reader_settle(r);
                    if (topic->qos.reliability==RAMBLE_RELIABLE)    /* ack after delivery */
                        i_ramble_reader_ack_now(st,topic,r,topic_index,peer_slot,1,0);
                    i_ramble_reader_deliver(st,topic_index,peer_slot);
                }
            } else
#endif
            {
                /* un park and run the ordinary delivery, which re parks on a refusal */
                r->parked = 0;
                i_ramble_reader_deliver(st,topic_index,peer_slot);
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


/* The wire name is the whole name, so the identity recomputed from it equals ramble_topic_id. */
static uint64_t i_ramble_identity_hash(const uint8_t *name, size_t n){ return i_ramble_fnv1a64(name, n); }

uint64_t ramble_topic_id(const char *name){ return i_ramble_fnv1a64_str(name); }

uint64_t ramble_topic_identity(const RambleTopicDef *def){
    return ramble_topic_id(def->name);
}

static size_t i_ramble_name_len(const char *s){            /* capped strlen */
    size_t n = 0;
    if (s) while (s[n] && n < RAMBLE_TOPIC_NAME_MAX) n++;
    return n;
}


/* Applied once at init so the stored qos is authoritative. repair_delay_us 0 stays
   adaptive and is resolved per NACK in i_ramble_reader_emit. */
static void i_ramble_qos_defaults(RambleQos *q){
    if (q->keep_last == 0)        q->keep_last       = q->reliability==RAMBLE_RELIABLE
                                                     ? RAMBLE_QOS_DEF_KEEP_LAST_REL
                                                     : RAMBLE_QOS_DEF_KEEP_LAST;
    if (q->heartbeat_us == 0)     q->heartbeat_us    = RAMBLE_QOS_DEF_HEARTBEAT_US;
}


uint16_t ramble_clamp_frag(uint16_t frag_size){
    uint16_t f = frag_size ? frag_size : RAMBLE_FRAG_SIZE;
    if (f < RAMBLE_FRAG_SIZE_MIN) f = RAMBLE_FRAG_SIZE_MIN;
    if (f > RAMBLE_FRAG_SIZE_MAX) f = RAMBLE_FRAG_SIZE_MAX;
    return f;
}

uint16_t ramble_transport_frag(RambleTransportState *st){ return st ? st->frag : ramble_clamp_frag(0); }


/* Lays everything out, measure mode when b->base is NULL. The arena holds only the fixed
   tables. Message buffers, reassembly state, index maps and lane records are hook allocations. */
static RambleTransportState *i_ramble_transport_build(i_RambleBump *b, const RambleConfig *cfg){
    uint16_t c; uint32_t max_peers = cfg->max_peers, n_topics = cfg->n_topics;
    uint16_t bitmap_len = (uint16_t)((n_topics+7u)/8u);
    uint32_t name_bytes = 0; char *name_pool = NULL;
    RambleTransportState *st = (RambleTransportState*)i_ramble_bump_take(b, sizeof(RambleTransportState), 16);
    if (st && b->base) memset(st, 0, sizeof(*st));

    /* one fixed name slot per topic, so a reserve slot can be named later */
    name_bytes = (uint32_t)n_topics * (RAMBLE_TOPIC_NAME_MAX + 1u);

    { uint32_t nlanes = n_topics*max_peers, ndest = max_peers;
      uint32_t *peer_ids = (uint32_t*)i_ramble_bump_take(b, max_peers*sizeof(uint32_t), 8);
      uint8_t  *peer_used = (uint8_t*) i_ramble_bump_take(b, max_peers*sizeof(uint8_t), 1);
      uint8_t  *peer_dormant= (uint8_t*) i_ramble_bump_take(b, max_peers*sizeof(uint8_t), 1);
      uint16_t *peer_frag = (uint16_t*)i_ramble_bump_take(b, max_peers*sizeof(uint16_t), 2);
      RamblePeerRtt *peer_rtt = (RamblePeerRtt*)i_ramble_bump_take(b, max_peers*sizeof(RamblePeerRtt), 8);
#ifdef RAMBLE_SHM
      uint8_t  *peer_shm= (uint8_t*) i_ramble_bump_take(b, max_peers*sizeof(uint8_t), 1);
#endif
      uint8_t  *peer_pub_bitmap = (uint8_t*) i_ramble_bump_take(b, (size_t)max_peers*bitmap_len, 1);
      uint8_t  *peer_sub_bitmap = (uint8_t*) i_ramble_bump_take(b, (size_t)max_peers*bitmap_len, 1);
      uint8_t  *peer_sub_reliable = (uint8_t*) i_ramble_bump_take(b, (size_t)max_peers*bitmap_len, 1);
      i_RambleTopic *topic = (i_RambleTopic*)i_ramble_bump_take(b, n_topics*sizeof(i_RambleTopic), 16);
      /* only the u16 ticket table lives in the arena, records are pool allocated per match */
      uint16_t   *lane_index = (uint16_t*)i_ramble_bump_take(b, (size_t)nlanes*sizeof(uint16_t), 2);
      uint32_t *dest_head = (uint32_t*)i_ramble_bump_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint32_t *dest_tail = (uint32_t*)i_ramble_bump_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint8_t  *dest_queued = (uint8_t*) i_ramble_bump_take(b, (size_t)ndest, 1);
      uint32_t *dest_queue = (uint32_t*)i_ramble_bump_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      /* index maps: a per peer pointer and length, each map allocated on demand */
      uint16_t **peer_index    = (uint16_t**)i_ramble_bump_take(b, (size_t)max_peers*sizeof(uint16_t*), 8);
      uint32_t *peer_index_len = (uint32_t*) i_ramble_bump_take(b, (size_t)max_peers*sizeof(uint32_t), 8);
      uint8_t **peer_astate    = (uint8_t**) i_ramble_bump_take(b, (size_t)max_peers*sizeof(uint8_t*), 8);
      uint8_t **peer_agen      = (uint8_t**) i_ramble_bump_take(b, (size_t)max_peers*sizeof(uint8_t*), 8);
      uint8_t **peer_attrs     = (uint8_t**) i_ramble_bump_take(b, (size_t)max_peers*sizeof(uint8_t*), 8);
      uint32_t *peer_seen_version = (uint32_t*)i_ramble_bump_take(b, (size_t)max_peers*sizeof(uint32_t), 8);
      name_pool = (char*)i_ramble_bump_take(b, name_bytes ? name_bytes : 1u, 1);
      if (st && b->base){
          st->cfg=*cfg; st->peer_ids=peer_ids; st->peer_used=peer_used;
          st->peer_dormant=peer_dormant; st->peer_frag=peer_frag;
          st->peer_rtt=peer_rtt; memset(peer_rtt, 0, max_peers*sizeof(RamblePeerRtt));
          st->frag = ramble_clamp_frag(cfg->frag_size);
          st->peer_pub_bitmap=peer_pub_bitmap; st->peer_sub_bitmap=peer_sub_bitmap;
          st->peer_sub_reliable=peer_sub_reliable; st->bitmap_len=bitmap_len;
          st->topics=topic; st->reader_epoch_counter=1;
          st->next_deadline_us=RAMBLE__NO_DEADLINE;
          st->lanes=NULL; st->lane_cap=0u; st->lane_free=RAMBLE__NIL;
          st->lane_index=lane_index;
          st->dest_head=dest_head; st->dest_tail=dest_tail; st->dest_queued=dest_queued; st->dest_queue=dest_queue;
          st->peer_index=peer_index; st->peer_index_len=peer_index_len;
          st->peer_astate=peer_astate;
          st->peer_agen=peer_agen; st->peer_attrs=peer_attrs;
          st->peer_seen_version=peer_seen_version;
          memset(peer_used,0,max_peers); memset(peer_dormant,0,max_peers);
          { uint32_t k; for (k=0;k<max_peers;k++) peer_frag[k]=RAMBLE_FRAG_SIZE; }
#ifdef RAMBLE_SHM
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
          memset(dest_head,0xFF,(size_t)ndest*sizeof(uint32_t));   /* all RAMBLE__NIL */
      }
    }

    for (c=0;c<n_topics;c++){
        /* every slot starts inactive with its own name slot */
        if (st && b->base){
            i_RambleTopic *topic = &st->topics[c];
            memset(topic,0,sizeof(*topic));
            topic->role = RAMBLE_INACTIVE;
            topic->lane_head = RAMBLE__NIL;
            topic->name = name_pool + (size_t)c*(RAMBLE_TOPIC_NAME_MAX + 1u);
            ((char*)topic->name)[0] = '\0';
        }
        if (!cfg->topics) continue;    /* reserve mode: slots are filled by topic_define later */
        {   const RambleTopicDef *def = &cfg->topics[c];
            RambleQos q = def->qos;            /* a normalized copy */
            i_RambleWriterSample *history; uint16_t depth;
            i_ramble_qos_defaults(&q);
            depth = q.keep_last;
            history = (i_RambleWriterSample*)i_ramble_bump_take(b, depth*sizeof(i_RambleWriterSample), 16);
            if (st && b->base){
                i_RambleTopic *topic = &st->topics[c];
                size_t lane = i_ramble_name_len(def->name);
                topic->qos=q;
                topic->role=def->role;
                topic->kind=def->kind; topic->prefix_bytes=def->prefix_bytes; topic->directed=def->directed;
                topic->attrs=def->attrs;
                topic->identity = ramble_topic_identity(def);
                if (lane){ memcpy((char*)topic->name, def->name, lane); ((char*)topic->name)[lane]='\0'; }
                topic->name_len = (uint8_t)lane;
                topic->history=history; topic->history_owned=0; topic->history_head=0; topic->next_seqno=0; topic->have_first=0;
                memset(history,0,depth*sizeof(i_RambleWriterSample));   /* buffers grow lazily */
            }
        }
    }
    return st;
}


size_t ramble_transport_required_memory(const RambleConfig *cfg){
    i_RambleBump b; memset(&b,0,sizeof b);
    if (!cfg || cfg->n_topics==0 || cfg->max_peers==0) return 0;
    i_ramble_transport_build(&b, cfg);
    return b.offset + 16;   /* slack for base alignment */
}


RambleTransportState *ramble_transport_init(void *mem, size_t cap, const RambleConfig *cfg){
    i_RambleBump b; RambleTransportState *st; uint16_t i;
    if (!mem || !cfg || cfg->n_topics==0 || cfg->max_peers==0) return NULL;
    if (!cfg->allocator) return NULL;    /* the allocator is the one memory model */
    if (cfg->topics) for (i=0;i<cfg->n_topics;i++){
        const RambleTopicDef *d = &cfg->topics[i];
        size_t lane = 0; uint16_t j;
        if (!d->name || !d->name[0]) return NULL;          /* the name is the identity */
        while (d->name[lane]) lane++;
        if (lane > RAMBLE_TOPIC_NAME_MAX) return NULL;           /* the wire name is the whole name */
        if (d->directed && d->qos.catch_up) return NULL;   /* directed history never replays */
        for (j=0;j<i;j++)   /* same name under a different kind: the maps bind by identity, so
                               local twins would cross bind. See spec/interest.md */
            if (cfg->topics[j].kind != d->kind &&
                ramble_topic_id(cfg->topics[j].name) == ramble_topic_id(d->name)) return NULL;
    }
    memset(&b,0,sizeof b);
    b.base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    b.cap  = cap - (size_t)((uint8_t*)b.base - (uint8_t*)mem);
    st = i_ramble_transport_build(&b, cfg);
    if (!st || b.oom) return NULL;
    st->cfg.topics = NULL;   /* only read during init */
    return st;
}


/* Heap buffers stay put and the struct copies carry their pointers. The scheduler is
 * rebuilt from proxy state. The caller frees old's arena but must not destroy old. */
RambleTransportState *ramble_transport_migrate(RambleTransportState *old, void *new_mem, size_t new_cap,
                        uint16_t new_max_peers, uint16_t new_n_topics){
    RambleConfig nc; RambleTransportState *nw; uint16_t omp, onc, c, p;
    if (!old) return NULL;
    nc = old->cfg; nc.topics = NULL;
    nc.max_peers = new_max_peers; nc.n_topics = new_n_topics;
    nw = ramble_transport_init(new_mem, new_cap, &nc);
    if (!nw) return NULL;
    omp = old->cfg.max_peers; onc = old->cfg.n_topics;

    nw->reader_epoch_counter = old->reader_epoch_counter;
    nw->frag = old->frag;
    memcpy(nw->peer_ids,     old->peer_ids,     (size_t)omp*sizeof(uint32_t));
    memcpy(nw->peer_used,    old->peer_used,    omp);
    memcpy(nw->peer_dormant, old->peer_dormant, omp);
    memcpy(nw->peer_frag,    old->peer_frag,    (size_t)omp*sizeof(uint16_t));
    memcpy(nw->peer_rtt,     old->peer_rtt,     (size_t)omp*sizeof(RamblePeerRtt));
#ifdef RAMBLE_SHM
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
            if (nw->lanes[li].in_use){ nw->lanes[li].queued=0; nw->lanes[li].sched_next=RAMBLE__NIL; }
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
        for (p=0;p<omp;p++) if (nw->peer_used[p]) i_ramble_lane_wake(nw, c, p);
    nw->next_deadline_us = 0;
    return nw;
}


int i_ramble_peer_slot(RambleTransportState *st, uint32_t id){
    uint16_t i;
    for (i=0;i<st->cfg.max_peers;i++) if (st->peer_used[i] && st->peer_ids[i]==id) return (int)i;
    return -1;
}

/* the local handle is the index */
i_RambleTopic *i_ramble_topic_at(RambleTransportState *st, uint16_t topic_index, int *idx_out){
    if (topic_index >= st->cfg.n_topics) return NULL;
    if (idx_out) *idx_out = (int)topic_index;
    return &st->topics[topic_index];
}

/* Prefers a non INACTIVE match so a parked twin never shadows a live one, else the first
 * match so resolution stays deterministic. A RETIRED slot is invisible here. */
static i_RambleTopic *i_ramble_topic_by_identity(RambleTransportState *st, uint64_t identity, int *idx_out){
    uint16_t i; int first=-1;
    for (i=0;i<st->cfg.n_topics;i++){
        if (st->topics[i].retired || st->topics[i].identity!=identity) continue;
        if (first<0) first=(int)i;
        if (st->topics[i].role!=RAMBLE_INACTIVE){ if(idx_out)*idx_out=(int)i; return &st->topics[i]; }
    }
    if (first>=0){ if(idx_out)*idx_out=first; return &st->topics[first]; }
    return NULL;
}


/* first and count are the kind's two numeric slots, routed to the named fields. */
void i_ramble_transport_fire_event(RambleTransportState *st, RambleTransportEventKind kind, uint16_t topic_index,
                        uint32_t peer, uint64_t first, uint64_t count){
    RambleTransportEvent ev;
    if (!st->cfg.on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind=kind; ev.topic=topic_index; ev.peer=peer; ev.user=st->cfg.user;
    switch (kind){
    case RAMBLE_TRANSPORT_MSG_LOST:       ev.lost_first = first; ev.lost_count = count; break;
    case RAMBLE_TRANSPORT_MSG_TOO_BIG:    ev.too_big_bytes = count; break;
    case RAMBLE_TRANSPORT_NAME_COLLISION: ev.identity = first; break;
    case RAMBLE_TRANSPORT_SCHEMA_MISMATCH: ev.peer_is_pub = (uint8_t)first; break;
    default: break;
    }
    st->cfg.on_event(&ev);
}


/* the head minus catch_up cached samples, reliable only */
uint64_t i_ramble_topic_unicast_join_seqno(const i_RambleTopic *topic){
    uint16_t depth = topic->qos.keep_last;   /* normalized at init */
    uint16_t want = topic->qos.catch_up, k, i;
    uint64_t s = topic->next_seqno;
    if (topic->qos.reliability != RAMBLE_RELIABLE || want == 0) return s;
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
static i_RambleLane *i_ramble_lane_ensure(RambleTransportState *st, uint16_t c, uint32_t peer_slot){
    size_t k = (size_t)c*st->cfg.max_peers + peer_slot;
    uint32_t li;
    li = st->lane_index[k]==0xFFFFu ? RAMBLE__NIL : (uint32_t)st->lane_index[k];
    if (li != RAMBLE__NIL) return &st->lanes[li];
    if (st->lane_free == RAMBLE__NIL){                  /* pool dry: grow it */
        uint32_t ncap = st->lane_cap ? st->lane_cap*2u : 8u, i;
        i_RambleLane *nl;
        if (ncap > 0xFFFFu) ncap = 0xFFFFu;           /* the u16 ticket space */
        if (ncap <= st->lane_cap) return NULL;        /* 65535 live matches: refuse */
        nl = (i_RambleLane*)st->cfg.allocator(st->cfg.user, st->lanes, (size_t)ncap*sizeof(i_RambleLane));
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
    memset(&st->lanes[li], 0, sizeof(i_RambleLane));
    st->lanes[li].topic = c; st->lanes[li].peer_slot = (uint16_t)peer_slot;
    st->lanes[li].sched_next = RAMBLE__NIL; st->lanes[li].topic_next = RAMBLE__NIL;
    st->lanes[li].in_use = 1;
    st->lane_index[k] = (uint16_t)li;
    return &st->lanes[li];
}

/* free one assembly slot's grown buffers */
static void i_ramble_asm_free(RambleTransportState *st, i_RambleAssembly *a){
    if (a->buf){ st->cfg.allocator(st->cfg.user, a->buf, 0); a->buf=NULL; a->cap=0; }
    if (a->bitmap){ st->cfg.allocator(st->cfg.user, a->bitmap, 0); a->bitmap=NULL; a->bitmap_cap=0; }
    a->active=0;
}

/* A lane with neither side matched leaves the topic chain, frees its buffers, leaves the
 * scheduler and recycles its record. A no op while a side is matched. */
static void i_ramble_lane_release(RambleTransportState *st, uint16_t c, uint32_t peer_slot){
    uint32_t li = i_ramble_lane_id(st, c, peer_slot);
    i_RambleLane *l;
    if (li == RAMBLE__NIL) return;
    l = &st->lanes[li];
    if (l->w.used || l->r.used) return;
    {   uint32_t *pp = &st->topics[c].lane_head;    /* unlink from the topic chain */
        while (*pp != RAMBLE__NIL && *pp != li) pp = &st->lanes[*pp].topic_next;
        if (*pp == li) *pp = l->topic_next;
    }
    l->topic_next = RAMBLE__NIL;
    i_ramble_asm_free(st, &l->r.cur); i_ramble_asm_free(st, &l->r.next);
    i_ramble_sched_drop(st, li);
    l->in_use = 0;
    l->sched_next = st->lane_free; st->lane_free = li;
    st->lane_index[(size_t)c*st->cfg.max_peers + peer_slot] = 0xFFFF;
}

/* match one lane side */
static void i_ramble_writer_match(RambleTransportState *st, uint16_t c, uint16_t peer_slot, i_RambleLane *l){
    i_RambleTopic *topic=&st->topics[c];
    i_RambleWriterProxy *w=&l->w;
    memset(w,0,sizeof(*w));
    w->used=1;
    topic->matched_writers++;   /* a genuine 0 to 1, rematch guards on used */
    /* only a reliable reader acks. A best effort one stays out of flow control so it can
       never stall this writer */
    w->reader_reliable = i_ramble_bit_get(&st->peer_sub_reliable[(size_t)peer_slot*st->bitmap_len], c) ? 1u : 0u;
    w->sent_upto = i_ramble_topic_unicast_join_seqno(topic);
    w->acked_upto = w->sent_upto;
    w->wire_skip = w->sent_upto;   /* the private wire seqno starts at 0 */
    i_ramble_lane_wake(st, c, peer_slot);   /* primed for new data and ack or hb */
}

static void i_ramble_writer_unmatch(RambleTransportState *st, uint16_t c, i_RambleLane *l){
    if (!l->w.used) return;
    l->w.used=0;
    st->topics[c].matched_writers--;   /* exactly one 1 to 0 per unmatch */
}

static void i_ramble_reader_match(RambleTransportState *st, uint16_t c, uint16_t peer_slot, i_RambleLane *l){
    i_RambleReaderProxy *r=&l->r;
    i_RambleAssembly cur=r->cur, next=r->next;   /* keep the grown buffers across rematch */
    memset(r,0,sizeof(*r));
    r->cur=cur; r->next=next; r->cur.active=0; r->next.active=0;
    r->epoch=st->reader_epoch_counter++;   /* a new incarnation: writers re join on seeing it */
    r->used=1;       /* started 0: the first DATA adopts the writer's position */
    st->topics[c].matched_readers++;   /* a genuine 0 to 1, rematch guards on used */
    /* announce the incarnation once so an idle writer re joins and replays. A discovery
       blip keeps its position through dormant and resume and never lands here. */
    if (st->topics[c].qos.reliability==RAMBLE_RELIABLE){
        r->ack_pending=1; r->ack_due_us=0; r->ack_force=1;
        i_ramble_lane_wake(st,c,peer_slot);
    }
}

static void i_ramble_reader_unmatch(RambleTransportState *st, uint16_t c, i_RambleLane *l){
    if (l->r.used) st->topics[c].matched_readers--;   /* peer_remove calls this unconditionally */
    l->r.used=0; l->r.cur.active=0; l->r.next.active=0;
}


/* Recomputes one lane from our role and the peer's bits. Only a changed side is touched,
 * so reader positions survive a re apply. */
static void i_ramble_topic_rematch(RambleTransportState *st, uint16_t c, uint16_t peer_slot){
    i_RambleTopic *topic=&st->topics[c];
    const uint8_t *peer_pub_bitmap=&st->peer_pub_bitmap[(size_t)peer_slot*st->bitmap_len];
    const uint8_t *peer_sub_bitmap=&st->peer_sub_bitmap[(size_t)peer_slot*st->bitmap_len];
    int wuse = ramble_role_pubs(topic->role) && i_ramble_bit_get(peer_sub_bitmap,c);
    int ruse = ramble_role_subs(topic->role) && i_ramble_bit_get(peer_pub_bitmap,c);
    i_RambleLane *l;
    /* the rebind hold: nothing goes to a peer until it proved it applied our announce at
       the slot's rebind version. Inbound needs no hold, our own verdicts were re pended. */
    if (wuse && topic->rebind_version && st->peer_seen_version[peer_slot] < topic->rebind_version)
        wuse = 0;
    l = i_ramble_lane_at(st,c,peer_slot);
    int had = l && (l->w.used || l->r.used);
    if (!wuse && !ruse){
        if (!had) return;
        i_ramble_writer_unmatch(st,c,l);
        i_ramble_reader_unmatch(st,c,l);
        i_ramble_lane_release(st,c,peer_slot);
        return;
    }
    if (!l){
        l = i_ramble_lane_ensure(st,c,peer_slot);   /* may relocate the pool */
        if (!l) return;                             /* OOM: refused, the next announce retries */
    }
    if (wuse && !l->w.used) i_ramble_writer_match(st,c,peer_slot,l);
    else if (!wuse && l->w.used) i_ramble_writer_unmatch(st,c,l);
    if (ruse && !l->r.used) i_ramble_reader_match(st,c,peer_slot,l);
    else if (!ruse && l->r.used) i_ramble_reader_unmatch(st,c,l);
    if (!had && (l->w.used || l->r.used)){        /* a first match: onto the topic chain */
        l->topic_next = topic->lane_head;
        topic->lane_head = i_ramble_lane_id(st,c,peer_slot);
    } else if (had && !(l->w.used || l->r.used)){
        i_ramble_lane_release(st,c,peer_slot);
    }
}


void ramble_transport_peer_add(RambleTransportState *st, uint32_t id, uint16_t peer_frag){
    uint16_t i; int free=-1; uint32_t max_peers=st->cfg.max_peers;
    if (i_ramble_peer_slot(st,id)>=0) return;
    for (i=0;i<max_peers;i++) if(!st->peer_used[i]){free=(int)i;break;}
    if (free<0) return;
    st->peer_used[free]=1; st->peer_ids[free]=id;
    memset(&st->peer_rtt[free], 0, sizeof(RamblePeerRtt));   /* a new peer starts unmeasured */
    st->peer_dormant[free]=0;
    st->peer_frag[free]=ramble_clamp_frag(peer_frag);
#ifdef RAMBLE_SHM
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


void ramble_transport_peer_remove(RambleTransportState *st, uint32_t id){
    int s = i_ramble_peer_slot(st,id); uint16_t c;
    if (s<0) return;
    for (c=0;c<st->cfg.n_topics;c++){
        i_RambleLane *l = i_ramble_lane_at(st,c,(uint32_t)s);
        if (!l) continue;
        i_ramble_writer_unmatch(st,c,l);
        i_ramble_reader_unmatch(st,c,l);
        /* release frees the lane's buffers too, so a gone peer keeps no memory */
        i_ramble_lane_release(st,c,(uint32_t)s);
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
    memset(&st->peer_rtt[s], 0, sizeof(RamblePeerRtt));
#ifdef RAMBLE_SHM
    st->peer_shm[s]=0;
#endif
}

/* The per peer round trip estimator. See spec/transport.md. */

/* Folds one unambiguous sample with the RFC 6298 weights. The first seeds both. */
void i_ramble_rtt_sample(RambleTransportState *st, uint32_t peer_slot, uint64_t sample_us){
    RamblePeerRtt *e = &st->peer_rtt[peer_slot];
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

/* smoothed plus max(one tick, 4 x deviation), never under RAMBLE_RTO_MIN_US, and
 * fallback_us until the first sample */
uint32_t i_ramble_rtt_rto(RambleTransportState *st, uint32_t peer_slot, uint32_t fallback_us){
    const RamblePeerRtt *e = &st->peer_rtt[peer_slot];
    uint64_t var, rto;
    if (e->samples == 0) return fallback_us;
    var = 4ull * e->rtt_jitter_us;
    if (var < RAMBLE_RTO_GRAIN_US) var = RAMBLE_RTO_GRAIN_US;
    rto = (uint64_t)e->rtt_us + var;
    if (rto < RAMBLE_RTO_MIN_US) rto = RAMBLE_RTO_MIN_US;
    return rto > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)rto;
}

int ramble_transport_peer_rtt(RambleTransportState *st, uint32_t peer_id, RamblePeerRtt *out){
    int s = st ? i_ramble_peer_slot(st, peer_id) : -1;
    if (out) memset(out, 0, sizeof *out);
    if (s < 0) return 0;
    if (out) *out = st->peer_rtt[s];
    return 1;
}


/* Keeps every proxy and reader position, drops the peer from flow control. */
void ramble_transport_peer_dormant(RambleTransportState *st, uint32_t id){
    int s = i_ramble_peer_slot(st,id);
    if (s>=0) st->peer_dormant[s]=1;
}


/* Re includes the peer and re reports each reader position, so the writer fills any gap
 * and the reader dedups any replay. */
void ramble_transport_peer_resume(RambleTransportState *st, uint32_t id){
    int s = i_ramble_peer_slot(st,id); uint16_t c;
    if (s<0) return;
    st->peer_dormant[s]=0;
    for (c=0;c<st->cfg.n_topics;c++){
        i_RambleLane *l=i_ramble_lane_at(st,c,(uint32_t)s);
        if (!l) continue;
        if (st->topics[c].qos.reliability!=RAMBLE_RELIABLE) continue;
        if (l->r.used){ l->r.ack_pending=1; l->r.ack_due_us=0; l->r.ack_force=1; }
        if (l->w.used || l->r.used) i_ramble_lane_wake(st,c,(uint16_t)s);
    }
}


void ramble_transport_peer_set_frag(RambleTransportState *st, uint32_t id, uint16_t peer_frag){
    int s = i_ramble_peer_slot(st,id);
    if (s>=0) st->peer_frag[s]=ramble_clamp_frag(peer_frag);
}

#ifdef RAMBLE_SHM

void ramble_transport_peer_set_shm(RambleTransportState *st, uint32_t id, int is_shm){
    int s = i_ramble_peer_slot(st,id);
    if (s>=0) st->peer_shm[s]=(uint8_t)(is_shm?1:0);
}
#endif


void ramble_transport_destroy(RambleTransportState *st){
    uint16_t c; uint32_t li;
    if (!st) return;
    for (c=0;c<st->cfg.n_topics;c++){
        i_RambleTopic *topic=&st->topics[c];     /* an undefined slot has no ring */
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
        i_RambleLane *l=&st->lanes[li];
        if (!l->in_use) continue;
        i_ramble_asm_free(st, &l->r.cur); i_ramble_asm_free(st, &l->r.next);
    }
    if (st->lanes){                              /* the record pool */
        st->cfg.allocator(st->cfg.user, st->lanes, 0);
        st->lanes=NULL; st->lane_cap=0; st->lane_free=RAMBLE__NIL;
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
static uint16_t *i_ramble_peer_index_ensure(RambleTransportState *st, int peer_slot, uint32_t need){
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
static int i_ramble_hash32_candidates(RambleTransportState *st, uint32_t h, int *idx_out){
    uint16_t i; int first=-1, live=-1, n=0;
    for (i=0;i<st->cfg.n_topics;i++){
        if (!i_ramble_topic_announced(&st->topics[i]) || (uint32_t)st->topics[i].identity != h) continue;
        n++;
        if (first<0) first=(int)i;
        if (live<0 && st->topics[i].role!=RAMBLE_INACTIVE) live=(int)i;
    }
    if (idx_out) *idx_out = live>=0 ? live : first;
    return n;
}


/* Worst case: 5 bytes per slot plus the rate and generation sections at every topic. */
size_t ramble_interest_max(uint16_t n_topics){
    return 2u + 5u * (size_t)n_topics + 2u + 4u * (size_t)n_topics
         + 2u + 3u * (size_t)n_topics;
}

/* the rate section's membership test, shared by the count and write walks */
static int i_ramble_topic_rate_sub(const i_RambleTopic *t){
    return i_ramble_topic_announced(t) && t->qos.max_rate_hz && ramble_role_subs(t->role);
}

/* the generation section's membership test, shared by the count and write walks */
static int i_ramble_topic_rebound(const i_RambleTopic *t){
    return i_ramble_topic_announced(t) && t->gen != 0;
}


/* Serializes our interest, or measures it when out is NULL, in one walk so the two agree
 * byte for byte. The layout is in spec/interest.md. */
static size_t i_ramble_interest_emit(RambleTransportState *st, uint8_t *out, size_t cap){
    uint8_t *e, *rp;
    uint16_t c, n=0, n_rates=0, n_gens=0;
    uint32_t cells=0; int in_hole=0; size_t len;
    for (c=0;c<st->cfg.n_topics;c++) if (i_ramble_topic_announced(&st->topics[c])) n=(uint16_t)(c+1u);
    for (c=0;c<n;c++){   /* one pass: the exact cell count and the section counts */
        const i_RambleTopic *topic = &st->topics[c];
        if (i_ramble_topic_announced(topic)){ cells++; in_hole=0; }
        else { if (!in_hole) cells++; in_hole=1; }
        if (i_ramble_topic_rate_sub(topic)) n_rates++;
        if (i_ramble_topic_rebound(topic))  n_gens++;
    }
    len = 2u + 5u*(size_t)cells + 2u + 4u*(size_t)n_rates + 2u + 3u*(size_t)n_gens;
    if (!out) return len;
    if (cap < len) return 0;
    i_ramble_le_w16(out, n);
    e = out + 2u;
    for (c=0;c<n;c++){
        const i_RambleTopic *topic = &st->topics[c];
        if (!i_ramble_topic_announced(topic)){
            uint32_t run = 1;
            while ((uint16_t)(c+run) < n && !i_ramble_topic_announced(&st->topics[c+run])) run++;
            i_ramble_le_w32(e, run);
            e[4] = (uint8_t)(RAMBLE_INACTIVE | RAMBLE__INT_HOLE_RUN);
            e += 5u; c = (uint16_t)(c + run - 1u);
            continue;
        }
        i_ramble_le_w32(e, (uint32_t)topic->identity);
        e[4] = (uint8_t)((topic->role & RAMBLE__INT_ROLE_MASK)
             | (topic->qos.reliability==RAMBLE_RELIABLE ? RAMBLE__INT_RELIABLE : 0u)
             | ((topic->kind << RAMBLE__INT_KIND_SHIFT) & RAMBLE__INT_KIND_MASK));
        e += 5u;
    }
    rp = e;
    i_ramble_le_w16(rp, n_rates); rp += 2u;
    for (c=0;c<n;c++){
        const i_RambleTopic *topic = &st->topics[c];
        if (i_ramble_topic_rate_sub(topic)){
            i_ramble_le_w16(rp, c); i_ramble_le_w16(rp+2, topic->qos.max_rate_hz); rp += 4u;
        }
    }
    i_ramble_le_w16(rp, n_gens); rp += 2u;
    for (c=0;c<n;c++){
        const i_RambleTopic *topic = &st->topics[c];
        if (i_ramble_topic_rebound(topic)){
            i_ramble_le_w16(rp, c); rp[2] = topic->gen; rp += 3u;
        }
    }
    return (size_t)(rp - out);
}

size_t ramble_transport_build_interest(RambleTransportState *st, void *out, size_t cap){
    return i_ramble_interest_emit(st, (uint8_t*)out, cap);
}

/* The entry stream's byte length when it accounts exactly n slots inside len, else 0.
 * Every parser walks this first, so a bad blob is rejected whole. */
static uint32_t i_ramble_interest_walk_len(const uint8_t *d, size_t len, uint16_t n){
    const uint8_t *e = d + 2u, *end = d + len;
    uint32_t a = 0;
    while (a < n){
        uint32_t step = 1;
        if (e + 5 > end) return 0;
        if (e[4] & RAMBLE__INT_HOLE_RUN){
            step = i_ramble_le_r32(e);
            if (step == 0 || step > (uint32_t)n - a) return 0;
        }
        e += 5u; a += step;
    }
    return (uint32_t)(e - d);
}

/* The two sparse sections after the entries, located in one pass. A section that does
 * not fit is absent along with everything after it. */
typedef struct { RambleBytes rate, gen; } i_RambleInterestSections;

static i_RambleInterestSections i_ramble_interest_sections(const uint8_t *d, size_t len,
                                                       uint32_t entries_len){
    static const uint8_t width[2] = { 4u, 3u };  /* (index, rate_hz) and (index, gen) */
    i_RambleInterestSections s; RambleBytes *view[2];
    size_t off = entries_len; int k;
    s.rate = s.gen = ramble_bytes(NULL, 0);
    view[0] = &s.rate; view[1] = &s.gen;
    for (k=0;k<2;k++){
        size_t bytes;
        if (len < off + 2u) break;                   /* no header: this one and the rest absent */
        bytes = (size_t)i_ramble_le_r16(d + off) * width[k];
        off += 2u;
        if (len < off + bytes) break;                /* entries truncated: the same */
        *view[k] = ramble_bytes(d + off, bytes);
        off += bytes;
    }
    return s;
}


/* Re derives the peer's bits from cached verdicts plus the entry's current flags, then
 * rematches every topic. Idempotent. The gates are in spec/interest.md. */
void ramble_transport_apply_peer_interest(RambleTransportState *st, uint32_t peer_id, RambleBytes blob){
    const uint8_t *d=blob.data, *e, *gp, *g_end;
    uint16_t n, c; uint32_t a, entries_len; int peer_slot=i_ramble_peer_slot(st,peer_id);
    uint8_t *peer_pub_bitmap, *peer_sub_bitmap, *peer_sub_reliable;
    uint16_t *amap; uint8_t *astate;
    i_RambleInterestSections sec;
    uint32_t unmappable = 0;
    if (peer_slot<0 || !d || blob.len<2) return;
    n = i_ramble_le_r16(d);
    entries_len = i_ramble_interest_walk_len(d, blob.len, n);
    if (!entries_len) return;                          /* truncated or malformed: reject whole */
    peer_pub_bitmap  =&st->peer_pub_bitmap[(size_t)peer_slot*st->bitmap_len];
    peer_sub_bitmap  =&st->peer_sub_bitmap[(size_t)peer_slot*st->bitmap_len];
    peer_sub_reliable=&st->peer_sub_reliable[(size_t)peer_slot*st->bitmap_len];
    memset(peer_pub_bitmap,0,st->bitmap_len); memset(peer_sub_bitmap,0,st->bitmap_len);
    memset(peer_sub_reliable,0,st->bitmap_len);
    amap   = n ? i_ramble_peer_index_ensure(st, peer_slot, n) : NULL;
    astate = amap ? st->peer_astate[peer_slot] : NULL;
    /* the generation data is consumed by the entry loop with a merge cursor, the rate
       values are applied after the rematch once lanes exist */
    sec = i_ramble_interest_sections(d, blob.len, entries_len);
    gp = g_end = NULL;
    if (sec.gen.data){ gp = sec.gen.data; g_end = gp + sec.gen.len; }
    e = d + 2u;
    for (a=0;a<n;a++,e+=5u){
        uint8_t flags = e[4], role = (uint8_t)(flags & RAMBLE__INT_ROLE_MASK);
        int their_pub, their_sub, rel;
        uint16_t cidx; i_RambleTopic *topic;
        if (flags & RAMBLE__INT_HOLE_RUN){ a += i_ramble_le_r32(e) - 1u; continue; }
        if (astate && a < st->peer_index_len[peer_slot] && st->peer_agen[peer_slot]){
            /* the generation gate: the peer rebound this position since our verdict, so it
               goes back to pending with the demux severed. Runs for INACTIVE entries too. */
            uint8_t g = 0;
            while (gp && gp + 3 <= g_end && i_ramble_le_r16(gp) < a) gp += 3;
            if (gp && gp + 3 <= g_end && i_ramble_le_r16(gp) == a) g = gp[2];
            if (st->peer_agen[peer_slot][a] != g){
                if (astate[a]){
                    astate[a] = 0; amap[a] = 0xFFFFu;
                    if (st->peer_attrs[peer_slot]) st->peer_attrs[peer_slot][a] = 0;
                }
                st->peer_agen[peer_slot][a] = g;
            }
        }
        if (role == RAMBLE_INACTIVE) continue;
        if (!astate || a >= st->peer_index_len[peer_slot]){
            /* no verdict storage: this entry can never verify, count it if it was a candidate */
            if (i_ramble_hash32_candidates(st, i_ramble_le_r32(e), NULL)) unmappable++;
            continue;
        }
        if (!(astate[a] & RAMBLE__AST_DETAILED) || !(astate[a] & RAMBLE__AST_NAME_OK))
            continue;                     /* pending, or a verified non match */
        cidx = amap[a];
        if (cidx >= st->cfg.n_topics) continue;      /* defensive: a stale map */
        topic = &st->topics[cidx];
        if ((uint32_t)topic->identity != i_ramble_le_r32(e)){
            /* the hash no longer names the bound identity: the position was rebound */
            astate[a] = 0; amap[a] = 0xFFFFu;
            if (st->peer_attrs[peer_slot]) st->peer_attrs[peer_slot][a] = 0;
            continue;
        }
        if (topic->role == RAMBLE_INACTIVE || topic->retired){
            /* the verdict bound the then live twin, so re verify against the live one */
            int resolved_index = (int)cidx;
            i_ramble_topic_by_identity(st, topic->identity, &resolved_index);
            if ((uint16_t)resolved_index != cidx){
                astate[a] = 0; amap[a] = 0xFFFFu;      /* pending again */
                if (st->peer_attrs[peer_slot]) st->peer_attrs[peer_slot][a] = 0;
                continue;
            }
            if (topic->retired) continue;   /* no live successor yet */
        }
        {   /* the kind gate: the same name under another kind is refused, never cross wired */
            uint8_t their_kind = (uint8_t)((flags & RAMBLE__INT_KIND_MASK) >> RAMBLE__INT_KIND_SHIFT);
            if (their_kind != topic->kind){
                i_ramble_transport_fire_event(st, RAMBLE_TRANSPORT_KIND_MISMATCH, cidx, peer_id, 0, 0);
                continue;
            }
        }
        their_pub = ramble_role_pubs(role);
        their_sub = ramble_role_subs(role);
        rel       = (flags & RAMBLE__INT_RELIABLE) != 0;
        if (their_pub){   /* their offered qos against our subscription */
            int ours_sub = ramble_role_subs(topic->role);
            if (ours_sub && topic->qos.reliability==RAMBLE_RELIABLE && !rel){
                i_ramble_transport_fire_event(st, RAMBLE_TRANSPORT_QOS_INCOMPATIBLE, cidx, peer_id, 0, 0);
            } else if (!(astate[a] & RAMBLE__AST_READ_OK)){
                i_ramble_transport_fire_event(st, RAMBLE_TRANSPORT_SCHEMA_MISMATCH, cidx, peer_id, 1, 0);
            } else {
                i_ramble_bit_set(peer_pub_bitmap,(uint32_t)cidx);
            }
        }
        if (their_sub){                                /* their requested qos, for our writer */
            if (!(astate[a] & RAMBLE__AST_WRITE_OK)){
                i_ramble_transport_fire_event(st, RAMBLE_TRANSPORT_SCHEMA_MISMATCH, cidx, peer_id, 0, 0);
            } else {
                i_ramble_bit_set(peer_sub_bitmap,(uint32_t)cidx);
                if (rel) i_ramble_bit_set(peer_sub_reliable,(uint32_t)cidx);
            }
        }
    }
    if (unmappable)   /* never silent: those topics can never deliver here */
        i_ramble_transport_fire_event(st, RAMBLE_TRANSPORT_INTEREST_OVERFLOW, 0, peer_id, 0, unmappable);
    for (c=0;c<st->cfg.n_topics;c++) i_ramble_topic_rematch(st,c,(uint16_t)peer_slot);

    /* per lane values, applied after the rematch so the lanes exist and re derived on
       every announce, since a fresh match memsets the proxy */
    if (amap && sec.rate.data){
        /* their best effort delivery cap for a topic they subscribe paces our lane */
        const uint8_t *p = sec.rate.data, *end = p + sec.rate.len;
        for (; p + 4u <= end; p += 4u){
            uint16_t their_idx = i_ramble_le_r16(p), rate_hz = i_ramble_le_r16(p+2), cidx;
            i_RambleWriterProxy *w;
            if (their_idx >= st->peer_index_len[peer_slot]) continue;
            cidx = amap[their_idx];
            if (cidx >= st->cfg.n_topics) continue;             /* unverified */
            w = i_ramble_writer_proxy_at(st, cidx, (uint32_t)peer_slot);
            if (w && w->used)
                w->rate_interval_us = rate_hz ? (1000000u / (uint32_t)rate_hz) : 0u;
        }
    }
    if (amap && st->peer_attrs[peer_slot]){
        /* NO_TIMESTAMP from the detail cache onto our reader lane, so delivery strips
           exactly what the writer prepended. Unset = stamped */
        const uint8_t *attrs = st->peer_attrs[peer_slot];
        for (a=0;a<st->peer_index_len[peer_slot];a++){
            uint16_t cidx; i_RambleReaderProxy *r;
            if (!(attrs[a] & RAMBLE_ATTR_NO_TIMESTAMP)) continue;
            cidx = amap[a];
            if (cidx >= st->cfg.n_topics) continue;             /* unverified */
            r = i_ramble_reader_proxy_at(st, cidx, (uint32_t)peer_slot);
            if (r && r->used) r->no_timestamp = 1;
        }
    }
}


int ramble_transport_peer_timestamped(RambleTransportState *st, uint16_t topic_index, uint32_t peer_id){
    int peer_slot;
    i_RambleReaderProxy *r;
    if (!st || topic_index >= st->cfg.n_topics) return 1;
    peer_slot = i_ramble_peer_slot(st, peer_id);
    if (peer_slot < 0) return 1;                       /* unknown peer: the default framing */
    r = i_ramble_reader_proxy_at(st, topic_index, (uint32_t)peer_slot);
    return (r && r->no_timestamp) ? 0 : 1;
}


uint8_t ramble_transport_peer_attrs(RambleTransportState *st, uint32_t peer_id, uint16_t their_index){
    int peer_slot;
    if (!st) return 0;
    peer_slot = i_ramble_peer_slot(st, peer_id);
    if (peer_slot < 0 || !st->peer_attrs[peer_slot]
        || (uint32_t)their_index >= st->peer_index_len[peer_slot]) return 0;
    return st->peer_attrs[peer_slot][their_index];
}


void ramble_transport_peer_match_counts(RambleTransportState *st, uint32_t peer_id,
                            uint16_t *publish_to, uint16_t *receive_from){
    int s; uint16_t c, w=0, r=0;
    if (publish_to)   *publish_to   = 0;
    if (receive_from) *receive_from = 0;
    if (!st) return;
    s = i_ramble_peer_slot(st, peer_id);
    if (s < 0) return;
    for (c=0;c<st->cfg.n_topics;c++){
        i_RambleLane *l = i_ramble_lane_at(st,c,(uint32_t)s);
        if (!l) continue;
        if (l->w.used) w++;
        if (l->r.used) r++;
    }
    if (publish_to)   *publish_to   = w;
    if (receive_from) *receive_from = r;
}


/* The announce overlay codec. The version byte tags the one current format, and a foreign
   blob is rejected. The layout is in spec/interest.md. */
#define RAMBLE__META_BASE_NOSHM 6u    /* 'D','N',ver, frag_lo, frag_hi, iflags */
#define RAMBLE__META_BASE_SHM   23u   /* 'D','N',ver, frag_lo, frag_hi, shm, host[16], iflags */
#define RAMBLE__META_IFLAG_EXTERNAL 0x01u   /* iflags bit 0: the interest is served by paging */
#ifdef RAMBLE_SHM
#define RAMBLE__META_VER  21u                 /* odd versions carry the SHM base */
#define RAMBLE__META_BASE RAMBLE__META_BASE_SHM
#else
#define RAMBLE__META_VER  20u
#define RAMBLE__META_BASE RAMBLE__META_BASE_NOSHM
#endif

static int i_ramble_meta_ok(RambleBytes meta){
    return meta.data && meta.len >= RAMBLE__META_BASE_NOSHM
        && meta.data[0]=='D' && meta.data[1]=='N'
        && meta.data[2]>=20 && meta.data[2]<=21;
}
/* the base prefix through the iflags byte. The odd version carries shm and host */
static uint16_t i_ramble_meta_base(const uint8_t *meta){
    return (meta[2] & 1u) ? RAMBLE__META_BASE_SHM : RAMBLE__META_BASE_NOSHM;
}
uint16_t ramble_meta_cap(uint16_t n_topics){
    size_t cap = (size_t)RAMBLE__META_BASE + ramble_interest_max(n_topics);
    if (cap > 65000u) cap = 65000u;
    return (uint16_t)cap;
}

uint32_t ramble_transport_interest_size(RambleTransportState *st){
    return (uint32_t)i_ramble_interest_emit(st, NULL, 0);
}

uint16_t ramble_transport_meta_size(RambleTransportState *st){
    size_t len = (size_t)RAMBLE__META_BASE + ramble_transport_interest_size(st);
    if (len > 65000u) len = 65000u;              /* the ramble_meta_cap ceiling */
    return (uint16_t)len;
}

uint16_t ramble_transport_meta_bootstrap_size(void){ return RAMBLE__META_BASE; }

uint16_t ramble_transport_meta_build(RambleTransportState *st, uint8_t *out, uint16_t cap,
                         uint16_t frag_size, int shm_capable, const uint8_t host[16],
                         int interest_external){
    size_t interest_len, len; uint16_t off = RAMBLE__META_BASE;
    out[0]='D'; out[1]='N'; out[2]=RAMBLE__META_VER;
    out[3]=(uint8_t)(frag_size & 0xFF); out[4]=(uint8_t)(frag_size >> 8);
#ifdef RAMBLE_SHM
    out[5]=(uint8_t)(shm_capable?1:0);
    if (host) memcpy(out+6, host, 16); else memset(out+6, 0, 16);
#else
    (void)shm_capable; (void)host;
#endif
    out[off-1] = interest_external ? RAMBLE__META_IFLAG_EXTERNAL : 0u;
    if (interest_external) return off;     /* the bootstrap: locator sized, always one datagram */
    interest_len = ramble_transport_build_interest(st, out + off, cap - off);
    len = (size_t)off + interest_len;
    if (interest_len == 0)    /* did not fit: never silent */
        i_ramble_transport_fire_event(st, RAMBLE_TRANSPORT_META_TRUNCATED_INTEREST, 0, 0, 0, 0);
    return (uint16_t)len;
}

uint16_t ramble_meta_frag(RambleBytes meta){
    if (!i_ramble_meta_ok(meta)) return 0;
    return (uint16_t)(meta.data[3] | ((uint16_t)meta.data[4] << 8));
}

int ramble_meta_interest_external(RambleBytes meta){
    uint16_t base;
    if (!i_ramble_meta_ok(meta)) return 0;
    base = i_ramble_meta_base(meta.data);
    if (meta.len < base) return 0;
    return (meta.data[base-1] & RAMBLE__META_IFLAG_EXTERNAL) ? 1 : 0;
}

RambleBytes ramble_meta_interest(RambleBytes meta){
    uint16_t off;
    if (!i_ramble_meta_ok(meta)) return ramble_bytes(NULL, 0);
    off = i_ramble_meta_base(meta.data);     /* the interest follows the base */
    if (meta.len < off) return ramble_bytes(NULL, 0);
    if (meta.data[off-1] & RAMBLE__META_IFLAG_EXTERNAL)
        return ramble_bytes(NULL, 0);        /* a bootstrap never reads as an empty interest list */
    return ramble_bytes(meta.data + off, meta.len - off);
}

int ramble_interest_next(RambleBytes interest, RambleInterestIter *it, RambleTopicEntry *out){
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
        flags = interest.data[off + 4]; role = (uint8_t)(flags & RAMBLE__INT_ROLE_MASK);
        if (flags & RAMBLE__INT_HOLE_RUN){  /* a run of undefined slots: skip them all */
            uint32_t run = i_ramble_le_r32(interest.data + off);
            if (run == 0 || run > it->left){ it->left = 0; return 0; }    /* malformed: stop */
            it->left = (uint16_t)(it->left - run); it->index = (uint16_t)(it->index + run);
            it->off = off + 5u; it->phase = 0;
            continue;
        }
        if (role == RAMBLE_INACTIVE){       /* declared but off: not advertised */
            it->left--; it->index++; it->off = off + 5u; it->phase = 0;
            continue;
        }
        out->index    = it->index;
        out->role     = role;
        out->reliable = (uint8_t)((flags & RAMBLE__INT_RELIABLE) ? 1 : 0);
        out->kind     = (uint8_t)((flags & RAMBLE__INT_KIND_MASK) >> RAMBLE__INT_KIND_SHIFT);
        out->hash     = i_ramble_le_r32(interest.data + off);
        if (it->phase == 0 && ramble_role_pubs(role)){
            out->is_pub = 1;
            if (role==RAMBLE_PUBSUB){ it->phase = 1; return 1; }   /* the sub direction next call */
            it->left--; it->index++; it->off = off + 5u;
            return 1;
        }
        out->is_pub = 0;                  /* SUB_ONLY, or the second yield of a PUBSUB entry */
        it->phase = 0; it->left--; it->index++; it->off = off + 5u;
        return 1;
    }
    return 0;
}

int ramble_meta_interest_next(RambleBytes meta, RambleInterestIter *it, RambleTopicEntry *out){
    return ramble_interest_next(ramble_meta_interest(meta), it, out);
}

#ifdef RAMBLE_SHM
int ramble_meta_shm(RambleBytes meta, uint8_t host[16]){
    if (!i_ramble_meta_ok(meta) || !(meta.data[2] & 1u)   /* the odd version carries the SHM base */
        || meta.len < RAMBLE__META_BASE_SHM || !meta.data[5]) return 0;
    memcpy(host, meta.data+6, 16);
    return 1;
}
#endif


/* The uDTL codec. A malformed request or response is dropped whole. The responder is a
   pure read of topic and schema state. The layout is in spec/interest.md. */
#define RAMBLE__DETAIL_HDR 14u   /* magic(4) kind(1) ver(1) domain(2) meta_version(4) n(2) */
#define RAMBLE__DETAIL_VER 2u

static int i_ramble_detail_hdr_ok(RambleBytes d){
    return d.data && d.len >= RAMBLE__DETAIL_HDR
        && d.data[0]=='u' && d.data[1]=='D' && d.data[2]=='T' && d.data[3]=='L'
        && d.data[5]==RAMBLE__DETAIL_VER;
}

int ramble_detail_kind(RambleBytes dgram){
    if (!i_ramble_detail_hdr_ok(dgram)) return 0;
    return (dgram.data[4]>=RAMBLE_DETAIL_REQ && dgram.data[4]<=RAMBLE_INTEREST_RESP)
         ? dgram.data[4] : 0;
}
uint16_t ramble_detail_domain(RambleBytes dgram){
    return i_ramble_detail_hdr_ok(dgram) ? i_ramble_le_r16(dgram.data+6) : 0;
}
uint32_t ramble_detail_meta_version(RambleBytes dgram){
    return i_ramble_detail_hdr_ok(dgram) ? i_ramble_le_r32(dgram.data+8) : 0;
}

static void i_ramble_detail_hdr_write(uint8_t *o, uint8_t kind, uint16_t domain,
                                    uint32_t meta_version, uint16_t n){
    o[0]='u'; o[1]='D'; o[2]='T'; o[3]='L';
    o[4]=kind; o[5]=(uint8_t)RAMBLE__DETAIL_VER;
    i_ramble_le_w16(o+6, domain);
    i_ramble_le_w32(o+8, meta_version);
    i_ramble_le_w16(o+12, n);
}

size_t ramble_detail_req_build(uint16_t domain, uint32_t peer_meta_version,
                             const RambleDetailWant *wants, uint16_t n_wants,
                             void *out, size_t cap){
    uint8_t *o=(uint8_t*)out; uint16_t k;
    size_t need = (size_t)RAMBLE__DETAIL_HDR + (size_t)n_wants*10u;
    if (!o || (n_wants && !wants) || cap < need) return 0;
    i_ramble_detail_hdr_write(o, RAMBLE_DETAIL_REQ, domain, peer_meta_version, n_wants);
    for (k=0;k<n_wants;k++){
        uint8_t *e = o + RAMBLE__DETAIL_HDR + (size_t)k*10u;
        i_ramble_le_w16(e, wants[k].index);
        i_ramble_le_w64(e+2, wants[k].schema_hash);
    }
    return need;
}


/* The interest paging codec: pure header build and parse. The responder's slicing and
   the requester's cursor live in the node core. */
size_t ramble_interest_req_build(uint16_t domain, uint32_t peer_meta_version,
                               uint32_t offset, void *out, size_t cap){
    uint8_t *o=(uint8_t*)out;
    if (!o || cap < (size_t)RAMBLE__DETAIL_HDR + 4u) return 0;
    i_ramble_detail_hdr_write(o, RAMBLE_INTEREST_REQ, domain, peer_meta_version, 0);
    i_ramble_le_w32(o + RAMBLE__DETAIL_HDR, offset);
    return (size_t)RAMBLE__DETAIL_HDR + 4u;
}

int ramble_interest_req_offset(RambleBytes dgram, uint32_t *offset){
    if (ramble_detail_kind(dgram) != RAMBLE_INTEREST_REQ) return 0;
    if (dgram.len < (size_t)RAMBLE__DETAIL_HDR + 4u) return 0;
    if (offset) *offset = i_ramble_le_r32(dgram.data + RAMBLE__DETAIL_HDR);
    return 1;
}

size_t ramble_interest_resp_head(uint16_t domain, uint32_t meta_version, uint32_t total_len,
                               uint32_t offset, uint16_t chunk_len, void *out, size_t cap){
    uint8_t *o=(uint8_t*)out;
    if (!o || cap < (size_t)RAMBLE_INTEREST_RESP_HEAD) return 0;
    i_ramble_detail_hdr_write(o, RAMBLE_INTEREST_RESP, domain, meta_version, 0);
    i_ramble_le_w32(o + RAMBLE__DETAIL_HDR,      total_len);
    i_ramble_le_w32(o + RAMBLE__DETAIL_HDR + 4u, offset);
    i_ramble_le_w16(o + RAMBLE__DETAIL_HDR + 8u, chunk_len);
    return (size_t)RAMBLE_INTEREST_RESP_HEAD;
}

int ramble_interest_resp_parse(RambleBytes dgram, uint32_t *total_len, uint32_t *offset,
                             RambleBytes *chunk){
    uint32_t total, off; uint16_t clen;
    if (ramble_detail_kind(dgram) != RAMBLE_INTEREST_RESP) return 0;
    if (dgram.len < (size_t)RAMBLE_INTEREST_RESP_HEAD) return 0;
    total = i_ramble_le_r32(dgram.data + RAMBLE__DETAIL_HDR);
    off   = i_ramble_le_r32(dgram.data + RAMBLE__DETAIL_HDR + 4u);
    clen  = i_ramble_le_r16(dgram.data + RAMBLE__DETAIL_HDR + 8u);
    if ((size_t)RAMBLE_INTEREST_RESP_HEAD + clen > dgram.len) return 0;   /* truncated: reject */
    if (off > total || (uint32_t)clen > total - off) return 0;   /* a range outside the blob */
    if (total_len) *total_len = total;
    if (offset)    *offset    = off;
    if (chunk)     *chunk     = ramble_bytes(dgram.data + RAMBLE_INTEREST_RESP_HEAD, clen);
    return 1;
}

/* One walk serves size and build (out NULL measures), so the two agree byte for byte. A
   truncated build stops at an entry boundary and the requester re asks for the rest. */
static size_t i_ramble_detail_answer(RambleTransportState *st, const RambleMetaSchema *schemas,
                                   uint32_t meta_version, RambleBytes req,
                                   uint8_t *out, size_t cap){
    const uint8_t *r; uint16_t n_req, k, n_out=0;
    size_t len = RAMBLE__DETAIL_HDR;
    if (!st || ramble_detail_kind(req) != RAMBLE_DETAIL_REQ) return 0;
    n_req = i_ramble_le_r16(req.data+12);
    if (req.len < (size_t)RAMBLE__DETAIL_HDR + (size_t)n_req*10u) return 0;   /* truncated: reject */
    if (out){
        if (cap < RAMBLE__DETAIL_HDR) return 0;
        i_ramble_detail_hdr_write(out, RAMBLE_DETAIL_RESP, ramble_detail_domain(req), meta_version, 0);
    }
    r = req.data + RAMBLE__DETAIL_HDR;
    for (k=0;k<n_req;k++,r+=10){
        uint16_t index    = i_ramble_le_r16(r);
        uint64_t req_hash = i_ramble_le_r64(r+2);
        const i_RambleTopic *topic;
        uint64_t hash; RambleBytes wire; size_t need;
        if (index >= st->cfg.n_topics) continue;             /* unknown: not advertised */
        topic = &st->topics[index];
        if (topic->role == RAMBLE_INACTIVE || topic->name_len == 0) continue;
        hash = schemas ? schemas[index].hash : 0;
        wire = ramble_bytes(NULL, 0);
        if (hash && hash != req_hash && schemas[index].wire.len <= 0xFFFFu)
            wire = schemas[index].wire;    /* differs: inline for the subset check */
        need = 2u + 1u + 1u + topic->name_len + 8u + 2u + wire.len;
        /* one datagram per response, but always at least one entry, so a lone oversized
           entry rides its own page instead of wedging paging with an empty reply */
        if (n_out > 0 && len + need > cap) break;
        if (out){
            uint8_t *e = out + len;
            i_ramble_le_w16(e, index);
            e[2] = (uint8_t)(topic->attrs
                 | (topic->qos.no_timestamp ? RAMBLE_ATTR_NO_TIMESTAMP : 0u));
            e[3] = topic->name_len;
            memcpy(e+4, topic->name, topic->name_len);
            i_ramble_le_w64(e+4+topic->name_len, hash);
            i_ramble_le_w16(e+4+topic->name_len+8, (uint16_t)wire.len);
            if (wire.len) memcpy(e+4+topic->name_len+10, wire.data, wire.len);
        }
        len += need;
        n_out++;
    }
    if (out) i_ramble_le_w16(out+12, n_out);
    return len;
}

size_t ramble_transport_detail_resp_size(RambleTransportState *st, const RambleMetaSchema *schemas,
                                       RambleBytes req){
    /* one page, so the response never IP fragments and the requester pages the rest */
    return i_ramble_detail_answer(st, schemas, 0, req, NULL, RAMBLE_DGRAM_MAX);
}

size_t ramble_transport_detail_respond(RambleTransportState *st, const RambleMetaSchema *schemas,
                                     uint32_t meta_version, RambleBytes req,
                                     void *out, size_t cap){
    return i_ramble_detail_answer(st, schemas, meta_version, req, (uint8_t*)out, cap);
}

int ramble_detail_next(RambleBytes resp, RambleDetailIter *it, RambleDetail *out){
    uint32_t off; uint8_t nlen; uint16_t wlen;
    if (!it || !out) return 0;
    if (!it->started){
        it->started = 1; it->left = 0; it->off = RAMBLE__DETAIL_HDR;
        if (ramble_detail_kind(resp) != RAMBLE_DETAIL_RESP) return 0;
        it->left = i_ramble_le_r16(resp.data+12);
    }
    if (!it->left) return 0;
    off = it->off;
    if ((size_t)off + 4u > resp.len){ it->left = 0; return 0; }        /* truncated: stop */
    nlen = resp.data[off+3];
    if ((size_t)off + 4u + nlen + 10u > resp.len){ it->left = 0; return 0; }
    wlen = i_ramble_le_r16(resp.data + off + 4u + nlen + 8u);
    if ((size_t)off + 4u + nlen + 10u + wlen > resp.len){ it->left = 0; return 0; }
    out->index       = i_ramble_le_r16(resp.data + off);
    out->attrs       = resp.data[off+2];
    out->name        = ramble_string((const char*)(resp.data + off + 4u), nlen);
    out->schema_hash = i_ramble_le_r64(resp.data + off + 4u + nlen);
    out->schema_wire = wlen ? ramble_bytes(resp.data + off + 4u + nlen + 10u, wlen)
                            : ramble_bytes(NULL, 0);
    it->off = off + 4u + nlen + 10u + wlen;
    it->left--;
    return 1;
}

/* Iterator over the live entries of an interest stream, shared by the candidate scans so
 * they cannot drift. The stream must have passed i_ramble_interest_walk_len first. */
typedef struct {
    const uint8_t *e;        /* next entry */
    uint32_t a;              /* its position */
    uint32_t n;              /* slots in the stream */
    uint32_t pos;            /* yielded: the entry's position */
    uint32_t hash;           /* yielded: its 32 bit nomination */
    uint8_t  flags;          /* yielded: the raw flags */
    uint8_t  their_pub, their_sub;   /* yielded: the advertised role by direction */
} i_RambleInterestScan;

static void i_ramble_interest_scan_init(i_RambleInterestScan *s, const uint8_t *d, uint16_t n){
    s->e = d + 2u; s->a = 0; s->n = n;
    s->pos = 0; s->hash = 0; s->flags = 0; s->their_pub = s->their_sub = 0;
}

static int i_ramble_interest_scan_next(i_RambleInterestScan *s){
    while (s->a < s->n){
        const uint8_t *e = s->e;
        uint32_t a = s->a, step = 1;
        uint8_t flags = e[4], role = (uint8_t)(flags & RAMBLE__INT_ROLE_MASK);
        if (flags & RAMBLE__INT_HOLE_RUN) step = i_ramble_le_r32(e);
        s->e = e + 5u; s->a = a + step;
        if (flags & RAMBLE__INT_HOLE_RUN) continue;   /* a run of undefined slots */
        if (role == RAMBLE_INACTIVE) continue;        /* declared but off */
        s->pos = a; s->hash = i_ramble_le_r32(e); s->flags = flags;
        s->their_pub = (uint8_t)ramble_role_pubs(role);
        s->their_sub = (uint8_t)ramble_role_subs(role);
        return 1;
    }
    return 0;
}

/* The pending candidates of a peer: a hash and role overlap with no cached verdict. */
uint16_t ramble_transport_detail_wants(RambleTransportState *st, const RambleMetaSchema *schemas,
                                     uint32_t peer_id, RambleBytes interest,
                                     RambleDetailWant *out, uint16_t max_wants){
    const uint8_t *d=interest.data;
    i_RambleInterestScan scan;
    uint16_t n, cnt=0; int slot;
    uint8_t *astate; uint32_t alen;
    if (!st || !d || interest.len < 2) return 0;
    slot = i_ramble_peer_slot(st, peer_id);
    if (slot < 0) return 0;
    n = i_ramble_le_r16(d);
    if (!i_ramble_interest_walk_len(d, interest.len, n)) return 0;
    astate = st->peer_astate[slot]; alen = st->peer_index_len[slot];
    i_ramble_interest_scan_init(&scan, d, n);
    while (i_ramble_interest_scan_next(&scan)){
        int cidx = -1, nc;
        i_RambleTopic *topic;
        int ours_pub, ours_sub;
        if (astate && scan.pos < alen && (astate[scan.pos] & RAMBLE__AST_DETAILED)) continue;
        nc = i_ramble_hash32_candidates(st, scan.hash, &cidx);
        if (!nc) continue;                                 /* no local topic */
        topic = &st->topics[cidx];
        ours_pub = ramble_role_pubs(topic->role);
        ours_sub = ramble_role_subs(topic->role);
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
uint16_t ramble_transport_topic_unresolved(RambleTransportState *st, uint16_t topic_index,
                                         uint32_t peer_id, RambleBytes interest){
    const uint8_t *d=interest.data;
    i_RambleTopic *topic;
    i_RambleInterestScan scan;
    uint16_t n, cnt=0; int slot;
    uint8_t *astate; uint32_t alen;
    int ours_pub, ours_sub;
    topic = i_ramble_topic_at(st, topic_index, NULL);
    if (!topic || !i_ramble_topic_announced(topic) || !d || interest.len < 2) return 0;
    slot = i_ramble_peer_slot(st, peer_id);
    if (slot < 0) return 0;
    n = i_ramble_le_r16(d);
    if (!i_ramble_interest_walk_len(d, interest.len, n)) return 0;
    ours_pub = ramble_role_pubs(topic->role);
    ours_sub = ramble_role_subs(topic->role);
    if (!ours_pub && !ours_sub) return 0;
    astate = st->peer_astate[slot]; alen = st->peer_index_len[slot];
    i_ramble_interest_scan_init(&scan, d, n);
    while (i_ramble_interest_scan_next(&scan)){
        if (scan.hash != (uint32_t)topic->identity) continue;   /* not this topic */
        if (!astate || scan.pos >= alen) continue;          /* unresolvable: never counted */
        if (astate[scan.pos] & RAMBLE__AST_DETAILED){
            /* decided, but a verified subscriber whose writer lane is under the rebind hold
               is still resolving, so the match wait must cover it */
            if (scan.their_sub && ours_pub && (astate[scan.pos] & RAMBLE__AST_NAME_OK)
                && topic->rebind_version
                && st->peer_seen_version[slot] < topic->rebind_version) cnt++;
            continue;
        }
        if ((scan.their_pub && ours_sub) || (scan.their_sub && ours_pub)) cnt++;
    }
    return cnt;
}

void ramble_transport_peer_unresolved_fill(RambleTransportState *st, uint32_t peer_id,
                                         RambleBytes interest, uint16_t *counts, uint16_t n){
    const uint8_t *d=interest.data;
    i_RambleInterestScan scan;
    uint16_t nent, c; int slot;
    uint8_t *astate; uint16_t *amap; uint32_t alen;
    if (!st || !counts || !n || !d || interest.len < 2) return;
    slot = i_ramble_peer_slot(st, peer_id);
    if (slot < 0) return;
    nent = i_ramble_le_r16(d);
    if (!i_ramble_interest_walk_len(d, interest.len, nent)) return;
    astate = st->peer_astate[slot]; amap = st->peer_index[slot]; alen = st->peer_index_len[slot];
    if (!astate || !amap) return;                        /* unresolvable entries: never counted */
    if (n > st->cfg.n_topics) n = (uint16_t)st->cfg.n_topics;
    i_ramble_interest_scan_init(&scan, d, nent);
    while (i_ramble_interest_scan_next(&scan)){
        if (scan.pos >= alen) continue;
        if (astate[scan.pos] & RAMBLE__AST_DETAILED){
            /* decided: the verdict names the topic, and only the rebind hold still resolves */
            uint16_t cidx = amap[scan.pos];
            const i_RambleTopic *topic;
            if (!(astate[scan.pos] & RAMBLE__AST_NAME_OK) || cidx >= n) continue;
            topic = &st->topics[cidx];
            if (scan.their_sub && ramble_role_pubs(topic->role) && topic->rebind_version
                && st->peer_seen_version[slot] < topic->rebind_version && counts[cidx] != 0xFFFFu)
                counts[cidx]++;
            continue;
        }
        for (c=0;c<n;c++){   /* pending: every topic the hash nominates */
            const i_RambleTopic *topic = &st->topics[c];
            int ours_pub, ours_sub;
            if (!i_ramble_topic_announced(topic) || (uint32_t)topic->identity != scan.hash) continue;
            ours_pub = ramble_role_pubs(topic->role); ours_sub = ramble_role_subs(topic->role);
            if (((scan.their_pub && ours_sub) || (scan.their_sub && ours_pub)) && counts[c] != 0xFFFFu)
                counts[c]++;
        }
    }
}

/* Verifies each entry and caches the verdict. Idempotent, decided indices are skipped, so
 * duplicate and crossing responses are harmless. */
uint16_t ramble_transport_apply_peer_details(RambleTransportState *st, uint32_t peer_id, RambleBytes resp){
    RambleDetailIter it; RambleDetail dd;
    int slot; uint16_t fresh = 0;
    if (!st) return 0;
    slot = i_ramble_peer_slot(st, peer_id);
    if (slot < 0) return 0;
    memset(&it, 0, sizeof it);
    while (ramble_detail_next(resp, &it, &dd)){
        uint16_t *amap = st->peer_index[slot]; uint8_t *astate = st->peer_astate[slot];
        uint64_t id64; i_RambleTopic *topic; int cidx = -1;
        if (!amap || !astate || (uint32_t)dd.index >= st->peer_index_len[slot])
            continue;   /* unseen: ignored, a response racing the announce re resolves */
        if (astate[dd.index] & RAMBLE__AST_DETAILED) continue;
        if (st->peer_attrs[slot]) st->peer_attrs[slot][dd.index] = dd.attrs;
        if (dd.name.len == 0){ astate[dd.index] = RAMBLE__AST_DETAILED; fresh++; continue; }
        id64 = i_ramble_identity_hash((const uint8_t*)dd.name.data, dd.name.len);
        topic = i_ramble_topic_by_identity(st, id64, &cidx);
        if (!topic){                              /* the 32 bit nomination was a false positive */
            astate[dd.index] = RAMBLE__AST_DETAILED;
            fresh++; continue;
        }
        if (topic->name_len != dd.name.len || memcmp(topic->name, dd.name.data, dd.name.len) != 0){
            astate[dd.index] = RAMBLE__AST_DETAILED;   /* the same id, a different name: refused */
            i_ramble_transport_fire_event(st, RAMBLE_TRANSPORT_NAME_COLLISION, (uint16_t)cidx,
                        peer_id, id64, 0);
            fresh++; continue;
        }
        amap[dd.index] = (uint16_t)cidx;
        astate[dd.index] = (uint8_t)(RAMBLE__AST_DETAILED | RAMBLE__AST_NAME_OK
            | ((!st->cfg.schema_check || st->cfg.schema_check(st->cfg.user, peer_id, (uint16_t)cidx,
                    1, dd.schema_hash, dd.schema_wire)) ? RAMBLE__AST_READ_OK : 0u)
            | ((!st->cfg.schema_check || st->cfg.schema_check(st->cfg.user, peer_id, (uint16_t)cidx,
                    0, dd.schema_hash, dd.schema_wire)) ? RAMBLE__AST_WRITE_OK : 0u));
        fresh++;
    }
    return fresh;
}


int ramble_transport_set_role(RambleTransportState *st, uint16_t topic_index, uint8_t role){
    i_RambleTopic *topic; uint16_t p;
    if (role > RAMBLE_INACTIVE) return -1;
    topic = i_ramble_topic_at(st, topic_index, NULL);
    if (!topic || topic->retired) return -1;   /* a retired slot only returns through reuse */
    if (topic->role == role) return 0;
    topic->role = role;
    for (p=0;p<st->cfg.max_peers;p++)
        if (st->peer_used[p]) i_ramble_topic_rematch(st,(uint16_t)topic_index,p);
    /* the caller re advertises */
    return 0;
}


int ramble_transport_topic_define(RambleTransportState *st, uint16_t topic_index, const RambleTopicDef *def){
    i_RambleTopic *topic; RambleQos q; uint16_t depth, p; size_t lane;
    if (!st || !def) return -1;
    if (topic_index >= st->cfg.n_topics) return -1;            /* out of the reserved range */
    if (!def->name || !def->name[0]) return -1;             /* the name is the identity */
    lane = i_ramble_name_len(def->name);
    if (def->name[lane]) return -1;                          /* longer than RAMBLE_TOPIC_NAME_MAX */
    if (def->directed && def->qos.catch_up) return -1;       /* directed history never replays */
    topic = &st->topics[topic_index];
    if (topic->identity != 0 || topic->history) return -1;        /* already defined */
    {   /* the same name under a different kind on one node is refused, since the maps bind
           by identity. Retired slots are exempt. See spec/interest.md */
        uint64_t id = ramble_topic_identity(def); uint16_t c;
        for (c=0;c<st->cfg.n_topics;c++)
            if (i_ramble_topic_announced(&st->topics[c]) && st->topics[c].identity == id
                && st->topics[c].kind != def->kind) return -1;
    }
    q = def->qos; i_ramble_qos_defaults(&q);
    depth = q.keep_last;
    topic->history = (i_RambleWriterSample*)st->cfg.allocator(st->cfg.user, NULL,
                                                         (size_t)depth*sizeof(i_RambleWriterSample));
    if (!topic->history) return -4;                            /* OOM */
    memset(topic->history, 0, (size_t)depth*sizeof(i_RambleWriterSample));
    topic->history_owned = 1;
    topic->qos = q;
    topic->role = def->role;
    topic->kind = def->kind; topic->prefix_bytes = def->prefix_bytes; topic->directed = def->directed;
    topic->attrs = def->attrs;
    topic->identity = ramble_topic_identity(def);
    memcpy((char*)topic->name, def->name, lane); ((char*)topic->name)[lane] = '\0';
    topic->name_len = (uint8_t)lane;
    topic->history_head = 0; topic->next_seqno = 0; topic->have_first = 0;
    /* a dissolved verdict is only as durable as the topic set it was judged against, and
       that set just grew: send them back to pending. Attrs stay. See spec/interest.md */
    for (p=0;p<st->cfg.max_peers;p++){
        uint8_t *as = st->peer_astate[p]; uint32_t a, alen = st->peer_index_len[p];
        if (!st->peer_used[p] || !as) continue;
        for (a=0;a<alen;a++)
            if ((as[a] & RAMBLE__AST_DETAILED) && !(as[a] & RAMBLE__AST_NAME_OK))
                as[a] = 0;
    }
    for (p=0;p<st->cfg.max_peers;p++)        /* match the new topic to known peers */
        if (st->peer_used[p]) i_ramble_topic_rematch(st, topic_index, p);
    return 0;
}


/* Parks a slot: every lane releases, the ring is freed and the slot leaves the announce.
 * Identity, name, kind, qos, gen and next_seqno stay for reuse to compare. */
int ramble_transport_topic_retire(RambleTransportState *st, uint16_t topic_index){
    i_RambleTopic *topic; uint16_t p, d, depth;
    if (!st) return -1;
    topic = i_ramble_topic_at(st, topic_index, NULL);
    if (!topic || topic->name_len == 0 || topic->retired) return -1;
    topic->role = RAMBLE_INACTIVE; topic->retired = 1;
    for (p=0;p<st->cfg.max_peers;p++)          /* both sides unmatch, every lane releases */
        if (st->peer_used[p]) i_ramble_topic_rematch(st, topic_index, p);
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
            memset(topic->history, 0, (size_t)depth*sizeof(i_RambleWriterSample));
        }
    }
    topic->history_head = 0; topic->have_first = 0; topic->first_seqno = 0;
    memset(&topic->repair_stats, 0, sizeof topic->repair_stats);
    /* the caller re advertises: the slot now rides the announce as a hole */
    return 0;
}


int ramble_transport_topic_reuse_find(RambleTransportState *st, const char *name, uint8_t kind,
                                    uint16_t *index_out){
    uint64_t id; uint16_t c; int any = -1;
    if (!st || !name || !name[0]) return 0;
    id = ramble_topic_id(name);
    for (c=0;c<st->cfg.n_topics;c++){
        const i_RambleTopic *t = &st->topics[c];
        if (!t->retired) continue;
        if (t->identity == id && t->kind == kind){ if (index_out) *index_out = c; return 2; }
        if (any < 0) any = (int)c;
    }
    if (any >= 0){ if (index_out) *index_out = (uint16_t)any; return 1; }
    return 0;
}


/* Rebinds a retired slot. The seqno line always continues: an identical rebind keeps a
 * peer's reader position valid, and a changed one re forms every lane anyway. */
int ramble_transport_topic_reuse(RambleTransportState *st, uint16_t topic_index,
                               const RambleTopicDef *def, int binding_changed,
                               uint32_t rebind_version){
    i_RambleTopic *topic; RambleQos q; uint16_t depth, p; size_t nlen; uint64_t id;
    if (!st || !def) return -1;
    topic = i_ramble_topic_at(st, topic_index, NULL);
    if (!topic || !topic->retired) return -1;
    if (!def->name || !def->name[0]) return -1;
    nlen = i_ramble_name_len(def->name);
    if (def->name[nlen]) return -1;                          /* longer than RAMBLE_TOPIC_NAME_MAX */
    if (def->directed && def->qos.catch_up) return -1;       /* directed history never replays */
    id = ramble_topic_identity(def);
    {   /* the same name under a different kind on one node is refused, as at define */
        uint16_t c;
        for (c=0;c<st->cfg.n_topics;c++)
            if (c != topic_index && i_ramble_topic_announced(&st->topics[c])
                && st->topics[c].identity == id && st->topics[c].kind != def->kind) return -1;
    }
    if (!binding_changed && (topic->identity != id || topic->kind != def->kind))
        return -1;                       /* asserted identical, but the stored binding differs */
    q = def->qos; i_ramble_qos_defaults(&q);
    depth = q.keep_last;
    if (topic->history && !topic->history_owned && depth <= topic->qos.keep_last){
        /* an arena ring that still fits is reused in place */
        memset(topic->history, 0, (size_t)depth*sizeof(i_RambleWriterSample));
    } else {
        i_RambleWriterSample *h = (i_RambleWriterSample*)st->cfg.allocator(st->cfg.user, NULL,
                                                     (size_t)depth*sizeof(i_RambleWriterSample));
        if (!h) return -4;                                   /* OOM: the slot stays retired */
        memset(h, 0, (size_t)depth*sizeof(i_RambleWriterSample));
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
            i_ramble_bit_clr(&st->peer_pub_bitmap[(size_t)p*st->bitmap_len], topic_index);
            i_ramble_bit_clr(&st->peer_sub_bitmap[(size_t)p*st->bitmap_len], topic_index);
            i_ramble_bit_clr(&st->peer_sub_reliable[(size_t)p*st->bitmap_len], topic_index);
            /* dissolved verdicts re pend, as at define. Attrs stay. */
            if (st->peer_astate[p]){
                uint8_t *as = st->peer_astate[p];
                for (a=0;a<st->peer_index_len[p];a++)
                    if ((as[a] & RAMBLE__AST_DETAILED) && !(as[a] & RAMBLE__AST_NAME_OK))
                        as[a] = 0;
            }
        }
    }
    for (p=0;p<st->cfg.max_peers;p++)
        if (st->peer_used[p]) i_ramble_topic_rematch(st, topic_index, p);
    return 0;
}


/* Releases the writer lanes the advance takes out of the rebind hold. 1 when a lane
 * actually formed, so the caller re fires its interest event. */
int ramble_transport_peer_seen_version(RambleTransportState *st, uint32_t peer_id, uint32_t version){
    int s; uint16_t c; uint32_t old; int changed = 0;
    if (!st) return 0;
    s = i_ramble_peer_slot(st, peer_id);
    if (s < 0) return 0;
    old = st->peer_seen_version[s];
    if (version <= old) return 0;
    st->peer_seen_version[s] = version;
    for (c=0;c<st->cfg.n_topics;c++){
        i_RambleTopic *t = &st->topics[c];
        i_RambleLane *l; int was;
        if (!t->rebind_version || t->rebind_version <= old || t->rebind_version > version) continue;
        if (!i_ramble_topic_announced(t)) continue;
        l = i_ramble_lane_at(st, c, (uint32_t)s); was = l && l->w.used;
        i_ramble_topic_rematch(st, c, (uint16_t)s);
        l = i_ramble_lane_at(st, c, (uint32_t)s);
        if ((l && l->w.used) != was) changed = 1;
    }
    return changed;
}


uint64_t ramble_transport_topic_seqno(RambleTransportState *st, uint16_t topic_index){
    i_RambleTopic *topic = st ? i_ramble_topic_at(st, topic_index, NULL) : NULL;
    return topic ? topic->next_seqno : 0;
}


RambleString ramble_transport_topic_name(RambleTransportState *st, uint16_t topic_index){
    i_RambleTopic *topic = i_ramble_topic_at(st, topic_index, NULL);
    if (!topic || topic->name_len == 0) return ramble_string(NULL, 0);   /* undefined */
    return ramble_string(topic->name, topic->name_len);
}


const RambleQos *ramble_transport_topic_qos(RambleTransportState *st, uint16_t topic_index){
    i_RambleTopic *topic = i_ramble_topic_at(st, topic_index, NULL);
    return topic ? &topic->qos : NULL;
}

uint8_t ramble_transport_topic_attrs(RambleTransportState *st, uint16_t topic_index){
    i_RambleTopic *topic = i_ramble_topic_at(st, topic_index, NULL);
    return topic ? topic->attrs : 0;
}


void ramble_transport_repair_stats(RambleTransportState *st, uint16_t topic_index, RambleRepairStats *out){
    i_RambleTopic *topic = i_ramble_topic_at(st, topic_index, NULL);
    if (!out) return;
    if (topic) *out = topic->repair_stats;
    else memset(out, 0, sizeof *out);
}


void ramble_transport_on_datagram(RambleTransportState *st, uint32_t from, RambleBytes datagram, uint64_t now){
    const uint8_t *p=datagram.data; size_t rem=datagram.len;
    int peer_slot=i_ramble_peer_slot(st,from);
    if (peer_slot<0) return;
    /* concatenated submessages, each length from its own header */
    while (rem>=3){
        uint8_t b0=p[0], type=(uint8_t)(b0 & RAMBLE_MSG_MASK); uint16_t index; size_t sub; int topic_index;
        switch(type){
            case RAMBLE_DATA:
#ifdef RAMBLE_SHM
                            if (b0 & RAMBLE_F_SHM){ if (rem<RAMBLE_SHM_DATA_BYTES) return; sub=RAMBLE_SHM_DATA_BYTES; }
                            else
#endif
                            if (b0 & RAMBLE_F_SINGLE){ if (rem<RAMBLE_HEADER_DATA_SINGLE) return; sub=RAMBLE_HEADER_DATA_SINGLE+(size_t)i_ramble_le_r16(p+RAMBLE_OFFSET_PAYLOAD_LEN_SINGLE); }
                            else { if (rem<RAMBLE_HEADER_DATA_MULTI) return; sub=RAMBLE_HEADER_DATA_MULTI+(size_t)i_ramble_le_r16(p+RAMBLE_OFFSET_PAYLOAD_LEN); } break;
            case RAMBLE_HB:   if (rem<RAMBLE_HEADER_HB) return; sub=RAMBLE_HEADER_HB; break;
            case RAMBLE_NACK: if (rem<RAMBLE_HEADER_NACK) return; sub=RAMBLE_HEADER_NACK; break;
            default: return;             /* unknown type: cannot resync, drop the rest */
        }
        if (sub>rem) return;             /* truncated */
        index = i_ramble_le_r16(p+RAMBLE_OFFSET_INDEX);
        if ((uint32_t)index < st->peer_index_len[peer_slot]){
            uint16_t m=st->peer_index[peer_slot][index]; topic_index=(m==0xFFFFu)?-1:(int)m;
        } else topic_index=-1;
        if (topic_index>=0){
            switch(type){
                case RAMBLE_DATA:
#ifdef RAMBLE_SHM
                                if (p[0] & RAMBLE_F_SHM){ i_ramble_reader_shm(st,topic_index,peer_slot,p,now); break; }
#endif
                                i_ramble_reader_data(st,topic_index,peer_slot,p,now); break;
                case RAMBLE_HB:   i_ramble_reader_hb  (st,topic_index,peer_slot,p,now); break;
                case RAMBLE_NACK: i_ramble_writer_nack(st,topic_index,peer_slot,p,now); break;
            }
        }
        p+=sub; rem-=sub;
    }
}
#pragma endregion

#ifndef RAMBLE_TRANSPORT_SANS_IO
#pragma region serialize/schema.c
#include <string.h>

/* The wire format and the layout rules are in spec/schema.md. */

/* One field of the flat table. Offsets are message absolute, except members of a
 * variable struct array, which are relative to their element. */
typedef struct {
    RambleString name;      /* a view into the wire bytes */
    RambleString type_name; /* the field type's NAMED tag, or {NULL,0} */
    RambleString elem_name; /* an array element type's NAMED tag, or {NULL,0} */
    uint32_t     offset;    /* element 0 under an array, 0 for variable kinds */
    uint32_t     size;      /* 0 for variable kinds */
    uint32_t     elem_size; /* ARR and VARR: bytes of one element, else 0 */
    uint32_t     type_off;  /* wire offset of the type encoding, for the subset compare */
    uint32_t     type_len;
    uint16_t     count;     /* ARR element count, or ENUM variant count, else 0 */
    uint16_t     depth;     /* 0 = top level */
    uint16_t     parent;    /* flat index of the enclosing struct or array, 0xFFFF = root */
    uint16_t     arr_parent;/* flat index of the enclosing struct array, 0xFFFF = none */
    uint16_t     str_cap;   /* STR fields and STR elements, else 0 */
    uint16_t     var_ord;   /* variable kinds: the ordinal of this field's tail frame */
    uint8_t      kind;
    uint8_t      elem;      /* ARR and VARR element kind, else 0 */
} i_Field;

struct RambleSchema {
    RambleBytes   wire;       /* the canonical bytes, a view into the block */
    uint64_t      hash;
    uint32_t      size;       /* the fixed section size */
    RambleString  name;       /* the root name, a view into the wire, "" if anonymous */
    uint16_t      nfields;
    uint16_t      n_var;      /* variable fields */
    uint8_t       value_root; /* 1 = a bare or alias root, not a struct */
    i_Field       fields[1];   /* nfields entries in the block */
};

#define I_RAMBLE_NO_PARENT 0xFFFFu

static int i_ramble_kind_var(uint8_t k){
    return k == RAMBLE_VSTR || k == RAMBLE_VARR || k == RAMBLE_MAP;
}

uint32_t ramble_schema_scalar_size(RambleSchemaTypeKind kind){
    switch (kind){
        case RAMBLE_U8: case RAMBLE_I8: case RAMBLE_BOOL: return 1;
        case RAMBLE_U16: case RAMBLE_I16:               return 2;
        case RAMBLE_U32: case RAMBLE_I32: case RAMBLE_F32: return 4;
        case RAMBLE_U64: case RAMBLE_I64: case RAMBLE_F64: return 8;
        default: return 0;
    }
}

/* enum backing values: an integer kind, read and written like the scalar */
static int i_ramble_enum_backing_ok(uint8_t backing){ return backing <= (uint8_t)RAMBLE_I64; }
/* widened to int64, sign extended for a signed kind */
static int64_t i_ramble_enum_read_val(uint8_t backing, const uint8_t *p){
    switch (backing){
        case RAMBLE_U8:  return (int64_t)(uint64_t)p[0];
        case RAMBLE_U16: return (int64_t)(uint64_t)i_ramble_le_r16(p);
        case RAMBLE_U32: return (int64_t)(uint64_t)i_ramble_le_r32(p);
        case RAMBLE_U64: return (int64_t)i_ramble_le_r64(p);
        case RAMBLE_I8:  return (int64_t)(int8_t)p[0];
        case RAMBLE_I16: return (int64_t)(int16_t)i_ramble_le_r16(p);
        case RAMBLE_I32: return (int64_t)(int32_t)i_ramble_le_r32(p);
        case RAMBLE_I64: return (int64_t)i_ramble_le_r64(p);
        default:       return 0;
    }
}
/* truncated to the backing */
static void i_ramble_enum_write_val(uint8_t backing, uint8_t *p, int64_t v){
    uint64_t u = (uint64_t)v; uint32_t bs = ramble_schema_scalar_size((RambleSchemaTypeKind)backing), i;
    for (i = 0; i < bs; i++) p[i] = (uint8_t)(u >> (8 * i));
}
/* a u64 above INT64_MAX is not expressible */
static int i_ramble_enum_val_fits(uint8_t backing, int64_t v){
    switch (backing){
        case RAMBLE_U8:  return v >= 0 && v <= 0xFF;
        case RAMBLE_U16: return v >= 0 && v <= 0xFFFF;
        case RAMBLE_U32: return v >= 0 && v <= (int64_t)0xFFFFFFFF;
        case RAMBLE_U64: return v >= 0;
        case RAMBLE_I8:  return v >= -128 && v <= 127;
        case RAMBLE_I16: return v >= -32768 && v <= 32767;
        case RAMBLE_I32: return v >= (int64_t)(-2147483647 - 1) && v <= 2147483647;
        case RAMBLE_I64: return 1;
        default:       return 0;
    }
}

/* the bounds checked reader over possibly hostile wire bytes */
typedef struct { const uint8_t *w; size_t n, pos; int fail; } i_Rd;
static uint8_t  i_ramble_rd_u8 (i_Rd *r){ if (r->pos + 1 > r->n){ r->fail = 1; return 0; } return r->w[r->pos++]; }
static uint16_t i_ramble_rd_u16(i_Rd *r){ uint16_t v; if (r->pos + 2 > r->n){ r->fail = 1; return 0; } v = i_ramble_le_r16(r->w + r->pos); r->pos += 2; return v; }
static void     i_ramble_rd_skip(i_Rd *r, size_t k){ if (r->pos + k > r->n){ r->fail = 1; r->pos = r->n; return; } r->pos += k; }

/* The kind at r->pos with any NAMED wrappers peeled off, 0xFF if the wire runs out. */
static uint8_t i_ramble_peek_kind(const i_Rd *r){
    size_t p = r->pos;
    for (;;){
        if (p >= r->n) return 0xFFu;
        if (r->w[p] != RAMBLE_NAMED) return r->w[p];
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
static uint32_t i_ramble_rd_type_size(i_Rd *r, uint32_t *fields, uint32_t *nvar,
                                    uint16_t depth, uint32_t fl){
    uint8_t k = i_ramble_rd_u8(r);
    if (r->fail) return 0;
    switch (k){
        case RAMBLE_U8: case RAMBLE_I8: case RAMBLE_BOOL: return 1;
        case RAMBLE_U16: case RAMBLE_I16:               return 2;
        case RAMBLE_U32: case RAMBLE_I32: case RAMBLE_F32: return 4;
        case RAMBLE_U64: case RAMBLE_I64: case RAMBLE_F64: return 8;
        case RAMBLE_STR:
            return 2u + (uint32_t)i_ramble_rd_u16(r);              /* [u16 len][cap bytes] */
        case RAMBLE_ARR: {
            uint16_t count; uint8_t ek; uint64_t es;
            if (fl & I_T_ELEM_DIRECT){ r->fail = 1; return 0; }  /* no array of arrays */
            count = i_ramble_rd_u16(r);
            if (r->fail) return 0;
            ek = i_ramble_peek_kind(r);
            if (ek == 0xFFu){ r->fail = 1; return 0; }
            if ((fl & I_T_ELEM_INSIDE) && ek == RAMBLE_STRUCT){ r->fail = 1; return 0; }
            es = i_ramble_rd_type_size(r, fields, nvar, (uint16_t)(depth + 1),
                                     I_T_ELEM_DIRECT | I_T_ELEM_INSIDE);
            if (r->fail || es == 0){ r->fail = 1; return 0; }    /* elements must be fixed */
            if ((uint64_t)count * es > 0xFFFFFFFFu){ r->fail = 1; return 0; }
            return (uint32_t)((uint64_t)count * es);
        }
        case RAMBLE_ENUM: {
            uint8_t backing; uint16_t n; uint32_t bs, i;
            if (fl & I_T_ELEM_DIRECT){ r->fail = 1; return 0; }  /* the element kind is ambiguous */
            backing = i_ramble_rd_u8(r);
            n = i_ramble_rd_u16(r);
            bs = ramble_schema_scalar_size((RambleSchemaTypeKind)backing);
            if (r->fail || bs == 0 || !i_ramble_enum_backing_ok(backing)){ r->fail = 1; return 0; }
            for (i = 0; i < n && !r->fail; i++){                  /* skip the option table */
                i_ramble_rd_skip(r, bs);
                i_ramble_rd_skip(r, i_ramble_rd_u8(r));
            }
            if (r->fail) return 0;
            return bs;   /* on the wire: just the backing scalar */
        }
        case RAMBLE_VSTR: case RAMBLE_MAP:
            if (fl){ r->fail = 1; return 0; }                    /* never inside an array element */
            if (nvar) (*nvar)++;
            return 0;
        case RAMBLE_VARR: {
            uint64_t es;
            if (fl){ r->fail = 1; return 0; }
            if (i_ramble_peek_kind(r) == 0xFFu){ r->fail = 1; return 0; }
            es = i_ramble_rd_type_size(r, fields, nvar, (uint16_t)(depth + 1),
                                     I_T_ELEM_DIRECT | I_T_ELEM_INSIDE);
            if (r->fail || es == 0){ r->fail = 1; return 0; }
            if (nvar) (*nvar)++;
            return 0;
        }
        case RAMBLE_STRUCT: {
            uint8_t nf; uint64_t sum = 0; uint16_t i;
            if (depth >= RAMBLE_SCHEMA_MAX_DEPTH){ r->fail = 1; return 0; }
            nf = i_ramble_rd_u8(r);
            for (i = 0; i < nf && !r->fail; i++){
                uint8_t fnl = i_ramble_rd_u8(r);
                i_ramble_rd_skip(r, fnl);
                if (fields) (*fields)++;
                sum += i_ramble_rd_type_size(r, fields, nvar, (uint16_t)(depth + 1),
                                           fl & I_T_ELEM_INSIDE);
            }
            if (sum > 0xFFFFFFFFu){ r->fail = 1; return 0; }
            return (uint32_t)sum;
        }
        case RAMBLE_NAMED: {
            uint8_t nl = i_ramble_rd_u8(r);
            if (r->fail || nl == 0){ r->fail = 1; return 0; }    /* a name is required */
            i_ramble_rd_skip(r, nl);
            if (r->fail) return 0;
            if (r->pos < r->n && r->w[r->pos] == RAMBLE_NAMED){ r->fail = 1; return 0; }
            return i_ramble_rd_type_size(r, fields, nvar, depth, fl);
        }
        default: r->fail = 1; return 0;                          /* unknown kind: reject */
    }
}

/* Offset of the compiled handle in buf: past the wire bytes, 8 aligned. */
static size_t i_ramble_schema_handle_off(const uint8_t *buf, size_t wire_len){
    uintptr_t addr = (uintptr_t)buf + wire_len;
    size_t pad = (size_t)((8u - (addr & 7u)) & 7u);
    return wire_len + pad;
}

/* Counts every field of the root into *n and its variable fields into *nvar. 0 on
 * malformed wire. A bare or alias root is 1 plus whatever its type flattens to. */
static int i_ramble_schema_wire_fields(const void *wire, size_t wire_len,
                                     uint32_t *n, uint32_t *nvar){
    i_Rd r; uint8_t ver, rl;
    *n = 0; *nvar = 0;
    r.w = (const uint8_t *)wire; r.n = wire_len; r.pos = 0; r.fail = 0;
    ver = i_ramble_rd_u8(&r); if (r.fail || ver != RAMBLE_SCHEMA_WIRE_VERSION) return 0;
    rl = i_ramble_rd_u8(&r); i_ramble_rd_skip(&r, rl);
    if (r.fail || (size_t)r.pos >= r.n) return 0;
    if (r.w[r.pos] == RAMBLE_NAMED) return 0;          /* a root's name rides the header */
    if (r.w[r.pos] == RAMBLE_STRUCT){
        i_ramble_rd_type_size(&r, n, nvar, 0, 0);
    } else {
        uint32_t sub = 0;
        i_ramble_rd_type_size(&r, &sub, nvar, 0, 0);   /* depth 0: a variable root is allowed */
        *n = 1u + sub;                                 /* the root is the first field */
    }
    return r.fail ? 0 : 1;
}

/* flattening a wire type into the field table */
static void i_ramble_field_init(i_Field *f, RambleString name, uint16_t depth, uint16_t parent,
                              uint32_t offset){
    memset(f, 0, sizeof *f);
    f->name = name;
    f->type_name = ramble_string(NULL, 0);
    f->elem_name = ramble_string(NULL, 0);
    f->depth = depth; f->parent = parent; f->arr_parent = I_RAMBLE_NO_PARENT;
    f->offset = offset;
}

static uint32_t i_ramble_emit_type(uint8_t *buf, size_t wl, i_Rd *r, RambleSchema *s,
                                 uint16_t *emitted, uint32_t total,
                                 uint16_t idx, uint16_t depth, uint32_t base);

/* Emits the members of the struct body at r->pos into the table, depth first. Returns
 * the struct's fixed size and sets r->fail on malformed wire. */
static uint32_t i_ramble_emit_struct(uint8_t *buf, size_t wl, i_Rd *r, RambleSchema *s,
                                   uint16_t *emitted, uint32_t total,
                                   uint16_t depth, uint16_t parent, uint32_t base){
    uint8_t nf = i_ramble_rd_u8(r); uint16_t i;
    uint32_t running = base;
    if (depth >= RAMBLE_SCHEMA_MAX_DEPTH){ r->fail = 1; return 0; }
    for (i = 0; i < nf && !r->fail; i++){
        uint8_t fl = i_ramble_rd_u8(r);
        const char *fn = (const char *)(buf + r->pos);
        uint16_t idx; uint32_t sz;
        i_ramble_rd_skip(r, fl);
        if (r->fail) return 0;
        if (*emitted >= total){ r->fail = 1; return 0; }
        idx = (*emitted)++;
        i_ramble_field_init(&s->fields[idx], ramble_string(fn, fl), depth, parent, running);
        sz = i_ramble_emit_type(buf, wl, r, s, emitted, total, idx, depth, running);
        if (r->fail) return 0;
        running += sz;
    }
    return running - base;
}

/* Fills field idx from the type at r->pos, NAMED wrappers unwrapped into type_name, and
 * emits any child fields it flattens to. base is where the field's storage starts. */
static uint32_t i_ramble_emit_type(uint8_t *buf, size_t wl, i_Rd *r, RambleSchema *s,
                                 uint16_t *emitted, uint32_t total,
                                 uint16_t idx, uint16_t depth, uint32_t base){
    size_t kpos = r->pos;
    uint8_t k;
    uint32_t sz = 0;
    i_Field *f = &s->fields[idx];
    f->type_off = (uint32_t)kpos;
    k = i_ramble_rd_u8(r);
    if (k == RAMBLE_NAMED){
        uint8_t nl = i_ramble_rd_u8(r);
        const char *tn = (const char *)(buf + r->pos);
        i_ramble_rd_skip(r, nl);
        if (r->fail || nl == 0){ r->fail = 1; return 0; }
        f->type_name = ramble_string(tn, nl);
        k = i_ramble_rd_u8(r);
        if (k == RAMBLE_NAMED){ r->fail = 1; return 0; }
    }
    if (r->fail) return 0;
    f->kind = k;
    switch (k){
        case RAMBLE_STRUCT:
            sz = i_ramble_emit_struct(buf, wl, r, s, emitted, total,
                                    (uint16_t)(depth + 1), idx, base);
            break;
        case RAMBLE_ARR: case RAMBLE_VARR: {
            uint8_t ek; uint32_t esz = 0;
            if (k == RAMBLE_ARR){
                f->count = i_ramble_rd_u16(r);
                if (r->fail) return 0;
            }
            ek = i_ramble_rd_u8(r);
            if (ek == RAMBLE_NAMED){
                uint8_t nl = i_ramble_rd_u8(r);
                const char *tn = (const char *)(buf + r->pos);
                i_ramble_rd_skip(r, nl);
                if (r->fail || nl == 0){ r->fail = 1; return 0; }
                f->elem_name = ramble_string(tn, nl);
                ek = i_ramble_rd_u8(r);
                if (ek == RAMBLE_NAMED){ r->fail = 1; return 0; }
            }
            if (r->fail) return 0;
            f->elem = ek;
            if (ek == RAMBLE_STRUCT){                       /* one element 0 template */
                esz = i_ramble_emit_struct(buf, wl, r, s, emitted, total,
                                         (uint16_t)(depth + 1), idx,
                                         k == RAMBLE_ARR ? base : 0u);
            } else if (ek == RAMBLE_STR){
                f->str_cap = i_ramble_rd_u16(r);
                esz = 2u + (uint32_t)f->str_cap;
            } else {
                esz = ramble_schema_scalar_size((RambleSchemaTypeKind)ek);
            }
            if (r->fail || esz == 0){ r->fail = 1; return 0; }
            f->elem_size = esz;
            if (k == RAMBLE_ARR){
                if ((uint64_t)f->count * esz > 0xFFFFFFFFu){ r->fail = 1; return 0; }
                sz = (uint32_t)((uint64_t)f->count * esz);
            }
            break;
        }
        case RAMBLE_STR:
            f->str_cap = i_ramble_rd_u16(r);
            sz = 2u + (uint32_t)f->str_cap;
            break;
        case RAMBLE_ENUM: {
            uint16_t i;
            f->elem = i_ramble_rd_u8(r);
            f->count = i_ramble_rd_u16(r);
            sz = ramble_schema_scalar_size((RambleSchemaTypeKind)f->elem);
            if (r->fail || sz == 0 || !i_ramble_enum_backing_ok(f->elem)){ r->fail = 1; return 0; }
            for (i = 0; i < f->count && !r->fail; i++){
                i_ramble_rd_skip(r, sz);
                i_ramble_rd_skip(r, i_ramble_rd_u8(r));
            }
            break;
        }
        case RAMBLE_VSTR: case RAMBLE_MAP:
            sz = 0;
            break;
        default:
            sz = ramble_schema_scalar_size((RambleSchemaTypeKind)k);
            if (sz == 0){ r->fail = 1; return 0; }
            break;
    }
    if (r->fail) return 0;
    f = &s->fields[idx];                      /* unchanged, recursion never moves it */
    f->type_len = (uint32_t)(r->pos - kpos);
    f->size = sz;
    return sz;
}

/* Compiles the wire bytes at buf[0..wire_len] into a RambleSchema placed after them in
 * buf. NULL on a malformed blob or when cap is too small. */
static RambleSchema *i_ramble_schema_compile(uint8_t *buf, size_t wire_len, size_t cap){
    i_Rd r; uint8_t ver, root_kind, root_namelen;
    const char *root_name; size_t hoff, need; RambleSchema *s;
    uint32_t total, nvar; uint16_t emitted = 0;

    if (!i_ramble_schema_wire_fields(buf, wire_len, &total, &nvar) || total > 0xFFFFu) return NULL;

    r.w = buf; r.n = wire_len; r.pos = 0; r.fail = 0;
    ver = i_ramble_rd_u8(&r);
    if (r.fail || ver != RAMBLE_SCHEMA_WIRE_VERSION) return NULL;
    root_namelen = i_ramble_rd_u8(&r);
    root_name = (const char *)(buf + r.pos);
    i_ramble_rd_skip(&r, root_namelen);
    if (r.fail || r.pos >= r.n) return NULL;
    root_kind = buf[r.pos];

    hoff = i_ramble_schema_handle_off(buf, wire_len);
    need = hoff + sizeof(RambleSchema) + (size_t)(total ? total - 1u : 0u) * sizeof(i_Field);
    if (need > cap) return NULL;

    s = (RambleSchema *)(buf + hoff);
    s->wire = ramble_bytes(buf, wire_len);
    s->name = ramble_string(root_name, root_namelen);
    s->nfields = (uint16_t)total;
    s->n_var = (uint16_t)nvar;
    s->value_root = (uint8_t)(root_kind != RAMBLE_STRUCT);
    s->hash = i_ramble_fnv1a64(buf, wire_len);
    if (!s->value_root){
        r.pos++;                                 /* past the root's STRUCT kind byte */
        s->size = i_ramble_emit_struct(buf, wire_len, &r, s, &emitted, total,
                                     0, I_RAMBLE_NO_PARENT, 0);
    } else {                                     /* the bare or alias root is the first field */
        if (total == 0) return NULL;
        i_ramble_field_init(&s->fields[0], ramble_string((const char *)(buf + r.pos), 0),
                          0, I_RAMBLE_NO_PARENT, 0);
        emitted = 1;
        s->size = i_ramble_emit_type(buf, wire_len, &r, s, &emitted, total, 0, 0, 0);
    }
    if (r.fail || emitted != (uint16_t)total) return NULL;
    {   /* tail frame ordinals in depth first order, and the array ancestry */
        uint16_t i, ord = 0;
        for (i = 0; i < s->nfields; i++){
            i_Field *f = &s->fields[i];
            if (f->parent != I_RAMBLE_NO_PARENT){
                const i_Field *p = &s->fields[f->parent];
                f->arr_parent = (p->kind == RAMBLE_ARR || p->kind == RAMBLE_VARR)
                              ? f->parent : p->arr_parent;
            }
            if (i_ramble_kind_var(f->kind)){
                f->offset = 0;                   /* a frame has no static offset */
                f->var_ord = ord++;
            }
        }
    }
    return s;
}
/* the builder */
/* room for extra more bytes, growing the wire buffer through the hook */
static int i_ramble_schema_builder_reserve(RambleSchemaBuilder *b, size_t extra){
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
static void i_ramble_schema_builder_put(RambleSchemaBuilder *b, uint8_t v){
    if (!i_ramble_schema_builder_reserve(b, 1)) return;
    b->buf[b->len++] = v;
}
static void i_ramble_schema_builder_put_u16(RambleSchemaBuilder *b, uint16_t v){
    if (!i_ramble_schema_builder_reserve(b, 2)) return;
    i_ramble_le_w16(b->buf + b->len, v); b->len += 2;
}
/* raw bytes, a compiled type encoding from elsewhere */
static void i_ramble_schema_builder_put_raw(RambleSchemaBuilder *b, const void *src, size_t len){
    if (!len) return;
    if (!src){ b->err = -6; return; }
    if (!i_ramble_schema_builder_reserve(b, len)) return;
    memcpy(b->buf + b->len, src, len);
    b->len += len;
}
/* bytes little endian bytes of v, an enum option's backing sized value */
static void i_ramble_schema_builder_put_le(RambleSchemaBuilder *b, uint64_t v, uint32_t bytes){
    uint32_t i;
    if (!i_ramble_schema_builder_reserve(b, bytes)) return;
    for (i = 0; i < bytes; i++) b->buf[b->len++] = (uint8_t)(v >> (8 * i));
}
static void i_ramble_schema_builder_put_name(RambleSchemaBuilder *b, const char *name){
    size_t n = 0, i; if (name) while (name[n]) n++;
    if (b->depth == 0 && (b->raw_type || b->value_root)){   /* the root carries no field name */
        if (n) b->err = -4;
        return;
    }
    if (n > 255){ b->err = -4; return; }
    if (!i_ramble_schema_builder_reserve(b, 1 + n)) return;
    b->buf[b->len++] = (uint8_t)n;
    for (i = 0; i < n; i++) b->buf[b->len++] = (uint8_t)name[i];
}
/* count a field on the innermost open struct. A bare or alias root takes exactly one type */
static void i_ramble_schema_builder_count(RambleSchemaBuilder *b){
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
static void i_ramble_schema_builder_open_struct(RambleSchemaBuilder *b){
    if (b->err) return;
    if (b->depth >= RAMBLE_SCHEMA_MAX_DEPTH){ b->err = -2; return; }
    i_ramble_schema_builder_put(b, (uint8_t)RAMBLE_STRUCT);
    b->count_pos[b->depth] = b->len;
    i_ramble_schema_builder_put(b, 0);
    b->field_count[b->depth] = 0;
    b->depth++;
}

RambleSchemaBuilder ramble_schema_begin(RambleAllocFn alloc, void *user, const char *root_name){
    RambleSchemaBuilder b;
    memset(&b, 0, sizeof b);
    b.alloc = alloc; b.user = user; b.arr_depth = 0xFFFFu; b.base_depth = 1;
    if (!alloc){ b.err = -1; return b; }
    b.cap = 64u;
    b.buf = (uint8_t *)alloc(user, NULL, b.cap);
    if (!b.buf){ b.err = -1; b.cap = 0; return b; }
    i_ramble_schema_builder_put(&b, (uint8_t)RAMBLE_SCHEMA_WIRE_VERSION);
    i_ramble_schema_builder_put_name(&b, root_name);
    i_ramble_schema_builder_open_struct(&b);              /* the root is a struct, depth 1 */
    return b;
}

/* the shared head of a bare type root: [version][root_namelen][root_name] */
static RambleSchemaBuilder i_ramble_schema_begin_bare(RambleAllocFn alloc, void *user,
                                                  const char *name){
    RambleSchemaBuilder b; size_t n = 0, i;
    memset(&b, 0, sizeof b);
    b.alloc = alloc; b.user = user; b.arr_depth = 0xFFFFu;
    if (name) while (name[n]) n++;
    if (!alloc || n > 255){ b.err = -1; return b; }
    b.cap = 64u;
    b.buf = (uint8_t *)alloc(user, NULL, b.cap);
    if (!b.buf){ b.err = -1; b.cap = 0; return b; }
    b.value_root = 1;                                  /* depth stays 0: no struct is open */
    i_ramble_schema_builder_put(&b, (uint8_t)RAMBLE_SCHEMA_WIRE_VERSION);
    i_ramble_schema_builder_put(&b, (uint8_t)n);
    for (i = 0; i < n; i++) i_ramble_schema_builder_put(&b, (uint8_t)name[i]);
    return b;
}

RambleSchemaBuilder ramble_schema_begin_value(RambleAllocFn alloc, void *user){
    return i_ramble_schema_begin_bare(alloc, user, NULL);
}

RambleSchemaBuilder ramble_schema_begin_alias(RambleAllocFn alloc, void *user, const char *name){
    RambleSchemaBuilder b = i_ramble_schema_begin_bare(alloc, user, name);
    if (!b.err && (!name || !name[0])) b.err = -4;     /* an alias needs a name */
    return b;
}

/* a standalone type encoding with no header and no field name: the DSL's definition arena */
static RambleSchemaBuilder i_ramble_schema_begin_raw(RambleAllocFn alloc, void *user){
    RambleSchemaBuilder b;
    memset(&b, 0, sizeof b);
    b.alloc = alloc; b.user = user; b.arr_depth = 0xFFFFu; b.raw_type = 1;
    if (!alloc){ b.err = -1; return b; }
    b.cap = 64u;
    b.buf = (uint8_t *)alloc(user, NULL, b.cap);
    if (!b.buf){ b.err = -1; b.cap = 0; }
    return b;
}

void ramble_schema_field(RambleSchemaBuilder *b, const char *name, RambleSchemaTypeKind kind){
    if (!b || b->err) return;
    if (ramble_schema_scalar_size(kind) == 0){ b->err = -6; return; }  /* fixed scalars only */
    i_ramble_schema_builder_count(b); i_ramble_schema_builder_put_name(b, name); i_ramble_schema_builder_put(b, (uint8_t)kind);
}

void ramble_schema_field_array(RambleSchemaBuilder *b, const char *name,
                             RambleSchemaTypeKind elem_scalar, uint16_t count){
    if (!b || b->err) return;
    if (ramble_schema_scalar_size(elem_scalar) == 0){ b->err = -6; return; }  /* scalars only */
    i_ramble_schema_builder_count(b); i_ramble_schema_builder_put_name(b, name);
    i_ramble_schema_builder_put(b, (uint8_t)RAMBLE_ARR); i_ramble_schema_builder_put_u16(b, count);
    i_ramble_schema_builder_put(b, (uint8_t)elem_scalar);
}

void ramble_schema_field_string(RambleSchemaBuilder *b, const char *name, uint16_t cap){
    if (!b || b->err) return;
    if (cap == 0){ b->err = -6; return; }
    i_ramble_schema_builder_count(b); i_ramble_schema_builder_put_name(b, name);
    i_ramble_schema_builder_put(b, (uint8_t)RAMBLE_STR); i_ramble_schema_builder_put_u16(b, cap);
}

void ramble_schema_field_string_array(RambleSchemaBuilder *b, const char *name,
                                    uint16_t cap, uint16_t count){
    if (!b || b->err) return;
    if (cap == 0){ b->err = -6; return; }
    i_ramble_schema_builder_count(b); i_ramble_schema_builder_put_name(b, name);
    i_ramble_schema_builder_put(b, (uint8_t)RAMBLE_ARR); i_ramble_schema_builder_put_u16(b, count);
    i_ramble_schema_builder_put(b, (uint8_t)RAMBLE_STR); i_ramble_schema_builder_put_u16(b, cap);
}

/* variable kinds may sit at any struct depth but never inside an array element */
static int i_ramble_schema_builder_var_ok(RambleSchemaBuilder *b){
    if (b->err) return 0;
    if (b->arr_depth != 0xFFFFu){ b->err = -8; return 0; }
    return 1;
}

void ramble_schema_field_var_string(RambleSchemaBuilder *b, const char *name){
    if (!b || !i_ramble_schema_builder_var_ok(b)) return;
    i_ramble_schema_builder_count(b); i_ramble_schema_builder_put_name(b, name);
    i_ramble_schema_builder_put(b, (uint8_t)RAMBLE_VSTR);
}

void ramble_schema_field_var_array(RambleSchemaBuilder *b, const char *name,
                                 RambleSchemaTypeKind elem_scalar){
    if (!b || !i_ramble_schema_builder_var_ok(b)) return;
    if (ramble_schema_scalar_size(elem_scalar) == 0){ b->err = -6; return; }  /* scalars only */
    i_ramble_schema_builder_count(b); i_ramble_schema_builder_put_name(b, name);
    i_ramble_schema_builder_put(b, (uint8_t)RAMBLE_VARR); i_ramble_schema_builder_put(b, (uint8_t)elem_scalar);
}

void ramble_schema_field_var_string_array(RambleSchemaBuilder *b, const char *name, uint16_t cap){
    if (!b || !i_ramble_schema_builder_var_ok(b)) return;
    if (cap == 0){ b->err = -6; return; }
    i_ramble_schema_builder_count(b); i_ramble_schema_builder_put_name(b, name);
    i_ramble_schema_builder_put(b, (uint8_t)RAMBLE_VARR);
    i_ramble_schema_builder_put(b, (uint8_t)RAMBLE_STR); i_ramble_schema_builder_put_u16(b, cap);
}

void ramble_schema_field_map(RambleSchemaBuilder *b, const char *name){
    if (!b || !i_ramble_schema_builder_var_ok(b)) return;
    i_ramble_schema_builder_count(b); i_ramble_schema_builder_put_name(b, name);
    i_ramble_schema_builder_put(b, (uint8_t)RAMBLE_MAP);
}

/* the root type bytes of a compiled schema, past [version][namelen][name] */
static int i_ramble_schema_root_type(const RambleSchema *t, const uint8_t **bytes, size_t *len){
    size_t off;
    if (!t || !t->wire.data) return 0;
    off = 2u + t->name.len;
    if (off >= t->wire.len) return 0;
    *bytes = t->wire.data + off;
    *len   = t->wire.len - off;
    return 1;
}

/* emit [NAMED][len][name] plus the referenced schema's root type */
static void i_ramble_schema_put_named(RambleSchemaBuilder *b, const RambleSchema *type){
    const uint8_t *tb; size_t tl;
    if (b->err) return;
    if (!type || !type->name.len || type->name.len > 255 ||
        !i_ramble_schema_root_type(type, &tb, &tl)){ b->err = -6; return; }
    i_ramble_schema_builder_put(b, (uint8_t)RAMBLE_NAMED);
    i_ramble_schema_builder_put(b, (uint8_t)type->name.len);
    i_ramble_schema_builder_put_raw(b, type->name.data, type->name.len);
    i_ramble_schema_builder_put_raw(b, tb, tl);
}

void ramble_schema_field_named(RambleSchemaBuilder *b, const char *name, const RambleSchema *type){
    if (!b || b->err) return;
    i_ramble_schema_builder_count(b); i_ramble_schema_builder_put_name(b, name);
    i_ramble_schema_put_named(b, type);
}

void ramble_schema_field_named_array(RambleSchemaBuilder *b, const char *name,
                                   const RambleSchema *type, uint16_t count){
    if (!b || b->err) return;
    if (count == 0 && !i_ramble_schema_builder_var_ok(b)) return;
    i_ramble_schema_builder_count(b); i_ramble_schema_builder_put_name(b, name);
    if (count){
        i_ramble_schema_builder_put(b, (uint8_t)RAMBLE_ARR);
        i_ramble_schema_builder_put_u16(b, count);
    } else {
        i_ramble_schema_builder_put(b, (uint8_t)RAMBLE_VARR);
    }
    i_ramble_schema_put_named(b, type);
}

/* Streaming enum construction, shared with the DSL so it never buffers the option list:
 * open writes the head and returns the count placeholder, add appends, finish backpatches. */
static size_t i_ramble_schema_field_enum_open(RambleSchemaBuilder *b, const char *name,
                                            RambleSchemaTypeKind backing){
    size_t count_pos;
    i_ramble_schema_builder_count(b);
    i_ramble_schema_builder_put_name(b, name);
    i_ramble_schema_builder_put(b, (uint8_t)RAMBLE_ENUM);
    i_ramble_schema_builder_put(b, (uint8_t)backing);
    count_pos = b->len;
    i_ramble_schema_builder_put_u16(b, 0);               /* n, backpatched by finish */
    return count_pos;
}
static void i_ramble_schema_field_enum_add(RambleSchemaBuilder *b, RambleSchemaTypeKind backing,
                                         int64_t value, const char *name, size_t name_len){
    size_t i;
    if (!b || b->err) return;
    if (name_len > 255){ b->err = -4; return; }
    i_ramble_schema_builder_put_le(b, (uint64_t)value, ramble_schema_scalar_size(backing));
    if (!i_ramble_schema_builder_reserve(b, 1 + name_len)) return;
    b->buf[b->len++] = (uint8_t)name_len;
    for (i = 0; i < name_len; i++) b->buf[b->len++] = (uint8_t)name[i];
}
static void i_ramble_schema_field_enum_finish(RambleSchemaBuilder *b, size_t count_pos, uint16_t count){
    if (!b || b->err) return;
    i_ramble_le_w16(b->buf + count_pos, count);
}

void ramble_schema_field_enum(RambleSchemaBuilder *b, const char *name, RambleSchemaTypeKind backing,
                            const RambleEnumVariant *variants, uint16_t n){
    size_t count_pos; uint16_t i;
    if (!b || b->err) return;
    if (ramble_schema_scalar_size(backing) == 0 || !i_ramble_enum_backing_ok((uint8_t)backing)){
        b->err = -6; return;                           /* integer backings only */
    }
    count_pos = i_ramble_schema_field_enum_open(b, name, backing);
    for (i = 0; i < n; i++){
        int64_t v = variants ? variants[i].value : 0;
        const char *vn = variants ? variants[i].name : NULL;
        size_t vl = 0; if (vn) while (vn[vl]) vl++;
        if (!i_ramble_enum_val_fits((uint8_t)backing, v)){ b->err = -6; return; }
        i_ramble_schema_field_enum_add(b, backing, v, vn, vl);
    }
    i_ramble_schema_field_enum_finish(b, count_pos, n);
}

void ramble_schema_begin_struct(RambleSchemaBuilder *b, const char *name){
    if (!b || b->err) return;
    if (b->value_root && b->depth == 0){ b->err = -3; return; }  /* struct roots use begin */
    i_ramble_schema_builder_count(b);                 /* a field of the parent */
    i_ramble_schema_builder_put_name(b, name);
    i_ramble_schema_builder_open_struct(b);
}

void ramble_schema_begin_struct_array(RambleSchemaBuilder *b, const char *name, uint16_t count){
    if (!b || b->err) return;
    if (count == 0 && !i_ramble_schema_builder_var_ok(b)) return;
    if (b->arr_depth != 0xFFFFu){ b->err = -8; return; }   /* one array level only */
    i_ramble_schema_builder_count(b);
    i_ramble_schema_builder_put_name(b, name);
    if (count){
        i_ramble_schema_builder_put(b, (uint8_t)RAMBLE_ARR);
        i_ramble_schema_builder_put_u16(b, count);
    } else {
        i_ramble_schema_builder_put(b, (uint8_t)RAMBLE_VARR);
    }
    i_ramble_schema_builder_open_struct(b);
    if (!b->err) b->arr_depth = b->depth;           /* this level is an array element */
}

void ramble_schema_end_struct(RambleSchemaBuilder *b){
    if (!b || b->err) return;
    if (b->depth <= b->base_depth){ b->err = -3; return; }   /* the root closes in finish */
    if (b->arr_depth == b->depth) b->arr_depth = 0xFFFFu;
    b->depth--;
    b->buf[b->count_pos[b->depth]] = (uint8_t)b->field_count[b->depth];
}

RambleSchema *ramble_schema_finish(RambleSchemaBuilder *b){
    RambleSchema *s = NULL;
    int closed = b && !b->err &&
                 (b->value_root ? (b->depth == 0 && b->value_root == 2)   /* the one bare type */
                                : b->depth == 1);            /* else an unbalanced begin and end */
    if (closed){
        size_t need; uint8_t *nb; uint32_t total = 0, nvar = 0;
        if (!b->value_root)
            b->buf[b->count_pos[0]] = (uint8_t)b->field_count[0]; /* backpatch the count */
        i_ramble_schema_wire_fields(b->buf, b->len, &total, &nvar);
        need = b->len + 7u + sizeof(RambleSchema)
             + (size_t)(total ? total - 1u : 0u) * sizeof(i_Field);
        nb = (uint8_t *)b->alloc(b->user, b->buf, need);      /* room for the handle */
        if (nb){ b->buf = nb; b->cap = need; s = i_ramble_schema_compile(b->buf, b->len, b->cap); }
    }
    if (!s && b && b->buf) b->alloc(b->user, b->buf, 0);      /* free on any failure */
    if (b) b->buf = NULL;                                     /* owned by s now, or freed */
    return s;
}

/* bytes a compiled schema needs for wire: the copy, the alignment pad, the handle and
   the field table. 0 if the wire is malformed. */
static size_t i_ramble_schema_compiled_size(const void *wire, size_t wire_len){
    uint32_t total, nvar;
    if (!i_ramble_schema_wire_fields(wire, wire_len, &total, &nvar) || total > 0xFFFFu) return 0;
    return wire_len + 7u + sizeof(RambleSchema) + (size_t)(total ? total - 1u : 0u) * sizeof(i_Field);
}

RambleSchema *ramble_schema_parse(const void *wire, size_t wire_len, RambleAllocFn alloc, void *user){
    size_t need, i; uint8_t *buf; RambleSchema *s;
    if (!wire || !alloc || wire_len == 0) return NULL;
    need = i_ramble_schema_compiled_size(wire, wire_len);
    if (need == 0) return NULL;                              /* a malformed header */
    buf = (uint8_t *)alloc(user, NULL, need);
    if (!buf) return NULL;
    for (i = 0; i < wire_len; i++) buf[i] = ((const uint8_t *)wire)[i];   /* persist the bytes */
    s = i_ramble_schema_compile(buf, wire_len, need);
    if (!s) alloc(user, buf, 0);                             /* a malformed body: no leak */
    return s;
}

RambleSchema *ramble_schema_copy(const RambleSchema *s, RambleAllocFn alloc, void *user){
    RambleBytes w;
    if (!s || !alloc) return NULL;
    w = ramble_schema_wire(s);
    return ramble_schema_parse(w.data, w.len, alloc, user);
}

void ramble_schema_free(RambleSchema *s, RambleAllocFn alloc, void *user){
    if (s && alloc) alloc(user, (void *)s->wire.data, 0);    /* wire.data is the block base */
}

/* queries */
RambleBytes ramble_schema_wire(const RambleSchema *s){
    RambleBytes b; if (s) return s->wire;
    b.data = NULL; b.len = 0; return b;
}
uint64_t     ramble_schema_hash(const RambleSchema *s){ return s ? s->hash : 0; }
RambleString ramble_schema_name(const RambleSchema *s){
    RambleString n; if (s) return s->name;
    n.data = NULL; n.len = 0; return n;
}
uint32_t ramble_schema_size(const RambleSchema *s){ return s ? s->size : 0; }
uint16_t ramble_schema_field_count(const RambleSchema *s){ return s ? s->nfields : 0; }

int ramble_schema_field_at(const RambleSchema *s, uint16_t i, RambleSchemaFieldInfo *out){
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
static int i_ramble_schema_path_match(const RambleSchema *s, const i_Field *f, const char *path,
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
                    if (f->kind != RAMBLE_ARR && f->kind != RAMBLE_VARR) return 0;
                    found = idx;
                    nend = br - 1;
                }
            }
        }
        if (f->name.len != (size_t)(nend - seg) ||
            (f->name.len && memcmp(f->name.data, seg, f->name.len) != 0)) return 0;
        if (f->parent == I_RAMBLE_NO_PARENT){                /* root: all segments consumed */
            if (seg != path) return 0;
            if (index) *index = found;
            return 1;
        }
        if (seg == path) return 0;                         /* segments ran out early */
        end = seg - 1;                                     /* past the dot */
        f = &s->fields[f->parent];
    }
}

static const i_Field *i_ramble_schema_field_by_path(const RambleSchema *s, const char *path,
                                                  uint32_t *index){
    uint16_t i; size_t n;
    if (index) *index = 0;
    if (!s || !path) return NULL;
    n = strlen(path);
    for (i = 0; i < s->nfields; i++)
        if (i_ramble_schema_path_match(s, &s->fields[i], path, n, index)) return &s->fields[i];
    return NULL;
}

int ramble_schema_field_index(const RambleSchema *s, const char *path){
    const i_Field *f = i_ramble_schema_field_by_path(s, path, NULL);
    return f ? (int)(f - s->fields) : -1;
}

RambleBytes ramble_schema_field_type_wire(const RambleSchema *s, uint16_t field){
    const i_Field *f;
    if (!s || field >= s->nfields) return ramble_bytes(NULL, 0);
    f = &s->fields[field];
    if ((size_t)f->type_off + f->type_len > s->wire.len) return ramble_bytes(NULL, 0);
    return ramble_bytes(s->wire.data + f->type_off, f->type_len);
}

uint16_t ramble_schema_enum_count(const RambleSchema *s, uint16_t field){
    if (!s || field >= s->nfields) return 0;
    return s->fields[field].kind == RAMBLE_ENUM ? s->fields[field].count : 0;
}

int ramble_schema_enum_variant(const RambleSchema *s, uint16_t field, uint16_t i,
                             int64_t *value, RambleString *name){
    const i_Field *f; const uint8_t *w; uint32_t pos, end; uint16_t k; uint8_t bs;
    if (!s || field >= s->nfields) return 0;
    f = &s->fields[field];
    if (f->kind != RAMBLE_ENUM || i >= f->count) return 0;
    bs  = (uint8_t)ramble_schema_scalar_size((RambleSchemaTypeKind)f->elem);
    w   = s->wire.data;
    /* past an optional NAMED tag, then [ENUM][backing][u16 n], to the first option */
    pos = f->type_off;
    if (w[pos] == RAMBLE_NAMED) pos += 2u + w[pos + 1];
    pos += 4u;
    end = f->type_off + f->type_len;
    for (k = 0; k < i; k++){                 /* hop over earlier options: [value][u8 nl][name] */
        if ((size_t)pos + bs + 1u > end) return 0;
        pos += bs + 1u + w[pos + bs];
    }
    if ((size_t)pos + bs + 1u > end) return 0;
    { uint8_t nl = w[pos + bs];
      if ((size_t)pos + bs + 1u + nl > end) return 0;
      if (value) *value = i_ramble_enum_read_val(f->elem, w + pos);
      if (name)  *name  = ramble_string((const char *)(w + pos + bs + 1u), nl); }
    return 1;
}

RambleString ramble_enum_name_of(const RambleSchema *s, uint16_t field, int64_t value){
    uint16_t i, n = ramble_schema_enum_count(s, field);
    for (i = 0; i < n; i++){
        int64_t v; RambleString nm;
        if (ramble_schema_enum_variant(s, field, i, &v, &nm) && v == value) return nm;
    }
    return ramble_string(NULL, 0);
}

int ramble_enum_value_of(const RambleSchema *s, uint16_t field, const char *name, int64_t *out){
    uint16_t i, n = ramble_schema_enum_count(s, field);
    size_t want = 0;
    if (name) while (name[want]) want++;
    for (i = 0; i < n; i++){
        int64_t v; RambleString nm;
        if (ramble_schema_enum_variant(s, field, i, &v, &nm) && nm.len == want &&
            (want == 0 || memcmp(nm.data, name, want) == 0)){ if (out) *out = v; return 1; }
    }
    return 0;
}
/* spelling types back as DSL text */
static const char *i_ramble_why_kind(uint8_t k){
    switch (k){
        case RAMBLE_U8:  return "u8";  case RAMBLE_U16: return "u16";
        case RAMBLE_U32: return "u32"; case RAMBLE_U64: return "u64";
        case RAMBLE_I8:  return "i8";  case RAMBLE_I16: return "i16";
        case RAMBLE_I32: return "i32"; case RAMBLE_I64: return "i64";
        case RAMBLE_F32: return "f32"; case RAMBLE_F64: return "f64";
        case RAMBLE_BOOL: return "bool"; case RAMBLE_STRUCT: return "struct";
        default: return "?";
    }
}

/* A counting text sink: appends into [p, end) but always tallies the full length in n,
   so a NULL or short buffer still measures. */
typedef struct { char *p, *end; uint32_t n; } i_RambleTextOut;
static void i_ramble_out_raw(i_RambleTextOut *o, const char *s, size_t len){
    size_t i;
    o->n += (uint32_t)len;
    for (i = 0; i < len && o->p < o->end; i++) *o->p++ = s[i];
}
static void i_ramble_out_str(i_RambleTextOut *o, const char *s){ i_ramble_out_raw(o, s, strlen(s)); }
static void i_ramble_out_view(i_RambleTextOut *o, RambleString v){ if (v.data) i_ramble_out_raw(o, v.data, v.len); }
static void i_ramble_out_indent(i_RambleTextOut *o, int levels){ while (levels-- > 0) i_ramble_out_raw(o, "  ", 2); }
static void i_ramble_out_i64(i_RambleTextOut *o, int64_t v){
    char tmp[20]; int k = 0; uint64_t u = v < 0 ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    if (v < 0) i_ramble_out_raw(o, "-", 1);
    do { tmp[k++] = (char)('0' + (int)(u % 10)); u /= 10; } while (u);
    while (k) { char c = tmp[--k]; i_ramble_out_raw(o, &c, 1); }
}

/* Steps over the type at pos in already validated wire. Returns the position past it. */
static size_t i_ramble_skip_type(const uint8_t *w, size_t n, size_t pos){
    uint8_t k;
    if (pos >= n) return n;
    k = w[pos++];
    switch (k){
        case RAMBLE_NAMED:
            if (pos >= n) return n;
            pos += 1u + w[pos];
            return i_ramble_skip_type(w, n, pos);
        case RAMBLE_STR: return pos + 2u <= n ? pos + 2u : n;
        case RAMBLE_ARR: return i_ramble_skip_type(w, n, pos + 2u <= n ? pos + 2u : n);
        case RAMBLE_VARR: return i_ramble_skip_type(w, n, pos);
        case RAMBLE_VSTR: case RAMBLE_MAP: return pos;
        case RAMBLE_ENUM: {
            uint8_t bs; uint16_t cnt, i;
            if (pos + 3u > n) return n;
            bs = (uint8_t)ramble_schema_scalar_size((RambleSchemaTypeKind)w[pos]);
            cnt = i_ramble_le_r16(w + pos + 1);
            pos += 3u;
            for (i = 0; i < cnt; i++){
                if (pos + bs + 1u > n) return n;
                pos += bs + 1u + w[pos + bs];
            }
            return pos <= n ? pos : n;
        }
        case RAMBLE_STRUCT: {
            uint8_t nf; uint16_t i;
            if (pos >= n) return n;
            nf = w[pos++];
            for (i = 0; i < nf; i++){
                if (pos >= n) return n;
                pos += 1u + w[pos];
                if (pos > n) return n;
                pos = i_ramble_skip_type(w, n, pos);
            }
            return pos;
        }
        default: return pos;
    }
}

/* Spells the type at pos as DSL text. A NAMED type spells as its bare name and the
 * caller hoists its definition. Returns the position past the type. */
static size_t i_ramble_spell_type(i_RambleTextOut *o, const uint8_t *w, size_t n, size_t pos,
                                int indent){
    uint8_t k;
    if (pos >= n) return n;
    k = w[pos++];
    switch (k){
        case RAMBLE_NAMED: {
            uint8_t nl;
            if (pos >= n) return n;
            nl = w[pos++];
            i_ramble_out_raw(o, (const char *)(w + pos), nl);
            return i_ramble_skip_type(w, n, pos + nl);
        }
        case RAMBLE_STR:
            i_ramble_out_str(o, "string<");
            i_ramble_out_i64(o, (int64_t)i_ramble_le_r16(w + pos));
            i_ramble_out_str(o, ">");
            return pos + 2u;
        case RAMBLE_VSTR: i_ramble_out_str(o, "string"); return pos;
        case RAMBLE_MAP:  i_ramble_out_str(o, "map");    return pos;
        case RAMBLE_ARR: {
            uint16_t cnt = i_ramble_le_r16(w + pos);
            size_t after = i_ramble_spell_type(o, w, n, pos + 2u, indent);
            i_ramble_out_str(o, "[");
            i_ramble_out_i64(o, (int64_t)cnt);
            i_ramble_out_str(o, "]");
            return after;
        }
        case RAMBLE_VARR: {
            size_t after = i_ramble_spell_type(o, w, n, pos, indent);
            i_ramble_out_str(o, "[]");
            return after;
        }
        case RAMBLE_ENUM: {
            uint8_t backing; uint16_t cnt, i; uint32_t bs;
            if (pos + 3u > n) return n;
            backing = w[pos]; cnt = i_ramble_le_r16(w + pos + 1); pos += 3u;
            bs = ramble_schema_scalar_size((RambleSchemaTypeKind)backing);
            i_ramble_out_str(o, "enum<");
            i_ramble_out_str(o, i_ramble_why_kind(backing));
            i_ramble_out_str(o, "> { ");
            for (i = 0; i < cnt; i++){
                uint8_t nl;
                if (pos + bs + 1u > n) return n;
                if (i) i_ramble_out_str(o, ", ");
                nl = w[pos + bs];
                i_ramble_out_raw(o, (const char *)(w + pos + bs + 1u), nl);
                i_ramble_out_str(o, " = ");
                i_ramble_out_i64(o, i_ramble_enum_read_val(backing, w + pos));
                pos += bs + 1u + nl;
            }
            i_ramble_out_str(o, " }");
            return pos;
        }
        case RAMBLE_STRUCT: {
            uint8_t nf; uint16_t i;
            if (pos >= n) return n;
            nf = w[pos++];
            i_ramble_out_str(o, "{\n");
            for (i = 0; i < nf; i++){
                uint8_t nl;
                if (pos >= n) return n;
                nl = w[pos++];
                i_ramble_out_indent(o, indent + 1);
                i_ramble_out_raw(o, (const char *)(w + pos), nl);
                pos += nl;
                i_ramble_out_str(o, ": ");
                pos = i_ramble_spell_type(o, w, n, pos, indent + 1);
                i_ramble_out_str(o, ",\n");
            }
            i_ramble_out_indent(o, indent);
            i_ramble_out_str(o, "}");
            return pos;
        }
        default:
            i_ramble_out_str(o, i_ramble_why_kind(k));
            return pos;
    }
}

/* The named types a schema uses, in dependency order and deduped by name, so each
 * prints one leading definition. */
#define I_RAMBLE_MAX_DEFS 48u
typedef struct {
    const uint8_t *w; size_t n;
    struct { uint32_t noff, toff; uint8_t nlen; } d[I_RAMBLE_MAX_DEFS];
    uint16_t count;
} i_RambleDefScan;

static void i_ramble_defscan_add(i_RambleDefScan *L, uint32_t noff, uint8_t nlen, uint32_t toff){
    uint16_t i;
    for (i = 0; i < L->count; i++)
        if (L->d[i].nlen == nlen && memcmp(L->w + L->d[i].noff, L->w + noff, nlen) == 0) return;
    if (L->count >= I_RAMBLE_MAX_DEFS) return;   /* past the cap the tail spells by reference */
    L->d[L->count].noff = noff; L->d[L->count].nlen = nlen; L->d[L->count].toff = toff;
    L->count++;
}

static size_t i_ramble_defscan_type(i_RambleDefScan *L, size_t pos){
    uint8_t k;
    if (pos >= L->n) return L->n;
    k = L->w[pos];
    if (k == RAMBLE_NAMED){
        uint8_t nl; size_t ipos, after;
        if (pos + 2u > L->n) return L->n;
        nl = L->w[pos + 1];
        ipos = pos + 2u + nl;
        if (ipos > L->n) return L->n;
        after = i_ramble_defscan_type(L, ipos);        /* dependencies first */
        i_ramble_defscan_add(L, (uint32_t)(pos + 2u), nl, (uint32_t)ipos);
        return after;
    }
    pos++;
    switch (k){
        case RAMBLE_ARR:  return i_ramble_defscan_type(L, pos + 2u <= L->n ? pos + 2u : L->n);
        case RAMBLE_VARR: return i_ramble_defscan_type(L, pos);
        case RAMBLE_STRUCT: {
            uint8_t nf; uint16_t i;
            if (pos >= L->n) return L->n;
            nf = L->w[pos++];
            for (i = 0; i < nf; i++){
                if (pos >= L->n) return L->n;
                pos += 1u + L->w[pos];
                if (pos > L->n) return L->n;
                pos = i_ramble_defscan_type(L, pos);
            }
            return pos;
        }
        default: return i_ramble_skip_type(L->w, L->n, pos - 1u);
    }
}

uint32_t ramble_schema_print(const RambleSchema *s, char *buf, size_t cap){
    i_RambleTextOut o;
    i_RambleDefScan scan;
    size_t root_type;
    uint16_t i;
    o.p   = (buf && cap) ? buf : NULL;
    o.end = o.p ? buf + cap - 1 : NULL;   /* one byte reserved for the NUL */
    o.n   = 0;
    if (!s){ if (buf && cap) buf[0] = '\0'; return 0; }
    root_type = 2u + s->name.len;
    scan.w = s->wire.data; scan.n = s->wire.len; scan.count = 0;
    i_ramble_defscan_type(&scan, root_type);
    for (i = 0; i < scan.count; i++){                    /* each named type, once, in order */
        i_ramble_out_raw(&o, (const char *)(scan.w + scan.d[i].noff), scan.d[i].nlen);
        i_ramble_out_str(&o, " = ");
        i_ramble_spell_type(&o, scan.w, scan.n, scan.d[i].toff, 0);
        i_ramble_out_str(&o, "\n");
    }
    if (s->value_root){                    /* a bare type, or Name = type for an alias */
        if (s->name.len){
            i_ramble_out_view(&o, s->name);
            i_ramble_out_str(&o, " = ");
        }
        i_ramble_spell_type(&o, scan.w, scan.n, root_type, 0);
        i_ramble_out_str(&o, "\n");
    } else {
        i_ramble_out_view(&o, s->name);
        i_ramble_out_str(&o, " ");
        i_ramble_spell_type(&o, scan.w, scan.n, root_type, 0);
        i_ramble_out_str(&o, "\n");
    }
    if (o.p) *o.p = '\0';                   /* o.p is at most end, the reserved byte */
    else if (buf && cap) buf[0] = '\0';
    return o.n;
}

/* reader and writer compatibility */
/* a top level field by name. A nested type is compared as one exact unit */
static const i_Field *i_ramble_schema_find(const RambleSchema *s, RambleString name){
    uint16_t i;
    for (i = 0; i < s->nfields; i++){
        const i_Field *f = &s->fields[i];
        if (f->depth == 0 && f->name.len == name.len &&
            (name.len == 0 || memcmp(f->name.data, name.data, name.len) == 0)) return f;
    }
    return NULL;
}

/* consumes a NAMED wrapper if present, returning its name, else {NULL,0} */
static RambleString i_ramble_rd_type_name(i_Rd *r){
    RambleString nm = ramble_string(NULL, 0);
    if (r->pos < r->n && r->w[r->pos] == RAMBLE_NAMED){
        uint8_t nl;
        r->pos++;
        nl = i_ramble_rd_u8(r);
        nm = ramble_string((const char *)(r->w + r->pos), nl);
        i_ramble_rd_skip(r, nl);
        if (r->fail) return ramble_string(NULL, 0);
    }
    return nm;
}

static void i_ramble_rd_skip_enum(i_Rd *r, uint8_t backing){
    uint16_t n = i_ramble_rd_u16(r), i;
    uint32_t bs = ramble_schema_scalar_size((RambleSchemaTypeKind)backing);
    for (i = 0; i < n && !r->fail; i++){
        i_ramble_rd_skip(r, bs);
        i_ramble_rd_skip(r, i_ramble_rd_u8(r));
    }
}

/* Can a reader declaring the type at ra read a writer's type at rb. Both advance past
 * their type. Names narrow, everything else compares exactly. See spec/schema.md. */
static int i_ramble_type_cmp(i_Rd *ra, i_Rd *rb, uint16_t depth){
    RambleString na, nb; uint8_t ka, kb;
    if (depth > RAMBLE_SCHEMA_MAX_DEPTH + 1u) return 0;
    na = i_ramble_rd_type_name(ra);
    nb = i_ramble_rd_type_name(rb);
    if (ra->fail || rb->fail) return 0;
    if (na.len && !ramble_string_eq(na, nb)) return 0;
    ka = i_ramble_rd_u8(ra); kb = i_ramble_rd_u8(rb);
    if (ra->fail || rb->fail || ka != kb) return 0;
    switch (ka){
        case RAMBLE_STR: {
            uint16_t ca = i_ramble_rd_u16(ra), cb = i_ramble_rd_u16(rb);
            return !ra->fail && !rb->fail && ca == cb;
        }
        case RAMBLE_ARR: {
            uint16_t ca = i_ramble_rd_u16(ra), cb = i_ramble_rd_u16(rb);
            if (ra->fail || rb->fail || ca != cb) return 0;
            return i_ramble_type_cmp(ra, rb, (uint16_t)(depth + 1));
        }
        case RAMBLE_VARR:
            return i_ramble_type_cmp(ra, rb, (uint16_t)(depth + 1));
        case RAMBLE_VSTR: case RAMBLE_MAP:
            return 1;
        case RAMBLE_ENUM: {                        /* the backing width only, names are advisory */
            uint8_t ba = i_ramble_rd_u8(ra), bb = i_ramble_rd_u8(rb);
            i_ramble_rd_skip_enum(ra, ba);
            i_ramble_rd_skip_enum(rb, bb);
            return !ra->fail && !rb->fail && ba == bb;
        }
        case RAMBLE_STRUCT: {
            uint8_t nfa = i_ramble_rd_u8(ra), nfb = i_ramble_rd_u8(rb); uint16_t i;
            if (ra->fail || rb->fail || nfa != nfb) return 0;
            for (i = 0; i < nfa; i++){
                uint8_t la = i_ramble_rd_u8(ra), lb = i_ramble_rd_u8(rb);
                const char *pa = (const char *)(ra->w + ra->pos);
                const char *pb = (const char *)(rb->w + rb->pos);
                i_ramble_rd_skip(ra, la); i_ramble_rd_skip(rb, lb);
                if (ra->fail || rb->fail || la != lb) return 0;
                if (la && memcmp(pa, pb, la) != 0) return 0;
                if (!i_ramble_type_cmp(ra, rb, (uint16_t)(depth + 1))) return 0;
            }
            return 1;
        }
        default:
            return ramble_schema_scalar_size((RambleSchemaTypeKind)ka) != 0;
    }
}

/* the two fields' types, compared from the wire */
static int i_ramble_field_cmp(const RambleSchema *sa, const i_Field *a,
                            const RambleSchema *sb, const i_Field *b){
    i_Rd ra, rb;
    ra.w = sa->wire.data; ra.n = sa->wire.len; ra.pos = a->type_off; ra.fail = 0;
    rb.w = sb->wire.data; rb.n = sb->wire.len; rb.pos = b->type_off; rb.fail = 0;
    return i_ramble_type_cmp(&ra, &rb, 0);
}

/* bounded appenders for the subset why text. No stdio, and a NULL buffer skips all text */
static char *i_ramble_why_str(char *p, char *end, const char *s){
    if (!p) return NULL;
    while (*s && p < end) *p++ = *s++;
    return p;
}
static char *i_ramble_why_view(char *p, char *end, RambleString s){
    size_t i;
    if (!p) return NULL;
    for (i = 0; i < s.len && p < end; i++) *p++ = s.data[i];
    return p;
}
static char *i_ramble_why_u(char *p, char *end, uint32_t v){
    char tmp[10]; int n = 0;
    if (!p) return NULL;
    do { tmp[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n && p < end) *p++ = tmp[--n];
    return p;
}
/* a field's type in compact DSL form: "Float3[4]", "f32[8]", "string<33>", "map" */
static char *i_ramble_why_type(char *p, char *end, const i_Field *f){
    uint8_t is_arr = (uint8_t)(f->kind == RAMBLE_ARR || f->kind == RAMBLE_VARR);
    if (f->type_name.len) return i_ramble_why_view(p, end, f->type_name);
    if (f->kind == RAMBLE_MAP)  return i_ramble_why_str(p, end, "map");
    if (f->kind == RAMBLE_VSTR) return i_ramble_why_str(p, end, "string");
    if (f->kind == RAMBLE_ENUM){                            /* the width is what matters */
        p = i_ramble_why_str(p, end, "enum<");
        p = i_ramble_why_str(p, end, i_ramble_why_kind(f->elem));
        return i_ramble_why_str(p, end, ">");
    }
    if (is_arr && f->elem_name.len){
        p = i_ramble_why_view(p, end, f->elem_name);
    } else {
        uint8_t elem = is_arr ? f->elem : f->kind;
        if (elem == RAMBLE_STR){
            p = i_ramble_why_str(p, end, "string<");
            p = i_ramble_why_u(p, end, f->str_cap);
            p = i_ramble_why_str(p, end, ">");
        } else {
            p = i_ramble_why_str(p, end, i_ramble_why_kind(elem));
        }
    }
    if (f->kind == RAMBLE_ARR){
        p = i_ramble_why_str(p, end, "[");
        p = i_ramble_why_u(p, end, f->count);
        p = i_ramble_why_str(p, end, "]");
    } else if (f->kind == RAMBLE_VARR){
        p = i_ramble_why_str(p, end, "[]");
    }
    return p;
}
static int i_ramble_why_done(char *buf, char *p){   /* NUL terminate the reason and refuse */
    if (buf) *p = '\0';
    return 0;
}
/* a schema's root in DSL words: a bare type's spelling, or struct 'Name' */
static char *i_ramble_why_root(char *p, char *end, const RambleSchema *s){
    if (s->value_root){
        if (s->name.len){
            p = i_ramble_why_view(p, end, s->name);
            p = i_ramble_why_str(p, end, " = ");
        }
        return i_ramble_why_type(p, end, &s->fields[0]);
    }
    p = i_ramble_why_str(p, end, "struct '");
    p = i_ramble_why_view(p, end, s->name);
    return i_ramble_why_str(p, end, "'");
}

int ramble_schema_subset_why(const RambleSchema *sub, const RambleSchema *pub,
                           char *buf, size_t cap){
    uint16_t i;
    char *p = (buf && cap) ? buf : NULL, *end = p ? buf + cap - 1 : NULL;
    if (p) *p = '\0';
    if (!sub || !pub)
        return i_ramble_why_done(p, i_ramble_why_str(p, end, "schema missing"));
    if (sub->value_root || pub->value_root){        /* a bare root: the two roots are the types */
        int named_ok = !sub->name.len || ramble_string_eq(sub->name, pub->name);
        if (sub->value_root == pub->value_root && named_ok &&
            i_ramble_field_cmp(sub, &sub->fields[0], pub, &pub->fields[0])) return 1;
        p = i_ramble_why_str(p, end, "root: reader ");
        p = i_ramble_why_root(p, end, sub);
        p = i_ramble_why_str(p, end, ", writer ");
        return i_ramble_why_done(buf, i_ramble_why_root(p, end, pub));
    }
    if (sub->name.len != pub->name.len ||
        (sub->name.len && memcmp(sub->name.data, pub->name.data, sub->name.len) != 0)){
        p = i_ramble_why_str(p, end, "reader type '"); p = i_ramble_why_view(p, end, sub->name);
        p = i_ramble_why_str(p, end, "' != writer type '"); p = i_ramble_why_view(p, end, pub->name);
        return i_ramble_why_done(buf, i_ramble_why_str(p, end, "'"));
    }
    for (i = 0; i < sub->nfields; i++){
        const i_Field *a = &sub->fields[i], *b;
        if (a->depth != 0) continue;                          /* members ride their struct */
        b = i_ramble_schema_find(pub, a->name);
        if (!b){
            p = i_ramble_why_str(p, end, "field '"); p = i_ramble_why_view(p, end, a->name);
            return i_ramble_why_done(buf, i_ramble_why_str(p, end, "' missing from writer"));
        }
        if (!i_ramble_field_cmp(sub, a, pub, b)){
            p = i_ramble_why_str(p, end, "field '"); p = i_ramble_why_view(p, end, a->name);
            p = i_ramble_why_str(p, end, "': reader "); p = i_ramble_why_type(p, end, a);
            p = i_ramble_why_str(p, end, ", writer ");
            return i_ramble_why_done(buf, i_ramble_why_type(p, end, b));
        }
    }
    return 1;
}

int ramble_schema_subset(const RambleSchema *sub, const RambleSchema *pub){
    return ramble_schema_subset_why(sub, pub, NULL, 0);
}

/* the flat index just past a field's subtree */
static uint16_t i_ramble_subtree_end(const RambleSchema *s, uint16_t i){
    uint16_t d = s->fields[i].depth, k = (uint16_t)(i + 1u);
    while (k < s->nfields && s->fields[k].depth > d) k++;
    return k;
}

RambleSchema *ramble_schema_rebase(const RambleSchema *sub, const RambleSchema *pub,
                               RambleAllocFn alloc, void *user){
    RambleSchema *r; uint16_t i = 0;
    if (!alloc || !ramble_schema_subset(sub, pub)) return NULL;
    r = ramble_schema_parse(sub->wire.data, sub->wire.len, alloc, user);
    if (!r) return NULL;
    while (i < r->nfields){                                   /* the writer's layout */
        const i_Field *p = i_ramble_schema_find(pub, r->fields[i].name);
        uint16_t re = i_ramble_subtree_end(r, i), j, k;
        if (!p || r->fields[i].depth != 0){ ramble_schema_free(r, alloc, user); return NULL; }
        j = (uint16_t)(p - pub->fields);
        for (k = 0; (uint16_t)(i + k) < re; k++){             /* the subtrees match exactly */
            i_Field *rf = &r->fields[i + k];
            const i_Field *pf;
            if ((uint16_t)(j + k) >= pub->nfields){ ramble_schema_free(r, alloc, user); return NULL; }
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
uint32_t ramble_schema_msg_min(const RambleSchema *s){
    return s ? s->size + 4u * s->n_var : 0;
}

uint32_t ramble_schema_msg_len(const RambleSchema *s, const void *buf, size_t cap){
    const uint8_t *p = (const uint8_t *)buf;
    uint64_t total; uint16_t i;
    if (!s || !p) return 0;
    total = s->size;
    for (i = 0; i < s->n_var; i++){                     /* hop the frames, bounds checked */
        if (total + 4u > cap) return 0;
        total += 4u + (uint64_t)i_ramble_le_r32(p + total);
        if (total > cap) return 0;
    }
    return total <= 0xFFFFFFFFu ? (uint32_t)total : 0;
}

/* the payload view of variable field ordinal ord, {NULL,0} on any bound break */
static RambleBytes i_ramble_schema_frame(const RambleSchema *s, RambleBytes msg, uint16_t ord){
    uint64_t pos = s->size; uint32_t flen; uint16_t i;
    for (i = 0; i <= ord && i < s->n_var; i++){
        if (pos + 4u > msg.len) break;
        flen = i_ramble_le_r32(msg.data + pos);
        if (pos + 4u + flen > msg.len) break;
        if (i == ord) return ramble_bytes(msg.data + pos + 4u, flen);
        pos += 4u + (uint64_t)flen;
    }
    return ramble_bytes(NULL, 0);
}

/* Resizes variable field ord's frame to new_len, moving the rest of the tail. src NULL
 * keeps the existing prefix and zeroes any growth. src must not alias buf. */
static int i_ramble_schema_frame_write(const RambleSchema *s, uint8_t *buf, size_t cap,
                                     uint16_t ord, const void *src, size_t new_len){
    uint64_t pos = s->size, total; uint32_t old; uint16_t i;
    if (new_len && !src && src != NULL) return 0;
    if (new_len > 0xFFFFFFFFu - 4u) return 0;
    total = ramble_schema_msg_len(s, buf, cap);
    if (total == 0) return 0;                           /* a malformed or uninitialized buffer */
    for (i = 0; i < ord; i++) pos += 4u + (uint64_t)i_ramble_le_r32(buf + pos);
    old = i_ramble_le_r32(buf + pos);                   /* bounds proven by msg_len's walk */
    if (total - old + new_len > cap) return 0;          /* never silently truncate */
    memmove(buf + pos + 4u + new_len, buf + pos + 4u + old,
            (size_t)(total - (pos + 4u + old)));
    i_ramble_le_w32(buf + pos, (uint32_t)new_len);
    if (src){ if (new_len) memcpy(buf + pos + 4u, src, new_len); }
    else if (new_len > old) memset(buf + pos + 4u + old, 0, new_len - old);
    return 1;
}
static int i_ramble_schema_set_frame(const RambleSchema *s, uint8_t *buf, size_t cap,
                                   uint16_t ord, const void *src, size_t src_len){
    if (src_len && !src) return 0;
    return i_ramble_schema_frame_write(s, buf, cap, ord, src_len ? src : (const void *)"", src_len);
}

/* reading a message */
int ramble_schema_validate(const RambleSchema *s, RambleBytes msg){
    uint32_t total;
    if (!s) return 0;
    if (s->n_var == 0) return msg.len == s->size;
    total = ramble_schema_msg_len(s, msg.data, msg.len);  /* the frames must consume it exactly */
    return total != 0 && total == msg.len;
}

/* Where field f's bytes sit in msg for array element index, bounds checked. A fixed
 * struct array strides from element 0, a variable one strides inside the array's frame. */
static int i_ramble_field_addr(const RambleSchema *s, RambleBytes msg, const i_Field *f,
                             uint32_t index, size_t *out_off){
    size_t off;
    if (i_ramble_kind_var(f->kind)){ *out_off = 0; return 1; }   /* frames locate themselves */
    if (f->arr_parent == I_RAMBLE_NO_PARENT){
        off = f->offset;
    } else {
        const i_Field *a = &s->fields[f->arr_parent];
        if (a->elem_size == 0) return 0;
        if (a->kind == RAMBLE_ARR){
            if (index >= a->count) return 0;
            off = (size_t)f->offset + (size_t)index * a->elem_size;
        } else {
            RambleBytes fr = i_ramble_schema_frame(s, msg, a->var_ord);
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
static const i_Field *i_ramble_schema_read_lookup(const RambleSchema *s, RambleBytes msg,
                                                const char *field, size_t *off){
    uint32_t index = 0;
    const i_Field *f = i_ramble_schema_field_by_path(s, field, &index);
    if (!f || !i_ramble_field_addr(s, msg, f, index, off)) return NULL;
    return f;
}

static uint64_t i_ramble_schema_read_uint(uint8_t kind, const uint8_t *p){
    switch (kind){
        case RAMBLE_U8: case RAMBLE_BOOL: return p[0];
        case RAMBLE_U16: return i_ramble_le_r16(p);
        case RAMBLE_U32: return i_ramble_le_r32(p);
        case RAMBLE_U64: return i_ramble_le_r64(p);
        default: return 0;
    }
}
static int64_t i_ramble_schema_read_int(uint8_t kind, const uint8_t *p){
    switch (kind){
        case RAMBLE_I8:  return (int8_t)p[0];
        case RAMBLE_I16: return (int16_t)i_ramble_le_r16(p);
        case RAMBLE_I32: return (int32_t)i_ramble_le_r32(p);
        case RAMBLE_I64: return (int64_t)i_ramble_le_r64(p);
        default: return 0;
    }
}
static double i_ramble_schema_read_f64(uint8_t kind, const uint8_t *p){
    if (kind == RAMBLE_F64){ uint64_t b = i_ramble_le_r64(p); double d; memcpy(&d, &b, 8); return d; }
    if (kind == RAMBLE_F32){ uint32_t b = i_ramble_le_r32(p); float  x; memcpy(&x, &b, 4); return (double)x; }
    return 0.0;
}

uint64_t ramble_get_uint(RambleBytes msg, const RambleSchema *s, const char *field){
    size_t off; const i_Field *f = i_ramble_schema_read_lookup(s, msg, field, &off);
    if (!f) return 0;
    if (f->kind == RAMBLE_ENUM) return (uint64_t)i_ramble_enum_read_val(f->elem, msg.data + off);
    return i_ramble_schema_read_uint(f->kind, msg.data + off);
}

int64_t ramble_get_int(RambleBytes msg, const RambleSchema *s, const char *field){
    size_t off; const i_Field *f = i_ramble_schema_read_lookup(s, msg, field, &off);
    if (!f) return 0;
    if (f->kind == RAMBLE_ENUM) return i_ramble_enum_read_val(f->elem, msg.data + off);
    return i_ramble_schema_read_int(f->kind, msg.data + off);
}

double ramble_get_f64(RambleBytes msg, const RambleSchema *s, const char *field){
    size_t off; const i_Field *f = i_ramble_schema_read_lookup(s, msg, field, &off);
    return f ? i_ramble_schema_read_f64(f->kind, msg.data + off) : 0.0;
}

float ramble_get_f32(RambleBytes msg, const RambleSchema *s, const char *field){
    size_t off; const i_Field *f = i_ramble_schema_read_lookup(s, msg, field, &off);
    if (!f || f->kind != RAMBLE_F32) return 0.0f;
    { uint32_t b = i_ramble_le_r32(msg.data + off); float x; memcpy(&x, &b, 4); return x; }
}

RambleBytes ramble_get_array(RambleBytes msg, const RambleSchema *s, const char *field){
    RambleBytes out; size_t off; const i_Field *f = i_ramble_schema_read_lookup(s, msg, field, &off);
    out.data = NULL; out.len = 0;
    if (!f) return out;
    if (f->kind == RAMBLE_VARR){
        RambleBytes fr = i_ramble_schema_frame(s, msg, f->var_ord);
        if (!fr.data || !f->elem_size) return out;
        out.data = fr.data; out.len = fr.len - fr.len % f->elem_size;   /* whole elements only */
        return out;
    }
    if (f->kind != RAMBLE_ARR) return out;
    out.data = msg.data + off; out.len = f->size;
    return out;
}

/* the live element count of an array field */
static uint32_t i_ramble_array_count(const RambleSchema *s, RambleBytes msg, const i_Field *f){
    if (!f || !f->elem_size) return 0;
    if (f->kind == RAMBLE_ARR) return f->count;
    if (f->kind == RAMBLE_VARR){
        RambleBytes fr = i_ramble_schema_frame(s, msg, f->var_ord);
        return fr.data ? (uint32_t)(fr.len / f->elem_size) : 0;
    }
    return 0;
}

uint32_t ramble_get_array_count(RambleBytes msg, const RambleSchema *s, const char *field){
    size_t off; const i_Field *f = i_ramble_schema_read_lookup(s, msg, field, &off);
    return i_ramble_array_count(s, msg, f);
}

uint32_t ramble_array_count_at(RambleBytes msg, const RambleSchema *s, uint16_t field){
    if (!s || field >= s->nfields) return 0;
    return i_ramble_array_count(s, msg, &s->fields[field]);
}

/* one slot's live string, the length clamped to the cap against a hostile message */
static RambleString i_ramble_schema_str_view(const uint8_t *slot, uint16_t cap){
    uint16_t len = i_ramble_le_r16(slot);
    if (len > cap) len = cap;
    return ramble_string((const char *)(slot + 2), len);
}

RambleString ramble_get_string(RambleBytes msg, const RambleSchema *s, const char *field){
    size_t off; const i_Field *f = i_ramble_schema_read_lookup(s, msg, field, &off);
    if (!f) return ramble_string(NULL, 0);
    if (f->kind == RAMBLE_VSTR){
        RambleBytes fr = i_ramble_schema_frame(s, msg, f->var_ord);   /* the frame is the string */
        return ramble_string((const char *)fr.data, fr.data ? fr.len : 0);
    }
    if (f->kind != RAMBLE_STR) return ramble_string(NULL, 0);
    return i_ramble_schema_str_view(msg.data + off, f->str_cap);
}

RambleString ramble_get_string_at(RambleBytes msg, const RambleSchema *s, const char *field,
                              uint16_t index){
    size_t off; const i_Field *f = i_ramble_schema_read_lookup(s, msg, field, &off);
    if (!f || f->elem != RAMBLE_STR) return ramble_string(NULL, 0);
    if (f->kind == RAMBLE_VARR){
        RambleBytes fr = i_ramble_schema_frame(s, msg, f->var_ord);
        uint32_t esz = f->elem_size;
        if (!fr.data || !esz || ((uint64_t)index + 1u) * esz > fr.len) return ramble_string(NULL, 0);
        return i_ramble_schema_str_view(fr.data + (size_t)index * esz, f->str_cap);
    }
    if (f->kind != RAMBLE_ARR || index >= f->count) return ramble_string(NULL, 0);
    return i_ramble_schema_str_view(msg.data + off + (size_t)index * f->elem_size, f->str_cap);
}

RambleBytes ramble_get_map(RambleBytes msg, const RambleSchema *s, const char *field){
    size_t off; const i_Field *f = i_ramble_schema_read_lookup(s, msg, field, &off);
    if (!f || f->kind != RAMBLE_MAP) return ramble_bytes(NULL, 0);
    return i_ramble_schema_frame(s, msg, f->var_ord);
}

RambleString ramble_get_enum(RambleBytes msg, const RambleSchema *s, const char *field){
    size_t off; const i_Field *f = i_ramble_schema_read_lookup(s, msg, field, &off);
    if (!f || f->kind != RAMBLE_ENUM) return ramble_string(NULL, 0);
    return ramble_enum_name_of(s, (uint16_t)(f - s->fields),
                             i_ramble_enum_read_val(f->elem, msg.data + off));
}

int ramble_get_value_at(RambleBytes msg, const RambleSchema *s, uint16_t field, uint32_t elem,
                      RambleValue *out){
    const i_Field *f; const uint8_t *p; size_t off;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!s || field >= s->nfields) return 0;
    f = &s->fields[field];
    if (!i_ramble_field_addr(s, msg, f, elem, &off)) return 0;
    p = msg.data + off;
    out->kind = f->kind; out->elem = f->elem; out->count = f->count; out->str_cap = f->str_cap;
    switch (f->kind){
        case RAMBLE_U8: case RAMBLE_U16: case RAMBLE_U32: case RAMBLE_U64: case RAMBLE_BOOL:
            out->v.u = i_ramble_schema_read_uint(f->kind, p); break;
        case RAMBLE_I8: case RAMBLE_I16: case RAMBLE_I32: case RAMBLE_I64:
            out->v.i = i_ramble_schema_read_int(f->kind, p); break;
        case RAMBLE_F32: case RAMBLE_F64:
            out->v.f = i_ramble_schema_read_f64(f->kind, p); break;
        case RAMBLE_ARR: case RAMBLE_STRUCT:
            out->bytes = ramble_bytes(p, f->size); break;
        case RAMBLE_STR: {
            RambleString sv = i_ramble_schema_str_view(p, f->str_cap);
            out->bytes = ramble_bytes(sv.data, sv.len); break;
        }
        case RAMBLE_VSTR: {
            RambleBytes fr = i_ramble_schema_frame(s, msg, f->var_ord);
            if (!fr.data) return 0;
            out->bytes = fr; break;
        }
        case RAMBLE_VARR: {
            RambleBytes fr = i_ramble_schema_frame(s, msg, f->var_ord);
            uint64_t n;
            if (!fr.data || !f->elem_size) return 0;
            n = fr.len / f->elem_size;
            out->bytes = ramble_bytes(fr.data, (size_t)(n * f->elem_size));
            out->count = n > 0xFFFFu ? 0xFFFFu : (uint16_t)n;   /* saturated, bytes.len rules */
            break;
        }
        case RAMBLE_MAP: {
            RambleBytes fr = i_ramble_schema_frame(s, msg, f->var_ord);
            if (!fr.data) return 0;
            out->bytes = fr;
            out->count = ramble_map_count(fr); break;
        }
        case RAMBLE_ENUM:   /* v.i is the value, elem and count carry the backing and options */
            out->v.i = i_ramble_enum_read_val(f->elem, p); break;
        default: return 0;
    }
    return 1;
}

int ramble_get_value(RambleBytes msg, const RambleSchema *s, uint16_t field, RambleValue *out){
    return ramble_get_value_at(msg, s, field, 0, out);
}

/* writing a message */
int ramble_schema_message_default(const RambleSchema *s, void *buf, size_t cap){
    size_t min;
    if (!s || !buf) return 0;
    min = (size_t)s->size + 4u * s->n_var;
    if (cap < min) return 0;
    memset(buf, 0, min);            /* zeroed fixed fields plus one empty frame per variable */
    return 1;
}

static const i_Field *i_ramble_schema_set_lookup(const RambleSchema *s, const void *buf, size_t cap,
                                               const char *field, size_t *off){
    if (!buf) return NULL;
    return i_ramble_schema_read_lookup(s, ramble_bytes(buf, cap), field, off);
}

static int i_ramble_schema_write_uint(const i_Field *f, uint8_t *p, uint64_t v){
    switch (f->kind){
        case RAMBLE_U8:   p[0] = (uint8_t)v;  return 1;
        case RAMBLE_BOOL: p[0] = v ? 1u : 0u; return 1;
        case RAMBLE_U16: i_ramble_le_w16(p, (uint16_t)v); return 1;
        case RAMBLE_U32: i_ramble_le_w32(p, (uint32_t)v); return 1;
        case RAMBLE_U64: i_ramble_le_w64(p, v); return 1;
        default: return 0;
    }
}
static int i_ramble_schema_write_int(const i_Field *f, uint8_t *p, int64_t v){
    switch (f->kind){
        case RAMBLE_I8:  p[0] = (uint8_t)v; return 1;
        case RAMBLE_I16: i_ramble_le_w16(p, (uint16_t)v); return 1;
        case RAMBLE_I32: i_ramble_le_w32(p, (uint32_t)v); return 1;
        case RAMBLE_I64: i_ramble_le_w64(p, (uint64_t)v); return 1;
        default: return 0;
    }
}
static int i_ramble_schema_write_f64(const i_Field *f, uint8_t *p, double v){
    if (f->kind == RAMBLE_F64){ uint64_t b; memcpy(&b, &v, 8); i_ramble_le_w64(p, b); return 1; }
    if (f->kind == RAMBLE_F32){ float x = (float)v; uint32_t b; memcpy(&b, &x, 4); i_ramble_le_w32(p, b); return 1; }
    return 0;
}
/* one string slot: [u16 len][bytes][zeroed tail]. Refuses v.len over the cap */
static int i_ramble_schema_write_str_slot(uint8_t *p, uint16_t cap, RambleString v){
    if (v.len > cap || (v.len && !v.data)) return 0;             /* never silently truncate */
    i_ramble_le_w16(p, (uint16_t)v.len);
    if (v.len) memcpy(p + 2, v.data, v.len);
    memset(p + 2 + v.len, 0, (size_t)cap - v.len);
    return 1;
}
/* element payload sanity shared by fixed and variable arrays: whole elements, and every
 * string slot's length prefix within its cap */
static int i_ramble_schema_check_elems(const i_Field *f, RambleBytes elems, uint32_t esz){
    if (!esz || elems.len % esz != 0) return 0;     /* never silently truncate */
    if (elems.len && !elems.data) return 0;
    if (f->elem == RAMBLE_STR){
        size_t off;
        for (off = 0; off + esz <= elems.len; off += esz)
            if (i_ramble_le_r16(elems.data + off) > f->str_cap) return 0;
    }
    return 1;
}
/* copy elems over the front of a fixed array and zero the rest */
static int i_ramble_schema_write_array(const i_Field *f, uint8_t *p, RambleBytes elems){
    if (f->kind != RAMBLE_ARR) return 0;
    if (elems.len > f->size || !i_ramble_schema_check_elems(f, elems, f->elem_size)) return 0;
    if (elems.len) memcpy(p, elems.data, elems.len);
    memset(p + elems.len, 0, f->size - elems.len);
    return 1;
}
/* a variable array's frame becomes exactly elems */
static int i_ramble_schema_write_var_array(const RambleSchema *s, uint8_t *buf, size_t cap,
                                         const i_Field *f, RambleBytes elems){
    if (!i_ramble_schema_check_elems(f, elems, f->elem_size)) return 0;
    return i_ramble_schema_set_frame(s, buf, cap, f->var_ord, elems.data, elems.len);
}

int ramble_set_uint(void *buf, size_t cap, const RambleSchema *s, const char *field, uint64_t v){
    size_t off; const i_Field *f = i_ramble_schema_set_lookup(s, buf, cap, field, &off);
    if (!f) return 0;
    if (f->kind == RAMBLE_ENUM){ i_ramble_enum_write_val(f->elem, (uint8_t *)buf + off, (int64_t)v); return 1; }
    return i_ramble_schema_write_uint(f, (uint8_t *)buf + off, v);
}

int ramble_set_int(void *buf, size_t cap, const RambleSchema *s, const char *field, int64_t v){
    size_t off; const i_Field *f = i_ramble_schema_set_lookup(s, buf, cap, field, &off);
    if (!f) return 0;
    if (f->kind == RAMBLE_ENUM){ i_ramble_enum_write_val(f->elem, (uint8_t *)buf + off, v); return 1; }
    return i_ramble_schema_write_int(f, (uint8_t *)buf + off, v);
}

int ramble_set_f64(void *buf, size_t cap, const RambleSchema *s, const char *field, double v){
    size_t off; const i_Field *f = i_ramble_schema_set_lookup(s, buf, cap, field, &off);
    return f ? i_ramble_schema_write_f64(f, (uint8_t *)buf + off, v) : 0;
}

int ramble_set_f32(void *buf, size_t cap, const RambleSchema *s, const char *field, float v){
    size_t off; const i_Field *f = i_ramble_schema_set_lookup(s, buf, cap, field, &off); uint32_t b;
    if (!f || f->kind != RAMBLE_F32) return 0;
    memcpy(&b, &v, 4); i_ramble_le_w32((uint8_t *)buf + off, b);
    return 1;
}

int ramble_set_array(void *buf, size_t cap, const RambleSchema *s, const char *field, RambleBytes elems){
    size_t off; const i_Field *f = i_ramble_schema_set_lookup(s, buf, cap, field, &off);
    if (!f) return 0;
    if (f->kind == RAMBLE_VARR)
        return i_ramble_schema_write_var_array(s, (uint8_t *)buf, cap, f, elems);
    return i_ramble_schema_write_array(f, (uint8_t *)buf + off, elems);
}

int ramble_set_array_count(void *buf, size_t cap, const RambleSchema *s, const char *field,
                         uint32_t count){
    size_t off; const i_Field *f = i_ramble_schema_set_lookup(s, buf, cap, field, &off);
    if (!f || f->kind != RAMBLE_VARR || !f->elem_size) return 0;
    if ((uint64_t)count * f->elem_size > 0xFFFFFFFFu) return 0;
    return i_ramble_schema_frame_write(s, (uint8_t *)buf, cap, f->var_ord, NULL,
                                     (size_t)count * f->elem_size);
}

int ramble_set_string(void *buf, size_t cap, const RambleSchema *s, const char *field, RambleString v){
    size_t off; const i_Field *f = i_ramble_schema_set_lookup(s, buf, cap, field, &off);
    if (!f) return 0;
    if (f->kind == RAMBLE_VSTR){
        if (v.len && !v.data) return 0;
        return i_ramble_schema_set_frame(s, (uint8_t *)buf, cap, f->var_ord, v.data, v.len);
    }
    if (f->kind != RAMBLE_STR) return 0;
    return i_ramble_schema_write_str_slot((uint8_t *)buf + off, f->str_cap, v);
}

int ramble_set_string_at(void *buf, size_t cap, const RambleSchema *s, const char *field,
                       uint16_t index, RambleString v){
    size_t off; const i_Field *f = i_ramble_schema_set_lookup(s, buf, cap, field, &off);
    if (!f || f->elem != RAMBLE_STR) return 0;
    if (f->kind == RAMBLE_VARR){                      /* in place, under the live count */
        RambleBytes fr = i_ramble_schema_frame(s, ramble_bytes(buf, cap), f->var_ord);
        uint32_t esz = f->elem_size;
        if (!fr.data || !esz || ((uint64_t)index + 1u) * esz > fr.len) return 0;
        return i_ramble_schema_write_str_slot((uint8_t *)fr.data + (size_t)index * esz,
                                            f->str_cap, v);
    }
    if (f->kind != RAMBLE_ARR || index >= f->count) return 0;
    return i_ramble_schema_write_str_slot((uint8_t *)buf + off + (size_t)index * f->elem_size,
                                        f->str_cap, v);
}

int ramble_set_map(void *buf, size_t cap, const RambleSchema *s, const char *field, RambleBytes map){
    size_t off; const i_Field *f = i_ramble_schema_set_lookup(s, buf, cap, field, &off);
    if (!f || f->kind != RAMBLE_MAP) return 0;
    if (!ramble_map_valid(map)) return 0;             /* malformed bytes never enter a message */
    return i_ramble_schema_set_frame(s, (uint8_t *)buf, cap, f->var_ord, map.data, map.len);
}

int ramble_set_enum(void *buf, size_t cap, const RambleSchema *s, const char *field, const char *name){
    size_t off; const i_Field *f = i_ramble_schema_set_lookup(s, buf, cap, field, &off);
    int64_t v;
    if (!f || f->kind != RAMBLE_ENUM) return 0;
    if (!ramble_enum_value_of(s, (uint16_t)(f - s->fields), name, &v)) return 0;
    i_ramble_enum_write_val(f->elem, (uint8_t *)buf + off, v);
    return 1;
}

int ramble_set_value_at(void *buf, size_t cap, const RambleSchema *s, uint16_t field, uint32_t elem,
                      const RambleValue *val){
    const i_Field *f; uint8_t *p; size_t off;
    if (!buf || !val || !s || field >= s->nfields) return 0;
    f = &s->fields[field];
    if (!i_ramble_field_addr(s, ramble_bytes(buf, cap), f, elem, &off)) return 0;
    p = (uint8_t *)buf + off;
    switch (f->kind){
        case RAMBLE_U8: case RAMBLE_U16: case RAMBLE_U32: case RAMBLE_U64: case RAMBLE_BOOL:
            return i_ramble_schema_write_uint(f, p, val->v.u);
        case RAMBLE_I8: case RAMBLE_I16: case RAMBLE_I32: case RAMBLE_I64:
            return i_ramble_schema_write_int(f, p, val->v.i);
        case RAMBLE_F32: case RAMBLE_F64:
            return i_ramble_schema_write_f64(f, p, val->v.f);
        case RAMBLE_ARR:
            return i_ramble_schema_write_array(f, p, val->bytes);
        case RAMBLE_STR:
            return i_ramble_schema_write_str_slot(p, f->str_cap,
                       ramble_string((const char *)val->bytes.data, val->bytes.len));
        case RAMBLE_STRUCT:   /* raw bytes, a short source zero fills the tail */
            if (val->bytes.len > f->size || (val->bytes.len && !val->bytes.data)) return 0;
            if (val->bytes.len) memcpy(p, val->bytes.data, val->bytes.len);
            memset(p + val->bytes.len, 0, f->size - val->bytes.len);
            return 1;
        case RAMBLE_VSTR:
            if (val->bytes.len && !val->bytes.data) return 0;
            return i_ramble_schema_set_frame(s, (uint8_t *)buf, cap, f->var_ord,
                                           val->bytes.data, val->bytes.len);
        case RAMBLE_VARR:
            return i_ramble_schema_write_var_array(s, (uint8_t *)buf, cap, f, val->bytes);
        case RAMBLE_MAP:
            if (!ramble_map_valid(val->bytes)) return 0;
            return i_ramble_schema_set_frame(s, (uint8_t *)buf, cap, f->var_ord,
                                           val->bytes.data, val->bytes.len);
        case RAMBLE_ENUM:
            i_ramble_enum_write_val(f->elem, p, val->v.i);
            return 1;
        default: return 0;
    }
}

int ramble_set_value(void *buf, size_t cap, const RambleSchema *s, uint16_t field, const RambleValue *val){
    return ramble_set_value_at(buf, cap, s, field, 0, val);
}
/* The map. Readers walk hostile bytes: every read is bounds checked, the kind vocabulary
 * is closed and nesting is depth capped. The body grammar is in spec/schema.md. */

/* Reads (out set) or skips (out NULL) one value at r->pos. 1, or 0 with r->fail. */
static int i_ramble_map_value(i_Rd *r, uint16_t depth, RambleValue *out){
    uint8_t k = i_ramble_rd_u8(r);
    uint32_t sz;
    if (r->fail) return 0;
    if (out){ memset(out, 0, sizeof *out); out->kind = k; }
    sz = ramble_schema_scalar_size((RambleSchemaTypeKind)k);
    if (sz){
        const uint8_t *p = r->w + r->pos;
        i_ramble_rd_skip(r, sz);
        if (r->fail) return 0;
        if (out) switch (k){
            case RAMBLE_U8: case RAMBLE_U16: case RAMBLE_U32: case RAMBLE_U64: case RAMBLE_BOOL:
                out->v.u = i_ramble_schema_read_uint(k, p); break;
            case RAMBLE_I8: case RAMBLE_I16: case RAMBLE_I32: case RAMBLE_I64:
                out->v.i = i_ramble_schema_read_int(k, p); break;
            default:
                out->v.f = i_ramble_schema_read_f64(k, p); break;
        }
        return 1;
    }
    if (k == RAMBLE_VSTR){
        uint16_t len = i_ramble_rd_u16(r);
        const uint8_t *p = r->w + r->pos;
        i_ramble_rd_skip(r, len);
        if (r->fail) return 0;
        if (out) out->bytes = ramble_bytes(p, len);
        return 1;
    }
    if (k == RAMBLE_MAP || k == RAMBLE_VARR){
        size_t start = r->pos; uint16_t n, i;
        if (depth + 1u >= RAMBLE_SCHEMA_MAX_DEPTH){ r->fail = 1; return 0; }
        n = i_ramble_rd_u16(r);
        for (i = 0; i < n && !r->fail; i++){
            if (k == RAMBLE_MAP){
                uint8_t kl = i_ramble_rd_u8(r);
                i_ramble_rd_skip(r, kl);
            }
            if (!i_ramble_map_value(r, (uint16_t)(depth + 1), NULL)) return 0;
        }
        if (r->fail) return 0;
        if (out){ out->bytes = ramble_bytes(r->w + start, r->pos - start); out->count = n; }
        return 1;
    }
    r->fail = 1;                                        /* unknown kind: reject */
    return 0;
}

uint16_t ramble_map_count(RambleBytes map){
    return (map.data && map.len >= 2) ? i_ramble_le_r16(map.data) : 0;
}
uint16_t ramble_map_array_count(RambleBytes arr){ return ramble_map_count(arr); }

int ramble_map_at(RambleBytes map, uint16_t index, RambleString *key, RambleValue *out){
    i_Rd r; uint16_t n, i;
    if (!map.data || map.len < 2) return 0;
    r.w = map.data; r.n = map.len; r.pos = 0; r.fail = 0;
    n = i_ramble_rd_u16(&r);
    if (index >= n) return 0;
    for (i = 0; i <= index; i++){
        uint8_t kl = i_ramble_rd_u8(&r);
        const char *kp = (const char *)(r.w + r.pos);
        i_ramble_rd_skip(&r, kl);
        if (r.fail) return 0;
        if (i == index){
            if (!i_ramble_map_value(&r, 0, out)) return 0;
            if (key) *key = ramble_string(kp, kl);
            return 1;
        }
        if (!i_ramble_map_value(&r, 0, NULL)) return 0;
    }
    return 0;
}

int ramble_map_get(RambleBytes map, const char *key, RambleValue *out){
    i_Rd r; uint16_t n, i; size_t want;
    if (!map.data || map.len < 2 || !key) return 0;
    want = strlen(key);
    r.w = map.data; r.n = map.len; r.pos = 0; r.fail = 0;
    n = i_ramble_rd_u16(&r);
    for (i = 0; i < n; i++){
        uint8_t kl = i_ramble_rd_u8(&r);
        const char *kp = (const char *)(r.w + r.pos);
        i_ramble_rd_skip(&r, kl);
        if (r.fail) return 0;
        if (kl == want && (want == 0 || memcmp(kp, key, want) == 0))
            return i_ramble_map_value(&r, 0, out);
        if (!i_ramble_map_value(&r, 0, NULL)) return 0;
    }
    return 0;
}

int ramble_map_array_at(RambleBytes arr, uint16_t index, RambleValue *out){
    i_Rd r; uint16_t n, i;
    if (!arr.data || arr.len < 2) return 0;
    r.w = arr.data; r.n = arr.len; r.pos = 0; r.fail = 0;
    n = i_ramble_rd_u16(&r);
    if (index >= n) return 0;
    for (i = 0; i < index; i++)
        if (!i_ramble_map_value(&r, 0, NULL)) return 0;
    return i_ramble_map_value(&r, 0, out);
}

int ramble_map_valid(RambleBytes map){
    i_Rd r; uint16_t n, i;
    if (map.len == 0) return 1;                         /* an empty body is an empty map */
    if (!map.data || map.len < 2) return 0;
    r.w = map.data; r.n = map.len; r.pos = 0; r.fail = 0;
    n = i_ramble_rd_u16(&r);
    for (i = 0; i < n; i++){
        uint8_t kl = i_ramble_rd_u8(&r);
        i_ramble_rd_skip(&r, kl);
        if (r.fail || !i_ramble_map_value(&r, 0, NULL)) return 0;
    }
    return r.pos == r.n;                                /* no trailing garbage */
}

/* the map writer */
RambleMapWriter ramble_map_begin(void *buf, size_t cap){
    RambleMapWriter w;
    memset(&w, 0, sizeof w);
    w.buf = (uint8_t *)buf; w.cap = cap;
    if (!buf || cap < 2){ w.err = -1; return w; }
    w.buf[0] = 0; w.buf[1] = 0;                         /* the root map's count */
    w.count_pos[0] = 0; w.len = 2;
    w.depth = 1;
    return w;
}

/* room for extra more bytes. The buffer is the caller's, so no growth */
static int i_ramble_map_room(RambleMapWriter *w, size_t extra){
    if (w->err) return 0;
    if (w->len + extra > w->cap){ w->err = -1; return 0; }
    return 1;
}
/* start an entry: a key in a map, none in an array. Bumps the open count */
static int i_ramble_map_entry(RambleMapWriter *w, const char *key){
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
        if (!i_ramble_map_room(w, 1 + kl)) return 0;
        w->buf[w->len++] = (uint8_t)kl;
        if (kl) memcpy(w->buf + w->len, key, kl);
        w->len += kl;
    }
    w->count[w->depth - 1]++;
    return 1;
}
static int i_ramble_map_put_scalar(RambleMapWriter *w, const char *key, uint8_t kind,
                                 const uint8_t *le, uint32_t sz){
    if (!w || !i_ramble_map_entry(w, key) || !i_ramble_map_room(w, 1u + sz)) return 0;
    w->buf[w->len++] = kind;
    memcpy(w->buf + w->len, le, sz);
    w->len += sz;
    return 1;
}

int ramble_map_put_uint(RambleMapWriter *w, const char *key, uint64_t v){
    uint8_t le[8];                                       /* the smallest kind that fits */
    uint8_t k = v <= 0xFFu ? RAMBLE_U8 : v <= 0xFFFFu ? RAMBLE_U16
              : v <= 0xFFFFFFFFu ? RAMBLE_U32 : RAMBLE_U64;
    i_ramble_le_w64(le, v);
    return i_ramble_map_put_scalar(w, key, k, le, ramble_schema_scalar_size((RambleSchemaTypeKind)k));
}

int ramble_map_put_int(RambleMapWriter *w, const char *key, int64_t v){
    uint8_t le[8];
    uint8_t k = (v >= -128 && v <= 127) ? RAMBLE_I8
              : (v >= -32768 && v <= 32767) ? RAMBLE_I16
              : (v >= -2147483647 - 1 && v <= 2147483647) ? RAMBLE_I32 : RAMBLE_I64;
    i_ramble_le_w64(le, (uint64_t)v);                      /* two's complement LE: the low bytes */
    return i_ramble_map_put_scalar(w, key, k, le, ramble_schema_scalar_size((RambleSchemaTypeKind)k));
}

int ramble_map_put_f64(RambleMapWriter *w, const char *key, double v){
    uint8_t le[8]; uint64_t b;
    memcpy(&b, &v, 8); i_ramble_le_w64(le, b);
    return i_ramble_map_put_scalar(w, key, (uint8_t)RAMBLE_F64, le, 8);
}

int ramble_map_put_f32(RambleMapWriter *w, const char *key, float v){
    uint8_t le[4]; uint32_t b;
    memcpy(&b, &v, 4); i_ramble_le_w32(le, b);
    return i_ramble_map_put_scalar(w, key, (uint8_t)RAMBLE_F32, le, 4);
}

int ramble_map_put_bool(RambleMapWriter *w, const char *key, int v){
    uint8_t b = v ? 1u : 0u;
    return i_ramble_map_put_scalar(w, key, (uint8_t)RAMBLE_BOOL, &b, 1);
}

int ramble_map_put_string(RambleMapWriter *w, const char *key, RambleString v){
    if (!w) return 0;
    if (v.len > 0xFFFFu || (v.len && !v.data)){ w->err = -4; return 0; }
    if (!i_ramble_map_entry(w, key) || !i_ramble_map_room(w, 3u + v.len)) return 0;
    w->buf[w->len++] = (uint8_t)RAMBLE_VSTR;
    i_ramble_le_w16(w->buf + w->len, (uint16_t)v.len); w->len += 2;
    if (v.len) memcpy(w->buf + w->len, v.data, v.len);
    w->len += v.len;
    return 1;
}

static int i_ramble_map_open(RambleMapWriter *w, const char *key, uint8_t kind, uint8_t is_arr){
    if (!w || !i_ramble_map_entry(w, key)) return 0;
    if (w->depth >= RAMBLE_SCHEMA_MAX_DEPTH){ w->err = -2; return 0; }
    if (!i_ramble_map_room(w, 3)) return 0;
    w->buf[w->len++] = kind;
    w->count_pos[w->depth] = w->len;                    /* the nested body's count */
    w->buf[w->len++] = 0; w->buf[w->len++] = 0;
    w->count[w->depth] = 0; w->is_arr[w->depth] = is_arr;
    w->depth++;
    return 1;
}
int ramble_map_open_map(RambleMapWriter *w, const char *key){
    return i_ramble_map_open(w, key, (uint8_t)RAMBLE_MAP, 0);
}
int ramble_map_open_array(RambleMapWriter *w, const char *key){
    return i_ramble_map_open(w, key, (uint8_t)RAMBLE_VARR, 1);
}
int ramble_map_close(RambleMapWriter *w){
    if (!w || w->err) return 0;
    if (w->depth <= 1){ w->err = -3; return 0; }        /* the root closes in finish */
    w->depth--;
    i_ramble_le_w16(w->buf + w->count_pos[w->depth], w->count[w->depth]);
    return 1;
}

uint32_t ramble_map_finish(RambleMapWriter *w){
    if (!w || w->err || w->depth != 1) return 0;        /* else an unbalanced open and close */
    i_ramble_le_w16(w->buf + w->count_pos[0], w->count[0]);
    w->depth = 0;
    return (uint32_t)w->len;
}
/* The schema DSL. The grammar is in spec/schema.md. A type word that is not a built in
 * resolves against the definitions, the environment and the standard library. */
#ifndef RAMBLE_NO_STDTYPES
const char *i_ramble_std_lookup(const char *name);   /* serialize/stdtypes.c */
#else
#define i_ramble_std_lookup(name) ((const char *)0)
#endif

typedef struct { const char *p; const char *err; } i_RambleDsl;

/* The definition registry: one arena holding each named type's name and compiled type,
 * plus an index. A definition compiles in its own scratch builder before it is appended. */
#define I_RAMBLE_DSL_MAX_DEFS 64u
typedef struct {
    RambleSchemaBuilder arena;
    struct { uint32_t noff, toff, tlen; uint8_t nlen; } e[I_RAMBLE_DSL_MAX_DEFS];
    uint16_t n;
    uint16_t rec;                                  /* standard type expansion depth */
    const RambleSchema *const *env; size_t n_env;
} i_RambleDefs;

static void i_ramble_dsl_ws(i_RambleDsl *d){
    for (;;){
        while (*d->p==' ' || *d->p=='\t' || *d->p=='\r' || *d->p=='\n') d->p++;
        if (d->p[0]=='-' && d->p[1]=='-'){ while (*d->p && *d->p!='\n') d->p++; continue; }
        return;
    }
}
static void i_ramble_dsl_fail(i_RambleDsl *d, const char *at){ if (!d->err) d->err = at; }
/* an identifier into out[256], NUL terminated. 0 and err when missing or overlong */
static int i_ramble_dsl_ident(i_RambleDsl *d, char out[256]){
    const char *q = d->p; size_t n;
    if (!((*q>='A'&&*q<='Z') || (*q>='a'&&*q<='z') || *q=='_')){ i_ramble_dsl_fail(d, q); return 0; }
    while ((*q>='A'&&*q<='Z') || (*q>='a'&&*q<='z') || (*q>='0'&&*q<='9') || *q=='_') q++;
    n = (size_t)(q - d->p);
    if (n > 255){ i_ramble_dsl_fail(d, d->p); return 0; }
    memcpy(out, d->p, n); out[n] = '\0';
    d->p = q;
    return 1;
}
static int i_ramble_dsl_expect(i_RambleDsl *d, char c){
    if (*d->p == c){ d->p++; return 1; }
    i_ramble_dsl_fail(d, d->p);
    return 0;
}
/* a decimal array count, 1 to 65535 */
static int i_ramble_dsl_count(i_RambleDsl *d, uint16_t *out){
    const char *at = d->p; uint32_t v = 0;
    while (*d->p>='0' && *d->p<='9'){
        v = v*10u + (uint32_t)(*d->p - '0');
        if (v > 0xFFFFu){ i_ramble_dsl_fail(d, at); return 0; }
        d->p++;
    }
    if (d->p == at || v == 0){ i_ramble_dsl_fail(d, at); return 0; }
    *out = (uint16_t)v;
    return 1;
}
static int i_ramble_dsl_kind(const char *s, RambleSchemaTypeKind *k){
    static const struct { const char *word; uint8_t kind; } table[] = {
        {"u8",RAMBLE_U8},{"u16",RAMBLE_U16},{"u32",RAMBLE_U32},{"u64",RAMBLE_U64},
        {"i8",RAMBLE_I8},{"i16",RAMBLE_I16},{"i32",RAMBLE_I32},{"i64",RAMBLE_I64},
        {"f32",RAMBLE_F32},{"f64",RAMBLE_F64},{"bool",RAMBLE_BOOL} };
    size_t i;
    for (i = 0; i < sizeof table / sizeof table[0]; i++)
        if (strcmp(s, table[i].word) == 0){ *k = (RambleSchemaTypeKind)table[i].kind; return 1; }
    return 0;
}
/* a signed decimal enum value fitting int64 */
static int i_ramble_dsl_enum_value(i_RambleDsl *d, int64_t *out){
    const char *at = d->p; int neg = 0, any = 0; uint64_t v = 0, lim;
    if (*d->p == '-'){ neg = 1; d->p++; }
    lim = neg ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    while (*d->p >= '0' && *d->p <= '9'){
        v = v * 10u + (uint64_t)(*d->p - '0');
        if (v > lim){ i_ramble_dsl_fail(d, at); return 0; }
        d->p++; any = 1;
    }
    if (!any){ i_ramble_dsl_fail(d, at); return 0; }
    *out = neg ? -(int64_t)v : (int64_t)v;
    return 1;
}
/* enum<uN> { Name [= value], ... } after the word enum was read. Streams the options. */
static void i_ramble_dsl_enum(i_RambleDsl *d, RambleSchemaBuilder *b, const char *name){
    char wname[256]; RambleSchemaTypeKind backing; size_t count_pos; uint16_t count = 0; int64_t next = 0;
    i_ramble_dsl_ws(d);
    if (!i_ramble_dsl_expect(d, '<')) return;
    i_ramble_dsl_ws(d);
    if (!i_ramble_dsl_ident(d, wname)) return;
    if (!i_ramble_dsl_kind(wname, &backing) || !i_ramble_enum_backing_ok((uint8_t)backing)){
        i_ramble_dsl_fail(d, d->p); return;                /* the backing must be an integer kind */
    }
    i_ramble_dsl_ws(d);
    if (!i_ramble_dsl_expect(d, '>')) return;
    i_ramble_dsl_ws(d);
    if (!i_ramble_dsl_expect(d, '{')) return;
    count_pos = i_ramble_schema_field_enum_open(b, name, backing);
    for (;;){
        char vname[256]; int64_t v;
        i_ramble_dsl_ws(d);
        if (*d->p == '}' || *d->p == '\0' || d->err || b->err) break;
        if (!i_ramble_dsl_ident(d, vname)) return;
        i_ramble_dsl_ws(d);
        if (*d->p == '='){   /* an explicit value, else auto increment */
            d->p++; i_ramble_dsl_ws(d);
            if (!i_ramble_dsl_enum_value(d, &v)) return;
        } else v = next;
        if (!i_ramble_enum_val_fits((uint8_t)backing, v)){ i_ramble_dsl_fail(d, d->p); return; }
        i_ramble_schema_field_enum_add(b, backing, v, vname, strlen(vname));
        count++; next = v + 1;
        i_ramble_dsl_ws(d);
        if (*d->p == ',') d->p++;                        /* an optional separator */
    }
    if (!i_ramble_dsl_expect(d, '}')) return;
    i_ramble_schema_field_enum_finish(b, count_pos, count);
}

static void i_ramble_dsl_field_type(i_RambleDsl *d, RambleSchemaBuilder *b, i_RambleDefs *defs,
                                  const char *name);
static void i_ramble_dsl_fields(i_RambleDsl *d, RambleSchemaBuilder *b, i_RambleDefs *defs);

/* records a named type: its name bytes then its compiled type, both in the arena */
static int i_ramble_defs_add_bytes(i_RambleDefs *defs, const char *name, size_t nlen,
                                 const uint8_t *type, size_t tlen){
    uint32_t noff, toff;
    if (defs->n >= I_RAMBLE_DSL_MAX_DEFS || nlen == 0 || nlen > 255 || tlen == 0) return 0;
    noff = (uint32_t)defs->arena.len;
    i_ramble_schema_builder_put_raw(&defs->arena, name, nlen);
    toff = (uint32_t)defs->arena.len;
    i_ramble_schema_builder_put_raw(&defs->arena, type, tlen);
    if (defs->arena.err) return 0;
    defs->e[defs->n].noff = noff; defs->e[defs->n].nlen = (uint8_t)nlen;
    defs->e[defs->n].toff = toff; defs->e[defs->n].tlen = (uint32_t)tlen;
    defs->n++;
    return 1;
}

static int i_ramble_defs_find(i_RambleDefs *defs, const char *name, uint32_t *toff, uint32_t *tlen){
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
static int i_ramble_defs_add_text(i_RambleDefs *defs, const char *name, const char *text){
    RambleSchemaBuilder sb; i_RambleDsl sd; int ok = 0;
    if (defs->rec >= 8u) return 0;                       /* the roster is acyclic, but be sure */
    defs->rec++;
    sb = i_ramble_schema_begin_raw(defs->arena.alloc, defs->arena.user);
    sd.p = text; sd.err = NULL;
    i_ramble_dsl_field_type(&sd, &sb, defs, "");
    i_ramble_dsl_ws(&sd);
    if (*sd.p) i_ramble_dsl_fail(&sd, sd.p);
    if (!sd.err && !sb.err && sb.len)
        ok = i_ramble_defs_add_bytes(defs, name, strlen(name), sb.buf, sb.len);
    if (sb.buf) sb.alloc(sb.user, sb.buf, 0);
    defs->rec--;
    return ok;
}

/* Resolves a type name to its encoding in the arena: the text's own definitions first,
 * then the environment schemas, then the standard library. */
static int i_ramble_dsl_ref(i_RambleDefs *defs, const char *name, uint32_t *toff, uint32_t *tlen){
    size_t nlen = strlen(name), i;
    const char *std;
    if (i_ramble_defs_find(defs, name, toff, tlen)) return 1;
    for (i = 0; i < defs->n_env; i++){
        const RambleSchema *t = defs->env ? defs->env[i] : NULL;
        const uint8_t *tb; size_t tl;
        if (!t || t->name.len != nlen || memcmp(t->name.data, name, nlen) != 0) continue;
        if (!i_ramble_schema_root_type(t, &tb, &tl)) continue;
        if (!i_ramble_defs_add_bytes(defs, name, nlen, tb, tl)) return 0;
        return i_ramble_defs_find(defs, name, toff, tlen);
    }
    std = i_ramble_std_lookup(name);
    if (std && i_ramble_defs_add_text(defs, name, std))
        return i_ramble_defs_find(defs, name, toff, tlen);
    return 0;
}

/* turns the type just written at b->buf[head..len) into an array of itself by splicing
 * the array head in front of it, since the count follows the body in the text */
static void i_ramble_dsl_splice_array(RambleSchemaBuilder *b, size_t head, uint16_t count,
                                    int variable){
    size_t extra = variable ? 1u : 3u, tail;
    if (b->err) return;
    if (!i_ramble_schema_builder_reserve(b, extra)) return;
    tail = b->len - head;
    memmove(b->buf + head + extra, b->buf + head, tail);
    if (variable){
        b->buf[head] = (uint8_t)RAMBLE_VARR;
    } else {
        b->buf[head] = (uint8_t)RAMBLE_ARR;
        i_ramble_le_w16(b->buf + head + 1u, count);
    }
    b->len += extra;
}

/* an optional [N] or [] suffix. 1 if one was read, variable for the [] form */
static int i_ramble_dsl_suffix(i_RambleDsl *d, uint16_t *count, int *variable){
    *count = 0; *variable = 0;
    i_ramble_dsl_ws(d);
    if (*d->p != '[') return 0;
    d->p++;
    i_ramble_dsl_ws(d);
    if (*d->p == ']'){ d->p++; *variable = 1; return 1; }
    if (!i_ramble_dsl_count(d, count)) return 0;
    i_ramble_dsl_ws(d);
    if (!i_ramble_dsl_expect(d, ']')) return 0;
    return 1;
}

/* Name, Name[N] or Name[]: a reference to an already resolved named type */
static void i_ramble_dsl_emit_ref(i_RambleDsl *d, RambleSchemaBuilder *b, i_RambleDefs *defs,
                                const char *name, const char *tname,
                                uint32_t toff, uint32_t tlen){
    uint16_t cnt; int variable, arr;
    size_t nlen = strlen(tname);
    arr = i_ramble_dsl_suffix(d, &cnt, &variable);
    if (d->err) return;
    if (arr && variable && !i_ramble_schema_builder_var_ok(b)) return;
    i_ramble_schema_builder_count(b);
    i_ramble_schema_builder_put_name(b, name);
    if (arr){
        if (variable) i_ramble_schema_builder_put(b, (uint8_t)RAMBLE_VARR);
        else { i_ramble_schema_builder_put(b, (uint8_t)RAMBLE_ARR); i_ramble_schema_builder_put_u16(b, cnt); }
    }
    i_ramble_schema_builder_put(b, (uint8_t)RAMBLE_NAMED);
    i_ramble_schema_builder_put(b, (uint8_t)nlen);
    i_ramble_schema_builder_put_raw(b, tname, nlen);
    i_ramble_schema_builder_put_raw(b, defs->arena.buf + toff, tlen);
}

/* One type whose leading word is already in tname, with at pointing at it for errors:
 * whatever follows plus the field it defines. A bare root passes name "". */
static void i_ramble_dsl_word_type(i_RambleDsl *d, RambleSchemaBuilder *b, i_RambleDefs *defs,
                                 const char *name, const char *tname, const char *at){
    RambleSchemaTypeKind k = RAMBLE_U8;
    int is_str = 0, has_cap = 0; uint16_t str_cap = 0;
    uint16_t cnt; int variable;
    if (strcmp(tname, "map") == 0){
        ramble_schema_field_map(b, name);
        return;
    }
    if (strcmp(tname, "enum") == 0){
        i_ramble_dsl_enum(d, b, name);
        return;
    }
    if (strcmp(tname, "string") == 0){                   /* string or string<cap> */
        is_str = 1;
        i_ramble_dsl_ws(d);
        if (*d->p == '<'){
            d->p++; has_cap = 1;
            i_ramble_dsl_ws(d);
            if (!i_ramble_dsl_count(d, &str_cap)) return;
            i_ramble_dsl_ws(d);
            if (!i_ramble_dsl_expect(d, '>')) return;
        }
    } else if (!i_ramble_dsl_kind(tname, &k)){             /* a named type */
        uint32_t toff, tlen;
        if (!i_ramble_dsl_ref(defs, tname, &toff, &tlen)){ i_ramble_dsl_fail(d, at); return; }
        i_ramble_dsl_emit_ref(d, b, defs, name, tname, toff, tlen);
        return;
    }
    if (i_ramble_dsl_suffix(d, &cnt, &variable)){
        if (d->err) return;
        if (is_str && !has_cap){ i_ramble_dsl_fail(d, at); return; }  /* string[] is ragged */
        if (variable){
            if (is_str) ramble_schema_field_var_string_array(b, name, str_cap);
            else        ramble_schema_field_var_array(b, name, k);
        } else {
            if (is_str) ramble_schema_field_string_array(b, name, str_cap, cnt);
            else        ramble_schema_field_array(b, name, k, cnt);
        }
    } else if (d->err){
        return;
    } else if (is_str){
        if (has_cap) ramble_schema_field_string(b, name, str_cap);
        else         ramble_schema_field_var_string(b, name);
    } else {
        ramble_schema_field(b, name, k);
    }
}

static void i_ramble_dsl_field_type(i_RambleDsl *d, RambleSchemaBuilder *b, i_RambleDefs *defs,
                                  const char *name){
    char tname[256]; const char *at;
    i_ramble_dsl_ws(d);
    if (*d->p == '{'){                                   /* a struct, or an array of them */
        size_t head; uint16_t cnt; int variable;
        d->p++;
        i_ramble_schema_builder_count(b);
        i_ramble_schema_builder_put_name(b, name);
        head = b->len;
        i_ramble_schema_builder_open_struct(b);
        i_ramble_dsl_fields(d, b, defs);
        if (!i_ramble_dsl_expect(d, '}')) return;
        ramble_schema_end_struct(b);
        if (i_ramble_dsl_suffix(d, &cnt, &variable) && !d->err)
            i_ramble_dsl_splice_array(b, head, cnt, variable);
        return;
    }
    at = d->p;
    if (!i_ramble_dsl_ident(d, tname)) return;
    i_ramble_dsl_word_type(d, b, defs, name, tname, at);
}

/* the fields of one struct body, up to and not consuming the closing brace */
static void i_ramble_dsl_fields(i_RambleDsl *d, RambleSchemaBuilder *b, i_RambleDefs *defs){
    char name[256];
    for (;;){
        i_ramble_dsl_ws(d);
        if (*d->p == '}' || *d->p == '\0' || d->err || b->err) return;
        if (!i_ramble_dsl_ident(d, name)) return;
        i_ramble_dsl_ws(d);
        if (!i_ramble_dsl_expect(d, ':')) return;
        i_ramble_dsl_field_type(d, b, defs, name);
        if (d->err) return;
        i_ramble_dsl_ws(d);
        if (*d->p == ',') d->p++;                        /* an optional separator */
    }
}

/* Name = type: compiles the body on its own, then keeps it, or verifies it matches an
 * existing or reserved definition of the same name exactly. */
static void i_ramble_dsl_def(i_RambleDsl *d, i_RambleDefs *defs, const char *name){
    RambleSchemaBuilder sb; uint32_t toff = 0, tlen = 0; int ok = 0, exists;
    exists = i_ramble_dsl_ref(defs, name, &toff, &tlen);
    sb = i_ramble_schema_begin_raw(defs->arena.alloc, defs->arena.user);
    i_ramble_dsl_field_type(d, &sb, defs, "");
    if (!d->err && !sb.err && sb.len){
        if (exists)                                      /* redefining is fine if identical */
            ok = (tlen == sb.len && memcmp(defs->arena.buf + toff, sb.buf, sb.len) == 0);
        else
            ok = i_ramble_defs_add_bytes(defs, name, strlen(name), sb.buf, sb.len);
    }
    if (sb.buf) sb.alloc(sb.user, sb.buf, 0);
    if (!ok) i_ramble_dsl_fail(d, d->p);
}

RambleSchema *ramble_schema_compile_env(RambleAllocFn alloc, void *user, const char *text,
                                    const RambleSchema *const *env, size_t n_env,
                                    const char **err){
    i_RambleDsl d; i_RambleDefs defs; RambleSchemaBuilder root;
    RambleSchema *s = NULL;
    char rootbuf[256];
    const char *rname = NULL; size_t rnlen = 0;
    const uint8_t *rtype = NULL; size_t rtlen = 0;
    int have_root = 0, is_struct = 0;

    if (err) *err = NULL;
    if (!alloc || !text) return NULL;
    memset(&defs, 0, sizeof defs);
    defs.env = env; defs.n_env = n_env;
    defs.arena = i_ramble_schema_begin_raw(alloc, user);
    root = i_ramble_schema_begin_raw(alloc, user);
    d.p = text; d.err = NULL;
    if (defs.arena.err || root.err) i_ramble_dsl_fail(&d, text);

    while (!d.err){
        const char *at;
        i_ramble_dsl_ws(&d);
        if (!*d.p) break;
        at = d.p;
        if (!i_ramble_dsl_ident(&d, rootbuf)) break;
        i_ramble_dsl_ws(&d);
        if (*d.p == '='){                                /* a definition */
            d.p++;
            i_ramble_dsl_def(&d, &defs, rootbuf);
            continue;
        }
        if (*d.p == '{'){                                /* Name { fields }: a struct root */
            d.p++;
            i_ramble_schema_builder_open_struct(&root);
            i_ramble_dsl_fields(&d, &root, &defs);
            if (!i_ramble_dsl_expect(&d, '}')) break;
            ramble_schema_end_struct(&root);
            rnlen = strlen(rootbuf); rname = rootbuf;
            is_struct = 1;
        } else {                                         /* a bare type, maybe a reference */
            d.p = at;
            i_ramble_dsl_field_type(&d, &root, &defs, "");
        }
        have_root = 1;
        i_ramble_dsl_ws(&d);
        if (*d.p) i_ramble_dsl_fail(&d, d.p);              /* the root ends the text */
        break;
    }
    if (!d.err && root.err) i_ramble_dsl_fail(&d, d.p);
    if (!d.err && defs.arena.err) i_ramble_dsl_fail(&d, d.p);

    if (!d.err){
        if (have_root){
            rtype = root.buf; rtlen = root.len;
            if (!is_struct && rtlen >= 2 && rtype[0] == RAMBLE_NAMED){
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
        if (!rtype || !rtlen) i_ramble_dsl_fail(&d, d.p);
    }
    /* the reservation covers definitions and references, never a plain Name { ... } root,
     * since nothing can reference a root. See spec/schema.md. */
    if (!d.err){
        size_t wlen = 2u + rnlen + rtlen;
        uint8_t *w = (uint8_t *)alloc(user, NULL, wlen);
        if (w){
            w[0] = (uint8_t)RAMBLE_SCHEMA_WIRE_VERSION;
            w[1] = (uint8_t)rnlen;
            if (rnlen) memcpy(w + 2, rname, rnlen);
            memcpy(w + 2 + rnlen, rtype, rtlen);
            s = ramble_schema_parse(w, wlen, alloc, user);
            alloc(user, w, 0);
        }
        if (!s) i_ramble_dsl_fail(&d, d.p);
    }
    if (root.buf) alloc(user, root.buf, 0);
    if (defs.arena.buf) alloc(user, defs.arena.buf, 0);
    if (!s && err) *err = d.err ? d.err : d.p;
    return s;
}

RambleSchema *ramble_schema_compile(RambleAllocFn alloc, void *user, const char *text, const char **err){
    return ramble_schema_compile_env(alloc, user, text, NULL, 0, err);
}
#pragma endregion
#ifndef RAMBLE_NO_STDTYPES
#pragma region serialize/stdtypes.c
#include <string.h>

/* The roster: a name and the canonical spelling of its type. These strings are the one
 * definition, the DSL compiles them on demand. Order does not matter, resolution is recursive. */
typedef struct { const char *name, *text; } i_RambleStdEntry;

static const i_RambleStdEntry i_ramble_std_table[] = {
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

/* the table is indexed by RambleStdType minus 1, a mismatch would shift every name */
typedef char i_ramble_std_table_check[
    (sizeof i_ramble_std_table / sizeof i_ramble_std_table[0] == (size_t)RAMBLE_STD_COUNT - 1u) ? 1 : -1];

const char *ramble_std_name(RambleStdType t){
    if (t <= RAMBLE_STD_NONE || t >= RAMBLE_STD_COUNT) return 0;
    return i_ramble_std_table[(int)t - 1].name;
}
const char *ramble_std_text(RambleStdType t){
    if (t <= RAMBLE_STD_NONE || t >= RAMBLE_STD_COUNT) return 0;
    return i_ramble_std_table[(int)t - 1].text;
}

RambleStdType ramble_std_by_name(RambleString name){
    int i;
    if (!name.data || !name.len) return RAMBLE_STD_NONE;
    for (i = 0; i < (int)RAMBLE_STD_COUNT - 1; i++){
        const char *n = i_ramble_std_table[i].name;
        size_t k = strlen(n);
        if (k == name.len && memcmp(n, name.data, k) == 0) return (RambleStdType)(i + 1);
    }
    return RAMBLE_STD_NONE;
}

/* The one seam the DSL calls: an unknown type word is looked up here and compiled from its text. */
const char *i_ramble_std_lookup(const char *name){
    int i;
    if (!name) return 0;
    for (i = 0; i < (int)RAMBLE_STD_COUNT - 1; i++)
        if (strcmp(i_ramble_std_table[i].name, name) == 0) return i_ramble_std_table[i].text;
    return 0;
}

RambleSchema *ramble_std_schema(RambleStdType t, RambleAllocFn alloc, void *user){
    const char *n = ramble_std_name(t);
    if (!n || !alloc) return 0;
    return ramble_schema_compile(alloc, user, n, 0);   /* the name alone resolves to the type */
}

/* Does type match the canonical root type bytes of the standard type. */
static int i_ramble_std_shape_eq(RambleStdType t, RambleBytes type, RambleAllocFn alloc, void *user){
    RambleSchema *c = ramble_std_schema(t, alloc, user);
    RambleBytes w; RambleString nm; size_t off; int ok = 0;
    if (!c) return 0;
    w = ramble_schema_wire(c); nm = ramble_schema_name(c);
    off = 2u + nm.len;
    if (type.data && off < w.len && type.len == w.len - off)
        ok = memcmp(type.data, w.data + off, type.len) == 0;
    ramble_schema_free(c, alloc, user);
    return ok;
}

/* peel a NAMED wrapper: fills *name and returns the inner type bytes */
static int i_ramble_std_peel(RambleBytes type, RambleString *name, RambleBytes *inner){
    size_t nl;
    if (!type.data || type.len < 2u || type.data[0] != RAMBLE_NAMED) return 0;
    nl = type.data[1];
    if (2u + nl > type.len || nl == 0) return 0;
    *name  = ramble_string((const char *)(type.data + 2), nl);
    *inner = ramble_bytes(type.data + 2 + nl, type.len - 2 - nl);
    return 1;
}

static RambleStdType i_ramble_std_check(RambleString name, RambleBytes inner,
                                    RambleAllocFn alloc, void *user){
    RambleStdType t = ramble_std_by_name(name);
    if (t == RAMBLE_STD_NONE || !alloc) return RAMBLE_STD_NONE;
    return i_ramble_std_shape_eq(t, inner, alloc, user) ? t : RAMBLE_STD_NONE;
}

RambleStdType ramble_std_recognize(const RambleSchema *s, RambleAllocFn alloc, void *user){
    RambleBytes w = ramble_schema_wire(s);
    RambleString nm = ramble_schema_name(s);
    size_t off = 2u + nm.len;
    if (!w.data || off >= w.len) return RAMBLE_STD_NONE;
    return i_ramble_std_check(nm, ramble_bytes(w.data + off, w.len - off), alloc, user);
}

RambleStdType ramble_std_recognize_field(const RambleSchema *s, uint16_t field,
                                     RambleAllocFn alloc, void *user){
    RambleBytes type = ramble_schema_field_type_wire(s, field), inner;
    RambleString name;
    if (!i_ramble_std_peel(type, &name, &inner)) return RAMBLE_STD_NONE;
    return i_ramble_std_check(name, inner, alloc, user);
}

RambleStdType ramble_std_recognize_elem(const RambleSchema *s, uint16_t field,
                                    RambleAllocFn alloc, void *user){
    RambleBytes type = ramble_schema_field_type_wire(s, field), inner;
    RambleSchemaFieldInfo fi;
    RambleString name;
    size_t head;
    if (!ramble_schema_field_at(s, field, &fi) || !type.data) return RAMBLE_STD_NONE;
    if (fi.kind == RAMBLE_ARR)       head = 3u;      /* [ARR][u16 count] */
    else if (fi.kind == RAMBLE_VARR) head = 1u;      /* [VARR] */
    else return RAMBLE_STD_NONE;
    if (head >= type.len) return RAMBLE_STD_NONE;
    if (!i_ramble_std_peel(ramble_bytes(type.data + head, type.len - head), &name, &inner))
        return RAMBLE_STD_NONE;
    return i_ramble_std_check(name, inner, alloc, user);
}

/* No math.h, so a consumer's build line never grows an -lm. Newton from the halved
 * exponent seed converges to full double precision within five steps. */
static double i_ramble_sqrt(double x){
    uint64_t b; double r; int i;
    if (!(x > 0.0)) return 0.0;                       /* 0, negatives and NaN alike */
    memcpy(&b, &x, 8);
    b = (b >> 1) + 0x1FF8000000000000ull;
    memcpy(&r, &b, 8);
    for (i = 0; i < 5; i++) r = 0.5 * (r + x / r);
    return r;
}

double ramble_double3_length(RambleDouble3 a){
    return i_ramble_sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}

RambleDouble3 ramble_double3_normalize(RambleDouble3 a){
    double n = ramble_double3_length(a);
    return n > 0.0 ? ramble_double3_scale(a, 1.0 / n) : a;
}

RambleQuaternion ramble_quaternion_normalize(RambleQuaternion q){
    double n = i_ramble_sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n <= 0.0) return ramble_quaternion_identity();
    n = 1.0 / n;
    return ramble_quaternion(q.x * n, q.y * n, q.z * n, q.w * n);
}

/* v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v), the branch free rotation form */
RambleDouble3 ramble_quaternion_rotate(RambleQuaternion q, RambleDouble3 v){
    RambleDouble3 u = ramble_double3(q.x, q.y, q.z);
    RambleDouble3 t = ramble_double3_cross(u, v);
    t = ramble_double3_add(t, ramble_double3_scale(v, q.w));
    return ramble_double3_add(v, ramble_double3_scale(ramble_double3_cross(u, t), 2.0));
}
#pragma endregion
#endif /* !RAMBLE_NO_STDTYPES */
#pragma region shm/core.c
/* Segment mapping and chunks over the platform layer. Compiles to nothing without RAMBLE_SHM. */


#ifdef RAMBLE_SHM
#include <string.h>
#include <stdlib.h>

size_t i_ramble_shm_desc_encode(const i_RambleShmDesc *d, uint8_t out[RAMBLE_SHM_DESC_WIRE]){
    i_ramble_le_w64(out,    d->segment_id);
    i_ramble_le_w32(out+8,  d->chunk);
    i_ramble_le_w32(out+12, d->length);
    i_ramble_le_w64(out+16, d->generation);
    return RAMBLE_SHM_DESC_WIRE;
}
int i_ramble_shm_desc_decode(i_RambleShmDesc *d, const uint8_t *in, size_t len){
    if (len < RAMBLE_SHM_DESC_WIRE) return 0;
    d->segment_id = i_ramble_le_r64(in);
    d->chunk      = i_ramble_le_r32(in+8);
    d->length     = i_ramble_le_r32(in+12);
    d->generation = i_ramble_le_r64(in+16);
    return 1;
}

void i_ramble_shm_seg_name(char *buf, uint64_t segment_id){
    static const char hex_digits[] = "0123456789abcdef";
    const char prefix[] = "/ramble.shm."; int i, k = 0;
    while (prefix[k]){ buf[k] = prefix[k]; k++; }
    for (i=15;i>=0;i--) buf[k++] = hex_digits[(segment_id >> (4*i)) & 0xF];
    buf[k] = 0;
}

uint32_t i_ramble_shm_class_bytes(uint32_t k){ return RAMBLE_SHM_CLASS_BASE << (k*RAMBLE_SHM_CLASS_SHIFT); }
uint32_t i_ramble_shm_class_for(uint32_t len){
    uint32_t k;
    for (k=0;k<RAMBLE_SHM_N_CLASSES;k++) if (i_ramble_shm_class_bytes(k) >= len) return k;
    return RAMBLE_SHM_N_CLASSES;   /* bigger than the top class, the caller sends inline */
}

struct i_RambleShmPool {
    void    *base;          /* mapping base */
    void    *handle;        /* OS handle for detach */
    size_t   map_bytes;     /* total mapped size */
    i_RambleShmSegHdr *hdr;
    uint8_t *chunks;        /* base of the chunk region */
    uint32_t chunk_bytes;
    uint32_t n_chunks;
    uint32_t stride;        /* per chunk bytes including the header */
    int      is_creator;
};

size_t i_ramble_shm_state_bytes(void){ return i_ramble_align_up(sizeof(struct i_RambleShmPool), 16u); }

#define RAMBLE__SHM_HDR_SZ  ((uint32_t)i_ramble_align_up(sizeof(i_RambleShmSegHdr), 16u))
#define RAMBLE__SHM_CHDR_SZ ((uint32_t)i_ramble_align_up(sizeof(i_RambleShmChunkHdr), 16u))

static void i_ramble_shm_geom(uint32_t chunk_bytes, uint32_t n_chunks,
                           uint32_t *out_stride, size_t *out_total){
    uint32_t aligned = (uint32_t)i_ramble_align_up(chunk_bytes, 16u);
    uint32_t stride = RAMBLE__SHM_CHDR_SZ + aligned;
    *out_stride = stride;
    *out_total  = (size_t)RAMBLE__SHM_HDR_SZ + (size_t)n_chunks * stride;
}

static i_RambleShmChunkHdr *i_ramble_shm_chunk_hdr(struct i_RambleShmPool *p, uint32_t i){
    return (i_RambleShmChunkHdr*)(p->chunks + (size_t)i * p->stride);
}
static uint8_t *i_ramble_shm_chunk_pay(struct i_RambleShmPool *p, uint32_t i){
    return (uint8_t*)i_ramble_shm_chunk_hdr(p, i) + RAMBLE__SHM_CHDR_SZ;
}

i_RambleShmPool *i_ramble_shm_create(void *pool_mem, const i_RambleShmConfig *cfg){
    struct i_RambleShmPool *p = (struct i_RambleShmPool*)pool_mem;
    uint32_t chunk_bytes = cfg->chunk_bytes ? cfg->chunk_bytes : RAMBLE_SHM_CHUNK_BYTES;
    uint32_t n_chunks = cfg->n_chunks    ? cfg->n_chunks    : RAMBLE_SHM_CHUNKS;
    uint32_t stride; size_t total; void *handle = NULL, *base; uint32_t i;
    if (!p || !cfg) return NULL;
    i_ramble_shm_geom(chunk_bytes, n_chunks, &stride, &total);
    base = i_ramble_plat_shm_create(cfg->name, total, &handle);
    if (!base) return NULL;
    memset(p, 0, sizeof *p);
    p->base = base; p->handle = handle; p->map_bytes = total;
    p->hdr = (i_RambleShmSegHdr*)base;
    p->chunks = (uint8_t*)base + RAMBLE__SHM_HDR_SZ;
    p->chunk_bytes = chunk_bytes; p->n_chunks = n_chunks; p->stride = stride; p->is_creator = 1;
    /* the segment starts zero filled: stamp the header and clear the generations */
    p->hdr->magic = RAMBLE_SHM_MAGIC; p->hdr->version = RAMBLE_SHM_VERSION;
    p->hdr->segment_id = cfg->segment_id; p->hdr->chunk_bytes = chunk_bytes; p->hdr->n_chunks = n_chunks;
    p->hdr->owner_pid = i_ramble_plat_pid();
    i_ramble_plat_host_uuid(p->hdr->owner_host);
    for (i = 0; i < n_chunks; i++){ i_RambleShmChunkHdr *c = i_ramble_shm_chunk_hdr(p, i); c->generation = 0; c->length = 0; }
    return p;
}

i_RambleShmPool *i_ramble_shm_attach(void *pool_mem, const i_RambleShmConfig *cfg){
    struct i_RambleShmPool *p = (struct i_RambleShmPool*)pool_mem;
    uint32_t chunk_bytes, n_chunks, stride; size_t map_bytes = 0, expect;
    void *handle = NULL, *base; uint8_t ours[16];
    if (!p || !cfg) return NULL;
    /* the geometry comes from the writer's header, cfg's create only fields are ignored */
    base = i_ramble_plat_shm_attach(cfg->name, &map_bytes, &handle);
    if (!base) return NULL;
    memset(p, 0, sizeof *p);
    p->base = base; p->handle = handle; p->map_bytes = map_bytes;
    p->hdr = (i_RambleShmSegHdr*)base;
    i_ramble_plat_host_uuid(ours);
    chunk_bytes = p->hdr->chunk_bytes; n_chunks = p->hdr->n_chunks;
    i_ramble_shm_geom(chunk_bytes, n_chunks, &stride, &expect);
    /* a stale, foreign, mismatched or truncated segment: the caller falls back to UDP */
    if (p->hdr->magic != RAMBLE_SHM_MAGIC || p->hdr->version != RAMBLE_SHM_VERSION ||
        memcmp(p->hdr->owner_host, ours, 16) != 0 ||
        chunk_bytes == 0 || n_chunks == 0 || expect > map_bytes){
        i_ramble_plat_shm_detach(base, map_bytes, handle, 0);
        return NULL;
    }
    p->chunks = (uint8_t*)base + RAMBLE__SHM_HDR_SZ;
    p->chunk_bytes = chunk_bytes; p->n_chunks = n_chunks; p->stride = stride; p->is_creator = 0;
    return p;
}

void *i_ramble_shm_chunk(i_RambleShmPool *p, uint32_t chunk, uint32_t *out_cap){
    if (!p || chunk >= p->n_chunks) return NULL;
    if (out_cap) *out_cap = p->chunk_bytes;
    return i_ramble_shm_chunk_pay(p, chunk);
}

void i_ramble_shm_stamp(i_RambleShmPool *p, uint32_t chunk, uint32_t len, i_RambleShmDesc *out){
    i_RambleShmChunkHdr *c;
    uint64_t generation;
    if (!p || chunk >= p->n_chunks) return;
    c = i_ramble_shm_chunk_hdr(p, chunk);
    c->length = len;
    generation = c->generation + 1u;                       /* bump so a straggler sees the reuse */
    i_ramble_plat_atomic_store64(&c->generation, generation);  /* release: publishes the payload */
    if (out){ out->segment_id = p->hdr->segment_id; out->chunk = chunk; out->length = len; out->generation = generation; }
}

const void *i_ramble_shm_read(i_RambleShmPool *p, const i_RambleShmDesc *d, uint32_t *out_len){
    i_RambleShmChunkHdr *c;
    if (!p || !d || d->chunk >= p->n_chunks) return NULL;
    c = i_ramble_shm_chunk_hdr(p, d->chunk);
    if (i_ramble_plat_atomic_load64(&c->generation) != d->generation) return NULL;  /* recycled */
    if (d->length > p->chunk_bytes) return NULL;
    if (out_len) *out_len = d->length;
    return i_ramble_shm_chunk_pay(p, d->chunk);
}

int i_ramble_shm_verify(i_RambleShmPool *p, const i_RambleShmDesc *d){
    i_RambleShmChunkHdr *c;
    if (!p || !d || d->chunk >= p->n_chunks) return 0;
    c = i_ramble_shm_chunk_hdr(p, d->chunk);
    return i_ramble_plat_atomic_load64(&c->generation) == d->generation;
}

void i_ramble_shm_detach(i_RambleShmPool *p){
    if (!p || !p->base) return;
    i_ramble_plat_shm_detach(p->base, p->map_bytes, p->handle, p->is_creator);
    p->base = NULL; p->handle = NULL;
}

int i_ramble_shm_host_match(const uint8_t peer_host[16], const uint8_t our_host[16]){
    return memcmp(peer_host, our_host, 16) == 0;
}

#endif /* RAMBLE_SHM */
#pragma endregion
#pragma region node/core.c
/* The sans-IO node core. A runtime drives it and does the IO. The rules are in spec/node.md. */

#include <string.h>

/* ramble_event_str and its bounded appenders. No stdio, so it stays in the sans-IO core. */
static char *i_ramble_event_append_str(char *p, char *end, const char *s){
    if (!s) return p;
    while (*s && p < end) *p++ = *s++;
    return p;
}
static char *i_ramble_event_append_u64(char *p, char *end, uint64_t v){
    char tmp[20]; int n = 0;
    do { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v);
    while (n && p < end) *p++ = tmp[--n];
    return p;
}
/* dotted quad and port */
static char *i_ramble_event_append_addr(char *p, char *end, const RambleEvent *ev){
    int i;
    for (i = 0; i < 4; i++){ if (i) p = i_ramble_event_append_str(p,end,"."); p = i_ramble_event_append_u64(p,end,ev->ip[i]); }
    p = i_ramble_event_append_str(p,end,":"); return i_ramble_event_append_u64(p,end,ev->port);
}
/* topic=name or topic=index */
static char *i_ramble_event_append_topic(char *p, char *end, const RambleEvent *ev){
    p = i_ramble_event_append_str(p,end,"topic=");
    if (ev->topic_name) return i_ramble_event_append_str(p,end,ev->topic_name);
    return i_ramble_event_append_u64(p,end,ev->topic);
}
/* the peer's name, else id=n */
static char *i_ramble_event_append_peer(char *p, char *end, const RambleEvent *ev){
    if (ev->peer_name && ev->peer_name[0]) return i_ramble_event_append_str(p,end,ev->peer_name);
    p = i_ramble_event_append_str(p,end,"id="); return i_ramble_event_append_u64(p,end,ev->peer);
}
#ifndef RAMBLE_NO_DIAG   /* only the verbose error body uses these two */
static char *i_ramble_event_append_hex(char *p, char *end, uint64_t v){
    char tmp[16]; int n = 0;
    do { int d = (int)(v & 0xF); tmp[n++] = (char)(d < 10 ? '0'+d : 'a'+d-10); v >>= 4; } while (v);
    while (n && p < end) *p++ = tmp[--n];
    return p;
}
static char *i_ramble_event_append_oserr(char *p, char *end, const RambleEvent *ev){
    if (!ev->os_error) return p;
    p = i_ramble_event_append_str(p,end," (os_err="); p = i_ramble_event_append_u64(p,end,(uint64_t)(unsigned int)ev->os_error);
    return i_ramble_event_append_str(p,end,")");
}
#endif

/* the RAMBLE_ERROR body, split out so the text compiles away under RAMBLE_NO_DIAG */
static char *i_ramble_event_error_str(char *p, char *end, const RambleEvent *ev){
#ifdef RAMBLE_NO_DIAG
    p = i_ramble_event_append_str(p,end,"error "); return i_ramble_event_append_u64(p,end,(uint64_t)ev->error);
#else
    switch (ev->error){
    case RAMBLE_E_NAME_COLLISION:
        p=i_ramble_event_append_str(p,end,"name-collision "); p=i_ramble_event_append_topic(p,end,ev);
        p=i_ramble_event_append_str(p,end," peer "); p=i_ramble_event_append_peer(p,end,ev);
        p=i_ramble_event_append_str(p,end," identity=0x"); p=i_ramble_event_append_hex(p,end,ev->identity);
        p=i_ramble_event_append_str(p,end,": match refused"); break;
    case RAMBLE_E_QOS_INCOMPATIBLE:
        p=i_ramble_event_append_str(p,end,"qos-incompatible "); p=i_ramble_event_append_topic(p,end,ev);
        p=i_ramble_event_append_str(p,end," from "); p=i_ramble_event_append_peer(p,end,ev);
        p=i_ramble_event_append_str(p,end,": reliable subscriber refused best-effort publisher"); break;
    case RAMBLE_E_KIND_MISMATCH:
        p=i_ramble_event_append_str(p,end,"kind-mismatch "); p=i_ramble_event_append_topic(p,end,ev);
        p=i_ramble_event_append_str(p,end," from "); p=i_ramble_event_append_peer(p,end,ev);
        p=i_ramble_event_append_str(p,end,": same name, different entity kind, refused"); break;
    case RAMBLE_E_SCHEMA_MISMATCH:
        p=i_ramble_event_append_str(p,end,"schema-mismatch "); p=i_ramble_event_append_topic(p,end,ev);
        p=i_ramble_event_append_str(p,end," peer "); p=i_ramble_event_append_peer(p,end,ev);
        p=i_ramble_event_append_str(p,end,": ");
        p=i_ramble_event_append_str(p,end, ev->schema_detail && ev->schema_detail[0]
                                  ? ev->schema_detail : "incompatible schemas, refused"); break;
    case RAMBLE_E_INTEREST_OVERFLOW:
        p=i_ramble_event_append_str(p,end,"interest-overflow peer "); p=i_ramble_event_append_peer(p,end,ev);
        p=i_ramble_event_append_str(p,end,": "); p=i_ramble_event_append_u64(p,end,ev->lost_count);
        p=i_ramble_event_append_str(p,end," matched topics whose index map could not be allocated"); break;
    case RAMBLE_E_META_TRUNCATED_INTEREST:
        p=i_ramble_event_append_str(p,end,"meta-truncated: interest list dropped (announce overlay full)"); break;
    case RAMBLE_E_META_TRUNCATED_SCHEMA:
        p=i_ramble_event_append_str(p,end,"meta-truncated: schema section dropped (announce overlay full)"); break;
    case RAMBLE_E_PEER_META_TOO_BIG:
        p=i_ramble_event_append_str(p,end,"peer-meta-too-big "); p=i_ramble_event_append_peer(p,end,ev);
        if (ev->ip_len==4){ p=i_ramble_event_append_str(p,end," at "); p=i_ramble_event_append_addr(p,end,ev); }
        p=i_ramble_event_append_str(p,end,": "); p=i_ramble_event_append_u64(p,end,ev->too_big_bytes);
        p=i_ramble_event_append_str(p,end," byte blob exceeds our capacity, refused"); break;
    case RAMBLE_E_MSG_TOO_BIG:
        p=i_ramble_event_append_str(p,end,"msg-too-big "); p=i_ramble_event_append_topic(p,end,ev);
        p=i_ramble_event_append_str(p,end," from "); p=i_ramble_event_append_peer(p,end,ev);
        p=i_ramble_event_append_str(p,end," ("); p=i_ramble_event_append_u64(p,end,ev->too_big_bytes);
        p=i_ramble_event_append_str(p,end," bytes), skipped"); break;
    case RAMBLE_E_PEER_REFUSED:
        p=i_ramble_event_append_str(p,end,"peer-refused at "); p=i_ramble_event_append_addr(p,end,ev);
        p=i_ramble_event_append_str(p,end,": peer table full of active peers (raise max_peers)"); break;
    case RAMBLE_E_EVICTED_UNSENT:
        p=i_ramble_event_append_str(p,end,"evicted-unsent "); p=i_ramble_event_append_topic(p,end,ev);
        p=i_ramble_event_append_str(p,end," seqno "); p=i_ramble_event_append_u64(p,end,ev->lost_first);
        p=i_ramble_event_append_str(p,end,".."); p=i_ramble_event_append_u64(p,end,ev->lost_first + ev->lost_count - 1);
        p=i_ramble_event_append_str(p,end,": send burst outran the TX drain"); break;
    case RAMBLE_E_UNMATCHED_SEND:
        p=i_ramble_event_append_str(p,end,"unmatched-send "); p=i_ramble_event_append_topic(p,end,ev);
        p=i_ramble_event_append_str(p,end,": committed with no subscriber while a match was still resolving (likely missed an already-present subscriber)"); break;
    case RAMBLE_E_DUPLICATE_AUTHORITY:
        p=i_ramble_event_append_str(p,end,"duplicate-authority "); p=i_ramble_event_append_topic(p,end,ev);
        p=i_ramble_event_append_str(p,end,": peer "); p=i_ramble_event_append_peer(p,end,ev);
        p=i_ramble_event_append_str(p,end," also claims the handler/owner side (expected exactly one)"); break;
    case RAMBLE_E_OOM:
        p=i_ramble_event_append_str(p,end,"out-of-memory");
        if (ev->too_big_bytes){ p=i_ramble_event_append_str(p,end,": "); p=i_ramble_event_append_u64(p,end,ev->too_big_bytes);
                                p=i_ramble_event_append_str(p,end," bytes needed"); }
        break;
    case RAMBLE_E_PLATFORM:
        p=i_ramble_event_append_str(p,end,"platform net init failed"); break;
    case RAMBLE_E_BAD_ADDRESS:
        p=i_ramble_event_append_str(p,end,"configured address could not be parsed"); break;
    case RAMBLE_E_SOCKET:
        p=i_ramble_event_append_str(p,end,"socket open failed"); p=i_ramble_event_append_oserr(p,end,ev); break;
    case RAMBLE_E_BIND:
        p=i_ramble_event_append_str(p,end,"bind failed on port "); p=i_ramble_event_append_u64(p,end,ev->port);
        p=i_ramble_event_append_oserr(p,end,ev); break;
    case RAMBLE_E_MCAST_JOIN:
        p=i_ramble_event_append_str(p,end,"multicast join failed"); p=i_ramble_event_append_oserr(p,end,ev); break;
    case RAMBLE_E_SEND:
        p=i_ramble_event_append_str(p,end,"send failed to "); p=i_ramble_event_append_peer(p,end,ev);
        if (ev->topic_name){ p=i_ramble_event_append_str(p,end," "); p=i_ramble_event_append_topic(p,end,ev); }
        if (ev->too_big_bytes){ p=i_ramble_event_append_str(p,end," ("); p=i_ramble_event_append_u64(p,end,ev->too_big_bytes);
                                p=i_ramble_event_append_str(p,end," B)"); }
        p=i_ramble_event_append_oserr(p,end,ev); break;
    case RAMBLE_E_RECV:
        p=i_ramble_event_append_str(p,end,"recv failed"); p=i_ramble_event_append_oserr(p,end,ev); break;
    case RAMBLE_E_POLL:
        p=i_ramble_event_append_str(p,end,"poll failed"); p=i_ramble_event_append_oserr(p,end,ev); break;
    case RAMBLE_E_WAKER:
        p=i_ramble_event_append_str(p,end,"cross-thread waker unavailable (wakes at next tick)"); break;
    case RAMBLE_E_NONE: default:
        p=i_ramble_event_append_str(p,end,"error"); break;
    }
    return p;
#endif
}

const char *ramble_event_str(const RambleEvent *ev, char *buf, size_t cap){
    char *p, *end;
    if (!buf || !cap) return buf;
    p = buf; end = buf + cap - 1;                  /* one byte reserved for the NUL */
    switch (ev->kind){
    case RAMBLE_PEER_UP:
        p = i_ramble_event_append_str(p,end,"peer-up "); p = i_ramble_event_append_peer(p,end,ev);
        if (ev->ip_len == 4){ p = i_ramble_event_append_str(p,end," at "); p = i_ramble_event_append_addr(p,end,ev); }
        break;
    case RAMBLE_PEER_DOWN:
        p = i_ramble_event_append_str(p,end,"peer-down "); p = i_ramble_event_append_peer(p,end,ev);
        break;
    case RAMBLE_PEER_INTEREST:
        p = i_ramble_event_append_str(p,end,"interest from "); p = i_ramble_event_append_peer(p,end,ev);
        p = i_ramble_event_append_str(p,end," publish-to="); p = i_ramble_event_append_u64(p,end,ev->publish_topics);
        p = i_ramble_event_append_str(p,end," topics, receive-from="); p = i_ramble_event_append_u64(p,end,ev->receive_topics);
        p = i_ramble_event_append_str(p,end," topics");
        break;
    case RAMBLE_MSG_LOST:
        p = i_ramble_event_append_str(p,end,"msg-lost "); p = i_ramble_event_append_topic(p,end,ev);
        p = i_ramble_event_append_str(p,end," from "); p = i_ramble_event_append_peer(p,end,ev);
        p = i_ramble_event_append_str(p,end," seqno "); p = i_ramble_event_append_u64(p,end,ev->lost_first);
        p = i_ramble_event_append_str(p,end,".."); p = i_ramble_event_append_u64(p,end,ev->lost_first + ev->lost_count - 1);
        break;
    case RAMBLE_ERROR:
        p = i_ramble_event_error_str(p, end, ev);
        break;
    }
    *p = '\0';                                     /* p is at most end, in range */
    return buf;
}

/* The reflection tables: one channel per advertised topic index, folded into entities by
   kind and the four byte suffix convention. See spec/reflection.md. */
#define I_RAMBLE_NONE16   0xFFFFu
#define I_RAMBLE_NAME_NONE 0xFFFFFFFFu
typedef struct {
    uint32_t hash;          /* the low 32 name id from the announce */
    uint32_t name_off;      /* into the names arena, NAME_NONE until details land */
    const RambleSchema *schema;   /* interned, lives until close */
    uint64_t schema_hash;
    uint16_t entity;        /* the entity slot, NONE16 until folded */
    uint8_t  name_len, kind, role, reliable, attrs, present;
} i_RambleChannel;
typedef struct {
    uint64_t id;            /* ramble_topic_id of the base name, 0 while unfetched */
    uint32_t name_off, hash;
    uint16_t primary, rsp, prg, set;   /* channel indices, NONE16 where absent */
    uint8_t  name_len, kind, incomplete;
} i_RamblePeerEntity;
typedef struct {
    i_RambleChannel    *chan; uint32_t n_chan, cap_chan;   /* dense by the peer's topic index */
    i_RamblePeerEntity *ent;  uint32_t n_ent,  cap_ent;
    char               *names; uint32_t names_len, names_cap;
    uint8_t  dirty;
    uint8_t  addr_len;
    char     addr[48];      /* "ip:port", formatted once at peer up */
} i_RambleReflect;
/* one folded entity of the whole mesh, keyed by kind and the 64 bit name id */
typedef struct {
    uint64_t id, generation;
    uint32_t from_peer, provider;
    uint16_t from_slot, provider_slot, providers, consumers;
    uint8_t  kind, conflict, has_provider;
} i_RambleMeshEntity;

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
    i_RambleReflect refl;          /* what this peer advertises, as tables */
} i_RambleNodePeerExtra;

/* The schema state for the gate and delivery: schemas interned by hash, reader views per
   (schema, topic), the decode map per (peer, topic). Flat, hook allocated, kept until close. */
typedef struct { uint64_t hash; RambleSchema *parsed; } i_RambleNodeSchemaIntern;
typedef struct { uint64_t hash; uint16_t topic; RambleSchema *rebased; } i_RambleNodeSchemaBind;
typedef struct { uint32_t peer; uint16_t topic; const RambleSchema *schema; } i_RambleNodePeerSchema;
#ifndef RAMBLE_NO_DIAG
/* why the gate refused (peer, topic, direction), recorded at detail intake for the event */
#define RAMBLE__SCHEMA_WHY_MAX 128
typedef struct { uint32_t peer; uint16_t topic; uint8_t peer_is_pub;
                 char text[RAMBLE__SCHEMA_WHY_MAX]; } i_RambleNodeSchemaWhy;
#endif

struct i_RambleNodeCore {
    RambleTransportState           *transport;
    RambleDiscoveryState  *discovery;   /* the peer table we delegate to */
    RambleEventFn         on_event;
    void                 *user;
    int                   oob_capable;
    uint8_t               oob_host[16];
    uint8_t              *meta_buf;    /* our announce overlay, hook allocated at its actual size */
    uint16_t              meta_cap;
    uint16_t              meta_len;
    uint16_t              frag_size;   /* baked into the overlay */
    RambleMetaSchema       *chan_schemas;  /* per topic schema advertisement, hash 0 = none */
    const RambleSchema    **chan_compiled; /* per topic parsed schema, the gate's local side */
    uint16_t                n_topics;
    RambleAllocFn           alloc;         /* required */
    void                 *alloc_user;
    uint8_t              *detail_buf;    /* detail response scratch, grown on demand */
    uint32_t              detail_cap;
    uint8_t               detail_due_any;/* some peer's request is due */
    i_RambleNodeSchemaIntern *interned;     uint32_t n_interned,     cap_interned;
    i_RambleNodeSchemaBind   *binds;        uint32_t n_binds,        cap_binds;
    i_RambleNodePeerSchema   *peer_schemas; uint32_t n_peer_schemas, cap_peer_schemas;
    uint8_t                   fetch_details; /* observer mode */
    i_RambleReflect           self;          /* this node's own channels */
    RambleString              self_name;
    i_RambleMeshEntity       *mesh; uint32_t n_mesh, cap_mesh;
    uint8_t                 mesh_dirty;
    uint32_t                mesh_epoch;
    void                   *scratch; uint32_t scratch_cap;   /* fold and mesh build workspace */
#ifndef RAMBLE_NO_DIAG
    i_RambleNodeSchemaWhy    *schema_whys;   uint32_t n_schema_whys,   cap_schema_whys;
#endif
};

/* grow one flat array through the hook. 1 with *arr and *cap updated, else 0 */
static int i_ramble_node_core_array_reserve(i_RambleNodeCore *c, void **arr, uint32_t *cap,
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

uint16_t i_ramble_node_core_peer_user_bytes(void){ return (uint16_t)sizeof(i_RambleNodePeerExtra); }
void i_ramble_node_core_bind_discovery(i_RambleNodeCore *c, RambleDiscoveryState *discovery){ c->discovery = discovery; }

/* the arena layout: the core struct, then the per topic schema registry. One sequence so
   measure and build agree */
static void i_ramble_node_core_layout(i_RambleBump *b, uint16_t n_topics,
                              i_RambleNodeCore **out_c,
                              RambleMetaSchema **out_schemas, const RambleSchema ***out_compiled){
    i_RambleNodeCore *c       = (i_RambleNodeCore*)i_ramble_bump_take(b, sizeof(struct i_RambleNodeCore), 16);
    RambleMetaSchema *schemas = (RambleMetaSchema*)i_ramble_bump_take(b, (size_t)n_topics*sizeof(RambleMetaSchema), 16);
    const RambleSchema **compiled = (const RambleSchema**)i_ramble_bump_take(b, (size_t)n_topics*sizeof(RambleSchema*), 16);
    if (out_c)        *out_c        = c;
    if (out_schemas)  *out_schemas  = schemas;
    if (out_compiled) *out_compiled = compiled;
}

size_t i_ramble_node_core_required_memory(uint16_t n_topics){
    i_RambleBump b; memset(&b, 0, sizeof b);
    i_ramble_node_core_layout(&b, n_topics, NULL, NULL, NULL);
    return b.offset + 16u;   /* slack to align the caller's mem up to base */
}

i_RambleNodeCore *i_ramble_node_core_init(void *mem, size_t cap, const i_RambleNodeCoreConfig *cfg){
    i_RambleBump b; i_RambleNodeCore *c; uint8_t *base;
    RambleMetaSchema *schemas; const RambleSchema **compiled;
    if (!mem || !cfg || !cfg->transport || !cfg->alloc) return NULL;
    if (cap < i_ramble_node_core_required_memory(cfg->n_topics)) return NULL;
    base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    memset(&b, 0, sizeof b); b.base = base; b.cap = cap - (size_t)(base - (uint8_t*)mem);
    i_ramble_node_core_layout(&b, cfg->n_topics, &c, &schemas, &compiled);

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
i_RambleNodeCore *i_ramble_node_core_migrate(i_RambleNodeCore *old, void *new_mem, size_t new_cap,
                                       uint16_t new_n_topics){
    i_RambleBump b; i_RambleNodeCore *c; uint8_t *base;
    RambleMetaSchema *schemas; const RambleSchema **compiled;
    uint16_t keep;
    if (!old) return NULL;
    if (new_cap < i_ramble_node_core_required_memory(new_n_topics)) return NULL;
    base = (uint8_t*)(((uintptr_t)new_mem + 15u) & ~(uintptr_t)15u);
    memset(&b, 0, sizeof b); b.base = base; b.cap = new_cap - (size_t)(base - (uint8_t*)new_mem);
    i_ramble_node_core_layout(&b, new_n_topics, &c, &schemas, &compiled);
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

void i_ramble_node_core_set_topic_schema(i_RambleNodeCore *c, uint16_t topic_index,
                                         const RambleSchema *schema){
    if (!c || topic_index >= c->n_topics) return;
    c->chan_compiled[topic_index]     = schema;
    c->chan_schemas[topic_index].hash = schema ? ramble_schema_hash(schema) : 0;
    c->chan_schemas[topic_index].wire = schema ? ramble_schema_wire(schema) : ramble_bytes(NULL, 0);
}

/* Drops the live pointers but keeps the hash as the slot's fingerprint. The responder
 * never answers a retired slot, so the stale hash is never served. */
void i_ramble_node_core_retire_topic_schema(i_RambleNodeCore *c, uint16_t topic_index){
    if (!c || topic_index >= c->n_topics) return;
    c->chan_compiled[topic_index]     = NULL;
    c->chan_schemas[topic_index].wire = ramble_bytes(NULL, 0);
}

/* The retired slot's fingerprint, 0 = it carried no schema. */
uint64_t i_ramble_node_core_topic_schema_hash(i_RambleNodeCore *c, uint16_t topic_index){
    return (c && topic_index < c->n_topics) ? c->chan_schemas[topic_index].hash : 0;
}

/* the schema gate and the delivery binding */

/* A peer schema parsed once per hash. The claimed hash must equal the wire's real hash,
   or a lying peer could poison the intern for every honest one. */
static RambleSchema *i_ramble_node_core_intern(i_RambleNodeCore *c, uint64_t hash, RambleBytes wire){
    uint32_t i; RambleSchema *p;
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
    p = ramble_schema_parse(wire.data, wire.len, c->alloc, c->alloc_user);
    if (!p) return NULL;
    if (ramble_schema_hash(p) != hash ||
        !i_ramble_node_core_array_reserve(c, (void**)&c->interned, &c->cap_interned,
                                        c->n_interned + 1u, sizeof *c->interned)){
        ramble_schema_free(p, c->alloc, c->alloc_user);
        return NULL;
    }
    c->interned[c->n_interned].hash = hash;
    c->interned[c->n_interned].parsed = p;
    c->n_interned++;
    return p;
}

/* the reader view for (writer schema, topic): our fields on their layout, cached */
static RambleSchema *i_ramble_node_core_bind(i_RambleNodeCore *c, uint64_t hash, uint16_t topic_index,
                                         const RambleSchema *ours, const RambleSchema *pub){
    uint32_t i; RambleSchema *rb;
    for (i = 0; i < c->n_binds; i++)
        if (c->binds[i].hash == hash && c->binds[i].topic == topic_index) return c->binds[i].rebased;
    rb = ramble_schema_rebase(ours, pub, c->alloc, c->alloc_user);
    if (!rb) return NULL;
    if (!i_ramble_node_core_array_reserve(c, (void**)&c->binds, &c->cap_binds,
                                        c->n_binds + 1u, sizeof *c->binds)){
        ramble_schema_free(rb, c->alloc, c->alloc_user);
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
#define i_RAMBLE_NODE_SCHEMA_OURS ((const RambleSchema *)(uintptr_t)1)
static void i_ramble_node_core_peer_schema_set(i_RambleNodeCore *c, uint32_t peer, uint16_t topic_index,
                                             const RambleSchema *schema){
    uint32_t i;
    for (i = 0; i < c->n_peer_schemas; i++)
        if (c->peer_schemas[i].peer == peer && c->peer_schemas[i].topic == topic_index){
            c->peer_schemas[i].schema = schema;
            return;
        }
    if (!schema) return;
    if (!i_ramble_node_core_array_reserve(c, (void**)&c->peer_schemas, &c->cap_peer_schemas,
                                        c->n_peer_schemas + 1u, sizeof *c->peer_schemas)) return;
    c->peer_schemas[c->n_peer_schemas].peer = peer;
    c->peer_schemas[c->n_peer_schemas].topic = topic_index;
    c->peer_schemas[c->n_peer_schemas].schema = schema;
    c->n_peer_schemas++;
}

/* drop a peer's map entries, on GONE or when its id is recycled */
static void i_ramble_node_core_peer_schema_clear(i_RambleNodeCore *c, uint32_t peer){
    uint32_t i = 0;
    while (i < c->n_peer_schemas){
        if (c->peer_schemas[i].peer == peer)
            c->peer_schemas[i] = c->peer_schemas[--c->n_peer_schemas];   /* swap remove */
        else i++;
    }
}

/* why a schema gate refused, feeding RambleEvent.schema_detail */

#ifndef RAMBLE_NO_DIAG
static void i_ramble_node_core_schema_why_set(i_RambleNodeCore *c, uint32_t peer, uint16_t topic_index,
                                            int peer_is_pub, const char *text){
    uint32_t i; i_RambleNodeSchemaWhy *w = NULL; size_t n;
    for (i = 0; i < c->n_schema_whys; i++)
        if (c->schema_whys[i].peer == peer && c->schema_whys[i].topic == topic_index &&
            c->schema_whys[i].peer_is_pub == (uint8_t)peer_is_pub){ w = &c->schema_whys[i]; break; }
    if (!w){
        if (!i_ramble_node_core_array_reserve(c, (void**)&c->schema_whys, &c->cap_schema_whys,
                                            c->n_schema_whys + 1u, sizeof *c->schema_whys)) return;
        w = &c->schema_whys[c->n_schema_whys++];
        w->peer = peer; w->topic = topic_index; w->peer_is_pub = (uint8_t)peer_is_pub;
    }
    n = 0;
    while (text[n] && n < RAMBLE__SCHEMA_WHY_MAX - 1u){ w->text[n] = text[n]; n++; }
    w->text[n] = '\0';
}
static void i_ramble_node_core_schema_why_drop(i_RambleNodeCore *c, uint32_t peer, uint16_t topic_index,
                                             int peer_is_pub){
    uint32_t i = 0;
    while (i < c->n_schema_whys){
        if (c->schema_whys[i].peer == peer && c->schema_whys[i].topic == topic_index &&
            c->schema_whys[i].peer_is_pub == (uint8_t)peer_is_pub)
            c->schema_whys[i] = c->schema_whys[--c->n_schema_whys];   /* swap remove */
        else i++;
    }
}
static void i_ramble_node_core_schema_why_clear(i_RambleNodeCore *c, uint32_t peer){
    uint32_t i = 0;
    while (i < c->n_schema_whys){
        if (c->schema_whys[i].peer == peer)
            c->schema_whys[i] = c->schema_whys[--c->n_schema_whys];
        else i++;
    }
}
const char *i_ramble_node_core_schema_why(i_RambleNodeCore *c, uint32_t peer, uint16_t topic_index,
                                        int peer_is_pub){
    uint32_t i;
    if (!c) return NULL;
    for (i = 0; i < c->n_schema_whys; i++)
        if (c->schema_whys[i].peer == peer && c->schema_whys[i].topic == topic_index &&
            c->schema_whys[i].peer_is_pub == (uint8_t)peer_is_pub)
            return c->schema_whys[i].text;
    return NULL;
}
const char *i_ramble_node_core_note_size_mismatch(i_RambleNodeCore *c, uint32_t peer,
                                        uint16_t topic_index, uint64_t got_len, uint64_t want_len){
    char text[RAMBLE__SCHEMA_WHY_MAX];
    char *p = text, *end = text + sizeof text - 1;
    if (!c) return NULL;
    p = i_ramble_event_append_str(p, end, "message is ");
    p = i_ramble_event_append_u64(p, end, got_len);
    p = i_ramble_event_append_str(p, end, " bytes, the publisher's schema says ");
    p = i_ramble_event_append_u64(p, end, want_len);
    *p = '\0';
    i_ramble_node_core_schema_why_set(c, peer, topic_index, 1, text);
    return i_ramble_node_core_schema_why(c, peer, topic_index, 1);
}
#else
static void i_ramble_node_core_schema_why_clear(i_RambleNodeCore *c, uint32_t peer){
    (void)c; (void)peer;
}
const char *i_ramble_node_core_schema_why(i_RambleNodeCore *c, uint32_t peer, uint16_t topic_index,
                                        int peer_is_pub){
    (void)c; (void)peer; (void)topic_index; (void)peer_is_pub; return NULL;
}
const char *i_ramble_node_core_note_size_mismatch(i_RambleNodeCore *c, uint32_t peer,
                                        uint16_t topic_index, uint64_t got_len, uint64_t want_len){
    (void)c; (void)peer; (void)topic_index; (void)got_len; (void)want_len; return NULL;
}
#endif

const RambleSchema *i_ramble_node_core_msg_schema(i_RambleNodeCore *c, uint32_t peer, uint16_t topic_index){
    uint32_t i;
    if (!c) return NULL;
    for (i = 0; i < c->n_peer_schemas; i++)
        if (c->peer_schemas[i].peer == peer && c->peer_schemas[i].topic == topic_index){
            const RambleSchema *s = c->peer_schemas[i].schema;
            if (s == i_RAMBLE_NODE_SCHEMA_OURS)   /* an identical schema entry: resolve live */
                return topic_index < c->n_topics ? c->chan_compiled[topic_index] : NULL;
            return s;
        }
    return NULL;
}

/* The slot was rebound with a retype: every binding, view and refusal reason recorded for
 * the old occupant is stale. The verdicts re pended in the transport, so they re derive. */
void i_ramble_node_core_topic_rebound(i_RambleNodeCore *c, uint16_t topic_index){
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
            if (c->binds[i].rebased) ramble_schema_free(c->binds[i].rebased, c->alloc, c->alloc_user);
            c->binds[i] = c->binds[--c->n_binds];
        } else i++;
    }
#ifndef RAMBLE_NO_DIAG
    i = 0;
    while (i < c->n_schema_whys){
        if (c->schema_whys[i].topic == topic_index)
            c->schema_whys[i] = c->schema_whys[--c->n_schema_whys];
        else i++;
    }
#endif
}

/* the reflection tables */

static RambleBytes i_ramble_node_core_interest_of(i_RambleNodePeerExtra *ex, RambleBytes meta,
                            uint32_t meta_version);

static uint64_t i_ramble_node_core_fnv(uint64_t h, const void *p, size_t n){
    const uint8_t *b = (const uint8_t*)p; size_t i;
    for (i = 0; i < n; i++){ h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

static void i_ramble_reflect_free(i_RambleNodeCore *c, i_RambleReflect *r){
    if (!c->alloc) return;
    if (r->chan)  c->alloc(c->alloc_user, r->chan, 0);
    if (r->ent)   c->alloc(c->alloc_user, r->ent, 0);
    if (r->names) c->alloc(c->alloc_user, r->names, 0);
    memset(r, 0, sizeof *r);
}

static void i_ramble_node_core_mesh_bump(i_RambleNodeCore *c){
    c->mesh_dirty = 1;
    c->mesh_epoch++;
}

/* the channel record for index, growing the dense table with new slots absent */
static i_RambleChannel *i_ramble_reflect_channel(i_RambleNodeCore *c, i_RambleReflect *r, uint16_t index){
    if ((uint32_t)index >= r->n_chan){
        uint32_t i, need = (uint32_t)index + 1u;
        if (!i_ramble_node_core_array_reserve(c, (void**)&r->chan, &r->cap_chan, need, sizeof *r->chan))
            return NULL;
        for (i = r->n_chan; i < need; i++){
            memset(&r->chan[i], 0, sizeof r->chan[i]);
            r->chan[i].name_off = I_RAMBLE_NAME_NONE;
            r->chan[i].entity = I_RAMBLE_NONE16;
        }
        r->n_chan = need;
    }
    return &r->chan[index];
}

/* appends a NUL terminated name copy and returns its offset, NAME_NONE on OOM */
static uint32_t i_ramble_reflect_name(i_RambleNodeCore *c, i_RambleReflect *r, RambleString name){
    uint32_t off = r->names_len, need = r->names_len + (uint32_t)name.len + 1u;
    if (!i_ramble_node_core_array_reserve(c, (void**)&r->names, &r->names_cap, need, 1)) return I_RAMBLE_NAME_NONE;
    memcpy(r->names + off, name.data, name.len);
    r->names[off + name.len] = '\0';
    r->names_len = need;
    return off;
}

/* an interest apply: kind, role, reliability and hash per advertised index. Absent slots clear */
static void i_ramble_node_core_reflect_interest(i_RambleNodeCore *c, i_RambleNodePeerExtra *ex,
                                              RambleBytes interest){
    i_RambleReflect *r = &ex->refl;
    RambleInterestIter it; RambleTopicEntry e; uint32_t i;
    if (!interest.data) return;
    for (i = 0; i < r->n_chan; i++) r->chan[i].present = 0;
    memset(&it, 0, sizeof it);
    while (ramble_interest_next(interest, &it, &e)){
        i_RambleChannel *ch = i_ramble_reflect_channel(c, r, e.index);
        if (!ch) return;
        ch->hash = e.hash; ch->kind = e.kind; ch->role = e.role;
        ch->reliable = e.reliable; ch->present = 1;
    }
    r->dirty = 1;
}

/* a detail entry: the name, the attrs and the interned schema for one index */
static void i_ramble_node_core_reflect_detail(i_RambleNodeCore *c, i_RambleNodePeerExtra *ex,
                                            const RambleDetail *d){
    i_RambleReflect *r = &ex->refl;
    i_RambleChannel *ch;
    if (d->name.len == 0 || d->name.len > RAMBLE_TOPIC_NAME_MAX) return;
    ch = i_ramble_reflect_channel(c, r, d->index);
    if (!ch) return;
    if (ch->name_off == I_RAMBLE_NAME_NONE){
        ch->name_off = i_ramble_reflect_name(c, r, d->name);
        if (ch->name_off == I_RAMBLE_NAME_NONE) return;
        ch->name_len = (uint8_t)d->name.len;
    }
    ch->attrs = d->attrs;
    ch->schema_hash = d->schema_hash;
    ch->schema = d->schema_hash ? i_ramble_node_core_intern(c, d->schema_hash, d->schema_wire) : NULL;
    r->dirty = 1;
}

static void i_ramble_reflect_format_addr(i_RambleReflect *r, const RambleDiscoveryAddr *a){
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

/* one row per RambleTopicKind: its entity, whether it is the primary channel, its name
   suffix, and which role is the provider side */
typedef struct { uint8_t entity, primary, provider_pubs; const char *suffix; } i_RambleKindRow;
static const i_RambleKindRow i_ramble_kind_rows[8] = {
    { RAMBLE_ENTITY_TOPIC,    1, 1, ""     },   /* TOPIC    */
    { RAMBLE_ENTITY_FUNCTION, 1, 0, "@req" },   /* FUNC_REQ */
    { RAMBLE_ENTITY_FUNCTION, 0, 1, "@rsp" },   /* FUNC_RSP */
    { RAMBLE_ENTITY_VARIABLE, 1, 1, ""     },   /* VARIABLE */
    { RAMBLE_ENTITY_VARIABLE, 0, 0, "@set" },   /* VAR_SET  */
    { RAMBLE_ENTITY_TASK,     1, 0, "@req" },   /* TASK_REQ */
    { RAMBLE_ENTITY_TASK,     0, 1, "@prg" },   /* TASK_PRG */
    { RAMBLE_ENTITY_TASK,     0, 1, "@rsp" }    /* TASK_RSP */
};
static const i_RambleKindRow *i_ramble_kind_row(uint8_t kind){
    return kind < 8 ? &i_ramble_kind_rows[kind] : &i_ramble_kind_rows[0];
}

static int i_ramble_reflect_hidden(const char *name, size_t len){
    return len >= 8 && memcmp(name, "@ramble/", 8) == 0;
}
static int i_ramble_reflect_hidden_hash(uint32_t h){
    static const char *const nm[] = { "@ramble/log/error", "@ramble/log/warn", "@ramble/log/info",
                                      "@ramble/meta@req", "@ramble/meta@rsp" };
    size_t i;
    for (i = 0; i < sizeof nm / sizeof nm[0]; i++)
        if (h == (uint32_t)ramble_topic_id(nm[i])) return 1;
    return 0;
}

typedef struct { uint32_t hash; uint16_t index; } i_RambleHashPair;

static void *i_ramble_node_core_scratch(i_RambleNodeCore *c, uint32_t bytes){
    if (!i_ramble_node_core_array_reserve(c, &c->scratch, &c->scratch_cap, bytes, 1)) return NULL;
    return c->scratch;
}

static void i_ramble_hash_sort(i_RambleHashPair *a, uint32_t n){   /* shell sort, no libc */
    uint32_t gap, i, j;
    for (gap = n / 2u; gap > 0; gap /= 2u)
        for (i = gap; i < n; i++){
            i_RambleHashPair v = a[i];
            for (j = i; j >= gap && (a[j-gap].hash > v.hash
                                     || (a[j-gap].hash == v.hash && a[j-gap].index > v.index)); j -= gap)
                a[j] = a[j-gap];
            a[j] = v;
        }
}

/* the channel of kind whose name hashes like base plus suffix, NONE16 when absent */
static uint16_t i_ramble_reflect_partner(const i_RambleReflect *r, const i_RambleHashPair *sorted,
                                       uint32_t n, const char *base, size_t base_len,
                                       const char *suffix, uint8_t kind){
    char buf[RAMBLE_TOPIC_NAME_MAX + 8]; size_t sl = strlen(suffix);
    uint32_t want, lo = 0, hi = n;
    if (base_len + sl > RAMBLE_TOPIC_NAME_MAX) return I_RAMBLE_NONE16;
    memcpy(buf, base, base_len); memcpy(buf + base_len, suffix, sl + 1);
    want = (uint32_t)ramble_topic_id(buf);
    while (lo < hi){ uint32_t mid = (lo + hi) / 2u; if (sorted[mid].hash < want) lo = mid + 1; else hi = mid; }
    for (; lo < n && sorted[lo].hash == want; lo++){
        const i_RambleChannel *ch = &r->chan[sorted[lo].index];
        if (ch->kind != kind) continue;
        /* a fetched partner name must really be base plus suffix, 32 bit hashes collide */
        if (ch->name_off != I_RAMBLE_NAME_NONE
            && (ch->name_len != base_len + sl || memcmp(r->names + ch->name_off, buf, base_len + sl) != 0))
            continue;
        return sorted[lo].index;
    }
    return I_RAMBLE_NONE16;
}

static i_RamblePeerEntity *i_ramble_reflect_entity_new(i_RambleNodeCore *c, i_RambleReflect *r){
    i_RamblePeerEntity *e;
    if (!i_ramble_node_core_array_reserve(c, (void**)&r->ent, &r->cap_ent, r->n_ent + 1u, sizeof *r->ent))
        return NULL;
    e = &r->ent[r->n_ent++];
    memset(e, 0, sizeof *e);
    e->primary = e->rsp = e->prg = e->set = I_RAMBLE_NONE16;
    return e;
}

/* attaches channel idx as the entity's partner of its kind */
static void i_ramble_reflect_attach(i_RambleReflect *r, i_RamblePeerEntity *e, uint16_t idx, uint16_t slot){
    i_RambleChannel *ch = &r->chan[idx];
    ch->entity = slot;
    switch (ch->kind){
        case RAMBLE_KIND_FUNC_RSP: case RAMBLE_KIND_TASK_RSP: e->rsp = idx; break;
        case RAMBLE_KIND_TASK_PRG: e->prg = idx; break;
        case RAMBLE_KIND_VAR_SET:  e->set = idx; break;
        default: break;
    }
}

static void i_ramble_reflect_set_name(i_RamblePeerEntity *e, const i_RambleReflect *r, uint32_t off, size_t len){
    char buf[RAMBLE_TOPIC_NAME_MAX + 1];
    e->name_off = off; e->name_len = (uint8_t)len;
    memcpy(buf, r->names + off, len); buf[len] = '\0';
    e->id = ramble_topic_id(buf);
}

static void i_ramble_reflect_fold(i_RambleNodeCore *c, i_RambleReflect *r){
    i_RambleHashPair *sorted; uint32_t n = 0, i;
    r->dirty = 0;
    r->n_ent = 0;
    for (i = 0; i < r->n_chan; i++) r->chan[i].entity = I_RAMBLE_NONE16;
    sorted = (i_RambleHashPair*)i_ramble_node_core_scratch(c, r->n_chan * (uint32_t)sizeof *sorted + 1u);
    if (!sorted) return;
    for (i = 0; i < r->n_chan; i++)
        if (r->chan[i].present){ sorted[n].hash = r->chan[i].hash; sorted[n].index = (uint16_t)i; n++; }
    i_ramble_hash_sort(sorted, n);
    /* pass 1: primaries open entities and claim their partners */
    for (i = 0; i < r->n_chan; i++){
        i_RambleChannel *ch = &r->chan[i];
        const i_RambleKindRow *row;
        i_RamblePeerEntity *e;
        const char *nm; size_t nl;
        uint16_t slot, p;
        if (!ch->present || ch->kind >= 8 || i_ramble_reflect_hidden_hash(ch->hash)) continue;
        row = i_ramble_kind_row(ch->kind);
        if (!row->primary) continue;
        if (ch->name_off != I_RAMBLE_NAME_NONE
            && i_ramble_reflect_hidden(r->names + ch->name_off, ch->name_len)) continue;
        e = i_ramble_reflect_entity_new(c, r);
        if (!e) return;
        slot = (uint16_t)(r->n_ent - 1u);
        e->kind = row->entity; e->primary = (uint16_t)i; e->hash = ch->hash;
        ch->entity = slot;
        if (ch->name_off == I_RAMBLE_NAME_NONE){
            if (row->suffix[0]) e->incomplete = 1;   /* partners need the name */
            continue;
        }
        nm = r->names + ch->name_off; nl = ch->name_len;
        if (row->suffix[0]){
            size_t sl = strlen(row->suffix);
            if (nl > sl && memcmp(nm + nl - sl, row->suffix, sl) == 0) nl -= sl;
            else e->incomplete = 1;                  /* a pattern kind without the convention */
        }
        i_ramble_reflect_set_name(e, r, ch->name_off, nl);
        if (e->incomplete) continue;
        switch (ch->kind){
        case RAMBLE_KIND_FUNC_REQ:
            p = i_ramble_reflect_partner(r, sorted, n, nm, nl, "@rsp", RAMBLE_KIND_FUNC_RSP);
            if (p != I_RAMBLE_NONE16) i_ramble_reflect_attach(r, e, p, slot); else e->incomplete = 1;
            break;
        case RAMBLE_KIND_TASK_REQ:
            p = i_ramble_reflect_partner(r, sorted, n, nm, nl, "@rsp", RAMBLE_KIND_TASK_RSP);
            if (p != I_RAMBLE_NONE16) i_ramble_reflect_attach(r, e, p, slot); else e->incomplete = 1;
            p = i_ramble_reflect_partner(r, sorted, n, nm, nl, "@prg", RAMBLE_KIND_TASK_PRG);
            if (p != I_RAMBLE_NONE16) i_ramble_reflect_attach(r, e, p, slot); else e->incomplete = 1;
            break;
        case RAMBLE_KIND_VARIABLE:
            p = i_ramble_reflect_partner(r, sorted, n, nm, nl, "@set", RAMBLE_KIND_VAR_SET);
            if (p != I_RAMBLE_NONE16) i_ramble_reflect_attach(r, e, p, slot);
            break;
        default: break;
        }
    }
    /* pass 2: an unclaimed partner is a half pair or an unknown kind, surfaced incomplete */
    for (i = 0; i < r->n_chan; i++){
        i_RambleChannel *ch = &r->chan[i];
        const i_RambleKindRow *row;
        i_RamblePeerEntity *e;
        uint16_t slot;
        if (!ch->present || ch->entity != I_RAMBLE_NONE16 || i_ramble_reflect_hidden_hash(ch->hash)) continue;
        if (ch->name_off != I_RAMBLE_NAME_NONE
            && i_ramble_reflect_hidden(r->names + ch->name_off, ch->name_len)) continue;
        row = i_ramble_kind_row(ch->kind);
        e = i_ramble_reflect_entity_new(c, r);
        if (!e) return;
        slot = (uint16_t)(r->n_ent - 1u);
        e->kind = (ch->kind < 8) ? row->entity : RAMBLE_ENTITY_TOPIC;
        e->hash = ch->hash; e->incomplete = 1;
        if (ch->kind < 8 && !row->primary) i_ramble_reflect_attach(r, e, (uint16_t)i, slot);
        else { e->primary = (uint16_t)i; ch->entity = slot; }
        if (ch->name_off != I_RAMBLE_NAME_NONE){
            const char *nm = r->names + ch->name_off; size_t nl = ch->name_len;
            size_t sl = ch->kind < 8 ? strlen(row->suffix) : 0;
            if (sl && nl > sl && memcmp(nm + nl - sl, row->suffix, sl) == 0) nl -= sl;
            i_ramble_reflect_set_name(e, r, ch->name_off, nl);
        }
    }
}

/* the reflection record behind a peer id, RAMBLE_SELF is this node, NULL if unknown */
static i_RambleReflect *i_ramble_node_core_reflect_of(i_RambleNodeCore *c, uint32_t peer){
    i_RambleNodePeerExtra *ex;
    if (peer == RAMBLE_SELF) return &c->self;
    ex = c->discovery ? (i_RambleNodePeerExtra*)ramble_discovery_peer_user(c->discovery, peer) : NULL;
    return (ex && ex->added) ? &ex->refl : NULL;
}

/* the channel of an entity that which names: 0 primary, 1 rsp, 2 prg */
static const i_RambleChannel *i_ramble_reflect_which(const i_RambleReflect *r, const i_RamblePeerEntity *e, int which){
    uint16_t idx = which == 1 ? e->rsp : which == 2 ? e->prg : e->primary;
    return idx != I_RAMBLE_NONE16 ? &r->chan[idx] : NULL;
}

/* the channel that says which side a node is on: the primary, else whichever partner exists */
static const i_RambleChannel *i_ramble_reflect_side(const i_RambleReflect *r, const i_RamblePeerEntity *e){
    if (e->primary != I_RAMBLE_NONE16) return &r->chan[e->primary];
    if (e->rsp != I_RAMBLE_NONE16) return &r->chan[e->rsp];
    if (e->prg != I_RAMBLE_NONE16) return &r->chan[e->prg];
    if (e->set != I_RAMBLE_NONE16) return &r->chan[e->set];
    return NULL;
}

static uint64_t i_ramble_reflect_generation(const uint8_t uuid[16], const i_RambleReflect *r,
                                          const i_RamblePeerEntity *e){
    uint64_t h = 1469598103934665603ull;
    const i_RambleChannel *p = e->primary != I_RAMBLE_NONE16 ? &r->chan[e->primary] : NULL;
    uint64_t hs = p ? p->schema_hash : 0;
    uint64_t hr = e->rsp != I_RAMBLE_NONE16 ? r->chan[e->rsp].schema_hash : 0;
    uint64_t hp = e->prg != I_RAMBLE_NONE16 ? r->chan[e->prg].schema_hash : 0;
    uint8_t attrs = p ? p->attrs : 0, set = (uint8_t)(e->set != I_RAMBLE_NONE16);
    if (uuid) h = i_ramble_node_core_fnv(h, uuid, 16);
    h = i_ramble_node_core_fnv(h, &hs, sizeof hs);
    h = i_ramble_node_core_fnv(h, &hr, sizeof hr);
    h = i_ramble_node_core_fnv(h, &hp, sizeof hp);
    h = i_ramble_node_core_fnv(h, &attrs, 1);
    h = i_ramble_node_core_fnv(h, &set, 1);
    h = i_ramble_node_core_fnv(h, &e->kind, 1);
    return h;
}

/* the uuid behind a peer id, NULL if unknown. RAMBLE_SELF is our own */
static const uint8_t *i_ramble_node_core_uuid_of(i_RambleNodeCore *c, uint32_t peer, RambleDiscoveryPeer *scratch){
    uint16_t q, np;
    if (!c->discovery) return NULL;
    if (peer == RAMBLE_SELF) return ramble_discovery_uuid(c->discovery);
    np = ramble_discovery_max_peers(c->discovery);
    for (q = 0; q < np; q++)
        if (ramble_discovery_peer_at(c->discovery, q, scratch) && scratch->id == peer) return scratch->uuid;
    return NULL;
}

/* fills the public view of one entity slot at one node */
static void i_ramble_reflect_info(i_RambleNodeCore *c, uint32_t peer, const i_RambleReflect *r,
                                uint16_t slot, RambleEntityInfo *out){
    const i_RamblePeerEntity *e = &r->ent[slot];
    const i_RambleChannel *p = e->primary != I_RAMBLE_NONE16 ? &r->chan[e->primary] : NULL;
    const i_RambleChannel *side = i_ramble_reflect_side(r, e);
    RambleDiscoveryPeer scratch;
    memset(out, 0, sizeof *out);
    out->kind = (RambleEntityKind)e->kind;
    out->name = e->name_len ? ramble_string(r->names + e->name_off, e->name_len) : ramble_string(NULL, 0);
    out->hash = e->hash;
    out->incomplete = e->incomplete;
    if (side){
        const i_RambleKindRow *row = i_ramble_kind_row(side->kind);
        int pubs = ramble_role_pubs(side->role), subs = ramble_role_subs(side->role);
        out->provides = (uint8_t)(row->provider_pubs ? pubs : subs);
        out->consumes = (uint8_t)(row->provider_pubs ? subs : pubs);
        out->reliable = side->reliable;
    }
    if (p){
        out->schema = p->schema; out->schema_hash = p->schema_hash;
        out->forceable   = (uint8_t)((p->attrs & RAMBLE_ATTR_FORCEABLE)   ? 1 : 0);
        out->cancellable = (uint8_t)((p->attrs & RAMBLE_ATTR_CANCELLABLE) ? 1 : 0);
        out->exclusive   = (uint8_t)((p->attrs & RAMBLE_ATTR_EXCLUSIVE)   ? 1 : 0);
        out->multi       = (uint8_t)((p->attrs & RAMBLE_ATTR_MULTI)       ? 1 : 0);
    }
    if (e->rsp != I_RAMBLE_NONE16){ out->rsp_schema = r->chan[e->rsp].schema; out->rsp_schema_hash = r->chan[e->rsp].schema_hash; }
    if (e->prg != I_RAMBLE_NONE16){ out->progress_schema = r->chan[e->prg].schema; out->progress_schema_hash = r->chan[e->prg].schema_hash; }
    out->writable = (uint8_t)(e->kind == RAMBLE_ENTITY_VARIABLE && e->set != I_RAMBLE_NONE16);
    out->providers = out->provides; out->consumers = out->consumes;
    out->provider = peer;
    out->from = peer == RAMBLE_SELF ? c->self_name : i_ramble_node_core_peer_name(c, peer);
    out->generation = i_ramble_reflect_generation(i_ramble_node_core_uuid_of(c, peer, &scratch), r, e);
}

/* the mesh table */

typedef struct {
    uint64_t id; uint64_t last_heard; uint64_t schema_hash; const RambleSchema *schema;
    uint32_t peer; uint16_t slot; uint8_t kind, provides, consumes;
} i_RambleMeshRec;

static int i_ramble_mesh_rec_less(const i_RambleMeshRec *a, const i_RambleMeshRec *b){
    if (a->kind != b->kind) return a->kind < b->kind;
    if (a->id != b->id) return a->id < b->id;
    return a->peer < b->peer;
}
static void i_ramble_mesh_sort(i_RambleMeshRec *a, uint32_t n){
    uint32_t gap, i, j;
    for (gap = n / 2u; gap > 0; gap /= 2u)
        for (i = gap; i < n; i++){
            i_RambleMeshRec v = a[i];
            for (j = i; j >= gap && i_ramble_mesh_rec_less(&v, &a[j-gap]); j -= gap) a[j] = a[j-gap];
            a[j] = v;
        }
}

/* one node's entities as records. last_heard orders rival providers by freshness */
static uint32_t i_ramble_mesh_gather(i_RambleNodeCore *c, i_RambleMeshRec *recs, uint32_t n, uint32_t cap,
                                   uint32_t peer, i_RambleReflect *r, uint64_t last_heard){
    uint32_t i;
    if (r->dirty) i_ramble_reflect_fold(c, r);
    for (i = 0; i < r->n_ent && n < cap; i++){
        const i_RamblePeerEntity *e = &r->ent[i];
        const i_RambleChannel *side = i_ramble_reflect_side(r, e);
        const i_RambleKindRow *row;
        i_RambleMeshRec *m;
        if (!e->id || !side) continue;               /* unnamed yet: cannot key it */
        row = i_ramble_kind_row(side->kind);
        m = &recs[n++];
        m->id = e->id; m->kind = e->kind; m->peer = peer; m->slot = (uint16_t)i;
        m->last_heard = last_heard;
        m->provides = (uint8_t)(row->provider_pubs ? ramble_role_pubs(side->role) : ramble_role_subs(side->role));
        m->consumes = (uint8_t)(row->provider_pubs ? ramble_role_subs(side->role) : ramble_role_pubs(side->role));
        m->schema = e->primary != I_RAMBLE_NONE16 ? r->chan[e->primary].schema : NULL;
        m->schema_hash = e->primary != I_RAMBLE_NONE16 ? r->chan[e->primary].schema_hash : 0;
    }
    return n;
}

static void i_ramble_node_core_mesh_build(i_RambleNodeCore *c){
    i_RambleMeshRec *recs; uint32_t total = c->self.n_ent + 16u, n = 0, i;
    uint16_t s, np = c->discovery ? ramble_discovery_max_peers(c->discovery) : 0;
    c->mesh_dirty = 0;
    c->n_mesh = 0;
    if (c->self.dirty) i_ramble_reflect_fold(c, &c->self);
    total = c->self.n_ent + 16u;
    for (s = 0; s < np; s++){
        RambleDiscoveryPeer v; i_RambleNodePeerExtra *ex;
        if (!ramble_discovery_peer_at(c->discovery, s, &v) || v.liveness != RAMBLE_PEER_ACTIVE) continue;
        ex = (i_RambleNodePeerExtra*)v.user;
        if (!ex || !ex->added) continue;
        if (ex->refl.dirty) i_ramble_reflect_fold(c, &ex->refl);
        total += ex->refl.n_ent;
    }
    recs = (i_RambleMeshRec*)i_ramble_node_core_scratch(c, total * (uint32_t)sizeof *recs);
    if (!recs) return;
    n = i_ramble_mesh_gather(c, recs, n, total, RAMBLE_SELF, &c->self, ~(uint64_t)0);
    for (s = 0; s < np; s++){
        RambleDiscoveryPeer v; i_RambleNodePeerExtra *ex;
        if (!ramble_discovery_peer_at(c->discovery, s, &v) || v.liveness != RAMBLE_PEER_ACTIVE) continue;
        ex = (i_RambleNodePeerExtra*)v.user;
        if (!ex || !ex->added) continue;
        n = i_ramble_mesh_gather(c, recs, n, total, v.id, &ex->refl, v.last_heard_us);
    }
    i_ramble_mesh_sort(recs, n);
    if (!i_ramble_node_core_array_reserve(c, (void**)&c->mesh, &c->cap_mesh, n ? n : 1u, sizeof *c->mesh)) return;
    for (i = 0; i < n; ){
        i_RambleMeshEntity *m = &c->mesh[c->n_mesh++];
        const i_RambleMeshRec *pick = NULL, *first_consumer = NULL;
        uint32_t j, k;
        memset(m, 0, sizeof *m);
        m->id = recs[i].id; m->kind = recs[i].kind;
        for (j = i; j < n && recs[j].kind == m->kind && recs[j].id == m->id; j++){
            const i_RambleMeshRec *rec = &recs[j];
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
                const i_RambleMeshRec *rec = &recs[k];
                if (!rec->consumes || !rec->schema) continue;
                if (!pick->schema || (rec->schema_hash != pick->schema_hash
                                      && ramble_schema_subset(pick->schema, rec->schema))) pick = rec;
            }
        }
        if (pick){
            m->from_peer = pick->peer; m->from_slot = pick->slot;
            /* a live endpoint that can neither read the pick nor be read by it is a conflict */
            for (k = i; k < j; k++){
                const i_RambleMeshRec *rec = &recs[k];
                if (rec == pick || rec->schema_hash == pick->schema_hash || !rec->schema || !pick->schema) continue;
                if (!ramble_schema_subset(rec->schema, pick->schema) && !ramble_schema_subset(pick->schema, rec->schema))
                    m->conflict = 1;
            }
        }
        {   i_RambleReflect *r = i_ramble_node_core_reflect_of(c, m->from_peer);
            RambleDiscoveryPeer scratch;
            const uint8_t *uuid = m->has_provider ? i_ramble_node_core_uuid_of(c, m->provider, &scratch) : NULL;
            m->generation = r ? i_ramble_reflect_generation(uuid, r, &r->ent[m->from_slot]) : 0;
        }
        i = j;
    }
}

static void i_ramble_mesh_info(i_RambleNodeCore *c, const i_RambleMeshEntity *m, RambleEntityInfo *out){
    i_RambleReflect *r = i_ramble_node_core_reflect_of(c, m->from_peer);
    if (!r){ memset(out, 0, sizeof *out); return; }
    i_ramble_reflect_info(c, m->from_peer, r, m->from_slot, out);
    out->providers = m->providers; out->consumers = m->consumers;
    out->provides = (uint8_t)(m->providers > 0); out->consumes = (uint8_t)(m->consumers > 0);
    out->provider = m->has_provider ? m->provider : 0;
    out->conflict = m->conflict;
    out->generation = m->generation;
}

/* the seams the runtime's walks call, with the node lock held by the caller */

void i_ramble_node_core_set_self_name(i_RambleNodeCore *c, RambleString name){ if (c) c->self_name = name; }

void i_ramble_node_core_self_begin(i_RambleNodeCore *c){
    uint32_t i;
    if (!c) return;
    for (i = 0; i < c->self.n_chan; i++) c->self.chan[i].present = 0;
    c->self.names_len = 0;
}
void i_ramble_node_core_self_channel(i_RambleNodeCore *c, uint16_t index, RambleString name, uint8_t kind,
                                   uint8_t role, uint8_t reliable, uint8_t attrs, const RambleSchema *schema){
    i_RambleChannel *ch;
    char buf[RAMBLE_TOPIC_NAME_MAX + 1];
    if (!c || name.len == 0 || name.len > RAMBLE_TOPIC_NAME_MAX) return;
    ch = i_ramble_reflect_channel(c, &c->self, index);
    if (!ch) return;
    memcpy(buf, name.data, name.len); buf[name.len] = '\0';
    ch->hash = (uint32_t)ramble_topic_id(buf);
    ch->name_off = i_ramble_reflect_name(c, &c->self, name);
    ch->name_len = (uint8_t)name.len;
    ch->kind = kind; ch->role = role; ch->reliable = reliable; ch->attrs = attrs;
    ch->schema = schema; ch->schema_hash = schema ? ramble_schema_hash(schema) : 0;
    ch->present = (uint8_t)(role != RAMBLE_INACTIVE && ch->name_off != I_RAMBLE_NAME_NONE);
}
void i_ramble_node_core_self_end(i_RambleNodeCore *c){
    if (!c) return;
    c->self.dirty = 1;
    i_ramble_node_core_mesh_bump(c);
}

int i_ramble_node_core_peers_next(i_RambleNodeCore *c, RambleIter *it, RamblePeerInfo *out){
    uint16_t np;
    if (!c || !c->discovery || !it || !out) return 0;
    np = ramble_discovery_max_peers(c->discovery);
    for (; it->a < np; it->a++){
        RambleDiscoveryPeer v; i_RambleNodePeerExtra *ex;
        if (!ramble_discovery_peer_at(c->discovery, (uint16_t)it->a, &v)) continue;
        ex = (i_RambleNodePeerExtra*)v.user;
        it->a++;
        memset(out, 0, sizeof *out);
        out->id = v.id;
        memcpy(out->uuid, v.uuid, 16);
        out->name = i_ramble_node_core_peer_name(c, v.id);
        out->address = ex ? ramble_string(ex->refl.addr, ex->refl.addr_len) : ramble_string(NULL, 0);
        out->liveness = v.liveness;
        out->last_heard_us = v.last_heard_us;
        out->epoch = ex ? ex->interest_epoch : 0;
        out->catching_up = (uint8_t)(v.adv_meta_version > v.meta_version);
        out->fragment_size = v.meta.data ? ramble_meta_frag(v.meta) : 0;
        return 1;
    }
    return 0;
}

int i_ramble_node_core_entities_next(i_RambleNodeCore *c, uint32_t peer, RambleIter *it, RambleEntityInfo *out){
    i_RambleReflect *r;
    if (!c || !it || !out) return 0;
    r = i_ramble_node_core_reflect_of(c, peer);
    if (!r) return 0;
    if (r->dirty) i_ramble_reflect_fold(c, r);
    if (it->a >= r->n_ent) return 0;
    i_ramble_reflect_info(c, peer, r, (uint16_t)it->a, out);
    it->a++;
    return 1;
}

int i_ramble_node_core_mesh_next(i_RambleNodeCore *c, RambleIter *it, RambleEntityInfo *out){
    if (!c || !it || !out) return 0;
    if (c->mesh_dirty) i_ramble_node_core_mesh_build(c);
    if (it->a >= c->n_mesh) return 0;
    i_ramble_mesh_info(c, &c->mesh[it->a], out);
    it->a++;
    return 1;
}

static const i_RambleMeshEntity *i_ramble_node_core_mesh_lookup(i_RambleNodeCore *c, uint8_t kind, uint64_t id){
    uint32_t lo = 0, hi;
    if (c->mesh_dirty) i_ramble_node_core_mesh_build(c);
    hi = c->n_mesh;
    while (lo < hi){
        uint32_t mid = (lo + hi) / 2u;
        const i_RambleMeshEntity *m = &c->mesh[mid];
        if (m->kind < kind || (m->kind == kind && m->id < id)) lo = mid + 1; else hi = mid;
    }
    return (lo < c->n_mesh && c->mesh[lo].kind == kind && c->mesh[lo].id == id) ? &c->mesh[lo] : NULL;
}

int i_ramble_node_core_mesh_find(i_RambleNodeCore *c, RambleEntityKind kind, const char *name, RambleEntityInfo *out){
    const i_RambleMeshEntity *m;
    if (!c || !name || !out) return 0;
    m = i_ramble_node_core_mesh_lookup(c, (uint8_t)kind, ramble_topic_id(name));
    if (!m) return 0;
    i_ramble_mesh_info(c, m, out);
    return 1;
}

uint32_t i_ramble_node_core_mesh_epoch(i_RambleNodeCore *c){ return c ? c->mesh_epoch : 0; }

int i_ramble_node_core_reflect_pick(i_RambleNodeCore *c, RambleEntityKind kind, const char *name, int which,
                                  int writer, const RambleSchema **schema, uint8_t *reliable,
                                  uint64_t *generation){
    const i_RambleMeshEntity *m;
    const RambleSchema *best = NULL; uint64_t best_hash = 0;
    uint8_t rel = 0; int found = 0;
    uint64_t id;
    uint16_t s, np;
    if (schema) *schema = NULL;
    if (reliable) *reliable = 0;
    if (generation) *generation = 0;
    if (!c || !name) return 0;
    id = ramble_topic_id(name);
    m = i_ramble_node_core_mesh_lookup(c, (uint8_t)kind, id);
    if (!m) return 0;
    if (generation) *generation = m->generation;
    /* a reader takes the provider's declaration for this channel */
    if (!writer && m->has_provider){
        i_RambleReflect *r = i_ramble_node_core_reflect_of(c, m->provider);
        const i_RambleChannel *ch = r ? i_ramble_reflect_which(r, &r->ent[m->provider_slot], which) : NULL;
        if (ch){
            if (schema) *schema = ch->schema;
            if (reliable) *reliable = ch->reliable;
            return 1;
        }
    }
    /* a writer, or a reader with no provider: the widest live declaration, reliable if
       any reader of the channel requests it */
    np = c->discovery ? ramble_discovery_max_peers(c->discovery) : 0;
    for (s = 0; s <= np; s++){
        i_RambleReflect *r; uint32_t i;
        if (s == np) r = &c->self;
        else {
            RambleDiscoveryPeer v; i_RambleNodePeerExtra *ex;
            if (!ramble_discovery_peer_at(c->discovery, s, &v) || v.liveness != RAMBLE_PEER_ACTIVE) continue;
            ex = (i_RambleNodePeerExtra*)v.user;
            if (!ex || !ex->added) continue;
            r = &ex->refl;
        }
        if (r->dirty) i_ramble_reflect_fold(c, r);
        for (i = 0; i < r->n_ent; i++){
            const i_RamblePeerEntity *e = &r->ent[i];
            const i_RambleChannel *ch;
            if (e->kind != (uint8_t)kind || e->id != id) continue;
            ch = i_ramble_reflect_which(r, e, which);
            if (!ch) continue;
            found = 1;
            if (ramble_role_subs(ch->role) && ch->reliable) rel = 1;
            if (!ch->schema) continue;
            if (!best || (ch->schema_hash != best_hash && ramble_schema_subset(best, ch->schema))){
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
static i_RambleNodePeerExtra *i_ramble_node_core_peer_extra(i_RambleNodeCore *c, uint32_t id);
static uint16_t i_ramble_node_core_greedy_extend(i_RambleNodeCore *c, uint32_t peer,
                            RambleBytes interest, RambleDetailWant *wants, uint16_t n,
                            uint16_t max_wants){
    RambleInterestIter it; RambleTopicEntry e;
    uint16_t k;
    memset(&it, 0, sizeof it);
    while (n < max_wants && ramble_interest_next(interest, &it, &e)){
        /* the iterator skips INACTIVE entries and hole runs, a PUBSUB double yield dedupes below */
        {   i_RambleNodePeerExtra *ex = i_ramble_node_core_peer_extra(c, peer);
            if (ex && e.index < ex->refl.n_chan && ex->refl.chan[e.index].name_off != I_RAMBLE_NAME_NONE) continue; }
        for (k = 0; k < n; k++) if (wants[k].index == e.index) break;
        if (k < n) continue;
        wants[n].index = e.index;
        wants[n].schema_hash = 0;   /* force the wire inline, we may not hold that schema */
        n++;
    }
    return n;
}

/* why the intern returned NULL, as text */
static char *i_ramble_node_core_intern_why(i_RambleNodeCore *c, RambleBytes wire, char *p, char *end){
    if (!c->alloc)
        return i_ramble_event_append_str(p, end, "no allocator here to parse peer schemas");
    if (!wire.data || wire.len == 0)
        return i_ramble_event_append_str(p, end,
            "their schema wire is unavailable (not inlined in the detail response)");
    p = i_ramble_event_append_str(p, end, "their schema wire was rejected: malformed, "
            "hash-mismatched, or a different Ramble schema wire version than ours (v");
    p = i_ramble_event_append_u64(p, end, RAMBLE_SCHEMA_WIRE_VERSION);
    return i_ramble_event_append_str(p, end, ")");
}

/* The verdict. A refusal writes its reason into [p, end], where end is the last writable
 * byte, and p == end means no text. The wrapper below records or clears the reason. */
static int i_ramble_node_core_schema_verdict(i_RambleNodeCore *c, uint32_t peer, uint16_t topic_index,
                                           int peer_is_pub, uint64_t hash, RambleBytes wire,
                                           char *p, char *end){
    const RambleSchema *ours = (topic_index < c->n_topics) ? c->chan_compiled[topic_index] : NULL;
    int peer_has = hash != 0;
    if (peer_is_pub){                                   /* their publish side: we would read */
        if (!ours){                                     /* a generic reader decodes with theirs */
            i_ramble_node_core_peer_schema_set(c, peer, topic_index,
                peer_has ? i_ramble_node_core_intern(c, hash, wire) : NULL);
            return 1;
        }
        if (!peer_has){                                 /* a typed reader refuses untyped */
            p = i_ramble_event_append_str(p, end, "their writer has no schema, our typed reader refuses");
            *p = '\0'; return 0;
        }
        if (hash == ramble_schema_hash(ours)){            /* identical: our own view works */
            i_ramble_node_core_peer_schema_set(c, peer, topic_index, i_RAMBLE_NODE_SCHEMA_OURS);
            return 1;
        }
        {   RambleSchema *pub = i_ramble_node_core_intern(c, hash, wire);   /* the wire verifies it */
            RambleSchema *view;
            if (!pub){
                p = i_ramble_node_core_intern_why(c, wire, p, end);
                *p = '\0'; return 0;
            }
            if (!ramble_schema_subset_why(ours, pub, p, (size_t)(end - p) + 1u)) return 0;
            view = i_ramble_node_core_bind(c, hash, topic_index, ours, pub);
            if (!view){                                 /* OOM: refuse rather than misdecode */
                p = i_ramble_event_append_str(p, end, "out of memory binding the reader view");
                *p = '\0'; return 0;
            }
            i_ramble_node_core_peer_schema_set(c, peer, topic_index, view);
            return 1;
        }
    } else {                                            /* their subscribe side: we would write */
        if (!peer_has) return 1;                        /* a generic reader takes anything */
        if (!ours){                                     /* a typed reader refuses our raw topic */
            p = i_ramble_event_append_str(p, end, "their reader is typed, our topic has no schema");
            *p = '\0'; return 0;
        }
        if (hash == ramble_schema_hash(ours)) return 1;
        {   RambleSchema *sub = i_ramble_node_core_intern(c, hash, wire);
            if (!sub){
                p = i_ramble_node_core_intern_why(c, wire, p, end);
                *p = '\0'; return 0;
            }
            return ramble_schema_subset_why(sub, ours, p, (size_t)(end - p) + 1u);
        }
    }
}

int i_ramble_node_core_schema_check(i_RambleNodeCore *c, uint32_t peer, uint16_t topic_index,
                                  int peer_is_pub, uint64_t hash, RambleBytes wire){
#ifndef RAMBLE_NO_DIAG
    char why[RAMBLE__SCHEMA_WHY_MAX];
    int ok;
    why[0] = '\0';
    ok = i_ramble_node_core_schema_verdict(c, peer, topic_index, peer_is_pub, hash, wire,
                                         why, why + sizeof why - 1);
    if (ok) i_ramble_node_core_schema_why_drop(c, peer, topic_index, peer_is_pub);
    else    i_ramble_node_core_schema_why_set (c, peer, topic_index, peer_is_pub, why);
    return ok;
#else
    char why[1];
    return i_ramble_node_core_schema_verdict(c, peer, topic_index, peer_is_pub, hash, wire, why, why);
#endif
}

/* Rebuilds our overlay from the core's fields. The whole announce must fit one datagram,
   else the bootstrap form goes out and peers page the interest. See spec/interest.md. */
uint16_t i_ramble_node_core_build_meta(i_RambleNodeCore *c){
    uint16_t need = ramble_transport_meta_size(c->transport);
    int external = ((size_t)RAMBLE_DISCOVERY_META_OFF + RAMBLE_DISCOVERY_DISC_MAX + need
                    > (size_t)RAMBLE_DGRAM_MAX);
    if (external) need = ramble_transport_meta_bootstrap_size();
    if (need > c->meta_cap){   /* size the blob buffer to the exact content */
        uint8_t *nb = (uint8_t*)c->alloc(c->alloc_user, c->meta_buf, need);
        if (!nb) return c->meta_len;   /* OOM: keep the previous blob, stale but consistent */
        c->meta_buf = nb; c->meta_cap = need;
    }
    c->meta_len = ramble_transport_meta_build(c->transport, c->meta_buf, c->meta_cap,
                                  c->frag_size, c->oob_capable, c->oob_host, external);
    return c->meta_len;
}

RambleBytes i_ramble_node_core_meta(i_RambleNodeCore *c){
    return ramble_bytes(c->meta_buf, c->meta_len);
}

/* Answers a DETAIL_REQ into the core's grown scratch. One page per response, so it never
   IP fragments. A header only response still tells the requester the indices are gone. */
RambleBytes i_ramble_node_core_detail_respond(i_RambleNodeCore *c, uint16_t domain, RambleBytes req){
    size_t need, len;
    if (!c || !c->alloc || !c->discovery) return ramble_bytes(NULL, 0);
    if (ramble_detail_kind(req) != RAMBLE_DETAIL_REQ || ramble_detail_domain(req) != domain)
        return ramble_bytes(NULL, 0);
    need = ramble_transport_detail_resp_size(c->transport, c->chan_schemas, req);
    if (!need) return ramble_bytes(NULL, 0);
    if (need > c->detail_cap){
        uint8_t *nb = (uint8_t*)c->alloc(c->alloc_user, c->detail_buf, need);
        if (!nb) return ramble_bytes(NULL, 0);
        c->detail_buf = nb; c->detail_cap = (uint32_t)need;
    }
    len = ramble_transport_detail_respond(c->transport, c->chan_schemas,
                                        ramble_discovery_meta_version(c->discovery), req,
                                        c->detail_buf, need);
    return ramble_bytes(c->detail_buf, len);
}

#ifdef RAMBLE_SHM
/* A peer can receive our shared memory payload iff we are capable, it advertised a host
 * id, and that host equals ours. The core knows nothing of SHM beyond this. */
static void i_ramble_node_core_set_peer_oob(i_RambleNodeCore *c, uint32_t id, RambleBytes meta){
    uint8_t host[16];
    int oob = c->oob_capable && ramble_meta_shm(meta, host) &&
              memcmp(host, c->oob_host, 16) == 0;
    ramble_transport_peer_set_shm(c->transport, id, oob);
}
#else
#define i_ramble_node_core_set_peer_oob(c, id, meta) ((void)0)
#endif

/* fires PEER_UP or PEER_DOWN */
static void i_ramble_node_core_fire(i_RambleNodeCore *c, RambleEventKind kind, uint32_t id,
                            const RambleDiscoveryAddr *addr){
    RambleEvent ev;
    if (!c->on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind; ev.peer = id; ev.user = c->user;
    if (addr){ memcpy(ev.ip, addr->ip, 16); ev.ip_len = addr->ip_len; ev.port = addr->port; }
    c->on_event(&ev);
}

/* fires a RAMBLE_ERROR. too_big carries the OOM or meta too big byte count */
static void i_ramble_node_core_fire_error(i_RambleNodeCore *c, RambleErrorKind err, uint32_t id,
                            const RambleDiscoveryAddr *addr, uint64_t too_big){
    RambleEvent ev;
    if (!c->on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = RAMBLE_ERROR; ev.error = err; ev.peer = id; ev.user = c->user; ev.too_big_bytes = too_big;
    if (addr){ memcpy(ev.ip, addr->ip, 16); ev.ip_len = addr->ip_len; ev.port = addr->port; }
    c->on_event(&ev);
}

/* Every reflected interest change funnels through here, so this is also where the peer's
 * interest epoch bumps. It must bump even with no event handler. */
static void i_ramble_node_core_fire_interest(i_RambleNodeCore *c, uint32_t id){
    RambleEvent ev; uint16_t publish_to = 0, receive_from = 0;
    {   i_RambleNodePeerExtra *ex = c->discovery
            ? (i_RambleNodePeerExtra*)ramble_discovery_peer_user(c->discovery, id) : NULL;
        if (ex){
            uint32_t mv = 0; RambleBytes meta = ramble_discovery_peer_meta(c->discovery, id, &mv);
            ex->interest_epoch++;
            i_ramble_node_core_reflect_interest(c, ex, i_ramble_node_core_interest_of(ex, meta, mv));
            i_ramble_node_core_mesh_bump(c);
        }
    }
    if (!c->on_event) return;
    ramble_transport_peer_match_counts(c->transport, id, &publish_to, &receive_from);
    memset(&ev, 0, sizeof ev);
    ev.kind = RAMBLE_PEER_INTEREST; ev.peer = id;
    ev.publish_topics = publish_to; ev.receive_topics = receive_from;
    ev.user = c->user;
    c->on_event(&ev);
}

/* the peer's lifecycle state in the discovery scratch. NULL only before discovery is bound */
static i_RambleNodePeerExtra *i_ramble_node_core_peer_extra(i_RambleNodeCore *c, uint32_t id){
    return (i_RambleNodePeerExtra*)ramble_discovery_peer_user(c->discovery, id);
}

/* The one read point for a peer's interest: the inline section, or the assembled external
 * blob at the advertised version, or {NULL,0} while a fetch is in flight. */
static RambleBytes i_ramble_node_core_interest_of(i_RambleNodePeerExtra *ex, RambleBytes meta,
                            uint32_t meta_version){
    if (!meta.data) return ramble_bytes(NULL, 0);
    if (!ramble_meta_interest_external(meta)) return ramble_meta_interest(meta);
    if (ex && ex->interest_buf && ex->interest_version == meta_version)
        return ramble_bytes(ex->interest_buf, ex->fetch_len);
    return ramble_bytes(NULL, 0);
}

/* Queues an INTEREST_REQ for an external peer whose blob is not assembled at its current
 * version. The version dedup makes a steady state announce free. */
static void i_ramble_node_core_interest_check(i_RambleNodeCore *c, i_RambleNodePeerExtra *ex,
                            RambleBytes meta, uint32_t meta_version){
    if (!ex || !meta.data || !ramble_meta_interest_external(meta)) return;
    if (ex->interest_version == meta_version) return;
    ex->interest_due = 1;
    c->detail_due_any = 1;
}

/* After an apply: queue a DETAIL_REQ while unverified candidates remain. Runs on every
   apply, so the pending state is the retry state and a lost datagram heals. */
static void i_ramble_node_core_detail_check(i_RambleNodeCore *c, i_RambleNodePeerExtra *ex,
                            uint32_t id, RambleBytes interest){
    RambleDetailWant probe;
    if (!interest.data) return;
    if (ramble_transport_detail_wants(c->transport, c->chan_schemas, id, interest, NULL, 0)
        || (c->fetch_details
            && i_ramble_node_core_greedy_extend(c, id, interest, &probe, 0, 1))){
        ex->detail_due = 1;
        c->detail_due_any = 1;
    }
}

static void i_ramble_node_core_peer_up(i_RambleNodeCore *c, uint32_t id, const RambleDiscoveryAddr *addr,
                            RambleBytes meta){
    i_RambleNodePeerExtra *ex = i_ramble_node_core_peer_extra(c, id);
    uint16_t frag = ramble_meta_frag(meta);
    uint32_t meta_version = 0;
    RambleBytes interest;
    if (!ex) return;                                  /* discovery not bound */
    ramble_discovery_peer_meta(c->discovery, id, &meta_version);
    interest = i_ramble_node_core_interest_of(ex, meta, meta_version);
    if (!ex->added){                                  /* a new peer: wire it into the transport */
        i_ramble_node_core_peer_schema_clear(c, id);    /* a recycled id: no stale bindings */
        i_ramble_reflect_free(c, &ex->refl);
        i_ramble_node_core_schema_why_clear(c, id);
        ramble_transport_peer_add(c->transport, id, frag);
        ex->added = 1; ex->dormant = 0; ex->detail_due = 0;
        if (addr) i_ramble_reflect_format_addr(&ex->refl, addr);
        i_ramble_node_core_mesh_bump(c);
        i_ramble_node_core_set_peer_oob(c, id, meta);
        i_ramble_node_core_fire(c, RAMBLE_PEER_UP, id, addr);
        if (interest.data){ ramble_transport_apply_peer_interest(c->transport, id, interest);
                       i_ramble_node_core_fire_interest(c, id);
                       i_ramble_node_core_detail_check(c, ex, id, interest); }
    } else {                                          /* a known peer: an update */
        ramble_transport_peer_set_frag(c->transport, id, frag);
        i_ramble_node_core_set_peer_oob(c, id, meta);
        if (interest.data){ ramble_transport_apply_peer_interest(c->transport, id, interest);
                       i_ramble_node_core_fire_interest(c, id);
                       i_ramble_node_core_detail_check(c, ex, id, interest); }
        if (ex->dormant){    /* a DROPPED peer's same incarnation returned: resume */
            ex->dormant = 0;
            ramble_transport_peer_resume(c->transport, id);
            i_ramble_node_core_mesh_bump(c);
            i_ramble_node_core_fire(c, RAMBLE_PEER_UP, id, addr);
        }
    }
    i_ramble_node_core_interest_check(c, ex, meta, meta_version);   /* external and stale: pull it */
}

/* Ingests a DETAIL_RESP: caches the verdicts, then re applies the peer's current interest
   so they form their matches exactly as a fresh announce would. Idempotent. */
void i_ramble_node_core_apply_details(i_RambleNodeCore *c, uint16_t domain, uint32_t peer,
                            RambleBytes resp){
    i_RambleNodePeerExtra *ex;
    RambleBytes meta, interest;
    if (!c || ramble_detail_kind(resp) != RAMBLE_DETAIL_RESP || ramble_detail_domain(resp) != domain)
        return;
    ex = i_ramble_node_core_peer_extra(c, peer);
    if (!ex || !ex->added) return;
    {   /* a response older than the blob we hold may describe a binding the peer has since
           rebound: refuse it, the pending state re asks */
        uint32_t held = 0;
        ramble_discovery_peer_meta(c->discovery, peer, &held);
        if (held && ramble_detail_meta_version(resp) < held) return;
    }
    {   /* every detail received feeds reflection */
        RambleDetailIter it; RambleDetail dd;
        memset(&it, 0, sizeof it);
        while (ramble_detail_next(resp, &it, &dd)) i_ramble_node_core_reflect_detail(c, ex, &dd);
        if (ex->refl.dirty) i_ramble_node_core_mesh_bump(c);
    }
    if (!ramble_transport_apply_peer_details(c->transport, peer, resp)) return;   /* nothing new */
    {   uint32_t meta_version = 0;
        meta = ramble_discovery_peer_meta(c->discovery, peer, &meta_version);
        interest = i_ramble_node_core_interest_of(ex, meta, meta_version);
    }
    if (!interest.data) return;
    ramble_transport_apply_peer_interest(c->transport, peer, interest);
    i_ramble_node_core_fire_interest(c, peer);
    i_ramble_node_core_detail_check(c, ex, peer, interest);
}

/* Queues a request for every active peer: the periodic retry sweep. A converged peer
   costs one wants walk and sends nothing. */
void i_ramble_node_core_detail_rearm(i_RambleNodeCore *c){
    uint16_t s, n;
    if (!c || !c->discovery) return;
    n = ramble_discovery_max_peers(c->discovery);
    for (s=0;s<n;s++){
        RambleDiscoveryPeer v; i_RambleNodePeerExtra *ex;
        if (!ramble_discovery_peer_at(c->discovery, s, &v)) continue;
        if (v.liveness != RAMBLE_PEER_ACTIVE) continue;
        ex = (i_RambleNodePeerExtra*)v.user;
        if (ex && ex->added){
            ex->detail_due = 1; c->detail_due_any = 1;
            i_ramble_node_core_interest_check(c, ex, v.meta, v.meta_version);
        }
    }
}

int i_ramble_node_core_detail_any(i_RambleNodeCore *c){ return c ? c->detail_due_any : 0; }

/* A request from peer named our blob version: proof it applied our announce at it. When
 * the advance releases a held writer lane, the interest event re fires. */
int i_ramble_node_core_seen_version(i_RambleNodeCore *c, uint32_t peer, uint32_t version){
    if (!c || !version) return 0;
    if (!ramble_transport_peer_seen_version(c->transport, peer, version)) return 0;
    i_ramble_node_core_fire_interest(c, peer);
    return 1;
}

/* A peer counts once while its interest is unknown (no blob yet, or a fetch in flight),
 * else by its unresolved entries. Dropped peers are skipped, they will not answer. */
int i_ramble_node_core_topic_unresolved(i_RambleNodeCore *c, uint16_t topic_index){
    uint16_t s, n; int cnt = 0;
    if (!c || !c->discovery) return 0;
    n = ramble_discovery_max_peers(c->discovery);
    for (s=0;s<n;s++){
        RambleDiscoveryPeer v; i_RambleNodePeerExtra *ex;
        RambleBytes interest;
        if (!ramble_discovery_peer_at(c->discovery, s, &v)) continue;
        if (v.liveness != RAMBLE_PEER_ACTIVE) continue;
        ex = (i_RambleNodePeerExtra*)v.user;
        if (!ex || !ex->added) continue;
        if (!v.meta.data){ cnt++; continue; }   /* the blob is still being fetched */
        interest = i_ramble_node_core_interest_of(ex, v.meta, v.meta_version);
        if (!interest.data){
            if (ramble_meta_interest_external(v.meta)) cnt++;   /* a fetch in flight: resolving */
            continue;                           /* else no interest: nothing to resolve */
        }
        cnt += ramble_transport_topic_unresolved(c->transport, topic_index, v.id, interest);
    }
    return cnt;
}

void i_ramble_node_core_topics_unresolved(i_RambleNodeCore *c, uint16_t *counts, uint16_t n){
    uint16_t s, np, i;
    if (!counts || !n) return;
    memset(counts, 0, (size_t)n * sizeof *counts);
    if (!c || !c->discovery) return;
    np = ramble_discovery_max_peers(c->discovery);
    for (s=0;s<np;s++){
        RambleDiscoveryPeer v; i_RambleNodePeerExtra *ex;
        RambleBytes interest = ramble_bytes(NULL, 0); int all = 0;
        if (!ramble_discovery_peer_at(c->discovery, s, &v)) continue;
        if (v.liveness != RAMBLE_PEER_ACTIVE) continue;
        ex = (i_RambleNodePeerExtra*)v.user;
        if (!ex || !ex->added) continue;
        if (!v.meta.data) all = 1;                        /* the blob is still being fetched */
        else {
            interest = i_ramble_node_core_interest_of(ex, v.meta, v.meta_version);
            if (!interest.data && ramble_meta_interest_external(v.meta)) all = 1;   /* fetching */
        }
        if (all){   /* its interest is unknown, so it may nominate any topic */
            for (i=0;i<n;i++) if (counts[i] != 0xFFFFu) counts[i]++;
            continue;
        }
        if (!interest.data) continue;                     /* no interest: nothing to resolve */
        ramble_transport_peer_unresolved_fill(c->transport, v.id, interest, counts, n);
    }
}

/* Drains one queued request: INTEREST_REQs first, since an unassembled interest gates
   candidate discovery, then DETAIL_REQs, capped at 128 wants. Loop until 0. */
size_t i_ramble_node_core_detail_req_next(i_RambleNodeCore *c, uint16_t domain,
                            void *out, size_t cap, i_RambleNodeDest *to){
    RambleDetailWant wants[128];
    uint16_t s, n;
    if (!c || !c->discovery || cap < 24u) return 0;
    n = ramble_discovery_max_peers(c->discovery);
    for (s=0;s<n;s++){    /* the interest pass: one INTEREST_REQ per due peer, cursor driven */
        RambleDiscoveryPeer v; i_RambleNodePeerExtra *ex;
        uint32_t offset;
        if (!ramble_discovery_peer_at(c->discovery, s, &v)) continue;
        ex = (i_RambleNodePeerExtra*)v.user;
        if (!ex || !ex->interest_due) continue;
        ex->interest_due = 0;
        if (!ex->added || v.liveness != RAMBLE_PEER_ACTIVE) continue;
        if (!ramble_meta_interest_external(v.meta)) continue;    /* the flag cleared meanwhile */
        if (ex->interest_version == v.meta_version) continue;    /* assembled meanwhile */
        offset = (ex->fetch_version == v.meta_version) ? ex->fetch_cursor : 0;
        if (!i_ramble_node_core_resolve(c, v.id, to)) continue;
        {   size_t len = ramble_interest_req_build(domain, v.meta_version, offset, out, cap);
            if (len) return len;
        }
    }
    for (s=0;s<n;s++){
        RambleDiscoveryPeer v; i_RambleNodePeerExtra *ex;
        RambleBytes interest; uint16_t nw, maxw;
        if (!ramble_discovery_peer_at(c->discovery, s, &v)) continue;
        ex = (i_RambleNodePeerExtra*)v.user;
        if (!ex || !ex->detail_due) continue;
        ex->detail_due = 0;
        if (!ex->added || v.liveness != RAMBLE_PEER_ACTIVE) continue;
        interest = i_ramble_node_core_interest_of(ex, v.meta, v.meta_version);
        if (!interest.data) continue;
        maxw = (uint16_t)((cap - 14u) / 10u);
        if (maxw > 128u) maxw = 128u;
        nw = ramble_transport_detail_wants(c->transport, c->chan_schemas, v.id, interest,
                                         wants, maxw);
        if (c->fetch_details)
            nw = i_ramble_node_core_greedy_extend(c, v.id, interest, wants, nw, maxw);
        if (!nw) continue;
        if (!i_ramble_node_core_resolve(c, v.id, to)) continue;
        {   size_t len = ramble_detail_req_build(domain, v.meta_version, wants, nw, out, cap);
            if (len) return len;
        }
    }
    c->detail_due_any = 0;
    return 0;
}

/* Answers an INTEREST_REQ with one page of our interest blob, built into the scratch after
   a header's worth of headroom so the page is returned without a copy. Stateless. */
RambleBytes i_ramble_node_core_interest_respond(i_RambleNodeCore *c, uint16_t domain, RambleBytes req){
    uint32_t offset, total; size_t chunk, need, built;
    uint8_t *blob, *head;
    if (!c || !c->discovery) return ramble_bytes(NULL, 0);
    if (ramble_detail_kind(req) != RAMBLE_INTEREST_REQ || ramble_detail_domain(req) != domain)
        return ramble_bytes(NULL, 0);
    if (!ramble_interest_req_offset(req, &offset)) return ramble_bytes(NULL, 0);
    total = ramble_transport_interest_size(c->transport);
    need  = (size_t)RAMBLE_INTEREST_RESP_HEAD + total;
    if (need > c->detail_cap){
        uint8_t *nb = (uint8_t*)c->alloc(c->alloc_user, c->detail_buf, need);
        if (!nb) return ramble_bytes(NULL, 0);
        c->detail_buf = nb; c->detail_cap = (uint32_t)need;
    }
    blob  = c->detail_buf + RAMBLE_INTEREST_RESP_HEAD;
    built = ramble_transport_build_interest(c->transport, blob, total);
    if (built != total) return ramble_bytes(NULL, 0);   /* drifted: never serve garbage */
    if (offset > total) offset = total;                 /* clamp: a header only page reports total */
    chunk = total - offset;
    if (chunk > (size_t)RAMBLE_DGRAM_MAX - RAMBLE_INTEREST_RESP_HEAD)
        chunk = (size_t)RAMBLE_DGRAM_MAX - RAMBLE_INTEREST_RESP_HEAD;   /* one sub datagram page */
    head = blob + offset - RAMBLE_INTEREST_RESP_HEAD;   /* the header sits right before the chunk */
    ramble_interest_resp_head(domain, ramble_discovery_meta_version(c->discovery), total,
                            offset, (uint16_t)chunk, head, RAMBLE_INTEREST_RESP_HEAD);
    return ramble_bytes(head, RAMBLE_INTEREST_RESP_HEAD + chunk);
}

/* Ingests one INTEREST_RESP page at the cursor, re asks while incomplete, and on completion
   applies the blob as an inline announce would. A version change mid fetch restarts. */
void i_ramble_node_core_apply_interest_page(i_RambleNodeCore *c, uint16_t domain, uint32_t peer,
                            RambleBytes resp){
    i_RambleNodePeerExtra *ex;
    uint32_t total, offset, resp_version; RambleBytes chunk;
    if (!c || ramble_detail_kind(resp) != RAMBLE_INTEREST_RESP || ramble_detail_domain(resp) != domain)
        return;
    ex = i_ramble_node_core_peer_extra(c, peer);
    if (!ex || !ex->added) return;
    if (!ramble_interest_resp_parse(resp, &total, &offset, &chunk)) return;
    if (total > ramble_interest_max(0xFFFFu)) return;   /* an absurd total: never a real blob */
    resp_version = ramble_detail_meta_version(resp);
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
        if (!nb){ i_ramble_node_core_fire_error(c, RAMBLE_E_OOM, peer, NULL, total); return; }
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
    ramble_transport_apply_peer_interest(c->transport, peer, ramble_bytes(ex->interest_buf, total));
    i_ramble_node_core_fire_interest(c, peer);
    i_ramble_node_core_detail_check(c, ex, peer, ramble_bytes(ex->interest_buf, total));
}

static void i_ramble_node_core_peer_down(i_RambleNodeCore *c, uint32_t id, RambleDiscoveryDownReason reason){
    i_RambleNodePeerExtra *ex = i_ramble_node_core_peer_extra(c, id);   /* freed after this event */
    if (reason == RAMBLE_DISCOVERY_DROP){
        /* fell silent: keep the transport state for a same incarnation resume, drop the
           peer from flow control and tell the app once */
        if (ex && ex->added && !ex->dormant){
            ex->dormant = 1;
            ramble_transport_peer_dormant(c->transport, id);
            i_ramble_node_core_mesh_bump(c);
            i_ramble_node_core_fire(c, RAMBLE_PEER_DOWN, id, NULL);
        }
    } else {   /* GONE: free the transport state */
        int notify = (ex && ex->added && !ex->dormant);   /* the app was not told yet */
        ramble_transport_peer_remove(c->transport, id);
        i_ramble_node_core_peer_schema_clear(c, id);      /* the id may be reassigned */
        if (ex) i_ramble_reflect_free(c, &ex->refl);
        i_ramble_node_core_schema_why_clear(c, id);
        i_ramble_node_core_mesh_bump(c);
        if (ex && ex->interest_buf){                    /* the retained external interest */
            c->alloc(c->alloc_user, ex->interest_buf, 0);
            ex->interest_buf = NULL; ex->interest_cap = 0;
            ex->interest_version = 0; ex->fetch_version = 0;
            ex->fetch_cursor = 0; ex->fetch_len = 0; ex->interest_due = 0;
        }
        if (notify) i_ramble_node_core_fire(c, RAMBLE_PEER_DOWN, id, NULL);
    }
}

static void i_ramble_node_core_peer_refused(i_RambleNodeCore *c, const RambleDiscoveryAddr *addr){
    i_ramble_node_core_fire_error(c, RAMBLE_E_PEER_REFUSED, 0, addr, 0);
}

/* The discovery core's event sink, demuxed into the lifecycle handlers above. */
void i_ramble_node_core_on_disc_event(const RambleDiscoveryEvent *ev){
    i_RambleNodeCore *c = (i_RambleNodeCore*)ev->user;
    switch (ev->kind){
        case RAMBLE_DISCOVERY_PEER_UP:
            i_ramble_node_core_peer_up(c, ev->peer, &ev->addr, ev->meta);
            break;
        case RAMBLE_DISCOVERY_PEER_DOWN:
            i_ramble_node_core_peer_down(c, ev->peer, ev->reason);
            break;
        case RAMBLE_DISCOVERY_PEER_REFUSED:
            i_ramble_node_core_peer_refused(c, &ev->addr);
            break;
        case RAMBLE_DISCOVERY_META_TOO_BIG:
            i_ramble_node_core_fire_error(c, RAMBLE_E_PEER_META_TOO_BIG, ev->peer, &ev->addr, ev->meta.len);
            break;
        default: break;
    }
}

int i_ramble_node_core_resolve(i_RambleNodeCore *c, uint32_t to, i_RambleNodeDest *out){
    RambleDiscoveryAddr a;
    memset(out, 0, sizeof *out);
    if (!ramble_discovery_addr_of_id(c->discovery, to, &a)) return 0;   /* the peer vanished */
    memcpy(out->ip, a.ip, 16);
    out->ip_len = a.ip_len;
    out->port   = a.port;
    return 1;
}

int i_ramble_node_core_id_for_addr(i_RambleNodeCore *c, const uint8_t ip[4], uint16_t port, uint32_t *id){
    return ramble_discovery_id_for_addr(c->discovery, ip, 4, port, id);
}

RambleString i_ramble_node_core_peer_name(i_RambleNodeCore *c, uint32_t id){
    RambleString name = ramble_discovery_peer_name(c->discovery, id);
    if (!name.data) return name;                            /* not a known peer */
    if (name.len == 0) name = ramble_cstr("unknown-peer");  /* known but unnamed */
    return name;
}

uint16_t i_ramble_node_core_max_peers(i_RambleNodeCore *c){ return ramble_discovery_max_peers(c->discovery); }

int i_ramble_node_core_peer_at(i_RambleNodeCore *c, uint16_t slot, uint32_t *id,
                           uint8_t ip[16], uint8_t *ip_len, uint16_t *port){
    RambleDiscoveryPeer v;
    if (!ramble_discovery_peer_at(c->discovery, slot, &v)) return 0;
    if (id)     *id = v.id;
    if (ip)     memcpy(ip, v.addr.ip, 16);
    if (ip_len) *ip_len = v.addr.ip_len;
    if (port)   *port = v.addr.port;
    return 1;
}

uint16_t i_ramble_node_peer_frag(const RambleDiscoveryPeer *peer){
    return (peer && peer->meta.data) ? ramble_meta_frag(peer->meta) : 0;
}

uint32_t i_ramble_node_peer_interest_epoch(const RambleDiscoveryPeer *peer){
    const i_RambleNodePeerExtra *ex = peer ? (const i_RambleNodePeerExtra*)peer->user : NULL;
    return ex ? ex->interest_epoch : 0;
}

int i_ramble_node_peer_interest_next(const RambleDiscoveryPeer *peer,
                                   RambleInterestIter *it, RambleTopicEntry *out){
    if (!peer || !peer->meta.data) return 0;
    /* one read point for both interest homes, so every walk resolves external peers alike */
    return ramble_interest_next(
        i_ramble_node_core_interest_of((i_RambleNodePeerExtra*)peer->user, peer->meta,
                                     peer->meta_version),
        it, out);
}
#pragma endregion
#pragma region node/runtime.c
/* The node runtime: the sockets, the clock, the send and receive paths, and the public
 * ramble_node_* and ramble_topic_* API over the sans-IO cores. The rules are in spec/node.md. */

#ifdef RAMBLE_SHM
#endif
#include <string.h>    /* heap access goes through the node pool, no stdlib.h here */
#include <stdarg.h>    /* ramble_node_log */
#include <stdio.h>     /* vsnprintf for log text only, never the data path */

#ifndef RAMBLE_NO_PATTERNS
struct RambleNode;       /* patterns/core.c hosts the @ramble/meta endpoint, open calls this seam */
void i_ramble_patterns_meta_open(struct RambleNode *n);
#endif

/* A consumer queue ring record: this header, the publisher name, then the payload at an
 * 8 aligned offset. Records never wrap, a short tail holds the RAMBLE__QWRAP sentinel. */
typedef struct {
    uint32_t rec_bytes;    /* the whole record, 8 aligned. First, the ring reads it at offset 0 */
    uint32_t data_len;
    uint64_t t_recv_us;    /* the arrival stamp, RambleMsg.recv_us */
    uint64_t t_written_us; /* the publisher's source stamp, stripped at enqueue, 0 = opted out */
    uint64_t t_capture_us; /* the publisher's capture stamp, 0 = it sent none */
    uint32_t publisher_id;
    uint8_t  name_len;     /* the name is copied inline, discovery views die with the peer */
    uint8_t  pad[3];
} i_RambleQRec;
#define RAMBLE__QWRAP 0xFFFFFFFFu
#define RAMBLE__QALIGN(x) (((uint32_t)(x) + 7u) & ~7u)

/* One topic's consumer queue: a byte ring filled by the poll thread and drained by take or
 * dispatch. Under the node lock. The ring and this struct are stable pool allocations. */
typedef struct {
    uint8_t  *buf;
    uint32_t  cap;         /* current ring bytes, 8 aligned, grows on demand */
    uint32_t  cap_limit;   /* the growth bound: qos.queue_bytes or RAMBLE_QUEUE_CAP */
    uint32_t  head, tail;  /* byte offsets, empty iff count == 0 */
    uint32_t  bytes;       /* queued record bytes, excludes wrap padding */
    uint32_t  count;       /* queued records, including one being viewed */
    uint32_t  dropped;     /* best effort records overwritten or refused since open */
    uint8_t   viewing;     /* the record at tail is the consumer's live take view */
    uint8_t   busy;        /* inside dispatch's unlocked callback window, no reentry */
    uint8_t   reliable;    /* the full queue policy: park (reliable) or overwrite */
    uint8_t   parked;      /* transport lanes parked on this topic, retried as we drain */
} i_RambleMsgQueue;

struct RambleTopic {
    RambleNode *n; uint16_t index; RambleSchema *schema;   /* the schema is the node's own copy */
    i_RambleMsgQueue *q;                  /* the consumer queue, NULL = inline callbacks */
    i_RambleSysMsgFn sys_on_message;      /* the patterns layer's routing, NULL = a normal topic */
    void    *sys_msg_user;
    uint64_t  tx_msgs, tx_bytes;         /* committed by our sends */
    uint64_t  rx_msgs, rx_bytes;         /* delivered to us, parked excluded */
    uint32_t  resolve_epoch;             /* match_epoch at the last convergence, 0 = never */
    uint8_t   prefix_bytes;              /* pattern header bytes split into .header, 0 = plain */
    uint8_t   prefix_string;             /* [u8 len][bytes] follows the prefix on a response kind */
    uint8_t   kind;                      /* RambleTopicKind */
    uint8_t   role;                      /* RambleRole, mirrored at create and set_role */
    uint8_t   attrs, directed;           /* the def's declared facts, kept so refresh can redefine */
    uint8_t   reflect;                   /* created with reflect_from_mesh, so refresh applies */
    uint64_t  generation;                /* the mesh generation the schema was taken at */
    RambleQos qos;
    uint8_t   name_len;                  /* stable copy, queued views never point into the arena */
    char      name[RAMBLE_TOPIC_NAME_MAX];
};

/* One pending error to log mirror entry. Errors fire deep inside receive processing where
 * a send may not re enter the transport, so emit records here and the poll pass publishes. */
#define RAMBLE_LOG_PEND 8
typedef struct {
    uint64_t wall_us, mono_us;   /* when the first occurrence fired */
    uint32_t peer, count;
    uint16_t topic;
    uint8_t  error;              /* RambleErrorKind */
    char     text[192];
} i_RambleLogPend;

struct RambleNode {
    RambleTransportState     *transport;
    i_RambleNodeCore *core;     /* the peer table and discovery lifecycle, sans-IO */
    RambleDiscovery     *discovery;
    i_RambleSock       fd;       /* the unicast data socket */
    uint16_t      domain;
    RambleNodeNet  net;         /* a copy of opts.net */
    /* the detail exchange retry cadence: queued requests drain each poll and a sweep re
       asks once per announce interval until a sweep sends nothing */
    uint32_t      announce_us;     /* the resolved discovery announce interval */
    uint64_t      next_detail_us;  /* the next retry sweep, 0 = disarmed */
    /* the datagram the socket refused, retried first next poll so it is never lost */
    uint8_t       tx_hold[RAMBLE_DGRAM_MAX];
    size_t        tx_hold_len;
    uint32_t      tx_hold_peer;
    /* the data socket RX buffer, sized for the largest discovery datagram we accept too */
    uint8_t      *rx_buf;
    size_t        rx_buf_bytes;
    /* backpressure accumulators, read via ramble_node_backpressure_stats */
    uint64_t      backpressure_total_us;
    uint32_t      backpressure_wait_count;
    uint32_t      evicted_unsent;  /* the RAMBLE_E_EVICTED_UNSENT count */
#ifdef RAMBLE_THREADS
    /* the node lock and the optional service thread. See spec/node.md */
    i_RambleMutex   mu;             /* the node lock: every public entry point takes it */
    i_RambleCond    cv;             /* senders and drainers waiting on the poller's progress */
    uint32_t        cv_waiters;     /* under mu, broadcast skipped when 0 */
    i_RambleWaker   waker;          /* interrupts the unlocked socket wait */
    i_RambleThread  svc;
    volatile uint8_t svc_running; /* a service thread is alive */
    volatile uint8_t svc_stop;    /* stop requested, the service loop exits on it */
    uint8_t       svc_joining;    /* under mu: one stopper owns the join, others wait on cv */
    uint32_t      pollers_sleeping; /* under mu: pollers in or headed into the unlocked wait */
    uint8_t       wake_signaled;  /* under mu: a burst coalesces into one waker datagram */
    uint8_t       user_locked;    /* the public ramble_node_lock is held */
    uint64_t      work_seq;       /* completed work passes, the unsent wait predicate */
    volatile uint8_t  lock_held;  /* the owner reentrancy check, written by the owner only */
    volatile uint64_t lock_owner;
#endif
    /* the in pump probe sampled inside the backpressure wait */
    RamblePumpProbeFn pump_probe;
    void          *pump_probe_user;
    uint64_t       pump_probe_interval_us;
    /* the app callbacks */
    RambleMsgFn    user_on_message;
    RambleEventFn  on_event;
    void          *user_data;
    /* the patterns layer's hooks, all optional */
    i_RambleSysEventFn sys_on_event;
    i_RambleSysTickFn  sys_tick;
    i_RambleSysCloseFn sys_on_close;
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
    RambleEvent     last_error;  /* the most recent RAMBLE_ERROR, RAMBLE_E_NONE until one fires */
    /* the built in @ramble/log topics and the error mirror ring */
    RambleTopic    *log_topics[3];       /* by RambleLogLevel, all NULL under opts.disable_logs */
    RambleSchema   *log_schema;          /* RambleLog { wall_us, mono_us, text }, node owned */
    uint8_t         log_errors;          /* the default on RAMBLE_ERROR mirror */
    uint8_t         log_flushing;        /* reentrancy guard: a flush must not re enter the ring */
    uint8_t         log_pend_n;
    uint32_t        log_pend_dropped;    /* ring overflow between flushes, summarized, never silent */
    i_RambleLogPend *log_pend;           /* [RAMBLE_LOG_PEND], allocated on the first recorded error */
    /* the @ramble/meta snapshot scratch, grown on demand */
    uint8_t      *snap_buf; uint32_t snap_cap;
    uint16_t     *snap_pend; uint16_t snap_pend_cap;   /* per topic pending counts, one walk */
    char          name[RAMBLE_NODE_NAME_MAX + 1];   /* our advertised node name */
    uint8_t       name_len;
    void          *arena;      /* the control structs block, relocated on grow */
    /* the node's pool, copied from the caller's allocator at open and reset on close */
    RambleAllocator pool;
    uint8_t        alloc_dynamic;
    uint8_t        grow_pending;   /* a peer was refused for lack of slots, grow at the next poll */
    uint16_t       max_peers;      /* the current peer table capacity, doubles on a dynamic grow */
    /* the accept bound: the largest peer announce blob we store. It self heals up, see
       spec/discovery.md. meta_grow_failed remembers a size that would not allocate */
    uint16_t       meta_cap;
    uint16_t       meta_grow_need;
    uint16_t       meta_grow_failed;
    /* topic handles: a pointer array in the arena, each RambleTopic a stable allocation */
    RambleTopic **handles;    /* [max_topics] */
    uint16_t      max_topics;
    uint16_t      n_created;       /* user topics created, dense from 0, stepping over builtins */
    /* the builtins live at [builtin_lo, builtin_lo + n_builtin), above the user budget */
    uint16_t      builtin_lo;      /* the first builtin index */
    uint16_t      n_builtin;
    uint8_t       creating_builtin;/* open is creating builtins: allocate from the block */
#ifdef RAMBLE_SHM
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
static void *i_ramble_node_alloc(void *u, void *ptr, size_t size){
    return ramble_allocator_alloc(&((RambleNode*)u)->pool, ptr, size);
}

#ifdef RAMBLE_THREADS
/* Raw lock ops keeping the owner id consistent. lock_owner is zeroed before the release
   so a non owner reading the pair unlocked never sees lock_held with its own id. */
static void i_ramble_node_lock_raw(RambleNode *n){
    i_ramble_plat_mutex_lock(&n->mu);
    n->lock_owner = i_ramble_plat_thread_id();
    n->lock_held = 1;
}
static void i_ramble_node_unlock_raw(RambleNode *n){
    n->lock_owner = 0;
    n->lock_held = 0;
    i_ramble_plat_mutex_unlock(&n->mu);
}
/* The reentrant aware entry lock: 1 = this call acquired mu, 0 = the calling thread
   already held it (a callback, or the pump's nested poll) and proceeds without waiting. */
static int i_ramble_node_lock(RambleNode *n){
    if (n->lock_held && n->lock_owner == i_ramble_plat_thread_id()) return 0;
    i_ramble_node_lock_raw(n);
    return 1;
}
static void i_ramble_node_unlock(RambleNode *n, int acquired){
    if (acquired) i_ramble_node_unlock_raw(n);
}
/* With mu held, before unlocking: cut the pollers' wait short so the change is serviced
   now. One datagram wakes every sleeping poller, and a burst coalesces to one per sleep. */
static void i_ramble_node_kick(RambleNode *n){
    if (n->pollers_sleeping && !n->wake_signaled && i_ramble_plat_waker_signal(&n->waker))
        n->wake_signaled = 1;
}
/* A condvar wait that re stamps ownership after reacquire. Nothing cached from inside
   the node survives it: the poller may have relocated the arena. */
static void i_ramble_node_cv_wait(RambleNode *n, uint64_t timeout_us){
    n->lock_owner = 0;
    n->lock_held = 0;
    i_ramble_plat_cond_wait(&n->cv, &n->mu,
                          timeout_us > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)timeout_us);
    n->lock_owner = i_ramble_plat_thread_id();
    n->lock_held = 1;
}
#else
static int  i_ramble_node_lock(RambleNode *n){ (void)n; return 1; }
static void i_ramble_node_unlock(RambleNode *n, int acquired){ (void)n; (void)acquired; }
static void i_ramble_node_kick(RambleNode *n){ (void)n; }
#endif /* RAMBLE_THREADS */

/* The kick a send owes: only when the transport has a datagram to hand out or a held one
   to retry. The waker costs tens of microseconds, an idle publisher must not pay it. */
static void i_ramble_node_kick_tx(RambleNode *n){
#ifdef RAMBLE_THREADS
    if (n->tx_hold_len || ramble_transport_tx_pending(n->transport)) i_ramble_node_kick(n);
#else
    (void)n;
#endif
}

/* error reporting: one emit path stamps user_data, keeps the last error and hands on */

/* one past the highest defined topic index, the bound over handles[] (holes are NULL) */
static uint16_t i_ramble_node_topic_hi(RambleNode *n){
    uint16_t hi = n->n_created;
    if (n->n_builtin){
        if (hi >= n->builtin_lo) hi = (uint16_t)(hi + n->n_builtin);   /* users grew past it */
        if ((uint16_t)(n->builtin_lo + n->n_builtin) > hi) hi = (uint16_t)(n->builtin_lo + n->n_builtin);
    }
    return hi;
}

static int i_ramble_node_is_log_topic(RambleNode *n, uint16_t topic_index){
    RambleTopic *h = (topic_index < i_ramble_node_topic_hi(n)) ? n->handles[topic_index] : NULL;
    return h && (h == n->log_topics[0] || h == n->log_topics[1] || h == n->log_topics[2]);
}

static void i_ramble_node_emit(RambleNode *n, RambleEvent *e){
    e->user = n->user_data;
    if (e->peer){    /* resolve the peer's name once so every event message prints a label */
        RambleString nm = i_ramble_node_core_peer_name(n->core, e->peer);
        e->peer_name = nm.data;   /* a NUL terminated view into discovery state, NULL if unknown */
    }
    if (e->kind == RAMBLE_ERROR) n->last_error = *e;
    /* the error to log mirror records only, the poll pass publishes. Errors on a log
       topic itself are excluded, and nothing is recorded during a flush */
    if (e->kind == RAMBLE_ERROR && n->log_errors && !n->log_flushing
        && !(e->topic_name && i_ramble_node_is_log_topic(n, e->topic))){
        uint8_t i;
        if (!n->log_pend){                 /* the first error ever: allocate the mirror ring */
            n->log_pend = (i_RambleLogPend*)i_ramble_node_alloc(n, NULL,
                              sizeof(i_RambleLogPend) * RAMBLE_LOG_PEND);
            if (!n->log_pend){ n->log_pend_dropped++; goto pend_done; }
        }
        for (i = 0; i < n->log_pend_n; i++)
            if (n->log_pend[i].error == (uint8_t)e->error && n->log_pend[i].topic == e->topic
                && n->log_pend[i].peer == e->peer) break;
        if (i < n->log_pend_n) n->log_pend[i].count++;
        else if (n->log_pend_n < RAMBLE_LOG_PEND){
            i_RambleLogPend *p = &n->log_pend[n->log_pend_n++];
            p->error = (uint8_t)e->error; p->topic = e->topic; p->peer = e->peer; p->count = 1;
            p->wall_us = i_ramble_plat_wall_us(); p->mono_us = i_ramble_plat_now_us();
            ramble_event_str(e, p->text, sizeof p->text);
        } else n->log_pend_dropped++;
    }
pend_done:
    if (e->kind == RAMBLE_PEER_UP || e->kind == RAMBLE_PEER_DOWN || e->kind == RAMBLE_PEER_INTEREST){
        n->settle_topology_us = i_ramble_plat_now_us();   /* ramble_node_settle's quiet window clock */
        n->match_epoch++;                                 /* stale the topics' converged memos */
    }
    if (n->sys_on_event) n->sys_on_event(n->sys_user, e);
    if (n->on_event) n->on_event(e);
}

/* our topic's name as a C string, NULL if undefined: the topic_name view on events */
static const char *i_ramble_node_topic_name(RambleNode *n, uint16_t topic_index){
    RambleString s = ramble_transport_topic_name(n->transport, topic_index);
    return (const char*)s.data;
}

/* Strips the stamps a stamped publisher prepends, filling *written_us and *capture_us.
 * The one strip point: every delivery path runs through here before the header split. */
static RambleBytes i_ramble_node_strip_ts(RambleBytes wire, int stamped, uint64_t *written_us,
                                      uint64_t *capture_us){
    uint64_t w;
    *written_us = 0; *capture_us = 0;
    if (!stamped || wire.len < RAMBLE_TIMESTAMP_BYTES) return wire;
    w = i_ramble_le_r64(wire.data);
    *written_us = w & RAMBLE_STAMP_MASK;
    if (!(w & RAMBLE_STAMP_CAPTURE))
        return ramble_bytes(wire.data + RAMBLE_TIMESTAMP_BYTES, wire.len - RAMBLE_TIMESTAMP_BYTES);
    /* the marker promises a second slot: a sample too short for it is malformed, keep the
       bytes whole rather than reading past them */
    if (wire.len < (size_t)RAMBLE_TIMESTAMP_BYTES + RAMBLE_CAPTURE_BYTES) return wire;
    *capture_us = i_ramble_le_r64(wire.data + RAMBLE_TIMESTAMP_BYTES);
    return ramble_bytes(wire.data + RAMBLE_TIMESTAMP_BYTES + RAMBLE_CAPTURE_BYTES,
                      wire.len - RAMBLE_TIMESTAMP_BYTES - RAMBLE_CAPTURE_BYTES);
}

/* Splits a delivered wire into its pattern header and the payload after it, so the schema
 * validates a message free payload. A plain topic yields an empty header. */
static void i_ramble_node_split(RambleTopic *h, RambleBytes wire, RambleBytes *hdr, RambleBytes *payload){
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
static const i_RambleQRec *i_ramble_q_peek(i_RambleMsgQueue *q){
    if (!q->count) return NULL;
    if (q->cap - q->tail < (uint32_t)sizeof(i_RambleQRec) ||
        ((const i_RambleQRec*)(q->buf + q->tail))->rec_bytes == RAMBLE__QWRAP)
        q->tail = 0;
    return (const i_RambleQRec*)(q->buf + q->tail);
}

/* drops the oldest record, never a viewed one */
static void i_ramble_q_pop(i_RambleMsgQueue *q){
    const i_RambleQRec *rec = i_ramble_q_peek(q);
    if (!rec) return;
    q->tail += rec->rec_bytes;
    q->bytes -= rec->rec_bytes;
    if (--q->count == 0){ q->head = 0; q->tail = 0; }
}

/* contiguous space for need bytes, fills *at and pre writes the wrap sentinel */
static int i_ramble_q_fit(i_RambleMsgQueue *q, uint32_t need, uint32_t *at){
    if (!q->buf || need > q->cap) return 0;
    if (q->count == 0){ q->head = 0; q->tail = 0; *at = 0; return 1; }
    if (q->head > q->tail){
        if (q->cap - q->head >= need){ *at = q->head; return 1; }
        if (q->tail >= need){                                    /* wrap to the start */
            if (q->cap - q->head >= 4u)
                ((i_RambleQRec*)(q->buf + q->head))->rec_bytes = RAMBLE__QWRAP;
            *at = 0; return 1;
        }
        return 0;
    }
    if (q->head < q->tail && q->tail - q->head >= need){ *at = q->head; return 1; }
    return 0;                                                    /* full (head == tail) */
}

/* Relinearizes into a bigger ring, doubling toward cap_limit. One over cap message still
 * fits. 0 = cannot grow now: at the cap, OOM, or a live take view pins the ring. */
static int i_ramble_q_grow(RambleNode *n, i_RambleMsgQueue *q, uint32_t need_total, uint32_t need_one){
    uint32_t target = q->cap ? q->cap * 2u : 4096u;
    uint8_t *nb;
    if (q->viewing) return 0;
    if (target < 4096u) target = 4096u;
    while (target < need_total && target < 0x80000000u) target *= 2u;
    if (target > q->cap_limit) target = q->cap_limit;
    if (target < need_one) target = RAMBLE__QALIGN(need_one);      /* one message always fits */
    if (target <= q->cap) return 0;
    nb = (uint8_t*)i_ramble_node_alloc(n, NULL, target);
    if (!nb) return 0;
    {   uint32_t off = 0, i, src = q->tail;                        /* compact, oldest first */
        for (i = 0; i < q->count; i++){
            const i_RambleQRec *rec;
            if (q->cap - src < (uint32_t)sizeof(i_RambleQRec) ||
                ((const i_RambleQRec*)(q->buf + src))->rec_bytes == RAMBLE__QWRAP) src = 0;
            rec = (const i_RambleQRec*)(q->buf + src);
            memcpy(nb + off, rec, rec->rec_bytes);
            off += rec->rec_bytes; src += rec->rec_bytes;
        }
        q->tail = 0; q->head = off;
    }
    if (q->buf) i_ramble_node_alloc(n, q->buf, 0);
    q->buf = nb; q->cap = target;
    return 1;
}

/* best effort queue loss is loss like any other: RAMBLE_MSG_LOST, never silent */
static void i_ramble_node_queue_lost(RambleNode *n, uint16_t topic_index, uint32_t from, uint32_t count){
    RambleEvent e; memset(&e, 0, sizeof e);
    e.kind = RAMBLE_MSG_LOST; e.topic = topic_index; e.peer = from;
    e.topic_name = i_ramble_node_topic_name(n, topic_index);
    e.lost_count = count;
    i_ramble_node_emit(n, &e);
}

/* Enqueues one message. 0 = accepted (stored, or dropped per the best effort contract),
 * 1 = refused, a reliable queue at cap: the transport parks and flow control backpressures. */
static int i_ramble_node_queue_push(RambleNode *n, uint16_t topic_index, i_RambleMsgQueue *q,
                                  uint32_t from, RambleBytes data, uint64_t written_us,
                                  uint64_t capture_us){
    RambleString name = i_ramble_node_core_peer_name(n->core, from);
    uint32_t name_len = name.len > RAMBLE_NODE_NAME_MAX ? (uint32_t)RAMBLE_NODE_NAME_MAX
                                                      : (uint32_t)name.len;
    uint32_t payload_off = RAMBLE__QALIGN(sizeof(i_RambleQRec) + name_len);
    uint32_t need = RAMBLE__QALIGN(payload_off + data.len);
    uint32_t at = 0, evicted = 0;
    for (;;){
        if (i_ramble_q_fit(q, need, &at)) break;
        if (i_ramble_q_grow(n, q, q->bytes + need, need)) continue;
        if (q->reliable){ q->parked = 1; return 1; }
        if (!q->viewing && q->count){                        /* overwrite the oldest */
            i_ramble_q_pop(q); q->dropped++; evicted++; continue;
        }
        q->dropped++;                     /* a live view pins the ring: drop the incoming */
        i_ramble_node_queue_lost(n, topic_index, from, evicted + 1u);
        return 0;
    }
    {   i_RambleQRec *rec = (i_RambleQRec*)(q->buf + at);
        rec->rec_bytes = need; rec->data_len = (uint32_t)data.len;
        rec->t_recv_us = i_ramble_plat_now_us();
        rec->t_written_us = written_us;
        rec->t_capture_us = capture_us;
        rec->publisher_id = from; rec->name_len = (uint8_t)name_len;
        rec->pad[0] = rec->pad[1] = rec->pad[2] = 0;
        if (name_len) memcpy((uint8_t*)rec + sizeof *rec, name.data, name_len);
        if (data.len) memcpy(q->buf + at + payload_off, data.data, data.len);
        q->head = at + need;
        q->bytes += need; q->count++;
    }
    if (evicted) i_ramble_node_queue_lost(n, topic_index, from, evicted);
    return 0;
}

/* A queued RambleMsg: every view points at stable memory, valid until the next take or
 * dispatch. The schema is re resolved now, since the delivery map may repoint. */
static void i_ramble_node_queue_msg(RambleNode *n, RambleTopic *h, const i_RambleQRec *rec, RambleMsg *m){
    memset(m, 0, sizeof *m);
    m->node = n; m->user = n->user_data;
    m->topic_index = h->index;
    m->publisher_id = rec->publisher_id;
    m->publisher_name = rec->name_len ? ramble_string((const char*)(rec + 1), rec->name_len)
                                   : ramble_cstr("unknown-peer");
    m->topic_name = ramble_string(h->name, h->name_len);
    i_ramble_node_split(h, ramble_bytes((const uint8_t*)rec + RAMBLE__QALIGN(sizeof *rec + rec->name_len),
                         rec->data_len), &m->header, &m->data);
    m->schema = i_ramble_node_core_msg_schema(n->core, rec->publisher_id, h->index);
    if (h->prefix_bytes && m->data.len == 0) m->schema = NULL;   /* an op only pattern message */
    m->recv_us = rec->t_recv_us;
    m->written_us = rec->t_written_us;
    m->capture_us = rec->t_capture_us;
}

/* Creates the queue. An explicit queue_bytes allocates in full, the lazy default starts at
 * one page and grows toward RAMBLE_QUEUE_CAP. NULL on OOM. */
static i_RambleMsgQueue *i_ramble_node_queue_ensure(RambleNode *n, RambleTopic *h, const RambleQos *qos){
    i_RambleMsgQueue *q = h->q;
    uint32_t limit, initial;
    if (q) return q;
    if (!qos) qos = ramble_transport_topic_qos(n->transport, h->index);
    limit = RAMBLE__QALIGN((qos && qos->queue_bytes) ? qos->queue_bytes : RAMBLE_QUEUE_CAP);
    initial = (qos && qos->queue_bytes) ? limit : (limit < 4096u ? limit : 4096u);
    q = (i_RambleMsgQueue*)i_ramble_node_alloc(n, NULL, sizeof *q);
    if (!q) return NULL;
    memset(q, 0, sizeof *q);
    q->buf = (uint8_t*)i_ramble_node_alloc(n, NULL, initial);
    if (!q->buf){ i_ramble_node_alloc(n, q, 0); return NULL; }
    q->cap = initial; q->cap_limit = limit;
    q->reliable = (qos && qos->reliability == RAMBLE_RELIABLE) ? 1u : 0u;
    h->q = q;
    return q;
}

/* Finishes an outstanding take view, then retries parked lanes into the freed space. Their
 * acks need a TX pass, so kick. Lock held. */
static void i_ramble_node_queue_release(RambleNode *n, RambleTopic *h, i_RambleMsgQueue *q){
    if (q->viewing){
        q->viewing = 0;
        i_ramble_q_pop(q);
    }
    if (q->parked){
        q->parked = ramble_transport_deliver_parked(n->transport, h->index,
                                                  i_ramble_plat_now_us()) ? 1u : 0u;
        i_ramble_node_kick(n);
    }
}

/* The process global last error slot for failures during open, before the node exists.
 * A plain global, meaningful right after a failed open on the calling thread. */
static RambleEvent g_last_error;

/* Reports an open time failure into the global slot and the caller's on_event. Returns NULL. */
static RambleNode *i_ramble_node_open_fail(RambleEventFn on_event, void *user, RambleErrorKind err,
                            int os_error, uint16_t port, uint64_t need){
    RambleEvent e; memset(&e, 0, sizeof e);
    e.kind = RAMBLE_ERROR; e.error = err; e.user = user;
    e.os_error = os_error; e.port = port; e.too_big_bytes = need;
    g_last_error = e;
    if (on_event) on_event(&e);
    return NULL;
}

RambleEvent ramble_last_error(RambleNode *n){ return n ? n->last_error : g_last_error; }

/* Builds a RambleMsg and hands it to the app, inline or copied into the consumer queue.
 * Returns 0 accepted, nonzero refused (a reliable queue at cap, the transport parks). */
static int i_ramble_node_deliver(RambleNode *n, uint16_t topic_index, uint32_t from, RambleBytes data){
    RambleMsg m;
    RambleTopic *h = (topic_index < i_ramble_node_topic_hi(n)) ? n->handles[topic_index] : NULL;
    RambleBytes hdr, payload, body;
    uint64_t written_us, capture_us;
    const RambleSchema *schema = i_ramble_node_core_msg_schema(n->core, from, topic_index);
    /* the stamps first, then the header split, then the schema validates the payload */
    body = i_ramble_node_strip_ts(data,
              ramble_transport_peer_timestamped(n->transport, topic_index, from),
              &written_us, &capture_us);
    i_ramble_node_split(h, body, &hdr, &payload);
    /* an op only pattern message (a zero payload, or a task op that is not CALL) skips
       the schema, the pattern layer judges it. See spec/node.md */
    if (h && h->prefix_bytes
        && (payload.len == 0
            || (h->kind == RAMBLE_KIND_TASK_REQ && hdr.len >= 5 && hdr.data[4] != 0)))
        schema = NULL;
    if (schema && !ramble_schema_validate(schema, payload)){
        RambleEvent e; memset(&e, 0, sizeof e);
        e.kind = RAMBLE_ERROR; e.error = RAMBLE_E_SCHEMA_MISMATCH;
        e.peer = from; e.topic = topic_index; e.topic_name = i_ramble_node_topic_name(n, topic_index);
        e.schema_detail = i_ramble_node_core_note_size_mismatch(n->core, from, topic_index,
                                            payload.len, ramble_schema_msg_min(schema));
        i_ramble_node_emit(n, &e);
        return 0;
    }
    if (h && h->q){
        /* store the wire minus the stamps, the stamps ride the record */
        if (i_ramble_node_queue_push(n, topic_index, h->q, from, body, written_us, capture_us))
            return 1;   /* parked: the accepted retry re counts */
        h->rx_msgs++; h->rx_bytes += data.len;
        return 0;
    }
    if (h){ h->rx_msgs++; h->rx_bytes += data.len; }
    if (!(h && h->sys_on_message) && !n->user_on_message) return 0;
    memset(&m, 0, sizeof m);
    m.node = n; m.user = n->user_data;
    m.topic_index = topic_index; m.publisher_id = from;
    m.publisher_name = i_ramble_node_core_peer_name(n->core, from);          /* a discovery view */
    if (!m.publisher_name.data) m.publisher_name = ramble_cstr("unknown-peer"); /* never NULL */
    m.topic_name = ramble_transport_topic_name(n->transport, topic_index);
    m.header = hdr; m.data = payload;
    m.schema = schema;
    m.recv_us = i_ramble_plat_now_us();
    m.written_us = written_us;
    m.capture_us = capture_us;
    if (h && h->sys_on_message) h->sys_on_message(h->sys_msg_user, &m);    /* the patterns layer */
    else n->user_on_message(&m);
    return 0;
}
static int i_ramble_node_on_message(void *u, uint16_t topic_index, uint32_t from, RambleBytes data){
    return i_ramble_node_deliver((RambleNode*)u, topic_index, from, data);
}
/* Node core events funnel through here. The core sets ev->user to this node, swap it for
 * the app's user_data before handing on. */
static void i_ramble_node_on_event(const RambleEvent *ev){
    RambleNode *n = (RambleNode*)ev->user;
    /* dynamic mode has no peer cap: a refusal means grow at the next poll, out of this
       callback, and the peer's next announce is admitted. Static mode surfaces it */
    if (n->alloc_dynamic && ev->kind == RAMBLE_ERROR && ev->error == RAMBLE_E_PEER_REFUSED){
        n->grow_pending = 1; return;
    }
    /* a blob we cannot hold is a growth signal too: grow at the next poll, then solicit.
       Sizes past the wire ceiling and a size that already failed surface */
    if (n->alloc_dynamic && ev->kind == RAMBLE_ERROR && ev->error == RAMBLE_E_PEER_META_TOO_BIG){
        uint64_t need = ev->too_big_bytes;
        if (need > n->meta_cap && need <= 65000u && (uint16_t)need != n->meta_grow_failed){
            n->meta_grow_need = (uint16_t)need;
            return;
        }
    }
    { RambleEvent e = *ev; i_ramble_node_emit(n, &e); }
}

/* the transport's schema gate, answered by the node core. u is the node */
static int i_ramble_node_schema_check(void *u, uint32_t peer, uint16_t topic_index,
                                    int peer_is_pub, uint64_t hash, RambleBytes wire){
    return i_ramble_node_core_schema_check(((RambleNode*)u)->core, peer, topic_index,
                                         peer_is_pub, hash, wire);
}

/* the transport's source clock: the wall clock, since it is read on other hosts */
static uint64_t i_ramble_node_source_time(void *u){ (void)u; return i_ramble_plat_wall_us(); }

/* Maps a transport event onto the app RambleEvent. MSG_LOST is an info kind, everything
 * else is a RAMBLE_ERROR with its RambleErrorKind, and topic scoped kinds get our name. */
static void i_ramble_node_on_transport_event(const RambleTransportEvent *tev){
    RambleNode *n = (RambleNode*)tev->user;
    RambleEvent e;
    memset(&e, 0, sizeof e);
    e.peer = tev->peer; e.topic = tev->topic;
    e.lost_first = tev->lost_first; e.lost_count = tev->lost_count;
    e.too_big_bytes = tev->too_big_bytes; e.identity = tev->identity;
    switch (tev->kind){
    case RAMBLE_TRANSPORT_MSG_LOST:
        e.kind = RAMBLE_MSG_LOST; e.topic_name = i_ramble_node_topic_name(n, tev->topic); break;
    case RAMBLE_TRANSPORT_MSG_TOO_BIG:
        e.kind = RAMBLE_ERROR; e.error = RAMBLE_E_MSG_TOO_BIG; e.topic_name = i_ramble_node_topic_name(n, tev->topic); break;
    case RAMBLE_TRANSPORT_NAME_COLLISION:
        e.kind = RAMBLE_ERROR; e.error = RAMBLE_E_NAME_COLLISION; e.topic_name = i_ramble_node_topic_name(n, tev->topic); break;
    case RAMBLE_TRANSPORT_QOS_INCOMPATIBLE:
        e.kind = RAMBLE_ERROR; e.error = RAMBLE_E_QOS_INCOMPATIBLE; e.topic_name = i_ramble_node_topic_name(n, tev->topic); break;
    case RAMBLE_TRANSPORT_KIND_MISMATCH:
        e.kind = RAMBLE_ERROR; e.error = RAMBLE_E_KIND_MISMATCH; e.topic_name = i_ramble_node_topic_name(n, tev->topic); break;
    case RAMBLE_TRANSPORT_SCHEMA_MISMATCH:
        e.kind = RAMBLE_ERROR; e.error = RAMBLE_E_SCHEMA_MISMATCH; e.topic_name = i_ramble_node_topic_name(n, tev->topic);
        e.schema_detail = i_ramble_node_core_schema_why(n->core, tev->peer, tev->topic,
                                                      tev->peer_is_pub); break;
    case RAMBLE_TRANSPORT_INTEREST_OVERFLOW:
        e.kind = RAMBLE_ERROR; e.error = RAMBLE_E_INTEREST_OVERFLOW; break;
    case RAMBLE_TRANSPORT_META_TRUNCATED_INTEREST:
        e.kind = RAMBLE_ERROR; e.error = RAMBLE_E_META_TRUNCATED_INTEREST; break;
    case RAMBLE_TRANSPORT_META_TRUNCATED_SCHEMA:
        e.kind = RAMBLE_ERROR; e.error = RAMBLE_E_META_TRUNCATED_SCHEMA; break;
    default: return;
    }
    i_ramble_node_emit(n, &e);
}

/* The arena's sub blocks, laid out in one place so the measure pass and the build pass
 * run the same sequence and never drift. */
typedef struct {
    uint8_t   *handles, *node_core, *transport, *discovery, *rx_buf;
#ifdef RAMBLE_SHM
    uint8_t   *shm_pool;
#endif
    size_t     node_core_bytes, transport_bytes, discovery_bytes, rx_buf_bytes;
} i_RambleNodeBlocks;

static void i_ramble_node_layout(i_RambleBump *b, uint16_t max_peers, uint16_t max_topics,
                              const RambleConfig *transport_cfg,
                              const RambleDiscoveryNetConfig *discovery_rt_cfg, i_RambleNodeBlocks *o){
    o->handles       = (uint8_t*)i_ramble_bump_take(b, (size_t)max_topics * sizeof(RambleTopic*), 16);
    o->node_core_bytes = i_ramble_node_core_required_memory(max_topics);   /* no peers here */
    o->node_core = (uint8_t*)i_ramble_bump_take(b, o->node_core_bytes, 16);
    o->transport_bytes = ramble_transport_required_memory(transport_cfg);
    o->transport = (uint8_t*)i_ramble_bump_take(b, o->transport_bytes, 16);
#ifdef RAMBLE_SHM
    /* only the (topic, class) segment pointer table lives in the arena */
    o->shm_pool = (uint8_t*)i_ramble_bump_take(b, (size_t)max_topics * RAMBLE_SHM_N_CLASSES * sizeof(void*), 16);
#endif
    o->discovery_bytes = ramble_discovery_placement_memory(discovery_rt_cfg);
    o->discovery = (uint8_t*)i_ramble_bump_take(b, o->discovery_bytes, 16);
    /* the RX buffer fits a unicast announce carrying the largest blob we accept, since
       recvfrom drops an oversized datagram and a late joiner has no other path to it */
    o->rx_buf_bytes = ramble_discovery_wire_size(discovery_rt_cfg->discovery.meta_cap);
    if (o->rx_buf_bytes < RAMBLE_DGRAM_MAX) o->rx_buf_bytes = RAMBLE_DGRAM_MAX;
    o->rx_buf = (uint8_t*)i_ramble_bump_take(b, o->rx_buf_bytes, 16);
}

/* Sends one datagram to a peer. 1 when done with it, 0 only on a would block TX full. */
static int i_ramble_node_tx(RambleNode *n, uint32_t to, const uint8_t *buf, size_t len){
    i_RambleNodeDest d;
    if (!i_ramble_node_core_resolve(n->core, to, &d)) return 1;   /* the peer vanished */
    if (i_ramble_plat_send(n->fd, buf, len, d.ip, d.port) < 0){
        if (i_ramble_plat_would_block()) return 0;                /* TX full: retry next tick */
        {   /* a hard failure: report and drop, reliable data is repaired. Byte 0 is type and
               flags, bytes 1 and 2 the first submessage's topic index */
            RambleEvent e; memset(&e, 0, sizeof e);
            e.kind = RAMBLE_ERROR; e.error = RAMBLE_E_SEND; e.peer = to;
            e.os_error = i_ramble_plat_last_socket_error();
            e.too_big_bytes = len;
            if (len >= 3){
                uint16_t idx = (uint16_t)(buf[1] | ((uint16_t)buf[2] << 8));
                e.topic = idx;
                e.topic_name = i_ramble_node_topic_name(n, idx);
            }
            i_ramble_node_emit(n, &e);
        }
    }
    return 1;
}

#ifdef RAMBLE_SHM
/* Lazily creates our per topic segment: chunk_bytes is the size class, n_chunks the
 * keep_last, so history slot i binds chunk i. NULL on failure. */
static i_RambleShmPool *i_ramble_node_shm_topic_pool(RambleNode *n, uint16_t topic_index, uint32_t k, uint16_t keep_last){
    i_RambleShmConfig c; uint8_t *mem; uint64_t seg; size_t idx;
    if (k >= RAMBLE_SHM_N_CLASSES) return NULL;
    idx = (size_t)topic_index * RAMBLE_SHM_N_CLASSES + k;
    if (n->shm_pool[idx]) return (i_RambleShmPool*)n->shm_pool[idx];
    memset(&c, 0, sizeof c);
    seg = n->shm_base | ((uint64_t)topic_index << 3) | (uint64_t)k;   /* class low, topic above */
    c.segment_id = seg;
    i_ramble_shm_seg_name(c.name, seg);
    c.chunk_bytes = i_ramble_shm_class_bytes(k);
    c.n_chunks = keep_last ? keep_last : 1u;
    mem = (uint8_t*)i_ramble_node_alloc(n, NULL, i_ramble_shm_state_bytes());   /* stable */
    if (!mem) return NULL;
    n->shm_pool[idx] = i_ramble_shm_create(mem, &c);
    if (!n->shm_pool[idx]) i_ramble_node_alloc(n, mem, 0);
    return (i_RambleShmPool*)n->shm_pool[idx];
}
/* Lazily attaches a peer's segment by id and caches it. The cache grows with segments
 * actually attached, so a node with no same host peer holds none. */
static i_RambleShmPool *i_ramble_node_shm_reader_pool(RambleNode *n, uint64_t seg){
    uint16_t i; i_RambleShmConfig c; uint8_t *mem;
    for (i=0;i<n->shm_reader_count;i++)
        if (n->shm_reader_segments[i]==seg) return (i_RambleShmPool*)n->shm_reader_states[i];
    if (n->shm_reader_count == n->shm_reader_cap){        /* grow the id and state arrays */
        uint16_t ncap = n->shm_reader_cap ? (uint16_t)(n->shm_reader_cap*2u) : 8u;
        uint64_t *nseg; void **nst;
        if (ncap <= n->shm_reader_cap) return NULL;       /* u16 wrap: an absurd segment count */
        nseg = (uint64_t*)i_ramble_node_alloc(n, n->shm_reader_segments, (size_t)ncap*sizeof(uint64_t));
        if (!nseg) return NULL;
        n->shm_reader_segments = nseg;
        nst = (void**)i_ramble_node_alloc(n, n->shm_reader_states, (size_t)ncap*sizeof(void*));
        if (!nst) return NULL;
        n->shm_reader_states = nst;
        n->shm_reader_cap = ncap;
    }
    memset(&c, 0, sizeof c);
    c.segment_id = seg; i_ramble_shm_seg_name(c.name, seg);     /* attach reads the geometry */
    mem = (uint8_t*)i_ramble_node_alloc(n, NULL, i_ramble_shm_state_bytes());
    if (!mem) return NULL;
    if (!i_ramble_shm_attach(mem, &c)){ i_ramble_node_alloc(n, mem, 0); return NULL; }
    n->shm_reader_segments[n->shm_reader_count] = seg;
    n->shm_reader_states[n->shm_reader_count] = mem;
    n->shm_reader_count++;
    return (i_RambleShmPool*)mem;
}
static int i_ramble_node_on_shm(void *u, uint16_t topic_index, uint32_t from, const uint8_t *desc){
    RambleNode *n = (RambleNode*)u; i_RambleShmDesc d; i_RambleShmPool *reader_pool; const void *p; uint32_t len;
    if (!i_ramble_shm_desc_decode(&d, desc, RAMBLE_SHM_DESC_WIRE)) return 0;
    reader_pool = i_ramble_node_shm_reader_pool(n, d.segment_id);
    if (!reader_pool) return 0;                                     /* cannot attach: NACK */
    p = i_ramble_shm_read(reader_pool, &d, &len);                       /* the seqlock head */
    if (!p) return 0;                                      /* recycled: NACK, then repair or skip */
    /* one copy out of shared memory so the user owns the bytes */
    if (len > n->shm_scratch_cap){
        void *new_buf = i_ramble_node_alloc(n, n->shm_scratch, len?len:1u);
        if (!new_buf) return 0;
        n->shm_scratch = new_buf; n->shm_scratch_cap = len;
    }
    memcpy(n->shm_scratch, p, len);
    if (!i_ramble_shm_verify(reader_pool, &d)) return 0;                /* the seqlock tail: torn */
    if (i_ramble_node_deliver(n, topic_index, from, ramble_bytes(n->shm_scratch, len)))
        return -1;                                         /* the queue is full: park */
    n->shm_rx++;
    return 1;
}
#endif

static void i_ramble_node_logs_open(RambleNode *n);   /* defined with the log API below */

RambleAllocator ramble_allocator_heap(uint32_t page_size){
    return ramble_allocator_dynamic(i_ramble_plat_realloc, page_size);
}

void *ramble_heap_realloc(void *user, void *ptr, size_t size){
    (void)user;
    return i_ramble_plat_realloc(ptr, size);
}

RambleNode *ramble_node_open(RambleAllocator *alloc, const char *name, RambleMsgFn on_message, RambleEventFn on_event, const RambleNodeOpts *opts){
    RambleNodeOpts o; RambleDiscoveryNetConfig dc; RambleConfig tc; i_RambleNodeBlocks blocks;
    uint16_t max_peers, max_topics, user_topics;
    uint8_t *base; void *arena; size_t need; RambleAllocator pool;
    RambleNode *n; i_RambleSock fd; uint16_t local_port;
    char node_name[RAMBLE_NODE_NAME_MAX + 1]; uint8_t node_name_len = 0;

    memset(&o, 0, sizeof o);
    if (opts) o = *opts;
    /* a node has no memory of its own, so no allocator is the same fault as one that
       returns NULL. Reported, never a bare NULL with an empty last error. */
    if (!alloc) return i_ramble_node_open_fail(on_event, o.user_data, RAMBLE_E_OOM, 0, 0, sizeof *n);
    user_topics = o.max_topics ? o.max_topics : 8;
    /* the builtins ride outside the app's budget, in a block above it */
    max_topics = (uint16_t)(user_topics + (o.disable_logs ? 0 : 3));
#ifndef RAMBLE_NO_PATTERNS
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
    dc.discovery.meta_cap = ramble_meta_cap(max_topics);
    dc.discovery.peer_user_bytes = i_ramble_node_core_peer_user_bytes();   /* the core's scratch */
    dc.discovery.alloc = i_ramble_node_alloc;   /* per peer blobs at their size. Set before sizing so
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
    tc.allocator   = i_ramble_node_alloc;   /* required by ramble_transport_init */

    {   i_RambleBump b; memset(&b,0,sizeof b);
        i_ramble_node_layout(&b, max_peers, max_topics, &tc, &dc, &blocks);
        need = b.offset + 32u; }

    /* The node copies the caller's allocator into its own pool, so the caller's may be a
       temporary. The node struct stays put across a grow, the arena is relocated. */
    pool = *alloc;
    n = (RambleNode*)ramble_allocator_alloc(&pool, NULL, sizeof *n);
    if (!n) return i_ramble_node_open_fail(on_event, o.user_data, RAMBLE_E_OOM, 0, 0, sizeof *n);
    memset(n, 0, sizeof *n);
    n->pool = pool;                                  /* the node owns the pool now */
    n->alloc_dynamic = (alloc->page_realloc != NULL);
    arena = ramble_allocator_alloc(&n->pool, NULL, need);
    if (!arena){ RambleAllocator p = n->pool; ramble_allocator_reset(&p);
                 return i_ramble_node_open_fail(on_event, o.user_data, RAMBLE_E_OOM, 0, 0, need); }
    base = (uint8_t*)(((uintptr_t)arena+15u)&~(uintptr_t)15u);
    {   i_RambleBump b; memset(&b,0,sizeof b);
        b.base = base; b.cap = need - (size_t)(base - (uint8_t*)arena);
        i_ramble_node_layout(&b, max_peers, max_topics, &tc, &dc, &blocks); }

    if (!i_ramble_plat_startup()){
        RambleAllocator p = n->pool; ramble_allocator_reset(&p);
        return i_ramble_node_open_fail(on_event, o.user_data, RAMBLE_E_PLATFORM, 0, 0, 0);
    }

    n->fd = RAMBLE_SOCK_BAD;
    n->user_data = o.user_data;      /* set early so emit can stamp any open time error */
    n->on_event = on_event;
#ifdef RAMBLE_THREADS
    i_ramble_plat_mutex_init(&n->mu);
    i_ramble_plat_cond_init(&n->cv);
    /* opened for every node: it also wakes a plain poll when another thread sends. A
       platform whose loopback cannot carry it degrades, reported once. See spec/node.md */
    if (!i_ramble_plat_waker_open(&n->waker)){
        RambleEvent e; memset(&e, 0, sizeof e);
        e.kind = RAMBLE_ERROR; e.error = RAMBLE_E_WAKER;
        i_ramble_node_emit(n, &e);
    }
#endif
    n->domain = o.domain;
    n->net = o.net;
    n->announce_us = o.discovery.announce_interval_us ? o.discovery.announce_interval_us
                                                      : 3000000u;   /* discovery's default */
    n->match_wait_us = o.match_wait_ms < 0 ? 0u
                     : o.match_wait_ms ? (uint32_t)o.match_wait_ms * 1000u
                                       : (uint32_t)RAMBLE_MATCH_WAIT_MS * 1000u;
    n->match_epoch = 1;   /* a topic memo at 0 = never computed, so a first send always checks */
    n->user_on_message = on_message;
    n->arena = arena;
    n->handles = (RambleTopic**)blocks.handles;
    memset(n->handles, 0, (size_t)max_topics * sizeof(RambleTopic*));
    n->rx_buf = blocks.rx_buf; n->rx_buf_bytes = blocks.rx_buf_bytes;
    n->max_topics = max_topics;
    n->builtin_lo = user_topics;           /* the builtin block sits above the app's budget */
    n->max_peers = max_peers;
    n->meta_cap = dc.discovery.meta_cap;   /* the initial accept bound, self heals up */

    tc.on_message = i_ramble_node_on_message;     /* wrapped so on_message receives a RambleMsg */
    tc.on_event   = i_ramble_node_on_transport_event;
    tc.schema_check = i_ramble_node_schema_check;
    tc.source_time = i_ramble_node_source_time;
    tc.user       = n;
#ifdef RAMBLE_SHM
    n->shm_capable = (uint8_t)(n->alloc_dynamic && !o.disable_shm);   /* never in static mode */
    if (n->shm_capable){
        i_ramble_plat_host_uuid(n->shm_host);
        if (!i_ramble_plat_random(&n->shm_base, sizeof n->shm_base)) n->shm_base = i_ramble_plat_pid();
        n->shm_base ^= (uint64_t)i_ramble_plat_pid() << 32;    /* unique per process */
        n->shm_base &= ~(((uint64_t)1u << 19) - 1u);       /* low 19 bits: 3 class, 16 topic */
        if (n->shm_base == 0) n->shm_base = (uint64_t)1u << 19;
    }
    tc.on_shm = i_ramble_node_on_shm;
#endif

    n->transport = ramble_transport_init(blocks.transport, blocks.transport_bytes, &tc);
    if (!n->transport){ (void)i_ramble_node_open_fail(on_event, o.user_data, RAMBLE_E_OOM, 0, 0, 0); goto fail_threads; }
#ifdef RAMBLE_SHM
    {   uint32_t n_segments = (uint32_t)max_topics * RAMBLE_SHM_N_CLASSES; uint32_t i;
        n->shm_n_topics = max_topics;
        n->shm_pool = (void**)blocks.shm_pool;
        for (i=0;i<n_segments;i++) n->shm_pool[i]=NULL;
        n->shm_reader_segments = NULL; n->shm_reader_states = NULL;   /* lazy */
        n->shm_reader_cap = 0; n->shm_reader_count = 0;
    }
#endif

    /* the sans-IO node core, bound to discovery's peer table below once it exists */
    node_name_len = ramble_discovery_default_name(node_name, sizeof node_name, name);
    memcpy(n->name, node_name, node_name_len); n->name[node_name_len] = '\0';   /* snapshot copy */
    n->name_len = node_name_len;
    {   i_RambleNodeCoreConfig cc;
        memset(&cc, 0, sizeof cc);
        cc.transport = n->transport;   /* cc.discovery is bound after ramble_discovery_place */
        cc.n_topics = max_topics; cc.frag_size = ramble_clamp_frag(o.net.fragment_size);
        cc.on_event = i_ramble_node_on_event; cc.user = n;
        cc.alloc = i_ramble_node_alloc; cc.alloc_user = n;   /* backs the peer schema state */
        cc.fetch_details = o.fetch_details;
#ifdef RAMBLE_SHM
        cc.oob_capable = n->shm_capable; memcpy(cc.oob_host, n->shm_host, 16);
#endif
        n->core = i_ramble_node_core_init(blocks.node_core, blocks.node_core_bytes, &cc);
        if (!n->core){ (void)i_ramble_node_open_fail(on_event, o.user_data, RAMBLE_E_OOM, 0, 0, 0); goto fail_threads; }
    }

    /* the data socket is bound before discovery opens so we advertise its real port. No
       reuse: a unicast endpoint owns its port, so a collision fails loudly here */
    fd = i_ramble_plat_udp_open();
    if (fd==RAMBLE_SOCK_BAD){ (void)i_ramble_node_open_fail(on_event, o.user_data, RAMBLE_E_SOCKET, i_ramble_plat_last_socket_error(), 0, 0); goto fail_threads; }
    n->fd=fd;                       /* owned now: fail_sock closes it */
    if (!i_ramble_plat_bind(fd, 0, o.net.data_port, 0)){ (void)i_ramble_node_open_fail(on_event, o.user_data, RAMBLE_E_BIND, i_ramble_plat_last_socket_error(), o.net.data_port, 0); goto fail_sock; }
    local_port = i_ramble_plat_local_port(fd);
    if (local_port==0){ (void)i_ramble_node_open_fail(on_event, o.user_data, RAMBLE_E_SOCKET, 0, 0, 0); goto fail_sock; }
    i_ramble_plat_set_nonblock(fd);   /* never block in recv or send, poll drains the queue */
    i_ramble_plat_suppress_connreset(fd);  /* suppress WSAECONNRESET from a bounced send */
    if (o.net.recv_buffer_bytes) i_ramble_plat_set_rcvbuf(fd, (int)o.net.recv_buffer_bytes);
    if (o.net.send_buffer_bytes) i_ramble_plat_set_sndbuf(fd, (int)o.net.send_buffer_bytes);
    /* our advertised locator: the port we really bound and no address, unless the caller
       states one outright. See docs/discovery.md */
    dc.discovery.data_port = o.net.advertise_port ? o.net.advertise_port : local_port;
    if (o.net.self_ip){
        uint32_t naddr = i_ramble_plat_parse_ip(o.net.self_ip);
        /* 0 and 0xFFFFFFFF are inet_addr's failure value and the broadcast address, neither
           a unicast locator, so a bad string is a config error */
        if (!naddr || naddr == 0xFFFFFFFFu){
            (void)i_ramble_node_open_fail(on_event, o.user_data, RAMBLE_E_BAD_ADDRESS, 0, 0, 0);
            goto fail_sock;
        }
        i_ramble_plat_naddr_to_ip4(naddr, dc.discovery.self_ip);
        dc.discovery.self_ip_len = 4;
    }

    dc.discovery.on_event = i_ramble_node_core_on_disc_event;   /* the core demuxes peer events */
    dc.discovery.user     = n->core;
    dc.discovery.alloc_user = n;               /* the blob hook allocates from the node's pool */
    dc.discovery.name     = ramble_string(node_name, node_name_len);   /* discovery owned */
    /* the core builds our overlay, discovery wraps it in its blob after the locator and name */
    i_ramble_node_core_build_meta(n->core);
    dc.discovery.meta = i_ramble_node_core_meta(n->core);
    n->discovery = ramble_discovery_place(blocks.discovery, blocks.discovery_bytes, &dc);
    if (!n->discovery){
        RambleErrorKind err;   /* discovery's setup failure in our vocabulary */
        switch (ramble_discovery_last_error()){
        case RAMBLE_DISCOVERY_E_PLATFORM:   err = RAMBLE_E_PLATFORM;   break;
        case RAMBLE_DISCOVERY_E_SOCKET:     err = RAMBLE_E_SOCKET;     break;
        case RAMBLE_DISCOVERY_E_BIND:       err = RAMBLE_E_BIND;       break;
        case RAMBLE_DISCOVERY_E_MCAST_JOIN: err = RAMBLE_E_MCAST_JOIN; break;
        default:                          err = RAMBLE_E_OOM;        break;
        }
        (void)i_ramble_node_open_fail(on_event, o.user_data, err, ramble_discovery_last_os_error(),
                                    o.net.discovery_port, 0);
        goto fail_sock;
    }
    /* the node core delegates address resolution and per peer scratch to discovery's table */
    i_ramble_node_core_bind_discovery(n->core, ramble_discovery_state(n->discovery));
    i_ramble_node_core_set_self_name(n->core, ramble_string(n->name, strlen(n->name)));

    /* every unicast discovery send goes out of the data socket, so a NAT's per flow
       mappings are the ones the data will use. See spec/discovery.md */
    ramble_discovery_set_tx_fd(n->discovery, fd);

    /* the post open gather anchor: discovery solicits on startup, so every peer already
       out there answers within an RTT of the first poll */
    n->open_us = i_ramble_plat_now_us();

    /* the builtins last, since they create topics. A failed creation degrades and never
       fails the open */
    n->log_errors = (uint8_t)(!o.disable_error_logs && !o.disable_logs);
    n->creating_builtin = 1;               /* allocate from the builtin block */
    if (!o.disable_logs) i_ramble_node_logs_open(n);
#ifndef RAMBLE_NO_PATTERNS
    if (!o.disable_meta) i_ramble_patterns_meta_open(n);
#endif
    n->creating_builtin = 0;
    return n;

fail_sock:
    if (n->fd != RAMBLE_SOCK_BAD) i_ramble_plat_close(n->fd);
    n->fd = RAMBLE_SOCK_BAD;
fail_threads:
#ifdef RAMBLE_THREADS
    if (n->waker.fd != RAMBLE_SOCK_BAD) i_ramble_plat_waker_close(&n->waker);
    i_ramble_plat_cond_destroy(&n->cv);
    i_ramble_plat_mutex_destroy(&n->mu);
#endif
    i_ramble_plat_cleanup();
    { RambleAllocator p = n->pool; ramble_allocator_reset(&p); }   /* frees the node struct and arena */
    return NULL;
}

/* Dynamic mode growth: relocates the node into a bigger arena at the given counts. Message
 * buffers, SHM segments and the user held handles stay put. 0 leaves n unchanged. */
static int i_ramble_node_grow(RambleNode *n, uint16_t new_max_peers, uint16_t new_max_topics,
                            uint16_t want_meta_cap){
    RambleConfig tc; RambleDiscoveryNetConfig dc; i_RambleNodeBlocks nb; i_RambleBump b;
    RambleTransportState *nt; i_RambleNodeCore *ncore; RambleDiscovery *ndisc;
    void *new_arena, *old_arena = n->arena;
    uint8_t *nbase; size_t need;
    uint16_t old_max_topics = n->max_topics;
    /* the accept bound never shrinks: topic derived, previously grown, or requested */
    uint16_t new_meta_cap = ramble_meta_cap(new_max_topics);
    if (n->meta_cap > new_meta_cap) new_meta_cap = n->meta_cap;
    if (want_meta_cap    > new_meta_cap) new_meta_cap = want_meta_cap;

    if (!n->alloc_dynamic) return 0;
    if (new_max_peers <= n->max_peers && new_max_topics <= n->max_topics
        && new_meta_cap <= n->meta_cap) return 0;

    memset(&tc,0,sizeof tc); memset(&dc,0,sizeof dc);
    tc.topics=NULL; tc.n_topics=new_max_topics; tc.max_peers=new_max_peers;
    tc.allocator=i_ramble_node_alloc; tc.frag_size=n->net.fragment_size;
    dc.discovery.max_peers=new_max_peers; dc.discovery.meta_cap=new_meta_cap;
    dc.discovery.peer_user_bytes = i_ramble_node_core_peer_user_bytes();   /* scratch to match */
    /* sizing must match the live core's hook mode: no arena blob pool */
    dc.discovery.alloc = i_ramble_node_alloc; dc.discovery.alloc_user = n;

    memset(&b,0,sizeof b);
    i_ramble_node_layout(&b, new_max_peers, new_max_topics, &tc, &dc, &nb);
    need = b.offset + 32u;
    new_arena = ramble_allocator_alloc(&n->pool, NULL, need);
    if (!new_arena) return 0;
    nbase = (uint8_t*)(((uintptr_t)new_arena+15u)&~(uintptr_t)15u);
    memset(&b,0,sizeof b); b.base=nbase; b.cap=need-(size_t)(nbase-(uint8_t*)new_arena);
    i_ramble_node_layout(&b, new_max_peers, new_max_topics, &tc, &dc, &nb);

    /* migrate the three cores. Each leaves the old intact, so a failure frees the new
       arena and the old node keeps running, only refusing the growth */
    nt = ramble_transport_migrate(n->transport, nb.transport, nb.transport_bytes, new_max_peers, new_max_topics);
    if (!nt){ ramble_allocator_alloc(&n->pool, new_arena, 0); return 0; }
    ncore = i_ramble_node_core_migrate(n->core, nb.node_core, nb.node_core_bytes, new_max_topics);
    if (!ncore){ ramble_allocator_alloc(&n->pool, new_arena, 0); return 0; }
    ncore->transport = nt;                         /* re point the cross layer pointer */
    i_ramble_node_core_build_meta(ncore);              /* rebuild the blob in the new buffer */
    ndisc = ramble_discovery_migrate(n->discovery, nb.discovery, nb.discovery_bytes,
                                      new_max_peers, new_meta_cap, i_ramble_node_core_meta(ncore).data, ncore);
    if (!ndisc){ ramble_allocator_alloc(&n->pool, new_arena, 0); return 0; }
    i_ramble_node_core_bind_discovery(ncore, ramble_discovery_state(ndisc));   /* the relocated table */

    /* the handle pointer array. The handle structs are stable and do not move */
    memcpy(nb.handles, n->handles, (size_t)old_max_topics*sizeof(RambleTopic*));
    memset((RambleTopic**)nb.handles + old_max_topics, 0,
           (size_t)(new_max_topics-old_max_topics)*sizeof(RambleTopic*));

#ifdef RAMBLE_SHM
    if (n->shm_capable){
        uint32_t old_segs=(uint32_t)n->shm_n_topics*RAMBLE_SHM_N_CLASSES;
        uint32_t new_segs=(uint32_t)new_max_topics*RAMBLE_SHM_N_CLASSES, i;
        void **np = (void**)nb.shm_pool;
        for (i=0;i<new_segs;i++) np[i]=NULL;
        for (i=0;i<old_segs;i++) np[i]=n->shm_pool[i];   /* only the table moves */
        n->shm_pool=np; n->shm_n_topics=new_max_topics;
        /* the reader attach cache is hook allocated too, so attachments survive */
    }
#endif

    n->transport=nt; n->core=ncore; n->discovery=ndisc;
    n->handles=(RambleTopic**)nb.handles;
    n->rx_buf=nb.rx_buf; n->rx_buf_bytes=nb.rx_buf_bytes;
    n->max_topics=new_max_topics; n->max_peers=new_max_peers;
    n->meta_cap=new_meta_cap;
    ramble_allocator_alloc(&n->pool, old_arena, 0);    /* control structs only */
    n->arena=new_arena;
    return 1;
}

/* the local channel table behind RAMBLE_SELF reflection: every live handle as a peer sees it */
static void i_ramble_node_reflect_self(RambleNode *n){
    uint16_t i;
    i_ramble_node_core_self_begin(n->core);
    for (i = 0; i < n->max_topics; i++){
        RambleTopic *h = n->handles[i];
        const RambleQos *q;
        if (!h) continue;
        q = ramble_transport_topic_qos(n->transport, i);
        i_ramble_node_core_self_channel(n->core, i, ramble_string(h->name, h->name_len), h->kind, h->role,
                                      (uint8_t)(q ? q->reliability : 0),
                                      ramble_transport_topic_attrs(n->transport, i), h->schema);
    }
    i_ramble_node_core_self_end(n->core);
}

/* Our own interest changed: re advertise, replay known peers' interest against the new
 * state, stale the converged memos and kick so the announce goes out now. Lock held. */
static void i_ramble_node_readvertise(RambleNode *n){
    i_ramble_node_reflect_self(n);
    i_ramble_node_core_build_meta(n->core);
    ramble_discovery_advertise(n->discovery, i_ramble_node_core_meta(n->core));
    ramble_discovery_replay(n->discovery);
    n->match_epoch++;
    i_ramble_node_kick(n);
}

/* The shared topic create: the public create and the patterns layer's both funnel here.
 * kind, prefix_bytes and directed stamp the entity, allow_at permits the reserved '@'. */
static RambleTopic *i_ramble_node_create_impl(RambleNode *n, const char *name, RambleRole role,
                              const RambleSchema *schema, const RambleTopicOpts *opts,
                              uint8_t kind, uint8_t prefix_bytes, uint8_t directed, uint8_t attrs,
                              i_RambleSysMsgFn sys_msg, void *sys_user, int allow_at){
    RambleTopicDef def; RambleTopic *h; uint16_t idx; int acquired;
    int reuse = 0;
    if (!n || !name) return NULL;
    if (!allow_at){   /* '@' is reserved for pattern channels */
        const char *s = name;
        while (*s){ if (*s=='@') return NULL; s++; }
    }
    acquired = i_ramble_node_lock(n);
    if (!acquired) return NULL;   /* from a callback: a grow would move the arena mid delivery */
    if (n->creating_builtin){
        /* open time builtins fill their block. A full block degrades to no builtin, never a grow */
        idx = (uint16_t)(n->builtin_lo + n->n_builtin);
        if (idx >= n->max_topics){ i_ramble_node_unlock(n, acquired); return NULL; }
    } else if ((reuse = ramble_transport_topic_reuse_find(n->transport, name, kind, &idx)) != 0){
        /* a retired slot takes this create so churn never grows the table. Same identity,
           kind and schema (find = 2) relinks silently, anything else rebinds. See spec/interest.md */
    } else {
        idx = n->n_created;
        if (n->n_builtin && idx >= n->builtin_lo)
            idx = (uint16_t)(idx + n->n_builtin);   /* step over the builtin block */
        if (idx >= n->max_topics){       /* the reserve is full: grow or refuse (static) */
            uint16_t want = n->max_topics < 0x8000u ? (uint16_t)(n->max_topics*2u) : 0xFFFFu;
            if (want <= n->max_topics || !i_ramble_node_grow(n, n->max_peers, want, 0)){
                i_ramble_node_unlock(n, acquired);
                return NULL;
            }
        }
    }
    h = (RambleTopic*)i_ramble_node_alloc(n, NULL, sizeof *h);   /* stable: outlives any arena grow */
    if (!h){ i_ramble_node_unlock(n, acquired); return NULL; }
    memset(h, 0, sizeof *h);
    if (opts) h->qos = opts->qos;
    if (opts && opts->reflect_from_mesh && kind == RAMBLE_KIND_TOPIC){
        /* fill what the caller left unspecified from the mesh: a reader takes the
           provider's schema, a writer the widest every reader accepts */
        const RambleSchema *ms = NULL; uint8_t rel = 0;
        h->reflect = 1;
        i_ramble_node_core_reflect_pick(n->core, RAMBLE_ENTITY_TOPIC, name, 0,
                                      ramble_role_pubs((uint8_t)role), &ms, &rel, &h->generation);
        if (!schema) schema = ms;
        if (!h->qos.reliability) h->qos.reliability = rel ? RAMBLE_RELIABLE : RAMBLE_BEST_EFFORT;
    }
    if (schema){   /* copied into node memory so the caller's schema need not outlive the topic */
        RambleBytes w = ramble_schema_wire(schema);
        h->schema = ramble_schema_parse(w.data, w.len, i_ramble_node_alloc, n);
        if (!h->schema){ i_ramble_node_alloc(n, h, 0); i_ramble_node_unlock(n, acquired); return NULL; }
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
            uint64_t new_hash = h->schema ? ramble_schema_hash(h->schema) : 0;
            int changed = (reuse != 2)
                       || (new_hash != i_ramble_node_core_topic_schema_hash(n->core, idx));
            uint32_t rb = changed
                ? ramble_discovery_meta_version(ramble_discovery_state(n->discovery)) + 1u : 0u;
            rc = ramble_transport_topic_reuse(n->transport, idx, &def, changed, rb);
            if (rc == 0 && changed) i_ramble_node_core_topic_rebound(n->core, idx);
        } else {
            rc = ramble_transport_topic_define(n->transport, idx, &def);
        }
        if (rc != 0){
            if (h->schema) ramble_schema_free(h->schema, i_ramble_node_alloc, n);
            i_ramble_node_alloc(n, h, 0);
            i_ramble_node_unlock(n, acquired);
            return NULL;
        }
    }
    h->n = n; h->index = idx; h->prefix_bytes = prefix_bytes; h->kind = kind;
    /* a response prefix is followed by [u8 len][message] on the wire, derived from the
       kind so every creator of the kind splits alike */
    h->prefix_string = (uint8_t)(kind == RAMBLE_KIND_FUNC_RSP || kind == RAMBLE_KIND_TASK_RSP);
    h->role = (uint8_t)role;
    h->sys_on_message = sys_msg; h->sys_msg_user = sys_user;
    {   /* the stable name copy, queued views must not point into the arena */
        size_t nl = strlen(name);
        if (nl > RAMBLE_TOPIC_NAME_MAX) nl = RAMBLE_TOPIC_NAME_MAX;
        memcpy(h->name, name, nl);
        h->name_len = (uint8_t)nl;
    }
    if (def.qos.queue_bytes)   /* queued from creation, best effort on OOM (take retries) */
        (void)i_ramble_node_queue_ensure(n, h, &def.qos);
    if (h->schema || reuse)   /* advertise and gate with it. A reused slot must also clear
                                 the retired occupant's fingerprint when the new topic is untyped */
        i_ramble_node_core_set_topic_schema(n->core, idx, h->schema);
    n->handles[idx] = h;
    if (n->creating_builtin) n->n_builtin++;
    else if (!reuse) n->n_created++;   /* a reused slot is already inside the dense region */
    /* the handle is installed before the publish: the replay fires peer events into the
       patterns layer and the app, which must never see a half created topic */
    i_ramble_node_readvertise(n);
    i_ramble_node_unlock(n, acquired);
    return h;
}

RambleTopic *ramble_node_create_topic(RambleNode *n, const char *name, RambleRole role,
                                      const RambleSchema *schema, const RambleTopicOpts *opts){
    return i_ramble_node_create_impl(n, name, role, schema, opts, RAMBLE_KIND_TOPIC, 0, 0, 0, NULL, NULL, 0);
}

RambleTopic *i_ramble_node_create_pattern_topic(RambleNode *n, const char *name, RambleRole role,
                              const RambleSchema *schema, const RambleTopicOpts *opts,
                              uint8_t kind, uint8_t prefix_bytes, uint8_t directed, uint8_t attrs,
                              i_RambleSysMsgFn on_msg, void *on_msg_user){
    return i_ramble_node_create_impl(n, name, role, schema, opts, kind, prefix_bytes, directed, attrs,
                                   on_msg, on_msg_user, 1);
}

/* the built in log topics */

static int i_ramble_node_do_send(RambleNode *n, uint16_t topic_index, RambleBytes data,
                               uint64_t capture_us, int may_wait);

static const char *const i_ramble_log_topic_names[3] =
    { "@ramble/log/error", "@ramble/log/warn", "@ramble/log/info" };

/* Creates the three log topics at open: reliable, keep_last = catch_up as the replayable
 * history, publish only, and no backpressure wait. NULL handles on OOM, never a failed open. */
static void i_ramble_node_logs_open(RambleNode *n){
    RambleTopicOpts topt; int lvl;
    n->log_schema = ramble_schema_compile(i_ramble_node_alloc, n,
        "RambleLog { wall_us: u64, mono_us: u64, text: string }", NULL);
    if (!n->log_schema) return;
    for (lvl = 0; lvl < 3; lvl++){
        memset(&topt, 0, sizeof topt);
        topt.qos.reliability = RAMBLE_RELIABLE;
        topt.qos.keep_last = topt.qos.catch_up = (lvl == RAMBLE_LOG_INFO) ? 8 : 16;
        /* backpressure_wait_us stays 0, so logs never block */
        n->log_topics[lvl] = i_ramble_node_create_impl(n, i_ramble_log_topic_names[lvl],
                                 RAMBLE_PUB_ONLY, n->log_schema, &topt,
                                 RAMBLE_KIND_TOPIC, 0, 0, 0, NULL, NULL, 1);
    }
}

/* Builds one RambleLog message and publishes it on the level's topic. locked = 1 is the
 * mirror flush under the node lock, 0 the public thread safe path. */
static int i_ramble_node_log_publish(RambleNode *n, RambleLogLevel level, const char *text,
                                   size_t text_len, uint64_t wall_us, uint64_t mono_us,
                                   int locked){
    uint8_t msg[20u + RAMBLE_LOG_MAX];   /* the fixed section (16) and the text frame header (4) */
    uint32_t len;
    RambleTopic *h = n->log_topics[level];
    if (!h || !n->log_schema) return RAMBLE_ERR_NOSYS;
    if (!ramble_schema_message_default(n->log_schema, msg, sizeof msg)) return RAMBLE_ERR_OOM;
    ramble_set_uint(msg, sizeof msg, n->log_schema, "wall_us", wall_us);
    ramble_set_uint(msg, sizeof msg, n->log_schema, "mono_us", mono_us);
    if (!ramble_set_string(msg, sizeof msg, n->log_schema, "text", ramble_string(text, text_len)))
        return RAMBLE_ERR_TOO_BIG;
    len = ramble_schema_msg_len(n->log_schema, msg, sizeof msg);
    if (locked) return i_ramble_node_do_send(n, h->index, ramble_bytes(msg, len), 0, 0);
    return ramble_topic_send(h, ramble_bytes(msg, len), NULL);
}

int ramble_node_log(RambleNode *n, RambleLogLevel level, const char *fmt, ...){
    char text[RAMBLE_LOG_MAX]; int tn;
    va_list ap;
    if (!n || (int)level < 0 || level > RAMBLE_LOG_INFO || !fmt) return RAMBLE_ERR_NO_TOPIC;
    if (!n->log_topics[level]) return RAMBLE_ERR_NOSYS;
    va_start(ap, fmt);
    tn = vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (tn < 0) tn = 0;                                       /* an encoding error: an empty line */
    if (tn >= (int)sizeof text) tn = (int)sizeof text - 1;    /* truncated at RAMBLE_LOG_MAX */
    return i_ramble_node_log_publish(n, level, text, (size_t)tn,
                                   i_ramble_plat_wall_us(), i_ramble_plat_now_us(), 0);
}

int ramble_node_log_text(RambleNode *n, RambleLogLevel level, const char *text, int len){
    size_t tl;
    if (!n || (int)level < 0 || level > RAMBLE_LOG_INFO || !text) return RAMBLE_ERR_NO_TOPIC;
    if (!n->log_topics[level]) return RAMBLE_ERR_NOSYS;
    tl = len < 0 ? strlen(text) : (size_t)len;
    if (tl >= RAMBLE_LOG_MAX) tl = RAMBLE_LOG_MAX - 1;   /* match the variadic path's truncation */
    return i_ramble_node_log_publish(n, level, text, tl,
                                   i_ramble_plat_wall_us(), i_ramble_plat_now_us(), 0);
}

RambleTopic *ramble_node_log_topic(RambleNode *n, RambleLogLevel level){
    if (!n || (int)level < 0 || level > RAMBLE_LOG_INFO) return NULL;
    return n->log_topics[level];
}

/* Publishes the pending error mirror ring under the lock. A coalesced burst appends its
 * count and overflow between flushes becomes one summary line. */
static void i_ramble_node_log_flush(RambleNode *n){
    uint8_t i, cnt;
    if (!n->log_topics[RAMBLE_LOG_ERROR]){ n->log_pend_n = 0; n->log_pend_dropped = 0; return; }
    n->log_flushing = 1;
    cnt = n->log_pend_n; n->log_pend_n = 0;
    for (i = 0; i < cnt; i++){
        i_RambleLogPend *p = &n->log_pend[i];
        char line[sizeof p->text + 16]; int ln;
        ln = (p->count > 1) ? snprintf(line, sizeof line, "%s (x%u)", p->text, (unsigned)p->count)
                            : snprintf(line, sizeof line, "%s", p->text);
        if (ln < 0) ln = 0;
        if (ln >= (int)sizeof line) ln = (int)sizeof line - 1;
        i_ramble_node_log_publish(n, RAMBLE_LOG_ERROR, line, (size_t)ln, p->wall_us, p->mono_us, 1);
    }
    if (n->log_pend_dropped){
        char line[64];
        int ln = snprintf(line, sizeof line, "log mirror overflow: %u error events dropped",
                          (unsigned)n->log_pend_dropped);
        n->log_pend_dropped = 0;
        i_ramble_node_log_publish(n, RAMBLE_LOG_ERROR, line, (size_t)(ln < 0 ? 0 : ln),
                                i_ramble_plat_wall_us(), i_ramble_plat_now_us(), 1);
    }
    n->log_flushing = 0;
}

/* the longest one poll tick drains RX before yielding to discovery and send */
#ifndef RAMBLE_RX_BUDGET_US
#define RAMBLE_RX_BUDGET_US 5000u
#endif

/* Drains the socket into the transport until empty or past the deadline. A full drain
 * avoids NACK storms. Distinct from the public ramble_topic_drain. */
static void i_ramble_node_rx_drain(RambleNode *n, i_RambleSock fd, uint64_t deadline){
    uint8_t *buf = n->rx_buf;
    for (;;){
        uint8_t src_ip[4]; uint16_t src_port;
        int r = i_ramble_plat_recv(fd, buf, n->rx_buf_bytes, src_ip, &src_port);
        if (r<0){
            if (i_ramble_plat_would_block()) break;        /* the queue is empty */
            {   /* a hard error: report and stop this tick, never spin on a wedged socket */
                RambleEvent e; memset(&e, 0, sizeof e);
                e.kind = RAMBLE_ERROR; e.error = RAMBLE_E_RECV; e.os_error = i_ramble_plat_last_socket_error();
                i_ramble_node_emit(n, &e);
            }
            break;
        }
        if (r>0){
            if (r>=4 && buf[0]=='u' && buf[1]=='D' && buf[2]=='S' && buf[3]=='C'){
                /* a unicast announce aimed at our data port goes to discovery with its
                   source port, so a translated peer's observed source can bind */
                RambleDiscoveryAddr src;
                memset(&src, 0, sizeof src);
                memcpy(src.ip, src_ip, 4); src.ip_len = 4; src.port = src_port;
                ramble_discovery_feed(n->discovery, &src, ramble_bytes(buf, (size_t)r));
            } else if (r>=5 && buf[0]=='u' && buf[1]=='D' && buf[2]=='T' && buf[3]=='L'){
                /* the pairwise detail exchange, stateless: a request is answered to its
                   source, a response feeds the pending match cycle. See spec/interest.md */
                if (buf[4]==RAMBLE_DETAIL_REQ){
                    RambleBytes resp = i_ramble_node_core_detail_respond(n->core, n->domain,
                                                                     ramble_bytes(buf, (size_t)r));
                    if (resp.len) i_ramble_plat_send(fd, resp.data, resp.len, src_ip, src_port);
                    {   /* the request names the version the peer applied: feed the rebind hold */
                        uint32_t from;
                        if (i_ramble_node_core_id_for_addr(n->core, src_ip, src_port, &from)
                            && i_ramble_node_core_seen_version(n->core, from,
                                   ramble_detail_meta_version(ramble_bytes(buf, (size_t)r))))
                            n->match_epoch++;
                    }
                } else if (buf[4]==RAMBLE_DETAIL_RESP){
                    uint32_t from;
                    if (i_ramble_node_core_id_for_addr(n->core, src_ip, src_port, &from))
                        i_ramble_node_core_apply_details(n->core, n->domain, from,
                                                       ramble_bytes(buf, (size_t)r));
                } else if (buf[4]==RAMBLE_INTEREST_REQ){
                    /* interest paging: serve one page. Not a rebind hold confirmation, the
                       peer is still fetching that version */
                    RambleBytes resp = i_ramble_node_core_interest_respond(n->core, n->domain,
                                                                       ramble_bytes(buf, (size_t)r));
                    if (resp.len) i_ramble_plat_send(fd, resp.data, resp.len, src_ip, src_port);
                } else if (buf[4]==RAMBLE_INTEREST_RESP){
                    uint32_t from;
                    if (i_ramble_node_core_id_for_addr(n->core, src_ip, src_port, &from))
                        i_ramble_node_core_apply_interest_page(n->core, n->domain, from,
                                                             ramble_bytes(buf, (size_t)r));
                }
            } else {
                uint32_t from;
                if (i_ramble_node_core_id_for_addr(n->core, src_ip, src_port, &from))
                    ramble_transport_on_datagram(n->transport, from, ramble_bytes(buf, (size_t)r), i_ramble_plat_now_us());
            }
        }
        if (i_ramble_plat_now_us() >= deadline) break;      /* yield to discovery and send */
    }
}

/* threaded mode: the longest a send waits for a poller to hand unsent history to the
 * wire before overwriting it. One kicked pass normally clears it in microseconds. */
#ifndef RAMBLE_UNSENT_WAIT_US
#define RAMBLE_UNSENT_WAIT_US 20000u
#endif

/* Caps a wait at the transport's next timer, discovery's next announce and the patterns
 * tick, so each fires on time with no traffic. Lock held, pure compute. */
static int i_ramble_node_wait_ms(RambleNode *n, int timeout_ms){
    uint64_t next = ramble_transport_next_deadline_us(n->transport);
    uint64_t due  = ramble_discovery_next_due_us(ramble_discovery_state(n->discovery));
    if (timeout_ms < 0) timeout_ms = 0;
    if (!next || due < next) next = due;   /* due is a real time (0 = now), next 0 = none */
    /* the patterns tick */
    if (n->sys_tick_next && (!next || n->sys_tick_next < next)) next = n->sys_tick_next;
    if (next){
        uint64_t t0 = i_ramble_plat_now_us();
        uint64_t us = (next > t0) ? next - t0 : 0;
        int ms = (us >= (uint64_t)timeout_ms*1000u) ? timeout_ms   /* rounded up: no busy spin */
                                                    : (int)((us + 999u)/1000u);
        if (ms < timeout_ms) timeout_ms = ms;
    } else timeout_ms = 0;                 /* something is due right now */
    return timeout_ms;
}

static int i_ramble_node_gather_done(RambleNode *n, uint64_t now);   /* defined with the match wait */

/* One poll tick under the node lock: deferred grows, the wait, discovery's tick, RX drain,
 * TX flush. The one poll body. outer = 1 lets the wait drop the lock. See spec/node.md. */
static void i_ramble_node_poll_locked(RambleNode *n, int timeout_ms, int outer){
    uint8_t buf[RAMBLE_DGRAM_MAX]; uint32_t to; size_t out_len; uint64_t now;
    i_RamblePollfd pfd[5]; int nfds = 0, wait_ms, poll_rc;
    int disc_slot = 0, disc_n = 0;
#ifdef RAMBLE_THREADS
    int waker_slot = -1;
#endif

    /* a peer was refused last tick for lack of slots: grow now, between ticks */
    if (n->grow_pending){
        uint16_t want = n->max_peers < 0x8000u ? (uint16_t)(n->max_peers*2u) : 0xFFFFu;
        n->grow_pending = 0;
        if (want > n->max_peers) i_ramble_node_grow(n, want, n->max_topics, 0);
    }
    /* a peer's blob exceeded the accept bound last tick: raise it, then solicit so the
       peer re sends into buffers that fit. A failed grow is remembered, see spec/node.md */
    if (n->meta_grow_need){
        uint16_t want = n->meta_grow_need;
        n->meta_grow_need = 0;
        if (want > n->meta_cap){
            if (i_ramble_node_grow(n, n->max_peers, n->max_topics, want)){
                n->meta_grow_failed = 0;
                ramble_discovery_solicit(ramble_discovery_state(n->discovery));
            } else
                n->meta_grow_failed = want;
        }
    }

    /* the discovery tick runs after the wait off its revents, so a pass costs one syscall */
    wait_ms = i_ramble_node_wait_ms(n, timeout_ms);
    memset(pfd, 0, sizeof pfd);
    pfd[nfds].fd = n->fd; pfd[nfds].events = RAMBLE_POLLIN; nfds++;
#ifdef RAMBLE_THREADS
    if (n->waker.fd != RAMBLE_SOCK_BAD){
        waker_slot = nfds;
        pfd[nfds].fd = n->waker.fd; pfd[nfds].events = RAMBLE_POLLIN; nfds++;
    }
#endif
    {   /* discovery's sockets join the wait so an announce cuts a long sleep short. The
           fds are stable by value: a grow relocates structs, never sockets */
        i_RambleSock dfds[2]; int i;
        disc_slot = nfds; disc_n = ramble_discovery_pollfds(n->discovery, dfds);
        for (i = 0; i < disc_n; i++){ pfd[nfds].fd = dfds[i]; pfd[nfds].events = RAMBLE_POLLIN; nfds++; }
    }

#ifdef RAMBLE_THREADS
    if (outer){
        /* the wait runs unlocked so a sender on another thread is never blocked behind
           it. pollers_sleeping is a counter, several threads may poll the same node */
        n->pollers_sleeping++;
        i_ramble_node_unlock_raw(n);
        poll_rc = i_ramble_plat_poll(pfd, nfds, wait_ms);
        i_ramble_node_lock_raw(n);
        n->pollers_sleeping--;
    } else {
        poll_rc = i_ramble_plat_poll(pfd, nfds, wait_ms);
    }
    if (waker_slot >= 0 && (pfd[waker_slot].revents & RAMBLE_POLLIN)){
        /* revents gated, so an idle pass costs no drain syscall. A kick still in flight
           wakes the next wait, which drains and clears it, so no kick is ever lost */
        i_ramble_plat_waker_drain(&n->waker);
        n->wake_signaled = 0;
    }
#else
    (void)outer;
    poll_rc = i_ramble_plat_poll(pfd, nfds, wait_ms);
#endif
    if (poll_rc < 0){   /* the wait itself failed, a real fault such as a bad fd */
        RambleEvent e; memset(&e, 0, sizeof e);
        e.kind = RAMBLE_ERROR; e.error = RAMBLE_E_POLL; e.os_error = i_ramble_plat_last_socket_error();
        i_ramble_node_emit(n, &e);
    }

    /* the discovery tick off this wait's readiness. A failed wait leaves revents zeroed,
       so the clock driven work still runs */
    ramble_discovery_service(n->discovery,
                           disc_n > 0 && (pfd[disc_slot].revents & RAMBLE_POLLIN) != 0,
                           disc_n > 1 && (pfd[disc_slot + 1].revents & RAMBLE_POLLIN) != 0);
    if (pfd[0].revents & RAMBLE_POLLIN)
        i_ramble_node_rx_drain(n, n->fd, i_ramble_plat_now_us() + RAMBLE_RX_BUDGET_US);

    /* mirrored errors publish now, before the TX pull, so the lines ride this pass */
    if (n->log_pend_n || n->log_pend_dropped) i_ramble_node_log_flush(n);

    now=i_ramble_plat_now_us();
    /* the detail requester: drain queued requests and rearm every active peer once per
       announce interval while any went out. A sweep that sends nothing disarms the timer */
    if (i_ramble_node_core_detail_any(n->core) || (n->next_detail_us && now >= n->next_detail_us)){
        i_RambleNodeDest dst; size_t len; int sent = 0;
        if (n->next_detail_us && now >= n->next_detail_us)
            i_ramble_node_core_detail_rearm(n->core);
        while ((len = i_ramble_node_core_detail_req_next(n->core, n->domain, buf, sizeof buf, &dst)) != 0){
            i_ramble_plat_send(n->fd, buf, len, dst.ip, dst.port);   /* best effort: retries heal */
            sent = 1;
        }
        n->next_detail_us = sent ? now + n->announce_us : 0;
    }
    /* the core already consumed any held datagram, so retry it before pulling new */
    if (n->tx_hold_len && i_ramble_node_tx(n, n->tx_hold_peer, n->tx_hold, n->tx_hold_len))
        n->tx_hold_len = 0;
    if (!n->tx_hold_len)
        while (ramble_transport_poll_send(n->transport,&to,buf,sizeof buf,&out_len,now)){
            if (!i_ramble_node_tx(n, to, buf, out_len)){
                memcpy(n->tx_hold, buf, out_len);
                n->tx_hold_len = out_len; n->tx_hold_peer = to;
                break;          /* the TX buffer is full: yield this tick */
            }
            now=i_ramble_plat_now_us();
        }

    /* latch the post open gather the moment it settles, so a later create's replay events
       cannot reset the quiet clock and make a first send re wait */
    if (!n->gather_done) (void)i_ramble_node_gather_done(n, now);

    /* the patterns tick runs under the lock like a callback and returns the next deadline */
    if (n->sys_tick) n->sys_tick_next = n->sys_tick(n->sys_user, i_ramble_plat_now_us());

#ifdef RAMBLE_THREADS
    n->work_seq++;
    if (n->cv_waiters) i_ramble_plat_cond_broadcast(&n->cv);   /* acks or TX may have progressed */
#endif
}

int ramble_node_poll(RambleNode *n, int timeout_ms){
    int acquired;
    if (!n) return RAMBLE_ERR_STATE;
    acquired = i_ramble_node_lock(n);
#ifdef RAMBLE_THREADS
    if (!acquired || n->svc_running){
        /* from inside a callback, or a service thread owns the loop: refuse loudly */
        i_ramble_node_unlock(n, acquired);
        return RAMBLE_ERR_STATE;
    }
#endif
    i_ramble_node_poll_locked(n, timeout_ms, acquired);
    i_ramble_node_unlock(n, acquired);
    return 0;
}

/* The one blocking wait skeleton: sample the clock, ask done (which states the deadline),
 * run periodic, then sleep on the condvar or pump. See spec/node.md. */
typedef struct {
    /* nonzero = the wait is over, *deadline is this iteration's bound, UINT64_MAX = none */
    int    (*done)(RambleNode *n, void *ctx, uint64_t now, uint64_t *deadline);
    void   (*periodic)(RambleNode *n, void *ctx, uint64_t now);   /* optional, NULL = nothing */
    void    *ctx;
    uint64_t cv_cap_us;      /* the longest single sleep, 0 = the whole remaining time */
    int      pump_ms;        /* the pump tick in ms, 0 = the remaining time */
    int      pump_outer;     /* poll_locked's outer flag: 1 lets the pump's wait drop the lock */
    int      pump_after_svc; /* 1 = a service thread stopping under us finishes in the pump,
                                0 = the wait ends there with RAMBLE__WAIT_SVC_GONE */
} i_RambleWait;

/* what ended the wait: the predicate, the deadline, or the service thread going away */
#define RAMBLE__WAIT_DONE     1
#define RAMBLE__WAIT_TIMEOUT  0
#define RAMBLE__WAIT_SVC_GONE (-1)

static int i_ramble_node_wait_until(RambleNode *n, const i_RambleWait *w){
    for (;;){
        uint64_t deadline = 0, left, now = i_ramble_plat_now_us();
        if (w->done(n, w->ctx, now, &deadline)) return RAMBLE__WAIT_DONE;
        if (now >= deadline) return RAMBLE__WAIT_TIMEOUT;
        if (w->periodic) w->periodic(n, w->ctx, now);
        left = deadline - now;
        if (w->cv_cap_us && left > w->cv_cap_us) left = w->cv_cap_us;
#ifdef RAMBLE_THREADS
        if (n->svc_running){
            n->cv_waiters++;
            i_ramble_node_cv_wait(n, left);   /* mu drops: nothing cached survives */
            n->cv_waiters--;
            if (!n->svc_running && !w->pump_after_svc) return RAMBLE__WAIT_SVC_GONE;
            continue;                       /* stopped under us: the next pass pumps */
        }
#endif
        {   int ms = w->pump_ms;
            if (!ms){                       /* derive the tick from the time left */
                uint64_t left_ms = left / 1000u;
                ms = left < 1000u ? 1 : (left_ms > 0x7FFFFFFFu ? 0x7FFFFFFF : (int)left_ms);
            }
            i_ramble_node_poll_locked(n, ms, w->pump_outer);
        }
    }
}

/* The kick a wait owes before it sleeps: only a service thread needs telling, a pump is
 * the poller and services the change on its own next tick. */
static void i_ramble_node_wait_kick(RambleNode *n, void *ctx, uint64_t now){
    (void)n; (void)ctx; (void)now;
#ifdef RAMBLE_THREADS
    if (n->svc_running) i_ramble_node_kick(n);
#endif
}

/* the send path match wait, see spec/interest.md */

static int i_ramble_node_settled(RambleNode *n, uint64_t start, uint64_t now);   /* with settle */

/* Has the post open gather completed: the settle predicate anchored at open, latched
 * once true. */
static int i_ramble_node_gather_done(RambleNode *n, uint64_t now){
    if (n->gather_done) return 1;
    if (!i_ramble_node_settled(n, n->open_us, now)) return 0;
    n->gather_done = 1;
    return 1;
}

/* Would a send now race a forming match: 1 while the gather is unsettled or verdicts are
 * in flight, else 0, memoized against the topology epoch. Lock held. */
static int i_ramble_node_topic_unsettled(RambleNode *n, RambleTopic *h, uint64_t now){
    if (h->resolve_epoch == n->match_epoch) return 0;   /* converged at this topology */
    if (!i_ramble_node_gather_done(n, now)) return 1;
    if (i_ramble_node_core_topic_unresolved(n->core, h->index) > 0) return 1;
    h->resolve_epoch = n->match_epoch;
    return 0;
}

/* The match wait's predicate: done once a subscriber matched, or once matching converged
 * with none. The match count is re read from the transport every call. */
typedef struct {
    RambleTopic *h;
    uint16_t   index;
    uint64_t   deadline;
    int        matched;   /* the count the wait ended on */
} i_RambleMatchWait;

static int i_ramble_node_match_wait_done(RambleNode *n, void *ctx, uint64_t now, uint64_t *deadline){
    i_RambleMatchWait *c = (i_RambleMatchWait*)ctx;
    *deadline = c->deadline;
    c->matched = ramble_transport_publisher_match_count(n->transport, c->index);
    if (c->matched) return 1;
    return !i_ramble_node_topic_unsettled(n, c->h, now);
}

/* Bounded wait for a forming match before a zero subscriber send commits. Returns the
 * matched count. loud fires RAMBLE_E_UNMATCHED_SEND on timeout, a pattern write passes 0. */
static int i_ramble_node_match_wait(RambleNode *n, uint16_t topic_index, RambleTopic *h, int loud){
    i_RambleMatchWait c; i_RambleWait w = { 0 };
    c.h = h; c.index = topic_index; c.matched = 0;
    c.deadline = i_ramble_plat_now_us() + n->match_wait_us;
    w.done = i_ramble_node_match_wait_done; w.ctx = &c;
    w.cv_cap_us = 50000u;   /* re check the clock: gather settling is partly time driven */
    w.pump_ms   = 1;        /* a nested tick: the lock stays held */
    if (i_ramble_node_wait_until(n, &w) == RAMBLE__WAIT_TIMEOUT && loud){
        RambleEvent e; memset(&e, 0, sizeof e);
        e.kind = RAMBLE_ERROR; e.error = RAMBLE_E_UNMATCHED_SEND;
        e.topic = topic_index; e.topic_name = i_ramble_node_topic_name(n, topic_index);
        i_ramble_node_emit(n, &e);
    }
    return c.matched;
}

#ifdef RAMBLE_THREADS
/* The threaded flow control wait's predicate: done when neither eviction looms. Unacked
 * history is bounded by qos.backpressure_wait_us, unsent history by RAMBLE_UNSENT_WAIT_US. */
typedef struct {
    uint16_t index;
    uint64_t rel_deadline;      /* 0 = no reliable bound */
    uint64_t unsent_deadline;
    uint64_t seq0;              /* the work_seq snapshot, the completed pass test */
    int      waited;            /* at least one sleep happened, gates the stats */
} i_RambleSendWait;

static int i_ramble_node_send_wait_done(RambleNode *n, void *ctx, uint64_t now, uint64_t *deadline){
    i_RambleSendWait *c = (i_RambleSendWait*)ctx;
    int evict_unacked = c->rel_deadline && ramble_transport_send_would_evict(n->transport, c->index);
    int evict_unsent  = ramble_transport_send_would_evict_unsent(n->transport, c->index, NULL, NULL);
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
static void i_ramble_node_send_wait_tick(RambleNode *n, void *ctx, uint64_t now){
    ((i_RambleSendWait*)ctx)->waited = 1;
    i_ramble_node_wait_kick(n, ctx, now);
}
#endif /* RAMBLE_THREADS */

/* The unthreaded pump's predicate: done once the send no longer evicts unacked history.
 * It also samples the in pump probe on a timer, after a tick and before the re check. */
typedef struct {
    uint16_t index;
    uint64_t deadline;
    uint64_t t0, sample_last, interval;
    uint32_t polls, polls_idle;
    int      polled;
    RambleRepairStats prev;
} i_RamblePumpWait;

static int i_ramble_node_pump_wait_done(RambleNode *n, void *ctx, uint64_t now, uint64_t *deadline){
    i_RamblePumpWait *c = (i_RamblePumpWait*)ctx;
    *deadline = c->deadline;
    if (n->pump_probe && c->polled){
        c->polls++;
        if (ramble_transport_repair_pending(n->transport, c->index) == 0) c->polls_idle++;
        if (now - c->sample_last >= c->interval){
            RambleRepairStats sample_now; RamblePumpSample sample;
            ramble_transport_repair_stats(n->transport, c->index, &sample_now);
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
    return !ramble_transport_send_would_evict(n->transport, c->index);
}

static void i_ramble_node_pump_wait_tick(RambleNode *n, void *ctx, uint64_t now){
    (void)n; (void)now;
    ((i_RamblePumpWait*)ctx)->polled = 1;   /* a tick runs next: sample on the call after it */
}

/* Publishes on a topic index: the match wait, the flow control wait, then SHM or UDP.
 * may_wait = 0 is a reentrant send from a callback: it never blocks or runs the loop. */
static int i_ramble_node_do_send_ex(RambleNode *n, uint16_t topic_index, RambleBytes hdr, RambleBytes data,
                                  uint64_t capture_us, int directed, uint32_t to_peer,
                                  int may_wait){
    /* one O(1) count gates the per send fast paths: an unsubscribed topic skips them all */
    int matched = ramble_transport_publisher_match_count(n->transport, topic_index);
    const RambleQos *q = ramble_transport_topic_qos(n->transport, topic_index);
    /* the source stamp is ordinary payload, so every size rule here counts it. qos is
       immutable, so this stays valid across the waits that may re fetch q */
    size_t ts_bytes = (q && q->no_timestamp) ? 0u : (size_t)RAMBLE_TIMESTAMP_BYTES;
    size_t cap_bytes = (ts_bytes && capture_us) ? (size_t)RAMBLE_CAPTURE_BYTES : 0u;
    size_t len = ts_bytes + cap_bytes + hdr.len + data.len;
    int guarded = 0;   /* the unsent eviction check runs after the wait */

    /* a send to zero subscribers while a match forms waits for it to converge. A topic
       that retains history is exempt, since its history replays. See spec/interest.md */
    if (!matched && topic_index < n->max_topics
        && !(q && q->reliability == RAMBLE_RELIABLE && q->catch_up > 0)){
        RambleTopic *h = n->handles[topic_index];
        if (h && ramble_role_pubs(h->role)
              && i_ramble_node_topic_unsettled(n, h, i_ramble_plat_now_us())){
            if (may_wait && n->match_wait_us){
                matched = i_ramble_node_match_wait(n, topic_index, h, 1);
                q = ramble_transport_topic_qos(n->transport, topic_index);   /* the arena may move */
            } else {
                RambleEvent e; memset(&e, 0, sizeof e);
                e.kind = RAMBLE_ERROR; e.error = RAMBLE_E_UNMATCHED_SEND;
                e.topic = topic_index; e.topic_name = i_ramble_node_topic_name(n, topic_index);
                i_ramble_node_emit(n, &e);
            }
        }
    }

#ifdef RAMBLE_THREADS
    if (matched && n->svc_running){
        guarded = 1;
        /* threaded: wait on the condvar for the poller's progress. A service thread that
           stops under us ends the wait, there is no poller left */
        if (may_wait){
            i_RambleSendWait c; i_RambleWait w = { 0 };
            uint64_t t0 = i_ramble_plat_now_us();
            c.index = topic_index;
            c.rel_deadline = (q && q->backpressure_wait_us) ? t0 + q->backpressure_wait_us : 0;
            c.unsent_deadline = t0 + RAMBLE_UNSENT_WAIT_US;
            c.seq0 = n->work_seq;
            c.waited = 0;
            w.done = i_ramble_node_send_wait_done; w.periodic = i_ramble_node_send_wait_tick;
            w.ctx = &c;
            w.pump_ms = 1;   /* a pump is unreachable here, bounded regardless */
            i_ramble_node_wait_until(n, &w);   /* every outcome proceeds: KEEP_LAST applies */
            if (c.waited){
                n->backpressure_total_us += i_ramble_plat_now_us() - t0;
                n->backpressure_wait_count++;
            }
            /* the poller may have relocated the arena while we slept */
            q = ramble_transport_topic_qos(n->transport, topic_index);
            matched = ramble_transport_publisher_match_count(n->transport, topic_index);
        }
    } else
#endif
    /* no service thread: the bounded backpressure pump runs the loop until a slow reader
       acks or the wait elapses, then sends anyway */
    if (matched && may_wait && q && q->backpressure_wait_us && ramble_transport_send_would_evict(n->transport, topic_index)){
        i_RamblePumpWait c = { 0 }; i_RambleWait w = { 0 };
        c.index = topic_index;
        c.t0 = i_ramble_plat_now_us();
        c.deadline = c.t0 + q->backpressure_wait_us;
        c.sample_last = c.t0;
        c.interval = n->pump_probe_interval_us ? n->pump_probe_interval_us : 200000u;
        if (n->pump_probe) ramble_transport_repair_stats(n->transport, topic_index, &c.prev);
        w.done = i_ramble_node_pump_wait_done; w.periodic = i_ramble_node_pump_wait_tick;
        w.ctx = &c;
        w.pump_ms = 1;   /* a nested tick: the lock stays held */
        guarded = 1;
        i_ramble_node_wait_until(n, &w);
        n->backpressure_total_us += i_ramble_plat_now_us() - c.t0;
        n->backpressure_wait_count++;
        /* a mid pump grow relocates the arena: re derive the cached pointers */
        q = ramble_transport_topic_qos(n->transport, topic_index);
        matched = ramble_transport_publisher_match_count(n->transport, topic_index);
    }

    /* a send that evicts never sent history is surfaced. The state is read before the
       commit, but the event fires only if the send commits: a rejected send overwrote nothing */
    {
        uint64_t evict_base = 0; uint32_t evict_count = 0;
        int will_evict = guarded &&
            ramble_transport_send_would_evict_unsent(n->transport, topic_index, &evict_base, &evict_count);
        int r;
#ifdef RAMBLE_SHM
        /* only a message that would fragment gains from SHM, below the fragment size
           inline UDP is strictly cheaper. See spec/transport.md */
        if (!directed && n->shm_capable && len > ramble_transport_frag(n->transport)
            && topic_index < n->shm_n_topics && matched
            && ramble_transport_publisher_shm_eligible(n->transport, topic_index)){
            uint16_t keep_last = (q && q->keep_last) ? q->keep_last : 1u;
            /* a hint pins the topic to one class, else each message uses its own size
               class's segment. Per topic either way */
            uint32_t hint = q ? (q->shm_max_bytes ? q->shm_max_bytes : q->max_message_bytes) : 0u;
            uint32_t k = i_ramble_shm_class_for(hint ? hint : (uint32_t)len);
            /* fits its class: publish via SHM. The chunk index is the history slot this
               send occupies, so chunk i binds slot i */
            if (k < RAMBLE_SHM_N_CLASSES && (uint32_t)len <= i_ramble_shm_class_bytes(k)){
                i_RambleShmPool *pool = i_ramble_node_shm_topic_pool(n, topic_index, k, keep_last);
                uint16_t slot = ramble_transport_topic_hist_head(n->transport, topic_index);
                void *chunk_ptr = pool ? i_ramble_shm_chunk(pool, slot, NULL) : NULL;
                if (chunk_ptr){
                    i_RambleShmDesc d; uint8_t desc[RAMBLE_SHM_DESC_WIRE];
                    /* gather the whole wire sample into the chunk: stamp, header, payload,
                       exactly as the inline commit writes it */
                    if (ts_bytes){
                        uint64_t w = i_ramble_plat_wall_us() & RAMBLE_STAMP_MASK;
                        if (cap_bytes) w |= RAMBLE_STAMP_CAPTURE;
                        i_ramble_le_w64((uint8_t*)chunk_ptr, w);
                        if (cap_bytes) i_ramble_le_w64((uint8_t*)chunk_ptr + ts_bytes, capture_us);
                    }
                    if (hdr.len) memcpy((uint8_t*)chunk_ptr + ts_bytes + cap_bytes, hdr.data, hdr.len);
                    memcpy((uint8_t*)chunk_ptr + ts_bytes + cap_bytes + hdr.len, data.data, data.len);
                    i_ramble_shm_stamp(pool, slot, (uint32_t)len, &d);
                    i_ramble_shm_desc_encode(&d, desc);
                    if (ramble_transport_send_shm(n->transport, topic_index, ramble_bytes(chunk_ptr, len), desc, i_ramble_plat_now_us())==0){
                        n->shm_tx++;
                        r = RAMBLE_OK;
                        goto committed;
                    }
                }
            }
        }
#endif
        r = directed ? ramble_transport_send_to(n->transport, topic_index, to_peer, hdr, data,
                                              capture_us, i_ramble_plat_now_us())
                     : ramble_transport_send_hdr(n->transport, topic_index, hdr, data,
                                               capture_us, i_ramble_plat_now_us());
#ifdef RAMBLE_SHM
committed:
#endif
        if (r == RAMBLE_OK && topic_index < i_ramble_node_topic_hi(n) && n->handles[topic_index]){
            n->handles[topic_index]->tx_msgs++;                      /* ramble_topic_counts */
            n->handles[topic_index]->tx_bytes += len;
        }
        if (r == RAMBLE_OK && will_evict){
            RambleEvent e; memset(&e, 0, sizeof e);
            n->evicted_unsent++;
            e.kind = RAMBLE_ERROR; e.error = RAMBLE_E_EVICTED_UNSENT;
            e.topic = topic_index; e.topic_name = i_ramble_node_topic_name(n, topic_index);
            e.lost_first = evict_base; e.lost_count = evict_count;
            i_ramble_node_emit(n, &e);
        }
        return r;
    }
}

/* the plain broadcast send, the hot ramble_topic_send path */
static int i_ramble_node_do_send(RambleNode *n, uint16_t topic_index, RambleBytes data,
                               uint64_t capture_us, int may_wait){
    RambleBytes nohdr; nohdr.data=NULL; nohdr.len=0;
    return i_ramble_node_do_send_ex(n, topic_index, nohdr, data, capture_us, 0, 0, may_wait);
}

int ramble_topic_send(RambleTopic *topic, RambleBytes data, const RambleSendOpts *opts){
    int acquired, r;
    if (!topic) return RAMBLE_ERR_NO_TOPIC;
    acquired = i_ramble_node_lock(topic->n);
    r = i_ramble_node_do_send(topic->n, topic->index, data,
                            opts ? opts->capture_us : 0u, acquired);
    i_ramble_node_kick_tx(topic->n);           /* flush the commit now, not at the next tick */
    i_ramble_node_unlock(topic->n, acquired);
    return r;
}

int i_ramble_topic_send_hdr(RambleTopic *topic, RambleBytes hdr, RambleBytes data){
    int acquired, r;
    if (!topic) return RAMBLE_ERR_NO_TOPIC;
    acquired = i_ramble_node_lock(topic->n);
    r = i_ramble_node_do_send_ex(topic->n, topic->index, hdr, data, 0, 0, 0, acquired);
    i_ramble_node_kick_tx(topic->n);
    i_ramble_node_unlock(topic->n, acquired);
    return r;
}

uint64_t i_ramble_topic_seqno(RambleTopic *topic){
    return topic ? ramble_transport_topic_seqno(topic->n->transport, topic->index) : 0;
}

void i_ramble_node_flush_tx(RambleNode *n){
    uint8_t buf[RAMBLE_DGRAM_MAX]; uint32_t to; size_t out_len;
    int acquired;
    if (!n || n->fd == RAMBLE_SOCK_BAD) return;
    acquired = i_ramble_node_lock(n);
    if (n->tx_hold_len && i_ramble_node_tx(n, n->tx_hold_peer, n->tx_hold, n->tx_hold_len))
        n->tx_hold_len = 0;
    if (!n->tx_hold_len)
        while (ramble_transport_poll_send(n->transport, &to, buf, sizeof buf, &out_len,
                                        i_ramble_plat_now_us()))
            if (!i_ramble_node_tx(n, to, buf, out_len)) break;   /* TX full: best effort */
    i_ramble_node_unlock(n, acquired);
}

void i_ramble_topic_clear_sys(RambleTopic *topic){
    if (!topic) return;
    topic->sys_on_message = NULL;
    topic->sys_msg_user = NULL;
}

int i_ramble_topic_match_wait(RambleTopic *topic){
    RambleNode *n; int acquired, matched;
    if (!topic) return 0;
    n = topic->n;
    acquired = i_ramble_node_lock(n);
    matched = ramble_transport_publisher_match_count(n->transport, topic->index);
    /* wait only when it can help and can run: a match still forming, the knob on, a
       publishing role, and not from a callback. Converged matching returns at once */
    if (acquired && !matched && n->match_wait_us
        && ramble_role_pubs(topic->role)
        && i_ramble_node_topic_unsettled(n, topic, i_ramble_plat_now_us()))
        matched = i_ramble_node_match_wait(n, topic->index, topic, 0);
    i_ramble_node_unlock(n, acquired);
    return matched;
}

int i_ramble_topic_send_to(RambleTopic *topic, uint32_t to_peer, RambleBytes hdr, RambleBytes data){
    int acquired, r;
    if (!topic) return RAMBLE_ERR_NO_TOPIC;
    acquired = i_ramble_node_lock(topic->n);
    r = i_ramble_node_do_send_ex(topic->n, topic->index, hdr, data, 0, 1, to_peer, acquired);
    i_ramble_node_kick_tx(topic->n);
    i_ramble_node_unlock(topic->n, acquired);
    return r;
}

/* The patterns layer's seams. Not lock guarded, the layer calls them under the node lock. */
void  *i_ramble_node_sys_alloc(RambleNode *n, void *ptr, size_t size){ return i_ramble_node_alloc(n, ptr, size); }
void **i_ramble_node_sys_slot (RambleNode *n){ return &n->patterns; }
uint64_t i_ramble_node_now_us (RambleNode *n){ (void)n; return i_ramble_plat_now_us(); }
uint64_t i_ramble_node_wall_us(RambleNode *n){ (void)n; return i_ramble_plat_wall_us(); }
/* The node lock for a pattern call, reentrancy aware like the internal entry lock. No kick
 * on unlock: every mutating path kicks at its own layer, and read only ops must not wake. */
int  i_ramble_node_sys_lock  (RambleNode *n){ return i_ramble_node_lock(n); }
void i_ramble_node_sys_unlock(RambleNode *n, int acquired){ i_ramble_node_unlock(n, acquired); }
int  i_ramble_node_sys_poll  (RambleNode *n, int timeout_ms){ return ramble_node_poll(n, timeout_ms); }

/* A topic scoped RAMBLE_ERROR from the patterns layer, through the node's one event path. */
void i_ramble_node_sys_error(RambleNode *n, RambleErrorKind error, RambleTopic *topic, uint32_t peer){
    RambleEvent e;
    if (!n) return;
    memset(&e, 0, sizeof e);
    e.kind = RAMBLE_ERROR; e.error = error; e.peer = peer;
    if (topic){ e.topic = topic->index; e.topic_name = i_ramble_node_topic_name(n, topic->index); }
    i_ramble_node_emit(n, &e);
}

/* Matched subscribers excluding dormant peers, since ramble_topic_match_count keeps counting
 * a dropped but resumable peer. */
int i_ramble_topic_live_match_count(RambleTopic *topic){
    int acquired, r;
    if (!topic) return 0;
    acquired = i_ramble_node_lock(topic->n);
    r = ramble_transport_publisher_live_matches(topic->n->transport, topic->index);
    i_ramble_node_unlock(topic->n, acquired);
    return r;
}
int i_ramble_topic_peer_matched(RambleTopic *topic, uint32_t peer){
    int acquired, r;
    if (!topic) return 0;
    acquired = i_ramble_node_lock(topic->n);
    r = ramble_transport_publisher_peer_matched(topic->n->transport, topic->index, peer);
    i_ramble_node_unlock(topic->n, acquired);
    return r;
}

/* Matched publishers feeding this topic's subscription side. */
int i_ramble_topic_source_match_count(RambleTopic *topic){
    int acquired, r;
    if (!topic) return 0;
    acquired = i_ramble_node_lock(topic->n);
    r = ramble_transport_subscriber_match_count(topic->n->transport, topic->index);
    i_ramble_node_unlock(topic->n, acquired);
    return r;
}

/* The oldest live matched subscriber's peer id, 0 = none. */
uint32_t i_ramble_topic_oldest_match(RambleTopic *topic){
    uint32_t r; int acquired;
    if (!topic) return 0;
    acquired = i_ramble_node_lock(topic->n);
    r = ramble_transport_publisher_oldest_match(topic->n->transport, topic->index);
    i_ramble_node_unlock(topic->n, acquired);
    return r;
}

const uint8_t *i_ramble_node_uuid(RambleNode *n){
    return n ? ramble_discovery_uuid(ramble_discovery_state(n->discovery)) : NULL;
}

/* Reflection getters for the patterns layer's entity enumeration. */
uint8_t    i_ramble_topic_kind (const RambleTopic *topic){ return topic ? topic->kind : 0; }
uint8_t    i_ramble_topic_role (const RambleTopic *topic){ return topic ? topic->role : (uint8_t)RAMBLE_INACTIVE; }
uint8_t    i_ramble_topic_reliability(const RambleTopic *topic){ return topic ? (uint8_t)topic->qos.reliability : 0; }
const uint8_t *i_ramble_node_peer_uuid(RambleNode *n, uint32_t peer){
    const RambleDiscoveryState *st; uint16_t q, np; int acquired; const uint8_t *out = NULL;
    RambleDiscoveryPeer v;   /* the uuid is a view into discovery state, the copy only carries it */
    if (!n) return NULL;
    acquired = i_ramble_node_lock(n);
    st = ramble_discovery_state(n->discovery);
    np = ramble_discovery_max_peers(st);
    for (q = 0; q < np; q++)
        if (ramble_discovery_peer_at(st, q, &v) && v.id == peer){ out = v.uuid; break; }
    i_ramble_node_unlock(n, acquired);
    return out;
}
RambleString i_ramble_topic_name (const RambleTopic *topic){
    return topic ? ramble_string(topic->name, topic->name_len) : ramble_string(NULL, 0);
}
uint16_t   i_ramble_node_topic_count(RambleNode *n){ return n ? i_ramble_node_topic_hi(n) : 0; }

void i_ramble_node_set_sys_hooks(RambleNode *n, i_RambleSysEventFn on_event, i_RambleSysTickFn tick,
                               i_RambleSysCloseFn on_close, void *user){
    int acquired;
    if (!n) return;
    acquired = i_ramble_node_lock(n);
    n->sys_on_event = on_event; n->sys_tick = tick; n->sys_on_close = on_close; n->sys_user = user;
    n->sys_tick_next = 0;
    i_ramble_node_kick(n);   /* re evaluate the wait cap with the new tick */
    i_ramble_node_unlock(n, acquired);
}

int ramble_topic_set_role(RambleTopic *topic, RambleRole role){
    int r, acquired;
    if (!topic) return -1;
    acquired = i_ramble_node_lock(topic->n);
    if (!acquired){
        /* from a callback: the replay would rematch the reader proxy mid delivery, refuse loudly */
        return RAMBLE_ERR_STATE;
    }
    /* a log builtin is queued before its subscribe side goes live, so the catch up replay
       can never race the first take into the inline path. See spec/node.md */
    if (ramble_role_subs((uint8_t)role) && !topic->q
        && i_ramble_node_is_log_topic(topic->n, topic->index))
        (void)i_ramble_node_queue_ensure(topic->n, topic, NULL);
    r = ramble_transport_set_role(topic->n->transport, topic->index, (uint8_t)role);
    if (r == 0){
        topic->role = (uint8_t)role;
        i_ramble_node_readvertise(topic->n);   /* a role flip may raise fresh candidates */
    }
    i_ramble_node_unlock(topic->n, acquired);
    return r;
}

int ramble_topic_retire(RambleTopic *topic){
    RambleNode *n; int acquired; uint16_t idx;
    if (!topic) return RAMBLE_ERR_NO_TOPIC;
    n = topic->n;
    acquired = i_ramble_node_lock(n);
    if (!acquired) return RAMBLE_ERR_STATE;   /* from a callback: lanes are live mid delivery */
    if (topic->sys_on_message                       /* a pattern channel: retire its handle */
        || i_ramble_node_is_log_topic(n, topic->index)
        || (n->n_builtin && topic->index >= n->builtin_lo
            && topic->index < (uint16_t)(n->builtin_lo + n->n_builtin))
        || (topic->q && topic->q->busy)){           /* mid dispatch on another thread */
        i_ramble_node_unlock(n, acquired);
        return RAMBLE_ERR_STATE;
    }
    idx = topic->index;
    if (ramble_transport_topic_retire(n->transport, idx) != 0){
        i_ramble_node_unlock(n, acquired);
        return RAMBLE_ERR_STATE;
    }
    /* the slot keeps its schema hash as the reuse fingerprint, the parsed copy goes */
    i_ramble_node_core_retire_topic_schema(n->core, idx);
    if (topic->q){
        if (topic->q->buf) i_ramble_node_alloc(n, topic->q->buf, 0);
        i_ramble_node_alloc(n, topic->q, 0);
        topic->q = NULL;
    }
    if (topic->schema) ramble_schema_free(topic->schema, i_ramble_node_alloc, n);
    n->handles[idx] = NULL;
    i_ramble_node_alloc(n, topic, 0);         /* the handle is invalid from here */
    /* the slot rides the announce as a hole from the next blob on */
    i_ramble_node_readvertise(n);
    i_ramble_node_unlock(n, acquired);
    return RAMBLE_OK;
}

uint16_t ramble_topic_index(const RambleTopic *topic){ return topic ? topic->index : 0; }

const RambleSchema *ramble_topic_schema(const RambleTopic *topic){ return topic ? topic->schema : NULL; }

RambleTopic *ramble_node_topic(RambleNode *n, uint16_t index){
    RambleTopic *h; int acquired;
    if (!n) return NULL;
    acquired = i_ramble_node_lock(n);
    h = (index < i_ramble_node_topic_hi(n)) ? n->handles[index] : NULL;   /* holes yield NULL */
    i_ramble_node_unlock(n, acquired);
    return h;
}

/* reflection: the walks read the core's tables under the node lock */

int ramble_node_peers_next(RambleNode *n, RambleIter *it, RamblePeerInfo *out){
    int r, acquired;
    if (!n) return 0;
    acquired = i_ramble_node_lock(n);
    r = i_ramble_node_core_peers_next(n->core, it, out);
    if (r){   /* the transport's path measurement, folded in since the core is sans transport */
        RamblePeerRtt e;
        if (ramble_transport_peer_rtt(n->transport, out->id, &e)){
            out->rtt_us = e.rtt_us; out->rtt_jitter_us = e.rtt_jitter_us;
            out->rtt_min_us = e.rtt_min_us; out->rtt_samples = e.samples;
        }
    }
    i_ramble_node_unlock(n, acquired);
    return r;
}

int ramble_node_entities_next(RambleNode *n, uint32_t peer, RambleIter *it, RambleEntityInfo *out){
    int r, acquired;
    if (!n) return 0;
    acquired = i_ramble_node_lock(n);
    r = i_ramble_node_core_entities_next(n->core, peer, it, out);
    i_ramble_node_unlock(n, acquired);
    return r;
}

int ramble_node_mesh_next(RambleNode *n, RambleIter *it, RambleEntityInfo *out){
    int r, acquired;
    if (!n) return 0;
    acquired = i_ramble_node_lock(n);
    r = i_ramble_node_core_mesh_next(n->core, it, out);
    i_ramble_node_unlock(n, acquired);
    return r;
}

int ramble_node_mesh_find(RambleNode *n, RambleEntityKind kind, const char *name, RambleEntityInfo *out){
    int r, acquired;
    if (!n) return 0;
    acquired = i_ramble_node_lock(n);
    r = i_ramble_node_core_mesh_find(n->core, kind, name, out);
    i_ramble_node_unlock(n, acquired);
    return r;
}

uint32_t ramble_node_mesh_epoch(RambleNode *n){
    uint32_t e; int acquired;
    if (!n) return 0;
    acquired = i_ramble_node_lock(n);
    e = i_ramble_node_core_mesh_epoch(n->core);
    i_ramble_node_unlock(n, acquired);
    return e;
}

/* Re types a live topic in place: the slot is retired and reused at the same index under
 * a bumped generation, the schema copy replaced, the handle and queue kept. Lock held. */
int i_ramble_topic_retype(RambleTopic *topic, const RambleSchema *schema, uint8_t reliability){
    RambleNode *n = topic->n; RambleTopicDef def; RambleSchema *copy = NULL; uint16_t idx = topic->index;
    uint32_t rb; int rc;
    if (schema){
        RambleBytes w = ramble_schema_wire(schema);
        copy = ramble_schema_parse(w.data, w.len, i_ramble_node_alloc, n);
        if (!copy) return RAMBLE_ERR_OOM;
    }
    if (ramble_transport_topic_retire(n->transport, idx) != 0){
        if (copy) ramble_schema_free(copy, i_ramble_node_alloc, n);
        return RAMBLE_ERR_STATE;
    }
    i_ramble_node_core_retire_topic_schema(n->core, idx);
    if (topic->schema) ramble_schema_free(topic->schema, i_ramble_node_alloc, n);
    topic->schema = copy;
    topic->qos.reliability = (RambleReliability)reliability;
    memset(&def, 0, sizeof def);
    def.name = topic->name; def.role = topic->role; def.kind = topic->kind;
    def.prefix_bytes = topic->prefix_bytes; def.directed = topic->directed; def.attrs = topic->attrs;
    def.qos = topic->qos;
    rb = ramble_discovery_meta_version(ramble_discovery_state(n->discovery)) + 1u;
    rc = ramble_transport_topic_reuse(n->transport, idx, &def, 1, rb);
    if (rc != 0) return rc == -4 ? RAMBLE_ERR_OOM : RAMBLE_ERR_STATE;
    i_ramble_node_core_topic_rebound(n->core, idx);
    i_ramble_node_core_set_topic_schema(n->core, idx, topic->schema);
    i_ramble_node_readvertise(n);
    return RAMBLE_OK;
}

int ramble_topic_refresh(RambleTopic *topic){
    RambleNode *n; int acquired, r = 0;
    const RambleSchema *ms = NULL; uint8_t rel = 0; uint64_t gen = 0;
    if (!topic) return RAMBLE_ERR_NO_TOPIC;
    if (!topic->reflect) return RAMBLE_ERR_ROLE;
    n = topic->n;
    acquired = i_ramble_node_lock(n);
    if (!acquired) return RAMBLE_ERR_STATE;   /* from a callback: lanes are live mid delivery */
    if (i_ramble_node_core_reflect_pick(n->core, RAMBLE_ENTITY_TOPIC, topic->name, 0,
                                      ramble_role_pubs(topic->role), &ms, &rel, &gen)
        && gen != topic->generation){
        r = i_ramble_topic_retype(topic, ms, rel ? RAMBLE_RELIABLE : RAMBLE_BEST_EFFORT);
        if (r == 0){ topic->generation = gen; r = 1; }
    }
    i_ramble_node_unlock(n, acquired);
    return r;
}

/* the patterns layer's reflect_from_mesh: the pick for one channel of an entity */
int i_ramble_node_reflect_pick(RambleNode *n, RambleEntityKind kind, const char *name, int which, int writer,
                             const RambleSchema **schema, uint8_t *reliable, uint64_t *generation){
    int r, acquired;
    if (!n) return 0;
    acquired = i_ramble_node_lock(n);
    r = i_ramble_node_core_reflect_pick(n->core, kind, name, which, writer, schema, reliable, generation);
    i_ramble_node_unlock(n, acquired);
    return r;
}

void ramble_node_backpressure_stats(RambleNode *n, uint64_t *waited_us, uint32_t *waited_sends){
    int acquired = i_ramble_node_lock(n);
    if (waited_us)    *waited_us    = n->backpressure_total_us;
    if (waited_sends) *waited_sends = n->backpressure_wait_count;
    i_ramble_node_unlock(n, acquired);
}

/* Sends that evicted never sent history after the bounded wait, since open. */
uint32_t ramble_node_evicted_unsent(RambleNode *n){
    uint32_t v; int acquired;
    if (!n) return 0;
    acquired = i_ramble_node_lock(n);
    v = n->evicted_unsent;
    i_ramble_node_unlock(n, acquired);
    return v;
}

#ifndef RAMBLE_NO_STDTYPES
/* The standard types that need a platform. They sit here so serialize/ keeps needing
 * nothing but memory. */
RambleTimestamp ramble_timestamp_now(void){
    return (RambleTimestamp)i_ramble_plat_wall_us();
}

void ramble_uuid_new(RambleUuid *out){
    /* one generator: the CSPRNG path, else the host identity mix a node's own uuid uses */
    if (out) i_ramble_discovery_auto_uuid(out->bytes);
}
#endif

void ramble_node_mem_stats(RambleNode *n, size_t *in_use, size_t *peak, uint64_t *alloc_calls){
    int acquired;
    if (!n) return;
    acquired = i_ramble_node_lock(n);
    ramble_allocator_stats(&n->pool, in_use, peak, alloc_calls);
    i_ramble_node_unlock(n, acquired);
}

void ramble_topic_repair_stats(RambleTopic *topic, RambleRepairStats *out){
    if (topic){
        int acquired = i_ramble_node_lock(topic->n);
        ramble_transport_repair_stats(topic->n->transport, topic->index, out);
        i_ramble_node_unlock(topic->n, acquired);
    } else if (out) memset(out, 0, sizeof *out);
}

void ramble_topic_counts(RambleTopic *topic, uint64_t *tx_msgs, uint64_t *tx_bytes,
                       uint64_t *rx_msgs, uint64_t *rx_bytes){
    uint64_t tm = 0, tb = 0, rm = 0, rb = 0;
    if (topic){
        int acquired = i_ramble_node_lock(topic->n);
        tm = topic->tx_msgs; tb = topic->tx_bytes; rm = topic->rx_msgs; rb = topic->rx_bytes;
        i_ramble_node_unlock(topic->n, acquired);
    }
    if (tx_msgs)  *tx_msgs  = tm;
    if (tx_bytes) *tx_bytes = tb;
    if (rx_msgs)  *rx_msgs  = rm;
    if (rx_bytes) *rx_bytes = rb;
}

/* the @ramble/meta snapshot builder, see spec/node.md */

/* one pass of the map body. A latched writer error makes finish return 0 and the caller
 * grows and rebuilds */
static void i_ramble_node_snapshot_fill(RambleNode *n, RambleMapWriter *w, uint32_t sections){
    uint64_t now = i_ramble_plat_now_us();
    if (sections & RAMBLE_META_NODE){
        size_t in_use = 0, peak = 0; uint64_t allocs = 0;
        ramble_allocator_stats(&n->pool, &in_use, &peak, &allocs);
        ramble_map_open_map(w, "node");
        ramble_map_put_string(w, "name", ramble_string(n->name, n->name_len));
        ramble_map_put_uint(w, "uptime_us", now - n->open_us);
        ramble_map_put_uint(w, "mono_us", now);
        ramble_map_put_uint(w, "wall_us", i_ramble_plat_wall_us());
        ramble_map_put_uint(w, "mem_in_use", (uint64_t)in_use);
        ramble_map_put_uint(w, "mem_peak", (uint64_t)peak);
        ramble_map_put_uint(w, "alloc_calls", allocs);
        ramble_map_put_uint(w, "evicted_unsent", n->evicted_unsent);
        ramble_map_put_uint(w, "bp_waited_us", n->backpressure_total_us);
        ramble_map_put_uint(w, "bp_waits", n->backpressure_wait_count);
        ramble_map_put_uint(w, "peers", ramble_discovery_peer_count(ramble_discovery_state(n->discovery)));
        ramble_map_put_uint(w, "max_peers", n->max_peers);
        /* app topics only: the @ramble/ builtins are hidden, so neither the counts nor the
           topics array below surface them */
        ramble_map_put_uint(w, "topics", (uint64_t)n->n_created);
        ramble_map_put_uint(w, "max_topics", (uint64_t)(n->max_topics - n->n_builtin));
#ifdef RAMBLE_SHM
        ramble_map_put_uint(w, "shm_tx", n->shm_tx);
        ramble_map_put_uint(w, "shm_rx", n->shm_rx);
#endif
        ramble_map_put_uint(w, "last_error", (uint64_t)n->last_error.error);
        if (n->last_error.error != RAMBLE_E_NONE){
            /* format from a sanitized copy: the stored event's name views can outlive what
               they pointed at, so the text carries the indices instead */
            RambleEvent le = n->last_error; char txt[160];
            le.topic_name = NULL; le.peer_name = NULL; le.schema_detail = NULL;
            ramble_event_str(&le, txt, sizeof txt);
            ramble_map_put_string(w, "last_error_text", ramble_cstr(txt));
        }
        ramble_map_close(w);
    }
#ifdef RAMBLE_PROC_STATS
    if (sections & RAMBLE_META_PROC){
        uint64_t cpu = 0, rss = 0, peak_rss = 0; int have_cpu = 0;
        if (i_ramble_plat_proc_stats(&cpu, &rss, &peak_rss, &have_cpu)){   /* absent if unsupported */
            ramble_map_open_map(w, "proc");
            ramble_map_put_uint(w, "pid", i_ramble_plat_pid());
            if (have_cpu) ramble_map_put_uint(w, "cpu_us", cpu);
            ramble_map_put_uint(w, "rss", rss);
            ramble_map_put_uint(w, "peak_rss", peak_rss);
            {   uint64_t heap_total, heap_free, heap_min_free, heap_largest_free_block;
                if (i_ramble_plat_heap_stats(&heap_total, &heap_free, &heap_min_free,
                                           &heap_largest_free_block)){
                    ramble_map_put_uint(w, "heap_total", heap_total);
                    ramble_map_put_uint(w, "heap_free", heap_free);
                    ramble_map_put_uint(w, "heap_min_free", heap_min_free);
                    ramble_map_put_uint(w, "heap_largest_free_block", heap_largest_free_block);
                }
            }
            ramble_map_close(w);
        }
    }
#endif
    if (sections & RAMBLE_META_TOPICS){
        uint16_t i, hi = i_ramble_node_topic_hi(n);
        const uint16_t *pend = NULL;
        if (hi){
            /* every topic's unresolved count in one walk of each peer's interest, since the
               per topic query is quadratic for an observer. On OOM the per topic query stands in */
            if (n->snap_pend_cap < hi){
                uint16_t *nb = (uint16_t*)i_ramble_node_alloc(n, n->snap_pend, (size_t)hi * sizeof *nb);
                if (nb){ n->snap_pend = nb; n->snap_pend_cap = hi; }
            }
            if (n->snap_pend_cap >= hi){
                i_ramble_node_core_topics_unresolved(n->core, n->snap_pend, hi);
                pend = n->snap_pend;
            }
        }
        ramble_map_open_array(w, "topics");
        for (i = 0; i < hi; i++){
            RambleTopic *h = n->handles[i];
            const RambleQos *q = ramble_transport_topic_qos(n->transport, i);
            RambleRepairStats rs;
            if (!h) continue;
            if (n->n_builtin && i >= n->builtin_lo
                             && i < (uint16_t)(n->builtin_lo + n->n_builtin)) continue;
            ramble_transport_repair_stats(n->transport, i, &rs);
            ramble_map_open_map(w, NULL);
            ramble_map_put_uint(w, "index", i);
            ramble_map_put_string(w, "name", ramble_string(h->name, h->name_len));
            ramble_map_put_uint(w, "kind", h->kind);
            ramble_map_put_uint(w, "role", h->role);
            ramble_map_put_bool(w, "reliable", q && q->reliability == RAMBLE_RELIABLE);
            ramble_map_put_uint(w, "keep_last", q ? q->keep_last : 0);
            ramble_map_put_uint(w, "catch_up", q ? q->catch_up : 0);
            ramble_map_put_uint(w, "subs", (uint64_t)ramble_transport_publisher_match_count(n->transport, i));
            ramble_map_put_uint(w, "pubs", (uint64_t)ramble_transport_subscriber_match_count(n->transport, i));
            ramble_map_put_uint(w, "pending", pend ? (uint64_t)pend[i]
                                                 : (uint64_t)i_ramble_node_core_topic_unresolved(n->core, i));
            ramble_map_put_uint(w, "tx_msgs", h->tx_msgs);
            ramble_map_put_uint(w, "tx_bytes", h->tx_bytes);
            ramble_map_put_uint(w, "rx_msgs", h->rx_msgs);
            ramble_map_put_uint(w, "rx_bytes", h->rx_bytes);
            ramble_map_put_uint(w, "nacks_recv", rs.nacks_recv);
            ramble_map_put_uint(w, "frags_resent", rs.frags_resent);
            ramble_map_put_uint(w, "frags_sent", rs.frags_sent);
            ramble_map_put_uint(w, "nacks_sent", rs.nacks_sent);
            ramble_map_put_uint(w, "frags_recv", rs.frags_recv);
            ramble_map_put_uint(w, "frags_dup", rs.frags_dup);
            ramble_map_put_uint(w, "msgs_skipped", rs.msgs_skipped);
            if (h->q){
                ramble_map_put_uint(w, "q_msgs", h->q->count);
                ramble_map_put_uint(w, "q_bytes", h->q->bytes);
                ramble_map_put_uint(w, "q_cap", h->q->cap);
                ramble_map_put_uint(w, "q_dropped", h->q->dropped);
            }
            ramble_map_close(w);
        }
        ramble_map_close(w);
    }
    if (sections & RAMBLE_META_PEERS){
        uint16_t cnt = 0, i;
        const RambleDiscoveryPeer *ps = ramble_discovery_peers(n->discovery, &cnt);
        ramble_map_open_array(w, "peers");
        for (i = 0; i < cnt; i++){
            uint16_t pub_to = 0, recv_from = 0;
            char ip[16]; int ln = 0;
            ramble_transport_peer_match_counts(n->transport, ps[i].id, &pub_to, &recv_from);
            ramble_map_open_map(w, NULL);
            ramble_map_put_uint(w, "id", ps[i].id);
            ramble_map_put_string(w, "name", ps[i].name);
            ramble_map_put_bool(w, "active", ps[i].liveness == RAMBLE_PEER_ACTIVE);
            if (ps[i].addr.ip_len == 4)
                ln = snprintf(ip, sizeof ip, "%u.%u.%u.%u", ps[i].addr.ip[0], ps[i].addr.ip[1],
                              ps[i].addr.ip[2], ps[i].addr.ip[3]);
            ramble_map_put_string(w, "ip", ramble_string(ip, ln > 0 ? (size_t)ln : 0));
            ramble_map_put_uint(w, "port", ps[i].addr.port);
            ramble_map_put_uint(w, "age_us", now > ps[i].last_heard_us ? now - ps[i].last_heard_us : 0);
            ramble_map_put_uint(w, "publish_to", pub_to);
            ramble_map_put_uint(w, "receive_from", recv_from);
            {   RamblePeerRtt e;   /* the measured round trip, absent until the first sample */
                if (ramble_transport_peer_rtt(n->transport, ps[i].id, &e) && e.samples){
                    ramble_map_put_uint(w, "rtt_us", e.rtt_us);
                    ramble_map_put_uint(w, "rtt_jitter_us", e.rtt_jitter_us);
                    ramble_map_put_uint(w, "rtt_min_us", e.rtt_min_us);
                    ramble_map_put_uint(w, "rtt_samples", e.samples);
                } }
            ramble_map_close(w);
        }
        ramble_map_close(w);
    }
}

RambleBytes i_ramble_node_snapshot(RambleNode *n, uint32_t sections){
    uint32_t body;
    if (!n) return ramble_bytes(NULL, 0);
    if (!sections) sections = 0xFFFFFFFFu;
    for (;;){
        RambleMapWriter w;
        if (!n->snap_buf){
            /* a small node's snapshot is 1 to 2 KB: start there, the doubling finds the size */
            n->snap_buf = (uint8_t*)i_ramble_node_alloc(n, NULL, 1024u);
            if (!n->snap_buf) return ramble_bytes(NULL, 0);
            n->snap_cap = 1024u;
        }
        w = ramble_map_begin(n->snap_buf, n->snap_cap);
        i_ramble_node_snapshot_fill(n, &w, sections);
        body = ramble_map_finish(&w);
        if (body) break;
        {   /* did not fit (the writer latches on any failure): double and rebuild */
            uint32_t ncap = n->snap_cap * 2u; uint8_t *nb;
            if (ncap > (1u << 22)) return ramble_bytes(NULL, 0);   /* 4 MB: not a size problem */
            nb = (uint8_t*)i_ramble_node_alloc(n, n->snap_buf, ncap);
            if (!nb) return ramble_bytes(NULL, 0);
            n->snap_buf = nb; n->snap_cap = ncap;
        }
    }
    return ramble_bytes(n->snap_buf, body);
}

void ramble_node_set_pump_probe(RambleNode *n, RamblePumpProbeFn fn, uint64_t interval_us, void *user){
    int acquired = i_ramble_node_lock(n);
    n->pump_probe = fn; n->pump_probe_interval_us = interval_us; n->pump_probe_user = user;
    i_ramble_node_unlock(n, acquired);
}

int ramble_topic_subscriber_progress(RambleTopic *topic, uint32_t peer,
                                 uint64_t *base_seqno, uint32_t *have, uint32_t *total){
    int r, acquired;
    if (!topic) return 0;
    acquired = i_ramble_node_lock(topic->n);
    r = ramble_transport_subscriber_progress(topic->n->transport, topic->index, peer, base_seqno, have, total);
    i_ramble_node_unlock(topic->n, acquired);
    return r;
}

#ifdef RAMBLE_SHM
void ramble_node_shm_stats(RambleNode *n, uint32_t *sent, uint32_t *recv){
    int acquired = i_ramble_node_lock(n);
    if (sent) *sent = n->shm_tx;
    if (recv) *recv = n->shm_rx;
    i_ramble_node_unlock(n, acquired);
}
#endif

/* drain's predicate: the topic's send queue is empty at the transport */
typedef struct { uint16_t index; uint64_t deadline; } i_RambleDrainWait;

static int i_ramble_node_drain_wait_done(RambleNode *n, void *ctx, uint64_t now, uint64_t *deadline){
    i_RambleDrainWait *c = (i_RambleDrainWait*)ctx;
    (void)now;
    *deadline = c->deadline;
    return ramble_transport_send_drained(n->transport, c->index) != 0;
}

int ramble_topic_drain(RambleTopic *topic, int timeout_ms){
    RambleNode *n; i_RambleDrainWait c; i_RambleWait w = { 0 }; int acquired, drained;
    if (!topic) return 0;
    n = topic->n;
    acquired = i_ramble_node_lock(n);
    if (!acquired) return 0;   /* from a callback: can neither pump nor wait */
    c.index = topic->index;
    c.deadline = i_ramble_plat_now_us() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000u;
    w.done = i_ramble_node_drain_wait_done; w.periodic = i_ramble_node_wait_kick; w.ctx = &c;
    w.pump_ms = 1;          /* a nested tick: the lock stays held */
    w.pump_after_svc = 1;   /* a service thread stopping under us finishes with the pump */
    drained = i_ramble_node_wait_until(n, &w) == RAMBLE__WAIT_DONE;
    i_ramble_node_unlock(n, acquired);
    return drained;
}

int ramble_topic_match_count(RambleTopic *topic){
    int r, acquired;
    if (!topic) return 0;
    acquired = i_ramble_node_lock(topic->n);
    r = ramble_transport_publisher_match_count(topic->n->transport, topic->index);
    i_ramble_node_unlock(topic->n, acquired);
    return r;
}

int ramble_topic_pending_count(RambleTopic *topic){
    int r, acquired;
    if (!topic) return 0;
    acquired = i_ramble_node_lock(topic->n);
    r = i_ramble_node_core_topic_unresolved(topic->n->core, topic->index);
    i_ramble_node_unlock(topic->n, acquired);
    return r;
}

/* 1 = a send now would not wait: matched, or converged with nobody to wait for. Shares the
 * send path's predicate, so a GUI polling this then sending sees what the send decides. */
int ramble_topic_ready(RambleTopic *topic){
    int r, acquired;
    if (!topic) return 0;
    acquired = i_ramble_node_lock(topic->n);
    r = ramble_transport_publisher_match_count(topic->n->transport, topic->index) > 0
     || !i_ramble_node_topic_unsettled(topic->n, topic, i_ramble_plat_now_us());
    i_ramble_node_unlock(topic->n, acquired);
    return r;
}

/* Settled: the network answered and went quiet. Every active peer heard since the solicit,
 * the topology quiet for one window, one window passed overall. See spec/node.md. */
static int i_ramble_node_settled(RambleNode *n, uint64_t start, uint64_t now){
    uint64_t quiet = n->announce_us < 300000u ? n->announce_us : 300000u;
    uint16_t i, count = 0;
    const RambleDiscoveryPeer *peers = ramble_discovery_peers(n->discovery, &count);
    int any = 0;
    for (i = 0; i < count; i++){
        if (peers[i].liveness != RAMBLE_PEER_ACTIVE) continue;   /* dropped: not expected to answer */
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
} i_RambleSettleWait;

static int i_ramble_node_settle_wait_done(RambleNode *n, void *ctx, uint64_t now, uint64_t *deadline){
    i_RambleSettleWait *c = (i_RambleSettleWait*)ctx;
    *deadline = c->deadline;
    return i_ramble_node_settled(n, c->start, now);
}

/* re solicit 4 times a second so a lost one retries. The kick makes a service thread send
 * it now, a pump sends it on the tick that follows. */
static void i_ramble_node_settle_wait_solicit(RambleNode *n, void *ctx, uint64_t now){
    i_RambleSettleWait *c = (i_RambleSettleWait*)ctx;
    if (now - c->last_solicit < 250000u) return;
    ramble_discovery_solicit(ramble_discovery_state(n->discovery));
    c->last_solicit = now;
    i_ramble_node_wait_kick(n, ctx, now);
}

int ramble_node_settle(RambleNode *n, int timeout_ms){
    i_RambleSettleWait c; i_RambleWait w = { 0 }; int acquired, settled;
    if (!n) return 0;
    acquired = i_ramble_node_lock(n);
    if (!acquired){ return 0; }   /* from a callback: can neither pump nor wait */
    c.start = i_ramble_plat_now_us();
    c.last_solicit = 0;
    c.deadline = c.start + (timeout_ms >= 0 ? (uint64_t)timeout_ms * 1000u
                                            : (uint64_t)n->announce_us * 3u);
    w.done = i_ramble_node_settle_wait_done; w.periodic = i_ramble_node_settle_wait_solicit;
    w.ctx = &c;
    w.cv_cap_us = 50000u;   /* re check the clock: settling is partly time driven */
    w.pump_ms = 1;          /* a nested tick: sends the solicit, takes in the replies */
    w.pump_after_svc = 1;   /* a service thread stopping under us finishes with the pump */
    settled = i_ramble_node_wait_until(n, &w) == RAMBLE__WAIT_DONE;
    i_ramble_node_unlock(n, acquired);
    return settled;
}

/* the consumer queue API, see spec/node.md */

static int i_ramble_node_any_queued(RambleNode *n){
    uint16_t i, hi = i_ramble_node_topic_hi(n);
    for (i = 0; i < hi; i++)
        if (n->handles[i] && n->handles[i]->q && n->handles[i]->q->count) return 1;
    return 0;
}

/* the queue wait's predicate: data on q when one was given, else on any queued topic. The
 * queue struct is a stable allocation, so the pointer survives the waits. */
typedef struct { i_RambleMsgQueue *q; uint64_t deadline; } i_RambleQueueWait;

static int i_ramble_node_queue_wait_done(RambleNode *n, void *ctx, uint64_t now, uint64_t *deadline){
    i_RambleQueueWait *c = (i_RambleQueueWait*)ctx;
    (void)now;
    *deadline = c->deadline;
    return c->q ? (c->q->count != 0) : i_ramble_node_any_queued(n);
}

/* Waits until data is queued: on the service thread's progress when one runs, else by
 * driving the poll loop itself. Lock held on entry and exit, never from a callback. */
static void i_ramble_node_queue_wait(RambleNode *n, i_RambleMsgQueue *q, int timeout_ms){
    i_RambleQueueWait c; i_RambleWait w = { 0 };
    c.q = q;
    c.deadline = timeout_ms < 0 ? (uint64_t)-1
                                : i_ramble_plat_now_us() + (uint64_t)timeout_ms * 1000u;
    w.done = i_ramble_node_queue_wait_done; w.ctx = &c;
    w.cv_cap_us = 3600000000u;   /* an unbounded wait still re checks hourly */
    w.pump_outer = 1;            /* outer: this wait may drop the lock */
    w.pump_after_svc = 1;        /* a service thread stopping under us pumps from then on */
    i_ramble_node_wait_until(n, &w);
}

/* Dispatches up to max_msgs of the messages queued at entry, callbacks on the calling
 * thread and outside the lock when this thread owns it. A reentrant caller keeps the lock. */
static int i_ramble_topic_dispatch_locked(RambleNode *n, RambleTopic *h, int max_msgs, int acquired){
    i_RambleMsgQueue *q = h->q;
    int done = 0; uint32_t todo;
    if (!q || q->busy) return 0;
    i_ramble_node_queue_release(n, h, q);
    todo = q->count;                    /* a snapshot: later arrivals wait for the next call */
    if (max_msgs > 0 && todo > (uint32_t)max_msgs) todo = (uint32_t)max_msgs;
    while (todo-- && q->count){
        const i_RambleQRec *rec = i_ramble_q_peek(q);
        RambleMsg m;
        i_ramble_node_queue_msg(n, h, rec, &m);
        q->viewing = 1;
        if (n->user_on_message){
            q->busy = 1;
#ifdef RAMBLE_THREADS
            if (acquired){
                i_ramble_node_unlock_raw(n);
                n->user_on_message(&m);
                i_ramble_node_lock_raw(n);
            } else
#endif
            n->user_on_message(&m);
            q->busy = 0;
        }
        i_ramble_node_queue_release(n, h, q);
        done++;
    }
    (void)acquired;
    return done;
}

int ramble_topic_take(RambleTopic *topic, RambleMsg *out, int timeout_ms){
    RambleNode *n; i_RambleMsgQueue *q; int acquired, got = 0;
    if (!topic || !out) return RAMBLE_ERR_NO_TOPIC;
    n = topic->n;
    acquired = i_ramble_node_lock(n);
    q = i_ramble_node_queue_ensure(n, topic, NULL);
    if (!q){ i_ramble_node_unlock(n, acquired); return RAMBLE_ERR_OOM; }
    if (q->busy){ i_ramble_node_unlock(n, acquired); return RAMBLE_ERR_STATE; }
    i_ramble_node_queue_release(n, topic, q);      /* finish the previous view first */
    if (!q->count && timeout_ms != 0 && acquired)
        i_ramble_node_queue_wait(n, q, timeout_ms);
    {   const i_RambleQRec *rec = i_ramble_q_peek(q);
        if (rec){
            i_ramble_node_queue_msg(n, topic, rec, out);
            q->viewing = 1;                   /* the view lives until the next take or dispatch */
            got = 1;
        }
    }
    i_ramble_node_unlock(n, acquired);
    return got;
}

int ramble_topic_dispatch(RambleTopic *topic, int max_msgs, int timeout_ms){
    RambleNode *n; i_RambleMsgQueue *q; int acquired, done;
    if (!topic) return RAMBLE_ERR_NO_TOPIC;
    n = topic->n;
    acquired = i_ramble_node_lock(n);
    q = i_ramble_node_queue_ensure(n, topic, NULL);
    if (!q){ i_ramble_node_unlock(n, acquired); return RAMBLE_ERR_OOM; }
    if (q->busy){ i_ramble_node_unlock(n, acquired); return RAMBLE_ERR_STATE; }
    i_ramble_node_queue_release(n, topic, q);
    if (!q->count && timeout_ms != 0 && acquired)
        i_ramble_node_queue_wait(n, q, timeout_ms);
    done = i_ramble_topic_dispatch_locked(n, topic, max_msgs, acquired);
    i_ramble_node_unlock(n, acquired);
    return done;
}

int ramble_node_dispatch(RambleNode *n, int max_msgs, int timeout_ms){
    int acquired, total = 0;
    uint16_t i;
    if (!n) return RAMBLE_ERR_STATE;
    acquired = i_ramble_node_lock(n);
    if (!i_ramble_node_any_queued(n) && timeout_ms != 0 && acquired)
        i_ramble_node_queue_wait(n, NULL, timeout_ms);
    for (i = 0; i < i_ramble_node_topic_hi(n); i++){
        RambleTopic *h = n->handles[i];
        if (!h || !h->q || !h->q->count) continue;
        total += i_ramble_topic_dispatch_locked(n, h, max_msgs > 0 ? max_msgs - total : 0, acquired);
        if (max_msgs > 0 && total >= max_msgs) break;
    }
    i_ramble_node_unlock(n, acquired);
    return total;
}

void ramble_topic_queue_stats(RambleTopic *topic, uint32_t *msgs, uint32_t *bytes,
                              uint32_t *capacity, uint32_t *dropped){
    uint32_t m = 0, b = 0, c = 0, d = 0;
    if (topic && topic->q){
        int acquired = i_ramble_node_lock(topic->n);
        m = topic->q->count; b = topic->q->bytes; c = topic->q->cap; d = topic->q->dropped;
        i_ramble_node_unlock(topic->n, acquired);
    }
    if (msgs)     *msgs = m;
    if (bytes)    *bytes = b;
    if (capacity) *capacity = c;
    if (dropped)  *dropped = d;
}

#ifdef RAMBLE_THREADS
/* The service thread: the poll body in a loop, holding the lock for every work pass and
 * dropping it inside the wait. The 250 ms cap bounds a lost wakeup. See spec/node.md. */
static void i_ramble_node_service(void *arg){
    RambleNode *n = (RambleNode *)arg;
    i_ramble_node_lock_raw(n);
    while (!n->svc_stop)
        i_ramble_node_poll_locked(n, 250, 1);
    i_ramble_node_unlock_raw(n);
}
#endif

int ramble_node_start(RambleNode *n){
#ifndef RAMBLE_THREADS
    (void)n;
    return RAMBLE_ERR_NOSYS;
#else
    int acquired;
    if (!n) return RAMBLE_ERR_STATE;
    acquired = i_ramble_node_lock(n);
    if (!acquired) return RAMBLE_ERR_STATE;               /* from a callback */
    if (n->svc_running){ i_ramble_node_unlock(n, acquired); return RAMBLE_ERR_STATE; }
    n->svc_stop = 0;
    n->svc_running = 1;
    if (!i_ramble_plat_thread_start(&n->svc, i_ramble_node_service, n)){
        n->svc_running = 0;
        i_ramble_node_unlock(n, acquired);
        return RAMBLE_ERR_NOSYS;
    }
    i_ramble_node_unlock(n, acquired);                    /* the service takes the lock now */
    return RAMBLE_OK;
#endif
}

int ramble_node_stop(RambleNode *n){
#ifndef RAMBLE_THREADS
    (void)n;
    return RAMBLE_OK;                                     /* nothing to stop, idempotent */
#else
    int acquired;
    if (!n) return RAMBLE_ERR_STATE;
    acquired = i_ramble_node_lock(n);
    if (!acquired) return RAMBLE_ERR_STATE;               /* from a callback (it is the service) */
    if (n->svc_joining){
        /* another thread owns the join: wait for it rather than join twice. Counted as
           a cv waiter so its broadcasts land */
        n->cv_waiters++;
        while (n->svc_running) i_ramble_node_cv_wait(n, 100000u);
        n->cv_waiters--;
        i_ramble_node_unlock(n, acquired);
        return RAMBLE_OK;
    }
    if (!n->svc_running){ i_ramble_node_unlock(n, acquired); return RAMBLE_OK; }
    n->svc_joining = 1;
    n->svc_stop = 1;
    i_ramble_node_kick(n);
    i_ramble_node_unlock(n, acquired);
    i_ramble_plat_thread_join(&n->svc);                   /* never joined holding the lock */
    acquired = i_ramble_node_lock(n);
    n->svc_running = 0;
    n->svc_stop = 0;
    n->svc_joining = 0;
    /* free every waiter: blocked senders proceed unwaited, a parked stopper returns */
    if (n->cv_waiters) i_ramble_plat_cond_broadcast(&n->cv);
    i_ramble_node_unlock(n, acquired);
    return RAMBLE_OK;
#endif
}

int ramble_node_is_started(RambleNode *n){
#ifndef RAMBLE_THREADS
    (void)n;
    return 0;
#else
    return (n && n->svc_running) ? 1 : 0;
#endif
}

void ramble_node_lock(RambleNode *n){
#ifndef RAMBLE_THREADS
    (void)n;
#else
    if (!n) return;
    if (n->lock_held && n->lock_owner == i_ramble_plat_thread_id()) return;  /* already ours */
    i_ramble_node_lock_raw(n);
    n->user_locked = 1;
#endif
}

void ramble_node_unlock(RambleNode *n){
#ifndef RAMBLE_THREADS
    (void)n;
#else
    if (!n || !n->user_locked) return;   /* only releases what ramble_node_lock took */
    if (!(n->lock_held && n->lock_owner == i_ramble_plat_thread_id())) return;
    n->user_locked = 0;
    i_ramble_node_unlock_raw(n);
#endif
}

int ramble_node_close(RambleNode *n, int send_bye){
    RambleAllocator pool;
    if (!n) return RAMBLE_OK;
#ifdef RAMBLE_THREADS
    /* from a callback: refuse loudly, the return code lets a binding keep its handle alive */
    if (n->lock_held && n->lock_owner == i_ramble_plat_thread_id()) return RAMBLE_ERR_STATE;
    ramble_node_stop(n);
    /* from here no other thread is inside, or may enter, any call on this node */
#endif
    /* settle outstanding pattern promises while the node is fully alive, cleared first so
       a callback closing the node cannot recurse */
    if (n->sys_on_close){
        i_RambleSysCloseFn f = n->sys_on_close;
        n->sys_on_close = NULL;
        f(n->sys_user);
    }
    /* discovery frees peer blobs via our hook, so it must close before the pool is copied out */
    if (n->discovery) ramble_discovery_close(n->discovery, send_bye);
    if (n->fd != RAMBLE_SOCK_BAD) i_ramble_plat_close(n->fd);
#ifdef RAMBLE_SHM
    if (n->shm_capable){                              /* the pool reset unmaps nothing */
        uint32_t i, n_segments = (uint32_t)n->shm_n_topics * RAMBLE_SHM_N_CLASSES;
        for (i=0;i<n_segments;i++)
            if (n->shm_pool[i]) i_ramble_shm_detach((i_RambleShmPool*)n->shm_pool[i]);
        for (i=0;i<n->shm_reader_count;i++)
            if (n->shm_reader_states[i]) i_ramble_shm_detach((i_RambleShmPool*)n->shm_reader_states[i]);
    }
#endif
#ifdef RAMBLE_THREADS
    if (n->waker.fd != RAMBLE_SOCK_BAD) i_ramble_plat_waker_close(&n->waker);
    i_ramble_plat_cond_destroy(&n->cv);
    i_ramble_plat_mutex_destroy(&n->mu);
#endif
    i_ramble_plat_cleanup();
    pool = n->pool;                  /* copied out last, the reset frees n itself */
    ramble_allocator_reset(&pool);   /* frees the node struct, arena, buffers, schemas and handles */
    return RAMBLE_OK;
}
#pragma endregion
#ifndef RAMBLE_NO_PATTERNS
#pragma region patterns/core.c
/* The patterns layer over the node's public API and its kind agnostic seams. The node
 * knows nothing of what these patterns mean. The rules are in spec/patterns.md. */
#include <string.h>
#include <stddef.h>            /* offsetof */

/* the per node manager: fans the node wide event and tick out to every entity */

/* one authority side entity keyed the way a peer's announce names it, for the duplicate
 * authority check */
typedef struct {
    uint32_t    hash;          /* the primary channel's low 32 name hash */
    uint8_t     kind;          /* RambleEntityKind */
    RambleTopic  *primary;     /* the entity's primary channel */
    uint32_t  **ids; uint16_t *n_ids, *cap;   /* the entity's reported peers list */
} i_RamblePatAuth;

typedef struct i_RamblePatterns {
    RambleNode            *n;
    struct RambleFunction *funcs;   /* linked lists for the tick and event fanout */
    struct RambleVariable *vars;
    /* the authorities sorted by (kind, hash), rebuilt lazily after a create or retire */
    i_RamblePatAuth       *auth; uint16_t auth_n, auth_cap; uint8_t auth_dirty;
    struct RambleFunction *meta;    /* the built in @ramble/meta endpoint, both sides in one handle */
    RambleSchema          *meta_rsp_schema;   /* RambleMeta { info: map } */
    uint8_t               *meta_msg; uint32_t meta_msg_cap;   /* reply scratch, grown on demand */
} i_RamblePatterns;

static void     i_ramble_patterns_on_event(void *user, const RambleEvent *ev);
static uint64_t i_ramble_patterns_tick(void *user, uint64_t now_us);
static void     i_ramble_patterns_on_close(void *user);
/* the pending call reaper, retire cancels through it */
static uint64_t i_ramble_func_reap(struct RambleFunction *fn, uint64_t now,
                                 RambleCallStatus fail_status, int all, uint32_t dest);
/* the duplicate authority sweep for a just created provider or owner */
static void i_ramble_pat_dup_sweep(RambleNode *n, RambleTopic *primary, RambleEntityKind kind,
                                 uint32_t **ids, uint16_t *n_ids, uint16_t *cap);
/* a peer's uuid low 32, the task wire's caller discriminator */
static uint32_t i_ramble_pat_peer_lo(RambleNode *n, uint32_t peer){
    const uint8_t *u = i_ramble_node_peer_uuid(n, peer);
    return u ? i_ramble_le_r32(u) : 0;
}

/* the entity channel belongs to as peer advertises it, by kind and base name. 1 + *out */
static int i_ramble_pat_peer_entity(RambleNode *n, uint32_t peer, RambleTopic *channel, RambleEntityKind kind,
                                  RambleEntityInfo *out){
    RambleString nm = i_ramble_topic_name(channel);
    RambleIter it; size_t len = nm.len;
    char buf[RAMBLE_TOPIC_NAME_MAX + 1]; uint32_t hash;
    memcpy(buf, nm.data, nm.len); buf[nm.len] = '\0';
    hash = (uint32_t)ramble_topic_id(buf);           /* the primary channel's announce hash */
    if (len >= 5 && nm.data[len - 4] == '@') len -= 4;
    memset(&it, 0, sizeof it);
    while (ramble_node_entities_next(n, peer, &it, out)){
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
static i_RamblePatterns *i_ramble_patterns_get(RambleNode *n){
    void **slot = i_ramble_node_sys_slot(n);
    i_RamblePatterns *pm = (i_RamblePatterns*)*slot;
    if (pm) return pm;
    pm = (i_RamblePatterns*)i_ramble_node_sys_alloc(n, NULL, sizeof *pm);
    if (!pm) return NULL;
    memset(pm, 0, sizeof *pm);
    pm->n = n; *slot = pm;
    i_ramble_node_set_sys_hooks(n, i_ramble_patterns_on_event, i_ramble_patterns_tick,
                              i_ramble_patterns_on_close, pm);
    return pm;
}

/* the shared entity mechanics: the create prologue and the three retire phases */

/* The create prologue: take the lock, get the manager, allocate a zeroed handle, release.
 * The lock must be released before the topics are created. See spec/patterns.md. */
static void *i_ramble_pat_handle_new(RambleNode *n, size_t size, i_RamblePatterns **pm_out){
    i_RamblePatterns *pm; void *h; int acquired;
    acquired = i_ramble_node_sys_lock(n);
    pm = i_ramble_patterns_get(n);
    h = pm ? i_ramble_node_sys_alloc(n, NULL, size) : NULL;
    if (h) memset(h, 0, size);
    i_ramble_node_sys_unlock(n, acquired);
    *pm_out = h ? pm : NULL;
    return h;
}

/* A partial create: the created half may already route into the handle, so park it and
 * keep the handle allocated until close. Returns NULL, the create's verdict. */
static void *i_ramble_pat_half_create_fail(RambleTopic *created_half){
    ramble_topic_set_role(created_half, RAMBLE_INACTIVE);
    return NULL;
}

/* Retire phase 1, before the lock: park the channels. set_role refuses from a callback
 * with nothing mutated, so the primary's verdict is the whole retire's. */
static int i_ramble_pat_park_channels(RambleTopic *primary, RambleTopic *secondary){
    int r = ramble_topic_set_role(primary, RAMBLE_INACTIVE);
    if (r != 0) return r;
    if (secondary) (void)ramble_topic_set_role(secondary, RAMBLE_INACTIVE);
    return 0;
}

/* Retire phase 2, lock held: drop the delivery routing into the handle. */
static void i_ramble_pat_clear_channels(RambleTopic *primary, RambleTopic *secondary){
    i_ramble_topic_clear_sys(primary);
    if (secondary) i_ramble_topic_clear_sys(secondary);
}

/* Retire phase 3, lock released: release the slots for reuse. The topics outlive the
 * handle free, since a reap callback fired under the lock may have used them. */
static void i_ramble_pat_release_channels(RambleTopic *primary, RambleTopic *secondary){
    (void)ramble_topic_retire(primary);
    if (secondary) (void)ramble_topic_retire(secondary);
}

/* Unlinks elem from a manager list threaded at next_off. An absent elem is a no op. Lock held. */
static void i_ramble_pat_unlink(void **head, void *elem, size_t next_off){
    void **pp = head;
    while (*pp){
        void **next = (void**)((char*)*pp + next_off);
        if (*pp == elem){ *pp = *next; return; }
        pp = next;
    }
}

/* functions */

#define RAMBLE__FN_PREFIX 5u   /* req: [u32 call_id][u8 op]. rsp: [u32 call_id][u8 status], then
                                [u8 msg_len][msg], which the node's split puts in the header */
#define RAMBLE__FN_RSP_HDR_MAX (RAMBLE__FN_PREFIX + 1u + RAMBLE_CALL_MSG_MAX)
#define RAMBLE__FN_OP_CALL   0u   /* the payload is the request */
#define RAMBLE__FN_OP_CANCEL 1u   /* task req channel, op only: an empty payload cancels the
                                   sender's call, [u32 caller_lo] another caller's */
#define RAMBLE__PRG_PREFIX 8u  /* prg: [u32 caller_lo][u32 call_id]. caller_lo demuxes the shared
                                broadcast, since call ids are per caller counters */
#define RAMBLE__NO_DEADLINE ((uint64_t)-1)   /* a RUNNING call has no timeout */

/* the default response text, so a generic consumer always has text for a failure */
static RambleString i_ramble_call_status_msg(RambleCallStatus s){
    switch (s){
    case RAMBLE_CALL_APP_ERROR:  return ramble_cstr("app error");
    case RAMBLE_CALL_NO_HANDLER: return ramble_cstr("no handler");
    case RAMBLE_CALL_TIMEOUT:    return ramble_cstr("timeout");
    case RAMBLE_CALL_PEER_LOST:  return ramble_cstr("peer lost");
    case RAMBLE_CALL_CANCELLED:  return ramble_cstr("cancelled");
    case RAMBLE_CALL_OK: default: return ramble_cstr("");
    }
}

/* A caller side outstanding call. queued_req != NULL = not on the wire yet: the request
 * waits for the first provider match. See spec/patterns.md. */
typedef struct i_RamblePending {
    struct i_RamblePending *next;
    uint32_t          call_id;
    uint32_t          dest;        /* the peer the call is directed at, 0 = undirected */
    uint64_t          deadline_us; /* RAMBLE__NO_DEADLINE once RUNNING or progress arrived */
    RambleResponseFn  on_response;
    void           *user;
    RambleProgressFn  on_progress; /* task: per progress update */
    void           *progress_user;
    uint8_t         running;     /* a non terminal response arrived, the deadline is dropped */
    uint8_t        *queued_req;
    uint32_t        queued_len;
} i_RamblePending;

/* a deferred provider reply, linked into the handle's live defer registry so every token
 * verb validates membership first */
typedef struct i_RambleDefer {
    struct i_RambleDefer *next;
    RambleFunction *fn;
    uint32_t      caller;
    uint32_t      caller_lo;   /* the caller's uuid low 32 for progress and cancel */
    uint32_t      call_id;
    uint8_t       cancelled;   /* a cancel landed */
} i_RambleDefer;

struct RambleFunction {
    RambleNode       *n;
    i_RamblePatterns *pm;
    struct RambleFunction *next;      /* the manager list */
    RambleTopic      *req;            /* provider SUB_ONLY, caller PUB_ONLY */
    RambleTopic      *rsp;            /* provider PUB_ONLY, caller SUB_ONLY, directed */
    RambleTopic      *prg;            /* task: the broadcast progress channel, NULL = a function */
    RambleRequestFn   on_request; void *on_request_user;   /* provider */
    uint32_t          next_call_id;   /* the caller counter */
    uint32_t          timeout_us;
    i_RamblePending  *pending;        /* caller: outstanding calls */
    i_RambleDefer    *defers;         /* provider: the live defer registry */
    RambleCancelFn    on_cancel; void *on_cancel_user;     /* task definition, one slot */
    uint32_t          self_lo;        /* our own uuid low 32, the progress demux filter */
    uint8_t        *sync_buf; uint32_t sync_cap;     /* the sync call reply scratch */
    char            sync_msg[RAMBLE_CALL_MSG_MAX];   /* the sync call message scratch */
    uint8_t         sync_msg_len;
    uint8_t         is_provider;    /* a pure definition, 0 for a both sides handle */
    uint8_t         both;           /* both sides in one handle, the @ramble/meta shape */
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
    RambleRequest   pub;
    RambleFunction *fn;
    uint32_t      call_id;
    uint32_t      caller_lo;   /* task: the caller's uuid low 32 */
    uint8_t       replied;     /* a reply, fail or defer happened: no auto ack */
    uint8_t       started;     /* task: RUNNING already sent */
} i_RambleRequest;
/* the downcast needs pub at offset 0, and C99 has no static assert */
typedef char i_ramble_request_pub_first[(offsetof(i_RambleRequest, pub) == 0) ? 1 : -1];

/* builds the response header. A message over RAMBLE_CALL_MSG_MAX truncates. Returns the length */
static size_t i_ramble_func_rsp_hdr(uint8_t *hdr, uint32_t call_id, uint8_t status,
                                  const char *message){
    size_t ml = message ? strlen(message) : 0;
    if (ml > RAMBLE_CALL_MSG_MAX) ml = RAMBLE_CALL_MSG_MAX;
    i_ramble_le_w32(hdr, call_id); hdr[4] = status; hdr[5] = (uint8_t)ml;
    if (ml) memcpy(hdr + 6, message, ml);
    return RAMBLE__FN_PREFIX + 1u + ml;
}

/* sends a response to one caller. Runs under the lock, so the send commits reentrantly */
static void i_ramble_func_send_reply(RambleFunction *fn, uint32_t caller, uint32_t call_id,
                                   uint8_t status, const char *message, RambleBytes rsp){
    uint8_t hdr[RAMBLE__FN_RSP_HDR_MAX];
    size_t hl = i_ramble_func_rsp_hdr(hdr, call_id, status, message);
    (void)i_ramble_topic_send_to(fn->rsp, caller, ramble_bytes(hdr, hl), rsp);
}

/* provider: a CANCEL op landed. The target is the payload's caller_lo, else the sender's.
 * A match on a live defer sets its flag and fires on_cancel once, no match is a no op. */
static void i_ramble_func_cancel_op(RambleFunction *fn, const RambleMsg *msg){
    uint32_t lo = msg->data.len >= 4 ? i_ramble_le_r32(msg->data.data)
                                     : i_ramble_pat_peer_lo(msg->node, msg->publisher_id);
    uint32_t call_id = i_ramble_le_r32(msg->header.data);
    i_RambleDefer *d;
    for (d = fn->defers; d; d = d->next)
        if (d->caller_lo == lo && d->call_id == call_id) break;
    if (!d || d->cancelled) return;
    d->cancelled = 1;
    if (fn->on_cancel) fn->on_cancel((uint64_t)(uintptr_t)d, fn->on_cancel_user);
}

/* provider: a request arrived, or an op */
static void i_ramble_func_on_request(void *user, const RambleMsg *msg){
    RambleFunction *fn = (RambleFunction*)user;
    i_RambleRequest r;
    if (msg->header.len < RAMBLE__FN_PREFIX) return;   /* a malformed prefix */
    if (msg->header.data[4] != RAMBLE__FN_OP_CALL){    /* an op, not a request */
        if (fn->is_task && msg->header.data[4] == RAMBLE__FN_OP_CANCEL)
            i_ramble_func_cancel_op(fn, msg);
        return;   /* an unknown op is dropped */
    }
    r.pub.node          = msg->node;
    r.pub.function_name = ramble_string(msg->topic_name.data,
                              msg->topic_name.len > 4u ? msg->topic_name.len - 4u : 0);
    r.pub.data          = msg->data;
    r.pub.schema        = msg->schema;
    r.pub.caller        = msg->publisher_id;
    r.pub.caller_name   = msg->publisher_name;
    r.pub.recv_us       = msg->recv_us;
    r.pub.written_us    = msg->written_us;
    r.fn = fn; r.call_id = i_ramble_le_r32(msg->header.data); r.replied = 0; r.started = 0;
    r.caller_lo = fn->is_task ? i_ramble_pat_peer_lo(msg->node, msg->publisher_id) : 0;
    if (fn->on_request) fn->on_request(&r.pub, fn->on_request_user);
    else {   /* no handler: NO_HANDLER with no text, the caller fills the default */
        i_ramble_func_send_reply(fn, r.pub.caller, r.call_id, RAMBLE_CALL_NO_HANDLER, NULL, ramble_bytes(NULL,0));
        r.replied = 1;
    }
    if (!r.replied){
        /* a function's return is its answer, a task's is not: an instant empty OK on a
           long operation would read as success that never ran */
        if (fn->is_task)
            i_ramble_func_send_reply(fn, r.pub.caller, r.call_id, RAMBLE_CALL_APP_ERROR,
                                   "handler returned no result", ramble_bytes(NULL,0));
        else
            i_ramble_func_send_reply(fn, r.pub.caller, r.call_id, RAMBLE_CALL_OK, NULL, ramble_bytes(NULL,0));
    }
}

/* caller: unlinks the pending entry for call_id, so a second provider's reply finds none */
static i_RamblePending *i_ramble_func_take_pending(RambleFunction *fn, uint32_t call_id){
    i_RamblePending **pp = &fn->pending, *p;
    for (; (p = *pp) != NULL; pp = &p->next)
        if (p->call_id == call_id){ *pp = p->next; return p; }
    return NULL;
}

/* caller: the pending entry for call_id, left linked */
static i_RamblePending *i_ramble_func_find_pending(RambleFunction *fn, uint32_t call_id){
    i_RamblePending *p;
    for (p = fn->pending; p; p = p->next) if (p->call_id == call_id) return p;
    return NULL;
}

/* a non terminal response reached p: the call runs as long as it runs now */
static void i_ramble_func_mark_running(i_RamblePending *p){
    p->running = 1; p->deadline_us = RAMBLE__NO_DEADLINE;
}

/* frees an unlinked pending entry and any queued request bytes */
static void i_ramble_func_free_pending(RambleFunction *fn, i_RamblePending *p){
    if (p->queued_req) i_ramble_node_sys_alloc(fn->n, p->queued_req, 0);
    i_ramble_node_sys_alloc(fn->n, p, 0);
}

/* Sends every call queued before a provider matched, oldest first. Runs under the lock,
 * so the sends commit reentrantly, fine on a fresh lane with empty history. */
static void i_ramble_func_flush_queued(RambleFunction *fn){
    if (fn->is_provider || !fn->pending) return;
    if (ramble_topic_match_count(fn->req) == 0) return;   /* still no provider */
    for (;;){
        i_RamblePending *pick = NULL, *p;
        uint8_t hdr[RAMBLE__FN_PREFIX];
        for (p = fn->pending; p; p = p->next) if (p->queued_req) pick = p;
        if (!pick) return;
        if (fn->is_task && !pick->dest)   /* task requests are always directed: resolve now */
            pick->dest = i_ramble_topic_oldest_match(fn->req);
        i_ramble_le_w32(hdr, pick->call_id); hdr[4] = RAMBLE__FN_OP_CALL;
        if (pick->dest)
            (void)i_ramble_topic_send_to(fn->req, pick->dest, ramble_bytes(hdr, RAMBLE__FN_PREFIX),
                                       ramble_bytes(pick->queued_req, pick->queued_len));
        else
            (void)i_ramble_topic_send_hdr(fn->req, ramble_bytes(hdr, RAMBLE__FN_PREFIX),
                                        ramble_bytes(pick->queued_req, pick->queued_len));
        i_ramble_node_sys_alloc(fn->n, pick->queued_req, 0);
        pick->queued_req = NULL; pick->queued_len = 0;
    }
}

/* caller: a progress datagram. caller_lo filters other callers' calls off the broadcast.
 * Progress can beat RUNNING on its own lane, so it also drops the deadline. */
static void i_ramble_func_on_progress(void *user, const RambleMsg *msg){
    RambleFunction *fn = (RambleFunction*)user;
    i_RamblePending *p;
    RambleProgress pr;
    if (msg->header.len < RAMBLE__PRG_PREFIX) return;
    if (i_ramble_le_r32(msg->header.data) != fn->self_lo) return;   /* another caller's call */
    p = i_ramble_func_find_pending(fn, i_ramble_le_r32(msg->header.data + 4));
    if (!p) return;   /* already answered: a straggler */
    i_ramble_func_mark_running(p);
    if (!p->on_progress) return;
    pr.call_id = p->call_id; pr.provider = msg->publisher_id;
    pr.data = msg->data; pr.schema = msg->schema;
    pr.written_us = msg->written_us; pr.recv_us = msg->recv_us;
    pr.user = p->progress_user;
    p->on_progress(&pr);
}

/* caller: a response arrived */
static void i_ramble_func_on_response(void *user, const RambleMsg *msg){
    RambleFunction *fn = (RambleFunction*)user;
    i_RamblePending *p;
    RambleResponse r;
    if (msg->header.len < RAMBLE__FN_PREFIX) return;
    if (fn->is_task && (RambleCallStatus)msg->header.data[4] == RAMBLE_CALL_RUNNING){
        /* non terminal: the call stays pending with no deadline and on_progress fires once
           with zero length data, unless progress already beat the status here */
        p = i_ramble_func_find_pending(fn, i_ramble_le_r32(msg->header.data));
        if (!p) return;
        if (!p->running && p->on_progress){
            RambleProgress pr;
            pr.call_id = p->call_id; pr.provider = msg->publisher_id;
            pr.data = ramble_bytes(NULL, 0); pr.schema = NULL;
            pr.written_us = msg->written_us; pr.recv_us = msg->recv_us;
            pr.user = p->progress_user;
            i_ramble_func_mark_running(p);
            p->on_progress(&pr);
        } else i_ramble_func_mark_running(p);
        return;
    }
    p = i_ramble_func_take_pending(fn, i_ramble_le_r32(msg->header.data));
    if (!p) return;                          /* an unknown or duplicate call_id: dropped */
    r.status = (RambleCallStatus)msg->header.data[4];
    r.data = msg->data; r.schema = msg->schema;
    r.provider = msg->publisher_id; r.user = p->user;
    r.written_us = msg->written_us;
    /* the message follows the prefix as [u8 len][msg], and the node's split already
       bounded it, so the header length is the truth. None = the default status text */
    r.message = (msg->header.len > RAMBLE__FN_PREFIX + 1u)
              ? ramble_string((const char*)msg->header.data + RAMBLE__FN_PREFIX + 1u,
                            msg->header.len - (RAMBLE__FN_PREFIX + 1u))
              : i_ramble_call_status_msg(r.status);
    if (p->on_response) p->on_response(&r);
    i_ramble_func_free_pending(fn, p);
}

/* a leading '@' is reserved for the @ramble/ builtins, refused in every public constructor */
static int i_ramble_pat_reserved(const char *name){ return name && name[0] == '@'; }

/* Creates the channels for a function or task. mode 1 = a definition, 0 = a remote, 2 =
 * both sides in one handle (the @ramble/meta shape). topts non NULL = the task shape. */
static RambleFunction *i_ramble_function_new(RambleNode *n, const char *name,
                    const RambleSchema *req_schema, const RambleSchema *rsp_schema,
                    RambleRequestFn on_request, void *user, const RambleFunctionOpts *opts, int mode,
                    const RambleSchema *prg_schema, const RambleTaskOpts *topts){
    i_RamblePatterns *pm; RambleFunction *fn;
    char rn[RAMBLE_TOPIC_NAME_MAX + 1]; size_t nl;
    RambleTopicOpts topt; int acquired;
    int handles_req = (mode != 0), makes_calls = (mode != 1);
    RambleRole req_role = mode == 2 ? RAMBLE_PUBSUB : (handles_req ? RAMBLE_SUB_ONLY : RAMBLE_PUB_ONLY);
    RambleRole rsp_role = mode == 2 ? RAMBLE_PUBSUB : (handles_req ? RAMBLE_PUB_ONLY : RAMBLE_SUB_ONLY);
    i_RambleSysMsgFn req_cb = handles_req ? i_ramble_func_on_request : NULL;
    i_RambleSysMsgFn rsp_cb = makes_calls ? i_ramble_func_on_response : NULL;
    uint8_t req_kind = topts ? RAMBLE_KIND_TASK_REQ : RAMBLE_KIND_FUNC_REQ;
    uint8_t rsp_kind = topts ? RAMBLE_KIND_TASK_RSP : RAMBLE_KIND_FUNC_RSP;
    uint8_t req_attrs = 0;
    if (topts && handles_req)   /* only the definition declares the task facts */
        req_attrs = (uint8_t)((topts->no_cancel ? 0u : RAMBLE_ATTR_CANCELLABLE)
                            | (topts->exclusive ? RAMBLE_ATTR_EXCLUSIVE : 0u)
                            | (topts->multi     ? RAMBLE_ATTR_MULTI     : 0u));
    if (!n || !name) return NULL;
    nl = strlen(name);
    if (nl == 0 || nl + 4 > RAMBLE_TOPIC_NAME_MAX) return NULL;   /* room for the suffix */
    memset(&topt, 0, sizeof topt);
    topt.qos.reliability = RAMBLE_RELIABLE;
    topt.qos.catch_up = 0;   /* calls carry no replay: history is purely the repair window */
    /* an inline reply is a reentrant send that cannot wait for a TX pass, so this ring is
       the only thing holding a drained batch of replies. See spec/patterns.md */
    topt.qos.keep_last = (opts && opts->keep_last) ? opts->keep_last : 0u;
    topt.qos.backpressure_wait_us = (opts && opts->backpressure_wait_us) ? opts->backpressure_wait_us
                                                                         : RAMBLE_PATTERN_BP_WAIT_US;
    fn = (RambleFunction*)i_ramble_pat_handle_new(n, sizeof *fn, &pm);
    if (!fn) return NULL;
    if (opts && opts->reflect_from_mesh){
        /* the caller writes @req and reads @rsp and @prg, a definition the reverse */
        RambleEntityKind ek = topts ? RAMBLE_ENTITY_TASK : RAMBLE_ENTITY_FUNCTION;
        const RambleSchema *ms;
        fn->reflect = 1;
        i_ramble_node_reflect_pick(n, ek, name, 0, !handles_req, &ms, NULL, &fn->generation);
        if (!req_schema) req_schema = ms;
        i_ramble_node_reflect_pick(n, ek, name, 1, handles_req, &ms, NULL, NULL);
        if (!rsp_schema) rsp_schema = ms;
        if (topts){
            i_ramble_node_reflect_pick(n, ek, name, 2, handles_req, &ms, NULL, NULL);
            if (!prg_schema) prg_schema = ms;
        }
    }
    fn->n = n; fn->pm = pm; fn->is_provider = (uint8_t)(mode == 1);
    fn->both = (uint8_t)(mode == 2);
    fn->multi = (uint8_t)(opts && opts->multi);
    fn->is_task = (uint8_t)(topts != NULL);
    if (topts){ fn->no_cancel = topts->no_cancel; fn->exclusive = topts->exclusive; }
    fn->on_request = on_request; fn->on_request_user = user;
    fn->timeout_us = (opts && opts->timeout_us) ? opts->timeout_us : RAMBLE_CALL_TIMEOUT_US;
    fn->next_call_id = 1;
    {   const uint8_t *u = i_ramble_node_uuid(n);   /* the progress demux filter */
        fn->self_lo = u ? i_ramble_le_r32(u) : 0;
    }

    memcpy(rn, name, nl); memcpy(rn + nl, "@req", 5);   /* NUL included */
    /* the req channel is directed in every mode, or a directed request would leak to every
       other matched provider when their lanes next wake. See spec/patterns.md */
    fn->req = i_ramble_node_create_pattern_topic(n, rn, req_role, req_schema, &topt,
                              req_kind, RAMBLE__FN_PREFIX, 1 /*directed*/, req_attrs, req_cb, fn);
    if (!fn->req) return NULL;   /* fn stays pool allocated: nothing routes into it yet */
    if (topts){
        /* the broadcast progress channel: reliability is the definition's offer or the
           remote's request, the RxO rule composes them */
        RambleTopicOpts popt = topt;
        popt.qos.reliability = topts->progress_best_effort ? RAMBLE_BEST_EFFORT : RAMBLE_RELIABLE;
        if (topts->progress_keep_last) popt.qos.keep_last = topts->progress_keep_last;
        memcpy(rn + nl, "@prg", 5);
        fn->prg = i_ramble_node_create_pattern_topic(n, rn,
                              handles_req ? RAMBLE_PUB_ONLY : RAMBLE_SUB_ONLY, prg_schema, &popt,
                              RAMBLE_KIND_TASK_PRG, RAMBLE__PRG_PREFIX, 0, 0,
                              handles_req ? NULL : i_ramble_func_on_progress, fn);
        if (!fn->prg)
            return (RambleFunction*)i_ramble_pat_half_create_fail(fn->req);
    }
    memcpy(rn + nl, "@rsp", 5);
    fn->rsp = i_ramble_node_create_pattern_topic(n, rn, rsp_role, rsp_schema, &topt,
                              rsp_kind, RAMBLE__FN_PREFIX, 1 /*directed*/, 0, rsp_cb, fn);
    if (!fn->rsp){  /* req and prg already route into fn: keep the handle, park the halves */
        if (fn->prg) (void)ramble_topic_set_role(fn->prg, RAMBLE_INACTIVE);
        return (RambleFunction*)i_ramble_pat_half_create_fail(fn->req);
    }

    acquired = i_ramble_node_sys_lock(n);       /* publish into the manager list */
    fn->next = pm->funcs; pm->funcs = fn; pm->auth_dirty = 1;
    if (handles_req && !fn->multi)   /* a rival provider may already be on the network */
        i_ramble_pat_dup_sweep(n, fn->req, topts ? RAMBLE_ENTITY_TASK : RAMBLE_ENTITY_FUNCTION,
                             &fn->dup_peers, &fn->dup_n, &fn->dup_cap);
    i_ramble_node_sys_unlock(n, acquired);
    return fn;
}

RambleFunction *ramble_node_create_function_definition(RambleNode *n, const char *name,
                    const RambleSchema *req_schema, const RambleSchema *rsp_schema,
                    RambleRequestFn on_request, void *user, const RambleFunctionOpts *opts){
    if (i_ramble_pat_reserved(name)) return NULL;
    return i_ramble_function_new(n, name, req_schema, rsp_schema, on_request, user, opts, 1, NULL, NULL);
}
RambleFunction *ramble_node_create_remote_function(RambleNode *n, const char *name,
                    const RambleSchema *req_schema, const RambleSchema *rsp_schema,
                    const RambleFunctionOpts *opts){
    if (i_ramble_pat_reserved(name)) return NULL;
    return i_ramble_function_new(n, name, req_schema, rsp_schema, NULL, NULL, opts, 0, NULL, NULL);
}

/* a pattern channel's entity name, the topic name minus its suffix, in a static scratch */
static const char *i_ramble_pat_base_name(RambleTopic *channel){
    static char buf[RAMBLE_TOPIC_NAME_MAX + 1];
    RambleString nm = i_ramble_topic_name(channel);
    size_t len = nm.len;
    if (len >= 5 && nm.data[len - 4] == '@') len -= 4;
    memcpy(buf, nm.data, len); buf[len] = '\0';
    return buf;
}

/* the shared function fields of RambleTaskOpts, so the task constructors reuse one path */
static RambleFunctionOpts i_ramble_task_fn_opts(const RambleTaskOpts *to){
    RambleFunctionOpts fo;
    memset(&fo, 0, sizeof fo);
    fo.backpressure_wait_us = to->backpressure_wait_us;
    fo.timeout_us = to->timeout_us;
    fo.keep_last = to->keep_last;
    fo.multi = to->multi;
    return fo;
}

RambleFunction *ramble_node_create_task_definition(RambleNode *n, const char *name,
                    const RambleSchema *req_schema, const RambleSchema *prg_schema,
                    const RambleSchema *rsp_schema, RambleRequestFn on_request, void *user,
                    const RambleTaskOpts *opts){
    RambleTaskOpts to; RambleFunctionOpts fo;
    if (i_ramble_pat_reserved(name)) return NULL;
    memset(&to, 0, sizeof to); if (opts) to = *opts;
    fo = i_ramble_task_fn_opts(&to);
    return i_ramble_function_new(n, name, req_schema, rsp_schema, on_request, user, &fo, 1,
                               prg_schema, &to);
}
RambleFunction *ramble_node_create_remote_task(RambleNode *n, const char *name,
                    const RambleSchema *req_schema, const RambleSchema *prg_schema,
                    const RambleSchema *rsp_schema, const RambleTaskOpts *opts){
    RambleTaskOpts to; RambleFunctionOpts fo;
    if (i_ramble_pat_reserved(name)) return NULL;
    memset(&to, 0, sizeof to); if (opts) to = *opts;
    fo = i_ramble_task_fn_opts(&to);
    return i_ramble_function_new(n, name, req_schema, rsp_schema, NULL, NULL, &fo, 0,
                               prg_schema, &to);
}

/* Links the pending entry under the lock, then sends outside it so the request engages the
 * flow control wait. A task call with no provider auto directs at the oldest matched. */
static int i_ramble_function_call_id(RambleFunction *fn, RambleBytes req, RambleResponseFn on_response,
                                   void *user, const RambleCallOpts *opts, uint32_t *id_out){
    uint8_t hdr[RAMBLE__FN_PREFIX]; i_RamblePending *p; int acquired, r; uint32_t id;
    uint32_t dest = opts ? opts->provider : 0;
    if (!fn || !fn->req || !fn->rsp) return RAMBLE_ERR_NO_TOPIC;
    acquired = i_ramble_node_sys_lock(fn->n);
    p = (i_RamblePending*)i_ramble_node_sys_alloc(fn->n, NULL, sizeof *p);
    if (!p){ i_ramble_node_sys_unlock(fn->n, acquired); return RAMBLE_ERR_OOM; }
    if (fn->is_task && !dest) dest = i_ramble_topic_oldest_match(fn->req);   /* always directed */
    id = fn->next_call_id++;
    p->call_id = id;
    p->dest = dest;
    p->deadline_us = i_ramble_node_now_us(fn->n) + fn->timeout_us;
    p->on_response = on_response; p->user = user;
    p->on_progress = opts ? opts->on_progress : NULL;
    p->progress_user = opts ? opts->progress_user : NULL;
    p->running = 0;
    p->queued_req = NULL; p->queued_len = 0;
    p->next = fn->pending; fn->pending = p;
    if (id_out) *id_out = id;
    if (opts && opts->id_out) *opts->id_out = id;   /* set before the send */
    if (ramble_topic_match_count(fn->req) == 0){
        /* no provider matched yet: the transport would drop an unmatched send, so queue
           the request and flush the instant the match forms. See spec/patterns.md */
        p->queued_req = (uint8_t*)i_ramble_node_sys_alloc(fn->n, NULL, req.len ? req.len : 1u);
        if (!p->queued_req){
            fn->pending = p->next;
            i_ramble_node_sys_alloc(fn->n, p, 0);
            i_ramble_node_sys_unlock(fn->n, acquired);
            return RAMBLE_ERR_OOM;
        }
        if (req.len) memcpy(p->queued_req, req.data, req.len);
        p->queued_len = (uint32_t)req.len;
        i_ramble_node_sys_unlock(fn->n, acquired);
        return RAMBLE_OK;
    }
    i_ramble_node_sys_unlock(fn->n, acquired);
    i_ramble_le_w32(hdr, id); hdr[4] = RAMBLE__FN_OP_CALL;
    r = dest ? i_ramble_topic_send_to(fn->req, dest, ramble_bytes(hdr, RAMBLE__FN_PREFIX), req)
             : i_ramble_topic_send_hdr(fn->req, ramble_bytes(hdr, RAMBLE__FN_PREFIX), req);
    if (r != RAMBLE_OK){
        acquired = i_ramble_node_sys_lock(fn->n);
        p = i_ramble_func_take_pending(fn, id);   /* may already be reaped or answered: then gone */
        if (p) i_ramble_func_free_pending(fn, p);
        i_ramble_node_sys_unlock(fn->n, acquired);
        return r;
    }
    return RAMBLE_OK;
}

int ramble_function_call_async(RambleFunction *fn, RambleBytes req, RambleResponseFn on_response,
                             void *user, const RambleCallOpts *opts){
    return i_ramble_function_call_id(fn, req, on_response, user, opts, NULL);
}

/* Answers and frees every live deferred call of a definition with CANCELLED. Lock held,
 * the replies commit reentrantly, so run it while the rsp channel is still up. */
static void i_ramble_func_drain_defers(RambleFunction *fn, const char *message){
    while (fn->defers){
        i_RambleDefer *d = fn->defers; fn->defers = d->next;
        i_ramble_func_send_reply(fn, d->caller, d->call_id, RAMBLE_CALL_CANCELLED,
                               message, ramble_bytes(NULL, 0));
        i_ramble_node_sys_alloc(fn->n, d, 0);
    }
}

int ramble_function_retire(RambleFunction *fn){
    i_RamblePatterns *pm; RambleNode *n; int acquired, r;
    RambleTopic *req, *rsp, *prg;
    if (!fn) return RAMBLE_ERR_NO_TOPIC;
    pm = fn->pm; n = fn->n;
    if (pm && fn == pm->meta) return RAMBLE_ERR_STATE;   /* the builtin is node infrastructure */
    acquired = i_ramble_node_sys_lock(n);
    if (!acquired){   /* from a callback: refuse with nothing mutated */
        i_ramble_node_sys_unlock(n, acquired);
        return RAMBLE_ERR_STATE;
    }
    /* live deferred calls answer CANCELLED while the channels are still up, and the
       replies must flush to the wire before the park below tears the lanes down */
    i_ramble_func_drain_defers(fn, "provider retired");
    i_ramble_node_sys_unlock(n, acquired);
    i_ramble_node_flush_tx(n);
    r = i_ramble_pat_park_channels(fn->req, fn->rsp);
    if (r != 0) return r;
    if (fn->prg) (void)ramble_topic_set_role(fn->prg, RAMBLE_INACTIVE);
    acquired = i_ramble_node_sys_lock(n);
    i_ramble_pat_clear_channels(fn->req, fn->rsp);
    if (fn->prg) i_ramble_topic_clear_sys(fn->prg);
    /* a request that deferred in the drain to park window: a best effort answer, and the
       entries are freed either way */
    i_ramble_func_drain_defers(fn, "provider retired");
    /* every outstanding call gets its one outcome, CANCELLED. A reentrant call from a
       cancel callback queues and the next round cancels it too */
    while (fn->pending) i_ramble_func_reap(fn, 0, RAMBLE_CALL_CANCELLED, 1, 0);
    if (pm){ i_ramble_pat_unlink((void**)&pm->funcs, fn, offsetof(RambleFunction, next)); pm->auth_dirty = 1; }
    req = fn->req; rsp = fn->rsp; prg = fn->prg;   /* outlive fn, see phase 3 */
    if (fn->sync_buf)  i_ramble_node_sys_alloc(n, fn->sync_buf, 0);
    if (fn->dup_peers) i_ramble_node_sys_alloc(n, fn->dup_peers, 0);
    i_ramble_node_sys_alloc(n, fn, 0);
    i_ramble_node_sys_unlock(n, acquired);
    i_ramble_pat_release_channels(req, rsp);
    if (prg) (void)ramble_topic_retire(prg);
    return RAMBLE_OK;
}

int ramble_function_refresh(RambleFunction *fn){
    RambleNode *n; RambleEntityKind ek; uint64_t gen = 0; int acquired, r;
    const RambleSchema *rq = NULL, *rs = NULL, *pg = NULL;
    if (!fn) return RAMBLE_ERR_NO_TOPIC;
    if (!fn->reflect) return RAMBLE_ERR_ROLE;
    n = fn->n; ek = fn->is_task ? RAMBLE_ENTITY_TASK : RAMBLE_ENTITY_FUNCTION;
    if (!i_ramble_node_reflect_pick(n, ek, i_ramble_pat_base_name(fn->req), 0, !fn->is_provider, &rq, NULL, &gen)
        || gen == fn->generation) return 0;
    i_ramble_node_reflect_pick(n, ek, i_ramble_pat_base_name(fn->req), 1, fn->is_provider, &rs, NULL, NULL);
    if (fn->prg) i_ramble_node_reflect_pick(n, ek, i_ramble_pat_base_name(fn->req), 2, fn->is_provider, &pg, NULL, NULL);
    /* every outstanding call gets its one outcome before the lanes go */
    acquired = i_ramble_node_sys_lock(n);
    if (!acquired){ i_ramble_node_sys_unlock(n, acquired); return RAMBLE_ERR_STATE; }
    i_ramble_func_drain_defers(fn, "provider re-typed");
    while (fn->pending) i_ramble_func_reap(fn, 0, RAMBLE_CALL_CANCELLED, 1, 0);
    i_ramble_node_sys_unlock(n, acquired);
    i_ramble_node_flush_tx(n);
    r = i_ramble_topic_retype(fn->req, rq, RAMBLE_RELIABLE);
    if (r != 0) return r;
    r = i_ramble_topic_retype(fn->rsp, rs, RAMBLE_RELIABLE);
    if (r != 0) return r;
    if (fn->prg){
        r = i_ramble_topic_retype(fn->prg, pg, i_ramble_topic_reliability(fn->prg));
        if (r != 0) return r;
    }
    fn->generation = gen;
    return 1;
}

/* The blocking call's response capture: copies the payload into the function's scratch.
 * The schema pointer is safe to hold, an interned schema lives until close. */
typedef struct { RambleFunction *fn; volatile int done; RambleCallStatus status;
                 const RambleSchema *schema; uint32_t len; uint32_t provider;
                 uint64_t written_us; } i_RambleSyncCtx;
static void i_ramble_func_sync_response(const RambleResponse *r){
    i_RambleSyncCtx *c = (i_RambleSyncCtx*)r->user;
    RambleFunction *fn = c->fn;
    c->status = r->status; c->schema = r->schema; c->len = (uint32_t)r->data.len;
    c->provider = r->provider; c->written_us = r->written_us;
    {   /* the message view dies with the callback: copy into the handle's scratch */
        size_t ml = r->message.len <= RAMBLE_CALL_MSG_MAX ? r->message.len : RAMBLE_CALL_MSG_MAX;
        if (ml) memcpy(fn->sync_msg, r->message.data, ml);
        fn->sync_msg_len = (uint8_t)ml;
    }
    if (r->data.len){
        if (fn->sync_cap < r->data.len){
            uint8_t *nb = (uint8_t*)i_ramble_node_sys_alloc(fn->n, fn->sync_buf, r->data.len);
            if (nb){ fn->sync_buf = nb; fn->sync_cap = (uint32_t)r->data.len; }
            else c->len = 0;
        }
        if (fn->sync_cap >= r->data.len) memcpy(fn->sync_buf, r->data.data, r->data.len);
    }
    c->done = 1;
}

int ramble_function_call(RambleFunction *fn, RambleBytes req, RambleResponse *out, int timeout_ms,
                       const RambleCallOpts *opts){
    i_RambleSyncCtx ctx; int acquired, r; uint64_t deadline; uint32_t id;
    if (!fn) return RAMBLE_ERR_NO_TOPIC;
    acquired = i_ramble_node_sys_lock(fn->n);
    if (!acquired || ramble_node_is_started(fn->n)){   /* a callback or a service thread */
        i_ramble_node_sys_unlock(fn->n, acquired);
        return RAMBLE_ERR_STATE;
    }
    i_ramble_node_sys_unlock(fn->n, acquired);
    ctx.fn = fn; ctx.done = 0; ctx.status = RAMBLE_CALL_TIMEOUT; ctx.schema = NULL; ctx.len = 0;
    ctx.provider = 0; ctx.written_us = 0;
    r = i_ramble_function_call_id(fn, req, i_ramble_func_sync_response, &ctx, opts, &id);
    if (r != RAMBLE_OK) return r;
    deadline = i_ramble_node_now_us(fn->n)
             + (uint64_t)(timeout_ms >= 0 ? (uint64_t)timeout_ms * 1000u : fn->timeout_us) + 20000u;
    while (!ctx.done){
        if (i_ramble_node_now_us(fn->n) >= deadline){
            /* a task that reached RUNNING has no deadline: wait for the terminal outcome,
               and cancel from another thread is the impatience tool */
            int running = 0;
            acquired = i_ramble_node_sys_lock(fn->n);
            {   i_RamblePending *p = i_ramble_func_find_pending(fn, id);
                running = p && p->running;
            }
            i_ramble_node_sys_unlock(fn->n, acquired);
            if (!running) break;
            deadline = RAMBLE__NO_DEADLINE;
        }
        i_ramble_node_sys_poll(fn->n, 5);            /* the tick synthesizes TIMEOUT */
    }
    if (!ctx.done){
        /* the local wait expired first: unlink the entry before this frame dies, or a late
           response fires into a reclaimed frame. A racing poller may have completed it */
        i_RamblePending *p;
        acquired = i_ramble_node_sys_lock(fn->n);
        if (!ctx.done && (p = i_ramble_func_take_pending(fn, id)) != NULL)
            i_ramble_node_sys_alloc(fn->n, p, 0);
        i_ramble_node_sys_unlock(fn->n, acquired);
    }
    if (out){
        memset(out, 0, sizeof *out);
        out->status = ctx.done ? ctx.status : RAMBLE_CALL_TIMEOUT;
        out->data = ramble_bytes(fn->sync_buf,
                               (ctx.done && ctx.status != RAMBLE_CALL_TIMEOUT) ? ctx.len : 0);
        out->schema = out->data.len ? ctx.schema : NULL;
        out->provider = ctx.done ? ctx.provider : 0;
        out->written_us = ctx.done ? ctx.written_us : 0;
        /* the same lifetime rule as data: the scratch holds it until the next blocking call */
        out->message = ctx.done ? ramble_string(fn->sync_msg, fn->sync_msg_len)
                                : i_ramble_call_status_msg(RAMBLE_CALL_TIMEOUT);
    }
    /* 0 = timed out, whichever deadline expired first, 1 = a real outcome */
    return (ctx.done && ctx.status != RAMBLE_CALL_TIMEOUT) ? 1 : 0;
}

int ramble_function_match_count(RambleFunction *fn){
    if (!fn) return 0;
    return ramble_topic_match_count(fn->is_provider ? fn->rsp : fn->req);
}

/* The provider handler API. The handler's RambleRequest is the pub head of an i_RambleRequest,
 * so these recover the reply machinery by downcast. A copy answers nothing. */
static void i_ramble_request_answer(RambleRequest *request, uint8_t status, const char *message,
                                  RambleBytes rsp){
    i_RambleRequest *r = (i_RambleRequest*)request;
    if (!request || r->replied) return;
    r->replied = 1;
    i_ramble_func_send_reply(r->fn, request->caller, r->call_id, status, message, rsp);
}
void ramble_request_reply(RambleRequest *request, RambleBytes rsp){
    i_ramble_request_answer(request, RAMBLE_CALL_OK, NULL, rsp);
}
void ramble_request_fail (RambleRequest *request, const char *message, RambleBytes rsp){
    i_ramble_request_answer(request, RAMBLE_CALL_APP_ERROR, message, rsp);
}

int ramble_request_start(RambleRequest *request){
    i_RambleRequest *r = (i_RambleRequest*)request;
    if (!request) return RAMBLE_ERR_NO_TOPIC;
    if (!r->fn->is_task || r->replied) return RAMBLE_ERR_STATE;   /* task only, before the answer */
    if (r->started) return RAMBLE_OK;   /* idempotent */
    r->started = 1;
    i_ramble_func_send_reply(r->fn, request->caller, r->call_id, RAMBLE_CALL_RUNNING,
                           NULL, ramble_bytes(NULL, 0));
    return RAMBLE_OK;
}

uint64_t ramble_request_defer(RambleRequest *request){
    i_RambleRequest *r = (i_RambleRequest*)request;
    i_RambleDefer *d;
    int acquired;
    if (!request || r->replied) return 0;
    if (r->fn->is_task && !r->started) (void)ramble_request_start(request);   /* implies RUNNING */
    acquired = i_ramble_node_sys_lock(r->fn->n);
    d = (i_RambleDefer*)i_ramble_node_sys_alloc(r->fn->n, NULL, sizeof *d);
    if (d){
        d->fn = r->fn; d->caller = request->caller; d->caller_lo = r->caller_lo;
        d->call_id = r->call_id; d->cancelled = 0;
        d->next = r->fn->defers; r->fn->defers = d;   /* into the live defer registry */
        r->replied = 1;   /* no auto ack, the reply comes via ramble_function_complete */
    }
    i_ramble_node_sys_unlock(r->fn->n, acquired);
    return (uint64_t)(uintptr_t)d;
}

/* the token as a live defer of fn, unlink_it pops it. NULL = stale. Lock held */
static i_RambleDefer *i_ramble_func_defer_find(RambleFunction *fn, uint64_t token, int unlink_it){
    i_RambleDefer **pp = &fn->defers, *d = (i_RambleDefer*)(uintptr_t)token;
    for (; *pp; pp = &(*pp)->next)
        if (*pp == d){ if (unlink_it) *pp = d->next; return d; }
    return NULL;
}

int ramble_function_complete(RambleFunction *fn, uint64_t token, RambleCallStatus status,
                           const char *message, RambleBytes rsp){
    i_RambleDefer *d;
    uint8_t hdr[RAMBLE__FN_RSP_HDR_MAX]; size_t hl; RambleTopic *rsp_topic; uint32_t caller;
    int acquired;
    if (!fn || !token) return RAMBLE_ERR_NO_TOPIC;
    acquired = i_ramble_node_sys_lock(fn->n);
    d = i_ramble_func_defer_find(fn, token, 1);
    if (!d){   /* a stale token answers nothing and frees nothing */
        i_ramble_node_sys_unlock(fn->n, acquired);
        return RAMBLE_ERR_STATE;
    }
    hl = i_ramble_func_rsp_hdr(hdr, d->call_id, (uint8_t)status, message);
    rsp_topic = fn->rsp; caller = d->caller;
    i_ramble_node_sys_alloc(fn->n, d, 0);
    i_ramble_node_sys_unlock(fn->n, acquired);
    /* send outside the lock: a completion from an app thread engages backpressure, from a
       callback it commits reentrantly */
    return i_ramble_topic_send_to(rsp_topic, caller, ramble_bytes(hdr, hl), rsp);
}

int ramble_function_progress(RambleFunction *fn, uint64_t token, RambleBytes progress){
    i_RambleDefer *d;
    uint8_t hdr[RAMBLE__PRG_PREFIX]; RambleTopic *prg;
    int acquired;
    if (!fn) return RAMBLE_ERR_NO_TOPIC;
    if (!fn->is_task) return RAMBLE_ERR_STATE;
    acquired = i_ramble_node_sys_lock(fn->n);
    d = i_ramble_func_defer_find(fn, token, 0);
    if (!d){ i_ramble_node_sys_unlock(fn->n, acquired); return RAMBLE_ERR_STATE; }
    i_ramble_le_w32(hdr, d->caller_lo); i_ramble_le_w32(hdr + 4, d->call_id);
    prg = fn->prg;
    i_ramble_node_sys_unlock(fn->n, acquired);
    /* broadcast outside the lock: reliable subscribers get backpressure end to end, best
       effort observers are fire and forget */
    return i_ramble_topic_send_hdr(prg, ramble_bytes(hdr, RAMBLE__PRG_PREFIX), progress);
}

int ramble_function_cancelled(RambleFunction *fn, uint64_t token){
    i_RambleDefer *d; int acquired, r;
    if (!fn) return RAMBLE_ERR_NO_TOPIC;
    if (!fn->is_task) return RAMBLE_ERR_STATE;
    acquired = i_ramble_node_sys_lock(fn->n);
    d = i_ramble_func_defer_find(fn, token, 0);
    r = d ? (d->cancelled ? 1 : 0) : RAMBLE_ERR_STATE;
    i_ramble_node_sys_unlock(fn->n, acquired);
    return r;
}

int ramble_function_on_cancel(RambleFunction *def, RambleCancelFn on_cancel, void *user){
    int acquired;
    if (!def) return RAMBLE_ERR_NO_TOPIC;
    if (!def->is_task || !def->is_provider) return RAMBLE_ERR_STATE;
    acquired = i_ramble_node_sys_lock(def->n);
    def->on_cancel = on_cancel; def->on_cancel_user = user;
    i_ramble_node_sys_unlock(def->n, acquired);
    return RAMBLE_OK;
}

int ramble_function_cancel(RambleFunction *fn, uint32_t call_id){
    i_RamblePending *p; int acquired; uint32_t dest;
    uint8_t hdr[RAMBLE__FN_PREFIX];
    if (!fn || !fn->req) return RAMBLE_ERR_NO_TOPIC;
    acquired = i_ramble_node_sys_lock(fn->n);
    p = i_ramble_func_find_pending(fn, call_id);
    if (!p){   /* already answered or never made: nothing to cancel */
        i_ramble_node_sys_unlock(fn->n, acquired);
        return RAMBLE_ERR_STATE;
    }
    if (p->queued_req){
        /* never sent: cancel locally with the one CANCELLED outcome */
        RambleResponse r;
        (void)i_ramble_func_take_pending(fn, call_id);
        r.status = RAMBLE_CALL_CANCELLED; r.data = ramble_bytes(NULL, 0);
        r.schema = NULL; r.provider = 0; r.user = p->user; r.written_us = 0;
        r.message = i_ramble_call_status_msg(RAMBLE_CALL_CANCELLED);
        if (p->on_response) p->on_response(&r);
        i_ramble_func_free_pending(fn, p);
        i_ramble_node_sys_unlock(fn->n, acquired);
        return RAMBLE_OK;
    }
    dest = p->dest;
    {   /* the provider's declared attrs gate cancel locally: no CANCELLABLE bit refuses
           with nothing sent, like a read only variable */
        RambleEntityInfo ei;
        int cancellable = i_ramble_pat_peer_entity(fn->n, dest, fn->req, RAMBLE_ENTITY_TASK, &ei)
                          && ei.cancellable;
        if (!cancellable){
            i_ramble_node_sys_unlock(fn->n, acquired);
            return RAMBLE_ERR_ROLE;
        }
    }
    i_ramble_node_sys_unlock(fn->n, acquired);
    /* never acked: delivery is reliable and the terminal status is the answer. A cancel
       racing the call's own answer lands on nothing at the provider */
    i_ramble_le_w32(hdr, call_id); hdr[4] = RAMBLE__FN_OP_CANCEL;
    return i_ramble_topic_send_to(fn->req, dest, ramble_bytes(hdr, RAMBLE__FN_PREFIX), ramble_bytes(NULL, 0));
}

/* The built in @ramble/meta endpoint, hosted at node open as a both sides multi function.
 * The node builds the snapshot, this layer wires it into the function machinery. */

/* the node pool RambleAllocFn adapter */
static void *i_ramble_pat_alloc(void *user, void *ptr, size_t size){
    return i_ramble_node_sys_alloc((RambleNode*)user, ptr, size);
}

static void i_ramble_meta_on_request(RambleRequest *request, void *user){
    RambleNode *n = (RambleNode*)user;
    i_RamblePatterns *pm = (i_RamblePatterns*)*i_ramble_node_sys_slot(n);
    uint32_t mask = 0, need;
    RambleBytes body;
    if (!pm || !pm->meta_rsp_schema){ ramble_request_fail(request, "meta schema unavailable", ramble_bytes(NULL, 0)); return; }
    if (request->data.len >= 4) mask = i_ramble_le_r32(request->data.data);   /* empty = everything */
    body = i_ramble_node_snapshot(n, mask);
    if (!body.data){ ramble_request_fail(request, "snapshot failed", ramble_bytes(NULL, 0)); return; }
    need = ramble_schema_msg_min(pm->meta_rsp_schema) + (uint32_t)body.len;
    if (pm->meta_msg_cap < need){
        uint8_t *nb = (uint8_t*)i_ramble_node_sys_alloc(n, pm->meta_msg, need);
        if (!nb){ ramble_request_fail(request, "out of memory", ramble_bytes(NULL, 0)); return; }
        pm->meta_msg = nb; pm->meta_msg_cap = need;
    }
    if (!ramble_schema_message_default(pm->meta_rsp_schema, pm->meta_msg, pm->meta_msg_cap)
        || !ramble_set_map(pm->meta_msg, pm->meta_msg_cap, pm->meta_rsp_schema, "info", body)){
        ramble_request_fail(request, "meta encode failed", ramble_bytes(NULL, 0));
        return;
    }
    ramble_request_reply(request, ramble_bytes(pm->meta_msg,
                ramble_schema_msg_len(pm->meta_rsp_schema, pm->meta_msg, pm->meta_msg_cap)));
}

/* the node open seam declared in node/runtime.c: hosts @ramble/meta on this node */
void i_ramble_patterns_meta_open(RambleNode *n){
    i_RamblePatterns *pm;
    RambleSchema *rsp;
    RambleFunctionOpts fo;
    int acquired;
    if (!n) return;
    acquired = i_ramble_node_sys_lock(n);
    pm = i_ramble_patterns_get(n);
    if (pm && !pm->meta_rsp_schema)
        pm->meta_rsp_schema = ramble_schema_compile(i_ramble_pat_alloc, n, "RambleMeta { info: map }", NULL);
    rsp = pm ? pm->meta_rsp_schema : NULL;
    i_ramble_node_sys_unlock(n, acquired);
    if (!pm || pm->meta || !rsp) return;   /* OOM degrades: no endpoint, the node stays healthy */
    memset(&fo, 0, sizeof fo);
    fo.multi = 1;                          /* every node hosting one is the design */
    pm->meta = i_ramble_function_new(n, "@ramble/meta", NULL /* the raw mask */, rsp,
                                   i_ramble_meta_on_request, n, &fo, 2 /* both sides */, NULL, NULL);
}

RambleFunction *ramble_node_meta_function(RambleNode *n){
    i_RamblePatterns *pm; RambleFunction *meta; int acquired;
    if (!n) return NULL;
    acquired = i_ramble_node_sys_lock(n);
    pm = (i_RamblePatterns*)*i_ramble_node_sys_slot(n);
    meta = pm ? pm->meta : NULL;
    i_ramble_node_sys_unlock(n, acquired);
    return meta;
}

/* variables */

#define RAMBLE__VAR_PREFIX      5u    /* value channel: [u8 flags][u32 write_seq] */
#define RAMBLE__VAR_FLAG_FORCED 0x01u
#define RAMBLE__SET_PREFIX      1u    /* set channel: [u8 op] */
#define RAMBLE__SET_OP_FORCE    0x01u
#define RAMBLE__SET_OP_UNFORCE  0x02u

struct RambleVariable {
    RambleNode       *n;
    i_RamblePatterns *pm;
    struct RambleVariable *next;   /* the manager list */
    RambleTopic      *value;   /* owner PUB_ONLY, accessor SUB_ONLY */
    RambleTopic      *set;     /* owner SUB_ONLY, accessor PUB_ONLY, NULL = a read only owner */
    uint8_t         is_owner, allow_force, readonly, forced, has_value;
    uint32_t        write_seq;
    uint8_t        *store;  uint32_t store_len,  store_cap;    /* the value or the cache */
    uint8_t        *shadow; uint32_t shadow_len, shadow_cap;   /* owner: the source while forced */
    RambleVariableUpdateFn on_change; void *on_change_user;   /* fires only on a state change */
    RambleVariableUpdateFn on_write;  void *on_write_user;    /* fires on every applied write */
    const RambleSchema *cur_schema;  /* what store decodes with */
    uint32_t        last_source;     /* the peer behind the current state, 0 = a local call */
    uint64_t        last_write_us;   /* when the current state applied, node clock */
    uint64_t        last_written_us;   /* the writer's wall clock for that write, 0 = unstamped */
    uint32_t       *dup_peers;      /* owner: peers already reported for duplicate authority */
    uint16_t        dup_n, dup_cap;
    uint8_t         reflect;        /* created with reflect_from_mesh */
    uint64_t        generation;     /* the mesh generation the schema was taken at */
};

/* copies v into a grown buffer, 0 on OOM with the buffer unchanged */
static int i_ramble_buf_put(RambleNode *n, uint8_t **buf, uint32_t *len, uint32_t *cap, RambleBytes v){
    if (*cap < v.len){
        uint8_t *nb = (uint8_t*)i_ramble_node_sys_alloc(n, *buf, v.len ? v.len : 1u);
        if (!nb) return 0;
        *buf = nb; *cap = (uint32_t)(v.len ? v.len : 1u);
    }
    if (v.len) memcpy(*buf, v.data, v.len);
    *len = (uint32_t)v.len;
    return 1;
}

static void i_ramble_var_hdr(const RambleVariable *v, uint8_t hdr[RAMBLE__VAR_PREFIX]){
    hdr[0] = (uint8_t)(v->forced ? RAMBLE__VAR_FLAG_FORCED : 0);
    i_ramble_le_w32(hdr + 1, v->write_seq);
}

/* under the lock, before the store is overwritten: would applying (val, forced_now) change
 * the observed state? The first value, different bytes, or a forced flag flip. */
static int i_ramble_var_would_change(const RambleVariable *v, RambleBytes val, int forced_now){
    if (!v->has_value) return 1;
    if ((v->forced != 0) != (forced_now != 0)) return 1;
    if (v->store_len != val.len) return 1;
    return val.len ? memcmp(v->store, val.data, val.len) != 0 : 0;
}

/* the current state as a RambleVariableUpdate, views into manager memory */
static void i_ramble_var_update_view(RambleVariable *v, RambleVariableUpdate *u){
    u->variable   = v;
    u->name       = i_ramble_topic_name(v->value);
    u->value      = ramble_bytes(v->store, v->store_len);
    u->schema     = v->cur_schema;
    u->forced     = v->forced;
    u->write_seq  = v->write_seq;
    u->source     = v->last_source;
    u->recv_us    = v->last_write_us;
    u->written_us = v->last_written_us;
}

/* A write just applied, lock held: stamp the write context, then fire on_write always and
 * on_change when the state changed, inline on this thread. */
static void i_ramble_var_notify(RambleVariable *v, int changed, uint32_t source, uint64_t when_us,
                              uint64_t written_us){
    RambleVariableUpdate u;
    v->last_source = source; v->last_write_us = when_us; v->last_written_us = written_us;
    if (!v->on_write && !(changed && v->on_change)) return;
    i_ramble_var_update_view(v, &u);
    if (v->on_write)             v->on_write(&u, v->on_write_user);
    if (changed && v->on_change) v->on_change(&u, v->on_change_user);
}

/* owner: publishes the store under a held lock, a reentrant send with no wait */
static void i_ramble_var_publish_locked(RambleVariable *v){
    uint8_t hdr[RAMBLE__VAR_PREFIX];
    i_ramble_var_hdr(v, hdr);
    (void)i_ramble_topic_send_hdr(v->value, ramble_bytes(hdr, RAMBLE__VAR_PREFIX),
                                ramble_bytes(v->store, v->store_len));
}

/* owner: a dumb write from a held lock context. Absorbed into the shadow while forced,
 * with no event, else stored, published and notified. */
static void i_ramble_var_owner_apply(RambleVariable *v, RambleBytes val, uint32_t source,
                                   uint64_t when_us, uint64_t written_us){
    int changed;
    if (v->forced){ i_ramble_buf_put(v->n, &v->shadow, &v->shadow_len, &v->shadow_cap, val); return; }
    changed = i_ramble_var_would_change(v, val, 0);
    if (!i_ramble_buf_put(v->n, &v->store, &v->store_len, &v->store_cap, val)) return;
    v->has_value = 1; v->write_seq++;
    i_ramble_var_publish_locked(v);
    i_ramble_var_notify(v, changed, source, when_us, written_us);
}
/* owner: force to val. Without allow_force this is a silent no op, force is opaque on the
 * wire and the local API refuses loudly before reaching here. */
static void i_ramble_var_owner_force(RambleVariable *v, RambleBytes val, uint32_t source,
                                   uint64_t when_us, uint64_t written_us){
    int changed;
    if (!v->allow_force) return;
    changed = i_ramble_var_would_change(v, val, 1);
    if (!v->forced)   /* entering force: save the current source into the shadow */
        i_ramble_buf_put(v->n, &v->shadow, &v->shadow_len, &v->shadow_cap, ramble_bytes(v->store, v->store_len));
    if (!i_ramble_buf_put(v->n, &v->store, &v->store_len, &v->store_cap, val)) return;
    v->forced = 1; v->has_value = 1; v->write_seq++;
    i_ramble_var_publish_locked(v);
    i_ramble_var_notify(v, changed, source, when_us, written_us);
}
static void i_ramble_var_owner_unforce(RambleVariable *v, uint32_t source, uint64_t when_us,
                                     uint64_t written_us){
    if (!v->forced) return;
    v->forced = 0;
    i_ramble_buf_put(v->n, &v->store, &v->store_len, &v->store_cap, ramble_bytes(v->shadow, v->shadow_len));
    v->write_seq++;
    i_ramble_var_publish_locked(v);
    i_ramble_var_notify(v, 1, source, when_us, written_us);   /* a flag flip is always a change */
}

/* owner: a set, force or unforce op arrived, lock held. A plain set must carry a value,
 * an empty one is ignored. */
static void i_ramble_var_on_set(void *user, const RambleMsg *msg){
    RambleVariable *v = (RambleVariable*)user;
    uint8_t op = msg->header.len >= 1 ? msg->header.data[0] : 0;
    if (op & RAMBLE__SET_OP_UNFORCE)    i_ramble_var_owner_unforce(v, msg->publisher_id, msg->recv_us, msg->written_us);
    else if (op & RAMBLE__SET_OP_FORCE) { if (msg->data.len) i_ramble_var_owner_force(v, msg->data, msg->publisher_id, msg->recv_us, msg->written_us); }
    else                              { if (msg->data.len) i_ramble_var_owner_apply(v, msg->data, msg->publisher_id, msg->recv_us, msg->written_us); }
}

/* accessor: a new value arrived. Cache it with its forced flag and write_seq, then notify
 * with the owner as the source. */
static void i_ramble_var_on_value(void *user, const RambleMsg *msg){
    RambleVariable *v = (RambleVariable*)user;
    int forced_in, changed; uint32_t seq_in;
    if (msg->header.len < RAMBLE__VAR_PREFIX) return;
    /* the stale order guard: a same owner value behind the cached seq is dropped (signed
       distance, wrap safe). A different publisher always applies. See spec/patterns.md */
    seq_in = i_ramble_le_r32(msg->header.data + 1);
    if (v->has_value && msg->publisher_id == v->last_source
        && (int32_t)(seq_in - v->write_seq) < 0) return;
    forced_in = (msg->header.data[0] & RAMBLE__VAR_FLAG_FORCED) ? 1 : 0;
    changed = i_ramble_var_would_change(v, msg->data, forced_in);
    if (i_ramble_buf_put(v->n, &v->store, &v->store_len, &v->store_cap, msg->data)){
        v->has_value = 1;
        v->forced = (uint8_t)forced_in;
        v->write_seq = i_ramble_le_r32(msg->header.data + 1);
        v->cur_schema = msg->schema;
        i_ramble_var_notify(v, changed, msg->publisher_id, msg->recv_us, msg->written_us);
    }
}

static RambleVariable *i_ramble_variable_new(RambleNode *n, const char *name, const RambleSchema *schema,
                                         const RambleVariableOpts *opts, int owner){
    i_RamblePatterns *pm; RambleVariable *v;
    char sn[RAMBLE_TOPIC_NAME_MAX + 1]; size_t nl;
    RambleTopicOpts vopt, sopt; int acquired, readonly, make_set;
    if (!n || !name) return NULL;
    nl = strlen(name);
    if (nl == 0 || nl + 4 > RAMBLE_TOPIC_NAME_MAX) return NULL;   /* room for "@set" */
    readonly = (opts && opts->access == RAMBLE_VAR_READONLY);
    make_set = owner ? !readonly : 1;   /* a read only owner has no set channel */

    memset(&vopt, 0, sizeof vopt);
    vopt.qos.reliability = RAMBLE_RELIABLE;
    vopt.qos.catch_up = (opts && opts->catch_up) ? opts->catch_up : 1u;   /* for late accessors */
    /* keep_last is the repair window, not the replay window. Tying it to catch_up once left
       a reliable variable one slot deep. 0 = the reliable default (10). See spec/patterns.md */
    vopt.qos.keep_last = (opts && opts->keep_last) ? opts->keep_last : 0u;
    if (vopt.qos.keep_last && vopt.qos.keep_last < vopt.qos.catch_up)
        vopt.qos.keep_last = vopt.qos.catch_up;      /* the ring must hold what it replays */
    vopt.qos.backpressure_wait_us = (opts && opts->backpressure_wait_us) ? opts->backpressure_wait_us
                                                                         : RAMBLE_PATTERN_BP_WAIT_US;
    sopt = vopt; sopt.qos.catch_up = 0;   /* the set channel: no replay, the same repair depth */

    v = (RambleVariable*)i_ramble_pat_handle_new(n, sizeof *v, &pm);
    if (!v) return NULL;
    if (opts && opts->reflect_from_mesh){
        const RambleSchema *ms;
        v->reflect = 1;
        i_ramble_node_reflect_pick(n, RAMBLE_ENTITY_VARIABLE, name, 0, owner, &ms, NULL, &v->generation);
        if (!schema) schema = ms;
    }
    v->n = n; v->pm = pm; v->is_owner = (uint8_t)owner;
    v->allow_force = (uint8_t)(opts && opts->allow_force);
    v->readonly = (uint8_t)readonly;
    v->cur_schema = schema;
    v->value = i_ramble_node_create_pattern_topic(n, name, owner ? RAMBLE_PUB_ONLY : RAMBLE_SUB_ONLY,
                              schema, &vopt, RAMBLE_KIND_VARIABLE, RAMBLE__VAR_PREFIX, 0,
                              (uint8_t)((owner && v->allow_force) ? RAMBLE_ATTR_FORCEABLE : 0u),
                              owner ? NULL : i_ramble_var_on_value, v);
    if (!v->value) return NULL;   /* v stays pool allocated: nothing routes into it yet */
    if (owner)
        /* seed write_seq from the slot's continuing seqno line, so a successor definition's
           first write orders above its predecessor's last. See spec/patterns.md */
        v->write_seq = (uint32_t)i_ramble_topic_seqno(v->value);
    if (make_set){
        memcpy(sn, name, nl); memcpy(sn + nl, "@set", 5);
        v->set = i_ramble_node_create_pattern_topic(n, sn, owner ? RAMBLE_SUB_ONLY : RAMBLE_PUB_ONLY,
                              schema, &sopt, RAMBLE_KIND_VAR_SET, RAMBLE__SET_PREFIX, 0, 0,
                              owner ? i_ramble_var_on_set : NULL, v);
        if (!v->set)   /* the value topic may route to v: keep the handle */
            return (RambleVariable*)i_ramble_pat_half_create_fail(v->value);
    }
    acquired = i_ramble_node_sys_lock(n);       /* publish into the manager list */
    v->next = pm->vars; pm->vars = v; pm->auth_dirty = 1;
    if (owner)      /* a rival owner may already be on the network */
        i_ramble_pat_dup_sweep(n, v->value, RAMBLE_ENTITY_VARIABLE,
                             &v->dup_peers, &v->dup_n, &v->dup_cap);
    i_ramble_node_sys_unlock(n, acquired);
    if (owner && opts && opts->initial.len)   /* seed the store and history */
        ramble_variable_set(v, opts->initial);
    return v;
}

RambleVariable *ramble_node_create_variable_definition(RambleNode *n, const char *name,
                              const RambleSchema *schema, const RambleVariableOpts *opts){
    if (i_ramble_pat_reserved(name)) return NULL;
    return i_ramble_variable_new(n, name, schema, opts, 1);
}
RambleVariable *ramble_node_create_remote_variable(RambleNode *n, const char *name,
                              const RambleSchema *schema, const RambleVariableOpts *opts){
    if (i_ramble_pat_reserved(name)) return NULL;
    return i_ramble_variable_new(n, name, schema, opts, 0);
}

int ramble_variable_get(RambleVariable *var, RambleBytes *out){
    int acquired, has;
    if (!var) return 0;
    acquired = i_ramble_node_sys_lock(var->n);
    has = var->has_value;
    if (out){ out->data = var->store; out->len = has ? var->store_len : 0; }
    i_ramble_node_sys_unlock(var->n, acquired);
    return has;   /* out views manager memory */
}

/* accessor write routing: no owner at all versus a read only owner */
static int i_ramble_var_accessor_route(RambleVariable *var){
    if (i_ramble_topic_source_match_count(var->value) == 0) return RAMBLE_ERR_NO_TOPIC;
    if (!var->set || ramble_topic_match_count(var->set) == 0) return RAMBLE_ERR_ROLE;
    return RAMBLE_OK;
}

/* The routed form every accessor write goes through: while the owner match is still
 * forming, wait on the set channel like a first send does, then re derive the verdict. */
static int i_ramble_var_accessor_route_wait(RambleVariable *var){
    int r = i_ramble_var_accessor_route(var);
    if (r == RAMBLE_OK || !var->set) return r;
    (void)i_ramble_topic_match_wait(var->set);
    return i_ramble_var_accessor_route(var);
}

int ramble_variable_set(RambleVariable *var, RambleBytes value){
    int acquired, r = RAMBLE_OK, publish = 0;
    uint32_t my_seq = 0;
    uint8_t hdr[RAMBLE__VAR_PREFIX];
    if (!var) return RAMBLE_ERR_NO_TOPIC;
    if (var->is_owner){
        /* mutate the store under the lock, publish outside it from the caller's buffer so
           the send engages backpressure */
        acquired = i_ramble_node_sys_lock(var->n);
        if (var->forced){
            i_ramble_buf_put(var->n, &var->shadow, &var->shadow_len, &var->shadow_cap, value);
        } else {
            int changed = i_ramble_var_would_change(var, value, 0);
            if (!i_ramble_buf_put(var->n, &var->store, &var->store_len, &var->store_cap, value)){
                r = RAMBLE_ERR_OOM;
            } else {
                var->has_value = 1; var->write_seq++;
                my_seq = var->write_seq;
                i_ramble_var_hdr(var, hdr);
                publish = 1;
                i_ramble_var_notify(var, changed, 0, i_ramble_node_now_us(var->n),
                                  i_ramble_node_wall_us(var->n));   /* a local write: our clock */
                /* a reentrant set inside the callback committed first, so sending ours now
                   would put stale bytes newest in history. Skip, the write itself stands */
                if (var->write_seq != my_seq) publish = 0;
            }
        }
        i_ramble_node_sys_unlock(var->n, acquired);
        return publish ? i_ramble_topic_send_hdr(var->value, ramble_bytes(hdr, RAMBLE__VAR_PREFIX), value) : r;
    }
    r = i_ramble_var_accessor_route_wait(var);
    if (r != RAMBLE_OK) return r;
    {   uint8_t op = 0;
        return i_ramble_topic_send_hdr(var->set, ramble_bytes(&op, 1), value);
    }
}

int ramble_variable_force(RambleVariable *var, RambleBytes value){
    int acquired, r = RAMBLE_OK;
    if (!var) return RAMBLE_ERR_NO_TOPIC;
    if (var->is_owner){
        if (!var->allow_force) return RAMBLE_ERR_STATE;   /* locally checkable: refuse loudly */
        acquired = i_ramble_node_sys_lock(var->n);
        i_ramble_var_owner_force(var, value, 0, i_ramble_node_now_us(var->n),
                               i_ramble_node_wall_us(var->n));   /* a rare debug op, reentrant */
        i_ramble_node_sys_unlock(var->n, acquired);
        return RAMBLE_OK;
    }
    r = i_ramble_var_accessor_route_wait(var);
    if (r != RAMBLE_OK) return r;
    {   uint8_t op = RAMBLE__SET_OP_FORCE;
        return i_ramble_topic_send_hdr(var->set, ramble_bytes(&op, 1), value);
    }
}

int ramble_variable_unforce(RambleVariable *var){
    int acquired, r = RAMBLE_OK;
    if (!var) return RAMBLE_ERR_NO_TOPIC;
    if (var->is_owner){
        if (!var->allow_force) return RAMBLE_ERR_STATE;
        acquired = i_ramble_node_sys_lock(var->n);
        i_ramble_var_owner_unforce(var, 0, i_ramble_node_now_us(var->n), i_ramble_node_wall_us(var->n));
        i_ramble_node_sys_unlock(var->n, acquired);
        return RAMBLE_OK;
    }
    r = i_ramble_var_accessor_route_wait(var);
    if (r != RAMBLE_OK) return r;
    {   uint8_t op = RAMBLE__SET_OP_UNFORCE;
        return i_ramble_topic_send_hdr(var->set, ramble_bytes(&op, 1), ramble_bytes(NULL,0));
    }
}

int ramble_variable_forced(RambleVariable *var){ return var ? var->forced : 0; }

int ramble_variable_retire(RambleVariable *var){
    i_RamblePatterns *pm; RambleNode *n; int acquired, r;
    RambleTopic *value, *set;
    if (!var) return RAMBLE_ERR_NO_TOPIC;
    pm = var->pm; n = var->n;
    r = i_ramble_pat_park_channels(var->value, var->set);   /* set is NULL on a read only owner */
    if (r != 0) return r;
    acquired = i_ramble_node_sys_lock(n);
    i_ramble_pat_clear_channels(var->value, var->set);
    if (pm){ i_ramble_pat_unlink((void**)&pm->vars, var, offsetof(RambleVariable, next)); pm->auth_dirty = 1; }
    value = var->value; set = var->set;   /* outlive var, see phase 3 */
    if (var->store)     i_ramble_node_sys_alloc(n, var->store, 0);
    if (var->shadow)    i_ramble_node_sys_alloc(n, var->shadow, 0);
    if (var->dup_peers) i_ramble_node_sys_alloc(n, var->dup_peers, 0);
    i_ramble_node_sys_alloc(n, var, 0);
    i_ramble_node_sys_unlock(n, acquired);
    i_ramble_pat_release_channels(value, set);
    return RAMBLE_OK;
}

int ramble_variable_refresh(RambleVariable *var){
    RambleNode *n; uint64_t gen = 0; int r;
    const RambleSchema *ms = NULL;
    if (!var) return RAMBLE_ERR_NO_TOPIC;
    if (!var->reflect) return RAMBLE_ERR_ROLE;
    n = var->n;
    if (!i_ramble_node_reflect_pick(n, RAMBLE_ENTITY_VARIABLE, i_ramble_pat_base_name(var->value), 0,
                                  var->is_owner, &ms, NULL, &gen)
        || gen == var->generation) return 0;
    r = i_ramble_topic_retype(var->value, ms, RAMBLE_RELIABLE);
    if (r != 0) return r;
    if (var->set){
        r = i_ramble_topic_retype(var->set, ms, RAMBLE_RELIABLE);
        if (r != 0) return r;
    }
    var->generation = gen;
    return 1;
}

int ramble_variable_on_change(RambleVariable *var, RambleVariableUpdateFn on_change, void *user){
    int acquired;
    if (!var) return RAMBLE_ERR_NO_TOPIC;
    acquired = i_ramble_node_sys_lock(var->n);
    var->on_change = on_change; var->on_change_user = user;
    if (on_change && var->has_value){
        /* replay the current state once, right here, so a value that arrived between
           create and register is never missed */
        RambleVariableUpdate u;
        i_ramble_var_update_view(var, &u);
        on_change(&u, user);
    }
    i_ramble_node_sys_unlock(var->n, acquired);
    return RAMBLE_OK;
}

int ramble_variable_on_write(RambleVariable *var, RambleVariableUpdateFn on_write, void *user){
    int acquired;
    if (!var) return RAMBLE_ERR_NO_TOPIC;
    acquired = i_ramble_node_sys_lock(var->n);
    var->on_write = on_write; var->on_write_user = user;
    i_ramble_node_sys_unlock(var->n, acquired);
    return RAMBLE_OK;   /* writes are events, not state: no replay */
}

int ramble_variable_wait(RambleVariable *var, int timeout_ms){
    int acquired; uint64_t deadline;
    if (!var) return 0;
    acquired = i_ramble_node_sys_lock(var->n);
    if (!acquired || ramble_node_is_started(var->n)){ i_ramble_node_sys_unlock(var->n, acquired); return var->has_value; }
    if (var->has_value){ i_ramble_node_sys_unlock(var->n, acquired); return 1; }
    deadline = i_ramble_node_now_us(var->n) + (uint64_t)(timeout_ms >= 0 ? (uint64_t)timeout_ms*1000u : 3600000000ull);
    i_ramble_node_sys_unlock(var->n, acquired);
    while (!var->has_value){
        if (i_ramble_node_now_us(var->n) >= deadline) break;
        i_ramble_node_sys_poll(var->n, 5);
    }
    return var->has_value;
}

int ramble_variable_match_count(RambleVariable *var){
    if (!var) return 0;
    return ramble_topic_match_count(var->is_owner ? var->value : var->set);
}

/* Duplicate authority detection. Two authorities never match each other, so the announce
   interest is checked instead. The rules are in spec/patterns.md. */

static int i_ramble_pat_dup_reported(const uint32_t *ids, uint16_t n_ids, uint32_t peer){
    uint16_t i;
    for (i = 0; i < n_ids; i++) if (ids[i] == peer) return 1;
    return 0;
}

/* remembers peer in the entity's reported list. On OOM it may report again later */
static void i_ramble_pat_dup_remember(RambleNode *n, uint32_t **ids, uint16_t *n_ids,
                                    uint16_t *cap, uint32_t peer){
    if (*n_ids == *cap){
        uint16_t grown = *cap ? (uint16_t)(*cap * 2u) : 4u;
        uint32_t *nb = (uint32_t*)i_ramble_node_sys_alloc(n, *ids, grown * sizeof **ids);
        if (!nb) return;
        *ids = nb; *cap = grown;
    }
    (*ids)[(*n_ids)++] = peer;
}

/* forgets peer in one entity's reported list, so a genuine return re reports */
static void i_ramble_pat_dup_drop(uint32_t *ids, uint16_t *n_ids, uint32_t peer){
    uint16_t i;
    for (i = 0; i < *n_ids; i++)
        if (ids[i] == peer){ ids[i] = ids[--*n_ids]; return; }
}

/* checks one authority side entity against one active peer, reporting a fresh claim once */
static void i_ramble_pat_dup_check(RambleNode *n, uint32_t peer, RambleTopic *primary, RambleEntityKind kind,
                                 uint32_t **ids, uint16_t *n_ids, uint16_t *cap){
    RambleEntityInfo ei;
    if (!primary) return;
    if (i_ramble_pat_dup_reported(*ids, *n_ids, peer)) return;
    if (!i_ramble_pat_peer_entity(n, peer, primary, kind, &ei) || !ei.provides) return;
    i_ramble_pat_dup_remember(n, ids, n_ids, cap, peer);
    i_ramble_node_sys_error(n, RAMBLE_E_DUPLICATE_AUTHORITY, primary, peer);
}

static RambleEntityKind i_ramble_pat_fn_entity_kind(const RambleFunction *fn){
    return fn->is_task ? RAMBLE_ENTITY_TASK : RAMBLE_ENTITY_FUNCTION;
}

/* (kind, hash) order for the authority index */
static int i_ramble_pat_auth_before(const i_RamblePatAuth *a, const i_RamblePatAuth *b){
    return a->kind != b->kind ? a->kind < b->kind : a->hash < b->hash;
}

static void i_ramble_pat_auth_put(i_RamblePatterns *pm, uint32_t hash, RambleEntityKind kind,
                                RambleTopic *primary, uint32_t **ids, uint16_t *n_ids, uint16_t *cap){
    i_RamblePatAuth *a = &pm->auth[pm->auth_n++];
    a->hash = hash; a->kind = (uint8_t)kind; a->primary = primary;
    a->ids = ids; a->n_ids = n_ids; a->cap = cap;
}

static uint32_t i_ramble_pat_auth_hash(RambleTopic *primary){
    RambleString nm = i_ramble_topic_name(primary);
    char buf[RAMBLE_TOPIC_NAME_MAX + 1];
    memcpy(buf, nm.data, nm.len); buf[nm.len] = '\0';
    return (uint32_t)ramble_topic_id(buf);           /* what the announce carries for it */
}

/* rebuilds the sorted authority index from the entity lists, a shell sort. 0 on OOM */
static int i_ramble_pat_auth_rebuild(i_RamblePatterns *pm){
    RambleFunction *fn; RambleVariable *v; uint16_t want = 0, gap, i, j;
    for (fn = pm->funcs; fn; fn = fn->next) if ((fn->is_provider || fn->both) && !fn->multi) want++;
    for (v = pm->vars; v; v = v->next) if (v->is_owner) want++;
    if (want > pm->auth_cap){
        i_RamblePatAuth *nb = (i_RamblePatAuth*)i_ramble_node_sys_alloc(pm->n, pm->auth,
                                                                  (size_t)want * sizeof *nb);
        if (!nb) return 0;
        pm->auth = nb; pm->auth_cap = want;
    }
    pm->auth_n = 0;
    for (fn = pm->funcs; fn; fn = fn->next)
        if ((fn->is_provider || fn->both) && !fn->multi)   /* multi: rivals are the design */
            i_ramble_pat_auth_put(pm, i_ramble_pat_auth_hash(fn->req), i_ramble_pat_fn_entity_kind(fn),
                                fn->req, &fn->dup_peers, &fn->dup_n, &fn->dup_cap);
    for (v = pm->vars; v; v = v->next)
        if (v->is_owner)
            i_ramble_pat_auth_put(pm, i_ramble_pat_auth_hash(v->value), RAMBLE_ENTITY_VARIABLE,
                                v->value, &v->dup_peers, &v->dup_n, &v->dup_cap);
    for (gap = pm->auth_n / 2u; gap > 0; gap /= 2u)
        for (i = gap; i < pm->auth_n; i++){
            i_RamblePatAuth t = pm->auth[i];
            for (j = i; j >= gap && i_ramble_pat_auth_before(&t, &pm->auth[j - gap]); j -= gap)
                pm->auth[j] = pm->auth[j - gap];
            pm->auth[j] = t;
        }
    pm->auth_dirty = 0;
    return 1;
}

/* the first index whose (kind, hash) is not below the key */
static uint16_t i_ramble_pat_auth_lower(const i_RamblePatterns *pm, RambleEntityKind kind, uint32_t hash){
    uint16_t lo = 0, hi = pm->auth_n;
    i_RamblePatAuth key; key.kind = (uint8_t)kind; key.hash = hash;
    while (lo < hi){
        uint16_t mid = (uint16_t)(lo + (hi - lo) / 2u);
        if (i_ramble_pat_auth_before(&pm->auth[mid], &key)) lo = (uint16_t)(mid + 1u); else hi = mid;
    }
    return lo;
}

/* Every authority against one peer whose interest was just applied: one walk of the
 * peer's entities with a binary search each. On OOM the index is skipped this time. */
static void i_ramble_pat_dup_check_peer(i_RamblePatterns *pm, uint32_t peer){
    RambleIter it; RambleEntityInfo ei;
    if (pm->auth_dirty && !i_ramble_pat_auth_rebuild(pm)) return;
    if (!pm->auth_n) return;
    memset(&it, 0, sizeof it);
    while (ramble_node_entities_next(pm->n, peer, &it, &ei)){
        uint16_t k;
        if (!ei.provides || ei.kind == RAMBLE_ENTITY_TOPIC) continue;
        for (k = i_ramble_pat_auth_lower(pm, ei.kind, ei.hash);
             k < pm->auth_n && pm->auth[k].kind == (uint8_t)ei.kind && pm->auth[k].hash == ei.hash; k++){
            i_RamblePatAuth *a = &pm->auth[k];
            if (ei.name.len){
                RambleString nm = i_ramble_topic_name(a->primary); size_t len = nm.len;
                if (len >= 5 && nm.data[len - 4] == '@') len -= 4;   /* the entity's base name */
                if (ei.name.len != len || memcmp(ei.name.data, nm.data, len) != 0) continue;
            }
            if (i_ramble_pat_dup_reported(*a->ids, *a->n_ids, peer)) continue;
            i_ramble_pat_dup_remember(pm->n, a->ids, a->n_ids, a->cap, peer);
            i_ramble_node_sys_error(pm->n, RAMBLE_E_DUPLICATE_AUTHORITY, a->primary, peer);
        }
    }
}

/* a just created authority against every active peer */
static void i_ramble_pat_dup_sweep(RambleNode *n, RambleTopic *primary, RambleEntityKind kind,
                                 uint32_t **ids, uint16_t *n_ids, uint16_t *cap){
    RambleIter it; RamblePeerInfo p;
    memset(&it, 0, sizeof it);
    while (ramble_node_peers_next(n, &it, &p))
        if (p.liveness == RAMBLE_PEER_ACTIVE)
            i_ramble_pat_dup_check(n, p.id, primary, kind, ids, n_ids, cap);
}

static void i_ramble_pat_dup_forget_peer(i_RamblePatterns *pm, uint32_t peer){
    RambleFunction *fn; RambleVariable *v;
    for (fn = pm->funcs; fn; fn = fn->next) i_ramble_pat_dup_drop(fn->dup_peers, &fn->dup_n, peer);
    for (v = pm->vars; v; v = v->next)      i_ramble_pat_dup_drop(v->dup_peers, &v->dup_n, peer);
}

/* the manager hooks: call timeouts on the tick, provider loss on the event */

/* Fails pending calls with one synthesized outcome each: those directed at dest, or all,
 * or those past now. Callbacks run after the unlink. Returns the earliest deadline left. */
static uint64_t i_ramble_func_reap(RambleFunction *fn, uint64_t now, RambleCallStatus fail_status,
                                 int all, uint32_t dest){
    i_RamblePending **pp = &fn->pending, *p;
    uint64_t soonest = 0;
    while ((p = *pp) != NULL){
        if (dest ? (p->dest == dest) : (all || now >= p->deadline_us)){
            *pp = p->next;
            {   RambleResponse r; r.status = fail_status; r.data = ramble_bytes(NULL,0);
                r.schema = NULL; r.provider = 0; r.user = p->user; r.written_us = 0;
                r.message = i_ramble_call_status_msg(fail_status);
                if (p->on_response) p->on_response(&r);
            }
            i_ramble_func_free_pending(fn, p);
            continue;
        }
        if (!soonest || p->deadline_us < soonest) soonest = p->deadline_us;
        pp = &p->next;
    }
    return soonest;
}

/* Fails every sent call directed at peer whose lane no longer exists with CANCELLED: the
 * wire CANCELLED can be lost to the very announce that severed it. See spec/patterns.md. */
static void i_ramble_func_reap_severed(RambleFunction *fn, uint32_t peer){
    i_RamblePending **pp = &fn->pending, *p;
    while ((p = *pp) != NULL){
        if (p->dest == peer && !p->queued_req && !i_ramble_topic_peer_matched(fn->req, peer)){
            *pp = p->next;
            {   RambleResponse r; r.status = RAMBLE_CALL_CANCELLED; r.data = ramble_bytes(NULL,0);
                r.schema = NULL; r.provider = 0; r.user = p->user; r.written_us = 0;
                r.message = ramble_cstr("provider retired");
                if (p->on_response) p->on_response(&r);
            }
            i_ramble_func_free_pending(fn, p);
            continue;
        }
        pp = &p->next;
    }
}

static uint64_t i_ramble_patterns_tick(void *user, uint64_t now_us){
    i_RamblePatterns *pm = (i_RamblePatterns*)user;
    RambleFunction *fn; uint64_t soonest = 0;
    for (fn = pm->funcs; fn; fn = fn->next){
        uint64_t s;
        i_ramble_func_flush_queued(fn);   /* the backstop: the interest event is the fast path */
        s = i_ramble_func_reap(fn, now_us, RAMBLE_CALL_TIMEOUT, 0, 0);
        if (s && (!soonest || s < soonest)) soonest = s;
    }
    return soonest;   /* the next timeout deadline for the poll wait cap */
}

/* The node is closing: every pending call gets CANCELLED and every live deferred call
 * answers CANCELLED while the channels are still up. Runs once, node still fully alive. */
static void i_ramble_patterns_on_close(void *user){
    i_RamblePatterns *pm = (i_RamblePatterns*)user;
    RambleFunction *fn;
    for (fn = pm->funcs; fn; fn = fn->next){
        if (fn->defers){
            int acquired = i_ramble_node_sys_lock(pm->n);
            i_ramble_func_drain_defers(fn, "node closing");
            i_ramble_node_sys_unlock(pm->n, acquired);
        }
        if (!fn->is_provider && fn->pending)
            i_ramble_func_reap(fn, 0, RAMBLE_CALL_CANCELLED, 1, 0);
    }
    /* the drained CANCELLED replies must leave before the socket closes: no poll pass
       follows this hook */
    i_ramble_node_flush_tx(pm->n);
}

static void i_ramble_patterns_on_event(void *user, const RambleEvent *ev){
    i_RamblePatterns *pm = (i_RamblePatterns*)user;
    RambleFunction *fn;
    if (ev->kind == RAMBLE_PEER_INTEREST){
        RambleVariable *v;
        /* a match may just have formed: flush calls queued while no provider was matched */
        for (fn = pm->funcs; fn; fn = fn->next)
            if (!fn->is_provider && fn->pending) i_ramble_func_flush_queued(fn);
        /* and one may just have been severed: a directed call there can never resolve */
        for (fn = pm->funcs; fn; fn = fn->next)
            if (!fn->is_provider && fn->pending) i_ramble_func_reap_severed(fn, ev->peer);
        /* an accessor whose owner just parked or left: disarm the stale order guard, since
           a successor on the same node keeps the peer id with write_seq restarting */
        for (v = pm->vars; v; v = v->next)
            if (!v->is_owner && v->last_source
                && i_ramble_topic_source_match_count(v->value) == 0) v->last_source = 0;
        i_ramble_pat_dup_check_peer(pm, ev->peer);   /* a rival authority may have appeared */
        return;
    }
    if (ev->kind != RAMBLE_PEER_DOWN) return;
    i_ramble_pat_dup_forget_peer(pm, ev->peer);
    /* a provider dropped: a caller with no live provider left fails its calls now with
       PEER_LOST, and a call directed at the dropped peer fails regardless */
    for (fn = pm->funcs; fn; fn = fn->next){
        if (fn->is_provider || !fn->pending) continue;
        (void)i_ramble_func_reap(fn, 0, RAMBLE_CALL_PEER_LOST, 0, ev->peer);   /* directed at it */
        if (fn->pending && i_ramble_topic_live_match_count(fn->req) == 0)
            i_ramble_func_reap(fn, 0, RAMBLE_CALL_PEER_LOST, 1, 0);
    }
}
#pragma endregion
#endif /* !RAMBLE_NO_PATTERNS */
#endif /* !RAMBLE_TRANSPORT_SANS_IO */
#endif /* RAMBLE_TRANSPORT_IMPLEMENTATION */
