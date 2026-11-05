/* The latency and throughput node: probes ping and pong every peer for RTT, an optional load
 * stream counts drops, and a timed run ends with the SUMMARY lines the sweep aggregates. */
#include "bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
  #define bench_getpid() ((unsigned long)GetCurrentProcessId())
#else
  #include <unistd.h>
  #define bench_getpid() ((unsigned long)getpid())
#endif

#define CH_PROBE           0       /* topic indices, in creation order */
#define CH_LOAD            1
#define XCH_ID0            2       /* extra load topics: indices 2 to 2+xch-1 */
#define XCH_MAX            512
#define LAT_DOMAIN         11
#define PING_INTERVAL_NS   250000000ull
#define REPORT_INTERVAL_NS 2000000000ull
#define RTT_SANITY_NS      2000000000ull
#define STALL_GAP_NS       50000000ull     /* a loop gap counted as a stall */
#define LOAD_DEPTH         2048            /* load history ring, at least the burst */
#define LOAD_BURST_MAX     2048            /* load messages per loop iteration at most */
#define MAX_PEERS          64
#define NAME_LEN           16

/* probe: [0]'L' [1]type [2..5]tag [6..9]seq [10..17]t_send [18..33]name */
#define PROBE_MAGIC 'L'
#define PROBE_PING  1
#define PROBE_PONG  2
#define PROBE_LEN   34
/* load: [0]'D' [1..4]seq [5..47]filler */
#define LOAD_MAGIC  'D'
#define LOAD_LEN    48

static void put32(uint8_t *p, uint32_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static void put64(uint8_t *p, uint64_t v){ int i; for(i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }
static uint32_t get32(const uint8_t *p){ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static uint64_t get64(const uint8_t *p){ uint64_t v=0; int i; for(i=0;i<8;i++) v|=((uint64_t)p[i])<<(8*i); return v; }

static const char *g_name = "node";
static uint32_t    g_tag  = 0;
static uint64_t    g_report_ns = 0;   /* wall time spent inside print_report */
static int         g_xch = 0;         /* extra topics declared */
static int         g_spread = 0;      /* 1 = load round robins across them. 2 = the same, but extras
                                         are PUB_ONLY on every node, so those writes have no readers */
static RantTopic  *g_topics[2+XCH_MAX];

static size_t build_probe(uint8_t *o, uint8_t type, uint32_t tag, uint32_t seq,
                          uint64_t t, const char *name){
    o[0]=PROBE_MAGIC; o[1]=type;
    put32(o+2,tag); put32(o+6,seq); put64(o+10,t);
    memset(o+18,0,NAME_LEN);
    { size_t k=strlen(name); if(k>NAME_LEN) k=NAME_LEN; memcpy(o+18,name,k); }
    return PROBE_LEN;
}
static size_t build_load(uint8_t *o, uint32_t seq){
    o[0]=LOAD_MAGIC; put32(o+1,seq);
    memset(o+5,0xAB,LOAD_LEN-5);
    return LOAD_LEN;
}

/* per peer stats, keyed by transport peer id */
typedef struct {
    int      used;
    uint32_t peer_id;
    char     name[NAME_LEN+1];
    uint64_t last_rtt_ns, min_rtt_ns;
    double   ewma_ns;
    double   jit_ns;             /* RFC 3550 style smoothed |delta RTT| */
    unsigned long rtt_n;
    /* load and drops per topic: cross topic arrival order is not defined, so one shared
       counter would read reorder as drops */
    uint8_t  ch_init[1+XCH_MAX];      /* [0]=CH_LOAD, [1+k]=extra k */
    uint32_t ch_expect[1+XCH_MAX];
    unsigned long load_recv, load_drops;
    uint64_t last_seen_ns;
} peer_stat;
static peer_stat g_tbl[MAX_PEERS];

static peer_stat *tbl_get(uint32_t peer_id){
    int i, freei=-1;
    for (i=0;i<MAX_PEERS;i++){
        if (g_tbl[i].used && g_tbl[i].peer_id==peer_id) return &g_tbl[i];
        if (!g_tbl[i].used && freei<0) freei=i;
    }
    if (freei<0) return NULL;
    memset(&g_tbl[freei],0,sizeof g_tbl[freei]);
    g_tbl[freei].used=1; g_tbl[freei].peer_id=peer_id;
    return &g_tbl[freei];
}

/* deferred pong queue: no sends from on_message */
typedef struct { uint32_t tag, seq; uint64_t t; } pong_req;
static pong_req g_pong_q[512];
static int      g_pong_n = 0;

/* transport reported permanent skips: keep_last supersession on reliable topics, detected
   wire loss on best effort */
static unsigned long      g_gap_evt = 0;
static unsigned long long g_gap_tus = 0;
/* backpressure cost, read from the node before SUMMARY */
static uint64_t g_wait_us = 0;
static uint32_t g_wait_n  = 0;

static void lat_on_event(const RantEvent *ev){
    if (ev->kind == RANT_MSG_LOST){ g_gap_evt++; g_gap_tus += ev->lost_count; }
}

static void lat_on_message(const RantMsg *msg){
    uint16_t ch = msg->topic_index; uint32_t from = msg->publisher_id;
    const void *data = msg->data.data; size_t len = msg->data.len;
    const uint8_t *p = (const uint8_t*)data; uint64_t now = bench_now_ns();
    peer_stat *e = tbl_get(from);
    if (!e) return;
    e->last_seen_ns = now;

    if (ch==CH_PROBE){
        uint8_t type; uint32_t tag, seq; uint64_t t;
        if (len < PROBE_LEN || p[0]!=PROBE_MAGIC) return;
        type=p[1]; tag=get32(p+2); seq=get32(p+6); t=get64(p+10);
        memcpy(e->name, p+18, NAME_LEN); e->name[NAME_LEN]=0;
        if (type==PROBE_PING){
            if (g_pong_n < (int)(sizeof g_pong_q/sizeof g_pong_q[0])){
                g_pong_q[g_pong_n].tag=tag; g_pong_q[g_pong_n].seq=seq; g_pong_q[g_pong_n].t=t;
                g_pong_n++;
            }
        } else if (type==PROBE_PONG && tag==g_tag){
            uint64_t rtt = now - t;
            if (rtt <= RTT_SANITY_NS){
                if (e->rtt_n > 0){   /* RFC 3550: J += (|D| minus J) / 16 */
                    double d = (rtt > e->last_rtt_ns) ? (double)(rtt - e->last_rtt_ns)
                                                      : (double)(e->last_rtt_ns - rtt);
                    e->jit_ns += (d - e->jit_ns) / 16.0;
                }
                e->last_rtt_ns = rtt;
                if (e->min_rtt_ns==0 || rtt < e->min_rtt_ns) e->min_rtt_ns = rtt;
                e->ewma_ns = (e->rtt_n==0) ? (double)rtt : (0.875*e->ewma_ns + 0.125*(double)rtt);
                e->rtt_n++;
            }
        }
    } else {
        uint32_t seq; int idx;
        if (ch==CH_LOAD) idx=0;
        else if (ch>=XCH_ID0 && ch<XCH_ID0+g_xch) idx=1+(ch-XCH_ID0);
        else return;
        if (len < 5 || p[0]!=LOAD_MAGIC) return;
        seq = get32(p+1);
        if (!e->ch_init[idx]){ e->ch_init[idx]=1; e->ch_expect[idx]=seq+1; e->load_recv++; }
        else if (seq >= e->ch_expect[idx]){
            e->load_drops += (seq - e->ch_expect[idx]);   /* a gap is a drop */
            e->ch_expect[idx] = seq+1;
            e->load_recv++;
        } else {
            e->load_recv++;                             /* reorder or duplicate */
        }
    }
}

static void print_report(void){
    int i, any=0;
    printf("[%s] peers:\n", g_name);
    for (i=0;i<MAX_PEERS;i++){
        peer_stat *e=&g_tbl[i];
        unsigned long tot;
        if (!e->used) continue;
        tot = e->load_recv + e->load_drops;
        printf("    %-10s (peer %u): rtt last=%.3f min=%.3f avg=%.3f jit=%.3f ms (n=%lu)",
               e->name[0]?e->name:"?", e->peer_id,
               e->last_rtt_ns/1e6, e->min_rtt_ns/1e6, e->ewma_ns/1e6, e->jit_ns/1e6, e->rtt_n);
        if (tot) printf("  load recv=%lu drop=%lu (%.2f%%)",
                        e->load_recv, e->load_drops, 100.0*e->load_drops/tot);
        printf("\n");
        any=1;
    }
    if (!any) printf("    (no peers yet)\n");
}

static void print_summary(uint64_t elapsed_ns, int load_hz, int reliable, int block_ms,
                          unsigned long load_sent,
                          unsigned long forgiven, uint64_t max_gap_ns, uint64_t stall_ns){
    int i, peers=0;
    double rtt_sum=0, jit_sum=0, rtt_min=1e18, secs=elapsed_ns/1e9;
    unsigned long recv=0, drops=0;
    for (i=0;i<MAX_PEERS;i++){
        peer_stat *e=&g_tbl[i];
        if (!e->used) continue;
        if (e->rtt_n>0){
            rtt_sum += e->ewma_ns; jit_sum += e->jit_ns;
            if(e->min_rtt_ns<rtt_min) rtt_min=e->min_rtt_ns;
            peers++;
        }
        recv += e->load_recv; drops += e->load_drops;
    }
    printf("SUMMARY name=%s load_hz=%d rel=%d blk_ms=%d sent_hz=%.0f peers=%d rtt_avg_ms=%.3f rtt_min_ms=%.3f rtt_jit_ms=%.3f recv=%lu drops=%lu droppct=%.3f gap_evt=%lu gap_tus=%llu forgiven=%lu maxgap_ms=%.1f stall_ms=%.0f report_ms=%.0f blk_wait_ms=%.0f blk_n=%lu\n",
           g_name, load_hz, reliable, block_ms,
           secs>0 ? load_sent/secs : 0.0,
           peers,
           peers ? (rtt_sum/peers)/1e6 : 0.0,
           rtt_min<1e18 ? rtt_min/1e6 : 0.0,
           peers ? (jit_sum/peers)/1e6 : 0.0,
           recv, drops,
           (recv+drops) ? 100.0*drops/(recv+drops) : 0.0,
           g_gap_evt, g_gap_tus,
           forgiven, max_gap_ns/1e6, stall_ns/1e6, g_report_ns/1e6,
           g_wait_us/1000.0, (unsigned long)g_wait_n);
}

static void print_summary2(unsigned long long iters, unsigned long long t_stage,
                           unsigned long long t_poll0, unsigned long long t_poll1,
                           uint64_t max_gap, uint64_t max_gap_at, uint64_t stall_ns,
                           unsigned long nstalls, unsigned long long burst_capped,
                           unsigned long long max_deficit){
    WireTap t;
    wire_tap_snapshot(&t);
    printf("SUMMARY2 name=%s iters=%llu stage_ms=%.0f poll0_ms=%.0f poll1_ms=%.0f "
           "max_gap_ms=%.1f max_gap_at_s=%.2f stall_ms=%.0f nstalls=%lu "
           "burst_capped=%llu max_deficit=%llu "
           "tx_calls=%lld tx_wouldblock=%lld tx_reset=%lld tx_err=%lld tx_ms=%.0f "
           "rx_calls=%lld rx_would=%lld rx_reset=%lld rx_err=%lld rx_ms=%.0f "
           "txDATA=%lld txHB=%lld txNACK=%lld txOTH=%lld "
           "rxDATA=%lld rxHB=%lld rxNACK=%lld rxOTH=%lld "
           "txD_probe=%lld txD_load=%lld rxD_probe=%lld rxD_load=%lld\n",
           g_name, iters, t_stage/1e6, t_poll0/1e6, t_poll1/1e6,
           max_gap/1e6, max_gap_at/1e9, stall_ns/1e6, nstalls,
           burst_capped, max_deficit,
           t.tx_calls, t.tx_would_block, t.tx_reset, t.tx_error, t.tx_ns/1e6,
           t.rx_calls, t.rx_would_block, t.rx_reset, t.rx_error, t.rx_ns/1e6,
           t.tx_type[WIRE_TAP_DATA], t.tx_type[WIRE_TAP_HB], t.tx_type[WIRE_TAP_NACK], t.tx_type[0],
           t.rx_type[WIRE_TAP_DATA], t.rx_type[WIRE_TAP_HB], t.rx_type[WIRE_TAP_NACK], t.rx_type[0],
           t.tx_data_topic[CH_PROBE&3], t.tx_data_topic[CH_LOAD&3],
           t.rx_data_topic[CH_PROBE&3], t.rx_data_topic[CH_LOAD&3]);
}

static void trace_peers(RantNode *n, uint16_t domain){
    RantIter it; RantPeerInfo p;
    memset(&it, 0, sizeof it);
    printf("TRACE node domain=%u\n", domain);
    while (rant_node_peers_next(n, &it, &p))
        printf("TRACE peer id=%u addr=%.*s\n", p.id, (int)p.address.len, p.address.data);
}

int node_main(int argc, char **argv){
    uint16_t domain;
    int load_hz, duration_s, if_mode, reliable, block_ms, i;
    const char *if_ip, *peer_ip;
    static char xnames[XCH_MAX][8];   /* "x0" to "x511", the extra topics */
    RantAllocator alloc = rant_allocator_heap(0);
    RantNodeOpts opts;
    RantTopicOpts co;
    RantAddr seed;
    RantNode *n;

    if (argc>1) g_name = argv[1];
    domain     = (uint16_t)(argc>2 ? atoi(argv[2]) : LAT_DOMAIN);
    load_hz    = (argc>3 ? atoi(argv[3]) : 0);
    duration_s = (argc>4 ? atoi(argv[4]) : 0);
    if_mode    = (argc>5 ? atoi(argv[5]) : 0);   /* discovery interface: 1 loopback, 2 NIC */
    reliable   = (argc>6 ? atoi(argv[6]) : 0);
    block_ms   = (argc>7 ? atoi(argv[7]) : 0);
    g_xch      = (argc>8 ? atoi(argv[8]) : 0);
    g_spread   = (argc>9 ? atoi(argv[9]) : 0);
    if_ip      = (argc>10 && strcmp(argv[10],"0")) ? argv[10] : NULL;
    peer_ip    = (argc>11 && strcmp(argv[11],"0")) ? argv[11] : NULL;
    if (g_xch < 0) g_xch = 0;
    if (g_xch > XCH_MAX) g_xch = XCH_MAX;

    { uint64_t s = bench_now_ns();
      g_tag = (uint32_t)(s ^ (s>>32) ^ ((uint32_t)bench_getpid() << 16)); }

    /* disable_shm: the sweep measures the UDP path, so same host peers must not switch to
       shared memory */
    memset(&opts, 0, sizeof opts);
    opts.domain = domain;
    opts.disable_shm = 1;
    opts.discovery.max_peers = MAX_PEERS;
    opts.max_topics = (uint16_t)(2 + g_xch);
    if (!if_ip && if_mode == 1) if_ip = "127.0.0.1";   /* loopback only, off the LAN */
    bench_net(&opts, if_ip, peer_ip, &seed);
    { const char *rb = getenv("RANT_DIAG_RCVBUF"), *sb = getenv("RANT_DIAG_SNDBUF");
      if (rb) opts.net.recv_buffer_bytes = (uint32_t)atoi(rb);
      if (sb) opts.net.send_buffer_bytes = (uint32_t)atoi(sb);
      if (rb || sb) printf("[%s] buffer override rcvbuf=%u sndbuf=%u\n",
                           g_name, opts.net.recv_buffer_bytes, opts.net.send_buffer_bytes); }
    if (getenv("RANT_DIAG_TRACE")) wire_tap_write(&wire_tap.trace_left, 24);

    n = rant_node_open(&alloc, g_name, lat_on_message, lat_on_event, &opts);
    if (!n){ fprintf(stderr, "[%s] rant_node_open failed\n", g_name); return 1; }

    /* the probe depth must cover the pongs one tick can stage, one per peer, or the ring
       evicts them before the flush and RTT samples are lost */
    memset(&co, 0, sizeof co);
    co.qos.reliability = RANT_BEST_EFFORT; co.qos.keep_last = 16; co.qos.max_message_bytes = 64;
    g_topics[CH_PROBE] = rant_node_create_topic(n, "probe", RANT_PUBSUB, NULL, &co);
    co.qos.reliability = reliable ? RANT_RELIABLE : RANT_BEST_EFFORT;
    co.qos.keep_last = LOAD_DEPTH;
    co.qos.backpressure_wait_us = (uint32_t)(block_ms > 0 ? block_ms : 0) * 1000u;
    g_topics[CH_LOAD] = rant_node_create_topic(n, "load", RANT_PUBSUB, NULL, &co);
    co.qos.keep_last = g_spread ? 128 : 1;   /* extra topics: idle (depth 1) or load bearing */
    for (i=0;i<g_xch;i++){
        sprintf(xnames[i], "x%d", i);
        g_topics[XCH_ID0+i] = rant_node_create_topic(n, xnames[i],
                                  g_spread==2 ? RANT_PUB_ONLY : RANT_PUBSUB, NULL, &co);
    }
    for (i=0;i<2+g_xch;i++){
        if (!g_topics[i] || rant_topic_index(g_topics[i]) != i){
            fprintf(stderr, "[%s] topic %d create failed\n", g_name, i);
            rant_node_close(n, 0);
            return 1;
        }
    }

    printf("[%s] up (domain %u, tag %08x, load %d Hz, %ds, %s load, block %d ms, +%d ch %s)\n",
           g_name, domain, g_tag, load_hz, duration_s,
           reliable ? "reliable" : "best-effort", block_ms,
           g_xch, g_spread==2 ? "void" : g_spread ? "spread" : "idle");

    {
    uint64_t start = bench_now_ns(), last_ping = start, last_report = start;
    static uint32_t load_seq_ch[1+XCH_MAX];
    uint32_t ping_seq = 0;
    int rr = 0;                  /* spread round robin cursor */
    unsigned long load_sent = 0, load_forgiven = 0;
    int wait_ms = (load_hz > 0) ? 1 : 20;   /* never block long while loading */
    /* a loop gap far beyond wait_ms means we lost the CPU. Voluntary backpressure waits are
       subtracted and reported separately as blk_wait_ms */
    uint64_t prev_iter = 0, max_gap = 0, stall_ns = 0, max_gap_at = 0;
    uint64_t prev_wait_us = 0;
    unsigned long nstalls = 0;
    unsigned long long iters = 0, t_stage = 0, t_poll0 = 0, t_poll1 = 0;
    unsigned long long burst_capped = 0, max_deficit = 0;
    int traced_peers = 0;

    for (;;){
        uint64_t now = bench_now_ns(), ph;
        uint8_t o[64];
        iters++;
        { uint64_t wait_us;
          rant_node_backpressure_stats(n, &wait_us, NULL);
          if (prev_iter){
              uint64_t gap = now - prev_iter;
              uint64_t waited = (wait_us - prev_wait_us) * 1000u;
              gap = (gap > waited) ? gap - waited : 0;
              if (gap > max_gap){ max_gap = gap; max_gap_at = now - start; }
              if (gap > STALL_GAP_NS){ stall_ns += gap; nstalls++; }
          }
          prev_wait_us = wait_us; }
        prev_iter = now;

        for (i=0;i<g_pong_n;i++){
            build_probe(o, PROBE_PONG, g_pong_q[i].tag, g_pong_q[i].seq, g_pong_q[i].t, g_name);
            rant_topic_send(g_topics[CH_PROBE], rant_bytes(o, PROBE_LEN), NULL);
        }
        g_pong_n = 0;

        if (now - last_ping >= PING_INTERVAL_NS){
            last_ping = now;
            build_probe(o, PROBE_PING, g_tag, ping_seq++, bench_now_ns(), g_name);
            rant_topic_send(g_topics[CH_PROBE], rant_bytes(o, PROBE_LEN), NULL);
        }

        if (load_hz > 0){
            double elapsed = (now - start)/1e9;
            unsigned long want = (unsigned long)(elapsed * (double)load_hz);
            unsigned long done = load_sent + load_forgiven;
            int burst = 0;
            if (want > done && (unsigned long long)(want - done) > max_deficit)
                max_deficit = want - done;
            /* forgive backlog older than about 250 ms: repaying a stall as one burst from every
               node overflows the receive buffers of peers and reads as drops (spec/testing.md) */
            { unsigned long slack = (unsigned long)load_hz/4 + 1;
              if (want > done + slack){ load_forgiven += want - slack - done; done = want - slack; } }
            ph = bench_now_ns();
            while (done + burst < want && burst < LOAD_BURST_MAX){
                int idx = 0, chid = CH_LOAD;
                if (g_spread && g_xch){
                    idx = rr; rr = (rr+1)%(1+g_xch);
                    if (idx) chid = XCH_ID0+idx-1;
                }
                build_load(o, load_seq_ch[idx]++);
                rant_topic_send(g_topics[chid], rant_bytes(o, LOAD_LEN), NULL);
                load_sent++; burst++;
            }
            t_stage += bench_now_ns() - ph;
            if (burst >= LOAD_BURST_MAX) burst_capped++;
        }

        ph = bench_now_ns(); rant_node_poll(n, 0);       t_poll0 += bench_now_ns() - ph;  /* flush */
        ph = bench_now_ns(); rant_node_poll(n, wait_ms); t_poll1 += bench_now_ns() - ph;  /* wait */

        if (wire_tap_read(&wire_tap.trace_left) > 0 && !traced_peers
            && bench_now_ns() - start > 3000000000ull){
            traced_peers = 1;
            trace_peers(n, domain);
        }

        now = bench_now_ns();
        /* periodic reports only when running forever: stdout is unbuffered, so a blocked write
           on an unpumped pipe freezes the node for seconds. Timed runs need only the SUMMARY */
        if (duration_s == 0 && now - last_report >= REPORT_INTERVAL_NS){
            last_report = now;
            print_report();
            g_report_ns += bench_now_ns() - now;
        }
        if (duration_s > 0 && (now - start) >= (uint64_t)duration_s*1000000000ull){
            rant_node_backpressure_stats(n, &g_wait_us, &g_wait_n);
            print_summary(now - start, load_hz, reliable, block_ms,
                          load_sent, load_forgiven, max_gap, stall_ns);
            print_summary2(iters, t_stage, t_poll0, t_poll1, max_gap, max_gap_at, stall_ns,
                           nstalls, burst_capped, max_deficit);
            break;
        }
    }
    }
    rant_node_close(n, 1);     /* BYE: peers drop us now instead of after the timeout */
    return 0;
}
