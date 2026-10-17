/* The tunnel's sans IO parts: SHA-1 and base64 against known vectors, the WebSocket handshake
 * against RFC 6455's example, frame heads, the JSON reader and batch records. */
#include "support/test.h"

static void hex(const uint8_t *b, size_t n, char *out){
    size_t i;
    for (i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]);
}

static void hash_checks(void){
    uint8_t d[20]; char h[41], b[64];
    i_rant_sha1("abc", 3, d); hex(d, 20, h);
    CHECK(strcmp(h, "a9993e364706816aba3e25717850c26c9cd0d89d") == 0, "sha1: abc (%s)", h);
    i_rant_sha1("", 0, d); hex(d, 20, h);
    CHECK(strcmp(h, "da39a3ee5e6b4b0d3255bfef95601890afd80709") == 0, "sha1: empty (%s)", h);
    i_rant_sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d); hex(d, 20, h);
    CHECK(strcmp(h, "84983e441c3bd26ebaae4aa1f95129e5e54670f1") == 0, "sha1: 56 bytes pad into a second block (%s)", h);
    CHECK(i_rant_base64((const uint8_t*)"foobar", 6, b, sizeof b) == 8 && strcmp(b, "Zm9vYmFy") == 0, "base64: foobar");
    CHECK(i_rant_base64((const uint8_t*)"fo", 2, b, sizeof b) == 4 && strcmp(b, "Zm8=") == 0, "base64: one pad");
    CHECK(i_rant_base64((const uint8_t*)"f", 1, b, sizeof b) == 4 && strcmp(b, "Zg==") == 0, "base64: two pads");
    CHECK(i_rant_base64((const uint8_t*)"foobar", 6, b, 8) == 0, "base64: refuses a short buffer");
}

static void handshake_checks(void){
    i_RantWsUrl u; char req[512];
    const uint8_t *key = (const uint8_t*)"the sample nonce";
    const char *ok = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "sec-websocket-accept:  s3pPLMBiTxaQ9kYGzzhZRbK+xOo= \r\n\r\nrest";
    const char *bad = "HTTP/1.1 101 Switching Protocols\r\nSec-WebSocket-Accept: AAAA\r\n\r\n";
    const char *http = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";

    CHECK(i_rant_ws_url("ws://192.168.1.20:7480", &u) && strcmp(u.host, "192.168.1.20") == 0
          && u.port == 7480 && strcmp(u.path, "/") == 0, "url: host and port, path /");
    CHECK(i_rant_ws_url("ws://bridge.lan/rant?x=1", &u) && strcmp(u.host, "bridge.lan") == 0
          && u.port == 80 && strcmp(u.path, "/rant?x=1") == 0, "url: port 80 by default, path kept");
    CHECK(i_rant_ws_url("ws://h?q", &u) && strcmp(u.path, "/?q") == 0, "url: a query alone gets /");
    CHECK(!i_rant_ws_url("wss://h:1", &u), "url: wss refused");
    CHECK(!i_rant_ws_url("ws://:7480", &u), "url: no host refused");
    CHECK(!i_rant_ws_url("ws://h:70000", &u) && !i_rant_ws_url("ws://h:", &u) && !i_rant_ws_url("ws://h:7a", &u),
          "url: a bad port refused");
    CHECK(!i_rant_ws_url("http://h", &u) && !i_rant_ws_url(NULL, &u), "url: another scheme refused");

    i_rant_ws_url("ws://h:7480", &u);
    CHECK(i_rant_ws_request(&u, key, req, sizeof req) > 0
          && strstr(req, "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n") != NULL
          && strstr(req, "GET / HTTP/1.1\r\nHost: h:7480\r\n") == req
          && strcmp(req + strlen(req) - 4, "\r\n\r\n") == 0, "request: the key and the head");
    CHECK(i_rant_ws_request(&u, key, req, 40) == 0, "request: refuses a short buffer");
    CHECK(i_rant_ws_accept(key, ok, strlen(ok)) == (int)(strlen(ok) - 4), "accept: RFC 6455's example, header case and spaces aside");
    CHECK(i_rant_ws_accept(key, ok, 40) == 0, "accept: waits for the whole head");
    CHECK(i_rant_ws_accept(key, bad, strlen(bad)) < 0, "accept: a wrong accept refused");
    CHECK(i_rant_ws_accept(key, http, strlen(http)) < 0, "accept: no upgrade refused");
}

static void frame_checks(void){
    uint8_t h[I_RANT_WS_HEAD_MAX], buf[32];
    const uint8_t mask[4] = { 1, 2, 3, 4 };
    i_RantWsFrame f;
    CHECK(i_rant_ws_head(h, I_RANT_WS_TEXT, 5, mask) == 6 && h[0] == 0x81 && h[1] == 0x85 && h[2] == 1,
          "head: short, masked, fin");
    CHECK(i_rant_ws_head(h, I_RANT_WS_BINARY, 300, mask) == 8 && h[1] == (0x80 | 126) && h[2] == 1 && h[3] == 44,
          "head: 16 bit length");
    CHECK(i_rant_ws_head(h, I_RANT_WS_BINARY, 70000, mask) == 14 && h[1] == (0x80 | 127) && h[7] == 1 && h[8] == 0x11
          && h[9] == 0x70, "head: 64 bit length");
    memcpy(buf, "hello", 5);
    i_rant_ws_mask(buf, 3, mask, 0); i_rant_ws_mask(buf + 3, 2, mask, 3);
    CHECK(buf[0] == ('h' ^ 1) && buf[3] == ('l' ^ 4) && buf[4] == ('o' ^ 1), "mask: in pieces at an offset");

    buf[0] = 0x82; buf[1] = 126; buf[2] = 1; buf[3] = 0;
    CHECK(i_rant_ws_frame(buf, 3, &f) == 0, "frame: waits for its extended length");
    CHECK(i_rant_ws_frame(buf, 4, &f) == 1 && f.opcode == I_RANT_WS_BINARY && f.fin && f.head == 4 && f.len == 256,
          "frame: 16 bit length");
    buf[0] = 0x01; buf[1] = 3;
    CHECK(i_rant_ws_frame(buf, 2, &f) == 1 && f.opcode == I_RANT_WS_TEXT && !f.fin && f.len == 3, "frame: a first fragment");
    buf[0] = 0x89; buf[1] = 0x83;
    CHECK(i_rant_ws_frame(buf, 2, &f) < 0, "frame: a masked server frame refused");
    buf[0] = 0x09; buf[1] = 3;
    CHECK(i_rant_ws_frame(buf, 2, &f) < 0, "frame: a fragmented control frame refused");
    buf[0] = 0xC2; buf[1] = 3;
    CHECK(i_rant_ws_frame(buf, 2, &f) < 0, "frame: an extension bit refused");
}

static void json_checks(void){
    const char *t = "{\"op\":\"reply\", \"seq\" : 12,\"ok\":true,\"nested\":{\"ok\":false},"
                    "\"interfaces\":[{\"addr\":\"192.168.1.20\",\"mask\":\"255.255.255.0\"},{\"addr\":\"10.0.0.2\"}],"
                    "\"error\":\"a \\\"quoted\\\" \\u00e9\\n\",\"neg\":-7}";
    i_RantJson j = i_rant_json(t, strlen(t)), v, e;
    char s[64], q[64]; size_t at = 0; int n = 0;
    CHECK(i_rant_json_int(i_rant_json_get(j, "seq"), -1) == 12, "json: an int after spaces");
    CHECK(i_rant_json_true(i_rant_json_get(j, "ok")), "json: true at the top");
    CHECK(i_rant_json_get(j, "missing").len == 0 && i_rant_json_int(i_rant_json_get(j, "missing"), 5) == 5,
          "json: absent gives the fallback");
    CHECK(i_rant_json_int(i_rant_json_get(j, "neg"), 0) == -7, "json: a negative int");
    CHECK(!i_rant_json_true(i_rant_json_get(i_rant_json_get(j, "nested"), "ok")), "json: a nested object");
    CHECK(i_rant_json_str(i_rant_json_get(j, "op"), s, sizeof s) == 5 && strcmp(s, "reply") == 0, "json: a string");
    CHECK(i_rant_json_str(i_rant_json_get(j, "error"), s, sizeof s) > 0 && strcmp(s, "a \"quoted\" \xc3\xa9\n") == 0,
          "json: escapes and \\u to UTF-8");
    CHECK(i_rant_json_str(i_rant_json_get(j, "op"), s, 3) == 2 && strcmp(s, "re") == 0, "json: a string cut to fit");
    v = i_rant_json_get(j, "interfaces");
    while (i_rant_json_next(v, &at, &e)){
        i_rant_json_str(i_rant_json_get(e, "addr"), s, sizeof s);
        if (n == 0) CHECK(strcmp(s, "192.168.1.20") == 0, "json: the first element");
        if (n == 1) CHECK(strcmp(s, "10.0.0.2") == 0 && i_rant_json_get(e, "mask").len == 0, "json: the second element");
        n++;
    }
    CHECK(n == 2, "json: an array walked to its end (%d)", n);
    CHECK(i_rant_json_get(i_rant_json("{\"a\":[1,", 8), "a").len == 0, "json: a cut text gives nothing");
    CHECK(i_rant_json_get(i_rant_json("[1]", 3), "a").len == 0, "json: no object gives nothing");
    CHECK(i_rant_json_quote("say \"hi\"\\\x01", q, sizeof q) > 0 && strcmp(q, "\"say \\\"hi\\\"\\\\\\u0001\"") == 0,
          "json: quote escapes (%s)", q);
    CHECK(i_rant_json_quote("abcdef", q, 6) == 0, "json: quote refuses a short buffer");
}

static void batch_checks(void){
    uint8_t b[128]; size_t len = 2, at = 0;
    const uint8_t ip1[4] = { 192, 168, 1, 5 }, ip2[4] = { 127, 0, 0, 1 };
    i_RantTunnelDgram d;
    b[0] = I_RANT_TUNNEL_UDP; b[1] = 0;
    len += i_rant_tunnel_record(b + len, 3, ip1, 7400, 0, 4); memcpy(b + len, "ping", 4); len += 4;
    len += i_rant_tunnel_record(b + len, 65535, ip2, 50000, 0, 0);
    CHECK(i_rant_tunnel_next(b, len, &at, &d) && d.sock == 3 && memcmp(d.ip, ip1, 4) == 0 && d.port == 7400
          && d.len == 4 && memcmp(d.data, "ping", 4) == 0, "batch: the first datagram");
    CHECK(i_rant_tunnel_next(b, len, &at, &d) && d.sock == 65535 && d.port == 50000 && d.len == 0, "batch: an empty one");
    CHECK(!i_rant_tunnel_next(b, len, &at, &d), "batch: then the end");
    at = 0;
    CHECK(!i_rant_tunnel_next(b, 2 + I_RANT_TUNNEL_RECORD + 3, &at, &d), "batch: a cut record ends it");
    b[0] = I_RANT_TUNNEL_PING; at = 0;
    CHECK(!i_rant_tunnel_next(b, len, &at, &d), "batch: another op is no batch");
    i_rant_tunnel_record(b, 1, ip1, 1, 0x0100007Fu, 0);
    CHECK(b[8] == 0x7F && b[11] == 0x01, "record: via keeps the naddr's bytes");
}

static const TestCase cases[] = {
    { "hash", hash_checks },
    { "handshake", handshake_checks },
    { "frame", frame_checks },
    { "json", json_checks },
    { "batch", batch_checks },
};

TEST_MAIN(cases)
