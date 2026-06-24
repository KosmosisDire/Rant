/* NODE runtime: owns the data sockets, drives discovery, and the clock; it wires
 * peers into the transport via the sans-IO node core (node/core.h). All OS access
 * goes through dart_plat. See node/runtime.h for the public dart_node_* API. */

#include "runtime.h"   /* public dart_node_* API + config structs */
#include "core.h"      /* sans-IO node core: peer table + discovery->transport lifecycle */
#include "../discovery/runtime.h"
#include "../platform/core.h"
#ifdef DART_SHM
#include "../shm/core.h"
#endif
#include "../common/arena.h"
#include <string.h>

struct dart_node {
    dart_state     *transport;
    dart_node_core *core;     /* peer table (id<->address) + discovery lifecycle (sans-IO) */
    dart_discovery_rt     *discovery;
    dart_sock       fd;       /* unicast data socket (also group TX) */
    dart_sock       multicast_fd;     /* multicast data RX socket (DART_SOCK_BAD if unused) */
    uint16_t      domain;
    uint16_t      multicast_port;
    /* datagram the socket refused; retried first next poll so it is never lost */
    uint8_t       tx_hold[DART_DGRAM_MAX];
    size_t        tx_hold_len;
    uint32_t      tx_hold_peer;
    /* backpressure accumulators, read via dart_node_backpressure_stats */
    uint64_t      backpressure_total_us;
    uint32_t      backpressure_wait_count;
    /* diagnostic: in-pump probe sampled inside the backpressure wait (dart_node_send) */
    dart_pump_probe_fn pump_probe;
    void          *pump_probe_user;
    uint64_t       pump_probe_interval_us;
    /* one app callback for everything but message delivery; node fills PEER_UP/DOWN */
    dart_event_fn  on_event;
    void          *user_data;
#ifdef DART_SHM
    /* zero-fragment same-host path: lazy per-size-class segments (see dart_shm.h) */
    uint8_t        shm_capable;       /* 1 = an allocator is set, so SHM is usable */
    uint8_t        shm_host[16];  /* our host uuid (advertised; same-host check) */
    uint64_t       shm_base;      /* per-node segment id base; low 19 bits = (channel<<3)|class */
    dart_message_fn shm_user_on_message;  /* the app's real callback (we wrap it) */
    dart_alloc_fn  shm_alloc;     /* one-copy receive scratch (== cfg.allocator) */
    void          *shm_scratch; uint32_t shm_scratch_cap;
    void         **shm_pool;      /* [nchan*N_CLASSES] our (channel,class) segments (NULL = not created) */
    uint8_t       *shm_pool_mem;  /* arena: nchan * N_CLASSES * dart_shm_state_bytes */
    uint16_t       shm_n_channels;
    uint64_t      *shm_reader_segments;      /* [reader_max] attached reader-segment ids (0 = empty) */
    uint8_t       *shm_reader_pool_mem; /* [reader_max * dart_shm_state_bytes] */
    uint16_t       shm_reader_max;
    uint32_t       shm_tx, shm_rx;/* messages published / delivered via SHM (observability) */
#endif
};

#ifdef DART_SHM
/* reader-pool cache capacity: a same-host peer publishes on its channels, one segment
 * each, so size to peers x channels (clamped to the u16 the cache index uses) */
static uint16_t dart__node_shm_reader_max(uint16_t max_peers, uint16_t n_channels){
    uint32_t r = (uint32_t)max_peers * (n_channels ? n_channels : 1u) * DART_SHM_N_CLASSES;
    return (uint16_t)(r > 0xFFFFu ? 0xFFFFu : r);
}
#endif

/* split node config into discovery + transport sub-configs */
static void dart__node_cfgs(const dart_node_config *cfg, dart_discovery_rt_config *discovery_rt_cfg,
                          dart_config *transport_cfg, uint16_t *max_peers_out){
    uint16_t max_peers = cfg->discovery.max_peers ? cfg->discovery.max_peers : 16;
    memset(discovery_rt_cfg, 0, sizeof *discovery_rt_cfg); memset(transport_cfg, 0, sizeof *transport_cfg);
    discovery_rt_cfg->discovery.domain_id   = cfg->domain;
    discovery_rt_cfg->discovery.data_port   = cfg->net.data_port;
    discovery_rt_cfg->discovery.announce_interval_us = cfg->discovery.announce_interval_us; /* 0 uses default */
    discovery_rt_cfg->discovery.peer_timeout_us  = cfg->discovery.peer_timeout_us;
    discovery_rt_cfg->discovery.max_peers   = max_peers;
    discovery_rt_cfg->discovery.meta_capacity    = dart_meta_capacity(cfg->n_channels);
    discovery_rt_cfg->group            = cfg->net.discovery_group;
    discovery_rt_cfg->discovery_port        = cfg->net.discovery_port;
    discovery_rt_cfg->ttl              = cfg->net.multicast_ttl;
    discovery_rt_cfg->multicast_interface         = cfg->net.multicast_interface;
    discovery_rt_cfg->seeds            = cfg->net.seed_peers;
    discovery_rt_cfg->n_seeds          = cfg->net.n_seed_peers;
    transport_cfg->channels   = cfg->channels;
    transport_cfg->n_channels = cfg->n_channels;
    transport_cfg->max_peers  = max_peers;
    transport_cfg->frag_payload = cfg->net.fragment_size;   /* 0 = default; dart_init clamps to [MIN,MAX] */
    transport_cfg->on_message = cfg->on_message;
    transport_cfg->on_event   = cfg->on_event;
    transport_cfg->allocator  = cfg->allocator;
    transport_cfg->user       = cfg->user_data;
    if (max_peers_out) *max_peers_out = max_peers;
}

/* The node arena's sub-blocks, laid out in ONE place so dart_node_required_memory
 * (measure: bump.base NULL, read bump.offset) and dart_node_open (build: read the
 * pointers) run the same dart_take sequence and can never drift. */
typedef struct {
    dart_node *n;
    uint8_t   *node_core, *transport, *discovery;
#ifdef DART_SHM
    uint8_t   *shm_pool, *shm_pool_mem, *shm_reader_segments, *shm_reader_pool;
#endif
    size_t     node_core_bytes, transport_bytes, discovery_bytes;
} dart_node_blocks;

static void dart__node_layout(dart_bump *b, const dart_node_config *cfg, uint16_t max_peers,
                              const dart_config *transport_cfg,
                              const dart_discovery_rt_config *discovery_rt_cfg, dart_node_blocks *o){
    o->n     = (dart_node*)dart_take(b, sizeof(struct dart_node), 16);
    o->node_core_bytes = dart_node_core_required_memory(max_peers, cfg->n_channels);
    o->node_core = (uint8_t*)dart_take(b, o->node_core_bytes, 16);
    o->transport_bytes = dart_required_memory(transport_cfg);
    o->transport = (uint8_t*)dart_take(b, o->transport_bytes, 16);
#ifdef DART_SHM
    {   size_t state_bytes  = dart_shm_state_bytes();
        uint32_t n_segments = (uint32_t)cfg->n_channels * DART_SHM_N_CLASSES;
        uint16_t reader_max = dart__node_shm_reader_max(max_peers, cfg->n_channels);
        o->shm_pool            = (uint8_t*)dart_take(b, (size_t)n_segments * sizeof(void*), 16);
        o->shm_pool_mem        = (uint8_t*)dart_take(b, (size_t)n_segments * state_bytes, 16);
        o->shm_reader_segments = (uint8_t*)dart_take(b, (size_t)reader_max * 8u, 16);
        o->shm_reader_pool     = (uint8_t*)dart_take(b, (size_t)reader_max * state_bytes, 16);
    }
#endif
    o->discovery_bytes = dart_discovery_rt_required_memory(discovery_rt_cfg);
    o->discovery = (uint8_t*)dart_take(b, o->discovery_bytes, 16);
}

/* node-core hook: 1 if a peer address is on this host (a route probe selecting that
 * same address as source). Drives OOB/SHM eligibility; the core stays platform-free. */
static int dart__node_is_local(void *user, const uint8_t *ip, uint8_t ip_len){
    uint32_t d;
    (void)user;
    if (ip_len != 4) return 0;
    d = dart_plat_ip4_to_naddr(ip);
    return dart_plat_route_src(d, 7) == d;
}

/* data multicast group from a selector (topic identity & 0xFF); &0xFF wrap is
 * harmless, RX filters by peer table + identity */
static uint32_t dart__node_group_addr(uint16_t domain, uint16_t sel){
    return dart_plat_ipv4(239u, 255u, (uint8_t)(domain & 0xFFu), (uint8_t)(sel & 0xFFu));
}
/* a channel's group, from its identity so it matches the core's group send and every peer */
static uint32_t dart__node_chan_group(uint16_t domain, const dart_channel_def *def){
    return dart__node_group_addr(domain, DART_DEST_GROUP_SEL(dart_channel_identity(def)));
}

/* send one datagram; returns 1 when done with it, 0 only on a would-block TX-full. The
 * core resolves the abstract destination; we just map it to a UDP address. */
static int dart__node_tx(dart_node *n, uint32_t to, const uint8_t *buf, size_t len){
    dart_node_dest d; uint8_t ip[4]; uint16_t port;
    if (!dart_node_core_resolve(n->core, to, &d)) return 1;   /* peer vanished */
    if (d.is_group){
        dart_plat_naddr_to_ip4(dart__node_group_addr(n->domain, d.group_sel), ip);
        port = n->multicast_port;
    } else {
        memcpy(ip, d.ip, 4);
        port = d.port;
    }
    if (dart_plat_send(n->fd, buf, len, ip, port) < 0 && dart_plat_would_block())
        return 0;
    return 1;
}

#ifdef DART_SHM
/* lazily create our per-channel segment: chunk_bytes = the channel's locked size class,
 * n_chunks = its keep_last, so history slot i binds chunk i (no free list). NULL on fail. */
static dart_shm_pool *dart__shm_chan_pool(dart_node *n, uint16_t ch, uint32_t k, uint16_t keep_last){
    dart_shm_config c; uint8_t *mem; uint64_t seg; size_t idx;
    if (k >= DART_SHM_N_CLASSES) return NULL;
    idx = (size_t)ch * DART_SHM_N_CLASSES + k;
    if (n->shm_pool[idx]) return (dart_shm_pool*)n->shm_pool[idx];
    memset(&c, 0, sizeof c);
    seg = n->shm_base | ((uint64_t)ch << 3) | (uint64_t)k;   /* low 3 bits class, next 16 channel */
    c.segment_id = seg;
    dart_shm_seg_name(c.name, seg);
    c.chunk_bytes = dart_shm_class_bytes(k);
    c.n_chunks = keep_last ? keep_last : 1u;
    mem = n->shm_pool_mem + idx * dart_shm_state_bytes();
    n->shm_pool[idx] = dart_shm_create(mem, &c);
    return (dart_shm_pool*)n->shm_pool[idx];
}
/* lazily attach a peer's segment by id (class is in the low bits); cache it. */
static dart_shm_pool *dart__shm_reader_pool(dart_node *n, uint64_t seg){
    uint16_t i, slot = 0xFFFF; dart_shm_config c; uint8_t *mem; size_t state_bytes;
    state_bytes = dart_shm_state_bytes();
    for (i=0;i<n->shm_reader_max;i++){
        if (n->shm_reader_segments[i]==seg) return (dart_shm_pool*)(n->shm_reader_pool_mem + (size_t)i*state_bytes);
        if (n->shm_reader_segments[i]==0 && slot==0xFFFF) slot = i;
    }
    if (slot==0xFFFF) return NULL;                         /* cache full */
    memset(&c, 0, sizeof c);
    c.segment_id = seg; dart_shm_seg_name(c.name, seg);     /* attach maps whole + reads geometry */
    mem = n->shm_reader_pool_mem + (size_t)slot*state_bytes;
    if (!dart_shm_attach(mem, &c)) return NULL;
    n->shm_reader_segments[slot] = seg;
    return (dart_shm_pool*)mem;
}
/* transport->node delivery wrappers (the transport's user is the node so on_shm can
 * reach SHM state; the app's single on_message sees identical bytes inline or SHM).
 * Because the transport shares one user across on_message/on_shm/allocator, we also
 * wrap the allocator to forward the app's real user_data. */
static void *dart__node_alloc(void *u, void *ptr, size_t size){
    dart_node *n = (dart_node*)u;
    return n->shm_alloc(n->user_data, ptr, size);
}
static void dart__node_on_message(void *u, uint16_t ch, uint32_t from, const void *data, size_t len){
    dart_node *n = (dart_node*)u;
    if (n->shm_user_on_message) n->shm_user_on_message(n->user_data, ch, from, data, len);
}
/* transport events (MSG_LOST/TOO_BIG/NAME_COLLISION) fire with the transport's user,
 * which the rewrap below points at the node; forward them to the app's real user_data
 * (same chokepoint as on_message so the wrap can never go half-applied again). */
static void dart__node_on_event(void *u, const dart_event *ev){
    dart_node *n = (dart_node*)u;
    if (n->on_event) n->on_event(n->user_data, ev);
}
static int dart__node_on_shm(void *u, uint16_t ch, uint32_t from, const uint8_t *desc){
    dart_node *n = (dart_node*)u; dart_shm_desc d; dart_shm_pool *reader_pool; const void *p; uint32_t len;
    if (!dart_shm_desc_decode(&d, desc, DART_SHM_DESC_WIRE)) return 0;
    reader_pool = dart__shm_reader_pool(n, d.segment_id);
    if (!reader_pool) return 0;                                     /* can't attach -> reader NACKs (repair) */
    p = dart_shm_read(reader_pool, &d, &len);                       /* seqlock head: generation == descriptor? */
    if (!p) return 0;                                      /* recycled -> NACK -> repair or skip */
    /* one-copy: copy out of shared memory so the user owns the bytes (ack-timing safe) */
    if (len > n->shm_scratch_cap){
        void *new_buf = n->shm_alloc(n->user_data, n->shm_scratch, len?len:1u);
        if (!new_buf) return 0;
        n->shm_scratch = new_buf; n->shm_scratch_cap = len;
    }
    memcpy(n->shm_scratch, p, len);
    if (!dart_shm_verify(reader_pool, &d)) return 0;                /* seqlock tail: writer recycled mid-copy -> torn -> drop */
    n->shm_rx++;
    if (n->shm_user_on_message) n->shm_user_on_message(n->user_data, ch, from, n->shm_scratch, len);
    return 1;
}
#endif

size_t dart_node_required_memory(const dart_node_config *cfg){
    dart_discovery_rt_config discovery_rt_cfg; dart_config transport_cfg; uint16_t max_peers;
    dart_bump b; dart_node_blocks blocks;
    if (!cfg || cfg->n_channels==0) return 0;
    dart__node_cfgs(cfg,&discovery_rt_cfg,&transport_cfg,&max_peers);
    memset(&b,0,sizeof b);
    dart__node_layout(&b, cfg, max_peers, &transport_cfg, &discovery_rt_cfg, &blocks);
    return b.offset + 32u;     /* slack to align the caller's mem up to base */
}

dart_node *dart_node_open(void *mem, size_t cap, const dart_node_config *cfg){
    dart_discovery_rt_config discovery_rt_cfg; dart_config transport_cfg; uint16_t max_peers;
    uint8_t *base; dart_node_blocks blocks;
    dart_node *n; dart_sock fd; uint16_t local_port;
    if (!mem || !cfg || cfg->n_channels==0) return NULL;
    if (cap < dart_node_required_memory(cfg)) return NULL;
    dart__node_cfgs(cfg,&discovery_rt_cfg,&transport_cfg,&max_peers);
    base = (uint8_t*)(((uintptr_t)mem+15u)&~(uintptr_t)15u);
    {   dart_bump b; memset(&b,0,sizeof b);
        b.base = base; b.cap = cap - (size_t)(base - (uint8_t*)mem);
        dart__node_layout(&b, cfg, max_peers, &transport_cfg, &discovery_rt_cfg, &blocks);
    }

    if (!dart_plat_startup()) return NULL;

    n=blocks.n; memset(n,0,sizeof *n);
    n->fd = DART_SOCK_BAD; n->multicast_fd = DART_SOCK_BAD;
    n->domain = cfg->domain;
    n->on_event = cfg->on_event; n->user_data = cfg->user_data;
    n->multicast_port = cfg->net.multicast_port ? cfg->net.multicast_port
               : (uint16_t)((cfg->net.discovery_port ? cfg->net.discovery_port : 7400) + 1);
#ifdef DART_SHM
    /* SHM is usable only with an allocator (the one-copy receive scratch + dynamic
       buffers); advertise capability accordingly and wrap the transport callbacks so
       on_shm can reach node state. The big segments are OS-mapped lazily, not here. */
    n->shm_capable = (uint8_t)(cfg->allocator != NULL);
    if (n->shm_capable){
        dart_plat_host_uuid(n->shm_host);
        if (!dart_plat_random(&n->shm_base, sizeof n->shm_base)) n->shm_base = dart_plat_pid();
        n->shm_base ^= (uint64_t)dart_plat_pid() << 32;    /* fold in pid for cross-process uniqueness */
        n->shm_base &= ~(((uint64_t)1u << 19) - 1u);       /* low 19 bits: 3 class + 16 channel index */
        if (n->shm_base == 0) n->shm_base = (uint64_t)1u << 19;
        n->shm_user_on_message = cfg->on_message;
        n->shm_alloc = cfg->allocator;
        transport_cfg.on_message = dart__node_on_message;
        transport_cfg.on_event   = dart__node_on_event;
        transport_cfg.on_shm     = dart__node_on_shm;
        transport_cfg.allocator  = dart__node_alloc;
        transport_cfg.user       = n;
    }
#endif
    n->transport = dart_init(blocks.transport, blocks.transport_bytes, &transport_cfg);
    if (!n->transport) goto fail_startup;
#ifdef DART_SHM
    {   uint32_t n_segments = (uint32_t)cfg->n_channels * DART_SHM_N_CLASSES;
        uint16_t reader_max = dart__node_shm_reader_max(max_peers, cfg->n_channels); uint32_t i;
        n->shm_n_channels = cfg->n_channels; n->shm_reader_max = reader_max;
        n->shm_pool            = (void**)blocks.shm_pool;
        n->shm_pool_mem        = blocks.shm_pool_mem;
        n->shm_reader_segments = (uint64_t*)blocks.shm_reader_segments;
        n->shm_reader_pool_mem = blocks.shm_reader_pool;
        for (i=0;i<n_segments;i++) n->shm_pool[i]=NULL;
        for (i=0;i<reader_max;i++) n->shm_reader_segments[i]=0;
    }
#endif

    /* sans-IO node core: owns the peer table (id<->address) and the discovery->
       transport lifecycle; the runtime drives it and resolves addresses for IO. */
    {   dart_node_core_config cc; memset(&cc, 0, sizeof cc);
        cc.transport = n->transport; cc.max_peers = max_peers;
        cc.n_channels = cfg->n_channels; cc.frag_size = dart_clamp_frag(cfg->net.fragment_size);
        cc.on_event = cfg->on_event; cc.user = cfg->user_data;
        cc.is_local = dart__node_is_local; cc.is_local_user = NULL;
#ifdef DART_SHM
        cc.oob_capable = n->shm_capable; memcpy(cc.oob_host, n->shm_host, 16);
#endif
        n->core = dart_node_core_init(blocks.node_core, blocks.node_core_bytes, &cc);
        if (!n->core) goto fail_startup;
    }

    /* Bind the data socket before opening discovery so we can advertise its real
       port (0 => OS ephemeral, read back via getsockname). No reuse: a unicast
       endpoint owns its port, so a collision fails loudly here. */
    fd = dart_plat_udp_open();
    if (fd==DART_SOCK_BAD) goto fail_startup;
    n->fd=fd;                       /* owned now: fail_sock closes it */
    if (!dart_plat_bind(fd, 0, cfg->net.data_port, 0)) goto fail_sock;
    local_port = dart_plat_local_port(fd);
    if (local_port==0) goto fail_sock;
    dart_plat_set_nonblock(fd);   /* never block in recv/send; poll drains the queue */
    /* suppress WSAECONNRESET from a bounced send leaking into the shared RX path */
    dart_plat_suppress_connreset(fd);
    if (cfg->net.recv_buffer_bytes) dart_plat_set_rcvbuf(fd, (int)cfg->net.recv_buffer_bytes);
    if (cfg->net.send_buffer_bytes) dart_plat_set_sndbuf(fd, (int)cfg->net.send_buffer_bytes);
    discovery_rt_cfg.discovery.data_port = local_port;       /* advertise the actual port */

    /* multicast data: group TX rides the unicast socket; group RX needs its own
       socket on the shared multicast_port with one IGMP join per subscribed channel */
    { int want_rx=0, want_tx=0; uint16_t i; uint32_t interface_ip;
      for (i=0;i<cfg->n_channels;i++) if (cfg->channels[i].multicast){
          if (cfg->channels[i].role!=DART_PUB_ONLY) want_rx=1;
          if (cfg->channels[i].role!=DART_SUB_ONLY) want_tx=1;
      }
      /* pin data multicast to discovery's egress interface, else multihomed hosts
         pick per-group interfaces and break peer identification by source addr */
      interface_ip = !(want_rx||want_tx) ? 0
           : cfg->net.multicast_interface ? dart_plat_parse_ip(cfg->net.multicast_interface)
           : dart_plat_route_src(dart_plat_parse_ip(cfg->net.discovery_group?cfg->net.discovery_group:"239.255.0.7"),
                                  cfg->net.discovery_port?cfg->net.discovery_port:7400);
      if (want_tx){
          unsigned char multicast_ttl = cfg->net.multicast_ttl ? cfg->net.multicast_ttl : 1;
          dart_plat_mcast_setif(n->fd, interface_ip);
          dart_plat_mcast_ttl(n->fd, multicast_ttl);
          dart_plat_mcast_loop(n->fd, 1);
      }
      if (want_rx){
          dart_sock multicast_fd = dart_plat_udp_open();
          if (multicast_fd==DART_SOCK_BAD) goto fail_sock;
          n->multicast_fd=multicast_fd;       /* owned now: fail_mcast closes it */
          if (!dart_plat_bind(multicast_fd, 0, n->multicast_port, 1)) goto fail_mcast;
          /* one join per distinct group (kernels reject dups); memberships are
             OS-capped (~20: Linux net.ipv4.igmp_max_memberships). Past the cap a join
             fails: degrade instead of failing the whole node -- skip that channel's
             group (it still receives data sent to it unicast) and fire a diagnostic so
             the over-subscription is never silent. */
          for (i=0;i<cfg->n_channels;i++)
              if (cfg->channels[i].multicast && cfg->channels[i].role!=DART_PUB_ONLY){
                  uint32_t group_addr = dart__node_chan_group(cfg->domain, &cfg->channels[i]);
                  uint16_t j; int dup=0;
                  for (j=0;j<i;j++)
                      if (cfg->channels[j].multicast && cfg->channels[j].role!=DART_PUB_ONLY
                          && dart__node_chan_group(cfg->domain, &cfg->channels[j])==group_addr){
                          dup=1; break;
                      }
                  if (dup) continue;
                  if (!dart_plat_mcast_join(multicast_fd, group_addr, interface_ip) && n->on_event){
                      dart_event ev; memset(&ev, 0, sizeof ev);
                      ev.kind=DART_MCAST_JOIN_FAILED; ev.channel=i;
                      ev.detail="multicast group join failed (over OS membership cap); channel receives unicast only";
                      n->on_event(n->user_data, &ev);
                  }
              }
          dart_plat_mcast_loop(multicast_fd, 1);
          dart_plat_set_nonblock(multicast_fd);
          if (cfg->net.recv_buffer_bytes) dart_plat_set_rcvbuf(multicast_fd, (int)cfg->net.recv_buffer_bytes);
      } }

    discovery_rt_cfg.discovery.on_peer_up      = dart_node_core_peer_up;
    discovery_rt_cfg.discovery.on_peer_down    = dart_node_core_peer_down;
    discovery_rt_cfg.discovery.on_peer_refused = dart_node_core_peer_refused;
    discovery_rt_cfg.discovery.user            = n->core;
    /* the core owns + builds our announce blob (frag size + OOB host + interest); we
       just hand its bytes to discovery so peers reassemble and match from discovery */
    dart_node_core_build_meta(n->core);
    discovery_rt_cfg.discovery.meta = dart_node_core_meta(n->core, &discovery_rt_cfg.discovery.meta_len);
    n->discovery = dart_discovery_rt_open(blocks.discovery, blocks.discovery_bytes, &discovery_rt_cfg);
    if (!n->discovery) goto fail_mcast;

    return n;

fail_mcast:
    if (n->multicast_fd != DART_SOCK_BAD) dart_plat_close(n->multicast_fd);
    n->multicast_fd = DART_SOCK_BAD;
fail_sock:
    if (n->fd != DART_SOCK_BAD) dart_plat_close(n->fd);
    n->fd = DART_SOCK_BAD;
fail_startup:
    dart_plat_cleanup();
    return NULL;
}

/* max wall-time draining RX (and running on_message) per poll tick before
 * yielding to discovery, so a slow on_message never starves it */
#ifndef DART_RX_BUDGET_US
#define DART_RX_BUDGET_US 5000u
#endif

/* drain one socket's RX queue into the transport until empty or past deadline
 * (full drain avoids NACK storms). Distinct from public dart_node_drain. */
static void dart__node_rx_drain(dart_node *n, dart_sock fd, uint64_t deadline){
    uint8_t buf[DART_DGRAM_MAX];
    for (;;){
        uint8_t src_ip[4]; uint16_t src_port;
        int r = dart_plat_recv(fd, buf, sizeof buf, src_ip, &src_port);
        if (r<0){
            if (dart_plat_would_block()) break;        /* queue empty */
            continue;   /* per-datagram error (e.g. bounced send); keep draining */
        }
        if (r>0){
            if (r>=4 && buf[0]=='u' && buf[1]=='D' && buf[2]=='S' && buf[3]=='C'){
                /* unicast announce aimed at our data port: hand it to discovery */
                dart_discovery_rt_feed(n->discovery, src_ip, 4, buf, (size_t)r);
            } else {
                uint32_t from;
                if (dart_node_core_id_for_addr(n->core, src_ip, src_port, &from))
                    dart_on_datagram(n->transport, from, buf, (size_t)r, dart_plat_now_us());
            }
        }
        if (dart_plat_now_us() >= deadline) break;      /* yield to discovery/send */
    }
}

int dart_node_poll(dart_node *n, int timeout_ms){
    uint8_t buf[DART_DGRAM_MAX]; uint32_t to; size_t out_len; uint64_t now;
    dart_pollfd pfd[2]; int n_fds=1;

    dart_discovery_rt_poll(n->discovery, 0);                 /* discovery tick (non-blocking) */

    memset(pfd,0,sizeof pfd);
    pfd[0].fd=n->fd; pfd[0].events=DART_POLLIN;
    if (n->multicast_fd!=DART_SOCK_BAD){ pfd[1].fd=n->multicast_fd; pfd[1].events=DART_POLLIN; n_fds=2; }
    /* cap the wait at the next internal timer so a due ack/NACK/heartbeat fires on
       time, not after the full quantum (no traffic to wake us when a writer stalls) */
    { uint64_t next_deadline = dart_next_deadline_us(n->transport);
      if (next_deadline){ uint64_t t0 = dart_plat_now_us();
               uint64_t us = (next_deadline > t0) ? next_deadline - t0 : 0;          /* until the timer */
               int ms = (us >= (uint64_t)timeout_ms*1000u) ? timeout_ms
                                                           : (int)((us + 999u)/1000u);
               if (ms < timeout_ms) timeout_ms = ms; } }       /* round up: no busy-spin */
    if (dart_plat_poll(pfd,n_fds,timeout_ms) > 0){
        uint64_t rx_deadline = dart_plat_now_us() + DART_RX_BUDGET_US;
        if (pfd[0].revents & DART_POLLIN) dart__node_rx_drain(n, n->fd, rx_deadline);
        if (n_fds==2 && (pfd[1].revents & DART_POLLIN)) dart__node_rx_drain(n, n->multicast_fd, rx_deadline);
    }

    now=dart_plat_now_us();
    /* the core already consumed any held datagram, so retry it before pulling new */
    if (n->tx_hold_len && dart__node_tx(n, n->tx_hold_peer, n->tx_hold, n->tx_hold_len))
        n->tx_hold_len = 0;
    if (!n->tx_hold_len)
        while (dart_poll_send(n->transport,&to,buf,sizeof buf,&out_len,now)){
            if (!dart__node_tx(n, to, buf, out_len)){
                memcpy(n->tx_hold, buf, out_len);
                n->tx_hold_len = out_len; n->tx_hold_peer = to;
                break;          /* TX buffer full: yield this tick */
            }
            now=dart_plat_now_us();
        }
    return 0;
}

int dart_node_send(dart_node *n, uint16_t channel, const void *data, size_t len){
    /* bounded backpressure: pump the loop (on_message/on_event may fire here) until
       a slow reader acks or qos.backpressure_wait_us elapses, then send anyway */
    const dart_qos *q = dart_channel_qos(n->transport, channel);
    if (q && q->backpressure_wait_us && dart_send_would_evict(n->transport, channel)){
        uint64_t t0 = dart_plat_now_us(), deadline = t0 + q->backpressure_wait_us;
        /* in-pump diagnostic: the publisher is blocked here for the whole wait, so its
           normal per-message print sees nothing within it. When a probe is set, sample
           the writer's repair progress on a ~interval timer so the stall is visible as a
           within-block time series (resends bursty-then-flat vs steady; writer idle for
           lack of NACKs). Observational; when no probe is set this whole block is skipped. */
        uint64_t sample_last = t0; uint32_t sample_polls = 0, sample_idle = 0;
        uint64_t interval = n->pump_probe_interval_us ? n->pump_probe_interval_us : 200000u;
        dart_repair_stats_t sample_prev;
        if (n->pump_probe) dart_repair_stats(n->transport, channel, &sample_prev);
        do {
            dart_node_poll(n, 1);
            if (n->pump_probe){
                uint64_t now = dart_plat_now_us();
                sample_polls++;
                if (dart_repair_pending(n->transport, channel) == 0) sample_idle++;
                if (now - sample_last >= interval){
                    dart_repair_stats_t sample_now; dart_pump_sample sample;
                    dart_repair_stats(n->transport, channel, &sample_now);
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
            if (!dart_send_would_evict(n->transport, channel)) break;
        } while (dart_plat_now_us() < deadline);
        n->backpressure_total_us += dart_plat_now_us() - t0;
        n->backpressure_wait_count++;
    }
#ifdef DART_SHM
    if (n->shm_capable && len>0 && channel < n->shm_n_channels && dart_writer_shm_eligible(n->transport, channel)){
        uint16_t keep_last = (q && q->keep_last) ? q->keep_last : 1u;
        /* a hint (shm_max_bytes / max_message_bytes) pins the channel to one class, so
           same-sized traffic reuses a single prefix-sized segment; without it each message
           uses its own size class's segment, created on demand. Per channel either way. */
        uint32_t hint = q ? (q->shm_max_bytes ? q->shm_max_bytes : q->max_message_bytes) : 0u;
        uint32_t k = dart_shm_class_for(hint ? hint : (uint32_t)len);
        /* fits its class (with a hint, the hinted class) -> publish via SHM; chunk index =
           the history slot this send will occupy, so chunk i binds slot i (no free list) */
        if (k < DART_SHM_N_CLASSES && (uint32_t)len <= dart_shm_class_bytes(k)){
            dart_shm_pool *pool = dart__shm_chan_pool(n, channel, k, keep_last);
            uint16_t slot = dart_channel_hist_head(n->transport, channel);
            void *chunk_ptr = pool ? dart_shm_chunk(pool, slot, NULL) : NULL;
            if (chunk_ptr){
                dart_shm_desc d; uint8_t desc[DART_SHM_DESC_WIRE];
                memcpy(chunk_ptr, data, len);                       /* one-copy write into shm */
                dart_shm_stamp(pool, slot, (uint32_t)len, &d);
                dart_shm_desc_encode(&d, desc);
                if (dart_send_shm(n->transport, channel, chunk_ptr, len, desc, dart_plat_now_us())==0){
                    n->shm_tx++;
                    return 0;
                }
            }
        }
    }
#endif
    return dart_send(n->transport, channel, data, len, dart_plat_now_us());
}

int dart_node_set_role(dart_node *n, uint16_t channel, uint8_t role){
    int r = dart_set_role(n->transport, channel, role);
    if (r == 0){   /* re-advertise our interest: peers rematch as the new blob arrives */
        uint16_t mlen = dart_node_core_build_meta(n->core);
        dart_discovery_rt_set_meta(n->discovery, dart_node_core_meta(n->core, NULL), mlen);
    }
    return r;
}

void dart_node_backpressure_stats(dart_node *n, uint64_t *waited_us, uint32_t *waited_sends){
    if (waited_us)    *waited_us    = n->backpressure_total_us;
    if (waited_sends) *waited_sends = n->backpressure_wait_count;
}

void dart_node_repair_stats(dart_node *n, uint16_t channel, dart_repair_stats_t *out){
    dart_repair_stats(n->transport, channel, out);
}

void dart_node_set_pump_probe(dart_node *n, dart_pump_probe_fn fn, uint64_t interval_us, void *user){
    n->pump_probe = fn; n->pump_probe_interval_us = interval_us; n->pump_probe_user = user;
}

int dart_node_reader_progress(dart_node *n, uint16_t channel, uint32_t peer,
                              uint64_t *base_seqno, uint32_t *have, uint32_t *total){
    return dart_reader_progress(n->transport, channel, peer, base_seqno, have, total);
}

#ifdef DART_SHM
void dart_node_shm_stats(dart_node *n, uint32_t *sent, uint32_t *recv){
    if (sent) *sent = n->shm_tx;
    if (recv) *recv = n->shm_rx;
}
#endif

int dart_node_drain(dart_node *n, uint16_t channel, int timeout_ms){
    uint64_t deadline = dart_plat_now_us() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000u;
    while (!dart_send_drained(n->transport, channel)){
        if (dart_plat_now_us() >= deadline) return 0;
        dart_node_poll(n, 1);
    }
    return 1;
}

int dart_node_writer_match_count(dart_node *n, uint16_t channel){
    return dart_writer_match_count(n->transport, channel);
}

void dart_node_close(dart_node *n, int send_bye){
    if (!n) return;
    if (n->discovery) dart_discovery_rt_close(n->discovery, send_bye);
    if (n->multicast_fd != DART_SOCK_BAD) dart_plat_close(n->multicast_fd);
    if (n->fd != DART_SOCK_BAD) dart_plat_close(n->fd);
    if (n->transport) dart_destroy(n->transport);     /* free hook-allocated dynamic buffers */
#ifdef DART_SHM
    if (n->shm_capable){
        size_t state_bytes = dart_shm_state_bytes(); uint32_t i, n_segments = (uint32_t)n->shm_n_channels * DART_SHM_N_CLASSES;
        for (i=0;i<n_segments;i++)
            if (n->shm_pool[i]) dart_shm_detach((dart_shm_pool*)n->shm_pool[i]);   /* unlinks ours */
        for (i=0;i<n->shm_reader_max;i++)
            if (n->shm_reader_segments[i]) dart_shm_detach((dart_shm_pool*)(n->shm_reader_pool_mem + (size_t)i*state_bytes));
        if (n->shm_scratch) n->shm_alloc(n->user_data, n->shm_scratch, 0);
    }
#endif
    dart_plat_cleanup();
}
