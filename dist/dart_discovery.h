/* GENERATED single-header build. DO NOT EDIT.
 * DART = Discovery And Realtime Transport. Amalgamated from src/ by the CMake
 * build (tools/pack.cmake). Edit the split sources in src/ and rebuild (or run
 * tools/pack.cmake) to regenerate. See the flag scheme in tools/pack.cmake.
 */
#if defined(__GNUC__)   /* let the section markers below fold quietly */
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
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
