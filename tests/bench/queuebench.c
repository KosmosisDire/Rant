/* queuebench: the callback queue cost against inline callbacks, one started pair on loopback
 * per payload size and mode, flat out reliable (spec/testing.md). */
#include "bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef RANT_THREADS

static volatile unsigned long g_qb_recv;
static void qb_on_message(const RantMsg *m){ (void)m; g_qb_recv++; }

static volatile int g_qb_stop;
static uint32_t     g_qb_len;
static void qb_sender(void *arg){
    static uint8_t payload[1u << 20];
    RantTopic *ch = (RantTopic *)arg;
    memset(payload, 0x42, g_qb_len);
    while (!g_qb_stop)
        rant_topic_send(ch, rant_bytes(payload, g_qb_len), NULL);
}

static void qb_run(uint32_t size, int queued, int disable_shm){
    RantAllocator aw = rant_allocator_heap(0);
    RantAllocator ar = rant_allocator_heap(0);
    RantNodeOpts o; RantTopicOpts co;
    RantNode *w, *r; RantTopic *cw, *cr; RantQueue *qq = NULL;
    i_RantThread th;
    uint64_t t0, t_end;
    unsigned long recv = 0;
    double wall, rate;
    uint32_t shm_rx0 = 0, shm_rx1 = 0;

    memset(&o, 0, sizeof o);
    o.domain = 52; o.disable_shm = (uint8_t)disable_shm;
    o.net.multicast_interface = "127.0.0.1";
    w = rant_node_open(&aw, "qb-pub", NULL, NULL, &o);
    r = rant_node_open(&ar, "qb-sub", qb_on_message, NULL, &o);
    if (!w || !r){ fprintf(stderr, "queuebench: open failed\n"); exit(1); }
    if (queued && !(qq = rant_node_create_queue(r))){ fprintf(stderr, "queuebench: queue\n"); exit(1); }
    memset(&co, 0, sizeof co);
    co.qos.reliability = RANT_RELIABLE;
    co.qos.keep_last = (uint16_t)(size >= 262144u ? 8 : 64);   /* shallow history when huge */
    co.qos.backpressure_wait_us = 200000;   /* lossless: the sender paces to the consumer */
    co.qos.heartbeat_us = 20000;
    co.qos.repair_delay_us = 5000;
    co.qos.shm_max_bytes = size;            /* pin one SHM size class per run */
    cw = rant_node_create_topic(w, "qb/t", RANT_PUB_ONLY, NULL, &co);
    /* the queue must cover the writer's in flight burst, else the reader parks and the
       overrun heals through the paced repair path. Twice the burst, at least 4 MB */
    {   uint32_t qb = 2u * co.qos.keep_last * size;
        if (qb < (4u << 20)) qb = 4u << 20;
        co.qos.queue_bytes = queued ? qb : 0;
        co.queue = qq;
    }
    cr = rant_node_create_topic(r, "qb/t", RANT_SUB_ONLY, NULL, &co);
    if (!cw || !cr){ fprintf(stderr, "queuebench: topic create failed\n"); exit(1); }
    rant_node_start(w); rant_node_start(r);
    {   uint64_t end = bench_now_ns() + 5000000000ull;
        while (rant_topic_match_count(cw) == 0 && bench_now_ns() < end) bench_sleep_ms(2); }
    if (rant_topic_match_count(cw) != 1){ fprintf(stderr, "queuebench: no match\n"); exit(1); }

#ifdef RANT_SHM
    rant_node_shm_stats(r, NULL, &shm_rx0);
#endif
    g_qb_len = size; g_qb_stop = 0; g_qb_recv = 0;
    if (!i_rant_plat_thread_start(&th, qb_sender, cw)){ fprintf(stderr, "queuebench: thread\n"); exit(1); }
    t0 = bench_now_ns(); t_end = t0 + 2000000000ull;
    if (queued){
        while (bench_now_ns() < t_end) (void)rant_queue_dispatch(qq, 0, 5);
    } else {
        while (bench_now_ns() < t_end) bench_sleep_ms(5);
    }
    recv = g_qb_recv;
    wall = (double)(bench_now_ns() - t0) / 1e9;
    g_qb_stop = 1;
    i_rant_plat_thread_join(&th);
#ifdef RANT_SHM
    rant_node_shm_stats(r, NULL, &shm_rx1);
#endif
    rate = wall > 0.0 ? recv / wall : 0.0;
    printf("%9u  %-7s  %12.0f  %10.1f  %11lu  %9u\n",
           size, queued ? "queued" : "inline", rate, rate * size / 1e6, recv, shm_rx1 - shm_rx0);
    rant_node_close(r, 1);
    rant_node_close(w, 1);
}

int queuebench_main(void){
    static const uint32_t sizes[] = { 1024, 16384, 65536, 262144, 1048576 };
    unsigned k;
    printf("Rant callback queue throughput: inline callback (service thread) against queue dispatch\n"
           "(flat out reliable, keep_last 64, backpressure paced so lossless goodput, loopback, 2s per run)\n");
#ifdef RANT_SHM
    printf("\n== SHM same host path ==\n");
    printf("%9s  %-7s  %12s  %10s  %11s  %9s\n", "payload", "mode", "msg/s", "MB/s", "delivered", "shm_rx");
    for (k = 0; k < sizeof sizes / sizeof sizes[0]; k++){
        qb_run(sizes[k], 0, 0);
        qb_run(sizes[k], 1, 0);
    }
#endif
    printf("\n== UDP loopback path (shm off) ==\n");
    printf("%9s  %-7s  %12s  %10s  %11s  %9s\n", "payload", "mode", "msg/s", "MB/s", "delivered", "shm_rx");
    for (k = 0; k < sizeof sizes / sizeof sizes[0]; k++){
        qb_run(sizes[k], 0, 1);
        qb_run(sizes[k], 1, 1);
    }
    return 0;
}

#else

int queuebench_main(void){
    fprintf(stderr, "queuebench needs threads (RANT_THREADS is off)\n");
    return 1;
}

#endif
