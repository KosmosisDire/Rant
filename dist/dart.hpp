/* DART C++ wrapper: a small, header-only OOP layer over the C single-header
 * (dart.h). Include this from C++; the raw C API is pulled into a private
 * `dart::detail` namespace so it is not visible at global scope.
 *
 * The shipped dist/dart.hpp is fully self-contained: the C single-header is
 * embedded inside it, so this one file is all a consumer needs.
 *
 * Usage: include "dart.hpp" from your C++ code for the wrapper API, and in
 * exactly ONE translation unit define DART_IMPLEMENTATION before including it
 * to emit the library implementation:
 *
 *     #define DART_IMPLEMENTATION
 *     #include "dart.hpp"          // the one implementation-anchor TU
 *
 * The DART library compiles cleanly as both C and C++, so that anchor may be a
 * .cpp (a pure C++ project needs no C toolchain) or a .c (a .c that just
 * #includes "dart.hpp" is auto-treated as the anchor). The anchor emits only
 * the implementation; use the wrapper from your other TUs.
 *
 *     auto node = dart::Node::open("robot1",
 *         [](const dart::MessageIn& m){
 *             std::printf("%.*s > %.*s\n",
 *                 (int)m.sender_name().size(), m.sender_name().data(),
 *                 (int)m.text().size(),        m.text().data());
 *         },
 *         [](const dart::Event& e){ std::fprintf(stderr, "event: %s\n", e.to_string().c_str()); },
 *         { .domain = 7 });                  // std::optional
 *     if (!node) return 1;
 *     auto ch = node->create_channel("chat", dart::Role::PubSub, nullptr,
 *                                    { .reliability = dart::Reliability::Reliable });
 *     node->start();                         // background service thread owns the loop
 *     for (;;) ch.send("hello");             // thread-safe; or skip start() and poll(1) yourself
 */
#ifndef DART_HPP_INCLUDED
#define DART_HPP_INCLUDED

/* ------------------------------------------------------------------------- *
 *  Implementation dispatch.
 *
 *  dart.h is embedded below EXACTLY ONCE (the packer splices it in for the
 *  shipped dist/dart.hpp; in-tree it resolves through the include path). Where
 *  that single copy lands is chosen at compile time:
 *
 *   - DART_IMPLEMENTATION defined (in one anchor TU, .c OR .cpp): emitted at
 *     global scope, producing the C99 implementation. The library compiles
 *     cleanly as BOTH C and C++, so the anchor may be either -- a pure C++
 *     project needs no C toolchain. dart.h's extern "C" keeps the symbols' C
 *     linkage either way, so the namespaced declarations below link to them.
 *
 *   - a normal C++ consumer TU: emitted inside `namespace dart::detail`,
 *     declarations only, so no raw C symbol reaches global scope; the OOP
 *     wrapper follows. (As with any single-header lib, the one anchor TU emits
 *     only the implementation -- put DART_IMPLEMENTATION in a dedicated TU.)
 *
 *  A .c TU with nothing defined is treated as the implementation anchor. */
#if !defined(DART_IMPLEMENTATION) && !defined(__cplusplus)
#define DART_IMPLEMENTATION
#endif

#if defined(__cplusplus) && !defined(DART_IMPLEMENTATION)

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#if defined(__has_include) && __has_include(<span>)
#include <span>
#endif

/* Pre-include the (only) C std headers dart.h's declaration side pulls, so
 * their include guards are set before the embed and no std name (size_t,
 * uint8_t, ...) gets dragged into `dart::detail`. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(DART_STRING_H) || defined(DART_TRANSPORT_H) || defined(DART_NODE_H)
#error "include dart.hpp instead of dart.h (do not include dart.h before dart.hpp)"
#endif

namespace dart {
namespace detail {

#endif  /* C++ consumer: open the hiding namespace before the embed */

/* ---- embedded C library (dist/dart.h spliced here by tools/pack.cmake) ---- */
/* GENERATED single-header build. DO NOT EDIT.
 * DART = Discovery And Realtime Transport. Amalgamated from src/ by the CMake
 * build (tools/pack.cmake). Edit the split sources in src/ and rebuild (or run
 * tools/pack.cmake) to regenerate. See the flag scheme in tools/pack.cmake.
 */
#if defined(__GNUC__)   /* let the section markers below fold quietly */
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif
#ifdef DART_IMPLEMENTATION
  #ifndef DART_DISCOVERY_IMPLEMENTATION
  #define DART_DISCOVERY_IMPLEMENTATION
  #endif
  #ifndef DART_TRANSPORT_IMPLEMENTATION
  #define DART_TRANSPORT_IMPLEMENTATION
  #endif
#endif
#ifdef DART_SANS_IO
  #ifndef DART_DISCOVERY_SANS_IO
  #define DART_DISCOVERY_SANS_IO
  #endif
  #ifndef DART_TRANSPORT_SANS_IO
  #define DART_TRANSPORT_SANS_IO
  #endif
#endif

#if defined(DART_DISCOVERY_IMPLEMENTATION) && !defined(DART_DISCOVERY_SANS_IO) && !defined(_WIN32)
  #ifndef _POSIX_C_SOURCE
  #define _POSIX_C_SOURCE 200809L
  #endif
  #ifndef _DEFAULT_SOURCE
  #define _DEFAULT_SOURCE 1
  #endif
#endif

#pragma region common/string.h
/* DartBytes and DartString: the two basic (pointer + length) views DART passes
 * bytes and strings around with. Neither owns or copies; both are valid only
 * while the memory they reference lives, and neither implies a NUL terminator.
 * Shared by discovery, transport, node, and the serializer, so it sits in common/
 * and the amalgamator emits it once near the top of every distributable. */
#ifndef DART_STRING_H
#define DART_STRING_H

#include <stddef.h>
#include <stdint.h>

/* A run of bytes: message payloads, wire blobs (meta / interest), datagrams. */
typedef struct {
    const uint8_t *data;
    size_t         len;
} DartBytes;

/* A run of string bytes that is NOT NUL-terminated: use for any string carried
 * on the wire (a topic name, the peer name inside an announce). Where a string
 * IS 0-delimited (a config input, a human label) keep a plain const char*. */
typedef struct {
    const char *data;
    size_t      len;
} DartString;

/* Constructors and a couple of helpers. static inline: no link symbol and no
 * unused-function warning; an FFI caller just fills the struct directly. */
static inline DartBytes dart_bytes(const void *data, size_t len){
    DartBytes b; b.data = (const uint8_t *)data; b.len = len; return b;
}
static inline DartString dart_string(const char *data, size_t len){
    DartString s; s.data = data; s.len = len; return s;
}
/* From a NUL-terminated C string (NULL => empty). */
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
 * A freed freeable block is POOLED, not returned to the backing: the next fitting
 * allocation reuses it. So after a long-running node reaches its high-water mark, churn
 * (peers joining/leaving, buffers regrowing) cycles through the pool and the system heap
 * sees no steady-state malloc/free at all -- it cannot fragment from us. Freeable sizes
 * are rounded to quarter-pow2 classes ({1, 1.25, 1.5, 1.75} x 2^k, waste <= 25%) so the
 * same few sizes recur and pooled blocks actually get reused.
 *
 * dart_allocator_reset frees EVERYTHING (both intents plus the pool), so no per-allocation
 * free is ever required; free a freeable block only to reclaim it mid-run. In static mode
 * there are no pages, so both intents just bump the buffer, a freed block goes to the same
 * reuse pool, and reset rewinds. All static-inline: like arena.h, the amalgamator emits it
 * once and a layer that does not use it pays nothing. */

#ifndef DART_ALLOCATOR_PAGE
#define DART_ALLOCATOR_PAGE (64u * 1024u)   /* default shared-page size (dynamic mode) */
#endif

/* Page backing (dynamic only): allocate/grow/free whole pages, ptr NULL = alloc, size 0 =
 * free. The runtime injects i_dart_plat_realloc; a test injects stdlib realloc. */
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
    i_DartPage *free_pool;      /* freed freeable blocks kept for reuse (both modes) */
    uint32_t    page_size;
    size_t      max_bytes;      /* dynamic runaway guard (0 = unlimited) */
    size_t      in_use, pooled, peak;
    uint64_t    alloc_calls, pages_live;
} DartAllocator;

static inline size_t i_dart_allocator_align(size_t n){ return (n + 15u) & ~(size_t)15u; }
/* freeable size class: the next {1, 1.25, 1.5, 1.75} x 2^k >= n, min 16. Waste is
 * bounded at 25% (pow2 wastes up to 100%) while sizes still land on a few recurring
 * classes, so the free pool gets exact-class hits. */
static inline size_t i_dart_allocator_class(size_t n){
    size_t p = 16u, q;
    if (n <= 16u) return 16u;
    while ((p << 1) <= n){ if (p > (SIZE_MAX >> 2)) return n; p <<= 1; }
    q = p >> 2;                               /* n in [p, 2p): round up to a quarter step */
    return p + ((n - p + q - 1u) / q) * q;
}
/* dynamic runaway guard: 1 if `need` more BACKING bytes would breach max_bytes (the
 * pool is held memory, so it counts; a pool reuse allocates nothing and skips this). */
static inline int i_dart_allocator_over(const DartAllocator *a, size_t need){
    return a->max_bytes && a->in_use + a->pooled + need > a->max_bytes;
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
static inline void *i_dart_allocator_bump(DartAllocator *a, size_t need){
    i_DartPage *pg = a->shared;
    need = i_dart_allocator_align(need);
    if (!pg || i_dart_allocator_align(pg->used) + need > pg->cap){
        size_t psz, floor_sz; i_DartPage *np;
        if (!a->page_realloc || i_dart_allocator_over(a, need)) return NULL;   /* static full, or over the guard */
        psz = a->page_size;
        floor_sz = need + sizeof(i_DartPage);
        if (floor_sz > psz) psz = floor_sz;
        np = (i_DartPage *)a->page_realloc(NULL, psz);
        while (!np && psz > floor_sz){
            /* a fragmented backing heap (e.g. an ESP32 after WiFi init) may have the bytes
               only in shreds: halve the page until one fits, before giving up */
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

/* a freeable block of >= `cap` payload bytes: a pooled freed block when one fits, else its
 * own page (dynamic) or a header+payload bumped from the buffer (static). */
static inline void *i_dart_allocator_new_owned(DartAllocator *a, size_t cap){
    i_DartPage *pg;
    {   /* pool first: the smallest fitting freed block, within 2x so a giant block is
           not burned on a small ask. A pool hit touches no backing memory at all. */
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
        pg->prev = pg->next = NULL;   /* not linked; reset rewinds the buffer */
    }
    pg->cap = cap; pg->used = cap;
    a->alloc_calls++; a->in_use += cap; if (a->in_use > a->peak) a->peak = a->in_use;
    return (void *)(pg + 1);
}

/* free = move to the reuse pool (both modes), never back to the backing heap: churn
 * recycles these, so a long run makes no steady-state system malloc/free. reset reclaims. */
static inline void i_dart_allocator_free_owned(DartAllocator *a, void *ptr){
    i_DartPage *pg = (i_DartPage *)ptr - 1;
    a->in_use -= pg->cap;
    if (a->page_realloc){                              /* dynamic: unlink from owned */
        if (pg->prev) pg->prev->next = pg->next; else a->owned = pg->next;
        if (pg->next) pg->next->prev = pg->prev;
    }
    pg->prev = NULL; pg->next = a->free_pool;
    if (a->free_pool) a->free_pool->prev = pg;
    a->free_pool = pg;
    a->pooled += pg->cap;
}

/* The freeable allocator function: a DartAllocFn (pass &allocator as `user`).
 *   ptr NULL -> allocate    size 0 -> free (to the reuse pool)    else -> resize. */
static inline void *dart_allocator_alloc(void *alloc, void *ptr, size_t size){
    DartAllocator *a = (DartAllocator *)alloc; size_t cap;
    if (!a) return NULL;
    if (size == 0){ if (ptr) i_dart_allocator_free_owned(a, ptr); return NULL; }
    cap = a->page_realloc ? i_dart_allocator_class(size) : i_dart_allocator_align(size);   /* classed dynamic, tight static */
    if (!ptr) return i_dart_allocator_new_owned(a, cap);
    {   i_DartPage *pg = (i_DartPage *)ptr - 1;
        if (cap <= pg->cap) return ptr;                          /* still fits: keep it */
        {   void *np = i_dart_allocator_new_owned(a, cap);                   /* grow: new + copy + free old */
            if (!np) return NULL;                                /* old left intact */
            memcpy(np, ptr, pg->cap);
            i_dart_allocator_free_owned(a, ptr);
            return np;
        }
    }
}

/* Free everything (both intents + the pool). Static: rewind the buffer (keeps it). */
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
#pragma region discovery/core.h
/* sans-IO peer-discovery core: no socket, clock, or heap. Feed it datagrams +
 * now_us; it returns datagrams to send and fires peer up/down callbacks. For an
 * IO-owning layer see discovery/runtime.h (DartDiscovery). */
#ifndef DART_DISCOVERY_H
#define DART_DISCOVERY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DART_DISCOVERY_PROTO_VERSION
#define DART_DISCOVERY_PROTO_VERSION 3     /* v3: versioned meta blob, u16 meta_len */
#endif

#define DART_DISCOVERY_META_MAX 64   /* default per-peer OVERLAY capacity (cfg.meta_capacity overrides) */
#define DART_DISCOVERY_NAME_MAX 32   /* max advertised peer-name bytes (blob's discovery section) */
/* Fixed header (magic, ver, flags, domain, uuid) is sent EVERY announce, then
 * [u32 meta_version][u16 meta_len][meta...]. The meta blob = [discovery section: locator +
 * name][opaque overlay]: the locator + name moved out of the per-announce header into the
 * on-change blob so steady-state announces stay small (cached on the other side). */
#define DART_DISCOVERY_META_OFF 30   /* HDR_LEN(24) + 4 (version) + 2 (len) */
/* smallest egress/ingress datagram buffer; the runtime grows it to fit meta_capacity */
#define DART_DISCOVERY_WIRE_MAX 128

typedef struct {
    uint8_t  ip[16];   /* network-order bytes */
    uint8_t  ip_len;   /* 4 = IPv4, 16 = IPv6 */
    uint16_t port;     /* data port, host order */
} DartDiscoveryAddr;

/* Why a peer is going down, so the IO layer can keep transport state across a
 * transient blip instead of tearing it down on every silence timeout. */
typedef enum {
    DART_DISCOVERY_DROP = 0,  /* fell silent past peer_timeout_us: same UUID may return, keep state */
    DART_DISCOVERY_GONE = 1   /* said BYE, stayed silent past the gone timeout, or its slot was
                                 reclaimed for a new peer: free state */
} DartDiscoveryDownReason;

/* Discovery's own event, delivered through one on_event. Discovery is generic: it
 * carries an opaque meta blob and knows nothing of the overlay (transport/node), so it
 * has its own event type rather than sharing one. The node translates these into its
 * app-facing DartEvent.
 *   DART_DISCOVERY_PEER_UP      reachable at .addr; .name is the advertised peer name and
 *                               .meta/.meta_len the opaque overlay (NULL if none, valid only
 *                               for the call). Re-fires on a known peer's addr/meta change and
 *                               when a DROPPED peer returns under the SAME .peer (resume).
 *   DART_DISCOVERY_PEER_DOWN    going down; .reason (DROP keep / GONE freed). .peer is a
 *                               local handle, stable across a DROP/return, freed on GONE.
 *   DART_DISCOVERY_PEER_REFUSED table full of ACTIVE peers: a new peer at .addr was
 *                               refused rather than evicting a live one. Diagnostic. */
typedef enum {
    DART_DISCOVERY_PEER_UP,
    DART_DISCOVERY_PEER_DOWN,
    DART_DISCOVERY_PEER_REFUSED,
    DART_DISCOVERY_META_TOO_BIG   /* a peer's announce blob exceeds what this side can hold or even
                                     receive, so the whole announce was dropped: .addr = its locator
                                     (or the datagram source), .peer = its id (0 if not yet in the
                                     table), .meta.len = the bytes it wanted to send (.meta.data is
                                     the oversized overlay when the datagram arrived whole, NULL when
                                     the OS truncated it to our RX buffer). An IO layer that can grow
                                     raises its capacity and re-solicits; otherwise raise capacity. */
} DartDiscoveryEventKind;

typedef struct {
    DartDiscoveryEventKind   kind;
    void                    *user;     /* DartDiscoveryCoreConfig.user */
    uint32_t                 peer;     /* local peer id (UP / DOWN) */
    DartDiscoveryAddr        addr;     /* UP / REFUSED: advertised locator */
    DartDiscoveryDownReason  reason;   /* DOWN: DROP vs GONE */
    DartString               name;     /* UP: advertised peer name (not NUL-terminated; {NULL,0} if none) */
    DartBytes                meta;     /* UP: opaque overlay blob ({NULL,0} if none) */
} DartDiscoveryEvent;
typedef void (*DartDiscoveryEventFn)(const DartDiscoveryEvent *ev);

/* A peer's liveness, for the read-only peer view (dart_discovery_peer_at). */
typedef enum {
    DART_PEER_ACTIVE  = 0,   /* heard within peer_timeout_us */
    DART_PEER_DROPPED = 1     /* fell silent; state kept, the same UUID may return */
} DartPeerLiveness;

/* The public face of one discovered peer, filled from the internal table by
 * dart_discovery_peer_at. Discovery is generic, so this carries only what discovery
 * itself knows: identity, locator, liveness, name, and the OPAQUE overlay blob (the
 * higher layer's data). The overlay's transport meaning (frag size, interest topics)
 * is decoded by the consumer via the transport codec (dart_meta_frag /
 * dart_meta_interest_next), never by discovery. The pointers are into discovery state,
 * valid until the next poll mutates the table. */
typedef struct {
    uint32_t          id;            /* local handle, stable across a drop/return */
    uint8_t           uuid[16];      /* the peer's real GUID */
    DartDiscoveryAddr addr;          /* advertised unicast locator */
    DartPeerLiveness  liveness;      /* ACTIVE, or DROPPED (silent, may return) */
    uint64_t          last_heard_us; /* timestamp of its last announce (caller derives age) */
    DartString        name;          /* advertised name (not NUL-terminated; {NULL,0} if none) */
    DartBytes         meta;          /* opaque overlay blob ({NULL,0} if none) */
    uint32_t          meta_version;  /* version of the overlay we hold */
    uint32_t          adv_meta_version; /* highest version the peer advertises (> meta_version => held blob is stale) */
    void             *user;          /* this peer's user scratch (cfg.peer_user_bytes), or NULL */
} DartDiscoveryPeer;

typedef struct {
    uint8_t  uuid[16];      /* unique per process instance (regen each boot) */
    uint16_t domain_id;     /* logical-network selector */
    uint16_t data_port;     /* unicast port we advertise */
    uint8_t  self_ip[16];   /* optional advertised IP; len 0 => use src addr */
    uint8_t  self_ip_len;   /* 0, 4, or 16 */
    uint32_t announce_interval_us;   /* re-announce interval */
    uint32_t peer_timeout_us;    /* drop peer after this much silence */
    uint32_t gone_timeout_us;    /* promote a DROPPED (silent) peer to GONE this long after it dropped:
                                    its transport state is freed and the slot reclaimed, so a long-dead
                                    peer can't hold a slot indefinitely. 0 = never promote (linger for
                                    resume until the slot is needed for a new peer). */
    uint16_t max_peers;     /* table capacity */
    DartString name;        /* advertised peer name (goes in the blob's discovery section);
                               {NULL,0} = none. Copied at init, so it need not outlive the call. */
    DartBytes meta;         /* opaque OVERLAY blob (the higher layer's data, e.g. transport
                               frag/interest); discovery carries it after its own section. The
                               INITIAL value (dart_discovery_set_meta updates it). Must stay
                               valid. .len <= meta_capacity */
    uint16_t meta_capacity;      /* per-peer OVERLAY buffer capacity; 0 => DART_DISCOVERY_META_MAX */
    uint16_t peer_user_bytes;    /* opaque scratch reserved per peer (0 = none); see dart_discovery_peer_user.
                                    Zeroed when a new UUID takes a slot, preserved across a drop -> resume. */
    DartDiscoveryEventFn on_event;   /* optional: PEER_UP / PEER_DOWN / PEER_REFUSED */
    void *user;
    /* Optional allocation hook. When set, per-peer overlay blobs are allocated on demand
     * at each blob's ACTUAL length (grown per peer as bigger blobs arrive) instead of a
     * fixed max_peers x meta_capacity arena pool -- typically a >90% cut, since real blobs
     * are far smaller than the worst-case capacity. meta_capacity stays the accept bound
     * (META_TOO_BIG above it) either way. NULL (embedded default) keeps the fixed pool.
     * Pair with dart_discovery_destroy unless the hook's owner reclaims everything itself
     * (the node's allocator reset does). */
    DartAllocFn alloc;
    void       *alloc_user;
} DartDiscoveryCoreConfig;

typedef struct DartDiscoveryState DartDiscoveryState;

/* Fill any zero (unset) timing/size field with its default: announce_interval_us
 * (1s), peer_timeout_us (3.5x the interval), gone_timeout_us (60s), max_peers (32).
 * dart_discovery_init REQUIRES announce_interval_us + peer_timeout_us non-zero (it rejects
 * a zero); gone_timeout_us may stay 0 (never promote a dropped peer). An IO layer applies
 * this once before both sizing and init so the two always agree. Idempotent. */
void         dart_discovery_config_defaults(DartDiscoveryCoreConfig *cfg);

/* Bytes an IO layer must allocate for one rx/tx datagram scratch buffer: the fixed
 * header + version + len + meta_capacity (0 => DART_DISCOVERY_META_MAX), floored at
 * DART_DISCOVERY_WIRE_MAX. The core constants that size it live here, so it owns the math. */
uint32_t     dart_discovery_wire_size(uint16_t meta_capacity);

size_t       dart_discovery_required_memory(const DartDiscoveryCoreConfig *cfg);
DartDiscoveryState *dart_discovery_init(void *mem, size_t mem_size, const DartDiscoveryCoreConfig *cfg);
/* Free the hook-allocated per-peer blobs (cfg.alloc mode only; no-op otherwise). The
 * arena stays the caller's. Skippable when the hook's owner reclaims wholesale (the
 * node resets its allocator instead). */
void         dart_discovery_destroy(DartDiscoveryState *st);
/* Relocate a live core into a bigger block at grown counts, preserving UUID, blob version,
 * local-id counter and the peer table (NOT a re-init). self_meta = the announce blob's new
 * address (the node core moved). Caller frees the old block afterward. Dynamic growth only. */
DartDiscoveryState *dart_discovery_core_migrate(DartDiscoveryState *old, void *new_mem,
        size_t new_cap, uint16_t new_max_peers, uint16_t new_meta_capacity,
        const uint8_t *self_meta, void *peer_cb_user);
void         dart_discovery_on_datagram(DartDiscoveryState *st, const uint8_t *src_ip, uint8_t src_ip_len,
                               DartBytes datagram, uint64_t now_us);
size_t       dart_discovery_update(DartDiscoveryState *st, uint64_t now_us, void *out, size_t cap);
/* The next monotonic time dart_discovery_update wants to run its timers (announce due;
 * 0 = immediately, e.g. a pending solicit or a just-advertised blob). Lets a driving
 * loop sleep exactly until due instead of ticking. Peer-timeout sweeps ride the same
 * cadence, so detection lags by at most one announce interval (well inside the
 * default peer timeout). */
uint64_t     dart_discovery_next_due_us(const DartDiscoveryState *st);
size_t       dart_discovery_leave(DartDiscoveryState *st, void *out, size_t cap);
/* Queue a one-shot solicit: the next update asks peers to announce now (sent once at startup). */
void         dart_discovery_solicit(DartDiscoveryState *st);
/* Re-fire on_peer_up for every live (non-dropped) peer with the meta blob we already
 * hold, without any version change. A caller that just changed its OWN advertised data
 * (e.g. added a local channel / changed a role) uses this to re-apply every peer's
 * interest, so the new local state matches interest the peers advertised earlier --
 * which the peer would otherwise only re-send on its own next change. */
void         dart_discovery_replay_peers(DartDiscoveryState *st);
/* Replace the opaque meta blob and bump its version, so peers re-fetch it. The
 * blob rides the next few announces, then announces carry the version only; a peer
 * that fell behind re-fetches via a targeted solicit. meta must stay valid. */
void         dart_discovery_set_meta(DartDiscoveryState *st, DartBytes meta);
/* The version our announces currently advertise (bumped by each set_meta; 0 = none
 * set). A consumer stamps data derived from the blob with it (e.g. detail responses). */
uint32_t     dart_discovery_meta_version(const DartDiscoveryState *st);
/* Set the unicast locator port advertised in announces (the header data_port). The IO
 * runtime calls this when it binds its own same-host unicast RX socket, so peers reply
 * to a port unique to THIS process instead of the shared discovery port (which the OS
 * hands to one arbitrary same-port socket). 0 = none (peers fall back to the disc port). */
void         dart_discovery_set_data_port(DartDiscoveryState *st, uint16_t port);
/* Drain one targeted (unicast) datagram and its destination: a solicit REPLY to a
 * peer that solicited us (carries the blob), or a re-fetch REQ to a peer whose
 * advertised version is ahead of what we hold. Returns bytes + fills *to, or 0 when
 * none. Loop like dart_discovery_update; the runtime unicasts each to *to. */
size_t       dart_discovery_poll_targeted(DartDiscoveryState *st, void *out, size_t cap,
                             DartDiscoveryAddr *to);
/* Count of live peers currently known. */
uint16_t     dart_discovery_peer_count(const DartDiscoveryState *st);
/* Table capacity (the slot range for dart_discovery_peer_addr / dart_discovery_peer_at). */
uint16_t     dart_discovery_max_peers(const DartDiscoveryState *st);
/* Address of the peer in table slot (0..max_peers-1); 1 + fills *out only if it holds an
 * ACTIVE peer (not a dropped/silent one). Lets a runtime reinforce announces over unicast to
 * survive multicast outages, without bouncing them off peers that have gone away. */
int          dart_discovery_peer_addr(const DartDiscoveryState *st, uint16_t slot,
                             DartDiscoveryAddr *out);
/* Read-only peer view: fill *out for the peer in table slot (0..max_peers-1) and return
 * 1 if it holds one (ACTIVE or DROPPED), else 0. The filled name/meta pointers point into
 * discovery state, valid until the next poll. Scan slot 0..dart_discovery_max_peers()-1 to
 * enumerate; the overlay is opaque (decode it with the transport codec). */
int          dart_discovery_peer_at(const DartDiscoveryState *st, uint16_t slot,
                             DartDiscoveryPeer *out);

/* By-id lookups (a higher layer keys its state on the peer id). Each scans the table
 * for the peer whose local id == id, including DROPPED peers (a dropped peer keeps its
 * slot for a same-UUID return, so it still resolves). The peer table is discovery's; a
 * consumer (e.g. the node core) uses these instead of duplicating it.
 *   peer_user  -> pointer to the peer's opaque scratch (cfg.peer_user_bytes), or NULL.
 *   addr_of_id -> 1 + fills *out with the advertised locator, else 0.
 *   peer_name  -> advertised name as a DartString (into discovery state; {NULL,0} if unknown).
 *   id_for_addr-> reverse map an (ip, port) back to a peer id: 1 + *id on a hit, else 0. */
void        *dart_discovery_peer_user(DartDiscoveryState *st, uint32_t id);
int          dart_discovery_addr_of_id(const DartDiscoveryState *st, uint32_t id,
                             DartDiscoveryAddr *out);
DartString   dart_discovery_peer_name(const DartDiscoveryState *st, uint32_t id);
int          dart_discovery_id_for_addr(const DartDiscoveryState *st, const uint8_t *ip,
                             uint8_t ip_len, uint16_t port, uint32_t *id);
/* Deterministic UUID from a stable input (e.g. serial/MAC) + boot seed. RFC 9562 v8. NOT cryptographic. */
void         dart_discovery_make_uuid(uint8_t out[16], DartBytes stable, uint64_t boot_seed);

#ifdef __cplusplus
}
#endif
#endif /* DART_DISCOVERY_H */
#pragma endregion

#ifndef DART_DISCOVERY_SANS_IO
#pragma region platform/core.h
/* dart_plat: the one platform layer. Every OS dependency the runtimes need lives
 * behind this contract: a monotonic clock, UDP sockets, multicast, entropy,
 * the source-address route probe, and threads (thread/mutex/condvar/waker,
 * auto-detected as DART_THREADS; DART_NO_THREADS opts out). The layers above
 * (discovery_rt, node) speak
 * only dart_plat_* and never touch a sockaddr, winsock, or a platform #ifdef, so
 * a new platform is one new implementation of this header. A platform that
 * already has BSD sockets needs no new code: the bundled implementation covers
 * Windows and POSIX (Linux/macOS/BSD/ESP-lwIP). Pure IO: stripped under *_SANS_IO
 * with the runtime layers. On non-MSVC Windows link -lws2_32 -lbcrypt.
 *
 * Address convention: endpoints (send/recv, peers, seeds) are a uint8_t ip[4]
 * plus a host-order uint16_t port. Multicast group and interface addresses are a
 * uint32_t in NETWORK byte order ("naddr", as from i_dart_plat_parse_ip /
 * i_dart_plat_ipv4). The two are the same four bytes; move between them with
 * i_dart_plat_ip4_to_naddr / i_dart_plat_naddr_to_ip4.
 */
#ifndef DART_PLAT_H
#define DART_PLAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SHM (the zero-fragment same-host path): DART_SHM is AUTO-DETECTED on where the
 * bundled platform layer provides it (Windows file mappings; shm_open/mmap on
 * Linux/macOS/BSD), off elsewhere (ESP-IDF and unknown targets have no shm_open),
 * and DART_NO_SHM always wins (strip it explicitly, e.g. to drop the Linux -lrt).
 * A new platform layer that implements the i_dart_plat_shm_* contract declares
 * support by defining DART_SHM itself. This block mirrors dart_transport.h
 * EXACTLY so every TU agrees whichever header it saw first. */
#if !defined(DART_SHM) && !defined(DART_NO_SHM)
  #if defined(_WIN32) || defined(__linux__) || defined(__APPLE__) || \
      defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
      defined(__DragonFly__)
    #define DART_SHM
  #endif
#endif
#if defined(DART_SHM) && defined(DART_NO_SHM)
  #undef DART_SHM              /* both set: the opt-out wins */
#endif

/* Threading (the node lock + optional service thread) follows the same flag shape:
 * DART_THREADS is AUTO-DETECTED on where the bundled platform layer provides it
 * (Windows; POSIX with pthreads: Linux/macOS/BSD/ESP-IDF, plus any other libc that
 * advertises <pthread.h>), off elsewhere, and DART_NO_THREADS always wins. A NEW
 * platform implementation that provides the thread/mutex/cond/waker contract below
 * declares support by defining DART_THREADS itself (in its build flags or before
 * this header), which turns the threaded node on with no other change. */
#if !defined(DART_THREADS) && !defined(DART_NO_THREADS)
  #if defined(_WIN32) || defined(__linux__) || defined(__APPLE__) || \
      defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
      defined(__DragonFly__) || defined(ESP_PLATFORM)
    #define DART_THREADS
  #elif defined(__has_include)
    #if __has_include(<pthread.h>)
      #define DART_THREADS   /* unknown POSIX with pthreads: the bundled layer covers it */
    #endif
  #endif
#endif
#if defined(DART_THREADS) && defined(DART_NO_THREADS)
  #undef DART_THREADS        /* both set: the opt-out wins */
#endif

/* Opaque socket handle: a POSIX fd or a Windows SOCKET, both fit in intptr_t. */
typedef intptr_t i_DartSock;
#define DART_SOCK_BAD ((i_DartSock)-1)

/* Poll-set entry; mirrors struct pollfd but platform-neutral. */
#define DART_POLLIN 0x01
typedef struct { i_DartSock fd; short events; short revents; } i_DartPollfd;

/* Process-wide net init/teardown (WSAStartup/WSACleanup; no-op elsewhere).
 * The OS refcounts matched calls per process, so a node and its discovery
 * opening/closing in turn pair safely from any thread.
 * startup returns 1 on success, 0 on failure. */
int  i_dart_plat_startup(void);
void i_dart_plat_cleanup(void);

/* Monotonic microseconds from an arbitrary epoch. */
uint64_t i_dart_plat_now_us(void);

/* CSPRNG fill; 1 on success, 0 if no entropy source (caller falls back). */
int      i_dart_plat_random(void *buf, size_t len);
/* Best-effort host identity for a UUID fallback when the CSPRNG is unavailable. */
size_t   i_dart_plat_hostname(char *buf, size_t cap);   /* returns bytes written */
uint64_t i_dart_plat_pid(void);

/* realloc-style heap hook backing a node's dynamic memory mode: ptr NULL =
 * allocate, size 0 = free (returns NULL). The single heap dependency, so the node
 * layer holds no <stdlib.h>; a target with a custom heap overrides just this. */
void    *i_dart_plat_realloc(void *ptr, size_t size);

/* --- UDP sockets --- */
i_DartSock i_dart_plat_udp_open(void);                   /* DART_SOCK_BAD on failure */
void      i_dart_plat_close(i_DartSock s);
/* Bind to if_naddr (0 = INADDR_ANY) : port (0 = OS ephemeral). reuse sets
 * SO_REUSEADDR (+ SO_REUSEPORT where it exists) before binding. 1 ok, 0 fail. */
int       i_dart_plat_bind(i_DartSock s, uint32_t if_naddr, uint16_t port, int reuse);
/* Bound port in host order (read an ephemeral bind back); 0 on failure. */
uint16_t  i_dart_plat_local_port(i_DartSock s);
void      i_dart_plat_set_nonblock(i_DartSock s);
void      i_dart_plat_set_rcvbuf(i_DartSock s, int bytes);
void      i_dart_plat_set_sndbuf(i_DartSock s, int bytes);
/* Stop a bounced datagram (ICMP port-unreachable) from failing the next recv on
 * a shared RX socket (Windows SIO_UDP_CONNRESET; no-op elsewhere). */
void      i_dart_plat_suppress_connreset(i_DartSock s);

/* --- multicast --- */
void i_dart_plat_mcast_setif(i_DartSock s, uint32_t if_naddr);
void i_dart_plat_mcast_ttl  (i_DartSock s, uint8_t ttl);
void i_dart_plat_mcast_loop (i_DartSock s, int on);
int  i_dart_plat_mcast_join (i_DartSock s, uint32_t group_naddr, uint32_t if_naddr); /* 1 ok */

/* --- datagram IO --- */
/* sendto: returns bytes sent, <0 on error (test i_dart_plat_would_block). */
int  i_dart_plat_send(i_DartSock s, const void *buf, size_t len,
                    const uint8_t ip[4], uint16_t port);
/* recvfrom: returns bytes (>0), 0 or <0 if none. src_ip/src_port out, may be NULL. */
int  i_dart_plat_recv(i_DartSock s, void *buf, size_t cap,
                    uint8_t src_ip[4], uint16_t *src_port);
int  i_dart_plat_would_block(void);
/* poll up to n fds for timeout_ms; >0 ready, 0 timeout, <0 error. */
int  i_dart_plat_poll(i_DartPollfd *fds, int n, int timeout_ms);
/* The OS's last socket error for the calling thread (WSAGetLastError on Windows,
 * errno elsewhere), for diagnostics after a failed socket call. Just reads the OS;
 * keeps no state. */
int  i_dart_plat_last_socket_error(void);

/* --- address helpers (uint32_t naddr is network byte order) --- */
uint32_t i_dart_plat_parse_ip(const char *dotted);          /* "1.2.3.4" -> naddr */
uint32_t i_dart_plat_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
uint32_t i_dart_plat_ip4_to_naddr(const uint8_t ip[4]);
void     i_dart_plat_naddr_to_ip4(uint32_t naddr, uint8_t out[4]);
/* Source address the OS would use to reach dst_naddr:port (connect + getsockname
 * on an unbound UDP socket; no packet leaves). 0 on failure. Backs interface
 * pinning and the same-host check. */
uint32_t i_dart_plat_route_src(uint32_t dst_naddr, uint16_t port);
/* Enumerate this host's usable IPv4 interface addresses (up, non-loopback) as
 * network-order naddr into out[0..max), returning the count written (0 if none, or
 * if the platform offers no enumeration). Backs the auto interface-pin fallback when
 * a route probe can't name a real LAN interface. */
int      i_dart_plat_local_ipv4s(uint32_t *out, int max);

/* --- threads (present only under DART_THREADS; see the detection above) -------
 * What the node runtime's thread safety and background service thread need: a
 * thread, a mutex, a condvar, and a waker that can interrupt i_dart_plat_poll
 * from another thread. Opaque aligned blobs keep OS headers out of this header;
 * core.c static-asserts the real types fit. Off (undetected platform or
 * DART_NO_THREADS) the node reverts to the single-threaded contract; a new
 * platform layer implements this contract and defines DART_THREADS.
 * POSIX: link -lpthread (older toolchains). */
#ifdef DART_THREADS
typedef union { void *align_p; uint64_t align_8; unsigned char b[64]; } i_DartMutex;
typedef union { void *align_p; uint64_t align_8; unsigned char b[64]; } i_DartCond;
typedef union { void *align_p; uint64_t align_8; unsigned char b[32]; } i_DartThread;

/* start runs fn(arg) on a new thread; 1 on success. join blocks until fn returns. */
int      i_dart_plat_thread_start(i_DartThread *t, void (*fn)(void *), void *arg);
void     i_dart_plat_thread_join (i_DartThread *t);
/* Nonzero id of the calling thread (compared, never dereferenced). */
uint64_t i_dart_plat_thread_id   (void);

void i_dart_plat_mutex_init   (i_DartMutex *m);
void i_dart_plat_mutex_destroy(i_DartMutex *m);
void i_dart_plat_mutex_lock   (i_DartMutex *m);
void i_dart_plat_mutex_unlock (i_DartMutex *m);

void i_dart_plat_cond_init     (i_DartCond *c);
void i_dart_plat_cond_destroy  (i_DartCond *c);
/* Wait up to timeout_us with m held; m is reacquired before returning. May wake
 * early or spuriously: callers loop on a predicate plus a monotonic deadline. */
void i_dart_plat_cond_wait     (i_DartCond *c, i_DartMutex *m, uint32_t timeout_us);
void i_dart_plat_cond_broadcast(i_DartCond *c);

/* Waker: a self-pipe whose fd sits in a normal i_dart_plat_poll set, so another
 * thread can cut a blocking wait short. A nonblocking UDP socket bound to
 * 127.0.0.1:ephemeral and connected to itself: connect filters foreign
 * datagrams, repeat signals coalesce in the socket buffer, and it is the one
 * mechanism that is pollable on both Windows and POSIX with no new poll API. */
typedef struct { i_DartSock fd; } i_DartWaker;
int  i_dart_plat_waker_open  (i_DartWaker *w);   /* 1 on success */
int  i_dart_plat_waker_signal(i_DartWaker *w);   /* 1 = the signal went out */
void i_dart_plat_waker_drain (i_DartWaker *w);
void i_dart_plat_waker_close (i_DartWaker *w);
#endif /* DART_THREADS */

/* --- shared memory (only under DART_SHM; see the detection above) -------------
 * The few primitives src/dart_shm.h needs. Absent when DART_SHM is off (undetected
 * platform or DART_NO_SHM), so a target lacking shm support builds and links
 * without them; a new platform layer implements this contract and defines
 * DART_SHM. (POSIX: shm_open may want -lrt on older glibc.) */
#ifdef DART_SHM
/* create maps a FRESH named segment of `bytes` RW (zero-filled). attach maps an
 * EXISTING one WHOLE: the reader needn't know its size, it discovers it from the OS
 * and reports it via *out_bytes (which detach then needs). *handle receives an OS
 * handle that detach needs. Return the mapped base, or NULL on failure. Names: POSIX
 * "/name" form, Windows a plain object name; dart_shm derives one from the node uuid. */
void *i_dart_plat_shm_create(const char *name, size_t bytes, void **handle);
void *i_dart_plat_shm_attach(const char *name, size_t *out_bytes, void **handle);
/* unmap; the creator passes unlink_it=1 to also remove the OS object. */
void  i_dart_plat_shm_detach(void *base, size_t bytes, void *handle, int unlink_it);
/* stable per-host id (Linux machine-id, else a hostname hash) for the same-host
 * pre-check; a successful attach is the real gate. */
void  i_dart_plat_host_uuid(uint8_t out[16]);
/* cross-process 64-bit atomic for the chunk generation stamp (acquire/release). */
uint64_t i_dart_plat_atomic_load64 (volatile uint64_t *p);
void     i_dart_plat_atomic_store64(volatile uint64_t *p, uint64_t v);
#endif /* DART_SHM */

#ifdef __cplusplus
}
#endif
#endif /* DART_PLAT_H */
#pragma endregion
#pragma region discovery/runtime.h
/* peer-discovery runtime: UDP multicast, clock, UUID, and a one-tick loop over the
 * dart_discovery core. Two ways to construct one DartDiscovery:
 *   dart_discovery_open  - defaults-first, owns its memory via a DartAllocator
 *                          (mirrors dart_node_open; what standalone observers use).
 *   dart_discovery_place - advanced: place the runtime in a caller-provided buffer
 *                          (the node embeds discovery in its own arena this way).
 * On non-MSVC Windows, link -lws2_32 -lbcrypt. */
#ifndef DART_DISCOVERY_RT_H
#define DART_DISCOVERY_RT_H


#ifdef __cplusplus
extern "C" {
#endif

#define DART_DISCOVERY_MAX_SEEDS 4

typedef struct DartDiscovery DartDiscovery;

/* -------------------------------------------------------- defaults-first open */
/* Flat, zero-means-default options for dart_discovery_open. Discovery is generic:
 * the optional meta blob is OPAQUE (a higher layer's overlay), carried verbatim. */
typedef struct {
    uint16_t              domain;               /* logical-network selector; 0 */
    const char           *discovery_group;      /* multicast group; "239.255.0.7" */
    uint16_t              discovery_port;       /* rendezvous port; 7400 */
    const char           *multicast_interface;  /* interface IP; NULL = auto (pin on multihomed) */
    uint8_t               multicast_ttl;        /* hops; 1 */
    uint16_t              max_peers;            /* table capacity; 32 */
    DartDiscoveryEventFn  on_event;             /* optional: PEER_UP / PEER_DOWN / PEER_REFUSED */
    void                 *user;                 /* passed to on_event */
    const DartDiscoveryAddr *seed_peers;        /* unicast seeds for multicast-filtered nets */
    uint16_t              n_seed_peers;
    DartBytes             meta;                 /* optional OPAQUE overlay to advertise; {NULL,0} = none */
    uint16_t              meta_capacity;        /* per-peer INCOMING overlay buffer; 0 = default */
    uint16_t              peer_user_bytes;      /* opaque scratch reserved per peer; 0 = none
                                                   (see dart_discovery_peer_user) */
} DartDiscoveryConfig;

/* Open a discovery runtime backed by `alloc` (a static or dynamic DartAllocator, copied
 * in and reset on close, so it may be a temporary). name is this instance's advertised peer
 * name (a primary arg, like dart_node_open); NULL/empty => an auto-generated "node-XXXXXXXX".
 * cfg may be NULL for all defaults. The UUID is auto-generated. Returns NULL on failure
 * (allocator too small / socket setup failed). Close with dart_discovery_close. */
DartDiscovery   *dart_discovery_open(DartAllocator *alloc, const char *name, const DartDiscoveryConfig *cfg);

/* ------------------------------------------------------------------ lifecycle */
/* One loop tick: wait up to timeout_ms for a datagram, feed RX, pump timers, send
 * what's due. Returns 1 if a datagram arrived, 0 if idle, <0 on socket error. */
int        dart_discovery_poll(DartDiscovery *d, int timeout_ms);
/* Gather membership at startup: solicit, then pump until the peer set is quiet for
 * quiet_ms or timeout_ms total. Re-solicits periodically. Returns the peer count. Blocks. */
int        dart_discovery_gather(DartDiscovery *d, int quiet_ms, int timeout_ms);
/* Optionally multicast a graceful BYE, then close the socket and free owned memory. */
void       dart_discovery_close(DartDiscovery *d, int send_bye);

/* The live peer list, by pointer (zero copy). *count gets the length; the array is
 * valid until the next dart_discovery_poll mutates the table. Iterate it to find peers
 * by name/addr/overlay; the overlay is opaque (decode with the transport codec). Each
 * entry's .user points at that peer's scratch (cfg.peer_user_bytes), writable in place. */
const DartDiscoveryPeer *dart_discovery_peers(DartDiscovery *d, uint16_t *count);

/* The underlying sans-IO core. Advanced: a higher layer (e.g. the node core) uses it for
 * the by-id lookups (dart_discovery_peer_user / addr_of_id / peer_name / id_for_addr)
 * instead of keeping a parallel peer table. Valid for the runtime's life (NULL if d is). */
DartDiscoveryState *dart_discovery_state(DartDiscovery *d);

/* ---------------------------------------------------- advanced: placement open */
/* Network addressing + the embedded core config; zero/NULL fields get defaults. Leave
 * discovery.uuid all-zero to auto-generate one. Used by dart_discovery_place when a
 * caller (e.g. the node) supplies the memory and needs the full config surface. */
typedef struct {
    DartDiscoveryCoreConfig discovery;        /* core config: ids, timing, callbacks, meta */
    const char  *group;       /* multicast group, default "239.255.0.7" */
    uint16_t     discovery_port;   /* rendezvous port, default 7400 */
    uint8_t      ttl;         /* multicast TTL, default 1 */
    const char  *multicast_interface;    /* interface IP to join/send on; NULL = auto
                                 (route probe, falling back to a real LAN interface),
                                 "127.0.0.1" = single-host */
    const DartDiscoveryAddr *seeds;  /* peers to also unicast announces to, for
                                 networks where multicast is filtered (max DART_DISCOVERY_MAX_SEEDS) */
    uint16_t     n_seeds;
} DartDiscoveryNetConfig;

size_t     dart_discovery_placement_memory(const DartDiscoveryNetConfig *cfg);
/* Place a runtime in caller memory (mem[0..mem_size)): open the socket, join the group,
 * init core state. NULL on failure. The caller owns mem (dart_discovery_close frees only
 * the socket, not mem). On NULL, dart_discovery_last_error names the failed step. */
DartDiscovery   *dart_discovery_place(void *mem, size_t mem_size, const DartDiscoveryNetConfig *cfg);

/* Why the most recent dart_discovery_open / dart_discovery_place returned NULL, so a
 * caller (or the node, translating to its own DART_ERROR event) can report the step.
 * Best-effort: a process-global with no lock, meaningful right after a NULL return. */
typedef enum {
    DART_DISCOVERY_OK = 0,
    DART_DISCOVERY_E_MEMORY,      /* the allocator/buffer was too small for the core */
    DART_DISCOVERY_E_PLATFORM,    /* platform net startup failed (WSAStartup) */
    DART_DISCOVERY_E_SOCKET,      /* udp socket open failed */
    DART_DISCOVERY_E_BIND,        /* bind to the discovery port failed (in use?) */
    DART_DISCOVERY_E_MCAST_JOIN   /* joining the multicast group failed (bad interface?) */
} DartDiscoveryPlaceError;
DartDiscoveryPlaceError dart_discovery_last_error(void);
/* The OS socket errno captured alongside the last SOCKET/BIND/MCAST_JOIN failure (0 if
 * none / not applicable). */
int        dart_discovery_last_os_error(void);
/* Relocate a placed runtime into a bigger block at grown counts, preserving the live
 * socket, UUID and peer table. self_meta = the new announce-blob address. Caller frees
 * the old block afterward. Placement (caller-owned) path only. */
DartDiscovery   *dart_discovery_migrate(DartDiscovery *old, void *new_mem, size_t new_cap,
        uint16_t new_max_peers, uint16_t new_meta_capacity, const uint8_t *self_meta, void *peer_cb_user);

/* ----------------------------------------------------------- node integration */
/* Hand the core a discovery datagram that arrived on another socket (unicast announces
 * target the peer's data port, so the data-socket owner forwards them). */
void       dart_discovery_feed(DartDiscovery *d, const uint8_t *src_ip, uint8_t src_ip_len,
                          DartBytes datagram);
/* Replace the opaque overlay carried in announces and bump its version, so peers
 * re-fetch it (e.g. after an interest change). meta must outlive the runtime. */
void       dart_discovery_advertise(DartDiscovery *d, DartBytes meta);
/* Re-apply every known peer's interest against our current local state (see
 * dart_discovery_replay_peers). Call after changing our own advertised meta so a newly
 * added local channel matches interest peers advertised before it existed. */
void       dart_discovery_replay(DartDiscovery *d);
/* This runtime's receive sockets (the multicast group fd, plus the own unicast RX fd
 * when one exists), for a caller embedding discovery in its own blocking wait. Fills
 * out[0..1] and returns the count (1 or 2). The fds are stable across a migrate. */
int        dart_discovery_pollfds(DartDiscovery *d, i_DartSock out[2]);

/* ---------------------------------------------------------------- UUID / iface */
/* Fill out[16] with a random RFC 9562 v4 UUID; 1 ok, 0 if no entropy source. */
int        dart_discovery_make_uuid4(uint8_t out[16]);

/* Resolve an advertised peer name into out[cap]: the caller's want (clamped to cap-1 and
 * DART_DISCOVERY_NAME_MAX), or an auto-generated "node-XXXXXXXX" if want is NULL/empty.
 * Returns its length. Shared by dart_discovery_open and the node so both name peers the
 * same way. */
uint8_t    dart_discovery_default_name(char *out, size_t cap, const char *want);

/* The one interface every multicast socket should pin to: route-probe group:port,
 * falling back to the default-route LAN interface (a multicast route can resolve to
 * loopback on Windows) and then to interface enumeration. INADDR_ANY (0) only if nothing
 * usable is found. Exposed so layers above pin to the same interface on multihomed hosts. */
uint32_t   dart_discovery_mcast_if_for(uint32_t group_naddr, uint16_t port);

#ifdef __cplusplus
}
#endif
#endif /* DART_DISCOVERY_RT_H */
#pragma endregion
#endif /* !DART_DISCOVERY_SANS_IO */
#pragma region transport/core.h
/* sans-IO reliable-UDP transport core: no socket, clock, or heap. Feed it
 * datagrams + now_us + a peer set; it returns datagrams to send and delivers
 * reassembled messages. RTPS-inspired, not wire-compatible. Layer DartNode.h
 * on top for a socket-owning node. */
#ifndef DART_TRANSPORT_H
#define DART_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The zero-fragment same-host shared-memory path: DART_SHM is AUTO-DETECTED on where
 * the platform layer provides it (Windows file mappings; shm_open/mmap on Linux/
 * macOS/BSD, -lrt on older glibc), off elsewhere, and DART_NO_SHM always wins (strip
 * it explicitly, e.g. for a slimmer build). A new platform layer that implements the
 * i_dart_plat_shm_* contract declares support by defining DART_SHM itself. It is used
 * only between same-host nodes that set an allocator; a node with no local SHM peer
 * creates no segment and pays nothing at runtime. This block mirrors platform/core.h
 * EXACTLY so every TU agrees whichever header it saw first. */
#if !defined(DART_SHM) && !defined(DART_NO_SHM)
  #if defined(_WIN32) || defined(__linux__) || defined(__APPLE__) || \
      defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
      defined(__DragonFly__)
    #define DART_SHM
  #endif
#endif
#if defined(DART_SHM) && defined(DART_NO_SHM)
  #undef DART_SHM              /* both set: the opt-out wins */
#endif

#ifndef DART_FRAG_PAYLOAD
#define DART_FRAG_PAYLOAD 1350u          /* default bytes of message data per fragment */
#endif
/* The UDP fragment size is set PER NODE at init (DartConfig.frag_payload) and
 * advertised via discovery, so a receiver reassembles each message at the SOURCE
 * node's size -- a writer always fragments with one size, so its seqno line stays
 * self-consistent (no per-message field on the wire). These two compile bounds
 * frame the runtime range so fixed buffers can be sized; both default to
 * DART_FRAG_PAYLOAD, i.e. no change unless you opt in. MAX sizes the datagram
 * buffers (raise it for jumbo frames / a bigger same-LAN size); MIN sizes the
 * reassembly bitmaps (lower it only if some node uses a smaller size). Every
 * node's frag_payload must lie in [MIN, MAX]. */
#ifndef DART_FRAG_PAYLOAD_MAX
#define DART_FRAG_PAYLOAD_MAX DART_FRAG_PAYLOAD
#endif
#ifndef DART_FRAG_PAYLOAD_MIN
#define DART_FRAG_PAYLOAD_MIN DART_FRAG_PAYLOAD
#endif
#define DART_DGRAM_MAX (DART_FRAG_PAYLOAD_MAX + 40u)   /* + largest header */

#ifdef DART_SHM
#define DART_SHM_DESC_BYTES 24u   /* opaque SHM descriptor on the wire; == dart_shm.h DART_SHM_DESC_WIRE */
#endif

#ifndef DART_TOPIC_NAME_MAX
#define DART_TOPIC_NAME_MAX 64u          /* max topic-name bytes on the wire */
#endif

#ifndef DART_NODE_NAME_MAX
#define DART_NODE_NAME_MAX 32u           /* max node-name bytes carried in the announce meta blob */
#endif

/* FIXED (no-allocator) mode only: sizes the static per-peer alias tables, bounding the
 * highest peer alias that can demux. Auto-raised to 2*n_channels. Dynamic mode ignores
 * it: each peer's alias map is allocated at that peer's actual advertised size. */
#ifndef DART_META_MAX_IDS
#define DART_META_MAX_IDS 256u
#endif

typedef enum { DART_BEST_EFFORT = 0, DART_RELIABLE = 1 } DartReliability;
/* DART_INACTIVE = declared but off (resources stay allocated; dart_transport_set_role flips it) */
typedef enum { DART_PUBSUB = 0, DART_PUB_ONLY = 1, DART_SUB_ONLY = 2,
               DART_INACTIVE = 3 } DartRole;

/* Every field except reliability is zero-means-default, so a reliable channel is
 * just { .reliability = DART_RELIABLE }. */
typedef struct {
    DartReliability reliability;
    uint16_t keep_last;          /* recent messages retained for late join / repair. 0 = 1 */
    uint16_t catch_up;           /* recent messages a new subscriber gets at once. 0 = future
                                    only, 1 = latest value. Keep small (bursts at startup) */
    uint32_t max_message_bytes;  /* biggest message. 0 = one fragment, or grow-to-fit with an allocator */
    uint32_t heartbeat_us;       /* reliable: idle-writer ping (repairs a lost final message). 0 = 100ms */
    uint32_t repair_delay_us;    /* reliable: reader's delay before requesting a resend. 0 = 20ms */
    uint32_t backpressure_wait_us;/* reliable: how long a send pauses for a slow reader before
                                    evicting un-acked history. 0 = none (pure KEEP_LAST) */
    uint32_t shm_max_bytes;      /* same-host SHM: pin this channel to one size class big enough for
                                    this many bytes, so same-sized traffic reuses one pre-sized
                                    segment (a larger message falls back to UDP). 0 = each message
                                    uses its own size class's segment, created on demand. */
} DartQos;

/* A channel (topic). Cross-peer identity is the name (64-bit hash); the LOCAL
 * handle for dart_transport_send / on_message is the channel's index in channels[]. */
typedef struct {
    const char *name;  /* topic name = cross-peer identity. Required, same on every node, <= DART_TOPIC_NAME_MAX */
    DartQos   qos;
    uint8_t  role;     /* DartRole; 0 = pub+sub */
} DartChannelDef;

/* dart_transport_poll_send destination: a peer id. Data is unicast point-to-point per matched reader. */

/* A complete message; channel is the local handle. Do not call back into dart_*. */
typedef void (*DartMessageFn)(void *user, uint16_t channel, uint32_t from_peer, DartBytes data);

#ifdef DART_SHM
/* SHM delivery: the transport reassembled nothing -- it hands the node the
 * DART_SHM_DESC_BYTES descriptor from an SHM-DATA submessage and the node resolves it
 * to bytes and calls the user's on_message. Returns 1 if delivered, 0 if it could not
 * resolve the chunk (recycled / unattachable) -- then the reader leaves the gap so the
 * reliability layer repairs or skips it. Internal (transport->node); the user's
 * on_message is unchanged and never sees this. */
typedef int (*i_DartShmMsgFn)(void *user, uint16_t channel, uint32_t from_peer,
                             const uint8_t *desc);
#endif

/* Transport events (optional), delivered through one on_event. The transport is
 * independent of discovery/node: it emits only its own kinds. The node maps these
 * into its app-facing DartEvent (node/core.h); a sans-IO transport user handles them
 * directly. Flat and self-describing: read only the fields named for the .kind. */
typedef enum {
    DART_TRANSPORT_MSG_LOST,        /* messages skipped: .channel, .peer, .lost_first .. +.lost_count-1 */
    DART_TRANSPORT_MSG_TOO_BIG,     /* a received message exceeded max_message_bytes (.too_big_bytes), skipped */
    DART_TRANSPORT_NAME_COLLISION,  /* a peer's name hashes to ours but differs (.identity, .channel), refused */
    DART_TRANSPORT_QOS_INCOMPATIBLE,/* a reliable subscriber refused a best-effort publisher (.channel, .peer) */
    DART_TRANSPORT_SCHEMA_MISMATCH, /* the schema_check hook refused a match (.channel, .peer) */
    DART_TRANSPORT_INTEREST_OVERFLOW,/* a peer's matched topics carry aliases we cannot map (.peer,
                                        .lost_count = entry count): their data can never demux here.
                                        Fixed mode: raise DART_META_MAX_IDS; dynamic: map alloc failed. */
    DART_TRANSPORT_META_TRUNCATED_INTEREST, /* our announce overlay overflowed: the interest list was
                                               dropped, so peers see none of our topics. Fewer/shorter names. */
    DART_TRANSPORT_META_TRUNCATED_SCHEMA    /* our announce overlay overflowed: the schema section was
                                               dropped, so peers see partial schemas. Fewer/smaller schemas. */
} DartTransportEventKind;

/* The channel name for a channel-scoped event is not carried here: read it with
 * dart_transport_channel_name(st, ev->channel). Flat and self-describing: read only
 * the fields named for the .kind. */
typedef struct {
    DartTransportEventKind kind;
    void       *user;          /* DartConfig.user */
    uint32_t   peer;           /* peer id (0 = n/a) */
    uint16_t   channel;        /* local channel handle */
    uint64_t   lost_first;     /* MSG_LOST: first skipped seqno */
    uint64_t   lost_count;     /* MSG_LOST: number of messages skipped */
    uint64_t   too_big_bytes;  /* MSG_TOO_BIG: size of the dropped message */
    uint64_t   identity;       /* NAME_COLLISION: the colliding 64-bit topic identity */
} DartTransportEvent;
typedef void (*DartTransportEventFn)(const DartTransportEvent *ev);

/* Largest message the wire can carry (65535 fragments, ~64 MB by default). */
#define DART_MESSAGE_MAX (65535u * DART_FRAG_PAYLOAD_MAX)

/* DartConfig.allocator is a DartAllocFn (common/alloc.h): set it and user channels grow to
 * fit (max_message_bytes may be 0); NULL (default, embedded) keeps fixed buffers and a
 * bigger message is refused/skipped. Pair a set allocator with dart_transport_destroy to free it. */

/* Two ways to populate the channel table:
 *   fixed/at-init : channels != NULL, n_channels = its length. Slots are defined now;
 *                   buffers come from the arena (or the allocator if one is set).
 *   reserve/lazy  : channels == NULL, n_channels = the reserved capacity, allocator set.
 *                   All slots start DART_INACTIVE; fill them later with dart_transport_channel_define
 *                   (this is how the node's runtime dart_node_create_channel works). */
typedef struct {
    const DartChannelDef *channels;     /* NULL = reserve mode (see above) */
    uint16_t              n_channels;   /* defined count, or reserved capacity in reserve mode */
    uint16_t              max_peers;
    uint16_t              frag_payload; /* UDP fragment size this node sends with; 0 =
                                           DART_FRAG_PAYLOAD. Clamped to [MIN, MAX]. */
    DartMessageFn         on_message;
#ifdef DART_SHM
    i_DartShmMsgFn         on_shm;     /* SHM-DATA delivery (descriptor); the node resolves it */
#endif
    DartTransportEventFn  on_event;   /* optional: transport events (loss/too-big/collision/qos) */
    DartAllocFn           allocator;  /* optional: set => dynamic message sizing */
    /* optional schema gate: called while a peer's interest is applied, once per would-be
     * match, with the local channel and the peer's advertised alias (peer_is_pub: their
     * entry is a publish). Return 1 to allow, 0 to refuse (no proxy either way, and the
     * transport fires SCHEMA_MISMATCH). The transport knows nothing of schema contents;
     * the node implements this over the serialize layer with the overlay it is applying. */
    int                 (*schema_check)(void *user, uint16_t channel, uint16_t peer_alias,
                                        int peer_is_pub);
    void                 *user;
} DartConfig;

typedef struct DartTransportState DartTransportState;

size_t    dart_transport_required_memory(const DartConfig *cfg);
DartTransportState *dart_transport_init(void *mem, size_t mem_size, const DartConfig *cfg);
/* Relocate a live transport into new_mem (>= dart_transport_required_memory at the grown counts),
 * re-striding its tables to new_max_peers/new_n_channels and carrying live reliability
 * state (positions, history, in-flight repair) across. Heap buffers are not in the arena,
 * so the caller frees old's arena block afterward but must NOT dart_transport_destroy old. Returns
 * the new state, or NULL on failure (old is left intact). Dynamic-mode growth only. */
DartTransportState *dart_transport_migrate(DartTransportState *old, void *new_mem, size_t new_cap,
                        uint16_t new_max_peers, uint16_t new_n_channels);
/* Free allocator-allocated buffers (dynamic channels). No-op in fixed mode; the
 * arena stays the caller's. The node calls it from close. */
void      dart_transport_destroy(DartTransportState *st);

/* 64-bit topic identity from a name (FNV-1a): matches topics across peers. */
uint64_t  dart_topic_id(const char *name);
uint64_t  dart_channel_identity(const DartChannelDef *def);   /* = dart_topic_id(def->name) */

/* Normalize a UDP fragment size: 0 -> DART_FRAG_PAYLOAD, then clamp to [MIN,MAX].
 * The rule dart_transport_init and the node's announce blob both apply (single source). */
uint16_t  dart_clamp_frag(uint16_t frag_payload);

/* A new peer matches nothing until dart_transport_apply_peer_interest feeds its interest
 * list (carried in its discovery announce). peer_frag: that peer's advertised UDP
 * fragment size (from discovery), used to reassemble its messages; 0 = DART_FRAG_PAYLOAD.
 * Clamped to [MIN, MAX]. */
void      dart_transport_peer_add   (DartTransportState *st, uint32_t peer_id, uint16_t peer_frag);
void      dart_transport_peer_remove(DartTransportState *st, uint32_t peer_id);

/* Discovery-blip lifecycle: a peer that fell silent (discovery timeout) is made
 * DORMANT instead of removed, so its reader position survives and a same-incarnation
 * return resumes losslessly. Dormant peers are dropped from flow control (the writer
 * stops heartbeating/draining them so a dead reader can't stall it; the reader stops
 * acking them), but their proxies and deliver position are preserved. dart_transport_peer_resume
 * re-includes the peer and re-reports reader positions so the writer fills any gap.
 * Both no-op for an unknown peer; the node drives them off discovery DROP/return. */
void      dart_transport_peer_dormant(DartTransportState *st, uint32_t peer_id);
void      dart_transport_peer_resume (DartTransportState *st, uint32_t peer_id);
/* Update a peer's advertised UDP fragment size (its announce blob may arrive after
 * first contact). Clamped to [MIN, MAX]; no-op for an unknown peer. */
void      dart_transport_peer_set_frag(DartTransportState *st, uint32_t peer_id, uint16_t peer_frag);

/* Interest exchange (the node carries these in discovery announces; sans-IO callers
 * disseminate them however they like). dart_transport_build_interest serializes OUR pub/sub
 * set into out ([u16 npub][u16 nsub] then [u16 alias][u8 namelen][name] entries),
 * returning bytes written or 0 if cap is too small; size out via dart_interest_max.
 * dart_transport_apply_peer_interest applies a peer's serialized set, (re)matching channels;
 * it is idempotent. Re-build + re-disseminate after dart_transport_set_role. */
size_t    dart_interest_max(uint16_t n_channels);
size_t    dart_transport_build_interest(DartTransportState *st, void *out, size_t cap);
void      dart_transport_apply_peer_interest(DartTransportState *st, uint32_t peer_id, DartBytes blob);

/* Discovery-announce meta blob (sans-IO codec). A versioned, opaque-to-discovery
 * payload wrapping this node's UDP fragment size, SHM capability + host uuid, its
 * interest list, and its published channels' schemas: the transport's OVERLAY, carried
 * opaquely inside discovery's announce blob (the node name lives in discovery's own
 * section, not here). Layout:
 *   v8: ['D','N',8, frag_lo, frag_hi,                <interest> <schemas>]
 *   v9: ['D','N',9, frag_lo, frag_hi, shm, host[16], <interest> <schemas>]
 * frag sits at [3..4] in both; v9 adds the SHM byte + host. dart_transport_meta_build writes v9 when
 * DART_SHM is compiled, v8 otherwise. The schema section follows the (self-delimiting)
 * interest list:
 *   [u16 n_map]  ( [u16 alias][u64 hash] )*          advertised alias -> schema identity
 *   [u16 n_wire] ( [u64 hash][u16 len][bytes] )*     distinct schema wires, inlined when small
 * The wire bytes are OPAQUE here (serialize/schema.h builds and parses them); the
 * transport just frames them, so it keeps no serialize dependency. */

/* Largest schema wire the overlay inlines (and reserves capacity for, per channel); a
 * bigger schema is advertised by hash alone. Define before the include to raise it. */
#ifndef DART_META_SCHEMA_INLINE_MAX
#define DART_META_SCHEMA_INLINE_MAX 512u
#endif

/* One channel's schema advertisement, registered by the node: the 64-bit identity plus a
 * view of the canonical wire bytes (valid for the channel's lifetime). hash 0 = none. */
typedef struct {
    uint64_t  hash;
    DartBytes wire;
} DartMetaSchema;

/* Bytes to reserve for the overlay: prefix + the largest interest list and schema section
 * n_channels can produce, capped to one (IP-fragmentable) UDP datagram. Sizes discovery's
 * meta_capacity. */
uint16_t  dart_meta_capacity(uint16_t n_channels);
/* Exact bytes the next dart_transport_meta_build will emit for the CURRENT channel + schema
 * state, so a growable caller sizes its buffer to actual content; dart_meta_capacity stays
 * the fixed-buffer worst case (and the accept bound for peers' overlays). */
uint16_t  dart_transport_meta_size(DartTransportState *st, const DartMetaSchema *schemas);
/* Build the overlay into out[cap] (cap >= dart_meta_capacity): the version prefix (frag_size,
 * plus shm_capable + host[16] when DART_SHM is compiled), st's interest list, then the
 * schema section. schemas is one entry per channel (index = channel index) or NULL; every
 * non-INACTIVE channel with a nonzero hash is advertised (publishers offer their layout,
 * subscribers their required subset), each distinct wire once. Returns total bytes; host
 * may be NULL when !shm_capable. The node NAME is not here: it rides discovery's own
 * section of the announce blob. */
uint16_t  dart_transport_meta_build(DartTransportState *st, uint8_t *out, uint16_t cap,
                       uint16_t frag_size, int shm_capable, const uint8_t host[16],
                       const DartMetaSchema *schemas);
/* A peer's advertised UDP fragment size from the overlay; 0 if malformed. */
uint16_t  dart_meta_frag(DartBytes meta);
/* The interest sub-blob inside the overlay; {NULL, 0} if absent. */
DartBytes dart_meta_interest(DartBytes meta);

/* One advertised topic, as decoded by dart_meta_interest_next. name points into the
 * source overlay (NOT NUL-terminated), so keep that blob alive while reading it. */
typedef struct {
    uint16_t    alias;      /* the advertiser's local channel index (opaque to us) */
    uint8_t     reliable;   /* flags bit 0: offered (pub) / requested (sub) reliability */
    uint8_t     is_pub;     /* 1 = a publish entry, 0 = a subscribe entry */
    DartString  name;       /* topic name in the source blob (not NUL-terminated) */
} DartTopic;

/* Iterator state for dart_meta_interest_next: zero-initialize, then call until it
 * returns 0. The fields are internal walk state, not for direct use. */
typedef struct {
    uint16_t pub_left;   /* publish entries still to yield */
    uint16_t sub_left;   /* subscribe entries still to yield */
    uint32_t off;        /* byte offset of the next entry within the overlay */
    uint8_t  started;    /* 0 until the first call parses the [npub][nsub] header */
} DartInterestIter;

/* Walk a peer's interest list (the publish entries, then the subscribe entries) one
 * topic at a time, so consumers stop re-implementing the [u16 npub][u16 nsub] +
 * [u16 alias][u8 flags][u8 namelen][name] format. Pass the same overlay/len each call
 * with a zeroed DartInterestIter; returns 1 and fills *out, or 0 at the end (or on a
 * malformed/truncated blob: it stops rather than reading past the end). Usage:
 *   DartInterestIter it = {0}; DartTopic t;
 *   while (dart_meta_interest_next(meta, &it, &t)) { ... } */
int       dart_meta_interest_next(DartBytes meta, DartInterestIter *it, DartTopic *out);
/* An advertised alias's schema: returns 1 and fills *hash if the alias advertises one,
 * else 0. *wire is the inlined canonical bytes (a view into the overlay, parse with
 * dart_schema_parse), or {NULL,0} when the peer advertised the hash alone (schema too
 * big to inline). hash/wire may be NULL. */
int       dart_meta_schema(DartBytes meta, uint16_t alias, uint64_t *hash, DartBytes *wire);
#ifdef DART_SHM
/* A peer's SHM capability + host uuid (v3/v5 blobs only): 1 if SHM-capable (fills
 * host[16]), else 0. */
int       dart_meta_shm(DartBytes meta, uint8_t host[16]);
#endif

/* Pairwise detail exchange (sans-IO codec for the 'uDTL' datagram family). A requester
 * asks a peer for the full details of specific advertised topics (by alias): the topic
 * NAME (collision check), the SCHEMA hash, and the schema wire where the two hashes
 * differ. The responder is STATELESS: a response is a read-only answer built from the
 * request's alias list and sent back to the request's source address, so duplicates are
 * harmless and nobody stores requests; a lost response heals by the requester re-asking.
 * The node runtime routes these on its unicast data socket next to the transport
 * datagrams; sans-IO callers run the codec over their own pipe. Layout (LE):
 *   ['u','D','T','L'][kind][fam ver=1][u16 domain][u32 meta_version][u16 n][entries]
 *   REQ  entry: [u16 alias][u64 schema_hash]     the REQUESTER's hash for its matching
 *               channel (0 = none), so the responder inlines the wire only on mismatch
 *   RESP entry: [u16 alias][u8 namelen][name][u64 schema_hash][u16 wire_len][wire]
 * meta_version: on a REQ, the responder announce version the aliases were read from; on
 * a RESP, the responder's CURRENT version (what the details bind to). A RESP holds only
 * the requested aliases the responder currently advertises, truncated at an entry
 * boundary when it cannot fit the cap: the requester re-requests what it still lacks. */
#define DART_DETAIL_REQ  1
#define DART_DETAIL_RESP 2

/* One requested topic: the peer's alias + our schema hash for it (0 = untyped/none). */
typedef struct {
    uint16_t alias;
    uint64_t schema_hash;
} DartDetailWant;

/* Header accessors, safe on any buffer: kind returns DART_DETAIL_REQ/RESP, or 0 when the
 * datagram is not a well-formed detail header (wrong magic/version/too short). */
int       dart_detail_kind(DartBytes dgram);
uint16_t  dart_detail_domain(DartBytes dgram);
uint32_t  dart_detail_meta_version(DartBytes dgram);

/* Build a DETAIL_REQ for n_wants topics. Returns bytes written, or 0 if cap is too
 * small for all of them (14 + 10 per want): batch per peer, split only if huge. */
size_t    dart_detail_req_build(uint16_t domain, uint32_t peer_meta_version,
                       const DartDetailWant *wants, uint16_t n_wants,
                       void *out, size_t cap);

/* Exact bytes a full (untruncated) response to req takes, for sizing the buffer; 0 if
 * req is malformed. Same walk as dart_transport_detail_respond, byte for byte. */
size_t    dart_transport_detail_resp_size(DartTransportState *st, const DartMetaSchema *schemas,
                       DartBytes req);
/* Answer req into out[cap]: one entry per requested alias this st currently advertises
 * (unknown/INACTIVE aliases are skipped), the schema wire inlined only where the
 * request's hash differs from ours. schemas is the same per-channel array
 * dart_transport_meta_build takes (or NULL). meta_version stamps the response (pass the
 * current announce version). Fills what fits, truncating at an entry boundary (the
 * requester re-requests the rest). Returns bytes written; 0 = malformed req or cap
 * cannot hold the header. Does NOT check the domain: that is the caller's. */
size_t    dart_transport_detail_respond(DartTransportState *st, const DartMetaSchema *schemas,
                       uint32_t meta_version, DartBytes req, void *out, size_t cap);

/* One topic's details, as decoded by dart_detail_next. name/schema_wire point into the
 * source response (NOT NUL-terminated / not owned), so keep that buffer alive. */
typedef struct {
    uint16_t   alias;       /* the responder's local channel index */
    DartString name;        /* topic name (not NUL-terminated) */
    uint64_t   schema_hash; /* the responder's schema identity (0 = untyped) */
    DartBytes  schema_wire; /* canonical wire bytes, only when the request's hash differed
                               ({NULL,0} otherwise: identical hash means identical wire) */
} DartDetail;

/* Iterator state for dart_detail_next: zero-initialize, then call until it returns 0.
 * The fields are internal walk state, not for direct use. */
typedef struct {
    uint32_t off;      /* byte offset of the next entry */
    uint16_t left;     /* entries still to yield */
    uint8_t  started;  /* 0 until the first call parses the header */
} DartDetailIter;

/* Walk a DETAIL_RESP one topic at a time. Pass the same response each call with a zeroed
 * DartDetailIter; returns 1 and fills *out, or 0 at the end (or on a malformed/truncated
 * response: it stops rather than reading past the end). */
int       dart_detail_next(DartBytes resp, DartDetailIter *it, DartDetail *out);

/* Change a channel's role at runtime (rematches peers locally; caller re-advertises
 * interest). A (re)subscribe joins like a late joiner. Returns 0 ok, <0 unknown. */
int       dart_transport_set_role(DartTransportState *st, uint16_t channel, uint8_t role);

/* Define a reserved (currently inactive) channel slot at runtime: set its name/qos/
 * role, allocate its history ring via the allocator, and rematch known peers.
 * Reserve mode only (an allocator is required). Returns 0 ok, or negative: -1 bad index/
 * name / slot already defined / no allocator, -4 out of memory. Re-advertise interest
 * after (the node bumps its discovery announce). */
int       dart_transport_channel_define(DartTransportState *st, uint16_t channel, const DartChannelDef *def);

/* The channel's topic name ({NULL,0} if undefined or out of range), for surfacing it on a
 * delivered message. Not NUL-terminated: use .data/.len. The name is a local lookup; it is
 * never on the data path. */
DartString dart_transport_channel_name(DartTransportState *st, uint16_t channel);

/* dart_transport_send / dart_transport_send_shm result: 0 ok, negative on error (returned as int). */
typedef enum {
    DART_OK             =  0,
    DART_ERR_NO_CHANNEL = -1,  /* channel index out of range */
    DART_ERR_TOO_BIG    = -2,  /* exceeds max_message_bytes or the wire fragment cap */
    DART_ERR_ROLE       = -3,  /* channel is SUB_ONLY or INACTIVE: cannot publish */
    DART_ERR_OOM        = -4,  /* dynamic allocator returned NULL */
    DART_ERR_STATE      = -5,  /* wrong state: poll while a service thread runs, start while
                                  started, or a call not allowed from inside a callback */
    DART_ERR_NOSYS      = -6   /* not compiled in (dart_node_start with DART_THREADS off) */
} DartResult;

/* Publish a message to all peers. Returns DART_OK, or a negative DartResult. */
int       dart_transport_send(DartTransportState *st, uint16_t channel, DartBytes data, uint64_t now_us);

#ifdef DART_SHM
/* Publish a message whose payload lives in an external shared-memory buffer: the
 * transport stores the sample referencing chunk (NOT copied) plus the descriptor,
 * fragments from chunk for non-SHM peers, and sends ONE SHM-DATA (the descriptor) to
 * SHM-capable peers. desc is DART_SHM_DESC_BYTES. Same return as dart_transport_send. The chunk
 * must stay valid until the sample leaves history (acked / evicted). */
int       dart_transport_send_shm(DartTransportState *st, uint16_t channel, DartBytes chunk,
                      const uint8_t *desc, uint64_t now_us);
/* Mark whether a peer can receive SHM-DATA (same host AND its segment is attached).
 * Off by default; the node sets it on attach, clears it on dormant/remove. */
void      dart_transport_peer_set_shm(DartTransportState *st, uint32_t peer_id, int is_shm);
/* 1 if every matched reader of channel is SHM-capable, so a publish may go via SHM
 * (else inline). The node checks this per message. */
int       dart_transport_writer_shm_eligible(DartTransportState *st, uint16_t channel);
/* The history slot the next publish to channel will occupy (binds chunk<->slot). */
uint16_t  dart_transport_channel_hist_head(DartTransportState *st, uint16_t channel);
#endif

/* The channel's qos as stored at init; NULL if unknown. */
const DartQos *dart_transport_channel_qos(DartTransportState *st, uint16_t channel);

/* 1 if appending here would overwrite history not yet acked by every reader. A
 * writer pumps while this is 1, then sends anyway after qos.backpressure_wait_us. */
int       dart_transport_send_would_evict(DartTransportState *st, uint16_t channel);

/* 1 if appending here would overwrite history some matched, live reader was never
 * HANDED TO THE WIRE (committed but not yet emitted by poll_send). Unlike
 * would_evict this applies to best-effort lanes too: it detects a send burst
 * outrunning the TX drain, not slow-reader flow control. The node waits on it so
 * a send cannot silently vaporize data that never left the process. On 1, the
 * evicted sample's base seqno / fragment count are written to the (NULLable) outs. */
int       dart_transport_send_would_evict_unsent(DartTransportState *st, uint16_t channel,
                                                 uint64_t *evict_base, uint32_t *evict_count);

/* 1 if every live reader has acked all messages on this reliable channel (so a
 * writer may close without truncating). Best-effort/unknown return 1. Wrapped as
 * dart_channel_drain. */
int       dart_transport_send_drained(DartTransportState *st, uint16_t channel);

/* Peers currently matched as readers (subscribers) of this channel. 0 = a publish
 * goes nowhere; a one-shot publisher can poll this before sending. */
int       dart_transport_writer_match_count(DartTransportState *st, uint16_t channel);

/* Per-peer match summary (diagnostic): how many channels we now PUBLISH to this peer
 * (it subscribes and we publish) and how many we RECEIVE from it (it publishes and we
 * subscribe). Counts unicast lanes; either out-pointer may be NULL, both 0 for an
 * unknown peer. Surfaced on DART_PEER_INTEREST so a caller can watch a connection form. */
void      dart_transport_peer_match_counts(DartTransportState *st, uint32_t peer_id,
                                 uint16_t *publish_to, uint16_t *receive_from);

/* Cumulative reliable-repair counters for a channel, summed over its peer/reader
 * proxies (writer side = this node publishing; reader side = subscribing). Always on;
 * each field is a plain bump on a path that already runs. The per-second deltas of
 * frags_resent (writer) and non-dup frags_recv (reader) are repair throughput; a flat
 * HOL snapshot (dart_transport_reader_progress) with rising nacks_sent is a wedged stream. */
typedef struct {
    /* writer side (node as publisher) */
    uint64_t nacks_recv;     /* ACKNACKs received that requested missing fragments (nbits>0) */
    uint64_t frags_resent;   /* DATA fragments retransmitted to satisfy a NACK */
    uint64_t frags_sent;     /* all DATA fragments sent (new + repair); repair fraction = resent/sent */
    /* reader side (node as subscriber) */
    uint64_t nacks_sent;     /* repair requests we emitted (ACKNACK with nbits>0) */
    uint64_t frags_recv;     /* all DATA fragments received, including duplicates */
    uint64_t frags_dup;      /* fragments received that we already held (repair overlap / waste) */
    uint64_t msgs_skipped;   /* messages given up on (sum of DART_MSG_LOST counts) */
    /* repair-arm attribution (diagnostic): each counts a 0->1 arming of the reader's
       pending-ACK, by what triggered it. arms_data = a DATA/SHM-DATA arrival re-armed
       it; arms_hb = a heartbeat did. A stall where arms_hb ticks at the heartbeat rate
       while arms_data is flat means the reader only re-asks on arrivals, not on a timer. */
    uint64_t arms_data;
    uint64_t arms_hb;
    /* RX disposition of received DATA fragments (diagnostic): every DATA fragment that
       reaches the reader is one of these. frags_recv counts ACCEPTED only (base ==
       deliver_upto), so "recv 0" while the writer floods can mean the fragments are
       landing but being rejected as old/ahead, not that they aren't arriving. */
    uint64_t frags_old;        /* base < deliver_upto: whole message already delivered/skipped */
    uint64_t frags_ahead;      /* base > deliver_upto: a future message (no out-of-order buffer) */
    uint64_t frags_malformed;  /* count==0 || frag>=count, or not subscribed */
} DartRepairStats;

/* Fill *out with the channel's cumulative repair counters (zeroed if channel is
 * out of range). Per-channel aggregate; a per-peer breakdown is a later extension. */
void      dart_transport_repair_stats(DartTransportState *st, uint16_t channel, DartRepairStats *out);

/* Writer-side: number of reader lanes on this channel with a pending repair NACK to
 * service. 0 => the writer has nothing to resend right now (idle for lack of NACKs).
 * Diagnostic for the backpressure stall (distinguishes "no NACKs" from "resends
 * dropped"); sampled by the node's in-pump probe. */
int       dart_transport_repair_pending(DartTransportState *st, uint16_t channel);

/* Head-of-line reassembly snapshot for the in-progress message from `peer` on
 * `channel` (the message at the reader's deliver_upto). Returns 1 and fills the
 * out-params if a message is mid-reassembly, else 0.
 *   base_seqno : first seqno of the in-progress message (= reader deliver_upto)
 *   have       : fragments received so far (popcount of the reassembly bitmap)
 *   total      : fragments the message needs
 * `have` rising across calls => repair is crawling forward; flat => wedged. Any
 * out-pointer may be NULL. Wrapped as dart_channel_reader_progress. */
int       dart_transport_reader_progress(DartTransportState *st, uint16_t channel, uint32_t peer,
                            uint64_t *base_seqno, uint32_t *have, uint32_t *total);

/* Feed a received datagram, tagged with the peer it came from. */
void      dart_transport_on_datagram(DartTransportState *st, uint32_t from_peer, DartBytes datagram,
                         uint64_t now_us);

/* Pull one outgoing datagram (may batch submessages for one peer). Returns 1 and
 * fills *to_peer/out/out_len, or 0 when nothing is due. Loop until 0; pass DART_DGRAM_MAX cap. */
int       dart_transport_poll_send(DartTransportState *st, uint32_t *to_peer, void *out, size_t cap,
                       size_t *out_len, uint64_t now_us);

/* Absolute us of the next internal timer (deferred ack / NACK / heartbeat), or 0 if
 * none is pending. Cap a blocking poll at this so a due timer is serviced on time
 * instead of waiting out the poll quantum or the amortized sweep. */
uint64_t  dart_transport_next_deadline_us(DartTransportState *st);

#ifdef __cplusplus
}
#endif
#endif /* DART_TRANSPORT_H */
#pragma endregion

#ifndef DART_TRANSPORT_SANS_IO
#pragma region serialize/schema.h
/* DART serialize: a standalone schema + (de)serialization layer. Both ends share the
 * schema, so the wire carries no type tags or field names; a value's meaning is its
 * position in the byte stream. Every schema is fully FIXED for now (variable-length and
 * map types are deferred), so a message has an exact size and every field a static offset:
 * reads are O(1), zero-copy, zero-allocation. Building and parsing a schema take a
 * DartAllocFn hook (common/alloc.h); pass a node's and you inherit its static/dynamic
 * memory. Depends only on common/, so it is usable on its own. */
#ifndef DART_SCHEMA_H
#define DART_SCHEMA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DART_SCHEMA_WIRE_VERSION
#define DART_SCHEMA_WIRE_VERSION 2u    /* bumped on any schema wire-format change */
#endif
#ifndef DART_SCHEMA_MAX_DEPTH
#define DART_SCHEMA_MAX_DEPTH 8u        /* struct nesting the builder accepts */
#endif

/* The type kinds; the enum value is the kind byte on the wire. All fixed: scalars are
 * little-endian with no padding, so a field's offset is the sum of the preceding sizes.
 * An array is exactly-N scalars with no hidden framing; a live count, if wanted, is just
 * another field the schema declares (variable-length strings/arrays are deferred). */
typedef enum {
    DART_U8 = 0, DART_U16 = 1, DART_U32 = 2, DART_U64 = 3,
    DART_I8 = 4, DART_I16 = 5, DART_I32 = 6, DART_I64 = 7,
    DART_F32 = 8, DART_F64 = 9, DART_BOOL = 10,   /* 0..10: fixed scalars */
    DART_ARR    = 11,   /* fixed array of a scalar (uuid u8[16], cov f64[36]): [u8 elem][u16 count] */
    DART_STRUCT = 12    /* [u8 nfields] ( [u8 namelen][name][type] )*                               */
} DartSchemaTypeKind;

/* Bytes of a fixed scalar kind (U8..BOOL); 0 otherwise. */
uint32_t dart_schema_scalar_size(DartSchemaTypeKind kind);

/* The compiled schema (hot-path form): wire bytes, identity hash, message size, and a
 * precomputed offset per top-level field. One block allocated via the caller's hook; free
 * it with dart_schema_free. */
typedef struct DartSchema DartSchema;

/* Read-only view of one field, filled by dart_schema_field_at. The compiled schema
 * flattens EVERY field at every depth into one depth-first table (a struct's members
 * directly follow it, one level deeper), so reflection walks nested structures without
 * recursion and offsets are always message-absolute. */
typedef struct {
    DartString name;      /* field's own name (into the schema's wire bytes) */
    uint8_t    kind;      /* DartSchemaTypeKind */
    uint8_t    elem;      /* ARR element kind, else 0 */
    uint16_t   count;     /* ARR element count, else 0 */
    uint16_t   depth;     /* 0 = top level; n = member of the struct n levels up */
    uint32_t   offset;    /* absolute byte offset of this field in a message */
    uint32_t   size;      /* byte size of this field */
} DartSchemaFieldInfo;

/* Builder: define a schema and get back a compiled DartSchema. It grows its wire buffer
 * through the alloc hook as you add fields; finish resizes that block to hold the compiled
 * schema and returns it (free with dart_schema_free, same hook). A field's index is its
 * creation order (its id on read). On OOM or misuse the builder latches an error and finish
 * returns NULL (freeing anything it allocated). Always finish a builder you began.
 *
 *   DartSchemaBuilder b = dart_schema_begin(alloc, user, "Pose");
 *   dart_schema_field(&b, "x", DART_F64);
 *   dart_schema_field(&b, "y", DART_F64);
 *   dart_schema_field_array(&b, "id", DART_U8, 16);
 *   DartSchema *pose = dart_schema_finish(&b);
 */
typedef struct {
    DartAllocFn alloc; void *user;                 /* the memory hook and its context */
    uint8_t *buf;
    size_t   cap;
    size_t   len;                              /* wire bytes written so far */
    int      err;                              /* 0 ok; nonzero latches failure */
    uint16_t depth;                            /* open structs (1 = root only) */
    size_t   count_pos[DART_SCHEMA_MAX_DEPTH]; /* wire offset of each open struct's nfields byte */
    uint16_t field_count[DART_SCHEMA_MAX_DEPTH];
} DartSchemaBuilder;

/* Compile a schema from its text form: one string, pasted verbatim on the writer and
 * every reader (a reader may declare just the subset of fields it uses).
 *
 *   "Pose\n"
 *   "{\n"
 *   "    stamp:    u64,\n"
 *   "    x:        f64,\n"
 *   "    y:        f64,\n"
 *   "    uuid:     u8[16],                  -- fixed array: exactly 16 bytes\n"
 *   "    tags:     u8[8],\n"
 *   "    tagCount: u8,                      -- a live count is just another field\n"
 *   "    velocity: { dx: f32, dy: f32 }     -- nested struct\n"
 *   "}"
 *
 * Scalars: u8 u16 u32 u64 i8 i16 i32 i64 f32 f64 bool. `name: elem[N]` is a fixed
 * array of exactly N scalars; `name: { ... }` nests a struct. Commas between fields
 * are optional (fields self-delimit), whitespace is free, `--` comments run to end of
 * line (so C string literals using comments need their `\n`s, as above). Types are
 * structural: there are no named type references, because a peer's schema can only be
 * trusted by its shape (variable-length `string` is deferred).
 * A field's index is its order in the text. Identical text compiles to identical wire
 * bytes, hence the same hash on both ends. Free with dart_schema_free. On any error
 * returns NULL and points *err (optional, may be NULL) at the offending character. */
DartSchema *dart_schema_compile(DartAllocFn alloc, void *user, const char *text, const char **err);

DartSchemaBuilder dart_schema_begin(DartAllocFn alloc, void *user, const char *root_name);
/* A fixed scalar field (U8..BOOL). */
void        dart_schema_field(DartSchemaBuilder *b, const char *name, DartSchemaTypeKind kind);
/* Fixed array of exactly `count` scalar elements. */
void        dart_schema_field_array(DartSchemaBuilder *b, const char *name,
                                    DartSchemaTypeKind elem_scalar, uint16_t count);
/* Nested struct field: open it, add its fields, close it. */
void        dart_schema_begin_struct(DartSchemaBuilder *b, const char *name);
void        dart_schema_end_struct(DartSchemaBuilder *b);
/* Close the root struct, compile, and return the schema (NULL on any latched error). */
DartSchema *dart_schema_finish(DartSchemaBuilder *b);

/* Deserialize a received schema into a compiled one, allocated via the hook. wire may be
 * malformed, so this bounds-checks and returns NULL on overrun or version mismatch. The
 * bytes are copied in, so wire need not outlive the call. Free with dart_schema_free. */
DartSchema *dart_schema_parse(const void *wire, size_t wire_len, DartAllocFn alloc, void *user);
/* Free a schema from dart_schema_finish / dart_schema_parse (same hook it was made with). */
void        dart_schema_free(DartSchema *s, DartAllocFn alloc, void *user);

DartBytes   dart_schema_wire(const DartSchema *s);        /* canonical bytes (advertise these) */
uint64_t    dart_schema_hash(const DartSchema *s);        /* 64-bit identity (FNV-1a over wire) */
DartString  dart_schema_name(const DartSchema *s);        /* root type name */
uint32_t    dart_schema_size(const DartSchema *s);        /* exact message size in bytes */
/* Fields in the flattened depth-first table (EVERY depth; a field's index is its order
 * of appearance in the schema text, nested members included). */
uint16_t    dart_schema_field_count(const DartSchema *s);
int         dart_schema_field_at(const DartSchema *s, uint16_t i, DartSchemaFieldInfo *out); /* 1 + fills out, else 0 */
/* Resolve a field by name; nested members by dotted path ("velocity.dx"). Returns the
 * flat index, or -1. Fields are addressed by NAME everywhere (the getters/setters take
 * the same paths); resolve once and use dart_get/set_value if a hot path measures it. */
int         dart_schema_field_index(const DartSchema *s, const char *path);

int       dart_schema_validate(const DartSchema *s, DartBytes msg); /* 1 if msg.len == schema size */

/* Reader/writer structural compatibility: 1 if a reader declaring `sub` can read
 * messages written with `pub`. Same root name, and every sub field must exist in pub
 * under the same name with the same type (a nested struct field must match exactly).
 * The subset applies at the top level: field order and extra pub fields are free. */
int dart_schema_subset(const DartSchema *sub, const DartSchema *pub);
/* The reader's view of a publisher's layout: sub's fields (names, order, indices) with
 * pub's offsets, and pub's message size (so dart_schema_validate matches the
 * publisher's messages). Requires dart_schema_subset(sub, pub); NULL otherwise or on
 * OOM. Free with dart_schema_free. */
DartSchema *dart_schema_rebase(const DartSchema *sub, const DartSchema *pub,
                               DartAllocFn alloc, void *user);

/* Scalar getters: read a field BY NAME (nested members by dotted path: "velocity.dx"),
 * widened. Pick the family matching its kind; a mismatch or unknown field yields 0.
 * Read in place, no copy. */
uint64_t  dart_get_uint(DartBytes msg, const DartSchema *s, const char *field);  /* U8..U64, BOOL */
int64_t   dart_get_int (DartBytes msg, const DartSchema *s, const char *field);  /* I8..I64        */
double    dart_get_f64 (DartBytes msg, const DartSchema *s, const char *field);  /* F64 (or F32)   */
float     dart_get_f32 (DartBytes msg, const DartSchema *s, const char *field);  /* F32            */
/* ARR: a zero-copy view of the whole array (count * element size bytes); {NULL,0} on
 * mismatch. Element kind/count via dart_schema_field_at. */
DartBytes dart_get_array(DartBytes msg, const DartSchema *s, const char *field);

/* The canonical default message: every field is zero (numeric 0, false, zeroed arrays
 * and structs). Writes exactly dart_schema_size bytes into buf; 1, or 0 if cap is too
 * small. Start a message from this, then set the fields you care about. */
int dart_schema_message_default(const DartSchema *s, void *buf, size_t cap);

/* Setters (the writer mirror of the getters): write one field BY NAME (dotted paths for
 * nested members) of a message being built in buf[0..cap); cap must cover the field, so
 * a buffer of dart_schema_size bytes always works. Values narrow like a C cast. Zero the
 * buffer first (dart_schema_message_default) unless you set every field (dart_set_array
 * zero-fills its own tail), so the bytes are canonical. Returns 1; 0 on a kind mismatch,
 * unknown field, or short buffer. */
int dart_set_uint(void *buf, size_t cap, const DartSchema *s, const char *field, uint64_t v); /* U8..U64, BOOL */
int dart_set_int (void *buf, size_t cap, const DartSchema *s, const char *field, int64_t v);  /* I8..I64        */
int dart_set_f64 (void *buf, size_t cap, const DartSchema *s, const char *field, double v);   /* F64 (or F32)   */
int dart_set_f32 (void *buf, size_t cap, const DartSchema *s, const char *field, float v);    /* F32            */
/* ARR: copy elems over the front of the array and zero the rest. elems.len is bytes, must
 * be a multiple of the element size and fit the field (never silently truncated). */
int dart_set_array(void *buf, size_t cap, const DartSchema *s, const char *field, DartBytes elems);

/* Reflection access by flat index (tools walking a schema they've never seen: the
 * explorer, loggers, bridges). One tagged value covers every kind; the typed name-based
 * getters/setters above stay the API for code that knows its fields. */
typedef struct {
    uint8_t  kind;        /* DartSchemaTypeKind */
    uint8_t  elem;        /* ARR element kind */
    uint16_t count;       /* ARR element count */
    union { uint64_t u; int64_t i; double f; } v;   /* scalar value (BOOL in u as 0/1) */
    DartBytes bytes;      /* ARR/STRUCT raw bytes (get: view into msg; set: source, may be
                             shorter than the field: the rest is zeroed) */
} DartValue;
int dart_get_value(DartBytes msg, const DartSchema *s, uint16_t field, DartValue *out);
int dart_set_value(void *buf, size_t cap, const DartSchema *s, uint16_t field, const DartValue *val);

#ifdef __cplusplus
}
#endif
#endif /* DART_SCHEMA_H */
#pragma endregion
#pragma region node/core.h
/* sans-IO NODE core: the peer table (peer id <-> physical address), the
 * discovery->transport lifecycle (add / resume / dormant / remove + interest +
 * frag + out-of-band capability, plus PEER_UP/DOWN/REFUSED events), and address
 * resolution. No socket, clock, or platform: a node RUNTIME owns IO and drives
 * this, so a second transport (serial, Bluetooth, ...) is a new runtime over this
 * same core with no sans-IO change. See node/runtime.h for the IO layer and the
 * public dart_node_* API.
 *
 * The one transport-specific thing the core does NOT own is mapping the abstract
 * destination to wire bytes: it resolves a peer id to a physical address record and
 * hands that to the runtime, which sends it however its link works (data is unicast). */
#ifndef DART_NODE_CORE_H
#define DART_NODE_CORE_H


#ifdef __cplusplus
extern "C" {
#endif

/* The node's app-facing event: what the user receives via DartNodeOpts.on_event. Four
 * lifecycle/info kinds plus ONE catch-all DART_ERROR: everything that went wrong (a
 * refused match, an overflow, a socket/setup failure) arrives as DART_ERROR, and
 * ev->error (a DartErrorKind) says which. So "handle every error the same" is just
 * `case DART_ERROR:` (print dart_event_str), and `switch (ev->error)` drills in when you
 * care. The node maps discovery's DartDiscoveryEvent and the transport's
 * DartTransportEvent into this one type. Flat and self-describing: read only the fields
 * named for the .kind / .error. dart_event_str formats any of them as a one-line message. */
typedef enum {
    DART_PEER_UP,        /* peer discovered or resumed: .peer, .ip/.ip_len/.port */
    DART_PEER_DOWN,      /* peer lost or fell silent: .peer */
    DART_PEER_INTEREST,  /* a peer's interest list was (re)applied: .peer, .publish_topics, .receive_topics */
    DART_MSG_LOST,       /* messages skipped (best-effort loss / unrecoverable gap): .channel, .peer,
                            .lost_first .. +.lost_count-1. Not an error: expected under best-effort. */
    DART_ERROR           /* something went wrong: read .error (a DartErrorKind) and dart_event_str */
} DartEventKind;

/* The specific error carried by a DART_ERROR event (and returned by dart_last_error).
 * It IS the error code: switch on it, or feed the whole event to dart_event_str for text.
 * Named DART_E_* to stay distinct from the DartResult return codes (DART_ERR_*). */
typedef enum {
    DART_E_NONE = 0,
    /* ---- match / config (a match was refused, or advertised data cannot flow) ---- */
    DART_E_NAME_COLLISION,   /* a peer's topic name hashes to ours but differs (.identity, .channel,
                                .channel_name): the match is refused, never silently cross-wired */
    DART_E_QOS_INCOMPATIBLE, /* a reliable subscriber refused a best-effort publisher (.channel, .peer,
                                .channel_name): no silent downgrade; forms if the publisher upgrades */
    DART_E_SCHEMA_MISMATCH,  /* incompatible schemas: a match was refused, or a message that did not fit
                                its sender's schema was dropped (.channel, .peer, .channel_name) */
    DART_E_INTEREST_OVERFLOW,/* a peer's matched topics exceed our alias table (.peer, .lost_count =
                                entries): their data cannot deliver here. Raise DART_META_MAX_IDS. */
    DART_E_META_TRUNCATED_INTEREST, /* our announce overlay overflowed: the interest list was dropped,
                                       so peers see none of our topics. Fewer / shorter topic names. */
    DART_E_META_TRUNCATED_SCHEMA,   /* our announce overlay overflowed: the schema section was dropped,
                                       so peers see partial schemas. Fewer / smaller schemas. */
    DART_E_PEER_META_TOO_BIG,/* a peer's announce blob exceeds our per-peer buffer (.peer 0 if not yet
                                admitted, .too_big_bytes, .ip/.port): its metadata is refused entirely */
    DART_E_MSG_TOO_BIG,      /* a received message exceeded max_message_bytes (.too_big_bytes), skipped */
    DART_E_PEER_REFUSED,     /* peer table full of active peers: a new peer was refused (.ip/.ip_len/.port).
                                Raise discovery.max_peers (dynamic mode grows automatically). */
    DART_E_EVICTED_UNSENT,   /* a send overwrote history never handed to the wire for some matched reader,
                                after the bounded wait (.channel, .lost_first = evicted base seqno,
                                .lost_count = fragment count): the send burst outran the TX drain. */
    /* ---- low-level IO / setup (mostly at dart_node_open; .os_error carries errno) ---- */
    DART_E_OOM,              /* allocator returned NULL / static buffer too small (.too_big_bytes = bytes needed) */
    DART_E_PLATFORM,         /* platform net init failed (WSAStartup) */
    DART_E_SOCKET,           /* opening a UDP socket failed (.os_error) */
    DART_E_BIND,             /* bind failed, port in use? (.port, .os_error) */
    DART_E_MCAST_JOIN,       /* joining the discovery multicast group failed, bad interface? (.os_error) */
    DART_E_SEND,             /* a datagram send hard-failed (.peer, .os_error); reliable data is repaired */
    DART_E_RECV,             /* a socket receive hard-failed (.os_error) */
    DART_E_POLL,             /* the socket poll/wait failed (.os_error) */
    DART_E_WAKER             /* the cross-thread wake loopback is unavailable; a send wakes a blocked poll
                                only at the next timer tick (still works, just less snappy) */
} DartErrorKind;

typedef struct {
    DartEventKind kind;
    DartErrorKind error;       /* DART_ERROR: which error (DART_E_NONE otherwise) */
    const char *channel_name;  /* channel-scoped events: our channel's name (a view into node state,
                                  valid for the callback; NULL when not channel-scoped) */
    void       *user;          /* your DartNodeOpts.user_data (mirrors DartMsg.user) */
    uint32_t   peer;           /* peer id, where applicable (0 = n/a) */
    uint16_t   channel;        /* local channel handle, where applicable */
    int        os_error;       /* SOCKET/BIND/MCAST_JOIN/SEND/RECV/POLL: OS errno / WSAGetLastError (0 = n/a) */
    uint8_t    ip[16];         /* PEER_UP / PEER_REFUSED / PEER_META_TOO_BIG: peer address (network order) */
    uint8_t    ip_len;         /* 4 or 16; else 0 */
    uint16_t   port;           /* peer data port, where applicable */
    uint64_t   lost_first;     /* MSG_LOST / EVICTED_UNSENT: first skipped/evicted seqno */
    uint64_t   lost_count;     /* MSG_LOST / EVICTED_UNSENT: count; INTEREST_OVERFLOW: entry count */
    uint64_t   too_big_bytes;  /* MSG_TOO_BIG / PEER_META_TOO_BIG: size; OOM: bytes needed */
    uint64_t   identity;       /* NAME_COLLISION: the colliding 64-bit topic identity */
    uint16_t   publish_topics; /* PEER_INTEREST: topics we now publish to this peer */
    uint16_t   receive_topics; /* PEER_INTEREST: topics we now receive from this peer */
} DartEvent;
typedef void (*DartEventFn)(const DartEvent *ev);

/* Format ev as a one-line human-readable message into buf (always NUL-terminated,
 * truncated to cap). Returns buf. Covers every kind incl. DART_ERROR (per .error).
 * Under DART_NO_DIAG the descriptive text is compiled out for size and this yields a
 * terse "error N" for DART_ERROR (the numeric fields still print). */
const char *dart_event_str(const DartEvent *ev, char *buf, size_t cap);

/* Everything the core needs from the runtime, set once at init. The peer table itself
 * lives in the discovery core: the node core delegates id<->address resolution and peer
 * naming to it (dart_discovery_*), and stores its small per-peer transport-lifecycle
 * state in the discovery peer's user scratch (i_dart_node_core_peer_user_bytes). */
typedef struct {
    DartTransportState           *transport;    /* the peers are wired into this (sans-IO) */
    DartDiscoveryState  *discovery;    /* the peer table (id<->addr, name, user scratch); may be
                                          NULL at init, then bound via i_dart_node_core_bind_discovery */
    uint16_t              n_channels;    /* sizes the announce-blob buffer */
    uint16_t              frag_size;     /* our UDP fragment size, baked into the overlay */
    DartEventFn         on_event;      /* PEER_UP/DOWN/REFUSED sink (optional) */
    void                 *user;          /* passed to on_event */
    DartAllocFn           alloc;         /* optional: backs interned/rebased peer schemas (no
                                            hook = the schema gate refuses typed matches) */
    void                 *alloc_user;
    int                   oob_capable;   /* 1 = we can deliver out-of-band (SHM) payloads */
    uint8_t               oob_host[16];  /* our host id; a peer is OOB-reachable iff it matches */
} i_DartNodeCoreConfig;

typedef struct i_DartNodeCore i_DartNodeCore;

/* dynamic_meta = an alloc hook will be set: the announce blob is then hook-allocated at
 * actual size, so no arena reservation for it (must match the init cfg's alloc). */
size_t          i_dart_node_core_required_memory(uint16_t n_channels, int dynamic_meta);
i_DartNodeCore *i_dart_node_core_init(void *mem, size_t mem_size, const i_DartNodeCoreConfig *cfg);
/* Relocate the sans-IO core into a bigger block at grown counts. The transport, discovery,
 * and announce-blob pointers are re-pointed by the caller after those move. Dynamic growth. */
i_DartNodeCore *i_dart_node_core_migrate(i_DartNodeCore *old, void *new_mem, size_t new_cap,
                                       uint16_t new_n_channels);
/* Bind (or rebind, after a migrate) the discovery core whose peer table this core delegates
 * to. The runtime calls it once discovery exists, and again after discovery relocates. */
void            i_dart_node_core_bind_discovery(i_DartNodeCore *c, DartDiscoveryState *discovery);
/* Bytes of per-peer scratch the core needs in the discovery peer table (its transport-
 * lifecycle state). The runtime sets discovery's cfg.peer_user_bytes to this. */
uint16_t        i_dart_node_core_peer_user_bytes(void);

/* The discovery announce blob this node sends: its frag size, OOB host, interest
 * list, and published schemas. The core owns the buffer and builds it (the codec is
 * dart_meta_* in the transport core). build_meta (re)builds it from the core's current
 * fields and returns the length. meta returns the bytes (a view of the core's buffer)
 * for the runtime to feed to discovery. Rebuild after a role change, then re-feed
 * discovery. */
uint16_t        i_dart_node_core_build_meta(i_DartNodeCore *c);
DartBytes       i_dart_node_core_meta(i_DartNodeCore *c);
/* Register (or clear: NULL) a channel's schema: advertised in the overlay, matched by
 * the schema gate, and the base of the reader's bound view. schema must outlive the
 * channel (it is the node-owned parsed copy). Rebuild the meta after; only
 * non-INACTIVE channels are advertised, so a role flip just rebuilds. */
void            i_dart_node_core_set_channel_schema(i_DartNodeCore *c, uint16_t channel,
                                                    const DartSchema *schema);

/* Answer a peer's DETAIL_REQ ('uDTL', see the detail codec in transport/core.h): validate
 * kind + domain, build the response in the core's own grown buffer, and return it for the
 * runtime to send to the request's SOURCE address ({NULL,0} = not answerable: malformed,
 * wrong domain, or OOM; the requester re-asks). Stateless and idempotent: nothing is
 * recorded, so duplicate or crossing requests are harmless. The returned view is valid
 * until the next call. */
DartBytes i_dart_node_core_detail_respond(i_DartNodeCore *c, uint16_t domain, DartBytes req);

/* The transport's DartConfig.schema_check, node-style (see transport/core.h): decide a
 * would-be match against the overlay currently being applied (peer_up stashes it).
 * Typed vs typed matches iff same root name and the reader's fields are a subset of the
 * writer's (dart_schema_subset); a typed reader refuses an untyped or unverifiable
 * writer; an untyped (generic) reader accepts anything. On an allowed read-side match
 * this also interns the peer's schema and records the reader view for delivery. */
int i_dart_node_core_schema_check(i_DartNodeCore *c, uint16_t channel, uint16_t alias,
                                  int peer_is_pub);

/* The schema to decode a delivered message with: the channel's own schema when the
 * sender's is identical, a rebased view of the sender's layout when it is a superset,
 * the sender's interned schema for a generic (schema-less) channel, or NULL (raw). */
const DartSchema *i_dart_node_core_msg_schema(i_DartNodeCore *c, uint32_t peer, uint16_t channel);

/* Discovery event sink: register as the discovery core's on_event (cfg.user = this
 * core). Demuxes the generic DartDiscoveryEvent (PEER_UP/DOWN/REFUSED), keeps the peer
 * table and transport peer set in lockstep (dormant/resume/evict), and fires the app's
 * PEER_UP/DOWN/INTEREST/REFUSED DartEvents (the node maps discovery's events to its own). */
void i_dart_node_core_on_disc_event(const DartDiscoveryEvent *ev);

/* A resolved outbound destination: a unicast peer by physical address. The runtime
 * turns this into wire bytes for its link. */
typedef struct {
    uint8_t  ip[16];     /* peer physical address (IPv4 today) */
    uint8_t  ip_len;
    uint16_t port;       /* peer data port */
} i_DartNodeDest;

/* Destination resolution: the node-core/runtime boundary. resolve turns the transport's
 * abstract destination (dart_transport_poll_send's to_peer = a peer id) into a peer address, so the
 * runtime only maps the result to wire bytes. Returns 1 if sendable, 0 if the peer is
 * unknown. id_for_addr maps an inbound source address back to a peer id (1 + *id on a
 * hit, else 0). */
int  i_dart_node_core_resolve(i_DartNodeCore *c, uint32_t to, i_DartNodeDest *out);
int  i_dart_node_core_id_for_addr(i_DartNodeCore *c, const uint8_t ip[4], uint16_t port, uint32_t *id);

/* A peer's human-readable name, learned from its announce blob: a DartString viewing the
 * peer-table slot (not NUL-terminated; stable until the peer is evicted). Non-empty for any
 * known peer ("unknown-peer" if its announce carried none); .data is NULL only when id is
 * not a known peer. For debug/observability only. */
DartString i_dart_node_core_peer_name(i_DartNodeCore *c, uint32_t id);

/* Read-only peer-table enumeration (diagnostics / tests). max_peers is the capacity;
 * peer_at fills the out-params for table slot in [0, max_peers) and returns 1 if it
 * holds a live peer, else 0. Any out-pointer may be NULL. */
uint16_t i_dart_node_core_max_peers(i_DartNodeCore *c);
int      i_dart_node_core_peer_at(i_DartNodeCore *c, uint16_t slot, uint32_t *id,
                                uint8_t ip[16], uint8_t *ip_len, uint16_t *port);

/* Decode helpers for a peer's announce overlay (the transport meta blob discovery carries
 * opaquely). The node owns the transport codec, so a diagnostics caller reads a peer's
 * fragment size + interest off a DartDiscoveryPeer (from dart_node_peers) without ever
 * touching dart_meta_*. Both read the peer's raw overlay pointer, valid until the next poll. */
uint16_t dart_node_peer_frag(const DartDiscoveryPeer *peer);   /* advertised UDP fragment size; 0 if none/malformed */
/* Walk a peer's interest list one topic at a time (publishes, then subscribes): zero a
 * DartInterestIter, then call until it returns 0. Fills *out (out->name points into the
 * peer's overlay, NOT NUL-terminated). 0 when the peer carries no overlay or at the end. */
int      dart_node_peer_interest_next(const DartDiscoveryPeer *peer,
                              DartInterestIter *it, DartTopic *out);
/* A peer's advertised schema for one of its publish topics (alias = the DartTopic.alias
 * from the interest walk): 1 + fills *hash if the topic advertises one, else 0. *wire is
 * the schema's canonical bytes when the peer inlined them (a view into the overlay,
 * decode with dart_schema_parse), or {NULL,0} when only the hash was advertised. */
int      dart_node_peer_schema(const DartDiscoveryPeer *peer, uint16_t alias,
                              uint64_t *hash, DartBytes *wire);

#ifdef __cplusplus
}
#endif
#endif /* DART_NODE_CORE_H */
#pragma endregion
#pragma region node/runtime.h
/* NODE runtime (the public dart_node_* / dart_channel_* API): owns the data sockets,
 * drives discovery and the clock, and wires peers into the transport via the sans-IO
 * node core (node/core.h). Channels are created at runtime and handed back as opaque
 * handles; a second transport would be a new runtime over that same core. */
#ifndef DART_NODE_H
#define DART_NODE_H


#ifdef __cplusplus
extern "C" {
#endif

/* Network addressing and sockets; every field is zero-means-default (defaults shown). */
typedef struct {
    uint16_t              data_port;         /* unicast data port; 0 = OS-assigned */
    const char           *discovery_group;   /* "239.255.0.7" */
    uint16_t              discovery_port;    /* 7400 */
    const char           *multicast_interface;/* interface IP for discovery multicast; NULL = auto,
                                                "127.0.0.1" = single-host. Pin on multihomed hosts */
    uint8_t               multicast_ttl;     /* hops discovery announces may travel; 1 */
    const DartDiscoveryAddr *seed_peers;   /* peers to also unicast announces to (port 0 =
                                                discovery_port), so discovery works without multicast */
    uint16_t              n_seed_peers;
    uint32_t              recv_buffer_bytes; /* data-socket SO_RCVBUF; 0 = OS default */
    uint32_t              send_buffer_bytes; /* data-socket SO_SNDBUF; 0 = OS default */
    uint16_t              fragment_size;     /* UDP payload bytes per fragment this node sends;
                                                0 = DART_FRAG_PAYLOAD. Advertised via discovery so
                                                peers reassemble at our size. Clamp [MIN, MAX]; raise
                                                MAX (compile) for jumbo frames. One size per node. */
} DartNodeNet;

/* Discovery cadence and peer-table size; zero-means-default (defaults shown). */
typedef struct {
    uint32_t              announce_interval_us; /* "I'm here" broadcast period; 1s */
    uint32_t              peer_timeout_us;   /* drop a peer after this silence; 3.5s */
    uint16_t              max_peers;         /* peer-table capacity; 16 */
} DartNodeDiscovery;

/* Optional node config, passed to dart_node_open as a compound literal (every field is
 * zero-means-default, so &(DartNodeOpts){0} or NULL is "all defaults"):
 *   dart_node_open(mem, "robot1", on_message, on_event, &(DartNodeOpts){ .domain = 7 });
 */
typedef struct {
    uint16_t              domain;        /* logical-network selector */
    uint16_t              max_channels;  /* how many channels can be created; 0 = 8 */
    void                 *user_data;     /* surfaced as DartMsg.user and DartEvent.user */
    uint8_t               disable_shm;   /* 1 = never use the same-host shared-memory fast path
                                            (force on-wire UDP even to a same-host peer; dynamic
                                            mode only, the static path never uses SHM) */
    DartNodeNet         net;           /* addressing/sockets (optional) */
    DartNodeDiscovery   discovery;     /* discovery cadence (optional) */
} DartNodeOpts;

/* Optional per-channel config, passed to dart_node_create_channel as a compound literal:
 *   dart_node_create_channel(n, "msg", DART_PUBSUB, pose_schema,
 *                            &(DartChannelOpts){ .qos = { .reliability = DART_RELIABLE } });
 */
typedef struct {
    DartQos  qos;
} DartChannelOpts;

typedef struct DartNode    DartNode;
typedef struct DartChannel DartChannel;   /* opaque channel handle (stable for the node's life) */

/* A delivered message: all of its properties in one place. channel_name is a local
 * lookup (never on the wire). From the callback, dart_channel_send and read-only
 * queries on the SAME node are allowed; poll/create_channel/set_role/drain/start/
 * stop/close are not (refused with DART_ERR_STATE / NULL / 0; under DART_NO_THREADS
 * the guards are gone, so simply do not call back in). See "threading" below. */
typedef struct {
    DartNode      *node;
    void          *user;             /* DartNodeOpts.user_data */
    uint16_t       channel_id;       /* local channel index */
    uint32_t       sender_id;        /* peer id the message came from */
    DartString     sender_name;      /* sender's node name (not NUL-terminated; use .data/.len).
                                        .data is never NULL for a delivered message ("unknown-peer"
                                        if somehow unavailable), so no null check is needed. A view
                                        into discovery state, never on the per-message wire; valid
                                        for the callback's duration. */
    DartString     channel_name;     /* topic name (not NUL-terminated; use .data/.len), or {NULL,0} */
    DartBytes      data;             /* the message payload (data.data, data.len) */
    const DartSchema *schema;        /* the schema data decodes with: this channel's fields bound
                                        to the sender's layout (a typed channel), or the sender's
                                        own schema (a NULL-schema channel; may still be NULL if
                                        the sender advertised none). Non-NULL means data.len was
                                        validated against it before delivery. */
} DartMsg;
typedef void (*DartMsgFn)(const DartMsg *msg);

/* Open a node backed by `alloc` (required): a static or dynamic DartAllocator (common/alloc.h)
 * the node allocates all its memory from and RESETS on close, so construct one per node and do
 * not reuse or touch it after close. name is this node's human-readable label, synced via
 * discovery and surfaced as DartMsg.sender_name; NULL/empty => an auto-generated "node-XXXXXXXX".
 * on_message (delivered messages) and on_event (peer/loss/QoS events) may each be NULL. opts may
 * be NULL for all defaults. Returns NULL on failure (incl. a static buffer too small). */
DartNode    *dart_node_open(DartAllocator *alloc, const char *name, DartMsgFn on_message,
                            DartEventFn on_event, const DartNodeOpts *opts);
/* The most recent error as a DartEvent (kind DART_ERROR; read .error / dart_event_str).
 * n == NULL returns the process-global slot: the reason a dart_node_open just returned
 * NULL (there is no handle then), so `if (!dart_node_open(...)) e = dart_last_error(NULL);`.
 * A non-NULL n returns that node's most recent runtime error. .kind is DART_PEER_UP (0)
 * with .error == DART_E_NONE if none has occurred. The global slot is best-effort under
 * threads (open is rare); a node's slot is only written from its own poll/callbacks. */
DartEvent    dart_last_error(DartNode *n);
/* One loop tick: discovery, RX + delivery, timers, TX flush; blocks up to timeout_ms
 * waiting for traffic (capped to the next internal timer; negative is treated as 0,
 * there is no infinite wait). Returns 0, or DART_ERR_STATE if a service thread is
 * running (dart_node_start owns the loop then) or when called from inside a callback. */
int          dart_node_poll(DartNode *n, int timeout_ms);
/* Stops the service thread if one runs, then tears the node down. Returns DART_OK, or
 * DART_ERR_STATE when called from inside a callback (refused: the node keeps running
 * and the handle stays valid, so close it from another thread). No other thread may be
 * inside (or later enter) any dart_* call on this node once close begins. */
int          dart_node_close(DartNode *n, int send_bye);

/* ---- threading --------------------------------------------------------------------
 * Every dart_node_* / dart_channel_* call is thread-safe: a node-level lock serializes
 * them, and it is never held across a blocking wait (a poller drops it around the
 * socket wait; mutating calls wake the poller through a loopback waker, so a send hits
 * the wire in microseconds, not at the next tick). Two ways to drive a node:
 *   - call dart_node_poll yourself (from one thread or several; sends from other
 *     threads interleave safely and wake a blocked poll), or
 *   - dart_node_start: a background SERVICE THREAD owns the loop; dart_node_poll then
 *     returns DART_ERR_STATE. Callbacks fire on the service thread (events triggered
 *     directly by one of your calls fire on that calling thread), and never two at
 *     once for one node.
 * From inside on_message/on_event: dart_channel_send and the read-only queries are
 * allowed ON THE SAME NODE; dart_node_poll / create_channel / set_role / drain /
 * start / stop / close are refused with DART_ERR_STATE (or NULL/0). Do NOT call into
 * a DIFFERENT in-process node from a callback: its lock is a plain acquisition, so
 * two nodes whose callbacks each send to the other deadlock (relay between in-process
 * nodes by queueing to your own thread instead). Flow control without a poll cadence:
 * a send that would overwrite reliable history not yet acked waits (condvar, bounded
 * by qos.backpressure_wait_us), and one that would overwrite history NEVER YET SENT
 * to a matched reader waits for one TX pass (bounded by DART_UNSENT_WAIT_US); if it
 * still must evict, it proceeds KEEP_LAST-style and fires DART_E_EVICTED_UNSENT, never
 * silently. All of this exists only under DART_THREADS, auto-detected where the
 * platform layer provides threads (Windows; POSIX with pthreads) and off elsewhere:
 * single-threaded contract, start returns DART_ERR_NOSYS. DART_NO_THREADS forces it
 * off; a new platform layer defines DART_THREADS to declare support (platform/core.h). */
/* Run the background service thread. DART_OK; DART_ERR_STATE if already running or
 * called from a callback; DART_ERR_NOSYS when threads are compiled out (DART_THREADS
 * off) or if the thread could not be created. */
int          dart_node_start(DartNode *n);
/* Stop and join the service thread (idempotent and safe to race: concurrent stoppers
 * elect one joiner; implied by dart_node_close). Senders blocked in a flow-control
 * wait wake and proceed (KEEP_LAST + the unsent guard, no pumping). */
int          dart_node_stop(DartNode *n);
/* 1 while the service thread runs. */
int          dart_node_is_started(DartNode *n);
/* Hold / release the node lock from a NON-callback thread around reads of a zero-copy
 * view (dart_node_peers) while a poller runs elsewhere. Hold it briefly: the node makes
 * no progress meanwhile, and while this thread holds it the node treats its calls like
 * callback calls (sends commit without waiting; poll/create_channel/set_role/drain/
 * start/stop/close refuse). Not recursion-counted: one lock pairs with one unlock.
 * No-ops from inside a callback (the lock is already held) and when threads are
 * compiled out. */
void         dart_node_lock(DartNode *n);
void         dart_node_unlock(DartNode *n);
/* Sends that evicted never-sent history after the bounded wait (the DART_E_EVICTED_UNSENT
 * count) since open: the send-burst/overload indicator. The guard runs when a service
 * thread drives the node (and for the reliable backpressure path when it does not);
 * classic poll-it-yourself best-effort keeps plain KEEP_LAST overwrite semantics. */
uint32_t     dart_node_evicted_unsent(DartNode *n);

/* Create a channel (topic). name is the cross-peer identity (same on every node, copied
 * in). role is DART_PUBSUB / DART_PUB_ONLY / DART_SUB_ONLY / DART_INACTIVE. schema is this
 * channel's data schema (built via dart_schema_*), required and held by reference so it
 * must outlive the channel; pass NULL for a raw-bytes channel (no serialization). opts may
 * be NULL for defaults. Returns a handle, or NULL if the reserve (opts.max_channels) is
 * full, the name is bad/too long, or out of memory. */
DartChannel *dart_node_create_channel(DartNode *n, const char *name, DartRole role,
                                      const DartSchema *schema, const DartChannelOpts *opts);
/* Publish to all matched subscribers. Returns DART_OK or a negative DartResult. */
int          dart_channel_send(DartChannel *ch, DartBytes data);
/* Flip a channel's role at runtime (re-advertises interest). Returns 0 ok, <0 on error. */
int          dart_channel_set_role(DartChannel *ch, DartRole role);
/* This channel's local index (== DartMsg.channel_id for its messages). */
uint16_t     dart_channel_index(const DartChannel *ch);
/* This channel's schema, as passed to create (NULL for a raw-bytes channel). */
const DartSchema *dart_channel_schema(const DartChannel *ch);
/* Recover an already-created channel handle by its creation index (0-based), or NULL if
 * out of range. Lets a caller use a handle without storing the create_channel result. */
DartChannel *dart_node_channel(DartNode *n, uint16_t index);

/* ---- read-only peer inspection (diagnostics / a discovery explorer) ----------------
 * The live peer table as a zero-copy array, valid until the next dart_node_poll. A node
 * peer IS a discovery peer: identity, locator, liveness, name, uuid, and the OPAQUE
 * announce overlay are exactly what discovery already holds, so this hands back discovery's
 * own view rather than copying into a parallel struct. The overlay's transport meaning (the
 * peer's UDP fragment size and pub/sub interest) is decoded on demand via dart_node_peer_frag
 * / dart_node_peer_interest_next (node/core.h), so a caller never touches dart_meta_*.
 * Returns the packed array + *count (used peers, ACTIVE or DROPPED); NULL if n is NULL. */
const DartDiscoveryPeer *dart_node_peers(DartNode *n, uint16_t *count);

/* Cumulative backpressure since open: us waited on slow readers and how many sends
 * waited. Either out-pointer may be NULL. */
void     dart_node_backpressure_stats(DartNode *n, uint64_t *waited_us, uint32_t *waited_sends);

/* Message-buffer memory (dynamic mode): in_use = live bytes, peak = high-water, alloc_calls
 * = how many heap (re)allocations have happened. alloc_calls stops rising once buffers reach
 * their steady-state sizes, so a flat count over a window proves the hot path is alloc-free.
 * Any out-pointer may be NULL. (Static mode: in_use/peak are 0; alloc_calls counts bumps.) */
void     dart_node_mem_stats(DartNode *n, size_t *in_use, size_t *peak, uint64_t *alloc_calls);
/* Cumulative reliable-repair counters for a channel (see DartRepairStats). The
 * per-second deltas are repair throughput; *out is zeroed for a NULL channel. */
void     dart_channel_repair_stats(DartChannel *ch, DartRepairStats *out);

/* In-pump diagnostic probe. A reliable publisher blocks inside dart_channel_send for the
 * whole backpressure wait, so its normal once-a-second print can't see within a stall.
 * A registered probe is called on a ~interval_us timer DURING that wait with the
 * writer's repair progress over each interval -- making the stall a within-block time
 * series (resends bursty-then-flat => reader stopped asking; steady => resends dropped).
 * Observational only; deltas are since the previous sample in the same wait. */
typedef struct {
    uint16_t channel;         /* channel index being pumped */
    uint64_t wait_elapsed_us; /* us since this backpressure wait began */
    uint64_t interval_us;     /* us since the previous sample (normalise deltas by this for true /s) */
    uint64_t frags_resent;    /* writer DATA fragments resent in the interval */
    uint64_t nacks_recv;      /* repair NACKs received in the interval */
    uint32_t polls;           /* dart_node_poll calls in the interval */
    uint32_t polls_idle;      /* of those, polls with no repair pending (writer idle for lack of NACKs) */
} DartPumpSample;
typedef void (*DartPumpProbeFn)(void *user, const DartPumpSample *s);
/* Register the in-pump probe (NULL fn disables). interval_us 0 => default 200ms. */
void     dart_node_set_pump_probe(DartNode *n, DartPumpProbeFn fn, uint64_t interval_us, void *user);
/* Head-of-line reassembly snapshot for the in-progress message from `peer` on this
 * channel: returns 1 + fills base_seqno/have/total if one is mid-reassembly, else 0.
 * `have` rising across calls = repair crawling; flat = wedged. Any pointer may be NULL. */
int      dart_channel_reader_progress(DartChannel *ch, uint32_t peer,
                            uint64_t *base_seqno, uint32_t *have, uint32_t *total);
/* Wait until every reader has acked all messages on this channel, or timeout_ms
 * elapses: with a service thread it sleeps on its progress, otherwise it pumps the
 * loop. Returns 1 if drained, 0 on timeout (or when called from a callback). Call
 * before close so a burst isn't cut by the BYE. */
int      dart_channel_drain(DartChannel *ch, int timeout_ms);
/* Subscribers matched on this channel now; a one-shot publisher polls it before sending. */
int      dart_channel_match_count(DartChannel *ch);
#ifdef DART_SHM
/* Messages published / delivered via the zero-fragment shared-memory path since open
 * (observability; same-host readers only). Either out-pointer may be NULL. */
void     dart_node_shm_stats(DartNode *n, uint32_t *sent, uint32_t *recv);
#endif

#ifdef __cplusplus
}
#endif
#endif /* DART_NODE_H */
#pragma endregion
#pragma region shm/core.h
/* dart_shm: zero-copy same-host payload path. OPT-IN -- nothing here compiles or
 * links unless you define DART_SHM, so embedded / non-SHM targets carry zero cost
 * and need no shared-memory platform support. Speaks only dart_plat_* (shm mapping,
 * host uuid, an atomic for the generation stamp).
 *
 * Model (per-peer, inside the transport's reliable stream -- NOT a side channel).
 * A published message occupies count seqnos on the writer's per-channel line, as
 * today. The per-peer LANE picks the wire form:
 *   - remote peer  -> count DATA fragments, read from the message buffer (as now)
 *   - same-host peer-> ONE SHM-DATA submessage (a DATA flag) covering [base,count),
 *                      carrying a 24-byte descriptor (segment+chunk+gen+len); the
 *                      reader marks the whole range delivered and reads the chunk
 *                      in place (zero copy), then ACKs the range like any reader.
 * The shared seqno line is untouched, so a channel serves local and remote
 * subscribers at once (and multicast: group-multicast to remote, unicast SHM-DATA
 * to each local sub). Eligibility is automatic: a lane uses SHM iff that peer is
 * same-host and attached.
 *
 * Lifecycle reuses reliability, so there is NO separate refcount/reclaim protocol:
 *   - the chunk IS the writer's history slot's buffer (one chunk per keep_last slot)
 *   - the reader delivers SYNCHRONOUSLY (on_message reads the chunk in place) and only
 *     THEN arms its ACK, which leaves on a later poll_send -> an ACK provably means
 *     "the user finished reading." The writer holds the chunk until that ACK.
 *   - so the writer recycles a slot only when the reader ACKED (done) or discovery
 *     declared it dormant/gone (a live reader mid-read is announcing, never dormant).
 *     SHM eviction is gated on ACK-or-LIVENESS, NOT the short backpressure_wait_us
 *     timer -- a slow-but-alive reader applies backpressure instead of having its
 *     chunk yanked mid-read. That is the torn-free guarantee for RELIABLE SHM.
 *   - generation is the backstop: a straggler that reads a reused chunk sees a
 *     generation mismatch and counts the sample lost (repaired on reliable, dropped
 *     on best-effort) instead of delivering torn bytes. Best-effort SHM has no ACKs,
 *     so a too-slow reader misses lapped samples, exactly like best-effort UDP.
 *   - contract: the on_message pointer is valid FOR THE CALL ONLY (already true for
 *     UDP); consume or copy it there. SHM just makes honoring it matter for safety.
 *
 * Zero copy both ways: the app loans a chunk and writes into it (dart_node_loan),
 * remote peers fragment straight from that chunk, local peers read it in place in
 * on_message (valid-for-the-call, the existing contract). One-copy fallback:
 * plain dart_channel_send memcpys into the chunk.
 *
 * Read modes (a future toggle; ship the safe one first):
 *   - ONE-COPY SHM (default): the reader memcpys the chunk into its own assembly_buf, then
 *     OWNS the bytes -- so it acks like UDP (ack timing is free, no deliver-before-ack
 *     coupling), the writer is released immediately, and there is no slow-reader stall
 *     or torn-read window. Still a big win: one SHM-DATA submessage + one local bulk
 *     copy replaces N fragment datagrams + reassembly.
 *   - ZERO-COPY SHM (opt-in): no copy, on_message reads the chunk in place; REQUIRES
 *     deliver-before-ack and holds the writer until the read completes (see below).
 *     The last increment, for latency/throughput-critical paths that accept the
 *     coupling. The difference is purely read-side: same wire format, same descriptor.
 *
 * This header is the portable mapping + chunk module. Its hooks into the transport
 * (the SHM-DATA submessage, the per-peer lane choice, chunk-backed history) and the
 * node (advertise, attach, deliver) are the contract in "INTEGRATION" below.
 */
#ifndef DART_SHM_H
#define DART_SHM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compile bounds (a fixed segment; an SBC sets these small, a workstation large). */
#ifndef DART_SHM_CHUNK_BYTES
#define DART_SHM_CHUNK_BYTES (4u*1024u*1024u)  /* default chunk; node overrides per size class */
#endif
#ifndef DART_SHM_CHUNKS
#define DART_SHM_CHUNKS 4u                     /* default chunks/segment; node overrides per class */
#endif
#ifndef DART_SHM_NAME_MAX
#define DART_SHM_NAME_MAX 64u                  /* OS object name, derived from the node uuid */
#endif

/* Size-class ladder (iceoryx-style): class k chunk payload = BASE << (k*SHIFT).
 * Defaults 64K,256K,1M,4M,16M,64M,256M (k=0..6) at SHIFT=2. The node lazily creates
 * one segment PER CHANNEL at that channel's size class (n_chunks = its keep_last), and
 * encodes the class in the low 3 bits of segment_id, the channel in the next 16. */
#ifndef DART_SHM_CLASS_BASE
#define DART_SHM_CLASS_BASE  (64u*1024u)
#endif
#ifndef DART_SHM_CLASS_SHIFT
#define DART_SHM_CLASS_SHIFT 2u
#endif
#ifndef DART_SHM_N_CLASSES
#define DART_SHM_N_CLASSES   7u
#endif
#define DART_SHM_CLASS_MASK  0x7u               /* class lives in the low 3 bits of segment_id */

uint32_t i_dart_shm_class_bytes(uint32_t k);      /* chunk payload bytes for class k */
uint32_t i_dart_shm_class_for(uint32_t len);      /* smallest class fitting len; N_CLASSES if too big */

/* Derive the OS object name for a segment id into buf[DART_SHM_NAME_MAX]:
 * "/dart.shm.<16 hex>", valid on POSIX (leading /) and Windows. The node fills
 * i_DartShmConfig.name with this for create/attach. */
void i_dart_shm_seg_name(char *buf, uint64_t segment_id);

/* ----------------------------------------------------------------- descriptor
 * The SHM locator. Travels INSIDE an SHM-DATA submessage, whose framing supplies
 * the seqno base + count (the transport fills those from the history sample), so
 * the descriptor itself is just where-to-read. generation lets a straggling reader
 * detect a recycled chunk and fall back to reliable repair. */
typedef struct {
    uint64_t segment_id;   /* writer's segment (its discovery uuid, hashed to 64) */
    uint32_t chunk;        /* chunk index in [0, n_chunks) */
    uint32_t length;       /* payload bytes */
    uint64_t generation;   /* chunk reuse counter at publish; reader rechecks after reading */
} i_DartShmDesc;

#define DART_SHM_DESC_WIRE 24u   /* little-endian; rides the SHM-DATA submessage body */
size_t i_dart_shm_desc_encode(const i_DartShmDesc *d, uint8_t out[DART_SHM_DESC_WIRE]);
int    i_dart_shm_desc_decode(i_DartShmDesc *d, const uint8_t *in, size_t len);  /* 1 ok, 0 malformed */

/* ------------------------------------------------------------- segment layout
 *   [ i_DartShmSegHdr ][ chunk 0 ] ... [ chunk N-1 ]
 *   chunk = [ i_DartShmChunkHdr (padded to 16) ][ chunk_bytes payload ]
 * generation is the only cross-process mutable field: written (atomic release) by
 * the writer before the descriptor is sent, read (atomic acquire) by the reader
 * after reading the payload. No refcount -- reliability owns the lifecycle. */
typedef struct {
    uint32_t magic;         /* DART_SHM_MAGIC; reject a stale/foreign mapping */
    uint32_t version;
    uint64_t segment_id;
    uint32_t chunk_bytes;   /* must equal the reader's compile bound, else reject */
    uint32_t n_chunks;
    uint64_t owner_pid;     /* writer pid: external janitor can reclaim an orphan */
    uint8_t  owner_host[16];/* writer host uuid: reader confirms same kernel */
} i_DartShmSegHdr;

typedef struct {
    uint64_t generation;    /* bumped each reuse; matched against the descriptor */
    uint32_t length;
    uint32_t _pad;
} i_DartShmChunkHdr;

#define DART_SHM_MAGIC    0x4D484453u   /* 'DSHM' */
#define DART_SHM_VERSION  1u

/* ------------------------------------------------------------------ pool (API)
 * Opaque per-process handle over one mapped segment, placed in caller memory
 * (the node arena; size via i_dart_shm_state_bytes). A node CREATEs one segment for
 * its own publishes and ATTACHes one per same-host peer it subscribes to. */
typedef struct i_DartShmPool i_DartShmPool;
size_t i_dart_shm_state_bytes(void);

typedef struct {
    char     name[DART_SHM_NAME_MAX];  /* writer makes it from its uuid; reader gets it via meta */
    uint64_t segment_id;
    uint32_t chunk_bytes;              /* create only (0 => DART_SHM_CHUNK_BYTES); attach reads it from the header */
    uint32_t n_chunks;                 /* create only (0 => DART_SHM_CHUNKS); attach reads it from the header */
} i_DartShmConfig;

/* Writer. create maps a fresh segment (i_dart_plat_shm_create); NULL => stay on UDP. */
i_DartShmPool *i_dart_shm_create(void *pool_mem, const i_DartShmConfig *cfg);
/* The chunk backing a history slot: loan returns a writable pointer (app fills it),
 * stamp bumps generation + sets length and fills *out for the transport to frame in
 * the SHM-DATA submessage. The node owns chunk<->slot assignment (1 chunk per
 * keep_last slot), so there is no free list here. */
void *i_dart_shm_chunk(i_DartShmPool *p, uint32_t chunk, uint32_t *out_cap);
void  i_dart_shm_stamp(i_DartShmPool *p, uint32_t chunk, uint32_t len, i_DartShmDesc *out);

/* Reader. attach maps an existing segment WHOLE by name and reads its geometry
 * (chunk_bytes/n_chunks) from the header the writer stamped, so the reader needs to
 * know nothing about its size; validates magic/version/owner_host==ours and that the
 * geometry fits the mapped object. NULL => fall back to the UDP path. read resolves a
 * descriptor to an in-segment pointer and verifies generation still matches (else
 * recycled -> NULL, reliable repair covers it). No release call: the reader's
 * transport ACK of the range is the release. */
i_DartShmPool *i_dart_shm_attach(void *pool_mem, const i_DartShmConfig *cfg);
const void    *i_dart_shm_read  (i_DartShmPool *p, const i_DartShmDesc *d, uint32_t *out_len);
/* re-check the chunk generation AFTER a one-copy read (seqlock tail): 1 if it still
 * matches d (the copy is clean), 0 if a best-effort writer recycled it mid-copy (the
 * copy may be torn -> discard). Lock-free: the writer never blocks. */
int            i_dart_shm_verify(i_DartShmPool *p, const i_DartShmDesc *d);

void i_dart_shm_detach(i_DartShmPool *p);  /* unmap; the writer also unlinks the OS object */

/* Same-host id: SHM is valid only between processes sharing one kernel AND able to
 * map the object (loopback addr alone is not sufficient -- containers/namespaces).
 * The node advertises i_dart_plat_host_uuid() + segment name in discovery; a peer is
 * SHM-reachable iff its host uuid equals ours and i_dart_shm_attach succeeds. */
int i_dart_shm_host_match(const uint8_t peer_host[16], const uint8_t our_host[16]);

/* === INTEGRATION (implemented under #ifdef DART_SHM) ========================
 *
 * dart_plat (add behind the existing Windows/POSIX split):
 *   void *i_dart_plat_shm_create(const char *name, size_t bytes, void **handle);
 *   void *i_dart_plat_shm_attach(const char *name, size_t *out_bytes, void **handle);
 *   void  i_dart_plat_shm_detach(void *base, size_t bytes, void *handle, int unlink_it);
 *   void  i_dart_plat_host_uuid(uint8_t out[16]);             (boot id / machine guid)
 *   uint64_t i_dart_plat_atomic_load64 / _store64(volatile uint64_t*[, v]);  (generation)
 *
 * transport (the per-peer lane + the new submessage; the only core change):
 *   - per-peer flag peer_shm[] (node sets it; like the existing peer_frag)
 *   - a history sample may be chunk-backed: a publish that hands in an external
 *     buffer (the chunk) + its descriptor, so dart_transport_send does not memcpy (zero copy)
 *   - SHM-DATA submessage: byte0 = DATA | DART_F_SHM, [alias][base seqno][count]
 *     [24-byte descriptor]. Writer lane emits it for an SHM peer instead of frags;
 *     reader marks [base,base+count) delivered, hands the descriptor up flagged.
 *   - delivery carries an "is SHM descriptor" flag to the node (the public app
 *     on_message is unchanged; the node wraps it -- see below)
 *
 * node (wiring):
 *   - advertise: meta blob -> ver 3, insert [u8 shm][u8 host[16]][u64 segment_id]
 *     between the frag prefix and the interest list (ver-2 peers ignore it)
 *   - on peer up: if peer.shm and host matches ours, i_dart_shm_attach its segment and
 *     set peer_shm in the transport; on down/dormant, detach / clear it
 *   - dart_node_loan(n, ch, len, &ptr) / dart_node_publish(n, ch): loan a chunk for
 *     the channel's next history slot, app fills ptr, publish hands the chunk +
 *     descriptor to the transport. dart_channel_send keeps working (one-copy into the
 *     chunk when the channel has any SHM peer, else plain inline)
 *   - on receive: the node's on_message wrapper sees the SHM flag, i_dart_shm_read the
 *     descriptor, calls the app on_message with the in-place pointer
 */

#ifdef __cplusplus
}
#endif
#endif /* DART_SHM_H */
#pragma endregion
#endif /* !DART_TRANSPORT_SANS_IO */

#ifdef DART_DISCOVERY_IMPLEMENTATION
#pragma region common/bytes.h
/* Shared little-endian byte packing, used by the discovery, transport, and SHM
 * layers (each formerly carried its own copy). static inline: no link symbol and
 * no unused-function warning in a layer that doesn't use a given width. The
 * amalgamator emits this once per implementation TU; the local #include is for
 * standalone compilation of a single layer. */
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
/* Bump allocator shared by the layers that pack sub-blocks into one caller-provided
 * arena (transport state, node). Measure mode (base==NULL): i_dart_bump_take returns NULL but
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
#pragma region discovery/core.c
/* sans-IO peer-discovery core. See dart_discovery.h. */
#include <string.h>

#define DART_DISCOVERY_HDR_LEN 24            /* magic(4) ver(1) flags(1) domain(2) uuid(16) */
/* the blob's discovery section: [u16 data_port][u8 self_ip_len][self_ip..][u8 name_len][name..] */
#define DART_DISCOVERY_DISC_MAX (2u + 1u + 16u + 1u + DART_DISCOVERY_NAME_MAX)
#define DART_DISCOVERY_FLAG_BYE 0x01
#define DART_DISCOVERY_FLAG_REQ 0x02         /* solicit: recipients announce back now */
#define DART_DISCOVERY_BLOB_RESEND 3u        /* announces that carry the full blob after a change */

struct i_DartDiscoveryPeer {
    uint8_t  used;
    uint8_t  dropped;       /* used but silent past peer_timeout_us: state kept for a same-UUID return */
    uint8_t  uuid[16];
    uint32_t local_id;
    uint8_t  ip[16];
    uint8_t  ip_len;
    uint16_t port;
    uint64_t last_heard_us;
    uint8_t *meta;          /* the OVERLAY only: a meta_pool slot (capacity st->meta_capacity),
                               or a hook allocation grown to the largest blob this slot has held */
    uint16_t meta_cap;      /* allocated capacity of this slot's meta buffer */
    uint16_t meta_len;
    uint32_t meta_version;  /* version of the blob we hold (0 = none yet) */
    uint32_t adv_version;   /* highest version the peer has advertised (> meta_version => we hold a stale blob) */
    uint8_t *user;          /* -> user_pool slot (opaque consumer scratch), stride st->user_stride */
    char     name[DART_DISCOVERY_NAME_MAX + 1];  /* advertised peer name, parsed from the blob */
    uint8_t  name_len;
    uint8_t  reply_due;     /* owes a unicast announce+blob (this peer solicited us) */
    uint8_t  solicit_due;   /* owes a unicast REQ (re-fetch: peer's version is ahead) */
};
typedef struct i_DartDiscoveryPeer i_DartDiscoveryPeer;

struct DartDiscoveryState {
    DartDiscoveryCoreConfig  cfg;
    uint64_t      next_announce_us;
    uint32_t      next_local_id;
    uint8_t       started;
    uint8_t       want_solicit;   /* a multicast solicit (REQ) is queued for the next update */
    uint16_t      cap_peers;
    uint16_t      meta_capacity;       /* per-peer meta buffer capacity */
    uint8_t      *meta_pool;      /* [cap_peers * meta_capacity]; NULL in hook mode (per-peer
                                     blobs are then cfg.alloc allocations at actual size) */
    uint16_t      user_stride;         /* per-peer user-scratch bytes, 8-aligned (0 = none) */
    uint8_t      *user_pool;      /* [cap_peers * user_stride] opaque consumer scratch */
    /* our outgoing blob + monotonic version */
    DartBytes     self_meta;        /* our OVERLAY (the node's frag/interest); discovery carries it.
                                       A read-only view of the node's buffer, not owned here. */
    uint32_t      self_meta_version;
    char          self_name[DART_DISCOVERY_NAME_MAX + 1];  /* our advertised name (copied from cfg) */
    uint8_t       self_name_len;
    uint16_t      self_blob_resend; /* announces remaining that carry the full blob */
    uint16_t      targeted_cursor;  /* round-robin over peers for poll_targeted */
    i_DartDiscoveryPeer  *peers;
};

/* peer events out: build the DartDiscoveryEvent and hand it to the one on_event sink.
 * peer_up surfaces the parsed name + the opaque overlay we hold for the peer. */
static void i_dart_discovery_fire_up(DartDiscoveryState *st, const i_DartDiscoveryPeer *peer,
                               const DartDiscoveryAddr *addr){
    DartDiscoveryEvent ev;
    if (!st->cfg.on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = DART_DISCOVERY_PEER_UP; ev.user = st->cfg.user; ev.peer = peer->local_id;
    if (addr) ev.addr = *addr;
    ev.name = dart_string(peer->name_len ? peer->name : NULL, peer->name_len);
    ev.meta = dart_bytes(peer->meta_len ? peer->meta : NULL, peer->meta_len);
    st->cfg.on_event(&ev);
}
static void i_dart_discovery_fire_down(DartDiscoveryState *st, uint32_t id, DartDiscoveryDownReason reason){
    DartDiscoveryEvent ev;
    if (!st->cfg.on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = DART_DISCOVERY_PEER_DOWN; ev.user = st->cfg.user; ev.peer = id; ev.reason = reason;
    st->cfg.on_event(&ev);
}
static void i_dart_discovery_fire_refused(DartDiscoveryState *st, const DartDiscoveryAddr *addr){
    DartDiscoveryEvent ev;
    if (!st->cfg.on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = DART_DISCOVERY_PEER_REFUSED; ev.user = st->cfg.user;
    if (addr) ev.addr = *addr;
    st->cfg.on_event(&ev);
}
static void i_dart_discovery_fire_meta_too_big(DartDiscoveryState *st, uint32_t id,
                                     const DartDiscoveryAddr *addr, DartBytes overlay){
    DartDiscoveryEvent ev;
    if (!st->cfg.on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = DART_DISCOVERY_META_TOO_BIG; ev.user = st->cfg.user; ev.peer = id;
    if (addr) ev.addr = *addr;
    ev.meta = overlay;
    st->cfg.on_event(&ev);
}

static uint32_t i_dart_discovery_fnv(const uint8_t *d, size_t n){
    uint32_t h = 2166136261u; size_t i;
    for (i=0;i<n;i++){ h ^= d[i]; h *= 16777619u; }
    return h;
}

void dart_discovery_make_uuid(uint8_t out[16], DartBytes stable, uint64_t seed){
    uint64_t x = 1469598103934665603ull; size_t i; int k;
    for (i=0;i<stable.len;i++){ x = (x ^ stable.data[i]) * 1099511628211ull; }
    x ^= seed;
    for (k=0;k<2;k++){
        uint64_t z = (x += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z>>30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z>>27)) * 0x94D049BB133111EBull;
        z ^=  z>>31;
        memcpy(out + (size_t)k*8, &z, 8);
    }
    out[6] = (uint8_t)((out[6] & 0x0Fu) | 0x80u);  /* version 8 (custom) */
    out[8] = (uint8_t)((out[8] & 0x3Fu) | 0x80u);  /* variant 10x (RFC) */
}

static uint16_t i_dart_discovery_meta_capacity(const DartDiscoveryCoreConfig *cfg){
    return cfg->meta_capacity ? cfg->meta_capacity : DART_DISCOVERY_META_MAX;
}

/* per-peer user-scratch stride: the requested bytes rounded up to 8 so every slot is
 * 8-aligned (a consumer may store a pointer there). 0 bytes => no pool. */
static uint16_t i_dart_discovery_user_stride(const DartDiscoveryCoreConfig *cfg){
    return cfg->peer_user_bytes ? (uint16_t)((cfg->peer_user_bytes + 7u) & ~7u) : 0u;
}

void dart_discovery_config_defaults(DartDiscoveryCoreConfig *cfg){
    if (!cfg) return;
    if (cfg->announce_interval_us == 0) cfg->announce_interval_us = 1000000u;
    if (cfg->peer_timeout_us == 0)      cfg->peer_timeout_us = cfg->announce_interval_us * 7u / 2u;
    if (cfg->gone_timeout_us == 0)      cfg->gone_timeout_us = 60000000u;   /* 1 min dropped -> GONE */
    if (cfg->max_peers == 0)            cfg->max_peers = 32u;
}

uint32_t dart_discovery_wire_size(uint16_t meta_capacity){
    uint32_t cap = meta_capacity ? meta_capacity : DART_DISCOVERY_META_MAX;
    /* the wire blob = discovery section + overlay, so the scratch buffer must hold both */
    uint32_t w = (uint32_t)DART_DISCOVERY_META_OFF + DART_DISCOVERY_DISC_MAX + cap;
    return w < DART_DISCOVERY_WIRE_MAX ? DART_DISCOVERY_WIRE_MAX : w;
}

/* Single source of the discovery arena layout: state, the peer table, the meta pool.
   measure (bump.base NULL) feeds required_memory; build feeds init -- one definition. */
typedef struct { DartDiscoveryState *st; uint8_t *peers, *meta_pool, *user_pool; } i_DartDiscoveryBlocks;
static void i_dart_discovery_layout(i_DartBump *b, const DartDiscoveryCoreConfig *cfg, i_DartDiscoveryBlocks *o){
    uint16_t meta_capacity = i_dart_discovery_meta_capacity(cfg);
    uint16_t user_stride   = i_dart_discovery_user_stride(cfg);
    o->st        = (DartDiscoveryState*)i_dart_bump_take(b, sizeof(struct DartDiscoveryState), 8);
    o->peers     = (uint8_t*)i_dart_bump_take(b, (size_t)cfg->max_peers * sizeof(i_DartDiscoveryPeer), 8);
    /* hook mode allocates each peer's blob on demand at its actual size; only the
       no-hook (embedded) path reserves the worst-case max_peers x meta_capacity pool */
    o->meta_pool = cfg->alloc ? NULL
                 : (uint8_t*)i_dart_bump_take(b, (size_t)cfg->max_peers * meta_capacity, 1);
    o->user_pool = user_stride ? (uint8_t*)i_dart_bump_take(b, (size_t)cfg->max_peers * user_stride, 8) : NULL;
}

size_t dart_discovery_required_memory(const DartDiscoveryCoreConfig *cfg){
    i_DartBump b; i_DartDiscoveryBlocks blk;
    if (!cfg) return 0;
    memset(&b, 0, sizeof b);
    i_dart_discovery_layout(&b, cfg, &blk);
    return b.offset + 8u;     /* slack to align the caller's mem up to base */
}

DartDiscoveryState *dart_discovery_init(void *mem, size_t cap, const DartDiscoveryCoreConfig *cfg){
    i_DartBump b; i_DartDiscoveryBlocks blk; DartDiscoveryState *st; uint16_t i, meta_capacity;
    if (!mem || !cfg || cfg->max_peers == 0) return NULL;
    if (cfg->announce_interval_us == 0 || cfg->peer_timeout_us == 0) return NULL;
    meta_capacity = i_dart_discovery_meta_capacity(cfg);
    if (cfg->meta.len > meta_capacity) return NULL;
    if (cfg->meta.len && !cfg->meta.data) return NULL;
    if (cap < dart_discovery_required_memory(cfg)) return NULL;

    memset(&b, 0, sizeof b);
    b.base = (uint8_t*)(((uintptr_t)mem + 7u) & ~(uintptr_t)7u);
    b.cap  = cap - (size_t)(b.base - (uint8_t*)mem);
    i_dart_discovery_layout(&b, cfg, &blk);

    st = blk.st;
    memset(st, 0, sizeof(*st));
    st->cfg           = *cfg;
    st->cap_peers     = cfg->max_peers;
    st->meta_capacity      = meta_capacity;
    st->peers         = (i_DartDiscoveryPeer *)blk.peers;
    st->meta_pool     = blk.meta_pool;
    st->user_stride   = i_dart_discovery_user_stride(cfg);
    st->user_pool     = blk.user_pool;
    st->next_local_id = 1;
    st->started       = 0;
    memset(st->peers, 0, (size_t)st->cap_peers * sizeof(i_DartDiscoveryPeer));
    if (st->user_pool) memset(st->user_pool, 0, (size_t)st->cap_peers * st->user_stride);
    for (i=0;i<st->cap_peers;i++){
        st->peers[i].meta     = st->meta_pool ? st->meta_pool + (size_t)i * meta_capacity : NULL;
        st->peers[i].meta_cap = st->meta_pool ? meta_capacity : 0;
        st->peers[i].user = st->user_pool ? st->user_pool + (size_t)i * st->user_stride : NULL;
    }
    st->self_meta         = cfg->meta;
    st->self_meta_version = 1;
    st->self_blob_resend  = DART_DISCOVERY_BLOB_RESEND;
    {   uint8_t nl = cfg->name.len > DART_DISCOVERY_NAME_MAX ? DART_DISCOVERY_NAME_MAX : (uint8_t)cfg->name.len;
        if (cfg->name.data && nl) memcpy(st->self_name, cfg->name.data, nl);
        st->self_name[nl] = '\0'; st->self_name_len = nl; }
    return st;
}


/* Relocate a live discovery core into a bigger block at grown counts. NOT a re-init:
 * the UUID, the monotonic blob version, the local-id counter and the peer table must
 * survive (a re-init would reset them and peers would treat us as a new node). self_meta
 * is an external pointer (our announce blob, in the node core); the caller passes its new
 * address. Each peer's meta is re-pointed into the new pool and its blob bytes copied. */
DartDiscoveryState *dart_discovery_core_migrate(DartDiscoveryState *old, void *new_mem,
        size_t new_cap, uint16_t new_max_peers, uint16_t new_meta_capacity,
        const uint8_t *self_meta, void *peer_cb_user){
    i_DartBump b; i_DartDiscoveryBlocks blk; DartDiscoveryState *st; DartDiscoveryCoreConfig dc; uint16_t i, omp;
    if (!old) return NULL;
    dc = old->cfg; dc.max_peers = new_max_peers; dc.meta_capacity = new_meta_capacity;
    if (new_cap < dart_discovery_required_memory(&dc)) return NULL;
    memset(&b,0,sizeof b);
    b.base = (uint8_t*)(((uintptr_t)new_mem + 7u) & ~(uintptr_t)7u);
    b.cap  = new_cap - (size_t)(b.base - (uint8_t*)new_mem);
    i_dart_discovery_layout(&b, &dc, &blk);
    st = blk.st;
    *st = *old;                            /* cfg (uuid!), counters, version, started, cursors */
    st->cfg.max_peers     = new_max_peers;
    st->cfg.meta_capacity = new_meta_capacity;
    st->cfg.user          = peer_cb_user;  /* peer callbacks fire on the relocated node core */
    st->cap_peers         = new_max_peers;
    st->meta_capacity     = new_meta_capacity;
    st->peers             = (i_DartDiscoveryPeer*)blk.peers;
    st->meta_pool         = blk.meta_pool;
    st->user_pool         = blk.user_pool;     /* user_stride is unchanged (copied via *st = *old) */
    st->self_meta.data    = self_meta;     /* re-point our blob (len preserved via *st=*old); version NOT bumped */
    memset(st->peers, 0, (size_t)new_max_peers * sizeof(i_DartDiscoveryPeer));
    for (i=0;i<new_max_peers;i++){
        st->peers[i].meta     = st->meta_pool ? st->meta_pool + (size_t)i * new_meta_capacity : NULL;
        st->peers[i].meta_cap = st->meta_pool ? new_meta_capacity : 0;
        st->peers[i].user = st->user_pool ? st->user_pool + (size_t)i * st->user_stride : NULL;
    }
    if (st->user_pool) memset(st->user_pool, 0, (size_t)new_max_peers * st->user_stride);
    omp = old->cap_peers;
    for (i=0;i<omp;i++){
        uint8_t *nmeta = st->peers[i].meta, *nuser = st->peers[i].user;
        uint16_t ncap  = st->peers[i].meta_cap;
        st->peers[i] = old->peers[i];      /* carries old meta/user ptrs + everything */
        if (st->meta_pool){                /* pool mode: re-point into the new pool + copy the
                                              bytes. Hook mode: the blob allocation is stable
                                              (outside the arena), so the struct copy carried it. */
            st->peers[i].meta = nmeta; st->peers[i].meta_cap = ncap;
            if (old->peers[i].meta_len) memcpy(nmeta, old->peers[i].meta, old->peers[i].meta_len);
        }
        st->peers[i].user = nuser;         /* re-point into the new user pool */
        if (st->user_stride && nuser && old->peers[i].user)
            memcpy(nuser, old->peers[i].user, st->user_stride);   /* preserve consumer scratch */
    }
    return st;
}

void dart_discovery_destroy(DartDiscoveryState *st){
    uint16_t i;
    if (!st || !st->cfg.alloc) return;   /* pool mode: nothing hook-allocated */
    for (i=0;i<st->cap_peers;i++)
        if (st->peers[i].meta){
            st->cfg.alloc(st->cfg.alloc_user, st->peers[i].meta, 0);
            st->peers[i].meta = NULL; st->peers[i].meta_cap = 0; st->peers[i].meta_len = 0;
        }
}

/* find by UUID, including DROPPED entries: a same-UUID return reuses the slot (and
 * thus the local_id), so the IO layer's transport state keyed by local_id resumes. */
static int i_dart_discovery_find(DartDiscoveryState *st, const uint8_t *uuid){
    uint16_t i;
    for (i=0;i<st->cap_peers;i++)
        if (st->peers[i].used && memcmp(st->peers[i].uuid, uuid, 16)==0) return (int)i;
    return -1;
}

/* a slot for a brand-new peer: a FREE one, else the oldest DROPPED one (evicted,
 * fired GONE so its state is freed). ACTIVE peers are never evicted; -1 = refuse. */
static int i_dart_discovery_alloc(DartDiscoveryState *st){
    uint16_t i, victim = 0; uint64_t oldest = (uint64_t)-1; int found = -1;
    for (i=0;i<st->cap_peers;i++){
        if (!st->peers[i].used) return (int)i;
        if (st->peers[i].dropped && st->peers[i].last_heard_us <= oldest){
            oldest = st->peers[i].last_heard_us; victim = i; found = 1;
        }
    }
    if (found < 0) return -1;   /* table full of ACTIVE peers: caller refuses + signals */
    i_dart_discovery_fire_down(st, st->peers[victim].local_id, DART_DISCOVERY_GONE);
    st->peers[victim].used = 0;
    return (int)victim;
}

/* A different uuid announcing from an (ip,port) we already hold means that endpoint's
 * process restarted: one socket is one process, so the old entry is provably dead.
 * Evict it as GONE (state freed) before adopting the newcomer, so its stale transport
 * state can't shadow the new incarnation whose data routes to the same address.
 * A 0 port is "locator not advertised yet" (a peer first seen via a blob-less announce),
 * not a real socket: distinct peers awaiting their blob share that non-endpoint, so it
 * must NOT trigger eviction (else several same-host instances churn, evicting each other). */
static void i_dart_discovery_evict_endpoint(DartDiscoveryState *st, const DartDiscoveryAddr *addr){
    uint16_t i;
    if (!addr->ip_len || !addr->port) return;
    for (i=0;i<st->cap_peers;i++){
        i_DartDiscoveryPeer *peer = &st->peers[i];
        if (!peer->used) continue;
        if (peer->ip_len==addr->ip_len && peer->port==addr->port && memcmp(peer->ip, addr->ip, 16)==0){
            i_dart_discovery_fire_down(st, peer->local_id, DART_DISCOVERY_GONE);   /* fire, then free */
            peer->used = 0;
        }
    }
}

/* build an announce/solicit/bye into p. with_blob includes the meta blob =
 * [discovery section: locator + name][opaque overlay]; every datagram carries the meta
 * version so a blob-less announce still signals change. The fixed header is the small part. */
static size_t i_dart_discovery_build(DartDiscoveryState *st, uint8_t flags, int with_blob,
                                   uint8_t *p, size_t cap){
    uint16_t meta_len = 0;
    if (cap < (size_t)DART_DISCOVERY_META_OFF) return 0;
    p[0]='u'; p[1]='D'; p[2]='S'; p[3]='C';
    p[4]=(uint8_t)DART_DISCOVERY_PROTO_VERSION;
    p[5]=flags;
    i_dart_le_w16(p+6, st->cfg.domain_id);
    memcpy(p+8, st->cfg.uuid, 16);
    if (with_blob){
        uint8_t *b = p + DART_DISCOVERY_META_OFF, *bend = p + cap;
        uint8_t ipl = (st->cfg.self_ip_len==4 || st->cfg.self_ip_len==16) ? st->cfg.self_ip_len : 0;
        size_t need = 2u + 1u + (size_t)ipl + 1u + st->self_name_len + st->self_meta.len;
        if ((size_t)(bend - b) < need) return 0;
        i_dart_le_w16(b, st->cfg.data_port); b += 2;            /* discovery section: locator */
        *b++ = ipl;
        if (ipl){ memcpy(b, st->cfg.self_ip, ipl); b += ipl; }
        *b++ = st->self_name_len;                             /* ... + name */
        if (st->self_name_len){ memcpy(b, st->self_name, st->self_name_len); b += st->self_name_len; }
        if (st->self_meta.len){ memcpy(b, st->self_meta.data, st->self_meta.len); b += st->self_meta.len; }  /* overlay */
        meta_len = (uint16_t)(b - (p + DART_DISCOVERY_META_OFF));
    }
    i_dart_le_w32(p+DART_DISCOVERY_HDR_LEN, st->self_meta_version);
    i_dart_le_w16(p+DART_DISCOVERY_HDR_LEN+4, meta_len);
    return (size_t)DART_DISCOVERY_META_OFF + meta_len;
}

static void i_dart_discovery_addr_of(const i_DartDiscoveryPeer *peer, DartDiscoveryAddr *out){
    memset(out, 0, sizeof *out);
    memcpy(out->ip, peer->ip, 16);
    out->ip_len = peer->ip_len;
    out->port   = peer->port;
}

void dart_discovery_on_datagram(DartDiscoveryState *st, const uint8_t *src_ip, uint8_t src_ip_len,
                       DartBytes datagram, uint64_t now){
    const uint8_t *p = datagram.data; size_t len = datagram.len;
    uint8_t flags; uint16_t meta_len; uint32_t meta_version;
    const uint8_t *uuid, *blob;
    DartDiscoveryAddr addr; int idx, addr_changed, first_contact=0, blob_changed=0, revived=0;
    i_DartDiscoveryPeer *peer;
    /* the blob's discovery section (valid only when have_disc): locator + name, then overlay.
       disc_name points into the datagram (NOT NUL-terminated): a non-NUL wire string. */
    int have_disc=0; uint16_t disc_port=0; uint8_t disc_ip_len=0;
    const uint8_t *disc_ip=NULL; DartBytes overlay = {NULL, 0}; DartString disc_name = {NULL, 0};

    if (len < (size_t)DART_DISCOVERY_META_OFF) return;
    if (p[0]!='u'||p[1]!='D'||p[2]!='S'||p[3]!='C') return;
    if (p[4]!=(uint8_t)DART_DISCOVERY_PROTO_VERSION) return;
    if (i_dart_le_r16(p+6) != st->cfg.domain_id) return;
    uuid = p+8;
    if (memcmp(uuid, st->cfg.uuid, 16)==0) return;  /* ignore self */
    meta_version = i_dart_le_r32(p+DART_DISCOVERY_HDR_LEN);
    meta_len = i_dart_le_r16(p+DART_DISCOVERY_HDR_LEN+4);
    if ((size_t)DART_DISCOVERY_META_OFF + meta_len > len){
        /* the OS truncated the datagram to our RX buffer: the fixed header (always
           intact) says the blob is meta_len bytes, so this peer's metadata exceeds what
           this side can currently receive. Same never-silent signal as the capacity
           refuse below, with .meta = {NULL, needed bytes}: an IO layer that can grow
           does so and re-solicits; one that cannot surfaces it. */
        DartDiscoveryAddr a; int at = i_dart_discovery_find(st, uuid);
        memset(&a, 0, sizeof a);
        if (src_ip && (src_ip_len==4 || src_ip_len==16)){ memcpy(a.ip, src_ip, src_ip_len); a.ip_len = src_ip_len; }
        i_dart_discovery_fire_meta_too_big(st, at >= 0 ? st->peers[at].local_id : 0, &a,
                                           dart_bytes(NULL, meta_len));
        return;
    }
    blob = p + DART_DISCOVERY_META_OFF;
    flags = p[5];

    /* parse the blob's discovery section (locator + name); the remainder is the opaque
       overlay we hand up. Fully bounds-checked: a malformed blob drops the datagram. */
    if (meta_len){
        const uint8_t *b = blob, *bend = blob + meta_len;
        if (b + 3 > bend) return;                          /* port(2) + ip_len(1) */
        disc_port = i_dart_le_r16(b); b += 2;
        disc_ip_len = *b++;
        if (disc_ip_len==4 || disc_ip_len==16){ if (b + disc_ip_len > bend) return; disc_ip = b; b += disc_ip_len; }
        else disc_ip_len = 0;
        if (b + 1 > bend) return;                          /* name_len(1) */
        {   uint8_t dnl = *b++;
            if (dnl){ if (b + dnl > bend) return; disc_name = dart_string((const char*)b, dnl); b += dnl; } }
        overlay = dart_bytes(b, (size_t)(bend - b));
        if (overlay.len > st->meta_capacity){   /* overlay must fit the per-peer buffer: this side
                                                   can NEVER hold that peer's metadata, so say so */
            DartDiscoveryAddr a; int at = i_dart_discovery_find(st, uuid);
            memset(&a, 0, sizeof a);
            if (disc_ip_len){ memcpy(a.ip, disc_ip, disc_ip_len); a.ip_len = disc_ip_len; }
            a.port = disc_port;
            i_dart_discovery_fire_meta_too_big(st, at >= 0 ? st->peers[at].local_id : 0, &a, overlay);
            return;
        }
        have_disc = 1;
    }

    idx = i_dart_discovery_find(st, uuid);

    if (flags & DART_DISCOVERY_FLAG_BYE){
        if (idx >= 0){
            /* fire GONE while the slot still exists, so the handler can read its scratch,
               then free it */
            i_dart_discovery_fire_down(st, st->peers[idx].local_id, DART_DISCOVERY_GONE);
            st->peers[idx].used = 0;
        }
        return;
    }

    /* address: from the blob's locator when present (self_ip override, else the datagram
       source); otherwise the cached locator (it is static between blob updates), or the
       datagram source on first contact with no blob. */
    memset(&addr, 0, sizeof addr);
    if (have_disc){
        if (disc_ip_len){ addr.ip_len = disc_ip_len; memcpy(addr.ip, disc_ip, disc_ip_len); }
        else if (src_ip && (src_ip_len==4 || src_ip_len==16)){ addr.ip_len = src_ip_len; memcpy(addr.ip, src_ip, src_ip_len); }
        else return;
        addr.port = disc_port;
    } else if (idx >= 0){
        addr.ip_len = st->peers[idx].ip_len; addr.port = st->peers[idx].port;
        memcpy(addr.ip, st->peers[idx].ip, 16);
    } else if (src_ip && (src_ip_len==4 || src_ip_len==16)){
        addr.ip_len = src_ip_len; memcpy(addr.ip, src_ip, src_ip_len);   /* port unknown until the blob */
    } else return;

    if (idx < 0){
        uint8_t *keep_meta, *keep_user; uint16_t keep_cap;
        /* new uuid from an address we already hold => the endpoint's process restarted;
           evict the dead predecessor (frees its slot for reuse) so it can't shadow us */
        i_dart_discovery_evict_endpoint(st, &addr);
        idx = i_dart_discovery_alloc(st);
        if (idx < 0){       /* table full of active peers: refuse, never evict a live one */
            i_dart_discovery_fire_refused(st, &addr);
            return;
        }
        keep_meta = st->peers[idx].meta;            /* preserve the blob buffer across reset: the
                                                       pool slot, or the previous occupant's grown
                                                       hook allocation (reused, not freed) */
        keep_cap  = st->peers[idx].meta_cap;
        keep_user = st->peers[idx].user;
        memset(&st->peers[idx], 0, sizeof(i_DartDiscoveryPeer));
        st->peers[idx].meta     = keep_meta;
        st->peers[idx].meta_cap = keep_cap;
        st->peers[idx].user     = keep_user;
        if (keep_user) memset(keep_user, 0, st->user_stride);   /* fresh consumer scratch for the new peer */
        st->peers[idx].used     = 1;
        memcpy(st->peers[idx].uuid, uuid, 16);
        st->peers[idx].local_id = st->next_local_id++;
        st->peers[idx].ip_len   = 0xFF;   /* force first peer_up */
        first_contact = 1;
    }
    peer = &st->peers[idx];
    if (peer->dropped){ peer->dropped = 0; revived = 1; }   /* a DROPPED peer returned: resume it */
    peer->last_heard_us = now;

    addr_changed = (peer->ip_len != addr.ip_len)
                || (peer->port   != addr.port)
                || (memcmp(peer->ip, addr.ip, 16) != 0);
    if (addr_changed){
        peer->ip_len = addr.ip_len; peer->port = addr.port;
        memcpy(peer->ip, addr.ip, 16);
    }

    /* meta: a newer version with the blob present updates our stored overlay + name; a newer
       version without the blob (a steady-state version-only announce) means we fell behind,
       so re-fetch via a targeted solicit. adv_version tracks the highest version the peer has
       claimed (blob present or not); > meta_version means our held blob is stale. */
    if (meta_version > peer->adv_version) peer->adv_version = meta_version;
    if (have_disc){
        if (meta_version > peer->meta_version){
            uint8_t nl = disc_name.len > DART_DISCOVERY_NAME_MAX ? DART_DISCOVERY_NAME_MAX : (uint8_t)disc_name.len;
            int fits = 1;
            if (overlay.len > peer->meta_cap){   /* hook mode: (re)size to the actual blob;
                                                    pool-mode slots are pre-sized (checked above) */
                uint8_t *nb = st->cfg.alloc ? (uint8_t*)st->cfg.alloc(st->cfg.alloc_user, peer->meta, overlay.len)
                                            : NULL;
                if (nb){ peer->meta = nb; peer->meta_cap = (uint16_t)overlay.len; }
                else fits = 0;                   /* OOM: keep the stale blob + version; adv_version
                                                    stays ahead, so the re-fetch path retries */
            }
            if (fits){
                if (overlay.len) memcpy(peer->meta, overlay.data, overlay.len);
                peer->meta_len = (uint16_t)overlay.len; peer->meta_version = meta_version;
                if (disc_name.data && nl) memcpy(peer->name, disc_name.data, nl);
                peer->name[nl] = '\0'; peer->name_len = nl;
                blob_changed = 1;
            }
        }
        peer->solicit_due = 0;
    } else if (meta_version > peer->meta_version){
        peer->solicit_due = 1;
    }

    if (first_contact || addr_changed || blob_changed || revived)
        i_dart_discovery_fire_up(st, peer, &addr);

    if ((flags & DART_DISCOVERY_FLAG_REQ) && st->started)
        peer->reply_due = 1;   /* answer the solicit with a unicast announce + blob */
}

size_t dart_discovery_update(DartDiscoveryState *st, uint64_t now, void *out, size_t cap){
    uint16_t i;
    if (!st->started){
        st->started = 1;
        st->next_announce_us = now + (i_dart_discovery_fnv(st->cfg.uuid,16) % st->cfg.announce_interval_us);
        st->want_solicit = 1;   /* solicit on startup */
    }
    for (i=0;i<st->cap_peers;i++){
        if (!st->peers[i].used) continue;
        if (st->peers[i].dropped){
            /* dropped and still silent past the gone timeout: a same-UUID return is no longer
               expected, so promote to GONE -- free the transport state and reclaim the slot
               (fire before free so the handler can still read it). 0 = never promote. */
            if (st->cfg.gone_timeout_us &&
                now - st->peers[i].last_heard_us > (uint64_t)st->cfg.peer_timeout_us + st->cfg.gone_timeout_us){
                i_dart_discovery_fire_down(st, st->peers[i].local_id, DART_DISCOVERY_GONE);
                st->peers[i].used = 0;
            }
            continue;
        }
        if (now - st->peers[i].last_heard_us > st->cfg.peer_timeout_us){
            /* fell silent: DEMOTE (keep the entry + local_id) so a same-UUID return
               resumes; the IO layer keeps its transport state on a DROP reason */
            st->peers[i].dropped = 1;
            i_dart_discovery_fire_down(st, st->peers[i].local_id, DART_DISCOVERY_DROP);
        }
    }
    if (st->want_solicit){   /* multicast solicit: announce us (with blob) AND ask peers to reply */
        st->want_solicit = 0;
        return i_dart_discovery_build(st, DART_DISCOVERY_FLAG_REQ, 1, (uint8_t *)out, cap);
    }
    if (now >= st->next_announce_us){
        int with_blob = st->self_blob_resend > 0;
        if (with_blob) st->self_blob_resend--;
        st->next_announce_us = now + st->cfg.announce_interval_us;
        return i_dart_discovery_build(st, 0, with_blob, (uint8_t *)out, cap);
    }
    return 0;
}

uint64_t dart_discovery_next_due_us(const DartDiscoveryState *st){
    if (!st || !st->started || st->want_solicit) return 0;
    return st->next_announce_us;   /* 0 after an advertise = announce the change now */
}

void dart_discovery_set_meta(DartDiscoveryState *st, DartBytes meta){
    if (!st || meta.len > st->meta_capacity) return;   /* the node sizes meta_capacity to fit */
    st->self_meta         = meta;
    st->self_meta_version++;
    st->self_blob_resend  = DART_DISCOVERY_BLOB_RESEND;
    st->next_announce_us  = 0;   /* announce the change now, don't wait for the timer */
}

uint32_t dart_discovery_meta_version(const DartDiscoveryState *st){
    return st ? st->self_meta_version : 0;
}

void dart_discovery_set_data_port(DartDiscoveryState *st, uint16_t port){
    if (!st || st->cfg.data_port == port) return;
    st->cfg.data_port = port;            /* the port now rides the blob's discovery section, so */
    st->self_meta_version++;             /* bump the version + re-send so peers re-fetch it */
    st->self_blob_resend = DART_DISCOVERY_BLOB_RESEND;
    st->next_announce_us = 0;
}

size_t dart_discovery_poll_targeted(DartDiscoveryState *st, void *out, size_t cap,
                                    DartDiscoveryAddr *to){
    uint16_t n = st->cap_peers, k;
    if (n == 0) return 0;
    for (k=0;k<n;k++){
        uint16_t i = st->targeted_cursor;
        i_DartDiscoveryPeer *peer = &st->peers[i];
        st->targeted_cursor = (uint16_t)((i+1u) % n);
        if (!peer->used){ peer->reply_due = peer->solicit_due = 0; continue; }
        if (peer->reply_due){                 /* reply to a soliciter: announce + blob, unicast */
            peer->reply_due = 0;
            i_dart_discovery_addr_of(peer, to);
            return i_dart_discovery_build(st, 0, 1, (uint8_t *)out, cap);
        }
        if (peer->solicit_due){               /* re-fetch: ask this peer to announce back to us */
            peer->solicit_due = 0;
            i_dart_discovery_addr_of(peer, to);
            return i_dart_discovery_build(st, DART_DISCOVERY_FLAG_REQ, 1, (uint8_t *)out, cap);
        }
    }
    return 0;
}

/* queue a one-shot multicast solicit: the next update emits a REQ asking peers to announce now */
void dart_discovery_solicit(DartDiscoveryState *st){ if (st) st->want_solicit = 1; }

/* re-deliver every live peer's last-known announce to on_peer_up, so a caller that just
 * changed its own advertised data re-applies all peer interest against the new state. No
 * version change is involved: a peer's blob is unchanged, but the LOCAL side may now have
 * a channel that the blob's interest matches. */
void dart_discovery_replay_peers(DartDiscoveryState *st){
    uint16_t i;
    if (!st || !st->cfg.on_event) return;
    for (i=0;i<st->cap_peers;i++){
        i_DartDiscoveryPeer *peer = &st->peers[i];
        DartDiscoveryAddr addr;
        if (!peer->used || peer->dropped) continue;
        i_dart_discovery_addr_of(peer, &addr);
        i_dart_discovery_fire_up(st, peer, &addr);
    }
}

uint16_t dart_discovery_peer_count(const DartDiscoveryState *st){
    uint16_t i, c = 0;   /* live peers only; DROPPED entries linger for resume, not as members */
    for (i=0;i<st->cap_peers;i++) if (st->peers[i].used && !st->peers[i].dropped) c++;
    return c;
}

uint16_t dart_discovery_max_peers(const DartDiscoveryState *st){ return st->cap_peers; }

int dart_discovery_peer_at(const DartDiscoveryState *st, uint16_t slot, DartDiscoveryPeer *out){
    const i_DartDiscoveryPeer *p;
    if (slot >= st->cap_peers) return 0;
    p = &st->peers[slot];
    if (!p->used) return 0;                  /* free slot: DROPPED entries are still "used" */
    memset(out, 0, sizeof *out);
    out->id = p->local_id;
    memcpy(out->uuid, p->uuid, 16);
    i_dart_discovery_addr_of(p, &out->addr);
    out->liveness      = p->dropped ? DART_PEER_DROPPED : DART_PEER_ACTIVE;
    out->last_heard_us = p->last_heard_us;
    out->name          = dart_string(p->name_len ? p->name : NULL, p->name_len);   /* parsed from the blob */
    out->meta          = dart_bytes(p->meta_len ? p->meta : NULL, p->meta_len);
    out->meta_version  = p->meta_version;
    out->adv_meta_version = p->adv_version;
    out->user          = st->user_stride ? p->user : NULL;
    return 1;
}

/* find a used peer (incl. DROPPED) by its local id; NULL if none. */
static i_DartDiscoveryPeer *i_dart_discovery_by_id(const DartDiscoveryState *st, uint32_t id){
    uint16_t i;
    for (i=0;i<st->cap_peers;i++)
        if (st->peers[i].used && st->peers[i].local_id == id) return (i_DartDiscoveryPeer*)&st->peers[i];
    return NULL;
}

void *dart_discovery_peer_user(DartDiscoveryState *st, uint32_t id){
    i_DartDiscoveryPeer *p;
    if (!st || !st->user_stride) return NULL;
    p = i_dart_discovery_by_id(st, id);
    return p ? p->user : NULL;
}

int dart_discovery_addr_of_id(const DartDiscoveryState *st, uint32_t id, DartDiscoveryAddr *out){
    i_DartDiscoveryPeer *p = i_dart_discovery_by_id(st, id);
    if (!p) return 0;
    i_dart_discovery_addr_of(p, out);
    return 1;
}

DartString dart_discovery_peer_name(const DartDiscoveryState *st, uint32_t id){
    i_DartDiscoveryPeer *p = i_dart_discovery_by_id(st, id);
    if (!p) return dart_string(NULL, 0);                 /* unknown peer: .data NULL */
    return dart_string(p->name, p->name_len);            /* known: .data non-NULL, .len may be 0 */
}

int dart_discovery_id_for_addr(const DartDiscoveryState *st, const uint8_t *ip, uint8_t ip_len,
                               uint16_t port, uint32_t *id){
    uint16_t i;
    if (!ip || (ip_len != 4 && ip_len != 16)) return 0;
    for (i=0;i<st->cap_peers;i++){
        const i_DartDiscoveryPeer *p = &st->peers[i];
        if (p->used && p->ip_len == ip_len && p->port == port && memcmp(p->ip, ip, ip_len) == 0){
            if (id) *id = p->local_id;
            return 1;
        }
    }
    return 0;
}

size_t dart_discovery_leave(DartDiscoveryState *st, void *out, size_t cap){
    return i_dart_discovery_build(st, DART_DISCOVERY_FLAG_BYE, 0, (uint8_t *)out, cap);
}

int dart_discovery_peer_addr(const DartDiscoveryState *st, uint16_t slot, DartDiscoveryAddr *out){
    const i_DartDiscoveryPeer *p;
    if (slot >= st->cap_peers) return 0;
    p = &st->peers[slot];
    if (!p->used || p->dropped) return 0;   /* ACTIVE peers only: don't reinforce announces to a
                                               dropped peer (it is silent/dead; the unicast just
                                               bounces, and a churned table fills with such ghosts) */
    i_dart_discovery_addr_of(p, out);
    return 1;
}
#pragma endregion

#ifndef DART_DISCOVERY_SANS_IO
#ifndef DART_PLAT_CUSTOM
#pragma region platform/core.c
/* dart_plat: the Windows + POSIX implementation of the platform contract. This
 * is the only file in DART carrying an OS #ifdef. Port to a new platform by
 * adding a branch here (or a sibling file against dart_plat.h); BSD-socket
 * platforms are already covered. See dart_plat.h. */

/* feature-test macros must precede the first system header (POSIX only) */
#if !defined(_WIN32)
  #ifndef _POSIX_C_SOURCE
  #define _POSIX_C_SOURCE 200809L
  #endif
  #ifndef _DEFAULT_SOURCE
  #define _DEFAULT_SOURCE 1
  #endif
#endif

#include <string.h>
#include <stdlib.h>           /* malloc/realloc/free behind i_dart_plat_realloc */

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <bcrypt.h>            /* BCryptGenRandom (CSPRNG) */
  #include <mmsystem.h>          /* timeBeginPeriod */
  #ifdef _MSC_VER
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "bcrypt.lib")
    #pragma comment(lib, "winmm.lib")
  #endif
  #ifndef SIO_UDP_CONNRESET
  #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
  #endif
  typedef int i_DartSocklen;
  #define DART__FD(s) ((SOCKET)(s))
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #if !defined(ESP_PLATFORM)
    #include <ifaddrs.h>         /* getifaddrs: enumerate local interfaces */
    #include <net/if.h>          /* IFF_UP / IFF_LOOPBACK */
  #endif
  #include <unistd.h>
  #if defined(ESP_PLATFORM)
    #include <sys/poll.h>       /* the ESP (xtensa/riscv) newlib has no <poll.h>; poll() rides the VFS */
  #else
    #include <poll.h>
  #endif
  #include <time.h>
  #ifdef DART_THREADS
    #include <pthread.h>
    #if defined(ESP_PLATFORM)
      #include <freertos/FreeRTOS.h>
      #include <freertos/task.h>   /* xTaskGetCurrentTaskHandle: a task id valid on ANY task */
    #endif
  #endif
  #include <fcntl.h>
  #include <errno.h>
  #include <stdio.h>
  #include <stdlib.h>           /* arc4random_buf on macOS/BSD */
  #if defined(ESP_PLATFORM)
    #include <esp_random.h>     /* esp_fill_random (HW RNG) */
    #include <esp_netif.h>      /* esp_netif_get_ip_info: the interface-IP enumeration */
  #elif defined(__linux__)
    #include <sys/random.h>     /* getrandom(2) */
  #endif
  typedef socklen_t i_DartSocklen;
  #define DART__FD(s) ((int)(s))
#endif

/* ----------------------------------------------------------------- lifecycle */
#ifdef _WIN32
static LARGE_INTEGER i_dart_plat_qpc_freq;   /* set once in startup; lazy fallback */
int i_dart_plat_startup(void){
    WSADATA w;
    /* WSAStartup and timeBeginPeriod are both refcounted by the OS per process,
       so unconditional matched calls are safe from any thread (a hand-rolled
       counter here would be the one racy global in DART). */
    if (WSAStartup(MAKEWORD(2,2), &w) != 0) return 0;
  #ifndef DART_NO_HIGHRES_TIMER
    timeBeginPeriod(1);  /* 1ms timer: default ~15.6ms throttles sub ACK/repair rate */
  #endif
    QueryPerformanceFrequency(&i_dart_plat_qpc_freq);
    return 1;
}
void i_dart_plat_cleanup(void){
  #ifndef DART_NO_HIGHRES_TIMER
    timeEndPeriod(1);
  #endif
    WSACleanup();
}
#else
int  i_dart_plat_startup(void){ return 1; }
void i_dart_plat_cleanup(void){}
#endif

/* --------------------------------------------------------------------- clock */
uint64_t i_dart_plat_now_us(void){
#ifdef _WIN32
    LARGE_INTEGER c;
    if (!i_dart_plat_qpc_freq.QuadPart) QueryPerformanceFrequency(&i_dart_plat_qpc_freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart * 1000000ull) / (uint64_t)i_dart_plat_qpc_freq.QuadPart);
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
#endif
}

/* ------------------------------------------------------------ entropy / host */
int i_dart_plat_random(void *buf, size_t len){
#if defined(_WIN32)
    /* NULL handle selects the system-preferred RNG. 0 == SUCCESS. */
    return BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(ESP_PLATFORM)
    esp_fill_random(buf, len);       /* HW RNG, true random while RF is up */
    return 1;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
      defined(__NetBSD__) || defined(__DragonFly__)
    arc4random_buf(buf, len);        /* CSPRNG, cannot fail */
    return 1;
#else
    {
        uint8_t *p = (uint8_t*)buf; size_t got = 0;
  #if defined(__linux__)
        while (got < len){           /* getrandom(2) */
            ssize_t r = getrandom(p + got, len - got, 0);
            if (r < 0){ if (errno == EINTR) continue; break; }
            got += (size_t)r;
        }
        if (got == len) return 1;
  #endif
        {                            /* fallback: /dev/urandom */
            FILE *f = fopen("/dev/urandom", "rb");
            if (f){
                size_t n = fread(p + got, 1, len - got, f);
                fclose(f);
                if (got + n == len) return 1;
            }
        }
        return 0;
    }
#endif
}

size_t i_dart_plat_hostname(char *buf, size_t cap){
    if (!buf || cap == 0) return 0;
    buf[0] = 0;
    gethostname(buf, (int)cap - 1);
    buf[cap - 1] = 0;
    return strlen(buf);
}

uint64_t i_dart_plat_pid(void){
#ifdef _WIN32
    return (uint64_t)GetCurrentProcessId();
#else
    return (uint64_t)getpid();
#endif
}

/* The one heap dependency, kept behind the platform layer so the node holds no
 * <stdlib.h>: ptr NULL = allocate, size 0 = free (returns NULL), else realloc. */
void *i_dart_plat_realloc(void *ptr, size_t size){
    if (size == 0){ free(ptr); return NULL; }
    return realloc(ptr, size);
}

/* --------------------------------------------------------------- UDP sockets */
i_DartSock i_dart_plat_udp_open(void){
#ifdef _WIN32
    SOCKET fd = socket(AF_INET, SOCK_DGRAM, 0);
    return (fd == INVALID_SOCKET) ? DART_SOCK_BAD : (i_DartSock)fd;
#else
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    return (fd < 0) ? DART_SOCK_BAD : (i_DartSock)fd;
#endif
}

void i_dart_plat_close(i_DartSock s){
    if (s == DART_SOCK_BAD) return;
#ifdef _WIN32
    closesocket((SOCKET)s);
#else
    close((int)s);
#endif
}

int i_dart_plat_bind(i_DartSock s, uint32_t if_naddr, uint16_t port, int reuse){
    struct sockaddr_in a;
    if (reuse){
        int on = 1;
        setsockopt(DART__FD(s), SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);
#ifdef SO_REUSEPORT
        setsockopt(DART__FD(s), SOL_SOCKET, SO_REUSEPORT, (const char*)&on, sizeof on);
#endif
    }
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = if_naddr;            /* 0 == INADDR_ANY */
    a.sin_port = htons(port);
    return bind(DART__FD(s), (struct sockaddr*)&a, sizeof a) == 0;
}

uint16_t i_dart_plat_local_port(i_DartSock s){
    struct sockaddr_in a; i_DartSocklen ll = sizeof a;
    memset(&a, 0, sizeof a);
    if (getsockname(DART__FD(s), (struct sockaddr*)&a, &ll) != 0) return 0;
    return ntohs(a.sin_port);
}

void i_dart_plat_set_nonblock(i_DartSock s){
#ifdef _WIN32
    u_long nb = 1; ioctlsocket((SOCKET)s, FIONBIO, &nb);
#else
    int fl = fcntl((int)s, F_GETFL, 0);
    if (fl != -1) fcntl((int)s, F_SETFL, fl | O_NONBLOCK);
#endif
}

void i_dart_plat_set_rcvbuf(i_DartSock s, int bytes){
    setsockopt(DART__FD(s), SOL_SOCKET, SO_RCVBUF, (const char*)&bytes, sizeof bytes);
}
void i_dart_plat_set_sndbuf(i_DartSock s, int bytes){
    setsockopt(DART__FD(s), SOL_SOCKET, SO_SNDBUF, (const char*)&bytes, sizeof bytes);
}

void i_dart_plat_suppress_connreset(i_DartSock s){
#ifdef _WIN32
    BOOL off = FALSE; DWORD bv = 0;
    WSAIoctl((SOCKET)s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &bv, NULL, NULL);
#else
    (void)s;
#endif
}

/* ----------------------------------------------------------------- multicast */
void i_dart_plat_mcast_setif(i_DartSock s, uint32_t if_naddr){
    setsockopt(DART__FD(s), IPPROTO_IP, IP_MULTICAST_IF, (const char*)&if_naddr, sizeof if_naddr);
}
void i_dart_plat_mcast_ttl(i_DartSock s, uint8_t ttl){
    unsigned char t = ttl;
    setsockopt(DART__FD(s), IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&t, sizeof t);
}
void i_dart_plat_mcast_loop(i_DartSock s, int on){
    unsigned char l = (unsigned char)(on ? 1 : 0);
    setsockopt(DART__FD(s), IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&l, sizeof l);
}
int i_dart_plat_mcast_join(i_DartSock s, uint32_t group_naddr, uint32_t if_naddr){
    struct ip_mreq mr; memset(&mr, 0, sizeof mr);
    mr.imr_multiaddr.s_addr = group_naddr;
    mr.imr_interface.s_addr = if_naddr;
    return setsockopt(DART__FD(s), IPPROTO_IP, IP_ADD_MEMBERSHIP,
                      (const char*)&mr, sizeof mr) == 0;
}

/* --------------------------------------------------------------- datagram IO */
int i_dart_plat_send(i_DartSock s, const void *buf, size_t len,
                   const uint8_t ip[4], uint16_t port){
    struct sockaddr_in d;
    memset(&d, 0, sizeof d);
    d.sin_family = AF_INET;
    memcpy(&d.sin_addr.s_addr, ip, 4);
    d.sin_port = htons(port);
    return (int)sendto(DART__FD(s), (const char*)buf, (int)len, 0,
                       (struct sockaddr*)&d, sizeof d);
}

int i_dart_plat_recv(i_DartSock s, void *buf, size_t cap,
                   uint8_t src_ip[4], uint16_t *src_port){
    struct sockaddr_in src; i_DartSocklen sl = sizeof src;
    int n;
    memset(&src, 0, sizeof src);
    n = (int)recvfrom(DART__FD(s), (char*)buf, (int)cap, 0,
                      (struct sockaddr*)&src, &sl);
#ifdef _WIN32
    /* an oversized datagram: Windows fills the buffer, then fails with WSAEMSGSIZE;
       POSIX silently delivers the prefix. Deliver the prefix here too: discovery reads
       the intact fixed header (which carries the blob's true length) and turns the
       truncation into a grow-and-refetch instead of a hard recv error. */
    if (n < 0 && WSAGetLastError() == WSAEMSGSIZE) n = (int)cap;
#endif
    if (n > 0){
        if (src_ip)   memcpy(src_ip, &src.sin_addr.s_addr, 4);
        if (src_port) *src_port = ntohs(src.sin_port);
    }
    return n;
}

int i_dart_plat_would_block(void){
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

int i_dart_plat_last_socket_error(void){
#ifdef _WIN32
    return WSAGetLastError();   /* winsock keeps its error off errno */
#else
    return errno;
#endif
}

int i_dart_plat_poll(i_DartPollfd *fds, int n, int timeout_ms){
    /* callers poll one or two sockets; cap the on-stack translation buffer */
#ifdef _WIN32
    WSAPOLLFD p[8];
#else
    struct pollfd p[8];
#endif
    int i, r;
    if (n < 0) return -1;
    if (n > 8) n = 8;
    memset(p, 0, sizeof p);
    for (i = 0; i < n; i++){
        p[i].fd = DART__FD(fds[i].fd);
        p[i].events = (short)((fds[i].events & DART_POLLIN) ? POLLIN : 0);
    }
#ifdef _WIN32
    r = WSAPoll(p, (ULONG)n, timeout_ms);
#else
    r = poll(p, (nfds_t)n, timeout_ms);
#endif
    for (i = 0; i < n; i++)
        fds[i].revents = (short)((p[i].revents & POLLIN) ? DART_POLLIN : 0);
    return r;
}

/* ----------------------------------------------------------- address helpers */
uint32_t i_dart_plat_parse_ip(const char *dotted){
    return dotted ? (uint32_t)inet_addr(dotted) : 0;
}
uint32_t i_dart_plat_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d){
    return htonl(((uint32_t)a << 24) | ((uint32_t)b << 16) |
                 ((uint32_t)c << 8)  |  (uint32_t)d);
}
uint32_t i_dart_plat_ip4_to_naddr(const uint8_t ip[4]){
    uint32_t n; memcpy(&n, ip, 4); return n;
}
void i_dart_plat_naddr_to_ip4(uint32_t naddr, uint8_t out[4]){
    memcpy(out, &naddr, 4);
}

uint32_t i_dart_plat_route_src(uint32_t dst_naddr, uint16_t port){
    i_DartSock s = i_dart_plat_udp_open();
    struct sockaddr_in a;
    uint32_t ip = 0;                         /* INADDR_ANY on failure */
    if (s == DART_SOCK_BAD) return ip;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = dst_naddr; a.sin_port = htons(port);
    if (connect(DART__FD(s), (struct sockaddr*)&a, sizeof a) == 0){
        struct sockaddr_in loc; i_DartSocklen ll = sizeof loc;
        if (getsockname(DART__FD(s), (struct sockaddr*)&loc, &ll) == 0)
            ip = loc.sin_addr.s_addr;
    }
    i_dart_plat_close(s);
    return ip;
}

#if defined(_WIN32)
/* SIO_GET_INTERFACE_LIST flag values (mirrors the BSD IFF_* bits) if the SDK's
 * headers didn't define them for this WSAIoctl. */
#ifndef IFF_UP
#define IFF_UP 0x00000001
#endif
#ifndef IFF_LOOPBACK
#define IFF_LOOPBACK 0x00000004
#endif
int i_dart_plat_local_ipv4s(uint32_t *out, int max){
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    INTERFACE_INFO info[32];
    DWORD bytes = 0;
    int n = 0, i, count;
    if (s == INVALID_SOCKET || !out || max <= 0){ if (s != INVALID_SOCKET) closesocket(s); return 0; }
    if (WSAIoctl(s, SIO_GET_INTERFACE_LIST, NULL, 0, info, sizeof info, &bytes, NULL, NULL) != 0){
        closesocket(s); return 0;
    }
    closesocket(s);
    count = (int)(bytes / sizeof(INTERFACE_INFO));
    for (i = 0; i < count && n < max; i++){
        u_long flags = info[i].iiFlags;
        struct sockaddr_in *a = &info[i].iiAddress.AddressIn;
        if (!(flags & IFF_UP) || (flags & IFF_LOOPBACK)) continue;
        if (a->sin_family != AF_INET) continue;
        out[n++] = a->sin_addr.s_addr;
    }
    return n;
}
#elif defined(ESP_PLATFORM)
/* lwIP connect() does not assign a local source address, so the route probes in
 * dart_discovery_mcast_if_for come back empty on the ESP. This backstop hands the
 * multicast join a concrete interface (the STA / Ethernet / SoftAP IP) from
 * esp_netif; without it the join lands on INADDR_ANY, which lwIP refuses, and
 * dart_node_open fails. Requires WiFi/Ethernet already up (the node opens after). */
int i_dart_plat_local_ipv4s(uint32_t *out, int max){
    static const char *const keys[] = { "WIFI_STA_DEF", "ETH_DEF", "WIFI_AP_DEF" };
    int n = 0; unsigned i;
    if (!out || max <= 0) return 0;
    for (i = 0; i < sizeof keys / sizeof keys[0] && n < max; i++){
        esp_netif_t *nif = esp_netif_get_handle_from_ifkey(keys[i]);
        esp_netif_ip_info_t info;
        if (nif && esp_netif_get_ip_info(nif, &info) == ESP_OK && info.ip.addr != 0)
            out[n++] = info.ip.addr;   /* esp_ip4_addr is network-order: our naddr convention */
    }
    return n;
}
#else
int i_dart_plat_local_ipv4s(uint32_t *out, int max){
    struct ifaddrs *ifs = NULL, *p;
    int n = 0;
    if (!out || max <= 0 || getifaddrs(&ifs) != 0) return 0;
    for (p = ifs; p && n < max; p = p->ifa_next){
        struct sockaddr_in *a;
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
        a = (struct sockaddr_in*)p->ifa_addr;
        out[n++] = a->sin_addr.s_addr;
    }
    freeifaddrs(ifs);
    return n;
}
#endif

/* ------------------------------------------------------------------- threads */
#ifdef DART_THREADS

/* The opaque header blobs must fit the real OS types (C99: no _Static_assert). */
#define DART__FITS(name, real, blob) \
    typedef char name[(sizeof(real) <= sizeof(blob)) ? 1 : -1]

#ifdef _WIN32

typedef struct { HANDLE h; void (*fn)(void *); void *arg; } i_DartThreadImpl;
DART__FITS(i_dart_plat_mutex_fits,  SRWLOCK,            i_DartMutex);
DART__FITS(i_dart_plat_cond_fits,   CONDITION_VARIABLE, i_DartCond);
DART__FITS(i_dart_plat_thread_fits, i_DartThreadImpl,   i_DartThread);

static DWORD WINAPI i_dart_plat_thread_tramp(LPVOID p){
    i_DartThreadImpl *t = (i_DartThreadImpl *)p;
    t->fn(t->arg);
    return 0;
}
int i_dart_plat_thread_start(i_DartThread *t, void (*fn)(void *), void *arg){
    i_DartThreadImpl *ti = (i_DartThreadImpl *)t;
    ti->fn = fn; ti->arg = arg;
    ti->h = CreateThread(NULL, 0, i_dart_plat_thread_tramp, ti, 0, NULL);
    return ti->h != NULL;
}
void i_dart_plat_thread_join(i_DartThread *t){
    i_DartThreadImpl *ti = (i_DartThreadImpl *)t;
    if (!ti->h) return;
    WaitForSingleObject(ti->h, INFINITE);
    CloseHandle(ti->h);
    ti->h = NULL;
}
uint64_t i_dart_plat_thread_id(void){ return (uint64_t)GetCurrentThreadId(); }

/* SRWLOCK over CRITICAL_SECTION: pointer-sized, pairs with condvars, and
   deliberately non-recursive (the condvar contract requires it; the node's
   reentrancy is an owner-id check above this layer). */
void i_dart_plat_mutex_init   (i_DartMutex *m){ InitializeSRWLock((PSRWLOCK)m); }
void i_dart_plat_mutex_destroy(i_DartMutex *m){ (void)m; }
void i_dart_plat_mutex_lock   (i_DartMutex *m){ AcquireSRWLockExclusive((PSRWLOCK)m); }
void i_dart_plat_mutex_unlock (i_DartMutex *m){ ReleaseSRWLockExclusive((PSRWLOCK)m); }

void i_dart_plat_cond_init   (i_DartCond *c){ InitializeConditionVariable((PCONDITION_VARIABLE)c); }
void i_dart_plat_cond_destroy(i_DartCond *c){ (void)c; }
void i_dart_plat_cond_wait(i_DartCond *c, i_DartMutex *m, uint32_t timeout_us){
    /* 64-bit round-up: (0xFFFFFFFF + 999) would wrap in 32 bits and turn the
       longest waits into 1 ms spins */
    DWORD ms = (DWORD)(((uint64_t)timeout_us + 999u) / 1000u);
    SleepConditionVariableSRW((PCONDITION_VARIABLE)c, (PSRWLOCK)m, ms ? ms : 1, 0);
}
void i_dart_plat_cond_broadcast(i_DartCond *c){ WakeAllConditionVariable((PCONDITION_VARIABLE)c); }

#else /* POSIX */

typedef struct { pthread_t t; void (*fn)(void *); void *arg; } i_DartThreadImpl;
DART__FITS(i_dart_plat_mutex_fits,  pthread_mutex_t,  i_DartMutex);
DART__FITS(i_dart_plat_cond_fits,   pthread_cond_t,   i_DartCond);
DART__FITS(i_dart_plat_thread_fits, i_DartThreadImpl, i_DartThread);

static void *i_dart_plat_thread_tramp(void *p){
    i_DartThreadImpl *t = (i_DartThreadImpl *)p;
    t->fn(t->arg);
    return NULL;
}
int i_dart_plat_thread_start(i_DartThread *t, void (*fn)(void *), void *arg){
    i_DartThreadImpl *ti = (i_DartThreadImpl *)t;
    ti->fn = fn; ti->arg = arg;
    return pthread_create(&ti->t, NULL, i_dart_plat_thread_tramp, ti) == 0;
}
void i_dart_plat_thread_join(i_DartThread *t){
    pthread_join(((i_DartThreadImpl *)t)->t, NULL);
}
uint64_t i_dart_plat_thread_id(void){
#if defined(ESP_PLATFORM)
    /* pthread_self() ABORTS on ESP-IDF when called from a task not created by
       pthread_create (e.g. the Arduino loopTask that drives poll). The task
       handle is a unique per-task id valid on both native tasks and pthreads. */
    return (uint64_t)(uintptr_t)xTaskGetCurrentTaskHandle();
#else
    return (uint64_t)(uintptr_t)pthread_self();
#endif
}

void i_dart_plat_mutex_init   (i_DartMutex *m){ pthread_mutex_init((pthread_mutex_t *)m, NULL); }
void i_dart_plat_mutex_destroy(i_DartMutex *m){ pthread_mutex_destroy((pthread_mutex_t *)m); }
void i_dart_plat_mutex_lock   (i_DartMutex *m){ pthread_mutex_lock((pthread_mutex_t *)m); }
void i_dart_plat_mutex_unlock (i_DartMutex *m){ pthread_mutex_unlock((pthread_mutex_t *)m); }

void i_dart_plat_cond_init(i_DartCond *c){
#if defined(__linux__)
    /* wait on the monotonic clock so a wall-clock step cannot stretch a timeout */
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init((pthread_cond_t *)c, &a);
    pthread_condattr_destroy(&a);
#else
    pthread_cond_init((pthread_cond_t *)c, NULL);
#endif
}
void i_dart_plat_cond_destroy(i_DartCond *c){ pthread_cond_destroy((pthread_cond_t *)c); }
void i_dart_plat_cond_wait(i_DartCond *c, i_DartMutex *m, uint32_t timeout_us){
#if defined(__APPLE__)
    struct timespec rel;
    rel.tv_sec  = (time_t)(timeout_us / 1000000u);
    rel.tv_nsec = (long)(timeout_us % 1000000u) * 1000L;
    pthread_cond_timedwait_relative_np((pthread_cond_t *)c, (pthread_mutex_t *)m, &rel);
#else
    /* Linux: monotonic (set at init). Elsewhere: realtime; a wall-clock jump can
       cut the wait short, which the caller's predicate-plus-deadline loop absorbs. */
    struct timespec ts;
  #if defined(__linux__)
    clock_gettime(CLOCK_MONOTONIC, &ts);
  #else
    clock_gettime(CLOCK_REALTIME, &ts);
  #endif
    ts.tv_sec  += (time_t)(timeout_us / 1000000u);
    ts.tv_nsec += (long)(timeout_us % 1000000u) * 1000L;
    if (ts.tv_nsec >= 1000000000L){ ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait((pthread_cond_t *)c, (pthread_mutex_t *)m, &ts);
#endif
}
void i_dart_plat_cond_broadcast(i_DartCond *c){ pthread_cond_broadcast((pthread_cond_t *)c); }

#endif /* _WIN32 */

/* Waker: platform-neutral over the socket helpers above. Bound to loopback and
   connected to itself, so only its own 1-byte signals ever arrive. */
int i_dart_plat_waker_open(i_DartWaker *w){
    struct sockaddr_in a; i_DartSocklen al = sizeof a;
    w->fd = i_dart_plat_udp_open();
    if (w->fd == DART_SOCK_BAD) return 0;
    memset(&a, 0, sizeof a);
    if (!i_dart_plat_bind(w->fd, i_dart_plat_ipv4(127, 0, 0, 1), 0, 0)) goto fail;
    if (getsockname(DART__FD(w->fd), (struct sockaddr *)&a, &al) != 0) goto fail;
    if (connect(DART__FD(w->fd), (struct sockaddr *)&a, sizeof a) != 0) goto fail;
    i_dart_plat_set_nonblock(w->fd);
    return 1;
fail:
    i_dart_plat_close(w->fd);
    w->fd = DART_SOCK_BAD;
    return 0;
}
int i_dart_plat_waker_signal(i_DartWaker *w){
    char b = 1;
    if (w->fd == DART_SOCK_BAD) return 0;
    return (int)send(DART__FD(w->fd), &b, 1, 0) == 1;
}
void i_dart_plat_waker_drain(i_DartWaker *w){
    char b[64];
    if (w->fd == DART_SOCK_BAD) return;
    while (recv(DART__FD(w->fd), b, sizeof b, 0) > 0) {}
}
void i_dart_plat_waker_close(i_DartWaker *w){
    i_dart_plat_close(w->fd);
    w->fd = DART_SOCK_BAD;
}

#endif /* DART_THREADS */

/* ------------------------------------------------------------- shared memory */
#ifdef DART_SHM
#ifndef _WIN32
  #include <sys/mman.h>            /* shm_open/mmap; fcntl/unistd/stdlib already in */
  #include <sys/stat.h>            /* fstat: a reader learns a segment's size from the OS */
#endif

/* 128-bit non-cryptographic id from a byte string: two FNV-1a passes with distinct
 * seeds. Stable per input, enough to pre-filter same-host (attach is the real gate). */
static void i_dart_plat_hash16(const void *data, size_t len, uint8_t out[16]){
    const uint8_t *p = (const uint8_t*)data; size_t i;
    uint64_t a = 14695981039346656037ull, b = 1099511628211ull;
    for (i = 0; i < len; i++){
        a = (a ^ p[i]) * 1099511628211ull;
        b = (b ^ (uint8_t)(p[i] + 0x9Eu)) * 1099511628211ull;
    }
    for (i = 0; i < 8; i++){ out[i] = (uint8_t)(a >> (8*i)); out[8+i] = (uint8_t)(b >> (8*i)); }
}

void i_dart_plat_host_uuid(uint8_t out[16]){
#if defined(__linux__)
    FILE *f = fopen("/etc/machine-id", "rb");   /* 32 hex chars = a 128-bit id */
    if (f){
        char hx[32]; size_t n = fread(hx, 1, sizeof hx, f); int i, ok = (n == 32);
        fclose(f);
        for (i = 0; ok && i < 16; i++){
            int hi = hx[2*i], lo = hx[2*i+1];
            hi = (hi>='0'&&hi<='9')?hi-'0':(hi>='a'&&hi<='f')?hi-'a'+10:(hi>='A'&&hi<='F')?hi-'A'+10:-1;
            lo = (lo>='0'&&lo<='9')?lo-'0':(lo>='a'&&lo<='f')?lo-'a'+10:(lo>='A'&&lo<='F')?lo-'A'+10:-1;
            if (hi < 0 || lo < 0) ok = 0; else out[i] = (uint8_t)((hi<<4)|lo);
        }
        if (ok) return;
    }
#endif
    {   char host[256]; size_t n = i_dart_plat_hostname(host, sizeof host);
        if (n == 0){ host[0] = '?'; n = 1; }
        i_dart_plat_hash16(host, n, out);
    }
}

#ifdef _WIN32
void *i_dart_plat_shm_create(const char *name, size_t bytes, void **handle){
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                  (DWORD)((uint64_t)bytes >> 32),
                                  (DWORD)(bytes & 0xFFFFFFFFu), name);
    void *base;
    if (!h) return NULL;
    base = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
    if (!base){ CloseHandle(h); return NULL; }
    *handle = h;
    return base;
}
void *i_dart_plat_shm_attach(const char *name, size_t *out_bytes, void **handle){
    HANDLE h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    void *base; MEMORY_BASIC_INFORMATION mbi;
    if (!h) return NULL;
    base = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);   /* 0 = the whole section */
    if (!base){ CloseHandle(h); return NULL; }
    if (out_bytes) *out_bytes = VirtualQuery(base, &mbi, sizeof mbi) ? (size_t)mbi.RegionSize : 0;
    *handle = h;
    return base;
}
void i_dart_plat_shm_detach(void *base, size_t bytes, void *handle, int unlink_it){
    (void)bytes; (void)unlink_it;   /* the object dies when the last handle closes */
    if (base) UnmapViewOfFile(base);
    if (handle) CloseHandle((HANDLE)handle);
}
uint64_t i_dart_plat_atomic_load64(volatile uint64_t *p){
    return (uint64_t)InterlockedCompareExchange64((volatile LONGLONG*)p, 0, 0);
}
void i_dart_plat_atomic_store64(volatile uint64_t *p, uint64_t v){
    InterlockedExchange64((volatile LONGLONG*)p, (LONGLONG)v);
}
#else /* POSIX */
void *i_dart_plat_shm_create(const char *name, size_t bytes, void **handle){
    int fd = shm_open(name, O_CREAT|O_RDWR, 0600);
    void *base; char *nm;
    if (fd < 0) return NULL;
    if (ftruncate(fd, (off_t)bytes) != 0){ close(fd); shm_unlink(name); return NULL; }
    base = mmap(NULL, bytes, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);                                  /* the mapping outlives the fd */
    if (base == MAP_FAILED){ shm_unlink(name); return NULL; }
    nm = (char*)malloc(strlen(name) + 1);       /* carry the name for shm_unlink */
    if (nm) strcpy(nm, name);
    *handle = nm;
    return base;
}
void *i_dart_plat_shm_attach(const char *name, size_t *out_bytes, void **handle){
    int fd = shm_open(name, O_RDWR, 0600);
    void *base; struct stat st;
    if (fd < 0) return NULL;
    if (fstat(fd, &st) != 0 || st.st_size <= 0){ close(fd); return NULL; }
    base = mmap(NULL, (size_t)st.st_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED) return NULL;
    if (out_bytes) *out_bytes = (size_t)st.st_size;
    *handle = NULL;                             /* a reader never unlinks */
    return base;
}
void i_dart_plat_shm_detach(void *base, size_t bytes, void *handle, int unlink_it){
    if (base && base != MAP_FAILED) munmap(base, bytes);
    if (handle){
        if (unlink_it) shm_unlink((const char*)handle);
        free(handle);
    }
}
uint64_t i_dart_plat_atomic_load64(volatile uint64_t *p){
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
void i_dart_plat_atomic_store64(volatile uint64_t *p, uint64_t v){
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
#endif /* _WIN32 */
#endif /* DART_SHM */
#pragma endregion
#endif /* !DART_PLAT_CUSTOM */
#pragma region discovery/runtime.c
/* peer-discovery runtime: the one-tick loop over the dart_discovery core, plus
 * UUID generation. All OS access goes through dart_plat. See discovery/runtime.h. */

#include <string.h>

#define DART_DISCOVERY_BYE_SENDS 3   /* a graceful BYE is one-shot UDP: resend a few (idempotent,
                                        deduped by uuid) so one lands even under loss on exit */

struct DartDiscovery {
    DartDiscoveryState *core;
    i_DartSock            fd;
    i_DartSock            unicast_fd;    /* own same-host unicast RX on a unique port, or
                                            DART_SOCK_BAD when the caller gave a data_port */
    uint32_t             group_naddr;   /* discovery multicast group, network order */
    uint16_t             discovery_port;
    uint16_t             max_peers;
    uint32_t             wire_max;    /* scratch buffer size = META_OFF + meta_capacity */
    uint8_t             *rxbuf;       /* arena, wire_max */
    uint8_t             *txbuf;       /* arena, wire_max */
    DartDiscoveryPeer   *peer_view;   /* arena, [max_peers]: zero-copy snapshot for dart_discovery_peers */
    DartAllocator       pool;        /* dart_discovery_open's allocator (owns the block; reset at close).
                                         Empty (zeroed) for dart_discovery_place: the caller owns the memory. */
    DartDiscoveryAddr  seeds[DART_DISCOVERY_MAX_SEEDS];
    uint16_t             n_seeds;
};

static void i_dart_discovery_tx1(DartDiscovery *d, const uint8_t *out, size_t n_bytes,
                          const uint8_t ip[4], uint16_t port){
    i_dart_plat_send(d->fd, out, n_bytes, ip, port);
}

/* unicast one datagram to a peer: at the discovery port, and at its data port too (the
 * only per-process address when processes share the discovery port; the data-socket owner
 * forwards discovery datagrams to its core). */
static void i_dart_discovery_tx_to(DartDiscovery *d, const uint8_t *out, size_t n_bytes,
                          const DartDiscoveryAddr *addr){
    if (addr->ip_len != 4) return;
    i_dart_discovery_tx1(d, out, n_bytes, addr->ip, d->discovery_port);
    if (addr->port && addr->port != d->discovery_port) i_dart_discovery_tx1(d, out, n_bytes, addr->ip, addr->port);
}

/* send to the group, every seed, and every known peer. Survives multicast outages;
 * receivers dedup by uuid. */
static void i_dart_discovery_tx(DartDiscovery *d, const uint8_t *out, size_t n_bytes){
    uint16_t s, discovery_port = d->discovery_port;
    DartDiscoveryAddr addr;
    uint8_t group_ip[4];
    i_dart_plat_naddr_to_ip4(d->group_naddr, group_ip);
    i_dart_plat_send(d->fd, out, n_bytes, group_ip, discovery_port);
    for (s=0; s<d->n_seeds; s++){
        const DartDiscoveryAddr *seed = &d->seeds[s];
        if (seed->ip_len != 4) continue;
        i_dart_discovery_tx1(d, out, n_bytes, seed->ip, seed->port ? seed->port : discovery_port);
    }
    for (s=0; s<d->max_peers; s++){
        if (!dart_discovery_peer_addr(d->core, s, &addr)) continue;
        i_dart_discovery_tx_to(d, out, n_bytes, &addr);
    }
}

void dart_discovery_feed(DartDiscovery *d, const uint8_t *src_ip, uint8_t src_ip_len,
                    DartBytes datagram){
    if (!d) return;
    dart_discovery_on_datagram(d->core, src_ip, src_ip_len, datagram, i_dart_plat_now_us());
}

void dart_discovery_advertise(DartDiscovery *d, DartBytes meta){
    if (d) dart_discovery_set_meta(d->core, meta);
}

void dart_discovery_replay(DartDiscovery *d){
    if (d) dart_discovery_replay_peers(d->core);
}

int dart_discovery_pollfds(DartDiscovery *d, i_DartSock out[2]){
    int n = 0;
    if (!d) return 0;
    out[n++] = d->fd;
    if (d->unicast_fd != DART_SOCK_BAD) out[n++] = d->unicast_fd;
    return n;
}

/* A loopback (127/8) or unspecified address is never a usable multicast egress. */
static int i_dart_discovery_if_routable(uint32_t naddr){
    uint8_t ip[4];
    if (!naddr) return 0;
    i_dart_plat_naddr_to_ip4(naddr, ip);
    return ip[0] != 127;
}
/* Link-local autoconfig (169.254/16): an adapter with no DHCP lease. Usable only as a
 * last resort, behind any properly addressed LAN interface. */
static int i_dart_discovery_if_apipa(uint32_t naddr){
    uint8_t ip[4];
    i_dart_plat_naddr_to_ip4(naddr, ip);
    return ip[0] == 169 && ip[1] == 254;
}

/* Every multicast send and join should pin to this one interface. Auto path (no
 * explicit --if): route-probe the group, which is correct on POSIX. But Windows
 * source-address selection for a multicast destination can resolve to loopback, which
 * would strand all traffic on 127.0.0.1; so when the group probe isn't a routable
 * address, probe the default route via a reserved global-unicast destination (connect()
 * on UDP sends nothing) to get the LAN interface, then finally enumerate and prefer a
 * real (non-APIPA) LAN address. Returns 0 only if nothing usable was found (the OS then
 * picks the default interface). */
uint32_t dart_discovery_mcast_if_for(uint32_t group_naddr, uint16_t port){
    uint32_t ip, ifs[16];
    int n, i, apipa = -1;

    ip = i_dart_plat_route_src(group_naddr, port);
    if (i_dart_discovery_if_routable(ip)) return ip;

    /* 192.0.2.1 is TEST-NET-1 (RFC 5737): never a real host, so this resolves only the
     * default-route source interface; no datagram is transmitted. */
    ip = i_dart_plat_route_src(i_dart_plat_ipv4(192u, 0u, 2u, 1u), port);
    if (i_dart_discovery_if_routable(ip)) return ip;

    n = i_dart_plat_local_ipv4s(ifs, (int)(sizeof ifs / sizeof ifs[0]));
    for (i = 0; i < n; i++){
        if (!i_dart_discovery_if_routable(ifs[i])) continue;
        if (i_dart_discovery_if_apipa(ifs[i])){ if (apipa < 0) apipa = i; continue; }
        return ifs[i];                 /* first real (DHCP/static) LAN address */
    }
    return apipa >= 0 ? ifs[apipa] : 0;  /* APIPA only if nothing better; else OS default */
}

int dart_discovery_make_uuid4(uint8_t out[16]){
    if (!i_dart_plat_random(out, 16)) return 0;
    out[6] = (uint8_t)((out[6] & 0x0Fu) | 0x40u);  /* version 4 */
    out[8] = (uint8_t)((out[8] & 0x3Fu) | 0x80u);  /* variant 10x */
    return 1;
}

static void i_dart_discovery_auto_uuid(uint8_t out[16]){
    char host[80]; uint64_t seed; size_t hostname_len;
    if (dart_discovery_make_uuid4(out)) return;     /* normal path */

    /* No CSPRNG: derive a best-effort unique id from hostname, pid and clock. */
    hostname_len = i_dart_plat_hostname(host, sizeof host);
    seed = (i_dart_plat_pid() << 32) ^ i_dart_plat_now_us();
    dart_discovery_make_uuid(out, dart_bytes(host, hostname_len), seed);
}

uint8_t dart_discovery_default_name(char *out, size_t cap, const char *want){
    static const char hex[] = "0123456789abcdef";
    uint32_t r; size_t i, max;
    if (!out || cap == 0) return 0;
    max = cap - 1;
    if (max > DART_DISCOVERY_NAME_MAX) max = DART_DISCOVERY_NAME_MAX;
    if (want && *want){                              /* the caller's name, clamped */
        for (i = 0; i < max && want[i]; i++) out[i] = want[i];
        out[i] = '\0';
        return (uint8_t)i;
    }
    if (max < 13){ out[0] = '\0'; return 0; }        /* no room for "node-XXXXXXXX" */
    if (!i_dart_plat_random(&r, sizeof r)) r = (uint32_t)i_dart_plat_pid();
    memcpy(out, "node-", 5);                          /* auto: "node-" + 8 hex (random, pid fallback) */
    for (i = 0; i < 8; i++) out[5+i] = hex[(r >> ((7-i)*4)) & 0xF];
    out[13] = '\0';
    return 13;
}

/* Single source of the discovery-runtime arena layout: the d struct, the rx/tx wire
   scratch buffers, the zero-copy peer view, then the discovery-core sub-arena. measure
   feeds placement_memory; build feeds place -- one definition. */
typedef struct {
    DartDiscovery *d;
    uint8_t *rxbuf, *txbuf, *peer_view, *core;
    size_t   wire_max, core_bytes;
} i_DartRtBlocks;
static void i_dart_discovery_rt_layout(i_DartBump *b, const DartDiscoveryCoreConfig *c, i_DartRtBlocks *o){
    uint16_t max_peers = c->max_peers ? c->max_peers : 32u;
    o->wire_max   = dart_discovery_wire_size(c->meta_capacity);
    o->d     = (DartDiscovery*)i_dart_bump_take(b, sizeof(struct DartDiscovery), 16);
    o->rxbuf = (uint8_t*)i_dart_bump_take(b, o->wire_max, 16);
    o->txbuf = (uint8_t*)i_dart_bump_take(b, o->wire_max, 16);
    o->peer_view = (uint8_t*)i_dart_bump_take(b, (size_t)max_peers * sizeof(DartDiscoveryPeer), 16);
    o->core_bytes = dart_discovery_required_memory(c);
    o->core  = (uint8_t*)i_dart_bump_take(b, o->core_bytes, 16);
}

size_t dart_discovery_placement_memory(const DartDiscoveryNetConfig *cfg){
    DartDiscoveryCoreConfig c; i_DartBump b; i_DartRtBlocks blk;
    if (!cfg) return 0;
    c = cfg->discovery;
    dart_discovery_config_defaults(&c);
    memset(&b, 0, sizeof b);
    i_dart_discovery_rt_layout(&b, &c, &blk);
    return b.offset + 16u;     /* slack to align the caller's mem up to base */
}

/* Why the most recent open/place returned NULL (best-effort process-globals, no lock:
   meaningful right after a NULL return). The node reads these to build its DART_ERROR. */
static DartDiscoveryPlaceError g_place_error = DART_DISCOVERY_OK;
static int                     g_place_os_error = 0;
static DartDiscovery *i_dart_discovery_fail(DartDiscoveryPlaceError e, int os_error){
    g_place_error = e; g_place_os_error = os_error;
    return NULL;
}
DartDiscoveryPlaceError dart_discovery_last_error(void){ return g_place_error; }
int                     dart_discovery_last_os_error(void){ return g_place_os_error; }

DartDiscovery *dart_discovery_place(void *mem, size_t cap, const DartDiscoveryNetConfig *cfg){
    DartDiscoveryNetConfig c;
    DartDiscovery *d;
    uint8_t *base, *core_mem;
    size_t need;
    i_DartRtBlocks blk;
    i_DartSock fd;
    int allzero = 1, i;
    uint8_t ttl;
    uint32_t group_naddr, interface_ip;
    const char *group;

    if (!mem || !cfg) return NULL;
    g_place_error = DART_DISCOVERY_OK; g_place_os_error = 0;
    c = *cfg;
    dart_discovery_config_defaults(&c.discovery);
    group     = c.group     ? c.group     : "239.255.0.7";
    if (c.discovery_port == 0)    c.discovery_port  = 7400;
    ttl  = c.ttl ? c.ttl : 1;

    need = dart_discovery_placement_memory(&c);
    if (cap < need) return i_dart_discovery_fail(DART_DISCOVERY_E_MEMORY, 0);

    base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    {   i_DartBump b; memset(&b, 0, sizeof b);
        b.base = base; b.cap = cap - (size_t)(base - (uint8_t*)mem);
        i_dart_discovery_rt_layout(&b, &c.discovery, &blk); }
    d = blk.d;
    d->wire_max  = (uint32_t)blk.wire_max;
    d->rxbuf     = blk.rxbuf;
    d->txbuf     = blk.txbuf;
    d->peer_view = (DartDiscoveryPeer*)blk.peer_view;
    memset(&d->pool, 0, sizeof d->pool); /* caller owns mem; the open path overwrites this */
    core_mem     = blk.core;

    /* auto-generate a UUID if the caller left it zero */
    for (i=0;i<16;i++) if (c.discovery.uuid[i]) { allzero = 0; break; }

    if (!i_dart_plat_startup()) return i_dart_discovery_fail(DART_DISCOVERY_E_PLATFORM, 0);
    if (allzero) i_dart_discovery_auto_uuid(c.discovery.uuid);

    d->core = dart_discovery_init(core_mem, cap - (size_t)(core_mem - (uint8_t*)mem), &c.discovery);
    if (!d->core){ i_dart_plat_cleanup(); return i_dart_discovery_fail(DART_DISCOVERY_E_MEMORY, 0); }

    fd = i_dart_plat_udp_open();
    if (fd == DART_SOCK_BAD){ int e=i_dart_plat_last_socket_error(); i_dart_plat_cleanup(); return i_dart_discovery_fail(DART_DISCOVERY_E_SOCKET, e); }
    if (!i_dart_plat_bind(fd, 0, c.discovery_port, 1)){ int e=i_dart_plat_last_socket_error(); i_dart_plat_close(fd); i_dart_plat_cleanup(); return i_dart_discovery_fail(DART_DISCOVERY_E_BIND, e); }

    /* pin join and egress to one deterministic interface */
    group_naddr = i_dart_plat_parse_ip(group);
    interface_ip = c.multicast_interface ? i_dart_plat_parse_ip(c.multicast_interface)
                      : dart_discovery_mcast_if_for(group_naddr, c.discovery_port);
    if (!i_dart_plat_mcast_join(fd, group_naddr, interface_ip)){
        int e=i_dart_plat_last_socket_error();
        i_dart_plat_close(fd); i_dart_plat_cleanup(); return i_dart_discovery_fail(DART_DISCOVERY_E_MCAST_JOIN, e);
    }
    i_dart_plat_mcast_setif(fd, interface_ip);
    i_dart_plat_mcast_ttl(fd, ttl);
    /* loop always on (uuid self-filter drops echoes); needed for multi-instance per host */
    i_dart_plat_mcast_loop(fd, 1);
    i_dart_plat_set_nonblock(fd);   /* poll then DRAIN to empty (i_dart_discovery_rt_drain): the recv that
                                     finds the queue empty must return would-block, not block */
    i_dart_plat_suppress_connreset(fd);   /* we reinforce announces by unicast to known peers; a
                                           peer that died bounces an ICMP unreachable that would
                                           otherwise surface as WSAECONNRESET and disrupt RX */

    d->fd = fd;
    d->unicast_fd = DART_SOCK_BAD;
    d->group_naddr = group_naddr;
    d->discovery_port = c.discovery_port;
    d->max_peers = c.discovery.max_peers;
    d->n_seeds = 0;
    if (c.seeds){
        uint16_t k, seed_count = c.n_seeds;
        if (seed_count > DART_DISCOVERY_MAX_SEEDS) seed_count = DART_DISCOVERY_MAX_SEEDS;
        for (k=0;k<seed_count;k++) d->seeds[k] = c.seeds[k];
        d->n_seeds = seed_count;
    }

    /* If the caller advertised no unicast locator (data_port 0: a discovery-only instance
       with no transport socket to multiplex discovery onto), bind our own unicast RX socket
       on a unique ephemeral port and advertise it. A same-host peer's unicast reply (a
       solicit answer or a re-fetch) then reaches THIS process instead of the shared discovery
       port, which the OS hands to one arbitrary same-port socket. Best-effort: on failure we
       keep data_port 0 and only cross-host unicast (where we own the discovery port) works. */
    if (c.discovery.data_port == 0){
        i_DartSock uc = i_dart_plat_udp_open();
        if (uc != DART_SOCK_BAD){
            uint16_t uport = i_dart_plat_bind(uc, 0, 0, 0) ? i_dart_plat_local_port(uc) : 0;
            if (uport){
                i_dart_plat_set_nonblock(uc);
                i_dart_plat_suppress_connreset(uc);   /* same: a bounced unicast must not disrupt RX */
                d->unicast_fd = uc;
                dart_discovery_set_data_port(d->core, uport);
            } else i_dart_plat_close(uc);
        }
    }
    return d;
}

/* Open a discovery runtime that owns its memory via a DartAllocator (defaults-first;
 * mirrors dart_node_open). Translates the flat opts into the placement config, sizes,
 * allocates, and places the runtime; dart_discovery_close frees the heap block. No
 * automatic growth: a full peer table refuses rather than relocating. */
DartDiscovery *dart_discovery_open(DartAllocator *alloc, const char *name, const DartDiscoveryConfig *cfg){
    DartDiscoveryNetConfig nc; DartDiscoveryConfig o; DartDiscovery *d;
    char namebuf[DART_DISCOVERY_NAME_MAX + 1]; uint8_t namelen;
    void *block; size_t need; DartAllocator pool;
    if (!alloc) return NULL;
    memset(&o, 0, sizeof o); if (cfg) o = *cfg;
    namelen = dart_discovery_default_name(namebuf, sizeof namebuf, name);   /* positional; auto if NULL */

    memset(&nc, 0, sizeof nc);
    nc.discovery.domain_id     = o.domain;
    nc.discovery.max_peers     = o.max_peers;          /* 0 => default applied in place */
    nc.discovery.meta_capacity = o.meta_capacity;
    nc.discovery.peer_user_bytes = o.peer_user_bytes;
    nc.discovery.meta          = o.meta;
    nc.discovery.on_event      = o.on_event;
    nc.discovery.user          = o.user;
    nc.discovery.name          = dart_string(namebuf, namelen);   /* the core copies it at init */
    nc.group               = o.discovery_group;
    nc.discovery_port      = o.discovery_port;
    nc.ttl                 = o.multicast_ttl;
    nc.multicast_interface = o.multicast_interface;
    nc.seeds               = o.seed_peers;
    nc.n_seeds             = o.n_seed_peers;

    need = dart_discovery_placement_memory(&nc);
    pool = *alloc;                                 /* copied: the caller's allocator may be a temporary */
    block = dart_allocator_alloc(&pool, NULL, need);
    if (!block) return i_dart_discovery_fail(DART_DISCOVERY_E_MEMORY, 0);
    d = dart_discovery_place(block, need, &nc);
    if (!d){ DartAllocator p = pool; dart_allocator_reset(&p); return NULL; }
    d->pool = pool;                                /* the block lives in this pool; close resets it */
    return d;
}

/* Relocate the discovery runtime into a bigger block at grown counts (node arena grow).
 * The d struct copy preserves the live socket fd, the multicast group/interface and the
 * seed list; the core is migrated (UUID/version/peers preserved) and self_meta re-pointed
 * to the node core's new announce-blob address. Caller frees the old block afterward. */
DartDiscovery *dart_discovery_migrate(DartDiscovery *old, void *new_mem, size_t new_cap,
        uint16_t new_max_peers, uint16_t new_meta_capacity, const uint8_t *self_meta, void *peer_cb_user){
    DartDiscoveryCoreConfig dc; i_DartRtBlocks blk; i_DartBump b; DartDiscovery *d;
    DartDiscoveryState *nc; uint8_t *base; size_t need;
    if (!old) return NULL;
    dc = old->core->cfg; dc.max_peers = new_max_peers; dc.meta_capacity = new_meta_capacity;
    {   i_DartBump mb; memset(&mb,0,sizeof mb); i_dart_discovery_rt_layout(&mb, &dc, &blk); need = mb.offset + 16u; }
    if (new_cap < need) return NULL;
    base = (uint8_t*)(((uintptr_t)new_mem + 15u) & ~(uintptr_t)15u);
    memset(&b,0,sizeof b); b.base = base; b.cap = new_cap - (size_t)(base - (uint8_t*)new_mem);
    i_dart_discovery_rt_layout(&b, &dc, &blk);
    d = blk.d;
    *d = *old;                          /* fd, group_naddr, discovery_port, seeds, n_seeds, pool */
    d->wire_max = (uint32_t)blk.wire_max;
    d->rxbuf = blk.rxbuf; d->txbuf = blk.txbuf;
    d->peer_view = (DartDiscoveryPeer*)blk.peer_view;
    d->max_peers = new_max_peers;
    nc = dart_discovery_core_migrate(old->core, blk.core,
             new_cap - (size_t)(blk.core - (uint8_t*)new_mem), new_max_peers, new_meta_capacity,
             self_meta, peer_cb_user);
    if (!nc) return NULL;                /* old left intact; caller frees the new block */
    d->core = nc;
    return d;
}

/* Drain one socket's RX queue into the core until empty (or the burst cap, a flood
 * backstop). One recv per poll would let the queue back up for a caller that polls
 * slowly (e.g. the explorer at its frame rate): new peers, drops, and announce updates
 * would then lag the network by the backlog depth (a dead peer's queued old announces
 * keep refreshing its last_heard, so it never times out). Drain fully so timing tracks
 * real arrival, like the node's data socket already does. */
#define DART_DISCOVERY_RX_BURST 2048
static int i_dart_discovery_rt_drain(DartDiscovery *d, i_DartSock fd){
    int got = 0, guard;
    for (guard = 0; guard < DART_DISCOVERY_RX_BURST; guard++){
        uint8_t src_ip[4];
        int n = i_dart_plat_recv(fd, d->rxbuf, d->wire_max, src_ip, NULL);
        if (n < 0){ if (i_dart_plat_would_block()) break; continue; }  /* empty vs transient error */
        if (n > 0){ dart_discovery_on_datagram(d->core, src_ip, 4, dart_bytes(d->rxbuf, (size_t)n), i_dart_plat_now_us()); got = 1; }
    }
    return got;
}

int dart_discovery_poll(DartDiscovery *d, int timeout_ms){
    i_DartPollfd pfd[2];
    DartDiscoveryAddr to;
    int got = 0, nfds = 1; size_t n_bytes;

    memset(pfd, 0, sizeof pfd);
    pfd[0].fd = d->fd; pfd[0].events = DART_POLLIN;
    if (d->unicast_fd != DART_SOCK_BAD){ pfd[1].fd = d->unicast_fd; pfd[1].events = DART_POLLIN; nfds = 2; }
    if (i_dart_plat_poll(pfd, nfds, timeout_ms) < 0) return -1;

    if (pfd[0].revents & DART_POLLIN) got |= i_dart_discovery_rt_drain(d, d->fd);
    /* our own unicast port: solicit replies + re-fetch answers land here, so a same-host
       peer's reply reaches THIS process rather than the shared discovery port */
    if (nfds == 2 && (pfd[1].revents & DART_POLLIN)) got |= i_dart_discovery_rt_drain(d, d->unicast_fd);

    n_bytes = dart_discovery_update(d->core, i_dart_plat_now_us(), d->txbuf, d->wire_max);
    if (n_bytes) i_dart_discovery_tx(d, d->txbuf, n_bytes);

    /* targeted unicast: replies to soliciters + re-fetch requests for stale blobs */
    while ((n_bytes = dart_discovery_poll_targeted(d->core, d->txbuf, d->wire_max, &to)) != 0)
        i_dart_discovery_tx_to(d, d->txbuf, n_bytes, &to);
    return got;
}

int dart_discovery_gather(DartDiscovery *d, int quiet_ms, int timeout_ms){
    uint64_t start, last_change, last_solicit = 0;
    uint16_t count;
    if (!d) return 0;
    start = i_dart_plat_now_us(); last_change = start;
    count = dart_discovery_peer_count(d->core);
    for (;;){
        uint64_t now = i_dart_plat_now_us(); uint16_t c;
        if (now - last_solicit >= 250000u){     /* (re)solicit ~4x/s so a lost one retries */
            dart_discovery_solicit(d->core); last_solicit = now;
        }
        dart_discovery_poll(d, 10);              /* sends the solicit, takes in replies */
        now = i_dart_plat_now_us();
        c = dart_discovery_peer_count(d->core);
        if (c > count){ count = c; last_change = now; }   /* grew: keep waiting */
        if (count > 0 && now - last_change >= (uint64_t)quiet_ms*1000u) break;
        if (now - start >= (uint64_t)timeout_ms*1000u) break;
    }
    return (int)count;
}

DartDiscoveryState *dart_discovery_state(DartDiscovery *d){ return d ? d->core : NULL; }

const DartDiscoveryPeer *dart_discovery_peers(DartDiscovery *d, uint16_t *count){
    uint16_t i, n = 0;
    if (!d){ if (count) *count = 0; return NULL; }
    for (i=0;i<d->max_peers;i++)
        if (dart_discovery_peer_at(d->core, i, &d->peer_view[n])) n++;   /* pack used peers */
    if (count) *count = n;
    return d->peer_view;
}

void dart_discovery_close(DartDiscovery *d, int send_bye){
    DartAllocator pool;
    if (!d) return;
    if (send_bye){
        size_t n_bytes = dart_discovery_leave(d->core, d->txbuf, d->wire_max);
        int k;
        for (k = 0; n_bytes && k < DART_DISCOVERY_BYE_SENDS; k++) i_dart_discovery_tx(d, d->txbuf, n_bytes);
    }
    dart_discovery_destroy(d->core);   /* hook-allocated peer blobs (no-op without a hook); may
                                          mutate the pool, so it must run before the copy-out */
    i_dart_plat_close(d->fd);
    if (d->unicast_fd != DART_SOCK_BAD) i_dart_plat_close(d->unicast_fd);
    i_dart_plat_cleanup();
    pool = d->pool;                /* copy out LAST: the reset below frees the block (incl d) */
    dart_allocator_reset(&pool);   /* open path: frees the block; place path: empty pool, no-op */
}
#pragma endregion
#endif /* !DART_DISCOVERY_SANS_IO */
#endif /* DART_DISCOVERY_IMPLEMENTATION */

#ifdef DART_TRANSPORT_IMPLEMENTATION
#pragma region common/bytes.h
/* Shared little-endian byte packing, used by the discovery, transport, and SHM
 * layers (each formerly carried its own copy). static inline: no link symbol and
 * no unused-function warning in a layer that doesn't use a given width. The
 * amalgamator emits this once per implementation TU; the local #include is for
 * standalone compilation of a single layer. */
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
/* Bump allocator shared by the layers that pack sub-blocks into one caller-provided
 * arena (transport state, node). Measure mode (base==NULL): i_dart_bump_take returns NULL but
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
/* Shared FNV-1a 64 for a stable 64-bit identity from bytes (topic ids, schema ids).
 * static inline: no link symbol and no unused-function warning in a layer that doesn't use
 * it. The amalgamator emits it once per implementation TU; the local #include is for
 * standalone compilation of a layer. The basis/prime match DART's existing topic ids (so
 * on-wire identities are unchanged), not the textbook offset basis. */
#ifndef DART_HASH_H
#define DART_HASH_H

#include <stddef.h>
#include <stdint.h>

static inline uint64_t i_dart_fnv1a64(const void *data, size_t n){
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 1469598103934665603ull; size_t i;
    for (i = 0; i < n; i++){ h ^= (uint64_t)p[i]; h *= 1099511628211ull; }
    return h;
}

/* Over a NUL-terminated string (the topic-name identity form); NULL => 0. */
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
/* Shared internals of the split transport core: wire/scheduler constants, the state
 * structs, the small cross-file inline helpers, and prototypes for the helpers that
 * cross the wire/sched/writer/reader file boundaries. Not a public header. */
#ifndef DART_TRANSPORT_INTERNAL_H
#define DART_TRANSPORT_INTERNAL_H

#include <string.h>

/* byte 0 of every submessage: type in the low 3 bits, flags above */
#define DART_DATA 1
#define DART_HB   2
#define DART_NACK 3
#define DART_MSG_MASK 0x07u
#define DART_F_SINGLE 0x08u     /* DATA: single fragment (frag/count/len omitted) */
#define DART_F_UNPOS  0x10u     /* NACK: reader has delivered nothing yet */
#ifdef DART_SHM
#define DART_F_SHM    0x20u     /* DATA: SHM-DATA -- body is a descriptor, no payload */
#endif

#define DART_NACK_WINDOW 32u    /* seqnos covered by one ACKNACK bitmap */
#define DART__NIL 0xFFFFFFFFu

#ifndef DART_HB_SWEEP_US
#define DART_HB_SWEEP_US 25000u /* the timer sweep covers every lane this often */
#endif

#define DART__NO_DEADLINE ((uint64_t)-1)  /* next_deadline_us: nothing armed */
#ifdef DART_SHM
#ifndef DART_SHM_MAX_RETRY
#define DART_SHM_MAX_RETRY 8u   /* give up on an unresolvable descriptor after this many */
#endif
#endif

#define DART_QOS_DEF_KEEP_LAST    1u
#define DART_QOS_DEF_HEARTBEAT_US 100000u   /* 100 ms idle writer heartbeat */
#define DART_QOS_DEF_REPAIR_US    20000u    /* 20 ms reader repair-request delay */

/* Submessage wire layout. Byte 0 = type|flags, bytes 1-2 = alias, then the body.
 * Builders (dart_mk_*) and the readers both index off these, so moving a field is one
 * edit, never a silent builder/parser drift. Several fields share an offset (distinct
 * names on purpose). Header sizes: DATA 13 (single)/21 (multi), HB 23, NACK 21. */
#define DART_OFFSET_ALIAS      1u  /* u16, every submessage */
#define DART_OFFSET_SEQNO      3u  /* DATA seqno; also HB first, NACK base, SHM base (u64) */
#define DART_OFFSET_PAYLOAD_LEN_SINGLE     11u  /* DATA single: u16 payload_len (payload at DART_HEADER_DATA_SINGLE) */
#define DART_HEADER_DATA_SINGLE   13u  /* DATA single: header bytes */
#define DART_OFFSET_FRAG      11u  /* DATA multi: u16 frag */
#define DART_OFFSET_COUNT     13u  /* DATA multi: u16 count */
#define DART_OFFSET_SAMPLE_LEN      15u  /* DATA multi: u32 sample_len */
#define DART_OFFSET_PAYLOAD_LEN      19u  /* DATA multi: u16 payload_len (payload at DART_HEADER_DATA_MULTI) */
#define DART_HEADER_DATA_MULTI    21u  /* DATA multi: header bytes */
#define DART_OFFSET_HB_LAST   11u  /* HB: u64 last */
#define DART_OFFSET_HB_COUNT    19u  /* HB: u32 count */
#define DART_HEADER_HB      23u
#define DART_OFFSET_NACK_NBITS  11u  /* NACK: u16 nbits */
#define DART_OFFSET_NACK_BITMAP   13u  /* NACK: u32 bitmap */
#define DART_OFFSET_NACK_EPOCH  17u  /* NACK: u32 epoch */
#define DART_HEADER_NACK    21u
#define DART_OFFSET_SHM_COUNT 11u  /* SHM-DATA: u16 count */
#define DART_OFFSET_SHM_DESC  13u  /* SHM-DATA: descriptor (DART_SHM_DESC_BYTES follow) */
#ifdef DART_SHM
#define DART_SHM_DATA_BYTES (DART_OFFSET_SHM_DESC + DART_SHM_DESC_BYTES)
#endif

/* The three per-lane/per-slot structs below are laid out widest-field-first (u64s,
 * pointers, u32s, u16s, then u8s) so they carry no padding holes: they are allocated
 * n_channels*max_peers (proxies) and keep_last (samples) times, so padding multiplies. */
typedef struct {
    uint64_t base;       /* seqno of frag 0 */
    uint8_t *buf;        /* >= len bytes; arena (fixed) or hook-malloc'd (dynamic) */
#ifdef DART_SHM
    const uint8_t *shm_buf;            /* external chunk payload (remote peers fragment from it) */
#endif
    uint32_t len;        /* message bytes */
    uint32_t cap;        /* allocated bytes of buf (dynamic grows it) */
    uint16_t count;      /* frag count */
    uint8_t  valid;
#ifdef DART_SHM
    uint8_t  shm;        /* 1 = SHM-backed: bytes live in shm_buf, desc set, buf unused */
    uint8_t  desc[DART_SHM_DESC_BYTES];/* descriptor sent to SHM peers as one SHM-DATA */
#endif
} i_DartWriterSample;

typedef struct {        /* writer-side, per (channel,peer) */
    uint64_t sent_upto;  /* next seqno to push as new data */
    uint64_t acked_upto; /* peer received all TUs < this */
    uint64_t nack_base;
    uint64_t hb_next_us; /* heartbeat timer */
    uint32_t nack_bits;
    uint32_t hb_count;
    uint32_t reader_epoch; /* reader incarnation from last ACKNACK (0 = none); a change
                              means the peer rebuilt state, so the lane re-joins */
    uint8_t  used;
    uint8_t  reader_reliable; /* the matched reader requested RELIABLE: only then does this
                                 lane impose backpressure + heartbeats. A best-effort reader
                                 never acks, so it must stay out of flow control (fire-and-
                                 forget), else it stalls a reliable writer forever. */
    uint8_t  has_nack;   /* pending repair request from ACKNACK */
} i_DartWriterProxy;

typedef struct {        /* reader-side, per (channel,peer) */
    uint64_t deliver_upto;  /* base of current sample; all below delivered/skipped */
    uint64_t hb_last;       /* highest seqno the writer CLAIMS to hold (heartbeat only) */
    uint64_t received_high;      /* highest seqno we have actually RECEIVED a frag for. UDP is
                               assumed in-order, so a hole below this is real loss to repair
                               while anything above it is still in flight: never NACK past it. */
    uint64_t nack_high;       /* highest seqno already requested this episode; refills ask only
                               (nack_high, top] so in-flight repairs are not re-requested */
    uint64_t nack_retransmit_us;  /* earliest time to re-request a stalled floor (lost-repair backstop) */
    uint64_t ack_due_us;
    uint8_t *assembly_buf;       /* >= assembly_len; arena (fixed) or hook-malloc'd (dynamic) */
    uint8_t *frag_bitmap;       /* ceil(assembly_count/8) */
    uint32_t epoch;         /* this incarnation's id, sent in every ACKNACK */
    uint32_t assembly_len;
    uint32_t assembly_cap;       /* allocated bytes of assembly_buf (dynamic grows it) */
    uint32_t bitmap_cap;        /* allocated bytes of frag_bitmap */
    uint16_t assembly_count;
    uint16_t assembly_low;        /* lowest still-missing frag index of current sample (its
                               contiguous-received front); deliver_upto+assembly_low = first hole */
    uint8_t  used;
    uint8_t  started;       /* accepted any DATA from this writer yet */
    uint8_t  assembly_active;    /* received >=1 frag of current sample */
    uint8_t  ack_force;     /* a delivery/skip/HB/(re)match owes the writer an ACKNACK even if
                               the repair floor did not move (avoids a stuck cumulative ack) */
    uint8_t  ack_pending;
#ifdef DART_SHM
    uint8_t  shm_fail;      /* consecutive SHM-DATA resolve failures at deliver_upto */
#endif
} i_DartReaderProxy;

/* One matched lane: both direction proxies plus the scheduler's chain fields. In dynamic
 * mode records are pool-allocated only while the (channel,peer) pair actually matches, so
 * lane memory scales with real matches, not channels x peers; fixed mode (no allocator)
 * keeps the dense identity layout with per-record buffers pre-bound at init. Records move
 * when the pool grows, so nothing holds an i_DartLane* across a call that can allocate:
 * durable references are pool INDICES. */
typedef struct {
    i_DartWriterProxy w;
    i_DartReaderProxy r;
    uint16_t channel;     /* backref: the lane this record serves */
    uint16_t peer_slot;
    uint32_t sched_next;  /* next record on its dest's active list (DART__NIL);
                             doubles as the free-list link while not in_use */
    uint32_t ch_next;     /* next matched lane of the same channel (DART__NIL) */
    uint8_t  queued;      /* on its dest's active list */
    uint8_t  in_use;      /* dynamic: allocated to a lane (0 = free-list); fixed: always 1 */
} i_DartLane;

typedef struct {
    DartQos    qos;
    uint64_t  identity;     /* cross-peer topic identity (hash of name) */
    const char *name;       /* our copy of the topic name (NUL-terminated storage) */
    uint8_t   name_len;     /* its length, stored so it is never re-derived (dart_transport_channel_name is per-delivery) */
    uint16_t  max_fragments;     /* ceil(max_message_bytes/FRAG) (fixed mode only) */
    uint8_t   role;         /* DartRole */
    uint8_t   dynamic;      /* 1 = buffers grow via cfg.allocator, no fixed cap */
    uint8_t   history_owned;/* 1 = history ring was allocator-allocated (reserve-mode
                               dart_transport_channel_define), so dart_transport_destroy frees it */
    /* writer */
    i_DartWriterSample *history;       /* [depth] ring */
    uint16_t  history_head;    /* next slot to overwrite */
    uint64_t  next_seqno;
    uint64_t  first_seqno;  /* lowest seqno still cached */
    uint8_t   have_first;
    uint32_t  matched_writers; /* count of used writer proxies (matched subscribers, dormant
                                  included); kept exact at writer match/unmatch so the send path
                                  can skip the copy+commit for a publisher no one subscribes to */
    uint32_t  matched_readers; /* same, for reader proxies; with matched_writers it lets the
                                  timer sweep skip a whole channel row that owes no timer work */
    uint32_t  lane_head;       /* first matched lane record of this channel (DART__NIL = none);
                                  the send path walks this chain instead of scanning peer slots */
    /* cumulative repair counters, summed over proxies; read via dart_transport_repair_stats */
    DartRepairStats repair_stats;
} i_DartChannel;

struct DartTransportState {
    DartConfig    cfg;       /* n_channels = user channels (no internal channel) */
    uint32_t    *peer_ids;  /* [max_peers] */
    uint8_t     *peer_used; /* [max_peers] */
    uint8_t     *peer_dormant;/* [max_peers] 1 = silent (discovery DROP): out of flow control,
                                 proxies + reader position preserved for a same-incarnation resume */
    uint16_t    *peer_frag; /* [max_peers] each peer's advertised fragment size (writer side) */
    uint16_t     frag;      /* this node's fragment size: what we fragment our sends into */
#ifdef DART_SHM
    uint8_t     *peer_shm;  /* [max_peers] 1 = peer can receive SHM-DATA (same host, attached) */
#endif
    /* peer interest over OUR channel table, one bit per user channel; the proxies
       plus these bits are the whole stored interest (full peer lists are not kept).
       Fed by dart_transport_apply_peer_interest from the peer's discovery announce. */
    uint8_t     *peer_pub_bitmap; /* [max_peers][bitmap_len] peer publishes channel c */
    uint8_t     *peer_sub_bitmap; /* [max_peers][bitmap_len] peer subscribes channel c */
    uint8_t     *peer_sub_reliable; /* [max_peers][bitmap_len] ...and requested RELIABLE; sourced
                                       at match time into i_DartWriterProxy.reader_reliable */
    uint16_t     bitmap_len;       /* ceil(n_channels / 8) */
    /* per-peer wire alias -> our channel index; the data path carries the 2-byte
       alias instead of the topic name. Dynamic mode: each map is a hook allocation
       sized to that peer's highest ADVERTISED alias, made on its first matched topic
       (an irrelevant peer costs nothing; a later local subscribe re-applies interest
       and the map already covers every advertised alias). Fixed mode: every map is a
       fixed arena slice of alias_max entries, exactly the old dense table. */
    uint16_t   **peer_alias;      /* [max_peers] -> alias map (0xFFFF = unmapped) */
    uint32_t    *peer_alias_len;  /* [max_peers] entries in each map */
    uint32_t     alias_max;       /* fixed-mode stride = effective DART_META_MAX_IDS */
    i_DartChannel  *channels;     /* [n_channels] */
    /* matched-lane records (the proxies live inside). Dynamic mode: one hook allocation
       grown by doubling, records allocated per real match, lane_index maps (channel,peer)
       -> record. Fixed mode: a dense arena array in identity order (record c*max_peers+p),
       lane_index NULL, buffers pre-bound at init. */
    i_DartLane  *lanes;       /* [lane_cap] */
    uint32_t     lane_cap;
    uint32_t     lane_free;   /* free-list head (dynamic), DART__NIL when empty */
    uint16_t    *lane_index;  /* [n_channels*max_peers] record idx, 0xFFFF = unmatched;
                                 NULL = fixed mode (identity mapping, no table) */
    /* active-lane scheduler: a lane is one (channel,peer) pair. The event that gives a
       lane work enqueues it, so poll_send pays for work done, not idle lanes. Timer work
       is found by an amortized clock-driven sweep over the record pool. */
    uint32_t    *dest_head;   /* [max_peers] record-index list per dest */
    uint32_t    *dest_tail;
    uint8_t     *dest_queued;
    uint32_t    *dest_queue;       /* ring of active destinations */
    uint32_t     dest_queue_head, dest_queue_count;
    uint32_t     sweep;       /* timer-sweep lane cursor */
    uint64_t     sweep_time_us;     /* clock position the sweep has paid for */
    uint64_t     next_deadline_us; /* earliest armed timer (ack/HB); caps the poll wait.
                                      Lower bound fed at arm sites, made exact by a full sweep.
                                      DART__NO_DEADLINE = nothing deferred (the hot path). */
    uint32_t     reader_epoch_counter;      /* reader-epoch counter (starts at 1; 0 = none) */
};

typedef enum { DART_ORDER_OLD, DART_ORDER_GAP, DART_ORDER_ADOPTED, DART_ORDER_INORDER } i_DartReaderOrder;

/* small shared helpers (kept inline so every fragment can use them) */
static inline void i_dart_bit_set(uint8_t*bitmap,uint32_t i){bitmap[i>>3]|=(uint8_t)(1u<<(i&7));}
static inline int  i_dart_bit_get(const uint8_t*bitmap,uint32_t i){return (bitmap[i>>3]>>(i&7))&1;}
#ifdef DART_SHM
/* where a sample's bytes live: the external chunk for SHM samples, else our buf */
static inline const uint8_t *i_dart_sample_buf(const i_DartWriterSample *s){ return s->shm ? s->shm_buf : s->buf; }
#else
static inline const uint8_t *i_dart_sample_buf(const i_DartWriterSample *s){ return s->buf; }
#endif
/* a deferred timer was (re)armed for absolute time t: keep next_deadline as the
   minimum so the poll wakes when it is due. t==0 is an immediate ack (woken via the
   active-lane queue, not a timer), so it is ignored here. */
static inline void i_dart_transport_arm_deadline(DartTransportState *st, uint64_t t){
    if (t && t < st->next_deadline_us) st->next_deadline_us = t;
}

/* Does a late joiner ever receive samples published before it matched? Only a reliable
 * channel with catch_up>0 replays cached history (i_dart_channel_unicast_join_seqno reaches
 * back); every other channel joins at next_seqno. So when no subscriber is matched, history
 * on any other channel is dead weight and the send can be skipped outright. */
static inline int i_dart_channel_retains_history(const i_DartChannel *ch){
    return ch->qos.reliability==DART_RELIABLE && ch->qos.catch_up>0;
}

/* Does a channel owe the timer sweep any work? Writer heartbeats and reader acks both
 * require a reliable channel with a used proxy, so a best-effort channel (the common
 * high-rate case) or a reliable one no peer has matched owes nothing: the sweep skips its
 * whole peer row. Reliability is fixed at define and the counts are exact, so this never
 * skips a lane that has a live HB/ack timer. */
static inline int i_dart_channel_needs_sweep(const i_DartChannel *ch){
    return ch->qos.reliability==DART_RELIABLE && (ch->matched_writers || ch->matched_readers);
}

/* lane record for a (channel,peer) pair: pool index, or DART__NIL when the lane has
   never matched (dynamic mode; fixed mode maps every lane by identity). Centralizing
   the index math here keeps a transposed channel/peer from silently corrupting a
   neighbor lane. */
static inline uint32_t i_dart_lane_id(DartTransportState *st, uint16_t channel_idx, uint32_t peer_slot){
    size_t k = (size_t)channel_idx*st->cfg.max_peers + peer_slot;
    uint16_t t;
    if (!st->lane_index) return (uint32_t)k;          /* fixed: identity */
    t = st->lane_index[k];
    return t == 0xFFFFu ? DART__NIL : (uint32_t)t;
}
static inline i_DartLane *i_dart_lane_at(DartTransportState *st, uint16_t channel_idx, uint32_t peer_slot){
    uint32_t li = i_dart_lane_id(st, channel_idx, peer_slot);
    return li == DART__NIL ? NULL : &st->lanes[li];
}
/* NULL when the lane is unmatched (no record): every caller treats that as !used. */
static inline i_DartWriterProxy *i_dart_writer_proxy_at(DartTransportState *st, uint16_t channel_idx, uint32_t peer_slot){
    i_DartLane *l = i_dart_lane_at(st, channel_idx, peer_slot);
    return l ? &l->w : NULL;
}
static inline i_DartReaderProxy *i_dart_reader_proxy_at(DartTransportState *st, uint16_t channel_idx, uint32_t peer_slot){
    i_DartLane *l = i_dart_lane_at(st, channel_idx, peer_slot);
    return l ? &l->r : NULL;
}
/* wire alias for a local channel: its own index (the advertiser's handle). The
 * peer mapped this alias to its matching channel from our interest list. */
static inline uint16_t i_dart_alias_of(DartTransportState *st, int channel_idx){
    (void)st; return (uint16_t)channel_idx;
}

/* cross-file helper prototypes (definitions in wire/sched/writer/reader.c + transport.c) */
size_t i_dart_wire_mk_data(uint8_t *o, uint16_t alias, uint64_t seqno, i_DartWriterSample *s, uint16_t frag, const uint8_t *payload, uint16_t payload_len);
#ifdef DART_SHM
size_t i_dart_wire_mk_shm(uint8_t *o, uint16_t alias, uint64_t base, uint16_t count, const uint8_t *desc);
#endif
size_t i_dart_wire_mk_hb(uint8_t *o, uint16_t alias, uint64_t first, uint64_t last, uint32_t cnt);
size_t i_dart_wire_mk_nack(uint8_t *o, uint16_t alias, uint64_t base, uint16_t nbits, uint32_t bitmap, uint32_t epoch, uint8_t flags);
void   i_dart_lane_wake(DartTransportState *st, uint16_t channel_idx, uint32_t peer_slot);
void   i_dart_lane_enq_idx(DartTransportState *st, uint32_t rec);   /* enqueue by record index */
void   i_dart_sched_drop(DartTransportState *st, uint32_t rec);     /* unlink a record from its dest list */
size_t i_dart_writer_emit(DartTransportState *st, int channel_idx, int peer_slot, uint8_t *out, size_t cap, uint64_t now);
void   i_dart_writer_nack(DartTransportState *st, int channel_idx, int peer_slot, const uint8_t *p);
void   i_dart_reader_data(DartTransportState *st, int channel_idx, int peer_slot, const uint8_t *p, uint64_t now);
#ifdef DART_SHM
void   i_dart_reader_shm(DartTransportState *st, int channel_idx, int peer_slot, const uint8_t *p, uint64_t now);
#endif
void   i_dart_reader_hb(DartTransportState *st, int channel_idx, int peer_slot, const uint8_t *p, uint64_t now);
size_t i_dart_reader_emit(DartTransportState *st, int channel_idx, int peer_slot, uint8_t *out, size_t cap, uint64_t now);
i_DartChannel *i_dart_channel_at(DartTransportState *st, uint16_t channel, int *idx_out);
int    i_dart_peer_slot(DartTransportState *st, uint32_t id);
void   i_dart_transport_fire_event(DartTransportState *st, DartTransportEventKind kind, uint16_t channel, uint32_t peer, uint64_t first, uint64_t count);
uint64_t i_dart_channel_unicast_join_seqno(const i_DartChannel *ch);

#endif /* DART_TRANSPORT_INTERNAL_H */
#pragma endregion
#pragma region transport/wire.c
/* Transport wire codec: the DATA/HB/NACK[/SHM-DATA] submessage builders. */


/* Submessage wire layout. Byte 0 = type|flags, bytes 1-2 = alias, then the body.
 * Builders (dart_mk_*) and the readers both index off these, so moving a field is one
 * edit, never a silent builder/parser drift. Several fields share an offset (distinct
 * names on purpose). Header sizes: DATA 13 (single)/21 (multi), HB 23, NACK 21. */

/* datagram builders (return length) */
size_t i_dart_wire_mk_data(uint8_t *o, uint16_t alias, uint64_t seqno, i_DartWriterSample *s,
                         uint16_t frag, const uint8_t *payload, uint16_t payload_len){
    i_dart_le_w16(o+DART_OFFSET_ALIAS,alias);
    if (s->count==1){                       /* frag=0, count=1, len=payload_len implied */
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
/* SHM-DATA: one submessage covers [base, base+count); body is the descriptor, no
 * payload. 37 bytes = 1 (type|F_SHM) + 2 (alias) + 8 (base) + 2 (count) + 24 (desc). */
size_t i_dart_wire_mk_shm(uint8_t *o, uint16_t alias, uint64_t base, uint16_t count,
                          const uint8_t *desc){
    o[0]=(uint8_t)(DART_DATA|DART_F_SHM); i_dart_le_w16(o+DART_OFFSET_ALIAS,alias);
    i_dart_le_w64(o+DART_OFFSET_SEQNO,base); i_dart_le_w16(o+DART_OFFSET_SHM_COUNT,count);
    memcpy(o+DART_OFFSET_SHM_DESC,desc,DART_SHM_DESC_BYTES);
    return DART_SHM_DATA_BYTES;
}
#endif

size_t i_dart_wire_mk_hb(uint8_t *o, uint16_t alias, uint64_t first, uint64_t last, uint32_t cnt){
    o[0]=DART_HB; i_dart_le_w16(o+DART_OFFSET_ALIAS,alias); i_dart_le_w64(o+DART_OFFSET_SEQNO,first); i_dart_le_w64(o+DART_OFFSET_HB_LAST,last); i_dart_le_w32(o+DART_OFFSET_HB_COUNT,cnt);
    return DART_HEADER_HB;
}

size_t i_dart_wire_mk_nack(uint8_t *o, uint16_t alias, uint64_t base, uint16_t nbits, uint32_t bitmap,
                         uint32_t epoch, uint8_t flags){
    o[0]=(uint8_t)(DART_NACK|flags); i_dart_le_w16(o+DART_OFFSET_ALIAS,alias); i_dart_le_w64(o+DART_OFFSET_SEQNO,base);
    i_dart_le_w16(o+DART_OFFSET_NACK_NBITS,nbits); i_dart_le_w32(o+DART_OFFSET_NACK_BITMAP,bitmap); i_dart_le_w32(o+DART_OFFSET_NACK_EPOCH,epoch);
    return DART_HEADER_NACK;
}
#pragma endregion
#pragma region transport/sched.c
/* Transport active-lane scheduler and the outgoing poll. */


/* scheduler: work is queued as lane-RECORD indices; destination = the record's peer slot */
static void i_dart_dest_push(DartTransportState *st, uint32_t d){
    uint32_t ndest = st->cfg.max_peers, t;
    if (st->dest_queued[d]) return;
    st->dest_queued[d]=1;
    t = st->dest_queue_head + st->dest_queue_count;
    if (t >= ndest) t -= ndest;
    st->dest_queue[t]=d; st->dest_queue_count++;
}


/* enqueue a lane record that just got sendable work; idempotent while queued */
void i_dart_lane_enq_idx(DartTransportState *st, uint32_t li){
    i_DartLane *l=&st->lanes[li];
    uint32_t d=l->peer_slot;
    if (l->queued) return;
    l->queued=1; l->sched_next=DART__NIL;
    if (st->dest_head[d]==DART__NIL) st->dest_head[d]=li;
    else st->lanes[st->dest_tail[d]].sched_next=li;
    st->dest_tail[d]=li;
    i_dart_dest_push(st, d);
}


/* remove a record from its dest list (a record being recycled must never linger on a
 * list: reallocated to another peer's lane, it would misroute that lane's submessages
 * into a datagram addressed to the OLD peer). Unmatch-time only; lists are short. */
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


/* enqueue, and track a freshly-armed reader ack/NACK deadline for the poll cap. Used
 * by the arm sites (ack_due_us is future or 0); the sweep enqueues due lanes with
 * i_dart_lane_enq_idx instead, since it recomputes next_deadline itself. */
void i_dart_lane_wake(DartTransportState *st, uint16_t channel_idx, uint32_t peer_slot){
    uint32_t li=i_dart_lane_id(st,channel_idx,peer_slot);
    i_DartLane *l;
    if (li==DART__NIL) return;                     /* unmatched lane: nothing to schedule */
    l=&st->lanes[li];
    if (l->r.used && l->r.ack_pending) i_dart_transport_arm_deadline(st, l->r.ack_due_us);
    i_dart_lane_enq_idx(st, li);
}


/* sendable work a popped record still owes now (timer-armed work is the sweep's job) */
static int i_dart_lane_work(DartTransportState *st, const i_DartLane *l, uint64_t now){
    i_DartChannel *ch=&st->channels[l->channel];
    uint32_t peer_slot=l->peer_slot;
    if (!st->peer_used[peer_slot] || st->peer_dormant[peer_slot]) return 0;   /* dormant: out of flow control */
    if (l->w.used && l->w.has_nack) return 1;
    if (l->w.used && l->w.sent_upto < ch->next_seqno) return 1;
    if (l->r.used && ch->qos.reliability==DART_RELIABLE
        && l->r.ack_pending && now >= l->r.ack_due_us) return 1;
    return 0;
}


/* clock-driven counterpart of the wake calls: a cursor walks the record POOL waking
 * lanes whose timers came due. Two triggers: the amortized backstop (full coverage
 * every DART_HB_SWEEP_US) and a forced full pass when next_deadline_us comes due, so
 * a deadline-capped poll that wakes for a timer actually services it. A full pass
 * also recomputes next_deadline_us exactly (the global min of not-yet-due timers).
 * Read-only; cost is bounded by the pool (i.e. by matched lanes, not channels x peers:
 * an idle channel has no records to visit at all in dynamic mode). */
static void i_dart_hb_sweep(DartTransportState *st, uint64_t now){
    uint32_t total=st->lane_cap, due, k;
    uint64_t span = now - st->sweep_time_us;
    int forced = (now >= st->next_deadline_us);    /* a tracked timer is due */
    int full;
    uint64_t mind = DART__NO_DEADLINE;             /* earliest not-yet-due timer seen */
    if (total==0){                                 /* no lanes have ever matched */
        if (forced) st->next_deadline_us = DART__NO_DEADLINE;
        st->sweep_time_us = now;
        return;
    }
    due = (forced || span >= DART_HB_SWEEP_US) ? total
        : (uint32_t)(span * total / DART_HB_SWEEP_US);
    if (!due) return;              /* sweep_time_us advances only when lanes are paid */
    full = (due >= total);         /* covered every record -> mind is the global minimum */
    st->sweep_time_us = now;
    for (k=0;k<due;k++){
        uint32_t li=st->sweep;
        i_DartLane *l=&st->lanes[li];
        i_DartChannel *ch;
        uint32_t peer_slot;
        st->sweep = (st->sweep+1u>=total) ? 0u : st->sweep+1u;
        if (!l->in_use) continue;                  /* free pool slot */
        ch=&st->channels[l->channel];
        if (!i_dart_channel_needs_sweep(ch)) continue;   /* best-effort, or nothing matched */
        peer_slot=l->peer_slot;
        /* gate writer heartbeats on next_seqno, never the reader ack: a sub-only
           node's data channels never advance next_seqno but still owe acks */
        if (!st->peer_used[peer_slot] || st->peer_dormant[peer_slot]) continue;   /* dormant: out of flow control */
        if (l->w.used && l->w.reader_reliable && l->w.acked_upto < ch->next_seqno){
            if (now>=l->w.hb_next_us) i_dart_lane_enq_idx(st,li);
            else if (l->w.hb_next_us < mind) mind = l->w.hb_next_us;
        }
        if (l->r.used && l->r.ack_pending){
            if (now>=l->r.ack_due_us) i_dart_lane_enq_idx(st,li);
            else if (l->r.ack_due_us < mind) mind = l->r.ack_due_us;
        }
    }
    /* a full pass saw every timer: mind is the exact next deadline. Lanes woken above
       re-arm during emit (i_dart_transport_arm_deadline) and re-lower it; reader acks just clear. */
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
            uint16_t channel_idx=l->channel; uint32_t peer_slot=l->peer_slot;
            size_t n;
            do {
                /* acks first: small, one-shot, and carry the NACKs that drive
                   repair, so a backlogged writer can't starve them */
                n=i_dart_reader_emit(st,(int)channel_idx,(int)peer_slot,(uint8_t*)out+offset,cap-offset,now);
                if (!n) n=i_dart_writer_emit(st,(int)channel_idx,(int)peer_slot,(uint8_t*)out+offset,cap-offset,now);
                offset+=n;
            } while (n && offset<cap);
            st->dest_head[d]=l->sched_next;
            if (i_dart_lane_work(st,l,now)){
                /* datagram full mid-lane: rotate the lane to the back so siblings get the next */
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
        if (st->dest_head[d]!=DART__NIL) i_dart_dest_push(st,d);  /* fair: re-queue at tail */
        if (offset){
            *to_peer = st->peer_ids[d];
            *out_len = offset;
            return 1;
        }
        if (st->dest_head[d]!=DART__NIL)
            return 0;    /* work pending but nothing fit: caller's cap too small */
    }
    return 0;
}


uint64_t dart_transport_next_deadline_us(DartTransportState *st){
    return st->next_deadline_us == DART__NO_DEADLINE ? 0 : st->next_deadline_us;
}
#pragma endregion
#pragma region transport/writer.c
/* Transport writer path: history, send, per-lane emit, ACKNACK handling. */


/* find cached sample containing seqno (newest-first, so pushing new data is O(1)) */
static i_DartWriterSample *i_dart_sample_find(i_DartChannel *ch, uint64_t seqno){
    uint16_t depth = ch->qos.keep_last, k;
    uint16_t i = ch->history_head;
    for (k=0;k<depth;k++){
        i_DartWriterSample *s;
        i = (uint16_t)(i ? i-1 : depth-1);
        s = &ch->history[i];
        if (!s->valid) break;                  /* reached the unwritten tail */
        if (seqno >= s->base)
            return (seqno < s->base + s->count) ? s : NULL;
    }
    return NULL;
}


/* append the filled head slot to history and wake the lanes that carry it */
static void i_dart_writer_commit(DartTransportState *st, uint16_t channel_idx, size_t len){
    i_DartChannel *ch = &st->channels[channel_idx];
    uint16_t depth = ch->qos.keep_last;
    uint16_t count = (uint16_t)((len + st->frag - 1) / st->frag);
    i_DartWriterSample *slot = &ch->history[ch->history_head];
    if (count==0) count=1;
    slot->valid=1; slot->base=ch->next_seqno; slot->count=count; slot->len=(uint32_t)len;
    ch->history_head = (uint16_t)((ch->history_head+1) % depth);
    ch->next_seqno += count;
    /* oldest cached: where the head points once wrapped, else slot 0 */
    ch->first_seqno = ch->history[ch->history_head].valid ? ch->history[ch->history_head].base
                                                    : ch->history[0].base;
    ch->have_first  = 1;
    { uint32_t li = ch->lane_head;       /* wake the matched lanes: O(matches), not O(max_peers) */
      while (li != DART__NIL){
          i_DartLane *l = &st->lanes[li];
          if (l->w.used && !st->peer_dormant[l->peer_slot]) i_dart_lane_enq_idx(st, li);
          li = l->ch_next;
      }
    }
}


int dart_transport_send(DartTransportState *st, uint16_t channel, DartBytes data, uint64_t now){
    int channel_idx; i_DartChannel *ch; size_t len = data.len;
    (void)now;
    ch = i_dart_channel_at(st, channel, &channel_idx);                /* rejects the internal meta channel */
    if (!ch) return DART_ERR_NO_CHANNEL;
    if (ch->dynamic){
        if (len > 65535u*(uint32_t)st->frag) return DART_ERR_TOO_BIG;   /* wire fragment-count cap */
    } else if (len > ch->qos.max_message_bytes) return DART_ERR_TOO_BIG;
    if (ch->role == DART_SUB_ONLY || ch->role == DART_INACTIVE) return DART_ERR_ROLE;
    /* Nobody subscribes and nothing durable to keep: the sample would land in the ring and
       be orphaned (a fresh match joins at next_seqno unless reliable+catch_up), so skip the
       grow, the copy, and the commit sweep entirely. The many-idle-publishers fast path. */
    if (ch->matched_writers == 0 && !i_dart_channel_retains_history(ch)) return DART_OK;
    if (ch->dynamic){
        i_DartWriterSample *slot = &ch->history[ch->history_head];
        size_t need = len ? len : 1u;
        if ((size_t)slot->cap < need){                    /* grow the slot to fit (size checked above) */
            uint8_t *new_buf = (uint8_t*)st->cfg.allocator(st->cfg.user, slot->buf, need);
            if (!new_buf) return DART_ERR_OOM;                 /* out of memory */
            slot->buf = new_buf; slot->cap = (uint32_t)need;
        }
    }
    if (len) memcpy(ch->history[ch->history_head].buf, data.data, len);
#ifdef DART_SHM
    ch->history[ch->history_head].shm = 0;   /* an inline send: this slot is not SHM-backed */
#endif
    i_dart_writer_commit(st, (uint16_t)channel_idx, len);
    return DART_OK;
}

#ifdef DART_SHM

/* publish a sample whose bytes live in an external (shared-memory) chunk: store the
 * chunk pointer + descriptor on the history slot without copying. Remote peers
 * fragment from the chunk; SHM peers get the one-submessage descriptor. */
int dart_transport_send_shm(DartTransportState *st, uint16_t channel, DartBytes chunk,
                  const uint8_t *desc, uint64_t now){
    int channel_idx; i_DartChannel *ch; i_DartWriterSample *slot; size_t len = chunk.len;
    (void)now;
    ch = i_dart_channel_at(st, channel, &channel_idx);
    if (!ch) return DART_ERR_NO_CHANNEL;
    if (len > 65535u*(uint32_t)st->frag) return DART_ERR_TOO_BIG;      /* wire fragment-count cap */
    if (ch->role == DART_SUB_ONLY || ch->role == DART_INACTIVE) return DART_ERR_ROLE;
    slot = &ch->history[ch->history_head];
    slot->shm = 1;
    slot->shm_buf = chunk.data;
    memcpy(slot->desc, desc, DART_SHM_DESC_BYTES);
    i_dart_writer_commit(st, (uint16_t)channel_idx, len);
    return DART_OK;
}
#endif


int dart_transport_send_would_evict(DartTransportState *st, uint16_t channel){
    int channel_idx; i_DartChannel *ch = i_dart_channel_at(st, channel, &channel_idx);
    i_DartWriterSample *slot; uint32_t li;
    if (!ch || ch->qos.reliability != DART_RELIABLE) return 0;
    slot = &ch->history[ch->history_head];        /* slot the next send overwrites */
    if (!slot->valid) return 0;
    for (li=ch->lane_head; li!=DART__NIL; li=st->lanes[li].ch_next){
        i_DartLane *l=&st->lanes[li];
        if (l->w.used && l->w.reader_reliable && !st->peer_dormant[l->peer_slot]
            && l->w.acked_upto < slot->base + slot->count) return 1;
    }
    return 0;
}


int dart_transport_send_would_evict_unsent(DartTransportState *st, uint16_t channel,
                                            uint64_t *evict_base, uint32_t *evict_count){
    int channel_idx; i_DartChannel *ch = i_dart_channel_at(st, channel, &channel_idx);
    i_DartWriterSample *slot; uint32_t li;
    if (!ch) return 0;
    slot = &ch->history[ch->history_head];        /* slot the next send overwrites */
    if (!slot->valid) return 0;
    for (li=ch->lane_head; li!=DART__NIL; li=st->lanes[li].ch_next){
        i_DartLane *l=&st->lanes[li];
        if (l->w.used && !st->peer_dormant[l->peer_slot] && l->w.sent_upto < slot->base + slot->count){
            if (evict_base)  *evict_base  = slot->base;
            if (evict_count) *evict_count = slot->count;
            return 1;
        }
    }
    return 0;
}


int dart_transport_send_drained(DartTransportState *st, uint16_t channel){
    int channel_idx; i_DartChannel *ch = i_dart_channel_at(st, channel, &channel_idx);
    uint32_t li;
    if (!ch || ch->qos.reliability != DART_RELIABLE) return 1;  /* no acks to await */
    for (li=ch->lane_head; li!=DART__NIL; li=st->lanes[li].ch_next){
        i_DartLane *l=&st->lanes[li];
        if (l->w.used && l->w.reader_reliable && !st->peer_dormant[l->peer_slot]
            && l->w.acked_upto < ch->next_seqno) return 0;  /* reliable reader still behind */
    }
    return 1;
}


int dart_transport_writer_match_count(DartTransportState *st, uint16_t channel){
    i_DartChannel *ch = i_dart_channel_at(st, channel, NULL);
    return ch ? (int)ch->matched_writers : 0;   /* cached at match/unmatch, so O(1) */
}


int dart_transport_repair_pending(DartTransportState *st, uint16_t channel){
    int channel_idx; i_DartChannel *ch = i_dart_channel_at(st, channel, &channel_idx);
    uint32_t li; int cnt = 0;
    if (!ch) return 0;
    for (li=ch->lane_head; li!=DART__NIL; li=st->lanes[li].ch_next){
        i_DartLane *l=&st->lanes[li];
        if (l->w.used && l->w.has_nack) cnt++;
    }
    return cnt;   /* writer lanes with a NACK to service; 0 = nothing to resend right now */
}

#ifdef DART_SHM

/* 1 if the channel has >=1 matched reader and EVERY matched (non-dormant) reader is
 * SHM-capable -> the node may publish this message via SHM. One non-SHM (remote)
 * reader forces inline UDP for the whole message. */
int dart_transport_writer_shm_eligible(DartTransportState *st, uint16_t channel){
    int channel_idx; i_DartChannel *ch = i_dart_channel_at(st, channel, &channel_idx);
    uint32_t li; int any=0;
    if (!ch) return 0;
    for (li=ch->lane_head; li!=DART__NIL; li=st->lanes[li].ch_next){
        i_DartLane *l=&st->lanes[li];
        if (!l->w.used || st->peer_dormant[l->peer_slot]) continue;
        if (!st->peer_shm[l->peer_slot]) return 0;
        any = 1;
    }
    return any;
}
#endif

#ifdef DART_SHM
/* the history slot the next publish will occupy (so the node binds a chunk to it) */
uint16_t dart_transport_channel_hist_head(DartTransportState *st, uint16_t channel){
    int channel_idx; i_DartChannel *ch = i_dart_channel_at(st, channel, &channel_idx);
    return ch ? ch->history_head : 0;
}
#endif

/* emit an HB advertising this lane's current window. Doubles as the "skip past a
 * hole" signal that replaces GAP: reader_hb advances deliver_upto to `first`, so a
 * superseded NACK or a ring-overrun push answers with an HB whose first = our floor.
 * Resets the idle-HB timer so we don't double-send. */
static size_t i_dart_writer_hb(DartTransportState *st, i_DartChannel *ch, i_DartWriterProxy *w, uint16_t alias,
                             uint8_t *out, size_t cap, uint64_t now){
    uint64_t first = ch->have_first ? ch->first_seqno : 0;
    if (cap < DART_HEADER_HB) return 0;
    if (w->acked_upto > first) first = w->acked_upto;   /* fresh reader adopts join point */
    w->hb_next_us = now + ch->qos.heartbeat_us;
    i_dart_transport_arm_deadline(st, w->hb_next_us);                  /* wake to send the next idle HB */
    w->hb_count++;
    return i_dart_wire_mk_hb(out, alias, first, ch->next_seqno-1, w->hb_count);
}


/* writer side: handle ACKNACK */
void i_dart_writer_nack(DartTransportState *st, int channel_idx, int peer_slot, const uint8_t *p){
    i_DartChannel *ch=&st->channels[channel_idx];
    i_DartWriterProxy *w=i_dart_writer_proxy_at(st,channel_idx,peer_slot);
    uint64_t base=i_dart_le_r64(p+DART_OFFSET_SEQNO); uint16_t nbits=i_dart_le_r16(p+DART_OFFSET_NACK_NBITS); uint32_t bitmap=i_dart_le_r32(p+DART_OFFSET_NACK_BITMAP);
    uint32_t epoch=i_dart_le_r32(p+DART_OFFSET_NACK_EPOCH); uint8_t flags=p[0];
    if (!w || !w->used) return;
    if (w->reader_epoch != epoch){
        if (w->reader_epoch){
            /* reader is a new incarnation (one-sided flap): our positions describe its
               dead predecessor, so re-join the lane as if freshly matched */
            w->sent_upto  = i_dart_channel_unicast_join_seqno(ch);
            w->acked_upto = w->sent_upto;
            w->has_nack   = 0;
            w->hb_next_us = 0;
            i_dart_lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
            w->reader_epoch = epoch;
            return;
        }
        w->reader_epoch = epoch;      /* first contact: lane is already fresh */
    }
    if (flags & DART_F_UNPOS){
        /* reader has delivered nothing and never NACKs: re-push from the unacked
           edge (the join window) so a push that raced ahead isn't lost */
        if (w->acked_upto < w->sent_upto){
            w->sent_upto = w->acked_upto;
            i_dart_lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
        }
        return;                    /* no position information to apply */
    }
    if (base > w->acked_upto) w->acked_upto=base;
    if (nbits>0 && bitmap!=0){
        ch->repair_stats.nacks_recv++;                           /* a repair request, not a bare ack */
        w->has_nack=1; w->nack_base=base; w->nack_bits=bitmap;
        i_dart_lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
    }
}


/* produce one writer submessage for (channel_idx,peer_slot) if due and it fits cap; 0 if none.
 * On no-fit, state is untouched so the same submessage is produced next time. */
size_t i_dart_writer_emit(DartTransportState *st, int channel_idx, int peer_slot, uint8_t *out, size_t cap, uint64_t now){
    i_DartChannel *ch=&st->channels[channel_idx];
    i_DartWriterProxy *w=i_dart_writer_proxy_at(st,channel_idx,peer_slot);
    int reliable=(ch->qos.reliability==DART_RELIABLE);
    uint16_t alias = i_dart_alias_of(st, channel_idx);
    if (!w || !w->used || st->peer_dormant[peer_slot]) return 0;   /* unmatched/dormant: nothing to emit */

    /* 1. repair (reliable only) */
    if (reliable && w->has_nack){
        uint32_t i;
        for (i=0;i<DART_NACK_WINDOW;i++){
            if (w->nack_bits & (1u<<i)){
                uint64_t seqno=w->nack_base+i;
                i_DartWriterSample *s;
                if (seqno>=ch->next_seqno){                 /* nothing there */
                    w->nack_bits &= ~(1u<<i);
                    if (w->nack_bits==0) w->has_nack=0;
                    continue;
                }
                s=i_dart_sample_find(ch,seqno);
                if (s){
#ifdef DART_SHM
                    if (st->peer_shm[peer_slot] && s->shm){   /* re-send the whole message as one SHM-DATA */
                        uint32_t j;
                        if (cap < DART_SHM_DATA_BYTES) return 0;
                        for (j=0;j<DART_NACK_WINDOW;j++){
                            uint64_t sq=w->nack_base+j;
                            if (sq>=s->base && sq<s->base+s->count) w->nack_bits &= ~(1u<<j);
                        }
                        if (w->nack_bits==0) w->has_nack=0;
                        return i_dart_wire_mk_shm(out,alias,s->base,s->count,s->desc);
                    }
#endif
                    {
                    uint16_t frag_idx=(uint16_t)(seqno - s->base);
                    uint32_t offset=(uint32_t)frag_idx*st->frag;
                    uint16_t payload_len=(uint16_t)((s->len-offset)<st->frag?(s->len-offset):st->frag);
                    if (cap < (size_t)(s->count==1?DART_HEADER_DATA_SINGLE:DART_HEADER_DATA_MULTI)+(size_t)payload_len) return 0;   /* bit stays set */
                    w->nack_bits &= ~(1u<<i);
                    if (w->nack_bits==0) w->has_nack=0;
                    ch->repair_stats.frags_sent++; ch->repair_stats.frags_resent++;   /* retransmit to satisfy a NACK */
                    return i_dart_wire_mk_data(out,alias,seqno,s,frag_idx,i_dart_sample_buf(s)+offset,payload_len);
                    }
                } else {
                    /* superseded: skip the reader past the dropped region with an HB
                       (its first = our floor); keep still-cached seqnos for later repair */
                    uint64_t floor = (ch->have_first?ch->first_seqno:ch->next_seqno);
                    uint32_t j;
                    if (cap < DART_HEADER_HB) return 0;
                    for (j=0;j<DART_NACK_WINDOW;j++)
                        if (w->nack_base+j < floor) w->nack_bits &= ~(1u<<j);
                    if (w->nack_bits==0) w->has_nack=0;
                    return i_dart_writer_hb(st,ch,w,alias,out,cap,now);
                }
            }
        }
        w->has_nack=0;
    }

    /* 2. push new data */
    if (w->sent_upto < ch->next_seqno){
        uint64_t seqno=w->sent_upto;
        i_DartWriterSample *s=i_dart_sample_find(ch,seqno);
        if (s){
#ifdef DART_SHM
            /* peer_shm is set at attach (before data flows), so sent_upto sits at a
               sample boundary here: emit the whole message as one SHM-DATA */
            if (st->peer_shm[peer_slot] && s->shm){
                if (cap < DART_SHM_DATA_BYTES) return 0;
                w->sent_upto = s->base + s->count;
                return i_dart_wire_mk_shm(out,alias,s->base,s->count,s->desc);
            }
#endif
            {
            uint16_t frag_idx=(uint16_t)(seqno - s->base);
            uint32_t offset=(uint32_t)frag_idx*st->frag;
            uint16_t payload_len=(uint16_t)((s->len-offset)<st->frag?(s->len-offset):st->frag);
            if (cap < (size_t)(s->count==1?DART_HEADER_DATA_SINGLE:DART_HEADER_DATA_MULTI)+(size_t)payload_len) return 0;
            w->sent_upto++;
            ch->repair_stats.frags_sent++;                       /* new data (unicast lane) */
            return i_dart_wire_mk_data(out,alias,seqno,s,frag_idx,i_dart_sample_buf(s)+offset,payload_len);
            }
        } else {
            /* fell out of the ring before we sent it: skip the reader up to first
               cached with an HB (its first = our floor) */
            if (cap < DART_HEADER_HB) return 0;
            w->sent_upto=(ch->have_first?ch->first_seqno:ch->next_seqno);
            return i_dart_writer_hb(st,ch,w,alias,out,cap,now);
        }
    }

    /* 3. heartbeat (reliable, timer due, and this reader is behind). Once it has
       acked everything (acked_upto == next_seqno) there's nothing to repair, so the
       lane goes silent until new data or a (re)subscribe drops acked_upto again. The
       HB advertises from acked_upto so a fresh reader adopts the join point. */
    if (reliable && w->reader_reliable && now>=w->hb_next_us && w->acked_upto < ch->next_seqno)
        return i_dart_writer_hb(st,ch,w,alias,out,cap,now);
    return 0;
}
#pragma endregion
#pragma region transport/reader.c
/* Transport reader path: ordering, reassembly, delivery, the ACKNACK emit. */


int dart_transport_reader_progress(DartTransportState *st, uint16_t channel, uint32_t peer,
                         uint64_t *base_seqno, uint32_t *have, uint32_t *total){
    int channel_idx; i_DartChannel *ch = i_dart_channel_at(st, channel, &channel_idx);
    int peer_slot; i_DartReaderProxy *r;
    if (!ch) return 0;
    peer_slot = i_dart_peer_slot(st, peer);
    if (peer_slot < 0) return 0;
    r = i_dart_reader_proxy_at(st,channel_idx,peer_slot);
    if (!r || !r->used || !r->assembly_active) return 0;      /* no message mid-reassembly */
    if (base_seqno) *base_seqno = r->deliver_upto;       /* HOL message starts here */
    if (total)      *total      = r->assembly_count;
    if (have){
        uint32_t i, c=0;
        for (i=0;i<r->assembly_count;i++) if (i_dart_bit_get(r->frag_bitmap,i)) c++;
        *have = c;
    }
    return 1;
}


/* diagnostic: attribute each reader NACK-arm (a 0->1 transition of ack_pending) to
 * its cause -- a DATA/SHM-DATA arrival or a heartbeat. Pure counting, call it right
 * before any r->ack_pending=1 in the repair paths. arms_data tracks gap-triggered and
 * progress-refill arming; arms_hb tracks the writer's idle ping (which also drives the
 * tail-loss backstop). Read via dart_transport_repair_stats. (The self-clocked retransmit backstop
 * re-fires via nack_retransmit_us without a fresh arm, so it is not counted here. Resume/
 * position-report arms are control, not counted.) */
static void i_dart_reader_arm(i_DartChannel *ch, i_DartReaderProxy *r, int is_hb){
    if (!r->ack_pending){ if (is_hb) ch->repair_stats.arms_hb++; else ch->repair_stats.arms_data++; }
}

static i_DartReaderOrder i_dart_reader_order_arrival(DartTransportState *st, int channel_idx, int peer_slot,
                                             i_DartReaderProxy *r, uint64_t base, uint64_t top){
    i_DartChannel *ch=&st->channels[channel_idx];
    if (base < r->deliver_upto) return DART_ORDER_OLD;
    if (top > r->received_high) r->received_high = top;          /* proof these seqnos exist */
    if (base > r->deliver_upto){
        if (ch->qos.reliability==DART_RELIABLE && r->started){   /* gap: arm a repair NACK */
            if (!r->ack_pending){ i_dart_reader_arm(ch,r,0); r->ack_pending=1; r->ack_due_us=0; }
            i_dart_lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
            return DART_ORDER_GAP;
        }
        if (r->started){                                         /* best-effort / first contact: adopt */
            i_dart_transport_fire_event(st, DART_TRANSPORT_MSG_LOST, (uint16_t)channel_idx, st->peer_ids[peer_slot],
                        r->deliver_upto, base - r->deliver_upto);
            ch->repair_stats.msgs_skipped += base - r->deliver_upto;
        }
        r->deliver_upto = base;
        return DART_ORDER_ADOPTED;
    }
    return DART_ORDER_INORDER;
}

#ifdef DART_SHM

/* reader side: handle an SHM-DATA submessage. It covers [base, base+count) in one
 * shot (payload is in shared memory), so there is no reassembly -- just ordering,
 * then hand the descriptor to on_shm (the node resolves + delivers + acks). The gap
 * case re-uses the normal NACK window (i_dart_reader_emit's !assembly_active branch). */
void i_dart_reader_shm(DartTransportState *st, int channel_idx, int peer_slot, const uint8_t *p, uint64_t now){
    i_DartChannel *ch=&st->channels[channel_idx];
    i_DartReaderProxy *r=i_dart_reader_proxy_at(st,channel_idx,peer_slot);
    int reliable = (ch->qos.reliability==DART_RELIABLE);
    uint64_t base = i_dart_le_r64(p+DART_OFFSET_SEQNO);
    uint16_t count = i_dart_le_r16(p+DART_OFFSET_SHM_COUNT);
    const uint8_t *desc = p+DART_OFFSET_SHM_DESC;          /* DART_SHM_DESC_BYTES */
    if (!r || !r->used || count==0) return;
    {   i_DartReaderOrder ord = i_dart_reader_order_arrival(st, channel_idx, peer_slot, r, base, base+count-1);
        if (ord==DART_ORDER_OLD || ord==DART_ORDER_GAP) return;   /* old/dup, or repair armed for a gap */
    }
    r->started = 1; r->assembly_active = 0;
    /* in order (base == deliver_upto). Resolve the chunk; advance + ack ONLY if the
       node delivered. A failed resolve (recycled, or a transient unattachable segment)
       leaves the gap so the reliability layer repairs it (re-sent descriptor) or skips
       it (writer HB, sample evicted). A persistently unresolvable descriptor (mis-
       configured SHM constants) would loop, so after DART_SHM_MAX_RETRY tries we skip
       it loudly instead of wedging. */
    {   int ok = st->cfg.on_shm &&
                 st->cfg.on_shm(st->cfg.user, (uint16_t)channel_idx, st->peer_ids[peer_slot], desc);
        if (ok){
            r->shm_fail = 0;
            r->deliver_upto = base + count;
            if (reliable){                  /* ack now, AFTER delivery (zero-copy invariant) */
                i_dart_reader_arm(ch,r,0); r->ack_pending=1; r->ack_due_us=0; r->ack_force=1;
                i_dart_lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
            }
            return;
        }
        if (reliable && ++r->shm_fail >= DART_SHM_MAX_RETRY){
            i_dart_transport_fire_event(st, DART_TRANSPORT_MSG_LOST, (uint16_t)channel_idx, st->peer_ids[peer_slot],
                        base, count);
            ch->repair_stats.msgs_skipped += count;
            r->shm_fail = 0;
            r->deliver_upto = base + count;             /* give up: skip past it, ack the new edge */
            i_dart_reader_arm(ch,r,0); r->ack_pending=1; r->ack_due_us=0; r->ack_force=1;
        } else if (reliable){                           /* leave the gap, NACK for a re-send */
            i_dart_reader_arm(ch,r,0); r->ack_pending=1; r->ack_due_us=0;
        } else {
            r->deliver_upto = base + count;             /* best-effort: no repair, drop it */
        }
        i_dart_lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
    }
}
#endif


/* reader side: handle DATA */
void i_dart_reader_data(DartTransportState *st, int channel_idx, int peer_slot, const uint8_t *p,
                           uint64_t now){
    i_DartChannel *ch=&st->channels[channel_idx];
    i_DartReaderProxy *r=i_dart_reader_proxy_at(st,channel_idx,peer_slot);
    int reliable = (ch->qos.reliability==DART_RELIABLE);
    int new_fragment = 0;
    uint64_t seqno, base; uint16_t frag, count, payload_len; uint32_t sample_len; const uint8_t *payload;
    if (p[0] & DART_F_SINGLE){           /* single fragment: frag/count/len implied */
        seqno=i_dart_le_r64(p+DART_OFFSET_SEQNO); frag=0; count=1; payload_len=i_dart_le_r16(p+DART_OFFSET_PAYLOAD_LEN_SINGLE); sample_len=payload_len; payload=p+DART_HEADER_DATA_SINGLE;
    } else {
        seqno=i_dart_le_r64(p+DART_OFFSET_SEQNO); frag=i_dart_le_r16(p+DART_OFFSET_FRAG); count=i_dart_le_r16(p+DART_OFFSET_COUNT);
        sample_len=i_dart_le_r32(p+DART_OFFSET_SAMPLE_LEN); payload_len=i_dart_le_r16(p+DART_OFFSET_PAYLOAD_LEN); payload=p+DART_HEADER_DATA_MULTI;
    }
    base = seqno - frag;

    if (!r || !r->used){ ch->repair_stats.frags_malformed++; return; }     /* not subscribed */
    if (count==0 || frag>=count){ ch->repair_stats.frags_malformed++; return; }  /* malformed */
    switch (i_dart_reader_order_arrival(st, channel_idx, peer_slot, r, base, seqno)){
        case DART_ORDER_OLD:     ch->repair_stats.frags_old++;   return;   /* already delivered/skipped */
        case DART_ORDER_GAP:     ch->repair_stats.frags_ahead++; return;   /* future frag; repair armed */
        case DART_ORDER_ADOPTED: ch->repair_stats.frags_ahead++; r->assembly_active=0; break;  /* skipped past loss */
        case DART_ORDER_INORDER: break;
    }
    r->started = 1;   /* writer engaged: position adopted */
    /* fit the reassembly buffers (dynamic grows via the hook, fixed is capped at
       max_message_bytes); "too big" skips the whole sample and reports it */
    { uint32_t bitmap_need = ((uint32_t)count + 7u) / 8u, buf_cap, bitmap_bytes; int too_big = 0;
      if (ch->dynamic){
          if (r->assembly_cap < sample_len){
              uint8_t *new_buf = (uint8_t*)st->cfg.allocator(st->cfg.user, r->assembly_buf, sample_len?sample_len:1u);
              if (!new_buf) too_big = 1; else { r->assembly_buf = new_buf; r->assembly_cap = sample_len?sample_len:1u; }
          }
          if (!too_big && r->bitmap_cap < bitmap_need){
              uint8_t *new_bitmap = (uint8_t*)st->cfg.allocator(st->cfg.user, r->frag_bitmap, bitmap_need?bitmap_need:1u);
              if (!new_bitmap) too_big = 1; else { r->frag_bitmap = new_bitmap; r->bitmap_cap = bitmap_need?bitmap_need:1u; }
          }
      } else if (sample_len > ch->qos.max_message_bytes) too_big = 1;
      if (too_big){
          i_dart_transport_fire_event(st, DART_TRANSPORT_MSG_TOO_BIG, (uint16_t)channel_idx, st->peer_ids[peer_slot],
                      0, sample_len);
          r->deliver_upto = base + count; r->assembly_active = 0;
          if (reliable){
              i_dart_reader_arm(ch,r,0); r->ack_pending = 1; r->ack_due_us = 0; r->ack_force = 1;
              i_dart_lane_wake(st, (uint16_t)channel_idx, (uint32_t)peer_slot);
          }
          return;
      }
      buf_cap  = ch->dynamic ? r->assembly_cap : ch->qos.max_message_bytes;
      bitmap_bytes = ch->dynamic ? bitmap_need     : (uint32_t)((ch->max_fragments+7u)/8u);
      /* base == deliver_upto: current sample */
      if (!r->assembly_active){
          r->assembly_active=1; r->assembly_count=count; r->assembly_len=sample_len; r->assembly_low=0;
          r->nack_high=base;     /* in-flight dedup is per-message: start this one fresh */
          memset(r->frag_bitmap,0,bitmap_bytes);
      }
      if (count!=r->assembly_count) return;                  /* inconsistent, ignore */
      ch->repair_stats.frags_recv++;                             /* every accepted DATA fragment, dups included */
      new_fragment = !i_dart_bit_get(r->frag_bitmap,frag);
      if (new_fragment){
          /* reassemble at the SOURCE peer's fragment size (advertised via discovery);
             a peer staying within [MIN, MAX] keeps count <= max_fragments, so the bitmap
             can't overflow and the buf_cap guard catches any stray offset */
          uint32_t offset=(uint32_t)frag*st->peer_frag[peer_slot];
          if (offset+payload_len<=buf_cap) memcpy(r->assembly_buf+offset,payload,payload_len);
          i_dart_bit_set(r->frag_bitmap,frag);
          if (frag==r->assembly_low)                          /* extended the contiguous-received front */
              while (r->assembly_low<count && i_dart_bit_get(r->frag_bitmap,r->assembly_low)) r->assembly_low++;
      } else ch->repair_stats.frags_dup++;                        /* already held: repair overlap / waste */
    }
    /* assembly_low is the contiguous front, so the sample is complete iff it reached the end.
       Deliver in order, advance, then arm the ACKNACK. A completed sample owes an immediate
       cumulative ack (ack_force). A still-partial sample only re-arms when this frag opened or
       advanced a real gap (a hole below received_high): a healthy in-order fill owes nothing, and
       emit dedups + paces the repair request so we never re-flood the writer with in-flight
       fragments. */
    { int done = (r->assembly_low == count);
      int hole = r->assembly_active && (r->deliver_upto + r->assembly_low <= r->received_high);
      if (done){
          if (st->cfg.on_message)
              st->cfg.on_message(st->cfg.user, (uint16_t)channel_idx, st->peer_ids[peer_slot],
                                 dart_bytes(r->assembly_buf, r->assembly_len));
          r->deliver_upto = base + count;
          r->assembly_active=0;
      }
      if (reliable){
          if (done){
              i_dart_reader_arm(ch,r,0); r->ack_pending=1; r->ack_due_us=0; r->ack_force=1;
              i_dart_lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
          } else if (new_fragment && hole){           /* gap revealed, or repair advanced: request now */
              i_dart_reader_arm(ch,r,0); r->ack_pending=1; r->ack_due_us=0;
              i_dart_lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
          }
      }
    }
}


void i_dart_reader_hb(DartTransportState *st, int channel_idx, int peer_slot, const uint8_t *p, uint64_t now){
    i_DartChannel *ch=&st->channels[channel_idx];
    i_DartReaderProxy *r=i_dart_reader_proxy_at(st,channel_idx,peer_slot);
    uint64_t first=i_dart_le_r64(p+DART_OFFSET_SEQNO), last=i_dart_le_r64(p+DART_OFFSET_HB_LAST);
    if (!r || !r->used) return;
    if (ch->qos.reliability!=DART_RELIABLE) return;
    /* un-started readers adopt no position from heartbeats (a one-sided flap's
       advertised first may be a dead predecessor's); the ack below carries our epoch.
       A writer raises its HB `first` to our acked_upto (the join-point trick), and our
       ACKNACK acks the contiguous-received front -- which sits INSIDE the sample we are
       still assembling. Eviction is whole-message, so a genuine floor never splits a
       sample: ignore a `first` that lands in our current partial (it is just our own
       mid-message ack echoed back), else we would skip past frags we are repairing and
       reject every resend as old. */
    if (r->started && first > r->deliver_upto &&
        (!r->assembly_active || first >= r->deliver_upto + r->assembly_count)){
        i_dart_transport_fire_event(st, DART_TRANSPORT_MSG_LOST, (uint16_t)channel_idx, st->peer_ids[peer_slot],   /* superseded before repair */
                    r->deliver_upto, first - r->deliver_upto);
        ch->repair_stats.msgs_skipped += first - r->deliver_upto;
        r->deliver_upto=first; r->assembly_active=0;
#ifdef DART_SHM
        r->shm_fail=0;          /* skipped past the stuck descriptor: fresh count */
#endif
    }
    /* hb_last is the writer's CLAIM (it may exceed what we've received). It is the only
       way to learn of tail loss -- frags past received_high that no later arrival will reveal --
       so emit lets the slow retransmit backstop chase up to it, never the fast gap path.
       The HB always owes a cumulative ack so a writer that lost ours stops re-pinging. */
    r->hb_last=last;
    i_dart_reader_arm(ch,r,1); r->ack_pending=1; r->ack_due_us=0; r->ack_force=1;
    i_dart_lane_wake(st,(uint16_t)channel_idx,(uint32_t)peer_slot);
}


/* produce a reader ACKNACK for (channel_idx,peer_slot) if due; 0 if none.
 *
 * The repair request is GAP-TRIGGERED and bounded by what we have actually RECEIVED.
 * UDP frags are assumed delivered in order, so a hole below received_high is real loss while
 * anything above it is still in flight and must NOT be NACKed -- that "request up to the
 * writer's heartbeat CLAIM, every poll" was the old congestion collapse. Each floor is
 * asked once: nack_high tracks how far we have already requested, so a progress refill asks
 * only (nack_high, top] and never re-requests the still-outstanding lower frags. A stalled
 * floor is re-asked only after the retransmit backstop (repair_delay), which is also the
 * one path allowed to chase the writer's claim (hb_last) so tail loss still repairs.
 * Outstanding repair is therefore capped at one DART_NACK_WINDOW and clocked to delivery,
 * so it cannot scale into a flood with the gap size or the message size. */
size_t i_dart_reader_emit(DartTransportState *st, int channel_idx, int peer_slot, uint8_t *out, size_t cap, uint64_t now){
    i_DartChannel *ch=&st->channels[channel_idx];
    i_DartReaderProxy *r=i_dart_reader_proxy_at(st,channel_idx,peer_slot);
    uint64_t first_missing, bound, top; uint16_t nbits=0; uint32_t bitmap=0;
    uint16_t alias = i_dart_alias_of(st, channel_idx);
    int due, holes=0, repair=0, force;
    if (!r || !r->used || st->peer_dormant[peer_slot]) return 0;   /* unmatched/dormant: don't ack */
    if (ch->qos.reliability!=DART_RELIABLE) return 0;
    if (cap<DART_HEADER_NACK) return 0;
    if (!r->ack_pending || now<r->ack_due_us) return 0;
    r->ack_pending=0; force=r->ack_force; r->ack_force=0;

    if (!r->started)                                     /* no position yet: F_UNPOS announces our epoch */
        return i_dart_wire_mk_nack(out,alias,r->deliver_upto,0,0,r->epoch,(uint8_t)DART_F_UNPOS);

    /* the cumulative-ack point and repair-window base: our contiguous-received front */
    first_missing = r->assembly_active ? r->deliver_upto + r->assembly_low : r->deliver_upto;

    /* request ceiling = what we've received. Only the slow backstop may reach the writer's
       claim, so tail loss (no later frag will ever reveal it) still gets repaired. */
    due   = (now >= r->nack_retransmit_us);
    bound = r->received_high;
    if (due && r->hb_last > bound) bound = r->hb_last;

    if (first_missing <= bound){                         /* a hole sits below something we've heard */
        holes = 1;
        top = first_missing + DART_NACK_WINDOW;          /* one window per ACKNACK: the in-flight cap */
        if (top > bound + 1) top = bound + 1;
        if (r->assembly_active && top > r->deliver_upto + r->assembly_count)
            top = r->deliver_upto + r->assembly_count;        /* this sample's frags only (bitmap range) */
        {   /* in-flight dedup (skip the still-outstanding lower part) is only valid while we
               are assembling THIS message: its clear frag_bitmap bits are genuinely in flight. With
               no sample yet (whole message missing) there is nothing we can hold, and the slow
               backstop re-asks everything, so both ask from the floor. */
            uint64_t from = (due || !r->assembly_active) ? first_missing
                : (r->nack_high > first_missing ? r->nack_high : first_missing); /* refill: only the new part */
            uint64_t s;
            for (s=from; s<top; s++){
                int missing = r->assembly_active ? !i_dart_bit_get(r->frag_bitmap,(uint32_t)(s - r->deliver_upto)) : 1;
                if (missing) bitmap |= (1u << (uint32_t)(s - first_missing));
            }
            if (bitmap){
                nbits = (uint16_t)(top - first_missing);
                repair = 1;
                ch->repair_stats.nacks_sent++;
                if (top > r->nack_high) r->nack_high = top;
                r->nack_retransmit_us = now + ch->qos.repair_delay_us;
            }
        }
    } else r->nack_high = first_missing;                   /* caught up to received: end the episode */

    /* keep the lane live while a hole remains so the backstop re-fires; an arrival that
       advances the floor re-arms us immediately (ack_due_us=0) for the next window. */
    if (holes){ r->ack_pending=1; r->ack_due_us=r->nack_retransmit_us; i_dart_transport_arm_deadline(st,r->ack_due_us); }

    /* send only to carry a repair request or a delivery/skip/HB/(re)match cumulative ack;
       a bare re-ack at an unchanged floor would be pure noise. */
    if (!repair && !force) return 0;
    return i_dart_wire_mk_nack(out,alias,first_missing,nbits,bitmap,r->epoch,0);
}
#pragma endregion
#pragma region transport/core.c
/* sans-IO reliable-UDP transport core: state, init/teardown, peer + interest matching,
 * the RX demux, and public queries. The wire codec, scheduler, and writer/reader paths
 * live in transport/{wire,sched,writer,reader}.c; shared decls in transport/internal.h. */
#include <string.h>


/* Topic identity = FNV-1a 64 of the name (i_dart_fnv1a64, common/hash.h). The interest blob
 * carries length-prefixed (not NUL-term) names, so the identity is recomputed from the
 * name on receive; init caps names at DART_TOPIC_NAME_MAX so the wire name is the whole
 * name and i_dart_identity_hash (over the wire bytes) == dart_topic_id of the same name. */
static uint64_t i_dart_identity_hash(const uint8_t *name, size_t n){ return i_dart_fnv1a64(name, n); }

uint64_t dart_topic_id(const char *name){ return i_dart_fnv1a64_str(name); }

uint64_t dart_channel_identity(const DartChannelDef *def){
    return dart_topic_id(def->name);   /* the name is the cross-peer identity */
}

static size_t i_dart_name_len(const char *s){            /* capped strlen */
    size_t n = 0;
    if (s) while (s[n] && n < DART_TOPIC_NAME_MAX) n++;
    return n;
}


/* Reader-side fragment-count bound: a peer may fragment at the smallest size in
 * the deployment, so size the reassembly bitmap by DART_FRAG_PAYLOAD_MIN. */
static uint16_t i_dart_max_fragments(uint32_t max_message_bytes){
    uint32_t f = (max_message_bytes + DART_FRAG_PAYLOAD_MIN - 1) / DART_FRAG_PAYLOAD_MIN;
    if (f == 0) f = 1;
    return (uint16_t)f;
}


/* zero-means-default for the tunable QoS fields, applied once at init so the
 * stored qos is authoritative */
static void i_dart_qos_defaults(DartQos *q, int dynamic){
    if (q->keep_last == 0)        q->keep_last       = DART_QOS_DEF_KEEP_LAST;
    if (q->heartbeat_us == 0)     q->heartbeat_us    = DART_QOS_DEF_HEARTBEAT_US;
    if (q->repair_delay_us == 0)  q->repair_delay_us = DART_QOS_DEF_REPAIR_US;
    /* fixed mode only: a dynamic channel keeps 0 = grow-to-fit via allocator */
    if (!dynamic && q->max_message_bytes == 0) q->max_message_bytes = DART_FRAG_PAYLOAD;
}


/* Normalize a node/peer UDP fragment size: 0 -> default, then clamp to [MIN,MAX].
   Public so the transport (dart_transport_init) and the node (announce blob) clamp identically. */
uint16_t dart_clamp_frag(uint16_t frag_payload){
    uint16_t f = frag_payload ? frag_payload : DART_FRAG_PAYLOAD;
    if (f < DART_FRAG_PAYLOAD_MIN) f = DART_FRAG_PAYLOAD_MIN;
    if (f > DART_FRAG_PAYLOAD_MAX) f = DART_FRAG_PAYLOAD_MAX;
    return f;
}


/* lay out everything (b->base==NULL = measure only) */
static DartTransportState *i_dart_transport_build(i_DartBump *b, const DartConfig *cfg){
    uint16_t c, p; uint32_t max_peers = cfg->max_peers, n_channels = cfg->n_channels;
    uint16_t bitmap_len = (uint16_t)((n_channels+7u)/8u);
    uint32_t meta_ids = DART_META_MAX_IDS;
    uint32_t name_bytes = 0; char *name_pool = NULL;
    DartTransportState *st = (DartTransportState*)i_dart_bump_take(b, sizeof(DartTransportState), 16);
    if (st && b->base) memset(st, 0, sizeof(*st));

    /* name pool: one fixed-size slot per channel so a reserve-mode slot can be named
       later by dart_transport_channel_define without repacking. ch->name points at its slot. */
    name_bytes = (uint32_t)n_channels * (DART_TOPIC_NAME_MAX + 1u);
    if (meta_ids < 2u*n_channels) meta_ids = 2u*n_channels;     /* our own interest list must always fit */

    { uint32_t nlanes = n_channels*max_peers, ndest = max_peers;
      int dyn = (cfg->allocator != NULL);
      uint32_t *peer_ids = (uint32_t*)i_dart_bump_take(b, max_peers*sizeof(uint32_t), 8);
      uint8_t  *peer_used = (uint8_t*) i_dart_bump_take(b, max_peers*sizeof(uint8_t), 1);
      uint8_t  *peer_dormant= (uint8_t*) i_dart_bump_take(b, max_peers*sizeof(uint8_t), 1);
      uint16_t *peer_frag = (uint16_t*)i_dart_bump_take(b, max_peers*sizeof(uint16_t), 2);
#ifdef DART_SHM
      uint8_t  *peer_shm= (uint8_t*) i_dart_bump_take(b, max_peers*sizeof(uint8_t), 1);
#endif
      uint8_t  *peer_pub_bitmap = (uint8_t*) i_dart_bump_take(b, (size_t)max_peers*bitmap_len, 1);
      uint8_t  *peer_sub_bitmap = (uint8_t*) i_dart_bump_take(b, (size_t)max_peers*bitmap_len, 1);
      uint8_t  *peer_sub_reliable = (uint8_t*) i_dart_bump_take(b, (size_t)max_peers*bitmap_len, 1);
      i_DartChannel *ch = (i_DartChannel*)i_dart_bump_take(b, n_channels*sizeof(i_DartChannel), 16);
      /* lanes: dynamic keeps only the u16 ticket table (records are pool-allocated per real
         match, so memory scales with matches); fixed embeds the dense record array, whose
         reassembly buffers are pre-bound per channel below */
      uint16_t   *lane_index = dyn ? (uint16_t*)i_dart_bump_take(b, (size_t)nlanes*sizeof(uint16_t), 2) : NULL;
      i_DartLane *lanes      = dyn ? NULL : (i_DartLane*)i_dart_bump_take(b, (size_t)nlanes*sizeof(i_DartLane), 16);
      uint32_t *dest_head = (uint32_t*)i_dart_bump_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint32_t *dest_tail = (uint32_t*)i_dart_bump_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint8_t  *dest_queued = (uint8_t*) i_dart_bump_take(b, (size_t)ndest, 1);
      uint32_t *dest_queue = (uint32_t*)i_dart_bump_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      /* alias maps: per-peer pointer + length; dynamic allocates each map on demand at the
         peer's advertised size, fixed pre-slices a dense meta_ids-stride pool (as before) */
      uint16_t **peer_alias    = (uint16_t**)i_dart_bump_take(b, (size_t)max_peers*sizeof(uint16_t*), 8);
      uint32_t *peer_alias_len = (uint32_t*) i_dart_bump_take(b, (size_t)max_peers*sizeof(uint32_t), 8);
      uint16_t *alias_pool     = dyn ? NULL
                               : (uint16_t*)i_dart_bump_take(b, (size_t)max_peers*meta_ids*sizeof(uint16_t), 2);
      name_pool = (char*)i_dart_bump_take(b, name_bytes ? name_bytes : 1u, 1);
      if (st && b->base){
          st->cfg=*cfg; st->peer_ids=peer_ids; st->peer_used=peer_used;
          st->peer_dormant=peer_dormant; st->peer_frag=peer_frag;
          st->frag = dart_clamp_frag(cfg->frag_payload);
          st->peer_pub_bitmap=peer_pub_bitmap; st->peer_sub_bitmap=peer_sub_bitmap;
          st->peer_sub_reliable=peer_sub_reliable; st->bitmap_len=bitmap_len;
          st->channels=ch; st->reader_epoch_counter=1;
          st->next_deadline_us=DART__NO_DEADLINE;
          st->lanes=lanes; st->lane_cap = dyn ? 0u : nlanes; st->lane_free=DART__NIL;
          st->lane_index=lane_index;
          st->dest_head=dest_head; st->dest_tail=dest_tail; st->dest_queued=dest_queued; st->dest_queue=dest_queue;
          st->peer_alias=peer_alias; st->peer_alias_len=peer_alias_len; st->alias_max=meta_ids;
          memset(peer_used,0,max_peers); memset(peer_dormant,0,max_peers);
          { uint32_t k; for (k=0;k<max_peers;k++) peer_frag[k]=DART_FRAG_PAYLOAD; }  /* set per peer on add */
#ifdef DART_SHM
          st->peer_shm=peer_shm; memset(peer_shm,0,max_peers);
#endif
          if (dyn){
              memset(peer_alias, 0, (size_t)max_peers*sizeof(uint16_t*));
              memset(peer_alias_len, 0, (size_t)max_peers*sizeof(uint32_t));
          } else {
              uint32_t k;
              memset(alias_pool,0xFF,(size_t)max_peers*meta_ids*sizeof(uint16_t));   /* all unmapped */
              for (k=0;k<max_peers;k++){
                  peer_alias[k] = alias_pool + (size_t)k*meta_ids;
                  peer_alias_len[k] = meta_ids;
              }
          }
          memset(peer_pub_bitmap,0,(size_t)max_peers*bitmap_len); memset(peer_sub_bitmap,0,(size_t)max_peers*bitmap_len);
          memset(peer_sub_reliable,0,(size_t)max_peers*bitmap_len);
          if (dyn) memset(lane_index,0xFF,(size_t)nlanes*sizeof(uint16_t));   /* all unmatched */
          else {
              uint32_t li;
              memset(lanes,0,(size_t)nlanes*sizeof(i_DartLane));
              for (li=0;li<nlanes;li++){          /* fixed: identity records, permanent */
                  lanes[li].channel   = (uint16_t)(li / max_peers);
                  lanes[li].peer_slot = (uint16_t)(li % max_peers);
                  lanes[li].sched_next = DART__NIL; lanes[li].ch_next = DART__NIL;
                  lanes[li].in_use = 1;
              }
          }
          memset(dest_queued,0,ndest);
          memset(dest_head,0xFF,(size_t)ndest*sizeof(uint32_t));   /* all DART__NIL */
      }
    }

    for (c=0;c<n_channels;c++){
        /* every slot starts inactive with its own name-pool slot; reserve-mode slots
           stay this way until dart_transport_channel_define fills them. */
        if (st && b->base){
            i_DartChannel *ch = &st->channels[c];
            memset(ch,0,sizeof(*ch));
            ch->role = DART_INACTIVE;
            ch->lane_head = DART__NIL;
            ch->name = name_pool + (size_t)c*(DART_TOPIC_NAME_MAX + 1u);
            ((char*)ch->name)[0] = '\0';
        }
        if (!cfg->channels) continue;    /* reserve mode: arena holds no per-channel buffers */
        {   const DartChannelDef *def = &cfg->channels[c];
            /* dynamic = an allocator is set: buffers grow via the hook, not the arena */
            int dyn = (cfg->allocator != NULL);
            DartQos q = def->qos;            /* local, normalized copy */
            i_DartWriterSample *history; uint16_t depth, max_fragments, d;
            i_dart_qos_defaults(&q, dyn);
            depth = q.keep_last;
            max_fragments = i_dart_max_fragments(q.max_message_bytes);
            history = (i_DartWriterSample*)i_dart_bump_take(b, depth*sizeof(i_DartWriterSample), 16);
            if (st && b->base){
                i_DartChannel *ch = &st->channels[c];
                size_t lane = i_dart_name_len(def->name);
                ch->qos=q; ch->max_fragments=max_fragments;
                ch->role=def->role; ch->dynamic=(uint8_t)dyn;
                ch->identity = dart_channel_identity(def);
                if (lane){ memcpy((char*)ch->name, def->name, lane); ((char*)ch->name)[lane]='\0'; }
                ch->name_len = (uint8_t)lane;
                ch->history=history; ch->history_owned=0; ch->history_head=0; ch->next_seqno=0; ch->have_first=0;
                memset(history,0,depth*sizeof(i_DartWriterSample));
            }
            for (d=0; d<depth; d++){
                uint8_t *buf = dyn ? NULL : (uint8_t*)i_dart_bump_take(b, q.max_message_bytes, 8);
                if (st && b->base){ st->channels[c].history[d].buf = buf;
                                    st->channels[c].history[d].cap = dyn ? 0u : q.max_message_bytes; }
            }
            /* reader asm buffers + frag bitmaps, per peer. Fixed mode only: it binds into
               the permanent identity records; a dynamic record starts empty and grows via
               the hook (and does not exist yet here). */
            if (!dyn) for (p=0;p<max_peers;p++){
                uint8_t *assembly_buf = (uint8_t*)i_dart_bump_take(b, q.max_message_bytes, 8);
                uint8_t *frag_bitmap  = (uint8_t*)i_dart_bump_take(b, (max_fragments+7u)/8u, 1);
                if (st && b->base){
                    i_DartReaderProxy *r = i_dart_reader_proxy_at(st,c,p);
                    r->assembly_buf=assembly_buf; r->frag_bitmap=frag_bitmap;
                    r->assembly_cap = q.max_message_bytes;
                    r->bitmap_cap  = (uint32_t)((max_fragments+7u)/8u);
                }
            }
        }
    }
    return st;
}


size_t dart_transport_required_memory(const DartConfig *cfg){
    i_DartBump b; memset(&b,0,sizeof b);
    if (!cfg || cfg->n_channels==0 || cfg->max_peers==0) return 0;
    i_dart_transport_build(&b, cfg);
    return b.offset + 16;   /* slack for base alignment */
}


DartTransportState *dart_transport_init(void *mem, size_t cap, const DartConfig *cfg){
    i_DartBump b; DartTransportState *st; uint16_t i;
    if (!mem || !cfg || cfg->n_channels==0 || cfg->max_peers==0) return NULL;
    if (!cfg->channels && !cfg->allocator) return NULL;    /* reserve mode needs an allocator */
    if (cfg->channels) for (i=0;i<cfg->n_channels;i++){
        const DartChannelDef *d = &cfg->channels[i];
        size_t lane = 0;
        if (!d->name || !d->name[0]) return NULL;          /* name = identity, required */
        while (d->name[lane]) lane++;
        if (lane > DART_TOPIC_NAME_MAX) return NULL;           /* the wire name is the whole name */
    }
    memset(&b,0,sizeof b);
    b.base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    b.cap  = cap - (size_t)((uint8_t*)b.base - (uint8_t*)mem);
    st = i_dart_transport_build(&b, cfg);
    if (!st || b.oom) return NULL;
    st->cfg.channels = NULL;   /* only read during init; detach the caller's pointer */
    return st;
}


/* Relocate a live transport into a bigger block at grown counts (dynamic-mode growth).
 * Heap buffers (history rings, sample/assembly bufs, frag bitmaps) are NOT in the arena,
 * so the struct copies carry their pointers across and the OLD arena can be freed without
 * touching them. The 2D tables are re-strided into the new max_peers/n_channels; the
 * active-lane scheduler (indices encode the old strides) is dropped and rebuilt from the
 * proxy state. The caller frees old's arena block afterward; it must NOT dart_transport_destroy old
 * (that would free the heap buffers now owned by the new state). Returns the new state. */
DartTransportState *dart_transport_migrate(DartTransportState *old, void *new_mem, size_t new_cap,
                        uint16_t new_max_peers, uint16_t new_n_channels){
    DartConfig nc; DartTransportState *nw; uint16_t omp, onc, c, p;
    if (!old) return NULL;
    nc = old->cfg; nc.channels = NULL;
    nc.max_peers = new_max_peers; nc.n_channels = new_n_channels;
    nw = dart_transport_init(new_mem, new_cap, &nc);
    if (!nw) return NULL;
    omp = old->cfg.max_peers; onc = old->cfg.n_channels;

    nw->reader_epoch_counter = old->reader_epoch_counter;
    nw->frag = old->frag;
    memcpy(nw->peer_ids,     old->peer_ids,     (size_t)omp*sizeof(uint32_t));
    memcpy(nw->peer_used,    old->peer_used,    omp);
    memcpy(nw->peer_dormant, old->peer_dormant, omp);
    memcpy(nw->peer_frag,    old->peer_frag,    (size_t)omp*sizeof(uint16_t));
#ifdef DART_SHM
    memcpy(nw->peer_shm,     old->peer_shm,     omp);
#endif
    /* channels: keep the new name-pool slot pointer, carry everything else (incl. the
       heap history ring pointer) and re-copy the name string into the new pool */
    for (c=0;c<onc;c++){
        char *nm = (char*)nw->channels[c].name;
        size_t l = old->channels[c].name_len;
        nw->channels[c] = old->channels[c];   /* struct copy carries name_len */
        nw->channels[c].name = nm;
        if (l) memcpy(nm, old->channels[c].name, l);
        nm[l] = '\0';
    }
    /* lane records: the pool is ONE hook allocation outside both arenas, so adopt it
       wholesale (record backrefs use channel indices + peer slots, both preserved; the
       channels' lane_head chains were carried by the struct copies above). Only the
       ticket table is arena memory: re-stride it into the new max_peers. */
    nw->lanes = old->lanes; nw->lane_cap = old->lane_cap; nw->lane_free = old->lane_free;
    for (c=0;c<onc;c++) for (p=0;p<omp;p++)
        nw->lane_index[(size_t)c*new_max_peers+p] = old->lane_index[(size_t)c*omp+p];
    {   uint32_t li;   /* the old scheduler dies with the old arena: clear per-record state
                          (free-list records keep sched_next: it is their free link) */
        for (li=0; li<nw->lane_cap; li++)
            if (nw->lanes[li].in_use){ nw->lanes[li].queued=0; nw->lanes[li].sched_next=DART__NIL; }
    }
    /* per-peer interest bitmaps (stride grows with n_channels) + alias table */
    for (p=0;p<omp;p++){
        memcpy(nw->peer_pub_bitmap + (size_t)p*nw->bitmap_len,
               old->peer_pub_bitmap + (size_t)p*old->bitmap_len, old->bitmap_len);
        memcpy(nw->peer_sub_bitmap + (size_t)p*nw->bitmap_len,
               old->peer_sub_bitmap + (size_t)p*old->bitmap_len, old->bitmap_len);
        memcpy(nw->peer_sub_reliable + (size_t)p*nw->bitmap_len,
               old->peer_sub_reliable + (size_t)p*old->bitmap_len, old->bitmap_len);
        nw->peer_alias[p]     = old->peer_alias[p];   /* hook allocations: stable across the move */
        nw->peer_alias_len[p] = old->peer_alias_len[p];
    }
    /* scheduler is fresh/empty: re-enqueue every used lane, then force a full sweep
       next poll so timers re-arm and next_deadline is recomputed exactly */
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

/* the local handle IS the channel's index; out-of-range rejected */
i_DartChannel *i_dart_channel_at(DartTransportState *st, uint16_t channel, int *idx_out){
    if (channel >= st->cfg.n_channels) return NULL;
    if (idx_out) *idx_out = (int)channel;
    return &st->channels[channel];
}

/* Find the local channel for a wire identity. An INACTIVE channel (declared but off) must
 * not shadow an active same-identity channel, so prefer a non-INACTIVE match; fall back to
 * the first match (e.g. all inactive) so resolution stays deterministic. Lets a caller hold
 * two channels of one identity (different QoS) and switch which is live by role. */
static i_DartChannel *i_dart_channel_by_identity(DartTransportState *st, uint64_t identity, int *idx_out){
    uint16_t i; int first=-1;
    for (i=0;i<st->cfg.n_channels;i++){
        if (st->channels[i].identity!=identity) continue;
        if (first<0) first=(int)i;
        if (st->channels[i].role!=DART_INACTIVE){ if(idx_out)*idx_out=(int)i; return &st->channels[i]; }
    }
    if (first>=0){ if(idx_out)*idx_out=first; return &st->channels[first]; }
    return NULL;
}


/* fire one DartTransportEvent (no-op if no on_event). first/count are the kind's two
 * numeric slots; route them to named fields. A channel name is not carried: a consumer
 * reads it with dart_transport_channel_name(st, ev.channel). */
void i_dart_transport_fire_event(DartTransportState *st, DartTransportEventKind kind, uint16_t channel,
                        uint32_t peer, uint64_t first, uint64_t count){
    DartTransportEvent ev;
    if (!st->cfg.on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind=kind; ev.channel=channel; ev.peer=peer; ev.user=st->cfg.user;
    switch (kind){
    case DART_TRANSPORT_MSG_LOST:       ev.lost_first = first; ev.lost_count = count; break;
    case DART_TRANSPORT_MSG_TOO_BIG:    ev.too_big_bytes = count; break;
    case DART_TRANSPORT_NAME_COLLISION: ev.identity = first; break;
    default: break;
    }
    st->cfg.on_event(&ev);
}

/* dart_event_str (and its bounded appenders) moved to the node (node/core.c): the
   formatter covers the node's app-facing DartEvent union, not the transport's own
   events. The transport stays independent of the node's event vocabulary. */


/* unicast join seqno: head minus qos.catch_up cached samples (reliable only) */
uint64_t i_dart_channel_unicast_join_seqno(const i_DartChannel *ch){
    uint16_t depth = ch->qos.keep_last;   /* normalized at init (>=1) */
    uint16_t want = ch->qos.catch_up, k, i;
    uint64_t s = ch->next_seqno;
    if (ch->qos.reliability != DART_RELIABLE || want == 0) return s;
    if (want > depth) want = depth;
    i = ch->history_head;
    for (k=0; k<want; k++){
        uint16_t j = (uint16_t)(i ? i-1 : depth-1);
        if (!ch->history[j].valid) break;        /* fewer than want cached */
        s = ch->history[j].base;
        i = j;
    }
    return s;
}


/* Get-or-allocate the lane record for (c,peer_slot). Dynamic mode grows the pool by
 * doubling through the hook; records MOVE on growth, so callers re-derive any lane
 * pointer after this call. Returns NULL on OOM or a full u16 ticket space: the match is
 * refused for now (the peer's next announce re-applies and retries). Fixed mode always
 * succeeds (the identity record is permanent). */
static i_DartLane *i_dart_lane_ensure(DartTransportState *st, uint16_t c, uint32_t peer_slot){
    size_t k = (size_t)c*st->cfg.max_peers + peer_slot;
    uint32_t li;
    if (!st->lane_index) return &st->lanes[k];        /* fixed: identity */
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
    st->lanes[li].channel = c; st->lanes[li].peer_slot = (uint16_t)peer_slot;
    st->lanes[li].sched_next = DART__NIL; st->lanes[li].ch_next = DART__NIL;
    st->lanes[li].in_use = 1;
    st->lane_index[k] = (uint16_t)li;
    return &st->lanes[li];
}

/* A lane with neither side matched leaves the channel chain; dynamic mode also frees its
 * grown reassembly buffers, drops any scheduler entry (a recycled record must never sit
 * on another peer's dest list), and recycles the record. No-op while a side is matched. */
static void i_dart_lane_release(DartTransportState *st, uint16_t c, uint32_t peer_slot){
    uint32_t li = i_dart_lane_id(st, c, peer_slot);
    i_DartLane *l;
    if (li == DART__NIL) return;
    l = &st->lanes[li];
    if (l->w.used || l->r.used) return;
    {   uint32_t *pp = &st->channels[c].lane_head;    /* unlink from the channel chain */
        while (*pp != DART__NIL && *pp != li) pp = &st->lanes[*pp].ch_next;
        if (*pp == li) *pp = l->ch_next;
    }
    l->ch_next = DART__NIL;
    if (!st->lane_index) return;                      /* fixed: the record itself is permanent */
    if (l->r.assembly_buf){ st->cfg.allocator(st->cfg.user, l->r.assembly_buf, 0); l->r.assembly_buf=NULL; l->r.assembly_cap=0; }
    if (l->r.frag_bitmap){ st->cfg.allocator(st->cfg.user, l->r.frag_bitmap, 0); l->r.frag_bitmap=NULL; l->r.bitmap_cap=0; }
    i_dart_sched_drop(st, li);
    l->in_use = 0;
    l->sched_next = st->lane_free; st->lane_free = li;
    st->lane_index[(size_t)c*st->cfg.max_peers + peer_slot] = 0xFFFF;
}

/* match one (channel,peer) lane side: it carries new data, repairs, acks/HB */
static void i_dart_writer_match(DartTransportState *st, uint16_t c, uint16_t peer_slot, i_DartLane *l){
    i_DartChannel *ch=&st->channels[c];
    i_DartWriterProxy *w=&l->w;
    memset(w,0,sizeof(*w));
    w->used=1;
    ch->matched_writers++;   /* only reached on a genuine 0->1 (rematch guards on !used) */
    /* only a reader that advertised RELIABLE acks; a best-effort reader stays out of
       flow control so it can't stall this writer (it gets new data, never repairs/HB) */
    w->reader_reliable = i_dart_bit_get(&st->peer_sub_reliable[(size_t)peer_slot*st->bitmap_len], c) ? 1u : 0u;
    w->sent_upto = i_dart_channel_unicast_join_seqno(ch);
    w->acked_upto = w->sent_upto;
    i_dart_lane_wake(st, c, peer_slot);   /* lane primed for new data + ack/hb */
}

static void i_dart_writer_unmatch(DartTransportState *st, uint16_t c, i_DartLane *l){
    if (!l->w.used) return;
    l->w.used=0;
    st->channels[c].matched_writers--;   /* guarded on used above: exactly one 1->0 per unmatch */
}

static void i_dart_reader_match(DartTransportState *st, uint16_t c, uint16_t peer_slot, i_DartLane *l){
    i_DartReaderProxy *r=&l->r;
    uint8_t *assembly_buf=r->assembly_buf, *frag_bitmap=r->frag_bitmap;
    uint32_t assembly_cap=r->assembly_cap, bitmap_cap=r->bitmap_cap;   /* keep grown buffers across rematch */
    memset(r,0,sizeof(*r));
    r->assembly_buf=assembly_buf; r->frag_bitmap=frag_bitmap; r->assembly_cap=assembly_cap; r->bitmap_cap=bitmap_cap;
    r->epoch=st->reader_epoch_counter++;   /* new incarnation: writers re-join on seeing it */
    r->used=1;       /* started==0: first DATA adopts the writer's position */
    st->channels[c].matched_readers++;   /* only reached on a genuine 0->1 (rematch guards on !used) */
    /* announce this incarnation once so a caught-up (idle, non-pinging) writer
       re-joins and replays. A genuine discovery blip keeps its position through
       dart_transport_peer_dormant/resume and never lands here, so a single ACKNACK suffices. */
    if (st->channels[c].qos.reliability==DART_RELIABLE){
        r->ack_pending=1; r->ack_due_us=0; r->ack_force=1;
        i_dart_lane_wake(st,c,peer_slot);
    }
}

static void i_dart_reader_unmatch(DartTransportState *st, uint16_t c, i_DartLane *l){
    if (l->r.used) st->channels[c].matched_readers--;   /* peer_remove calls this unconditionally */
    l->r.used=0; l->r.assembly_active=0;
}


/* recompute one (channel,peer) match from our role and the peer's interest bits.
 * Lane records exist only while a side is matched: the unmatched->matched edge allocates
 * (and links the channel chain), the matched->unmatched edge releases. Idempotent
 * re-application never touches a lane whose match state did not change, so reader
 * positions survive it exactly as before. */
static void i_dart_channel_rematch(DartTransportState *st, uint16_t c, uint16_t peer_slot){
    i_DartChannel *ch=&st->channels[c];
    const uint8_t *peer_pub_bitmap=&st->peer_pub_bitmap[(size_t)peer_slot*st->bitmap_len];
    const uint8_t *peer_sub_bitmap=&st->peer_sub_bitmap[(size_t)peer_slot*st->bitmap_len];
    int wuse = (ch->role==DART_PUBSUB || ch->role==DART_PUB_ONLY) && i_dart_bit_get(peer_sub_bitmap,c);
    int ruse = (ch->role==DART_PUBSUB || ch->role==DART_SUB_ONLY) && i_dart_bit_get(peer_pub_bitmap,c);
    i_DartLane *l = i_dart_lane_at(st,c,peer_slot);
    int had = l && (l->w.used || l->r.used);
    if (!wuse && !ruse){
        if (!had) return;
        i_dart_writer_unmatch(st,c,l);
        i_dart_reader_unmatch(st,c,l);
        i_dart_lane_release(st,c,peer_slot);
        return;
    }
    if (!l){
        l = i_dart_lane_ensure(st,c,peer_slot);   /* may relocate the pool: l is fresh */
        if (!l) return;                           /* OOM: refused; the peer's next announce retries */
    }
    if (wuse && !l->w.used) i_dart_writer_match(st,c,peer_slot,l);
    else if (!wuse && l->w.used) i_dart_writer_unmatch(st,c,l);
    if (ruse && !l->r.used) i_dart_reader_match(st,c,peer_slot,l);
    else if (!ruse && l->r.used) i_dart_reader_unmatch(st,c,l);
    if (!had && (l->w.used || l->r.used)){        /* first match on this lane: onto the channel chain */
        l->ch_next = ch->lane_head;
        ch->lane_head = i_dart_lane_id(st,c,peer_slot);
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
    st->peer_dormant[free]=0;
    st->peer_frag[free]=dart_clamp_frag(peer_frag);
#ifdef DART_SHM
    st->peer_shm[free]=0;   /* node sets it once the peer's segment is attached */
#endif
    memset(&st->peer_pub_bitmap[(size_t)free*st->bitmap_len],0,st->bitmap_len);
    memset(&st->peer_sub_bitmap[(size_t)free*st->bitmap_len],0,st->bitmap_len);
    memset(&st->peer_sub_reliable[(size_t)free*st->bitmap_len],0,st->bitmap_len);
    if (st->peer_alias[free] && st->peer_alias_len[free])   /* slot reuse: no stale mappings */
        memset(st->peer_alias[free],0xFF,(size_t)st->peer_alias_len[free]*sizeof(uint16_t));
    /* nothing matches until dart_transport_apply_peer_interest feeds the peer's interest
       list (carried in its discovery announce) */
}


void dart_transport_peer_remove(DartTransportState *st, uint32_t id){
    int s = i_dart_peer_slot(st,id); uint16_t c;
    if (s<0) return;
    for (c=0;c<st->cfg.n_channels;c++){
        i_DartLane *l = i_dart_lane_at(st,c,(uint32_t)s);
        if (!l) continue;
        i_dart_writer_unmatch(st,c,l);
        i_dart_reader_unmatch(st,c,l);
        /* release recycles the record AND frees the lane's grown reassembly buffers, so a
           gone peer keeps no per-lane memory at all */
        i_dart_lane_release(st,c,(uint32_t)s);
    }
    if (st->cfg.allocator && st->peer_alias[s]){   /* dynamic: the alias map goes too */
        st->cfg.allocator(st->cfg.user, st->peer_alias[s], 0);
        st->peer_alias[s]=NULL; st->peer_alias_len[s]=0;
    }
    st->peer_used[s]=0; st->peer_dormant[s]=0;
#ifdef DART_SHM
    st->peer_shm[s]=0;
#endif
}


/* A peer fell silent (discovery timeout): keep every proxy and the reader's
 * deliver position, just drop the peer from flow control so a dead reader can't
 * stall the writer and a dead writer isn't acked. State revives via dart_transport_peer_resume. */
void dart_transport_peer_dormant(DartTransportState *st, uint32_t id){
    int s = i_dart_peer_slot(st,id);
    if (s>=0) st->peer_dormant[s]=1;
}


/* A dormant peer's SAME incarnation returned: re-include it in flow control and
 * re-report each reader position so the writer fills any gap (the reader dedups any
 * replay for free). The writer side needs nothing proactive; the reader's ACKNACK
 * re-arms its heartbeats. Proxies and deliver_upto were never touched, so no dup,
 * no loss. */
void dart_transport_peer_resume(DartTransportState *st, uint32_t id){
    int s = i_dart_peer_slot(st,id); uint16_t c;
    if (s<0) return;
    st->peer_dormant[s]=0;
    for (c=0;c<st->cfg.n_channels;c++){
        i_DartLane *l=i_dart_lane_at(st,c,(uint32_t)s);
        if (!l) continue;
        if (st->channels[c].qos.reliability!=DART_RELIABLE) continue;
        if (l->r.used){ l->r.ack_pending=1; l->r.ack_due_us=0; l->r.ack_force=1; }  /* report our position now */
        if (l->w.used || l->r.used) i_dart_lane_wake(st,c,(uint16_t)s);
    }
}


/* update a peer's advertised fragment size (its blob may arrive after first contact) */
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
    if (!st || !st->cfg.allocator) return;     /* fixed mode: nothing hook-allocated */
    for (c=0;c<st->cfg.n_channels;c++){
        i_DartChannel *ch=&st->channels[c];
        uint16_t depth, d;
        if (!ch->dynamic) continue;
        depth = ch->qos.keep_last;
        for (d=0; d<depth; d++)
            if (ch->history[d].buf){ st->cfg.allocator(st->cfg.user, ch->history[d].buf, 0);
                                  ch->history[d].buf=NULL; ch->history[d].cap=0; }
        if (ch->history_owned && ch->history){   /* ring allocated by dart_transport_channel_define */
            st->cfg.allocator(st->cfg.user, ch->history, 0);
            ch->history=NULL; ch->history_owned=0;
        }
    }
    for (li=0; li<st->lane_cap; li++){           /* live records' grown reassembly buffers */
        i_DartLane *l=&st->lanes[li];
        if (!l->in_use) continue;
        if (l->r.assembly_buf){ st->cfg.allocator(st->cfg.user, l->r.assembly_buf, 0); l->r.assembly_buf=NULL; l->r.assembly_cap=0; }
        if (l->r.frag_bitmap){ st->cfg.allocator(st->cfg.user, l->r.frag_bitmap, 0); l->r.frag_bitmap=NULL; l->r.bitmap_cap=0; }
    }
    if (st->lane_index && st->lanes){            /* the record pool itself (one hook allocation) */
        st->cfg.allocator(st->cfg.user, st->lanes, 0);
        st->lanes=NULL; st->lane_cap=0; st->lane_free=DART__NIL;
    }
    {   uint32_t p;                              /* per-peer alias maps (hook allocations) */
        for (p=0;p<st->cfg.max_peers;p++)
            if (st->peer_alias[p]){
                st->cfg.allocator(st->cfg.user, st->peer_alias[p], 0);
                st->peer_alias[p]=NULL; st->peer_alias_len[p]=0;
            }
    }
}


/* per-entry flags byte (interest is sent rarely, so a whole byte, not a stolen bit) */
#define DART_META_F_RELIABLE 0x01u   /* advertiser offers reliable delivery on this topic */

/* one interest entry: [u16 alias][u8 flags][u8 namelen][name]. The name rides along so
 * a hash collision is detected (not cross-wired); the identity is recomputed from it. */
static uint8_t *i_dart_meta_put(uint8_t *p, uint16_t alias, const i_DartChannel *ch){
    size_t lane = ch->name_len;
    i_dart_le_w16(p, alias); p += 2;
    *p++ = (uint8_t)(ch->qos.reliability==DART_RELIABLE ? DART_META_F_RELIABLE : 0u);
    *p++ = (uint8_t)lane;
    if (lane){ memcpy(p, ch->name, lane); p += lane; }
    return p;
}

static int i_dart_meta_name_eq(const i_DartChannel *ch, const uint8_t *name, size_t nlen){
    size_t ours = ch->name_len;
    if (nlen != ours) return 0;
    return nlen==0 ? 1 : (memcmp(ch->name, name, nlen)==0);
}

/* The peer's alias map, guaranteed to cover `need` entries: the existing map, a grown/new
 * hook allocation (dynamic; the new tail starts unmapped), or NULL when it cannot grow
 * (fixed-mode arena slice too small, or OOM) -- the caller then counts the entry as
 * unmappable. Called only for MATCHED entries and sized to the peer's highest ADVERTISED
 * alias, so a peer we share nothing with allocates nothing, and a later local subscribe
 * (interest replay) finds every advertised alias already covered. */
static uint16_t *i_dart_peer_alias_ensure(DartTransportState *st, int peer_slot, uint32_t need){
    uint32_t have = st->peer_alias_len[peer_slot];
    uint16_t *nm;
    if (need <= have) return st->peer_alias[peer_slot];
    if (!st->cfg.allocator) return NULL;                /* fixed: the arena slice is the limit */
    nm = (uint16_t*)st->cfg.allocator(st->cfg.user, st->peer_alias[peer_slot], (size_t)need*sizeof(uint16_t));
    if (!nm) return NULL;
    memset(nm + have, 0xFF, (size_t)(need-have)*sizeof(uint16_t));   /* grown tail: unmapped */
    st->peer_alias[peer_slot] = nm; st->peer_alias_len[peer_slot] = need;
    return nm;
}

/* match count entries to local channels by identity (recomputed from each name),
 * recording the alias map. Same-identity-different-name is a collision: refused.
 * is_pub: the peer's publish list, so each entry's flags carry its OFFERED QoS, which
 * the RxO check uses to refuse a reliable subscriber a best-effort publisher. rel_bitmap
 * (sub list only, else NULL): records which subscribed channels the peer requested RELIABLE,
 * so the writer can keep best-effort readers out of flow control. alias_need: highest
 * advertised alias + 1 (the size a map must be to cover this peer's whole list). */
static const uint8_t *i_dart_meta_scan(DartTransportState *st, int peer_slot, const uint8_t *p,
                                      uint32_t count, uint8_t *bitmap, int is_pub, uint8_t *rel_bitmap,
                                      uint32_t alias_need, uint32_t *overflowed){
    uint32_t k;
    for (k=0;k<count;k++){
        uint16_t alias=i_dart_le_r16(p); uint8_t flags=p[2]; uint32_t nlen=p[3];
        const uint8_t *name=p+4; int channel_idx;
        uint64_t id=i_dart_identity_hash(name,nlen);
        i_DartChannel *ch=i_dart_channel_by_identity(st,id,&channel_idx);
        p = name + nlen;
        if (!ch) continue;                                  /* not ours */
        if (!i_dart_meta_name_eq(ch,name,nlen)){
            i_dart_transport_fire_event(st, DART_TRANSPORT_NAME_COLLISION, (uint16_t)channel_idx, st->peer_ids[peer_slot],
                        id, 0);
            continue;
        }
        /* RxO QoS: a reliable subscriber refuses a best-effort publisher (no silent
           downgrade). We keep requesting reliable, so the match forms automatically if
           the publisher later upgrades and re-advertises. */
        if (is_pub && (ch->role==DART_PUBSUB || ch->role==DART_SUB_ONLY) &&
            ch->qos.reliability==DART_RELIABLE && !(flags & DART_META_F_RELIABLE)){
            i_dart_transport_fire_event(st, DART_TRANSPORT_QOS_INCOMPATIBLE, (uint16_t)channel_idx,
                        st->peer_ids[peer_slot], 0, 0);
            continue;                                       /* refuse: no bit, no alias map */
        }
        /* schema gate (RxO for types): both sides run the same check off the same two
           advertised schemas, so a refused pair forms no proxy on either end (the writer
           never streams to, or flow-controls on, a reader that will not decode it). */
        if (st->cfg.schema_check &&
            !st->cfg.schema_check(st->cfg.user, (uint16_t)channel_idx, alias, is_pub)){
            i_dart_transport_fire_event(st, DART_TRANSPORT_SCHEMA_MISMATCH, (uint16_t)channel_idx,
                        st->peer_ids[peer_slot], 0, 0);
            continue;                                       /* refuse: no bit, no alias map */
        }
        i_dart_bit_set(bitmap,(uint32_t)channel_idx);
        if (rel_bitmap && (flags & DART_META_F_RELIABLE)) i_dart_bit_set(rel_bitmap,(uint32_t)channel_idx);
        {   uint16_t *map = i_dart_peer_alias_ensure(st, peer_slot, alias_need);
            if (map && (uint32_t)alias < st->peer_alias_len[peer_slot])
                map[alias] = (uint16_t)channel_idx;
            else if (is_pub && overflowed)
                (*overflowed)++;   /* matched, but its data carries an alias we cannot demux
                                      (fixed-mode table too small, or map OOM) */
        }
    }
    return p;
}


/* Upper bound on dart_transport_build_interest output, for sizing the announce buffer: a
 * PUBSUB channel appears in both lists, so 2*n_channels max-length entries. */
size_t dart_interest_max(uint16_t n_channels){
    return 4u + (size_t)(2u+1u+1u+DART_TOPIC_NAME_MAX) * 2u * (size_t)n_channels;  /* alias+flags+namelen+name */
}


/* Serialize our interest into out: [u16 npub][u16 nsub][pub..][sub..], each entry
 * [u16 alias][u8 namelen][name]. Returns bytes written, or 0 if cap is too small.
 * The node carries this in its discovery announce; size out via dart_interest_max. */
size_t dart_transport_build_interest(DartTransportState *st, void *out, size_t cap){
    uint8_t *o=(uint8_t*)out, *p, *end=o+cap;
    uint16_t c; uint32_t n_pub=0, n_sub=0;
    if (cap < 4) return 0;
    p=o+4;
    for (c=0;c<st->cfg.n_channels;c++){
        uint8_t d=st->channels[c].role;
        if (d==DART_PUBSUB || d==DART_PUB_ONLY){
            if (p + 4u + st->channels[c].name_len > end) return 0;
            p=i_dart_meta_put(p,c,&st->channels[c]); n_pub++;
        }
    }
    for (c=0;c<st->cfg.n_channels;c++){
        uint8_t d=st->channels[c].role;
        if (d==DART_PUBSUB || d==DART_SUB_ONLY){
            if (p + 4u + st->channels[c].name_len > end) return 0;
            p=i_dart_meta_put(p,c,&st->channels[c]); n_sub++;
        }
    }
    i_dart_le_w16(o,(uint16_t)n_pub); i_dart_le_w16(o+2,(uint16_t)n_sub);
    return (size_t)(p - o);
}


/* A peer's interest list arrived (from its discovery announce): refresh its bits
 * and rematch every channel. Idempotent; re-applying re-derives all matches. */
void dart_transport_apply_peer_interest(DartTransportState *st, uint32_t peer_id, DartBytes blob){
    const uint8_t *d=blob.data, *p, *end=d+blob.len;
    uint16_t n_pub, n_sub, c; int peer_slot=i_dart_peer_slot(st,peer_id);
    uint8_t *peer_pub_bitmap, *peer_sub_bitmap;
    uint32_t alias_need = 0;
    if (peer_slot<0 || blob.len<4) return;
    peer_pub_bitmap=&st->peer_pub_bitmap[(size_t)peer_slot*st->bitmap_len];
    peer_sub_bitmap=&st->peer_sub_bitmap[(size_t)peer_slot*st->bitmap_len];
    n_pub=i_dart_le_r16(d); n_sub=i_dart_le_r16(d+2);
    /* validate the whole variable-length list first (a truncated blob must not drop a
       match), and learn the highest advertised alias: the size a lazily-made alias map
       must be to cover the peer's whole list */
    { uint32_t k, tot=(uint32_t)n_pub+n_sub; p=d+4;
      for (k=0;k<tot;k++){
          if (p+4 > end) return;
          { uint32_t a = (uint32_t)i_dart_le_r16(p) + 1u; if (a > alias_need) alias_need = a; }
          p += 4u + (uint32_t)p[3];
          if (p > end) return;
      } }
    memset(peer_pub_bitmap,0,st->bitmap_len); memset(peer_sub_bitmap,0,st->bitmap_len);
    memset(&st->peer_sub_reliable[(size_t)peer_slot*st->bitmap_len],0,st->bitmap_len);
    if (st->peer_alias[peer_slot] && st->peer_alias_len[peer_slot])   /* re-apply: clear old mappings */
        memset(st->peer_alias[peer_slot],0xFF,(size_t)st->peer_alias_len[peer_slot]*sizeof(uint16_t));
    {   uint32_t overflow = 0;
        p = i_dart_meta_scan(st, peer_slot, d+4, n_pub, peer_pub_bitmap, 1, NULL, alias_need, &overflow);  /* pub list: offered QoS */
        p = i_dart_meta_scan(st, peer_slot, p,   n_sub, peer_sub_bitmap, 0,                    /* sub list: requested QoS */
                            &st->peer_sub_reliable[(size_t)peer_slot*st->bitmap_len], alias_need, NULL);
        if (overflow)   /* never silent: those topics look matched but will not deliver */
            i_dart_transport_fire_event(st, DART_TRANSPORT_INTEREST_OVERFLOW, 0, peer_id, 0, overflow);
    }
    for (c=0;c<st->cfg.n_channels;c++) i_dart_channel_rematch(st,c,(uint16_t)peer_slot);
}


/* diagnostic: how many channels we now publish to / receive from this peer (unicast
 * lanes). Surfaced on DART_PEER_INTEREST so a caller can see a match form (or not). */
void dart_transport_peer_match_counts(DartTransportState *st, uint32_t peer_id,
                            uint16_t *publish_to, uint16_t *receive_from){
    int s; uint16_t c, w=0, r=0;
    if (publish_to)   *publish_to   = 0;
    if (receive_from) *receive_from = 0;
    if (!st) return;
    s = i_dart_peer_slot(st, peer_id);
    if (s < 0) return;
    for (c=0;c<st->cfg.n_channels;c++){
        i_DartLane *l = i_dart_lane_at(st,c,(uint32_t)s);
        if (!l) continue;
        if (l->w.used) w++;
        if (l->r.used) r++;
    }
    if (publish_to)   *publish_to   = w;
    if (receive_from) *receive_from = r;
}


/* Discovery-announce meta blob codec (see dart_meta_* in core.h for the layout). The
   interest list is wrapped in a prefix carrying frag size, (odd ver) SHM info, and the
   node name. No back-compat: the version byte just tags the one current format, and a
   blob whose magic/version we don't expect is rejected, not reinterpreted. An interest
   entry is [u16 alias][u8 flags][u8 namelen][name]; flags bit 0 = offered reliability.
   Parsing is fully bounds-checked (see dart_transport_apply_peer_interest), so a malformed or
   foreign blob is dropped wholesale, never trusted. */
#define DART__META_BASE_NOSHM 5u    /* 'D','N',ver, frag_lo, frag_hi */
#define DART__META_BASE_SHM   22u   /* ... + shm(1) + host[16] */
#ifdef DART_SHM
#define DART__META_VER  9u                  /* what WE write */
#define DART__META_BASE DART__META_BASE_SHM
#else
#define DART__META_VER  8u
#define DART__META_BASE DART__META_BASE_NOSHM
#endif

static int i_dart_meta_ok(DartBytes meta){
    return meta.data && meta.len >= 5 && meta.data[0]=='D' && meta.data[1]=='N'
        && meta.data[2]>=8 && meta.data[2]<=9;
}
/* base prefix through host[16], by version (odd v9 carries shm+host, even v8 doesn't). */
static uint16_t i_dart_meta_base(const uint8_t *meta){
    return (meta[2] & 1u) ? DART__META_BASE_SHM : DART__META_BASE_NOSHM;
}
/* upper bound on the schema section: every channel mapped, every wire distinct + inlined */
static size_t i_dart_meta_schemas_max(uint16_t n_channels){
    return 4u + (size_t)n_channels * (2u + 8u)
              + (size_t)n_channels * (8u + 2u + DART_META_SCHEMA_INLINE_MAX);
}
uint16_t dart_meta_capacity(uint16_t n_channels){
    size_t cap = (size_t)DART__META_BASE + dart_interest_max(n_channels)   /* overlay: no name (it's discovery's) */
               + i_dart_meta_schemas_max(n_channels);
    if (cap > 65000u) cap = 65000u;
    return (uint16_t)cap;
}

/* the schema section: map every advertising alias (any non-INACTIVE role: publishers
 * offer their layout, subscribers their required subset) to its schema identity, then
 * each distinct wire once (interned by hash), inlined only when it fits
 * DART_META_SCHEMA_INLINE_MAX. Always present (two zero counts when there is nothing to
 * advertise). Returns bytes written, or 0 if cap is too small (the caller then ships
 * the overlay without the section). */
static int i_dart_meta_schema_advertised(DartTransportState *st, const DartMetaSchema *schemas,
                                         uint16_t c){
    return schemas && schemas[c].hash != 0 && st->channels[c].role != DART_INACTIVE;
}
static size_t i_dart_meta_schemas_build(DartTransportState *st, uint8_t *out, size_t cap,
                                        const DartMetaSchema *schemas){
    uint8_t *p = out + 2, *end = out + cap, *wires;
    uint16_t c, k, n_map = 0, n_wire = 0;
    if (cap < 4) return 0;
    for (c = 0; c < st->cfg.n_channels; c++){          /* alias -> hash map */
        if (!i_dart_meta_schema_advertised(st, schemas, c)) continue;
        if (p + 10 > end) return 0;
        i_dart_le_w16(p, c); i_dart_le_w64(p + 2, schemas[c].hash);
        p += 10; n_map++;
    }
    i_dart_le_w16(out, n_map);
    wires = p; p += 2;
    if (p > end) return 0;
    for (c = 0; c < st->cfg.n_channels; c++){          /* distinct wires, inlined when small */
        int seen = 0;
        if (!i_dart_meta_schema_advertised(st, schemas, c)) continue;
        if (!schemas[c].wire.data || schemas[c].wire.len == 0
            || schemas[c].wire.len > DART_META_SCHEMA_INLINE_MAX) continue;
        for (k = 0; k < c; k++)                        /* interned: emitted once per hash */
            if (i_dart_meta_schema_advertised(st, schemas, k)
                && schemas[k].hash == schemas[c].hash){ seen = 1; break; }
        if (seen) continue;
        if (p + 10 + schemas[c].wire.len > end) return 0;
        i_dart_le_w64(p, schemas[c].hash); i_dart_le_w16(p + 8, (uint16_t)schemas[c].wire.len);
        memcpy(p + 10, schemas[c].wire.data, schemas[c].wire.len);
        p += 10 + schemas[c].wire.len; n_wire++;
    }
    i_dart_le_w16(wires, n_wire);
    return (size_t)(p - out);
}

/* Exact overlay size the next dart_transport_meta_build will emit for the current channel +
 * schema state (the same walks, byte for byte), so a caller can size the buffer to the
 * actual content instead of dart_meta_capacity's every-channel-has-a-max-schema worst case. */
uint16_t dart_transport_meta_size(DartTransportState *st, const DartMetaSchema *schemas){
    size_t len = (size_t)DART__META_BASE + 4u;   /* base prefix + [npub][nsub] */
    uint16_t c, k;
    for (c=0;c<st->cfg.n_channels;c++){
        uint8_t d=st->channels[c].role;
        if (d==DART_PUBSUB || d==DART_PUB_ONLY) len += 4u + st->channels[c].name_len;
        if (d==DART_PUBSUB || d==DART_SUB_ONLY) len += 4u + st->channels[c].name_len;
    }
    len += 4u;                                   /* schema section: [n_map][n_wire] */
    for (c=0;c<st->cfg.n_channels;c++){
        int seen = 0;
        if (!i_dart_meta_schema_advertised(st, schemas, c)) continue;
        len += 10u;                              /* alias -> hash map entry */
        if (!schemas[c].wire.data || schemas[c].wire.len == 0
            || schemas[c].wire.len > DART_META_SCHEMA_INLINE_MAX) continue;
        for (k = 0; k < c; k++)                  /* interned: counted once per hash */
            if (i_dart_meta_schema_advertised(st, schemas, k)
                && schemas[k].hash == schemas[c].hash){ seen = 1; break; }
        if (!seen) len += 10u + schemas[c].wire.len;
    }
    if (len > 65000u) len = 65000u;              /* the dart_meta_capacity ceiling; past it the build truncates */
    return (uint16_t)len;
}

uint16_t dart_transport_meta_build(DartTransportState *st, uint8_t *out, uint16_t cap,
                         uint16_t frag_size, int shm_capable, const uint8_t host[16],
                         const DartMetaSchema *schemas){
    size_t interest_len, len; uint16_t off = DART__META_BASE;
    out[0]='D'; out[1]='N'; out[2]=DART__META_VER;
    out[3]=(uint8_t)(frag_size & 0xFF); out[4]=(uint8_t)(frag_size >> 8);
#ifdef DART_SHM
    out[5]=(uint8_t)(shm_capable?1:0);
    if (host) memcpy(out+6, host, 16); else memset(out+6, 0, 16);
#else
    (void)shm_capable; (void)host;
#endif
    interest_len = dart_transport_build_interest(st, out + off, cap - off);   /* no name here: that is discovery's */
    len = (size_t)off + interest_len;
    if (interest_len == 0){   /* did not fit (returns >= 4 even with zero channels): never silent */
        i_dart_transport_fire_event(st, DART_TRANSPORT_META_TRUNCATED_INTEREST, 0, 0, 0, 0);
    } else {   /* the schema section is located by walking the interest list, so it needs one */
        size_t s = i_dart_meta_schemas_build(st, out + len, cap - len, schemas);
        if (s){
            len += s;
        } else {   /* 0 = did not fit; report only if there was something to advertise */
            uint16_t c;
            for (c = 0; c < st->cfg.n_channels; c++)
                if (i_dart_meta_schema_advertised(st, schemas, c)){
                    i_dart_transport_fire_event(st, DART_TRANSPORT_META_TRUNCATED_SCHEMA, 0, 0, 0, 0);
                    break;
                }
        }
    }
    return (uint16_t)len;
}

uint16_t dart_meta_frag(DartBytes meta){
    if (!i_dart_meta_ok(meta)) return 0;
    return (uint16_t)(meta.data[3] | ((uint16_t)meta.data[4] << 8));
}

DartBytes dart_meta_interest(DartBytes meta){
    uint16_t off;
    if (!i_dart_meta_ok(meta)) return dart_bytes(NULL, 0);
    off = i_dart_meta_base(meta.data);     /* interest follows the base prefix (no name in the overlay) */
    if (meta.len < off) return dart_bytes(NULL, 0);
    return dart_bytes(meta.data + off, meta.len - off);
}

int dart_meta_interest_next(DartBytes meta, DartInterestIter *it, DartTopic *out){
    uint32_t off; uint8_t nlen;
    if (!it || !out) return 0;
    if (!it->started){                    /* first call: parse the [npub][nsub] header */
        DartBytes in = dart_meta_interest(meta);
        it->started = 1; it->pub_left = it->sub_left = 0; it->off = 0;
        if (!in.data || in.len < 4) return 0;      /* no/short interest list: nothing to yield */
        it->pub_left = (uint16_t)(in.data[0] | ((uint16_t)in.data[1] << 8));
        it->sub_left = (uint16_t)(in.data[2] | ((uint16_t)in.data[3] << 8));
        it->off = (uint32_t)(in.data - meta.data) + 4u;   /* first entry, past npub/nsub */
    }
    if (it->pub_left == 0 && it->sub_left == 0) return 0;
    off = it->off;
    if (off + 4u > meta.len){ it->pub_left = it->sub_left = 0; return 0; }   /* truncated: stop */
    nlen = meta.data[off + 3];
    if (off + 4u + nlen > meta.len){ it->pub_left = it->sub_left = 0; return 0; }
    out->alias    = (uint16_t)(meta.data[off] | ((uint16_t)meta.data[off + 1] << 8));
    out->reliable = (uint8_t)(meta.data[off + 2] & DART_META_F_RELIABLE);
    out->is_pub   = (uint8_t)(it->pub_left > 0);   /* pub list first, then sub */
    out->name     = dart_string((const char *)(meta.data + off + 4u), nlen);
    it->off = off + 4u + nlen;
    if (it->pub_left > 0) it->pub_left--; else it->sub_left--;
    return 1;
}

/* Offset of the schema section: the base prefix, then a bounds-checked walk over the
 * (count-delimited) interest list. 0 = malformed/absent. */
static uint32_t i_dart_meta_schemas_off(DartBytes meta){
    uint32_t off, k, tot; uint16_t n_pub, n_sub;
    if (!i_dart_meta_ok(meta)) return 0;
    off = i_dart_meta_base(meta.data);
    if ((size_t)off + 4u > meta.len) return 0;
    n_pub = i_dart_le_r16(meta.data + off); n_sub = i_dart_le_r16(meta.data + off + 2);
    off += 4u; tot = (uint32_t)n_pub + n_sub;
    for (k = 0; k < tot; k++){
        if ((size_t)off + 4u > meta.len) return 0;
        off += 4u + (uint32_t)meta.data[off + 3];
        if ((size_t)off > meta.len) return 0;
    }
    return off;
}

int dart_meta_schema(DartBytes meta, uint16_t alias, uint64_t *hash, DartBytes *wire){
    uint32_t off = i_dart_meta_schemas_off(meta), k;
    uint16_t n_map, n_wire; uint64_t h = 0; int found = 0;
    if (hash) *hash = 0;
    if (wire) *wire = dart_bytes(NULL, 0);
    if (off == 0 || (size_t)off + 4u > meta.len) return 0;
    n_map = i_dart_le_r16(meta.data + off); off += 2;
    for (k = 0; k < n_map; k++, off += 10){            /* alias -> hash */
        if ((size_t)off + 10u > meta.len) return 0;
        if (i_dart_le_r16(meta.data + off) == alias){ h = i_dart_le_r64(meta.data + off + 2); found = 1; }
    }
    if (!found || h == 0) return 0;
    if (hash) *hash = h;
    if ((size_t)off + 2u > meta.len) return 1;         /* hash-only blob: no wire table */
    n_wire = i_dart_le_r16(meta.data + off); off += 2;
    for (k = 0; k < n_wire; k++){                      /* hash -> inlined wire */
        uint16_t wlen;
        if ((size_t)off + 10u > meta.len) return 1;
        wlen = i_dart_le_r16(meta.data + off + 8);
        if ((size_t)off + 10u + wlen > meta.len) return 1;
        if (i_dart_le_r64(meta.data + off) == h){
            if (wire) *wire = dart_bytes(meta.data + off + 10, wlen);
            return 1;
        }
        off += 10u + wlen;
    }
    return 1;                                          /* advertised, but not inlined */
}

#ifdef DART_SHM
int dart_meta_shm(DartBytes meta, uint8_t host[16]){
    if (!i_dart_meta_ok(meta) || meta.data[2]!=9
        || meta.len < DART__META_BASE_SHM || !meta.data[5]) return 0;
    memcpy(host, meta.data+6, 16);
    return 1;
}
#endif


/* Pairwise detail exchange codec ('uDTL', see core.h for the layout and contract).
   Parsing is fully bounds-checked: a malformed request or response is dropped wholesale,
   never trusted. The responder side is a pure read of channel + schema state. */
#define DART__DETAIL_HDR 14u   /* magic(4) kind(1) ver(1) domain(2) meta_version(4) n(2) */
#define DART__DETAIL_VER 1u

static int i_dart_detail_hdr_ok(DartBytes d){
    return d.data && d.len >= DART__DETAIL_HDR
        && d.data[0]=='u' && d.data[1]=='D' && d.data[2]=='T' && d.data[3]=='L'
        && d.data[5]==DART__DETAIL_VER;
}

int dart_detail_kind(DartBytes dgram){
    if (!i_dart_detail_hdr_ok(dgram)) return 0;
    return (dgram.data[4]==DART_DETAIL_REQ || dgram.data[4]==DART_DETAIL_RESP)
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
        i_dart_le_w16(e, wants[k].alias);
        i_dart_le_w64(e+2, wants[k].schema_hash);
    }
    return need;
}

/* One walk serves size and build (out NULL = measure), so the two agree byte for byte.
   A truncated build stops at an entry boundary: the response stays parseable and the
   requester re-requests the aliases it still lacks (the paging seam). */
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
        uint16_t alias    = i_dart_le_r16(r);
        uint64_t req_hash = i_dart_le_r64(r+2);
        const i_DartChannel *ch;
        uint64_t hash; DartBytes wire; size_t need;
        if (alias >= st->cfg.n_channels) continue;             /* unknown: not advertised */
        ch = &st->channels[alias];
        if (ch->role == DART_INACTIVE || ch->name_len == 0) continue;
        hash = schemas ? schemas[alias].hash : 0;
        wire = dart_bytes(NULL, 0);
        if (hash && hash != req_hash && schemas[alias].wire.len <= 0xFFFFu)
            wire = schemas[alias].wire;    /* differs: inline for the subset check */
        need = 2u + 1u + ch->name_len + 8u + 2u + wire.len;
        if (out){
            uint8_t *e = out + len;
            if (len + need > cap) break;
            i_dart_le_w16(e, alias);
            e[2] = ch->name_len;
            memcpy(e+3, ch->name, ch->name_len);
            i_dart_le_w64(e+3+ch->name_len, hash);
            i_dart_le_w16(e+3+ch->name_len+8, (uint16_t)wire.len);
            if (wire.len) memcpy(e+3+ch->name_len+10, wire.data, wire.len);
        }
        len += need;
        n_out++;
    }
    if (out) i_dart_le_w16(out+12, n_out);
    return len;
}

size_t dart_transport_detail_resp_size(DartTransportState *st, const DartMetaSchema *schemas,
                                       DartBytes req){
    return i_dart_detail_answer(st, schemas, 0, req, NULL, 0);
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
    if ((size_t)off + 3u > resp.len){ it->left = 0; return 0; }        /* truncated: stop */
    nlen = resp.data[off+2];
    if ((size_t)off + 3u + nlen + 10u > resp.len){ it->left = 0; return 0; }
    wlen = i_dart_le_r16(resp.data + off + 3u + nlen + 8u);
    if ((size_t)off + 3u + nlen + 10u + wlen > resp.len){ it->left = 0; return 0; }
    out->alias       = i_dart_le_r16(resp.data + off);
    out->name        = dart_string((const char*)(resp.data + off + 3u), nlen);
    out->schema_hash = i_dart_le_r64(resp.data + off + 3u + nlen);
    out->schema_wire = wlen ? dart_bytes(resp.data + off + 3u + nlen + 10u, wlen)
                            : dart_bytes(NULL, 0);
    it->off = off + 3u + nlen + 10u + wlen;
    it->left--;
    return 1;
}


int dart_transport_set_role(DartTransportState *st, uint16_t channel, uint8_t role){
    int channel_idx; i_DartChannel *ch; uint16_t p;
    if (role > DART_INACTIVE) return -1;
    ch = i_dart_channel_at(st, channel, &channel_idx);
    if (!ch) return -1;
    if (ch->role == role) return 0;
    ch->role = role;
    for (p=0;p<st->cfg.max_peers;p++)
        if (st->peer_used[p]) i_dart_channel_rematch(st,(uint16_t)channel_idx,p);
    /* caller re-advertises interest (the node bumps its discovery announce) */
    return 0;
}


int dart_transport_channel_define(DartTransportState *st, uint16_t channel, const DartChannelDef *def){
    i_DartChannel *ch; DartQos q; uint16_t depth, p; size_t lane;
    if (!st || !def || !st->cfg.allocator) return -1;       /* dynamic (reserve) mode only */
    if (channel >= st->cfg.n_channels) return -1;            /* out of reserved range */
    if (!def->name || !def->name[0]) return -1;             /* name = identity, required */
    lane = i_dart_name_len(def->name);
    if (def->name[lane]) return -1;                          /* longer than DART_TOPIC_NAME_MAX */
    ch = &st->channels[channel];
    if (ch->identity != 0 || ch->history) return -1;        /* slot already defined */
    q = def->qos; i_dart_qos_defaults(&q, 1);                 /* dynamic: grow-to-fit buffers */
    depth = q.keep_last;
    ch->history = (i_DartWriterSample*)st->cfg.allocator(st->cfg.user, NULL,
                                                         (size_t)depth*sizeof(i_DartWriterSample));
    if (!ch->history) return -4;                            /* OOM */
    memset(ch->history, 0, (size_t)depth*sizeof(i_DartWriterSample));
    ch->history_owned = 1; ch->dynamic = 1;
    ch->qos = q; ch->max_fragments = i_dart_max_fragments(q.max_message_bytes);
    ch->role = def->role;
    ch->identity = dart_channel_identity(def);
    memcpy((char*)ch->name, def->name, lane); ((char*)ch->name)[lane] = '\0';
    ch->name_len = (uint8_t)lane;
    ch->history_head = 0; ch->next_seqno = 0; ch->have_first = 0;
    for (p=0;p<st->cfg.max_peers;p++)        /* match the newly active channel to known peers */
        if (st->peer_used[p]) i_dart_channel_rematch(st, channel, p);
    return 0;
}


DartString dart_transport_channel_name(DartTransportState *st, uint16_t channel){
    i_DartChannel *ch = i_dart_channel_at(st, channel, NULL);
    if (!ch || ch->name_len == 0) return dart_string(NULL, 0);   /* undefined / reserve slot */
    return dart_string(ch->name, ch->name_len);
}


const DartQos *dart_transport_channel_qos(DartTransportState *st, uint16_t channel){
    i_DartChannel *ch = i_dart_channel_at(st, channel, NULL);
    return ch ? &ch->qos : NULL;
}


void dart_transport_repair_stats(DartTransportState *st, uint16_t channel, DartRepairStats *out){
    i_DartChannel *ch = i_dart_channel_at(st, channel, NULL);
    if (!out) return;
    if (ch) *out = ch->repair_stats;
    else memset(out, 0, sizeof *out);
}


void dart_transport_on_datagram(DartTransportState *st, uint32_t from, DartBytes datagram, uint64_t now){
    const uint8_t *p=datagram.data; size_t rem=datagram.len;
    int peer_slot=i_dart_peer_slot(st,from);
    if (peer_slot<0) return;
    /* concatenated submessages; each length comes from its header, so no framing */
    while (rem>=3){
        uint8_t b0=p[0], type=(uint8_t)(b0 & DART_MSG_MASK); uint16_t alias; size_t sub; int channel_idx;
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
            default: return;             /* unknown type: cannot resync, drop rest */
        }
        if (sub>rem) return;             /* truncated/malformed */
        alias = i_dart_le_r16(p+DART_OFFSET_ALIAS);
        if ((uint32_t)alias < st->peer_alias_len[peer_slot]){
            uint16_t m=st->peer_alias[peer_slot][alias]; channel_idx=(m==0xFFFFu)?-1:(int)m;
        } else channel_idx=-1;
        if (channel_idx>=0){
            switch(type){
                case DART_DATA:
#ifdef DART_SHM
                                if (p[0] & DART_F_SHM){ i_dart_reader_shm(st,channel_idx,peer_slot,p,now); break; }
#endif
                                i_dart_reader_data(st,channel_idx,peer_slot,p,now); break;
                case DART_HB:   i_dart_reader_hb  (st,channel_idx,peer_slot,p,now); break;
                case DART_NACK: i_dart_writer_nack(st,channel_idx,peer_slot,p); break;
            }
        }
        p+=sub; rem-=sub;
    }
}
#pragma endregion

#ifndef DART_TRANSPORT_SANS_IO
#pragma region serialize/schema.c
#include <string.h>            /* memcpy (float bit reinterpret) */

/* The schema wire format (what the builder emits and dart_schema_parse reads):
 *   schema := [u8 version][u8 root_namelen][root_name...][type]   ; root type is a STRUCT
 *   type   := [u8 kind] payload
 *     scalar (U8..BOOL)      : (none; size implied by kind)
 *     ARR                    : [u8 elem][u16 count]               ; elem must be a scalar
 *     STRUCT                 : [u8 nfields] ( [u8 namelen][name][type] )*nfields
 * Every type is fixed: a field's byte offset is the sum of the preceding field sizes. */

/* The per-field record, one for EVERY field at every depth (flattened depth-first: a
 * struct's members directly follow it). Offsets are message-absolute. */
typedef struct {
    DartString name;      /* field's own name, a view into the wire bytes */
    uint32_t   offset;    /* absolute byte offset in a message */
    uint32_t   size;      /* byte size */
    uint32_t   type_off;  /* wire offset of the field's type encoding (subset compare) */
    uint32_t   type_len;  /* wire length of the type encoding */
    uint16_t   count;     /* ARR element count, else 0 */
    uint16_t   depth;     /* 0 = top level */
    uint16_t   parent;    /* flat index of the enclosing struct field; 0xFFFF = root */
    uint8_t    kind;
    uint8_t    elem;      /* ARR element kind, else 0 */
} i_Field;

struct DartSchema {
    DartBytes   wire;       /* the canonical bytes (a view into the caller's buffer) */
    uint64_t    hash;
    uint32_t    size;       /* exact message size in bytes */
    DartString  name;       /* root type name, a view into the wire bytes */
    uint16_t    nfields;
    i_Field     fields[1];   /* nfields entries, laid out in the caller's buffer */
};

uint32_t dart_schema_scalar_size(DartSchemaTypeKind kind){
    switch (kind){
        case DART_U8: case DART_I8: case DART_BOOL: return 1;
        case DART_U16: case DART_I16:               return 2;
        case DART_U32: case DART_I32: case DART_F32: return 4;
        case DART_U64: case DART_I64: case DART_F64: return 8;
        default: return 0;
    }
}

/* ---- bounds-checked reader over (possibly hostile) wire bytes ---------------------- */
typedef struct { const uint8_t *w; size_t n, pos; int fail; } i_Rd;
static uint8_t  i_dart_rd_u8 (i_Rd *r){ if (r->pos + 1 > r->n){ r->fail = 1; return 0; } return r->w[r->pos++]; }
static uint16_t i_dart_rd_u16(i_Rd *r){ uint16_t v; if (r->pos + 2 > r->n){ r->fail = 1; return 0; } v = i_dart_le_r16(r->w + r->pos); r->pos += 2; return v; }
static void     i_dart_rd_skip(i_Rd *r, size_t k){ if (r->pos + k > r->n){ r->fail = 1; r->pos = r->n; return; } r->pos += k; }

/* Byte size of the type at r->pos; advances r past it. Counts every field it walks
 * (all depths) into *fields when given. Fails on an unknown/variable kind or nesting
 * past DART_SCHEMA_MAX_DEPTH (the read-side twin of the builder's cap: hostile wire
 * must not recurse unboundedly). */
static uint32_t i_dart_rd_type_size(i_Rd *r, uint32_t *fields, uint16_t depth){
    uint8_t k = i_dart_rd_u8(r);
    if (r->fail) return 0;
    switch (k){
        case DART_U8: case DART_I8: case DART_BOOL: return 1;
        case DART_U16: case DART_I16:               return 2;
        case DART_U32: case DART_I32: case DART_F32: return 4;
        case DART_U64: case DART_I64: case DART_F64: return 8;
        case DART_ARR: {
            uint32_t es = dart_schema_scalar_size((DartSchemaTypeKind)i_dart_rd_u8(r));
            uint16_t count = i_dart_rd_u16(r);
            if (r->fail || es == 0){ r->fail = 1; return 0; }    /* elem must be a scalar */
            return (uint32_t)count * es;
        }
        case DART_STRUCT: {
            uint8_t nf; uint32_t sum = 0; uint16_t i;
            if (depth >= DART_SCHEMA_MAX_DEPTH){ r->fail = 1; return 0; }
            nf = i_dart_rd_u8(r);
            for (i = 0; i < nf && !r->fail; i++){
                uint8_t fl = i_dart_rd_u8(r);
                i_dart_rd_skip(r, fl);
                if (fields) (*fields)++;
                sum += i_dart_rd_type_size(r, fields, (uint16_t)(depth + 1));
            }
            return sum;
        }
        default: r->fail = 1; return 0;                          /* unknown/variable kind: reject */
    }
}

/* Offset of the compiled handle in buf: past the wire bytes, 8-aligned for the handle. */
static size_t i_dart_schema_handle_off(const uint8_t *buf, size_t wire_len){
    uintptr_t addr = (uintptr_t)buf + wire_len;
    size_t pad = (size_t)((8u - (addr & 7u)) & 7u);
    return wire_len + pad;
}

/* Count every field (all depths) of the schema wire's root struct into *n; 1, or 0 on
 * malformed wire (an empty-but-valid schema is 1 with *n == 0). */
static int i_dart_schema_wire_fields(const void *wire, size_t wire_len, uint32_t *n){
    i_Rd r; uint8_t ver, rl;
    *n = 0;
    r.w = (const uint8_t *)wire; r.n = wire_len; r.pos = 0; r.fail = 0;
    ver = i_dart_rd_u8(&r); if (r.fail || ver != DART_SCHEMA_WIRE_VERSION) return 0;
    rl = i_dart_rd_u8(&r); i_dart_rd_skip(&r, rl);
    if (r.fail || (size_t)r.pos >= r.n || r.w[r.pos] != DART_STRUCT) return 0;   /* root: a struct */
    i_dart_rd_type_size(&r, n, 0);
    return r.fail ? 0 : 1;
}

/* Emit the fields of the struct body at r->pos (its nfields byte) into the flat table,
 * depth-first. Returns the struct's byte size; sets r->fail on malformed wire. */
static uint32_t i_dart_schema_emit(uint8_t *buf, size_t wire_len, i_Rd *r, DartSchema *s,
                                   uint16_t *emitted, uint32_t total,
                                   uint16_t depth, uint16_t parent, uint32_t base){
    uint8_t nf = i_dart_rd_u8(r); uint16_t i;
    uint32_t running = base;
    if (depth >= DART_SCHEMA_MAX_DEPTH){ r->fail = 1; return 0; }
    for (i = 0; i < nf && !r->fail; i++){
        i_Field *f;
        uint8_t fl = i_dart_rd_u8(r); const char *fn = (const char *)(buf + r->pos);
        size_t kpos; uint16_t idx; uint32_t sz;
        i_dart_rd_skip(r, fl);
        kpos = r->pos;
        if (*emitted >= total){ r->fail = 1; return 0; }
        idx = (*emitted)++;
        f = &s->fields[idx];
        f->name = dart_string(fn, fl);
        f->kind = (kpos < r->n) ? buf[kpos] : 0;
        f->count = 0; f->elem = 0;
        f->depth = depth; f->parent = parent;
        f->offset = running;
        if (f->kind == DART_STRUCT){
            i_dart_rd_u8(r);                                   /* consume the kind byte */
            sz = i_dart_schema_emit(buf, wire_len, r, s, emitted, total,
                                    (uint16_t)(depth + 1), idx, running);
        } else {
            sz = i_dart_rd_type_size(r, NULL, depth);
            if (f->kind == DART_ARR){                          /* capture elem/count for readers */
                i_Rd q; q.w = buf; q.n = wire_len; q.pos = kpos + 1; q.fail = 0;
                f->elem = i_dart_rd_u8(&q);
                f->count = i_dart_rd_u16(&q);
            }
        }
        if (r->fail) return 0;
        f->type_off = (uint32_t)kpos;                          /* type extents: subset compare */
        f->type_len = (uint32_t)(r->pos - kpos);
        f->size = sz;
        running += sz;
    }
    return running - base;
}

/* Compile wire bytes already sitting at buf[0..wire_len] into a DartSchema placed after
 * them in buf. Returns NULL on a malformed blob or if buf[cap] is too small. */
static DartSchema *i_dart_schema_compile(uint8_t *buf, size_t wire_len, size_t cap){
    i_Rd r; uint8_t ver, root_kind, root_namelen;
    const char *root_name; size_t hoff, need; DartSchema *s;
    uint32_t total; uint16_t emitted = 0;

    if (!i_dart_schema_wire_fields(buf, wire_len, &total) || total > 0xFFFFu) return NULL;

    r.w = buf; r.n = wire_len; r.pos = 0; r.fail = 0;
    ver = i_dart_rd_u8(&r);
    if (r.fail || ver != DART_SCHEMA_WIRE_VERSION) return NULL;
    root_namelen = i_dart_rd_u8(&r);
    root_name = (const char *)(buf + r.pos);
    i_dart_rd_skip(&r, root_namelen);
    root_kind = i_dart_rd_u8(&r);
    if (r.fail || root_kind != DART_STRUCT) return NULL;       /* the root must be a struct */

    hoff = i_dart_schema_handle_off(buf, wire_len);
    need = hoff + sizeof(DartSchema) + (size_t)(total ? total - 1u : 0u) * sizeof(i_Field);
    if (need > cap) return NULL;

    s = (DartSchema *)(buf + hoff);
    s->wire = dart_bytes(buf, wire_len);
    s->name = dart_string(root_name, root_namelen);
    s->nfields = (uint16_t)total;
    s->hash = i_dart_fnv1a64(buf, wire_len);
    s->size = i_dart_schema_emit(buf, wire_len, &r, s, &emitted, (uint32_t)total, 0, 0xFFFFu, 0);
    if (r.fail || emitted != (uint16_t)total) return NULL;
    return s;
}

/* ---- builder ----------------------------------------------------------------------- */
/* ensure room for `extra` more bytes, growing the wire buffer through the hook */
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
    if (!nb){ b->err = -1; return 0; }              /* realloc failure leaves b->buf intact */
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
static void i_dart_schema_builder_put_name(DartSchemaBuilder *b, const char *name){
    size_t n = 0, i; if (name) while (name[n]) n++;
    if (n > 255){ b->err = -4; return; }
    if (!i_dart_schema_builder_reserve(b, 1 + n)) return;
    b->buf[b->len++] = (uint8_t)n;
    for (i = 0; i < n; i++) b->buf[b->len++] = (uint8_t)name[i];
}
/* count a field on the current innermost open struct */
static void i_dart_schema_builder_count(DartSchemaBuilder *b){
    uint16_t *c;
    if (b->err) return;
    if (b->depth == 0){ b->err = -3; return; }
    c = &b->field_count[b->depth - 1];
    if (*c >= 255){ b->err = -5; return; }   /* nfields is a u8 */
    (*c)++;
}
/* open a struct type: [u8 STRUCT][u8 nfields placeholder], push a nesting level */
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
    b.alloc = alloc; b.user = user;
    if (!alloc){ b.err = -1; return b; }
    b.cap = 64u;
    b.buf = (uint8_t *)alloc(user, NULL, b.cap);
    if (!b.buf){ b.err = -1; b.cap = 0; return b; }
    i_dart_schema_builder_put(&b, (uint8_t)DART_SCHEMA_WIRE_VERSION);
    i_dart_schema_builder_put_name(&b, root_name);
    i_dart_schema_builder_open_struct(&b);              /* the root is a struct; depth -> 1 */
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
    if (dart_schema_scalar_size(elem_scalar) == 0){ b->err = -6; return; }  /* scalar elements only */
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name);
    i_dart_schema_builder_put(b, (uint8_t)DART_ARR); i_dart_schema_builder_put(b, (uint8_t)elem_scalar); i_dart_schema_builder_put_u16(b, count);
}

void dart_schema_begin_struct(DartSchemaBuilder *b, const char *name){
    if (!b || b->err) return;
    i_dart_schema_builder_count(b);                 /* a field of the parent */
    i_dart_schema_builder_put_name(b, name);        /* the field's name */
    i_dart_schema_builder_open_struct(b);           /* the field's type: a struct */
}

void dart_schema_end_struct(DartSchemaBuilder *b){
    if (!b || b->err) return;
    if (b->depth <= 1){ b->err = -3; return; }   /* the root closes in finish */
    b->depth--;
    b->buf[b->count_pos[b->depth]] = (uint8_t)b->field_count[b->depth];
}

DartSchema *dart_schema_finish(DartSchemaBuilder *b){
    DartSchema *s = NULL;
    if (b && !b->err && b->depth == 1){                       /* depth != 1 = unbalanced begin/end */
        size_t need; uint8_t *nb; uint32_t total = 0;
        b->buf[b->count_pos[0]] = (uint8_t)b->field_count[0]; /* backpatch the root field count */
        i_dart_schema_wire_fields(b->buf, b->len, &total);    /* every field, all depths */
        need = b->len + 7u + sizeof(DartSchema)
             + (size_t)(total ? total - 1u : 0u) * sizeof(i_Field);
        nb = (uint8_t *)b->alloc(b->user, b->buf, need);      /* resize the block to hold the handle */
        if (nb){ b->buf = nb; b->cap = need; s = i_dart_schema_compile(b->buf, b->len, b->cap); }
    }
    if (!s && b && b->buf) b->alloc(b->user, b->buf, 0);      /* free on any failure */
    if (b) b->buf = NULL;                                     /* owned by s now, or freed */
    return s;
}

/* ---- parse a received schema ------------------------------------------------------- */
/* bytes a compiled schema needs for `wire`: copy + alignment pad + handle + the full
   flat field table. 0 if the wire is malformed. */
static size_t i_dart_schema_compiled_size(const void *wire, size_t wire_len){
    uint32_t total;
    if (!i_dart_schema_wire_fields(wire, wire_len, &total) || total > 0xFFFFu) return 0;
    return wire_len + 7u + sizeof(DartSchema) + (size_t)(total ? total - 1u : 0u) * sizeof(i_Field);
}

DartSchema *dart_schema_parse(const void *wire, size_t wire_len, DartAllocFn alloc, void *user){
    size_t need, i; uint8_t *buf; DartSchema *s;
    if (!wire || !alloc || wire_len == 0) return NULL;
    need = i_dart_schema_compiled_size(wire, wire_len);
    if (need == 0) return NULL;                              /* malformed header */
    buf = (uint8_t *)alloc(user, NULL, need);
    if (!buf) return NULL;
    for (i = 0; i < wire_len; i++) buf[i] = ((const uint8_t *)wire)[i];   /* persist the bytes */
    s = i_dart_schema_compile(buf, wire_len, need);
    if (!s) alloc(user, buf, 0);                             /* malformed body: free, no leak */
    return s;
}

void dart_schema_free(DartSchema *s, DartAllocFn alloc, void *user){
    if (s && alloc) alloc(user, (void *)s->wire.data, 0);    /* wire.data is the block base */
}

/* ---- queries ----------------------------------------------------------------------- */
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
        out->kind = f->kind; out->elem = f->elem;
        out->count = f->count; out->depth = f->depth;
        out->offset = f->offset; out->size = f->size;
    }
    return 1;
}

/* Match a dotted path against a field: the last segment is its own name, the ones
 * before it its ancestors, and the first segment must sit at the root. */
static int i_dart_schema_path_match(const DartSchema *s, const i_Field *f, const char *path,
                                    size_t path_len){
    const char *end = path + path_len;
    for (;;){
        const char *seg = end;
        while (seg > path && seg[-1] != '.') seg--;
        if (f->name.len != (size_t)(end - seg) ||
            (f->name.len && memcmp(f->name.data, seg, f->name.len) != 0)) return 0;
        if (f->parent == 0xFFFFu) return seg == path;          /* root: all segments consumed */
        if (seg == path) return 0;                             /* segments ran out early */
        end = seg - 1;                                         /* past the '.' */
        f = &s->fields[f->parent];
    }
}

static const i_Field *i_dart_schema_field_by_path(const DartSchema *s, const char *path){
    uint16_t i; size_t n;
    if (!s || !path) return NULL;
    n = strlen(path);
    for (i = 0; i < s->nfields; i++)
        if (i_dart_schema_path_match(s, &s->fields[i], path, n)) return &s->fields[i];
    return NULL;
}

int dart_schema_field_index(const DartSchema *s, const char *path){
    const i_Field *f = i_dart_schema_field_by_path(s, path);
    return f ? (int)(f - s->fields) : -1;
}

/* ---- reader/writer compatibility ----------------------------------------------------- */
/* find a TOP-LEVEL field by name (the match predicate works on top-level fields; a
 * nested struct is compared as one exact-encoded unit) */
static const i_Field *i_dart_schema_find(const DartSchema *s, DartString name){
    uint16_t i;
    for (i = 0; i < s->nfields; i++){
        const i_Field *f = &s->fields[i];
        if (f->depth == 0 && f->name.len == name.len &&
            (name.len == 0 || memcmp(f->name.data, name.data, name.len) == 0)) return f;
    }
    return NULL;
}

int dart_schema_subset(const DartSchema *sub, const DartSchema *pub){
    uint16_t i;
    if (!sub || !pub) return 0;
    if (sub->name.len != pub->name.len ||
        (sub->name.len && memcmp(sub->name.data, pub->name.data, sub->name.len) != 0)) return 0;
    for (i = 0; i < sub->nfields; i++){
        const i_Field *a = &sub->fields[i], *b;
        if (a->depth != 0) continue;                          /* members ride their struct */
        b = i_dart_schema_find(pub, a->name);
        if (!b || a->kind != b->kind) return 0;
        if (a->kind == DART_ARR && (a->elem != b->elem || a->count != b->count)) return 0;
        if (a->kind == DART_STRUCT &&                         /* nested: exact type encoding */
            (a->type_len != b->type_len ||
             memcmp(sub->wire.data + a->type_off, pub->wire.data + b->type_off, a->type_len) != 0))
            return 0;
    }
    return 1;
}

DartSchema *dart_schema_rebase(const DartSchema *sub, const DartSchema *pub,
                               DartAllocFn alloc, void *user){
    DartSchema *r; uint16_t i; uint32_t delta = 0;
    if (!alloc || !dart_schema_subset(sub, pub)) return NULL;
    r = dart_schema_parse(sub->wire.data, sub->wire.len, alloc, user);
    if (!r) return NULL;
    for (i = 0; i < r->nfields; i++){                         /* the writer's layout... */
        i_Field *f = &r->fields[i];
        if (f->depth == 0)                                    /* members shift with their struct */
            delta = i_dart_schema_find(pub, f->name)->offset - f->offset;
        f->offset += delta;
    }
    r->size = pub->size;                                      /* ...and the writer's message size */
    return r;
}

/* ---- read a message ---------------------------------------------------------------- */
int dart_schema_validate(const DartSchema *s, DartBytes msg){
    return s ? (msg.len == s->size) : 0;
}

/* Resolve a field by dotted path and bounds-check it against the message; NULL if
 * unknown or the message is too short. */
static const i_Field *i_dart_schema_read_lookup(const DartSchema *s, DartBytes msg, const char *field){
    const i_Field *f = i_dart_schema_field_by_path(s, field);
    if (!f || (size_t)f->offset + f->size > msg.len) return NULL;
    return f;
}

static uint64_t i_dart_schema_read_uint(const i_Field *f, const uint8_t *p){
    switch (f->kind){
        case DART_U8: case DART_BOOL: return p[0];
        case DART_U16: return i_dart_le_r16(p);
        case DART_U32: return i_dart_le_r32(p);
        case DART_U64: return i_dart_le_r64(p);
        default: return 0;
    }
}
static int64_t i_dart_schema_read_int(const i_Field *f, const uint8_t *p){
    switch (f->kind){
        case DART_I8:  return (int8_t)p[0];
        case DART_I16: return (int16_t)i_dart_le_r16(p);
        case DART_I32: return (int32_t)i_dart_le_r32(p);
        case DART_I64: return (int64_t)i_dart_le_r64(p);
        default: return 0;
    }
}
static double i_dart_schema_read_f64(const i_Field *f, const uint8_t *p){
    if (f->kind == DART_F64){ uint64_t b = i_dart_le_r64(p); double d; memcpy(&d, &b, 8); return d; }
    if (f->kind == DART_F32){ uint32_t b = i_dart_le_r32(p); float  x; memcpy(&x, &b, 4); return (double)x; }
    return 0.0;
}

uint64_t dart_get_uint(DartBytes msg, const DartSchema *s, const char *field){
    const i_Field *f = i_dart_schema_read_lookup(s, msg, field);
    return f ? i_dart_schema_read_uint(f, msg.data + f->offset) : 0;
}

int64_t dart_get_int(DartBytes msg, const DartSchema *s, const char *field){
    const i_Field *f = i_dart_schema_read_lookup(s, msg, field);
    return f ? i_dart_schema_read_int(f, msg.data + f->offset) : 0;
}

double dart_get_f64(DartBytes msg, const DartSchema *s, const char *field){
    const i_Field *f = i_dart_schema_read_lookup(s, msg, field);
    return f ? i_dart_schema_read_f64(f, msg.data + f->offset) : 0.0;
}

float dart_get_f32(DartBytes msg, const DartSchema *s, const char *field){
    const i_Field *f = i_dart_schema_read_lookup(s, msg, field);
    if (!f || f->kind != DART_F32) return 0.0f;
    { uint32_t b = i_dart_le_r32(msg.data + f->offset); float x; memcpy(&x, &b, 4); return x; }
}

DartBytes dart_get_array(DartBytes msg, const DartSchema *s, const char *field){
    DartBytes out; const i_Field *f = i_dart_schema_read_lookup(s, msg, field);
    out.data = NULL; out.len = 0;
    if (!f || f->kind != DART_ARR) return out;
    out.data = msg.data + f->offset; out.len = f->size;
    return out;
}

int dart_get_value(DartBytes msg, const DartSchema *s, uint16_t field, DartValue *out){
    const i_Field *f; const uint8_t *p;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!s || field >= s->nfields) return 0;
    f = &s->fields[field];
    if ((size_t)f->offset + f->size > msg.len) return 0;
    p = msg.data + f->offset;
    out->kind = f->kind; out->elem = f->elem; out->count = f->count;
    switch (f->kind){
        case DART_U8: case DART_U16: case DART_U32: case DART_U64: case DART_BOOL:
            out->v.u = i_dart_schema_read_uint(f, p); break;
        case DART_I8: case DART_I16: case DART_I32: case DART_I64:
            out->v.i = i_dart_schema_read_int(f, p); break;
        case DART_F32: case DART_F64:
            out->v.f = i_dart_schema_read_f64(f, p); break;
        case DART_ARR: case DART_STRUCT:
            out->bytes = dart_bytes(p, f->size); break;
        default: return 0;
    }
    return 1;
}

/* ---- write a message ----------------------------------------------------------------- */
int dart_schema_message_default(const DartSchema *s, void *buf, size_t cap){
    if (!s || !buf || cap < s->size) return 0;
    memset(buf, 0, s->size);
    return 1;
}

static const i_Field *i_dart_schema_set_lookup(const DartSchema *s, const void *buf, size_t cap,
                                               const char *field){
    const i_Field *f;
    if (!buf) return NULL;
    f = i_dart_schema_field_by_path(s, field);
    if (!f || (size_t)f->offset + f->size > cap) return NULL;
    return f;
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
/* copy elems over the front of the array, zero the rest; refuses misfits */
static int i_dart_schema_write_array(const i_Field *f, uint8_t *p, DartBytes elems){
    uint32_t esz = dart_schema_scalar_size((DartSchemaTypeKind)f->elem);
    if (f->kind != DART_ARR) return 0;
    if (elems.len > f->size || (esz && elems.len % esz != 0)) return 0;  /* never silently truncate */
    if (elems.len && !elems.data) return 0;
    if (elems.len) memcpy(p, elems.data, elems.len);
    memset(p + elems.len, 0, f->size - elems.len);
    return 1;
}

int dart_set_uint(void *buf, size_t cap, const DartSchema *s, const char *field, uint64_t v){
    const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field);
    return f ? i_dart_schema_write_uint(f, (uint8_t *)buf + f->offset, v) : 0;
}

int dart_set_int(void *buf, size_t cap, const DartSchema *s, const char *field, int64_t v){
    const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field);
    return f ? i_dart_schema_write_int(f, (uint8_t *)buf + f->offset, v) : 0;
}

int dart_set_f64(void *buf, size_t cap, const DartSchema *s, const char *field, double v){
    const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field);
    return f ? i_dart_schema_write_f64(f, (uint8_t *)buf + f->offset, v) : 0;
}

int dart_set_f32(void *buf, size_t cap, const DartSchema *s, const char *field, float v){
    const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field); uint32_t b;
    if (!f || f->kind != DART_F32 || (size_t)f->offset + 4 > cap) return 0;
    memcpy(&b, &v, 4); i_dart_le_w32((uint8_t *)buf + f->offset, b);
    return 1;
}

int dart_set_array(void *buf, size_t cap, const DartSchema *s, const char *field, DartBytes elems){
    const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field);
    return f ? i_dart_schema_write_array(f, (uint8_t *)buf + f->offset, elems) : 0;
}

int dart_set_value(void *buf, size_t cap, const DartSchema *s, uint16_t field, const DartValue *val){
    const i_Field *f; uint8_t *p;
    if (!buf || !val || !s || field >= s->nfields) return 0;
    f = &s->fields[field];
    if ((size_t)f->offset + f->size > cap) return 0;
    p = (uint8_t *)buf + f->offset;
    switch (f->kind){
        case DART_U8: case DART_U16: case DART_U32: case DART_U64: case DART_BOOL:
            return i_dart_schema_write_uint(f, p, val->v.u);
        case DART_I8: case DART_I16: case DART_I32: case DART_I64:
            return i_dart_schema_write_int(f, p, val->v.i);
        case DART_F32: case DART_F64:
            return i_dart_schema_write_f64(f, p, val->v.f);
        case DART_ARR:
            return i_dart_schema_write_array(f, p, val->bytes);
        case DART_STRUCT:                                  /* raw bytes, short = zero-filled tail */
            if (val->bytes.len > f->size || (val->bytes.len && !val->bytes.data)) return 0;
            if (val->bytes.len) memcpy(p, val->bytes.data, val->bytes.len);
            memset(p + val->bytes.len, 0, f->size - val->bytes.len);
            return 1;
        default: return 0;
    }
}

/* ---- the schema DSL ------------------------------------------------------------------ */
/* dart_schema_compile input (see schema.h for the full doc):
 *   schema := name '{' fields '}'
 *   field  := name ':' type  (',')?          fields self-delimit; commas optional
 *   type   := scalar | scalar '[' count ']' | '{' fields '}'
 * The parser is a thin front end over the builder, so all structural limits (name
 * lengths, field counts, nesting depth) are the builder's. */
typedef struct { const char *p; const char *err; } i_DartDsl;

static void i_dart_dsl_ws(i_DartDsl *d){
    for (;;){
        while (*d->p==' ' || *d->p=='\t' || *d->p=='\r' || *d->p=='\n') d->p++;
        if (d->p[0]=='-' && d->p[1]=='-'){ while (*d->p && *d->p!='\n') d->p++; continue; }
        return;
    }
}
static void i_dart_dsl_fail(i_DartDsl *d, const char *at){ if (!d->err) d->err = at; }
/* identifier into out[256] (NUL-terminated); 0 + err on missing/overlong */
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
/* decimal array count, 1..65535 */
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

/* fields of one struct body, up to (not consuming) the closing '}' */
static void i_dart_dsl_fields(i_DartDsl *d, DartSchemaBuilder *b){
    char name[256], tname[256];
    for (;;){
        i_dart_dsl_ws(d);
        if (*d->p == '}' || *d->p == '\0' || d->err || b->err) return;
        if (!i_dart_dsl_ident(d, name)) return;
        i_dart_dsl_ws(d);
        if (!i_dart_dsl_expect(d, ':')) return;
        i_dart_dsl_ws(d);
        if (*d->p == '{'){                                   /* nested struct */
            d->p++;
            dart_schema_begin_struct(b, name);
            i_dart_dsl_fields(d, b);
            if (!i_dart_dsl_expect(d, '}')) return;
            dart_schema_end_struct(b);
        } else {
            const char *at = d->p; DartSchemaTypeKind k;
            if (!i_dart_dsl_ident(d, tname)) return;
            if (!i_dart_dsl_kind(tname, &k)){ i_dart_dsl_fail(d, at); return; }   /* unknown type */
            i_dart_dsl_ws(d);
            if (*d->p == '['){
                uint16_t count;
                d->p++;
                i_dart_dsl_ws(d);
                if (!i_dart_dsl_count(d, &count)) return;
                i_dart_dsl_ws(d);
                if (!i_dart_dsl_expect(d, ']')) return;
                dart_schema_field_array(b, name, k, count);
            } else {
                dart_schema_field(b, name, k);
            }
        }
        i_dart_dsl_ws(d);
        if (*d->p == ',') d->p++;                            /* optional separator */
    }
}

DartSchema *dart_schema_compile(DartAllocFn alloc, void *user, const char *text, const char **err){
    i_DartDsl d; char root[256]; DartSchemaBuilder b; DartSchema *s;
    if (err) *err = NULL;
    if (!alloc || !text){ return NULL; }
    d.p = text; d.err = NULL;
    i_dart_dsl_ws(&d);
    if (!i_dart_dsl_ident(&d, root)){ if (err) *err = d.err; return NULL; }
    i_dart_dsl_ws(&d);
    b = dart_schema_begin(alloc, user, root);
    if (i_dart_dsl_expect(&d, '{')){
        i_dart_dsl_fields(&d, &b);
        if (i_dart_dsl_expect(&d, '}')){
            i_dart_dsl_ws(&d);
            if (*d.p) i_dart_dsl_fail(&d, d.p);              /* trailing garbage */
        }
    }
    if (d.err && !b.err) b.err = -7;                         /* parse error: make finish fail */
    s = dart_schema_finish(&b);                              /* frees everything on any error */
    if (!s && err) *err = d.err ? d.err : d.p;
    return s;
}
#pragma endregion
#pragma region shm/core.c
/* dart_shm: the portable segment-mapping + chunk module behind dart_shm.h. Pure
 * over dart_plat (shm mapping, host uuid, the generation atomic); no transport or
 * node knowledge. Compiles to nothing without DART_SHM. See dart_shm.h. */


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

/* OS object name "/dart.shm.<16 hex>" -- valid on POSIX (leading /) and Windows. */
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
    return DART_SHM_N_CLASSES;   /* bigger than the top class -> caller sends inline */
}

struct i_DartShmPool {
    void    *base;          /* mapping base */
    void    *handle;        /* OS handle for detach */
    size_t   map_bytes;     /* total mapped size */
    i_DartShmSegHdr *hdr;
    uint8_t *chunks;        /* base of the chunk region */
    uint32_t chunk_bytes;
    uint32_t n_chunks;
    uint32_t stride;        /* per-chunk bytes incl. header */
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
    /* the segment starts zero-filled; stamp the header and clear generations */
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
    /* map the whole OS object; its geometry (chunk_bytes/n_chunks) comes from the
       header the writer stamped, so the reader needs to know nothing up front --
       cfg's chunk_bytes/n_chunks are create-only. */
    base = i_dart_plat_shm_attach(cfg->name, &map_bytes, &handle);
    if (!base) return NULL;
    memset(p, 0, sizeof *p);
    p->base = base; p->handle = handle; p->map_bytes = map_bytes;
    p->hdr = (i_DartShmSegHdr*)base;
    i_dart_plat_host_uuid(ours);
    chunk_bytes = p->hdr->chunk_bytes; n_chunks = p->hdr->n_chunks;
    i_dart_shm_geom(chunk_bytes, n_chunks, &stride, &expect);
    /* reject a stale/foreign/mismatched/truncated segment -> caller falls back to UDP */
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
    i_dart_plat_atomic_store64(&c->generation, generation);  /* release: publishes the payload writes */
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
/* sans-IO NODE core: peer table + the discovery->transport lifecycle. No platform
 * access; a runtime drives it (see node/runtime.c) and does the IO. See node/core.h. */

#include <string.h>

/* dart_event_str + bounded appenders: format the node's app DartEvent as one line.
   No stdio, so it stays in the sans-IO core. (It covers the union event, including the
   peer/interest/mcast kinds the node adds; the transport keeps no formatter of its own.) */
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
static char *i_dart_event_append_addr(char *p, char *end, const DartEvent *ev){   /* dotted quad + :port (IPv4 only) */
    int i;
    for (i = 0; i < 4; i++){ if (i) p = i_dart_event_append_str(p,end,"."); p = i_dart_event_append_u64(p,end,ev->ip[i]); }
    p = i_dart_event_append_str(p,end,":"); return i_dart_event_append_u64(p,end,ev->port);
}
static char *i_dart_event_append_ch(char *p, char *end, const DartEvent *ev){   /* ch=<name> or ch=<index> */
    p = i_dart_event_append_str(p,end,"ch=");
    if (ev->channel_name) return i_dart_event_append_str(p,end,ev->channel_name);
    return i_dart_event_append_u64(p,end,ev->channel);
}
#ifndef DART_NO_DIAG   /* only the verbose error body below uses these two */
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

/* the DART_ERROR body, split out so the descriptive text can compile away under
   DART_NO_DIAG (leaving only the numeric error code) with no other change. */
static char *i_dart_event_error_str(char *p, char *end, const DartEvent *ev){
#ifdef DART_NO_DIAG
    p = i_dart_event_append_str(p,end,"error "); return i_dart_event_append_u64(p,end,(uint64_t)ev->error);
#else
    switch (ev->error){
    case DART_E_NAME_COLLISION:
        p=i_dart_event_append_str(p,end,"name-collision "); p=i_dart_event_append_ch(p,end,ev);
        p=i_dart_event_append_str(p,end," id=0x"); p=i_dart_event_append_hex(p,end,ev->identity);
        p=i_dart_event_append_str(p,end,": match refused"); break;
    case DART_E_QOS_INCOMPATIBLE:
        p=i_dart_event_append_str(p,end,"qos-incompatible "); p=i_dart_event_append_ch(p,end,ev);
        p=i_dart_event_append_str(p,end," from id="); p=i_dart_event_append_u64(p,end,ev->peer);
        p=i_dart_event_append_str(p,end,": reliable subscriber refused best-effort publisher"); break;
    case DART_E_SCHEMA_MISMATCH:
        p=i_dart_event_append_str(p,end,"schema-mismatch "); p=i_dart_event_append_ch(p,end,ev);
        p=i_dart_event_append_str(p,end," peer id="); p=i_dart_event_append_u64(p,end,ev->peer);
        p=i_dart_event_append_str(p,end,": incompatible schemas, refused"); break;
    case DART_E_INTEREST_OVERFLOW:
        p=i_dart_event_append_str(p,end,"interest-overflow peer id="); p=i_dart_event_append_u64(p,end,ev->peer);
        p=i_dart_event_append_str(p,end,": "); p=i_dart_event_append_u64(p,end,ev->lost_count);
        p=i_dart_event_append_str(p,end," matched topics beyond our alias table (raise DART_META_MAX_IDS)"); break;
    case DART_E_META_TRUNCATED_INTEREST:
        p=i_dart_event_append_str(p,end,"meta-truncated: interest list dropped (announce overlay full)"); break;
    case DART_E_META_TRUNCATED_SCHEMA:
        p=i_dart_event_append_str(p,end,"meta-truncated: schema section dropped (announce overlay full)"); break;
    case DART_E_PEER_META_TOO_BIG:
        p=i_dart_event_append_str(p,end,"peer-meta-too-big id="); p=i_dart_event_append_u64(p,end,ev->peer);
        if (ev->ip_len==4){ p=i_dart_event_append_str(p,end," at "); p=i_dart_event_append_addr(p,end,ev); }
        p=i_dart_event_append_str(p,end,": "); p=i_dart_event_append_u64(p,end,ev->too_big_bytes);
        p=i_dart_event_append_str(p,end," byte blob exceeds our capacity, refused"); break;
    case DART_E_MSG_TOO_BIG:
        p=i_dart_event_append_str(p,end,"msg-too-big "); p=i_dart_event_append_ch(p,end,ev);
        p=i_dart_event_append_str(p,end," from id="); p=i_dart_event_append_u64(p,end,ev->peer);
        p=i_dart_event_append_str(p,end," ("); p=i_dart_event_append_u64(p,end,ev->too_big_bytes);
        p=i_dart_event_append_str(p,end," bytes), skipped"); break;
    case DART_E_PEER_REFUSED:
        p=i_dart_event_append_str(p,end,"peer-refused at "); p=i_dart_event_append_addr(p,end,ev);
        p=i_dart_event_append_str(p,end,": peer table full of active peers (raise max_peers)"); break;
    case DART_E_EVICTED_UNSENT:
        p=i_dart_event_append_str(p,end,"evicted-unsent "); p=i_dart_event_append_ch(p,end,ev);
        p=i_dart_event_append_str(p,end," seqno "); p=i_dart_event_append_u64(p,end,ev->lost_first);
        p=i_dart_event_append_str(p,end,".."); p=i_dart_event_append_u64(p,end,ev->lost_first + ev->lost_count - 1);
        p=i_dart_event_append_str(p,end,": send burst outran the TX drain"); break;
    case DART_E_OOM:
        p=i_dart_event_append_str(p,end,"out-of-memory");
        if (ev->too_big_bytes){ p=i_dart_event_append_str(p,end,": "); p=i_dart_event_append_u64(p,end,ev->too_big_bytes);
                                p=i_dart_event_append_str(p,end," bytes needed"); }
        break;
    case DART_E_PLATFORM:
        p=i_dart_event_append_str(p,end,"platform net init failed"); break;
    case DART_E_SOCKET:
        p=i_dart_event_append_str(p,end,"socket open failed"); p=i_dart_event_append_oserr(p,end,ev); break;
    case DART_E_BIND:
        p=i_dart_event_append_str(p,end,"bind failed on port "); p=i_dart_event_append_u64(p,end,ev->port);
        p=i_dart_event_append_oserr(p,end,ev); break;
    case DART_E_MCAST_JOIN:
        p=i_dart_event_append_str(p,end,"multicast join failed"); p=i_dart_event_append_oserr(p,end,ev); break;
    case DART_E_SEND:
        p=i_dart_event_append_str(p,end,"send failed to id="); p=i_dart_event_append_u64(p,end,ev->peer);
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
    p = buf; end = buf + cap - 1;                  /* reserve one byte for the NUL */
    switch (ev->kind){
    case DART_PEER_UP:
        p = i_dart_event_append_str(p,end,"peer-up id="); p = i_dart_event_append_u64(p,end,ev->peer);
        if (ev->ip_len == 4){ p = i_dart_event_append_str(p,end," at "); p = i_dart_event_append_addr(p,end,ev); }
        break;
    case DART_PEER_DOWN:
        p = i_dart_event_append_str(p,end,"peer-down id="); p = i_dart_event_append_u64(p,end,ev->peer);
        break;
    case DART_PEER_INTEREST:
        p = i_dart_event_append_str(p,end,"interest id="); p = i_dart_event_append_u64(p,end,ev->peer);
        p = i_dart_event_append_str(p,end," publish-to="); p = i_dart_event_append_u64(p,end,ev->publish_topics);
        p = i_dart_event_append_str(p,end," topics, receive-from="); p = i_dart_event_append_u64(p,end,ev->receive_topics);
        p = i_dart_event_append_str(p,end," topics");
        break;
    case DART_MSG_LOST:
        p = i_dart_event_append_str(p,end,"msg-lost "); p = i_dart_event_append_ch(p,end,ev);
        p = i_dart_event_append_str(p,end," from id="); p = i_dart_event_append_u64(p,end,ev->peer);
        p = i_dart_event_append_str(p,end," seqno "); p = i_dart_event_append_u64(p,end,ev->lost_first);
        p = i_dart_event_append_str(p,end,".."); p = i_dart_event_append_u64(p,end,ev->lost_first + ev->lost_count - 1);
        break;
    case DART_ERROR:
        p = i_dart_event_error_str(p, end, ev);
        break;
    }
    *p = '\0';                                     /* p <= end = buf+cap-1, in range */
    return buf;
}

/* the node core's per-peer transport-lifecycle state, kept in the discovery peer's user
   scratch (so the node holds NO peer table of its own). added = wired into the transport
   yet (dart_transport_peer_add called); dormant = discovery DROPPED it, kept for a same-incarnation
   resume. Discovery zeroes this when a new UUID takes the slot, preserves it on resume. */
typedef struct { uint8_t added; uint8_t dormant; } i_DartNodePeerExtra;

/* schema state for the gate + delivery: peer schemas interned by hash (parsed once),
   reader views cached per (peer schema, channel), and the per-(peer, channel) schema a
   delivered message decodes with. All flat, linearly scanned (counts stay small), and
   allocated through the injected hook so they survive an arena migrate untouched. An
   interned/rebased schema lives until node close; a stale map pointer therefore never
   dangles. */
typedef struct { uint64_t hash; DartSchema *parsed; } i_DartNodeSchemaIntern;
typedef struct { uint64_t hash; uint16_t channel; DartSchema *rebased; } i_DartNodeSchemaBind;
typedef struct { uint32_t peer; uint16_t channel; const DartSchema *schema; } i_DartNodePeerSchema;

struct i_DartNodeCore {
    DartTransportState           *transport;
    DartDiscoveryState  *discovery;   /* the peer table (id<->addr, name, user scratch) we delegate to */
    DartEventFn         on_event;
    void                 *user;
    int                   oob_capable;
    uint8_t               oob_host[16];
    uint8_t              *meta_buf;    /* our outgoing discovery announce blob */
    uint16_t              meta_cap;
    uint16_t              meta_len;
    uint16_t              frag_size;   /* baked into the overlay */
    DartMetaSchema       *chan_schemas;  /* per-channel schema advertisement (hash 0 = none) */
    const DartSchema    **chan_compiled; /* per-channel parsed schema (the gate's local side) */
    uint16_t              n_channels;    /* sizes the per-channel arrays (and the meta buffer) */
    DartAllocFn           alloc;         /* backs the schema state below (may be NULL) */
    void                 *alloc_user;
    DartBytes             applying_meta; /* overlay being applied right now (schema-gate context) */
    uint32_t              applying_peer;
    uint8_t              *detail_buf;    /* detail-response scratch, hook-allocated + grown on
                                            demand (stable across a migrate; pool reset frees it) */
    uint32_t              detail_cap;
    i_DartNodeSchemaIntern *interned;     uint32_t n_interned,     cap_interned;
    i_DartNodeSchemaBind   *binds;        uint32_t n_binds,        cap_binds;
    i_DartNodePeerSchema   *peer_schemas; uint32_t n_peer_schemas, cap_peer_schemas;
};

/* grow one of the flat schema arrays through the hook; 1 + *arr/cap updated, or 0 */
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

/* arena layout: the core struct, the announce-blob buffer (fixed mode only: with an alloc
   hook the blob is hook-allocated at its ACTUAL size and grown at build time, instead of
   reserving dart_meta_capacity's worst case), then the per-channel schema registry (no peer
   table -- that lives in the discovery core). One sequence so measure and build agree. */
static void i_dart_node_core_layout(i_DartBump *b, uint16_t n_channels, int dynamic_meta,
                              i_DartNodeCore **out_c, uint8_t **out_meta,
                              DartMetaSchema **out_schemas, const DartSchema ***out_compiled){
    i_DartNodeCore *c       = (i_DartNodeCore*)i_dart_bump_take(b, sizeof(struct i_DartNodeCore), 16);
    uint8_t *meta           = dynamic_meta ? NULL
                            : (uint8_t*)   i_dart_bump_take(b, dart_meta_capacity(n_channels), 16);
    DartMetaSchema *schemas = (DartMetaSchema*)i_dart_bump_take(b, (size_t)n_channels*sizeof(DartMetaSchema), 16);
    const DartSchema **compiled = (const DartSchema**)i_dart_bump_take(b, (size_t)n_channels*sizeof(DartSchema*), 16);
    if (out_c)        *out_c        = c;
    if (out_meta)     *out_meta     = meta;
    if (out_schemas)  *out_schemas  = schemas;
    if (out_compiled) *out_compiled = compiled;
}

size_t i_dart_node_core_required_memory(uint16_t n_channels, int dynamic_meta){
    i_DartBump b; memset(&b, 0, sizeof b);
    i_dart_node_core_layout(&b, n_channels, dynamic_meta, NULL, NULL, NULL, NULL);
    return b.offset + 16u;   /* slack to align the caller's mem up to base */
}

i_DartNodeCore *i_dart_node_core_init(void *mem, size_t cap, const i_DartNodeCoreConfig *cfg){
    i_DartBump b; i_DartNodeCore *c; uint8_t *base, *meta;
    DartMetaSchema *schemas; const DartSchema **compiled;
    if (!mem || !cfg || !cfg->transport) return NULL;
    if (cap < i_dart_node_core_required_memory(cfg->n_channels, cfg->alloc != NULL)) return NULL;
    base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    memset(&b, 0, sizeof b); b.base = base; b.cap = cap - (size_t)(base - (uint8_t*)mem);
    i_dart_node_core_layout(&b, cfg->n_channels, cfg->alloc != NULL, &c, &meta, &schemas, &compiled);

    memset(c, 0, sizeof *c);
    c->transport     = cfg->transport;
    c->discovery     = cfg->discovery;     /* may be NULL now, bound via bind_discovery later */
    c->on_event      = cfg->on_event;      c->user = cfg->user;
    c->alloc         = cfg->alloc;         c->alloc_user = cfg->alloc_user;
    c->oob_capable   = cfg->oob_capable;
    memcpy(c->oob_host, cfg->oob_host, 16);
    c->meta_buf      = meta;                                            /* dynamic: NULL until the first build */
    c->meta_cap      = meta ? dart_meta_capacity(cfg->n_channels) : 0;
    c->frag_size     = cfg->frag_size;
    c->chan_schemas  = schemas;
    c->chan_compiled = compiled;
    c->n_channels    = cfg->n_channels;
    memset(schemas, 0, (size_t)cfg->n_channels * sizeof *schemas);
    memset(compiled, 0, (size_t)cfg->n_channels * sizeof *compiled);
    return c;
}

/* Relocate the sans-IO node core into a bigger block at grown counts. No peer table (it
 * lives in discovery); a struct copy carries the scalars. The transport, discovery, and
 * announce-blob pointers are re-pointed by the caller after those move. */
i_DartNodeCore *i_dart_node_core_migrate(i_DartNodeCore *old, void *new_mem, size_t new_cap,
                                       uint16_t new_n_channels){
    i_DartBump b; i_DartNodeCore *c; uint8_t *base, *meta;
    DartMetaSchema *schemas; const DartSchema **compiled;
    uint16_t keep;
    if (!old) return NULL;
    if (new_cap < i_dart_node_core_required_memory(new_n_channels, old->alloc != NULL)) return NULL;
    base = (uint8_t*)(((uintptr_t)new_mem + 15u) & ~(uintptr_t)15u);
    memset(&b, 0, sizeof b); b.base = base; b.cap = new_cap - (size_t)(base - (uint8_t*)new_mem);
    i_dart_node_core_layout(&b, new_n_channels, old->alloc != NULL, &c, &meta, &schemas, &compiled);
    *c = *old;                          /* scalars + transport/discovery ptrs (caller re-points);
                                           the hook-allocated schema arrays ride along untouched */
    if (meta){                          /* fixed mode: caller rebuilds the blob into the new slot */
        c->meta_buf = meta;
        c->meta_cap = dart_meta_capacity(new_n_channels);
    }                                   /* dynamic: the hook-allocated blob is stable, carried by *c = *old
                                           (the caller's rebuild grows it if the new counts need more) */
    keep = old->n_channels < new_n_channels ? old->n_channels : new_n_channels;
    memset(schemas, 0, (size_t)new_n_channels * sizeof *schemas);
    memcpy(schemas, old->chan_schemas, (size_t)keep * sizeof *schemas);   /* wire views stay valid: the
                                          DartSchema blocks live outside the arena and do not move */
    memset(compiled, 0, (size_t)new_n_channels * sizeof *compiled);
    memcpy(compiled, old->chan_compiled, (size_t)keep * sizeof *compiled);
    c->chan_schemas  = schemas;
    c->chan_compiled = compiled;
    c->n_channels    = new_n_channels;
    return c;
}

void i_dart_node_core_set_channel_schema(i_DartNodeCore *c, uint16_t channel,
                                         const DartSchema *schema){
    if (!c || channel >= c->n_channels) return;
    c->chan_compiled[channel]     = schema;
    c->chan_schemas[channel].hash = schema ? dart_schema_hash(schema) : 0;
    c->chan_schemas[channel].wire = schema ? dart_schema_wire(schema) : dart_bytes(NULL, 0);
}

/* ---- the schema gate + delivery binding ---------------------------------------------- */

/* a peer schema, parsed once per distinct hash. The claimed hash must equal the wire's
   real hash, or a lying peer could poison the intern for every honest one. NULL when the
   wire is absent (hash-only advert) or malformed. */
static DartSchema *i_dart_node_core_intern(i_DartNodeCore *c, uint64_t hash, DartBytes wire){
    uint32_t i; DartSchema *p;
    for (i = 0; i < c->n_interned; i++)
        if (c->interned[i].hash == hash) return c->interned[i].parsed;
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

/* the reader view for (writer schema, channel): our fields on their layout, cached */
static DartSchema *i_dart_node_core_bind(i_DartNodeCore *c, uint64_t hash, uint16_t channel,
                                         const DartSchema *ours, const DartSchema *pub){
    uint32_t i; DartSchema *rb;
    for (i = 0; i < c->n_binds; i++)
        if (c->binds[i].hash == hash && c->binds[i].channel == channel) return c->binds[i].rebased;
    rb = dart_schema_rebase(ours, pub, c->alloc, c->alloc_user);
    if (!rb) return NULL;
    if (!i_dart_node_core_array_reserve(c, (void**)&c->binds, &c->cap_binds,
                                        c->n_binds + 1u, sizeof *c->binds)){
        dart_schema_free(rb, c->alloc, c->alloc_user);
        return NULL;
    }
    c->binds[c->n_binds].hash = hash;
    c->binds[c->n_binds].channel = channel;
    c->binds[c->n_binds].rebased = rb;
    c->n_binds++;
    return rb;
}

/* the delivery map: which schema decodes (peer, channel). NULL entries are stored too
   (they overwrite an older binding when a peer re-advertises without a schema). */
static void i_dart_node_core_peer_schema_set(i_DartNodeCore *c, uint32_t peer, uint16_t channel,
                                             const DartSchema *schema){
    uint32_t i;
    for (i = 0; i < c->n_peer_schemas; i++)
        if (c->peer_schemas[i].peer == peer && c->peer_schemas[i].channel == channel){
            c->peer_schemas[i].schema = schema;
            return;
        }
    if (!schema) return;
    if (!i_dart_node_core_array_reserve(c, (void**)&c->peer_schemas, &c->cap_peer_schemas,
                                        c->n_peer_schemas + 1u, sizeof *c->peer_schemas)) return;
    c->peer_schemas[c->n_peer_schemas].peer = peer;
    c->peer_schemas[c->n_peer_schemas].channel = channel;
    c->peer_schemas[c->n_peer_schemas].schema = schema;
    c->n_peer_schemas++;
}

/* drop a peer's map entries (GONE, or its id recycled onto a new peer) */
static void i_dart_node_core_peer_schema_clear(i_DartNodeCore *c, uint32_t peer){
    uint32_t i = 0;
    while (i < c->n_peer_schemas){
        if (c->peer_schemas[i].peer == peer)
            c->peer_schemas[i] = c->peer_schemas[--c->n_peer_schemas];   /* swap-remove */
        else i++;
    }
}

const DartSchema *i_dart_node_core_msg_schema(i_DartNodeCore *c, uint32_t peer, uint16_t channel){
    uint32_t i;
    if (!c) return NULL;
    for (i = 0; i < c->n_peer_schemas; i++)
        if (c->peer_schemas[i].peer == peer && c->peer_schemas[i].channel == channel)
            return c->peer_schemas[i].schema;
    return NULL;
}

int i_dart_node_core_schema_check(i_DartNodeCore *c, uint16_t channel, uint16_t alias,
                                  int peer_is_pub){
    const DartSchema *ours = (channel < c->n_channels) ? c->chan_compiled[channel] : NULL;
    uint64_t hash = 0; DartBytes wire = dart_bytes(NULL, 0);
    int peer_has = c->applying_meta.data
                && dart_meta_schema(c->applying_meta, alias, &hash, &wire);
    if (peer_is_pub){                                   /* their publish entry: we would read */
        if (!ours){                                     /* generic reader: accept, decode with theirs */
            i_dart_node_core_peer_schema_set(c, c->applying_peer, channel,
                peer_has ? i_dart_node_core_intern(c, hash, wire) : NULL);
            return 1;
        }
        if (!peer_has) return 0;                        /* typed reader refuses an untyped writer */
        if (hash == dart_schema_hash(ours)){            /* identical schema: our own view works */
            i_dart_node_core_peer_schema_set(c, c->applying_peer, channel, ours);
            return 1;
        }
        {   DartSchema *pub = i_dart_node_core_intern(c, hash, wire);   /* need the wire to verify */
            DartSchema *view;
            if (!pub || !dart_schema_subset(ours, pub)) return 0;
            view = i_dart_node_core_bind(c, hash, channel, ours, pub);
            if (!view) return 0;                        /* OOM: refuse rather than misdecode */
            i_dart_node_core_peer_schema_set(c, c->applying_peer, channel, view);
            return 1;
        }
    } else {                                            /* their subscribe entry: we would write */
        if (!peer_has) return 1;                        /* a generic reader takes anything */
        if (!ours) return 0;                            /* typed reader refuses our raw channel */
        if (hash == dart_schema_hash(ours)) return 1;
        {   DartSchema *sub = i_dart_node_core_intern(c, hash, wire);
            return sub != NULL && dart_schema_subset(sub, ours);
        }
    }
}

/* (Re)build our discovery OVERLAY (frag size + OOB host + interest) from the core's
   current fields. The codec lives in the transport core; the OOB fields default to 0 in a
   non-SHM build. The node NAME is not here: the runtime hands it to discovery directly. */
uint16_t i_dart_node_core_build_meta(i_DartNodeCore *c){
    if (c->alloc){   /* dynamic: (re)size the blob buffer to the exact content first */
        uint16_t need = dart_transport_meta_size(c->transport, c->chan_schemas);
        if (need > c->meta_cap){
            uint8_t *nb = (uint8_t*)c->alloc(c->alloc_user, c->meta_buf, need);
            if (!nb) return c->meta_len;   /* OOM: keep the previous blob (stale but consistent) */
            c->meta_buf = nb; c->meta_cap = need;
        }
    }
    c->meta_len = dart_transport_meta_build(c->transport, c->meta_buf, c->meta_cap,
                                  c->frag_size, c->oob_capable, c->oob_host,
                                  c->chan_schemas);
    return c->meta_len;
}

DartBytes i_dart_node_core_meta(i_DartNodeCore *c){
    return dart_bytes(c->meta_buf, c->meta_len);
}

/* Answer a peer's DETAIL_REQ: validate kind + domain, then build the response (the codec
   is dart_transport_detail_* in the transport core) into the core's grown scratch buffer.
   Stateless: a pure read of channel + schema state, idempotent under duplicate requests.
   Returns the response bytes to send to the request's source, or {NULL,0} when not
   answerable (malformed, wrong domain, no alloc hook, or OOM: the requester just
   re-asks). An all-skipped response (header only) is still sent: it tells the requester
   those aliases are not advertised at our current version. */
DartBytes i_dart_node_core_detail_respond(i_DartNodeCore *c, uint16_t domain, DartBytes req){
    size_t need, len;
    if (!c || !c->alloc || !c->discovery) return dart_bytes(NULL, 0);
    if (dart_detail_kind(req) != DART_DETAIL_REQ || dart_detail_domain(req) != domain)
        return dart_bytes(NULL, 0);
    need = dart_transport_detail_resp_size(c->transport, c->chan_schemas, req);
    if (!need) return dart_bytes(NULL, 0);
    if (need > 65000u) need = 65000u;   /* one datagram: the build truncates at an entry
                                           boundary and the requester re-requests the rest */
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
/* a peer can receive our out-of-band (SHM) payload iff we are OOB-capable, it
 * advertised an OOB host in its meta blob, and that host equals ours (same kernel).
 * Set the transport's per-peer flag. The core knows nothing of SHM beyond this. */
static void i_dart_node_core_set_peer_oob(i_DartNodeCore *c, uint32_t id, DartBytes meta){
    uint8_t host[16];
    int oob = c->oob_capable && dart_meta_shm(meta, host) &&
              memcmp(host, c->oob_host, 16) == 0;
    dart_transport_peer_set_shm(c->transport, id, oob);
}
#else
#define i_dart_node_core_set_peer_oob(c, id, meta) ((void)0)
#endif

/* fire an info peer event (PEER_UP / PEER_DOWN). */
static void i_dart_node_core_fire(i_DartNodeCore *c, DartEventKind kind, uint32_t id,
                            const DartDiscoveryAddr *addr){
    DartEvent ev;
    if (!c->on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind; ev.peer = id; ev.user = c->user;
    if (addr){ memcpy(ev.ip, addr->ip, 16); ev.ip_len = addr->ip_len; ev.port = addr->port; }
    c->on_event(&ev);
}

/* fire a DART_ERROR event (err = which). too_big carries the OOM/meta-too-big byte count. */
static void i_dart_node_core_fire_error(i_DartNodeCore *c, DartErrorKind err, uint32_t id,
                            const DartDiscoveryAddr *addr, uint64_t too_big){
    DartEvent ev;
    if (!c->on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = DART_ERROR; ev.error = err; ev.peer = id; ev.user = c->user; ev.too_big_bytes = too_big;
    if (addr){ memcpy(ev.ip, addr->ip, 16); ev.ip_len = addr->ip_len; ev.port = addr->port; }
    c->on_event(&ev);
}

/* fired whenever a peer's interest list is (re)applied to the transport: reports how
 * many topics now flow each way, so an app/example can watch a connection form. */
static void i_dart_node_core_fire_interest(i_DartNodeCore *c, uint32_t id){
    DartEvent ev; uint16_t publish_to = 0, receive_from = 0;
    if (!c->on_event) return;
    dart_transport_peer_match_counts(c->transport, id, &publish_to, &receive_from);
    memset(&ev, 0, sizeof ev);
    ev.kind = DART_PEER_INTEREST; ev.peer = id;
    ev.publish_topics = publish_to; ev.receive_topics = receive_from;
    ev.user = c->user;
    c->on_event(&ev);
}

/* the peer's transport-lifecycle state lives in the discovery peer's user scratch; the
 * node core keeps no table of its own. NULL only before discovery is bound (no events yet). */
static i_DartNodePeerExtra *i_dart_node_core_peer_extra(i_DartNodeCore *c, uint32_t id){
    return (i_DartNodePeerExtra*)dart_discovery_peer_user(c->discovery, id);
}

static void i_dart_node_core_peer_up(i_DartNodeCore *c, uint32_t id, const DartDiscoveryAddr *addr,
                            DartBytes meta){
    i_DartNodePeerExtra *ex = i_dart_node_core_peer_extra(c, id);
    uint16_t frag = dart_meta_frag(meta);
    DartBytes interest = dart_meta_interest(meta);
    if (!ex) return;                                  /* discovery not bound / no scratch */
    c->applying_meta = meta; c->applying_peer = id;   /* schema-gate context for the applies below */
    if (!ex->added){                                  /* brand-new peer: wire it into the transport */
        i_dart_node_core_peer_schema_clear(c, id);    /* its id may be recycled: no stale bindings */
        dart_transport_peer_add(c->transport, id, frag);          /* blob carries frag + pub/sub interest */
        ex->added = 1; ex->dormant = 0;
        i_dart_node_core_set_peer_oob(c, id, meta);
        i_dart_node_core_fire(c, DART_PEER_UP, id, addr);
        if (interest.data){ dart_transport_apply_peer_interest(c->transport, id, interest);
                       i_dart_node_core_fire_interest(c, id); }
    } else {                                          /* known peer: addr/interest update */
        dart_transport_peer_set_frag(c->transport, id, frag);
        i_dart_node_core_set_peer_oob(c, id, meta);
        if (interest.data){ dart_transport_apply_peer_interest(c->transport, id, interest);
                       i_dart_node_core_fire_interest(c, id); }
        if (ex->dormant){    /* a DROPPED peer's same incarnation returned: resume */
            ex->dormant = 0;
            dart_transport_peer_resume(c->transport, id);   /* keeps reader position; writer fills any gap */
            i_dart_node_core_fire(c, DART_PEER_UP, id, addr);
        }
    }
    c->applying_meta = dart_bytes(NULL, 0); c->applying_peer = 0;
}

static void i_dart_node_core_peer_down(i_DartNodeCore *c, uint32_t id, DartDiscoveryDownReason reason){
    i_DartNodePeerExtra *ex = i_dart_node_core_peer_extra(c, id);   /* discovery frees the slot AFTER this event */
    if (reason == DART_DISCOVERY_DROP){
        /* fell silent: keep transport state so a same-incarnation return resumes
           losslessly; just stop flow-controlling it and tell the app once */
        if (ex && ex->added && !ex->dormant){
            ex->dormant = 1;
            dart_transport_peer_dormant(c->transport, id);
            i_dart_node_core_fire(c, DART_PEER_DOWN, id, NULL);
        }
    } else {   /* GONE: said BYE or its slot was reclaimed; free the transport state */
        int notify = (ex && ex->added && !ex->dormant);   /* active->gone: app not yet told */
        dart_transport_peer_remove(c->transport, id);   /* discovery zeroes the scratch on slot reuse */
        i_dart_node_core_peer_schema_clear(c, id);      /* the id may be reassigned */
        if (notify) i_dart_node_core_fire(c, DART_PEER_DOWN, id, NULL);
    }
}

static void i_dart_node_core_peer_refused(i_DartNodeCore *c, const DartDiscoveryAddr *addr){
    i_dart_node_core_fire_error(c, DART_E_PEER_REFUSED, 0, addr, 0);
}

/* The discovery core's on_event sink (cfg.user = this core): demux the generic
 * DartDiscoveryEvent into the lifecycle handlers above, which fire the app DartEvents. */
void i_dart_node_core_on_disc_event(const DartDiscoveryEvent *ev){
    i_DartNodeCore *c = (i_DartNodeCore*)ev->user;
    switch (ev->kind){
        case DART_DISCOVERY_PEER_UP:
            i_dart_node_core_peer_up(c, ev->peer, &ev->addr, ev->meta);  /* name lives in discovery */
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
    if (!dart_discovery_addr_of_id(c->discovery, to, &a)) return 0;   /* peer vanished */
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
    if (!name.data) return name;                          /* not a known peer: {NULL,0} */
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

uint16_t dart_node_peer_frag(const DartDiscoveryPeer *peer){
    return (peer && peer->meta.data) ? dart_meta_frag(peer->meta) : 0;
}

int dart_node_peer_interest_next(const DartDiscoveryPeer *peer,
                                 DartInterestIter *it, DartTopic *out){
    if (!peer || !peer->meta.data) return 0;
    return dart_meta_interest_next(peer->meta, it, out);
}

int dart_node_peer_schema(const DartDiscoveryPeer *peer, uint16_t alias,
                          uint64_t *hash, DartBytes *wire){
    if (!peer || !peer->meta.data){
        if (hash) *hash = 0;
        if (wire) *wire = dart_bytes(NULL, 0);
        return 0;
    }
    return dart_meta_schema(peer->meta, alias, hash, wire);
}
#pragma endregion
#pragma region node/runtime.c
/* NODE runtime: owns the data sockets, drives discovery, and the clock; it wires
 * peers into the transport via the sans-IO node core (node/core.h). Channels are
 * created at runtime and handed back as opaque handles. All OS access goes through
 * dart_plat. See node/runtime.h for the public dart_node_* / dart_channel_* API. */

#ifdef DART_SHM
#endif
#include <string.h>    /* heap access goes through i_dart_plat_realloc (no <stdlib.h> here) */

struct DartChannel { DartNode *n; uint16_t index; DartSchema *schema; };   /* schema: node-owned copy */

struct DartNode {
    DartTransportState     *transport;
    i_DartNodeCore *core;     /* peer table (id<->address) + discovery lifecycle (sans-IO) */
    DartDiscovery     *discovery;
    i_DartSock       fd;       /* unicast data socket */
    uint16_t      domain;
    DartNodeNet  net;         /* copy of opts.net: socket buffers + discovery addressing */
    /* datagram the socket refused; retried first next poll so it is never lost */
    uint8_t       tx_hold[DART_DGRAM_MAX];
    size_t        tx_hold_len;
    uint32_t      tx_hold_peer;
    /* data-socket RX buffer (in the arena): unicast discovery announces land on the data
       port too (solicit replies carry the full blob), so it is sized for the larger of a
       transport datagram and the biggest discovery datagram this node accepts. */
    uint8_t      *rx_buf;
    size_t        rx_buf_bytes;
    /* backpressure accumulators, read via dart_node_backpressure_stats */
    uint64_t      backpressure_total_us;
    uint32_t      backpressure_wait_count;
    uint32_t      evicted_unsent;  /* DART_E_EVICTED_UNSENT count (dart_node_evicted_unsent) */
#ifdef DART_THREADS
    /* thread safety + the optional service thread (runtime.h "Threading"). The lock is
       never held across a blocking wait: the poller drops it around the socket poll and
       any thread that mutates state kicks the waker so the change is serviced now. */
    i_DartMutex   mu;             /* THE node lock: every public entry point takes it */
    i_DartCond    cv;             /* senders/drainers waiting on the poller's progress */
    uint32_t      cv_waiters;     /* under mu; broadcast skipped when 0 */
    i_DartWaker   waker;          /* interrupts the unlocked socket wait */
    i_DartThread  svc;
    volatile uint8_t svc_running; /* a service thread is alive (dart_node_start) */
    volatile uint8_t svc_stop;    /* stop requested; the service loop exits on it */
    uint8_t       svc_joining;    /* under mu: one stopper owns the join; others wait on cv */
    uint32_t      pollers_sleeping; /* under mu: pollers in (or headed into) the unlocked wait */
    uint8_t       wake_signaled;  /* under mu: coalesce a burst into one waker datagram */
    uint8_t       user_locked;    /* the public dart_node_lock is held (pairs its unlock) */
    uint64_t      work_seq;       /* completed work passes (the unsent-wait predicate) */
    volatile uint8_t  lock_held;  /* owner-reentrancy check: only the owner thread ever */
    volatile uint64_t lock_owner; /* stores its own id, and zeroes it before releasing */
#endif
    /* diagnostic: in-pump probe sampled inside the backpressure wait (dart_channel_send) */
    DartPumpProbeFn pump_probe;
    void          *pump_probe_user;
    uint64_t       pump_probe_interval_us;
    /* app callbacks: one message sink (wrapped to build a DartMsg) + everything-else */
    DartMsgFn    user_on_message;
    DartEventFn  on_event;
    void          *user_data;
    DartEvent     last_error;  /* most recent DART_ERROR (dart_last_error(n)); DART_E_NONE until one fires */
    void          *arena;      /* control-structs block (a freeable pool allocation); relocated on grow */
    /* the node's paged region allocator (common/alloc.h): backs the node struct, arena, message
       buffers, schemas, and handles. COPIED from the caller's allocator at open (so the caller's
       may be a temporary); the node owns it and resets it on close. */
    DartAllocator pool;
    uint8_t        alloc_dynamic;
    uint8_t        grow_pending;   /* a peer was refused for lack of slots; grow at next poll */
    uint16_t       max_peers;      /* current peer-table capacity (doubles on a dynamic grow) */
    /* the metadata accept bound (largest peer announce blob we store/receive). Starts at
       dart_meta_capacity(max_channels) and SELF-HEALS: a peer whose blob exceeds it fires
       META_TOO_BIG with the needed size, we grow the bound + RX buffers at the next poll
       and solicit a re-announce, so a big-topology peer and a default node interoperate
       with no configuration. meta_grow_need is the pending request; meta_grow_failed
       remembers a size that would not allocate, so the retry loop surfaces instead of
       silently spinning. */
    uint16_t       meta_capacity;
    uint16_t       meta_grow_need;
    uint16_t       meta_grow_failed;
    /* channel handles. handles is a pointer array in the arena; each DartChannel struct
       is a separate stable allocation, so a grow that relocates the arena never moves a
       handle the user holds. */
    DartChannel **handles;    /* [max_channels] -> stable per-channel structs */
    uint16_t      max_channels;
    uint16_t      n_created;
#ifdef DART_SHM
    /* zero-fragment same-host path: lazy per-size-class segments (see shm/core.h) */
    uint8_t        shm_capable;       /* always 1: the node always has an allocator */
    uint8_t        shm_host[16];  /* our host uuid (advertised; same-host check) */
    uint64_t       shm_base;      /* per-node segment id base; low 19 bits = (channel<<3)|class */
    void          *shm_scratch; uint32_t shm_scratch_cap;   /* one-copy receive scratch */
    void         **shm_pool;      /* [nchan*N_CLASSES] our (channel,class) segments (NULL = not
                                     created); each created segment's state is a stable hook
                                     allocation, made when the segment is (lazily) created */
    uint16_t       shm_n_channels;
    /* reader-side attach cache, hook-allocated and grown on demand: sized by segments
       actually attached (same-host peers x their publishing channels), not the worst-case
       peers x channels x classes. A node with no same-host peer allocates none of it. */
    uint64_t      *shm_reader_segments;  /* [shm_reader_cap] attached segment ids */
    void         **shm_reader_states;    /* [shm_reader_cap] their pool states (stable allocations) */
    uint16_t       shm_reader_cap, shm_reader_count;
    uint32_t       shm_tx, shm_rx;/* messages published / delivered via SHM (observability) */
#endif
};

/* The node's message-buffer allocator, handed to the transport as its realloc hook and used
 * for schemas and channel handles (u is the node). It delegates to the node's paged region
 * allocator: one freeable allocation per call, so free/resize work and reset reclaims all. */
static void *i_dart_node_alloc(void *u, void *ptr, size_t size){
    return dart_allocator_alloc(&((DartNode*)u)->pool, ptr, size);
}

#ifdef DART_THREADS
/* Raw lock ops that keep the owner-id bookkeeping consistent. lock_owner is zeroed
   BEFORE the release so a non-owner reading the pair unlocked can never observe
   lock_held with its own id (it is the only thread that ever writes that id). */
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
/* Reentrant-aware entry lock: 1 = this call acquired mu (unlock releases it), 0 = the
   calling thread already held it (a callback calling back in, or the pump's nested
   poll). Reentrant callers proceed without waiting anywhere. */
static int i_dart_node_lock(DartNode *n){
    if (n->lock_held && n->lock_owner == i_dart_plat_thread_id()) return 0;
    i_dart_node_lock_raw(n);
    return 1;
}
static void i_dart_node_unlock(DartNode *n, int acquired){
    if (acquired) i_dart_node_unlock_raw(n);
}
/* With mu held, before unlocking: cut the pollers' blocking waits short so the state
   change (a queued send, a new advertisement, a stop) is serviced now, not at the
   next tick. One datagram wakes every sleeping poller (they all poll the same waker
   fd; the first to re-lock drains it), and the burst coalesces to one datagram per
   sleep. Armed only when the signal actually went out, so a failed loopback send
   can never wedge the coalescing. */
static void i_dart_node_kick(DartNode *n){
    if (n->pollers_sleeping && !n->wake_signaled && i_dart_plat_waker_signal(&n->waker))
        n->wake_signaled = 1;
}
/* Condvar wait that keeps the owner bookkeeping honest: mu is released inside the
   wait (another thread will take it and zero/claim the fields), so re-stamp ownership
   after it is reacquired. Anything cached from inside the node across this call is
   invalid: the poller may have grown/relocated the arena while we slept. */
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

/* --- error reporting --------------------------------------------------------------
 * One path for every runtime-side event: stamp the app's user_data, capture a DART_ERROR
 * into the node's last-error slot (so dart_last_error(n) can report it even with no
 * on_event set), then hand it to on_event. Zero cost on the success path: only errors and
 * the (already rare) lifecycle events ever reach here. */
static void i_dart_node_emit(DartNode *n, DartEvent *e){
    e->user = n->user_data;
    if (e->kind == DART_ERROR) n->last_error = *e;
    if (n->on_event) n->on_event(e);
}

/* our channel's name as a C string (the name pool is NUL-terminated), or NULL if the
 * channel is undefined: the channel_name view carried on channel-scoped events. */
static const char *i_dart_node_ch_name(DartNode *n, uint16_t ch){
    DartString s = dart_transport_channel_name(n->transport, ch);
    return (const char*)s.data;
}

/* The process-global last-error slot, for failures during dart_node_open where the node
 * does not exist yet (or is half-built). Best-effort: a plain global with no lock,
 * meaningful right after a failed open on the calling thread (open is rare; runtime
 * errors live in the per-node slot and never touch this). */
static DartEvent g_last_error;

/* Report an open-time failure: fill a DART_ERROR event, store it in the global slot, and
 * fire the caller's on_event directly (the node handle does not exist yet). Returns NULL
 * so a failing open can `return i_dart_node_open_fail(...)`. */
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

/* build a DartMsg and hand it to the app (the channel name is a local lookup, never
 * on the wire). Shared by the inline and SHM delivery paths. A message that does not
 * fit its sender's declared schema broke the sender's own contract: dropped + surfaced,
 * never handed to the app to misdecode. */
static void i_dart_node_deliver(DartNode *n, uint16_t ch, uint32_t from, DartBytes data){
    DartMsg m;
    const DartSchema *schema = i_dart_node_core_msg_schema(n->core, from, ch);
    if (schema && !dart_schema_validate(schema, data)){
        DartEvent e; memset(&e, 0, sizeof e);
        e.kind = DART_ERROR; e.error = DART_E_SCHEMA_MISMATCH;
        e.peer = from; e.channel = ch; e.channel_name = i_dart_node_ch_name(n, ch);
        i_dart_node_emit(n, &e);
        return;
    }
    if (!n->user_on_message) return;
    memset(&m, 0, sizeof m);
    m.node = n; m.user = n->user_data;
    m.channel_id = ch; m.sender_id = from;
    m.sender_name = i_dart_node_core_peer_name(n->core, from);          /* view into discovery state */
    if (!m.sender_name.data) m.sender_name = dart_cstr("unknown-peer"); /* .data never NULL on delivery */
    m.channel_name = dart_transport_channel_name(n->transport, ch);
    m.data = data;
    m.schema = schema;
    n->user_on_message(&m);
}
static void i_dart_node_on_message(void *u, uint16_t ch, uint32_t from, DartBytes data){
    i_dart_node_deliver((DartNode*)u, ch, from, data);
}
/* node-core events (the app DartEvent: PEER_UP/DOWN/INTEREST/REFUSED) funnel through
 * here; transport events arrive separately via i_dart_node_on_transport_event. The core
 * sets ev->user to this node; swap it for the app's real user_data before handing on. */
static void i_dart_node_on_event(const DartEvent *ev){
    DartNode *n = (DartNode*)ev->user;
    /* dynamic mode has no peer cap: a refusal means grow the table (deferred to the next
       poll, out of this callback); the peer re-announces and is admitted, so the app is
       never told it was refused. Static mode keeps the cap and surfaces the error. */
    if (n->alloc_dynamic && ev->kind == DART_ERROR && ev->error == DART_E_PEER_REFUSED){
        n->grow_pending = 1; return;
    }
    /* same treatment for the accept bound: a peer's blob we cannot hold (or even receive:
       the OS truncated the datagram, but the header carried its true size) is a growth
       signal, not a verdict. Grow at the next poll, then solicit the re-announce. Sizes
       past the wire ceiling, and a size that already failed to allocate, surface. */
    if (n->alloc_dynamic && ev->kind == DART_ERROR && ev->error == DART_E_PEER_META_TOO_BIG){
        uint64_t need = ev->too_big_bytes;
        if (need > n->meta_capacity && need <= 65000u && (uint16_t)need != n->meta_grow_failed){
            n->meta_grow_need = (uint16_t)need;
            return;
        }
    }
    { DartEvent e = *ev; i_dart_node_emit(n, &e); }
}

/* the transport's schema gate (DartConfig.schema_check): the node core answers it over
 * the serialize layer, using the overlay it is currently applying. u is the node. */
static int i_dart_node_schema_check(void *u, uint16_t channel, uint16_t alias, int peer_is_pub){
    return i_dart_node_core_schema_check(((DartNode*)u)->core, channel, alias, peer_is_pub);
}

/* transport events arrive as a DartTransportEvent; the node maps them onto its app
 * DartEvent union and hands them on. This is the node combining the two lower layers'
 * events into one app callback (peer events come via the node core, above). */
static void i_dart_node_on_transport_event(const DartTransportEvent *tev){
    DartNode *n = (DartNode*)tev->user;
    DartEvent e;
    memset(&e, 0, sizeof e);
    e.peer = tev->peer; e.channel = tev->channel;
    e.lost_first = tev->lost_first; e.lost_count = tev->lost_count;
    e.too_big_bytes = tev->too_big_bytes; e.identity = tev->identity;
    /* MSG_LOST is a top-level info kind (best-effort loss is expected); everything else
       is a DART_ERROR carrying its specific DartErrorKind. Channel-scoped kinds get our
       channel name for the message string. */
    switch (tev->kind){
    case DART_TRANSPORT_MSG_LOST:
        e.kind = DART_MSG_LOST; e.channel_name = i_dart_node_ch_name(n, tev->channel); break;
    case DART_TRANSPORT_MSG_TOO_BIG:
        e.kind = DART_ERROR; e.error = DART_E_MSG_TOO_BIG; e.channel_name = i_dart_node_ch_name(n, tev->channel); break;
    case DART_TRANSPORT_NAME_COLLISION:
        e.kind = DART_ERROR; e.error = DART_E_NAME_COLLISION; e.channel_name = i_dart_node_ch_name(n, tev->channel); break;
    case DART_TRANSPORT_QOS_INCOMPATIBLE:
        e.kind = DART_ERROR; e.error = DART_E_QOS_INCOMPATIBLE; e.channel_name = i_dart_node_ch_name(n, tev->channel); break;
    case DART_TRANSPORT_SCHEMA_MISMATCH:
        e.kind = DART_ERROR; e.error = DART_E_SCHEMA_MISMATCH; e.channel_name = i_dart_node_ch_name(n, tev->channel); break;
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

/* The node arena's sub-blocks, laid out in ONE place so the measure pass (bump.base
 * NULL, read bump.offset) and the build pass (read the pointers) run the same i_dart_bump_take
 * sequence and can never drift. */
typedef struct {
    uint8_t   *handles, *node_core, *transport, *discovery, *rx_buf;
#ifdef DART_SHM
    uint8_t   *shm_pool;
#endif
    size_t     node_core_bytes, transport_bytes, discovery_bytes, rx_buf_bytes;
} i_DartNodeBlocks;

static void i_dart_node_layout(i_DartBump *b, uint16_t max_peers, uint16_t max_channels,
                              const DartConfig *transport_cfg,
                              const DartDiscoveryNetConfig *discovery_rt_cfg, i_DartNodeBlocks *o){
    o->handles       = (uint8_t*)i_dart_bump_take(b, (size_t)max_channels * sizeof(DartChannel*), 16);
    o->node_core_bytes = i_dart_node_core_required_memory(max_channels, 1);   /* peers live in discovery; the
                                                                                 node always has an alloc hook,
                                                                                 so the blob is dynamic */
    o->node_core = (uint8_t*)i_dart_bump_take(b, o->node_core_bytes, 16);
    o->transport_bytes = dart_transport_required_memory(transport_cfg);
    o->transport = (uint8_t*)i_dart_bump_take(b, o->transport_bytes, 16);
#ifdef DART_SHM
    /* only the (channel,class) -> segment pointer table lives in the arena; segment state
       and the reader attach cache are lazy hook allocations, made when SHM is actually used */
    o->shm_pool = (uint8_t*)i_dart_bump_take(b, (size_t)max_channels * DART_SHM_N_CLASSES * sizeof(void*), 16);
#endif
    o->discovery_bytes = dart_discovery_placement_memory(discovery_rt_cfg);
    o->discovery = (uint8_t*)i_dart_bump_take(b, o->discovery_bytes, 16);
    /* data-socket RX buffer: sized so a unicast announce carrying the largest blob this
       node accepts fits (recvfrom drops an oversized datagram, which would leave a late
       joiner unable to ever fetch a big peer blob: its only path is this socket) */
    o->rx_buf_bytes = dart_discovery_wire_size(discovery_rt_cfg->discovery.meta_capacity);
    if (o->rx_buf_bytes < DART_DGRAM_MAX) o->rx_buf_bytes = DART_DGRAM_MAX;
    o->rx_buf = (uint8_t*)i_dart_bump_take(b, o->rx_buf_bytes, 16);
}

/* send one datagram to a peer; returns 1 when done with it, 0 only on a would-block
 * TX-full. The core resolves the abstract destination; we just map it to a UDP address. */
static int i_dart_node_tx(DartNode *n, uint32_t to, const uint8_t *buf, size_t len){
    i_DartNodeDest d;
    if (!i_dart_node_core_resolve(n->core, to, &d)) return 1;   /* peer vanished */
    if (i_dart_plat_send(n->fd, buf, len, d.ip, d.port) < 0){
        if (i_dart_plat_would_block()) return 0;                /* TX full: retry this datagram next tick */
        {   /* hard send failure: report it and drop the datagram (reliable data is repaired) */
            DartEvent e; memset(&e, 0, sizeof e);
            e.kind = DART_ERROR; e.error = DART_E_SEND; e.peer = to;
            e.os_error = i_dart_plat_last_socket_error();
            i_dart_node_emit(n, &e);
        }
    }
    return 1;
}

#ifdef DART_SHM
/* lazily create our per-channel segment: chunk_bytes = the channel's locked size class,
 * n_chunks = its keep_last, so history slot i binds chunk i (no free list). NULL on fail. */
static i_DartShmPool *i_dart_node_shm_channel_pool(DartNode *n, uint16_t ch, uint32_t k, uint16_t keep_last){
    i_DartShmConfig c; uint8_t *mem; uint64_t seg; size_t idx;
    if (k >= DART_SHM_N_CLASSES) return NULL;
    idx = (size_t)ch * DART_SHM_N_CLASSES + k;
    if (n->shm_pool[idx]) return (i_DartShmPool*)n->shm_pool[idx];
    memset(&c, 0, sizeof c);
    seg = n->shm_base | ((uint64_t)ch << 3) | (uint64_t)k;   /* low 3 bits class, next 16 channel */
    c.segment_id = seg;
    i_dart_shm_seg_name(c.name, seg);
    c.chunk_bytes = i_dart_shm_class_bytes(k);
    c.n_chunks = keep_last ? keep_last : 1u;
    mem = (uint8_t*)i_dart_node_alloc(n, NULL, i_dart_shm_state_bytes());   /* stable: survives arena grows */
    if (!mem) return NULL;
    n->shm_pool[idx] = i_dart_shm_create(mem, &c);
    if (!n->shm_pool[idx]) i_dart_node_alloc(n, mem, 0);
    return (i_DartShmPool*)n->shm_pool[idx];
}
/* lazily attach a peer's segment by id (class is in the low bits); cache it. The cache
 * grows with segments actually attached, so a node with no same-host peer holds none. */
static i_DartShmPool *i_dart_node_shm_reader_pool(DartNode *n, uint64_t seg){
    uint16_t i; i_DartShmConfig c; uint8_t *mem;
    for (i=0;i<n->shm_reader_count;i++)
        if (n->shm_reader_segments[i]==seg) return (i_DartShmPool*)n->shm_reader_states[i];
    if (n->shm_reader_count == n->shm_reader_cap){        /* grow the id + state-ptr arrays */
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
    c.segment_id = seg; i_dart_shm_seg_name(c.name, seg);     /* attach maps whole + reads geometry */
    mem = (uint8_t*)i_dart_node_alloc(n, NULL, i_dart_shm_state_bytes());
    if (!mem) return NULL;
    if (!i_dart_shm_attach(mem, &c)){ i_dart_node_alloc(n, mem, 0); return NULL; }
    n->shm_reader_segments[n->shm_reader_count] = seg;
    n->shm_reader_states[n->shm_reader_count] = mem;
    n->shm_reader_count++;
    return (i_DartShmPool*)mem;
}
static int i_dart_node_on_shm(void *u, uint16_t ch, uint32_t from, const uint8_t *desc){
    DartNode *n = (DartNode*)u; i_DartShmDesc d; i_DartShmPool *reader_pool; const void *p; uint32_t len;
    if (!i_dart_shm_desc_decode(&d, desc, DART_SHM_DESC_WIRE)) return 0;
    reader_pool = i_dart_node_shm_reader_pool(n, d.segment_id);
    if (!reader_pool) return 0;                                     /* can't attach -> reader NACKs (repair) */
    p = i_dart_shm_read(reader_pool, &d, &len);                       /* seqlock head: generation == descriptor? */
    if (!p) return 0;                                      /* recycled -> NACK -> repair or skip */
    /* one-copy: copy out of shared memory so the user owns the bytes (ack-timing safe) */
    if (len > n->shm_scratch_cap){
        void *new_buf = i_dart_node_alloc(n, n->shm_scratch, len?len:1u);
        if (!new_buf) return 0;
        n->shm_scratch = new_buf; n->shm_scratch_cap = len;
    }
    memcpy(n->shm_scratch, p, len);
    if (!i_dart_shm_verify(reader_pool, &d)) return 0;                /* seqlock tail: writer recycled mid-copy -> torn -> drop */
    n->shm_rx++;
    i_dart_node_deliver(n, ch, from, dart_bytes(n->shm_scratch, len));
    return 1;
}
#endif

DartNode *dart_node_open(DartAllocator *alloc, const char *name, DartMsgFn on_message, DartEventFn on_event, const DartNodeOpts *opts){
    DartNodeOpts o; DartDiscoveryNetConfig dc; DartConfig tc; i_DartNodeBlocks blocks;
    uint16_t max_peers, max_channels;
    uint8_t *base; void *arena; size_t need; DartAllocator pool;
    DartNode *n; i_DartSock fd; uint16_t local_port;
    char node_name[DART_NODE_NAME_MAX + 1]; uint8_t node_name_len = 0;   /* name is discovery-level */

    if (!alloc) return NULL;
    memset(&o, 0, sizeof o);
    if (opts) o = *opts;
    max_channels = o.max_channels ? o.max_channels : 8;
    max_peers    = o.discovery.max_peers ? o.discovery.max_peers : 16;

    /* sub-configs (sizing only depends on counts; the callback wrappers are set later) */
    memset(&dc,0,sizeof dc); memset(&tc,0,sizeof tc);
    dc.discovery.domain_id   = o.domain;
    dc.discovery.data_port   = o.net.data_port;
    dc.discovery.announce_interval_us = o.discovery.announce_interval_us;
    dc.discovery.peer_timeout_us = o.discovery.peer_timeout_us;
    dc.discovery.max_peers   = max_peers;
    dc.discovery.meta_capacity = dart_meta_capacity(max_channels);
    dc.discovery.peer_user_bytes = i_dart_node_core_peer_user_bytes();   /* node-core lifecycle state per peer */
    dc.discovery.alloc = i_dart_node_alloc;   /* per-peer blobs at actual size, not the worst-case pool
                                                 (set before sizing so measure and place agree; the
                                                 alloc_user is patched to n below, once it exists) */
    dc.group                 = o.net.discovery_group;
    dc.discovery_port        = o.net.discovery_port;
    dc.ttl                   = o.net.multicast_ttl;
    dc.multicast_interface   = o.net.multicast_interface;
    dc.seeds                 = o.net.seed_peers;
    dc.n_seeds               = o.net.n_seed_peers;
    tc.channels    = NULL;            /* reserve mode: channels created at runtime */
    tc.n_channels  = max_channels;
    tc.max_peers   = max_peers;
    tc.frag_payload= o.net.fragment_size;
    tc.allocator   = i_dart_node_alloc;   /* non-NULL => reserve/dynamic mode in dart_transport_init */

    {   i_DartBump b; memset(&b,0,sizeof b);
        i_dart_node_layout(&b, max_peers, max_channels, &tc, &dc, &blocks);
        need = b.offset + 32u; }

    /* The node COPIES the caller's allocator into its own pool (so the caller's may be a
       temporary, e.g. an open-and-return helper, and is left pristine). The node struct + the
       control-structs arena are freeable allocations from that pool: the node struct stays put
       across a grow (the user holds DartNode*), the arena is freed + relocated on grow, and
       message buffers/schemas/handles come from it. A failure resets a copy of the pool (which
       holds only what the node allocated), then returns. */
    pool = *alloc;
    n = (DartNode*)dart_allocator_alloc(&pool, NULL, sizeof *n);
    if (!n) return i_dart_node_open_fail(on_event, o.user_data, DART_E_OOM, 0, 0, sizeof *n);
    memset(n, 0, sizeof *n);
    n->pool = pool;                                  /* the node owns the pool now; allocate via &n->pool */
    n->alloc_dynamic = (alloc->page_realloc != NULL);
    arena = dart_allocator_alloc(&n->pool, NULL, need);
    if (!arena){ DartAllocator p = n->pool; dart_allocator_reset(&p);
                 return i_dart_node_open_fail(on_event, o.user_data, DART_E_OOM, 0, 0, need); }
    base = (uint8_t*)(((uintptr_t)arena+15u)&~(uintptr_t)15u);
    {   i_DartBump b; memset(&b,0,sizeof b);
        b.base = base; b.cap = need - (size_t)(base - (uint8_t*)arena);
        i_dart_node_layout(&b, max_peers, max_channels, &tc, &dc, &blocks); }

    if (!i_dart_plat_startup()){
        DartAllocator p = n->pool; dart_allocator_reset(&p);
        return i_dart_node_open_fail(on_event, o.user_data, DART_E_PLATFORM, 0, 0, 0);
    }

    n->fd = DART_SOCK_BAD;
    n->user_data = o.user_data;      /* set early so i_dart_node_emit can stamp it on any open-time error */
    n->on_event = on_event;
#ifdef DART_THREADS
    i_dart_plat_mutex_init(&n->mu);
    i_dart_plat_cond_init(&n->cv);
    /* opened for every node (not just started ones): it also wakes a plain
       dart_node_poll blocked in its wait when another thread sends. A platform
       whose loopback cannot carry it (exotic lwIP configs) DEGRADES rather than
       failing the open: the fd stays DART_SOCK_BAD, kick becomes a no-op, and
       cross-thread mutations are serviced at the next timer-capped tick (reported once). */
    if (!i_dart_plat_waker_open(&n->waker)){
        DartEvent e; memset(&e, 0, sizeof e);
        e.kind = DART_ERROR; e.error = DART_E_WAKER;
        i_dart_node_emit(n, &e);
    }
#endif
    n->domain = o.domain;
    n->net = o.net;
    n->user_on_message = on_message;   /* on_event + user_data were set early (open-time error reporting) */
    n->arena = arena;
    n->handles = (DartChannel**)blocks.handles;
    memset(n->handles, 0, (size_t)max_channels * sizeof(DartChannel*));
    n->rx_buf = blocks.rx_buf; n->rx_buf_bytes = blocks.rx_buf_bytes;
    n->max_channels = max_channels;
    n->max_peers = max_peers;
    n->meta_capacity = dc.discovery.meta_capacity;   /* the initial accept bound (self-heals up) */

    tc.on_message = i_dart_node_on_message;     /* wrap so on_message receives a DartMsg */
    tc.on_event   = i_dart_node_on_transport_event;  /* map DartTransportEvent -> app DartEvent */
    tc.schema_check = i_dart_node_schema_check; /* the schema gate, answered by the node core */
    tc.user       = n;
#ifdef DART_SHM
    n->shm_capable = (uint8_t)(n->alloc_dynamic && !o.disable_shm);   /* static mode never uses SHM */
    if (n->shm_capable){
        i_dart_plat_host_uuid(n->shm_host);
        if (!i_dart_plat_random(&n->shm_base, sizeof n->shm_base)) n->shm_base = i_dart_plat_pid();
        n->shm_base ^= (uint64_t)i_dart_plat_pid() << 32;    /* fold in pid for cross-process uniqueness */
        n->shm_base &= ~(((uint64_t)1u << 19) - 1u);       /* low 19 bits: 3 class + 16 channel index */
        if (n->shm_base == 0) n->shm_base = (uint64_t)1u << 19;
    }
    tc.on_shm = i_dart_node_on_shm;
#endif

    n->transport = dart_transport_init(blocks.transport, blocks.transport_bytes, &tc);
    if (!n->transport){ (void)i_dart_node_open_fail(on_event, o.user_data, DART_E_OOM, 0, 0, 0); goto fail_threads; }
#ifdef DART_SHM
    {   uint32_t n_segments = (uint32_t)max_channels * DART_SHM_N_CLASSES; uint32_t i;
        n->shm_n_channels = max_channels;
        n->shm_pool = (void**)blocks.shm_pool;
        for (i=0;i<n_segments;i++) n->shm_pool[i]=NULL;
        n->shm_reader_segments = NULL; n->shm_reader_states = NULL;   /* lazy: first attach allocates */
        n->shm_reader_cap = 0; n->shm_reader_count = 0;
    }
#endif

    /* sans-IO node core: drives the discovery->transport lifecycle and resolves addresses
       over the discovery core's peer table (bound below, once discovery exists). */
    node_name_len = dart_discovery_default_name(node_name, sizeof node_name, name);   /* handed to discovery below */
    {   i_DartNodeCoreConfig cc;
        memset(&cc, 0, sizeof cc);
        cc.transport = n->transport;   /* cc.discovery bound after dart_discovery_place */
        cc.n_channels = max_channels; cc.frag_size = dart_clamp_frag(o.net.fragment_size);
        cc.on_event = i_dart_node_on_event; cc.user = n;
        cc.alloc = i_dart_node_alloc; cc.alloc_user = n;   /* backs interned/rebased peer schemas */
#ifdef DART_SHM
        cc.oob_capable = n->shm_capable; memcpy(cc.oob_host, n->shm_host, 16);
#endif
        n->core = i_dart_node_core_init(blocks.node_core, blocks.node_core_bytes, &cc);
        if (!n->core){ (void)i_dart_node_open_fail(on_event, o.user_data, DART_E_OOM, 0, 0, 0); goto fail_threads; }
    }

    /* Bind the data socket before opening discovery so we can advertise its real
       port (0 => OS ephemeral, read back via getsockname). No reuse: a unicast
       endpoint owns its port, so a collision fails loudly here. */
    fd = i_dart_plat_udp_open();
    if (fd==DART_SOCK_BAD){ (void)i_dart_node_open_fail(on_event, o.user_data, DART_E_SOCKET, i_dart_plat_last_socket_error(), 0, 0); goto fail_threads; }
    n->fd=fd;                       /* owned now: fail_sock closes it */
    if (!i_dart_plat_bind(fd, 0, o.net.data_port, 0)){ (void)i_dart_node_open_fail(on_event, o.user_data, DART_E_BIND, i_dart_plat_last_socket_error(), o.net.data_port, 0); goto fail_sock; }
    local_port = i_dart_plat_local_port(fd);
    if (local_port==0){ (void)i_dart_node_open_fail(on_event, o.user_data, DART_E_SOCKET, 0, 0, 0); goto fail_sock; }
    i_dart_plat_set_nonblock(fd);   /* never block in recv/send; poll drains the queue */
    i_dart_plat_suppress_connreset(fd);  /* suppress WSAECONNRESET from a bounced send */
    if (o.net.recv_buffer_bytes) i_dart_plat_set_rcvbuf(fd, (int)o.net.recv_buffer_bytes);
    if (o.net.send_buffer_bytes) i_dart_plat_set_sndbuf(fd, (int)o.net.send_buffer_bytes);
    dc.discovery.data_port = local_port;       /* advertise the actual port */

    dc.discovery.on_event = i_dart_node_core_on_disc_event;   /* node core demuxes PEER_UP/DOWN/REFUSED */
    dc.discovery.user     = n->core;
    dc.discovery.alloc_user = n;               /* the blob hook allocates from the node's pool */
    dc.discovery.name     = dart_string(node_name, node_name_len);   /* discovery-owned (its own blob section) */
    /* the core builds our OVERLAY (frag size + OOB host + interest); discovery wraps it in
       its blob (after the locator + name) so peers reassemble and match from discovery */
    i_dart_node_core_build_meta(n->core);
    dc.discovery.meta = i_dart_node_core_meta(n->core);
    n->discovery = dart_discovery_place(blocks.discovery, blocks.discovery_bytes, &dc);
    if (!n->discovery){
        DartErrorKind err;   /* translate discovery's setup-failure reason into our vocabulary */
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
    /* the node core delegates id<->address resolution + per-peer scratch to discovery's table */
    i_dart_node_core_bind_discovery(n->core, dart_discovery_state(n->discovery));

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
    { DartAllocator p = n->pool; dart_allocator_reset(&p); }   /* frees the node struct + arena */
    return NULL;
}

/* Dynamic-mode growth: relocate the whole node into a bigger arena at the given counts so
 * a full peer table or channel reserve stops being a hard cap. Heap message buffers and SHM
 * writer segments stay put (only their owning control structures move); the user-held
 * DartNode and DartChannel handles live outside the arena, so they survive. Returns 1 with n
 * now on the new arena, or 0 if the bigger arena couldn't be allocated (n left unchanged). */
static int i_dart_node_grow(DartNode *n, uint16_t new_max_peers, uint16_t new_max_channels,
                            uint16_t want_meta_cap){
    DartConfig tc; DartDiscoveryNetConfig dc; i_DartNodeBlocks nb; i_DartBump b;
    DartTransportState *nt; i_DartNodeCore *ncore; DartDiscovery *ndisc;
    void *new_arena, *old_arena = n->arena;
    uint8_t *nbase; size_t need;
    uint16_t old_max_channels = n->max_channels;
    /* the accept bound never shrinks: channel-derived, previously grown, or requested */
    uint16_t new_meta_cap = dart_meta_capacity(new_max_channels);
    if (n->meta_capacity > new_meta_cap) new_meta_cap = n->meta_capacity;
    if (want_meta_cap    > new_meta_cap) new_meta_cap = want_meta_cap;

    if (!n->alloc_dynamic) return 0;
    if (new_max_peers <= n->max_peers && new_max_channels <= n->max_channels
        && new_meta_cap <= n->meta_capacity) return 0;

    memset(&tc,0,sizeof tc); memset(&dc,0,sizeof dc);
    tc.channels=NULL; tc.n_channels=new_max_channels; tc.max_peers=new_max_peers;
    tc.allocator=i_dart_node_alloc; tc.frag_payload=n->net.fragment_size;
    dc.discovery.max_peers=new_max_peers; dc.discovery.meta_capacity=new_meta_cap;
    dc.discovery.peer_user_bytes = i_dart_node_core_peer_user_bytes();   /* size discovery's scratch to match */
    dc.discovery.alloc = i_dart_node_alloc; dc.discovery.alloc_user = n; /* sizing must match the live core's
                                                                            hook mode (no arena blob pool) */

    memset(&b,0,sizeof b);
    i_dart_node_layout(&b, new_max_peers, new_max_channels, &tc, &dc, &nb);
    need = b.offset + 32u;
    new_arena = dart_allocator_alloc(&n->pool, NULL, need);
    if (!new_arena) return 0;
    nbase = (uint8_t*)(((uintptr_t)new_arena+15u)&~(uintptr_t)15u);
    memset(&b,0,sizeof b); b.base=nbase; b.cap=need-(size_t)(nbase-(uint8_t*)new_arena);
    i_dart_node_layout(&b, new_max_peers, new_max_channels, &tc, &dc, &nb);

    /* migrate the three cores; each leaves the old intact, so a failure just frees the new
       arena and bails (the old node keeps running, only refusing the would-be growth) */
    nt = dart_transport_migrate(n->transport, nb.transport, nb.transport_bytes, new_max_peers, new_max_channels);
    if (!nt){ dart_allocator_alloc(&n->pool, new_arena, 0); return 0; }
    ncore = i_dart_node_core_migrate(n->core, nb.node_core, nb.node_core_bytes, new_max_channels);
    if (!ncore){ dart_allocator_alloc(&n->pool, new_arena, 0); return 0; }
    ncore->transport = nt;                         /* re-point cross-layer pointer */
    i_dart_node_core_build_meta(ncore);              /* rebuild the announce blob into the new buf */
    ndisc = dart_discovery_migrate(n->discovery, nb.discovery, nb.discovery_bytes,
                                      new_max_peers, new_meta_cap, i_dart_node_core_meta(ncore).data, ncore);
    if (!ndisc){ dart_allocator_alloc(&n->pool, new_arena, 0); return 0; }
    i_dart_node_core_bind_discovery(ncore, dart_discovery_state(ndisc));   /* re-point to the relocated table */

    /* handle pointer array (the handle structs themselves are stable, not moved) */
    memcpy(nb.handles, n->handles, (size_t)old_max_channels*sizeof(DartChannel*));
    memset((DartChannel**)nb.handles + old_max_channels, 0,
           (size_t)(new_max_channels-old_max_channels)*sizeof(DartChannel*));

#ifdef DART_SHM
    if (n->shm_capable){
        uint32_t old_segs=(uint32_t)n->shm_n_channels*DART_SHM_N_CLASSES;
        uint32_t new_segs=(uint32_t)new_max_channels*DART_SHM_N_CLASSES, i;
        void **np = (void**)nb.shm_pool;
        for (i=0;i<new_segs;i++) np[i]=NULL;
        for (i=0;i<old_segs;i++) np[i]=n->shm_pool[i];   /* segment states are stable hook
                                                            allocations: only the pointer table moves */
        n->shm_pool=np; n->shm_n_channels=new_max_channels;
        /* the reader attach cache is hook-allocated too: attachments survive the grow */
    }
#endif

    n->transport=nt; n->core=ncore; n->discovery=ndisc;
    n->handles=(DartChannel**)nb.handles;
    n->rx_buf=nb.rx_buf; n->rx_buf_bytes=nb.rx_buf_bytes;
    n->max_channels=new_max_channels; n->max_peers=new_max_peers;
    n->meta_capacity=new_meta_cap;
    dart_allocator_alloc(&n->pool, old_arena, 0);    /* control structs only; heap bufs + segments moved by ref */
    n->arena=new_arena;
    return 1;
}

DartChannel *dart_node_create_channel(DartNode *n, const char *name, DartRole role,
                                      const DartSchema *schema, const DartChannelOpts *opts){
    DartChannelDef def; DartChannel *h; uint16_t idx; int acquired;
    if (!n || !name) return NULL;
    acquired = i_dart_node_lock(n);
    if (!acquired) return NULL;   /* from a callback: a grow here would relocate the arena mid-delivery */
    if (n->n_created >= n->max_channels){       /* reserve full: grow (dynamic) or refuse (static) */
        uint16_t want = n->max_channels < 0x8000u ? (uint16_t)(n->max_channels*2u) : 0xFFFFu;
        if (want <= n->max_channels || !i_dart_node_grow(n, n->max_peers, want, 0)){
            i_dart_node_unlock(n, acquired);
            return NULL;
        }
    }
    idx = n->n_created;
    h = (DartChannel*)i_dart_node_alloc(n, NULL, sizeof *h);   /* stable: outlives any arena grow */
    if (!h){ i_dart_node_unlock(n, acquired); return NULL; }
    h->schema = NULL;
    if (schema){   /* copy into node memory so the caller's schema need not outlive the channel */
        DartBytes w = dart_schema_wire(schema);
        h->schema = dart_schema_parse(w.data, w.len, i_dart_node_alloc, n);
        if (!h->schema){ i_dart_node_alloc(n, h, 0); i_dart_node_unlock(n, acquired); return NULL; }
    }
    memset(&def, 0, sizeof def);
    def.name = name; def.role = (uint8_t)role;
    if (opts) def.qos = opts->qos;
    if (dart_transport_channel_define(n->transport, idx, &def) != 0){
        if (h->schema) dart_schema_free(h->schema, i_dart_node_alloc, n);
        i_dart_node_alloc(n, h, 0);
        i_dart_node_unlock(n, acquired);
        return NULL;
    }
    h->n = n; h->index = idx;
    if (h->schema)   /* advertise + gate matches with it (any non-INACTIVE role) */
        i_dart_node_core_set_channel_schema(n->core, idx, h->schema);
    /* re-advertise our interest so peers match the new channel as the blob arrives, and
       replay known peers' interest so this channel matches what they already advertised */
    i_dart_node_core_build_meta(n->core);
    dart_discovery_advertise(n->discovery, i_dart_node_core_meta(n->core));
    dart_discovery_replay(n->discovery);
    n->handles[idx] = h;
    n->n_created++;
    i_dart_node_kick(n);                       /* announce the new channel now */
    i_dart_node_unlock(n, acquired);
    return h;
}

/* max wall-time draining RX (and running on_message) per poll tick before
 * yielding to discovery, so a slow on_message never starves it */
#ifndef DART_RX_BUDGET_US
#define DART_RX_BUDGET_US 5000u
#endif

/* drain one socket's RX queue into the transport until empty or past deadline
 * (full drain avoids NACK storms). Distinct from public dart_channel_drain. */
static void i_dart_node_rx_drain(DartNode *n, i_DartSock fd, uint64_t deadline){
    uint8_t *buf = n->rx_buf;
    for (;;){
        uint8_t src_ip[4]; uint16_t src_port;
        int r = i_dart_plat_recv(fd, buf, n->rx_buf_bytes, src_ip, &src_port);
        if (r<0){
            if (i_dart_plat_would_block()) break;        /* queue empty */
            {   /* a hard recv error (rare: connreset is suppressed on the data socket).
                   report it and stop draining this tick so we never spin on a wedged socket. */
                DartEvent e; memset(&e, 0, sizeof e);
                e.kind = DART_ERROR; e.error = DART_E_RECV; e.os_error = i_dart_plat_last_socket_error();
                i_dart_node_emit(n, &e);
            }
            break;
        }
        if (r>0){
            if (r>=4 && buf[0]=='u' && buf[1]=='D' && buf[2]=='S' && buf[3]=='C'){
                /* unicast announce aimed at our data port: hand it to discovery */
                dart_discovery_feed(n->discovery, src_ip, 4, dart_bytes(buf, (size_t)r));
            } else if (r>=5 && buf[0]=='u' && buf[1]=='D' && buf[2]=='T' && buf[3]=='L'){
                /* pairwise detail exchange: answer a request to its SOURCE (stateless, so
                   any requester works, peer or not: the explorer, a not-yet-added node).
                   A failed/refused send is just dropped: the requester re-asks on the
                   responder's next announce. A RESP has no consumer here yet. */
                if (buf[4]==DART_DETAIL_REQ){
                    DartBytes resp = i_dart_node_core_detail_respond(n->core, n->domain,
                                                                     dart_bytes(buf, (size_t)r));
                    if (resp.len) i_dart_plat_send(fd, resp.data, resp.len, src_ip, src_port);
                }
            } else {
                uint32_t from;
                if (i_dart_node_core_id_for_addr(n->core, src_ip, src_port, &from))
                    dart_transport_on_datagram(n->transport, from, dart_bytes(buf, (size_t)r), i_dart_plat_now_us());
            }
        }
        if (i_dart_plat_now_us() >= deadline) break;      /* yield to discovery/send */
    }
}

/* threaded mode: the longest a send waits for a poller to hand unsent head history
 * to the wire before overwriting it (KEEP_LAST + DART_E_EVICTED_UNSENT after). One
 * waker-kicked work pass normally clears it in microseconds; the bound only gates
 * a genuinely wedged socket. */
#ifndef DART_UNSENT_WAIT_US
#define DART_UNSENT_WAIT_US 20000u
#endif

/* Cap a wait at the transport's next timer (ack/NACK/heartbeat) and discovery's next
 * announce, so both fire on time with no traffic to wake us. Lock held; pure compute. */
static int i_dart_node_wait_ms(DartNode *n, int timeout_ms){
    uint64_t next = dart_transport_next_deadline_us(n->transport);
    uint64_t due  = dart_discovery_next_due_us(dart_discovery_state(n->discovery));
    if (timeout_ms < 0) timeout_ms = 0;
    if (!next || due < next) next = due;   /* due is a real time (0 = now); next 0 = none */
    if (next){
        uint64_t t0 = i_dart_plat_now_us();
        uint64_t us = (next > t0) ? next - t0 : 0;
        int ms = (us >= (uint64_t)timeout_ms*1000u) ? timeout_ms
                                                    : (int)((us + 999u)/1000u);   /* round up: no busy-spin */
        if (ms < timeout_ms) timeout_ms = ms;
    } else timeout_ms = 0;                 /* something is due right now */
    return timeout_ms;
}

/* One poll tick with the node lock held: deferred grow + discovery tick, the blocking
 * wait, then RX drain + TX flush. The ONE poll body: dart_node_poll, the service loop,
 * and the backpressure pump all run it. outer=1 means this thread's own acquisition
 * wraps the call, so the wait may drop the lock (other threads send into it and kick
 * the waker); the pump's nested call passes 0 and keeps the lock across its wait. */
static void i_dart_node_poll_locked(DartNode *n, int timeout_ms, int outer){
    uint8_t buf[DART_DGRAM_MAX]; uint32_t to; size_t out_len; uint64_t now;
    i_DartPollfd pfd[5]; int nfds = 0, wait_ms, poll_rc;
#ifdef DART_THREADS
    int waker_slot = -1;
#endif

    /* a peer was refused last tick for lack of slots: grow the table now, between ticks
       (safe: not inside any layer's processing), then the peer's next announce is admitted */
    if (n->grow_pending){
        uint16_t want = n->max_peers < 0x8000u ? (uint16_t)(n->max_peers*2u) : 0xFFFFu;
        n->grow_pending = 0;
        if (want > n->max_peers) i_dart_node_grow(n, want, n->max_channels, 0);
    }
    /* a peer's announce blob exceeded the accept bound last tick: raise the bound (grows
       the RX buffers + discovery capacity with it), then solicit so the peer re-sends its
       blob to buffers that now fit. A failed grow is remembered so the retry loop surfaces
       through the normal event path instead of silently spinning. */
    if (n->meta_grow_need){
        uint16_t want = n->meta_grow_need;
        n->meta_grow_need = 0;
        if (want > n->meta_capacity){
            if (i_dart_node_grow(n, n->max_peers, n->max_channels, want)){
                n->meta_grow_failed = 0;
                dart_discovery_solicit(dart_discovery_state(n->discovery));
            } else
                n->meta_grow_failed = want;
        }
    }

    dart_discovery_poll(n->discovery, 0);                 /* discovery tick (non-blocking) */

    wait_ms = i_dart_node_wait_ms(n, timeout_ms);
    memset(pfd, 0, sizeof pfd);
    pfd[nfds].fd = n->fd; pfd[nfds].events = DART_POLLIN; nfds++;
#ifdef DART_THREADS
    if (n->waker.fd != DART_SOCK_BAD){
        waker_slot = nfds;
        pfd[nfds].fd = n->waker.fd; pfd[nfds].events = DART_POLLIN; nfds++;
    }
#endif
    {   /* discovery's sockets join the wait so an inbound announce cuts a long sleep
           short (drained by the next tick's discovery poll). The fds are stable by
           value: a grow while the lock is dropped relocates structs, never sockets. */
        i_DartSock dfds[2]; int i, dn = dart_discovery_pollfds(n->discovery, dfds);
        for (i = 0; i < dn; i++){ pfd[nfds].fd = dfds[i]; pfd[nfds].events = DART_POLLIN; nfds++; }
    }

#ifdef DART_THREADS
    if (outer){
        /* the wait runs UNLOCKED so a sender on another thread is never blocked behind
           it; pollers_sleeping tells such a sender to kick the waker on a state change
           (a counter, not a flag: several threads may poll the same node) */
        n->pollers_sleeping++;
        i_dart_node_unlock_raw(n);
        poll_rc = i_dart_plat_poll(pfd, nfds, wait_ms);
        i_dart_node_lock_raw(n);
        n->pollers_sleeping--;
    } else {
        poll_rc = i_dart_plat_poll(pfd, nfds, wait_ms);
    }
    if (waker_slot >= 0){
        i_dart_plat_waker_drain(&n->waker);
        n->wake_signaled = 0;
    }
#else
    (void)outer;
    poll_rc = i_dart_plat_poll(pfd, nfds, wait_ms);
#endif
    if (poll_rc < 0){   /* the wait itself failed (a real fault, e.g. a bad fd; would-block is not it) */
        DartEvent e; memset(&e, 0, sizeof e);
        e.kind = DART_ERROR; e.error = DART_E_POLL; e.os_error = i_dart_plat_last_socket_error();
        i_dart_node_emit(n, &e);
    }

    if (pfd[0].revents & DART_POLLIN)
        i_dart_node_rx_drain(n, n->fd, i_dart_plat_now_us() + DART_RX_BUDGET_US);

    now=i_dart_plat_now_us();
    /* the core already consumed any held datagram, so retry it before pulling new */
    if (n->tx_hold_len && i_dart_node_tx(n, n->tx_hold_peer, n->tx_hold, n->tx_hold_len))
        n->tx_hold_len = 0;
    if (!n->tx_hold_len)
        while (dart_transport_poll_send(n->transport,&to,buf,sizeof buf,&out_len,now)){
            if (!i_dart_node_tx(n, to, buf, out_len)){
                memcpy(n->tx_hold, buf, out_len);
                n->tx_hold_len = out_len; n->tx_hold_peer = to;
                break;          /* TX buffer full: yield this tick */
            }
            now=i_dart_plat_now_us();
        }

#ifdef DART_THREADS
    n->work_seq++;
    if (n->cv_waiters) i_dart_plat_cond_broadcast(&n->cv);   /* acks/TX may have progressed */
#endif
}

int dart_node_poll(DartNode *n, int timeout_ms){
    int acquired;
    if (!n) return DART_ERR_STATE;
    acquired = i_dart_node_lock(n);
#ifdef DART_THREADS
    if (!acquired || n->svc_running){
        /* called from inside a callback, or a service thread owns the loop: refuse
           loudly rather than double-drive the node */
        i_dart_node_unlock(n, acquired);
        return DART_ERR_STATE;
    }
#endif
    i_dart_node_poll_locked(n, timeout_ms, acquired);
    i_dart_node_unlock(n, acquired);
    return 0;
}

/* publish on a channel index: flow-control wait (condvar under a service thread, the
 * nested pump otherwise), then SHM fast path, then UDP. may_wait=0 is a reentrant send
 * (from a callback): it can never block or run the loop, so it commits KEEP_LAST-style
 * and only the unsent guard below applies. */
static int i_dart_node_do_send(DartNode *n, uint16_t channel, DartBytes data, int may_wait){
    size_t len = data.len;
    /* one O(1) count gates the per-send fast paths: a channel no peer subscribes to skips
       backpressure and the SHM eligibility scan here, and the copy+commit in the core. */
    int matched = dart_transport_writer_match_count(n->transport, channel);
    const DartQos *q = dart_transport_channel_qos(n->transport, channel);
    int guarded = 0;   /* the unsent-eviction check runs after the wait */

#ifdef DART_THREADS
    if (matched && n->svc_running){
        guarded = 1;
        /* Threaded: wait on the condvar for the poller's progress instead of pumping the
           loop ourselves. Two predicates: unacked reliable history (bounded by
           qos.backpressure_wait_us, as the pump always was) and UNSENT history on any
           lane (bounded by DART_UNSENT_WAIT_US; one kicked work pass normally clears it,
           so bursts block for microseconds, not the bound). */
        if (may_wait){
            uint64_t now = i_dart_plat_now_us(), t0 = now;
            uint64_t rel_deadline = (q && q->backpressure_wait_us) ? now + q->backpressure_wait_us : 0;
            uint64_t unsent_deadline = now + DART_UNSENT_WAIT_US;
            uint64_t seq0 = n->work_seq;
            int waited = 0;
            n->cv_waiters++;
            for (;;){
                int evict_unacked = rel_deadline && dart_transport_send_would_evict(n->transport, channel);
                int evict_unsent  = dart_transport_send_would_evict_unsent(n->transport, channel, NULL, NULL);
                uint64_t deadline;
                if (!evict_unacked && !evict_unsent) break;
                /* unsent-only after a completed work pass: a pass either empties the TX
                   queue or parks a datagram in tx_hold on a socket would-block. Held
                   datagram = the SOCKET is the bottleneck, so KEEP_LAST drop-oldest
                   applies; no hold = another sender refilled the ring meanwhile
                   (contention), so re-arm and keep waiting (the deadline still bounds it) */
                if (!evict_unacked && n->work_seq != seq0){
                    if (n->tx_hold_len) break;
                    seq0 = n->work_seq;
                }
                now = i_dart_plat_now_us();
                deadline = evict_unacked
                         ? (rel_deadline > unsent_deadline ? rel_deadline : unsent_deadline)
                         : unsent_deadline;
                if (now >= deadline) break;
                waited = 1;
                i_dart_node_kick(n);
                i_dart_node_cv_wait(n, deadline - now);   /* mu drops: nothing cached survives */
                if (!n->svc_running) break;               /* stopped under us */
            }
            n->cv_waiters--;
            if (waited){
                n->backpressure_total_us += i_dart_plat_now_us() - t0;
                n->backpressure_wait_count++;
            }
            /* the poller may have grown/relocated the arena while we slept */
            q = dart_transport_channel_qos(n->transport, channel);
            matched = dart_transport_writer_match_count(n->transport, channel);
        }
    } else
#endif
    /* No service thread: bounded backpressure pump. Run the loop (on_message/on_event
       fire here) until a slow reader acks or qos.backpressure_wait_us elapses, then
       send anyway. */
    if (matched && may_wait && q && q->backpressure_wait_us && dart_transport_send_would_evict(n->transport, channel)){
        uint64_t t0 = i_dart_plat_now_us(), deadline = t0 + q->backpressure_wait_us;
        /* in-pump diagnostic: the publisher is blocked here for the whole wait, so its
           normal per-message print sees nothing within it. When a probe is set, sample
           the writer's repair progress on a ~interval timer so the stall is visible as a
           within-block time series (resends bursty-then-flat vs steady; writer idle for
           lack of NACKs). Observational; when no probe is set this whole block is skipped. */
        uint64_t sample_last = t0; uint32_t sample_polls = 0, sample_idle = 0;
        uint64_t interval = n->pump_probe_interval_us ? n->pump_probe_interval_us : 200000u;
        DartRepairStats sample_prev;
        if (n->pump_probe) dart_transport_repair_stats(n->transport, channel, &sample_prev);
        guarded = 1;
        do {
            i_dart_node_poll_locked(n, 1, 0);   /* nested tick: the lock stays held */
            if (n->pump_probe){
                uint64_t now = i_dart_plat_now_us();
                sample_polls++;
                if (dart_transport_repair_pending(n->transport, channel) == 0) sample_idle++;
                if (now - sample_last >= interval){
                    DartRepairStats sample_now; DartPumpSample sample;
                    dart_transport_repair_stats(n->transport, channel, &sample_now);
                    sample.channel         = channel;
                    sample.wait_elapsed_us = now - t0;
                    sample.interval_us     = now - sample_last;
                    sample.frags_resent    = sample_now.frags_resent - sample_prev.frags_resent;
                    sample.nacks_recv      = sample_now.nacks_recv  - sample_prev.nacks_recv;
                    sample.polls           = sample_polls;
                    sample.polls_idle      = sample_idle;
                    n->pump_probe(n->pump_probe_user, &sample);
                    sample_prev = sample_now; sample_last = now; sample_polls = 0; sample_idle = 0;
                }
            }
            if (!dart_transport_send_would_evict(n->transport, channel)) break;
        } while (i_dart_plat_now_us() < deadline);
        n->backpressure_total_us += i_dart_plat_now_us() - t0;
        n->backpressure_wait_count++;
        /* a mid-pump grow relocates the arena: re-derive the cached pointers */
        q = dart_transport_channel_qos(n->transport, channel);
        matched = dart_transport_writer_match_count(n->transport, channel);
    }

    /* Never-silent: a send that evicts history never handed to the wire (the wait
       above timed out, the socket is wedged, or a reentrant send cannot wait) is
       surfaced. The would-evict state is read here (before the commit changes it),
       but the event fires only if the send actually commits, below: a send the
       transport then rejects (TOO_BIG / ROLE / OOM) overwrote nothing. */
    {
        uint64_t evict_base = 0; uint32_t evict_count = 0;
        int will_evict = guarded &&
            dart_transport_send_would_evict_unsent(n->transport, channel, &evict_base, &evict_count);
        int r;
#ifdef DART_SHM
        if (n->shm_capable && len>0 && channel < n->shm_n_channels && matched && dart_transport_writer_shm_eligible(n->transport, channel)){
            uint16_t keep_last = (q && q->keep_last) ? q->keep_last : 1u;
            /* a hint (shm_max_bytes / max_message_bytes) pins the channel to one class, so
               same-sized traffic reuses a single prefix-sized segment; without it each message
               uses its own size class's segment, created on demand. Per channel either way. */
            uint32_t hint = q ? (q->shm_max_bytes ? q->shm_max_bytes : q->max_message_bytes) : 0u;
            uint32_t k = i_dart_shm_class_for(hint ? hint : (uint32_t)len);
            /* fits its class (with a hint, the hinted class) -> publish via SHM; chunk index =
               the history slot this send will occupy, so chunk i binds slot i (no free list) */
            if (k < DART_SHM_N_CLASSES && (uint32_t)len <= i_dart_shm_class_bytes(k)){
                i_DartShmPool *pool = i_dart_node_shm_channel_pool(n, channel, k, keep_last);
                uint16_t slot = dart_transport_channel_hist_head(n->transport, channel);
                void *chunk_ptr = pool ? i_dart_shm_chunk(pool, slot, NULL) : NULL;
                if (chunk_ptr){
                    i_DartShmDesc d; uint8_t desc[DART_SHM_DESC_WIRE];
                    memcpy(chunk_ptr, data.data, len);                  /* one-copy write into shm */
                    i_dart_shm_stamp(pool, slot, (uint32_t)len, &d);
                    i_dart_shm_desc_encode(&d, desc);
                    if (dart_transport_send_shm(n->transport, channel, dart_bytes(chunk_ptr, len), desc, i_dart_plat_now_us())==0){
                        n->shm_tx++;
                        r = DART_OK;
                        goto committed;
                    }
                }
            }
        }
#endif
        r = dart_transport_send(n->transport, channel, data, i_dart_plat_now_us());
#ifdef DART_SHM
committed:
#endif
        if (r == DART_OK && will_evict){
            DartEvent e; memset(&e, 0, sizeof e);
            n->evicted_unsent++;
            e.kind = DART_ERROR; e.error = DART_E_EVICTED_UNSENT;
            e.channel = channel; e.channel_name = i_dart_node_ch_name(n, channel);
            e.lost_first = evict_base; e.lost_count = evict_count;
            i_dart_node_emit(n, &e);
        }
        return r;
    }
}

int dart_channel_send(DartChannel *ch, DartBytes data){
    int acquired, r;
    if (!ch) return DART_ERR_NO_CHANNEL;
    acquired = i_dart_node_lock(ch->n);
    r = i_dart_node_do_send(ch->n, ch->index, data, acquired);
    i_dart_node_kick(ch->n);              /* flush the commit now, not at the next tick */
    i_dart_node_unlock(ch->n, acquired);
    return r;
}

int dart_channel_set_role(DartChannel *ch, DartRole role){
    int r, acquired;
    if (!ch) return -1;
    acquired = i_dart_node_lock(ch->n);
    if (!acquired){
        /* from a callback: the discovery replay would rematch/reset the very reader
           proxy mid-delivery, so refuse loudly */
        return DART_ERR_STATE;
    }
    r = dart_transport_set_role(ch->n->transport, ch->index, (uint8_t)role);
    if (r == 0){   /* re-advertise our interest: peers rematch as the new blob arrives */
        i_dart_node_core_build_meta(ch->n->core);
        dart_discovery_advertise(ch->n->discovery, i_dart_node_core_meta(ch->n->core));
        dart_discovery_replay(ch->n->discovery);   /* re-apply peers' interest to our new role */
        i_dart_node_kick(ch->n);                   /* announce the change now */
    }
    i_dart_node_unlock(ch->n, acquired);
    return r;
}

uint16_t dart_channel_index(const DartChannel *ch){ return ch ? ch->index : 0; }

const DartSchema *dart_channel_schema(const DartChannel *ch){ return ch ? ch->schema : NULL; }

DartChannel *dart_node_channel(DartNode *n, uint16_t index){
    DartChannel *h; int acquired;
    if (!n) return NULL;
    acquired = i_dart_node_lock(n);
    h = (index < n->n_created) ? n->handles[index] : NULL;
    i_dart_node_unlock(n, acquired);
    return h;
}

/* Read-only peer view: discovery already packs its peer table into a zero-copy array, and a
 * node peer IS a discovery peer (it adds only the decoded overlay, read on demand via
 * dart_node_peer_frag / dart_node_peer_interest_next). So this just forwards. With a poller
 * on another thread, bracket the CALL AND THE USE with dart_node_lock/dart_node_unlock
 * (from inside a callback the view is already safe for the callback's duration). */
const DartDiscoveryPeer *dart_node_peers(DartNode *n, uint16_t *count){
    return dart_discovery_peers(n ? n->discovery : NULL, count);
}

void dart_node_backpressure_stats(DartNode *n, uint64_t *waited_us, uint32_t *waited_sends){
    int acquired = i_dart_node_lock(n);
    if (waited_us)    *waited_us    = n->backpressure_total_us;
    if (waited_sends) *waited_sends = n->backpressure_wait_count;
    i_dart_node_unlock(n, acquired);
}

/* Sends that evicted never-emitted history after the bounded wait (the DART_E_EVICTED_UNSENT
 * count) since open: the burst/overload indicator. */
uint32_t dart_node_evicted_unsent(DartNode *n){
    uint32_t v; int acquired;
    if (!n) return 0;
    acquired = i_dart_node_lock(n);
    v = n->evicted_unsent;
    i_dart_node_unlock(n, acquired);
    return v;
}

void dart_node_mem_stats(DartNode *n, size_t *in_use, size_t *peak, uint64_t *alloc_calls){
    int acquired;
    if (!n) return;
    acquired = i_dart_node_lock(n);
    dart_allocator_stats(&n->pool, in_use, peak, alloc_calls);   /* live bytes, high-water, (re)allocs */
    i_dart_node_unlock(n, acquired);
}

void dart_channel_repair_stats(DartChannel *ch, DartRepairStats *out){
    if (ch){
        int acquired = i_dart_node_lock(ch->n);
        dart_transport_repair_stats(ch->n->transport, ch->index, out);
        i_dart_node_unlock(ch->n, acquired);
    } else if (out) memset(out, 0, sizeof *out);
}

void dart_node_set_pump_probe(DartNode *n, DartPumpProbeFn fn, uint64_t interval_us, void *user){
    int acquired = i_dart_node_lock(n);
    n->pump_probe = fn; n->pump_probe_interval_us = interval_us; n->pump_probe_user = user;
    i_dart_node_unlock(n, acquired);
}

int dart_channel_reader_progress(DartChannel *ch, uint32_t peer,
                                 uint64_t *base_seqno, uint32_t *have, uint32_t *total){
    int r, acquired;
    if (!ch) return 0;
    acquired = i_dart_node_lock(ch->n);
    r = dart_transport_reader_progress(ch->n->transport, ch->index, peer, base_seqno, have, total);
    i_dart_node_unlock(ch->n, acquired);
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

int dart_channel_drain(DartChannel *ch, int timeout_ms){
    DartNode *n; uint64_t deadline; int acquired, drained = 1;
    if (!ch) return 0;
    n = ch->n;
    acquired = i_dart_node_lock(n);
    if (!acquired) return 0;   /* from a callback: can neither pump nor wait */
    deadline = i_dart_plat_now_us() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000u;
#ifdef DART_THREADS
    if (n->svc_running){
        n->cv_waiters++;
        while (!dart_transport_send_drained(n->transport, ch->index)){
            uint64_t now = i_dart_plat_now_us();
            if (now >= deadline){ drained = 0; break; }
            i_dart_node_kick(n);
            i_dart_node_cv_wait(n, deadline - now);
            if (!n->svc_running) break;   /* stopped under us: finish with the pump below */
        }
        n->cv_waiters--;
        if (n->svc_running || !drained){
            i_dart_node_unlock(n, acquired);
            return drained;
        }
    }
#endif
    while (!dart_transport_send_drained(n->transport, ch->index)){
        if (i_dart_plat_now_us() >= deadline){ drained = 0; break; }
        i_dart_node_poll_locked(n, 1, 0);
    }
    i_dart_node_unlock(n, acquired);
    return drained;
}

int dart_channel_match_count(DartChannel *ch){
    int r, acquired;
    if (!ch) return 0;
    acquired = i_dart_node_lock(ch->n);
    r = dart_transport_writer_match_count(ch->n->transport, ch->index);
    i_dart_node_unlock(ch->n, acquired);
    return r;
}

#ifdef DART_THREADS
/* the service thread: the same poll body in a loop, sleeping exactly until the next
 * timer (or a waker kick). It holds the lock for every work pass and drops it inside
 * poll_locked's blocking wait, so user threads only ever contend with bounded work. */
static void i_dart_node_service(void *arg){
    DartNode *n = (DartNode *)arg;
    i_dart_node_lock_raw(n);
    while (!n->svc_stop)
        i_dart_node_poll_locked(n, 3600000, 1);   /* cap is moot: timers/kicks wake it */
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
    return DART_OK;                                     /* nothing to stop; idempotent */
#else
    int acquired;
    if (!n) return DART_ERR_STATE;
    acquired = i_dart_node_lock(n);
    if (!acquired) return DART_ERR_STATE;               /* from a callback (it IS the service) */
    if (n->svc_joining){
        /* another thread owns the join: wait for it to finish rather than joining
           the same thread twice (UB). Counted as a cv waiter so its broadcasts land. */
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
    i_dart_plat_thread_join(&n->svc);                   /* NEVER joined holding the lock */
    acquired = i_dart_node_lock(n);
    n->svc_running = 0;
    n->svc_stop = 0;
    n->svc_joining = 0;
    /* free every waiter: blocked senders see svc_running == 0 and proceed unwaited,
       and any concurrent stopper parked above returns */
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
    if (n->lock_held && n->lock_owner == i_dart_plat_thread_id()) return;  /* already ours (a callback) */
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
    /* from a callback: refuse LOUDLY (the return code lets a binding keep its handle
       alive instead of stranding a running node it can never close again) */
    if (n->lock_held && n->lock_owner == i_dart_plat_thread_id()) return DART_ERR_STATE;
    dart_node_stop(n);
    /* from here the contract holds: no other thread is inside, or may enter, any
       dart_* call on this node, so the teardown runs truly single-threaded */
#endif
    if (n->discovery) dart_discovery_close(n->discovery, send_bye);   /* frees peer blobs via our
                                                                         hook, so it must run BEFORE
                                                                         the pool is copied out */
    if (n->fd != DART_SOCK_BAD) i_dart_plat_close(n->fd);
#ifdef DART_SHM
    if (n->shm_capable){                              /* unmap OS segments; the pool reset frees only pool memory */
        uint32_t i, n_segments = (uint32_t)n->shm_n_channels * DART_SHM_N_CLASSES;
        for (i=0;i<n_segments;i++)
            if (n->shm_pool[i]) i_dart_shm_detach((i_DartShmPool*)n->shm_pool[i]);   /* unlinks ours */
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
    pool = n->pool;                /* copy out LAST (no hook frees may follow): the reset frees n itself */
    dart_allocator_reset(&pool);   /* frees the node struct, arena, message buffers, schemas, handles */
    return DART_OK;
}
#pragma endregion
#endif /* !DART_TRANSPORT_SANS_IO */
#endif /* DART_TRANSPORT_IMPLEMENTATION */


#if defined(__cplusplus) && !defined(DART_IMPLEMENTATION)
}   /* namespace detail */

/* Enums (1:1 with the C enums by value; asserted below). */
enum class Reliability { BestEffort = 0, Reliable = 1 };
enum class Role        { PubSub = 0, PubOnly = 1, SubOnly = 2, Inactive = 3 };

/* dart_channel_send / create result. Ok is 0; the rest mirror DartResult. */
enum class SendStatus  { Ok = 0, NoChannel = -1, TooBig = -2, BadRole = -3, OutOfMemory = -4,
                         State = -5, NoSys = -6 };

enum class EventKind {
    PeerUp = 0, PeerDown, PeerInterest, MessageLost, Error
};

/* The specific error carried by an EventKind::Error event (Event::error()); mirrors
   DartErrorKind. Everything that goes wrong is EventKind::Error + one of these. */
enum class ErrorKind {
    None = 0,
    NameCollision, QosIncompatible, SchemaMismatch, InterestOverflow,
    MetaTruncatedInterest, MetaTruncatedSchema, PeerMetaTooBig, MessageTooBig,
    PeerRefused, EvictedUnsent,
    Oom, Platform, Socket, Bind, McastJoin, Send, Recv, Poll, Waker
};

/* Schema field kinds for reflection (Schema::Field); values match the C wire. */
enum class FieldType : uint8_t {
    U8 = 0, U16, U32, U64, I8, I16, I32, I64, F32, F64, Bool, Array, Struct
};

static_assert((int)Reliability::Reliable == detail::DART_RELIABLE, "reliability enum drift");
static_assert((int)Role::Inactive == detail::DART_INACTIVE, "role enum drift");
static_assert((int)SendStatus::NoSys == detail::DART_ERR_NOSYS, "result enum drift");
static_assert((int)EventKind::Error == detail::DART_ERROR, "event enum drift");
static_assert((int)ErrorKind::Waker == detail::DART_E_WAKER, "error enum drift");
static_assert((int)FieldType::Struct == detail::DART_STRUCT, "field-type enum drift");

/* forward decls */
class Node;
class Channel;
class MessageIn;
class Event;

/* Bytes: a non-owning (pointer + length) view of a payload. It is a proper
 * contiguous range (begin/end/operator[], usable in range-for and algorithms)
 * and interops with the standard library both ways: construct it from a
 * string_view / std::string / const char*, or from any contiguous range of
 * byte-sized elements (std::vector / std::array / std::span of uint8_t, char, or
 * std::byte); convert it to std::string_view, std::string, std::vector, or
 * (with C++20) std::span. Multi-byte element types are rejected -- pass their
 * raw bytes explicitly via Bytes(ptr, len). */
class Bytes {
public:
    using value_type     = uint8_t;
    using iterator       = const uint8_t*;
    using const_iterator = const uint8_t*;

    constexpr Bytes() noexcept = default;
    constexpr Bytes(const void* d, size_t n) noexcept
        : data_(static_cast<const uint8_t*>(d)), size_(n) {}
    Bytes(std::string_view s) noexcept
        : data_(reinterpret_cast<const uint8_t*>(s.data())), size_(s.size()) {}
    Bytes(const std::string& s) noexcept : Bytes(std::string_view(s)) {}
    Bytes(const char* s) noexcept : Bytes(std::string_view(s ? s : "")) {}

    /* Any contiguous range of byte-sized elements. Strings are handled by the
     * overloads above (excluded here), so this covers vector/array/span/... of
     * uint8_t / char / std::byte without ambiguity. */
    template <class C,
              class E = std::remove_reference_t<decltype(*std::data(std::declval<const C&>()))>,
              class = std::enable_if_t<
                  !std::is_same<std::decay_t<C>, Bytes>::value &&
                  !std::is_convertible<const C&, std::string_view>::value &&
                  sizeof(E) == 1 && std::is_trivially_copyable<E>::value>>
    Bytes(const C& c) noexcept
        : data_(reinterpret_cast<const uint8_t*>(std::data(c))), size_(std::size(c)) {}

    const uint8_t* data()  const noexcept { return data_; }
    size_t         size()  const noexcept { return size_; }
    bool           empty() const noexcept { return size_ == 0; }
    const uint8_t* begin() const noexcept { return data_; }
    const uint8_t* end()   const noexcept { return data_ + size_; }
    const uint8_t& operator[](size_t i) const noexcept { return data_[i]; }

    std::string_view     str()       const noexcept { return { reinterpret_cast<const char*>(data_), size_ }; }
    std::string          to_string() const { return { reinterpret_cast<const char*>(data_), size_ }; }
    std::vector<uint8_t> to_vector() const { return { data_, data_ + size_ }; }

#if defined(__cpp_lib_span)
    operator std::span<const uint8_t>() const noexcept { return { data_, size_ }; }
    std::span<const uint8_t> span() const noexcept { return { data_, size_ }; }
#endif

private:
    const uint8_t* data_ = nullptr;
    size_t         size_ = 0;
};

/* Qos / NodeOptions: plain structs mirroring the C config, all zero = default. */
struct Qos {
    Reliability reliability          = Reliability::BestEffort;
    uint16_t    keep_last            = 0;   /* recent messages retained (late join / repair) */
    uint16_t    catch_up             = 0;   /* recent messages a new subscriber replays */
    uint32_t    max_message_bytes    = 0;   /* 0 = one fragment, or grow-to-fit */
    uint32_t    heartbeat_us         = 0;   /* reliable idle-writer ping (0 = 100ms) */
    uint32_t    repair_delay_us      = 0;   /* reliable reader's resend delay (0 = 20ms) */
    uint32_t    backpressure_wait_us = 0;   /* reliable: send pause for a slow reader (0 = none) */
    uint32_t    shm_max_bytes        = 0;   /* pin channel to one same-host SHM size class */
};

struct NodeOptions {
    uint16_t                 domain               = 0;   /* logical-network selector */
    uint16_t                 max_channels         = 8;   /* how many channels may be created */
    bool                     disable_shm          = false;
    /* networking (all optional) */
    uint16_t                 data_port            = 0;   /* 0 = OS-assigned */
    std::string              discovery_group;            /* empty = "239.255.0.<domain>" default */
    uint16_t                 discovery_port       = 0;   /* 0 = 7400 */
    std::string              multicast_interface;        /* empty = auto; "127.0.0.1" = single-host */
    uint8_t                  multicast_ttl        = 0;   /* 0 = 1 hop */
    std::vector<std::string> seed_peers;                 /* "ip" or "ip:port", unicast announce targets */
    uint16_t                 fragment_size        = 0;   /* UDP payload bytes per fragment */
    /* discovery cadence */
    uint32_t                 announce_interval_us = 0;   /* 0 = 1s */
    uint32_t                 peer_timeout_us      = 0;   /* 0 = 3.5s */
    uint16_t                 max_peers            = 0;   /* 0 = 16 */
    /* Memory. Leave `memory` null for the default dynamic allocator (grows on
     * demand). Set it to a fixed buffer for STATIC allocation: the node draws
     * ALL its memory from there, with no heap and no growth (and the same-host
     * SHM fast path off, since it needs a growable allocator). The buffer must
     * outlive the node and be big enough for the configured max_peers /
     * max_channels plus message buffers; open returns nullopt if it is too
     * small. A typed channel copies its schema into this buffer, so pair it with
     * Schema::compile(text, scratch, size) for a fully heap-free node. */
    void*                    memory      = nullptr;
    size_t                   memory_size = 0;
};

/* Schema: an owned, compiled message schema (see the DSL in schema.h). */
class Schema {
public:
    /* Compile a schema from its text form (see the DSL in schema.h). Returns
     * nullopt on error; if `err` is non-null it receives a short message
     * pointing near the offending text. Uses an internal dynamic (heap)
     * allocator for the compiled schema, freed when the Schema is destroyed. */
    static std::optional<Schema> compile(std::string_view text, std::string* err = nullptr) {
        return compile_with(detail::dart_allocator_dynamic(detail::i_dart_plat_realloc, 0), text, err);
    }
    /* Zero-heap variant: compile into your fixed `scratch` buffer instead of the
     * heap (for a static-memory deployment). The buffer must outlive this Schema;
     * once create_channel has copied the schema into the node, the Schema and its
     * scratch may be dropped or reused. Returns nullopt if the buffer is too small. */
    static std::optional<Schema> compile(std::string_view text, void* scratch, size_t scratch_size,
                                         std::string* err = nullptr) {
        if (!scratch || scratch_size == 0) { if (err) *err = "scratch buffer missing"; return std::nullopt; }
        return compile_with(detail::dart_allocator_static(scratch, scratch_size), text, err);
    }

    Schema(Schema&& o) noexcept : alloc_(o.alloc_), schema_(o.schema_) {
        std::memset(&o.alloc_, 0, sizeof o.alloc_);
        o.schema_ = nullptr;
    }
    Schema& operator=(Schema&& o) noexcept {
        if (this != &o) { reset(); alloc_ = o.alloc_; schema_ = o.schema_;
                          std::memset(&o.alloc_, 0, sizeof o.alloc_); o.schema_ = nullptr; }
        return *this;
    }
    Schema(const Schema&) = delete;
    Schema& operator=(const Schema&) = delete;
    ~Schema() { reset(); }

    std::string_view name() const {
        detail::DartString n = detail::dart_schema_name(schema_);
        return { n.data, n.len };
    }
    uint32_t size() const { return detail::dart_schema_size(schema_); }
    uint64_t hash() const { return detail::dart_schema_hash(schema_); }
    uint16_t field_count() const { return detail::dart_schema_field_count(schema_); }

    struct Field {
        std::string_view name;
        FieldType kind;          /* the field's type */
        FieldType elem;          /* array element type (only when kind == Array) */
        uint16_t  count, depth;
        uint32_t  offset, size;
    };
    bool field_at(uint16_t i, Field& out) const {
        detail::DartSchemaFieldInfo f;
        if (!detail::dart_schema_field_at(schema_, i, &f)) return false;
        out.name   = { f.name.data, f.name.len };
        out.kind   = static_cast<FieldType>(f.kind);
        out.elem   = static_cast<FieldType>(f.elem);
        out.count  = f.count; out.depth = f.depth;
        out.offset = f.offset; out.size = f.size;
        return true;
    }

private:
    Schema() = default;
    static std::optional<Schema> compile_with(detail::DartAllocator a, std::string_view text, std::string* err) {
        Schema s;
        s.alloc_ = a;
        std::string t(text);   /* NUL-terminate for the C API */
        const char* e = nullptr;
        s.schema_ = detail::dart_schema_compile(detail::dart_allocator_alloc, &s.alloc_, t.c_str(), &e);
        if (!s.schema_) {
            if (err) *err = e ? (std::string("schema error near: ") + e) : "schema compile failed";
            detail::dart_allocator_reset(&s.alloc_);
            return std::nullopt;
        }
        return s;
    }
    void reset() { if (alloc_.page_realloc || alloc_.shared) detail::dart_allocator_reset(&alloc_); schema_ = nullptr; }
    detail::DartAllocator alloc_{};
    detail::DartSchema*   schema_ = nullptr;

    friend class Node;
    friend class MessageOut;
    const detail::DartSchema* raw() const { return schema_; }
};

/* MessageOut: a mutable message buffer bound to a Schema, for typed encoding.
 * Set fields by name (nested members by dotted path, e.g. "vel.dx"), then
 * pass it straight to Channel::send (it converts to Bytes). */
class MessageOut {
public:
    explicit MessageOut(const Schema& s) : schema_(s.raw()), buf_(detail::dart_schema_size(s.raw())) {
        detail::dart_schema_message_default(schema_, buf_.data(), buf_.size());
    }
    MessageOut& set_uint (const char* field, uint64_t v) { detail::dart_set_uint (buf_.data(), buf_.size(), schema_, field, v); return *this; }
    MessageOut& set_int  (const char* field, int64_t  v) { detail::dart_set_int  (buf_.data(), buf_.size(), schema_, field, v); return *this; }
    MessageOut& set_f64  (const char* field, double   v) { detail::dart_set_f64  (buf_.data(), buf_.size(), schema_, field, v); return *this; }
    MessageOut& set_f32  (const char* field, float    v) { detail::dart_set_f32  (buf_.data(), buf_.size(), schema_, field, v); return *this; }
    MessageOut& set_bool (const char* field, bool     v) { detail::dart_set_uint (buf_.data(), buf_.size(), schema_, field, v ? 1u : 0u); return *this; }
    MessageOut& set_array(const char* field, Bytes elems) {
        detail::dart_set_array(buf_.data(), buf_.size(), schema_, field, detail::dart_bytes(elems.data(), elems.size()));
        return *this;
    }
    Bytes bytes() const { return { buf_.data(), buf_.size() }; }
    operator Bytes() const { return bytes(); }

private:
    const detail::DartSchema* schema_;
    std::vector<uint8_t>      buf_;
};

/* MessageIn: a delivered message. Non-owning; valid only inside the handler. */
class MessageIn {
public:
    std::string_view sender_name()  const { return { msg_->sender_name.data,  msg_->sender_name.len  }; }
    uint32_t         sender_id()    const { return msg_->sender_id; }
    std::string_view channel_name() const { return { msg_->channel_name.data, msg_->channel_name.len }; }
    uint16_t         channel_id()   const { return msg_->channel_id; }
    Bytes            data()         const { return { msg_->data.data, msg_->data.len }; }
    std::string_view text()         const { return { reinterpret_cast<const char*>(msg_->data.data), msg_->data.len }; }
    bool             has_schema()   const { return msg_->schema != nullptr; }

    /* Typed field reads (only meaningful when has_schema()); by name / dotted path. */
    uint64_t get_uint (const char* field) const { return detail::dart_get_uint (msg_->data, msg_->schema, field); }
    int64_t  get_int  (const char* field) const { return detail::dart_get_int  (msg_->data, msg_->schema, field); }
    double   get_f64  (const char* field) const { return detail::dart_get_f64  (msg_->data, msg_->schema, field); }
    float    get_f32  (const char* field) const { return detail::dart_get_f32  (msg_->data, msg_->schema, field); }
    bool     get_bool (const char* field) const { return detail::dart_get_uint (msg_->data, msg_->schema, field) != 0; }
    Bytes    get_array(const char* field) const { auto a = detail::dart_get_array(msg_->data, msg_->schema, field); return { a.data, a.len }; }

private:
    explicit MessageIn(const detail::DartMsg* m) : msg_(m) {}
    const detail::DartMsg* msg_;
    friend class Node;
};

/* Event: a peer / message-loss / error notification. Everything that goes wrong arrives
   as kind() == EventKind::Error with error() set; to_string() formats any of them. */
class Event {
public:
    EventKind        kind()           const { return static_cast<EventKind>(ev_->kind); }
    ErrorKind        error()          const { return static_cast<ErrorKind>(ev_->error); }
    bool             is_error()       const { return ev_->kind == detail::DART_ERROR; }
    uint32_t         peer()           const { return ev_->peer; }
    uint16_t         channel()        const { return ev_->channel; }
    /* our channel name for channel-scoped events, else empty */
    std::string_view channel_name()   const { return ev_->channel_name ? std::string_view(ev_->channel_name) : std::string_view{}; }
    int              os_error()       const { return ev_->os_error; }
    uint64_t         lost_first()     const { return ev_->lost_first; }
    uint64_t         lost_count()     const { return ev_->lost_count; }
    uint64_t         too_big_bytes()  const { return ev_->too_big_bytes; }
    uint64_t         identity()       const { return ev_->identity; }
    uint16_t         publish_topics() const { return ev_->publish_topics; }
    uint16_t         receive_topics() const { return ev_->receive_topics; }

    /* A one-line human-readable rendering (uses the C formatter). */
    std::string to_string() const { char b[192]; return detail::dart_event_str(ev_, b, sizeof b); }

private:
    explicit Event(const detail::DartEvent* e) : ev_(e) {}
    const detail::DartEvent* ev_;
    friend class Node;
};

/* Peer / PeerTopic: a copied snapshot of a discovered peer (safe after poll). */
struct PeerTopic {
    std::string name;
    bool        is_publisher;
    bool        reliable;
};
struct Peer {
    uint32_t               id = 0;
    std::string            name;
    std::string            address;         /* "1.2.3.4:port" */
    bool                   active = false;
    uint16_t               fragment_size = 0;
    std::vector<PeerTopic> topics;
};

/* Channel: a lightweight handle (owned by the Node, stable for its life). */
class Channel {
public:
    Channel() = default;
    bool valid() const noexcept { return ch_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    SendStatus send(Bytes data) {
        if (!ch_) return SendStatus::NoChannel;
        return static_cast<SendStatus>(
            detail::dart_channel_send(ch_, detail::dart_bytes(data.data(), data.size())));
    }
    void set_role(Role r) {
        if (!ch_) return;
        detail::dart_channel_set_role(ch_, static_cast<detail::DartRole>(r));
    }
    uint16_t index() const {
        if (!ch_) return 0xffff;
        return detail::dart_channel_index(ch_);
    }
    int match_count() const {
        if (!ch_) return 0;
        return detail::dart_channel_match_count(ch_);
    }
    /* Pump until every reader has acked, or timeout_ms elapses. Call before
     * closing so a final burst is not cut off by the BYE. */
    bool drain(int timeout_ms) {
        if (!ch_) return true;
        return detail::dart_channel_drain(ch_, timeout_ms) == 1;
    }

private:
    explicit Channel(detail::DartChannel* c) : ch_(c) {}
    detail::DartChannel* ch_ = nullptr;
    friend class Node;
};

/* Node: owns the DartNode, its memory, and the user callbacks.
 *
 * Thread-safe: every call is serialized by a node-level lock inside the C core.
 * Drive it either by calling poll() from your own loop, or via start(): a C-level
 * background service thread owns the loop and handlers then fire on it (never two
 * at once for one node). From inside a handler, send() and read-only queries are
 * allowed; poll/create_channel/set_role/drain/stop are refused (SendStatus::State
 * / no-op), never corrupting. */
class Node {
public:
    using MessageHandler = std::function<void(const MessageIn&)>;
    using EventHandler   = std::function<void(const Event&)>;

    /* Open a node. name = a human-readable label synced via discovery (empty =>
     * an auto "node-XXXXXXXX"). on_message/on_event are required (pass {} / nullptr
     * for either if truly not needed) so they are wired in before open() even
     * returns -- no early peer/error event is ever missed waiting for a deferred
     * on_message()/on_event() call; those below can still rebind them later.
     * Returns nullopt on failure. */
    static std::optional<Node> open(std::string_view name, MessageHandler on_message,
                                    EventHandler on_event, const NodeOptions& o = {}) {
        std::unique_ptr<Impl> impl(new Impl());
        impl->on_msg   = std::move(on_message);
        impl->on_event = std::move(on_event);
        /* The node retains the net string/seed pointers, so own that storage. */
        impl->disc_group = o.discovery_group;
        impl->mcast_if   = o.multicast_interface;
        for (const std::string& s : o.seed_peers) {
            detail::DartDiscoveryAddr a;
            if (parse_addr(s, a)) impl->seeds.push_back(a);
        }
        std::string nm(name);

        detail::DartNodeOpts co;
        std::memset(&co, 0, sizeof co);
        co.domain       = o.domain;
        co.max_channels = o.max_channels;
        co.disable_shm  = o.disable_shm ? 1 : 0;
        co.user_data    = impl.get();
        co.net.data_port           = o.data_port;
        co.net.discovery_group     = impl->disc_group.empty() ? nullptr : impl->disc_group.c_str();
        co.net.discovery_port      = o.discovery_port;
        co.net.multicast_interface = impl->mcast_if.empty()   ? nullptr : impl->mcast_if.c_str();
        co.net.multicast_ttl       = o.multicast_ttl;
        co.net.seed_peers          = impl->seeds.empty() ? nullptr : impl->seeds.data();
        co.net.n_seed_peers        = static_cast<uint16_t>(impl->seeds.size());
        co.net.fragment_size       = o.fragment_size;
        co.discovery.announce_interval_us = o.announce_interval_us;
        co.discovery.peer_timeout_us      = o.peer_timeout_us;
        co.discovery.max_peers            = o.max_peers;

        detail::DartAllocator mem = (o.memory && o.memory_size)
            ? detail::dart_allocator_static(o.memory, o.memory_size)
            : detail::dart_allocator_dynamic(detail::i_dart_plat_realloc, 0);
        detail::DartNode* n = detail::dart_node_open(
            &mem, nm.empty() ? nullptr : nm.c_str(),
            &Node::on_msg_tramp, &Node::on_evt_tramp, &co);
        if (!n) return std::nullopt;
        impl->node = n;
        return Node(std::move(impl));
    }

    /* Why the most recent open() returned std::nullopt (there is no Node to query on
       failure): the formatted one-line reason, and its machine-readable ErrorKind. */
    static std::string last_open_error() {
        detail::DartEvent e = detail::dart_last_error(nullptr);
        char b[192]; return detail::dart_event_str(&e, b, sizeof b);
    }
    static ErrorKind last_open_error_kind() {
        return static_cast<ErrorKind>(detail::dart_last_error(nullptr).error);
    }

    Node(Node&&) noexcept = default;
    Node& operator=(Node&&) noexcept = default;
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
    ~Node() = default;   /* teardown lives in Impl::~Impl (so a move-assign tears down correctly too) */

    /* Rebind a handler set at open(). Rarely needed: open() already requires initial ones. */
    Node& on_message(MessageHandler h) { impl_->on_msg = std::move(h);   return *this; }
    Node& on_event  (EventHandler   h) { impl_->on_event = std::move(h); return *this; }

    /* Create a topic. schema = an optional typed schema (its bytes are copied
     * into the node, so the Schema need not outlive the channel). */
    Channel create_channel(std::string_view name, Role role = Role::PubSub,
                           const Schema* schema = nullptr, const Qos& qos = {}) {
        std::string nm(name);
        detail::DartChannelOpts co;
        std::memset(&co, 0, sizeof co);
        co.qos = to_c(qos);
        detail::DartChannel* ch = detail::dart_node_create_channel(
            impl_->node, nm.c_str(), static_cast<detail::DartRole>(role),
            schema ? schema->raw() : nullptr, &co);
        return Channel(ch);
    }

    /* Recover an already-created channel handle by its creation index. */
    Channel channel(uint16_t index) const {
        return Channel(detail::dart_node_channel(impl_->node, index));
    }

    /* One loop tick: drives discovery, RX, timers, and flushes queued TX. Blocks
     * up to timeout_ms in the socket wait (wakes early on RX or a send from another
     * thread; 0 = non-blocking). Not needed (and refused) while start() runs. */
    int poll(int timeout_ms = 0) {
        return detail::dart_node_poll(impl_->node, timeout_ms);
    }

    /* Run the C-level background service thread: it owns the loop and fires the
     * handlers; every Node/Channel call stays safe from any thread, and a send is
     * flushed immediately (a waker cuts the service's socket wait short). */
    bool start() { return detail::dart_node_start(impl_->node) == 0; }
    /* Stop and join the service thread (idempotent; implied by node teardown). */
    void stop()  { detail::dart_node_stop(impl_->node); }
    bool is_started() const { return detail::dart_node_is_started(impl_->node) == 1; }

    /* A copied snapshot of the live peer table (safe to keep after the poll). */
    std::vector<Peer> peers() const {
        std::vector<Peer> out;
        uint16_t count = 0;
        /* the C view is zero-copy: hold the node lock across the copy so a poller
           on another thread cannot mutate it mid-read (no-op if we are that thread).
           RAII so a bad_alloc mid-copy cannot leak the lock and stall the node. */
        struct LockGuard {
            detail::DartNode* n;
            explicit LockGuard(detail::DartNode* node) : n(node) { detail::dart_node_lock(n); }
            ~LockGuard() { detail::dart_node_unlock(n); }
        } guard(impl_->node);
        const detail::DartDiscoveryPeer* ps = detail::dart_node_peers(impl_->node, &count);
        for (uint16_t i = 0; ps && i < count; i++) {
            const detail::DartDiscoveryPeer& p = ps[i];
            Peer peer;
            peer.id            = p.id;
            peer.name          = p.name.data ? std::string(p.name.data, p.name.len) : std::string();
            peer.address       = addr_string(p.addr);
            peer.active        = (p.liveness == detail::DART_PEER_ACTIVE);
            peer.fragment_size = detail::dart_node_peer_frag(&p);
            detail::DartInterestIter it;
            std::memset(&it, 0, sizeof it);
            detail::DartTopic t;
            while (detail::dart_node_peer_interest_next(&p, &it, &t))
                peer.topics.push_back({ std::string(t.name.data, t.name.len), t.is_pub != 0, t.reliable != 0 });
            out.push_back(std::move(peer));
        }
        return out;
    }

    struct MemoryStats { size_t in_use = 0, peak = 0; uint64_t alloc_calls = 0; };
    MemoryStats memory_stats() const {
        MemoryStats s;
        detail::dart_node_mem_stats(impl_->node, &s.in_use, &s.peak, &s.alloc_calls);
        return s;
    }
    struct BackpressureStats { uint64_t waited_us = 0; uint32_t waited_sends = 0; };
    BackpressureStats backpressure_stats() const {
        BackpressureStats b;
        detail::dart_node_backpressure_stats(impl_->node, &b.waited_us, &b.waited_sends);
        return b;
    }
    /* Sends that evicted never-sent history after the bounded wait (the
     * ErrorKind::EvictedUnsent count): the send-burst/overload indicator. */
    uint32_t evicted_unsent() const { return detail::dart_node_evicted_unsent(impl_->node); }

    /* The most recent error this node reported (also delivered via on_event): the
     * formatted one-line message, and its machine-readable ErrorKind. */
    std::string last_error() const {
        detail::DartEvent e = detail::dart_last_error(impl_->node);
        char b[192]; return detail::dart_event_str(&e, b, sizeof b);
    }
    ErrorKind last_error_kind() const {
        return static_cast<ErrorKind>(detail::dart_last_error(impl_->node).error);
    }

private:
    struct Impl {
        detail::DartNode*        node = nullptr;
        MessageHandler           on_msg;
        EventHandler             on_event;
        std::string              disc_group;
        std::string              mcast_if;
        std::vector<detail::DartDiscoveryAddr> seeds;

        ~Impl() {
            if (node) detail::dart_node_close(node, /*send_bye=*/1);
        }
    };
    std::unique_ptr<Impl> impl_;

    explicit Node(std::unique_ptr<Impl> i) : impl_(std::move(i)) {}

    static void on_msg_tramp(const detail::DartMsg* m) {
        Impl* impl = static_cast<Impl*>(m->user);
        if (impl && impl->on_msg) { MessageIn msg(m); impl->on_msg(msg); }
    }
    static void on_evt_tramp(const detail::DartEvent* e) {
        Impl* impl = static_cast<Impl*>(e->user);
        if (impl && impl->on_event) { Event ev(e); impl->on_event(ev); }
    }

    static detail::DartQos to_c(const Qos& q) {
        detail::DartQos c;
        std::memset(&c, 0, sizeof c);
        c.reliability          = static_cast<detail::DartReliability>(q.reliability);
        c.keep_last            = q.keep_last;
        c.catch_up             = q.catch_up;
        c.max_message_bytes    = q.max_message_bytes;
        c.heartbeat_us         = q.heartbeat_us;
        c.repair_delay_us      = q.repair_delay_us;
        c.backpressure_wait_us = q.backpressure_wait_us;
        c.shm_max_bytes        = q.shm_max_bytes;
        return c;
    }

    static std::string addr_string(const detail::DartDiscoveryAddr& a) {
        char b[64];
        if (a.ip_len == 4)
            std::snprintf(b, sizeof b, "%u.%u.%u.%u:%u", a.ip[0], a.ip[1], a.ip[2], a.ip[3], a.port);
        else
            std::snprintf(b, sizeof b, "[?]:%u", a.port);
        return b;
    }

    /* Parse "ip" or "ip:port" (IPv4) into a locator; port 0 = discovery_port.
     * Hand-rolled so no locale/CRT scanf (and its MSVC deprecation) is needed. */
    static bool parse_addr(const std::string& s, detail::DartDiscoveryAddr& out) {
        unsigned oct[4] = {0}, port = 0;
        size_t i = 0, n = s.size();
        for (int part = 0; part < 4; ++part) {
            if (i >= n || s[i] < '0' || s[i] > '9') return false;
            unsigned v = 0, digits = 0;
            while (i < n && s[i] >= '0' && s[i] <= '9') { v = v * 10 + unsigned(s[i++] - '0'); if (++digits > 3 || v > 255) return false; }
            oct[part] = v;
            if (part < 3) { if (i >= n || s[i] != '.') return false; ++i; }
        }
        if (i < n) {                       /* optional ":port" */
            if (s[i] != ':') return false;
            ++i;
            if (i >= n) return false;
            while (i < n && s[i] >= '0' && s[i] <= '9') { port = port * 10 + unsigned(s[i++] - '0'); if (port > 65535) return false; }
            if (i != n) return false;       /* trailing garbage */
        }
        std::memset(&out, 0, sizeof out);
        out.ip[0] = (uint8_t)oct[0]; out.ip[1] = (uint8_t)oct[1];
        out.ip[2] = (uint8_t)oct[2]; out.ip[3] = (uint8_t)oct[3];
        out.ip_len = 4; out.port = (uint16_t)port;
        return true;
    }
};

}   /* namespace dart */

#endif /* C++ consumer (not the implementation anchor) */
#endif /* DART_HPP_INCLUDED */
