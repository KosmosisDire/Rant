/* The wire tap behind wire_tap.h. Two clock reads per call are cheap enough to keep on. */
#include "wire_tap.h"
#include "atomic.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
  #include <errno.h>
  #include <arpa/inet.h>
  #include <netinet/in.h>
#endif

WireTap wire_tap;
void (*wire_tap_on_recv)(long long sock, const unsigned char *buf, long len,
                         const struct sockaddr *from);

long long wire_tap_read(const long long *field){ return test_atomic_load(field); }
void wire_tap_write(long long *field, long long value){ test_atomic_store(field, value); }

/* WireTap is all long long, so its fields walk as an array. */
#define TAP_FIELDS (sizeof(WireTap) / sizeof(long long))

void wire_tap_snapshot(WireTap *out){
    long long *src = (long long *)&wire_tap, *dst = (long long *)out;
    size_t i;
    for (i = 0; i < TAP_FIELDS; i++) dst[i] = test_atomic_load(&src[i]);
}

void wire_tap_reset_counters(void){
    long long *p = (long long *)&wire_tap, *end = &wire_tap.trace_left;
    for (; p < end; p++) test_atomic_store(p, 0);
}

static long long tap_now_ns(void){
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (long long)((c.QuadPart / f.QuadPart) * 1000000000ll
                       + (c.QuadPart % f.QuadPart) * 1000000000ll / f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000ll + ts.tv_nsec;
#endif
}

/* A datagram may carry several concatenated transport submessages: count each. Discovery and
 * detail datagrams start with a 'u', so they count as other. */
static void tap_classify(const unsigned char *b, long len, long long *types, long long *data_topic){
    long off = 0;
    int counted = 0;
    while (len - off >= 3){
        unsigned b0 = b[off], t = b0 & WIRE_TAP_MSG_MASK;
        long sub;
        if (t == WIRE_TAP_DATA){
            if (b0 & WIRE_TAP_F_SHM) sub = WIRE_TAP_SHM_DATA_BYTES;
            else if (b0 & WIRE_TAP_F_SINGLE){
                if (len - off < WIRE_TAP_HEADER_DATA_SINGLE) break;
                sub = WIRE_TAP_HEADER_DATA_SINGLE + (b[off + WIRE_TAP_OFFSET_LEN_SINGLE]
                      | ((unsigned)b[off + WIRE_TAP_OFFSET_LEN_SINGLE + 1] << 8));
            } else {
                if (len - off < WIRE_TAP_HEADER_DATA_MULTI) break;
                sub = WIRE_TAP_HEADER_DATA_MULTI + (b[off + WIRE_TAP_OFFSET_LEN_MULTI]
                      | ((unsigned)b[off + WIRE_TAP_OFFSET_LEN_MULTI + 1] << 8));
            }
        }
        else if (t == WIRE_TAP_HB) sub = WIRE_TAP_HEADER_HB;
        else if (t == WIRE_TAP_NACK) sub = WIRE_TAP_HEADER_NACK;
        else break;
        if (sub > len - off) break;
        test_atomic_add(&types[t], 1);
        counted = 1;
        if (t == WIRE_TAP_DATA)
            test_atomic_add(&data_topic[(b[off + WIRE_TAP_OFFSET_INDEX]
                                 | ((unsigned)b[off + WIRE_TAP_OFFSET_INDEX + 1] << 8)) & 3], 1);
        off += sub;
    }
    if (!counted) test_atomic_add(&types[0], 1);
}

static int tap_is_detail(const unsigned char *b, long len, unsigned kind){
    return len >= 5 && b[0] == 'u' && b[1] == 'D' && b[2] == 'T' && b[3] == 'L' && b[4] == kind;
}

/* 1 when the fault switches eat this datagram. would_block says how: a transport datagram
 * fails as would block, a swallowed one reports success. */
static int tap_fault(const unsigned char *b, long len, const struct sockaddr *to, int *would_block){
    unsigned type = len >= 1 ? (b[0] & WIRE_TAP_MSG_MASK) : 0;
    long long port = test_atomic_load(&wire_tap.drop_detail_resp_port);
    if (test_atomic_load(&wire_tap.isolate)){
        *would_block = 0;
        return 1;
    }
    if (test_atomic_load(&wire_tap.block_data) && type >= WIRE_TAP_DATA && type <= WIRE_TAP_NACK){
        *would_block = 1;
        return 1;
    }
    if (port && len >= 6 && tap_is_detail(b, len, 2) && to && to->sa_family == AF_INET
        && ntohs(((const struct sockaddr_in *)to)->sin_port) == (unsigned short)port){
        *would_block = 0;
        return 1;
    }
    return 0;
}

/* A socket bound to any or to a network address listens on the network, which is what makes
   the Windows firewall ask. Loopback binds never do */
static void tap_bound(const struct sockaddr *addr){
    const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
    if (!addr || addr->sa_family != AF_INET) return;
    if (((const unsigned char *)&in->sin_addr)[0] == 127) return;
    test_atomic_add(&wire_tap.bind_off_loopback, 1);
    test_atomic_store(&wire_tap.bind_off_loopback_port, ntohs(in->sin_port));
}

static void tap_sent(const unsigned char *b, long r, long long dt){
    long long max;
    test_atomic_add(&wire_tap.tx_ns, dt);
    test_atomic_add(&wire_tap.tx_calls, 1);
    if (r < 0) return;
    tap_classify(b, r, wire_tap.tx_type, wire_tap.tx_data_topic);
    max = test_atomic_load(&wire_tap.tx_max_len);
    while (r > max && !test_atomic_cas(&wire_tap.tx_max_len, max, r))
        max = test_atomic_load(&wire_tap.tx_max_len);
    if (tap_is_detail(b, r, 3)) test_atomic_add(&wire_tap.tx_interest_req, 1);
}

static void tap_received(long long sock, const unsigned char *b, long r, const struct sockaddr *from,
                         long long dt){
    test_atomic_add(&wire_tap.rx_ns, dt);
    test_atomic_add(&wire_tap.rx_calls, 1);
    if (r < 0) return;
    if (wire_tap_on_recv) wire_tap_on_recv(sock, b, r, from);
    tap_classify(b, r, wire_tap.rx_type, wire_tap.rx_data_topic);
    if (r >= 1 && (b[0] & WIRE_TAP_MSG_MASK) >= WIRE_TAP_DATA && (b[0] & WIRE_TAP_MSG_MASK) <= WIRE_TAP_NACK
        && from && from->sa_family == AF_INET
        && test_atomic_load(&wire_tap.trace_left) > 0 && test_atomic_add(&wire_tap.trace_left, -1) > 0){
        const struct sockaddr_in *si = (const struct sockaddr_in *)from;
        const unsigned char *ip = (const unsigned char *)&si->sin_addr;
        printf("TRACE rx src=%u.%u.%u.%u:%u len=%ld type=%u\n", ip[0], ip[1], ip[2], ip[3],
               ntohs(si->sin_port), r, b[0]);
    }
}

#ifdef _WIN32

static void tap_error(int e, long long *would_block, long long *reset, long long *error){
    test_atomic_add(e == WSAEWOULDBLOCK ? would_block : e == WSAECONNRESET ? reset : error, 1);
}

int wire_tap_bind(SOCKET s, const struct sockaddr *addr, int len){
    int r = bind(s, addr, len);
    int e = WSAGetLastError();
    if (r == 0) tap_bound(addr);
    WSASetLastError(e);
    return r;
}

int wire_tap_setsockopt(SOCKET s, int level, int name, const char *value, int len){
    if (level == IPPROTO_IP && name == IP_ADD_MEMBERSHIP && test_atomic_load(&wire_tap.fail_join)){
        WSASetLastError(WSAEADDRNOTAVAIL);
        return -1;
    }
    return setsockopt(s, level, name, value, len);
}

int wire_tap_sendto(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen){
    long long a;
    int r, would_block, e;
    if (tap_fault((const unsigned char *)buf, len, to, &would_block)){
        test_atomic_add(&wire_tap.tx_calls, 1);
        if (!would_block) return len;
        test_atomic_add(&wire_tap.tx_would_block, 1);
        WSASetLastError(WSAEWOULDBLOCK);
        return -1;
    }
    a = tap_now_ns();
    r = sendto(s, buf, len, flags, to, tolen);
    e = WSAGetLastError();
    if (r < 0) tap_error(e, &wire_tap.tx_would_block, &wire_tap.tx_reset, &wire_tap.tx_error);
    tap_sent((const unsigned char *)buf, r, tap_now_ns() - a);
    WSASetLastError(e);
    return r;
}

int wire_tap_recvfrom(SOCKET s, char *buf, int len, int flags, struct sockaddr *from, int *fromlen){
    long long a;
    int r, e;
    if (test_atomic_load(&wire_tap.isolate)){
        WSASetLastError(WSAEWOULDBLOCK);
        return -1;
    }
    a = tap_now_ns();
    r = recvfrom(s, buf, len, flags, from, fromlen);
    e = WSAGetLastError();
    if (r < 0) tap_error(e, &wire_tap.rx_would_block, &wire_tap.rx_reset, &wire_tap.rx_error);
    tap_received((long long)s, (const unsigned char *)buf, r, from, tap_now_ns() - a);
    WSASetLastError(e);
    return r;
}

#else

static void tap_error(int e, long long *would_block, long long *reset, long long *error){
    test_atomic_add(e == EAGAIN || e == EWOULDBLOCK ? would_block : e == ECONNREFUSED ? reset : error, 1);
}

int wire_tap_bind(int s, const struct sockaddr *addr, socklen_t len){
    int r = bind(s, addr, len);
    int e = errno;
    if (r == 0) tap_bound(addr);
    errno = e;
    return r;
}

int wire_tap_setsockopt(int s, int level, int name, const void *value, socklen_t len){
    if (level == IPPROTO_IP && name == IP_ADD_MEMBERSHIP && test_atomic_load(&wire_tap.fail_join)){
        errno = EADDRNOTAVAIL;
        return -1;
    }
    return setsockopt(s, level, name, value, len);
}

ssize_t wire_tap_sendto(int s, const void *buf, size_t len, int flags, const struct sockaddr *to,
                        socklen_t tolen){
    long long a;
    ssize_t r;
    int would_block, e;
    if (tap_fault((const unsigned char *)buf, (long)len, to, &would_block)){
        test_atomic_add(&wire_tap.tx_calls, 1);
        if (!would_block) return (ssize_t)len;
        test_atomic_add(&wire_tap.tx_would_block, 1);
        errno = EAGAIN;
        return -1;
    }
    a = tap_now_ns();
    r = sendto(s, buf, len, flags, to, tolen);
    e = errno;
    if (r < 0) tap_error(e, &wire_tap.tx_would_block, &wire_tap.tx_reset, &wire_tap.tx_error);
    tap_sent((const unsigned char *)buf, (long)r, tap_now_ns() - a);
    errno = e;
    return r;
}

ssize_t wire_tap_recvfrom(int s, void *buf, size_t len, int flags, struct sockaddr *from,
                          socklen_t *fromlen){
    long long a;
    ssize_t r;
    int e;
    if (test_atomic_load(&wire_tap.isolate)){
        errno = EAGAIN;
        return -1;
    }
    a = tap_now_ns();
    r = recvfrom(s, buf, len, flags, from, fromlen);
    e = errno;
    if (r < 0) tap_error(e, &wire_tap.rx_would_block, &wire_tap.rx_reset, &wire_tap.rx_error);
    tap_received((long long)s, (const unsigned char *)buf, (long)r, from, tap_now_ns() - a);
    errno = e;
    return r;
}

#endif
