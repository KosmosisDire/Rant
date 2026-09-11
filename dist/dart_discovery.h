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
#pragma region discovery/core.h
/* The sans-IO discovery core. Feed it datagrams and the clock, it returns datagrams to
 * send and fires peer events. The rules are in spec/discovery.md. */
#ifndef DART_DISCOVERY_H
#define DART_DISCOVERY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DART_DISCOVERY_PROTO_VERSION
#define DART_DISCOVERY_PROTO_VERSION 5
#endif

#define DART_DISCOVERY_META_MAX 64   /* default per peer overlay capacity */
#define DART_DISCOVERY_NAME_MAX 32   /* advertised peer name bytes */
/* The fixed header is 24 bytes, then [u32 meta_version][u16 meta_len][meta]. */
#define DART_DISCOVERY_META_OFF 30
/* Largest discovery section of the blob: port, self ip and name. The overlay follows. */
#define DART_DISCOVERY_DISC_MAX (2u + 1u + 16u + 1u + DART_DISCOVERY_NAME_MAX)
/* Smallest datagram buffer. The runtime grows it to fit meta_cap. */
#define DART_DISCOVERY_WIRE_MAX 128

typedef struct {
    uint8_t  ip[16];   /* network order bytes */
    uint8_t  ip_len;   /* 4 or 16 */
    uint16_t port;     /* data port, host order */
} DartDiscoveryAddr;

/* Which local socket a datagram arrived on. A NAT may rewrite a peer's source per flow,
 * so the core keeps one observed source per channel. See spec/discovery.md. */
typedef enum {
    DART_DISCOVERY_VIA_DISCOVERY = 0,   /* the shared discovery port */
    DART_DISCOVERY_VIA_DATA      = 1    /* the port we advertise as our locator */
} DartDiscoveryVia;

#define DART_DISCOVERY_MAX_SUBNETS 16
/* One IPv4 subnet this host is on, network order bytes. */
typedef struct { uint8_t ip[4], mask[4]; } DartDiscoverySubnet;

/* Why a peer is going down. A DROP keeps transport state for a same uuid return. */
typedef enum {
    DART_DISCOVERY_DROP = 0,  /* silent past peer_timeout_us, state kept */
    DART_DISCOVERY_GONE = 1   /* said BYE, silent past the gone timeout, or evicted: free state */
} DartDiscoveryDownReason;

/* Discovery is generic and carries an opaque blob, so it has its own event type. The
 * node translates these into its DartEvent. */
typedef enum {
    DART_DISCOVERY_PEER_UP,        /* first contact, an address or blob change, or a resume */
    DART_DISCOVERY_PEER_DOWN,      /* .reason says DROP or GONE */
    DART_DISCOVERY_PEER_REFUSED,   /* table full of active peers, the newcomer was refused */
    DART_DISCOVERY_META_TOO_BIG    /* a peer's blob exceeds meta_cap or the receive buffer */
} DartDiscoveryEventKind;

typedef struct {
    DartDiscoveryEventKind   kind;
    void                    *user;     /* DartDiscoveryCoreConfig.user */
    uint32_t                 peer;     /* local peer id, 0 in META_TOO_BIG for an unknown peer */
    DartDiscoveryAddr        addr;     /* UP and REFUSED: the locator. TOO_BIG: locator or source */
    DartDiscoveryDownReason  reason;   /* DOWN only */
    DartString               name;     /* UP: the advertised name, {NULL,0} if none */
    DartBytes                meta;     /* UP: the overlay. TOO_BIG: .len is the wanted size */
} DartDiscoveryEvent;
typedef void (*DartDiscoveryEventFn)(const DartDiscoveryEvent *ev);

/* Liveness for the read only peer view. */
typedef enum {
    DART_PEER_ACTIVE  = 0,   /* heard within peer_timeout_us */
    DART_PEER_DROPPED = 1     /* silent, state kept, the same uuid may return */
} DartPeerLiveness;

/* The public face of one peer. The pointers are into discovery state, valid until the
 * next poll. The overlay is opaque here and decoded by the transport codec. */
typedef struct {
    uint32_t          id;            /* local handle, stable across a drop and return */
    uint8_t           uuid[16];
    DartDiscoveryAddr addr;          /* advertised unicast locator */
    DartPeerLiveness  liveness;
    uint64_t          last_heard_us;
    DartString        name;          /* {NULL,0} if none */
    DartBytes         meta;          /* the overlay, {NULL,0} if none */
    uint32_t          meta_version;  /* version of the overlay we hold */
    uint32_t          adv_meta_version; /* the highest version it advertises, above ours = stale */
    void             *user;          /* the peer's user scratch, or NULL */
} DartDiscoveryPeer;

typedef struct {
    uint8_t  uuid[16];      /* unique per process instance */
    uint16_t domain_id;
    uint16_t data_port;     /* unicast port we advertise */
    uint8_t  self_ip[16];   /* optional stated IP. len 0 means peers use the source address */
    uint8_t  self_ip_len;   /* 0, 4 or 16 */
    uint32_t announce_interval_us;
    uint32_t peer_timeout_us;    /* drop a peer after this much silence */
    uint32_t gone_timeout_us;    /* DROPPED to GONE this long after the drop, 0 = never */
    uint16_t max_peers;
    DartString name;        /* advertised name, copied at init */
    DartBytes meta;         /* the initial overlay. Must stay valid, .len <= meta_cap */
    uint16_t meta_cap;      /* per peer overlay capacity, 0 = DART_DISCOVERY_META_MAX */
    uint16_t peer_user_bytes;    /* scratch reserved per peer, 0 = none */
    uint8_t  relay_me;      /* mark our announces relay me, for a node with no multicast */
    DartDiscoveryEventFn on_event;
    void *user;
    /* Optional hook. Per peer blobs are then allocated at their actual length instead of
     * a fixed max_peers x meta_cap pool. Pair with dart_discovery_destroy. */
    DartAllocFn alloc;
    void       *alloc_user;
} DartDiscoveryCoreConfig;

typedef struct DartDiscoveryState DartDiscoveryState;

/* Fills every zero timing and size field with its default. Idempotent. */
DART_API void         dart_discovery_config_defaults(DartDiscoveryCoreConfig *cfg);

/* Bytes one datagram scratch buffer needs for meta_cap (0 = the default). */
DART_API uint32_t     dart_discovery_wire_size(uint16_t meta_cap);

DART_API size_t       dart_discovery_required_memory(const DartDiscoveryCoreConfig *cfg);
DART_API DartDiscoveryState *dart_discovery_init(void *mem, size_t mem_size, const DartDiscoveryCoreConfig *cfg);
/* Frees the hook allocated peer blobs. A no op without a hook. The arena stays the caller's. */
DART_API void         dart_discovery_destroy(DartDiscoveryState *st);
/* Relocates a live core into a bigger block, keeping the uuid, versions, ids and peers.
 * self_meta is the announce blob's new address. The caller frees the old block after. */
DART_API DartDiscoveryState *dart_discovery_core_migrate(DartDiscoveryState *old, void *new_mem,
                 size_t new_cap, uint16_t new_max_peers, uint16_t new_meta_cap,
                 const uint8_t *self_meta, void *peer_cb_user);
/* src NULL or port 0 means the IO layer cannot say, so the locator comes from the blob
 * alone and no observed source binds. via says which local socket it arrived on. */
DART_API void         dart_discovery_on_datagram(DartDiscoveryState *st, const DartDiscoveryAddr *src,
                                        DartDiscoveryVia via, DartBytes datagram, uint64_t now_us);
DART_API size_t       dart_discovery_update(DartDiscoveryState *st, uint64_t now_us, void *out, size_t cap);
/* When update next wants to run its timers. 0 means now. Peer timeout sweeps ride the
 * announce cadence. */
DART_API uint64_t     dart_discovery_next_due_us(const DartDiscoveryState *st);
DART_API size_t       dart_discovery_leave(DartDiscoveryState *st, void *out, size_t cap);
/* Queues a one shot solicit. The next update asks peers to announce now. */
DART_API void         dart_discovery_solicit(DartDiscoveryState *st);
/* Re fires peer up for every live peer with the blob we hold, so a caller that changed
 * its own advertised data re applies every peer's interest. */
DART_API void         dart_discovery_replay_peers(DartDiscoveryState *st);
/* Replaces the overlay and bumps its version so peers re fetch it. meta must stay valid. */
DART_API void         dart_discovery_set_meta(DartDiscoveryState *st, DartBytes meta);
/* The version our announces advertise, 0 if none. Detail responses are stamped with it. */
DART_API uint32_t     dart_discovery_meta_version(const DartDiscoveryState *st);
/* Sets the advertised locator port. 0 = none, peers then use the discovery port. */
DART_API void         dart_discovery_set_data_port(DartDiscoveryState *st, uint16_t port);
/* Our own subnets, for locator ranking. Re callable when the interface set changes. A
 * zero mask is ignored. */
DART_API void         dart_discovery_set_local_subnets(DartDiscoveryState *st,
                                      const DartDiscoverySubnet *nets, uint8_t n);
/* Drains one unicast datagram: a solicit reply or a re fetch request. Returns bytes and
 * fills *to, or 0. *exact 1 means *to is an observed source, send exactly there. */
DART_API size_t       dart_discovery_poll_targeted(DartDiscoveryState *st, void *out, size_t cap,
                                      DartDiscoveryAddr *to, int *exact);
/* Drains one proxied announce built on behalf of a relay me peer. The runtime sends it
 * on every path. Loop until 0. */
DART_API size_t       dart_discovery_poll_relay(DartDiscoveryState *st, void *out, size_t cap);
/* Drains one introduction: a proxied announce of a peer we hear directly, addressed to
 * one relay me peer. *to and *exact as in poll_targeted. Loop until 0. */
DART_API size_t       dart_discovery_poll_introduce(DartDiscoveryState *st, void *out, size_t cap,
                                      DartDiscoveryAddr *to, int *exact);
/* Live peers, DROPPED entries excluded. */
DART_API uint16_t     dart_discovery_peer_count(const DartDiscoveryState *st);
/* Table capacity, the slot range for peer_addr and peer_at. */
DART_API uint16_t     dart_discovery_max_peers(const DartDiscoveryState *st);
/* Discovery TX destination for the ACTIVE peer in a slot. 0 none, 1 the locator, which
 * the runtime expands to both ports, 2 an observed source, send exactly there. */
DART_API int          dart_discovery_peer_addr(const DartDiscoveryState *st, uint16_t slot,
                                      DartDiscoveryAddr *out);
/* Read only view of the peer in a slot, ACTIVE or DROPPED. 1 if it holds one. */
DART_API int          dart_discovery_peer_at(const DartDiscoveryState *st, uint16_t slot,
                                      DartDiscoveryPeer *out);

/* By id lookups, DROPPED peers included. The node keys its state on the id and uses
 * these instead of a second peer table. */
DART_API void        *dart_discovery_peer_user(DartDiscoveryState *st, uint32_t id);
/* The overlay we hold and, through *version, the version it is at. A view until the next poll. */
DART_API DartBytes    dart_discovery_peer_meta(const DartDiscoveryState *st, uint32_t id,
                                      uint32_t *version);
/* The one address to send data to: the observed data source when bound, else the locator. */
DART_API int          dart_discovery_addr_of_id(const DartDiscoveryState *st, uint32_t id,
                                      DartDiscoveryAddr *out);
/* {NULL,0} for an unknown peer. */
DART_API DartString   dart_discovery_peer_name(const DartDiscoveryState *st, uint32_t id);
/* Maps a source back to a peer id. A peer with observed sources matches only those. */
DART_API int          dart_discovery_id_for_addr(const DartDiscoveryState *st, const uint8_t *ip,
                                      uint8_t ip_len, uint16_t port, uint32_t *id);
/* Deterministic RFC 9562 v8 uuid from a stable input plus a boot seed. Not cryptographic. */
DART_API void         dart_discovery_make_uuid(uint8_t out[16], DartBytes stable, uint64_t boot_seed);
/* Our own uuid, a view valid for the state's lifetime. */
DART_API const uint8_t *dart_discovery_uuid(const DartDiscoveryState *st);

#ifdef __cplusplus
}
#endif
#endif /* DART_DISCOVERY_H */
#pragma endregion

#ifndef DART_DISCOVERY_SANS_IO
#pragma region platform/core.h
/* The one OS layer. A runtime speaks only these functions and never a sockaddr or an OS
 * ifdef. The contract and the address rules are in spec/platform.md. */
#ifndef DART_PLAT_H
#define DART_PLAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DART_SHM is auto detected where the bundled layer has it and DART_NO_SHM always wins.
 * This block mirrors transport/core.h exactly. Edit both together. */
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

/* DART_THREADS follows the same shape. A new platform layer that implements the thread
 * contract below defines it itself. */
#if !defined(DART_THREADS) && !defined(DART_NO_THREADS)
  #if defined(_WIN32) || defined(__linux__) || defined(__APPLE__) || \
      defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
      defined(__DragonFly__) || defined(ESP_PLATFORM)
    #define DART_THREADS
  #elif defined(__has_include)
    #if __has_include(<pthread.h>)
      #define DART_THREADS   /* unknown POSIX with pthreads */
    #endif
  #endif
#endif
#if defined(DART_THREADS) && defined(DART_NO_THREADS)
  #undef DART_THREADS        /* both set, the opt out wins */
#endif

/* A POSIX fd or a Windows SOCKET, both fit in intptr_t. */
typedef intptr_t i_DartSock;
#define DART_SOCK_BAD ((i_DartSock)-1)

/* Mirrors struct pollfd without the OS header. */
#define DART_POLLIN 0x01
typedef struct { i_DartSock fd; short events; short revents; } i_DartPollfd;

/* Process wide net init and teardown. The OS refcounts matched calls, so pairs nest safely. */
int  i_dart_plat_startup(void);   /* 1 on success */
void i_dart_plat_cleanup(void);

/* Monotonic microseconds from an arbitrary epoch. */
uint64_t i_dart_plat_now_us(void);

/* CSPRNG fill. 0 means no entropy source and the caller falls back. */
int      i_dart_plat_random(void *buf, size_t len);
/* Host identity for the uuid fallback. Returns the bytes written. */
size_t   i_dart_plat_hostname(char *buf, size_t cap);
uint64_t i_dart_plat_pid(void);
/* Wall clock microseconds since the Unix epoch, for timestamps compared across hosts. */
uint64_t i_dart_plat_wall_us(void);

/* DART_PROC_STATS follows the same shape. When off the two functions below are absent and
 * every consumer compiles out with them, so a layer that cannot measure implements nothing. */
#if !defined(DART_PROC_STATS) && !defined(DART_NO_PROC_STATS)
  #if defined(_WIN32) || defined(__linux__) || defined(__APPLE__) || \
      defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
      defined(__DragonFly__) || defined(ESP_PLATFORM)
    #define DART_PROC_STATS
  #endif
#endif
#if defined(DART_PROC_STATS) && defined(DART_NO_PROC_STATS)
  #undef DART_PROC_STATS       /* both set, the opt out wins */
#endif
#ifdef DART_PROC_STATS
/* Per process, not per node. Any out pointer may be NULL. 0 on a transient OS failure. */
int      i_dart_plat_proc_stats(uint64_t *cpu_us, uint64_t *rss_bytes, uint64_t *peak_rss_bytes,
                                int *have_cpu);
/* Heap diagnostics. Only ESP has them, the others return 0 so callers omit the fields. */
int      i_dart_plat_heap_stats(uint64_t *total_bytes, uint64_t *free_bytes,
                                uint64_t *min_free_bytes, uint64_t *largest_free_block_bytes);
#endif /* DART_PROC_STATS */

/* The one heap dependency. ptr NULL allocates, size 0 frees and returns NULL. */
void    *i_dart_plat_realloc(void *ptr, size_t size);

/* UDP sockets */
i_DartSock i_dart_plat_udp_open(void);                   /* DART_SOCK_BAD on failure */
void      i_dart_plat_close(i_DartSock s);
/* if_naddr 0 is INADDR_ANY and port 0 is ephemeral. reuse sets SO_REUSEADDR, and
 * SO_REUSEPORT where it exists, before binding. 1 ok, 0 fail. */
int       i_dart_plat_bind(i_DartSock s, uint32_t if_naddr, uint16_t port, int reuse);
/* Bound port in host order, 0 on failure. */
uint16_t  i_dart_plat_local_port(i_DartSock s);
void      i_dart_plat_set_nonblock(i_DartSock s);
void      i_dart_plat_set_rcvbuf(i_DartSock s, int bytes);
void      i_dart_plat_set_sndbuf(i_DartSock s, int bytes);
/* Stops an ICMP port unreachable from failing the next recv. Windows only, no op elsewhere. */
void      i_dart_plat_suppress_connreset(i_DartSock s);

/* multicast */
void i_dart_plat_mcast_setif(i_DartSock s, uint32_t if_naddr);
void i_dart_plat_mcast_ttl  (i_DartSock s, uint8_t ttl);
void i_dart_plat_mcast_loop (i_DartSock s, int on);
int  i_dart_plat_mcast_join (i_DartSock s, uint32_t group_naddr, uint32_t if_naddr); /* 1 ok */
void i_dart_plat_mcast_leave(i_DartSock s, uint32_t group_naddr, uint32_t if_naddr);

/* datagram IO */
/* Bytes sent, or negative on error. Test i_dart_plat_would_block after a failure. */
int  i_dart_plat_send(i_DartSock s, const void *buf, size_t len,
                    const uint8_t ip[4], uint16_t port);
/* Bytes received, or 0 or negative when none. src_ip and src_port may be NULL. */
int  i_dart_plat_recv(i_DartSock s, void *buf, size_t cap,
                    uint8_t src_ip[4], uint16_t *src_port);
int  i_dart_plat_would_block(void);
/* Positive when ready, 0 on timeout, negative on error. */
int  i_dart_plat_poll(i_DartPollfd *fds, int n, int timeout_ms);
/* The OS's last socket error on the calling thread, for diagnostics. Keeps no state. */
int  i_dart_plat_last_socket_error(void);

/* address helpers, a naddr is network byte order */
uint32_t i_dart_plat_parse_ip(const char *dotted);          /* "1.2.3.4" to naddr */
uint32_t i_dart_plat_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
uint32_t i_dart_plat_ip4_to_naddr(const uint8_t ip[4]);
void     i_dart_plat_naddr_to_ip4(uint32_t naddr, uint8_t out[4]);
/* The source address the OS would use toward dst, with no packet sent. A diagnostic only. */
uint32_t i_dart_plat_route_src(uint32_t dst_naddr, uint16_t port);

/* One IPv4 interface, address and netmask in network order. */
typedef struct { uint32_t addr, mask; } i_DartIface;
/* The up, non loopback interfaces. A netmask the platform cannot report stays 0. */
int      i_dart_plat_local_ifaces(i_DartIface *out, int max);

/* threads, only under DART_THREADS */
#ifdef DART_THREADS
/* Opaque blobs keep OS headers out of this header. core.c checks the real types fit. */
typedef union { void *align_p; uint64_t align_8; unsigned char b[64]; } i_DartMutex;
typedef union { void *align_p; uint64_t align_8; unsigned char b[64]; } i_DartCond;
typedef union { void *align_p; uint64_t align_8; unsigned char b[32]; } i_DartThread;

/* start runs fn(arg) on a new thread, 1 on success. join blocks until fn returns. */
int      i_dart_plat_thread_start(i_DartThread *t, void (*fn)(void *), void *arg);
void     i_dart_plat_thread_join (i_DartThread *t);
/* Nonzero id of the calling thread, compared and never dereferenced. */
uint64_t i_dart_plat_thread_id   (void);

void i_dart_plat_mutex_init   (i_DartMutex *m);
void i_dart_plat_mutex_destroy(i_DartMutex *m);
void i_dart_plat_mutex_lock   (i_DartMutex *m);
void i_dart_plat_mutex_unlock (i_DartMutex *m);

void i_dart_plat_cond_init     (i_DartCond *c);
void i_dart_plat_cond_destroy  (i_DartCond *c);
/* Waits with m held and holds it again on return. May wake early or spuriously, so
 * callers loop on a predicate plus a monotonic deadline. */
void i_dart_plat_cond_wait     (i_DartCond *c, i_DartMutex *m, uint32_t timeout_us);
void i_dart_plat_cond_broadcast(i_DartCond *c);

/* A self connected loopback UDP socket that sits in a poll set so another thread can cut
 * a wait short. Signals coalesce. */
typedef struct { i_DartSock fd; } i_DartWaker;
int  i_dart_plat_waker_open  (i_DartWaker *w);   /* 1 on success */
int  i_dart_plat_waker_signal(i_DartWaker *w);   /* 1 when the signal went out */
void i_dart_plat_waker_drain (i_DartWaker *w);
void i_dart_plat_waker_close (i_DartWaker *w);
#endif /* DART_THREADS */

/* shared memory, only under DART_SHM */
#ifdef DART_SHM
/* create maps a fresh zero filled segment. attach maps an existing one whole and reports
 * its size, which detach needs. Both return the base, or NULL on failure. */
void *i_dart_plat_shm_create(const char *name, size_t bytes, void **handle);
void *i_dart_plat_shm_attach(const char *name, size_t *out_bytes, void **handle);
/* The creator passes unlink_it to also remove the OS object. */
void  i_dart_plat_shm_detach(void *base, size_t bytes, void *handle, int unlink_it);
/* A stable per host id for the same host pre check. A successful attach is the real gate. */
void  i_dart_plat_host_uuid(uint8_t out[16]);
/* Cross process 64 bit atomics with acquire and release order, for the chunk stamp. */
uint64_t i_dart_plat_atomic_load64 (volatile uint64_t *p);
void     i_dart_plat_atomic_store64(volatile uint64_t *p, uint64_t v);
#endif /* DART_SHM */

#ifdef __cplusplus
}
#endif
#endif /* DART_PLAT_H */
#pragma endregion
#pragma region discovery/runtime.h
/* The discovery runtime: sockets, clock and uuid over the core. dart_discovery_open owns
 * its memory. dart_discovery_place uses a caller buffer, which is how the node embeds it. */
#ifndef DART_DISCOVERY_RT_H
#define DART_DISCOVERY_RT_H


#ifdef __cplusplus
extern "C" {
#endif

#define DART_DISCOVERY_MAX_SEEDS 4

typedef struct DartDiscovery DartDiscovery;

/* Options for dart_discovery_open. Zero means default. The meta blob is opaque. */
typedef struct {
    uint16_t              domain;               /* default 0 */
    const char           *discovery_group;      /* default "239.255.0.7" */
    uint16_t              discovery_port;       /* default 7400 */
    const char           *multicast_interface;  /* pin to this interface IP, NULL = all */
    uint8_t               multicast_ttl;        /* default 1 */
    uint16_t              max_peers;            /* default 32 */
    DartDiscoveryEventFn  on_event;             /* optional */
    void                 *user;                 /* passed to on_event */
    const DartDiscoveryAddr *seed_peers;        /* unicast seeds for multicast filtered nets */
    uint16_t              n_seed_peers;
    uint8_t               unicast_only;         /* 1 = never touch multicast */
    DartBytes             meta;                 /* optional opaque overlay to advertise */
    uint16_t              meta_cap;        /* per peer incoming overlay buffer, 0 = default */
    uint16_t              peer_user_bytes;      /* scratch reserved per peer, 0 = none */
} DartDiscoveryConfig;

/* alloc is copied in and reset on close, so it may be a temporary. A NULL name is auto
 * generated and a NULL cfg means all defaults. NULL on failure, see dart_discovery_last_error. */
DART_API DartDiscovery   *dart_discovery_open(DartAllocator *alloc, const char *name, const DartDiscoveryConfig *cfg);

/* lifecycle */
/* One loop tick. 1 if a datagram arrived, 0 if idle, negative on a socket error. */
DART_API int        dart_discovery_poll(DartDiscovery *d, int timeout_ms);
/* Blocks: solicits, then pumps until the peer set is quiet for quiet_ms or timeout_ms
 * passes. Returns the peer count. */
DART_API int        dart_discovery_gather(DartDiscovery *d, int quiet_ms, int timeout_ms);
/* Optionally multicasts a BYE, then closes the sockets and frees owned memory. */
DART_API void       dart_discovery_close(DartDiscovery *d, int send_bye);

/* The live peer list by pointer, valid until the next poll. Each .user is writable in place. */
DART_API const DartDiscoveryPeer *dart_discovery_peers(DartDiscovery *d, uint16_t *count);

/* The sans-IO core, for the by id lookups. Valid for the runtime's life. */
DART_API DartDiscoveryState *dart_discovery_state(DartDiscovery *d);

/* advanced: placement open */
/* The placement config. Zero and NULL fields get defaults. A zero uuid is auto generated. */
typedef struct {
    DartDiscoveryCoreConfig discovery;        /* the core config */
    const char  *group;       /* default "239.255.0.7" */
    uint16_t     discovery_port;   /* default 7400 */
    uint8_t      ttl;         /* default 1 */
    const char  *multicast_interface;    /* this interface IP only, never rescanned. NULL = every
                                 interface, "127.0.0.1" = single host isolation */
    const DartDiscoveryAddr *seeds;  /* also unicast announces here, at most MAX_SEEDS */
    uint16_t     n_seeds;
    uint8_t      unicast_only;  /* 1 = no multicast at all. Implies discovery.relay_me. Seed at
                                 least one peer, or be seeded by one, or nothing can find us. */
} DartDiscoveryNetConfig;

DART_API size_t     dart_discovery_placement_memory(const DartDiscoveryNetConfig *cfg);
/* Places a runtime in caller memory. The caller owns mem. NULL on failure, and
 * dart_discovery_last_error names the step. */
DART_API DartDiscovery   *dart_discovery_place(void *mem, size_t mem_size, const DartDiscoveryNetConfig *cfg);

/* Why the last open or place returned NULL. A process global with no lock, read it right after. */
typedef enum {
    DART_DISCOVERY_OK = 0,
    DART_DISCOVERY_E_MEMORY,      /* the buffer was too small */
    DART_DISCOVERY_E_PLATFORM,    /* net startup failed */
    DART_DISCOVERY_E_SOCKET,      /* socket open failed */
    DART_DISCOVERY_E_BIND,        /* bind to the discovery port failed */
    DART_DISCOVERY_E_MCAST_JOIN   /* joining the group failed */
} DartDiscoveryPlaceError;
DART_API DartDiscoveryPlaceError dart_discovery_last_error(void);
/* The OS socket error captured with the last failure, 0 if none. */
DART_API int        dart_discovery_last_os_error(void);
/* Relocates a placed runtime into a bigger block, keeping the socket, uuid and peers.
 * self_meta is the new announce blob address. The caller frees the old block after. */
DART_API DartDiscovery   *dart_discovery_migrate(DartDiscovery *old, void *new_mem, size_t new_cap,
                 uint16_t new_max_peers, uint16_t new_meta_cap, const uint8_t *self_meta, void *peer_cb_user);

/* node integration */
/* A discovery datagram that arrived on the data socket. src carries the port too, so the
 * core can bind an observed source. */
DART_API void       dart_discovery_feed(DartDiscovery *d, const DartDiscoveryAddr *src, DartBytes datagram);
/* Routes unicast discovery TX out of fd, the node's data socket. DART_SOCK_BAD restores
 * the own socket. Group TX stays on the own socket. */
DART_API void       dart_discovery_set_tx_fd(DartDiscovery *d, i_DartSock fd);
/* Replaces the overlay and bumps its version. meta must outlive the runtime. */
DART_API void       dart_discovery_advertise(DartDiscovery *d, DartBytes meta);
/* Re applies every peer's interest. Call after changing our own advertised meta. */
DART_API void       dart_discovery_replay(DartDiscovery *d);
/* The receive sockets, 1 or 2, for a caller with its own wait. Stable across a migrate. */
DART_API int        dart_discovery_pollfds(DartDiscovery *d, i_DartSock out[2]);
/* dart_discovery_poll without the wait. Pass each fd's readability in pollfds order. Call
 * it every pass, the clock driven work needs no readable fd. */
DART_API int        dart_discovery_service(DartDiscovery *d, int fd_readable, int unicast_readable);

/* uuid */
/* A random RFC 9562 v4 uuid. 0 if there is no entropy source. */
DART_API int        dart_discovery_make_uuid4(uint8_t out[16]);
/* The CSPRNG path, else a host identity fallback. Shared with the node's DartUuid. */
void       i_dart_discovery_auto_uuid(uint8_t out[16]);

/* Resolves an advertised name into out: want clamped, or an auto "node-XXXXXXXX" when
 * want is NULL or empty. Returns the length. */
DART_API uint8_t    dart_discovery_default_name(char *out, size_t cap, const char *want);

#ifdef __cplusplus
}
#endif
#endif /* DART_DISCOVERY_RT_H */
#pragma endregion
#endif /* !DART_DISCOVERY_SANS_IO */

#ifdef DART_DISCOVERY_IMPLEMENTATION
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
#pragma region discovery/core.c
/* The sans-IO discovery core. The rules are in spec/discovery.md. */
#include <string.h>

#define DART_DISCOVERY_HDR_LEN 24            /* magic(4) ver(1) flags(1) domain(2) uuid(16) */
#define DART_DISCOVERY_FLAG_BYE 0x01
#define DART_DISCOVERY_FLAG_REQ 0x02         /* solicit: recipients announce back now */
#define DART_DISCOVERY_FLAG_RELAY_ME 0x04    /* no multicast: direct hearers announce it on */
#define DART_DISCOVERY_FLAG_PROXIED  0x08    /* rebuilt by a relay: never re relayed */
#define DART_DISCOVERY_BLOB_RESEND 3u        /* announces that carry the full blob after a change */
#define DART_DISCOVERY_INTRODUCE_IDLE 0xFFFFu   /* no introduction walk in progress */
#define DART_DISCOVERY_INTRODUCE_SWEEP 10u      /* re introduce every this many intervals */

struct i_DartDiscoveryPeer {
    uint8_t  used;
    uint8_t  dropped;       /* silent past peer_timeout_us, state kept for a same uuid return */
    uint8_t  uuid[16];
    uint32_t local_id;
    uint8_t  ip[16];
    uint8_t  ip_len;
    uint16_t port;
    uint64_t last_heard_us;
    uint64_t last_direct_us; /* last non proxied announce. The relay gate, never last_heard_us */
    uint64_t addr_heard_us;  /* last datagram that arrived from ip. A fresh incumbent stays put */
    uint8_t *meta;          /* the overlay: a pool slot or a grown hook block */
    uint16_t meta_cap;
    uint16_t meta_len;
    uint32_t meta_version;  /* version of the blob we hold, 0 = none yet */
    uint32_t adv_version;   /* highest version advertised. Above meta_version means ours is stale */
    uint8_t *user;          /* consumer scratch, stride st->user_stride */
    char     name[DART_DISCOVERY_NAME_MAX + 1];
    uint8_t  name_len;
    uint8_t  reply_due;     /* owes a unicast announce plus blob, it solicited us */
    uint8_t  solicit_due;   /* owes a unicast REQ, its version is ahead of ours */
    uint8_t  wants_relay;   /* its direct announces carry RELAY_ME */
    uint8_t  relay_due;     /* a proxied announce for it is due out of poll_relay */
    uint8_t  proxies_heard; /* foreign proxies since our last tick. 2 or more suppress ours */
    uint8_t  relay_me;      /* a unicast only origin, so its locator is not an endpoint identity */
    uint8_t  stated_ip;     /* its blob states a self_ip, so observed sources never bind */
    uint8_t  heard_direct;  /* heard a non proxied announce since appearing: ours to introduce */
    /* Observed sources of its direct RELAY_ME announces, one per local channel. These, not
       the locator, are a NAT'd peer's return paths. ip_len 0 means none. */
    DartDiscoveryAddr obs_disc;
    DartDiscoveryAddr obs_data;
    uint16_t introduce_cursor;   /* next slot to introduce to this relay me peer, or IDLE */
    uint64_t introduce_sweep_us; /* next periodic re introduction */
};
typedef struct i_DartDiscoveryPeer i_DartDiscoveryPeer;

struct DartDiscoveryState {
    DartDiscoveryCoreConfig  cfg;
    uint64_t      next_announce_us;
    uint32_t      next_local_id;
    uint8_t       started;
    uint8_t       want_solicit;   /* a multicast solicit is queued for the next update */
    uint16_t      cap_peers;
    uint16_t      meta_cap;       /* per peer meta buffer capacity */
    uint8_t      *meta_pool;      /* cap_peers x meta_cap. NULL in hook mode */
    uint16_t      user_stride;         /* per peer user scratch bytes, 8 aligned, 0 = none */
    uint8_t      *user_pool;      /* cap_peers x user_stride */
    DartBytes     self_meta;        /* our overlay, a view of the node's buffer */
    uint32_t      self_meta_version;
    char          self_name[DART_DISCOVERY_NAME_MAX + 1];
    uint8_t       self_name_len;
    uint16_t      self_blob_resend; /* announces remaining that carry the full blob */
    uint16_t      targeted_cursor;  /* round robin over peers for poll_targeted */
    uint16_t      relay_cursor;     /* round robin over peers for poll_relay */
    DartDiscoverySubnet local_nets[DART_DISCOVERY_MAX_SUBNETS];
    uint8_t       n_local_nets;
    i_DartDiscoveryPeer  *peers;
};

/* The event builders. peer_up carries the parsed name and the overlay we hold. */
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
    out[6] = (uint8_t)((out[6] & 0x0Fu) | 0x80u);  /* version 8 */
    out[8] = (uint8_t)((out[8] & 0x3Fu) | 0x80u);  /* variant 10x */
}

const uint8_t *dart_discovery_uuid(const DartDiscoveryState *st){
    return st ? st->cfg.uuid : NULL;
}

static uint16_t i_dart_discovery_meta_cap(const DartDiscoveryCoreConfig *cfg){
    return cfg->meta_cap ? cfg->meta_cap : DART_DISCOVERY_META_MAX;
}

/* Rounded up to 8 so a consumer may store a pointer in its slot. */
static uint16_t i_dart_discovery_user_stride(const DartDiscoveryCoreConfig *cfg){
    return cfg->peer_user_bytes ? (uint16_t)((cfg->peer_user_bytes + 7u) & ~7u) : 0u;
}

void dart_discovery_config_defaults(DartDiscoveryCoreConfig *cfg){
    if (!cfg) return;
    if (cfg->announce_interval_us == 0) cfg->announce_interval_us = 3000000u;
    if (cfg->peer_timeout_us == 0)      cfg->peer_timeout_us = 3000000u * 4u;   /* 12 s */
    if (cfg->gone_timeout_us == 0)      cfg->gone_timeout_us = 60000000u * 2u;   /* 2 min */
    if (cfg->max_peers == 0)            cfg->max_peers = 32u;
}

uint32_t dart_discovery_wire_size(uint16_t meta_cap){
    uint32_t cap = meta_cap ? meta_cap : DART_DISCOVERY_META_MAX;
    /* the blob is the discovery section plus the overlay */
    uint32_t w = (uint32_t)DART_DISCOVERY_META_OFF + DART_DISCOVERY_DISC_MAX + cap;
    return w < DART_DISCOVERY_WIRE_MAX ? DART_DISCOVERY_WIRE_MAX : w;
}

/* The one arena layout. Measure mode feeds required_memory and build mode feeds init. */
typedef struct { DartDiscoveryState *st; uint8_t *peers, *meta_pool, *user_pool; } i_DartDiscoveryBlocks;
static void i_dart_discovery_layout(i_DartBump *b, const DartDiscoveryCoreConfig *cfg, i_DartDiscoveryBlocks *o){
    uint16_t meta_cap = i_dart_discovery_meta_cap(cfg);
    uint16_t user_stride   = i_dart_discovery_user_stride(cfg);
    o->st        = (DartDiscoveryState*)i_dart_bump_take(b, sizeof(struct DartDiscoveryState), 8);
    o->peers     = (uint8_t*)i_dart_bump_take(b, (size_t)cfg->max_peers * sizeof(i_DartDiscoveryPeer), 8);
    /* hook mode allocates each blob on demand, only the pool path reserves the worst case */
    o->meta_pool = cfg->alloc ? NULL
                 : (uint8_t*)i_dart_bump_take(b, (size_t)cfg->max_peers * meta_cap, 1);
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
    i_DartBump b; i_DartDiscoveryBlocks blk; DartDiscoveryState *st; uint16_t i, meta_cap;
    if (!mem || !cfg || cfg->max_peers == 0) return NULL;
    if (cfg->announce_interval_us == 0 || cfg->peer_timeout_us == 0) return NULL;
    meta_cap = i_dart_discovery_meta_cap(cfg);
    if (cfg->meta.len > meta_cap) return NULL;
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
    st->meta_cap      = meta_cap;
    st->peers         = (i_DartDiscoveryPeer *)blk.peers;
    st->meta_pool     = blk.meta_pool;
    st->user_stride   = i_dart_discovery_user_stride(cfg);
    st->user_pool     = blk.user_pool;
    st->next_local_id = 1;
    st->started       = 0;
    memset(st->peers, 0, (size_t)st->cap_peers * sizeof(i_DartDiscoveryPeer));
    if (st->user_pool) memset(st->user_pool, 0, (size_t)st->cap_peers * st->user_stride);
    for (i=0;i<st->cap_peers;i++){
        st->peers[i].meta     = st->meta_pool ? st->meta_pool + (size_t)i * meta_cap : NULL;
        st->peers[i].meta_cap = st->meta_pool ? meta_cap : 0;
        st->peers[i].user = st->user_pool ? st->user_pool + (size_t)i * st->user_stride : NULL;
        st->peers[i].introduce_cursor = DART_DISCOVERY_INTRODUCE_IDLE;
    }
    st->self_meta         = cfg->meta;
    st->self_meta_version = 1;
    st->self_blob_resend  = DART_DISCOVERY_BLOB_RESEND;
    {   uint8_t nl = cfg->name.len > DART_DISCOVERY_NAME_MAX ? DART_DISCOVERY_NAME_MAX : (uint8_t)cfg->name.len;
        if (cfg->name.data && nl) memcpy(st->self_name, cfg->name.data, nl);
        st->self_name[nl] = '\0'; st->self_name_len = nl; }
    return st;
}


/* Not a re init: the uuid, blob version, id counter and peer table survive, or peers
 * would treat us as a new node. self_meta is the node core's new blob address. */
DartDiscoveryState *dart_discovery_core_migrate(DartDiscoveryState *old, void *new_mem,
        size_t new_cap, uint16_t new_max_peers, uint16_t new_meta_cap,
        const uint8_t *self_meta, void *peer_cb_user){
    i_DartBump b; i_DartDiscoveryBlocks blk; DartDiscoveryState *st; DartDiscoveryCoreConfig dc; uint16_t i, omp;
    if (!old) return NULL;
    dc = old->cfg; dc.max_peers = new_max_peers; dc.meta_cap = new_meta_cap;
    if (new_cap < dart_discovery_required_memory(&dc)) return NULL;
    memset(&b,0,sizeof b);
    b.base = (uint8_t*)(((uintptr_t)new_mem + 7u) & ~(uintptr_t)7u);
    b.cap  = new_cap - (size_t)(b.base - (uint8_t*)new_mem);
    i_dart_discovery_layout(&b, &dc, &blk);
    st = blk.st;
    *st = *old;                            /* cfg, counters, version, cursors */
    st->cfg.max_peers     = new_max_peers;
    st->cfg.meta_cap = new_meta_cap;
    st->cfg.user          = peer_cb_user;  /* peer callbacks fire on the relocated node core */
    st->cap_peers         = new_max_peers;
    st->meta_cap     = new_meta_cap;
    st->peers             = (i_DartDiscoveryPeer*)blk.peers;
    st->meta_pool         = blk.meta_pool;
    st->user_pool         = blk.user_pool;
    st->self_meta.data    = self_meta;     /* the new blob address, version not bumped */
    memset(st->peers, 0, (size_t)new_max_peers * sizeof(i_DartDiscoveryPeer));
    for (i=0;i<new_max_peers;i++){
        st->peers[i].meta     = st->meta_pool ? st->meta_pool + (size_t)i * new_meta_cap : NULL;
        st->peers[i].meta_cap = st->meta_pool ? new_meta_cap : 0;
        st->peers[i].user = st->user_pool ? st->user_pool + (size_t)i * st->user_stride : NULL;
        st->peers[i].introduce_cursor = DART_DISCOVERY_INTRODUCE_IDLE;
    }
    if (st->user_pool) memset(st->user_pool, 0, (size_t)new_max_peers * st->user_stride);
    omp = old->cap_peers;
    for (i=0;i<omp;i++){
        uint8_t *nmeta = st->peers[i].meta, *nuser = st->peers[i].user;
        uint16_t ncap  = st->peers[i].meta_cap;
        st->peers[i] = old->peers[i];
        if (st->meta_pool){                /* a hook block came with the struct copy */
            st->peers[i].meta = nmeta; st->peers[i].meta_cap = ncap;
            if (old->peers[i].meta_len) memcpy(nmeta, old->peers[i].meta, old->peers[i].meta_len);
        }
        st->peers[i].user = nuser;
        if (st->user_stride && nuser && old->peers[i].user)
            memcpy(nuser, old->peers[i].user, st->user_stride);
    }
    return st;
}

void dart_discovery_destroy(DartDiscoveryState *st){
    uint16_t i;
    if (!st || !st->cfg.alloc) return;   /* pool mode has nothing hook allocated */
    for (i=0;i<st->cap_peers;i++)
        if (st->peers[i].meta){
            st->cfg.alloc(st->cfg.alloc_user, st->peers[i].meta, 0);
            st->peers[i].meta = NULL; st->peers[i].meta_cap = 0; st->peers[i].meta_len = 0;
        }
}

/* Includes DROPPED entries, so a same uuid return reuses the slot and the local_id. */
static int i_dart_discovery_find(DartDiscoveryState *st, const uint8_t *uuid){
    uint16_t i;
    for (i=0;i<st->cap_peers;i++)
        if (st->peers[i].used && memcmp(st->peers[i].uuid, uuid, 16)==0) return (int)i;
    return -1;
}

/* Fires GONE while the slot still exists, so the handler can read its scratch, then frees it. */
static void i_dart_discovery_peer_gone(DartDiscoveryState *st, i_DartDiscoveryPeer *peer){
    i_dart_discovery_fire_down(st, peer->local_id, DART_DISCOVERY_GONE);
    peer->used = 0;
}

/* A free slot, else the oldest DROPPED one evicted as GONE. Active peers are never evicted. */
static int i_dart_discovery_alloc(DartDiscoveryState *st){
    uint16_t i, victim = 0; uint64_t oldest = (uint64_t)-1; int found = -1;
    for (i=0;i<st->cap_peers;i++){
        if (!st->peers[i].used) return (int)i;
        if (st->peers[i].dropped && st->peers[i].last_heard_us <= oldest){
            oldest = st->peers[i].last_heard_us; victim = i; found = 1;
        }
    }
    if (found < 0) return -1;   /* table full of active peers: the caller refuses */
    i_dart_discovery_peer_gone(st, &st->peers[victim]);
    return (int)victim;
}

/* A new uuid from an (ip, port) we hold means that process restarted, so the old entry is
 * evicted as GONE. Port 0 is not an endpoint, and a relay me peer's locator is not either. */
static void i_dart_discovery_evict_endpoint(DartDiscoveryState *st, const DartDiscoveryAddr *addr){
    uint16_t i;
    if (!addr->ip_len || !addr->port) return;
    for (i=0;i<st->cap_peers;i++){
        i_DartDiscoveryPeer *peer = &st->peers[i];
        if (!peer->used) continue;
        if (peer->relay_me || peer->obs_disc.ip_len || peer->obs_data.ip_len) continue;
        if (peer->ip_len==addr->ip_len && peer->port==addr->port && memcmp(peer->ip, addr->ip, 16)==0)
            i_dart_discovery_peer_gone(st, peer);
    }
}

static int i_dart_discovery_addr_is(const DartDiscoveryAddr *a, const uint8_t *ip, uint8_t ip_len,
                                    uint16_t port){
    return a->ip_len == ip_len && a->port == port && memcmp(a->ip, ip, ip_len) == 0;
}

/* The observed source twin of evict_endpoint. */
static void i_dart_discovery_evict_observed(DartDiscoveryState *st, const uint8_t *ip,
                                            uint8_t ip_len, uint16_t port){
    uint16_t i;
    if (!ip || !port) return;
    for (i=0;i<st->cap_peers;i++){
        i_DartDiscoveryPeer *peer = &st->peers[i];
        if (!peer->used) continue;
        if ((peer->obs_disc.ip_len && i_dart_discovery_addr_is(&peer->obs_disc, ip, ip_len, port)) ||
            (peer->obs_data.ip_len && i_dart_discovery_addr_is(&peer->obs_data, ip, ip_len, port)))
            i_dart_discovery_peer_gone(st, peer);
    }
}

/* Builds an announce, solicit or bye. with_blob adds the discovery section and the overlay. */
static size_t i_dart_discovery_build(DartDiscoveryState *st, uint8_t flags, int with_blob,
                                   uint8_t *p, size_t cap){
    uint16_t meta_len = 0;
    if (cap < (size_t)DART_DISCOVERY_META_OFF) return 0;
    /* a node with no multicast asks whoever hears it to announce it onward */
    if (st->cfg.relay_me && !(flags & DART_DISCOVERY_FLAG_BYE)) flags |= DART_DISCOVERY_FLAG_RELAY_ME;
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
        *b++ = st->self_name_len;                             /* name */
        if (st->self_name_len){ memcpy(b, st->self_name, st->self_name_len); b += st->self_name_len; }
        if (st->self_meta.len){ memcpy(b, st->self_meta.data, st->self_meta.len); b += st->self_meta.len; }
        meta_len = (uint16_t)(b - (p + DART_DISCOVERY_META_OFF));
    }
    i_dart_le_w32(p+DART_DISCOVERY_HDR_LEN, st->self_meta_version);
    i_dart_le_w16(p+DART_DISCOVERY_HDR_LEN+4, meta_len);
    return (size_t)DART_DISCOVERY_META_OFF + meta_len;
}

/* Builds a PROXIED announce for peer: its uuid and blob version, its locator stated
 * outright, then our uuid as a trailer after the blob so a relay can tell its own echo. */
static size_t i_dart_discovery_build_proxy(DartDiscoveryState *st, const i_DartDiscoveryPeer *peer,
                                   uint8_t *p, size_t cap){
    uint8_t *b, *bend;
    size_t need;
    if (cap < (size_t)DART_DISCOVERY_META_OFF) return 0;
    if (!peer->port || (peer->ip_len != 4 && peer->ip_len != 16)) return 0;
    if (!peer->meta_version) return 0;
    p[0]='u'; p[1]='D'; p[2]='S'; p[3]='C';
    p[4]=(uint8_t)DART_DISCOVERY_PROTO_VERSION;
    /* PROXIED is the loop stop. RELAY_ME is carried as origin information only. */
    p[5]=(uint8_t)(DART_DISCOVERY_FLAG_PROXIED |
                   (peer->relay_me ? DART_DISCOVERY_FLAG_RELAY_ME : 0));
    i_dart_le_w16(p+6, st->cfg.domain_id);
    memcpy(p+8, peer->uuid, 16);
    b = p + DART_DISCOVERY_META_OFF; bend = p + cap;
    need = 2u + 1u + (size_t)peer->ip_len + 1u + peer->name_len + peer->meta_len + 16u;
    if ((size_t)(bend - b) < need) return 0;
    i_dart_le_w16(b, peer->port); b += 2;
    *b++ = peer->ip_len;
    memcpy(b, peer->ip, peer->ip_len); b += peer->ip_len;
    *b++ = peer->name_len;
    if (peer->name_len){ memcpy(b, peer->name, peer->name_len); b += peer->name_len; }
    if (peer->meta_len){ memcpy(b, peer->meta, peer->meta_len); b += peer->meta_len; }
    i_dart_le_w32(p+DART_DISCOVERY_HDR_LEN, peer->meta_version);   /* the origin's version */
    i_dart_le_w16(p+DART_DISCOVERY_HDR_LEN+4, (uint16_t)(b - (p + DART_DISCOVERY_META_OFF)));
    memcpy(b, st->cfg.uuid, 16); b += 16;    /* relayer trailer, after the blob */
    return (size_t)(b - p);
}

/* Higher wins: 2 on one of our subnets, 1 routed, 0 link local. See spec/discovery.md. */
static int i_dart_discovery_addr_rank(const DartDiscoveryState *st, const uint8_t *ip, uint8_t ip_len){
    uint8_t i;
    if (ip_len != 4) return 1;
    /* first, or a link local adapter's own /16 makes every 169.254 address look shared */
    if (ip[0] == 169 && ip[1] == 254) return 0;
    for (i = 0; i < st->n_local_nets; i++){
        const DartDiscoverySubnet *net = &st->local_nets[i];
        int k, same = 1;
        for (k = 0; k < 4; k++) if (((ip[k] ^ net->ip[k]) & net->mask[k]) != 0){ same = 0; break; }
        if (same) return 2;
    }
    return 1;
}

static void i_dart_discovery_addr_of(const i_DartDiscoveryPeer *peer, DartDiscoveryAddr *out){
    memset(out, 0, sizeof *out);
    memcpy(out->ip, peer->ip, 16);
    out->ip_len = peer->ip_len;
    out->port   = peer->port;
}

/* Discovery TX destination: an observed source (2, exact), else the locator (1, expanded). */
static int i_dart_discovery_disc_dest(const i_DartDiscoveryPeer *peer, DartDiscoveryAddr *out){
    if (peer->obs_disc.ip_len){ *out = peer->obs_disc; return 2; }
    if (peer->obs_data.ip_len){ *out = peer->obs_data; return 2; }
    i_dart_discovery_addr_of(peer, out);
    return 1;
}

void dart_discovery_on_datagram(DartDiscoveryState *st, const DartDiscoveryAddr *src,
                       DartDiscoveryVia via, DartBytes datagram, uint64_t now){
    const uint8_t *p = datagram.data; size_t len = datagram.len;
    const uint8_t *src_ip = (src && (src->ip_len==4 || src->ip_len==16)) ? src->ip : NULL;
    uint8_t  src_ip_len = src_ip ? src->ip_len : 0;
    uint16_t src_port   = src_ip ? src->port : 0;
    uint8_t flags; uint16_t meta_len; uint32_t meta_version;
    const uint8_t *uuid, *blob;
    DartDiscoveryAddr addr; int idx, addr_changed, first_contact=0, blob_changed=0, revived=0;
    int keep_held;  /* a unicast only receiver holds its known locator against the source */
    int proxied;   /* rebuilt by a relay: the locator is a candidate and it enlists nobody */
    const uint8_t *relayer = NULL;   /* the proxy trailer, NULL on an old build */
    i_DartDiscoveryPeer *peer;
    /* the blob's discovery section, valid when have_disc. disc_name is not NUL terminated */
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
        /* the OS truncated the datagram. The intact header says how much the peer wanted
           to send, so an IO layer that can grow does so and re solicits. */
        DartDiscoveryAddr a; int at = i_dart_discovery_find(st, uuid);
        memset(&a, 0, sizeof a);
        if (src_ip && (src_ip_len==4 || src_ip_len==16)){ memcpy(a.ip, src_ip, src_ip_len); a.ip_len = src_ip_len; }
        i_dart_discovery_fire_meta_too_big(st, at >= 0 ? st->peers[at].local_id : 0, &a,
                                           dart_bytes(NULL, meta_len));
        return;
    }
    blob = p + DART_DISCOVERY_META_OFF;
    flags = p[5];
    proxied = (flags & DART_DISCOVERY_FLAG_PROXIED) != 0;
    if (proxied && len >= (size_t)DART_DISCOVERY_META_OFF + meta_len + 16u)
        relayer = p + DART_DISCOVERY_META_OFF + meta_len;   /* build_proxy's relayer trailer */

    /* the discovery section, then the opaque overlay. A malformed blob drops the datagram */
    if (meta_len){
        const uint8_t *b = blob, *bend = blob + meta_len;
        if (b + 3 > bend) return;                          /* port(2) ip_len(1) */
        disc_port = i_dart_le_r16(b); b += 2;
        disc_ip_len = *b++;
        if (disc_ip_len==4 || disc_ip_len==16){ if (b + disc_ip_len > bend) return; disc_ip = b; b += disc_ip_len; }
        else disc_ip_len = 0;
        if (b + 1 > bend) return;                          /* name_len(1) */
        {   uint8_t dnl = *b++;
            if (dnl){ if (b + dnl > bend) return; disc_name = dart_string((const char*)b, dnl); b += dnl; } }
        overlay = dart_bytes(b, (size_t)(bend - b));
        if (overlay.len > st->meta_cap){   /* this side can never hold it, so say so */
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
        if (idx >= 0) i_dart_discovery_peer_gone(st, &st->peers[idx]);   /* a BYE is GONE */
        return;
    }

    /* The IP is a candidate from the source, or a stated self_ip. A unicast only node keeps
       a held locator, since its own NAT rewrites sources. See spec/discovery.md. */
    keep_held = st->cfg.relay_me && !proxied && !disc_ip_len && idx >= 0
                && (st->peers[idx].ip_len == 4 || st->peers[idx].ip_len == 16);
    memset(&addr, 0, sizeof addr);
    if (have_disc){
        if (disc_ip_len){ addr.ip_len = disc_ip_len; memcpy(addr.ip, disc_ip, disc_ip_len); }
        else if (keep_held){ addr.ip_len = st->peers[idx].ip_len; memcpy(addr.ip, st->peers[idx].ip, 16); }
        else if (src_ip && (src_ip_len==4 || src_ip_len==16)){ addr.ip_len = src_ip_len; memcpy(addr.ip, src_ip, src_ip_len); }
        else return;
        addr.port = disc_port;
    } else if (idx >= 0){
        addr.port = st->peers[idx].port;
        if (!keep_held && src_ip && (src_ip_len==4 || src_ip_len==16)){ addr.ip_len = src_ip_len; memcpy(addr.ip, src_ip, src_ip_len); }
        else { addr.ip_len = st->peers[idx].ip_len; memcpy(addr.ip, st->peers[idx].ip, 16); }
    } else if (src_ip && (src_ip_len==4 || src_ip_len==16)){
        addr.ip_len = src_ip_len; memcpy(addr.ip, src_ip, src_ip_len);   /* port from the blob */
    } else return;

    if (idx < 0){
        uint8_t *keep_meta, *keep_user; uint16_t keep_cap;
        /* a new uuid from a held endpoint means a restart: evict the dead predecessor. A
           RELAY_ME locator asserts no endpoint, its source is the real one. */
        if (!(flags & DART_DISCOVERY_FLAG_RELAY_ME))
            i_dart_discovery_evict_endpoint(st, &addr);
        if (!proxied && src_ip)
            i_dart_discovery_evict_observed(st, src_ip, src_ip_len, src_port);
        idx = i_dart_discovery_alloc(st);
        if (idx < 0){       /* table full of active peers: refuse, never evict a live one */
            i_dart_discovery_fire_refused(st, &addr);
            return;
        }
        keep_meta = st->peers[idx].meta;            /* the blob buffer survives the reset */
        keep_cap  = st->peers[idx].meta_cap;
        keep_user = st->peers[idx].user;
        memset(&st->peers[idx], 0, sizeof(i_DartDiscoveryPeer));
        st->peers[idx].meta     = keep_meta;
        st->peers[idx].meta_cap = keep_cap;
        st->peers[idx].user     = keep_user;
        if (keep_user) memset(keep_user, 0, st->user_stride);   /* fresh scratch for the new peer */
        st->peers[idx].used     = 1;
        memcpy(st->peers[idx].uuid, uuid, 16);
        st->peers[idx].local_id = st->next_local_id++;
        st->peers[idx].ip_len   = 0xFF;   /* force first peer_up */
        st->peers[idx].introduce_cursor = DART_DISCOVERY_INTRODUCE_IDLE;
        first_contact = 1;
    }
    peer = &st->peers[idx];
    if (peer->dropped){ peer->dropped = 0; revived = 1; }   /* a DROPPED peer returned: resume it */
    peer->last_heard_us = now;
    if (!proxied) peer->last_direct_us = now;   /* only the peer itself is direct liveness */
    else if (!(relayer && memcmp(relayer, st->cfg.uuid, 16)==0) && peer->proxies_heard != 0xFFu)
        peer->proxies_heard++;   /* a foreign proxy counts, our own echo must not */

    /* Rank the candidate against the incumbent so a multi path locator never flaps. A
       stated self_ip skips this, a proxied one is only a candidate. See spec/discovery.md. */
    if ((!disc_ip_len || proxied) && peer->ip_len == 4 && addr.ip_len == 4 && memcmp(peer->ip, addr.ip, 4) != 0){
        int rank_new = i_dart_discovery_addr_rank(st, addr.ip, 4);
        int rank_old = i_dart_discovery_addr_rank(st, peer->ip, 4);
        if (rank_new < rank_old ||
            (rank_new == rank_old &&
             now - peer->addr_heard_us < (uint64_t)st->cfg.announce_interval_us * 2u))
            memcpy(addr.ip, peer->ip, 4);          /* keep the incumbent */
    }

    addr_changed = (peer->ip_len != addr.ip_len)
                || (peer->port   != addr.port)
                || (memcmp(peer->ip, addr.ip, 16) != 0);
    if (addr_changed){
        peer->ip_len = addr.ip_len; peer->port = addr.port;
        memcpy(peer->ip, addr.ip, 16);
    }
    /* only a datagram that really arrived from the held locator counts as freshness */
    if (peer->ip_len == 4 && src_ip && src_ip_len == 4 && memcmp(peer->ip, src_ip, 4) == 0)
        peer->addr_heard_us = now;

    /* A newer version with the blob updates our copy. A newer version without it means we
       fell behind, so re fetch with a targeted solicit. */
    if (meta_version > peer->adv_version) peer->adv_version = meta_version;
    if (have_disc){
        if (meta_version > peer->meta_version){
            uint8_t nl = disc_name.len > DART_DISCOVERY_NAME_MAX ? DART_DISCOVERY_NAME_MAX : (uint8_t)disc_name.len;
            int fits = 1;
            if (overlay.len > peer->meta_cap){   /* hook mode resizes, pool slots are pre sized */
                uint8_t *nb = st->cfg.alloc ? (uint8_t*)st->cfg.alloc(st->cfg.alloc_user, peer->meta, overlay.len)
                                            : NULL;
                if (nb){ peer->meta = nb; peer->meta_cap = (uint16_t)overlay.len; }
                else fits = 0;                   /* OOM: keep the stale blob, re fetch retries */
            }
            if (fits){
                if (overlay.len) memcpy(peer->meta, overlay.data, overlay.len);
                peer->meta_len = (uint16_t)overlay.len; peer->meta_version = meta_version;
                if (disc_name.data && nl) memcpy(peer->name, disc_name.data, nl);
                peer->name[nl] = '\0'; peer->name_len = nl;
                /* only a direct announce may state a self_ip. A proxy always embeds a
                   locator, which asserts nothing. */
                if (!proxied){
                    peer->stated_ip = disc_ip_len ? 1u : 0u;
                    if (peer->stated_ip){
                        memset(&peer->obs_disc, 0, sizeof peer->obs_disc);
                        memset(&peer->obs_data, 0, sizeof peer->obs_data);
                    }
                }
                blob_changed = 1;
            }
        }
        peer->solicit_due = 0;
    } else if (meta_version > peer->meta_version){
        peer->solicit_due = 1;
    }

    /* Only a direct announce enlists a relay. A new or changed relay me peer is introduced
       at once, not at our next announce. */
    if (!proxied){
        peer->heard_direct = 1;
        peer->wants_relay = (flags & DART_DISCOVERY_FLAG_RELAY_ME) ? 1u : 0u;
        peer->relay_me    = peer->wants_relay;   /* its own word beats any proxy's */
        if (!peer->relay_me && !st->cfg.relay_me){   /* ordinary again */
            memset(&peer->obs_disc, 0, sizeof peer->obs_disc);
            memset(&peer->obs_data, 0, sizeof peer->obs_data);
        }
        if (peer->wants_relay && (first_contact || blob_changed || revived || addr_changed))
            peer->relay_due = 1;
    } else if (!peer->heard_direct && (flags & DART_DISCOVERY_FLAG_RELAY_ME))
        peer->relay_me = 1;   /* known only second hand, the proxy's origin information stands */

    /* Bind, or quietly rebind, the source per arrival channel. A unicast only node binds it
       for every direct peer, since everything reached it through its own NAT. */
    if (!proxied && ((flags & DART_DISCOVERY_FLAG_RELAY_ME) || st->cfg.relay_me)
        && !peer->stated_ip && src_ip && src_port){
        DartDiscoveryAddr *obs = (via == DART_DISCOVERY_VIA_DATA) ? &peer->obs_data
                                                                  : &peer->obs_disc;
        memset(obs, 0, sizeof *obs);
        memcpy(obs->ip, src_ip, src_ip_len);
        obs->ip_len = src_ip_len; obs->port = src_port;
    }

    /* Introductions: a relay me peer is walked the whole table when it appears, and every
       relay me peer is re walked when a direct peer appears, resumes or moves. */
    if (!proxied && (first_contact || revived) && peer->wants_relay)
        peer->introduce_cursor = 0;
    if (!proxied && (first_contact || revived || addr_changed)){
        uint16_t k;
        for (k=0;k<st->cap_peers;k++){
            i_DartDiscoveryPeer *rme = &st->peers[k];
            if (rme == peer || !rme->used || rme->dropped || !rme->wants_relay) continue;
            rme->introduce_cursor = 0;
        }
    }

    if (first_contact || addr_changed || blob_changed || revived)
        i_dart_discovery_fire_up(st, peer, &addr);

    if ((flags & DART_DISCOVERY_FLAG_REQ) && st->started)
        peer->reply_due = 1;   /* answer the solicit with a unicast announce and blob */
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
            /* silent past the gone timeout too: a return is no longer expected, free it */
            if (st->cfg.gone_timeout_us &&
                now - st->peers[i].last_heard_us > (uint64_t)st->cfg.peer_timeout_us + st->cfg.gone_timeout_us)
                i_dart_discovery_peer_gone(st, &st->peers[i]);
            continue;
        }
        if (st->peers[i].heard_direct &&
            now - st->peers[i].last_direct_us > st->cfg.peer_timeout_us)
            st->peers[i].heard_direct = 0;   /* direct path silent: not ours to introduce */
        if (now - st->peers[i].last_heard_us > st->cfg.peer_timeout_us){
            /* fell silent: demote, keeping the slot and id so a same uuid return resumes */
            st->peers[i].dropped = 1;
            st->peers[i].heard_direct = 0;   /* a proxy only return is not ours to introduce */
            i_dart_discovery_fire_down(st, st->peers[i].local_id, DART_DISCOVERY_DROP);
        }
    }
    if (st->want_solicit){   /* multicast solicit: announce with the blob and ask peers to reply */
        st->want_solicit = 0;
        return i_dart_discovery_build(st, DART_DISCOVERY_FLAG_REQ, 1, (uint8_t *)out, cap);
    }
    if (now >= st->next_announce_us){
        int with_blob = st->self_blob_resend > 0;
        uint16_t k;
        if (with_blob) st->self_blob_resend--;
        st->next_announce_us = now + st->cfg.announce_interval_us;
        /* One proxy per relay me peer per interval, gated on direct liveness and suppressed
           when two foreign proxies were heard. The introduction sweep rides the same cadence. */
        for (k=0;k<st->cap_peers;k++){
            i_DartDiscoveryPeer *rp = &st->peers[k];
            uint8_t heard = rp->proxies_heard;
            rp->proxies_heard = 0;
            if (rp->used && !rp->dropped && rp->wants_relay &&
                now - rp->last_direct_us <= st->cfg.peer_timeout_us){
                if (heard < 2u) rp->relay_due = 1;
                if (now >= rp->introduce_sweep_us){
                    rp->introduce_cursor   = 0;
                    rp->introduce_sweep_us = now +
                        (uint64_t)st->cfg.announce_interval_us * DART_DISCOVERY_INTRODUCE_SWEEP;
                }
            }
        }
        return i_dart_discovery_build(st, 0, with_blob, (uint8_t *)out, cap);
    }
    return 0;
}

uint64_t dart_discovery_next_due_us(const DartDiscoveryState *st){
    if (!st || !st->started || st->want_solicit) return 0;
    return st->next_announce_us;   /* 0 after an advertise: announce now */
}

void dart_discovery_set_meta(DartDiscoveryState *st, DartBytes meta){
    if (!st || meta.len > st->meta_cap) return;   /* the node sizes meta_cap to fit */
    st->self_meta         = meta;
    st->self_meta_version++;
    st->self_blob_resend  = DART_DISCOVERY_BLOB_RESEND;
    st->next_announce_us  = 0;   /* announce the change now */
}

uint32_t dart_discovery_meta_version(const DartDiscoveryState *st){
    return st ? st->self_meta_version : 0;
}

void dart_discovery_set_data_port(DartDiscoveryState *st, uint16_t port){
    if (!st || st->cfg.data_port == port) return;
    st->cfg.data_port = port;            /* the port rides the blob, so bump and re send it */
    st->self_meta_version++;
    st->self_blob_resend = DART_DISCOVERY_BLOB_RESEND;
    st->next_announce_us = 0;
}

void dart_discovery_set_local_subnets(DartDiscoveryState *st, const DartDiscoverySubnet *nets, uint8_t n){
    uint8_t i, k = 0;
    if (!st) return;
    if (n > DART_DISCOVERY_MAX_SUBNETS) n = DART_DISCOVERY_MAX_SUBNETS;
    for (i = 0; i < n; i++){
        const uint8_t *m = nets[i].mask;
        if (!(m[0] | m[1] | m[2] | m[3])) continue;   /* an unknown prefix matches everything */
        st->local_nets[k++] = nets[i];
    }
    st->n_local_nets = k;
}

size_t dart_discovery_poll_targeted(DartDiscoveryState *st, void *out, size_t cap,
                                    DartDiscoveryAddr *to, int *exact){
    uint16_t n = st->cap_peers, k;
    if (n == 0) return 0;
    for (k=0;k<n;k++){
        uint16_t i = st->targeted_cursor;
        i_DartDiscoveryPeer *peer = &st->peers[i];
        st->targeted_cursor = (uint16_t)((i+1u) % n);
        if (!peer->used){ peer->reply_due = peer->solicit_due = 0; continue; }
        if (peer->reply_due){                 /* reply to a soliciter with the blob */
            peer->reply_due = 0;
            if (exact) *exact = i_dart_discovery_disc_dest(peer, to) == 2;
            else       i_dart_discovery_disc_dest(peer, to);
            return i_dart_discovery_build(st, 0, 1, (uint8_t *)out, cap);
        }
        if (peer->solicit_due){               /* re fetch: ask this peer to announce back */
            peer->solicit_due = 0;
            if (exact) *exact = i_dart_discovery_disc_dest(peer, to) == 2;
            else       i_dart_discovery_disc_dest(peer, to);
            return i_dart_discovery_build(st, DART_DISCOVERY_FLAG_REQ, 1, (uint8_t *)out, cap);
        }
    }
    return 0;
}

size_t dart_discovery_poll_introduce(DartDiscoveryState *st, void *out, size_t cap,
                                     DartDiscoveryAddr *to, int *exact){
    uint16_t i;
    if (!st) return 0;
    for (i=0;i<st->cap_peers;i++){
        i_DartDiscoveryPeer *peer = &st->peers[i];
        if (!peer->used || peer->dropped || !peer->wants_relay) continue;
        if (peer->introduce_cursor == (uint16_t)DART_DISCOVERY_INTRODUCE_IDLE) continue;
        while (peer->introduce_cursor < st->cap_peers){
            i_DartDiscoveryPeer *origin = &st->peers[peer->introduce_cursor++];
            size_t n_bytes;
            /* only peers we hear directly are ours to introduce, the one hop rule */
            if (origin == peer || !origin->used || origin->dropped || !origin->heard_direct)
                continue;
            n_bytes = i_dart_discovery_build_proxy(st, origin, (uint8_t *)out, cap);
            if (!n_bytes) continue;           /* nothing useful to say about it yet */
            if (exact) *exact = i_dart_discovery_disc_dest(peer, to) == 2;
            else       i_dart_discovery_disc_dest(peer, to);
            return n_bytes;
        }
        peer->introduce_cursor = (uint16_t)DART_DISCOVERY_INTRODUCE_IDLE;
    }
    return 0;
}

size_t dart_discovery_poll_relay(DartDiscoveryState *st, void *out, size_t cap){
    uint16_t n, k;
    if (!st) return 0;
    n = st->cap_peers;
    if (n == 0) return 0;
    for (k=0;k<n;k++){
        uint16_t i = st->relay_cursor;
        i_DartDiscoveryPeer *peer = &st->peers[i];
        st->relay_cursor = (uint16_t)((i+1u) % n);
        /* a dropped peer is introduced to nobody: the silence is how third parties learn it went */
        if (!peer->used || peer->dropped || !peer->wants_relay){ peer->relay_due = 0; continue; }
        if (!peer->relay_due) continue;
        peer->relay_due = 0;
        {   size_t n_bytes = i_dart_discovery_build_proxy(st, peer, (uint8_t *)out, cap);
            if (n_bytes) return n_bytes; }   /* 0 means nothing to say yet, try the next */
    }
    return 0;
}

void dart_discovery_solicit(DartDiscoveryState *st){ if (st) st->want_solicit = 1; }

/* No version changes. The local side may now have a topic a held blob's interest matches. */
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
    uint16_t i, c = 0;   /* DROPPED entries linger for resume and are not members */
    for (i=0;i<st->cap_peers;i++) if (st->peers[i].used && !st->peers[i].dropped) c++;
    return c;
}

uint16_t dart_discovery_max_peers(const DartDiscoveryState *st){ return st->cap_peers; }

int dart_discovery_peer_at(const DartDiscoveryState *st, uint16_t slot, DartDiscoveryPeer *out){
    const i_DartDiscoveryPeer *p;
    if (slot >= st->cap_peers) return 0;
    p = &st->peers[slot];
    if (!p->used) return 0;                  /* a DROPPED entry is still used */
    memset(out, 0, sizeof *out);
    out->id = p->local_id;
    memcpy(out->uuid, p->uuid, 16);
    i_dart_discovery_addr_of(p, &out->addr);
    out->liveness      = p->dropped ? DART_PEER_DROPPED : DART_PEER_ACTIVE;
    out->last_heard_us = p->last_heard_us;
    out->name          = dart_string(p->name_len ? p->name : NULL, p->name_len);
    out->meta          = dart_bytes(p->meta_len ? p->meta : NULL, p->meta_len);
    out->meta_version  = p->meta_version;
    out->adv_meta_version = p->adv_version;
    out->user          = st->user_stride ? p->user : NULL;
    return 1;
}

/* DROPPED included. */
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
    /* the observed data source when bound, else the locator. See spec/discovery.md */
    if      (p->obs_data.ip_len) *out = p->obs_data;
    else if (p->obs_disc.ip_len) *out = p->obs_disc;
    else i_dart_discovery_addr_of(p, out);
    return 1;
}

DartBytes dart_discovery_peer_meta(const DartDiscoveryState *st, uint32_t id, uint32_t *version){
    i_DartDiscoveryPeer *p = i_dart_discovery_by_id(st, id);
    if (version) *version = p ? p->meta_version : 0;
    if (!p || !p->meta_len) return dart_bytes(NULL, 0);
    return dart_bytes(p->meta, p->meta_len);
}

DartString dart_discovery_peer_name(const DartDiscoveryState *st, uint32_t id){
    i_DartDiscoveryPeer *p = i_dart_discovery_by_id(st, id);
    if (!p) return dart_string(NULL, 0);                 /* unknown peer: .data NULL */
    return dart_string(p->name, p->name_len);            /* known: .data set, .len may be 0 */
}

int dart_discovery_id_for_addr(const DartDiscoveryState *st, const uint8_t *ip, uint8_t ip_len,
                               uint16_t port, uint32_t *id){
    uint16_t i;
    if (!ip || (ip_len != 4 && ip_len != 16)) return 0;
    for (i=0;i<st->cap_peers;i++){
        const i_DartDiscoveryPeer *p = &st->peers[i];
        int hit;
        if (!p->used) continue;
        /* a peer with observed sources matches only those. Its locator is not its endpoint
           and may even equal another peer's. */
        if (p->obs_disc.ip_len || p->obs_data.ip_len)
            hit = (p->obs_disc.ip_len && i_dart_discovery_addr_is(&p->obs_disc, ip, ip_len, port)) ||
                  (p->obs_data.ip_len && i_dart_discovery_addr_is(&p->obs_data, ip, ip_len, port));
        else
            hit = p->ip_len == ip_len && p->port == port && memcmp(p->ip, ip, ip_len) == 0;
        if (hit){
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
    if (!p->used || p->dropped) return 0;   /* active only, a dropped peer just bounces */
    return i_dart_discovery_disc_dest(p, out);   /* 2 = observed source: exact, never expanded */
}
#pragma endregion

#ifndef DART_DISCOVERY_SANS_IO
#ifndef DART_PLAT_CUSTOM
#pragma region platform/core.c
/* The Windows and POSIX implementation. The only file in DART with an OS ifdef. */

/* feature test macros must precede the first system header */
#if !defined(_WIN32)
  #ifndef _POSIX_C_SOURCE
  #define _POSIX_C_SOURCE 200809L
  #endif
  #ifndef _DEFAULT_SOURCE
  #define _DEFAULT_SOURCE 1
  #endif
#endif

#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <bcrypt.h>            /* BCryptGenRandom */
  #include <mmsystem.h>          /* timeBeginPeriod */
  #ifndef PSAPI_VERSION
  #define PSAPI_VERSION 2        /* GetProcessMemoryInfo from kernel32, no psapi.lib */
  #endif
  #include <psapi.h>             /* GetProcessMemoryInfo */
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
    #include <ifaddrs.h>         /* getifaddrs */
    #include <net/if.h>          /* IFF_UP and IFF_LOOPBACK */
  #endif
  #include <unistd.h>
  #if defined(ESP_PLATFORM)
    #include <sys/poll.h>       /* the ESP newlib has no poll.h */
  #else
    #include <poll.h>
  #endif
  #include <time.h>
  #ifdef DART_THREADS
    #include <pthread.h>
    #if defined(ESP_PLATFORM)
      #include <freertos/FreeRTOS.h>
      #include <freertos/task.h>   /* xTaskGetCurrentTaskHandle */
    #endif
  #endif
  #include <fcntl.h>
  #include <errno.h>
  #include <stdio.h>
  #include <stdlib.h>           /* arc4random_buf */
  #ifdef DART_PROC_STATS
    #if defined(ESP_PLATFORM)
      #include <esp_heap_caps.h>  /* heap_caps_get_* */
      #include <freertos/FreeRTOS.h>
      #include <freertos/task.h>  /* uxTaskGetSystemState */
    #else
      #include <sys/resource.h>   /* getrusage */
      #if defined(__APPLE__)
        #include <mach/mach.h>    /* task_info */
      #endif
    #endif
  #endif
  #if defined(ESP_PLATFORM)
    #include <esp_random.h>     /* esp_fill_random */
    #include <esp_netif.h>      /* esp_netif_get_ip_info */
    #if defined(__has_include) && __has_include(<esp_mac.h>)
      #include <esp_mac.h>      /* IDF 5: esp_efuse_mac_get_default */
    #else
      #include <esp_system.h>   /* IDF 4: the same declaration */
    #endif
  #elif defined(__linux__)
    #include <sys/random.h>     /* getrandom */
  #endif
  typedef socklen_t i_DartSocklen;
  #define DART__FD(s) ((int)(s))
#endif

/* lifecycle */
#ifdef _WIN32
static LARGE_INTEGER i_dart_plat_qpc_freq;   /* set in startup, lazily elsewhere */
int i_dart_plat_startup(void){
    WSADATA w;
    /* both calls are refcounted by the OS per process, so matched calls need no counter here */
    if (WSAStartup(MAKEWORD(2,2), &w) != 0) return 0;
  #ifndef DART_NO_HIGHRES_TIMER
    timeBeginPeriod(1);  /* the default 15.6 ms tick throttles ACK and repair rates */
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

/* clock */
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

/* entropy and host */
int i_dart_plat_random(void *buf, size_t len){
#if defined(_WIN32)
    /* a NULL handle selects the system preferred RNG, 0 is success */
    return BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(ESP_PLATFORM)
    esp_fill_random(buf, len);       /* hardware RNG, true random while RF is up */
    return 1;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
      defined(__NetBSD__) || defined(__DragonFly__)
    arc4random_buf(buf, len);        /* cannot fail */
    return 1;
#else
    {
        uint8_t *p = (uint8_t*)buf; size_t got = 0;
  #if defined(__linux__)
        while (got < len){
            ssize_t r = getrandom(p + got, len - got, 0);
            if (r < 0){ if (errno == EINTR) continue; break; }
            got += (size_t)r;
        }
        if (got == len) return 1;
  #endif
        {
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
#if defined(ESP_PLATFORM)
    /* lwIP has no gethostname. The factory MAC is unique and readable before any
       interface is up, which is what the uuid fallback wants. */
    {   static const char hex[] = "0123456789abcdef";
        uint8_t mac[6]; size_t i;
        if (cap >= 17 && esp_efuse_mac_get_default(mac) == ESP_OK){
            memcpy(buf, "esp-", 4);
            for (i = 0; i < 6; i++){
                buf[4 + i*2]     = hex[mac[i] >> 4];
                buf[4 + i*2 + 1] = hex[mac[i] & 0x0F];
            }
            buf[16] = 0;
        }
    }
#else
    gethostname(buf, (int)cap - 1);
#endif
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

/* process usage and wall clock */
#ifdef DART_PROC_STATS
int i_dart_plat_proc_stats(uint64_t *cpu_us, uint64_t *rss_bytes, uint64_t *peak_rss_bytes,
                           int *have_cpu){
#if defined(_WIN32)
    FILETIME created, exited, kern, user; PROCESS_MEMORY_COUNTERS pmc;
    ULARGE_INTEGER uk, uu;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kern, &user)) return 0;
    uk.LowPart = kern.dwLowDateTime; uk.HighPart = kern.dwHighDateTime;
    uu.LowPart = user.dwLowDateTime; uu.HighPart = user.dwHighDateTime;
    if (cpu_us) *cpu_us = (uk.QuadPart + uu.QuadPart) / 10u;   /* 100 ns to us */
    if (have_cpu) *have_cpu = 1;
    pmc.cb = sizeof pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return 0;
    if (rss_bytes)      *rss_bytes      = (uint64_t)pmc.WorkingSetSize;
    if (peak_rss_bytes) *peak_rss_bytes = (uint64_t)pmc.PeakWorkingSetSize;
    return 1;
#elif defined(ESP_PLATFORM)
    /* The firmware image is the process: RSS is heap in use and peak RSS is the free heap
       low water mark. CPU needs the ESP timer run time stats, else it is omitted. */
    {   size_t total   = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
        size_t freeb   = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
        size_t minfree = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
        if (cpu_us)         *cpu_us         = 0;
        if (have_cpu) *have_cpu = 0;
#if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS \
 && defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && CONFIG_FREERTOS_USE_TRACE_FACILITY \
 && defined(CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS) && CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS \
 && defined(CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER) && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
        {   static TaskStatus_t *tasks; static UBaseType_t cap;
            static uint32_t prev_total, prev_idle; static uint64_t busy_us; static int started;
            UBaseType_t need = uxTaskGetNumberOfTasks(), got, i;
            uint32_t total_run = 0, idle_run = 0;
            if (need > cap){
                TaskStatus_t *p = (TaskStatus_t*)realloc(tasks, (size_t)need * sizeof *tasks);
                if (!p) goto esp_cpu_done;
                tasks = p; cap = need;
            }
            got = uxTaskGetSystemState(tasks, cap, &total_run);
            for (i = 0; i < got; i++)
                if (strncmp(tasks[i].pcTaskName, "IDLE", 4) == 0) idle_run += tasks[i].ulRunTimeCounter;
            if (started){
                uint32_t dt = total_run - prev_total, di = idle_run - prev_idle;
                uint64_t span = (uint64_t)dt * (uint64_t)portNUM_PROCESSORS;
                busy_us += span > di ? span - di : 0;
            }
            prev_total = total_run; prev_idle = idle_run; started = 1;
            if (cpu_us) *cpu_us = busy_us;
            if (have_cpu) *have_cpu = 1;
        }
esp_cpu_done:
#endif
        if (rss_bytes)      *rss_bytes      = (uint64_t)(total > freeb   ? total - freeb   : 0);
        if (peak_rss_bytes) *peak_rss_bytes = (uint64_t)(total > minfree ? total - minfree : 0);
        return 1;
    }
#else
    {   struct rusage ru;
        if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
        if (cpu_us) *cpu_us = (uint64_t)ru.ru_utime.tv_sec * 1000000ull + (uint64_t)ru.ru_utime.tv_usec
                            + (uint64_t)ru.ru_stime.tv_sec * 1000000ull + (uint64_t)ru.ru_stime.tv_usec;
        if (have_cpu) *have_cpu = 1;
  #if defined(__APPLE__)
        if (peak_rss_bytes) *peak_rss_bytes = (uint64_t)ru.ru_maxrss;         /* bytes on macOS */
        if (rss_bytes){   /* current: task_info */
            struct mach_task_basic_info info; mach_msg_type_number_t cnt = MACH_TASK_BASIC_INFO_COUNT;
            *rss_bytes = (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                                    (task_info_t)&info, &cnt) == KERN_SUCCESS)
                       ? (uint64_t)info.resident_size : 0;
        }
  #elif defined(__linux__)
        if (peak_rss_bytes) *peak_rss_bytes = (uint64_t)ru.ru_maxrss * 1024u; /* KB on Linux */
        if (rss_bytes){   /* current: statm field 2 */
            FILE *f = fopen("/proc/self/statm", "rb");
            unsigned long total_pages = 0, res_pages = 0;
            *rss_bytes = 0;
            if (f){
                if (fscanf(f, "%lu %lu", &total_pages, &res_pages) == 2)
                    *rss_bytes = (uint64_t)res_pages * (uint64_t)sysconf(_SC_PAGESIZE);
                fclose(f);
            }
        }
  #else
        if (peak_rss_bytes) *peak_rss_bytes = (uint64_t)ru.ru_maxrss * 1024u; /* KB on the BSDs */
        if (rss_bytes)      *rss_bytes      = 0;   /* no cheap current RSS read */
  #endif
        return 1;
    }
#endif
}

int i_dart_plat_heap_stats(uint64_t *total_bytes, uint64_t *free_bytes,
                           uint64_t *min_free_bytes, uint64_t *largest_free_block_bytes){
#if defined(ESP_PLATFORM)
    if (total_bytes)              *total_bytes = (uint64_t)heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    if (free_bytes)               *free_bytes = (uint64_t)heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    if (min_free_bytes)           *min_free_bytes = (uint64_t)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    if (largest_free_block_bytes) *largest_free_block_bytes =
        (uint64_t)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    return 1;
#else
    (void)total_bytes; (void)free_bytes; (void)min_free_bytes; (void)largest_free_block_bytes;
    return 0;
#endif
}
#endif /* DART_PROC_STATS */

uint64_t i_dart_plat_wall_us(void){
#ifdef _WIN32
    FILETIME ft; ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return (u.QuadPart - 116444736000000000ull) / 10u;   /* FILETIME epoch to Unix, 100 ns to us */
#else
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
#endif
}

void *i_dart_plat_realloc(void *ptr, size_t size){
    if (size == 0){ free(ptr); return NULL; }
    return realloc(ptr, size);
}

/* UDP sockets */
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
    a.sin_addr.s_addr = if_naddr;
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

/* multicast */
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
/* Best effort. An interface that went away may have dropped its membership already. */
void i_dart_plat_mcast_leave(i_DartSock s, uint32_t group_naddr, uint32_t if_naddr){
    struct ip_mreq mr; memset(&mr, 0, sizeof mr);
    mr.imr_multiaddr.s_addr = group_naddr;
    mr.imr_interface.s_addr = if_naddr;
    setsockopt(DART__FD(s), IPPROTO_IP, IP_DROP_MEMBERSHIP, (const char*)&mr, sizeof mr);
}

/* datagram IO */
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
    /* Windows fails an oversized datagram with WSAEMSGSIZE after filling the buffer. POSIX
       delivers the prefix. Deliver it here too, so discovery can grow and refetch. */
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
    /* callers poll a few sockets, so the translation buffer lives on the stack */
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

/* address helpers */
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
/* The SIO_GET_INTERFACE_LIST flag bits, when the SDK headers do not define them. */
#ifndef IFF_UP
#define IFF_UP 0x00000001
#endif
#ifndef IFF_LOOPBACK
#define IFF_LOOPBACK 0x00000004
#endif
int i_dart_plat_local_ifaces(i_DartIface *out, int max){
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
        out[n].addr = a->sin_addr.s_addr;
        out[n].mask = info[i].iiNetmask.AddressIn.sin_addr.s_addr;
        n++;
    }
    return n;
}
#elif defined(ESP_PLATFORM)
/* lwIP has no getifaddrs, so name the netifs the IDF defines. WiFi or Ethernet must be up
 * before the node opens. */
int i_dart_plat_local_ifaces(i_DartIface *out, int max){
    static const char *const keys[] = { "WIFI_STA_DEF", "ETH_DEF", "WIFI_AP_DEF" };
    int n = 0; unsigned i;
    if (!out || max <= 0) return 0;
    for (i = 0; i < sizeof keys / sizeof keys[0] && n < max; i++){
        esp_netif_t *nif = esp_netif_get_handle_from_ifkey(keys[i]);
        esp_netif_ip_info_t info;
        if (nif && esp_netif_get_ip_info(nif, &info) == ESP_OK && info.ip.addr != 0){
            out[n].addr = info.ip.addr;   /* esp_ip4_addr is network order, our naddr */
            out[n].mask = info.netmask.addr;
            n++;
        }
    }
    return n;
}
#else
int i_dart_plat_local_ifaces(i_DartIface *out, int max){
    struct ifaddrs *ifs = NULL, *p;
    int n = 0;
    if (!out || max <= 0 || getifaddrs(&ifs) != 0) return 0;
    for (p = ifs; p && n < max; p = p->ifa_next){
        struct sockaddr_in *a;
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
        a = (struct sockaddr_in*)p->ifa_addr;
        out[n].addr = a->sin_addr.s_addr;
        out[n].mask = (p->ifa_netmask && p->ifa_netmask->sa_family == AF_INET)
                        ? ((struct sockaddr_in*)p->ifa_netmask)->sin_addr.s_addr : 0u;
        n++;
    }
    freeifaddrs(ifs);
    return n;
}
#endif

/* threads */
#ifdef DART_THREADS

/* The opaque blobs must fit the real OS types. C99 has no _Static_assert. */
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

/* SRWLOCK is pointer sized, pairs with condvars and is not recursive, which the condvar
   contract needs. Node reentrancy is an owner id check above this layer. */
void i_dart_plat_mutex_init   (i_DartMutex *m){ InitializeSRWLock((PSRWLOCK)m); }
void i_dart_plat_mutex_destroy(i_DartMutex *m){ (void)m; }
void i_dart_plat_mutex_lock   (i_DartMutex *m){ AcquireSRWLockExclusive((PSRWLOCK)m); }
void i_dart_plat_mutex_unlock (i_DartMutex *m){ ReleaseSRWLockExclusive((PSRWLOCK)m); }

void i_dart_plat_cond_init   (i_DartCond *c){ InitializeConditionVariable((PCONDITION_VARIABLE)c); }
void i_dart_plat_cond_destroy(i_DartCond *c){ (void)c; }
void i_dart_plat_cond_wait(i_DartCond *c, i_DartMutex *m, uint32_t timeout_us){
    /* round up in 64 bits: 0xFFFFFFFF + 999 would wrap and make the longest waits 1 ms spins */
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
    /* pthread_self aborts on ESP-IDF from a task not made by pthread_create, like the
       Arduino loopTask. The task handle is a unique id on any task. */
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
    /* the monotonic clock, so a wall clock step cannot stretch a timeout */
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
    /* Linux waits on the monotonic clock set at init. Elsewhere a wall clock jump can cut
       the wait short, which the caller's predicate loop absorbs. */
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

/* Bound to loopback and connected to itself, so only its own signals ever arrive. */
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

/* shared memory */
#ifdef DART_SHM
#ifndef _WIN32
  #include <sys/mman.h>            /* shm_open and mmap */
  #include <sys/stat.h>            /* fstat */
#endif

/* A 128 bit id from bytes, two FNV-1a passes with distinct seeds. Not cryptographic. */
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
    FILE *f = fopen("/etc/machine-id", "rb");   /* 32 hex chars, a 128 bit id */
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
    base = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);   /* 0 maps the whole section */
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
/* The discovery runtime. All OS access goes through the platform layer. */

#include <string.h>

#define DART_DISCOVERY_BYE_SENDS 3   /* one shot UDP, so resend. Receivers dedup by uuid */
#define DART_DISCOVERY_IF_SCAN_US 3000000u   /* re enumerate interfaces this often in auto mode */

struct DartDiscovery {
    DartDiscoveryState *core;
    i_DartSock            fd;
    i_DartSock            tx_fd;         /* unicast TX leaves here: the node's data socket, the own
                                            unicast_fd, else fd */
    i_DartSock            unicast_fd;    /* our unicast RX port, DART_SOCK_BAD with a data_port */
    i_DartIface          ifs[DART_DISCOVERY_MAX_SUBNETS];  /* joined and announced out of */
    uint8_t              n_ifs;          /* 0 = none usable, the OS picks */
    uint8_t              pinned;         /* explicit multicast_interface, never rescanned */
    uint8_t              unicast_only;   /* no joins and no group sends */
    uint64_t             if_scan_us;     /* next interface re enumeration */
    uint32_t             group_naddr;   /* network order */
    uint16_t             discovery_port;
    uint16_t             max_peers;
    uint32_t             wire_max;    /* scratch buffer size */
    uint8_t             *rxbuf;       /* arena, wire_max */
    uint8_t             *txbuf;       /* arena, wire_max */
    DartDiscoveryPeer   *peer_view;   /* arena, max_peers: the snapshot for dart_discovery_peers */
    DartAllocator       pool;        /* the open path's allocator, reset at close. Zeroed on the
                                         place path, where the caller owns the memory */
    DartDiscoveryAddr  seeds[DART_DISCOVERY_MAX_SEEDS];
    uint16_t             n_seeds;
};

static void i_dart_discovery_tx1(DartDiscovery *d, const uint8_t *out, size_t n_bytes,
                          const uint8_t ip[4], uint16_t port){
    i_dart_plat_send(d->tx_fd, out, n_bytes, ip, port);
}

/* One copy: the data port once the blob named one, else the discovery port. A peer whose
 * data port is unreachable should look dead. */
static void i_dart_discovery_tx_to(DartDiscovery *d, const uint8_t *out, size_t n_bytes,
                          const DartDiscoveryAddr *addr){
    if (addr->ip_len != 4) return;
    i_dart_discovery_tx1(d, out, n_bytes, addr->ip, addr->port ? addr->port : d->discovery_port);
}

/* An observed source exactly, else the locator. */
static void i_dart_discovery_tx_peer(DartDiscovery *d, const uint8_t *out, size_t n_bytes,
                          const DartDiscoveryAddr *addr, int kind){
    if (kind == 2){ if (addr->ip_len == 4) i_dart_discovery_tx1(d, out, n_bytes, addr->ip, addr->port); }
    else if (kind == 1) i_dart_discovery_tx_to(d, out, n_bytes, addr);
}

/* Out of every interface, so each copy carries the source correct for its path. A failed
 * sendto on one adapter does not stop the others. */
static void i_dart_discovery_tx_group(DartDiscovery *d, const uint8_t *out, size_t n_bytes){
    uint8_t group_ip[4], i;
    if (d->unicast_only) return;   /* seeds and known peers are the only paths */
    i_dart_plat_naddr_to_ip4(d->group_naddr, group_ip);
    if (!d->n_ifs){ i_dart_plat_send(d->fd, out, n_bytes, group_ip, d->discovery_port); return; }
    for (i = 0; i < d->n_ifs; i++){
        i_dart_plat_mcast_setif(d->fd, d->ifs[i].addr);
        i_dart_plat_send(d->fd, out, n_bytes, group_ip, d->discovery_port);
    }
}

/* The group, every seed and every known peer. skip_uuid leaves out a proxy's own origin,
 * whose self filter would only discard it. */
static void i_dart_discovery_tx(DartDiscovery *d, const uint8_t *out, size_t n_bytes,
                          const uint8_t *skip_uuid){
    uint16_t s, discovery_port = d->discovery_port;
    DartDiscoveryAddr addr;
    i_dart_discovery_tx_group(d, out, n_bytes);
    for (s=0; s<d->n_seeds; s++){
        const DartDiscoveryAddr *seed = &d->seeds[s];
        if (seed->ip_len != 4) continue;
        i_dart_discovery_tx1(d, out, n_bytes, seed->ip, seed->port ? seed->port : discovery_port);
    }
    for (s=0; s<d->max_peers; s++){
        int kind;
        if (skip_uuid){
            DartDiscoveryPeer v;
            if (dart_discovery_peer_at(d->core, s, &v) && memcmp(v.uuid, skip_uuid, 16)==0)
                continue;
        }
        kind = dart_discovery_peer_addr(d->core, s, &addr);
        if (kind) i_dart_discovery_tx_peer(d, out, n_bytes, &addr, kind);
    }
}

void dart_discovery_feed(DartDiscovery *d, const DartDiscoveryAddr *src, DartBytes datagram){
    if (!d) return;
    /* the data socket is the DATA channel */
    dart_discovery_on_datagram(d->core, src, DART_DISCOVERY_VIA_DATA, datagram, i_dart_plat_now_us());
}

void dart_discovery_set_tx_fd(DartDiscovery *d, i_DartSock fd){
    if (d) d->tx_fd = (fd == DART_SOCK_BAD) ? d->fd : fd;
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

/* The up, non loopback interfaces. Loopback is the fallback for a host with nothing else
 * up, never a member of the normal set. 0 only when the platform cannot enumerate. */
static uint8_t i_dart_discovery_if_scan(i_DartIface *out, uint8_t max){
    int n = i_dart_plat_local_ifaces(out, (int)max), i;
    uint8_t k = 0;
    for (i = 0; i < n; i++){
        uint8_t ip[4];
        i_dart_plat_naddr_to_ip4(out[i].addr, ip);
        if (!out[i].addr || ip[0] == 127) continue;   /* unspecified or loopback */
        out[k++] = out[i];
    }
    if (!k){                        /* nothing up, or a loopback only host */
        out[0].addr = i_dart_plat_ipv4(127u, 0u, 0u, 1u);
        out[0].mask = i_dart_plat_ipv4(255u, 0u, 0u, 0u);
        k = 1;
    }
    return k;
}

/* The netmask the host reports for addr, 0 if none. A pinned interface still ranks. */
static uint32_t i_dart_discovery_if_mask_of(uint32_t addr){
    i_DartIface all[DART_DISCOVERY_MAX_SUBNETS];
    int n = i_dart_plat_local_ifaces(all, DART_DISCOVERY_MAX_SUBNETS), i;
    for (i = 0; i < n; i++) if (all[i].addr == addr) return all[i].mask;
    return 0;
}

static int i_dart_discovery_if_has(const i_DartIface *set, uint8_t n, uint32_t addr){
    uint8_t i;
    for (i = 0; i < n; i++) if (set[i].addr == addr) return 1;
    return 0;
}

/* Leaves what went away, joins what appeared, and hands the core our subnets. A per
 * interface join failure is ordinary, so this returns how many memberships are live. */
static uint8_t i_dart_discovery_if_apply(DartDiscovery *d, const i_DartIface *want, uint8_t n_want){
    DartDiscoverySubnet nets[DART_DISCOVERY_MAX_SUBNETS];
    uint8_t i, joined = 0;
    if (n_want > DART_DISCOVERY_MAX_SUBNETS) n_want = DART_DISCOVERY_MAX_SUBNETS;
    if (d->unicast_only) joined = n_want;   /* no membership, but the core still gets our subnets */
    else {
        for (i = 0; i < d->n_ifs; i++)
            if (!i_dart_discovery_if_has(want, n_want, d->ifs[i].addr))
                i_dart_plat_mcast_leave(d->fd, d->group_naddr, d->ifs[i].addr);
        for (i = 0; i < n_want; i++){
            if (i_dart_discovery_if_has(d->ifs, d->n_ifs, want[i].addr)){ joined++; continue; }
            if (i_dart_plat_mcast_join(d->fd, d->group_naddr, want[i].addr)) joined++;
        }
    }
    for (i = 0; i < n_want; i++){
        i_dart_plat_naddr_to_ip4(want[i].addr, nets[i].ip);
        i_dart_plat_naddr_to_ip4(want[i].mask, nets[i].mask);
        d->ifs[i] = want[i];
    }
    d->n_ifs = n_want;
    dart_discovery_set_local_subnets(d->core, nets, n_want);
    return joined;
}

/* A link that comes up after open is joined, and one that goes away drops its membership. */
static void i_dart_discovery_if_refresh(DartDiscovery *d){
    i_DartIface want[DART_DISCOVERY_MAX_SUBNETS];
    uint8_t n_want = i_dart_discovery_if_scan(want, DART_DISCOVERY_MAX_SUBNETS);
    uint8_t i, same = (uint8_t)(n_want == d->n_ifs);
    if (same)
        for (i = 0; i < n_want; i++)
            if (want[i].addr != d->ifs[i].addr || want[i].mask != d->ifs[i].mask){ same = 0; break; }
    if (!same) i_dart_discovery_if_apply(d, want, n_want);
}

int dart_discovery_make_uuid4(uint8_t out[16]){
    if (!i_dart_plat_random(out, 16)) return 0;
    out[6] = (uint8_t)((out[6] & 0x0Fu) | 0x40u);  /* version 4 */
    out[8] = (uint8_t)((out[8] & 0x3Fu) | 0x80u);  /* variant 10x */
    return 1;
}

void i_dart_discovery_auto_uuid(uint8_t out[16]){
    char host[80]; uint64_t seed; size_t hostname_len;
    if (dart_discovery_make_uuid4(out)) return;

    /* no CSPRNG: hostname, pid and clock */
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
    memcpy(out, "node-", 5);                          /* 8 random hex digits, pid fallback */
    for (i = 0; i < 8; i++) out[5+i] = hex[(r >> ((7-i)*4)) & 0xF];
    out[13] = '\0';
    return 13;
}

/* The one arena layout: the struct, the wire scratch, the peer view, then the core. */
typedef struct {
    DartDiscovery *d;
    uint8_t *rxbuf, *txbuf, *peer_view, *core;
    size_t   wire_max, core_bytes;
} i_DartRtBlocks;
static void i_dart_discovery_rt_layout(i_DartBump *b, const DartDiscoveryCoreConfig *c, i_DartRtBlocks *o){
    uint16_t max_peers = c->max_peers ? c->max_peers : 32u;
    o->wire_max   = dart_discovery_wire_size(c->meta_cap);
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

/* Process globals with no lock, read right after a NULL return. The node builds its
   DART_ERROR from them. */
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
    uint8_t ttl, n_want, joined;
    uint32_t group_naddr;
    i_DartIface want[DART_DISCOVERY_MAX_SUBNETS];
    const char *group;

    if (!mem || !cfg) return NULL;
    g_place_error = DART_DISCOVERY_OK; g_place_os_error = 0;
    c = *cfg;
    dart_discovery_config_defaults(&c.discovery);
    /* a node with no multicast asks whoever hears it to announce it onward */
    if (c.unicast_only) c.discovery.relay_me = 1;
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
    memset(&d->pool, 0, sizeof d->pool); /* the caller owns mem, the open path overwrites this */
    core_mem     = blk.core;

    /* auto generate a uuid if the caller left it zero */
    for (i=0;i<16;i++) if (c.discovery.uuid[i]) { allzero = 0; break; }

    if (!i_dart_plat_startup()) return i_dart_discovery_fail(DART_DISCOVERY_E_PLATFORM, 0);
    if (allzero) i_dart_discovery_auto_uuid(c.discovery.uuid);

    d->core = dart_discovery_init(core_mem, cap - (size_t)(core_mem - (uint8_t*)mem), &c.discovery);
    if (!d->core){ i_dart_plat_cleanup(); return i_dart_discovery_fail(DART_DISCOVERY_E_MEMORY, 0); }

    fd = i_dart_plat_udp_open();
    if (fd == DART_SOCK_BAD){ int e=i_dart_plat_last_socket_error(); i_dart_plat_cleanup(); return i_dart_discovery_fail(DART_DISCOVERY_E_SOCKET, e); }
    if (!i_dart_plat_bind(fd, 0, c.discovery_port, 1)){ int e=i_dart_plat_last_socket_error(); i_dart_plat_close(fd); i_dart_plat_cleanup(); return i_dart_discovery_fail(DART_DISCOVERY_E_BIND, e); }

    /* Join and announce on every interface. A pinned interface means exactly that one. */
    group_naddr = i_dart_plat_parse_ip(group);
    d->fd = fd;
    d->tx_fd = fd;
    d->group_naddr = group_naddr;
    d->n_ifs = 0;
    d->unicast_only = c.unicast_only ? 1u : 0u;
    d->pinned = c.multicast_interface ? 1u : 0u;
    if (d->pinned){
        want[0].addr = i_dart_plat_parse_ip(c.multicast_interface);
        want[0].mask = i_dart_discovery_if_mask_of(want[0].addr);   /* 0 if it names no real one */
        n_want = 1;
    } else n_want = i_dart_discovery_if_scan(want, DART_DISCOVERY_MAX_SUBNETS);
    joined = i_dart_discovery_if_apply(d, want, n_want);
    if (!d->unicast_only){
        if (!joined && !(n_want == 1 && want[0].addr == 0)){
            i_DartIface any; any.addr = 0; any.mask = 0;      /* let the OS choose */
            joined = i_dart_discovery_if_apply(d, &any, 1);
        }
        if (!joined){
            int e=i_dart_plat_last_socket_error();
            i_dart_plat_close(fd); i_dart_plat_cleanup(); return i_dart_discovery_fail(DART_DISCOVERY_E_MCAST_JOIN, e);
        }
        i_dart_plat_mcast_ttl(fd, ttl);
        /* loop stays on for several instances per host, the uuid filter drops the echoes */
        i_dart_plat_mcast_loop(fd, 1);
    }   /* unicast_only sets no group option, so a stack with no multicast opens */
    i_dart_plat_set_nonblock(fd);   /* the drain's last recv must return would block */
    i_dart_plat_suppress_connreset(fd);   /* a dead peer's ICMP bounce must not disrupt RX */

    d->unicast_fd = DART_SOCK_BAD;
    d->if_scan_us = i_dart_plat_now_us() + DART_DISCOVERY_IF_SCAN_US;
    d->discovery_port = c.discovery_port;
    d->max_peers = c.discovery.max_peers;
    d->n_seeds = 0;
    if (c.seeds){
        uint16_t k, seed_count = c.n_seeds;
        if (seed_count > DART_DISCOVERY_MAX_SEEDS) seed_count = DART_DISCOVERY_MAX_SEEDS;
        for (k=0;k<seed_count;k++) d->seeds[k] = c.seeds[k];
        d->n_seeds = seed_count;
    }

    /* No locator (a discovery only instance): bind an own unicast RX port and advertise it,
       so a same host reply reaches this process, not the shared port's arbitrary owner. */
    if (c.discovery.data_port == 0){
        i_DartSock uc = i_dart_plat_udp_open();
        if (uc != DART_SOCK_BAD){
            uint16_t uport = i_dart_plat_bind(uc, 0, 0, 0) ? i_dart_plat_local_port(uc) : 0;
            if (uport){
                i_dart_plat_set_nonblock(uc);
                i_dart_plat_suppress_connreset(uc);   /* same as above */
                d->unicast_fd = uc;
                d->tx_fd = uc;   /* a reply to the arrival source then reaches this process */
                dart_discovery_set_data_port(d->core, uport);
            } else i_dart_plat_close(uc);
        }
    }
    return d;
}

/* The defaults first constructor. No automatic growth, a full peer table refuses. */
DartDiscovery *dart_discovery_open(DartAllocator *alloc, const char *name, const DartDiscoveryConfig *cfg){
    DartDiscoveryNetConfig nc; DartDiscoveryConfig o; DartDiscovery *d;
    char namebuf[DART_DISCOVERY_NAME_MAX + 1]; uint8_t namelen;
    void *block; size_t need; DartAllocator pool;
    if (!alloc) return NULL;
    memset(&o, 0, sizeof o); if (cfg) o = *cfg;
    namelen = dart_discovery_default_name(namebuf, sizeof namebuf, name);   /* auto if NULL */

    memset(&nc, 0, sizeof nc);
    nc.discovery.domain_id     = o.domain;
    nc.discovery.max_peers     = o.max_peers;          /* 0 = default */
    nc.discovery.meta_cap = o.meta_cap;
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
    nc.unicast_only        = o.unicast_only;

    need = dart_discovery_placement_memory(&nc);
    pool = *alloc;                                 /* copied, the caller's may be a temporary */
    block = dart_allocator_alloc(&pool, NULL, need);
    if (!block) return i_dart_discovery_fail(DART_DISCOVERY_E_MEMORY, 0);
    d = dart_discovery_place(block, need, &nc);
    if (!d){ DartAllocator p = pool; dart_allocator_reset(&p); return NULL; }
    d->pool = pool;                                /* the block's pool, close resets it */
    return d;
}

/* The struct copy keeps the socket, group and seeds. The core is migrated and self_meta
 * re pointed. The caller frees the old block after. */
DartDiscovery *dart_discovery_migrate(DartDiscovery *old, void *new_mem, size_t new_cap,
        uint16_t new_max_peers, uint16_t new_meta_cap, const uint8_t *self_meta, void *peer_cb_user){
    DartDiscoveryCoreConfig dc; i_DartRtBlocks blk; i_DartBump b; DartDiscovery *d;
    DartDiscoveryState *nc; uint8_t *base; size_t need;
    if (!old) return NULL;
    dc = old->core->cfg; dc.max_peers = new_max_peers; dc.meta_cap = new_meta_cap;
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
             new_cap - (size_t)(blk.core - (uint8_t*)new_mem), new_max_peers, new_meta_cap,
             self_meta, peer_cb_user);
    if (!nc) return NULL;                /* old left intact, the caller frees the new block */
    d->core = nc;
    return d;
}

/* Drains to empty, capped by the burst guard. One recv per poll would let a slow poller's
 * backlog keep a dead peer alive. */
#define DART_DISCOVERY_RX_BURST 2048
static int i_dart_discovery_rt_drain(DartDiscovery *d, i_DartSock fd, DartDiscoveryVia via){
    int got = 0, guard;
    for (guard = 0; guard < DART_DISCOVERY_RX_BURST; guard++){
        DartDiscoveryAddr src;
        uint8_t src_ip[4]; uint16_t src_port = 0;
        int n = i_dart_plat_recv(fd, d->rxbuf, d->wire_max, src_ip, &src_port);
        if (n < 0){ if (i_dart_plat_would_block()) break; continue; }  /* empty or transient */
        if (n > 0){
            memset(&src, 0, sizeof src);
            memcpy(src.ip, src_ip, 4); src.ip_len = 4; src.port = src_port;
            dart_discovery_on_datagram(d->core, &src, via, dart_bytes(d->rxbuf, (size_t)n), i_dart_plat_now_us());
            got = 1;
        }
    }
    return got;
}

/* The poll body without the wait. The node folds discovery into its one wait and calls
 * this every pass. */
int dart_discovery_service(DartDiscovery *d, int fd_readable, int unicast_readable){
    DartDiscoveryAddr to;
    uint64_t now;
    int got = 0, exact; size_t n_bytes;
    if (!d) return 0;
    if (fd_readable) got |= i_dart_discovery_rt_drain(d, d->fd, DART_DISCOVERY_VIA_DISCOVERY);
    /* our own unicast port is the port we advertise, so it is the DATA channel */
    if (unicast_readable && d->unicast_fd != DART_SOCK_BAD)
        got |= i_dart_discovery_rt_drain(d, d->unicast_fd, DART_DISCOVERY_VIA_DATA);

    now = i_dart_plat_now_us();
    if (!d->pinned && now >= d->if_scan_us){    /* pick up an interface that came up since open */
        d->if_scan_us = now + DART_DISCOVERY_IF_SCAN_US;
        i_dart_discovery_if_refresh(d);
    }

    n_bytes = dart_discovery_update(d->core, now, d->txbuf, d->wire_max);
    if (n_bytes) i_dart_discovery_tx(d, d->txbuf, n_bytes, NULL);

    /* targeted unicast: replies to soliciters and re fetch requests */
    while ((n_bytes = dart_discovery_poll_targeted(d->core, d->txbuf, d->wire_max, &to, &exact)) != 0)
        i_dart_discovery_tx_peer(d, d->txbuf, n_bytes, &to, exact ? 2 : 1);

    /* relay: proxied announces for peers that cannot multicast, out of every path. The
       origin itself is skipped, bytes 8 to 23 carry its uuid. */
    while ((n_bytes = dart_discovery_poll_relay(d->core, d->txbuf, d->wire_max)) != 0)
        i_dart_discovery_tx(d, d->txbuf, n_bytes, d->txbuf + 8);

    /* introductions: proxied announces of the peers we hear directly, unicast to one relay
       me peer, which behind a NAT must always speak first */
    while ((n_bytes = dart_discovery_poll_introduce(d->core, d->txbuf, d->wire_max, &to, &exact)) != 0)
        i_dart_discovery_tx_peer(d, d->txbuf, n_bytes, &to, exact ? 2 : 1);
    return got;
}

int dart_discovery_poll(DartDiscovery *d, int timeout_ms){
    i_DartPollfd pfd[2];
    int nfds = 1;
    memset(pfd, 0, sizeof pfd);
    pfd[0].fd = d->fd; pfd[0].events = DART_POLLIN;
    if (d->unicast_fd != DART_SOCK_BAD){ pfd[1].fd = d->unicast_fd; pfd[1].events = DART_POLLIN; nfds = 2; }
    if (i_dart_plat_poll(pfd, nfds, timeout_ms) < 0) return -1;
    return dart_discovery_service(d, (pfd[0].revents & DART_POLLIN) != 0,
                                  nfds == 2 && (pfd[1].revents & DART_POLLIN) != 0);
}

int dart_discovery_gather(DartDiscovery *d, int quiet_ms, int timeout_ms){
    uint64_t start, last_change, last_solicit = 0;
    uint16_t count;
    if (!d) return 0;
    start = i_dart_plat_now_us(); last_change = start;
    count = dart_discovery_peer_count(d->core);
    for (;;){
        uint64_t now = i_dart_plat_now_us(); uint16_t c;
        if (now - last_solicit >= 250000u){     /* re solicit 4 times a second */
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
        for (k = 0; n_bytes && k < DART_DISCOVERY_BYE_SENDS; k++) i_dart_discovery_tx(d, d->txbuf, n_bytes, NULL);
    }
    dart_discovery_destroy(d->core);   /* may mutate the pool, so it runs before the copy out */
    i_dart_plat_close(d->fd);
    if (d->unicast_fd != DART_SOCK_BAD) i_dart_plat_close(d->unicast_fd);
    i_dart_plat_cleanup();
    pool = d->pool;                /* copy out last: the reset frees the block holding d */
    dart_allocator_reset(&pool);   /* the place path has an empty pool, a no op */
}
#pragma endregion
#endif /* !DART_DISCOVERY_SANS_IO */
#endif /* DART_DISCOVERY_IMPLEMENTATION */
