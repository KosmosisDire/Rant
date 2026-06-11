/* middleware test & diagnostic CLI.
 *
 * Subcommands: node (full-mesh latency/throughput node), sweep (spawns node
 * children per rate, aggregates into a table), selftest (on_gap + backpressure
 * functional test), sendbench (UDP send-cost microbench, Windows-only).
 *
 * Built against single-header dist/dart.h with DART_IMPLEMENTATION, so the diag
 * sendto/recvfrom wrappers below can intercept the transport's syscalls.
 *   build: run tools/pack first to generate dist/, then
 *     POSIX  : cc  -std=c99 -Wall -Idist tests/dart_test.c -o dart_test
 *     Windows: gcc -std=c99 -Wall -Idist tests/dart_test.c -o dart_test.exe -lws2_32 -lbcrypt -lwinmm
 */
#if !defined(_WIN32)
  #ifndef _POSIX_C_SOURCE
  #define _POSIX_C_SOURCE 200809L
  #endif
  #ifndef _DEFAULT_SOURCE
  #define _DEFAULT_SOURCE 1
  #endif
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef _WIN32
  #include <unistd.h>      /* fork, execvp, getpid */
  #include <sys/wait.h>    /* waitpid              */
  #include <signal.h>      /* kill                 */
  #include <time.h>        /* nanosleep            */
#endif

/* Syscall instrumentation (Windows): wraps the transport's sendto/recvfrom.
 * Two QPC reads per syscall are cheap enough to keep always on. On POSIX the
 * wrappers are absent and the counters stay zero. */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <timeapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif
#endif

static unsigned long long g_tx_calls, g_tx_wouldblock, g_tx_reset, g_tx_err, g_tx_ticks;
static unsigned long long g_rx_calls, g_rx_would,      g_rx_reset, g_rx_err, g_rx_ticks;
static unsigned long long g_tx_type[5], g_rx_type[5];   /* [0]=other/disc, 1=DATA 2=HB 3=NACK 4=GAP */
static unsigned long long g_tx_data_ch[4], g_rx_data_ch[4];
static int g_trace = 0, g_trace_left = 24;

#ifdef _WIN32
static LARGE_INTEGER g_qpf;

static void diag_classify(const char *b, int len, unsigned long long *types,
                          unsigned long long *data_ch){
    /* datagrams may carry several concatenated submessages; count each */
    int off = 0, counted = 0;
    while (len - off >= 4){
        unsigned t = (unsigned char)b[off];
        int sub;
        if (t == 1){
            if (len - off < 22) break;
            sub = 22 + ((unsigned char)b[off+20] | ((unsigned)(unsigned char)b[off+21] << 8));
        }
        else if (t == 2) sub = 24;
        else if (t == 3) sub = 18;
        else if (t == 4) sub = 20;
        else break;                       /* discovery/unknown datagram */
        if (sub > len - off) break;
        types[t]++; counted = 1;
        if (t == 1){
            unsigned ch = (unsigned char)b[off+2] | ((unsigned)(unsigned char)b[off+3] << 8);
            data_ch[ch & 3]++;
        }
        off += sub;
    }
    if (!counted) types[0]++;
}

static int diag_sendto(SOCKET s, const char *buf, int len, int flags,
                       const struct sockaddr *to, int tolen){
    LARGE_INTEGER a, b; int r;
    QueryPerformanceCounter(&a);
    r = sendto(s, buf, len, flags, to, tolen);
    QueryPerformanceCounter(&b);
    g_tx_ticks += (unsigned long long)(b.QuadPart - a.QuadPart);
    g_tx_calls++;
    if (r < 0){
        int e = WSAGetLastError();
        if (e == WSAEWOULDBLOCK) g_tx_wouldblock++;
        else if (e == WSAECONNRESET) g_tx_reset++;
        else g_tx_err++;
    } else diag_classify(buf, len, g_tx_type, g_tx_data_ch);
    return r;
}

static int diag_recvfrom(SOCKET s, char *buf, int len, int flags,
                         struct sockaddr *from, int *fromlen){
    LARGE_INTEGER a, b; int r;
    QueryPerformanceCounter(&a);
    r = recvfrom(s, buf, len, flags, from, fromlen);
    QueryPerformanceCounter(&b);
    g_rx_ticks += (unsigned long long)(b.QuadPart - a.QuadPart);
    g_rx_calls++;
    if (r < 0){
        int e = WSAGetLastError();
        if (e == WSAEWOULDBLOCK) g_rx_would++;
        else if (e == WSAECONNRESET) g_rx_reset++;
        else g_rx_err++;
    } else {
        diag_classify(buf, r, g_rx_type, g_rx_data_ch);
        if (g_trace && g_trace_left > 0 && r >= 1 && (unsigned char)buf[0] <= 4){
            struct sockaddr_in *si = (struct sockaddr_in*)from;
            printf("TRACE rx fd=%u src=%s:%u len=%d type=%u\n",
                   (unsigned)s, inet_ntoa(si->sin_addr), ntohs(si->sin_port),
                   r, (unsigned char)buf[0]);
            g_trace_left--;
        }
    }
    return r;
}

#define sendto   diag_sendto
#define recvfrom diag_recvfrom
#endif /* _WIN32 */

#define DART_IMPLEMENTATION
#include "dart.h"   /* discovery + transport + node runtime */

#ifdef _WIN32
#undef sendto
#undef recvfrom
#endif

/* ===================== shared helpers =================================== */
static uint64_t now_ns(void){
#ifdef _WIN32
    static LARGE_INTEGER f; LARGE_INTEGER c; uint64_t cc, ff;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    cc = (uint64_t)c.QuadPart; ff = (uint64_t)f.QuadPart;
    return (cc / ff) * 1000000000ull + (cc % ff) * 1000000000ull / ff;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static void put32(uint8_t *p, uint32_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static void put64(uint8_t *p, uint64_t v){ int i; for(i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }
static uint32_t get32(const uint8_t *p){ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static uint64_t get64(const uint8_t *p){ uint64_t v=0; int i; for(i=0;i<8;i++) v|=((uint64_t)p[i])<<(8*i); return v; }

/* ======================= node: latency/throughput ======================= */

#define CH_PROBE          1
#define CH_LOAD           2
#define LAT_DOMAIN        11
#define PING_INTERVAL_NS  250000000ull    /* probe ping cadence: 250 ms       */
#define REPORT_INTERVAL_NS 2000000000ull  /* human report cadence: 2 s        */
#define RTT_SANITY_NS     2000000000ull
#define STALL_GAP_NS      50000000ull     /* loop gap counted as a stall      */
#define LOAD_DEPTH        2048            /* load history ring (>= burst)     */
#define LOAD_BURST_MAX    2048            /* cap load msgs per loop iteration */
#define MAX_PEERS         64
#define NAME_LEN          16

/* probe (PING/PONG): [0]'L' [1]type [2..5]tag [6..9]seq [10..17]t_send [18..33]name */
#define PROBE_MAGIC 'L'
#define PROBE_PING  1
#define PROBE_PONG  2
#define PROBE_LEN   34
/* load: [0]'D' [1..4]seq [5..47]filler */
#define LOAD_MAGIC  'D'
#define LOAD_LEN    48

static const char *g_name = "node";
static uint32_t    g_tag  = 0;
static uint64_t    g_report_ns = 0;   /* wall time spent inside print_report */

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

/* per-peer stats, keyed by transport peer id */
typedef struct {
    int      used;
    uint32_t peer_id;
    char     name[NAME_LEN+1];
    /* probe / RTT */
    uint64_t last_rtt_ns, min_rtt_ns;
    double   ewma_ns;
    double   jit_ns;             /* RFC3550-style smoothed |delta RTT| */
    unsigned long rtt_n;
    /* load / drops */
    int      load_init;
    uint32_t load_expect;
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

/* deferred pong queue (no dart_* calls from on_sample) */
typedef struct { uint32_t tag, seq; uint64_t t; } pong_req;
static pong_req g_pong_q[512];
static int      g_pong_n = 0;

/* transport-reported permanent skips (KEEP_LAST supersession on reliable
 * channels, detected wire loss on best-effort). */
static unsigned long      g_gap_evt = 0;
static unsigned long long g_gap_tus = 0;
/* backpressure cost, read from the node before SUMMARY */
static uint64_t g_blk_us = 0;
static uint32_t g_blk_n  = 0;

static void lat_on_gap(void *u, uint16_t ch, uint32_t from, uint64_t first, uint64_t count){
    (void)u;(void)ch;(void)from;(void)first;
    g_gap_evt++; g_gap_tus += count;
}

static void lat_on_sample(void *u, uint16_t ch, uint32_t from, const void *data, size_t len){
    const uint8_t *p = (const uint8_t*)data; uint64_t now = now_ns();
    peer_stat *e = tbl_get(from);
    (void)u;
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
                if (e->rtt_n > 0){   /* RFC3550: J += (|D| - J)/16 */
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
    } else if (ch==CH_LOAD){
        uint32_t seq;
        if (len < 5 || p[0]!=LOAD_MAGIC) return;
        seq = get32(p+1);
        if (!e->load_init){ e->load_init=1; e->load_expect=seq+1; e->load_recv=1; }
        else if (seq >= e->load_expect){
            e->load_drops += (seq - e->load_expect);   /* gap = dropped */
            e->load_expect = seq+1;
            e->load_recv++;
        } else {
            e->load_recv++;                             /* reorder/dup */
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
           g_blk_us/1000.0, (unsigned long)g_blk_n);
}

static int node_main(int argc, char **argv){
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef _WIN32
    timeBeginPeriod(1);   /* avoid ~15.6ms idle-wakeup latency on Windows */
    QueryPerformanceFrequency(&g_qpf);
#endif
    if (argc>1) g_name = argv[1];
    uint16_t domain = (uint16_t)(argc>2 ? atoi(argv[2]) : LAT_DOMAIN);
    int load_hz     = (argc>3 ? atoi(argv[3]) : 0);
    int duration_s  = (argc>4 ? atoi(argv[4]) : 0);
    int use_mcast   = (argc>5 ? atoi(argv[5]) : 0);
    int reliable    = (argc>6 ? atoi(argv[6]) : 0);
    int block_ms    = (argc>7 ? atoi(argv[7]) : 0);

    { uint64_t s = now_ns();
      g_tag = (uint32_t)(s ^ (s>>32) ^ ((uint32_t)
#ifdef _WIN32
              GetCurrentProcessId()
#else
              getpid()
#endif
              << 16)); }

    dart_channel_def ch[2];
    memset(ch, 0, sizeof ch);
    ch[0].channel_id = CH_PROBE;
    /* depth must cover the pongs one tick can stage (one per peer) or the ring
       evicts them before the flush and RTT samples are lost */
    ch[0].qos.reliability = DART_BEST_EFFORT; ch[0].qos.history_depth = 16; ch[0].qos.max_sample_bytes = 64;
    ch[1].channel_id = CH_LOAD;
    ch[1].qos.reliability = reliable ? DART_RELIABLE : DART_BEST_EFFORT;
    ch[1].qos.history_depth = LOAD_DEPTH; ch[1].qos.max_sample_bytes = 64;
    ch[1].qos.max_block_us = (uint32_t)(block_ms > 0 ? block_ms : 0) * 1000u;
    ch[0].mcast = ch[1].mcast = (uint8_t)(use_mcast ? 1 : 0);

    dart_node_config cfg; memset(&cfg, 0, sizeof cfg);
    cfg.domain_id  = domain;
    cfg.data_port  = 0;
    cfg.channels   = ch;
    cfg.n_channels = 2;
    cfg.on_sample  = lat_on_sample;
    cfg.on_gap     = lat_on_gap;
    if (use_mcast)
        cfg.mcast_if = "127.0.0.1";   /* single-host test: stay off the NIC */
    { const char *rb = getenv("DART_DIAG_RCVBUF"), *sb = getenv("DART_DIAG_SNDBUF");
      if (rb) cfg.so_rcvbuf = (uint32_t)atoi(rb);
      if (sb) cfg.so_sndbuf = (uint32_t)atoi(sb);
      if (rb || sb) printf("[%s] buffer override rcvbuf=%u sndbuf=%u\n",
                           g_name, cfg.so_rcvbuf, cfg.so_sndbuf); }
    g_trace = (getenv("DART_DIAG_TRACE") != NULL);

    static uint8_t mem[4<<20];   /* deep load ring needs the headroom */
    dart_node *n = dart_node_open(mem, sizeof mem, &cfg);
    if (!n){ fprintf(stderr, "[%s] dart_node_open failed\n", g_name); return 1; }

    printf("[%s] up (domain %u, tag %08x, load %d Hz, %ds, %s, %s load, block %d ms)\n",
           g_name, domain, g_tag, load_hz, duration_s,
           use_mcast ? "multicast" : "unicast",
           reliable ? "reliable" : "best-effort", block_ms);

    uint64_t start = now_ns(), last_ping = start, last_report = start;
    uint32_t ping_seq = 0, load_seq = 0;
    unsigned long load_sent = 0, load_forgiven = 0;
    int wait_ms = (load_hz > 0) ? 1 : 20;   /* don't block long while loading */
    /* stall detection: a loop gap far beyond wait_ms means we lost the CPU, so
       big stalls make a run suspect. Voluntary backpressure waits
       (qos.max_block_us) are subtracted so stall numbers mean INVOLUNTARY loss;
       voluntary time is reported separately as blk_wait_ms. */
    uint64_t prev_iter = 0, max_gap = 0, stall_ns = 0, max_gap_at = 0;
    uint64_t prev_blk_us = 0;
    unsigned long nstalls = 0;
    /* loop phase timers + load pacing diagnostics (SUMMARY2) */
    unsigned long long iters = 0, t_stage = 0, t_poll0 = 0, t_poll1 = 0;
    unsigned long long burst_capped = 0, max_deficit = 0;
    int traced_peers = 0;

    for (;;){
        uint64_t now = now_ns(), ph;
        uint8_t o[64];
        int i;
        iters++;
        { uint64_t blk_us;
          dart_node_block_stats(n, &blk_us, NULL);
          if (prev_iter){
              uint64_t gap = now - prev_iter;
              uint64_t blk = (blk_us - prev_blk_us) * 1000u;   /* us to ns */
              gap = (gap > blk) ? gap - blk : 0;     /* voluntary waits out */
              if (gap > max_gap){ max_gap = gap; max_gap_at = now - start; }
              if (gap > STALL_GAP_NS){ stall_ns += gap; nstalls++; }
          }
          prev_blk_us = blk_us; }
        prev_iter = now;

        for (i=0;i<g_pong_n;i++){
            build_probe(o, PROBE_PONG, g_pong_q[i].tag, g_pong_q[i].seq, g_pong_q[i].t, g_name);
            dart_node_send(n, CH_PROBE, o, PROBE_LEN);
        }
        g_pong_n = 0;

        if (now - last_ping >= PING_INTERVAL_NS){
            last_ping = now;
            build_probe(o, PROBE_PING, g_tag, ping_seq++, now_ns(), g_name);
            dart_node_send(n, CH_PROBE, o, PROBE_LEN);
        }

        if (load_hz > 0){
            double elapsed = (now - start)/1e9;
            unsigned long want = (unsigned long)(elapsed * (double)load_hz);
            unsigned long done = load_sent + load_forgiven;
            int burst = 0;
            if (want > done && (unsigned long long)(want - done) > max_deficit)
                max_deficit = want - done;
            /* forgive backlog older than ~250 ms: repaying a long stall as one
               mega-burst from every node overflows peers' RX buffers and reads
               as drops. Forgiven samples never consume a seq, so receivers see
               no gap, and the sent_hz dip still reports the stall. */
            { unsigned long slack = (unsigned long)load_hz/4 + 1;
              if (want > done + slack){ load_forgiven += want - slack - done; done = want - slack; } }
            ph = now_ns();
            while (done + burst < want && burst < LOAD_BURST_MAX){
                build_load(o, load_seq++);
                dart_node_send(n, CH_LOAD, o, LOAD_LEN);
                load_sent++; burst++;
            }
            t_stage += now_ns() - ph;
            if (burst >= LOAD_BURST_MAX) burst_capped++;
        }

        ph = now_ns(); dart_node_poll(n, 0);       t_poll0 += now_ns() - ph;  /* flush */
        ph = now_ns(); dart_node_poll(n, wait_ms); t_poll1 += now_ns() - ph;  /* wait  */

        if (g_trace && !traced_peers && now_ns() - start > 3000000000ull){
            traced_peers = 1;
            printf("TRACE node fd=%u mcfd=%u domain=%u mc_port=%u\n",
                   (unsigned)n->fd, (unsigned)n->mcfd, n->domain, n->mc_port);
            for (i = 0; i < (int)n->max_peers; i++)
                if (n->peers[i].used)
                    printf("TRACE peer id=%u addr=%u.%u.%u.%u:%u\n", n->peers[i].id,
                           n->peers[i].ip[0], n->peers[i].ip[1], n->peers[i].ip[2],
                           n->peers[i].ip[3], n->peers[i].port);
        }

        now = now_ns();
        /* periodic reports only in run-forever mode: stdout is unbuffered, so a
           blocked write (e.g. a PowerShell redirect pipe nobody pumps) freezes
           the node for seconds. Timed runs only need the SUMMARY. */
        if (duration_s == 0 && now - last_report >= REPORT_INTERVAL_NS){
            last_report = now;
            print_report();
            g_report_ns += now_ns() - now;
        }
        if (duration_s > 0 && (now - start) >= (uint64_t)duration_s*1000000000ull){
            dart_node_block_stats(n, &g_blk_us, &g_blk_n);
            print_summary(now - start, load_hz, reliable, block_ms,
                          load_sent, load_forgiven, max_gap, stall_ns);
#ifdef _WIN32
            { double f = (double)g_qpf.QuadPart;
#else
            { double f = 1.0;
#endif
              printf("SUMMARY2 name=%s iters=%llu stage_ms=%.0f poll0_ms=%.0f poll1_ms=%.0f "
                     "max_gap_ms=%.1f max_gap_at_s=%.2f stall_ms=%.0f nstalls=%lu "
                     "burst_capped=%llu max_deficit=%llu "
                     "tx_calls=%llu tx_wouldblock=%llu tx_reset=%llu tx_err=%llu tx_ms=%.0f "
                     "rx_calls=%llu rx_would=%llu rx_reset=%llu rx_err=%llu rx_ms=%.0f "
                     "txDATA=%llu txHB=%llu txNACK=%llu txGAP=%llu txOTH=%llu "
                     "rxDATA=%llu rxHB=%llu rxNACK=%llu rxGAP=%llu rxOTH=%llu "
                     "txD_probe=%llu txD_load=%llu rxD_probe=%llu rxD_load=%llu\n",
                     g_name, iters, t_stage/1e6, t_poll0/1e6, t_poll1/1e6,
                     max_gap/1e6, max_gap_at/1e9, stall_ns/1e6, nstalls,
                     burst_capped, max_deficit,
                     g_tx_calls, g_tx_wouldblock, g_tx_reset, g_tx_err, g_tx_ticks*1000.0/f,
                     g_rx_calls, g_rx_would, g_rx_reset, g_rx_err, g_rx_ticks*1000.0/f,
                     g_tx_type[1], g_tx_type[2], g_tx_type[3], g_tx_type[4], g_tx_type[0],
                     g_rx_type[1], g_rx_type[2], g_rx_type[3], g_rx_type[4], g_rx_type[0],
                     g_tx_data_ch[CH_PROBE&3], g_tx_data_ch[CH_LOAD&3],
                     g_rx_data_ch[CH_PROBE&3], g_rx_data_ch[CH_LOAD&3]); }
            break;
        }
    }
    dart_node_close(n, 1);   /* BYE: peers drop us now instead of after timeout */
    return 0;
}

/* ===================== sendbench: UDP send-cost microbench =============== */
#ifdef _WIN32
#define SB_N 200000

static double sb_end(LARGE_INTEGER a, int n){
    LARGE_INTEGER b, f;
    QueryPerformanceCounter(&b); QueryPerformanceFrequency(&f);
    return (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)f.QuadPart / n;
}

static int sendbench_main(void){
    WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
    char buf[1024]; memset(buf, 0xAB, sizeof buf);
    LARGE_INTEGER t0;
    int i, rb = 8<<20;

    /* sink socket on a known port, big rcvbuf, never read */
    SOCKET sink = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(47123);
    setsockopt(sink, SOL_SOCKET, SO_RCVBUF, (const char*)&rb, sizeof rb);
    if (bind(sink, (struct sockaddr*)&sa, sizeof sa) != 0){ printf("bind fail\n"); return 1; }

    SOCKET tx = socket(AF_INET, SOCK_DGRAM, 0);

    QueryPerformanceCounter(&t0);
    for (i=0;i<SB_N;i++) sendto(tx, buf, 70, 0, (struct sockaddr*)&sa, sizeof sa);
    printf("unconnected sendto 70B  : %.2f us/call\n", sb_end(t0, SB_N));

    { SOCKET txc = socket(AF_INET, SOCK_DGRAM, 0);
      connect(txc, (struct sockaddr*)&sa, sizeof sa);
      QueryPerformanceCounter(&t0);
      for (i=0;i<SB_N;i++) send(txc, buf, 70, 0);
      printf("connected   send   70B  : %.2f us/call\n", sb_end(t0, SB_N));
      closesocket(txc); }

    QueryPerformanceCounter(&t0);
    for (i=0;i<SB_N;i++) sendto(tx, buf, 1024, 0, (struct sockaddr*)&sa, sizeof sa);
    printf("unconnected sendto 1024B: %.2f us/call\n", sb_end(t0, SB_N));

    /* multicast send cost vs number of local subscriber sockets. Windows
       charges the SENDER per local fan-out delivery, so this scales with
       joiner count. */
    { int joiners[] = {0, 1, 5, 10};
      SOCKET js[10]; int nj = 0, k, ji;
      unsigned long lo = inet_addr("127.0.0.1");
      SOCKET txm = socket(AF_INET, SOCK_DGRAM, 0);
      struct sockaddr_in ma; memset(&ma, 0, sizeof ma);
      unsigned char ttl = 1, loop = 1;
      int onx = 1, nm = SB_N/10;
      ma.sin_family = AF_INET; ma.sin_addr.s_addr = inet_addr("239.255.0.99");
      ma.sin_port = htons(47124);
      setsockopt(txm, IPPROTO_IP, IP_MULTICAST_IF,   (const char*)&lo,   sizeof lo);
      setsockopt(txm, IPPROTO_IP, IP_MULTICAST_TTL,  (const char*)&ttl,  sizeof ttl);
      setsockopt(txm, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof loop);
      for (k=0;k<4;k++){
          while (nj < joiners[k]){
              struct sockaddr_in ja; struct ip_mreq mr;
              SOCKET j = socket(AF_INET, SOCK_DGRAM, 0);
              setsockopt(j, SOL_SOCKET, SO_REUSEADDR, (const char*)&onx, sizeof onx);
              setsockopt(j, SOL_SOCKET, SO_RCVBUF, (const char*)&rb, sizeof rb);
              memset(&ja,0,sizeof ja);
              ja.sin_family=AF_INET; ja.sin_addr.s_addr=htonl(INADDR_ANY);
              ja.sin_port=htons(47124);
              bind(j,(struct sockaddr*)&ja,sizeof ja);
              memset(&mr,0,sizeof mr);
              mr.imr_multiaddr.s_addr=inet_addr("239.255.0.99");
              mr.imr_interface.s_addr=lo;
              setsockopt(j,IPPROTO_IP,IP_ADD_MEMBERSHIP,(const char*)&mr,sizeof mr);
              js[nj++]=j;
          }
          QueryPerformanceCounter(&t0);
          for (i=0;i<nm;i++) sendto(txm, buf, 70, 0, (struct sockaddr*)&ma, sizeof ma);
          printf("multicast   sendto 70B  : %.2f us/call  (%d local joiners)\n",
                 sb_end(t0, nm), joiners[k]);
      }
      for (ji=0;ji<nj;ji++) closesocket(js[ji]);
      closesocket(txm); }

    closesocket(tx); closesocket(sink);
    WSACleanup();
    return 0;
}
#endif /* _WIN32 */

/* ============ selftest: on_gap, backpressure, dynamic interest =========== *
 * Two nodes in ONE process (writer pub-only, reader sub-only) so the test
 * controls exactly when each side runs. Phases:
 *   1. JOIN     : reader joins mid-stream; on_gap must NOT fire.
 *   2. GAP      : writer stages a burst beyond history_depth without flushing,
 *                 so KEEP_LAST evicts and the reader gets one GAP plus the
 *                 surviving tail; on_gap count must equal the evicted span.
 *   3. BLOCKED  : on a max_block_us channel, sends that would evict un-acked
 *                 history wait ~max_block_us while the reader never acks, then
 *                 proceed (KEEP_LAST fallback, never refusal).
 *   4. RELEASED : same channel once the reader acks; sends are instant.
 *   5. DYNAMIC  : reader flips a channel inactive/subscribed at runtime;
 *                 each (re)subscribe replays cached history with no gap.
 *   6. SCALE    : 40 channels, past the old 31-id announce cap. */

#define ST_DOMAIN   33
#define ST_CH_GAP   1   /* reliable, depth 4, no backpressure  */
#define ST_CH_BLOCK 2   /* reliable, depth 4, max_block 100 ms */
#define ST_CH_DYN   3   /* reliable, depth 4, reader starts DART_NONE */
#define ST_DEPTH    4
#define ST_BLOCK_US 100000u
#define ST_NCH      40

static int st_fail = 0;
#define ST_CHECK(cond, ...) do { \
    printf((cond) ? "  ok   " : "  FAIL "); printf(__VA_ARGS__); printf("\n"); \
    if (!(cond)) st_fail = 1; } while (0)

static unsigned long st_samples[8], st_gap_calls[8], st_gap_tus[8], st_any;

static void st_on_sample(void *u, uint16_t ch, uint32_t from, const void *d, size_t n){
    (void)u;(void)from;(void)d;(void)n;
    if (ch < 8) st_samples[ch]++;
    st_any++;
}
static void st_on_gap(void *u, uint16_t ch, uint32_t from, uint64_t first, uint64_t count){
    (void)u;(void)from;(void)first;
    if (ch < 8){ st_gap_calls[ch]++; st_gap_tus[ch] += (unsigned long)count; }
}

static void st_pump(dart_node *a, dart_node *b, int ms){     /* run both nodes */
    uint64_t end = dart_now_us() + (uint64_t)ms*1000u;
    while (dart_now_us() < end){ dart_node_poll(a, 1); if (b) dart_node_poll(b, 0); }
}

static int selftest_main(void){
    static uint8_t mem_w[1<<20], mem_r[1<<20];
    uint8_t payload[32]; unsigned i;
    memset(payload, 0x5A, sizeof payload);

    dart_channel_def ch[3]; memset(ch, 0, sizeof ch);
    ch[0].channel_id = ST_CH_GAP;
    ch[0].qos.reliability = DART_RELIABLE; ch[0].qos.history_depth = ST_DEPTH;
    ch[0].qos.max_sample_bytes = 64; ch[0].qos.heartbeat_us = 50000;
    ch[1] = ch[0]; ch[1].channel_id = ST_CH_BLOCK; ch[1].qos.max_block_us = ST_BLOCK_US;
    ch[2] = ch[0]; ch[2].channel_id = ST_CH_DYN;
    ch[2].qos.join_replay = ST_DEPTH;   /* phase 5 asserts ring replay on join */

    dart_node_config wc; memset(&wc, 0, sizeof wc);
    wc.domain_id = ST_DOMAIN; wc.channels = ch; wc.n_channels = 3;
    { dart_node_config rc = wc; dart_channel_def chr[3]; dart_node *w, *r;
      memcpy(chr, ch, sizeof ch);
      ch[0].dir = ch[1].dir   = ch[2].dir  = DART_PUB_ONLY;
      chr[0].dir = chr[1].dir = DART_SUB_ONLY;
      chr[2].dir = DART_NONE;
      rc.channels = chr; rc.on_sample = st_on_sample; rc.on_gap = st_on_gap;

      w = dart_node_open(mem_w, sizeof mem_w, &wc);
      r = dart_node_open(mem_r, sizeof mem_r, &rc);
      if (!w || !r){ fprintf(stderr, "node open failed\n"); return 1; }

      /* 1. JOIN: writer streams while discovery completes; the reader must
            adopt the stream head silently (no gap for a late joiner) */
      { uint64_t end = dart_now_us() + 5000000u;
        while (st_samples[ST_CH_GAP]==0 && dart_now_us() < end){
            dart_node_send(w, ST_CH_GAP, payload, sizeof payload);
            st_pump(w, r, 20);
        } }
      ST_CHECK(st_samples[ST_CH_GAP] > 0, "join: reader receives (got %lu)", st_samples[ST_CH_GAP]);
      ST_CHECK(st_gap_calls[ST_CH_GAP] == 0, "join: no on_gap for late join (calls=%lu)", st_gap_calls[ST_CH_GAP]);

      /* 2. GAP: stage 50 samples with no flush in between; only the last
            ST_DEPTH survive, the rest must arrive as exactly one gap */
      st_pump(w, r, 200);                           /* settle acks */
      { unsigned long s0 = st_samples[ST_CH_GAP];
        for (i=0;i<50;i++) dart_node_send(w, ST_CH_GAP, payload, sizeof payload);
        st_pump(w, r, 500);
        ST_CHECK(st_gap_calls[ST_CH_GAP] == 1, "gap: one on_gap call (calls=%lu)", st_gap_calls[ST_CH_GAP]);
        ST_CHECK(st_gap_tus[ST_CH_GAP] == 50-ST_DEPTH, "gap: count == evicted span (%lu, want %u)",
                 st_gap_tus[ST_CH_GAP], 50-ST_DEPTH);
        ST_CHECK(st_samples[ST_CH_GAP]-s0 == ST_DEPTH, "gap: surviving tail delivered (%lu, want %u)",
                 st_samples[ST_CH_GAP]-s0, ST_DEPTH);
      }

      /* 3. BLOCKED: fill ST_CH_BLOCK's ring, then keep sending while the
            reader never runs: each eviction-send must wait ~ST_BLOCK_US */
      st_pump(w, r, 200);
      for (i=0;i<ST_DEPTH;i++) dart_node_send(w, ST_CH_BLOCK, payload, sizeof payload);
      st_pump(w, NULL, 50);                         /* flush; reader silent, no acks */
      { uint64_t t0 = dart_now_us(), dt;
        dart_node_send(w, ST_CH_BLOCK, payload, sizeof payload);   /* evicts un-acked */
        dt = dart_now_us() - t0;
        ST_CHECK(dt >= ST_BLOCK_US-10000 && dt < 4*ST_BLOCK_US,
                 "blocked: send waited ~max_block_us (%.1f ms)", dt/1000.0);
      }

      /* 4. RELEASED: let the reader catch up and ack; sends are instant */
      st_pump(w, r, 300);
      { uint64_t t0 = dart_now_us(), dt;
        dart_node_send(w, ST_CH_BLOCK, payload, sizeof payload);
        dt = dart_now_us() - t0;
        ST_CHECK(dt < 20000, "released: acked ring sends instantly (%.1f ms)", dt/1000.0);
        st_pump(w, r, 100);
        /* the blocked-phase eviction dropped only history already in flight to
           the reader's socket, so nothing was lost: every send must have been
           delivered and no gap reported */
        ST_CHECK(st_gap_calls[ST_CH_BLOCK] == 0 && st_samples[ST_CH_BLOCK] == ST_DEPTH+2,
                 "blocked: in-flight eviction loses nothing (gaps=%lu, samples=%lu/%u)",
                 st_gap_calls[ST_CH_BLOCK], st_samples[ST_CH_BLOCK], ST_DEPTH+2);
      }

      /* 5. DYNAMIC: ST_CH_DYN is inactive on the reader; subscribe replays
            the cached ring, unsubscribe goes silent at the writer, resubscribe
            replays again. All joins are gap-free. */
      st_pump(w, r, 200);
      for (i=0;i<3;i++) dart_node_send(w, ST_CH_DYN, payload, sizeof payload);
      st_pump(w, r, 300);
      ST_CHECK(st_samples[ST_CH_DYN] == 0, "dynamic: inactive receives nothing (%lu)",
               st_samples[ST_CH_DYN]);
      dart_node_set_dir(r, ST_CH_DYN, DART_SUB_ONLY);
      st_pump(w, r, 400);
      ST_CHECK(st_samples[ST_CH_DYN] == 3, "dynamic: subscribe replays cached history (%lu, want 3)",
               st_samples[ST_CH_DYN]);
      dart_node_send(w, ST_CH_DYN, payload, sizeof payload);
      st_pump(w, r, 300);
      ST_CHECK(st_samples[ST_CH_DYN] == 4, "dynamic: live sample delivered (%lu, want 4)",
               st_samples[ST_CH_DYN]);
      dart_node_set_dir(r, ST_CH_DYN, DART_NONE);
      st_pump(w, r, 300);                  /* let the new list reach the writer */
      for (i=0;i<5;i++) dart_node_send(w, ST_CH_DYN, payload, sizeof payload);
      st_pump(w, r, 300);
      ST_CHECK(st_samples[ST_CH_DYN] == 4, "dynamic: unsubscribed receives nothing (%lu)",
               st_samples[ST_CH_DYN]);
      dart_node_set_dir(r, ST_CH_DYN, DART_SUB_ONLY);
      st_pump(w, r, 400);
      ST_CHECK(st_samples[ST_CH_DYN] == 4+ST_DEPTH, "dynamic: resubscribe replays ring (%lu, want %u)",
               st_samples[ST_CH_DYN], 4+ST_DEPTH);
      ST_CHECK(st_gap_calls[ST_CH_DYN] == 0, "dynamic: joins are silent (gaps=%lu)",
               st_gap_calls[ST_CH_DYN]);

      dart_node_close(r, 1);
      dart_node_close(w, 1);
    }

    /* 6. SCALE: 40 channels, past the old 31-id announce cap; interest now
          rides the meta channel as one reliable sample */
    { static uint8_t mem_a[1<<20], mem_b[1<<20];
      static dart_channel_def cha[ST_NCH], chb[ST_NCH];
      dart_node_config ac, bc; dart_node *a, *b; uint16_t k;
      memset(cha, 0, sizeof cha);
      for (k=0;k<ST_NCH;k++){
          cha[k].channel_id = (uint16_t)(100+k);
          cha[k].qos.reliability = DART_RELIABLE;
          cha[k].qos.history_depth = 1;
          cha[k].qos.join_replay = 1;      /* sent before discovery completes */
          cha[k].qos.max_sample_bytes = 32;
          cha[k].qos.heartbeat_us = 50000;
          cha[k].dir = DART_PUB_ONLY;
      }
      memcpy(chb, cha, sizeof cha);
      for (k=0;k<ST_NCH;k++) chb[k].dir = DART_SUB_ONLY;
      memset(&ac, 0, sizeof ac);
      ac.domain_id = ST_DOMAIN+1; ac.channels = cha; ac.n_channels = ST_NCH;
      bc = ac; bc.channels = chb; bc.on_sample = st_on_sample; bc.on_gap = st_on_gap;
      a = dart_node_open(mem_a, sizeof mem_a, &ac);
      b = dart_node_open(mem_b, sizeof mem_b, &bc);
      ST_CHECK(a && b, "scale: %u-channel nodes open", ST_NCH);
      if (a && b){
          st_any = 0;
          for (k=0;k<ST_NCH;k++) dart_node_send(a, (uint16_t)(100+k), payload, 16);
          { uint64_t end = dart_now_us() + 5000000u;
            while (st_any < ST_NCH && dart_now_us() < end) st_pump(a, b, 20); }
          ST_CHECK(st_any == ST_NCH, "scale: all channels delivered (%lu/%u)", st_any, ST_NCH);
          dart_node_close(b, 1);
          dart_node_close(a, 1);
      }
    }
    printf(st_fail ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    return st_fail;
}

/* ===================== sweep: cross-platform latency sweep =============== *
 * Spawns N "dart_test node" children per rate (stdout to a temp file), waits
 * for their self-exit, and aggregates the SUMMARY (+SUMMARY2) lines into a
 * table. Pure C process orchestration: no shell, runs on Windows and POSIX. */

#define SW_MAX_NODES 64

typedef struct {
#ifdef _WIN32
    HANDLE h;
#else
    pid_t  pid;
#endif
    char   out[512];
    int    reaped;
} sw_child;

typedef struct { char keys[64][24]; char vals[64][40]; int n; } sw_kv;

#ifndef _WIN32
static void sw_sleep_ms(int ms){
    struct timespec ts; ts.tv_sec = ms/1000; ts.tv_nsec = (long)(ms%1000)*1000000L;
    nanosleep(&ts, NULL);
}
#endif

/* parse "10000" / "10k" / "1m" into an integer */
static long sw_num(const char *p){
    char *end; double v = strtod(p, &end);
    if (*end=='k'||*end=='K') v *= 1e3;
    else if (*end=='m'||*end=='M') v *= 1e6;
    return (long)v;
}

static void sw_kv_parse(const char *line, sw_kv *o){
    const char *p = line; o->n = 0;
    while (*p && o->n < 64){
        const char *start, *eq;
        while (*p==' '||*p=='\t'||*p=='\n'||*p=='\r') p++;
        if (!*p) break;
        start = p;
        while (*p && *p!=' ' && *p!='\t' && *p!='\n' && *p!='\r') p++;
        eq = start; while (eq < p && *eq != '=') eq++;
        if (eq < p){
            int kl=(int)(eq-start), vl=(int)(p-eq-1);
            if (kl>23) kl=23;
            if (vl>39) vl=39;
            memcpy(o->keys[o->n], start, (size_t)kl); o->keys[o->n][kl]=0;
            memcpy(o->vals[o->n], eq+1,  (size_t)vl); o->vals[o->n][vl]=0;
            o->n++;
        }
    }
}
static double sw_kv_get(const sw_kv *o, const char *key, double dflt){
    int i; for (i=0;i<o->n;i++) if (strcmp(o->keys[i],key)==0) return atof(o->vals[i]);
    return dflt;
}

/* read the LAST SUMMARY / SUMMARY2 line out of a child's output file */
static int sw_read_summary(const char *path, sw_kv *sum, sw_kv *sum2){
    FILE *f = fopen(path, "rb"); char line[4096]; int got = 0;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)){
        if (strncmp(line, "SUMMARY2 ", 9)==0) sw_kv_parse(line+9, sum2);
        else if (strncmp(line, "SUMMARY ", 8)==0){ sw_kv_parse(line+8, sum); got=1; }
    }
    fclose(f);
    return got;
}

static int sw_spawn(sw_child *c, const char *self, const char *name,
                    int domain, long rate, int dur, int mcast, int rel, int blk){
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa; STARTUPINFOA si; PROCESS_INFORMATION pi;
    HANDLE hout; char cmd[1024];
    memset(&sa, 0, sizeof sa); sa.nLength = sizeof sa; sa.bInheritHandle = TRUE;
    hout = CreateFileA(c->out, GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE,
                       &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hout == INVALID_HANDLE_VALUE) return -1;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hout; si.hStdError = hout;
    snprintf(cmd, sizeof cmd, "\"%s\" node %s %d %ld %d %d %d %d",
             self, name, domain, rate, dur, mcast, rel, blk);
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessA(self, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)){
        CloseHandle(hout); return -1;
    }
    CloseHandle(hout); CloseHandle(pi.hThread);
    c->h = pi.hProcess; c->reaped = 0;
    return 0;
#else
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0){
        char sd[16], sr[24], su[16], sm[8], se[8], sb[16];
        snprintf(sd,sizeof sd,"%d",domain);  snprintf(sr,sizeof sr,"%ld",rate);
        snprintf(su,sizeof su,"%d",dur);     snprintf(sm,sizeof sm,"%d",mcast);
        snprintf(se,sizeof se,"%d",rel);     snprintf(sb,sizeof sb,"%d",blk);
        if (!freopen(c->out, "w", stdout)) _exit(126);
        { char *av[] = { (char*)self, "node", (char*)name, sd, sr, su, sm, se, sb, NULL };
          execvp(self, av); }
        _exit(127);
    }
    c->pid = pid; c->reaped = 0;
    return 0;
#endif
}

static void sw_wait(sw_child *cs, int n, int timeout_ms){
    uint64_t deadline = now_ns() + (uint64_t)timeout_ms*1000000ull;
#ifdef _WIN32
    int i;
    for (i=0;i<n;i++){
        uint64_t now = now_ns();
        DWORD remain = (now < deadline) ? (DWORD)((deadline-now)/1000000ull) : 0;
        if (WaitForSingleObject(cs[i].h, remain) == WAIT_TIMEOUT)
            TerminateProcess(cs[i].h, 1);
        CloseHandle(cs[i].h); cs[i].reaped = 1;
    }
#else
    int left = n, i;
    while (left > 0 && now_ns() < deadline){
        int any = 0;
        for (i=0;i<n;i++){
            int st;
            if (cs[i].reaped) continue;
            if (waitpid(cs[i].pid, &st, WNOHANG) == cs[i].pid){ cs[i].reaped=1; left--; any=1; }
        }
        if (!any) sw_sleep_ms(5);
    }
    for (i=0;i<n;i++) if (!cs[i].reaped){ kill(cs[i].pid, SIGKILL); waitpid(cs[i].pid, NULL, 0); cs[i].reaped=1; }
#endif
}

/* aggregate helpers over an array of parsed SUMMARY kvsets */
static double sw_avg(const sw_kv *a, int n, const char *k){
    double s=0; int i,c=0; for (i=0;i<n;i++){ s+=sw_kv_get(&a[i],k,0); c++; } return c?s/c:0; }
static double sw_sum(const sw_kv *a, int n, const char *k){
    double s=0; int i; for (i=0;i<n;i++) s+=sw_kv_get(&a[i],k,0); return s; }
static double sw_max(const sw_kv *a, int n, const char *k){
    double m=0; int i; for (i=0;i<n;i++){ double v=sw_kv_get(&a[i],k,0); if(v>m)m=v; } return m; }

static int sweep_main(int argc, char **argv){
    int nodes=10, base_domain=20, dur=8, mcast=0, rel=0, blk=0, diag=0;
    long rates[64]; int nrates=0, i, ri;
    const char *self;
    static sw_child  cs[SW_MAX_NODES];
    static sw_kv     sum[SW_MAX_NODES], sum2[SW_MAX_NODES];
    char tmpdir[260]; unsigned long mypid;

    for (i=2;i<argc;i++){
        if (!strcmp(argv[i],"--nodes")    && i+1<argc) nodes=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--domain")  && i+1<argc) base_domain=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--duration")&& i+1<argc) dur=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--mcast")   && i+1<argc) mcast=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--block-ms")&& i+1<argc) blk=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--reliable")) rel=1;
        else if (!strcmp(argv[i],"--diag"))     diag=1;
        else if (!strcmp(argv[i],"--rates")   && i+1<argc){
            const char *p = argv[++i];
            nrates = 0;
            while (*p && nrates<64){
                rates[nrates++]=sw_num(p);
                while (*p && *p!=',') p++;
                if (*p==',') p++;
            }
        }
        else fprintf(stderr, "sweep: ignoring unknown option '%s'\n", argv[i]);
    }
    if (nrates==0){ long d[]={0,1000,10000,50000,100000}; for (i=0;i<5;i++) rates[i]=d[i]; nrates=5; }
    if (nodes>SW_MAX_NODES){ nodes=SW_MAX_NODES; fprintf(stderr,"sweep: capping nodes at %d\n",SW_MAX_NODES); }

#ifdef _WIN32
    { static char selfbuf[1024]; GetModuleFileNameA(NULL, selfbuf, sizeof selfbuf); self=selfbuf; }
    GetTempPathA(sizeof tmpdir, tmpdir);                 /* trailing backslash */
    mypid = (unsigned long)GetCurrentProcessId();
#else
    self = argv[0];
    { const char *td=getenv("TMPDIR"); if(!td||!*td) td="/tmp";
      snprintf(tmpdir,sizeof tmpdir,"%s/",td); }
    mypid = (unsigned long)getpid();
#endif

    printf("sweep: %d nodes, %ds each, rates", nodes, dur);
    for (ri=0;ri<nrates;ri++) printf(" %ld", rates[ri]);
    printf("%s%s%s\n\n", mcast?", multicast":"", rel?", reliable load":"",
           blk>0?" (block)":"");
    printf("%10s %12s %10s %10s %10s %8s %9s %9s %6s\n",
           "TargetHz","Achieved/n","RTTavg ms","RTTmin ms","Jitter ms",
           "Drop%","Block ms","Stall ms","Nodes");

    for (ri=0; ri<nrates; ri++){
        long rate = rates[ri];
        int domain = base_domain + ri;     /* distinct domain per rate run */
        int count = 0, peers_n = 0;
        double achieved, rttAvg=0, rttJit=0, rttMin=1e18, recv, drops, stall, blkMs, dpct;

        for (i=0;i<nodes;i++){
            char name[16];
            snprintf(cs[i].out, sizeof cs[i].out, "%smwsweep_%lu_r%ld_n%d.txt",
                     tmpdir, mypid, rate, i);
            snprintf(name, sizeof name, "n%d", i);
            sum[i].n = 0; sum2[i].n = 0;
            if (sw_spawn(&cs[i], self, name, domain, rate, dur, mcast, rel, blk) != 0){
                fprintf(stderr, "sweep: failed to spawn node %d\n", i);
                cs[i].reaped = 1;
            }
        }
        sw_wait(cs, nodes, (dur+10)*1000);

        /* collect: compact the parsed SUMMARYs into [0..count) of sum[]/sum2[] */
        { sw_kv s, s2;
          for (i=0;i<nodes;i++){
              s.n = 0; s2.n = 0;
              if (sw_read_summary(cs[i].out, &s, &s2)){
                  sum[count] = s; sum2[count] = s2; count++;
                  if (sw_kv_get(&s,"peers",0) > 0){
                      rttAvg += sw_kv_get(&s,"rtt_avg_ms",0);
                      rttJit += sw_kv_get(&s,"rtt_jit_ms",0);
                      peers_n++;
                  }
                  { double rm = sw_kv_get(&s,"rtt_min_ms",0); if (rm>0 && rm<rttMin) rttMin=rm; }
              }
              remove(cs[i].out);
          }
        }
        if (count==0){ printf("%10ld   (no SUMMARY captured)\n", rate); continue; }

        achieved = sw_avg(sum, count, "sent_hz");
        recv     = sw_sum(sum, count, "recv");
        drops    = sw_sum(sum, count, "drops");
        stall    = sw_max(sum, count, "stall_ms");
        blkMs    = sw_avg(sum, count, "blk_wait_ms");
        dpct     = (recv+drops)>0 ? 100.0*drops/(recv+drops) : 0.0;
        if (peers_n){ rttAvg/=peers_n; rttJit/=peers_n; } else { rttAvg=rttJit=0; }
        if (rttMin>1e17) rttMin=0;

        printf("%10ld %12d %10.3f %10.3f %10.3f %8.2f %9.0f %9.0f %4d/%d\n",
               rate, (int)(achieved+0.5), rttAvg, rttMin, rttJit, dpct, blkMs, stall,
               count, nodes);

        if (diag && count){
            double txDATA=sw_sum(sum2,count,"txDATA"), rxDATA=sw_sum(sum2,count,"rxDATA");
            double wire = txDATA>0 ? 100.0*(txDATA-rxDATA)/txDATA : 0.0;
            printf("    LOOP    iters/s %.0f  stage_ms %.0f  poll0_ms %.0f  poll1_ms %.0f  burst_capped %.1f  max_deficit %.0f\n",
                   sw_avg(sum2,count,"iters")/(dur>0?dur:1), sw_avg(sum2,count,"stage_ms"),
                   sw_avg(sum2,count,"poll0_ms"), sw_avg(sum2,count,"poll1_ms"),
                   sw_avg(sum2,count,"burst_capped"), sw_avg(sum2,count,"max_deficit"));
            printf("    SYSCALL tx %.0f/n (%.0f ms) wblock %.1f reset %.1f err %.1f   rx %.0f/n (%.0f ms) reset %.1f err %.1f\n",
                   sw_avg(sum2,count,"tx_calls"), sw_avg(sum2,count,"tx_ms"),
                   sw_avg(sum2,count,"tx_wouldblock"), sw_avg(sum2,count,"tx_reset"), sw_avg(sum2,count,"tx_err"),
                   sw_avg(sum2,count,"rx_calls"), sw_avg(sum2,count,"rx_ms"),
                   sw_avg(sum2,count,"rx_reset"), sw_avg(sum2,count,"rx_err"));
            printf("    TYPES   txDATA %.0f (probe %.0f / load %.0f) txGAP %.0f   rxDATA %.0f (load %.0f) rxGAP %.0f   wire loss %.2f%%\n",
                   txDATA, sw_sum(sum2,count,"txD_probe"), sw_sum(sum2,count,"txD_load"),
                   sw_sum(sum2,count,"txGAP"), rxDATA, sw_sum(sum2,count,"rxD_load"),
                   sw_sum(sum2,count,"rxGAP"), wire);
        }
    }
    printf("\nDone.\n");
    return 0;
}

/* ============================== dispatch ================================ */
int main(int argc, char **argv){
    if (argc >= 2 && strcmp(argv[1], "node") == 0)
        return node_main(argc-1, argv+1);
    if (argc >= 2 && strcmp(argv[1], "selftest") == 0)
        return selftest_main();
    if (argc >= 2 && strcmp(argv[1], "sweep") == 0)
        return sweep_main(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "sendbench") == 0){
#ifdef _WIN32
        return sendbench_main();
#else
        fprintf(stderr, "sendbench is Windows-only (it measures winsock loopback costs)\n");
        return 1;
#endif
    }
    fprintf(stderr,
        "usage: dart_test <command> [args]\n"
        "  node <name> [domain] [load_hz] [duration_s] [mcast] [reliable] [block_ms]\n"
        "        latency/throughput node; SUMMARY+SUMMARY2 on timed exit\n"
        "        reliable=1: load channel DART_RELIABLE; block_ms: writer\n"
        "        backpressure window (qos.max_block_us) for that channel\n"
        "        env: DART_DIAG_RCVBUF/DART_DIAG_SNDBUF (bytes), DART_DIAG_TRACE\n"
        "  sweep [--nodes N] [--domain D] [--duration S] [--rates a,b,c]\n"
        "        [--mcast 0|1] [--reliable] [--block-ms N] [--diag]\n"
        "        spawn N node children per rate; print an RTT-vs-throughput table\n"
        "  sendbench\n"
        "        UDP send-cost microbench on loopback (Windows)\n"
        "  selftest\n"
        "        on_gap, backpressure, dynamic-interest functional test (exit 0 = pass)\n");
    return 2;
}
