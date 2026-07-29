/* dart_plat: the one platform layer. Every OS dependency the runtimes need lives
 * behind this contract: a monotonic clock, UDP sockets, multicast, entropy,
 * the source-address route probe, and threads (thread/mutex/condvar/waker,
 * auto-detected as DART_THREADS; DART_NO_THREADS opts out). The layers above
 * (discovery_rt, node) speak
 * only dart_plat_* and never touch a sockaddr, winsock, or a platform #ifdef, so
 * a new platform is one new implementation of this header. A platform that
 * already has BSD sockets needs no new code: the bundled implementation covers
 * Windows and POSIX (Linux/macOS/BSD/ESP-lwIP). Pure IO: stripped under *_SANS_IO
 * with the runtime layers. On non-MSVC Windows link -lws2_32 -lbcrypt.
 *
 * Address convention: endpoints (send/recv, peers, seeds) are a uint8_t ip[4]
 * plus a host-order uint16_t port. Multicast group and interface addresses are a
 * uint32_t in NETWORK byte order ("naddr", as from i_dart_plat_parse_ip /
 * i_dart_plat_ipv4). The two are the same four bytes; move between them with
 * i_dart_plat_ip4_to_naddr / i_dart_plat_naddr_to_ip4.
 */
#ifndef DART_PLAT_H
#define DART_PLAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SHM (the zero-fragment same-host path): DART_SHM is AUTO-DETECTED on where the
 * bundled platform layer provides it (Windows file mappings; shm_open/mmap on
 * Linux/macOS/BSD), off elsewhere (ESP-IDF and unknown targets have no shm_open),
 * and DART_NO_SHM always wins (strip it explicitly, e.g. to drop the Linux -lrt).
 * A new platform layer that implements the i_dart_plat_shm_* contract declares
 * support by defining DART_SHM itself. This block mirrors dart_transport.h
 * EXACTLY so every TU agrees whichever header it saw first. */
#if !defined(DART_SHM) && !defined(DART_NO_SHM)
  #if defined(_WIN32) || defined(__linux__) || defined(__APPLE__) || \
      defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
      defined(__DragonFly__)
    #define DART_SHM
  #endif
#endif
#if defined(DART_SHM) && defined(DART_NO_SHM)
  #undef DART_SHM              /* both set: the opt-out wins */
#endif

/* Threading (the node lock + optional service thread) follows the same flag shape:
 * DART_THREADS is AUTO-DETECTED on where the bundled platform layer provides it
 * (Windows; POSIX with pthreads: Linux/macOS/BSD/ESP-IDF, plus any other libc that
 * advertises <pthread.h>), off elsewhere, and DART_NO_THREADS always wins. A NEW
 * platform implementation that provides the thread/mutex/cond/waker contract below
 * declares support by defining DART_THREADS itself (in its build flags or before
 * this header), which turns the threaded node on with no other change. */
#if !defined(DART_THREADS) && !defined(DART_NO_THREADS)
  #if defined(_WIN32) || defined(__linux__) || defined(__APPLE__) || \
      defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
      defined(__DragonFly__) || defined(ESP_PLATFORM)
    #define DART_THREADS
  #elif defined(__has_include)
    #if __has_include(<pthread.h>)
      #define DART_THREADS   /* unknown POSIX with pthreads: the bundled layer covers it */
    #endif
  #endif
#endif
#if defined(DART_THREADS) && defined(DART_NO_THREADS)
  #undef DART_THREADS        /* both set: the opt-out wins */
#endif

/* Opaque socket handle: a POSIX fd or a Windows SOCKET, both fit in intptr_t. */
typedef intptr_t i_DartSock;
#define DART_SOCK_BAD ((i_DartSock)-1)

/* Poll-set entry; mirrors struct pollfd but platform-neutral. */
#define DART_POLLIN 0x01
typedef struct { i_DartSock fd; short events; short revents; } i_DartPollfd;

/* Process-wide net init/teardown (WSAStartup/WSACleanup; no-op elsewhere).
 * The OS refcounts matched calls per process, so a node and its discovery
 * opening/closing in turn pair safely from any thread.
 * startup returns 1 on success, 0 on failure. */
int  i_dart_plat_startup(void);
void i_dart_plat_cleanup(void);

/* Monotonic microseconds from an arbitrary epoch. */
uint64_t i_dart_plat_now_us(void);

/* CSPRNG fill; 1 on success, 0 if no entropy source (caller falls back). */
int      i_dart_plat_random(void *buf, size_t len);
/* Best-effort host identity for a UUID fallback when the CSPRNG is unavailable. */
size_t   i_dart_plat_hostname(char *buf, size_t cap);   /* returns bytes written */
uint64_t i_dart_plat_pid(void);
/* Wall-clock microseconds since the Unix epoch, for log timestamps that must compare
 * across nodes (the monotonic clock's epoch is arbitrary and per host). */
uint64_t i_dart_plat_wall_us(void);

/* Process-usage stats follow the DART_SHM / DART_THREADS flag shape: DART_PROC_STATS is
 * AUTO-DETECTED where the bundled layer can measure (Windows; POSIX with getrusage:
 * Linux/macOS/BSD; ESP-IDF via the FreeRTOS heap allocator), off elsewhere (bare metal
 * has no accounting), and DART_NO_PROC_STATS always wins. When OFF the function below is
 * ABSENT and every consumer is compiled out with it (the @dart/meta snapshot simply
 * omits its proc section; all other stats are unaffected), so a platform layer with no
 * measurement implements NOTHING. A new platform layer that can measure declares
 * support by defining DART_PROC_STATS itself. */
#if !defined(DART_PROC_STATS) && !defined(DART_NO_PROC_STATS)
  #if defined(_WIN32) || defined(__linux__) || defined(__APPLE__) || \
      defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
      defined(__DragonFly__) || defined(ESP_PLATFORM)
    #define DART_PROC_STATS
  #endif
#endif
#if defined(DART_PROC_STATS) && defined(DART_NO_PROC_STATS)
  #undef DART_PROC_STATS       /* both set: the opt-out wins */
#endif
#ifdef DART_PROC_STATS
/* Process-wide resource usage for the introspection endpoint's proc section:
 * cumulative CPU microseconds (user + kernel), current resident set bytes, and peak
 * resident bytes. Any out-pointer may be NULL. Returns 1 on success, 0 on a transient
 * OS failure; a platform may fill peak but not current (current then reports 0). Per
 * PROCESS, not per node: several nodes in one process report the same numbers
 * (consumers dedup by pid). */
int      i_dart_plat_proc_stats(uint64_t *cpu_us, uint64_t *rss_bytes, uint64_t *peak_rss_bytes,
                                int *have_cpu);
/* Optional heap diagnostics for the introspection endpoint. ESP reports the default-capability
 * heap's total/free/low-water/largest-contiguous-block bytes; other platforms return 0 so
 * callers omit these fields rather than pretending RSS is a heap capacity. */
int      i_dart_plat_heap_stats(uint64_t *total_bytes, uint64_t *free_bytes,
                                uint64_t *min_free_bytes, uint64_t *largest_free_block_bytes);
#endif /* DART_PROC_STATS */

/* realloc-style heap hook backing a node's dynamic memory mode: ptr NULL =
 * allocate, size 0 = free (returns NULL). The single heap dependency, so the node
 * layer holds no <stdlib.h>; a target with a custom heap overrides just this. */
void    *i_dart_plat_realloc(void *ptr, size_t size);

/* --- UDP sockets --- */
i_DartSock i_dart_plat_udp_open(void);                   /* DART_SOCK_BAD on failure */
void      i_dart_plat_close(i_DartSock s);
/* Bind to if_naddr (0 = INADDR_ANY) : port (0 = OS ephemeral). reuse sets
 * SO_REUSEADDR (+ SO_REUSEPORT where it exists) before binding. 1 ok, 0 fail. */
int       i_dart_plat_bind(i_DartSock s, uint32_t if_naddr, uint16_t port, int reuse);
/* Bound port in host order (read an ephemeral bind back); 0 on failure. */
uint16_t  i_dart_plat_local_port(i_DartSock s);
void      i_dart_plat_set_nonblock(i_DartSock s);
void      i_dart_plat_set_rcvbuf(i_DartSock s, int bytes);
void      i_dart_plat_set_sndbuf(i_DartSock s, int bytes);
/* Stop a bounced datagram (ICMP port-unreachable) from failing the next recv on
 * a shared RX socket (Windows SIO_UDP_CONNRESET; no-op elsewhere). */
void      i_dart_plat_suppress_connreset(i_DartSock s);

/* --- multicast --- */
void i_dart_plat_mcast_setif(i_DartSock s, uint32_t if_naddr);
void i_dart_plat_mcast_ttl  (i_DartSock s, uint8_t ttl);
void i_dart_plat_mcast_loop (i_DartSock s, int on);
int  i_dart_plat_mcast_join (i_DartSock s, uint32_t group_naddr, uint32_t if_naddr); /* 1 ok */
void i_dart_plat_mcast_leave(i_DartSock s, uint32_t group_naddr, uint32_t if_naddr);

/* --- datagram IO --- */
/* sendto: returns bytes sent, <0 on error (test i_dart_plat_would_block). */
int  i_dart_plat_send(i_DartSock s, const void *buf, size_t len,
                    const uint8_t ip[4], uint16_t port);
/* recvfrom: returns bytes (>0), 0 or <0 if none. src_ip/src_port out, may be NULL. */
int  i_dart_plat_recv(i_DartSock s, void *buf, size_t cap,
                    uint8_t src_ip[4], uint16_t *src_port);
int  i_dart_plat_would_block(void);
/* poll up to n fds for timeout_ms; >0 ready, 0 timeout, <0 error. */
int  i_dart_plat_poll(i_DartPollfd *fds, int n, int timeout_ms);
/* The OS's last socket error for the calling thread (WSAGetLastError on Windows,
 * errno elsewhere), for diagnostics after a failed socket call. Just reads the OS;
 * keeps no state. */
int  i_dart_plat_last_socket_error(void);

/* --- address helpers (uint32_t naddr is network byte order) --- */
uint32_t i_dart_plat_parse_ip(const char *dotted);          /* "1.2.3.4" -> naddr */
uint32_t i_dart_plat_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
uint32_t i_dart_plat_ip4_to_naddr(const uint8_t ip[4]);
void     i_dart_plat_naddr_to_ip4(uint32_t naddr, uint8_t out[4]);
/* Source address the OS would use to reach dst_naddr:port (connect + getsockname
 * on an unbound UDP socket; no packet leaves). 0 on failure. A diagnostic (see
 * tools/if_probe_check.c): discovery no longer routes anything through it. */
uint32_t i_dart_plat_route_src(uint32_t dst_naddr, uint16_t port);

/* One of this host's IPv4 interfaces: address + netmask, both network order. */
typedef struct { uint32_t addr, mask; } i_DartIface;
/* Enumerate this host's usable IPv4 interfaces (up, non-loopback) into out[0..max),
 * returning the count written (0 if none, or if the platform offers no enumeration).
 * Discovery joins the multicast group on every one and announces out every one, and
 * the netmasks tell the core which peer addresses are on a segment we share. A
 * platform that cannot report a netmask leaves it 0 (unknown, never matches). */
int      i_dart_plat_local_ifaces(i_DartIface *out, int max);

/* --- threads (present only under DART_THREADS; see the detection above) -------
 * What the node runtime's thread safety and background service thread need: a
 * thread, a mutex, a condvar, and a waker that can interrupt i_dart_plat_poll
 * from another thread. Opaque aligned blobs keep OS headers out of this header;
 * core.c static-asserts the real types fit. Off (undetected platform or
 * DART_NO_THREADS) the node reverts to the single-threaded contract; a new
 * platform layer implements this contract and defines DART_THREADS.
 * POSIX: link -lpthread (older toolchains). */
#ifdef DART_THREADS
typedef union { void *align_p; uint64_t align_8; unsigned char b[64]; } i_DartMutex;
typedef union { void *align_p; uint64_t align_8; unsigned char b[64]; } i_DartCond;
typedef union { void *align_p; uint64_t align_8; unsigned char b[32]; } i_DartThread;

/* start runs fn(arg) on a new thread; 1 on success. join blocks until fn returns. */
int      i_dart_plat_thread_start(i_DartThread *t, void (*fn)(void *), void *arg);
void     i_dart_plat_thread_join (i_DartThread *t);
/* Nonzero id of the calling thread (compared, never dereferenced). */
uint64_t i_dart_plat_thread_id   (void);

void i_dart_plat_mutex_init   (i_DartMutex *m);
void i_dart_plat_mutex_destroy(i_DartMutex *m);
void i_dart_plat_mutex_lock   (i_DartMutex *m);
void i_dart_plat_mutex_unlock (i_DartMutex *m);

void i_dart_plat_cond_init     (i_DartCond *c);
void i_dart_plat_cond_destroy  (i_DartCond *c);
/* Wait up to timeout_us with m held; m is reacquired before returning. May wake
 * early or spuriously: callers loop on a predicate plus a monotonic deadline. */
void i_dart_plat_cond_wait     (i_DartCond *c, i_DartMutex *m, uint32_t timeout_us);
void i_dart_plat_cond_broadcast(i_DartCond *c);

/* Waker: a self-pipe whose fd sits in a normal i_dart_plat_poll set, so another
 * thread can cut a blocking wait short. A nonblocking UDP socket bound to
 * 127.0.0.1:ephemeral and connected to itself: connect filters foreign
 * datagrams, repeat signals coalesce in the socket buffer, and it is the one
 * mechanism that is pollable on both Windows and POSIX with no new poll API. */
typedef struct { i_DartSock fd; } i_DartWaker;
int  i_dart_plat_waker_open  (i_DartWaker *w);   /* 1 on success */
int  i_dart_plat_waker_signal(i_DartWaker *w);   /* 1 = the signal went out */
void i_dart_plat_waker_drain (i_DartWaker *w);
void i_dart_plat_waker_close (i_DartWaker *w);
#endif /* DART_THREADS */

/* --- shared memory (only under DART_SHM; see the detection above) -------------
 * The few primitives src/dart_shm.h needs. Absent when DART_SHM is off (undetected
 * platform or DART_NO_SHM), so a target lacking shm support builds and links
 * without them; a new platform layer implements this contract and defines
 * DART_SHM. (POSIX: shm_open may want -lrt on older glibc.) */
#ifdef DART_SHM
/* create maps a FRESH named segment of `bytes` RW (zero-filled). attach maps an
 * EXISTING one WHOLE: the reader needn't know its size, it discovers it from the OS
 * and reports it via *out_bytes (which detach then needs). *handle receives an OS
 * handle that detach needs. Return the mapped base, or NULL on failure. Names: POSIX
 * "/name" form, Windows a plain object name; dart_shm derives one from the node uuid. */
void *i_dart_plat_shm_create(const char *name, size_t bytes, void **handle);
void *i_dart_plat_shm_attach(const char *name, size_t *out_bytes, void **handle);
/* unmap; the creator passes unlink_it=1 to also remove the OS object. */
void  i_dart_plat_shm_detach(void *base, size_t bytes, void *handle, int unlink_it);
/* stable per-host id (Linux machine-id, else a hostname hash) for the same-host
 * pre-check; a successful attach is the real gate. */
void  i_dart_plat_host_uuid(uint8_t out[16]);
/* cross-process 64-bit atomic for the chunk generation stamp (acquire/release). */
uint64_t i_dart_plat_atomic_load64 (volatile uint64_t *p);
void     i_dart_plat_atomic_store64(volatile uint64_t *p, uint64_t v);
#endif /* DART_SHM */

#ifdef __cplusplus
}
#endif
#endif /* DART_PLAT_H */
