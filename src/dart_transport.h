/* sans-IO reliable-UDP transport core. Owns no socket,
 * clock, or heap: feed it datagrams plus now_us and a peer set; it returns
 * datagrams to send and delivers reassembled samples via callback. RTPS-inspired
 * but not wire-compatible. Layer dart_node.h on top for a socket-owning node. */
#ifndef DART_TRANSPORT_H
#define DART_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DART_FRAG_PAYLOAD
#define DART_FRAG_PAYLOAD 1024u          /* bytes of sample data per TU */
#endif
#define DART_DGRAM_MAX (DART_FRAG_PAYLOAD + 32u)   /* + largest header */

typedef enum { DART_BEST_EFFORT = 0, DART_RELIABLE = 1 } dart_reliability;
/* DART_NONE: declared but inactive. All resources stay allocated at init;
 * dart_set_dir flips interest at runtime. */
typedef enum { DART_PUBSUB = 0, DART_PUB_ONLY = 1, DART_SUB_ONLY = 2,
               DART_NONE = 3 } dart_direction;

/* Built-in channel carrying pub/sub interest between peers, appended to every
 * channel table. Interest lists ride it as ordinary reliable KEEP_LAST(1)
 * samples: latest list wins, late joiners get a replay, subscription changes
 * are just new samples. The id is reserved; dart_init rejects it. */
#define DART_CHAN_META 0xFFFFu

typedef struct {
    dart_reliability reliability;
    uint16_t history_depth;     /* KEEP_LAST depth in samples (>=1)        */
    uint16_t join_replay;       /* reliable: cached samples replayed to a late-
                                   joining reader (capped at what history still
                                   holds). 0 (default) = join at the head and
                                   see only future samples; 1 = latest-value on
                                   join. Deep replays burst-multiply at startup
                                   (every joiner gets depth x sample_bytes at
                                   once), so keep this small.               */
    uint32_t max_sample_bytes;  /* largest sample on this channel          */
    uint32_t heartbeat_us;      /* reliable: writer heartbeat cadence      */
    uint32_t nack_delay_us;     /* reader: delay before NACK (suppression) */
    uint32_t max_block_us;      /* reliable: how long a send may wait for slow
                                   readers to ack before overwriting un-acked
                                   history. 0 (default) = pure KEEP_LAST. Worst
                                   case is min(max_block_us, peer timeout), since
                                   a dead reader's proxy also releases pressure.
                                   sans-IO callers wait via dart_send_would_evict. */
} dart_qos;

typedef struct {
    uint16_t channel_id;
    dart_qos   qos;
    uint8_t  dir;     /* dart_direction; 0 (zero-init) = publish + subscribe   */
    uint8_t  mcast;   /* 1 = new data and heartbeats may use a multicast group,
                         engaged only while remote subscribers exist. Same-host
                         subscribers always stay unicast. Repairs, acks, and
                         gap-to-one-reader stay unicast. Reliable mcast is
                         reliable-from-join-point: history replay on join is
                         unicast-only. */
} dart_channel_def;

/* dart_poll_send destination: peer id, or a per-channel multicast group flagged
 * by the top bit (peer ids are small and nonzero). */
#define DART_DEST_GROUP_BIT      0x80000000u
#define DART_DEST_GROUP(ch)      (DART_DEST_GROUP_BIT | (uint32_t)(ch))
#define DART_DEST_IS_GROUP(d)    (((d) & DART_DEST_GROUP_BIT) != 0u)
#define DART_DEST_GROUP_CHAN(d)  ((uint16_t)((d) & 0xFFFFu))

/* Complete sample delivered. Do not call back into dart_* for this state. */
typedef void (*dart_sample_fn)(void *user, uint16_t channel_id, uint32_t from_peer,
                             const void *data, size_t len);

/* Reader permanently skips TUs from a peer: writer superseded them (KEEP_LAST
 * eviction) or best-effort data was lost. first..first+count-1 are TU seqnos.
 * Not called for the initial join position: a late joiner adopting the writer's
 * head has lost nothing it expected. Same reentrancy rule as dart_sample_fn. */
typedef void (*dart_gap_fn)(void *user, uint16_t channel_id, uint32_t from_peer,
                          uint64_t first_tu, uint64_t count);

typedef struct {
    const dart_channel_def *channels;
    uint16_t              n_channels;
    uint16_t              max_peers;
    uint16_t              meta_max_ids; /* largest pub+sub id count accepted in a
                                           peer's interest list; bounds the meta
                                           channel's sample size (2 bytes per id).
                                           0 = 1024. Raised to fit our own table. */
    dart_sample_fn          on_sample;
    dart_gap_fn             on_gap;     /* optional; NULL = no gap reporting */
    void                 *user;
} dart_config;

typedef struct dart_state dart_state;

size_t    dart_required_memory(const dart_config *cfg);
dart_state *dart_init(void *mem, size_t mem_size, const dart_config *cfg);

/* A new peer matches only the meta channel; data channels match as its
 * interest list arrives over it, and rematch on every change (theirs via new
 * lists, ours via dart_set_dir). peer_is_local: 1 if on this host; local
 * subscribers of mcast channels stay unicast, and the group lane engages only
 * with a remote one. */
void      dart_peer_add   (dart_state *st, uint32_t peer_id, int peer_is_local);
void      dart_peer_remove(dart_state *st, uint32_t peer_id);

/* Change our own interest in a channel at runtime. Creates/destroys proxies
 * against every live peer and announces the new list on the meta channel.
 * A (re)subscribe joins like a late joiner: reliable channels replay cached
 * history, nothing is reported as a gap. Returns 0 ok, <0 unknown channel. */
int       dart_set_dir(dart_state *st, uint16_t channel_id, uint8_t dir);

/* Publish a sample to all peers. Returns 0 ok, <0 on error. */
int       dart_send(dart_state *st, uint16_t channel_id, const void *data, size_t len,
                  uint64_t now_us);

/* The channel's qos as stored at init; NULL if the channel is unknown. */
const dart_qos *dart_channel_qos(dart_state *st, uint16_t channel_id);

/* Backpressure signal. 1 if appending a sample on this reliable channel would
 * overwrite history not yet acked by every live reader; 0 otherwise. A writer
 * accepting pressure pumps its loop while this returns 1, then sends anyway
 * after qos.max_block_us (KEEP_LAST eviction is the fallback, never refusal). */
int       dart_send_would_evict(dart_state *st, uint16_t channel_id);

/* Feed a received datagram, tagged with the peer it came from. */
void      dart_on_datagram(dart_state *st, uint32_t from_peer, const void *dg, size_t len,
                         uint64_t now_us);

/* Pull one outgoing datagram (may batch several submessages for one peer).
 * Returns 1 and fills *to_peer/out/out_len, or 0 when nothing is due. Loop until
 * 0. Pass DART_DGRAM_MAX of cap; smaller caps just batch less. */
int       dart_poll_send(dart_state *st, uint32_t *to_peer, void *out, size_t cap,
                       size_t *out_len, uint64_t now_us);

#ifdef __cplusplus
}
#endif
#endif /* DART_TRANSPORT_H */
