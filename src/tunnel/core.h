/* The tunnel's sans IO parts: the WebSocket a bridge serves, the JSON of its control messages
 * and the records of its datagram batches. spec/tunnel.md has the protocol. */
#ifndef RANT_TUNNEL_CORE_H
#define RANT_TUNNEL_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RANT_TUNNEL_PROTO    13
#define RANT_TUNNEL_UDP      9      /* a batch [u8 9][u8 flags] then records */
#define RANT_TUNNEL_PING     7      /* [u8 7][u32 seq][u32 us][u32 jitter_us][u32 samples][u32 missed] */
#define RANT_TUNNEL_PONG     8      /* [u8 8][u32 seq] */
#define RANT_TUNNEL_PING_BYTES 21
#define RANT_TUNNEL_PONG_BYTES 5
#define RANT_TUNNEL_RECORD   14     /* [u16 sock][4 ip][u16 port][4 via][u16 len] */
#define RANT_TUNNEL_BATCH    60000  /* the most one batch carries */

void   i_rant_sha1(const void *data, size_t len, uint8_t out[20]);
/* NUL terminated. Returns the characters written, 0 when cap is too small. */
size_t i_rant_base64(const uint8_t *in, size_t len, char *out, size_t cap);

/* A ws:// URL taken apart. Port 80 and path "/" when it names none. */
typedef struct { char host[256]; uint16_t port; char path[256]; } i_RantWsUrl;
/* 1 for a ws:// URL, 0 otherwise, wss:// included. */
int    i_rant_ws_url(const char *url, i_RantWsUrl *out);

/* The opening request for a 16 byte random key. Returns its length, 0 when cap is too small. */
size_t i_rant_ws_request(const i_RantWsUrl *u, const uint8_t key[16], char *out, size_t cap);
/* The server's reply to it: the length of its head once whole and accepted, 0 while more
 * is needed, negative when it refused or is no WebSocket. */
int    i_rant_ws_accept(const uint8_t key[16], const char *reply, size_t len);

enum { RANT_WS_CONT = 0, RANT_WS_TEXT = 1, RANT_WS_BINARY = 2, RANT_WS_CLOSE = 8,
       RANT_WS_PING = 9, RANT_WS_PONG = 10 };
#define RANT_WS_HEAD_MAX 14

/* A client frame's head, which is masked. Returns its length. */
size_t i_rant_ws_head(uint8_t out[RANT_WS_HEAD_MAX], int opcode, size_t len, const uint8_t mask[4]);
/* Masks p in place, offset bytes into the payload. */
void   i_rant_ws_mask(uint8_t *p, size_t len, const uint8_t mask[4], size_t offset);

/* A frame from the server, which never masks. */
typedef struct { int opcode; int fin; size_t head; uint64_t len; } i_RantWsFrame;
/* 1 with its parts when its head is whole, 0 when more is needed, -1 when malformed. */
int    i_rant_ws_frame(const uint8_t *buf, size_t len, i_RantWsFrame *out);

/* A JSON value inside a text, as a view. len 0 means absent. */
typedef struct { const char *p; size_t len; } i_RantJson;
i_RantJson i_rant_json(const char *text, size_t len);
/* The value of key in the object j. */
i_RantJson i_rant_json_get(i_RantJson j, const char *key);
/* Walks the array j: *at starts at 0. 1 with the next element, 0 at the end. */
int        i_rant_json_next(i_RantJson j, size_t *at, i_RantJson *elem);
int64_t    i_rant_json_int(i_RantJson v, int64_t fallback);
int        i_rant_json_true(i_RantJson v);
/* A string unescaped into out, NUL terminated and cut to fit. Its length, 0 when no string. */
size_t     i_rant_json_str(i_RantJson v, char *out, size_t cap);
/* s as a quoted JSON string. Returns the length, 0 when cap is too small. */
size_t     i_rant_json_quote(const char *s, char *out, size_t cap);

/* One datagram in a batch. */
typedef struct {
    uint16_t       sock;
    uint8_t        ip[4];      /* the source toward the client */
    uint16_t       port;
    const uint8_t *data;
    uint16_t       len;
} i_RantTunnelDgram;
/* Walks the batch b: *at starts at 0. 1 with the next datagram, 0 at the end or at a record
 * cut short. */
int    i_rant_tunnel_next(const uint8_t *b, size_t len, size_t *at, i_RantTunnelDgram *out);
/* One record's head, its payload to follow. Returns RANT_TUNNEL_RECORD. */
size_t i_rant_tunnel_record(uint8_t out[RANT_TUNNEL_RECORD], uint16_t sock, const uint8_t ip[4],
                            uint16_t port, uint32_t via_naddr, uint16_t len);

#ifdef __cplusplus
}
#endif
#endif /* RANT_TUNNEL_CORE_H */
