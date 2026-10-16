/* threadbench: the threaded send path cost and the drain backpressure, one started pub node,
 * one started sub node and this sender thread per target rate (spec/testing.md). */
#include "bench.h"

#include <stdio.h>
#include <string.h>

#ifdef RANT_THREADS

static volatile unsigned long g_tb_recv;
static void tb_on_message(const RantMsg *m){ (void)m; g_tb_recv++; }

static RantNode *tb_open(const char *name, int sub, int disable_shm){
    RantAllocator a = rant_allocator_heap(0);
    RantNodeOpts o;
    memset(&o, 0, sizeof o);
    o.domain = 51;
    o.disable_shm = (uint8_t)disable_shm;
    o.net.multicast_interface = "127.0.0.1";
    return rant_node_open(&a, name, sub ? tb_on_message : NULL, NULL, &o);
}

static RantTopic *tb_channel(RantNode *n, int sub){
    RantTopicOpts co;
    memset(&co, 0, sizeof co);
    co.qos.keep_last = 64;               /* best effort: isolates the unsent drain guard */
    co.qos.max_message_bytes = 64;
    co.qos.heartbeat_us = 50000;
    return rant_node_create_topic(n, "tb/bench", sub ? RANT_SUB_ONLY : RANT_PUB_ONLY, NULL, &co);
}

static void tb_run_rate(RantNode *w, RantTopic *ch, unsigned rate_hz, double dur_s){
    uint8_t payload[32];
    uint64_t t0, t_end, next = 0, wait_us0, wait_us1;
    uint32_t wait_n0, wait_n1, ev0, ev1;
    unsigned long sent = 0, recv0 = g_tb_recv;
    double wall;
    char label[16];

    memset(payload, 0x42, sizeof payload);
    rant_node_backpressure_stats(w, &wait_us0, &wait_n0);
    ev0 = rant_node_evicted_unsent(w);

    t0 = bench_now_ns() / 1000u;
    t_end = t0 + (uint64_t)(dur_s * 1e6);
    if (rate_hz) next = t0;
    while (bench_now_ns() / 1000u < t_end){
        if (rate_hz){
            uint64_t now = bench_now_ns() / 1000u;
            if (now < next) continue;                /* busy wait pacing */
            next += 1000000u / rate_hz;
            if (next < now) next = now;              /* fell behind: no burst catch up */
        }
        rant_topic_send(ch, rant_bytes(payload, sizeof payload), NULL);
        sent++;
    }
    wall = (bench_now_ns() / 1000u - t0) / 1e6;

    bench_sleep_ms(300);                             /* let deliveries settle */
    rant_node_backpressure_stats(w, &wait_us1, &wait_n1);
    ev1 = rant_node_evicted_unsent(w);

    if (rate_hz) sprintf(label, "%u", rate_hz);
    else         sprintf(label, "flat-out");
    printf("%9s  %10.0f  %8.1f%%  %10.3f  %7.2f%%  %7u  %8.2f%%\n",
           label,
           sent / wall,
           100.0 * (double)(wait_n1 - wait_n0) / (sent ? sent : 1),
           (wait_us1 - wait_us0) / 1e3,
           100.0 * (wait_us1 - wait_us0) / (wall * 1e6),
           ev1 - ev0,
           100.0 * (double)(g_tb_recv - recv0) / (sent ? sent : 1));
}

static int tb_mode(const char *title, int disable_shm){
    static const unsigned rates[] = { 10000, 50000, 100000, 200000, 500000, 0 };
    RantNode *w = tb_open("tb-pub", 0, disable_shm);
    RantNode *r = tb_open("tb-sub", 1, disable_shm);
    RantTopic *cw, *cr;
    unsigned k;
    if (!w || !r){ fprintf(stderr, "threadbench: open failed\n"); return 1; }
    cw = tb_channel(w, 0); cr = tb_channel(r, 1); (void)cr;
    rant_node_start(w); rant_node_start(r);
    { uint64_t end = bench_now_ns() + 5000000000ull;
      while (rant_topic_match_count(cw) == 0 && bench_now_ns() < end) bench_sleep_ms(2); }
    if (rant_topic_match_count(cw) != 1){ fprintf(stderr, "threadbench: no match\n"); return 1; }

    printf("\n== %s ==\n", title);
    printf("%9s  %10s  %9s  %10s  %8s  %7s  %9s\n",
           "target", "sent/s", "waited", "wait ms", "wait%", "evicted", "delivered");
    for (k = 0; k < sizeof rates / sizeof rates[0]; k++)
        tb_run_rate(w, cw, rates[k], 2.0);
    rant_node_close(r, 1);
    rant_node_close(w, 1);
    return 0;
}

int threadbench_main(void){
    printf("Rant threaded send path bench (32B best effort, keep_last 64, loopback, one sender)\n"
           "waited = sends that slept in the drain or backpressure wait, wait%% = of sender wall time\n");
    if (tb_mode("threaded, UDP loopback (shm off)", 1)) return 1;
#ifdef RANT_SHM
    if (tb_mode("threaded, SHM same host path", 0)) return 1;
#endif

    /* the single threaded flat out baseline: the naive send, poll(0), poll(0) loop */
    {
        RantNode *w = tb_open("tb-base-pub", 0, 1);
        RantNode *r = tb_open("tb-base-sub", 1, 1);
        RantTopic *cw, *cr;
        uint8_t payload[32]; unsigned long sent = 0, recv0;
        uint64_t t0, t_end; double wall;
        if (!w || !r){ fprintf(stderr, "threadbench: baseline open failed\n"); return 1; }
        cw = tb_channel(w, 0); cr = tb_channel(r, 1);
        (void)cr;
        if (!cw){ fprintf(stderr, "threadbench: baseline topic failed\n"); return 1; }
        memset(payload, 0x42, sizeof payload);
        { uint64_t end = bench_now_ns() + 5000000000ull;
          while (rant_topic_match_count(cw) == 0 && bench_now_ns() < end){
              rant_node_poll(w, 1); rant_node_poll(r, 0);
          } }
        recv0 = g_tb_recv;
        t0 = bench_now_ns(); t_end = t0 + 2000000000ull;
        while (bench_now_ns() < t_end){
            rant_topic_send(cw, rant_bytes(payload, sizeof payload), NULL);
            rant_node_poll(w, 0);
            rant_node_poll(r, 0);
            sent++;
        }
        wall = (bench_now_ns() - t0) / 1e9;
        { uint64_t settle = bench_now_ns() + 300000000ull;
          while (bench_now_ns() < settle){ rant_node_poll(w, 0); rant_node_poll(r, 0); } }
        printf("\n== single threaded baseline: send, poll(0), poll(0) flat out, UDP ==\n");
        printf("  %10.0f send/s   delivered %.2f%%\n",
               sent / wall, sent ? 100.0 * (double)(g_tb_recv - recv0) / sent : 0.0);
        rant_node_close(r, 1);
        rant_node_close(w, 1);
    }
    return 0;
}

#else

int threadbench_main(void){
    fprintf(stderr, "threadbench needs threads (RANT_THREADS is off)\n");
    return 1;
}

#endif
