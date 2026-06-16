/* peer-discovery runtime: sockets, clock, UUID, and the one-tick loop. */

/* feature-test macros must precede the first system header (POSIX only) */
#if !defined(_WIN32)
  #ifndef _POSIX_C_SOURCE
  #define _POSIX_C_SOURCE 200809L
  #endif
  #ifndef _DEFAULT_SOURCE
  #define _DEFAULT_SOURCE 1
  #endif
#endif

#include "dart_discovery_rt.h"
#include <string.h>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <bcrypt.h>            /* BCryptGenRandom (CSPRNG) */
  #ifdef _MSC_VER
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "bcrypt.lib")
  #endif
  typedef SOCKET    dart_discovery_sock_t;
  typedef WSAPOLLFD dart_discovery_pollfd_t;
  #define DART_DISCOVERY_BADSOCK  INVALID_SOCKET
  #define DART_DISCOVERY_CLOSESOCK closesocket
  #define DART_DISCOVERY_POLL     WSAPoll
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <poll.h>
  #include <time.h>
  #include <stdio.h>
  #include <stdlib.h>           /* arc4random_buf on macOS/BSD */
  #if defined(ESP_PLATFORM)
    #include <esp_random.h>     /* esp_fill_random (HW RNG) */
  #elif defined(__linux__)
    #include <errno.h>
    #include <sys/random.h>     /* getrandom(2) */
  #endif
  typedef int           dart_discovery_sock_t;
  typedef struct pollfd dart_discovery_pollfd_t;
  #define DART_DISCOVERY_BADSOCK  (-1)
  #define DART_DISCOVERY_CLOSESOCK close
  #define DART_DISCOVERY_POLL     poll
#endif

struct dart_discovery_rt {
    dart_discovery_state        *core;
    dart_discovery_sock_t        fd;
    struct sockaddr_in  grp;
    uint16_t            max_peers;
    dart_discovery_addr seeds[DART_DISCOVERY_MAX_SEEDS];
    uint16_t            n_seeds;
};

static void dart_discovery_rt_tx1(dart_discovery_rt *rt, const uint8_t *out, size_t m,
                          const uint8_t ip[4], uint16_t port){
    struct sockaddr_in d;
    memset(&d, 0, sizeof d);
    d.sin_family = AF_INET;
    memcpy(&d.sin_addr.s_addr, ip, 4);
    d.sin_port = htons(port);
    sendto(rt->fd, (const char*)out, (int)m, 0, (struct sockaddr*)&d, sizeof d);
}

/* send to the group, every seed, and every known peer (peers get a copy at the
 * disc port and at their data port, the only per-process address when processes
 * share the disc port). Survives multicast outages; receivers dedup by uuid. */
static void dart_discovery_rt_tx(dart_discovery_rt *rt, const uint8_t *out, size_t m){
    uint16_t s, dport = ntohs(rt->grp.sin_port);
    dart_discovery_addr a;
    sendto(rt->fd, (const char*)out, (int)m, 0,
           (struct sockaddr*)&rt->grp, sizeof rt->grp);
    for (s=0; s<rt->n_seeds; s++){
        const dart_discovery_addr *sd = &rt->seeds[s];
        if (sd->ip_len != 4) continue;
        dart_discovery_rt_tx1(rt, out, m, sd->ip, sd->port ? sd->port : dport);
    }
    for (s=0; s<rt->max_peers; s++){
        if (!dart_discovery_peer_addr(rt->core, s, &a) || a.ip_len != 4) continue;
        dart_discovery_rt_tx1(rt, out, m, a.ip, dport);
        if (a.port && a.port != dport) dart_discovery_rt_tx1(rt, out, m, a.ip, a.port);
    }
}

static uint64_t dart_discovery_now_us(void){
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

static void dart_discovery_net_startup(void){
#ifdef _WIN32
    WSADATA w; WSAStartup(MAKEWORD(2,2), &w);
#endif
}

void dart_discovery_rt_feed(dart_discovery_rt *rt, const uint8_t *src_ip, uint8_t src_ip_len,
                    const void *dg, size_t len){
    if (!rt) return;
    dart_discovery_on_datagram(rt->core, src_ip, src_ip_len, dg, len, dart_discovery_now_us());
}

/* Every multicast send and join should pin to this one interface. */
uint32_t dart_discovery_mcast_if_for(uint32_t grp_naddr, uint16_t port){
    dart_discovery_sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a;
    uint32_t ip = htonl(INADDR_ANY);
    if (s == DART_DISCOVERY_BADSOCK) return ip;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = grp_naddr; a.sin_port = htons(port);
    if (connect(s, (struct sockaddr*)&a, sizeof a) == 0){
        struct sockaddr_in loc;
#ifdef _WIN32
        int ll = (int)sizeof loc;
#else
        socklen_t ll = sizeof loc;
#endif
        if (getsockname(s, (struct sockaddr*)&loc, &ll) == 0)
            ip = loc.sin_addr.s_addr;
    }
    DART_DISCOVERY_CLOSESOCK(s);
    return ip;
}

/* Fill buf from the platform CSPRNG. Returns 1 on success, 0 if unavailable. */
static int dart_discovery_os_random(void *buf, size_t len){
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

int dart_discovery_make_uuid4(uint8_t out[16]){
    if (!dart_discovery_os_random(out, 16)) return 0;
    out[6] = (uint8_t)((out[6] & 0x0Fu) | 0x40u);  /* version 4 */
    out[8] = (uint8_t)((out[8] & 0x3Fu) | 0x80u);  /* variant 10x */
    return 1;
}

static void dart_discovery_auto_uuid(uint8_t out[16]){
    char host[80]; uint64_t seed; size_t hl;
    if (dart_discovery_make_uuid4(out)) return;     /* normal path */

    /* No CSPRNG: derive a best-effort unique id from hostname, pid and clock. */
    memset(host, 0, sizeof host);
    gethostname(host, (int)sizeof host - 1);
    hl = strlen(host);
#ifdef _WIN32
    seed = ((uint64_t)GetCurrentProcessId() << 32) ^ dart_discovery_now_us();
#else
    seed = (uint64_t)getpid() ^ dart_discovery_now_us();
#endif
    dart_discovery_make_uuid(out, (const uint8_t*)host, hl, seed);
}

size_t dart_discovery_rt_required_memory(const dart_discovery_rt_config *cfg){
    dart_discovery_config c;
    size_t rt = (sizeof(struct dart_discovery_rt) + 15u) & ~(size_t)15u;
    if (!cfg) return 0;
    c = cfg->disc;
    if (c.announce_us == 0) c.announce_us = 1000000u;
    if (c.timeout_us  == 0) c.timeout_us  = c.announce_us * 7u / 2u;
    if (c.max_peers   == 0) c.max_peers   = 32u;
    return 16u + rt + dart_discovery_required_memory(&c);
}

dart_discovery_rt *dart_discovery_rt_open(void *mem, size_t cap, const dart_discovery_rt_config *cfg){
    dart_discovery_rt_config c;
    dart_discovery_rt *rt;
    uint8_t *base, *core_mem;
    size_t rtsz, need;
    dart_discovery_sock_t fd;
    int allzero = 1, i;
    int on = 1; unsigned char ttl, loop;
    struct sockaddr_in addr; struct ip_mreq mr;
    const char *group;

    if (!mem || !cfg) return NULL;
    c = *cfg;
    if (c.disc.announce_us == 0) c.disc.announce_us = 1000000u;
    if (c.disc.timeout_us  == 0) c.disc.timeout_us  = c.disc.announce_us * 7u / 2u;
    if (c.disc.max_peers   == 0) c.disc.max_peers   = 32u;
    group     = c.group     ? c.group     : "239.255.0.7";
    if (c.disc_port == 0)    c.disc_port  = 7400;
    ttl  = c.ttl ? c.ttl : 1;
    loop = 1;   /* always on (uuid self-filter drops echoes); needed for multi-instance per host */

    need = dart_discovery_rt_required_memory(&c);
    if (cap < need) return NULL;

    base = (uint8_t*)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    rt   = (dart_discovery_rt*)base;
    rtsz = (sizeof(struct dart_discovery_rt) + 15u) & ~(size_t)15u;
    core_mem = base + rtsz;

    /* auto-generate a UUID if the caller left it zero */
    for (i=0;i<16;i++) if (c.disc.uuid[i]) { allzero = 0; break; }
    if (allzero) dart_discovery_auto_uuid(c.disc.uuid);

    dart_discovery_net_startup();

    rt->core = dart_discovery_init(core_mem, cap - (size_t)(core_mem - (uint8_t*)mem), &c.disc);
    if (!rt->core) return NULL;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == DART_DISCOVERY_BADSOCK) return NULL;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const char*)&on, sizeof on);
#endif
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(c.disc_port);
    if (bind(fd, (struct sockaddr*)&addr, sizeof addr) != 0){ DART_DISCOVERY_CLOSESOCK(fd); return NULL; }

    /* pin join and egress to one deterministic interface */
    { uint32_t ifip = c.mcast_if ? inet_addr(c.mcast_if)
                                 : dart_discovery_mcast_if_for(inet_addr(group), c.disc_port);
      memset(&mr, 0, sizeof mr);
      mr.imr_multiaddr.s_addr = inet_addr(group);
      mr.imr_interface.s_addr = ifip;
      if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mr, sizeof mr) != 0){
          DART_DISCOVERY_CLOSESOCK(fd); return NULL;
      }
      setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&ifip, sizeof ifip); }
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL,  (const char*)&ttl,  sizeof ttl);
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof loop);

    rt->fd = fd;
    rt->max_peers = c.disc.max_peers;
    rt->n_seeds = 0;
    if (c.seeds){
        uint16_t k, ns = c.n_seeds;
        if (ns > DART_DISCOVERY_MAX_SEEDS) ns = DART_DISCOVERY_MAX_SEEDS;
        for (k=0;k<ns;k++) rt->seeds[k] = c.seeds[k];
        rt->n_seeds = ns;
    }
    memset(&rt->grp, 0, sizeof rt->grp);
    rt->grp.sin_family = AF_INET;
    rt->grp.sin_addr.s_addr = inet_addr(group);
    rt->grp.sin_port = htons(c.disc_port);
    return rt;
}

int dart_discovery_rt_poll(dart_discovery_rt *rt, int timeout_ms){
    dart_discovery_pollfd_t pfd;
    uint8_t out[DART_DISCOVERY_WIRE_MAX];
    int got = 0; size_t m;

    memset(&pfd, 0, sizeof pfd);
    pfd.fd = rt->fd; pfd.events = POLLIN;
    if (DART_DISCOVERY_POLL(&pfd, 1, timeout_ms) < 0) return -1;

    if (pfd.revents & POLLIN){
        uint8_t buf[DART_DISCOVERY_WIRE_MAX];
        struct sockaddr_in src; socklen_t slen = sizeof src;
        int n = (int)recvfrom(rt->fd, (char*)buf, (int)sizeof buf, 0,
                              (struct sockaddr*)&src, &slen);
        if (n > 0){
            uint8_t sip[4];
            memcpy(sip, &src.sin_addr.s_addr, 4);
            dart_discovery_on_datagram(rt->core, sip, 4, buf, (size_t)n, dart_discovery_now_us());
            got = 1;
        }
    }

    m = dart_discovery_update(rt->core, dart_discovery_now_us(), out, sizeof out);
    if (m) dart_discovery_rt_tx(rt, out, m);
    return got;
}

int dart_discovery_rt_settle(dart_discovery_rt *rt, int quiet_ms, int timeout_ms){
    uint64_t start, last_change, last_solicit = 0;
    uint16_t count;
    if (!rt) return 0;
    start = dart_discovery_now_us(); last_change = start;
    count = dart_discovery_peer_count(rt->core);
    for (;;){
        uint64_t now = dart_discovery_now_us(); uint16_t c;
        if (now - last_solicit >= 250000u){     /* (re)solicit ~4x/s so a lost one retries */
            dart_discovery_solicit(rt->core); last_solicit = now;
        }
        dart_discovery_rt_poll(rt, 10);          /* sends the solicit, takes in replies */
        now = dart_discovery_now_us();
        c = dart_discovery_peer_count(rt->core);
        if (c > count){ count = c; last_change = now; }   /* grew: keep waiting */
        if (count > 0 && now - last_change >= (uint64_t)quiet_ms*1000u) break;
        if (now - start >= (uint64_t)timeout_ms*1000u) break;
    }
    return (int)count;
}

void dart_discovery_rt_close(dart_discovery_rt *rt, int send_bye){
    if (!rt) return;
    if (send_bye){
        uint8_t out[DART_DISCOVERY_WIRE_MAX];
        size_t m = dart_discovery_leave(rt->core, out, sizeof out);
        if (m) dart_discovery_rt_tx(rt, out, m);
    }
    DART_DISCOVERY_CLOSESOCK(rt->fd);
#ifdef _WIN32
    WSACleanup();
#endif
}
