/* The Windows and POSIX implementation. The only file in DART with an OS ifdef. */

/* feature test macros must precede the first system header */
#if !defined(_WIN32)
  #ifndef _POSIX_C_SOURCE
  #define _POSIX_C_SOURCE 200809L
  #endif
  #ifndef _DEFAULT_SOURCE
  #define _DEFAULT_SOURCE 1
  #endif
  #ifndef _DARWIN_C_SOURCE
  #define _DARWIN_C_SOURCE 1   /* Darwin hides its own extensions under _POSIX_C_SOURCE without this */
  #endif
#endif

#include "core.h"
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <bcrypt.h>            /* BCryptGenRandom */
  #include <mmsystem.h>          /* timeBeginPeriod */
  #ifndef PSAPI_VERSION
  #define PSAPI_VERSION 2        /* GetProcessMemoryInfo from kernel32, no psapi.lib */
  #endif
  #include <psapi.h>             /* GetProcessMemoryInfo */
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
    #include <ifaddrs.h>         /* getifaddrs */
    #include <net/if.h>          /* IFF_UP and IFF_LOOPBACK */
  #endif
  #include <unistd.h>
  #if defined(ESP_PLATFORM)
    #include <sys/poll.h>       /* the ESP newlib has no poll.h */
  #else
    #include <poll.h>
  #endif
  #include <time.h>
  #ifdef DART_THREADS
    #include <pthread.h>
    #if defined(ESP_PLATFORM)
      #include <freertos/FreeRTOS.h>
      #include <freertos/task.h>   /* xTaskGetCurrentTaskHandle */
    #endif
  #endif
  #include <fcntl.h>
  #include <errno.h>
  #include <stdio.h>
  #include <stdlib.h>           /* arc4random_buf */
  #ifdef DART_PROC_STATS
    #if defined(ESP_PLATFORM)
      #include <esp_heap_caps.h>  /* heap_caps_get_* */
      #include <freertos/FreeRTOS.h>
      #include <freertos/task.h>  /* uxTaskGetSystemState */
    #else
      #include <sys/resource.h>   /* getrusage */
      #if defined(__APPLE__)
        #include <mach/mach.h>    /* task_info */
      #endif
    #endif
  #endif
  #if defined(ESP_PLATFORM)
    #include <esp_random.h>     /* esp_fill_random */
    #include <esp_netif.h>      /* esp_netif_get_ip_info */
    #if defined(__has_include) && __has_include(<esp_mac.h>)
      #include <esp_mac.h>      /* IDF 5: esp_efuse_mac_get_default */
    #else
      #include <esp_system.h>   /* IDF 4: the same declaration */
    #endif
  #elif defined(__linux__)
    #include <sys/random.h>     /* getrandom */
  #endif
  typedef socklen_t i_DartSocklen;
  #define DART__FD(s) ((int)(s))
#endif

/* lifecycle */
#ifdef _WIN32
static LARGE_INTEGER i_dart_plat_qpc_freq;   /* set in startup, lazily elsewhere */
int i_dart_plat_startup(void){
    WSADATA w;
    /* both calls are refcounted by the OS per process, so matched calls need no counter here */
    if (WSAStartup(MAKEWORD(2,2), &w) != 0) return 0;
  #ifndef DART_NO_HIGHRES_TIMER
    timeBeginPeriod(1);  /* the default 15.6 ms tick throttles ACK and repair rates */
  #endif
    QueryPerformanceFrequency(&i_dart_plat_qpc_freq);
    return 1;
}
void i_dart_plat_cleanup(void){
  #ifndef DART_NO_HIGHRES_TIMER
    timeEndPeriod(1);
  #endif
    WSACleanup();
}
#else
int  i_dart_plat_startup(void){ return 1; }
void i_dart_plat_cleanup(void){}
#endif

/* clock */
uint64_t i_dart_plat_now_us(void){
#ifdef _WIN32
    LARGE_INTEGER c;
    if (!i_dart_plat_qpc_freq.QuadPart) QueryPerformanceFrequency(&i_dart_plat_qpc_freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart * 1000000ull) / (uint64_t)i_dart_plat_qpc_freq.QuadPart);
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
#endif
}

/* entropy and host */
int i_dart_plat_random(void *buf, size_t len){
#if defined(_WIN32)
    /* a NULL handle selects the system preferred RNG, 0 is success */
    return BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(ESP_PLATFORM)
    esp_fill_random(buf, len);       /* hardware RNG, true random while RF is up */
    return 1;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
      defined(__NetBSD__) || defined(__DragonFly__)
    arc4random_buf(buf, len);        /* cannot fail */
    return 1;
#else
    {
        uint8_t *p = (uint8_t*)buf; size_t got = 0;
  #if defined(__linux__)
        while (got < len){
            ssize_t r = getrandom(p + got, len - got, 0);
            if (r < 0){ if (errno == EINTR) continue; break; }
            got += (size_t)r;
        }
        if (got == len) return 1;
  #endif
        {
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

size_t i_dart_plat_hostname(char *buf, size_t cap){
    if (!buf || cap == 0) return 0;
    buf[0] = 0;
#if defined(ESP_PLATFORM)
    /* lwIP has no gethostname. The factory MAC is unique and readable before any
       interface is up, which is what the uuid fallback wants. */
    {   static const char hex[] = "0123456789abcdef";
        uint8_t mac[6]; size_t i;
        if (cap >= 17 && esp_efuse_mac_get_default(mac) == ESP_OK){
            memcpy(buf, "esp-", 4);
            for (i = 0; i < 6; i++){
                buf[4 + i*2]     = hex[mac[i] >> 4];
                buf[4 + i*2 + 1] = hex[mac[i] & 0x0F];
            }
            buf[16] = 0;
        }
    }
#else
    gethostname(buf, (int)cap - 1);
#endif
    buf[cap - 1] = 0;
    return strlen(buf);
}

uint64_t i_dart_plat_pid(void){
#ifdef _WIN32
    return (uint64_t)GetCurrentProcessId();
#else
    return (uint64_t)getpid();
#endif
}

/* process usage and wall clock */
#ifdef DART_PROC_STATS
int i_dart_plat_proc_stats(uint64_t *cpu_us, uint64_t *rss_bytes, uint64_t *peak_rss_bytes,
                           int *have_cpu){
#if defined(_WIN32)
    FILETIME created, exited, kern, user; PROCESS_MEMORY_COUNTERS pmc;
    ULARGE_INTEGER uk, uu;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kern, &user)) return 0;
    uk.LowPart = kern.dwLowDateTime; uk.HighPart = kern.dwHighDateTime;
    uu.LowPart = user.dwLowDateTime; uu.HighPart = user.dwHighDateTime;
    if (cpu_us) *cpu_us = (uk.QuadPart + uu.QuadPart) / 10u;   /* 100 ns to us */
    if (have_cpu) *have_cpu = 1;
    pmc.cb = sizeof pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return 0;
    if (rss_bytes)      *rss_bytes      = (uint64_t)pmc.WorkingSetSize;
    if (peak_rss_bytes) *peak_rss_bytes = (uint64_t)pmc.PeakWorkingSetSize;
    return 1;
#elif defined(ESP_PLATFORM)
    /* The firmware image is the process: RSS is heap in use and peak RSS is the free heap
       low water mark. CPU needs the ESP timer run time stats, else it is omitted. */
    {   size_t total   = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
        size_t freeb   = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
        size_t minfree = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
        if (cpu_us)         *cpu_us         = 0;
        if (have_cpu) *have_cpu = 0;
#if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS \
 && defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && CONFIG_FREERTOS_USE_TRACE_FACILITY \
 && defined(CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS) && CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS \
 && defined(CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER) && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
        {   static TaskStatus_t *tasks; static UBaseType_t cap;
            static uint32_t prev_total, prev_idle; static uint64_t busy_us; static int started;
            UBaseType_t need = uxTaskGetNumberOfTasks(), got, i;
            uint32_t total_run = 0, idle_run = 0;
            if (need > cap){
                TaskStatus_t *p = (TaskStatus_t*)realloc(tasks, (size_t)need * sizeof *tasks);
                if (!p) goto esp_cpu_done;
                tasks = p; cap = need;
            }
            got = uxTaskGetSystemState(tasks, cap, &total_run);
            for (i = 0; i < got; i++)
                if (strncmp(tasks[i].pcTaskName, "IDLE", 4) == 0) idle_run += tasks[i].ulRunTimeCounter;
            if (started){
                uint32_t dt = total_run - prev_total, di = idle_run - prev_idle;
                uint64_t span = (uint64_t)dt * (uint64_t)portNUM_PROCESSORS;
                busy_us += span > di ? span - di : 0;
            }
            prev_total = total_run; prev_idle = idle_run; started = 1;
            if (cpu_us) *cpu_us = busy_us;
            if (have_cpu) *have_cpu = 1;
        }
esp_cpu_done:
#endif
        if (rss_bytes)      *rss_bytes      = (uint64_t)(total > freeb   ? total - freeb   : 0);
        if (peak_rss_bytes) *peak_rss_bytes = (uint64_t)(total > minfree ? total - minfree : 0);
        return 1;
    }
#else
    {   struct rusage ru;
        if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
        if (cpu_us) *cpu_us = (uint64_t)ru.ru_utime.tv_sec * 1000000ull + (uint64_t)ru.ru_utime.tv_usec
                            + (uint64_t)ru.ru_stime.tv_sec * 1000000ull + (uint64_t)ru.ru_stime.tv_usec;
        if (have_cpu) *have_cpu = 1;
  #if defined(__APPLE__)
        if (peak_rss_bytes) *peak_rss_bytes = (uint64_t)ru.ru_maxrss;         /* bytes on macOS */
        if (rss_bytes){   /* current: task_info */
            struct mach_task_basic_info info; mach_msg_type_number_t cnt = MACH_TASK_BASIC_INFO_COUNT;
            *rss_bytes = (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                                    (task_info_t)&info, &cnt) == KERN_SUCCESS)
                       ? (uint64_t)info.resident_size : 0;
        }
  #elif defined(__linux__)
        if (peak_rss_bytes) *peak_rss_bytes = (uint64_t)ru.ru_maxrss * 1024u; /* KB on Linux */
        if (rss_bytes){   /* current: statm field 2 */
            FILE *f = fopen("/proc/self/statm", "rb");
            unsigned long total_pages = 0, res_pages = 0;
            *rss_bytes = 0;
            if (f){
                if (fscanf(f, "%lu %lu", &total_pages, &res_pages) == 2)
                    *rss_bytes = (uint64_t)res_pages * (uint64_t)sysconf(_SC_PAGESIZE);
                fclose(f);
            }
        }
  #else
        if (peak_rss_bytes) *peak_rss_bytes = (uint64_t)ru.ru_maxrss * 1024u; /* KB on the BSDs */
        if (rss_bytes)      *rss_bytes      = 0;   /* no cheap current RSS read */
  #endif
        return 1;
    }
#endif
}

int i_dart_plat_heap_stats(uint64_t *total_bytes, uint64_t *free_bytes,
                           uint64_t *min_free_bytes, uint64_t *largest_free_block_bytes){
#if defined(ESP_PLATFORM)
    if (total_bytes)              *total_bytes = (uint64_t)heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    if (free_bytes)               *free_bytes = (uint64_t)heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    if (min_free_bytes)           *min_free_bytes = (uint64_t)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    if (largest_free_block_bytes) *largest_free_block_bytes =
        (uint64_t)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    return 1;
#else
    (void)total_bytes; (void)free_bytes; (void)min_free_bytes; (void)largest_free_block_bytes;
    return 0;
#endif
}
#endif /* DART_PROC_STATS */

uint64_t i_dart_plat_wall_us(void){
#ifdef _WIN32
    FILETIME ft; ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return (u.QuadPart - 116444736000000000ull) / 10u;   /* FILETIME epoch to Unix, 100 ns to us */
#else
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
#endif
}

void *i_dart_plat_realloc(void *ptr, size_t size){
    if (size == 0){ free(ptr); return NULL; }
    return realloc(ptr, size);
}

/* UDP sockets */
i_DartSock i_dart_plat_udp_open(void){
#ifdef _WIN32
    SOCKET fd = socket(AF_INET, SOCK_DGRAM, 0);
    return (fd == INVALID_SOCKET) ? DART_SOCK_BAD : (i_DartSock)fd;
#else
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    return (fd < 0) ? DART_SOCK_BAD : (i_DartSock)fd;
#endif
}

void i_dart_plat_close(i_DartSock s){
    if (s == DART_SOCK_BAD) return;
#ifdef _WIN32
    closesocket((SOCKET)s);
#else
    close((int)s);
#endif
}

int i_dart_plat_bind(i_DartSock s, uint32_t if_naddr, uint16_t port, int reuse){
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
    a.sin_addr.s_addr = if_naddr;
    a.sin_port = htons(port);
    return bind(DART__FD(s), (struct sockaddr*)&a, sizeof a) == 0;
}

uint16_t i_dart_plat_local_port(i_DartSock s){
    struct sockaddr_in a; i_DartSocklen ll = sizeof a;
    memset(&a, 0, sizeof a);
    if (getsockname(DART__FD(s), (struct sockaddr*)&a, &ll) != 0) return 0;
    return ntohs(a.sin_port);
}

void i_dart_plat_set_nonblock(i_DartSock s){
#ifdef _WIN32
    u_long nb = 1; ioctlsocket((SOCKET)s, FIONBIO, &nb);
#else
    int fl = fcntl((int)s, F_GETFL, 0);
    if (fl != -1) fcntl((int)s, F_SETFL, fl | O_NONBLOCK);
#endif
}

void i_dart_plat_set_rcvbuf(i_DartSock s, int bytes){
    setsockopt(DART__FD(s), SOL_SOCKET, SO_RCVBUF, (const char*)&bytes, sizeof bytes);
}
void i_dart_plat_set_sndbuf(i_DartSock s, int bytes){
    setsockopt(DART__FD(s), SOL_SOCKET, SO_SNDBUF, (const char*)&bytes, sizeof bytes);
}

void i_dart_plat_suppress_connreset(i_DartSock s){
#ifdef _WIN32
    BOOL off = FALSE; DWORD bv = 0;
    WSAIoctl((SOCKET)s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &bv, NULL, NULL);
#else
    (void)s;
#endif
}

/* multicast */
void i_dart_plat_mcast_setif(i_DartSock s, uint32_t if_naddr){
    setsockopt(DART__FD(s), IPPROTO_IP, IP_MULTICAST_IF, (const char*)&if_naddr, sizeof if_naddr);
}
void i_dart_plat_mcast_ttl(i_DartSock s, uint8_t ttl){
    unsigned char t = ttl;
    setsockopt(DART__FD(s), IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&t, sizeof t);
}
void i_dart_plat_mcast_loop(i_DartSock s, int on){
    unsigned char l = (unsigned char)(on ? 1 : 0);
    setsockopt(DART__FD(s), IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&l, sizeof l);
}
int i_dart_plat_mcast_join(i_DartSock s, uint32_t group_naddr, uint32_t if_naddr){
    struct ip_mreq mr; memset(&mr, 0, sizeof mr);
    mr.imr_multiaddr.s_addr = group_naddr;
    mr.imr_interface.s_addr = if_naddr;
    return setsockopt(DART__FD(s), IPPROTO_IP, IP_ADD_MEMBERSHIP,
                      (const char*)&mr, sizeof mr) == 0;
}
/* Best effort. An interface that went away may have dropped its membership already. */
void i_dart_plat_mcast_leave(i_DartSock s, uint32_t group_naddr, uint32_t if_naddr){
    struct ip_mreq mr; memset(&mr, 0, sizeof mr);
    mr.imr_multiaddr.s_addr = group_naddr;
    mr.imr_interface.s_addr = if_naddr;
    setsockopt(DART__FD(s), IPPROTO_IP, IP_DROP_MEMBERSHIP, (const char*)&mr, sizeof mr);
}

/* datagram IO */
int i_dart_plat_send(i_DartSock s, const void *buf, size_t len,
                   const uint8_t ip[4], uint16_t port){
    struct sockaddr_in d;
    memset(&d, 0, sizeof d);
    d.sin_family = AF_INET;
    memcpy(&d.sin_addr.s_addr, ip, 4);
    d.sin_port = htons(port);
    return (int)sendto(DART__FD(s), (const char*)buf, (int)len, 0,
                       (struct sockaddr*)&d, sizeof d);
}

int i_dart_plat_recv(i_DartSock s, void *buf, size_t cap,
                   uint8_t src_ip[4], uint16_t *src_port){
    struct sockaddr_in src; i_DartSocklen sl = sizeof src;
    int n;
    memset(&src, 0, sizeof src);
    n = (int)recvfrom(DART__FD(s), (char*)buf, (int)cap, 0,
                      (struct sockaddr*)&src, &sl);
#ifdef _WIN32
    /* Windows fails an oversized datagram with WSAEMSGSIZE after filling the buffer. POSIX
       delivers the prefix. Deliver it here too, so discovery can grow and refetch. */
    if (n < 0 && WSAGetLastError() == WSAEMSGSIZE) n = (int)cap;
#endif
    if (n > 0){
        if (src_ip)   memcpy(src_ip, &src.sin_addr.s_addr, 4);
        if (src_port) *src_port = ntohs(src.sin_port);
    }
    return n;
}

int i_dart_plat_would_block(void){
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

int i_dart_plat_last_socket_error(void){
#ifdef _WIN32
    return WSAGetLastError();   /* winsock keeps its error off errno */
#else
    return errno;
#endif
}

int i_dart_plat_poll(i_DartPollfd *fds, int n, int timeout_ms){
    /* callers poll a few sockets, so the translation buffer lives on the stack */
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

/* address helpers */
uint32_t i_dart_plat_parse_ip(const char *dotted){
    return dotted ? (uint32_t)inet_addr(dotted) : 0;
}
uint32_t i_dart_plat_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d){
    return htonl(((uint32_t)a << 24) | ((uint32_t)b << 16) |
                 ((uint32_t)c << 8)  |  (uint32_t)d);
}
uint32_t i_dart_plat_ip4_to_naddr(const uint8_t ip[4]){
    uint32_t n; memcpy(&n, ip, 4); return n;
}
void i_dart_plat_naddr_to_ip4(uint32_t naddr, uint8_t out[4]){
    memcpy(out, &naddr, 4);
}

uint32_t i_dart_plat_route_src(uint32_t dst_naddr, uint16_t port){
    i_DartSock s = i_dart_plat_udp_open();
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
    i_dart_plat_close(s);
    return ip;
}

#if defined(_WIN32)
/* The SIO_GET_INTERFACE_LIST flag bits, when the SDK headers do not define them. */
#ifndef IFF_UP
#define IFF_UP 0x00000001
#endif
#ifndef IFF_LOOPBACK
#define IFF_LOOPBACK 0x00000004
#endif
int i_dart_plat_local_ifaces(i_DartIface *out, int max){
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
        out[n].addr = a->sin_addr.s_addr;
        out[n].mask = info[i].iiNetmask.AddressIn.sin_addr.s_addr;
        n++;
    }
    return n;
}
#elif defined(ESP_PLATFORM)
/* lwIP has no getifaddrs, so name the netifs the IDF defines. WiFi or Ethernet must be up
 * before the node opens. */
int i_dart_plat_local_ifaces(i_DartIface *out, int max){
    static const char *const keys[] = { "WIFI_STA_DEF", "ETH_DEF", "WIFI_AP_DEF" };
    int n = 0; unsigned i;
    if (!out || max <= 0) return 0;
    for (i = 0; i < sizeof keys / sizeof keys[0] && n < max; i++){
        esp_netif_t *nif = esp_netif_get_handle_from_ifkey(keys[i]);
        esp_netif_ip_info_t info;
        if (nif && esp_netif_get_ip_info(nif, &info) == ESP_OK && info.ip.addr != 0){
            out[n].addr = info.ip.addr;   /* esp_ip4_addr is network order, our naddr */
            out[n].mask = info.netmask.addr;
            n++;
        }
    }
    return n;
}
#else
int i_dart_plat_local_ifaces(i_DartIface *out, int max){
    struct ifaddrs *ifs = NULL, *p;
    int n = 0;
    if (!out || max <= 0 || getifaddrs(&ifs) != 0) return 0;
    for (p = ifs; p && n < max; p = p->ifa_next){
        struct sockaddr_in *a;
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
        a = (struct sockaddr_in*)p->ifa_addr;
        out[n].addr = a->sin_addr.s_addr;
        out[n].mask = (p->ifa_netmask && p->ifa_netmask->sa_family == AF_INET)
                        ? ((struct sockaddr_in*)p->ifa_netmask)->sin_addr.s_addr : 0u;
        n++;
    }
    freeifaddrs(ifs);
    return n;
}
#endif

/* threads */
#ifdef DART_THREADS

/* The opaque blobs must fit the real OS types. C99 has no _Static_assert. */
#define DART__FITS(name, real, blob) \
    typedef char name[(sizeof(real) <= sizeof(blob)) ? 1 : -1]

#ifdef _WIN32

typedef struct { HANDLE h; void (*fn)(void *); void *arg; } i_DartThreadImpl;
DART__FITS(i_dart_plat_mutex_fits,  SRWLOCK,            i_DartMutex);
DART__FITS(i_dart_plat_cond_fits,   CONDITION_VARIABLE, i_DartCond);
DART__FITS(i_dart_plat_thread_fits, i_DartThreadImpl,   i_DartThread);

static DWORD WINAPI i_dart_plat_thread_tramp(LPVOID p){
    i_DartThreadImpl *t = (i_DartThreadImpl *)p;
    t->fn(t->arg);
    return 0;
}
int i_dart_plat_thread_start(i_DartThread *t, void (*fn)(void *), void *arg){
    i_DartThreadImpl *ti = (i_DartThreadImpl *)t;
    ti->fn = fn; ti->arg = arg;
    ti->h = CreateThread(NULL, 0, i_dart_plat_thread_tramp, ti, 0, NULL);
    return ti->h != NULL;
}
void i_dart_plat_thread_join(i_DartThread *t){
    i_DartThreadImpl *ti = (i_DartThreadImpl *)t;
    if (!ti->h) return;
    WaitForSingleObject(ti->h, INFINITE);
    CloseHandle(ti->h);
    ti->h = NULL;
}
uint64_t i_dart_plat_thread_id(void){ return (uint64_t)GetCurrentThreadId(); }

/* SRWLOCK is pointer sized, pairs with condvars and is not recursive, which the condvar
   contract needs. Node reentrancy is an owner id check above this layer. */
void i_dart_plat_mutex_init   (i_DartMutex *m){ InitializeSRWLock((PSRWLOCK)m); }
void i_dart_plat_mutex_destroy(i_DartMutex *m){ (void)m; }
void i_dart_plat_mutex_lock   (i_DartMutex *m){ AcquireSRWLockExclusive((PSRWLOCK)m); }
void i_dart_plat_mutex_unlock (i_DartMutex *m){ ReleaseSRWLockExclusive((PSRWLOCK)m); }

void i_dart_plat_cond_init   (i_DartCond *c){ InitializeConditionVariable((PCONDITION_VARIABLE)c); }
void i_dart_plat_cond_destroy(i_DartCond *c){ (void)c; }
void i_dart_plat_cond_wait(i_DartCond *c, i_DartMutex *m, uint32_t timeout_us){
    /* round up in 64 bits: 0xFFFFFFFF + 999 would wrap and make the longest waits 1 ms spins */
    DWORD ms = (DWORD)(((uint64_t)timeout_us + 999u) / 1000u);
    SleepConditionVariableSRW((PCONDITION_VARIABLE)c, (PSRWLOCK)m, ms ? ms : 1, 0);
}
void i_dart_plat_cond_broadcast(i_DartCond *c){ WakeAllConditionVariable((PCONDITION_VARIABLE)c); }

#else /* POSIX */

typedef struct { pthread_t t; void (*fn)(void *); void *arg; } i_DartThreadImpl;
DART__FITS(i_dart_plat_mutex_fits,  pthread_mutex_t,  i_DartMutex);
DART__FITS(i_dart_plat_cond_fits,   pthread_cond_t,   i_DartCond);
DART__FITS(i_dart_plat_thread_fits, i_DartThreadImpl, i_DartThread);

static void *i_dart_plat_thread_tramp(void *p){
    i_DartThreadImpl *t = (i_DartThreadImpl *)p;
    t->fn(t->arg);
    return NULL;
}
int i_dart_plat_thread_start(i_DartThread *t, void (*fn)(void *), void *arg){
    i_DartThreadImpl *ti = (i_DartThreadImpl *)t;
    ti->fn = fn; ti->arg = arg;
    return pthread_create(&ti->t, NULL, i_dart_plat_thread_tramp, ti) == 0;
}
void i_dart_plat_thread_join(i_DartThread *t){
    pthread_join(((i_DartThreadImpl *)t)->t, NULL);
}
uint64_t i_dart_plat_thread_id(void){
#if defined(ESP_PLATFORM)
    /* pthread_self aborts on ESP-IDF from a task not made by pthread_create, like the
       Arduino loopTask. The task handle is a unique id on any task. */
    return (uint64_t)(uintptr_t)xTaskGetCurrentTaskHandle();
#else
    return (uint64_t)(uintptr_t)pthread_self();
#endif
}

void i_dart_plat_mutex_init   (i_DartMutex *m){ pthread_mutex_init((pthread_mutex_t *)m, NULL); }
void i_dart_plat_mutex_destroy(i_DartMutex *m){ pthread_mutex_destroy((pthread_mutex_t *)m); }
void i_dart_plat_mutex_lock   (i_DartMutex *m){ pthread_mutex_lock((pthread_mutex_t *)m); }
void i_dart_plat_mutex_unlock (i_DartMutex *m){ pthread_mutex_unlock((pthread_mutex_t *)m); }

void i_dart_plat_cond_init(i_DartCond *c){
#if defined(__linux__)
    /* the monotonic clock, so a wall clock step cannot stretch a timeout */
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init((pthread_cond_t *)c, &a);
    pthread_condattr_destroy(&a);
#else
    pthread_cond_init((pthread_cond_t *)c, NULL);
#endif
}
void i_dart_plat_cond_destroy(i_DartCond *c){ pthread_cond_destroy((pthread_cond_t *)c); }
void i_dart_plat_cond_wait(i_DartCond *c, i_DartMutex *m, uint32_t timeout_us){
#if defined(__APPLE__)
    struct timespec rel;
    rel.tv_sec  = (time_t)(timeout_us / 1000000u);
    rel.tv_nsec = (long)(timeout_us % 1000000u) * 1000L;
    pthread_cond_timedwait_relative_np((pthread_cond_t *)c, (pthread_mutex_t *)m, &rel);
#else
    /* Linux waits on the monotonic clock set at init. Elsewhere a wall clock jump can cut
       the wait short, which the caller's predicate loop absorbs. */
    struct timespec ts;
  #if defined(__linux__)
    clock_gettime(CLOCK_MONOTONIC, &ts);
  #else
    clock_gettime(CLOCK_REALTIME, &ts);
  #endif
    ts.tv_sec  += (time_t)(timeout_us / 1000000u);
    ts.tv_nsec += (long)(timeout_us % 1000000u) * 1000L;
    if (ts.tv_nsec >= 1000000000L){ ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait((pthread_cond_t *)c, (pthread_mutex_t *)m, &ts);
#endif
}
void i_dart_plat_cond_broadcast(i_DartCond *c){ pthread_cond_broadcast((pthread_cond_t *)c); }

#endif /* _WIN32 */

/* Bound to loopback and connected to itself, so only its own signals ever arrive. */
int i_dart_plat_waker_open(i_DartWaker *w){
    struct sockaddr_in a; i_DartSocklen al = sizeof a;
    w->fd = i_dart_plat_udp_open();
    if (w->fd == DART_SOCK_BAD) return 0;
    memset(&a, 0, sizeof a);
    if (!i_dart_plat_bind(w->fd, i_dart_plat_ipv4(127, 0, 0, 1), 0, 0)) goto fail;
    if (getsockname(DART__FD(w->fd), (struct sockaddr *)&a, &al) != 0) goto fail;
    if (connect(DART__FD(w->fd), (struct sockaddr *)&a, sizeof a) != 0) goto fail;
    i_dart_plat_set_nonblock(w->fd);
    return 1;
fail:
    i_dart_plat_close(w->fd);
    w->fd = DART_SOCK_BAD;
    return 0;
}
int i_dart_plat_waker_signal(i_DartWaker *w){
    char b = 1;
    if (w->fd == DART_SOCK_BAD) return 0;
    return (int)send(DART__FD(w->fd), &b, 1, 0) == 1;
}
void i_dart_plat_waker_drain(i_DartWaker *w){
    char b[64];
    if (w->fd == DART_SOCK_BAD) return;
    while (recv(DART__FD(w->fd), b, sizeof b, 0) > 0) {}
}
void i_dart_plat_waker_close(i_DartWaker *w){
    i_dart_plat_close(w->fd);
    w->fd = DART_SOCK_BAD;
}

#endif /* DART_THREADS */

/* shared memory */
#ifdef DART_SHM
#ifndef _WIN32
  #include <sys/mman.h>            /* shm_open and mmap */
  #include <sys/stat.h>            /* fstat */
#endif

/* A 128 bit id from bytes, two FNV-1a passes with distinct seeds. Not cryptographic. */
static void i_dart_plat_hash16(const void *data, size_t len, uint8_t out[16]){
    const uint8_t *p = (const uint8_t*)data; size_t i;
    uint64_t a = 14695981039346656037ull, b = 1099511628211ull;
    for (i = 0; i < len; i++){
        a = (a ^ p[i]) * 1099511628211ull;
        b = (b ^ (uint8_t)(p[i] + 0x9Eu)) * 1099511628211ull;
    }
    for (i = 0; i < 8; i++){ out[i] = (uint8_t)(a >> (8*i)); out[8+i] = (uint8_t)(b >> (8*i)); }
}

void i_dart_plat_host_uuid(uint8_t out[16]){
#if defined(__linux__)
    FILE *f = fopen("/etc/machine-id", "rb");   /* 32 hex chars, a 128 bit id */
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
    {   char host[256]; size_t n = i_dart_plat_hostname(host, sizeof host);
        if (n == 0){ host[0] = '?'; n = 1; }
        i_dart_plat_hash16(host, n, out);
    }
}

#ifdef _WIN32
void *i_dart_plat_shm_create(const char *name, size_t bytes, void **handle){
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
void *i_dart_plat_shm_attach(const char *name, size_t *out_bytes, void **handle){
    HANDLE h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    void *base; MEMORY_BASIC_INFORMATION mbi;
    if (!h) return NULL;
    base = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);   /* 0 maps the whole section */
    if (!base){ CloseHandle(h); return NULL; }
    if (out_bytes) *out_bytes = VirtualQuery(base, &mbi, sizeof mbi) ? (size_t)mbi.RegionSize : 0;
    *handle = h;
    return base;
}
void i_dart_plat_shm_detach(void *base, size_t bytes, void *handle, int unlink_it){
    (void)bytes; (void)unlink_it;   /* the object dies when the last handle closes */
    if (base) UnmapViewOfFile(base);
    if (handle) CloseHandle((HANDLE)handle);
}
uint64_t i_dart_plat_atomic_load64(volatile uint64_t *p){
    return (uint64_t)InterlockedCompareExchange64((volatile LONGLONG*)p, 0, 0);
}
void i_dart_plat_atomic_store64(volatile uint64_t *p, uint64_t v){
    InterlockedExchange64((volatile LONGLONG*)p, (LONGLONG)v);
}
#else /* POSIX */
void *i_dart_plat_shm_create(const char *name, size_t bytes, void **handle){
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
void *i_dart_plat_shm_attach(const char *name, size_t *out_bytes, void **handle){
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
void i_dart_plat_shm_detach(void *base, size_t bytes, void *handle, int unlink_it){
    if (base && base != MAP_FAILED) munmap(base, bytes);
    if (handle){
        if (unlink_it) shm_unlink((const char*)handle);
        free(handle);
    }
}
uint64_t i_dart_plat_atomic_load64(volatile uint64_t *p){
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
void i_dart_plat_atomic_store64(volatile uint64_t *p, uint64_t v){
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
#endif /* _WIN32 */
#endif /* DART_SHM */
