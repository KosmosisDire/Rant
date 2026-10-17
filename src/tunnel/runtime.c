/* The sockets a runtime uses, and the tunnel client behind them: a node's sockets lent by a
 * bridge over one WebSocket (spec/tunnel.md). A tunnel's socket is a handle below
 * RANT_SOCK_BAD, so one poll set mixes them with the OS's. The link reconnects on its own,
 * one step per poll, and never blocks a poll except for the OS's name lookup. */

#include "runtime.h"
#include "core.h"
#include "../common/bytes.h"
#include <string.h>
#include <stdio.h>

#ifdef RANT_TUNNEL

#define I_RANT_TUNNEL_SOCKS      16           /* the bridge's limit per session */
#define I_RANT_TUNNEL_GROUPS     8            /* the groups a socket keeps to join again */
#define I_RANT_TUNNEL_QUEUE      (1u << 20)   /* what a socket holds received, as a receive buffer */
#define I_RANT_TUNNEL_BACKLOG    (4u << 20)   /* bytes waiting for TCP before a send would block */
#define I_RANT_TUNNEL_STEP_US    5000000u     /* connect, handshake and open each get this long */
#define I_RANT_TUNNEL_SILENT_US  6000000u     /* the bridge pings, so silence this long is a loss */
#define I_RANT_TUNNEL_MSG_MAX    (2u << 20)   /* a message from the bridge past this is a fault */

#define I_RANT_TUNNEL_HANDLE(slot) ((i_RantSock)(-2 - (intptr_t)(slot)))
#define I_RANT_TUNNEL_SLOT(s)      ((int)(-2 - (intptr_t)(s)))

typedef struct {
    uint8_t  used, bound;
    uint16_t sock;              /* the bridge's number for it */
    uint16_t port;
    uint32_t if_naddr;
    uint8_t  reuse;
    uint32_t via;               /* the interface group datagrams leave by, sent with each */
    int16_t  ttl;               /* options to set again on a new session, -1 = never set */
    int8_t   loop, own_groups;
    struct { uint32_t group, if_naddr; } groups[I_RANT_TUNNEL_GROUPS];
    uint8_t  n_groups;
    /* received: [u16 len][4 ip][u16 port][payload] from q_head to q_tail */
    uint8_t *q; size_t q_cap, q_head, q_tail, q_max;
} i_RantTunnelSock;

enum { I_RANT_LINK_DOWN, I_RANT_LINK_CONNECTING, I_RANT_LINK_HANDSHAKE, I_RANT_LINK_OPENING,
       I_RANT_LINK_OPEN, I_RANT_LINK_REFUSED };

struct i_RantTunnel {
    i_RantWsUrl      url;
    char             passphrase[256];
    char             name[128];
    char             token[128];        /* takes the session back after a drop */
    int              state;
    i_RantSock       tcp;
    uint8_t          key[16];           /* the handshake's */
    uint32_t         open_seq;
    uint64_t         deadline_us;       /* for the step under way */
    uint64_t         heard_us;
    uint64_t         retry_us;
    uint32_t         backoff_ms;
    uint64_t         mask;              /* xorshift state for frame masks */
    uint8_t         *rx;  size_t rx_len, rx_cap;     /* read, not yet whole frames */
    uint8_t         *msg; size_t msg_len, msg_cap;   /* a fragmented message being joined */
    int              msg_op;
    uint8_t         *tx;  size_t tx_len, tx_cap;     /* for the socket */
    uint8_t          batch[I_RANT_TUNNEL_BATCH];       /* datagrams sent since the last flush */
    size_t           batch_len;
    uint32_t         seq;
    uint32_t         want_seq;          /* the request a caller waits on */
    char            *reply; size_t reply_cap; int got;
    i_RantIface      ifaces[16];
    int              n_ifaces;
    int              group_binds_any;
    i_RantTunnelSock socks[I_RANT_TUNNEL_SOCKS];
    uint16_t         next_sock;
    char             note[192];
    int              noted;
    char             why[160];          /* the last drop's reason */
    uint32_t         drops;
    int              closing;           /* the node closes: no reconnect, no notes */
#ifdef RANT_THREADS
    i_RantMutex      mu;
#endif
};

static void i_rant_tunnel_lock(i_RantTunnel *t){
#ifdef RANT_THREADS
    i_rant_plat_mutex_lock(&t->mu);
#else
    (void)t;
#endif
}
static void i_rant_tunnel_unlock(i_RantTunnel *t){
#ifdef RANT_THREADS
    i_rant_plat_mutex_unlock(&t->mu);
#else
    (void)t;
#endif
}

static int i_rant_tunnel_reserve(uint8_t **buf, size_t *cap, size_t need){
    size_t c = *cap ? *cap : 4096; uint8_t *p;
    if (need <= *cap) return 1;
    while (c < need) c *= 2;
    p = (uint8_t*)i_rant_plat_realloc(*buf, c);
    if (!p) return 0;
    *buf = p; *cap = c;
    return 1;
}

static void i_rant_tunnel_note_set(i_RantTunnel *t, const char *what, const char *why){
    if (t->noted) return;        /* the first stays until the node takes it */
    snprintf(t->note, sizeof t->note, "%s%s%s", what, why && *why ? ": " : "", why ? why : "");
    t->noted = 1;
}

static void i_rant_tunnel_dotted(uint32_t naddr, char out[16]){
    uint8_t b[4]; memcpy(b, &naddr, 4);
    snprintf(out, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

/* ---- frames out ---- */

static void i_rant_tunnel_mask(i_RantTunnel *t, uint8_t out[4]){
    uint64_t x = t->mask;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    t->mask = x;
    memcpy(out, &x, 4);
}

/* One frame of a then b into tx, masked. 0 when it cannot be held. */
static int i_rant_tunnel_frame(i_RantTunnel *t, int op, const void *a, size_t al, const void *b, size_t bl){
    uint8_t head[I_RANT_WS_HEAD_MAX], mask[4]; size_t hl;
    i_rant_tunnel_mask(t, mask);
    hl = i_rant_ws_head(head, op, al + bl, mask);
    if (!i_rant_tunnel_reserve(&t->tx, &t->tx_cap, t->tx_len + hl + al + bl)) return 0;
    memcpy(t->tx + t->tx_len, head, hl); t->tx_len += hl;
    if (al){ memcpy(t->tx + t->tx_len, a, al); i_rant_ws_mask(t->tx + t->tx_len, al, mask, 0); t->tx_len += al; }
    if (bl){ memcpy(t->tx + t->tx_len, b, bl); i_rant_ws_mask(t->tx + t->tx_len, bl, mask, al); t->tx_len += bl; }
    return 1;
}

/* A request, body being the JSON members after op and seq. Returns its seq. */
static uint32_t i_rant_tunnel_ask(i_RantTunnel *t, const char *body){
    char text[1024]; int n;
    uint32_t seq = ++t->seq;
    n = snprintf(text, sizeof text, "{\"seq\":%u,%s}", (unsigned)seq, body);
    if (n > 0 && (size_t)n < sizeof text) i_rant_tunnel_frame(t, I_RANT_WS_TEXT, text, (size_t)n, NULL, 0);
    return seq;
}

static void i_rant_tunnel_batch_out(i_RantTunnel *t){
    if (t->batch_len > 2) i_rant_tunnel_frame(t, I_RANT_WS_BINARY, t->batch, t->batch_len, NULL, 0);
    t->batch_len = 0;
}

/* ---- the link ---- */

static void i_rant_tunnel_drop(i_RantTunnel *t, const char *why, int refused){
    int was_open = t->state == I_RANT_LINK_OPEN;
    snprintf(t->why, sizeof t->why, "%s", why);
    t->drops++;
    if (t->tcp != RANT_SOCK_BAD) i_rant_plat_close(t->tcp);
    t->tcp = RANT_SOCK_BAD;
    t->rx_len = t->msg_len = t->tx_len = t->batch_len = 0;
    t->got = 0;
    t->state = refused ? I_RANT_LINK_REFUSED : I_RANT_LINK_DOWN;
    t->retry_us = i_rant_plat_now_us() + (uint64_t)t->backoff_ms * 1000u;
    if (t->backoff_ms < 5000u) t->backoff_ms *= 2u;
    if (t->closing) return;
    if (refused) i_rant_tunnel_note_set(t, "the bridge refused the tunnel", why);
    else if (was_open) i_rant_tunnel_note_set(t, "the link to the bridge dropped, reconnecting", why);
}

static void i_rant_tunnel_start(i_RantTunnel *t){
    t->tcp = i_rant_plat_tcp_connect(t->url.host, t->url.port);
    t->deadline_us = i_rant_plat_now_us() + I_RANT_TUNNEL_STEP_US;
    if (t->tcp == RANT_SOCK_BAD){ i_rant_tunnel_drop(t, "cannot resolve or reach the bridge's host", 0); return; }
    t->state = I_RANT_LINK_CONNECTING;
}

static void i_rant_tunnel_send_open(i_RantTunnel *t){
    char body[900], pass[300], name[200], token[160];
    i_rant_json_quote(t->passphrase, pass, sizeof pass);
    i_rant_json_quote(t->name, name, sizeof name);
    if (t->token[0]) i_rant_json_quote(t->token, token, sizeof token);
    snprintf(body, sizeof body, "\"op\":\"open\",\"proto\":%d,\"passphrase\":%s,\"name\":%s%s%s",
             I_RANT_TUNNEL_PROTO, pass, name, t->token[0] ? ",\"resume\":" : "", t->token[0] ? token : "");
    t->open_seq = i_rant_tunnel_ask(t, body);
    t->state = I_RANT_LINK_OPENING;
    t->deadline_us = i_rant_plat_now_us() + I_RANT_TUNNEL_STEP_US;
}

static void i_rant_tunnel_ask_option(i_RantTunnel *t, i_RantTunnelSock *k){
    char body[160]; int n;
    n = snprintf(body, sizeof body, "\"op\":\"udp_option\",\"sock\":%u", (unsigned)k->sock);
    if (k->ttl >= 0) n += snprintf(body + n, sizeof body - (size_t)n, ",\"ttl\":%d", k->ttl);
    if (k->loop >= 0) n += snprintf(body + n, sizeof body - (size_t)n, ",\"loop\":%s", k->loop ? "true" : "false");
    if (k->own_groups > 0) snprintf(body + n, sizeof body - (size_t)n, ",\"own_groups\":true");
    i_rant_tunnel_ask(t, body);
}

static void i_rant_tunnel_ask_member(i_RantTunnel *t, i_RantTunnelSock *k, uint32_t group, uint32_t if_naddr, int join){
    char body[160], g[16], f[16];
    i_rant_tunnel_dotted(group, g); i_rant_tunnel_dotted(if_naddr, f);
    snprintf(body, sizeof body, "\"op\":\"udp_member\",\"sock\":%u,\"group\":\"%s\",\"if\":\"%s\",\"join\":%s",
             (unsigned)k->sock, g, f, join ? "true" : "false");
    i_rant_tunnel_ask(t, body);
}

static void i_rant_tunnel_ask_bind(i_RantTunnel *t, i_RantTunnelSock *k){
    char body[160], f[16];
    i_rant_tunnel_dotted(k->if_naddr, f);
    snprintf(body, sizeof body, "\"op\":\"udp_bind\",\"sock\":%u,\"port\":%u,\"if\":\"%s\",\"reuse\":%s",
             (unsigned)k->sock, (unsigned)k->port, f, k->reuse ? "true" : "false");
    i_rant_tunnel_ask(t, body);
}

/* The bridge lost the session and its sockets: each is bound again to the port its node
 * knows, with its options and groups. */
static void i_rant_tunnel_restore(i_RantTunnel *t){
    int i; uint8_t g;
    for (i = 0; i < I_RANT_TUNNEL_SOCKS; i++){
        i_RantTunnelSock *k = &t->socks[i];
        if (!k->used || !k->bound) continue;
        i_rant_tunnel_ask_bind(t, k);
        if (k->ttl >= 0 || k->loop >= 0 || k->own_groups > 0) i_rant_tunnel_ask_option(t, k);
        for (g = 0; g < k->n_groups; g++) i_rant_tunnel_ask_member(t, k, k->groups[g].group, k->groups[g].if_naddr, 1);
    }
}

/* The open's reply: the session, the host's interfaces, and whether it was resumed. */
static void i_rant_tunnel_opened(i_RantTunnel *t, i_RantJson r){
    i_RantJson list = i_rant_json_get(r, "interfaces"), e; size_t at = 0;
    int resuming = t->token[0] != 0;
    char addr[32], mask[32];
    i_rant_json_str(i_rant_json_get(r, "resume_token"), t->token, sizeof t->token);
    t->n_ifaces = 0;
    while (t->n_ifaces < 16 && i_rant_json_next(list, &at, &e)){
        i_rant_json_str(i_rant_json_get(e, "addr"), addr, sizeof addr);
        i_rant_json_str(i_rant_json_get(e, "mask"), mask, sizeof mask);
        t->ifaces[t->n_ifaces].addr = i_rant_plat_parse_ip(addr);
        t->ifaces[t->n_ifaces].mask = mask[0] ? i_rant_plat_parse_ip(mask) : 0;
        t->n_ifaces++;
    }
    t->group_binds_any = i_rant_json_true(i_rant_json_get(r, "group_binds_any"));
    t->state = I_RANT_LINK_OPEN;
    t->backoff_ms = 250;
    if (resuming && !i_rant_json_true(i_rant_json_get(r, "resumed"))) i_rant_tunnel_restore(t);
}

/* ---- frames in ---- */

static void i_rant_tunnel_enqueue(i_RantTunnelSock *k, const i_RantTunnelDgram *d){
    size_t need = 8u + d->len;
    if (k->q_tail + need > k->q_cap && k->q_head){
        memmove(k->q, k->q + k->q_head, k->q_tail - k->q_head);
        k->q_tail -= k->q_head; k->q_head = 0;
    }
    if (k->q_tail + need > k->q_cap){
        if (k->q_tail + need > k->q_max) return;      /* full, so it drops, as a socket's buffer does */
        if (!i_rant_tunnel_reserve(&k->q, &k->q_cap, k->q_tail + need)) return;
    }
    i_rant_le_w16(k->q + k->q_tail, d->len);
    memcpy(k->q + k->q_tail + 2, d->ip, 4);
    i_rant_le_w16(k->q + k->q_tail + 6, d->port);
    memcpy(k->q + k->q_tail + 8, d->data, d->len);
    k->q_tail += need;
}

static i_RantTunnelSock *i_rant_tunnel_by_number(i_RantTunnel *t, uint16_t sock){
    int i;
    for (i = 0; i < I_RANT_TUNNEL_SOCKS; i++)
        if (t->socks[i].used && t->socks[i].sock == sock) return &t->socks[i];
    return NULL;
}

static void i_rant_tunnel_binary(i_RantTunnel *t, const uint8_t *b, size_t len){
    if (len >= 1 && b[0] == I_RANT_TUNNEL_UDP){
        size_t at = 0; i_RantTunnelDgram d;
        while (i_rant_tunnel_next(b, len, &at, &d)){
            i_RantTunnelSock *k = i_rant_tunnel_by_number(t, d.sock);
            if (k) i_rant_tunnel_enqueue(k, &d);
        }
    } else if (len >= I_RANT_TUNNEL_PING_BYTES && b[0] == I_RANT_TUNNEL_PING){
        uint8_t pong[I_RANT_TUNNEL_PONG_BYTES];
        memcpy(pong, b, I_RANT_TUNNEL_PONG_BYTES);
        pong[0] = I_RANT_TUNNEL_PONG;
        i_rant_tunnel_frame(t, I_RANT_WS_BINARY, pong, sizeof pong, NULL, 0);
    }
}

static void i_rant_tunnel_text(i_RantTunnel *t, const char *s, size_t len){
    i_RantJson j = i_rant_json(s, len);
    char op[16], err[160];
    uint32_t seq;
    int ok;
    i_rant_json_str(i_rant_json_get(j, "op"), op, sizeof op);
    if (strcmp(op, "reply") != 0){
        if (strcmp(op, "warning") == 0){
            i_rant_json_str(i_rant_json_get(j, "text"), err, sizeof err);
            i_rant_tunnel_note_set(t, "the bridge warns", err);
        }
        return;
    }
    seq = (uint32_t)i_rant_json_int(i_rant_json_get(j, "seq"), 0);
    ok = i_rant_json_true(i_rant_json_get(j, "ok"));
    i_rant_json_str(i_rant_json_get(j, "error"), err, sizeof err);
    if (t->state == I_RANT_LINK_OPENING && seq == t->open_seq){
        if (ok) i_rant_tunnel_opened(t, j);
        else i_rant_tunnel_drop(t, err[0] ? err : "refused", 1);
        return;
    }
    if (seq && seq == t->want_seq){
        if (!i_rant_tunnel_reserve((uint8_t**)&t->reply, &t->reply_cap, len + 1)) return;
        memcpy(t->reply, s, len); t->reply[len] = 0;
        t->got = 1;
        return;
    }
    if (!ok) i_rant_tunnel_note_set(t, "the bridge refused a socket op", err);
}

static void i_rant_tunnel_message(i_RantTunnel *t, int op, const uint8_t *p, size_t len){
    if (op == I_RANT_WS_TEXT) i_rant_tunnel_text(t, (const char*)p, len);
    else if (op == I_RANT_WS_BINARY) i_rant_tunnel_binary(t, p, len);
}

/* Takes every whole frame out of rx. */
static void i_rant_tunnel_frames(i_RantTunnel *t){
    size_t at = 0;
    while (t->state != I_RANT_LINK_DOWN && t->state != I_RANT_LINK_REFUSED){
        i_RantWsFrame f; const uint8_t *p; size_t plen;
        int r = i_rant_ws_frame(t->rx + at, t->rx_len - at, &f);
        if (r < 0){ i_rant_tunnel_drop(t, "a malformed frame", 0); return; }
        if (r == 0 || f.len > I_RANT_TUNNEL_MSG_MAX || f.head + f.len > t->rx_len - at){
            if (r == 1 && f.len > I_RANT_TUNNEL_MSG_MAX){ i_rant_tunnel_drop(t, "a message too big", 0); return; }
            break;
        }
        p = t->rx + at + f.head; plen = (size_t)f.len;
        at += f.head + plen;
        switch (f.opcode){
        case I_RANT_WS_TEXT: case I_RANT_WS_BINARY:
            if (f.fin) i_rant_tunnel_message(t, f.opcode, p, plen);
            else {
                t->msg_op = f.opcode; t->msg_len = 0;
                if (!i_rant_tunnel_reserve(&t->msg, &t->msg_cap, plen)){ i_rant_tunnel_drop(t, "out of memory", 0); return; }
                memcpy(t->msg, p, plen); t->msg_len = plen;
            }
            break;
        case I_RANT_WS_CONT:
            if (t->msg_len + plen > I_RANT_TUNNEL_MSG_MAX
                || !i_rant_tunnel_reserve(&t->msg, &t->msg_cap, t->msg_len + plen)){ i_rant_tunnel_drop(t, "a message too big", 0); return; }
            memcpy(t->msg + t->msg_len, p, plen); t->msg_len += plen;
            if (f.fin){ i_rant_tunnel_message(t, t->msg_op, t->msg, t->msg_len); t->msg_len = 0; }
            break;
        case I_RANT_WS_PING:
            i_rant_tunnel_frame(t, I_RANT_WS_PONG, p, plen, NULL, 0);
            break;
        case I_RANT_WS_CLOSE: {
            char why[160]; unsigned code = plen >= 2 ? ((unsigned)p[0] << 8) | p[1] : 0;
            int n = snprintf(why, sizeof why, "closed with %u", code);
            if (plen > 2 && n > 0 && (size_t)n + 1 < sizeof why)
                snprintf(why + n, sizeof why - (size_t)n, " %.*s", (int)(plen - 2), (const char*)p + 2);
            i_rant_tunnel_drop(t, why, code == 4001 || code == 4003);
            return; }
        default: break;
        }
    }
    if (t->state == I_RANT_LINK_DOWN || t->state == I_RANT_LINK_REFUSED) return;
    if (at){ memmove(t->rx, t->rx + at, t->rx_len - at); t->rx_len -= at; }
}

/* Sends what tx holds until the socket is full. */
static void i_rant_tunnel_write(i_RantTunnel *t){
    size_t done = 0;
    while (done < t->tx_len && t->tcp != RANT_SOCK_BAD){
        int r = i_rant_plat_tcp_send(t->tcp, t->tx + done, t->tx_len - done);
        if (r > 0){ done += (size_t)r; continue; }
        if (r < 0 && i_rant_plat_would_block()) break;
        i_rant_tunnel_drop(t, "the connection failed while sending", 0);
        return;
    }
    if (done){ memmove(t->tx, t->tx + done, t->tx_len - done); t->tx_len -= done; }
}

/* Reads what the socket has, then acts on it. */
static void i_rant_tunnel_read(i_RantTunnel *t){
    for (;;){
        int r;
        if (!i_rant_tunnel_reserve(&t->rx, &t->rx_cap, t->rx_len + 65536u)){ i_rant_tunnel_drop(t, "out of memory", 0); return; }
        r = i_rant_plat_tcp_recv(t->tcp, t->rx + t->rx_len, t->rx_cap - t->rx_len);
        if (r > 0){ t->rx_len += (size_t)r; t->heard_us = i_rant_plat_now_us(); continue; }
        if (r < 0 && i_rant_plat_would_block()) break;
        i_rant_tunnel_drop(t, r == 0 ? "the bridge closed the connection" : "the connection failed", 0);
        return;
    }
    if (t->state == I_RANT_LINK_HANDSHAKE){
        int head = i_rant_ws_accept(t->key, (const char*)t->rx, t->rx_len);
        if (head < 0){ i_rant_tunnel_drop(t, "no Rant bridge answered there", 0); return; }
        if (head == 0) return;
        memmove(t->rx, t->rx + head, t->rx_len - (size_t)head);
        t->rx_len -= (size_t)head;
        i_rant_tunnel_send_open(t);
    }
    i_rant_tunnel_frames(t);
}

/* One step of the link off its socket's readiness, then its clock. Lock held. */
static void i_rant_tunnel_step(i_RantTunnel *t, short revents){
    uint64_t now;
    if (t->state == I_RANT_LINK_CONNECTING && revents){
        char req[700]; size_t n;
        if (!i_rant_plat_tcp_connected(t->tcp)){ i_rant_tunnel_drop(t, "cannot reach the bridge", 0); return; }
        if (!i_rant_plat_random(t->key, sizeof t->key)) memcpy(t->key, &t->mask, 8);
        n = i_rant_ws_request(&t->url, t->key, req, sizeof req);
        if (!n || !i_rant_tunnel_reserve(&t->tx, &t->tx_cap, t->tx_len + n)){ i_rant_tunnel_drop(t, "out of memory", 0); return; }
        memcpy(t->tx + t->tx_len, req, n); t->tx_len += n;
        t->state = I_RANT_LINK_HANDSHAKE;
        t->heard_us = i_rant_plat_now_us();
    } else if (t->state >= I_RANT_LINK_HANDSHAKE && t->state <= I_RANT_LINK_OPEN && (revents & (RANT_POLLIN | RANT_POLLERR)))
        i_rant_tunnel_read(t);
    if (t->tcp != RANT_SOCK_BAD && t->state != I_RANT_LINK_CONNECTING) i_rant_tunnel_write(t);

    now = i_rant_plat_now_us();
    switch (t->state){
    case I_RANT_LINK_DOWN:
        if (now >= t->retry_us && !t->closing) i_rant_tunnel_start(t);
        break;
    case I_RANT_LINK_CONNECTING: case I_RANT_LINK_HANDSHAKE: case I_RANT_LINK_OPENING:
        if (now >= t->deadline_us) i_rant_tunnel_drop(t, "the bridge did not answer in time", 0);
        break;
    case I_RANT_LINK_OPEN:
        if (now - t->heard_us > I_RANT_TUNNEL_SILENT_US) i_rant_tunnel_drop(t, "the bridge went silent", 0);
        break;
    default: break;
    }
}

/* What the link's socket waits for. */
static short i_rant_tunnel_events(const i_RantTunnel *t){
    if (t->state == I_RANT_LINK_CONNECTING) return RANT_POLLOUT;
    return (short)(RANT_POLLIN | (t->tx_len ? RANT_POLLOUT : 0));
}

/* How long the link may sleep before its clock needs it. */
static int i_rant_tunnel_wait_ms(const i_RantTunnel *t, int timeout_ms){
    uint64_t now = i_rant_plat_now_us(), at;
    if (t->state == I_RANT_LINK_REFUSED) return timeout_ms;
    at = t->state == I_RANT_LINK_DOWN ? t->retry_us
       : t->state == I_RANT_LINK_OPEN ? t->heard_us + I_RANT_TUNNEL_SILENT_US : t->deadline_us;
    {   int ms = at <= now ? 0 : (int)((at - now + 999u) / 1000u);
        return timeout_ms < 0 || ms < timeout_ms ? ms : timeout_ms; }
}

/* Steps the link until done says so or timeout_ms passes. Lock not held. */
static int i_rant_tunnel_drive(i_RantTunnel *t, int (*done)(i_RantTunnel *t, void *u), void *u, int timeout_ms){
    uint64_t end = i_rant_plat_now_us() + (uint64_t)timeout_ms * 1000u;
    for (;;){
        i_RantPollfd p; int ok, wait;
        uint64_t now;
        i_rant_tunnel_lock(t);
        ok = done(t, u);
        if (ok || t->state == I_RANT_LINK_REFUSED || (now = i_rant_plat_now_us()) >= end){ i_rant_tunnel_unlock(t); return ok; }
        wait = i_rant_tunnel_wait_ms(t, (int)((end - now + 999u) / 1000u));
        p.fd = t->tcp; p.events = i_rant_tunnel_events(t); p.revents = 0;
        i_rant_tunnel_unlock(t);
        if (p.fd != RANT_SOCK_BAD) i_rant_plat_poll(&p, 1, wait);
        else i_rant_plat_poll(NULL, 0, wait);
        i_rant_tunnel_lock(t);
        if (t->tcp == p.fd) i_rant_tunnel_step(t, p.revents);
        else i_rant_tunnel_step(t, 0);
        i_rant_tunnel_unlock(t);
    }
}

/* The first open stops at the first failure, which the caller reports. */
static int i_rant_tunnel_first_open(i_RantTunnel *t, void *u){
    (void)u;
    return t->state == I_RANT_LINK_OPEN || t->drops;
}
static int i_rant_tunnel_has_reply(i_RantTunnel *t, void *u){
    (void)u;
    return t->got || t->state != I_RANT_LINK_OPEN;
}
static int i_rant_tunnel_closed(i_RantTunnel *t, void *u){
    (void)u;
    return t->state != I_RANT_LINK_OPEN;
}

i_RantTunnel *i_rant_tunnel_open(const char *url, const char *passphrase, const char *name,
                                 char *why, size_t why_cap){
    i_RantTunnel *t;
    if (why && why_cap) why[0] = 0;
    t = (i_RantTunnel*)i_rant_plat_realloc(NULL, sizeof *t);
    if (!t){ if (why) snprintf(why, why_cap, "out of memory"); return NULL; }
    memset(t, 0, sizeof *t);
    if (!i_rant_ws_url(url, &t->url)){
        if (why) snprintf(why, why_cap, "%s is no ws:// URL%s", url ? url : "(null)",
                          url && strncmp(url, "wss://", 6) == 0 ? ", and this library speaks ws:// only" : "");
        i_rant_plat_realloc(t, 0);
        return NULL;
    }
    snprintf(t->passphrase, sizeof t->passphrase, "%s", passphrase ? passphrase : "");
    snprintf(t->name, sizeof t->name, "%s", name ? name : "");
    t->tcp = RANT_SOCK_BAD;
    t->backoff_ms = 250;
    t->next_sock = 1;
    if (!i_rant_plat_random(&t->mask, sizeof t->mask)) t->mask = i_rant_plat_now_us() ^ i_rant_plat_pid();
    t->mask |= 1u;
#ifdef RANT_THREADS
    i_rant_plat_mutex_init(&t->mu);
#endif
    i_rant_tunnel_start(t);
    i_rant_tunnel_drive(t, i_rant_tunnel_first_open, NULL, 2 * (int)(I_RANT_TUNNEL_STEP_US / 1000u));
    if (t->state != I_RANT_LINK_OPEN){
        if (why) snprintf(why, why_cap, "%s", t->drops ? t->why : "the bridge did not answer in time");
        i_rant_tunnel_close(t);
        return NULL;
    }
    t->noted = 0;
    return t;
}

void i_rant_tunnel_close(i_RantTunnel *t){
    int i;
    if (!t) return;
    i_rant_tunnel_lock(t);
    t->closing = 1;
    if (t->state == I_RANT_LINK_OPEN){
        uint8_t bye[2] = { 0x03, 0xE8 };     /* 1000, a normal close */
        i_rant_tunnel_batch_out(t);
        i_rant_tunnel_frame(t, I_RANT_WS_CLOSE, bye, sizeof bye, NULL, 0);
    }
    i_rant_tunnel_unlock(t);
    /* the bridge answers a close with its own, which ends the link */
    i_rant_tunnel_drive(t, i_rant_tunnel_closed, NULL, 1000);
    if (t->tcp != RANT_SOCK_BAD) i_rant_plat_close(t->tcp);
    for (i = 0; i < I_RANT_TUNNEL_SOCKS; i++) i_rant_plat_realloc(t->socks[i].q, 0);
    i_rant_plat_realloc(t->rx, 0);
    i_rant_plat_realloc(t->msg, 0);
    i_rant_plat_realloc(t->tx, 0);
    i_rant_plat_realloc(t->reply, 0);
#ifdef RANT_THREADS
    i_rant_plat_mutex_destroy(&t->mu);
#endif
    i_rant_plat_realloc(t, 0);
}

int i_rant_tunnel_note(i_RantTunnel *t, char *why, size_t why_cap){
    int had;
    i_rant_tunnel_lock(t);
    had = t->noted;
    if (had) snprintf(why, why_cap, "%s", t->note);
    t->noted = 0;
    i_rant_tunnel_unlock(t);
    return had;
}

/* ---- the sockets ---- */

static i_RantTunnelSock *i_rant_tunnel_sock(i_RantTunnel *t, i_RantSock s){
    int slot = I_RANT_TUNNEL_SLOT(s);
    if (slot < 0 || slot >= I_RANT_TUNNEL_SOCKS || !t->socks[slot].used) return NULL;
    return &t->socks[slot];
}

static i_RantSock i_rant_tunnel_udp_open(i_RantTunnel *t){
    int i; i_RantSock s = RANT_SOCK_BAD;
    i_rant_tunnel_lock(t);
    for (i = 0; i < I_RANT_TUNNEL_SOCKS; i++){
        i_RantTunnelSock *k = &t->socks[i];
        if (k->used) continue;
        memset(k, 0, sizeof *k);
        k->used = 1;
        /* numbers are not reused at once, so a late datagram for a closed one finds nothing */
        for (;;){
            int j, taken = 0;
            k->sock = t->next_sock;
            t->next_sock = (uint16_t)(t->next_sock == 65535u ? 1u : t->next_sock + 1u);
            for (j = 0; j < I_RANT_TUNNEL_SOCKS; j++)
                if (j != i && t->socks[j].used && t->socks[j].sock == k->sock) taken = 1;
            if (!taken) break;
        }
        k->ttl = -1; k->loop = -1;
        k->q_max = I_RANT_TUNNEL_QUEUE;
        s = I_RANT_TUNNEL_HANDLE(i);
        break;
    }
    i_rant_tunnel_unlock(t);
    return s;
}

static void i_rant_tunnel_sock_close(i_RantTunnel *t, i_RantSock s){
    i_RantTunnelSock *k;
    i_rant_tunnel_lock(t);
    k = i_rant_tunnel_sock(t, s);
    if (k){
        char body[64];
        if (k->bound && t->state == I_RANT_LINK_OPEN){
            snprintf(body, sizeof body, "\"op\":\"udp_close\",\"sock\":%u", (unsigned)k->sock);
            i_rant_tunnel_ask(t, body);
        }
        i_rant_plat_realloc(k->q, 0);
        memset(k, 0, sizeof *k);
    }
    i_rant_tunnel_unlock(t);
}

/* A bind waits for the bridge, since the node needs its port now. */
static int i_rant_tunnel_bind(i_RantTunnel *t, i_RantSock s, uint32_t if_naddr, uint16_t port, int reuse){
    i_RantTunnelSock *k; int ok = 0;
    i_rant_tunnel_lock(t);
    k = i_rant_tunnel_sock(t, s);
    if (k && t->state == I_RANT_LINK_OPEN){
        k->if_naddr = if_naddr; k->port = port; k->reuse = (uint8_t)(reuse != 0);
        t->got = 0;
        k->bound = 0;
        {   char body[160], f[16];
            i_rant_tunnel_dotted(if_naddr, f);
            snprintf(body, sizeof body, "\"op\":\"udp_bind\",\"sock\":%u,\"port\":%u,\"if\":\"%s\",\"reuse\":%s",
                     (unsigned)k->sock, (unsigned)port, f, reuse ? "true" : "false");
            t->want_seq = i_rant_tunnel_ask(t, body); }
        i_rant_tunnel_write(t);
        i_rant_tunnel_unlock(t);
        i_rant_tunnel_drive(t, i_rant_tunnel_has_reply, NULL, (int)(I_RANT_TUNNEL_STEP_US / 1000u));
        i_rant_tunnel_lock(t);
        k = i_rant_tunnel_sock(t, s);
        if (k && t->got){
            i_RantJson r = i_rant_json(t->reply, strlen(t->reply));
            int64_t bound = i_rant_json_int(i_rant_json_get(r, "port"), 0);
            if (i_rant_json_true(i_rant_json_get(r, "ok")) && bound > 0 && bound <= 65535){
                k->port = (uint16_t)bound; k->bound = 1; ok = 1;
            }
        }
        t->want_seq = 0; t->got = 0;
    }
    i_rant_tunnel_unlock(t);
    return ok;
}

static int i_rant_tunnel_send(i_RantTunnel *t, i_RantSock s, const void *buf, size_t len,
                              const uint8_t ip[4], uint16_t port){
    i_RantTunnelSock *k; int r = (int)len;
    if (len > 0xFFFFu - I_RANT_TUNNEL_RECORD) return r;     /* no UDP datagram is this big */
    i_rant_tunnel_lock(t);
    k = i_rant_tunnel_sock(t, s);
    /* with the link down a datagram is lost, as on a network that is down */
    if (k && k->bound && t->state == I_RANT_LINK_OPEN){
        if (t->tx_len > I_RANT_TUNNEL_BACKLOG) r = -1;
        else {
            if (t->batch_len + I_RANT_TUNNEL_RECORD + len > I_RANT_TUNNEL_BATCH) i_rant_tunnel_batch_out(t);
            if (!t->batch_len){ t->batch[0] = I_RANT_TUNNEL_UDP; t->batch[1] = 0; t->batch_len = 2; }
            t->batch_len += i_rant_tunnel_record(t->batch + t->batch_len, k->sock, ip, port, k->via, (uint16_t)len);
            memcpy(t->batch + t->batch_len, buf, len);
            t->batch_len += len;
        }
    }
    i_rant_tunnel_unlock(t);
    return r;
}

static int i_rant_tunnel_recv(i_RantTunnel *t, i_RantSock s, void *buf, size_t cap,
                              uint8_t src_ip[4], uint16_t *src_port){
    i_RantTunnelSock *k; int r = -1;
    i_rant_tunnel_lock(t);
    k = i_rant_tunnel_sock(t, s);
    if (k && k->q_head < k->q_tail){
        const uint8_t *e = k->q + k->q_head;
        size_t len = i_rant_le_r16(e), n = len < cap ? len : cap;
        memcpy(buf, e + 8, n);
        if (src_ip) memcpy(src_ip, e + 2, 4);
        if (src_port) *src_port = i_rant_le_r16(e + 6);
        k->q_head += 8u + len;
        if (k->q_head == k->q_tail) k->q_head = k->q_tail = 0;
        r = (int)n;
    }
    i_rant_tunnel_unlock(t);
    return r;
}

static void i_rant_tunnel_flush(i_RantTunnel *t){
    i_rant_tunnel_lock(t);
    if (t->state == I_RANT_LINK_OPEN){
        i_rant_tunnel_batch_out(t);
        i_rant_tunnel_write(t);
    }
    i_rant_tunnel_unlock(t);
}

/* Readiness of the tunnel's sockets in fds, the others' zeroed. Lock held. */
static int i_rant_tunnel_ready(i_RantTunnel *t, i_RantPollfd *fds, int n){
    int i, ready = 0;
    for (i = 0; i < n; i++){
        i_RantTunnelSock *k = i_rant_tunnel_sock(t, fds[i].fd);
        fds[i].revents = 0;
        if (k && (fds[i].events & RANT_POLLIN) && k->q_head < k->q_tail){ fds[i].revents = RANT_POLLIN; ready++; }
    }
    return ready;
}

static int i_rant_tunnel_poll(i_RantTunnel *t, i_RantPollfd *fds, int n, int timeout_ms){
    i_RantPollfd real[8]; int map[8], nr = 0, i, ready, link = -1, r;
    i_rant_tunnel_lock(t);
    ready = i_rant_tunnel_ready(t, fds, n);
    if (ready){
        /* a step without a wait, so pings are answered while datagrams wait */
        if (t->tcp != RANT_SOCK_BAD) i_rant_tunnel_step(t, t->state == I_RANT_LINK_CONNECTING ? 0 : RANT_POLLIN);
        i_rant_tunnel_unlock(t);
        return ready;
    }
    if (t->tcp != RANT_SOCK_BAD){
        real[nr].fd = t->tcp; real[nr].events = i_rant_tunnel_events(t); real[nr].revents = 0;
        link = nr++;
    }
    for (i = 0; i < n && nr < 8; i++){
        if (fds[i].fd < RANT_SOCK_BAD) continue;          /* the tunnel's own */
        real[nr] = fds[i]; real[nr].revents = 0; map[nr] = i; nr++;
    }
    timeout_ms = i_rant_tunnel_wait_ms(t, timeout_ms);
    i_rant_tunnel_unlock(t);
    r = i_rant_plat_poll(real, nr, timeout_ms);
    i_rant_tunnel_lock(t);
    if (link >= 0 && t->tcp == real[link].fd) i_rant_tunnel_step(t, real[link].revents);
    else i_rant_tunnel_step(t, 0);
    ready = i_rant_tunnel_ready(t, fds, n);
    for (i = 0; i < nr; i++){
        if (i == link) continue;
        fds[map[i]].revents = real[i].revents;
        if (real[i].revents) ready++;
    }
    i_rant_tunnel_unlock(t);
    return r < 0 ? r : ready;
}

#endif /* RANT_TUNNEL */

/* ---- the sockets every runtime uses ---- */

i_RantSock i_rant_net_udp_open(i_RantTunnel *t){
#ifdef RANT_TUNNEL
    if (t) return i_rant_tunnel_udp_open(t);
#endif
    (void)t;
    return i_rant_plat_udp_open();
}

void i_rant_net_close(i_RantTunnel *t, i_RantSock s){
#ifdef RANT_TUNNEL
    if (t){ i_rant_tunnel_sock_close(t, s); return; }
#endif
    (void)t;
    i_rant_plat_close(s);
}

int i_rant_net_bind(i_RantTunnel *t, i_RantSock s, uint32_t if_naddr, uint16_t port, int reuse){
#ifdef RANT_TUNNEL
    if (t) return i_rant_tunnel_bind(t, s, if_naddr, port, reuse);
#endif
    (void)t;
    return i_rant_plat_bind(s, if_naddr, port, reuse);
}

uint16_t i_rant_net_local_port(i_RantTunnel *t, i_RantSock s){
#ifdef RANT_TUNNEL
    if (t){
        uint16_t port = 0; i_RantTunnelSock *k;
        i_rant_tunnel_lock(t);
        k = i_rant_tunnel_sock(t, s);
        if (k && k->bound) port = k->port;
        i_rant_tunnel_unlock(t);
        return port;
    }
#endif
    (void)t;
    return i_rant_plat_local_port(s);
}

void i_rant_net_set_nonblock(i_RantTunnel *t, i_RantSock s){
    if (!t) i_rant_plat_set_nonblock(s);       /* a tunnel's sockets never block */
}

void i_rant_net_set_rcvbuf(i_RantTunnel *t, i_RantSock s, int bytes){
#ifdef RANT_TUNNEL
    if (t){
        i_RantTunnelSock *k;
        i_rant_tunnel_lock(t);
        k = i_rant_tunnel_sock(t, s);
        if (k && bytes > (int)I_RANT_TUNNEL_QUEUE) k->q_max = (size_t)bytes;
        i_rant_tunnel_unlock(t);
        return;
    }
#endif
    (void)t;
    i_rant_plat_set_rcvbuf(s, bytes);
}

void i_rant_net_set_sndbuf(i_RantTunnel *t, i_RantSock s, int bytes){
    if (!t) i_rant_plat_set_sndbuf(s, bytes);  /* the tunnel's backlog is the send buffer */
}

void i_rant_net_suppress_connreset(i_RantTunnel *t, i_RantSock s){
    if (!t) i_rant_plat_suppress_connreset(s);
}

void i_rant_net_mcast_setif(i_RantTunnel *t, i_RantSock s, uint32_t if_naddr){
#ifdef RANT_TUNNEL
    if (t){
        i_RantTunnelSock *k;
        i_rant_tunnel_lock(t);
        k = i_rant_tunnel_sock(t, s);
        if (k) k->via = if_naddr;
        i_rant_tunnel_unlock(t);
        return;
    }
#endif
    (void)t;
    i_rant_plat_mcast_setif(s, if_naddr);
}

#ifdef RANT_TUNNEL
/* An option is kept for a new session and sent now. */
static void i_rant_tunnel_option(i_RantTunnel *t, i_RantSock s, int ttl, int loop, int own_groups){
    i_RantTunnelSock *k;
    i_rant_tunnel_lock(t);
    k = i_rant_tunnel_sock(t, s);
    if (k){
        if (ttl >= 0) k->ttl = (int16_t)ttl;
        if (loop >= 0) k->loop = (int8_t)loop;
        if (own_groups > 0) k->own_groups = 1;
        if (k->bound && t->state == I_RANT_LINK_OPEN) i_rant_tunnel_ask_option(t, k);
    }
    i_rant_tunnel_unlock(t);
}
#endif

void i_rant_net_mcast_ttl(i_RantTunnel *t, i_RantSock s, uint8_t ttl){
#ifdef RANT_TUNNEL
    if (t){ i_rant_tunnel_option(t, s, ttl, -1, 0); return; }
#endif
    (void)t;
    i_rant_plat_mcast_ttl(s, ttl);
}

void i_rant_net_mcast_loop(i_RantTunnel *t, i_RantSock s, int on){
#ifdef RANT_TUNNEL
    if (t){ i_rant_tunnel_option(t, s, -1, on ? 1 : 0, 0); return; }
#endif
    (void)t;
    i_rant_plat_mcast_loop(s, on);
}

void i_rant_net_mcast_own_groups(i_RantTunnel *t, i_RantSock s){
#ifdef RANT_TUNNEL
    if (t){ i_rant_tunnel_option(t, s, -1, -1, 1); return; }
#endif
    (void)t;
    i_rant_plat_mcast_own_groups(s);
}

int i_rant_net_mcast_join(i_RantTunnel *t, i_RantSock s, uint32_t group_naddr, uint32_t if_naddr){
#ifdef RANT_TUNNEL
    if (t){
        i_RantTunnelSock *k; int ok = 0;
        i_rant_tunnel_lock(t);
        k = i_rant_tunnel_sock(t, s);
        if (k && k->n_groups < I_RANT_TUNNEL_GROUPS){
            k->groups[k->n_groups].group = group_naddr;
            k->groups[k->n_groups].if_naddr = if_naddr;
            k->n_groups++;
            if (k->bound && t->state == I_RANT_LINK_OPEN) i_rant_tunnel_ask_member(t, k, group_naddr, if_naddr, 1);
            ok = 1;
        }
        i_rant_tunnel_unlock(t);
        return ok;
    }
#endif
    (void)t;
    return i_rant_plat_mcast_join(s, group_naddr, if_naddr);
}

void i_rant_net_mcast_leave(i_RantTunnel *t, i_RantSock s, uint32_t group_naddr, uint32_t if_naddr){
#ifdef RANT_TUNNEL
    if (t){
        i_RantTunnelSock *k; uint8_t g;
        i_rant_tunnel_lock(t);
        k = i_rant_tunnel_sock(t, s);
        for (g = 0; k && g < k->n_groups; g++){
            if (k->groups[g].group != group_naddr || k->groups[g].if_naddr != if_naddr) continue;
            k->groups[g] = k->groups[--k->n_groups];
            if (k->bound && t->state == I_RANT_LINK_OPEN) i_rant_tunnel_ask_member(t, k, group_naddr, if_naddr, 0);
            break;
        }
        i_rant_tunnel_unlock(t);
        return;
    }
#endif
    (void)t;
    i_rant_plat_mcast_leave(s, group_naddr, if_naddr);
}

int i_rant_net_send(i_RantTunnel *t, i_RantSock s, const void *buf, size_t len,
                    const uint8_t ip[4], uint16_t port){
#ifdef RANT_TUNNEL
    if (t) return i_rant_tunnel_send(t, s, buf, len, ip, port);
#endif
    (void)t;
    return i_rant_plat_send(s, buf, len, ip, port);
}

int i_rant_net_recv(i_RantTunnel *t, i_RantSock s, void *buf, size_t cap,
                    uint8_t src_ip[4], uint16_t *src_port){
#ifdef RANT_TUNNEL
    if (t) return i_rant_tunnel_recv(t, s, buf, cap, src_ip, src_port);
#endif
    (void)t;
    return i_rant_plat_recv(s, buf, cap, src_ip, src_port);
}

int i_rant_net_would_block(i_RantTunnel *t){
    return t ? 1 : i_rant_plat_would_block();
}

int i_rant_net_last_error(i_RantTunnel *t){
    return t ? 0 : i_rant_plat_last_socket_error();
}

int i_rant_net_poll(i_RantTunnel *t, i_RantPollfd *fds, int n, int timeout_ms){
#ifdef RANT_TUNNEL
    if (t) return i_rant_tunnel_poll(t, fds, n, timeout_ms);
#endif
    (void)t;
    return i_rant_plat_poll(fds, n, timeout_ms);
}

void i_rant_net_flush(i_RantTunnel *t){
#ifdef RANT_TUNNEL
    if (t) i_rant_tunnel_flush(t);
#else
    (void)t;
#endif
}

int i_rant_net_local_ifaces(i_RantTunnel *t, i_RantIface *out, int max){
#ifdef RANT_TUNNEL
    if (t){
        int i, n;
        i_rant_tunnel_lock(t);
        n = t->n_ifaces < max ? t->n_ifaces : max;
        for (i = 0; i < n; i++) out[i] = t->ifaces[i];
        i_rant_tunnel_unlock(t);
        return n;
    }
#endif
    (void)t;
    return i_rant_plat_local_ifaces(out, max);
}

uint32_t i_rant_net_loopback_group_bind(i_RantTunnel *t, uint32_t loopback_naddr){
#ifdef RANT_TUNNEL
    if (t){
        int any;
        i_rant_tunnel_lock(t);
        any = t->group_binds_any;
        i_rant_tunnel_unlock(t);
        return any ? 0 : loopback_naddr;
    }
#endif
    (void)t;
    return i_rant_plat_loopback_group_bind(loopback_naddr);
}
