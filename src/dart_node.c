/* NODE runtime: owns the data socket, drives discovery, wires
 * peers into the transport. See dart_node.h. */

/* Feature-test macros must precede the first system header in the TU. */
#if !defined(_WIN32)
  #ifndef _POSIX_C_SOURCE
  #define _POSIX_C_SOURCE 200809L
  #endif
  #ifndef _DEFAULT_SOURCE
  #define _DEFAULT_SOURCE 1
  #endif
#endif

#include "dart_node.h"   /* node API (brings dart_transport.h) */
#include "dart_discovery_rt.h"    /* discovery runtime used by the node impl */
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

/* node-local little-endian 16-bit helpers (the core's are static to dart_transport.c). */
static void     dart_node_w16(uint8_t *p, uint16_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static uint16_t dart_node_r16(const uint8_t *p){ return (uint16_t)(p[0] | ((uint16_t)p[1]<<8)); }

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
    uint16_t port;     /* peer's advertised transport (data) port */
} dart__nodepeer;

struct dart_node {
    dart_state     *tr;
    dart_discovery_rt     *disc;
    dart_sock_t     fd;       /* unicast transport data socket (also group TX) */
    dart_sock_t     mcfd;     /* multicast data RX socket (DART_BADSOCK if unused) */
    dart__nodepeer *peers;
    uint16_t      max_peers;
    uint16_t      domain;
    uint16_t      mc_port;
    /* pub/sub channel lists announced through discovery */
    uint8_t       meta[DART_DISCOVERY_META_MAX];
    uint8_t       meta_len;
    /* datagram consumed from the core but refused by the socket; retried first
     * next poll so it is never lost */
    uint8_t       txhold[DART_DGRAM_MAX];
    size_t        txhold_len;
    uint32_t      txhold_peer;
    /* cumulative backpressure (max_block_us waits in dart_node_send) */
    uint64_t      block_us;
    uint32_t      block_n;
};

/* split node config into discovery + transport sub-configs (callbacks/user
 * installed later by open) */
static void dart__node_cfgs(const dart_node_config *cfg, dart_discovery_rt_config *dc,
                          dart_config *tc, uint16_t *mp_out){
    uint16_t mp = cfg->max_peers ? cfg->max_peers : 16;
    memset(dc, 0, sizeof *dc); memset(tc, 0, sizeof *tc);
    dc->disc.domain_id   = cfg->domain_id;
    dc->disc.data_port   = cfg->data_port;     /* advertised to peers */
    dc->disc.announce_us = cfg->announce_us;   /* 0 uses dart_discovery default */
    dc->disc.timeout_us  = cfg->timeout_us;
    dc->disc.max_peers   = mp;
    dc->group            = cfg->disc_group;
    dc->disc_port        = cfg->disc_port;
    dc->ttl              = cfg->ttl;
    dc->mcast_if         = cfg->mcast_if;
    tc->channels   = cfg->channels;
    tc->n_channels = cfg->n_channels;
    tc->max_peers  = mp;
    tc->on_sample  = cfg->on_sample;
    tc->on_gap     = cfg->on_gap;
    tc->user       = cfg->user;
    if (mp_out) *mp_out = mp;
}

/* discovery meta payload: [0]=#pub [1]=#sub then little-endian u16 channel
 * ids, pubs first. */
#define DART__META_IDS_MAX ((DART_DISCOVERY_META_MAX-2u)/2u)

static int dart__node_build_meta(const dart_node_config *cfg, uint8_t *out, uint8_t *out_len){
    uint16_t i; uint32_t np=0, ns=0; uint8_t *p=out+2;
    for (i=0;i<cfg->n_channels;i++){
        if (cfg->channels[i].dir!=DART_SUB_ONLY) np++;
        if (cfg->channels[i].dir!=DART_PUB_ONLY) ns++;
    }
    if (np+ns > DART__META_IDS_MAX) return 0;
    out[0]=(uint8_t)np; out[1]=(uint8_t)ns;
    for (i=0;i<cfg->n_channels;i++)
        if (cfg->channels[i].dir!=DART_SUB_ONLY){ dart_node_w16(p,cfg->channels[i].channel_id); p+=2; }
    for (i=0;i<cfg->n_channels;i++)
        if (cfg->channels[i].dir!=DART_PUB_ONLY){ dart_node_w16(p,cfg->channels[i].channel_id); p+=2; }
    *out_len=(uint8_t)(2u+2u*(np+ns));
    return 1;
}

/* A peer address is local iff a route probe to it selects that same address as
 * source: true for 127.0.0.1 and this host's own addresses, never for another machine. */
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
    /* decode the peer's pub/sub channel lists; absent/malformed meta degrades
       to "all channels" */
    { uint16_t pubs[DART__META_IDS_MAX], subs[DART__META_IDS_MAX];
      const uint16_t *pp=NULL, *ss=NULL; uint16_t np=0, ns=0;
      int is_local = (addr->ip_len==4) && dart__node_is_local_ip(addr->ip);
      if (meta && mlen>=2){
          uint8_t cp=meta[0], cs=meta[1];
          if ((uint32_t)cp+cs<=DART__META_IDS_MAX && 2u+2u*((uint32_t)cp+cs)<=mlen){
              const uint8_t *q=meta+2; uint16_t j;
              for (j=0;j<cp;j++){ pubs[j]=dart_node_r16(q); q+=2; }
              for (j=0;j<cs;j++){ subs[j]=dart_node_r16(q); q+=2; }
              pp=pubs; np=cp; ss=subs; ns=cs;
          }
      }
      dart_peer_add(n->tr, id, pp, np, ss, ns, is_local);
    }
}
static void dart__node_down(void *u, uint32_t id){
    dart_node *n=(dart_node*)u; uint16_t i;
    for (i=0;i<n->max_peers;i++) if (n->peers[i].used && n->peers[i].id==id){ n->peers[i].used=0; break; }
    dart_peer_remove(n->tr, id);
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

/* deterministic per-(domain, channel) data multicast group. &0xFF wrap
 * collisions are harmless: the receive path filters by peer table and channel id. */
static uint32_t dart__node_group_addr(uint16_t domain, uint16_t chan){
    uint32_t a = (239u<<24)|(255u<<16)|((uint32_t)(domain&0xFFu)<<8)|(uint32_t)(chan&0xFFu);
    return htonl(a);
}

/* Send one datagram to a peer or multicast group. Returns 1 when the datagram
 * is done with (sent, peer unknown, or hard error), 0 only on a would-block
 * TX-buffer-full condition, where the caller must keep it. */
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
    n->domain = cfg->domain_id;
    n->mc_port = cfg->mc_port ? cfg->mc_port
               : (uint16_t)((cfg->disc_port ? cfg->disc_port : 7400) + 1);
    if (!dart__node_build_meta(cfg, n->meta, &n->meta_len)){
        dart__net_cleanup(); return NULL;    /* too many channels for announce */
    }
    p = base + node_sz;
    n->peers=(dart__nodepeer*)p; memset(n->peers,0,(size_t)mp*sizeof(dart__nodepeer));
    p += tbl_sz;

    n->tr = dart_init(p, tr_sz, &tc);
    if (!n->tr){ dart__net_cleanup(); return NULL; }
    p += tr_sz;

    /* Bind the unicast data socket before opening discovery so we can advertise
       its real port (data_port == 0 gets an OS ephemeral, read back via
       getsockname). No SO_REUSEADDR: a unicast endpoint owns its port
       exclusively, so a port collision fails loudly here. */
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd==DART_BADSOCK){ dart__net_cleanup(); return NULL; }
    memset(&a,0,sizeof a);
    a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_ANY);
    a.sin_port=htons(cfg->data_port);          /* 0 => ephemeral */
    if (bind(fd,(struct sockaddr*)&a,sizeof a)!=0){
        DART_CLOSESOCK(fd); dart__net_cleanup(); return NULL;
    }
    memset(&a,0,sizeof a); alen=(int)sizeof a;
    if (getsockname(fd,(struct sockaddr*)&a,&alen)!=0){
        DART_CLOSESOCK(fd); dart__net_cleanup(); return NULL;
    }
    dart__set_nonblock(fd);   /* never block in recv/send: poll drains the whole
                               RX queue and a full TX buffer never deadlocks */
#ifdef _WIN32
    /* Without this, a bounced send (peer gone, ICMP port unreachable) surfaces
       as WSAECONNRESET on a later recvfrom, injecting one peer's error into the
       shared RX path. Turn the reporting off. */
    { BOOL off = FALSE; DWORD bv = 0;
      WSAIoctl(fd, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &bv, NULL, NULL); }
#endif
    if (cfg->so_rcvbuf){ int v=(int)cfg->so_rcvbuf; setsockopt(fd,SOL_SOCKET,SO_RCVBUF,(const char*)&v,sizeof v); }
    if (cfg->so_sndbuf){ int v=(int)cfg->so_sndbuf; setsockopt(fd,SOL_SOCKET,SO_SNDBUF,(const char*)&v,sizeof v); }
    n->fd=fd;
    dc.disc.data_port = ntohs(a.sin_port);     /* advertise the actual port */

    /* multicast data: group TX goes out the unicast data socket (source port
       still identifies the sender); group RX needs its own socket on the shared
       mc_port, one IGMP join per subscribed channel. */
    { int want_rx=0, want_tx=0; uint16_t i; uint32_t ifip;
      for (i=0;i<cfg->n_channels;i++) if (cfg->channels[i].mcast){
          if (cfg->channels[i].dir!=DART_PUB_ONLY) want_rx=1;
          if (cfg->channels[i].dir!=DART_SUB_ONLY) want_tx=1;
      }
      /* pin data multicast to the same egress interface discovery uses, so the
         source address peers learned from announces matches group data. Otherwise
         multihomed hosts pick per-group interfaces and break peer identification. */
      ifip = !(want_rx||want_tx) ? htonl(INADDR_ANY)
           : cfg->mcast_if       ? inet_addr(cfg->mcast_if)
           : dart_discovery_mcast_if_for(inet_addr(cfg->disc_group?cfg->disc_group:"239.255.0.7"),
                                cfg->disc_port?cfg->disc_port:7400);
      if (want_tx){
          unsigned char mttl = cfg->ttl ? cfg->ttl : 1, mloop = 1;
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
              for (i=0;i<cfg->n_channels;i++)
                  if (cfg->channels[i].mcast && cfg->channels[i].dir!=DART_PUB_ONLY){
                      struct ip_mreq mr; memset(&mr,0,sizeof mr);
                      mr.imr_multiaddr.s_addr=dart__node_group_addr(cfg->domain_id, cfg->channels[i].channel_id);
                      mr.imr_interface.s_addr=ifip;
                      if (setsockopt(mfd,IPPROTO_IP,IP_ADD_MEMBERSHIP,(const char*)&mr,sizeof mr)!=0){
                          mc_ok=0; break;
                      }
                  }
          }
          if (!mc_ok){
              if (mfd!=DART_BADSOCK) DART_CLOSESOCK(mfd);
              DART_CLOSESOCK(fd); n->fd=DART_BADSOCK; dart__net_cleanup(); return NULL;
          }
          setsockopt(mfd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&mloop, sizeof mloop);
          dart__set_nonblock(mfd);
          if (cfg->so_rcvbuf){ int v=(int)cfg->so_rcvbuf; setsockopt(mfd,SOL_SOCKET,SO_RCVBUF,(const char*)&v,sizeof v); }
          n->mcfd=mfd;
      } }

    dc.disc.meta         = n->meta;
    dc.disc.meta_len     = n->meta_len;
    dc.disc.on_peer_up   = dart__node_up;
    dc.disc.on_peer_down = dart__node_down;
    dc.disc.user         = n;
    n->disc = dart_discovery_rt_open(p, disc_sz, &dc);
    if (!n->disc){
        if (n->mcfd!=DART_BADSOCK){ DART_CLOSESOCK(n->mcfd); n->mcfd=DART_BADSOCK; }
        DART_CLOSESOCK(fd); n->fd=DART_BADSOCK; dart__net_cleanup(); return NULL;
    }
    p += disc_sz;

    return n;
}

/* Max wall-time draining the RX queue (and running on_sample) per poll tick
 * before yielding to discovery. Bounds how long a slow on_sample starves the
 * single-threaded loop, keeping discovery alive so peers never time out. */
#ifndef DART_RX_BUDGET_US
#define DART_RX_BUDGET_US 5000u
#endif

/* Drain one socket's receive queue into the transport until empty or the
 * deadline passes. Draining fully (vs one-per-tick) avoids NACK storms.
 * Sockets are non-blocking, so recvfrom <=0 means empty. */
static void dart__node_drain(dart_node *n, dart_sock_t fd, uint64_t deadline){
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
            continue;   /* per-datagram error (e.g. a bounced send surfaces as
                           WSAECONNRESET); datagrams behind it are fine, keep draining */
        }
        if (r>0){
            int pi=dart__node_find_addr(n,&src);
            if (pi>=0) dart_on_datagram(n->tr, n->peers[pi].id, buf, (size_t)r, dart_now_us());
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
        if (pfd[0].revents & POLLIN) dart__node_drain(n, n->fd, rx_deadline);
        if (nf==2 && (pfd[1].revents & POLLIN)) dart__node_drain(n, n->mcfd, rx_deadline);
    }

    now=dart_now_us();
    /* a datagram the socket refused last tick was already consumed from the core,
       so dropping it would lose best-effort data. Retry it before pulling new. */
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

int dart_node_send(dart_node *n, uint16_t channel_id, const void *data, size_t len){
    /* bounded backpressure (qos.max_block_us): let slow-but-live readers ack
       before un-acked history is overwritten. The wait pumps the node loop, so
       on_sample/on_gap may fire from inside this call. Releases on ack, reader
       death, or deadline; then the send proceeds. max_block_us == 0 never waits. */
    const dart_qos *q = dart_channel_qos(n->tr, channel_id);
    if (q && q->max_block_us && dart_send_would_evict(n->tr, channel_id)){
        uint64_t t0 = dart_now_us(), deadline = t0 + q->max_block_us;
        do {
            dart_node_poll(n, 1);
            if (!dart_send_would_evict(n->tr, channel_id)) break;
        } while (dart_now_us() < deadline);
        n->block_us += dart_now_us() - t0;
        n->block_n++;
    }
    return dart_send(n->tr, channel_id, data, len, dart_now_us());
}

void dart_node_block_stats(dart_node *n, uint64_t *block_us, uint32_t *blocked_sends){
    if (block_us)      *block_us      = n->block_us;
    if (blocked_sends) *blocked_sends = n->block_n;
}

void dart_node_close(dart_node *n, int send_bye){
    if (!n) return;
    if (n->disc) dart_discovery_rt_close(n->disc, send_bye);
    if (n->mcfd != DART_BADSOCK) DART_CLOSESOCK(n->mcfd);
    if (n->fd != DART_BADSOCK) DART_CLOSESOCK(n->fd);
    dart__net_cleanup();
}
