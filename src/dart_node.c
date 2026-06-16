/* NODE runtime: owns the data socket, drives discovery, wires peers into the
 * transport. See dart_node.h. */

/* feature-test macros must precede the first system header in the TU */
#if !defined(_WIN32)
  #ifndef _POSIX_C_SOURCE
  #define _POSIX_C_SOURCE 200809L
  #endif
  #ifndef _DEFAULT_SOURCE
  #define _DEFAULT_SOURCE 1
  #endif
#endif

#include "dart_node.h"
#include "dart_discovery_rt.h"
#include <string.h>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #ifdef _MSC_VER
    #pragma comment(lib, "ws2_32.lib")
  #endif
  typedef SOCKET    dart_sock_t;
  typedef WSAPOLLFD dart_pollfd_t;
  #define DART_BADSOCK   INVALID_SOCKET
  #define DART_CLOSESOCK closesocket
  #define DART_POLL      WSAPoll
  #ifndef SIO_UDP_CONNRESET
  #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
  #endif
  static void dart__net_startup(void){ WSADATA w; WSAStartup(MAKEWORD(2,2), &w); }
  static void dart__net_cleanup(void){ WSACleanup(); }
  static void dart__set_nonblock(dart_sock_t fd){ u_long nb=1; ioctlsocket(fd, FIONBIO, &nb); }
  static int  dart__would_block(void){ return WSAGetLastError()==WSAEWOULDBLOCK; }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <poll.h>
  #include <time.h>
  #include <fcntl.h>
  #include <errno.h>
  typedef int           dart_sock_t;
  typedef struct pollfd dart_pollfd_t;
  #define DART_BADSOCK   (-1)
  #define DART_CLOSESOCK close
  #define DART_POLL      poll
  static void dart__net_startup(void){}
  static void dart__net_cleanup(void){}
  static void dart__set_nonblock(dart_sock_t fd){ int fl=fcntl(fd,F_GETFL,0); if(fl!=-1) fcntl(fd,F_SETFL,fl|O_NONBLOCK); }
  static int  dart__would_block(void){ return errno==EAGAIN || errno==EWOULDBLOCK; }
#endif

static uint64_t dart_now_us(void){
#ifdef _WIN32
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart * 1000000ull) / (uint64_t)f.QuadPart);
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
#endif
}

typedef struct {
    uint8_t  used;
    uint32_t id;
    uint8_t  ip[16];
    uint8_t  ip_len;
    uint16_t port;     /* peer's advertised data port */
} dart__nodepeer;

struct dart_node {
    dart_state     *tr;
    dart_discovery_rt     *disc;
    dart_sock_t     fd;       /* unicast data socket (also group TX) */
    dart_sock_t     mcfd;     /* multicast data RX socket (DART_BADSOCK if unused) */
    dart__nodepeer *peers;
    uint16_t      max_peers;
    uint16_t      domain;
    uint16_t      mc_port;
    /* datagram the socket refused; retried first next poll so it is never lost */
    uint8_t       txhold[DART_DGRAM_MAX];
    size_t        txhold_len;
    uint32_t      txhold_peer;
    /* backpressure accumulators, read via dart_node_backpressure_stats */
    uint64_t      backpressure_accum_us;
    uint32_t      backpressure_accum_n;
    /* one app callback for everything but message delivery; node fills PEER_UP/DOWN */
    dart_event_fn  on_event;
    void          *user_data;
    /* opaque payload appended to every discovery announce; must outlive the node.
       Currently advertises this node's UDP fragment size (see dart__meta_*). */
    uint8_t        disc_meta[8];
    uint8_t        disc_meta_len;
};

/* Discovery-announce metadata. A tiny versioned blob; today it carries only this
 * node's fragment size, but it is laid out to grow (e.g. a future SHM segment id).
 * Layout: ['D','N', ver=1, frag_lo, frag_hi]. */
static uint8_t dart__meta_encode(uint8_t out[8], uint16_t frag){
    out[0]='D'; out[1]='N'; out[2]=1;
    out[3]=(uint8_t)(frag & 0xFF); out[4]=(uint8_t)(frag >> 8);
    return 5;
}
/* Read a peer's advertised fragment size; 0 if absent/unrecognized (caller defaults). */
static uint16_t dart__meta_frag(const uint8_t *meta, uint8_t mlen){
    if (!meta || mlen < 5 || meta[0]!='D' || meta[1]!='N' || meta[2]!=1) return 0;
    return (uint16_t)(meta[3] | ((uint16_t)meta[4] << 8));
}

/* split node config into discovery + transport sub-configs */
static void dart__node_cfgs(const dart_node_config *cfg, dart_discovery_rt_config *dc,
                          dart_config *tc, uint16_t *mp_out){
    uint16_t mp = cfg->discovery.max_peers ? cfg->discovery.max_peers : 16;
    memset(dc, 0, sizeof *dc); memset(tc, 0, sizeof *tc);
    dc->disc.domain_id   = cfg->domain;
    dc->disc.data_port   = cfg->net.data_port;
    dc->disc.announce_us = cfg->discovery.announce_interval_us; /* 0 uses default */
    dc->disc.timeout_us  = cfg->discovery.peer_timeout_us;
    dc->disc.max_peers   = mp;
    dc->group            = cfg->net.discovery_group;
    dc->disc_port        = cfg->net.discovery_port;
    dc->ttl              = cfg->net.multicast_ttl;
    dc->mcast_if         = cfg->net.multicast_interface;
    dc->seeds            = cfg->net.seed_peers;
    dc->n_seeds          = cfg->net.n_seed_peers;
    tc->channels   = cfg->channels;
    tc->n_channels = cfg->n_channels;
    tc->max_peers  = mp;
    tc->frag_payload = cfg->net.fragment_size;   /* 0 = default; dart_init clamps to [MIN,MAX] */
    tc->on_message = cfg->on_message;
    tc->on_event   = cfg->on_event;
    tc->allocator  = cfg->allocator;
    tc->user       = cfg->user_data;
    if (mp_out) *mp_out = mp;
}

/* local iff a route probe to the address selects that same address as source */
static int dart__node_is_local_ip(const uint8_t ip[4]){
    dart_sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a; int local = 0;
    if (s==DART_BADSOCK) return 0;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; memcpy(&a.sin_addr.s_addr, ip, 4); a.sin_port = htons(7);
    if (connect(s, (struct sockaddr*)&a, sizeof a)==0){
        struct sockaddr_in loc;
#ifdef _WIN32
        int ll = (int)sizeof loc;
#else
        socklen_t ll = sizeof loc;
#endif
        if (getsockname(s, (struct sockaddr*)&loc, &ll)==0)
            local = (memcmp(&loc.sin_addr.s_addr, ip, 4)==0);
    }
    DART_CLOSESOCK(s);
    return local;
}

static void dart__node_up(void *u, uint32_t id, const dart_discovery_addr *addr,
                        const uint8_t *meta, uint8_t mlen){
    dart_node *n = (dart_node*)u; uint16_t i; int slot = -1;
    for (i=0;i<n->max_peers;i++){
        if (n->peers[i].used && n->peers[i].id==id){      /* address update */
            memcpy(n->peers[i].ip, addr->ip, 16);
            n->peers[i].ip_len = addr->ip_len; n->peers[i].port = addr->port;
            return;
        }
        if (!n->peers[i].used && slot<0) slot=(int)i;
    }
    if (slot<0) return;
    n->peers[slot].used=1; n->peers[slot].id=id;
    memcpy(n->peers[slot].ip, addr->ip, 16);
    n->peers[slot].ip_len=addr->ip_len; n->peers[slot].port=addr->port;
    /* interest rides the meta channel; the announce meta carries the peer's frag size */
    dart_peer_add(n->tr, id, (addr->ip_len==4) && dart__node_is_local_ip(addr->ip),
                  dart__meta_frag(meta, mlen));
    if (n->on_event){
        dart_event ev; memset(&ev, 0, sizeof ev);
        ev.kind=DART_PEER_UP; ev.peer=id; ev.detail="peer discovered";
        memcpy(ev.ip, addr->ip, 16); ev.ip_len=addr->ip_len; ev.port=addr->port;
        n->on_event(n->user_data, &ev);
    }
}
static void dart__node_down(void *u, uint32_t id){
    dart_node *n=(dart_node*)u; uint16_t i; int found=0;
    for (i=0;i<n->max_peers;i++) if (n->peers[i].used && n->peers[i].id==id){ n->peers[i].used=0; found=1; break; }
    dart_peer_remove(n->tr, id);
    if (found && n->on_event){
        dart_event ev; memset(&ev, 0, sizeof ev);
        ev.kind=DART_PEER_DOWN; ev.peer=id; ev.detail="peer lost";
        n->on_event(n->user_data, &ev);
    }
}

static int dart__node_find_addr(dart_node *n, const struct sockaddr_in *s){
    uint16_t i, port=ntohs(s->sin_port);
    for (i=0;i<n->max_peers;i++)
        if (n->peers[i].used && n->peers[i].ip_len>=4 && n->peers[i].port==port
            && memcmp(n->peers[i].ip, &s->sin_addr.s_addr, 4)==0) return (int)i;
    return -1;
}
static int dart__node_find_id(dart_node *n, uint32_t id){
    uint16_t i;
    for (i=0;i<n->max_peers;i++) if (n->peers[i].used && n->peers[i].id==id) return (int)i;
    return -1;
}

/* data multicast group from a selector (topic identity & 0xFF); &0xFF wrap is
 * harmless, RX filters by peer table + identity */
static uint32_t dart__node_group_addr(uint16_t domain, uint16_t sel){
    uint32_t a = (239u<<24)|(255u<<16)|((uint32_t)(domain&0xFFu)<<8)|(uint32_t)(sel&0xFFu);
    return htonl(a);
}
/* a channel's group, from its identity so it matches DART_DEST_GROUP and every peer */
static uint32_t dart__node_chan_group(uint16_t domain, const dart_channel_def *def){
    return dart__node_group_addr(domain, (uint16_t)(dart_channel_identity(def) & 0xFFu));
}

/* send one datagram; returns 1 when done with it, 0 only on a would-block TX-full */
static int dart__node_tx(dart_node *n, uint32_t to, const uint8_t *buf, size_t len){
    struct sockaddr_in d;
    memset(&d, 0, sizeof d);
    d.sin_family = AF_INET;
    if (DART_DEST_IS_GROUP(to)){
        d.sin_addr.s_addr = dart__node_group_addr(n->domain, DART_DEST_GROUP_CHAN(to));
        d.sin_port = htons(n->mc_port);
    } else {
        int pi = dart__node_find_id(n, to);
        if (pi < 0) return 1;              /* peer vanished */
        memcpy(&d.sin_addr.s_addr, n->peers[pi].ip, 4);
        d.sin_port = htons(n->peers[pi].port);
    }
    if (sendto(n->fd, (const char*)buf, (int)len, 0, (struct sockaddr*)&d, sizeof d) < 0
        && dart__would_block())
        return 0;
    return 1;
}

size_t dart_node_required_memory(const dart_node_config *cfg){
    dart_discovery_rt_config dc; dart_config tc; uint16_t mp;
    size_t node_sz, tbl_sz, disc_sz, tr_sz;
    if (!cfg || cfg->n_channels==0) return 0;
    dart__node_cfgs(cfg,&dc,&tc,&mp);
    node_sz = (sizeof(struct dart_node)+15u)&~(size_t)15u;
    tbl_sz  = ((size_t)mp*sizeof(dart__nodepeer)+15u)&~(size_t)15u;
    disc_sz = (dart_discovery_rt_required_memory(&dc)+15u)&~(size_t)15u;
    tr_sz   = (dart_required_memory(&tc)+15u)&~(size_t)15u;
    return 32u + node_sz + tbl_sz + disc_sz + tr_sz;
}

dart_node *dart_node_open(void *mem, size_t cap, const dart_node_config *cfg){
    dart_discovery_rt_config dc; dart_config tc; uint16_t mp;
    uint8_t *base, *p; size_t node_sz, tbl_sz, disc_sz, tr_sz;
    dart_node *n; dart_sock_t fd; struct sockaddr_in a;
#ifdef _WIN32
    int alen;
#else
    socklen_t alen;
#endif
    if (!mem || !cfg || cfg->n_channels==0) return NULL;
    if (cap < dart_node_required_memory(cfg)) return NULL;
    dart__node_cfgs(cfg,&dc,&tc,&mp);

    dart__net_startup();

    base = (uint8_t*)(((uintptr_t)mem+15u)&~(uintptr_t)15u);
    node_sz = (sizeof(struct dart_node)+15u)&~(size_t)15u;
    tbl_sz  = ((size_t)mp*sizeof(dart__nodepeer)+15u)&~(size_t)15u;
    disc_sz = (dart_discovery_rt_required_memory(&dc)+15u)&~(size_t)15u;
    tr_sz   = (dart_required_memory(&tc)+15u)&~(size_t)15u;

    n=(dart_node*)base; memset(n,0,sizeof *n);
    n->fd = DART_BADSOCK; n->mcfd = DART_BADSOCK; n->max_peers=mp;
    n->domain = cfg->domain;
    n->on_event = cfg->on_event; n->user_data = cfg->user_data;
    n->mc_port = cfg->net.multicast_port ? cfg->net.multicast_port
               : (uint16_t)((cfg->net.discovery_port ? cfg->net.discovery_port : 7400) + 1);
    p = base + node_sz;
    n->peers=(dart__nodepeer*)p; memset(n->peers,0,(size_t)mp*sizeof(dart__nodepeer));
    p += tbl_sz;

    n->tr = dart_init(p, tr_sz, &tc);
    if (!n->tr){ dart__net_cleanup(); return NULL; }
    p += tr_sz;

    /* Bind the data socket before opening discovery so we can advertise its real
       port (0 => OS ephemeral, read back via getsockname). No SO_REUSEADDR: a
       unicast endpoint owns its port, so a collision fails loudly here. */
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd==DART_BADSOCK){ dart__net_cleanup(); return NULL; }
    memset(&a,0,sizeof a);
    a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_ANY);
    a.sin_port=htons(cfg->net.data_port);      /* 0 => ephemeral */
    if (bind(fd,(struct sockaddr*)&a,sizeof a)!=0){
        DART_CLOSESOCK(fd); dart__net_cleanup(); return NULL;
    }
    memset(&a,0,sizeof a); alen=(int)sizeof a;
    if (getsockname(fd,(struct sockaddr*)&a,&alen)!=0){
        DART_CLOSESOCK(fd); dart__net_cleanup(); return NULL;
    }
    dart__set_nonblock(fd);   /* never block in recv/send; poll drains the queue */
#ifdef _WIN32
    /* suppress WSAECONNRESET from a bounced send leaking into the shared RX path */
    { BOOL off = FALSE; DWORD bv = 0;
      WSAIoctl(fd, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &bv, NULL, NULL); }
#endif
    if (cfg->net.recv_buffer_bytes){ int v=(int)cfg->net.recv_buffer_bytes; setsockopt(fd,SOL_SOCKET,SO_RCVBUF,(const char*)&v,sizeof v); }
    if (cfg->net.send_buffer_bytes){ int v=(int)cfg->net.send_buffer_bytes; setsockopt(fd,SOL_SOCKET,SO_SNDBUF,(const char*)&v,sizeof v); }
    n->fd=fd;
    dc.disc.data_port = ntohs(a.sin_port);     /* advertise the actual port */

    /* multicast data: group TX rides the unicast socket; group RX needs its own
       socket on the shared mc_port with one IGMP join per subscribed channel */
    { int want_rx=0, want_tx=0; uint16_t i; uint32_t ifip;
      for (i=0;i<cfg->n_channels;i++) if (cfg->channels[i].multicast){
          if (cfg->channels[i].role!=DART_PUB_ONLY) want_rx=1;
          if (cfg->channels[i].role!=DART_SUB_ONLY) want_tx=1;
      }
      /* pin data multicast to discovery's egress interface, else multihomed hosts
         pick per-group interfaces and break peer identification by source addr */
      ifip = !(want_rx||want_tx) ? htonl(INADDR_ANY)
           : cfg->net.multicast_interface ? inet_addr(cfg->net.multicast_interface)
           : dart_discovery_mcast_if_for(inet_addr(cfg->net.discovery_group?cfg->net.discovery_group:"239.255.0.7"),
                                cfg->net.discovery_port?cfg->net.discovery_port:7400);
      if (want_tx){
          unsigned char mttl = cfg->net.multicast_ttl ? cfg->net.multicast_ttl : 1, mloop = 1;
          setsockopt(n->fd, IPPROTO_IP, IP_MULTICAST_IF,   (const char*)&ifip,  sizeof ifip);
          setsockopt(n->fd, IPPROTO_IP, IP_MULTICAST_TTL,  (const char*)&mttl,  sizeof mttl);
          setsockopt(n->fd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&mloop, sizeof mloop);
      }
      if (want_rx){
          dart_sock_t mfd = socket(AF_INET, SOCK_DGRAM, 0);
          int on=1, mc_ok=(mfd!=DART_BADSOCK);
          unsigned char mloop = 1;
          struct sockaddr_in ma;
          if (mc_ok){
              setsockopt(mfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);
#ifdef SO_REUSEPORT
              setsockopt(mfd, SOL_SOCKET, SO_REUSEPORT, (const char*)&on, sizeof on);
#endif
              memset(&ma,0,sizeof ma);
              ma.sin_family=AF_INET; ma.sin_addr.s_addr=htonl(INADDR_ANY);
              ma.sin_port=htons(n->mc_port);
              if (bind(mfd,(struct sockaddr*)&ma,sizeof ma)!=0) mc_ok=0;
          }
          if (mc_ok){
              /* one join per distinct group (kernels reject dups); memberships are
                 OS-capped (~20: Linux net.ipv4.igmp_max_memberships), a fail fails open */
              for (i=0;i<cfg->n_channels;i++)
                  if (cfg->channels[i].multicast && cfg->channels[i].role!=DART_PUB_ONLY){
                      uint32_t g = dart__node_chan_group(cfg->domain, &cfg->channels[i]);
                      uint16_t j; int dup=0;
                      for (j=0;j<i;j++)
                          if (cfg->channels[j].multicast && cfg->channels[j].role!=DART_PUB_ONLY
                              && dart__node_chan_group(cfg->domain, &cfg->channels[j])==g){
                              dup=1; break;
                          }
                      if (dup) continue;
                      { struct ip_mreq mr; memset(&mr,0,sizeof mr);
                        mr.imr_multiaddr.s_addr=g;
                        mr.imr_interface.s_addr=ifip;
                        if (setsockopt(mfd,IPPROTO_IP,IP_ADD_MEMBERSHIP,(const char*)&mr,sizeof mr)!=0){
                            mc_ok=0; break;
                        } }
                  }
          }
          if (!mc_ok){
              if (mfd!=DART_BADSOCK) DART_CLOSESOCK(mfd);
              DART_CLOSESOCK(fd); n->fd=DART_BADSOCK; dart__net_cleanup(); return NULL;
          }
          setsockopt(mfd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&mloop, sizeof mloop);
          dart__set_nonblock(mfd);
          if (cfg->net.recv_buffer_bytes){ int v=(int)cfg->net.recv_buffer_bytes; setsockopt(mfd,SOL_SOCKET,SO_RCVBUF,(const char*)&v,sizeof v); }
          n->mcfd=mfd;
      } }

    dc.disc.on_peer_up   = dart__node_up;
    dc.disc.on_peer_down = dart__node_down;
    dc.disc.user         = n;
    /* advertise our fragment size (clamped exactly as dart_init clamps it) so peers
       reassemble our messages at the right size. The buffer lives in the node. */
    { uint16_t f = cfg->net.fragment_size ? cfg->net.fragment_size : DART_FRAG_PAYLOAD;
      if (f < DART_FRAG_PAYLOAD_MIN) f = DART_FRAG_PAYLOAD_MIN;
      if (f > DART_FRAG_PAYLOAD_MAX) f = DART_FRAG_PAYLOAD_MAX;
      n->disc_meta_len = dart__meta_encode(n->disc_meta, f);
      dc.disc.meta = n->disc_meta; dc.disc.meta_len = n->disc_meta_len; }
    n->disc = dart_discovery_rt_open(p, disc_sz, &dc);
    if (!n->disc){
        if (n->mcfd!=DART_BADSOCK){ DART_CLOSESOCK(n->mcfd); n->mcfd=DART_BADSOCK; }
        DART_CLOSESOCK(fd); n->fd=DART_BADSOCK; dart__net_cleanup(); return NULL;
    }
    p += disc_sz;

    return n;
}

/* max wall-time draining RX (and running on_message) per poll tick before
 * yielding to discovery, so a slow on_message never starves it */
#ifndef DART_RX_BUDGET_US
#define DART_RX_BUDGET_US 5000u
#endif

/* drain one socket's RX queue into the transport until empty or past deadline
 * (full drain avoids NACK storms). Distinct from public dart_node_drain. */
static void dart__node_rx_drain(dart_node *n, dart_sock_t fd, uint64_t deadline){
    uint8_t buf[DART_DGRAM_MAX];
    for (;;){
        struct sockaddr_in src;
#ifdef _WIN32
        int sl=(int)sizeof src;
#else
        socklen_t sl=sizeof src;
#endif
        int r=(int)recvfrom(fd,(char*)buf,(int)sizeof buf,0,(struct sockaddr*)&src,&sl);
        if (r<0){
            if (dart__would_block()) break;        /* queue empty */
            continue;   /* per-datagram error (e.g. bounced send); keep draining */
        }
        if (r>0){
            if (r>=4 && buf[0]=='u' && buf[1]=='D' && buf[2]=='S' && buf[3]=='C'){
                /* unicast announce aimed at our data port: hand it to discovery */
                uint8_t sip[4]; memcpy(sip, &src.sin_addr.s_addr, 4);
                dart_discovery_rt_feed(n->disc, sip, 4, buf, (size_t)r);
            } else {
                int pi=dart__node_find_addr(n,&src);
                if (pi>=0) dart_on_datagram(n->tr, n->peers[pi].id, buf, (size_t)r, dart_now_us());
            }
        }
        if (dart_now_us() >= deadline) break;      /* yield to discovery/send */
    }
}

int dart_node_poll(dart_node *n, int timeout_ms){
    uint8_t buf[DART_DGRAM_MAX]; uint32_t to; size_t ol; uint64_t now;
    dart_pollfd_t pfd[2]; int nf=1;

    dart_discovery_rt_poll(n->disc, 0);                 /* discovery tick (non-blocking) */

    memset(pfd,0,sizeof pfd);
    pfd[0].fd=n->fd; pfd[0].events=POLLIN;
    if (n->mcfd!=DART_BADSOCK){ pfd[1].fd=n->mcfd; pfd[1].events=POLLIN; nf=2; }
    if (DART_POLL(pfd,nf,timeout_ms) > 0){
        uint64_t rx_deadline = dart_now_us() + DART_RX_BUDGET_US;
        if (pfd[0].revents & POLLIN) dart__node_rx_drain(n, n->fd, rx_deadline);
        if (nf==2 && (pfd[1].revents & POLLIN)) dart__node_rx_drain(n, n->mcfd, rx_deadline);
    }

    now=dart_now_us();
    /* the core already consumed any held datagram, so retry it before pulling new */
    if (n->txhold_len && dart__node_tx(n, n->txhold_peer, n->txhold, n->txhold_len))
        n->txhold_len = 0;
    if (!n->txhold_len)
        while (dart_poll_send(n->tr,&to,buf,sizeof buf,&ol,now)){
            if (!dart__node_tx(n, to, buf, ol)){
                memcpy(n->txhold, buf, ol);
                n->txhold_len = ol; n->txhold_peer = to;
                break;          /* TX buffer full: yield this tick */
            }
            now=dart_now_us();
        }
    return 0;
}

int dart_node_send(dart_node *n, uint16_t channel, const void *data, size_t len){
    /* bounded backpressure: pump the loop (on_message/on_event may fire here) until
       a slow reader acks or qos.backpressure_wait_us elapses, then send anyway */
    const dart_qos *q = dart_channel_qos(n->tr, channel);
    if (q && q->backpressure_wait_us && dart_send_would_evict(n->tr, channel)){
        uint64_t t0 = dart_now_us(), deadline = t0 + q->backpressure_wait_us;
        do {
            dart_node_poll(n, 1);
            if (!dart_send_would_evict(n->tr, channel)) break;
        } while (dart_now_us() < deadline);
        n->backpressure_accum_us += dart_now_us() - t0;
        n->backpressure_accum_n++;
    }
    return dart_send(n->tr, channel, data, len, dart_now_us());
}

int dart_node_set_role(dart_node *n, uint16_t channel, uint8_t role){
    return dart_set_role(n->tr, channel, role);
}

void dart_node_backpressure_stats(dart_node *n, uint64_t *waited_us, uint32_t *waited_sends){
    if (waited_us)    *waited_us    = n->backpressure_accum_us;
    if (waited_sends) *waited_sends = n->backpressure_accum_n;
}

int dart_node_drain(dart_node *n, uint16_t channel, int timeout_ms){
    uint64_t deadline = dart_now_us() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000u;
    while (!dart_send_drained(n->tr, channel)){
        if (dart_now_us() >= deadline) return 0;
        dart_node_poll(n, 1);
    }
    return 1;
}

int dart_node_writer_match_count(dart_node *n, uint16_t channel){
    return dart_writer_match_count(n->tr, channel);
}

void dart_node_close(dart_node *n, int send_bye){
    if (!n) return;
    if (n->disc) dart_discovery_rt_close(n->disc, send_bye);
    if (n->mcfd != DART_BADSOCK) DART_CLOSESOCK(n->mcfd);
    if (n->fd != DART_BADSOCK) DART_CLOSESOCK(n->fd);
    if (n->tr) dart_destroy(n->tr);     /* free hook-allocated dynamic buffers */
    dart__net_cleanup();
}
