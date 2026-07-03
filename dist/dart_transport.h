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
#endif

#ifndef DART_TRANSPORT_SANS_IO
  #if defined(DART_TRANSPORT_IMPLEMENTATION) && !defined(DART_DISCOVERY_IMPLEMENTATION)
  #define DART_DISCOVERY_IMPLEMENTATION
  #endif
  #include "dart_discovery.h"   /* discovery: needed by the node runtime */
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
 * dart_allocator_reset frees EVERYTHING (both intents), so no per-allocation free is ever
 * required; free a freeable block only to reclaim it mid-run. In static mode there are no
 * pages, so both intents just bump the buffer, free is a no-op, and reset rewinds. All
 * static-inline: like arena.h, the amalgamator emits it once and a layer that does not use
 * it pays nothing. */

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
    uint32_t    page_size;
    size_t      max_bytes;      /* dynamic runaway guard (0 = unlimited) */
    size_t      in_use, peak;
    uint64_t    alloc_calls, pages_live;
} DartAllocator;

static inline size_t i_dart_allocator_align(size_t n){ return (n + 15u) & ~(size_t)15u; }
static inline size_t i_dart_allocator_pow2(size_t n){                 /* round up to a power of two, >= 16 */
    size_t p = 16u;
    while (p < n){ if (p > (SIZE_MAX >> 1)) return n; p <<= 1; }
    return p;
}
/* dynamic runaway guard: 1 if `need` more bytes would breach max_bytes */
static inline int i_dart_allocator_over(const DartAllocator *a, size_t need){
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
static inline void *i_dart_allocator_bump(DartAllocator *a, size_t need){
    i_DartPage *pg = a->shared;
    need = i_dart_allocator_align(need);
    if (!pg || i_dart_allocator_align(pg->used) + need > pg->cap){
        size_t psz; i_DartPage *np;
        if (!a->page_realloc || i_dart_allocator_over(a, need)) return NULL;   /* static full, or over the guard */
        psz = a->page_size;
        if (need + sizeof(i_DartPage) > psz) psz = need + sizeof(i_DartPage);   /* oversized page */
        np = (i_DartPage *)a->page_realloc(NULL, psz);
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

/* a freeable block of `cap` payload bytes: its own page (dynamic) or a header+payload bumped
 * from the buffer (static, where free is a no-op). */
static inline void *i_dart_allocator_new_owned(DartAllocator *a, size_t cap){
    i_DartPage *pg;
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

static inline void i_dart_allocator_free_owned(DartAllocator *a, void *ptr){
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
    if (size == 0){ if (ptr) i_dart_allocator_free_owned(a, ptr); return NULL; }
    cap = a->page_realloc ? i_dart_allocator_pow2(size) : i_dart_allocator_align(size);   /* pow2 dynamic, tight static */
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
#pragma endregion
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

/* The zero-fragment same-host shared-memory path is ON by default. Opt out with
 * DART_NO_SHM for a slimmer build or a target without shm_open/mmap (on POSIX, link
 * -lrt on older glibc). It is used only between same-host nodes that set an allocator;
 * a node with no local SHM peer creates no segment and pays nothing at runtime. */
#if !defined(DART_SHM) && !defined(DART_NO_SHM)
#define DART_SHM
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

/* Max pub+sub topic count accepted in a peer's interest list; sizes the per-peer
 * alias table. Auto-raised to 2*n_channels; raise (a compile bound) only to accept
 * a peer with more topics. */
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
    DART_TRANSPORT_NAME_COLLISION,  /* a peer's name hashes to ours but differs (.identity, .detail = our name), refused */
    DART_TRANSPORT_QOS_INCOMPATIBLE,/* a reliable subscriber refused a best-effort publisher (.channel, .peer); .detail = our channel name */
    DART_TRANSPORT_SCHEMA_MISMATCH  /* the schema_check hook refused a match (.channel, .peer); .detail = our channel name */
} DartTransportEventKind;

typedef struct {
    DartTransportEventKind kind;
    const char *detail;        /* short human label (NAME_COLLISION / QOS_INCOMPATIBLE: our channel name) */
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
    DART_ERR_OOM        = -4   /* dynamic allocator returned NULL */
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

/* The node's app-facing event: the union the user receives via DartNodeOpts.on_event.
 * The node maps discovery's DartDiscoveryEvent (the peer kinds) and the transport's
 * DartTransportEvent (the message/QoS kinds) into this one type, and adds its own
 * (PEER_INTEREST). Flat and self-describing: read only the fields named for the .kind.
 * dart_event_str formats any of them as a one-line message. */
typedef enum {
    DART_PEER_UP,        /* peer discovered or resumed: .peer, .ip/.ip_len/.port */
    DART_PEER_DOWN,      /* peer lost or fell silent: .peer */
    DART_PEER_INTEREST,  /* a peer's interest list was (re)applied: .peer, .publish_topics, .receive_topics */
    DART_MSG_LOST,       /* messages skipped: .channel, .peer, .lost_first .. +.lost_count-1 */
    DART_MSG_TOO_BIG,    /* a received message exceeded max_message_bytes (.too_big_bytes), skipped */
    DART_NAME_COLLISION, /* a peer's name hashes to ours but differs (.identity, .detail = our name), refused */
    DART_QOS_INCOMPATIBLE, /* a reliable subscriber refused a best-effort publisher (.channel, .peer); .detail = our channel name */
    DART_SCHEMA_MISMATCH,  /* incompatible schemas: a match was refused, or a message that did not
                              fit its sender's schema was dropped (.channel, .peer; .detail = our channel name) */
    DART_PEER_REFUSED    /* peer table full of active peers: a new peer was refused (.ip/.ip_len/.port) */
} DartEventKind;

typedef struct {
    DartEventKind kind;
    const char *detail;        /* short human-readable label (NAME_COLLISION / QOS_INCOMPATIBLE: our channel name) */
    void       *user;          /* your DartNodeOpts.user_data (mirrors DartMsg.user) */
    uint32_t   peer;           /* peer id, where applicable (0 = n/a) */
    uint16_t   channel;        /* local channel handle, where applicable */
    uint8_t    ip[16];         /* PEER_UP / PEER_REFUSED: peer address (network order) */
    uint8_t    ip_len;         /* PEER_UP / PEER_REFUSED: 4 or 16; else 0 */
    uint16_t   port;           /* PEER_UP / PEER_REFUSED: peer data port */
    uint64_t   lost_first;     /* MSG_LOST: first skipped seqno */
    uint64_t   lost_count;     /* MSG_LOST: number of messages skipped */
    uint64_t   too_big_bytes;  /* MSG_TOO_BIG: size of the dropped message */
    uint64_t   identity;       /* NAME_COLLISION: the colliding 64-bit topic identity */
    uint16_t   publish_topics; /* PEER_INTEREST: topics we now publish to this peer */
    uint16_t   receive_topics; /* PEER_INTEREST: topics we now receive from this peer */
} DartEvent;
typedef void (*DartEventFn)(const DartEvent *ev);

/* Format ev as a one-line human-readable message into buf (always NUL-terminated,
 * truncated to cap). Returns buf. */
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

size_t          i_dart_node_core_required_memory(uint16_t n_channels);
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
 * lookup (never on the wire). Do not call back into dart_* from the callback. */
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
int          dart_node_poll(DartNode *n, int timeout_ms);          /* one loop tick */
void         dart_node_close(DartNode *n, int send_bye);

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
/* Pump until every reader has acked all messages on this channel, or timeout_ms elapses.
 * Returns 1 if drained, 0 on timeout. Call before close so a burst isn't cut by the BYE. */
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

typedef struct {
    uint8_t  valid;
    uint64_t base;       /* seqno of frag 0 */
    uint16_t count;      /* frag count */
    uint32_t len;        /* message bytes */
    uint8_t *buf;        /* >= len bytes; arena (fixed) or hook-malloc'd (dynamic) */
    uint32_t cap;        /* allocated bytes of buf (dynamic grows it) */
#ifdef DART_SHM
    uint8_t  shm;        /* 1 = SHM-backed: bytes live in shm_buf, desc set, buf unused */
    const uint8_t *shm_buf;            /* external chunk payload (remote peers fragment from it) */
    uint8_t  desc[DART_SHM_DESC_BYTES];/* descriptor sent to SHM peers as one SHM-DATA */
#endif
} i_DartWriterSample;

typedef struct {        /* writer-side, per (channel,peer) */
    uint8_t  used;
    uint8_t  reader_reliable; /* the matched reader requested RELIABLE: only then does this
                                 lane impose backpressure + heartbeats. A best-effort reader
                                 never acks, so it must stay out of flow control (fire-and-
                                 forget), else it stalls a reliable writer forever. */
    uint32_t reader_epoch; /* reader incarnation from last ACKNACK (0 = none); a change
                              means the peer rebuilt state, so the lane re-joins */
    uint64_t sent_upto;  /* next seqno to push as new data */
    uint64_t acked_upto; /* peer received all TUs < this */
    uint8_t  has_nack;   /* pending repair request from ACKNACK */
    uint64_t nack_base;
    uint32_t nack_bits;
    uint64_t hb_next_us; /* heartbeat timer */
    uint32_t hb_count;
} i_DartWriterProxy;

typedef struct {        /* reader-side, per (channel,peer) */
    uint8_t  used;
    uint8_t  started;       /* accepted any DATA from this writer yet */
    uint32_t epoch;         /* this incarnation's id, sent in every ACKNACK */
    uint64_t deliver_upto;  /* base of current sample; all below delivered/skipped */
    uint8_t  assembly_active;    /* received >=1 frag of current sample */
    uint16_t assembly_count;
    uint16_t assembly_low;        /* lowest still-missing frag index of current sample (its
                               contiguous-received front); deliver_upto+assembly_low = first hole */
    uint32_t assembly_len;
    uint8_t *assembly_buf;       /* >= assembly_len; arena (fixed) or hook-malloc'd (dynamic) */
    uint8_t *frag_bitmap;       /* ceil(assembly_count/8) */
    uint32_t assembly_cap;       /* allocated bytes of assembly_buf (dynamic grows it) */
    uint32_t bitmap_cap;        /* allocated bytes of frag_bitmap */
    uint64_t hb_last;       /* highest seqno the writer CLAIMS to hold (heartbeat only) */
    uint64_t received_high;      /* highest seqno we have actually RECEIVED a frag for. UDP is
                               assumed in-order, so a hole below this is real loss to repair
                               while anything above it is still in flight: never NACK past it. */
    uint64_t nack_high;       /* highest seqno already requested this episode; refills ask only
                               (nack_high, top] so in-flight repairs are not re-requested */
    uint64_t nack_retransmit_us;  /* earliest time to re-request a stalled floor (lost-repair backstop) */
    uint8_t  ack_force;     /* a delivery/skip/HB/(re)match owes the writer an ACKNACK even if
                               the repair floor did not move (avoids a stuck cumulative ack) */
    uint8_t  ack_pending;
    uint64_t ack_due_us;
#ifdef DART_SHM
    uint8_t  shm_fail;      /* consecutive SHM-DATA resolve failures at deliver_upto */
#endif
} i_DartReaderProxy;

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
       alias instead of the topic name */
    uint16_t    *alias_to_channel;    /* [max_peers * alias_max]; 0xFFFF = unmapped */
    uint32_t     alias_max;        /* alias-table stride = effective meta_max_ids */
    i_DartChannel  *channels;     /* [n_channels] */
    i_DartWriterProxy   *writer_proxies;     /* [n_channels*max_peers] */
    i_DartReaderProxy   *reader_proxies;     /* [n_channels*max_peers] */
    /* active-lane scheduler: a lane is one (channel,peer) pair. The event that gives a
       lane work enqueues it, so poll_send pays for work done, not idle lanes. Timer work
       is found by an amortized clock-driven sweep. */
    uint32_t    *lane_next;   /* [n_channels*max_peers] next in dest list */
    uint8_t     *lane_queued;    /* [n_channels*max_peers] queued flag */
    uint32_t    *dest_head;   /* [max_peers] lane list per dest */
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

/* writer/reader proxy for a (channel,peer) lane. The proxies are
   [n_channels][max_peers] row-major; centralizing the index math here keeps a
   transposed channel/peer from silently corrupting a neighbor lane. */
static inline i_DartWriterProxy *i_dart_writer_proxy_at(DartTransportState *st, uint16_t channel_idx, uint32_t peer_slot){
    return &st->writer_proxies[(size_t)channel_idx*st->cfg.max_peers + peer_slot];
}
static inline i_DartReaderProxy *i_dart_reader_proxy_at(DartTransportState *st, uint16_t channel_idx, uint32_t peer_slot){
    return &st->reader_proxies[(size_t)channel_idx*st->cfg.max_peers + peer_slot];
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
void   i_dart_transport_fire_event(DartTransportState *st, DartTransportEventKind kind, uint16_t channel, uint32_t peer, uint64_t first, uint64_t count, const char *detail);
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


/* scheduler: lane index = channel_idx*max_peers + peer_slot; destination = peer slot */
static void i_dart_dest_push(DartTransportState *st, uint32_t d){
    uint32_t ndest = st->cfg.max_peers, t;
    if (st->dest_queued[d]) return;
    st->dest_queued[d]=1;
    t = st->dest_queue_head + st->dest_queue_count;
    if (t >= ndest) t -= ndest;
    st->dest_queue[t]=d; st->dest_queue_count++;
}


/* enqueue a lane that just got sendable work; idempotent while queued */
static void i_dart_lane_enq(DartTransportState *st, uint16_t channel_idx, uint32_t peer_slot){
    uint32_t max_peers=st->cfg.max_peers;
    uint32_t lane=(uint32_t)channel_idx*max_peers+peer_slot;
    uint32_t d=peer_slot;
    if (st->lane_queued[lane]) return;
    st->lane_queued[lane]=1; st->lane_next[lane]=DART__NIL;
    if (st->dest_head[d]==DART__NIL) st->dest_head[d]=lane;
    else st->lane_next[st->dest_tail[d]]=lane;
    st->dest_tail[d]=lane;
    i_dart_dest_push(st, d);
}


/* enqueue, and track a freshly-armed reader ack/NACK deadline for the poll cap. Used
 * by the arm sites (ack_due_us is future or 0); the sweep enqueues due lanes with
 * i_dart_lane_enq instead, since it recomputes next_deadline itself. */
void i_dart_lane_wake(DartTransportState *st, uint16_t channel_idx, uint32_t peer_slot){
    i_DartReaderProxy *r=i_dart_reader_proxy_at(st,channel_idx,peer_slot);
    if (r->used && r->ack_pending) i_dart_transport_arm_deadline(st, r->ack_due_us);
    i_dart_lane_enq(st, channel_idx, peer_slot);
}


/* sendable work a popped lane still owes now (timer-armed work is the sweep's job) */
static int i_dart_lane_work(DartTransportState *st, uint16_t channel_idx, uint32_t peer_slot, uint64_t now){
    i_DartChannel *ch=&st->channels[channel_idx];
    if (!st->peer_used[peer_slot] || st->peer_dormant[peer_slot]) return 0;   /* dormant: out of flow control */
    { i_DartWriterProxy *w=i_dart_writer_proxy_at(st,channel_idx,peer_slot);
      i_DartReaderProxy *r=i_dart_reader_proxy_at(st,channel_idx,peer_slot);
      if (w->used && w->has_nack) return 1;
      if (w->used && w->sent_upto < ch->next_seqno) return 1;
      if (r->used && ch->qos.reliability==DART_RELIABLE
          && r->ack_pending && now >= r->ack_due_us) return 1;
    }
    return 0;
}


/* clock-driven counterpart of the wake calls: a cursor walks the lane table waking
 * lanes whose timers came due. Two triggers: the amortized backstop (full coverage
 * every DART_HB_SWEEP_US) and a forced full pass when next_deadline_us comes due, so
 * a deadline-capped poll that wakes for a timer actually services it. A full pass
 * also recomputes next_deadline_us exactly (the global min of not-yet-due timers).
 * Read-only; cost is bounded by table size. */
static void i_dart_hb_sweep(DartTransportState *st, uint64_t now){
    uint32_t max_peers=st->cfg.max_peers;
    uint32_t total=(uint32_t)st->cfg.n_channels*max_peers, due, k;
    uint64_t span = now - st->sweep_time_us;
    int forced = (now >= st->next_deadline_us);    /* a tracked timer is due */
    int full;
    uint64_t mind = DART__NO_DEADLINE;             /* earliest not-yet-due timer seen */
    due = (forced || span >= DART_HB_SWEEP_US) ? total
        : (uint32_t)(span * total / DART_HB_SWEEP_US);
    if (!due) return;              /* sweep_time_us advances only when lanes are paid */
    full = (due >= total);         /* covered every lane -> mind is the global minimum */
    st->sweep_time_us = now;
    for (k=0;k<due;k++){
        uint32_t lane=st->sweep, peer_slot=lane%max_peers;
        uint16_t channel_idx=(uint16_t)(lane/max_peers);
        i_DartChannel *ch=&st->channels[channel_idx];
        st->sweep = (st->sweep+1u>=total) ? 0u : st->sweep+1u;
        /* gate writer heartbeats on next_seqno, never the reader ack: a sub-only
           node's data channels never advance next_seqno but still owe acks */
        if (ch->qos.reliability!=DART_RELIABLE) continue;
        if (!st->peer_used[peer_slot] || st->peer_dormant[peer_slot]) continue;   /* dormant: out of flow control */
        { i_DartWriterProxy *w=i_dart_writer_proxy_at(st,channel_idx,peer_slot);
          i_DartReaderProxy *r=i_dart_reader_proxy_at(st,channel_idx,peer_slot);
          if (w->used && w->reader_reliable && w->acked_upto < ch->next_seqno){
              if (now>=w->hb_next_us) i_dart_lane_enq(st,channel_idx,peer_slot);
              else if (w->hb_next_us < mind) mind = w->hb_next_us;
          }
          if (r->used && r->ack_pending){
              if (now>=r->ack_due_us) i_dart_lane_enq(st,channel_idx,peer_slot);
              else if (r->ack_due_us < mind) mind = r->ack_due_us;
          }
        }
    }
    /* a full pass saw every timer: mind is the exact next deadline. Lanes woken above
       re-arm during emit (i_dart_transport_arm_deadline) and re-lower it; reader acks just clear. */
    if (full) st->next_deadline_us = mind;
}


int dart_transport_poll_send(DartTransportState *st, uint32_t *to_peer, void *out, size_t cap, size_t *out_len, uint64_t now){
    uint32_t max_peers=st->cfg.max_peers, ndest=max_peers;
    i_dart_hb_sweep(st, now);
    while (st->dest_queue_count){
        uint32_t d; size_t offset=0;
        d = st->dest_queue[st->dest_queue_head];
        st->dest_queue_head = (st->dest_queue_head+1u>=ndest) ? 0u : st->dest_queue_head+1u;
        st->dest_queue_count--; st->dest_queued[d]=0;
        /* drain this destination's lanes into one datagram */
        while (st->dest_head[d]!=DART__NIL){
            uint32_t lane=st->dest_head[d], peer_slot=lane%max_peers;
            uint16_t channel_idx=(uint16_t)(lane/max_peers);
            size_t n;
            do {
                /* acks first: small, one-shot, and carry the NACKs that drive
                   repair, so a backlogged writer can't starve them */
                n=i_dart_reader_emit(st,(int)channel_idx,(int)peer_slot,(uint8_t*)out+offset,cap-offset,now);
                if (!n) n=i_dart_writer_emit(st,(int)channel_idx,(int)peer_slot,(uint8_t*)out+offset,cap-offset,now);
                offset+=n;
            } while (n && offset<cap);
            st->dest_head[d]=st->lane_next[lane];
            if (i_dart_lane_work(st,channel_idx,peer_slot,now)){
                /* datagram full mid-lane: rotate the lane to the back so siblings get the next */
                if (st->dest_head[d]==DART__NIL) st->dest_head[d]=lane;
                else {
                    st->lane_next[lane]=DART__NIL;
                    st->lane_next[st->dest_tail[d]]=lane;
                    st->dest_tail[d]=lane;
                }
                break;
            }
            st->lane_queued[lane]=0;     /* lane drained */
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
    { uint32_t max_peers=st->cfg.max_peers, p;
      for (p=0;p<max_peers;p++)
          if (i_dart_writer_proxy_at(st,channel_idx,p)->used && !st->peer_dormant[p]) i_dart_lane_wake(st, channel_idx, p);
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
    i_DartWriterSample *slot; uint32_t max_peers; uint16_t p;
    if (!ch || ch->qos.reliability != DART_RELIABLE) return 0;
    slot = &ch->history[ch->history_head];        /* slot the next send overwrites */
    if (!slot->valid) return 0;
    max_peers = st->cfg.max_peers;
    for (p=0;p<(uint16_t)max_peers;p++){
        i_DartWriterProxy *w=i_dart_writer_proxy_at(st,channel_idx,p);
        if (w->used && w->reader_reliable && !st->peer_dormant[p] && w->acked_upto < slot->base + slot->count) return 1;
    }
    return 0;
}


int dart_transport_send_drained(DartTransportState *st, uint16_t channel){
    int channel_idx; i_DartChannel *ch = i_dart_channel_at(st, channel, &channel_idx);
    uint32_t max_peers; uint16_t p;
    if (!ch || ch->qos.reliability != DART_RELIABLE) return 1;  /* no acks to await */
    max_peers = st->cfg.max_peers;
    for (p=0;p<(uint16_t)max_peers;p++){
        i_DartWriterProxy *w=i_dart_writer_proxy_at(st,channel_idx,p);
        if (w->used && w->reader_reliable && !st->peer_dormant[p] && w->acked_upto < ch->next_seqno) return 0;  /* reliable reader still behind */
    }
    return 1;
}


int dart_transport_writer_match_count(DartTransportState *st, uint16_t channel){
    i_DartChannel *ch = i_dart_channel_at(st, channel, NULL);
    return ch ? (int)ch->matched_writers : 0;   /* cached at match/unmatch, so O(1) */
}


int dart_transport_repair_pending(DartTransportState *st, uint16_t channel){
    int channel_idx; i_DartChannel *ch = i_dart_channel_at(st, channel, &channel_idx);
    uint32_t max_peers, p; int cnt = 0;
    if (!ch) return 0;
    max_peers = st->cfg.max_peers;
    for (p=0;p<max_peers;p++){ i_DartWriterProxy *w=i_dart_writer_proxy_at(st,channel_idx,p); if (w->used && w->has_nack) cnt++; }
    return cnt;   /* writer lanes with a NACK to service; 0 = nothing to resend right now */
}

#ifdef DART_SHM

/* 1 if the channel has >=1 matched reader and EVERY matched (non-dormant) reader is
 * SHM-capable -> the node may publish this message via SHM. One non-SHM (remote)
 * reader forces inline UDP for the whole message. */
int dart_transport_writer_shm_eligible(DartTransportState *st, uint16_t channel){
    int channel_idx; i_DartChannel *ch = i_dart_channel_at(st, channel, &channel_idx);
    uint32_t max_peers, p; int any=0;
    if (!ch) return 0;
    max_peers = st->cfg.max_peers;
    for (p=0;p<max_peers;p++){
        if (!i_dart_writer_proxy_at(st,channel_idx,p)->used || st->peer_dormant[p]) continue;
        if (!st->peer_shm[p]) return 0;
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
    if (!w->used) return;
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
    if (!w->used || st->peer_dormant[peer_slot]) return 0;   /* dormant: out of flow control */

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
    if (!r->used || !r->assembly_active) return 0;            /* no message mid-reassembly */
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
                        r->deliver_upto, base - r->deliver_upto, "message(s) lost");
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
    if (!r->used || count==0) return;
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
                        base, count, "SHM descriptor unresolvable (check DART_SHM_* build constants)");
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

    if (!r->used){ ch->repair_stats.frags_malformed++; return; }     /* not subscribed */
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
                      0, sample_len, "message exceeds max_message_bytes");
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
    if (!r->used) return;
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
                    r->deliver_upto, first - r->deliver_upto, "message(s) lost");
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
    if (!r->used || st->peer_dormant[peer_slot]) return 0;   /* dormant: don't ack a silent writer */
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
      i_DartWriterProxy *writer_proxies = (i_DartWriterProxy*)i_dart_bump_take(b, (size_t)n_channels*max_peers*sizeof(i_DartWriterProxy), 16);
      i_DartReaderProxy *reader_proxies = (i_DartReaderProxy*)i_dart_bump_take(b, (size_t)n_channels*max_peers*sizeof(i_DartReaderProxy), 16);
      uint32_t *lane_next = (uint32_t*)i_dart_bump_take(b, (size_t)nlanes*sizeof(uint32_t), 8);
      uint8_t  *lane_queued = (uint8_t*) i_dart_bump_take(b, (size_t)nlanes, 1);
      uint32_t *dest_head = (uint32_t*)i_dart_bump_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint32_t *dest_tail = (uint32_t*)i_dart_bump_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint8_t  *dest_queued = (uint8_t*) i_dart_bump_take(b, (size_t)ndest, 1);
      uint32_t *dest_queue = (uint32_t*)i_dart_bump_take(b, (size_t)ndest*sizeof(uint32_t), 8);
      uint16_t *alias_to_channel = (uint16_t*)i_dart_bump_take(b, (size_t)max_peers*meta_ids*sizeof(uint16_t), 2);
      name_pool = (char*)i_dart_bump_take(b, name_bytes ? name_bytes : 1u, 1);
      if (st && b->base){
          st->cfg=*cfg; st->peer_ids=peer_ids; st->peer_used=peer_used;
          st->peer_dormant=peer_dormant; st->peer_frag=peer_frag;
          st->frag = dart_clamp_frag(cfg->frag_payload);
          st->peer_pub_bitmap=peer_pub_bitmap; st->peer_sub_bitmap=peer_sub_bitmap;
          st->peer_sub_reliable=peer_sub_reliable; st->bitmap_len=bitmap_len;
          st->channels=ch; st->writer_proxies=writer_proxies; st->reader_proxies=reader_proxies; st->reader_epoch_counter=1;
          st->next_deadline_us=DART__NO_DEADLINE;
          st->lane_next=lane_next; st->lane_queued=lane_queued;
          st->dest_head=dest_head; st->dest_tail=dest_tail; st->dest_queued=dest_queued; st->dest_queue=dest_queue;
          st->alias_to_channel=alias_to_channel; st->alias_max=meta_ids;
          memset(peer_used,0,max_peers); memset(peer_dormant,0,max_peers);
          { uint32_t k; for (k=0;k<max_peers;k++) peer_frag[k]=DART_FRAG_PAYLOAD; }  /* set per peer on add */
#ifdef DART_SHM
          st->peer_shm=peer_shm; memset(peer_shm,0,max_peers);
#endif
          memset(alias_to_channel,0xFF,(size_t)max_peers*meta_ids*sizeof(uint16_t));   /* all unmapped */
          memset(peer_pub_bitmap,0,(size_t)max_peers*bitmap_len); memset(peer_sub_bitmap,0,(size_t)max_peers*bitmap_len);
          memset(peer_sub_reliable,0,(size_t)max_peers*bitmap_len);
          memset(writer_proxies,0,(size_t)n_channels*max_peers*sizeof(i_DartWriterProxy));
          memset(reader_proxies,0,(size_t)n_channels*max_peers*sizeof(i_DartReaderProxy));
          memset(lane_queued,0,nlanes); memset(dest_queued,0,ndest);
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
            /* reader asm buffers + frag bitmaps, per peer (skipped when dynamic) */
            for (p=0;p<max_peers;p++){
                uint8_t *assembly_buf = dyn ? NULL : (uint8_t*)i_dart_bump_take(b, q.max_message_bytes, 8);
                uint8_t *frag_bitmap  = dyn ? NULL : (uint8_t*)i_dart_bump_take(b, (max_fragments+7u)/8u, 1);
                if (st && b->base){
                    i_DartReaderProxy *r = i_dart_reader_proxy_at(st,c,p);
                    r->assembly_buf=assembly_buf; r->frag_bitmap=frag_bitmap;
                    r->assembly_cap = dyn ? 0u : q.max_message_bytes;
                    r->bitmap_cap  = dyn ? 0u : (uint32_t)((max_fragments+7u)/8u);
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
    /* per-(channel,peer) proxies: re-stride into the new max_peers (heap bufs ride along) */
    for (c=0;c<onc;c++) for (p=0;p<omp;p++){
        *i_dart_writer_proxy_at(nw,c,p) = *i_dart_writer_proxy_at(old,c,p);
        *i_dart_reader_proxy_at(nw,c,p) = *i_dart_reader_proxy_at(old,c,p);
    }
    /* per-peer interest bitmaps (stride grows with n_channels) + alias table */
    for (p=0;p<omp;p++){
        memcpy(nw->peer_pub_bitmap + (size_t)p*nw->bitmap_len,
               old->peer_pub_bitmap + (size_t)p*old->bitmap_len, old->bitmap_len);
        memcpy(nw->peer_sub_bitmap + (size_t)p*nw->bitmap_len,
               old->peer_sub_bitmap + (size_t)p*old->bitmap_len, old->bitmap_len);
        memcpy(nw->peer_sub_reliable + (size_t)p*nw->bitmap_len,
               old->peer_sub_reliable + (size_t)p*old->bitmap_len, old->bitmap_len);
        memcpy(nw->alias_to_channel + (size_t)p*nw->alias_max,
               old->alias_to_channel + (size_t)p*old->alias_max,
               (size_t)old->alias_max*sizeof(uint16_t));
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


/* fire one DartTransportEvent (no-op if no on_event). Transport emits MSG_LOST/TOO_BIG/
 * COLLISION/QOS. first/count are the kind's two numeric slots; route them to named fields. */
void i_dart_transport_fire_event(DartTransportState *st, DartTransportEventKind kind, uint16_t channel,
                        uint32_t peer, uint64_t first, uint64_t count, const char *detail){
    DartTransportEvent ev;
    if (!st->cfg.on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind=kind; ev.channel=channel; ev.peer=peer; ev.detail=detail; ev.user=st->cfg.user;
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


/* match one (channel,peer) proxy: the per-peer lane carries new data, repairs, acks/HB */
static void i_dart_writer_match(DartTransportState *st, uint16_t c, uint16_t peer_slot){
    i_DartChannel *ch=&st->channels[c];
    i_DartWriterProxy *w=i_dart_writer_proxy_at(st,c,peer_slot);
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

static void i_dart_writer_unmatch(DartTransportState *st, uint16_t c, uint16_t peer_slot){
    i_DartWriterProxy *w=i_dart_writer_proxy_at(st,c,peer_slot);
    if (!w->used) return;
    w->used=0;
    st->channels[c].matched_writers--;   /* guarded on used above: exactly one 1->0 per unmatch */
}

static void i_dart_reader_match(DartTransportState *st, uint16_t c, uint16_t peer_slot){
    i_DartReaderProxy *r=i_dart_reader_proxy_at(st,c,peer_slot);
    uint8_t *assembly_buf=r->assembly_buf, *frag_bitmap=r->frag_bitmap;
    uint32_t assembly_cap=r->assembly_cap, bitmap_cap=r->bitmap_cap;   /* keep grown buffers across rematch */
    memset(r,0,sizeof(*r));
    r->assembly_buf=assembly_buf; r->frag_bitmap=frag_bitmap; r->assembly_cap=assembly_cap; r->bitmap_cap=bitmap_cap;
    r->epoch=st->reader_epoch_counter++;   /* new incarnation: writers re-join on seeing it */
    r->used=1;       /* started==0: first DATA adopts the writer's position */
    /* announce this incarnation once so a caught-up (idle, non-pinging) writer
       re-joins and replays. A genuine discovery blip keeps its position through
       dart_transport_peer_dormant/resume and never lands here, so a single ACKNACK suffices. */
    if (st->channels[c].qos.reliability==DART_RELIABLE){
        r->ack_pending=1; r->ack_due_us=0; r->ack_force=1;
        i_dart_lane_wake(st,c,peer_slot);
    }
}

static void i_dart_reader_unmatch(DartTransportState *st, uint16_t c, uint16_t peer_slot){
    i_DartReaderProxy *r=i_dart_reader_proxy_at(st,c,peer_slot);
    r->used=0; r->assembly_active=0;
}


/* recompute one (channel,peer) match from our role and the peer's interest bits */
static void i_dart_channel_rematch(DartTransportState *st, uint16_t c, uint16_t peer_slot){
    i_DartChannel *ch=&st->channels[c];
    const uint8_t *peer_pub_bitmap=&st->peer_pub_bitmap[(size_t)peer_slot*st->bitmap_len];
    const uint8_t *peer_sub_bitmap=&st->peer_sub_bitmap[(size_t)peer_slot*st->bitmap_len];
    int wuse = (ch->role==DART_PUBSUB || ch->role==DART_PUB_ONLY) && i_dart_bit_get(peer_sub_bitmap,c);
    int ruse = (ch->role==DART_PUBSUB || ch->role==DART_SUB_ONLY) && i_dart_bit_get(peer_pub_bitmap,c);
    i_DartWriterProxy *w=i_dart_writer_proxy_at(st,c,peer_slot);
    i_DartReaderProxy *r=i_dart_reader_proxy_at(st,c,peer_slot);
    if (wuse && !w->used) i_dart_writer_match(st,c,peer_slot);
    else if (!wuse && w->used) i_dart_writer_unmatch(st,c,peer_slot);
    if (ruse && !r->used) i_dart_reader_match(st,c,peer_slot);
    else if (!ruse && r->used) i_dart_reader_unmatch(st,c,peer_slot);
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
    memset(&st->alias_to_channel[(size_t)free*st->alias_max],0xFF,(size_t)st->alias_max*sizeof(uint16_t));
    /* nothing matches until dart_transport_apply_peer_interest feeds the peer's interest
       list (carried in its discovery announce) */
}


void dart_transport_peer_remove(DartTransportState *st, uint32_t id){
    int s = i_dart_peer_slot(st,id); uint16_t c;
    if (s<0) return;
    for (c=0;c<st->cfg.n_channels;c++){
        i_dart_writer_unmatch(st,c,(uint16_t)s);
        i_dart_reader_unmatch(st,c,(uint16_t)s);
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
        i_DartWriterProxy *w=i_dart_writer_proxy_at(st,c,s);
        i_DartReaderProxy *r=i_dart_reader_proxy_at(st,c,s);
        if (st->channels[c].qos.reliability!=DART_RELIABLE) continue;
        if (r->used){ r->ack_pending=1; r->ack_due_us=0; r->ack_force=1; }  /* report our position now */
        if (w->used || r->used) i_dart_lane_wake(st,c,(uint16_t)s);
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
    uint16_t c; uint32_t p, max_peers;
    if (!st || !st->cfg.allocator) return;     /* fixed mode: nothing hook-allocated */
    max_peers = st->cfg.max_peers;
    for (c=0;c<st->cfg.n_channels;c++){
        i_DartChannel *ch=&st->channels[c];
        uint16_t depth, d;
        if (!ch->dynamic) continue;
        depth = ch->qos.keep_last;
        for (d=0; d<depth; d++)
            if (ch->history[d].buf){ st->cfg.allocator(st->cfg.user, ch->history[d].buf, 0);
                                  ch->history[d].buf=NULL; ch->history[d].cap=0; }
        for (p=0;p<max_peers;p++){
            i_DartReaderProxy *r=i_dart_reader_proxy_at(st,c,p);
            if (r->assembly_buf){ st->cfg.allocator(st->cfg.user, r->assembly_buf, 0); r->assembly_buf=NULL; r->assembly_cap=0; }
            if (r->frag_bitmap){ st->cfg.allocator(st->cfg.user, r->frag_bitmap, 0); r->frag_bitmap=NULL; r->bitmap_cap=0; }
        }
        if (ch->history_owned && ch->history){   /* ring allocated by dart_transport_channel_define */
            st->cfg.allocator(st->cfg.user, ch->history, 0);
            ch->history=NULL; ch->history_owned=0;
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

/* match count entries to local channels by identity (recomputed from each name),
 * recording the alias map. Same-identity-different-name is a collision: refused.
 * is_pub: the peer's publish list, so each entry's flags carry its OFFERED QoS, which
 * the RxO check uses to refuse a reliable subscriber a best-effort publisher. rel_bitmap
 * (sub list only, else NULL): records which subscribed channels the peer requested RELIABLE,
 * so the writer can keep best-effort readers out of flow control. */
static const uint8_t *i_dart_meta_scan(DartTransportState *st, int peer_slot, const uint8_t *p,
                                      uint32_t count, uint8_t *bitmap, int is_pub, uint8_t *rel_bitmap){
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
                        id, 0, ch->name ? ch->name : "");
            continue;
        }
        /* RxO QoS: a reliable subscriber refuses a best-effort publisher (no silent
           downgrade). We keep requesting reliable, so the match forms automatically if
           the publisher later upgrades and re-advertises. */
        if (is_pub && (ch->role==DART_PUBSUB || ch->role==DART_SUB_ONLY) &&
            ch->qos.reliability==DART_RELIABLE && !(flags & DART_META_F_RELIABLE)){
            i_dart_transport_fire_event(st, DART_TRANSPORT_QOS_INCOMPATIBLE, (uint16_t)channel_idx,
                        st->peer_ids[peer_slot], 0, 0, ch->name ? ch->name : "");
            continue;                                       /* refuse: no bit, no alias map */
        }
        /* schema gate (RxO for types): both sides run the same check off the same two
           advertised schemas, so a refused pair forms no proxy on either end (the writer
           never streams to, or flow-controls on, a reader that will not decode it). */
        if (st->cfg.schema_check &&
            !st->cfg.schema_check(st->cfg.user, (uint16_t)channel_idx, alias, is_pub)){
            i_dart_transport_fire_event(st, DART_TRANSPORT_SCHEMA_MISMATCH, (uint16_t)channel_idx,
                        st->peer_ids[peer_slot], 0, 0, ch->name ? ch->name : "");
            continue;                                       /* refuse: no bit, no alias map */
        }
        i_dart_bit_set(bitmap,(uint32_t)channel_idx);
        if (rel_bitmap && (flags & DART_META_F_RELIABLE)) i_dart_bit_set(rel_bitmap,(uint32_t)channel_idx);
        if ((uint32_t)alias < st->alias_max)
            st->alias_to_channel[(size_t)peer_slot*st->alias_max + alias] = (uint16_t)channel_idx;
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
    if (peer_slot<0 || blob.len<4) return;
    peer_pub_bitmap=&st->peer_pub_bitmap[(size_t)peer_slot*st->bitmap_len];
    peer_sub_bitmap=&st->peer_sub_bitmap[(size_t)peer_slot*st->bitmap_len];
    n_pub=i_dart_le_r16(d); n_sub=i_dart_le_r16(d+2);
    /* validate the whole variable-length list first: a truncated blob must not drop a match */
    { uint32_t k, tot=(uint32_t)n_pub+n_sub; p=d+4;
      for (k=0;k<tot;k++){
          if (p+4 > end) return;
          p += 4u + (uint32_t)p[3];
          if (p > end) return;
      } }
    memset(peer_pub_bitmap,0,st->bitmap_len); memset(peer_sub_bitmap,0,st->bitmap_len);
    memset(&st->peer_sub_reliable[(size_t)peer_slot*st->bitmap_len],0,st->bitmap_len);
    memset(&st->alias_to_channel[(size_t)peer_slot*st->alias_max],0xFF,(size_t)st->alias_max*sizeof(uint16_t));
    p = i_dart_meta_scan(st, peer_slot, d+4, n_pub, peer_pub_bitmap, 1, NULL);   /* pub list: offered QoS */
    p = i_dart_meta_scan(st, peer_slot, p,   n_sub, peer_sub_bitmap, 0,         /* sub list: requested QoS */
                        &st->peer_sub_reliable[(size_t)peer_slot*st->bitmap_len]);
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
        if (i_dart_writer_proxy_at(st,c,(uint32_t)s)->used) w++;
        if (i_dart_reader_proxy_at(st,c,(uint32_t)s)->used) r++;
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
    if (interest_len >= 4)   /* the section is located by walking the interest list, so it needs one */
        len += i_dart_meta_schemas_build(st, out + len, cap - len, schemas);
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
        if ((uint32_t)alias < st->alias_max){
            uint16_t m=st->alias_to_channel[(size_t)peer_slot*st->alias_max+alias]; channel_idx=(m==0xFFFFu)?-1:(int)m;
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
static char *i_dart_event_append_hex(char *p, char *end, uint64_t v){
    char tmp[16]; int n = 0;
    do { int d = (int)(v & 0xF); tmp[n++] = (char)(d < 10 ? '0'+d : 'a'+d-10); v >>= 4; } while (v);
    while (n && p < end) *p++ = tmp[--n];
    return p;
}
static char *i_dart_event_append_addr(char *p, char *end, const DartEvent *ev){   /* dotted quad + :port (IPv4 only) */
    int i;
    for (i = 0; i < 4; i++){ if (i) p = i_dart_event_append_str(p,end,"."); p = i_dart_event_append_u64(p,end,ev->ip[i]); }
    p = i_dart_event_append_str(p,end,":"); return i_dart_event_append_u64(p,end,ev->port);
}

const char *dart_event_str(const DartEvent *ev, char *buf, size_t cap){
    char *p, *end;
    if (!buf || !cap) return buf;
    p = buf; end = buf + cap - 1;                  /* reserve one byte for the NUL */
    switch (ev->kind){
    case DART_PEER_UP:
        p = i_dart_event_append_str(p,end,"peer-up id="); p = i_dart_event_append_u64(p,end,ev->peer);
        if (ev->ip_len == 4){ p = i_dart_event_append_str(p,end," at "); p = i_dart_event_append_addr(p,end,ev); }
        p = i_dart_event_append_str(p,end," ("); p = i_dart_event_append_str(p,end,ev->detail); p = i_dart_event_append_str(p,end,")");
        break;
    case DART_PEER_DOWN:
        p = i_dart_event_append_str(p,end,"peer-down id="); p = i_dart_event_append_u64(p,end,ev->peer);
        p = i_dart_event_append_str(p,end," ("); p = i_dart_event_append_str(p,end,ev->detail); p = i_dart_event_append_str(p,end,")");
        break;
    case DART_PEER_INTEREST:
        p = i_dart_event_append_str(p,end,"interest id="); p = i_dart_event_append_u64(p,end,ev->peer);
        p = i_dart_event_append_str(p,end," publish-to="); p = i_dart_event_append_u64(p,end,ev->publish_topics);
        p = i_dart_event_append_str(p,end," topics, receive-from="); p = i_dart_event_append_u64(p,end,ev->receive_topics);
        p = i_dart_event_append_str(p,end," topics");
        break;
    case DART_PEER_REFUSED:
        p = i_dart_event_append_str(p,end,"peer-refused at "); p = i_dart_event_append_addr(p,end,ev);
        p = i_dart_event_append_str(p,end," (table full of active peers)");
        break;
    case DART_NAME_COLLISION:
        p = i_dart_event_append_str(p,end,"name-collision ch="); p = i_dart_event_append_u64(p,end,ev->channel);
        p = i_dart_event_append_str(p,end," id=0x"); p = i_dart_event_append_hex(p,end,ev->identity);
        p = i_dart_event_append_str(p,end," ("); p = i_dart_event_append_str(p,end,ev->detail);
        p = i_dart_event_append_str(p,end,"): match refused");
        break;
    case DART_QOS_INCOMPATIBLE:
        p = i_dart_event_append_str(p,end,"qos-incompatible ch="); p = i_dart_event_append_u64(p,end,ev->channel);
        p = i_dart_event_append_str(p,end," from id="); p = i_dart_event_append_u64(p,end,ev->peer);
        p = i_dart_event_append_str(p,end," ("); p = i_dart_event_append_str(p,end,ev->detail);
        p = i_dart_event_append_str(p,end,"): reliable subscriber refused best-effort publisher");
        break;
    case DART_SCHEMA_MISMATCH:
        p = i_dart_event_append_str(p,end,"schema-mismatch ch="); p = i_dart_event_append_u64(p,end,ev->channel);
        p = i_dart_event_append_str(p,end," peer id="); p = i_dart_event_append_u64(p,end,ev->peer);
        p = i_dart_event_append_str(p,end," ("); p = i_dart_event_append_str(p,end,ev->detail);
        p = i_dart_event_append_str(p,end,"): incompatible schemas, refused");
        break;
    case DART_MSG_LOST:
        p = i_dart_event_append_str(p,end,"msg-lost ch="); p = i_dart_event_append_u64(p,end,ev->channel);
        p = i_dart_event_append_str(p,end," from id="); p = i_dart_event_append_u64(p,end,ev->peer);
        p = i_dart_event_append_str(p,end," seqno "); p = i_dart_event_append_u64(p,end,ev->lost_first);
        p = i_dart_event_append_str(p,end,".."); p = i_dart_event_append_u64(p,end,ev->lost_first + ev->lost_count - 1);
        break;
    case DART_MSG_TOO_BIG:
        p = i_dart_event_append_str(p,end,"msg-too-big ch="); p = i_dart_event_append_u64(p,end,ev->channel);
        p = i_dart_event_append_str(p,end," from id="); p = i_dart_event_append_u64(p,end,ev->peer);
        p = i_dart_event_append_str(p,end," ("); p = i_dart_event_append_u64(p,end,ev->too_big_bytes);
        p = i_dart_event_append_str(p,end," bytes), skipped");
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

/* arena layout: the core struct, the announce-blob buffer, then the per-channel schema
   registry (no peer table -- that lives in the discovery core). One sequence so measure
   and build agree. */
static void i_dart_node_core_layout(i_DartBump *b, uint16_t n_channels,
                              i_DartNodeCore **out_c, uint8_t **out_meta,
                              DartMetaSchema **out_schemas, const DartSchema ***out_compiled){
    i_DartNodeCore *c       = (i_DartNodeCore*)i_dart_bump_take(b, sizeof(struct i_DartNodeCore), 16);
    uint8_t *meta           = (uint8_t*)       i_dart_bump_take(b, dart_meta_capacity(n_channels), 16);
    DartMetaSchema *schemas = (DartMetaSchema*)i_dart_bump_take(b, (size_t)n_channels*sizeof(DartMetaSchema), 16);
    const DartSchema **compiled = (const DartSchema**)i_dart_bump_take(b, (size_t)n_channels*sizeof(DartSchema*), 16);
    if (out_c)        *out_c        = c;
    if (out_meta)     *out_meta     = meta;
    if (out_schemas)  *out_schemas  = schemas;
    if (out_compiled) *out_compiled = compiled;
}

size_t i_dart_node_core_required_memory(uint16_t n_channels){
    i_DartBump b; memset(&b, 0, sizeof b);
    i_dart_node_core_layout(&b, n_channels, NULL, NULL, NULL, NULL);
    return b.offset + 16u;   /* slack to align the caller's mem up to base */
}

i_DartNodeCore *i_dart_node_core_init(void *mem, size_t cap, const i_DartNodeCoreConfig *cfg){
    i_DartBump b; i_DartNodeCore *c; uint8_t *base, *meta;
    DartMetaSchema *schemas; const DartSchema **compiled;
    if (!mem || !cfg || !cfg->transport) return NULL;
    if (cap < i_dart_node_core_required_memory(cfg->n_channels)) return NULL;
    base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    memset(&b, 0, sizeof b); b.base = base; b.cap = cap - (size_t)(base - (uint8_t*)mem);
    i_dart_node_core_layout(&b, cfg->n_channels, &c, &meta, &schemas, &compiled);

    memset(c, 0, sizeof *c);
    c->transport     = cfg->transport;
    c->discovery     = cfg->discovery;     /* may be NULL now, bound via bind_discovery later */
    c->on_event      = cfg->on_event;      c->user = cfg->user;
    c->alloc         = cfg->alloc;         c->alloc_user = cfg->alloc_user;
    c->oob_capable   = cfg->oob_capable;
    memcpy(c->oob_host, cfg->oob_host, 16);
    c->meta_buf      = meta;
    c->meta_cap      = dart_meta_capacity(cfg->n_channels);
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
    if (new_cap < i_dart_node_core_required_memory(new_n_channels)) return NULL;
    base = (uint8_t*)(((uintptr_t)new_mem + 15u) & ~(uintptr_t)15u);
    memset(&b, 0, sizeof b); b.base = base; b.cap = new_cap - (size_t)(base - (uint8_t*)new_mem);
    i_dart_node_core_layout(&b, new_n_channels, &c, &meta, &schemas, &compiled);
    *c = *old;                          /* scalars + transport/discovery ptrs (caller re-points);
                                           the hook-allocated schema arrays ride along untouched */
    c->meta_buf  = meta;                /* caller rebuilds the blob into it */
    c->meta_cap  = dart_meta_capacity(new_n_channels);
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
    c->meta_len = dart_transport_meta_build(c->transport, c->meta_buf, c->meta_cap,
                                  c->frag_size, c->oob_capable, c->oob_host,
                                  c->chan_schemas);
    return c->meta_len;
}

DartBytes i_dart_node_core_meta(i_DartNodeCore *c){
    return dart_bytes(c->meta_buf, c->meta_len);
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

static void i_dart_node_core_fire(i_DartNodeCore *c, DartEventKind kind, uint32_t id,
                            const DartDiscoveryAddr *addr, const char *detail){
    DartEvent ev;
    if (!c->on_event) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind; ev.peer = id; ev.detail = detail; ev.user = c->user;
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
    ev.detail = "interest applied"; ev.user = c->user;
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
        i_dart_node_core_fire(c, DART_PEER_UP, id, addr, "peer discovered");
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
            i_dart_node_core_fire(c, DART_PEER_UP, id, addr, "peer resumed");
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
            i_dart_node_core_fire(c, DART_PEER_DOWN, id, NULL, "peer dropped");
        }
    } else {   /* GONE: said BYE or its slot was reclaimed; free the transport state */
        int notify = (ex && ex->added && !ex->dormant);   /* active->gone: app not yet told */
        dart_transport_peer_remove(c->transport, id);   /* discovery zeroes the scratch on slot reuse */
        i_dart_node_core_peer_schema_clear(c, id);      /* the id may be reassigned */
        if (notify) i_dart_node_core_fire(c, DART_PEER_DOWN, id, NULL, "peer lost");
    }
}

static void i_dart_node_core_peer_refused(i_DartNodeCore *c, const DartDiscoveryAddr *addr){
    i_dart_node_core_fire(c, DART_PEER_REFUSED, 0, addr, "peer table full (all active)");
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
    /* backpressure accumulators, read via dart_node_backpressure_stats */
    uint64_t      backpressure_total_us;
    uint32_t      backpressure_wait_count;
    /* diagnostic: in-pump probe sampled inside the backpressure wait (dart_channel_send) */
    DartPumpProbeFn pump_probe;
    void          *pump_probe_user;
    uint64_t       pump_probe_interval_us;
    /* app callbacks: one message sink (wrapped to build a DartMsg) + everything-else */
    DartMsgFn    user_on_message;
    DartEventFn  on_event;
    void          *user_data;
    void          *arena;      /* control-structs block (a freeable pool allocation); relocated on grow */
    /* the node's paged region allocator (common/alloc.h): backs the node struct, arena, message
       buffers, schemas, and handles. COPIED from the caller's allocator at open (so the caller's
       may be a temporary); the node owns it and resets it on close. */
    DartAllocator pool;
    uint8_t        alloc_dynamic;
    uint8_t        grow_pending;   /* a peer was refused for lack of slots; grow at next poll */
    uint16_t       max_peers;      /* current peer-table capacity (doubles on a dynamic grow) */
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
    void         **shm_pool;      /* [nchan*N_CLASSES] our (channel,class) segments (NULL = not created) */
    uint8_t       *shm_pool_mem;  /* arena: nchan * N_CLASSES * i_dart_shm_state_bytes */
    uint16_t       shm_n_channels;
    uint64_t      *shm_reader_segments;      /* [reader_max] attached reader-segment ids (0 = empty) */
    uint8_t       *shm_reader_pool_mem; /* [reader_max * i_dart_shm_state_bytes] */
    uint16_t       shm_reader_max;
    uint32_t       shm_tx, shm_rx;/* messages published / delivered via SHM (observability) */
#endif
};

/* The node's message-buffer allocator, handed to the transport as its realloc hook and used
 * for schemas and channel handles (u is the node). It delegates to the node's paged region
 * allocator: one freeable allocation per call, so free/resize work and reset reclaims all. */
static void *i_dart_node_alloc(void *u, void *ptr, size_t size){
    return dart_allocator_alloc(&((DartNode*)u)->pool, ptr, size);
}

/* build a DartMsg and hand it to the app (the channel name is a local lookup, never
 * on the wire). Shared by the inline and SHM delivery paths. A message that does not
 * fit its sender's declared schema broke the sender's own contract: dropped + surfaced,
 * never handed to the app to misdecode. */
static void i_dart_node_deliver(DartNode *n, uint16_t ch, uint32_t from, DartBytes data){
    DartMsg m;
    const DartSchema *schema = i_dart_node_core_msg_schema(n->core, from, ch);
    if (schema && !dart_schema_validate(schema, data)){
        if (n->on_event){
            DartEvent e; memset(&e, 0, sizeof e);
            e.kind = DART_SCHEMA_MISMATCH; e.user = n->user_data;
            e.peer = from; e.channel = ch;
            e.detail = "message does not fit the sender's schema";
            n->on_event(&e);
        }
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
       never told it was refused. Static mode keeps the cap and surfaces the event. */
    if (n->alloc_dynamic && ev->kind == DART_PEER_REFUSED){ n->grow_pending = 1; return; }
    if (n->on_event){ DartEvent e = *ev; e.user = n->user_data; n->on_event(&e); }
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
    if (!n->on_event) return;
    memset(&e, 0, sizeof e);
    switch (tev->kind){
    case DART_TRANSPORT_MSG_LOST:        e.kind = DART_MSG_LOST; break;
    case DART_TRANSPORT_MSG_TOO_BIG:     e.kind = DART_MSG_TOO_BIG; break;
    case DART_TRANSPORT_NAME_COLLISION:  e.kind = DART_NAME_COLLISION; break;
    case DART_TRANSPORT_QOS_INCOMPATIBLE:e.kind = DART_QOS_INCOMPATIBLE; break;
    case DART_TRANSPORT_SCHEMA_MISMATCH: e.kind = DART_SCHEMA_MISMATCH; break;
    default: return;
    }
    e.detail = tev->detail; e.user = n->user_data; e.peer = tev->peer; e.channel = tev->channel;
    e.lost_first = tev->lost_first; e.lost_count = tev->lost_count;
    e.too_big_bytes = tev->too_big_bytes; e.identity = tev->identity;
    n->on_event(&e);
}

#ifdef DART_SHM
/* reader-pool cache capacity: a same-host peer publishes on its channels, one segment
 * each, so size to peers x channels (clamped to the u16 the cache index uses) */
static uint16_t i_dart_node_shm_reader_max(uint16_t max_peers, uint16_t n_channels){
    uint32_t r = (uint32_t)max_peers * (n_channels ? n_channels : 1u) * DART_SHM_N_CLASSES;
    return (uint16_t)(r > 0xFFFFu ? 0xFFFFu : r);
}
#endif

/* The node arena's sub-blocks, laid out in ONE place so the measure pass (bump.base
 * NULL, read bump.offset) and the build pass (read the pointers) run the same i_dart_bump_take
 * sequence and can never drift. */
typedef struct {
    uint8_t   *handles, *node_core, *transport, *discovery;
#ifdef DART_SHM
    uint8_t   *shm_pool, *shm_pool_mem, *shm_reader_segments, *shm_reader_pool;
#endif
    size_t     node_core_bytes, transport_bytes, discovery_bytes;
} i_DartNodeBlocks;

static void i_dart_node_layout(i_DartBump *b, uint16_t max_peers, uint16_t max_channels,
                              const DartConfig *transport_cfg,
                              const DartDiscoveryNetConfig *discovery_rt_cfg, i_DartNodeBlocks *o){
    o->handles       = (uint8_t*)i_dart_bump_take(b, (size_t)max_channels * sizeof(DartChannel*), 16);
    o->node_core_bytes = i_dart_node_core_required_memory(max_channels);   /* peers live in discovery now */
    o->node_core = (uint8_t*)i_dart_bump_take(b, o->node_core_bytes, 16);
    o->transport_bytes = dart_transport_required_memory(transport_cfg);
    o->transport = (uint8_t*)i_dart_bump_take(b, o->transport_bytes, 16);
#ifdef DART_SHM
    {   size_t state_bytes  = i_dart_shm_state_bytes();
        uint32_t n_segments = (uint32_t)max_channels * DART_SHM_N_CLASSES;
        uint16_t reader_max = i_dart_node_shm_reader_max(max_peers, max_channels);
        o->shm_pool            = (uint8_t*)i_dart_bump_take(b, (size_t)n_segments * sizeof(void*), 16);
        o->shm_pool_mem        = (uint8_t*)i_dart_bump_take(b, (size_t)n_segments * state_bytes, 16);
        o->shm_reader_segments = (uint8_t*)i_dart_bump_take(b, (size_t)reader_max * 8u, 16);
        o->shm_reader_pool     = (uint8_t*)i_dart_bump_take(b, (size_t)reader_max * state_bytes, 16);
    }
#endif
    o->discovery_bytes = dart_discovery_placement_memory(discovery_rt_cfg);
    o->discovery = (uint8_t*)i_dart_bump_take(b, o->discovery_bytes, 16);
}

/* send one datagram to a peer; returns 1 when done with it, 0 only on a would-block
 * TX-full. The core resolves the abstract destination; we just map it to a UDP address. */
static int i_dart_node_tx(DartNode *n, uint32_t to, const uint8_t *buf, size_t len){
    i_DartNodeDest d;
    if (!i_dart_node_core_resolve(n->core, to, &d)) return 1;   /* peer vanished */
    if (i_dart_plat_send(n->fd, buf, len, d.ip, d.port) < 0 && i_dart_plat_would_block())
        return 0;
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
    mem = n->shm_pool_mem + idx * i_dart_shm_state_bytes();
    n->shm_pool[idx] = i_dart_shm_create(mem, &c);
    return (i_DartShmPool*)n->shm_pool[idx];
}
/* lazily attach a peer's segment by id (class is in the low bits); cache it. */
static i_DartShmPool *i_dart_node_shm_reader_pool(DartNode *n, uint64_t seg){
    uint16_t i, slot = 0xFFFF; i_DartShmConfig c; uint8_t *mem; size_t state_bytes;
    state_bytes = i_dart_shm_state_bytes();
    for (i=0;i<n->shm_reader_max;i++){
        if (n->shm_reader_segments[i]==seg) return (i_DartShmPool*)(n->shm_reader_pool_mem + (size_t)i*state_bytes);
        if (n->shm_reader_segments[i]==0 && slot==0xFFFF) slot = i;
    }
    if (slot==0xFFFF) return NULL;                         /* cache full */
    memset(&c, 0, sizeof c);
    c.segment_id = seg; i_dart_shm_seg_name(c.name, seg);     /* attach maps whole + reads geometry */
    mem = n->shm_reader_pool_mem + (size_t)slot*state_bytes;
    if (!i_dart_shm_attach(mem, &c)) return NULL;
    n->shm_reader_segments[slot] = seg;
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
    if (!n) return NULL;
    memset(n, 0, sizeof *n);
    n->pool = pool;                                  /* the node owns the pool now; allocate via &n->pool */
    n->alloc_dynamic = (alloc->page_realloc != NULL);
    arena = dart_allocator_alloc(&n->pool, NULL, need);
    if (!arena){ DartAllocator p = n->pool; dart_allocator_reset(&p); return NULL; }
    base = (uint8_t*)(((uintptr_t)arena+15u)&~(uintptr_t)15u);
    {   i_DartBump b; memset(&b,0,sizeof b);
        b.base = base; b.cap = need - (size_t)(base - (uint8_t*)arena);
        i_dart_node_layout(&b, max_peers, max_channels, &tc, &dc, &blocks); }

    if (!i_dart_plat_startup()){
        DartAllocator p = n->pool; dart_allocator_reset(&p);
        return NULL;
    }

    n->fd = DART_SOCK_BAD;
    n->domain = o.domain;
    n->net = o.net;
    n->user_on_message = on_message; n->on_event = on_event;
    n->user_data = o.user_data;
    n->arena = arena;
    n->handles = (DartChannel**)blocks.handles;
    memset(n->handles, 0, (size_t)max_channels * sizeof(DartChannel*));
    n->max_channels = max_channels;
    n->max_peers = max_peers;

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
    if (!n->transport) goto fail_startup;
#ifdef DART_SHM
    {   uint32_t n_segments = (uint32_t)max_channels * DART_SHM_N_CLASSES;
        uint16_t reader_max = i_dart_node_shm_reader_max(max_peers, max_channels); uint32_t i;
        n->shm_n_channels = max_channels; n->shm_reader_max = reader_max;
        n->shm_pool            = (void**)blocks.shm_pool;
        n->shm_pool_mem        = blocks.shm_pool_mem;
        n->shm_reader_segments = (uint64_t*)blocks.shm_reader_segments;
        n->shm_reader_pool_mem = blocks.shm_reader_pool;
        for (i=0;i<n_segments;i++) n->shm_pool[i]=NULL;
        for (i=0;i<reader_max;i++) n->shm_reader_segments[i]=0;
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
        if (!n->core) goto fail_startup;
    }

    /* Bind the data socket before opening discovery so we can advertise its real
       port (0 => OS ephemeral, read back via getsockname). No reuse: a unicast
       endpoint owns its port, so a collision fails loudly here. */
    fd = i_dart_plat_udp_open();
    if (fd==DART_SOCK_BAD) goto fail_startup;
    n->fd=fd;                       /* owned now: fail_sock closes it */
    if (!i_dart_plat_bind(fd, 0, o.net.data_port, 0)) goto fail_sock;
    local_port = i_dart_plat_local_port(fd);
    if (local_port==0) goto fail_sock;
    i_dart_plat_set_nonblock(fd);   /* never block in recv/send; poll drains the queue */
    i_dart_plat_suppress_connreset(fd);  /* suppress WSAECONNRESET from a bounced send */
    if (o.net.recv_buffer_bytes) i_dart_plat_set_rcvbuf(fd, (int)o.net.recv_buffer_bytes);
    if (o.net.send_buffer_bytes) i_dart_plat_set_sndbuf(fd, (int)o.net.send_buffer_bytes);
    dc.discovery.data_port = local_port;       /* advertise the actual port */

    dc.discovery.on_event = i_dart_node_core_on_disc_event;   /* node core demuxes PEER_UP/DOWN/REFUSED */
    dc.discovery.user     = n->core;
    dc.discovery.name     = dart_string(node_name, node_name_len);   /* discovery-owned (its own blob section) */
    /* the core builds our OVERLAY (frag size + OOB host + interest); discovery wraps it in
       its blob (after the locator + name) so peers reassemble and match from discovery */
    i_dart_node_core_build_meta(n->core);
    dc.discovery.meta = i_dart_node_core_meta(n->core);
    n->discovery = dart_discovery_place(blocks.discovery, blocks.discovery_bytes, &dc);
    if (!n->discovery) goto fail_sock;
    /* the node core delegates id<->address resolution + per-peer scratch to discovery's table */
    i_dart_node_core_bind_discovery(n->core, dart_discovery_state(n->discovery));

    return n;

fail_sock:
    if (n->fd != DART_SOCK_BAD) i_dart_plat_close(n->fd);
    n->fd = DART_SOCK_BAD;
fail_startup:
    i_dart_plat_cleanup();
    { DartAllocator p = n->pool; dart_allocator_reset(&p); }   /* frees the node struct + arena */
    return NULL;
}

/* Dynamic-mode growth: relocate the whole node into a bigger arena at the given counts so
 * a full peer table or channel reserve stops being a hard cap. Heap message buffers and SHM
 * writer segments stay put (only their owning control structures move); the user-held
 * DartNode and DartChannel handles live outside the arena, so they survive. Returns 1 with n
 * now on the new arena, or 0 if the bigger arena couldn't be allocated (n left unchanged). */
static int i_dart_node_grow(DartNode *n, uint16_t new_max_peers, uint16_t new_max_channels){
    DartConfig tc; DartDiscoveryNetConfig dc; i_DartNodeBlocks nb; i_DartBump b;
    DartTransportState *nt; i_DartNodeCore *ncore; DartDiscovery *ndisc;
    void *new_arena, *old_arena = n->arena;
    uint8_t *nbase; size_t need;
    uint16_t old_max_channels = n->max_channels, new_meta_cap = dart_meta_capacity(new_max_channels);

    if (!n->alloc_dynamic) return 0;
    if (new_max_peers <= n->max_peers && new_max_channels <= n->max_channels) return 0;

    memset(&tc,0,sizeof tc); memset(&dc,0,sizeof dc);
    tc.channels=NULL; tc.n_channels=new_max_channels; tc.max_peers=new_max_peers;
    tc.allocator=i_dart_node_alloc; tc.frag_payload=n->net.fragment_size;
    dc.discovery.max_peers=new_max_peers; dc.discovery.meta_capacity=new_meta_cap;
    dc.discovery.peer_user_bytes = i_dart_node_core_peer_user_bytes();   /* size discovery's scratch to match */

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
        size_t sb = i_dart_shm_state_bytes();
        uint32_t old_segs=(uint32_t)n->shm_n_channels*DART_SHM_N_CLASSES;
        uint32_t new_segs=(uint32_t)new_max_channels*DART_SHM_N_CLASSES, i;
        uint16_t new_reader_max = i_dart_node_shm_reader_max(new_max_peers, new_max_channels);
        void **np = (void**)nb.shm_pool;
        for (i=0;i<new_segs;i++) np[i]=NULL;
        for (i=0;i<old_segs;i++)        /* writer pool: keep segments mapped, relocate the state */
            if (n->shm_pool[i]){
                memcpy(nb.shm_pool_mem + (size_t)i*sb, n->shm_pool[i], sb);
                np[i] = nb.shm_pool_mem + (size_t)i*sb;
            }
        for (i=0;i<n->shm_reader_max;i++)  /* reader pool: detach + reset (re-attaches lazily) */
            if (n->shm_reader_segments[i]) i_dart_shm_detach((i_DartShmPool*)(n->shm_reader_pool_mem+(size_t)i*sb));
        n->shm_pool=np; n->shm_pool_mem=nb.shm_pool_mem;
        n->shm_reader_segments=(uint64_t*)nb.shm_reader_segments;
        n->shm_reader_pool_mem=nb.shm_reader_pool;
        for (i=0;i<new_reader_max;i++) n->shm_reader_segments[i]=0;
        n->shm_reader_max=new_reader_max; n->shm_n_channels=new_max_channels;
    }
#endif

    n->transport=nt; n->core=ncore; n->discovery=ndisc;
    n->handles=(DartChannel**)nb.handles;
    n->max_channels=new_max_channels; n->max_peers=new_max_peers;
    dart_allocator_alloc(&n->pool, old_arena, 0);    /* control structs only; heap bufs + segments moved by ref */
    n->arena=new_arena;
    return 1;
}

DartChannel *dart_node_create_channel(DartNode *n, const char *name, DartRole role,
                                      const DartSchema *schema, const DartChannelOpts *opts){
    DartChannelDef def; DartChannel *h; uint16_t idx;
    if (!n || !name) return NULL;
    if (n->n_created >= n->max_channels){       /* reserve full: grow (dynamic) or refuse (static) */
        uint16_t want = n->max_channels < 0x8000u ? (uint16_t)(n->max_channels*2u) : 0xFFFFu;
        if (want <= n->max_channels || !i_dart_node_grow(n, n->max_peers, want)) return NULL;
    }
    idx = n->n_created;
    h = (DartChannel*)i_dart_node_alloc(n, NULL, sizeof *h);   /* stable: outlives any arena grow */
    if (!h) return NULL;
    h->schema = NULL;
    if (schema){   /* copy into node memory so the caller's schema need not outlive the channel */
        DartBytes w = dart_schema_wire(schema);
        h->schema = dart_schema_parse(w.data, w.len, i_dart_node_alloc, n);
        if (!h->schema){ i_dart_node_alloc(n, h, 0); return NULL; }
    }
    memset(&def, 0, sizeof def);
    def.name = name; def.role = (uint8_t)role;
    if (opts) def.qos = opts->qos;
    if (dart_transport_channel_define(n->transport, idx, &def) != 0){
        if (h->schema) dart_schema_free(h->schema, i_dart_node_alloc, n);
        i_dart_node_alloc(n, h, 0); return NULL;
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
    uint8_t buf[DART_DGRAM_MAX];
    for (;;){
        uint8_t src_ip[4]; uint16_t src_port;
        int r = i_dart_plat_recv(fd, buf, sizeof buf, src_ip, &src_port);
        if (r<0){
            if (i_dart_plat_would_block()) break;        /* queue empty */
            continue;   /* per-datagram error (e.g. bounced send); keep draining */
        }
        if (r>0){
            if (r>=4 && buf[0]=='u' && buf[1]=='D' && buf[2]=='S' && buf[3]=='C'){
                /* unicast announce aimed at our data port: hand it to discovery */
                dart_discovery_feed(n->discovery, src_ip, 4, dart_bytes(buf, (size_t)r));
            } else {
                uint32_t from;
                if (i_dart_node_core_id_for_addr(n->core, src_ip, src_port, &from))
                    dart_transport_on_datagram(n->transport, from, dart_bytes(buf, (size_t)r), i_dart_plat_now_us());
            }
        }
        if (i_dart_plat_now_us() >= deadline) break;      /* yield to discovery/send */
    }
}

int dart_node_poll(DartNode *n, int timeout_ms){
    uint8_t buf[DART_DGRAM_MAX]; uint32_t to; size_t out_len; uint64_t now;
    i_DartPollfd pfd[1];

    /* a peer was refused last tick for lack of slots: grow the table now, between ticks
       (safe: not inside any layer's processing), then the peer's next announce is admitted */
    if (n->grow_pending){
        uint16_t want = n->max_peers < 0x8000u ? (uint16_t)(n->max_peers*2u) : 0xFFFFu;
        n->grow_pending = 0;
        if (want > n->max_peers) i_dart_node_grow(n, want, n->max_channels);
    }

    dart_discovery_poll(n->discovery, 0);                 /* discovery tick (non-blocking) */

    memset(pfd,0,sizeof pfd);
    pfd[0].fd=n->fd; pfd[0].events=DART_POLLIN;
    /* cap the wait at the next internal timer so a due ack/NACK/heartbeat fires on
       time, not after the full quantum (no traffic to wake us when a writer stalls) */
    { uint64_t next_deadline = dart_transport_next_deadline_us(n->transport);
      if (next_deadline){ uint64_t t0 = i_dart_plat_now_us();
               uint64_t us = (next_deadline > t0) ? next_deadline - t0 : 0;          /* until the timer */
               int ms = (us >= (uint64_t)timeout_ms*1000u) ? timeout_ms
                                                           : (int)((us + 999u)/1000u);
               if (ms < timeout_ms) timeout_ms = ms; } }       /* round up: no busy-spin */
    if (i_dart_plat_poll(pfd,1,timeout_ms) > 0){
        uint64_t rx_deadline = i_dart_plat_now_us() + DART_RX_BUDGET_US;
        if (pfd[0].revents & DART_POLLIN) i_dart_node_rx_drain(n, n->fd, rx_deadline);
    }

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
    return 0;
}

/* publish on a channel index: bounded backpressure pump, then SHM fast path, then UDP */
static int i_dart_node_do_send(DartNode *n, uint16_t channel, DartBytes data){
    size_t len = data.len;
    /* one O(1) count gates the per-send fast paths: a channel no peer subscribes to skips
       backpressure and the SHM eligibility scan here, and the copy+commit in the core. */
    int matched = dart_transport_writer_match_count(n->transport, channel);
    /* bounded backpressure: pump the loop (on_message/on_event may fire here) until
       a slow reader acks or qos.backpressure_wait_us elapses, then send anyway */
    const DartQos *q = dart_transport_channel_qos(n->transport, channel);
    if (matched && q && q->backpressure_wait_us && dart_transport_send_would_evict(n->transport, channel)){
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
        do {
            dart_node_poll(n, 1);
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
    }
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
                    return 0;
                }
            }
        }
    }
#endif
    return dart_transport_send(n->transport, channel, data, i_dart_plat_now_us());
}

int dart_channel_send(DartChannel *ch, DartBytes data){
    if (!ch) return DART_ERR_NO_CHANNEL;
    return i_dart_node_do_send(ch->n, ch->index, data);
}

int dart_channel_set_role(DartChannel *ch, DartRole role){
    int r;
    if (!ch) return -1;
    r = dart_transport_set_role(ch->n->transport, ch->index, (uint8_t)role);
    if (r == 0){   /* re-advertise our interest: peers rematch as the new blob arrives */
        i_dart_node_core_build_meta(ch->n->core);
        dart_discovery_advertise(ch->n->discovery, i_dart_node_core_meta(ch->n->core));
        dart_discovery_replay(ch->n->discovery);   /* re-apply peers' interest to our new role */
    }
    return r;
}

uint16_t dart_channel_index(const DartChannel *ch){ return ch ? ch->index : 0; }

const DartSchema *dart_channel_schema(const DartChannel *ch){ return ch ? ch->schema : NULL; }

DartChannel *dart_node_channel(DartNode *n, uint16_t index){
    if (!n || index >= n->n_created) return NULL;
    return n->handles[index];
}

/* Read-only peer view: discovery already packs its peer table into a zero-copy array, and a
 * node peer IS a discovery peer (it adds only the decoded overlay, read on demand via
 * dart_node_peer_frag / dart_node_peer_interest_next). So this just forwards. */
const DartDiscoveryPeer *dart_node_peers(DartNode *n, uint16_t *count){
    return dart_discovery_peers(n ? n->discovery : NULL, count);
}

void dart_node_backpressure_stats(DartNode *n, uint64_t *waited_us, uint32_t *waited_sends){
    if (waited_us)    *waited_us    = n->backpressure_total_us;
    if (waited_sends) *waited_sends = n->backpressure_wait_count;
}

void dart_node_mem_stats(DartNode *n, size_t *in_use, size_t *peak, uint64_t *alloc_calls){
    if (!n) return;
    dart_allocator_stats(&n->pool, in_use, peak, alloc_calls);   /* live bytes, high-water, (re)allocs */
}

void dart_channel_repair_stats(DartChannel *ch, DartRepairStats *out){
    if (ch) dart_transport_repair_stats(ch->n->transport, ch->index, out);
    else if (out) memset(out, 0, sizeof *out);
}

void dart_node_set_pump_probe(DartNode *n, DartPumpProbeFn fn, uint64_t interval_us, void *user){
    n->pump_probe = fn; n->pump_probe_interval_us = interval_us; n->pump_probe_user = user;
}

int dart_channel_reader_progress(DartChannel *ch, uint32_t peer,
                                 uint64_t *base_seqno, uint32_t *have, uint32_t *total){
    return ch ? dart_transport_reader_progress(ch->n->transport, ch->index, peer, base_seqno, have, total) : 0;
}

#ifdef DART_SHM
void dart_node_shm_stats(DartNode *n, uint32_t *sent, uint32_t *recv){
    if (sent) *sent = n->shm_tx;
    if (recv) *recv = n->shm_rx;
}
#endif

int dart_channel_drain(DartChannel *ch, int timeout_ms){
    DartNode *n; uint64_t deadline;
    if (!ch) return 0;
    n = ch->n;
    deadline = i_dart_plat_now_us() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000u;
    while (!dart_transport_send_drained(n->transport, ch->index)){
        if (i_dart_plat_now_us() >= deadline) return 0;
        dart_node_poll(n, 1);
    }
    return 1;
}

int dart_channel_match_count(DartChannel *ch){
    return ch ? dart_transport_writer_match_count(ch->n->transport, ch->index) : 0;
}

void dart_node_close(DartNode *n, int send_bye){
    DartAllocator pool;
    if (!n) return;
    pool = n->pool;                                   /* copy out: the reset below frees n itself */
    if (n->discovery) dart_discovery_close(n->discovery, send_bye);
    if (n->fd != DART_SOCK_BAD) i_dart_plat_close(n->fd);
#ifdef DART_SHM
    if (n->shm_capable){                              /* unmap OS segments; the pool reset frees only pool memory */
        size_t state_bytes = i_dart_shm_state_bytes(); uint32_t i, n_segments = (uint32_t)n->shm_n_channels * DART_SHM_N_CLASSES;
        for (i=0;i<n_segments;i++)
            if (n->shm_pool[i]) i_dart_shm_detach((i_DartShmPool*)n->shm_pool[i]);   /* unlinks ours */
        for (i=0;i<n->shm_reader_max;i++)
            if (n->shm_reader_segments[i]) i_dart_shm_detach((i_DartShmPool*)(n->shm_reader_pool_mem + (size_t)i*state_bytes));
    }
#endif
    i_dart_plat_cleanup();
    dart_allocator_reset(&pool);   /* frees the node struct, arena, message buffers, schemas, handles */
}
#pragma endregion
#endif /* !DART_TRANSPORT_SANS_IO */
#endif /* DART_TRANSPORT_IMPLEMENTATION */
