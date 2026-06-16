/* DESIGN SKETCH -- not yet wired into pack.c / the build.
 *
 * Zero-copy same-host transport for DART. The control plane stays UDP: discovery,
 * liveness, ordering, and reliability are unchanged. For a reader on THIS host the
 * payload never hits the wire -- it lives in a shared-memory chunk and the reader
 * reads it in place. What travels over the existing reliable channel is a small
 * DESCRIPTOR (segment + chunk + length + generation), not the bytes.
 *
 * Why this shape:
 *   - reuse everything: only the payload delivery changes, per same-host reader.
 *   - no payload fragmentation, no reassembly, no per-datagram syscalls for SHM
 *     readers; the descriptor is tiny and always one datagram.
 *   - reliability collapses to "deliver the descriptor" (already handled) plus
 *     "reader reads before the chunk is recycled" (the refcount + backpressure).
 *   - KEEP_LAST falls out for free: the last K chunks sit in the ring, so a late
 *     same-host joiner just replays K descriptors.
 *
 * The hard parts, called out below: crash-safe reclamation (a reader that dies
 * holding a chunk) and same-host identification (same kernel, not just same
 * subnet). Mixed readership keeps both paths: descriptor to local readers, the
 * normal fragmented-UDP DATA to remote ones.
 *
 * Phasing: this header sketches the ZERO-COPY path from the start (loan on the
 * write side; read-in-place on the read side) because that constraint shapes the
 * whole lifecycle. A one-copy fallback (memcpy into a chunk, keep dart_send as-is)
 * is the same machinery minus the loan API.
 */
#ifndef DART_SHM_H
#define DART_SHM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compile bounds (fixed segment, embedded-friendly: an SBC sets these small). */
#ifndef DART_SHM_CHUNK_BYTES
#define DART_SHM_CHUNK_BYTES (4u*1024u*1024u)  /* max payload per chunk; sized to your biggest message */
#endif
#ifndef DART_SHM_CHUNKS
#define DART_SHM_CHUNKS 16u                    /* ring depth; >= the deepest channel keep_last + slack */
#endif
#ifndef DART_SHM_NAME_MAX
#define DART_SHM_NAME_MAX 64u
#endif

/* ------------------------------------------------------------------ descriptor
 * The payload that rides the existing reliable channel in place of the bytes.
 * Self-describing so a reader that attaches the segment can find the chunk and
 * confirm it is the generation the writer intended (catches recycle races). */
typedef struct {
    uint64_t segment_id;   /* which writer segment (host-unique; see handshake) */
    uint32_t chunk;        /* chunk index in [0, DART_SHM_CHUNKS) */
    uint32_t length;       /* payload bytes */
    uint64_t generation;   /* per-chunk counter; reader rereads it after reading to
                              detect the writer recycling the chunk underneath it */
} dart_shm_desc;

/* 24-byte wire form, little-endian. A node tags an SHM message so the receiver
 * routes it here instead of to the normal reassembly path (e.g. a reserved DATA
 * flag bit, or a distinct submessage type). */
#define DART_SHM_DESC_WIRE 24u
size_t dart_shm_desc_encode(const dart_shm_desc *d, uint8_t out[DART_SHM_DESC_WIRE]);
int    dart_shm_desc_decode(dart_shm_desc *d, const uint8_t *in, size_t len);

/* ------------------------------------------------------------- segment layout
 * One shared segment per writing node (not per channel): a header followed by
 * DART_SHM_CHUNKS fixed-size chunks. All cross-process fields are accessed with
 * atomics (the refcount especially); offsets are segment-relative because the
 * segment maps at different addresses in each process.
 *
 *   [ dart_shm_segment_hdr ][ chunk 0 ][ chunk 1 ] ... [ chunk K-1 ]
 *   each chunk = [ dart_shm_chunk_hdr ][ DART_SHM_CHUNK_BYTES payload ]
 */
typedef struct {
    uint32_t magic;        /* 'DSHM' -- reject a stale/foreign mapping */
    uint32_t version;
    uint64_t segment_id;
    uint32_t chunk_bytes;  /* must match the reader's compile bound or reject */
    uint32_t n_chunks;
    uint32_t owner_pid;    /* for liveness / external cleanup of an orphaned segment */
    uint32_t _pad;
} dart_shm_segment_hdr;

typedef struct {
    uint64_t generation;   /* bumped each time the writer reuses this chunk */
    int32_t  refcount;     /* atomic: writer sets = #local readers; each reader decs */
    uint32_t length;       /* valid payload bytes */
} dart_shm_chunk_hdr;

/* ------------------------------------------------------------------ pool (API)
 * Opaque per-process handle over a mapped segment. The writer CREATES; readers
 * ATTACH by the name carried in discovery. */
typedef struct dart_shm_pool dart_shm_pool;

typedef struct {
    char     name[DART_SHM_NAME_MAX];  /* OS object name; derive from node uuid */
    uint64_t segment_id;
    uint32_t chunk_bytes;              /* 0 => DART_SHM_CHUNK_BYTES */
    uint32_t n_chunks;                 /* 0 => DART_SHM_CHUNKS */
} dart_shm_config;

/* Writer side. create maps a fresh segment (shm_open+ftruncate+mmap / Win32
 * CreateFileMapping+MapViewOfFile). Returns NULL on failure -> caller stays on UDP. */
dart_shm_pool *dart_shm_create(const dart_shm_config *cfg);

/* ZERO-COPY WRITE. loan reserves a free chunk and hands back a writable pointer of
 * at least len bytes; the app fills it directly (no intermediate copy). Returns
 * NULL when the ring is full of chunks still held by readers -- the SHM analog of
 * backpressure_wait_us; the node either waits (reliable) or drops oldest (KEEP_LAST).
 * publish stamps length + generation, sets refcount = n_local_readers, and fills
 * *out_desc to send over the reliable channel. A publish with zero local readers
 * just frees the chunk. */
void *dart_shm_loan   (dart_shm_pool *p, uint32_t len, uint32_t *out_chunk);
int   dart_shm_publish(dart_shm_pool *p, uint32_t chunk, uint32_t len,
                       int n_local_readers, dart_shm_desc *out_desc);

/* Reader side. attach maps an existing segment by name (read-only is fine). */
dart_shm_pool *dart_shm_attach(const dart_shm_config *cfg);

/* Resolve a descriptor to an in-segment pointer, validating magic/bounds and that
 * the chunk generation still matches d->generation (else the writer recycled it
 * and the read is stale -> rely on reliable repair / KEEP_LAST). The node hands
 * this pointer to on_message (valid for that call, matching the existing
 * contract), then calls release -- so the READ path needs no new app API. */
const void *dart_shm_read   (dart_shm_pool *p, const dart_shm_desc *d, uint32_t *out_len);
void        dart_shm_release(dart_shm_pool *p, const dart_shm_desc *d);  /* atomic dec; frees at 0 */

/* --------------------------------------------------------------- reclamation
 * The genuinely hard part. A reader that dies holding a chunk never decs its
 * refcount, so the ring slowly starves. Tie reclamation to the liveness DART
 * already has: when discovery times out a peer, the writer force-releases every
 * chunk that peer was holding. Track per-(reader,chunk) holds so a dead peer's
 * references can be reclaimed without touching live readers' holds.
 * owner_pid in the segment header lets an external janitor reclaim a whole
 * segment whose writer crashed. */
void dart_shm_reclaim_peer(dart_shm_pool *p, uint32_t peer_id);

void dart_shm_detach(dart_shm_pool *p);  /* unmap; writer also unlinks the OS object */

/* ----------------------------------------------------------- same-host check
 * SHM is only valid between processes sharing one kernel AND able to map the same
 * object. A loopback/local-subnet address is necessary but NOT sufficient (NAT,
 * containers, separate namespaces). Exchange a host-UUID (e.g. boot id / machine
 * id) in the discovery meta alongside the fragment size, and the SHM segment name;
 * use SHM only when the peer's host-UUID equals ours and attach succeeds.
 * Returns 1 if the peer is SHM-reachable. */
int dart_shm_peer_local(const uint8_t peer_host_uuid[16], const uint8_t our_host_uuid[16]);

#ifdef __cplusplus
}
#endif
#endif /* DART_SHM_H */
