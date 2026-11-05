/* The tunnel's sans IO parts. RFC 6455 for the WebSocket, RFC 3174 for SHA-1, which only
 * the handshake uses, FIPS 180-4 and RFC 2104 for the passphrase proof. */

#include "core.h"
#include "../common/bytes.h"
#include <string.h>
#include <stdio.h>

/* ---- SHA-1 and base64 ---- */

static uint32_t i_rant_rol(uint32_t v, int n){ return (v << n) | (v >> (32 - n)); }

static void i_rant_sha1_block(uint32_t h[5], const uint8_t *p){
    uint32_t w[80], a, b, c, d, e, t; int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16) | ((uint32_t)p[4*i+2] << 8) | p[4*i+3];
    for (i = 16; i < 80; i++) w[i] = i_rant_rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
    for (i = 0; i < 80; i++){
        uint32_t f, k;
        if (i < 20){ f = (b & c) | (~b & d); k = 0x5A827999u; }
        else if (i < 40){ f = b ^ c ^ d; k = 0x6ED9EBA1u; }
        else if (i < 60){ f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
        t = i_rant_rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = i_rant_rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void i_rant_sha1(const void *data, size_t len, uint8_t out[20]){
    uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
    const uint8_t *p = (const uint8_t*)data;
    uint8_t tail[128]; size_t left = len, n, i; uint64_t bits = (uint64_t)len * 8u;
    while (left >= 64){ i_rant_sha1_block(h, p); p += 64; left -= 64; }
    memset(tail, 0, sizeof tail);
    memcpy(tail, p, left);
    tail[left] = 0x80;
    n = left + 9 <= 64 ? 64 : 128;
    for (i = 0; i < 8; i++) tail[n - 1 - i] = (uint8_t)(bits >> (8 * i));
    i_rant_sha1_block(h, tail);
    if (n == 128) i_rant_sha1_block(h, tail + 64);
    for (i = 0; i < 5; i++){
        out[4*i] = (uint8_t)(h[i] >> 24); out[4*i+1] = (uint8_t)(h[i] >> 16);
        out[4*i+2] = (uint8_t)(h[i] >> 8); out[4*i+3] = (uint8_t)h[i];
    }
}

static uint32_t i_rant_ror(uint32_t v, int n){ return (v >> n) | (v << (32 - n)); }

static void i_rant_sha256_block(uint32_t h[8], const uint8_t *p){
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u };
    uint32_t w[64], a, b, c, d, e, f, g, x, t1, t2; int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16) | ((uint32_t)p[4*i+2] << 8) | p[4*i+3];
    for (i = 16; i < 64; i++)
        w[i] = w[i-16] + (i_rant_ror(w[i-15], 7) ^ i_rant_ror(w[i-15], 18) ^ (w[i-15] >> 3)) + w[i-7]
             + (i_rant_ror(w[i-2], 17) ^ i_rant_ror(w[i-2], 19) ^ (w[i-2] >> 10));
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; x = h[7];
    for (i = 0; i < 64; i++){
        t1 = x + (i_rant_ror(e, 6) ^ i_rant_ror(e, 11) ^ i_rant_ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        t2 = (i_rant_ror(a, 2) ^ i_rant_ror(a, 13) ^ i_rant_ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        x = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += x;
}

/* SHA-256 of a then b, so HMAC needs no copy of its message. */
static void i_rant_sha256_two(const uint8_t *a, size_t al, const uint8_t *b, size_t bl, uint8_t out[32]){
    uint32_t h[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    uint8_t block[64]; size_t fill = 0, i; uint64_t bits = (uint64_t)(al + bl) * 8u;
    const uint8_t *parts[2]; size_t lens[2]; int k;
    parts[0] = a; lens[0] = al; parts[1] = b; lens[1] = bl;
    for (k = 0; k < 2; k++){
        const uint8_t *p = parts[k]; size_t n = lens[k];
        while (n){
            size_t take = 64 - fill < n ? 64 - fill : n;
            memcpy(block + fill, p, take);
            fill += take; p += take; n -= take;
            if (fill == 64){ i_rant_sha256_block(h, block); fill = 0; }
        }
    }
    block[fill++] = 0x80;
    if (fill > 56){ memset(block + fill, 0, 64 - fill); i_rant_sha256_block(h, block); fill = 0; }
    memset(block + fill, 0, 56 - fill);
    for (i = 0; i < 8; i++) block[63 - i] = (uint8_t)(bits >> (8 * i));
    i_rant_sha256_block(h, block);
    for (i = 0; i < 8; i++){
        out[4*i] = (uint8_t)(h[i] >> 24); out[4*i+1] = (uint8_t)(h[i] >> 16);
        out[4*i+2] = (uint8_t)(h[i] >> 8); out[4*i+3] = (uint8_t)h[i];
    }
}

void i_rant_sha256(const void *data, size_t len, uint8_t out[32]){
    i_rant_sha256_two((const uint8_t*)data, len, NULL, 0, out);
}

void i_rant_tunnel_proof(const char *passphrase, const char *nonce, char out[65]){
    uint8_t key[64], pad[64], inner[32], mac[32]; size_t kl = passphrase ? strlen(passphrase) : 0; int i;
    memset(key, 0, sizeof key);
    if (kl > 64) i_rant_sha256(passphrase, kl, key);
    else if (kl) memcpy(key, passphrase, kl);
    for (i = 0; i < 64; i++) pad[i] = (uint8_t)(key[i] ^ 0x36);
    i_rant_sha256_two(pad, 64, (const uint8_t*)nonce, nonce ? strlen(nonce) : 0, inner);
    for (i = 0; i < 64; i++) pad[i] = (uint8_t)(key[i] ^ 0x5c);
    i_rant_sha256_two(pad, 64, inner, 32, mac);
    for (i = 0; i < 32; i++){ out[2*i] = i_rant_hex(mac[i] >> 4); out[2*i+1] = i_rant_hex(mac[i]); }
    out[64] = 0;
}

size_t i_rant_base64(const uint8_t *in, size_t len, char *out, size_t cap){
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = (len + 2) / 3 * 4, i, o = 0;
    if (cap < need + 1) return 0;
    for (i = 0; i < len; i += 3){
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i+1] << 8;
        if (i + 2 < len) v |= in[i+2];
        out[o++] = abc[(v >> 18) & 63];
        out[o++] = abc[(v >> 12) & 63];
        out[o++] = i + 1 < len ? abc[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? abc[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

/* ---- the handshake ---- */

int i_rant_ws_url(const char *url, i_RantWsUrl *out){
    const char *p, *host, *end, *colon = NULL;
    size_t hl;
    memset(out, 0, sizeof *out);
    if (!url || strncmp(url, "ws://", 5) != 0) return 0;
    host = p = url + 5;
    while (*p && *p != '/' && *p != '?'){ if (*p == ':') colon = p; p++; }
    end = colon ? colon : p;
    hl = (size_t)(end - host);
    if (!hl || hl >= sizeof out->host) return 0;
    memcpy(out->host, host, hl);
    out->port = 80;
    if (colon){
        unsigned long v = 0; const char *q = colon + 1;
        if (q == p) return 0;
        for (; q < p; q++){
            if (*q < '0' || *q > '9') return 0;
            v = v * 10u + (unsigned long)(*q - '0');
            if (v > 65535u) return 0;
        }
        if (!v) return 0;
        out->port = (uint16_t)v;
    }
    {   size_t at = *p == '/' ? 0 : 1, pl = strlen(p);
        if (at + pl >= sizeof out->path) return 0;
        out->path[0] = '/';
        memcpy(out->path + at, p, pl);
    }
    return 1;
}

static size_t i_rant_ws_key(const uint8_t key[16], char out[32]){
    return i_rant_base64(key, 16, out, 32);
}

size_t i_rant_ws_request(const i_RantWsUrl *u, const uint8_t key[16], char *out, size_t cap){
    char k[32]; int n;
    i_rant_ws_key(key, k);
    n = snprintf(out, cap, "GET %s HTTP/1.1\r\nHost: %s:%u\r\nUpgrade: websocket\r\n"
                 "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
                 u->path, u->host, (unsigned)u->port, k);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

static int i_rant_lower(int c){ return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* The value of a header in an HTTP head, trimmed, or NULL. */
static const char *i_rant_ws_header(const char *head, size_t len, const char *name, size_t *vlen){
    size_t nl = strlen(name), i = 0;
    while (i < len){
        size_t eol = i, j;
        while (eol < len && head[eol] != '\r' && head[eol] != '\n') eol++;
        if (eol - i > nl && head[i + nl] == ':'){
            for (j = 0; j < nl && i_rant_lower(head[i + j]) == i_rant_lower(name[j]); j++) {}
            if (j == nl){
                size_t v = i + nl + 1, e = eol;
                while (v < e && (head[v] == ' ' || head[v] == '\t')) v++;
                while (e > v && (head[e-1] == ' ' || head[e-1] == '\t')) e--;
                *vlen = e - v;
                return head + v;
            }
        }
        i = eol;
        while (i < len && (head[i] == '\r' || head[i] == '\n')) i++;
    }
    return NULL;
}

int i_rant_ws_accept(const uint8_t key[16], const char *reply, size_t len){
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char joined[64], want[32]; uint8_t sha[20];
    const char *v; size_t vl, head = 0, i, kl;
    for (i = 0; i + 3 < len; i++)
        if (reply[i] == '\r' && reply[i+1] == '\n' && reply[i+2] == '\r' && reply[i+3] == '\n'){ head = i + 4; break; }
    if (!head) return len > 8192 ? -1 : 0;
    if (len < 12 || strncmp(reply, "HTTP/1.1 101", 12) != 0) return -1;
    kl = i_rant_ws_key(key, joined);
    memcpy(joined + kl, guid, sizeof guid);
    i_rant_sha1(joined, kl + sizeof guid - 1, sha);
    i_rant_base64(sha, 20, want, sizeof want);
    v = i_rant_ws_header(reply, head, "Sec-WebSocket-Accept", &vl);
    if (!v || vl != strlen(want) || memcmp(v, want, vl) != 0) return -1;
    return (int)head;
}

/* ---- frames ---- */

size_t i_rant_ws_head(uint8_t out[I_RANT_WS_HEAD_MAX], int opcode, size_t len, const uint8_t mask[4]){
    size_t n = 2;
    out[0] = (uint8_t)(0x80 | (opcode & 15));
    if (len < 126) out[1] = (uint8_t)(0x80 | len);
    else if (len <= 0xFFFFu){
        out[1] = 0x80 | 126; out[2] = (uint8_t)(len >> 8); out[3] = (uint8_t)len; n = 4;
    } else {
        int i; uint64_t l = (uint64_t)len;
        out[1] = 0x80 | 127;
        for (i = 0; i < 8; i++) out[2 + i] = (uint8_t)(l >> (56 - 8 * i));
        n = 10;
    }
    memcpy(out + n, mask, 4);
    return n + 4;
}

void i_rant_ws_mask(uint8_t *p, size_t len, const uint8_t mask[4], size_t offset){
    size_t i;
    for (i = 0; i < len; i++) p[i] ^= mask[(offset + i) & 3];
}

int i_rant_ws_frame(const uint8_t *buf, size_t len, i_RantWsFrame *out){
    size_t head = 2; uint64_t l;
    if (len < 2) return 0;
    if (buf[0] & 0x70) return -1;             /* no extension was negotiated */
    if (buf[1] & 0x80) return -1;             /* a server never masks */
    l = buf[1] & 0x7F;
    if (l == 126){
        if (len < 4) return 0;
        l = ((uint64_t)buf[2] << 8) | buf[3]; head = 4;
    } else if (l == 127){
        int i;
        if (len < 10) return 0;
        for (l = 0, i = 0; i < 8; i++) l = (l << 8) | buf[2 + i];
        if (l >> 62) return -1;
        head = 10;
    }
    out->opcode = buf[0] & 15;
    out->fin = (buf[0] & 0x80) != 0;
    out->head = head;
    out->len = l;
    if (out->opcode >= 8 && (!out->fin || l > 125)) return -1;     /* control frames are whole and short */
    return 1;
}

/* ---- JSON ---- */

i_RantJson i_rant_json(const char *text, size_t len){
    i_RantJson j; j.p = text; j.len = len; return j;
}

static size_t i_rant_json_ws(const char *p, size_t i, size_t n){
    while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\r' || p[i] == '\n')) i++;
    return i;
}

/* The end of the value at i, or 0 when it is malformed. Depth bounded. */
static size_t i_rant_json_skip(const char *p, size_t i, size_t n, int depth){
    if (i >= n || depth > 32) return 0;
    if (p[i] == '"'){
        for (i++; i < n; i++){
            if (p[i] == '\\'){ i++; continue; }
            if (p[i] == '"') return i + 1;
        }
        return 0;
    }
    if (p[i] == '{' || p[i] == '['){
        char close = p[i] == '{' ? '}' : ']';
        int obj = p[i] == '{';
        i = i_rant_json_ws(p, i + 1, n);
        if (i < n && p[i] == close) return i + 1;
        for (;;){
            if (obj){
                i = i_rant_json_skip(p, i, n, depth + 1);
                if (!i) return 0;
                i = i_rant_json_ws(p, i, n);
                if (i >= n || p[i] != ':') return 0;
                i = i_rant_json_ws(p, i + 1, n);
            }
            i = i_rant_json_skip(p, i, n, depth + 1);
            if (!i) return 0;
            i = i_rant_json_ws(p, i, n);
            if (i >= n) return 0;
            if (p[i] == close) return i + 1;
            if (p[i] != ',') return 0;
            i = i_rant_json_ws(p, i + 1, n);
        }
    }
    {   size_t s = i;
        while (i < n && p[i] != ',' && p[i] != '}' && p[i] != ']' && p[i] != ' '
               && p[i] != '\t' && p[i] != '\r' && p[i] != '\n') i++;
        return i > s ? i : 0;
    }
}

i_RantJson i_rant_json_get(i_RantJson j, const char *key){
    i_RantJson none = { NULL, 0 };
    const char *p = j.p; size_t n = j.len, i, kl = strlen(key);
    i = i_rant_json_ws(p, 0, n);
    if (i >= n || p[i] != '{') return none;
    i = i_rant_json_ws(p, i + 1, n);
    while (i < n && p[i] == '"'){
        size_t ks = i + 1, ke = i_rant_json_skip(p, i, n, 0), vs, ve;
        if (!ke) return none;
        i = i_rant_json_ws(p, ke, n);
        if (i >= n || p[i] != ':') return none;
        vs = i_rant_json_ws(p, i + 1, n);
        ve = i_rant_json_skip(p, vs, n, 0);
        if (!ve) return none;
        if (ke - 1 - ks == kl && memcmp(p + ks, key, kl) == 0){
            i_RantJson v; v.p = p + vs; v.len = ve - vs; return v;
        }
        i = i_rant_json_ws(p, ve, n);
        if (i < n && p[i] == ',') i = i_rant_json_ws(p, i + 1, n);
        else break;
    }
    return none;
}

int i_rant_json_next(i_RantJson j, size_t *at, i_RantJson *elem){
    const char *p = j.p; size_t n = j.len, i = *at, e;
    if (i == 0){
        i = i_rant_json_ws(p, 0, n);
        if (i >= n || p[i] != '[') return 0;
        i++;
    }
    i = i_rant_json_ws(p, i, n);
    if (i < n && p[i] == ',') i = i_rant_json_ws(p, i + 1, n);
    if (i >= n || p[i] == ']') return 0;
    e = i_rant_json_skip(p, i, n, 0);
    if (!e) return 0;
    elem->p = p + i; elem->len = e - i;
    *at = e;
    return 1;
}

int64_t i_rant_json_int(i_RantJson v, int64_t fallback){
    size_t i = 0; int neg = 0; int64_t r = 0;
    if (!v.len) return fallback;
    if (v.p[0] == '-'){ neg = 1; i = 1; }
    if (i >= v.len || v.p[i] < '0' || v.p[i] > '9') return fallback;
    for (; i < v.len && v.p[i] >= '0' && v.p[i] <= '9'; i++) r = r * 10 + (v.p[i] - '0');
    return neg ? -r : r;
}

int i_rant_json_true(i_RantJson v){
    return v.len == 4 && memcmp(v.p, "true", 4) == 0;
}

static int i_rant_hex_value(int c){
    if (c >= '0' && c <= '9') return c - '0';
    c = i_rant_lower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

size_t i_rant_json_str(i_RantJson v, char *out, size_t cap){
    size_t i, o = 0;
    if (!cap) return 0;
    out[0] = 0;
    if (v.len < 2 || v.p[0] != '"') return 0;
    for (i = 1; i + 1 < v.len; i++){
        unsigned c = (unsigned char)v.p[i];
        char enc[4]; size_t el = 1, k;
        if (c == '\\' && i + 2 < v.len){
            c = (unsigned char)v.p[++i];
            switch (c){
            case 'n': c = '\n'; break; case 't': c = '\t'; break; case 'r': c = '\r'; break;
            case 'b': c = '\b'; break; case 'f': c = '\f'; break;
            case 'u': {
                int d0, d1, d2, d3;
                if (i + 4 >= v.len) return 0;
                d0 = i_rant_hex_value(v.p[i+1]); d1 = i_rant_hex_value(v.p[i+2]); d2 = i_rant_hex_value(v.p[i+3]); d3 = i_rant_hex_value(v.p[i+4]);
                if (d0 < 0 || d1 < 0 || d2 < 0 || d3 < 0) return 0;
                c = (unsigned)((d0 << 12) | (d1 << 8) | (d2 << 4) | d3);
                i += 4;
                break; }
            default: break;      /* \" \\ \/ stand for themselves */
            }
        }
        /* UTF-8. The bridge's strings are ASCII, so a surrogate pair is not joined */
        if (c < 0x80) enc[0] = (char)c;
        else if (c < 0x800){ enc[0] = (char)(0xC0 | (c >> 6)); enc[1] = (char)(0x80 | (c & 63)); el = 2; }
        else { enc[0] = (char)(0xE0 | (c >> 12)); enc[1] = (char)(0x80 | ((c >> 6) & 63)); enc[2] = (char)(0x80 | (c & 63)); el = 3; }
        if (o + el >= cap) break;
        for (k = 0; k < el; k++) out[o++] = enc[k];
    }
    out[o] = 0;
    return o;
}

size_t i_rant_json_quote(const char *s, char *out, size_t cap){
    size_t o = 0;
    if (cap < 3) return 0;
    out[o++] = '"';
    for (; s && *s; s++){
        unsigned char c = (unsigned char)*s;
        char esc[8]; size_t el, k;
        if (c == '"' || c == '\\'){ esc[0] = '\\'; esc[1] = (char)c; el = 2; }
        else if (c < 0x20){ snprintf(esc, sizeof esc, "\\u%04x", c); el = 6; }
        else { esc[0] = (char)c; el = 1; }
        if (o + el + 2 > cap) return 0;
        for (k = 0; k < el; k++) out[o++] = esc[k];
    }
    out[o++] = '"';
    out[o] = 0;
    return o;
}

/* ---- batches ---- */

int i_rant_tunnel_next(const uint8_t *b, size_t len, size_t *at, i_RantTunnelDgram *out){
    size_t i = *at ? *at : 2;
    uint16_t l;
    if (len < 2 || b[0] != I_RANT_TUNNEL_UDP || i + I_RANT_TUNNEL_RECORD > len) return 0;
    l = i_rant_le_r16(b + i + 12);
    if (i + I_RANT_TUNNEL_RECORD + l > len) return 0;
    out->sock = i_rant_le_r16(b + i);
    memcpy(out->ip, b + i + 2, 4);
    out->port = i_rant_le_r16(b + i + 6);
    out->len = l;
    out->data = b + i + I_RANT_TUNNEL_RECORD;
    *at = i + I_RANT_TUNNEL_RECORD + l;
    return 1;
}

size_t i_rant_tunnel_record(uint8_t out[I_RANT_TUNNEL_RECORD], uint16_t sock, const uint8_t ip[4],
                            uint16_t port, uint32_t via_naddr, uint16_t len){
    i_rant_le_w16(out, sock);
    memcpy(out + 2, ip, 4);
    i_rant_le_w16(out + 6, port);
    memcpy(out + 8, &via_naddr, 4);
    i_rant_le_w16(out + 12, len);
    return I_RANT_TUNNEL_RECORD;
}
