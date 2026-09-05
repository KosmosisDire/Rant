/* The one OS layer. A runtime speaks only these functions and never a sockaddr or an OS
 * ifdef. The contract and the address rules are in spec/platform.md. */
#ifndef DART_PLAT_H
#define DART_PLAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DART_SHM is auto detected where the bundled layer has it and DART_NO_SHM always wins.
 * This block mirrors transport/core.h exactly. Edit both together. */
#if !defined(DART_SHM) && !defined(DART_NO_SHM)
  #if defined(_WIN32) || defined(__linux__) || defined(__APPLE__) || \
      defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
      defined(__DragonFly__)
    #define DART_SHM
  #endif
#endif
#if defined(DART_SHM) && defined(DART_NO_SHM)
  #undef DART_SHM              /* both set, the opt out wins */
#endif

/* DART_THREADS follows the same shape. A new platform layer that implements the thread
 * contract below defines it itself. */
#if !defined(DART_THREADS) && !defined(DART_NO_THREADS)
  #if defined(_WIN32) || defined(__linux__) || defined(__APPLE__) || \
      defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
      defined(__DragonFly__) || defined(ESP_PLATFORM)
    #define DART_THREADS
  #elif defined(__has_include)
    #if __has_include(<pthread.h>)
      #define DART_THREADS   /* unknown POSIX with pthreads */
    #endif
  #endif
#endif
#if defined(DART_THREADS) && defined(DART_NO_THREADS)
  #undef DART_THREADS        /* both set, the opt out wins */
#endif

/* A POSIX fd or a Windows SOCKET, both fit in intptr_t. */
typedef intptr_t i_DartSock;
#define DART_SOCK_BAD ((i_DartSock)-1)

/* Mirrors struct pollfd without the OS header. */
#define DART_POLLIN 0x01
typedef struct { i_DartSock fd; short events; short revents; } i_DartPollfd;

/* Process wide net init and teardown. The OS refcounts matched calls, so pairs nest safely. */
int  i_dart_plat_startup(void);   /* 1 on success */
void i_dart_plat_cleanup(void);

/* Monotonic microseconds from an arbitrary epoch. */
uint64_t i_dart_plat_now_us(void);

/* CSPRNG fill. 0 means no entropy source and the caller falls back. */
int      i_dart_plat_random(void *buf, size_t len);
/* Host identity for the uuid fallback. Returns the bytes written. */
size_t   i_dart_plat_hostname(char *buf, size_t cap);
uint64_t i_dart_plat_pid(void);
/* Wall clock microseconds since the Unix epoch, for timestamps compared across hosts. */
uint64_t i_dart_plat_wall_us(void);

/* DART_PROC_STATS follows the same shape. When off the two functions below are absent and
 * every consumer compiles out with them, so a layer that cannot measure implements nothing. */
#if !defined(DART_PROC_STATS) && !defined(DART_NO_PROC_STATS)
  #if defined(_WIN32) || defined(__linux__) || defined(__APPLE__) || \
      defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
      defined(__DragonFly__) || defined(ESP_PLATFORM)
    #define DART_PROC_STATS
  #endif
#endif
#if defined(DART_PROC_STATS) && defined(DART_NO_PROC_STATS)
  #undef DART_PROC_STATS       /* both set, the opt out wins */
#endif
#ifdef DART_PROC_STATS
/* Per process, not per node. Any out pointer may be NULL. 0 on a transient OS failure. */
int      i_dart_plat_proc_stats(uint64_t *cpu_us, uint64_t *rss_bytes, uint64_t *peak_rss_bytes,
                                int *have_cpu);
/* Heap diagnostics. Only ESP has them, the others return 0 so callers omit the fields. */
int      i_dart_plat_heap_stats(uint64_t *total_bytes, uint64_t *free_bytes,
                                uint64_t *min_free_bytes, uint64_t *largest_free_block_bytes);
#endif /* DART_PROC_STATS */

/* The one heap dependency. ptr NULL allocates, size 0 frees and returns NULL. */
void    *i_dart_plat_realloc(void *ptr, size_t size);

/* UDP sockets */
i_DartSock i_dart_plat_udp_open(void);                   /* DART_SOCK_BAD on failure */
void      i_dart_plat_close(i_DartSock s);
/* if_naddr 0 is INADDR_ANY and port 0 is ephemeral. reuse sets SO_REUSEADDR, and
 * SO_REUSEPORT where it exists, before binding. 1 ok, 0 fail. */
int       i_dart_plat_bind(i_DartSock s, uint32_t if_naddr, uint16_t port, int reuse);
/* Bound port in host order, 0 on failure. */
uint16_t  i_dart_plat_local_port(i_DartSock s);
void      i_dart_plat_set_nonblock(i_DartSock s);
void      i_dart_plat_set_rcvbuf(i_DartSock s, int bytes);
void      i_dart_plat_set_sndbuf(i_DartSock s, int bytes);
/* Stops an ICMP port unreachable from failing the next recv. Windows only, no op elsewhere. */
void      i_dart_plat_suppress_connreset(i_DartSock s);

/* multicast */
void i_dart_plat_mcast_setif(i_DartSock s, uint32_t if_naddr);
void i_dart_plat_mcast_ttl  (i_DartSock s, uint8_t ttl);
void i_dart_plat_mcast_loop (i_DartSock s, int on);
int  i_dart_plat_mcast_join (i_DartSock s, uint32_t group_naddr, uint32_t if_naddr); /* 1 ok */
void i_dart_plat_mcast_leave(i_DartSock s, uint32_t group_naddr, uint32_t if_naddr);

/* datagram IO */
/* Bytes sent, or negative on error. Test i_dart_plat_would_block after a failure. */
int  i_dart_plat_send(i_DartSock s, const void *buf, size_t len,
                    const uint8_t ip[4], uint16_t port);
/* Bytes received, or 0 or negative when none. src_ip and src_port may be NULL. */
int  i_dart_plat_recv(i_DartSock s, void *buf, size_t cap,
                    uint8_t src_ip[4], uint16_t *src_port);
int  i_dart_plat_would_block(void);
/* Positive when ready, 0 on timeout, negative on error. */
int  i_dart_plat_poll(i_DartPollfd *fds, int n, int timeout_ms);
/* The OS's last socket error on the calling thread, for diagnostics. Keeps no state. */
int  i_dart_plat_last_socket_error(void);

/* address helpers, a naddr is network byte order */
uint32_t i_dart_plat_parse_ip(const char *dotted);          /* "1.2.3.4" to naddr */
uint32_t i_dart_plat_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
uint32_t i_dart_plat_ip4_to_naddr(const uint8_t ip[4]);
void     i_dart_plat_naddr_to_ip4(uint32_t naddr, uint8_t out[4]);
/* The source address the OS would use toward dst, with no packet sent. A diagnostic only. */
uint32_t i_dart_plat_route_src(uint32_t dst_naddr, uint16_t port);

/* One IPv4 interface, address and netmask in network order. */
typedef struct { uint32_t addr, mask; } i_DartIface;
/* The up, non loopback interfaces. A netmask the platform cannot report stays 0. */
int      i_dart_plat_local_ifaces(i_DartIface *out, int max);

/* threads, only under DART_THREADS */
#ifdef DART_THREADS
/* Opaque blobs keep OS headers out of this header. core.c checks the real types fit. */
typedef union { void *align_p; uint64_t align_8; unsigned char b[64]; } i_DartMutex;
typedef union { void *align_p; uint64_t align_8; unsigned char b[64]; } i_DartCond;
typedef union { void *align_p; uint64_t align_8; unsigned char b[32]; } i_DartThread;

/* start runs fn(arg) on a new thread, 1 on success. join blocks until fn returns. */
int      i_dart_plat_thread_start(i_DartThread *t, void (*fn)(void *), void *arg);
void     i_dart_plat_thread_join (i_DartThread *t);
/* Nonzero id of the calling thread, compared and never dereferenced. */
uint64_t i_dart_plat_thread_id   (void);

void i_dart_plat_mutex_init   (i_DartMutex *m);
void i_dart_plat_mutex_destroy(i_DartMutex *m);
void i_dart_plat_mutex_lock   (i_DartMutex *m);
void i_dart_plat_mutex_unlock (i_DartMutex *m);

void i_dart_plat_cond_init     (i_DartCond *c);
void i_dart_plat_cond_destroy  (i_DartCond *c);
/* Waits with m held and holds it again on return. May wake early or spuriously, so
 * callers loop on a predicate plus a monotonic deadline. */
void i_dart_plat_cond_wait     (i_DartCond *c, i_DartMutex *m, uint32_t timeout_us);
void i_dart_plat_cond_broadcast(i_DartCond *c);

/* A self connected loopback UDP socket that sits in a poll set so another thread can cut
 * a wait short. Signals coalesce. */
typedef struct { i_DartSock fd; } i_DartWaker;
int  i_dart_plat_waker_open  (i_DartWaker *w);   /* 1 on success */
int  i_dart_plat_waker_signal(i_DartWaker *w);   /* 1 when the signal went out */
void i_dart_plat_waker_drain (i_DartWaker *w);
void i_dart_plat_waker_close (i_DartWaker *w);
#endif /* DART_THREADS */

/* shared memory, only under DART_SHM */
#ifdef DART_SHM
/* create maps a fresh zero filled segment. attach maps an existing one whole and reports
 * its size, which detach needs. Both return the base, or NULL on failure. */
void *i_dart_plat_shm_create(const char *name, size_t bytes, void **handle);
void *i_dart_plat_shm_attach(const char *name, size_t *out_bytes, void **handle);
/* The creator passes unlink_it to also remove the OS object. */
void  i_dart_plat_shm_detach(void *base, size_t bytes, void *handle, int unlink_it);
/* A stable per host id for the same host pre check. A successful attach is the real gate. */
void  i_dart_plat_host_uuid(uint8_t out[16]);
/* Cross process 64 bit atomics with acquire and release order, for the chunk stamp. */
uint64_t i_dart_plat_atomic_load64 (volatile uint64_t *p);
void     i_dart_plat_atomic_store64(volatile uint64_t *p, uint64_t v);
#endif /* DART_SHM */

#ifdef __cplusplus
}
#endif
#endif /* DART_PLAT_H */
