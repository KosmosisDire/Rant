/* The wire tap wraps the platform's sendto, recvfrom, bind and setsockopt: it counts datagrams
 * by type, injects faults, isolates a node and notes binds outside loopback. rant_impl.h
 * routes them. */
#ifndef RANT_WIRE_TAP_H
#define RANT_WIRE_TAP_H

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
#else
  #include <sys/types.h>
  #include <sys/socket.h>
#endif

/* The transport submessage framing, mirrored from transport/internal.h because the tap sits
 * outside the implementation. unit/transport compares them, so a wire change fails loudly. */
#define WIRE_TAP_DATA                 1
#define WIRE_TAP_HB                   2
#define WIRE_TAP_NACK                 3
#define WIRE_TAP_MSG_MASK             0x07u
#define WIRE_TAP_F_SINGLE             0x08u
#define WIRE_TAP_F_SHM                0x20u
#define WIRE_TAP_OFFSET_INDEX         1
#define WIRE_TAP_OFFSET_LEN_SINGLE    11
#define WIRE_TAP_OFFSET_LEN_MULTI     19
#define WIRE_TAP_HEADER_DATA_SINGLE   13
#define WIRE_TAP_HEADER_DATA_MULTI    21
#define WIRE_TAP_HEADER_HB            23
#define WIRE_TAP_HEADER_NACK          21
#define WIRE_TAP_SHM_DATA_BYTES       37

/* Every counter is updated atomically, since service threads send and receive too. Read them
 * with wire_tap_read once the traffic of interest is over. */
typedef struct {
    long long tx_calls, tx_would_block, tx_reset, tx_error, tx_ns;
    long long rx_calls, rx_would_block, rx_reset, rx_error, rx_ns;
    long long tx_type[4], rx_type[4];               /* by WIRE_TAP_DATA, HB and NACK, 0 = other */
    long long tx_data_topic[4], rx_data_topic[4];   /* DATA submessages by topic index & 3 */
    long long tx_max_len;                           /* the largest datagram sent */
    long long tx_interest_req;                      /* interest page requests sent */
    long long trace_left;                           /* received datagrams still to print */
    long long block_data;                           /* 1 = transport datagrams would block */
    long long isolate;                              /* 1 = sends vanish, receives find nothing */
    long long drop_detail_resp_port;                /* swallow DETAIL_RESP to this port, 0 = off */
    long long fail_join;                            /* 1 = every multicast join fails */
    long long bind_off_loopback;                    /* sockets bound to a network address */
    long long bind_off_loopback_port;               /* the last such socket's port */
} WireTap;

extern WireTap wire_tap;

long long wire_tap_read(const long long *field);
void      wire_tap_write(long long *field, long long value);
void      wire_tap_snapshot(WireTap *out);
/* Zeroes the datagram counters and keeps the switches and the bind record. */
void      wire_tap_reset_counters(void);
/* Sees every datagram a socket receives, NULL = off. Set it while no node runs */
extern void (*wire_tap_on_recv)(long long sock, const unsigned char *buf, long len,
                                const struct sockaddr *from);

#ifdef _WIN32
int wire_tap_bind(SOCKET s, const struct sockaddr *addr, int len);
int wire_tap_setsockopt(SOCKET s, int level, int name, const char *value, int len);
int wire_tap_sendto(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen);
int wire_tap_recvfrom(SOCKET s, char *buf, int len, int flags, struct sockaddr *from, int *fromlen);
#else
int     wire_tap_bind(int s, const struct sockaddr *addr, socklen_t len);
int     wire_tap_setsockopt(int s, int level, int name, const void *value, socklen_t len);
ssize_t wire_tap_sendto(int s, const void *buf, size_t len, int flags, const struct sockaddr *to,
                        socklen_t tolen);
ssize_t wire_tap_recvfrom(int s, void *buf, size_t len, int flags, struct sockaddr *from,
                          socklen_t *fromlen);
#endif

#endif /* RANT_WIRE_TAP_H */

