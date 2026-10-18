/* The sockets a runtime uses: the OS's own, or those a bridge lends a node through a tunnel
 * (spec/tunnel.md). Every i_rant_net function takes the node's tunnel, NULL for the OS. */
#ifndef RANT_TUNNEL_RT_H
#define RANT_TUNNEL_RT_H

#include "../platform/core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct i_RantTunnel i_RantTunnel;

#ifdef RANT_TUNNEL
/* Connects to the bridge at url, a ws:// URL, and opens a session named name. NULL with
 * why filled when it cannot be reached or refuses. */
i_RantTunnel *i_rant_tunnel_open(const char *url, const char *passphrase, const char *name,
                                 char *why, size_t why_cap);
/* Sends what is queued, within a bounded wait, then ends the session. Its sockets close first. */
void          i_rant_tunnel_close(i_RantTunnel *t);
/* 1 with one line in why when the link dropped or a socket op failed since the last call. */
int           i_rant_tunnel_note(i_RantTunnel *t, char *why, size_t why_cap);
#endif /* RANT_TUNNEL */

i_RantSock i_rant_net_udp_open(i_RantTunnel *t);
void       i_rant_net_close(i_RantTunnel *t, i_RantSock s);
int        i_rant_net_bind(i_RantTunnel *t, i_RantSock s, uint32_t if_naddr, uint16_t port, int reuse);
uint16_t   i_rant_net_local_port(i_RantTunnel *t, i_RantSock s);
void       i_rant_net_set_nonblock(i_RantTunnel *t, i_RantSock s);
void       i_rant_net_set_rcvbuf(i_RantTunnel *t, i_RantSock s, int bytes);
void       i_rant_net_set_sndbuf(i_RantTunnel *t, i_RantSock s, int bytes);
void       i_rant_net_suppress_connreset(i_RantTunnel *t, i_RantSock s);
void       i_rant_net_mcast_setif(i_RantTunnel *t, i_RantSock s, uint32_t if_naddr);
void       i_rant_net_mcast_ttl(i_RantTunnel *t, i_RantSock s, uint8_t ttl);
void       i_rant_net_mcast_loop(i_RantTunnel *t, i_RantSock s, int on);
int        i_rant_net_mcast_join(i_RantTunnel *t, i_RantSock s, uint32_t group_naddr, uint32_t if_naddr);
void       i_rant_net_mcast_leave(i_RantTunnel *t, i_RantSock s, uint32_t group_naddr, uint32_t if_naddr);
int        i_rant_net_send(i_RantTunnel *t, i_RantSock s, const void *buf, size_t len,
                           const uint8_t ip[4], uint16_t port);
int        i_rant_net_recv(i_RantTunnel *t, i_RantSock s, void *buf, size_t cap,
                           uint8_t src_ip[4], uint16_t *src_port);
/* A tunnel's send and recv fail only for a full backlog or an empty queue. */
int        i_rant_net_would_block(i_RantTunnel *t);
int        i_rant_net_last_error(i_RantTunnel *t);
/* fds may mix a tunnel's sockets with the OS's, such as the node's waker. */
int        i_rant_net_poll(i_RantTunnel *t, i_RantPollfd *fds, int n, int timeout_ms);
/* What sends left batched goes to the bridge. A no op for the OS. */
void       i_rant_net_flush(i_RantTunnel *t);
/* The interfaces of the host whose sockets these are. */
int        i_rant_net_local_ifaces(i_RantTunnel *t, i_RantIface *out, int max);
uint32_t   i_rant_net_loopback_group_bind(i_RantTunnel *t, uint32_t loopback_naddr);

#ifdef __cplusplus
}
#endif
#endif /* RANT_TUNNEL_RT_H */
