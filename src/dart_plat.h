/* dart_plat: the one platform layer. Every OS dependency the runtimes need lives
 * behind this contract: a monotonic clock, UDP sockets, multicast, entropy, and
 * the source-address route probe. The layers above (discovery_rt, node) speak
 * only dart_plat_* and never touch a sockaddr, winsock, or a platform #ifdef, so
 * a new platform is one new implementation of this header. A platform that
 * already has BSD sockets needs no new code: the bundled implementation covers
 * Windows and POSIX (Linux/macOS/BSD/ESP-lwIP). Pure IO: stripped under *_SANS_IO
 * with the runtime layers. On non-MSVC Windows link -lws2_32 -lbcrypt.
 *
 * Address convention: endpoints (send/recv, peers, seeds) are a uint8_t ip[4]
 * plus a host-order uint16_t port. Multicast group and interface addresses are a
 * uint32_t in NETWORK byte order ("naddr", as from dart_plat_parse_ip /
 * dart_plat_ipv4). The two are the same four bytes; move between them with
 * dart_plat_ip4_to_naddr / dart_plat_naddr_to_ip4.
 */
#ifndef DART_PLAT_H
#define DART_PLAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque socket handle: a POSIX fd or a Windows SOCKET, both fit in intptr_t. */
typedef intptr_t dart_sock;
#define DART_SOCK_BAD ((dart_sock)-1)

/* Poll-set entry; mirrors struct pollfd but platform-neutral. */
#define DART_POLLIN 0x01
typedef struct { dart_sock fd; short events; short revents; } dart_pollfd;

/* Process-wide net init/teardown (WSAStartup/WSACleanup; no-op elsewhere).
 * Refcounted, so a node and its discovery opening/closing in turn pair safely.
 * startup returns 1 on success, 0 on failure. */
int  dart_plat_startup(void);
void dart_plat_cleanup(void);

/* Monotonic microseconds from an arbitrary epoch. */
uint64_t dart_plat_now_us(void);

/* CSPRNG fill; 1 on success, 0 if no entropy source (caller falls back). */
int      dart_plat_random(void *buf, size_t len);
/* Best-effort host identity for a UUID fallback when the CSPRNG is unavailable. */
size_t   dart_plat_hostname(char *buf, size_t cap);   /* returns bytes written */
uint64_t dart_plat_pid(void);

/* --- UDP sockets --- */
dart_sock dart_plat_udp_open(void);                   /* DART_SOCK_BAD on failure */
void      dart_plat_close(dart_sock s);
/* Bind to if_naddr (0 = INADDR_ANY) : port (0 = OS ephemeral). reuse sets
 * SO_REUSEADDR (+ SO_REUSEPORT where it exists) before binding. 1 ok, 0 fail. */
int       dart_plat_bind(dart_sock s, uint32_t if_naddr, uint16_t port, int reuse);
/* Bound port in host order (read an ephemeral bind back); 0 on failure. */
uint16_t  dart_plat_local_port(dart_sock s);
void      dart_plat_set_nonblock(dart_sock s);
void      dart_plat_set_rcvbuf(dart_sock s, int bytes);
void      dart_plat_set_sndbuf(dart_sock s, int bytes);
/* Stop a bounced datagram (ICMP port-unreachable) from failing the next recv on
 * a shared RX socket (Windows SIO_UDP_CONNRESET; no-op elsewhere). */
void      dart_plat_suppress_connreset(dart_sock s);

/* --- multicast --- */
void dart_plat_mcast_setif(dart_sock s, uint32_t if_naddr);
void dart_plat_mcast_ttl  (dart_sock s, uint8_t ttl);
void dart_plat_mcast_loop (dart_sock s, int on);
int  dart_plat_mcast_join (dart_sock s, uint32_t grp_naddr, uint32_t if_naddr); /* 1 ok */

/* --- datagram IO --- */
/* sendto: returns bytes sent, <0 on error (test dart_plat_would_block). */
int  dart_plat_send(dart_sock s, const void *buf, size_t len,
                    const uint8_t ip[4], uint16_t port);
/* recvfrom: returns bytes (>0), 0 or <0 if none. src_ip/src_port out, may be NULL. */
int  dart_plat_recv(dart_sock s, void *buf, size_t cap,
                    uint8_t src_ip[4], uint16_t *src_port);
int  dart_plat_would_block(void);
/* poll up to n fds for timeout_ms; >0 ready, 0 timeout, <0 error. */
int  dart_plat_poll(dart_pollfd *fds, int n, int timeout_ms);

/* --- address helpers (uint32_t naddr is network byte order) --- */
uint32_t dart_plat_parse_ip(const char *dotted);          /* "1.2.3.4" -> naddr */
uint32_t dart_plat_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
uint32_t dart_plat_ip4_to_naddr(const uint8_t ip[4]);
void     dart_plat_naddr_to_ip4(uint32_t naddr, uint8_t out[4]);
/* Source address the OS would use to reach dst_naddr:port (connect + getsockname
 * on an unbound UDP socket; no packet leaves). 0 on failure. Backs interface
 * pinning and the same-host check. */
uint32_t dart_plat_route_src(uint32_t dst_naddr, uint16_t port);

/* --- SHM platform surface (DESIGN ONLY; defined when dart_shm lands) ---------
 * The zero-copy same-host path (src/dart_shm.h) needs three more primitives,
 * named here so the platform contract is whole. dart_plat.c does NOT define them
 * yet; the SHM implementation adds them behind the same Windows/POSIX split.
 *
 *   shared-memory mapping (shm_open+ftruncate+mmap / CreateFileMapping+MapView):
 *     void *dart_plat_shm_create(const char *name, size_t bytes, void **handle);
 *     void *dart_plat_shm_attach(const char *name, size_t bytes, void **handle);
 *     void  dart_plat_shm_detach(void *base, size_t bytes, void *handle, int unlink_it);
 *   cross-process atomics on the chunk refcount (C11 stdatomic / Interlocked*):
 *     int32_t dart_plat_atomic_add (volatile int32_t *p, int32_t delta);
 *     int32_t dart_plat_atomic_load(volatile int32_t *p);
 *   a stable per-kernel id for the same-host check (boot id / machine GUID):
 *     void dart_plat_host_uuid(uint8_t out[16]);
 */

#ifdef __cplusplus
}
#endif
#endif /* DART_PLAT_H */
