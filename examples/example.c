/* full node demo (discovery + reliable transport, no socket code).
 *
 *   POSIX  : cc  -std=c99 -Wall -Idist examples/example.c -o node
 *   Windows: gcc -std=c99 -Wall -Idist examples/example.c -o example.exe -lws2_32 -lbcrypt
 *
 * Run two instances on one host: ./node alice and ./node bob. They discover
 * each other over multicast and exchange a reliable counter on channel 1.
 */
#define DART_TRANSPORT_IMPLEMENTATION
#include "dart_transport.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *g_name = "node";

/* Per-channel receive counters. Keep on_sample cheap so it never throttles the poll loop. */
#define MAX_CH 64
static unsigned long g_rx[MAX_CH];

static void on_sample(void *u, uint16_t ch, uint32_t from, const void *data, size_t len){
    (void)u; (void)from; (void)data; (void)len;
    if (ch < MAX_CH) g_rx[ch]++;
}

int main(int argc, char **argv){
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc>1) g_name = argv[1];
    /* Optional role: "pub", "sub", or "both" (default). */
    const char *role = (argc>2 ? argv[2] : "both");

    dart_channel_def ch; memset(&ch, 0, sizeof ch);
    ch.channel_id = 1;
    ch.qos.reliability     = DART_RELIABLE;
    ch.qos.history_depth   = 8;
    ch.qos.max_sample_bytes= 256;
    ch.qos.heartbeat_us    = 200000;   /* 200 ms */
    ch.qos.nack_delay_us   = 20000;    /* 20 ms */
    ch.dir = strcmp(role,"pub")==0 ? DART_PUB_ONLY
           : strcmp(role,"sub")==0 ? DART_SUB_ONLY : DART_PUBSUB;
    ch.mcast = (uint8_t)(argc>3 ? atoi(argv[3]) : 0);

    dart_node_config cfg; memset(&cfg, 0, sizeof cfg);
    cfg.domain_id  = 7;
    cfg.data_port  = 0;            /* 0 means OS assigns a free ephemeral port */
    cfg.channels   = &ch;
    cfg.n_channels = 1;
    cfg.on_sample  = on_sample;
    if (ch.mcast) cfg.mcast_if = "127.0.0.1";  /* same-host demo: stay local */
    /* Remaining cfg fields use defaults. */

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
        dart_node_send(n, 1, msg, (size_t)ln);

        /* Every 5s, report the receive rate (Hz) on each channel. */
        time_t now = time(NULL);
        if (now - last >= 5){
            double secs = (double)(now - last);
            uint16_t i;
            for (i = 0; i < cfg.n_channels; i++){
                uint16_t id = cfg.channels[i].channel_id;
                if (id < MAX_CH){
                    printf("[%s] channel %u: %.1f Hz\n", g_name, id, g_rx[id]/secs);
                    g_rx[id] = 0;
                }
            }
            last = now;
        }
    }
    /* dart_node_close(n, 1); */
}