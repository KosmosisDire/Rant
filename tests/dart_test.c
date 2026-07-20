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
/* selftest fault injection: force transport datagrams (submessage type byte 1..4) to
 * would-block, so the threaded phases can prove eviction is surfaced, never silent.
 * Discovery datagrams ('uDSC') pass. Windows only (the wrappers are absent on POSIX). */
static volatile int g_tx_block_data;
/* swallow 'uDTL' DETAIL_RESP datagrams destined to this port (0 = off): the destination
 * node then never verifies its candidates while everyone else converges normally, which
 * is how the match-wait phases hold one side of the detail exchange open on demand. */
static volatile unsigned g_tx_block_detail_resp_port;
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
    /* byte 0 is type|flags: DATA/HB/NACK are types 1..3 in the low bits ('uDSC'
       discovery datagrams land on 5 and pass through) */
    if (g_tx_block_data && len >= 1 && ((unsigned char)buf[0] & 0x07u) >= 1
                                    && ((unsigned char)buf[0] & 0x07u) <= 3){
        WSASetLastError(WSAEWOULDBLOCK);
        g_tx_calls++; g_tx_wouldblock++;
        return -1;
    }
    if (g_tx_block_detail_resp_port && len >= 6
        && buf[0]=='u' && buf[1]=='D' && buf[2]=='T' && buf[3]=='L' && (unsigned char)buf[4]==2
        && to && to->sa_family == AF_INET
        && ntohs(((const struct sockaddr_in*)to)->sin_port) == (unsigned short)g_tx_block_detail_resp_port){
        g_tx_calls++;
        return len;   /* swallowed, not would-blocked: the requester's own re-ask heals it later */
    }
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
 * The public node API is handle-based (dart_node_create_topic -> DartTopic*,
 * dart_topic_send, ...). These test-internal helpers keep the index-based call
 * sites concise: dart_node_topic(n, i) maps a creation index back to its handle,
 * so the old (node, topic-index) call form maps straight onto the handle calls. */
#define dart_node_send(n, idx, d, l)         dart_topic_send(dart_node_topic((n),(idx)), dart_bytes((d),(l)))
#define dart_node_set_role(n, idx, r)        dart_topic_set_role(dart_node_topic((n),(idx)), (r))
#define dart_node_drain(n, idx, ms)          dart_topic_drain(dart_node_topic((n),(idx)), (ms))
#define dart_node_publisher_match_count(n, idx) dart_topic_match_count(dart_node_topic((n),(idx)))
#define dart_node_repair_stats(n, idx, o)    dart_topic_repair_stats(dart_node_topic((n),(idx)), (o))
#define dart_node_subscriber_progress(n, idx, p, b, h, t) \
        dart_topic_subscriber_progress(dart_node_topic((n),(idx)), (p),(b),(h),(t))

/* Open a node and create its topics from a DartTopicDef array in index order, so
 * the array index is the topic handle index the shims above resolve. opts carries
 * everything that used to live in DartNodeConfig except topics/on_message. */
static DartNode *test_node_open(uint8_t *mem, size_t cap, const char *name, DartMsgFn on_msg,
                               DartEventFn on_event, DartNodeOpts opts, const DartTopicDef *chans, uint16_t nch){
    DartNode *node; uint16_t i; DartAllocator alloc = dart_allocator_dynamic(i_dart_plat_realloc, 0);
    (void)mem; (void)cap;             /* heap-backed: the node self-sizes (was a static arena) */
    if (!opts.max_topics) opts.max_topics = nch ? nch : 1;
    node = dart_node_open(&alloc, name, on_msg, on_event, &opts);
    if (!node) return NULL;
    for (i=0;i<nch;i++){
        DartTopicOpts co; memset(&co, 0, sizeof co);
        co.qos = chans[i].qos;
        if (!dart_node_create_topic(node, chans[i].name, (DartRole)chans[i].role, NULL, &co)){
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

#define CH_PROBE          0     /* topic handles are array indices         */
#define CH_LOAD           1
#define XCH_ID0           2     /* extra load topics: indices 2..2+xch-1   */
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
static int         g_xch = 0;         /* extra topics declared              */
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
    /* load / drops, tracked per topic: cross-topic arrival order is not
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
 * topics, detected wire loss on best-effort). */
static unsigned long      g_gap_evt = 0;
static unsigned long long g_gap_tus = 0;
/* backpressure cost, read from the node before SUMMARY */
static uint64_t g_wait_us = 0;
static uint32_t g_wait_n  = 0;

static void lat_on_event(const DartEvent *ev){
    if (ev->kind == DART_MSG_LOST){ g_gap_evt++; g_gap_tus += ev->lost_count; }
}

static void lat_on_message(const DartMsg *msg){
    uint16_t ch = msg->topic_index; uint32_t from = msg->publisher_id;
    const void *data = msg->data.data; size_t len = msg->data.len;
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

    static DartTopicDef ch[2+XCH_MAX];
    static char xnames[XCH_MAX][8];   /* "x0".."x511": extra topics' topic names */
    memset(ch, 0, sizeof ch);
    ch[0].name = "probe";
    /* depth must cover the pongs one tick can stage (one per peer) or the ring
       evicts them before the flush and RTT samples are lost */
    ch[0].qos.reliability = DART_BEST_EFFORT; ch[0].qos.keep_last = 16; ch[0].qos.max_message_bytes = 64;
    ch[1].name = "load";
    ch[1].qos.reliability = reliable ? DART_RELIABLE : DART_BEST_EFFORT;
    ch[1].qos.keep_last = LOAD_DEPTH; ch[1].qos.max_message_bytes = 64;
    ch[1].qos.backpressure_wait_us = (uint32_t)(block_ms > 0 ? block_ms : 0) * 1000u;
    { int i;     /* extra topics: idle (depth 1) or load-bearing when spread */
      for (i=0;i<g_xch;i++){
          ch[2+i] = ch[1];
          sprintf(xnames[i], "x%d", i); ch[2+i].name = xnames[i];
          ch[2+i].qos.keep_last = g_spread ? 128 : 1;
          if (g_spread==2) ch[2+i].role = DART_PUB_ONLY;   /* nobody subscribes */
      } }

    /* meta_max_ids is gone: the core auto-raises it to 2*n_topics, which
       already covers every extra topic here. disable_shm: the sweep measures the
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

    static uint8_t mem[48<<20];  /* deep load ring + extra topics x 64 peers */
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
            for (i = 0; i < (int)i_dart_node_core_max_peers(n->core); i++){
                uint32_t pid; uint8_t pip[16]; uint16_t pport;
                if (i_dart_node_core_peer_at(n->core, (uint16_t)i, &pid, pip, NULL, &pport))
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
 *   3. BLOCKED  : on a backpressure_wait_us topic, sends that would evict un-acked
 *                 history wait ~backpressure_wait_us while the reader never acks, then
 *                 proceed (KEEP_LAST fallback, never refusal).
 *   4. RELEASED : same topic once the reader acks; sends are instant.
 *   4b SWEEP-ACK: a sub-only reader whose ACKNACK is timer-armed (nack_delay>0)
 *                 must flush it via the periodic sweep when the writer goes
 *                 quiet; backpressure must release with no data event to ride.
 *   5. DYNAMIC  : reader flips a topic inactive/subscribed at runtime;
 *                 each (re)subscribe replays cached history with no gap.
 *   6. SCALE    : 40 topics, past the old 31-id announce cap. */

#define ST_DOMAIN   33
/* topic handles are array indices (declaration order in ch[]) */
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
static char st_last_sender[64];   /* publisher_name of the most recently delivered message */

static void st_on_message(const DartMsg *msg){
    if (msg->topic_index < 8) st_samples[msg->topic_index]++;
    st_last_sender[0] = '\0';
    if (msg->publisher_name.data){
        size_t n = msg->publisher_name.len < sizeof st_last_sender - 1 ? msg->publisher_name.len : sizeof st_last_sender - 1;
        memcpy(st_last_sender, msg->publisher_name.data, n); st_last_sender[n] = '\0';
    }
    st_any++;
}
static unsigned long st_collisions;
static void st_on_event(const DartEvent *ev){
    if (ev->kind == DART_MSG_LOST){
        if (ev->topic < 8){ st_gap_calls[ev->topic]++; st_gap_tus[ev->topic] += (unsigned long)ev->lost_count; }
    } else if (ev->kind == DART_ERROR && ev->error == DART_E_NAME_COLLISION){
        st_collisions++;
    }
}

static void st_pump(DartNode *a, DartNode *b, int ms){     /* run both nodes */
    uint64_t end = i_dart_plat_now_us() + (uint64_t)ms*1000u;
    while (i_dart_plat_now_us() < end){ dart_node_poll(a, 1); if (b) dart_node_poll(b, 0); }
}

/* v10 one-way link between RAW transports: hand src's interest to dst (which knows src
   as peer src_id) and run the pairwise detail exchange by hand, exactly as the two
   runtimes would. The announce only NOMINATES (32-bit hashes); the details verify; the
   re-apply forms the matches. */
static void st_apply_verified(DartTransportState *dst, uint32_t src_id, DartTransportState *src){
    uint8_t ib[512], rq[512], rp[2048]; size_t il, rl, pl; uint16_t nw;
    DartDetailWant wl[16];
    il = dart_transport_build_interest(src, ib, sizeof ib);
    dart_transport_apply_peer_interest(dst, src_id, dart_bytes(ib, il));
    nw = dart_transport_detail_wants(dst, NULL, src_id, dart_bytes(ib, il), wl, 16);
    if (!nw) return;                       /* nothing shared (or already verified) */
    rl = dart_detail_req_build(0, 0, wl, nw, rq, sizeof rq);
    pl = dart_transport_detail_respond(src, NULL, 0, dart_bytes(rq, rl), rp, sizeof rp);
    dart_transport_apply_peer_details(dst, src_id, dart_bytes(rp, pl));
    dart_transport_apply_peer_interest(dst, src_id, dart_bytes(ib, il));
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
    n=dc_mk(buf,1,0,99,5001,1); dart_discovery_on_datagram(st,sa,4,dart_bytes(buf,n),2000); idA=dc_up_id;
    n=dc_mk(buf,2,0,99,5002,1); dart_discovery_on_datagram(st,sb,4,dart_bytes(buf,n),2000); idB=dc_up_id;
    ST_CHECK(dc_up_n==2 && dart_discovery_peer_count(st)==2,
             "disc-core: two peers up (ups=%u count=%u)", dc_up_n, dart_discovery_peer_count(st));

    /* 2. a new peer is REFUSED when the table is full of ACTIVE peers */
    dc_up_n=dc_refused_n=0;
    n=dc_mk(buf,3,0,99,5003,1); dart_discovery_on_datagram(st,sc,4,dart_bytes(buf,n),2000);
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
    n=dc_mk(buf,1,0,99,5001,1); dart_discovery_on_datagram(st,sa,4,dart_bytes(buf,n),2100000);
    ST_CHECK(dc_up_n==1 && dc_up_id==idA && dart_discovery_peer_count(st)==1,
             "disc-core: same uuid resumes same id (up=%u sameid=%d count=%u)",
             dc_up_n, dc_up_id==idA, dart_discovery_peer_count(st));

    /* 5. a new peer now evicts the oldest DROPPED peer (B) as GONE */
    dc_down_n=dc_up_n=0;
    n=dc_mk(buf,3,0,99,5003,1); dart_discovery_on_datagram(st,sc,4,dart_bytes(buf,n),2100000);
    ST_CHECK(dc_down_n==1 && dc_down_id==idB && dc_down_reason==(int)DART_DISCOVERY_GONE && dc_up_n==1,
             "disc-core: new peer evicts oldest dropped as GONE (downs=%u sameid=%d reason=%d up=%u)",
             dc_down_n, dc_down_id==idB, dc_down_reason, dc_up_n);

    /* 6. BYE is GONE */
    dc_down_n=0;
    n=dc_mk(buf,1,0x01,99,5001,1); dart_discovery_on_datagram(st,sa,4,dart_bytes(buf,n),2100000);
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
      n=dc_mk(buf,7,0,99,6001,1); dart_discovery_on_datagram(st,sa,4,dart_bytes(buf,n),3000); idP=dc_up_id;
      dc_up_n=dc_down_n=0;
      n=dc_mk(buf,8,0,99,6001,1); dart_discovery_on_datagram(st,sa,4,dart_bytes(buf,n),3100);  /* new uuid, same ip:port */
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
    else if (ev->kind==DART_ERROR && ev->error==DART_E_PEER_REFUSED){ nc_refused_n++; }
}
/* feed the node core through a REAL (sans-IO) discovery core: build an announce datagram
   (discovery section [u16 port][u8 self_ip_len=0][u8 name_len][name], no overlay) so
   discovery populates its table and fires events INTO i_dart_node_core_on_disc_event.
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
    DartConfig tc; DartTransportState *tr; DartTopicDef ch[1];
    DartDiscoveryCoreConfig dcfg; DartDiscoveryState *st;
    i_DartNodeCoreConfig cc; i_DartNodeCore *nc;
    i_DartNodeDest d; uint32_t id, idA, idB; size_t n;

    memset(ch,0,sizeof ch); ch[0].name="nc/topic";
    memset(&tc,0,sizeof tc); tc.topics=ch; tc.n_topics=1; tc.max_peers=2;
    tr = dart_transport_init(tmem, sizeof tmem, &tc);
    ST_CHECK(tr!=NULL, "node-core: transport init");
    if (!tr) return;

    /* node core first (discovery bound once it exists, exactly like the runtime) */
    memset(&cc,0,sizeof cc);
    cc.transport=tr; cc.n_topics=1; cc.frag_size=1200; cc.on_event=nc_event;
    nc = i_dart_node_core_init(cmem, sizeof cmem, &cc);
    ST_CHECK(nc!=NULL, "node-core: init");
    if (!nc) return;

    /* the discovery core whose peer table the node core delegates to: reserve the node's
       per-peer scratch via peer_user_bytes; wire its events into the node core */
    memset(&dcfg,0,sizeof dcfg); memset(dcfg.uuid,0xEE,16);
    dcfg.domain_id=99; dcfg.announce_interval_us=1000000; dcfg.peer_timeout_us=1000000; dcfg.max_peers=2;
    dcfg.peer_user_bytes=i_dart_node_core_peer_user_bytes();
    dcfg.on_event=i_dart_node_core_on_disc_event; dcfg.user=nc;
    st = dart_discovery_init(dmem, sizeof dmem, &dcfg);
    ST_CHECK(st!=NULL, "node-core: discovery init");
    if (!st) return;
    i_dart_node_core_bind_discovery(nc, st);
    dart_discovery_update(st, 1000, out, sizeof out);   /* start */

    /* build-meta still works (uses the transport, not any peer table) */
    {   DartBytes mb;
        i_dart_node_core_build_meta(nc);
        mb = i_dart_node_core_meta(nc);
        ST_CHECK(mb.len>=5 && dart_meta_frag(mb)==1200,
                 "node-core: builds overlay (frag=%u)", dart_meta_frag(mb)); }

    /* 1. two peers announce -> two ups; discovery assigns the ids; resolve each way */
    nc_up_n=nc_down_n=nc_refused_n=0;
    n=nc_dgram(buf,1,0,99,5001,"nc-self",1); dart_discovery_on_datagram(st,sa,4,dart_bytes(buf,n),2000); idA=nc_up_id;
    n=nc_dgram(buf,2,0,99,5002,"nc-self",1); dart_discovery_on_datagram(st,sb,4,dart_bytes(buf,n),2000); idB=nc_up_id;
    ST_CHECK(nc_up_n==2, "node-core: two peers up (ups=%u)", nc_up_n);
    {   DartString pn = i_dart_node_core_peer_name(nc, idA);
        ST_CHECK(pn.data && pn.len==7 && memcmp(pn.data,"nc-self",7)==0,
                 "node-core: peer name learned from announce (%.*s)", (int)pn.len, pn.data?pn.data:"?"); }
    ST_CHECK(i_dart_node_core_resolve(nc,idA,&d) && d.port==5001 && d.ip[3]==1,
             "node-core: peer id resolves to addr (port=%u ip3=%u)", d.port, d.ip[3]);
    ST_CHECK(i_dart_node_core_id_for_addr(nc, sb, 5002, &id) && id==idB,
             "node-core: addr resolves to id (id=%u)", id);

    /* a nameless announce -> the peer name falls back to "unknown-peer" (never empty/NULL) */
    {   DartString pn;
        n=nc_dgram(buf,1,0,99,5001,NULL,2);     dart_discovery_on_datagram(st,sa,4,dart_bytes(buf,n),2001);
        pn = i_dart_node_core_peer_name(nc, idA);
        ST_CHECK(pn.data && pn.len==12 && memcmp(pn.data,"unknown-peer",12)==0,
                 "node-core: nameless announce -> unknown-peer (%.*s)", (int)pn.len, pn.data?pn.data:"(null)");
        n=nc_dgram(buf,1,0,99,5001,"nc-self",3); dart_discovery_on_datagram(st,sa,4,dart_bytes(buf,n),2002); }

    /* 2. a 3rd peer is REFUSED while the table is full of ACTIVE peers; the node forwards it */
    nc_refused_n=0;
    n=nc_dgram(buf,3,0,99,5003,"three",1); dart_discovery_on_datagram(st,sc,4,dart_bytes(buf,n),2003);
    ST_CHECK(nc_refused_n==1, "node-core: refused forwarded (refused=%u)", nc_refused_n);

    /* 3. silence past the timeout DROPS both: a PEER_DOWN each, but the slots are kept (resolve) */
    nc_down_n=0;
    dart_discovery_update(st, 1003000, out, sizeof out);
    ST_CHECK(nc_down_n==2, "node-core: timeout drops both (downs=%u)", nc_down_n);
    ST_CHECK(i_dart_node_core_resolve(nc,idA,&d)==1, "node-core: dropped peer kept (resolves)");

    /* 4. the same uuid returns -> RESUME re-fires PEER_UP under the same id */
    nc_up_n=0;
    n=nc_dgram(buf,1,0,99,5001,"nc-self",4); dart_discovery_on_datagram(st,sa,4,dart_bytes(buf,n),1100000);
    ST_CHECK(nc_up_n==1 && nc_up_id==idA, "node-core: resume re-ups same id (ups=%u id=%u)", nc_up_n, nc_up_id);

    /* 5. GONE (BYE) on the now-active peer: one PEER_DOWN, and it no longer resolves */
    nc_down_n=0;
    n=nc_dgram(buf,1,0x01,99,5001,NULL,1); dart_discovery_on_datagram(st,sa,4,dart_bytes(buf,n),1100001);
    ST_CHECK(nc_down_n==1, "node-core: GONE on active fires down (downs=%u)", nc_down_n);
    ST_CHECK(i_dart_node_core_resolve(nc,idA,&d)==0, "node-core: GONE peer freed (no resolve)");
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
    i_dart_plat_startup();
    memset(&cfg,0,sizeof cfg);
#ifdef _WIN32
    strcpy(cfg.name,"dart-shm-stmod");
#else
    strcpy(cfg.name,"/dart-shm-stmod");
#endif
    cfg.segment_id=0x1234; cfg.chunk_bytes=4096; cfg.n_chunks=4;
    pw=malloc(i_dart_shm_state_bytes()); pr=malloc(i_dart_shm_state_bytes());
    w=i_dart_shm_create(pw,&cfg); r=i_dart_shm_attach(pr,&cfg);
    ST_CHECK(w && r, "shm-mod: create + attach by name");
    if (w && r){
        i_dart_plat_host_uuid(a); i_dart_plat_host_uuid(b);
        ST_CHECK(i_dart_shm_host_match(a,b)==1, "shm-mod: host_uuid stable + matches");
        cp=i_dart_shm_chunk(w,0,&cap); memcpy(cp,msg,sizeof msg);
        i_dart_shm_stamp(w,0,(uint32_t)sizeof msg,&d);
        ST_CHECK(cap==4096 && d.generation==1, "shm-mod: loan + stamp (gen=%llu)", (unsigned long long)d.generation);
        i_dart_shm_desc_encode(&d,wire);
        ST_CHECK(i_dart_shm_desc_decode(&d2,wire,sizeof wire) &&
                 d2.chunk==d.chunk && d2.length==d.length && d2.generation==d.generation,
                 "shm-mod: descriptor wire round-trip");
        rp=i_dart_shm_read(r,&d2,&rlen);
        ST_CHECK(rp && rlen==sizeof msg && memcmp(rp,msg,sizeof msg)==0, "shm-mod: read sees writer bytes");
        ST_CHECK(i_dart_shm_verify(r,&d2)==1, "shm-mod: verify current gen ok");
        { void *cp2=i_dart_shm_chunk(w,0,NULL); i_DartShmDesc dn; uint32_t l;
          memcpy(cp2,"new",4); i_dart_shm_stamp(w,0,4,&dn);
          ST_CHECK(i_dart_shm_read(r,&d2,&l)==NULL, "shm-mod: recycled chunk -> old descriptor refused");
          ST_CHECK(i_dart_shm_verify(r,&d2)==0, "shm-mod: verify recycled gen fails (torn guard)"); }
        i_dart_shm_detach(r); i_dart_shm_detach(w);
    }
    free(pw); free(pr);
    i_dart_plat_cleanup();
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
        while (dart_transport_poll_send(shml_W,&to,buf,sizeof buf,&ol,shml_now)){
            if (shml_drop>0 && (buf[0]&0x20u)){ shml_drop--; continue; }   /* drop SHM-DATA */
            dart_transport_on_datagram(shml_R, 1u, dart_bytes(buf, ol), shml_now);
        }
        while (dart_transport_poll_send(shml_R,&to,buf,sizeof buf,&ol,shml_now))
            dart_transport_on_datagram(shml_W, 2u, dart_bytes(buf, ol), shml_now);
        shml_now += 30000;   /* 30 ms: past repair_delay (20ms), lets heartbeats fire */
    }
}
static void shml_send(void){
    static unsigned char chunk[2048]; unsigned char desc[DART_SHM_DESC_WIRE];
    memset(desc,0,sizeof desc); dart_transport_send_shm(shml_W, 0, dart_bytes(chunk, 1000), desc, shml_now);
}
static void shm_loss_checks(void){
    DartTopicDef cw, cr; DartConfig wc, rc; void *mw, *mr; size_t nw, nr;
    DartQos q; memset(&q,0,sizeof q); q.reliability=DART_RELIABLE; q.keep_last=8;
    q.heartbeat_us=50000; q.repair_delay_us=20000;
    memset(&cw,0,sizeof cw); cw.name="shmloss"; cw.qos=q; cw.role=DART_PUB_ONLY;
    memset(&cr,0,sizeof cr); cr.name="shmloss"; cr.qos=q; cr.role=DART_SUB_ONLY;
    memset(&wc,0,sizeof wc); wc.topics=&cw; wc.n_topics=1; wc.max_peers=2;
    memset(&rc,0,sizeof rc); rc.topics=&cr; rc.n_topics=1; rc.max_peers=2;
    rc.on_shm=shml_on_shm; rc.on_event=shml_on_event;
    nw=dart_transport_required_memory(&wc); mw=malloc(nw); shml_W=dart_transport_init(mw,nw,&wc);
    nr=dart_transport_required_memory(&rc); mr=malloc(nr); shml_R=dart_transport_init(mr,nr,&rc);
    shml_now=1000000;
    dart_transport_peer_add(shml_W,2u,DART_FRAG_SIZE); dart_transport_peer_add(shml_R,1u,DART_FRAG_SIZE);
    st_apply_verified(shml_R, 1u, shml_W);
    st_apply_verified(shml_W, 2u, shml_R);
    dart_transport_peer_set_shm(shml_W,2u,1);
    ST_CHECK(dart_transport_publisher_match_count(shml_W,0)>0, "shm-loss: writer matched reader");
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
    dart_transport_destroy(shml_W); dart_transport_destroy(shml_R); free(mw); free(mr);
}

/* (3) full nodes on loopback: SHM across size classes (byte-exact + shm_tx/rx), and
   the inline fallback for a non-SHM-capable subscriber. */
static int shmn_recv; static size_t shmn_len; static unsigned long shmn_sum;
static void shmn_on_message(const DartMsg *msg){
    const unsigned char *p=(const unsigned char*)msg->data.data; size_t i, len=msg->data.len; unsigned long s=0;
    for(i=0;i<len;i++) s+=p[i];
    shmn_recv++; shmn_len=len; shmn_sum=s;
}
static DartNode *shmn_open(int is_pub, int shm_capable, uint16_t domain, void **mem_out){
    static DartTopicDef ch[2]; static int slot;
    DartTopicDef *d=&ch[slot++ & 1]; DartDiscoveryAddr seed; DartNodeOpts opts;
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
        for (i=0;i<800 && dart_node_publisher_match_count(P,0)==0;i++){ dart_node_poll(P,2); dart_node_poll(S,2); }
        ST_CHECK(dart_node_publisher_match_count(P,0)>0, "shm-node: matched");
        /* sizes[0] fits one datagram (< frag) so it ships INLINE, not SHM; the two
           larger ones would fragment, so they take the SHM path. */
        sizes[0]=200; sizes[1]=300*1024; sizes[2]=4*1024*1024;
        for (i=0;i<3;i++){
            unsigned long want=0; size_t j; int before=shmn_recv, t;
            for (j=0;j<sizes[i];j++){ buf[j]=(unsigned char)((j*31u+(unsigned)i+1)&0xFF); want+=buf[j]; }
            dart_node_send(P,0,buf,sizes[i]);
            for (t=0;t<500 && shmn_recv==before;t++){ dart_node_poll(P,2); dart_node_poll(S,2); }
            ST_CHECK(shmn_recv==before+1 && shmn_len==sizes[i] && shmn_sum==want,
                     "shm-node: byte-exact %lu bytes", (unsigned long)sizes[i]);
            if (i==0){ tx=0; dart_node_shm_stats(P,&tx,NULL);
                ST_CHECK(tx==0, "shm-node: sub-fragment %lu B went inline, not SHM (tx=%u)",
                         (unsigned long)sizes[i], tx); }
        }
        dart_node_shm_stats(P,&tx,NULL); dart_node_shm_stats(S,NULL,&rx);
        ST_CHECK(tx==2 && rx==2, "shm-node: 2 fragmenting msgs over SHM, small inline (tx=%u rx=%u)", tx, rx);
        dart_node_close(P,1); dart_node_close(S,1);
    }
    free(mp); free(ms);
    /* inline fallback: a non-SHM-capable subscriber forces inline UDP */
    shmn_recv=0;
    P=shmn_open(1,1,78,&mp); S=shmn_open(0,0,78,&ms);
    if (P&&S){
        unsigned long want=0; size_t j; int before, t;
        for (i=0;i<800 && dart_node_publisher_match_count(P,0)==0;i++){ dart_node_poll(P,2); dart_node_poll(S,2); }
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
 * clamp, the dart_transport_send result codes, and the shared little-endian byte packing.
 * These need no sockets, so they run straight against the transport core. */
static void unit_checks(void){
    /* dart_clamp_frag: 0 -> default, otherwise clamp into [MIN, MAX] */
    ST_CHECK(dart_clamp_frag(0) == DART_FRAG_SIZE,
             "clamp: 0 -> default frag (%u)", (unsigned)dart_clamp_frag(0));
    ST_CHECK(dart_clamp_frag(65535) == DART_FRAG_SIZE_MAX,
             "clamp: above-max -> MAX (%u)", (unsigned)dart_clamp_frag(65535));
    ST_CHECK(dart_clamp_frag(1) >= DART_FRAG_SIZE_MIN,
             "clamp: tiny -> >= MIN (%u)", (unsigned)dart_clamp_frag(1));

    /* shared little-endian helpers: byte order + round-trip */
    {   uint8_t b[8];
        i_dart_le_w16(b, 0xBEEFu);
        ST_CHECK(b[0]==0xEF && b[1]==0xBE && i_dart_le_r16(b)==0xBEEFu,
                 "bytes: w16/r16 little-endian round-trip");
        i_dart_le_w32(b, 0x01020304u);
        ST_CHECK(b[0]==0x04 && b[3]==0x01 && i_dart_le_r32(b)==0x01020304u,
                 "bytes: w32/r32 little-endian round-trip");
        i_dart_le_w64(b, 0x0102030405060708ull);
        ST_CHECK(b[0]==0x08 && b[7]==0x01 && i_dart_le_r64(b)==0x0102030405060708ull,
                 "bytes: w64/r64 little-endian round-trip");
    }

    /* topic identity: deterministic and name-distinct */
    ST_CHECK(dart_topic_id("alpha") == dart_topic_id("alpha")
             && dart_topic_id("alpha") != dart_topic_id("beta"),
             "topic-id: deterministic and name-distinct");

    /* dart_transport_send result codes (transport core, no sockets). The size check precedes
       the role check, so an oversize send on the pub topic is TOO_BIG, while a
       valid-size send on the sub-only topic is ROLE. */
    {   static uint8_t tmem[1<<16];
        DartTopicDef uch[2]; DartConfig tc; DartTransportState *ts; uint8_t buf[128];
        memset(uch, 0, sizeof uch);
        uch[0].name = "u/pub"; uch[0].role = DART_PUBSUB;   uch[0].qos.max_message_bytes = 64;
        uch[1].name = "u/sub"; uch[1].role = DART_SUB_ONLY; uch[1].qos.max_message_bytes = 64;
        memset(&tc, 0, sizeof tc);
        tc.topics = uch; tc.n_topics = 2; tc.max_peers = 2;
        ts = dart_transport_init(tmem, sizeof tmem, &tc);
        ST_CHECK(ts != NULL, "result: transport init");
        if (ts){
            memset(buf, 0, sizeof buf);
            ST_CHECK(dart_transport_send(ts, 5, dart_bytes(buf, 16),  0) == DART_ERR_NO_TOPIC, "result: out-of-range topic -> NO_CHANNEL");
            ST_CHECK(dart_transport_send(ts, 1, dart_bytes(buf, 16),  0) == DART_ERR_ROLE,       "result: sub-only topic -> ROLE");
            ST_CHECK(dart_transport_send(ts, 0, dart_bytes(buf, 100), 0) == DART_ERR_TOO_BIG,    "result: oversize message -> TOO_BIG");
            ST_CHECK(dart_transport_send(ts, 0, dart_bytes(buf, 16),  0) == DART_OK,             "result: valid publish -> OK");
        }
    }
}

/* Exercise dart_node_open's staged-cleanup (goto fail) paths: force a failure at a
 * different stage each time so a distinct label runs, assert the open returns NULL,
 * then confirm a normal open still works -- proving cleanup left the platform balanced
 * (a missed i_dart_plat_cleanup unbalances the refcount; a missed close leaks the socket). */
/* open-failure event sink: capture the DART_ERROR kind fired during a failing open */
static int of_seen_error;
static void of_on_event(const DartEvent *ev){ if (ev->kind == DART_ERROR) of_seen_error = (int)ev->error; }

static void open_fail_checks(void){
    static uint8_t mem[1<<20];
    DartNode *n;
    of_seen_error = DART_E_NONE;

    /* create-fail: an over-long topic name is rejected by dart_node_create_topic; the
       node opened fine and stays usable (validation moved from init to topic create) */
    {   static char longname[DART_TOPIC_NAME_MAX + 8]; DartTopic *c;
        DartAllocator a = dart_allocator_static(mem, sizeof mem);
        memset(longname, 'x', sizeof longname - 1); longname[sizeof longname - 1] = 0;
        n = dart_node_open(&a, NULL, NULL, NULL, &(DartNodeOpts){ .domain=ST_DOMAIN });
        ST_CHECK(n != NULL, "open-fail: node opens for create-fail check");
        c = n ? dart_node_create_topic(n, longname, DART_PUBSUB, NULL, NULL) : NULL;
        ST_CHECK(c == NULL, "open-fail: over-long topic name -> create_channel NULL");
        if (n) dart_node_close(n, 0);
    }

    /* a non-multicast discovery group makes discovery's IGMP join fail, so
       dart_discovery_place returns NULL and the node unwinds through fail_sock.
       dart_last_error(NULL) then names the step (MCAST_JOIN) with no handle to query. */
    {   DartAllocator a = dart_allocator_static(mem, sizeof mem);
        DartEvent err; char line[160];
        n = dart_node_open(&a, NULL, NULL, NULL,
            &(DartNodeOpts){ .domain=ST_DOMAIN, .net={ .discovery_group="1.2.3.4" } });
        ST_CHECK(n == NULL, "open-fail: non-multicast discovery group -> NULL");
        err = dart_last_error(NULL);
        ST_CHECK(err.kind == DART_ERROR && err.error == DART_E_MCAST_JOIN,
                 "open-fail: last_error is DART_E_MCAST_JOIN (%s)", dart_event_str(&err, line, sizeof line));
        if (n) dart_node_close(n, 0);
    }

    /* fail_sock: occupy an ephemeral port, then aim the node's data socket at it; the
       unicast data bind takes no reuse, so it collides and unwinds through fail_sock.
       last_error names DART_E_BIND and carries the offending port + OS errno. */
    {   i_DartSock occupy;
        i_dart_plat_startup();
        occupy = i_dart_plat_udp_open();
        if (occupy != DART_SOCK_BAD && i_dart_plat_bind(occupy, 0, 0, 0)){
            uint16_t port = i_dart_plat_local_port(occupy);
            DartAllocator a = dart_allocator_static(mem, sizeof mem);
            DartEvent err; char line[160];
            n = dart_node_open(&a, NULL, NULL, NULL,
                &(DartNodeOpts){ .domain=ST_DOMAIN, .net={ .data_port=port } });
            ST_CHECK(n == NULL, "open-fail: data-port collision -> NULL (fail_sock)");
            err = dart_last_error(NULL);
            ST_CHECK(err.kind == DART_ERROR && err.error == DART_E_BIND && err.port == port,
                     "open-fail: last_error is DART_E_BIND port=%u (%s)", port, dart_event_str(&err, line, sizeof line));
            if (n) dart_node_close(n, 0);
        }
        if (occupy != DART_SOCK_BAD) i_dart_plat_close(occupy);
        i_dart_plat_cleanup();
    }

    /* an on_event handler also receives the open failure directly (no handle needed): the
       node fires it on the passed-in callback before returning NULL. */
    {   DartAllocator a = dart_allocator_static(mem, sizeof mem);
        n = dart_node_open(&a, NULL, NULL, of_on_event,
            &(DartNodeOpts){ .domain=ST_DOMAIN, .net={ .discovery_group="1.2.3.4" } });
        ST_CHECK(n == NULL, "open-fail: open still returns NULL with on_event set");
        ST_CHECK(of_seen_error == DART_E_MCAST_JOIN,
                 "open-fail: on_event received the failure (error=%d)", of_seen_error);
        if (n) dart_node_close(n, 0);
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
    if (ev->kind == DART_ERROR && ev->error == DART_E_NAME_COLLISION){ evu_user = ev->user; evu_collisions++; }
}
static void event_user_checks(void){
    static uint8_t mem_w[1<<20], mem_r[1<<20]; static int sentinel;
    const char *A="iuZA9tcJzAG", *B="5wVGxhTCmOC";   /* both -> one identity (see collide.c) */
    DartTopicDef cw, cr; DartNodeOpts wo, ro; DartNode *w, *r;
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


/* Dynamic growth: creating topics past the reserve relocates the whole node into a bigger
 * arena, carrying live reliable state across. Stream on topic 0, force several grows by
 * creating topics mid-stream, then keep streaming on the ORIGINAL handle and assert no
 * message was lost, duplicated or reordered -- i.e. the migration preserved reader/writer
 * position, the peer/discovery state and the user's handle. */
static int dg_recv[24]; static int dg_seq_ok; static int dg_next0;
static void dg_on_message(const DartMsg *msg){
    if (msg->topic_index < 24) dg_recv[msg->topic_index]++;
    if (msg->topic_index == 0 && msg->data.len >= 1){
        int s = ((const uint8_t*)msg->data.data)[0];
        if (s != dg_next0) dg_seq_ok = 0;            /* gap, dup or reorder */
        dg_next0 = s + 1;
    }
}
static void dynamic_grow_checks(void){
    static const char *names[12] = {"dg/0","dg/1","dg/2","dg/3","dg/4","dg/5",
                                    "dg/6","dg/7","dg/8","dg/9","dg/10","dg/11"};
    DartAllocator pa = dart_allocator_dynamic(i_dart_plat_realloc, 0), sa = dart_allocator_dynamic(i_dart_plat_realloc, 0);
    DartNodeOpts po, so; DartNode *P=NULL, *S=NULL; DartTopic *pc0=NULL, *pcN;
    DartTopicOpts co; DartDiscoveryAddr seed; uint8_t payload[8]; int i, t;
    memset(&co,0,sizeof co); co.qos.reliability=DART_RELIABLE; co.qos.keep_last=32;
    co.qos.catch_up=32; co.qos.heartbeat_us=50000;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=ST_DOMAIN+7; po.max_topics=2; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    so=po;
    for (i=0;i<24;i++) dg_recv[i]=0;
    dg_seq_ok=1; dg_next0=0;
    P = dart_node_open(&pa, "dg-pub", NULL, NULL, &po);
    S = dart_node_open(&sa, "dg-sub", dg_on_message, NULL, &so);
    ST_CHECK(P&&S, "dyn-grow: nodes open (max_topics=2)");
    if (!(P&&S)){ if(P)dart_node_close(P,0); if(S)dart_node_close(S,0); return; }
    pc0 = dart_node_create_topic(P, names[0], DART_PUB_ONLY, NULL, &co);
    dart_node_create_topic(S, names[0], DART_SUB_ONLY, NULL, &co);
    ST_CHECK(pc0 != NULL, "dyn-grow: topic 0 created");
    for (t=0;t<800 && dart_topic_match_count(pc0)==0;t++){ dart_node_poll(P,2); dart_node_poll(S,2); }
    ST_CHECK(dart_topic_match_count(pc0)>0, "dyn-grow: topic 0 matched");
    for (i=0;i<5;i++){ payload[0]=(uint8_t)i; dart_topic_send(pc0,dart_bytes(payload,1)); dart_node_poll(P,1); dart_node_poll(S,2); }
    /* create topics 1..11 on both -> several grows (max_topics 2 -> 4 -> 8 -> 16) */
    for (i=1;i<12;i++){ dart_node_create_topic(P, names[i], DART_PUB_ONLY, NULL, &co);
                        dart_node_create_topic(S, names[i], DART_SUB_ONLY, NULL, &co);
                        dart_node_poll(P,1); dart_node_poll(S,1); }
    ST_CHECK(dart_topic_match_count(pc0)>0, "dyn-grow: topic 0 still matched after grows");
    for (i=5;i<10;i++){ payload[0]=(uint8_t)i; dart_topic_send(pc0,dart_bytes(payload,1)); dart_node_poll(P,1); dart_node_poll(S,2); }
    for (t=0;t<400 && dg_recv[0]<10;t++){ dart_node_poll(P,1); dart_node_poll(S,2); }
    ST_CHECK(dg_recv[0]==10, "dyn-grow: all 10 on topic 0 delivered across grows (got %d)", dg_recv[0]);
    ST_CHECK(dg_seq_ok, "dyn-grow: topic 0 in-order, no loss/dup across grows");
    pcN = dart_node_topic(P, 11);                  /* a topic created AFTER a grow */
    for (i=0;i<3;i++){ payload[0]=0xAA; if(pcN) dart_topic_send(pcN,dart_bytes(payload,1)); dart_node_poll(P,1); dart_node_poll(S,2); }
    for (t=0;t<200 && dg_recv[11]<3;t++){ dart_node_poll(P,1); dart_node_poll(S,2); }
    ST_CHECK(dg_recv[11]==3, "dyn-grow: post-grow topic delivers (got %d)", dg_recv[11]);
    dart_node_close(P,1); dart_node_close(S,1);
}

/* (17) QoS RxO: a DART_RELIABLE subscriber must REFUSE a best-effort publisher (no
   silent downgrade); every other direction matches. Sans-IO transport core, no data
   pump: build the publisher's interest, apply it to the reader, read the match. */
static unsigned long qos_incompat_n;
static void qos_on_event(const DartTransportEvent *ev){ if (ev->kind==DART_TRANSPORT_QOS_INCOMPATIBLE) qos_incompat_n++; }
static void qos_pair(int wrel, int rrel, uint16_t *recv_out, unsigned long *evt_out){
    DartTopicDef cw, cr; DartConfig wc, rc; void *mw, *mr; size_t nw, nr;
    DartTransportState *W, *R; uint16_t pub=0, recv=0;
    memset(&cw,0,sizeof cw); cw.name="qostopic"; cw.role=DART_PUB_ONLY;
    cw.qos.reliability=wrel?DART_RELIABLE:DART_BEST_EFFORT; cw.qos.keep_last=4;
    memset(&cr,0,sizeof cr); cr.name="qostopic"; cr.role=DART_SUB_ONLY;
    cr.qos.reliability=rrel?DART_RELIABLE:DART_BEST_EFFORT; cr.qos.keep_last=4;
    memset(&wc,0,sizeof wc); wc.topics=&cw; wc.n_topics=1; wc.max_peers=2;
    memset(&rc,0,sizeof rc); rc.topics=&cr; rc.n_topics=1; rc.max_peers=2; rc.on_event=qos_on_event;
    nw=dart_transport_required_memory(&wc); mw=malloc(nw); W=dart_transport_init(mw,nw,&wc);
    nr=dart_transport_required_memory(&rc); mr=malloc(nr); R=dart_transport_init(mr,nr,&rc);
    dart_transport_peer_add(W,2u,DART_FRAG_SIZE); dart_transport_peer_add(R,1u,DART_FRAG_SIZE);
    qos_incompat_n=0;
    st_apply_verified(R, 1u, W);
    dart_transport_peer_match_counts(R,1u,&pub,&recv);
    if (recv_out) *recv_out=recv;
    if (evt_out)  *evt_out=qos_incompat_n;
    dart_transport_destroy(W); dart_transport_destroy(R); free(mw); free(mr);
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
    DartTopicDef cw, cr; DartConfig wc, rc; void *mw, *mr; size_t nw, nr;
    DartTransportState *W, *R; uint8_t payload[8]; int i, evict;
    memset(&cw,0,sizeof cw); cw.name="beff"; cw.role=DART_PUB_ONLY;
    cw.qos.reliability=DART_RELIABLE; cw.qos.keep_last=2; cw.qos.max_message_bytes=8;
    memset(&cr,0,sizeof cr); cr.name="beff"; cr.role=DART_SUB_ONLY;
    cr.qos.reliability=rrel?DART_RELIABLE:DART_BEST_EFFORT; cr.qos.keep_last=2; cr.qos.max_message_bytes=8;
    memset(&wc,0,sizeof wc); wc.topics=&cw; wc.n_topics=1; wc.max_peers=2;
    memset(&rc,0,sizeof rc); rc.topics=&cr; rc.n_topics=1; rc.max_peers=2;
    nw=dart_transport_required_memory(&wc); mw=malloc(nw); W=dart_transport_init(mw,nw,&wc);
    nr=dart_transport_required_memory(&rc); mr=malloc(nr); R=dart_transport_init(mr,nr,&rc);
    dart_transport_peer_add(W,2u,DART_FRAG_SIZE); dart_transport_peer_add(R,1u,DART_FRAG_SIZE);
    st_apply_verified(W, 2u, R);   /* W learns (and verifies) that R subscribes */
    memset(payload,0x5A,sizeof payload);
    for (i=0;i<5;i++) dart_transport_send(W,0,dart_bytes(payload,sizeof payload),1000u+(uint64_t)i);  /* 5 sends, keep_last=2: ring wraps */
    evict = dart_transport_send_would_evict(W,0);
    dart_transport_destroy(W); dart_transport_destroy(R); free(mw); free(mr);
    return evict;
}
static void beff_flow_checks(void){
    ST_CHECK(beff_would_evict(0)==0, "flow: best-effort reader never stalls a reliable writer");
    ST_CHECK(beff_would_evict(1)==1, "flow: reliable reader does apply backpressure");
}

/* dart_schema_print is the inverse of compile: print a schema back to DSL, recompile that
   text, and assert an exact wire round-trip plus that the (NULL,0) measure matches. */
static void schema_print_roundtrip(DartAllocator *ma, DartSchema *s, const char *label){
    char buf[1024]; uint32_t need; DartSchema *back;
    if (!s) return;
    need = dart_schema_print(s, NULL, 0);                 /* snprintf-style measure */
    dart_schema_print(s, buf, sizeof buf);
    ST_CHECK(need > 0 && need < sizeof buf && strlen(buf) == (size_t)need,
             "schema-print: %s measures (%u) and matches the written length", label, need);
    back = dart_schema_compile(dart_allocator_alloc, ma, buf, NULL);
    ST_CHECK(back && dart_schema_hash(back) == dart_schema_hash(s),
             "schema-print: %s DSL recompiles to the same wire (hash)", label);
}

/* (17c) the schema DSL: text compiles to the same wire bytes (hence hash) the builder
   emits, layout comes out right, and malformed text fails with a useful position. */
static void schema_dsl_checks(void){
    DartAllocator ma = dart_allocator_dynamic(i_dart_plat_realloc, 0);
    static const char POSE[] =
        "Pose\n"
        "{\n"
        "    stamp:    u64,\n"
        "    x:        f64,\n"
        "    y:        f64,\n"
        "    uuid:     u8[16],   -- fixed array\n"
        "    tagCount: u8,\n"
        "    velocity: { dx: f32, dy: f32 }\n"
        "}";
    DartSchema *txt, *built;
    txt = dart_schema_compile(dart_allocator_alloc, &ma, POSE, NULL);
    ST_CHECK(txt != NULL, "schema-dsl: compiles");
    {   DartSchemaBuilder b = dart_schema_begin(dart_allocator_alloc, &ma, "Pose");
        dart_schema_field(&b, "stamp", DART_U64);
        dart_schema_field(&b, "x", DART_F64);
        dart_schema_field(&b, "y", DART_F64);
        dart_schema_field_array(&b, "uuid", DART_U8, 16);
        dart_schema_field(&b, "tagCount", DART_U8);
        dart_schema_begin_struct(&b, "velocity");
        dart_schema_field(&b, "dx", DART_F32);
        dart_schema_field(&b, "dy", DART_F32);
        dart_schema_end_struct(&b);
        built = dart_schema_finish(&b);
    }
    ST_CHECK(built != NULL, "schema-dsl: builder twin builds");
    ST_CHECK(txt && built && dart_schema_hash(txt) == dart_schema_hash(built),
             "schema-dsl: text and builder produce the same wire (same hash)");
    schema_print_roundtrip(&ma, txt, "Pose (nested struct + array)");
    if (txt){
        DartSchemaFieldInfo fi;
        ST_CHECK(dart_schema_size(txt) == 8+8+8+16+1+8, "schema-dsl: size %u", dart_schema_size(txt));
        ST_CHECK(dart_schema_field_count(txt) == 8, "schema-dsl: 8 flat fields (6 top + 2 nested)");
        ST_CHECK(dart_schema_field_index(txt, "tagCount") == 4
              && dart_schema_field_index(txt, "velocity.dx") == 6
              && dart_schema_field_index(txt, "dx") == -1,      /* nested needs its path */
                 "schema-dsl: index by name incl. dotted paths");
        ST_CHECK(dart_schema_field_at(txt, 3, &fi) && fi.kind == DART_ARR
                 && fi.elem == DART_U8 && fi.count == 16 && fi.offset == 24 && fi.size == 16,
                 "schema-dsl: array field info (off=%u size=%u count=%u)", fi.offset, fi.size, fi.count);
        ST_CHECK(dart_schema_field_at(txt, 5, &fi) && fi.kind == DART_STRUCT && fi.size == 8
                 && fi.offset == 41 && fi.depth == 0,
                 "schema-dsl: struct field info (off=%u size=%u)", fi.offset, fi.size);
        ST_CHECK(dart_schema_field_at(txt, 7, &fi) && fi.kind == DART_F32
                 && fi.offset == 45 && fi.depth == 1,
                 "schema-dsl: nested member is flattened (off=%u depth=%u)", fi.offset, fi.depth);
    }
    if (txt){   /* setters: build a message BY NAME, read it back through the getters */
        uint8_t m[49], uuid[16]; int i, ok;
        memset(m, 0xAA, sizeof m);                  /* dirty: the set fields must fully determine it */
        for (i = 0; i < 16; i++) uuid[i] = (uint8_t)i;
        ok  = dart_set_uint (m, sizeof m, txt, "stamp", 42);
        ok &= dart_set_f64  (m, sizeof m, txt, "x", 1.5);
        ok &= dart_set_f64  (m, sizeof m, txt, "y", -2.5);
        ok &= dart_set_array(m, sizeof m, txt, "uuid", dart_bytes(uuid, 5));   /* short write */
        ok &= dart_set_uint (m, sizeof m, txt, "tagCount", 300);               /* narrows like a cast */
        ok &= dart_set_f32  (m, sizeof m, txt, "velocity.dy", 7.5f);           /* nested by path */
        ST_CHECK(ok, "schema-dsl: setters accept (incl. nested path)");
        ST_CHECK(dart_get_uint(dart_bytes(m,sizeof m), txt, "stamp") == 42
              && dart_get_f64 (dart_bytes(m,sizeof m), txt, "y") == -2.5
              && dart_get_uint(dart_bytes(m,sizeof m), txt, "tagCount") == (300u & 0xFF)
              && dart_get_f32 (dart_bytes(m,sizeof m), txt, "velocity.dy") == 7.5f,
                 "schema-dsl: getters read the setters back (incl. nested path)");
        {   DartBytes a = dart_get_array(dart_bytes(m,sizeof m), txt, "uuid");
            ST_CHECK(a.len == 16 && a.data[4] == 4 && a.data[5] == 0 && a.data[15] == 0,
                     "schema-dsl: short array write zero-fills the tail");
        }
        {   DartValue v;                            /* reflection access by flat index */
            ST_CHECK(dart_get_value(dart_bytes(m,sizeof m), txt, 7, &v)
                     && v.kind == DART_F32 && v.v.f == 7.5,
                     "schema-dsl: dart_get_value reads the nested member");
            v.v.u = 9;
            ST_CHECK(dart_set_value(m, sizeof m, txt, 4, &v)
                     && dart_get_uint(dart_bytes(m,sizeof m), txt, "tagCount") == 9,
                     "schema-dsl: dart_set_value writes by index");
        }
        ST_CHECK(!dart_set_uint (m, sizeof m, txt, "x", 1)                        /* f64: wrong family */
              && !dart_set_f64  (m, sizeof m, txt, "velocity", 0.0)               /* struct: no setter */
              && !dart_set_array(m, sizeof m, txt, "uuid", dart_bytes(uuid, 17))  /* overflow: refused */
              && !dart_set_uint (m, 8, txt, "x", 1)                               /* short buffer */
              && !dart_set_uint (m, sizeof m, txt, "nope", 1),                    /* unknown field */
                 "schema-dsl: bad sets refused");
    }
    {   /* strings: string<cap> and string<cap>[N] are fixed slots of [u16 len][cap bytes] */
        static const char TAGGED[] =
            "Tagged { id: u32, name: string<12>, labels: string<8>[3], meta: { note: string<4> } }";
        DartSchema *ts, *twin;
        ts = dart_schema_compile(dart_allocator_alloc, &ma, TAGGED, NULL);
        ST_CHECK(ts != NULL, "schema-dsl: strings compile");
        {   DartSchemaBuilder b = dart_schema_begin(dart_allocator_alloc, &ma, "Tagged");
            dart_schema_field(&b, "id", DART_U32);
            dart_schema_field_string(&b, "name", 12);
            dart_schema_field_string_array(&b, "labels", 8, 3);
            dart_schema_begin_struct(&b, "meta");
            dart_schema_field_string(&b, "note", 4);
            dart_schema_end_struct(&b);
            twin = dart_schema_finish(&b);
        }
        ST_CHECK(twin && ts && dart_schema_hash(ts) == dart_schema_hash(twin),
                 "schema-dsl: string text and builder produce the same wire (same hash)");
        schema_print_roundtrip(&ma, ts, "Tagged (capped strings + string array)");
        if (ts){
            DartSchemaFieldInfo fi;
            ST_CHECK(dart_schema_size(ts) == 4 + (2+12) + 3*(2+8) + (2+4),
                     "schema-dsl: string sizes (%u)", dart_schema_size(ts));
            ST_CHECK(dart_schema_field_at(ts, 1, &fi) && fi.kind == DART_STR
                     && fi.str_cap == 12 && fi.offset == 4 && fi.size == 14,
                     "schema-dsl: string field info (off=%u size=%u cap=%u)",
                     fi.offset, fi.size, fi.str_cap);
            ST_CHECK(dart_schema_field_at(ts, 2, &fi) && fi.kind == DART_ARR && fi.elem == DART_STR
                     && fi.count == 3 && fi.str_cap == 8 && fi.size == 30,
                     "schema-dsl: string array field info (size=%u cap=%u)", fi.size, fi.str_cap);
        }
        if (ts){
            uint8_t m[54]; DartString v; int ok;
            ok  = dart_schema_message_default(ts, m, sizeof m);
            ok &= dart_set_uint  (m, sizeof m, ts, "id", 7);
            ok &= dart_set_string(m, sizeof m, ts, "name", dart_string("robot-1", 7));
            ok &= dart_set_string_at(m, sizeof m, ts, "labels", 0, dart_string("fast", 4));
            ok &= dart_set_string_at(m, sizeof m, ts, "labels", 2, dart_string("red", 3));
            ok &= dart_set_string(m, sizeof m, ts, "meta.note", dart_string("ok", 2));  /* nested by path */
            ST_CHECK(ok, "schema-dsl: string setters accept (incl. nested + indexed)");
            v = dart_get_string(dart_bytes(m,sizeof m), ts, "name");
            ST_CHECK(v.len == 7 && memcmp(v.data, "robot-1", 7) == 0,
                     "schema-dsl: string round-trips");
            v = dart_get_string_at(dart_bytes(m,sizeof m), ts, "labels", 2);
            ST_CHECK(v.len == 3 && memcmp(v.data, "red", 3) == 0,
                     "schema-dsl: string array element round-trips");
            v = dart_get_string_at(dart_bytes(m,sizeof m), ts, "labels", 1);
            ST_CHECK(v.len == 0 && v.data != NULL, "schema-dsl: unset string element is empty");
            v = dart_get_string(dart_bytes(m,sizeof m), ts, "meta.note");
            ST_CHECK(v.len == 2 && memcmp(v.data, "ok", 2) == 0,
                     "schema-dsl: nested string round-trips");
            {   DartValue dv;   /* reflection sees the live bytes + the cap */
                ST_CHECK(dart_get_value(dart_bytes(m,sizeof m), ts, 1, &dv)
                         && dv.kind == DART_STR && dv.str_cap == 12
                         && dv.bytes.len == 7 && memcmp(dv.bytes.data, "robot-1", 7) == 0,
                         "schema-dsl: dart_get_value yields the live string");
            }
            ST_CHECK(!dart_set_string(m, sizeof m, ts, "name", dart_string("a-name-too-long", 15)) /* > cap: refused */
                  && !dart_set_string(m, sizeof m, ts, "id", dart_string("x", 1))                  /* not a string */
                  && !dart_set_string_at(m, sizeof m, ts, "labels", 3, dart_string("x", 1)),       /* index >= count */
                     "schema-dsl: bad string sets refused");
            {   /* a hostile length prefix reads back clamped to the cap */
                DartSchemaFieldInfo fi;
                dart_schema_field_at(ts, 1, &fi);
                m[fi.offset] = 0xFF; m[fi.offset + 1] = 0xFF;          /* len = 65535 */
                v = dart_get_string(dart_bytes(m,sizeof m), ts, "name");
                ST_CHECK(v.len == 12, "schema-dsl: hostile string length clamps to cap (%u)",
                         (unsigned)v.len);
            }
        }
        {   DartSchema *r_ok  = dart_schema_compile(dart_allocator_alloc, &ma, "Tagged { name: string<12> }", NULL);
            DartSchema *r_bad = dart_schema_compile(dart_allocator_alloc, &ma, "Tagged { name: string<10> }", NULL);
            ST_CHECK(r_ok && r_bad && ts && dart_schema_subset(r_ok, ts) && !dart_schema_subset(r_bad, ts),
                     "schema-dsl: string subset needs the same cap");
        }
    }
    {   /* variable fields: `string` / `elem[]` / `string<cap>[]` / `map` ride the tail
           as [u32 len] frames in schema order; fixed offsets are unaffected */
        static const char VDSL[] =
            "Var { id: u32, note: string, samples: f32[], labels: string<6>[], extras: map, tail: u8 }";
        DartSchema *vs, *twin;
        vs = dart_schema_compile(dart_allocator_alloc, &ma, VDSL, NULL);
        ST_CHECK(vs != NULL, "schema-var: compiles");
        {   DartSchemaBuilder b = dart_schema_begin(dart_allocator_alloc, &ma, "Var");
            dart_schema_field(&b, "id", DART_U32);
            dart_schema_field_var_string(&b, "note");
            dart_schema_field_var_array(&b, "samples", DART_F32);
            dart_schema_field_var_string_array(&b, "labels", 6);
            dart_schema_field_map(&b, "extras");
            dart_schema_field(&b, "tail", DART_U8);
            twin = dart_schema_finish(&b);
        }
        ST_CHECK(twin && vs && dart_schema_hash(vs) == dart_schema_hash(twin),
                 "schema-var: text and builder produce the same wire (same hash)");
        schema_print_roundtrip(&ma, vs, "Var (variable string/array/map)");
        if (vs){
            DartSchemaFieldInfo fi;
            ST_CHECK(dart_schema_size(vs) == 5 && dart_schema_msg_min(vs) == 5 + 4*4,
                     "schema-var: fixed size %u, msg_min %u",
                     dart_schema_size(vs), dart_schema_msg_min(vs));
            ST_CHECK(dart_schema_field_at(vs, 5, &fi) && fi.kind == DART_U8 && fi.offset == 4,
                     "schema-var: fixed fields pack around the variable ones (off=%u)", fi.offset);
            ST_CHECK(dart_schema_field_at(vs, 1, &fi) && fi.kind == DART_VSTR
                     && fi.offset == 0 && fi.size == 0,
                     "schema-var: variable field reports no static offset");
            ST_CHECK(dart_schema_field_at(vs, 3, &fi) && fi.kind == DART_VARR
                     && fi.elem == DART_STR && fi.str_cap == 6 && fi.count == 0,
                     "schema-var: string<6>[] field info (cap=%u)", fi.str_cap);
        }
        if (vs){
            uint8_t m[256], smp[8], slots[16], tmp[128], big[250];
            uint32_t blen, len; int ok; DartString v; DartBytes a, mb;
            {   int i; for (i = 0; i < 8; i++) smp[i] = (uint8_t)(i + 1); }
            memset(slots, 0, sizeof slots);              /* two empty string<6> slots */
            memset(big, 'x', sizeof big);

            ok = dart_schema_message_default(vs, m, sizeof m);
            ST_CHECK(ok && dart_schema_msg_len(vs, m, sizeof m) == dart_schema_msg_min(vs)
                     && dart_schema_validate(vs, dart_bytes(m, dart_schema_msg_min(vs))),
                     "schema-var: the default message is all empty frames and validates");
            ok  = dart_set_uint(m, sizeof m, vs, "id", 7);
            ok &= dart_set_uint(m, sizeof m, vs, "tail", 9);
            ok &= dart_set_array(m, sizeof m, vs, "samples", dart_bytes(smp, 8));  /* 2 live f32 */
            ok &= dart_set_string(m, sizeof m, vs, "note", dart_string("hello, tail", 11));
            ok &= dart_set_array(m, sizeof m, vs, "labels", dart_bytes(slots, 16));
            ok &= dart_set_string_at(m, sizeof m, vs, "labels", 1, dart_string("red", 3));
            ST_CHECK(ok, "schema-var: variable setters accept (out of schema order)");
            ST_CHECK(dart_get_uint(dart_bytes(m,sizeof m), vs, "id") == 7
                  && dart_get_uint(dart_bytes(m,sizeof m), vs, "tail") == 9,
                     "schema-var: fixed fields intact after tail resizes");
            v = dart_get_string(dart_bytes(m,sizeof m), vs, "note");
            ST_CHECK(v.len == 11 && memcmp(v.data, "hello, tail", 11) == 0,
                     "schema-var: variable string round-trips");
            a = dart_get_array(dart_bytes(m,sizeof m), vs, "samples");
            ST_CHECK(a.len == 8 && memcmp(a.data, smp, 8) == 0,
                     "schema-var: variable array survives an earlier frame's resize");
            v = dart_get_string_at(dart_bytes(m,sizeof m), vs, "labels", 1);
            ST_CHECK(v.len == 3 && memcmp(v.data, "red", 3) == 0,
                     "schema-var: variable string-array element round-trips");
            ST_CHECK(!dart_set_string_at(m, sizeof m, vs, "labels", 2, dart_string("x", 1)),
                     "schema-var: index past the live count refused");
            {   DartValue dv;                      /* reflection sees the live extent */
                ST_CHECK(dart_get_value(dart_bytes(m,sizeof m), vs, 2, &dv)
                         && dv.kind == DART_VARR && dv.elem == DART_F32
                         && dv.count == 2 && dv.bytes.len == 8,
                         "schema-var: dart_get_value yields the live element count");
            }

            {   DartMapWriter w = dart_map_begin(tmp, sizeof tmp);   /* the escape hatch */
                dart_map_put_uint(&w, "battery", 87);
                dart_map_put_string(&w, "state", dart_string("docked", 6));
                dart_map_open_array(&w, "temps");
                dart_map_put_f64(&w, NULL, 36.5);
                dart_map_put_int(&w, NULL, -3);
                dart_map_close(&w);
                dart_map_open_map(&w, "pose");
                dart_map_put_f64(&w, "x", 1.5);
                dart_map_close(&w);
                blen = dart_map_finish(&w);
            }
            ST_CHECK(blen > 0 && dart_set_map(m, sizeof m, vs, "extras", dart_bytes(tmp, blen)),
                     "schema-var: map writes and installs");
            mb = dart_get_map(dart_bytes(m,sizeof m), vs, "extras");
            ST_CHECK(mb.data && mb.len == blen && dart_map_count(mb) == 4,
                     "schema-var: map body round-trips (%u entries)", dart_map_count(mb));
            {   DartValue dv, e0, e1;
                ST_CHECK(dart_map_get(mb, "battery", &dv) && dv.kind == DART_U8 && dv.v.u == 87,
                         "schema-var: map uint stores in the smallest kind");
                ST_CHECK(dart_map_get(mb, "state", &dv) && dv.kind == DART_VSTR
                         && dv.bytes.len == 6 && memcmp(dv.bytes.data, "docked", 6) == 0,
                         "schema-var: map string");
                ST_CHECK(dart_map_get(mb, "temps", &dv) && dv.kind == DART_VARR && dv.count == 2
                      && dart_map_array_at(dv.bytes, 0, &e0) && e0.kind == DART_F64 && e0.v.f == 36.5
                      && dart_map_array_at(dv.bytes, 1, &e1) && e1.kind == DART_I8 && e1.v.i == -3,
                         "schema-var: map array elements (heterogeneous)");
                ST_CHECK(dart_map_get(mb, "pose", &dv) && dv.kind == DART_MAP
                      && dart_map_count(dv.bytes) == 1
                      && dart_map_get(dv.bytes, "x", &e0) && e0.v.f == 1.5,
                         "schema-var: nested map recurses");
                ST_CHECK(!dart_map_get(mb, "nope", &dv), "schema-var: missing key misses");
            }
            ST_CHECK(!dart_map_valid(dart_bytes(tmp, blen - 1))
                  && !dart_set_map(m, sizeof m, vs, "extras", dart_bytes(tmp, blen - 1)),
                     "schema-var: truncated map body refused");

            len = dart_schema_msg_len(vs, m, sizeof m);
            ST_CHECK(len == 5u + (4+11) + (4+8) + (4+16) + (4+blen),
                     "schema-var: live length (%u)", len);
            ST_CHECK(dart_schema_validate(vs, dart_bytes(m, len))
                  && !dart_schema_validate(vs, dart_bytes(m, len - 1))
                  && !dart_schema_validate(vs, dart_bytes(m, len + 1)),
                     "schema-var: frames must consume the message exactly");
            ST_CHECK(!dart_set_string(m, sizeof m, vs, "note", dart_string((char *)big, 250)),
                     "schema-var: a frame that would exceed the buffer is refused");
            v = dart_get_string(dart_bytes(m,sizeof m), vs, "note");
            ST_CHECK(v.len == 11, "schema-var: refused set leaves the message untouched");
            ok = dart_set_string(m, sizeof m, vs, "note", dart_string("hi", 2));
            a = dart_get_array(dart_bytes(m,sizeof m), vs, "samples");
            ST_CHECK(ok && a.len == 8 && memcmp(a.data, smp, 8) == 0
                     && dart_get_uint(dart_bytes(m,sizeof m), vs, "tail") == 9,
                     "schema-var: shrinking a frame memmoves the tail intact");

            {   /* subset + rebase: the reader skips frames it does not declare */
                DartSchema *sub = dart_schema_compile(dart_allocator_alloc, &ma,
                    "Var { tail: u8, samples: f32[], extras: map }", NULL);
                DartSchema *bad1 = dart_schema_compile(dart_allocator_alloc, &ma,
                    "Var { note: string<8> }", NULL);     /* capped vs variable */
                DartSchema *bad2 = dart_schema_compile(dart_allocator_alloc, &ma,
                    "Var { samples: f64[] }", NULL);      /* element kind differs */
                ST_CHECK(sub && dart_schema_subset(sub, vs), "schema-var: variable subset matches");
                ST_CHECK(bad1 && !dart_schema_subset(bad1, vs)
                      && bad2 && !dart_schema_subset(bad2, vs),
                         "schema-var: capped-vs-variable and element mismatches refused");
                {   DartSchema *rb = dart_schema_rebase(sub, vs, dart_allocator_alloc, &ma);
                    ST_CHECK(rb != NULL, "schema-var: rebase");
                    if (rb){
                        uint32_t rlen = dart_schema_msg_len(rb, m, sizeof m);
                        ST_CHECK(rlen == dart_schema_msg_len(vs, m, sizeof m)
                              && dart_schema_validate(rb, dart_bytes(m, rlen)),
                                 "schema-var: rebased schema walks the writer's frames");
                        ST_CHECK(dart_get_uint(dart_bytes(m, rlen), rb, "tail") == 9,
                                 "schema-var: rebased fixed offset");
                        a = dart_get_array(dart_bytes(m, rlen), rb, "samples");
                        ST_CHECK(a.len == 8 && memcmp(a.data, smp, 8) == 0,
                                 "schema-var: rebased variable ordinal finds the right frame");
                        mb = dart_get_map(dart_bytes(m, rlen), rb, "extras");
                        ST_CHECK(mb.data && dart_map_count(mb) == 4,
                                 "schema-var: rebased map field reads");
                    }
                }
            }
        }
    }
    {   /* schema-print: two struct levels deep, and a struct followed by more top-level fields */
        DartSchema *deep = dart_schema_compile(dart_allocator_alloc, &ma,
            "Deep { a: u32, g: { b: u16, inner: { c: i8, d: f64 }, e: u8 }, z: string<5>, arr: i32[3] }", NULL);
        schema_print_roundtrip(&ma, deep, "Deep (two nesting levels)");
    }
    {   /* errors: NULL + err points into the text at the offending spot */
        static const char *bad[] = {
            "Pose { x: f65 }",              /* unknown type */
            "Pose { x f64 }",               /* missing ':' */
            "Pose { x: f64 ",               /* missing '}' */
            "Pose { x: u8[0] }",            /* zero count */
            "Pose { x: u8[70000] }",        /* count > u16 */
            "Pose { x: f64 } y",            /* trailing garbage */
            "{ x: f64 }",                   /* missing root name */
            "Pose { x: string[] }",         /* ragged: an array of unbounded strings is a map's job */
            "Pose { x: string[4] }",        /* a fixed string array needs its <cap> */
            "Pose { x: string<0> }",        /* zero cap */
            "Pose { x: string<12 }",        /* missing '>' */
            "Pose { v: { y: u8[] } }",      /* variable field inside a nested struct */
            "Pose { m: map[3] }"            /* a map has no element form */
        };
        unsigned i, ok = 1;
        for (i = 0; i < sizeof bad / sizeof bad[0]; i++){
            const char *ep = NULL;
            DartSchema *s = dart_schema_compile(dart_allocator_alloc, &ma, bad[i], &ep);
            if (s || !ep || ep < bad[i] || ep > bad[i] + strlen(bad[i])) ok = 0;
        }
        ST_CHECK(ok, "schema-dsl: malformed text rejected with a position");
    }
    dart_allocator_reset(&ma);
}

/* (18) announce interest (v10): the blob carries one positional [u32 hash][u8 flags]
   entry per topic slot, no names or schemas. The subscriber's peer view yields each
   advertised direction with the right index (= the publisher's topic index) and the
   low 32 bits of the topic identity; an INACTIVE topic is not yielded but HOLDS ITS
   POSITION, so a later role flip advertises the same index. A typed match forming at
   all proves the schema now travels via the detail exchange. */
static void schema_advert_checks(void){
    DartAllocator pa = dart_allocator_dynamic(i_dart_plat_realloc, 0);
    DartAllocator sa = dart_allocator_dynamic(i_dart_plat_realloc, 0);
    DartAllocator ma = dart_allocator_dynamic(i_dart_plat_realloc, 0);   /* the caller-side schema */
    DartNodeOpts po, so; DartNode *P=NULL, *S=NULL; DartTopic *pc;
    DartTopicOpts co; DartDiscoveryAddr seed; DartSchema *sch=NULL;
    uint32_t pose_h = (uint32_t)dart_topic_id("sch/pose");
    uint32_t raw_h  = (uint32_t)dart_topic_id("sch/raw");
    uint32_t late_h = (uint32_t)dart_topic_id("sch/late");
    int t, pose_ok=0, raw_ok=0;
    const DartDiscoveryPeer *peers; uint16_t n_peers=0;

    {   DartSchemaBuilder b = dart_schema_begin(dart_allocator_alloc, &ma, "Pose");
        dart_schema_field(&b, "x", DART_F64);
        dart_schema_field(&b, "y", DART_F64);
        dart_schema_field_array(&b, "tags", DART_U8, 16);
        sch = dart_schema_finish(&b);
    }
    ST_CHECK(sch!=NULL, "announce: schema builds");
    if (!sch) return;

    memset(&co,0,sizeof co); co.qos.keep_last=4;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=ST_DOMAIN+8; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    so=po;
    P = dart_node_open(&pa, "sch-pub", NULL, NULL, &po);
    S = dart_node_open(&sa, "sch-sub", NULL, NULL, &so);
    ST_CHECK(P&&S, "announce: nodes open");
    if (!(P&&S)){ if(P)dart_node_close(P,0); if(S)dart_node_close(S,0); dart_allocator_reset(&ma); return; }
    pc = dart_node_create_topic(P, "sch/pose", DART_PUB_ONLY, sch, &co);   /* index 0, typed */
    dart_node_create_topic(P, "sch/raw",  DART_PUB_ONLY, NULL, &co);       /* index 1, raw */
    dart_node_create_topic(S, "sch/pose", DART_SUB_ONLY, sch, &co);
    ST_CHECK(pc!=NULL, "announce: topics created");
    dart_schema_free(sch, dart_allocator_alloc, &ma);   /* node owns its copy: caller's freed NOW */

    /* a typed-typed match through hash nomination + detail verification */
    for (t=0;t<800 && dart_topic_match_count(pc)==0;t++){ dart_node_poll(P,2); dart_node_poll(S,2); }
    ST_CHECK(dart_topic_match_count(pc)>0, "announce: typed match formed via detail exchange");

    peers = dart_node_peers(S, &n_peers);   /* S's view of P */
    ST_CHECK(peers && n_peers==1, "announce: subscriber sees one peer (%u)", n_peers);
    if (peers && n_peers==1){
        DartInterestIter it; DartTopicEntry tp;
        memset(&it,0,sizeof it);
        while (dart_node_peer_interest_next(&peers[0], &it, &tp)){
            if (!tp.is_pub) continue;
            if (tp.index==0 && tp.hash==pose_h && tp.role==DART_PUB_ONLY) pose_ok=1;
            if (tp.index==1 && tp.hash==raw_h  && tp.role==DART_PUB_ONLY) raw_ok=1;
        }
        ST_CHECK(pose_ok && raw_ok, "announce: positional indices carry the 32-bit hashes");

        /* the runtime-flip flow (the example's): an INACTIVE topic holds its position
           but is not yielded; flipping it to publish advertises the SAME index */
        {   DartTopic *lc = dart_node_create_topic(P, "sch/late", DART_INACTIVE, NULL, &co);
            ST_CHECK(lc!=NULL, "announce: inactive topic created");
            for (t=0;t<200;t++){ dart_node_poll(P,2); dart_node_poll(S,2); }
            {   DartInterestIter it2; DartTopicEntry tp2; int seen=0;
                peers = dart_node_peers(S, &n_peers);
                memset(&it2,0,sizeof it2);
                while (dart_node_peer_interest_next(&peers[0], &it2, &tp2))
                    if (tp2.hash==late_h) seen=1;
                ST_CHECK(!seen, "announce: inactive topic not advertised");
            }
            dart_topic_set_role(lc, DART_PUB_ONLY);
            for (t=0;t<400;t++){ dart_node_poll(P,2); dart_node_poll(S,2); }
            {   DartInterestIter it2; DartTopicEntry tp2; uint16_t late_alias=0xFFFF;
                peers = dart_node_peers(S, &n_peers);
                memset(&it2,0,sizeof it2);
                while (dart_node_peer_interest_next(&peers[0], &it2, &tp2))
                    if (tp2.is_pub && tp2.hash==late_h) late_alias=tp2.index;
                ST_CHECK(late_alias==2, "announce: role flip advertises the held position (index %u)",
                         late_alias);
            }
        }
    }
    dart_node_close(P,1); dart_node_close(S,1);
    dart_allocator_reset(&ma);
}

/* (19) reader-side subset binding: a subscriber declaring a SUBSET of the publisher's
   schema (by name, any order) matches; messages arrive with a rebased schema (the
   reader's indices on the writer's layout) and a validated length. An incompatible
   subscriber (same field, different kind) is refused on BOTH sides with
   DART_SCHEMA_MISMATCH; a wrong-size message from a matched writer is dropped. */
static int      sb_recv, sb_schema_ok;
static uint64_t sb_stamp; static double sb_y;
static unsigned long sb_mismatch_n;
static void sb_on_message(const DartMsg *msg){
    sb_recv++;
    if (!msg->schema) return;
    sb_schema_ok = (dart_schema_size(msg->schema) == 25 && dart_schema_field_count(msg->schema) == 2);
    sb_y     = dart_get_f64 (msg->data, msg->schema, "y");
    sb_stamp = dart_get_uint(msg->data, msg->schema, "stamp");
}
static void sb_on_event(const DartEvent *ev){
    if (ev->kind == DART_ERROR && ev->error == DART_E_SCHEMA_MISMATCH) sb_mismatch_n++;
}
static void schema_bind_checks(void){
    DartAllocator pa = dart_allocator_dynamic(i_dart_plat_realloc, 0);
    DartAllocator sa = dart_allocator_dynamic(i_dart_plat_realloc, 0);
    DartAllocator ma = dart_allocator_dynamic(i_dart_plat_realloc, 0);
    DartNodeOpts po, so; DartNode *P=NULL, *S=NULL;
    DartTopic *pc, *pc_bad; DartTopicOpts co; DartDiscoveryAddr seed;
    DartSchema *W, *R, *WB, *RB; int t;
    /* writer: full Pose; reader: a reordered subset of it */
    W  = dart_schema_compile(dart_allocator_alloc, &ma,
             "Pose { stamp: u64, x: f64, y: f64, tag: u8 }", NULL);      /* 25 B */
    R  = dart_schema_compile(dart_allocator_alloc, &ma,
             "Pose { y: f64, stamp: u64 }", NULL);
    WB = dart_schema_compile(dart_allocator_alloc, &ma, "Bad { v: u64 }", NULL);
    RB = dart_schema_compile(dart_allocator_alloc, &ma, "Bad { v: f64 }", NULL);  /* kind conflict */
    ST_CHECK(W && R && WB && RB, "schema-bind: schemas compile");
    ST_CHECK(W && R && dart_schema_subset(R, W) && !dart_schema_subset(W, R),
             "schema-bind: subset is one-way (reader within writer)");
    if (!(W && R && WB && RB)){ dart_allocator_reset(&ma); return; }

    memset(&co,0,sizeof co); co.qos.keep_last=4;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=ST_DOMAIN+9; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    so=po;
    sb_recv=0; sb_schema_ok=0; sb_stamp=0; sb_y=0.0; sb_mismatch_n=0;
    P = dart_node_open(&pa, "sb-pub", NULL,          sb_on_event, &po);
    S = dart_node_open(&sa, "sb-sub", sb_on_message, sb_on_event, &so);
    ST_CHECK(P&&S, "schema-bind: nodes open");
    if (!(P&&S)){ if(P)dart_node_close(P,0); if(S)dart_node_close(S,0); dart_allocator_reset(&ma); return; }
    pc     = dart_node_create_topic(P, "sb/pose", DART_PUB_ONLY, W,  &co);
    pc_bad = dart_node_create_topic(P, "sb/bad",  DART_PUB_ONLY, WB, &co);
    dart_node_create_topic(S, "sb/pose", DART_SUB_ONLY, R,  &co);
    dart_node_create_topic(S, "sb/bad",  DART_SUB_ONLY, RB, &co);
    ST_CHECK(pc && pc_bad, "schema-bind: topics created");

    for (t=0;t<800 && dart_topic_match_count(pc)==0;t++){ dart_node_poll(P,2); dart_node_poll(S,2); }
    ST_CHECK(dart_topic_match_count(pc)==1, "schema-bind: subset reader matched");
    ST_CHECK(dart_topic_match_count(pc_bad)==0, "schema-bind: kind-conflict reader refused");
    ST_CHECK(sb_mismatch_n>=1, "schema-bind: refusal surfaced (%lu DART_E_SCHEMA_MISMATCH)", sb_mismatch_n);

    {   /* publish one Pose packed in the WRITER's layout; the reader decodes through
           the rebased schema with its own indices */
        uint8_t buf[25]; uint64_t bits; double x=1.5, y=-2.25;
        i_dart_le_w64(buf, 0x1122334455667788ULL);              /* stamp @0 */
        memcpy(&bits,&x,8); i_dart_le_w64(buf+8,  bits);        /* x     @8 */
        memcpy(&bits,&y,8); i_dart_le_w64(buf+16, bits);        /* y     @16 */
        buf[24]=7;                                              /* tag   @24 */
        dart_topic_send(pc, dart_bytes(buf, sizeof buf));
        for (t=0;t<400 && sb_recv==0;t++){ dart_node_poll(P,1); dart_node_poll(S,2); }
        ST_CHECK(sb_recv==1, "schema-bind: subset message delivered");
        ST_CHECK(sb_schema_ok, "schema-bind: DartMsg.schema is the rebased view (writer size, reader fields)");
        ST_CHECK(sb_y==-2.25 && sb_stamp==0x1122334455667788ULL,
                 "schema-bind: reader indices read the writer's offsets (y=%.2f)", sb_y);
    }
    {   /* a message that does not fit the sender's schema is dropped + surfaced */
        unsigned long before = sb_mismatch_n;
        uint8_t junk[3] = {1,2,3};
        dart_topic_send(pc, dart_bytes(junk, sizeof junk));
        for (t=0;t<200 && sb_mismatch_n==before;t++){ dart_node_poll(P,1); dart_node_poll(S,2); }
        ST_CHECK(sb_recv==1 && sb_mismatch_n>before,
                 "schema-bind: wrong-size message dropped + surfaced (recv=%d)", sb_recv);
    }
    dart_node_close(P,1); dart_node_close(S,1);
    dart_allocator_reset(&ma);
}

/* (19b) pairwise detail codec ('uDTL', sans-IO): request build + header accessors, the
   stateless responder (advertised indices answered, INACTIVE/unknown skipped), schema
   wire inlined ONLY on hash mismatch, entry-boundary truncation (the paging seam), and
   wholesale rejection of malformed input. No sockets: the codec is called directly. */
static void detail_codec_checks(void){
    static uint8_t tmem[1<<18];
    DartAllocator ma = dart_allocator_dynamic(i_dart_plat_realloc, 0);
    DartConfig tc; DartTransportState *tr; DartTopicDef ch[3];
    DartMetaSchema schemas[3]; DartSchema *S;
    DartDetailWant wants[4];
    uint8_t req[256], resp[1024], out2[1024];
    size_t rl, need, len;

    S = dart_schema_compile(dart_allocator_alloc, &ma, "Pose { stamp: u64, x: f64 }", NULL);
    memset(ch,0,sizeof ch);
    ch[0].name="dt/typed"; ch[1].name="dt/raw";
    ch[2].name="dt/off"; ch[2].role=DART_INACTIVE;
    memset(&tc,0,sizeof tc); tc.topics=ch; tc.n_topics=3; tc.max_peers=2;
    tr = dart_transport_init(tmem, sizeof tmem, &tc);
    ST_CHECK(tr!=NULL && S!=NULL, "detail: transport + schema ready");
    if (!tr || !S){ dart_allocator_reset(&ma); return; }
    memset(schemas,0,sizeof schemas);
    schemas[0].hash = dart_schema_hash(S); schemas[0].wire = dart_schema_wire(S);

    /* request four indices: typed (requester untyped: hash 0), raw, INACTIVE, unknown */
    wants[0].index=0; wants[0].schema_hash=0;
    wants[1].index=1; wants[1].schema_hash=0;
    wants[2].index=2; wants[2].schema_hash=0;
    wants[3].index=9; wants[3].schema_hash=0;
    rl = dart_detail_req_build(77, 5, wants, 4, req, sizeof req);
    ST_CHECK(rl == 14u+4u*10u, "detail: req builds (%u bytes)", (unsigned)rl);
    ST_CHECK(dart_detail_kind(dart_bytes(req,rl))==DART_DETAIL_REQ
          && dart_detail_domain(dart_bytes(req,rl))==77
          && dart_detail_meta_version(dart_bytes(req,rl))==5,
             "detail: req header round-trips (kind/domain/version)");
    ST_CHECK(dart_detail_req_build(77,5,wants,4,req,20)==0, "detail: req refuses a short buffer");

    need = dart_transport_detail_resp_size(tr, schemas, dart_bytes(req,rl));
    len  = dart_transport_detail_respond(tr, schemas, 9, dart_bytes(req,rl), resp, sizeof resp);
    ST_CHECK(need==len && len>14u, "detail: resp_size == respond, byte for byte (%u)", (unsigned)len);
    ST_CHECK(dart_detail_kind(dart_bytes(resp,len))==DART_DETAIL_RESP
          && dart_detail_domain(dart_bytes(resp,len))==77
          && dart_detail_meta_version(dart_bytes(resp,len))==9,
             "detail: resp header carries the responder version");
    {   DartDetailIter it; DartDetail d; int n=0, ok_typed=0, ok_raw=0;
        DartBytes wire = dart_bytes(NULL, 0);
        memset(&it,0,sizeof it);
        while (dart_detail_next(dart_bytes(resp,len), &it, &d)){
            n++;
            if (d.index==0){
                ok_typed = d.name.len==8 && memcmp(d.name.data,"dt/typed",8)==0
                        && d.schema_hash==dart_schema_hash(S)
                        && d.schema_wire.len==dart_schema_wire(S).len;
                wire = d.schema_wire;
            }
            if (d.index==1)
                ok_raw = d.name.len==6 && memcmp(d.name.data,"dt/raw",6)==0
                      && d.schema_hash==0 && d.schema_wire.len==0;
        }
        ST_CHECK(n==2, "detail: advertised indices answered, INACTIVE + unknown skipped (n=%d)", n);
        ST_CHECK(ok_typed, "detail: typed entry carries name + hash + inlined wire");
        ST_CHECK(ok_raw, "detail: raw entry carries name, no schema");
        {   DartSchema *P2 = wire.len ? dart_schema_parse(wire.data, wire.len,
                                                          dart_allocator_alloc, &ma) : NULL;
            ST_CHECK(P2 && dart_schema_hash(P2)==dart_schema_hash(S),
                     "detail: inlined wire parses back to the same identity");
        }
    }

    /* identical hash: hash-only entry, zero wire bytes (identical wire is implied) */
    wants[0].schema_hash = dart_schema_hash(S);
    rl  = dart_detail_req_build(77, 5, wants, 2, req, sizeof req);
    len = dart_transport_detail_respond(tr, schemas, 9, dart_bytes(req,rl), resp, sizeof resp);
    {   DartDetailIter it; DartDetail d; int hash_only=0; memset(&it,0,sizeof it);
        while (dart_detail_next(dart_bytes(resp,len), &it, &d))
            if (d.index==0) hash_only = d.schema_wire.len==0 && d.schema_hash==dart_schema_hash(S);
        ST_CHECK(hash_only, "detail: identical hash rides hash-only (no wire)");
    }

    /* a cap one byte short of full truncates at an entry boundary: still parseable,
       holding exactly the leading entries that fit (the requester re-asks for the rest) */
    wants[0].schema_hash = 0;
    rl = dart_detail_req_build(77, 5, wants, 2, req, sizeof req);
    {   size_t full = dart_transport_detail_resp_size(tr, schemas, dart_bytes(req,rl));
        size_t cut  = dart_transport_detail_respond(tr, schemas, 9, dart_bytes(req,rl), out2, full-1);
        DartDetailIter it; DartDetail d; int n=0; uint16_t first=0xFFFF;
        memset(&it,0,sizeof it);
        while (dart_detail_next(dart_bytes(out2,cut), &it, &d)){ if (!n) first=d.index; n++; }
        ST_CHECK(cut>0 && cut<full && n==1 && first==0,
                 "detail: truncation stops at an entry boundary (paging: %d/%d entries)", n, 2);
    }

    /* malformed input is rejected wholesale, never partially trusted */
    ST_CHECK(dart_transport_detail_respond(tr, schemas, 9, dart_bytes(req, rl-1), out2, sizeof out2)==0,
             "detail: truncated req rejected");
    ST_CHECK(dart_transport_detail_respond(tr, schemas, 9, dart_bytes(resp, len), out2, sizeof out2)==0,
             "detail: a RESP fed to the responder is refused (kind gate)");
    {   uint8_t junk[32]; memset(junk, 0x5A, sizeof junk);
        ST_CHECK(dart_detail_kind(dart_bytes(junk, sizeof junk))==0
              && dart_detail_kind(dart_bytes(req, 4))==0,
                 "detail: non-detail bytes yield kind 0");
    }
    dart_allocator_reset(&ma);
}

/* (19c) live 'uDTL' routing: a DETAIL_REQ at a node's data socket is answered to the
   request's SOURCE address even though the requester is a bare socket, never a peer
   (the stateless-responder contract the explorer will rely on). The peer's v9 announce
   blob, read from a second node's peer view, is the oracle the response must match.
   Duplicate requests are idempotent; wrong-domain and garbage uDTL datagrams are
   ignored without wedging the node. */
static void detail_live_checks(void){
    DartAllocator pa = dart_allocator_dynamic(i_dart_plat_realloc, 0);
    DartAllocator sa = dart_allocator_dynamic(i_dart_plat_realloc, 0);
    DartAllocator ma = dart_allocator_dynamic(i_dart_plat_realloc, 0);
    DartNodeOpts po, so; DartNode *P=NULL, *S=NULL; DartTopicOpts co; DartDiscoveryAddr seed;
    DartSchema *W; DartTopic *pc;
    i_DartSock q = DART_SOCK_BAD;
    uint8_t pip[4]={0,0,0,0}; uint16_t pport=0; uint32_t pversion=0;
    /* the oracle, copied out of the peer view BEFORE any further poll invalidates it */
    uint16_t oalias[4]; char oname[4][64]; uint8_t onlen[4]; uint64_t ohash[4]; uint16_t nw=0;
    uint16_t dom = ST_DOMAIN+10;
    int t;

    W = dart_schema_compile(dart_allocator_alloc, &ma, "Pose { stamp: u64, x: f64 }", NULL);
    memset(&co,0,sizeof co); co.qos.keep_last=2;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=dom; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    so=po;
    P = dart_node_open(&pa, "dt-pub", NULL, NULL, &po);
    S = dart_node_open(&sa, "dt-sub", st_on_message, NULL, &so);
    ST_CHECK(P && S && W, "detail-live: nodes open");
    if (!(P && S && W)){
        if (P) dart_node_close(P,0); if (S) dart_node_close(S,0);
        dart_allocator_reset(&ma); return;
    }
    pc = dart_node_create_topic(P, "dt/pose",  DART_PUB_ONLY, W,    &co);
    dart_node_create_topic(P, "dt/plain", DART_PUB_ONLY, NULL, &co);
    dart_node_create_topic(S, "dt/pose",  DART_SUB_ONLY, W,    &co);
    for (t=0;t<800 && dart_topic_match_count(pc)==0;t++){ dart_node_poll(P,2); dart_node_poll(S,2); }
    ST_CHECK(pc && dart_topic_match_count(pc)==1, "detail-live: matched");

    /* the oracle: the publisher's locator, version, and advertised topics as its v9
       blob (held by the subscriber) states them */
    {   uint16_t cnt=0, k; const DartDiscoveryPeer *ps = dart_node_peers(S, &cnt);
        const DartDiscoveryPeer *pp = NULL;
        for (k=0;k<cnt;k++) if (ps[k].name.len==6 && memcmp(ps[k].name.data,"dt-pub",6)==0) pp=&ps[k];
        ST_CHECK(pp!=NULL, "detail-live: publisher in the peer view");
        if (pp){
            DartInterestIter it; DartTopicEntry tp;
            memcpy(pip, pp->addr.ip, 4); pport = pp->addr.port; pversion = pp->meta_version;
            memset(&it,0,sizeof it);
            /* v10 entries carry 32-bit hashes only: bind each announced entry to the
               topic we created on P by hash, and expect the RESP to fill in the rest */
            while (nw<4 && dart_node_peer_interest_next(pp, &it, &tp)){
                if (!tp.is_pub) continue;
                oalias[nw] = tp.index;
                if (tp.hash == (uint32_t)dart_topic_id("dt/pose")){
                    onlen[nw]=7; memcpy(oname[nw],"dt/pose",7);  ohash[nw]=dart_schema_hash(W);
                } else if (tp.hash == (uint32_t)dart_topic_id("dt/plain")){
                    onlen[nw]=8; memcpy(oname[nw],"dt/plain",8); ohash[nw]=0;
                } else continue;
                nw++;
            }
        }
    }
    ST_CHECK(nw==2 && pport!=0, "detail-live: oracle holds both pub topics (nw=%u)", nw);

    /* observer mode (opts.fetch_details): a topic-less node greedily fetches every
       peer topic's name + schema and serves the dart_node_peer_topic_* queries */
    {   DartAllocator oa = dart_allocator_dynamic(i_dart_plat_realloc, 0);
        DartNodeOpts oo = po; DartNode *O;
        oo.fetch_details = 1;
        O = dart_node_open(&oa, "dt-obs", st_on_message, NULL, &oo);
        ST_CHECK(O != NULL, "detail-obs: observer node opens");
        if (O){
            uint32_t pid = 0;
            DartString n0 = dart_string(NULL,0), n1 = dart_string(NULL,0);
            for (t=0;t<800;t++){
                uint16_t cnt=0, k; const DartDiscoveryPeer *ops;
                dart_node_poll(O,2); dart_node_poll(P,1); dart_node_poll(S,1);
                ops = dart_node_peers(O, &cnt);
                pid = 0;
                for (k=0;k<cnt;k++)
                    if (ops[k].name.len==6 && memcmp(ops[k].name.data,"dt-pub",6)==0) pid = ops[k].id;
                if (!pid) continue;
                n0 = dart_node_peer_topic_name(O, pid, 0);
                n1 = dart_node_peer_topic_name(O, pid, 1);
                if (n0.data && n1.data) break;
            }
            ST_CHECK(n0.data && n0.len==7 && memcmp(n0.data,"dt/pose",7)==0
                  && n1.data && n1.len==8 && memcmp(n1.data,"dt/plain",8)==0,
                     "detail-obs: greedy cache resolves both topic names");
            {   uint64_t h0=0, h1=1;
                const DartSchema *s0 = dart_node_peer_topic_schema(O, pid, 0, &h0);
                const DartSchema *s1 = dart_node_peer_topic_schema(O, pid, 1, &h1);
                ST_CHECK(s0 && h0==dart_schema_hash(W) && dart_schema_hash(s0)==h0,
                         "detail-obs: typed topic's schema cached parsed (hash matches)");
                ST_CHECK(!s1 && h1==0, "detail-obs: raw topic cached untyped");
            }
            /* the SUBSCRIBER is authoritative about ITS schema too: the observer fetches a
               SUB_ONLY topic's schema exactly like a publisher's (v10 detail exchange is
               role-agnostic). A subscriber-in-charge topic must show its schema. */
            {   uint32_t sid = 0; const DartSchema *ss = NULL; uint64_t hs = 0;
                for (t=0;t<800;t++){
                    uint16_t cnt=0, k; const DartDiscoveryPeer *ops;
                    dart_node_poll(O,2); dart_node_poll(P,1); dart_node_poll(S,1);
                    ops = dart_node_peers(O, &cnt);
                    sid = 0;
                    for (k=0;k<cnt;k++)
                        if (ops[k].name.len==6 && memcmp(ops[k].name.data,"dt-sub",6)==0) sid = ops[k].id;
                    if (!sid) continue;
                    ss = dart_node_peer_topic_schema(O, sid, 0, &hs);
                    if (ss) break;
                }
                ST_CHECK(ss && hs==dart_schema_hash(W) && dart_schema_hash(ss)==hs,
                         "detail-obs: subscriber-only topic's schema fetched (subscriber authoritative)");
            }
            /* observe THEN subscribe (the explorer's flow): the greedy fetch dissolved
               these indices against a topic-less node, so creating the topic must
               send them back to pending, re-verify, and form the match */
            {   DartTopic *osub = dart_node_create_topic(O, "dt/pose", DART_SUB_ONLY, W, &co);
                unsigned long a0 = st_any;
                ST_CHECK(osub != NULL, "detail-obs: late subscribe topic created");
                for (t=0;t<800 && dart_topic_match_count(pc)<2;t++){
                    dart_node_poll(O,2); dart_node_poll(P,1); dart_node_poll(S,1);
                }
                ST_CHECK(dart_topic_match_count(pc)==2,
                         "detail-obs: observe-then-subscribe re-verifies and matches (%u readers)",
                         dart_topic_match_count(pc));
                {   uint8_t pose[16]; memset(pose, 0x33, sizeof pose);   /* stamp + x */
                    dart_topic_send(pc, dart_bytes(pose, sizeof pose));
                    for (t=0;t<400 && st_any < a0+2;t++){
                        dart_node_poll(P,1); dart_node_poll(S,1); dart_node_poll(O,2);
                    }
                    ST_CHECK(st_any >= a0+2, "detail-obs: late subscriber receives (got %lu new)",
                             st_any - a0);
                }
            }
            dart_node_close(O, 1);
        }
    }

    q = i_dart_plat_udp_open();
    if (q != DART_SOCK_BAD){ if (!i_dart_plat_bind(q, 0, 0, 0)){ i_dart_plat_close(q); q = DART_SOCK_BAD; } }
    ST_CHECK(q != DART_SOCK_BAD, "detail-live: raw requester socket");
    if (q != DART_SOCK_BAD && nw==2 && pport){
        uint8_t req[128], r1[2048], r2[2048]; size_t rl; int n1=-1, n2=-1;
        DartDetailWant wants[4]; uint16_t k;
        i_dart_plat_set_nonblock(q);
        for (k=0;k<nw;k++){ wants[k].index=oalias[k]; wants[k].schema_hash=0; }
        rl = dart_detail_req_build(dom, pversion, wants, nw, req, sizeof req);

        /* a wrong-domain request is ignored (the node core's domain gate) */
        {   uint8_t bad[128]; size_t bl = dart_detail_req_build((uint16_t)(dom+1), pversion,
                                                                wants, nw, bad, sizeof bad);
            i_dart_plat_send(q, bad, bl, pip, pport);
            for (t=0;t<50;t++){ dart_node_poll(P,1); dart_node_poll(S,1);
                                if (i_dart_plat_recv(q, r1, sizeof r1, NULL, NULL) > 0){ n1=1; break; } }
            ST_CHECK(n1<0, "detail-live: wrong-domain request ignored");
        }

        /* the real request, re-sent like a real requester until answered */
        n1 = -1;
        for (t=0;t<400 && n1<=0;t++){
            if ((t & 63)==0) i_dart_plat_send(q, req, rl, pip, pport);
            dart_node_poll(P,2); dart_node_poll(S,1);
            n1 = i_dart_plat_recv(q, r1, sizeof r1, NULL, NULL);
        }
        ST_CHECK(n1>0, "detail-live: response reached the request's source socket");
        if (n1>0){
            DartBytes rb = dart_bytes(r1, (size_t)n1);
            DartDetailIter it; DartDetail d; int n=0, names_ok=1, hashes_ok=1, wire_ok=0;
            ST_CHECK(dart_detail_kind(rb)==DART_DETAIL_RESP && dart_detail_domain(rb)==dom
                  && dart_detail_meta_version(rb)==pversion,
                     "detail-live: header matches the announced version (%u)", pversion);
            memset(&it,0,sizeof it);
            while (dart_detail_next(rb, &it, &d)){
                for (k=0;k<nw;k++) if (oalias[k]==d.index) break;
                if (k==nw){ names_ok=0; continue; }
                if (d.name.len!=onlen[k] || memcmp(d.name.data, oname[k], onlen[k])!=0) names_ok=0;
                if (d.schema_hash != ohash[k]) hashes_ok=0;
                if (ohash[k] && d.schema_wire.len){
                    DartSchema *ps2 = dart_schema_parse(d.schema_wire.data, d.schema_wire.len,
                                                        dart_allocator_alloc, &ma);
                    if (ps2 && dart_schema_hash(ps2)==ohash[k]) wire_ok=1;
                }
                n++;
            }
            ST_CHECK(n==(int)nw && names_ok, "detail-live: names match the announce blob (n=%d)", n);
            ST_CHECK(hashes_ok, "detail-live: schema hashes match the announce blob");
            ST_CHECK(wire_ok, "detail-live: typed topic's wire inlined and parses to the advertised hash");
        }

        /* duplicates are idempotent: two more asks, two byte-identical answers */
        {   int got=0; n1=n2=-1;
            for (t=0;t<400 && got<2;t++){
                if ((t & 63)==0) i_dart_plat_send(q, req, rl, pip, pport);
                dart_node_poll(P,2); dart_node_poll(S,1);
                {   int r = i_dart_plat_recv(q, got==0?r1:r2, sizeof r1, NULL, NULL);
                    if (r>0){ if (got==0) n1=r; else n2=r; got++; } }
            }
            ST_CHECK(n1>0 && n1==n2 && memcmp(r1,r2,(size_t)n1)==0,
                     "detail-live: duplicate requests answer byte-identically");
        }

        /* a RESP and garbage 'uDTL' bytes at the node are ignored; it still answers */
        if (n1>0) i_dart_plat_send(q, r1, (size_t)n1, pip, pport);
        {   uint8_t junk[6]={'u','D','T','L',0x7F,0x00};
            i_dart_plat_send(q, junk, sizeof junk, pip, pport); }
        n2 = -1;
        for (t=0;t<400 && n2<=0;t++){
            if ((t & 63)==0) i_dart_plat_send(q, req, rl, pip, pport);
            dart_node_poll(P,2); dart_node_poll(S,1);
            n2 = i_dart_plat_recv(q, r2, sizeof r2, NULL, NULL);
        }
        ST_CHECK(n2>0, "detail-live: node still answers after RESP/garbage datagrams");
    }
    if (q != DART_SOCK_BAD) i_dart_plat_close(q);
    dart_node_close(P,1); dart_node_close(S,1);
    dart_allocator_reset(&ma);
}

/* ============ threaded: service thread, condvar flow control, waker ======== *
 * 20. RELIABLE   : both nodes on service threads, 3 sender threads x 1000 reliable
 *                  messages; exactly-once, per-thread ordered, no loss, no unsent
 *                  eviction, drain completes. No dart_node_poll anywhere.
 * 21. BURST      : best-effort keep_last 4; 64 back-to-back sends from one thread
 *                  must ALL reach the wire (the unsent guard closes the burst-
 *                  between-ticks overwrite race).
 * 21b HOSTILE    : (Windows) TX forced to would-block; the guard must surface
 *                  DART_EVICTED_UNSENT instead of silence, and every send is either
 *                  delivered or accounted an eviction.
 * 22. WAKER      : an idle started pair delivers a single send within ms, not at
 *                  the next announce-capped wakeup.
 * 23. REENTRANT  : a callback send (echo) works; create_channel/set_role from a
 *                  callback and poll from a foreign thread are refused loudly.
 * 24. STOP-UNDER-LOAD: stop with hammer threads mid-send and senders parked in the
 *                  backpressure wait; everything unblocks, close never hangs. */
#ifdef DART_THREADS

static void sw_sleep_ms(int ms);   /* defined with the sweep helpers below */

#define TH_SENDERS 3
#define TH_MSGS    1000u

static volatile unsigned long th_recv, th_order_bad, th_lost, th_evicted_evt;
static unsigned long th_next_seq[TH_SENDERS];   /* only the sub's service thread writes */

static void th_on_message(const DartMsg *m){
    if (m->data.len >= 8){
        const uint8_t *p = (const uint8_t*)m->data.data;
        uint32_t tid = (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
        uint32_t seq = (uint32_t)p[4] | ((uint32_t)p[5]<<8) | ((uint32_t)p[6]<<16) | ((uint32_t)p[7]<<24);
        if (tid < TH_SENDERS){
            if (seq != th_next_seq[tid]) th_order_bad++;
            th_next_seq[tid] = seq + 1;
        }
        th_recv++;
    }
}
static void th_on_event(const DartEvent *ev){
    if (ev->kind == DART_MSG_LOST) th_lost += (unsigned long)ev->lost_count;
    else if (ev->kind == DART_ERROR && ev->error == DART_E_EVICTED_UNSENT) th_evicted_evt++;
}

typedef struct { DartTopic *ch; uint32_t id; } th_sender_arg;
static void th_sender(void *arg){
    th_sender_arg *a = (th_sender_arg*)arg;
    uint8_t buf[8]; uint32_t i;
    for (i=0;i<TH_MSGS;i++){
        buf[0]=(uint8_t)a->id; buf[1]=(uint8_t)(a->id>>8); buf[2]=(uint8_t)(a->id>>16); buf[3]=(uint8_t)(a->id>>24);
        buf[4]=(uint8_t)i; buf[5]=(uint8_t)(i>>8); buf[6]=(uint8_t)(i>>16); buf[7]=(uint8_t)(i>>24);
        dart_topic_send(a->ch, dart_bytes(buf, sizeof buf));
    }
}

/* phase 23 state: node B echoes every request onto its reply topic from the
 * callback, and probes the forbidden reentrant calls exactly once */
static DartNode *th_echo_b;
static DartTopic *th_echo_rep;
static volatile unsigned long th_echo_replies;
static volatile int th_cb_send_rc = -100, th_cb_create_refused = -1, th_cb_setrole_rc = -100;
static void th_echo_b_on_msg(const DartMsg *m){
    th_cb_send_rc = dart_topic_send(th_echo_rep, m->data);
    if (th_cb_create_refused < 0){
        th_cb_create_refused = (dart_node_create_topic(th_echo_b, "th/na", DART_PUBSUB, NULL, NULL) == NULL);
        th_cb_setrole_rc = dart_topic_set_role(th_echo_rep, DART_PUB_ONLY);
    }
}
static void th_echo_a_on_msg(const DartMsg *m){ (void)m; th_echo_replies++; }

/* phase 24: hammer a reliable topic until told to stop */
static volatile int th_hammer_stop;
static void th_hammer(void *arg){
    uint8_t buf[8]; memset(buf, 0x77, sizeof buf);
    while (!th_hammer_stop) dart_topic_send((DartTopic*)arg, dart_bytes(buf, sizeof buf));
}

static void threaded_checks(void){
    static uint8_t dummy[1];
    uint8_t payload[16]; unsigned i;
    memset(payload, 0x33, sizeof payload);

    /* 20. RELIABLE: exactly-once ordered delivery under multithreaded send */
    { DartTopicDef cd[1]; DartNodeOpts o; DartNode *w, *r;
      memset(cd, 0, sizeof cd);
      cd[0].name = "th/rel"; cd[0].role = DART_PUB_ONLY;
      cd[0].qos.reliability = DART_RELIABLE; cd[0].qos.keep_last = 8;
      cd[0].qos.max_message_bytes = 32; cd[0].qos.heartbeat_us = 50000;
      cd[0].qos.backpressure_wait_us = 500000;
      memset(&o, 0, sizeof o);
      o.domain = ST_DOMAIN+4; o.disable_shm = 1; o.discovery.max_peers = 4;
      w = test_node_open(dummy, 0, "th-pub", NULL, th_on_event, o, cd, 1);
      cd[0].role = DART_SUB_ONLY;
      r = test_node_open(dummy, 0, "th-sub", th_on_message, th_on_event, o, cd, 1);
      ST_CHECK(w && r, "threaded: nodes open");
      if (w && r){
          th_recv = th_order_bad = th_lost = th_evicted_evt = 0;
          memset(th_next_seq, 0, sizeof th_next_seq);
          /* ST_CHECK evaluates its condition twice: keep side effects out of it */
          { int s1 = dart_node_start(w), s2 = dart_node_start(r), again, poll_rc;
            again = dart_node_start(w);
            poll_rc = dart_node_poll(w, 0);
            ST_CHECK(s1 == DART_OK && s2 == DART_OK, "threaded: service threads start");
            ST_CHECK(again == DART_ERR_STATE, "threaded: double start refused");
            ST_CHECK(poll_rc == DART_ERR_STATE, "threaded: foreign poll refused while started");
            ST_CHECK(dart_node_is_started(w) == 1, "threaded: is_started");
          }
          /* the service threads own discovery + matching: no polling from here on */
          { uint64_t end = i_dart_plat_now_us() + 5000000u;
            while (dart_node_publisher_match_count(w, 0) == 0 && i_dart_plat_now_us() < end)
                sw_sleep_ms(5); }
          ST_CHECK(dart_node_publisher_match_count(w, 0) == 1, "threaded: match formed by the services");
          {   i_DartThread th[TH_SENDERS]; th_sender_arg ta[TH_SENDERS]; uint32_t t;
              for (t=0;t<TH_SENDERS;t++){
                  ta[t].ch = dart_node_topic(w, 0); ta[t].id = t;
                  i_dart_plat_thread_start(&th[t], th_sender, &ta[t]);
              }
              for (t=0;t<TH_SENDERS;t++) i_dart_plat_thread_join(&th[t]);
          }
          { int drained = dart_node_drain(w, 0, 10000);
            ST_CHECK(drained == 1, "threaded: drain completes"); }
          { uint64_t end = i_dart_plat_now_us() + 3000000u;    /* delivered before acked; settle */
            while (th_recv < (unsigned long)TH_SENDERS*TH_MSGS && i_dart_plat_now_us() < end)
                sw_sleep_ms(5); }
          dart_node_stop(r); dart_node_stop(w);                /* join: counters now settled */
          ST_CHECK(th_recv == (unsigned long)TH_SENDERS*TH_MSGS,
                   "threaded: exactly-once delivery (%lu, want %lu)",
                   th_recv, (unsigned long)TH_SENDERS*TH_MSGS);
          ST_CHECK(th_order_bad == 0, "threaded: per-thread order kept (bad=%lu)", th_order_bad);
          ST_CHECK(th_lost == 0, "threaded: no loss (lost=%lu)", th_lost);
          ST_CHECK(th_evicted_evt == 0 && dart_node_evicted_unsent(w) == 0,
                   "threaded: no unsent eviction (evt=%lu cnt=%u)",
                   th_evicted_evt, dart_node_evicted_unsent(w));
          dart_node_close(r, 1); dart_node_close(w, 1);
      }
    }

    /* 21 + 21b + 22. BURST / HOSTILE / WAKER on one best-effort pair */
    { DartTopicDef cd[1]; DartNodeOpts o; DartNode *w, *r;
      memset(cd, 0, sizeof cd);
      cd[0].name = "th/burst"; cd[0].role = DART_PUB_ONLY;
      cd[0].qos.keep_last = 4;   /* best-effort */
      cd[0].qos.max_message_bytes = 32; cd[0].qos.heartbeat_us = 50000;
      memset(&o, 0, sizeof o);
      o.domain = ST_DOMAIN+5; o.disable_shm = 1; o.discovery.max_peers = 4;
      w = test_node_open(dummy, 0, "th-bpub", NULL, th_on_event, o, cd, 1);
      cd[0].role = DART_SUB_ONLY;
      r = test_node_open(dummy, 0, "th-bsub", th_on_message, th_on_event, o, cd, 1);
      ST_CHECK(w && r, "burst: nodes open");
      if (w && r){
          th_recv = th_lost = th_evicted_evt = 0;
          dart_node_start(w); dart_node_start(r);
          { uint64_t end = i_dart_plat_now_us() + 5000000u;
            while (dart_node_publisher_match_count(w, 0) == 0 && i_dart_plat_now_us() < end)
                sw_sleep_ms(5); }

          /* 21. 64 back-to-back sends, keep_last 4: the unsent guard must let every
             one reach the wire (each send waits at most one kicked TX pass) */
          for (i=0;i<64;i++) dart_node_send(w, 0, payload, sizeof payload);
          { uint64_t end = i_dart_plat_now_us() + 3000000u;
            while (th_recv < 64 && i_dart_plat_now_us() < end) sw_sleep_ms(5); }
          ST_CHECK(th_recv == 64, "burst: all 64 delivered past a depth-4 ring (%lu)", th_recv);
          ST_CHECK(dart_node_evicted_unsent(w) == 0, "burst: nothing evicted unsent (%u)",
                   dart_node_evicted_unsent(w));

          /* 22. WAKER: quiesce, then one send must land well inside the ~1s
             announce-capped sleep (only the waker explains that). Typical is
             sub-ms; the margin absorbs a loaded CI box losing the CPU. */
          sw_sleep_ms(300);
          { unsigned long r0 = th_recv; uint64_t t0 = i_dart_plat_now_us(), dt;
            dart_node_send(w, 0, payload, sizeof payload);
            while (th_recv == r0 && i_dart_plat_now_us() - t0 < 1000000u) { /* spin */ }
            dt = i_dart_plat_now_us() - t0;
            ST_CHECK(th_recv == r0+1 && dt < 400000u,
                     "waker: idle-node send delivered in %.1f ms", dt/1000.0);
          }

#ifdef _WIN32
          /* 21b. HOSTILE: transport TX forced to would-block. The first emit pass
             parks one datagram in tx_hold (a safe copy, delivered later); past that
             the burst overwrites truly unsent history, which must surface as
             DART_EVICTED_UNSENT, and every send is delivered or accounted evicted. */
          { unsigned long r0 = th_recv; uint32_t e0 = dart_node_evicted_unsent(w);
            uint32_t evicted;
            g_tx_block_data = 1;
            for (i=0;i<64;i++) dart_node_send(w, 0, payload, sizeof payload);
            evicted = dart_node_evicted_unsent(w) - e0;
            ST_CHECK(evicted >= 1, "hostile: blocked TX surfaces DART_E_EVICTED_UNSENT (%u)", evicted);
            g_tx_block_data = 0;
            /* the service retries the held datagram + drains the ring on its next
               pass; announce cadence bounds it, so give it time */
            { uint64_t end = i_dart_plat_now_us() + 3000000u;
              while (th_recv - r0 + evicted < 64 && i_dart_plat_now_us() < end) sw_sleep_ms(10); }
            ST_CHECK(th_recv - r0 + (unsigned long)evicted == 64,
                     "hostile: every send delivered or accounted (recv=%lu evicted=%u)",
                     th_recv - r0, evicted);
          }
#endif
          /* lock/unlock smoke: bracket a peer-view read while the services run */
          { uint16_t cnt = 0;
            dart_node_lock(w);
            (void)dart_node_peers(w, &cnt);
            dart_node_unlock(w);
            ST_CHECK(cnt >= 1, "lock: bracketed peer view reads (%u peers)", cnt);
          }
          dart_node_close(r, 1); dart_node_close(w, 1);
      }
    }

    /* 23. REENTRANT: echo from the callback; forbidden calls refuse loudly.
       The ring must be deeper than the request burst: a reentrant send can never
       wait (it runs inside the service pass), so if all 10 requests batch into one
       RX drain the 10 replies commit with no TX pass between them, and a shallower
       ring would (correctly, counted) evict the overflow. */
    { DartTopicDef ca[2], cb[2]; DartNodeOpts o; DartNode *a, *b;
      memset(ca, 0, sizeof ca);
      ca[0].name = "th/req"; ca[0].role = DART_PUB_ONLY;
      ca[0].qos.reliability = DART_RELIABLE; ca[0].qos.keep_last = 16;
      ca[0].qos.max_message_bytes = 32; ca[0].qos.heartbeat_us = 50000;
      ca[1] = ca[0]; ca[1].name = "th/rep"; ca[1].role = DART_SUB_ONLY;
      memcpy(cb, ca, sizeof ca);
      cb[0].role = DART_SUB_ONLY; cb[1].role = DART_PUB_ONLY;
      memset(&o, 0, sizeof o);
      o.domain = ST_DOMAIN+6; o.disable_shm = 1; o.discovery.max_peers = 4;
      a = test_node_open(dummy, 0, "th-echo-a", th_echo_a_on_msg, NULL, o, ca, 2);
      b = test_node_open(dummy, 0, "th-echo-b", th_echo_b_on_msg, NULL, o, cb, 2);
      ST_CHECK(a && b, "reentrant: nodes open");
      if (a && b){
          th_echo_b = b; th_echo_rep = dart_node_topic(b, 1);
          th_echo_replies = 0; th_cb_send_rc = -100; th_cb_create_refused = -1; th_cb_setrole_rc = -100;
          dart_node_start(a); dart_node_start(b);
          { uint64_t end = i_dart_plat_now_us() + 5000000u;
            while ((dart_node_publisher_match_count(a, 0) == 0 || dart_node_publisher_match_count(b, 1) == 0)
                   && i_dart_plat_now_us() < end)
                sw_sleep_ms(5); }
          for (i=0;i<10;i++) dart_node_send(a, 0, payload, sizeof payload);
          { uint64_t end = i_dart_plat_now_us() + 3000000u;
            while (th_echo_replies < 10 && i_dart_plat_now_us() < end) sw_sleep_ms(5); }
          dart_node_stop(b); dart_node_stop(a);
          ST_CHECK(th_echo_replies == 10, "reentrant: callback send echoes (%lu/10)", th_echo_replies);
          ST_CHECK(th_cb_send_rc == DART_OK, "reentrant: callback send returns DART_OK (%d)", th_cb_send_rc);
          ST_CHECK(th_cb_create_refused == 1, "reentrant: callback create_topic refused");
          ST_CHECK(th_cb_setrole_rc == DART_ERR_STATE, "reentrant: callback set_role refused (%d)", th_cb_setrole_rc);
          dart_node_close(b, 1); dart_node_close(a, 1);
      }
    }

    /* 24. STOP-UNDER-LOAD: hammer threads parked in the backpressure wait (a
       matched reader that never acks) while stop broadcasts them loose; repeat.
       The whole phase under a watchdog: a hang here is the deadlock detector. */
    { uint64_t phase_t0 = i_dart_plat_now_us(); int iter;
      for (iter=0; iter<3; iter++){
          DartTopicDef cd[1]; DartNodeOpts o; DartNode *w, *r;
          i_DartThread h1, h2;
          memset(cd, 0, sizeof cd);
          cd[0].name = "th/stop"; cd[0].role = DART_PUB_ONLY;
          cd[0].qos.reliability = DART_RELIABLE; cd[0].qos.keep_last = 4;
          cd[0].qos.max_message_bytes = 32; cd[0].qos.heartbeat_us = 50000;
          cd[0].qos.backpressure_wait_us = 300000;
          memset(&o, 0, sizeof o);
          o.domain = (uint16_t)(ST_DOMAIN+7+iter); o.disable_shm = 1; o.discovery.max_peers = 4;
          w = test_node_open(dummy, 0, "th-hpub", NULL, NULL, o, cd, 1);
          cd[0].role = DART_SUB_ONLY;
          r = test_node_open(dummy, 0, "th-hsub", NULL, NULL, o, cd, 1);
          if (!w || !r){ ST_CHECK(0, "stop-load: nodes open (iter %d)", iter); break; }
          { uint64_t end = i_dart_plat_now_us() + 5000000u;   /* match, then silence the reader */
            while (dart_node_publisher_match_count(w, 0) == 0 && i_dart_plat_now_us() < end)
                st_pump(w, r, 10); }
          dart_node_start(w);                    /* reader stays unpolled: never acks */
          th_hammer_stop = 0;
          i_dart_plat_thread_start(&h1, th_hammer, dart_node_topic(w, 0));
          i_dart_plat_thread_start(&h2, th_hammer, dart_node_topic(w, 0));
          sw_sleep_ms(50);                       /* hammers now parked in the wait */
          dart_node_stop(w);                     /* broadcasts the waiters loose */
          th_hammer_stop = 1;
          i_dart_plat_thread_join(&h1);
          i_dart_plat_thread_join(&h2);
          dart_node_close(r, 0);
          dart_node_close(w, 0);
      }
      ST_CHECK(i_dart_plat_now_us() - phase_t0 < 30000000u,
               "stop-load: 3 stop-under-fire cycles, no hang (%.1f s)",
               (i_dart_plat_now_us() - phase_t0)/1e6);
    }
}
#endif /* DART_THREADS */

/* ===================== consumer queues: take / dispatch ==================
 * One manually-pumped pair (pub-only A, sub-only B). Q1 proves lazy enablement
 * and that a timeout-take drives the poll loop itself; Q2 the best-effort
 * overwrite-oldest policy at a hard cap (DART_MSG_LOST, newest survive); Q3 the
 * reliable park: a full queue withholds acks (writer not drained), yet a slow
 * take loop receives every message in order with zero skips; Q4 dispatch runs
 * the node callback on the calling thread; Q5 (threads) the cv-wait take path
 * alongside service threads. */
static unsigned long qc_dispatched;
static void qc_on_message(const DartMsg *msg){ (void)msg; qc_dispatched++; }

static void queue_checks(void){
    static uint8_t mem_a[1], mem_b[1];
    uint8_t payload[512];
    DartTopicDef ca[4], cb[4];
    DartNodeOpts ao, bo;
    DartNode *a, *b;
    DartTopic *lazy, *be, *rel, *disp;
    DartMsg m;
    int i, r, got;
    memset(ca, 0, sizeof ca); memset(payload, 0, sizeof payload); memset(&m, 0, sizeof m);
    ca[0].name="q/lazy"; ca[0].role=DART_PUB_ONLY;
    ca[0].qos.reliability=DART_RELIABLE; ca[0].qos.keep_last=8; ca[0].qos.heartbeat_us=20000;
    ca[1].name="q/be";   ca[1].role=DART_PUB_ONLY;                  /* best-effort */
    ca[1].qos.keep_last=8;
    ca[2].name="q/rel";  ca[2].role=DART_PUB_ONLY;
    ca[2].qos.reliability=DART_RELIABLE; ca[2].qos.keep_last=16;    /* holds the whole burst */
    ca[2].qos.heartbeat_us=20000; ca[2].qos.repair_delay_us=5000;
    ca[3].name="q/disp"; ca[3].role=DART_PUB_ONLY;
    ca[3].qos.reliability=DART_RELIABLE; ca[3].qos.keep_last=8; ca[3].qos.heartbeat_us=20000;
    memcpy(cb, ca, sizeof ca);
    for (i=0;i<4;i++) cb[i].role=DART_SUB_ONLY;
    cb[1].qos.queue_bytes=4096;    /* hard consumer cap: forces overwrite-oldest */
    cb[2].qos.queue_bytes=2048;    /* holds ~4 of the 400 B messages: forces parking */
    cb[3].qos.queue_bytes=65536;   /* queued from creation: the dispatch tests */
    ao = (DartNodeOpts){ .domain=ST_DOMAIN+4, .discovery={ .max_peers=4 } };
    bo = ao;
    st_gap_calls[0]=st_gap_calls[1]=st_gap_calls[2]=st_gap_calls[3]=0;
    a = test_node_open(mem_a, sizeof mem_a, "qa-node", NULL, NULL, ao, ca, 4);
    b = test_node_open(mem_b, sizeof mem_b, "qb-node", qc_on_message, st_on_event, bo, cb, 4);
    ST_CHECK(a && b, "queue: nodes open");
    if (!a || !b){ if (a) dart_node_close(a,0); if (b) dart_node_close(b,0); return; }
    lazy = dart_node_topic(b, 0); be   = dart_node_topic(b, 1);
    rel  = dart_node_topic(b, 2); disp = dart_node_topic(b, 3);

    { uint64_t end = i_dart_plat_now_us()+5000000u;    /* all four matches first */
      while (i_dart_plat_now_us()<end &&
             (dart_node_publisher_match_count(a,0)<1 || dart_node_publisher_match_count(a,1)<1 ||
              dart_node_publisher_match_count(a,2)<1 || dart_node_publisher_match_count(a,3)<1))
          st_pump(a,b,10); }
    ST_CHECK(dart_node_publisher_match_count(a,0)==1 && dart_node_publisher_match_count(a,2)==1,
             "queue: matches formed");

    /* Q1: the first take enables the queue; a timeout-take pumps the loop itself */
    r = dart_topic_take(lazy, &m, 0);
    ST_CHECK(r == 0, "queue: first take is empty (rc=%d) and enables queued delivery", r);
    qc_dispatched = 0;
    for (i=0;i<5;i++){ put32(payload,(uint32_t)i); dart_node_send(a, 0, payload, 64); }
    got = 0;
    { uint64_t end = i_dart_plat_now_us()+5000000u;
      while (got<5 && i_dart_plat_now_us()<end){
          dart_node_poll(a, 0);
          if (dart_topic_take(lazy, &m, 50) == 1){    /* waits by pumping b's own loop */
              if ((int)get32(m.data.data) != got || m.data.len != 64) break;
              got++;
          }
      } }
    ST_CHECK(got==5, "queue: take drains 5 in order via its own pump (%d)", got);
    ST_CHECK(m.recv_us != 0, "queue: taken msg carries the poll-side arrival stamp");
    ST_CHECK(m.topic_name.len==6 && !memcmp(m.topic_name.data,"q/lazy",6),
             "queue: taken msg carries the topic name");
    ST_CHECK(m.publisher_name.len==7 && !memcmp(m.publisher_name.data,"qa-node",7),
             "queue: taken msg carries the sender name");
    ST_CHECK(qc_dispatched==0, "queue: no inline callback once queued (%lu)", qc_dispatched);

    /* Q2: best-effort at a hard cap overwrites oldest, fires DART_MSG_LOST, keeps newest */
    { uint32_t msgs=0, bytes=0, cap=0, dropped=0, last=0;
      for (i=0;i<60;i++){ put32(payload,(uint32_t)i); dart_node_send(a, 1, payload, 256); st_pump(a,b,1); }
      st_pump(a,b,50);
      dart_topic_queue_stats(be, &msgs, &bytes, &cap, &dropped);
      ST_CHECK(cap==4096 && dropped>0 && msgs>0,
               "queue: BE cap held, oldest dropped (cap=%u msgs=%u dropped=%u)", cap, msgs, dropped);
      got=0;
      while (dart_topic_take(be, &m, 0)==1){ last=get32(m.data.data); got++; }
      ST_CHECK(got>0 && last==59u, "queue: newest survive a BE overflow (got=%d last=%u)", got, last);
      ST_CHECK(st_gap_calls[1]>0, "queue: BE queue loss fired DART_MSG_LOST (%lu)", st_gap_calls[1]);
    }

    /* Q3: reliable + tiny queue parks (no acks) instead of losing; a slow take loop
       still receives everything in order via unpark + the normal repair machinery */
    got=0;
    for (i=0;i<12;i++){ put32(payload,(uint32_t)i); dart_node_send(a, 2, payload, 400); st_pump(a,b,2); }
    st_pump(a,b,30);
    ST_CHECK(dart_node_drain(a, 2, 0)==0, "queue: parked reader withholds acks (writer not drained)");
    { uint64_t end=i_dart_plat_now_us()+8000000u;
      while (got<12 && i_dart_plat_now_us()<end){
          if (dart_topic_take(rel, &m, 20)==1){
              if ((int)get32(m.data.data)!=got) break;
              got++;
          }
          dart_node_poll(a, 0);
      } }
    ST_CHECK(got==12, "queue: reliable park loses nothing, in order (%d/12)", got);
    { DartRepairStats rs; dart_node_repair_stats(b, 2, &rs);
      ST_CHECK(rs.msgs_skipped==0 && st_gap_calls[2]==0,
               "queue: no skip during park (skipped=%llu lost_events=%lu)",
               (unsigned long long)rs.msgs_skipped, st_gap_calls[2]); }
    st_pump(a,b,50);
    ST_CHECK(dart_node_drain(a, 2, 2000)==1, "queue: writer fully acked once drained");

    /* Q4: dispatch runs the node's on_message on the calling thread */
    qc_dispatched=0;
    for (i=0;i<3;i++){ put32(payload,(uint32_t)i); dart_node_send(a, 3, payload, 64); }
    { uint64_t end=i_dart_plat_now_us()+5000000u; uint32_t msgs=0;
      while (msgs<3 && i_dart_plat_now_us()<end){ st_pump(a,b,10); dart_topic_queue_stats(disp,&msgs,NULL,NULL,NULL); } }
    r = dart_topic_dispatch(disp, 0, 0);
    ST_CHECK(r==3 && qc_dispatched==3, "queue: dispatch runs the callback here (r=%d cb=%lu)", r, qc_dispatched);
    r = dart_node_dispatch(b, 0, 0);
    ST_CHECK(r==0, "queue: node dispatch finds nothing left (%d)", r);

#ifdef DART_THREADS
    /* Q5: take alongside service threads (the cv-wait path). ST_CHECK evaluates its
       condition twice, so the side-effecting starts run outside it. */
    r = (dart_node_start(a)==DART_OK && dart_node_start(b)==DART_OK);
    ST_CHECK(r, "queue: services start");
    for (i=0;i<40;i++){
        put32(payload,(uint32_t)i);
        dart_node_send(a, 3, payload, 64);
        if (dart_topic_take(disp, &m, 2000)!=1 || (int)get32(m.data.data)!=i) break;
    }
    ST_CHECK(i==40, "queue: threaded take (cv wait) delivers 40 in order (%d)", i);
    dart_node_stop(b); dart_node_stop(a);
#endif

    dart_node_close(b, 1);
    dart_node_close(a, 1);
}

/* ============ patterns layer: FUNCTIONS ============ *
 * A provider node and a caller node in one process. Exercises match, async call/reply,
 * empty-ack (no rsp schema, handler just returns), deferred completion, NO_HANDLER (a
 * provider with no handler), TIMEOUT (no provider), and (threaded) a synchronous call
 * answered by the provider's service thread. */
static volatile int   pf_reply_done;
static DartCallStatus pf_reply_status;
static uint32_t       pf_reply_val;
static void pf_on_reply(const DartResponse *r){
    pf_reply_status = r->status;
    pf_reply_val = r->data.len>=4 ? i_dart_le_r32(r->data.data) : 0;
    pf_reply_done = 1;
}
static int pf_calls;
static void pf_add_handler(DartRequest *req, void *user){
    uint8_t out[4];
    uint32_t v = req->data.len>=4 ? i_dart_le_r32(req->data.data) : 0;
    (void)user; pf_calls++;
    i_dart_le_w32(out, v+1);
    dart_request_reply(req, dart_bytes(out,4));
}
static void pf_empty_handler(DartRequest *req, void *user){ (void)req;(void)user; pf_calls++; /* no reply -> auto OK */ }
/* second caller's reply capture (two-caller directed-isolation test) */
static volatile int   pf_reply2_done;
static DartCallStatus pf_reply2_status;
static uint32_t       pf_reply2_val;
static void pf_on_reply2(const DartResponse *r){
    pf_reply2_status = r->status;
    pf_reply2_val = r->data.len>=4 ? i_dart_le_r32(r->data.data) : 0;
    pf_reply2_done = 1;
}
/* burst capture: counts completions, sums returned values, flags any non-OK */
static volatile int pf_burst_done; static uint32_t pf_burst_sum; static int pf_burst_bad;
static void pf_on_reply_burst(const DartResponse *r){
    if (r->status == DART_CALL_OK && r->data.len>=4) pf_burst_sum += i_dart_le_r32(r->data.data);
    else pf_burst_bad++;
    pf_burst_done++;
}
static volatile uint64_t pf_defer_token;
static void pf_defer_handler(DartRequest *req, void *user){ (void)user; pf_calls++; pf_defer_token = dart_request_defer(req); }
static int pf_sig_count; static uint32_t pf_sig_last;
static void pf_on_signal(const DartMsg *m, void *user){ (void)user; pf_sig_count++; pf_sig_last = m->data.len>=4 ? i_dart_le_r32(m->data.data) : 0; }
/* variable on_change / on_write capture */
typedef struct { int n; uint32_t val, seq, source; uint8_t forced; } PfVarEvt;
static void pf_on_var_update(const DartVariableUpdate *u, void *user){
    PfVarEvt *e = (PfVarEvt*)user;
    e->n++; e->val = u->value.len>=4 ? i_dart_le_r32(u->value.data) : 0;
    e->seq = u->write_seq; e->source = u->source; e->forced = u->forced;
}
/* cancel-at-close capture: a call pending at close must get exactly one CANCELLED outcome */
static volatile int pf_cancel_count; static DartCallStatus pf_cancel_status;
static void pf_on_cancel(const DartResponse *r){ pf_cancel_status = r->status; pf_cancel_count++; }
/* reentrant on_write: the first write of 100 immediately re-sets to 200, once */
static DartVariable *pf_reent_var; static int pf_reent_done;
static void pf_on_var_reenter(const DartVariableUpdate *u, void *user){
    (void)user;
    if (!pf_reent_done && u->value.len>=4 && i_dart_le_r32(u->value.data)==100){
        uint8_t nb[4];
        pf_reent_done = 1;
        i_dart_le_w32(nb, 200);
        dart_variable_set(pf_reent_var, dart_bytes(nb,4));
    }
}

static void pf_pump(DartNode *a, DartNode *b, int ms){
    uint64_t end = i_dart_plat_now_us() + (uint64_t)ms*1000u;
    while (i_dart_plat_now_us() < end){ dart_node_poll(a,2); dart_node_poll(b,2); }
}

static void patterns_checks(void){
    DartAllocator pa = dart_allocator_dynamic(i_dart_plat_realloc, 0);
    DartAllocator ca = dart_allocator_dynamic(i_dart_plat_realloc, 0);
    DartNodeOpts po, co; DartNode *P=NULL, *C=NULL; DartDiscoveryAddr seed;
    DartFunction *prov, *call_add, *pe, *ce, *pd, *cd, *pnh, *cnh, *ghost;
    uint16_t dom = ST_DOMAIN+20; int t;

    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=dom; po.discovery.max_peers=4;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    co=po;
    co.fetch_details = 1;   /* C doubles as the reflection observer: full entity names */
    P = dart_node_open(&pa, "fn-prov", NULL, NULL, &po);
    C = dart_node_open(&ca, "fn-call", NULL, NULL, &co);
    ST_CHECK(P && C, "patterns: nodes open");
    if (!(P && C)){ if(P)dart_node_close(P,0); if(C)dart_node_close(C,0); return; }

    prov = dart_node_create_function_definition(P, "add",  NULL, NULL, pf_add_handler,   NULL, NULL);
    call_add = dart_node_create_remote_function(C, "add",  NULL, NULL, NULL);
    pe   = dart_node_create_function_definition(P, "noop", NULL, NULL, pf_empty_handler, NULL, NULL);
    ce   = dart_node_create_remote_function(C, "noop", NULL, NULL, NULL);
    pd   = dart_node_create_function_definition(P, "defr", NULL, NULL, pf_defer_handler, NULL, NULL);
    cd   = dart_node_create_remote_function(C, "defr", NULL, NULL, NULL);
    pnh  = dart_node_create_function_definition(P, "nohd", NULL, NULL, NULL /* no handler */, NULL, NULL);
    cnh  = dart_node_create_remote_function(C, "nohd", NULL, NULL, NULL);
    ghost= dart_node_create_remote_function(C, "ghost",NULL, NULL, &(DartFunctionOpts){ .timeout_us = 150000u });
    ST_CHECK(prov&&call_add&&pe&&ce&&pd&&cd&&pnh&&cnh&&ghost, "patterns: functions created/opened");
    (void)pe;(void)ce;

    for (t=0;t<2000 && dart_function_match_count(call_add)==0;t++) pf_pump(P,C,2);
    ST_CHECK(dart_function_match_count(call_add)==1, "patterns: provider matched (%d)",
             dart_function_match_count(call_add));

    /* settle: the startup idiom solicits and blocks until every live peer answered and the
       topology went quiet. The peer must be able to ANSWER while we block, so P runs its
       service thread for the duration (in reality peers are independent processes). */
    { int sr;
      dart_node_start(P);
      sr = dart_node_settle(C, 3000);
      dart_node_stop(P);
      ST_CHECK(sr == 1, "patterns: dart_node_settle settles (%d)", sr); }

    /* async call: add(41) -> 42, status OK */
    { uint8_t req[4]; i_dart_le_w32(req,41); pf_reply_done=0;
      dart_function_call_async(call_add, dart_bytes(req,4), pf_on_reply, NULL);
      for (t=0;t<800 && !pf_reply_done;t++) pf_pump(P,C,2);
      ST_CHECK(pf_reply_done && pf_reply_status==DART_CALL_OK && pf_reply_val==42,
               "patterns: async add(41)=42 OK (done=%d st=%d val=%u)", pf_reply_done, pf_reply_status, pf_reply_val); }

    /* empty-ack: handler returns without replying -> auto OK, empty payload */
    { pf_reply_done=0; pf_calls=0;
      dart_function_call_async(ce, dart_bytes(NULL,0), pf_on_reply, NULL);
      for (t=0;t<800 && !pf_reply_done;t++) pf_pump(P,C,2);
      ST_CHECK(pf_reply_done && pf_reply_status==DART_CALL_OK && pf_reply_val==0,
               "patterns: empty-ack auto OK (done=%d st=%d)", pf_reply_done, pf_reply_status); }

    /* deferred: handler defers, we complete later with a value */
    { pf_reply_done=0; pf_defer_token=0;
      dart_function_call_async(cd, dart_bytes(NULL,0), pf_on_reply, NULL);
      for (t=0;t<800 && !pf_defer_token;t++) pf_pump(P,C,2);
      ST_CHECK(pf_defer_token!=0, "patterns: handler deferred (token=%llu)", (unsigned long long)pf_defer_token);
      { uint8_t out[4]; i_dart_le_w32(out,99);
        dart_function_complete(pd, pf_defer_token, DART_CALL_OK, dart_bytes(out,4)); }
      for (t=0;t<800 && !pf_reply_done;t++) pf_pump(P,C,2);
      ST_CHECK(pf_reply_done && pf_reply_status==DART_CALL_OK && pf_reply_val==99,
               "patterns: deferred completion delivers 99 (done=%d val=%u)", pf_reply_done, pf_reply_val); }

    /* no-handler: provider has NULL on_request -> NO_HANDLER */
    { pf_reply_done=0;
      dart_function_call_async(cnh, dart_bytes(NULL,0), pf_on_reply, NULL);
      for (t=0;t<800 && !pf_reply_done;t++) pf_pump(P,C,2);
      ST_CHECK(pf_reply_done && pf_reply_status==DART_CALL_NO_HANDLER,
               "patterns: no-handler -> NO_HANDLER (done=%d st=%d)", pf_reply_done, pf_reply_status); }

    /* timeout: no provider for "ghost" -> client-synthesized TIMEOUT */
    { pf_reply_done=0;
      dart_function_call_async(ghost, dart_bytes(NULL,0), pf_on_reply, NULL);
      for (t=0;t<400 && !pf_reply_done;t++) pf_pump(P,C,2);
      ST_CHECK(pf_reply_done && pf_reply_status==DART_CALL_TIMEOUT,
               "patterns: no-provider -> TIMEOUT (done=%d st=%d)", pf_reply_done, pf_reply_status); }

    /* call BEFORE the match forms: open a fresh function pair and call immediately, before
       any pump could run the announce/detail cycle. The request must QUEUE and flush when
       the provider matches, never silently drop into a timeout. */
    { DartFunction *pe2, *ce2; int cr; uint8_t req[4];
      pe2 = dart_node_create_function_definition(P, "early", NULL, NULL, pf_add_handler, NULL, NULL);
      ce2 = dart_node_create_remote_function(C, "early", NULL, NULL, NULL);
      ST_CHECK(pe2 && ce2, "patterns: early function pair created");
      i_dart_le_w32(req, 6); pf_reply_done = 0;
      cr = dart_function_call_async(ce2, dart_bytes(req,4), pf_on_reply, NULL);   /* unmatched right now */
      ST_CHECK(cr==DART_OK, "patterns: early call accepted (%d)", cr);
      for (t=0;t<2000 && !pf_reply_done;t++) pf_pump(P,C,2);
      ST_CHECK(pf_reply_done && pf_reply_status==DART_CALL_OK && pf_reply_val==7,
               "patterns: early call flushed on match -> 7 (done=%d st=%d val=%u)",
               pf_reply_done, pf_reply_status, pf_reply_val); }

    /* two callers answered in ONE provider tick: both requests drain in one RX pass, both
       replies commit back-to-back before any TX runs. Each caller must receive ITS OWN
       reply (the directed-send regression: the second commit used to jump the first
       caller's un-emitted reply, and a leaked reply is accepted cross-caller because call
       ids are per-caller counters). */
    { DartAllocator c2a = dart_allocator_dynamic(i_dart_plat_realloc, 0);
      DartNodeOpts c2o = co; DartNode *C2 = dart_node_open(&c2a, "fn-call2", NULL, NULL, &c2o);
      DartFunction *call2 = C2 ? dart_node_create_remote_function(C2, "add", NULL, NULL, NULL) : NULL;
      ST_CHECK(C2 && call2, "patterns: second caller open");
      if (C2 && call2){
          uint8_t r1[4], r2[4]; int i2;
          for (t=0;t<2000 && dart_function_match_count(call2)==0;t++){ pf_pump(P,C,1); dart_node_poll(C2,1); }
          ST_CHECK(dart_function_match_count(call2)==1, "patterns: second caller matched");
          /* park both requests at the provider before it polls once */
          i_dart_le_w32(r1,100); i_dart_le_w32(r2,200);
          pf_reply_done=0; pf_reply2_done=0;
          dart_function_call_async(call_add, dart_bytes(r1,4), pf_on_reply,  NULL);
          dart_function_call_async(call2,    dart_bytes(r2,4), pf_on_reply2, NULL);
          dart_node_poll(C,0); dart_node_poll(C2,0);      /* flush both requests out */
          for (i2=0;i2<800 && !(pf_reply_done && pf_reply2_done);i2++){
              dart_node_poll(P,2); dart_node_poll(C,2); dart_node_poll(C2,2);
          }
          ST_CHECK(pf_reply_done && pf_reply_status==DART_CALL_OK && pf_reply_val==101,
                   "patterns: caller1 got ITS reply (done=%d st=%d val=%u)",
                   pf_reply_done, pf_reply_status, pf_reply_val);
          ST_CHECK(pf_reply2_done && pf_reply2_status==DART_CALL_OK && pf_reply2_val==201,
                   "patterns: caller2 got ITS reply (done=%d st=%d val=%u)",
                   pf_reply2_done, pf_reply2_status, pf_reply2_val);
          dart_node_close(C2,1);
      } else if (C2) dart_node_close(C2,1);
      dart_allocator_reset(&c2a); }

#ifdef DART_THREADS
    /* burst past keep_last (10): with the provider on its own service thread, the caller's
       sends engage backpressure (the pattern layer must NOT pre-hold the node lock across
       the send) and every call completes: nothing evicted, nothing timed out. */
    { uint8_t req[4]; int i2; uint32_t expect_sum=0;
      dart_node_start(P);
      pf_burst_done=0; pf_burst_sum=0; pf_burst_bad=0;
      for (i2=0;i2<14;i2++){
          int cr;   /* hoisted: ST_CHECK evaluates its condition twice */
          i_dart_le_w32(req,(uint32_t)(1000+i2)); expect_sum += (uint32_t)(1000+i2+1);
          cr = dart_function_call_async(call_add, dart_bytes(req,4), pf_on_reply_burst, NULL);
          ST_CHECK(cr==DART_OK, "patterns: burst call %d accepted (%d)", i2, cr);
      }
      for (t=0;t<2000 && pf_burst_done<14;t++) dart_node_poll(C,2);
      ST_CHECK(pf_burst_done==14 && pf_burst_bad==0 && pf_burst_sum==expect_sum,
               "patterns: burst 14/keep_last 10 all OK (done=%d bad=%d sum=%u want=%u)",
               pf_burst_done, pf_burst_bad, pf_burst_sum, expect_sum);
      ST_CHECK(dart_node_evicted_unsent(C)==0, "patterns: burst evicted nothing (%u)",
               dart_node_evicted_unsent(C));
      dart_node_stop(P); }

    /* sync call that times out locally, then the deferred reply lands LATE: the pending
       entry must have been unlinked (no write into the dead stack frame) and the late
       reply dropped; a subsequent sync call still works. */
    { DartResponse rep; int rc;
      dart_node_start(P);
      pf_defer_token=0;
      rc = dart_function_call(cd, dart_bytes(NULL,0), &rep, 120);
      ST_CHECK(rc==0 && rep.status==DART_CALL_TIMEOUT,
               "patterns: sync local timeout (rc=%d st=%d)", rc, rep.status);
      for (t=0;t<400 && !pf_defer_token;t++) dart_node_poll(C,2);
      ST_CHECK(pf_defer_token!=0, "patterns: deferred token arrived");
      if (pf_defer_token){ uint8_t out[4]; i_dart_le_w32(out,7);
          dart_function_complete(pd, pf_defer_token, DART_CALL_OK, dart_bytes(out,4)); }
      for (t=0;t<200;t++) dart_node_poll(C,2);   /* late reply arrives: must be dropped safely */
      pf_reply_done=0;
      { uint8_t req[4]; i_dart_le_w32(req,60);
        rc = dart_function_call(call_add, dart_bytes(req,4), &rep, 1000); }
      ST_CHECK(rc==1 && rep.status==DART_CALL_OK && rep.data.len>=4 && i_dart_le_r32(rep.data.data)==61,
               "patterns: sync works after late-reply drop (rc=%d st=%d)", rc, rep.status);
      dart_node_stop(P); }
#endif

    /* ---- variables: catch-up, convergence, read-only refusal, force/absorb/unforce ---- */
    { DartVariable *ov, *av; DartBytes gv; uint8_t b[4];
      i_dart_le_w32(b,20);
      ov = dart_node_create_variable_definition(P, "temp", NULL, &(DartVariableOpts){ .initial=dart_bytes(b,4), .allow_force=1 });
      av = dart_node_create_remote_variable(C, "temp", NULL, NULL);
      ST_CHECK(ov && av, "var: created/opened");
      for (t=0;t<2000 && dart_variable_match_count(ov)==0;t++) pf_pump(P,C,2);
      ST_CHECK(dart_variable_match_count(ov)==1, "var: accessor matched (%d)", dart_variable_match_count(ov));
      for (t=0;t<600 && !dart_variable_get(av,&gv);t++) pf_pump(P,C,2);
      ST_CHECK(dart_variable_get(av,&gv) && gv.len==4 && i_dart_le_r32(gv.data)==20, "var: catch_up initial=20");
      i_dart_le_w32(b,25); dart_variable_set(ov, dart_bytes(b,4));
      for (t=0;t<600;t++){ pf_pump(P,C,2); if (dart_variable_get(av,&gv)&&gv.len==4&&i_dart_le_r32(gv.data)==25) break; }
      ST_CHECK(dart_variable_get(av,&gv)&&i_dart_le_r32(gv.data)==25, "var: owner set converges (25)");
      i_dart_le_w32(b,30);
      { int sr = dart_variable_set(av, dart_bytes(b,4)); ST_CHECK(sr==DART_OK, "var: accessor set ok (%d)", sr); }
      for (t=0;t<600;t++){ pf_pump(P,C,2); if (dart_variable_get(ov,&gv)&&gv.len==4&&i_dart_le_r32(gv.data)==30) break; }
      ST_CHECK(dart_variable_get(ov,&gv)&&i_dart_le_r32(gv.data)==30, "var: accessor set applied at owner (30)");
      i_dart_le_w32(b,99); dart_variable_force(ov, dart_bytes(b,4));
      for (t=0;t<600;t++){ pf_pump(P,C,2); if (dart_variable_forced(av)&&dart_variable_get(av,&gv)&&i_dart_le_r32(gv.data)==99) break; }
      ST_CHECK(dart_variable_forced(av)&&dart_variable_get(av,&gv)&&i_dart_le_r32(gv.data)==99,
               "var: force visible at accessor (99, forced)");
      i_dart_le_w32(b,50); dart_variable_set(av, dart_bytes(b,4));   /* absorbed into the shadow */
      pf_pump(P,C,80);
      ST_CHECK(dart_variable_get(av,&gv)&&i_dart_le_r32(gv.data)==99, "var: set absorbed while forced (still 99)");
      dart_variable_unforce(ov);
      for (t=0;t<600;t++){ pf_pump(P,C,2); if (!dart_variable_forced(av)&&dart_variable_get(av,&gv)&&i_dart_le_r32(gv.data)==50) break; }
      ST_CHECK(!dart_variable_forced(av)&&dart_variable_get(av,&gv)&&i_dart_le_r32(gv.data)==50,
               "var: unforce restores latest absorbed (50)"); }
    { DartVariable *ro_o, *ro_a; uint8_t b[4]; int sr; i_dart_le_w32(b,7);
      ro_o = dart_node_create_variable_definition(P, "rovar", NULL, &(DartVariableOpts){ .initial=dart_bytes(b,4), .access=DART_VAR_READONLY });
      ro_a = dart_node_create_remote_variable(C, "rovar", NULL, NULL);
      ST_CHECK(ro_o&&ro_a, "var: read-only created/opened");
      for (t=0;t<600;t++) pf_pump(P,C,2);
      i_dart_le_w32(b,8); sr = dart_variable_set(ro_a, dart_bytes(b,4));
      ST_CHECK(sr==DART_ERR_ROLE, "var: read-only set refused (%d)", sr); }
    { DartVariable *orphan = dart_node_create_remote_variable(C, "nobody-owns-this", NULL, NULL);
      uint8_t b[4]; int sr; i_dart_le_w32(b,1);
      ST_CHECK(orphan != NULL, "var: orphan accessor opens");
      sr = orphan ? dart_variable_set(orphan, dart_bytes(b,4)) : 0;
      ST_CHECK(sr==DART_ERR_NO_TOPIC, "var: set with no owner -> NO_TOPIC (%d)", sr); }
    { /* TYPED variable, remote force/unforce: the op-only (zero-payload) unforce must pass
         the schema gate (the empty-payload exemption on prefix channels), and remote sets
         while forced absorb into the shadow */
      DartAllocator ma = dart_allocator_dynamic(i_dart_plat_realloc, 0);
      DartSchema *ts = dart_schema_compile(dart_allocator_alloc, &ma, "T { v: u32 }", NULL);
      DartVariable *to, *ta; DartBytes gv; uint8_t b[4]; int fr, ur;
      ST_CHECK(ts != NULL, "var: typed schema compiles");
      i_dart_le_w32(b,10);
      to = dart_node_create_variable_definition(P, "ttemp", ts, &(DartVariableOpts){ .initial=dart_bytes(b,4), .allow_force=1 });
      ta = dart_node_create_remote_variable(C, "ttemp", ts, NULL);
      ST_CHECK(to && ta, "var: typed created/opened");
      for (t=0;t<2000 && dart_variable_match_count(to)==0;t++) pf_pump(P,C,2);
      for (t=0;t<600 && !dart_variable_get(ta,&gv);t++) pf_pump(P,C,2);
      i_dart_le_w32(b,777); fr = dart_variable_force(ta, dart_bytes(b,4));   /* remote force */
      ST_CHECK(fr==DART_OK, "var: typed remote force sent (%d)", fr);
      for (t=0;t<600;t++){ pf_pump(P,C,2); if (dart_variable_forced(ta)&&dart_variable_get(ta,&gv)&&i_dart_le_r32(gv.data)==777) break; }
      ST_CHECK(dart_variable_forced(ta)&&dart_variable_get(ta,&gv)&&i_dart_le_r32(gv.data)==777,
               "var: typed force pins (777, forced)");
      ur = dart_variable_unforce(ta);                                        /* remote, op-only payload */
      ST_CHECK(ur==DART_OK, "var: typed remote unforce sent (%d)", ur);
      for (t=0;t<600;t++){ pf_pump(P,C,2); if (!dart_variable_forced(ta)) break; }
      ST_CHECK(!dart_variable_forced(ta), "var: typed remote unforce applies (op-only payload)");
      dart_allocator_reset(&ma); }
    { /* on_change / on_write: change dedup, write-every-write, replay at registration,
         force transitions (a flag flip alone is a change), absorbed writes stay silent,
         and the source peer stamp */
      PfVarEvt oc, ow, ac, aw;   /* owner / accessor, change / write */
      DartVariable *eo, *ea; uint8_t b[4]; int rr;
      memset(&oc,0,sizeof oc); memset(&ow,0,sizeof ow); memset(&ac,0,sizeof ac); memset(&aw,0,sizeof aw);
      i_dart_le_w32(b,5);
      eo = dart_node_create_variable_definition(P, "evar", NULL, &(DartVariableOpts){ .initial=dart_bytes(b,4), .allow_force=1 });
      ea = dart_node_create_remote_variable(C, "evar", NULL, NULL);
      ST_CHECK(eo && ea, "var: event pair created");
      /* registering AFTER the initial value exists replays it once, immediately */
      rr = dart_variable_on_change(eo, pf_on_var_update, &oc);
      ST_CHECK(rr==DART_OK && oc.n==1 && oc.val==5,
               "var: on_change replays current at registration (n=%d val=%u)", oc.n, oc.val);
      dart_variable_on_write(eo, pf_on_var_update, &ow);
      ST_CHECK(ow.n==0, "var: on_write does not replay (writes are events, not state)");
      dart_variable_on_change(ea, pf_on_var_update, &ac);
      dart_variable_on_write(ea, pf_on_var_update, &aw);
      for (t=0;t<2000 && ac.n==0;t++) pf_pump(P,C,2);   /* catch-up: the first value fires both */
      ST_CHECK(ac.n==1 && aw.n==1 && ac.val==5,
               "var: accessor first value fires (change=%d write=%d val=%u)", ac.n, aw.n, ac.val);
      ST_CHECK(ac.source!=0, "var: accessor source = the owner peer (%u)", ac.source);
      i_dart_le_w32(b,6); dart_variable_set(eo, dart_bytes(b,4));   /* local set: inline, source 0 */
      ST_CHECK(oc.n==2 && ow.n==1 && oc.val==6 && oc.source==0,
               "var: local set fires inline on the caller (change=%d write=%d src=%u)", oc.n, ow.n, oc.source);
      dart_variable_set(eo, dart_bytes(b,4));               /* byte-identical re-set */
      ST_CHECK(oc.n==2 && ow.n==2 && ow.seq==oc.seq+1,
               "var: identical re-set fires write only, seq advances (change=%d write=%d)", oc.n, ow.n);
      for (t=0;t<600 && aw.n<3;t++) pf_pump(P,C,2);
      ST_CHECK(aw.n==3 && ac.n==2 && ac.val==6,
               "var: accessor saw 3 writes / 2 changes (w=%d c=%d val=%u)", aw.n, ac.n, ac.val);
      dart_variable_force(eo, dart_bytes(b,4));   /* same bytes: the forced flip alone is a change */
      ST_CHECK(oc.n==3 && oc.forced==1 && ow.n==3,
               "var: force with identical bytes still a change (n=%d forced=%u)", oc.n, oc.forced);
      for (t=0;t<600 && ac.n<3;t++) pf_pump(P,C,2);
      ST_CHECK(ac.n==3 && ac.forced==1, "var: accessor sees the forced flip (n=%d forced=%u)", ac.n, ac.forced);
      i_dart_le_w32(b,44); dart_variable_set(eo, dart_bytes(b,4));   /* absorbed into the shadow */
      pf_pump(P,C,40);
      ST_CHECK(oc.n==3 && ow.n==3, "var: absorbed write fires nothing (c=%d w=%d)", oc.n, ow.n);
      dart_variable_unforce(eo);
      ST_CHECK(oc.n==4 && !oc.forced && oc.val==44 && ow.n==4,
               "var: unforce fires with the restored value (n=%d val=%u)", oc.n, oc.val);
      for (t=0;t<600 && ac.n<4;t++) pf_pump(P,C,2);
      ST_CHECK(ac.n==4 && !ac.forced && ac.val==44,
               "var: accessor unforce change (n=%d val=%u forced=%u)", ac.n, ac.val, ac.forced);
      i_dart_le_w32(b,70);
      { int sr = dart_variable_set(ea, dart_bytes(b,4)); ST_CHECK(sr==DART_OK, "var: event remote set ok (%d)", sr); }
      for (t=0;t<600 && oc.n<5;t++) pf_pump(P,C,2);
      ST_CHECK(oc.n==5 && oc.val==70 && oc.source!=0,
               "var: remote set fires at the owner with the setter's peer (src=%u)", oc.source);
      dart_variable_on_change(eo, NULL, NULL); dart_variable_on_write(eo, NULL, NULL);
      i_dart_le_w32(b,71); dart_variable_set(eo, dart_bytes(b,4));
      ST_CHECK(oc.n==5 && ow.n==5, "var: cleared callbacks stay silent (c=%d w=%d)", oc.n, ow.n);
      for (t=0;t<600 && ac.n<5;t++) pf_pump(P,C,2);   /* drain the 70/71 echoes before the captures die */
      dart_variable_on_change(ea, NULL, NULL); dart_variable_on_write(ea, NULL, NULL); }

    /* ---- signals: derived roles (handler = subscription), emit/receive, payload-less ---- */
    { DartSignal *ps, *cs; uint8_t b[4]; int er;
      ps = dart_node_create_signal(P, "evt", NULL, NULL,         NULL, NULL);  /* no handler: emit-only */
      cs = dart_node_create_signal(C, "evt", NULL, pf_on_signal, NULL, NULL);  /* handler = subscription */
      ST_CHECK(ps && cs, "sig: created");
      for (t=0;t<2000 && dart_signal_listener_count(ps)==0;t++) pf_pump(P,C,2);
      ST_CHECK(dart_signal_listener_count(ps)==1, "sig: listener matched (%d)", dart_signal_listener_count(ps));
      ST_CHECK(dart_signal_listener_count(cs)==0, "sig: no listener subscribes to cs's emits (%d)",
               dart_signal_listener_count(cs));
      pf_sig_count=0; i_dart_le_w32(b,7); dart_signal_emit(ps, dart_bytes(b,4));
      for (t=0;t<400 && pf_sig_count==0;t++) pf_pump(P,C,2);
      ST_CHECK(pf_sig_count==1 && pf_sig_last==7, "sig: emit received (n=%d val=%u)", pf_sig_count, pf_sig_last);
      pf_sig_count=0; pf_sig_last=123; dart_signal_emit(ps, dart_bytes(NULL,0));
      for (t=0;t<400 && pf_sig_count==0;t++) pf_pump(P,C,2);
      ST_CHECK(pf_sig_count==1 && pf_sig_last==0, "sig: payload-less received (n=%d)", pf_sig_count);
      er = dart_signal_emit(cs, dart_bytes(b,4));   /* every handle may emit, even a listening one */
      ST_CHECK(er==DART_OK, "sig: emit from a listening handle allowed (%d)", er); }

    /* never latched + every handle both ways: P emits on "evt2" BEFORE C joins (must not be
       replayed to the late joiner), then both sides emit and both receive */
    { DartSignal *p2, *c2; uint8_t b[4];
      p2 = dart_node_create_signal(P, "evt2", NULL, pf_on_signal, NULL, NULL);
      ST_CHECK(p2 != NULL, "sig: evt2 created");
      i_dart_le_w32(b,55); dart_signal_emit(p2, dart_bytes(b,4));   /* nobody listening yet */
      pf_pump(P,C,50);
      pf_sig_count=0;
      c2 = dart_node_create_signal(C, "evt2", NULL, pf_on_signal, NULL, NULL);
      ST_CHECK(c2 != NULL, "sig: late joiner created");
      for (t=0;t<2000 && (dart_signal_listener_count(p2)==0 || dart_signal_listener_count(c2)==0);t++)
          pf_pump(P,C,2);
      ST_CHECK(dart_signal_listener_count(p2)==1 && dart_signal_listener_count(c2)==1,
               "sig: both sides matched (%d/%d)",
               dart_signal_listener_count(p2), dart_signal_listener_count(c2));
      pf_pump(P,C,100);
      ST_CHECK(pf_sig_count==0, "sig: late joiner replayed nothing (%d)", pf_sig_count);
      i_dart_le_w32(b,40); dart_signal_emit(p2, dart_bytes(b,4));
      i_dart_le_w32(b,41); dart_signal_emit(c2, dart_bytes(b,4));
      for (t=0;t<800 && pf_sig_count<2;t++) pf_pump(P,C,2);
      ST_CHECK(pf_sig_count==2, "sig: both directions received (%d)", pf_sig_count); }

#ifdef DART_THREADS
    /* sync call: the provider answers from its own service thread while the caller's
       sync loop drives its node */
    { DartResponse rep; int rc; uint8_t req[4]; i_dart_le_w32(req,7);
      dart_node_start(P);
      rc = dart_function_call(call_add, dart_bytes(req,4), &rep, 1000);
      ST_CHECK(rc==1 && rep.status==DART_CALL_OK && rep.data.len>=4 && i_dart_le_r32(rep.data.data)==8,
               "patterns: sync add(7)=8 (rc=%d st=%d)", rc, rep.status);
      dart_node_stop(P); }
#endif

    /* reflection: the entity walk folds P's channels into entities. P hosts 5 functions
       (add/noop/defr/nohd/early), 3 variables (temp rw, rovar ro, ttemp), 2 signals
       (evt, evt2): the peer walk from C and P's local walk must both yield exactly those,
       with no '@' internals and no incomplete pairs. */
    for (t=0;t<200;t++) pf_pump(P,C,2);   /* let any straggling detail fetches settle */
    { const DartDiscoveryPeer *ps; uint16_t pc = 0; uint32_t pid = 0;
      ps = dart_node_peers(C, &pc);
      if (ps && pc) pid = ps[0].id;
      { DartEntityIter eit; DartEntityInfo ei;
        int fns=0,vars=0,sigs=0,tops=0,ats=0,inc=0,temp_rw=0,rovar_ro=0,temp_forceable=0,rovar_forceable=0; size_t k;
        memset(&eit,0,sizeof eit);
        while (dart_node_peer_entity_next(C, pid, &eit, &ei)){
            switch (ei.kind){
            case DART_ENTITY_FUNCTION: fns++; break;
            case DART_ENTITY_VARIABLE:
                vars++;
                if (ei.name.len==4 && !memcmp(ei.name.data,"temp",4)) { temp_rw  = ei.writable; temp_forceable = ei.forceable; }
                if (ei.name.len==5 && !memcmp(ei.name.data,"rovar",5)){ rovar_ro = !ei.writable; rovar_forceable = ei.forceable; }
                break;
            case DART_ENTITY_SIGNAL: sigs++; break;
            default: tops++; break;
            }
            inc += ei.incomplete;
            for (k=0;k<ei.name.len;k++) if (ei.name.data[k]=='@') ats++;
        }
        ST_CHECK(fns==5 && vars==4 && sigs==2 && tops==0,
                 "reflect: peer entities fold (fn=%d var=%d sig=%d top=%d)", fns, vars, sigs, tops);
        ST_CHECK(ats==0 && inc==0, "reflect: no internals leak (@bytes=%d incomplete=%d)", ats, inc);
        ST_CHECK(temp_rw==1 && rovar_ro==1, "reflect: writability (temp rw=%d, rovar ro=%d)", temp_rw, rovar_ro);
        ST_CHECK(temp_forceable==1 && rovar_forceable==0,
                 "reflect: forceability (temp allow_force=%d, rovar=%d)", temp_forceable, rovar_forceable); }
      { DartEntityIter eit; DartEntityInfo ei; int fns=0,vars=0,sigs=0,tops=0,temp_forceable=0;
        memset(&eit,0,sizeof eit);
        while (dart_node_entity_next(P, &eit, &ei)){
            switch (ei.kind){
            case DART_ENTITY_FUNCTION: fns++; break;
            case DART_ENTITY_VARIABLE:
                vars++;
                if (ei.name.len==4 && !memcmp(ei.name.data,"temp",4)) temp_forceable = ei.forceable;
                break;
            case DART_ENTITY_SIGNAL: sigs++; break;
            default: tops++; break;
            } }
        ST_CHECK(fns==5 && vars==4 && sigs==2 && tops==0,
                 "reflect: local entities (fn=%d var=%d sig=%d top=%d)", fns, vars, sigs, tops);
        ST_CHECK(temp_forceable==1, "reflect: local forceability (temp allow_force=%d)", temp_forceable); } }

    { /* a reentrant set inside on_write publishes under the held lock, so it commits to
         transport history BEFORE the outer set's deferred send: the owner must SKIP the
         stale outer publish (else catch_up replays 100 as newest) and the remote's
         write_seq guard drops any stale same-owner value that still slips through */
      DartVariable *ro, *ra; uint8_t b[4]; DartBytes cur;
      ro = dart_node_create_variable_definition(P, "rvar", NULL, NULL);
      ra = dart_node_create_remote_variable(C, "rvar", NULL, NULL);
      ST_CHECK(ro && ra, "var: reentrancy pair created");
      pf_reent_var = ro; pf_reent_done = 0;
      dart_variable_on_write(ro, pf_on_var_reenter, NULL);
      i_dart_le_w32(b,100);
      dart_variable_set(ro, dart_bytes(b,4));
      ST_CHECK(pf_reent_done, "var: reentrant set ran inside on_write");
      { DartBytes ov; int have = dart_variable_get(ro, &ov);
        ST_CHECK(have && ov.len>=4 && i_dart_le_r32(ov.data)==200,
                 "var: owner store holds the newest (200)"); }
      for (t=0;t<2000;t++){ pf_pump(P,C,2);
          if (dart_variable_get(ra, &cur) && cur.len>=4 && i_dart_le_r32(cur.data)==200) break; }
      { int have = dart_variable_get(ra, &cur);
        ST_CHECK(have && cur.len>=4 && i_dart_le_r32(cur.data)==200,
                 "var: remote reached the newest value"); }
      pf_pump(P,C,60);   /* nothing stale may follow: the owner skipped the outer publish */
      { int have = dart_variable_get(ra, &cur);
        ST_CHECK(have && cur.len>=4 && i_dart_le_r32(cur.data)==200,
                 "var: stale outer publish skipped, remote stays newest (%u)",
                 (have && cur.len>=4) ? i_dart_le_r32(cur.data) : 0u); }
      dart_variable_on_write(ro, NULL, NULL); }

    /* a call still pending when the node closes gets one synthesized CANCELLED outcome */
    { DartFunction *never = dart_node_create_remote_function(C, "never-served", NULL, NULL,
                              &(DartFunctionOpts){ .timeout_us = 60000000u });
      ST_CHECK(never != NULL, "patterns: cancel-at-close remote created");
      pf_cancel_count = 0; pf_cancel_status = DART_CALL_OK;
      if (never) dart_function_call_async(never, dart_bytes(NULL,0), pf_on_cancel, NULL); }

    dart_node_close(P,0); dart_node_close(C,0);
    ST_CHECK(pf_cancel_count==1 && pf_cancel_status==DART_CALL_CANCELLED,
             "patterns: pending call cancelled at close (n=%d status=%d)",
             pf_cancel_count, (int)pf_cancel_status);
    dart_allocator_reset(&pa); dart_allocator_reset(&ca);
}

/* ============ duplicate-authority diagnostic (19e2) ============
 * The pattern contract expects ONE provider per function and ONE owner per variable. Two
 * authorities never form a lane (their roles are pub/pub or sub/sub), so the conflict is
 * detected off the announce interest and surfaced as DART_E_DUPLICATE_AUTHORITY, once per
 * (entity, peer), on BOTH rivals: the side creating second detects at create (the rival's
 * interest is already cached), the first detects when the rival's announce arrives.
 * Accessors/callers are not authorities and must never fire it. */
static void dup_on_event(const DartEvent *ev){
    if (ev->kind == DART_ERROR && ev->error == DART_E_DUPLICATE_AUTHORITY)
        ++*(int*)ev->user;
}
static void dup_authority_checks(void){
    DartAllocator aa = dart_allocator_dynamic(i_dart_plat_realloc, 0);
    DartAllocator ba = dart_allocator_dynamic(i_dart_plat_realloc, 0);
    DartNodeOpts ao, bo; DartNode *A, *B; DartDiscoveryAddr seed;
    int a_dups = 0, b_dups = 0, t;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&ao,0,sizeof ao); ao.domain=ST_DOMAIN+21; ao.discovery.max_peers=4;
    ao.net.multicast_interface="127.0.0.1"; ao.net.seed_peers=&seed; ao.net.n_seed_peers=1;
    bo=ao;
    ao.user_data=&a_dups; bo.user_data=&b_dups;
    A = dart_node_open(&aa, "dup-a", NULL, dup_on_event, &ao);
    B = dart_node_open(&ba, "dup-b", NULL, dup_on_event, &bo);
    ST_CHECK(A && B, "dup: nodes open");
    if (!(A && B)){ if(A)dart_node_close(A,0); if(B)dart_node_close(B,0); return; }

    { DartVariable *va = dart_node_create_variable_definition(A, "dupvar", NULL, NULL);
      DartFunction *fa = dart_node_create_function_definition(A, "dupfn", NULL, NULL, pf_empty_handler, NULL, NULL);
      ST_CHECK(va && fa, "dup: first authorities created");
      /* wait until B holds A's interest: the create-time sweep's precondition */
      { uint32_t want = (uint32_t)dart_topic_id("dupvar"); int seen = 0;
        for (t=0;t<2000 && !seen;t++){
            const DartDiscoveryPeer *ps; uint16_t pc;
            pf_pump(A,B,2);
            ps = dart_node_peers(B,&pc);
            if (ps && pc){ DartInterestIter it; DartTopicEntry e; memset(&it,0,sizeof it);
                while (dart_node_peer_interest_next(&ps[0], &it, &e))
                    if (e.hash==want){ seen=1; break; } }
        }
        ST_CHECK(seen, "dup: B holds A's interest"); }
      ST_CHECK(a_dups==0 && b_dups==0, "dup: quiet before the rival (a=%d b=%d)", a_dups, b_dups);

      { DartVariable *vb = dart_node_create_variable_definition(B, "dupvar", NULL, NULL);
        ST_CHECK(vb!=NULL, "dup: rival owner creates (diagnostic, not a refusal)");
        ST_CHECK(b_dups==1, "dup: rival owner detected at create (%d)", b_dups); }
      { DartFunction *fb = dart_node_create_function_definition(B, "dupfn", NULL, NULL, pf_empty_handler, NULL, NULL);
        ST_CHECK(fb!=NULL, "dup: rival provider creates");
        ST_CHECK(b_dups==2, "dup: rival provider detected at create (%d)", b_dups); }
      for (t=0;t<2000 && a_dups<2;t++) pf_pump(A,B,2);
      ST_CHECK(a_dups==2, "dup: first authority sees the rival's announce (%d)", a_dups);

      /* dedup + negative: repeated announces re-report nothing, and an accessor pairing
         with an owner is the LEGAL shape (sub side, no authority claim), never flagged */
      { DartVariable *acc  = dart_node_create_remote_variable(B, "solo", NULL, NULL);
        DartVariable *solo = dart_node_create_variable_definition(A, "solo", NULL, NULL);
        ST_CHECK(acc && solo, "dup: solo owner + accessor create");
        for (t=0;t<150;t++) pf_pump(A,B,2);
        ST_CHECK(a_dups==2 && b_dups==2,
                 "dup: once per (entity, peer); accessor never fires (a=%d b=%d)", a_dups, b_dups); } }

    dart_node_close(A,0); dart_node_close(B,0);
    dart_allocator_reset(&aa); dart_allocator_reset(&ba);
}

/* ===================== match-wait checks (19f) ===========================
 * The send-path match wait (runtime.h "MATCH WAIT"): a first send racing the announce/
 * detail cycle must reach an already-present subscriber; a disabled wait must drop
 * LOUDLY (DART_E_UNMATCHED_SEND); a topic nobody consumes must not stall; and delivery
 * must be WRITER-AUTHORITATIVE: a sample committed after our side matched but before the
 * peer verified us heals through reliable repair once its verdict lands (the property
 * the whole design leans on). */
static volatile unsigned long mw_recv, mw_lost, mw_unmatched;
static char mw_last[64];
static void mw_on_message(const DartMsg *m){
    size_t c = m->data.len < sizeof mw_last - 1 ? m->data.len : sizeof mw_last - 1;
    memcpy(mw_last, m->data.data, c); mw_last[c] = 0;
    mw_recv++;
}
static void mw_on_event(const DartEvent *ev){
    if (ev->kind == DART_MSG_LOST) mw_lost++;
    if (ev->kind == DART_ERROR && ev->error == DART_E_UNMATCHED_SEND) mw_unmatched++;
}

static void matchwait_checks(void){
    DartDiscoveryAddr seed; int t;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;

#ifdef _WIN32
    /* (a) WRITER-AUTHORITATIVE HEAL: B (sub, fixed port) never receives DETAIL_RESPs, so
       it cannot verify A's pub entry, while A verifies B normally and matches. A's send
       commits onto the formed lane; B drops the DATA (unverified index cannot demux).
       Unblocking lets B's re-ask (on A's next announce) verify A, and the reliable
       HB/NACK path must then deliver the ORIGINAL sample: our-side verdicts are
       sufficient for delivery, reader-side lateness heals. */
    { enum { MW_PORT = 47653 };
      DartAllocator aa = dart_allocator_dynamic(i_dart_plat_realloc, 0);
      DartAllocator ba = dart_allocator_dynamic(i_dart_plat_realloc, 0);
      DartNodeOpts ao, bo; DartNode *A, *B; DartTopic *at=NULL, *bt=NULL;
      memset(&bo,0,sizeof bo); bo.domain=ST_DOMAIN+24; bo.disable_shm=1; bo.discovery.max_peers=4;
      bo.discovery.announce_interval_us=200000;
      bo.net.multicast_interface="127.0.0.1"; bo.net.seed_peers=&seed; bo.net.n_seed_peers=1;
      ao=bo; bo.net.data_port=MW_PORT;
      mw_recv=0; mw_lost=0; mw_last[0]=0;
      B = dart_node_open(&ba, "mw-b", mw_on_message, mw_on_event, &bo);
      ST_CHECK(B!=NULL, "matchwait: sub node opens (is port %d free?)", (int)MW_PORT);
      if (B) bt = dart_node_create_topic(B, "mw-auth", DART_SUB_ONLY, NULL,
                      &(DartTopicOpts){ .qos={ .reliability=DART_RELIABLE } });
      g_tx_block_detail_resp_port = MW_PORT;   /* B stays PENDING on every candidate */
      A = dart_node_open(&aa, "mw-a", NULL, NULL, &ao);
      at = A ? dart_node_create_topic(A, "mw-auth", DART_PUB_ONLY, NULL,
                      &(DartTopicOpts){ .qos={ .reliability=DART_RELIABLE, .keep_last=8,
                                               .heartbeat_us=50000 } }) : NULL;
      ST_CHECK(A && at && bt, "matchwait: nodes + topics up");
      if (A && B && at && bt){
          int r;
          for (t=0;t<3000 && dart_topic_match_count(at)==0;t++){ dart_node_poll(A,2); dart_node_poll(B,2); }
          ST_CHECK(dart_topic_match_count(at)==1,
                   "matchwait: writer side matched while B's responses are blocked (%d)",
                   dart_topic_match_count(at));
          ST_CHECK(dart_topic_pending_count(bt) > 0,
                   "matchwait: reader side still PENDING (%d)", dart_topic_pending_count(bt));
          r = dart_topic_send(at, dart_bytes("authoritative", 13));
          ST_CHECK(r==DART_OK, "matchwait: matched send commits (%d)", r);
          for (t=0;t<150;t++){ dart_node_poll(A,2); dart_node_poll(B,2); }
          ST_CHECK(mw_recv==0, "matchwait: unverified reader drops the DATA (recv=%lu)", mw_recv);
          g_tx_block_detail_resp_port = 0;      /* B's next re-ask verifies A */
          for (t=0;t<3000 && mw_recv==0;t++){ dart_node_poll(A,2); dart_node_poll(B,2); }
          ST_CHECK(mw_recv==1 && strcmp(mw_last,"authoritative")==0,
                   "matchwait: repair delivers the pre-verify sample (recv=%lu '%s')", mw_recv, mw_last);
          ST_CHECK(mw_lost==0, "matchwait: healed with no MSG_LOST (%lu)", mw_lost);
          ST_CHECK(dart_topic_pending_count(at)==0 && dart_topic_ready(at)==1,
                   "matchwait: probes converge (pending=%d ready=%d)",
                   dart_topic_pending_count(at), dart_topic_ready(at));
      }
      g_tx_block_detail_resp_port = 0;
      if (A) dart_node_close(A,1);
      if (B) dart_node_close(B,1);
      dart_allocator_reset(&aa); dart_allocator_reset(&ba);
    }
#endif

#ifdef DART_THREADS
    /* (b) FIRST SEND vs the forming match: a reliable catch_up=0 publish (signal-shaped:
       retention exempt, so the wait applies) fired immediately after create_topic must
       WAIT for the already-present subscriber's match, commit, and deliver. The
       subscriber runs its service thread so it can answer announces and detail requests
       while the sender's blocked send pumps only its own loop (in reality peers are
       independent processes). */
    { DartAllocator aa = dart_allocator_dynamic(i_dart_plat_realloc, 0);
      DartAllocator ba = dart_allocator_dynamic(i_dart_plat_realloc, 0);
      DartNodeOpts o; DartNode *A, *B; DartTopic *at=NULL, *bt=NULL;
      memset(&o,0,sizeof o); o.domain=ST_DOMAIN+25; o.disable_shm=1; o.discovery.max_peers=4;
      o.net.multicast_interface="127.0.0.1"; o.net.seed_peers=&seed; o.net.n_seed_peers=1;
      mw_recv=0; mw_lost=0; mw_unmatched=0; mw_last[0]=0;
      B = dart_node_open(&ba, "mw-sub", mw_on_message, mw_on_event, &o);
      bt = B ? dart_node_create_topic(B, "mw-first", DART_SUB_ONLY, NULL,
                   &(DartTopicOpts){ .qos={ .reliability=DART_RELIABLE } }) : NULL;
      ST_CHECK(B && bt, "matchwait: first-send sub up");
      if (B) dart_node_start(B);
      A = dart_node_open(&aa, "mw-pub", NULL, mw_on_event, &o);
      at = A ? dart_node_create_topic(A, "mw-first", DART_PUB_ONLY, NULL,
                   &(DartTopicOpts){ .qos={ .reliability=DART_RELIABLE } }) : NULL;
      ST_CHECK(A && at, "matchwait: first-send pub up");
      if (A && B && at && bt){
          int r = dart_topic_send(at, dart_bytes("first", 5));   /* immediately: the race */
          ST_CHECK(r==DART_OK, "matchwait: racing first send commits (%d)", r);
          ST_CHECK(dart_topic_match_count(at) > 0,
                   "matchwait: ...after waiting out the match (%d)", dart_topic_match_count(at));
          for (t=0;t<1500 && mw_recv==0;t++) dart_node_poll(A,2);
          ST_CHECK(mw_recv==1 && strcmp(mw_last,"first")==0,
                   "matchwait: subscriber got the racing first send (recv=%lu '%s')", mw_recv, mw_last);
          ST_CHECK(mw_unmatched==0, "matchwait: no timeout diagnostic (%lu)", mw_unmatched);
          /* a topic NOBODY consumes must not stall once the gather has settled: the
             converged state is memoized, so this send early-outs as it always did */
          for (t=0;t<300;t++) dart_node_poll(A,2);   /* let the post-open gather settle */
          { DartTopic *v = dart_node_create_topic(A, "mw-void", DART_PUB_ONLY, NULL, NULL);
            uint64_t t0, el;
            ST_CHECK(v!=NULL, "matchwait: void topic up");
            t0 = i_dart_plat_now_us();
            r = v ? dart_topic_send(v, dart_bytes("x",1)) : -1;
            el = i_dart_plat_now_us() - t0;
            ST_CHECK(r==DART_OK && el < 250000u,
                     "matchwait: no-consumer send returns fast (%d, %luus)", r, (unsigned long)el); }
      }
      if (B) dart_node_stop(B);
      if (A) dart_node_close(A,1);
      if (B) dart_node_close(B,1);
      dart_allocator_reset(&aa); dart_allocator_reset(&ba);
    }
#endif

    /* (c) CONTROL, wait DISABLED (match_wait_ms < 0): the same racing send commits
       immediately to zero subscribers and is gone for good (catch_up 0: nothing
       replays when the match forms a beat later), but LOUDLY: DART_E_UNMATCHED_SEND
       fires on the way out. */
    { DartAllocator aa = dart_allocator_dynamic(i_dart_plat_realloc, 0);
      DartAllocator ba = dart_allocator_dynamic(i_dart_plat_realloc, 0);
      DartNodeOpts o, ao; DartNode *A, *B; DartTopic *at=NULL, *bt=NULL;
      memset(&o,0,sizeof o); o.domain=ST_DOMAIN+26; o.disable_shm=1; o.discovery.max_peers=4;
      o.net.multicast_interface="127.0.0.1"; o.net.seed_peers=&seed; o.net.n_seed_peers=1;
      ao=o; ao.match_wait_ms=-1;
      mw_recv=0; mw_unmatched=0;
      B = dart_node_open(&ba, "mw-sub2", mw_on_message, NULL, &o);
      bt = B ? dart_node_create_topic(B, "mw-off", DART_SUB_ONLY, NULL,
                   &(DartTopicOpts){ .qos={ .reliability=DART_RELIABLE } }) : NULL;
      A = dart_node_open(&aa, "mw-off-pub", NULL, mw_on_event, &ao);
      at = A ? dart_node_create_topic(A, "mw-off", DART_PUB_ONLY, NULL,
                   &(DartTopicOpts){ .qos={ .reliability=DART_RELIABLE } }) : NULL;
      ST_CHECK(A && B && at && bt, "matchwait: knob-off pair up");
      if (A && B && at && bt){
          int r = dart_topic_send(at, dart_bytes("gone", 4));   /* no wait: commits to zero */
          ST_CHECK(r==DART_OK && mw_unmatched==1,
                   "matchwait: disabled wait drops LOUDLY (r=%d events=%lu)", r, mw_unmatched);
          for (t=0;t<2000 && dart_topic_match_count(at)==0;t++){ dart_node_poll(A,2); dart_node_poll(B,2); }
          ST_CHECK(dart_topic_match_count(at)==1, "matchwait: the match still forms after (%d)",
                   dart_topic_match_count(at));
          for (t=0;t<150;t++){ dart_node_poll(A,2); dart_node_poll(B,2); }
          ST_CHECK(mw_recv==0, "matchwait: the dropped send never resurrects (recv=%lu)", mw_recv);
      }
      if (A) dart_node_close(A,1);
      if (B) dart_node_close(B,1);
      dart_allocator_reset(&aa); dart_allocator_reset(&ba);
    }
}

static int selftest_main(void){
    static uint8_t mem_w[1<<20], mem_r[1<<20];
    uint8_t payload[32]; unsigned i;
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: keep output on a crash */
    memset(payload, 0x5A, sizeof payload);

    DartTopicDef ch[4]; memset(ch, 0, sizeof ch);
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
    { DartNodeOpts ro = wo; DartTopicDef chr[4]; DartNode *w, *r;
      memcpy(chr, ch, sizeof ch);
      ch[0].role = ch[1].role = ch[2].role = ch[3].role = DART_PUB_ONLY;
      chr[0].role = chr[1].role = chr[3].role = DART_SUB_ONLY;
      chr[2].role = DART_INACTIVE;

      w = test_node_open(mem_w, sizeof mem_w, NULL, NULL, NULL, wo, ch, 4);
      r = test_node_open(mem_r, sizeof mem_r, NULL, st_on_message, st_on_event, ro, chr, 4);
      if (!w || !r){ fprintf(stderr, "node open failed\n"); return 1; }

      /* 1. JOIN: writer streams while discovery completes; the reader must
            adopt the stream head silently (no gap for a late joiner) */
      { uint64_t end = i_dart_plat_now_us() + 5000000u;
        while (st_samples[ST_CH_GAP]==0 && i_dart_plat_now_us() < end){
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
      { uint64_t t0 = i_dart_plat_now_us(), dt;
        dart_node_send(w, ST_CH_BLOCK, payload, sizeof payload);   /* evicts un-acked */
        dt = i_dart_plat_now_us() - t0;
        ST_CHECK(dt >= ST_BLOCK_US-10000 && dt < 4*ST_BLOCK_US,
                 "blocked: send waited ~backpressure_wait_us (%.1f ms)", dt/1000.0);
      }

      /* 4. RELEASED: let the reader catch up and ack; sends are instant */
      st_pump(w, r, 300);
      { uint64_t t0 = i_dart_plat_now_us(), dt;
        dart_node_send(w, ST_CH_BLOCK, payload, sizeof payload);
        dt = i_dart_plat_now_us() - t0;
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
            A sub-only reader's data topic never advances next_seqno, so a
            sweep that skips next_seqno==0 topics starves that ack and every
            later send waits the full backpressure_wait_us. Fill the ring, go quiet long
            enough for the sweep, then a send that would evict must NOT block. */
      st_pump(w, r, 200);                              /* match + settle */
      { unsigned long s0 = st_samples[ST_CH_BLOCK2];
        for (i=0;i<ST_DEPTH;i++) dart_node_send(w, ST_CH_BLOCK2, payload, sizeof payload);
        st_pump(w, r, 200);                            /* reader drains burst; sweep must ack */
        ST_CHECK(st_samples[ST_CH_BLOCK2]-s0 == ST_DEPTH, "sweep-ack: ring delivered (%lu, want %u)",
                 st_samples[ST_CH_BLOCK2]-s0, ST_DEPTH);
        { uint64_t t0 = i_dart_plat_now_us(), dt;
          dart_node_send(w, ST_CH_BLOCK2, payload, sizeof payload);   /* would evict slot 0 */
          dt = i_dart_plat_now_us() - t0;
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
        for (k=0;k<i_dart_node_core_max_peers(r->core);k++) if (i_dart_node_core_peer_at(r->core,k,&wid,NULL,NULL,NULL)) break;
        dart_transport_peer_remove(r->transport, wid);
        dart_transport_peer_add(r->transport, wid,DART_FRAG_SIZE);
        il = dart_transport_build_interest(w->transport, ib, sizeof ib);
        dart_transport_apply_peer_interest(r->transport, wid, dart_bytes(ib, il));
        /* v10: the apply only NOMINATES (peer_remove dropped the cached verdicts with
           the rest of the peer state); run the sans-IO detail exchange by hand, exactly
           as a runtime would, so the flapped reader re-verifies and rematches */
        {   DartDetailWant wl[8]; uint8_t rq[256], rp[1024]; uint16_t nw2, verified; size_t rl2, pl2;
            nw2 = dart_transport_detail_wants(r->transport, NULL, wid, dart_bytes(ib, il), wl, 8);
            ST_CHECK(nw2 > 0, "flap: re-added peer nominates pending candidates (%u)", nw2);
            rl2 = dart_detail_req_build(ST_DOMAIN, 0, wl, nw2, rq, sizeof rq);
            pl2 = dart_transport_detail_respond(w->transport, NULL, 0, dart_bytes(rq, rl2), rp, sizeof rp);
            verified = dart_transport_apply_peer_details(r->transport, wid, dart_bytes(rp, pl2));
            ST_CHECK(verified == nw2, "flap: details verify every candidate (%u/%u)", verified, nw2);
            dart_transport_apply_peer_interest(r->transport, wid, dart_bytes(ib, il));
        }
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
            dart_transport_peer_dormant/dart_transport_peer_resume expose (the node drives them off
            discovery DROP/return). */
      st_pump(w, r, 200);
      { unsigned long s0 = st_samples[ST_CH_DYN], g0 = st_gap_calls[ST_CH_DYN];
        uint32_t wid = 0, rid = 0; uint16_t k;
        for (k=0;k<i_dart_node_core_max_peers(r->core);k++) if (i_dart_node_core_peer_at(r->core,k,&wid,NULL,NULL,NULL)) break;
        for (k=0;k<i_dart_node_core_max_peers(w->core);k++) if (i_dart_node_core_peer_at(w->core,k,&rid,NULL,NULL,NULL)) break;
        dart_transport_peer_dormant(w->transport, rid);   /* writer drops the reader from flow control */
        dart_transport_peer_dormant(r->transport, wid);   /* reader stops acking the writer */
        for (i=0;i<3;i++) dart_node_send(w, ST_CH_DYN, payload, sizeof payload);
        st_pump(w, r, 300);
        ST_CHECK(st_samples[ST_CH_DYN] == s0, "resume: dormant peer withholds sends (%lu, want %lu)",
                 st_samples[ST_CH_DYN], s0);
        dart_transport_peer_resume(w->transport, rid);
        dart_transport_peer_resume(r->transport, wid);
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

    /* 6. SCALE: 40 topics; the full interest list rides one discovery announce
          blob (IP-fragmented if large), matched at peer_up */
    { static uint8_t mem_a[1<<20], mem_b[1<<20];
      static DartTopicDef cha[ST_NCH], chb[ST_NCH];
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
      ST_CHECK(a && b, "scale: %u-topic nodes open", ST_NCH);
      if (a && b){
          st_any = 0;
          for (k=0;k<ST_NCH;k++) dart_node_send(a, k, payload, 16);
          { uint64_t end = i_dart_plat_now_us() + 5000000u;
            while (st_any < ST_NCH && i_dart_plat_now_us() < end) st_pump(a, b, 20); }
          ST_CHECK(st_any == ST_NCH, "scale: all topics delivered (%lu/%u)", st_any, ST_NCH);
          dart_node_close(b, 1);
          dart_node_close(a, 1);
      }
    }

    /* 7. NAMED: the cross-peer identity is the topic NAME (its 64-bit hash),
          independent of each node's local topic handle. A matching name matches
          across differing handles; a distinct name never cross-wires; and clean
          names raise no false collision. */
    { static uint8_t mem_nw[1<<20], mem_nr[1<<20];
      DartTopicDef nw[1], nr[2];
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
          uint64_t end = i_dart_plat_now_us() + 5000000u;
          while (st_samples[0]==0 && i_dart_plat_now_us()<end){
              dart_node_send(w2, 0, payload, 16); st_pump(w2,r2,20);
          }
          ST_CHECK(st_samples[0] > 0, "named: same name matches across nodes (%lu)", st_samples[0]);
          /* the node name is synced via discovery and surfaces as DartMsg.publisher_name */
          ST_CHECK(strcmp(st_last_sender, "lidar-node")==0,
                   "named: publisher_name carries the publisher's node name (%s)", st_last_sender);
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
      DartTopicDef cw, cr; DartNodeOpts wo3, ro3; DartNode *w3, *r3;
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
          ST_CHECK(st_collisions >= 1, "collision: detected (DART_E_NAME_COLLISION fired %lu)", st_collisions);
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
    schema_dsl_checks();          /* 17c. schema DSL: text == builder wire, layout, rejects */
    schema_advert_checks();       /* 18. topic schema rides the announce; peer reads it back */
    schema_bind_checks();         /* 19. subset reader binds to the writer's layout; conflicts refused */
    detail_codec_checks();        /* 19b. pairwise detail codec: responder, wire inlining, paging */
    detail_live_checks();         /* 19c. 'uDTL' on the data socket: stateless reply to source */
    queue_checks();               /* 19d. consumer queues: take/dispatch, BE overwrite, reliable park */
    patterns_checks();            /* 19e. patterns layer: functions (req/resp, defer, timeout, sync) */
    dup_authority_checks();       /* 19e2. duplicate provider/owner diagnostic (both rivals, deduped) */
    matchwait_checks();           /* 19f. send-path match wait + writer-authoritative repair */
#ifdef DART_THREADS
    threaded_checks();            /* 20-24. service thread, condvar flow control, unsent guard, waker */
#endif

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
#define CTL_CMD        0     /* topic handles are array indices */
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
 * control-plane discovery and result topic stay live meanwhile; kill
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
 * command (reliable CMD topic, catch_up 0 so stale commands never replay
 * to late workers) and collects results (reliable RES topic, catch_up =
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
    uint16_t ch = msg->topic_index; const void *d = msg->data.data; size_t len = msg->data.len;
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
    static DartTopicDef ch[2];
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
    for (i=0;i<i_dart_node_core_max_peers(ctl->core);i++){
        uint8_t pip[16], pil;
        if (i_dart_node_core_peer_at(ctl->core, i, NULL, pip, &pil, NULL) && pil==4){
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

/* last SUMMARY/SUMMARY2 lines of a child's output, wrapped for the RES topic */
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
    DartTopic *pc=NULL; DartTopicOpts co; DartDiscoveryAddr seed;
    uint8_t *payload; int i, j, k, t, nwarm, nsteady; unsigned dom = ms_domain++;
    uint64_t a0=0, a1=0, a2=0; size_t ppeak=0, speak=0;
    double cold_sum=0, warm_sum=0, t0, t1, msg_s; int coldc=0;
    if (nsubs>16) nsubs=16;
    payload=(uint8_t*)malloc(plen?plen:1); if(!payload) return; memset(payload,0x5A,plen?plen:1);
    nsteady = plen<=1024 ? 300 : plen<=65536 ? 100 : 20;
    nwarm   = keep + 4;
    memset(&co,0,sizeof co); co.qos.reliability=DART_RELIABLE; co.qos.keep_last=keep; co.qos.heartbeat_us=50000;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4;
    memset(&po,0,sizeof po); po.domain=(uint16_t)dom; po.max_topics=4;
    po.discovery.max_peers=(uint16_t)(nsubs+2); po.disable_shm=(uint8_t)disable_shm;
    po.net.multicast_interface="127.0.0.1"; po.net.seed_peers=&seed; po.net.n_seed_peers=1;
    so=po;
    pa=dart_allocator_dynamic(i_dart_plat_realloc, 0);
    P=dart_node_open(&pa,"ms-pub",NULL,NULL,&po);
    for (i=0;i<nsubs;i++){ sa[i]=dart_allocator_dynamic(i_dart_plat_realloc, 0); S[i]=dart_node_open(&sa[i],"ms-sub",ms_on_message,NULL,&so); }
    if (!P){ free(payload); return; }
    pc=dart_node_create_topic(P,"ms/ch",DART_PUB_ONLY,NULL,&co);
    for (i=0;i<nsubs;i++) dart_node_create_topic(S[i],"ms/ch",DART_SUB_ONLY,NULL,&co);
    for (t=0;t<4000 && dart_topic_match_count(pc)<nsubs;t++){ dart_node_poll(P,1); for(j=0;j<nsubs;j++) dart_node_poll(S[j],1); }

    a0=ms_allocs(P,S,nsubs);                              /* total allocs before any traffic */
    g_ms_rx=0;                                            /* warmup: fill + size the buffers */
    for (i=0;i<nwarm;i++){
        double s0=(double)i_dart_plat_now_us(); dart_topic_send(pc,dart_bytes(payload,plen)); double s1=(double)i_dart_plat_now_us();
        if (i<keep){ cold_sum += s1-s0; coldc++; }
        for (k=0;k<400000 && g_ms_rx < (i+1)*nsubs;k++){ dart_node_poll(P,0); for(j=0;j<nsubs;j++) dart_node_poll(S[j],0); }
    }
    a1=ms_allocs(P,S,nsubs);                              /* allocs after warmup */
    g_ms_rx=0; t0=(double)i_dart_plat_now_us();             /* steady: buffers sized -> must not alloc */
    for (i=0;i<nsteady;i++){
        double s0=(double)i_dart_plat_now_us(); dart_topic_send(pc,dart_bytes(payload,plen)); double s1=(double)i_dart_plat_now_us();
        warm_sum += s1-s0;
        for (k=0;k<400000 && g_ms_rx < (i+1)*nsubs;k++){ dart_node_poll(P,0); for(j=0;j<nsubs;j++) dart_node_poll(S[j],0); }
    }
    t1=(double)i_dart_plat_now_us();
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

/* ============ threadbench: threaded send-path cost + drain backpressure ===== *
 * One started pub node, one started sub node, one sender thread (this one). For
 * each target rate (0 = flat out) send paced 32B best-effort messages for 2s and
 * report: achieved rate, how many sends slept in the drain/backpressure wait, the
 * time those waits ate (absolute + % of wall), unsent evictions, and delivery.
 * keep_last 64 so the drain guard (not ack flow control) is what engages. Runs
 * the UDP path (and the SHM path when compiled in), then a naive single-threaded
 * send+poll(0) baseline for comparison. Backpressure only ever engages flat-out:
 * paced rates should show wait% ~0 and delivery 100. */
#ifdef DART_THREADS

static volatile unsigned long g_tb_recv;
static void tb_on_message(const DartMsg *m){ (void)m; g_tb_recv++; }

static DartNode *tb_open(const char *name, int sub, int disable_shm){
    DartAllocator a = dart_allocator_dynamic(i_dart_plat_realloc, 0);
    DartNodeOpts o;
    memset(&o, 0, sizeof o);
    o.domain = 51;
    o.disable_shm = (uint8_t)disable_shm;
    o.net.multicast_interface = "127.0.0.1";
    return dart_node_open(&a, name, sub ? tb_on_message : NULL, NULL, &o);
}

static DartTopic *tb_channel(DartNode *n, int sub){
    DartTopicOpts co;
    memset(&co, 0, sizeof co);
    co.qos.keep_last = 64;               /* best-effort: isolates the unsent drain guard */
    co.qos.max_message_bytes = 64;
    co.qos.heartbeat_us = 50000;
    return dart_node_create_topic(n, "tb/bench", sub ? DART_SUB_ONLY : DART_PUB_ONLY, NULL, &co);
}

static void tb_run_rate(DartNode *w, DartTopic *ch, unsigned rate_hz, double dur_s){
    uint8_t payload[32];
    uint64_t t0, t_end, next = 0, wait_us0, wait_us1;
    uint32_t wait_n0, wait_n1, ev0, ev1;
    unsigned long sent = 0, recv0 = g_tb_recv;
    double wall;
    char label[16];

    memset(payload, 0x42, sizeof payload);
    dart_node_backpressure_stats(w, &wait_us0, &wait_n0);
    ev0 = dart_node_evicted_unsent(w);

    t0 = i_dart_plat_now_us();
    t_end = t0 + (uint64_t)(dur_s * 1e6);
    if (rate_hz) next = t0;
    while (i_dart_plat_now_us() < t_end){
        if (rate_hz){
            uint64_t now = i_dart_plat_now_us();
            if (now < next) continue;                /* busy-wait pacing */
            next += 1000000u / rate_hz;
            if (next < now) next = now;              /* fell behind: no burst catch-up */
        }
        dart_topic_send(ch, dart_bytes(payload, sizeof payload));
        sent++;
    }
    wall = (i_dart_plat_now_us() - t0) / 1e6;

    sw_sleep_ms(300);                                /* let deliveries settle */
    dart_node_backpressure_stats(w, &wait_us1, &wait_n1);
    ev1 = dart_node_evicted_unsent(w);

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
    DartNode *w = tb_open("tb-pub", 0, disable_shm);
    DartNode *r = tb_open("tb-sub", 1, disable_shm);
    DartTopic *cw, *cr;
    unsigned k;
    if (!w || !r){ fprintf(stderr, "threadbench: open failed\n"); return 1; }
    cw = tb_channel(w, 0); cr = tb_channel(r, 1); (void)cr;
    dart_node_start(w); dart_node_start(r);
    { uint64_t end = i_dart_plat_now_us() + 5000000u;
      while (dart_topic_match_count(cw) == 0 && i_dart_plat_now_us() < end) sw_sleep_ms(2); }
    if (dart_topic_match_count(cw) != 1){ fprintf(stderr, "threadbench: no match\n"); return 1; }

    printf("\n== %s ==\n", title);
    printf("%9s  %10s  %9s  %10s  %8s  %7s  %9s\n",
           "target", "sent/s", "waited", "wait ms", "wait%", "evicted", "delivered");
    for (k = 0; k < sizeof rates / sizeof rates[0]; k++)
        tb_run_rate(w, cw, rates[k], 2.0);
    dart_node_close(r, 1);
    dart_node_close(w, 1);
    return 0;
}

static int threadbench_main(void){
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("DART threaded send-path bench (32B best-effort, keep_last 64, loopback, one sender)\n"
           "waited = sends that slept in the drain/backpressure wait; wait%% = of sender wall time\n");
    if (tb_mode("threaded, UDP loopback (shm off)", 1)) return 1;
#ifdef DART_SHM
    if (tb_mode("threaded, SHM same-host path", 0)) return 1;
#endif

    /* single-threaded flat-out baseline: the naive send + poll(0) + poll(0) loop */
    {
        DartNode *w = tb_open("tb-base-pub", 0, 1);
        DartNode *r = tb_open("tb-base-sub", 1, 1);
        DartTopic *cw = tb_channel(w, 0), *cr = tb_channel(r, 1);
        uint8_t payload[32]; unsigned long sent = 0, recv0;
        uint64_t t0, t_end; double wall;
        (void)cr;
        if (!w || !r || !cw){ fprintf(stderr, "threadbench: baseline open failed\n"); return 1; }
        memset(payload, 0x42, sizeof payload);
        { uint64_t end = i_dart_plat_now_us() + 5000000u;
          while (dart_topic_match_count(cw) == 0 && i_dart_plat_now_us() < end){
              dart_node_poll(w, 1); dart_node_poll(r, 0);
          } }
        recv0 = g_tb_recv;
        t0 = i_dart_plat_now_us(); t_end = t0 + 2000000u;
        while (i_dart_plat_now_us() < t_end){
            dart_topic_send(cw, dart_bytes(payload, sizeof payload));
            dart_node_poll(w, 0);
            dart_node_poll(r, 0);
            sent++;
        }
        wall = (i_dart_plat_now_us() - t0) / 1e6;
        { uint64_t settle = i_dart_plat_now_us() + 300000u;
          while (i_dart_plat_now_us() < settle){ dart_node_poll(w, 0); dart_node_poll(r, 0); } }
        printf("\n== single-threaded baseline: send+poll(0)+poll(0) flat-out, UDP ==\n");
        printf("  %10.0f send/s   delivered %.2f%%\n",
               sent / wall, sent ? 100.0 * (double)(g_tb_recv - recv0) / sent : 0.0);
        dart_node_close(r, 1);
        dart_node_close(w, 1);
    }
    return 0;
}

/* ============ queuebench: consumer-queue cost vs inline callbacks ============ *
 * One started pub node + one started sub node on loopback, per payload size and
 * mode. Flat-out RELIABLE stream (keep_last 64, backpressure-paced: lossless, so
 * the number IS the sustainable end-to-end goodput), 2s per run:
 *   inline : the pre-queue model; the sub's on_message counts deliveries on its
 *            SERVICE thread (zero-copy view, no ring).
 *   queued : the sub topic owns a 4 MB consumer queue; THIS thread drains it
 *            with dart_topic_take (adds the one ring memcpy per message).
 * The shm_rx column verifies which path carried the payload (SHM section: it must
 * track delivered; UDP section: 0). */
static volatile unsigned long g_qb_recv;
static void qb_on_message(const DartMsg *m){ (void)m; g_qb_recv++; }

static volatile int g_qb_stop;
static uint32_t     g_qb_len;
static void qb_sender(void *arg){
    static uint8_t payload[1u << 20];
    DartTopic *ch = (DartTopic *)arg;
    memset(payload, 0x42, g_qb_len);
    while (!g_qb_stop)
        dart_topic_send(ch, dart_bytes(payload, g_qb_len));
}

static void qb_run(uint32_t size, int queued, int disable_shm){
    DartAllocator aw = dart_allocator_dynamic(i_dart_plat_realloc, 0);
    DartAllocator ar = dart_allocator_dynamic(i_dart_plat_realloc, 0);
    DartNodeOpts o; DartTopicOpts co;
    DartNode *w, *r; DartTopic *cw, *cr;
    i_DartThread th;
    uint64_t t0, t_end;
    unsigned long recv = 0;
    double wall, rate;
    uint32_t shm_rx0 = 0, shm_rx1 = 0;

    memset(&o, 0, sizeof o);
    o.domain = 52; o.disable_shm = (uint8_t)disable_shm;
    o.net.multicast_interface = "127.0.0.1";
    w = dart_node_open(&aw, "qb-pub", NULL, NULL, &o);
    r = dart_node_open(&ar, "qb-sub", queued ? NULL : qb_on_message, NULL, &o);
    if (!w || !r){ fprintf(stderr, "queuebench: open failed\n"); exit(1); }
    memset(&co, 0, sizeof co);
    co.qos.reliability = DART_RELIABLE;
    co.qos.keep_last = (uint16_t)(size >= 262144u ? 8 : 64);   /* realistic: shallow history for huge messages */
    co.qos.backpressure_wait_us = 200000;   /* lossless: the sender paces to the consumer */
    co.qos.heartbeat_us = 20000;
    co.qos.repair_delay_us = 5000;
    co.qos.shm_max_bytes = size;            /* pin one SHM size class per run */
    cw = dart_node_create_topic(w, "qb/t", DART_PUB_ONLY, NULL, &co);
    /* the queue must cover the writer's in-flight burst (keep_last x size), else the
       reader PARKS while the writer keeps bursting and the overrun heals through the
       paced repair path -- a sizing bug, not steady-state cost. 2x burst, min 4 MB. */
    {   uint32_t qb = 2u * co.qos.keep_last * size;
        if (qb < (4u << 20)) qb = 4u << 20;
        co.qos.queue_bytes = queued ? qb : 0;
    }
    cr = dart_node_create_topic(r, "qb/t", DART_SUB_ONLY, NULL, &co);
    if (!cw || !cr){ fprintf(stderr, "queuebench: topic create failed\n"); exit(1); }
    dart_node_start(w); dart_node_start(r);
    {   uint64_t end = i_dart_plat_now_us() + 5000000u;
        while (dart_topic_match_count(cw) == 0 && i_dart_plat_now_us() < end) sw_sleep_ms(2); }
    if (dart_topic_match_count(cw) != 1){ fprintf(stderr, "queuebench: no match\n"); exit(1); }

#ifdef DART_SHM
    dart_node_shm_stats(r, NULL, &shm_rx0);
#endif
    g_qb_len = size; g_qb_stop = 0; g_qb_recv = 0;
    if (!i_dart_plat_thread_start(&th, qb_sender, cw)){ fprintf(stderr, "queuebench: thread\n"); exit(1); }
    t0 = i_dart_plat_now_us(); t_end = t0 + 2000000u;
    if (queued){
        DartMsg m;
        while (i_dart_plat_now_us() < t_end)
            if (dart_topic_take(cr, &m, 5) == 1) recv++;
    } else {
        while (i_dart_plat_now_us() < t_end) sw_sleep_ms(5);
        recv = g_qb_recv;
    }
    wall = (double)(i_dart_plat_now_us() - t0) / 1e6;
    g_qb_stop = 1;
    i_dart_plat_thread_join(&th);
#ifdef DART_SHM
    dart_node_shm_stats(r, NULL, &shm_rx1);
#endif
    rate = wall > 0.0 ? recv / wall : 0.0;
    printf("%9u  %-7s  %12.0f  %10.1f  %11lu  %9u\n",
           size, queued ? "queued" : "inline", rate, rate * size / 1e6, recv, shm_rx1 - shm_rx0);
    dart_node_close(r, 1);
    dart_node_close(w, 1);
}

static int queuebench_main(void){
    static const uint32_t sizes[] = { 1024, 16384, 65536, 262144, 1048576 };
    unsigned k;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("DART consumer-queue throughput: inline callback (service thread) vs queued take\n"
           "(flat-out reliable, keep_last 64, backpressure-paced = lossless goodput, loopback, 2s/run)\n");
#ifdef DART_SHM
    printf("\n== SHM same-host path ==\n");
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
#endif /* DART_THREADS */

int main(int argc, char **argv){
    if (argc >= 2 && strcmp(argv[1], "memscale") == 0)
        return memscale_main();
    if (argc >= 2 && strcmp(argv[1], "queuebench") == 0){
#ifdef DART_THREADS
        return queuebench_main();
#else
        fprintf(stderr, "queuebench needs threads (DART_THREADS off)\n");
        return 1;
#endif
    }
    if (argc >= 2 && strcmp(argv[1], "threadbench") == 0){
#ifdef DART_THREADS
        return threadbench_main();
#else
        fprintf(stderr, "threadbench needs threads (DART_THREADS off: undetected platform or DART_NO_THREADS)\n");
        return 1;
#endif
    }
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
        "        reliable=1: load topic DART_RELIABLE; block_ms: writer\n"
        "        backpressure window (qos.backpressure_wait_us) for that topic\n"
        "        extra_ch: declare N more topics; spread=1 round-robins the\n"
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
        "  queuebench\n"
        "        consumer-queue throughput: inline callback vs queued take, per\n"
        "        payload size, on the SHM and UDP loopback paths\n"
        "  threadbench\n"
        "        threaded send-path bench: paced rates + flat-out through a started\n"
        "        node pair; reports the time eaten by the drain/backpressure wait\n"
        "  selftest\n"
        "        on_gap, backpressure, dynamic-interest functional test (exit 0 = pass)\n");
    return 2;
}
