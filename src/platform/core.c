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

#include "core.h"
#include <string.h>
#include <stdlib.h>           /* malloc/realloc/free behind dart_plat_realloc */

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <bcrypt.h>            /* BCryptGenRandom (CSPRNG) */
  #include <mmsystem.h>          /* timeBeginPeriod */
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
    #include <ifaddrs.h>         /* getifaddrs: enumerate local interfaces */
    #include <net/if.h>          /* IFF_UP / IFF_LOOPBACK */
  #endif
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
  typedef socklen_t i_DartSocklen;
  #define DART__FD(s) ((int)(s))
#endif

/* ----------------------------------------------------------------- lifecycle */
#ifdef _WIN32
static int dart__wsa_refs = 0;
int dart_plat_startup(void){
    WSADATA w;
    if (dart__wsa_refs == 0){
        if (WSAStartup(MAKEWORD(2,2), &w) != 0) return 0;
  #ifndef DART_NO_HIGHRES_TIMER
        timeBeginPeriod(1);  /* 1ms timer: default ~15.6ms throttles sub ACK/repair rate */
  #endif
    }
    dart__wsa_refs++;
    return 1;
}
void dart_plat_cleanup(void){
    if (dart__wsa_refs > 0 && --dart__wsa_refs == 0){
  #ifndef DART_NO_HIGHRES_TIMER
        timeEndPeriod(1);
  #endif
        WSACleanup();
    }
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

/* The one heap dependency, kept behind the platform layer so the node holds no
 * <stdlib.h>: ptr NULL = allocate, size 0 = free (returns NULL), else realloc. */
void *dart_plat_realloc(void *ptr, size_t size){
    if (size == 0){ free(ptr); return NULL; }
    return realloc(ptr, size);
}

/* --------------------------------------------------------------- UDP sockets */
i_DartSock dart_plat_udp_open(void){
#ifdef _WIN32
    SOCKET fd = socket(AF_INET, SOCK_DGRAM, 0);
    return (fd == INVALID_SOCKET) ? DART_SOCK_BAD : (i_DartSock)fd;
#else
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    return (fd < 0) ? DART_SOCK_BAD : (i_DartSock)fd;
#endif
}

void dart_plat_close(i_DartSock s){
    if (s == DART_SOCK_BAD) return;
#ifdef _WIN32
    closesocket((SOCKET)s);
#else
    close((int)s);
#endif
}

int dart_plat_bind(i_DartSock s, uint32_t if_naddr, uint16_t port, int reuse){
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

uint16_t dart_plat_local_port(i_DartSock s){
    struct sockaddr_in a; i_DartSocklen ll = sizeof a;
    memset(&a, 0, sizeof a);
    if (getsockname(DART__FD(s), (struct sockaddr*)&a, &ll) != 0) return 0;
    return ntohs(a.sin_port);
}

void dart_plat_set_nonblock(i_DartSock s){
#ifdef _WIN32
    u_long nb = 1; ioctlsocket((SOCKET)s, FIONBIO, &nb);
#else
    int fl = fcntl((int)s, F_GETFL, 0);
    if (fl != -1) fcntl((int)s, F_SETFL, fl | O_NONBLOCK);
#endif
}

void dart_plat_set_rcvbuf(i_DartSock s, int bytes){
    setsockopt(DART__FD(s), SOL_SOCKET, SO_RCVBUF, (const char*)&bytes, sizeof bytes);
}
void dart_plat_set_sndbuf(i_DartSock s, int bytes){
    setsockopt(DART__FD(s), SOL_SOCKET, SO_SNDBUF, (const char*)&bytes, sizeof bytes);
}

void dart_plat_suppress_connreset(i_DartSock s){
#ifdef _WIN32
    BOOL off = FALSE; DWORD bv = 0;
    WSAIoctl((SOCKET)s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &bv, NULL, NULL);
#else
    (void)s;
#endif
}

/* ----------------------------------------------------------------- multicast */
void dart_plat_mcast_setif(i_DartSock s, uint32_t if_naddr){
    setsockopt(DART__FD(s), IPPROTO_IP, IP_MULTICAST_IF, (const char*)&if_naddr, sizeof if_naddr);
}
void dart_plat_mcast_ttl(i_DartSock s, uint8_t ttl){
    unsigned char t = ttl;
    setsockopt(DART__FD(s), IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&t, sizeof t);
}
void dart_plat_mcast_loop(i_DartSock s, int on){
    unsigned char l = (unsigned char)(on ? 1 : 0);
    setsockopt(DART__FD(s), IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&l, sizeof l);
}
int dart_plat_mcast_join(i_DartSock s, uint32_t group_naddr, uint32_t if_naddr){
    struct ip_mreq mr; memset(&mr, 0, sizeof mr);
    mr.imr_multiaddr.s_addr = group_naddr;
    mr.imr_interface.s_addr = if_naddr;
    return setsockopt(DART__FD(s), IPPROTO_IP, IP_ADD_MEMBERSHIP,
                      (const char*)&mr, sizeof mr) == 0;
}

/* --------------------------------------------------------------- datagram IO */
int dart_plat_send(i_DartSock s, const void *buf, size_t len,
                   const uint8_t ip[4], uint16_t port){
    struct sockaddr_in d;
    memset(&d, 0, sizeof d);
    d.sin_family = AF_INET;
    memcpy(&d.sin_addr.s_addr, ip, 4);
    d.sin_port = htons(port);
    return (int)sendto(DART__FD(s), (const char*)buf, (int)len, 0,
                       (struct sockaddr*)&d, sizeof d);
}

int dart_plat_recv(i_DartSock s, void *buf, size_t cap,
                   uint8_t src_ip[4], uint16_t *src_port){
    struct sockaddr_in src; i_DartSocklen sl = sizeof src;
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

int dart_plat_poll(i_DartPollfd *fds, int n, int timeout_ms){
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
    i_DartSock s = dart_plat_udp_open();
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
    dart_plat_close(s);
    return ip;
}

#if defined(_WIN32)
/* SIO_GET_INTERFACE_LIST flag values (mirrors the BSD IFF_* bits) if the SDK's
 * headers didn't define them for this WSAIoctl. */
#ifndef IFF_UP
#define IFF_UP 0x00000001
#endif
#ifndef IFF_LOOPBACK
#define IFF_LOOPBACK 0x00000004
#endif
int dart_plat_local_ipv4s(uint32_t *out, int max){
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
        out[n++] = a->sin_addr.s_addr;
    }
    return n;
}
#elif defined(ESP_PLATFORM)
int dart_plat_local_ipv4s(uint32_t *out, int max){ (void)out; (void)max; return 0; }
#else
int dart_plat_local_ipv4s(uint32_t *out, int max){
    struct ifaddrs *ifs = NULL, *p;
    int n = 0;
    if (!out || max <= 0 || getifaddrs(&ifs) != 0) return 0;
    for (p = ifs; p && n < max; p = p->ifa_next){
        struct sockaddr_in *a;
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
        a = (struct sockaddr_in*)p->ifa_addr;
        out[n++] = a->sin_addr.s_addr;
    }
    freeifaddrs(ifs);
    return n;
}
#endif

/* ------------------------------------------------------------- shared memory */
#ifdef DART_SHM
#ifndef _WIN32
  #include <sys/mman.h>            /* shm_open/mmap; fcntl/unistd/stdlib already in */
  #include <sys/stat.h>            /* fstat: a reader learns a segment's size from the OS */
#endif

/* 128-bit non-cryptographic id from a byte string: two FNV-1a passes with distinct
 * seeds. Stable per input, enough to pre-filter same-host (attach is the real gate). */
static void dart__hash16(const void *data, size_t len, uint8_t out[16]){
    const uint8_t *p = (const uint8_t*)data; size_t i;
    uint64_t a = 14695981039346656037ull, b = 1099511628211ull;
    for (i = 0; i < len; i++){
        a = (a ^ p[i]) * 1099511628211ull;
        b = (b ^ (uint8_t)(p[i] + 0x9Eu)) * 1099511628211ull;
    }
    for (i = 0; i < 8; i++){ out[i] = (uint8_t)(a >> (8*i)); out[8+i] = (uint8_t)(b >> (8*i)); }
}

void dart_plat_host_uuid(uint8_t out[16]){
#if defined(__linux__)
    FILE *f = fopen("/etc/machine-id", "rb");   /* 32 hex chars = a 128-bit id */
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
    {   char host[256]; size_t n = dart_plat_hostname(host, sizeof host);
        if (n == 0){ host[0] = '?'; n = 1; }
        dart__hash16(host, n, out);
    }
}

#ifdef _WIN32
void *dart_plat_shm_create(const char *name, size_t bytes, void **handle){
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
void *dart_plat_shm_attach(const char *name, size_t *out_bytes, void **handle){
    HANDLE h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    void *base; MEMORY_BASIC_INFORMATION mbi;
    if (!h) return NULL;
    base = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);   /* 0 = the whole section */
    if (!base){ CloseHandle(h); return NULL; }
    if (out_bytes) *out_bytes = VirtualQuery(base, &mbi, sizeof mbi) ? (size_t)mbi.RegionSize : 0;
    *handle = h;
    return base;
}
void dart_plat_shm_detach(void *base, size_t bytes, void *handle, int unlink_it){
    (void)bytes; (void)unlink_it;   /* the object dies when the last handle closes */
    if (base) UnmapViewOfFile(base);
    if (handle) CloseHandle((HANDLE)handle);
}
uint64_t dart_plat_atomic_load64(volatile uint64_t *p){
    return (uint64_t)InterlockedCompareExchange64((volatile LONGLONG*)p, 0, 0);
}
void dart_plat_atomic_store64(volatile uint64_t *p, uint64_t v){
    InterlockedExchange64((volatile LONGLONG*)p, (LONGLONG)v);
}
#else /* POSIX */
void *dart_plat_shm_create(const char *name, size_t bytes, void **handle){
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
void *dart_plat_shm_attach(const char *name, size_t *out_bytes, void **handle){
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
void dart_plat_shm_detach(void *base, size_t bytes, void *handle, int unlink_it){
    if (base && base != MAP_FAILED) munmap(base, bytes);
    if (handle){
        if (unlink_it) shm_unlink((const char*)handle);
        free(handle);
    }
}
uint64_t dart_plat_atomic_load64(volatile uint64_t *p){
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
void dart_plat_atomic_store64(volatile uint64_t *p, uint64_t v){
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
#endif /* _WIN32 */
#endif /* DART_SHM */
