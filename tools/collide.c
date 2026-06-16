/* Reproduce a 64-bit topic-identity collision and watch DART detect it.
 *
 * Two different topic names that hash (FNV-1a 64) to the same dart_topic_id are
 * astronomically rare to meet by chance (~1 in 2^32 by the birthday bound), but
 * Pollard's rho finds one in O(1) memory and ~2^32 hash steps (seconds-to-a-
 * minute). We then stand up two nodes: a publisher on name A and a subscriber on
 * name B (A != B, same identity). DART must fire on_collision and refuse the
 * match, so the subscriber receives nothing: a hash clash never cross-wires.
 *
 *   Windows: gcc -O2 -std=c99 -Wall -Idist examples/collide.c -o collide.exe -lws2_32 -lbcrypt
 *   POSIX  : cc  -O2 -std=c99 -Wall -Idist examples/collide.c -o collide -lpthread
 */
#define DART_IMPLEMENTATION
#include "dart.h"

#include <stdio.h>
#include <string.h>

/* u64 -> 11 printable chars, base 64. Injective over the full u64 range (11*6 =
 * 66 bits), NUL-free, so it is a valid topic name and FNV sees every byte. */
static const char ALPHA[64] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
#define NAME_LEN 11

static void encode(uint64_t x, char *out){
    int i;
    for (i=0;i<NAME_LEN;i++){ out[i]=ALPHA[x & 63u]; x >>= 6; }
    out[NAME_LEN]='\0';
}
/* f(x) = dart_topic_id(encode(x)), computed without materializing the string.
 * Byte order matches encode(), so this equals hashing the produced name. */
static uint64_t f_step(uint64_t x){
    uint64_t h = 1469598103934665603ull; int i;
    for (i=0;i<NAME_LEN;i++){ h ^= (uint8_t)ALPHA[x & 63u]; x >>= 6; h *= 1099511628211ull; }
    return h;
}

/* Brent's cycle detection on f, then walk the tail to the two distinct inputs
 * that map to the cycle entrance: f(pa) == f(pb), pa != pb. Returns 1 on
 * success, filling the two out-params. */
static int rho_collision(uint64_t x0, uint64_t *pa_out, uint64_t *pb_out){
    uint64_t power=1, lam=1, tort=x0, hare=f_step(x0), a, b, pa=0, pb=0, i;
    uint64_t steps=0;
    while (tort != hare){
        if (power == lam){ tort = hare; power <<= 1; lam = 0; }
        hare = f_step(hare); lam++;
        if ((++steps & 0x3FFFFFFFu) == 0)
            printf("  searching... %.1f billion steps\n", (double)steps/1e9);
    }
    a = b = x0;
    for (i=0;i<lam;i++) b = f_step(b);
    if (a == b) return 0;                 /* x0 sat on the cycle: caller reseeds */
    while (a != b){ pa=a; pb=b; a=f_step(a); b=f_step(b); }
    *pa_out = pa; *pb_out = pb;
    return 1;
}

static unsigned long g_samples, g_collisions;
static void on_message(void *u, uint16_t ch, uint32_t from, const void *d, size_t n){
    (void)u;(void)ch;(void)from;(void)d;(void)n; g_samples++;
}
static void on_event(void *u, const dart_event *ev){
    (void)u;
    if (ev->kind != DART_NAME_COLLISION) return;
    g_collisions++;
    printf("  DART_NAME_COLLISION: identity %016llx  ours=\"%s\"  -> match refused\n",
           (unsigned long long)ev->first, ev->detail ? ev->detail : "");
}

int main(void){
    char a[NAME_LEN+1], b[NAME_LEN+1];
    uint64_t pa, pb, seed = 0x9E3779B97F4A7C15ull;
    int i;

    printf("Searching for two topic names with the same 64-bit dart_topic_id...\n");
    while (!rho_collision(seed, &pa, &pb)) seed++;
    encode(pa, a); encode(pb, b);

    printf("\nFound a collision:\n");
    printf("  name A = \"%s\"  -> %016llx\n", a, (unsigned long long)dart_topic_id(a));
    printf("  name B = \"%s\"  -> %016llx\n", b, (unsigned long long)dart_topic_id(b));
    if (strcmp(a,b)==0 || dart_topic_id(a)!=dart_topic_id(b)){
        printf("INTERNAL ERROR: not a valid distinct-name collision\n");
        return 1;
    }
    printf("  distinct names, identical identity: confirmed.\n\n");

    /* Now prove DART detects it: publisher on A, subscriber on B, same host. */
    printf("Standing up a publisher on A and a subscriber on B...\n");
    {
        static uint8_t mem_w[1<<20], mem_r[1<<20];
        dart_channel_def cw, cr; dart_node_config wc, rc; dart_node *w, *r;
        uint8_t payload[16]; uint64_t end;
        memset(payload, 0x5A, sizeof payload);

        cw = (dart_channel_def){ .name=a, .role=DART_PUB_ONLY,
            .qos={ .reliability=DART_RELIABLE, .keep_last=1, .catch_up=1,
                   .max_message_bytes=32, .heartbeat_us=50000 } };
        cr = cw; cr.name=b; cr.role=DART_SUB_ONLY;

        wc = (dart_node_config){ .domain=41, .channels=&cw, .n_channels=1,
                                 .discovery={ .max_peers=4 } };
        rc = wc; rc.channels=&cr;
        rc.on_message=on_message; rc.on_event=on_event;

        w = dart_node_open(mem_w, sizeof mem_w, &wc);
        r = dart_node_open(mem_r, sizeof mem_r, &rc);
        if (!w || !r){ fprintf(stderr, "node open failed\n"); return 1; }

        end = 0;
        for (i=0;i<150;i++){            /* ~3s: discover, exchange interest, send */
            dart_node_send(w, 0, payload, sizeof payload);   /* channel 0 */
            dart_node_poll(w, 0); dart_node_poll(r, 20);
            (void)end;
        }
        dart_node_close(r, 1);
        dart_node_close(w, 1);
    }

    printf("\nResult:\n");
    printf("  collisions detected : %lu  (expected >= 1)\n", g_collisions);
    printf("  samples cross-wired : %lu  (expected 0)\n", g_samples);
    if (g_collisions >= 1 && g_samples == 0){
        printf("PASS: the clash was detected and the match refused.\n");
        return 0;
    }
    printf("FAIL\n");
    return 1;
}
