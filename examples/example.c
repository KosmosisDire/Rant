/* full node demo: discovery + reliable transport, no socket code. Run two on one
 * host (./node alice, ./node bob); they discover and exchange a reliable counter.
 *   POSIX  : cc  -std=c99 -Wall -Idist examples/example.c -o node
 *   Windows: gcc -std=c99 -Wall -Idist examples/example.c -o example.exe -lws2_32 -lbcrypt */
#define DART_TRANSPORT_IMPLEMENTATION
#include "dart_transport.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *g_name = "node";

/* per-channel receive counters; keep on_message cheap so it never throttles poll */
#define MAX_CH 64
static unsigned long g_rx[MAX_CH];

static void on_message(void *u, uint16_t ch, uint32_t from, const void *data, size_t len){
    (void)u; (void)from; (void)data; (void)len;
    if (ch < MAX_CH) g_rx[ch]++;   /* ch is the channel handle (its index) */
}

int main(int argc, char **argv){
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc>1) g_name = argv[1];
    /* Optional role: "pub", "sub", or "both" (default). */
    const char *role = (argc>2 ? argv[2] : "both");

    uint8_t role_dir = strcmp(role,"pub")==0 ? DART_PUB_ONLY
                     : strcmp(role,"sub")==0 ? DART_SUB_ONLY : DART_PUBSUB;
    uint8_t use_mcast = (uint8_t)(argc>3 ? atoi(argv[3]) : 0);

    dart_channel_def ch = {
        .name       = "counter",
        .qos        = { .reliability = DART_RELIABLE },
        .role       = role_dir,
        .multicast  = use_mcast,
    };

    dart_node_config cfg = {
        .domain     = 7,
        .channels   = &ch,
        .n_channels = 1,
        .on_message = on_message,
        .net        = { .multicast_interface = use_mcast ? "127.0.0.1" : NULL },
    };

    static uint8_t mem[1<<20];   /* 1 MB */
    dart_node *n = dart_node_open(mem, sizeof mem, &cfg);
    if (!n){ fprintf(stderr, "dart_node_open failed\n"); return 1; }

    printf("[%s] up (%s), discovering...\n", g_name, role);

    time_t last = time(NULL);
    int counter = 0;
    for (;;){
        dart_node_poll(n, 10);         /* service the socket, 10 ms tick */
        char msg[64];
        int ln = sprintf(msg, "%s #%d", g_name, counter++);
        dart_node_send(n, 0, msg, (size_t)ln);   /* channel 0 = first in channels[] */

        /* Every 5s, report the receive rate (Hz) on each channel. */
        time_t now = time(NULL);
        if (now - last >= 5){
            double secs = (double)(now - last);
            uint16_t i;
            for (i = 0; i < cfg.n_channels; i++){
                if (i < MAX_CH){
                    printf("[%s] channel %u: %.1f Hz\n", g_name, i, g_rx[i]/secs);
                    g_rx[i] = 0;
                }
            }
            last = now;
        }
    }
}