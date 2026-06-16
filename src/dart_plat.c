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

#include "dart_plat.h"
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
  #ifndef SIO_UDP_CONNRESET
  #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
  #endif
  typedef int dart__socklen;
  #define DART__FD(s) ((SOCKET)(s))
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <poll.h>
  #include <time.h>
  #include <fcntl.h>
  #include <errno.h>
  #include <stdio.h>
  #include <stdlib.h>           /* arc4random_buf on macOS/BSD */
  #if defined(ESP_PLATFORM)
    #include <esp_random.h>     /* esp_fill_random (HW RNG) */
  #elif defined(__linux__)
    #include <sys/random.h>     /* getrandom(2) */
  #endif
  typedef socklen_t dart__socklen;
  #define DART__FD(s) ((int)(s))
#endif

/* ----------------------------------------------------------------- lifecycle */
#ifdef _WIN32
static int dart__wsa_refs = 0;
int dart_plat_startup(void){
    WSADATA w;
    if (dart__wsa_refs == 0 && WSAStartup(MAKEWORD(2,2), &w) != 0) return 0;
    dart__wsa_refs++;
    return 1;
}
void dart_plat_cleanup(void){
    if (dart__wsa_refs > 0 && --dart__wsa_refs == 0) WSACleanup();
}
#else
int  dart_plat_startup(void){ return 1; }
void dart_plat_cleanup(void){}
#endif

/* --------------------------------------------------------------------- clock */
uint64_t dart_plat_now_us(void){
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

/* ------------------------------------------------------------ entropy / host */
int dart_plat_random(void *buf, size_t len){
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

size_t dart_plat_hostname(char *buf, size_t cap){
    if (!buf || cap == 0) return 0;
    buf[0] = 0;
    gethostname(buf, (int)cap - 1);
    buf[cap - 1] = 0;
    return strlen(buf);
}

uint64_t dart_plat_pid(void){
#ifdef _WIN32
    return (uint64_t)GetCurrentProcessId();
#else
    return (uint64_t)getpid();
#endif
}

/* --------------------------------------------------------------- UDP sockets */
dart_sock dart_plat_udp_open(void){
#ifdef _WIN32
    SOCKET fd = socket(AF_INET, SOCK_DGRAM, 0);
    return (fd == INVALID_SOCKET) ? DART_SOCK_BAD : (dart_sock)fd;
#else
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    return (fd < 0) ? DART_SOCK_BAD : (dart_sock)fd;
#endif
}

void dart_plat_close(dart_sock s){
    if (s == DART_SOCK_BAD) return;
#ifdef _WIN32
    closesocket((SOCKET)s);
#else
    close((int)s);
#endif
}

int dart_plat_bind(dart_sock s, uint32_t if_naddr, uint16_t port, int reuse){
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

uint16_t dart_plat_local_port(dart_sock s){
    struct sockaddr_in a; dart__socklen ll = sizeof a;
    memset(&a, 0, sizeof a);
    if (getsockname(DART__FD(s), (struct sockaddr*)&a, &ll) != 0) return 0;
    return ntohs(a.sin_port);
}

void dart_plat_set_nonblock(dart_sock s){
#ifdef _WIN32
    u_long nb = 1; ioctlsocket((SOCKET)s, FIONBIO, &nb);
#else
    int fl = fcntl((int)s, F_GETFL, 0);
    if (fl != -1) fcntl((int)s, F_SETFL, fl | O_NONBLOCK);
#endif
}

void dart_plat_set_rcvbuf(dart_sock s, int bytes){
    setsockopt(DART__FD(s), SOL_SOCKET, SO_RCVBUF, (const char*)&bytes, sizeof bytes);
}
void dart_plat_set_sndbuf(dart_sock s, int bytes){
    setsockopt(DART__FD(s), SOL_SOCKET, SO_SNDBUF, (const char*)&bytes, sizeof bytes);
}

void dart_plat_suppress_connreset(dart_sock s){
#ifdef _WIN32
    BOOL off = FALSE; DWORD bv = 0;
    WSAIoctl((SOCKET)s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &bv, NULL, NULL);
#else
    (void)s;
#endif
}

/* ----------------------------------------------------------------- multicast */
void dart_plat_mcast_setif(dart_sock s, uint32_t if_naddr){
    setsockopt(DART__FD(s), IPPROTO_IP, IP_MULTICAST_IF, (const char*)&if_naddr, sizeof if_naddr);
}
void dart_plat_mcast_ttl(dart_sock s, uint8_t ttl){
    unsigned char t = ttl;
    setsockopt(DART__FD(s), IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&t, sizeof t);
}
void dart_plat_mcast_loop(dart_sock s, int on){
    unsigned char l = (unsigned char)(on ? 1 : 0);
    setsockopt(DART__FD(s), IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&l, sizeof l);
}
int dart_plat_mcast_join(dart_sock s, uint32_t grp_naddr, uint32_t if_naddr){
    struct ip_mreq mr; memset(&mr, 0, sizeof mr);
    mr.imr_multiaddr.s_addr = grp_naddr;
    mr.imr_interface.s_addr = if_naddr;
    return setsockopt(DART__FD(s), IPPROTO_IP, IP_ADD_MEMBERSHIP,
                      (const char*)&mr, sizeof mr) == 0;
}

/* --------------------------------------------------------------- datagram IO */
int dart_plat_send(dart_sock s, const void *buf, size_t len,
                   const uint8_t ip[4], uint16_t port){
    struct sockaddr_in d;
    memset(&d, 0, sizeof d);
    d.sin_family = AF_INET;
    memcpy(&d.sin_addr.s_addr, ip, 4);
    d.sin_port = htons(port);
    return (int)sendto(DART__FD(s), (const char*)buf, (int)len, 0,
                       (struct sockaddr*)&d, sizeof d);
}

int dart_plat_recv(dart_sock s, void *buf, size_t cap,
                   uint8_t src_ip[4], uint16_t *src_port){
    struct sockaddr_in src; dart__socklen sl = sizeof src;
    int n;
    memset(&src, 0, sizeof src);
    n = (int)recvfrom(DART__FD(s), (char*)buf, (int)cap, 0,
                      (struct sockaddr*)&src, &sl);
    if (n > 0){
        if (src_ip)   memcpy(src_ip, &src.sin_addr.s_addr, 4);
        if (src_port) *src_port = ntohs(src.sin_port);
    }
    return n;
}

int dart_plat_would_block(void){
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

int dart_plat_poll(dart_pollfd *fds, int n, int timeout_ms){
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
uint32_t dart_plat_parse_ip(const char *dotted){
    return dotted ? (uint32_t)inet_addr(dotted) : 0;
}
uint32_t dart_plat_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d){
    return htonl(((uint32_t)a << 24) | ((uint32_t)b << 16) |
                 ((uint32_t)c << 8)  |  (uint32_t)d);
}
uint32_t dart_plat_ip4_to_naddr(const uint8_t ip[4]){
    uint32_t n; memcpy(&n, ip, 4); return n;
}
void dart_plat_naddr_to_ip4(uint32_t naddr, uint8_t out[4]){
    memcpy(out, &naddr, 4);
}

uint32_t dart_plat_route_src(uint32_t dst_naddr, uint16_t port){
    dart_sock s = dart_plat_udp_open();
    struct sockaddr_in a;
    uint32_t ip = 0;                         /* INADDR_ANY on failure */
    if (s == DART_SOCK_BAD) return ip;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = dst_naddr; a.sin_port = htons(port);
    if (connect(DART__FD(s), (struct sockaddr*)&a, sizeof a) == 0){
        struct sockaddr_in loc; dart__socklen ll = sizeof loc;
        if (getsockname(DART__FD(s), (struct sockaddr*)&loc, &ll) == 0)
            ip = loc.sin_addr.s_addr;
    }
    dart_plat_close(s);
    return ip;
}
