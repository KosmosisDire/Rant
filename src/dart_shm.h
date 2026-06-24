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
 * plain dart_node_send memcpys into the chunk.
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
#include "dart_plat.h"   /* dart_plat_shm_*, dart_plat_host_uuid, dart_plat_atomic_* */

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

uint32_t dart_shm_class_bytes(uint32_t k);      /* chunk payload bytes for class k */
uint32_t dart_shm_class_for(uint32_t len);      /* smallest class fitting len; N_CLASSES if too big */

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
} dart_shm_desc;

#define DART_SHM_DESC_WIRE 24u   /* little-endian; rides the SHM-DATA submessage body */
size_t dart_shm_desc_encode(const dart_shm_desc *d, uint8_t out[DART_SHM_DESC_WIRE]);
int    dart_shm_desc_decode(dart_shm_desc *d, const uint8_t *in, size_t len);  /* 1 ok, 0 malformed */

/* ------------------------------------------------------------- segment layout
 *   [ dart_shm_seg_hdr ][ chunk 0 ] ... [ chunk N-1 ]
 *   chunk = [ dart_shm_chunk_hdr (padded to 16) ][ chunk_bytes payload ]
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
} dart_shm_seg_hdr;

typedef struct {
    uint64_t generation;    /* bumped each reuse; matched against the descriptor */
    uint32_t length;
    uint32_t _pad;
} dart_shm_chunk_hdr;

#define DART_SHM_MAGIC    0x4D484453u   /* 'DSHM' */
#define DART_SHM_VERSION  1u

/* ------------------------------------------------------------------ pool (API)
 * Opaque per-process handle over one mapped segment, placed in caller memory
 * (the node arena; size via dart_shm_state_bytes). A node CREATEs one segment for
 * its own publishes and ATTACHes one per same-host peer it subscribes to. */
typedef struct dart_shm_pool dart_shm_pool;
size_t dart_shm_state_bytes(void);

typedef struct {
    char     name[DART_SHM_NAME_MAX];  /* writer makes it from its uuid; reader gets it via meta */
    uint64_t segment_id;
    uint32_t chunk_bytes;              /* create only (0 => DART_SHM_CHUNK_BYTES); attach reads it from the header */
    uint32_t n_chunks;                 /* create only (0 => DART_SHM_CHUNKS); attach reads it from the header */
} dart_shm_config;

/* Writer. create maps a fresh segment (dart_plat_shm_create); NULL => stay on UDP. */
dart_shm_pool *dart_shm_create(void *pool_mem, const dart_shm_config *cfg);
/* The chunk backing a history slot: loan returns a writable pointer (app fills it),
 * stamp bumps generation + sets length and fills *out for the transport to frame in
 * the SHM-DATA submessage. The node owns chunk<->slot assignment (1 chunk per
 * keep_last slot), so there is no free list here. */
void *dart_shm_chunk(dart_shm_pool *p, uint32_t chunk, uint32_t *out_cap);
void  dart_shm_stamp(dart_shm_pool *p, uint32_t chunk, uint32_t len, dart_shm_desc *out);

/* Reader. attach maps an existing segment WHOLE by name and reads its geometry
 * (chunk_bytes/n_chunks) from the header the writer stamped, so the reader needs to
 * know nothing about its size; validates magic/version/owner_host==ours and that the
 * geometry fits the mapped object. NULL => fall back to the UDP path. read resolves a
 * descriptor to an in-segment pointer and verifies generation still matches (else
 * recycled -> NULL, reliable repair covers it). No release call: the reader's
 * transport ACK of the range is the release. */
dart_shm_pool *dart_shm_attach(void *pool_mem, const dart_shm_config *cfg);
const void    *dart_shm_read  (dart_shm_pool *p, const dart_shm_desc *d, uint32_t *out_len);
/* re-check the chunk generation AFTER a one-copy read (seqlock tail): 1 if it still
 * matches d (the copy is clean), 0 if a best-effort writer recycled it mid-copy (the
 * copy may be torn -> discard). Lock-free: the writer never blocks. */
int            dart_shm_verify(dart_shm_pool *p, const dart_shm_desc *d);

void dart_shm_detach(dart_shm_pool *p);  /* unmap; the writer also unlinks the OS object */

/* Same-host id: SHM is valid only between processes sharing one kernel AND able to
 * map the object (loopback addr alone is not sufficient -- containers/namespaces).
 * The node advertises dart_plat_host_uuid() + segment name in discovery; a peer is
 * SHM-reachable iff its host uuid equals ours and dart_shm_attach succeeds. */
int dart_shm_host_match(const uint8_t peer_host[16], const uint8_t our_host[16]);

/* === INTEGRATION (implemented under #ifdef DART_SHM) ========================
 *
 * dart_plat (add behind the existing Windows/POSIX split):
 *   void *dart_plat_shm_create(const char *name, size_t bytes, void **handle);
 *   void *dart_plat_shm_attach(const char *name, size_t *out_bytes, void **handle);
 *   void  dart_plat_shm_detach(void *base, size_t bytes, void *handle, int unlink_it);
 *   void  dart_plat_host_uuid(uint8_t out[16]);             (boot id / machine guid)
 *   uint64_t dart_plat_atomic_load64 / _store64(volatile uint64_t*[, v]);  (generation)
 *
 * transport (the per-peer lane + the new submessage; the only core change):
 *   - per-peer flag peer_shm[] (node sets it; like the existing peer_local/peer_frag)
 *   - a history sample may be chunk-backed: a publish that hands in an external
 *     buffer (the chunk) + its descriptor, so dart_send does not memcpy (zero copy)
 *   - SHM-DATA submessage: byte0 = DATA | DART_F_SHM, [alias][base seqno][count]
 *     [24-byte descriptor]. Writer lane emits it for an SHM peer instead of frags;
 *     reader marks [base,base+count) delivered, hands the descriptor up flagged.
 *   - delivery carries an "is SHM descriptor" flag to the node (the public app
 *     on_message is unchanged; the node wraps it -- see below)
 *
 * node (wiring):
 *   - advertise: meta blob -> ver 3, insert [u8 shm][u8 host[16]][u64 segment_id]
 *     between the frag prefix and the interest list (ver-2 peers ignore it)
 *   - on peer up: if peer.shm and host matches ours, dart_shm_attach its segment and
 *     set peer_shm in the transport; on down/dormant, detach / clear it
 *   - dart_node_loan(n, ch, len, &ptr) / dart_node_publish(n, ch): loan a chunk for
 *     the channel's next history slot, app fills ptr, publish hands the chunk +
 *     descriptor to the transport. dart_node_send keeps working (one-copy into the
 *     chunk when the channel has any SHM peer, else plain inline)
 *   - on receive: the node's on_message wrapper sees the SHM flag, dart_shm_read the
 *     descriptor, calls the app on_message with the in-place pointer
 */

#ifdef __cplusplus
}
#endif
#endif /* DART_SHM_H */
