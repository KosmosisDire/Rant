/* The platform layer of the web build: src/platform/core.h over the net object in net.js.
 * Sockets are ids the net object hands out, datagrams it receives queue here until the node
 * polls, and a poll never blocks: it tells the driver how long the node may sleep. */

#include "rant.h"
#include "rant_web.h"

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>     /* emscripten_date_now */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The imports net.js provides. Addresses are naddrs, network byte order. */
extern int  rant_net_open(void);
extern int  rant_net_bind(int id, uint32_t if_naddr, int port, int reuse);
extern void rant_net_option(int id, int what, int value);
extern int  rant_net_membership(int id, uint32_t group, uint32_t if_naddr, int join);
extern int  rant_net_send(int id, const void *buf, int len, uint32_t naddr, int port);
extern void rant_net_close(int id);
extern int  rant_net_interfaces(uint32_t *out, int max);
extern int  rant_net_hostname(char *buf, int cap);
extern void rant_net_wait(int ms);

enum { NET_SETIF = 1, NET_TTL = 2, NET_LOOP = 3, NET_OWN_GROUPS = 4 };

typedef struct Packet {
    struct Packet *next;
    uint8_t       *data;      /* malloc'd by net.js, owned here */
    size_t         len;
    uint32_t       src;
    uint16_t       port;
} Packet;

typedef struct {
    int      id;              /* 0 = a free slot */
    uint16_t port;
    Packet  *head, *tail;
    size_t   queued, cap;     /* bytes waiting, and the most that may */
} Socket;

#define MAX_SOCKETS 64
#define DEFAULT_RCVBUF (4u << 20)

static Socket sockets[MAX_SOCKETS];
static int    would_block;

static Socket *socket_of(i_RantSock s){
    int i;
    for (i = 0; i < MAX_SOCKETS; i++) if (sockets[i].id && sockets[i].id == (int)s) return &sockets[i];
    return NULL;
}

static void drop_queue(Socket *k){
    while (k->head){
        Packet *p = k->head;
        k->head = p->next;
        free(p->data);
        free(p);
    }
    k->tail = NULL;
    k->queued = 0;
}

/* A datagram for socket id from net.js, which hands over data. Past the socket's buffer it is
 * dropped, as the OS would. */
EMSCRIPTEN_KEEPALIVE void rant_web_deliver(int id, uint32_t src, int port, uint8_t *data, int len){
    Socket *k = socket_of((i_RantSock)id);
    Packet *p;
    if (!k || len < 0 || k->queued + (size_t)len > k->cap || !(p = (Packet*)malloc(sizeof *p))){
        free(data);
        return;
    }
    p->next = NULL; p->data = data; p->len = (size_t)len; p->src = src; p->port = (uint16_t)port;
    if (k->tail) k->tail->next = p; else k->head = p;
    k->tail = p;
    k->queued += (size_t)len;
}

int  i_rant_plat_startup(void){ return 1; }
void i_rant_plat_cleanup(void){}

uint64_t i_rant_plat_now_us(void){ return (uint64_t)(emscripten_get_now() * 1000.0); }
uint64_t i_rant_plat_wall_us(void){ return (uint64_t)(emscripten_date_now() * 1000.0); }

int i_rant_plat_random(void *buf, size_t len){
    uint8_t *p = (uint8_t*)buf;
    while (len){
        size_t n = len > 256 ? 256 : len;     /* getentropy's limit */
        if (getentropy(p, n) != 0) return 0;
        p += n; len -= n;
    }
    return 1;
}

size_t i_rant_plat_hostname(char *buf, size_t cap){
    int n = rant_net_hostname(buf, (int)cap);
    return n > 0 ? (size_t)n : 0;
}

/* A page has no pid, so each instance draws one, which keeps two tabs apart. */
uint64_t i_rant_plat_pid(void){
    static uint32_t pid;
    while (!pid) if (!i_rant_plat_random(&pid, sizeof pid)) pid = 1;
    return pid & 0x7fffffffu;
}

size_t i_rant_plat_env(const char *name, char *buf, size_t cap){
    (void)name;
    if (cap) buf[0] = 0;
    return 0;
}

void *i_rant_plat_realloc(void *ptr, size_t size){
    if (!size){ free(ptr); return NULL; }
    return realloc(ptr, size);
}

i_RantSock i_rant_plat_udp_open(void){
    int i, id;
    for (i = 0; i < MAX_SOCKETS && sockets[i].id; i++) {}
    if (i == MAX_SOCKETS) return RANT_SOCK_BAD;
    id = rant_net_open();
    if (id <= 0) return RANT_SOCK_BAD;
    memset(&sockets[i], 0, sizeof sockets[i]);
    sockets[i].id = id;
    sockets[i].cap = DEFAULT_RCVBUF;
    return (i_RantSock)id;
}

void i_rant_plat_close(i_RantSock s){
    Socket *k = socket_of(s);
    if (!k) return;
    rant_net_close(k->id);
    drop_queue(k);
    k->id = 0;
}

int i_rant_plat_bind(i_RantSock s, uint32_t if_naddr, uint16_t port, int reuse){
    Socket *k = socket_of(s);
    int got;
    if (!k) return 0;
    got = rant_net_bind(k->id, if_naddr, port, reuse);
    if (got <= 0) return 0;
    k->port = (uint16_t)got;
    return 1;
}

uint16_t i_rant_plat_local_port(i_RantSock s){
    Socket *k = socket_of(s);
    return k ? k->port : 0;
}

void i_rant_plat_set_nonblock(i_RantSock s){ (void)s; }
void i_rant_plat_set_sndbuf(i_RantSock s, int bytes){ (void)s; (void)bytes; }
void i_rant_plat_suppress_connreset(i_RantSock s){ (void)s; }

void i_rant_plat_set_rcvbuf(i_RantSock s, int bytes){
    Socket *k = socket_of(s);
    if (k && bytes > 0) k->cap = (size_t)bytes;
}

void i_rant_plat_mcast_setif(i_RantSock s, uint32_t if_naddr){ rant_net_option((int)s, NET_SETIF, (int)if_naddr); }
void i_rant_plat_mcast_ttl(i_RantSock s, uint8_t ttl){ rant_net_option((int)s, NET_TTL, ttl); }
void i_rant_plat_mcast_loop(i_RantSock s, int on){ rant_net_option((int)s, NET_LOOP, on); }
void i_rant_plat_mcast_own_groups(i_RantSock s){ rant_net_option((int)s, NET_OWN_GROUPS, 1); }

int i_rant_plat_mcast_join(i_RantSock s, uint32_t group_naddr, uint32_t if_naddr){
    return rant_net_membership((int)s, group_naddr, if_naddr, 1);
}

void i_rant_plat_mcast_leave(i_RantSock s, uint32_t group_naddr, uint32_t if_naddr){
    (void)rant_net_membership((int)s, group_naddr, if_naddr, 0);
}

int i_rant_plat_send(i_RantSock s, const void *buf, size_t len, const uint8_t ip[4], uint16_t port){
    int n = rant_net_send((int)s, buf, (int)len, i_rant_plat_ip4_to_naddr(ip), port);
    would_block = n < 0;
    return n;
}

int i_rant_plat_recv(i_RantSock s, void *buf, size_t cap, uint8_t src_ip[4], uint16_t *src_port){
    Socket *k = socket_of(s);
    Packet *p;
    size_t n;
    if (!k || !k->head){ would_block = 1; return -1; }
    p = k->head;
    k->head = p->next;
    if (!k->head) k->tail = NULL;
    k->queued -= p->len;
    n = p->len < cap ? p->len : cap;      /* the prefix, as the OS layer delivers */
    memcpy(buf, p->data, n);
    if (src_ip) i_rant_plat_naddr_to_ip4(p->src, src_ip);
    if (src_port) *src_port = p->port;
    free(p->data);
    free(p);
    would_block = 0;
    return (int)n;
}

int i_rant_plat_would_block(void){ return would_block; }

/* Never blocks: readiness is what is queued, and the wait goes to the driver as the longest
 * the node may sleep before its next poll. */
int i_rant_plat_poll(i_RantPollfd *fds, int n, int timeout_ms){
    int i, ready = 0;
    for (i = 0; i < n; i++){
        Socket *k = socket_of(fds[i].fd);
        fds[i].revents = (k && k->head && (fds[i].events & RANT_POLLIN)) ? RANT_POLLIN : 0;
        if (fds[i].revents) ready++;
    }
    if (!ready) rant_net_wait(timeout_ms < 0 ? 0 : timeout_ms);
    else rant_net_wait(0);
    return ready;
}

int i_rant_plat_last_socket_error(void){ return 0; }

uint32_t i_rant_plat_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d){
    uint8_t ip[4]; ip[0] = a; ip[1] = b; ip[2] = c; ip[3] = d;
    return i_rant_plat_ip4_to_naddr(ip);
}

uint32_t i_rant_plat_ip4_to_naddr(const uint8_t ip[4]){
    uint32_t v;
    memcpy(&v, ip, 4);         /* network order is the bytes as they stand */
    return v;
}

void i_rant_plat_naddr_to_ip4(uint32_t naddr, uint8_t out[4]){ memcpy(out, &naddr, 4); }

/* "1.2.3.4" to its naddr, 0xFFFFFFFF for anything else, as inet_addr answers. NULL is 0. */
uint32_t i_rant_plat_parse_ip(const char *dotted){
    uint8_t ip[4];
    int part = 0, digits = 0;
    unsigned v = 0;
    if (!dotted) return 0;
    for (;; dotted++){
        char c = *dotted;
        if (c >= '0' && c <= '9'){
            v = v * 10u + (unsigned)(c - '0');
            if (v > 255u || ++digits > 3) return 0xFFFFFFFFu;
        } else if ((c == '.' || c == 0) && digits){
            if (part > 3) return 0xFFFFFFFFu;
            ip[part++] = (uint8_t)v;
            v = 0; digits = 0;
            if (c == 0) break;
        } else return 0xFFFFFFFFu;
    }
    return part == 4 ? i_rant_plat_ip4_to_naddr(ip) : 0xFFFFFFFFu;
}

int i_rant_plat_is_loopback(uint32_t naddr){
    uint8_t ip[4];
    i_rant_plat_naddr_to_ip4(naddr, ip);
    return ip[0] == 127;
}

uint32_t i_rant_plat_loopback_group_bind(uint32_t loopback_naddr){ return loopback_naddr; }
uint32_t i_rant_plat_route_src(uint32_t dst_naddr, uint16_t port){ (void)dst_naddr; (void)port; return 0; }

/* The interfaces of the host whose sockets these are: the bridge's, or this machine's. */
int i_rant_plat_local_ifaces(i_RantIface *out, int max){
    uint32_t pairs[2 * 16];
    int n, i;
    if (max > 16) max = 16;
    n = rant_net_interfaces(pairs, max);
    for (i = 0; i < n; i++){ out[i].addr = pairs[2 * i]; out[i].mask = pairs[2 * i + 1]; }
    return n;
}
