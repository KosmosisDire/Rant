/* middleware test & diagnostic CLI.
 *
 * Subcommands: node (full-mesh latency/throughput node), sweep (spawns node
 * children per rate, aggregates into a table; --remote folds in results from
 * `serve` workers on other machines), serve (two-machine sweep worker),
 * selftest (on_gap, backpressure, dynamic-interest functional test),
 * sendbench (UDP send-cost microbench, Windows-only).
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
        else if (t == 3) sub = 22;
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

#undef setsockopt
#ifdef _WIN32
#undef sendto
#undef recvfrom
#endif

/* ===================== node-API test shims =============================
 * The public node API is handle-based (dart_node_create_channel -> DartChannel*,
 * dart_channel_send, ...). These test-internal helpers keep the index-based call
 * sites concise: dart_node_channel(n, i) maps a creation index back to its handle,
 * so the old (node, channel-index) call form maps straight onto the handle calls. */
#define dart_node_send(n, idx, d, l)         dart_channel_send(dart_node_channel((n),(idx)), (d), (l))
#define dart_node_set_role(n, idx, r)        dart_channel_set_role(dart_node_channel((n),(idx)), (r))
#define dart_node_drain(n, idx, ms)          dart_channel_drain(dart_node_channel((n),(idx)), (ms))
#define dart_node_writer_match_count(n, idx) dart_channel_match_count(dart_node_channel((n),(idx)))
#define dart_node_repair_stats(n, idx, o)    dart_channel_repair_stats(dart_node_channel((n),(idx)), (o))
#define dart_node_reader_progress(n, idx, p, b, h, t) \
        dart_channel_reader_progress(dart_node_channel((n),(idx)), (p),(b),(h),(t))

/* Open a node and create its channels from a DartChannelDef array in index order, so
 * the array index is the channel handle index the shims above resolve. opts carries
 * everything that used to live in DartNodeConfig except channels/on_message. */
static DartNode *test_node_open(uint8_t *mem, size_t cap, const char *name, DartMsgFn on_msg,
                               DartEventFn on_event, DartNodeOpts opts, const DartChannelDef *chans, uint16_t nch){
    DartNode *node; uint16_t i; DartAllocator alloc = dart_allocator_dynamic(0);
    (void)mem; (void)cap;             /* heap-backed: the node self-sizes (was a static arena) */
    if (!opts.max_channels) opts.max_channels = nch ? nch : 1;
    node = dart_node_open(&alloc, name, on_msg, on_event, &opts);
    if (!node) return NULL;
    for (i=0;i<nch;i++){
        DartChannelOpts co; memset(&co, 0, sizeof co);
        co.qos = chans[i].qos;
        if (!dart_node_create_channel(node, chans[i].name, (DartRole)chans[i].role, &co)){
            dart_node_close(node, 0); return NULL;
        }
    }
    return node;
}

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

#define CH_PROBE          0     /* channel handles are array indices         */
#define CH_LOAD           1
#define XCH_ID0           2     /* extra load channels: indices 2..2+xch-1   */
#define XCH_MAX           512
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
static int         g_xch = 0;         /* extra channels declared              */
static int         g_spread = 0;      /* 1 = load round-robins across them;
                                         2 = same but extras are PUB_ONLY on
                                         every node, so those writes have no
                                         readers anywhere (local cost only)   */

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
    /* load / drops, tracked per channel: cross-channel arrival order is not
       defined, so one shared counter would read reorder as drops */
    uint8_t  ch_init[1+XCH_MAX];      /* [0]=CH_LOAD, [1+k]=extra k          */
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

/* deferred pong queue (no dart_* calls from on_message) */
typedef struct { uint32_t tag, seq; uint64_t t; } pong_req;
static pong_req g_pong_q[512];
static int      g_pong_n = 0;

/* transport-reported permanent skips (KEEP_LAST supersession on reliable
 * channels, detected wire loss on best-effort). */
static unsigned long      g_gap_evt = 0;
static unsigned long long g_gap_tus = 0;
/* backpressure cost, read from the node before SUMMARY */
static uint64_t g_wait_us = 0;
static uint32_t g_wait_n  = 0;

static void lat_on_event(const DartEvent *ev){
    if (ev->kind == DART_MSG_LOST){ g_gap_evt++; g_gap_tus += ev->lost_count; }
}

static void lat_on_message(const DartMsg *msg){
    uint16_t ch = msg->channel_id; uint32_t from = msg->sender_id;
    const void *data = msg->data; size_t len = msg->len;
    const uint8_t *p = (const uint8_t*)data; uint64_t now = now_ns();
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
    } else {
        uint32_t seq; int idx;
        if (ch==CH_LOAD) idx=0;
        else if (ch>=XCH_ID0 && ch<XCH_ID0+g_xch) idx=1+(ch-XCH_ID0);
        else return;
        if (len < 5 || p[0]!=LOAD_MAGIC) return;
        seq = get32(p+1);
        if (!e->ch_init[idx]){ e->ch_init[idx]=1; e->ch_expect[idx]=seq+1; e->load_recv++; }
        else if (seq >= e->ch_expect[idx]){
            e->load_drops += (seq - e->ch_expect[idx]);   /* gap = dropped */
            e->ch_expect[idx] = seq+1;
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
           g_wait_us/1000.0, (unsigned long)g_wait_n);
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
    int if_mode     = (argc>5 ? atoi(argv[5]) : 0);   /* discovery interface: 1 loopback, 2 NIC */
    int reliable    = (argc>6 ? atoi(argv[6]) : 0);
    int block_ms    = (argc>7 ? atoi(argv[7]) : 0);
    g_xch           = (argc>8 ? atoi(argv[8]) : 0);
    g_spread        = (argc>9 ? atoi(argv[9]) : 0);
    const char *if_ip   = (argc>10 && strcmp(argv[10],"0")) ? argv[10] : NULL;
    const char *peer_ip = (argc>11 && strcmp(argv[11],"0")) ? argv[11] : NULL;
    if (g_xch < 0) g_xch = 0;
    if (g_xch > XCH_MAX) g_xch = XCH_MAX;

    { uint64_t s = now_ns();
      g_tag = (uint32_t)(s ^ (s>>32) ^ ((uint32_t)
#ifdef _WIN32
              GetCurrentProcessId()
#else
              getpid()
#endif
              << 16)); }

    static DartChannelDef ch[2+XCH_MAX];
    static char xnames[XCH_MAX][8];   /* "x0".."x511": extra channels' topic names */
    memset(ch, 0, sizeof ch);
    ch[0].name = "probe";
    /* depth must cover the pongs one tick can stage (one per peer) or the ring
       evicts them before the flush and RTT samples are lost */
    ch[0].qos.reliability = DART_BEST_EFFORT; ch[0].qos.keep_last = 16; ch[0].qos.max_message_bytes = 64;
    ch[1].name = "load";
    ch[1].qos.reliability = reliable ? DART_RELIABLE : DART_BEST_EFFORT;
    ch[1].qos.keep_last = LOAD_DEPTH; ch[1].qos.max_message_bytes = 64;
    ch[1].qos.backpressure_wait_us = (uint32_t)(block_ms > 0 ? block_ms : 0) * 1000u;
    { int i;     /* extra channels: idle (depth 1) or load-bearing when spread */
      for (i=0;i<g_xch;i++){
          ch[2+i] = ch[1];
          sprintf(xnames[i], "x%d", i); ch[2+i].name = xnames[i];
          ch[2+i].qos.keep_last = g_spread ? 128 : 1;
          if (g_spread==2) ch[2+i].role = DART_PUB_ONLY;   /* nobody subscribes */
      } }

    /* meta_max_ids is gone: the core auto-raises it to 2*n_channels, which
       already covers every extra channel here. disable_shm: the sweep measures the
       UDP path, so don't let same-host peers silently switch to shared memory. */
    DartNodeOpts opts = {
        .domain      = domain,
        .disable_shm = 1,
        .discovery   = { .max_peers = MAX_PEERS },
    };
    if (if_ip)
        opts.net.multicast_interface = if_ip;       /* multihomed host: pin discovery here */
    else if (if_mode==1)
        opts.net.multicast_interface = "127.0.0.1"; /* single-host test: discovery on loopback.
                                             if_mode=2 leaves the real interface for
                                             cross-machine runs (data is always unicast) */
    DartDiscoveryAddr seed;
    if (peer_ip){                     /* bootstrap without multicast */
        uint32_t a4 = inet_addr(peer_ip);
        if (a4 != INADDR_NONE){
            memset(&seed, 0, sizeof seed);
            memcpy(seed.ip, &a4, 4); seed.ip_len = 4;   /* port 0 = disc port */
            opts.net.seed_peers = &seed; opts.net.n_seed_peers = 1;
        }
    }
    { const char *rb = getenv("DART_DIAG_RCVBUF"), *sb = getenv("DART_DIAG_SNDBUF");
      if (rb) opts.net.recv_buffer_bytes = (uint32_t)atoi(rb);
      if (sb) opts.net.send_buffer_bytes = (uint32_t)atoi(sb);
      if (rb || sb) printf("[%s] buffer override rcvbuf=%u sndbuf=%u\n",
                           g_name, opts.net.recv_buffer_bytes, opts.net.send_buffer_bytes); }
    g_trace = (getenv("DART_DIAG_TRACE") != NULL);

    static uint8_t mem[48<<20];  /* deep load ring + extra channels x 64 peers */
    DartNode *n = test_node_open(mem, sizeof mem, g_name, lat_on_message, lat_on_event, opts, ch, (uint16_t)(2+g_xch));
    if (!n){ fprintf(stderr, "[%s] dart_node_open failed\n", g_name); return 1; }

    printf("[%s] up (domain %u, tag %08x, load %d Hz, %ds, %s load, block %d ms, +%d ch %s)\n",
           g_name, domain, g_tag, load_hz, duration_s,
           reliable ? "reliable" : "best-effort", block_ms,
           g_xch, g_spread==2 ? "void" : g_spread ? "spread" : "idle");

    uint64_t start = now_ns(), last_ping = start, last_report = start;
    static uint32_t load_seq_ch[1+XCH_MAX];
    uint32_t ping_seq = 0;
    int rr = 0;                  /* spread round-robin cursor */
    unsigned long load_sent = 0, load_forgiven = 0;
    int wait_ms = (load_hz > 0) ? 1 : 20;   /* don't block long while loading */
    /* stall detection: a loop gap far beyond wait_ms means we lost the CPU, so
       big stalls make a run suspect. Voluntary backpressure waits
       (qos.backpressure_wait_us) are subtracted so stall numbers mean INVOLUNTARY loss;
       voluntary time is reported separately as blk_wait_ms. */
    uint64_t prev_iter = 0, max_gap = 0, stall_ns = 0, max_gap_at = 0;
    uint64_t prev_wait_us = 0;
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
        { uint64_t wait_us;
          dart_node_backpressure_stats(n, &wait_us, NULL);
          if (prev_iter){
              uint64_t gap = now - prev_iter;
              uint64_t waited = (wait_us - prev_wait_us) * 1000u;   /* us to ns */
              gap = (gap > waited) ? gap - waited : 0;     /* voluntary waits out */
              if (gap > max_gap){ max_gap = gap; max_gap_at = now - start; }
              if (gap > STALL_GAP_NS){ stall_ns += gap; nstalls++; }
          }
          prev_wait_us = wait_us; }
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
                int idx = 0; uint16_t chid = CH_LOAD;
                if (g_spread && g_xch){
                    idx = rr; rr = (rr+1)%(1+g_xch);
                    if (idx) chid = (uint16_t)(XCH_ID0+idx-1);
                }
                build_load(o, load_seq_ch[idx]++);
                dart_node_send(n, chid, o, LOAD_LEN);
                load_sent++; burst++;
            }
            t_stage += now_ns() - ph;
            if (burst >= LOAD_BURST_MAX) burst_capped++;
        }

        ph = now_ns(); dart_node_poll(n, 0);       t_poll0 += now_ns() - ph;  /* flush */
        ph = now_ns(); dart_node_poll(n, wait_ms); t_poll1 += now_ns() - ph;  /* wait  */

        if (g_trace && !traced_peers && now_ns() - start > 3000000000ull){
            traced_peers = 1;
            printf("TRACE node fd=%u domain=%u\n", (unsigned)n->fd, n->domain);
            for (i = 0; i < (int)dart_node_core_max_peers(n->core); i++){
                uint32_t pid; uint8_t pip[16]; uint16_t pport;
                if (dart_node_core_peer_at(n->core, (uint16_t)i, &pid, pip, NULL, &pport))
                    printf("TRACE peer id=%u addr=%u.%u.%u.%u:%u\n", pid,
                           pip[0], pip[1], pip[2], pip[3], pport);
            }
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
            dart_node_backpressure_stats(n, &g_wait_us, &g_wait_n);
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
 *   2. GAP      : writer stages a burst beyond keep_last without flushing,
 *                 so KEEP_LAST evicts and the reader gets one GAP plus the
 *                 surviving tail; on_gap count must equal the evicted span.
 *   3. BLOCKED  : on a backpressure_wait_us channel, sends that would evict un-acked
 *                 history wait ~backpressure_wait_us while the reader never acks, then
 *                 proceed (KEEP_LAST fallback, never refusal).
 *   4. RELEASED : same channel once the reader acks; sends are instant.
 *   4b SWEEP-ACK: a sub-only reader whose ACKNACK is timer-armed (nack_delay>0)
 *                 must flush it via the periodic sweep when the writer goes
 *                 quiet; backpressure must release with no data event to ride.
 *   5. DYNAMIC  : reader flips a channel inactive/subscribed at runtime;
 *                 each (re)subscribe replays cached history with no gap.
 *   6. SCALE    : 40 channels, past the old 31-id announce cap. */

#define ST_DOMAIN   33
/* channel handles are array indices (declaration order in ch[]) */
#define ST_CH_GAP   0   /* reliable, depth 4, no backpressure  */
#define ST_CH_BLOCK 1   /* reliable, depth 4, slow_reader_wait 100 ms */
#define ST_CH_DYN   2   /* reliable, depth 4, reader starts DART_INACTIVE */
#define ST_CH_BLOCK2 3  /* like BLOCK but repair_delay>0: the reader's ack is
                           timer-armed, so it relies on the periodic sweep */
#define ST_DEPTH    4
#define ST_BLOCK_US 100000u
#define ST_NACK_US  5000u
#define ST_NCH      40

static int st_fail = 0;
#define ST_CHECK(cond, ...) do { \
    printf((cond) ? "  ok   " : "  FAIL "); printf(__VA_ARGS__); printf("\n"); \
    if (!(cond)) st_fail = 1; } while (0)

static unsigned long st_samples[8], st_gap_calls[8], st_gap_tus[8], st_any;
static char st_last_sender[64];   /* sender_name of the most recently delivered message */

static void st_on_message(const DartMsg *msg){
    if (msg->channel_id < 8) st_samples[msg->channel_id]++;
    st_last_sender[0] = '\0';
    if (msg->sender_name){
        size_t n = msg->sender_name_len < sizeof st_last_sender - 1 ? msg->sender_name_len : sizeof st_last_sender - 1;
        memcpy(st_last_sender, msg->sender_name, n); st_last_sender[n] = '\0';
    }
    st_any++;
}
static unsigned long st_collisions;
static void st_on_event(const DartEvent *ev){
    if (ev->kind == DART_MSG_LOST){
        if (ev->channel < 8){ st_gap_calls[ev->channel]++; st_gap_tus[ev->channel] += (unsigned long)ev->lost_count; }
    } else if (ev->kind == DART_NAME_COLLISION){
        st_collisions++;
    }
}

static void st_pump(DartNode *a, DartNode *b, int ms){     /* run both nodes */
    uint64_t end = dart_plat_now_us() + (uint64_t)ms*1000u;
    while (dart_plat_now_us() < end){ dart_node_poll(a, 1); if (b) dart_node_poll(b, 0); }
}

/* ---- discovery-core (sans-IO) checks: peer lifecycle without sockets ---- */
static uint32_t dc_up_id, dc_up_n, dc_down_id, dc_down_n, dc_refused_n;
static int      dc_down_reason;
static void dc_event(const DartDiscoveryEvent *ev){
    if      (ev->kind==DART_DISCOVERY_PEER_UP)     { dc_up_id=ev->peer; dc_up_n++; }
    else if (ev->kind==DART_DISCOVERY_PEER_DOWN)   { dc_down_id=ev->peer; dc_down_reason=(int)ev->reason; dc_down_n++; }
    else if (ev->kind==DART_DISCOVERY_PEER_REFUSED){ dc_refused_n++; }
}

/* craft a v3 announce for sender `uid` (uuid = all uid bytes), meta_len 0 */
static size_t dc_mk(uint8_t *p, uint8_t uid, uint8_t flags, uint16_t dom, uint16_t port, uint32_t mver){
    size_t off = DART_DISCOVERY_META_OFF;
    uint8_t *b = p + off;
    memset(p, 0, off);
    p[0]='u';p[1]='D';p[2]='S';p[3]='C';
    p[4]=(uint8_t)DART_DISCOVERY_PROTO_VERSION;
    p[5]=flags;                                   /* 0x01 = BYE (internal flag) */
    p[6]=(uint8_t)dom; p[7]=(uint8_t)(dom>>8);
    memset(p+8, uid, 16);                         /* a distinct uuid per uid */
    p[off-6]=(uint8_t)mver; p[off-5]=(uint8_t)(mver>>8);
    p[off-4]=(uint8_t)(mver>>16); p[off-3]=(uint8_t)(mver>>24);
    /* blob = discovery section [u16 data_port][u8 self_ip_len=0][u8 name_len=0]; no overlay.
       self_ip_len 0 -> the receiver uses the datagram src ip; the port now rides here. */
    b[0]=(uint8_t)port; b[1]=(uint8_t)(port>>8); b[2]=0; b[3]=0;
    p[off-2]=4; p[off-1]=0;                        /* meta_len = 4 (the discovery section) */
    return off + 4;
}

static void disc_core_checks(void){
    static uint8_t mem[8192];
    uint8_t buf[DART_DISCOVERY_WIRE_MAX], out[DART_DISCOVERY_WIRE_MAX];
    uint8_t sa[4]={10,0,0,1}, sb[4]={10,0,0,2}, sc[4]={10,0,0,3};
    DartDiscoveryCoreConfig c; DartDiscoveryState *st;
    uint32_t idA, idB; size_t n;
    memset(&c,0,sizeof c);
    memset(c.uuid,0xEE,16);                        /* receiver uuid, distinct from senders */
    c.domain_id=99; c.announce_interval_us=1000000; c.peer_timeout_us=1000000; c.max_peers=2;
    c.on_event=dc_event;
    st = dart_discovery_init(mem,sizeof mem,&c);
    ST_CHECK(st!=NULL, "disc-core: init");
    if (!st) return;
    dart_discovery_update(st, 1000, out, sizeof out);   /* start */

    /* 1. two peers announce -> two ups, both ACTIVE */
    dc_up_n=dc_down_n=dc_refused_n=0;
    n=dc_mk(buf,1,0,99,5001,1); dart_discovery_on_datagram(st,sa,4,buf,n,2000); idA=dc_up_id;
    n=dc_mk(buf,2,0,99,5002,1); dart_discovery_on_datagram(st,sb,4,buf,n,2000); idB=dc_up_id;
    ST_CHECK(dc_up_n==2 && dart_discovery_peer_count(st)==2,
             "disc-core: two peers up (ups=%u count=%u)", dc_up_n, dart_discovery_peer_count(st));

    /* 2. a new peer is REFUSED when the table is full of ACTIVE peers */
    dc_up_n=dc_refused_n=0;
    n=dc_mk(buf,3,0,99,5003,1); dart_discovery_on_datagram(st,sc,4,buf,n,2000);
    ST_CHECK(dc_refused_n==1 && dc_up_n==0 && dart_discovery_peer_count(st)==2,
             "disc-core: refuse new peer when full of active (refused=%u up=%u count=%u)",
             dc_refused_n, dc_up_n, dart_discovery_peer_count(st));

    /* 3. silence past timeout DROPS both (kept, not freed; excluded from count) */
    dc_down_n=0;
    dart_discovery_update(st, 2002000, out, sizeof out);
    ST_CHECK(dc_down_n==2 && dc_down_reason==(int)DART_DISCOVERY_DROP && dart_discovery_peer_count(st)==0,
             "disc-core: timeout drops both (downs=%u reason=%d count=%u)",
             dc_down_n, dc_down_reason, dart_discovery_peer_count(st));

    /* 4. same uuid returns -> RESUME under the SAME local_id */
    dc_up_n=0;
    n=dc_mk(buf,1,0,99,5001,1); dart_discovery_on_datagram(st,sa,4,buf,n,2100000);
    ST_CHECK(dc_up_n==1 && dc_up_id==idA && dart_discovery_peer_count(st)==1,
             "disc-core: same uuid resumes same id (up=%u sameid=%d count=%u)",
             dc_up_n, dc_up_id==idA, dart_discovery_peer_count(st));

    /* 5. a new peer now evicts the oldest DROPPED peer (B) as GONE */
    dc_down_n=dc_up_n=0;
    n=dc_mk(buf,3,0,99,5003,1); dart_discovery_on_datagram(st,sc,4,buf,n,2100000);
    ST_CHECK(dc_down_n==1 && dc_down_id==idB && dc_down_reason==(int)DART_DISCOVERY_GONE && dc_up_n==1,
             "disc-core: new peer evicts oldest dropped as GONE (downs=%u sameid=%d reason=%d up=%u)",
             dc_down_n, dc_down_id==idB, dc_down_reason, dc_up_n);

    /* 6. BYE is GONE */
    dc_down_n=0;
    n=dc_mk(buf,1,0x01,99,5001,1); dart_discovery_on_datagram(st,sa,4,buf,n,2100000);
    ST_CHECK(dc_down_n==1 && dc_down_id==idA && dc_down_reason==(int)DART_DISCOVERY_GONE
             && dart_discovery_peer_count(st)==1,
             "disc-core: BYE is GONE (downs=%u sameid=%d reason=%d count=%u)",
             dc_down_n, dc_down_id==idA, dc_down_reason, dart_discovery_peer_count(st));

    /* 7. a NEW uuid announcing from an (ip,port) we already hold = that endpoint's
          process restarted; the predecessor is provably dead (one socket == one
          process), so it must be evicted (GONE) rather than lingering to shadow the
          newcomer's data (the writer-restart-with-address-reuse hole). */
    { uint32_t idP;
      st = dart_discovery_init(mem,sizeof mem,&c);            /* fresh receiver */
      dart_discovery_update(st, 1000, out, sizeof out);
      dc_up_n=dc_down_n=0;
      n=dc_mk(buf,7,0,99,6001,1); dart_discovery_on_datagram(st,sa,4,buf,n,3000); idP=dc_up_id;
      dc_up_n=dc_down_n=0;
      n=dc_mk(buf,8,0,99,6001,1); dart_discovery_on_datagram(st,sa,4,buf,n,3100);  /* new uuid, same ip:port */
      ST_CHECK(dc_down_n==1 && dc_down_id==idP && dc_down_reason==(int)DART_DISCOVERY_GONE
               && dc_up_n==1 && dart_discovery_peer_count(st)==1,
               "disc-core: new uuid at a held ip:port evicts the predecessor (downs=%u sameid=%d reason=%d up=%u count=%u)",
               dc_down_n, dc_down_id==idP, dc_down_reason, dc_up_n, dart_discovery_peer_count(st));
    }
}

/* node-core peer lifecycle (sans-IO): drive dart_node_core_peer_up/down/refused
   directly over a transport, with NO sockets, clock, or platform anywhere. Proves
   the split: the peer table + discovery->transport wiring is testable in isolation. */
static uint32_t nc_up_n, nc_down_n, nc_refused_n, nc_up_id, nc_down_id;
static void nc_event(const DartEvent *ev){
    if      (ev->kind==DART_PEER_UP)     { nc_up_n++;   nc_up_id=ev->peer; }
    else if (ev->kind==DART_PEER_DOWN)   { nc_down_n++; nc_down_id=ev->peer; }
    else if (ev->kind==DART_PEER_REFUSED){ nc_refused_n++; }
}
/* feed the node core through a REAL (sans-IO) discovery core: build an announce datagram
   (discovery section [u16 port][u8 self_ip_len=0][u8 name_len][name], no overlay) so
   discovery populates its table and fires events INTO dart_node_core_on_disc_event.
   flags 0x01 = BYE. */
static size_t nc_dgram(uint8_t *p, uint8_t uid, uint8_t flags, uint16_t dom, uint16_t port,
                       const char *name, uint32_t mver){
    size_t off = DART_DISCOVERY_META_OFF;
    uint8_t *b = p + off;
    uint8_t nl = name ? (uint8_t)strlen(name) : 0;
    uint16_t ml = (uint16_t)(4u + nl);
    memset(p, 0, off);
    p[0]='u';p[1]='D';p[2]='S';p[3]='C';
    p[4]=(uint8_t)DART_DISCOVERY_PROTO_VERSION;
    p[5]=flags;
    p[6]=(uint8_t)dom; p[7]=(uint8_t)(dom>>8);
    memset(p+8, uid, 16);                          /* a distinct uuid per uid */
    p[off-6]=(uint8_t)mver; p[off-5]=(uint8_t)(mver>>8);
    p[off-4]=(uint8_t)(mver>>16); p[off-3]=(uint8_t)(mver>>24);
    b[0]=(uint8_t)port; b[1]=(uint8_t)(port>>8); b[2]=0; b[3]=nl;   /* port, self_ip_len=0, name_len */
    if (nl) memcpy(b+4, name, nl);
    p[off-2]=(uint8_t)ml; p[off-1]=(uint8_t)(ml>>8);
    return off + ml;
}

/* node-core peer lifecycle, fully sans-IO: a transport + a real discovery core (no
   sockets/clock/platform), the node core delegating its peer table to discovery. Proves
   the collapse: id<->address resolution + naming + the transport lifecycle all ride
   discovery's one peer table, with the node core's per-peer state in its user scratch. */
static void node_core_checks(void){
    static uint8_t tmem[1<<18], cmem[4096], dmem[8192];
    uint8_t buf[256], out[DART_DISCOVERY_WIRE_MAX];
    uint8_t sa[4]={10,0,0,1}, sb[4]={10,0,0,2}, sc[4]={10,0,0,3};
    DartConfig tc; DartTransportState *tr; DartChannelDef ch[1];
    DartDiscoveryCoreConfig dcfg; DartDiscoveryState *st;
    i_DartNodeCoreConfig cc; i_DartNodeCore *nc;
    i_DartNodeDest d; uint32_t id, idA, idB; size_t n;

    memset(ch,0,sizeof ch); ch[0].name="nc/topic";
    memset(&tc,0,sizeof tc); tc.channels=ch; tc.n_channels=1; tc.max_peers=2;
    tr = dart_init(tmem, sizeof tmem, &tc);
    ST_CHECK(tr!=NULL, "node-core: transport init");
    if (!tr) return;

    /* node core first (discovery bound once it exists, exactly like the runtime) */
    memset(&cc,0,sizeof cc);
    cc.transport=tr; cc.n_channels=1; cc.frag_size=1200; cc.on_event=nc_event;
    nc = dart_node_core_init(cmem, sizeof cmem, &cc);
    ST_CHECK(nc!=NULL, "node-core: init");
    if (!nc) return;

    /* the discovery core whose peer table the node core delegates to: reserve the node's
       per-peer scratch via peer_user_bytes; wire its events into the node core */
    memset(&dcfg,0,sizeof dcfg); memset(dcfg.uuid,0xEE,16);
    dcfg.domain_id=99; dcfg.announce_interval_us=1000000; dcfg.peer_timeout_us=1000000; dcfg.max_peers=2;
    dcfg.peer_user_bytes=dart_node_core_peer_user_bytes();
    dcfg.on_event=dart_node_core_on_disc_event; dcfg.user=nc;
    st = dart_discovery_init(dmem, sizeof dmem, &dcfg);
    ST_CHECK(st!=NULL, "node-core: discovery init");
    if (!st) return;
    dart_node_core_bind_discovery(nc, st);
    dart_discovery_update(st, 1000, out, sizeof out);   /* start */

    /* build-meta still works (uses the transport, not any peer table) */
    {   uint16_t ml; const uint8_t *mb;
        dart_node_core_build_meta(nc);
        mb = dart_node_core_meta(nc, &ml);
        ST_CHECK(ml>=5 && dart_meta_frag(mb, ml)==1200,
                 "node-core: builds overlay (frag=%u)", dart_meta_frag(mb, ml)); }

    /* 1. two peers announce -> two ups; discovery assigns the ids; resolve each way */
    nc_up_n=nc_down_n=nc_refused_n=0;
    n=nc_dgram(buf,1,0,99,5001,"nc-self",1); dart_discovery_on_datagram(st,sa,4,buf,n,2000); idA=nc_up_id;
    n=nc_dgram(buf,2,0,99,5002,"nc-self",1); dart_discovery_on_datagram(st,sb,4,buf,n,2000); idB=nc_up_id;
    ST_CHECK(nc_up_n==2, "node-core: two peers up (ups=%u)", nc_up_n);
    {   uint8_t pnl=0; const char *pn = dart_node_core_peer_name(nc, idA, &pnl);
        ST_CHECK(pn && pnl==7 && strcmp(pn,"nc-self")==0,
                 "node-core: peer name learned from announce (%s)", pn?pn:"?"); }
    ST_CHECK(dart_node_core_resolve(nc,idA,&d) && d.port==5001 && d.ip[3]==1,
             "node-core: peer id resolves to addr (port=%u ip3=%u)", d.port, d.ip[3]);
    ST_CHECK(dart_node_core_id_for_addr(nc, sb, 5002, &id) && id==idB,
             "node-core: addr resolves to id (id=%u)", id);

    /* a nameless announce -> the peer name falls back to "unknown-peer" (never empty/NULL) */
    {   uint8_t pnl=0; const char *pn;
        n=nc_dgram(buf,1,0,99,5001,NULL,2);     dart_discovery_on_datagram(st,sa,4,buf,n,2001);
        pn = dart_node_core_peer_name(nc, idA, &pnl);
        ST_CHECK(pn && strcmp(pn,"unknown-peer")==0,
                 "node-core: nameless announce -> unknown-peer (%s)", pn?pn:"(null)");
        n=nc_dgram(buf,1,0,99,5001,"nc-self",3); dart_discovery_on_datagram(st,sa,4,buf,n,2002); }

    /* 2. a 3rd peer is REFUSED while the table is full of ACTIVE peers; the node forwards it */
    nc_refused_n=0;
    n=nc_dgram(buf,3,0,99,5003,"three",1); dart_discovery_on_datagram(st,sc,4,buf,n,2003);
    ST_CHECK(nc_refused_n==1, "node-core: refused forwarded (refused=%u)", nc_refused_n);

    /* 3. silence past the timeout DROPS both: a PEER_DOWN each, but the slots are kept (resolve) */
    nc_down_n=0;
    dart_discovery_update(st, 1003000, out, sizeof out);
    ST_CHECK(nc_down_n==2, "node-core: timeout drops both (downs=%u)", nc_down_n);
    ST_CHECK(dart_node_core_resolve(nc,idA,&d)==1, "node-core: dropped peer kept (resolves)");

    /* 4. the same uuid returns -> RESUME re-fires PEER_UP under the same id */
    nc_up_n=0;
    n=nc_dgram(buf,1,0,99,5001,"nc-self",4); dart_discovery_on_datagram(st,sa,4,buf,n,1100000);
    ST_CHECK(nc_up_n==1 && nc_up_id==idA, "node-core: resume re-ups same id (ups=%u id=%u)", nc_up_n, nc_up_id);

    /* 5. GONE (BYE) on the now-active peer: one PEER_DOWN, and it no longer resolves */
    nc_down_n=0;
    n=nc_dgram(buf,1,0x01,99,5001,NULL,1); dart_discovery_on_datagram(st,sa,4,buf,n,1100001);
    ST_CHECK(nc_down_n==1, "node-core: GONE on active fires down (downs=%u)", nc_down_n);
    ST_CHECK(dart_node_core_resolve(nc,idA,&d)==0, "node-core: GONE peer freed (no resolve)");
}

#ifdef DART_SHM
/* ===================== SHM self-tests (only when built -DDART_SHM) ========= */

/* (1) the dart_shm mapping module: create/attach by name, write/stamp/read, the
   generation recycle guard, and the seqlock verify tail. */
static void shm_module_checks(void){
    i_DartShmConfig cfg; void *pw, *pr; i_DartShmPool *w, *r;
    uint32_t cap=0, rlen=0; void *cp; const void *rp;
    i_DartShmDesc d, d2; uint8_t wire[DART_SHM_DESC_WIRE], a[16], b[16];
    const char msg[] = "hello shared memory";
    dart_plat_startup();
    memset(&cfg,0,sizeof cfg);
#ifdef _WIN32
    strcpy(cfg.name,"dart-shm-stmod");
#else
    strcpy(cfg.name,"/dart-shm-stmod");
#endif
    cfg.segment_id=0x1234; cfg.chunk_bytes=4096; cfg.n_chunks=4;
    pw=malloc(dart_shm_state_bytes()); pr=malloc(dart_shm_state_bytes());
    w=dart_shm_create(pw,&cfg); r=dart_shm_attach(pr,&cfg);
    ST_CHECK(w && r, "shm-mod: create + attach by name");
    if (w && r){
        dart_plat_host_uuid(a); dart_plat_host_uuid(b);
        ST_CHECK(dart_shm_host_match(a,b)==1, "shm-mod: host_uuid stable + matches");
        cp=dart_shm_chunk(w,0,&cap); memcpy(cp,msg,sizeof msg);
        dart_shm_stamp(w,0,(uint32_t)sizeof msg,&d);
        ST_CHECK(cap==4096 && d.generation==1, "shm-mod: loan + stamp (gen=%llu)", (unsigned long long)d.generation);
        dart_shm_desc_encode(&d,wire);
        ST_CHECK(dart_shm_desc_decode(&d2,wire,sizeof wire) &&
                 d2.chunk==d.chunk && d2.length==d.length && d2.generation==d.generation,
                 "shm-mod: descriptor wire round-trip");
        rp=dart_shm_read(r,&d2,&rlen);
        ST_CHECK(rp && rlen==sizeof msg && memcmp(rp,msg,sizeof msg)==0, "shm-mod: read sees writer bytes");
        ST_CHECK(dart_shm_verify(r,&d2)==1, "shm-mod: verify current gen ok");
        { void *cp2=dart_shm_chunk(w,0,NULL); i_DartShmDesc dn; uint32_t l;
          memcpy(cp2,"new",4); dart_shm_stamp(w,0,4,&dn);
          ST_CHECK(dart_shm_read(r,&d2,&l)==NULL, "shm-mod: recycled chunk -> old descriptor refused");
          ST_CHECK(dart_shm_verify(r,&d2)==0, "shm-mod: verify recycled gen fails (torn guard)"); }
        dart_shm_detach(r); dart_shm_detach(w);
    }
    free(pw); free(pr);
    dart_plat_cleanup();
}

/* (2) transport-core loss/repair: a mock on_shm whose success is controllable and a
   pump that can drop SHM-DATA. Proves a failed resolve does NOT ack (so it repairs),
   a dropped descriptor re-sends, and a persistently unresolvable descriptor is
   skipped after the retry cap (MSG_LOST) without wedging the reader. */
static int shml_ok, shml_recv, shml_lost, shml_drop;
static uint64_t shml_now;
static DartTransportState *shml_W, *shml_R;
static int shml_on_shm(void *u, uint16_t ch, uint32_t from, const uint8_t *desc){
    (void)u;(void)ch;(void)from;(void)desc; if (shml_ok){ shml_recv++; return 1; } return 0;
}
static void shml_on_event(const DartTransportEvent *ev){ if (ev->kind==DART_TRANSPORT_MSG_LOST) shml_lost++; }
static void shml_pump(int n){
    uint8_t buf[DART_DGRAM_MAX]; uint32_t to; size_t ol; int i;
    for (i=0;i<n;i++){
        while (dart_poll_send(shml_W,&to,buf,sizeof buf,&ol,shml_now)){
            if (shml_drop>0 && (buf[0]&0x20u)){ shml_drop--; continue; }   /* drop SHM-DATA */
            dart_on_datagram(shml_R, 1u, buf, ol, shml_now);
        }
        while (dart_poll_send(shml_R,&to,buf,sizeof buf,&ol,shml_now))
            dart_on_datagram(shml_W, 2u, buf, ol, shml_now);
        shml_now += 30000;   /* 30 ms: past repair_delay (20ms), lets heartbeats fire */
    }
}
static void shml_send(void){
    static unsigned char chunk[2048]; unsigned char desc[DART_SHM_DESC_WIRE];
    memset(desc,0,sizeof desc); dart_send_shm(shml_W, 0, chunk, 1000, desc, shml_now);
}
static void shm_loss_checks(void){
    DartChannelDef cw, cr; DartConfig wc, rc; void *mw, *mr; size_t nw, nr; uint8_t blob[256]; size_t bl;
    DartQos q; memset(&q,0,sizeof q); q.reliability=DART_RELIABLE; q.keep_last=8;
    q.heartbeat_us=50000; q.repair_delay_us=20000;
    memset(&cw,0,sizeof cw); cw.name="shmloss"; cw.qos=q; cw.role=DART_PUB_ONLY;
    memset(&cr,0,sizeof cr); cr.name="shmloss"; cr.qos=q; cr.role=DART_SUB_ONLY;
    memset(&wc,0,sizeof wc); wc.channels=&cw; wc.n_channels=1; wc.max_peers=2;
    memset(&rc,0,sizeof rc); rc.channels=&cr; rc.n_channels=1; rc.max_peers=2;
    rc.on_shm=shml_on_shm; rc.on_event=shml_on_event;
    nw=dart_required_memory(&wc); mw=malloc(nw); shml_W=dart_init(mw,nw,&wc);
    nr=dart_required_memory(&rc); mr=malloc(nr); shml_R=dart_init(mr,nr,&rc);
    shml_now=1000000;
    dart_peer_add(shml_W,2u,DART_FRAG_PAYLOAD); dart_peer_add(shml_R,1u,DART_FRAG_PAYLOAD);
    bl=dart_build_interest(shml_W,blob,sizeof blob); dart_apply_peer_interest(shml_R,1u,blob,bl);
    bl=dart_build_interest(shml_R,blob,sizeof blob); dart_apply_peer_interest(shml_W,2u,blob,bl);
    dart_peer_set_shm(shml_W,2u,1);
    ST_CHECK(dart_writer_match_count(shml_W,0)>0, "shm-loss: writer matched reader");
    shml_ok=1; shml_recv=0; shml_lost=0; shml_drop=0; shml_send(); shml_pump(5);
    ST_CHECK(shml_recv==1 && shml_lost==0, "shm-loss: [a] normal delivered");
    shml_recv=0; shml_lost=0; shml_drop=1; shml_send(); shml_pump(10);
    ST_CHECK(shml_recv==1 && shml_lost==0, "shm-loss: [b] dropped descriptor repaired");
    shml_recv=0; shml_lost=0; shml_drop=0; shml_ok=0; shml_send(); shml_pump(4);
    ST_CHECK(shml_recv==0, "shm-loss: [c] unresolvable not delivered");
    shml_ok=1; shml_pump(6);
    ST_CHECK(shml_recv==1 && shml_lost==0, "shm-loss: [c] delivered after transient clears");
    shml_recv=0; shml_lost=0; shml_ok=0; shml_send(); shml_pump(20);
    ST_CHECK(shml_recv==0 && shml_lost>=1, "shm-loss: [d] persistent -> skip + MSG_LOST");
    shml_ok=1; shml_recv=0; shml_send(); shml_pump(6);
    ST_CHECK(shml_recv==1, "shm-loss: [d] not wedged, next delivered");
    dart_destroy(shml_W); dart_destroy(shml_R); free(mw); free(mr);
}

/* (3) full nodes on loopback: SHM across size classes (byte-exact + shm_tx/rx), and
   the inline fallback for a non-SHM-capable subscriber. */
static int shmn_recv; static size_t shmn_len; static unsigned long shmn_sum;
static void shmn_on_message(const DartMsg *msg){
    const unsigned char *p=(const unsigned char*)msg->data; size_t i, len=msg->len; unsigned long s=0;
    for(i=0;i<len;i++) s+=p[i];
    shmn_recv++; shmn_len=len; shmn_sum=s;
}
static DartNode *shmn_open(int is_pub, int shm_capable, uint16_t domain, void **mem_out){
    static DartChannelDef ch[2]; static int slot;
    DartChannelDef *d=&ch[slot++ & 1]; DartDiscoveryAddr seed; DartNodeOpts opts;
    size_t cap = 1u<<20; void *mem; DartQos q; memset(&q,0,sizeof q);
    q.reliability=DART_RELIABLE; q.keep_last=4; q.catch_up=1;
    memset(d,0,sizeof *d); d->name="shmnode"; d->qos=q; d->role=is_pub?DART_PUB_ONLY:DART_SUB_ONLY;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&opts,0,sizeof opts); opts.domain=domain;   /* dynamic mode (test_node_open) is SHM-capable */
    opts.disable_shm = (uint8_t)(!shm_capable);   /* a non-SHM peer forces the inline UDP path */
    opts.discovery.max_peers=4;
    opts.net.multicast_interface="127.0.0.1"; opts.net.seed_peers=&seed; opts.net.n_seed_peers=1;
    mem=malloc(cap); *mem_out=mem;
    return test_node_open(mem, cap, NULL, is_pub?NULL:shmn_on_message, NULL, opts, d, 1);
}
static void shm_node_checks(void){
    static unsigned char buf[6*1024*1024];
    DartNode *P,*S; void *mp,*ms; int i; uint32_t tx=0, rx=0; size_t sizes[3];
    P=shmn_open(1,1,77,&mp); S=shmn_open(0,1,77,&ms);
    ST_CHECK(P&&S, "shm-node: SHM-capable pub + sub open");
    if (P&&S){
        for (i=0;i<800 && dart_node_writer_match_count(P,0)==0;i++){ dart_node_poll(P,2); dart_node_poll(S,2); }
        ST_CHECK(dart_node_writer_match_count(P,0)>0, "shm-node: matched");
        sizes[0]=200; sizes[1]=300*1024; sizes[2]=4*1024*1024;
        for (i=0;i<3;i++){
            unsigned long want=0; size_t j; int before=shmn_recv, t;
            for (j=0;j<sizes[i];j++){ buf[j]=(unsigned char)((j*31u+(unsigned)i+1)&0xFF); want+=buf[j]; }
            dart_node_send(P,0,buf,sizes[i]);
            for (t=0;t<500 && shmn_recv==before;t++){ dart_node_poll(P,2); dart_node_poll(S,2); }
            ST_CHECK(shmn_recv==before+1 && shmn_len==sizes[i] && shmn_sum==want,
                     "shm-node: byte-exact %lu bytes", (unsigned long)sizes[i]);
        }
        dart_node_shm_stats(P,&tx,NULL); dart_node_shm_stats(S,NULL,&rx);
        ST_CHECK(tx==3 && rx==3, "shm-node: all 3 over SHM (tx=%u rx=%u)", tx, rx);
        dart_node_close(P,1); dart_node_close(S,1);
    }
    free(mp); free(ms);
    /* inline fallback: a non-SHM-capable subscriber forces inline UDP */
    shmn_recv=0;
    P=shmn_open(1,1,78,&mp); S=shmn_open(0,0,78,&ms);
    if (P&&S){
        unsigned long want=0; size_t j; int before, t;
        for (i=0;i<800 && dart_node_writer_match_count(P,0)==0;i++){ dart_node_poll(P,2); dart_node_poll(S,2); }
        for (j=0;j<50*1024;j++){ buf[j]=(unsigned char)((j*31u+9)&0xFF); want+=buf[j]; }
        before=shmn_recv; dart_node_send(P,0,buf,50*1024);
        for (t=0;t<500 && shmn_recv==before;t++){ dart_node_poll(P,2); dart_node_poll(S,2); }
        ST_CHECK(shmn_recv==before+1 && shmn_sum==want, "shm-node: non-SHM sub -> inline byte-exact");
        tx=0; dart_node_shm_stats(P,&tx,NULL);
        ST_CHECK(tx==0, "shm-node: no SHM used for the non-SHM reader (tx=%u)", tx);
        dart_node_close(P,1); dart_node_close(S,1);
    }
    free(mp); free(ms);
}

#endif /* DART_SHM */

/* Unit checks for the small pure helpers the Tier-1 cleanup touched: the fragment
 * clamp, the dart_send result codes, and the shared little-endian byte packing.
 * These need no sockets, so they run straight against the transport core. */
static void unit_checks(void){
    /* dart_clamp_frag: 0 -> default, otherwise clamp into [MIN, MAX] */
    ST_CHECK(dart_clamp_frag(0) == DART_FRAG_PAYLOAD,
             "clamp: 0 -> default frag (%u)", (unsigned)dart_clamp_frag(0));
    ST_CHECK(dart_clamp_frag(65535) == DART_FRAG_PAYLOAD_MAX,
             "clamp: above-max -> MAX (%u)", (unsigned)dart_clamp_frag(65535));
    ST_CHECK(dart_clamp_frag(1) >= DART_FRAG_PAYLOAD_MIN,
             "clamp: tiny -> >= MIN (%u)", (unsigned)dart_clamp_frag(1));

    /* shared little-endian helpers: byte order + round-trip */
    {   uint8_t b[8];
        dart_le_w16(b, 0xBEEFu);
        ST_CHECK(b[0]==0xEF && b[1]==0xBE && dart_le_r16(b)==0xBEEFu,
                 "bytes: w16/r16 little-endian round-trip");
        dart_le_w32(b, 0x01020304u);
        ST_CHECK(b[0]==0x04 && b[3]==0x01 && dart_le_r32(b)==0x01020304u,
                 "bytes: w32/r32 little-endian round-trip");
        dart_le_w64(b, 0x0102030405060708ull);
        ST_CHECK(b[0]==0x08 && b[7]==0x01 && dart_le_r64(b)==0x0102030405060708ull,
                 "bytes: w64/r64 little-endian round-trip");
    }

    /* topic identity: deterministic and name-distinct */
    ST_CHECK(dart_topic_id("alpha") == dart_topic_id("alpha")
             && dart_topic_id("alpha") != dart_topic_id("beta"),
             "topic-id: deterministic and name-distinct");

    /* dart_send result codes (transport core, no sockets). The size check precedes
       the role check, so an oversize send on the pub channel is TOO_BIG, while a
       valid-size send on the sub-only channel is ROLE. */
    {   static uint8_t tmem[1<<16];
        DartChannelDef uch[2]; DartConfig tc; DartTransportState *ts; uint8_t buf[128];
        memset(uch, 0, sizeof uch);
        uch[0].name = "u/pub"; uch[0].role = DART_PUBSUB;   uch[0].qos.max_message_bytes = 64;
        uch[1].name = "u/sub"; uch[1].role = DART_SUB_ONLY; uch[1].qos.max_message_bytes = 64;
        memset(&tc, 0, sizeof tc);
        tc.channels = uch; tc.n_channels = 2; tc.max_peers = 2;
        ts = dart_init(tmem, sizeof tmem, &tc);
        ST_CHECK(ts != NULL, "result: transport init");
        if (ts){
            memset(buf, 0, sizeof buf);
            ST_CHECK(dart_send(ts, 5, buf, 16,  0) == DART_ERR_NO_CHANNEL, "result: out-of-range channel -> NO_CHANNEL");
            ST_CHECK(dart_send(ts, 1, buf, 16,  0) == DART_ERR_ROLE,       "result: sub-only channel -> ROLE");
            ST_CHECK(dart_send(ts, 0, buf, 100, 0) == DART_ERR_TOO_BIG,    "result: oversize message -> TOO_BIG");
            ST_CHECK(dart_send(ts, 0, buf, 16,  0) == DART_OK,             "result: valid publish -> OK");
        }
    }
}

/* Exercise dart_node_open's staged-cleanup (goto fail) paths: force a failure at a
 * different stage each time so a distinct label runs, assert the open returns NULL,
 * then confirm a normal open still works -- proving cleanup left the platform balanced
 * (a missed dart_plat_cleanup unbalances the refcount; a missed close leaks the socket). */
static void open_fail_checks(void){
    static uint8_t mem[1<<20];
    DartNode *n;

    /* create-fail: an over-long topic name is rejected by dart_node_create_channel; the
       node opened fine and stays usable (validation moved from init to channel create) */
    {   static char longname[DART_TOPIC_NAME_MAX + 8]; DartChannel *c;
        DartAllocator a = dart_allocator_static(mem, sizeof mem);
        memset(longname, 'x', sizeof longname - 1); longname[sizeof longname - 1] = 0;
        n = dart_node_open(&a, NULL, NULL, NULL, &(DartNodeOpts){ .domain=ST_DOMAIN });
        ST_CHECK(n != NULL, "open-fail: node opens for create-fail check");
        c = n ? dart_node_create_channel(n, longname, DART_PUBSUB, NULL) : NULL;
        ST_CHECK(c == NULL, "open-fail: over-long topic name -> create_channel NULL");
        if (n) dart_node_close(n, 0);
    }

    /* a non-multicast discovery group makes discovery's IGMP join fail, so
       dart_discovery_place returns NULL and the node unwinds through fail_sock */
    {   DartAllocator a = dart_allocator_static(mem, sizeof mem);
        n = dart_node_open(&a, NULL, NULL, NULL,
            &(DartNodeOpts){ .domain=ST_DOMAIN, .net={ .discovery_group="1.2.3.4" } });
        ST_CHECK(n == NULL, "open-fail: non-multicast discovery group -> NULL");
        if (n) dart_node_close(n, 0);
    }

    /* fail_sock: occupy an ephemeral port, then aim the node's data socket at it; the
       unicast data bind takes no reuse, so it collides and unwinds through fail_sock */
    {   i_DartSock occupy;
        dart_plat_startup();
        occupy = dart_plat_udp_open();
        if (occupy != DART_SOCK_BAD && dart_plat_bind(occupy, 0, 0, 0)){
            uint16_t port = dart_plat_local_port(occupy);
            DartAllocator a = dart_allocator_static(mem, sizeof mem);
            n = dart_node_open(&a, NULL, NULL, NULL,
                &(DartNodeOpts){ .domain=ST_DOMAIN, .net={ .data_port=port } });
            ST_CHECK(n == NULL, "open-fail: data-port collision -> NULL (fail_sock)");
            if (n) dart_node_close(n, 0);
        }
        if (occupy != DART_SOCK_BAD) dart_plat_close(occupy);
        dart_plat_cleanup();
    }

    /* after the failed opens a normal open must still succeed (cleanup balanced) */
    {   DartAllocator a = dart_allocator_static(mem, sizeof mem);
        n = dart_node_open(&a, NULL, NULL, NULL, &(DartNodeOpts){ .domain=ST_DOMAIN });
        ST_CHECK(n != NULL, "open-fail: normal open still works after failures");
        if (n) dart_node_close(n, 0);
    }
}

/* Bug-1 regression: in an SHM+allocator build the node rewraps the transport's app
 * callbacks (on_message/on_shm/allocator) to forward the app's user_data, and points
 * the transport's user at the node. A transport-fired event (MSG_LOST/TOO_BIG/
 * NAME_COLLISION) must reach the app's on_event with that SAME user_data, not the node
 * pointer. NAME_COLLISION is the deterministic transport event: open a subscriber in
 * dynamic mode (so the node is SHM-capable, taking the rewrap path) with a sentinel
 * user_data, drive a topic-hash collision, and assert on_event saw the sentinel. */
static void *evu_user; static int evu_collisions;
static void evu_on_event(const DartEvent *ev){
    if (ev->kind == DART_NAME_COLLISION){ evu_user = ev->user; evu_collisions++; }
}
static void event_user_checks(void){
    static uint8_t mem_w[1<<20], mem_r[1<<20]; static int sentinel;
    const char *A="iuZA9tcJzAG", *B="5wVGxhTCmOC";   /* both -> one identity (see collide.c) */
    DartChannelDef cw, cr; DartNodeOpts wo, ro; DartNode *w, *r;
    uint8_t payload[16]; int i; memset(payload,0x5A,sizeof payload);
    memset(&cw,0,sizeof cw);
    cw.name=A; cw.role=DART_PUB_ONLY;
    cw.qos.reliability=DART_RELIABLE; cw.qos.keep_last=1; cw.qos.catch_up=1;
    cw.qos.max_message_bytes=32; cw.qos.heartbeat_us=50000;
    cr=cw; cr.name=B; cr.role=DART_SUB_ONLY;
    wo = (DartNodeOpts){ .domain=ST_DOMAIN+5, .discovery={ .max_peers=4 } };
    ro = wo;
    ro.user_data=&sentinel;   /* dynamic mode (test_node_open) -> SHM-capable */
    evu_user=NULL; evu_collisions=0;
    w=test_node_open(mem_w,sizeof mem_w,NULL,NULL,NULL,wo,&cw,1);
    r=test_node_open(mem_r,sizeof mem_r,NULL,NULL,evu_on_event,ro,&cr,1);
    ST_CHECK(w && r, "event-user: nodes open");
    if (w && r){
        for (i=0;i<120 && evu_collisions==0;i++){ dart_node_send(w,0,payload,16); dart_node_poll(w,0); dart_node_poll(r,20); }
        ST_CHECK(evu_collisions >= 1, "event-user: collision event fired (%d)", evu_collisions);
        ST_CHECK(evu_user == (void*)&sentinel,
                 "event-user: on_event gets user_data not node ptr (got %p want %p)", evu_user, (void*)&sentinel);
        dart_node_close(r,1); dart_node_close(w,1);
    }
}


/* Dynamic growth: creating channels past the reserve relocates the whole node into a bigger
 * arena, carrying live reliable state across. Stream on channel 0, force several grows by
 * creating channels mid-stream, then keep streaming on the ORIGINAL handle and assert no
 * message was lost, duplicated or reordered -- i.e. the migration preserved reader/writer
 * position, the peer/discovery state and the user's handle. */
static int dg_recv[24]; static int dg_seq_ok; static int dg_next0;
static void dg_on_message(const DartMsg *msg){
    if (msg->channel_id < 24) dg_recv[msg->channel_id]++;
    if (msg->channel_id == 0 && msg->len >= 1){
        int s = ((const uint8_t*)msg->data)[0];
        if (s != dg_next0) dg_seq_ok = 0;            /* gap, dup or reorder */
        dg_next0 = s + 1;
    }
}
static void dynamic_grow_checks(void){
    static const char *names[12] = {"dg/0","dg/1","dg/2","dg/3","dg/4","dg/5",
                                    "dg/6","dg/7","dg/8","dg/9","dg/10","dg/11"};
    DartAllocator pa = dart_allocator_dynamic(0), sa = dart_allocator_dynamic(0);
    DartNodeOpts po, so; DartNode *P=NULL, *S=NULL; DartChannel *pc0=NULL, *pcN;
    DartChannelOpts co; DartDiscoveryAddr seed; uint8_t payload[8]; int i, t;
    memset(&co,0,sizeof co); co.qos.reliability=DART_RELIABLE; co.qos.keep_last=32;
    co.qos.catch_up=32; co.qos.heartbeat_us=50000;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=ST_DOMAIN+7; po.max_channels=2; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    so=po;
    for (i=0;i<24;i++) dg_recv[i]=0;
    dg_seq_ok=1; dg_next0=0;
    P = dart_node_open(&pa, "dg-pub", NULL, NULL, &po);
    S = dart_node_open(&sa, "dg-sub", dg_on_message, NULL, &so);
    ST_CHECK(P&&S, "dyn-grow: nodes open (max_channels=2)");
    if (!(P&&S)){ if(P)dart_node_close(P,0); if(S)dart_node_close(S,0); return; }
    pc0 = dart_node_create_channel(P, names[0], DART_PUB_ONLY, &co);
    dart_node_create_channel(S, names[0], DART_SUB_ONLY, &co);
    ST_CHECK(pc0 != NULL, "dyn-grow: channel 0 created");
    for (t=0;t<800 && dart_channel_match_count(pc0)==0;t++){ dart_node_poll(P,2); dart_node_poll(S,2); }
    ST_CHECK(dart_channel_match_count(pc0)>0, "dyn-grow: channel 0 matched");
    for (i=0;i<5;i++){ payload[0]=(uint8_t)i; dart_channel_send(pc0,payload,1); dart_node_poll(P,1); dart_node_poll(S,2); }
    /* create channels 1..11 on both -> several grows (max_channels 2 -> 4 -> 8 -> 16) */
    for (i=1;i<12;i++){ dart_node_create_channel(P, names[i], DART_PUB_ONLY, &co);
                        dart_node_create_channel(S, names[i], DART_SUB_ONLY, &co);
                        dart_node_poll(P,1); dart_node_poll(S,1); }
    ST_CHECK(dart_channel_match_count(pc0)>0, "dyn-grow: channel 0 still matched after grows");
    for (i=5;i<10;i++){ payload[0]=(uint8_t)i; dart_channel_send(pc0,payload,1); dart_node_poll(P,1); dart_node_poll(S,2); }
    for (t=0;t<400 && dg_recv[0]<10;t++){ dart_node_poll(P,1); dart_node_poll(S,2); }
    ST_CHECK(dg_recv[0]==10, "dyn-grow: all 10 on channel 0 delivered across grows (got %d)", dg_recv[0]);
    ST_CHECK(dg_seq_ok, "dyn-grow: channel 0 in-order, no loss/dup across grows");
    pcN = dart_node_channel(P, 11);                  /* a channel created AFTER a grow */
    for (i=0;i<3;i++){ payload[0]=0xAA; if(pcN) dart_channel_send(pcN,payload,1); dart_node_poll(P,1); dart_node_poll(S,2); }
    for (t=0;t<200 && dg_recv[11]<3;t++){ dart_node_poll(P,1); dart_node_poll(S,2); }
    ST_CHECK(dg_recv[11]==3, "dyn-grow: post-grow channel delivers (got %d)", dg_recv[11]);
    dart_node_close(P,1); dart_node_close(S,1);
}

/* (17) QoS RxO: a DART_RELIABLE subscriber must REFUSE a best-effort publisher (no
   silent downgrade); every other direction matches. Sans-IO transport core, no data
   pump: build the publisher's interest, apply it to the reader, read the match. */
static unsigned long qos_incompat_n;
static void qos_on_event(const DartTransportEvent *ev){ if (ev->kind==DART_TRANSPORT_QOS_INCOMPATIBLE) qos_incompat_n++; }
static void qos_pair(int wrel, int rrel, uint16_t *recv_out, unsigned long *evt_out){
    DartChannelDef cw, cr; DartConfig wc, rc; void *mw, *mr; size_t nw, nr;
    DartTransportState *W, *R; uint8_t blob[128]; size_t bl; uint16_t pub=0, recv=0;
    memset(&cw,0,sizeof cw); cw.name="qostopic"; cw.role=DART_PUB_ONLY;
    cw.qos.reliability=wrel?DART_RELIABLE:DART_BEST_EFFORT; cw.qos.keep_last=4;
    memset(&cr,0,sizeof cr); cr.name="qostopic"; cr.role=DART_SUB_ONLY;
    cr.qos.reliability=rrel?DART_RELIABLE:DART_BEST_EFFORT; cr.qos.keep_last=4;
    memset(&wc,0,sizeof wc); wc.channels=&cw; wc.n_channels=1; wc.max_peers=2;
    memset(&rc,0,sizeof rc); rc.channels=&cr; rc.n_channels=1; rc.max_peers=2; rc.on_event=qos_on_event;
    nw=dart_required_memory(&wc); mw=malloc(nw); W=dart_init(mw,nw,&wc);
    nr=dart_required_memory(&rc); mr=malloc(nr); R=dart_init(mr,nr,&rc);
    dart_peer_add(W,2u,DART_FRAG_PAYLOAD); dart_peer_add(R,1u,DART_FRAG_PAYLOAD);
    qos_incompat_n=0;
    bl=dart_build_interest(W,blob,sizeof blob); dart_apply_peer_interest(R,1u,blob,bl);
    dart_peer_match_counts(R,1u,&pub,&recv);
    if (recv_out) *recv_out=recv;
    if (evt_out)  *evt_out=qos_incompat_n;
    dart_destroy(W); dart_destroy(R); free(mw); free(mr);
}
static void qos_match_checks(void){
    uint16_t recv; unsigned long evt;
    qos_pair(0,1,&recv,&evt);   /* best-effort pub, reliable sub: REFUSED */
    ST_CHECK(recv==0, "qos: reliable sub refuses best-effort pub (receive_from=%u)", recv);
    ST_CHECK(evt>=1,  "qos: refusal raised DART_QOS_INCOMPATIBLE (n=%lu)", evt);
    qos_pair(1,1,&recv,&evt);   /* reliable pub, reliable sub */
    ST_CHECK(recv==1 && evt==0, "qos: reliable sub matches reliable pub (recv=%u evt=%lu)", recv, evt);
    qos_pair(1,0,&recv,&evt);   /* reliable pub, best-effort sub: allowed downgrade */
    ST_CHECK(recv==1 && evt==0, "qos: best-effort sub matches reliable pub (recv=%u evt=%lu)", recv, evt);
    qos_pair(0,0,&recv,&evt);   /* both best-effort */
    ST_CHECK(recv==1 && evt==0, "qos: best-effort sub matches best-effort pub (recv=%u evt=%lu)", recv, evt);
}

/* (17b) flow control: a best-effort reader matched to a RELIABLE writer must NOT count
   toward backpressure (it never acks). Reliable writer + (rrel?reliable:best-effort)
   reader; fill the history ring past keep_last with no acks, return would-evict. */
static int beff_would_evict(int rrel){
    DartChannelDef cw, cr; DartConfig wc, rc; void *mw, *mr; size_t nw, nr;
    DartTransportState *W, *R; uint8_t blob[128], payload[8]; size_t bl; int i, evict;
    memset(&cw,0,sizeof cw); cw.name="beff"; cw.role=DART_PUB_ONLY;
    cw.qos.reliability=DART_RELIABLE; cw.qos.keep_last=2; cw.qos.max_message_bytes=8;
    memset(&cr,0,sizeof cr); cr.name="beff"; cr.role=DART_SUB_ONLY;
    cr.qos.reliability=rrel?DART_RELIABLE:DART_BEST_EFFORT; cr.qos.keep_last=2; cr.qos.max_message_bytes=8;
    memset(&wc,0,sizeof wc); wc.channels=&cw; wc.n_channels=1; wc.max_peers=2;
    memset(&rc,0,sizeof rc); rc.channels=&cr; rc.n_channels=1; rc.max_peers=2;
    nw=dart_required_memory(&wc); mw=malloc(nw); W=dart_init(mw,nw,&wc);
    nr=dart_required_memory(&rc); mr=malloc(nr); R=dart_init(mr,nr,&rc);
    dart_peer_add(W,2u,DART_FRAG_PAYLOAD); dart_peer_add(R,1u,DART_FRAG_PAYLOAD);
    bl=dart_build_interest(R,blob,sizeof blob); dart_apply_peer_interest(W,2u,blob,bl);  /* W learns R subscribes */
    memset(payload,0x5A,sizeof payload);
    for (i=0;i<5;i++) dart_send(W,0,payload,sizeof payload,1000u+(uint64_t)i);  /* 5 sends, keep_last=2: ring wraps */
    evict = dart_send_would_evict(W,0);
    dart_destroy(W); dart_destroy(R); free(mw); free(mr);
    return evict;
}
static void beff_flow_checks(void){
    ST_CHECK(beff_would_evict(0)==0, "flow: best-effort reader never stalls a reliable writer");
    ST_CHECK(beff_would_evict(1)==1, "flow: reliable reader does apply backpressure");
}

static int selftest_main(void){
    static uint8_t mem_w[1<<20], mem_r[1<<20];
    uint8_t payload[32]; unsigned i;
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: keep output on a crash */
    memset(payload, 0x5A, sizeof payload);

    DartChannelDef ch[4]; memset(ch, 0, sizeof ch);
    ch[0].name = "st/gap";
    ch[0].qos.reliability = DART_RELIABLE; ch[0].qos.keep_last = ST_DEPTH;
    ch[0].qos.max_message_bytes = 64; ch[0].qos.heartbeat_us = 50000;
    ch[1] = ch[0]; ch[1].name = "st/block"; ch[1].qos.backpressure_wait_us = ST_BLOCK_US;
    ch[2] = ch[0]; ch[2].name = "st/dyn";
    ch[2].qos.catch_up = ST_DEPTH;   /* phase 5 asserts ring replay on join */
    ch[3] = ch[0]; ch[3].name = "st/block2";
    ch[3].qos.backpressure_wait_us = ST_BLOCK_US; ch[3].qos.repair_delay_us = ST_NACK_US;

    /* disable_shm: phases 2-4b exercise the UDP reliability path (ring eviction,
       in-flight delivery, backpressure, sweep-ack). The same-host SHM fast path has
       its own coverage (shm_*_checks), and its chunk-recycle semantics differ, so the
       general transport phases stay on UDP -- as they did before (fixed, no allocator). */
    DartNodeOpts wo = { .domain = ST_DOMAIN, .disable_shm = 1 };
    { DartNodeOpts ro = wo; DartChannelDef chr[4]; DartNode *w, *r;
      memcpy(chr, ch, sizeof ch);
      ch[0].role = ch[1].role = ch[2].role = ch[3].role = DART_PUB_ONLY;
      chr[0].role = chr[1].role = chr[3].role = DART_SUB_ONLY;
      chr[2].role = DART_INACTIVE;

      w = test_node_open(mem_w, sizeof mem_w, NULL, NULL, NULL, wo, ch, 4);
      r = test_node_open(mem_r, sizeof mem_r, NULL, st_on_message, st_on_event, ro, chr, 4);
      if (!w || !r){ fprintf(stderr, "node open failed\n"); return 1; }

      /* 1. JOIN: writer streams while discovery completes; the reader must
            adopt the stream head silently (no gap for a late joiner) */
      { uint64_t end = dart_plat_now_us() + 5000000u;
        while (st_samples[ST_CH_GAP]==0 && dart_plat_now_us() < end){
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
      { uint64_t t0 = dart_plat_now_us(), dt;
        dart_node_send(w, ST_CH_BLOCK, payload, sizeof payload);   /* evicts un-acked */
        dt = dart_plat_now_us() - t0;
        ST_CHECK(dt >= ST_BLOCK_US-10000 && dt < 4*ST_BLOCK_US,
                 "blocked: send waited ~backpressure_wait_us (%.1f ms)", dt/1000.0);
      }

      /* 4. RELEASED: let the reader catch up and ack; sends are instant */
      st_pump(w, r, 300);
      { uint64_t t0 = dart_plat_now_us(), dt;
        dart_node_send(w, ST_CH_BLOCK, payload, sizeof payload);
        dt = dart_plat_now_us() - t0;
        ST_CHECK(dt < 20000, "released: acked ring sends instantly (%.1f ms)", dt/1000.0);
        st_pump(w, r, 100);
        /* the blocked-phase eviction dropped only history already in flight to
           the reader's socket, so nothing was lost: every send must have been
           delivered and no gap reported */
        ST_CHECK(st_gap_calls[ST_CH_BLOCK] == 0 && st_samples[ST_CH_BLOCK] == ST_DEPTH+2,
                 "blocked: in-flight eviction loses nothing (gaps=%lu, samples=%lu/%u)",
                 st_gap_calls[ST_CH_BLOCK], st_samples[ST_CH_BLOCK], ST_DEPTH+2);
      }

      /* 4b. SWEEP-ACK: ST_CH_BLOCK2 has nack_delay>0, so the reader's ACKNACK is
            timer-armed and, once a burst fills the ring and the writer goes
            quiet, can be flushed ONLY by the periodic sweep, not a data event.
            A sub-only reader's data channel never advances next_seqno, so a
            sweep that skips next_seqno==0 channels starves that ack and every
            later send waits the full backpressure_wait_us. Fill the ring, go quiet long
            enough for the sweep, then a send that would evict must NOT block. */
      st_pump(w, r, 200);                              /* match + settle */
      { unsigned long s0 = st_samples[ST_CH_BLOCK2];
        for (i=0;i<ST_DEPTH;i++) dart_node_send(w, ST_CH_BLOCK2, payload, sizeof payload);
        st_pump(w, r, 200);                            /* reader drains burst; sweep must ack */
        ST_CHECK(st_samples[ST_CH_BLOCK2]-s0 == ST_DEPTH, "sweep-ack: ring delivered (%lu, want %u)",
                 st_samples[ST_CH_BLOCK2]-s0, ST_DEPTH);
        { uint64_t t0 = dart_plat_now_us(), dt;
          dart_node_send(w, ST_CH_BLOCK2, payload, sizeof payload);   /* would evict slot 0 */
          dt = dart_plat_now_us() - t0;
          ST_CHECK(dt < 20000,
                   "sweep-ack: sub-only reader's timer ack releases backpressure (%.1f ms)", dt/1000.0);
        }
      }

      /* 5. DYNAMIC: ST_CH_DYN is inactive on the reader; subscribe replays
            the cached ring, unsubscribe goes silent at the writer, resubscribe
            replays again. All joins are gap-free. */
      st_pump(w, r, 200);
      for (i=0;i<3;i++) dart_node_send(w, ST_CH_DYN, payload, sizeof payload);
      st_pump(w, r, 300);
      ST_CHECK(st_samples[ST_CH_DYN] == 0, "dynamic: inactive receives nothing (%lu)",
               st_samples[ST_CH_DYN]);
      dart_node_set_role(r, ST_CH_DYN, DART_SUB_ONLY);
      st_pump(w, r, 400);
      ST_CHECK(st_samples[ST_CH_DYN] == 3, "dynamic: subscribe replays cached history (%lu, want 3)",
               st_samples[ST_CH_DYN]);
      dart_node_send(w, ST_CH_DYN, payload, sizeof payload);
      st_pump(w, r, 300);
      ST_CHECK(st_samples[ST_CH_DYN] == 4, "dynamic: live sample delivered (%lu, want 4)",
               st_samples[ST_CH_DYN]);
      dart_node_set_role(r, ST_CH_DYN, DART_INACTIVE);
      st_pump(w, r, 300);                  /* let the new list reach the writer */
      for (i=0;i<5;i++) dart_node_send(w, ST_CH_DYN, payload, sizeof payload);
      st_pump(w, r, 300);
      ST_CHECK(st_samples[ST_CH_DYN] == 4, "dynamic: unsubscribed receives nothing (%lu)",
               st_samples[ST_CH_DYN]);
      dart_node_set_role(r, ST_CH_DYN, DART_SUB_ONLY);
      st_pump(w, r, 400);
      ST_CHECK(st_samples[ST_CH_DYN] == 4+ST_DEPTH, "dynamic: resubscribe replays ring (%lu, want %u)",
               st_samples[ST_CH_DYN], 4+ST_DEPTH);
      ST_CHECK(st_gap_calls[ST_CH_DYN] == 0, "dynamic: joins are silent (gaps=%lu)",
               st_gap_calls[ST_CH_DYN]);

      /* 5b. FLAP / EPOCH GUARD: the reader rebuilds its transport state for the
            writer (a one-sided flap: only one side saw the peer go down). The
            writer's lanes still describe the dead incarnation; the changed reader
            EPOCH in the first ACKNACK must make every writer lane re-join and replay
            history. This is the regression guard for the epoch itself (also covers a
            reader restart that reuses its ip:port before discovery learns the new
            uuid). Re-applying the writer's interest stands in for the announce that
            re-discovery would deliver. */
      st_pump(w, r, 200);
      { unsigned long s0 = st_samples[ST_CH_DYN];
        uint32_t wid = 0; uint16_t k; uint8_t ib[256]; size_t il;
        for (k=0;k<dart_node_core_max_peers(r->core);k++) if (dart_node_core_peer_at(r->core,k,&wid,NULL,NULL,NULL)) break;
        dart_peer_remove(r->transport, wid);
        dart_peer_add(r->transport, wid,DART_FRAG_PAYLOAD);
        il = dart_build_interest(w->transport, ib, sizeof ib);
        dart_apply_peer_interest(r->transport, wid, ib, il);
        st_pump(w, r, 600);
        ST_CHECK(st_samples[ST_CH_DYN] == s0+ST_DEPTH,
                 "flap: writer re-joins new reader incarnation, replays ring (%lu, want %lu)",
                 st_samples[ST_CH_DYN], s0+ST_DEPTH);
        ST_CHECK(st_gap_calls[ST_CH_DYN] == 0, "flap: recovery is silent (gaps=%lu)",
                 st_gap_calls[ST_CH_DYN]);
      }

      /* 5c. RESUME: a discovery blip DROPS the peer on both sides without tearing
            down transport state. Dormant peers leave flow control, so new sends are
            withheld (the writer won't push to a dropped reader); a same-incarnation
            resume keeps the reader's deliver position, so the withheld backlog
            replays with no gap and no dup. This is the resume-model primitive that
            dart_peer_dormant/dart_peer_resume expose (the node drives them off
            discovery DROP/return). */
      st_pump(w, r, 200);
      { unsigned long s0 = st_samples[ST_CH_DYN], g0 = st_gap_calls[ST_CH_DYN];
        uint32_t wid = 0, rid = 0; uint16_t k;
        for (k=0;k<dart_node_core_max_peers(r->core);k++) if (dart_node_core_peer_at(r->core,k,&wid,NULL,NULL,NULL)) break;
        for (k=0;k<dart_node_core_max_peers(w->core);k++) if (dart_node_core_peer_at(w->core,k,&rid,NULL,NULL,NULL)) break;
        dart_peer_dormant(w->transport, rid);   /* writer drops the reader from flow control */
        dart_peer_dormant(r->transport, wid);   /* reader stops acking the writer */
        for (i=0;i<3;i++) dart_node_send(w, ST_CH_DYN, payload, sizeof payload);
        st_pump(w, r, 300);
        ST_CHECK(st_samples[ST_CH_DYN] == s0, "resume: dormant peer withholds sends (%lu, want %lu)",
                 st_samples[ST_CH_DYN], s0);
        dart_peer_resume(w->transport, rid);
        dart_peer_resume(r->transport, wid);
        st_pump(w, r, 400);
        ST_CHECK(st_samples[ST_CH_DYN] == s0+3,
                 "resume: backlog replays from preserved position (%lu, want %lu)",
                 st_samples[ST_CH_DYN], s0+3);
        ST_CHECK(st_gap_calls[ST_CH_DYN] == g0, "resume: lossless, no gap (gaps=%lu, want %lu)",
                 st_gap_calls[ST_CH_DYN], g0);
      }

      dart_node_close(r, 1);
      dart_node_close(w, 1);
    }

    /* 6. SCALE: 40 channels; the full interest list rides one discovery announce
          blob (IP-fragmented if large), matched at peer_up */
    { static uint8_t mem_a[1<<20], mem_b[1<<20];
      static DartChannelDef cha[ST_NCH], chb[ST_NCH];
      static char snames[ST_NCH][12];     /* "scale/0".."scale/39" */
      DartNodeOpts ao, bo; DartNode *a, *b; uint16_t k;
      memset(cha, 0, sizeof cha);
      for (k=0;k<ST_NCH;k++){
          sprintf(snames[k], "scale/%u", k); cha[k].name = snames[k];
          cha[k].qos.reliability = DART_RELIABLE;
          cha[k].qos.keep_last = 1;
          cha[k].qos.catch_up = 1;      /* sent before discovery completes */
          cha[k].qos.max_message_bytes = 32;
          cha[k].qos.heartbeat_us = 50000;
          cha[k].role = DART_PUB_ONLY;
      }
      memcpy(chb, cha, sizeof cha);
      for (k=0;k<ST_NCH;k++) chb[k].role = DART_SUB_ONLY;
      ao = (DartNodeOpts){ .domain = ST_DOMAIN+1 };
      bo = ao;
      a = test_node_open(mem_a, sizeof mem_a, NULL, NULL, NULL, ao, cha, ST_NCH);
      b = test_node_open(mem_b, sizeof mem_b, NULL, st_on_message, st_on_event, bo, chb, ST_NCH);
      ST_CHECK(a && b, "scale: %u-channel nodes open", ST_NCH);
      if (a && b){
          st_any = 0;
          for (k=0;k<ST_NCH;k++) dart_node_send(a, k, payload, 16);
          { uint64_t end = dart_plat_now_us() + 5000000u;
            while (st_any < ST_NCH && dart_plat_now_us() < end) st_pump(a, b, 20); }
          ST_CHECK(st_any == ST_NCH, "scale: all channels delivered (%lu/%u)", st_any, ST_NCH);
          dart_node_close(b, 1);
          dart_node_close(a, 1);
      }
    }

    /* 7. NAMED: the cross-peer identity is the topic NAME (its 64-bit hash),
          independent of each node's local channel handle. A matching name matches
          across differing handles; a distinct name never cross-wires; and clean
          names raise no false collision. */
    { static uint8_t mem_nw[1<<20], mem_nr[1<<20];
      DartChannelDef nw[1], nr[2];
      DartNodeOpts wo2, ro2; DartNode *w2, *r2;
      memset(nw,0,sizeof nw); memset(nr,0,sizeof nr);
      nw[0].name="robot/lidar"; nw[0].role=DART_PUB_ONLY;
      nw[0].qos.reliability=DART_RELIABLE; nw[0].qos.keep_last=1;
      nw[0].qos.catch_up=1; nw[0].qos.max_message_bytes=32; nw[0].qos.heartbeat_us=50000;
      nr[0]=nw[0]; nr[0].role=DART_SUB_ONLY;   /* same name (index 0), other node */
      nr[1]=nw[0]; nr[1].name="sensors/imu"; nr[1].role=DART_SUB_ONLY;   /* index 1 */
      wo2 = (DartNodeOpts){ .domain=ST_DOMAIN+2, .discovery={ .max_peers=4 } };
      ro2=wo2;
      st_samples[0]=st_samples[1]=0; st_collisions=0; st_last_sender[0]='\0';
      w2=test_node_open(mem_nw,sizeof mem_nw,"lidar-node",NULL,NULL,wo2,nw,1);
      r2=test_node_open(mem_nr,sizeof mem_nr,"reader-node",st_on_message,st_on_event,ro2,nr,2);
      ST_CHECK(w2 && r2, "named: nodes open");
      if (w2 && r2){
          uint64_t end = dart_plat_now_us() + 5000000u;
          while (st_samples[0]==0 && dart_plat_now_us()<end){
              dart_node_send(w2, 0, payload, 16); st_pump(w2,r2,20);
          }
          ST_CHECK(st_samples[0] > 0, "named: same name matches across nodes (%lu)", st_samples[0]);
          /* the node name is synced via discovery and surfaces as DartMsg.sender_name */
          ST_CHECK(strcmp(st_last_sender, "lidar-node")==0,
                   "named: sender_name carries the publisher's node name (%s)", st_last_sender);
          st_pump(w2,r2,200);
          ST_CHECK(st_samples[1] == 0, "named: distinct name never cross-wires (%lu)", st_samples[1]);
          ST_CHECK(st_collisions == 0, "named: clean names raise no collision (%lu)", st_collisions);
          dart_node_close(r2,1); dart_node_close(w2,1);
      }
    }

    /* 8. COLLISION: two distinct names with the SAME 64-bit identity, found by
          Pollard's rho (see examples/collide.c). A publishes one, B subscribes
          the other; DART must fire on_collision and refuse the match, never
          cross-wiring. The pair is tied to the FNV-1a dart_topic_id: if that
          ever changes, the first check fails loudly (regenerate via collide). */
    { static uint8_t mem_cw[1<<20], mem_cr[1<<20];
      const char *A="iuZA9tcJzAG", *B="5wVGxhTCmOC";   /* both -> 23f58aa8628b1cce */
      DartChannelDef cw, cr; DartNodeOpts wo3, ro3; DartNode *w3, *r3;
      ST_CHECK(dart_topic_id(A)==dart_topic_id(B) && strcmp(A,B)!=0,
               "collision: test pair still shares one identity (else regen via collide)");
      memset(&cw,0,sizeof cw);
      cw.name=A; cw.role=DART_PUB_ONLY;
      cw.qos.reliability=DART_RELIABLE; cw.qos.keep_last=1; cw.qos.catch_up=1;
      cw.qos.max_message_bytes=32; cw.qos.heartbeat_us=50000;
      cr=cw; cr.name=B; cr.role=DART_SUB_ONLY;
      wo3 = (DartNodeOpts){ .domain=ST_DOMAIN+3, .discovery={ .max_peers=4 } };
      ro3=wo3;
      st_samples[0]=0; st_collisions=0;
      w3=test_node_open(mem_cw,sizeof mem_cw,NULL,NULL,NULL,wo3,&cw,1);
      r3=test_node_open(mem_cr,sizeof mem_cr,NULL,st_on_message,st_on_event,ro3,&cr,1);
      ST_CHECK(w3 && r3, "collision: nodes open");
      if (w3 && r3){
          for (i=0;i<60;i++){ dart_node_send(w3,0,payload,16); dart_node_poll(w3,0); dart_node_poll(r3,20); }
          ST_CHECK(st_collisions >= 1, "collision: detected (DART_NAME_COLLISION fired %lu)", st_collisions);
          ST_CHECK(st_samples[0] == 0, "collision: match refused, no cross-wire (%lu)", st_samples[0]);
          dart_node_close(r3,1); dart_node_close(w3,1);
      }
    }
    disc_core_checks();   /* 8. discovery-core peer lifecycle (sans-IO) */
    node_core_checks();   /* 8b. node-core peer table + lifecycle (sans-IO, no sockets) */
#ifdef DART_SHM
    shm_module_checks();  /* 9.  SHM mapping module + seqlock guards          */
    shm_loss_checks();    /* 10. SHM loss/repair/skip (transport core)        */
    shm_node_checks();    /* 11. SHM full-node: size classes + inline fallback */
#endif
    unit_checks();        /* 12. pure-helper unit checks: clamp, result codes, byte packing */
    open_fail_checks();   /* 13. dart_node_open staged-cleanup (goto fail) paths             */
    event_user_checks();  /* 14. transport-fired event reaches on_event with the app user_data */
    dynamic_grow_checks();        /* 16. dynamic-mode grow: relocate mid-stream, lose nothing       */
    qos_match_checks();           /* 17. QoS RxO: reliable sub refuses best-effort pub (no downgrade) */
    beff_flow_checks();           /* 17b. best-effort reader stays out of a reliable writer's flow control */

    printf(st_fail ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    return st_fail;
}

/* ===================== sweep: cross-platform latency sweep =============== *
 * Spawns N "dart_test node" children per rate (stdout to a temp file), waits
 * for their self-exit, and aggregates the SUMMARY (+SUMMARY2) lines into a
 * table. Pure C process orchestration: no shell, runs on Windows and POSIX. */

#define SW_MAX_NODES 64

/* control plane (two-machine sweeps), constants shared by sweep and serve */
#define CTL_DOMAIN     9     /* keep test --domain away from this */
#define CTL_CMD        0     /* channel handles are array indices */
#define CTL_RES        1
#define CTL_RES_MAX    1400
#define SW_MAX_RESULTS 128

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

static void sw_sleep_ms(int ms){
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts; ts.tv_sec = ms/1000; ts.tv_nsec = (long)(ms%1000)*1000000L;
    nanosleep(&ts, NULL);
#endif
}

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
static const char *sw_kv_gets(const sw_kv *o, const char *key){
    int i; for (i=0;i<o->n;i++) if (strcmp(o->keys[i],key)==0) return o->vals[i];
    return NULL;
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
                    int domain, long rate, int dur, int mcast, int rel, int blk,
                    int xch, int spread, const char *ifip, const char *peerip){
    if (!ifip   || !*ifip)   ifip   = "0";
    if (!peerip || !*peerip) peerip = "0";
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
    snprintf(cmd, sizeof cmd, "\"%s\" node %s %d %ld %d %d %d %d %d %d %s %s",
             self, name, domain, rate, dur, mcast, rel, blk, xch, spread, ifip, peerip);
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
        char sd[16], sr[24], su[16], sm[8], se[8], sb[16], sx[16], sp[8];
        snprintf(sd,sizeof sd,"%d",domain);  snprintf(sr,sizeof sr,"%ld",rate);
        snprintf(su,sizeof su,"%d",dur);     snprintf(sm,sizeof sm,"%d",mcast);
        snprintf(se,sizeof se,"%d",rel);     snprintf(sb,sizeof sb,"%d",blk);
        snprintf(sx,sizeof sx,"%d",xch);     snprintf(sp,sizeof sp,"%d",spread);
        if (!freopen(c->out, "w", stdout)) _exit(126);
        { char *av[] = { (char*)self, "node", (char*)name, sd, sr, su, sm, se, sb, sx, sp,
                         (char*)ifip, (char*)peerip, NULL };
          execvp(self, av); }
        _exit(127);
    }
    c->pid = pid; c->reaped = 0;
    return 0;
#endif
}

/* wait for children to self-exit, pumping the control node (if any) so the
 * control-plane discovery and result channel stay live meanwhile; kill
 * stragglers at the deadline. cmd (optional) is re-published every 2s so a
 * worker that lost the control plane and recovered still hears about the run
 * (workers dedup by run nonce). */
static void sw_wait_pump(sw_child *cs, int n, int timeout_ms, DartNode *ctl, const char *cmd){
    uint64_t deadline = now_ns() + (uint64_t)timeout_ms*1000000ull;
    uint64_t next_cmd = 0;
    int left = 0, i;
    for (i=0;i<n;i++) if (!cs[i].reaped) left++;
    while (left > 0 && now_ns() < deadline){
        if (ctl) dart_node_poll(ctl, 20); else sw_sleep_ms(5);
        if (ctl && cmd && now_ns() >= next_cmd){
            dart_node_send(ctl, CTL_CMD, cmd, strlen(cmd));
            next_cmd = now_ns() + 2000000000ull;
        }
        for (i=0;i<n;i++){
            if (cs[i].reaped) continue;
#ifdef _WIN32
            if (WaitForSingleObject(cs[i].h, 0) == WAIT_OBJECT_0){
                CloseHandle(cs[i].h); cs[i].reaped=1; left--;
            }
#else
            int st;
            if (waitpid(cs[i].pid, &st, WNOHANG) == cs[i].pid){ cs[i].reaped=1; left--; }
#endif
        }
    }
    for (i=0;i<n;i++){
        if (cs[i].reaped) continue;
#ifdef _WIN32
        TerminateProcess(cs[i].h, 1); CloseHandle(cs[i].h);
#else
        kill(cs[i].pid, SIGKILL); waitpid(cs[i].pid, NULL, 0);
#endif
        cs[i].reaped = 1;
    }
}

/* ---- control plane: two-machine sweeps over DART itself ----------------- *
 * dart_test serve            on the other machine(s): a worker that waits on
 * a control domain, spawns the same node children the coordinator does, and
 * publishes each child's raw SUMMARY lines back.
 * dart_test sweep --remote   the coordinator: per rate it publishes one kv
 * command (reliable CMD channel, catch_up 0 so stale commands never replay
 * to late workers) and collects results (reliable RES channel, catch_up =
 * depth so results survive a control-peer flap; entries are tagged with
 * domain + run nonce so replays of older runs are filtered, not recounted).
 * Workers HELLO at startup so the coordinator knows how many results to
 * expect. Test nodes themselves discover each other over the LAN as usual:
 * both machines must share a subnet (announce TTL is 1). */
static char g_ctl_cmd[512];
static int  g_ctl_cmd_new = 0;
static char g_ctl_res[SW_MAX_RESULTS][CTL_RES_MAX];
static int  g_ctl_res_n = 0, g_ctl_done = 0, g_ctl_res_drop = 0;
static int  g_ctl_dom_filter = -1;   /* accept RESULTs for this domain only */
static char g_ctl_workers[16][24];
static int  g_ctl_nworkers = 0;

static void ctl_on_message(const DartMsg *msg){
    uint16_t ch = msg->channel_id; const void *d = msg->data; size_t len = msg->len;
    if (ch==CTL_CMD && len < sizeof g_ctl_cmd){
        memcpy(g_ctl_cmd, d, len); g_ctl_cmd[len]=0; g_ctl_cmd_new=1;
    } else if (ch==CTL_RES){
        /* every control-peer flap replays the worker's whole RES history (by
           design: that is how results survive an outage). Old-rate replays
           must not eat inbox slots, so filter by the rate's domain up front. */
        if (g_ctl_dom_filter >= 0 && len > 7 && !memcmp(d, "RESULT ", 7)){
            char head[48]; int dd = -1;
            size_t hl = len < sizeof head-1 ? len : sizeof head-1;
            memcpy(head, d, hl); head[hl] = 0;
            sscanf(head, "RESULT domain=%d", &dd);
            if (dd != g_ctl_dom_filter) return;
        }
        if (g_ctl_res_n < SW_MAX_RESULTS && len < CTL_RES_MAX){
            memcpy(g_ctl_res[g_ctl_res_n], d, len); g_ctl_res[g_ctl_res_n][len]=0;
            g_ctl_res_n++;
        } else g_ctl_res_drop++;
    }
}

static DartNode *ctl_open(uint16_t domain, int coordinator,
                         const char *if_ip, const char *peer_ip){
    static uint8_t mem[8<<20];
    static DartChannelDef ch[2];
    static DartDiscoveryAddr seed;
    DartNodeOpts opts;
    memset(ch, 0, sizeof ch);
    ch[0].name                  = "ctl/cmd";
    ch[0].qos.reliability       = DART_RELIABLE;
    ch[0].qos.keep_last         = 8;
    ch[0].qos.max_message_bytes = sizeof g_ctl_cmd;
    ch[0].role = coordinator ? DART_PUB_ONLY : DART_SUB_ONLY;
    ch[1] = ch[0];
    ch[1].name                  = "ctl/res";
    ch[1].qos.keep_last         = SW_MAX_RESULTS;
    ch[1].qos.catch_up          = SW_MAX_RESULTS;
    ch[1].qos.max_message_bytes = CTL_RES_MAX;
    ch[1].role = coordinator ? DART_SUB_ONLY : DART_PUB_ONLY;
    opts = (DartNodeOpts){
        .domain     = domain,
        .net  = { .multicast_interface = if_ip },     /* pin on multihomed hosts */
        .discovery = { .announce_interval_us = 500000, /* control plane must ride through
                                              data floods: announce harder and tolerate
                                              longer announce gaps */
                       .peer_timeout_us  = 10000000 },
    };
    if (peer_ip){                      /* bootstrap without multicast */
        uint32_t a4 = inet_addr(peer_ip);
        if (a4 != INADDR_NONE){
            memset(&seed, 0, sizeof seed);
            memcpy(seed.ip, &a4, 4); seed.ip_len = 4;
            opts.net.seed_peers = &seed; opts.net.n_seed_peers = 1;
        }
    }
    return test_node_open(mem, sizeof mem, NULL, ctl_on_message, NULL, opts, ch, 2);
}

/* IP of the first control peer (the other machine), for seeding the test
 * children; NULL if none known yet */
static const char *ctl_peer_ip(DartNode *ctl, char out[20]){
    uint16_t i;
    for (i=0;i<dart_node_core_max_peers(ctl->core);i++){
        uint8_t pip[16], pil;
        if (dart_node_core_peer_at(ctl->core, i, NULL, pip, &pil, NULL) && pil==4){
            snprintf(out, 20, "%u.%u.%u.%u", pip[0], pip[1], pip[2], pip[3]);
            return out;
        }
    }
    return NULL;
}

/* log control-peer transitions: which side lost whom, and when, is the first
 * question in any two-machine debugging session */
static void ctl_watch(const char *who, DartNode *ctl){
    static int had = 0;
    static uint64_t t0 = 0;
    char pb[20];
    int has = ctl_peer_ip(ctl, pb) != NULL;
    if (!t0) t0 = now_ns();
    if (has != had){
        printf("%s: [t=%.1fs] control peer %s%s%s\n", who,
               (now_ns()-t0)/1e9, has?"up (":"DOWN", has?pb:"", has?")":"");
        had = has;
    }
}

/* consume new inbox entries: HELLOs grow the worker set; RESULTs matching
 * (domain, run) parse into sum/sum2 at *count. Returns results consumed. */
static int ctl_drain(int domain, unsigned long run, sw_kv *sum, sw_kv *sum2, int *count){
    int got = 0;
    for (; g_ctl_done < g_ctl_res_n; g_ctl_done++){
        const char *e = g_ctl_res[g_ctl_done];
        if (!strncmp(e, "HELLO ", 6)){
            int k, known = 0;
            for (k=0;k<g_ctl_nworkers;k++) if (!strcmp(g_ctl_workers[k], e+6)) known=1;
            if (!known && g_ctl_nworkers < 16){
                snprintf(g_ctl_workers[g_ctl_nworkers], sizeof g_ctl_workers[0], "%s", e+6);
                g_ctl_nworkers++;
                printf("sweep: worker %s joined\n", e+6);
            }
        } else if (!strncmp(e, "RESULT ", 7) && sum && count && *count < SW_MAX_RESULTS){
            int d = -1; unsigned long r = 0;
            sscanf(e, "RESULT domain=%d run=%lu", &d, &r);
            if (d==domain && r==run){
                const char *p1 = strstr(e, "\nSUMMARY "), *p2 = strstr(e, "\nSUMMARY2 ");
                char line[2048]; size_t L;
                if (!p1) continue;
                L = strcspn(p1+1, "\n"); if (L >= sizeof line) L = sizeof line-1;
                memcpy(line, p1+1, L); line[L]=0;
                sum[*count].n = 0; sum2[*count].n = 0;
                sw_kv_parse(line+8, &sum[*count]);
                /* a flap replays this rate's earlier results too: same node
                   reporting twice must not skew the aggregates */
                { const char *nm = sw_kv_gets(&sum[*count], "name");
                  int k, dup = 0;
                  for (k=0;k<*count;k++){
                      const char *nm2 = sw_kv_gets(&sum[k], "name");
                      if (nm && nm2 && !strcmp(nm, nm2)){ dup=1; break; }
                  }
                  if (dup) continue;
                }
                if (p2){
                    L = strcspn(p2+1, "\n"); if (L >= sizeof line) L = sizeof line-1;
                    memcpy(line, p2+1, L); line[L]=0;
                    sw_kv_parse(line+9, &sum2[*count]);
                }
                (*count)++; got++;
            }
        }
    }
    return got;
}

/* last SUMMARY/SUMMARY2 lines of a child's output, wrapped for the RES channel */
static int sw_read_result(const char *path, char *out, size_t cap, int domain, unsigned long run){
    FILE *f = fopen(path, "rb"); char line[1024], s1[1024]="", s2[1024]="";
    if (!f) return 0;
    while (fgets(line, sizeof line, f)){
        if (!strncmp(line, "SUMMARY2 ", 9)) snprintf(s2, sizeof s2, "%s", line);
        else if (!strncmp(line, "SUMMARY ", 8)) snprintf(s1, sizeof s1, "%s", line);
    }
    fclose(f);
    if (!s1[0]) return 0;
    snprintf(out, cap, "RESULT domain=%d run=%lu\n%s%s", domain, run, s1, s2);
    return 1;
}

static int worker_main(int argc, char **argv){
    uint16_t dom = CTL_DOMAIN;
    char prefix[8], hello[32];
    const char *self; char tmpdir[260]; unsigned long mypid;
    const char *if_ip = NULL, *peer_arg = NULL;
    static sw_child cs[SW_MAX_NODES];
    DartNode *ctl;
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    for (i=2;i<argc;i++){
        if (!strcmp(argv[i],"--domain") && i+1<argc) dom=(uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i],"--if")   && i+1<argc) if_ip   = argv[++i];
        else if (!strcmp(argv[i],"--peer") && i+1<argc) peer_arg = argv[++i];
    }

#ifdef _WIN32
    { static char selfbuf[1024]; GetModuleFileNameA(NULL, selfbuf, sizeof selfbuf); self=selfbuf; }
    GetTempPathA(sizeof tmpdir, tmpdir);
    mypid = (unsigned long)GetCurrentProcessId();
#else
    self = argv[0];
    { const char *td=getenv("TMPDIR"); if(!td||!*td) td="/tmp";
      snprintf(tmpdir,sizeof tmpdir,"%s/",td); }
    mypid = (unsigned long)getpid();
#endif
    snprintf(prefix, sizeof prefix, "w%02lu", mypid%100);

    ctl = ctl_open(dom, 0, if_ip, peer_arg);
    if (!ctl){ fprintf(stderr, "serve: control node open failed\n"); return 1; }
    snprintf(hello, sizeof hello, "HELLO %s", prefix);
    dart_node_send(ctl, CTL_RES, hello, strlen(hello));
    printf("serve: worker %s on control domain %u, waiting for sweeps\n", prefix, dom);

    { unsigned long last_run = (unsigned long)-1;
    for (;;){
        sw_kv c; unsigned long run;
        int domain, dur, mcast, rel, blk, xch, spread, nodes; long rate;
        dart_node_poll(ctl, 100);
        ctl_watch("serve", ctl);
        if (!g_ctl_cmd_new) continue;
        g_ctl_cmd_new = 0;
        c.n = 0; sw_kv_parse(g_ctl_cmd, &c);
        domain=(int)sw_kv_get(&c,"domain",-1);  rate=(long)sw_kv_get(&c,"rate",0);
        dur   =(int)sw_kv_get(&c,"dur",5);      mcast=(int)sw_kv_get(&c,"mcast",0);
        rel   =(int)sw_kv_get(&c,"rel",0);      blk  =(int)sw_kv_get(&c,"blk",0);
        xch   =(int)sw_kv_get(&c,"xch",0);      spread=(int)sw_kv_get(&c,"spread",0);
        nodes =(int)sw_kv_get(&c,"nodes",0);    run  =(unsigned long)sw_kv_get(&c,"run",0);
        if (domain < 0 || nodes <= 0 || nodes > SW_MAX_NODES) continue;
        if (run == last_run) continue;          /* coordinator re-publishes */
        last_run = run;
        printf("serve: run %lu: %d nodes, domain %d, %ld Hz, %ds\n", run, nodes, domain, rate, dur);
        /* seed children with the coordinator's address so the test domain
           also bootstraps without multicast */
        { char pbuf[20];
          const char *cpeer = ctl_peer_ip(ctl, pbuf);
          if (!cpeer) cpeer = peer_arg;
          for (i=0;i<nodes;i++){
              char name[16];
              snprintf(cs[i].out, sizeof cs[i].out, "%sdartw_%lu_n%d.txt", tmpdir, mypid, i);
              snprintf(name, sizeof name, "%s.%d", prefix, i);
              if (sw_spawn(&cs[i], self, name, domain, rate, dur, mcast, rel, blk, xch, spread,
                           if_ip, cpeer) != 0){
                  fprintf(stderr, "serve: spawn %d failed\n", i);
                  cs[i].reaped = 1;
              }
          } }
        sw_wait_pump(cs, nodes, (dur+10)*1000, ctl, NULL);
        for (i=0;i<nodes;i++){
            char res[CTL_RES_MAX];
            if (sw_read_result(cs[i].out, res, sizeof res, domain, run))
                dart_node_send(ctl, CTL_RES, res, strlen(res));
            remove(cs[i].out);
        }
        { uint64_t end = now_ns()+1500000000ull;     /* flush + repair window */
          while (now_ns() < end) dart_node_poll(ctl, 20); }
        printf("serve: run %lu done\n", run);
    } }
}

/* aggregate helpers over an array of parsed SUMMARY kvsets */
static double sw_avg(const sw_kv *a, int n, const char *k){
    double s=0; int i,c=0; for (i=0;i<n;i++){ s+=sw_kv_get(&a[i],k,0); c++; } return c?s/c:0; }
static double sw_sum(const sw_kv *a, int n, const char *k){
    double s=0; int i; for (i=0;i<n;i++) s+=sw_kv_get(&a[i],k,0); return s; }
static double sw_max(const sw_kv *a, int n, const char *k){
    double m=0; int i; for (i=0;i<n;i++){ double v=sw_kv_get(&a[i],k,0); if(v>m)m=v; } return m; }

static int sweep_main(int argc, char **argv){
    int nodes=10, base_domain=20, dur=8, mcast=0, rel=0, blk=0, diag=0, xch=0, spread=0;
    int remote=0; uint16_t ctl_dom=CTL_DOMAIN; DartNode *ctl=NULL;
    const char *if_ip=NULL, *peer_arg=NULL;
    long rates[64]; int nrates=0, i, ri;
    const char *self;
    static sw_child  cs[SW_MAX_NODES];
    static sw_kv     sum[SW_MAX_RESULTS], sum2[SW_MAX_RESULTS];
    char tmpdir[260]; unsigned long mypid;

    for (i=2;i<argc;i++){
        if (!strcmp(argv[i],"--nodes")    && i+1<argc) nodes=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--domain")  && i+1<argc) base_domain=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--duration")&& i+1<argc) dur=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--mcast")   && i+1<argc) mcast=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--block-ms")&& i+1<argc) blk=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--extra-ch")&& i+1<argc) xch=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--spread"))   spread=1;
        else if (!strcmp(argv[i],"--void"))     spread=2;
        else if (!strcmp(argv[i],"--reliable")) rel=1;
        else if (!strcmp(argv[i],"--diag"))     diag=1;
        else if (!strcmp(argv[i],"--remote"))   remote=1;
        else if (!strcmp(argv[i],"--ctl-domain")&& i+1<argc) ctl_dom=(uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i],"--if")   && i+1<argc) if_ip=argv[++i];
        else if (!strcmp(argv[i],"--peer") && i+1<argc) peer_arg=argv[++i];
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

    if (remote){
        ctl = ctl_open(ctl_dom, 1, if_ip, peer_arg);
        if (!ctl){ fprintf(stderr, "sweep: control node open failed\n"); return 1; }
        printf("sweep: control domain %u, discovering workers...\n", ctl_dom);
        { uint64_t end = now_ns()+3000000000ull;
          while (now_ns() < end){ dart_node_poll(ctl, 50); ctl_drain(-1, 0, NULL, NULL, NULL); } }
        printf("sweep: %d worker(s), %d remote nodes per rate\n",
               g_ctl_nworkers, g_ctl_nworkers*nodes);
    }

    printf("sweep: %d nodes, %ds each, rates", nodes, dur);
    for (ri=0;ri<nrates;ri++) printf(" %ld", rates[ri]);
    printf("%s%s", rel?", reliable load":"", blk>0?" (block)":"");
    if (xch) printf(", +%d %s ch", xch, spread==2?"void":spread?"spread":"idle");
    printf("\n\n");
    printf("%10s %12s %10s %10s %10s %8s %9s %9s %6s\n",
           "TargetHz","Achieved/n","RTTavg ms","RTTmin ms","Jitter ms",
           "Drop%","Block ms","Stall ms","Nodes");

    for (ri=0; ri<nrates; ri++){
        long rate = rates[ri];
        int domain = base_domain + ri;     /* distinct domain per rate run */
        int count = 0, peers_n = 0, expect = nodes;
        unsigned long run = mypid*100u + (unsigned long)ri;
        double achieved, rttAvg=0, rttJit=0, rttMin=1e18, recv, drops, stall, blkMs, dpct;
        char cmd[256];

        if (remote){
            /* fresh inbox per rate: flap replays of past rates are filtered at
               insert (domain), so slots stay free for this rate's results */
            g_ctl_dom_filter = domain;
            g_ctl_res_n = 0; g_ctl_done = 0; g_ctl_res_drop = 0;
            snprintf(cmd, sizeof cmd,
                "domain=%d rate=%ld dur=%d mcast=%d rel=%d blk=%d xch=%d spread=%d nodes=%d run=%lu",
                domain, rate, dur, mcast, rel, blk, xch, spread, nodes, run);
            dart_node_send(ctl, CTL_CMD, cmd, strlen(cmd));
            for (i=0;i<10;i++) dart_node_poll(ctl, 1);   /* push it out now */
        }

        { char pbuf[20];
          const char *cpeer = remote ? ctl_peer_ip(ctl, pbuf) : NULL;
          if (!cpeer && remote) cpeer = peer_arg;
          for (i=0;i<nodes;i++){
              char name[16];
              snprintf(cs[i].out, sizeof cs[i].out, "%smwsweep_%lu_r%ld_n%d.txt",
                       tmpdir, mypid, rate, i);
              snprintf(name, sizeof name, "n%d", i);
              if (sw_spawn(&cs[i], self, name, domain, rate, dur, mcast, rel, blk, xch, spread,
                           if_ip, cpeer) != 0){
                  fprintf(stderr, "sweep: failed to spawn node %d\n", i);
                  cs[i].reaped = 1;
              }
          } }
        sw_wait_pump(cs, nodes, (dur+10)*1000, ctl, remote ? cmd : NULL);

        /* collect: compact the parsed SUMMARYs into [0..count) of sum[]/sum2[] */
        { sw_kv s, s2;
          for (i=0;i<nodes;i++){
              s.n = 0; s2.n = 0;
              if (sw_read_summary(cs[i].out, &s, &s2)){
                  sum[count] = s; sum2[count] = s2; count++;
              }
              remove(cs[i].out);
          }
        }
        if (remote){
            /* remote results: collected until every known worker reported its
               share or the window closes */
            uint64_t t0 = now_ns(), next_cmd = 0; int rgot = 0;
            for (;;){
                uint64_t lim = (uint64_t)(g_ctl_nworkers ? 15 : 5)*1000000000ull;
                dart_node_poll(ctl, 50);
                ctl_watch("sweep", ctl);
                if (now_ns() >= next_cmd){       /* keep recovered workers in sync */
                    dart_node_send(ctl, CTL_CMD, cmd, strlen(cmd));
                    next_cmd = now_ns() + 2000000000ull;
                }
                rgot += ctl_drain(domain, run, sum, sum2, &count);
                if (g_ctl_nworkers && rgot >= g_ctl_nworkers*nodes) break;
                if (now_ns() - t0 > lim) break;
            }
            expect = nodes*(1+g_ctl_nworkers);
            if (g_ctl_nworkers && rgot < g_ctl_nworkers*nodes)
                printf("    note: %d/%d remote results (inbox %d, dropped %d)\n",
                       rgot, g_ctl_nworkers*nodes, g_ctl_res_n, g_ctl_res_drop);
        }
        if (count==0){ printf("%10ld   (no SUMMARY captured)\n", rate); continue; }

        for (i=0;i<count;i++){
            if (sw_kv_get(&sum[i],"peers",0) > 0){
                rttAvg += sw_kv_get(&sum[i],"rtt_avg_ms",0);
                rttJit += sw_kv_get(&sum[i],"rtt_jit_ms",0);
                peers_n++;
            }
            { double rm = sw_kv_get(&sum[i],"rtt_min_ms",0); if (rm>0 && rm<rttMin) rttMin=rm; }
        }

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
               count, expect);

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
/* ===================== memscale: in-process memory/alloc scale test ===========
 * Spins up 1 reliable publisher + N subscribers on loopback and, for a grid of payload
 * size / keep_last / subscriber count, reports peak message-buffer memory, how many heap
 * (re)allocations happen during warmup vs steady state, end-to-end msg/s, and the cost of
 * a send call cold (allocating a new ring slot) vs warm (reusing it). The point: steady
 * state must be alloc-free (steady_alloc ~ 0) and warm sends must not pay an alloc cost. */
static int g_ms_rx;
static void ms_on_message(const DartMsg *m){ (void)m; g_ms_rx++; }

static unsigned ms_domain = 200;
/* sum heap (re)alloc counts across the publisher and every subscriber */
static uint64_t ms_allocs(DartNode *P, DartNode **S, int nsubs){
    uint64_t a, sum=0; int i; dart_node_mem_stats(P,NULL,NULL,&a); sum=a;
    for (i=0;i<nsubs;i++){ dart_node_mem_stats(S[i],NULL,NULL,&a); sum+=a; }
    return sum;
}
static void ms_run(size_t plen, uint16_t keep, int nsubs, int disable_shm){
    DartAllocator pa, sa[16]; DartNodeOpts po, so; DartNode *P=NULL, *S[16];
    DartChannel *pc=NULL; DartChannelOpts co; DartDiscoveryAddr seed;
    uint8_t *payload; int i, j, k, t, nwarm, nsteady; unsigned dom = ms_domain++;
    uint64_t a0=0, a1=0, a2=0; size_t ppeak=0, speak=0;
    double cold_sum=0, warm_sum=0, t0, t1, msg_s; int coldc=0;
    if (nsubs>16) nsubs=16;
    payload=(uint8_t*)malloc(plen?plen:1); if(!payload) return; memset(payload,0x5A,plen?plen:1);
    nsteady = plen<=1024 ? 300 : plen<=65536 ? 100 : 20;
    nwarm   = keep + 4;
    memset(&co,0,sizeof co); co.qos.reliability=DART_RELIABLE; co.qos.keep_last=keep; co.qos.heartbeat_us=50000;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=(uint16_t)dom; po.max_channels=4;
    po.discovery.max_peers=(uint16_t)(nsubs+2); po.disable_shm=(uint8_t)disable_shm;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    so=po;
    pa=dart_allocator_dynamic(0);
    P=dart_node_open(&pa,"ms-pub",NULL,NULL,&po);
    for (i=0;i<nsubs;i++){ sa[i]=dart_allocator_dynamic(0); S[i]=dart_node_open(&sa[i],"ms-sub",ms_on_message,NULL,&so); }
    if (!P){ free(payload); return; }
    pc=dart_node_create_channel(P,"ms/ch",DART_PUB_ONLY,&co);
    for (i=0;i<nsubs;i++) dart_node_create_channel(S[i],"ms/ch",DART_SUB_ONLY,&co);
    for (t=0;t<4000 && dart_channel_match_count(pc)<nsubs;t++){ dart_node_poll(P,1); for(j=0;j<nsubs;j++) dart_node_poll(S[j],1); }

    a0=ms_allocs(P,S,nsubs);                              /* total allocs before any traffic */
    g_ms_rx=0;                                            /* warmup: fill + size the buffers */
    for (i=0;i<nwarm;i++){
        double s0=(double)dart_plat_now_us(); dart_channel_send(pc,payload,plen); double s1=(double)dart_plat_now_us();
        if (i<keep){ cold_sum += s1-s0; coldc++; }
        for (k=0;k<400000 && g_ms_rx < (i+1)*nsubs;k++){ dart_node_poll(P,0); for(j=0;j<nsubs;j++) dart_node_poll(S[j],0); }
    }
    a1=ms_allocs(P,S,nsubs);                              /* allocs after warmup */
    g_ms_rx=0; t0=(double)dart_plat_now_us();             /* steady: buffers sized -> must not alloc */
    for (i=0;i<nsteady;i++){
        double s0=(double)dart_plat_now_us(); dart_channel_send(pc,payload,plen); double s1=(double)dart_plat_now_us();
        warm_sum += s1-s0;
        for (k=0;k<400000 && g_ms_rx < (i+1)*nsubs;k++){ dart_node_poll(P,0); for(j=0;j<nsubs;j++) dart_node_poll(S[j],0); }
    }
    t1=(double)dart_plat_now_us();
    a2=ms_allocs(P,S,nsubs);
    dart_node_mem_stats(P,NULL,&ppeak,NULL); dart_node_mem_stats(S[0],NULL,&speak,NULL);
    msg_s = (t1>t0) ? nsteady / ((t1-t0)/1e6) : 0;
    printf("%-9lu %-5u %-5d %11.1f %11.1f %11llu %13llu %10.0f %9.2f %9.2f\n",
        (unsigned long)plen, keep, nsubs, ppeak/1024.0, speak/1024.0,
        (unsigned long long)(a1-a0), (unsigned long long)(a2-a1),
        msg_s, coldc?cold_sum/coldc:0.0, nsteady?warm_sum/nsteady:0.0);
    dart_node_close(P,0); for(i=0;i<nsubs;i++) dart_node_close(S[i],0);
    free(payload);
}
static void ms_grid(int disable_shm){
    printf("\n--- %s path ---\n", disable_shm ? "UDP (cross-host; writer/reader buffers on the heap)"
                                              : "SHM (same-host; payload in mmap'd segments)");
    printf("%-9s %-5s %-5s %11s %11s %11s %13s %10s %9s %9s\n",
        "payload","keep","subs","pub_peak_kB","sub_peak_kB","warm_alloc","steady_alloc","msg/s","cold_us","warm_us");
    ms_run(64,8,1,disable_shm); ms_run(1024,8,1,disable_shm);
    ms_run(65536,8,1,disable_shm); ms_run(1048576,8,1,disable_shm);              /* payload sweep */
    ms_run(1024,1,1,disable_shm); ms_run(1024,16,1,disable_shm); ms_run(1024,256,1,disable_shm); /* history */
    ms_run(1024,8,4,disable_shm); ms_run(1024,8,16,disable_shm);                 /* subscriber sweep */
}
static int memscale_main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    printf("DART dynamic-allocator scale test (in-process, reliable, loopback)\n");
    printf("peak = live message-buffer bytes; warm/steady_alloc = heap (re)allocs (pub+subs); cold/warm_us = send-call time\n");
    ms_grid(1);   /* the allocation-relevant path */
    ms_grid(0);   /* same-host fast path, for comparison */
    return 0;
}

int main(int argc, char **argv){
    if (argc >= 2 && strcmp(argv[1], "memscale") == 0)
        return memscale_main();
    if (argc >= 2 && strcmp(argv[1], "node") == 0)
        return node_main(argc-1, argv+1);
    if (argc >= 2 && strcmp(argv[1], "selftest") == 0)
        return selftest_main();
    if (argc >= 2 && strcmp(argv[1], "sweep") == 0)
        return sweep_main(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "serve") == 0)
        return worker_main(argc, argv);
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
        "  node <name> [domain] [load_hz] [duration_s] [if_mode] [reliable] [block_ms]\n"
        "       [extra_ch] [spread] [if_ip] [peer_ip]\n"
        "        if_ip: pin the discovery interface (multihomed hosts);\n"
        "        peer_ip: seed discovery with this address (no multicast needed);\n"
        "        \"0\" = unset for either\n"
        "        latency/throughput node; SUMMARY+SUMMARY2 on timed exit. Data is unicast.\n"
        "        reliable=1: load channel DART_RELIABLE; block_ms: writer\n"
        "        backpressure window (qos.backpressure_wait_us) for that channel\n"
        "        extra_ch: declare N more channels; spread=1 round-robins the\n"
        "        load across them (0 = they stay idle, 2 = they are PUB_ONLY\n"
        "        everywhere so those writes have no readers)\n"
        "        if_mode: discovery interface -- 1 = loopback (single host), 2 = real NIC\n"
        "        env: DART_DIAG_RCVBUF/DART_DIAG_SNDBUF (bytes), DART_DIAG_TRACE\n"
        "  sweep [--nodes N] [--domain D] [--duration S] [--rates a,b,c]\n"
        "        [--mcast 0|1|2] [--reliable] [--block-ms N] [--extra-ch N] [--spread]\n"
        "        [--void] [--remote] [--ctl-domain D] [--if IP] [--peer IP] [--diag]\n"
        "        spawn N node children per rate; print an RTT-vs-throughput table.\n"
        "        --mcast selects the discovery interface (1 loopback, 2 NIC; data is unicast).\n"
        "        --remote also commands every 'serve' worker on the LAN to spawn N\n"
        "        nodes per rate and folds their SUMMARYs into the same table.\n"
        "        --if pins the discovery interface (multihomed hosts);\n"
        "        --peer seeds discovery with the other machine's address, so the\n"
        "        run works even where multicast is broken or filtered\n"
        "  serve [--domain D] [--if IP] [--peer IP]\n"
        "        two-machine sweep worker: waits on the control domain (default 9),\n"
        "        runs each commanded rate alongside the coordinator, reports back.\n"
        "        Machines must share a subnet (discovery TTL is 1)\n"
        "  sendbench\n"
        "        UDP send-cost microbench on loopback (Windows)\n"
        "  selftest\n"
        "        on_gap, backpressure, dynamic-interest functional test (exit 0 = pass)\n");
    return 2;
}
